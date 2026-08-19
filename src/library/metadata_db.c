#include "metadata_db.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifdef HOST_BUILD
  #define METADATA_DB_PATH "./open_hiby_player_music.db"
#else
  /* Real-device bug report: a library of 5000+ songs rebooted the device
   * mid-rescan. Root cause: this lived on /usr/data, a 35.8MB ubifs
   * partition already close to full with settings/logs/other app state (see
   * settings.c's own SETTINGS_FILE_PATH comment) -- metadata_db_begin_update()
   * wraps the ENTIRE rescan in one SQLite transaction, so the rollback
   * journal alone (roughly proportional to how many rows a big library
   * touches) stays open and grows for the whole scan, on top of the actual
   * database file itself also growing with new rows. A large enough library
   * could exhaust that partition mid-scan; what a failed write does to this
   * particular ubifs from there (not something this app can recover from
   * gracefully either way) is consistent with the reported reboot. Moved to
   * the SD card instead -- same MUSIC_ROOT_DIR the library itself lives on,
   * so there's no plausible library size this device could hold that
   * wouldn't also fit a cache of that library's own tags many times over.
   * Safe to depend on the SD card being mounted here: metadata_db_open() is
   * only ever called from gui_init() (see its own doc comment), which always
   * runs after main()'s mount_sd_card_if_needed(). If the card genuinely
   * isn't mounted, sqlite3_open() below just fails like any other open
   * failure -- metadata_db_open() already treats that as a degraded-but-
   * working no-cache state, not a fatal error, so this doesn't introduce a
   * new failure mode on top of the existing SD-mount-failure handling
   * (see main.c's own mount-retry/format-prompt logic). Dot-prefixed, same
   * pattern as dlna_control.c's own .dlna_cast, so it doesn't clutter a
   * plain directory listing of the card. */
  #define METADATA_DB_PATH "/data/mnt/sd_0/.open_hiby_player_music.db"
#endif

/* The stock (closed-source) hiby_player's own on-disk library cache --
 * confirmed against a real R1: /data/mnt/sd_0/usrlocal_media.db, i.e. the
 * SD card's own root (same directory MUSIC_ROOT_DIR in gui.c points at).
 * Its MEDIA_TABLE schema (recovered via sqlite3's `.recover` against a real
 * card -- the live file couldn't be opened directly, "database disk image
 * is malformed", seemingly from being copied off while still mounted)
 * stores paths in its own VFS's drive-letter form, e.g.
 * 'a:\Dream Theater\...\file.flac' -- 'a:' is that VFS's name for this same
 * SD card, so translate_stock_path() below strips it and swaps back to
 * forward slashes to get a real path under MUSIC_ROOT_DIR. Only 'a:' is
 * handled (imports skip any other drive letter) since that's the only one
 * confirmed to mean anything here. */
#ifndef HOST_BUILD
  #define STOCK_MEDIA_DB_PATH "/data/mnt/sd_0/usrlocal_media.db"
  #define STOCK_MEDIA_ROOT "/data/mnt/sd_0"
#endif

static sqlite3 * db = NULL;
static sqlite3_stmt * stmt_get = NULL;
static sqlite3_stmt * stmt_put = NULL;
static sqlite3_stmt * stmt_mark_seen = NULL;
static int64_t update_generation = 0;
static int update_ops_since_commit = 0;
#define METADATA_DB_UPDATE_BATCH 128

/* NULL is a valid, harmless argument to sqlite3_finalize()/sqlite3_close()
 * -- used freely below instead of guarding every call. */

static void prepare_statements(void) {
    sqlite3_prepare_v2(db,
        "SELECT mtime, size, title, artist, album, album_artist, genre FROM media WHERE path = ?;",
        -1, &stmt_get, NULL);
    sqlite3_prepare_v2(db,
        "INSERT INTO media (path, mtime, size, title, artist, album, album_artist, genre, scan_generation) VALUES (?,?,?,?,?,?,?,?,?) "
        "ON CONFLICT(path) DO UPDATE SET mtime=excluded.mtime, size=excluded.size, title=excluded.title, "
        "artist=excluded.artist, album=excluded.album, album_artist=excluded.album_artist, genre=excluded.genre, "
        "scan_generation=excluded.scan_generation;",
        -1, &stmt_put, NULL);
    sqlite3_prepare_v2(db, "UPDATE media SET scan_generation=? WHERE path=?;", -1, &stmt_mark_seen, NULL);
}

#ifndef HOST_BUILD
/* raw_path must be in the stock player's "a:\...\name.ext" form. Returns
 * false (and leaves *out untouched) for any other drive letter -- there's
 * nothing else to map those to. */
static bool translate_stock_path(const char * raw_path, char * out, size_t out_size) {
    if (!raw_path) return false;
    if ((raw_path[0] != 'a' && raw_path[0] != 'A') || raw_path[1] != ':' || raw_path[2] != '\\') return false;

    snprintf(out, out_size, "%s/%s", STOCK_MEDIA_ROOT, raw_path + 3);
    for (char * p = out; *p; p++) {
        if (*p == '\\') *p = '/';
    }
    return true;
}

