#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"
#include "tagcache.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SONGS 300

static void req(int ok, const char *msg) {
    if (!ok) { fprintf(stderr, "tagcache storage: %s\n", msg); exit(EXIT_FAILURE); }
}
static int sample_for(int index) { return index * 17 - 300; }
static void wav_fixture(const char *path, int16_t sample) {
    drwav_data_format fmt = {drwav_container_riff, DR_WAVE_FORMAT_PCM, 1, 8000, 16};
    drwav wav;
    req(drwav_init_file_write(&wav, path, &fmt, NULL), "create WAV");
    req(drwav_write_pcm_frames(&wav, 1, &sample) == 1, "write WAV");
    drwav_uninit(&wav);
}
static void upsert(const char *path, int generation, int index) {
    char title[32];
    snprintf(title, sizeof(title), "Song %03d", index);
    tagcache_upsert(path, generation * 1000 + index, 46, title, "Artist", "Album",
                    "Artist", "Genre", index, 1);
}
static void verify_wav(const char *path, int expected_index) {
    drwav wav; int16_t sample = 0;
    req(drwav_init_file(&wav, path, NULL), "decode snapshot WAV");
    req(drwav_read_pcm_frames_s16(&wav, 1, &sample) == 1, "read WAV sample");
    req(sample == sample_for(expected_index), "decoded sample mismatch");
    drwav_uninit(&wav);
}
static void verify_snapshot(tagcache_snapshot_t *snapshot, char paths[][TAGCACHE_PATH_MAX],
                            int decode_existing, int exact_order, const char *label) {
    char path[TAGCACHE_PATH_MAX];
    bool seen[SONGS] = {0};
    req(tagcache_snapshot_count(snapshot) == SONGS, "snapshot count");
    for (int rank = 0; rank < SONGS; rank++) {
        req(tagcache_snapshot_path_at(snapshot, rank, path, sizeof(path)), "snapshot path");
        int expected = -1;
        for (int j = 0; j < SONGS; j++) if (!strcmp(path, paths[j])) { expected = j; break; }
        req(expected >= 0 && !seen[expected], "snapshot permutation");
        seen[expected] = true;
        if (exact_order && expected != rank) {
            fprintf(stderr, "%s rank %d got %s expected %s\n", label, rank, path, paths[rank]);
            req(0, "snapshot rank/path mismatch");
        }
        if (decode_existing) verify_wav(path, expected);
    }
}
static void cleanup(const char *root) {
    DIR *dir = opendir(root);
    if (!dir) return;
    struct dirent *entry;
    char path[TAGCACHE_PATH_MAX + 64];
    while ((entry = readdir(dir)) != NULL) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        snprintf(path, sizeof(path), "%s/%s", root, entry->d_name);
        unlink(path);
    }
    closedir(dir);
    rmdir(root);
}
int main(void) {
    char root[] = "/tmp/compas-tagcache-storage-XXXXXX";
    req(mkdtemp(root) != NULL, "temporary database");
    char paths[SONGS][TAGCACHE_PATH_MAX];
    for (int i = 0; i < SONGS; i++) {
        snprintf(paths[i], sizeof(paths[i]), "%s/%03d spaced ü.wav", root, i);
        wav_fixture(paths[i], (int16_t)sample_for(i));
    }
    req(tagcache_open(root), "open database");
    tagcache_begin_update();
    for (int i = 0; i < SONGS; i++) upsert(paths[i], 1, i);
    req(tagcache_end_update(), "commit 1");
    tagcache_snapshot_t *old_title = tagcache_snapshot_open(false);
    tagcache_snapshot_t *old_recency = tagcache_snapshot_open(true);
    req(old_title && old_recency, "open retained snapshots");
    verify_snapshot(old_title, paths, 1, 1, "old-title");
    verify_snapshot(old_recency, paths, 1, 1, "old-recency");

    tagcache_set_rating(paths[123], 7);
    tagcache_add_play(paths[123], 1234);
    /* begin_update flushes the numeric queue without relying on timing. */
    tagcache_begin_update();
    tagcache_abort_update();
    tagcache_song_t song;
    req(tagcache_song_by_path(paths[123], &song), "numeric row");
    req(song.rating == 7 && song.playcount == 1 && song.last_played == 1234, "numeric values");

    /* Remove interleaved early and late slots, forcing dense remaps. */
    tagcache_begin_update();
    for (int i = 0; i < SONGS; i++) {
        if (i % 3 != 1) upsert(paths[i], 2, i); else unlink(paths[i]);
    }
    req(tagcache_end_update(), "commit 2");
    tagcache_begin_update();
    for (int i = 0; i < SONGS; i++) {
        if (i % 3 != 1 && i % 5 != 0) upsert(paths[i], 3, i); else unlink(paths[i]);
    }
    req(tagcache_end_update(), "commit 3");

    /* Recreate every removed file and publish a fourth generation. */
    for (int i = 0; i < SONGS; i++) if (access(paths[i], F_OK) != 0)
        wav_fixture(paths[i], (int16_t)sample_for(i));
    tagcache_begin_update();
    for (int i = 0; i < SONGS; i++) upsert(paths[i], 4, i);
    req(tagcache_end_update(), "commit 4");

    /* Old duplicated FDs must still expose the exact old rank/path mapping. */
    verify_snapshot(old_title, paths, 1, 1, "old-title");
    verify_snapshot(old_recency, paths, 1, 1, "old-recency");
    tagcache_snapshot_close(old_title);
    tagcache_snapshot_close(old_recency);
    tagcache_snapshot_t *current_title = tagcache_snapshot_open(false);
    tagcache_snapshot_t *current_recency = tagcache_snapshot_open(true);
    req(current_title && current_recency, "open current snapshots");
    verify_snapshot(current_title, paths, 1, 1, "current-title");
    verify_snapshot(current_recency, paths, 1, 0, "current-recency");
    tagcache_snapshot_close(current_title);
    tagcache_snapshot_close(current_recency);

    bool seen_ids[SONGS + 1] = {0};
    for (int i = 0; i < SONGS; i++) {
        req(tagcache_song_by_path(paths[i], &song), "current path lookup");
        req(song.id >= 1 && song.id <= SONGS && !seen_ids[song.id], "dense remap ids");
        seen_ids[song.id] = true;
        char expected_title[32]; snprintf(expected_title, sizeof(expected_title), "Song %03d", i);
        req(song.title && strcmp(song.title, expected_title) == 0, "current title");
    }
    tagcache_close();
    req(tagcache_open(root), "reopen current database");
    req(tagcache_live_count() == SONGS, "reopened live count");
    req(tagcache_song_by_path(paths[123], &song), "reopened numeric row");
    req(song.rating == 7 && song.playcount == 1 && song.last_played == 1234, "reopened numeric values");
    tagcache_snapshot_t *reopened = tagcache_snapshot_open(false);
    req(reopened != NULL, "reopened snapshot");
    verify_snapshot(reopened, paths, 1, 1, "reopened-title");
    tagcache_snapshot_close(reopened);
    tagcache_close();
    cleanup(root);
    puts("tagcache storage: PASS");
    return 0;
}
