#define _POSIX_C_SOURCE 200809L

#include "storage_migration.h"

#include <assert.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void mkdir_one(const char * path) { assert(mkdir(path, 0755) == 0); }

static void write_file(const char * path, const char * text) {
    FILE * f = fopen(path, "w");
    assert(f);
    assert(fputs(text, f) >= 0);
    assert(fclose(f) == 0);
}

static void assert_file(const char * path, const char * expected) {
    char contents[128] = { 0 };
    FILE * f = fopen(path, "r");
    assert(f);
    assert(fgets(contents, sizeof(contents), f));
    assert(fclose(f) == 0);
    assert(strcmp(contents, expected) == 0);
}

static bool exists(const char * path) { return access(path, F_OK) == 0; }

static void migrate_here(void) {
    int rootfd = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    assert(rootfd >= 0);
    storage_migrate_legacy_data_at(rootfd);
    close(rootfd);
}

static void remove_tree(const char * path) {
    DIR * dir = opendir(path);
    if (!dir) return;
    struct dirent * entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        char child[512];
        snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
        if (unlink(child) != 0) remove_tree(child);
    }
    closedir(dir);
    rmdir(path);
}

/* Cached cover files have to survive three layouts: no destination yet, an
 * empty destination created before the legacy tree is moved, and a
 * destination that already holds a newer file of the same name. */
static void test_albumart_migration(void) {
    char directory[] = "/tmp/compas-albumart-migration-XXXXXX";
    char previous[512];
    assert(getcwd(previous, sizeof(previous)));
    assert(mkdtemp(directory));
    assert(chdir(directory) == 0);

    mkdir_one(".open_hiby_player");
    mkdir_one(".compas");
    mkdir_one(".open_hiby_player/albumart");
    mkdir_one(".open_hiby_player/albumart/nested");
    write_file(".open_hiby_player/albumart/v2-aaaa.72x72.bmp", "thumb-a\n");
    write_file(".open_hiby_player/albumart/v2-bbbb.480x480.bmp", "player-b\n");
    write_file(".open_hiby_player/albumart/nested/v2-cccc.72x72.bmp", "nested-c\n");
    migrate_here();
    assert_file(".compas/albumart/v2-aaaa.72x72.bmp", "thumb-a\n");
    assert_file(".compas/albumart/v2-bbbb.480x480.bmp", "player-b\n");
    assert_file(".compas/albumart/nested/v2-cccc.72x72.bmp", "nested-c\n");
    assert(!exists(".open_hiby_player/albumart"));

    remove_tree(".compas/albumart");
    mkdir_one(".open_hiby_player/albumart");
    mkdir_one(".compas/albumart");
    write_file(".open_hiby_player/albumart/v2-empty.72x72.bmp", "from-legacy\n");
    migrate_here();
    assert_file(".compas/albumart/v2-empty.72x72.bmp", "from-legacy\n");
    assert(!exists(".open_hiby_player/albumart"));

    mkdir_one(".open_hiby_player/albumart");
    write_file(".compas/albumart/v2-shared.72x72.bmp", "current\n");
    write_file(".open_hiby_player/albumart/v2-shared.72x72.bmp", "legacy\n");
    write_file(".open_hiby_player/albumart/v2-only.72x72.bmp", "only-legacy\n");
    migrate_here();
    assert_file(".compas/albumart/v2-shared.72x72.bmp", "current\n");
    assert_file(".compas/albumart/v2-only.72x72.bmp", "only-legacy\n");
    assert_file(".compas/albumart/v2-empty.72x72.bmp", "from-legacy\n");
    assert_file(".open_hiby_player/albumart/v2-shared.72x72.bmp", "legacy\n");
    assert(!exists(".open_hiby_player/albumart/v2-only.72x72.bmp"));

    assert(chdir(previous) == 0);
    remove_tree(directory);
}

