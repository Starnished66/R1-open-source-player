#include "file_browser.h"
#include "assets.h"
#include "screen_builders.h" /* STATUS_BAR_CLEARANCE / TITLE_ROW_HEIGHT / LIST_ROW_* */
#include "playlist_files.h" /* playlist_files_resolve_path() -- shared M3U line resolution */

#include <dirent.h>
#include <sys/stat.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>

typedef struct {
    char name[256];
    bool is_dir;
    bool is_playlist;
} dir_entry_t;

static char root_dir[PATH_MAX];
static char current_dir[PATH_MAX];
static dir_entry_t * entries = NULL;
static int entry_count = 0;

static lv_obj_t * path_label;
static lv_obj_t * list;
static file_browser_select_cb_t select_cb;

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
    ".flac", ".mp3", ".wav", ".aiff", ".aif", ".dsf", ".dff", ".aac", ".m4a", ".ape", ".wma", ".opus",
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
        if (!is_dir && !is_playlist && !is_playable_file(de->d_name)) continue;

        if (count == capacity) {
            /* Audit finding: the previous version overwrote `result` with
             * realloc()'s return value unconditionally -- on allocation
             * failure that's NULL, leaking the existing buffer and crashing
             * on the very next write below. Stop growing and return
             * whatever was collected so far instead; a failed grow shouldn't
             * lose (or crash on) the entries already found. */
            dir_entry_t * grown = realloc(result, sizeof(dir_entry_t) * (size_t) (capacity * 2));
            if (!grown) break;
            result = grown;
            capacity *= 2;
        }

        snprintf(result[count].name, sizeof(result[count].name), "%s", de->d_name);
        result[count].is_dir = is_dir;
        result[count].is_playlist = is_playlist;
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
        if (entries[i].is_dir || entries[i].is_playlist) continue;
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
    FILE * f = fopen(m3u_path, "r");
    if (!f) return false;

    char ** playlist = NULL;
    int capacity = 0;
    int count = 0;
    char line[PATH_MAX];

    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';
        if (len == 0 || line[0] == '#') continue; /* blank line or M3U directive/comment */
        if (!is_playable_file(line)) continue;

        /* Resolves both absolute lines (old-style) and lines relative to
         * m3u_path's own directory (the standard M3U convention, and what
         * playlist_files_append()/_create() now write) -- see
         * playlist_files_resolve_path()'s own comment. */
        char full_path[PATH_MAX];
        playlist_files_resolve_path(m3u_path, line, full_path, sizeof(full_path));

        if (count == capacity) {
            /* Audit finding: same unguarded-realloc-plus-early-capacity-
             * update issue as scan_all_songs_recursive()'s own fix -- see
             * its comment for the full reasoning. */
            int new_capacity = capacity ? capacity * 2 : 8;
            char ** grown = realloc(playlist, sizeof(char *) * (size_t) new_capacity);
            if (!grown) break;
            playlist = grown;
            capacity = new_capacity;
        }
        playlist[count++] = strdup(full_path);
    }

    fclose(f);

    if (count == 0) {
        free(playlist);
        return false;
    }

    *out_playlist = playlist;
    *out_count = count;
    return true;
}

static void up_click_cb(lv_event_t * e) {
    (void) e;
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
    lv_obj_set_size(row, LIST_ROW_WIDTH_WIDE, LIST_ROW_HEIGHT); /* 15% wider than the shared default -- explicit user request */
    lv_obj_set_style_radius(row, LIST_ROW_RADIUS, 0);
    lv_obj_set_style_bg_color(row, LIST_ROW_BG_COLOR, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * label = lv_label_create(row);
    lv_label_set_text(label, label_text);
    lv_obj_set_style_text_color(label, lv_color_make(230, 230, 230), 0);
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
        add_file_row("Up", "touch_list/list_folder.png", up_click_cb, NULL);
    }

    for (int i = 0; i < entry_count; i++) {
        const char * icon_asset = NULL;
        if (entries[i].is_dir) icon_asset = "touch_list/list_folder.png";
        else if (entries[i].is_playlist) icon_asset = "sub_back/btn_playlist.png";
        add_file_row(entries[i].name, icon_asset, entry_click_cb, (void *) (intptr_t) i);
    }
}

