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
 *   <musicroot>/.compas/albumart/<artist>-<album><size>.{jpeg,jpg,png,bmp}
 *   the same names under <musicroot>/.open_hiby_player/albumart until moved
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

/* Same source-sidecar search order as albumart_search_files(), but skips the
 * player-generated RGB565-derived BMP caches. Remote catalog sync uses this
 * to serve the original compressed sidecar bytes instead of a resized cache. */
bool albumart_search_source_files(const albumart_info_t * info, const char * size_string,
                                  char * buf, size_t buflen);

/* Writes <musicroot>/.compas/albumart/<artist>-<album>.WxH.bmp from RGB565.
 * Source cover/audio mtime is stored in the BMP reserved field so a later
 * load can detect a replaced cover. */
bool albumart_store_rgb565(const albumart_info_t * info, int width, int height, const uint16_t * pixels);

/* True when a sized cache file exists and still matches the current source
 * cover (or audio file) mtime. User-supplied sized files next to the track
 * are accepted as-is. */
bool albumart_sized_thumb_fresh(const albumart_info_t * info, int width, int height, char * found, size_t found_size);
bool albumart_sized_thumb_fresh_with_source_mtime(const albumart_info_t * info, int width, int height,
                                                   uint32_t source_mtime, char * found, size_t found_size);

/* Strict variant for consumers that need the player-generated cache rather
 * than an arbitrary user-supplied .WxH image beside the track.  It only
 * accepts the hashed atomic BMP produced by albumart_store_rgb565(). */
bool albumart_generated_cache_fresh(const albumart_info_t * info, int width, int height,
                                    char * found, size_t found_size);

/* Shared case-insensitive (album, effective album artist) identity used by
 * tagcache album grouping, catalog album keys, and artwork thumbnail keys. */
uint64_t albumart_thumbnail_key(const albumart_info_t * info);

/* Exposes the shared thumbnail key for diagnostics only. Not for constructing
 * paths outside this file. */
uint64_t albumart_debug_thumbnail_key(const albumart_info_t * info);

/* Artist identity for the local library's shared thumbnail LRU (album
 * entries use albumart_thumbnail_key() above). Case folded, with its own
 * on-disk namespace. */
uint64_t albumart_artist_thumbnail_key(const char * artist);
bool albumart_is_disc_folder(const char * name);
bool albumart_find_artist_sidecar(const char * directory, char * found, size_t found_size);
uint32_t albumart_source_mtime(const albumart_info_t * info);
uint32_t albumart_source_mtime_with_path(const albumart_info_t * info, char * sidecar_path,
                                         size_t sidecar_path_size, bool * out_has_sidecar);

/* Artist thumbnails share the albumart cache directory, with a separate
 * filename namespace and a source mtime in the BMP header. Negative markers
 * store a caller-computed signature of the artist and album directories. */
bool albumart_artist_sized_thumb_fresh(const char * artist, uint32_t source_mtime,
                                       int width, int height, char * found, size_t found_size);
bool albumart_artist_store_rgb565(const char * artist, uint32_t source_mtime,
                                  int width, int height, const uint16_t * pixels);
/* Cheap existence check, so callers only compute the (stat-heavy) directory
 * signature when a marker is actually present. */
bool albumart_artist_negative_exists(const char * artist);
bool albumart_artist_negative_fresh(const char * artist, uint64_t source_signature);
bool albumart_artist_store_negative(const char * artist, uint64_t source_signature);
bool albumart_artist_alias_load(const char * artist, unsigned int scope, uint64_t * album_key,
                                int64_t * representative_song_id);
bool albumart_artist_alias_store(const char * artist, unsigned int scope, uint64_t album_key,
                                 int64_t representative_song_id);
void albumart_artist_alias_remove(const char * artist, unsigned int scope);

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
