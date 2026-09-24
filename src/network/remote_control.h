#ifndef REMOTE_CONTROL_H
#define REMOTE_CONTROL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Phone remote-control server: serves a static Now Playing web page and a JSON
 * API (status polling, playback control, library browsing, playlist creation/addition).
 * Existing API routes are also available under the /api/v1/ prefix; GET
 * /api/v1/capabilities describes the API version and currently available
 * features and supported transports (Wi-Fi HTTP and Bluetooth Classic RFCOMM).
 * Transport support is advertised independently of whether its radio is
 * currently enabled. Playback remains local to the player.
 * Playback requests set flags that are polled and consumed by update_timer_cb. */

/* Starts the HTTP listener thread on REMOTE_CONTROL_PORT. Idempotent. */
void remote_control_start(void);

/* Stops the listener thread and its socket. Idempotent. */
void remote_control_stop(void);

/* Replace the shared Wi-Fi/Bluetooth Remote Control PIN with a new random
 * 6-digit PIN, persist it, and clear any PIN lockout. Paired apps must be
 * given the new PIN. Returns false if the system random source is
 * unavailable, leaving the current PIN unchanged. */
bool remote_control_generate_new_pin(void);
/* Copy the current PIN, generating the random 6-digit PIN first if none has
 * been created yet. out_pin is empty only if that generation failed. */
void remote_control_get_pin(char * out_pin, size_t out_pin_size);

/* Handles one HTTP request on a connected byte-stream socket (for example an
 * RFCOMM connection). The caller retains ownership of fd and must close it. */
void remote_control_handle_stream(int fd);

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

/* POST /api/playback/mode without a parameter cycles play mode. With
 * ?mode=N, selects N directly (0=Sequential, 1=Repeat All, 2=Repeat One,
 * 3=Shuffle). Both requests are consumed on the UI thread. */
bool remote_control_consume_mode_cycle(void);
bool remote_control_consume_play_mode(int * out_mode);

/* Edge-triggered seek and volume control consumption. out_seconds/out_percent
 * are only written when returning true. percent is 0-100. */
bool remote_control_consume_seek(int * out_seconds);
bool remote_control_consume_volume(int * out_percent);

/* POST /api/playback/queue?index=N -- enqueue one library song by metadata_db id. */
bool remote_control_consume_queue_index(int64_t * out_index);
bool remote_control_consume_queue_remove(int * out_offset, uint64_t * out_revision);
bool remote_control_consume_queue_clear(uint64_t * out_revision);

/* Snapshot the live playback queue for GET /api/queue. */
void remote_control_sync_queue(const char * const * paths, int count, uint64_t revision);

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
 * - GET /api/library/genres
 * - GET /api/library/albums?artist=NAME or ?album_artist=NAME
 * - GET /api/library (with offset/limit/q and artist/album_artist/album/genre filters)
 * - GET /api/playlists/songs?name=NAME
 * - GET /api/art (optional ?index=N)
 * - GET /assets/icon?name= */

#endif /* REMOTE_CONTROL_H */
