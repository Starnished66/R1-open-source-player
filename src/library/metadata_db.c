#include "metadata_db.h"

#include "path_cache.h"
#include "remote_state.h"
#include "subsonic_saved_servers.h"
#include "tagcache.h"
#include "fallback_font.h"
#include "library_endian.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifdef HOST_BUILD
  #define METADATA_DB_ROOT "."
#else
  #define METADATA_DB_ROOT "/data/mnt/sd_0"
#endif
#define METADATA_DB_DIR METADATA_DB_ROOT "/.compas"
#define METADATA_DB_OLD_DIR METADATA_DB_ROOT "/.open_hiby_player"
#define MIGRATION_PENDING METADATA_DB_DIR "/migration.pending"
#define MIGRATION_COMPLETE METADATA_DB_DIR "/migration.complete"
#define CATALOG_INCARNATION_NAME "catalog.incarnation"
#define CATALOG_INCARNATION_TEMP_NAME "catalog.incarnation.tmp"

static bool db_ready;
static int migration_db_fd = -1;
static int migration_root_fd = -1;
static int migration_old_fd = -1;
static bool migration_pending;
static bool migration_cleanup;
static bool migration_archive_retained;
static bool migration_replayed;
static bool migration_applied;
static size_t migration_unmatched;
static tagcache_stats_snapshot_t * migration_stats;
static char catalog_library_id[METADATA_DB_CATALOG_LIBRARY_ID_SIZE];
static bool catalog_library_id_ready;
static bool catalog_library_id_force_rotation;
/* A database rebuilt after corruption is a provisional, empty tagcache until
 * its replacement generation commits. Never let a catalog client publish
 * that transient state under the old library identity. */
static bool catalog_rebuild_pending;
static pthread_once_t metadata_db_mutex_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t metadata_db_mutex;
static pthread_mutex_t metadata_update_mutex;
static _Thread_local unsigned metadata_query_depth;

static void metadata_db_mutex_init(void) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&metadata_db_mutex, &attr);
    pthread_mutex_init(&metadata_update_mutex, &attr);
    pthread_mutexattr_destroy(&attr);
}

typedef struct { bool locked; } metadata_db_guard_t;
static void metadata_db_guard_release(metadata_db_guard_t * guard) {
    if (guard->locked) { metadata_query_depth--; pthread_mutex_unlock(&metadata_db_mutex); }
}
#define METADATA_DB_GUARD \
    pthread_once(&metadata_db_mutex_once, metadata_db_mutex_init); \
    pthread_mutex_lock(&metadata_db_mutex); \
    metadata_query_depth++; \
    metadata_db_guard_t metadata_db_guard __attribute__((cleanup(metadata_db_guard_release))) = { true }

static void metadata_update_guard_release(metadata_db_guard_t *guard) {
    if (guard->locked) pthread_mutex_unlock(&metadata_update_mutex);
}
#define METADATA_UPDATE_GUARD \
    pthread_once(&metadata_db_mutex_once, metadata_db_mutex_init); \
    pthread_mutex_lock(&metadata_update_mutex); \
    metadata_db_guard_t metadata_update_guard __attribute__((cleanup(metadata_update_guard_release))) = { true }

static void metadata_query_unlock(void) {
    for (unsigned i = 0; i < metadata_query_depth; i++) pthread_mutex_unlock(&metadata_db_mutex);
}
static void metadata_query_lock(void) {
    for (unsigned i = 0; i < metadata_query_depth; i++) pthread_mutex_lock(&metadata_db_mutex);
}

static bool database_file_name(const char * name) {
    return strcmp(name, "tagcache.gen") == 0 ||
           (strncmp(name, "database_", 9) == 0 && strstr(name + 9, ".tcd") != NULL);
}

/* Returns -1 on an inspection error, 0 when absent, and 1 when present. */
static int database_files_present_fd(int fd) {
    if (fd < 0) return 0;
    int scan_fd = openat(fd, ".", O_RDONLY | O_DIRECTORY);
    if (scan_fd < 0) return errno == ENOENT ? 0 : -1;
    DIR * d = fdopendir(scan_fd);
    if (!d) { close(scan_fd); return -1; }
    int result = 0;
    for (;;) {
        errno = 0;
        struct dirent * de = readdir(d);
        if (!de) {
            if (errno) result = -1;
            break;
        }
        if (database_file_name(de->d_name)) result = 1;
    }
    if (closedir(d) != 0) result = -1;
    return result;
}

static int migration_marker_present_fd(const char *path) {
    if (migration_db_fd < 0) return -1;
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    struct stat st;
    if (fstatat(migration_db_fd, name, &st, AT_SYMLINK_NOFOLLOW) != 0)
        return errno == ENOENT ? 0 : -1;
    return S_ISREG(st.st_mode) ? 1 : -1;
}

static bool migration_pin_dirs(void) {
    if (migration_db_fd >= 0) close(migration_db_fd);
    if (migration_root_fd >= 0) close(migration_root_fd);
    if (migration_old_fd >= 0) close(migration_old_fd);
    migration_db_fd = tagcache_dup_directory_fd();
    migration_root_fd = migration_db_fd >= 0 ? openat(migration_db_fd, "..", O_RDONLY | O_DIRECTORY) : -1;
    migration_old_fd = migration_root_fd >= 0 ? openat(migration_root_fd, ".open_hiby_player", O_RDONLY | O_DIRECTORY) : -1;
    if (migration_db_fd < 0 || migration_root_fd < 0) return false;
    if (migration_old_fd < 0 && errno != ENOENT) return false;
    return true;
}

static void migration_unpin_dirs(void) {
    if (migration_old_fd >= 0) close(migration_old_fd);
    if (migration_root_fd >= 0) close(migration_root_fd);
    if (migration_db_fd >= 0) close(migration_db_fd);
    migration_old_fd = migration_root_fd = migration_db_fd = -1;
}

static bool migration_detect(void) {
    migration_pending = migration_cleanup = migration_archive_retained = migration_applied = false;
    migration_replayed = false;
    uint32_t stored_state = tagcache_migration_state();
    if (stored_state & TAGCACHE_MIGRATION_APPLIED) {
        migration_applied = true;
        migration_archive_retained = (stored_state & TAGCACHE_MIGRATION_ARCHIVE) != 0;
        return true;
    }
    int complete = migration_marker_present_fd(MIGRATION_COMPLETE);
    int pending = migration_marker_present_fd(MIGRATION_PENDING);
    if (complete < 0 || pending < 0) return false;
    if (complete) {
        int old = database_files_present_fd(migration_old_fd);
        if (old < 0) return false;
        migration_archive_retained = old != 0;
        migration_cleanup = false;
        return true;
    }
    if (pending) {
        migration_pending = true;
        return true;
    }
    int current = database_files_present_fd(migration_db_fd);
    if (current < 0) return false;
    if (current) return true;
    int old = database_files_present_fd(migration_old_fd);
    if (old < 0) return false;
    migration_pending = old != 0;
    return true;
}

static bool migration_write_marker(const char * path) {
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    int fd = openat(migration_db_fd, name, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) {
        if (errno != EEXIST || migration_marker_present_fd(path) != 1) return false;
        fd = openat(migration_db_fd, name, O_RDONLY);
        if (fd < 0) return false;
    }
    bool ok = fsync(fd) == 0;
    if (close(fd) != 0) ok = false;
    return ok && fsync(migration_db_fd) == 0 && fsync(migration_root_fd) == 0;
}

bool metadata_db_migration_needed(void) {
    METADATA_DB_GUARD;
    return migration_pending || migration_cleanup;
}

bool metadata_db_migration_cleanup_pending(void) {
    METADATA_DB_GUARD;
    return migration_cleanup;
}

bool metadata_db_migration_archive_retained(void) {
    METADATA_DB_GUARD;
    return migration_archive_retained;
}

void metadata_db_migration_cancel(void) {
    METADATA_UPDATE_GUARD;
    METADATA_DB_GUARD;
    tagcache_free_stats(migration_stats);
    migration_stats = NULL;
    migration_replayed = false;
    migration_unmatched = 0;
}

bool metadata_db_migration_prepare(void) {
    METADATA_UPDATE_GUARD;
    METADATA_DB_GUARD;
    metadata_db_migration_cancel();
    if (!db_ready) return false;
    if (!migration_pending) return true;
    if (migration_old_fd < 0) return false;
    char old_dir[64];
    snprintf(old_dir, sizeof(old_dir), "/proc/self/fd/%d", migration_old_fd);
    if (!tagcache_extract_stats(old_dir, &migration_stats)) return false;
    if (!migration_write_marker(MIGRATION_PENDING)) {
        metadata_db_migration_cancel();
        return false;
    }
    return true;
}

