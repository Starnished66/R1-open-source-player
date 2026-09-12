#ifndef LIBRARY_ENDIAN_H
#define LIBRARY_ENDIAN_H

#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

static inline uint16_t library_read_u16le(const uint8_t * p) {
    return (uint16_t) (p[0] | (p[1] << 8));
}

static inline uint32_t library_read_u32le(const uint8_t * p) {
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

static inline bool library_is_m3u_file(const char * name) {
    const char * ext = strrchr(name, '.');
    return ext && (strcasecmp(ext, ".m3u") == 0 || strcasecmp(ext, ".m3u8") == 0);
}

static inline size_t library_trim_eol(char * line) {
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';
    return len;
}

static inline size_t library_utf8_bom_skip(const char * p) {
    return (strncmp(p, "\xEF\xBB\xBF", 3) == 0) ? 3 : 0;
}

static inline bool library_fsync_dir(const char * dir) {
    int fd = open(dir, O_RDONLY | O_DIRECTORY);
    if (fd < 0) return false;
    bool ok = fsync(fd) == 0;
    close(fd);
    return ok;
}

#endif /* LIBRARY_ENDIAN_H */