/* Real-device incident (same failure class already documented and guarded
 * against for the file-walk scan in gui.c's scan_all_songs_with_timeout()):
 * a stuck read on a corrupted SD card block lands the calling thread in an
 * uninterruptible (D-state) kernel wait that no signal/alarm()/
 * pthread_cancel() can break. This function's stock_db reads sit on that
 * same SD card (STOCK_MEDIA_DB_PATH), and this was called synchronously
 * from metadata_db_open() -- itself called synchronously from gui_init(),
 * before the first frame renders -- with no timeout at all, unlike the
 * file-walk scan. Worse than a plain hang if fixed the naive way: an
 * earlier version of this fix ran the whole read-and-write sequence on a
 * background thread but kept writing into the shared `db` handle from that
 * thread mid-loop, so an abandoned-on-timeout worker stuck in D-state could
 * still be holding `db`'s own internal serialization mutex (SQLite's
 * default "Serialized" threading mode) -- the main thread's own later
 * writes to `db` would then block waiting on that same mutex, silently
 * reintroducing the identical indefinite hang one level removed, and
 * sqlite3_busy_timeout() wouldn't help since that only governs
 * file-locking retries, not this in-process mutex wait. Fixed by keeping
 * `db` completely untouched by the worker thread: it only ever reads from
 * `stock_db` (its own separate connection) into a plain heap buffer, and
 * only the main thread -- after the worker signals success within the
 * timeout -- ever writes into `db`. On timeout, the buffer and thread are
 * deliberately leaked (never joined/freed) rather than risking a
 * use-after-free against a worker that might still write to them, same
 * accepted tradeoff scan_all_songs_with_timeout's own comment describes. */
#define STOCK_DB_IMPORT_TIMEOUT_MS 5000

typedef struct {
    char path[600];
    int64_t mtime;
    int64_t size;
    cached_tags_t tags;
} stock_import_row_t;

typedef struct {
    volatile bool done;
    stock_import_row_t * rows;
    int count;
    int capacity;
} stock_import_work_t;

static void * import_from_stock_player_db_worker(void * arg) {
    stock_import_work_t * w = (stock_import_work_t *) arg;

    sqlite3 * stock_db = NULL;
    if (sqlite3_open_v2(STOCK_MEDIA_DB_PATH, &stock_db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        sqlite3_close(stock_db);
        w->done = true; /* written last -- the only field polled by the caller */
        return NULL;
    }

    sqlite3_stmt * st = NULL;
    /* "name" is MEDIA_TABLE's song-title column (confirmed via `.schema
     * MEDIA_TABLE` against a real device's copy, and by sampling rows --
     * e.g. name="Perfect Strangers" for a file whose path's basename is
     * "03 Perfect Strangers.flac"), imported as our own title field. */
    if (sqlite3_prepare_v2(stock_db, "SELECT path, name, artist, album, album_artist, genre, mtime, size FROM MEDIA_TABLE;",
                            -1, &st, NULL) != SQLITE_OK) {
        sqlite3_close(stock_db);
        w->done = true;
        return NULL;
    }

    w->capacity = 256;
    w->rows = malloc(sizeof(stock_import_row_t) * (size_t) w->capacity);
    w->count = 0;

    while (sqlite3_step(st) == SQLITE_ROW) {
        const char * raw_path = (const char *) sqlite3_column_text(st, 0);
        char path[600];
        if (!translate_stock_path(raw_path, path, sizeof(path))) continue;

        if (w->count >= w->capacity) {
            w->capacity *= 2;
            w->rows = realloc(w->rows, sizeof(stock_import_row_t) * (size_t) w->capacity);
        }

        stock_import_row_t * row = &w->rows[w->count];
        snprintf(row->path, sizeof(row->path), "%s", path);
        row->mtime = sqlite3_column_int64(st, 6);
        row->size = sqlite3_column_int64(st, 7);
        snprintf(row->tags.title, sizeof(row->tags.title), "%s", (const char *) sqlite3_column_text(st, 1));
        snprintf(row->tags.artist, sizeof(row->tags.artist), "%s", (const char *) sqlite3_column_text(st, 2));
        snprintf(row->tags.album, sizeof(row->tags.album), "%s", (const char *) sqlite3_column_text(st, 3));
        snprintf(row->tags.album_artist, sizeof(row->tags.album_artist), "%s", (const char *) sqlite3_column_text(st, 4));
        snprintf(row->tags.genre, sizeof(row->tags.genre), "%s", (const char *) sqlite3_column_text(st, 5));
        w->count++;
    }

    sqlite3_finalize(st);
    sqlite3_close(stock_db);
    w->done = true; /* written last */
    return NULL;
}

/* One-time warm start for a fresh install: seeds our cache from the stock
 * player's own already-scanned library, so the very first library_scan_once()
 * on a device that already had music cataloged doesn't have to re-read
 * every file's tags from scratch. Only called when our own cache is still
 * empty (see metadata_db_open()) -- once we have our own scan results,
 * they're always a strict superset of anything worth re-importing here.
 * Tolerates a missing, corrupted, or slow-to-the-point-of-suspect stock db
 * by simply importing nothing, same as if this function didn't run at all. */
static void import_from_stock_player_db(void) {
    stock_import_work_t * w = calloc(1, sizeof(*w));
    if (!w) return;

    pthread_t thread;
    if (pthread_create(&thread, NULL, import_from_stock_player_db_worker, w) != 0) {
        free(w);
        return;
    }
    pthread_detach(thread); /* never joined either way -- see block comment above */

    for (int waited_ms = 0; waited_ms < STOCK_DB_IMPORT_TIMEOUT_MS; waited_ms += 20) {
        if (w->done) break;
        usleep(20000);
    }

    if (!w->done) {
        fprintf(stderr,
                "Warning: timed out reading stock player database (possible SD card issue) -- skipping warm-start import\n");
        return; /* w and its rows deliberately leaked -- see block comment above */
    }

    if (w->count > 0) {
        sqlite3_exec(db, "BEGIN;", NULL, NULL, NULL);
        for (int i = 0; i < w->count; i++) {
            metadata_db_put(w->rows[i].path, w->rows[i].mtime, w->rows[i].size, &w->rows[i].tags);
        }
        sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
    }
    free(w->rows);
    free(w);
}
#endif

#ifndef HOST_BUILD
/* One-time migration for anyone updating from before this file moved to the
 * SD card (see METADATA_DB_PATH's own comment) -- without this, a device
 * with an existing database at the old path would silently appear to lose
 * every song_favorite/song_play_count/playlist/subsonic_server row (real
 * user data, not just the rebuildable tag cache) the next time the app
 * opens, since it would just create a fresh empty database at the new path
 * instead. Plain read/copy/write rather than rename() -- the old and new
 * paths are on different filesystems (ubifs vs. the SD card's vfat), and
 * rename(2) can't cross that boundary (EXDEV). Only removes the old copy
 * once every byte of the new one has been written successfully, so a
 * failure partway through (e.g. the SD card filling up) leaves the original
 * still in place to retry from, rather than losing both. */
#define OLD_METADATA_DB_PATH "/usr/data/open_hiby_player_music.db"
static void migrate_old_db_if_needed(void) {
    if (access(METADATA_DB_PATH, F_OK) == 0) return; /* already migrated (or fresh install) */
    if (access(OLD_METADATA_DB_PATH, F_OK) != 0) return; /* nothing to migrate */

    FILE * in = fopen(OLD_METADATA_DB_PATH, "rb");
    if (!in) return;
    FILE * out = fopen(METADATA_DB_PATH, "wb");
    if (!out) {
        fclose(in);
        return;
    }

    bool ok = true;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            ok = false;
            break;
        }
    }
    ok = ok && !ferror(in);
    fclose(in);
    fclose(out);

    if (ok) {
        remove(OLD_METADATA_DB_PATH);
    } else {
        remove(METADATA_DB_PATH); /* don't leave a truncated/partial db at the new path */
    }
}
#endif

