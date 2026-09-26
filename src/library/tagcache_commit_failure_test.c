#define _GNU_SOURCE
#include "tagcache.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

ssize_t __real_write(int, const void *, size_t);
int __real_fsync(int);
int __real_rename(const char *, const char *);

enum failure_kind { FAIL_REFS_WRITE, FAIL_REFS_FSYNC, FAIL_REFS_RENAME, FAIL_POINTER_RENAME };
static enum failure_kind armed;
static int fail_once;
static int master_generation_fsyncs;

static int fd_is_master_generation(int fd) {
    char link[64], path[PATH_MAX];
    snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
    ssize_t n = readlink(link, path, sizeof(path) - 1);
    if (n < 0) return 0;
    path[n] = '\0';
    const char * name = strrchr(path, '/');
    name = name ? name + 1 : path;
    const char * marker = strstr(name, "database_idx.tcd.g");
    if (!marker) return 0;
    marker += strlen("database_idx.tcd.g");
    if (*marker < '0' || *marker > '9') return 0;
    while (*marker >= '0' && *marker <= '9') marker++;
    return *marker == '\0';
}

static int fd_is(const char *needle, int fd) {
    char link[64], path[PATH_MAX];
    snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
    ssize_t n = readlink(link, path, sizeof(path) - 1);
    if (n < 0) return 0;
    path[n] = '\0';
    return strstr(path, needle) != NULL;
}

ssize_t __wrap_write(int fd, const void *buf, size_t n) {
    if (fail_once && armed == FAIL_REFS_WRITE && fd_is("tagcache.refs.g", fd) && fd_is(".tmp", fd)) {
        fail_once = 0; errno = EIO; return -1;
    }
    return __real_write(fd, buf, n);
}

int __wrap_fsync(int fd) {
    if (fd_is_master_generation(fd)) master_generation_fsyncs++;
    if (fail_once && armed == FAIL_REFS_FSYNC && fd_is("tagcache.refs.g", fd) && fd_is(".tmp", fd)) {
        fail_once = 0; errno = EIO; return -1;
    }
    return __real_fsync(fd);
}

int __wrap_rename(const char *from, const char *to) {
    if (fail_once && armed == FAIL_REFS_RENAME && strstr(from, "tagcache.refs.g") && strstr(from, ".tmp")) {
        fail_once = 0; errno = EIO; return -1;
    }
    if (fail_once && armed == FAIL_POINTER_RENAME && strstr(from, "tagcache.gen.tmp") &&
        strstr(to, "tagcache.gen")) {
        fail_once = 0; errno = EIO; return -1;
    }
    return __real_rename(from, to);
}

static void must(int ok, const char *what) {
    if (!ok) { fprintf(stderr, "tagcache commit failure: %s\n", what); exit(EXIT_FAILURE); }
}

static void fixture_file(const char *path) {
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    must(fd >= 0, "create actualfile");
    must(__real_write(fd, "fixture", 7) == 7, "write actualfile");
    must(close(fd) == 0, "close actualfile");
}

static void cleanup(const char *root) {
    DIR *d = opendir(root);
    if (!d) return;
    struct dirent *e;
    char path[700];
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(path, sizeof(path), "%s/%s", root, e->d_name);
        unlink(path);
    }
    closedir(d);
    rmdir(root);
}

static void upsert(const char *path, int mtime, const char *title) {
    tagcache_upsert(path, mtime, 7, title, "Artist", "Album", "Artist", "Genre", 1, 1);
}

static void check_old(const char *path, tagcache_snapshot_t *snapshot) {
    tagcache_song_t song;
    char snap_path[TAGCACHE_PATH_MAX];
    must(tagcache_song_by_path(path, &song), "old row after failed commit");
    must(!strcmp(song.title, "titleOne"), "old title after failed commit");
    must(tagcache_snapshot_count(snapshot) == 1, "retained snapshot count");
    must(tagcache_snapshot_path_at(snapshot, 0, snap_path, sizeof(snap_path)), "retained snapshot path read");
    must(!strcmp(snap_path, path), "retained snapshot path");
}

static void run_case(enum failure_kind kind, const char *label) {
    char root[] = "/tmp/tagcache-commit-failure-XXXXXX";
    must(mkdtemp(root) != NULL, "temporary directory");
    char path[700];
    snprintf(path, sizeof(path), "%s/actualfile", root);
    fixture_file(path);
    must(tagcache_open(root), "open database");
    tagcache_begin_update();
    upsert(path, 1, "titleOne");
    must(tagcache_end_update(), "initial commit");
    int32_t generation = tagcache_generation();
    tagcache_snapshot_t *snapshot = tagcache_snapshot_open(false);
    must(snapshot != NULL, "open retained snapshot");

    tagcache_begin_update();
    upsert(path, 2, "titleTwo");
    armed = kind;
    fail_once = 1;
    must(!tagcache_end_update(), label);
    must(!fail_once, "failure injection target was not reached");
    fail_once = 0;
    check_old(path, snapshot);
    must(tagcache_generation() == generation, "generation changed after failed commit");
    tagcache_close();
    must(tagcache_open(root), "reopen after failed commit");
    must(tagcache_generation() == generation, "reopened generation changed");
    tagcache_song_t song;
    must(tagcache_song_by_path(path, &song) && !strcmp(song.title, "titleOne"), "reopened old title");
    char snapshot_path[TAGCACHE_PATH_MAX];
    must(tagcache_snapshot_path_at(snapshot, 0, snapshot_path, sizeof(snapshot_path)),
         "retained snapshot after reopen");
    must(!strcmp(snapshot_path, path), "retained snapshot path after reopen");
    /* The snapshot owns duplicated generation descriptors and remains valid
     * while the database is closed and reopened on its old generation. */
    tagcache_snapshot_close(snapshot);
    tagcache_close();
    cleanup(root);
}

static void run_master_sync_case(void) {
    char root[] = "/tmp/tagcache-master-sync-XXXXXX";
    must(mkdtemp(root) != NULL, "master sync temporary directory");
    char path[700];
    snprintf(path, sizeof(path), "%s/actualfile", root);
    fixture_file(path);
    must(tagcache_open(root), "open master sync database");
    tagcache_begin_update();
    upsert(path, 1, "titleOne");
    must(tagcache_end_update(), "initial master sync commit");

    master_generation_fsyncs = 0;
    tagcache_begin_targeted_update_with_lock(NULL, NULL);
    upsert(path, 2, "titleTwo");
    must(tagcache_end_update(), "targeted master sync commit");
    must(master_generation_fsyncs == 1, "targeted commit syncs the master once before publication");
    tagcache_close();
    cleanup(root);
}

int main(void) {
    run_case(FAIL_REFS_WRITE, "refs tmp write failure accepted");
    run_case(FAIL_REFS_FSYNC, "refs tmp fsync failure accepted");
    run_case(FAIL_REFS_RENAME, "refs rename failure accepted");
    run_case(FAIL_POINTER_RENAME, "pointer rename failure accepted");
    run_master_sync_case();
    puts("tagcache commit failure: PASS");
    return 0;
}
