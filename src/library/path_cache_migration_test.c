#define _POSIX_C_SOURCE 200809L

#include "path_cache.h"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
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

static void assert_books(const char * first, const char * second) {
    char ** paths = NULL;
    int count = 0;
    path_cache_load(PATH_CACHE_BOOKS, &paths, &count);
    assert(count == 2);
    assert(strcmp(paths[0], first) == 0);
    assert(strcmp(paths[1], second) == 0);
    for (int i = 0; i < count; i++) free(paths[i]);
    free(paths);
}

int main(void) {
    char directory[] = "/tmp/compas-path-cache-XXXXXX";
    assert(mkdtemp(directory));
    assert(chdir(directory) == 0);

    /* A new namespace can already exist while an older card still contains
     * the list. Loading must fall back per file instead of treating an empty
     * .compas directory as authoritative. */
    mkdir_one(".compas");
    mkdir_one(".open_hiby_player");
    write_file(".open_hiby_player/books.list", "z-book\na-book\n");

    /* Metadata initialization may load the sidecar before the caller has a
     * directory fd to bind. The same per-file legacy fallback must work in
     * that ordinary path as well. */
    assert_books("a-book", "z-book");
    path_cache_drop();

    int rootfd = open(".", O_RDONLY | O_DIRECTORY);
    assert(rootfd >= 0);
    assert(path_cache_bind_directory_fd(rootfd));
    close(rootfd);
    assert_books("a-book", "z-book");

    /* Read-only-style compatibility: loading from the legacy directory does
     * not require a write or rename when .compas already exists. */
    path_cache_drop();
    assert(chmod(".compas", 0555) == 0);
    assert(chmod(".open_hiby_player", 0555) == 0);
    rootfd = open(".", O_RDONLY | O_DIRECTORY);
    assert(rootfd >= 0);
    assert(path_cache_bind_directory_fd(rootfd));
    close(rootfd);
    assert_books("a-book", "z-book");

    path_cache_drop();
    chmod(".compas", 0755);
    chmod(".open_hiby_player", 0755);
    unlink(".open_hiby_player/books.list");
    rmdir(".open_hiby_player");
    rmdir(".compas");
    assert(rmdir(directory) == 0);
    puts("Path-cache legacy migration: passed");
    return 0;
}
