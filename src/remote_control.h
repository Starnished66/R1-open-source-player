#ifndef REMOTE_CONTROL_H
#define REMOTE_CONTROL_H

#include <stdbool.h>

/* Phone remote-control feature -- see the project's own remote-control
 * design plan for the full multi-phase scope. A small hand-rolled
 * HTTP/1.1 server (matching this project's existing http_client.c
 * precedent for "no vendored HTTP library needed, this app's own request
 * surface is small and fixed"), serving a static Now Playing web page and
 * a JSON API: /api/status (polled), playback control (Phase 2), library
 * browsing, and playlist create/add-to (Phase 3). Not implemented:
 * deleting/renaming a playlist remotely, and removing a single song from
 * one (gui.c's own in-app Edit mode is still the only way to do either).
 *
 * No authentication -- consistent with this project's existing DLNA/
 * AirPlay/Import-via-WiFi trust model (anyone on the same Wi-Fi network
 * can reach it). Playback control and playlist mutation both widen that
 * from "can see what's playing" to "can control/change the device" -- a
 * real, deliberate tradeoff, not an oversight; a shared PIN/token is a
 * possible later hardening step, not implemented here.
 *
 * Modeled on dlna_control.c's own accept-loop-with-close-to-unblock shape
 * and http_client.c's own raw-socket conventions -- no HTTP/network-
 * listener code existed anywhere in this project before this. Playback
 * requests use the same "background thread sets a flag, only
 * update_timer_cb (the one thread allowed to touch LVGL/audio state)
 * consumes it" pattern already used for hw_buttons.c and
 * bt_media_player.c's own transport buttons -- see gui.c's own consumer
 * block, right next to those two. */

/* Starts the HTTP listener thread on REMOTE_CONTROL_PORT (see .c). Does
 * nothing if Wi-Fi has no IP yet -- caller (gui.c) is expected to only
 * call this when wifi_control_get_info() already reports a real address,
 * same gate the Import via Wi-Fi screen already uses. Idempotent -- safe
 * to call again while already running. */
void remote_control_start(void);

/* Stops the listener thread and its socket. Idempotent. */
void remote_control_stop(void);

/* Push a fresh now-playing snapshot for /api/status to serve -- call once
 * per gui.c tick, matching every other "background thread reads a
 * mutex-guarded snapshot the main thread refreshes" shape already used in
 * this codebase (bt_media_player.c, dlna_control.c). Cheap: just a
 * mutex-guarded struct copy, no I/O. Safe to call even when the listener
 * isn't running (the snapshot is just unread in that case).
 *
 * path is the currently-playing file on disk (gui.c's own playlist[
 * playlist_index], NULL/empty if nothing is loaded) -- used only by
 * GET /api/art (no index given) to know which file to pull embedded cover
 * art from; not otherwise displayed. */
void remote_control_notify_status(bool playing, bool paused, const char * title, const char * artist,
                                   const char * album, const char * path, int position_seconds,
                                   int duration_seconds, float volume);

/* Poll from update_timer_cb only, same edge-triggered "true once, then
 * clears itself" convention as hw_buttons_consume_play_pause()/
 * bt_media_player_consume_play_pause(). */
bool remote_control_consume_play_pause(void);
bool remote_control_consume_next(void);
bool remote_control_consume_prev(void);

/* Same edge-triggered convention, but also hands back the requested
 * target -- out_seconds/out_percent are only written when this returns
 * true. percent is 0-100 (this app's own volume slider range, see
 * volume_slider in gui.c), not the raw 0.0-1.0 audio_set_volume() takes;
 * callers convert the same way the physical volume buttons' own consumer
 * block already does. */
bool remote_control_consume_seek(int * out_seconds);
bool remote_control_consume_volume(int * out_percent);

/* Phase 3 (library browsing): hands remote_control.c its own copy of the
 * library's title/artist strings, indexed exactly the way gui.c's own
 * all_songs_paths/all_song_tags are -- GET /api/library returns these same
 * indices, and POST /api/playback/play?index=N (see
 * remote_control_consume_play_index() below) is interpreted against this
 * same indexing by all_songs_row_click_cb(), so the two must never drift
 * apart. Call once after gui_init()'s startup scan and again after every
 * library_scan_once()/library_load_from_cache_only() completes (both
 * gui.c call sites that rebuild all_songs_screen) -- see gui.c's own call
 * sites. Copies the strings (doesn't take ownership of titles/artists
 * themselves), since library_load_from_cache_only()/library_scan_once()
 * free and reallocate gui.c's own arrays on every rescan and this needs to
 * survive independently of that. */
void remote_control_sync_library(const char * const * titles, const char * const * artists,
                                  const char * const * album_artists, const char * const * albums,
                                  const char * const * paths, int count);

/* Same edge-triggered convention as remote_control_consume_seek() --
 * out_index is an index into the array remote_control_sync_library() was
 * last called with. */
bool remote_control_consume_play_index(int * out_index);

/* Phase 3b (playlist mutation): create a playlist / add a song to an
 * existing one, mirroring gui.c's own in-app "Add to Playlist" flow
 * exactly (new_playlist_name_done_cb()/existing_playlist_row_cb() --
 * same PLAYLISTS_DIR, same playlist_files_create()/_append()/_contains()
 * calls). Unlike playback actions, these run synchronously on the HTTP
 * thread itself rather than through a consume-flag -- they're plain file
 * I/O (via the paths remote_control_sync_library() already provides),
 * no LVGL/audio state involved, so there's no main-thread requirement.
 * GET /api/playlists lists existing playlists directly too (a fresh
 * playlist_files_scan() per request -- cheap, and always current, unlike
 * the synced library which only refreshes on rescan).
 *
 * Also added since the original three-phase plan: GET /api/library/artists
 * (distinct track-artist names + counts) and GET /api/library/album_artists
 * (same, but for album artist) -- both derived server-side from the arrays
 * remote_control_sync_library() takes. GET /api/library/albums?artist=NAME
 * or ?album_artist=NAME lists that artist's distinct albums + counts.
 * GET /api/library itself now takes optional artist=/album_artist=/album=
 * exact-match filters (ANDed with each other and with q=) on top of the
 * existing offset/limit/q paging -- together these let the phone UI walk
 * Artist -> Albums -> Songs (or Album Artist -> Albums -> Songs) the same
 * way gui.c's own show_artist_albums() does, instead of only a flat
 * searchable list. GET /api/playlists/songs?name=NAME lists one playlist's
 * songs (via the new playlist_files_read(), skipping any entry that no
 * longer matches a synced library path -- same silent-skip behavior as
 * gui.c's own rebuild_playlist_m3u_group()). GET /api/art (optionally
 * ?index=N) streams a song's embedded cover art image bytes -- with no
 * index, serves the currently-playing track's art (see
 * remote_control_notify_status()'s path parameter). GET /assets/icon?name=
 * serves one of a small fixed whitelist of the stock firmware's own
 * theme2/category/ PNG icons (all/artist/album_artist/genre), straight off
 * disk -- lets the phone UI's own Library menu reuse the exact same icons
 * gui.c's build_music_screen() uses, rather than a second icon set. */

#endif /* REMOTE_CONTROL_H */
