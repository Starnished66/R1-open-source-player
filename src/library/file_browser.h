#include <stdatomic.h>
#ifndef FILE_BROWSER_H
#define FILE_BROWSER_H

#include "lvgl/lvgl.h"
#include <stdbool.h>

/* Called when the user taps a playable file. `playlist` holds `count`
 * heap-allocated absolute paths -- every playable file in the same
 * directory as the tapped one, sorted the same way they're listed on
 * screen -- and `selected_index` says which one was tapped. The callee
 * takes ownership: free each entry and then the array itself. */
typedef void (*file_browser_select_cb_t)(char ** playlist, int count, int selected_index);

/* Called when the user taps a .cue sheet -- unlike a playable file or .m3u
 * playlist above, a cue sheet doesn't get played directly (it describes
 * track boundaries within one ALREADY-listed audio file, not a file of its
 * own to hand to a decoder); the callee is expected to parse it (cue_parser.h)
 * and open its own track-list screen. cue_path is the tapped file's full
 * path, valid only for the duration of the callback (same convention as
 * file_browser_select_cb_t's own playlist array -- copy it if needed past
 * this call). */
typedef void (*file_browser_cue_select_cb_t)(const char * cue_path);

/* Builds the file browser UI (current-path label + scrollable list) as a
 * child of `parent`, starting at `root_dir`. The user can descend into
 * subdirectories and back up, but never above `root_dir`. on_cue_select may
 * be NULL (a caller that doesn't care about .cue sheets at all -- they're
 * then just hidden from the listing entirely, same as any other
 * unrecognized file, rather than shown with nothing wired up to tap). */
void file_browser_init(lv_obj_t * parent, const char * root_dir, file_browser_select_cb_t on_select,
                        file_browser_cue_select_cb_t on_cue_select);

/* Resets the browser back to its root directory and re-scans it from disk,
 * discarding whatever subdirectory the user was previously browsing. For
 * refreshing after the underlying storage changes out from under the UI
 * (SD card removed/reinserted) rather than in response to user navigation.
 * No-op if file_browser_init() hasn't run yet. */
void file_browser_reset_to_root(void);

/* One-shot lookup that doesn't touch (or require) any browser UI state:
 * scans path's containing directory and builds the same kind of playlist
 * tapping it in the browser would have, for resuming a track on launch
 * before the user has ever opened the browser screen. On success, the
 * caller owns *out_playlist the same way as the select callback above.
 * Returns false if the directory can't be read or path isn't among its
 * playable files (e.g. it was deleted/moved since). */
bool file_browser_build_playlist_for_path(const char * path, char *** out_playlist, int * out_count, int * out_selected_index);

/* Bounded-memory library walk. Each playable file is delivered immediately
 * to cb; the path is valid only for the duration of the callback. Returning
 * false from cb stops the walk. out_count receives the number of songs
 * delivered. Memory use is bounded by the recursion stack + one PATH_MAX
 * buffer per active directory, independent of library size. */
typedef bool (*file_browser_song_visit_cb_t)(const char * path, void * user);

/* Database-oriented variant which prunes one immediate child directory of
 * root, case-insensitively. Descendants with the same name elsewhere are
 * not skipped. */
bool file_browser_walk_all_songs_excluding_top_level(const char * root, const char * excluded_dir,
                                                     file_browser_song_visit_cb_t cb, void * user,
                                                     int * out_count, atomic_int * progress);

/* Snapshot of the directory + on-screen row a file/playlist was last
 * tapped from -- for the player's "List" option to reopen the folder a
 * track was played from. The getters are only meaningful right after a tap
 * (same convention as e.g. lv_event_get_user_data() being valid only
 * within its own callback), so gui.c must read them synchronously from its
 * own select_cb before returning. Both return defaults if file_browser_init()
 * hasn't run yet. */
const char * file_browser_get_last_selected_dir(void);
int file_browser_get_last_selected_row(void);

/* True when the browser is showing root_dir itself. */
bool file_browser_at_root(void);

/* Steps current_dir up one level toward root_dir and rebuilds the list.
 * No-op if already at root. */
void file_browser_go_up(void);

/* Parses a M3U/M3U8 playlist file: one entry path per non-blank,
 * non-comment line, resolved relative to the playlist's own directory
 * (standard M3U convention) unless already absolute. Entries that aren't
 * recognized playable audio files are skipped rather than passed through
 * to the decoder to fail on. Same caller-owned-array convention as the
 * select callback above. Returns false if the file can't be opened or has
 * no playable entries. Exposed (not file_browser.c-local) for gui.c's
 * Playlists screen (Music submenu) to open a user-created .m3u without
 * going through the interactive browser UI. */
bool file_browser_build_playlist_from_m3u(const char * m3u_path, char *** out_playlist, int * out_count);

#endif /* FILE_BROWSER_H */
