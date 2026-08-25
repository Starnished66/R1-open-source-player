/* POSIX port of Rockbox tagcache (apps/tagcache.c).
 *
 * Copyright (C) 2005 Miika Pekkarinen (original engine / on-disk format)
 * Copyright (C) Open HiBy Player contributors (POSIX host, no Rockbox kernel)
 *
 * The on-disk layout, magic, unique/sorted tag files, master index, and
 * numeric-tag placement follow Rockbox TAGCACHE_MAGIC 0x54434810. Scanning
 * and metadata parsing stay in this project; this file is the database.
 *
 * RAM holds packed entries plus interned unique tags (artist/album/genre)
 * and compact group membership lists. Title and path strings are interned
 * only when MemAvailable can hold them; larger libraries mmap those two
 * tag files and resolve strings on demand so a 200k library does not keep
 * every filename in RSS. Each commit writes a complete generation
 * (`database_*.tcd.gN`) and atomically switches `tagcache.gen`. */

#include "tagcache.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

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
#define FLAG_DELETED 0x0001
#define FLAG_SEEN 0x01000000 /* RAM-only, stripped before persist */
#define FLAG_TAGS_INCOMPLETE 0x02000000 /* RAM-only: unique-tag seek missed on load */
#define FLAG_RAM_ONLY (FLAG_SEEN | FLAG_TAGS_INCOMPLETE)
#define TAGCACHE_MAX_ENTRIES 524288
#define INTERN_BUCKETS 8192
#define TAGCACHE_RAM_RESERVE (16ull * 1024ull * 1024ull)
#define TAGCACHE_FULL_BYTES_PER_SONG 200ull

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

typedef struct arena_block {
    struct arena_block * next;
    size_t used;
    size_t cap;
    char data[];
} arena_block_t;

typedef struct intern {
    uint32_t hash;
    const char * ptr;
    int persist_gen;
    int32_t persist_seek;
    struct intern * next;
} intern_t;

typedef struct {
    int32_t flag;
    int32_t mtime;
    int32_t size;
    int32_t first_seen;
    int32_t playcount;
    int32_t last_played;
    int32_t rating;
    int32_t path_seek;
    int32_t title_seek;
    uint32_t path_h;
    const char * path; /* interned, or NULL when titles/paths are mapped */
    const char * title;
    const char * artist;
    const char * album;
    const char * album_artist;
    const char * genre;
} entry_t;

typedef struct {
    const char * name;
    const char * album_artist;
    int song_count;
    int32_t first_song_id;
    int32_t * songs;
} group_t;

static char db_dir[512];
static bool db_open;
static bool disk_ready;
static bool opened_ok;

static arena_block_t * arena_blocks;
static intern_t ** intern_exact_buckets;
static intern_t ** intern_nocase_buckets;

static entry_t * ents;
static int32_t ent_count;
static int32_t ent_cap;
static int32_t live_count;

static int32_t * title_order;
static int32_t * recency_order;
static int32_t * title_rank_of;
static int32_t * recency_rank_of;

static int32_t * path_hash;
static int32_t path_hash_cap;

static group_t * groups[3];
static int group_n[3];

static int32_t master_serial = 1;
static int32_t master_commitid;
static int32_t disk_gen;
static int persist_gen;
static bool intern_path_title = true;
static const char * map_filename;
static size_t map_filename_len;
static const char * map_title;
static size_t map_title_len;
static int32_t * loaded_title_order;
static int32_t loaded_title_n;

static uint32_t fnv1a(const char * s, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        h ^= (unsigned char) s[i];
        h *= 16777619u;
    }
    return h;
}

static unsigned char ascii_fold(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return (unsigned char) (c + 32);
    return c;
}

