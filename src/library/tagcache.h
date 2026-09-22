#ifndef TAGCACHE_H
#define TAGCACHE_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

/* POSIX port of Rockbox's tagcache engine (apps/tagcache.c, GPLv2+).
 * Master records and tag files use the TCH layout; indexed generations carry
 * a versioned master magic and generation-local browse sidecars.
 * Ordinary commits write a complete generation (`database_*.tcd.gN`) and
 * atomically switch `tagcache.gen`; an explicit recovery rebuild leaves the
 * pointer unchanged until a later ordinary update. The previous generation
 * stays loadable until that pointer is replaced. Numeric tags live in the
 * master index. Records and strings are read into caller-owned result storage.
 * Scan mutations use private disk files. This header is the storage engine used
 * exclusively by metadata_db.c. Query access is serialized by METADATA_DB_GUARD;
 * private generation construction may release that lock under the update guard.
 * Public player code must use
 * metadata_db.h instead. */

#define TAGCACHE_PATH_MAX 600
#define TAGCACHE_TAG_MAX 128
/* Most artists a single ARTIST tag can be filed under. Splitting stops at the
 * cap rather than growing, so a pathological tag cannot blow up the index
 * build. */
#define TAGCACHE_ARTIST_SPLIT_MAX 8

#define TAGCACHE_GROUP_ARTIST 0
#define TAGCACHE_GROUP_ALBUM_ARTIST 1
#define TAGCACHE_GROUP_ALBUM 2

#define TAGCACHE_AZ_ARTIST 0
#define TAGCACHE_AZ_ALBUM_ARTIST 1
#define TAGCACHE_AZ_ALBUM 2
#define TAGCACHE_AZ_ALL_SONGS 3

typedef struct {
    int32_t id; /* 1-based stable slot id; deleted slots are never reused */
    int32_t flags; /* reader status flags, including incomplete optional tags */
    int32_t mtime;
    int32_t size;
    int32_t first_seen;
    int32_t playcount;
    int32_t last_played;
    int32_t rating; /* 1 = favorite, matching Rockbox's rating tag */
    int32_t track_number;
    int32_t disc_number;
    const char * path;
    const char * title;
    const char * artist;
    const char * album;
    const char * album_artist;
    const char * genre;
    /* Storage belongs to this result object.  The tagcache reader fills it
     * before publishing the corresponding pointers above. */
    char path_storage[TAGCACHE_PATH_MAX];
    char title_storage[TAGCACHE_PATH_MAX];
    char artist_storage[TAGCACHE_PATH_MAX];
    char album_storage[TAGCACHE_PATH_MAX];
    char album_artist_storage[TAGCACHE_PATH_MAX];
    char genre_storage[TAGCACHE_PATH_MAX];
} tagcache_song_t;

typedef struct {
    const char * name;
    const char * album_artist; /* album groups only; empty otherwise */
    int song_count;
    int32_t first_song_id;
    char name_storage[TAGCACHE_PATH_MAX];
    char album_artist_storage[TAGCACHE_PATH_MAX];
} tagcache_group_t;

typedef enum {
    TAGCACHE_LOAD_SUCCESS_NORMAL,
    TAGCACHE_LOAD_SUCCESS_FRESH,
    TAGCACHE_LOAD_SUCCESS_RECOVERED,
    TAGCACHE_LOAD_FAILED
} tagcache_load_outcome_t;

typedef struct tagcache_stats_snapshot tagcache_stats_snapshot_t;
typedef struct tagcache_snapshot tagcache_snapshot_t;
#define TAGCACHE_MIGRATION_APPLIED 0x1u
#define TAGCACHE_MIGRATION_ARCHIVE 0x2u

bool tagcache_open(const char * dir);
tagcache_snapshot_t *tagcache_snapshot_open(bool recency);
int tagcache_snapshot_count(const tagcache_snapshot_t *snapshot);
bool tagcache_snapshot_path_at(const tagcache_snapshot_t *snapshot, int rank, char *out, size_t out_size);
void tagcache_snapshot_close(tagcache_snapshot_t *snapshot);
tagcache_snapshot_t *tagcache_snapshot_retain(tagcache_snapshot_t *snapshot);
int tagcache_snapshot_dup_directory_fd(const tagcache_snapshot_t *snapshot);
/* True while the logical database path still names the directory pinned at
 * open time.  Callers use this to cancel work spanning card removal. */
bool tagcache_storage_current(void);
bool tagcache_numeric_write_failed(void);
int tagcache_dup_directory_fd(void);
/* Opens an existing library, or an empty writable database for explicit
 * rebuild when no usable generation can be loaded. */
bool tagcache_open_for_rebuild(const char * dir);
void tagcache_close(void);
/* Returns the outcome of the most recent open, update, or abort.
 * A successful ordinary update without a reload reports NORMAL; a successful
 * recovery rebuild reports RECOVERED. */
tagcache_load_outcome_t tagcache_get_load_outcome(void);

void tagcache_begin_update(void);
void tagcache_begin_update_with_lock(void (*unlock)(void), void (*lock)(void));
/* Marks path seen for this pass. Returns true and fills *out when a live
 * row exists whose stored mtime and size both match. */
