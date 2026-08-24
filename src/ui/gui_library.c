#include "gui_library.h"
#include "assets.h"
#include "idle_shutdown.h"

extern void on_file_browser_selected(char ** new_playlist, int count, int selected_index);
extern void on_file_selected_at(char ** new_playlist, int count, int selected_index, int start_seconds);
extern void open_queue_screen(void);
extern void nav_remove_stack_slot(int depth);
extern void mount_sd_card_if_needed(void);
extern void library_scan_once(void);
extern void library_load_from_cache_only(void);
void refresh_artist_albums_now_playing_indicator(void);
#include "gui.h"
#include "gui_theme.h"
#include "gui_notifications.h"
#include "gui_text_input.h"
#include "gui_subsonic.h"
#include "gui_books.h"
#include "plugin_manager.h"
#include "screen_builders.h"
#include "metadata.h"
#include "metadata_db.h"
#include "file_browser.h"
#include "playlist_files.h"
#include "cue_parser.h"
#include "cover_decode.h"
#include "albumart.h"
#include "device_config.h"
#include "settings.h"
#include "audio.h"
#include "subprocess.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>
#include <sys/stat.h>
#include <limits.h>
#include <ctype.h>

#ifdef HOST_BUILD
  #define MUSIC_ROOT_DIR "./music"
#else
  #define MUSIC_ROOT_DIR "/data/mnt/sd_0"
#endif

#define PLAYLISTS_DIR MUSIC_ROOT_DIR "/Playlists"
#define COMPACT_LIST_PAGE_CACHE_SIZE 64
#define STATUS_BAR_CLEARANCE 48
#define TITLE_ROW_HEIGHT 56
#define MUSIC_LIST_ROW_HEIGHT 88
#define LIST_ROW_WIDTH_WIDE 432
#define LIST_ROW_WIDTH_NARROW 384
#define MOST_PLAYED_LIMIT 100
#define EXTERNAL_COVER_MAX_BYTES (4U * 1024U * 1024U)

static lv_obj_t * album_thumbnail_active_list = NULL;
static int album_thumbnail_generation = 0;
static lv_obj_t * playlists_edit_btn = NULL;
static bool playlists_edit_mode = false;
static bool group_songs_source_is_album = false;
static gui_busy_handle_t library_rescan_token = 0;
static gui_busy_handle_t sd_format_token = 0;
static atomic_int library_scan_progress_total = 0;
static atomic_int library_scan_progress_done = 0;

static void album_thumbnail_screen_loaded_cb(lv_event_t * e);
static void album_thumbnail_screen_unloaded_cb(lv_event_t * e);
static void album_thumbnail_scroll_cb(lv_event_t * e);
static int artists_fetch_page(void * ctx, int offset, int count, compact_list_page_row_t out_rows[]);
static int albums_fetch_page(void * ctx, int offset, int count, compact_list_page_row_t out_rows[]);
static int album_artists_fetch_page(void * ctx, int offset, int count, compact_list_page_row_t out_rows[]);
static int all_songs_fetch_page(void * ctx, int offset, int count, compact_list_page_row_t out_rows[]);
static void music_files_tile_cb(lv_event_t * e);

/* Screen pointers owned by this module */
lv_obj_t * music_screen = NULL;
lv_obj_t * files_screen = NULL;
lv_obj_t * files_list = NULL;
lv_obj_t * files_title_label = NULL;
lv_obj_t * files_search_list = NULL;
lv_obj_t * files_search_title_label = NULL;

lv_obj_t * all_songs_screen = NULL;
lv_obj_t * all_songs_list = NULL;
lv_obj_t * all_songs_title_label = NULL;

lv_obj_t * recently_added_screen = NULL;
lv_obj_t * recently_added_list = NULL;
lv_obj_t * recently_added_title_label = NULL;

lv_obj_t * artists_screen = NULL;
lv_obj_t * artists_list = NULL;
lv_obj_t * artists_title_label = NULL;

lv_obj_t * albums_screen = NULL;
lv_obj_t * albums_list = NULL;
lv_obj_t * albums_title_label = NULL;

lv_obj_t * album_artist_screen = NULL;
lv_obj_t * album_artist_list = NULL;
lv_obj_t * album_artist_title_label = NULL;

lv_obj_t * group_songs_screen = NULL;
lv_obj_t * group_songs_list = NULL;
lv_obj_t * group_songs_title_label = NULL;

lv_obj_t * artist_albums_screen = NULL;
lv_obj_t * artist_albums_list = NULL;
lv_obj_t * artist_albums_title_label = NULL;

lv_obj_t * playlists_screen = NULL;
lv_obj_t * playlists_list = NULL;
lv_obj_t * playlists_title_label = NULL;

lv_obj_t * cue_tracks_screen = NULL;
lv_obj_t * cue_tracks_list = NULL;
lv_obj_t * cue_tracks_title_label = NULL;

lv_obj_t * add_to_playlist_screen = NULL;
lv_obj_t * add_to_playlist_list = NULL;

/* Externs to player/queue and global state */
extern player_settings_t current_settings;
extern void nav_push(lv_obj_t * screen);
extern void nav_pop(void);
extern void nav_reset_to_home(void);
extern void finalize_screen_navigation(lv_obj_t * screen);
extern void on_file_selected(char ** new_playlist, int count, int selected_index);
extern const char * playlist_path_at(int index);
extern void enable_gesture_bubble_recursive(lv_obj_t * obj);
extern lv_obj_t * build_confirm_popup(const char * title_text, lv_label_long_mode_t title_long_mode, lv_obj_t ** out_title, const char * body_text, const char * confirm_text, lv_color_t confirm_color, lv_event_cb_t confirm_cb, lv_obj_t ** out_confirm_row, const char * cancel_text, lv_color_t cancel_color, lv_event_cb_t cancel_cb, lv_obj_t ** out_cancel_row, lv_event_cb_t backdrop_cb, lv_obj_t ** out_backdrop);
extern void register_static_snapshot(int index, lv_obj_t * screen);
extern void unregister_static_snapshot(lv_obj_t * screen);




/* Which song this screen adds to whatever playlist is picked -- set by
 * open_add_to_playlist_for() right before nav_push(), not always the
 * currently-playing track anymore (see that function's own comment: the
 * song long-press context menu reaches this same screen for an arbitrary
 * row, not just the player's own "..." menu). */
static char add_to_playlist_target_path[600] = ""; /* 600, matching song_row_t.path's own bound (metadata_db.h) */

static void new_playlist_name_done_cb(const char * text, void * user_data) {
    (void) user_data;
    if (text[0] == '\0' || add_to_playlist_target_path[0] == '\0') return;

    char created_path[512];
    bool ok = playlist_files_create(PLAYLISTS_DIR, text, add_to_playlist_target_path, created_path, sizeof(created_path));
    if (ok) metadata_db_playlist_insert_one(created_path);
    show_error_toast(ok ? "Playlist created" : "Failed to create playlist");
    nav_pop(); /* leave the Add to Playlist picker too, back to the player */
}

static void new_playlist_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    show_text_entry("Playlist Name", "", false, false, new_playlist_name_done_cb, NULL);
}

static void existing_playlist_row_cb(lv_event_t * e) {
    const char * path = (const char *) lv_event_get_user_data(e);
    if (add_to_playlist_target_path[0] == '\0') return;

    /* Playlist design change: adding a song already in the target playlist
     * used to just append a second copy with no feedback -- confirmed by
     * playlist_files_append()'s own unconditional fprintf(). Checked here
     * rather than inside playlist_files_append() itself so that function
     * stays a plain, unconditional "add this line" primitive other callers
     * (e.g. new_playlist_name_done_cb() below, adding a brand-new file's
     * first song) don't pay an unnecessary duplicate scan for. */
    if (playlist_files_contains(path, add_to_playlist_target_path)) {
        show_error_toast("Song already added");
        nav_pop();
        return;
    }

    bool ok = playlist_files_append(path, add_to_playlist_target_path);
    show_error_toast(ok ? "Added to playlist" : "Failed to add to playlist");
    nav_pop();
}

static void populate_add_to_playlist_screen(void) {
    lv_obj_clean(add_to_playlist_list);

    lv_obj_t * new_row = lv_obj_create(add_to_playlist_list);
    lv_obj_set_size(new_row, LIST_ROW_WIDTH, LIST_ROW_HEIGHT);
    lv_obj_set_style_radius(new_row, LIST_ROW_RADIUS, 0);
    lv_obj_set_style_bg_color(new_row, LIST_ROW_BG_COLOR, 0);
    lv_obj_set_style_bg_opa(new_row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(new_row, 0, 0);
    lv_obj_remove_flag(new_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(new_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(new_row, new_playlist_row_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * new_label = lv_label_create(new_row);
    lv_label_set_text(new_label, "+ New Playlist");
    lv_obj_set_style_text_color(new_label, accent_lv_color(), 0);
    lv_obj_set_style_text_font(new_label, &LIST_ROW_FONT, 0);
    lv_obj_align(new_label, LV_ALIGN_LEFT_MID, LIST_ROW_LABEL_INSET, 0);

    char ** paths;
    int count;
    /* Persistent cache read, not a live scan -- same reasoning as
     * populate_playlists_screen()'s own comment; this screen is reachable
     * from the player's "more" menu, so it was paying the same ~5s SD-card
     * walk on every open too. */
    metadata_db_load_all_playlists(&paths, &count);
    if (count == 0) return;

    for (int i = 0; i < count; i++) {
        lv_obj_t * row = lv_obj_create(add_to_playlist_list);
        lv_obj_set_size(row, LIST_ROW_WIDTH, LIST_ROW_HEIGHT);
        lv_obj_set_style_radius(row, LIST_ROW_RADIUS, 0);
        lv_obj_set_style_bg_color(row, LIST_ROW_BG_COLOR, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, existing_playlist_row_cb, LV_EVENT_CLICKED, paths[i]); /* not freed here -- same lives-for-the-screen's-lifetime tradeoff as eq_profile_row_cb's own paths[i] */

        lv_obj_t * label = lv_label_create(row);
        lv_label_set_text(label, basename_of(paths[i]));
        lv_obj_add_style(label, &style_theme_text_primary, 0);
        lv_obj_set_style_text_font(label, &LIST_ROW_FONT, 0);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, LIST_ROW_LABEL_INSET, 0);
    }
    free(paths);
}

static lv_obj_t * build_add_to_playlist_screen(void) {
    lv_obj_t * title_label;
    return build_subsonic_list_screen("Add to Playlist", &title_label, &add_to_playlist_list);
}

/* Shared entry point into the Add to Playlist picker -- the player's own
 * "..." menu (more_menu_add_to_playlist_cb() below) and the song long-press
 * context menu (song_context_menu_add_to_playlist_cb(), defined with the
 * rest of that popup further down) both reach this screen for whatever
 * song path they have, not necessarily the currently-playing one. */
void open_add_to_playlist_for(const char * path) {
    snprintf(add_to_playlist_target_path, sizeof(add_to_playlist_target_path), "%s", path);
    populate_add_to_playlist_screen();
    nav_push(add_to_playlist_screen);
}

void on_cue_file_selected(const char * cue_path);
static lv_obj_t * build_files_screen(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &style_theme_screen_bg, 0);

    lv_obj_t * back_btn = lv_obj_create(scr);
    lv_obj_set_size(back_btn, 64, 64);
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, 0, STATUS_BAR_CLEARANCE);
    lv_obj_set_style_bg_opa(back_btn, 0, 0);
    lv_obj_set_style_border_width(back_btn, 0, 0);
    lv_obj_remove_flag(back_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(back_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back_btn, generic_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * back_arrow = lv_image_create(back_btn);
    lv_image_set_src(back_arrow, asset_path("sub_back/btn_back.png"));
    lv_obj_center(back_arrow);

    lv_obj_t * title = lv_label_create(scr);
    lv_label_set_text(title, "Files");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, STATUS_BAR_CLEARANCE + (TITLE_ROW_HEIGHT - 28) / 2);
    lv_obj_add_style(title, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(title, gui_theme_font(GUI_FONT_ROLE_TITLE), 0);

    file_browser_init(scr, MUSIC_ROOT_DIR, on_file_browser_selected, on_cue_file_selected);

    finalize_screen_navigation(scr);
    return scr;
}

/* One song within a group_songs_screen listing (Artist/Album Artist's own
 * albums, one album, Favorites, Most Played, a user .m3u playlist) -- an
 * owned path + precomputed display title, resolved via a targeted DB query
 * per screen-open rather than an index into any whole-library array. Every
 * one of those screens is inherently small (one album's tracks, a hand-
 * curated playlist, MOST_PLAYED_LIMIT), so a DB query plus a handful of
 * owned strings costs nothing next to what this app used to spend loading
 * the whole library into memory just to hand this screen a few dozen
 * entries. */
/* group_song_entry_t defined in gui_library.h */

void free_group_song_entries(group_song_entry_t * entries, int count) {
    if (!entries) return;
    for (int i = 0; i < count; i++) {
        free(entries[i].path);
        free(entries[i].title);
    }
    free(entries);
}

bool copy_group_song_entries(group_song_entry_t ** out, const group_song_entry_t * entries, int count) {
    *out = NULL;
    if (count <= 0) return true;
    group_song_entry_t * copy = calloc((size_t) count, sizeof(*copy));
    if (!copy) return false;
    for (int i = 0; i < count; i++) {
        copy[i].path = strdup(entries[i].path ? entries[i].path : "");
        copy[i].title = strdup(entries[i].title ? entries[i].title : "");
        if (!copy[i].path || !copy[i].title) {
            free_group_song_entries(copy, count);
            return false;
        }
    }
    *out = copy;
    return true;
}

/* search_remap_index */
int search_remap_index(search_binding_id_t binding_id, int display_index);

/* Paged All Songs -- see build_all_songs_screen()'s own comment. offset is
 * a position in the DB's own title-sorted order (metadata_db_get_songs_
 * filtered_page() with every filter NULL), the exact same order playlist_
 * lazy_sort_order/on_file_selected_lazy_all_songs() already assume, so a
 * row tapped here and the playback queue it starts always agree on what
 * comes next. Heap-allocated, not a stack array -- see remote_control.c's
 * own build_library_json() comment on why a COMPACT_LIST_PAGE_CACHE_SIZE-
 * sized array of song_row_t (~1.25KB each) has no business on a stack. */
static const char * song_quality_asset_for_path(const char * path) {
    const char * ext = strrchr(path ? path : "", '.');
    if (!ext) return "";
    if (!strcasecmp(ext, ".dsf") || !strcasecmp(ext, ".dff"))
        return "touch_list/quality_hr.png";
    if (!strcasecmp(ext, ".flac") || !strcasecmp(ext, ".wav") || !strcasecmp(ext, ".aif") ||
        !strcasecmp(ext, ".aiff") || !strcasecmp(ext, ".ape") || !strcasecmp(ext, ".alac"))
        return "touch_list/quality_high.png";
    return "touch_list/quality_nomal.png";
}

static void fill_song_page_visual(compact_list_page_row_t * out, const song_row_t * row) {
    metadata_db_song_display_title(row, out->label, sizeof(out->label));
    out->identity = row->id;
    snprintf(out->trailing_asset, sizeof(out->trailing_asset), "%s", song_quality_asset_for_path(row->path));
}

static int all_songs_fetch_page(void * ctx, int offset, int count, compact_list_page_row_t out_rows[]) {
    (void) ctx;
    song_row_t * rows = malloc(sizeof(song_row_t) * (size_t) count);
    int n = rows ? metadata_db_get_songs_filtered_page(NULL, NULL, NULL, NULL, offset, count, rows) : 0;
    for (int i = 0; i < n; i++) fill_song_page_visual(&out_rows[i], &rows[i]);
    free(rows);
    return n;
}

/* Resolves one All-Songs display position to a real path -- for the
 * long-press context menu, which needs a path directly (unlike the click
 * handler, which goes through play_track_at_from() -> playlist_path_at()'s
 * own resolution instead). false if display_index is out of range (e.g.
 * raced a rescan shrinking the library between the tap and this lookup --
 * same stale-reference tolerance as playlist_path_at()'s own comment). */
static bool all_songs_resolve_path_at(int display_index, char * out, size_t out_size) {
    song_row_t row;
    if (metadata_db_get_songs_filtered_page(NULL, NULL, NULL, NULL, display_index, 1, &row) != 1) return false;
    snprintf(out, out_size, "%s", row.path);
    return true;
}

static void all_songs_row_click_cb(int display_index) {
    display_index = search_remap_index(SEARCH_BINDING_ALL_SONGS, display_index);
    /* on_file_selected_lazy_all_songs() builds the queue lazily, identity-
     * mapped into the DB's own title-sorted order (the same display order
     * this screen's paged provider uses) instead of eagerly strdup'ing
     * every one of the library's paths on every tap -- see its own
     * comment. Tap-to-play from All Songs is DB-driven end to end now. */
    set_player_source_all_songs(display_index);
    on_file_selected_lazy_all_songs(display_index);
}

static void all_songs_row_long_press_cb(int display_index) {
    display_index = search_remap_index(SEARCH_BINDING_ALL_SONGS, display_index);
    char path[600];
    if (all_songs_resolve_path_at(display_index, path, sizeof(path))) open_song_context_menu(path);
}

/* Real-device fix: this used to build items[] eagerly from all_songs_
 * paths/all_songs_sort_order (all_songs_count of them), which meant every
 * tap-to-play (via ensure_library_arrays_loaded(), a prerequisite for that
 * array to even exist) cost O(library) memory and CPU regardless of which
 * one song was actually wanted -- confirmed at real risk of exhausting
 * this device's 55MB RAM for a large enough library (see this session's
 * own measurements). Now paged (compact_list_set_paged_provider()): built
 * and immediately activated against the current library, populated a
 * bounded page at a time as the list scrolls, regardless of library size.
 * Both call sites (gui_init(), refresh_library_screens_after_reload()) run
 * this after metadata_db.c is already open (library_load_from_cache_only()
 * always precedes the first, and the DB has been open since boot by the
 * time the second's rescan/reinsert-triggered rebuild can happen), so
 * metadata_db_get_song_count() here always reflects the real library, not
 * an unopened DB's 0. */
static lv_obj_t * build_all_songs_screen(void) {
    lv_obj_t * scr = build_compact_list_screen("All Songs", generic_back_cb, NULL, 0, all_songs_row_click_cb,
                                                all_songs_row_long_press_cb, &all_songs_list, NULL,
                                                LIST_ROW_WIDTH_WIDE, true, accent_lv_color());
    compact_list_set_row_height(all_songs_list, MUSIC_LIST_ROW_HEIGHT);
    compact_list_set_paged_provider(all_songs_list, all_songs_fetch_page, NULL, (int) metadata_db_get_song_count());
    finalize_screen_navigation(scr);
    return scr;
}

/* Recently Added -- same paged compact_list architecture as All Songs above
 * (see build_all_songs_screen()'s own comment on why: a bounded page at a
 * time regardless of library size, not the show_group_songs()/group_song_
 * entry_t path Favorites/Most Played use, which loads its whole, small,
 * deliberately-capped result set into RAM up front -- Recently Added has no
 * such cap, it's every song in the library just reordered by first_seen, so
 * it needs the same scale guarantee All Songs has). Only difference from
 * All Songs is the ORDER BY (first_seen DESC, rowid DESC via metadata_db_
 * get_songs_page_by_recency() instead of title via metadata_db_get_songs_
 * filtered_page()) -- no search/A-Z index wired up here, unlike All Songs,
 * since neither makes as much sense against a recency-ordered list. */
static int recently_added_fetch_page(void * ctx, int offset, int count, compact_list_page_row_t out_rows[]) {
    (void) ctx;
    song_row_t * rows = malloc(sizeof(song_row_t) * (size_t) count);
    int n = rows ? metadata_db_get_songs_page_by_recency(offset, count, rows) : 0;
    for (int i = 0; i < n; i++) fill_song_page_visual(&out_rows[i], &rows[i]);
    free(rows);
    return n;
}

/* Same role as all_songs_resolve_path_at() above, against the recency order. */
static bool recently_added_resolve_path_at(int display_index, char * out, size_t out_size) {
    song_row_t row;
    if (metadata_db_get_songs_page_by_recency(display_index, 1, &row) != 1) return false;
    snprintf(out, out_size, "%s", row.path);
    return true;
}

static void recently_added_row_click_cb(int display_index) {
    /* on_file_selected_lazy_recently_added() builds the queue lazily,
     * identity-mapped into the DB's own first_seen-DESC order (the same
     * display order this screen's paged provider uses) -- see its own
     * comment. Tap-to-play here is DB-driven end to end, same as All Songs. */
    set_player_source_recently_added(display_index);
    on_file_selected_lazy_recently_added(display_index);
}

static void recently_added_row_long_press_cb(int display_index) {
    char path[600];
    if (recently_added_resolve_path_at(display_index, path, sizeof(path))) open_song_context_menu(path);
}

static lv_obj_t * build_recently_added_screen(void) {
    lv_obj_t * scr = build_compact_list_screen("Recently Added", generic_back_cb, NULL, 0, recently_added_row_click_cb,
                                                recently_added_row_long_press_cb, &recently_added_list, NULL,
                                                LIST_ROW_WIDTH_WIDE, true, accent_lv_color());
    compact_list_set_row_height(recently_added_list, MUSIC_LIST_ROW_HEIGHT);
    compact_list_set_paged_provider(recently_added_list, recently_added_fetch_page, NULL,
                                     (int) metadata_db_get_song_count());
    finalize_screen_navigation(scr);
    return scr;
}

static void all_songs_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(all_songs_screen);
}

/* Shared drill-down target for both Artists and Albums: one persistent
 * screen whose row list is rebuilt each time (same "one object, rebuilt on
 * demand" approach file_browser.c uses for its own directory listing)
 * rather than pre-building a screen per artist/album -- a real library can
 * have hundreds of those, most never opened in a given session. */
static lv_obj_t * group_songs_edit_btn;
static group_song_entry_t * group_songs_entries; /* owned -- see set_group_songs_entries() */
static int group_songs_count;

/* Frees the previous group_songs_entries (if any) and replaces it with an
 * owned copy of entries[0..count) -- every show_group_songs_editable() call
 * and every "Removed from playlist" refresh goes through this single choke
 * point so the ownership rule (group_songs_entries is always either NULL or
 * a malloc'd copy this screen owns outright) never has to be re-derived at
 * each call site. Caller's own entries[] can be freed immediately after. */
static void set_group_songs_entries(const group_song_entry_t * entries, int count) {
    free_group_song_entries(group_songs_entries, group_songs_count);
    group_songs_entries = NULL;
    group_songs_count = copy_group_song_entries(&group_songs_entries, entries, count) ? count : 0;
}

/* A pathological album can contain tens of thousands of tracks. Building one
 * LVGL object per track caused a large post-scan RSS spike and could make ADB
 * unresponsive. Keep the backing group intact for playback, but materialize
 * only one bounded page of rows at a time. */
#define GROUP_SONGS_PAGE_SIZE 200
static int group_songs_page_start;

/* Now-playing indicator bar -- recreated fresh every populate_group_songs_
 * rows() call (that function's own lv_obj_clean(group_songs_list) destroys
 * whatever was here before, same as every row), so this pointer is only
 * ever valid between one populate call and the next, never stale across
 * one -- see refresh_group_songs_now_playing_indicator()'s own comment. */
static lv_obj_t * group_songs_now_playing_bar;

/* clear_player_source()/set_player_source_all_songs() etc. are defined
 * right after on_file_selected() -- this one needs group_songs_entries/
 * count/title_label above already in scope, which those didn't. */
/* Split out of set_player_source_group_songs() below so a caller that has
 * its own entries/title -- not the on-device Group Songs screen's own
 * group_songs_entries/title_label -- can set the same source kind without
 * touching that screen's shared, mutable display state. Used by remote
 * control's scoped play (play_remote_control_song()), which deliberately
 * never nav_pushes group_songs_screen and so must not reuse (and risk
 * replacing, via set_group_songs_entries(), out from under whatever that
 * screen currently has on-device) its live globals. */
/* set_player_source_group_songs_direct defined in gui.c */


static void set_player_source_group_songs(int pos) {
    set_player_source_group_songs_direct(group_songs_entries, group_songs_count,
                                          lv_label_get_text(group_songs_title_label), pos);
    current_settings.last_source_kind = group_songs_source_is_album ? 2 : 0;
    snprintf(current_settings.last_source_name, sizeof(current_settings.last_source_name), "%s",
             group_songs_source_is_album ? lv_label_get_text(group_songs_title_label) : "");
}

/* Playlist design change: user .m3u playlists needed a way to remove a song
 * again after adding it. Non-NULL only when the group currently shown is a
 * user-created .m3u playlist (set by show_m3u_playlist() below, via
 * show_group_songs_editable()) -- stays NULL for Artists/Albums/Favorites/
 * Most Played, none of which back onto a file this app can rewrite (Favorites
 * is metadata_db-backed, Most Played is derived play-count ranking, and
 * unfavoriting/uncounting a song isn't what "remove from playlist" means for
 * either). Borrowed from playlists_m3u_paths[], same lifetime guarantee
 * show_favorites()/show_most_played()'s own indices arrays already rely on
 * (valid for as long as this screen is showing it). group_songs_edit_mode is
 * reset to false on every fresh entry into this screen so leaving and
 * re-entering never starts already in edit mode. */
static const char * group_songs_edit_m3u_path = NULL;
static bool group_songs_edit_mode = false;

/* Defined near show_m3u_playlist() further down, where the playlist file
 * helpers are in scope -- forward-declared here since
 * populate_group_songs_rows() needs to wire it up as the remove icon's
 * click handler. */
static void group_song_remove_row_cb(lv_event_t * e);
static void populate_group_songs_rows(void);

static void group_songs_prev_page_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || group_songs_page_start <= 0) return;
    group_songs_page_start -= GROUP_SONGS_PAGE_SIZE;
    if (group_songs_page_start < 0) group_songs_page_start = 0;
    populate_group_songs_rows();
}