int main(void) {
    test_albumart_migration();
    char directory[] = "/tmp/compas-storage-migration-XXXXXX";
    assert(mkdtemp(directory));
    assert(chdir(directory) == 0);
    mkdir_one(".open_hiby_player");
    mkdir_one(".compas");

    /* Durable sidecars move into an already-created destination namespace. */
    write_file(".open_hiby_player/books.list", "legacy books\n");
    write_file(".open_hiby_player/remote_state.tsv", "legacy remote\n");
    write_file(".open_hiby_player/subsonic_servers.tsv", "legacy subsonic\n");
    write_file(".open_hiby_player/subsonic.list", "legacy subsonic list\n");
    write_file(".open_hiby_player/queue.bin", "legacy queue\n");
    write_file(".open_hiby_player/custom_state.bin", "custom state\n");
    for (int i = 0; i < 24; i++) {
        char path[128];
        snprintf(path, sizeof(path), ".open_hiby_player/custom_%02d.bin", i);
        write_file(path, "batch state\n");
    }
    /* Collisions must not consume the bounded collection budget: a movable
     * entry encountered after 256 existing destination names still needs to
     * migrate in this pass. */
    for (int i = 0; i < 256; i++) {
        char old_path[128], new_path[128];
        snprintf(old_path, sizeof(old_path), ".open_hiby_player/collision_%03d.bin", i);
        snprintf(new_path, sizeof(new_path), ".compas/collision_%03d.bin", i);
        write_file(old_path, "legacy collision\n");
        write_file(new_path, "current collision\n");
    }
    write_file(".open_hiby_player/late_movable.bin", "late movable\n");
    write_file(".open_hiby_player/database_000.tcd.g1", "database temp\n");
    write_file(".open_hiby_player/tagcache.gen.tmp", "generation temp\n");
    write_file(".open_hiby_player/migration.gnew", "migration temp\n");
    write_file(".open_hiby_player/state.tmp", "sidecar temp\n");
    write_file(".open_hiby_player/player.installing", "install temp\n");
    assert(symlink("missing-custom-state", ".open_hiby_player/custom_link") == 0);
    write_file(".open_hiby_player/hiby_player", "stock player\n");
    write_file(".open_hiby_player/open_hiby_player", "legacy update\n");
    write_file(".compas/playlists.list", "new playlist\n");

    /* Database files and migration markers belong to metadata_db.c and must
     * remain in the legacy archive for its replay/cleanup state machine. */
    write_file(".open_hiby_player/database_000.tcd", "legacy database\n");
    write_file(".open_hiby_player/tagcache.gen", "legacy generation\n");
    write_file(".open_hiby_player/migration.gpending", "legacy marker\n");

    /* A collision remains in the old namespace and the destination wins. */
    write_file(".open_hiby_player/books.list", "legacy collision\n");
    write_file(".compas/books.list", "current books\n");
    write_file(".open_hiby_player/remote_state.tsv", "legacy remote\n");
    assert(symlink("missing-remote-state", ".compas/remote_state.tsv") == 0);

    int rootfd = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    assert(rootfd >= 0);
    storage_migrate_legacy_data_at(rootfd);
    close(rootfd);

    assert_file(".compas/books.list", "current books\n");
    assert_file(".open_hiby_player/books.list", "legacy collision\n");
    struct stat collision_st;
    assert(lstat(".compas/remote_state.tsv", &collision_st) == 0 && S_ISLNK(collision_st.st_mode));
    assert(access(".compas/remote_state.tsv", F_OK) != 0);
    assert(access(".compas/remote_state.tsv", R_OK) != 0);
    assert_file(".open_hiby_player/remote_state.tsv", "legacy remote\n");
    assert_file(".compas/subsonic_servers.tsv", "legacy subsonic\n");
    assert_file(".compas/subsonic.list", "legacy subsonic list\n");
    assert_file(".compas/queue.bin", "legacy queue\n");
    assert_file(".compas/custom_state.bin", "custom state\n");
    for (int i = 0; i < 24; i++) {
        char path[128];
        snprintf(path, sizeof(path), ".compas/custom_%02d.bin", i);
        assert_file(path, "batch state\n");
    }
    assert_file(".compas/late_movable.bin", "late movable\n");
    assert_file(".open_hiby_player/database_000.tcd.g1", "database temp\n");
    assert_file(".open_hiby_player/tagcache.gen.tmp", "generation temp\n");
    assert_file(".open_hiby_player/migration.gnew", "migration temp\n");
    assert_file(".open_hiby_player/state.tmp", "sidecar temp\n");
    assert_file(".open_hiby_player/player.installing", "install temp\n");
    struct stat link_st;
    assert(lstat(".open_hiby_player/custom_link", &link_st) == 0 && S_ISLNK(link_st.st_mode));
    assert(exists(".open_hiby_player/hiby_player"));
    assert(exists(".open_hiby_player/open_hiby_player"));
    assert_file(".compas/playlists.list", "new playlist\n");
    assert_file(".open_hiby_player/database_000.tcd", "legacy database\n");
    assert_file(".open_hiby_player/tagcache.gen", "legacy generation\n");
    assert_file(".open_hiby_player/migration.gpending", "legacy marker\n");

    /* A second pass is idempotent and does not overwrite either namespace. */
    rootfd = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    assert(rootfd >= 0);
    storage_migrate_legacy_data_at(rootfd);
    close(rootfd);
    assert_file(".compas/books.list", "current books\n");
    assert_file(".open_hiby_player/remote_state.tsv", "legacy remote\n");

    /* A missing/invalid root descriptor is a safe no-op. */
    storage_migrate_legacy_data_at(-1);
    assert(exists(".open_hiby_player/database_000.tcd"));

    unlink(".compas/books.list");
    unlink(".compas/playlists.list");
    unlink(".compas/remote_state.tsv");
    unlink(".compas/subsonic.list");
    unlink(".compas/subsonic_servers.tsv");
    unlink(".compas/queue.bin");
    unlink(".compas/custom_state.bin");
    for (int i = 0; i < 24; i++) {
        char path[128];
        snprintf(path, sizeof(path), ".compas/custom_%02d.bin", i);
        unlink(path);
    }
    unlink(".compas/late_movable.bin");
    for (int i = 0; i < 256; i++) {
        char old_path[128], new_path[128];
        snprintf(old_path, sizeof(old_path), ".open_hiby_player/collision_%03d.bin", i);
        snprintf(new_path, sizeof(new_path), ".compas/collision_%03d.bin", i);
        unlink(old_path);
        unlink(new_path);
    }
    unlink(".open_hiby_player/books.list");
    unlink(".open_hiby_player/remote_state.tsv");
    unlink(".open_hiby_player/database_000.tcd");
    unlink(".open_hiby_player/tagcache.gen");
    unlink(".open_hiby_player/migration.gpending");
    unlink(".open_hiby_player/subsonic.list");
    unlink(".open_hiby_player/hiby_player");
    unlink(".open_hiby_player/open_hiby_player");
    unlink(".open_hiby_player/database_000.tcd.g1");
    unlink(".open_hiby_player/tagcache.gen.tmp");
    unlink(".open_hiby_player/migration.gnew");
    unlink(".open_hiby_player/state.tmp");
    unlink(".open_hiby_player/player.installing");
    unlink(".open_hiby_player/custom_link");
    rmdir(".open_hiby_player");
    rmdir(".compas");
    assert(rmdir(directory) == 0);
    puts("Storage migration: passed");
    return 0;
}
