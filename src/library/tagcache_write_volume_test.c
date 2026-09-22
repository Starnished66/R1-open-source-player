#include "tagcache.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef SONGS
#define SONGS 1000
#endif

static unsigned long long write_bytes, pwrite_bytes;
static unsigned long write_calls, pwrite_calls;

ssize_t __real_write(int fd, const void *buf, size_t count);
ssize_t __real_pwrite(int fd, const void *buf, size_t count, off_t offset);
ssize_t __wrap_write(int fd, const void *buf, size_t count) {
    ssize_t result = __real_write(fd, buf, count);
    if (result > 0) { write_calls++; write_bytes += (size_t) result; }
    return result;
}
ssize_t __wrap_pwrite(int fd, const void *buf, size_t count, off_t offset) {
    ssize_t result = __real_pwrite(fd, buf, count, offset);
    if (result > 0) { pwrite_calls++; pwrite_bytes += (size_t) result; }
    return result;
}

static void require_ok(int ok, const char *what) {
    if (!ok) {
        fprintf(stderr, "tagcache write volume: %s\n", what);
        exit(EXIT_FAILURE);
    }
}

static void reset_counters(void) {
    write_bytes = pwrite_bytes = 0;
    write_calls = pwrite_calls = 0;
}

static void print_phase(const char *name) {
    printf("phase=%s write_bytes=%llu pwrite_bytes=%llu write_calls=%lu pwrite_calls=%lu total_bytes=%llu\n",
           name, write_bytes, pwrite_bytes, write_calls, pwrite_calls, write_bytes + pwrite_bytes);
}

static void fixture_file(const char *path) {
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    require_ok(fd >= 0, "create fixture");
    require_ok(close(fd) == 0, "close fixture");
}

static void song_path(char *out, size_t size, const char *root, int index) {
    snprintf(out, size, "%s/song-%04d.flac", root, index);
}

static void upsert_song(const char *root, int index, int changed) {
    char path[TAGCACHE_PATH_MAX], title[64], album[32], artist[32];
    song_path(path, sizeof(path), root, index);
    snprintf(title, sizeof(title), changed && index == 317 ? "Title changed 317" : "Title %04d", index);
    snprintf(album, sizeof(album), "Album %02d", index % 10);
    snprintf(artist, sizeof(artist), "Artist %d", index % 5);
    tagcache_upsert(path, 100 + (changed && index == 317 ? 1 : 0), 4096, title, artist, album, artist, "Genre", 1, 1);
}

static void scan_song(const char *root, int index, int changed) {
    char path[TAGCACHE_PATH_MAX];
    song_path(path, sizeof(path), root, index);
    int mtime = 100 + (changed && index == 317 ? 1 : 0);
    if (!tagcache_lookup(path, mtime, 4096, NULL)) upsert_song(root, index, changed);
}

static void verify_song(const char *root, int index, const char *title) {
    char path[TAGCACHE_PATH_MAX];
    tagcache_song_t song;
    song_path(path, sizeof(path), root, index);
    require_ok(tagcache_song_by_path(path, &song), "lookup result");
    require_ok(strcmp(song.title, title) == 0, "title result");
}

static void cleanup(const char *root) {
    DIR *dir = opendir(root);
    if (!dir) return;
    struct dirent *entry;
    char path[TAGCACHE_PATH_MAX + 64];
    while ((entry = readdir(dir))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        snprintf(path, sizeof(path), "%s/%s", root, entry->d_name);
        unlink(path);
    }
    closedir(dir);
    rmdir(root);
}

int main(void) {
    char root[700];
    const char *base = getenv("TAGCACHE_TEST_DIR");
    if (!base || !base[0] || strlen(base) > 500) base = "/tmp";
    snprintf(root, sizeof(root), "%s/tagcache-write-volume-XXXXXX", base);
    require_ok(mkdtemp(root) != NULL, "temporary directory");
    for (int i = 0; i < SONGS + 1; i++) {
        char path[TAGCACHE_PATH_MAX];
        song_path(path, sizeof(path), root, i);
        fixture_file(path);
    }
    require_ok(tagcache_open(root), "open database");

    reset_counters();
    tagcache_begin_update();
    for (int i = 0; i < SONGS; i++) scan_song(root, i, 0);
    require_ok(tagcache_end_update(), "initial commit");
    print_phase("initial");
    verify_song(root, 317, "Title 0317");

    reset_counters();
    tagcache_begin_update();
    for (int i = 0; i < SONGS; i++) scan_song(root, i, 0);
    require_ok(tagcache_end_update(), "unchanged commit");
    print_phase("unchanged");
    verify_song(root, 317, "Title 0317");

    reset_counters();
    tagcache_begin_update();
    for (int i = 0; i < SONGS; i++) scan_song(root, i, 1);
    require_ok(tagcache_end_update(), "changed commit");
    print_phase("changed");
    verify_song(root, 317, "Title changed 317");

    reset_counters();
    tagcache_begin_update();
    for (int i = 0; i < SONGS; i++) scan_song(root, i, 1);
    scan_song(root, SONGS, 0);
    require_ok(tagcache_end_update(), "append commit");
    print_phase("append");
    require_ok(tagcache_live_count() == SONGS + 1, "append count");

    char deleted[TAGCACHE_PATH_MAX];
    song_path(deleted, sizeof(deleted), root, 777);
    require_ok(unlink(deleted) == 0, "delete fixture");
    reset_counters();
    tagcache_begin_update();
    for (int i = 0; i < SONGS + 1; i++) if (i != 777) scan_song(root, i, 1);
    require_ok(tagcache_end_update(), "delete commit");
    print_phase("delete");
    require_ok(!tagcache_song_by_path(deleted, NULL), "deleted lookup");
    require_ok(tagcache_live_count() == SONGS, "delete count");

    tagcache_close();
    cleanup(root);
    puts("tagcache write volume: PASS");
    return 0;
}
