#ifndef PLAYLIST_FILES_H
#define PLAYLIST_FILES_H

#include <stdbool.h>
#include <stddef.h>

/* Lists .m3u/.m3u8 files in `root` itself (not a recursive walk). Sorted
 * alphabetically by full path. Caller owns *out_paths (free each entry,
 * then the array). Returns false if root can't be read or has no playlist
 * files in it. */
bool playlist_files_scan(const char * root, char *** out_paths, int * out_count);
/* Recursive, complete scan: success includes an empty directory; failure
 * never publishes a partial result. Symlink directories are not followed. */
bool playlist_files_scan_complete(const char * root, char *** out_paths, int * out_count);
void playlist_files_refresh_async(const char * root);
bool playlist_files_refresh_poll(void);
bool playlist_files_reconcile(const char * root);
bool playlist_files_rename(const char * path, const char * name, char * out, size_t size);
/* Entry offsets count nonempty, non-comment lines, including unavailable files.
 * to < 0 removes exactly one occurrence; otherwise moves it before offset to. */
bool playlist_files_edit_entry(const char * path, int from, int to);
bool playlist_files_write_new(const char * dir, const char * name,
                              const char * const * paths, int count, char * out, size_t size);

/* Appends song_path as a new line to the M3U file at m3u_path, creating the
 * file (but not its parent directory) if it doesn't already exist. Returns
 * false if the file can't be opened for appending. */
bool playlist_files_append(const char * m3u_path, const char * song_path);

/* True if song_path already appears as a line in the M3U file at m3u_path
 * (exact string match). False if the file doesn't exist/can't be read, or
 * the path just isn't in it -- callers that need to distinguish those two
 * cases don't exist yet, so this collapses them the same way
 * playlist_files_append()'s own bool return does. */
bool playlist_files_contains(const char * m3u_path, const char * song_path);

/* Rewrites the M3U file at m3u_path with every line matching song_path
 * exactly removed (all occurrences, not just the first -- cheap insurance
 * against a stray duplicate from before playlist_files_contains() started
 * being checked on add). Returns false if m3u_path can't be read or the
 * rewrite can't be completed. */
bool playlist_files_remove(const char * m3u_path, const char * song_path);

/* Creates a new M3U playlist at dir/name.m3u (creating dir if needed) with
 * song_path as its first entry. name must be non-empty and is used as-is
 * for the filename (the caller is responsible for it being a sane filename
 * component, same trust level as every other user-typed name in this app --
 * see show_text_entry() call sites elsewhere). Writes the full created path
 * into out_path (if non-NULL). Returns false on any I/O failure. */
bool playlist_files_create(const char * dir, const char * name, const char * song_path, char * out_path,
                            size_t out_path_size);

/* Deletes the whole M3U file at m3u_path outright (not a song within it --
 * see playlist_files_remove() for that). Used both for an explicit
 * user-initiated "delete this playlist" and for auto-deleting a playlist
 * that's become empty (see gui.c's playlist_row_click_cb()/
 * group_song_remove_row_cb()) -- never call this on Favorites/Most Played,
 * which aren't backed by a real M3U file at all. Returns false if the file
 * doesn't exist or can't be removed. */
bool playlist_files_delete(const char * m3u_path);
bool playlist_files_has_active_write(void);

/* Reads back a playlist's song paths, in file order -- the read-back
 * counterpart to playlist_files_append()/_create(). Skips blank lines and
 * '#'-prefixed M3U directives/comments. Doesn't filter by file extension
 * the way file_browser.c's own file_browser_build_playlist_from_m3u() does
 * (that one also has to handle directory listings and hand-dropped M3U
 * files, which can contain non-audio entries) -- these playlists are only
 * ever written by this app's own playlist_files_append()/_create(), which
 * already only ever write real song paths, and this function deliberately
 * stays free of any LVGL/screen dependency (file_browser.c pulls those in
 * via screen_builders.h) so it can be called from a plain background
 * thread. Caller owns *out_paths (free each entry, then the array).
 * Returns false if the file can't be read. */
bool playlist_files_read(const char * m3u_path, char *** out_paths, int * out_count);
typedef enum {
    PLAYLIST_READ_OK, PLAYLIST_READ_EMPTY, PLAYLIST_READ_IO_ERROR,
    PLAYLIST_READ_INVALID, PLAYLIST_READ_NO_MEMORY
} playlist_read_status_t;
playlist_read_status_t playlist_files_read_ex(const char * path, char *** paths, int * count);

/* Resolves a single M3U line (as read via fgets(), already trimmed of its
 * trailing newline) against the directory m3u_path itself lives in -- a
 * line starting with '/' is treated as already-absolute and used as-is;
 * anything else is joined onto m3u_path's own directory (the standard M3U
 * convention, and the one every writer in this file now uses -- see
 * playlist_files_append()/_create()'s own comments). Shared by
 * playlist_files_read()/_contains()/_remove(), and by file_browser.c's own
 * file_browser_build_playlist_from_m3u(), which used to duplicate this
 * exact logic inline. */
void playlist_files_resolve_path(const char * m3u_path, const char * line, char * out_full_path, size_t out_size);

/* Rewrites every .m3u/.m3u8 file in dir (found via playlist_files_scan())
 * so every line becomes relative to that file's own
 * directory, converting any old absolute-path entries left over from
 * before playlist_files_append()/_create() started writing relative ones.
 * Runs at most once: gated by a marker file (dir/.relative_paths_migrated),
 * so calling this on every boot is a cheap no-op after the first real run.
 * Not meant to be called on a schedule -- gui_init() calls it once, guarded
 * #ifndef HOST_BUILD, before the first library/playlist scan. */
void playlist_files_migrate_to_relative(const char * dir);

/* Cached index of discovered .m3u paths -- tagcache does not store
 * playlists, so this lives as a path list next to the music database
 * directory rather than in the tag files. Refreshed by a full scan;
 * insert/delete keep a single create/delete instant. */
void playlist_files_index_replace(char * const * paths, int count);
void playlist_files_index_load(char *** out_paths, int * out_count);
void playlist_files_index_insert(const char * path);
void playlist_files_index_delete(const char * path);

#endif /* PLAYLIST_FILES_H */
