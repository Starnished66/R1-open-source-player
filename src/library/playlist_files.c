#include "playlist_files.h"
#include "path_cache.h"

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Audit finding: playlist_files_append() writes directly into the live
 * .m3u file, while playlist_files_remove() reads the whole file, writes a
 * filtered copy to a temp file, and atomically rename()s it over the
 * original -- with nothing serializing the two. The GUI thread and the
 * remote-control HTTP server thread (see remote_control.c) both reach
 * these functions, so a remove() that started reading before a concurrent
 * append()'s write lands can rename its own (now-stale) snapshot over the
 * file afterward, silently discarding the just-appended line with no error
 * surfaced to either caller -- a real, reachable lost update, not just a
 * theoretical one. One process-wide mutex around every mutating operation
 * (append/remove/create/delete) is simple and correct: these are rare,
 * user-initiated edits, not a hot path, so serializing across all
 * playlists rather than per-file is a fine trade for not having to manage
 * per-path lock lifetimes. playlist_files_migrate_to_relative() doesn't
 * need it -- its own doc comment guarantees it only ever runs once, before
 * any other thread could be touching playlists. */
static pthread_mutex_t playlist_files_mutex = PTHREAD_MUTEX_INITIALIZER;

bool playlist_files_has_active_write(void) {
    if (pthread_mutex_trylock(&playlist_files_mutex) != 0) return true;
    pthread_mutex_unlock(&playlist_files_mutex);
    return false;
}

static bool is_m3u_file(const char * name) {
    const char * ext = strrchr(name, '.');
    if (!ext) return false;
    return strcasecmp(ext, ".m3u") == 0 || strcasecmp(ext, ".m3u8") == 0;
}

/* Directory a file lives in, i.e. everything before its last '/' -- same
 * split every m3u_path-consuming function here needs, pulled out once
 * rather than reimplemented per call site. */
static void dir_of(const char * path, char * out, size_t out_size) {
    const char * slash = strrchr(path, '/');
    size_t len = slash ? (size_t) (slash - path) : 0;
    if (len >= out_size) len = out_size - 1;
    memcpy(out, path, len);
    out[len] = '\0';
}

#define NORMALIZE_MAX_COMPONENTS 64

/* Collapses "x/../" pairs in place, purely as string manipulation (no
 * syscalls, no dependency on the target actually existing). Necessary
 * because playlist_files_resolve_path()'s naive dir+"/"+line join for a
 * relative entry produces a path like ".../Playlists/../Ghost/song.flac"
 * -- the kernel resolves that fine for actual file I/O, but every plain
 * strcmp() against a "clean" absolute path built elsewhere (all_songs_paths[i]
 * via a straightforward directory-scan join, playlist_files_contains()/
 * _remove()'s own song_path argument, remote_control.c's library-path
 * matching) would never match it otherwise -- confirmed by writing a
 * standalone test harness against this exact file before wiring it into
 * the app. A leading "." token (HOST_BUILD's MUSIC_ROOT_DIR is the
 * cwd-relative "./music", not an absolute path) is preserved as a real
 * component rather than stripped, so the result still byte-matches
 * whatever this app's own directory-scan code would have built for the
 * same file on host, not just on target. */
static void normalize_path(char * path, size_t path_size) {
    char copy[PATH_MAX];
    snprintf(copy, sizeof(copy), "%s", path);
    bool absolute = (copy[0] == '/');

    const char * parts[NORMALIZE_MAX_COMPONENTS];
    int count = 0;
    char * saveptr;
    for (char * tok = strtok_r(copy, "/", &saveptr); tok; tok = strtok_r(NULL, "/", &saveptr)) {
        if (strcmp(tok, "..") == 0 && count > 0 && strcmp(parts[count - 1], "..") != 0 &&
            strcmp(parts[count - 1], ".") != 0) {
            count--;
            continue;
        }
        if (count < NORMALIZE_MAX_COMPONENTS) parts[count++] = tok;
    }

    size_t used = 0;
    if (absolute) {
        int n = snprintf(path, path_size, "/");
        if (n > 0) used = (size_t) n;
    } else {
        path[0] = '\0';
    }
    for (int i = 0; i < count; i++) {
        int n = snprintf(path + used, path_size - used, "%s%s", parts[i], (i + 1 < count) ? "/" : "");
        if (n < 0 || (size_t) n >= path_size - used) break;
        used += (size_t) n;
    }
}

/* fsync()s a directory's own inode -- needed after a rename() into it for
 * the rename itself to survive an unclean shutdown, same "write tmp ->
 * fsync tmp -> rename -> fsync directory" recipe settings.c's own
 * settings_save() uses (added there after a real data-loss incident on
 * this device's UBIFS partition from an fclose()+rename() alone). */
