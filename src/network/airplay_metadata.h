#ifndef AIRPLAY_METADATA_H
#define AIRPLAY_METADATA_H

#include <stdbool.h>
#include <stdint.h>

#include "board_config.h"

/* Reads shairport's metadata FIFO (/tmp/now_playing).
 * Format consists of "key=value\n" records separated by blank lines.
 * Artwork is decoded to RGB565 and displayed in the AirPlay overlay screen. */

#define AIRPLAY_NOW_PLAYING_PATH "/tmp/now_playing"

/* Cover art dimensions for the AirPlay overlay screen. */
#define AIRPLAY_COVER_WIDTH BOARD_SCREEN_WIDTH
#define AIRPLAY_COVER_HEIGHT BOARD_PLAYER_COVER_HEIGHT

#define AIRPLAY_META_TITLE_MAX 256
#define AIRPLAY_META_ARTIST_MAX 256
#define AIRPLAY_META_ALBUM_MAX 256

typedef struct {
    char title[AIRPLAY_META_TITLE_MAX];
    char artist[AIRPLAY_META_ARTIST_MAX];
    char album[AIRPLAY_META_ALBUM_MAX];
    bool has_cover;
    uint16_t * cover_pixels;      /* AIRPLAY_COVER_WIDTH x HEIGHT RGB565 -- caller-owned, must free() */
} airplay_metadata_update_t;

/* Starts the metadata-reader thread. Safe to call again while already
 * running (no-op, returns true). Call alongside airplay_bridge_start().
 * Returns true on success, false if thread creation fails. */
bool airplay_metadata_start(void);

/* Stops the metadata-reader thread asynchronously. Safe to call when not running. */
void airplay_metadata_stop(void);

/* Called by airplay_bridge.c when a streaming session ends to invalidate
 * pending metadata and artwork decodes, preventing stale track info on subsequent sessions. */
void airplay_metadata_invalidate(void);

/* Poll from the LVGL/main thread only. Returns true once when a new
 * track's metadata (and, if artwork was present, its decoded cover) has
 * finished processing -- *out is filled in and ownership of its pixel
 * buffer passes to the caller. Returns false (out untouched) otherwise;
 * cheap to call every tick. */
bool airplay_metadata_consume_update(airplay_metadata_update_t * out);

#endif /* AIRPLAY_METADATA_H */
