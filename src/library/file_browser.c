#include <stdatomic.h>
#include "file_browser.h"
#include "assets.h"
#include "screen_builders.h" /* STATUS_BAR_CLEARANCE / TITLE_ROW_HEIGHT / LIST_ROW_* */
#include "playlist_files.h" /* playlist_files_resolve_path() -- shared M3U line resolution */
#include "library_endian.h"
#include "fallback_font.h"

#include <dirent.h>
#include <sys/stat.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>

typedef struct {
    char name[256];
    bool is_dir;
    bool is_playlist;
    bool is_cue;
    int32_t playable_ordinal;
} dir_entry_t;

struct file_browser_index {
    int fd;
    int playable_fd;
    int directory_fd;
    dir_entry_t *memory_rows;
    dir_entry_t *memory_playable_rows;
    bool memory_backed;
    unsigned count;
    unsigned playable_count;
    char directory[PATH_MAX];
};

#ifndef INDEX_RUN_SIZE
#define INDEX_RUN_SIZE 256
#endif
#define INDEX_MEMORY_MAX_ROWS 4096


static char root_dir[PATH_MAX];
static char current_dir[PATH_MAX];
#define FILE_BROWSER_PAGE_SIZE 64
static dir_entry_t visible_entries[FILE_BROWSER_PAGE_SIZE];
static dir_entry_t * entries = visible_entries;
static int entry_count = 0;
/* `entries` is only the visible page. The full directory stays in current_index. */
static int page_start = 0;
static file_browser_index_t * current_index;
static pthread_mutex_t index_worker_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_t index_worker_thread;
static atomic_bool index_worker_running;
static atomic_bool index_worker_cancel;
static atomic_bool index_worker_error;
static atomic_bool index_worker_oversized;
static unsigned index_request_generation;
static file_browser_index_t * index_worker_result;
static unsigned index_worker_result_generation;
static char index_worker_directory[PATH_MAX];
static bool index_worker_include_cue;
static unsigned index_error_generation;
static unsigned index_error_rendered_generation;
static lv_timer_t * index_poll_timer;
static void browser_delete_cb(lv_event_t *event);

static lv_obj_t * path_label;
static lv_obj_t * list;
static file_browser_select_cb_t select_cb;
static file_browser_cue_select_cb_t cue_select_cb;
static file_browser_index_select_cb_t index_select_cb;
static bool is_playable_file(const char * name);
static bool is_cue_file(const char * name);
static bool index_entry_at(const file_browser_index_t *index, unsigned ordinal, dir_entry_t *out);
static bool index_row_playable(const dir_entry_t *row);

/* Snapshot of the directory + raw on-screen row (entries[] index, not the
 * file-only position select_cb receives) a file/playlist was last tapped
 * from -- set in entry_click_cb() right before invoking select_cb(), for
 * gui.c's player "List" option to later reopen the browser at that same
 * spot. */
static char last_selected_dir[PATH_MAX];
static int last_selected_row = -1;

static void rebuild_list(void);
static void scan_current_dir(void);

/* Where each ancestor's list was when a folder was opened from it, so going
 * back up returns to the same page and scroll offset instead of the top.
 * Deeper than the stack, back simply starts at the top. The listing loads
 * asynchronously, so the offset is applied once the parent's index arrives
 * (restore_generation ties it to that exact request). */
#define FILE_BROWSER_POSITION_STACK 32
typedef struct {
    int page_start;
    int32_t scroll_y;
} browser_position_t;
static browser_position_t position_stack[FILE_BROWSER_POSITION_STACK];
static int position_depth;
static int position_overflow;
static bool restore_pending;
static int32_t restore_scroll_y;
static unsigned restore_generation;

/* Kept in sync with audio.c's decoder dispatch. */
static const char * const PLAYABLE_EXTENSIONS[] = {
    ".flac", ".mp3", ".wav", ".aiff", ".aif", ".dsf", ".dff", ".aac", ".m4a", ".m4b", ".ape", ".wma", ".opus", ".ogg",
};

static bool is_playable_file(const char * name) {
    const char * ext = strrchr(name, '.');
    if (!ext) return false;
    for (size_t i = 0; i < sizeof(PLAYABLE_EXTENSIONS) / sizeof(PLAYABLE_EXTENSIONS[0]); i++) {
        if (strcasecmp(ext, PLAYABLE_EXTENSIONS[i]) == 0) return true;
    }
    return false;
}


static bool is_cue_file(const char * name) {
    const char * ext = strrchr(name, '.');
    if (!ext) return false;
    return strcasecmp(ext, ".cue") == 0;
}

static int compare_entries(const void * a, const void * b) {
    const dir_entry_t * ea = (const dir_entry_t *) a;
    const dir_entry_t * eb = (const dir_entry_t *) b;
    if (ea->is_dir != eb->is_dir) return ea->is_dir ? -1 : 1; /* directories before files */
    return strcasecmp(ea->name, eb->name);
}

static int index_make_entry(int directory_fd, const char *name, dir_entry_t *out, bool include_cue) {
    struct stat st;
    if (fstatat(directory_fd, name, &st, AT_SYMLINK_NOFOLLOW) != 0) return errno == ENOENT ? 0 : -1;
    if (S_ISLNK(st.st_mode)) return 0;
    bool dir = S_ISDIR(st.st_mode);
    bool playlist = !dir && library_is_m3u_file(name);
    bool cue = !dir && include_cue && is_cue_file(name);
    if (!dir && !playlist && !cue && !is_playable_file(name)) return 0;
    memset(out, 0, sizeof(*out));
    utf8_truncate_safe(out->name, name, sizeof(out->name));
    out->is_dir = dir; out->is_playlist = playlist; out->is_cue = cue;
    return 1;
}

static int index_temp_fd(int directory_fd) {
    char name[64];
    for (unsigned attempt = 0; attempt < 100; attempt++) {
        snprintf(name, sizeof(name), ".compas_files_%ld_%u", (long)getpid(), attempt);
        int fd = openat(directory_fd, name, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd >= 0) { if (unlinkat(directory_fd, name, 0) != 0) { close(fd); return -1; } return fd; }
        if (errno != EEXIST) return -1;
    }
    return -1;
}

