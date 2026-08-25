#include "path_cache.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef HOST_BUILD
  #define PATH_CACHE_DIR "./.open_hiby_player"
#else
  #define PATH_CACHE_DIR "/data/mnt/sd_0/.open_hiby_player"
#endif

#define PATH_CACHE_PATH_MAX 600

typedef struct {
    char ** paths;
    int count;
    int cap;
} path_list_t;

typedef struct {
    const char * name;
    path_list_t list;
    bool loaded;
} named_list_t;

static named_list_t lists[] = {
    { PATH_CACHE_PLAYLISTS, { 0 }, false },
    { PATH_CACHE_BOOKS, { 0 }, false },
    { PATH_CACHE_BOOK_FAVORITES, { 0 }, false },
};

static pthread_mutex_t path_cache_mu = PTHREAD_MUTEX_INITIALIZER;

static int cmp_path_ptr(const void * a, const void * b) {
    const char * const * pa = a;
    const char * const * pb = b;
    return strcasecmp(*pa, *pb);
}

static void path_list_free(path_list_t * list) {
    for (int i = 0; i < list->count; i++) free(list->paths[i]);
    free(list->paths);
    list->paths = NULL;
    list->count = 0;
    list->cap = 0;
}

static bool path_list_has(const path_list_t * list, const char * path) {
    if (!path) return false;
    for (int i = 0; i < list->count; i++) {
        if (strcmp(list->paths[i], path) == 0) return true;
    }
    return false;
}

static void path_list_add(path_list_t * list, const char * path) {
    if (!path || path_list_has(list, path)) return;
    if (list->count >= list->cap) {
        int cap = list->cap ? list->cap * 2 : 16;
        char ** n = realloc(list->paths, sizeof(*n) * (size_t) cap);
        if (!n) return;
        list->paths = n;
        list->cap = cap;
    }
    list->paths[list->count] = strdup(path);
    if (!list->paths[list->count]) return;
    list->count++;
}

static void path_list_remove(path_list_t * list, const char * path) {
    for (int i = 0; i < list->count; i++) {
        if (strcmp(list->paths[i], path) != 0) continue;
        free(list->paths[i]);
        memmove(&list->paths[i], &list->paths[i + 1], sizeof(char *) * (size_t) (list->count - i - 1));
        list->count--;
        return;
    }
}

static void cache_path(char * out, size_t out_size, const char * name) {
    snprintf(out, out_size, "%s/%s", PATH_CACHE_DIR, name);
}

static named_list_t * list_by_name(const char * name) {
    for (size_t i = 0; i < sizeof(lists) / sizeof(lists[0]); i++) {
        if (strcmp(lists[i].name, name) == 0) return &lists[i];
    }
    return NULL;
}

static bool path_valid_for_list(const named_list_t * entry, const char * path) {
    if (!entry || !path || !path[0]) return false;
    if (strcmp(entry->name, PATH_CACHE_PLAYLISTS) != 0) return true;

    /* playlists.list is an index of playlist files, never song/database
     * rows.  Older interrupted database builds could leave unrelated paths
     * in this sidecar; rejecting them while loading makes those devices
     * self-heal instead of presenting songs/albums as playlist rows forever. */
    const char * ext = strrchr(path, '.');
    return ext && (strcasecmp(ext, ".m3u") == 0 || strcasecmp(ext, ".m3u8") == 0);
}

static void list_load_file(named_list_t * entry) {
    path_list_free(&entry->list);
    entry->loaded = true;
    char path[640];
    cache_path(path, sizeof(path), entry->name);
    FILE * f = fopen(path, "r");
    if (!f) return;
    char line[PATH_CACHE_PATH_MAX];
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        /* A record longer than the fixed reader would otherwise be split
         * into several convincing-looking cache entries.  Discard its
         * remainder and the whole record. */
        bool complete = n > 0 && line[n - 1] == '\n';
        if (!complete && !feof(f)) {
            int c;
            while ((c = fgetc(f)) != '\n' && c != EOF) {}
            continue;
        }
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
        if (n == 0 || !path_valid_for_list(entry, line)) continue;
        path_list_add(&entry->list, line);
    }
    fclose(f);
}

static named_list_t * ensure_loaded(const char * name) {
    named_list_t * entry = list_by_name(name);
    if (!entry) return NULL;
    if (!entry->loaded) list_load_file(entry);
    return entry;
}

