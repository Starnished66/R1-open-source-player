/* POSIX port of Rockbox apps/recorder/albumart.c.
 *
 * Copyright (C) 2007 Nicolas Pennequin (original search order)
 * Copyright (C) Open HiBy Player contributors (POSIX host)
 *
 * Search paths and invalid-character folding follow Rockbox. Sized thumbs
 * are stored under .compas/albumart, next to tagcache. */

#include "albumart.h"
#include "library_endian.h"
#include "storage_paths.h"

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>
#include <stdint.h>
#include <errno.h>

#define ALBUMART_DIR SD_COMPAS_ROOT "/albumart"
#define ALBUMART_LEGACY_DIR SD_LEGACY_ROOT "/albumart"

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

static bool file_exists(const char * path) {
    return path && path[0] && access(path, F_OK) == 0;
}

static void strmemccpy_local(char * dst, const char * src, size_t n) {
    if (!dst || n == 0) return;
    snprintf(dst, n, "%s", src ? src : "");
}

/* Split directory (including trailing '/') into buf; return pointer to filename. */
static const char * strip_filename(char * buf, int buf_size, const char * fullpath) {
    if (!buf || buf_size <= 0 || !fullpath) return NULL;
    const char * sep = strrchr(fullpath, '/');
    if (!sep) {
        buf[0] = '\0';
        return fullpath;
    }
    int len = MIN((int) (sep - fullpath + 1), buf_size - 1);
    memcpy(buf, fullpath, (size_t) len);
    buf[len] = '\0';
    return sep + 1;
}

static void strip_extension(char * dst, size_t dst_size, const char * src) {
    strmemccpy_local(dst, src, dst_size);
    char * slash = strrchr(dst, '/');
    char * dot = strrchr(dst, '.');
    if (dot && (!slash || dot > slash)) *dot = '\0';
}

/* Rockbox fix_path_part: '"' -> '\'', and * / : < > ? \ | -> '_'. */
static void fix_path_part(char * path, int offset, int count) {
    static const char invalid_chars[] = "*/:<>?\\|";
    if (!path || offset < 0) return;
    char * p = path + offset;
    for (int i = 0; i <= count && *p; i++, p++) {
        if (*p == '"') *p = '\'';
        else if (strchr(invalid_chars, *p)) *p = '_';
    }
}

static const char * const extensions[] = { "jpeg", "jpg", "png", "bmp" };

/* Tagcache album groups compare ASCII bytes without case and use the
 * effective album artist as their second identity field. Keep the hash
 * ordering and separator aligned with that exact grouping identity. */
uint64_t albumart_thumbnail_key(const albumart_info_t * info) {
    if (!info) return UINT64_C(14695981039346656037);
    const char * album_artist = info->albumartist[0] ? info->albumartist : info->artist;
    const char * fields[] = { info->album, album_artist };
    uint64_t hash = UINT64_C(14695981039346656037);
    for (int field = 0; field < 2; field++) {
        for (const unsigned char * p = (const unsigned char *) fields[field]; *p; p++) {
            unsigned char c = *p;
            if (c >= 'A' && c <= 'Z') c = (unsigned char) (c + ('a' - 'A'));
            hash = (hash ^ c) * UINT64_C(1099511628211);
        }
        if (field == 0) hash = (hash ^ 0) * UINT64_C(1099511628211);
    }
    return hash;
}

/* Existing v2 artwork files used raw, case-sensitive album-artist then album
 * bytes. Keep looking for those names so an identity-key change does not
 * discard already generated thumbnail files. New shared-key files use v3. */
static uint64_t thumbnail_key_legacy(const albumart_info_t * info) {
    const char * fields[] = { info->albumartist[0] ? info->albumartist : info->artist, info->album };
    uint64_t hash = UINT64_C(14695981039346656037);
    for (int i = 0; i < 2; i++) {
        const unsigned char * p = (const unsigned char *) fields[i];
        do { hash = (hash ^ *p) * UINT64_C(1099511628211); } while (*p++);
    }
    return hash;
}

static bool under_directory(const char * path, const char * dir) {
    size_t len = strlen(dir);
    return path && strncmp(path, dir, len) == 0 && path[len] == '/';
}

static bool generated_cache_path(const char * path) {
    return under_directory(path, ALBUMART_DIR) || under_directory(path, ALBUMART_LEGACY_DIR);
}

static bool try_exts(char * path, int len);

static bool try_albumart_cache(const char * dir, const albumart_info_t * id3, const char * size_string, char * path) {
    const char * artist = id3->albumartist[0] ? id3->albumartist : id3->artist;
    if (!artist[0] || !id3->album[0]) return false;
    if (size_string[0]) {
        uint64_t keys[2] = { albumart_thumbnail_key(id3), thumbnail_key_legacy(id3) };
        for (size_t i = 0; i < 2; i++) {
            const char * version = i == 0 ? "v3" : "v2";
            int pathlen = snprintf(path, PATH_MAX, "%s/%s-%016llx%s.", dir, version,
                                   (unsigned long long) keys[i], size_string);
            if (pathlen < 0 || pathlen >= PATH_MAX) continue;
            fix_path_part(path, (int) strlen(dir) + 1, PATH_MAX);
            if (try_exts(path, pathlen)) return true;
        }
        return false;
    }
    int pathlen = snprintf(path, PATH_MAX, "%s/%s-%s%s.", dir, artist, id3->album, size_string);
    if (pathlen < 0 || pathlen >= PATH_MAX) return false;
    fix_path_part(path, (int) strlen(dir) + 1, PATH_MAX);
    return try_exts(path, pathlen);
}

static bool try_exts(char * path, int len) {
    if (len < 0 || (size_t) len >= PATH_MAX) return false;
    for (size_t i = 0; i < sizeof(extensions) / sizeof(extensions[0]); i++) {
        if ((size_t) len + 1 + strlen(extensions[i]) >= PATH_MAX) continue;
        path[len] = '\0';
        strcat(path, extensions[i]);
        if (file_exists(path)) return true;
    }
    path[len] = '\0';
    return false;
}