static int index_read_record(int fd, dir_entry_t *row) {
    size_t got = 0;
    while (got < sizeof(*row)) {
        ssize_t n = read(fd, (char *)row + got, sizeof(*row) - got);
        if (n > 0) { got += (size_t)n; continue; }
        if (n == 0 && got == 0) return 1;
        if (n < 0 && errno == EINTR) continue;
        return -1;
    }
    return 0;
}

static bool index_write_full(int fd, const void *data, size_t size) {
    const char *p = data;
    while (size) {
        ssize_t n = write(fd, p, size);
        if (n > 0) { p += n; size -= (size_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

static int index_write_run(int directory_fd, dir_entry_t *rows, size_t count) {
    qsort(rows, count, sizeof(*rows), compare_entries);
    int fd = index_temp_fd(directory_fd);
    if (fd < 0 || !index_write_full(fd, rows, count * sizeof(*rows))) {
        if (fd >= 0) close(fd); return -1;
    }
    lseek(fd, 0, SEEK_SET); return fd;
}

static int index_merge_runs(int left, int right, int directory_fd, atomic_bool *cancel) {
    int out = index_temp_fd(directory_fd);
    if (out < 0) { close(left); close(right); return -1; }
    dir_entry_t a, b;
    int na = index_read_record(left, &a), nb = index_read_record(right, &b);
    if (na < 0 || nb < 0) { close(left); close(right); close(out); return -1; }
    while (na == 0 || nb == 0) {
        if (cancel && atomic_load(cancel)) { close(left); close(right); close(out); return -1; }
        dir_entry_t *pick;
        if (nb != 0 || (na == 0 && compare_entries(&a, &b) <= 0)) pick = &a;
        else pick = &b;
        if (!index_write_full(out, pick, sizeof(*pick))) { close(left); close(right); close(out); return -1; }
        if (pick == &a) na = index_read_record(left, &a); else nb = index_read_record(right, &b);
        if (na < 0 || nb < 0) { close(left); close(right); close(out); return -1; }
    }
    close(left); close(right); lseek(out, 0, SEEK_SET); return out;
}

static bool index_cancelled(atomic_bool *cancel) {
    return cancel && atomic_load_explicit(cancel, memory_order_acquire);
}

static bool index_open_memory(const char *directory, DIR *dir, int dfd,
                              file_browser_index_t **out,
                              unsigned *out_count, bool include_cue,
                              atomic_bool *cancel) {
    dir_entry_t *rows = malloc(sizeof(*rows) * INDEX_MEMORY_MAX_ROWS);
    if (!rows) { close(dfd); closedir(dir); return false; }
    unsigned count = 0;
    bool ok = true;
    for (;;) {
        errno = 0;
        struct dirent *de = readdir(dir);
        if (!de) { if (errno) ok = false; break; }
        if (index_cancelled(cancel)) { ok = false; break; }
        if (de->d_name[0] == '.') continue;
        dir_entry_t candidate;
        int status = index_make_entry(dfd, de->d_name, &candidate, include_cue);
        if (status < 0) { ok = false; break; }
        if (status > 0) {
            if (count >= INDEX_MEMORY_MAX_ROWS) { errno = EFBIG; ok = false; break; }
            rows[count] = candidate;
            count++;
        }
    }
    int failure_errno = errno;
    closedir(dir);
    if (!ok) { free(rows); close(dfd); errno = failure_errno; return false; }
    qsort(rows, count, sizeof(*rows), compare_entries);
    file_browser_index_t *idx = calloc(1, sizeof(*idx));
    if (!idx) { free(rows); close(dfd); return false; }
    idx->fd = -1; idx->playable_fd = -1; idx->directory_fd = dfd;
    idx->memory_rows = rows;
    idx->memory_playable_rows = count ? malloc(sizeof(*rows) * count) : NULL;
    idx->memory_backed = true; idx->count = count;
    if (count && !idx->memory_playable_rows) {
        free(idx->memory_rows); free(idx); close(dfd); return false;
    }
    for (unsigned i = 0; i < count; i++) {
        idx->memory_rows[i].playable_ordinal = -1;
        if (index_row_playable(&idx->memory_rows[i])) {
            idx->memory_rows[i].playable_ordinal = (int32_t)idx->playable_count;
            idx->memory_playable_rows[idx->playable_count++] = idx->memory_rows[i];
        }
    }
    snprintf(idx->directory, sizeof(idx->directory), "%s", directory);
    *out = idx; if (out_count) *out_count = count;
    return true;
}

static bool file_browser_index_open_ex(const char *directory, file_browser_index_t **out,
                                       unsigned *out_count, bool include_cue,
                                       atomic_bool *cancel) {
    if (!directory || !out) return false;
    *out = NULL; if (out_count) *out_count = 0;
    DIR *dir = opendir(directory); if (!dir) return false;
    int dfd = dup(dirfd(dir)), levels[32];
    if (dfd < 0) { closedir(dir); return false; }
    memset(levels, -1, sizeof(levels));
    errno = 0;
    int probe = index_temp_fd(dfd);
    if (probe < 0 && (errno == EROFS || errno == EACCES || errno == EPERM)) {
        return index_open_memory(directory, dir, dfd, out, out_count, include_cue, cancel);
    }
    if (probe < 0) goto fail;
    close(probe);
    size_t row_count = 0; unsigned total = 0;
    dir_entry_t rows[INDEX_RUN_SIZE]; struct dirent *de;
    for (;;) {
        errno = 0;
        de = readdir(dir);
        if (!de) { if (errno) goto fail; break; }
        if (cancel && atomic_load(cancel)) goto fail;
        if (de->d_name[0] == '.') continue;
        int entry_status = index_make_entry(dfd, de->d_name, &rows[row_count], include_cue);
        if (entry_status < 0) goto fail;
        if (entry_status == 0) continue;
        if (++row_count == INDEX_RUN_SIZE) {
            int fd = index_write_run(dfd, rows, row_count); if (fd < 0) goto fail;
            total += (unsigned)row_count; row_count = 0;
            for (unsigned level = 0; ; level++) { if (level >= 32) { close(fd); goto fail; } if (levels[level] < 0) { levels[level] = fd; break; } int prior = levels[level]; levels[level] = -1; fd = index_merge_runs(prior, fd, dfd, cancel); if (fd < 0) goto fail; }
        }
    }
    if (row_count) { int fd = index_write_run(dfd, rows, row_count); if (fd < 0) goto fail; total += (unsigned)row_count; for (unsigned level = 0; ; level++) { if (level >= 32) { close(fd); goto fail; } if (levels[level] < 0) { levels[level] = fd; break; } int prior = levels[level]; levels[level] = -1; fd = index_merge_runs(prior, fd, dfd, cancel); if (fd < 0) goto fail; } }
    closedir(dir);
    int final = -1; for (unsigned level = 0; level < 32; level++) if (levels[level] >= 0) { int prior = levels[level]; levels[level] = -1; if (final < 0) final = prior; else { final = index_merge_runs(prior, final, dfd, cancel); if (final < 0) goto fail_closed; } }
    file_browser_index_t *idx = calloc(1, sizeof(*idx)); if (!idx) { if (final >= 0) close(final); goto fail_closed; }
    idx->fd = final >= 0 ? final : index_temp_fd(dfd); idx->count = total; idx->playable_fd = index_temp_fd(dfd); idx->directory_fd = dup(dfd); idx->playable_count = 0;
    if (idx->fd < 0 || idx->playable_fd < 0 || idx->directory_fd < 0) { if (idx->fd >= 0) close(idx->fd); if (idx->playable_fd >= 0) close(idx->playable_fd); if (idx->directory_fd >= 0) close(idx->directory_fd); free(idx); goto fail_closed; }
    dir_entry_t row;
    for (unsigned i = 0; i < idx->count; i++) {
        if (pread(idx->fd, &row, sizeof(row), (off_t)i * sizeof(row)) != (ssize_t)sizeof(row)) { close(idx->fd); close(idx->playable_fd); close(idx->directory_fd); free(idx); goto fail_closed; }
        row.playable_ordinal = -1;
        if (index_row_playable(&row)) {
            row.playable_ordinal = (int32_t)idx->playable_count;
            if (pwrite(idx->fd, &row, sizeof(row), (off_t)i * sizeof(row)) != (ssize_t)sizeof(row) ||
                !index_write_full(idx->playable_fd, &row, sizeof(row))) { close(idx->fd); close(idx->playable_fd); close(idx->directory_fd); free(idx); goto fail_closed; }
            idx->playable_count++;
        }
    }
    lseek(idx->playable_fd, 0, SEEK_SET);
    snprintf(idx->directory, sizeof(idx->directory), "%s", directory);
    close(dfd); *out = idx; if (out_count) *out_count = idx->count; return true;
fail:
    closedir(dir);
fail_closed:
    close(dfd); for (unsigned i = 0; i < 32; i++) if (levels[i] >= 0) close(levels[i]); return false;
}

bool file_browser_index_open(const char *directory, file_browser_index_t **out, unsigned *out_count) {
    return file_browser_index_open_ex(directory, out, out_count, false, NULL);
}

bool file_browser_index_retain(const file_browser_index_t *source, file_browser_index_t **out) {
    if (!source || !out) return false; file_browser_index_t *copy = calloc(1, sizeof(*copy)); if (!copy) return false;
    if (source->memory_backed) {
        copy->fd = -1; copy->playable_fd = -1;
        copy->directory_fd = dup(source->directory_fd);
        copy->count = source->count; copy->playable_count = source->playable_count;
        copy->memory_backed = true;
        copy->memory_rows = malloc(sizeof(*copy->memory_rows) * copy->count);
        copy->memory_playable_rows = malloc(sizeof(*copy->memory_playable_rows) * copy->playable_count);
        if (copy->directory_fd < 0 || (copy->count && !copy->memory_rows) ||
            (copy->playable_count && !copy->memory_playable_rows)) {
            if (copy->directory_fd >= 0) close(copy->directory_fd);
            free(copy->memory_rows); free(copy->memory_playable_rows); free(copy); return false;
        }
        memcpy(copy->memory_rows, source->memory_rows, sizeof(*copy->memory_rows) * copy->count);
        memcpy(copy->memory_playable_rows, source->memory_playable_rows,
               sizeof(*copy->memory_playable_rows) * copy->playable_count);
        snprintf(copy->directory, sizeof(copy->directory), "%s", source->directory);
        *out = copy; return true;
    }
    copy->fd = dup(source->fd); copy->playable_fd = dup(source->playable_fd); copy->directory_fd = dup(source->directory_fd);
    if (copy->fd < 0 || copy->playable_fd < 0 || copy->directory_fd < 0) { if (copy->fd >= 0) close(copy->fd); if (copy->playable_fd >= 0) close(copy->playable_fd); if (copy->directory_fd >= 0) close(copy->directory_fd); free(copy); return false; }
    copy->count = source->count; copy->playable_count = source->playable_count; snprintf(copy->directory, sizeof(copy->directory), "%s", source->directory); *out = copy; return true;
}

bool file_browser_index_path_at(const file_browser_index_t *index, unsigned ordinal, char *out_path, size_t out_size) {
    if (!index || ordinal >= index->count || !out_path || !out_size) return false;
    dir_entry_t row;
    if (index->memory_backed) row = index->memory_rows[ordinal];
    else if (pread(index->fd, &row, sizeof(row), (off_t)ordinal * sizeof(row)) != (ssize_t)sizeof(row)) return false;
    return snprintf(out_path, out_size, "%s/%s", index->directory, row.name) < (int)out_size;
}

static bool index_row_playable(const dir_entry_t *row) {
    return row && !row->is_dir && !row->is_playlist && !row->is_cue;
}

unsigned file_browser_index_playable_count(const file_browser_index_t *index) {
    if (!index) return 0;
    return index->playable_count;
}

bool file_browser_index_playable_path_at(const file_browser_index_t *index, unsigned ordinal,
                                         char *out_path, size_t out_size) {
    if (!index || !out_path || !out_size) return false;
    if (ordinal >= index->playable_count) return false;
    dir_entry_t row;
    if (index->memory_backed) row = index->memory_playable_rows[ordinal];
    else if (pread(index->playable_fd, &row, sizeof(row), (off_t)ordinal * sizeof(row)) != (ssize_t)sizeof(row)) return false;
    return snprintf(out_path, out_size, "%s/%s", index->directory, row.name) < (int)out_size;
}

int file_browser_index_dup_directory_fd(const file_browser_index_t *index) {
    return index ? dup(index->directory_fd) : -1;
}

const char *file_browser_index_directory(const file_browser_index_t *index) {
    return index ? index->directory : NULL;
}

void file_browser_index_close(file_browser_index_t *index) {
    if (!index) return;
    if (index->fd >= 0) close(index->fd);
    if (index->playable_fd >= 0) close(index->playable_fd);
    if (index->directory_fd >= 0) close(index->directory_fd);
    free(index->memory_rows); free(index->memory_playable_rows); free(index);
}

static bool index_entry_at(const file_browser_index_t *index, unsigned ordinal, dir_entry_t *out) {
    if (!index || ordinal >= index->count || !out) return false;
    if (index->memory_backed) { *out = index->memory_rows[ordinal]; return true; }
    return pread(index->fd, out, sizeof(*out), (off_t)ordinal * sizeof(*out)) == (ssize_t)sizeof(*out);
}

bool file_browser_index_entry_name(const file_browser_index_t *index, unsigned ordinal,
                                   char *name, size_t name_size, bool *is_dir) {
    dir_entry_t row;
    if (!name || name_size == 0 || !index_entry_at(index, ordinal, &row)) return false;
    if (strlen(row.name) >= name_size) return false;
    memcpy(name, row.name, strlen(row.name) + 1);
    if (is_dir) *is_dir = row.is_dir;
    return true;
}

/* Copy one screen of an already-open index. Does not rescan or allocate. */
static bool load_visible_entries(void) {
    if (entry_count < 0) return false;
    if (page_start < 0) page_start = 0;
    if (page_start >= entry_count)
        page_start = entry_count > 0 ? ((entry_count - 1) / FILE_BROWSER_PAGE_SIZE) * FILE_BROWSER_PAGE_SIZE : 0;
    if (!current_index) return entry_count == 0;
    int end = page_start + FILE_BROWSER_PAGE_SIZE;
    if (end > entry_count) end = entry_count;
    for (int i = page_start; i < end; i++) {
        if (!index_entry_at(current_index, (unsigned) i, &visible_entries[i - page_start])) return false;
    }
    return true;
}

static void free_entries(void) {
    if (current_index) { file_browser_index_close(current_index); current_index = NULL; }
    memset(visible_entries, 0, sizeof(visible_entries));
    entries = visible_entries;
    entry_count = 0;
}

/* Scans dir_path into a freshly malloc'd, sorted (dirs-first, then alpha)
 * array of playable entries. Caller owns the result (free() it). Used both
 * for the interactive browser's current directory (via scan_current_dir)
 * and for one-shot lookups that don't touch the browser's own state (e.g.
 * resuming a track without ever having opened the browser screen). */
static int scan_entry_limit(void) {
    const char *limit = getenv("FILE_BROWSER_TEST_ENTRY_LIMIT");
    if (!limit || !limit[0]) return INT_MAX;
    char *end = NULL;
    long value = strtol(limit, &end, 10);
    if (end == limit || value < 0 || value > INT_MAX) return INT_MAX;
    return (int) value;
}

/* Returns the entry count, 0 when the directory cannot be opened, and -1
 * when memory cannot hold the whole directory. A short list is never
 * reported as success. */
static int scan_directory(const char * dir_path, dir_entry_t ** out_entries) {
    DIR * dir = opendir(dir_path);
    if (!dir) {
        fprintf(stderr, "file_browser: failed to open '%s'\n", dir_path);
        *out_entries = NULL;
        return 0;
    }

    int capacity = 32;
    int count = 0;
    int limit = scan_entry_limit();
    dir_entry_t * result = malloc(sizeof(dir_entry_t) * (size_t) capacity);
    if (!result) {
        closedir(dir);
        *out_entries = NULL;
        return -1;
    }

    struct dirent * de;
    while ((de = readdir(dir)) != NULL) {
        if (de->d_name[0] == '.') continue; /* skips ".", "..", and hidden files/dirs */

        char full_path[PATH_MAX];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, de->d_name);

        bool is_dir;
        if (de->d_type == DT_DIR) {
            is_dir = true;
        } else if (de->d_type == DT_REG) {
            is_dir = false;
        } else {
            /* DT_UNKNOWN (and symlinks) are common on some filesystems. Keep
             * the stat fallback so the old target-following behavior stays
             * unchanged for those entries. */
            struct stat st;
            if (stat(full_path, &st) != 0) continue;
            is_dir = S_ISDIR(st.st_mode);
        }
        bool is_playlist = !is_dir && library_is_m3u_file(de->d_name);
        /* Only shown at all if a caller actually wants .cue sheets (see
         * file_browser_init()'s own comment) -- a caller with cue_select_cb
         * == NULL never sees them, same as any other file type this
         * browser doesn't recognize. */
        bool is_cue = !is_dir && cue_select_cb && is_cue_file(de->d_name);
        if (!is_dir && !is_playlist && !is_cue && !is_playable_file(de->d_name)) continue;

        if (count >= limit) {
            free(result);
            closedir(dir);
            *out_entries = NULL;
            return -1;
        }
        if (count == capacity) {
            dir_entry_t * grown = realloc(result, sizeof(dir_entry_t) * (size_t) (capacity * 2));
            if (!grown) {
                free(result);
                closedir(dir);
                *out_entries = NULL;
                return -1;
            }
            result = grown;
            capacity *= 2;
        }

        utf8_truncate_safe(result[count].name, de->d_name, sizeof(result[count].name));
        result[count].is_dir = is_dir;
        result[count].is_playlist = is_playlist;
        result[count].is_cue = is_cue;
        count++;
    }

    closedir(dir);

    qsort(result, (size_t) count, sizeof(dir_entry_t), compare_entries);
    *out_entries = result;
    return count;
}

static void *index_worker_main(void *arg) {
    unsigned generation = *(unsigned *)arg; free(arg);
    char directory[PATH_MAX];
    bool include_cue;
    pthread_mutex_lock(&index_worker_mu);
    snprintf(directory, sizeof(directory), "%s", index_worker_directory);
    include_cue = index_worker_include_cue;
    pthread_mutex_unlock(&index_worker_mu);
    file_browser_index_t *built = NULL; unsigned count = 0;
    bool ok = file_browser_index_open_ex(directory, &built, &count, include_cue, &index_worker_cancel);
    pthread_mutex_lock(&index_worker_mu);
    if (ok && !atomic_load(&index_worker_cancel) && generation == index_request_generation) {
        if (index_worker_result) file_browser_index_close(index_worker_result);
        index_worker_result = built; index_worker_result_generation = generation;
    } else {
        if (built) file_browser_index_close(built);
        if (!ok && generation == index_request_generation) {
            atomic_store(&index_worker_error, true);
            atomic_store(&index_worker_oversized, errno == EFBIG);
            index_error_generation = generation;
        }
    }
    atomic_store(&index_worker_running, false);
    pthread_mutex_unlock(&index_worker_mu);
    return NULL;
}

static void index_poll_cb(lv_timer_t *timer) {
    (void)timer;
    pthread_mutex_lock(&index_worker_mu);
    file_browser_index_t *ready = index_worker_result;
    unsigned generation = index_worker_result_generation;
    index_worker_result = NULL;
    bool running = atomic_load(&index_worker_running);
    pthread_mutex_unlock(&index_worker_mu);
    if (!ready || generation != index_request_generation) {
        if (ready) file_browser_index_close(ready);
        if (!running && atomic_load(&index_worker_error) &&
            index_error_generation == index_request_generation &&
            index_error_rendered_generation != index_request_generation) {
            index_error_rendered_generation = index_request_generation;
            free_entries();
            entry_count = -1;
            rebuild_list();
        }
        else if (!running && !atomic_load(&index_worker_error) && !current_index && entry_count == 0) {
            pthread_mutex_lock(&index_worker_mu);
            atomic_store(&index_worker_cancel, false);
            atomic_store(&index_worker_running, true);
            unsigned *request = malloc(sizeof(*request));
            if (request) {
                *request = index_request_generation;
                if (pthread_create(&index_worker_thread, NULL, index_worker_main, request) != 0) {
                    free(request); atomic_store(&index_worker_running, false);
                    atomic_store(&index_worker_error, true);
                    index_error_generation = index_request_generation;
                } else pthread_detach(index_worker_thread);
            } else {
                atomic_store(&index_worker_running, false);
                atomic_store(&index_worker_error, true);
                index_error_generation = index_request_generation;
            }
            pthread_mutex_unlock(&index_worker_mu);
        }
        return;
    }
    if (current_index) file_browser_index_close(current_index);
    current_index = ready; entry_count = (int)ready->count;
    if (!load_visible_entries()) {
        atomic_store(&index_worker_error, true);
        entry_count = -1;
    }
    rebuild_list();
    if (restore_pending && restore_generation == generation) {
        restore_pending = false;
        if (entry_count > 0) {
            /* Clamped by LVGL if the folder shrank since. */
            lv_obj_update_layout(list);
            lv_obj_scroll_to_y(list, restore_scroll_y, LV_ANIM_OFF);
        }
    }
    (void)running;
}

static void scan_current_dir(void) {
    free_entries();
    pthread_mutex_lock(&index_worker_mu);
    unsigned generation = ++index_request_generation;
    atomic_store(&index_worker_error, false);
    atomic_store(&index_worker_oversized, false);
    index_worker_include_cue = cue_select_cb != NULL;
    atomic_store(&index_worker_cancel, true);
    if (index_worker_result) { file_browser_index_close(index_worker_result); index_worker_result = NULL; }
    snprintf(index_worker_directory, sizeof(index_worker_directory), "%s", current_dir);
    if (!atomic_load(&index_worker_running)) {
        atomic_store(&index_worker_cancel, false);
        atomic_store(&index_worker_running, true);
        unsigned *request = malloc(sizeof(*request));
        if (request) {
            *request = generation;
            if (pthread_create(&index_worker_thread, NULL, index_worker_main, request) != 0) {
                free(request); atomic_store(&index_worker_running, false);
                atomic_store(&index_worker_error, true);
                index_error_generation = generation;
            } else pthread_detach(index_worker_thread);
        }
        else {
            atomic_store(&index_worker_running, false);
            atomic_store(&index_worker_error, true);
            index_error_generation = generation;
        }
    }
    pthread_mutex_unlock(&index_worker_mu);
    entry_count = 0;
}

static void browser_delete_cb(lv_event_t *event) {
    (void)event;
    pthread_mutex_lock(&index_worker_mu);
    ++index_request_generation;
    atomic_store(&index_worker_cancel, true);
    if (index_worker_result) { file_browser_index_close(index_worker_result); index_worker_result = NULL; }
    if (current_index) { file_browser_index_close(current_index); current_index = NULL; }
    if (index_poll_timer) { lv_timer_del(index_poll_timer); index_poll_timer = NULL; }
    list = NULL; path_label = NULL;
    pthread_mutex_unlock(&index_worker_mu);
}

/* Builds the playlist from every playable file in the current directory
 * (in the same sorted order they're displayed) and reports which position
 * within that file-only list corresponds to `file_display_index`. */
static void build_playlist_and_select(int file_display_index) {
    dir_entry_t selected_entry;
    if (!index_entry_at(current_index, (unsigned)file_display_index, &selected_entry)) return;
    if (index_select_cb) {
        unsigned playable = current_index->playable_count;
        unsigned selected = selected_entry.playable_ordinal >= 0 ? (unsigned)selected_entry.playable_ordinal : 0;
        file_browser_index_t *retained = NULL;
        if (file_browser_index_retain(current_index, &retained)) index_select_cb(retained, playable, selected);
        return;
    }
    dir_entry_t *all = NULL;
    int all_count = scan_directory(current_dir, &all);
    if (all_count <= 0) { free(all); return; }
    char ** playlist = malloc(sizeof(char *) * (size_t) all_count);
    if (!playlist) { free(all); return; }
    int count = 0;
    int selected = -1;

    for (int i = 0; i < all_count; i++) {
        if (all[i].is_dir || all[i].is_playlist || all[i].is_cue) continue;
        if (strcmp(all[i].name, selected_entry.name) == 0) selected = count;

        char full_path[PATH_MAX];
        snprintf(full_path, sizeof(full_path), "%s/%s", current_dir, all[i].name);
        playlist[count] = strdup(full_path);
        if (!playlist[count]) { for (int j = 0; j < count; j++) free(playlist[j]); free(playlist); free(all); return; }
        count++;
    }
    free(all);

    if (selected < 0) {
        for (int i = 0; i < count; i++) free(playlist[i]);
        free(playlist);
        return;
    }

    select_cb(playlist, count, selected);
}

void file_browser_set_index_select_cb(file_browser_index_select_cb_t callback) {
    index_select_cb = callback;
}

bool file_browser_build_playlist_from_m3u(const char * m3u_path, char *** out_playlist, int * out_count) {
    char ** paths = NULL;
    int count = 0;
    *out_playlist = NULL;
    *out_count = 0;
    if (!playlist_files_read(m3u_path, &paths, &count)) return false;
    int kept = 0;
    for (int i = 0; i < count; i++) {
        if (is_playable_file(paths[i])) paths[kept++] = paths[i];
        else free(paths[i]);
    }
    if (!kept) { free(paths); return false; }
    *out_playlist = paths;
    *out_count = kept;
    return true;
}

bool file_browser_at_root(void) {
    return strlen(current_dir) <= strlen(root_dir);
}

void file_browser_go_up(void) {
    char * last_slash = strrchr(current_dir, '/');
    if (last_slash && strlen(current_dir) > strlen(root_dir)) {
        *last_slash = '\0';
        if (strlen(current_dir) < strlen(root_dir)) {
            snprintf(current_dir, sizeof(current_dir), "%s", root_dir);
        }
        bool restore = false;
        if (position_overflow > 0) {
            position_overflow--;
            page_start = 0;
        } else if (position_depth > 0) {
            position_depth--;
            page_start = position_stack[position_depth].page_start;
            restore_scroll_y = position_stack[position_depth].scroll_y;
            restore = true;
        } else {
            page_start = 0;
        }
        scan_current_dir();
        restore_pending = restore;
        restore_generation = index_request_generation;
        rebuild_list();
    }
}

static void up_click_cb(lv_event_t * e) {
    (void) e;
    file_browser_go_up();
}

static void page_click_cb(lv_event_t * e) {
    int delta = (int) (intptr_t) lv_event_get_user_data(e);
    int next = page_start + delta;
    if (next < 0) next = 0;
    if (next >= entry_count) next = entry_count > 0
        ? ((entry_count - 1) / FILE_BROWSER_PAGE_SIZE) * FILE_BROWSER_PAGE_SIZE : 0;
    if (next == page_start) return;
    page_start = next;
    if (!load_visible_entries()) {
        atomic_store(&index_worker_error, true);
        entry_count = -1;
    }
    rebuild_list();
}

static void entry_click_cb(lv_event_t * e) {
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    dir_entry_t clicked;
    if (!index_entry_at(current_index, (unsigned)index, &clicked)) return;

    if (clicked.is_dir) {
        char new_dir[PATH_MAX];
        snprintf(new_dir, sizeof(new_dir), "%s/%s", current_dir, clicked.name);
        snprintf(current_dir, sizeof(current_dir), "%s", new_dir);
        if (position_depth < FILE_BROWSER_POSITION_STACK) {
            position_stack[position_depth].page_start = page_start;
            position_stack[position_depth].scroll_y = list ? lv_obj_get_scroll_y(list) : 0;
            position_depth++;
        } else {
            position_overflow++;
        }
        restore_pending = false;
        page_start = 0;
        scan_current_dir();
        rebuild_list();
    } else if (clicked.is_playlist) {
        char m3u_path[PATH_MAX];
        snprintf(m3u_path, sizeof(m3u_path), "%s/%s", current_dir, clicked.name);

        char ** playlist;
        int count;
        if (file_browser_build_playlist_from_m3u(m3u_path, &playlist, &count)) {
            snprintf(last_selected_dir, sizeof(last_selected_dir), "%s", current_dir);
            last_selected_row = index;
            select_cb(playlist, count, 0);
        }
    } else if (clicked.is_cue) {
        char cue_path[PATH_MAX];
        snprintf(cue_path, sizeof(cue_path), "%s/%s", current_dir, clicked.name);
        snprintf(last_selected_dir, sizeof(last_selected_dir), "%s", current_dir);
        last_selected_row = index;
        cue_select_cb(cue_path);
    } else {
        snprintf(last_selected_dir, sizeof(last_selected_dir), "%s", current_dir);
        last_selected_row = index;
        build_playlist_and_select(index);
    }
}

/* A single touch-list row, shared geometry/style with every other row list
 * in the app (LIST_ROW_* in screen_builders.h). `icon_asset` is NULL for a
 * plain file (just an indented label); directories and playlists each get
 * their own real icon. */
static lv_obj_t * add_file_row(const char * label_text, const char * icon_asset, lv_event_cb_t cb, void * user_data) {
    lv_obj_t * row = lv_obj_create(list);
    /* Files is a Music submenu, so it shares the roomier 100px browsing
     * density used by Artists/Albums/All Songs; Settings stays at the
     * shared 84px default. */
    lv_obj_set_size(row, LIST_ROW_WIDTH_WIDE, MUSIC_LIST_ROW_HEIGHT);
    lv_obj_add_style(row, &pill_row_bg_style, 0);
    lv_obj_add_style(row, &list_row_pressed_style, LV_STATE_PRESSED);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * label = lv_label_create(row);
    lv_label_set_text(label, label_text);
    lv_obj_add_style(label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(label, &LIST_ROW_FONT, 0);

    if (icon_asset) {
        lv_obj_t * icon = lv_image_create(row);
        lv_image_set_src(icon, asset_path(icon_asset));
        lv_obj_align(icon, LV_ALIGN_LEFT_MID, 16, 0);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 72, 0);
    } else {
        lv_obj_align(label, LV_ALIGN_LEFT_MID, LIST_ROW_LABEL_INSET, 0);
    }

    if (cb) {
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, user_data);
    }
    return row;
}

static void rebuild_list(void) {
    lv_obj_clean(list);
    lv_label_set_text(path_label, current_dir);
    if (entry_count < 0) {
            add_file_row(atomic_load(&index_worker_oversized)
                         ? "Folder too large to index (tap Back)"
                         : "Unable to read folder (tap Back and retry)", NULL, NULL, NULL);
        return;
    }

    if (strlen(current_dir) > strlen(root_dir)) {
        add_file_row("Back", "sub_back/btn_back.png", up_click_cb, NULL);
    }

    if (page_start > 0) {
        add_file_row("Previous", "sub_back/btn_back.png", page_click_cb,
                     (void *) (intptr_t) -FILE_BROWSER_PAGE_SIZE);
    }

    int page_end = page_start + FILE_BROWSER_PAGE_SIZE;
    if (page_end > entry_count) page_end = entry_count;
    for (int i = page_start; i < page_end; i++) {
        const char * icon_asset = NULL;
        dir_entry_t *entry = &entries[i - page_start];
        if (entry->is_dir) icon_asset = "touch_list/list_folder.png";
        else if (entry->is_playlist) icon_asset = "sub_back/btn_playlist.png";
        /* No dedicated cue-sheet icon asset exists in this theme -- reuses
         * the playlist one, the closest existing match semantically (both
         * represent "tap to see a list of tracks", not a single song). */
        else if (entry->is_cue) icon_asset = "sub_back/btn_playlist.png";
        add_file_row(entry->name, icon_asset, entry_click_cb, (void *) (intptr_t) i);
    }

    if (page_end < entry_count) {
        add_file_row("Next", "playing_plane/btn_next.png", page_click_cb,
                     (void *) (intptr_t) FILE_BROWSER_PAGE_SIZE);
    }
}

/* Maximum directory recursion depth when walking song directories. */
#define SCAN_ALL_SONGS_MAX_DEPTH 64


/* Bounded-memory variant used by the database scanner. Does not sort:
 * ordering belongs in the on-disk DB, not in the discovery pass.
 * False means the scan is void. Unreadable entries and subtrees are stepped
 * over and counted in *skipped, which marks the walk incomplete. add_files is
 * inherited from the parent and flipped by database.ignore/database.unignore;
 * subdirectories are descended either way, so a nested database.unignore can
 * re-include a subtree. */
static bool walk_all_songs_recursive(const char * dir_path, file_browser_song_visit_cb_t cb, void * user,
                                     int * count, int depth, atomic_int * progress,
                                     const char * excluded_top_level_dir, int * skipped, bool add_files) {
    if (depth > SCAN_ALL_SONGS_MAX_DEPTH) { (*skipped)++; return true; }

    DIR * dir = opendir(dir_path);
    if (!dir) { (*skipped)++; return true; }

    /* One path buffer for the whole frame: this function recurses to
     * SCAN_ALL_SONGS_MAX_DEPTH on a fixed-size thread stack. */
    char path_buf[PATH_MAX];
    int probe_len = snprintf(path_buf, sizeof(path_buf), "%s/database.ignore", dir_path);
    bool has_ignore = (probe_len > 0 && (size_t) probe_len < sizeof(path_buf) && access(path_buf, F_OK) == 0);

    probe_len = snprintf(path_buf, sizeof(path_buf), "%s/database.unignore", dir_path);
    bool has_unignore = (probe_len > 0 && (size_t) probe_len < sizeof(path_buf) && access(path_buf, F_OK) == 0);

    if (has_ignore != has_unignore) add_files = has_unignore;

    bool fatal = false;
    struct dirent * de;
    for (;;) {
        errno = 0;
        de = readdir(dir);
        if (!de) {
            /* NULL is end-of-directory or a read error; errno separates them.
             * An error hides the rest of this directory, so it counts as an
             * omission rather than a clean end. */
            if (errno) (*skipped)++;
            break;
        }
        if (de->d_name[0] == '.') continue;

        int length = snprintf(path_buf, sizeof(path_buf), "%s/%s", dir_path, de->d_name);
        if (length < 0 || (size_t) length >= sizeof(path_buf)) { (*skipped)++; continue; }

        struct stat st;
        bool stat_ok = lstat(path_buf, &st) == 0;
        if (progress) atomic_fetch_add_explicit(progress, 1, memory_order_relaxed);
        if (!stat_ok) { (*skipped)++; continue; }
        /* Reject symlinks to prevent path traversal outside the music root. */
        if (S_ISLNK(st.st_mode)) continue;

        if (S_ISDIR(st.st_mode)) {
            /* Skip the excluded top-level directory before recursion.
             * depth==0 ensures subdirectories with the same name deeper
             * in the tree are still discovered. */
            if (depth == 0 && excluded_top_level_dir &&
                strcasecmp(de->d_name, excluded_top_level_dir) == 0)
                continue;
            if (!walk_all_songs_recursive(path_buf, cb, user, count, depth + 1, progress,
                                          excluded_top_level_dir, skipped, add_files)) {
                fatal = true;
                break;
            }
            continue;
        }
        if (!is_playable_file(de->d_name)) continue;

        if (add_files) {
            (*count)++;
            if (cb && !cb(path_buf, user)) { fatal = true; break; }
        }
    }

    closedir(dir);
    return !fatal;
}

bool file_browser_walk_all_songs_excluding_top_level(const char * root, const char * excluded_dir,
                                                     file_browser_song_visit_cb_t cb, void * user,
                                                     int * out_count, atomic_int * progress,
                                                     int * out_skipped) {
    int count = 0;
    int skipped = 0;
    /* A root that cannot be opened is a void scan, not an empty library. */
    DIR * root_dir = opendir(root);
    if (!root_dir) {
        if (out_count) *out_count = 0;
        if (out_skipped) *out_skipped = 0;
        return false;
    }
    closedir(root_dir);
    bool completed = walk_all_songs_recursive(root, cb, user, &count, 0, progress, excluded_dir, &skipped, true);
    if (out_count) *out_count = count;
    if (out_skipped) *out_skipped = skipped;
    return completed;
}

void file_browser_init(lv_obj_t * parent, const char * root, file_browser_select_cb_t on_select,
                        file_browser_cue_select_cb_t on_cue_select) {
    lv_obj_add_event_cb(parent, browser_delete_cb, LV_EVENT_DELETE, NULL);
    select_cb = on_select;
    cue_select_cb = on_cue_select;
    snprintf(root_dir, sizeof(root_dir), "%s", root);
    snprintf(current_dir, sizeof(current_dir), "%s", root);
    page_start = 0;
    position_depth = position_overflow = 0;
    restore_pending = false;

    path_label = lv_label_create(parent);
    lv_obj_set_style_text_color(path_label, lv_color_make(180, 180, 180), 0);
    lv_obj_align(path_label, LV_ALIGN_TOP_LEFT, 10, STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT + 4);
    lv_label_set_text(path_label, current_dir);

    /* Plain flex-column container, not lv_list -- rows are hand-built pill
     * shapes (add_file_row), not lv_list's own button/text item API. */
    list = lv_obj_create(parent);
    lv_obj_set_size(list, lv_pct(100),
                    lv_display_get_vertical_resolution(lv_display_get_default()) - STATUS_BAR_CLEARANCE -
                        TITLE_ROW_HEIGHT - 32 /* path_label row */);
    lv_obj_align(list, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(list, 0, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    /* Vertical-only scrolling so horizontal back-swipe gestures can escalate
     * to LV_EVENT_GESTURE instead of being consumed as scroll events. */
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    /* Clear default theme padding so rows center properly without edge clipping. */
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(list, GUI_ROW_GAP, 0);
    lv_obj_set_style_pad_top(list, GUI_ROW_GAP, 0);
    /* Rows follow the live display width. Explicit cross-axis centering also
     * keeps this correct if a future parent is narrower than the display. */
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);

    if (!index_poll_timer) index_poll_timer = lv_timer_create(index_poll_cb, 100, NULL);
    scan_current_dir();
    rebuild_list();
}

/* Resets the browser to root and refreshes the directory listing on SD card
 * hotplug events (mount/unmount) to avoid displaying stale or removed files. */
void file_browser_reset_to_root(void) {
    if (!list) return; /* gui_library_get_files_screen() not built yet -- nothing to refresh */
    snprintf(current_dir, sizeof(current_dir), "%s", root_dir);
    page_start = 0;
    position_depth = position_overflow = 0;
    restore_pending = false;
    scan_current_dir();
    rebuild_list();
}

const char * file_browser_get_last_selected_dir(void) {
    return last_selected_dir;
}

int file_browser_get_last_selected_row(void) {
    return last_selected_row;
}


bool file_browser_build_playlist_for_path(const char * path, char *** out_playlist, int * out_count, int * out_selected_index) {
    const char * slash = strrchr(path, '/');
    if (!slash) return false;

    char dir_path[PATH_MAX];
    size_t dir_len = (size_t) (slash - path);
    if (dir_len >= sizeof(dir_path)) dir_len = sizeof(dir_path) - 1;
    memcpy(dir_path, path, dir_len);
    dir_path[dir_len] = '\0';

    dir_entry_t * scanned = NULL;
    int scanned_count = scan_directory(dir_path, &scanned);
    if (scanned_count < 0) return false;

    char ** playlist = malloc(sizeof(char *) * (size_t) (scanned_count > 0 ? scanned_count : 1));
    if (!playlist) {
        free(scanned);
        return false;
    }
    int count = 0;
    int selected = -1;

    for (int i = 0; i < scanned_count; i++) {
        if (scanned[i].is_dir || scanned[i].is_playlist || scanned[i].is_cue) continue;

        char full_path[PATH_MAX];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, scanned[i].name);
        if (strcmp(full_path, path) == 0) selected = count;

        playlist[count] = strdup(full_path);
        if (!playlist[count]) {
            for (int j = 0; j < count; j++) free(playlist[j]);
            free(playlist);
            free(scanned);
            return false;
        }
        count++;
    }

    free(scanned);

    if (selected < 0) {
        for (int i = 0; i < count; i++) free(playlist[i]);
        free(playlist);
        return false;
    }

    *out_playlist = playlist;
    *out_count = count;
    *out_selected_index = selected;
    return true;
}

bool file_browser_open_lazy_directory(const char *track_path, file_browser_index_t **out_index,
                                      unsigned *out_playable, unsigned *out_selected) {
    if (out_index) *out_index = NULL;
    if (out_playable) *out_playable = 0;
    if (out_selected) *out_selected = 0;
    if (!track_path || !out_index || !out_playable || !out_selected) return false;
    const char *slash = strrchr(track_path, '/');
    if (!slash || slash == track_path) return false;
    char dir_path[PATH_MAX];
    size_t dir_len = (size_t) (slash - track_path);
    if (dir_len >= sizeof(dir_path)) return false;
    memcpy(dir_path, track_path, dir_len);
    dir_path[dir_len] = '\0';
    file_browser_index_t *index = NULL;
    unsigned count = 0;
    if (!file_browser_index_open(dir_path, &index, &count)) return false;
    unsigned playable = file_browser_index_playable_count(index);
    for (unsigned i = 0; i < playable; i++) {
        char path[PATH_MAX];
        if (!file_browser_index_playable_path_at(index, i, path, sizeof(path))) {
            file_browser_index_close(index);
            return false;
        }
        if (strcmp(path, track_path) == 0) {
            *out_index = index;
            *out_playable = playable;
            *out_selected = i;
            return true;
        }
    }
    file_browser_index_close(index);
    return false;
}
