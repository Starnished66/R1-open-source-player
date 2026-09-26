#define _GNU_SOURCE
#include "metadata_db.h"
#include "tagcache.h"
#include "albumart.h"
#include "metadata_refresh_targets.h"
#include "catalog_source_cache.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void path_cache_drop(void) {}
void remote_state_drop(void) {}
bool remote_state_take(const char *path, int32_t *rating, int32_t *count, int32_t *last) {
    (void) path; (void) rating; (void) count; (void) last;
    return false;
}

static void must(bool ok, const char *what) {
    if (!ok) { fprintf(stderr, "metadata catalog: %s\n", what); exit(1); }
}

static void touch(const char *path) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    must(fd >= 0 && close(fd) == 0, "touch song source");
}

static void put_song(const char *path, const char *title, const char *artist,
                     const char *album, const char *album_artist) {
    cached_tags_t tags;
    memset(&tags, 0, sizeof(tags));
    snprintf(tags.title, sizeof(tags.title), "%s", title);
    snprintf(tags.artist, sizeof(tags.artist), "%s", artist);
    snprintf(tags.album, sizeof(tags.album), "%s", album);
    snprintf(tags.album_artist, sizeof(tags.album_artist), "%s", album_artist);
    snprintf(tags.genre, sizeof(tags.genre), "Genre");
    tags.track_number = 1;
    tags.disc_number = -1;
    metadata_db_put(path, 1234, 10, &tags);
}

typedef struct {
    int * calls;
    int64_t representative_id;
    bool has_sidecar;
} cache_test_context_t;

static bool cache_test_load(void * opaque, catalog_source_resolution_t * out) {
    cache_test_context_t * context = opaque;
    (*context->calls)++;
    out->has_sidecar = context->has_sidecar;
    snprintf(out->cover_key, sizeof(out->cover_key), "e-%016llx",
             (unsigned long long) context->representative_id);
    if (context->has_sidecar)
        snprintf(out->sidecar_path, sizeof(out->sidecar_path), "/covers/%lld.jpg",
                 (long long) context->representative_id);
    out->stat_mtime = 123;
    out->stat_size = 456;
    return true;
}

static void test_catalog_source_cache(void) {
    catalog_source_cache_reset();
    int calls = 0;
    catalog_source_resolution_t first, again;
    cache_test_context_t context = { .calls = &calls, .representative_id = 7, .has_sidecar = true };
    must(catalog_source_cache_resolve("library:1", 7, cache_test_load, &context, &first),
         "resolve first catalog source");
    must(catalog_source_cache_resolve("library:1", 7, cache_test_load, &context, &again) && calls == 1,
         "reuse source resolution within one revision");
    must(first.has_sidecar && strcmp(first.cover_key, again.cover_key) == 0 &&
         strcmp(again.sidecar_path, "/covers/7.jpg") == 0 &&
         again.stat_mtime == 123 && again.stat_size == 456,
         "cached source path and stat fingerprint are retained");
    must(catalog_source_cache_resolve("library:2", 7, cache_test_load, &context, &again) && calls == 2,
         "new revision invalidates source resolutions");

    catalog_source_cache_reset();
    calls = 0;
    for (int64_t id = 100; id < 100 + CATALOG_SOURCE_CACHE_CAPACITY + 1; id++) {
        context.representative_id = id;
        context.has_sidecar = false;
        must(catalog_source_cache_resolve("library:bounded", id, cache_test_load, &context, &again),
             "fill bounded source cache");
    }
    context.representative_id = 100;
    must(catalog_source_cache_resolve("library:bounded", 100, cache_test_load, &context, &again) &&
         calls == CATALOG_SOURCE_CACHE_CAPACITY + 2,
         "least recently used source entry is evicted at the fixed bound");
    catalog_source_cache_reset();
}

static uint64_t reference_album_key(const char * album, const char * album_artist) {
    uint64_t hash = UINT64_C(14695981039346656037);
    const char * fields[] = { album, album_artist };
    for (int field = 0; field < 2; field++) {
        for (const unsigned char * p = (const unsigned char *) fields[field]; *p; p++) {
            unsigned char c = *p;
            if (c >= 'A' && c <= 'Z') c = (unsigned char) (c + ('a' - 'A'));
            hash = (hash ^ c) * UINT64_C(1099511628211);
        }
        if (field == 0) hash = (hash ^ 0) * UINT64_C(1099511628211);
    }
    return hash;
}

