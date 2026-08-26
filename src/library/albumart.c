/* POSIX port of Rockbox apps/recorder/albumart.c.
 *
 * Copyright (C) 2007 Nicolas Pennequin (original search order)
 * Copyright (C) Open HiBy Player contributors (POSIX host)
 *
 * Search paths and invalid-character folding follow Rockbox. Sized thumbs
 * are stored under .open_hiby_player/albumart, next to tagcache. */

#include "albumart.h"

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

bool albumart_search_files(const albumart_info_t * id3, const char * size_string, char * buf, size_t buflen) {
    char path[PATH_MAX];
    char dir[PATH_MAX];
    bool found = false;
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

    for (int pass = 0; pass < 2 - track_first; pass++) {
        if (track_first || pass) {
            strip_extension(path, sizeof(path) - strlen(size_string) - 5, id3->path);
            strcat(path, size_string);
            strcat(path, ".");
            pathlen = (int) strlen(path);
            found = try_exts(path, pathlen);
        }
        if (pass) break;

        if (!found && albumlen > 0) {
            pathlen = snprintf(path, sizeof(path), "%s%s%s.", dir, id3->album, size_string);
            fix_path_part(path, dirlen, albumlen);
            found = try_exts(path, pathlen);
        }

        if (!found) {
            pathlen = snprintf(path, sizeof(path), "%scover%s.", dir, size_string);
            found = try_exts(path, pathlen);
        }

        if (!found && size_string[0] == '\0') {
            snprintf(path, sizeof(path), "%sfolder.jpg", dir);
            found = file_exists(path);
            if (!found) {
                snprintf(path, sizeof(path), "%sfolder.jpeg", dir);
                found = file_exists(path);
            }
            if (!found) {
                snprintf(path, sizeof(path), "%sfolder.png", dir);
                found = file_exists(path);
            }
        }

        artist = id3->albumartist[0] ? id3->albumartist : id3->artist;
        if (!found && artist[0] && id3->album[0]) {
            pathlen = snprintf(path, sizeof(path), "%s/%s-%s%s.", ALBUMART_DIR, artist, id3->album, size_string);
            fix_path_part(path, (int) strlen(ALBUMART_DIR) + 1, PATH_MAX);
            found = try_exts(path, pathlen);
        }

        if (!found && dirlen > 1) {
            strcpy(path, dir);
            path[dirlen - 1] = '\0';
            strip_filename(dir, (int) sizeof(dir), path);
            dirlen = (int) strlen(dir);
        }

        if (dirlen > 0) {
            if (!found && albumlen > 0) {
                pathlen = snprintf(path, sizeof(path), "%s%s%s.", dir, id3->album, size_string);
                fix_path_part(path, dirlen, albumlen);
                found = try_exts(path, pathlen);
            }
            if (!found) {
                pathlen = snprintf(path, sizeof(path), "%scover%s.", dir, size_string);
                found = try_exts(path, pathlen);
            }
        }
        if (found) break;
    }

    if (!found) return false;
    strmemccpy_local(buf, path, buflen);
    return true;
}

bool albumart_find(const albumart_info_t * info, char * buf, size_t buflen, int width, int height) {
    if (!info || !buf) return false;
    char size_string[24];
    if (width > 0 && height > 0)
        snprintf(size_string, sizeof(size_string), ".%dx%d", width, height);
    else
        size_string[0] = '\0';
    if (size_string[0] && albumart_search_files(info, size_string, buf, buflen)) return true;
    return albumart_search_files(info, "", buf, buflen);
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

static uint16_t bmp_le16(const unsigned char * p) {
    return (uint16_t) p[0] | ((uint16_t) p[1] << 8);
}

static uint32_t bmp_le32(const unsigned char * p) {
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) |
           ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
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

    uint32_t file_size = bmp_le32(hdr + 2);
    uint32_t pixel_offset = bmp_le32(hdr + 10);
    uint32_t dib_size = bmp_le32(hdr + 14);
    uint32_t width = bmp_le32(hdr + 18);
    uint32_t height = bmp_le32(hdr + 22);
    uint16_t planes = bmp_le16(hdr + 26);
    uint16_t bits = bmp_le16(hdr + 28);
    uint32_t compression = bmp_le32(hdr + 30);
    if (file_size != (uint32_t) st.st_size || dib_size < 40 ||
        pixel_offset < 54 || (uint64_t) pixel_offset < 14ULL + dib_size ||
        width != (uint32_t) expected_width || height != (uint32_t) expected_height ||
        planes != 1 || bits != 24 || compression != 0)
        return false;

    uint64_t row_bytes = (uint64_t) width * 3ULL;
    uint64_t stride = (row_bytes + 3ULL) & ~3ULL;
    uint64_t expected_size = (uint64_t) pixel_offset + stride * (uint64_t) height;
    if (expected_size != file_size) return false;

    *out = bmp_le32(hdr + 6);
    return true;
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

bool albumart_store_rgb565(const albumart_info_t * info, int width, int height, const uint16_t * pixels) {
    if (!info || !pixels || width <= 0 || height <= 0) return false;
    const char * artist = info->albumartist[0] ? info->albumartist : info->artist;
    if (!artist[0] || !info->album[0]) return false;

    mkdir(OPEN_HIBY_DIR, 0755);
    mkdir(ALBUMART_DIR, 0755);

    char path[PATH_MAX], tmp[PATH_MAX + 16];
    int pathlen = snprintf(path, sizeof(path), "%s/%s-%s.%dx%d.bmp", ALBUMART_DIR, artist, info->album, width, height);
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
    if (ok) {
        int dfd = open(ALBUMART_DIR, O_RDONLY | O_DIRECTORY);
        if (dfd >= 0) {
            fsync(dfd);
            close(dfd);
        }
    }
    if (!ok) unlink(tmp);
    return ok;
}

bool albumart_load_file(const char * path, uint8_t ** out_data, uint32_t * out_size, uint32_t max_bytes) {
    *out_data = NULL;
    *out_size = 0;
    if (!path || !path[0]) return false;
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) return false;
    if ((uint64_t) st.st_size > max_bytes) return false;
    FILE * f = fopen(path, "rb");
    if (!f) return false;
    uint8_t * data = malloc((size_t) st.st_size);
    bool ok = data && fread(data, 1, (size_t) st.st_size, f) == (size_t) st.st_size;
    fclose(f);
    if (!ok) {
        free(data);
        return false;
    }
    *out_data = data;
    *out_size = (uint32_t) st.st_size;
    return true;
}
