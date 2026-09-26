/* Seek-based POSIX tagcache using the Rockbox master and string-file format.
 * Copyright (C) 2005 Miika Pekkarinen (original on-disk format)
 * Copyright (C) Open HiBy Player contributors (POSIX implementation)
 * Generation commits preserve the previous library until pointer publication.
 */
#include "tagcache.h"
#include "tagcache_generation_refs.h"
#include "library_endian.h"
#include "db_log.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

static void tagcache_log_free_space(const char * dir) {
    if (!db_log_enabled()) return;
    struct statvfs vfs;
    if (statvfs(dir, &vfs) != 0) {
        DB_LOG("DB", "statvfs_failed dir=%s errno=%d(%s)", dir, errno, strerror(errno));
        return;
    }
    uint64_t free_bytes = (uint64_t) vfs.f_bsize * (uint64_t) vfs.f_bavail;
    DB_LOG("DB", "free_space dir=%s free_kb=%" PRIu64 " free_inodes=%" PRIu64, dir, free_bytes / 1024,
           (uint64_t) vfs.f_favail);
}



enum tag_type {
    tag_artist = 0,
    tag_album,
    tag_genre,
    tag_title,
    tag_filename,
    tag_composer,
    tag_comment,
    tag_albumartist,
    tag_grouping,
    tag_year,
    tag_discnumber,
    tag_tracknumber,
    tag_virt_canonicalartist,
    tag_bitrate,
    tag_length,
    tag_playcount,
    tag_rating,
    tag_playtime,
    tag_lastplayed,
    tag_commitid,
    tag_mtime,
    tag_lastelapsed,
    tag_lastoffset,
    TAG_COUNT
};

#define TAGCACHE_MAGIC 0x54434810
#define TAGCACHE_INDEXED_MAGIC 0x54434811
#define FLAG_DELETED 0x0001
#define FLAG_SEEN 0x01000000 /* RAM-only, stripped before persist */
#define FLAG_TAGS_INCOMPLETE 0x02000000 /* RAM-only: unique-tag seek missed on load */
#define FLAG_RAM_ONLY (FLAG_SEEN | FLAG_TAGS_INCOMPLETE)
#ifndef TAGCACHE_MAX_ENTRIES
#define TAGCACHE_MAX_ENTRIES 524288
#endif
#define TAGCACHE_MAX_STAGING_ENTRIES (TAGCACHE_MAX_ENTRIES * 2)
#define TAGCACHE_NUMERIC_TAGS                                                                                          \
    ((1u << tag_year) | (1u << tag_discnumber) | (1u << tag_tracknumber) | (1u << tag_bitrate) | (1u << tag_length) |  \
     (1u << tag_playcount) | (1u << tag_rating) | (1u << tag_playtime) | (1u << tag_lastplayed) |                      \
     (1u << tag_commitid) | (1u << tag_mtime) | (1u << tag_lastelapsed) | (1u << tag_lastoffset))

struct tagfile_entry {
    int32_t tag_length;
    int32_t idx_id;
};

struct index_entry {
    int32_t tag_seek[TAG_COUNT];
    int32_t flag;
};

struct tagcache_header {
    int32_t magic;
    int32_t datasize;
    int32_t entry_count;
};

struct master_header {
    struct tagcache_header tch;
    int32_t serial;
    int32_t commitid;
    int32_t dirty;
};


/* db_dir is deliberately the proc-fd path for the directory opened at
 * tagcache_open() time.  The logical path is retained only for detecting a
 * card replacement; using it for I/O would reopen whichever card happens to
 * be mounted there now. */
static char db_dir[512];
static char db_logical_dir[512];
static int db_dir_fd = -1;
static struct stat db_dir_identity;
static bool db_dir_identity_valid;
static bool db_open, disk_ready, rebuild_preserve_generations;
static uint32_t committed_migration_state, staged_migration_state;
#define TAGCACHE_MIGRATION_STATE_MAGIC 0x54434d53u
#define TAGCACHE_MIGRATION_STATE_VERSION 1u
static tagcache_load_outcome_t last_load_outcome = TAGCACHE_LOAD_FAILED;
#define READER_SORT_CACHE_ROWS 64
#define TC_QUERY_BLOCK_VALUES 128
typedef struct {
    int32_t entries, live, serial, commitid, generation;
    int master_fd, tag_fd[TAG_COUNT];
    size_t tag_size[TAG_COUNT];
    bool fds_ready, requires_indexes, legacy_active, compact_sort, compact_order, sort_failed;
    int query_fd, artist_names_fd, title_fd, recency_fd, path_fd, rank_fd[2], group_fd[3], members_fd[3], group_n[3], sort_kind;
    bool query_cache_valid;
    uint64_t query_cache_block;
    uint32_t query_cache_values[TC_QUERY_BLOCK_VALUES];
    struct { bool valid; int32_t slot; unsigned loaded; tagcache_song_t song; } sort_cache[READER_SORT_CACHE_ROWS];
} reader_context_t;
static reader_context_t committed_reader, scan_reader, build_reader;
static _Thread_local reader_context_t *selected_reader;
static bool scan_view_ready;
static int32_t *commit_slot_map;
static int32_t commit_slot_map_count;
#define READER (selected_reader ? selected_reader : &committed_reader)
#define ent_count (READER->entries)
#define live_count (READER->live)
#define master_serial (READER->serial)
#define master_commitid (READER->commitid)
#define disk_gen (READER->generation)
#define reader_master_fd (READER->master_fd)
#define reader_tag_fd (READER->tag_fd)
#define reader_tag_size (READER->tag_size)
#define reader_fds_ready (READER->fds_ready)
#define reader_requires_indexes (READER->requires_indexes)
#define reader_legacy_active (READER->legacy_active)
#define reader_compact_sort (READER->compact_sort)
#define reader_compact_order (READER->compact_order)
#define reader_sort_failed (READER->sort_failed)
#define reader_sort_kind (READER->sort_kind)
#define reader_sort_cache (READER->sort_cache)
#define reader_query_fd (READER->query_fd)
#define reader_artist_names_fd (READER->artist_names_fd)
#define reader_title_fd (READER->title_fd)
#define reader_recency_fd (READER->recency_fd)
#define reader_path_fd (READER->path_fd)
#define reader_rank_fd (READER->rank_fd)
#define reader_group_fd (READER->group_fd)
#define reader_members_fd (READER->members_fd)
#define reader_group_n (READER->group_n)
#define reader_query_cache_valid (READER->query_cache_valid)
#define reader_query_cache_block (READER->query_cache_block)
#define reader_query_cache_values (READER->query_cache_values)
static void reader_context_init(reader_context_t *context) {
    memset(context, 0, sizeof(*context));
    context->query_fd = context->artist_names_fd = context->master_fd = context->title_fd = context->recency_fd = context->path_fd = -1;
    for (int i = 0; i < TAG_COUNT; i++) context->tag_fd[i] = -1;
    for (int i = 0; i < 3; i++) context->group_fd[i] = context->members_fd[i] = -1;
    for (int i = 0; i < 2; i++) context->rank_fd[i] = -1;
    context->fds_ready = true;
    context->serial = 1;
    context->query_cache_valid = false;
}
static bool updating;
static _Atomic bool update_failed;
/* A scan starts as a read-only view.  Bits are set by lookups and are only
 * copied into staging if the scan actually needs to publish a generation. */
static unsigned char *scan_seen_bitmap;
static size_t scan_seen_bitmap_bytes;
static bool scan_lazy_active;
static bool scan_force_publication;
static bool scan_preserve_unseen;
static void (*scan_unlock_cb)(void);
static void (*scan_lock_cb)(void);
static inline bool checked_add_size(size_t a, size_t b, size_t * out) {
    if (SIZE_MAX - a < b) return false;
    *out = a + b;
    return true;
}

static inline bool checked_mul_size(size_t a, size_t b, size_t * out) {
    if (a != 0 && b > SIZE_MAX / a) return false;
    *out = a * b;
    return true;
}

static inline bool checked_grow_cap(size_t cur, size_t need, size_t max_cap, size_t * out_cap) {
    if (need > max_cap) return false;
    if (cur >= need) {
        *out_cap = cur;
        return true;
    }
    size_t cap = cur ? cur : 64;
    while (cap < need) {
        if (cap > max_cap / 2) {
            cap = max_cap;
            break;
        }
        cap *= 2;
    }
    *out_cap = cap;
    return true;
}

static uint32_t fnv1a(const char * s, size_t n) {
    if (!s) return 0;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        h ^= (uint8_t) s[i];
        h *= 16777619u;
    }
    return h;
}

static unsigned char ascii_fold(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return (unsigned char) (c + 32);
    return c;
}

static int ascii_casecmp(const char * a, const char * b) {
    if (!a) a = "";
    if (!b) b = "";
    for (;;) {
        unsigned char ca = ascii_fold((unsigned char) *a++);
        unsigned char cb = ascii_fold((unsigned char) *b++);
        if (ca != cb) return (int) ca - (int) cb;
        if (ca == 0) return 0;
    }
}

#define TAGCACHE_SORT_TEXT_PREFIX 40
#define TAGCACHE_SORT_PATH_PREFIX 64

/* Folded byte order matches ascii_casecmp; NUL padding proves equality only when a prefix terminates. */
static bool tagcache_sort_prefix(const char *value, unsigned char *prefix, size_t width) {
    if (!value || !prefix || !width) return false;
    const char *end = memchr(value, '\0', TAGCACHE_PATH_MAX);
    if (!end) return false;
    memset(prefix, 0, width);
    size_t length = (size_t)(end - value);
    size_t copy = length < width ? length : width;
    for (size_t i = 0; i < copy; i++) prefix[i] = ascii_fold((unsigned char)value[i]);
    return true;
}

static int tagcache_sort_prefix_compare(const unsigned char *a, const unsigned char *b,
                                       size_t width, bool *full_compare) {
    int c = memcmp(a, b, width);
    *full_compare = c == 0 && memchr(a, '\0', width) == NULL;
    return c;
}

int tagcache_cmp_ascii(const char * a, const char * b) {
    return ascii_casecmp(a ? a : "", b ? b : "");
}

static const char * ascii_casestr(const char * hay, const char * needle) {
    if (!needle[0]) return hay;
    for (const char * h = hay; *h; h++) {
        const char * p = h;
        const char * n = needle;
        while (*n && ascii_fold((unsigned char) *p) == ascii_fold((unsigned char) *n)) {
            p++;
            n++;
        }
        if (!*n) return h;
        if (!*p) return NULL;
    }
    return NULL;
}

static void db_path(char * out, size_t out_size, const char * name) {
    snprintf(out, out_size, "%s/%s", db_dir, name);
}

/* 1 = logical path still names the pinned directory, 0 = a different
 * directory, -1 = the path could not be statted. Callers that publish must
 * not treat -1 as a clean card change. */
static int db_logical_directory_status(void) {
    if (!db_dir_identity_valid || !db_logical_dir[0]) return -1;
    struct stat st;
    if (stat(db_logical_dir, &st) != 0) return -1;
    if (st.st_dev == db_dir_identity.st_dev && st.st_ino == db_dir_identity.st_ino) return 1;
    return 0;
}

static bool db_identity_current(void) {
    return db_logical_directory_status() == 1;
}

static bool db_pin_directory(const char * dir) {
    if (!dir || !dir[0] || strlen(dir) >= sizeof(db_logical_dir)) return false;
    int fd = open(dir, O_RDONLY | O_DIRECTORY);
    if (fd < 0) return false;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISDIR(st.st_mode)) {
        close(fd);
        return false;
    }
    if (snprintf(db_logical_dir, sizeof(db_logical_dir), "%s", dir) >= (int)sizeof(db_logical_dir) ||
        snprintf(db_dir, sizeof(db_dir), "/proc/self/fd/%d", fd) >= (int)sizeof(db_dir)) {
        close(fd);
        db_logical_dir[0] = db_dir[0] = '\0';
        return false;
    }
    db_dir_fd = fd;
    db_dir_identity = st;
    db_dir_identity_valid = true;
    return true;
}

bool tagcache_storage_current(void) {
    return db_open && db_identity_current();
}

int tagcache_dup_directory_fd(void) {
    return db_dir_fd >= 0 ? dup(db_dir_fd) : -1;
}

static bool write_fully(int fd, const void * buf, size_t n) {
    const unsigned char * p = buf;
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(fd, p + off, n - off);
        if (w <= 0) return false;
        off += (size_t) w;
    }
    return true;
}

static bool close_synced(int fd) {
    bool ok = fsync(fd) == 0;
    if (close(fd) != 0) ok = false;
    return ok;
}

static void migration_state_name(char * out, size_t n, int32_t gen) {
    snprintf(out, n, "migration.g%d", gen);
}

struct migration_state_record { uint32_t magic, version, generation, state; };

