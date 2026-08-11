#include "playlist_files.h"

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static bool is_m3u_file(const char * name) {
    const char * ext = strrchr(name, '.');
    if (!ext) return false;
    return strcasecmp(ext, ".m3u") == 0 || strcasecmp(ext, ".m3u8") == 0;
}

/* Same depth-first, unsorted-until-the-end approach as text_reader.c's own
 * scan_recursive() -- see its comment for why sorting per-directory would be
 * wasted work. */
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
        if (!is_m3u_file(de->d_name)) continue;

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

bool playlist_files_scan(const char * root, char *** out_paths, int * out_count) {
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

bool playlist_files_append(const char * m3u_path, const char * song_path) {
    FILE * f = fopen(m3u_path, "a");
    if (!f) return false;
    fprintf(f, "%s\n", song_path);
    fclose(f);
    return true;
}

/* Trims a trailing \n and/or \r in place -- fgets() below always leaves one
 * on any line short enough to fit in the buffer, and playlist_files_append()/
 * playlist_files_create() only ever write bare \n themselves, but a file
 * dropped onto the SD card by hand could have \r\n line endings. */
static void trim_eol(char * line) {
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';
}

bool playlist_files_contains(const char * m3u_path, const char * song_path) {
    FILE * f = fopen(m3u_path, "r");
    if (!f) return false;

    char line[PATH_MAX];
    bool found = false;
    while (fgets(line, sizeof(line), f)) {
        trim_eol(line);
        if (strcmp(line, song_path) == 0) {
            found = true;
            break;
        }
    }
    fclose(f);
    return found;
}

bool playlist_files_remove(const char * m3u_path, const char * song_path) {
    FILE * f = fopen(m3u_path, "r");
    if (!f) return false;

    char tmp_path[PATH_MAX + 8];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", m3u_path);
    FILE * out = fopen(tmp_path, "w");
    if (!out) {
        fclose(f);
        return false;
    }

    char line[PATH_MAX];
    while (fgets(line, sizeof(line), f)) {
        trim_eol(line);
        if (line[0] == '\0') continue; /* drop blank lines while rewriting anyway */
        if (strcmp(line, song_path) == 0) continue; /* the entry being removed */
        fprintf(out, "%s\n", line);
    }
    fclose(f);
    fclose(out);

    rename(tmp_path, m3u_path); /* atomic replace on POSIX -- no window where m3u_path is missing or half-written */
    return true;
}

bool playlist_files_create(const char * dir, const char * name, const char * song_path, char * out_path,
                            size_t out_path_size) {
    if (name[0] == '\0') return false;

    mkdir(dir, 0755); /* ignore EEXIST -- same "best effort, check the open" pattern as peq.c's profiles dir */

    char path[512];
    snprintf(path, sizeof(path), "%s/%s.m3u", dir, name);

    FILE * f = fopen(path, "w");
    if (!f) return false;
    fprintf(f, "%s\n", song_path);
    fclose(f);

    if (out_path) snprintf(out_path, out_path_size, "%s", path);
    return true;
}
