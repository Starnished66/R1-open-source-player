#ifndef REMOTE_CONTROL_H
#define REMOTE_CONTROL_H

#include <stdbool.h>
#include <stddef.h>

/* Phone remote-control feature: a small hand-rolled HTTP/1.1 server
 * serving a static Now Playing web page and a JSON API (status polling,
 * playback control, library browsing, playlist create/add-to). No
 * authentication, consistent with this project's existing DLNA/AirPlay/
 * Import-via-WiFi trust model -- anyone on the same Wi-Fi network can
 * control the device, a deliberate tradeoff. Playback requests use the
 * same "background thread sets a flag, only update_timer_cb consumes it"
 * pattern as hw_buttons.c and bt_media_player.c's transport buttons. */

/* Starts the HTTP listener thread on REMOTE_CONTROL_PORT (see .c). Caller
 * (gui.c) should only call this once Wi-Fi has a real IP. Idempotent. */
void remote_control_start(void);

/* Stops the listener thread and its socket. Idempotent. */
void remote_control_stop(void);

/* Push a fresh now-playing snapshot for /api/status to serve -- call once
 * per gui.c tick. Cheap mutex-guarded struct copy, no I/O; safe to call
 * even when the listener isn't running. path is the currently-playing
 * file on disk, used only by GET /api/art (no index given) to know which
 * file to pull embedded cover art from. play_mode is gui.c's own
 * play_mode_t cast to int (0=Sequential, 1=Repeat All, 2=Repeat One,
 * 3=Shuffle) -- duplicated as a plain int here rather than pulling in
 * gui.c's own enum, same reasoning as this file's own MUSIC_ROOT_DIR
 * duplication above. */
void remote_control_notify_status(bool playing, bool paused, const char * title, const char * artist,
                                   const char * album, const char * path, int position_seconds,
                                   int duration_seconds, float volume, int play_mode);

/* Poll from update_timer_cb only, same edge-triggered "true once, then
 * clears itself" convention as hw_buttons_consume_play_pause()/
 * bt_media_player_consume_play_pause(). */
bool remote_control_consume_play_pause(void);
bool remote_control_consume_next(void);
bool remote_control_consume_prev(void);

/* POST /api/playback/mode -- same one-button cycle as the player screen's
 * own order_icon_event_cb (Sequential -> Repeat All -> Repeat One ->
 * Shuffle -> Sequential), not an independent shuffle/repeat toggle pair --
 * the app has no state to represent shuffle and repeat at the same time.
 * Consumer should call whatever gui.c function order_icon_event_cb itself
 * calls, so the player screen's icon and this stay in sync. */
bool remote_control_consume_mode_cycle(void);

/* Same edge-triggered convention, but also hands back the requested
 * target -- out_seconds/out_percent are only written when this returns
 * true. percent is 0-100 (this app's volume slider range), not the raw
 * 0.0-1.0 audio_set_volume() takes. */
bool remote_control_consume_seek(int * out_seconds);
bool remote_control_consume_volume(int * out_percent);

/* POST /api/playback/queue?index=N -- enqueue one library song through
 * gui.c's normal Up Next splice. out_index uses the same synced-library
 * index space as remote_control_consume_play_index(). */
bool remote_control_consume_queue_index(int * out_index);
bool remote_control_consume_queue_remove(int * out_offset);
bool remote_control_consume_queue_clear(void);

/* Snapshot the live Up Next run for GET /api/queue. Paths are copied and
 * translated to synced-library indices while the GUI owns playlist[]. */
void remote_control_sync_queue(const char * const * paths, int count);

/* Hands remote_control.c its own copy of the library's title/artist
 * strings, indexed exactly the way gui.c's all_songs_paths/all_song_tags
 * are -- GET /api/library returns these same indices, and POST
 * /api/playback/play?index=N is interpreted against this same indexing,
 * so the two must never drift apart. Call once after gui_init()'s startup
 * scan and again after every library rescan. Copies the strings rather
 * than taking ownership, since gui.c frees and reallocates its own arrays
 * on every rescan. */
void remote_control_sync_library(const char * const * titles, const char * const * artists,
                                  const char * const * album_artists, const char * const * albums,
                                  const char * const * paths, int count);

/* Same edge-triggered convention as remote_control_consume_seek() --
 * out_index is an index into the array remote_control_sync_library() was
 * last called with. out_playlist/out_artist/out_album_artist/out_album are
 * always written (empty string if the request carried no such context, the
 * "whole library" case) -- see request_play_playlist_name's own comment in
 * remote_control.c for why these exist: they let the caller scope the
 * playback queue to whichever Album/Playlist/Artist view the song was
 * actually tapped from, matching the on-device Group Songs/Playlist
 * screens, instead of always queuing the entire library. */
bool remote_control_consume_play_index(int * out_index, char * out_playlist, size_t playlist_size,
                                        char * out_artist, size_t artist_size, char * out_album_artist,
                                        size_t album_artist_size, char * out_album, size_t album_size);

/* Playlist mutation (create a playlist / add a song to an existing one),
 * mirroring gui.c's in-app "Add to Playlist" flow (same PLAYLISTS_DIR,
 * same playlist_files_create()/_append()/_contains() calls). Unlike
 * playback actions these run synchronously on the HTTP thread itself
 * rather than through a consume-flag, since they're plain file I/O with
 * no LVGL/audio state involved.
 *
 * The API also exposes: GET /api/library/artists and
 * /api/library/album_artists (distinct names + counts), GET
 * /api/library/albums?artist=NAME or ?album_artist=NAME (that artist's
 * distinct albums + counts), and artist=/album_artist=/album= exact-match
 * filters on GET /api/library on top of offset/limit/q -- together these
 * let the phone UI walk Artist -> Albums -> Songs the same way gui.c's
 * show_artist_albums() does. GET /api/playlists/songs?name=NAME lists one
 * playlist's songs, skipping any entry that no longer matches a synced
 * library path. GET /api/art (optionally ?index=N) streams a song's
 * embedded cover art; with no index, serves the currently-playing track's
 * art. GET /assets/icon?name= serves one of a small fixed whitelist of the
 * stock firmware's theme2/category/ PNG icons, so the phone UI can reuse
 * the same icons gui.c's build_music_screen() uses. */

#endif /* REMOTE_CONTROL_H */