static int ascii_casecmp(const char * a, const char * b) {
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

static const char * arena_put(const char * s, size_t len) {
    arena_block_t * b = arena_blocks;
    if (!b || b->used + len + 1 > b->cap) {
        size_t cap = 65536;
        if (len + 1 > cap) cap = len + 4096;
        b = malloc(sizeof(*b) + cap);
        if (!b) return "";
        b->next = arena_blocks;
        b->used = 0;
        b->cap = cap;
        arena_blocks = b;
    }
    char * p = b->data + b->used;
    memcpy(p, s, len);
    p[len] = '\0';
    b->used += len + 1;
    return p;
}

static intern_t ** intern_table(bool nocase) {
    intern_t *** root = nocase ? &intern_nocase_buckets : &intern_exact_buckets;
    if (!*root) *root = calloc(INTERN_BUCKETS, sizeof(**root));
    return *root;
}

static intern_t * intern_node(const char * s, bool nocase) {
    if (!s) s = "";
    size_t len = strlen(s);
    char folded[TAGCACHE_PATH_MAX];
    const char * key = s;
    if (nocase) {
        size_t n = len < sizeof(folded) - 1 ? len : sizeof(folded) - 1;
        for (size_t i = 0; i < n; i++) folded[i] = (char) ascii_fold((unsigned char) s[i]);
        folded[n] = '\0';
        key = folded;
        len = n;
    }
    uint32_t h = fnv1a(key, len);
    intern_t ** buckets = intern_table(nocase);
    if (!buckets) return NULL;
    uint32_t slot = h & (INTERN_BUCKETS - 1);
    for (intern_t * it = buckets[slot]; it; it = it->next) {
        if (it->hash != h) continue;
        if (nocase) {
            if (ascii_casecmp(it->ptr, s) == 0) return it;
        } else if (strcmp(it->ptr, s) == 0) {
            return it;
        }
    }
    intern_t * node = malloc(sizeof(*node));
    if (!node) return NULL;
    node->hash = h;
    node->persist_gen = 0;
    node->persist_seek = 0;
    node->ptr = arena_put(s, strlen(s));
    node->next = buckets[slot];
    buckets[slot] = node;
    return node;
}

static const char * intern_len(const char * s, bool nocase) {
    intern_t * node = intern_node(s, nocase);
    return node ? node->ptr : "";
}

static const char * intern_tag(const char * s) {
    if (!s) s = "";
    char buf[TAGCACHE_TAG_MAX];
    snprintf(buf, sizeof(buf), "%s", s);
    return intern_len(buf, true);
}

static const char * intern_path(const char * s) {
    if (!s) s = "";
    char buf[TAGCACHE_PATH_MAX];
    snprintf(buf, sizeof(buf), "%s", s);
    return intern_len(buf, false);
}

static size_t mem_available_bytes(void) {
    FILE * f = fopen("/proc/meminfo", "r");
    if (f) {
        char line[160];
        size_t avail = 0, memfree = 0, cached = 0;
        while (fgets(line, sizeof(line), f)) {
            unsigned long v = 0;
            if (sscanf(line, "MemAvailable: %lu", &v) == 1) avail = (size_t) v * 1024u;
            else if (sscanf(line, "MemFree: %lu", &v) == 1) memfree = (size_t) v * 1024u;
            else if (sscanf(line, "Cached: %lu", &v) == 1) cached = (size_t) v * 1024u;
        }
        fclose(f);
        if (avail) return avail;
        return memfree + cached;
    }
#ifdef _SC_AVPHYS_PAGES
    {
        long pages = sysconf(_SC_AVPHYS_PAGES);
        long sz = sysconf(_SC_PAGESIZE);
        if (pages > 0 && sz > 0) return (size_t) pages * (size_t) sz;
    }
#endif
    return 256ull * 1024ull * 1024ull;
}

static bool choose_intern_strings(int32_t n) {
    const char * env = getenv("TAGCACHE_FORCE_COMPACT");
    if (env && env[0] && env[0] != '0') return false;
    env = getenv("TAGCACHE_FORCE_INTERN");
    if (env && env[0] && env[0] != '0') return true;
    if (n < 0) n = 0;
    size_t avail = mem_available_bytes();
    size_t full = (size_t) n * TAGCACHE_FULL_BYTES_PER_SONG + 3ull * 1024ull * 1024ull;
    return full + TAGCACHE_RAM_RESERVE <= avail;
}

static void unmap_string_files(void) {
    if (map_filename && map_filename != MAP_FAILED) munmap((void *) map_filename, map_filename_len);
    if (map_title && map_title != MAP_FAILED) munmap((void *) map_title, map_title_len);
    map_filename = NULL;
    map_title = NULL;
    map_filename_len = 0;
    map_title_len = 0;
}

static const char * mapped_string(const char * map, size_t len, int32_t seek) {
    if (!map || seek < (int32_t) sizeof(struct tagcache_header)) return NULL;
    if ((size_t) seek + sizeof(struct tagfile_entry) > len) return NULL;
    struct tagfile_entry te;
    memcpy(&te, map + (size_t) seek, sizeof(te));
    /* tag_length includes the trailing NUL. 1 is a valid empty title. */
    if (te.tag_length <= 0) return NULL;
    if ((size_t) seek + sizeof(te) + (size_t) te.tag_length > len) return NULL;
    const char * s = map + (size_t) seek + sizeof(te);
    /* Compact loads mmap the tag file. strlen/casecmp must not run off the
     * map if a torn write left the record without a terminator. */
    if (!memchr(s, '\0', (size_t) te.tag_length)) return NULL;
    return s;
}

static const char * entry_path_str(const entry_t * e) {
    if (!e) return "";
    if (e->path) return e->path;
    const char * s = mapped_string(map_filename, map_filename_len, e->path_seek);
    return s ? s : "";
}

static const char * entry_title_str(const entry_t * e) {
    if (!e) return "";
    if (e->title) return e->title;
    const char * s = mapped_string(map_title, map_title_len, e->title_seek);
    return s ? s : "";
}

static void intern_free_table(intern_t ** buckets) {
    if (!buckets) return;
    for (int i = 0; i < INTERN_BUCKETS; i++) {
        intern_t * it = buckets[i];
        while (it) {
            intern_t * n = it->next;
            free(it);
            it = n;
        }
    }
    free(buckets);
}

static void arena_reset(void) {
    intern_free_table(intern_exact_buckets);
    intern_free_table(intern_nocase_buckets);
    intern_exact_buckets = NULL;
    intern_nocase_buckets = NULL;
    while (arena_blocks) {
        arena_block_t * n = arena_blocks->next;
        free(arena_blocks);
        arena_blocks = n;
    }
}

static void path_hash_clear(void) {
    free(path_hash);
    path_hash = NULL;
    path_hash_cap = 0;
}

static void path_hash_grow(int32_t min_cap) {
    if (min_cap < 16) min_cap = 16;
    if (min_cap > TAGCACHE_MAX_ENTRIES * 4) min_cap = TAGCACHE_MAX_ENTRIES * 4;
    int32_t cap = 16;
    while (cap < min_cap) {
        if (cap > INT32_MAX / 2) {
            cap = min_cap;
            break;
        }
        cap *= 2;
    }
    int32_t * next = calloc((size_t) cap, sizeof(*next));
    if (!next) return;
    int32_t old_cap = path_hash_cap;
    int32_t * old = path_hash;
    path_hash = next;
    path_hash_cap = cap;
    if (old) {
        for (int32_t i = 0; i < old_cap; i++) {
            if (old[i] == 0) continue;
            int32_t idx = old[i] - 1;
            uint32_t h = ents[idx].path_h;
            if (!h && ents[idx].path) h = fnv1a(ents[idx].path, strlen(ents[idx].path));
            int32_t slot = (int32_t) (h & (uint32_t) (cap - 1));
            for (int32_t probe = 0; probe < cap; probe++) {
                if (path_hash[slot] == 0) {
                    path_hash[slot] = old[i];
                    break;
                }
                slot = (slot + 1) & (cap - 1);
            }
        }
        free(old);
    }
}

static void path_hash_insert(int32_t idx) {
    if (path_hash_cap == 0 || (ent_count + 1) * 2 >= path_hash_cap)
        path_hash_grow(path_hash_cap ? path_hash_cap * 2 : 64);
    if (!path_hash || path_hash_cap == 0) return;
    uint32_t h = ents[idx].path_h;
    if (!h && ents[idx].path) h = fnv1a(ents[idx].path, strlen(ents[idx].path));
    if (!h && !entry_path_str(&ents[idx])[0]) return;
    int32_t slot = (int32_t) (h & (uint32_t) (path_hash_cap - 1));
    for (int32_t probe = 0; probe < path_hash_cap; probe++) {
        if (path_hash[slot] == 0) {
            path_hash[slot] = idx + 1;
            return;
        }
        slot = (slot + 1) & (path_hash_cap - 1);
    }
}

static int32_t path_hash_find(const char * path) {
    if (!path_hash || path_hash_cap == 0 || !path) return -1;
    uint32_t h = fnv1a(path, strlen(path));
    int32_t slot = (int32_t) (h & (uint32_t) (path_hash_cap - 1));
    int32_t deleted_idx = -1;
    for (int32_t n = 0; n < path_hash_cap; n++) {
        int32_t v = path_hash[slot];
        if (v == 0) return deleted_idx;
        int32_t idx = v - 1;
        if (ents[idx].path_h == h || ents[idx].path) {
            if (strcmp(entry_path_str(&ents[idx]), path) == 0) {
                if (!(ents[idx].flag & FLAG_DELETED)) return idx;
                if (deleted_idx < 0) deleted_idx = idx;
            }
        }
        slot = (slot + 1) & (path_hash_cap - 1);
    }
    return deleted_idx;
}

static void fill_song(int32_t idx, tagcache_song_t * out) {
    const entry_t * e = &ents[idx];
    out->id = idx + 1;
    out->mtime = e->mtime;
    out->size = e->size;
    out->first_seen = e->first_seen;
    out->playcount = e->playcount;
    out->last_played = e->last_played;
    out->rating = e->rating;
    out->path = entry_path_str(e);
    out->title = entry_title_str(e);
    out->artist = e->artist ? e->artist : "";
    out->album = e->album ? e->album : "";
    out->album_artist = e->album_artist ? e->album_artist : "";
    out->genre = e->genre ? e->genre : "";
}

static int cmp_title(const void * a, const void * b) {
    int32_t ia = *(const int32_t *) a;
    int32_t ib = *(const int32_t *) b;
    int c = ascii_casecmp(entry_title_str(&ents[ia]), entry_title_str(&ents[ib]));
    if (c) return c;
    return (ia > ib) - (ia < ib);
}

static int cmp_recency(const void * a, const void * b) {
    int32_t ia = *(const int32_t *) a;
    int32_t ib = *(const int32_t *) b;
    if (ents[ia].first_seen != ents[ib].first_seen) return ents[ia].first_seen < ents[ib].first_seen ? 1 : -1;
    if (intern_path_title) {
        int c = ascii_casecmp(entry_path_str(&ents[ia]), entry_path_str(&ents[ib]));
        if (c) return c;
    }
    return (ia > ib) - (ia < ib);
}

static int cmp_group(const void * a, const void * b) {
    const group_t * ga = a;
    const group_t * gb = b;
    int c = ascii_casecmp(ga->name, gb->name);
    if (c) return c;
    return ascii_casecmp(ga->album_artist ? ga->album_artist : "", gb->album_artist ? gb->album_artist : "");
}

static int cmp_slot_album_path(const void * a, const void * b) {
    int32_t ia = *(const int32_t *) a;
    int32_t ib = *(const int32_t *) b;
    int c = ascii_casecmp(ents[ia].album ? ents[ia].album : "", ents[ib].album ? ents[ib].album : "");
    if (c) return c;
    if (intern_path_title) {
        int p = ascii_casecmp(entry_path_str(&ents[ia]), entry_path_str(&ents[ib]));
        if (p) return p;
    }
    return (ia > ib) - (ia < ib);
}

static int cmp_slot_path(const void * a, const void * b) {
    int32_t ia = *(const int32_t *) a;
    int32_t ib = *(const int32_t *) b;
    if (intern_path_title) {
        int c = ascii_casecmp(entry_path_str(&ents[ia]), entry_path_str(&ents[ib]));
        if (c) return c;
    }
    return (ia > ib) - (ia < ib);
}

static void free_groups(void) {
    for (int k = 0; k < 3; k++) {
        if (groups[k]) {
            for (int i = 0; i < group_n[k]; i++) free(groups[k][i].songs);
        }
        free(groups[k]);
        groups[k] = NULL;
        group_n[k] = 0;
    }
}

/* Finalizing a large scan used to build every replacement index/group while
 * retaining the complete old derived view. None of those old structures is
 * needed after pruning has finished: lookup/upsert is over, and a failed
 * rebuild already recovers by reloading the last committed generation.
 * Drop them before rebuild_indexes() so old and new group membership,
 * ordering, rank, and hash allocations do not overlap at the scan's peak. */
static void drop_derived_indexes_before_rebuild(void) {
    free(title_order);
    free(recency_order);
    free(title_rank_of);
    free(recency_rank_of);
    title_order = NULL;
    recency_order = NULL;
    title_rank_of = NULL;
    recency_rank_of = NULL;
    path_hash_clear();
    free_groups();
}

typedef struct gmap {
    const char * a;
    const char * b;
    int count;
    int cap;
    int32_t first;
    int32_t * slots;
    struct gmap * next;
} gmap_t;

static void gmap_free_table(gmap_t ** map, int buckets) {
    if (!map) return;
    for (int s = 0; s < buckets; s++) {
        gmap_t * it = map[s];
        while (it) {
            gmap_t * n = it->next;
            free(it->slots);
            free(it);
            it = n;
        }
    }
    free(map);
}

static bool rebuild_indexes(void) {
    int32_t new_live = 0;
    for (int32_t i = 0; i < ent_count; i++) {
        if (!(ents[i].flag & FLAG_DELETED)) new_live++;
    }

    int32_t * new_title = NULL;
    int32_t * new_recency = NULL;
    if (new_live > 0) {
        new_title = malloc(sizeof(int32_t) * (size_t) new_live);
        new_recency = malloc(sizeof(int32_t) * (size_t) new_live);
        if (!new_title || !new_recency) {
            free(new_title);
            free(new_recency);
            return false;
        }
    }

    int32_t * new_hash = NULL;
    int32_t new_hash_cap = 0;
    int32_t hash_need = (ent_count + 1) * 2;
    if (hash_need < 64) hash_need = 64;
    new_hash_cap = 16;
    while (new_hash_cap < hash_need) {
        if (new_hash_cap > INT32_MAX / 2) {
            new_hash_cap = hash_need;
            break;
        }
        new_hash_cap *= 2;
    }
    new_hash = calloc((size_t) new_hash_cap, sizeof(*new_hash));
    if (!new_hash) {
        free(new_title);
        free(new_recency);
        return false;
    }

    int32_t nlive = 0;
    for (int32_t i = 0; i < ent_count; i++) {
        if (!(ents[i].flag & FLAG_DELETED)) {
            uint32_t h = ents[i].path_h;
            if (!h && ents[i].path) h = fnv1a(ents[i].path, strlen(ents[i].path));
            if (h) {
                int32_t slot = (int32_t) (h & (uint32_t) (new_hash_cap - 1));
                while (new_hash[slot] != 0) slot = (slot + 1) & (new_hash_cap - 1);
                new_hash[slot] = i + 1;
            }
        }
        if (ents[i].flag & FLAG_DELETED) continue;
        if (new_title) {
            new_title[nlive] = i;
            new_recency[nlive] = i;
        }
        nlive++;
    }
    if (nlive > 0) {
        if (loaded_title_order && loaded_title_n == nlive) {
            memcpy(new_title, loaded_title_order, sizeof(int32_t) * (size_t) nlive);
            free(loaded_title_order);
            loaded_title_order = NULL;
            loaded_title_n = 0;
        } else {
            qsort(new_title, (size_t) nlive, sizeof(int32_t), cmp_title);
        }
        qsort(new_recency, (size_t) nlive, sizeof(int32_t), cmp_recency);
    }

    const int buckets = 4096;
    group_t * new_groups[3] = { 0 };
    int new_n[3] = { 0 };
    bool ok = true;
    for (int kind = 0; kind < 3 && ok; kind++) {
        gmap_t ** map = calloc((size_t) buckets, sizeof(*map));
        if (!map) {
            ok = false;
            break;
        }
        int unique = 0;
        for (int32_t i = 0; i < ent_count && ok; i++) {
            if (ents[i].flag & FLAG_DELETED) continue;
            const char * a = "";
            const char * b = "";
            if (kind == TAGCACHE_GROUP_ARTIST) a = ents[i].artist;
            else if (kind == TAGCACHE_GROUP_ALBUM_ARTIST) a = ents[i].album_artist;
            else {
                a = ents[i].album;
                b = ents[i].album_artist;
            }
            uint32_t h = fnv1a(a, strlen(a)) ^ (fnv1a(b, strlen(b)) << 1);
            int slot = (int) (h & (uint32_t) (buckets - 1));
            gmap_t * it = map[slot];
            while (it && (it->a != a || it->b != b)) it = it->next;
            if (!it) {
                it = calloc(1, sizeof(*it));
                if (!it) {
                    ok = false;
                    break;
                }
                it->a = a;
                it->b = b;
                it->first = i + 1;
                it->next = map[slot];
                map[slot] = it;
                unique++;
            }
            if (it->count >= it->cap) {
                int cap = it->cap ? it->cap * 2 : 4;
                int32_t * ns = realloc(it->slots, sizeof(*ns) * (size_t) cap);
                if (!ns) {
                    ok = false;
                    break;
                }
                it->slots = ns;
                it->cap = cap;
            }
            it->slots[it->count++] = i;
            if (i + 1 < it->first) it->first = i + 1;
        }
        if (!ok) {
            gmap_free_table(map, buckets);
            break;
        }
        new_groups[kind] = calloc((size_t) (unique > 0 ? unique : 1), sizeof(group_t));
        if (!new_groups[kind]) {
            gmap_free_table(map, buckets);
            ok = false;
            break;
        }
        int w = 0;
        for (int s = 0; s < buckets; s++) {
            gmap_t * it = map[s];
            while (it) {
                gmap_t * next = it->next;
                new_groups[kind][w].name = it->a;
                new_groups[kind][w].album_artist = kind == TAGCACHE_GROUP_ALBUM ? it->b : "";
                new_groups[kind][w].song_count = it->count;
                new_groups[kind][w].first_song_id = it->first;
                new_groups[kind][w].songs = it->slots;
                it->slots = NULL;
                if (kind == TAGCACHE_GROUP_ALBUM)
                    qsort(new_groups[kind][w].songs, (size_t) it->count, sizeof(int32_t), cmp_slot_path);
                else
                    qsort(new_groups[kind][w].songs, (size_t) it->count, sizeof(int32_t), cmp_slot_album_path);
                w++;
                free(it);
                it = next;
            }
        }
        new_n[kind] = w;
        qsort(new_groups[kind], (size_t) w, sizeof(group_t), cmp_group);
        free(map);
    }

    int32_t * new_title_rank = NULL;
    int32_t * new_recency_rank = NULL;
    if (ok && ent_count > 0) {
        new_title_rank = malloc(sizeof(int32_t) * (size_t) ent_count);
        new_recency_rank = malloc(sizeof(int32_t) * (size_t) ent_count);
        if (!new_title_rank || !new_recency_rank) {
            free(new_title_rank);
            free(new_recency_rank);
            new_title_rank = new_recency_rank = NULL;
            ok = false;
        } else {
            for (int32_t i = 0; i < ent_count; i++) {
                new_title_rank[i] = -1;
                new_recency_rank[i] = -1;
            }
            for (int32_t i = 0; i < nlive; i++) {
                new_title_rank[new_title[i]] = i;
                new_recency_rank[new_recency[i]] = i;
            }
        }
    }

    if (!ok) {
        for (int k = 0; k < 3; k++) {
            if (new_groups[k]) {
                for (int i = 0; i < new_n[k]; i++) free(new_groups[k][i].songs);
            }
            free(new_groups[k]);
        }
        free(new_title);
        free(new_recency);
        free(new_hash);
        free(new_title_rank);
        free(new_recency_rank);
        return false;
    }

    free(title_order);
    free(recency_order);
    free(title_rank_of);
    free(recency_rank_of);
    path_hash_clear();
    free_groups();
    title_order = new_title;
    recency_order = new_recency;
    title_rank_of = new_title_rank;
    recency_rank_of = new_recency_rank;
    path_hash = new_hash;
    path_hash_cap = new_hash_cap;
    live_count = nlive;
    for (int k = 0; k < 3; k++) {
        groups[k] = new_groups[k];
        group_n[k] = new_n[k];
    }
    return true;
}

static bool ensure_cap(int32_t need) {
    if (need < 0 || need > TAGCACHE_MAX_ENTRIES) return false;
    if (need <= ent_cap) return true;
    int32_t cap = ent_cap ? ent_cap : 64;
    while (cap < need) {
        if (cap > TAGCACHE_MAX_ENTRIES / 2) {
            cap = TAGCACHE_MAX_ENTRIES;
            break;
        }
        cap *= 2;
    }
    if (cap < need) return false;
    entry_t * n = realloc(ents, sizeof(*n) * (size_t) cap);
    if (!n) return false;
    memset(n + ent_cap, 0, sizeof(*n) * (size_t) (cap - ent_cap));
    ents = n;
    ent_cap = cap;
    return true;
}

static void apply_tags(entry_t * e, const char * path, int32_t mtime, int32_t size, const char * title,
                       const char * artist, const char * album, const char * album_artist, const char * genre) {
    e->path_h = path ? fnv1a(path, strlen(path)) : 0;
    e->path = intern_path(path);
    e->title = intern_tag(title);
    e->artist = intern_tag(artist);
    e->album = intern_tag(album);
    e->album_artist = intern_tag(album_artist);
    e->genre = intern_tag(genre);
    e->mtime = mtime;
    e->size = size;
    e->flag &= ~(FLAG_DELETED | FLAG_TAGS_INCOMPLETE);
    e->flag |= FLAG_SEEN;
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

static bool read_fully(int fd, void * buf, size_t n) {
    unsigned char * p = buf;
    size_t off = 0;
    while (off < n) {
        ssize_t r = read(fd, p + off, n - off);
        if (r == 0) return false;
        if (r < 0) return false;
        off += (size_t) r;
    }
    return true;
}

static bool close_synced(int fd) {
    bool ok = fsync(fd) == 0;
    if (close(fd) != 0) ok = false;
    return ok;
}

static bool fsync_dir(const char * dir) {
    int fd = open(dir, O_RDONLY | O_DIRECTORY);
    if (fd < 0) return false;
    bool ok = fsync(fd) == 0;
    close(fd);
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
    if (fd < 0) return false;
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%d\n", gen);
    bool ok = n > 0 && write_fully(fd, buf, (size_t) n);
    if (!close_synced(fd) || !ok) {
        unlink(tmp);
        return false;
    }
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return false;
    }
    return fsync_dir(db_dir);
}

static bool read_gen_pointer(int32_t * out) {
    char path[640];
    db_path(path, sizeof(path), "tagcache.gen");
    FILE * f = fopen(path, "r");
    if (!f) return false;
    int gen = 0;
    bool ok = fscanf(f, "%d", &gen) == 1 && gen > 0;
    fclose(f);
    if (ok) *out = gen;
    return ok;
}

static void unlink_generation(int32_t gen) {
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

static const char * unique_string(int tag, const entry_t * e) {
    switch (tag) {
        case tag_artist:
        case tag_composer:
        case tag_virt_canonicalartist: return e->artist;
        case tag_album: return e->album;
        case tag_genre: return e->genre;
        case tag_albumartist: return e->album_artist;
        case tag_grouping: return intern_path_title ? entry_title_str(e) : "";
        default: return "";
    }
}

static bool write_tag_new(int tag, int32_t gen, int32_t * title_seek, int32_t * path_seek) {
    char name[80], path[640];
    tag_file_name(name, sizeof(name), tag, gen);
    db_path(path, sizeof(path), name);
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0) return false;
    struct tagcache_header hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = TAGCACHE_MAGIC;
    if (!write_fully(fd, &hdr, sizeof(hdr))) {
        close(fd);
        unlink(path);
        return false;
    }
    int32_t pos = (int32_t) sizeof(hdr);
    int32_t datasize = 0;
    int n = 0;
    bool ok = true;

    if (tag == tag_title) {
        for (int32_t r = 0; ok && r < live_count; r++) {
            int32_t i = title_order[r];
            const char * s = entry_title_str(&ents[i]);
            int32_t len = (int32_t) strlen(s) + 1;
            struct tagfile_entry te = { len, i };
            if (title_seek) title_seek[i] = pos;
            ok = write_fully(fd, &te, sizeof(te)) && write_fully(fd, s, (size_t) len);
            pos += (int32_t) sizeof(te) + len;
            datasize += (int32_t) sizeof(te) + len;
            n++;
        }
    } else if (tag == tag_filename) {
        for (int32_t i = 0; ok && i < ent_count; i++) {
            if (ents[i].flag & FLAG_DELETED) continue;
            const char * s = entry_path_str(&ents[i]);
            int32_t len = (int32_t) strlen(s) + 1;
            struct tagfile_entry te = { len, i };
            if (path_seek) path_seek[i] = pos;
            ok = write_fully(fd, &te, sizeof(te)) && write_fully(fd, s, (size_t) len);
            pos += (int32_t) sizeof(te) + len;
            datasize += (int32_t) sizeof(te) + len;
            n++;
        }
    } else {
        persist_gen++;
        for (int32_t i = 0; ok && i < ent_count; i++) {
            if (ents[i].flag & FLAG_DELETED) continue;
            intern_t * node = intern_node(unique_string(tag, &ents[i]), true);
            if (!node || node->persist_gen == persist_gen) continue;
            int32_t len = (int32_t) strlen(node->ptr) + 1;
            struct tagfile_entry te = { len, i };
            node->persist_gen = persist_gen;
            node->persist_seek = pos;
            ok = write_fully(fd, &te, sizeof(te)) && write_fully(fd, node->ptr, (size_t) len);
            pos += (int32_t) sizeof(te) + len;
            datasize += (int32_t) sizeof(te) + len;
            n++;
        }
    }
    hdr.entry_count = n;
    hdr.datasize = datasize;
    if (ok) ok = lseek(fd, 0, SEEK_SET) == 0 && write_fully(fd, &hdr, sizeof(hdr));
    if (!close_synced(fd) || !ok) {
        unlink(path);
        return false;
    }
    return true;
}

static void snapshot_unique_seeks(int tag, int32_t * out) {
    for (int32_t i = 0; i < ent_count; i++) {
        out[i] = 0;
        if (ents[i].flag & FLAG_DELETED) continue;
        intern_t * node = intern_node(unique_string(tag, &ents[i]), true);
        if (node) out[i] = node->persist_seek;
    }
}

static bool write_all(void) {
    if (db_dir[0] == '\0') return false;
    mkdir(db_dir, 0755);

    int32_t nslot = ent_count > 0 ? ent_count : 1;
    int32_t * title_seek = calloc((size_t) nslot, sizeof(int32_t));
    int32_t * path_seek = calloc((size_t) nslot, sizeof(int32_t));
    int32_t * artist_seek_snap = calloc((size_t) nslot, sizeof(int32_t));
    int32_t * album_seek_snap = calloc((size_t) nslot, sizeof(int32_t));
    int32_t * genre_seek_snap = calloc((size_t) nslot, sizeof(int32_t));
    int32_t * aa_seek_snap = calloc((size_t) nslot, sizeof(int32_t));
    if (!title_seek || !path_seek || !artist_seek_snap || !album_seek_snap || !genre_seek_snap || !aa_seek_snap) {
        free(title_seek);
        free(path_seek);
        free(artist_seek_snap);
        free(album_seek_snap);
        free(genre_seek_snap);
        free(aa_seek_snap);
        return false;
    }

    int32_t new_gen = disk_gen > 0 ? disk_gen + 1 : 1;
    if (new_gen <= 0) new_gen = 1;

    /* intern_t.persist_seek is overwritten by each unique tag file (artist
     * and albumartist often intern to the same node when TPE2==TPE1).
     * Snapshot immediately after each file so index seeks stay in that file. */
    bool ok = true;
    if (ok) ok = write_tag_new(tag_artist, new_gen, NULL, NULL);
    if (ok) snapshot_unique_seeks(tag_artist, artist_seek_snap);
    if (ok) ok = write_tag_new(tag_album, new_gen, NULL, NULL);
    if (ok) snapshot_unique_seeks(tag_album, album_seek_snap);
    if (ok) ok = write_tag_new(tag_genre, new_gen, NULL, NULL);
    if (ok) snapshot_unique_seeks(tag_genre, genre_seek_snap);
    if (ok) ok = write_tag_new(tag_albumartist, new_gen, NULL, NULL);
    if (ok) snapshot_unique_seeks(tag_albumartist, aa_seek_snap);
    if (ok) ok = write_tag_new(tag_composer, new_gen, NULL, NULL);
    if (ok) ok = write_tag_new(tag_comment, new_gen, NULL, NULL);
    if (ok) ok = write_tag_new(tag_grouping, new_gen, NULL, NULL);
    if (ok) ok = write_tag_new(tag_virt_canonicalartist, new_gen, NULL, NULL);
    if (ok) ok = write_tag_new(tag_title, new_gen, title_seek, NULL);
    if (ok) ok = write_tag_new(tag_filename, new_gen, NULL, path_seek);

    char master_name[80], master_path[640];
    master_file_name(master_name, sizeof(master_name), new_gen);
    db_path(master_path, sizeof(master_path), master_name);
    if (ok) {
        struct master_header mh;
        memset(&mh, 0, sizeof(mh));
        mh.tch.magic = TAGCACHE_MAGIC;
        mh.tch.entry_count = ent_count;
        mh.tch.datasize = (int32_t) ((uint32_t) ent_count * (uint32_t) sizeof(struct index_entry));
        mh.serial = master_serial;
        mh.commitid = new_gen;
        mh.dirty = 0;
        int fd = open(master_path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
        if (fd < 0) ok = false;
        else {
            ok = write_fully(fd, &mh, sizeof(mh));
            for (int32_t i = 0; ok && i < ent_count; i++) {
                struct index_entry idx;
                memset(&idx, 0, sizeof(idx));
                idx.flag = ents[i].flag & ~FLAG_RAM_ONLY;
                if (ents[i].flag & FLAG_DELETED) {
                    idx.flag = FLAG_DELETED;
                } else {
                    idx.tag_seek[tag_artist] = artist_seek_snap[i];
                    idx.tag_seek[tag_album] = album_seek_snap[i];
                    idx.tag_seek[tag_genre] = genre_seek_snap[i];
                    idx.tag_seek[tag_albumartist] = aa_seek_snap[i];
                    idx.tag_seek[tag_title] = title_seek[i];
                    idx.tag_seek[tag_filename] = path_seek[i];
                    idx.tag_seek[tag_mtime] = ents[i].mtime;
                    idx.tag_seek[tag_lastoffset] = ents[i].size;
                    idx.tag_seek[tag_commitid] = ents[i].first_seen;
                    idx.tag_seek[tag_playcount] = ents[i].playcount;
                    idx.tag_seek[tag_lastplayed] = ents[i].last_played;
                    idx.tag_seek[tag_rating] = ents[i].rating;
                    idx.tag_seek[tag_composer] = idx.tag_seek[tag_artist];
                    idx.tag_seek[tag_virt_canonicalartist] = idx.tag_seek[tag_artist];
                    idx.tag_seek[tag_grouping] = title_seek[i];
                }
                ok = write_fully(fd, &idx, sizeof(idx));
            }
            if (!close_synced(fd) || !ok) {
                unlink(master_path);
                ok = false;
            }
        }
        if (ok) master_commitid = mh.commitid;
    }

    int32_t previous_gen = 0;
    if (ok) read_gen_pointer(&previous_gen);
    if (ok) ok = fsync_dir(db_dir);
    if (ok) ok = write_gen_pointer(new_gen);

    if (!ok) {
        unlink_generation(new_gen);
        char leftover[640];
        db_path(leftover, sizeof(leftover), "tagcache.gen.tmp");
        unlink(leftover);
    } else {
        disk_gen = new_gen;
        unlink_other_generations(new_gen, previous_gen);
        disk_ready = true;
    }
    free(title_seek);
    free(path_seek);
    free(artist_seek_snap);
    free(album_seek_snap);
    free(genre_seek_snap);
    free(aa_seek_snap);
    return ok;
}

typedef struct {
    int32_t seek;
    const char * ptr;
} seek_str_t;

static const char * lookup_seek(seek_str_t * map, int n, int32_t seek) {
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (map[mid].seek == seek) return map[mid].ptr;
        if (map[mid].seek < seek) lo = mid + 1;
        else hi = mid - 1;
    }
    return NULL;
}

static bool load_tag_strings(int tag, int32_t gen, seek_str_t ** out_map, int * out_n, bool nocase) {
    *out_map = NULL;
    *out_n = 0;
    char name[80], path[640];
    tag_file_name(name, sizeof(name), tag, gen);
    db_path(path, sizeof(path), name);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    struct stat st;
    if (fstat(fd, &st) != 0) {
        close(fd);
        return false;
    }
    struct tagcache_header hdr;
    if (!read_fully(fd, &hdr, sizeof(hdr)) || hdr.magic != TAGCACHE_MAGIC || hdr.entry_count < 0 ||
        hdr.entry_count > TAGCACHE_MAX_ENTRIES) {
        close(fd);
        return false;
    }
    if ((uint64_t) st.st_size < sizeof(hdr)) {
        close(fd);
        return false;
    }
    seek_str_t * map = malloc(sizeof(seek_str_t) * (size_t) (hdr.entry_count > 0 ? hdr.entry_count : 1));
    if (!map) {
        close(fd);
        return false;
    }
    int n = 0;
    int32_t pos = (int32_t) sizeof(hdr);
    char buf[TAGCACHE_PATH_MAX];
    bool ok = true;
    for (int32_t i = 0; i < hdr.entry_count; i++) {
        struct tagfile_entry te;
        if (!read_fully(fd, &te, sizeof(te))) {
            ok = false;
            break;
        }
        if (te.tag_length <= 0 || te.tag_length > (int32_t) sizeof(buf)) {
            ok = false;
            break;
        }
        if (!read_fully(fd, buf, (size_t) te.tag_length)) {
            ok = false;
            break;
        }
        buf[te.tag_length - 1] = '\0';
        map[n].seek = pos;
        map[n].ptr = intern_len(buf, nocase);
        n++;
        pos += (int32_t) sizeof(te) + te.tag_length;
    }
    close(fd);
    int32_t parsed = pos - (int32_t) sizeof(hdr);
    if (ok && hdr.datasize > 0 && parsed != hdr.datasize) ok = false;
    if (ok && hdr.datasize > 0 && (uint64_t) st.st_size < (uint64_t) sizeof(hdr) + (uint64_t) hdr.datasize) ok = false;
    if (!ok) {
        free(map);
        return false;
    }
    *out_map = map;
    *out_n = n;
    return true;
}

static bool map_tag_file(int tag, int32_t gen, const char ** out_map, size_t * out_len) {
    char name[80], path[640];
    tag_file_name(name, sizeof(name), tag, gen);
    db_path(path, sizeof(path), name);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < (off_t) sizeof(struct tagcache_header)) {
        close(fd);
        return false;
    }
    void * m = mmap(NULL, (size_t) st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (m == MAP_FAILED) return false;
    struct tagcache_header hdr;
    memcpy(&hdr, m, sizeof(hdr));
    if (hdr.magic != TAGCACHE_MAGIC) {
        munmap(m, (size_t) st.st_size);
        return false;
    }
    *out_map = (const char *) m;
    *out_len = (size_t) st.st_size;
    return true;
}

static bool compact_scan_filename(int32_t count) {
    if (!map_filename || map_filename_len < sizeof(struct tagcache_header)) return false;
    struct tagcache_header hdr;
    memcpy(&hdr, map_filename, sizeof(hdr));
    if (hdr.entry_count < 0 || hdr.entry_count > TAGCACHE_MAX_ENTRIES) return false;
    int32_t pos = (int32_t) sizeof(hdr);
    for (int32_t i = 0; i < hdr.entry_count; i++) {
        if ((size_t) pos + sizeof(struct tagfile_entry) > map_filename_len) return false;
        struct tagfile_entry te;
        memcpy(&te, map_filename + pos, sizeof(te));
        if (te.tag_length <= 1 || te.tag_length > TAGCACHE_PATH_MAX) return false;
        if ((size_t) pos + sizeof(te) + (size_t) te.tag_length > map_filename_len) return false;
        if (te.idx_id < 0 || te.idx_id >= count) return false;
        entry_t * e = &ents[te.idx_id];
        if (e->path_seek != pos) return false;
        e->path_h = fnv1a(map_filename + pos + (int32_t) sizeof(te), (size_t) te.tag_length - 1);
        pos += (int32_t) sizeof(te) + te.tag_length;
    }
    for (int32_t i = 0; i < count; i++) {
        if (ents[i].flag & FLAG_DELETED) continue;
        if (!ents[i].path_h) return false;
    }
    return true;
}

static bool compact_scan_title_order(int32_t count) {
    if (!map_title || map_title_len < sizeof(struct tagcache_header)) return false;
    struct tagcache_header hdr;
    memcpy(&hdr, map_title, sizeof(hdr));
    if (hdr.entry_count < 0 || hdr.entry_count > TAGCACHE_MAX_ENTRIES) return false;
    free(loaded_title_order);
    loaded_title_order = NULL;
    loaded_title_n = 0;
    if (hdr.entry_count == 0) return true;
    int32_t * order = malloc(sizeof(int32_t) * (size_t) hdr.entry_count);
    if (!order) return false;
    int32_t pos = (int32_t) sizeof(hdr);
    int32_t n = 0;
    for (int32_t i = 0; i < hdr.entry_count; i++) {
        if ((size_t) pos + sizeof(struct tagfile_entry) > map_title_len) {
            free(order);
            return false;
        }
        struct tagfile_entry te;
        memcpy(&te, map_title + pos, sizeof(te));
        /* Empty titles are stored as a single NUL (tag_length == 1). */
        if (te.tag_length <= 0 || te.tag_length > TAGCACHE_PATH_MAX) {
            free(order);
            return false;
        }
        if ((size_t) pos + sizeof(te) + (size_t) te.tag_length > map_title_len) {
            free(order);
            return false;
        }
        if (te.idx_id < 0 || te.idx_id >= count) {
            free(order);
            return false;
        }
        if (ents[te.idx_id].title_seek != pos) {
            free(order);
            return false;
        }
        order[n++] = te.idx_id;
        pos += (int32_t) sizeof(te) + te.tag_length;
    }
    loaded_title_order = order;
    loaded_title_n = n;
    return true;
}

static int collect_load_generations(int32_t * out, int max) {
    int n = 0;
    int32_t pointed = 0;
    bool have_pointer = read_gen_pointer(&pointed) && pointed > 0;
    if (have_pointer && n < max) out[n++] = pointed;

    /* tagcache.gen is the transaction's commit record.  A crash after some
     * database_*.tcd.gN files were created but before that pointer was
     * durably renamed must not make N visible on the next boot.  In
     * particular, accepting every generation found here used to expose a
     * half-created library through the Playlists screen's database-backed
     * Favorites/Most Played/Recently Added rows.
     *
     * With a valid pointer, generations below it are safe recovery
     * candidates: write_all() keeps only the previously pointed generation
     * alongside the new one.  Generations above it are uncommitted orphans
     * and are deliberately ignored.  With no pointer, no versioned
     * generation has proof of commit, so only the legacy unversioned cache
     * considered below may be loaded. */
    DIR * d = have_pointer ? opendir(db_dir) : NULL;
    if (d) {
        struct dirent * de;
        while ((de = readdir(d)) != NULL && n < max) {
            int g = 0;
            if (sscanf(de->d_name, "database_idx.tcd.g%d", &g) != 1 || g <= 0) continue;
            if (g > pointed) continue;
            bool dup = false;
            for (int i = 0; i < n; i++) {
                if (out[i] == g) {
                    dup = true;
                    break;
                }
            }
            if (!dup) out[n++] = g;
        }
        closedir(d);
    }

    int start = (n > 0 && have_pointer && out[0] == pointed) ? 1 : 0;
    for (int i = start + 1; i < n; i++) {
        int32_t v = out[i];
        int j = i;
        while (j > start && out[j - 1] < v) {
            out[j] = out[j - 1];
            j--;
        }
        out[j] = v;
    }

    char unversioned[80];
    master_file_name(unversioned, sizeof(unversioned), 0);
    char unversioned_path[640];
    db_path(unversioned_path, sizeof(unversioned_path), unversioned);
    if (n < max && access(unversioned_path, F_OK) == 0) {
        bool dup = false;
        for (int i = 0; i < n; i++) {
            if (out[i] == 0) {
                dup = true;
                break;
            }
        }
        if (!dup) out[n++] = 0;
    }
    return n;
}

static bool load_generation(int32_t gen) {
    char master_name[80], master_path[640];
    master_file_name(master_name, sizeof(master_name), gen);
    db_path(master_path, sizeof(master_path), master_name);
    intern_path_title = true;
    int fd = open(master_path, O_RDONLY);
    if (fd < 0) return false;

    struct stat st;
    if (fstat(fd, &st) != 0) {
        close(fd);
        return false;
    }
    struct master_header mh;
    if (!read_fully(fd, &mh, sizeof(mh)) || mh.tch.magic != TAGCACHE_MAGIC || mh.dirty) {
        close(fd);
        return false;
    }
    int32_t count = mh.tch.entry_count;
    if (count < 0 || count > TAGCACHE_MAX_ENTRIES) {
        close(fd);
        return false;
    }
    uint64_t need = (uint64_t) sizeof(mh) + (uint64_t) count * (uint64_t) sizeof(struct index_entry);
    if ((uint64_t) st.st_size < need) {
        close(fd);
        return false;
    }
    if (mh.tch.datasize > 0 &&
        (uint64_t) mh.tch.datasize != (uint64_t) count * (uint64_t) sizeof(struct index_entry)) {
        close(fd);
        return false;
    }
    if (!ensure_cap(count)) {
        close(fd);
        return false;
    }

    int32_t * path_seek = NULL;
    int32_t * title_seek = NULL;
    int32_t * artist_seek = NULL;
    int32_t * album_seek = NULL;
    int32_t * aa_seek = NULL;
    int32_t * genre_seek = NULL;
    if (count > 0) {
        path_seek = malloc(sizeof(int32_t) * (size_t) count);
        title_seek = malloc(sizeof(int32_t) * (size_t) count);
        artist_seek = malloc(sizeof(int32_t) * (size_t) count);
        album_seek = malloc(sizeof(int32_t) * (size_t) count);
        aa_seek = malloc(sizeof(int32_t) * (size_t) count);
        genre_seek = malloc(sizeof(int32_t) * (size_t) count);
        if (!path_seek || !title_seek || !artist_seek || !album_seek || !aa_seek || !genre_seek) {
            free(path_seek);
            free(title_seek);
            free(artist_seek);
            free(album_seek);
            free(aa_seek);
            free(genre_seek);
            close(fd);
            return false;
        }
    }

    for (int32_t i = 0; i < count; i++) {
        struct index_entry idx;
        if (!read_fully(fd, &idx, sizeof(idx))) {
            free(path_seek);
            free(title_seek);
            free(artist_seek);
            free(album_seek);
            free(aa_seek);
            free(genre_seek);
            close(fd);
            return false;
        }
        entry_t * e = &ents[i];
        memset(e, 0, sizeof(*e));
        e->flag = idx.flag & ~FLAG_RAM_ONLY;
        e->mtime = idx.tag_seek[tag_mtime];
        e->size = idx.tag_seek[tag_lastoffset];
        e->first_seen = idx.tag_seek[tag_commitid];
        e->playcount = idx.tag_seek[tag_playcount];
        e->last_played = idx.tag_seek[tag_lastplayed];
        e->rating = idx.tag_seek[tag_rating];
        e->path_seek = idx.tag_seek[tag_filename];
        e->title_seek = idx.tag_seek[tag_title];
        path_seek[i] = e->path_seek;
        title_seek[i] = e->title_seek;
        artist_seek[i] = idx.tag_seek[tag_artist];
        album_seek[i] = idx.tag_seek[tag_album];
        aa_seek[i] = idx.tag_seek[tag_albumartist];
        genre_seek[i] = idx.tag_seek[tag_genre];
    }
    close(fd);

    master_serial = mh.serial;
    master_commitid = mh.commitid;
    ent_count = count;
    intern_path_title = choose_intern_strings(count);
    if (!intern_path_title)
        fprintf(stderr, "tagcache: compact mode n=%d (title/path mmap, unique tags interned)\n", (int) count);

    seek_str_t * maps[6] = { 0 };
    int mapn[6] = { 0 };
    bool strings_ok;
    if (intern_path_title) {
        strings_ok = load_tag_strings(tag_filename, gen, &maps[0], &mapn[0], false) &&
                     load_tag_strings(tag_title, gen, &maps[1], &mapn[1], true) &&
                     load_tag_strings(tag_artist, gen, &maps[2], &mapn[2], true) &&
                     load_tag_strings(tag_album, gen, &maps[3], &mapn[3], true) &&
                     load_tag_strings(tag_albumartist, gen, &maps[4], &mapn[4], true) &&
                     load_tag_strings(tag_genre, gen, &maps[5], &mapn[5], true);
    } else {
        strings_ok = map_tag_file(tag_filename, gen, &map_filename, &map_filename_len) &&
                     map_tag_file(tag_title, gen, &map_title, &map_title_len) &&
                     compact_scan_filename(count) && compact_scan_title_order(count) &&
                     load_tag_strings(tag_artist, gen, &maps[2], &mapn[2], true) &&
                     load_tag_strings(tag_album, gen, &maps[3], &mapn[3], true) &&
                     load_tag_strings(tag_albumartist, gen, &maps[4], &mapn[4], true) &&
                     load_tag_strings(tag_genre, gen, &maps[5], &mapn[5], true);
    }
    if (!strings_ok) {
        for (int t = 0; t < 6; t++) free(maps[t]);
        free(path_seek);
        free(title_seek);
        free(artist_seek);
        free(album_seek);
        free(aa_seek);
        free(genre_seek);
        unmap_string_files();
        free(loaded_title_order);
        loaded_title_order = NULL;
        loaded_title_n = 0;
        return false;
    }

    bool rows_ok = true;
    for (int32_t i = 0; rows_ok && i < count; i++) {
        entry_t * e = &ents[i];
        if (e->flag & FLAG_DELETED) {
            e->path = intern_path_title ? intern_path("") : NULL;
            e->title = intern_path_title ? intern_tag("") : NULL;
            e->artist = intern_tag("");
            e->album = intern_tag("");
            e->album_artist = intern_tag("");
            e->genre = intern_tag("");
            continue;
        }
        const char * artist = lookup_seek(maps[2], mapn[2], artist_seek[i]);
        const char * album = lookup_seek(maps[3], mapn[3], album_seek[i]);
        const char * album_artist = lookup_seek(maps[4], mapn[4], aa_seek[i]);
        const char * genre = lookup_seek(maps[5], mapn[5], genre_seek[i]);
        /* Older commits stored intern persist_seek from whichever unique
         * tag file last overwrote the shared intern node (albumartist often
         * equals artist). Missing unique-tag seeks must not fail the whole
         * open -- fall back so a good generation still loads. */
        if (!artist) {
            rows_ok = false;
            break;
        }
        /* Unique-tag seek misses must not fail the whole open, but they
         * also must not look like real empty tags on the next rescan:
         * mtime/size cache hits would persist intern("") as the album. */
        bool incomplete = false;
        if (!album) {
            album = "";
            incomplete = true;
        }
        if (!album_artist) {
            album_artist = artist;
            incomplete = true;
        }
        if (!genre) {
            genre = "";
            incomplete = true;
        }
        e->artist = intern_tag(artist);
        e->album = intern_tag(album);
        e->album_artist = intern_tag(album_artist);
        e->genre = intern_tag(genre);
        if (incomplete) e->flag |= FLAG_TAGS_INCOMPLETE;
        if (intern_path_title) {
            const char * path = lookup_seek(maps[0], mapn[0], path_seek[i]);
            const char * title = lookup_seek(maps[1], mapn[1], title_seek[i]);
            if (!path || !path[0] || !title) {
                rows_ok = false;
                break;
            }
            e->path = intern_path(path);
            e->title = intern_tag(title);
            e->path_h = fnv1a(path, strlen(path));
        } else {
            e->path = NULL;
            e->title = NULL;
            if (!mapped_string(map_filename, map_filename_len, e->path_seek) ||
                !mapped_string(map_title, map_title_len, e->title_seek)) {
                rows_ok = false;
                break;
            }
        }
    }
    for (int t = 0; t < 6; t++) free(maps[t]);
    free(path_seek);
    free(title_seek);
    free(artist_seek);
    free(album_seek);
    free(aa_seek);
    free(genre_seek);

    if (!rows_ok) {
        unmap_string_files();
        free(loaded_title_order);
        loaded_title_order = NULL;
        loaded_title_n = 0;
        return false;
    }
    if (!rebuild_indexes()) {
        unmap_string_files();
        free(loaded_title_order);
        loaded_title_order = NULL;
        loaded_title_n = 0;
        return false;
    }
    disk_gen = gen;
    disk_ready = true;
    return true;
}

static bool load_all(void) {
    int32_t gens[48];
    int n = collect_load_generations(gens, 48);
    if (n == 0) {
        intern_path_title = choose_intern_strings(0);
        return true;
    }

    int32_t pointed = 0;
    bool have_ptr = read_gen_pointer(&pointed);
    for (int i = 0; i < n; i++) {
        intern_path_title = true;
        if (load_generation(gens[i])) {
            if (gens[i] > 0 && (!have_ptr || pointed != gens[i])) {
                fprintf(stderr, "tagcache: recovered generation %d (%s)\n", gens[i],
                        have_ptr ? "tagcache.gen stale" : "tagcache.gen missing");
                write_gen_pointer(gens[i]);
            }
            return true;
        }
        arena_reset();
        intern_path_title = true;
        unmap_string_files();
        free(loaded_title_order);
        loaded_title_order = NULL;
        loaded_title_n = 0;
        ent_count = 0;
        disk_ready = false;
        disk_gen = 0;
    }
    return false;
}

static void persist_numeric(int32_t idx) {
    if (!disk_ready || db_dir[0] == '\0' || idx < 0 || idx >= ent_count) return;
    char master_name[80], master_path[640];
    master_file_name(master_name, sizeof(master_name), disk_gen);
    db_path(master_path, sizeof(master_path), master_name);
    int fd = open(master_path, O_RDWR);
    if (fd < 0) return;
    struct index_entry ie;
    off_t off = (off_t) sizeof(struct master_header) + (off_t) idx * (off_t) sizeof(ie);
    if (pread(fd, &ie, sizeof(ie), off) != (ssize_t) sizeof(ie)) {
        close(fd);
        return;
    }
    ie.flag = ents[idx].flag & ~FLAG_RAM_ONLY;
    ie.tag_seek[tag_mtime] = ents[idx].mtime;
    ie.tag_seek[tag_lastoffset] = ents[idx].size;
    ie.tag_seek[tag_commitid] = ents[idx].first_seen;
    ie.tag_seek[tag_playcount] = ents[idx].playcount;
    ie.tag_seek[tag_lastplayed] = ents[idx].last_played;
    ie.tag_seek[tag_rating] = ents[idx].rating;
    if (pwrite(fd, &ie, sizeof(ie), off) != (ssize_t) sizeof(ie)) {
        close(fd);
        return;
    }
    fsync(fd);
    close(fd);
}

static bool reload_from_disk(void) {
    char dir[512];
    snprintf(dir, sizeof(dir), "%s", db_dir);
    if (dir[0] == '\0') return false;
    tagcache_close();
    return tagcache_open(dir);
}

bool tagcache_open(const char * dir) {
    tagcache_close();
    if (!dir || !dir[0]) return false;
    snprintf(db_dir, sizeof(db_dir), "%s", dir);
    db_open = true;
    opened_ok = load_all();
    if (!opened_ok) {
        fprintf(stderr, "tagcache: failed to load %s -- presenting an empty library; on-disk files left in place\n",
                db_dir);
        free(ents);
        ents = NULL;
        ent_count = ent_cap = live_count = 0;
        path_hash_clear();
        free_groups();
        free(title_order);
        free(recency_order);
        free(title_rank_of);
        free(recency_rank_of);
        title_order = recency_order = title_rank_of = recency_rank_of = NULL;
        arena_reset();
        unmap_string_files();
        free(loaded_title_order);
        loaded_title_order = NULL;
        loaded_title_n = 0;
        intern_path_title = true;
        disk_ready = false;
        disk_gen = 0;
        /* Still mark open so Settings > Update Music Database can write a
         * new generation. disk_ready stays false so numeric in-place writes
         * cannot touch the generation that failed to load. */
        opened_ok = true;
    }
    return true;
}

void tagcache_close(void) {
    free(ents);
    ents = NULL;
    ent_count = ent_cap = live_count = 0;
    free(title_order);
    free(recency_order);
    free(title_rank_of);
    free(recency_rank_of);
    title_order = recency_order = title_rank_of = recency_rank_of = NULL;
    path_hash_clear();
    free_groups();
    arena_reset();
    unmap_string_files();
    free(loaded_title_order);
    loaded_title_order = NULL;
    loaded_title_n = 0;
    intern_path_title = true;
    db_open = false;
    disk_ready = false;
    opened_ok = false;
    disk_gen = 0;
    db_dir[0] = '\0';
}

bool tagcache_is_open(void) {
    return db_open && opened_ok;
}

void tagcache_begin_update(void) {
    if (!db_open) return;
    for (int32_t i = 0; i < ent_count; i++) ents[i].flag &= ~FLAG_SEEN;
}

bool tagcache_lookup(const char * path, int32_t mtime, int32_t size, tagcache_song_t * out) {
    if (!db_open || !path) return false;
    int32_t idx = path_hash_find(path);
    if (idx < 0 || (ents[idx].flag & FLAG_DELETED)) return false;
    ents[idx].flag |= FLAG_SEEN;
    if (ents[idx].mtime != mtime || ents[idx].size != size) return false;
    /* "" album is not a real ID3 result (scan writes "Unknown Album").
     * Load-fallback and a later commit of that fallback both look like a
     * cache hit unless we force a tag re-read. */
    if ((ents[idx].flag & FLAG_TAGS_INCOMPLETE) || !ents[idx].album || !ents[idx].album[0])
        return false;
    if (out) fill_song(idx, out);
    return true;
}

void tagcache_upsert(const char * path, int32_t mtime, int32_t size, const char * title, const char * artist,
                     const char * album, const char * album_artist, const char * genre) {
    if (!db_open || !path) return;
    int32_t idx = path_hash_find(path);
    if (idx >= 0 && !(ents[idx].flag & FLAG_DELETED)) {
        apply_tags(&ents[idx], path, mtime, size, title, artist, album, album_artist, genre);
        return;
    }
    if (idx >= 0) {
        int32_t first_seen = ents[idx].first_seen;
        int32_t playcount = ents[idx].playcount;
        int32_t last_played = ents[idx].last_played;
        int32_t rating = ents[idx].rating;
        apply_tags(&ents[idx], path, mtime, size, title, artist, album, album_artist, genre);
        ents[idx].first_seen = first_seen;
        ents[idx].playcount = playcount;
        ents[idx].last_played = last_played;
        ents[idx].rating = rating;
        return;
    }
    if (!ensure_cap(ent_count + 1)) {
        fprintf(stderr, "tagcache: at TAGCACHE_MAX_ENTRIES (%d), dropping %s\n", TAGCACHE_MAX_ENTRIES, path);
        return;
    }
    idx = ent_count++;
    memset(&ents[idx], 0, sizeof(ents[idx]));
    apply_tags(&ents[idx], path, mtime, size, title, artist, album, album_artist, genre);
    ents[idx].first_seen = (int32_t) time(NULL);
    path_hash_insert(idx);
}

bool tagcache_end_update(bool prune) {
    if (!db_open) return false;
    if (prune) {
        for (int32_t i = 0; i < ent_count; i++) {
            if (ents[i].flag & FLAG_DELETED) continue;
            if (!(ents[i].flag & FLAG_SEEN)) ents[i].flag |= FLAG_DELETED;
        }
    }
    for (int32_t i = 0; i < ent_count; i++) ents[i].flag &= ~FLAG_SEEN;
    drop_derived_indexes_before_rebuild();
    if (!rebuild_indexes() || !write_all()) {
        reload_from_disk();
        return false;
    }
    /* Compact loads mmap the new generation. Interned loads that outgrew
     * the RAM budget drop title/path intern tables on reload. */
    if (!intern_path_title || !choose_intern_strings(live_count)) {
        if (!reload_from_disk()) return false;
    }
    return true;
}

void tagcache_abort_update(void) {
    if (!db_open) return;
    reload_from_disk();
}

int32_t tagcache_live_count(void) {
    return live_count;
}

int32_t tagcache_slot_count(void) {
    return ent_count;
}

bool tagcache_song_by_id(int32_t id, tagcache_song_t * out) {
    if (!db_open || id < 1 || id > ent_count) return false;
    int32_t idx = id - 1;
    if (ents[idx].flag & FLAG_DELETED) return false;
    if (out) fill_song(idx, out);
    return true;
}

bool tagcache_song_by_path(const char * path, tagcache_song_t * out) {
    if (!db_open) return false;
    int32_t idx = path_hash_find(path);
    if (idx < 0 || (ents[idx].flag & FLAG_DELETED)) return false;
    if (out) fill_song(idx, out);
    return true;
}

bool tagcache_song_at_slot(int32_t slot, tagcache_song_t * out) {
    if (!db_open || slot < 0 || slot >= ent_count) return false;
    if (ents[slot].flag & FLAG_DELETED) return false;
    if (out) fill_song(slot, out);
    return true;
}

bool tagcache_song_at_title_rank(int32_t rank, tagcache_song_t * out) {
    if (!db_open || rank < 0 || rank >= live_count || !title_order) return false;
    if (out) fill_song(title_order[rank], out);
    return true;
}

bool tagcache_song_at_recency_rank(int32_t rank, tagcache_song_t * out) {
    if (!db_open || rank < 0 || rank >= live_count || !recency_order) return false;
    if (out) fill_song(recency_order[rank], out);
    return true;
}

int tagcache_group_count(int kind) {
    if (kind < 0 || kind > 2) return 0;
    return group_n[kind];
}

bool tagcache_group_at(int kind, int index, tagcache_group_t * out) {
    if (kind < 0 || kind > 2 || index < 0 || index >= group_n[kind]) return false;
    if (out) {
        out->name = groups[kind][index].name;
        out->album_artist = groups[kind][index].album_artist ? groups[kind][index].album_artist : "";
        out->song_count = groups[kind][index].song_count;
        out->first_song_id = groups[kind][index].first_song_id;
    }
    return true;
}

static int find_group(int kind, const char * name, const char * album_artist) {
    if (kind < 0 || kind > 2 || !groups[kind]) return -1;
    const char * n = name ? name : "";
    const char * aa = album_artist ? album_artist : "";
    int lo = 0, hi = group_n[kind] - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        int c = ascii_casecmp(groups[kind][mid].name, n);
        if (c == 0 && kind == TAGCACHE_GROUP_ALBUM) c = ascii_casecmp(groups[kind][mid].album_artist, aa);
        if (c == 0) return mid;
        if (c < 0) lo = mid + 1;
        else hi = mid - 1;
    }
    return -1;
}