bool metadata_db_migration_finish(void) {
    METADATA_UPDATE_GUARD;
    METADATA_DB_GUARD;
    if (!db_ready || metadata_db_get_load_outcome() != METADATA_DB_LOAD_SUCCESS_NORMAL) return false;
    if (migration_pending) {
        /* Replay occurs in metadata_db_end_update(), before the first
         * generation pointer publication.  If finish is called before that
         * boundary, leave the pending migration untouched. */
        if (!migration_replayed || !migration_applied) return false;
        if (!migration_write_marker(MIGRATION_COMPLETE)) {
            /* The new generation and its applied state are already durable;
             * completion is cleanup bookkeeping and must not report the old
             * database as still active. */
            migration_pending = false;
            migration_cleanup = true;
            migration_archive_retained = true;
            tagcache_free_stats(migration_stats);
            migration_stats = NULL;
            return true;
        }
        migration_pending = false;
        migration_cleanup = false;
        migration_archive_retained = true;
        metadata_db_migration_cancel();
    }
    if (migration_cleanup) {
        if (!migration_write_marker(MIGRATION_COMPLETE)) return true;
        migration_cleanup = false;
        migration_archive_retained = true;
        tagcache_free_stats(migration_stats);
        migration_stats = NULL;
    }
    if (migration_applied && !migration_pending && migration_marker_present_fd(MIGRATION_COMPLETE) == 0)
        migration_write_marker(MIGRATION_COMPLETE);
    return true;
}

#ifndef HOST_BUILD
static bool music_root_is_mounted(void) {
    struct stat parent_st, root_st;
    if (stat("/data/mnt", &parent_st) != 0) return false;
    if (stat("/data/mnt/sd_0", &root_st) != 0) return false;
    return parent_st.st_dev != root_st.st_dev;
}
#endif

static int cmp_path_ptr(const void * a, const void * b) {
    const char * const * pa = a;
    const char * const * pb = b;
    return tagcache_cmp_ascii(*pa, *pb);
}

static int32_t clamp_i32(int64_t v) {
    if (v > INT32_MAX) return INT32_MAX;
    if (v < 0) return 0;
    return (int32_t) v;
}

typedef struct {
    int32_t id;
    int32_t count;
    int32_t last_played;
    int32_t ordinal;
} played_hit_t;

static int cmp_hit_played(const void * a, const void * b) {
    const played_hit_t * ha = a;
    const played_hit_t * hb = b;
    if (ha->count != hb->count) return ha->count < hb->count ? 1 : -1;
    if (ha->last_played != hb->last_played) return ha->last_played < hb->last_played ? 1 : -1;
    /* Equal play counts and timestamps are ordered by ascending stable song ID. */
    if (ha->ordinal != hb->ordinal) return ha->ordinal < hb->ordinal ? -1 : 1;
    return 0;
}

static void played_hit_heap_sift_up(played_hit_t * heap, int index) {
    while (index > 0) {
        int parent = (index - 1) / 2;
        if (cmp_hit_played(&heap[parent], &heap[index]) >= 0) break;
        played_hit_t swap = heap[parent];
        heap[parent] = heap[index];
        heap[index] = swap;
        index = parent;
    }
}

static void played_hit_heap_sift_down(played_hit_t * heap, int count, int index) {
    for (;;) {
        if (count < 2 || index > (count - 2) / 2) return;
        int child = index * 2 + 1;
        if (child + 1 < count && cmp_hit_played(&heap[child], &heap[child + 1]) < 0) child++;
        if (cmp_hit_played(&heap[index], &heap[child]) >= 0) return;
        played_hit_t swap = heap[index];
        heap[index] = heap[child];
        heap[child] = swap;
        index = child;
    }
}

static void copy_song(const tagcache_song_t * src, song_row_t * dst) {
    dst->id = src->id;
    snprintf(dst->path, sizeof(dst->path), "%s", src->path);
    snprintf(dst->tags.title, sizeof(dst->tags.title), "%s", src->title);
    snprintf(dst->tags.artist, sizeof(dst->tags.artist), "%s", src->artist);
    snprintf(dst->tags.album, sizeof(dst->tags.album), "%s", src->album);
    snprintf(dst->tags.album_artist, sizeof(dst->tags.album_artist), "%s", src->album_artist);
    snprintf(dst->tags.genre, sizeof(dst->tags.genre), "%s", src->genre);
    dst->tags.track_number = src->track_number;
    dst->tags.disc_number = src->disc_number;
}

static void copy_group(const tagcache_group_t * src, group_row_t * dst) {
    snprintf(dst->name, sizeof(dst->name), "%s", src->name ? src->name : "");
    snprintf(dst->album_artist, sizeof(dst->album_artist), "%s", src->album_artist ? src->album_artist : "");
    dst->song_count = src->song_count;
    dst->first_song_id = src->first_song_id;
}

static int az_kind_to_group(metadata_db_az_kind_t kind) {
    switch (kind) {
        case METADATA_DB_AZ_ARTIST: return TAGCACHE_GROUP_ARTIST;
        case METADATA_DB_AZ_ALBUM_ARTIST: return TAGCACHE_GROUP_ALBUM_ARTIST;
        case METADATA_DB_AZ_ALBUM: return TAGCACHE_GROUP_ALBUM;
        default: return -1;
    }
}

static metadata_db_load_outcome_t last_outcome = METADATA_DB_LOAD_UNMOUNTED;

typedef enum {
    CATALOG_ID_READ_ERROR = -1,
    CATALOG_ID_MISSING = 0,
    CATALOG_ID_INVALID = 1,
    CATALOG_ID_VALID = 2
} catalog_id_read_result_t;

