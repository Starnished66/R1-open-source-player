#include "metadata_db.h"

#include "path_cache.h"
#include "remote_state.h"
#include "subsonic_saved_servers.h"
#include "tagcache.h"

#include <limits.h>
#include <stdint.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifdef HOST_BUILD
  #define METADATA_DB_DIR "./.open_hiby_player"
#else
  #define METADATA_DB_DIR "/data/mnt/sd_0/.open_hiby_player"
#endif

static bool db_ready;
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
    const char * path;
    int32_t count;
    int32_t last_played;
} played_hit_t;

static int cmp_hit_played(const void * a, const void * b) {
    const played_hit_t * ha = a;
    const played_hit_t * hb = b;
    if (ha->count != hb->count) return ha->count < hb->count ? 1 : -1;
    if (ha->last_played != hb->last_played) return ha->last_played < hb->last_played ? 1 : -1;
    return 0;
}

static void copy_song(const tagcache_song_t * src, song_row_t * dst) {
    dst->id = src->id;
    snprintf(dst->path, sizeof(dst->path), "%s", src->path);
    snprintf(dst->tags.title, sizeof(dst->tags.title), "%s", src->title);
    snprintf(dst->tags.artist, sizeof(dst->tags.artist), "%s", src->artist);
    snprintf(dst->tags.album, sizeof(dst->tags.album), "%s", src->album);
    snprintf(dst->tags.album_artist, sizeof(dst->tags.album_artist), "%s", src->album_artist);
    snprintf(dst->tags.genre, sizeof(dst->tags.genre), "%s", src->genre);
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

typedef struct pair_node {
    const char * a;
    const char * b;
    struct pair_node * next;
} pair_node_t;

static bool pair_set_add(pair_node_t ** buckets, int bucket_n, const char * a, const char * b) {
    uint32_t h = 2166136261u;
    const char * s = a ? a : "";
    while (*s) {
        h ^= (unsigned char) *s++;
        h *= 16777619u;
    }
    s = b ? b : "";
    while (*s) {
        h ^= (unsigned char) *s++;
        h *= 16777619u;
    }
    int slot = (int) (h & (uint32_t) (bucket_n - 1));
    for (pair_node_t * it = buckets[slot]; it; it = it->next) {
        if (it->a == a && it->b == b) return false;
    }
    pair_node_t * node = malloc(sizeof(*node));
    if (!node) return false;
    node->a = a;
    node->b = b;
    node->next = buckets[slot];
    buckets[slot] = node;
    return true;
}

static bool pair_set_has(pair_node_t ** buckets, int bucket_n, const char * a, const char * b) {
    uint32_t h = 2166136261u;
    const char * s = a ? a : "";
    while (*s) {
        h ^= (unsigned char) *s++;
        h *= 16777619u;
    }
    s = b ? b : "";
    while (*s) {
        h ^= (unsigned char) *s++;
        h *= 16777619u;
    }
    int slot = (int) (h & (uint32_t) (bucket_n - 1));
    for (pair_node_t * it = buckets[slot]; it; it = it->next) {
        if (it->a == a && it->b == b) return true;
    }
    return false;
}

static void pair_set_free(pair_node_t ** buckets, int bucket_n) {
    for (int i = 0; i < bucket_n; i++) {
        pair_node_t * it = buckets[i];
        while (it) {
            pair_node_t * n = it->next;
            free(it);
            it = n;
        }
    }
}

static bool song_matches_filters(const tagcache_song_t * song, const char * query, const char * artist_filter,
                                 const char * album_artist_filter, const char * album_filter) {
    if (query && query[0]) {
        if (!tagcache_ascii_casestr(song->title, query) && !tagcache_ascii_casestr(song->artist, query)) return false;
    }
    if (artist_filter && artist_filter[0] && tagcache_cmp_ascii(song->artist, artist_filter) != 0) return false;
    if (album_artist_filter && album_artist_filter[0] &&
        tagcache_cmp_ascii(song->album_artist, album_artist_filter) != 0)
        return false;
    if (album_filter && album_filter[0] && tagcache_cmp_ascii(song->album, album_filter) != 0) return false;
    return true;
}

void metadata_db_open(void) {
    METADATA_DB_GUARD;
    if (db_ready) return;

#ifndef HOST_BUILD
    if (!music_root_is_mounted()) return;
#endif

    mkdir(METADATA_DB_DIR, 0755);
    if (!tagcache_open(METADATA_DB_DIR)) return;
    db_ready = true;
}

void metadata_db_close(void) {
    METADATA_DB_GUARD;
    tagcache_close();
    db_ready = false;
}

void metadata_db_begin_update(void) {
    METADATA_DB_GUARD;
    if (!db_ready) return;
    tagcache_begin_update();
}

bool metadata_db_get(const char * path, int64_t mtime, int64_t size, cached_tags_t * out) {
    METADATA_DB_GUARD;
    if (!db_ready) return false;
    tagcache_song_t song;
    if (!tagcache_lookup(path, clamp_i32(mtime), clamp_i32(size), &song)) return false;
    snprintf(out->title, sizeof(out->title), "%s", song.title);
    snprintf(out->artist, sizeof(out->artist), "%s", song.artist);
    snprintf(out->album, sizeof(out->album), "%s", song.album);
    snprintf(out->album_artist, sizeof(out->album_artist), "%s", song.album_artist);
    snprintf(out->genre, sizeof(out->genre), "%s", song.genre);
    return true;
}

void metadata_db_put(const char * path, int64_t mtime, int64_t size, const cached_tags_t * tags) {
    METADATA_DB_GUARD;
    if (!db_ready || !tags) return;
    tagcache_upsert(path, clamp_i32(mtime), clamp_i32(size), tags->title, tags->artist, tags->album, tags->album_artist,
                    tags->genre);
    int32_t rating = 0, playcount = 0, last_played = 0;
    if (remote_state_take(path, &rating, &playcount, &last_played))
        tagcache_overlay_stats(path, rating, playcount, last_played);
}

bool metadata_db_end_update(void) {
    METADATA_DB_GUARD;
    if (!db_ready) return false;
    return tagcache_end_update(true);
}

void metadata_db_abort_update(void) {
    METADATA_DB_GUARD;
    if (!db_ready) return;
    tagcache_abort_update();
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

int metadata_db_get_songs_page(const char * after_title, int64_t after_id, int max_rows, song_row_t * out_rows) {
    METADATA_DB_GUARD;
    if (!db_ready || max_rows <= 0) return 0;
    int32_t live = tagcache_live_count();
    int32_t start = 0;
    if (after_title) start = tagcache_title_rank_after(after_title, (int32_t) after_id);
    int w = 0;
    for (int32_t i = start; i < live && w < max_rows; i++) {
        tagcache_song_t song;
        if (!tagcache_song_at_title_rank(i, &song)) continue;
        copy_song(&song, &out_rows[w++]);
    }
    return w;
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

    bool seen[26] = { false };
    int position = 0;
    int gkind = az_kind_to_group(kind);
    if (gkind >= 0) {
        int n = tagcache_group_count(gkind);
        for (int i = 0; i < n; i++) {
            tagcache_group_t g;
            if (!tagcache_group_at(gkind, i, &g)) continue;
            const char * text = g.name ? g.name : "";
            if (text[0]) {
                unsigned char c = (unsigned char) text[0];
                if (c >= 'a' && c <= 'z') c = (unsigned char) (c - ('a' - 'A'));
                if (c >= 'A' && c <= 'Z' && !seen[c - 'A']) {
                    seen[c - 'A'] = true;
                    out_table[c - 'A'] = position;
                }
            }
            position++;
        }
    } else {
        int32_t live = tagcache_live_count();
        for (int32_t i = 0; i < live; i++) {
            tagcache_song_t song;
            if (!tagcache_song_at_title_rank(i, &song)) continue;
            const char * text = song.title ? song.title : "";
            if (text[0]) {
                unsigned char c = (unsigned char) text[0];
                if (c >= 'a' && c <= 'z') c = (unsigned char) (c - ('a' - 'A'));
                if (c >= 'A' && c <= 'Z' && !seen[c - 'A']) {
                    seen[c - 'A'] = true;
                    out_table[c - 'A'] = position;
                }
            }
            position++;
        }
    }
    for (int i = 24; i >= 0; i--) {
        if (out_table[i] == -1) out_table[i] = out_table[i + 1];
    }
    out_table[26] = out_table[0] > 0 ? 0 : -1;
}

int metadata_db_search_names(metadata_db_az_kind_t kind, const char * needle, int max_rows,
                              metadata_db_search_hit_t * out_hits) {
    METADATA_DB_GUARD;
    if (!db_ready || max_rows <= 0 || !needle || !needle[0]) return 0;
    int w = 0;
    int gkind = az_kind_to_group(kind);
    if (gkind >= 0) {
        int n = tagcache_group_count(gkind);
        for (int i = 0; i < n && w < max_rows; i++) {
            tagcache_group_t g;
            if (!tagcache_group_at(gkind, i, &g)) continue;
            if (!tagcache_ascii_casestr(g.name, needle)) continue;
            snprintf(out_hits[w].label, sizeof(out_hits[w].label), "%s", g.name);
            out_hits[w].offset = i;
            w++;
        }
        return w;
    }
    int32_t live = tagcache_live_count();
    for (int32_t i = 0; i < live && w < max_rows; i++) {
        tagcache_song_t song;
        if (!tagcache_song_at_title_rank(i, &song)) continue;
        if (!tagcache_ascii_casestr(song.title, needle)) continue;
        song_row_t row = { 0 };
        copy_song(&song, &row);
        metadata_db_song_display_title(&row, out_hits[w].label, sizeof(out_hits[w].label));
        out_hits[w].offset = (int) i;
        w++;
    }
    return w;
}

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
    if (!db_ready || max_rows <= 0 || !query_text || !query_text[0]) return 0;
    int32_t live = tagcache_live_count();
    int w = 0;
    for (int32_t i = 0; i < live && w < max_rows; i++) {
        tagcache_song_t song;
        if (!tagcache_song_at_title_rank(i, &song)) continue;
        if (!tagcache_ascii_casestr(song.title, query_text) && !tagcache_ascii_casestr(song.artist, query_text)) continue;
        copy_song(&song, &out_rows[w++]);
    }
    return w;
}

int64_t metadata_db_count_songs_filtered(const char * query, const char * artist_filter,
                                          const char * album_artist_filter, const char * album_filter) {
    METADATA_DB_GUARD;
    if (!db_ready) return 0;
    int32_t live = tagcache_live_count();
    int64_t n = 0;
    for (int32_t i = 0; i < live; i++) {
        tagcache_song_t song;
        if (!tagcache_song_at_title_rank(i, &song)) continue;
        if (song_matches_filters(&song, query, artist_filter, album_artist_filter, album_filter)) n++;
    }
    return n;
}

int metadata_db_get_songs_filtered_page(const char * query, const char * artist_filter,
                                         const char * album_artist_filter, const char * album_filter, int offset,
                                         int max_rows, song_row_t * out_rows) {
    METADATA_DB_GUARD;
    if (!db_ready || max_rows <= 0) return 0;
    if (offset < 0) offset = 0;
    int32_t live = tagcache_live_count();
    bool unfiltered = !(query && query[0]) && !(artist_filter && artist_filter[0]) &&
                      !(album_artist_filter && album_artist_filter[0]) && !(album_filter && album_filter[0]);
    int w = 0;
    if (unfiltered) {
        for (int32_t i = offset; i < live && w < max_rows; i++) {
            tagcache_song_t song;
            if (!tagcache_song_at_title_rank(i, &song)) continue;
            copy_song(&song, &out_rows[w++]);
        }
        return w;
    }
    int skipped = 0;
    for (int32_t i = 0; i < live && w < max_rows; i++) {
        tagcache_song_t song;
        if (!tagcache_song_at_title_rank(i, &song)) continue;
        if (!song_matches_filters(&song, query, artist_filter, album_artist_filter, album_filter)) continue;
        if (skipped < offset) {
            skipped++;
            continue;
        }
        copy_song(&song, &out_rows[w++]);
    }
    return w;
}

int metadata_db_get_albums_page_filtered(const char * artist_or_album_artist_filter, int offset, int max_rows,
                                          group_row_t * out_rows) {
    METADATA_DB_GUARD;
    if (!db_ready || max_rows <= 0) return 0;
    if (offset < 0) offset = 0;
    bool have_filter = artist_or_album_artist_filter && artist_or_album_artist_filter[0];
    const int buckets = 4096;
    pair_node_t ** set = NULL;
    if (have_filter) {
        set = calloc((size_t) buckets, sizeof(*set));
        if (!set) return 0;
        int32_t slots = tagcache_slot_count();
        for (int32_t s = 0; s < slots; s++) {
            tagcache_song_t song;
            if (!tagcache_song_at_slot(s, &song)) continue;
            if (tagcache_cmp_ascii(song.artist, artist_or_album_artist_filter) != 0 &&
                tagcache_cmp_ascii(song.album_artist, artist_or_album_artist_filter) != 0)
                continue;
            pair_set_add(set, buckets, song.album, song.album_artist);
        }
    }
    int n = tagcache_group_count(TAGCACHE_GROUP_ALBUM);
    int skipped = 0;
    int w = 0;
    for (int i = 0; i < n && w < max_rows; i++) {
        tagcache_group_t g;
        if (!tagcache_group_at(TAGCACHE_GROUP_ALBUM, i, &g)) continue;
        if (have_filter && !pair_set_has(set, buckets, g.name, g.album_artist)) continue;
        if (skipped < offset) {
            skipped++;
            continue;
        }
        copy_group(&g, &out_rows[w++]);
    }
    if (set) {
        pair_set_free(set, buckets);
        free(set);
    }
    return w;
}

int64_t metadata_db_count_albums_for_group(metadata_db_group_kind_t kind, const char * name) {
    METADATA_DB_GUARD;
    if (!db_ready || !name || (kind != METADATA_DB_GROUP_ARTIST && kind != METADATA_DB_GROUP_ALBUM_ARTIST)) return 0;
    const int buckets = 4096;
    pair_node_t ** set = calloc((size_t) buckets, sizeof(*set));
    if (!set) return 0;
    int32_t slots = tagcache_slot_count();
    for (int32_t s = 0; s < slots; s++) {
        tagcache_song_t song;
        if (!tagcache_song_at_slot(s, &song)) continue;
        const char * col = kind == METADATA_DB_GROUP_ARTIST ? song.artist : song.album_artist;
        if (tagcache_cmp_ascii(col, name) != 0) continue;
        pair_set_add(set, buckets, song.album, song.album_artist);
    }
    int n = tagcache_group_count(TAGCACHE_GROUP_ALBUM);
    int64_t count = 0;
    for (int i = 0; i < n; i++) {
        tagcache_group_t g;
        if (!tagcache_group_at(TAGCACHE_GROUP_ALBUM, i, &g)) continue;
        if (pair_set_has(set, buckets, g.name, g.album_artist)) count++;
    }
    pair_set_free(set, buckets);
    free(set);
    return count;
}

int metadata_db_get_albums_for_group(metadata_db_group_kind_t kind, const char * name, int offset, int max_rows,
                                      group_row_t * out_rows) {
    METADATA_DB_GUARD;
    if (!db_ready || !name || max_rows <= 0 ||
        (kind != METADATA_DB_GROUP_ARTIST && kind != METADATA_DB_GROUP_ALBUM_ARTIST))
        return 0;
    if (offset < 0) offset = 0;
    const int buckets = 4096;
    pair_node_t ** set = calloc((size_t) buckets, sizeof(*set));
    if (!set) return 0;
    int32_t slots = tagcache_slot_count();
    for (int32_t s = 0; s < slots; s++) {
        tagcache_song_t song;
        if (!tagcache_song_at_slot(s, &song)) continue;
        const char * col = kind == METADATA_DB_GROUP_ARTIST ? song.artist : song.album_artist;
        if (tagcache_cmp_ascii(col, name) != 0) continue;
        pair_set_add(set, buckets, song.album, song.album_artist);
    }
    int n = tagcache_group_count(TAGCACHE_GROUP_ALBUM);
    int skipped = 0;
    int w = 0;
    for (int i = 0; i < n && w < max_rows; i++) {
        tagcache_group_t g;
        if (!tagcache_group_at(TAGCACHE_GROUP_ALBUM, i, &g)) continue;
        if (!pair_set_has(set, buckets, g.name, g.album_artist)) continue;
        if (skipped < offset) {
            skipped++;
            continue;
        }
        copy_group(&g, &out_rows[w++]);
    }
    pair_set_free(set, buckets);
    free(set);
    return w;
}

void metadata_db_load_all(char *** out_paths, cached_tags_t ** out_tags, int * out_count) {
    METADATA_DB_GUARD;
    *out_paths = NULL;
    *out_tags = NULL;
    *out_count = 0;
    if (!db_ready) return;
    int32_t live = tagcache_live_count();
    if (live <= 0) return;
    char ** paths = calloc((size_t) live, sizeof(*paths));
    cached_tags_t * tags = malloc(sizeof(*tags) * (size_t) live);
    if (!paths || !tags) {
        free(paths);
        free(tags);
        return;
    }
    int w = 0;
    for (int32_t i = 0; i < live; i++) {
        tagcache_song_t song;
        if (!tagcache_song_at_title_rank(i, &song)) continue;
        paths[w] = strdup(song.path);
        if (!paths[w]) break;
        snprintf(tags[w].title, sizeof(tags[w].title), "%s", song.title);
        snprintf(tags[w].artist, sizeof(tags[w].artist), "%s", song.artist);
        snprintf(tags[w].album, sizeof(tags[w].album), "%s", song.album);
        snprintf(tags[w].album_artist, sizeof(tags[w].album_artist), "%s", song.album_artist);
        snprintf(tags[w].genre, sizeof(tags[w].genre), "%s", song.genre);
        w++;
    }
    if (w != live) {
        for (int j = 0; j < w; j++) free(paths[j]);
        free(paths);
        free(tags);
        return;
    }
    *out_paths = paths;
    *out_tags = tags;
    *out_count = w;
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
    for (int32_t i = 0; i < slots; i++) {
        tagcache_song_t song;
        if (!tagcache_song_at_slot(i, &song)) continue;
        if (song.rating == 0) continue;
        if (n >= cap) {
            int next = cap ? cap * 2 : 16;
            char ** p = realloc(paths, sizeof(*p) * (size_t) next);
            if (!p) break;
            paths = p;
            cap = next;
        }
        paths[n] = strdup(song.path);
        if (!paths[n]) break;
        n++;
    }
    if (n == 0) {
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
    for (int32_t i = 0; i < slots; i++) {
        tagcache_song_t song;
        if (!tagcache_song_at_slot(i, &song)) continue;
        if (song.playcount <= 0) continue;
        if (n >= cap) {
            int next = cap ? cap * 2 : 16;
            played_hit_t * h = realloc(hits, sizeof(*h) * (size_t) next);
            if (!h) break;
            hits = h;
            cap = next;
        }
        hits[n].path = song.path;
        hits[n].count = song.playcount;
        hits[n].last_played = song.last_played;
        n++;
    }
    qsort(hits, (size_t) n, sizeof(*hits), cmp_hit_played);
    if (n > limit) n = limit;
    if (n <= 0) {
        free(hits);
        return;
    }
    char ** paths = malloc(sizeof(*paths) * (size_t) n);
    int w = 0;
    for (int i = 0; i < n; i++) {
        paths[w] = strdup(hits[i].path);
        if (!paths[w]) break;
        w++;
    }
    free(hits);
    if (w == 0) {
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
    int w = 0;
    for (int32_t i = 0; i < live && w < n; i++) {
        tagcache_song_t song;
        if (!tagcache_song_at_recency_rank(i, &song)) continue;
        paths[w] = strdup(song.path);
        if (!paths[w]) break;
        w++;
    }
    if (w == 0) {
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

void metadata_db_playlist_replace_all(char * const * paths, int count) {
    path_cache_replace(PATH_CACHE_PLAYLISTS, paths, count);
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