static int copy_group_page(int kind, int gi, int offset, int32_t * out_ids, int max) {
    if (gi < 0 || !out_ids || max <= 0) return 0;
    const group_t * g = &groups[kind][gi];
    if (!g->songs || offset >= g->song_count) return 0;
    if (offset < 0) offset = 0;
    int w = 0;
    for (int i = offset; i < g->song_count && w < max; i++)
        out_ids[w++] = g->songs[i] + 1;
    return w;
}

int tagcache_artist_song_ids(const char * artist, int offset, int32_t * out_ids, int max) {
    if (!db_open) return 0;
    return copy_group_page(TAGCACHE_GROUP_ARTIST, find_group(TAGCACHE_GROUP_ARTIST, artist, ""), offset, out_ids, max);
}

int tagcache_album_song_ids(const char * album, const char * album_artist, int offset, int32_t * out_ids, int max) {
    if (!db_open) return 0;
    return copy_group_page(TAGCACHE_GROUP_ALBUM, find_group(TAGCACHE_GROUP_ALBUM, album, album_artist), offset, out_ids,
                           max);
}

void tagcache_set_rating(const char * path, int32_t rating) {
    int32_t idx = path_hash_find(path);
    if (idx < 0 || (ents[idx].flag & FLAG_DELETED)) return;
    ents[idx].rating = rating;
    persist_numeric(idx);
}