/* Tries <album><size_string>.*, then cover<size_string>.*, then (only when
 * size_string is empty) folder.jpg/.jpeg/.png, inside dir. dirlen is
 * strlen(dir) including the trailing '/'. path must point at a PATH_MAX-sized
 * buffer (same contract as try_exts); on success it holds the found path. */
static bool try_art_in_dir(const char * dir, int dirlen, const albumart_info_t * id3,
                            const char * size_string, int albumlen, char * path) {
    int pathlen;

    if (albumlen > 0) {
        pathlen = snprintf(path, PATH_MAX, "%s%s%s.", dir, id3->album, size_string);
        fix_path_part(path, dirlen, albumlen);
        if (try_exts(path, pathlen)) return true;
    }

    pathlen = snprintf(path, PATH_MAX, "%scover%s.", dir, size_string);
    if (try_exts(path, pathlen)) return true;

    if (size_string[0] == '\0') {
        snprintf(path, PATH_MAX, "%sfolder.jpg", dir);
        if (file_exists(path)) return true;
        snprintf(path, PATH_MAX, "%sfolder.jpeg", dir);
        if (file_exists(path)) return true;
        snprintf(path, PATH_MAX, "%sfolder.png", dir);
        if (file_exists(path)) return true;
    }

    return false;
}

/* Matches (case-insensitively) a directory name that is exactly a disc-set
 * marker: "cd"/"disc"/"disk", optional separators (space/'_'/'-'/'.'), then
 * one or more digits and nothing else -- e.g. "CD1", "Disc 2", "disk_03".
 * Deliberately narrow: bare numbers, Roman numerals and descriptive suffixes
 * ("2 Disc Set", "Disc") are excluded to avoid false positives on ordinary
 * (non-multi-disc) album folders. */
bool albumart_is_disc_folder(const char * name) {
    static const char * const prefixes[] = { "cd", "disc", "disk" };
    if (!name || !name[0]) return false;

    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
        size_t plen = strlen(prefixes[i]);
        size_t j;
        for (j = 0; j < plen; j++) {
            if (!name[j] || tolower((unsigned char) name[j]) != prefixes[i][j]) break;
        }
        if (j != plen) continue;

        const char * p = name + plen;
        while (*p == ' ' || *p == '_' || *p == '-' || *p == '.') p++;
        if (!*p) continue;

        bool all_digits = true;
        for (const char * q = p; *q; q++) {
            if (!('0' <= *q && *q <= '9')) { all_digits = false; break; }
        }
        if (all_digits) return true;
    }
    return false;
}

bool albumart_find_artist_sidecar(const char * directory, char * found, size_t found_size) {
    static const char * const names[] = {
        "artist.jpg", "artist.png", "folder.jpg", "folder.png", "cover.jpg", "cover.png"
    };
    if (!directory || !directory[0] || !found || found_size == 0) return false;
    DIR * dp = opendir(directory);
    if (!dp) return false;
    bool have = false;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]) && !have; i++) {
        rewinddir(dp);
        struct dirent * de;
        while ((de = readdir(dp)) != NULL) {
            if (strcasecmp(de->d_name, names[i]) != 0) continue;
            char path[PATH_MAX];
            int n = snprintf(path, sizeof(path), "%s%s%s", directory,
                             directory[strlen(directory) - 1] == '/' ? "" : "/", de->d_name);
            struct stat st;
            if (n <= 0 || (size_t) n >= sizeof(path) || stat(path, &st) != 0 || !S_ISREG(st.st_mode) ||
                strlen(path) >= found_size)
                continue;
            strmemccpy_local(found, path, found_size);
            have = true;
            break;
        }
    }
    closedir(dp);
    return have;
}

/* Extracts the final path component (no trailing slash) from dir (which
 * itself must end in '/', as produced by strip_filename()). */
static void basename_of_dir(const char * dir, char * out, size_t out_size) {
    out[0] = '\0';
    size_t len = strlen(dir);
    if (len < 2 || out_size == 0) return;
    size_t end = len - 1; /* skip trailing '/' */
    size_t start = end;
    while (start > 0 && dir[start - 1] != '/') start--;
    size_t n = end - start;
    if (n >= out_size) n = out_size - 1;
    memcpy(out, dir + start, n);
    out[n] = '\0';
}

#define SIBLING_DISC_SCAN_MAX 32
#define SIBLING_DISC_NAME_MAX 256

/* Unsized-only fallback for multi-disc layouts: when the current disc
 * folder (e.g. "Disc2") has no own art and the album-root retry also found
 * nothing, look at sibling disc folders (e.g. "Disc1") under the same
 * parent and use the first one (by name, ascending) that has its own art.
 * Gated by the caller on size_string being empty and current_disc_name
 * matching looks_like_disc_dir(); parent_dir must end in '/'. */
