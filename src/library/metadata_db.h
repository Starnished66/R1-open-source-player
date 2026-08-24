#ifndef METADATA_DB_H
#define METADATA_DB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* On-disk cache of every scanned song's title/artist/album/album_artist/
 * genre tags, keyed by path + the file's mtime/size at the time it was
 * cached --
 * lets library_scan_once() (gui.c) skip re-reading tags (metadata_read(),
 * one open+parse per file) for every file that hasn't changed since the
 * last scan. Backed by a POSIX port of Rockbox's tagcache (database_idx.tcd
 * plus per-tag database_N.tcd files), not SQLite. Large libraries keep
 * unique tags in RAM and mmap title/path files instead of interning every
 * string. */

typedef struct {
    char title[128];
    char artist[128];
    char album[128];
    char album_artist[128];
    char genre[128];
} cached_tags_t;

/* Opens the on-disk tagcache at its fixed path (the SD-card root on
 * target, ./ on host). Safe to call more than once; a no-op if already
 * open. */
void metadata_db_open(void);
void metadata_db_close(void);

/* Starts a scan pass against the SD-resident tagcache. Unchanged files
 * are marked seen; new/changed files are upserted. metadata_db_end_update()
 * deletes unseen rows after a complete pass. */
void metadata_db_begin_update(void);

/* Looks up `path`, marking it as seen for this pass either way. Returns
 * true and fills *out only if a cached row exists whose stored mtime/size
 * match the values passed in -- callers should treat false as "the file is
 * new or has changed, re-read its real tags and call metadata_db_put()". */
bool metadata_db_get(const char * path, int64_t mtime, int64_t size, cached_tags_t * out);

/* Inserts or replaces the cached row for `path`. */
void metadata_db_put(const char * path, int64_t mtime, int64_t size, const cached_tags_t * tags);

/* Ends a successful scan pass: deletes rows whose scan generation was not
 * refreshed by this pass (files removed/renamed), then commits. Returns
 * false if the on-disk write failed; the last committed generation remains
 * loadable. */
bool metadata_db_end_update(void);

/* Ends an interrupted scan without committing. Reloads the last committed
 * database so in-RAM upserts from the aborted pass are discarded. */
void metadata_db_abort_update(void);

/* ---- Bounded, paged access to the tagcache -- Rockbox-style: the unique
 * tag files and master index are the lookup structure, and nothing beyond
 * the rows a caller actually asks for is copied out. Replaces the old
 * "load every song into one big array" boot-time model for the paged/
 * on-demand queries below -- metadata_db_load_all() itself is NOT unused,
 * though: gui.c's own ensure_library_arrays_loaded() still calls it as a
 * deliberate lazy-fallback full load (see that function's own doc comment
 * there, and metadata_db_load_all()'s own comment here, for why). `id` in
 * every row below is the song's stable tagcache slot id (1-based). Deleted
 * slots stay holes and are never reused, so a surviving song keeps its id
 * across rescans. ---- */

typedef struct {
    int64_t id;
    char path[600];
    cached_tags_t tags;
} song_row_t;

typedef struct {
    char name[128];
    int song_count;
    int64_t first_song_id; /* one representative song, e.g. for cover art -- not a full song_row_t, callers query that separately if needed */
    /* Only populated by metadata_db_get_albums_page_filtered() below (empty
     * string from metadata_db_get_groups_page(), whose ARTIST/ALBUM_ARTIST
     * kinds have no second identity component) -- see that function's own
     * comment on why an album's real identity is (album, album_artist), not
     * album name alone. */
    char album_artist[128];
} group_row_t;

int64_t metadata_db_get_song_count(void);

/* Distinct artist / album_artist / album counts -- for the group screens'
 * own paging math (total row count) without materializing the groups
 * themselves. */
void metadata_db_get_group_counts(int * out_artist_count, int * out_album_artist_count, int * out_album_count);

/* Keyset-paginated page of songs ordered by title. First page: pass
 * after_title = NULL, after_id = 0. Next page: pass the last row's own
 * title/id from the previous call. Returns the number of rows written into
 * out_rows (a caller-owned buffer of at least max_rows entries) -- less
 * than max_rows (including 0) means this was the last page. */
int metadata_db_get_songs_page(const char * after_title, int64_t after_id, int max_rows, song_row_t * out_rows);

/* Offset-paginated page of songs ordered by first_seen DESC (path
 * case-insensitive tiebreak) -- backs the Recently Added screen's compact_list_set_paged_
 * provider(), the same bounded-page-at-a-time shape metadata_db_get_songs_
 * filtered_page() gives All Songs, rather than loading the whole library
 * into RAM. Pair with metadata_db_get_song_recency_offset() below for a
 * "now playing" position in this same order. */
