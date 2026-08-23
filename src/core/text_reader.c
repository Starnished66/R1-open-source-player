#include "text_reader.h"
#include "path_cache.h"

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static bool is_txt_file(const char * name) {
    const char * ext = strrchr(name, '.');
    return ext && strcasecmp(ext, ".txt") == 0;
}

/* Same depth-first, unsorted-until-the-end approach as
 * file_browser.c's scan_all_songs_recursive() -- see its own comment for
 * why sorting per-directory would be wasted work. */
static void scan_recursive(const char * dir_path, char *** paths, int * count, int * capacity) {
    DIR * dir = opendir(dir_path);
    if (!dir) return;

    struct dirent * de;
    while ((de = readdir(dir)) != NULL) {
        if (de->d_name[0] == '.') continue;

        char full_path[PATH_MAX];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, de->d_name);

        struct stat st;
        if (stat(full_path, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            scan_recursive(full_path, paths, count, capacity);
            continue;
        }
        if (!is_txt_file(de->d_name)) continue;

        if (*count == *capacity) {
            *capacity = *capacity ? *capacity * 2 : 64;
            *paths = realloc(*paths, sizeof(char *) * (size_t) *capacity);
        }
        (*paths)[*count] = strdup(full_path);
        (*count)++;
    }

    closedir(dir);
}

static int compare_paths(const void * a, const void * b) {
    const char * const * pa = (const char * const *) a;
    const char * const * pb = (const char * const *) b;
    return strcasecmp(*pa, *pb);
}

bool text_reader_scan_txt_files(const char * root, char *** out_paths, int * out_count) {
    char ** paths = NULL;
    int count = 0;
    int capacity = 0;

    scan_recursive(root, &paths, &count, &capacity);

    if (count == 0) {
        free(paths);
        return false;
    }

    qsort(paths, (size_t) count, sizeof(char *), compare_paths);
    *out_paths = paths;
    *out_count = count;
    return true;
}

char * text_reader_load(const char * path, bool * out_truncated) {
    FILE * f = fopen(path, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0) size = 0;

    size_t read_size = (size_t) size;
    bool truncated = false;
    if (read_size > TEXT_READER_MAX_BYTES) {
        read_size = TEXT_READER_MAX_BYTES;
        truncated = true;
    }

    char * buf = malloc(read_size + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }

    size_t got = fread(buf, 1, read_size, f);
    fclose(f);
    buf[got] = '\0';

    if (out_truncated) *out_truncated = truncated;
    return buf;
}

void text_reader_index_replace(char * const * paths, int count) {
    path_cache_replace(PATH_CACHE_BOOKS, paths, count);
}

void text_reader_index_load(char *** out_paths, int * out_count) {
    path_cache_load(PATH_CACHE_BOOKS, out_paths, out_count);
}

bool text_reader_favorite_is_set(const char * path) {
    return path_cache_has(PATH_CACHE_BOOK_FAVORITES, path);
}

void text_reader_favorite_set(const char * path, bool is_favorite) {
    if (is_favorite) path_cache_insert(PATH_CACHE_BOOK_FAVORITES, path);
    else path_cache_delete(PATH_CACHE_BOOK_FAVORITES, path);
}

void text_reader_load_favorites(char *** out_paths, int * out_count) {
    path_cache_load_matching(PATH_CACHE_BOOK_FAVORITES, PATH_CACHE_BOOKS, out_paths, out_count);
}