static void fsync_dir(const char * dir_path) {
    int dir_fd = open(dir_path, O_RDONLY);
    if (dir_fd < 0) return;
    fsync(dir_fd);
    close(dir_fd);
}

#define MAKE_RELATIVE_MAX_COMPONENTS 64

/* Computes target_abs_path relative to base_dir (both must be absolute --
 * true for every path this app generates itself, always built from
 * MUSIC_ROOT_DIR + real scanned directory entries, never containing "."/
 * ".." components). Splits both on '/' into components, finds the longest
 * shared prefix, emits one "../" per remaining base_dir component, then
 * appends target_abs_path's own remaining components. Returns false (out
 * left untouched) if the two paths share no common directory at all --
 * every caller here falls back to writing target_abs_path unchanged in
 * that case, so a playlist entry is never left broken, just not relative.
 * strtok_r (not strtok) since remote_control.c's HTTP server thread and
 * the main GUI thread can both reach these functions.
 *
 * No leading-'/' requirement -- deliberately works on any two paths
 * expressed the same way, not just genuinely absolute ones, since
 * HOST_BUILD's own MUSIC_ROOT_DIR ("./music") is cwd-relative, not
 * absolute; every path this app builds from it (and every m3u_path's own
 * directory) shares that same non-absolute root, so the component-split
 * below still finds a correct common prefix either way. */
static bool make_relative_path(const char * base_dir, const char * target_abs_path, char * out, size_t out_size) {
    char base_copy[PATH_MAX];
    char target_copy[PATH_MAX];
    snprintf(base_copy, sizeof(base_copy), "%s", base_dir);
    snprintf(target_copy, sizeof(target_copy), "%s", target_abs_path);

    const char * base_parts[MAKE_RELATIVE_MAX_COMPONENTS];
    const char * target_parts[MAKE_RELATIVE_MAX_COMPONENTS];
    int base_count = 0, target_count = 0;
    char * saveptr;

    for (char * tok = strtok_r(base_copy, "/", &saveptr); tok && base_count < MAKE_RELATIVE_MAX_COMPONENTS;
         tok = strtok_r(NULL, "/", &saveptr)) {
        base_parts[base_count++] = tok;
    }
    for (char * tok = strtok_r(target_copy, "/", &saveptr); tok && target_count < MAKE_RELATIVE_MAX_COMPONENTS;
         tok = strtok_r(NULL, "/", &saveptr)) {
        target_parts[target_count++] = tok;
    }

    int common = 0;
    while (common < base_count && common < target_count && strcmp(base_parts[common], target_parts[common]) == 0) {
        common++;
    }
    if (common == 0) return false;

    out[0] = '\0';
    size_t used = 0;
    for (int i = common; i < base_count; i++) {
        int n = snprintf(out + used, out_size - used, "../");
        if (n < 0 || (size_t) n >= out_size - used) return false;
        used += (size_t) n;
    }
    for (int i = common; i < target_count; i++) {
        int n = snprintf(out + used, out_size - used, "%s%s", target_parts[i], (i + 1 < target_count) ? "/" : "");
        if (n < 0 || (size_t) n >= out_size - used) return false;
        used += (size_t) n;
    }
    return true;
}

/* Direct children of dir_path only -- playlists live in MUSIC_ROOT_DIR/Playlists,
 * not scattered through the rest of the card. */
static void scan_dir(const char * dir_path, char *** paths, int * count, int * capacity) {
    DIR * dir = opendir(dir_path);
    if (!dir) return;

    struct dirent * de;
    while ((de = readdir(dir)) != NULL) {
        if (de->d_name[0] == '.') continue;
        if (!is_m3u_file(de->d_name)) continue;

        char full_path[PATH_MAX];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, de->d_name);

        struct stat st;
        if (stat(full_path, &st) != 0 || !S_ISREG(st.st_mode)) continue;

        if (*count == *capacity) {
            int new_capacity = *capacity ? *capacity * 2 : 64;
            char ** grown = realloc(*paths, sizeof(char *) * (size_t) new_capacity);
            if (!grown) break;
            *paths = grown;
            *capacity = new_capacity;
        }
        char * copy = strdup(full_path);
        if (!copy) break;
        (*paths)[*count] = copy;
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

    scan_dir(root, &paths, &count, &capacity);

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
    pthread_mutex_lock(&playlist_files_mutex);
    FILE * f = fopen(m3u_path, "a");
    if (!f) {
        pthread_mutex_unlock(&playlist_files_mutex);
        return false;
    }

    char dir_path[PATH_MAX];
    dir_of(m3u_path, dir_path, sizeof(dir_path));

    char rel[PATH_MAX];
    const char * line_to_write = make_relative_path(dir_path, song_path, rel, sizeof(rel)) ? rel : song_path;

    bool ok = fprintf(f, "%s\n", line_to_write) >= 0;
    if (ok) ok = fflush(f) == 0;
    if (ok) ok = fsync(fileno(f)) == 0;
    if (fclose(f) != 0) ok = false;
    pthread_mutex_unlock(&playlist_files_mutex);
    return ok;
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
        if (line[0] == '\0') continue;

        /* line may be an old-style absolute entry or a new-style relative
         * one -- resolve to a real absolute path before comparing, same as
         * every other reader in this file, so this doesn't silently stop
         * matching the moment playlist_files_append() started writing
         * relative lines. */
        char full_path[PATH_MAX];
        playlist_files_resolve_path(m3u_path, line, full_path, sizeof(full_path));
        if (strcmp(full_path, song_path) == 0) {
            found = true;
            break;
        }
    }
    fclose(f);
    return found;
}