static void list_save_file(const named_list_t * entry) {
    mkdir(PATH_CACHE_DIR, 0755);
    char path[640], tmp[640];
    cache_path(path, sizeof(path), entry->name);
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int) sizeof(tmp)) return;
    FILE * f = fopen(tmp, "w");
    if (!f) return;
    bool ok = true;
    for (int i = 0; i < entry->list.count; i++) {
        if (fprintf(f, "%s\n", entry->list.paths[i]) < 0) {
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
    int dfd = open(PATH_CACHE_DIR, O_RDONLY | O_DIRECTORY);
    if (dfd >= 0) {
        fsync(dfd);
        close(dfd);
    }
}

static void dup_sorted(const path_list_t * list, const path_list_t * filter, char *** out_paths, int * out_count) {
    *out_paths = NULL;
    *out_count = 0;
    int n = 0;
    for (int i = 0; i < list->count; i++) {
        if (filter && !path_list_has(filter, list->paths[i])) continue;
        n++;
    }
    if (n <= 0) return;
    char ** paths = malloc(sizeof(*paths) * (size_t) n);
    if (!paths) return;
    int w = 0;
    for (int i = 0; i < list->count; i++) {
        if (filter && !path_list_has(filter, list->paths[i])) continue;
        paths[w] = strdup(list->paths[i]);
        if (!paths[w]) {
            for (int j = 0; j < w; j++) free(paths[j]);
            free(paths);
            return;
        }
        w++;
    }
    qsort(paths, (size_t) w, sizeof(*paths), cmp_path_ptr);
    *out_paths = paths;
    *out_count = w;
}

void path_cache_drop(void) {
    pthread_mutex_lock(&path_cache_mu);
    for (size_t i = 0; i < sizeof(lists) / sizeof(lists[0]); i++) {
        path_list_free(&lists[i].list);
        lists[i].loaded = false;
    }
    pthread_mutex_unlock(&path_cache_mu);
}

void path_cache_replace(const char * name, char * const * paths, int count) {
    pthread_mutex_lock(&path_cache_mu);
    named_list_t * entry = ensure_loaded(name);
    if (entry) {
        path_list_free(&entry->list);
        for (int i = 0; i < count; i++) {
            if (path_valid_for_list(entry, paths[i])) path_list_add(&entry->list, paths[i]);
        }
        list_save_file(entry);
    }
    pthread_mutex_unlock(&path_cache_mu);
}

void path_cache_load(const char * name, char *** out_paths, int * out_count) {
    pthread_mutex_lock(&path_cache_mu);
    named_list_t * entry = ensure_loaded(name);
    if (!entry) {
        *out_paths = NULL;
        *out_count = 0;
        pthread_mutex_unlock(&path_cache_mu);
        return;
    }
    dup_sorted(&entry->list, NULL, out_paths, out_count);
    pthread_mutex_unlock(&path_cache_mu);
}

void path_cache_load_matching(const char * name, const char * filter_name, char *** out_paths, int * out_count) {
    pthread_mutex_lock(&path_cache_mu);
    named_list_t * entry = ensure_loaded(name);
    named_list_t * filter = ensure_loaded(filter_name);
    if (!entry || !filter) {
        *out_paths = NULL;
        *out_count = 0;
        pthread_mutex_unlock(&path_cache_mu);
        return;
    }
    dup_sorted(&entry->list, &filter->list, out_paths, out_count);
    pthread_mutex_unlock(&path_cache_mu);
}

void path_cache_insert(const char * name, const char * path) {
    pthread_mutex_lock(&path_cache_mu);
    named_list_t * entry = ensure_loaded(name);
    if (entry && path_valid_for_list(entry, path)) {
        path_list_add(&entry->list, path);
        list_save_file(entry);
    }
    pthread_mutex_unlock(&path_cache_mu);
}

void path_cache_delete(const char * name, const char * path) {
    pthread_mutex_lock(&path_cache_mu);
    named_list_t * entry = ensure_loaded(name);
    if (entry) {
        path_list_remove(&entry->list, path);
        list_save_file(entry);
    }
    pthread_mutex_unlock(&path_cache_mu);
}

bool path_cache_has(const char * name, const char * path) {
    pthread_mutex_lock(&path_cache_mu);
    named_list_t * entry = ensure_loaded(name);
    bool has = entry && path_list_has(&entry->list, path);
    pthread_mutex_unlock(&path_cache_mu);
    return has;
}