int metadata_db_get_songs_page_by_recency(int offset, int max_rows, song_row_t * out_rows);

/* Offset-paginated page of distinct values of the named column, ordered
 * alphabetically (ASCII case-insensitive) -- ARTIST/ALBUM_ARTIST kinds only in
 * practice (every real caller wants "all albums" grouped by the *pair*
 * (album, album_artist) instead, which is what metadata_db_get_albums_
 * page_filtered() below does -- ALBUM here would just group by album name
 * alone, merging different artists' same-titled albums, so nothing calls
 * it that way). Returns the number of rows written into out_rows (a
 * caller-owned buffer of at least max_rows entries) -- fewer than max_rows
 * (including 0) means this was the last page. */
typedef enum { METADATA_DB_GROUP_ARTIST, METADATA_DB_GROUP_ALBUM_ARTIST, METADATA_DB_GROUP_ALBUM } metadata_db_group_kind_t;
int metadata_db_get_groups_page(metadata_db_group_kind_t kind, int offset, int max_rows, group_row_t * out_rows);

/* Every song credited to one artist, across all their albums -- Artist ->
 * (their own songs) drill-down. Caller-owned out_rows buffer; returns the
 * number of rows written, capped at max_rows (if a real artist somehow has
 * more songs than max_rows, load_from_cache_only()'s own boot totals
 * already bound how large this can plausibly get -- see its own comment). */
int metadata_db_get_artist_songs(const char * artist, int offset, song_row_t * out_rows, int max_rows);

/* Every song in one album -- keyed by (album, album_artist) together, not
 * album name alone, so two different artists' same-titled albums ("Greatest
 * Hits", a self-titled album) don't collide. */
int metadata_db_get_album_songs(const char * album, const char * album_artist, int offset, song_row_t * out_rows,
                                 int max_rows);

bool metadata_db_get_song_by_id(int64_t id, song_row_t * out_row);
bool metadata_db_get_song_by_path(const char * path, song_row_t * out_row);

/* Batched equivalents of the two functions above. out_rows must have room
 * for `count` entries; out_rows[i].id is set to -1 for any ids[i]/paths[i]
 * that doesn't match a real song, so a caller that needs to skip missing
 * ones just checks that field -- every other field of a -1 entry is left
 * untouched (whatever out_rows[i] already held), not zeroed. */
void metadata_db_get_songs_by_ids(const int64_t * ids, int count, song_row_t * out_rows);
void metadata_db_get_songs_by_paths(const char * const * paths, int count, song_row_t * out_rows);
int64_t metadata_db_get_song_title_offset(const char * path);

/* Same role as metadata_db_get_song_title_offset() above, but for the
 * Recently Added screen's own order -- see metadata_db_get_songs_page_by_
 * recency()'s own comment for the exact ORDER BY this matches. */
int64_t metadata_db_get_song_recency_offset(const char * path);
int64_t metadata_db_get_group_offset(metadata_db_group_kind_t kind, const char * name, const char * album_artist);

/* 27-entry (A-Z, then '#') table of "display offset of the first entry at
 * or after this letter", matching gui.c's "jump forward to nearest match"
 * semantics. Built by walking the already-sorted in-memory group or title
 * index once -- out_table[i] for i in 0..25 is a letter's offset (-1 if
 * nothing sorts at/after it); out_table[26] is 0 if any row sorts before
 * 'A' (digits/symbols), else -1. kind selects which of the four top-level
 * library screens' own order to match: ARTIST/ALBUM_ARTIST/ALBUM as in
 * metadata_db_get_groups_page()/metadata_db_get_albums_page_filtered()
 * (offsets are into that grouped list), or METADATA_DB_AZ_ALL_SONGS for
 * the ungrouped All Songs screen's title-ordered list. */
typedef enum {
    METADATA_DB_AZ_ARTIST,
    METADATA_DB_AZ_ALBUM_ARTIST,
    METADATA_DB_AZ_ALBUM,
    METADATA_DB_AZ_ALL_SONGS
} metadata_db_az_kind_t;
void metadata_db_get_az_table(metadata_db_az_kind_t kind, int out_table[27]);

typedef struct {
    char label[128];
    int offset; /* position in the same unfiltered sequence metadata_db_get_groups_page()/
                 * get_albums_page_filtered()/get_songs_filtered_page() themselves page
                 * through for this kind -- a clicked hit means the same thing as an
                 * ordinary unfiltered row tap at that offset. */
} metadata_db_search_hit_t;

