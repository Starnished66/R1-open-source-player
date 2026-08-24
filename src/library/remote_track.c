#include "remote_track.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REMOTE_TRACK_MAX_ENTRIES 500 /* same bound as PLUGIN_MAX_LIST_ITEMS (plugin_manager.c), the Lua-facing cap on any queue-list plugin API -- kept in sync by comment, not #include, to avoid a library->plugins dependency */

/* Guards remote_track_table/remote_track_table_count -- written from the UI
 * thread only (gui_plugin_play_remote_tracks() -> remote_track_meta_set_all()),
 * read from the UI thread AND the audio/seek-worker threads (decoder_open()
 * -> remote_track_meta_copy_for_path()). See remote_track_meta_copy_for_path()'s
 * own header comment for the exact race this closes. */
static pthread_mutex_t remote_track_mutex = PTHREAD_MUTEX_INITIALIZER;
static remote_track_meta_t * remote_track_table = NULL;
static int remote_track_table_count = 0;

bool remote_track_make_key(const char * provider, const char * track_id, char * out, size_t out_size) {
    if (!provider || !provider[0] || !track_id || !track_id[0]) return false;
    for (const char * p = provider; *p; p++) {
        if (*p == '/' || (unsigned char) *p < 0x20) return false;
    }
    for (const char * p = track_id; *p; p++) {
        if (*p == '/' || (unsigned char) *p < 0x20) return false;
    }
    int n = snprintf(out, out_size, "remote://%s/%s", provider, track_id);
    return n > 0 && (size_t) n < out_size;
}

bool remote_track_path_is_remote(const char * path) {
    return path && strncmp(path, "remote://", 9) == 0;
}

bool remote_track_meta_set_all(const remote_track_meta_t * entries, int count) {
    if (count < 0) count = 0;
    if (count > REMOTE_TRACK_MAX_ENTRIES) count = REMOTE_TRACK_MAX_ENTRIES;

    remote_track_meta_t * new_table = NULL;
    if (count > 0) {
        /* Validate every entry's key BEFORE touching any shared state or
         * allocating -- an all-or-nothing publish, never a partially
         * replaced table (a plugin's own malformed entry shouldn't be able
         * to wipe out a previously-working remote queue). */
        char key[256];
        for (int i = 0; i < count; i++) {
            if (!remote_track_make_key(entries[i].provider, entries[i].track_id, key, sizeof(key))) return false;
        }
        new_table = malloc(sizeof(remote_track_meta_t) * (size_t) count);
        if (!new_table) return false;
        memcpy(new_table, entries, sizeof(remote_track_meta_t) * (size_t) count);
    }

    pthread_mutex_lock(&remote_track_mutex);
    remote_track_meta_t * old_table = remote_track_table;
    remote_track_table = new_table;
    remote_track_table_count = new_table ? count : 0;
    pthread_mutex_unlock(&remote_track_mutex);

    /* Freed AFTER the swap, outside the lock -- any reader that was mid-
     * copy under the lock already finished (and released it) before this
     * unlock could have happened; remote_track_meta_copy_for_path() never
     * hands out a pointer that could still be in use here. */
    free(old_table);

    return true;
}

bool remote_track_meta_copy_for_path(const char * path, remote_track_meta_t * out) {
    if (!remote_track_path_is_remote(path)) return false;
    char key[256];
    bool found = false;
    pthread_mutex_lock(&remote_track_mutex);
    for (int i = 0; i < remote_track_table_count; i++) {
        if (!remote_track_make_key(remote_track_table[i].provider, remote_track_table[i].track_id, key, sizeof(key))) continue;
        if (strcmp(path, key) == 0) {
            *out = remote_track_table[i];
            found = true;
            break;
        }
    }
    pthread_mutex_unlock(&remote_track_mutex);
    return found;
}
