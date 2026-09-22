#include "path_cache.h"
#include "library_endian.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>

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
static int cache_dirfd = -1;

static bool ensure_cache_dirfd(void) {
    if (cache_dirfd >= 0) return true;
    (void) mkdir(PATH_CACHE_DIR, 0755);
    cache_dirfd = open(PATH_CACHE_DIR, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    return cache_dirfd >= 0;
}

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

static bool path_list_add(path_list_t * list, const char * path) {
    if (!path || path_list_has(list, path)) return true;
    if (list->count >= list->cap) {
        int cap = list->cap ? list->cap * 2 : 16;
        char ** n = realloc(list->paths, sizeof(*n) * (size_t) cap);
        if (!n) return false;
        list->paths = n;
        list->cap = cap;
    }
    list->paths[list->count] = strdup(path);
    if (!list->paths[list->count]) return false;
    list->count++;
    return true;
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
    return library_is_m3u_file(path);
}

static void list_load_file(named_list_t * entry) {
    path_list_free(&entry->list);
    entry->loaded = false;
    if (!ensure_cache_dirfd()) return;
    int fd = openat(cache_dirfd, entry->name, O_RDONLY | O_CLOEXEC);
    int open_errno = fd < 0 ? errno : 0;
    FILE * f = fd >= 0 ? fdopen(fd, "r") : NULL;
    if (!f && fd >= 0) close(fd);
    if (!f) { if (open_errno == ENOENT) entry->loaded = true; return; }
    char line[PATH_CACHE_PATH_MAX];
    bool ok = true;
    while (fgets(line, sizeof(line), f)) {
        size_t raw_len = strlen(line);
        /* A record longer than the fixed reader would otherwise be split
         * into several convincing-looking cache entries.  Discard its
         * remainder and the whole record. */
        bool complete = raw_len > 0 && line[raw_len - 1] == '\n';
        if (!complete && !feof(f)) {
            int c;
            while ((c = fgetc(f)) != '\n' && c != EOF) {}
            continue;
        }
        size_t n = library_trim_eol(line);
        if (n == 0 || !path_valid_for_list(entry, line)) continue;
        if (!path_list_add(&entry->list, line)) { ok = false; break; }
    }
    if (ferror(f)) ok = false;
    fclose(f);
    if (!ok) { path_list_free(&entry->list); entry->loaded = false; return; }
    entry->loaded = true;
}

static named_list_t * ensure_loaded(const char * name) {
    named_list_t * entry = list_by_name(name);
    if (!entry) return NULL;
    if (!entry->loaded) {
        if (!ensure_cache_dirfd()) return NULL;
        list_load_file(entry);
        if (!entry->loaded) return NULL;
    }
    return entry;
}

static void list_save_file(const named_list_t * entry) {
    if (!ensure_cache_dirfd()) return;
    char tmp[NAME_MAX];
    if (snprintf(tmp, sizeof(tmp), ".%s.tmp", entry->name) >= (int) sizeof(tmp)) return;
    int fd = openat(cache_dirfd, tmp, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    FILE * f = fd >= 0 ? fdopen(fd, "w") : NULL;
    if (!f && fd >= 0) close(fd);
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
        unlinkat(cache_dirfd, tmp, 0);
        return;
    }
    if (renameat(cache_dirfd, tmp, cache_dirfd, entry->name) != 0) {
        unlinkat(cache_dirfd, tmp, 0);
        return;
    }
    (void) fsync(cache_dirfd);
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
    if (cache_dirfd >= 0) { close(cache_dirfd); cache_dirfd = -1; }
    pthread_mutex_unlock(&path_cache_mu);
}

bool path_cache_bind_directory_fd(int dirfd) {
    if (dirfd < 0) return false;
    pthread_mutex_lock(&path_cache_mu);
    bool any_loaded = false;
    for (size_t i = 0; i < sizeof(lists) / sizeof(lists[0]); i++) {
        if (lists[i].loaded) {
            any_loaded = true;
            break;
        }
    }

    /* Once a list has been loaded, its descriptor is the cache's identity.
     * Do not create or bind a directory for a later card: a failed identity
     * check must leave that card completely untouched. */
    if (any_loaded) {
        int requested = openat(dirfd, ".open_hiby_player",
                               O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (requested < 0) {
            pthread_mutex_unlock(&path_cache_mu);
            return false;
        }
        struct stat a, b;
        bool same = cache_dirfd >= 0 && fstat(cache_dirfd, &a) == 0 &&
                    fstat(requested, &b) == 0 && a.st_dev == b.st_dev &&
                    a.st_ino == b.st_ino;
        close(requested);
        pthread_mutex_unlock(&path_cache_mu);
        return same;
    }

    /* No list has been loaded yet, so this is the only point where creating
     * the per-card cache directory is allowed. */
    (void) mkdirat(dirfd, ".open_hiby_player", 0755);
    int requested = openat(dirfd, ".open_hiby_player",
                           O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (requested < 0) {
        pthread_mutex_unlock(&path_cache_mu);
        return false;
    }
    if (cache_dirfd >= 0) {
        struct stat a, b;
        bool same = fstat(cache_dirfd, &a) == 0 && fstat(requested, &b) == 0 &&
                    a.st_dev == b.st_dev && a.st_ino == b.st_ino;
        if (!same) {
            close(requested);
            pthread_mutex_unlock(&path_cache_mu);
            return false;
        }
    }
    int copy = requested;
    if (cache_dirfd >= 0) close(cache_dirfd);
    cache_dirfd = copy;
    pthread_mutex_unlock(&path_cache_mu);
    return true;
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
