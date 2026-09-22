#define _GNU_SOURCE
#include "metadata_db.h"
#include "tagcache.h"

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
bool remote_state_take(const char *p, int32_t *r, int32_t *c, int32_t *l) {
    (void)p; (void)r; (void)c; (void)l; return false;
}

static void must(bool ok, const char *what) {
    if (!ok) { fprintf(stderr, "metadata migration retry: %s\n", what); exit(1); }
}

static void touch(const char *path) {
    int fd = open(path, O_WRONLY | O_CREAT, 0644);
    must(fd >= 0 && close(fd) == 0, "touch");
}

static void scan_song(const char *path) {
    cached_tags_t t;
    memset(&t, 0, sizeof(t));
    snprintf(t.title, sizeof(t.title), "Title");
    snprintf(t.artist, sizeof(t.artist), "Artist");
    snprintf(t.album, sizeof(t.album), "Album");
    snprintf(t.album_artist, sizeof(t.album_artist), "Artist");
    snprintf(t.genre, sizeof(t.genre), "Genre");
    t.track_number = 1;
    t.disc_number = -1;
    metadata_db_put(path, 1, 1, &t);
}

int main(void) {
    char root[] = "/tmp/compas-metadata-retry-XXXXXX";
    must(mkdtemp(root) != NULL && chdir(root) == 0, "fixture directory");
    must(mkdir(".open_hiby_player", 0755) == 0, "legacy directory");
    touch("favorite.flac");
    touch("ordinary.flac");
    touch("gone.flac");

    must(tagcache_open(".open_hiby_player"), "open legacy database");
    tagcache_begin_update();
    tagcache_upsert("favorite.flac", 1, 1, "Old", "Artist", "Album", "Artist", "Genre", 1, -1);
    tagcache_upsert("ordinary.flac", 1, 1, "Old", "Artist", "Album", "Artist", "Genre", 1, -1);
    tagcache_upsert("gone.flac", 1, 1, "Old", "Artist", "Album", "Artist", "Genre", 1, -1);
    must(tagcache_end_update(), "commit legacy database");
    tagcache_begin_update();
    tagcache_overlay_stats("favorite.flac", 1, 7, 1234);
    tagcache_overlay_stats("gone.flac", 1, 5, 999);
    must(tagcache_end_update(), "commit legacy favorite");
    tagcache_close();
    must(unlink("gone.flac") == 0, "remove deleted favorite");

    must(metadata_db_open() == METADATA_DB_LOAD_SUCCESS_FRESH, "open migration destination");
    must(metadata_db_migration_prepare(), "prepare migration");
    metadata_db_begin_update();
    /* The favorite path still exists, but the scan omitted it. This is the
     * parser-failure case that previously became permanently APPLIED. */
    scan_song("ordinary.flac");
    must(!metadata_db_end_update(), "reject migration with existing skipped path");
    must(access(".compas/migration.pending", F_OK) == 0, "pending marker retained");
    metadata_db_close();

    must(metadata_db_open() == METADATA_DB_LOAD_SUCCESS_FRESH, "reopen pending migration");
    must(metadata_db_migration_needed(), "migration remains pending");
    must(metadata_db_migration_prepare(), "retry prepare");
    metadata_db_begin_update();
    scan_song("favorite.flac");
    scan_song("ordinary.flac");
    must(metadata_db_end_update(), "retry commit");
    must(metadata_db_migration_finish(), "finish retry");
    metadata_db_close();

    must(metadata_db_open() == METADATA_DB_LOAD_SUCCESS_NORMAL, "reopen committed migration");
    tagcache_song_t song;
    must(tagcache_song_by_path("favorite.flac", &song), "find migrated favorite");
    must(song.rating == 1 && song.playcount == 7 && song.last_played == 1234,
         "favorite history survives retry");
    metadata_db_close();
    puts("metadata migration retry: PASS");
    return 0;
}