static bool catalog_id_format_valid(char id[METADATA_DB_CATALOG_LIBRARY_ID_SIZE]) {
    if (strlen(id) != METADATA_DB_CATALOG_LIBRARY_ID_SIZE - 1) return false;
    for (size_t i = 0; i < METADATA_DB_CATALOG_LIBRARY_ID_SIZE - 1; i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (id[i] != '-') return false;
            continue;
        }
        unsigned char c = (unsigned char) id[i];
        if (c >= 'A' && c <= 'F') id[i] = (char) (c + ('a' - 'A'));
        else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

static catalog_id_read_result_t catalog_read_id(char out[METADATA_DB_CATALOG_LIBRARY_ID_SIZE]) {
    if (migration_db_fd < 0) return CATALOG_ID_READ_ERROR;
    int fd = openat(migration_db_fd, CATALOG_INCARNATION_NAME,
                    O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return errno == ENOENT ? CATALOG_ID_MISSING : CATALOG_ID_READ_ERROR;

    struct stat st;
    bool ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) &&
              (st.st_size == 36 || st.st_size == 37);
    char id[METADATA_DB_CATALOG_LIBRARY_ID_SIZE] = {0};
    size_t needed = ok ? (size_t) st.st_size : 0;
    size_t received = 0;
    while (ok && received < needed) {
        ssize_t n = read(fd, id + received, needed - received);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { ok = false; break; }
        received += (size_t) n;
    }
    if (close(fd) != 0) ok = false;
    if (!ok) return CATALOG_ID_READ_ERROR;
    if (needed == 37) {
        if (id[36] != '\n') return CATALOG_ID_INVALID;
        id[36] = '\0';
    }
    if (!catalog_id_format_valid(id)) return CATALOG_ID_INVALID;
    memcpy(out, id, sizeof(id));
    return CATALOG_ID_VALID;
}

static bool catalog_write_id(const char * id) {
    if (migration_db_fd < 0) return false;
    int fd = openat(migration_db_fd, CATALOG_INCARNATION_TEMP_NAME,
                    O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return false;
    size_t length = strlen(id);
    size_t written = 0;
    bool ok = true;
    while (written < length) {
        ssize_t n = write(fd, id + written, length - written);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { ok = false; break; }
        written += (size_t) n;
    }
    if (ok && fsync(fd) != 0) ok = false;
    if (close(fd) != 0) ok = false;
    if (ok && renameat(migration_db_fd, CATALOG_INCARNATION_TEMP_NAME,
                       migration_db_fd, CATALOG_INCARNATION_NAME) != 0) ok = false;
    if (ok && fsync(migration_db_fd) != 0) ok = false;
    if (!ok) unlinkat(migration_db_fd, CATALOG_INCARNATION_TEMP_NAME, 0);
    return ok;
}

static bool catalog_generate_id(char out[METADATA_DB_CATALOG_LIBRARY_ID_SIZE]) {
    uint8_t bytes[16];
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    size_t received = 0;
    while (received < sizeof(bytes)) {
        ssize_t n = read(fd, bytes + received, sizeof(bytes) - received);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { close(fd); return false; }
        received += (size_t) n;
    }
    if (close(fd) != 0) return false;
    bytes[6] = (uint8_t) ((bytes[6] & 0x0f) | 0x40); /* UUID v4 */
    bytes[8] = (uint8_t) ((bytes[8] & 0x3f) | 0x80); /* RFC 4122 variant */
    int n = snprintf(out, METADATA_DB_CATALOG_LIBRARY_ID_SIZE,
                     "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                     bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
                     bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
    return n == METADATA_DB_CATALOG_LIBRARY_ID_SIZE - 1;
}

/* Called only with metadata_db_mutex held. Missing/invalid identity files are
 * replaced; I/O errors fail closed so a catalog revision is never served with
 * a possibly reused identity. */
static bool catalog_identity_prepare(void) {
    if (catalog_library_id_ready && !catalog_library_id_force_rotation) return true;
    char id[METADATA_DB_CATALOG_LIBRARY_ID_SIZE] = {0};
    if (!catalog_library_id_force_rotation) {
        catalog_id_read_result_t read_result = catalog_read_id(id);
        if (read_result == CATALOG_ID_READ_ERROR) return false;
        if (read_result == CATALOG_ID_VALID) {
            memcpy(catalog_library_id, id, sizeof(id));
            catalog_library_id_ready = true;
            return true;
        }
    }
    if (!catalog_generate_id(id) || !catalog_write_id(id)) return false;
    memcpy(catalog_library_id, id, sizeof(id));
    catalog_library_id_ready = true;
    catalog_library_id_force_rotation = false;
    return true;
}

static metadata_db_load_outcome_t capture_tagcache_outcome(bool update_ready) {
    switch (tagcache_get_load_outcome()) {
        case TAGCACHE_LOAD_SUCCESS_NORMAL: last_outcome = METADATA_DB_LOAD_SUCCESS_NORMAL; break;
        case TAGCACHE_LOAD_SUCCESS_FRESH: last_outcome = METADATA_DB_LOAD_SUCCESS_FRESH; break;
        case TAGCACHE_LOAD_SUCCESS_RECOVERED: last_outcome = METADATA_DB_LOAD_SUCCESS_RECOVERED; break;
        case TAGCACHE_LOAD_FAILED: last_outcome = METADATA_DB_LOAD_FAILED; break;
    }
    if (update_ready) db_ready = last_outcome != METADATA_DB_LOAD_FAILED;
    return last_outcome;
}

metadata_db_load_outcome_t metadata_db_open(void) {
    METADATA_UPDATE_GUARD;
    METADATA_DB_GUARD;
    if (db_ready) {
        return last_outcome;
    }

#ifndef HOST_BUILD
    if (!music_root_is_mounted()) {
        last_outcome = METADATA_DB_LOAD_UNMOUNTED;
        return last_outcome;
    }
#endif

    struct stat directory_stat;
    if (stat(METADATA_DB_DIR, &directory_stat) != 0 &&
        (errno != ENOENT || mkdir(METADATA_DB_DIR, 0755) != 0)) {
        last_outcome = METADATA_DB_LOAD_FAILED;
        return last_outcome;
    }
    if (!tagcache_open(METADATA_DB_DIR) || !migration_pin_dirs() || !migration_detect()) {
        tagcache_close();
        migration_unpin_dirs();
        last_outcome = METADATA_DB_LOAD_FAILED;
        return last_outcome;
    }
    metadata_db_load_outcome_t outcome = capture_tagcache_outcome(true);
    if (outcome == METADATA_DB_LOAD_SUCCESS_FRESH ||
        outcome == METADATA_DB_LOAD_SUCCESS_RECOVERED) {
        catalog_library_id_ready = false;
        catalog_library_id_force_rotation = true;
    }
    /* A fresh or recovered tagcache starts a new catalog incarnation. Older
     * installs without the marker get one stable ID on their first open. */
    catalog_identity_prepare();
    return outcome;
}

bool metadata_db_storage_current(void) {
    METADATA_DB_GUARD;
    return db_ready && tagcache_storage_current();
}

int metadata_db_dup_directory_fd(void) {
    METADATA_DB_GUARD;
    return migration_db_fd >= 0 ? dup(migration_db_fd) : -1;
}

bool metadata_db_numeric_write_failed(void) {
    METADATA_DB_GUARD;
    return tagcache_numeric_write_failed();
}

metadata_db_snapshot_t *metadata_db_snapshot_open(bool recency) {
    METADATA_DB_GUARD;
    return db_ready ? tagcache_snapshot_open(recency) : NULL;
}

int metadata_db_snapshot_count(const metadata_db_snapshot_t *snapshot) {
    METADATA_DB_GUARD;
    return tagcache_snapshot_count(snapshot);
}

bool metadata_db_snapshot_path_at(const metadata_db_snapshot_t *snapshot, int rank, char *out, size_t out_size) {
    METADATA_DB_GUARD;
    return tagcache_snapshot_path_at(snapshot, rank, out, out_size);
}

void metadata_db_snapshot_close(metadata_db_snapshot_t *snapshot) {
    METADATA_DB_GUARD;
    tagcache_snapshot_close(snapshot);
}

metadata_db_snapshot_t *metadata_db_snapshot_retain(metadata_db_snapshot_t *snapshot) {
    METADATA_DB_GUARD;
    return tagcache_snapshot_retain(snapshot);
}

int metadata_db_snapshot_dup_directory_fd(const metadata_db_snapshot_t *snapshot) {
    METADATA_DB_GUARD;
    return tagcache_snapshot_dup_directory_fd(snapshot);
}

metadata_db_load_outcome_t metadata_db_reload(void) {
    METADATA_UPDATE_GUARD;
    METADATA_DB_GUARD;
    bool had_saved_db = db_ready &&
                        (last_outcome == METADATA_DB_LOAD_SUCCESS_NORMAL ||
                         last_outcome == METADATA_DB_LOAD_SUCCESS_RECOVERED);
    metadata_db_close();
    metadata_db_load_outcome_t outcome = metadata_db_open();
    if (had_saved_db && outcome == METADATA_DB_LOAD_SUCCESS_FRESH) {
        metadata_db_close();
        last_outcome = METADATA_DB_LOAD_FAILED;
        db_ready = false;
        return last_outcome;
    }
    return outcome;
}

bool metadata_db_prepare_rebuild(void) {
    METADATA_UPDATE_GUARD;
    METADATA_DB_GUARD;
    metadata_db_load_outcome_t outcome = metadata_db_open();
    if (outcome == METADATA_DB_LOAD_SUCCESS_NORMAL ||
        outcome == METADATA_DB_LOAD_SUCCESS_FRESH ||
        outcome == METADATA_DB_LOAD_SUCCESS_RECOVERED)
        return db_ready;
    if (outcome == METADATA_DB_LOAD_UNMOUNTED) return false;

    mkdir(METADATA_DB_DIR, 0755);
    bool prepared = tagcache_open_for_rebuild(METADATA_DB_DIR);
    if (prepared) prepared = migration_pin_dirs();
    if (prepared) prepared = migration_detect();
    if (!prepared) {
        tagcache_close();
        migration_unpin_dirs();
    }
    capture_tagcache_outcome(false);
    db_ready = prepared;
    if (prepared && outcome == METADATA_DB_LOAD_FAILED) {
        /* Keep catalog reads unavailable until the explicit replacement
         * generation commits and receives a fresh persistent identity. */
        catalog_library_id_ready = false;
        catalog_library_id_force_rotation = true;
        catalog_rebuild_pending = true;
    }
    return db_ready;
}

metadata_db_load_outcome_t metadata_db_get_load_outcome(void) {
    METADATA_DB_GUARD;
    return last_outcome;
}

/* Re-files every track under a new delimiter set. Held under the same guard as
 * every other tagcache access: the rebuild frees and replaces the published
 * indexes, so a reader or a background scan running concurrently would be left
 * holding freed memory. Blocking and allocation-heavy on a large library --
 * call it off the UI thread. */
/* Which Artists row a track belongs to, resolved in ONE lock scope: the name
 * is derived from the tag and looked up in the index without releasing in
 * between, so it cannot be split under one delimiter set and searched in an
 * index built from another.
 *
 * Does not wait. This runs on the UI refresh path, and a split rebuild holds
 * the lock for its whole run, so blocking here would freeze the screen for as
 * long as the rebuild takes. A highlight is cosmetic: when the lock is busy
 * this reports no row, and the refresh that follows the rebuild sets it. */
bool metadata_db_try_artist_row(const char * raw_artist, int64_t * out_offset) {
    if (!out_offset) return false;
    pthread_once(&metadata_db_mutex_once, metadata_db_mutex_init);
    if (pthread_mutex_trylock(&metadata_db_mutex) != 0) return false;

    char primary[TAGCACHE_TAG_MAX];
    tagcache_artist_primary(raw_artist, primary, sizeof(primary));
    /* Safe to call while holding the lock: it takes the same recursive mutex. */
    *out_offset = metadata_db_get_group_offset(METADATA_DB_GROUP_ARTIST, primary, NULL);

    pthread_mutex_unlock(&metadata_db_mutex);
    return true;
}


void metadata_db_close(void) {
    METADATA_UPDATE_GUARD;
    METADATA_DB_GUARD;
    metadata_db_migration_cancel();
    migration_pending = migration_cleanup = false;
    tagcache_close();
    migration_unpin_dirs();
    db_ready = false;
    last_outcome = METADATA_DB_LOAD_UNMOUNTED;
    catalog_library_id[0] = '\0';
    catalog_library_id_ready = false;
    catalog_library_id_force_rotation = false;
    catalog_rebuild_pending = false;
    path_cache_drop();
    remote_state_drop();
}

void metadata_db_begin_update(void) {
    METADATA_UPDATE_GUARD;
    METADATA_DB_GUARD;
    if (!db_ready) return;
    tagcache_begin_update_with_lock(metadata_query_unlock, metadata_query_lock);
}

bool metadata_db_get(const char * path, int64_t mtime, int64_t size, cached_tags_t * out) {
    METADATA_UPDATE_GUARD;
    METADATA_DB_GUARD;
    if (!db_ready) return false;
    tagcache_song_t song;
    if (!tagcache_lookup(path, clamp_i32(mtime), clamp_i32(size), &song)) return false;
    /* Databases written before album-order support left Rockbox's numeric
     * track/disc slots at zero. New scans persist -1 for a genuinely absent
     * tag, so zero/zero is an unambiguous one-time upgrade miss. */
    if (song.track_number == 0 && song.disc_number == 0) return false;
    snprintf(out->title, sizeof(out->title), "%s", song.title);
    snprintf(out->artist, sizeof(out->artist), "%s", song.artist);
    snprintf(out->album, sizeof(out->album), "%s", song.album);
    snprintf(out->album_artist, sizeof(out->album_artist), "%s", song.album_artist);
    snprintf(out->genre, sizeof(out->genre), "%s", song.genre);
    out->track_number = song.track_number;
    out->disc_number = song.disc_number;
    return true;
}

void metadata_db_put(const char * path, int64_t mtime, int64_t size, const cached_tags_t * tags) {
    METADATA_UPDATE_GUARD;
    METADATA_DB_GUARD;
    if (!db_ready || !tags) return;
    tagcache_upsert(path, clamp_i32(mtime), clamp_i32(size), tags->title, tags->artist, tags->album, tags->album_artist,
                    tags->genre, tags->track_number, tags->disc_number);
    int32_t rating = 0, playcount = 0, last_played = 0;
    if (remote_state_take(path, &rating, &playcount, &last_played))
        tagcache_overlay_stats(path, rating, playcount, last_played);
}

bool metadata_db_end_update(void) {
    METADATA_UPDATE_GUARD;
    METADATA_DB_GUARD;
    if (!db_ready) return false;
    if (migration_pending && migration_stats && !migration_replayed) {
        if (!tagcache_replay_stats_allow_missing(migration_stats, &migration_unmatched)) {
            tagcache_abort_update();
            return false;
        }
        migration_replayed = true;
        migration_archive_retained = migration_unmatched != 0;
        tagcache_set_staged_migration_state(TAGCACHE_MIGRATION_APPLIED |
                                            (migration_archive_retained ? TAGCACHE_MIGRATION_ARCHIVE : 0));
    }
    bool ok = tagcache_end_update_with_lock(metadata_query_unlock, metadata_query_lock);
    capture_tagcache_outcome(true);
    if (ok && catalog_library_id_force_rotation && catalog_identity_prepare())
        catalog_rebuild_pending = false;
    if (ok && migration_pending) migration_applied = true;
    if (!ok) { migration_replayed = false; migration_unmatched = 0; }
    return ok;
}

void metadata_db_abort_update(void) {
    METADATA_UPDATE_GUARD;
    METADATA_DB_GUARD;
    if (!db_ready) return;
    tagcache_abort_update();
    migration_replayed = false;
    migration_unmatched = 0;
    capture_tagcache_outcome(true);
}

int64_t metadata_db_get_song_count(void) {
    METADATA_DB_GUARD;
    if (!db_ready) return 0;
    return tagcache_live_count();
}

void metadata_db_get_group_counts(int * out_artist_count, int * out_album_artist_count, int * out_album_count) {
    METADATA_DB_GUARD;
    *out_artist_count = 0;
    *out_album_artist_count = 0;
    *out_album_count = 0;
    if (!db_ready) return;
    *out_artist_count = tagcache_group_count(TAGCACHE_GROUP_ARTIST);
    *out_album_artist_count = tagcache_group_count(TAGCACHE_GROUP_ALBUM_ARTIST);
    *out_album_count = tagcache_group_count(TAGCACHE_GROUP_ALBUM);
}

int metadata_db_get_songs_page_by_recency(int offset, int max_rows, song_row_t * out_rows) {
    METADATA_DB_GUARD;
    if (!db_ready || max_rows <= 0) return 0;
    if (offset < 0) offset = 0;
    int32_t live = tagcache_live_count();
    int w = 0;
    for (int32_t i = offset; i < live && w < max_rows; i++) {
        tagcache_song_t song;
        if (!tagcache_song_at_recency_rank(i, &song)) continue;
        copy_song(&song, &out_rows[w++]);
    }
    return w;
}

int metadata_db_get_groups_page(metadata_db_group_kind_t kind, int offset, int max_rows, group_row_t * out_rows) {
    METADATA_DB_GUARD;
    if (!db_ready || max_rows <= 0) return 0;
    if (offset < 0) offset = 0;
    if (kind == METADATA_DB_GROUP_GENRE) {
        enum { GENRE_LIMIT = 2000, GENRE_HASH_SIZE = 4096 };
        typedef struct { uint32_t hash; group_row_t row; } genre_group_t;
        genre_group_t * groups = calloc(GENRE_LIMIT, sizeof(*groups));
        int * slots = malloc(sizeof(*slots) * GENRE_HASH_SIZE);
        if (!groups || !slots) { free(groups); free(slots); return 0; }
        for (int i = 0; i < GENRE_HASH_SIZE; i++) slots[i] = -1;
        int used = 0;
        tagcache_query_t query;
        tagcache_query_begin(&query, TAGCACHE_QUERY_SONGS, NULL, NULL, NULL, NULL);
        int32_t rank;
        while (tagcache_query_next(&query, &rank)) {
            tagcache_song_t song;
            if (!tagcache_song_fields_at_title_rank(rank, TAGCACHE_FIELD_GENRE, &song) || !song.genre[0]) continue;
            uint32_t hash = 2166136261u;
            for (const unsigned char *p = (const unsigned char *)song.genre; *p; p++) {
                unsigned char c = *p >= 'A' && *p <= 'Z' ? (unsigned char)(*p + ('a' - 'A')) : *p;
                hash = (hash ^ c) * 16777619u;
            }
            unsigned bucket = hash % GENRE_HASH_SIZE;
            while (slots[bucket] >= 0 && (groups[slots[bucket]].hash != hash ||
                   strcasecmp(groups[slots[bucket]].row.genre_name, song.genre) != 0)) bucket = (bucket + 1) % GENRE_HASH_SIZE;
            if (slots[bucket] >= 0) { groups[slots[bucket]].row.song_count++; continue; }
            if (used >= GENRE_LIMIT) continue;
            int index = used++;
            slots[bucket] = index;
            groups[index].hash = hash;
            snprintf(groups[index].row.genre_name, sizeof(groups[index].row.genre_name), "%s", song.genre);
            groups[index].row.song_count = 1;
            groups[index].row.first_song_id = song.id;
        }
        /* Sort the bounded distinct genre list alphabetically for a stable UI. */
        for (int i = 1; i < used; i++) {
            group_row_t value = groups[i].row;
            int j = i;
            while (j > 0 && strcasecmp(groups[j - 1].row.genre_name, value.genre_name) > 0) {
                groups[j].row = groups[j - 1].row;
                j--;
            }
            groups[j].row = value;
        }
        int written = 0;
        for (int i = offset; i < used && written < max_rows; i++) out_rows[written++] = groups[i].row;
        free(slots);
        free(groups);
        return written;
    }
    int tc_kind = (kind == METADATA_DB_GROUP_ALBUM_ARTIST) ? TAGCACHE_GROUP_ALBUM_ARTIST
                  : (kind == METADATA_DB_GROUP_ALBUM)     ? TAGCACHE_GROUP_ALBUM
                                                          : TAGCACHE_GROUP_ARTIST;
    int n = tagcache_group_count(tc_kind);
    int w = 0;
    for (int i = offset; i < n && w < max_rows; i++) {
        tagcache_group_t g;
        if (!tagcache_group_at(tc_kind, i, &g)) continue;
        copy_group(&g, &out_rows[w]);
        if (tc_kind != TAGCACHE_GROUP_ALBUM) out_rows[w].album_artist[0] = '\0';
        w++;
    }
    return w;
}

int metadata_db_get_artist_songs(const char * artist, int offset, song_row_t * out_rows, int max_rows) {
    METADATA_DB_GUARD;
    if (!db_ready || max_rows <= 0) return 0;
    if (offset < 0) offset = 0;
    int32_t stack_ids[64];
    int32_t * ids = stack_ids;
    if (max_rows > 64) {
        ids = malloc(sizeof(*ids) * (size_t) max_rows);
        if (!ids) return 0;
    }
    int n = tagcache_artist_song_ids(artist, offset, ids, max_rows);
    int w = 0;
    for (int i = 0; i < n && w < max_rows; i++) {
        tagcache_song_t song;
        if (!tagcache_song_by_id(ids[i], &song)) continue;
        copy_song(&song, &out_rows[w++]);
    }
    if (ids != stack_ids) free(ids);
    return w;
}

/* Mirrors metadata_db_get_artist_songs() above, against the ALBUM_ARTIST
 * group instead -- see tagcache_album_artist_song_ids()'s own comment. */
int metadata_db_get_album_artist_songs(const char * album_artist, int offset, song_row_t * out_rows, int max_rows) {
    METADATA_DB_GUARD;
    if (!db_ready || max_rows <= 0) return 0;
    if (offset < 0) offset = 0;
    int32_t stack_ids[64];
    int32_t * ids = stack_ids;
    if (max_rows > 64) {
        ids = malloc(sizeof(*ids) * (size_t) max_rows);
        if (!ids) return 0;
    }
    int n = tagcache_album_artist_song_ids(album_artist, offset, ids, max_rows);
    int w = 0;
    for (int i = 0; i < n && w < max_rows; i++) {
        tagcache_song_t song;
        if (!tagcache_song_by_id(ids[i], &song)) continue;
        copy_song(&song, &out_rows[w++]);
    }
    if (ids != stack_ids) free(ids);
    return w;
}

int metadata_db_get_album_songs(const char * album, const char * album_artist, int offset, song_row_t * out_rows,
                                 int max_rows) {
    METADATA_DB_GUARD;
    if (!db_ready || max_rows <= 0) return 0;
    if (offset < 0) offset = 0;
    int32_t stack_ids[64];
    int32_t * ids = stack_ids;
    if (max_rows > 64) {
        ids = malloc(sizeof(*ids) * (size_t) max_rows);
        if (!ids) return 0;
    }
    int n = tagcache_album_song_ids(album, album_artist, offset, ids, max_rows);
    int w = 0;
    for (int i = 0; i < n && w < max_rows; i++) {
        tagcache_song_t song;
        if (!tagcache_song_by_id(ids[i], &song)) continue;
        copy_song(&song, &out_rows[w++]);
    }
    if (ids != stack_ids) free(ids);
    return w;
}

bool metadata_db_get_song_by_id(int64_t id, song_row_t * out_row) {
    METADATA_DB_GUARD;
    if (!db_ready) return false;
    tagcache_song_t song;
    if (!tagcache_song_by_id((int32_t) id, &song)) return false;
    copy_song(&song, out_row);
    return true;
}

bool metadata_db_get_song_by_path(const char * path, song_row_t * out_row) {
    METADATA_DB_GUARD;
    if (!db_ready) return false;
    tagcache_song_t song;
    if (!tagcache_song_by_path(path, &song)) return false;
    copy_song(&song, out_row);
    return true;
}

void metadata_db_get_songs_by_ids(const int64_t * ids, int count, song_row_t * out_rows) {
    METADATA_DB_GUARD;
    for (int i = 0; i < count; i++) out_rows[i].id = -1;
    if (!db_ready || count <= 0) return;
    for (int i = 0; i < count; i++) {
        tagcache_song_t song;
        if (!tagcache_song_by_id((int32_t) ids[i], &song)) continue;
        copy_song(&song, &out_rows[i]);
    }
}

void metadata_db_get_songs_by_paths(const char * const * paths, int count, song_row_t * out_rows) {
    METADATA_DB_GUARD;
    for (int i = 0; i < count; i++) out_rows[i].id = -1;
    if (!db_ready || count <= 0) return;
    for (int i = 0; i < count; i++) {
        tagcache_song_t song;
        if (!tagcache_song_by_path(paths[i], &song)) continue;
        copy_song(&song, &out_rows[i]);
    }
}

int64_t metadata_db_get_song_title_offset(const char * path) {
    METADATA_DB_GUARD;
    if (!db_ready || !path) return -1;
    int32_t rank = tagcache_title_rank_of_path(path);
    return rank >= 0 ? rank : -1;
}

int64_t metadata_db_get_song_recency_offset(const char * path) {
    METADATA_DB_GUARD;
    if (!db_ready || !path) return -1;
    int32_t rank = tagcache_recency_rank_of_path(path);
    return rank >= 0 ? rank : -1;
}

int64_t metadata_db_get_group_offset(metadata_db_group_kind_t kind, const char * name, const char * album_artist) {
    METADATA_DB_GUARD;
    if (!db_ready || !name) return -1;
    int tc_kind = (kind == METADATA_DB_GROUP_ALBUM_ARTIST) ? TAGCACHE_GROUP_ALBUM_ARTIST
                  : (kind == METADATA_DB_GROUP_ALBUM)     ? TAGCACHE_GROUP_ALBUM
                                                          : TAGCACHE_GROUP_ARTIST;
    int idx = tagcache_group_index(tc_kind, name, album_artist);
    return idx >= 0 ? idx : -1;
}

void metadata_db_get_az_table(metadata_db_az_kind_t kind, int out_table[27]) {
    METADATA_DB_GUARD;
    for (int i = 0; i < 27; i++) out_table[i] = -1;
    if (!db_ready) return;

    int group_kind = az_kind_to_group(kind);
    int count = group_kind >= 0 ? tagcache_group_count(group_kind) : tagcache_live_count();
    for (int letter = 0; letter < 26; letter++) {
        int lo = 0, hi = count;
        unsigned char target = (unsigned char)('a' + letter);
        while (lo < hi) {
            int mid = lo + (hi - lo) / 2;
            unsigned char first;
            if (group_kind >= 0) {
                tagcache_group_t group;
                if (!tagcache_group_at(group_kind, mid, &group)) break;
                first = (unsigned char)group.name[0];
            } else {
                tagcache_song_t song;
                if (!tagcache_song_fields_at_title_rank(mid, TAGCACHE_FIELD_TITLE, &song)) break;
                first = (unsigned char)song.title[0];
            }
            if (first >= 'A' && first <= 'Z') first += 'a' - 'A';
            if (first < target) lo = mid + 1; else hi = mid;
        }
        if (lo >= count) continue;
        unsigned char first;
        if (group_kind >= 0) {
            tagcache_group_t group;
            if (!tagcache_group_at(group_kind, lo, &group)) continue;
            first = (unsigned char)group.name[0];
        } else {
            tagcache_song_t song;
            if (!tagcache_song_fields_at_title_rank(lo, TAGCACHE_FIELD_TITLE, &song)) continue;
            first = (unsigned char)song.title[0];
        }
        if (first >= 'A' && first <= 'Z') first += 'a' - 'A';
        if (first == target) out_table[letter] = lo;
    }
    for (int i = 24; i >= 0; i--) {
        if (out_table[i] == -1) out_table[i] = out_table[i + 1];
    }
    /* The first row may itself be a digit or punctuation.  Checking the
     * offset of A misses that case when A is present at offset zero. */
    out_table[26] = -1;
    if (count > 0) {
        unsigned char first = 0;
        if (group_kind >= 0) {
            tagcache_group_t group;
            if (tagcache_group_at(group_kind, 0, &group)) first = (unsigned char)group.name[0];
        } else {
            tagcache_song_t song;
            if (tagcache_song_fields_at_title_rank(0, TAGCACHE_FIELD_TITLE, &song)) first = (unsigned char)song.title[0];
        }
        if (!((first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z'))) out_table[26] = 0;
    }
}

int metadata_db_search_names(metadata_db_az_kind_t kind, const char *needle, int max_rows,
                             metadata_db_search_hit_t *out_hits) {
    METADATA_DB_GUARD;
    if (!db_ready || max_rows <= 0 || !needle || !needle[0]) return 0;
    int group_kind = az_kind_to_group(kind), w = 0;
    tagcache_query_t query;
    tagcache_query_begin(&query, group_kind >= 0 ? group_kind : TAGCACHE_QUERY_TITLES,
                         needle, NULL, NULL, NULL);
    int32_t rank;
    while (w < max_rows && tagcache_query_next(&query, &rank)) {
        if (group_kind >= 0) {
            tagcache_group_t group;
            if (!tagcache_group_at(group_kind, rank, &group)) continue;
            snprintf(out_hits[w].label, sizeof(out_hits[w].label), "%s", group.name);
        } else {
            tagcache_song_t song;
            if (!tagcache_song_fields_at_title_rank(rank, TAGCACHE_FIELD_TITLE, &song)) continue;
            /* Display truncation follows the public song-row title size. */
            song_row_t row = {0};
            snprintf(row.tags.title, sizeof(row.tags.title), "%s", song.title);
            if (!row.tags.title[0]) {
                if (!tagcache_song_fields_by_id(song.id, TAGCACHE_FIELD_PATH, &song)) continue;
                snprintf(row.path, sizeof(row.path), "%s", song.path);
            }
            metadata_db_song_display_title(&row, out_hits[w].label, sizeof(out_hits[w].label));
        }
        out_hits[w++].offset = rank;
    }
    return w;
}

void metadata_db_song_display_title(const song_row_t * row, char * out, size_t out_size) {
    if (!row || !out || out_size == 0) return;
    if (row->tags.title[0] != '\0') {
        utf8_truncate_safe(out, row->tags.title, out_size);
        return;
    }
    const char * slash = strrchr(row->path, '/');
    utf8_truncate_safe(out, slash ? slash + 1 : row->path, out_size);
}

/* All helpers below require metadata_db_mutex to stay held for the whole
 * revision/count/page snapshot. Never call the public metadata_db page helpers
 * from these paths: they acquire their own guard and would split this snapshot
 * into separately locked reads. */
static metadata_db_catalog_result_t catalog_page_begin_locked(
    const char * expected_revision, metadata_db_catalog_page_t * out_page) {
    if (!db_ready || catalog_rebuild_pending || !tagcache_storage_current() || !catalog_identity_prepare())
        return METADATA_DB_CATALOG_UNAVAILABLE;
    int32_t generation = tagcache_generation();
    if (generation < 0) return METADATA_DB_CATALOG_UNAVAILABLE;
    snprintf(out_page->library_id, sizeof(out_page->library_id), "%s", catalog_library_id);
    snprintf(out_page->revision, sizeof(out_page->revision), "%s:%d", catalog_library_id, generation);
    if (expected_revision && strcmp(expected_revision, out_page->revision) != 0)
        return METADATA_DB_CATALOG_STALE;
    return METADATA_DB_CATALOG_OK;
}

static bool catalog_album_representative_locked(const tagcache_song_t * song, int64_t * out_id) {
    *out_id = 0;
    if (!song || !song->album[0]) return true;
    /* The library scanner normalizes absent ALBUMARTIST to ARTIST before
     * upserting. Keep that same identity for older/incomplete cache rows. */
    const char * album_artist = song->album_artist[0] ? song->album_artist : song->artist;
    int index = tagcache_group_index(TAGCACHE_GROUP_ALBUM, song->album, album_artist);
    if (index < 0 && !song->album_artist[0])
        index = tagcache_group_index(TAGCACHE_GROUP_ALBUM, song->album, "");
    tagcache_group_t group;
    if (index < 0 || !tagcache_group_at(TAGCACHE_GROUP_ALBUM, index, &group)) return false;
    *out_id = group.first_song_id;
    return true;
}

metadata_db_catalog_result_t metadata_db_catalog_songs_page(
    const char * expected_revision, int offset, int limit,
    metadata_db_catalog_page_t * out_page, metadata_db_catalog_song_t * out_rows) {
    if (!out_page || !out_rows || offset < 0 || limit < 1 || limit > METADATA_DB_CATALOG_PAGE_MAX)
        return METADATA_DB_CATALOG_UNAVAILABLE;
    memset(out_page, 0, sizeof(*out_page));
    METADATA_DB_GUARD;
    metadata_db_catalog_result_t result = catalog_page_begin_locked(expected_revision, out_page);
    if (result != METADATA_DB_CATALOG_OK) return result;

    out_page->total = tagcache_live_count();
    tagcache_query_t query;
    tagcache_query_begin(&query, TAGCACHE_QUERY_SONGS, NULL, NULL, NULL, NULL);
    tagcache_query_skip(&query, (uint64_t) offset);
    int32_t rank;
    while (out_page->count < limit && tagcache_query_next(&query, &rank)) {
        tagcache_song_t song;
        if (!tagcache_song_at_title_rank(rank, &song)) return METADATA_DB_CATALOG_UNAVAILABLE;
        metadata_db_catalog_song_t * dst = &out_rows[out_page->count];
        copy_song(&song, &dst->song);
        if (!catalog_album_representative_locked(&song, &dst->album_representative_id))
            return METADATA_DB_CATALOG_UNAVAILABLE;
        out_page->count++;
    }
    if (out_page->count < limit && (int64_t) offset + out_page->count < out_page->total)
        return METADATA_DB_CATALOG_UNAVAILABLE;
    return METADATA_DB_CATALOG_OK;
}

metadata_db_catalog_result_t metadata_db_catalog_covers_page(
    const char * expected_revision, int offset, int limit,
    metadata_db_catalog_page_t * out_page, metadata_db_catalog_cover_t * out_rows) {
    if (!out_page || !out_rows || offset < 0 || limit < 1 || limit > METADATA_DB_CATALOG_PAGE_MAX)
        return METADATA_DB_CATALOG_UNAVAILABLE;
    memset(out_page, 0, sizeof(*out_page));
    METADATA_DB_GUARD;
    metadata_db_catalog_result_t result = catalog_page_begin_locked(expected_revision, out_page);
    if (result != METADATA_DB_CATALOG_OK) return result;

    int album_count = tagcache_group_count(TAGCACHE_GROUP_ALBUM);
    out_page->total = album_count;
    for (int rank = offset; rank < album_count && out_page->count < limit; rank++) {
        tagcache_group_t group;
        if (!tagcache_group_at(TAGCACHE_GROUP_ALBUM, rank, &group) || group.first_song_id <= 0)
            return METADATA_DB_CATALOG_UNAVAILABLE;
        tagcache_song_t representative;
        if (!tagcache_song_by_id((int32_t) group.first_song_id, &representative))
            return METADATA_DB_CATALOG_UNAVAILABLE;

        metadata_db_catalog_cover_t * dst = &out_rows[out_page->count];
        memset(dst, 0, sizeof(*dst));
        copy_song(&representative, &dst->representative);
        dst->representative_id = group.first_song_id;
        dst->scanned_mtime = representative.mtime;
        dst->scanned_size = representative.size;
        out_page->count++;
    }
    if (out_page->count < limit && (int64_t) offset + out_page->count < out_page->total)
        return METADATA_DB_CATALOG_UNAVAILABLE;
    return METADATA_DB_CATALOG_OK;
}

metadata_db_catalog_result_t metadata_db_catalog_cover_source(
    const char * expected_revision, int64_t representative_id,
    metadata_db_catalog_page_t * out_page, metadata_db_catalog_cover_t * out_source) {
    if (!out_page || !out_source || representative_id <= 0 || representative_id > INT32_MAX)
        return METADATA_DB_CATALOG_STALE;
    memset(out_page, 0, sizeof(*out_page));
    memset(out_source, 0, sizeof(*out_source));
    METADATA_DB_GUARD;
    metadata_db_catalog_result_t result = catalog_page_begin_locked(expected_revision, out_page);
    if (result != METADATA_DB_CATALOG_OK) return result;

    tagcache_song_t song;
    if (!tagcache_song_by_id((int32_t) representative_id, &song)) return METADATA_DB_CATALOG_STALE;
    int group_index = song.album[0]
        ? tagcache_group_index(TAGCACHE_GROUP_ALBUM, song.album,
                              song.album_artist[0] ? song.album_artist : song.artist)
        : -1;
    if (group_index < 0 && song.album[0] && !song.album_artist[0])
        group_index = tagcache_group_index(TAGCACHE_GROUP_ALBUM, song.album, "");
    tagcache_group_t group;
    if (group_index < 0 || !tagcache_group_at(TAGCACHE_GROUP_ALBUM, group_index, &group) ||
        group.first_song_id != representative_id) return METADATA_DB_CATALOG_STALE;

    copy_song(&song, &out_source->representative);
    out_source->representative_id = representative_id;
    out_source->scanned_mtime = song.mtime;
    out_source->scanned_size = song.size;
    out_page->total = tagcache_group_count(TAGCACHE_GROUP_ALBUM);
    out_page->count = 1;
    return METADATA_DB_CATALOG_OK;
}

metadata_db_catalog_result_t metadata_db_catalog_validate_song_revision(
    const char * expected_revision, int64_t song_id) {
    if (!expected_revision || !expected_revision[0] || song_id <= 0 || song_id > INT32_MAX)
        return METADATA_DB_CATALOG_STALE;
    metadata_db_catalog_page_t page;
    memset(&page, 0, sizeof(page));
    METADATA_DB_GUARD;
    metadata_db_catalog_result_t result = catalog_page_begin_locked(expected_revision, &page);
    if (result != METADATA_DB_CATALOG_OK) return result;
    tagcache_song_t song;
    return tagcache_song_by_id((int32_t) song_id, &song)
        ? METADATA_DB_CATALOG_OK : METADATA_DB_CATALOG_STALE;
}

metadata_db_catalog_result_t metadata_db_catalog_get_song_revision(
    const char * expected_revision, int64_t song_id, song_row_t * out_song) {
    if (!expected_revision || !expected_revision[0] || !out_song ||
        song_id <= 0 || song_id > INT32_MAX)
        return METADATA_DB_CATALOG_STALE;
    memset(out_song, 0, sizeof(*out_song));
    metadata_db_catalog_page_t page;
    memset(&page, 0, sizeof(page));
    METADATA_DB_GUARD;
    metadata_db_catalog_result_t result = catalog_page_begin_locked(expected_revision, &page);
    if (result != METADATA_DB_CATALOG_OK) return result;
    tagcache_song_t song;
    if (!tagcache_song_by_id((int32_t) song_id, &song)) return METADATA_DB_CATALOG_STALE;
    copy_song(&song, out_song);
    return METADATA_DB_CATALOG_OK;
}

int metadata_db_search_songs(const char *query_text, song_row_t *out_rows, int max_rows) {
    if (!query_text || !query_text[0]) return 0;
    return metadata_db_get_songs_filtered_page(query_text, NULL, NULL, NULL, NULL, 0, max_rows, out_rows);
}

int64_t metadata_db_count_songs_filtered(const char *needle, const char *artist,
                                        const char *album_artist, const char *album, const char *genre) {
    METADATA_DB_GUARD;
    if (!db_ready) return 0;
    if (genre && strlen(genre) >= METADATA_DB_TEXT_MAX) return 0;
    tagcache_query_t query;
    tagcache_query_begin(&query, TAGCACHE_QUERY_SONGS, needle, artist, album_artist, album);
    if (genre && genre[0]) {
        int32_t rank, count = 0;
        while (tagcache_query_next(&query, &rank)) {
            tagcache_song_t song;
            if (tagcache_song_fields_at_title_rank(rank, TAGCACHE_FIELD_GENRE, &song) &&
                strcasecmp(song.genre, genre) == 0) count++;
        }
        return count;
    }
    return tagcache_query_count(&query);
}

int metadata_db_get_songs_filtered_page(const char *needle, const char *artist,
                                        const char *album_artist, const char *album, const char *genre,
                                        int offset, int max_rows, song_row_t *out_rows) {
    METADATA_DB_GUARD;
    if (!db_ready || max_rows <= 0) return 0;
    if (genre && strlen(genre) >= METADATA_DB_TEXT_MAX) return 0;
    tagcache_query_t query;
    tagcache_query_begin(&query, TAGCACHE_QUERY_SONGS, needle, artist, album_artist, album);
    if (genre && genre[0]) {
        if (offset < 0) offset = 0;
        int w = 0, skipped = 0;
        int32_t rank;
        while (w < max_rows && tagcache_query_next(&query, &rank)) {
            tagcache_song_t song;
            if (!tagcache_song_fields_at_title_rank(rank, TAGCACHE_FIELD_GENRE, &song) ||
                strcasecmp(song.genre, genre) != 0) continue;
            if (skipped++ < offset) continue;
            if (tagcache_song_at_title_rank(rank, &song)) copy_song(&song, &out_rows[w++]);
        }
        return w;
    }
    tagcache_query_skip(&query, offset);
    int w = 0;
    int32_t rank;
    while (w < max_rows && tagcache_query_next(&query, &rank)) {
        tagcache_song_t song;
        if (tagcache_song_at_title_rank(rank, &song)) copy_song(&song, &out_rows[w++]);
    }
    return w;
}

int metadata_db_get_albums_page_filtered(const char *filter, int offset, int max_rows,
                                         group_row_t *out_rows) {
    METADATA_DB_GUARD;
    if (!db_ready || max_rows <= 0) return 0;
    if (!filter || !filter[0]) return metadata_db_get_groups_page(METADATA_DB_GROUP_ALBUM, offset, max_rows, out_rows);
    int a = 0, b = 0, skipped = 0, w = 0;
    int left = tagcache_group_album_at(TAGCACHE_GROUP_ARTIST, filter, a);
    int right = tagcache_group_album_at(TAGCACHE_GROUP_ALBUM_ARTIST, filter, b);
    while ((left >= 0 || right >= 0) && w < max_rows) {
        int rank = left < 0 ? right : right < 0 ? left : left < right ? left : right;
        if (left == rank) left = tagcache_group_album_at(TAGCACHE_GROUP_ARTIST, filter, ++a);
        if (right == rank) right = tagcache_group_album_at(TAGCACHE_GROUP_ALBUM_ARTIST, filter, ++b);
        if (skipped++ < offset) continue;
        tagcache_group_t group;
        if (tagcache_group_at(TAGCACHE_GROUP_ALBUM, rank, &group)) copy_group(&group, &out_rows[w++]);
    }
    return w;
}

int64_t metadata_db_count_albums_for_group(metadata_db_group_kind_t kind, const char *name) {
    METADATA_DB_GUARD;
    if (!db_ready || !name || (kind != METADATA_DB_GROUP_ARTIST && kind != METADATA_DB_GROUP_ALBUM_ARTIST)) return 0;
    return tagcache_group_album_count(kind == METADATA_DB_GROUP_ARTIST ? TAGCACHE_GROUP_ARTIST : TAGCACHE_GROUP_ALBUM_ARTIST, name);
}

int metadata_db_get_albums_for_group(metadata_db_group_kind_t kind, const char *name, int offset, int max_rows,
                                    group_row_t *out_rows) {
    METADATA_DB_GUARD;
    if (!db_ready || !name || max_rows <= 0 ||
        (kind != METADATA_DB_GROUP_ARTIST && kind != METADATA_DB_GROUP_ALBUM_ARTIST)) return 0;
    if (offset < 0) offset = 0;
    int tc_kind = kind == METADATA_DB_GROUP_ARTIST ? TAGCACHE_GROUP_ARTIST : TAGCACHE_GROUP_ALBUM_ARTIST;
    int w = 0;
    for (int i = offset; w < max_rows; i++) {
        int rank = tagcache_group_album_at(tc_kind, name, i);
        if (rank < 0) break;
        tagcache_group_t group;
        if (tagcache_group_at(TAGCACHE_GROUP_ALBUM, rank, &group)) copy_group(&group, &out_rows[w++]);
    }
    return w;
}

int64_t metadata_db_get_album_for_group_offset(metadata_db_group_kind_t kind, const char *name,
                                               const char *album, const char *album_artist) {
    METADATA_DB_GUARD;
    if (!db_ready || !name || (kind != METADATA_DB_GROUP_ARTIST && kind != METADATA_DB_GROUP_ALBUM_ARTIST)) return -1;
    return tagcache_group_album_offset(kind == METADATA_DB_GROUP_ARTIST ? TAGCACHE_GROUP_ARTIST : TAGCACHE_GROUP_ALBUM_ARTIST,
                                      name, album, album_artist);
}

bool metadata_db_song_favorite_is_set(const char * path) {
    METADATA_DB_GUARD;
    if (!db_ready || !path) return false;
    tagcache_song_t song;
    if (tagcache_song_by_path(path, &song)) return song.rating != 0;
    int32_t rating = 0;
    if (!remote_state_get(path, &rating, NULL, NULL)) return false;
    return rating != 0;
}

void metadata_db_song_favorite_set(const char * path, bool is_favorite) {
    METADATA_DB_GUARD;
    if (!db_ready || !path) return;
    if (tagcache_song_by_path(path, NULL)) tagcache_set_rating(path, is_favorite ? 1 : 0);
    else remote_state_set_rating(path, is_favorite ? 1 : 0);
}

void metadata_db_load_favorite_songs(char *** out_paths, int * out_count) {
    METADATA_DB_GUARD;
    *out_paths = NULL;
    *out_count = 0;
    if (!db_ready) return;
    int32_t slots = tagcache_slot_count();
    char ** paths = NULL;
    int n = 0, cap = 0;
    bool ok = true;
    for (int32_t i = 0; i < slots; i++) {
        tagcache_song_t song;
        if (!tagcache_song_fields_by_id(i + 1, 0, &song)) continue;
        if (song.rating == 0) continue;
        if (n >= cap) {
            int next = cap ? cap * 2 : 16;
            char ** p = realloc(paths, sizeof(*p) * (size_t) next);
            if (!p) {
                ok = false;
                break;
            }
            paths = p;
            cap = next;
        }
        if (!tagcache_song_fields_by_id(i + 1, TAGCACHE_FIELD_PATH, &song)) {
            ok = false;
            break;
        }
        paths[n] = strdup(song.path);
        if (!paths[n]) {
            ok = false;
            break;
        }
        n++;
    }
    if (!ok || n == 0) {
        for (int j = 0; j < n; j++) free(paths[j]);
        free(paths);
        return;
    }
    qsort(paths, (size_t) n, sizeof(*paths), cmp_path_ptr);
    *out_paths = paths;
    *out_count = n;
}

void metadata_db_song_play_count_increment(const char * path) {
    METADATA_DB_GUARD;
    if (!db_ready || !path) return;
    int32_t now = (int32_t) time(NULL);
    if (tagcache_song_by_path(path, NULL)) tagcache_add_play(path, now);
    else remote_state_add_play(path, now);
}

void metadata_db_load_top_played_songs(int limit, char *** out_paths, int * out_count) {
    METADATA_DB_GUARD;
    *out_paths = NULL;
    *out_count = 0;
    if (!db_ready || limit <= 0) return;
    int32_t slots = tagcache_slot_count();
    played_hit_t * hits = NULL;
    int n = 0, cap = 0;
    bool hits_ok = true;
    for (int32_t i = 0; i < slots; i++) {
        tagcache_song_t song;
        if (!tagcache_song_fields_by_id(i + 1, 0, &song)) continue;
        if (song.playcount <= 0) continue;
        played_hit_t candidate = {
            .id = song.id,
            .count = song.playcount,
            .last_played = song.last_played,
            .ordinal = i,
        };
        if (n < limit) {
            if (n >= cap) {
                int next = cap ? (cap > limit / 2 ? limit : cap * 2) : 16;
                if (next > limit) next = limit;
                if ((size_t) next > SIZE_MAX / sizeof(*hits)) {
                    hits_ok = false;
                    break;
                }
                played_hit_t * h = realloc(hits, sizeof(*h) * (size_t) next);
                if (!h) {
                    hits_ok = false;
                    break;
                }
                hits = h;
                cap = next;
            }
            hits[n] = candidate;
            played_hit_heap_sift_up(hits, n);
            n++;
        } else if (cmp_hit_played(&candidate, &hits[0]) < 0) {
            hits[0] = candidate;
            played_hit_heap_sift_down(hits, n, 0);
        }
    }
    if (!hits_ok) {
        free(hits);
        return;
    }
    qsort(hits, (size_t) n, sizeof(*hits), cmp_hit_played);
    if (n > limit) n = limit;
    if (n <= 0) {
        free(hits);
        return;
    }
    char ** paths = malloc(sizeof(*paths) * (size_t) n);
    if (!paths) {
        free(hits);
        return;
    }
    int w = 0;
    for (int i = 0; i < n; i++) {
        tagcache_song_t song;
        if (!tagcache_song_fields_by_id(hits[i].id, TAGCACHE_FIELD_PATH, &song)) break;
        paths[w] = strdup(song.path);
        if (!paths[w]) break;
        w++;
    }
    free(hits);
    if (w != n) {
        for (int j = 0; j < w; j++) free(paths[j]);
        free(paths);
        return;
    }
    *out_paths = paths;
    *out_count = w;
}

void metadata_db_load_recently_added_songs(int limit, char *** out_paths, int * out_count) {
    METADATA_DB_GUARD;
    *out_paths = NULL;
    *out_count = 0;
    if (!db_ready || limit <= 0) return;
    int32_t live = tagcache_live_count();
    if (live <= 0) return;
    int n = live < limit ? (int) live : limit;
    char ** paths = malloc(sizeof(*paths) * (size_t) n);
    if (!paths) return;
    int w = 0;
    for (int32_t i = 0; i < live && w < n; i++) {
        tagcache_song_t song;
        if (!tagcache_song_fields_at_recency_rank(i, TAGCACHE_FIELD_PATH, &song)) continue;
        paths[w] = strdup(song.path);
        if (!paths[w]) break;
        w++;
    }
    if (w != n) {
        for (int j = 0; j < w; j++) free(paths[j]);
        free(paths);
        return;
    }
    *out_paths = paths;
    *out_count = w;
}

bool metadata_db_book_favorite_is_set(const char * path) {
    return path && path[0] && path_cache_has(PATH_CACHE_BOOK_FAVORITES, path);
}

void metadata_db_book_favorite_set(const char * path, bool is_favorite) {
    if (!path || !path[0]) return;
    if (is_favorite) path_cache_insert(PATH_CACHE_BOOK_FAVORITES, path);
    else path_cache_delete(PATH_CACHE_BOOK_FAVORITES, path);
}

void metadata_db_book_replace_all(char * const * paths, int count) {
    path_cache_replace(PATH_CACHE_BOOKS, paths, count);
}

void metadata_db_load_favorite_books(char *** out_paths, int * out_count) {
    path_cache_load_matching(PATH_CACHE_BOOKS, PATH_CACHE_BOOK_FAVORITES, out_paths, out_count);
}

void metadata_db_load_all_books(char *** out_paths, int * out_count) {
    path_cache_load(PATH_CACHE_BOOKS, out_paths, out_count);
}

void metadata_db_load_all_playlists(char *** out_paths, int * out_count) {
    path_cache_load(PATH_CACHE_PLAYLISTS, out_paths, out_count);
}

void metadata_db_playlist_insert_one(const char * path) {
    if (!path || !path[0]) return;
    path_cache_insert(PATH_CACHE_PLAYLISTS, path);
}

void metadata_db_playlist_delete_one(const char * path) {
    if (!path || !path[0]) return;
    path_cache_delete(PATH_CACHE_PLAYLISTS, path);
}

void metadata_db_subsonic_server_save(const char * url, const char * username, const char * password, bool verify_tls) {
    subsonic_saved_servers_upsert(url, username, password, verify_tls);
}

void metadata_db_load_subsonic_servers(subsonic_server_row_t ** out_rows, int * out_count) {
    *out_rows = NULL;
    *out_count = 0;
    subsonic_saved_server_t * rows = NULL;
    int count = 0;
    subsonic_saved_servers_load(&rows, &count);
    if (count <= 0) {
        free(rows);
        return;
    }
    /* subsonic_saved_server_t and subsonic_server_row_t are separately
     * declared (subsonic_saved_servers.h has no dependency on
     * metadata_db.h, matching path_cache.h/remote_state.h's own
     * layering) but field-for-field identical -- copy rather than cast,
     * so a future divergence between the two is a compile error in this
     * one place, not a silent mismatch. */
    subsonic_server_row_t * out = malloc(sizeof(*out) * (size_t) count);
    if (!out) {
        free(rows);
        return;
    }
    for (int i = 0; i < count; i++) {
        snprintf(out[i].url, sizeof(out[i].url), "%s", rows[i].url);
        snprintf(out[i].username, sizeof(out[i].username), "%s", rows[i].username);
        snprintf(out[i].password, sizeof(out[i].password), "%s", rows[i].password);
        out[i].verify_tls = rows[i].verify_tls;
    }
    free(rows);
    *out_rows = out;
    *out_count = count;
}