static bool write_migration_state(int32_t gen, uint32_t state) {
    char name[64], path[640], tmp[680];
    migration_state_name(name, sizeof(name), gen);
    db_path(path, sizeof(path), name);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    int fd = open(tmp, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    struct migration_state_record record = {TAGCACHE_MIGRATION_STATE_MAGIC, TAGCACHE_MIGRATION_STATE_VERSION,
                                            (uint32_t)gen, state};
    bool ok = fd >= 0 && write_fully(fd, &record, sizeof(record));
    if (fd >= 0 && fsync(fd) != 0) ok = false;
    if (fd >= 0 && close(fd) != 0) ok = false;
    if (!ok) {
        unlink(tmp);
        return false;
    }
    if (rename(tmp, path) != 0) { unlink(tmp); return false; }
    return true;
}

static int read_migration_state(int32_t gen, uint32_t *out_state) {
    char name[64], path[640];
    migration_state_name(name, sizeof(name), gen);
    db_path(path, sizeof(path), name);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return errno == ENOENT ? 0 : -1;
    struct migration_state_record record;
    struct stat st;
    ssize_t n = pread(fd, &record, sizeof(record), 0);
    bool valid = fstat(fd, &st) == 0 && n == (ssize_t)sizeof(record) &&
                 st.st_size == (off_t)sizeof(record) && record.magic == TAGCACHE_MIGRATION_STATE_MAGIC &&
                 record.version == TAGCACHE_MIGRATION_STATE_VERSION && record.generation == (uint32_t)gen &&
                 (record.state & ~(TAGCACHE_MIGRATION_APPLIED | TAGCACHE_MIGRATION_ARCHIVE)) == 0;
    close(fd);
    if (!valid) return -1;
    if (out_state) *out_state = record.state;
    return 1;
}


static const int persist_tag_ids[] = { tag_artist,       tag_album,    tag_genre,     tag_albumartist, tag_composer,
                                       tag_comment,      tag_grouping, tag_virt_canonicalartist, tag_title, tag_filename };
_Static_assert(sizeof(persist_tag_ids) / sizeof(persist_tag_ids[0]) == TAGCACHE_REFS_COUNT,
               "persisted tag list must match generation refs manifest");

static int persisted_tag_index(int tag) {
    for (size_t i = 0; i < sizeof(persist_tag_ids) / sizeof(persist_tag_ids[0]); i++)
        if (persist_tag_ids[i] == tag) return (int)i;
    return -1;
}

static void generation_refs_name(char *out, size_t n, int32_t gen) {
    snprintf(out, n, "tagcache.refs.g%d", gen);
}

static uint32_t generation_refs_checksum(const struct tagcache_generation_refs *refs) {
    const unsigned char *p = (const unsigned char *)refs;
    size_t n = offsetof(struct tagcache_generation_refs, checksum);
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

static bool refs_read_full(int fd, void *buf, size_t size, off_t offset) {
    unsigned char *p = buf;
    size_t done = 0;
    while (done < size) {
        ssize_t n = pread(fd, p + done, size - done, offset + (off_t)done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        done += (size_t)n;
    }
    return true;
}

static bool generation_refs_valid(const struct tagcache_generation_refs *refs, int32_t gen) {
    if (!refs || refs->magic != TAGCACHE_REFS_MAGIC || refs->version != TAGCACHE_REFS_VERSION ||
        refs->generation != gen || refs->checksum != generation_refs_checksum(refs)) return false;
    for (size_t i = 0; i < TAGCACHE_REFS_COUNT; i++)
        if (refs->source[i] < 0 || refs->source[i] > gen) return false;
    return true;
}

/* Resolve one physical tag generation. Missing refs are the legacy format. */
static bool resolve_tag_generation_at(const char *dir, int32_t gen, int tag, int32_t *out_source) {
    if (!out_source || gen < 0) return false;
    int index = persisted_tag_index(tag);
    if (index < 0) return false;
    *out_source = gen;
    if (gen == 0) return true;
    char name[80], path[640];
    generation_refs_name(name, sizeof(name), gen);
    snprintf(path, sizeof(path), "%s/%s", dir ? dir : db_dir, name);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return errno == ENOENT;
    struct tagcache_generation_refs refs;
    struct stat st;
    bool ok = fstat(fd, &st) == 0 && st.st_size == (off_t)sizeof(refs) &&
              refs_read_full(fd, &refs, sizeof(refs), 0) &&
              generation_refs_valid(&refs, gen);
    if (ok) *out_source = refs.source[index];
    close(fd);
    return ok;
}

static bool resolve_tag_generation(int32_t gen, int tag, int32_t *out_source) {
    return resolve_tag_generation_at(db_dir, gen, tag, out_source);
}

static bool write_generation_refs(int32_t gen, const int32_t source[TAGCACHE_REFS_COUNT]) {
    char name[80], path[640], tmp[680];
    generation_refs_name(name, sizeof(name), gen);
    db_path(path, sizeof(path), name);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    struct tagcache_generation_refs refs = {TAGCACHE_REFS_MAGIC, TAGCACHE_REFS_VERSION, gen, {0}, 0};
    for (size_t i = 0; i < TAGCACHE_REFS_COUNT; i++) {
        if (source[i] < 0 || source[i] > gen) return false;
        refs.source[i] = source[i];
    }
    refs.checksum = generation_refs_checksum(&refs);
    int fd = open(tmp, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    bool ok = false;
    if (fd >= 0) {
        bool wrote = write_fully(fd, &refs, sizeof(refs));
        bool synced = close_synced(fd);
        fd = -1;
        ok = wrote && synced;
    }
    if (!ok) {
        if (fd >= 0) close(fd);
        unlink(tmp);
        return false;
    }
    if (rename(tmp, path) != 0) { unlink(tmp); return false; }
    return true;
}

static void tag_file_name(char * out, size_t n, int tag, int32_t gen) {
    if (gen > 0) snprintf(out, n, "database_%d.tcd.g%d", tag, gen);
    else snprintf(out, n, "database_%d.tcd", tag);
}

static void master_file_name(char * out, size_t n, int32_t gen) {
    if (gen > 0) snprintf(out, n, "database_idx.tcd.g%d", gen);
    else snprintf(out, n, "database_idx.tcd");
}

static bool write_gen_pointer(int32_t gen) {
    char path[640], tmp[640];
    db_path(path, sizeof(path), "tagcache.gen");
    db_path(tmp, sizeof(tmp), "tagcache.gen.tmp");
    int fd = open(tmp, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0) {
        DB_LOG("DB", "write_gen_pointer open_failed gen=%d errno=%d(%s) path=%s", gen, errno, strerror(errno), tmp);
        tagcache_log_free_space(db_dir);
        return false;
    }
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%d\n", gen);
    bool ok = n > 0 && write_fully(fd, buf, (size_t) n);
    bool synced = close_synced(fd);
    if (!synced || !ok) {
        DB_LOG("DB", "write_gen_pointer write_failed gen=%d errno=%d(%s) path=%s", gen, errno, strerror(errno), tmp);
        tagcache_log_free_space(db_dir);
        unlink(tmp);
        return false;
    }
    if (rename(tmp, path) != 0) {
        DB_LOG("DB", "write_gen_pointer rename_failed gen=%d errno=%d(%s) from=%s to=%s", gen, errno,
               strerror(errno), tmp, path);
        unlink(tmp);
        return false;
    }
    if (!library_fsync_dir(db_dir)) {
        /* Non-fatal: the rename() above already committed the pointer, so
         * the generation is live regardless. A failed directory fsync only
         * risks losing the directory entry to an immediate power cut. */
        DB_LOG("DB", "write_gen_pointer fsync_dir_failed gen=%d errno=%d(%s) dir=%s (non-fatal, rename already committed)",
               gen, errno, strerror(errno), db_dir);
    }
    return true;
}

static int read_gen_pointer(int32_t * out) {
    char path[640];
    db_path(path, sizeof(path), "tagcache.gen");
    errno = 0;
    FILE * f = fopen(path, "r");
    if (!f) return errno == ENOENT ? 0 : -1;
    char buf[64];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    bool read_error = ferror(f) != 0;
    if (fclose(f) != 0) read_error = true;
    if (read_error) return -1;
    if (n == sizeof(buf) - 1 || memchr(buf, '\0', n)) return -1;
    buf[n] = '\0';
    char * end;
    errno = 0;
    long gen = strtol(buf, &end, 10);
    if (end == buf || errno == ERANGE || gen <= 0 || gen > INT32_MAX) return -1;
    while (isspace((unsigned char) *end)) end++;
    if (*end) return -1;
    *out = (int32_t) gen;
    return 1;
}

static void reader_unlink_indexes(int32_t gen);

static void unlink_generation(int32_t gen) {
    reader_unlink_indexes(gen);
    char name[80], path[640];
    for (size_t t = 0; t < sizeof(persist_tag_ids) / sizeof(persist_tag_ids[0]); t++) {
        tag_file_name(name, sizeof(name), persist_tag_ids[t], gen);
        db_path(path, sizeof(path), name);
        unlink(path);
    }
    master_file_name(name, sizeof(name), gen);
    db_path(path, sizeof(path), name);
    unlink(path);
    migration_state_name(name, sizeof(name), gen);
    db_path(path, sizeof(path), name);
    unlink(path);
    generation_refs_name(name, sizeof(name), gen);
    db_path(path, sizeof(path), name);
    unlink(path);
    char tmp[680];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    unlink(tmp);
}

static void unlink_other_generations(int32_t keep, int32_t previous) {
    char protected_names[TAGCACHE_REFS_COUNT * 2][80];
    size_t protected_count = 0;
    int32_t generations[2] = {keep, previous};
    for (size_t g = 0; g < 2; g++) {
        if (generations[g] < 0 || (g == 1 && generations[1] == generations[0])) continue;
        for (size_t i = 0; i < sizeof(persist_tag_ids) / sizeof(persist_tag_ids[0]); i++) {
            int32_t source;
            if (!resolve_tag_generation(generations[g], persist_tag_ids[i], &source)) return;
            tag_file_name(protected_names[protected_count++], sizeof(protected_names[0]), persist_tag_ids[i], source);
        }
    }
    DIR *d = opendir(db_dir);
    if (!d) return;
    struct dirent * de;
    while ((de = readdir(d)) != NULL) {
        const char * name = de->d_name;
        char path[800];
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        if (strcmp(name, "tagcache.gen") == 0 || strcmp(name, "tagcache.gen.tmp") == 0) continue;
        bool protected = false;
        for (size_t i = 0; i < protected_count; i++)
            if (strcmp(name, protected_names[i]) == 0) { protected = true; break; }
        if (protected) continue;
        static const char refs_prefix[] = "tagcache.refs.g";
        if (strncmp(name, refs_prefix, sizeof(refs_prefix) - 1) == 0) {
            char *end;
            long parsed = strtol(name + sizeof(refs_prefix) - 1, &end, 10);
            if (*end == '\0' && parsed > 0 && parsed <= INT32_MAX &&
                (parsed == keep || parsed == previous)) continue;
            db_path(path, sizeof(path), name);
            unlink(path);
            continue;
        }
        if (strncmp(name, "migration.g", 11) == 0) {
            char *end;
            long gen = strtol(name + 11, &end, 10);
            if (*end == '\0' && gen > 0 && gen != keep && gen != previous) {
                db_path(path, sizeof(path), name);
                unlink(path);
            }
            continue;
        }
        const char *gpos = strstr(name, ".tcd.g");
        if (gpos) {
            int gen = atoi(gpos + 6);
            if ((keep > 0 && gen == keep) || (previous > 0 && gen == previous)) continue;
            db_path(path, sizeof(path), name);
            unlink(path);
            continue;
        }
        size_t n = strlen(name);
        if (n >= 4 && strcmp(name + n - 4, ".new") == 0) {
            db_path(path, sizeof(path), name);
            unlink(path);
            continue;
        }
        if (keep > 0 && (strcmp(name, "database_idx.tcd") == 0 ||
                         (strncmp(name, "database_", 9) == 0 && n >= 4 && strcmp(name + n - 4, ".tcd") == 0))) {
            db_path(path, sizeof(path), name);
            unlink(path);
        }
    }
    closedir(d);
}

/* Find the highest generation number in every generation file, including
 * partial tag-only generations left by an interrupted write. */
static bool scan_generation_max(int32_t * out_max) {
    DIR * d = opendir(db_dir);
    if (!d) return false;
    int32_t max_gen = 0;
    bool ok = true;
    struct dirent * de;
    for (;;) {
        errno = 0;
        de = readdir(d);
        if (!de) {
            if (errno != 0) ok = false;
            break;
        }
        const char * name = de->d_name;
        const char * gpos = strstr(name, ".tcd.g");
        if (!gpos || strncmp(name, "database_", 9) != 0) continue;
        char * end;
        errno = 0;
        long gen = strtol(gpos + 6, &end, 10);
        /* Invalid suffixes cannot collide with a generated filename. */
        if (end == gpos + 6 || *end != '\0' || errno == ERANGE || gen <= 0 || gen > INT32_MAX) continue;
        if ((int32_t) gen > max_gen) max_gen = (int32_t) gen;
    }
    if (closedir(d) != 0) ok = false;
    if (!ok) return false;
    *out_max = max_gen;
    return true;
}

static bool validate_master_header(const struct master_header * mh, size_t file_size) {
    if (!mh) return false;
    if (mh->tch.magic != TAGCACHE_MAGIC && mh->tch.magic != TAGCACHE_INDEXED_MAGIC) return false;
    if (mh->dirty != 0) return false;
    if (mh->tch.entry_count < 0 || mh->tch.entry_count > TAGCACHE_MAX_ENTRIES) return false;
    if (mh->tch.datasize < 0) return false;
    size_t payload_bytes = 0;
    if (!checked_mul_size((size_t) mh->tch.entry_count, sizeof(struct index_entry), &payload_bytes)) return false;
    size_t min_file_size = 0;
    if (!checked_add_size(sizeof(struct master_header), payload_bytes, &min_file_size)) return false;
    if (file_size < min_file_size) return false;
    if (mh->tch.datasize > 0 && (size_t) mh->tch.datasize != payload_bytes) return false;
    return true;
}

static bool validate_tag_header(const struct tagcache_header * hdr, size_t file_size) {
    if (!hdr) return false;
    if (hdr->magic != TAGCACHE_MAGIC) return false;
    if (hdr->entry_count < 0 || hdr->entry_count > TAGCACHE_MAX_ENTRIES) return false;
    if (hdr->datasize < 0) return false;
    if (file_size < sizeof(struct tagcache_header)) return false;
    if (hdr->datasize > 0) {
        size_t min_size = 0;
        if (!checked_add_size(sizeof(struct tagcache_header), (size_t) hdr->datasize, &min_size)) return false;
        if (file_size < min_size) return false;
    }
    return true;
}

struct tagcache_stats_row {
    char * path;
    int32_t rating;
    int32_t playcount;
    int32_t last_played;
};

struct tagcache_stats_snapshot {
    int master_fd;
    int filename_fd;
    struct master_header master;
    struct tagcache_header filename;
    int32_t entry_count;
    /* Compatibility view for existing diagnostics: only the first row is
     * retained; replay itself streams every row from the pinned descriptors. */
    struct tagcache_stats_row first_row;
    struct tagcache_stats_row *rows;
    size_t count;
};

static int compare_load_generations(const void * a, const void * b);

enum stats_read_result { STATS_READ_OK = 1, STATS_READ_EOF = 0, STATS_READ_ERROR = -1 };

static int stats_read_at(int fd, void * buf, size_t size, off_t offset) {
    unsigned char * p = buf;
    size_t done = 0;
    while (done < size) {
        ssize_t n = pread(fd, p + done, size - done, offset + (off_t) done);
        if (n == 0) return STATS_READ_EOF;
        if (n < 0) {
            if (errno == EINTR) continue;
            return STATS_READ_ERROR;
        }
        done += (size_t) n;
    }
    return STATS_READ_OK;
}

static bool stats_path_at(int fd, const struct tagcache_header * hdr, int32_t seek, int32_t row_id,
                          char * out, size_t out_size) {
    if (seek < (int32_t) sizeof(*hdr) || seek > INT32_MAX - (int32_t) sizeof(struct tagfile_entry)) return false;
    size_t end = 0;
    if (!checked_add_size(sizeof(*hdr), (size_t) hdr->datasize, &end) || (size_t) seek >= end) return false;
    struct tagfile_entry te;
    if (stats_read_at(fd, &te, sizeof(te), (off_t) seek) != STATS_READ_OK) return false;
    if (te.idx_id != row_id || te.tag_length <= 1 || te.tag_length > (int32_t) out_size) return false;
    size_t record_end = 0;
    if (!checked_add_size((size_t) seek, sizeof(te), &record_end) ||
        !checked_add_size(record_end, (size_t) te.tag_length, &record_end) || record_end > end)
        return false;
    if (stats_read_at(fd, out, (size_t) te.tag_length, (off_t) seek + (off_t) sizeof(te)) != STATS_READ_OK) return false;
    return out[0] != '\0' && !memchr(out, '\0', (size_t) te.tag_length - 1) && out[te.tag_length - 1] == '\0';
}

enum stats_pointer_result { STATS_POINTER_MISSING, STATS_POINTER_OK, STATS_POINTER_ERROR };

static enum stats_pointer_result stats_read_pointer(const char * dir, int32_t * out) {
    char path[640], buf[64];
    snprintf(path, sizeof(path), "%s/tagcache.gen", dir);
    FILE * f = fopen(path, "r");
    if (!f) return errno == ENOENT ? STATS_POINTER_MISSING : STATS_POINTER_ERROR;
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    bool read_error = ferror(f) != 0;
    bool close_error = fclose(f) != 0;
    if (read_error || close_error) return STATS_POINTER_ERROR;
    if (n == 0 || n >= sizeof(buf) - 1 || memchr(buf, '\0', n)) return STATS_POINTER_MISSING;
    buf[n] = '\0';
    char * end;
    errno = 0;
    long gen = strtol(buf, &end, 10);
    while (isspace((unsigned char) *end)) end++;
    if (end == buf || *end || errno == ERANGE || gen <= 0 || gen > INT32_MAX) return STATS_POINTER_MISSING;
    *out = (int32_t) gen;
    return STATS_POINTER_OK;
}

static bool stats_add_generation(int32_t ** gens, size_t * count, size_t * cap, int32_t gen) {
    for (size_t i = 0; i < *count; i++)
        if ((*gens)[i] == gen) return true;
    if (*count == *cap) {
        size_t next = *cap ? *cap * 2 : 16;
        if (next < *cap || next > SIZE_MAX / sizeof(**gens)) return false;
        int32_t * grown = realloc(*gens, next * sizeof(**gens));
        if (!grown) return false;
        *gens = grown;
        *cap = next;
    }
    (*gens)[(*count)++] = gen;
    return true;
}

static bool stats_collect_generations(const char * dir, int32_t pointed, int32_t ** out, size_t * out_count) {
    DIR * d = opendir(dir);
    if (!d) return false;
    int32_t * gens = NULL;
    size_t count = 0, cap = 0;
    bool ok = true;
    struct dirent * de;
    for (;;) {
        errno = 0;
        de = readdir(d);
        if (!de) {
            if (errno != 0) ok = false;
            break;
        }
        const char * name = de->d_name;
        int32_t gen;
        if (strcmp(name, "database_idx.tcd") == 0) {
            gen = 0;
        } else {
            static const char prefix[] = "database_idx.tcd.g";
            size_t prefix_len = sizeof(prefix) - 1;
            if (strncmp(name, prefix, prefix_len) != 0) continue;
            char * end;
            errno = 0;
            long parsed = strtol(name + prefix_len, &end, 10);
            if (end == name + prefix_len || *end || errno == ERANGE || parsed <= 0 || parsed > INT32_MAX) continue;
            gen = (int32_t) parsed;
        }
        if (gen != pointed && !stats_add_generation(&gens, &count, &cap, gen)) {
            ok = false;
            break;
        }
    }
    if (closedir(d) != 0) ok = false;
    if (!ok) {
        free(gens);
        return false;
    }
    if (count > 1) qsort(gens, count, sizeof(*gens), compare_load_generations);
    *out = gens;
    *out_count = count;
    return true;
}

enum stats_extract_result { STATS_CANDIDATE_BAD, STATS_EXTRACTED, STATS_EXTRACT_FATAL };

static void stats_snapshot_clear(struct tagcache_stats_snapshot * snapshot) {
    if (!snapshot) return;
    if (snapshot->master_fd >= 0) close(snapshot->master_fd);
    if (snapshot->filename_fd >= 0) close(snapshot->filename_fd);
    snapshot->master_fd = snapshot->filename_fd = -1;
    snapshot->entry_count = 0;
    free(snapshot->first_row.path);
    snapshot->first_row.path = NULL;
    snapshot->rows = &snapshot->first_row;
    snapshot->count = 0;
}

static enum stats_extract_result stats_extract_generation(const char * dir, int32_t gen,
                                                          struct tagcache_stats_snapshot * snapshot) {
    char name[80], master_path[640], filename_path[640];
    if (gen > 0) {
        snprintf(name, sizeof(name), "database_idx.tcd.g%d", gen);
        int32_t source_gen;
        /* A refs read/validation error is not an absent candidate: treating it
         * as one could replay stale favorites from an older generation. */
        if (!resolve_tag_generation_at(dir, gen, tag_filename, &source_gen)) return STATS_EXTRACT_FATAL;
        if (source_gen > 0)
            snprintf(filename_path, sizeof(filename_path), "%s/database_%d.tcd.g%d", dir, tag_filename, source_gen);
        else
            snprintf(filename_path, sizeof(filename_path), "%s/database_%d.tcd", dir, tag_filename);
    } else {
        snprintf(name, sizeof(name), "database_idx.tcd");
        snprintf(filename_path, sizeof(filename_path), "%s/database_%d.tcd", dir, tag_filename);
    }
    snprintf(master_path, sizeof(master_path), "%s/%s", dir, name);
    int master_fd = open(master_path, O_RDONLY);
    if (master_fd < 0) return errno == ENOENT ? STATS_CANDIDATE_BAD : STATS_EXTRACT_FATAL;
    struct stat master_st;
    struct master_header mh;
    if (fstat(master_fd, &master_st) != 0) {
        close(master_fd);
        return STATS_EXTRACT_FATAL;
    }
    int read_result = stats_read_at(master_fd, &mh, sizeof(mh), 0);
    if (read_result == STATS_READ_ERROR) {
        close(master_fd);
        return STATS_EXTRACT_FATAL;
    }
    if (read_result != STATS_READ_OK || !validate_master_header(&mh, (size_t) master_st.st_size)) {
        if (close(master_fd) != 0) return STATS_EXTRACT_FATAL;
        return STATS_CANDIDATE_BAD;
    }
    int filename_fd = open(filename_path, O_RDONLY);
    if (filename_fd < 0) {
        int open_errno = errno;
        bool close_failed = close(master_fd) != 0;
        if (close_failed) return STATS_EXTRACT_FATAL;
        return open_errno == ENOENT ? STATS_CANDIDATE_BAD : STATS_EXTRACT_FATAL;
    }
    struct stat filename_st;
    struct tagcache_header filename_hdr;
    if (fstat(filename_fd, &filename_st) != 0) {
        close(filename_fd);
        close(master_fd);
        return STATS_EXTRACT_FATAL;
    }
    read_result = stats_read_at(filename_fd, &filename_hdr, sizeof(filename_hdr), 0);
    if (read_result == STATS_READ_ERROR) {
        close(filename_fd);
        close(master_fd);
        return STATS_EXTRACT_FATAL;
    }
    if (read_result != STATS_READ_OK || !validate_tag_header(&filename_hdr, (size_t) filename_st.st_size)) {
        bool close_failed = close(filename_fd) != 0;
        close_failed = close(master_fd) != 0 || close_failed;
        if (close_failed) return STATS_EXTRACT_FATAL;
        return STATS_CANDIDATE_BAD;
    }
    char path[TAGCACHE_PATH_MAX];
    for (int32_t i = 0; i < mh.tch.entry_count; i++) {
        struct index_entry idx;
        read_result = stats_read_at(master_fd, &idx, sizeof(idx), (off_t) sizeof(mh) + (off_t) i * sizeof(idx));
        if (read_result != STATS_READ_OK) {
            close(filename_fd);
            close(master_fd);
            stats_snapshot_clear(snapshot);
            return STATS_EXTRACT_FATAL;
        }
        if (idx.flag & FLAG_DELETED || (idx.tag_seek[tag_rating] == 0 && idx.tag_seek[tag_playcount] == 0 &&
                                        idx.tag_seek[tag_lastplayed] == 0))
            continue;
        if (!stats_path_at(filename_fd, &filename_hdr, idx.tag_seek[tag_filename], i, path, sizeof(path))) {
            close(filename_fd);
            close(master_fd);
            stats_snapshot_clear(snapshot);
            return STATS_EXTRACT_FATAL;
        }
        /* Reading every historical path here validates the candidate while
         * keeping only the two generation descriptors.  Replay streams rows
         * from those descriptors later instead of duplicating every path. */
        if (!path[0]) {
            close(filename_fd);
            close(master_fd);
            stats_snapshot_clear(snapshot);
            return STATS_EXTRACT_FATAL;
        }
        if (snapshot->count == 0) {
            snapshot->first_row.path = strdup(path);
            if (!snapshot->first_row.path) {
                close(filename_fd);
                close(master_fd);
                stats_snapshot_clear(snapshot);
                return STATS_EXTRACT_FATAL;
            }
            snapshot->first_row.rating = idx.tag_seek[tag_rating];
            snapshot->first_row.playcount = idx.tag_seek[tag_playcount];
            snapshot->first_row.last_played = idx.tag_seek[tag_lastplayed];
        }
        snapshot->count++;
    }
    snapshot->master_fd = master_fd;
    snapshot->filename_fd = filename_fd;
    snapshot->master = mh;
    snapshot->filename = filename_hdr;
    snapshot->entry_count = mh.tch.entry_count;
    snapshot->rows = &snapshot->first_row;
    return STATS_EXTRACTED;
}

bool tagcache_extract_stats(const char * dir, tagcache_stats_snapshot_t ** out) {
    if (!out || !dir || !dir[0]) return false;
    *out = NULL;
    int pinned_dir_fd = open(dir, O_RDONLY | O_DIRECTORY);
    if (pinned_dir_fd < 0) return false;
    char pinned_dir[64];
    if (snprintf(pinned_dir, sizeof(pinned_dir), "/proc/self/fd/%d", pinned_dir_fd) >= (int)sizeof(pinned_dir)) {
        close(pinned_dir_fd);
        return false;
    }
    int32_t pointed = 0;
    enum stats_pointer_result pointer_result = stats_read_pointer(pinned_dir, &pointed);
    if (pointer_result == STATS_POINTER_ERROR) { close(pinned_dir_fd); return false; }
    bool have_pointer = pointer_result == STATS_POINTER_OK;
    int32_t * generations = NULL;
    size_t generation_count = 0;
    if (!stats_collect_generations(pinned_dir, have_pointer ? pointed : -1, &generations, &generation_count)) {
        close(pinned_dir_fd);
        return false;
    }
    struct tagcache_stats_snapshot * snapshot = calloc(1, sizeof(*snapshot));
    if (!snapshot) {
        free(generations);
        close(pinned_dir_fd);
        return false;
    }
    snapshot->master_fd = snapshot->filename_fd = -1;
    bool extracted = false;
    enum stats_extract_result result = STATS_CANDIDATE_BAD;
    if (have_pointer) {
        result = stats_extract_generation(pinned_dir, pointed, snapshot);
        if (result == STATS_EXTRACT_FATAL) {
            free(generations);
            tagcache_free_stats(snapshot);
            close(pinned_dir_fd);
            return false;
        }
        extracted = result == STATS_EXTRACTED;
    }
    for (size_t n = 0; !extracted && n < generation_count; n++) {
        stats_snapshot_clear(snapshot);
        result = stats_extract_generation(pinned_dir, generations[n], snapshot);
        if (result == STATS_EXTRACT_FATAL) {
            free(generations);
            tagcache_free_stats(snapshot);
            close(pinned_dir_fd);
            return false;
        }
        extracted = result == STATS_EXTRACTED;
    }
    free(generations);
    close(pinned_dir_fd);
    if (!extracted) {
        tagcache_free_stats(snapshot);
        return false;
    }
    *out = snapshot;
    return true;
}

/* The reader implementation is included below the migration helpers. */
static int32_t reader_find_path(const char *path);
static bool reader_index(int32_t slot, struct index_entry *out);
static bool reader_song(int32_t slot, tagcache_song_t *out);
static int32_t scan_find_path(const char *path, bool insert, int32_t new_slot);
static bool scan_seen_test(int32_t slot);

static bool tagcache_scan_song_seen(const char *path, tagcache_song_t *out) {
    reader_context_t *saved_reader = selected_reader;
    bool scan_view = scan_lazy_active || scan_view_ready;
    if (scan_view && scan_view_ready) selected_reader = &scan_reader;
    int32_t slot = scan_lazy_active ? reader_find_path(path) : scan_find_path(path, false, -1);
    struct index_entry idx;
    if (slot < 0 || !reader_index(slot, &idx) || (idx.flag & FLAG_DELETED) ||
        (scan_lazy_active ? !scan_seen_test(slot) : !(idx.flag & FLAG_SEEN))) {
        selected_reader = saved_reader;
        return false;
    }
    bool ok = reader_song(slot, out);
    selected_reader = saved_reader;
    return ok;
}

static bool tagcache_replay_stats_impl(const tagcache_stats_snapshot_t * snapshot, size_t * unmatched,
                                       bool reject_unmatched) {
    if (!snapshot) return false;
    (void) reject_unmatched;
    if (unmatched) *unmatched = 0;
    /* Migration replay runs between begin_update() and end_update(); resolve
     * against the staged view so rows scanned in this pass receive history
     * before the first generation publication. */
    reader_context_t *saved_reader = selected_reader;
    if (scan_view_ready) selected_reader = &scan_reader;
    bool ok = true;
    char path[TAGCACHE_PATH_MAX];
    for (int32_t i = 0; i < snapshot->entry_count; i++) {
        struct index_entry idx;
        if (stats_read_at(snapshot->master_fd, &idx, sizeof(idx),
                          (off_t)sizeof(snapshot->master) + (off_t)i * sizeof(idx)) != STATS_READ_OK) {
            selected_reader = saved_reader;
            return false;
        }
        if (idx.flag & FLAG_DELETED || (idx.tag_seek[tag_rating] == 0 && idx.tag_seek[tag_playcount] == 0 &&
                                        idx.tag_seek[tag_lastplayed] == 0)) continue;
        if (!stats_path_at(snapshot->filename_fd, &snapshot->filename, idx.tag_seek[tag_filename], i,
                           path, sizeof(path))) {
            selected_reader = saved_reader;
            return false;
        }
        tagcache_song_t current;
        if (tagcache_scan_song_seen(path, &current)) {
            int32_t playcount = current.playcount > idx.tag_seek[tag_playcount] ? current.playcount : idx.tag_seek[tag_playcount];
            int32_t last_played = current.last_played > idx.tag_seek[tag_lastplayed] ? current.last_played : idx.tag_seek[tag_lastplayed];
            tagcache_overlay_stats(path, idx.tag_seek[tag_rating], playcount, last_played);
            continue;
        }
        struct stat st;
        if (stat(path, &st) == 0 || errno != ENOENT) {
            /* Existing paths omitted by the scan indicate a parser/scan
             * failure, not deletion. Keep migration pending so a later scan
             * can replay their ratings; only ENOENT paths are allowable. */
            ok = false;
            if (unmatched) (*unmatched)++;
        } else if (unmatched) (*unmatched)++;
    }
    selected_reader = saved_reader;
    return ok;
}

bool tagcache_replay_stats(const tagcache_stats_snapshot_t * snapshot) {
    size_t unmatched = 0;
    return tagcache_replay_stats_impl(snapshot, &unmatched, true) && unmatched == 0;
}

bool tagcache_replay_stats_allow_missing(const tagcache_stats_snapshot_t * snapshot, size_t * unmatched) {
    return tagcache_replay_stats_impl(snapshot, unmatched, false);
}

void tagcache_free_stats(tagcache_stats_snapshot_t * snapshot) {
    if (!snapshot) return;
    stats_snapshot_clear(snapshot);
    free(snapshot);
}

int32_t tagcache_generation(void) { return db_open ? disk_gen : 0; }
uint32_t tagcache_migration_state(void) { return db_open ? committed_migration_state : 0; }
void tagcache_set_staged_migration_state(uint32_t state) {
    if (!db_open) return;
    staged_migration_state = state;
    if (scan_lazy_active && state != committed_migration_state) scan_force_publication = true;
}

static int compare_load_generations(const void * a, const void * b) {
    int32_t ga = *(const int32_t *) a;
    int32_t gb = *(const int32_t *) b;
    return (ga < gb) - (ga > gb);
}

static bool collect_load_generations(int32_t ** out, size_t * count, int32_t pointed,
                                     bool * saved_files, bool * pointer_file) {
    DIR * d = opendir(db_dir);
    if (!d) return false;
    int32_t * gens = NULL;
    size_t n = 0, cap = 0;
    bool ok = true;
    struct dirent * de;
    for (;;) {
        errno = 0;
        de = readdir(d);
        if (!de) {
            if (errno != 0) ok = false;
            break;
        }
        const char * name = de->d_name;
        if (strcmp(name, "tagcache.gen") == 0) *pointer_file = true;
        if (strncmp(name, "tagcache.gen", 12) == 0 ||
            (strncmp(name, "database_", 9) == 0 && strstr(name, ".tcd")))
            *saved_files = true;
        int32_t gen;
        if (strcmp(name, "database_idx.tcd") == 0) {
            gen = 0;
        } else {
            const char * prefix = "database_idx.tcd.g";
            size_t len = strlen(prefix);
            if (strncmp(name, prefix, len) != 0) continue;
            char * end;
            errno = 0;
            long g = strtol(name + len, &end, 10);
            if (end == name + len || *end || errno == ERANGE || g <= 0 || g > INT32_MAX) continue;
            gen = (int32_t) g;
        }
        if (gen == pointed && pointed > 0) continue;
        if (n == cap) {
            size_t new_cap = cap ? cap * 2 : 48;
            if (new_cap < cap || new_cap > SIZE_MAX / sizeof(*gens)) {
                ok = false;
                break;
            }
            int32_t * grown = realloc(gens, new_cap * sizeof(*gens));
            if (!grown) {
                ok = false;
                break;
            }
            gens = grown;
            cap = new_cap;
        }
        gens[n++] = gen;
    }
    if (closedir(d) != 0) ok = false;
    if (!ok) {
        free(gens);
        return false;
    }
    if (n > 1) qsort(gens, n, sizeof(*gens), compare_load_generations);
    *out = gens;
    *count = n;
    return true;
}



static void legacy_canonical_text(char *value);
static void numeric_overlay(int32_t slot, struct index_entry *idx);
static atomic_uint numeric_epoch;
static size_t scan_hash_mem_available_bytes(void);
#define TC_SORT_AVAILABLE_BYTES() scan_hash_mem_available_bytes()
#include "tagcache_reader.h"
#include "tagcache_sort.h"
static bool scan_intern_init(bool include_titles);
static bool scan_intern_find(const char *value, char *out, size_t out_size);
static bool write_at(int fd, const void *data, size_t size, off_t offset);
static void scan_seen_release(void);

static void scan_seen_release(void) {
    free(scan_seen_bitmap);
    scan_seen_bitmap = NULL;
    scan_seen_bitmap_bytes = 0;
    scan_lazy_active = false;
    scan_force_publication = false;
    scan_unlock_cb = NULL;
    scan_lock_cb = NULL;
}

static bool scan_seen_test(int32_t slot) {
    return slot >= 0 && (size_t)slot < scan_seen_bitmap_bytes * 8u &&
           scan_seen_bitmap && (scan_seen_bitmap[(size_t)slot >> 3] & (1u << ((unsigned)slot & 7u)));
}

static void scan_seen_set(int32_t slot) {
    if (slot >= 0 && (size_t)slot < scan_seen_bitmap_bytes * 8u && scan_seen_bitmap)
        scan_seen_bitmap[(size_t)slot >> 3] |= (unsigned char)(1u << ((unsigned)slot & 7u));
}
static bool query_build(int output_fd);
static bool query_validate(int fd, uint64_t file_size);
#include "tagcache_reader_index.h"
#include "tagcache_query_index.h"
#include "tagcache_query.h"
#include "tagcache_reader_legacy.h"
#include "tagcache_scan_intern.h"
/* Scan and commit work use the existing worker's selected reader. */
static bool scan_initializing;
#define scan_selected (selected_reader == &scan_reader)
static void scan_select(bool selected) {
    if (scan_view_ready) selected_reader = selected ? &scan_reader : &committed_reader;
}
typedef struct {
    reader_context_t *previous;
} scan_scope_t;
static void scan_scope_leave(scan_scope_t *scope) {
    if (scope) selected_reader = scope->previous;
}
#define SCAN_SCOPE scan_scope_t scan_scope __attribute__((cleanup(scan_scope_leave))) = { selected_reader }; scan_select(true)

#define NUMERIC_QUEUE_CAP 64
#define NUMERIC_COMMIT_DELAY_SEC 2

typedef struct {
    int32_t gen;
    int32_t slot;
    int32_t mtime;
    int32_t size;
    int32_t first_seen;
    int32_t playcount;
    int32_t last_played;
    int32_t rating;
    int32_t disc_number;
    int32_t track_number;
    int32_t flag;
} numeric_update_t;

static numeric_update_t numeric_queue[NUMERIC_QUEUE_CAP];
static int numeric_queue_count = 0;
static numeric_update_t numeric_inflight[NUMERIC_QUEUE_CAP];
static int numeric_inflight_count = 0;
static bool numeric_inflight_active;
static bool numeric_write_failed;
/* A rejected enqueue stays visible after later accepted writes succeed.
 * Cleared only when a new database session starts. */
static bool numeric_overflow_failed;
static pthread_mutex_t numeric_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t numeric_cond = PTHREAD_COND_INITIALIZER;
static pthread_cond_t numeric_drain_cond = PTHREAD_COND_INITIALIZER;
static pthread_t numeric_thread;
static bool numeric_thread_running = false;
static bool numeric_shutdown = false;

/* Performs disk pwrite + fdatasync for a snapshot of numeric updates. */
static bool flush_numeric_batch(const numeric_update_t * batch, int n) {
    if (n <= 0 || db_dir[0] == '\0') return true;
    /* A queued update belongs to the directory pinned when it was queued.
     * A different directory discards the batch. A failed stat keeps it:
     * the pinned fd may still be the right card, and success would hide
     * the loss. */
    int identity = db_logical_directory_status();
    if (identity < 0) return false;
    if (identity == 0) return true;
    bool ok = true;
    for (int i = 0; i < n; i++) {
        int32_t gen = batch[i].gen;
        if (gen < 0) continue;
        /* Avoid repeating work for the same generation in this batch */
        bool already_done = false;
        for (int k = 0; k < i; k++) {
            if (batch[k].gen == gen) {
                already_done = true;
                break;
            }
        }
        if (already_done) continue;

        char master_name[80], master_path[640];
        master_file_name(master_name, sizeof(master_name), gen);
        db_path(master_path, sizeof(master_path), master_name);
        int fd = open(master_path, O_RDWR);
        if (fd < 0) { ok = false; continue; }
        struct stat st;
        if (fstat(fd, &st) != 0) {
            close(fd);
            ok = false; continue;
        }
        struct master_header mh;
        if (pread(fd, &mh, sizeof(mh), 0) != (ssize_t) sizeof(mh) || !validate_master_header(&mh, (size_t) st.st_size)) {
            close(fd);
            ok = false; continue;
        }
        bool any_written = false;
        for (int j = i; j < n; j++) {
            if (batch[j].gen != gen) continue;
            int32_t slot = batch[j].slot;
            if (slot < 0 || slot >= mh.tch.entry_count) continue;
            struct index_entry ie;
            off_t off = (off_t) sizeof(struct master_header) + (off_t) slot * (off_t) sizeof(ie);
            if (pread(fd, &ie, sizeof(ie), off) != (ssize_t) sizeof(ie)) { ok = false; continue; }
            ie.tag_seek[tag_playcount] = batch[j].playcount;
            ie.tag_seek[tag_lastplayed] = batch[j].last_played;
            ie.tag_seek[tag_rating] = batch[j].rating;
            if (pwrite(fd, &ie, sizeof(ie), off) == (ssize_t) sizeof(ie)) {
                any_written = true;
            } else ok = false;
        }
        if (any_written) {
            if (fdatasync(fd) != 0) ok = false;
        }
        close(fd);
    }
    return ok;
}

static bool numeric_take_batch_locked(void) {
    if (numeric_queue_count == 0 || numeric_inflight_active) return false;
    numeric_inflight_count = numeric_queue_count;
    memcpy(numeric_inflight, numeric_queue, sizeof(numeric_update_t) * (size_t) numeric_queue_count);
    numeric_queue_count = 0;
    numeric_inflight_active = true;
    pthread_cond_broadcast(&numeric_drain_cond);
    return true;
}

static void numeric_retain_failed_locked(void) {
    for (int i = 0; i < numeric_inflight_count; i++) {
        numeric_update_t *u = &numeric_inflight[i];
        bool replaced = false;
        for (int j = numeric_queue_count - 1; j >= 0; j--)
            if (numeric_queue[j].gen == u->gen && numeric_queue[j].slot == u->slot) {
                /* A newer queued value supersedes the failed batch entry. */
                replaced = true;
                break;
            }
        if (!replaced && numeric_queue_count < NUMERIC_QUEUE_CAP)
            numeric_queue[numeric_queue_count++] = *u;
        else if (!replaced) {
            /* The combined cap below makes this unreachable. If it ever
             * happens, keep the failure latched instead of forgetting it
             * on the next successful drain. */
            numeric_write_failed = true;
            numeric_overflow_failed = true;
        }
    }
}

static bool tagcache_flush_numeric(void) {
    for (;;) {
        pthread_mutex_lock(&numeric_mutex);
        while (numeric_inflight_active) pthread_cond_wait(&numeric_drain_cond, &numeric_mutex);
        if (!numeric_take_batch_locked()) { bool ok = !numeric_write_failed; pthread_mutex_unlock(&numeric_mutex); return ok; }
        int n = numeric_inflight_count;
        numeric_update_t batch[NUMERIC_QUEUE_CAP];
        memcpy(batch, numeric_inflight, sizeof(batch));
        pthread_mutex_unlock(&numeric_mutex);
        bool ok = flush_numeric_batch(batch, n);
        pthread_mutex_lock(&numeric_mutex);
        numeric_write_failed |= !ok;
        if (!ok) numeric_retain_failed_locked();
        else if (numeric_queue_count == 0) numeric_write_failed = false;
        numeric_inflight_active = false;
        numeric_inflight_count = 0;
        atomic_fetch_add_explicit(&numeric_epoch, 1, memory_order_release);
        pthread_cond_broadcast(&numeric_drain_cond);
        pthread_mutex_unlock(&numeric_mutex);
        if (!ok) return false;
    }
}

static void * numeric_worker_func(void * arg) {
    (void) arg;
    pthread_mutex_lock(&numeric_mutex);
    while (!numeric_shutdown) {
        if (numeric_queue_count == 0) {
            pthread_cond_wait(&numeric_cond, &numeric_mutex);
            continue;
        }
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += NUMERIC_COMMIT_DELAY_SEC;
        int rc = pthread_cond_timedwait(&numeric_cond, &numeric_mutex, &ts);
        if (rc == ETIMEDOUT || numeric_shutdown || numeric_queue_count >= NUMERIC_QUEUE_CAP) {
            if (!numeric_take_batch_locked()) continue;
            int n = numeric_inflight_count;
            numeric_update_t batch[NUMERIC_QUEUE_CAP];
            memcpy(batch, numeric_inflight, sizeof(batch));
            pthread_mutex_unlock(&numeric_mutex);
            bool ok = flush_numeric_batch(batch, n);
            pthread_mutex_lock(&numeric_mutex);
            numeric_write_failed |= !ok;
            if (!ok) numeric_retain_failed_locked();
            else if (numeric_queue_count == 0) numeric_write_failed = false;
            numeric_inflight_active = false;
            numeric_inflight_count = 0;
            atomic_fetch_add_explicit(&numeric_epoch, 1, memory_order_release);
            pthread_cond_broadcast(&numeric_drain_cond);
        }
    }
    if (numeric_take_batch_locked()) {
        int n = numeric_inflight_count;
        numeric_update_t batch[NUMERIC_QUEUE_CAP];
        memcpy(batch, numeric_inflight, sizeof(batch));
        pthread_mutex_unlock(&numeric_mutex);
        bool ok = flush_numeric_batch(batch, n);
        pthread_mutex_lock(&numeric_mutex);
        numeric_write_failed |= !ok;
        if (!ok) numeric_retain_failed_locked();
        else if (numeric_queue_count == 0) numeric_write_failed = false;
        numeric_inflight_active = false;
        numeric_inflight_count = 0;
        atomic_fetch_add_explicit(&numeric_epoch, 1, memory_order_release);
    }
    pthread_mutex_unlock(&numeric_mutex);
    return NULL;
}

static void numeric_worker_start(void) {
    pthread_mutex_lock(&numeric_mutex);
    if (!numeric_thread_running) {
        numeric_shutdown = false;
        numeric_queue_count = 0;
        numeric_inflight_count = 0;
        numeric_inflight_active = false;
        numeric_write_failed = false;
        numeric_overflow_failed = false;
        const char * disable_env = getenv("TAGCACHE_TEST_DISABLE_NUMERIC_WORKER");
        if (disable_env && atoi(disable_env) != 0) {
            numeric_thread_running = false;
        } else if (pthread_create(&numeric_thread, NULL, numeric_worker_func, NULL) == 0) {
            numeric_thread_running = true;
        } else {
            fprintf(stderr, "tagcache: failed to spawn numeric worker thread -- running in degraded sync mode\n");
            numeric_thread_running = false;
        }
    }
    pthread_mutex_unlock(&numeric_mutex);
}

static void numeric_worker_stop(void) {
    pthread_mutex_lock(&numeric_mutex);
    if (numeric_thread_running) {
        numeric_shutdown = true;
        pthread_cond_broadcast(&numeric_cond);
        pthread_cond_broadcast(&numeric_drain_cond);
        pthread_mutex_unlock(&numeric_mutex);
        pthread_join(numeric_thread, NULL);
        pthread_mutex_lock(&numeric_mutex);
        numeric_thread_running = false;
        numeric_shutdown = false;
    }
    pthread_mutex_unlock(&numeric_mutex);
    tagcache_flush_numeric();
    pthread_mutex_lock(&numeric_mutex);
    numeric_queue_count = 0;
    pthread_mutex_unlock(&numeric_mutex);
}

static void queue_numeric_update(int32_t slot, const struct index_entry *idx) {
    if (!disk_ready || disk_gen < 0) return;
    pthread_mutex_lock(&numeric_mutex);
    for (int i = numeric_queue_count - 1; i >= 0; i--) {
        if (numeric_queue[i].gen == disk_gen && numeric_queue[i].slot == slot) {
            numeric_queue[i].playcount = idx->tag_seek[tag_playcount];
            numeric_queue[i].last_played = idx->tag_seek[tag_lastplayed];
            numeric_queue[i].rating = idx->tag_seek[tag_rating];
            if (numeric_thread_running) pthread_cond_signal(&numeric_cond);
            pthread_mutex_unlock(&numeric_mutex);
            return;
        }
    }
    /* Queue and in-flight batches share one cap, so a failed flush can
     * always copy its batch back. A distinct slot past that cap is rejected
     * without blocking the caller. */
    if (numeric_queue_count + numeric_inflight_count >= NUMERIC_QUEUE_CAP) {
        numeric_overflow_failed = true;
        pthread_mutex_unlock(&numeric_mutex);
        return;
    }
    numeric_update_t *u = &numeric_queue[numeric_queue_count++];
    *u = (numeric_update_t) {.gen = disk_gen, .slot = slot,
        .mtime = idx->tag_seek[tag_mtime], .size = idx->tag_seek[tag_lastoffset],
        .first_seen = idx->tag_seek[tag_commitid], .playcount = idx->tag_seek[tag_playcount],
        .last_played = idx->tag_seek[tag_lastplayed], .rating = idx->tag_seek[tag_rating],
        .disc_number = idx->tag_seek[tag_discnumber], .track_number = idx->tag_seek[tag_tracknumber],
        .flag = idx->flag};
    bool sync = !numeric_thread_running;
    if (!sync) pthread_cond_signal(&numeric_cond);
    pthread_mutex_unlock(&numeric_mutex);
    if (sync) tagcache_flush_numeric();
}

static void numeric_overlay(int32_t slot, struct index_entry *idx) {
    pthread_mutex_lock(&numeric_mutex);
    for (int i = numeric_queue_count - 1; i >= 0; i--)
        if (numeric_queue[i].gen == disk_gen && numeric_queue[i].slot == slot) {
            idx->tag_seek[tag_playcount] = numeric_queue[i].playcount;
            idx->tag_seek[tag_lastplayed] = numeric_queue[i].last_played;
            idx->tag_seek[tag_rating] = numeric_queue[i].rating;
            pthread_mutex_unlock(&numeric_mutex);
            return;
        }
    for (int i = numeric_inflight_count - 1; i >= 0; i--)
        if (numeric_inflight[i].gen == disk_gen && numeric_inflight[i].slot == slot) {
            idx->tag_seek[tag_playcount] = numeric_inflight[i].playcount;
            idx->tag_seek[tag_lastplayed] = numeric_inflight[i].last_played;
            idx->tag_seek[tag_rating] = numeric_inflight[i].rating;
            break;
        }
    pthread_mutex_unlock(&numeric_mutex);
}

bool tagcache_numeric_write_failed(void) {
    pthread_mutex_lock(&numeric_mutex);
    bool failed = numeric_write_failed || numeric_overflow_failed;
    pthread_mutex_unlock(&numeric_mutex);
    return failed;
}
/* Scan mutations use unlinked staging files in the database directory. */
#define SCAN_HASH_SLOTS (TAGCACHE_MAX_ENTRIES * 4u)
static int scan_hash_fd = -1, scan_numeric_fd = -1;
static int32_t *scan_hash_mem;
static bool scan_tag_private[TAG_COUNT];

/* The scan hash is deliberately bounded to the same 8 MiB footprint as the
 * old temporary file.  Keep a conservative reserve for the rest of the
 * scan, and retain the file-backed path when memory is tight or allocation
 * fails.  Tests can force the fallback deterministically. */
#define SCAN_HASH_MEM_RESERVE (16ull * 1024ull * 1024ull)
#define SCAN_HASH_MEM_MAX (8ull * 1024ull * 1024ull)
static bool write_at(int fd, const void *data, size_t size, off_t offset);

/* mem_available_bytes() intentionally has a generous fallback for unrelated
 * index heuristics.  Hash admission must fail closed when availability is
 * unknown, since allocating the full table is optional. */
static size_t scan_hash_mem_available_bytes(void) {
    FILE *f = fopen("/proc/meminfo", "r");
    if (f) {
        char line[160];
        size_t avail = 0, memfree = 0, cached = 0, shmem = 0;
        while (fgets(line, sizeof(line), f)) {
            unsigned long value = 0;
            if (sscanf(line, "MemAvailable: %lu", &value) == 1)
                avail = (size_t) value * 1024u;
            else if (sscanf(line, "MemFree: %lu", &value) == 1)
                memfree = (size_t) value * 1024u;
            else if (sscanf(line, "Cached: %lu", &value) == 1)
                cached = (size_t) value * 1024u;
            else if (sscanf(line, "Shmem: %lu", &value) == 1)
                shmem = (size_t) value * 1024u;
        }
        fclose(f);
        if (avail) return avail;
        if (cached > shmem) cached -= shmem; else cached = 0;
        if (memfree > SIZE_MAX - cached) return SIZE_MAX;
        if (memfree || cached) return memfree + cached;
    }
#ifdef _SC_AVPHYS_PAGES
    {
        long pages = sysconf(_SC_AVPHYS_PAGES);
        long page_size = sysconf(_SC_PAGESIZE);
        if (pages > 0 && page_size > 0 && (uintmax_t) pages <= SIZE_MAX / (size_t) page_size)
            return (size_t) pages * (size_t) page_size;
    }
#endif
    return 0;
}

static void scan_hash_release(void) {
    free(scan_hash_mem);
    scan_hash_mem = NULL;
    if (scan_hash_fd >= 0) close(scan_hash_fd);
    scan_hash_fd = -1;
}

static bool scan_hash_read(uint32_t bucket, int32_t *stored) {
    if (scan_hash_mem) {
        *stored = scan_hash_mem[bucket];
        return true;
    }
    return stats_read_at(scan_hash_fd, stored, sizeof(*stored), (off_t) bucket * sizeof(*stored)) == STATS_READ_OK;
}

static bool scan_hash_write(uint32_t bucket, int32_t stored) {
    if (scan_hash_mem) {
        scan_hash_mem[bucket] = stored;
        return true;
    }
    return write_at(scan_hash_fd, &stored, sizeof(stored), (off_t) bucket * sizeof(stored));
}

static bool write_at(int fd, const void *data, size_t size, off_t offset) {
    const unsigned char *p = data;
    size_t done = 0;
    while (done < size) {
        ssize_t n = pwrite(fd, p + done, size - done, offset + (off_t) done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        done += (size_t) n;
    }
    return true;
}

static bool copy_fd(int from, int to) {
    char buf[32768];
    off_t off = 0;
    for (;;) {
        ssize_t n = pread(from, buf, sizeof(buf), off);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return false;
        if (n == 0) return true;
        if (!write_at(to, buf, (size_t) n, off)) return false;
        off += n;
    }
}

static bool scan_write_index(int32_t slot, const struct index_entry *idx) {
    bool ok = write_at(reader_master_fd, idx, sizeof(*idx),
                       sizeof(struct master_header) + (off_t) slot * sizeof(*idx));
    if (!ok) update_failed = true;
    return ok;
}

static int32_t scan_find_path(const char *path, bool insert, int32_t new_slot) {
    uint32_t bucket = fnv1a(path, strlen(path)) & (SCAN_HASH_SLOTS - 1);
    for (uint32_t probe = 0; probe < SCAN_HASH_SLOTS; probe++) {
        int32_t stored;
        if (!scan_hash_read(bucket, &stored)) {
            update_failed = true;
            return -1;
        }
        if (!stored) {
            if (insert) {
                stored = new_slot + 1;
                if (!scan_hash_write(bucket, stored)) update_failed = true;
            }
            return -1;
        }
        struct index_entry idx;
        char existing[TAGCACHE_PATH_MAX];
        if (!reader_index(stored - 1, &idx) ||
            !reader_string(tag_filename, idx.tag_seek[tag_filename], stored - 1, existing, sizeof(existing))) {
            update_failed = true;
            return -1;
        }
        if (strcmp(existing, path) == 0) return stored - 1;
        bucket = (bucket + 1) & (SCAN_HASH_SLOTS - 1);
    }
    update_failed = true;
    return -1;
}

static bool replay_scan_numeric(int fd) {
    struct stat journal_stat;
    if (fstat(scan_numeric_fd, &journal_stat) != 0) return false;
    for (off_t at = 0; at < journal_stat.st_size; at += 4 * sizeof(int32_t)) {
        int32_t change[4];
        struct index_entry idx;
        if (!reader_read_at(scan_numeric_fd, change, sizeof(change), at) ||
            change[0] < 0 || change[0] >= scan_reader.entries) return false;
        int32_t target_slot = change[0];
        if (commit_slot_map) {
            if (change[0] >= commit_slot_map_count || commit_slot_map[change[0]] < 0) continue;
            target_slot = commit_slot_map[change[0]];
        }
        off_t offset = sizeof(struct master_header) + (off_t)target_slot * sizeof(idx);
        if (!reader_read_at(fd, &idx, sizeof(idx), offset)) return false;
        idx.tag_seek[tag_rating] = change[1]; idx.tag_seek[tag_playcount] = change[2];
        idx.tag_seek[tag_lastplayed] = change[3];
        if (!write_at(fd, &idx, sizeof(idx), offset)) return false;
    }
    return true;
}

static bool start_staging_with_lock(void (*unlock)(void), void (*lock)(void)) {
    if (updating) return !update_failed;
    bool had_lazy_scan = scan_lazy_active;
    updating = true;
    update_failed = false;
    int master = -1, strings[TAG_COUNT];
    for (int t = 0; t < TAG_COUNT; t++) strings[t] = -1;
    memset(scan_tag_private, 0, sizeof(scan_tag_private));
    bool ok = (mkdir(db_dir, 0755) == 0 || errno == EEXIST);
    if (ok) ok = (master = reader_create_temp()) >= 0;
    if (ok) ok = (scan_numeric_fd = reader_create_temp()) >= 0;
    scan_initializing = true;
    if (unlock) unlock();
    struct master_header mh = {{TAGCACHE_MAGIC, ent_count * (int32_t) sizeof(struct index_entry), ent_count},
                                master_serial, master_commitid, 0};
    if (ok) ok = reader_master_fd >= 0 ? copy_fd(reader_master_fd, master) : write_at(master, &mh, sizeof(mh), 0);
    const int tags[] = {tag_artist, tag_album, tag_genre, tag_albumartist, tag_title, tag_filename};
    for (size_t i = 0; ok && i < sizeof(tags) / sizeof(tags[0]); i++) {
        int tag = tags[i];
        if (reader_tag_fd[tag] >= 0) {
            strings[tag] = dup(reader_tag_fd[tag]);
            scan_tag_private[tag] = false;
        } else {
            strings[tag] = reader_create_temp();
            struct tagcache_header hdr = {TAGCACHE_MAGIC, 0, 0};
            scan_tag_private[tag] = true;
            ok = strings[tag] >= 0 && write_at(strings[tag], &hdr, sizeof(hdr), 0);
        }
        ok = ok && strings[tag] >= 0;
    }
    if (ok) {
        const char *disable_ram_hash = getenv("TAGCACHE_DISABLE_RAM_HASH");
        size_t hash_bytes = (size_t) SCAN_HASH_SLOTS * sizeof(int32_t);
        bool ram_hash_ok = hash_bytes <= SCAN_HASH_MEM_MAX &&
                           (!disable_ram_hash || disable_ram_hash[0] == '\0' || disable_ram_hash[0] == '0') &&
                           scan_hash_mem_available_bytes() >= hash_bytes + SCAN_HASH_MEM_RESERVE;
        if (ram_hash_ok) {
            scan_hash_mem = malloc(hash_bytes);
            if (scan_hash_mem) memset(scan_hash_mem, 0, hash_bytes);
            ram_hash_ok = scan_hash_mem != NULL;
        }
        if (!ram_hash_ok) {
            scan_hash_fd = reader_create_temp();
            ok = scan_hash_fd >= 0 && ftruncate(scan_hash_fd, (off_t) hash_bytes) == 0;
        }
    }
    if (!ok) {
        if (master >= 0) close(master);
        for (int t = 0; t < TAG_COUNT; t++) if (strings[t] >= 0) close(strings[t]);
        memset(scan_tag_private, 0, sizeof(scan_tag_private));
        scan_hash_release();
        if (lock) lock();
        scan_initializing = false;
        update_failed = true;
        return false;
    }
    reader_context_init(&scan_reader);
    scan_reader.master_fd = master;
    scan_reader.entries = ent_count; scan_reader.live = live_count;
    scan_reader.serial = master_serial; scan_reader.commitid = master_commitid; scan_reader.generation = disk_gen;
    memcpy(scan_reader.tag_fd, strings, sizeof(strings));
    memcpy(scan_reader.tag_size, reader_tag_size, sizeof(scan_reader.tag_size));
    scan_reader.compact_order = reader_compact_order;
    selected_reader = &scan_reader;
    if (!scan_intern_init(!reader_compact_order)) update_failed = true;
    for (int32_t slot = 0; slot < ent_count && !update_failed; slot++) {
        struct index_entry idx;
        if (!reader_index(slot, &idx)) { update_failed = true; break; }
        idx.flag &= ~FLAG_SEEN;
        if (!scan_write_index(slot, &idx)) break;
        if (idx.flag & FLAG_DELETED) continue;
        char path[TAGCACHE_PATH_MAX];
        if (!reader_string(tag_filename, idx.tag_seek[tag_filename], slot, path, sizeof(path))) {
            update_failed = true;
            break;
        }
        scan_find_path(path, true, slot);
    }
    /* The lazy session has no staged index to carry the seen state.  Apply it
     * after the copied rows and hash are ready, so subsequent mutations use
     * the normal staged reader. */
    if (ok && had_lazy_scan) {
        for (int32_t slot = 0; slot < ent_count && !update_failed; slot++) {
            if (!scan_seen_test(slot)) continue;
            struct index_entry idx;
            if (!reader_index(slot, &idx)) { update_failed = true; break; }
            idx.flag |= FLAG_SEEN;
            if (!scan_write_index(slot, &idx)) break;
        }
    }
    if (lock) lock();
    if (!replay_scan_numeric(reader_master_fd)) update_failed = true;
    scan_initializing = false;
    scan_view_ready = true;
    scan_lazy_active = false;
    scan_unlock_cb = NULL;
    scan_lock_cb = NULL;
    selected_reader = &committed_reader;
    return !update_failed;
}

static bool start_staging(void) {
    void (*unlock)(void) = scan_unlock_cb;
    void (*lock)(void) = scan_lock_cb;
    return start_staging_with_lock(unlock, lock);
}

static int32_t find_path(const char *path) {
    if (!path || !db_open) return -1;
    return scan_selected ? scan_find_path(path, false, -1) : reader_find_path(path);
}

static bool append_string(int tag, int32_t slot, const char *value, int32_t *seek) {
    if (!value) value = "";
    size_t len = strlen(value) + 1;
    if (len > TAGCACHE_PATH_MAX) return false;
    if (!scan_tag_private[tag]) {
        int private_fd = reader_create_temp();
        if (private_fd < 0 || !copy_fd(reader_tag_fd[tag], private_fd)) {
            if (private_fd >= 0) close(private_fd);
            return false;
        }
        close(reader_tag_fd[tag]);
        reader_tag_fd[tag] = private_fd;
        scan_tag_private[tag] = true;
    }
    off_t off = lseek(reader_tag_fd[tag], 0, SEEK_END);
    if (off < (off_t) sizeof(struct tagcache_header) || off > INT32_MAX - (off_t) sizeof(struct tagfile_entry) - (off_t) len)
        return false;
    struct tagfile_entry te = {(int32_t) len, slot};
    if (!write_at(reader_tag_fd[tag], &te, sizeof(te), off) ||
        !write_at(reader_tag_fd[tag], value, len, off + sizeof(te))) return false;
    *seek = (int32_t) off;
    reader_tag_size[tag] = (size_t) off + sizeof(te) + len;
    return true;
}

static void tagcache_begin_update_mode_with_lock(void (*unlock)(void), void (*lock)(void),
                                                 bool preserve_unseen) {
    if (!db_open) return;
    if (!tagcache_flush_numeric()) { update_failed = true; return; }
    /* A second begin belongs to the same scan, including a still-lazy scan;
     * restarting here would discard the RAM seen set. */
    if (updating || scan_lazy_active) return;
    scan_seen_release();
    scan_preserve_unseen = preserve_unseen;
    scan_unlock_cb = unlock;
    scan_lock_cb = lock;
    scan_force_publication = !disk_ready || rebuild_preserve_generations ||
                             last_load_outcome == TAGCACHE_LOAD_SUCCESS_RECOVERED;
    size_t bytes = ((size_t)ent_count + 7u) / 8u;
    if (bytes) scan_seen_bitmap = calloc(1, bytes);
    if (bytes && !scan_seen_bitmap) {
        /* The bitmap is an optimization.  Preserve the old safe behavior if
         * memory is tight rather than making a scan fail. */
        start_staging_with_lock(unlock, lock);
        return;
    }
    scan_seen_bitmap_bytes = bytes;
    scan_lazy_active = true;
}
void tagcache_begin_update_with_lock(void (*unlock)(void), void (*lock)(void)) {
    tagcache_begin_update_mode_with_lock(unlock, lock, false);
}
void tagcache_begin_update(void) { tagcache_begin_update_with_lock(NULL, NULL); }
void tagcache_begin_targeted_update_with_lock(void (*unlock)(void), void (*lock)(void)) {
    tagcache_begin_update_mode_with_lock(unlock, lock, true);
}

bool tagcache_lookup(const char *path, int32_t mtime, int32_t size, tagcache_song_t *out) {
    if (!db_open || !path) return false;
    SCAN_SCOPE;
    int32_t slot = find_path(path);
    struct index_entry idx;
    if (slot < 0 || !reader_index(slot, &idx) || (idx.flag & FLAG_DELETED)) return false;
    if (scan_selected) {
        idx.flag |= FLAG_SEEN;
        if (!scan_write_index(slot, &idx)) return false;
    } else if (scan_lazy_active) {
        scan_seen_set(slot);
    }
    if (idx.tag_seek[tag_mtime] != mtime || idx.tag_seek[tag_lastoffset] != size) return false;
    tagcache_song_t song;
    if (!reader_song(slot, out ? out : &song)) return false;
    if (out) {
        struct index_entry latest;
        if (reader_index(slot, &latest)) {
            numeric_overlay(slot, &latest);
            out->playcount = latest.tag_seek[tag_playcount];
            out->last_played = latest.tag_seek[tag_lastplayed];
            out->rating = latest.tag_seek[tag_rating];
        }
    }
    if (!(out ? out : &song)->album[0] || ((out ? out : &song)->flags & FLAG_TAGS_INCOMPLETE)) return false;
    return true;
}

void tagcache_upsert(const char *path, int32_t mtime, int32_t size, const char *title, const char *artist,
                     const char *album, const char *album_artist, const char *genre,
                     int32_t track_number, int32_t disc_number) {
    tagcache_upsert_changed(path, mtime, size, title, artist, album, album_artist, genre,
                            track_number, disc_number, NULL);
}

void tagcache_upsert_changed(const char *path, int32_t mtime, int32_t size, const char *title,
                             const char *artist, const char *album, const char *album_artist,
                             const char *genre, int32_t track_number, int32_t disc_number,
                             bool *out_tags_changed) {
    /* Anything short of a completed write with identical stored tags counts
     * as changed, so a failed upsert can never pass for a no-op. */
    if (out_tags_changed) *out_tags_changed = true;
    if (!db_open || !path || !path[0] || !start_staging() || update_failed) return;
    SCAN_SCOPE;
    int32_t slot = find_path(path);
    struct index_entry idx = {0};
    bool fresh = slot < 0;
    if (fresh) {
        if (update_failed) return;
        /* Keep targeted-transaction slots stable while numeric updates are
         * journaled by slot. Full scans compact tombstones at publication. */
        if (ent_count >= TAGCACHE_MAX_STAGING_ENTRIES) { update_failed = true; return; }
        slot = ent_count;
        idx.tag_seek[tag_commitid] = (int32_t) time(NULL);
    } else if (!reader_index(slot, &idx)) { update_failed = true; return; }
    int32_t stored_track = track_number > 0 ? track_number : -1;
    int32_t stored_disc = disc_number > 0 ? disc_number : -1;
    bool tags_changed = fresh || (idx.flag & (FLAG_DELETED | FLAG_TAGS_INCOMPLETE)) ||
                        idx.tag_seek[tag_tracknumber] != stored_track ||
                        idx.tag_seek[tag_discnumber] != stored_disc;
    const int tags[] = {tag_filename, tag_title, tag_artist, tag_album, tag_albumartist, tag_genre};
    const char *values[] = {path, title, artist, album, album_artist, genre};
    for (int i = 0; i < 6; i++) {
        char canonical[TAGCACHE_PATH_MAX];
        char existing[TAGCACHE_PATH_MAX];
        const char *value = values[i] ? values[i] : "";
        if (tags[i] != tag_filename && scan_intern_find(value, canonical, sizeof(canonical))) value = canonical;
        bool unchanged = false;
        if (!fresh) {
            if (!reader_string(tags[i], idx.tag_seek[tags[i]], slot, existing, sizeof(existing))) {
                update_failed = true;
                return;
            }
            unchanged = strcmp(existing, value) == 0;
        }
        if (unchanged) continue;
        tags_changed = true;
        if (update_failed || !append_string(tags[i], slot, value, &idx.tag_seek[tags[i]]) ||
            (tags[i] != tag_filename && !scan_intern_insert(tags[i], idx.tag_seek[tags[i]]))) {
            update_failed = true;
            return;
        }
    }
    idx.tag_seek[tag_mtime] = mtime;
    idx.tag_seek[tag_lastoffset] = size;
    idx.tag_seek[tag_tracknumber] = stored_track;
    idx.tag_seek[tag_discnumber] = stored_disc;
    idx.flag = (idx.flag & ~(FLAG_DELETED | FLAG_TAGS_INCOMPLETE)) | FLAG_SEEN;
    if (!scan_write_index(slot, &idx)) return;
    reader_reset_cache();
    if (fresh) {
        if (slot == ent_count) ent_count++;
        live_count++;
        scan_find_path(path, true, slot);
    }
    if (out_tags_changed && !update_failed) *out_tags_changed = tags_changed;
}

bool tagcache_delete_target_path(const char *path) {
    if (!db_open || !path || !path[0] || !scan_preserve_unseen) return false;
    SCAN_SCOPE;
    int32_t slot = find_path(path);
    if (slot < 0) return !update_failed;
    struct index_entry idx;
    if (!reader_index(slot, &idx)) { update_failed = true; return false; }
    if (idx.flag & FLAG_DELETED) return true;
    if (!start_staging() || update_failed) return false;
    scan_select(true);
    slot = find_path(path);
    if (slot < 0 || !reader_index(slot, &idx)) { update_failed = true; return false; }
    if (idx.flag & FLAG_DELETED) return true;
    idx.flag |= FLAG_DELETED;
    if (!scan_write_index(slot, &idx)) return false;
    if (live_count > 0) live_count--;
    return true;
}

typedef struct {
    int32_t seek, slot;
    unsigned char key[TAGCACHE_SORT_TEXT_PREFIX];
} commit_key_t;
static int commit_source_tag;
static bool commit_read_failed;
static int32_t commit_tag_sources[TAGCACHE_REFS_COUNT];

static bool tag_output_matches(int tag, int source, int64_t count, int keys, int32_t gen) {
    int32_t source_gen = gen;
    if (!resolve_tag_generation(gen, tag, &source_gen)) return false;
    char name[80], path[640];
    tag_file_name(name, sizeof(name), tag, source_gen);
    db_path(path, sizeof(path), name);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    struct tagcache_header old;
    struct stat st;
    bool ok = fstat(fd, &st) == 0 && pread(fd, &old, sizeof(old), 0) == (ssize_t)sizeof(old) &&
              old.magic == TAGCACHE_MAGIC;
    char previous[TAGCACHE_PATH_MAX] = "";
    off_t at = sizeof(old);
    int32_t entries = 0;
    for (int64_t i = 0; ok && i < count; i++) {
        commit_key_t key;
        char value[TAGCACHE_PATH_MAX] = "";
        char old_value[TAGCACHE_PATH_MAX] = "";
        ok = stats_read_at(keys, &key, sizeof(key), (off_t)i * sizeof(key)) == STATS_READ_OK;
        if (ok && tag != tag_comment) ok = reader_string(source, key.seek, key.slot, value, sizeof(value));
        if (!ok || (tag != tag_title && tag != tag_filename && i > 0 && ascii_casecmp(previous, value) == 0)) continue;
        struct tagfile_entry te;
        int32_t mapped = key.slot >= 0 && key.slot < commit_slot_map_count ? commit_slot_map[key.slot] : -1;
        int32_t len = (int32_t)strlen(value) + 1;
        ok = mapped >= 0 && pread(fd, &te, sizeof(te), at) == (ssize_t)sizeof(te) &&
             te.tag_length == len && te.idx_id == mapped &&
             pread(fd, old_value, (size_t)len, at + (off_t)sizeof(te)) == (ssize_t)len &&
             memcmp(old_value, value, (size_t)len) == 0;
        if (ok) { at += sizeof(te) + len; entries++; snprintf(previous, sizeof(previous), "%s", value); }
    }
    ok = ok && old.entry_count == entries && old.datasize == at - (off_t)sizeof(old) &&
         st.st_size == at;
    close(fd);
    return ok;
}

static int compare_commit_key(const void *a, const void *b, void *ctx) {
    (void) ctx;
    const commit_key_t *ka = a, *kb = b;
    bool full_compare;
    int c = tagcache_sort_prefix_compare(ka->key, kb->key, sizeof(ka->key), &full_compare);
    if (!c && full_compare) {
        char sa[TAGCACHE_PATH_MAX], sb[TAGCACHE_PATH_MAX];
        if (!reader_string(commit_source_tag, ka->seek, ka->slot, sa, sizeof(sa)) ||
            !reader_string(commit_source_tag, kb->seek, kb->slot, sb, sizeof(sb))) {
            commit_read_failed = true;
            return (ka->slot > kb->slot) - (ka->slot < kb->slot);
        }
        c = ascii_casecmp(sa, sb);
    }
    return c ? c : (ka->slot > kb->slot) - (ka->slot < kb->slot);
}

static bool write_commit_tag(int tag, int32_t gen, int master) {
    int source = tag;
    if (tag == tag_composer || tag == tag_virt_canonicalartist) source = tag_artist;
    if (tag == tag_grouping) source = tag_title;
    bool empty = tag == tag_comment;
    bool unique = tag != tag_title && tag != tag_filename;
    int keys = reader_create_temp(), scratch = reader_create_temp();
    int fd = -1;
    bool ok = keys >= 0 && scratch >= 0;
    size_t key_batch_capacity = (64u * 1024u) / sizeof(commit_key_t);
    unsigned char *key_batch = NULL;
    if (ok && key_batch_capacity == 0) ok = false;
    size_t key_batch_used = 0;
    int64_t count = 0;
    commit_source_tag = source;
    commit_read_failed = false;
    for (int32_t slot = 0; ok && slot < ent_count; slot++) {
        struct index_entry idx;
        if (!reader_index(slot, &idx)) { ok = false; break; }
        if (idx.flag & FLAG_DELETED) continue;
        commit_key_t key = {0};
        key.seek = empty ? 0 : idx.tag_seek[source];
        key.slot = slot;
        if (!empty && tag != tag_filename) {
            char value[TAGCACHE_PATH_MAX];
            if (!reader_string(source, key.seek, slot, value, sizeof(value)) ||
                !tagcache_sort_prefix(value, key.key, sizeof(key.key))) {
                commit_read_failed = true;
                ok = false;
                break;
            }
        }
        if (key_batch == NULL) {
            key_batch = (unsigned char *)malloc(64u * 1024u);
            if (key_batch == NULL) { ok = false; break; }
        }
        memcpy(key_batch + key_batch_used * sizeof(key), &key, sizeof(key));
        key_batch_used++;
        count++;
        if (key_batch_used == key_batch_capacity) {
            ok = tc_sort_io_write(keys, key_batch, key_batch_used * sizeof(key),
                                  (off_t)(count - (int64_t)key_batch_used) * (off_t)sizeof(key));
            if (ok) key_batch_used = 0;
        }
    }
    if (ok && key_batch_used != 0)
        ok = tc_sort_io_write(keys, key_batch, key_batch_used * sizeof(commit_key_t),
                              (off_t)(count - (int64_t)key_batch_used) * (off_t)sizeof(commit_key_t));
    free(key_batch);
    if (ok && !empty && tag != tag_filename)
        ok = tc_sort_fd(keys, sizeof(commit_key_t), count, compare_commit_key, NULL, scratch) && !commit_read_failed;
    bool reuse = ok && tag_output_matches(tag, source, count, keys, disk_gen);
    char name[80], path[640], tmp_path[680];
    tag_file_name(name, sizeof(name), tag, gen);
    db_path(path, sizeof(path), name);
    snprintf(tmp_path, sizeof(tmp_path), "%s.new", path);
    if (ok && !reuse) ok = (fd = open(tmp_path, O_CREAT | O_TRUNC | O_WRONLY, 0644)) >= 0;
    struct tagcache_header hdr = {TAGCACHE_MAGIC, 0, 0};
    if (ok && !reuse) ok = write_fully(fd, &hdr, sizeof(hdr));
    char previous[TAGCACHE_PATH_MAX] = "";
    int32_t pos = sizeof(hdr), seek = 0;
    for (int64_t i = 0; ok && i < count; i++) {
        commit_key_t key;
        char value[TAGCACHE_PATH_MAX] = "";
        ok = stats_read_at(keys, &key, sizeof(key), (off_t) i * sizeof(key)) == STATS_READ_OK;
        if (ok && !empty) ok = reader_string(source, key.seek, key.slot, value, sizeof(value));
        if (!ok) break;
        if (!unique || i == 0 || ascii_casecmp(previous, value) != 0) {
            int32_t len = (int32_t) strlen(value) + 1;
            if (!commit_slot_map || key.slot < 0 || key.slot >= commit_slot_map_count || commit_slot_map[key.slot] < 0) {
                ok = false;
                break;
            }
            struct tagfile_entry te = {len, commit_slot_map[key.slot]};
            seek = pos;
            if (!reuse) ok = write_fully(fd, &te, sizeof(te)) && write_fully(fd, value, (size_t) len);
            pos += sizeof(te) + len;
            hdr.entry_count++;
            snprintf(previous, sizeof(previous), "%s", value);
        }
        if (ok) ok = write_at(master, &seek, sizeof(seek), sizeof(struct master_header) +
                               (off_t) commit_slot_map[key.slot] * sizeof(struct index_entry) + (off_t) tag * sizeof(int32_t));
    }
    hdr.datasize = pos - sizeof(hdr);
    if (ok && !reuse) ok = write_at(fd, &hdr, sizeof(hdr), 0);
    if (fd >= 0 && !close_synced(fd)) ok = false;
    fd = -1;
    if (ok && !reuse) {
        ok = rename(tmp_path, path) == 0;
        if (ok) {
            int index = persisted_tag_index(tag);
            if (index >= 0) commit_tag_sources[index] = gen;
        }
    } else if (ok && reuse) {
        int32_t source_gen = disk_gen;
        ok = resolve_tag_generation(disk_gen, tag, &source_gen);
        int index = persisted_tag_index(tag);
        if (ok && index >= 0) commit_tag_sources[index] = source_gen;
    }
    if (keys >= 0) close(keys);
    if (scratch >= 0) close(scratch);
    if (!ok) { unlink(path); unlink(tmp_path); }
    return ok;
}

static bool write_all(void (*unlock)(void), void (*lock)(void)) {
    int32_t previous = 0, highest = 0, recovered = disk_gen;
    int pointer_result = read_gen_pointer(&previous);
    if (pointer_result <= 0) previous = 0;
    if (!scan_generation_max(&highest)) return false;
    if (highest < previous) highest = previous;
    if (highest < disk_gen) highest = disk_gen;
    if (highest == INT32_MAX) return false;
    int32_t gen = highest + 1;
    for (size_t i = 0; i < TAGCACHE_REFS_COUNT; i++) commit_tag_sources[i] = -1;
    free(commit_slot_map);
    commit_slot_map = NULL;
    commit_slot_map_count = ent_count;
    if (ent_count > 0) {
        commit_slot_map = malloc((size_t)ent_count * sizeof(*commit_slot_map));
        if (!commit_slot_map) return false;
        int32_t dense = 0;
        for (int32_t old = 0; old < ent_count; old++) {
            struct index_entry idx;
            if (!reader_index(old, &idx)) { free(commit_slot_map); commit_slot_map = NULL; return false; }
            if (idx.flag & FLAG_DELETED) commit_slot_map[old] = -1;
            else if (scan_preserve_unseen) { commit_slot_map[old] = old; dense++; }
            else if (dense >= TAGCACHE_MAX_ENTRIES) { free(commit_slot_map); commit_slot_map = NULL; return false; }
            else commit_slot_map[old] = dense++;
        }
        live_count = dense;
    }
    char name[80], path[640];
    master_file_name(name, sizeof(name), gen);
    db_path(path, sizeof(path), name);
    int master = open(path, O_CREAT | O_TRUNC | O_RDWR, 0644);
    if (master < 0) { free(commit_slot_map); commit_slot_map = NULL; return false; }
    int32_t master_entries = scan_preserve_unseen ? ent_count : live_count;
    struct master_header mh = {{TAGCACHE_INDEXED_MAGIC, master_entries * (int32_t) sizeof(struct index_entry), master_entries},
                                master_serial, gen, 0};
    if (unlock) unlock();
    bool ok = write_fully(master, &mh, sizeof(mh));
    for (int32_t slot = 0; ok && slot < ent_count; slot++) {
        struct index_entry idx;
        if (!scan_preserve_unseen && commit_slot_map && commit_slot_map[slot] < 0) continue;
        ok = reader_index(slot, &idx);
        if (!ok) break;
        idx.flag &= ~FLAG_RAM_ONLY;
        if (idx.flag & FLAG_DELETED) { memset(&idx, 0, sizeof(idx)); idx.flag = FLAG_DELETED; }
        int32_t output_slot = scan_preserve_unseen ? slot : commit_slot_map[slot];
        ok = write_at(master, &idx, sizeof(idx), sizeof(mh) + (off_t)output_slot * sizeof(idx));
    }
    for (size_t i = 0; ok && i < sizeof(persist_tag_ids) / sizeof(persist_tag_ids[0]); i++)
        ok = write_commit_tag(persist_tag_ids[i], gen, master);
    if (ok) {
        for (size_t i = 0; i < TAGCACHE_REFS_COUNT; i++)
            if (commit_tag_sources[i] < 0) commit_tag_sources[i] = gen;
        ok = write_generation_refs(gen, commit_tag_sources);
    }
    if (close(master) != 0) ok = false;
    if (ok) {
        selected_reader = &build_reader;
        reader_context_init(&build_reader);
        ok = reader_open(gen) && reader_build_indexes() && reader_write_indexes(gen);
        reader_close_indexes();
        reader_close();
        selected_reader = &scan_reader;
    }
    if (lock) lock();
    tagcache_flush_numeric();
    if (update_failed) ok = false;
    if (ok) {
        int fd = open(path, O_RDWR);
        ok = fd >= 0 && replay_scan_numeric(fd);
        if (fd >= 0 && !close_synced(fd)) ok = false;
    }
    /* The index build intentionally runs with the query lock released.  The
     * pinned descriptors keep writes on the original card, but a replacement
     * must still reject publication so it cannot become the active database
     * for the new card. */
    if (ok && !db_identity_current()) ok = false;
    if (ok && (staged_migration_state != 0 || committed_migration_state != 0))
        ok = write_migration_state(gen, staged_migration_state);
    if (ok) ok = library_fsync_dir(db_dir);
    if (ok && !rebuild_preserve_generations) ok = write_gen_pointer(gen);
    if (!ok) {
        unlink_generation(gen);
        free(commit_slot_map);
        commit_slot_map = NULL;
        return false;
    }
    disk_gen = gen;
    master_commitid = gen;
    disk_ready = true;
    if (!rebuild_preserve_generations) {
        if (pointer_result > 0) unlink_other_generations(gen, previous);
        else if (recovered > 0 && pointer_result == 0) unlink_other_generations(gen, recovered);
    }
    rebuild_preserve_generations = false;
    free(commit_slot_map);
    commit_slot_map = NULL;
    commit_slot_map_count = 0;
    return true;
}
static bool load_generation(int32_t gen) {
    reader_legacy_active = false;
    reader_close_indexes();
    bool ok = reader_open(gen);
    int indexed = ok ? reader_open_indexes(gen) : -1;
    if (ok && indexed == 0 && reader_requires_indexes) ok = false;
    if (ok && indexed == 0) ok = reader_legacy_init();
    if (!ok || indexed < 0) {
        reader_close_indexes();
        reader_close();
        ent_count = live_count = 0;
        return false;
    }
    int migration_state_result = read_migration_state(gen, &committed_migration_state);
    if (migration_state_result < 0) {
        reader_close_indexes();
        reader_close();
        ent_count = live_count = 0;
        return false;
    }
    if (migration_state_result == 0) committed_migration_state = 0;
    staged_migration_state = committed_migration_state;
    disk_ready = true;
    return true;
}

static bool load_all(void) {
    last_load_outcome = TAGCACHE_LOAD_FAILED;
    int32_t pointed = 0;
    int pointer_result = read_gen_pointer(&pointed);
    bool have_pointer = pointer_result > 0;
    bool saved = pointer_result != 0, pointer_file = have_pointer;
    int32_t *generations = NULL;
    size_t count = 0;
    if (!collect_load_generations(&generations, &count, pointed, &saved, &pointer_file)) return false;
    if (!saved) {
        free(generations);
        reader_compact_order = !choose_intern_strings(0);
        last_load_outcome = TAGCACHE_LOAD_SUCCESS_FRESH;
        return true;
    }
    size_t attempts = count + (have_pointer ? 1 : 0);
    for (size_t i = 0; i < attempts; i++) {
        int32_t gen = have_pointer && !i ? pointed : generations[i - (have_pointer ? 1 : 0)];
        if (load_generation(gen)) {
            bool recovered = pointer_result < 0 || (have_pointer ? i > 0 : gen > 0 || pointer_file || i > 0);
            last_load_outcome = recovered ? TAGCACHE_LOAD_SUCCESS_RECOVERED : TAGCACHE_LOAD_SUCCESS_NORMAL;
            free(generations);
            return true;
        }
        disk_ready = false;
        disk_gen = 0;
    }
    free(generations);
    return false;
}

static bool reload_from_disk(void) {
    if (db_dir_fd < 0 || !db_logical_dir[0]) return false;
    bool require_saved = disk_ready;
    int pinned_fd = dup(db_dir_fd);
    char logical[sizeof(db_logical_dir)];
    if (pinned_fd < 0) return false;
    snprintf(logical, sizeof(logical), "%s", db_logical_dir);
    tagcache_close();
    db_dir_fd = pinned_fd;
    snprintf(db_logical_dir, sizeof(db_logical_dir), "%s", logical);
    snprintf(db_dir, sizeof(db_dir), "/proc/self/fd/%d", db_dir_fd);
    db_dir_identity_valid = fstat(db_dir_fd, &db_dir_identity) == 0;
    db_open = true;
    bool ok = load_all();
    if (!ok) {
        tagcache_close();
        return false;
    }
    if (ok && require_saved && last_load_outcome == TAGCACHE_LOAD_SUCCESS_FRESH) {
        tagcache_close();
        last_load_outcome = TAGCACHE_LOAD_FAILED;
        return false;
    }
    if (ok) numeric_worker_start();
    return ok;
}

bool tagcache_open(const char *dir) {
    tagcache_close();
    last_load_outcome = TAGCACHE_LOAD_FAILED;
    if (!db_pin_directory(dir)) return false;
    db_open = true;
    if (!load_all()) { tagcache_close(); return false; }
    numeric_worker_start();
    return true;
}

bool tagcache_open_for_rebuild(const char *dir) {
    if (tagcache_open(dir)) return true;
    if (!dir || !dir[0] || strlen(dir) >= sizeof(db_logical_dir)) return false;
    struct stat st;
    if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode) || access(dir, R_OK | W_OK) != 0) return false;
    tagcache_close();
    if (!db_pin_directory(dir)) return false;
    db_open = true;
    rebuild_preserve_generations = true;
    numeric_worker_start();
    return true;
}

void tagcache_close(void) {
    if (!committed_reader.fds_ready) reader_context_init(&committed_reader);
    numeric_worker_stop();
    reader_legacy_active = false;
    if (scan_view_ready) {
        scan_select(true);
        reader_close();
        scan_select(false);
        scan_view_ready = false;
    }
    selected_reader = &committed_reader;
    if (scan_numeric_fd >= 0) close(scan_numeric_fd);
    scan_numeric_fd = -1;
    scan_intern_close();
    reader_close_indexes();
    reader_close();
    reader_reset_cache();
    memset(scan_tag_private, 0, sizeof(scan_tag_private));
    scan_hash_release();
    scan_seen_release();
    ent_count = live_count = 0;
    disk_gen = master_commitid = 0;
    master_serial = 1;
    db_open = disk_ready = rebuild_preserve_generations = updating = scan_initializing = update_failed = false;
    scan_preserve_unseen = false;
    committed_migration_state = staged_migration_state = 0;
    if (db_dir_fd >= 0) close(db_dir_fd);
    db_dir_fd = -1;
    db_dir_identity_valid = false;
    db_dir[0] = '\0';
    db_logical_dir[0] = '\0';
}

tagcache_load_outcome_t tagcache_get_load_outcome(void) { return last_load_outcome; }

bool tagcache_end_update_with_lock(void (*unlock)(void), void (*lock)(void)) {
    if (!db_open) return false;
    if (!db_identity_current()) {
        update_failed = true;
        tagcache_abort_update();
        return false;
    }
    if (!tagcache_flush_numeric()) { update_failed = true; return false; }
    if (scan_lazy_active && scan_preserve_unseen) {
        if (!updating && !scan_force_publication) {
            scan_seen_release();
            scan_preserve_unseen = false;
            return true;
        }
        if (!updating && !start_staging_with_lock(unlock, lock)) {
            scan_seen_release();
            return false;
        }
    } else if (scan_lazy_active) {
        bool changed = scan_force_publication;
        for (int32_t slot = 0; !changed && slot < ent_count; slot++) {
            if (scan_seen_test(slot)) continue;
            struct index_entry idx;
            char path[TAGCACHE_PATH_MAX];
            if (!reader_index(slot, &idx)) {
                update_failed = true;
                break;
            }
            if (idx.flag & FLAG_DELETED) continue;
            if (!reader_string(tag_filename, idx.tag_seek[tag_filename], slot, path, sizeof(path))) {
                update_failed = true;
                break;
            }
            struct stat st;
            if (unlock) unlock();
            if (path[0] && stat(path, &st) != 0 && errno == ENOENT) changed = true;
            if (lock) lock();
        }
        if (update_failed) {
            scan_seen_release();
            return false;
        }
        if (!db_identity_current()) {
            scan_seen_release();
            return false;
        }
        if (!changed) {
            scan_seen_release();
            updating = false;
            return true;
        }
        if (!start_staging_with_lock(unlock, lock)) {
            scan_seen_release();
            return false;
        }
    }
    bool preserve = rebuild_preserve_generations;
    bool ok = start_staging_with_lock(unlock, lock);
    SCAN_SCOPE;
    live_count = 0;
    for (int32_t slot = 0; ok && slot < ent_count; slot++) {
        struct index_entry idx;
        ok = reader_index(slot, &idx);
        if (!ok) break;
        if (!scan_preserve_unseen && !(idx.flag & (FLAG_DELETED | FLAG_SEEN))) {
            char path[TAGCACHE_PATH_MAX];
            ok = reader_string(tag_filename, idx.tag_seek[tag_filename], slot, path, sizeof(path));
            if (!ok) break;
            struct stat st;
            if (unlock) unlock();
            bool missing = path[0] && stat(path, &st) != 0 && errno == ENOENT;
            if (lock) lock();
            if (!reader_index(slot, &idx)) { ok = false; break; }
            if (missing) idx.flag |= FLAG_DELETED;
        }
        idx.flag &= ~FLAG_SEEN;
        if (!(idx.flag & FLAG_DELETED)) live_count++;
        ok = scan_write_index(slot, &idx);
        if (unlock && (slot & 63) == 63) { unlock(); sched_yield(); lock(); }
    }
    if (preserve && !live_count) ok = false;
    if (ok && !update_failed && db_identity_current()) ok = write_all(unlock, lock); else ok = false;
    scan_select(false);
    bool loaded = reload_from_disk();
    if (ok && loaded && preserve) last_load_outcome = TAGCACHE_LOAD_SUCCESS_RECOVERED;
    return ok && loaded;
}

bool tagcache_end_update(void) { return tagcache_end_update_with_lock(NULL, NULL); }

void tagcache_abort_update(void) {
    if (!db_open) return;
    bool numeric_ok = tagcache_flush_numeric();
    if (numeric_ok && scan_lazy_active && !updating && !scan_view_ready && !update_failed &&
        staged_migration_state == committed_migration_state) {
        scan_seen_release();
        scan_preserve_unseen = false;
        return;
    }
    reload_from_disk();
}

int32_t tagcache_live_count(void) { return db_open ? live_count : 0; }
int32_t tagcache_slot_count(void) { return db_open ? ent_count : 0; }

bool tagcache_song_fields_by_id(int32_t id, unsigned fields, tagcache_song_t *out) {
    if (!db_open || id < 1 || id > ent_count || !out) return false;
    return reader_song_fields(id - 1, fields, out);
}

bool tagcache_song_fields_at_title_rank(int32_t rank, unsigned fields, tagcache_song_t *out) {
    if (!db_open || rank < 0 || rank >= live_count || !out) return false;
    if (reader_legacy_active) {
        tagcache_song_t song;
        if (!legacy_order_song(false, rank, &song)) return false;
        return reader_song_fields(song.id - 1, fields, out);
    }
    int32_t slot;
    return reader_read_at(reader_title_fd, &slot, sizeof(slot), (off_t)rank * sizeof(slot)) &&
           reader_song_fields(slot, fields, out);
}

bool tagcache_song_fields_at_recency_rank(int32_t rank, unsigned fields, tagcache_song_t *out) {
    if (!db_open || rank < 0 || rank >= live_count || !out) return false;
    if (reader_legacy_active) {
        tagcache_song_t song;
        if (!legacy_order_song(true, rank, &song)) return false;
        return reader_song_fields(song.id - 1, fields, out);
    }
    int32_t slot;
    return reader_read_at(reader_recency_fd, &slot, sizeof(slot), (off_t)rank * sizeof(slot)) &&
           reader_song_fields(slot, fields, out);
}

bool tagcache_song_by_id(int32_t id, tagcache_song_t *out) {
    if (!db_open || id < 1 || id > ent_count) return false;
    struct index_entry idx;
    if (!reader_index(id - 1, &idx) || (idx.flag & FLAG_DELETED)) return false;
    numeric_overlay(id - 1, &idx);
    if (!out) return true;
    if (!reader_song(id - 1, out)) return false;
    out->playcount = idx.tag_seek[tag_playcount];
    out->last_played = idx.tag_seek[tag_lastplayed];
    out->rating = idx.tag_seek[tag_rating];
    return true;
}

bool tagcache_song_by_path(const char *path, tagcache_song_t *out) {
    int32_t slot = find_path(path);
    return slot >= 0 && tagcache_song_by_id(slot + 1, out);
}

bool tagcache_song_at_slot(int32_t slot, tagcache_song_t *out) {
    return slot >= 0 && slot < ent_count && tagcache_song_by_id(slot + 1, out);
}

bool tagcache_song_at_title_rank(int32_t rank, tagcache_song_t *out) {
    if (!db_open) return false;
    return reader_order_song(reader_title_fd, false, rank, out);
}

bool tagcache_song_at_recency_rank(int32_t rank, tagcache_song_t *out) {
    if (!db_open) return false;
    return reader_order_song(reader_recency_fd, true, rank, out);
}

int tagcache_group_count(int kind) { return db_open && kind >= 0 && kind < 3 ? (reader_legacy_active ? legacy_group_count(kind) : reader_group_n[kind]) : 0; }
bool tagcache_group_at(int kind, int index, tagcache_group_t *out) { return db_open && reader_group_at(kind, index, out); }
int tagcache_group_index(int kind, const char *name, const char *aa) {
    return db_open ? reader_group_index(kind, name, aa) : -1;
}

int tagcache_artist_song_ids(const char *artist, int offset, int32_t *ids, int max) {
    return reader_group_ids(TAGCACHE_GROUP_ARTIST, tagcache_group_index(TAGCACHE_GROUP_ARTIST, artist, ""), offset, ids, max);
}
int tagcache_album_artist_song_ids(const char *artist, int offset, int32_t *ids, int max) {
    return reader_group_ids(TAGCACHE_GROUP_ALBUM_ARTIST, tagcache_group_index(TAGCACHE_GROUP_ALBUM_ARTIST, artist, ""), offset, ids, max);
}
int tagcache_album_song_ids(const char *album, const char *artist, int offset, int32_t *ids, int max) {
    return reader_group_ids(TAGCACHE_GROUP_ALBUM, tagcache_group_index(TAGCACHE_GROUP_ALBUM, album, artist), offset, ids, max);
}

static void update_stats(struct index_entry *idx, int mode, int32_t rating, int32_t count, int32_t last) {
    if (mode == 0 || mode == 2) idx->tag_seek[tag_rating] = rating;
    if (mode == 1) {
        if (idx->tag_seek[tag_playcount] < INT32_MAX) idx->tag_seek[tag_playcount]++;
        idx->tag_seek[tag_lastplayed] = last;
    }
    if (mode == 2) { idx->tag_seek[tag_playcount] = count; idx->tag_seek[tag_lastplayed] = last; }
}

static void set_song_stats(const char *path, int mode, int32_t rating, int32_t count, int32_t last) {
    if (!db_open || !path) return;
    if (mode == 2 && !start_staging()) return;
    if (mode != 2) {
        int32_t slot = reader_find_path(path);
        struct index_entry idx;
        if (slot >= 0 && reader_index(slot, &idx) && !(idx.flag & FLAG_DELETED)) {
            update_stats(&idx, mode, rating, count, last);
            queue_numeric_update(slot, &idx);
            if (scan_initializing) {
                int32_t change[4] = {slot, idx.tag_seek[tag_rating], idx.tag_seek[tag_playcount], idx.tag_seek[tag_lastplayed]};
                if (!write_fully(scan_numeric_fd, change, sizeof(change))) update_failed = true;
            }
        }
    }
    if (scan_view_ready) {
        SCAN_SCOPE;
        int32_t slot = scan_find_path(path, false, -1);
        struct index_entry idx;
        if (slot >= 0 && reader_index(slot, &idx) && !(idx.flag & FLAG_DELETED)) {
            update_stats(&idx, mode, rating, count, last);
            if (scan_write_index(slot, &idx)) {
                int32_t change[4] = {slot, idx.tag_seek[tag_rating], idx.tag_seek[tag_playcount], idx.tag_seek[tag_lastplayed]};
                if (!write_fully(scan_numeric_fd, change, sizeof(change))) update_failed = true;
            }
        }
    }
    reader_reset_cache();
}
void tagcache_set_rating(const char *path, int32_t rating) { set_song_stats(path, 0, rating, 0, 0); }
void tagcache_add_play(const char *path, int32_t now) { set_song_stats(path, 1, 0, 0, now); }
void tagcache_overlay_stats(const char *path, int32_t rating, int32_t count, int32_t last) {
    set_song_stats(path, 2, rating, count, last);
}
int32_t tagcache_title_rank_of_path(const char *path) { return reader_rank_of(find_path(path), false); }
int32_t tagcache_recency_rank_of_path(const char *path) { return reader_rank_of(find_path(path), true); }
const char *tagcache_ascii_casestr(const char *hay, const char *needle) {
    return ascii_casestr(hay ? hay : "", needle ? needle : "");
}

struct tagcache_snapshot {
    atomic_uint references;
    int dir_fd;
    int master_fd;
    int filename_fd;
    int order_fd;
    int32_t count;
    bool legacy;
    int32_t *legacy_slots;
};

static bool snapshot_pread_full(int fd, void *buf, size_t size, off_t offset) {
    unsigned char *p = buf;
    size_t done = 0;
    while (done < size) {
        ssize_t n = pread(fd, p + done, size - done, offset + (off_t)done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        done += (size_t)n;
    }
    return true;
}

tagcache_snapshot_t *tagcache_snapshot_open(bool recency) {
    if (!db_open || !committed_reader.fds_ready || committed_reader.master_fd < 0 ||
        committed_reader.tag_fd[tag_filename] < 0 || committed_reader.live < 0)
        return NULL;
    tagcache_snapshot_t *snapshot = calloc(1, sizeof(*snapshot));
    if (!snapshot) return NULL;
    atomic_init(&snapshot->references, 1);
    snapshot->dir_fd = snapshot->master_fd = snapshot->filename_fd = snapshot->order_fd = -1;
    snapshot->dir_fd = db_dir_fd >= 0 ? dup(db_dir_fd) : -1;
    snapshot->count = committed_reader.live;
    snapshot->legacy = committed_reader.legacy_active;
    snapshot->master_fd = dup(committed_reader.master_fd);
    snapshot->filename_fd = dup(committed_reader.tag_fd[tag_filename]);
    if (snapshot->dir_fd < 0 || snapshot->master_fd < 0 || snapshot->filename_fd < 0) {
        tagcache_snapshot_close(snapshot);
        return NULL;
    }
    if (snapshot->legacy) {
        if (snapshot->count > 0) {
            size_t bytes;
            if (!checked_mul_size((size_t)snapshot->count, sizeof(*snapshot->legacy_slots), &bytes)) {
                tagcache_snapshot_close(snapshot);
                return NULL;
            }
            snapshot->legacy_slots = malloc(bytes);
            if (!snapshot->legacy_slots) {
                tagcache_snapshot_close(snapshot);
                return NULL;
            }
            reader_context_t *saved = selected_reader;
            selected_reader = &committed_reader;
            for (int32_t rank = 0; rank < snapshot->count; rank++) {
                tagcache_song_t song;
                if (!legacy_order_song(recency, rank, &song)) {
                    selected_reader = saved;
                    tagcache_snapshot_close(snapshot);
                    return NULL;
                }
                snapshot->legacy_slots[rank] = song.id - 1;
            }
            selected_reader = saved;
        }
    } else {
        int source = recency ? committed_reader.recency_fd : committed_reader.title_fd;
        snapshot->order_fd = source >= 0 ? dup(source) : -1;
        if (snapshot->order_fd < 0) {
            tagcache_snapshot_close(snapshot);
            return NULL;
        }
    }
    return snapshot;
}

int tagcache_snapshot_count(const tagcache_snapshot_t *snapshot) {
    return snapshot ? snapshot->count : 0;
}

bool tagcache_snapshot_path_at(const tagcache_snapshot_t *snapshot, int rank, char *out, size_t out_size) {
    if (!snapshot || !out || out_size == 0 || rank < 0 || rank >= snapshot->count) return false;
    int32_t slot = -1;
    if (snapshot->legacy) {
        slot = snapshot->legacy_slots[rank];
    } else if (!snapshot_pread_full(snapshot->order_fd, &slot, sizeof(slot), (off_t)rank * sizeof(slot))) {
        return false;
    }
    if (slot < 0 || slot >= TAGCACHE_MAX_ENTRIES) return false;
    struct index_entry idx;
    if (!snapshot_pread_full(snapshot->master_fd, &idx, sizeof(idx),
                             (off_t)sizeof(struct master_header) + (off_t)slot * sizeof(idx))) return false;
    int32_t seek = idx.tag_seek[tag_filename];
    struct tagcache_header hdr;
    struct stat filename_st;
    if (seek < (int32_t)sizeof(hdr) || fstat(snapshot->filename_fd, &filename_st) != 0 ||
        !snapshot_pread_full(snapshot->filename_fd, &hdr, sizeof(hdr), 0) ||
        !validate_tag_header(&hdr, (size_t)filename_st.st_size)) return false;
    struct tagfile_entry entry;
    if (!snapshot_pread_full(snapshot->filename_fd, &entry, sizeof(entry), seek) || entry.idx_id != slot ||
        entry.tag_length <= 0 || (size_t)entry.tag_length > out_size ||
        !snapshot_pread_full(snapshot->filename_fd, out, (size_t)entry.tag_length,
                             (off_t)seek + sizeof(entry))) return false;
    return out[entry.tag_length - 1] == '\0' && !memchr(out, '\0', (size_t)entry.tag_length - 1);
}

tagcache_snapshot_t *tagcache_snapshot_retain(tagcache_snapshot_t *snapshot) {
    if (snapshot) atomic_fetch_add_explicit(&snapshot->references, 1, memory_order_relaxed);
    return snapshot;
}

void tagcache_snapshot_close(tagcache_snapshot_t *snapshot) {
    if (!snapshot) return;
    if (atomic_fetch_sub_explicit(&snapshot->references, 1, memory_order_acq_rel) != 1) return;
    if (snapshot->dir_fd >= 0) close(snapshot->dir_fd);
    if (snapshot->master_fd >= 0) close(snapshot->master_fd);
    if (snapshot->filename_fd >= 0) close(snapshot->filename_fd);
    if (snapshot->order_fd >= 0) close(snapshot->order_fd);
    free(snapshot->legacy_slots);
    free(snapshot);
}

int tagcache_snapshot_dup_directory_fd(const tagcache_snapshot_t *snapshot) {
    return snapshot && snapshot->dir_fd >= 0 ? dup(snapshot->dir_fd) : -1;
}