#ifndef HOST_BUILD
/* True if MUSIC_ROOT_DIR (gui.c's own name for this same path) is
 * currently a real mount point -- same "compare st_dev against the parent
 * directory's" check gui.c's own sd_card_root_is_mounted() uses,
 * duplicated here rather than shared (same "each file that needs it
 * defines its own copy" convention as MUSIC_ROOT_DIR/METADATA_DB_PATH
 * themselves, see this file's own comments above).
 *
 * Real-device bug found while testing SD-hotplug reinsertion: sqlite3_open()
 * happily creates a brand-new, empty database file at whatever's sitting
 * at METADATA_DB_PATH right now, even if that's just this mount point's
 * own directory living directly on the internal rootfs because nothing's
 * actually mounted there at this instant. gui.c's own SD-hotplug removal
 * handling (poll_sd_card_hotplug()) calls this on a genuinely unmounted
 * MUSIC_ROOT_DIR by design (to collapse the in-memory library to empty),
 * racing against its own auto-remount attempt on a separate thread -- if
 * this function's sqlite3_open() below ran during that brief window
 * before the real card was back, it created exactly that kind of bogus
 * internal-storage stand-in file. The instant the real card remounts, that
 * bogus file gets silently hidden (mounted over, not replaced) by the SD
 * card's own real cache file underneath -- but this module's own `db`
 * handle keeps pointing at the now-hidden bogus file's already-open fd
 * forever after (the `if (db) return;` guard just below never revisits
 * it), so every later read/write silently misses the real, populated
 * cache sitting right there on the card. Confirmed exactly this failure
 * mode on a real device: a reinsertion's cache-only reload kept coming
 * back with 0 songs despite a real, populated database on the card. */
static bool music_root_is_mounted(void) {
    struct stat parent_st, root_st;
    if (stat("/data/mnt", &parent_st) != 0) return false;
    if (stat("/data/mnt/sd_0", &root_st) != 0) return false;
    return parent_st.st_dev != root_st.st_dev;
}
#endif

