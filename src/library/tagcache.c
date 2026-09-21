/* Seek-based POSIX tagcache using the Rockbox master and string-file format.
 * Copyright (C) 2005 Miika Pekkarinen (original on-disk format)
 * Copyright (C) Open HiBy Player contributors (POSIX implementation)
 * Generation commits preserve the previous library until pointer publication.
 */
#include "tagcache.h"
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
#define TAGCACHE_MAX_ENTRIES 524288
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


static char db_dir[512];
static bool db_open, disk_ready, rebuild_preserve_generations;
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
    struct { bool valid; int32_t slot; tagcache_song_t song; } sort_cache[READER_SORT_CACHE_ROWS];
} reader_context_t;
static reader_context_t committed_reader, scan_reader, build_reader;
static _Thread_local reader_context_t *selected_reader;
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


static const int persist_tag_ids[] = { tag_artist,       tag_album,    tag_genre,     tag_albumartist, tag_composer,
                                       tag_comment,      tag_grouping, tag_virt_canonicalartist, tag_title, tag_filename };

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
}

static void unlink_other_generations(int32_t keep, int32_t previous) {
    DIR * d = opendir(db_dir);
    if (!d) return;
    struct dirent * de;
    while ((de = readdir(d)) != NULL) {
        const char * name = de->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        if (strcmp(name, "tagcache.gen") == 0 || strcmp(name, "tagcache.gen.tmp") == 0) continue;
        const char * gpos = strstr(name, ".tcd.g");
        char path[800];
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
    struct tagcache_stats_row * rows;
    size_t count;
    size_t cap;
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

static bool stats_snapshot_add(struct tagcache_stats_snapshot * snapshot, const char * path,
                               int32_t rating, int32_t playcount, int32_t last_played) {
    if (snapshot->count == snapshot->cap) {
        size_t next = snapshot->cap ? snapshot->cap * 2 : 32;
        if (next < snapshot->cap || next > SIZE_MAX / sizeof(*snapshot->rows)) return false;
        struct tagcache_stats_row * grown = realloc(snapshot->rows, next * sizeof(*snapshot->rows));
        if (!grown) return false;
        snapshot->rows = grown;
        snapshot->cap = next;
    }
    char * copy = strdup(path);
    if (!copy) return false;
    snapshot->rows[snapshot->count++] = (struct tagcache_stats_row) { copy, rating, playcount, last_played };
    return true;
}

enum stats_extract_result { STATS_CANDIDATE_BAD, STATS_EXTRACTED, STATS_EXTRACT_FATAL };

static void stats_snapshot_clear(struct tagcache_stats_snapshot * snapshot) {
    for (size_t i = 0; i < snapshot->count; i++) free(snapshot->rows[i].path);
    snapshot->count = 0;
}

static enum stats_extract_result stats_extract_generation(const char * dir, int32_t gen,
                                                          struct tagcache_stats_snapshot * snapshot) {
    char name[80], master_path[640], filename_path[640];
    if (gen > 0) {
        snprintf(name, sizeof(name), "database_idx.tcd.g%d", gen);
        snprintf(filename_path, sizeof(filename_path), "%s/database_%d.tcd.g%d", dir, tag_filename, gen);
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
        if (!stats_snapshot_add(snapshot, path, idx.tag_seek[tag_rating], idx.tag_seek[tag_playcount],
                                idx.tag_seek[tag_lastplayed])) {
            close(filename_fd);
            close(master_fd);
            stats_snapshot_clear(snapshot);
            return STATS_EXTRACT_FATAL;
        }
    }
    bool close_failed = close(filename_fd) != 0;
    close_failed = close(master_fd) != 0 || close_failed;
    if (close_failed) {
        stats_snapshot_clear(snapshot);
        return STATS_EXTRACT_FATAL;
    }
    return STATS_EXTRACTED;
}

bool tagcache_extract_stats(const char * dir, tagcache_stats_snapshot_t ** out) {
    if (!out || !dir || !dir[0]) return false;
    *out = NULL;
    int32_t pointed = 0;
    enum stats_pointer_result pointer_result = stats_read_pointer(dir, &pointed);
    if (pointer_result == STATS_POINTER_ERROR) return false;
    bool have_pointer = pointer_result == STATS_POINTER_OK;
    int32_t * generations = NULL;
    size_t generation_count = 0;
    if (!stats_collect_generations(dir, have_pointer ? pointed : -1, &generations, &generation_count)) return false;
    struct tagcache_stats_snapshot * snapshot = calloc(1, sizeof(*snapshot));
    if (!snapshot) {
        free(generations);
        return false;
    }
    bool extracted = false;
    enum stats_extract_result result = STATS_CANDIDATE_BAD;
    if (have_pointer) {
        result = stats_extract_generation(dir, pointed, snapshot);
        if (result == STATS_EXTRACT_FATAL) {
            free(generations);
            tagcache_free_stats(snapshot);
            return false;
        }
        extracted = result == STATS_EXTRACTED;
    }
    for (size_t n = 0; !extracted && n < generation_count; n++) {
        stats_snapshot_clear(snapshot);
        result = stats_extract_generation(dir, generations[n], snapshot);
        if (result == STATS_EXTRACT_FATAL) {
            free(generations);
            tagcache_free_stats(snapshot);
            return false;
        }
        extracted = result == STATS_EXTRACTED;
    }
    free(generations);
    if (!extracted) {
        tagcache_free_stats(snapshot);
        return false;
    }
    *out = snapshot;
    return true;
}

bool tagcache_replay_stats(const tagcache_stats_snapshot_t * snapshot) {
    if (!snapshot) return false;
    bool ok = true;
    for (size_t i = 0; i < snapshot->count; i++) {
        const struct tagcache_stats_row * row = &snapshot->rows[i];
        tagcache_song_t current;
        if (tagcache_song_by_path(row->path, &current)) {
            int32_t playcount = current.playcount > row->playcount ? current.playcount : row->playcount;
            int32_t last_played = current.last_played > row->last_played ? current.last_played : row->last_played;
            tagcache_overlay_stats(row->path, row->rating, playcount, last_played);
            continue;
        }
        struct stat st;
        if (stat(row->path, &st) == 0 || errno != ENOENT) ok = false;
    }
    return ok;
}

void tagcache_free_stats(tagcache_stats_snapshot_t * snapshot) {
    if (!snapshot) return;
    for (size_t i = 0; i < snapshot->count; i++) free(snapshot->rows[i].path);
    free(snapshot->rows);
    free(snapshot);
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
#include "tagcache_reader.h"
#include "tagcache_sort.h"
static bool scan_intern_init(bool include_titles);
static bool scan_intern_find(const char *value, char *out, size_t out_size);
static bool query_build(int output_fd);
static bool query_validate(int fd, uint64_t file_size);
#include "tagcache_reader_index.h"
#include "tagcache_query_index.h"
#include "tagcache_query.h"
#include "tagcache_reader_legacy.h"
#include "tagcache_scan_intern.h"
/* Scan and commit work use the existing worker's selected reader. */
static bool scan_view_ready, scan_initializing;
#define scan_selected (selected_reader == &scan_reader)
static void scan_select(bool selected) {
    if (scan_view_ready) selected_reader = selected ? &scan_reader : &committed_reader;
}
static void scan_scope_leave(bool *unused) { (void)unused; scan_select(false); }
#define SCAN_SCOPE bool scan_scope __attribute__((cleanup(scan_scope_leave))) = true; scan_select(true)

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
static pthread_mutex_t numeric_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t numeric_cond = PTHREAD_COND_INITIALIZER;
static pthread_cond_t numeric_drain_cond = PTHREAD_COND_INITIALIZER;
static pthread_t numeric_thread;
static bool numeric_thread_running = false;
static bool numeric_shutdown = false;

/* Performs disk pwrite + fdatasync for a snapshot of numeric updates. */
static void flush_numeric_batch(const numeric_update_t * batch, int n) {
    if (n <= 0 || db_dir[0] == '\0') return;
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
        if (fd < 0) continue;
        struct stat st;
        if (fstat(fd, &st) != 0) {
            close(fd);
            continue;
        }
        struct master_header mh;
        if (pread(fd, &mh, sizeof(mh), 0) != (ssize_t) sizeof(mh) || !validate_master_header(&mh, (size_t) st.st_size)) {
            close(fd);
            continue;
        }
        bool any_written = false;
        for (int j = i; j < n; j++) {
            if (batch[j].gen != gen) continue;
            int32_t slot = batch[j].slot;
            if (slot < 0 || slot >= mh.tch.entry_count) continue;
            struct index_entry ie;
            off_t off = (off_t) sizeof(struct master_header) + (off_t) slot * (off_t) sizeof(ie);
            if (pread(fd, &ie, sizeof(ie), off) != (ssize_t) sizeof(ie)) continue;
            ie.tag_seek[tag_playcount] = batch[j].playcount;
            ie.tag_seek[tag_lastplayed] = batch[j].last_played;
            ie.tag_seek[tag_rating] = batch[j].rating;
            if (pwrite(fd, &ie, sizeof(ie), off) == (ssize_t) sizeof(ie)) {
                any_written = true;
            }
        }
        if (any_written) {
            fdatasync(fd);
        }
        close(fd);
    }
}

static void numeric_flush_locked(void) {
    if (numeric_queue_count == 0) return;
    numeric_update_t batch[NUMERIC_QUEUE_CAP];
    int n = numeric_queue_count;
    memcpy(batch, numeric_queue, sizeof(numeric_update_t) * (size_t) n);
    numeric_queue_count = 0;
    pthread_cond_broadcast(&numeric_drain_cond);
    flush_numeric_batch(batch, n);
}

static void tagcache_flush_numeric(void) {
    pthread_mutex_lock(&numeric_mutex);
    numeric_flush_locked();
    pthread_mutex_unlock(&numeric_mutex);
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
            numeric_flush_locked();
        }
    }
    numeric_flush_locked();
    pthread_mutex_unlock(&numeric_mutex);
    return NULL;
}