static bool find_sibling_disc_art(const char * parent_dir, const char * current_disc_name,
                                   const albumart_info_t * id3, int albumlen, char * path) {
    DIR * dp = opendir(parent_dir[0] ? parent_dir : ".");
    if (!dp) return false;

    char names[SIBLING_DISC_SCAN_MAX][SIBLING_DISC_NAME_MAX];
    int count = 0;
    struct dirent * de;

    while ((de = readdir(dp)) != NULL) {
        const char * name = de->d_name;
        if (name[0] == '.') continue;
        if (strcmp(name, current_disc_name) == 0) continue;
        if (strlen(name) >= SIBLING_DISC_NAME_MAX) continue;
        if (!albumart_is_disc_folder(name)) continue;

        char full[PATH_MAX];
        int n = snprintf(full, sizeof(full), "%s%s", parent_dir, name);
        if (n < 0 || (size_t) n >= sizeof(full)) continue;
        struct stat st;
        if (stat(full, &st) != 0 || !S_ISDIR(st.st_mode)) continue;

        if (count < SIBLING_DISC_SCAN_MAX) {
            strcpy(names[count], name);
            count++;
        } else {
            /* Keep the SIBLING_DISC_SCAN_MAX lexicographically-smallest
             * names collected so far (deterministic regardless of
             * readdir() order), by evicting the current largest if name
             * sorts smaller than it. */
            int max_idx = 0;
            for (int i = 1; i < SIBLING_DISC_SCAN_MAX; i++) {
                int cmp = strcasecmp(names[i], names[max_idx]);
                if (cmp > 0 || (cmp == 0 && strcmp(names[i], names[max_idx]) > 0)) max_idx = i;
            }
            int cmp = strcasecmp(name, names[max_idx]);
            if (cmp < 0 || (cmp == 0 && strcmp(name, names[max_idx]) < 0)) {
                strcpy(names[max_idx], name);
            }
        }
    }
    closedir(dp);

    if (count == 0) return false;

    for (int i = 0; i < count - 1; i++) {
        int best = i;
        for (int j = i + 1; j < count; j++) {
            int cmp = strcasecmp(names[j], names[best]);
            if (cmp < 0 || (cmp == 0 && strcmp(names[j], names[best]) < 0)) best = j;
        }
        if (best != i) {
            char tmp[SIBLING_DISC_NAME_MAX];
            strcpy(tmp, names[i]);
            strcpy(names[i], names[best]);
            strcpy(names[best], tmp);
        }
    }

    for (int i = 0; i < count; i++) {
        char sibling_dir[PATH_MAX];
        int n = snprintf(sibling_dir, sizeof(sibling_dir), "%s%s/", parent_dir, names[i]);
        if (n < 0 || (size_t) n >= sizeof(sibling_dir)) continue;
        if (try_art_in_dir(sibling_dir, n, id3, "", albumlen, path)) return true;
    }
    return false;
}

static bool albumart_search_files_internal(const albumart_info_t * id3, const char * size_string,
                                            char * buf, size_t buflen, bool include_generated_cache) {
    char path[PATH_MAX];
    char dir[PATH_MAX];
    char disc_name[SIBLING_DISC_NAME_MAX];
    bool found = false;
    bool walked_to_parent = false;
    int track_first = 1;
    int dirlen;
    int albumlen;
    int pathlen;

    if (!id3 || !buf || !size_string) return false;
    if (id3->path[0] == '\0' || strcmp(id3->path, "No file!") == 0) return false;

    if (*size_string == ':') {
        size_string++;
        track_first = 0;
    }

    strip_filename(dir, (int) sizeof(dir), id3->path);
    dirlen = (int) strlen(dir);
    albumlen = id3->album[0] ? (int) strlen(id3->album) : 0;
    disc_name[0] = '\0';

    for (int pass = 0; pass < 2 - track_first; pass++) {
        if (track_first || pass) {
            strip_extension(path, sizeof(path) - strlen(size_string) - 5, id3->path);
            strcat(path, size_string);
            strcat(path, ".");
            pathlen = (int) strlen(path);
            found = try_exts(path, pathlen);
        }
        if (pass) break;

        if (!found) found = try_art_in_dir(dir, dirlen, id3, size_string, albumlen, path);

        if (include_generated_cache) {
            if (!found) found = try_albumart_cache(ALBUMART_DIR, id3, size_string, path);
            /* A destination directory created before the legacy cache is moved
             * must not hide covers that are still stored under the old name. */
            if (!found) found = try_albumart_cache(ALBUMART_LEGACY_DIR, id3, size_string, path);
        }

        if (!found && dirlen > 1) {
            basename_of_dir(dir, disc_name, sizeof(disc_name));
            strcpy(path, dir);
            path[dirlen - 1] = '\0';
            strip_filename(dir, (int) sizeof(dir), path);
            dirlen = (int) strlen(dir);
            walked_to_parent = true;
        }

        if (dirlen > 0 && !found) found = try_art_in_dir(dir, dirlen, id3, size_string, albumlen, path);

        if (!found && walked_to_parent && dirlen > 0 && size_string[0] == '\0' && albumart_is_disc_folder(disc_name)) {
            found = find_sibling_disc_art(dir, disc_name, id3, albumlen, path);
        }

        if (found) break;
    }

    if (!found) return false;
    strmemccpy_local(buf, path, buflen);
    return true;
}

bool albumart_search_files(const albumart_info_t * id3, const char * size_string, char * buf, size_t buflen) {
    return albumart_search_files_internal(id3, size_string, buf, buflen, true);
}

bool albumart_search_source_files(const albumart_info_t * id3, const char * size_string,
                                  char * buf, size_t buflen) {
    return albumart_search_files_internal(id3, size_string, buf, buflen, false);
}

static void rgb565_to_bgr(uint16_t p, uint8_t * b, uint8_t * g, uint8_t * r) {
    uint8_t r5 = (uint8_t) ((p >> 11) & 0x1f);
    uint8_t g6 = (uint8_t) ((p >> 5) & 0x3f);
    uint8_t b5 = (uint8_t) (p & 0x1f);
    *r = (uint8_t) ((r5 << 3) | (r5 >> 2));
    *g = (uint8_t) ((g6 << 2) | (g6 >> 4));
    *b = (uint8_t) ((b5 << 3) | (b5 >> 2));
}

uint32_t albumart_source_mtime_with_path(const albumart_info_t * info, char * sidecar_path,
                                         size_t sidecar_path_size, bool * out_has_sidecar) {
    if (sidecar_path && sidecar_path_size > 0) sidecar_path[0] = '\0';
    if (out_has_sidecar) *out_has_sidecar = false;
    if (!info) return 0;
    char src[PATH_MAX];
    struct stat st;
    if (albumart_search_files(info, "", src, sizeof(src)) && stat(src, &st) == 0 && S_ISREG(st.st_mode)) {
        if (sidecar_path && sidecar_path_size > 0 && strlen(src) < sidecar_path_size)
            strmemccpy_local(sidecar_path, src, sidecar_path_size);
        if (out_has_sidecar) *out_has_sidecar = true;
        return (uint32_t) st.st_mtime;
    }
    if (info->path[0] && stat(info->path, &st) == 0 && S_ISREG(st.st_mode)) return (uint32_t) st.st_mtime;
    return 0;
}

