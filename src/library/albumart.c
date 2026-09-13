/* POSIX port of Rockbox apps/recorder/albumart.c.
 *
 * Copyright (C) 2007 Nicolas Pennequin (original search order)
 * Copyright (C) Open HiBy Player contributors (POSIX host)
 *
 * Search paths and invalid-character folding follow Rockbox. Sized thumbs
 * are stored under .open_hiby_player/albumart, next to tagcache. */

#include "albumart.h"
#include "library_endian.h"

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

#ifdef HOST_BUILD
  #define OPEN_HIBY_DIR "./.open_hiby_player"
#else
  #define OPEN_HIBY_DIR "/data/mnt/sd_0/.open_hiby_player"
#endif
#define ALBUMART_DIR OPEN_HIBY_DIR "/albumart"

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

/* Hash raw, separated identity fields before filename sanitization. */
static uint64_t thumbnail_key(const albumart_info_t * info) {
    const char * fields[] = { info->albumartist[0] ? info->albumartist : info->artist, info->album };
    uint64_t hash = UINT64_C(14695981039346656037);
    for (int i = 0; i < 2; i++) {
        const unsigned char * p = (const unsigned char *) fields[i];
        do { hash = (hash ^ *p) * UINT64_C(1099511628211); } while (*p++);
    }
    return hash;
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
static bool looks_like_disc_dir(const char * name) {
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
        if (!looks_like_disc_dir(name)) continue;

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

bool albumart_search_files(const albumart_info_t * id3, const char * size_string, char * buf, size_t buflen) {
    char path[PATH_MAX];
    char dir[PATH_MAX];
    char disc_name[SIBLING_DISC_NAME_MAX];
    bool found = false;
    bool walked_to_parent = false;
    int track_first = 1;
    const char * artist;
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

        artist = id3->albumartist[0] ? id3->albumartist : id3->artist;
        if (!found && artist[0] && id3->album[0]) {
            if (size_string[0])
                pathlen = snprintf(path, sizeof(path), "%s/v2-%016llx%s.", ALBUMART_DIR,
                                   (unsigned long long) thumbnail_key(id3), size_string);
            else
                pathlen = snprintf(path, sizeof(path), "%s/%s-%s%s.", ALBUMART_DIR, artist, id3->album, size_string);
            fix_path_part(path, (int) strlen(ALBUMART_DIR) + 1, PATH_MAX);
            found = try_exts(path, pathlen);
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

        if (!found && walked_to_parent && dirlen > 0 && size_string[0] == '\0' && looks_like_disc_dir(disc_name)) {
            found = find_sibling_disc_art(dir, disc_name, id3, albumlen, path);
        }

        if (found) break;
    }

    if (!found) return false;
    strmemccpy_local(buf, path, buflen);
    return true;
}

static void rgb565_to_bgr(uint16_t p, uint8_t * b, uint8_t * g, uint8_t * r) {
    uint8_t r5 = (uint8_t) ((p >> 11) & 0x1f);
    uint8_t g6 = (uint8_t) ((p >> 5) & 0x3f);
    uint8_t b5 = (uint8_t) (p & 0x1f);
    *r = (uint8_t) ((r5 << 3) | (r5 >> 2));
    *g = (uint8_t) ((g6 << 2) | (g6 >> 4));
    *b = (uint8_t) ((b5 << 3) | (b5 >> 2));
}

static uint32_t source_mtime_of(const albumart_info_t * info) {
    if (!info) return 0;
    char src[PATH_MAX];
    struct stat st;
    if (albumart_search_files(info, "", src, sizeof(src)) && stat(src, &st) == 0 && S_ISREG(st.st_mode))
        return (uint32_t) st.st_mtime;
    if (info->path[0] && stat(info->path, &st) == 0 && S_ISREG(st.st_mode)) return (uint32_t) st.st_mtime;
    return 0;
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
    return thumbnail_key(info);
}

bool albumart_sized_thumb_fresh(const albumart_info_t * info, int width, int height, char * found, size_t found_size) {
    if (!info || !found || width <= 0 || height <= 0) return false;
    char size_string[24];
    snprintf(size_string, sizeof(size_string), ".%dx%d", width, height);
    if (!albumart_search_files(info, size_string, found, found_size)) return false;
    size_t dir_len = strlen(ALBUMART_DIR);
    if (strncmp(found, ALBUMART_DIR, dir_len) != 0 || found[dir_len] != '/') return true;
    uint32_t stored = 0;
    /* A false result makes the caller regenerate and atomically replace the
     * cache. Do not unlink by pathname here: another worker may have renamed
     * a valid replacement after this function opened the old inode, and an
     * unlink at this point would delete that fresh file. */
    if (!bmp_source_mtime(found, width, height, &stored)) return false;
    uint32_t src = source_mtime_of(info);
    if (stored != 0 && src != 0 && stored != src) return false;
    return true;
}

bool albumart_generated_cache_fresh(const albumart_info_t * info, int width, int height,
                                    char * found, size_t found_size) {
    if (!info || !found || found_size == 0 || width <= 0 || height <= 0 ||
        !info->album[0] || !(info->albumartist[0] || info->artist[0]))
        return false;

    char path[PATH_MAX];
    int pathlen = snprintf(path, sizeof(path), "%s/v2-%016llx.%dx%d.bmp", ALBUMART_DIR,
                           (unsigned long long) thumbnail_key(info), width, height);
    if (pathlen < 0 || (size_t) pathlen >= sizeof(path) ||
        (size_t) pathlen >= found_size || !file_exists(path)) return false;

    uint32_t stored = 0;
    if (!bmp_source_mtime(path, width, height, &stored)) return false;
    uint32_t src = source_mtime_of(info);
    if (stored != 0 && src != 0 && stored != src) return false;
    strmemccpy_local(found, path, found_size);
    return found[0] != '\0';
}

bool albumart_store_rgb565(const albumart_info_t * info, int width, int height, const uint16_t * pixels) {
    if (!info || !pixels || width <= 0 || height <= 0) return false;
    const char * artist = info->albumartist[0] ? info->albumartist : info->artist;
    if (!artist[0] || !info->album[0]) return false;

    mkdir(OPEN_HIBY_DIR, 0755);
    mkdir(ALBUMART_DIR, 0755);

    char path[PATH_MAX], tmp[PATH_MAX + 16];
    int pathlen = snprintf(path, sizeof(path), "%s/v2-%016llx.%dx%d.bmp", ALBUMART_DIR,
                           (unsigned long long) thumbnail_key(info), width, height);
    if (pathlen < 0 || (size_t) pathlen >= sizeof(path)) return false;
    fix_path_part(path, (int) strlen(ALBUMART_DIR) + 1, PATH_MAX);
    if (snprintf(tmp, sizeof(tmp), "%s.tmp.XXXXXX", path) >= (int) sizeof(tmp)) return false;

    int row_bytes = width * 3;
    int pad = (4 - (row_bytes % 4)) % 4;
    int stride = row_bytes + pad;
    uint32_t pixel_bytes = (uint32_t) stride * (uint32_t) height;
    uint32_t file_size = 14 + 40 + pixel_bytes;
    uint32_t src_mtime = source_mtime_of(info);

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
    if (ok) (void) library_fsync_dir(ALBUMART_DIR);
    if (!ok) unlink(tmp);
    return ok;
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
