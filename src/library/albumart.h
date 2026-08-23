#ifndef ALBUMART_H
#define ALBUMART_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* POSIX port of Rockbox apps/recorder/albumart.c (GPLv2+).
 * Search order matches find_albumart()/search_albumart_files():
 *   ./<track><size>.{jpeg,jpg,png,bmp}
 *   ./<album><size>.{jpeg,jpg,png,bmp}
 *   ./cover<size>.{jpeg,jpg,png,bmp}
 *   ./folder.{jpg,jpeg,png}  (unsized pass only)
 *   <musicroot>/.rockbox/albumart/<artist>-<album><size>.{jpeg,jpg,png,bmp}
 *   same album/cover names in the parent directory
 * <size> is ".WxH" or empty for a generic file. */

typedef struct {
    char path[600];
    char artist[128];
    char album[128];
    char albumartist[128];
} albumart_info_t;

bool albumart_find(const albumart_info_t * info, char * buf, size_t buflen, int width, int height);
bool albumart_search_files(const albumart_info_t * info, const char * size_string, char * buf, size_t buflen);

/* Writes <musicroot>/.rockbox/albumart/<artist>-<album>.WxH.bmp from RGB565.
 * Source cover/audio mtime is stored in the BMP reserved field so a later
 * load can detect a replaced cover. */
bool albumart_store_rgb565(const albumart_info_t * info, int width, int height, const uint16_t * pixels);

/* True when a sized cache file exists and still matches the current source
 * cover (or audio file) mtime. User-supplied sized files next to the track
 * are accepted as-is. */
bool albumart_sized_thumb_fresh(const albumart_info_t * info, int width, int height, char * found, size_t found_size);

/* Reads a JPEG/PNG/BMP found by albumart_find into *out_data (caller frees). */
bool albumart_load_file(const char * path, uint8_t ** out_data, uint32_t * out_size, uint32_t max_bytes);

#endif /* ALBUMART_H */