bool playlist_files_remove(const char * m3u_path, const char * song_path) {
    pthread_mutex_lock(&playlist_files_mutex);
    FILE * f = fopen(m3u_path, "r");
    if (!f) {
        pthread_mutex_unlock(&playlist_files_mutex);
        return false;
    }

    char tmp_path[PATH_MAX + 8];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", m3u_path);
    FILE * out = fopen(tmp_path, "w");
    if (!out) {
        fclose(f);
        pthread_mutex_unlock(&playlist_files_mutex);
        return false;
    }

    char line[PATH_MAX];
    while (fgets(line, sizeof(line), f)) {
        trim_eol(line);
        if (line[0] == '\0') continue; /* drop blank lines while rewriting anyway */

        /* Same absolute-vs-relative resolve needed here as
         * playlist_files_contains() -- song_path (the caller's target to
         * remove) is always absolute, but line may now be a relative
         * entry. */
        char full_path[PATH_MAX];
        playlist_files_resolve_path(m3u_path, line, full_path, sizeof(full_path));
        if (strcmp(full_path, song_path) == 0) continue; /* the entry being removed */
        if (fprintf(out, "%s\n", line) < 0) {
            fclose(f);
            fclose(out);
            unlink(tmp_path);
            pthread_mutex_unlock(&playlist_files_mutex);
            return false;
        }
    }
    bool ok = !ferror(f);
    if (fclose(f) != 0) ok = false;
    if (ok) ok = fflush(out) == 0;
    if (ok) ok = fsync(fileno(out)) == 0;
    if (fclose(out) != 0) ok = false;

    if (ok) ok = rename(tmp_path, m3u_path) == 0;
    if (ok) {
        char dir_path[PATH_MAX];
        dir_of(m3u_path, dir_path, sizeof(dir_path));
        fsync_dir(dir_path);
    } else {
        unlink(tmp_path);
    }
    pthread_mutex_unlock(&playlist_files_mutex);
    return ok;
}

bool playlist_files_create(const char * dir, const char * name, const char * song_path, char * out_path,
                            size_t out_path_size) {
    if (name[0] == '\0') return false;

    pthread_mutex_lock(&playlist_files_mutex);

    mkdir(dir, 0755); /* ignore EEXIST -- same "best effort, check the open" pattern as peq.c's profiles dir */

    char path[512];
    int path_len = snprintf(path, sizeof(path), "%s/%s.m3u", dir, name);
    if (path_len < 0 || (size_t) path_len >= sizeof(path)) {
        pthread_mutex_unlock(&playlist_files_mutex);
        return false;
    }

    FILE * f = fopen(path, "wx");
    if (!f) {
        pthread_mutex_unlock(&playlist_files_mutex);
        return false;
    }

    char rel[PATH_MAX];
    const char * line_to_write = make_relative_path(dir, song_path, rel, sizeof(rel)) ? rel : song_path;
    bool ok = fprintf(f, "%s\n", line_to_write) >= 0;
    if (ok) ok = fflush(f) == 0;
    if (ok) ok = fsync(fileno(f)) == 0;
    if (fclose(f) != 0) ok = false;
    if (!ok) unlink(path);
    if (ok) fsync_dir(dir);
    pthread_mutex_unlock(&playlist_files_mutex);

    if (!ok) return false;
    if (out_path && snprintf(out_path, out_path_size, "%s", path) >= (int) out_path_size) return false;
    return true;
}

bool playlist_files_delete(const char * m3u_path) {
    pthread_mutex_lock(&playlist_files_mutex);
    bool ok = remove(m3u_path) == 0;
    pthread_mutex_unlock(&playlist_files_mutex);
    return ok;
}