/* Real-device incident: this recursion had no depth limit and followed
 * symlinks (plain stat(), not lstat()) -- confirmed live as the cause of a
 * hard crash (SIGSEGV inside vsnprintf, on the library-rescan background
 * thread) after a couple of Import-via-Wi-Fi cycles, each of which triggers
 * a full library rescan on exit. Each recursion level's own PATH_MAX
 * (4KB-ish) full_path buffer plus a struct stat adds up fast against a
 * background pthread's default (musl, well under a desktop's typical 8MB)
 * stack -- a symlink loop anywhere under the music root recurses
 * indefinitely and blows that stack in well under a hundred levels; even a
 * genuinely deep but finite real directory tree could get uncomfortably
 * close. Fixed two ways: lstat() instead of stat() so a symlinked
 * directory is identified as a symlink rather than resolved and recursed
 * into (the same reasoning most real media-library scanners use -- there's
 * no legitimate reason for a scan of the music root to leave it via a
 * symlink), and a hard depth cap as defense-in-depth against any other
 * pathologically deep tree, symlink-related or not. A symlinked individual
 * file is rejected the same way (S_ISLNK check right after lstat(), below)
 * -- an earlier version of this fix let it through, since lstat()'s
 * S_ISDIR is only false for it, same as for a plain file, so it fell
 * through to the ordinary is_playable_file() check; a later audit flagged
 * that as a real path-traversal gap (a playable-extension symlink can
 * point anywhere on the filesystem, feeding arbitrary on-device file
 * content into the tag parsers below).
 *
 * Directories are visited in readdir() order (not sorted) since the whole
 * result gets one final sort by full path anyway -- sorting each
 * directory's entries individually first would be wasted work. */