static void test_albumart_thumbnail_key(void) {
    albumart_info_t first = {0}, case_variant = {0}, fallback = {0}, explicit_artist = {0};
    snprintf(first.album, sizeof(first.album), "Shared Album");
    snprintf(first.artist, sizeof(first.artist), "Track Artist");
    snprintf(first.albumartist, sizeof(first.albumartist), "Album Artist");
    snprintf(case_variant.album, sizeof(case_variant.album), "shared album");
    snprintf(case_variant.artist, sizeof(case_variant.artist), "A different track artist");
    snprintf(case_variant.albumartist, sizeof(case_variant.albumartist), "album artist");
    must(albumart_thumbnail_key(&first) == albumart_thumbnail_key(&case_variant) &&
         albumart_thumbnail_key(&first) == reference_album_key("Shared Album", "Album Artist"),
         "artwork key follows tagcache case-insensitive album grouping");

    snprintf(fallback.album, sizeof(fallback.album), "Fallback Album");
    snprintf(fallback.artist, sizeof(fallback.artist), "Fallback Artist");
    snprintf(explicit_artist.album, sizeof(explicit_artist.album), "fallback album");
    snprintf(explicit_artist.albumartist, sizeof(explicit_artist.albumartist), "fallback artist");
    must(albumart_thumbnail_key(&fallback) == albumart_thumbnail_key(&explicit_artist),
         "empty album artist falls back to artist in artwork identity");
}

static void test_noop_refresh_abort(void) {
    must(metadata_refresh_should_skip_commit(0, false, false),
         "unchanged refresh with no inserts or deletes skips publication");
    must(!metadata_refresh_should_skip_commit(1, false, false),
         "changed tags keep the normal commit path");
    must(!metadata_refresh_should_skip_commit(0, true, false),
         "deleted rows keep the normal commit path");
    must(!metadata_refresh_should_skip_commit(0, false, true),
         "inserted rows keep the normal commit path");

    char root[] = "/tmp/compas-noop-refresh-XXXXXX";
    must(mkdtemp(root) != NULL && chdir(root) == 0, "no-op refresh fixture directory");
    must(metadata_db_open() == METADATA_DB_LOAD_SUCCESS_FRESH, "open no-op refresh database");
    touch("unchanged.flac");
    must(access("unchanged.flac", F_OK) == 0, "check no-op refresh source");
    metadata_db_begin_update();
    put_song("unchanged.flac", "Unchanged title", "Artist", "Album", "Album Artist");
    must(metadata_db_end_update(), "commit no-op refresh fixture row");

    int32_t generation = tagcache_generation();
    metadata_db_catalog_page_t before;
    metadata_db_catalog_song_t songs[METADATA_DB_CATALOG_PAGE_MAX];
    must(metadata_db_catalog_songs_page(NULL, 0, METADATA_DB_CATALOG_PAGE_MAX,
                                        &before, songs) == METADATA_DB_CATALOG_OK,
         "read no-op refresh catalog revision");
    metadata_db_begin_targeted_update();
    cached_tags_t reread;
    memset(&reread, 0, sizeof(reread));
    snprintf(reread.title, sizeof(reread.title), "Unchanged title");
    snprintf(reread.artist, sizeof(reread.artist), "ARTIST");
    snprintf(reread.album, sizeof(reread.album), "album");
    snprintf(reread.album_artist, sizeof(reread.album_artist), "Album Artist");
    snprintf(reread.genre, sizeof(reread.genre), "genre");
    reread.track_number = 1;
    reread.disc_number = -1;
    must(!metadata_db_put("unchanged.flac", 1234, 10, &reread),
         "a reread that interns back to the stored spelling reports no change");
    reread.track_number = 2;
    must(metadata_db_put("unchanged.flac", 1234, 10, &reread), "a changed track number reports a change");
    must(metadata_db_put("inserted.flac", 1234, 10, &reread), "an inserted row reports a change");
    metadata_db_abort_update();
    must(tagcache_generation() == generation, "aborted no-op refresh keeps tagcache generation");
    metadata_db_catalog_page_t after;
    must(metadata_db_catalog_songs_page(NULL, 0, METADATA_DB_CATALOG_PAGE_MAX,
                                        &after, songs) == METADATA_DB_CATALOG_OK &&
         strcmp(after.revision, before.revision) == 0,
         "aborted no-op refresh keeps catalog revision");

    cached_tags_t cached;
    must(metadata_db_get("unchanged.flac", 1234, 10, &cached) &&
         strcmp(cached.title, "Unchanged title") == 0,
         "abort leaves the committed tag cache readable at its stored stat key");
    must(!metadata_db_get("unchanged.flac", 1235, 10, &cached),
         "stale stat key safely forces one later ordinary reread");
    metadata_db_close();
}

