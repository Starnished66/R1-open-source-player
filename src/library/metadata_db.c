#include "metadata_db.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Audit finding: sqlite3_column_text() returns NULL for a genuinely NULL
 * column value (not just on allocation failure) -- every call site in this
 * file used to cast its result straight to `const char *` and hand it to
 * snprintf("%s", ...)/strdup() with no NULL check. glibc's printf family
 * tolerates a NULL %s argument (prints "(null)"), but this target's actual
 * libc (musl, per every mipsel-linux-musl-gcc build in this project) does
 * not special-case it -- a NULL column value crashes immediately. The
 * import path from the stock player's own recovered database is the
 * highest-risk caller (see this file's own "database disk image is
 * malformed" comment just below -- a real, already-observed corruption
 * case), but the same unguarded pattern existed at every other query site
 * in this file too, including ones reading from this app's own schema;
 * fixed once, everywhere, rather than only at the one flagged call site. */
static const char * col_text(sqlite3_stmt * stmt, int col) {
    const unsigned char * text = sqlite3_column_text(stmt, col);
    return text ? (const char *) text : "";
}

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
static bool update_failed = false;
#define METADATA_DB_UPDATE_BATCH 128

/* SQLite's serialized mode protects an individual sqlite3 handle itself, but
 * not this module's own plain-C globals above (the prepared statements,
 * update_generation/update_ops_since_commit/update_failed) shared between
 * the background rescan thread and the UI thread's own favorite/playlist/
 * subsonic-server call sites -- a recursive module lock covers those. Also
 * covers metadata_db_open()'s warm-start import, which calls
 * metadata_db_put() from inside the already-locked open path. */
static pthread_once_t metadata_db_mutex_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t metadata_db_mutex;

static void metadata_db_mutex_init(void) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&metadata_db_mutex, &attr);
    pthread_mutexattr_destroy(&attr);
}

typedef struct { bool locked; } metadata_db_guard_t;
static void metadata_db_guard_release(metadata_db_guard_t * guard) {
    if (guard->locked) pthread_mutex_unlock(&metadata_db_mutex);
}
#define METADATA_DB_GUARD \
    pthread_once(&metadata_db_mutex_once, metadata_db_mutex_init); \
    pthread_mutex_lock(&metadata_db_mutex); \
    metadata_db_guard_t metadata_db_guard __attribute__((cleanup(metadata_db_guard_release))) = { true }

/* NULL is a valid, harmless argument to sqlite3_finalize()/sqlite3_close()
 * -- used freely below instead of guarding every call. */