void metadata_db_open(void) {
    if (db) return;

#ifndef HOST_BUILD
    /* Degraded-but-working no-cache state, same as an open()/sqlite3_open()
     * failure just below -- see music_root_is_mounted()'s own comment for
     * why this guard exists at all. */
    if (!music_root_is_mounted()) return;
    migrate_old_db_if_needed();
#endif

    if (sqlite3_open(METADATA_DB_PATH, &db) != SQLITE_OK) {
        /* No cache is a degraded-but-working state (every scan just falls
         * back to re-reading every file's tags, same as before this cache
         * existed) -- not worth crashing the whole app over. */
        sqlite3_close(db);
        db = NULL;
        return;
    }

    sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS media ("
        "  path TEXT PRIMARY KEY,"
        "  mtime INTEGER NOT NULL,"
        "  size INTEGER NOT NULL,"
        "  title TEXT NOT NULL DEFAULT '',"
        "  artist TEXT NOT NULL,"
        "  album TEXT NOT NULL,"
        "  album_artist TEXT NOT NULL,"
        "  genre TEXT NOT NULL,"
        "  scan_generation INTEGER NOT NULL DEFAULT 0"
        ");",
        NULL, NULL, NULL);
    /* Migration for a database created before the title column existed --
     * CREATE TABLE IF NOT EXISTS above is a no-op against an already-existing
     * table, so a pre-existing on-device media.db needs this separately.
     * Fails harmlessly (ignored return value) on a fresh table that already
     * has the column from the CREATE TABLE above -- SQLite has no ADD COLUMN
     * IF NOT EXISTS, so "try it and ignore duplicate-column errors" is the
     * standard way to make this idempotent. */
    sqlite3_exec(db, "ALTER TABLE media ADD COLUMN title TEXT NOT NULL DEFAULT '';", NULL, NULL, NULL);
    sqlite3_exec(db, "ALTER TABLE media ADD COLUMN scan_generation INTEGER NOT NULL DEFAULT 0;", NULL, NULL, NULL);

    sqlite3_exec(db, "CREATE TABLE IF NOT EXISTS book (path TEXT PRIMARY KEY);", NULL, NULL, NULL);
    sqlite3_exec(db, "CREATE TABLE IF NOT EXISTS book_favorite (path TEXT PRIMARY KEY);", NULL, NULL, NULL);
    sqlite3_exec(db, "CREATE TABLE IF NOT EXISTS playlist (path TEXT PRIMARY KEY);", NULL, NULL, NULL);
    sqlite3_exec(db,
                 "CREATE TABLE IF NOT EXISTS subsonic_server (url TEXT PRIMARY KEY, username TEXT, password TEXT, verify_tls INTEGER);",
                 NULL, NULL, NULL);
    /* Song favorites (player-screen heart icon) and play counts (Most
     * Played auto-playlist) -- same "join against media to drop stale
     * entries" shape as book_favorite, see metadata_db_load_favorite_songs()/
     * metadata_db_load_top_played_songs() below. Separate table rather than
     * columns on media itself: media rows are wholesale REPLACEd by
     * metadata_db_put() on every rescan (see prepare_statements()'s own
     * ON CONFLICT DO UPDATE, which only touches the tag columns it lists),
     * so anything living directly on that row would risk being clobbered by
     * a future schema change there -- keeping these in their own
     * path-keyed tables, same as book_favorite, avoids that coupling
     * entirely. */
    sqlite3_exec(db, "CREATE TABLE IF NOT EXISTS song_favorite (path TEXT PRIMARY KEY);", NULL, NULL, NULL);
    sqlite3_exec(db,
                 "CREATE TABLE IF NOT EXISTS song_play_count ("
                 "  path TEXT PRIMARY KEY,"
                 "  count INTEGER NOT NULL DEFAULT 0,"
                 "  last_played INTEGER NOT NULL DEFAULT 0"
                 ");",
                 NULL, NULL, NULL);

    prepare_statements();

#ifndef HOST_BUILD
    sqlite3_stmt * count_stmt = NULL;
    long long row_count = -1;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM media;", -1, &count_stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(count_stmt) == SQLITE_ROW) row_count = sqlite3_column_int64(count_stmt, 0);
    }
    sqlite3_finalize(count_stmt);
    if (row_count == 0) import_from_stock_player_db();

#endif
}

void metadata_db_close(void) {
    sqlite3_finalize(stmt_get);
    sqlite3_finalize(stmt_put);
    sqlite3_finalize(stmt_mark_seen);
    stmt_get = NULL;
    stmt_put = NULL;
    stmt_mark_seen = NULL;
    sqlite3_close(db);
    db = NULL;
}

static void metadata_db_update_checkpoint_if_needed(void) {
    if (!db || update_generation == 0) return;
    if (++update_ops_since_commit < METADATA_DB_UPDATE_BATCH) return;

    /* Bound rollback-journal/WAL growth. Scan-generation marking makes a
     * partially checkpointed scan safe: stale rows are not pruned until
     * metadata_db_end_update(), so a crash or abandoned scan merely leaves
     * a mixture of old/new generations that the next full pass supersedes. */
    sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
    sqlite3_exec(db, "BEGIN;", NULL, NULL, NULL);
    update_ops_since_commit = 0;
}