static void test_refresh_target_path_deduplication(void) {
    const char * input[] = {
        "music/Z.flac", "music/A.flac", "music/Z.flac", "music/B.flac", "music/A.flac"
    };
    char ** paths = NULL;
    int count = metadata_refresh_copy_unique_paths(input, 5, &paths);
    must(count == 3 && paths != NULL, "multiple refresh paths deduplicate to their distinct count");
    must(strcmp(paths[0], "music/A.flac") == 0 && strcmp(paths[1], "music/B.flac") == 0 &&
         strcmp(paths[2], "music/Z.flac") == 0,
         "deduplicated refresh paths are sorted for stable processing");
    for (int i = 0; i < count; i++) free(paths[i]);
    free(paths);
}

static bool cached_tags_equal(const cached_tags_t * a, const cached_tags_t * b) {
    return strcmp(a->title, b->title) == 0 && strcmp(a->artist, b->artist) == 0 &&
           strcmp(a->album, b->album) == 0 && strcmp(a->album_artist, b->album_artist) == 0 &&
           strcmp(a->genre, b->genre) == 0 && a->track_number == b->track_number &&
           a->disc_number == b->disc_number;
}

static void test_force_metadata_upsert(void) {
    char root[] = "/tmp/compas-force-refresh-XXXXXX";
    must(mkdtemp(root) != NULL && chdir(root) == 0, "forced refresh fixture directory");
    must(metadata_db_open() == METADATA_DB_LOAD_SUCCESS_FRESH, "open forced refresh database");

    const char * paths[] = { "same-stats.flac", "untouched-one.flac", "untouched-two.flac" };
    struct stat stats[3];
    cached_tags_t original_tags[3] = {0};
    for (int i = 0; i < 3; i++) {
        touch(paths[i]);
        must(stat(paths[i], &stats[i]) == 0, "stat forced refresh source");
        snprintf(original_tags[i].title, sizeof(original_tags[i].title), "Old title %d", i + 1);
        snprintf(original_tags[i].artist, sizeof(original_tags[i].artist), "Artist %d", i + 1);
        snprintf(original_tags[i].album, sizeof(original_tags[i].album), "Album");
        snprintf(original_tags[i].album_artist, sizeof(original_tags[i].album_artist), "Album Artist");
        snprintf(original_tags[i].genre, sizeof(original_tags[i].genre), "Genre");
        original_tags[i].track_number = i + 1;
        original_tags[i].disc_number = -1;
    }

    metadata_db_begin_update();
    for (int i = 0; i < 3; i++)
        metadata_db_put(paths[i], stats[i].st_mtime, stats[i].st_size, &original_tags[i]);
    must(metadata_db_end_update(), "commit original forced refresh tags");

    song_row_t before[3];
    tagcache_song_t stats_before[3];
    for (int i = 0; i < 3; i++) {
        must(metadata_db_get_song_by_path(paths[i], &before[i]), "read original forced refresh row");
        tagcache_set_rating(paths[i], i + 1);
        for (int play = 0; play <= i; play++) tagcache_add_play(paths[i], 2222 + play);
        must(tagcache_song_by_path(paths[i], &stats_before[i]), "read original song statistics");
        must(stats_before[i].rating == i + 1 && stats_before[i].playcount == i + 1 &&
             stats_before[i].last_played > 0, "fixture has favorite and play history");
    }

    cached_tags_t still_cached;
    must(metadata_db_get(paths[0], stats[0].st_mtime, stats[0].st_size, &still_cached) &&
         strcmp(still_cached.title, "Old title 1") == 0,
         "unchanged mtime and size still return the old cached metadata before forced refresh");

    cached_tags_t new_tags = original_tags[0];
    snprintf(new_tags.title, sizeof(new_tags.title), "New title");
    snprintf(new_tags.artist, sizeof(new_tags.artist), "Updated Artist");
    int32_t generation_before = tagcache_generation();
    must(unlink(paths[1]) == 0, "remove unrelated source before targeted refresh");
    metadata_db_begin_targeted_update();
    metadata_db_put(paths[0], stats[0].st_mtime, stats[0].st_size, &new_tags);
    must(metadata_db_end_update(), "commit partial metadata refresh");
    struct stat missing_stat;
    must(stat(paths[1], &missing_stat) != 0 && errno == ENOENT,
         "unrelated source is absent while targeted refresh commits");

    cached_tags_t refreshed;
    must(metadata_db_get(paths[0], stats[0].st_mtime, stats[0].st_size, &refreshed) &&
         strcmp(refreshed.title, "New title") == 0 && strcmp(refreshed.artist, "Updated Artist") == 0,
         "forced upsert replaces tags despite unchanged mtime and size");
    must(tagcache_generation() != generation_before, "partial metadata refresh advances catalog revision");

    song_row_t after;
    tagcache_song_t stats_after;
    must(metadata_db_get_song_by_path(paths[0], &after) && tagcache_song_by_path(paths[0], &stats_after),
         "read refreshed row and statistics");
    must(after.id == before[0].id, "forced refresh preserves the stable song id");
    must(stats_after.rating == stats_before[0].rating && stats_after.playcount == stats_before[0].playcount &&
         stats_after.last_played == stats_before[0].last_played,
         "forced refresh preserves favorite, play count and last-played");

    for (int i = 1; i < 3; i++) {
        must(metadata_db_get_song_by_path(paths[i], &after) && tagcache_song_by_path(paths[i], &stats_after),
             "partial refresh retains every untouched song");
        must(after.id == before[i].id && cached_tags_equal(&after.tags, &before[i].tags),
             "partial refresh leaves untouched metadata and stable ID unchanged");
        must(stats_after.rating == stats_before[i].rating && stats_after.playcount == stats_before[i].playcount &&
             stats_after.last_played == stats_before[i].last_played,
             "partial refresh leaves untouched favorite and play history unchanged");
    }

    metadata_db_begin_targeted_update();
    must(metadata_db_delete_target_path(paths[1]), "targeted refresh marks confirmed missing target for deletion");
    must(metadata_db_end_update(), "commit targeted missing-path deletion");
    must(!metadata_db_get_song_by_path(paths[1], &after), "targeted refresh removes the selected missing song");
    must(metadata_db_get_song_by_path(paths[2], &after) && after.id == before[2].id,
         "deleting a targeted path preserves unrelated song IDs");

    song_row_t sparse_rows[4];
    must(metadata_db_count_songs_filtered(NULL, NULL, NULL, NULL, NULL) == 2 &&
         metadata_db_get_songs_filtered_page(NULL, NULL, NULL, NULL, NULL, 0, 4, sparse_rows) == 2,
         "song query count and page skip a targeted-delete tombstone");
    must(metadata_db_search_songs("Old title", sparse_rows, 4) == 1 &&
         strcmp(sparse_rows[0].path, paths[2]) == 0,
         "song search returns the live match after a targeted-delete tombstone");
    group_row_t sparse_groups[4];
    must(metadata_db_get_groups_page(METADATA_DB_GROUP_ARTIST, 0, 4, sparse_groups) == 2,
         "artist group page skips a targeted-delete tombstone");
    must(metadata_db_get_groups_page(METADATA_DB_GROUP_ALBUM, 0, 4, sparse_groups) == 1 &&
         sparse_groups[0].song_count == 2,
         "album group page counts only live rows around a tombstone");

    metadata_db_catalog_page_t sparse_catalog_page;
    metadata_db_catalog_song_t sparse_catalog_songs[METADATA_DB_CATALOG_PAGE_MAX];
    must(metadata_db_catalog_songs_page(NULL, 0, METADATA_DB_CATALOG_PAGE_MAX,
                                        &sparse_catalog_page, sparse_catalog_songs) ==
             METADATA_DB_CATALOG_OK && sparse_catalog_page.total == 2 && sparse_catalog_page.count == 2,
         "catalog song page skips a targeted-delete tombstone");
    metadata_db_catalog_cover_t sparse_covers[METADATA_DB_CATALOG_PAGE_MAX];
    must(metadata_db_catalog_covers_page(sparse_catalog_page.revision, 0,
                                         METADATA_DB_CATALOG_PAGE_MAX, &sparse_catalog_page, sparse_covers) ==
             METADATA_DB_CATALOG_OK && sparse_catalog_page.total == 1 && sparse_catalog_page.count == 1,
         "catalog cover page skips a targeted-delete tombstone");
    metadata_db_close();
}