/* Case-insensitive substring search (matching gui.c's own former in-memory
 * search_matches() semantics -- plain substring, not a raw LIKE pattern, so
 * the needle's own '%'/'_'/'\' are escaped first) against the column each
 * screen's own paged display leads its ORDER BY with, capped at max_rows
 * results (same bounded-search-cache convention as metadata_db_search_
 * songs()'s own cap) and returned in that same alphabetical order. An empty
 * needle always returns 0 (gui.c's search shows nothing until you've
 * actually typed something, not the unfiltered list). METADATA_DB_AZ_ALL_
 * SONGS matches against the raw title column only, not the basename
 * fallback metadata_db_song_display_title() shows for an untagged file (no
 * embedded title) -- same accepted divergence class as metadata_db_get_az_
 * table()'s own Unicode-ordering caveat, not worth a second query pass for
 * what's normally a small minority of a library. */
int metadata_db_search_names(metadata_db_az_kind_t kind, const char * needle, int max_rows,
                              metadata_db_search_hit_t * out_hits);

/* row->tags.title verbatim, or the file's own basename (with extension) if
 * title is empty (no embedded title tag) -- matches gui.c's own
 * song_display_title_of() fallback for the local on-device list screens,
 * so every song_row_t consumer (remote_control.c, plugin.library_*) shows
 * the same thing for an untagged file instead of a blank title. */
void metadata_db_song_display_title(const song_row_t * row, char * out, size_t out_size);

/* Title/artist substring search (case-insensitive), ordered by title then
 * stable id -- capped at max_rows, always replace-not-append the caller's
 * previous result set (see this app's own bounded-search-cache convention). */
int metadata_db_search_songs(const char * query, song_row_t * out_rows, int max_rows);

/* remote_control.c's GET /api/library: query (title/artist substring) and
 * the three exact-match filters (artist/album_artist/album) are each
 * independently optional -- NULL or "" skips that condition. Offset-based
 * (not keyset) since a remote HTTP client passes an arbitrary offset it
 * doesn't control the shape of, same paging contract that endpoint already
 * had against remote_control.c's own now-removed in-memory copy. Query the
 * count first (metadata_db_count_songs_filtered()) for the response's own
 * "total" field, then the page. */
int64_t metadata_db_count_songs_filtered(const char * query, const char * artist_filter,
                                          const char * album_artist_filter, const char * album_filter);
int metadata_db_get_songs_filtered_page(const char * query, const char * artist_filter,
                                         const char * album_artist_filter, const char * album_filter, int offset,
                                         int max_rows, song_row_t * out_rows);

/* Albums grouped and filtered by artist OR album_artist matching one name
 * -- remote_control.c's GET /api/library/albums?artist=X, gui.c's own
 * Albums screen (unfiltered). filter may be NULL/"" for every album,
 * unfiltered. Real bug caught in review: an earlier version of this
 * grouped by album name alone, so two different artists' same-titled
 * albums ("Greatest Hits", a self-titled album, a "Various Artists"
 * compilation reissued under different album_artist tags) silently merged
 * into one row -- disagreeing with metadata_db_get_group_counts()'s own
 * album count (which already correctly counted distinct (album,
 * album_artist) pairs) and combining unrelated albums' song counts/cover
 * art. Groups by the pair now, matching get_group_counts(); out_rows[i].
 * album_artist carries the second half of that identity (empty from every
 * other group-returning function here, which have no such ambiguity) --
 * needed by metadata_db_get_album_songs() below to fetch the right
 * album's songs, not just a same-named one. Offset-paginated (not
 * keyset) -- no real caller here ever continued a keyset cursor anyway
 * (every existing call passed after_name/NULL, i.e. "just the first
 * page"), and offset is what a virtualized UI list actually needs
 * (arbitrary scroll-driven jumps, not just "next page"). */
int metadata_db_get_albums_page_filtered(const char * artist_or_album_artist_filter, int offset, int max_rows,
                                          group_row_t * out_rows);

/* Exact Artist/Album-Artist drill-down variants. Unlike the legacy remote
 * helper above, these filter only the requested tag column. */
int64_t metadata_db_count_albums_for_group(metadata_db_group_kind_t kind, const char * name);
int metadata_db_get_albums_for_group(metadata_db_group_kind_t kind, const char * name, int offset, int max_rows,
                                      group_row_t * out_rows);

/* Deliberate full-library load, for the handful of consumers not yet
 * converted to the paged/on-demand queries above (gui.c's own
 * ensure_library_arrays_loaded(), called lazily on first actual need --
 * never from the boot path). See metadata_db.c's own comment on this
 * function for why it still exists rather than being deleted outright.
 * *out_paths and *out_tags are freshly malloc'd parallel arrays the caller
 * owns (including each out_paths[i] string); both are set to NULL and
 * *out_count to 0 if the cache is empty or unopened. */
void metadata_db_load_all(char *** out_paths, cached_tags_t ** out_tags, int * out_count);