uint32_t albumart_source_mtime(const albumart_info_t * info) {
    return albumart_source_mtime_with_path(info, NULL, 0, NULL);
}

static bool bmp_source_mtime(const char * path, int expected_width, int expected_height,
                             uint32_t * out) {
    if (!out || expected_width <= 0 || expected_height <= 0) return false;
    *out = 0;
    FILE * f = fopen(path, "rb");
    if (!f) return false;
    struct stat st;
    unsigned char hdr[54];
    bool ok = fstat(fileno(f), &st) == 0 && S_ISREG(st.st_mode) &&
              st.st_size >= (off_t) sizeof(hdr) && (uint64_t) st.st_size <= UINT32_MAX &&
              fread(hdr, 1, sizeof(hdr), f) == sizeof(hdr);
    fclose(f);
    if (!ok || hdr[0] != 'B' || hdr[1] != 'M') return false;

    uint32_t file_size = library_read_u32le(hdr + 2);
    uint32_t pixel_offset = library_read_u32le(hdr + 10);
    uint32_t dib_size = library_read_u32le(hdr + 14);
    uint32_t width = library_read_u32le(hdr + 18);
    uint32_t height = library_read_u32le(hdr + 22);
    uint16_t planes = library_read_u16le(hdr + 26);
    uint16_t bits = library_read_u16le(hdr + 28);
    uint32_t compression = library_read_u32le(hdr + 30);
    if (file_size != (uint32_t) st.st_size || dib_size < 40 ||
        pixel_offset < 54 || (uint64_t) pixel_offset < 14ULL + dib_size ||
        width != (uint32_t) expected_width || height != (uint32_t) expected_height ||
        planes != 1 || bits != 24 || compression != 0)
        return false;

    uint64_t row_bytes = (uint64_t) width * 3ULL;
    uint64_t stride = (row_bytes + 3ULL) & ~3ULL;
    uint64_t expected_size = (uint64_t) pixel_offset + stride * (uint64_t) height;
    if (expected_size != file_size) return false;

    *out = library_read_u32le(hdr + 6);
    return true;
}

uint64_t albumart_debug_thumbnail_key(const albumart_info_t * info) {
    return albumart_thumbnail_key(info);
}

uint64_t albumart_artist_thumbnail_key(const char * artist) {
    uint64_t hash = UINT64_C(14695981039346656037);
    const unsigned char * p = (const unsigned char *) (artist ? artist : "");
    do {
        unsigned char c = *p;
        if (c >= 'A' && c <= 'Z') c = (unsigned char) (c + ('a' - 'A'));
        hash = (hash ^ c) * UINT64_C(1099511628211);
    } while (*p++);
    return hash;
}

bool albumart_sized_thumb_fresh_with_source_mtime(const albumart_info_t * info, int width, int height,
                                                   uint32_t source_mtime, char * found, size_t found_size) {
    if (!info || !found || width <= 0 || height <= 0) return false;
    char size_string[24];
    snprintf(size_string, sizeof(size_string), ".%dx%d", width, height);
    if (!albumart_search_files(info, size_string, found, found_size)) return false;
    if (!generated_cache_path(found)) return true;
    uint32_t stored = 0;
    /* A false result makes the caller regenerate and atomically replace the
     * cache. Do not unlink by pathname here: another worker may have renamed
     * a valid replacement after this function opened the old inode, and an
     * unlink at this point would delete that fresh file. */
    if (!bmp_source_mtime(found, width, height, &stored)) return false;
    if (stored != 0 && source_mtime != 0 && stored != source_mtime) return false;
    return true;
}

bool albumart_sized_thumb_fresh(const albumart_info_t * info, int width, int height, char * found, size_t found_size) {
    return info && albumart_sized_thumb_fresh_with_source_mtime(info, width, height,
                    albumart_source_mtime(info), found, found_size);
}

static bool generated_cache_file(const char * dir, const albumart_info_t * info, int width, int height,
                                 char * path, size_t path_size) {
    uint64_t keys[2] = { albumart_thumbnail_key(info), thumbnail_key_legacy(info) };
    const char * versions[2] = { "v3", "v2" };
    for (size_t i = 0; i < 2; i++) {
        int pathlen = snprintf(path, path_size, "%s/%s-%016llx.%dx%d.bmp", dir, versions[i],
                               (unsigned long long) keys[i], width, height);
        if (pathlen > 0 && (size_t) pathlen < path_size && file_exists(path)) return true;
    }
    return false;
}

bool albumart_generated_cache_find(const albumart_info_t * info, int width, int height,
                                   char * found, size_t found_size) {
    if (!info || !found || found_size == 0 || width <= 0 || height <= 0 ||
        !info->album[0] || !(info->albumartist[0] || info->artist[0]))
        return false;
    char path[PATH_MAX];
    bool have = generated_cache_file(ALBUMART_DIR, info, width, height, path, sizeof(path));
    if (!have) have = generated_cache_file(ALBUMART_LEGACY_DIR, info, width, height, path, sizeof(path));
    if (!have || strlen(path) >= found_size) return false;
    memcpy(found, path, strlen(path) + 1);
    return true;
}

bool albumart_generated_cache_fresh(const albumart_info_t * info, int width, int height,
                                    char * found, size_t found_size) {
    if (!info || !found || found_size == 0 || width <= 0 || height <= 0 ||
        !info->album[0] || !(info->albumartist[0] || info->artist[0]))
        return false;

    char path[PATH_MAX];
    bool have = generated_cache_file(ALBUMART_DIR, info, width, height, path, sizeof(path));
    if (!have) have = generated_cache_file(ALBUMART_LEGACY_DIR, info, width, height, path, sizeof(path));
    if (!have || strlen(path) >= found_size) return false;

    uint32_t stored = 0;
    if (!bmp_source_mtime(path, width, height, &stored)) return false;
    uint32_t src = albumart_source_mtime(info);
    if (stored != 0 && src != 0 && stored != src) return false;
    strmemccpy_local(found, path, found_size);
    return found[0] != '\0';
}

