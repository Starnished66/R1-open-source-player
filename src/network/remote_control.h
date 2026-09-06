#ifndef REMOTE_CONTROL_H
#define REMOTE_CONTROL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Phone remote-control server: serves a static Now Playing web page and a JSON
 * API (status polling, playback control, library browsing, playlist creation/addition).
 * Playback requests set flags that are polled and consumed by update_timer_cb. */

/* Starts the HTTP listener thread on REMOTE_CONTROL_PORT. Idempotent. */
void remote_control_start(void);

/* Stops the listener thread and its socket. Idempotent. */
void remote_control_stop(void);

/* Push a fresh now-playing snapshot for /api/status. Thread-safe snapshot copy.
 * path is the currently-playing file on disk. play_mode is gui.c's play_mode_t
 * cast to int (0=Sequential, 1=Repeat All, 2=Repeat One, 3=Shuffle). */
void remote_control_notify_status(bool playing, bool paused, const char * title, const char * artist,
                                   const char * album, const char * path, int position_seconds,
                                   int duration_seconds, float volume, int play_mode);

/* Poll from update_timer_cb only. Edge-triggered (cleared once consumed). */
bool remote_control_consume_play_pause(void);
bool remote_control_consume_next(void);
bool remote_control_consume_prev(void);

/* POST /api/playback/mode -- cycles play mode (Sequential -> Repeat All ->
 * Repeat One -> Shuffle -> Sequential). */
bool remote_control_consume_mode_cycle(void);

/* Edge-triggered seek and volume control consumption. out_seconds/out_percent
 * are only written when returning true. percent is 0-100. */
bool remote_control_consume_seek(int * out_seconds);
bool remote_control_consume_volume(int * out_percent);

/* POST /api/playback/queue?index=N -- enqueue one library song by metadata_db id. */
bool remote_control_consume_queue_index(int64_t * out_index);
bool remote_control_consume_queue_remove(int * out_offset);
bool remote_control_consume_queue_clear(void);

/* Snapshot the live playback queue for GET /api/queue. */
void remote_control_sync_queue(const char * const * paths, int count);

/* Consume requested song id to play. Scope strings (playlist, artist,
 * album_artist, album) narrow the context for building the playback queue. */
bool remote_control_consume_play_index(int64_t * out_index, char * out_playlist, size_t playlist_size,
                                         char * out_artist, size_t artist_size, char * out_album_artist,
                                         size_t album_artist_size, char * out_album, size_t album_size);

/* Playlist mutation (create playlist / add song) runs synchronously on the HTTP
 * thread.
 *
 * Additional endpoints:
 * - GET /api/library/artists and /api/library/album_artists
 * - GET /api/library/albums?artist=NAME or ?album_artist=NAME
 * - GET /api/library (with offset/limit/q and artist/album_artist/album filters)
 * - GET /api/playlists/songs?name=NAME
 * - GET /api/art (optional ?index=N)
 * - GET /assets/icon?name= */

#endif /* REMOTE_CONTROL_H */