/* Per-song favorite flag, keyed by path. Local scan rows store this as the
 * Rockbox rating tag. Paths that are not tagcache rows (remote:// plugin
 * tracks, Subsonic stream URLs, unscanned files) persist in a sidecar so
 * the heart and play count accumulate across plays. Favorites / Most Played
 * screens still list only rows present in the local library. */
bool metadata_db_song_favorite_is_set(const char * path);
void metadata_db_song_favorite_set(const char * path, bool is_favorite);

/* Enumerates every favorited path that's also still in the current media
 * cache, alphabetically. */
void metadata_db_load_favorite_songs(char *** out_paths, int * out_count);

/* Bumps path's play count by one and stamps last_played. Local library
 * rows update tagcache; any other path creates/updates a sidecar row with
 * count=1 on first play. Called once per real "this track started playing"
 * event (explicit pick and gapless auto-advance, see gui.c's
 * apply_track_metadata_to_ui()), never on every UI refresh of the same
 * still-playing track. */
void metadata_db_song_play_count_increment(const char * path);

/* Enumerates up to `limit` paths with the highest play count (ties broken
 * by most-recently-played), filtered against the current media cache same
 * as metadata_db_load_favorite_songs() -- backs the "Most Played"
 * auto-generated playlist. Caller-owned array; *out_paths is NULL and
 * *out_count is 0 if there's no play history yet. */
void metadata_db_load_top_played_songs(int limit, char *** out_paths, int * out_count);

/* Enumerates up to `limit` paths, most-recently-added first (first_seen,
 * set only when a path is first inserted). A later retag bumps mtime but
 * must not resurface the song at the top of this list. Caller-owned array;
 * *out_paths is NULL and *out_count is 0 if the cache is empty or unopened. */
void metadata_db_load_recently_added_songs(int limit, char *** out_paths, int * out_count);

/* Books live in a path-list sidecar (not tagcache). Same caller-owned
 * array convention as the song enumerators above. */
bool metadata_db_book_favorite_is_set(const char * path);
void metadata_db_book_favorite_set(const char * path, bool is_favorite);
void metadata_db_book_replace_all(char * const * paths, int count);
void metadata_db_load_favorite_books(char *** out_paths, int * out_count);
void metadata_db_load_all_books(char *** out_paths, int * out_count);

/* Persistent playlist (.m3u) cache -- same "load whatever's cached, don't
 * touch the filesystem" reasoning as the book cache above (the Playlists
 * screen in gui.c was slow to open, ~5s against a real SD card, because it
 * ran playlist_files_scan() on every visit). Unlike books there's no fast
 * stock-db path to warm-start from (m3u playlists are this app's own
 * concept, not something the stock player ever indexes) -- populated only
 * by gui.c's rescan_playlists() (folded into library_scan_once(), same as
 * rescan_books(), so it runs at boot and on an explicit Settings > Update
 * Music Database rescan, never on the fast cache-only boot path) and by
 * the single-row insert/delete functions below for ordinary in-app
 * playlist create/delete, so those stay instant without a full rescan.
 * rescan_playlists() walks PLAYLISTS_DIR only, not the whole music tree. */
void metadata_db_playlist_replace_all(char * const * paths, int count);

/* Enumerates every cached playlist path, alphabetically -- caller-owned
 * array, same convention as metadata_db_load_all_books(). */
void metadata_db_load_all_playlists(char *** out_paths, int * out_count);

/* Adds/removes a single path from the playlist cache -- for gui.c's own
 * playlist_files_create()/playlist_files_delete() call sites, so creating
 * or deleting one playlist doesn't pay for a full metadata_db_playlist_
 * replace_all() rescan just to reflect that one file. INSERT is a no-op if
 * the path's already cached; DELETE is a no-op if it wasn't. */
void metadata_db_playlist_insert_one(const char * path);
void metadata_db_playlist_delete_one(const char * path);

/* Saved Subsonic server profiles -- the "Saved Servers" list in gui.c's
 * Subsonic setup flow. The active connection still lives in settings.c
 * (subsonic_url/username/password/verify_tls); this is the multi-server
 * list that used to be only those four fields. Deliberately plain char*
 * params rather than subsonic_client.h's subsonic_server_t: this file
 * doesn't otherwise depend on that header, and these are the only pieces
 * of it ever needed here -- gui.c does its own struct marshaling on both
 * sides. url is the natural unique key (a real server only has one), so
 * metadata_db_subsonic_server_save() is an upsert: saving the same URL
 * again (e.g. reconnecting with updated credentials) replaces the
 * existing row rather than creating a duplicate. Password is stored in
 * plain text, same as settings.c's existing single-server field already
 * did -- no new exposure, this app has no secret-storage mechanism to
 * upgrade to. Persisted on /usr/data (see subsonic_saved_servers.c). */
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