void tagcache_add_play(const char * path, int32_t now) {
    int32_t idx = path_hash_find(path);
    if (idx < 0 || (ents[idx].flag & FLAG_DELETED)) return;
    if (ents[idx].playcount < INT32_MAX) ents[idx].playcount++;
    ents[idx].last_played = now;
    persist_numeric(idx);
}

void tagcache_overlay_stats(const char * path, int32_t rating, int32_t playcount, int32_t last_played) {
    int32_t idx = path_hash_find(path);
    if (idx < 0 || (ents[idx].flag & FLAG_DELETED)) return;
    ents[idx].rating = rating;
    ents[idx].playcount = playcount;
    ents[idx].last_played = last_played;
}

int32_t tagcache_title_rank_of_path(const char * path) {
    int32_t idx = path_hash_find(path);
    if (idx < 0 || (ents[idx].flag & FLAG_DELETED) || !title_rank_of) return -1;
    return title_rank_of[idx];
}

int32_t tagcache_recency_rank_of_path(const char * path) {
    int32_t idx = path_hash_find(path);
    if (idx < 0 || (ents[idx].flag & FLAG_DELETED) || !recency_rank_of) return -1;
    return recency_rank_of[idx];
}

int32_t tagcache_title_rank_after(const char * after_title, int32_t after_id) {
    if (!after_title || !title_order || live_count <= 0) return 0;
    int lo = 0, hi = (int) live_count;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        int32_t idx = title_order[mid];
        int c = ascii_casecmp(entry_title_str(&ents[idx]), after_title);
        if (c == 0) {
            int32_t id = idx + 1;
            if (id <= after_id) lo = mid + 1;
            else hi = mid;
        } else if (c < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

int tagcache_group_index(int kind, const char * name, const char * album_artist) {
    if (!db_open) return -1;
    return find_group(kind, name, album_artist ? album_artist : "");
}

const char * tagcache_ascii_casestr(const char * hay, const char * needle) {
    return ascii_casestr(hay ? hay : "", needle ? needle : "");
}
