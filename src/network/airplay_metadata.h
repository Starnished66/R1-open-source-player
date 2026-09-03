#ifndef AIRPLAY_METADATA_H
#define AIRPLAY_METADATA_H

#include <stdbool.h>
#include <stdint.h>

#include "board_config.h"

/* Reads shairport's own metadata FIFO -- created automatically inside its
 * "-M" meta-dir (see airplay_control.c's "-M /tmp"), confirmed via `strings`
 * against the real binary ("Could not create metadata FIFO %s", "now_
 * playing") and by capturing real traffic live during on-device testing.
 * Wire format (confirmed live, not guessed): plain "key=value\n" lines --
 * artist/title/album/genre/comment/artwork -- one blank line terminating
 * each record. artwork's value is a filename (e.g. "cover-<hash>.jpg")
 * relative to the SAME meta-dir, not a full path.
 *
 * Consumed by a dedicated AirPlay overlay screen (gui_network.c's
 * airplay_overlay_screen, built alongside bt_dac_overlay_screen/usb_dac_
 * overlay_screen) rather than the Player screen -- an earlier pass reused
 * the Player screen's own widgets directly and hid its transport controls,
 * which turned out more trouble than it was worth (a shadowed-global bug
 * silently left prev/next unhideable, the layout looked broken with only
 * some controls hidden, and it coupled this feature's state into player
 * globals it has no other business touching). JPEG decode (cover_decode_
 * to_rgb565(), library/cover_decode.h) and the reflection blur this file
 * used to also compute are no longer done here -- the overlay only needs a
 * plain cover image, not the Player screen's blurred-reflection panel
 * effect. */

#define AIRPLAY_NOW_PLAYING_PATH "/tmp/now_playing"

/* Must match whatever size the AirPlay overlay screen displays cover art
 * at (gui_network.c, build_airplay_overlay_screen()) -- the consumer
 * assumes buffers of exactly these fixed dimensions. Tracks
 * BOARD_PLAYER_COVER_HEIGHT (board_config.h), same as the Player screen's
 * own COVER_ART_HEIGHT, since both display the identical playing_plane/
 * default_cover_565.png placeholder before real art arrives -- a bare
 * 480x480 here would leave real decoded AirPlay art a different size than
 * that placeholder on any board whose own copy of that asset isn't
 * square. */
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
 * Returns false only if the thread itself could not be created -- see
 * airplay_bridge_start()'s own comment on the identical, equally
 * near-impossible-on-this-target failure mode and why it's still checked. */
bool airplay_metadata_start(void);

/* Stops the thread -- same fire-and-forget shape as airplay_bridge_stop()
 * (see its own doc comment for why: every caller runs on the UI thread and
 * none need a synchronous exit guarantee). Safe to call when not running. */
void airplay_metadata_stop(void);

/* Called by airplay_bridge.c the instant a PCM streaming session ends on
 * its own (phone disconnect, read error) -- distinct from airplay_
 * metadata_stop() above, which tears the reader thread down entirely. This
 * module's own reader has no notion of the bridge's PCM session boundaries
 * (it runs its own independent connection cycle against /tmp/now_playing
 * and keeps running across many such bridge sessions without ever
 * stopping itself), so without a call here at each one, a metadata/artwork
 * decode still in flight for a session that just ended could still publish
 * afterward and surface on a later reconnect before that one's own
 * metadata arrives. Safe to call whether or not anything was actually
 * pending or mid-decode; does not stop the reader thread. */
void airplay_metadata_invalidate(void);

/* Poll from the LVGL/main thread only. Returns true once when a new
 * track's metadata (and, if artwork was present, its decoded cover) has
 * finished processing -- *out is filled in and ownership of its pixel
 * buffer passes to the caller. Returns false (out untouched) otherwise;
 * cheap to call every tick. */
bool airplay_metadata_consume_update(airplay_metadata_update_t * out);

#endif /* AIRPLAY_METADATA_H */
