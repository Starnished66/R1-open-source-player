#ifndef METADATA_DB_H
#define METADATA_DB_H

#include <stdbool.h>
#include <stdint.h>

/* On-disk cache of every scanned song's title/artist/album/album_artist/
 * genre tags, keyed by path + the file's mtime/size at the time it was
 * cached --
 * lets library_scan_once() (gui.c) skip re-reading tags (metadata_read(),
 * one open+parse per file) for every file that hasn't changed since the
 * last scan, the same role the stock hiby_player's own usrlocal_media.db
 * plays for its boot-time scan (confirmed via `file`/sqlite3's `.recover`
 * against a real device's SD card). */

typedef struct {
    char title[128];
    char artist[128];
    char album[128];
    char album_artist[128];
    char genre[128];
} cached_tags_t;

/* Opens (creating and migrating the schema if needed) the on-disk cache at
 * its fixed path (/usr/data on target, ./ on host -- same convention as
 * settings.c). Safe to call more than once; a no-op if already open. */
void metadata_db_open(void);
void metadata_db_close(void);

/* Starts a scan pass. Rows touched during the pass are stamped with a new
 * persistent scan generation. Writes are committed in small batches, so RAM
 * and rollback-journal growth remain bounded instead of scaling with the
 * library. metadata_db_end_update() prunes rows from older generations. */
void metadata_db_begin_update(void);

/* Looks up `path`, marking it as seen for this pass either way. Returns
 * true and fills *out only if a cached row exists whose stored mtime/size
 * match the values passed in -- callers should treat false as "the file is
 * new or has changed, re-read its real tags and call metadata_db_put()". */
bool metadata_db_get(const char * path, int64_t mtime, int64_t size, cached_tags_t * out);

/* Inserts or replaces the cached row for `path`. */
void metadata_db_put(const char * path, int64_t mtime, int64_t size, const cached_tags_t * tags);

/* Ends a successful scan pass: deletes rows whose scan generation was not
 * refreshed by this pass (files removed/renamed), then commits. */
void metadata_db_end_update(void);

/* Ends an interrupted scan without pruning unseen rows. Already-written rows
 * remain valid; the next complete pass advances the scan generation. */
void metadata_db_abort_update(void);

/* Enumerates every cached row as-is, without touching the filesystem or
 * verifying mtime/size against anything live -- for gui_init()'s boot-time
 * library load (see its own comment), which needs to match the stock
 * player's own boot behavior (load the db, don't rescan) rather than
 * walking the whole music root and re-reading every file's tags before the
 * first frame can render. *out_paths and *out_tags are freshly malloc'd
 * parallel arrays the caller owns (including each out_paths[i] string);
 * both are set to NULL and *out_count to 0 if the cache is empty or
 * unopened -- not an error, just nothing cached yet. */
void metadata_db_load_all(char *** out_paths, cached_tags_t ** out_tags, int * out_count);

/* Persistent book cache -- the "Books" row in gui.c's Books menu reads from
 * this instead of scanning anything itself, same "load whatever's cached,
 * don't touch the filesystem" boot behavior music's own library already
 * has (see library_load_from_cache_only() in gui.c). Replaced whenever the
 * user rescans, using only .txt files found below the dedicated Books
 * directory on the SD card. */
void metadata_db_book_replace_all(char * const * paths, int count);

/* Enumerates every cached book path, alphabetically -- caller-owned array,
 * same convention as metadata_db_load_all() and text_reader_scan_txt_
 * files(). *out_paths is NULL and *out_count is 0 if the cache is empty or
 * unopened. */
void metadata_db_load_all_books(char *** out_paths, int * out_count);

/* Per-book favorite flag, keyed by path -- toggled from the text reader
 * screen's own favorite icon. Persists across restarts and rescans (not
 * touched by metadata_db_book_replace_all(), which only rewrites the plain
 * book list). */
bool metadata_db_book_favorite_is_set(const char * path);
void metadata_db_book_favorite_set(const char * path, bool is_favorite);

/* Enumerates every favorited path that's also still in the current book
 * cache (a book removed by a rescan drops out of this even if its favorite
 * flag was never explicitly cleared), alphabetically -- same caller-owned
 * convention as metadata_db_load_all_books(). */
void metadata_db_load_favorite_books(char *** out_paths, int * out_count);