static bool store_rgb565_file(const char * path, const char * dir, uint32_t src_mtime,
                              int width, int height, const uint16_t * pixels) {
    if (!path || !dir || !pixels || width <= 0 || height <= 0) return false;
    char tmp[PATH_MAX + 16];
    if (snprintf(tmp, sizeof(tmp), "%s.tmp.XXXXXX", path) >= (int) sizeof(tmp)) return false;

    int row_bytes = width * 3;
    int pad = (4 - (row_bytes % 4)) % 4;
    int stride = row_bytes + pad;
    uint32_t pixel_bytes = (uint32_t) stride * (uint32_t) height;
    uint32_t file_size = 14 + 40 + pixel_bytes;
    int fd = mkstemp(tmp);
    if (fd < 0) return false;
    FILE * f = fdopen(fd, "wb");
    if (!f) {
        close(fd);
        unlink(tmp);
        return false;
    }

    unsigned char hdr[54];
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B';
    hdr[1] = 'M';
    hdr[2] = (unsigned char) (file_size);
    hdr[3] = (unsigned char) (file_size >> 8);
    hdr[4] = (unsigned char) (file_size >> 16);
    hdr[5] = (unsigned char) (file_size >> 24);
    hdr[6] = (unsigned char) src_mtime;
    hdr[7] = (unsigned char) (src_mtime >> 8);
    hdr[8] = (unsigned char) (src_mtime >> 16);
    hdr[9] = (unsigned char) (src_mtime >> 24);
    hdr[10] = 54;
    hdr[14] = 40;
    hdr[18] = (unsigned char) (width);
    hdr[19] = (unsigned char) (width >> 8);
    hdr[20] = (unsigned char) (width >> 16);
    hdr[21] = (unsigned char) (width >> 24);
    hdr[22] = (unsigned char) (height);
    hdr[23] = (unsigned char) (height >> 8);
    hdr[24] = (unsigned char) (height >> 16);
    hdr[25] = (unsigned char) (height >> 24);
    hdr[26] = 1;
    hdr[28] = 24;
    bool ok = fwrite(hdr, 1, sizeof(hdr), f) == sizeof(hdr);
    unsigned char * rowbuf = ok ? malloc((size_t) stride) : NULL;
    if (ok && !rowbuf) ok = false;
    if (rowbuf) memset(rowbuf, 0, (size_t) stride);
    for (int y = height - 1; ok && y >= 0; y--) {
        const uint16_t * row = pixels + (size_t) y * width;
        for (int x = 0; x < width; x++) {
            uint8_t b, g, r;
            rgb565_to_bgr(row[x], &b, &g, &r);
            rowbuf[x * 3 + 0] = b;
            rowbuf[x * 3 + 1] = g;
            rowbuf[x * 3 + 2] = r;
        }
        if (fwrite(rowbuf, 1, (size_t) stride, f) != (size_t) stride) ok = false;
    }
    free(rowbuf);
    if (ok && fflush(f) != 0) ok = false;
    if (ok && fsync(fileno(f)) != 0) ok = false;
    if (fclose(f) != 0) ok = false;
    if (ok && rename(tmp, path) != 0) ok = false;
    if (ok) (void) library_fsync_dir(dir);
    if (!ok) unlink(tmp);
    return ok;
}

bool albumart_store_rgb565(const albumart_info_t * info, int width, int height, const uint16_t * pixels) {
    if (!info || !pixels || width <= 0 || height <= 0) return false;
    const char * artist = info->albumartist[0] ? info->albumartist : info->artist;
    if (!artist[0] || !info->album[0]) return false;

    mkdir(SD_COMPAS_ROOT, 0755);
    mkdir(ALBUMART_DIR, 0755);
    char path[PATH_MAX];
    int pathlen = snprintf(path, sizeof(path), "%s/v3-%016llx.%dx%d.bmp", ALBUMART_DIR,
                           (unsigned long long) albumart_thumbnail_key(info), width, height);
    if (pathlen < 0 || (size_t) pathlen >= sizeof(path)) return false;
    fix_path_part(path, (int) strlen(ALBUMART_DIR) + 1, PATH_MAX);
    return store_rgb565_file(path, ALBUMART_DIR, albumart_source_mtime(info), width, height, pixels);
}

static bool artist_thumbnail_path(const char * artist, int width, int height,
                                  const char * suffix, char * path, size_t path_size) {
    if (!artist || !artist[0] || width <= 0 || height <= 0 || !suffix || !path || path_size == 0) return false;
    int n = snprintf(path, path_size, "%s/artist-v1-%016llx.%dx%d.%s", ALBUMART_DIR,
                     (unsigned long long) albumart_artist_thumbnail_key(artist), width, height, suffix);
    return n > 0 && (size_t) n < path_size;
}

static bool artist_alias_path(const char * artist, unsigned int scope, char * path, size_t path_size) {
    if (!artist || !artist[0] || !path || path_size == 0) return false;
    int n = snprintf(path, path_size, "%s/artist-v1-%016llx-%u.album", ALBUMART_DIR,
                     (unsigned long long) albumart_artist_thumbnail_key(artist), scope);
    return n > 0 && (size_t) n < path_size;
}

bool albumart_artist_sized_thumb_fresh(const char * artist, uint32_t source_mtime,
                                       int width, int height, char * found, size_t found_size) {
    if (!found || found_size == 0) return false;
    char path[PATH_MAX];
    if (!artist_thumbnail_path(artist, width, height, "bmp", path, sizeof(path)) ||
        strlen(path) >= found_size)
        return false;
    uint32_t stored = 0;
    if (!bmp_source_mtime(path, width, height, &stored)) return false;
    if (stored != 0 && source_mtime != 0 && stored != source_mtime) return false;
    strmemccpy_local(found, path, found_size);
    return found[0] != '\0';
}

