#ifndef ALBUMART_H
#define ALBUMART_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "artwork_coordinator.h"

/* Persistent RGB565 cache sizes shared by the library and player.  The
 * on-card representation is a 24-bit BMP, while callers receive RGB565
 * buffers from cover_decode.c. */
#define ALBUMART_THUMBNAIL_SIZE 72
#define ALBUMART_PLAYER_CACHE_SIZE 480

/* POSIX port of Rockbox apps/recorder/albumart.c (GPLv2+), extended with a
 * multi-disc fallback (own repo addition, not present in upstream Rockbox).
 * Search order matches find_albumart()/search_albumart_files(), plus that
 * extension:
 *   ./<track><size>.{jpeg,jpg,png,bmp}
 *   ./<album><size>.{jpeg,jpg,png,bmp}
 *   ./cover<size>.{jpeg,jpg,png,bmp}
 *   ./folder.{jpg,jpeg,png}  (unsized pass only)
 *   <musicroot>/.open_hiby_player/albumart/<artist>-<album><size>.{jpeg,jpg,png,bmp}
 *   same album/cover/folder names in the parent directory
 *   (unsized only) if this directory's name looks like a disc marker
 *   ("CD1", "Disc 2", "disk_03", ...), the same album/cover/folder names
 *   in the lexicographically-first sibling directory (under the same
 *   parent) whose name also looks like a disc marker and that has art
 * <size> is ".WxH" or empty for a generic file. */

typedef struct {
    char path[600];
    char artist[128];
    char album[128];
    char albumartist[128];
} albumart_info_t;

bool albumart_search_files(const albumart_info_t * info, const char * size_string, char * buf, size_t buflen);

/* Writes <musicroot>/.open_hiby_player/albumart/<artist>-<album>.WxH.bmp from RGB565.
 * Source cover/audio mtime is stored in the BMP reserved field so a later
 * load can detect a replaced cover. */
bool albumart_store_rgb565(const albumart_info_t * info, int width, int height, const uint16_t * pixels);

/* True when a sized cache file exists and still matches the current source
 * cover (or audio file) mtime. User-supplied sized files next to the track
 * are accepted as-is. */
bool albumart_sized_thumb_fresh(const albumart_info_t * info, int width, int height, char * found, size_t found_size);

/* Strict variant for consumers that need the player-generated cache rather
 * than an arbitrary user-supplied .WxH image beside the track.  It only
 * accepts the hashed atomic BMP produced by albumart_store_rgb565(). */
bool albumart_generated_cache_fresh(const albumart_info_t * info, int width, int height,
                                    char * found, size_t found_size);

/* Exposes the internal v2-<hash> filename key for diagnostics only (e.g.
 * logging the exact key a lookup computed, to compare against what's
 * actually on disk). Not for constructing paths outside this file. */
uint64_t albumart_debug_thumbnail_key(const albumart_info_t * info);

typedef enum {
    ALBUMART_LOAD_OK,
    ALBUMART_LOAD_INVALID,
    ALBUMART_LOAD_TEMPORARY,
} albumart_load_result_t;

/* Admit the opened file's actual size before allocating/reading its data.
 * Memory pressure, allocation failure and I/O failure remain retryable. */
albumart_load_result_t albumart_load_file_ex(const char * path, uint8_t ** out_data,
    uint32_t * out_size, uint32_t max_bytes, artwork_priority_t priority);

#endif /* ALBUMART_H */
