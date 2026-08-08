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

/* Starts a scan pass: wraps every metadata_db_get()/put() until the
 * matching metadata_db_end_update() in one transaction (SQLite's default
 * autocommit mode would otherwise fsync once per row, turning a library
 * scan into one disk sync per file) and begins tracking which paths this
 * pass actually saw, for metadata_db_end_update()'s prune step. */
void metadata_db_begin_update(void);

/* Looks up `path`, marking it as seen for this pass either way. Returns
 * true and fills *out only if a cached row exists whose stored mtime/size
 * match the values passed in -- callers should treat false as "the file is
 * new or has changed, re-read its real tags and call metadata_db_put()". */
bool metadata_db_get(const char * path, int64_t mtime, int64_t size, cached_tags_t * out);

/* Inserts or replaces the cached row for `path`. */
void metadata_db_put(const char * path, int64_t mtime, int64_t size, const cached_tags_t * tags);

/* Ends the current scan pass: deletes every cached row whose path wasn't
 * seen via metadata_db_get() since the matching metadata_db_begin_update()
 * (i.e. files removed or renamed since the last scan) and commits. */
void metadata_db_end_update(void);

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

/* Fast .txt "book" listing sourced from the stock hiby_player's own
 * already-scanned BOOK_TABLE (same stock database as import_from_stock_
 * player_db()'s MEDIA_TABLE read, see metadata_db.c's own comment on where
 * that lives and why its paths need translating) -- a plain indexed SELECT
 * against a small table, instead of a live recursive readdir()+stat() walk
 * of the whole SD card every time the Books screen opens (confirmed the
 * real cause of that screen loading slowly). Each result is verified to
 * still exist (a cheap access() per row, not a directory walk) so a file
 * the user deleted since the stock scanner last ran doesn't show as a dead
 * entry. Returns true and fills *out_paths and *out_count (caller-owned, same
 * convention as text_reader_scan_txt_files(), sorted the same way) only if
 * the stock db was actually readable and had at least one still-valid
 * entry; false (leaving *out_paths untouched) means the caller should fall
 * back to its own live scan -- same tolerant-of-a-missing/corrupt/slow
 * stock db behavior as import_from_stock_player_db(). Always false on
 * HOST_BUILD (no stock db concept there). */
bool metadata_db_list_books_from_stock(char *** out_paths, int * out_count);

/* Persistent book cache -- the "Books" row in gui.c's Books menu reads from
 * this instead of scanning anything itself, same "load whatever's cached,
 * don't touch the filesystem" boot behavior music's own library already
 * has (see library_load_from_cache_only() in gui.c). Populated two ways:
 * a fast warm-start from the stock db (metadata_db_open() itself, when this
 * table is still empty -- same shape as media's own warm start) and an
 * explicit full replace whenever the user rescans (Settings > Update Music
 * Database, see gui.c's rescan_books() -- folds in both the fast stock-db
 * path and, if that finds nothing, a real live filesystem walk, so a
 * rescan can find books the stock scanner never indexed). */
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
