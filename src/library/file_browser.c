#include <stdatomic.h>
#include "file_browser.h"
#include "assets.h"
#include "screen_builders.h" /* STATUS_BAR_CLEARANCE / TITLE_ROW_HEIGHT / LIST_ROW_* */
#include "playlist_files.h" /* playlist_files_resolve_path() -- shared M3U line resolution */
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

typedef struct {
    char name[256];
    bool is_dir;
    bool is_playlist;
    bool is_cue;
} dir_entry_t;

static char root_dir[PATH_MAX];
static char current_dir[PATH_MAX];
static dir_entry_t * entries = NULL;
static int entry_count = 0;

static lv_obj_t * path_label;
static lv_obj_t * list;
static file_browser_select_cb_t select_cb;
static file_browser_cue_select_cb_t cue_select_cb;

/* Snapshot of the directory + raw on-screen row (entries[] index, not the
 * file-only position select_cb receives) a file/playlist was last tapped
 * from -- set in entry_click_cb() right before invoking select_cb(), for
 * gui.c's player "List" option to later reopen the browser at that same
 * spot via file_browser_navigate_to(). */
static char last_selected_dir[PATH_MAX];
static int last_selected_row = -1;

static void rebuild_list(void);

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

static bool is_m3u_file(const char * name) {
    const char * ext = strrchr(name, '.');
    if (!ext) return false;
    return strcasecmp(ext, ".m3u") == 0 || strcasecmp(ext, ".m3u8") == 0;
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

static void free_entries(void) {
    free(entries);
    entries = NULL;
    entry_count = 0;
}

/* Scans dir_path into a freshly malloc'd, sorted (dirs-first, then alpha)
 * array of playable entries. Caller owns the result (free() it). Used both
 * for the interactive browser's current directory (via scan_current_dir)
 * and for one-shot lookups that don't touch the browser's own state (e.g.
 * resuming a track without ever having opened the browser screen). */
static int scan_directory(const char * dir_path, dir_entry_t ** out_entries) {
    DIR * dir = opendir(dir_path);
    if (!dir) {
        fprintf(stderr, "file_browser: failed to open '%s'\n", dir_path);
        *out_entries = NULL;
        return 0;
    }

    int capacity = 32;
    int count = 0;
    dir_entry_t * result = malloc(sizeof(dir_entry_t) * (size_t) capacity);
    if (!result) {
        closedir(dir);
        *out_entries = NULL;
        return 0;
    }

    struct dirent * de;
    while ((de = readdir(dir)) != NULL) {
        if (de->d_name[0] == '.') continue; /* skips ".", "..", and hidden files/dirs */

        char full_path[PATH_MAX];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, de->d_name);

        struct stat st;
        if (stat(full_path, &st) != 0) continue;

        bool is_dir = S_ISDIR(st.st_mode);
        bool is_playlist = !is_dir && is_m3u_file(de->d_name);
        /* Only shown at all if a caller actually wants .cue sheets (see
         * file_browser_init()'s own comment) -- a caller with cue_select_cb
         * == NULL never sees them, same as any other file type this
         * browser doesn't recognize. */
        bool is_cue = !is_dir && cue_select_cb && is_cue_file(de->d_name);
        if (!is_dir && !is_playlist && !is_cue && !is_playable_file(de->d_name)) continue;

        if (count == capacity) {
            /* Stop growing and return entries collected so far if realloc fails. */
            dir_entry_t * grown = realloc(result, sizeof(dir_entry_t) * (size_t) (capacity * 2));
            if (!grown) break;
            result = grown;
            capacity *= 2;
        }

        utf8_truncate_safe(result[count].name, de->d_name, sizeof(result[count].name));
        utf8_sanitize(result[count].name);
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

static void scan_current_dir(void) {
    free_entries();
    entry_count = scan_directory(current_dir, &entries);
}

/* Builds the playlist from every playable file in the current directory
 * (in the same sorted order they're displayed) and reports which position
 * within that file-only list corresponds to `file_display_index`. */
static void build_playlist_and_select(int file_display_index) {
    char ** playlist = malloc(sizeof(char *) * (size_t) entry_count);
    int count = 0;
    int selected = -1;

    for (int i = 0; i < entry_count; i++) {
        if (entries[i].is_dir || entries[i].is_playlist || entries[i].is_cue) continue;
        if (i == file_display_index) selected = count;

        char full_path[PATH_MAX];
        snprintf(full_path, sizeof(full_path), "%s/%s", current_dir, entries[i].name);
        playlist[count] = strdup(full_path);
        count++;
    }

    if (selected < 0) {
        for (int i = 0; i < count; i++) free(playlist[i]);
        free(playlist);
        return;
    }

    select_cb(playlist, count, selected);
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
        scan_current_dir();
        rebuild_list();
    }
}

static void up_click_cb(lv_event_t * e) {
    (void) e;
    file_browser_go_up();
}

static void entry_click_cb(lv_event_t * e) {
    int index = (int) (intptr_t) lv_event_get_user_data(e);

    if (entries[index].is_dir) {
        char new_dir[PATH_MAX];
        snprintf(new_dir, sizeof(new_dir), "%s/%s", current_dir, entries[index].name);
        snprintf(current_dir, sizeof(current_dir), "%s", new_dir);
        scan_current_dir();
        rebuild_list();
    } else if (entries[index].is_playlist) {
        char m3u_path[PATH_MAX];
        snprintf(m3u_path, sizeof(m3u_path), "%s/%s", current_dir, entries[index].name);

        char ** playlist;
        int count;
        if (file_browser_build_playlist_from_m3u(m3u_path, &playlist, &count)) {
            snprintf(last_selected_dir, sizeof(last_selected_dir), "%s", current_dir);
            last_selected_row = index;
            select_cb(playlist, count, 0);
        }
    } else if (entries[index].is_cue) {
        char cue_path[PATH_MAX];
        snprintf(cue_path, sizeof(cue_path), "%s/%s", current_dir, entries[index].name);
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

    if (strlen(current_dir) > strlen(root_dir)) {
        add_file_row("Back", "sub_back/btn_back.png", up_click_cb, NULL);
    }

    for (int i = 0; i < entry_count; i++) {
        const char * icon_asset = NULL;
        if (entries[i].is_dir) icon_asset = "touch_list/list_folder.png";
        else if (entries[i].is_playlist) icon_asset = "sub_back/btn_playlist.png";
        /* No dedicated cue-sheet icon asset exists in this theme -- reuses
         * the playlist one, the closest existing match semantically (both
         * represent "tap to see a list of tracks", not a single song). */
        else if (entries[i].is_cue) icon_asset = "sub_back/btn_playlist.png";
        add_file_row(entries[i].name, icon_asset, entry_click_cb, (void *) (intptr_t) i);
    }
}

/* Recursively scans directories up to SCAN_ALL_SONGS_MAX_DEPTH. Uses lstat()
 * and rejects symlinks (both directories and files) to prevent recursion loops
 * and path traversal outside the music root. Entries are visited in readdir()
 * order since the final result is sorted by full path. */
#define SCAN_ALL_SONGS_MAX_DEPTH 64
static void scan_all_songs_recursive(const char * dir_path, char *** paths, int * count, int * capacity, int depth,
                                      atomic_int * progress) {
    if (depth > SCAN_ALL_SONGS_MAX_DEPTH) return;

    DIR * dir = opendir(dir_path);
    if (!dir) return;

    struct dirent * de;
    while ((de = readdir(dir)) != NULL) {
        if (de->d_name[0] == '.') continue;

        char full_path[PATH_MAX];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, de->d_name);

        struct stat st;
        bool stat_ok = lstat(full_path, &st) == 0;
        /* Incremented after each lstat() returns, for every entry examined
         * (not just playable files), so callers can distinguish a long
         * stretch of non-music entries from a hung lstat(). Placed after
         * the call so a stuck lstat() is not counted as progress. */
        if (progress) atomic_fetch_add_explicit(progress, 1, memory_order_relaxed);
        if (!stat_ok) continue;
        /* Reject symlinks to prevent path traversal outside the music root. */
        if (S_ISLNK(st.st_mode)) continue;

        if (S_ISDIR(st.st_mode)) {
            scan_all_songs_recursive(full_path, paths, count, capacity, depth + 1, progress);
            continue;
        }
        if (!is_playable_file(de->d_name)) continue;

        if (*count == *capacity) {
            /* Safely reallocate paths array; stop collecting entries on failure. */
            int new_capacity = *capacity ? *capacity * 2 : 64;
            char ** grown = realloc(*paths, sizeof(char *) * (size_t) new_capacity);
            if (!grown) break;
            *paths = grown;
            *capacity = new_capacity;
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


/* Bounded-memory variant used by the database scanner. Does not sort:
 * ordering belongs in the on-disk DB, not in the discovery pass. */
static bool walk_all_songs_recursive(const char * dir_path, file_browser_song_visit_cb_t cb, void * user,
                                     int * count, int depth, atomic_int * progress,
                                     const char * excluded_top_level_dir) {
    if (depth > SCAN_ALL_SONGS_MAX_DEPTH) return false;

    DIR * dir = opendir(dir_path);
    if (!dir) return false;

    bool keep_going = true;
    struct dirent * de;
    while (keep_going) {
        errno = 0;
        de = readdir(dir);
        if (!de) { if (errno) keep_going = false; break; }
        if (de->d_name[0] == '.') continue;

        char full_path[PATH_MAX];
        int length = snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, de->d_name);
        if (length < 0 || (size_t) length >= sizeof(full_path)) { keep_going = false; break; }

        struct stat st;
        bool stat_ok = lstat(full_path, &st) == 0;
        if (progress) atomic_fetch_add_explicit(progress, 1, memory_order_relaxed);
        if (!stat_ok) { keep_going = false; break; }
        /* Reject symlinks to prevent path traversal outside the music root. */
        if (S_ISLNK(st.st_mode)) continue;

        if (S_ISDIR(st.st_mode)) {
            /* Skip the excluded top-level directory before recursion.
             * depth==0 ensures subdirectories with the same name deeper
             * in the tree are still discovered. */
            if (depth == 0 && excluded_top_level_dir &&
                strcasecmp(de->d_name, excluded_top_level_dir) == 0)
                continue;
            keep_going = walk_all_songs_recursive(full_path, cb, user, count, depth + 1, progress,
                                                  excluded_top_level_dir);
            continue;
        }
        if (!is_playable_file(de->d_name)) continue;

        (*count)++;
        if (cb && !cb(full_path, user)) keep_going = false;
    }

    closedir(dir);
    return keep_going;
}

bool file_browser_walk_all_songs(const char * root, file_browser_song_visit_cb_t cb, void * user,
                                 int * out_count, atomic_int * progress) {
    return file_browser_walk_all_songs_excluding_top_level(root, NULL, cb, user, out_count, progress);
}

bool file_browser_walk_all_songs_excluding_top_level(const char * root, const char * excluded_dir,
                                                     file_browser_song_visit_cb_t cb, void * user,
                                                     int * out_count, atomic_int * progress) {
    int count = 0;
    bool completed = walk_all_songs_recursive(root, cb, user, &count, 0, progress, excluded_dir);
    if (out_count) *out_count = count;
    return completed;
}

bool file_browser_scan_all_songs(const char * root, char *** out_paths, int * out_count, atomic_int * progress) {
    char ** paths = NULL;
    int count = 0;
    int capacity = 0;

    scan_all_songs_recursive(root, &paths, &count, &capacity, 0, progress);

    if (count == 0) {
        free(paths);
        return false;
    }

    qsort(paths, (size_t) count, sizeof(char *), compare_paths);
    *out_paths = paths;
    *out_count = count;
    return true;
}

void file_browser_init(lv_obj_t * parent, const char * root, file_browser_select_cb_t on_select,
                        file_browser_cue_select_cb_t on_cue_select) {
    select_cb = on_select;
    cue_select_cb = on_cue_select;
    snprintf(root_dir, sizeof(root_dir), "%s", root);
    snprintf(current_dir, sizeof(current_dir), "%s", root);

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

    scan_current_dir();
    rebuild_list();
}

/* Resets the browser to root and refreshes the directory listing on SD card
 * hotplug events (mount/unmount) to avoid displaying stale or removed files. */
void file_browser_reset_to_root(void) {
    if (!list) return; /* gui_library_get_files_screen() not built yet -- nothing to refresh */
    snprintf(current_dir, sizeof(current_dir), "%s", root_dir);
    scan_current_dir();
    rebuild_list();
}

const char * file_browser_get_last_selected_dir(void) {
    return last_selected_dir;
}

int file_browser_get_last_selected_row(void) {
    return last_selected_row;
}

/* row_to_reveal is the raw entries[] index (same value
 * file_browser_get_last_selected_row() returned), not a file-only
 * position -- rebuild_list() prepends a "Back" row whenever current_dir
 * isn't root_dir, shifting every entries[] row down by one on screen, so
 * that offset is added here to land on the right actual child. */
void file_browser_navigate_to(const char * dir, int row_to_reveal) {
    if (!list) return; /* gui_library_get_files_screen() not built yet */
    snprintf(current_dir, sizeof(current_dir), "%s", dir);
    scan_current_dir();
    rebuild_list();
    if (row_to_reveal < 0) return;
    int child_index = row_to_reveal + (strlen(current_dir) > strlen(root_dir) ? 1 : 0);
    lv_obj_update_layout(list);
    if (child_index < (int) lv_obj_get_child_count(list)) {
        lv_obj_scroll_to_view(lv_obj_get_child(list, child_index), LV_ANIM_OFF);
    }
}

bool file_browser_build_playlist_for_path(const char * path, char *** out_playlist, int * out_count, int * out_selected_index) {
    const char * slash = strrchr(path, '/');
    if (!slash) return false;

    char dir_path[PATH_MAX];
    size_t dir_len = (size_t) (slash - path);
    if (dir_len >= sizeof(dir_path)) dir_len = sizeof(dir_path) - 1;
    memcpy(dir_path, path, dir_len);
    dir_path[dir_len] = '\0';

    dir_entry_t * scanned;
    int scanned_count = scan_directory(dir_path, &scanned);

    char ** playlist = malloc(sizeof(char *) * (size_t) scanned_count);
    int count = 0;
    int selected = -1;

    for (int i = 0; i < scanned_count; i++) {
        if (scanned[i].is_dir || scanned[i].is_playlist || scanned[i].is_cue) continue;

        char full_path[PATH_MAX];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, scanned[i].name);
        if (strcmp(full_path, path) == 0) selected = count;

        playlist[count] = strdup(full_path);
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