#define SCAN_ALL_SONGS_MAX_DEPTH 64
static void scan_all_songs_recursive(const char * dir_path, char *** paths, int * count, int * capacity, int depth,
                                      volatile int * progress) {
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
        /* Bumped only once lstat() actually returns, for every real entry
         * examined (not just playable files found) -- a long stretch of
         * non-music subfolders (playlists, artwork, whatever else shares
         * the card) is still genuine forward progress, not a stall, and
         * counting only matches would let a caller's stall detector
         * false-trigger partway through a large library just because the
         * music itself is unevenly distributed across the tree. Placed
         * after the call, not before it starts, so a caller watching this
         * can't mistake a currently-hung lstat() (the exact corrupted-block
         * D-state scenario this exists to catch, per this file's own
         * caller in gui.c) for progress that already happened. See this
         * parameter's own doc comment in file_browser.h. */
        if (progress) (*progress)++;
        if (!stat_ok) continue;
        /* Audit finding: a symlinked directory is already excluded by
         * lstat() above (S_ISDIR is false for it, matching this file's own
         * "no legitimate reason for a scan of the music root to leave it
         * via a symlink" reasoning) -- but per this function's own header
         * comment, a symlinked individual FILE was deliberately still
         * followed. That lets a crafted SD card point a playable-extension
         * symlink at an arbitrary on-device path (e.g. /usr/data/
         * wpa_supplicant.conf), feeding attacker-chosen file content into
         * the tag parsers this same audit found multiple crash bugs in
         * (ID3v2/M4A) -- reject every symlink outright, not just directory
         * ones. */
        if (S_ISLNK(st.st_mode)) continue;

        if (S_ISDIR(st.st_mode)) {
            scan_all_songs_recursive(full_path, paths, count, capacity, depth + 1, progress);
            continue;
        }
        if (!is_playable_file(de->d_name)) continue;

        if (*count == *capacity) {
            /* Audit finding: same unguarded-realloc crash risk as scan_
             * directory() above, plus a second bug specific to this one --
             * *capacity was doubled BEFORE the realloc even ran, so a
             * failure left *paths NULL while *capacity already reflected
             * the bigger size, meaning the very next growth check wouldn't
             * even fire again and every recursive call would keep writing
             * through the NULL pointer. Compute the new size into a local
             * first and only commit *paths and *capacity once realloc
             * actually succeeds; on failure, stop collecting more entries at this
             * recursion level (every other in-flight/future call converges
             * the same way once real OOM is reached) rather than crash. */
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


/* Bounded-memory variant used by the database scanner. It deliberately does not
 * sort: ordering is a presentation/query concern and belongs in the on-disk DB,
 * not in the discovery pass. This is the same separation that keeps Rockbox's
 * tagcache builder from needing an in-RAM representation of the whole library. */
static bool walk_all_songs_recursive(const char * dir_path, file_browser_song_visit_cb_t cb, void * user,
                                     int * count, int depth, volatile int * progress) {
    if (depth > SCAN_ALL_SONGS_MAX_DEPTH) return true;

    DIR * dir = opendir(dir_path);
    if (!dir) return true;

    bool keep_going = true;
    struct dirent * de;
    while (keep_going && (de = readdir(dir)) != NULL) {
        if (de->d_name[0] == '.') continue;

        char full_path[PATH_MAX];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, de->d_name);

        struct stat st;
        bool stat_ok = lstat(full_path, &st) == 0;
        if (progress) (*progress)++;
        if (!stat_ok) continue;
        /* Audit finding -- see scan_all_songs_recursive()'s own comment,
         * the sibling walker this mirrors. */
        if (S_ISLNK(st.st_mode)) continue;

        if (S_ISDIR(st.st_mode)) {
            keep_going = walk_all_songs_recursive(full_path, cb, user, count, depth + 1, progress);
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
                                 int * out_count, volatile int * progress) {
    int count = 0;
    bool completed = walk_all_songs_recursive(root, cb, user, &count, 0, progress);
    if (out_count) *out_count = count;
    return completed;
}

bool file_browser_scan_all_songs(const char * root, char *** out_paths, int * out_count, volatile int * progress) {
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

void file_browser_init(lv_obj_t * parent, const char * root, file_browser_select_cb_t on_select) {
    select_cb = on_select;
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
    /* Vertical-only -- a plain lv_obj_create() defaults to scrollable in
     * every direction, which real-hardware testing showed capturing the
     * app-wide horizontal back-swipe as a (no-op) scroll attempt instead of
     * letting it escalate to LV_EVENT_GESTURE. Still scrolls vertically
     * fine for a directory with more files than fit on screen. */
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(list, 4, 0);
    lv_obj_set_style_pad_top(list, 4, 0);
    /* Rows are LIST_ROW_WIDTH_WIDE (476px, this device's 480px-wide screen
     * minus a thin 4px margin) -- explicit cross-axis centering so that
     * width is guaranteed to sit centered within this full-width container
     * regardless of the default theme's own flex padding, rather than
     * risking an unverified assumption about it clipping past the edge. */
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);

    scan_current_dir();
    rebuild_list();
}

/* Real-device bug report (2026-08-08): the Files screen's own listing (root
 * and any subdirectory) is otherwise only ever scanned once -- at
 * file_browser_init() time (app boot) and again on each interactive
 * up/entry click above -- so an SD-card hotplug event that happens while the
 * user isn't actively navigating the browser (or on the very first view of
 * the root, before any click) left it showing a stale snapshot: files that
 * no longer exist after a card is pulled, or a newly inserted card's
 * content not showing up at all without the user manually navigating away
 * and back. Called from gui.c's poll_sd_card_hotplug() on both the
 * unmounted->mounted and mounted->unmounted edges. Resets back to the
 * browser's root (not just re-scanning whatever directory happened to be
 * current) since a directory the user was sitting in on a since-removed SD
 * card may no longer exist at all. */
void file_browser_reset_to_root(void) {
    if (!list) return; /* files_screen not built yet -- nothing to refresh */
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
 * position -- rebuild_list() prepends an "Up" row whenever current_dir
 * isn't root_dir, shifting every entries[] row down by one on screen, so
 * that offset is added here to land on the right actual child. */
void file_browser_navigate_to(const char * dir, int row_to_reveal) {
    if (!list) return; /* files_screen not built yet */
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
        if (scanned[i].is_dir || scanned[i].is_playlist) continue;

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
