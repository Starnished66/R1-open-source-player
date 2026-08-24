#ifndef REMOTE_STATE_H
#define REMOTE_STATE_H

#include <stdbool.h>
#include <stdint.h>

/* Path-keyed sidecar for favorite/play-count state of tracks that are not
 * tagcache rows: remote:// plugin keys, Subsonic stream URLs, and local
 * files played before they are scanned. Local-library screens still list
 * only tagcache rows; this file is the invisible persistence those docs
 * describe. When a path is later upserted into tagcache, take() migrates
 * the row across. */

/* Drops the in-RAM sidecar so the next access rereads the file on the
 * currently mounted SD card. */
void remote_state_drop(void);
bool remote_state_get(const char * path, int32_t * rating, int32_t * playcount, int32_t * last_played);
void remote_state_set_rating(const char * path, int32_t rating);
void remote_state_add_play(const char * path, int32_t now);
/* Copies stats for path and deletes the sidecar row. False if none. */
bool remote_state_take(const char * path, int32_t * rating, int32_t * playcount, int32_t * last_played);

#endif /* REMOTE_STATE_H */