static void numeric_worker_start(void) {
    pthread_mutex_lock(&numeric_mutex);
    if (!numeric_thread_running) {
        numeric_shutdown = false;
        numeric_queue_count = 0;
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
    numeric_flush_locked();
    numeric_queue_count = 0;
    pthread_mutex_unlock(&numeric_mutex);
}

static void queue_numeric_update(int32_t slot, const struct index_entry *idx) {
    if (!disk_ready || disk_gen < 0) return;
    pthread_mutex_lock(&numeric_mutex);
    if (numeric_queue_count >= NUMERIC_QUEUE_CAP) numeric_flush_locked();
    numeric_update_t *u = &numeric_queue[numeric_queue_count++];
    *u = (numeric_update_t) {.gen = disk_gen, .slot = slot,
        .mtime = idx->tag_seek[tag_mtime], .size = idx->tag_seek[tag_lastoffset],
        .first_seen = idx->tag_seek[tag_commitid], .playcount = idx->tag_seek[tag_playcount],
        .last_played = idx->tag_seek[tag_lastplayed], .rating = idx->tag_seek[tag_rating],
        .disc_number = idx->tag_seek[tag_discnumber], .track_number = idx->tag_seek[tag_tracknumber],
        .flag = idx->flag};
    if (!numeric_thread_running) numeric_flush_locked();
    else pthread_cond_signal(&numeric_cond);
    pthread_mutex_unlock(&numeric_mutex);
}
/* Scan mutations use unlinked staging files in the database directory. */
#define SCAN_HASH_SLOTS (TAGCACHE_MAX_ENTRIES * 2u)
static int scan_hash_fd = -1, scan_numeric_fd = -1;

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
        off_t off = (off_t) bucket * sizeof(stored);
        if (stats_read_at(scan_hash_fd, &stored, sizeof(stored), off) != STATS_READ_OK) {
            update_failed = true;
            return -1;
        }
        if (!stored) {
            if (insert) {
                stored = new_slot + 1;
                if (!write_at(scan_hash_fd, &stored, sizeof(stored), off)) update_failed = true;
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
        off_t offset = sizeof(struct master_header) + (off_t)change[0] * sizeof(idx);
        if (!reader_read_at(fd, &idx, sizeof(idx), offset)) return false;
        idx.tag_seek[tag_rating] = change[1]; idx.tag_seek[tag_playcount] = change[2];
        idx.tag_seek[tag_lastplayed] = change[3];
        if (!write_at(fd, &idx, sizeof(idx), offset)) return false;
    }
    return true;
}

static bool start_staging_with_lock(void (*unlock)(void), void (*lock)(void)) {
    if (updating) return !update_failed;
    updating = true;
    update_failed = false;
    int master = -1, strings[TAG_COUNT];
    for (int t = 0; t < TAG_COUNT; t++) strings[t] = -1;
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
        strings[tag] = reader_create_temp();
        struct tagcache_header hdr = {TAGCACHE_MAGIC, 0, 0};
        ok = strings[tag] >= 0 && (reader_tag_fd[tag] >= 0 ? copy_fd(reader_tag_fd[tag], strings[tag]) :
                                  write_at(strings[tag], &hdr, sizeof(hdr), 0));
    }
    if (ok) {
        scan_hash_fd = reader_create_temp();
        ok = scan_hash_fd >= 0 && ftruncate(scan_hash_fd, (off_t) SCAN_HASH_SLOTS * sizeof(int32_t)) == 0;
    }
    if (!ok) {
        if (master >= 0) close(master);
        for (int t = 0; t < TAG_COUNT; t++) if (strings[t] >= 0) close(strings[t]);
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
    if (lock) lock();
    if (!replay_scan_numeric(reader_master_fd)) update_failed = true;
    scan_initializing = false;
    scan_view_ready = true;
    selected_reader = &committed_reader;
    return !update_failed;
}

static bool start_staging(void) { return start_staging_with_lock(NULL, NULL); }

static int32_t find_path(const char *path) {
    if (!path || !db_open) return -1;
    return scan_selected ? scan_find_path(path, false, -1) : reader_find_path(path);
}

static bool append_string(int tag, int32_t slot, const char *value, int32_t *seek) {
    if (!value) value = "";
    size_t len = strlen(value) + 1;
    if (len > TAGCACHE_PATH_MAX) return false;
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

void tagcache_begin_update_with_lock(void (*unlock)(void), void (*lock)(void)) {
    if (!db_open) return;
    tagcache_flush_numeric();
    start_staging_with_lock(unlock, lock);
}
void tagcache_begin_update(void) { tagcache_begin_update_with_lock(NULL, NULL); }

bool tagcache_lookup(const char *path, int32_t mtime, int32_t size, tagcache_song_t *out) {
    if (!db_open || !path) return false;
    SCAN_SCOPE;
    int32_t slot = find_path(path);
    struct index_entry idx;
    if (slot < 0 || !reader_index(slot, &idx) || (idx.flag & FLAG_DELETED)) return false;
    if (scan_selected) {
        idx.flag |= FLAG_SEEN;
        if (!scan_write_index(slot, &idx)) return false;
    }
    if (idx.tag_seek[tag_mtime] != mtime || idx.tag_seek[tag_lastoffset] != size) return false;
    tagcache_song_t song;
    if (!reader_song(slot, out ? out : &song)) return false;
    if (!(out ? out : &song)->album[0] || ((out ? out : &song)->flags & FLAG_TAGS_INCOMPLETE)) return false;
    return true;
}

void tagcache_upsert(const char *path, int32_t mtime, int32_t size, const char *title, const char *artist,
                     const char *album, const char *album_artist, const char *genre,
                     int32_t track_number, int32_t disc_number) {
    if (!db_open || !path || !path[0] || !start_staging() || update_failed) return;
    SCAN_SCOPE;
    int32_t slot = find_path(path);
    struct index_entry idx = {0};
    bool fresh = slot < 0;
    if (fresh) {
        if (update_failed || ent_count >= TAGCACHE_MAX_ENTRIES) { update_failed = true; return; }
        slot = ent_count;
        idx.tag_seek[tag_commitid] = (int32_t) time(NULL);
    } else if (!reader_index(slot, &idx)) { update_failed = true; return; }
    const int tags[] = {tag_filename, tag_title, tag_artist, tag_album, tag_albumartist, tag_genre};
    const char *values[] = {path, title, artist, album, album_artist, genre};
    for (int i = 0; i < 6; i++) {
        char canonical[TAGCACHE_PATH_MAX];
        const char *value = values[i] ? values[i] : "";
        if (tags[i] != tag_filename && scan_intern_find(value, canonical, sizeof(canonical))) value = canonical;
        if (update_failed || !append_string(tags[i], slot, value, &idx.tag_seek[tags[i]]) ||
            (tags[i] != tag_filename && !scan_intern_insert(tags[i], idx.tag_seek[tags[i]]))) {
            update_failed = true;
            return;
        }
    }
    idx.tag_seek[tag_mtime] = mtime;
    idx.tag_seek[tag_lastoffset] = size;
    idx.tag_seek[tag_tracknumber] = track_number > 0 ? track_number : -1;
    idx.tag_seek[tag_discnumber] = disc_number > 0 ? disc_number : -1;
    idx.flag = (idx.flag & ~(FLAG_DELETED | FLAG_TAGS_INCOMPLETE)) | FLAG_SEEN;
    if (!scan_write_index(slot, &idx)) return;
    reader_reset_cache();
    if (fresh) {
        ent_count++;
        live_count++;
        scan_find_path(path, true, slot);
    }
}

typedef struct { int32_t seek, slot; } commit_key_t;
static int commit_source_tag;
static bool commit_read_failed;

static int compare_commit_key(const void *a, const void *b, void *ctx) {
    (void) ctx;
    const commit_key_t *ka = a, *kb = b;
    char sa[TAGCACHE_PATH_MAX], sb[TAGCACHE_PATH_MAX];
    if (!reader_string(commit_source_tag, ka->seek, ka->slot, sa, sizeof(sa)) ||
        !reader_string(commit_source_tag, kb->seek, kb->slot, sb, sizeof(sb))) {
        commit_read_failed = true;
        return (ka->slot > kb->slot) - (ka->slot < kb->slot);
    }
    int c = ascii_casecmp(sa, sb);
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
    int64_t count = 0;
    for (int32_t slot = 0; ok && slot < ent_count; slot++) {
        struct index_entry idx;
        if (!reader_index(slot, &idx)) { ok = false; break; }
        if (idx.flag & FLAG_DELETED) continue;
        commit_key_t key = {empty ? 0 : idx.tag_seek[source], slot};
        ok = write_at(keys, &key, sizeof(key), (off_t) count * sizeof(key));
        count++;
    }
    commit_source_tag = source;
    commit_read_failed = false;
    if (ok && !empty && tag != tag_filename)
        ok = tc_sort_fd(keys, sizeof(commit_key_t), count, compare_commit_key, NULL, scratch) && !commit_read_failed;
    char name[80], path[640];
    tag_file_name(name, sizeof(name), tag, gen);
    db_path(path, sizeof(path), name);
    if (ok) ok = (fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644)) >= 0;
    struct tagcache_header hdr = {TAGCACHE_MAGIC, 0, 0};
    if (ok) ok = write_fully(fd, &hdr, sizeof(hdr));
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
            struct tagfile_entry te = {len, key.slot};
            seek = pos;
            ok = write_fully(fd, &te, sizeof(te)) && write_fully(fd, value, (size_t) len);
            pos += sizeof(te) + len;
            hdr.entry_count++;
            snprintf(previous, sizeof(previous), "%s", value);
        }
        if (ok) ok = write_at(master, &seek, sizeof(seek), sizeof(struct master_header) +
                               (off_t) key.slot * sizeof(struct index_entry) + (off_t) tag * sizeof(int32_t));
    }
    hdr.datasize = pos - sizeof(hdr);
    if (ok) ok = write_at(fd, &hdr, sizeof(hdr), 0);
    if (fd >= 0 && !close_synced(fd)) ok = false;
    if (keys >= 0) close(keys);
    if (scratch >= 0) close(scratch);
    if (!ok) unlink(path);
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
    char name[80], path[640];
    master_file_name(name, sizeof(name), gen);
    db_path(path, sizeof(path), name);
    int master = open(path, O_CREAT | O_TRUNC | O_RDWR, 0644);
    if (master < 0) return false;
    struct master_header mh = {{TAGCACHE_INDEXED_MAGIC, ent_count * (int32_t) sizeof(struct index_entry), ent_count},
                                master_serial, gen, 0};
    if (unlock) unlock();
    bool ok = write_fully(master, &mh, sizeof(mh));
    for (int32_t slot = 0; ok && slot < ent_count; slot++) {
        struct index_entry idx;
        ok = reader_index(slot, &idx);
        if (!ok) break;
        idx.flag &= ~FLAG_RAM_ONLY;
        if (idx.flag & FLAG_DELETED) { memset(&idx, 0, sizeof(idx)); idx.flag = FLAG_DELETED; }
        ok = write_fully(master, &idx, sizeof(idx));
    }
    for (size_t i = 0; ok && i < sizeof(persist_tag_ids) / sizeof(persist_tag_ids[0]); i++)
        ok = write_commit_tag(persist_tag_ids[i], gen, master);
    if (!close_synced(master)) ok = false;
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
    if (ok) ok = library_fsync_dir(db_dir);
    if (ok && !rebuild_preserve_generations) ok = write_gen_pointer(gen);
    if (!ok) {
        unlink_generation(gen);
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
    char dir[sizeof(db_dir)];
    snprintf(dir, sizeof(dir), "%s", db_dir);
    bool require_saved = disk_ready;
    bool ok = tagcache_open(dir);
    if (ok && require_saved && last_load_outcome == TAGCACHE_LOAD_SUCCESS_FRESH) {
        tagcache_close();
        last_load_outcome = TAGCACHE_LOAD_FAILED;
        return false;
    }
    return ok;
}