static void group_songs_next_page_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int next = group_songs_page_start + GROUP_SONGS_PAGE_SIZE;
    if (next >= group_songs_count) return;
    group_songs_page_start = next;
    populate_group_songs_rows();
}

static lv_obj_t * add_group_songs_page_row(const char * text, lv_event_cb_t cb) {
    lv_obj_t * row = lv_label_create(group_songs_list);
    lv_obj_add_style(row, &list_row_style, 0);
    lv_obj_add_style(row, &list_row_pressed_style, LV_STATE_PRESSED);
    row_label_enable_marquee(row);
    lv_obj_set_style_height(row, MUSIC_LIST_ROW_HEIGHT, LV_PART_MAIN);
    lv_obj_set_style_pad_top(row,
        (MUSIC_LIST_ROW_HEIGHT - lv_font_get_line_height(&LIST_ROW_FONT)) / 2, LV_PART_MAIN);
    lv_obj_set_style_text_align(row, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_label_set_text(row, text);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, NULL);
    return row;
}

/* Defined near populate_playlists_screen() further down -- forward-declared
 * here since group_song_remove_row_cb() (below) and playlist_row_click_cb()
 * both need to refresh the Playlists screen's list after auto-deleting a
 * playlist that's become empty. */
static void populate_playlists_screen(void);

/* Real-device incident: LVGL still sends LV_EVENT_CLICKED on release even
 * when LV_EVENT_LONG_PRESSED already fired earlier in that same press --
 * same root cause/fix as quick_drawer_wifi_long_press_cb's own doc comment
 * (search that name for the full story) and compact_list_row_click_cb's
 * own matching fix (screen_builders.c) for All Songs -- without this, long-
 * pressing a Group Songs row (Artist/Album/Playlist/Favorites/Most Played)
 * to open the context menu also started that song playing on release. One
 * flag, not per-row -- single-touch device, only one row can plausibly be
 * mid-press at a time. */
static bool group_song_row_long_press_fired = false;

static void group_song_row_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (group_song_row_long_press_fired) {
        group_song_row_long_press_fired = false;
        return;
    }
    int pos = (int) (intptr_t) lv_event_get_user_data(e); /* position within the CURRENT group, not the whole library */

    char ** playlist_copy = malloc(sizeof(char *) * (size_t) group_songs_count);
    for (int i = 0; i < group_songs_count; i++) playlist_copy[i] = strdup(group_songs_entries[i].path);
    set_player_source_group_songs(pos);
    on_file_selected(playlist_copy, group_songs_count, pos);
}

static void group_song_row_long_press_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_LONG_PRESSED) return;
    group_song_row_long_press_fired = true;
    int pos = (int) (intptr_t) lv_event_get_user_data(e);
    open_song_context_menu(group_songs_entries[pos].path);
}

/* Positions/shows or hides group_songs_now_playing_bar against the CURRENT
 * group_songs_entries/count -- callable standalone (no row rebuild, no
 * scroll reset) whenever now_playing_path changes while this screen
 * is open, and also called once at the end of populate_group_songs_rows()
 * itself so a freshly opened group (or an edit-mode toggle, which also goes
 * through a full repopulate) starts with the right state. Row height/gap
 * (LIST_ROW_HEIGHT+4, 4) are the same literals build_group_songs_screen()
 * already gives this list's own pad_top/pad_gap -- every row here is a
 * uniform LIST_ROW_HEIGHT regardless of edit mode, so row i's y is exactly
 * this formula even though this list is flex-laid-out (not manually
 * positioned like the compact-list infra's own pool). */