bool tagcache_lookup(const char * path, int32_t mtime, int32_t size, tagcache_song_t * out);
void tagcache_upsert(const char * path, int32_t mtime, int32_t size, const char * title, const char * artist,
                     const char * album, const char * album_artist, const char * genre,
                     int32_t track_number, int32_t disc_number);
/* Commits the pass: rebuilds indexes and persists. A row not seen during the
 * pass is removed only when stat() reports ENOENT, any other stat error keeps
 * it. A rebuild after a failed load rejects an empty result and leaves the
 * generation pointer unchanged. Returns false on rejection or write failure,
 * in which case the committed reader is reopened. */
bool tagcache_end_update(void);
/* Releases the caller query lock during private generation construction. */
bool tagcache_end_update_with_lock(void (*unlock)(void), void (*lock)(void));
/* Discard staged scan mutations and reopen the committed files. */
void tagcache_abort_update(void);

enum {
    TAGCACHE_FIELD_TITLE = 1u, TAGCACHE_FIELD_ARTIST = 2u,
    TAGCACHE_FIELD_ALBUM = 4u, TAGCACHE_FIELD_ALBUM_ARTIST = 8u,
    TAGCACHE_FIELD_GENRE = 16u, TAGCACHE_FIELD_PATH = 32u,
    TAGCACHE_FIELD_ALL = 63u
};
/* Numeric fields are always returned; requested string fields are populated. */
bool tagcache_song_fields_by_id(int32_t id, unsigned fields, tagcache_song_t *out);
bool tagcache_song_fields_at_title_rank(int32_t rank, unsigned fields, tagcache_song_t *out);
bool tagcache_song_fields_at_recency_rank(int32_t rank, unsigned fields, tagcache_song_t *out);
int32_t tagcache_live_count(void);
int32_t tagcache_slot_count(void);
bool tagcache_song_by_id(int32_t id, tagcache_song_t * out);
bool tagcache_song_by_path(const char * path, tagcache_song_t * out);
bool tagcache_song_at_slot(int32_t slot, tagcache_song_t * out);
bool tagcache_song_at_title_rank(int32_t rank, tagcache_song_t * out);
bool tagcache_song_at_recency_rank(int32_t rank, tagcache_song_t * out);

/* Query results are ranks in the unchanged title or group order. */
typedef struct {
    const char *needle, *artist, *album_artist, *album;
    uint64_t start, count, position;
    int domain;
    bool indexed, exact;
} tagcache_query_t;
#define TAGCACHE_QUERY_SONGS (-1)
#define TAGCACHE_QUERY_TITLES (-2)
void tagcache_query_begin(tagcache_query_t *query, int domain, const char *needle,
                         const char *artist, const char *album_artist, const char *album);
bool tagcache_query_next(tagcache_query_t *query, int32_t *rank);
int32_t tagcache_query_count(tagcache_query_t *query);
void tagcache_query_skip(tagcache_query_t *query, int32_t count);
int tagcache_group_album_count(int kind, const char *name);
int tagcache_group_album_at(int kind, const char *name, int offset);
int tagcache_group_album_offset(int kind, const char *name, const char *album, const char *album_artist);

int tagcache_group_count(int kind);
bool tagcache_group_at(int kind, int index, tagcache_group_t * out);
/* Page of 1-based song ids from a pre-sorted group membership list. */
int tagcache_artist_song_ids(const char * artist, int offset, int32_t * out_ids, int max);
int tagcache_album_artist_song_ids(const char * album_artist, int offset, int32_t * out_ids, int max);
int tagcache_album_song_ids(const char * album, const char * album_artist, int offset, int32_t * out_ids, int max);

void tagcache_set_rating(const char * path, int32_t rating);
void tagcache_add_play(const char * path, int32_t now);
/* Staging-only overlay used when migrating sidecar stats onto a just-upserted
 * scan row. Persisted by the following end_update write. */
void tagcache_overlay_stats(const char * path, int32_t rating, int32_t playcount, int32_t last_played);
bool tagcache_extract_stats(const char * dir, tagcache_stats_snapshot_t ** out);
/* Returns false when an interesting source path is present on disk but absent
 * from the current database. Matching rows are overlaid before returning. */
bool tagcache_replay_stats(const tagcache_stats_snapshot_t * snapshot);
bool tagcache_replay_stats_allow_missing(const tagcache_stats_snapshot_t * snapshot, size_t * unmatched);
void tagcache_free_stats(tagcache_stats_snapshot_t * snapshot);
int32_t tagcache_generation(void);
uint32_t tagcache_migration_state(void);
void tagcache_set_staged_migration_state(uint32_t state);

int32_t tagcache_title_rank_of_path(const char * path);
int32_t tagcache_recency_rank_of_path(const char * path);
int tagcache_group_index(int kind, const char * name, const char * album_artist);

int tagcache_cmp_ascii(const char * a, const char * b);

/* Whether a raw ARTIST tag belongs to artist `name` under the current
 * delimiters. Queries must use this instead of comparing the raw string. */
bool tagcache_artist_matches(const char * raw_artist, const char * name);
/* First artist name a raw ARTIST tag is filed under. */
void tagcache_artist_primary(const char * raw_artist, char * out, size_t out_size);
const char * tagcache_ascii_casestr(const char * hay, const char * needle);

#endif /* TAGCACHE_H */
