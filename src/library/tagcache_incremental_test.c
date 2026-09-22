#define _GNU_SOURCE
#include "tagcache.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

ssize_t __real_write(int, const void *, size_t);
ssize_t __real_pwrite(int, const void *, size_t, off_t);
static int count_writes;
static unsigned long write_calls;
static unsigned long long write_bytes;
static char swap_root[700], swap_backup[700];
static int swapped;
ssize_t __wrap_write(int fd, const void *p, size_t n) {
    ssize_t r = __real_write(fd, p, n);
    if (count_writes && r > 0) { write_calls++; write_bytes += (unsigned long long)r; }
    return r;
}
ssize_t __wrap_pwrite(int fd, const void *p, size_t n, off_t off) {
    ssize_t r = __real_pwrite(fd, p, n, off);
    if (count_writes && r > 0) { write_calls++; write_bytes += (unsigned long long)r; }
    return r;
}

static void die(const char *what) { fprintf(stderr, "tagcache incremental: %s\n", what); exit(1); }
static void must(int ok, const char *what) { if (!ok) die(what); }
static void reset_counter(void) { write_calls = write_bytes = 0; }
static void begin_counted(void) { reset_counter(); count_writes = 1; tagcache_begin_update(); }
static void end_counted(bool expected) {
    must(tagcache_end_update(), "end update");
    count_writes = 0;
    if (expected && (write_calls || write_bytes)) die("unexpected scan writes");
}
static void make_file(const char *path) {
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    must(fd >= 0, "create fixture");
    must(write(fd, "fixture", 7) == 7, "write fixture");
    close(fd);
}
static void swap_unlock(void) {
    if (swapped) return;
    must(rename(swap_root, swap_backup) == 0, "rename pinned directory");
    must(mkdir(swap_root, 0755) == 0, "replace logical directory");
    swapped = 1;
}
static void swap_lock(void) { }
static void upsert(const char *path, int mtime, const char *title) {
    tagcache_upsert(path, mtime, 7, title, "Artist", "Album", "Artist", "Genre", 1, 1);
}
static void cleanup(const char *root) {
    DIR *d = opendir(root);
    if (!d) return;
    struct dirent *e;
    char p[700];
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(p, sizeof(p), "%s/%s", root, e->d_name);
        unlink(p);
    }
    closedir(d);
    rmdir(root);
}