void metadata_db_begin_update(void) {
    if (!db) return;

    sqlite3_stmt * st = NULL;
    int64_t max_generation = 0;
    if (sqlite3_prepare_v2(db, "SELECT COALESCE(MAX(scan_generation), 0) FROM media;", -1, &st, NULL) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW) {
        max_generation = sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
    update_generation = max_generation + 1;
    if (update_generation <= 0) update_generation = 1; /* overflow/corruption guard */
    update_ops_since_commit = 0;
    sqlite3_exec(db, "BEGIN;", NULL, NULL, NULL);
}

bool metadata_db_get(const char * path, int64_t mtime, int64_t size, cached_tags_t * out) {
    if (!db) return false;

    /* Mark as observed without building an O(N) TEMP seen_paths table. */
    if (update_generation != 0) {
        sqlite3_reset(stmt_mark_seen);
        sqlite3_clear_bindings(stmt_mark_seen);
        sqlite3_bind_int64(stmt_mark_seen, 1, update_generation);
        sqlite3_bind_text(stmt_mark_seen, 2, path, -1, SQLITE_STATIC);
        sqlite3_step(stmt_mark_seen);
        metadata_db_update_checkpoint_if_needed();
    }

    sqlite3_reset(stmt_get);
    sqlite3_clear_bindings(stmt_get);
    sqlite3_bind_text(stmt_get, 1, path, -1, SQLITE_STATIC);
    if (sqlite3_step(stmt_get) != SQLITE_ROW) return false;

    int64_t cached_mtime = sqlite3_column_int64(stmt_get, 0);
    int64_t cached_size = sqlite3_column_int64(stmt_get, 1);
    if (cached_mtime != mtime || cached_size != size) return false;

    snprintf(out->title, sizeof(out->title), "%s", (const char *) sqlite3_column_text(stmt_get, 2));
    snprintf(out->artist, sizeof(out->artist), "%s", (const char *) sqlite3_column_text(stmt_get, 3));
    snprintf(out->album, sizeof(out->album), "%s", (const char *) sqlite3_column_text(stmt_get, 4));
    snprintf(out->album_artist, sizeof(out->album_artist), "%s", (const char *) sqlite3_column_text(stmt_get, 5));
    snprintf(out->genre, sizeof(out->genre), "%s", (const char *) sqlite3_column_text(stmt_get, 6));
    return true;
}

void metadata_db_put(const char * path, int64_t mtime, int64_t size, const cached_tags_t * tags) {
    if (!db) return;

    sqlite3_reset(stmt_put);
    sqlite3_clear_bindings(stmt_put);
    sqlite3_bind_text(stmt_put, 1, path, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt_put, 2, mtime);
    sqlite3_bind_int64(stmt_put, 3, size);
    sqlite3_bind_text(stmt_put, 4, tags->title, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt_put, 5, tags->artist, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt_put, 6, tags->album, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt_put, 7, tags->album_artist, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt_put, 8, tags->genre, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt_put, 9, update_generation);
    sqlite3_step(stmt_put);
    metadata_db_update_checkpoint_if_needed();
}

void metadata_db_end_update(void) {
    if (!db || update_generation == 0) return;

    /* Deletion detection is now an indexed generation comparison rather than
     * NOT IN against an in-memory TEMP table containing every scanned path. */
    sqlite3_stmt * st = NULL;
    if (sqlite3_prepare_v2(db, "DELETE FROM media WHERE scan_generation <> ?;", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, update_generation);
        sqlite3_step(st);
    }
    sqlite3_finalize(st);
    sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
    update_generation = 0;
    update_ops_since_commit = 0;
}

void metadata_db_abort_update(void) {
    if (!db || update_generation == 0) return;
    sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
    /* Do not prune anything: rows already marked with this generation are
     * harmless. The next complete scan chooses a larger generation. */
    update_generation = 0;
    update_ops_since_commit = 0;
}

void metadata_db_load_all(char *** out_paths, cached_tags_t ** out_tags, int * out_count) {
    *out_paths = NULL;
    *out_tags = NULL;
    *out_count = 0;
    if (!db) return;

    sqlite3_stmt * st = NULL;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM media;", -1, &st, NULL) != SQLITE_OK) return;
    int row_count = 0;
    if (sqlite3_step(st) == SQLITE_ROW) row_count = (int) sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    if (row_count <= 0) return;

    char ** paths = malloc(sizeof(char *) * (size_t) row_count);
    cached_tags_t * tags = malloc(sizeof(cached_tags_t) * (size_t) row_count);

    if (sqlite3_prepare_v2(db, "SELECT path, title, artist, album, album_artist, genre FROM media;", -1, &st, NULL) !=
        SQLITE_OK) {
        free(paths);
        free(tags);
        return;
    }

    int i = 0;
    while (i < row_count && sqlite3_step(st) == SQLITE_ROW) {
        paths[i] = strdup((const char *) sqlite3_column_text(st, 0));
        snprintf(tags[i].title, sizeof(tags[i].title), "%s", (const char *) sqlite3_column_text(st, 1));
        snprintf(tags[i].artist, sizeof(tags[i].artist), "%s", (const char *) sqlite3_column_text(st, 2));
        snprintf(tags[i].album, sizeof(tags[i].album), "%s", (const char *) sqlite3_column_text(st, 3));
        snprintf(tags[i].album_artist, sizeof(tags[i].album_artist), "%s", (const char *) sqlite3_column_text(st, 4));
        snprintf(tags[i].genre, sizeof(tags[i].genre), "%s", (const char *) sqlite3_column_text(st, 5));
        i++;
    }
    sqlite3_finalize(st);

    *out_paths = paths;
    *out_tags = tags;
    *out_count = i;
}

void metadata_db_book_replace_all(char * const * paths, int count) {
    if (!db) return;

    sqlite3_exec(db, "BEGIN;", NULL, NULL, NULL);
    sqlite3_exec(db, "DELETE FROM book;", NULL, NULL, NULL);

    sqlite3_stmt * st = NULL;
    if (sqlite3_prepare_v2(db, "INSERT INTO book (path) VALUES (?);", -1, &st, NULL) == SQLITE_OK) {
        for (int i = 0; i < count; i++) {
            sqlite3_reset(st);
            sqlite3_bind_text(st, 1, paths[i], -1, SQLITE_STATIC);
            sqlite3_step(st);
        }
        sqlite3_finalize(st);
    }

    /* Favorites for a path that dropped out of this replace (file deleted/
     * moved) are deliberately left alone here -- metadata_db_load_favorite_
     * books() below already filters against the current book table, so a
     * stale favorite flag just silently stops showing up rather than
     * needing an explicit prune; if the same path reappears in a later
     * rescan (e.g. a moved-then-restored file, or the same name reused),
     * its favorite status comes back too, which matches user expectation
     * better than losing it permanently over a transient absence. */
    sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
}

void metadata_db_load_all_books(char *** out_paths, int * out_count) {
    *out_paths = NULL;
    *out_count = 0;
    if (!db) return;

    sqlite3_stmt * st = NULL;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM book;", -1, &st, NULL) != SQLITE_OK) return;
    int row_count = 0;
    if (sqlite3_step(st) == SQLITE_ROW) row_count = (int) sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    if (row_count <= 0) return;

    char ** paths = malloc(sizeof(char *) * (size_t) row_count);
    if (sqlite3_prepare_v2(db, "SELECT path FROM book ORDER BY path COLLATE NOCASE;", -1, &st, NULL) != SQLITE_OK) {
        free(paths);
        return;
    }

    int i = 0;
    while (i < row_count && sqlite3_step(st) == SQLITE_ROW) {
        paths[i] = strdup((const char *) sqlite3_column_text(st, 0));
        i++;
    }
    sqlite3_finalize(st);

    *out_paths = paths;
    *out_count = i;
}

void metadata_db_playlist_replace_all(char * const * paths, int count) {
    if (!db) return;

    sqlite3_exec(db, "BEGIN;", NULL, NULL, NULL);
    sqlite3_exec(db, "DELETE FROM playlist;", NULL, NULL, NULL);

    sqlite3_stmt * st = NULL;
    if (sqlite3_prepare_v2(db, "INSERT INTO playlist (path) VALUES (?);", -1, &st, NULL) == SQLITE_OK) {
        for (int i = 0; i < count; i++) {
            sqlite3_reset(st);
            sqlite3_bind_text(st, 1, paths[i], -1, SQLITE_STATIC);
            sqlite3_step(st);
        }
        sqlite3_finalize(st);
    }

    sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
}

void metadata_db_load_all_playlists(char *** out_paths, int * out_count) {
    *out_paths = NULL;
    *out_count = 0;
    if (!db) return;

    sqlite3_stmt * st = NULL;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM playlist;", -1, &st, NULL) != SQLITE_OK) return;
    int row_count = 0;
    if (sqlite3_step(st) == SQLITE_ROW) row_count = (int) sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    if (row_count <= 0) return;

    char ** paths = malloc(sizeof(char *) * (size_t) row_count);
    if (sqlite3_prepare_v2(db, "SELECT path FROM playlist ORDER BY path COLLATE NOCASE;", -1, &st, NULL) != SQLITE_OK) {
        free(paths);
        return;
    }

    int i = 0;
    while (i < row_count && sqlite3_step(st) == SQLITE_ROW) {
        paths[i] = strdup((const char *) sqlite3_column_text(st, 0));
        i++;
    }
    sqlite3_finalize(st);

    *out_paths = paths;
    *out_count = i;
}

void metadata_db_playlist_insert_one(const char * path) {
    if (!db) return;

    sqlite3_stmt * st = NULL;
    if (sqlite3_prepare_v2(db, "INSERT INTO playlist (path) VALUES (?) ON CONFLICT(path) DO NOTHING;", -1, &st, NULL) !=
        SQLITE_OK) {
        return;
    }
    sqlite3_bind_text(st, 1, path, -1, SQLITE_STATIC);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

void metadata_db_playlist_delete_one(const char * path) {
    if (!db) return;

    sqlite3_stmt * st = NULL;
    if (sqlite3_prepare_v2(db, "DELETE FROM playlist WHERE path = ?;", -1, &st, NULL) != SQLITE_OK) return;
    sqlite3_bind_text(st, 1, path, -1, SQLITE_STATIC);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

bool metadata_db_book_favorite_is_set(const char * path) {
    if (!db) return false;

    sqlite3_stmt * st = NULL;
    if (sqlite3_prepare_v2(db, "SELECT 1 FROM book_favorite WHERE path = ?;", -1, &st, NULL) != SQLITE_OK) return false;
    sqlite3_bind_text(st, 1, path, -1, SQLITE_STATIC);
    bool is_set = sqlite3_step(st) == SQLITE_ROW;
    sqlite3_finalize(st);
    return is_set;
}

void metadata_db_book_favorite_set(const char * path, bool is_favorite) {
    if (!db) return;

    sqlite3_stmt * st = NULL;
    if (is_favorite) {
        if (sqlite3_prepare_v2(db, "INSERT INTO book_favorite (path) VALUES (?) ON CONFLICT(path) DO NOTHING;", -1, &st,
                                NULL) != SQLITE_OK) {
            return;
        }
    } else {
        if (sqlite3_prepare_v2(db, "DELETE FROM book_favorite WHERE path = ?;", -1, &st, NULL) != SQLITE_OK) return;
    }
    sqlite3_bind_text(st, 1, path, -1, SQLITE_STATIC);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

void metadata_db_load_favorite_books(char *** out_paths, int * out_count) {
    *out_paths = NULL;
    *out_count = 0;
    if (!db) return;

    sqlite3_stmt * st = NULL;
    /* INNER JOIN against book, not a plain SELECT from book_favorite --
     * see metadata_db_book_replace_all()'s own comment on why a stale
     * favorite (path no longer in the current book cache) is filtered out
     * here rather than pruned at rescan time. */
    static const char * const query =
        "SELECT book_favorite.path FROM book_favorite "
        "INNER JOIN book ON book.path = book_favorite.path "
        "ORDER BY book_favorite.path COLLATE NOCASE;";
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM book_favorite INNER JOIN book ON book.path = book_favorite.path;",
                            -1, &st, NULL) != SQLITE_OK) {
        return;
    }
    int row_count = 0;
    if (sqlite3_step(st) == SQLITE_ROW) row_count = (int) sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    if (row_count <= 0) return;

    char ** paths = malloc(sizeof(char *) * (size_t) row_count);
    if (sqlite3_prepare_v2(db, query, -1, &st, NULL) != SQLITE_OK) {
        free(paths);
        return;
    }

    int i = 0;
    while (i < row_count && sqlite3_step(st) == SQLITE_ROW) {
        paths[i] = strdup((const char *) sqlite3_column_text(st, 0));
        i++;
    }
    sqlite3_finalize(st);

    *out_paths = paths;
    *out_count = i;
}

/* ---- Song favorites / play counts (player-screen heart icon, Most Played
 * auto-playlist) -- same shape as the book favorite functions just above,
 * joined against media instead of book. ---- */

bool metadata_db_song_favorite_is_set(const char * path) {
    if (!db) return false;

    sqlite3_stmt * st = NULL;
    if (sqlite3_prepare_v2(db, "SELECT 1 FROM song_favorite WHERE path = ?;", -1, &st, NULL) != SQLITE_OK) return false;
    sqlite3_bind_text(st, 1, path, -1, SQLITE_STATIC);
    bool is_set = sqlite3_step(st) == SQLITE_ROW;
    sqlite3_finalize(st);
    return is_set;
}

void metadata_db_song_favorite_set(const char * path, bool is_favorite) {
    if (!db) return;

    sqlite3_stmt * st = NULL;
    if (is_favorite) {
        if (sqlite3_prepare_v2(db, "INSERT INTO song_favorite (path) VALUES (?) ON CONFLICT(path) DO NOTHING;", -1, &st,
                                NULL) != SQLITE_OK) {
            return;
        }
    } else {
        if (sqlite3_prepare_v2(db, "DELETE FROM song_favorite WHERE path = ?;", -1, &st, NULL) != SQLITE_OK) return;
    }
    sqlite3_bind_text(st, 1, path, -1, SQLITE_STATIC);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

void metadata_db_load_favorite_songs(char *** out_paths, int * out_count) {
    *out_paths = NULL;
    *out_count = 0;
    if (!db) return;

    sqlite3_stmt * st = NULL;
    /* INNER JOIN against media, not a plain SELECT from song_favorite --
     * same staleness filtering as metadata_db_load_favorite_books(), so a
     * favorited song that's since been removed from the library (deleted,
     * or just not seen by the current scan) silently drops off this list
     * rather than showing a dead entry. */
    static const char * const query =
        "SELECT song_favorite.path FROM song_favorite "
        "INNER JOIN media ON media.path = song_favorite.path "
        "ORDER BY song_favorite.path COLLATE NOCASE;";
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM song_favorite INNER JOIN media ON media.path = song_favorite.path;",
                            -1, &st, NULL) != SQLITE_OK) {
        return;
    }
    int row_count = 0;
    if (sqlite3_step(st) == SQLITE_ROW) row_count = (int) sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    if (row_count <= 0) return;

    char ** paths = malloc(sizeof(char *) * (size_t) row_count);
    if (sqlite3_prepare_v2(db, query, -1, &st, NULL) != SQLITE_OK) {
        free(paths);
        return;
    }

    int i = 0;
    while (i < row_count && sqlite3_step(st) == SQLITE_ROW) {
        paths[i] = strdup((const char *) sqlite3_column_text(st, 0));
        i++;
    }
    sqlite3_finalize(st);

    *out_paths = paths;
    *out_count = i;
}

void metadata_db_song_play_count_increment(const char * path) {
    if (!db) return;

    sqlite3_stmt * st = NULL;
    static const char * const query =
        "INSERT INTO song_play_count (path, count, last_played) VALUES (?, 1, ?) "
        "ON CONFLICT(path) DO UPDATE SET count = count + 1, last_played = excluded.last_played;";
    if (sqlite3_prepare_v2(db, query, -1, &st, NULL) != SQLITE_OK) return;
    sqlite3_bind_text(st, 1, path, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 2, (int64_t) time(NULL));
    sqlite3_step(st);
    sqlite3_finalize(st);
}

void metadata_db_load_top_played_songs(int limit, char *** out_paths, int * out_count) {
    *out_paths = NULL;
    *out_count = 0;
    if (!db || limit <= 0) return;

    sqlite3_stmt * st = NULL;
    /* Same INNER-JOIN-against-media staleness filtering as
     * metadata_db_load_favorite_songs() -- ORDER BY count DESC (most
     * played first), last_played DESC as a tiebreaker (most recently
     * played wins over a tie, an arbitrary but stable choice) rather than
     * an unspecified/path-order tiebreak. */
    static const char * const query =
        "SELECT song_play_count.path FROM song_play_count "
        "INNER JOIN media ON media.path = song_play_count.path "
        "ORDER BY song_play_count.count DESC, song_play_count.last_played DESC "
        "LIMIT ?;";
    if (sqlite3_prepare_v2(db, query, -1, &st, NULL) != SQLITE_OK) return;
    sqlite3_bind_int(st, 1, limit);

    char ** paths = malloc(sizeof(char *) * (size_t) limit);
    int i = 0;
    while (i < limit && sqlite3_step(st) == SQLITE_ROW) {
        paths[i] = strdup((const char *) sqlite3_column_text(st, 0));
        i++;
    }
    sqlite3_finalize(st);

    if (i == 0) {
        free(paths);
        return;
    }

    *out_paths = paths;
    *out_count = i;
}

void metadata_db_subsonic_server_save(const char * url, const char * username, const char * password, bool verify_tls) {
    if (!db) return;

    sqlite3_stmt * st = NULL;
    static const char * const query =
        "INSERT INTO subsonic_server (url, username, password, verify_tls) VALUES (?, ?, ?, ?) "
        "ON CONFLICT(url) DO UPDATE SET username = excluded.username, password = excluded.password, "
        "verify_tls = excluded.verify_tls;";
    if (sqlite3_prepare_v2(db, query, -1, &st, NULL) != SQLITE_OK) return;
    sqlite3_bind_text(st, 1, url, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, username, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 3, password, -1, SQLITE_STATIC);
    sqlite3_bind_int(st, 4, verify_tls ? 1 : 0);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

void metadata_db_load_subsonic_servers(subsonic_server_row_t ** out_rows, int * out_count) {
    *out_rows = NULL;
    *out_count = 0;
    if (!db) return;

    sqlite3_stmt * st = NULL;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM subsonic_server;", -1, &st, NULL) != SQLITE_OK) return;
    int row_count = 0;
    if (sqlite3_step(st) == SQLITE_ROW) row_count = (int) sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    if (row_count <= 0) return;

    subsonic_server_row_t * rows = malloc(sizeof(subsonic_server_row_t) * (size_t) row_count);
    static const char * const list_query =
        "SELECT url, username, password, verify_tls FROM subsonic_server ORDER BY url COLLATE NOCASE;";
    if (sqlite3_prepare_v2(db, list_query, -1, &st, NULL) != SQLITE_OK) {
        free(rows);
        return;
    }

    int i = 0;
    while (i < row_count && sqlite3_step(st) == SQLITE_ROW) {
        snprintf(rows[i].url, sizeof(rows[i].url), "%s", (const char *) sqlite3_column_text(st, 0));
        snprintf(rows[i].username, sizeof(rows[i].username), "%s", (const char *) sqlite3_column_text(st, 1));
        snprintf(rows[i].password, sizeof(rows[i].password), "%s", (const char *) sqlite3_column_text(st, 2));
        rows[i].verify_tls = sqlite3_column_int(st, 3) != 0;
        i++;
    }
    sqlite3_finalize(st);

    *out_rows = rows;
    *out_count = i;
}