static void refresh_group_songs_now_playing_indicator(void) {
    if (!group_songs_now_playing_bar) return;

    int match = -1;
    if (now_playing_path[0]) {
        for (int i = 0; i < group_songs_count; i++) {
            if (strcmp(group_songs_entries[i].path, now_playing_path) == 0) { match = i; break; }
        }
    }

    int page_end = group_songs_page_start + GROUP_SONGS_PAGE_SIZE;
    if (page_end > group_songs_count) page_end = group_songs_count;
    if (match < group_songs_page_start || match >= page_end) {
        lv_obj_add_flag(group_songs_now_playing_bar, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    int visible_row = match - group_songs_page_start + (group_songs_page_start > 0 ? 1 : 0);
    lv_obj_remove_flag(group_songs_now_playing_bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(group_songs_now_playing_bar, 0, 4 + visible_row * (MUSIC_LIST_ROW_HEIGHT + 4));
}

/* Rebuilds group_songs_list's rows from whatever group_songs_entries/count
 * currently hold, in either of two shapes: a plain tappable-to-play label
 * (list_row_style, same as every other group -- Artists/Albums/Favorites/
 * Most Played always use this one) when not actively editing a playlist, or
 * a row with a trailing remove icon (no play-on-tap -- see
 * group_song_remove_row_cb() below) when group_songs_edit_mode is on for an
 * editable .m3u playlist. Split out from show_group_songs_editable() so the
 * remove callback and the Edit/Done toggle can both redraw in place without
 * re-deriving the group or nav_push()ing a second copy of this screen. */
static void populate_group_songs_rows(void) {
    lv_obj_clean(group_songs_list);

    bool editable = group_songs_edit_m3u_path != NULL;
    if (editable) {
        lv_obj_clear_flag(group_songs_edit_btn, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(group_songs_edit_btn, group_songs_edit_mode ? "Done" : "Edit");
    } else {
        lv_obj_add_flag(group_songs_edit_btn, LV_OBJ_FLAG_HIDDEN);
    }

    bool editing = editable && group_songs_edit_mode;

    if (group_songs_count <= GROUP_SONGS_PAGE_SIZE) group_songs_page_start = 0;
    if (group_songs_page_start >= group_songs_count && group_songs_count > 0) {
        group_songs_page_start = ((group_songs_count - 1) / GROUP_SONGS_PAGE_SIZE) * GROUP_SONGS_PAGE_SIZE;
    }
    int page_end = group_songs_page_start + GROUP_SONGS_PAGE_SIZE;
    if (page_end > group_songs_count) page_end = group_songs_count;

    if (group_songs_page_start > 0) {
        char page_text[96];
        snprintf(page_text, sizeof(page_text), "Previous  •  %d–%d of %d",
                 group_songs_page_start + 1, page_end, group_songs_count);
        add_group_songs_page_row(page_text, group_songs_prev_page_cb);
    }

    for (int i = group_songs_page_start; i < page_end; i++) {
        if (editing) {
            /* Container row (label + remove icon), same shape as e.g.
             * build_wifi_screen()'s saved/scanned network rows -- unlike the
             * plain-label rows below, this needs a second child so it can't
             * reuse list_row_style as-is. */
            lv_obj_t * row = lv_obj_create(group_songs_list);
            lv_obj_set_size(row, LIST_ROW_WIDTH, MUSIC_LIST_ROW_HEIGHT);
            lv_obj_set_style_radius(row, LIST_ROW_RADIUS, 0);
            lv_obj_set_style_bg_color(row, LIST_ROW_BG_COLOR, 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(row, 0, 0);
            lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

            lv_obj_t * label = lv_label_create(row);
            lv_label_set_text(label, group_songs_entries[i].title);
            lv_obj_add_style(label, &style_theme_text_primary, 0);
            lv_obj_set_style_text_font(label, &LIST_ROW_FONT, 0);
            lv_obj_align(label, LV_ALIGN_LEFT_MID, LIST_ROW_LABEL_INSET, 0);
            configure_scrolling_row_label(label, LIST_ROW_WIDTH - LIST_ROW_LABEL_INSET - 84);

            lv_obj_t * remove_icon = lv_image_create(row);
            lv_image_set_src(remove_icon, asset_path("touch_list/del.png"));
            lv_obj_align(remove_icon, LV_ALIGN_RIGHT_MID, -20, 0);
            lv_obj_add_flag(remove_icon, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(remove_icon, group_song_remove_row_cb, LV_EVENT_CLICKED, (void *) (intptr_t) i);
        } else {
            /* One lv_label via the shared list_row_style, not a container +
             * child label each with their own local style properties -- see
             * list_row_style's own doc comment (screen_builders.h). */
            lv_obj_t * row = lv_label_create(group_songs_list);
            lv_obj_add_style(row, &list_row_style, 0);
            lv_obj_add_style(row, &list_row_pressed_style, LV_STATE_PRESSED);
            row_label_enable_marquee(row);
            lv_obj_set_style_height(row, MUSIC_LIST_ROW_HEIGHT, LV_PART_MAIN);
            lv_obj_set_style_pad_top(row,
                (MUSIC_LIST_ROW_HEIGHT - lv_font_get_line_height(&LIST_ROW_FONT)) / 2, LV_PART_MAIN);
            lv_obj_set_style_pad_right(row, 70, 0);
            lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
            lv_label_set_text(row, group_songs_entries[i].title);

            lv_obj_t * quality = lv_image_create(row);
            lv_image_set_src(quality, asset_path(song_quality_asset_for_path(group_songs_entries[i].path)));
            /* Child alignment is relative to the label's padded content
             * box. Cancel the 70px text reserve so the badge is physically
             * 14px from the card edge (same rule as compact-list rows). */
            int32_t quality_pad_top = lv_obj_get_style_pad_top(row, LV_PART_MAIN);
            int32_t quality_pad_bottom = lv_obj_get_style_pad_bottom(row, LV_PART_MAIN);
            lv_obj_align(quality, LV_ALIGN_RIGHT_MID, 70 - 14,
                         (quality_pad_top - quality_pad_bottom) / -2);
            lv_obj_remove_flag(quality, LV_OBJ_FLAG_CLICKABLE);

            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(row, group_song_row_click_cb, LV_EVENT_CLICKED, (void *) (intptr_t) i);
            lv_obj_add_event_cb(row, group_song_row_long_press_cb, LV_EVENT_LONG_PRESSED, (void *) (intptr_t) i);
        }
    }

    if (page_end < group_songs_count) {
        char page_text[96];
        snprintf(page_text, sizeof(page_text), "Next  •  %d–%d of %d",
                 group_songs_page_start + 1, page_end, group_songs_count);
        add_group_songs_page_row(page_text, group_songs_next_page_cb);
    }

    /* Recreated fresh here (lv_obj_clean() above just destroyed whatever
     * was here before) rather than kept as a truly persistent object --
     * every row in this list gets rebuilt on every populate call already,
     * so this just follows the same pattern. LV_OBJ_FLAG_IGNORE_LAYOUT
     * keeps this list's own flex column layout from trying to stack it in
     * as another row, same trick build_icon_grid_screen() uses for its
     * divider lines. Positioned/shown by refresh_group_songs_now_playing_
     * indicator() below, not here -- that also runs standalone (no rebuild)
     * whenever now_playing_path changes while this screen stays
     * open, e.g. a gapless auto-advance to the next track in the group. */
    group_songs_now_playing_bar = lv_obj_create(group_songs_list);
    lv_obj_remove_style_all(group_songs_now_playing_bar);
    lv_obj_set_size(group_songs_now_playing_bar, 5, MUSIC_LIST_ROW_HEIGHT);
    lv_obj_set_style_bg_color(group_songs_now_playing_bar, accent_lv_color(), 0);
    lv_obj_set_style_bg_opa(group_songs_now_playing_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(group_songs_now_playing_bar, 2, 0);
    lv_obj_add_flag(group_songs_now_playing_bar, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_remove_flag(group_songs_now_playing_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(group_songs_now_playing_bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(group_songs_now_playing_bar, LV_OBJ_FLAG_HIDDEN);
    refresh_group_songs_now_playing_indicator();
}

/* The player screen's "List" option -- reopens whichever screen the
 * current track was tapped from, scrolled back to it. Forward-declared
 * near the other more_menu_*_cb functions (build_more_menu_popup() wires
 * it up there); defined here instead since PLAYER_SOURCE_GROUP_SONGS
 * needs group_songs_screen/list/entries/count/title_label and
 * populate_group_songs_rows() all already in scope. */
void more_menu_list_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_more_menu_popup();

    switch (player_source_kind) {
    case PLAYER_SOURCE_ALL_SONGS:
        nav_push(all_songs_screen);
        compact_list_scroll_to_index(all_songs_list, player_source_all_songs_index);
        break;
    case PLAYER_SOURCE_RECENTLY_ADDED:
        nav_push(recently_added_screen);
        compact_list_scroll_to_index(recently_added_list, player_source_recently_added_index);
        break;
    case PLAYER_SOURCE_GROUP_SONGS:
        /* View-only when reached via List, even if the source group was
         * an editable .m3u playlist -- this is for orientation/navigation,
         * not editing, and group_songs_edit_m3u_path's own lifetime
         * guarantee (valid only "as long as this screen is showing it",
         * per its own comment) doesn't cover this snapshot anyway. */
        group_songs_edit_m3u_path = NULL;
        group_songs_edit_mode = false;
        group_songs_source_is_album = current_settings.last_source_kind == 2;
        set_group_songs_entries(player_source_group_entries, player_source_group_count);
        group_songs_page_start = player_source_group_pos >= 0
                                     ? (player_source_group_pos / GROUP_SONGS_PAGE_SIZE) * GROUP_SONGS_PAGE_SIZE
                                     : 0;
        lv_label_set_text(group_songs_title_label, player_source_group_title ? player_source_group_title : "");
        populate_group_songs_rows();
        nav_push(group_songs_screen);
        lv_obj_update_layout(group_songs_list);
        int visible_pos = player_source_group_pos - group_songs_page_start + (group_songs_page_start > 0 ? 1 : 0);
        if (visible_pos >= 0 && visible_pos < (int) lv_obj_get_child_count(group_songs_list)) {
            lv_obj_scroll_to_view(lv_obj_get_child(group_songs_list, visible_pos), LV_ANIM_OFF);
        }
        break;
    case PLAYER_SOURCE_FILE_BROWSER:
        nav_push(files_screen);
        file_browser_navigate_to(player_source_file_browser_dir, player_source_file_browser_row);
        break;
    case PLAYER_SOURCE_NONE:
    default:
        show_error_toast("No source list for this track");
        break;
    }
}

/* Every screen's back button is a fixed 64x64 at the screen's own left
 * edge (build_group_songs_screen() below, build_subsonic_list_screen()) --
 * this is where a title label should start, not centered over top of it. */
#define TITLE_LABEL_LEFT_INSET 76 /* 64px back button + 12px breathing room */
/* Default right margin for a title with no right-side icon of its own on
 * that particular screen -- most build_subsonic_list_screen() callers
 * (Playlists, Saved Servers, New Connection, Queue, DLNA, Remote Control,
 * the plugin list pool, ...) never get anywhere near this wide anyway
 * (always a short fixed string), so this is mostly headroom for the ones
 * that do have a long dynamic title but no button beside it (the local
 * library's Artist -> Albums drill-down, artist_albums_screen). */
#define TITLE_LABEL_DEFAULT_RIGHT_MARGIN 20

/* Narrows an already left-aligned, auto-scrolling title label (see
 * build_subsonic_list_screen()'s and build_group_songs_screen()'s own
 * construction) so its right edge stops before `right_icon`'s own left
 * edge, instead of running underneath it -- call once, right after
 * right_icon's own final on-screen position is set (real coordinates, not
 * a pending layout -- lv_label/lv_image both size/position synchronously
 * on lv_obj_align(), no intervening refresh pass needed). Safe to call
 * even while right_icon is currently hidden (e.g. a Download button only
 * shown for some views of a reused screen) -- a hidden object still has a
 * real position, and reserving room for it whether or not it's showing
 * right now is simpler and safer than tracking two different widths.
 *
 * Uses lv_obj_get_coords(), NOT lv_obj_get_x() -- real-device bug report:
 * lv_obj_get_x() reflects the coordinate as set relative to whichever
 * alignment reference the object was last lv_obj_align()'d against (e.g. a
 * TOP_RIGHT-aligned button's "x" isn't a left-relative position at all), so
 * comparing it against title's own TOP_LEFT-relative x produced nonsense.
 * lv_obj_get_coords() always returns real absolute screen coordinates
 * regardless of how the object was positioned, so title_area.x1/icon_area.x1
 * are directly comparable -- BUT only once a layout pass has actually run:
 * lv_obj_align() (lv_obj_pos.c) just sets style properties (align + offset)
 * for the layout engine to resolve LATER, it does not compute real
 * coordinates on the spot. Without lv_obj_update_layout() first,
 * lv_obj_get_coords() here was reading stale/uncommitted (0,0) coordinates
 * from before either object was ever positioned -- second real-device bug
 * report, same "only a couple characters visible" symptom as the
 * lv_obj_get_x() bug this replaced, different root cause. Forcing the
 * layout pass on the screen (both objects' common parent) resolves both at
 * once. */
void reserve_title_width_before(lv_obj_t * title, lv_obj_t * right_icon) {
    lv_obj_update_layout(lv_obj_get_parent(title));

    lv_area_t title_area, icon_area;
    lv_obj_get_coords(title, &title_area);
    lv_obj_get_coords(right_icon, &icon_area);
    int32_t width = icon_area.x1 - title_area.x1 - 12;
    if (width < 40) width = 40; /* never collapse to nothing/negative on a pathological layout */
    lv_obj_set_width(title, width);
}

static void group_songs_edit_btn_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    group_songs_edit_mode = !group_songs_edit_mode;
    populate_group_songs_rows();
}

/* entries[0..count) is copied into group_songs_entries (see set_group_songs_
 * entries()) -- the caller's own array/buffer can be freed immediately after
 * this call returns, unlike the old group_t-based API where the group_t's
 * .indices had to stay valid for as long as this screen kept showing it. */
static void show_group_songs_editable(const char * name, const group_song_entry_t * entries, int count,
                                       const char * editable_m3u_path) {
    group_songs_source_is_album = false;
    group_songs_edit_m3u_path = editable_m3u_path;
    group_songs_edit_mode = false;
    set_group_songs_entries(entries, count);
    group_songs_page_start = 0;

    lv_label_set_text(group_songs_title_label, name);
    populate_group_songs_rows();

    nav_push(group_songs_screen);
}

void show_group_songs(const char * name, const group_song_entry_t * entries, int count) {
    show_group_songs_editable(name, entries, count, NULL);
}

static lv_obj_t * build_group_songs_screen(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &style_theme_screen_bg, 0);

    lv_obj_t * back_btn = lv_obj_create(scr);
    lv_obj_set_size(back_btn, 64, 64);
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, 0, STATUS_BAR_CLEARANCE);
    lv_obj_set_style_bg_opa(back_btn, 0, 0);
    lv_obj_set_style_border_width(back_btn, 0, 0);
    lv_obj_remove_flag(back_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(back_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back_btn, generic_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * back_arrow = lv_image_create(back_btn);
    lv_image_set_src(back_arrow, asset_path("sub_back/btn_back.png"));
    lv_obj_center(back_arrow);

    group_songs_title_label = lv_label_create(scr);
    lv_label_set_text(group_songs_title_label, "");
    lv_obj_add_style(group_songs_title_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(group_songs_title_label, &app_font_28, 0); /* shows a metadata-derived artist/album/genre name -- see fallback_font.h */
    /* Real-device bug report: a long artist/album/genre/playlist name
     * (this label's whole reason for existing, per the comment above)
     * centered via LV_ALIGN_TOP_MID ran directly under the back button on
     * the left and the Edit button on the right. Left-aligned starting
     * just past the back button, narrowed below (once group_songs_edit_btn
     * itself exists) to stop before it, with auto-scroll for whatever
     * still overflows -- same fix as build_subsonic_list_screen()'s own
     * title. */
    int32_t scr_w = lv_display_get_horizontal_resolution(lv_display_get_default());
    lv_obj_set_width(group_songs_title_label, scr_w - TITLE_LABEL_LEFT_INSET - TITLE_LABEL_DEFAULT_RIGHT_MARGIN);
    /* Real-device bug report: this title scrolled immediately, with none of
     * the 2s-pause-before-scrolling every other marquee in the app has --
     * root cause: setting LV_LABEL_LONG_SCROLL_CIRCULAR directly here never
     * attached row_marquee_style (the shared style carrying that 2s delay/
     * repeat-delay), so it fell back to LVGL's own built-in default
     * animation timing instead. row_label_enable_marquee() sets the long
     * mode AND attaches that style in one call -- same fix as
     * build_subsonic_list_screen()'s own title label just below. */
    row_label_enable_marquee(group_songs_title_label);
    lv_obj_align(group_songs_title_label, LV_ALIGN_TOP_LEFT, TITLE_LABEL_LEFT_INSET,
                 STATUS_BAR_CLEARANCE + (TITLE_ROW_HEIGHT - 28) / 2);

    /* Hidden by default -- populate_group_songs_rows() (via
     * show_group_songs_editable()) is what actually shows this, and only
     * for a group backed by an editable .m3u playlist. */
    group_songs_edit_btn = lv_label_create(scr);
    lv_label_set_text(group_songs_edit_btn, "Edit");
    lv_obj_set_style_text_color(group_songs_edit_btn, accent_lv_color(), 0);
    lv_obj_set_style_text_font(group_songs_edit_btn, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_obj_align(group_songs_edit_btn, LV_ALIGN_TOP_RIGHT, -20, STATUS_BAR_CLEARANCE + (TITLE_ROW_HEIGHT - 28) / 2);
    lv_obj_add_flag(group_songs_edit_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(group_songs_edit_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(group_songs_edit_btn, group_songs_edit_btn_cb, LV_EVENT_CLICKED, NULL);
    /* Reserved whether or not Edit is currently visible -- see
     * reserve_title_width_before()'s own comment on why that's safe/
     * simpler than tracking two different widths. */
    reserve_title_width_before(group_songs_title_label, group_songs_edit_btn);

    group_songs_list = lv_obj_create(scr);
    lv_obj_set_size(group_songs_list, lv_pct(100),
                    lv_display_get_vertical_resolution(lv_display_get_default()) - STATUS_BAR_CLEARANCE -
                        TITLE_ROW_HEIGHT);
    lv_obj_align(group_songs_list, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(group_songs_list, 0, 0);
    lv_obj_set_style_border_width(group_songs_list, 0, 0);
    /* Real-device bug report: same root cause as build_compact_list_widget()'s
     * own fix (see that function's comment) -- rows here (Artist/Album
     * Artist's own song/album drill-down list) were shifted right, clipped
     * against the screen edge with a gap on the left. */
    lv_obj_set_style_pad_all(group_songs_list, 0, 0);
    lv_obj_set_scroll_dir(group_songs_list, LV_DIR_VER); /* see build_icon_grid_screen's comment */
    lv_obj_set_flex_flow(group_songs_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(group_songs_list, 4, 0);
    lv_obj_set_style_pad_top(group_songs_list, 4, 0);

    finalize_screen_navigation(scr);
    return scr;
}

/* Reusable on-screen keyboard text entry -- the app had none of this
 * before network streaming needed a way to type a server URL/username/
 * password. One persistent screen (title + textarea + T9 keypad),
 * repurposed for whatever's being entered via show_text_entry() rather
 * than a screen per use, same "one object, rebuilt/retargeted on demand"
 * approach as group_songs_screen above. Deliberately skips
 * finalize_screen_navigation() (no swipe-to-go-back) -- a swipe gesture
 * while typing on the keypad would be an easy accidental way to lose
 * whatever was being entered. (text_entry_done_cb_t itself is forward-
 * declared earlier, alongside show_text_entry(), for the PEQ screen's
 * tap-to-edit handlers.)
 *
 * Real T9 multi-tap keypad, replacing an earlier lv_keyboard_create()-based
 * QWERTY layout -- feature request: reuse the stock firmware's own
 * keyboard/ theme assets (confirmed present under THEME_ROOT on a real
 * device, same asset_path() convention as every other themed icon in this
 * app) for a phone-keypad-style input instead of LVGL's generic keyboard
 * widget. 12-key layout (1-9, Mode, Shift, plus dedicated Del/Left/Right/
 * Enter/Space keys) in a 4-column x 5-row grid; keys 2-9 cycle through their
 * letter group (e.g. key 2 -> a -> b -> c) on repeated taps within
 * TEXT_ENTRY_MULTITAP_MS, matching classic phone-keypad multi-tap text
 * entry. Three keypad modes (ABC/NUM/SYM), cycled via the Mode key: ABC
 * cycles letters per key (0 and 1 have no letter group on this asset set,
 * so they always insert their literal digit); NUM makes every digit key
 * insert its literal digit directly, no cycling; SYM cycles each key's own
 * punctuation set (symbol0.png..symbol9.png). PEQ's numeric-only fields
 * (show_text_entry()'s `numeric` flag) skip all three and lock permanently
 * to plain-digit entry, hiding Mode/Shift (neither means anything for a
 * frequency/gain/Q value) and exposing a dedicated "./-" key instead (gain
 * needs negative numbers; nothing else here provides them once Mode is
 * hidden).
 *
 * Scope cuts, deliberate for this first pass: num_more.png/symbol_more.png
 * (a second punctuation page) aren't wired up -- symbol0-9 alone covers
 * ordinary punctuation (. , ! ? etc.), and every text_entry field here is a
 * short single-line value (URLs, names, credentials), not prose. The
 * select/ subfolder's pressed-state art isn't used either (no PRESSED/
 * RELEASED image swap, unlike screen_builders.c's icon-grid tiles) --
 * LV_EVENT_CLICKED alone is enough feedback for a keypad tap. bg.png/
 * text_bg.png/cursor.png (decorative chrome) and ok.png/ok_s.png (redundant
 * with enter.png's own baked-in "Enter" text) are unused. char_l.png/
 * char_u.png are never referenced -- confirmed byte-identical to each other
 * and to no clear distinct purpose from char.png (the Mode key's own ABC
 * glyph) by direct pixel inspection. */

/* ---- Virtualized local-album thumbnails -------------------------------
 * Only the 20 recycled compact-list rows can request artwork. One worker at
 * a time reads/decodes a representative song's embedded or Rockbox albumart
 * file, while a 32-entry RGB565 LRU cache keeps the visible window plus
 * scroll headroom bounded at ~324 KiB. Persistent sized files live in
 * MUSIC_ROOT_DIR/.open_hiby_player/albumart/<artist>-<album>.72x72.bmp. */
#define ALBUM_THUMBNAIL_PX 72
#define ALBUM_THUMBNAIL_CACHE_SIZE 32

typedef struct {
    int64_t song_id;
    bool known; /* true even when pixels==NULL: negative cache for albums without art */
    uint8_t * pixels;
    lv_image_dsc_t dsc;
    uint32_t last_use;
} album_thumbnail_cache_entry_t;

typedef struct {
    int64_t song_id;
    int generation;
    int logical_index;
    lv_obj_t * list;
} album_thumbnail_request_t;

#define ALBUM_THUMBNAIL_QUEUE_SIZE 20

static album_thumbnail_cache_entry_t album_thumbnail_cache[ALBUM_THUMBNAIL_CACHE_SIZE];
static uint32_t album_thumbnail_use_counter;
static pthread_t album_thumbnail_thread;
bool album_thumbnail_active = false;
static atomic_bool album_thumbnail_done;
static int64_t album_thumbnail_result_song_id;
static int album_thumbnail_result_generation;
static int album_thumbnail_result_logical_index;
static lv_obj_t * album_thumbnail_result_list;
static uint8_t * album_thumbnail_result_pixels;
static lv_timer_t * album_thumbnail_poll_timer;
static album_thumbnail_request_t album_thumbnail_queue[ALBUM_THUMBNAIL_QUEUE_SIZE];
static int album_thumbnail_queue_count;
static bool album_thumbnail_scrolling;
static bool album_thumbnail_list_is_visible(lv_obj_t * list) {
    if (!list) return false;
    lv_obj_t * active = lv_screen_active();
    return (list == albums_list && active == albums_screen) ||
           (list == artist_albums_list && active == artist_albums_screen);
}

static album_thumbnail_cache_entry_t * album_thumbnail_cache_find(int64_t song_id) {
    for (int i = 0; i < ALBUM_THUMBNAIL_CACHE_SIZE; i++) {
        if (album_thumbnail_cache[i].known && album_thumbnail_cache[i].song_id == song_id) {
            album_thumbnail_cache[i].last_use = ++album_thumbnail_use_counter;
            return &album_thumbnail_cache[i];
        }
    }
    return NULL;
}

static void album_thumbnail_cache_clear(void) {
    for (int i = 0; i < ALBUM_THUMBNAIL_CACHE_SIZE; i++) {
        free(album_thumbnail_cache[i].pixels);
        memset(&album_thumbnail_cache[i], 0, sizeof(album_thumbnail_cache[i]));
    }
    album_thumbnail_use_counter = 0;
}

static bool album_thumbnail_sized_cache_hit(const albumart_info_t * info, char * found, size_t found_size) {
    return albumart_sized_thumb_fresh(info, ALBUM_THUMBNAIL_PX, ALBUM_THUMBNAIL_PX, found, found_size);
}

/* Rockbox albumart search, then embedded picture. A successful decode is
 * written as MUSIC_ROOT_DIR/.open_hiby_player/albumart/<artist>-<album>.72x72.bmp
 * so the next pass is a small BMP load instead of a JPEG/PNG decode.
 * Albums without artist+album tags still decode, but cannot be stored. */
static bool album_thumbnail_load_or_decode(const song_row_t * song, uint16_t ** out_pixels) {
    *out_pixels = NULL;
    if (!song || !song->path[0]) return false;

    albumart_info_t info;
    albumart_info_from_song_row(song, &info);

    char found[PATH_MAX];
    uint8_t * data = NULL;
    uint32_t size = 0;

    if (album_thumbnail_sized_cache_hit(&info, found, sizeof(found))) {
        bool ok = albumart_load_file(found, &data, &size, EXTERNAL_COVER_MAX_BYTES) &&
                  cover_decode_to_rgb565(data, size, ALBUM_THUMBNAIL_PX, ALBUM_THUMBNAIL_PX, out_pixels);
        free(data);
        return ok;
    }

    if (albumart_search_files(&info, "", found, sizeof(found))) {
        /* Sidecar present. A too-large or unreadable file must not fall
         * through into metadata_read() of the audio file -- that is the
         * in-process ID3/FLAC parse the 4 MiB / 1200px caps exist to avoid. */
        albumart_load_file(found, &data, &size, EXTERNAL_COVER_MAX_BYTES);
    } else {
        track_metadata_t meta;
        memset(&meta, 0, sizeof(meta));
        metadata_read(song->path, &meta);
        data = meta.picture_data;
        size = meta.picture_size;
        free(meta.lyrics);
        if (!info.artist[0]) snprintf(info.artist, sizeof(info.artist), "%s", meta.artist);
        if (!info.album[0]) snprintf(info.album, sizeof(info.album), "%s", meta.album);
        if (!info.albumartist[0]) snprintf(info.albumartist, sizeof(info.albumartist), "%s", meta.album_artist);
    }
    bool ok = data && size &&
              cover_decode_to_rgb565(data, size, ALBUM_THUMBNAIL_PX, ALBUM_THUMBNAIL_PX, out_pixels);
    free(data);
    if (ok && *out_pixels)
        albumart_store_rgb565(&info, ALBUM_THUMBNAIL_PX, ALBUM_THUMBNAIL_PX, *out_pixels);
    return ok;
}

static void * album_thumbnail_thread_func(void * arg) {
#ifdef UI_PERF_TRACE
    uint64_t perf_start_us = ui_perf_now_us();
#endif
    album_thumbnail_request_t * req = (album_thumbnail_request_t *) arg;
    uint16_t * pixels = NULL;
    song_row_t song;
    if (metadata_db_get_song_by_id(req->song_id, &song))
        album_thumbnail_load_or_decode(&song, &pixels);
    album_thumbnail_result_song_id = req->song_id;
    album_thumbnail_result_generation = req->generation;
    album_thumbnail_result_logical_index = req->logical_index;
    album_thumbnail_result_list = req->list;
    album_thumbnail_result_pixels = (uint8_t *) pixels;
    free(req);
    album_thumbnail_done = true;
#ifdef UI_PERF_TRACE
    printf("PERF album_thumb song=%lld total_us=%llu pixels=%d\n",
           (long long) album_thumbnail_result_song_id,
           (unsigned long long) (ui_perf_now_us() - perf_start_us),
           album_thumbnail_result_pixels != NULL);
#endif
    return NULL;
}

/* ---- Persistent (post-scan) album thumbnail generation ----------------
 * Runs once after Update Music Database completes (never before -- see
 * this file's own call site, placed right after the scan thread is joined
 * and the database is already fully committed), writing the same Rockbox
 * sized BMP files album_thumbnail_load_or_decode() produces one row at a
 * time -- so browsing Albums right after a scan shows artwork immediately
 * instead of only as each row happens to scroll into view. Reuses
 * metadata_db_get_albums_page_filtered(NULL, ...) (streamed/paged, never
 * the whole table at once) grouped by (album, album_artist) -- the same
 * grouping and first_song_id the Album Artist -> Albums drill-down's own
 * query produces for the same pair, so that screen automatically gets the
 * same cached thumbnails with no extra work here. Entirely worker-thread-
 * side: metadata_db / albumart / cover_decode never touch LVGL. Lazy
 * generation stays as the fallback for whatever this pass didn't reach
 * (interrupted by a newer scan, artwork missing/changed since, or simply
 * never run). */
static pthread_t album_thumb_gen_thread;
/* active/cancel/generation cross the UI/worker boundary and therefore must
 * be real atomics, not volatile. thread_joinable is owned by the UI thread:
 * it remains true after a worker naturally exits until that worker is
 * joined, preventing one joinable-thread resource leak per database scan. */
static atomic_bool album_thumb_gen_active;
static atomic_bool album_thumb_gen_cancel;
static atomic_int album_thumb_gen_generation;
static bool album_thumb_gen_thread_joinable;
static atomic_int album_thumb_gen_done_count;
static atomic_int album_thumb_gen_total_count;

static bool album_thumb_gen_should_cancel(int my_generation) {
    return atomic_load(&album_thumb_gen_cancel) ||
           atomic_load(&album_thumb_gen_generation) != my_generation;
}

static void cancel_album_thumbnail_generation(void) {
    if (!album_thumb_gen_thread_joinable || !atomic_load(&album_thumb_gen_active)) return;
    atomic_store(&album_thumb_gen_cancel, true);
    atomic_fetch_add(&album_thumb_gen_generation, 1);
}

static void reap_album_thumbnail_generation(void) {
    if (!album_thumb_gen_thread_joinable) return;
    pthread_join(album_thumb_gen_thread, NULL);
    album_thumb_gen_thread_joinable = false;
}

static void * album_thumb_gen_thread_func(void * arg) {
    int my_generation = (int) (intptr_t) arg;
#ifdef UI_PERF_TRACE
    uint64_t perf_start_us = ui_perf_now_us();
    int perf_generated = 0, perf_skipped = 0, perf_missing = 0, perf_failed = 0;
#endif
    atomic_store(&album_thumb_gen_done_count, 0);

#define ALBUM_THUMB_GEN_BATCH 64
#define ALBUM_THUMB_GEN_WARM_LIMIT 2048
#define ALBUM_THUMB_GEN_YIELD_US 10000
    group_row_t rows[ALBUM_THUMB_GEN_BATCH];
    int offset = 0;
    for (;;) {
        if (album_thumb_gen_should_cancel(my_generation)) break;
        /* Streamed/paged, per requirement -- never the whole album table
         * (which can be tens of thousands of rows on a large library) held
         * in memory at once, just ALBUM_THUMB_GEN_BATCH group_row_t at a
         * time (a few hundred bytes each). */
        int n = metadata_db_get_albums_page_filtered(NULL, offset, ALBUM_THUMB_GEN_BATCH, rows);
        if (n <= 0) break;
        for (int i = 0; i < n; i++) {
            if (album_thumb_gen_should_cancel(my_generation)) goto done;
            song_row_t song;
            if (!metadata_db_get_song_by_id(rows[i].first_song_id, &song)) {
#ifdef UI_PERF_TRACE
                perf_missing++;
#endif
                atomic_fetch_add(&album_thumb_gen_done_count, 1);
                continue;
            }
            /* Skip albums that already have a Rockbox sized BMP. Same
             * search the lazy path uses, so an interrupted-then-resumed
             * pass, or a re-run over an already-fully-generated library,
             * does no wasted decode work. */
            albumart_info_t info;
            albumart_info_from_song_row(&song, &info);
            char found[PATH_MAX];
            if (album_thumbnail_sized_cache_hit(&info, found, sizeof(found))) {
#ifdef UI_PERF_TRACE
                perf_skipped++;
#endif
                atomic_fetch_add(&album_thumb_gen_done_count, 1);
                continue;
            }
            uint16_t * pixels = NULL;
            album_thumbnail_load_or_decode(&song, &pixels);
            /* No-op store when pixels is NULL -- an album with no
             * resolvable art stays a lazy negative-cache case
             * (album_thumbnail_cache_find()'s own known-but-NULL entry),
             * never persisted as a permanent "no art" result, so artwork
             * added later is picked up normally. */
#ifdef UI_PERF_TRACE
            if (pixels) perf_generated++; else perf_failed++;
#endif
            free(pixels);
            atomic_fetch_add(&album_thumb_gen_done_count, 1);
            /* This is cache warming, not foreground scan work. Yield after
             * every album so sustained decoding and SD writes cannot starve
             * audio/UI work or pin the CPU at full load for minutes. */
            usleep(ALBUM_THUMB_GEN_YIELD_US);
        }
        offset += n;
        if (n < ALBUM_THUMB_GEN_BATCH || offset >= ALBUM_THUMB_GEN_WARM_LIMIT) break;
    }
done:
#ifdef UI_PERF_TRACE
    printf("PERF album_thumb_gen done=%d generated=%d skipped=%d missing=%d failed=%d us=%llu cancelled=%d\n",
           atomic_load(&album_thumb_gen_done_count), perf_generated, perf_skipped, perf_missing, perf_failed,
           (unsigned long long) (ui_perf_now_us() - perf_start_us),
           (int) album_thumb_gen_should_cancel(my_generation));
#endif
    atomic_store(&album_thumb_gen_active, false);
    return NULL;
}

/* Called once, right after Update Music Database's own scan thread is
 * joined and the database is already fully committed (see this function's
 * own call site) -- never blocks the caller waiting for generation itself
 * to finish, only (briefly) for a STILL-RUNNING previous pass to notice
 * it's been superseded and exit, which happens within roughly one album's
 * worth of decode work given the cancellation check at the top of every
 * iteration. */
static void start_album_thumbnail_generation(void) {
    cancel_album_thumbnail_generation();
    reap_album_thumbnail_generation();

    int artist_count = 0, album_artist_count = 0, album_count = 0;
    metadata_db_get_group_counts(&artist_count, &album_artist_count, &album_count);
    if (album_count > ALBUM_THUMB_GEN_WARM_LIMIT) album_count = ALBUM_THUMB_GEN_WARM_LIMIT;
    atomic_store(&album_thumb_gen_done_count, 0);
    atomic_store(&album_thumb_gen_total_count, album_count);
    atomic_store(&album_thumb_gen_cancel, false);
    int generation = atomic_fetch_add(&album_thumb_gen_generation, 1) + 1;
    atomic_store(&album_thumb_gen_active, true);
    if (pthread_create(&album_thumb_gen_thread, NULL, album_thumb_gen_thread_func,
                        (void *) (intptr_t) generation) != 0) {
        atomic_store(&album_thumb_gen_active, false);
    } else {
        album_thumb_gen_thread_joinable = true;
    }
}

static void start_next_album_thumbnail(void) {
    if (!album_thumbnail_active_list ||
        !album_thumbnail_list_is_visible(album_thumbnail_active_list) ||
        album_thumbnail_scrolling || album_thumbnail_active || album_thumbnail_queue_count <= 0) return;
    /* Never run two full cover decoders at once on the 56 MiB target. The
     * post-load warmer can still be finishing its current album after the
     * Albums screen opens; its cancellation is cooperative. Keep this poll
     * timer alive and give the visible-row job priority as soon as that
     * worker exits instead of doubling peak JPEG/PNG memory. */
    if (atomic_load(&album_thumb_gen_active)) {
        if (album_thumbnail_poll_timer) lv_timer_resume(album_thumbnail_poll_timer);
        return;
    }
    album_thumbnail_request_t * req = malloc(sizeof(*req));
    if (!req) return;
    *req = album_thumbnail_queue[0];
    memmove(&album_thumbnail_queue[0], &album_thumbnail_queue[1],
            sizeof(album_thumbnail_queue[0]) * (size_t) (--album_thumbnail_queue_count));
    album_thumbnail_done = false;
    album_thumbnail_active = true;
    if (pthread_create(&album_thumbnail_thread, NULL, album_thumbnail_thread_func, req) != 0) {
        album_thumbnail_active = false;
        free(req);
        return;
    }
    if (album_thumbnail_poll_timer) lv_timer_resume(album_thumbnail_poll_timer);
}

static void queue_album_thumbnail(lv_obj_t * list, int logical_index, int64_t song_id) {
    if (song_id <= 0 || album_thumbnail_scrolling || list != album_thumbnail_active_list ||
        !album_thumbnail_list_is_visible(list) || album_thumbnail_cache_find(song_id)) return;
    for (int i = 0; i < album_thumbnail_queue_count; i++)
        if (album_thumbnail_queue[i].song_id == song_id) return;
    if (album_thumbnail_queue_count >= ALBUM_THUMBNAIL_QUEUE_SIZE) return;
    album_thumbnail_queue[album_thumbnail_queue_count++] = (album_thumbnail_request_t) {
        .song_id = song_id,
        .generation = album_thumbnail_generation,
        .logical_index = logical_index,
        .list = list
    };
    start_next_album_thumbnail();
}

static void album_thumbnail_scroll_cb(lv_event_t * e) {
    lv_obj_t * list = lv_event_get_target(e);
    if (list != album_thumbnail_active_list) return;
    if (lv_event_get_code(e) == LV_EVENT_SCROLL_BEGIN) {
        album_thumbnail_scrolling = true;
        album_thumbnail_queue_count = 0;
        album_thumbnail_generation++; /* discard a decode that was already in flight */
    } else if (lv_event_get_code(e) == LV_EVENT_SCROLL_END) {
        album_thumbnail_scrolling = false;
        compact_list_refresh_visible(list); /* queues the newly settled visible window */
    }
}

static void album_thumbnail_begin_screen(lv_obj_t * list) {
    /* Visible rows are latency-sensitive and the lazy path already writes
     * the identical persistent entries. Stop warming after its current
     * decode, then let start_next_album_thumbnail() service this screen. */
    cancel_album_thumbnail_generation();
    album_thumbnail_active_list = list;
    album_thumbnail_scrolling = false;
    album_thumbnail_queue_count = 0;
    if (list) compact_list_refresh_visible(list);
}

static void album_thumbnail_end_screen(lv_obj_t * list) {
    if (album_thumbnail_active_list != list) return;
    album_thumbnail_active_list = NULL;
    album_thumbnail_scrolling = false;
    album_thumbnail_queue_count = 0;
    /* Codec work cannot safely be cancelled. Invalidate and discard it
     * when it completes rather than ever repainting a hidden screen. */
    album_thumbnail_generation++;
}

static void album_thumbnail_poll_cb(lv_timer_t * timer) {
    if (!album_thumbnail_active) {
        start_next_album_thumbnail();
        if (!album_thumbnail_active && !atomic_load(&album_thumb_gen_active)) lv_timer_pause(timer);
        return;
    }
    if (!atomic_load(&album_thumbnail_done)) return;
    pthread_join(album_thumbnail_thread, NULL);
    album_thumbnail_active = false;

    bool result_applied = false;
    if (album_thumbnail_result_generation == album_thumbnail_generation &&
        album_thumbnail_active_list && album_thumbnail_list_is_visible(album_thumbnail_active_list)) {
        int victim = -1;
        uint32_t oldest = UINT32_MAX;
        for (int i = 0; i < ALBUM_THUMBNAIL_CACHE_SIZE; i++) {
            if (!album_thumbnail_cache[i].known) { victim = i; break; }
            if (album_thumbnail_cache[i].last_use < oldest) {
                oldest = album_thumbnail_cache[i].last_use;
                victim = i;
            }
        }
        album_thumbnail_cache_entry_t * e = &album_thumbnail_cache[victim];
        uint8_t * retired_pixels = e->pixels;
        if (retired_pixels) {
            /* A leading lv_image can retain &e->dsc after its row last ran
             * the decorator. Freeing pixels first made that image descriptor
             * point into released heap memory until the row happened to be
             * recycled: a redraw/scroll use-after-free on libraries larger
             * than the LRU. Mark this as a known no-art entry temporarily and
             * repaint every visible row so all references are detached before
             * releasing/reusing the slot. */
            e->pixels = NULL;
            e->dsc.data = NULL;
            if (album_thumbnail_active_list)
                compact_list_refresh_visible(album_thumbnail_active_list);
            free(retired_pixels);
        }
        memset(e, 0, sizeof(*e));
        e->song_id = album_thumbnail_result_song_id;
        e->known = true;
        e->pixels = album_thumbnail_result_pixels;
        e->last_use = ++album_thumbnail_use_counter;
        if (e->pixels) {
            e->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
            e->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
            e->dsc.header.w = ALBUM_THUMBNAIL_PX;
            e->dsc.header.h = ALBUM_THUMBNAIL_PX;
            e->dsc.header.stride = ALBUM_THUMBNAIL_PX * 2;
            e->dsc.data = e->pixels;
            e->dsc.data_size = ALBUM_THUMBNAIL_PX * ALBUM_THUMBNAIL_PX * 2;
        }
        album_thumbnail_result_pixels = NULL;
        result_applied = true;
    }
    free(album_thumbnail_result_pixels);
    album_thumbnail_result_pixels = NULL;
    if (result_applied && album_thumbnail_result_list == album_thumbnail_active_list)
        compact_list_refresh_item(album_thumbnail_result_list, album_thumbnail_result_logical_index);
    start_next_album_thumbnail();
    if (!album_thumbnail_active) lv_timer_pause(timer);
}

static void album_row_thumbnail_decorator(lv_obj_t * list, lv_obj_t * row, lv_obj_t * image,
                                           int logical_index, int pool_slot, int64_t song_id, void * ctx) {
    (void) pool_slot; (void) ctx;
    /* 14px card inset + 72px cover + 14px breathing room before text. */
    lv_obj_set_style_pad_left(row, 100, 0);
    if (song_id <= 0) {
        lv_obj_add_flag(image, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    album_thumbnail_cache_entry_t * cached = album_thumbnail_cache_find(song_id);
    if (cached && cached->pixels) {
        lv_image_set_src(image, &cached->dsc);
        lv_image_set_scale(image, LV_SCALE_NONE);
        lv_obj_remove_flag(image, LV_OBJ_FLAG_HIDDEN);
    } else {
        const void * src = asset_path("touch_list/list_default_album.png");
        lv_image_set_src(image, src);
        /* Scale from the asset's own width. A hardcoded /72 assumed the PNG
         * was already 72px; LV_IMAGE_ALIGN_STRETCH before set_src divided by
         * img->w=0 and killed the process during build_albums_screen(). */
        lv_image_header_t header;
        int32_t src_w = 72;
        if (lv_image_decoder_get_info(src, &header) == LV_RESULT_OK && header.w > 0)
            src_w = header.w;
        lv_image_set_scale(image, (ALBUM_THUMBNAIL_PX * LV_SCALE_NONE) / src_w);
        lv_obj_remove_flag(image, LV_OBJ_FLAG_HIDDEN);
        int32_t scroll_y = lv_obj_get_scroll_y(list);
        int32_t row_y = lv_obj_get_y(row);
        bool in_viewport = row_y + lv_obj_get_height(row) >= scroll_y &&
                           row_y <= scroll_y + lv_obj_get_height(list);
        if (!cached && in_viewport) queue_album_thumbnail(list, logical_index, song_id);
    }
}

/* Unfiltered (NULL filter, "every album in the library") -- uses metadata_
 * db_get_albums_page_filtered() rather than metadata_db_get_groups_page(
 * METADATA_DB_GROUP_ALBUM, ...), since the latter groups by album name
 * alone (the exact bug this session fixed) and this screen must show the
 * corrected (album, album_artist) grouping. */
static int albums_fetch_page(void * ctx, int offset, int count, compact_list_page_row_t out_rows[]) {
    (void) ctx;
    group_row_t * rows = malloc(sizeof(group_row_t) * (size_t) count);
    int n = rows ? metadata_db_get_albums_page_filtered(NULL, offset, count, rows) : 0;
    for (int i = 0; i < n; i++) {
        snprintf(out_rows[i].label, sizeof(out_rows[i].label), "%s", rows[i].name);
        out_rows[i].identity = rows[i].first_song_id;
        out_rows[i].trailing_asset[0] = '\0';
    }
    free(rows);
    return n;
}

static void artist_row_click_cb(int index) {
    index = search_remap_index(SEARCH_BINDING_ARTISTS, index);
    group_row_t group;
    if (metadata_db_get_groups_page(METADATA_DB_GROUP_ARTIST, index, 1, &group) != 1) return;
    show_artist_albums(group.name, METADATA_DB_GROUP_ARTIST);
}

static void album_row_click_cb(int index) {
    index = search_remap_index(SEARCH_BINDING_ALBUMS, index);

    /* Resolve this specific (album, album_artist) pair at its current
     * display position via a single-row offset lookup (same pattern as All
     * Songs' own row-click resolution) -- disambiguated by album_artist too,
     * so two different artists sharing an album title never collide. */
    group_row_t group;
    if (metadata_db_get_albums_page_filtered(NULL, index, 1, &group) != 1) return;
    if (group.song_count <= 0) return;

    /* metadata_db_get_album_songs() gives the real, disambiguated song list
     * directly as owned paths -- show_group_songs() takes those straight
     * through as group_song_entry_t, no whole-library array involved. */
    group_song_entry_t * entries = calloc((size_t) group.song_count, sizeof(*entries));
    int n = 0;
    song_row_t page[64];
    while (entries && n < group.song_count) {
        int want = group.song_count - n;
        if (want > 64) want = 64;
        int got = metadata_db_get_album_songs(group.name, group.album_artist, n, page, want);
        if (got <= 0) break;
        for (int i = 0; i < got; i++) {
            char title[128];
            metadata_db_song_display_title(&page[i], title, sizeof(title));
            entries[n + i].path = strdup(page[i].path);
            entries[n + i].title = strdup(title);
            if (!entries[n + i].path || !entries[n + i].title) {
                free_group_song_entries(entries, group.song_count);
                entries = NULL;
                n = 0;
                break;
            }
        }
        if (!entries) break;
        n += got;
        if (got < want) break;
    }

    show_group_songs(group.name, entries, n);
    free_group_song_entries(entries, n);
    group_songs_source_is_album = true;
}

/* Real-device fix: these three screens used to build items[] eagerly from
 * artist_groups/album_groups/album_artist_groups (all_songs_count-scale
 * arrays), which meant every tile tap cost O(library) memory and CPU --
 * confirmed at real risk of exhausting this device's 55MB RAM for a large
 * enough library, same class as All Songs' own pre-paging cost (see build_
 * all_songs_screen()'s own comment). Artists/Album Artist are paged now
 * (compact_list_set_paged_provider()); Albums additionally fixes the real
 * album-identity bug found in review (see metadata_db_get_albums_page_
 * filtered()'s own comment) as part of the same conversion, since that's
 * exactly where the bug lived. Built and immediately activated against the
 * current library -- both call sites (gui_init(), refresh_library_screens_
 * after_reload()) run this after metadata_db.c is already open, same
 * reasoning as build_all_songs_screen()'s own comment. */
static lv_obj_t * build_artists_screen(void) {
    lv_obj_t * scr = build_compact_list_screen("Artists", generic_back_cb, NULL, 0, artist_row_click_cb, NULL,
                                                &artists_list, NULL, LIST_ROW_WIDTH_WIDE, true, accent_lv_color());
    compact_list_set_row_height(artists_list, MUSIC_LIST_ROW_HEIGHT);
    int artist_count = 0, album_artist_count = 0, album_count = 0;
    metadata_db_get_group_counts(&artist_count, &album_artist_count, &album_count);
    compact_list_set_paged_provider(artists_list, artists_fetch_page, NULL, artist_count);
    finalize_screen_navigation(scr);
    return scr;
}

static lv_obj_t * build_albums_screen(void) {
    lv_obj_t * scr = build_compact_list_screen("Albums", generic_back_cb, NULL, 0, album_row_click_cb, NULL,
                                                &albums_list, NULL, LIST_ROW_WIDTH_WIDE, true, accent_lv_color());
    compact_list_set_row_height(albums_list, MUSIC_LIST_ROW_HEIGHT);
    int artist_count = 0, album_artist_count = 0, album_count = 0;
    metadata_db_get_group_counts(&artist_count, &album_artist_count, &album_count);
    album_thumbnail_generation++;
    album_thumbnail_cache_clear();
    if (!album_thumbnail_poll_timer) {
        album_thumbnail_poll_timer = lv_timer_create(album_thumbnail_poll_cb, 50, NULL);
        lv_timer_pause(album_thumbnail_poll_timer);
    }
    /* Decorator before the paged provider so the first (now synchronous)
     * window fill already has pad_left=100 and the 72px cover slot; names
     * then lay out to the right of the art instead of under it. */
    compact_list_set_row_decorator(albums_list, album_row_thumbnail_decorator, NULL);
    compact_list_set_paged_provider(albums_list, albums_fetch_page, NULL, album_count);
    lv_obj_add_event_cb(scr, album_thumbnail_screen_loaded_cb, LV_EVENT_SCREEN_LOADED, albums_list);
    lv_obj_add_event_cb(scr, album_thumbnail_screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, albums_list);
    lv_obj_add_event_cb(albums_list, album_thumbnail_scroll_cb, LV_EVENT_SCROLL_BEGIN, NULL);
    lv_obj_add_event_cb(albums_list, album_thumbnail_scroll_cb, LV_EVENT_SCROLL_END, NULL);
    finalize_screen_navigation(scr);
    return scr;
}

static void album_artist_row_click_cb(int index) {
    index = search_remap_index(SEARCH_BINDING_ALBUM_ARTIST, index);
    group_row_t group;
    if (metadata_db_get_groups_page(METADATA_DB_GROUP_ALBUM_ARTIST, index, 1, &group) != 1) return;
    show_artist_albums(group.name, METADATA_DB_GROUP_ALBUM_ARTIST);
}

static lv_obj_t * build_album_artist_screen(void) {
    lv_obj_t * scr = build_compact_list_screen("Album Artist", generic_back_cb, NULL, 0, album_artist_row_click_cb,
                                                NULL, &album_artist_list, NULL, LIST_ROW_WIDTH_WIDE, true,
                                                accent_lv_color());
    compact_list_set_row_height(album_artist_list, MUSIC_LIST_ROW_HEIGHT);
    int artist_count = 0, album_artist_count = 0, album_count = 0;
    metadata_db_get_group_counts(&artist_count, &album_artist_count, &album_count);
    compact_list_set_paged_provider(album_artist_list, album_artists_fetch_page, NULL, album_artist_count);
    finalize_screen_navigation(scr);
    return scr;
}

/* ---- A-Z browse index (Artists/Album Artist/All Songs) --
 *
 * A draggable vertical letter strip (touch_list/a_z.png, stock HiBy asset)
 * overlaid on the right edge of each of these three screens, jumping the
 * underlying build_compact_list_screen() list straight to the first
 * entry starting with the touched letter -- the standard iOS Music/
 * Contacts pattern. touch_list/a_z_result_bg.png is the big popup letter
 * bubble shown while dragging, same convention.
 *
 * Drag tracking is polled from its own dedicated timer (poll_az_index_drag,
 * registered in gui_init() next to poll_quick_drawer_drag) rather than any
 * LVGL touch event, for the exact same reason poll_quick_drawer_drag()
 * does -- see its doc comment: LV_EVENT_PRESSING never fires at all on
 * this LVGL version, only PRESSED/RELEASED/CLICKED/LONG_PRESSED do,
 * regardless of which object was hit. A separate timer rather than folding
 * this into poll_quick_drawer_drag keeps that already-large function
 * scoped to its own three existing gestures. */

typedef const char * (*az_index_name_of_t)(int display_index);

typedef struct {
    lv_obj_t * screen;
    lv_obj_t * list;
    lv_obj_t * strip;       /* the a_z.png touch strip */
    lv_obj_t * popup;       /* the a_z_result_bg.png bubble, hidden except while dragging */
    lv_obj_t * popup_label; /* big current-letter text inside popup */
    metadata_db_az_kind_t db_kind; /* DB-backed jump table for this screen -- see poll_az_index_drag() */
} az_index_binding_t;

#define AZ_INDEX_BINDING_COUNT 4
static az_index_binding_t az_index_bindings[AZ_INDEX_BINDING_COUNT];
static int az_index_registered_count = 0;


/* Called once per screen right after that screen (and its list) is built,
 * at both gui_init()'s boot-time build and the post-library-rescan rebuild
 * -- az_index_registered_count is reset to 0 before the latter, since the
 * old screen/list/strip/popup pointers this table holds become dangling
 * the moment lv_obj_delete() runs on the old screens there. */
static void register_az_index(lv_obj_t * screen, lv_obj_t * list, metadata_db_az_kind_t db_kind) {
    /* Not touch_list/a_z.png -- confirmed via pixel sampling that stock
     * asset is a fully opaque solid-black rectangle with the letters
     * painted in (alpha=255 everywhere, RGB (0,0,0) in the "empty" areas),
     * not a real transparent PNG -- invisible only against HiBy's own
     * pure-black screens, never designed to overlap anything lighter. It
     * visibly blacks out the right edge of these lists' (28,28,30) row
     * backgrounds, which a widget-level bg_opa/border style can't fix
     * since that opaque black is baked into the image's own pixels, not
     * the object's separate background layer. A plain label (genuinely
     * transparent -- only its glyph pixels paint anything) sidesteps this
     * entirely and needs no chroma-key support this LVGL build doesn't have. */
    lv_obj_t * strip = lv_label_create(screen);
    lv_label_set_text(strip, "A\nB\nC\nD\nE\nF\nG\nH\nI\nJ\nK\nL\nM\nN\nO\nP\nQ\nR\nS\nT\nU\nV\nW\nX\nY\nZ\n#");
    lv_obj_set_style_text_font(strip, &lv_font_montserrat_16, 0);
    lv_obj_add_style(strip, &style_theme_text_primary, 0);
    lv_obj_set_style_text_align(strip, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_opa(strip, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(strip, 0, 0);
    lv_obj_remove_flag(strip, LV_OBJ_FLAG_SCROLLABLE);
    /* Stretched with extra line spacing to span close to the full list
     * height (27 lines * lv_font_montserrat_16's own 22px line height is
     * only ~594px, well short of the ~688px-tall list area) rather than
     * sitting bunched up near the top. */
    lv_obj_set_style_text_line_space(strip, 3, 0);
    /* Top edge (the "A") lines up with the list's own top edge; the
     * stretched height above lands the bottom ("#") close to the screen's
     * bottom corner, matching the list's own bottom edge -- the list
     * itself starts at exactly STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT and
     * runs flush to the screen bottom (see build_compact_list_screen()). */
    lv_obj_align(strip, LV_ALIGN_TOP_RIGHT, -4, STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT);

    lv_obj_t * popup = lv_image_create(screen);
    lv_image_set_src(popup, asset_path("touch_list/a_z_result_bg.png"));
    lv_obj_center(popup);
    lv_obj_add_flag(popup, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t * popup_label = lv_label_create(popup);
    lv_obj_set_style_text_font(popup_label, &lv_font_montserrat_32, 0);
    lv_obj_add_style(popup_label, &style_theme_text_primary, 0);
    lv_obj_center(popup_label);

    az_index_bindings[az_index_registered_count++] = (az_index_binding_t){ screen, list, strip, popup, popup_label, db_kind };
}

static az_index_binding_t * find_az_binding_for_screen(lv_obj_t * screen) {
    for (int i = 0; i < az_index_registered_count; i++) {
        if (az_index_bindings[i].screen == screen) return &az_index_bindings[i];
    }
    return NULL;
}

/* Same pause/resume treatment as quick_drawer_drag_timer above, and for the
 * same reason -- see its own comment. */
lv_timer_t * az_index_drag_timer = NULL;
static bool az_index_dragging = false;
static az_index_binding_t * az_index_active_binding = NULL;
static int az_index_jump_table[27];

void poll_az_index_drag(lv_timer_t * timer) {
    lv_indev_t * indev = find_pointer_indev();
    if (!indev) return;

    bool pressed = lv_indev_get_state(indev) == LV_INDEV_STATE_PRESSED;
    lv_point_t p;
    lv_indev_get_point(indev, &p);

    if (!az_index_dragging) {
        /* Only paused here (truly idle -- resumed on the next press-down by
         * resume_fast_gesture_timers_cb(), see this timer's own handle
         * comment) and in the just-released branch below, not in either
         * return just past this one: those happen mid-press (on a screen/
         * area with no A-Z strip *yet*), and a press can still slide into
         * the strip's own bounds before it lifts -- pausing there would
         * stop catching that. */
        if (!pressed) {
            lv_timer_pause(timer);
            return;
        }
        az_index_binding_t * b = find_az_binding_for_screen(lv_screen_active());
        if (!b) return;

        lv_area_t area;
        lv_obj_get_coords(b->strip, &area);
        if (p.x < area.x1 || p.x > area.x2 || p.y < area.y1 || p.y > area.y2) return;

        az_index_dragging = true;
        az_index_active_binding = b;
        /* One entry per letter A-Z plus '#' (index 26) for anything not
         * starting with a letter -- the display offset of the first
         * matching entry, or the nearest one after it if that exact letter
         * has no entries ("jump forward to nearest match"). Computed fresh
         * on every touch-down from the already-sorted in-memory index
         * (metadata_db_get_az_table()). */
        metadata_db_get_az_table(b->db_kind, az_index_jump_table);
        lv_obj_remove_flag(b->popup, LV_OBJ_FLAG_HIDDEN);
    } else if (!pressed) {
        lv_obj_add_flag(az_index_active_binding->popup, LV_OBJ_FLAG_HIDDEN);
        az_index_dragging = false;
        az_index_active_binding = NULL;
        lv_timer_pause(timer);
        return;
    }

    az_index_binding_t * b = az_index_active_binding;
    lv_area_t area;
    lv_obj_get_coords(b->strip, &area);
    int32_t rel_y = p.y - area.y1;
    int32_t h = area.y2 - area.y1;
    if (rel_y < 0) rel_y = 0;
    if (rel_y >= h) rel_y = h - 1;
    int letter_idx = (int) ((int64_t) rel_y * 27 / (h > 0 ? h : 1));
    if (letter_idx > 26) letter_idx = 26;

    int target = az_index_jump_table[letter_idx];
    if (target >= 0) compact_list_scroll_to_index(b->list, target);

    char letter = (letter_idx < 26) ? (char) ('A' + letter_idx) : '#';
    lv_label_set_text_fmt(b->popup_label, "%c", letter);
}

/* ---- Live search (Artists/Albums/Album Artist/All Songs/Files) --
 *
 * A search icon in the title row opens an inline search bar + the
 * existing T9 keypad at the bottom of the SAME screen (reparented via
 * t9_keypad_attach(), see its own doc comment) -- unlike this app's
 * existing modal show_text_entry() flow, the list stays visible and
 * narrows live between them as each character is typed, rather than
 * being hidden behind a full-screen keyboard. The search bar (bg_search.png)
 * is drawn as a full-title-row-width container created AFTER (so on top
 * of) the screen's own back button and title -- covering both rather than
 * needing direct references to hide them, and doubling as a tap-absorbing
 * surface so the covered back button can't be hit through it. The only
 * way out of search while it's open is the close ("x") button; the
 * screen's own back button becomes reachable again once that's tapped. */

#define SEARCH_BAR_Y STATUS_BAR_CLEARANCE
#define SEARCH_BAR_HEIGHT 80

typedef struct {
    lv_obj_t * screen;
    lv_obj_t * list;
    lv_obj_t * search_btn;
    lv_obj_t * search_bar;
    az_index_name_of_t name_of; /* "label for display index i" -- in-memory fallback, only actually used when db_backed is false (Files/Subsonic) */
    const int * count_ptr;      /* likewise -- only read when db_backed is false */
    bool active;
    int * filtered_indices; /* display index -> real index; NULL when not filtering (full list shown) */
    int filtered_count;     /* only meaningful while filtered_indices != NULL */
    bool is_overlay_list;   /* true only for Files -- `list` is a search-results overlay layered on top of
                              * file_browser.c's own folder-browsing UI (opaque, its own screen-colored background),
                              * hidden except while search is active, rather than the screen's one-and-only list
                              * the other four bindings resize in place. */
    bool db_backed; /* true for Artists/Albums/Album Artist/All Songs/Files -- search_apply_filter() below queries
                      * metadata_db_search_names() directly instead of scanning name_of/count_ptr. false only for
                      * the two Subsonic bindings (their own remote-fetched arrays, not this DB). */
    metadata_db_az_kind_t db_kind;              /* only meaningful when db_backed */
    compact_list_fetch_page_cb_t restore_fetch_page; /* re-applied on search_close() to switch the list back to paged
                                                        * mode -- the exact same provider build_*_screen() set up,
                                                        * matching its own compact_list_set_paged_provider() call.
                                                        * Only meaningful when db_backed. */
    char (*filtered_labels)[128]; /* owned label storage backing filtered compact_list_item_t.label pointers, since
                                    * a DB query's result strings (unlike name_of()'s pointers into persistent
                                    * in-memory arrays) have no other long-lived home -- see compact_list_set_items()'s
                                    * own doc comment on why the strings must outlive that call. Only used when
                                    * db_backed; NULL otherwise. */
} search_binding_t;

static search_binding_t search_bindings[SEARCH_BINDING_COUNT];

static search_binding_t * find_search_binding_for_screen(lv_obj_t * screen) {
    for (int i = 0; i < SEARCH_BINDING_COUNT; i++) {
        if (search_bindings[i].screen == screen) return &search_bindings[i];
    }
    return NULL;
}

int search_remap_index(search_binding_id_t binding_id, int display_index) {
    search_binding_t * b = &search_bindings[binding_id];
    if (b->filtered_indices) return b->filtered_indices[display_index];
    return display_index;
}

/* Plain case-insensitive substring match -- not strcasestr(), which needs
 * _GNU_SOURCE defined before <string.h> that this file doesn't set (and
 * musl guards the prototype on it -- confirmed via musl's own string.h).
 * Empty needle matches everything (the "search box is empty" case). */
static bool search_matches(const char * haystack, const char * needle) {
    if (!haystack) return false;
    if (!needle || !needle[0]) return true;
    size_t needle_len = strlen(needle);
    for (const char * h = haystack; *h; h++) {
        size_t i = 0;
        while (i < needle_len && h[i] != '\0' && tolower((unsigned char) h[i]) == tolower((unsigned char) needle[i])) i++;
        if (i == needle_len) return true;
    }
    return false;
}

/* Real, live results a search box could plausibly need to show at once --
 * matches this app's own bounded-search-cache convention (metadata_db_
 * search_songs()'s own cap, remote_control.c's LIBRARY_JSON_MAX_LIMIT).
 * Without this, typing a common single letter against a 30k+-song library
 * linearly scans the whole array and mallocs/copies a match-sized buffer on
 * every keystroke. compact_list_set_items() itself stays cheap at any
 * item_count (only ~20 real row widgets ever exist regardless, unlike
 * populate_indexed_list()'s one-widget-per-row cost), so this isn't a
 * crash risk -- but scrolling through thousands of results nobody will
 * ever reach the end of is a wasted allocation and a bad live-search feel
 * on every single character typed. */
#define SEARCH_RESULTS_MAX 200

/* ---- Live search: async DB query for db_backed bindings -------------
 * Efficiency finding: metadata_db_search_names()'s query shape (a
 * ROW_NUMBER() window function wrapped in a leading-wildcard LIKE) is
 * a full tagcache scan every time this
 * runs. Used to run synchronously, directly on the UI thread, once per
 * keystroke; now debounced (search_debounce_timer below, same one-shot-
 * timer idiom as pending_progress_seek_timer above) so a burst of
 * keystrokes collapses into one query, and that one query runs on a
 * background thread -- same shape as poll_lyrics_load()/poll_cover_decode()
 * elsewhere in this file -- so even a single slow scan on a large library
 * never blocks a frame. Non-db_backed bindings (the two Subsonic ones)
 * still scan their in-memory arrays synchronously inside search_apply_
 * filter() below -- cheap and already bounded at SEARCH_RESULTS_MAX, not
 * what this finding was about. */
#define SEARCH_DEBOUNCE_MS 200

static lv_timer_t * search_debounce_timer;

typedef struct {
    metadata_db_az_kind_t db_kind;
    char query[256];
} search_job_request_t;

static pthread_t search_job_thread;
static bool search_job_active = false;
static atomic_bool search_job_done_flag = false;
static search_binding_t * search_job_for_binding;
static int search_job_result_count;
static metadata_db_search_hit_t search_job_result_hits[SEARCH_RESULTS_MAX];

static bool search_job_pending_valid = false;
static search_job_request_t search_job_pending_request;
static search_binding_t * search_job_pending_binding;

static void * search_job_thread_func(void * arg) {
    search_job_request_t * req = (search_job_request_t *) arg;
    search_job_result_count = metadata_db_search_names(req->db_kind, req->query, SEARCH_RESULTS_MAX, search_job_result_hits);
    free(req);
    atomic_store_explicit(&search_job_done_flag, true, memory_order_release); /* written last -- poll_search_job() only checks this flag */
    return NULL;
}

/* At most one job in flight -- a debounce fire arriving while the previous
 * one is still running replaces the pending request rather than queuing,
 * same "only the latest ever matters" shape as launch_cover_decode_req()'s
 * cover_decode_pending. */
static void launch_search_job(search_binding_t * b, const char * query) {
    if (search_job_active) {
        search_job_pending_binding = b;
        search_job_pending_request.db_kind = b->db_kind;
        snprintf(search_job_pending_request.query, sizeof(search_job_pending_request.query), "%s", query);
        search_job_pending_valid = true;
        return;
    }

    search_job_request_t * req = malloc(sizeof(*req));
    if (!req) return;
    req->db_kind = b->db_kind;
    snprintf(req->query, sizeof(req->query), "%s", query);

    search_job_for_binding = b;
    atomic_store_explicit(&search_job_done_flag, false, memory_order_relaxed);
    search_job_active = true;
    if (pthread_create(&search_job_thread, NULL, search_job_thread_func, req) != 0) {
        free(req);
        search_job_active = false;
    }
}

/* Builds compact_list_item_t/filtered_indices/filtered_labels from a
 * finished search job's hits and applies them to b->list -- the same tail
 * search_apply_filter()'s own db_backed branch used to do synchronously
 * right after the query, now split out so poll_search_job() below can call
 * it once the background query actually finishes. Old filtered_indices/
 * filtered_labels are freed and replaced atomically here (the list only
 * ever shows one complete result set, never a stale-then-fresh flash). */
static void search_apply_results_to_list(search_binding_t * b, const metadata_db_search_hit_t * hits, int matched) {
    compact_list_item_t * items = malloc(sizeof(compact_list_item_t) * (size_t) (matched > 0 ? matched : 1));
    int * indices = malloc(sizeof(int) * (size_t) (matched > 0 ? matched : 1));
    char(*labels)[128] = malloc(sizeof(*labels) * (size_t) (matched > 0 ? matched : 1));
    for (int i = 0; i < matched; i++) {
        snprintf(labels[i], sizeof(labels[i]), "%s", hits[i].label);
        items[i] = (compact_list_item_t){ labels[i] };
        indices[i] = hits[i].offset;
    }

    compact_list_set_items(b->list, items, matched);
    free(items);

    free(b->filtered_indices);
    free(b->filtered_labels);
    b->filtered_indices = indices;
    b->filtered_labels = labels;
    b->filtered_count = matched;
}

/* Called every tick from update_timer_cb, same as poll_cover_decode()/poll_
 * lyrics_load(). Discards the result (never applies it) if the binding it
 * was for is no longer the active search -- the user may have closed
 * search, or switched to a different search-active screen, while the query
 * was in flight. */
void poll_search_job(void) {
    if (!search_job_active || !atomic_load_explicit(&search_job_done_flag, memory_order_acquire)) return;
    search_job_active = false;
    pthread_join(search_job_thread, NULL);

    if (search_job_for_binding->active && find_search_binding_for_screen(lv_screen_active()) == search_job_for_binding) {
        search_apply_results_to_list(search_job_for_binding, search_job_result_hits, search_job_result_count);
    }

    if (search_job_pending_valid) {
        search_job_pending_valid = false;
        launch_search_job(search_job_pending_binding, search_job_pending_request.query);
    }
}

/* Rebuilds binding->list's contents to only entries matching `query`, via
 * compact_list_set_items() rather than a screen rebuild. An empty query
 * shows NOTHING (not the full library) -- real-device feedback: search
 * should be an overlay that only shows something once you've actually
 * typed something, not the whole list up front. filtered_indices maps each
 * surviving display row back to its real index (an offset into the same
 * unfiltered sequence the screen's own paged provider/click handlers use),
 * for the row-click callbacks' search_remap_index() calls. Stops once
 * SEARCH_RESULTS_MAX matches are found -- see that constant's own comment.
 *
 * db_backed bindings (Artists/Albums/Album Artist/All Songs/Files) query
 * metadata_db_search_names() directly instead of scanning name_of/count_ptr
 * -- no whole-library array dependency at all, matching the A-Z index's own
 * conversion. filtered_labels owns the label strings compact_list_item_t
 * points at in this path, since a DB query's results have no other long-
 * lived home to point into. As of the async rework above, the query itself
 * runs on a background thread (launch_search_job()) -- this function only
 * ever handles the immediate "query cleared" case for db_backed bindings
 * synchronously now. */
static void search_apply_filter(search_binding_t * b, const char * query) {
    bool have_query = query && query[0];

    if (b->db_backed) {
        if (!have_query) {
            free(b->filtered_indices);
            b->filtered_indices = NULL;
            b->filtered_count = 0;
            free(b->filtered_labels);
            b->filtered_labels = NULL;
            compact_list_set_items(b->list, NULL, 0);
            return;
        }
        /* Deliberately does NOT clear filtered_indices/filtered_labels
         * here -- the previous result set stays on screen until the new
         * one actually lands (search_apply_results_to_list(), above),
         * rather than flashing to empty for however long the background
         * query takes. */
        launch_search_job(b, query);
        return;
    }

    free(b->filtered_indices);
    b->filtered_indices = NULL;
    b->filtered_count = 0;
    free(b->filtered_labels);
    b->filtered_labels = NULL;

    int count = have_query ? *b->count_ptr : 0;
    int cap = count < SEARCH_RESULTS_MAX ? count : SEARCH_RESULTS_MAX;
    compact_list_item_t * items = malloc(sizeof(compact_list_item_t) * (size_t) (cap > 0 ? cap : 1));
    int * indices = malloc(sizeof(int) * (size_t) (cap > 0 ? cap : 1));
    int matched = 0;

    for (int i = 0; i < count && matched < cap; i++) {
        const char * name = b->name_of(i);
        if (!search_matches(name, query)) continue;
        items[matched] = (compact_list_item_t){ name };
        indices[matched] = i;
        matched++;
    }

    compact_list_set_items(b->list, items, matched);
    free(items);

    if (have_query) {
        b->filtered_indices = indices;
        b->filtered_count = matched;
    } else {
        /* Unfiltered -- display index already equals the real index, no
         * remap table needed (also covers count == 0 cleanly). */
        free(indices);
    }
}

/* Fires once SEARCH_DEBOUNCE_MS after the last keystroke -- re-checks
 * everything fresh (not captured at keystroke time) since by definition no
 * newer keystroke has arrived without cancelling and recreating this timer
 * first (see search_textarea_value_changed_cb() below). */
static void search_debounce_timer_cb(lv_timer_t * timer) {
    (void) timer;
    search_debounce_timer = NULL;
    if (!t9_keypad_is_inline_active()) return;
    search_binding_t * b = find_search_binding_for_screen(lv_screen_active());
    if (!b) return;
    search_apply_filter(b, t9_keypad_get_text());
}

void search_textarea_value_changed_cb(lv_event_t * e) {
    (void) e;
    if (!t9_keypad_is_inline_active()) return;
    if (search_debounce_timer) {
        lv_timer_delete(search_debounce_timer);
        search_debounce_timer = NULL;
    }
    search_debounce_timer = lv_timer_create(search_debounce_timer_cb, SEARCH_DEBOUNCE_MS, NULL);
    lv_timer_set_repeat_count(search_debounce_timer, 1);
}

/* Restores a binding's list to its normal full-height, bottom-anchored
 * layout -- the exact geometry build_compact_list_screen() itself uses. */
static void search_restore_list_geometry(lv_obj_t * list) {
    lv_obj_set_size(list, lv_pct(100),
                     lv_display_get_vertical_resolution(lv_display_get_default()) - STATUS_BAR_CLEARANCE - TITLE_ROW_HEIGHT);
    lv_obj_align(list, LV_ALIGN_BOTTOM_MID, 0, 0);
}

/* Enter's meaning while a search binding owns the keypad: hide just the
 * keypad (search bar, typed query, and the current filter all stay) and
 * give the list back the vertical space the keypad occupied -- matches a
 * normal search box where dismissing the on-screen keyboard doesn't clear
 * what you searched for. text_entry_textarea is deliberately NOT
 * reparented back here (unlike the full t9_keypad_release()) so the typed
 * query stays visible in the search bar. */


static void search_open(search_binding_t * b) {
    /* db_backed bindings (Artists/Albums/Album Artist/All Songs) query
     * metadata_db_search_names() directly -- see search_apply_filter()'s own
     * comment -- so opening search on any of them never needs this. Only
     * Files/Subsonic still scan the in-memory name_of/count_ptr arrays. */
    lv_obj_add_flag(b->search_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(b->search_bar, LV_OBJ_FLAG_HIDDEN);

    if (b->is_overlay_list) lv_obj_remove_flag(b->list, LV_OBJ_FLAG_HIDDEN);

    az_index_binding_t * az = find_az_binding_for_screen(b->screen);
    if (az) lv_obj_add_flag(az->strip, LV_OBJ_FLAG_HIDDEN);

    /* Room for the search bar above AND the keypad below -- the keypad
     * attaches at its own fixed TEXT_ENTRY_GRID_X/Y position regardless of
     * which screen it's on, so that's the exact number to stop above. */
    lv_obj_set_size(b->list, lv_pct(100), t9_keypad_get_grid_y() - (SEARCH_BAR_Y + SEARCH_BAR_HEIGHT));
    lv_obj_align(b->list, LV_ALIGN_TOP_MID, 0, SEARCH_BAR_Y + SEARCH_BAR_HEIGHT);

    /* Spans from near the bar's left edge to just before close_btn (at
     * 440-51-8=381). */
    t9_keypad_attach(b->screen, b->search_bar, 8, 14, 365);

    b->active = true;
    search_apply_filter(b, ""); /* start unfiltered -- also (re)establishes filtered_indices == NULL */
}

static void search_close(search_binding_t * b) {
    t9_keypad_release();

    /* Not strictly required for correctness -- poll_search_job()/search_
     * debounce_timer_cb() both already re-check b->active/find_search_
     * binding_for_screen() at fire time and no-op harmlessly if this
     * binding is no longer the active search -- but cancelling outright
     * avoids a pointless wait for a query result nobody will ever see. */
    if (search_debounce_timer) {
        lv_timer_delete(search_debounce_timer);
        search_debounce_timer = NULL;
    }

    lv_obj_add_flag(b->search_bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(b->search_btn, LV_OBJ_FLAG_HIDDEN);

    az_index_binding_t * az = find_az_binding_for_screen(b->screen);
    if (az) lv_obj_remove_flag(az->strip, LV_OBJ_FLAG_HIDDEN);

    if (b->is_overlay_list) {
        lv_obj_add_flag(b->list, LV_OBJ_FLAG_HIDDEN); /* reveals file_browser.c's own folder-browsing UI underneath again */
    } else {
        search_restore_list_geometry(b->list);
    }

    free(b->filtered_indices);
    b->filtered_indices = NULL;
    free(b->filtered_labels);
    b->filtered_labels = NULL;

    if (b->db_backed) {
        /* Switches the list back to paged mode instead of rebuilding a
         * static items[] array -- the exact same provider/total_count
         * build_*_screen() set up originally (see its own compact_list_set_
         * paged_provider() call), recomputed fresh here in case a rescan
         * changed the library while search was open. */
        int artist_count = 0, album_artist_count = 0, album_count = 0;
        metadata_db_get_group_counts(&artist_count, &album_artist_count, &album_count);
        int total = 0;
        switch (b->db_kind) {
            case METADATA_DB_AZ_ARTIST: total = artist_count; break;
            case METADATA_DB_AZ_ALBUM_ARTIST: total = album_artist_count; break;
            case METADATA_DB_AZ_ALBUM: total = album_count; break;
            case METADATA_DB_AZ_ALL_SONGS: total = (int) metadata_db_get_song_count(); break;
        }
        compact_list_set_paged_provider(b->list, b->restore_fetch_page, NULL, total);
    } else {
        compact_list_item_t * items = NULL;
        int count = *b->count_ptr;
        if (count > 0) {
            items = malloc(sizeof(compact_list_item_t) * (size_t) count);
            for (int i = 0; i < count; i++) items[i] = (compact_list_item_t){ b->name_of(i) };
        }
        compact_list_set_items(b->list, items, count);
        free(items);
    }

    b->active = false;
}

/* screen_gesture_event_cb()'s back-swipe hook -- see its own forward
 * declaration for why. */
bool search_close_if_active_for_screen(lv_obj_t * screen) {
    search_binding_t * b = find_search_binding_for_screen(screen);
    if (!b || !b->active) return false;
    search_close(b);
    return true;
}

static void search_btn_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    search_binding_id_t id = (search_binding_id_t) (intptr_t) lv_event_get_user_data(e);
    search_open(&search_bindings[id]);
}

static void search_close_btn_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    search_binding_id_t id = (search_binding_id_t) (intptr_t) lv_event_get_user_data(e);
    search_close(&search_bindings[id]);
}

/* Called once per screen right after that screen (and its list) is built,
 * same two call sites (boot-time gui_init() + post-rescan rebuild) the
 * A-Z index already registers at. Builds the initial search icon (top-
 * right of the title row, same position/pattern playlists_edit_btn
 * already uses) and the search bar (hidden until search_btn is tapped). */
void register_search(search_binding_id_t id, lv_obj_t * screen, lv_obj_t * list, az_index_name_of_t name_of,
                             const int * count_ptr, bool is_overlay_list, bool db_backed, metadata_db_az_kind_t db_kind,
                             compact_list_fetch_page_cb_t restore_fetch_page) {
    search_binding_t * b = &search_bindings[id];
    free(b->filtered_indices); /* re-registering (post-rescan rebuild) over a binding left mid-filter would otherwise leak this */
    free(b->filtered_labels);

    lv_obj_t * search_btn = lv_image_create(screen);
    lv_image_set_src(search_btn, asset_path("sub_back/btn_search.png"));
    /* Vertically centered within the same STATUS_BAR_CLEARANCE..+TITLE_ROW_HEIGHT
     * band the back button's own 64x64 box occupies (build_back_button()),
     * not the shorter title-label-specific centering formula this used to
     * borrow -- real-device feedback: the two didn't line up. */
    lv_obj_align(search_btn, LV_ALIGN_TOP_RIGHT, -20, STATUS_BAR_CLEARANCE + (TITLE_ROW_HEIGHT - 51) / 2);
    lv_obj_add_flag(search_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(search_btn, search_btn_click_cb, LV_EVENT_CLICKED, (void *) (intptr_t) id);
    lv_obj_add_flag(search_btn, LV_OBJ_FLAG_GESTURE_BUBBLE); /* added after finalize_screen_navigation()'s one-time pass, needs this set explicitly -- see screen_gesture_event_cb()'s own comment */

    /* Sized to bg_search.png's own native 440x80 (not lv_pct(100)) and
     * left-aligned at x=0 -- LVGL's bg_image draws at native size CENTERED
     * within the object's own box (see LIST_ROW_WIDTH's own comment in
     * screen_builders.h), so a same-size box is what makes the image
     * render flush against bar's left edge instead of with margins on
     * both sides. Still fully covers the 64px-wide back button underneath
     * for tap-absorption purposes (440 >> 64). No mag_icon/clear_btn --
     * real-device feedback: search mode should show just the bar and the
     * close ("x") button, nothing else. */
    lv_obj_t * bar = lv_obj_create(screen);
    lv_obj_set_size(bar, 440, SEARCH_BAR_HEIGHT);
    lv_obj_set_pos(bar, 0, SEARCH_BAR_Y);
    lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_image_src(bar, asset_path("sub_back/bg_search.png"), 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    /* lv_obj_create() pulls in the default theme's "card" style, which sets
     * pad_all to a nonzero default -- every lv_obj_set_pos() below for this
     * bar's children is relative to its CONTENT area (inside that padding),
     * so left uncleared, close_btn (and the reparented textarea) land
     * further right/down than intended. Same root cause already documented
     * on volume_popup's own construction. */
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(bar, LV_OBJ_FLAG_CLICKABLE); /* absorbs taps so the covered back button/title beneath can't be hit through it */
    lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t * close_btn = lv_image_create(bar);
    lv_image_set_src(close_btn, asset_path("sub_back/close.png"));
    lv_obj_set_pos(close_btn, 440 - 51 - 8, 14);
    lv_obj_add_flag(close_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(close_btn, search_close_btn_click_cb, LV_EVENT_CLICKED, (void *) (intptr_t) id);

    /* Same reasoning as search_btn above -- bar and its children were all
     * added after finalize_screen_navigation()'s one-time recursive pass,
     * so a swipe starting on any of them (the bar covers the whole title
     * row) needs this set explicitly to bubble up to
     * screen_gesture_event_cb() at all. */
    lv_obj_add_flag(bar, LV_OBJ_FLAG_GESTURE_BUBBLE);
    enable_gesture_bubble_recursive(bar);

    *b = (search_binding_t){ screen,     list,    search_btn,        bar,      name_of, count_ptr,
                              false,      NULL,    0,                 is_overlay_list, db_backed, db_kind,
                              restore_fetch_page, NULL };
}

/* Files' search (SEARCH_BINDING_FILES) is the one binding with no per-
 * screen row-click callback of its own to add a search_remap_index() line
 * to (see the other four's own callbacks) -- it plays the tapped result
 * directly, building its own playlist fresh in exactly the order currently
 * displayed (the filtered subset if one's active, matching
 * all_songs_row_click_cb's own "build in display order, pass display_index
 * straight through" shape), so display_index never needs remapping here. */
static void files_search_row_click_cb(int display_index) {
    search_binding_t * b = &search_bindings[SEARCH_BINDING_FILES];
    int all_songs_display_index = b->filtered_indices ? b->filtered_indices[display_index] : display_index;
    set_player_source_all_songs(all_songs_display_index);
    on_file_selected_lazy_all_songs(all_songs_display_index);
}

/* ---- Playlists (Music submenu, replacing the old Genres tile -- real-
 * device feedback: genre tags are inconsistently populated across
 * libraries, unlike play history/manual curation) --
 *
 * Favorites and Most Played (top 20 by play count, see metadata_db's
 * song_play_count table) are resolved fresh from the DB on every tap and
 * shown through show_group_songs() -- the exact same drill-down screen
 * Artists/Albums/Album Artist already use -- rather than a screen of their
 * own. Membership can change between visits (unlike Artists/Albums/Album
 * Artist, fixed at scan time), so these two are recomputed fresh every
 * time their row is tapped rather than cached. show_group_songs() takes an
 * owned copy of the small result set, so the DB-returned path array can be
 * released immediately.
 *
 * User-created .m3u playlists (Player screen's "Add to Playlist", or
 * dropped into MUSIC_ROOT_DIR/Playlists by hand) are listed below those
 * from that folder only. ---- */

#define MOST_PLAYED_LIMIT 20
#define RECENTLY_ADDED_LIMIT 50

/* Resolves each of paths[0..count) to a display title via a single-row DB
 * lookup (metadata_db_get_song_by_path()) -- falls back to the raw
 * basename if a path somehow isn't in the library anymore (a stale
 * favorite/playlist entry that outlived a rescan). Caller owns the
 * returned array (free() it); paths[] itself is untouched. Shared by
 * show_favorites()/show_most_played()/show_m3u_playlist() below -- these
 * lists are small (a hand-curated favorites list, MOST_PLAYED_LIMIT, one
 * playlist), so a DB lookup per song costs nothing next to loading the
 * whole library just to resolve a title. */
static group_song_entry_t * build_group_song_entries_from_paths(char ** paths, int count) {
    group_song_entry_t * entries = calloc((size_t) (count > 0 ? count : 1), sizeof(*entries));
    if (!entries) return NULL;
    for (int i = 0; i < count; i++) {
        entries[i].path = strdup(paths[i]);
        song_row_t row;
        char title[128];
        if (metadata_db_get_song_by_path(paths[i], &row)) {
            metadata_db_song_display_title(&row, title, sizeof(title));
        } else {
            snprintf(title, sizeof(title), "%s", basename_of(paths[i]));
        }
        entries[i].title = strdup(title);
        if (!entries[i].path || !entries[i].title) {
            free_group_song_entries(entries, count);
            return NULL;
        }
    }
    return entries;
}

/* Real-device bug report: playing a song from the remote-control web UI
 * always queued the entire library (alphabetical order, whatever play_mode
 * happened to be) instead of just the Album/Playlist the song was actually
 * tapped from -- see remote_control.c's own request_play_playlist_name
 * comment for the full story. playlist_name/artist_filter/album_artist_
 * filter/album_filter are remote_control_consume_play_index()'s own
 * context, empty string meaning "not provided". song_path is the caller's
 * own DB resolution of remote_control.c's song id (metadata_db_get_song_
 * by_id()). Falls back to the existing whole-library behavior whenever no
 * usable scope resolves -- both the plain "no filters at all" case (someone
 * playing from All Songs/search) and a stale reference (e.g. a playlist
 * renamed/deleted since the phone last loaded its song list). */
void play_remote_control_song(const char * song_path, const char * playlist_name, const char * artist_filter,
                                      const char * album_artist_filter, const char * album_filter) {
    if (!song_path || !song_path[0]) return;
    group_song_entry_t * scoped_entries = NULL;
    int scoped_count = 0;
    char scoped_title[128] = "";

    if (playlist_name[0] != '\0') {
        char ** paths = NULL;
        int count = 0;
        bool loaded = false;
        if (strcmp(playlist_name, "@favorites") == 0 || strcmp(playlist_name, "Favorites") == 0) {
            metadata_db_load_favorite_songs(&paths, &count);
            loaded = true;
            snprintf(scoped_title, sizeof(scoped_title), "Favorites");
        } else if (strcmp(playlist_name, "@most_played") == 0 || strcmp(playlist_name, "Most Played") == 0) {
            metadata_db_load_top_played_songs(MOST_PLAYED_LIMIT, &paths, &count);
            loaded = true;
            snprintf(scoped_title, sizeof(scoped_title), "Most Played");
        } else if (strcmp(playlist_name, "@recently_added") == 0 || strcmp(playlist_name, "Recently Added") == 0) {
            metadata_db_load_recently_added_songs(RECENTLY_ADDED_LIMIT, &paths, &count);
            loaded = true;
            snprintf(scoped_title, sizeof(scoped_title), "Recently Added");
        } else {
            char m3u_path[512];
            snprintf(m3u_path, sizeof(m3u_path), "%s/%s.m3u", PLAYLISTS_DIR, playlist_name);
            loaded = file_browser_build_playlist_from_m3u(m3u_path, &paths, &count);
        }
        if (loaded && count > 0) {
            scoped_entries = build_group_song_entries_from_paths(paths, count);
            scoped_count = scoped_entries ? count : 0;
            for (int i = 0; i < count; i++) free(paths[i]);
            free(paths);
            if (scoped_title[0] == '\0') snprintf(scoped_title, sizeof(scoped_title), "%s", playlist_name);
        }
    } else if (album_filter[0] != '\0' && (artist_filter[0] != '\0' || album_artist_filter[0] != '\0')) {
        int64_t count64 = metadata_db_count_songs_filtered(NULL, artist_filter, album_artist_filter, album_filter);
        if (count64 > 0 && count64 <= INT_MAX) {
            scoped_entries = calloc((size_t) count64, sizeof(*scoped_entries));
            song_row_t rows[64];
            while (scoped_entries && scoped_count < count64) {
                int want = (int) (count64 - scoped_count);
                if (want > 64) want = 64;
                int got = metadata_db_get_songs_filtered_page(NULL, artist_filter, album_artist_filter, album_filter,
                                                               scoped_count, want, rows);
                if (got <= 0) break;
                for (int i = 0; i < got; i++) {
                    char title[128];
                    metadata_db_song_display_title(&rows[i], title, sizeof(title));
                    scoped_entries[scoped_count + i].path = strdup(rows[i].path);
                    scoped_entries[scoped_count + i].title = strdup(title);
                    if (!scoped_entries[scoped_count + i].path || !scoped_entries[scoped_count + i].title) {
                        free_group_song_entries(scoped_entries, (int) count64);
                        scoped_entries = NULL;
                        scoped_count = 0;
                        break;
                    }
                }
                if (!scoped_entries) break;
                scoped_count += got;
                if (got < want) break;
            }
            snprintf(scoped_title, sizeof(scoped_title), "%s", album_filter);
        }
    }

    if (scoped_entries && scoped_count > 0) {
        int pos = -1;
        for (int i = 0; i < scoped_count; i++) {
            if (strcmp(scoped_entries[i].path, song_path) == 0) {
                pos = i;
                break;
            }
        }
        if (pos >= 0) {
            char ** playlist_copy = malloc(sizeof(char *) * (size_t) scoped_count);
            for (int i = 0; i < scoped_count; i++) playlist_copy[i] = strdup(scoped_entries[i].path);
            set_player_source_group_songs_direct(scoped_entries, scoped_count, scoped_title, pos);
            on_file_selected(playlist_copy, scoped_count, pos);
            free_group_song_entries(scoped_entries, scoped_count);
            return;
        }
    }
    free_group_song_entries(scoped_entries, scoped_count);

    int64_t offset = metadata_db_get_song_title_offset(song_path);
    if (offset >= 0 && offset <= INT_MAX) {
        set_player_source_all_songs((int) offset);
        on_file_selected_lazy_all_songs((int) offset);
    }
}

/* Called from apply_track_metadata_to_ui() right after now_playing_path is
 * updated -- the single dispatch point that pushes it out to every now-
 * playing-aware list. Resolves the playing path's own tags via a single DB
 * lookup (metadata_db_get_song_by_path()), then each of Artists/Albums/
 * Album Artist/All Songs gets its own display offset via metadata_db_get_
 * group_offset()/metadata_db_get_song_title_offset() -- Artists/Albums/
 * Album Artist match by name (a whole group's songs share one row, so the
 * indicator lights up whichever artist/album/album-artist the CURRENT
 * TRACK belongs to, not just an exact-song match); All Songs matches by
 * exact song identity. Skips (rather than asserts on) any list that's
 * NULL -- Artists/Albums/Album Artist/All Songs are all built once at
 * startup so in practice this only ever matters before gui_init() finishes
 * building them, but there's no reason to depend on call-order here when a
 * simple guard covers it. */
void refresh_now_playing_indicators(void) {
    int artist_row = -1, album_row = -1, album_artist_row = -1, all_songs_row = -1, recently_added_row = -1;

    song_row_t row;
    if (now_playing_path[0] && metadata_db_get_song_by_path(now_playing_path, &row)) {
        int64_t v = metadata_db_get_group_offset(METADATA_DB_GROUP_ARTIST, row.tags.artist, NULL);
        if (v >= 0 && v <= INT_MAX) artist_row = (int) v;
        v = metadata_db_get_group_offset(METADATA_DB_GROUP_ALBUM, row.tags.album, row.tags.album_artist);
        if (v >= 0 && v <= INT_MAX) album_row = (int) v;
        v = metadata_db_get_group_offset(METADATA_DB_GROUP_ALBUM_ARTIST, row.tags.album_artist, NULL);
        if (v >= 0 && v <= INT_MAX) album_artist_row = (int) v;
        v = metadata_db_get_song_title_offset(now_playing_path);
        if (v >= 0 && v <= INT_MAX) all_songs_row = (int) v;
        v = metadata_db_get_song_recency_offset(now_playing_path);
        if (v >= 0 && v <= INT_MAX) recently_added_row = (int) v;
    }

    if (artists_list) compact_list_set_now_playing(artists_list, artist_row);
    if (albums_list) compact_list_set_now_playing(albums_list, album_row);
    if (album_artist_list) compact_list_set_now_playing(album_artist_list, album_artist_row);
    if (all_songs_list) compact_list_set_now_playing(all_songs_list, all_songs_row);
    if (recently_added_list) compact_list_set_now_playing(recently_added_list, recently_added_row);

    refresh_group_songs_now_playing_indicator(); /* group_songs isn't compact_list-based -- see its own comment */
    refresh_artist_albums_now_playing_indicator(); /* Artists/Album Artist's shared compact album drill-down */
}

static void show_favorites(void) {
    char ** paths;
    int count;
    metadata_db_load_favorite_songs(&paths, &count);

    group_song_entry_t * entries = build_group_song_entries_from_paths(paths, count);
    if (entries) show_group_songs("Favorites", entries, count);
    free_group_song_entries(entries, count);
    for (int i = 0; i < count; i++) free(paths[i]);
    free(paths);
}

static void show_most_played(void) {
    char ** paths;
    int count;
    metadata_db_load_top_played_songs(MOST_PLAYED_LIMIT, &paths, &count);

    group_song_entry_t * entries = build_group_song_entries_from_paths(paths, count);
    if (entries) show_group_songs("Most Played", entries, count);
    free_group_song_entries(entries, count);
    for (int i = 0; i < count; i++) free(paths[i]);
    free(paths);
}

/* Unlike Favorites/Most Played above, Recently Added is not built through
 * show_group_songs() -- it's the whole library just reordered, potentially
 * far larger than either of those two's own small deliberately-capped
 * result sets, so it needs recently_added_screen's own paged compact_list
 * (build_recently_added_screen()) instead. Same "prebuilt screen, just
 * nav_push it" shape as open_queue_screen(). */
static void open_recently_added_screen(void) {
    nav_push(recently_added_screen);
}

static char ** playlists_m3u_paths = NULL;
static int playlists_m3u_count = 0;

static char playlist_m3u_name[128];

/* The currently open M3U's title survives an in-place row removal. Song
 * paths and titles themselves live in group_songs_entries, independent of
 * the full-library arrays. */
/* Shows an .m3u's contents the same way Favorites/Most Played do -- a
 * tappable song list (show_group_songs_editable()), not straight into
 * playback -- rather than the old behavior of jumping directly to track 0
 * the moment the playlist row itself was tapped, matching every other entry
 * point into a group of songs elsewhere in this screen (Artists/Albums/
 * Album Artist all list first too). m3u_path is passed through as the
 * editable-playlist marker so the Edit/remove UI (group_songs_edit_m3u_path,
 * see its own comment) only ever shows up here, not for Favorites/Most
 * Played/Artists/Albums. */
static void show_m3u_playlist(const char * name, const char * m3u_path, char ** paths, int count) {
    group_song_entry_t * entries = build_group_song_entries_from_paths(paths, count);
    if (!entries) return;
    snprintf(playlist_m3u_name, sizeof(playlist_m3u_name), "%s", name);
    show_group_songs_editable(playlist_m3u_name, entries, count, m3u_path);
    free_group_song_entries(entries, count);
}

/* Remove-icon handler for an editable playlist's rows (see
 * populate_group_songs_rows() -- forward-declared near
 * group_songs_edit_m3u_path). Rewrites the underlying .m3u file, then
 * re-reads it back and redraws in place (no nav_push -- this stays on the
 * same group_songs_screen instance, just with the file's new contents)
 * rather than trusting the in-memory indices array to still match the file
 * after the rewrite. */
static void group_song_remove_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int pos = (int) (intptr_t) lv_event_get_user_data(e);
    if (!group_songs_edit_m3u_path || pos < 0 || pos >= group_songs_count) return;

    const char * m3u_path = group_songs_edit_m3u_path;
    playlist_files_remove(m3u_path, group_songs_entries[pos].path);

    char ** songs = NULL;
    int count = 0;
    file_browser_build_playlist_from_m3u(m3u_path, &songs, &count); /* false (empty playlist) leaves songs/count at 0, nothing allocated -- handled below */

    if (count == 0) {
        /* Empty playlists are auto-deleted rather than left around as a
         * stuck row nothing can open again -- this is the normal, in-app
         * way a playlist becomes empty (removing its last song). m3u_path
         * (== group_songs_edit_m3u_path) is a pointer into
         * playlists_m3u_paths, so delete before populate_playlists_screen()
         * below frees/rebuilds that array out from under it. */
        playlist_files_delete(m3u_path);
        metadata_db_playlist_delete_one(m3u_path);
        populate_playlists_screen(); /* not auto-refreshed by nav_pop() -- see build_playlists_screen()'s own comment */
        nav_pop();
        show_error_toast("Playlist deleted (last song removed)");
        return;
    }

    group_song_entry_t * entries = build_group_song_entries_from_paths(songs, count);
    for (int i = 0; i < count; i++) free(songs[i]);
    free(songs);
    if (!entries) return;
    set_group_songs_entries(entries, count);
    free_group_song_entries(entries, count);
    populate_group_songs_rows();
    show_error_toast("Removed from playlist");
}

static void playlist_row_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);

    if (index == 0) {
        show_favorites();
        return;
    }
    if (index == 1) {
        show_most_played();
        return;
    }
    if (index == 2) {
        open_queue_screen();
        return;
    }
    if (index == 3) {
        open_recently_added_screen();
        return;
    }

    int m3u_index = index - 4;
    if (m3u_index < 0 || m3u_index >= playlists_m3u_count) return;

    char ** songs;
    int count;
    if (!file_browser_build_playlist_from_m3u(playlists_m3u_paths[m3u_index], &songs, &count)) {
        /* False here means either "genuinely can't open" (leave it alone --
         * could be a transient SD-card issue, not the playlist's fault) or
         * "opened fine, zero playable entries" (a stale empty playlist,
         * e.g. one that predates the auto-delete-on-last-removal fix in
         * group_song_remove_row_cb() -- safe to clean up now that it's been
         * tapped). fopen() success alone distinguishes the two without
         * risking deleting a file we merely failed to read. */
        FILE * probe = fopen(playlists_m3u_paths[m3u_index], "r");
        if (probe) {
            fclose(probe);
            playlist_files_delete(playlists_m3u_paths[m3u_index]);
            metadata_db_playlist_delete_one(playlists_m3u_paths[m3u_index]);
            populate_playlists_screen();
            show_error_toast("Empty playlist deleted");
        } else {
            show_error_toast("Playlist is empty or unreadable");
        }
        return;
    }
    show_m3u_playlist(basename_of(playlists_m3u_paths[m3u_index]), playlists_m3u_paths[m3u_index], songs, count);
    for (int i = 0; i < count; i++) free(songs[i]);
    free(songs);
}

/* Playlists' own row shape -- plain rounded rect (LIST_ROW_* look, same as
 * Artists/Albums/All Songs/Files) at LIST_ROW_WIDTH_WIDE rather than
 * add_pill_row_base()'s shared touch_list/item_bg.png pill: that PNG draws
 * at its native size regardless of the row's own width (see LIST_ROW_WIDTH's
 * own comment), so widening a pill row would've just left dead space around
 * an unchanged-size graphic instead of an actually-bigger tap target.
 * add_pill_row_base() itself stays untouched -- it's shared with a dozen
 * unrelated settings screens (Bluetooth DAC, Codec, Font Size, USB Mode,
 * ...) not part of this request. */
static lv_obj_t * add_playlist_row_base(lv_obj_t * parent, const char * label_text) {
    lv_obj_t * row = lv_obj_create(parent);
    lv_obj_set_size(row, LIST_ROW_WIDTH_WIDE, MUSIC_LIST_ROW_HEIGHT);
    lv_obj_set_style_radius(row, LIST_ROW_RADIUS, 0);
    lv_obj_set_style_bg_color(row, LIST_ROW_BG_COLOR, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * label = lv_label_create(row);
    lv_label_set_text(label, label_text);
    lv_obj_add_style(label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(label, &LIST_ROW_FONT, 0);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, LIST_ROW_LABEL_INSET, 0);
    return row;
}

/* Edit-mode delete icon for a user-created .m3u row -- never wired onto the
 * Favorites/Most Played rows (see populate_playlists_screen() below), since
 * neither is backed by a real file playlist_files_delete() could remove. */
static void playlist_delete_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int m3u_index = (int) (intptr_t) lv_event_get_user_data(e);
    if (m3u_index < 0 || m3u_index >= playlists_m3u_count) return;

    playlist_files_delete(playlists_m3u_paths[m3u_index]);
    metadata_db_playlist_delete_one(playlists_m3u_paths[m3u_index]);
    populate_playlists_screen();
    show_error_toast("Playlist deleted");
}

static void playlists_edit_btn_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    playlists_edit_mode = !playlists_edit_mode;
    populate_playlists_screen();
}

static void populate_playlists_screen(void) {
    lv_obj_clean(playlists_list);

    for (int i = 0; i < playlists_m3u_count; i++) free(playlists_m3u_paths[i]);
    free(playlists_m3u_paths);
    playlists_m3u_paths = NULL;
    playlists_m3u_count = 0;
    /* Persistent cache of PLAYLISTS_DIR only -- see rescan_playlists(). */
    metadata_db_load_all_playlists(&playlists_m3u_paths, &playlists_m3u_count);

    lv_label_set_text(playlists_edit_btn, playlists_edit_mode ? "Done" : "Edit");

    /* Favorites/Most Played/Queue/Recently Added are never deletable (none
     * is a real .m3u file -- see playlist_row_click_cb()'s index==0..3
     * special cases), so edit mode just makes them inert instead of showing
     * a delete icon that would have nothing to act on. Same "row does
     * nothing, only the per-row action works" convention as
     * populate_group_songs_rows()'s own edit mode. */
    lv_obj_t * favorites_row = add_playlist_row_base(playlists_list, "Favorites");
    if (!playlists_edit_mode) {
        lv_obj_add_flag(favorites_row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(favorites_row, playlist_row_click_cb, LV_EVENT_CLICKED, (void *) (intptr_t) 0);
    }

    lv_obj_t * most_played_row = add_playlist_row_base(playlists_list, "Most Played");
    if (!playlists_edit_mode) {
        lv_obj_add_flag(most_played_row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(most_played_row, playlist_row_click_cb, LV_EVENT_CLICKED, (void *) (intptr_t) 1);
    }

    /* Never deletable either -- same treatment as Favorites/Most Played
     * above, see playlist_row_click_cb()'s index==2 case. */
    lv_obj_t * queue_row = add_playlist_row_base(playlists_list, "Queue");
    if (!playlists_edit_mode) {
        lv_obj_add_flag(queue_row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(queue_row, playlist_row_click_cb, LV_EVENT_CLICKED, (void *) (intptr_t) 2);
    }

    /* Never deletable either -- same treatment as Favorites/Most Played/Queue
     * above, see playlist_row_click_cb()'s index==3 case. */
    lv_obj_t * recently_added_row = add_playlist_row_base(playlists_list, "Recently Added");
    if (!playlists_edit_mode) {
        lv_obj_add_flag(recently_added_row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(recently_added_row, playlist_row_click_cb, LV_EVENT_CLICKED, (void *) (intptr_t) 3);
    }

    for (int i = 0; i < playlists_m3u_count; i++) {
        lv_obj_t * row = add_playlist_row_base(playlists_list, basename_of(playlists_m3u_paths[i]));
        if (playlists_edit_mode) {
            lv_obj_t * delete_icon = lv_image_create(row);
            lv_image_set_src(delete_icon, asset_path("touch_list/del.png"));
            lv_obj_align(delete_icon, LV_ALIGN_RIGHT_MID, -20, 0);
            lv_obj_add_flag(delete_icon, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(delete_icon, playlist_delete_row_cb, LV_EVENT_CLICKED, (void *) (intptr_t) i);
        } else {
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(row, playlist_row_click_cb, LV_EVENT_CLICKED, (void *) (intptr_t) (4 + i));
        }
    }
}

static lv_obj_t * build_playlists_screen(void) {
    lv_obj_t * title_label;
    lv_obj_t * scr = build_subsonic_list_screen("Playlists", &title_label, &playlists_list);

    /* Rows here are LIST_ROW_WIDTH_WIDE (see add_playlist_row_base()), wider
     * than build_subsonic_list_screen()'s own default 448px pill rows --
     * explicit cross-axis centering scoped to just this screen instance so
     * the wider rows are guaranteed centered rather than relying on
     * whatever the shared builder's own (untouched, ~20-screens-shared)
     * default flex alignment happens to be. */
    lv_obj_set_flex_align(playlists_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);

    /* Attached directly onto this specific screen instance rather than
     * threaded through build_subsonic_list_screen()'s own parameters --
     * that builder is shared by ~20 unrelated screens, same established
     * pattern as build_wifi_screen()'s Rescan button and the Subsonic
     * Download buttons. */
    playlists_edit_btn = lv_label_create(scr);
    lv_label_set_text(playlists_edit_btn, "Edit");
    lv_obj_set_style_text_color(playlists_edit_btn, accent_lv_color(), 0);
    lv_obj_set_style_text_font(playlists_edit_btn, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_obj_align(playlists_edit_btn, LV_ALIGN_TOP_RIGHT, -20, STATUS_BAR_CLEARANCE + (TITLE_ROW_HEIGHT - 28) / 2);
    lv_obj_add_flag(playlists_edit_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(playlists_edit_btn, playlists_edit_btn_cb, LV_EVENT_CLICKED, NULL);

    return scr;
}

/* ---- CUE sheet track list (File Browser -> tap a .cue) -- MVP scope, real
 * bug report/ISSUES.md to-do item: browse and jump straight to any track
 * within a single large lossless rip (album.flac + album.cue), rather than
 * only being able to play/scrub the one giant file with no idea where each
 * song actually starts. Tapping a track seeks the shared physical file to
 * that track's own INDEX 01 offset (on_file_selected_at() -- see its own
 * comment) and plays from there.
 *
 * Deliberately NOT wired into gapless next/prev-across-track-boundaries in
 * this pass: playlist[] here holds the SAME file path once per CUE track
 * (so the row tapped and the "Track N of M" label at least make sense), but
 * skip-next/skip-prev between two of them just restarts the same physical
 * file at 0:00 rather than seeking to the next track's own start -- doing
 * that correctly needs a per-playlist-slot start-offset the core playback
 * model doesn't carry today (every other source in this app is genuinely
 * one file per slot). Browsing back to this screen and tapping a different
 * track directly still works correctly either way. */
static cue_sheet_t current_cue_sheet;
static bool current_cue_sheet_valid = false;
static char current_cue_source_dir[PATH_MAX];
static int current_cue_source_row = -1;

static void cue_track_row_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    if (!current_cue_sheet_valid || index < 0 || index >= current_cue_sheet.track_count) return;

    int count = current_cue_sheet.track_count;
    char ** paths = malloc(sizeof(char *) * (size_t) count);
    if (!paths) return;
    int copied = 0;
    for (; copied < count; copied++) {
        paths[copied] = strdup(current_cue_sheet.audio_path);
        if (!paths[copied]) break;
    }
    if (copied != count) {
        for (int i = 0; i < copied; i++) free(paths[i]);
        free(paths);
        show_error_toast("Not enough memory to load CUE tracks");
        return;
    }
    set_player_source_file_browser(current_cue_source_dir, current_cue_source_row);
    on_file_selected_at(paths, count, index, current_cue_sheet.tracks[index].start_seconds);
}

static void populate_cue_tracks_screen(void) {
    lv_obj_clean(cue_tracks_list);
    for (int i = 0; i < current_cue_sheet.track_count; i++) {
        cue_track_t * t = &current_cue_sheet.tracks[i];
        char label[160];
        /* Falls back to the plain track number when a sheet doesn't set
         * TITLE for a track (rare but real -- some auto-generated sheets
         * only carry INDEX times) rather than showing an empty row. */
        if (t->title[0]) {
            snprintf(label, sizeof(label), "%d. %s", t->number, t->title);
        } else {
            snprintf(label, sizeof(label), "Track %d", t->number);
        }
        lv_obj_t * row = add_playlist_row_base(cue_tracks_list, label);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, cue_track_row_click_cb, LV_EVENT_CLICKED, (void *) (intptr_t) i);
    }
}

static lv_obj_t * build_cue_tracks_screen(void) {
    lv_obj_t * scr = build_subsonic_list_screen("Tracks", &cue_tracks_title_label, &cue_tracks_list);
    lv_obj_set_flex_align(cue_tracks_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    return scr;
}

/* file_browser.h's on_cue_select callback -- see file_browser_init()'s own
 * comment. Parses fresh on every tap (a .cue sheet is tiny, no reason to
 * cache) and replaces whatever sheet this screen was last showing. */
void on_cue_file_selected(const char * cue_path) {
    cue_sheet_free(&current_cue_sheet);
    current_cue_sheet_valid = cue_parse_file(cue_path, &current_cue_sheet);
    if (!current_cue_sheet_valid) {
        show_error_toast("Couldn't read this .cue file");
        return;
    }
    snprintf(current_cue_source_dir, sizeof(current_cue_source_dir), "%s", file_browser_get_last_selected_dir());
    current_cue_source_row = file_browser_get_last_selected_row();
    lv_label_set_text(cue_tracks_title_label, basename_of(current_cue_sheet.audio_path));
    populate_cue_tracks_screen();
    nav_push(cue_tracks_screen);
}

/* Settings > Update Music Database: reruns library_scan_once() (a full
 * rescan + tag re-read of MUSIC_ROOT_DIR, same as gui_init's startup call)
 * on a background thread -- a real library's tag reads are too slow to do
 * on the UI thread, same reasoning as every other pthread_create() in this
 * file -- then rebuilds the four prebuilt library screens (All Songs,
 * Artists, Albums, Album Artist), since unlike group_songs_screen/
 * artist_albums_screen (which rebuild their rows on every visit) these are
 * only ever built once at startup: each one's compact_list_set_paged_
 * provider() call captured a total_count at that build time, which a
 * rescan can make stale (more/fewer songs, artists, albums), so the whole
 * screen is rebuilt to pick up a fresh count rather than trying to patch
 * one in place. Playlists isn't one of these four -- see its own build_
 * playlists_screen()/populate_playlists_screen() comment for why it
 * doesn't need rebuilding here. */
static pthread_t library_rescan_thread;
bool library_rescan_active = false;
static atomic_bool library_rescan_done_flag = false;

static void * library_rescan_thread_func(void * arg) {
    (void) arg;
    library_scan_once();
    atomic_store_explicit(&library_rescan_done_flag, true, memory_order_release); /* written last -- update_timer_cb only checks this flag */
    return NULL;
}

void start_library_rescan(void) {
    /* Real-device incident: several call sites below don't already guard on
     * library_rescan_active themselves, and a second call while a rescan
     * thread is still running would spawn a second library_rescan_thread_
     * func() thread racing the first over metadata_db.c's own scan-
     * generation state (metadata_db_begin_update()/end_update()) and
     * stomping the single library_rescan_thread handle -- undefined
     * behavior, not just wasted work. */
    if (library_rescan_active) return;
    /* A previous scan's own post-scan thumbnail-generation pass (Task 2)
     * might still be running -- signal it to stop now rather than let it
     * keep working against data this new scan is about to make stale.
     * Non-blocking: whichever call eventually starts the NEXT generation
     * pass (including this new scan's own completion, a few seconds from
     * now) is what actually pthread_join()s it -- see start_album_
     * thumbnail_generation()'s own comment. */
    cancel_album_thumbnail_generation();
    atomic_store_explicit(&library_rescan_done_flag, false, memory_order_relaxed);
    library_rescan_active = true;
    library_rescan_token = gui_busy_show("Updating\nmusic database...", "");
    gui_busy_set_progress(library_rescan_token, 0);
        if (pthread_create(&library_rescan_thread, NULL, library_rescan_thread_func, NULL) != 0) {
        library_rescan_active = false;
        gui_busy_hide(library_rescan_token);
        show_error_toast("Thread launch failed");
    }
}

/* Rebuilds the five prebuilt library screens (All Songs, Artists, Albums,
 * Album Artist, Recently Added) so each one's compact_list_set_paged_
 * provider() call recaptures a fresh total_count against the just-reloaded
 * library --
 * unlike group_songs_screen/artist_albums_screen (which rebuild their rows
 * on every visit) these are only ever built once at startup, so a stale
 * cached count would otherwise persist after ANY reload of the underlying
 * data, not just a full rescan. Shared by poll_library_rescan()'s
 * background-scan-completion path below and reload_library_on_sd_reinsert()'s
 * fast-cache-load path further down, both of which replace that data via a
 * different route (library_scan_once() vs library_load_from_cache_only())
 * but need the exact same screen-side cleanup afterward. playlists_screen
 * deliberately NOT rebuilt here -- see poll_library_rescan()'s own former
 * comment on why (still applies verbatim): its content is recomputed fresh
 * on every visit already, never stale to begin with. */
static void refresh_library_screens_after_reload(void) {
    /* A hot-insert cache reload can arrive while one of these screens is
     * the active LVGL screen. Deleting an active screen leaves the display's
     * act_scr pointer dangling; the next refresh then crashes in
     * lv_obj_update_layout(). A user-triggered full rescan is different:
     * its non-library progress screen is active and must remain visible
     * long enough to show the completion message, so only preserve that
     * explicitly safe case. Resetting also removes deeper group screens
     * whose rows reference the library arrays replaced by the reload. */
    if (lv_screen_active() != gui_busy_get_screen()) {
        nav_reset_to_home();
    } else {
        /* Audit finding: this exception only skipped nav_reset_to_home()
         * -- the lv_obj_delete() calls below always run regardless. Real
         * bug: subsonic_downloading_screen is a shared singleton (Wi-Fi
         * "Connecting...", Subsonic connect, SD format, and library
         * rescan all reuse it), not exclusive to the rescan flow this
         * exception was written for. If the user reached one of these
         * four screens via some other in-progress navigation (e.g. All
         * Songs -> Wi-Fi settings -> "Connecting...") and a reload lands
         * while that shared screen is active, its stack slot goes stale
         * the instant lv_obj_delete() runs below -- and a later nav_pop()
         * back through it loads a freed screen. Purge any of the four
         * from nav_stack before deleting them, even though the active
         * screen itself is deliberately left alone. */
        lv_obj_t * being_replaced[] = { all_songs_screen, artists_screen, albums_screen, album_artist_screen,
                                         recently_added_screen };
        for (int i = nav_depth - 1; i >= 0; i--) {
            for (size_t j = 0; j < sizeof(being_replaced) / sizeof(being_replaced[0]); j++) {
                if (nav_stack[i] == being_replaced[j]) {
                    nav_remove_stack_slot(i);
                    break;
                }
            }
        }
    }

    lv_obj_delete(all_songs_screen);
    lv_obj_delete(artists_screen);
    lv_obj_delete(albums_screen);
    lv_obj_delete(album_artist_screen);
    lv_obj_delete(recently_added_screen);
    /* Each build_*_screen() below activates its own paged provider against
     * the current (fresh, post-reload) library internally -- no separate
     * populate step needed here, and no whole-library load either: drill-
     * down, search, and the A-Z index are all DB-backed end to end now, so
     * a rescan/SD-reinsert reload costs exactly what a fresh boot does,
     * nothing more. */
    all_songs_screen = build_all_songs_screen();
    artists_screen = build_artists_screen();
    albums_screen = build_albums_screen();
    album_artist_screen = build_album_artist_screen();
    recently_added_screen = build_recently_added_screen();

    /* The four old screens' A-Z index bindings (strip/popup/list pointers)
     * just went dangling along with the lv_obj_delete()s above -- re-register
     * against the freshly rebuilt screens/lists before anything can poll them. */
    az_index_registered_count = 0;
    register_az_index(artists_screen, artists_list, METADATA_DB_AZ_ARTIST);
    register_az_index(albums_screen, albums_list, METADATA_DB_AZ_ALBUM);
    register_az_index(album_artist_screen, album_artist_list, METADATA_DB_AZ_ALBUM_ARTIST);
    register_az_index(all_songs_screen, all_songs_list, METADATA_DB_AZ_ALL_SONGS);

    register_search(SEARCH_BINDING_ARTISTS, artists_screen, artists_list, NULL, NULL, false,
                     true, METADATA_DB_AZ_ARTIST, artists_fetch_page);
    register_search(SEARCH_BINDING_ALBUMS, albums_screen, albums_list, NULL, NULL, false,
                     true, METADATA_DB_AZ_ALBUM, albums_fetch_page);
    register_search(SEARCH_BINDING_ALBUM_ARTIST, album_artist_screen, album_artist_list, NULL,
                     NULL, false, true, METADATA_DB_AZ_ALBUM_ARTIST, album_artists_fetch_page);
    register_search(SEARCH_BINDING_ALL_SONGS, all_songs_screen, all_songs_list, NULL, NULL, false,
                     true, METADATA_DB_AZ_ALL_SONGS, all_songs_fetch_page);
}

/* SD-card-reinsertion fast path -- see poll_sd_card_hotplug()'s own call
 * site. Real-device feature request: reinserting a card the app has
 * already scanned before was recreating/re-reading its whole tag cache
 * from scratch every single time (a full start_library_rescan(), same
 * multi-minute cost as a genuinely new library, since that's the only path
 * poll_sd_card_hotplug() ever used on the mount edge) instead of just
 * loading the cache that same card's own root already carries (metadata_db.c's
 * METADATA_DB_PATH lives ON the SD card itself, not somewhere device-global
 * -- see its own comment -- so a previously-scanned card reinserted here
 * has its own already-populated database sitting right there).
 * library_load_from_cache_only() (a bounded tagcache read, not a filesystem
 * walk -- see its own comment) already does exactly this. A missing or
 * empty database leaves the library empty; Settings > Update Music
 * Database is the only path that walks files. A toast rather than the
 * "Updating music database..." screen -- this is a reload the user did
 * not ask for. */
static void reload_library_on_sd_reinsert(void) {
    library_load_from_cache_only();
    refresh_library_screens_after_reload();
    if (metadata_db_get_song_count() > 0) {
        show_info_toast("Library loaded");
        start_album_thumbnail_generation();
    }
}

/* How long the "Library updated" success message stays up once a rescan
 * finishes, before falling back to Home -- purely so the user gets a
 * moment to actually read it, not a wait for anything real. */
#define LIBRARY_RESCAN_SUCCESS_MS 1500
bool library_rescan_success_pending = false;
static uint32_t library_rescan_success_since_tick = 0;
void poll_library_rescan(void) {
    /* Cache-only SD reinsertion also starts a generation pass, but has no
     * progress phase of its own. Reap that naturally completed worker here
     * instead of retaining one joinable thread until some future scan. */
    if (album_thumb_gen_thread_joinable && !atomic_load(&album_thumb_gen_active))
        reap_album_thumbnail_generation();

    /* Keep the screen awake for the whole "Updating music database..."/
     * "Library updated" window -- label and progress-bar text updates
     * don't touch LVGL's own indev-driven inactivity clock (no touch, no
     * hw button), so without this the auto screen-timeout would fire mid-
     * scan on a short timeout setting same as it would on any other idle
     * screen. Real-device feedback: with the screen-timeout fix that made
     * touch no longer wake an auto-slept screen (only the power button
     * does now), that read as the whole device freezing -- a dark,
     * touch-unresponsive screen mid-rescan looks identical to a genuine
     * hang from the outside. */
    if (library_rescan_active || library_rescan_success_pending)
        lv_display_trigger_activity(NULL);

    if (library_rescan_success_pending) {
        if (lv_tick_elaps(library_rescan_success_since_tick) >= LIBRARY_RESCAN_SUCCESS_MS) {
            library_rescan_success_pending = false;
            nav_reset_to_home(); /* leaves the busy screen and discards any stale deeper screen */
        }
        return;
    }

    if (!library_rescan_active) return;

    if (!atomic_load_explicit(&library_rescan_done_flag, memory_order_acquire)) {
        /* Still scanning -- total stays 0 until the initial file walk
         * finishes (see library_scan_once()), so there's nothing
         * meaningful to show yet in that window; the label just keeps
         * reading "Updating music database..." until then. */
        if (library_scan_progress_total > 0) {
            gui_busy_set_progress(library_rescan_token, (int32_t) ((int64_t) library_scan_progress_done * 100 / library_scan_progress_total));
        }
        return;
    }

    library_rescan_active = false;
    pthread_join(library_rescan_thread, NULL);

    refresh_library_screens_after_reload();
    /* Thumbnail generation is optional cache warming. It must not become a
     * second user-visible phase after "Updating music database..." -- no
     * progress screen, no toast. The worker yields and cancels when Albums
     * opens so visible-row decode stays first. */
    start_album_thumbnail_generation();
    gui_busy_hide(library_rescan_token); show_info_toast("Library updated");
    library_rescan_success_pending = true;
    library_rescan_success_since_tick = lv_tick_get();
}

/* SD mount-failure detection + Format SD Card -- see poll_sd_card_hotplug()
 * below for the detection/debounce logic and sd_format_card_worker() further
 * down for the actual format sequence. These three are declared here
 * (rather than alongside their real definitions further down) so that both
 * halves of the feature -- the gated detection logic below, and the
 * ungated popups/background thread that come after it in this file -- can
 * see them regardless of which one is defined first. */
static void show_sd_mount_failed_popup(void); /* defined below, alongside its popup */
static bool sd_mount_fail_notified = false;
static volatile bool sd_format_active = false;

#ifndef HOST_BUILD
/* Real-device bug report: reinserting the SD card while the device is
 * already on doesn't populate its files in the player. Root cause -- see
 * main.c's own mount_sd_card_if_needed() comment for the full real-device
 * investigation: this firmware has NO hotplug mechanism at all for the
 * internal SD card slot (confirmed by reading every relevant piece of
 * config -- /etc/mdev.conf only has a rule for external USB mass-storage
 * sd[a-z] devices, not the internal mmcblk* card; no fstab entry; no
 * init.d script). Something else on the system does eventually retry
 * mounting it reactively, but confirmed (via dmesg) to take on the order
 * of many minutes, and even once mounted, this app itself never notices --
 * only the user-triggered Settings > Update Music Database rescan reads
 * the SD card, and nothing was polling for "did it just become mounted"
 * to trigger that automatically.
 *
 * This polls whether MUSIC_ROOT_DIR is currently a real mount point (its
 * st_dev differs from its parent /data/mnt's -- the standard POSIX "is
 * this a mountpoint" check, since an unmounted MUSIC_ROOT_DIR is just an
 * empty directory living directly on /data/mnt's own filesystem). While
 * not mounted, it retries mount_sd_card_if_needed() itself every few
 * seconds instead of waiting on whatever slow reactive mechanism the OS
 * has -- harmless to call when nothing's inserted (see that function's own
 * comment: it just fails silently). On the unmounted -> mounted edge, it
 * loads the tagcache already on that card (if any). It does not start
 * Settings > Update Music Database -- a full walk is user-triggered only.
 *
 * Real-device feature request (2026-08-08): removal wasn't handled at all
 * symmetrically -- pulling the card left All Songs/Artists/Albums/
 * Playlists, and the Files screen, still showing entries for files that no
 * longer exist (tapping one would just fail to play). The mounted ->
 * unmounted edge now closes the database, rebuilds the library screens
 * empty, and resets Files -- it must not scan, because a scan against the
 * empty unmounted directory would write a blank database onto the parent
 * filesystem and, if the card remounted mid-scan, walk the whole card.
 *
 * That edge relies on sd_card_root_is_mounted() actually flipping to false
 * on a physical eject, which isn't guaranteed on its own: this firmware has
 * no hotplug mechanism at all for this slot (see above), so nothing ever
 * calls umount() when the card is pulled -- the VFS mount entry for
 * MUSIC_ROOT_DIR can just sit there claiming to still be mounted (same
 * st_dev) with reads underneath it now failing at the I/O layer instead.
 * sd_card_device_node_present() checks for the card's block device node
 * separately -- mdev creates/removes /dev/mmcblk0p1 directly off the
 * kernel's own uevents for the mmc host controller's card-detect line, a
 * baseline mdev behavior independent of the custom per-device *rules*
 * mdev.conf lacks for mmcblk* (those only add extra actions on top, they're
 * not what makes the node itself appear/disappear) -- so the node going
 * away is a real, kernel-driven signal of physical removal even though
 * nothing above the block layer reacts to it. When it disappears while
 * still nominally mounted, this self-issues the same `umount` this
 * firmware's own mass_storage_removing.sh uses for external USB media, so
 * the ordinary mounted -> unmounted edge below fires correctly on the very
 * next poll instead of never firing at all. Unverified against a real
 * eject on this exact device/kernel at the time this was written (no live
 * unit on hand) -- flagged for a real removal-cycle test rather than
 * asserted as confirmed working. */
#define SD_CARD_MOUNT_POLL_SECONDS 5

/* Consecutive failed poll cycles (SD_CARD_MOUNT_POLL_SECONDS apart) before
 * the mount-failure popup shows -- 6 * 5s = 30s of genuinely stuck retries,
 * matching this file's own LIBRARY_SCAN_WALK_STALL_TIMEOUT_MS in spirit
 * (long enough that the kernel's own brief card-enumeration window right
 * after physical insertion can't false-positive, short enough the user
 * isn't left staring at an empty library wondering what's wrong). */
#define SD_MOUNT_FAIL_STREAK_THRESHOLD 6

static bool sd_card_root_is_mounted(void) {
    struct stat parent_st, root_st;
    if (stat("/data/mnt", &parent_st) != 0) return false;
    if (stat(MUSIC_ROOT_DIR, &root_st) != 0) return false;
    return parent_st.st_dev != root_st.st_dev;
}

static bool sd_card_device_node_present(void) {
    struct stat st;
    /* Either node counts as "still present" -- mount_sd_card_if_needed()
     * (main.c) now falls back to the whole-disk node for a partition-less
     * card (see its own comment), so a card mounted that way only ever has
     * a /dev/mmcblk0 node, never a p1 one. Checking p1 alone here would
     * make this wrongly conclude such a card had been physically removed
     * on the very next poll after it successfully mounted, force-unmounting
     * a card that's still sitting right there. */
    return stat("/dev/mmcblk0p1", &st) == 0 || stat("/dev/mmcblk0", &st) == 0;
}

/* Whole-disk node, as opposed to sd_card_device_node_present()'s first-
 * partition node above -- present whenever a card is physically inserted
 * regardless of whether it has a partition table at all, so this is what
 * distinguishes "no card" from "card inserted but can't be mounted" for the
 * mount-failure/Format SD Card flow below (mount_sd_card_if_needed() itself
 * -- see main.c -- already tries every filesystem type this platform
 * actually supports, so a card that's physically present but still never
 * mounts genuinely needs a repartition/reformat, not just a different -t). */
static bool sd_card_base_device_present(void) {
    struct stat st;
    return stat("/dev/mmcblk0", &st) == 0;
}

void poll_sd_card_hotplug(void) {
    /* Starts true: mount_sd_card_if_needed() already ran once at boot
     * (main.c, before gui_init()) and the initial library load already
     * happened against whatever was mounted by then -- assuming "already
     * mounted" here avoids this poll re-triggering a redundant rescan on
     * its very first tick when the card was present all along. If it
     * wasn't actually mounted yet at that point, this does NOT reliably
     * fall into the "not mounted" case below (see boot_library_recheck_
     * done's own comment further down for why -- that used to be this
     * comment's own claim, and was wrong: a boot-time race that self-heals
     * within this function's very first tick never confirms as "not
     * mounted" for long enough to flip this back to false). */
    static bool was_mounted = true;
    /* See its own comment further down, where it's checked -- separate
     * one-shot guard for the boot-time-mount-race case was_mounted's own
     * confirmed-transition logic can silently miss. */
    static bool boot_library_recheck_done = false;
    static time_t last_check = 0;
    static int mount_fail_streak = 0;
    /* Real-device testing: a plain single-poll "was mounted, now isn't"
     * edge fires the removal-collapse rescan below too eagerly -- confirmed
     * live that mount_sd_card_if_needed() (called synchronously, right
     * above that rescan's own pthread_create()) can complete a re-mount
     * faster than this function's own SD_CARD_MOUNT_POLL_SECONDS poll
     * interval, so a brief unmount (this app's own umount -l retry above,
     * or -- unverified but plausible -- a very fast physical reseat) can
     * have already remounted again by the time the removal handler
     * actually runs, making it close and reopen against the *already-back*
     * real card instead of collapsing to empty, which
     * reload_library_on_sd_reinsert() below exists to do on the insert
     * edge. Requiring the unmounted state to be seen on
     * SD_UNMOUNT_CONFIRM_STREAK_THRESHOLD consecutive polls (reset to 0 the
     * moment "mounted" is seen again, see the mounted branch below) before
     * acting filters that out -- a genuine removal stays gone far longer
     * than this short confirmation window, so real removal handling is
     * delayed by only a few seconds, not skipped. */
    static int unmount_confirm_streak = 0;
#define SD_UNMOUNT_CONFIRM_STREAK_THRESHOLD 2

    time_t now = time(NULL);
    if (last_check != 0 && now - last_check < SD_CARD_MOUNT_POLL_SECONDS) return;
    last_check = now;

    bool mounted = sd_card_root_is_mounted();
    if (mounted && !sd_card_device_node_present()) {
        /* -l (lazy): the node's already gone, so there's no real device
         * left to flush to, and a plain umount would fail with EBUSY if
         * this app (or anything else) still has an fd open on a file under
         * MUSIC_ROOT_DIR from right before the card was pulled -- the same
         * reasoning mass_storage_removing.sh already applies to external
         * USB media on this firmware (see its own "umount -l" call). */
        char * argv[] = { (char *) "umount", (char *) "-l", (char *) MUSIC_ROOT_DIR, NULL };
        subprocess_run(argv, NULL, 0);
        mounted = false;
    }

    if (!mounted) {
        mount_sd_card_if_needed();
        if (was_mounted) {
            /* Stop post-scan artwork reads/writes as soon as removal is
             * observed. Joining is deliberately deferred to the next
             * generator start/reap poll so this hotplug callback never
             * blocks the UI on an in-progress image decode. */
            cancel_album_thumbnail_generation();
            unmount_confirm_streak++;
            if (unmount_confirm_streak >= SD_UNMOUNT_CONFIRM_STREAK_THRESHOLD && !library_rescan_active) {
                /* Close the SD-resident tagcache so a later reinsert opens
                 * the files on whichever card is actually mounted, not a
                 * stale handle. Do not scan: the mountpoint is empty and
                 * a scan would write a blank database there. */
                metadata_db_close();
                refresh_library_screens_after_reload();
                file_browser_reset_to_root();
                was_mounted = false;
                unmount_confirm_streak = 0;
            }
            /* Else: not yet confirmed (or a rescan from something else,
             * e.g. Settings > Update Music Database, is already running)
             * -- was_mounted deliberately stays true, so a flicker that
             * remounts before reaching the threshold below is
             * indistinguishable from nothing having happened at all: the
             * mounted branch's own streak reset plus its own
             * `!was_mounted` reinsertion check (still false) mean neither
             * edge ever fires. */
        }

        /* Card is physically there (the whole-disk node exists) but still
         * won't mount after repeated retries. mount_sd_card_if_needed()
         * (main.c) already tries every supported filesystem type against
         * both the first-partition node and, as a fallback, the whole-disk
         * node itself (covering a "superfloppy" card with no partition
         * table/MBR at all) -- so reaching here means either the partition
         * exists but holds something none of those attempts could mount
         * (corruption, or a filesystem this platform genuinely doesn't
         * support), or the raw disk itself does too. Both need the same fix
         * -- reformat -- so this doesn't try to tell them apart. Debounced
         * (SD_MOUNT_FAIL_STREAK_THRESHOLD
         * consecutive failed polls, SD_CARD_MOUNT_POLL_SECONDS apart) so
         * the kernel's own brief card-enumeration window right after
         * physical insertion -- where the node legitimately isn't there
         * *yet* -- doesn't fire a false alarm; not re-shown on every poll
         * once shown once (sd_mount_fail_notified), so the user isn't
         * nagged again until the card actually changes (removed, or a
         * format attempt runs -- both reset it below/in poll_sd_format()). */
        if (sd_card_base_device_present()) {
            mount_fail_streak++;
            if (mount_fail_streak >= SD_MOUNT_FAIL_STREAK_THRESHOLD && !sd_mount_fail_notified && !sd_format_active) {
                sd_mount_fail_notified = true;
                show_sd_mount_failed_popup();
            }
        } else {
            mount_fail_streak = 0;
            sd_mount_fail_notified = false;
        }
        return;
    }

    mount_fail_streak = 0;
    sd_mount_fail_notified = false;
    unmount_confirm_streak = 0; /* seeing "mounted" again cancels any not-yet-confirmed removal */
    if (!was_mounted && !library_rescan_active) {
        reload_library_on_sd_reinsert();
        file_browser_reset_to_root();
    } else if (!boot_library_recheck_done) {
        /* Real-device bug report: library randomly not loaded after a
         * reboot (worked around by enabling Resume Last Track, whose own
         * fallback -- build_saved_resume_playlist()'s file_browser_build_
         * playlist_for_path() call -- does a live single-folder filesystem
         * scan independent of the in-memory library, masking this rather
         * than fixing it). Root cause: mount_sd_card_if_needed() (main.c)
         * runs once, synchronously, before gui_init() -- if the SD card's
         * device node hasn't been created by the kernel yet at that exact
         * point (enumeration timing varies boot to boot), that mount
         * silently fails with no retry, so library_load_from_cache_only()
         * (called from gui_init()) finds music_root_is_mounted() false and
         * leaves the library empty for the rest of the session. This
         * function's own retry above (mount_sd_card_if_needed() a few
         * lines up) usually fixes the mount itself within its very first
         * tick -- but that means was_mounted (which starts true, per its
         * own comment above) never actually observes a confirmed
         * not-mounted state (unmount_confirm_streak never reaches
         * SD_UNMOUNT_CONFIRM_STREAK_THRESHOLD), so it's never flipped to
         * false, so the !was_mounted branch above never fires either: the
         * card ends up correctly mounted at the filesystem level, but
         * nothing ever tells the app's in-memory library to reload. One-
         * shot (not tied to was_mounted) so this can't fire more than once
         * per boot even for a card that's mounted but genuinely has no
         * music -- reload_library_on_sd_reinsert() only loads the on-card
         * database (or leaves the library empty), so an empty card does
         * not start a repeating full scan. */
        boot_library_recheck_done = true;
        /* metadata_db_get_song_count() (the real database), not
         * all_songs_count -- that's deliberately never populated at boot
         * anymore (see library_load_from_cache_only()'s own comment), so
         * checking it here would read 0 on literally every single boot
         * regardless of whether the SD-mount race this guards against
         * actually happened, forcing reload_library_on_sd_reinsert()'s own
         * full eager load every time and defeating this rework's entire
         * point. */
        if (metadata_db_get_song_count() == 0 && !library_rescan_active) {
            reload_library_on_sd_reinsert();
            file_browser_reset_to_root();
        }
    }
    was_mounted = true;
}
#else
void poll_sd_card_hotplug(void) {
}
#endif

/* Actual "Format SD Card" sequence -- runs on a background thread (see
 * start_sd_format()/poll_sd_format() below), since fdisk/mkdosfs can take
 * real time on a slow card and this must never block the UI thread.
 *
 * UNVERIFIED ON REAL HARDWARE. Every command here was chosen from static
 * analysis of the real firmware's own busybox binary (`strings` confirmed
 * mkdosfs/fdisk/dd/umount/partprobe are all compiled-in applets, including
 * fdisk's own interactive prompt text, proving a standard util-linux-style
 * command menu), not from an actual run -- this is a MIPS binary and can't
 * be executed on the dev host, and no live device was available while this
 * was written. Test with a spare/non-critical card before trusting it with
 * a card that matters.
 *
 * Sequence: best-effort unmount, zero the first sector (so fdisk always
 * sees a blank disk regardless of whatever partition table, or lack of
 * one, the card came in with -- avoids needing to conditionally script
 * fdisk's own delete-partition flow for "already has a stale/foreign
 * table"), script fdisk to create one primary partition spanning the whole
 * disk typed W95 FAT32 (LBA), partprobe to force the kernel to notice the
 * new partition, wait for its device node to appear, mkdosfs it, then hand
 * off to the normal mount path and confirm it actually mounted. */
#ifndef HOST_BUILD
static bool sd_format_card_worker(void) {
    char * umount_argv[] = { (char *) "umount", (char *) "-l", (char *) MUSIC_ROOT_DIR, NULL };
    subprocess_run(umount_argv, NULL, 0);

    if (!sd_card_base_device_present()) return false; /* card pulled before/during format */

    char * dd_argv[] = { (char *) "dd", (char *) "if=/dev/zero", (char *) "of=/dev/mmcblk0",
                          (char *) "bs=512", (char *) "count=1", NULL };
    if (!subprocess_run_timeout(dd_argv, NULL, 0, 15000)) return false;

    pid_t fdisk_pid;
    int fdisk_write_fd;
    char * fdisk_argv[] = { (char *) "fdisk", (char *) "/dev/mmcblk0", NULL };
    if (!subprocess_popen_stdin(fdisk_argv, &fdisk_pid, &fdisk_write_fd)) return false;
    /* n(ew) -> p(rimary) -> partition 1 -> default first sector [Enter] ->
     * default last sector [Enter] -> t(ype) -> c (W95 FAT32, LBA) -> w(rite)+exit. */
    const char * fdisk_cmds = "n\np\n1\n\n\nt\nc\nw\n";
    ssize_t ignored = write(fdisk_write_fd, fdisk_cmds, strlen(fdisk_cmds));
    (void) ignored;
    close(fdisk_write_fd);
    subprocess_terminate(fdisk_pid); /* reaps it -- `w` alone isn't guaranteed to have taken effect yet */

    char * partprobe_argv[] = { (char *) "partprobe", (char *) "/dev/mmcblk0", NULL };
    subprocess_run_timeout(partprobe_argv, NULL, 0, 10000);

    bool got_partition = false;
    for (int i = 0; i < 20 && !got_partition; i++) {
        if (sd_card_device_node_present()) { got_partition = true; break; }
        usleep(250000);
    }
    if (!got_partition) return false;

    char * mkdosfs_argv[] = { (char *) "mkdosfs", (char *) "-F", (char *) "32", (char *) "-n",
                               (char *) "MUSIC", (char *) "/dev/mmcblk0p1", NULL };
    if (!subprocess_run_timeout(mkdosfs_argv, NULL, 0, 120000)) return false;

    mount_sd_card_if_needed();
    return sd_card_root_is_mounted();
}
#else
static bool sd_format_card_worker(void) {
    return false;
}
#endif

static pthread_t sd_format_thread;
static atomic_bool sd_format_done_flag = false;
static volatile bool sd_format_succeeded = false;

static void * sd_format_thread_func(void * arg) {
    (void) arg;
    sd_format_succeeded = sd_format_card_worker();
    atomic_store_explicit(&sd_format_done_flag, true, memory_order_release); /* written last -- poll_sd_format() only checks this flag */
    return NULL;
}

static void start_sd_format(void) {
    atomic_store_explicit(&sd_format_done_flag, false, memory_order_relaxed);
    sd_format_active = true;
    sd_format_token = gui_busy_show("Formatting\nSD Card...", "");
        if (pthread_create(&sd_format_thread, NULL, sd_format_thread_func, NULL) != 0) {
        sd_format_active = false;
        gui_busy_hide(sd_format_token);
        show_error_toast("Thread launch failed");
    }
}

void poll_sd_format(void) {
    if (!sd_format_active || !atomic_load_explicit(&sd_format_done_flag, memory_order_acquire)) return;

    sd_format_active = false;
    pthread_join(sd_format_thread, NULL);
    gui_busy_hide(sd_format_token);

    if (sd_format_succeeded) {
        show_error_toast("SD card formatted");
        sd_mount_fail_notified = false; /* give the freshly-formatted card a clean slate */
        if (!library_rescan_active) {
            start_library_rescan();
            file_browser_reset_to_root();
        }
    } else {
        show_error_toast("SD card format failed");
    }
}

static lv_obj_t * sd_mount_failed_popup;
static lv_obj_t * sd_mount_failed_popup_backdrop;
static lv_obj_t * sd_format_confirm_popup;
static lv_obj_t * sd_format_confirm_popup_backdrop;

static void hide_sd_mount_failed_popup(void) {
    lv_obj_add_flag(sd_mount_failed_popup_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(sd_mount_failed_popup, LV_OBJ_FLAG_HIDDEN);
}

static void hide_sd_format_confirm_popup(void) {
    lv_obj_add_flag(sd_format_confirm_popup_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(sd_format_confirm_popup, LV_OBJ_FLAG_HIDDEN);
}

static void sd_mount_failed_popup_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_sd_mount_failed_popup();
}

static void sd_mount_failed_dismiss_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_sd_mount_failed_popup();
}

static void sd_format_confirm_popup_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_sd_format_confirm_popup();
}

static void sd_format_cancel_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_sd_format_confirm_popup();
}

static void sd_mount_failed_format_btn_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_sd_mount_failed_popup();
    lv_obj_remove_flag(sd_format_confirm_popup_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(sd_format_confirm_popup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(sd_format_confirm_popup_backdrop);
    lv_obj_move_foreground(sd_format_confirm_popup);
}

static void sd_format_confirm_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_sd_format_confirm_popup();
    start_sd_format();
}

/* Only ever shown from the debounced detection in poll_sd_card_hotplug()
 * above -- deliberately has no permanent home in Settings, so it can't be
 * used to format a perfectly working card by mistake; it only exists at
 * all once a real, sustained mount failure has actually been observed. */
static void show_sd_mount_failed_popup(void) {
    lv_obj_remove_flag(sd_mount_failed_popup_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(sd_mount_failed_popup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(sd_mount_failed_popup_backdrop);
    lv_obj_move_foreground(sd_mount_failed_popup);
}

static void build_sd_mount_failed_popup(void) {
    sd_mount_failed_popup = build_confirm_popup(
        "SD card couldn't be read", LV_LABEL_LONG_WRAP, NULL,
        "It may have no partition table or a file system this player can't use. "
        "Formatting will erase it and set it up for this player.",
        "Format SD Card", lv_color_make(255, 120, 120), sd_mount_failed_format_btn_cb, NULL, "Dismiss",
        accent_lv_color(), sd_mount_failed_dismiss_cb, NULL, sd_mount_failed_popup_backdrop_cb,
        &sd_mount_failed_popup_backdrop);
}

static void build_sd_format_confirm_popup(void) {
    sd_format_confirm_popup = build_confirm_popup(
        "Erase and format SD card?", LV_LABEL_LONG_WRAP, NULL,
        "This permanently deletes everything on the card. This cannot be undone.", "Format",
        lv_color_make(255, 120, 120), sd_format_confirm_cb, NULL, "Cancel", accent_lv_color(), sd_format_cancel_cb,
        NULL, sd_format_confirm_popup_backdrop_cb, &sd_format_confirm_popup_backdrop);
}

/* Power-off countdown -- shown when hw_buttons_consume_power_long_press()
 * fires (see update_timer_cb()'s own consumer block). Same lv_layer_top()
 * overlay shape as sd_mount_failed_popup/sd_format_confirm_popup above,
 * built once and shown/hidden by flag rather than nav_push()'d, so it can
 * appear over whatever screen is currently active. Unlike those two, the
 * countdown itself isn't tied to the popup being visible for any particular
 * duration on its own -- poll_power_off_countdown() (called every tick from
 * update_timer_cb) drives it, and idle_shutdown_now() at zero doesn't
 * return on a real device, so there's no explicit "now power off" call site
 * beyond that. */
#define POWER_OFF_COUNTDOWN_SECONDS 3

static lv_obj_t * power_off_countdown_popup;
static lv_obj_t * power_off_countdown_popup_backdrop;
static lv_obj_t * power_off_countdown_label;
static bool power_off_countdown_active = false;
static uint32_t power_off_countdown_start_tick;

static void hide_power_off_countdown_popup(void) {
    lv_obj_add_flag(power_off_countdown_popup_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(power_off_countdown_popup, LV_OBJ_FLAG_HIDDEN);
}

static void cancel_power_off_countdown(void) {
    power_off_countdown_active = false;
    hide_power_off_countdown_popup();
}

static void power_off_countdown_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    cancel_power_off_countdown();
}

static void power_off_countdown_cancel_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    cancel_power_off_countdown();
}

void start_power_off_countdown(void) {
    power_off_countdown_active = true;
    power_off_countdown_start_tick = lv_tick_get();
    lv_label_set_text_fmt(power_off_countdown_label, "%d", POWER_OFF_COUNTDOWN_SECONDS);
    lv_obj_remove_flag(power_off_countdown_popup_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(power_off_countdown_popup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(power_off_countdown_popup_backdrop);
    lv_obj_move_foreground(power_off_countdown_popup);
}

/* Called every tick from update_timer_cb while power_off_countdown_active --
 * updates the on-screen seconds-remaining label, and calls idle_shutdown_now()
 * once the countdown reaches zero (only reachable by letting it run out;
 * tapping Cancel or the backdrop aborts it first, via
 * cancel_power_off_countdown() above). */
void poll_power_off_countdown(void) {
    if (!power_off_countdown_active) return;

    uint32_t elapsed_ms = lv_tick_elaps(power_off_countdown_start_tick);
    uint32_t total_ms = (uint32_t) POWER_OFF_COUNTDOWN_SECONDS * 1000;
    if (elapsed_ms >= total_ms) {
        power_off_countdown_active = false;
        idle_shutdown_now(); /* does not return on a real device */
        return;
    }

    int seconds_left = (int) ((total_ms - elapsed_ms + 999) / 1000);
    lv_label_set_text_fmt(power_off_countdown_label, "%d", seconds_left);
}

void build_power_off_countdown_popup(void) {
    lv_obj_t * top = lv_layer_top();

    power_off_countdown_popup_backdrop = lv_obj_create(top);
    lv_obj_set_size(power_off_countdown_popup_backdrop, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(power_off_countdown_popup_backdrop, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(power_off_countdown_popup_backdrop, LV_OPA_50, 0);
    lv_obj_set_style_border_width(power_off_countdown_popup_backdrop, 0, 0);
    lv_obj_remove_flag(power_off_countdown_popup_backdrop, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(power_off_countdown_popup_backdrop, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(power_off_countdown_popup_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(power_off_countdown_popup_backdrop, power_off_countdown_backdrop_cb, LV_EVENT_CLICKED, NULL);

    power_off_countdown_popup = lv_obj_create(top);
    lv_obj_set_size(power_off_countdown_popup, 320, 280);
    lv_obj_align(power_off_countdown_popup, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(power_off_countdown_popup, 16, 0);
    lv_obj_add_style(power_off_countdown_popup, &style_theme_card_bg, 0);
    lv_obj_set_style_bg_opa(power_off_countdown_popup, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(power_off_countdown_popup, 0, 0);
    lv_obj_remove_flag(power_off_countdown_popup, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(power_off_countdown_popup, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t * title = lv_label_create(power_off_countdown_popup);
    lv_obj_set_width(title, lv_pct(90));
    lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_style(title, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(title, gui_theme_font(GUI_FONT_ROLE_ROW), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);
    lv_label_set_text(title, "Powering Off");

    power_off_countdown_label = lv_label_create(power_off_countdown_popup);
    lv_obj_add_style(power_off_countdown_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(power_off_countdown_label, gui_theme_font(GUI_FONT_ROLE_TITLE), 0);
    lv_obj_align(power_off_countdown_label, LV_ALIGN_CENTER, 0, -10);
    lv_label_set_text_fmt(power_off_countdown_label, "%d", POWER_OFF_COUNTDOWN_SECONDS);

    lv_obj_t * cancel_row = lv_obj_create(power_off_countdown_popup);
    lv_obj_set_size(cancel_row, lv_pct(90), 56);
    lv_obj_align(cancel_row, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_set_style_radius(cancel_row, 12, 0);
    lv_obj_set_style_bg_opa(cancel_row, 0, 0);
    lv_obj_set_style_border_width(cancel_row, 0, 0);
    lv_obj_remove_flag(cancel_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(cancel_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(cancel_row, power_off_countdown_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * cancel_label = lv_label_create(cancel_row);
    lv_label_set_text(cancel_label, "Cancel");
    lv_obj_set_style_text_color(cancel_label, accent_lv_color(), 0);
    lv_obj_set_style_text_font(cancel_label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_obj_center(cancel_label);
}

void update_music_database_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    start_library_rescan();
}

/* No whole-library load anywhere in this app anymore -- all paged/DB-
 * backed now, see build_artists_screen()'s own comment. Drilling into an
 * artist/album-artist's own albums (artist_row_click_cb()/album_artist_
 * row_click_cb()), an album's own songs (album_row_click_cb()), search,
 * and the A-Z index are all direct, targeted DB queries. */
static void artists_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(artists_screen);
}

static void albums_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(albums_screen);
}

static void album_artist_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(album_artist_screen);
}

static void playlists_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    playlists_edit_mode = false; /* fresh entry from the menu always starts out of edit mode, same convention as show_group_songs_editable() */
    populate_playlists_screen();
    nav_push(playlists_screen);
}


static void music_files_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(files_screen);
}

static lv_obj_t * build_music_screen(void) {
    static icon_grid_item_t items[6];
    items[0] = (icon_grid_item_t){ "category/explorer.png", "category/explorer_s.png", "Files", music_files_tile_cb, NULL };
    items[1] = (icon_grid_item_t){ "category/artist.png", "category/artist_s.png", "Artists", artists_tile_cb, NULL };
    items[2] = (icon_grid_item_t){ "category/album.png", "category/album_s.png", "Albums", albums_tile_cb, NULL };
    items[3] = (icon_grid_item_t){ "category/album_artist.png", "category/album_artist_s.png", "Album Artist", album_artist_tile_cb, NULL };
    items[4] = (icon_grid_item_t){ "category/all.png", "category/all_s.png", "All Songs", all_songs_tile_cb, NULL };
    /* No dedicated "playlist" icon exists anywhere in the stock theme pack
     * (assets/theme2/category/ has album/album_artist/all/artist/explorer/
     * genre/item/net_radio and nothing else playlist-shaped) -- reusing
     * genre.png/genre_s.png here since Genres no longer has a tile of its
     * own to need it. */
    items[5] = (icon_grid_item_t){ "category/genre.png", "category/genre_s.png", "Playlists", playlists_tile_cb, NULL };
    lv_obj_t * scr = build_icon_grid_screen("Music", generic_back_cb, items, 6, 100, false, 0);
    finalize_screen_navigation(scr);
    return scr;
}


/* Shared click handler for every plugin-registered Stream Media tile below
 * -- user_data is the tile's index into plugin_manager's own
 * plugin_stream_tiles[] (not an LVGL object), same index-not-object shape
 * plugin_list_row_click_cb() already uses. */
void plugin_stream_tile_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    plugin_manager_stream_tile_clicked(index);
}

/* Subsonic is the one built-in, real, working option here -- Qobuz/Tidal
 * (paid regional subscriptions + a real API integration this project's
 * author couldn't personally test or verify against) and Net Radio (never
 * wired up past a placeholder) were removed rather than left as dead/
 * stalled tiles. Its icon (stream_media/subsonic.png/_s.png) isn't from
 * the stock theme pack at all (Subsonic isn't a stock HiBy feature) --
 * see assets.c's THEME_OVERRIDE_ROOT for how this app adds its own new
 * asset on top of the stock resource pack despite that living on
 * read-only storage on a real device.


/* Drill-down target for Artists and Album Artists: tapping either shows the
 * artist's own albums (regrouping just their songs by album, via
 * build_groups_by_indices) rather than dumping straight into a flat song
 * list -- tapping an album from there lands on show_group_songs() same as
 * every other terminal list. One persistent screen shared by both callers,
 * same rebuild-in-place approach as group_songs_screen. */
static group_row_t * artist_albums_groups;
static int artist_albums_group_count;

/* Defined with the shared bounded artwork cache below. */
static void album_row_thumbnail_decorator(lv_obj_t * list, lv_obj_t * row, lv_obj_t * image,
                                           int logical_index, int pool_slot, int64_t song_id, void * ctx);

/* Now-playing indicator bar -- same "recreated fresh every populate call"
 * lifecycle as group_songs_now_playing_bar (see its own comment):
 * populate_indexed_list()'s lv_obj_clean(artist_albums_list) destroys
 * whatever was here before, same as every row. */
/* This drill-down is a compact list too: fixed row count regardless of how
 * many albums one artist owns, and therefore able to share the exact album
 * artwork decorator/cache used by Music -> Albums. */
static void artist_album_row_click_cb(int index) {
    if (index < 0 || index >= artist_albums_group_count) return;
    group_row_t * group = &artist_albums_groups[index];
    group_song_entry_t * entries = calloc((size_t) group->song_count, sizeof(*entries));
    int n = 0;
    song_row_t page[64];
    while (entries && n < group->song_count) {
        int want = group->song_count - n;
        if (want > 64) want = 64;
        int got = metadata_db_get_album_songs(group->name, group->album_artist, n, page, want);
        if (got <= 0) break;
        for (int i = 0; i < got; i++) {
            char title[128];
            metadata_db_song_display_title(&page[i], title, sizeof(title));
            entries[n + i].path = strdup(page[i].path);
            entries[n + i].title = strdup(title);
            if (!entries[n + i].path || !entries[n + i].title) {
                free_group_song_entries(entries, group->song_count);
                entries = NULL;
                n = 0;
                break;
            }
        }
        if (!entries) break;
        n += got;
        if (got < want) break;
    }
    if (!entries) return;
    show_group_songs(group->name, entries, n);
    free_group_song_entries(entries, n);
    group_songs_source_is_album = true;
}

/* A direct lv_image_dsc_t points into the bounded LRU's pixel allocation.
 * Refresh on every screen load so a list that was hidden while the other
 * album screen populated/evicted cache entries never redraws an old row
 * against an evicted descriptor. This also immediately picks up artwork
 * decoded while the list was off-screen. */
static void album_thumbnail_screen_loaded_cb(lv_event_t * e) {
    lv_obj_t * list = (lv_obj_t *) lv_event_get_user_data(e);
    album_thumbnail_begin_screen(list);
}

static void album_thumbnail_screen_unloaded_cb(lv_event_t * e) {
    lv_obj_t * list = (lv_obj_t *) lv_event_get_user_data(e);
    album_thumbnail_end_screen(list);
}

/* Matches by song-index membership (not album NAME, unlike Albums/Album
 * Artist's own name-based match) -- this screen already only ever holds
 * albums belonging to the ONE artist just tapped into (artist_albums_
 * groups is built from artist_group->indices), so an index check is both
 * more precise (no risk of a same-named album by a different artist
 * lighting this up) and cheaper than resolving artist/album strings again.
 * Same standalone-refresh reasoning as refresh_group_songs_now_playing_
 * indicator() -- callable without rebuilding rows, e.g. on a live playback
 * change while this screen stays open. */
void refresh_artist_albums_now_playing_indicator(void) {
    if (!artist_albums_list) return;

    int match = -1;
    song_row_t playing;
    if (now_playing_path[0] && metadata_db_get_song_by_path(now_playing_path, &playing)) {
        for (int i = 0; i < artist_albums_group_count; i++) {
            if (strcasecmp(artist_albums_groups[i].name, playing.tags.album) == 0 &&
                strcasecmp(artist_albums_groups[i].album_artist, playing.tags.album_artist) == 0) {
                match = i;
                break;
            }
        }
    }

    compact_list_set_now_playing(artist_albums_list, match);
}

void show_artist_albums(const char * name, metadata_db_group_kind_t kind) {
    free(artist_albums_groups);
    artist_albums_groups = NULL;
    int64_t count64 = metadata_db_count_albums_for_group(kind, name);
    artist_albums_group_count = count64 > 0 && count64 <= INT_MAX ? (int) count64 : 0;
    if (artist_albums_group_count > 0) {
        artist_albums_groups = malloc(sizeof(*artist_albums_groups) * (size_t) artist_albums_group_count);
        if (!artist_albums_groups) artist_albums_group_count = 0;
        else artist_albums_group_count = metadata_db_get_albums_for_group(kind, name, 0, artist_albums_group_count,
                                                                           artist_albums_groups);
    }

    lv_label_set_text(artist_albums_title_label, name);
    compact_list_item_t * items = artist_albums_group_count > 0
        ? malloc(sizeof(*items) * (size_t) artist_albums_group_count) : NULL;
    if (items) {
        for (int i = 0; i < artist_albums_group_count; i++) {
            items[i] = (compact_list_item_t){
                .label = artist_albums_groups[i].name,
                .identity = artist_albums_groups[i].first_song_id,
                .trailing_asset = NULL
            };
        }
        compact_list_set_items(artist_albums_list, items, artist_albums_group_count);
        free(items);
    } else {
        compact_list_set_items(artist_albums_list, NULL, 0);
    }
    compact_list_set_row_decorator(artist_albums_list, album_row_thumbnail_decorator, NULL);
    refresh_artist_albums_now_playing_indicator();

    nav_push(artist_albums_screen);
}

/* Paged Artists/Album Artist -- see build_artists_screen()'s own comment.
 * offset is a position in the DB's own name-sorted order (ORDER BY artist/
 * album_artist COLLATE NOCASE) -- a row tapped here (artist_row_click_cb()/
 * album_artist_row_click_cb() below) re-resolves that same offset via
 * metadata_db_get_groups_page() itself to get the tapped name, rather than
 * caching anything from this page fetch. */
static int artists_fetch_page(void * ctx, int offset, int count, compact_list_page_row_t out_rows[]) {
    (void) ctx;
    group_row_t * rows = malloc(sizeof(group_row_t) * (size_t) count);
    int n = rows ? metadata_db_get_groups_page(METADATA_DB_GROUP_ARTIST, offset, count, rows) : 0;
    for (int i = 0; i < n; i++) {
        snprintf(out_rows[i].label, sizeof(out_rows[i].label), "%s", rows[i].name);
        out_rows[i].identity = rows[i].first_song_id;
        out_rows[i].trailing_asset[0] = '\0';
    }
    free(rows);
    return n;
}

static int album_artists_fetch_page(void * ctx, int offset, int count, compact_list_page_row_t out_rows[]) {
    (void) ctx;
    group_row_t * rows = malloc(sizeof(group_row_t) * (size_t) count);
    int n = rows ? metadata_db_get_groups_page(METADATA_DB_GROUP_ALBUM_ARTIST, offset, count, rows) : 0;
    for (int i = 0; i < n; i++) {
        snprintf(out_rows[i].label, sizeof(out_rows[i].label), "%s", rows[i].name);
        out_rows[i].identity = rows[i].first_song_id;
        out_rows[i].trailing_asset[0] = '\0';
    }
    free(rows);
    return n;
}



void gui_library_init(void) {
    files_screen = build_files_screen();
    all_songs_screen = build_all_songs_screen();
    recently_added_screen = build_recently_added_screen();
    artists_screen = build_artists_screen();
    albums_screen = build_albums_screen();
    album_artist_screen = build_album_artist_screen();
    playlists_screen = build_playlists_screen();
    cue_tracks_screen = build_cue_tracks_screen();
    group_songs_screen = build_group_songs_screen();

    artist_albums_screen = build_compact_list_screen("Albums", generic_back_cb, NULL, 0,
                                                      artist_album_row_click_cb, NULL,
                                                      &artist_albums_list, &artist_albums_title_label,
                                                      LIST_ROW_WIDTH_WIDE, true, accent_lv_color());
    compact_list_set_row_height(artist_albums_list, MUSIC_LIST_ROW_HEIGHT);
    lv_obj_add_event_cb(artist_albums_screen, album_thumbnail_screen_loaded_cb,
                        LV_EVENT_SCREEN_LOADED, artist_albums_list);
    lv_obj_add_event_cb(artist_albums_screen, album_thumbnail_screen_unloaded_cb,
                        LV_EVENT_SCREEN_UNLOADED, artist_albums_list);
    lv_obj_add_event_cb(artist_albums_list, album_thumbnail_scroll_cb, LV_EVENT_SCROLL_BEGIN, NULL);
    lv_obj_add_event_cb(artist_albums_list, album_thumbnail_scroll_cb, LV_EVENT_SCROLL_END, NULL);
    finalize_screen_navigation(artist_albums_screen);
    add_to_playlist_screen = build_add_to_playlist_screen();
    music_screen = build_music_screen();

    /* Register A-Z index & search */
    register_az_index(artists_screen, artists_list, METADATA_DB_AZ_ARTIST);
    register_az_index(albums_screen, albums_list, METADATA_DB_AZ_ALBUM);
    register_az_index(album_artist_screen, album_artist_list, METADATA_DB_AZ_ALBUM_ARTIST);
    register_az_index(all_songs_screen, all_songs_list, METADATA_DB_AZ_ALL_SONGS);

    register_search(SEARCH_BINDING_ARTISTS, artists_screen, artists_list, NULL, NULL, false,
                     true, METADATA_DB_AZ_ARTIST, artists_fetch_page);
    register_search(SEARCH_BINDING_ALBUMS, albums_screen, albums_list, NULL, NULL, false,
                     true, METADATA_DB_AZ_ALBUM, albums_fetch_page);
    register_search(SEARCH_BINDING_ALBUM_ARTIST, album_artist_screen, album_artist_list, NULL,
                     NULL, false, true, METADATA_DB_AZ_ALBUM_ARTIST, album_artists_fetch_page);
    register_search(SEARCH_BINDING_ALL_SONGS, all_songs_screen, all_songs_list, NULL, NULL, false,
                     true, METADATA_DB_AZ_ALL_SONGS, all_songs_fetch_page);

    files_search_list = build_compact_list_widget(files_screen, NULL, 0, files_search_row_click_cb, NULL, LIST_ROW_WIDTH_WIDE, false, lv_color_black());
    lv_obj_set_style_bg_opa(files_search_list, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(files_search_list, lv_color_black(), 0);
    lv_obj_add_flag(files_search_list, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(files_search_list, LV_OBJ_FLAG_GESTURE_BUBBLE);
    enable_gesture_bubble_recursive(files_search_list);

    register_search(SEARCH_BINDING_FILES, files_screen, files_search_list, NULL, NULL, true,
                     true, METADATA_DB_AZ_ALL_SONGS, all_songs_fetch_page);

    build_sd_mount_failed_popup();
    build_sd_format_confirm_popup();
}

void gui_library_resume_fast_timers(void) {
    if (az_index_drag_timer) lv_timer_resume(az_index_drag_timer);
}
