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

/* Builds the file browser UI (current-path label + scrollable list) as a
 * child of `parent`, starting at `root_dir`. The user can descend into
 * subdirectories and back up, but never above `root_dir`. */
void file_browser_init(lv_obj_t * parent, const char * root_dir, file_browser_select_cb_t on_select);

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

/* Recursively scans every subdirectory of root for playable files, sorted
 * alphabetically by full path -- for library-wide views (e.g. "All Songs")
 * that aren't scoped to one directory the way the interactive browser
 * above is. Caller owns *out_paths (free each entry, then the array).
 * Returns false if root can't be read or has no playable files anywhere
 * under it.
 *
 * progress, if non-NULL, is incremented (not overwritten -- start it at 0)
 * every time a playable file is found, so a caller running this on a
 * background thread can tell genuine forward progress on a large/deep
 * library apart from a stuck readdir()/lstat() (real filesystem
 * corruption) without needing its own copy of the running count -- see
 * gui.c's scan_all_songs_with_timeout() for the actual stall-detection
 * logic this exists to support. */
bool file_browser_scan_all_songs(const char * root, char *** out_paths, int * out_count, volatile int * progress);

/* Snapshot of the directory + on-screen row a file/playlist was last
 * tapped from, and a way to jump the browser back there -- for the
 * player's "List" option to reopen the folder a track was played from.
 * The getters are only meaningful right after a tap (same convention as
 * e.g. lv_event_get_user_data() being valid only within its own
 * callback), so gui.c must read them synchronously from its own
 * select_cb before returning. file_browser_navigate_to() points the
 * browser at `dir`, rebuilds its row list as if the user had tapped
 * their way there, and scrolls `row_to_reveal` (the exact value
 * file_browser_get_last_selected_row() returned) into view; both are
 * no-ops if file_browser_init() hasn't run yet. */
const char * file_browser_get_last_selected_dir(void);
int file_browser_get_last_selected_row(void);
void file_browser_navigate_to(const char * dir, int row_to_reveal);

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