bool tagcache_open(const char *dir) {
    tagcache_close();
    last_load_outcome = TAGCACHE_LOAD_FAILED;
    if (!dir || !dir[0] || strlen(dir) >= sizeof(db_dir)) return false;
    snprintf(db_dir, sizeof(db_dir), "%s", dir);
    db_open = true;
    if (!load_all()) { tagcache_close(); return false; }
    numeric_worker_start();
    return true;
}

bool tagcache_open_for_rebuild(const char *dir) {
    if (tagcache_open(dir)) return true;
    if (!dir || !dir[0] || strlen(dir) >= sizeof(db_dir)) return false;
    struct stat st;
    if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode) || access(dir, R_OK | W_OK) != 0) return false;
    tagcache_close();
    snprintf(db_dir, sizeof(db_dir), "%s", dir);
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
    if (scan_hash_fd >= 0) close(scan_hash_fd);
    scan_hash_fd = -1;
    ent_count = live_count = 0;
    disk_gen = master_commitid = 0;
    master_serial = 1;
    db_open = disk_ready = rebuild_preserve_generations = updating = scan_initializing = update_failed = false;
    db_dir[0] = '\0';
}

tagcache_load_outcome_t tagcache_get_load_outcome(void) { return last_load_outcome; }

bool tagcache_end_update_with_lock(void (*unlock)(void), void (*lock)(void)) {
    if (!db_open) return false;
    tagcache_flush_numeric();
    bool preserve = rebuild_preserve_generations;
    bool ok = start_staging_with_lock(unlock, lock);
    SCAN_SCOPE;
    live_count = 0;
    for (int32_t slot = 0; ok && slot < ent_count; slot++) {
        struct index_entry idx;
        ok = reader_index(slot, &idx);
        if (!ok) break;
        if (!(idx.flag & (FLAG_DELETED | FLAG_SEEN))) {
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
    if (ok && !update_failed) ok = write_all(unlock, lock); else ok = false;
    scan_select(false);
    bool loaded = reload_from_disk();
    if (ok && loaded && preserve) last_load_outcome = TAGCACHE_LOAD_SUCCESS_RECOVERED;
    return ok && loaded;
}

bool tagcache_end_update(void) { return tagcache_end_update_with_lock(NULL, NULL); }

void tagcache_abort_update(void) {
    if (!db_open) return;
    tagcache_flush_numeric();
    reload_from_disk();
}

int32_t tagcache_live_count(void) { return db_open ? live_count : 0; }
int32_t tagcache_slot_count(void) { return db_open ? ent_count : 0; }

bool tagcache_song_fields_by_id(int32_t id, unsigned fields, tagcache_song_t *out) {
    if (!db_open || id < 1 || id > ent_count || !out) return false;
    tagcache_flush_numeric();
    return reader_song_fields(id - 1, fields, out);
}

bool tagcache_song_fields_at_title_rank(int32_t rank, unsigned fields, tagcache_song_t *out) {
    if (!db_open || rank < 0 || rank >= live_count || !out) return false;
    tagcache_flush_numeric();
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
    tagcache_flush_numeric();
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
    tagcache_flush_numeric();
    struct index_entry idx;
    return out ? reader_song(id - 1, out) : reader_index(id - 1, &idx) && !(idx.flag & FLAG_DELETED);
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
    tagcache_flush_numeric();
    return reader_order_song(reader_title_fd, false, rank, out);
}

bool tagcache_song_at_recency_rank(int32_t rank, tagcache_song_t *out) {
    if (!db_open) return false;
    tagcache_flush_numeric();
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
    tagcache_flush_numeric();
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