static void test_targeted_delete_statistics(void) {
    char root[] = "/tmp/compas-targeted-delete-stats-XXXXXX";
    must(mkdtemp(root) != NULL && chdir(root) == 0, "targeted delete statistics fixture directory");
    must(metadata_db_open() == METADATA_DB_LOAD_SUCCESS_FRESH, "open targeted delete statistics database");

    touch("old.flac");
    cached_tags_t tags = {0};
    snprintf(tags.title, sizeof(tags.title), "Old song");
    snprintf(tags.artist, sizeof(tags.artist), "Artist");
    snprintf(tags.album, sizeof(tags.album), "Album");
    snprintf(tags.album_artist, sizeof(tags.album_artist), "Artist");
    snprintf(tags.genre, sizeof(tags.genre), "Genre");
    tags.track_number = 1;
    tags.disc_number = -1;
    metadata_db_begin_update();
    metadata_db_put("old.flac", 1234, 10, &tags);
    must(metadata_db_end_update(), "commit original targeted delete statistics row");
    tagcache_set_rating("old.flac", 1);
    tagcache_add_play("old.flac", 101);

    metadata_db_begin_targeted_update();
    tagcache_set_rating("old.flac", 7);
    tagcache_add_play("old.flac", 202);
    tagcache_add_play("old.flac", 303);
    must(metadata_db_delete_target_path("old.flac"), "delete updated row during targeted transaction");
    touch("new.flac");
    snprintf(tags.title, sizeof(tags.title), "New song");
    metadata_db_put("new.flac", 1234, 10, &tags);
    must(metadata_db_end_update(), "commit replacement row after targeted delete");

    song_row_t replacement;
    tagcache_song_t replacement_stats;
    must(!metadata_db_get_song_by_path("old.flac", &replacement),
         "targeted transaction removes the original row");
    must(metadata_db_get_song_by_path("new.flac", &replacement) && replacement.id == 2 &&
         tagcache_slot_count() == 2,
         "new row appends after the deleted slot in a targeted transaction");
    must(tagcache_song_by_path("new.flac", &replacement_stats) &&
         replacement_stats.rating == 0 && replacement_stats.playcount == 0 &&
         replacement_stats.last_played == 0,
         "statistics from a deleted row do not transfer to its replacement");
    metadata_db_close();
}