bool albumart_artist_store_rgb565(const char * artist, uint32_t source_mtime,
                                  int width, int height, const uint16_t * pixels) {
    if (!artist || !artist[0] || !pixels || width <= 0 || height <= 0) return false;
    mkdir(SD_COMPAS_ROOT, 0755);
    mkdir(ALBUMART_DIR, 0755);
    char path[PATH_MAX];
    if (!artist_thumbnail_path(artist, width, height, "bmp", path, sizeof(path))) return false;
    return store_rgb565_file(path, ALBUMART_DIR, source_mtime, width, height, pixels);
}

bool albumart_artist_negative_fresh(const char * artist, uint64_t source_signature) {
    char path[PATH_MAX];
    if (!artist_thumbnail_path(artist, ALBUMART_THUMBNAIL_SIZE, ALBUMART_THUMBNAIL_SIZE,
                               "noart", path, sizeof(path))) return false;
    FILE * f = fopen(path, "rb");
    if (!f) return false;
    unsigned char bytes[8];
    bool ok = fread(bytes, 1, sizeof(bytes), f) == sizeof(bytes) && fgetc(f) == EOF;
    fclose(f);
    if (!ok) return false;
    uint64_t stored = 0;
    for (int i = 0; i < 8; i++) stored |= (uint64_t) bytes[i] << (i * 8);
    return stored == source_signature;
}

bool albumart_artist_negative_exists(const char * artist) {
    char path[PATH_MAX];
    struct stat st;
    return artist_thumbnail_path(artist, ALBUMART_THUMBNAIL_SIZE, ALBUMART_THUMBNAIL_SIZE,
                                 "noart", path, sizeof(path)) &&
           stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

bool albumart_artist_store_negative(const char * artist, uint64_t source_signature) {
    if (!artist || !artist[0]) return false;
    mkdir(SD_COMPAS_ROOT, 0755);
    mkdir(ALBUMART_DIR, 0755);
    char path[PATH_MAX], tmp[PATH_MAX + 16];
    if (!artist_thumbnail_path(artist, ALBUMART_THUMBNAIL_SIZE, ALBUMART_THUMBNAIL_SIZE,
                               "noart", path, sizeof(path)) ||
        snprintf(tmp, sizeof(tmp), "%s.tmp.XXXXXX", path) >= (int) sizeof(tmp))
        return false;
    int fd = mkstemp(tmp);
    if (fd < 0) return false;
    unsigned char bytes[8];
    for (int i = 0; i < 8; i++) bytes[i] = (unsigned char) (source_signature >> (i * 8));
    size_t written = 0;
    bool ok = true;
    while (written < sizeof(bytes)) {
        ssize_t n = write(fd, bytes + written, sizeof(bytes) - written);
        if (n <= 0) {
            ok = false;
            break;
        }
        written += (size_t) n;
    }
    if (ok && fsync(fd) != 0) ok = false;
    if (close(fd) != 0) ok = false;
    if (ok && rename(tmp, path) != 0) ok = false;
    if (ok) (void) library_fsync_dir(ALBUMART_DIR);
    if (!ok) unlink(tmp);
    return ok;
}

bool albumart_artist_alias_load(const char * artist, unsigned int scope, uint64_t * album_key,
                                int64_t * representative_song_id) {
    if (!album_key || !representative_song_id) return false;
    char path[PATH_MAX];
    if (!artist_alias_path(artist, scope, path, sizeof(path))) return false;
    FILE * f = fopen(path, "rb");
    if (!f) return false;
    unsigned char bytes[24];
    bool ok = fread(bytes, 1, sizeof(bytes), f) == sizeof(bytes) && fgetc(f) == EOF;
    fclose(f);
    if (!ok || memcmp(bytes, "AALIAS1\0", 8) != 0) return false;
    uint64_t key = 0, song_id = 0;
    for (int i = 0; i < 8; i++) {
        key |= (uint64_t) bytes[8 + i] << (i * 8);
        song_id |= (uint64_t) bytes[16 + i] << (i * 8);
    }
    if (key == 0 || song_id == 0 || song_id > INT64_MAX) return false;
    *album_key = key;
    *representative_song_id = (int64_t) song_id;
    return true;
}

bool albumart_artist_alias_store(const char * artist, unsigned int scope, uint64_t album_key,
                                 int64_t representative_song_id) {
    if (!artist || !artist[0] || album_key == 0 || representative_song_id <= 0) return false;
    mkdir(SD_COMPAS_ROOT, 0755);
    mkdir(ALBUMART_DIR, 0755);
    char path[PATH_MAX], tmp[PATH_MAX + 16];
    if (!artist_alias_path(artist, scope, path, sizeof(path)) ||
        snprintf(tmp, sizeof(tmp), "%s.tmp.XXXXXX", path) >= (int) sizeof(tmp))
        return false;
    int fd = mkstemp(tmp);
    if (fd < 0) return false;
    unsigned char bytes[24] = { 'A', 'A', 'L', 'I', 'A', 'S', '1', 0 };
    uint64_t song_id = (uint64_t) representative_song_id;
    for (int i = 0; i < 8; i++) {
        bytes[8 + i] = (unsigned char) (album_key >> (i * 8));
        bytes[16 + i] = (unsigned char) (song_id >> (i * 8));
    }
    size_t written = 0;
    bool ok = true;
    while (written < sizeof(bytes)) {
        ssize_t n = write(fd, bytes + written, sizeof(bytes) - written);
        if (n <= 0) { ok = false; break; }
        written += (size_t) n;
    }
    if (ok && fsync(fd) != 0) ok = false;
    if (close(fd) != 0) ok = false;
    if (ok && rename(tmp, path) != 0) ok = false;
    if (ok) (void) library_fsync_dir(ALBUMART_DIR);
    if (!ok) unlink(tmp);
    return ok;
}

void albumart_artist_alias_remove(const char * artist, unsigned int scope) {
    char path[PATH_MAX];
    if (artist_alias_path(artist, scope, path, sizeof(path))) unlink(path);
}

static bool is_hex_digit_local(unsigned char c) {
    return ('0' <= c && c <= '9') || ('a' <= c && c <= 'f') || ('A' <= c && c <= 'F');
}

static bool has_hex_key(const char * text) {
    if (!text) return false;
    for (int i = 0; i < 16; i++)
        if (!text[i] || !is_hex_digit_local((unsigned char) text[i])) return false;
    return true;
}

static bool valid_generated_tail(const char * tail, bool allow_noart) {
    if (!tail || !('0' <= tail[0] && tail[0] <= '9')) return false;
    const char * p = tail;
    while ('0' <= *p && *p <= '9') p++;
    if (*p++ != 'x' || !('0' <= *p && *p <= '9')) return false;
    while ('0' <= *p && *p <= '9') p++;
    if (*p++ != '.') return false;

    const char * ext = NULL;
    if (strncmp(p, "bmp", 3) == 0 && (p[3] == '\0' || strncmp(p + 3, ".tmp.", 5) == 0))
        ext = "bmp";
    else if (allow_noart && strncmp(p, "noart", 5) == 0 &&
             (p[5] == '\0' || strncmp(p + 5, ".tmp.", 5) == 0))
        ext = "noart";
    if (!ext) return false;
    p += strlen(ext);
    if (*p == '\0') return true;
    if (strncmp(p, ".tmp.", 5) != 0) return false;
    p += 5;
    for (int i = 0; i < 6; i++, p++)
        if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'z') ||
              (*p >= 'A' && *p <= 'Z'))) return false;
    return *p == '\0';
}