/* Persistent playlist (.m3u) cache -- same "load whatever's cached, don't
 * touch the filesystem" reasoning as the book cache above (the Playlists
 * screen in gui.c was slow to open, ~5s against a real SD card, because it
 * ran playlist_files_scan()'s live recursive readdir()+stat() walk on
 * every visit). Unlike books there's no fast stock-db path to warm-start
 * from (m3u playlists are this app's own concept, not something the stock
 * player ever indexes) -- populated only by gui.c's rescan_playlists()
 * (folded into library_scan_once(), same as rescan_books(), so it runs at
 * boot and on an explicit Settings > Update Music Database rescan, never
 * on the fast cache-only boot path) and by the single-row insert/delete
 * functions below for ordinary in-app playlist create/delete, so those
 * stay instant without a full rescan. */
void metadata_db_playlist_replace_all(char * const * paths, int count);

/* Enumerates every cached playlist path, alphabetically -- caller-owned
 * array, same convention as metadata_db_load_all_books(). */
void metadata_db_load_all_playlists(char *** out_paths, int * out_count);

/* Adds/removes a single path from the playlist cache -- for gui.c's own
 * playlist_files_create()/playlist_files_delete() call sites, so creating
 * or deleting one playlist doesn't pay for a full metadata_db_playlist_
 * replace_all() rescan just to reflect that one file. INSERT is a no-op if
 * the path's already cached (PRIMARY KEY conflict, ignored); DELETE is a
 * no-op if it wasn't. */
void metadata_db_playlist_insert_one(const char * path);
void metadata_db_playlist_delete_one(const char * path);

/* Per-song favorite flag, keyed by path -- toggled from the player screen's
 * heart icon (favorite_icon/favorite_icon_event_cb in gui.c). Persists
 * across restarts and rescans, same convention as the book favorite
 * functions above. */
bool metadata_db_song_favorite_is_set(const char * path);
void metadata_db_song_favorite_set(const char * path, bool is_favorite);

/* Enumerates every favorited path that's also still in the current media
 * cache, alphabetically -- same caller-owned, stale-entry-filtered
 * convention as metadata_db_load_favorite_books(). */
void metadata_db_load_favorite_songs(char *** out_paths, int * out_count);

/* Bumps path's play count by one (creating its row with count=1 if this is
 * the first time), and stamps last_played to the current time -- called
 * once per real "this track started playing" event (both an explicit pick
 * and a gapless auto-advance, see gui.c's apply_track_metadata_to_ui()),
 * never on every UI refresh of the same still-playing track. */
void metadata_db_song_play_count_increment(const char * path);

/* Enumerates up to `limit` paths with the highest play count (ties broken
 * by most-recently-played), filtered against the current media cache same
 * as metadata_db_load_favorite_songs() -- backs the "Most Played"
 * auto-generated playlist. Caller-owned array; *out_paths is NULL and
 * *out_count is 0 if there's no play history yet. */
void metadata_db_load_top_played_songs(int limit, char *** out_paths, int * out_count);

/* Saved Subsonic server profiles -- the "Saved Servers" list in gui.c's
 * Subsonic setup flow reads/writes this instead of the single-connection
 * settings.c fields (subsonic_url/username/password/verify_tls) this app
 * used to have room for only one of. Deliberately plain char* params
 * rather than subsonic_client.h's subsonic_server_t: this file doesn't
 * otherwise depend on that header, and these are the only pieces of it
 * ever needed here -- gui.c does its own struct marshaling on both sides.
 * url is the natural unique key (a real server only has one), so
 * metadata_db_subsonic_server_save() is an upsert: saving the same URL
 * again (e.g. reconnecting with updated credentials) replaces the
 * existing row rather than creating a duplicate. Password is stored in
 * plain text, same as settings.c's existing single-server field already
 * did -- no new exposure, this app has no secret-storage mechanism to
 * upgrade to. */
void metadata_db_subsonic_server_save(const char * url, const char * username, const char * password, bool verify_tls);

/* Enumerates every saved server, alphabetically by URL -- caller-owned
 * array of subsonic_server_row_t, same convention as
 * metadata_db_load_all_books(). *out_rows is NULL and *out_count is 0 if
 * there are none. */
typedef struct {
    char url[256];
    char username[128];
    char password[128];
    bool verify_tls;
} subsonic_server_row_t;
void metadata_db_load_subsonic_servers(subsonic_server_row_t ** out_rows, int * out_count);

#endif /* METADATA_DB_H */