static bool valid_uuid(const char *id) {
    if (strlen(id) != 36) return false;
    for (int i = 0; i < 36; i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (id[i] != '-') return false;
        } else if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f'))) {
            return false;
        }
    }
    return true;
}

int main(void) {
    test_albumart_thumbnail_key();
    test_catalog_source_cache();

    char split_artists[TAGCACHE_ARTIST_SPLIT_MAX][TAGCACHE_TAG_MAX] = {{0}};
    int split_count = tagcache_artist_names("Artist One / Artist Two; artist one",
                                             split_artists, TAGCACHE_ARTIST_SPLIT_MAX);
    must(split_count == 2 && strcmp(split_artists[0], "Artist One") == 0 &&
         strcmp(split_artists[1], "Artist Two") == 0,
         "catalog artist memberships match tagcache splitting and deduplication");

    char root[] = "/tmp/compas-catalog-XXXXXX";
    must(mkdtemp(root) != NULL && chdir(root) == 0, "fixture directory");
    must(metadata_db_open() == METADATA_DB_LOAD_SUCCESS_FRESH, "open fresh catalog");

    metadata_db_catalog_page_t first;
    metadata_db_catalog_song_t songs[METADATA_DB_CATALOG_PAGE_MAX];
    must(metadata_db_catalog_songs_page(NULL, 0, 100, &first, songs) == METADATA_DB_CATALOG_OK,
         "capture initial page");
    must(valid_uuid(first.library_id) && first.total == 0 && first.count == 0,
         "fresh catalog identity and empty page");
    char initial_revision[METADATA_DB_CATALOG_REVISION_SIZE];
    snprintf(initial_revision, sizeof(initial_revision), "%s", first.revision);

    touch("one.flac");
    touch("two.flac");
    touch("three.flac");
    metadata_db_begin_update();
    /* Explicit album-artist values keep same-named albums by different
     * artists separate in the existing tagcache grouping. */
    put_song("one.flac", "One", "Artist One", "Shared Album", "Artist One");
    put_song("two.flac", "Two", "Artist Two", "Shared Album", "Artist Two");
    put_song("three.flac", "Three", "Artist One", "Other Album", "Explicit Album Artist");
    must(metadata_db_end_update(), "commit catalog rows");

    metadata_db_catalog_page_t page;
    must(metadata_db_catalog_songs_page(initial_revision, 0, 100, &page, songs) ==
         METADATA_DB_CATALOG_STALE, "old revision rejected after commit");
    must(metadata_db_catalog_songs_page(NULL, 0, 2, &page, songs) == METADATA_DB_CATALOG_OK,
         "first bounded page");
    must(page.total == 3 && page.count == 2, "song page totals");
    metadata_db_catalog_page_t all_page;
    metadata_db_catalog_song_t all_songs[METADATA_DB_CATALOG_PAGE_MAX];
    must(metadata_db_catalog_songs_page(page.revision, 0, 100, &all_page, all_songs) ==
         METADATA_DB_CATALOG_OK && all_page.count == 3, "full catalog page within bounded limit");
    int shared_group = tagcache_group_index(TAGCACHE_GROUP_ALBUM, "Shared Album", "Artist One");
    int folded_group = tagcache_group_index(TAGCACHE_GROUP_ALBUM, "shared album", "artist one");
    must(shared_group >= 0 && shared_group == folded_group,
         "tagcache resolves case variants to one album group");
    must(metadata_db_catalog_validate_song_revision(page.revision, all_songs[0].song.id) ==
         METADATA_DB_CATALOG_OK, "catalog action accepts a current song id");
    must(metadata_db_catalog_validate_song_revision(initial_revision, all_songs[0].song.id) ==
         METADATA_DB_CATALOG_STALE, "catalog action rejects an old revision");
    song_row_t resolved_song;
    must(metadata_db_catalog_get_song_revision(page.revision, all_songs[0].song.id, &resolved_song) ==
         METADATA_DB_CATALOG_OK && strcmp(resolved_song.path, all_songs[0].song.path) == 0,
         "catalog action resolves the concrete row under its revision guard");
    must(metadata_db_catalog_get_song_revision(initial_revision, all_songs[0].song.id, &resolved_song) ==
         METADATA_DB_CATALOG_STALE, "deferred catalog action cannot resolve an old revision");
    int64_t shared_keys[2] = {0};
    int shared_count = 0;
    for (int i = 0; i < all_page.count; i++) {
        if (strcmp(all_songs[i].song.tags.album, "Shared Album") == 0)
            shared_keys[shared_count++] = all_songs[i].album_representative_id;
    }
    must(shared_count == 2 && shared_keys[0] > 0 && shared_keys[1] > 0 &&
         shared_keys[0] != shared_keys[1], "same-named albums by different artists stay separate");

    metadata_db_catalog_page_t covers_page;
    metadata_db_catalog_cover_t covers[METADATA_DB_CATALOG_PAGE_MAX];
    must(metadata_db_catalog_covers_page(page.revision, 0, 2, &covers_page, covers) ==
         METADATA_DB_CATALOG_OK, "first covers page");
    must(covers_page.total == 3 && covers_page.count == 2, "cover associations page by album pair");
    bool found_first_shared = false, found_second_shared = false;
    for (int i = 0; i < covers_page.count; i++) {
        if (covers[i].representative_id == shared_keys[0]) found_first_shared = true;
        if (covers[i].representative_id == shared_keys[1]) found_second_shared = true;
    }
    metadata_db_catalog_cover_t last_cover[METADATA_DB_CATALOG_PAGE_MAX];
    must(metadata_db_catalog_covers_page(page.revision, 2, 2, &covers_page, last_cover) ==
         METADATA_DB_CATALOG_OK, "second covers page");
    must(covers_page.total == 3 && covers_page.count == 1, "cover page end count");
    for (int i = 0; i < covers_page.count; i++) {
        if (last_cover[i].representative_id == shared_keys[0]) found_first_shared = true;
        if (last_cover[i].representative_id == shared_keys[1]) found_second_shared = true;
    }
    must(found_first_shared && found_second_shared, "song and cover album keys match");

    metadata_db_catalog_cover_t source;
    must(metadata_db_catalog_cover_source(page.revision, shared_keys[0], &covers_page, &source) ==
         METADATA_DB_CATALOG_OK && source.representative_id == shared_keys[0],
         "only a current representative resolves");
    must(metadata_db_catalog_cover_source(initial_revision, shared_keys[0], &covers_page, &source) ==
         METADATA_DB_CATALOG_STALE, "cover source rejects stale revision");

    char persisted_id[METADATA_DB_CATALOG_LIBRARY_ID_SIZE];
    snprintf(persisted_id, sizeof(persisted_id), "%s", page.library_id);
    metadata_db_close();
    must(metadata_db_open() == METADATA_DB_LOAD_SUCCESS_NORMAL, "reopen persisted catalog");
    must(metadata_db_catalog_songs_page(NULL, 0, 100, &page, songs) == METADATA_DB_CATALOG_OK &&
         strcmp(page.library_id, persisted_id) == 0, "library ID persists across normal reopen");
    metadata_db_close();

    test_refresh_target_path_deduplication();
    test_noop_refresh_abort();
    test_force_metadata_upsert();
    test_targeted_delete_statistics();

    char other_root[] = "/tmp/compas-catalog-fresh-XXXXXX";
    must(mkdtemp(other_root) != NULL && chdir(other_root) == 0, "second fixture directory");
    must(metadata_db_open() == METADATA_DB_LOAD_SUCCESS_FRESH, "open second fresh catalog");
    must(metadata_db_catalog_songs_page(NULL, 0, 100, &page, songs) == METADATA_DB_CATALOG_OK &&
         valid_uuid(page.library_id) && strcmp(page.library_id, persisted_id) != 0,
         "fresh catalog gets a new incarnation ID");
    metadata_db_close();

    char rebuild_root[] = "/tmp/compas-catalog-rebuild-XXXXXX";
    must(mkdtemp(rebuild_root) != NULL && chdir(rebuild_root) == 0, "rebuild fixture directory");
    must(mkdir(".compas", 0755) == 0, "create corrupt catalog directory");
    touch(".compas/database_idx.tcd.g1");
    must(metadata_db_prepare_rebuild(), "prepare rebuild after corrupt catalog");
    must(metadata_db_catalog_songs_page(NULL, 0, 100, &page, songs) ==
         METADATA_DB_CATALOG_UNAVAILABLE, "catalog stays unavailable during provisional rebuild");
    touch("replacement.flac");
    metadata_db_begin_update();
    put_song("replacement.flac", "Replacement", "Artist", "Album", "Artist");
    must(metadata_db_end_update(), "commit replacement catalog generation");
    must(metadata_db_catalog_songs_page(NULL, 0, 100, &page, songs) == METADATA_DB_CATALOG_OK &&
         page.total == 1 && valid_uuid(page.library_id),
         "catalog becomes available only after replacement identity commits");
    metadata_db_close();
    puts("metadata catalog: PASS");
    return 0;
}