static bool parse_album_cache_filename(const char * name, uint64_t * out_key) {
    if (!name || !out_key || (strncmp(name, "v2-", 3) != 0 && strncmp(name, "v3-", 3) != 0) ||
        !has_hex_key(name + 3) || name[19] != '.' || !valid_generated_tail(name + 20, true))
        return false;
    uint64_t key = 0;
    for (int i = 0; i < 16; i++) {
        unsigned char c = (unsigned char) name[3 + i];
        unsigned int nibble = c >= '0' && c <= '9' ? (unsigned int) (c - '0')
            : (unsigned int) (tolower(c) - 'a' + 10);
        key = (key << 4) | nibble;
    }
    *out_key = key;
    return true;
}

static bool parse_artist_cache_filename(const char * name, uint64_t * out_key,
                                        bool * out_is_alias) {
    static const char prefix[] = "artist-v1-";
    if (!name || !out_key || !out_is_alias || strncmp(name, prefix, sizeof(prefix) - 1) != 0 ||
        !has_hex_key(name + sizeof(prefix) - 1))
        return false;
    const char * separator = name + sizeof(prefix) - 1 + 16;
    if (*separator != '.' && *separator != '-') return false;
    uint64_t key = 0;
    for (int i = 0; i < 16; i++) {
        unsigned char c = (unsigned char) name[sizeof(prefix) - 1 + i];
        unsigned int nibble = c >= '0' && c <= '9' ? (unsigned int) (c - '0')
            : (unsigned int) (tolower(c) - 'a' + 10);
        key = (key << 4) | nibble;
    }
    if (*separator == '.') {
        if (!valid_generated_tail(separator + 1, true)) return false;
        *out_is_alias = false;
    } else {
        const char * p = separator + 1;
        if (!('0' <= *p && *p <= '9')) return false;
        while ('0' <= *p && *p <= '9') p++;
        if (strncmp(p, ".album", 6) != 0 ||
            !(* (p + 6) == '\0' || strncmp(p + 6, ".tmp.", 5) == 0)) return false;
        if (p[6] != '\0') {
            p += 11;
            for (int i = 0; i < 6; i++, p++)
                if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'z') ||
                      (*p >= 'A' && *p <= 'Z'))) return false;
            if (*p != '\0') return false;
        }
        *out_is_alias = true;
    }
    *out_key = key;
    return true;
}

static bool generated_cover_filename(const char * name) {
    uint64_t key;
    bool is_alias;
    return parse_album_cache_filename(name, &key) ||
           parse_artist_cache_filename(name, &key, &is_alias);
}

bool albumart_is_generated_cache_file(const char * path) {
    if (!generated_cache_path(path)) return false;
    const char * name = strrchr(path, '/');
    return name && generated_cover_filename(name + 1);
}

/* Deletion never follows a symlink: the cache root and its albumart folder
 * are opened with O_NOFOLLOW and entries are removed relative to that
 * descriptor, so a swapped-in link cannot redirect it elsewhere. */
typedef bool (*cache_name_match_fn)(int dir_fd, const char * name, void * context);

