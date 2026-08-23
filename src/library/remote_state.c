#include "remote_state.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef HOST_BUILD
  #define REMOTE_STATE_DIR "./.open_hiby_player"
#else
  #define REMOTE_STATE_DIR "/data/mnt/sd_0/.open_hiby_player"
#endif

#define REMOTE_STATE_FILE "remote_state.tsv"
#define REMOTE_STATE_PATH_MAX 600
#define REMOTE_STATE_MAX 4096

typedef struct {
    char * path;
    int32_t rating;
    int32_t playcount;
    int32_t last_played;
} rs_entry_t;

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static rs_entry_t * entries;
static int entry_n;
static int entry_cap;
static bool loaded;

static int find_index(const char * path) {
    if (!path) return -1;
    for (int i = 0; i < entry_n; i++) {
        if (strcmp(entries[i].path, path) == 0) return i;
    }
    return -1;
}

static void free_all(void) {
    for (int i = 0; i < entry_n; i++) free(entries[i].path);
    free(entries);
    entries = NULL;
    entry_n = 0;
    entry_cap = 0;
}

static void load_file(void) {
    free_all();
    loaded = true;
    char path[640];
    snprintf(path, sizeof(path), "%s/%s", REMOTE_STATE_DIR, REMOTE_STATE_FILE);
    FILE * f = fopen(path, "r");
    if (!f) return;
    char line[REMOTE_STATE_PATH_MAX + 64];
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
        if (n == 0) continue;
        int32_t rating = 0, playcount = 0, last_played = 0;
        char * rest = line;
        rating = (int32_t) strtol(rest, &rest, 10);
        if (rest == line) continue;
        while (*rest == ' ' || *rest == '\t') rest++;
        char * next = rest;
        playcount = (int32_t) strtol(rest, &rest, 10);
        if (rest == next) continue;
        while (*rest == ' ' || *rest == '\t') rest++;
        next = rest;
        last_played = (int32_t) strtol(rest, &rest, 10);
        if (rest == next) continue;
        while (*rest == ' ' || *rest == '\t') rest++;
        if (!*rest || strlen(rest) >= REMOTE_STATE_PATH_MAX) continue;
        if (entry_n >= REMOTE_STATE_MAX) break;
        if (entry_n >= entry_cap) {
            int cap = entry_cap ? entry_cap * 2 : 16;
            rs_entry_t * nent = realloc(entries, sizeof(*nent) * (size_t) cap);
            if (!nent) break;
            entries = nent;
            entry_cap = cap;
        }
        entries[entry_n].path = strdup(rest);
        if (!entries[entry_n].path) break;
        entries[entry_n].rating = rating;
        entries[entry_n].playcount = playcount;
        entries[entry_n].last_played = last_played;
        entry_n++;
    }
    fclose(f);
}

static void ensure_loaded(void) {
    if (!loaded) load_file();
}

static void save_file(void) {
    mkdir(REMOTE_STATE_DIR, 0755);
    char path[640], tmp[640];
    snprintf(path, sizeof(path), "%s/%s", REMOTE_STATE_DIR, REMOTE_STATE_FILE);
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int) sizeof(tmp)) return;
    FILE * f = fopen(tmp, "w");
    if (!f) return;
    bool ok = true;
    for (int i = 0; i < entry_n; i++) {
        if (fprintf(f, "%d %d %d %s\n", entries[i].rating, entries[i].playcount, entries[i].last_played,
                    entries[i].path) < 0) {
            ok = false;
            break;
        }
    }
    if (fflush(f) != 0) ok = false;
    if (ok && fsync(fileno(f)) != 0) ok = false;
    if (fclose(f) != 0) ok = false;
    if (!ok) {
        unlink(tmp);
        return;
    }
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return;
    }
    int dfd = open(REMOTE_STATE_DIR, O_RDONLY | O_DIRECTORY);
    if (dfd >= 0) {
        fsync(dfd);
        close(dfd);
    }
}

static rs_entry_t * ensure_entry(const char * path) {
    int i = find_index(path);
    if (i >= 0) return &entries[i];
    if (!path || !path[0] || strlen(path) >= REMOTE_STATE_PATH_MAX) return NULL;
    if (entry_n >= REMOTE_STATE_MAX) return NULL;
    if (entry_n >= entry_cap) {
        int cap = entry_cap ? entry_cap * 2 : 16;
        rs_entry_t * nent = realloc(entries, sizeof(*nent) * (size_t) cap);
        if (!nent) return NULL;
        entries = nent;
        entry_cap = cap;
    }
    entries[entry_n].path = strdup(path);
    if (!entries[entry_n].path) return NULL;
    entries[entry_n].rating = 0;
    entries[entry_n].playcount = 0;
    entries[entry_n].last_played = 0;
    return &entries[entry_n++];
}

void remote_state_drop(void) {
    pthread_mutex_lock(&mu);
    free_all();
    loaded = false;
    pthread_mutex_unlock(&mu);
}

bool remote_state_get(const char * path, int32_t * rating, int32_t * playcount, int32_t * last_played) {
    pthread_mutex_lock(&mu);
    ensure_loaded();
    int i = find_index(path);
    bool ok = i >= 0;
    if (ok) {
        if (rating) *rating = entries[i].rating;
        if (playcount) *playcount = entries[i].playcount;
        if (last_played) *last_played = entries[i].last_played;
    }
    pthread_mutex_unlock(&mu);
    return ok;
}

void remote_state_set_rating(const char * path, int32_t rating) {
    pthread_mutex_lock(&mu);
    ensure_loaded();
    rs_entry_t * e = ensure_entry(path);
    if (e) {
        e->rating = rating;
        save_file();
    }
    pthread_mutex_unlock(&mu);
}

void remote_state_add_play(const char * path, int32_t now) {
    pthread_mutex_lock(&mu);
    ensure_loaded();
    rs_entry_t * e = ensure_entry(path);
    if (e) {
        if (e->playcount < INT32_MAX) e->playcount++;
        e->last_played = now;
        save_file();
    }
    pthread_mutex_unlock(&mu);
}

bool remote_state_take(const char * path, int32_t * rating, int32_t * playcount, int32_t * last_played) {
    pthread_mutex_lock(&mu);
    ensure_loaded();
    int i = find_index(path);
    if (i < 0) {
        pthread_mutex_unlock(&mu);
        return false;
    }
    if (rating) *rating = entries[i].rating;
    if (playcount) *playcount = entries[i].playcount;
    if (last_played) *last_played = entries[i].last_played;
    free(entries[i].path);
    memmove(&entries[i], &entries[i + 1], sizeof(entries[0]) * (size_t) (entry_n - i - 1));
    entry_n--;
    save_file();
    pthread_mutex_unlock(&mu);
    return true;
}