int main(void) {
    char root[] = "/tmp/tagcache-incremental-XXXXXX";
    must(mkdtemp(root) != NULL, "temporary directory");
    char a[700], b[700], c[700];
    snprintf(a, sizeof(a), "%s/a.flac", root);
    snprintf(b, sizeof(b), "%s/b.flac", root);
    snprintf(c, sizeof(c), "%s/c.flac", root);
    make_file(a); make_file(b); make_file(c);
    must(tagcache_open(root), "open");

    tagcache_begin_update();
    upsert(a, 1, "Alpha"); upsert(b, 1, "Beta"); upsert(c, 1, "Gamma");
    must(tagcache_end_update(), "initial commit");
    int32_t generation = tagcache_generation();
    tagcache_song_t song;

    /* Corrupting the pointer exercises generation recovery.  A subsequent
     * lazy no-op must still publish a repaired pointer. */
    char pointer[700];
    snprintf(pointer, sizeof(pointer), "%s/tagcache.gen", root);
    int pfd = open(pointer, O_WRONLY | O_TRUNC);
    must(pfd >= 0 && close(pfd) == 0, "truncate generation pointer");
    tagcache_close();
    must(tagcache_open(root), "reopen recovered database");
    must(tagcache_get_load_outcome() == TAGCACHE_LOAD_SUCCESS_RECOVERED, "recovery outcome");
    generation = tagcache_generation();
    tagcache_begin_update();
    must(tagcache_lookup(a, 1, 7, &song), "recovery lookup a");
    must(tagcache_lookup(b, 1, 7, &song), "recovery lookup b");
    must(tagcache_lookup(c, 1, 7, &song), "recovery lookup c");
    must(tagcache_end_update(), "recovery publication");
    must(tagcache_generation() != generation, "recovery pointer not republished");

    /* The logical directory can change while an unlocked end pass stats
     * files.  Publication must fail and must never write into the replacement
     * directory. */
    snprintf(swap_root, sizeof(swap_root), "%s", root);
    snprintf(swap_backup, sizeof(swap_backup), "%s.old", root);
    swapped = 0;
    tagcache_begin_update();
    must(!tagcache_end_update_with_lock(swap_unlock, swap_lock), "identity replacement accepted");
    must(!tagcache_storage_current(), "replacement still considered current");
    tagcache_close();
    cleanup(root);
    must(rename(swap_backup, root) == 0, "restore pinned directory");
    must(tagcache_open(root), "reopen after identity cancellation");
    generation = tagcache_generation();

    /* A complete unchanged pass must not create staging or write through the
     * committed master, and must retain the same generation. */
    begin_counted();
    must(tagcache_lookup(a, 1, 7, &song), "lookup a");
    must(tagcache_lookup(b, 1, 7, &song), "lookup b");
    must(tagcache_lookup(c, 1, 7, &song), "lookup c");
    end_counted(true);
    must(tagcache_generation() == generation, "unchanged generation changed");

    /* Re-entering begin_update must preserve the seen bitmap.  Even if the
     * path disappears transiently, restoring it before end must not prune it
     * or lose the earlier seen mark. */
    begin_counted();
    must(tagcache_lookup(a, 1, 7, &song), "repeat-begin lookup");
    must(unlink(a) == 0, "temporarily remove a");
    tagcache_begin_update();
    count_writes = 0;
    make_file(a);
    count_writes = 1;
    end_counted(true);
    must(tagcache_song_by_path(a, &song), "repeat begin lost seen row");

    /* A parser mismatch still marks the row seen; it must not delete it. */
    begin_counted();
    must(!tagcache_lookup(a, 99, 7, &song), "mismatched lookup unexpectedly matched");
    end_counted(true);
    must(tagcache_song_by_path(a, &song), "mismatch deleted row");

    /* Metadata changes materialize and publish. */
    tagcache_begin_update();
    upsert(a, 2, "Alpha Revised");
    must(tagcache_end_update(), "changed commit");
    must(tagcache_song_by_path(a, &song) && !strcmp(song.title, "Alpha Revised"), "changed title missing");

    /* An actually missing file is pruned by the existing materialization path. */
    must(unlink(b) == 0, "remove b");
    tagcache_begin_update();
    must(tagcache_end_update(), "deletion commit");
    must(!tagcache_song_by_path(b, &song), "removed row retained");

    /* Abort discards a lazy session; a retry remains usable and clean. */
    tagcache_begin_update();
    must(tagcache_lookup(a, 2, 7, &song), "retry lookup");
    tagcache_abort_update();
    generation = tagcache_generation();
    begin_counted();
    must(tagcache_lookup(a, 2, 7, &song), "post-abort lookup");
    end_counted(true);
    must(tagcache_generation() == generation, "post-abort generation changed");

    /* State-only migration changes force publication from the lazy session. */
    generation = tagcache_generation();
    tagcache_begin_update();
    tagcache_set_staged_migration_state(TAGCACHE_MIGRATION_APPLIED);
    must(tagcache_end_update(), "migration state commit");
    must(tagcache_generation() != generation && tagcache_migration_state() == TAGCACHE_MIGRATION_APPLIED,
         "migration state not published");

    /* Replay exercises lazy seen lookup, then starts staging for the overlay. */
    tagcache_set_rating(a, 6);
    tagcache_set_rating(c, 5);
    tagcache_begin_update();
    tagcache_abort_update();
    tagcache_stats_snapshot_t *stats = NULL;
    must(tagcache_extract_stats(root, &stats), "extract stats");
    tagcache_begin_update();
    must(tagcache_lookup(a, 2, 7, &song), "replay lookup");
    must(tagcache_lookup(c, 1, 7, &song), "replay lookup c");
    size_t unmatched = 0;
    must(tagcache_replay_stats_allow_missing(stats, &unmatched) && unmatched == 0, "replay stats");
    must(tagcache_end_update(), "replay commit");
    tagcache_free_stats(stats);
    must(tagcache_song_by_path(a, &song) && song.rating == 6, "replayed rating missing");
    must(tagcache_song_by_path(c, &song) && song.rating == 5, "second replayed rating missing");

    tagcache_close();
    cleanup(root);
    puts("tagcache incremental: PASS");
    return 0;
}