static bool remove_cache_files_matching(const char * root, cache_name_match_fn match, void * context) {
    int root_fd = open(root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (root_fd < 0) return errno == ENOENT;
    int dir_fd = openat(root_fd, "albumart", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    int open_errno = errno;
    close(root_fd);
    if (dir_fd < 0) return open_errno == ENOENT;
    int scan_fd = dup(dir_fd);
    DIR * dp = scan_fd >= 0 ? fdopendir(scan_fd) : NULL;
    if (!dp) {
        if (scan_fd >= 0) close(scan_fd);
        close(dir_fd);
        return false;
    }
    bool ok = true;
    for (;;) {
        errno = 0;
        struct dirent * de = readdir(dp);
        if (!de) {
            if (errno != 0) ok = false; /* an I/O error, not the end of the folder */
            break;
        }
        if (!match(dir_fd, de->d_name, context)) continue;
        struct stat st;
        if (fstatat(dir_fd, de->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
            if (errno != ENOENT) ok = false;
            continue;
        }
        if (!S_ISREG(st.st_mode)) continue;
        if (unlinkat(dir_fd, de->d_name, 0) != 0 && errno != ENOENT) ok = false;
    }
    if (closedir(dp) != 0) ok = false;
    close(dir_fd);
    return ok;
}

static const char * const cache_roots[] = { SD_COMPAS_ROOT, SD_LEGACY_ROOT };

static bool match_any_generated_cache(int dir_fd, const char * name, void * context) {
    (void) dir_fd;
    (void) context;
    return generated_cover_filename(name);
}

bool albumart_delete_all_generated_caches(void) {
    bool ok = true;
    for (size_t r = 0; r < sizeof(cache_roots) / sizeof(cache_roots[0]); r++)
        if (!remove_cache_files_matching(cache_roots[r], match_any_generated_cache, NULL)) ok = false;
    return ok;
}

static bool read_artist_alias_album_key(int dir_fd, const char * name, uint64_t * out_album_key) {
    int fd = openat(dir_fd, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return false;
    struct stat st;
    unsigned char bytes[25];
    bool ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size == 24 &&
              read(fd, bytes, sizeof(bytes)) == 24;
    close(fd);
    if (!ok || memcmp(bytes, "AALIAS1\0", 8) != 0) return false;
    uint64_t album_key = 0;
    for (int i = 0; i < 8; i++) album_key |= (uint64_t) bytes[8 + i] << (i * 8);
    *out_album_key = album_key;
    return album_key != 0;
}

typedef struct {
    uint64_t album_keys[2];
    uint64_t * artist_keys;
    size_t artist_count, artist_capacity;
    bool out_of_memory;
} album_reload_scan_t;

static void album_reload_add_artist(album_reload_scan_t * scan, uint64_t artist_key) {
    for (size_t i = 0; i < scan->artist_count; i++)
        if (scan->artist_keys[i] == artist_key) return;
    if (scan->artist_count == scan->artist_capacity) {
        size_t next = scan->artist_capacity ? scan->artist_capacity * 2 : 8;
        uint64_t * grown = realloc(scan->artist_keys, next * sizeof(*grown));
        if (!grown) {
            scan->out_of_memory = true;
            return;
        }
        scan->artist_keys = grown;
        scan->artist_capacity = next;
    }
    scan->artist_keys[scan->artist_count++] = artist_key;
}

/* Pass 1: this album's own files, and every artist whose saved alias points
 * at it (collected, removed in pass 2 with the rest of that family). */
static bool album_reload_first_pass(int dir_fd, const char * name, void * context) {
    album_reload_scan_t * scan = context;
    uint64_t key;
    bool is_alias;
    if (parse_album_cache_filename(name, &key))
        return key == scan->album_keys[0] || key == scan->album_keys[1];
    uint64_t album_key;
    if (parse_artist_cache_filename(name, &key, &is_alias) && is_alias &&
        read_artist_alias_album_key(dir_fd, name, &album_key) &&
        (album_key == scan->album_keys[0] || album_key == scan->album_keys[1]))
        album_reload_add_artist(scan, key);
    return false;
}

static bool album_reload_second_pass(int dir_fd, const char * name, void * context) {
    (void) dir_fd;
    album_reload_scan_t * scan = context;
    uint64_t key;
    bool is_alias;
    if (!parse_artist_cache_filename(name, &key, &is_alias)) return false;
    for (size_t i = 0; i < scan->artist_count; i++)
        if (scan->artist_keys[i] == key) return true;
    return false;
}

bool albumart_delete_generated_cache_for_album(const albumart_info_t * info,
                                               const uint64_t * artist_keys, size_t artist_key_count) {
    if (!info || !info->album[0]) return false;
    album_reload_scan_t scan = { .album_keys = { albumart_thumbnail_key(info), thumbnail_key_legacy(info) } };
    /* Every artist appearing on the album too: a "no art" marker or a
     * thumbnail derived before this album had art has no alias pointing here. */
    for (size_t i = 0; i < artist_key_count; i++) album_reload_add_artist(&scan, artist_keys[i]);
    if (info->artist[0]) album_reload_add_artist(&scan, albumart_artist_thumbnail_key(info->artist));
    if (info->albumartist[0]) album_reload_add_artist(&scan, albumart_artist_thumbnail_key(info->albumartist));
    bool ok = true;
    for (size_t r = 0; r < sizeof(cache_roots) / sizeof(cache_roots[0]); r++)
        if (!remove_cache_files_matching(cache_roots[r], album_reload_first_pass, &scan)) ok = false;
    for (size_t r = 0; r < sizeof(cache_roots) / sizeof(cache_roots[0]); r++)
        if (!remove_cache_files_matching(cache_roots[r], album_reload_second_pass, &scan)) ok = false;
    free(scan.artist_keys);
    return ok && !scan.out_of_memory;
}

albumart_load_result_t albumart_load_file_ex(const char * path, uint8_t ** out_data,
    uint32_t * out_size, uint32_t max_bytes, artwork_priority_t priority) {
    if (!out_data || !out_size) return ALBUMART_LOAD_INVALID;
    *out_data = NULL;
    *out_size = 0;
    if (!path || !path[0]) return ALBUMART_LOAD_INVALID;
    struct stat st;
    /* Nonblocking open prevents a substituted FIFO from hanging the worker
     * before we can reject non-regular files. Size comes from this same fd. */
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return ALBUMART_LOAD_TEMPORARY;
    if (fstat(fd, &st) != 0) {
        close(fd);
        return ALBUMART_LOAD_TEMPORARY;
    }
    if (!S_ISREG(st.st_mode) || st.st_size <= 0 || (uint64_t) st.st_size > max_bytes) {
        close(fd);
        return ALBUMART_LOAD_INVALID;
    }
    if (!artwork_check_memory_admission(priority, (size_t) st.st_size)) {
        close(fd);
        return ALBUMART_LOAD_TEMPORARY;
    }
    FILE * f = fdopen(fd, "rb");
    if (!f) {
        close(fd);
        return ALBUMART_LOAD_TEMPORARY;
    }
    uint8_t * data = malloc((size_t) st.st_size);
    bool ok = data && fread(data, 1, (size_t) st.st_size, f) == (size_t) st.st_size;
    fclose(f);
    if (!ok) {
        free(data);
        return ALBUMART_LOAD_TEMPORARY;
    }
    *out_data = data;
    *out_size = (uint32_t) st.st_size;
    return ALBUMART_LOAD_OK;
}