static void prepare_statements(void) {
    sqlite3_prepare_v2(db,
        "SELECT mtime, size, title, artist, album, album_artist, genre FROM media WHERE path = ?;",
        -1, &stmt_get, NULL);
    /* first_seen is bound only for the INSERT branch (a fresh row, see
     * metadata_db_put() below) and deliberately absent from the ON CONFLICT
     * DO UPDATE SET clause -- an update to an already-tracked row must never
     * touch it, see the column's own migration comment above for why. */
    sqlite3_prepare_v2(db,
        "INSERT INTO media (path, mtime, size, title, artist, album, album_artist, genre, scan_generation, first_seen) "
        "VALUES (?,?,?,?,?,?,?,?,?,?) "
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
        const char * raw_path = col_text(st, 0);
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
        snprintf(row->tags.title, sizeof(row->tags.title), "%s", col_text(st, 1));
        snprintf(row->tags.artist, sizeof(row->tags.artist), "%s", col_text(st, 2));
        snprintf(row->tags.album, sizeof(row->tags.album), "%s", col_text(st, 3));
        snprintf(row->tags.album_artist, sizeof(row->tags.album_artist), "%s", col_text(st, 4));
        snprintf(row->tags.genre, sizeof(row->tags.genre), "%s", col_text(st, 5));
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
/* Plain read/copy/write rather than rename() -- callers use this across the
 * ubifs (/usr/data) / SD card (vfat) boundary, and rename(2) can't cross
 * that (EXDEV). Leaves dst untouched (removes a partial write) on failure,
 * so a caller that finds this returning false can trust the destination is
 * either a complete copy or doesn't exist, never a truncated one. */
static bool copy_file(const char * src, const char * dst) {
    FILE * in = fopen(src, "rb");
    if (!in) return false;
    FILE * out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return false;
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
    if (ok && fflush(out) != 0) ok = false;
    if (ok && fsync(fileno(out)) != 0) ok = false;
    if (fclose(in) != 0) ok = false;
    if (fclose(out) != 0) ok = false;

    if (!ok) remove(dst);
    return ok;
}

/* One-time migration for anyone updating from before this file moved to the
 * SD card (see METADATA_DB_PATH's own comment) -- without this, a device
 * with an existing database at the old path would silently appear to lose
 * every song_favorite/song_play_count/playlist/subsonic_server row (real
 * user data, not just the rebuildable tag cache) the next time the app
 * opens, since it would just create a fresh empty database at the new path
 * instead. Only removes the old copy once every byte of the new one has
 * been written successfully (copy_file()'s own contract), so a failure
 * partway through (e.g. the SD card filling up) leaves the original still
 * in place to retry from, rather than losing both. */
#define OLD_METADATA_DB_PATH "/usr/data/open_hiby_player_music.db"
static void migrate_old_db_if_needed(void) {
    if (access(METADATA_DB_PATH, F_OK) == 0) return; /* already migrated (or fresh install) */
    if (access(OLD_METADATA_DB_PATH, F_OK) != 0) return; /* nothing to migrate */

    if (copy_file(OLD_METADATA_DB_PATH, METADATA_DB_PATH)) remove(OLD_METADATA_DB_PATH);
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
    METADATA_DB_GUARD;
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
        "  scan_generation INTEGER NOT NULL DEFAULT 0,"
        "  first_seen INTEGER NOT NULL DEFAULT 0"
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
    /* first_seen backs the Recently Added smart playlist (Music -> Playlists)
     * -- deliberately NOT mtime (the file's own filesystem last-modified
     * time, already tracked above): mtime gets legitimately bumped by a
     * later retag/edit of a song already in the library, which would
     * incorrectly resurface it at the top of Recently Added. first_seen is
     * set once, only on a row's very first INSERT (see metadata_db_put()
     * below) -- 0 for any row that predates this migration, which just
     * sorts old rows to the bottom of Recently Added rather than the top,
     * a reasonable default (nothing false-positives as "newly added"). */
    sqlite3_exec(db, "ALTER TABLE media ADD COLUMN first_seen INTEGER NOT NULL DEFAULT 0;", NULL, NULL, NULL);

    /* Covering indexes for every paged browse order the metadata_db_get_*_page()
     * functions below use -- CREATE INDEX IF NOT EXISTS is safe to run on
     * every open, no migration flag needed. Real bug caught in review: an
     * earlier version of each of these named `rowid` explicitly as a
     * trailing column ("... COLLATE NOCASE, rowid)"), on the theory that
     * this would let a keyset page's tiebreaker comparison be satisfied by
     * the index alone -- SQLite rejects that outright ("no such column:
     * rowid"), confirmed independently against this exact schema. Every one
     * of these CREATE INDEX calls was silently failing (sqlite3_exec()'s
     * return value was never checked), so the app has been running without
     * any of these indexes the whole time, falling back to a full table
     * scan + temporary sort for every title/artist/album/album_artist-
     * ordered query -- invisible against this device's real ~2,200-song
     * library, but exactly the cost this covering-index work exists to
     * avoid at real scale. Fixed two ways: dropped the invalid trailing
     * `rowid` column (unnecessary in the first place -- every index on a
     * rowid table already carries rowid as an implicit final tiebreaker,
     * per SQLite's own documented behavior), and added error logging so a
     * future mistake here fails loudly instead of silently. */
    const char * const index_sql[] = {
        "CREATE INDEX IF NOT EXISTS idx_media_title ON media(title COLLATE NOCASE);",
        "CREATE INDEX IF NOT EXISTS idx_media_artist ON media(artist COLLATE NOCASE, album COLLATE NOCASE);",
        "CREATE INDEX IF NOT EXISTS idx_media_album ON media(album COLLATE NOCASE, album_artist COLLATE NOCASE);",
        "CREATE INDEX IF NOT EXISTS idx_media_album_artist ON media(album_artist COLLATE NOCASE, album COLLATE NOCASE);",
        "CREATE INDEX IF NOT EXISTS idx_media_generation ON media(scan_generation);",
    };
    for (size_t i = 0; i < sizeof(index_sql) / sizeof(index_sql[0]); i++) {
        char * err = NULL;
        if (sqlite3_exec(db, index_sql[i], NULL, NULL, &err) != SQLITE_OK) {
            fprintf(stderr, "Warning: failed to create index (%s): %s\n", index_sql[i], err ? err : "unknown error");
            sqlite3_free(err);
        }
    }

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
    METADATA_DB_GUARD;
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
    if (sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL) != SQLITE_OK ||
        sqlite3_exec(db, "BEGIN;", NULL, NULL, NULL) != SQLITE_OK) {
        update_failed = true;
    }
    update_ops_since_commit = 0;
}

/* Writes directly against the SD-resident `db` for the whole scan, same as
 * Rockbox's own tagcache -- an earlier version of this function instead
 * scanned into a scratch copy on /usr/data and copied the finished result
 * back to the SD card in one shot at the end (metadata_db_end_update()),
 * specifically to avoid sustained direct writes to this device's FAT32 SD
 * card (see METADATA_DB_PATH's own comment on the corruption incident that
 * motivated it). Real-device testing of that approach against a 32,000-song
 * library found a worse failure: the end-of-scan verify-and-copy-back step
 * (a PRAGMA quick_check plus a whole-file read+write+fsync of the ~15MB
 * scratch database) itself starved the whole device of CPU/scheduling for
 * long enough to look and behave like a hang -- confirmed live (a second
 * concurrent ADB shell was itself unable to run even trivial commands) --
 * which is a worse outcome than the sustained-small-writes risk it was
 * meant to avoid. Reverted back to direct-to-SD: metadata_db_get()/put()'s
 * own periodic checkpointing (metadata_db_update_checkpoint_if_needed(),
 * every METADATA_DB_UPDATE_BATCH ops) already replaced the original bug's
 * actual cause (one giant unbounded transaction) with small, frequent
 * commits -- the same shape of write pattern Rockbox's own tagcache uses
 * against real SD cards -- without ever needing to hold the whole database
 * in memory or copy it wholesale. */
void metadata_db_begin_update(void) {
    METADATA_DB_GUARD;
    if (!db) return;

    update_failed = false;

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
    if (sqlite3_exec(db, "BEGIN;", NULL, NULL, NULL) != SQLITE_OK) update_failed = true;
}

bool metadata_db_get(const char * path, int64_t mtime, int64_t size, cached_tags_t * out) {
    METADATA_DB_GUARD;
    if (!db) return false;

    /* Mark as observed without building an O(N) TEMP seen_paths table. */
    if (update_generation != 0 && !update_failed) {
        sqlite3_reset(stmt_mark_seen);
        sqlite3_clear_bindings(stmt_mark_seen);
        sqlite3_bind_int64(stmt_mark_seen, 1, update_generation);
        sqlite3_bind_text(stmt_mark_seen, 2, path, -1, SQLITE_STATIC);
        if (sqlite3_step(stmt_mark_seen) != SQLITE_DONE) update_failed = true;
        metadata_db_update_checkpoint_if_needed();
    }

    sqlite3_reset(stmt_get);
    sqlite3_clear_bindings(stmt_get);
    sqlite3_bind_text(stmt_get, 1, path, -1, SQLITE_STATIC);
    if (sqlite3_step(stmt_get) != SQLITE_ROW) return false;

    int64_t cached_mtime = sqlite3_column_int64(stmt_get, 0);
    int64_t cached_size = sqlite3_column_int64(stmt_get, 1);
    if (cached_mtime != mtime || cached_size != size) return false;

    snprintf(out->title, sizeof(out->title), "%s", col_text(stmt_get, 2));
    snprintf(out->artist, sizeof(out->artist), "%s", col_text(stmt_get, 3));
    snprintf(out->album, sizeof(out->album), "%s", col_text(stmt_get, 4));
    snprintf(out->album_artist, sizeof(out->album_artist), "%s", col_text(stmt_get, 5));
    snprintf(out->genre, sizeof(out->genre), "%s", col_text(stmt_get, 6));
    return true;
}

void metadata_db_put(const char * path, int64_t mtime, int64_t size, const cached_tags_t * tags) {
    METADATA_DB_GUARD;
    if (!db) return;
    if (update_failed) return;

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
    sqlite3_bind_int64(stmt_put, 10, (int64_t) time(NULL)); /* only takes effect on a fresh INSERT -- see stmt_put's own comment */
    if (sqlite3_step(stmt_put) != SQLITE_DONE) update_failed = true;
    metadata_db_update_checkpoint_if_needed();
}

void metadata_db_end_update(void) {
    METADATA_DB_GUARD;
    if (!db || update_generation == 0) return;

    /* Deletion detection is now an indexed generation comparison rather than
     * NOT IN against an in-memory TEMP table containing every scanned path. */
    sqlite3_stmt * st = NULL;
    if (!update_failed) {
        if (sqlite3_prepare_v2(db, "DELETE FROM media WHERE scan_generation <> ?;", -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, update_generation);
            if (sqlite3_step(st) != SQLITE_DONE) update_failed = true;
        } else {
            update_failed = true;
        }
    }
    sqlite3_finalize(st);
    if (sqlite3_exec(db, update_failed ? "ROLLBACK;" : "COMMIT;", NULL, NULL, NULL) != SQLITE_OK) update_failed = true;
    update_generation = 0;
    update_ops_since_commit = 0;
    update_failed = false;
}

void metadata_db_abort_update(void) {
    METADATA_DB_GUARD;
    if (!db || update_generation == 0) return;
    sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
    /* Do not prune anything: rows already marked with this generation are
     * harmless. The next complete scan chooses a larger generation. */
    update_generation = 0;
    update_ops_since_commit = 0;
    update_failed = false;
}

int64_t metadata_db_get_song_count(void) {
    METADATA_DB_GUARD;
    if (!db) return 0;

    sqlite3_stmt * st = NULL;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM media;", -1, &st, NULL) != SQLITE_OK) return 0;
    int64_t count = 0;
    if (sqlite3_step(st) == SQLITE_ROW) count = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return count;
}

void metadata_db_get_group_counts(int * out_artist_count, int * out_album_artist_count, int * out_album_count) {
    METADATA_DB_GUARD;
    *out_artist_count = 0;
    *out_album_artist_count = 0;
    *out_album_count = 0;
    if (!db) return;

    /* Three independent scalar subqueries in one statement -- cheaper than
     * three separate prepare/step/finalize round trips for what's ultimately
     * three small COUNT(DISTINCT ...) scans against the same table. The
     * album count's DISTINCT key is LOWER(album)||sep||LOWER(album_artist),
     * not a COLLATE NOCASE suffix -- COLLATE binds to its one immediately
     * preceding operand, not a whole `a || b` expression, so a trailing
     * COLLATE NOCASE here would silently leave album's own case sensitive
     * while only normalizing album_artist. LOWER() folds both sides before
     * concatenation instead, sidestepping that entirely (and matches NOCASE's
     * own ASCII-only case folding, so this stays consistent with
     * metadata_db_get_groups_page()'s real GROUP BY ... COLLATE NOCASE). */
    static const char * const query =
        "SELECT (SELECT COUNT(DISTINCT artist COLLATE NOCASE) FROM media), "
        "       (SELECT COUNT(DISTINCT album_artist COLLATE NOCASE) FROM media), "
        "       (SELECT COUNT(DISTINCT LOWER(album) || '\x01' || LOWER(album_artist)) FROM media);";
    sqlite3_stmt * st = NULL;
    if (sqlite3_prepare_v2(db, query, -1, &st, NULL) != SQLITE_OK) return;
    if (sqlite3_step(st) == SQLITE_ROW) {
        *out_artist_count = sqlite3_column_int(st, 0);
        *out_album_artist_count = sqlite3_column_int(st, 1);
        *out_album_count = sqlite3_column_int(st, 2);
    }
    sqlite3_finalize(st);
}

static int fill_song_rows(sqlite3_stmt * st, int id_col, int path_col, int title_col, int artist_col, int album_col,
                           int album_artist_col, int genre_col, song_row_t * out_rows, int max_rows) {
    int i = 0;
    while (i < max_rows && sqlite3_step(st) == SQLITE_ROW) {
        out_rows[i].id = sqlite3_column_int64(st, id_col);
        snprintf(out_rows[i].path, sizeof(out_rows[i].path), "%s", col_text(st, path_col));
        snprintf(out_rows[i].tags.title, sizeof(out_rows[i].tags.title), "%s",
                 col_text(st, title_col));
        snprintf(out_rows[i].tags.artist, sizeof(out_rows[i].tags.artist), "%s",
                 col_text(st, artist_col));
        snprintf(out_rows[i].tags.album, sizeof(out_rows[i].tags.album), "%s",
                 col_text(st, album_col));
        snprintf(out_rows[i].tags.album_artist, sizeof(out_rows[i].tags.album_artist), "%s",
                 col_text(st, album_artist_col));
        snprintf(out_rows[i].tags.genre, sizeof(out_rows[i].tags.genre), "%s",
                 col_text(st, genre_col));
        i++;
    }
    return i;
}

int metadata_db_get_songs_page(const char * after_title, int64_t after_id, int max_rows, song_row_t * out_rows) {
    METADATA_DB_GUARD;
    if (!db || max_rows <= 0) return 0;

    sqlite3_stmt * st = NULL;
    int count;
    if (after_title) {
        static const char * const query =
            "SELECT rowid, path, title, artist, album, album_artist, genre FROM media "
            "WHERE title COLLATE NOCASE > ? OR (title COLLATE NOCASE = ? AND rowid > ?) "
            "ORDER BY title COLLATE NOCASE, rowid LIMIT ?;";
        if (sqlite3_prepare_v2(db, query, -1, &st, NULL) != SQLITE_OK) return 0;
        sqlite3_bind_text(st, 1, after_title, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 2, after_title, -1, SQLITE_STATIC);
        sqlite3_bind_int64(st, 3, after_id);
        sqlite3_bind_int(st, 4, max_rows);
    } else {
        static const char * const query =
            "SELECT rowid, path, title, artist, album, album_artist, genre FROM media "
            "ORDER BY title COLLATE NOCASE, rowid LIMIT ?;";
        if (sqlite3_prepare_v2(db, query, -1, &st, NULL) != SQLITE_OK) return 0;
        sqlite3_bind_int(st, 1, max_rows);
    }
    count = fill_song_rows(st, 0, 1, 2, 3, 4, 5, 6, out_rows, max_rows);
    sqlite3_finalize(st);
    return count;
}

/* Offset-paginated page of songs ordered by first_seen DESC (rowid DESC
 * tiebreak) -- backs the Recently Added screen's own compact_list_set_
 * paged_provider(), same "bounded page at a time, regardless of library
 * size" shape as metadata_db_get_songs_filtered_page() (used by All Songs)
 * rather than metadata_db_load_recently_added_songs() above (deliberately
 * capped at a small limit, for the scoped-playlist/remote-control case
 * only -- see its own comment). Offset-based, not keyset, matching every
 * other compact_list-paged provider's own query (Artists/Albums/Album
 * Artist/All Songs) -- a virtualized UI list needs arbitrary scroll-driven
 * jumps, not just "next page". */
int metadata_db_get_songs_page_by_recency(int offset, int max_rows, song_row_t * out_rows) {
    METADATA_DB_GUARD;
    if (!db || max_rows <= 0) return 0;
    if (offset < 0) offset = 0;

    static const char * const query =
        "SELECT rowid, path, title, artist, album, album_artist, genre FROM media "
        "ORDER BY first_seen DESC, rowid DESC LIMIT ? OFFSET ?;";
    sqlite3_stmt * st = NULL;
    if (sqlite3_prepare_v2(db, query, -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int(st, 1, max_rows);
    sqlite3_bind_int(st, 2, offset);
    int count = fill_song_rows(st, 0, 1, 2, 3, 4, 5, 6, out_rows, max_rows);
    sqlite3_finalize(st);
    return count;
}

static const char * group_kind_column(metadata_db_group_kind_t kind) {
    switch (kind) {
        case METADATA_DB_GROUP_ARTIST: return "artist";
        case METADATA_DB_GROUP_ALBUM_ARTIST: return "album_artist";
        case METADATA_DB_GROUP_ALBUM: return "album";
    }
    return "artist";
}

int metadata_db_get_groups_page(metadata_db_group_kind_t kind, int offset, int max_rows, group_row_t * out_rows) {
    METADATA_DB_GUARD;
    if (!db || max_rows <= 0) return 0;
    if (offset < 0) offset = 0;

    const char * col = group_kind_column(kind);
    char query[512];
    /* col comes only from group_kind_column()'s own fixed set above, never
     * from external input -- safe to interpolate directly, same as this
     * file's other internally-constant query fragments. */
    snprintf(query, sizeof(query),
              "SELECT %s, COUNT(*), MIN(rowid) FROM media "
              "GROUP BY %s COLLATE NOCASE ORDER BY %s COLLATE NOCASE LIMIT ? OFFSET ?;",
              col, col, col);

    sqlite3_stmt * st = NULL;
    if (sqlite3_prepare_v2(db, query, -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int(st, 1, max_rows);
    sqlite3_bind_int(st, 2, offset);

    int i = 0;
    while (i < max_rows && sqlite3_step(st) == SQLITE_ROW) {
        snprintf(out_rows[i].name, sizeof(out_rows[i].name), "%s", col_text(st, 0));
        out_rows[i].song_count = sqlite3_column_int(st, 1);
        out_rows[i].first_song_id = sqlite3_column_int64(st, 2);
        out_rows[i].album_artist[0] = '\0'; /* see this field's own comment (metadata_db.h) -- only ever meaningful from metadata_db_get_albums_page_filtered() */
        i++;
    }
    sqlite3_finalize(st);
    return i;
}

int metadata_db_get_artist_songs(const char * artist, int offset, song_row_t * out_rows, int max_rows) {
    METADATA_DB_GUARD;
    if (!db || max_rows <= 0) return 0;
    if (offset < 0) offset = 0;

    static const char * const query =
        "SELECT rowid, path, title, artist, album, album_artist, genre FROM media "
        "WHERE artist = ? COLLATE NOCASE "
        "ORDER BY album COLLATE NOCASE, path COLLATE NOCASE LIMIT ? OFFSET ?;";
    sqlite3_stmt * st = NULL;
    if (sqlite3_prepare_v2(db, query, -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(st, 1, artist, -1, SQLITE_STATIC);
    sqlite3_bind_int(st, 2, max_rows);
    sqlite3_bind_int(st, 3, offset);
    int count = fill_song_rows(st, 0, 1, 2, 3, 4, 5, 6, out_rows, max_rows);
    sqlite3_finalize(st);
    return count;
}

int metadata_db_get_album_songs(const char * album, const char * album_artist, int offset, song_row_t * out_rows,
                                 int max_rows) {
    METADATA_DB_GUARD;
    if (!db || max_rows <= 0) return 0;
    if (offset < 0) offset = 0;

    static const char * const query =
        "SELECT rowid, path, title, artist, album, album_artist, genre FROM media "
        "WHERE album = ? COLLATE NOCASE AND album_artist = ? COLLATE NOCASE "
        "ORDER BY path COLLATE NOCASE LIMIT ? OFFSET ?;";
    sqlite3_stmt * st = NULL;
    if (sqlite3_prepare_v2(db, query, -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(st, 1, album, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, album_artist, -1, SQLITE_STATIC);
    sqlite3_bind_int(st, 3, max_rows);
    sqlite3_bind_int(st, 4, offset);
    int count = fill_song_rows(st, 0, 1, 2, 3, 4, 5, 6, out_rows, max_rows);
    sqlite3_finalize(st);
    return count;
}

/* Builds the shared WHERE clause for metadata_db_count_songs_filtered()/
 * metadata_db_get_songs_filtered_page() below -- query (title/artist
 * substring, case-insensitive) and the three exact-match filters are each
 * independently optional (NULL or "" skips that condition entirely), same
 * semantics remote_control.c's own library_song_matches() used against its
 * in-memory array copy. Binds in the fixed order: query (x2, for title OR
 * artist), artist, album_artist, album -- caller must bind in that same
 * order, skipping any placeholder whose condition wasn't included. Returns
 * the number of placeholders appended (0-5), so the caller knows how many
 * binds to actually perform. */
static int append_song_filter_where(char * sql, size_t sql_size, const char * query, const char * artist_filter,
                                     const char * album_artist_filter, const char * album_filter) {
    size_t len = strlen(sql);
    int placeholders = 0;
    bool have_query = query && query[0];
    bool have_artist = artist_filter && artist_filter[0];
    bool have_album_artist = album_artist_filter && album_artist_filter[0];
    bool have_album = album_filter && album_filter[0];
    if (!have_query && !have_artist && !have_album_artist && !have_album) return 0;

    len += (size_t) snprintf(sql + len, sql_size - len, " WHERE ");
    bool first = true;
    if (have_query) {
        len += (size_t) snprintf(sql + len, sql_size - len,
                                  "%s(title LIKE ? ESCAPE '\\' OR artist LIKE ? ESCAPE '\\')", first ? "" : " AND ");
        placeholders += 2;
        first = false;
    }
    if (have_artist) {
        len += (size_t) snprintf(sql + len, sql_size - len, "%sartist = ? COLLATE NOCASE", first ? "" : " AND ");
        placeholders += 1;
        first = false;
    }
    if (have_album_artist) {
        len += (size_t) snprintf(sql + len, sql_size - len, "%salbum_artist = ? COLLATE NOCASE", first ? "" : " AND ");
        placeholders += 1;
        first = false;
    }
    if (have_album) {
        len += (size_t) snprintf(sql + len, sql_size - len, "%salbum = ? COLLATE NOCASE", first ? "" : " AND ");
        placeholders += 1;
    }
    (void) len;
    return placeholders;
}

/* query_text's LIKE metacharacters are escaped the same way metadata_db_
 * search_songs() escapes them -- shared here since both build a %...%
 * pattern from untrusted user text. out_pattern must be at least 260 bytes. */
static void build_like_pattern(const char * query_text, char * out_pattern, size_t out_pattern_size) {
    char escaped[256];
    size_t out_len = 0;
    for (const char * p = query_text; *p && out_len + 2 < sizeof(escaped) - 2; p++) {
        if (*p == '%' || *p == '_' || *p == '\\') escaped[out_len++] = '\\';
        escaped[out_len++] = *p;
    }
    escaped[out_len] = '\0';
    snprintf(out_pattern, out_pattern_size, "%%%s%%", escaped);
}

static int bind_song_filters(sqlite3_stmt * st, int start_index, const char * query, const char * pattern,
                              const char * artist_filter, const char * album_artist_filter,
                              const char * album_filter) {
    int idx = start_index;
    if (query && query[0]) {
        sqlite3_bind_text(st, idx++, pattern, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, idx++, pattern, -1, SQLITE_STATIC);
    }
    if (artist_filter && artist_filter[0]) sqlite3_bind_text(st, idx++, artist_filter, -1, SQLITE_STATIC);
    if (album_artist_filter && album_artist_filter[0]) sqlite3_bind_text(st, idx++, album_artist_filter, -1, SQLITE_STATIC);
    if (album_filter && album_filter[0]) sqlite3_bind_text(st, idx++, album_filter, -1, SQLITE_STATIC);
    return idx;
}

/* Remote-control's GET /api/library: a text search (title/artist substring)
 * optionally narrowed by exact artist/album_artist/album filters, same
 * combined semantics remote_control.c used to apply by hand against its own
 * full in-memory library copy -- this is the query that copy existed to
 * make cheap; querying SQLite directly here removes the need for that copy
 * to exist at all. */
int64_t metadata_db_count_songs_filtered(const char * query, const char * artist_filter,
                                          const char * album_artist_filter, const char * album_filter) {
    METADATA_DB_GUARD;
    if (!db) return 0;

    char sql[512] = "SELECT COUNT(*) FROM media";
    int placeholders = append_song_filter_where(sql, sizeof(sql), query, artist_filter, album_artist_filter, album_filter);
    strncat(sql, ";", sizeof(sql) - strlen(sql) - 1);

    sqlite3_stmt * st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return 0;
    char pattern[260];
    if (placeholders > 0 && query && query[0]) build_like_pattern(query, pattern, sizeof(pattern));
    bind_song_filters(st, 1, query, pattern, artist_filter, album_artist_filter, album_filter);

    int64_t count = 0;
    if (sqlite3_step(st) == SQLITE_ROW) count = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return count;
}

int metadata_db_get_songs_filtered_page(const char * query, const char * artist_filter,
                                         const char * album_artist_filter, const char * album_filter, int offset,
                                         int max_rows, song_row_t * out_rows) {
    METADATA_DB_GUARD;
    if (!db || max_rows <= 0) return 0;
    if (offset < 0) offset = 0;

    char sql[600] = "SELECT rowid, path, title, artist, album, album_artist, genre FROM media";
    append_song_filter_where(sql, sizeof(sql), query, artist_filter, album_artist_filter, album_filter);
    strncat(sql, " ORDER BY title COLLATE NOCASE, rowid LIMIT ? OFFSET ?;", sizeof(sql) - strlen(sql) - 1);

    sqlite3_stmt * st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return 0;
    char pattern[260];
    if (query && query[0]) build_like_pattern(query, pattern, sizeof(pattern));
    int next = bind_song_filters(st, 1, query, pattern, artist_filter, album_artist_filter, album_filter);
    sqlite3_bind_int(st, next++, max_rows);
    sqlite3_bind_int(st, next, offset);

    int count = fill_song_rows(st, 0, 1, 2, 3, 4, 5, 6, out_rows, max_rows);
    sqlite3_finalize(st);
    return count;
}

/* Albums grouped and filtered by artist OR album_artist matching one name
 * -- GET /api/library/albums?artist=X (or ?album_artist=X), gui.c's own
 * Albums screen (unfiltered). filter may be NULL/"" for the unfiltered
 * "every album in the library" case. Real bug caught in review: this used
 * to group by album name alone, silently merging two different artists'
 * same-titled albums into one row -- disagreeing with metadata_db_get_
 * group_counts()'s own album count (already correctly counted as distinct
 * (album, album_artist) pairs) and combining unrelated albums' song
 * counts/cover art. Groups by the pair now; out_rows[i].album_artist
 * carries the second half of that identity, needed by metadata_db_get_
 * album_songs() below to fetch the right album's songs. Offset-paginated,
 * not the keyset "after_name" this used to take -- no real caller ever
 * continued a keyset cursor here anyway (every one passed NULL, i.e. "just
 * the first page"), and offset is what a virtualized UI list actually
 * needs (arbitrary scroll-driven jumps, not just "next page"). */
int metadata_db_get_albums_page_filtered(const char * artist_or_album_artist_filter, int offset, int max_rows,
                                          group_row_t * out_rows) {
    METADATA_DB_GUARD;
    if (!db || max_rows <= 0) return 0;
    if (offset < 0) offset = 0;

    sqlite3_stmt * st = NULL;
    bool have_filter = artist_or_album_artist_filter && artist_or_album_artist_filter[0];
    if (have_filter) {
        static const char * const query =
            "SELECT album, album_artist, COUNT(*), MIN(rowid) FROM media "
            "WHERE artist = ? COLLATE NOCASE OR album_artist = ? COLLATE NOCASE "
            "GROUP BY album COLLATE NOCASE, album_artist COLLATE NOCASE "
            "ORDER BY album COLLATE NOCASE, album_artist COLLATE NOCASE LIMIT ? OFFSET ?;";
        if (sqlite3_prepare_v2(db, query, -1, &st, NULL) != SQLITE_OK) return 0;
        sqlite3_bind_text(st, 1, artist_or_album_artist_filter, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 2, artist_or_album_artist_filter, -1, SQLITE_STATIC);
        sqlite3_bind_int(st, 3, max_rows);
        sqlite3_bind_int(st, 4, offset);
    } else {
        static const char * const query =
            "SELECT album, album_artist, COUNT(*), MIN(rowid) FROM media "
            "GROUP BY album COLLATE NOCASE, album_artist COLLATE NOCASE "
            "ORDER BY album COLLATE NOCASE, album_artist COLLATE NOCASE LIMIT ? OFFSET ?;";
        if (sqlite3_prepare_v2(db, query, -1, &st, NULL) != SQLITE_OK) return 0;
        sqlite3_bind_int(st, 1, max_rows);
        sqlite3_bind_int(st, 2, offset);
    }

    int i = 0;
    while (i < max_rows && sqlite3_step(st) == SQLITE_ROW) {
        snprintf(out_rows[i].name, sizeof(out_rows[i].name), "%s", col_text(st, 0));
        snprintf(out_rows[i].album_artist, sizeof(out_rows[i].album_artist), "%s",
                 col_text(st, 1));
        out_rows[i].song_count = sqlite3_column_int(st, 2);
        out_rows[i].first_song_id = sqlite3_column_int64(st, 3);
        i++;
    }
    sqlite3_finalize(st);
    return i;
}

int64_t metadata_db_count_albums_for_group(metadata_db_group_kind_t kind, const char * name) {
    METADATA_DB_GUARD;
    if (!db || !name || (kind != METADATA_DB_GROUP_ARTIST && kind != METADATA_DB_GROUP_ALBUM_ARTIST)) return 0;
    const char * column = kind == METADATA_DB_GROUP_ARTIST ? "artist" : "album_artist";
    char query[256];
    snprintf(query, sizeof(query),
             "SELECT COUNT(*) FROM (SELECT 1 FROM media WHERE %s = ? COLLATE NOCASE "
             "GROUP BY album COLLATE NOCASE, album_artist COLLATE NOCASE);", column);
    sqlite3_stmt * st = NULL;
    if (sqlite3_prepare_v2(db, query, -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
    int64_t count = 0;
    if (sqlite3_step(st) == SQLITE_ROW) count = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return count;
}

int metadata_db_get_albums_for_group(metadata_db_group_kind_t kind, const char * name, int offset, int max_rows,
                                      group_row_t * out_rows) {
    METADATA_DB_GUARD;
    if (!db || !name || max_rows <= 0 ||
        (kind != METADATA_DB_GROUP_ARTIST && kind != METADATA_DB_GROUP_ALBUM_ARTIST)) return 0;
    if (offset < 0) offset = 0;
    const char * column = kind == METADATA_DB_GROUP_ARTIST ? "artist" : "album_artist";
    char query[384];
    snprintf(query, sizeof(query),
             "SELECT album, album_artist, COUNT(*), MIN(rowid) FROM media "
             "WHERE %s = ? COLLATE NOCASE "
             "GROUP BY album COLLATE NOCASE, album_artist COLLATE NOCASE "
             "ORDER BY album COLLATE NOCASE, album_artist COLLATE NOCASE LIMIT ? OFFSET ?;", column);
    sqlite3_stmt * st = NULL;
    if (sqlite3_prepare_v2(db, query, -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
    sqlite3_bind_int(st, 2, max_rows);
    sqlite3_bind_int(st, 3, offset);
    int i = 0;
    while (i < max_rows && sqlite3_step(st) == SQLITE_ROW) {
        snprintf(out_rows[i].name, sizeof(out_rows[i].name), "%s", col_text(st, 0));
        snprintf(out_rows[i].album_artist, sizeof(out_rows[i].album_artist), "%s",
                 col_text(st, 1));
        out_rows[i].song_count = sqlite3_column_int(st, 2);
        out_rows[i].first_song_id = sqlite3_column_int64(st, 3);
        i++;
    }
    sqlite3_finalize(st);
    return i;
}

bool metadata_db_get_song_by_id(int64_t id, song_row_t * out_row) {
    METADATA_DB_GUARD;
    if (!db) return false;

    static const char * const query =
        "SELECT rowid, path, title, artist, album, album_artist, genre FROM media WHERE rowid = ?;";
    sqlite3_stmt * st = NULL;
    if (sqlite3_prepare_v2(db, query, -1, &st, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int64(st, 1, id);
    int count = fill_song_rows(st, 0, 1, 2, 3, 4, 5, 6, out_row, 1);
    sqlite3_finalize(st);
    return count == 1;
}

bool metadata_db_get_song_by_path(const char * path, song_row_t * out_row) {
    METADATA_DB_GUARD;
    if (!db) return false;

    static const char * const query =
        "SELECT rowid, path, title, artist, album, album_artist, genre FROM media WHERE path = ?;";
    sqlite3_stmt * st = NULL;
    if (sqlite3_prepare_v2(db, query, -1, &st, NULL) != SQLITE_OK) return false;
    sqlite3_bind_text(st, 1, path, -1, SQLITE_STATIC);
    int count = fill_song_rows(st, 0, 1, 2, 3, 4, 5, 6, out_row, 1);
    sqlite3_finalize(st);
    return count == 1;
}

/* Efficiency finding: build_queue_json()/remote_control_sync_queue()
 * (remote_control.c) used to call metadata_db_get_song_by_id()/_by_path()
 * once per song in the play queue, each its own sqlite3_prepare_v2()/
 * sqlite3_finalize() round trip -- wasted re-parsing/re-planning of the
 * identical query for every entry. One prepared statement, reused via
 * bind+step+reset across all `count` lookups, does the same job without
 * the repeated parse/plan cost. */
void metadata_db_get_songs_by_ids(const int64_t * ids, int count, song_row_t * out_rows) {
    METADATA_DB_GUARD;
    for (int i = 0; i < count; i++) out_rows[i].id = -1;
    if (!db || count <= 0) return;

    static const char * const query =
        "SELECT rowid, path, title, artist, album, album_artist, genre FROM media WHERE rowid = ?;";
    sqlite3_stmt * st = NULL;
    if (sqlite3_prepare_v2(db, query, -1, &st, NULL) != SQLITE_OK) return;
    for (int i = 0; i < count; i++) {
        sqlite3_bind_int64(st, 1, ids[i]);
        fill_song_rows(st, 0, 1, 2, 3, 4, 5, 6, &out_rows[i], 1);
        sqlite3_reset(st);
        sqlite3_clear_bindings(st);
    }
    sqlite3_finalize(st);
}

void metadata_db_get_songs_by_paths(const char * const * paths, int count, song_row_t * out_rows) {
    METADATA_DB_GUARD;
    for (int i = 0; i < count; i++) out_rows[i].id = -1;
    if (!db || count <= 0) return;

    static const char * const query =
        "SELECT rowid, path, title, artist, album, album_artist, genre FROM media WHERE path = ?;";
    sqlite3_stmt * st = NULL;
    if (sqlite3_prepare_v2(db, query, -1, &st, NULL) != SQLITE_OK) return;
    for (int i = 0; i < count; i++) {
        sqlite3_bind_text(st, 1, paths[i], -1, SQLITE_STATIC);
        fill_song_rows(st, 0, 1, 2, 3, 4, 5, 6, &out_rows[i], 1);
        sqlite3_reset(st);
        sqlite3_clear_bindings(st);
    }
    sqlite3_finalize(st);
}

int64_t metadata_db_get_song_title_offset(const char * path) {
    METADATA_DB_GUARD;
    if (!db || !path) return -1;
    static const char * const query =
        "SELECT COUNT(*) FROM media AS preceding, media AS target "
        "WHERE target.path = ? AND (preceding.title COLLATE NOCASE < target.title COLLATE NOCASE "
        "OR (preceding.title COLLATE NOCASE = target.title COLLATE NOCASE AND preceding.rowid < target.rowid));";
    sqlite3_stmt * st = NULL;
    if (sqlite3_prepare_v2(db, query, -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(st, 1, path, -1, SQLITE_STATIC);
    int64_t offset = -1;
    if (sqlite3_step(st) == SQLITE_ROW) offset = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return offset;
}

/* Same role as metadata_db_get_song_title_offset() above, but resolving a
 * position in the Recently Added screen's own order (first_seen DESC, rowid
 * DESC tiebreak -- matching metadata_db_get_songs_page_by_recency()'s own
 * ORDER BY exactly, so a "now playing" offset from here always lands on the
 * right row of that screen's paged provider) rather than title order. */
int64_t metadata_db_get_song_recency_offset(const char * path) {
    METADATA_DB_GUARD;
    if (!db || !path) return -1;
    static const char * const query =
        "SELECT COUNT(*) FROM media AS preceding, media AS target "
        "WHERE target.path = ? AND (preceding.first_seen > target.first_seen "
        "OR (preceding.first_seen = target.first_seen AND preceding.rowid > target.rowid));";
    sqlite3_stmt * st = NULL;
    if (sqlite3_prepare_v2(db, query, -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(st, 1, path, -1, SQLITE_STATIC);
    int64_t offset = -1;
    if (sqlite3_step(st) == SQLITE_ROW) offset = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return offset;
}

int64_t metadata_db_get_group_offset(metadata_db_group_kind_t kind, const char * name, const char * album_artist) {
    METADATA_DB_GUARD;
    if (!db || !name) return -1;
    sqlite3_stmt * st = NULL;
    if (kind == METADATA_DB_GROUP_ALBUM) {
        static const char * const query =
            "SELECT COUNT(*) FROM (SELECT 1 FROM media WHERE album COLLATE NOCASE < ? "
            "OR (album COLLATE NOCASE = ? AND album_artist COLLATE NOCASE < ?) "
            "GROUP BY album COLLATE NOCASE, album_artist COLLATE NOCASE);";
        if (sqlite3_prepare_v2(db, query, -1, &st, NULL) != SQLITE_OK) return -1;
        sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 2, name, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 3, album_artist ? album_artist : "", -1, SQLITE_STATIC);
    } else {
        const char * column = kind == METADATA_DB_GROUP_ALBUM_ARTIST ? "album_artist" : "artist";
        char query[256];
        snprintf(query, sizeof(query),
                 "SELECT COUNT(*) FROM (SELECT 1 FROM media WHERE %s COLLATE NOCASE < ? "
                 "GROUP BY %s COLLATE NOCASE);", column, column);
        if (sqlite3_prepare_v2(db, query, -1, &st, NULL) != SQLITE_OK) return -1;
        sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
    }
    int64_t offset = -1;
    if (sqlite3_step(st) == SQLITE_ROW) offset = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return offset;
}

void metadata_db_get_az_table(metadata_db_az_kind_t kind, int out_table[27]) {
    METADATA_DB_GUARD;
    for (int i = 0; i < 27; i++) out_table[i] = -1;
    if (!db) return;

    /* sort_col is the column each screen's own query leads its ORDER BY
     * with; group_cols is NULL for the ungrouped All Songs screen (plain
     * per-row COUNT(*)) or the exact GROUP BY column list the grouped
     * screens' own metadata_db_get_groups_page()/get_albums_page_filtered()
     * use, so a boundary count here lines up with what's actually displayed
     * at that offset. Album is keyed on the (album, album_artist) pair, not
     * album name alone -- same identity fix as get_albums_page_filtered(). */
    const char * sort_col = "title";
    const char * group_cols = NULL;
    switch (kind) {
        case METADATA_DB_AZ_ARTIST: sort_col = "artist"; group_cols = "artist COLLATE NOCASE"; break;
        case METADATA_DB_AZ_ALBUM_ARTIST:
            sort_col = "album_artist";
            group_cols = "album_artist COLLATE NOCASE";
            break;
        case METADATA_DB_AZ_ALBUM:
            sort_col = "album";
            group_cols = "album COLLATE NOCASE, album_artist COLLATE NOCASE";
            break;
        case METADATA_DB_AZ_ALL_SONGS: sort_col = "title"; group_cols = NULL; break;
    }

    /* Efficiency finding: this used to run 26 separate `WHERE sort_col <
     * 'X'` boundary-count queries (one per letter) plus a 27th total-count
     * query -- letter 'Y' rescans almost everything letter 'X' already
     * scanned, roughly O(13n) row visits instead of O(n). A single ordered
     * (grouped, for the grouped kinds) pass gets the same per-letter
     * boundary positions from one sequential scan: stream through every
     * row/group in sort_col order, and the position at which each letter
     * is FIRST seen is exactly the count of rows/groups strictly before
     * it -- the same value the old per-letter COUNT(*) query computed,
     * just derived from one pass instead of 26. The running position after
     * the last row IS the total, so the separate total-count query (and
     * the clamp that used to depend on it) isn't needed either -- a letter
     * genuinely absent from the library just never gets touched here and
     * correctly stays at its initial -1. */
    char query[384];
    if (group_cols) {
        snprintf(query, sizeof(query), "SELECT %s FROM media GROUP BY %s ORDER BY %s COLLATE NOCASE;", sort_col,
                  group_cols, sort_col);
    } else {
        snprintf(query, sizeof(query), "SELECT %s FROM media ORDER BY %s COLLATE NOCASE;", sort_col, sort_col);
    }
    sqlite3_stmt * st = NULL;
    if (sqlite3_prepare_v2(db, query, -1, &st, NULL) != SQLITE_OK) return;

    bool seen[26] = { false };
    int position = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char * text = sqlite3_column_text(st, 0);
        if (text && text[0]) {
            unsigned char c = text[0];
            if (c >= 'a' && c <= 'z') c = (unsigned char) (c - ('a' - 'A'));
            if (c >= 'A' && c <= 'Z' && !seen[c - 'A']) {
                seen[c - 'A'] = true;
                out_table[c - 'A'] = position;
            }
        }
        position++;
    }
    sqlite3_finalize(st);

    /* A letter genuinely absent from the library (no row/group starts with
     * it) must still jump to the nearest FOLLOWING letter that has content,
     * matching the old per-letter "count of items strictly less than X"
     * query's own natural behavior (an empty 'B' bucket's boundary count
     * equals 'C''s, whenever nothing starts with exactly 'B') -- verified
     * against this app's own real on-device library.db that this backward
     * fill reproduces the old query's output exactly. Only a genuinely
     * empty trailing run (nothing at or after that letter, all the way to
     * 'Z') stays -1, which happens naturally here since there's nothing
     * for 'Z' itself to inherit from. */
    for (int i = 24; i >= 0; i--) {
        if (out_table[i] == -1) out_table[i] = out_table[i + 1];
    }

    /* '#' bucket: SQLite's default (post-NOCASE-folding) comparison is
     * plain ASCII byte order, where every digit/symbol code point sorts
     * strictly before every letter -- so anything not starting with A-Z
     * clusters at the very front of the sorted list (offset 0) in the
     * overwhelmingly common case, never interspersed among lettered
     * entries the way an arbitrary Unicode collation might allow. A name
     * starting with an accented or non-Latin character sorts *after* 'Z'
     * instead (untested edge case, not reproduced in this library) and
     * lands in the 'Z' bucket rather than here -- same divergence class
     * already accepted for the title fallback elsewhere in this file, not
     * worth a second query shape for. Read AFTER the backward fill above
     * (not the raw, possibly-still--1 position 'A' itself was set to) --
     * otherwise a library with '#' content but nothing starting with 'A'
     * specifically would miss its own '#' bucket entirely. */
    out_table[26] = out_table[0] > 0 ? 0 : -1;
}

int metadata_db_search_names(metadata_db_az_kind_t kind, const char * needle, int max_rows,
                              metadata_db_search_hit_t * out_hits) {
    METADATA_DB_GUARD;
    if (!db || max_rows <= 0 || !needle || !needle[0]) return 0;

    /* Same escaping approach as metadata_db_search_songs() -- see its own
     * comment. */
    char escaped[256];
    size_t out_len = 0;
    for (const char * p = needle; *p && out_len + 2 < sizeof(escaped) - 2; p++) {
        if (*p == '%' || *p == '_' || *p == '\\') escaped[out_len++] = '\\';
        escaped[out_len++] = *p;
    }
    escaped[out_len] = '\0';
    char pattern[260];
    snprintf(pattern, sizeof(pattern), "%%%s%%", escaped);

    char query[512];
    bool is_song = (kind == METADATA_DB_AZ_ALL_SONGS);
    switch (kind) {
        case METADATA_DB_AZ_ARTIST:
            snprintf(query, sizeof(query),
                      "SELECT artist, offs FROM "
                      "(SELECT artist, ROW_NUMBER() OVER (ORDER BY artist COLLATE NOCASE) - 1 AS offs "
                      " FROM media GROUP BY artist COLLATE NOCASE) "
                      "WHERE artist LIKE ? ESCAPE '\\' ORDER BY offs LIMIT ?;");
            break;
        case METADATA_DB_AZ_ALBUM_ARTIST:
            snprintf(query, sizeof(query),
                      "SELECT album_artist, offs FROM "
                      "(SELECT album_artist, ROW_NUMBER() OVER (ORDER BY album_artist COLLATE NOCASE) - 1 AS offs "
                      " FROM media GROUP BY album_artist COLLATE NOCASE) "
                      "WHERE album_artist LIKE ? ESCAPE '\\' ORDER BY offs LIMIT ?;");
            break;
        case METADATA_DB_AZ_ALBUM:
            snprintf(query, sizeof(query),
                      "SELECT album, offs FROM "
                      "(SELECT album, ROW_NUMBER() OVER (ORDER BY album COLLATE NOCASE, album_artist COLLATE NOCASE) - 1 AS offs "
                      " FROM media GROUP BY album COLLATE NOCASE, album_artist COLLATE NOCASE) "
                      "WHERE album LIKE ? ESCAPE '\\' ORDER BY offs LIMIT ?;");
            break;
        case METADATA_DB_AZ_ALL_SONGS:
            snprintf(query, sizeof(query),
                      "SELECT rowid, path, title, offs FROM "
                      "(SELECT rowid, path, title, ROW_NUMBER() OVER (ORDER BY title COLLATE NOCASE, rowid) - 1 AS offs "
                      " FROM media) "
                      "WHERE title LIKE ? ESCAPE '\\' ORDER BY offs LIMIT ?;");
            break;
        default: return 0;
    }

    sqlite3_stmt * st = NULL;
    if (sqlite3_prepare_v2(db, query, -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(st, 1, pattern, -1, SQLITE_STATIC);
    sqlite3_bind_int(st, 2, max_rows);

    int i = 0;
    while (i < max_rows && sqlite3_step(st) == SQLITE_ROW) {
        if (is_song) {
            song_row_t row = { 0 };
            row.id = sqlite3_column_int64(st, 0);
            snprintf(row.path, sizeof(row.path), "%s", col_text(st, 1));
            snprintf(row.tags.title, sizeof(row.tags.title), "%s", col_text(st, 2));
            metadata_db_song_display_title(&row, out_hits[i].label, sizeof(out_hits[i].label));
            out_hits[i].offset = sqlite3_column_int(st, 3);
        } else {
            snprintf(out_hits[i].label, sizeof(out_hits[i].label), "%s", col_text(st, 0));
            out_hits[i].offset = sqlite3_column_int(st, 1);
        }
        i++;
    }
    sqlite3_finalize(st);
    return i;
}

/* Real bug caught in review: every song_row_t consumer outside gui.c
 * (remote_control.c's JSON responses, plugin.library_*) was emitting
 * row->tags.title verbatim -- blank for any untagged file (confirmed live
 * against this device's own library: several WAV tracks with no embedded
 * title tag). gui.c's own on-device list screens never show this, because
 * song_display_title_of() (gui.c) already falls back to the file's own
 * basename when title is empty -- this is that same fallback, shared here
 * so every song_row_t consumer gets it instead of each reimplementing (or
 * forgetting) it. Basename keeps its extension (e.g. "Track 09.wav"),
 * matching song_display_title_of()'s own fallback exactly. */
void metadata_db_song_display_title(const song_row_t * row, char * out, size_t out_size) {
    if (row->tags.title[0] != '\0') {
        snprintf(out, out_size, "%s", row->tags.title);
        return;
    }
    const char * slash = strrchr(row->path, '/');
    snprintf(out, out_size, "%s", slash ? slash + 1 : row->path);
}

int metadata_db_search_songs(const char * query_text, song_row_t * out_rows, int max_rows) {
    METADATA_DB_GUARD;
    if (!db || max_rows <= 0 || !query_text || !query_text[0]) return 0;

    static const char * const query =
        "SELECT rowid, path, title, artist, album, album_artist, genre FROM media "
        "WHERE title LIKE ? ESCAPE '\\' OR artist LIKE ? ESCAPE '\\' "
        "ORDER BY title COLLATE NOCASE LIMIT ?;";
    sqlite3_stmt * st = NULL;
    if (sqlite3_prepare_v2(db, query, -1, &st, NULL) != SQLITE_OK) return 0;

    /* Escape the three LIKE metacharacters in the user's own query text
     * before wrapping it in %...% -- otherwise a search for e.g. "50%" would
     * silently become a wildcard instead of a literal percent sign. */
    char escaped[256];
    size_t out_len = 0;
    for (const char * p = query_text; *p && out_len + 2 < sizeof(escaped) - 2; p++) {
        if (*p == '%' || *p == '_' || *p == '\\') escaped[out_len++] = '\\';
        escaped[out_len++] = *p;
    }
    escaped[out_len] = '\0';

    char pattern[260];
    snprintf(pattern, sizeof(pattern), "%%%s%%", escaped);
    sqlite3_bind_text(st, 1, pattern, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, pattern, -1, SQLITE_STATIC);
    sqlite3_bind_int(st, 3, max_rows);
    int count = fill_song_rows(st, 0, 1, 2, 3, 4, 5, 6, out_rows, max_rows);
    sqlite3_finalize(st);
    return count;
}

/* Deliberate full-library load -- everything above (metadata_db_get_songs_
 * page() etc.) exists specifically so gui.c's boot path and the four core
 * list screens never need this, but a few lower-traffic consumers (Search's
 * own filter-the-whole-library fallback, Shuffle All, remote-control sync)
 * still work against one big resident array for now rather than being
 * converted to their own paged/on-demand queries yet -- see this session's
 * own scoping notes on why that conversion is sequenced later, not skipped.
 * Called lazily, once, on first actual need (gui.c's ensure_library_arrays_
 * loaded()), not unconditionally at boot -- that's the actual fix for the
 * boot-time OOM this whole rework exists to close. *out_paths and *out_tags
 * are freshly malloc'd parallel arrays the caller owns (including each
 * out_paths[i] string); both are set to NULL and *out_count to 0 if the
 * cache is empty or unopened. */
void metadata_db_load_all(char *** out_paths, cached_tags_t ** out_tags, int * out_count) {
    METADATA_DB_GUARD;
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

    char ** paths = calloc((size_t) row_count, sizeof(*paths));
    cached_tags_t * tags = malloc(sizeof(*tags) * (size_t) row_count);
    if (!paths || !tags) {
        free(paths);
        free(tags);
        return;
    }

    if (sqlite3_prepare_v2(db, "SELECT path, title, artist, album, album_artist, genre FROM media;", -1, &st, NULL) !=
        SQLITE_OK) {
        free(paths);
        free(tags);
        return;
    }

    int i = 0;
    while (i < row_count && sqlite3_step(st) == SQLITE_ROW) {
        paths[i] = strdup(col_text(st, 0));
        if (!paths[i]) break;
        snprintf(tags[i].title, sizeof(tags[i].title), "%s", col_text(st, 1));
        snprintf(tags[i].artist, sizeof(tags[i].artist), "%s", col_text(st, 2));
        snprintf(tags[i].album, sizeof(tags[i].album), "%s", col_text(st, 3));
        snprintf(tags[i].album_artist, sizeof(tags[i].album_artist), "%s", col_text(st, 4));
        snprintf(tags[i].genre, sizeof(tags[i].genre), "%s", col_text(st, 5));
        i++;
    }
    sqlite3_finalize(st);

    if (i != row_count) {
        for (int j = 0; j < i; j++) free(paths[j]);
        free(paths);
        free(tags);
        return;
    }

    *out_paths = paths;
    *out_tags = tags;
    *out_count = i;
}

void metadata_db_book_replace_all(char * const * paths, int count) {
    METADATA_DB_GUARD;
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
    METADATA_DB_GUARD;
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
        paths[i] = strdup(col_text(st, 0));
        i++;
    }
    sqlite3_finalize(st);

    *out_paths = paths;
    *out_count = i;
}

void metadata_db_playlist_replace_all(char * const * paths, int count) {
    METADATA_DB_GUARD;
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
    METADATA_DB_GUARD;
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
        paths[i] = strdup(col_text(st, 0));
        i++;
    }
    sqlite3_finalize(st);

    *out_paths = paths;
    *out_count = i;
}

void metadata_db_playlist_insert_one(const char * path) {
    METADATA_DB_GUARD;
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
    METADATA_DB_GUARD;
    if (!db) return;

    sqlite3_stmt * st = NULL;
    if (sqlite3_prepare_v2(db, "DELETE FROM playlist WHERE path = ?;", -1, &st, NULL) != SQLITE_OK) return;
    sqlite3_bind_text(st, 1, path, -1, SQLITE_STATIC);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

bool metadata_db_book_favorite_is_set(const char * path) {
    METADATA_DB_GUARD;
    if (!db) return false;

    sqlite3_stmt * st = NULL;
    if (sqlite3_prepare_v2(db, "SELECT 1 FROM book_favorite WHERE path = ?;", -1, &st, NULL) != SQLITE_OK) return false;
    sqlite3_bind_text(st, 1, path, -1, SQLITE_STATIC);
    bool is_set = sqlite3_step(st) == SQLITE_ROW;
    sqlite3_finalize(st);
    return is_set;
}

void metadata_db_book_favorite_set(const char * path, bool is_favorite) {
    METADATA_DB_GUARD;
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
    METADATA_DB_GUARD;
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
        paths[i] = strdup(col_text(st, 0));
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
    METADATA_DB_GUARD;
    if (!db) return false;

    sqlite3_stmt * st = NULL;
    if (sqlite3_prepare_v2(db, "SELECT 1 FROM song_favorite WHERE path = ?;", -1, &st, NULL) != SQLITE_OK) return false;
    sqlite3_bind_text(st, 1, path, -1, SQLITE_STATIC);
    bool is_set = sqlite3_step(st) == SQLITE_ROW;
    sqlite3_finalize(st);
    return is_set;
}

void metadata_db_song_favorite_set(const char * path, bool is_favorite) {
    METADATA_DB_GUARD;
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
    METADATA_DB_GUARD;
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
        paths[i] = strdup(col_text(st, 0));
        i++;
    }
    sqlite3_finalize(st);

    *out_paths = paths;
    *out_count = i;
}

void metadata_db_song_play_count_increment(const char * path) {
    METADATA_DB_GUARD;
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
    METADATA_DB_GUARD;
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
        paths[i] = strdup(col_text(st, 0));
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

void metadata_db_load_recently_added_songs(int limit, char *** out_paths, int * out_count) {
    METADATA_DB_GUARD;
    *out_paths = NULL;
    *out_count = 0;
    if (!db || limit <= 0) return;

    sqlite3_stmt * st = NULL;
    /* No INNER JOIN needed, unlike metadata_db_load_favorite_songs()/
     * metadata_db_load_top_played_songs() above -- this queries `media`
     * directly (first_seen lives there, not a separate side table), so
     * there's no separate table that could reference a since-removed path
     * to filter staleness against in the first place. path COLLATE NOCASE
     * as the tiebreaker for two rows sharing the same first_seen value
     * (e.g. a whole album's worth of files scanned in the same pass) gives
     * a stable, deterministic order rather than an unspecified one. */
    static const char * const query =
        "SELECT path FROM media ORDER BY first_seen DESC, path COLLATE NOCASE LIMIT ?;";
    if (sqlite3_prepare_v2(db, query, -1, &st, NULL) != SQLITE_OK) return;
    sqlite3_bind_int(st, 1, limit);

    char ** paths = malloc(sizeof(char *) * (size_t) limit);
    int i = 0;
    while (i < limit && sqlite3_step(st) == SQLITE_ROW) {
        paths[i] = strdup(col_text(st, 0));
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
    METADATA_DB_GUARD;
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
    METADATA_DB_GUARD;
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
        snprintf(rows[i].url, sizeof(rows[i].url), "%s", col_text(st, 0));
        snprintf(rows[i].username, sizeof(rows[i].username), "%s", col_text(st, 1));
        snprintf(rows[i].password, sizeof(rows[i].password), "%s", col_text(st, 2));
        rows[i].verify_tls = sqlite3_column_int(st, 3) != 0;
        i++;
    }
    sqlite3_finalize(st);

    *out_rows = rows;
    *out_count = i;
}
