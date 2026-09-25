#define _GNU_SOURCE
#include "metadata_db.h"
#include "tagcache.h"
#include "albumart.h"
#include "catalog_source_cache.h"

#include <assert.h>
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
