#ifndef TAGCACHE_H
#define TAGCACHE_H

#include <stdbool.h>
#include <stdint.h>

/* POSIX port of Rockbox's tagcache engine (apps/tagcache.c, GPLv2+).
 * Record layout matches Rockbox TCH (master index + per-tag string files).
 * Commits write a complete generation (`database_*.tcd.gN`) and atomically
 * switch `tagcache.gen`; the previous generation stays loadable until that
 * pointer is replaced. Numeric tags live in the master index. Title and
 * path strings are interned only when RAM allows; larger libraries mmap
 * those files instead. This header is the internal storage engine used
 * exclusively by metadata_db.c. All access must be serialized by
 * metadata_db.c under METADATA_DB_GUARD; public player code must use
 * metadata_db.h instead. */

#define TAGCACHE_PATH_MAX 600
#define TAGCACHE_TAG_MAX 128

#define TAGCACHE_GROUP_ARTIST 0
#define TAGCACHE_GROUP_ALBUM_ARTIST 1
#define TAGCACHE_GROUP_ALBUM 2

#define TAGCACHE_AZ_ARTIST 0
#define TAGCACHE_AZ_ALBUM_ARTIST 1
#define TAGCACHE_AZ_ALBUM 2
#define TAGCACHE_AZ_ALL_SONGS 3

typedef struct {
    int32_t id; /* 1-based stable slot id; deleted slots are never reused */
    int32_t mtime;
    int32_t size;
    int32_t first_seen;
    int32_t playcount;
    int32_t last_played;
    int32_t rating; /* 1 = favorite, matching Rockbox's rating tag */
    const char * path;
    const char * title;
    const char * artist;
    const char * album;
    const char * album_artist;
    const char * genre;
} tagcache_song_t;

typedef struct {
    const char * name;
    const char * album_artist; /* album groups only; empty otherwise */
    int song_count;
    int32_t first_song_id;
} tagcache_group_t;

bool tagcache_open(const char * dir);
void tagcache_close(void);
bool tagcache_is_open(void);

void tagcache_begin_update(void);
/* Marks path seen for this pass. Returns true and fills *out when a live
 * row exists whose stored mtime and size both match. */
bool tagcache_lookup(const char * path, int32_t mtime, int32_t size, tagcache_song_t * out);
void tagcache_upsert(const char * path, int32_t mtime, int32_t size, const char * title, const char * artist,
                     const char * album, const char * album_artist, const char * genre);
/* prune unseen rows, rebuild indexes, persist. Returns false if the
 * on-disk write failed -- RAM is reloaded from the last committed files. */
bool tagcache_end_update(bool prune);
/* Discard in-RAM scan mutations and reload the last committed files. */
void tagcache_abort_update(void);

int32_t tagcache_live_count(void);
int32_t tagcache_slot_count(void);
bool tagcache_song_by_id(int32_t id, tagcache_song_t * out);
bool tagcache_song_by_path(const char * path, tagcache_song_t * out);
bool tagcache_song_at_slot(int32_t slot, tagcache_song_t * out);
bool tagcache_song_at_title_rank(int32_t rank, tagcache_song_t * out);
bool tagcache_song_at_recency_rank(int32_t rank, tagcache_song_t * out);

int tagcache_group_count(int kind);
bool tagcache_group_at(int kind, int index, tagcache_group_t * out);
/* Page of 1-based song ids from a pre-sorted group membership list. */
int tagcache_artist_song_ids(const char * artist, int offset, int32_t * out_ids, int max);
int tagcache_album_song_ids(const char * album, const char * album_artist, int offset, int32_t * out_ids, int max);

void tagcache_set_rating(const char * path, int32_t rating);
void tagcache_add_play(const char * path, int32_t now);
/* RAM-only overlay used when migrating sidecar stats onto a just-upserted
 * scan row. Persisted by the following end_update write. */
void tagcache_overlay_stats(const char * path, int32_t rating, int32_t playcount, int32_t last_played);

int32_t tagcache_title_rank_of_path(const char * path);
int32_t tagcache_recency_rank_of_path(const char * path);
/* First title-order rank strictly after (after_title, after_id). */
int32_t tagcache_title_rank_after(const char * after_title, int32_t after_id);
int tagcache_group_index(int kind, const char * name, const char * album_artist);

int tagcache_cmp_ascii(const char * a, const char * b);
const char * tagcache_ascii_casestr(const char * hay, const char * needle);

#endif /* TAGCACHE_H */