void playlist_files_resolve_path(const char * m3u_path, const char * line, char * out_full_path, size_t out_size) {
    if (line[0] == '/') {
        snprintf(out_full_path, out_size, "%s", line);
        normalize_path(out_full_path, out_size);
        return;
    }

    char dir_path[PATH_MAX];
    dir_of(m3u_path, dir_path, sizeof(dir_path));
    snprintf(out_full_path, out_size, "%s/%s", dir_path, line);
    normalize_path(out_full_path, out_size);
}

bool playlist_files_read(const char * m3u_path, char *** out_paths, int * out_count) {
    FILE * f = fopen(m3u_path, "r");
    if (!f) return false;

    char ** paths = NULL;
    int count = 0;
    int capacity = 0;
    char line[PATH_MAX];

    while (fgets(line, sizeof(line), f)) {
        trim_eol(line);
        if (line[0] == '\0' || line[0] == '#') continue;

        char full_path[PATH_MAX];
        playlist_files_resolve_path(m3u_path, line, full_path, sizeof(full_path));

        if (count == capacity) {
            int new_capacity = capacity ? capacity * 2 : 16;
            char ** grown = realloc(paths, sizeof(char *) * (size_t) new_capacity);
            if (!grown) {
                for (int i = 0; i < count; i++) free(paths[i]);
                free(paths);
                fclose(f);
                return false;
            }
            paths = grown;
            capacity = new_capacity;
        }
        paths[count] = strdup(full_path);
        if (!paths[count]) {
            for (int i = 0; i < count; i++) free(paths[i]);
            free(paths);
            fclose(f);
            return false;
        }
        count++;
    }
    fclose(f);

    *out_paths = paths;
    *out_count = count;
    return true;
}

#define PLAYLIST_MIGRATION_MARKER_NAME ".relative_paths_migrated"

/* Rewrites one M3U file's lines to relative-to-its-own-folder, using the
 * same crash-safe "write tmp -> fsync tmp -> rename -> fsync directory"
 * recipe as settings.c's settings_save() -- these are real user playlists,
 * worth the same durability discipline. Drops blank/comment lines during
 * the rewrite, same precedent as playlist_files_remove() above. */
static bool migrate_one_file(const char * m3u_path) {
    FILE * in = fopen(m3u_path, "r");
    if (!in) return false;

    char dir_path[PATH_MAX];
    dir_of(m3u_path, dir_path, sizeof(dir_path));

    char tmp_path[PATH_MAX + 8];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", m3u_path);
    FILE * out = fopen(tmp_path, "w");
    if (!out) {
        fclose(in);
        return false;
    }

    char line[PATH_MAX];
    while (fgets(line, sizeof(line), in)) {
        trim_eol(line);
        if (line[0] == '\0' || line[0] == '#') continue;

        char full_path[PATH_MAX];
        playlist_files_resolve_path(m3u_path, line, full_path, sizeof(full_path));

        char rel[PATH_MAX];
        const char * line_to_write = make_relative_path(dir_path, full_path, rel, sizeof(rel)) ? rel : full_path;
        fprintf(out, "%s\n", line_to_write);
    }
    bool read_ok = !ferror(in);
    fclose(in);

    fflush(out);
    fsync(fileno(out));
    fclose(out);

    if (!read_ok) {
        remove(tmp_path);
        return false;
    }

    rename(tmp_path, m3u_path);
    fsync_dir(dir_path);
    return true;
}

void playlist_files_migrate_to_relative(const char * dir) {
    char marker_path[PATH_MAX];
    snprintf(marker_path, sizeof(marker_path), "%s/%s", dir, PLAYLIST_MIGRATION_MARKER_NAME);
    if (access(marker_path, F_OK) == 0) return; /* already migrated */

    char ** paths = NULL;
    int count = 0;
    bool all_ok = true;
    if (playlist_files_scan(dir, &paths, &count)) {
        for (int i = 0; i < count; i++) {
            if (!migrate_one_file(paths[i])) all_ok = false;
            free(paths[i]);
        }
        free(paths);
    }

    if (!all_ok) return; /* leave the marker unwritten -- retry on next boot */

    FILE * marker = fopen(marker_path, "w");
    if (marker) {
        fclose(marker);
        fsync_dir(dir);
    }
}

void playlist_files_index_replace(char * const * paths, int count) {
    path_cache_replace(PATH_CACHE_PLAYLISTS, paths, count);
}

void playlist_files_index_load(char *** out_paths, int * out_count) {
    path_cache_load(PATH_CACHE_PLAYLISTS, out_paths, out_count);
}

void playlist_files_index_insert(const char * path) {
    path_cache_insert(PATH_CACHE_PLAYLISTS, path);
}

void playlist_files_index_delete(const char * path) {
    path_cache_delete(PATH_CACHE_PLAYLISTS, path);
}
