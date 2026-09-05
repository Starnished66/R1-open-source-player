#include "playlist_files.h"
#include "playback_order.h"
#include "queue_resume.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void release(char ** paths, int count) {
    for (int i = 0; i < count; i++) free(paths[i]);
    free(paths);
}
static void fixture(const char * path, const char * text) {
    FILE * f = fopen(path, "w"); assert(f);
    assert(fputs(text, f) >= 0); assert(fclose(f) == 0);
}
static void test_files(void) {
    char dest[4096], renamed[4096];
    char ** paths = NULL; int count = 0;
    assert(playlist_files_create("Playlists", "Empty", NULL, dest, sizeof(dest)));
    assert(playlist_files_read_ex(dest, &paths, &count) == PLAYLIST_READ_EMPTY);
    assert(count == 0); release(paths, count);
    assert(!playlist_files_create("Playlists", "Empty", NULL, renamed, sizeof(renamed)));
    assert(!playlist_files_create("Playlists", "../escape", NULL, renamed, sizeof(renamed)));
    assert(playlist_files_rename(dest, "Renamed", renamed, sizeof(renamed)));
    assert(access(dest, F_OK) != 0 && access(renamed, F_OK) == 0);
    assert(mkdir("Playlists/Nested", 0755) == 0);
    fixture("Playlists/Nested/Test.M3U8", "\357\273\277#EXTM3U\r\n#EXTINF:1,first\r\n../same.flac\r\n#EXTINF:2,missing\r\nmissing.flac\r\n#EXTINF:3,duplicate\r\n../same.flac\r\n");
    assert(playlist_files_read_ex("Playlists/Nested/Test.M3U8", &paths, &count) == PLAYLIST_READ_OK);
    assert(count == 3 && !strcmp(paths[0], paths[2])); release(paths, count);
    assert(playlist_files_edit_entry("Playlists/Nested/Test.M3U8", 2, 0));
    FILE * f = fopen("Playlists/Nested/Test.M3U8", "r"); assert(f);
    char buf[1024] = {0}; assert(fread(buf, 1, sizeof(buf) - 1, f) > 0); fclose(f);
    assert(strstr(buf, "duplicate") < strstr(buf, "first"));
    assert(strstr(buf, "missing"));
    assert(playlist_files_edit_entry("Playlists/Nested/Test.M3U8", 0, -1));
    assert(playlist_files_read("Playlists/Nested/Test.M3U8", &paths, &count));
    assert(count == 2 && strstr(paths[1], "missing.flac")); release(paths, count);
    assert(!playlist_files_edit_entry("Playlists/Nested/Test.M3U8", 9, -1));
    assert(playlist_files_edit_entry("Playlists/Nested/Test.M3U8", 0, -1));
    assert(playlist_files_edit_entry("Playlists/Nested/Test.M3U8", 0, -1));
    assert(access("Playlists/Nested/Test.M3U8", F_OK) == 0);
    assert(playlist_files_read_ex("Playlists/Nested/Test.M3U8", &paths, &count) == PLAYLIST_READ_EMPTY);
    release(paths, count);
    assert(symlink("..", "Playlists/Nested/loop") == 0);
    assert(playlist_files_scan_complete("Playlists", &paths, &count));
    assert(count == 2); release(paths, count);
    assert(!playlist_files_scan_complete("NotMounted", &paths, &count));
    assert(paths == NULL && count == 0);
    const char * songs[] = {"Playlists/song.flac", "Playlists/song.flac"};
    assert(playlist_files_write_new("Playlists", "Saved", songs, 2, dest, sizeof(dest)));
    assert(playlist_files_read(dest, &paths, &count)); assert(count == 2); release(paths, count);
    assert(!playlist_files_write_new("Playlists", "Saved", songs, 2, dest, sizeof(dest)));
    fixture("Playlists/Invalid.m3u", "#EXTM3U\n");
    f = fopen("Playlists/Invalid.m3u", "a"); assert(f);
    for (int i = 0; i < 5000; i++) fputc('x', f);
    fclose(f);
    assert(playlist_files_read_ex("Playlists/Invalid.m3u", &paths, &count) == PLAYLIST_READ_INVALID);
    assert(paths == NULL && count == 0);
}

static void test_order(void) {
    int order[20] = {0, 1, 2, 3, 4};
    /* Album song 3, Add A, Add B, Play Next C: physical slots 3/4/5
     * are now C/A/B; album song 4 has moved to slot 6. */
    playback_order_insert(order, 5, 3, 3, 1);
    playback_order_insert(order, 6, 4, 4, 1);
    playback_order_insert(order, 7, 3, 3, 1);
    for (int i = 0; i < 8; i++) assert(order[i] == i);
    int shuffled[20] = {4, 2, 0, 3, 1};
    playback_order_insert(shuffled, 5, 3, 2, 2);
    int expected[] = {6, 2, 3, 4, 0, 5, 1};
    assert(!memcmp(shuffled, expected, sizeof(expected)));
    assert(playback_order_remove(shuffled, 7, 3) == 2);
    int removed[] = {5, 2, 3, 0, 4, 1};
    assert(!memcmp(shuffled, removed, sizeof(removed)));
}

static void test_resume(void) {
    char * paths[] = {"/music/a.flac", "/music/a.flac", "/music/missing.flac"};
    int order[] = {1, 0, 2}, continuation[] = {2, 1, 0};
    queue_resume_t state = {.paths = paths, .order = order, .continuation = continuation,
        .count = 3, .current = 0, .pending = 1, .mode = 3, .shuffle_pos = 1, .position = 12.5};
    assert(queue_resume_write("queue.bin", &state));
    queue_resume_t loaded;
    assert(queue_resume_read("queue.bin", &loaded));
    assert(loaded.count == 3 && loaded.current == 0 && loaded.pending == 1 && loaded.position == 12.5);
    assert(!strcmp(loaded.paths[0], loaded.paths[1]));
    assert(!memcmp(loaded.order, order, sizeof(order)) && !memcmp(loaded.continuation, continuation, sizeof(order)));
    queue_resume_free(&loaded);
    assert(truncate("queue.bin", 20) == 0);
    assert(!queue_resume_read("queue.bin", &loaded));
    order[1] = 1;
    assert(!queue_resume_write("queue.bin", &state));
}

int main(void) {
    char dir[] = "/tmp/hiby-playlist-test-XXXXXX";
    assert(mkdtemp(dir) && chdir(dir) == 0);
    test_files(); test_order(); test_resume();
    printf("Playlist, queue order and resume tests passed (%s)\n", dir);
    return 0;
}
