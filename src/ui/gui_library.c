#include "gui_navigation.h"
#include "gui_library.h"
#include "assets.h"
#include "idle_shutdown.h"
#include "backlight.h"
#include "usb_mode_control.h"
#include "gui_player.h"

extern void on_file_browser_selected(char ** new_playlist, int count, int selected_index);
extern void open_queue_screen(void);
extern void nav_remove_stack_slot(int depth);
extern void mount_sd_card_if_needed(void);
extern void library_scan_once(void);
extern void library_load_from_cache_only(void);
void refresh_artist_albums_now_playing_indicator(void);
#include "gui.h"
#include "gui_theme.h"
#include "gui_notifications.h"
#include "gui_settings.h"
#include "gui_text_input.h"
#include "gui_subsonic.h"
#include "gui_books.h"
#include "fallback_font.h"
#include "plugin_manager.h"
#include "screen_builders.h"
#include "metadata.h"
#include "metadata_db.h"
#include "file_browser.h"
#include "playlist_files.h"
#include "cue_parser.h"
#include "cover_decode.h"
#include "albumart.h"
#include "artwork_coordinator.h"
#include "device_config.h"
#include "settings.h"
#include "audio.h"
#include "subprocess.h"
#include "db_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <limits.h>
#include <ctype.h>
#include <time.h>
#include <stdarg.h>
#include <errno.h>
#include <sched.h>
/* SCHED_BATCH below is glibc's own <bits/sched.h> policy constant, gated
 * behind _GNU_SOURCE there -- this file deliberately doesn't define that
 * (see search_matches()'s own comment on strcasestr() for why). The raw
 * kernel UAPI header defines the same numeric policy constants ungated on
 * both glibc and musl (confirmed in both toolchains' linux/sched.h), so
 * this alone is enough on host and target alike without touching feature-
 * test macros anywhere in this file. */
#include <linux/sched.h>
#include <sys/resource.h>

#ifdef HOST_BUILD
  #define MUSIC_ROOT_DIR "./music"
#else
  #define MUSIC_ROOT_DIR "/data/mnt/sd_0"
#endif

#define PLAYLISTS_DIR MUSIC_ROOT_DIR "/Playlists"
#define COMPACT_LIST_PAGE_CACHE_SIZE 64
/* STATUS_BAR_CLEARANCE is provided by screen_builders.h. */
#define EXTERNAL_COVER_MAX_BYTES (4U * 1024U * 1024U)

static lv_obj_t * album_thumbnail_active_list = NULL;
static atomic_int album_thumbnail_generation = 0;
static bool group_songs_source_is_album = false;
static gui_busy_handle_t library_rescan_token = 0;
static gui_busy_handle_t sd_format_token = 0;
static atomic_int library_scan_progress_total = 0;
static atomic_int library_scan_progress_done = 0;

static uint64_t albums_page_open_requested_ms;
static uint64_t album_lazy_job_started_ms;
static unsigned album_lazy_queued;
static unsigned album_lazy_completed;
static unsigned album_lazy_with_art;
static unsigned album_lazy_stale;

static void album_thumbnail_screen_loaded_cb(lv_event_t * e);
static void album_thumbnail_screen_unloaded_cb(lv_event_t * e);
static void album_thumbnail_scroll_cb(lv_event_t * e);
static int artists_fetch_page(void * ctx, int offset, int count, compact_list_page_row_t out_rows[]);
static int albums_fetch_page(void * ctx, int offset, int count, compact_list_page_row_t out_rows[]);
static int album_artists_fetch_page(void * ctx, int offset, int count, compact_list_page_row_t out_rows[]);
static int all_songs_fetch_page(void * ctx, int offset, int count, compact_list_page_row_t out_rows[]);
static void music_files_tile_cb(lv_event_t * e);
static void album_more_click_cb(int index);
static void artist_album_more_click_cb(int index);
static void build_collection_menus(void);

/* Screen pointers owned by this module */
static lv_obj_t * music_screen = NULL;
static lv_obj_t * files_screen = NULL;
static lv_obj_t * files_search_list = NULL;

static lv_obj_t * all_songs_screen = NULL;
static lv_obj_t * all_songs_list = NULL;

static lv_obj_t * recently_added_screen = NULL;
static lv_obj_t * recently_added_list = NULL;

static lv_obj_t * artists_screen = NULL;
static lv_obj_t * artists_list = NULL;

static lv_obj_t * albums_screen = NULL;
static lv_obj_t * albums_list = NULL;

static lv_obj_t * album_artist_screen = NULL;
static lv_obj_t * album_artist_list = NULL;

static lv_obj_t * group_songs_screen = NULL;
static lv_obj_t * group_songs_list = NULL;
static lv_obj_t * group_songs_title_label = NULL;

static lv_obj_t * artist_albums_screen = NULL;
static lv_obj_t * artist_albums_list = NULL;
static lv_obj_t * artist_albums_title_label = NULL;

static lv_obj_t * playlists_screen = NULL;
static lv_obj_t * playlists_list = NULL;

static lv_obj_t * cue_tracks_screen = NULL;
static lv_obj_t * cue_tracks_list = NULL;
static lv_obj_t * cue_tracks_title_label = NULL;

static lv_obj_t * add_to_playlist_screen = NULL;
static lv_obj_t * add_to_playlist_list = NULL;

static lv_obj_t * collection_menu_popup = NULL;
static lv_obj_t * collection_menu_backdrop = NULL;
static lv_obj_t * album_collection_menu_popup = NULL;
static lv_obj_t * album_collection_menu_backdrop = NULL;
static bool collection_menu_is_album;
static char collection_menu_name[128];
static char collection_menu_album_artist[128];
static metadata_db_group_kind_t collection_menu_artist_kind;
static int collection_menu_song_count;

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
static void populate_playlists_screen(void);
static bool standalone_playlist_create;

static void new_playlist_name_done_cb(const char * text, void * user_data) {
    (void) user_data;
    if (text[0] == '\0') return;

    char created_path[512];
    bool ok = playlist_files_create(PLAYLISTS_DIR, text, add_to_playlist_target_path, created_path, sizeof(created_path));
    if (ok) metadata_db_playlist_insert_one(created_path);
    show_error_toast(ok ? "Playlist created" : "Failed to create playlist");
    if (!ok) return;
    if (standalone_playlist_create) populate_playlists_screen();
    else nav_pop();
}

static void new_playlist_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    standalone_playlist_create = lv_event_get_user_data(e) != NULL;
    if (standalone_playlist_create) add_to_playlist_target_path[0] = '\0';
    show_text_entry("Playlist Name", "", false, false, new_playlist_name_done_cb, NULL);
}

static void existing_playlist_row_cb(lv_event_t * e) {
    const char * path = (const char *) lv_event_get_user_data(e);
    if (add_to_playlist_target_path[0] == '\0') return;

    /* Check for duplicates before appending to avoid adding the same track twice. */
    if (playlist_files_contains(path, add_to_playlist_target_path)) {
        show_error_toast("Song already added");
        nav_pop();
        return;
    }

    bool ok = playlist_files_append(path, add_to_playlist_target_path);
    show_error_toast(ok ? "Added to playlist" : "Failed to add to playlist");
    nav_pop();
}

static void playlist_picker_path_free_cb(lv_event_t * e) {
    free(lv_event_get_user_data(e));
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
        lv_obj_add_event_cb(row, existing_playlist_row_cb, LV_EVENT_CLICKED, paths[i]);
        lv_obj_add_event_cb(row, playlist_picker_path_free_cb, LV_EVENT_DELETE, paths[i]);

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

    build_screen_header(scr, "Files", generic_back_cb, NULL, NULL);

    file_browser_init(scr, MUSIC_ROOT_DIR, on_file_browser_selected, on_cue_file_selected);

    finalize_screen_navigation(scr);
    return scr;
}

/* One song within a group_songs_screen listing (Artist/Album Artist's
 * albums, one album, Favorites, Most Played, a user .m3u playlist) -- an
 * owned path and display title resolved via targeted database query. */
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

static const char * library_codec_name(audio_codec_t codec) {
    switch (codec) {
        case AUDIO_CODEC_FLAC: return "FLAC";
        case AUDIO_CODEC_MP3: return "MP3";
        case AUDIO_CODEC_PCM: return "PCM";
        case AUDIO_CODEC_DSD: return "DSD";
        case AUDIO_CODEC_AAC: return "AAC";
        case AUDIO_CODEC_ALAC: return "ALAC";
        case AUDIO_CODEC_APE: return "APE";
        case AUDIO_CODEC_WMA: return "WMA";
        case AUDIO_CODEC_OPUS: return "Opus";
        case AUDIO_CODEC_VORBIS: return "Vorbis";
        case AUDIO_CODEC_UNKNOWN: break;
    }
    return "Audio";
}

static void format_music_submenu_identity(const song_row_t * song, char * out, size_t out_size) {
    char title[128];
    metadata_db_song_display_title(song, title, sizeof(title));
    audio_current_format_info_t info;
    if (audio_probe_file_format(song->path, &info) && info.duration_seconds > 0.0) {
        unsigned int seconds = (unsigned int)(info.duration_seconds + 0.5);
        snprintf(out, out_size, "%s\n%u:%02u · %s", title, seconds / 60, seconds % 60,
                 library_codec_name(info.codec));
        return;
    }
    const char * ext = strrchr(song->path, '.');
    snprintf(out, out_size, "%s\n%s", title, ext && ext[1] ? ext + 1 : "Audio");
}

void gui_library_format_song_identity(const song_row_t * row,
                                      char * title, size_t title_size,
                                      char * subtitle, size_t subtitle_size) {
    char artist[121], album[121];
    metadata_db_song_display_title(row, title, title_size);
    utf8_truncate_safe(artist, row->tags.artist[0] ? row->tags.artist : "Unknown artist", sizeof(artist));
    utf8_truncate_safe(album, row->tags.album[0] ? row->tags.album : "Unknown album", sizeof(album));
    snprintf(subtitle, subtitle_size, "%s · %s", artist, album);
}

static void fill_song_page_visual(compact_list_page_row_t * out, const song_row_t * row) {
    gui_library_format_song_identity(row, out->label, sizeof(out->label),
                                     out->subtitle, sizeof(out->subtitle));
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

/* Builds the "All Songs" screen with a paged provider
 * (compact_list_set_paged_provider()) so items are loaded incrementally
 * on scroll rather than materializing the full library in memory. */
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
static void playlist_start_options(bool recent);
static void format_song_identity(const song_row_t * row, char * out, size_t size) {
    char title[128], subtitle[256];
    gui_library_format_song_identity(row, title, sizeof(title), subtitle, sizeof(subtitle));
    snprintf(out, size, "%s\n%s", title, subtitle);
}

static int recently_added_fetch_page(void * ctx, int offset, int count, compact_list_page_row_t out_rows[]) {
    (void) ctx;
    int prefix = 0;
    if (offset == 0 && count > 0) {
        memset(&out_rows[0], 0, sizeof(out_rows[0]));
        snprintf(out_rows[0].label, sizeof(out_rows[0].label), "Play All");
        out_rows[0].is_action = true;
        snprintf(out_rows[0].subtitle, sizeof(out_rows[0].subtitle), "Choose sequential or shuffle playback");
        prefix = 1; count--; offset++;
    }
    if (count <= 0) return prefix;
    song_row_t * rows = malloc(sizeof(song_row_t) * (size_t) count);
    int n = rows ? metadata_db_get_songs_page_by_recency(offset - 1, count, rows) : 0;
    for (int i = 0; i < n; i++) {
        fill_song_page_visual(&out_rows[prefix + i], &rows[i]);
    }
    free(rows);
    return n + prefix;
}

/* Same role as all_songs_resolve_path_at() above, against the recency order. */
static bool recently_added_resolve_path_at(int display_index, char * out, size_t out_size) {
    song_row_t row;
    if (metadata_db_get_songs_page_by_recency(display_index, 1, &row) != 1) return false;
    snprintf(out, out_size, "%s", row.path);
    return true;
}

static void recently_added_row_click_cb(int display_index) {
    if (display_index == 0) { playlist_start_options(true); return; }
    display_index--;
    /* on_file_selected_lazy_recently_added() builds the queue lazily,
     * identity-mapped into the DB's own first_seen-DESC order (the same
     * display order this screen's paged provider uses) -- see its own
     * comment. Tap-to-play here is DB-driven end to end, same as All Songs. */
    set_player_source_recently_added(display_index);
    on_file_selected_lazy_recently_added(display_index);
}

static void recently_added_row_long_press_cb(int display_index) {
    if (display_index <= 0) return;
    display_index--;
    char path[600];
    if (recently_added_resolve_path_at(display_index, path, sizeof(path))) open_song_context_menu(path);
}

static lv_obj_t * build_recently_added_screen(void) {
    lv_obj_t * scr = build_compact_list_screen("Recently Added", generic_back_cb, NULL, 0, recently_added_row_click_cb,
                                                recently_added_row_long_press_cb, &recently_added_list, NULL,
                                                LIST_ROW_WIDTH_WIDE, true, accent_lv_color());
    compact_list_set_row_height(recently_added_list, MUSIC_LIST_ROW_HEIGHT);
    compact_list_set_paged_provider(recently_added_list, recently_added_fetch_page, NULL,
                                     (int) metadata_db_get_song_count() + 1);
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

/* Sibling to set_group_songs_entries() that takes ownership of an already
 * allocated entries[] array rather than copying it, avoiding duplicate
 * allocations for large song lists. The caller transfers ownership and
 * must not free `entries` or its elements afterward. */
static void set_group_songs_entries_owned(group_song_entry_t * entries, int count) {
    free_group_song_entries(group_songs_entries, group_songs_count);
    group_songs_entries = entries;
    group_songs_count = count;
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
static lv_obj_t * group_songs_visible_rows[GROUP_SONGS_PAGE_SIZE];
static bool group_songs_music_submenu;

/* Forward-declared here (defined after on_file_selected()) because
 * set_player_source_group_songs() needs group_songs_entries/count/title_label
 * already in scope, which the following definitions did not have. */
/* set_player_source_group_songs_direct defined in gui.c */


static void set_player_source_group_songs(int pos) {
    set_player_source_group_songs_direct(group_songs_entries, group_songs_count,
                                          lv_label_get_text(group_songs_title_label), pos);
    current_settings.last_source_kind = group_songs_source_is_album ? 2 : 0;
    snprintf(current_settings.last_source_name, sizeof(current_settings.last_source_name), "%s",
             group_songs_source_is_album ? lv_label_get_text(group_songs_title_label) : "");
}

/* Non-NULL only when the currently shown group is a user-created .m3u
 * playlist (set by show_m3u_playlist() via show_group_songs_editable()),
 * enabling the edit/remove-song UI. NULL for Artists/Albums/Favorites/
 * Most Played, which do not back onto a rewritable file. Borrowed pointer
 * from playlists_m3u_paths[]; valid for the lifetime of the screen.
 * group_songs_edit_mode is reset to false on every fresh entry. */
static const char * group_songs_edit_m3u_path = NULL;
static char group_songs_owned_m3u_path[PATH_MAX];
static void group_song_move_row_cb(lv_event_t * e);
static void reload_edited_playlist(void);
static struct stat group_songs_file_stat;
static bool group_songs_file_stat_valid;
static bool group_playlist_unchanged(void) {
    struct stat st;
    return group_songs_edit_m3u_path && group_songs_file_stat_valid &&
        stat(group_songs_edit_m3u_path, &st) == 0 && st.st_ino == group_songs_file_stat.st_ino &&
        st.st_dev == group_songs_file_stat.st_dev && st.st_size == group_songs_file_stat.st_size &&
        st.st_mtim.tv_sec == group_songs_file_stat.st_mtim.tv_sec &&
        st.st_mtim.tv_nsec == group_songs_file_stat.st_mtim.tv_nsec;
}
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
    if (group_songs_music_submenu) lv_obj_set_width(row, lv_pct(100));
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

/* Suppresses the follow-up LV_EVENT_CLICKED event when a long press has
 * already triggered the context menu on a group song row. */
static bool group_song_row_long_press_fired = false;

static void group_play_at(int pos) {
    if (pos < 0 || pos >= group_songs_count) return;
    if (group_songs_edit_m3u_path && !group_playlist_unchanged()) {
        reload_edited_playlist(); show_info_toast("Playlist changed. Select a song again."); return;
    }
    /* Allocate and copy playlist paths using calloc to handle potential
     * allocation failures cleanly. */
    char ** playlist_copy = calloc((size_t) group_songs_count, sizeof(char *));
    if (!playlist_copy) return;
    bool ok = true;
    for (int i = 0; i < group_songs_count; i++) {
        playlist_copy[i] = strdup(group_songs_entries[i].path);
        if (!playlist_copy[i]) {
            ok = false;
            break;
        }
    }
    if (!ok) {
        for (int i = 0; i < group_songs_count; i++) free(playlist_copy[i]);
        free(playlist_copy);
        return;
    }
    set_player_source_group_songs(pos);
    on_file_selected(playlist_copy, group_songs_count, pos);
}

static void group_song_row_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (group_song_row_long_press_fired) { group_song_row_long_press_fired = false; return; }
    group_play_at((int) (intptr_t) lv_event_get_user_data(e));
}

static lv_obj_t * playlist_start_popup, * playlist_start_backdrop;
static bool playlist_start_recent;

static void playlist_start_cancel(lv_event_t * e) {
    (void) e;
    lv_obj_add_flag(playlist_start_popup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(playlist_start_backdrop, LV_OBJ_FLAG_HIDDEN);
}
static void playlist_start_selected(lv_event_t * e, bool shuffle) {
    playlist_start_cancel(e);
    int count = playlist_start_recent ? (int) metadata_db_get_song_count() : group_songs_count;
    if (count <= 0) { show_info_toast("Playlist is empty"); return; }
    int index = 0;
    if (shuffle) {
        unsigned int value = (unsigned int) time(NULL) ^ lv_tick_get();
        int fd = open("/dev/urandom", O_RDONLY);
        if (fd >= 0) { ssize_t n = read(fd, &value, sizeof(value)); (void) n; close(fd); }
        index = (int) (value % (unsigned int) count);
    }
    gui_player_set_play_mode(shuffle ? PLAY_MODE_SHUFFLE : PLAY_MODE_SEQUENTIAL);
    if (playlist_start_recent) {
        set_player_source_recently_added(index);
        on_file_selected_lazy_recently_added(index);
    } else group_play_at(index);
}
static void playlist_start_sequential(lv_event_t * e) { playlist_start_selected(e, false); }
static void playlist_start_shuffle(lv_event_t * e) { playlist_start_selected(e, true); }
static void playlist_start_options(bool recent) {
    playlist_start_recent = recent;
    if (!playlist_start_popup) {
        static const menu_popup_row_t rows[] = {
            { "Start sequentially", playlist_start_sequential, false },
            { "Shuffle from a random song", playlist_start_shuffle, false },
            { "Cancel", playlist_start_cancel, false },
        };
        playlist_start_popup = build_menu_popup(rows, 3, playlist_start_cancel, &playlist_start_backdrop);
    }
    lv_obj_remove_flag(playlist_start_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(playlist_start_popup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(playlist_start_backdrop);
    lv_obj_move_foreground(playlist_start_popup);
}
static void group_start_options_cb(lv_event_t * e) {
    (void) e; playlist_start_options(false);
}

static void group_song_row_long_press_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_LONG_PRESSED) return;
    group_song_row_long_press_fired = true;
    int pos = (int) (intptr_t) lv_event_get_user_data(e);
    open_song_context_menu(group_songs_entries[pos].path);
}

/* Music's Artist/Album drill-down uses a deliberate two-line identity.
 * Playlist and Queue rows intentionally retain their established shared
 * builder geometry. */
static void layout_music_submenu_row_text(lv_obj_t * row) {
    if (!row || lv_obj_get_child_count(row) < 2) return;
    lv_obj_t * primary = lv_obj_get_child(row, 0);
    lv_obj_t * secondary = lv_obj_get_child(row, 1);
    if (lv_obj_has_flag(secondary, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_set_y(primary, BOARD_SCALE_PX(28));
        return;
    }
    lv_obj_set_y(primary, BOARD_SCALE_PX(18));
    lv_obj_set_y(secondary, BOARD_SCALE_PX(62));
}

/* Positions/shows or hides group_songs_now_playing_bar against the CURRENT
 * group_songs_entries/count -- callable standalone (no row rebuild, no
 * scroll reset) whenever now_playing_path changes while this screen
 * is open, and also called once at the end of populate_group_songs_rows()
 * itself so a freshly opened group (or an edit-mode toggle, which also goes
 * through a full repopulate) starts with the right state. The visible-row
 * table lets the marker follow LVGL's final flex layout, including runtime
 * font/touch sizing and any action or paging rows before the song. */
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
    lv_obj_t * row = group_songs_visible_rows[match - group_songs_page_start];
    if (!row) {
        lv_obj_add_flag(group_songs_now_playing_bar, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    /* Runtime font/touch sizing can make the rendered row taller than its
     * compile-time minimum. Follow the real laid-out object rather than
     * reconstructing its bounds from fixed height and gap constants. */
    lv_obj_update_layout(group_songs_list);
    lv_obj_remove_flag(group_songs_now_playing_bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(group_songs_now_playing_bar, 0, lv_obj_get_y(row));
    lv_obj_set_height(group_songs_now_playing_bar, lv_obj_get_height(row));
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
    memset(group_songs_visible_rows, 0, sizeof(group_songs_visible_rows));

    bool editable = group_songs_edit_m3u_path != NULL;
    if (editable) {
        lv_obj_clear_flag(group_songs_edit_btn, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(group_songs_edit_btn, group_songs_edit_mode ? "Done" : "Edit");
    } else {
        lv_obj_add_flag(group_songs_edit_btn, LV_OBJ_FLAG_HIDDEN);
    }

    bool editing = editable && group_songs_edit_mode;
    if (!editing) {
        lv_obj_t * start = add_group_songs_page_row(LV_SYMBOL_PLAY "  Play All", group_start_options_cb);
        lv_obj_add_style(start, &style_theme_card_bg, 0);
    }
    if (group_songs_count == 0) {
        build_list_message(group_songs_list, "Playlist is empty", "Add songs from a song menu.");
    }

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
            lv_obj_t * row = build_music_list_row(group_songs_list, group_songs_entries[i].title, NULL, BOARD_SCALE_PX(190));
            if (group_songs_music_submenu) lv_obj_set_width(row, lv_pct(100));
            group_songs_visible_rows[i - group_songs_page_start] = row;
            if (group_songs_music_submenu) layout_music_submenu_row_text(row);
            for (int direction = 0; direction < 2; direction++) {
                lv_obj_t * move = lv_label_create(row);
                lv_label_set_text(move, direction ? LV_SYMBOL_DOWN : LV_SYMBOL_UP);
                lv_obj_align(move, LV_ALIGN_RIGHT_MID, direction ? BOARD_SCALE_PX(-80) : BOARD_SCALE_PX(-130), 0);
                lv_obj_set_ext_click_area(move, BOARD_SCALE_PX(12));
                lv_obj_add_flag(move, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_add_event_cb(move, group_song_move_row_cb, LV_EVENT_CLICKED, (void *) (intptr_t) (i * 2 + direction));
            }

            lv_obj_t * remove_icon = lv_image_create(row);
            lv_image_set_src(remove_icon, asset_path("touch_list/del.png"));
            lv_obj_align(remove_icon, LV_ALIGN_RIGHT_MID, BOARD_SCALE_PX(-20), 0);
            lv_obj_add_flag(remove_icon, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(remove_icon, group_song_remove_row_cb, LV_EVENT_CLICKED, (void *) (intptr_t) i);
        } else {
            /* One lv_label via the shared list_row_style, not a container +
             * child label each with their own local style properties -- see
             * list_row_style's own doc comment (screen_builders.h). */
            lv_obj_t * row = build_music_list_row(group_songs_list, group_songs_entries[i].title, NULL, BOARD_SCALE_PX(70));
            if (group_songs_music_submenu) lv_obj_set_width(row, lv_pct(100));
            group_songs_visible_rows[i - group_songs_page_start] = row;
            if (group_songs_music_submenu) layout_music_submenu_row_text(row);

            lv_obj_t * quality = lv_image_create(row);
            lv_image_set_src(quality, asset_path(song_quality_asset_for_path(group_songs_entries[i].path)));
            /* Child alignment is relative to the label's padded content
             * box. Cancel the 70px text reserve so the badge is physically
             * 14px from the card edge (same rule as compact-list rows). */
            lv_obj_align(quality, LV_ALIGN_RIGHT_MID, BOARD_SCALE_PX(-14), 0);
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
    lv_obj_set_size(group_songs_now_playing_bar, BOARD_SCALE_PX(5), MUSIC_LIST_ROW_HEIGHT);
    lv_obj_set_style_bg_color(group_songs_now_playing_bar, accent_lv_color(), 0);
    lv_obj_set_style_bg_opa(group_songs_now_playing_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(group_songs_now_playing_bar, 2, 0);
    lv_obj_add_flag(group_songs_now_playing_bar, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_remove_flag(group_songs_now_playing_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(group_songs_now_playing_bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(group_songs_now_playing_bar, LV_OBJ_FLAG_HIDDEN);
    refresh_group_songs_now_playing_indicator();
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
 * Uses lv_obj_get_coords() after lv_obj_update_layout() on the common
 * parent so absolute coordinates are resolved before calculating the
 * available title width. */
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
                                       const char * editable_m3u_path, bool music_submenu) {
    group_songs_source_is_album = false;
    group_songs_music_submenu = music_submenu;
    if (editable_m3u_path) snprintf(group_songs_owned_m3u_path, sizeof(group_songs_owned_m3u_path), "%s", editable_m3u_path);
    group_songs_edit_m3u_path = editable_m3u_path ? group_songs_owned_m3u_path : NULL;
    group_songs_file_stat_valid = editable_m3u_path && stat(editable_m3u_path, &group_songs_file_stat) == 0;
    group_songs_edit_mode = false;
    set_group_songs_entries(entries, count);
    group_songs_page_start = 0;

    lv_label_set_text(group_songs_title_label, name);
    populate_group_songs_rows();

    nav_push(group_songs_screen);
}

void show_group_songs(const char * name, const group_song_entry_t * entries, int count) {
    show_group_songs_editable(name, entries, count, NULL, false);
}

static void show_music_group_songs(const char * name, const group_song_entry_t * entries, int count) {
    show_group_songs_editable(name, entries, count, NULL, true);
}

/* Ownership-transferring sibling of show_group_songs() -- see set_group_
 * songs_entries_owned()'s own comment for why this exists. Never editable
 * (no m3u path makes sense for a flattened, non-playlist view), same as
 * plain show_group_songs()'s own default. Caller must not free `entries`
 * after this call -- ownership has moved to group_songs_entries. */
static void show_group_songs_take_ownership(const char * name, group_song_entry_t * entries, int count) {
    group_songs_source_is_album = false;
    group_songs_music_submenu = true;
    group_songs_edit_m3u_path = NULL;
    group_songs_edit_mode = false;
    set_group_songs_entries_owned(entries, count);
    group_songs_page_start = 0;

    lv_label_set_text(group_songs_title_label, name);
    populate_group_songs_rows();

    nav_push(group_songs_screen);
}

static lv_obj_t * build_group_songs_screen(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &style_theme_screen_bg, 0);

    group_songs_title_label = build_screen_header(scr, "", generic_back_cb, NULL, NULL);
    /* Hidden by default -- populate_group_songs_rows() (via
     * show_group_songs_editable()) is what actually shows this, and only
     * for a group backed by an editable .m3u playlist. */
    group_songs_edit_btn = lv_label_create(scr);
    lv_label_set_text(group_songs_edit_btn, "Edit");
    lv_obj_set_style_text_color(group_songs_edit_btn, accent_lv_color(), 0);
    lv_obj_set_style_text_font(group_songs_edit_btn, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    align_screen_header_action(group_songs_edit_btn, BOARD_SCALE_PX(20));
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
    /* Clear padding so rows align flush with container edges. */
    lv_obj_set_style_pad_all(group_songs_list, 0, 0);
    lv_obj_set_scroll_dir(group_songs_list, LV_DIR_VER); /* see build_icon_grid_screen's comment */
    lv_obj_set_flex_flow(group_songs_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(group_songs_list, GUI_ROW_GAP, 0);
    lv_obj_set_style_pad_top(group_songs_list, GUI_ROW_GAP, 0);

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
 * T9 multi-tap keypad using themed keyboard assets. 12-key layout (1-9, Mode,
 * Shift, plus Del/Left/Right/Enter/Space) in a 4-column x 5-row grid; keys 2-9
 * cycle through their letter group (e.g. key 2 -> a -> b -> c) on repeated taps within
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
#define ALBUM_THUMBNAIL_PX ALBUMART_THUMBNAIL_SIZE
#define ALBUM_PLAYER_CACHE_PX ALBUMART_PLAYER_CACHE_SIZE
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
/* Set alongside result_pixels in album_thumbnail_thread_func(), valid only
 * when result_pixels is NULL (a failed decode) -- lets album_thumbnail_poll_cb()
 * ask artwork_failure_cache_is_blocked() whether THIS specific failure (already
 * recorded by album_thumbnail_load_or_decode_ex() on every one of its own
 * exit paths) was temporary or permanent, instead of caching every failure as
 * a permanent "no art" the way an unconditional `known = true` used to. */
static time_t album_thumbnail_result_source_mtime;
static bool album_thumbnail_result_have_source_mtime;
static lv_timer_t * album_thumbnail_poll_timer;
static album_thumbnail_request_t album_thumbnail_queue[ALBUM_THUMBNAIL_QUEUE_SIZE];
static atomic_bool album_thumbnail_screen_active;
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
    artwork_failure_cache_clear();
}

static bool album_thumbnail_sized_cache_hit(const albumart_info_t * info, char * found, size_t found_size) {
    return albumart_sized_thumb_fresh(info, ALBUM_THUMBNAIL_PX, ALBUM_THUMBNAIL_PX, found, found_size);
}

static bool album_player_cache_hit(const albumart_info_t * info, char * found, size_t found_size) {
    return albumart_generated_cache_fresh(info, ALBUM_PLAYER_CACHE_PX, ALBUM_PLAYER_CACHE_PX,
                                          found, found_size);
}

#define THUMBNAIL_SIDECAR_MAX_BYTES (2U * 1024U * 1024U)
#define ALBUM_ART_METADATA_TIMEOUT_MS 5000
/* Small startup allowance; the helper has a bounded address-space budget
 * and the returned picture is admitted at its actual compressed size. */
#define ALBUM_ART_METADATA_START_BYTES (1024U * 1024U)

/* Persistent warmer state flags */
static pthread_t album_thumb_gen_thread;
static atomic_bool album_thumb_gen_active;
static atomic_bool album_thumb_gen_cancel;
static atomic_int album_thumb_gen_generation;
static bool album_thumb_gen_thread_joinable;
static atomic_int album_thumb_gen_done_count;
static atomic_int album_thumb_gen_total_count;
static atomic_bool album_thumb_gen_retry_pending;
static uint32_t album_thumb_gen_retry_tick;
static volatile bool sd_format_active = false;

static bool album_thumb_gen_should_cancel(int my_generation) {
    return atomic_load(&album_thumb_gen_cancel) ||
           atomic_load(&album_thumb_gen_generation) != my_generation ||
           atomic_load(&album_thumbnail_screen_active);
}

static void cancel_album_thumbnail_generation(void) {
    if (!album_thumb_gen_thread_joinable || !album_thumb_gen_thread || !atomic_load(&album_thumb_gen_active)) return;
    atomic_store(&album_thumb_gen_cancel, true);
    atomic_fetch_add(&album_thumb_gen_generation, 1);
}

/* Waits for thumbnail generation thread completion. Requires both the
 * joinable flag and a non-NULL thread handle before calling pthread_join(). */
static void reap_album_thumbnail_generation(void) {
    if (!album_thumb_gen_thread_joinable) return;
    album_thumb_gen_thread_joinable = false;
    if (!album_thumb_gen_thread) return;
    pthread_join(album_thumb_gen_thread, NULL);
}

/* Dynamic cancellation callbacks */
static bool album_thumb_gen_cancel_cb(void * user_data) {
    int my_gen = (int) (intptr_t) user_data;
    return album_thumb_gen_should_cancel(my_gen);
}

static bool album_thumbnail_cancel_cb(void * user_data) {
    int gen = (int) (intptr_t) user_data;
    return (gen != album_thumbnail_generation);
}

static time_t album_source_mtime(const song_row_t * song, const albumart_info_t * info) {
    time_t max_mtime = 0;
    struct stat st;
    if (song && song->path[0] && stat(song->path, &st) == 0) {
        if (st.st_mtime > max_mtime) max_mtime = st.st_mtime;
        /* Parent folder mtime (detects adding or modifying cover.jpg) */
        char dir[PATH_MAX];
        snprintf(dir, sizeof(dir), "%s", song->path);
        char * slash = strrchr(dir, '/');
        if (slash) {
            *slash = '\0';
            if (stat(dir, &st) == 0 && st.st_mtime > max_mtime) max_mtime = st.st_mtime;
        }
    }
    if (info) {
        char found[PATH_MAX];
        if (albumart_search_files(info, "", found, sizeof(found))) {
            if (stat(found, &st) == 0 && st.st_mtime > max_mtime) max_mtime = st.st_mtime;
        }
    }
    return max_mtime;
}

/* The warmer builds the player-sized cache from the original compressed art
 * while it is already resident for the 72px decode.  Keeping this out of the
 * visible thumbnail path prevents scrolling from triggering a second large
 * decode; the player can then open the persistent 480px BMP without any
 * metadata extraction or lazy source decode.
 *
 * When a fresh 480px decode actually runs (warmer, cache miss) and is stored,
 * *out_player_pixels receives those pixels so the caller can derive the 72px
 * thumbnail from them instead of entropy-decoding the same source a second
 * time.  On every skip path (not warmer, cache already present) and every
 * failure, *out_player_pixels is NULL -- caller then does the 72px decode
 * itself, same as before.  Caller owns and must free() a non-NULL return. */
static cover_decode_result_t album_thumbnail_maybe_store_player_cache(
        const albumart_info_t * info, const uint8_t * data, uint32_t size,
        artwork_priority_t prio, artwork_cancel_fn cancel_cb, void * user_data,
        uint16_t ** out_player_pixels) {
    if (out_player_pixels) *out_player_pixels = NULL;
    if (prio != ARTWORK_PRIO_WARMER || !info || !data || size == 0)
        return COVER_DECODE_OK;
    char found[PATH_MAX];
    if (album_player_cache_hit(info, found, sizeof(found))) return COVER_DECODE_OK;

    uint16_t * pixels = NULL;
    cover_decode_result_t res = cover_decode_to_rgb565_ex(
        data, size, ALBUM_PLAYER_CACHE_PX, ALBUM_PLAYER_CACHE_PX, prio,
        cancel_cb, user_data, &pixels);
    if (res == COVER_DECODE_OK && pixels &&
        !albumart_store_rgb565(info, ALBUM_PLAYER_CACHE_PX, ALBUM_PLAYER_CACHE_PX, pixels))
        res = COVER_DECODE_FAIL_ALLOC;
    if (res == COVER_DECODE_OK && pixels && out_player_pixels) {
        *out_player_pixels = pixels;
        pixels = NULL;
    }
    free(pixels);
    return res;
}

/* 72px pixels: cover-fit from a just-decoded 480px buffer when the warmer
 * actually produced one, otherwise a single decode at thumbnail size. */
static cover_decode_result_t album_thumbnail_pixels_from_source(
        const uint8_t * data, uint32_t size, const uint16_t * player_pixels,
        artwork_priority_t prio, artwork_cancel_fn cancel_cb, void * user_data,
        uint16_t ** out_pixels) {
    if (player_pixels) {
        *out_pixels = cover_resize_rgb565(player_pixels,
                                          ALBUM_PLAYER_CACHE_PX, ALBUM_PLAYER_CACHE_PX,
                                          ALBUM_THUMBNAIL_PX, ALBUM_THUMBNAIL_PX);
        return *out_pixels ? COVER_DECODE_OK : COVER_DECODE_FAIL_ALLOC;
    }
    return cover_decode_to_rgb565_ex(data, size, ALBUM_THUMBNAIL_PX, ALBUM_THUMBNAIL_PX,
                                     prio, cancel_cb, user_data, out_pixels);
}

/* On-demand fallback for the Playing Now page when the warmer hasn't
 * produced the player-sized cache for this track yet: same source order as
 * album_thumbnail_load_or_decode_ex() (external sidecar, then embedded
 * picture via the bounded/isolated extraction below -- never a plain
 * metadata_read(), which can OOM on a huge APIC/covr tag), decodes and
 * persists the cache, and returns display-sized pixels so the caller has
 * something to show immediately instead of waiting for the warmer.
 *
 * *out_no_art_confirmed is set true only when this track was actually
 * determined to have no art anywhere (safe for the caller to stop asking
 * about it), and left false on every transient failure (coordinator/
 * memory-pressure admission refused, isolated-helper fork/pipe/timeout
 * failure, cancellation) -- a caller keying a negative cache off this must
 * not do so on a transient false, or a passing resource hiccup would
 * permanently hide art that is actually there. */
/* Shared by both sources below: decodes+stores the 480px player cache from
 * BORROWED `data`/`size` (caller still owns and must free it) and, on
 * success, resizes it into *out_pixels for immediate display. Deliberately
 * makes no negative-cache/"no art confirmed" decision of its own -- source
 * selection, input ownership, and confirmation semantics all stay the
 * caller's responsibility, since a permanent failure decoding ONE source
 * must not by itself confirm "no art anywhere" while another source still
 * has a chance (see gui_library_generate_player_cover()'s own comment). */
static cover_decode_result_t generate_player_cover_from_data(
        const albumart_info_t * info, const uint8_t * data, uint32_t size,
        artwork_cancel_fn cancel_cb, void * user_data, uint16_t ** out_pixels) {
    uint16_t * cache_pixels = NULL;
    cover_decode_result_t res = cover_decode_to_rgb565_ex(
        data, size, ALBUM_PLAYER_CACHE_PX, ALBUM_PLAYER_CACHE_PX,
        ARTWORK_PRIO_PLAYER, cancel_cb, user_data, &cache_pixels);
    if (res == COVER_DECODE_OK && cache_pixels) {
        albumart_store_rgb565(info, ALBUM_PLAYER_CACHE_PX, ALBUM_PLAYER_CACHE_PX, cache_pixels);
        *out_pixels = cover_resize_rgb565(cache_pixels, ALBUM_PLAYER_CACHE_PX, ALBUM_PLAYER_CACHE_PX,
                                          COVER_ART_WIDTH, COVER_ART_HEIGHT);
        if (!*out_pixels) res = COVER_DECODE_FAIL_ALLOC;
    }
    free(cache_pixels);
    return res;
}

bool gui_library_generate_player_cover(const char * track_path, const char * artist,
                                       const char * album, const char * album_artist,
                                       artwork_cancel_fn cancel_cb, void * user_data,
                                       uint16_t ** out_pixels, bool * out_no_art_confirmed) {
    *out_pixels = NULL;
    if (out_no_art_confirmed) *out_no_art_confirmed = false;
    if (!track_path || !track_path[0]) return false;

    albumart_info_t info = {0};
    snprintf(info.path, sizeof(info.path), "%s", track_path);
    snprintf(info.artist, sizeof(info.artist), "%s", artist ? artist : "");
    snprintf(info.album, sizeof(info.album), "%s", album ? album : "");
    snprintf(info.albumartist, sizeof(info.albumartist), "%s", album_artist ? album_artist : "");

    /* Step 1: try an external sidecar file first. A permanent DECODE failure
     * here (corrupt/oversized/unsupported -- as opposed to merely failing
     * to LOAD, or a transient/cancelled result) falls through to embedded
     * extraction below instead of giving up -- this function used to keep
     * whatever sidecar bytes loaded and never attempt embedded art at all
     * once ALBUMART_LOAD_OK was seen, permanently hiding real embedded art
     * behind one corrupt sidecar file. */
    char found[PATH_MAX];
    if (albumart_search_files(&info, "", found, sizeof(found))) {
        uint8_t * sidecar_data = NULL;
        uint32_t sidecar_size = 0;
        albumart_load_result_t load = albumart_load_file_ex(found, &sidecar_data, &sidecar_size,
                                                             THUMBNAIL_SIDECAR_MAX_BYTES, ARTWORK_PRIO_PLAYER);
        if (load == ALBUMART_LOAD_TEMPORARY) return false; /* transient -- do not confirm "no art" */
        if (load == ALBUMART_LOAD_OK) {
            DB_LOG("ART_PLAYER", "generate_source path=%s source=sidecar", track_path);
            cover_decode_result_t res = generate_player_cover_from_data(
                &info, sidecar_data, sidecar_size, cancel_cb, user_data, out_pixels);
            free(sidecar_data);
            if (res == COVER_DECODE_OK && *out_pixels) return true;
            if (res == COVER_DECODE_FAIL_CANCELLED) return false;
            if (cover_decode_result_is_temporary(res)) return false; /* transient -- do not confirm "no art" */
            /* Permanent sidecar decode failure -- fall through to embedded
             * extraction. Must NOT confirm "no art" here on its own; only
             * the fully-exhausted embedded path below may do that. */
        }
        /* ALBUMART_LOAD_INVALID (missing/unreadable/oversized) also falls
         * through here with no special-casing needed. */
    }

    /* Step 2: embedded picture from the audio file's own tags. Reached when
     * there's no sidecar, the sidecar failed to load, or it loaded but
     * failed to decode permanently. */
    artwork_acquire_result_t admission = artwork_coordinator_acquire(
        ARTWORK_PRIO_PLAYER, ALBUM_ART_METADATA_START_BYTES, 300, cancel_cb, user_data);
    if (admission != ARTWORK_ACQUIRE_OK) return false; /* transient -- coordinator busy/cancelled */
    track_metadata_t meta;
    memset(&meta, 0, sizeof(meta));
    metadata_artwork_result_t artwork_result = metadata_read_artwork_isolated(
        track_path, &meta, ALBUM_ART_METADATA_TIMEOUT_MS, ARTWORK_PRIO_PLAYER);
    artwork_coordinator_release(ARTWORK_PRIO_PLAYER);
    uint8_t * embedded_data = meta.picture_data;
    uint32_t embedded_size = meta.picture_size;
    free(meta.lyrics);
    if (!info.artist[0]) snprintf(info.artist, sizeof(info.artist), "%s", meta.artist);
    if (!info.album[0]) snprintf(info.album, sizeof(info.album), "%s", meta.album);
    if (!info.albumartist[0]) snprintf(info.albumartist, sizeof(info.albumartist), "%s", meta.album_artist);

    if (!embedded_data || embedded_size == 0) {
        free(embedded_data);
        /* Confirmed empty only if the embedded-picture helper actually ran
         * to completion and found nothing (or found something invalid) --
         * not if it merely failed to run (fork/pipe/timeout). Reaching here
         * already means embedded extraction had its chance regardless of
         * whether a sidecar file existed or how it failed. */
        if (out_no_art_confirmed && (artwork_result == METADATA_ARTWORK_NOT_FOUND ||
                                     artwork_result == METADATA_ARTWORK_INVALID))
            *out_no_art_confirmed = true;
        return false;
    }

    DB_LOG("ART_PLAYER", "generate_source path=%s source=embedded", track_path);
    cover_decode_result_t res = generate_player_cover_from_data(
        &info, embedded_data, embedded_size, cancel_cb, user_data, out_pixels);
    free(embedded_data);
    if (res != COVER_DECODE_OK && out_no_art_confirmed && cover_decode_result_is_permanent(res))
        *out_no_art_confirmed = true; /* corrupt/oversized source -- won't change until re-tagged */
    return res == COVER_DECODE_OK;
}

/* Rockbox albumart search, then embedded picture. A successful decode is
 * written as MUSIC_ROOT_DIR/.open_hiby_player/albumart/<artist>-<album>.72x72.bmp
 * so the next pass is a small BMP load instead of a JPEG/PNG decode.
 * Albums without artist+album tags still decode, but cannot be stored.
 * Checks the negative failure cache first to prevent repeated failed decodes.
 * If sized thumbnail or sidecar is invalid/unreadable, falls through to embedded art. */
static bool album_thumbnail_load_or_decode_ex(const song_row_t * song, artwork_priority_t prio,
                                              artwork_cancel_fn cancel_cb, void * user_data,
                                              uint16_t ** out_pixels) {
    *out_pixels = NULL;
    if (!song || !song->path[0]) return false;

    albumart_info_t info;
    albumart_info_from_song_row(song, &info);

    uint64_t mtime_t0 = db_log_now_ms();
    time_t source_mtime = album_source_mtime(song, &info);
    DB_LOG("ART_LAZY", "source_mtime_ms=%llu song=%lld", (unsigned long long) (db_log_now_ms() - mtime_t0),
           (long long) song->id);

    artwork_fail_reason_t fail_reason = ARTWORK_FAIL_NONE;
    if (artwork_failure_cache_is_blocked(song->id, source_mtime, &fail_reason)) return false;

    char found[PATH_MAX];
    char thumbnail_found[PATH_MAX];
    uint8_t * data = NULL;
    uint32_t size = 0;

    /* Step 1: Try sized Rockbox thumbnail cache (.72x72.bmp) */
    bool step1_hit = album_thumbnail_sized_cache_hit(&info, thumbnail_found, sizeof(thumbnail_found));
    DB_LOG("ART_LAZY", "step1 song=%lld artist=%s albumartist=%s album=%s key=%016llx hit=%d found=%s",
           (long long) song->id, info.artist, info.albumartist, info.album,
           (unsigned long long) albumart_debug_thumbnail_key(&info), step1_hit,
           step1_hit ? thumbnail_found : "");
    if (step1_hit) {
        albumart_load_result_t load = albumart_load_file_ex(thumbnail_found, &data, &size, THUMBNAIL_SIDECAR_MAX_BYTES, prio);
        if (load == ALBUMART_LOAD_TEMPORARY) {
            artwork_failure_cache_record(song->id, source_mtime, ARTWORK_FAIL_TEMPORARY);
            return false; /* Do not delete a valid cache file under memory pressure. */
        }
        if (load == ALBUMART_LOAD_OK) {
            cover_decode_result_t res = cover_decode_to_rgb565_ex(data, size, ALBUM_THUMBNAIL_PX, ALBUM_THUMBNAIL_PX,
                                                                  prio, cancel_cb, user_data, out_pixels);
            free(data);
            data = NULL;
            size = 0;
            if (res == COVER_DECODE_OK && *out_pixels) {
                /* A warmer pass must continue to the original source when
                 * only the 72px cache exists, so it can materialize 480px.
                 * Visible thumbnail requests can return immediately. */
                if (prio != ARTWORK_PRIO_WARMER || album_player_cache_hit(&info, found, sizeof(found)))
                    return true;
                /* Keep the valid 72px cache intact while the warmer obtains
                 * the original source for the missing player-sized cache. */
                free(*out_pixels);
                *out_pixels = NULL;
            }
            if (res == COVER_DECODE_FAIL_CANCELLED) return false;
            if (cover_decode_result_is_temporary(res)) {
                artwork_failure_cache_record(song->id, source_mtime, ARTWORK_FAIL_TEMPORARY);
                return false;
            }
            /* Corrupt sized thumbnail -> unlink and fall through to source files */
            if (res != COVER_DECODE_OK) unlink(thumbnail_found);
        } else {
            unlink(thumbnail_found);
        }
    }

    /* Step 2: Try external sidecar file (cover.jpg, folder.jpg, etc.) */
    bool sidecar_searched = albumart_search_files(&info, "", found, sizeof(found));
    if (sidecar_searched) {
        albumart_load_result_t load = albumart_load_file_ex(found, &data, &size, THUMBNAIL_SIDECAR_MAX_BYTES, prio);
        if (load == ALBUMART_LOAD_TEMPORARY) {
            artwork_failure_cache_record(song->id, source_mtime, ARTWORK_FAIL_TEMPORARY);
            return false;
        }
        if (load == ALBUMART_LOAD_OK) {
            uint16_t * player_pixels = NULL;
            cover_decode_result_t player_res = album_thumbnail_maybe_store_player_cache(
                &info, data, size, prio, cancel_cb, user_data, &player_pixels);
            if (player_res == COVER_DECODE_FAIL_CANCELLED) {
                free(player_pixels);
                free(data);
                return false;
            }
            /* Any OTHER temporary player-cache failure (memory admission,
             * coordinator busy) must not abort the whole attempt --
             * player_pixels is already NULL on any player_res failure (see
             * album_thumbnail_maybe_store_player_cache()'s own contract), so
             * falling through to the much smaller thumbnail-only decode
             * below costs nothing extra and can very plausibly still
             * succeed under the same pressure that blocked the larger 480px
             * attempt. Only the outcome of THAT decode (checked below) gets
             * recorded/returned. */
            cover_decode_result_t res = album_thumbnail_pixels_from_source(
                data, size, player_pixels, prio, cancel_cb, user_data, out_pixels);
            free(player_pixels);
            free(data);
            data = NULL;
            size = 0;
            if (res == COVER_DECODE_OK && *out_pixels) {
                albumart_store_rgb565(&info, ALBUM_THUMBNAIL_PX, ALBUM_THUMBNAIL_PX, *out_pixels);
                return true;
            }
            if (res == COVER_DECODE_FAIL_CANCELLED) return false;
            if (cover_decode_result_is_temporary(res)) {
                artwork_failure_cache_record(song->id, source_mtime, ARTWORK_FAIL_TEMPORARY);
                return false;
            }
            /* Corrupt or oversized sidecar falls through to embedded art */
        }
    }

    /* Step 3: Try embedded picture from audio file */
    /* Serialize extraction with decoding too. The helper bounds parser
     * allocations, and the parent admits the actual returned picture size. */
    artwork_acquire_result_t admission = artwork_coordinator_acquire(
        prio, ALBUM_ART_METADATA_START_BYTES, 300, cancel_cb, user_data);
    if (admission == ARTWORK_ACQUIRE_CANCELLED || admission == ARTWORK_ACQUIRE_SUSPENDED) return false;
    if (admission != ARTWORK_ACQUIRE_OK) {
        artwork_failure_cache_record(song->id, source_mtime, ARTWORK_FAIL_TEMPORARY);
        return false;
    }
    track_metadata_t meta;
    memset(&meta, 0, sizeof(meta));
    metadata_artwork_result_t metadata_result =
        metadata_read_artwork_isolated(song->path, &meta, ALBUM_ART_METADATA_TIMEOUT_MS, prio);
    artwork_coordinator_release(prio);
    data = meta.picture_data;
    size = meta.picture_size;
    free(meta.lyrics);
    if (!info.artist[0]) snprintf(info.artist, sizeof(info.artist), "%s", meta.artist);
    if (!info.album[0]) snprintf(info.album, sizeof(info.album), "%s", meta.album);
    if (!info.albumartist[0]) snprintf(info.albumartist, sizeof(info.albumartist), "%s", meta.album_artist);

    if (data && size > 0) {
        uint16_t * player_pixels = NULL;
        cover_decode_result_t player_res = album_thumbnail_maybe_store_player_cache(
            &info, data, size, prio, cancel_cb, user_data, &player_pixels);
        if (player_res == COVER_DECODE_FAIL_CANCELLED) {
            free(player_pixels);
            free(data);
            return false;
        }
        /* See the mirrored sidecar-path comment above (Step 2) -- any OTHER
         * temporary player-cache failure falls through to the smaller
         * thumbnail-only decode instead of aborting immediately. */
        cover_decode_result_t res = album_thumbnail_pixels_from_source(
            data, size, player_pixels, prio, cancel_cb, user_data, out_pixels);
        free(player_pixels);
        free(data);
        data = NULL;
        size = 0;
        if (res == COVER_DECODE_OK && *out_pixels) {
            albumart_store_rgb565(&info, ALBUM_THUMBNAIL_PX, ALBUM_THUMBNAIL_PX, *out_pixels);
            return true;
        }
        if (res == COVER_DECODE_FAIL_CANCELLED) return false;
        if (cover_decode_result_is_temporary(res)) {
            artwork_failure_cache_record(song->id, source_mtime, ARTWORK_FAIL_TEMPORARY);
            return false;
        }
    }

    if (metadata_result == METADATA_ARTWORK_TEMPORARY_FAILURE) {
        artwork_failure_cache_record(song->id, source_mtime, ARTWORK_FAIL_TEMPORARY);
        return false;
    }

    /* Step 4: No valid artwork found or permanent decode failure */
    artwork_failure_cache_record(song->id, source_mtime, ARTWORK_FAIL_PERMANENT);
    return false;
}

static bool album_thumbnail_load_or_decode(const song_row_t * song, int generation, uint16_t ** out_pixels) {
    return album_thumbnail_load_or_decode_ex(song, ARTWORK_PRIO_THUMBNAIL,
                                            album_thumbnail_cancel_cb, (void *) (intptr_t) generation,
                                            out_pixels);
}

static void * album_thumbnail_thread_func(void * arg) {
    install_thread_crash_altstack(); /* see its own comment (main.c) */
#ifdef UI_PERF_TRACE
    uint64_t perf_start_us = ui_perf_now_us();
#endif
    album_thumbnail_request_t * req = (album_thumbnail_request_t *) arg;
    /* Cached once -- see library_scan_once()'s own comment on why an
     * unconditional db_log_now_ms() call here would cost a real syscall on
     * every lazy-loaded thumbnail even with logging disabled. */
    bool db_logging = db_log_enabled();
    uint64_t started_ms = db_logging ? db_log_now_ms() : 0;
    if (db_logging)
        DB_LOG("ART_LAZY", "decode_begin song=%lld row=%d generation=%d rss_kb=%ld",
               (long long) req->song_id, req->logical_index, req->generation, db_log_rss_kb());
    uint16_t * pixels = NULL;
    song_row_t song;
    album_thumbnail_result_have_source_mtime = false;
    if (metadata_db_get_song_by_id(req->song_id, &song)) {
        album_thumbnail_load_or_decode(&song, req->generation, &pixels);
        if (!pixels) {
            /* Only needed to look up the failure this decode already
             * recorded (see result_source_mtime's own comment) -- skip the
             * work entirely on success. */
            albumart_info_t info;
            albumart_info_from_song_row(&song, &info);
            album_thumbnail_result_source_mtime = album_source_mtime(&song, &info);
            album_thumbnail_result_have_source_mtime = true;
        }
    }
    album_thumbnail_result_song_id = req->song_id;
    album_thumbnail_result_generation = req->generation;
    album_thumbnail_result_logical_index = req->logical_index;
    album_thumbnail_result_list = req->list;
    album_thumbnail_result_pixels = (uint8_t *) pixels;
    free(req);
    album_thumbnail_done = true;
    if (db_logging)
        DB_LOG("ART_LAZY", "decode_end song=%lld row=%d art=%d elapsed_ms=%llu rss_kb=%ld",
               (long long) album_thumbnail_result_song_id, album_thumbnail_result_logical_index,
               album_thumbnail_result_pixels != NULL,
               (unsigned long long) (db_log_now_ms() - started_ms), db_log_rss_kb());
#ifdef UI_PERF_TRACE
    printf("PERF album_thumb song=%lld total_us=%llu pixels=%d\n",
           (long long) album_thumbnail_result_song_id,
           (unsigned long long) (ui_perf_now_us() - perf_start_us),
           album_thumbnail_result_pixels != NULL);
#endif
    return NULL;
}

/* ---- Persistent (post-scan) album thumbnail generation ----------------
 * Runs once after Update Music Database completes, writing sized BMP files
 * in short bounded batches with generous cooperative delays so CPU and SD
 * activity never cause overheating or starve audio playback. Suspends
 * automatically when audio is playing, when Albums screen is active, or
 * when memory is low. */
#define ALBUM_THUMB_GEN_BATCH 16
#define ALBUM_THUMB_GEN_INTER_ALBUM_US 100000 /* 100 ms yield between albums */
#define ALBUM_THUMB_GEN_INTER_BATCH_US 1000000 /* 1.0 s pause between batches */

static void * album_thumb_gen_thread_func(void * arg) {
    install_thread_crash_altstack(); /* see its own comment (main.c) */
    int my_generation = (int) (intptr_t) arg;
    uint64_t started_ms = db_log_enabled() ? db_log_now_ms() : 0;
    int generated = 0, cached = 0, missing = 0, failed = 0;
    DB_LOG("ART_CACHE", "worker_begin generation=%d total=%d rss_kb=%ld", my_generation,
           atomic_load(&album_thumb_gen_total_count), db_log_rss_kb());
#ifdef UI_PERF_TRACE
    uint64_t perf_start_us = ui_perf_now_us();
    int perf_generated = 0, perf_skipped = 0, perf_missing = 0, perf_failed = 0;
#endif
    atomic_store(&album_thumb_gen_done_count, 0);

    group_row_t rows[ALBUM_THUMB_GEN_BATCH];
    int offset = 0;
    for (;;) {
        if (album_thumb_gen_should_cancel(my_generation)) break;

        /* Suspend warmer while audio is playing, albums screen is actively open, or memory is low */
        if (audio_is_playing() || atomic_load(&album_thumbnail_screen_active) ||
            !artwork_check_memory_admission(ARTWORK_PRIO_WARMER, ALBUM_ART_METADATA_START_BYTES)) {
            atomic_store(&album_thumb_gen_retry_pending, true);
            goto done;
        }

        int n = metadata_db_get_albums_page_filtered(NULL, offset, ALBUM_THUMB_GEN_BATCH, rows);
        if (n <= 0) break;
        for (int i = 0; i < n; i++) {
            if (album_thumb_gen_should_cancel(my_generation)) goto done;

            /* Check suspension before each album */
            if (audio_is_playing() || atomic_load(&album_thumbnail_screen_active)) {
                atomic_store(&album_thumb_gen_retry_pending, true);
                goto done;
            }

            song_row_t song;
            if (!metadata_db_get_song_by_id(rows[i].first_song_id, &song)) {
                missing++;
#ifdef UI_PERF_TRACE
                perf_missing++;
#endif
                atomic_fetch_add(&album_thumb_gen_done_count, 1);
                continue;
            }

            albumart_info_t info;
            albumart_info_from_song_row(&song, &info);

            time_t source_mtime = album_source_mtime(&song, &info);

            artwork_fail_reason_t fail_reason;
            if (artwork_failure_cache_is_blocked(song.id, source_mtime, &fail_reason)) {
                if (fail_reason == ARTWORK_FAIL_TEMPORARY)
                    atomic_store(&album_thumb_gen_retry_pending, true);
                failed++;
#ifdef UI_PERF_TRACE
                perf_skipped++;
#endif
                atomic_fetch_add(&album_thumb_gen_done_count, 1);
                continue;
            }

            char found[PATH_MAX];
            if (album_thumbnail_sized_cache_hit(&info, found, sizeof(found)) &&
                album_player_cache_hit(&info, found, sizeof(found))) {
                cached++;
#ifdef UI_PERF_TRACE
                perf_skipped++;
#endif
                atomic_fetch_add(&album_thumb_gen_done_count, 1);
                continue;
            }

            uint16_t * pixels = NULL;
            album_thumbnail_load_or_decode_ex(&song, ARTWORK_PRIO_WARMER,
                                              album_thumb_gen_cancel_cb, (void *) (intptr_t) my_generation,
                                              &pixels);
            if (!pixels && (audio_is_playing() || album_thumb_gen_should_cancel(my_generation) ||
                (artwork_failure_cache_is_blocked(song.id, source_mtime, &fail_reason) &&
                 fail_reason == ARTWORK_FAIL_TEMPORARY)))
                atomic_store(&album_thumb_gen_retry_pending, true);
            if (pixels) generated++; else failed++;

#ifdef UI_PERF_TRACE
            if (pixels) perf_generated++; else perf_failed++;
#endif
            free(pixels);
            atomic_fetch_add(&album_thumb_gen_done_count, 1);

            int diag_done = atomic_load(&album_thumb_gen_done_count);
            if ((diag_done % 50) == 0) {
                DB_LOG("ART_CACHE", "progress done=%d total=%d generated=%d cached=%d missing=%d failed=%d elapsed_ms=%llu rss_kb=%ld",
                       diag_done, atomic_load(&album_thumb_gen_total_count), generated, cached, missing, failed,
                       (unsigned long long) (db_log_now_ms() - started_ms), db_log_rss_kb());
            }

            /* Yield between albums to avoid heating up CPU */
            usleep(ALBUM_THUMB_GEN_INTER_ALBUM_US);
        }
        offset += n;
        if (n < ALBUM_THUMB_GEN_BATCH) break;

        /* Pause between batches to give CPU/SD bus complete rest */
        for (int p = 0; p < 10; p++) {
            if (album_thumb_gen_should_cancel(my_generation)) goto done;
            usleep(ALBUM_THUMB_GEN_INTER_BATCH_US / 10);
        }
    }
done:
    DB_LOG("ART_CACHE", "worker_end generation=%d done=%d total=%d generated=%d cached=%d missing=%d failed=%d cancelled=%d elapsed_ms=%llu rss_kb=%ld",
           my_generation, atomic_load(&album_thumb_gen_done_count), atomic_load(&album_thumb_gen_total_count),
           generated, cached, missing, failed, (int) album_thumb_gen_should_cancel(my_generation),
           (unsigned long long) (db_log_now_ms() - started_ms), db_log_rss_kb());
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
/* 4MB stack size configured for thumbnail generation thread to accommodate
 * stack usage of JPEG and PNG cover decoders. */
#define ALBUM_COVER_DECODE_THREAD_STACK_SIZE (4 * 1024 * 1024)

static void start_album_thumbnail_generation(void) {
    cancel_album_thumbnail_generation();
    reap_album_thumbnail_generation();
    atomic_store(&album_thumb_gen_retry_pending, false);
    album_thumb_gen_retry_tick = lv_tick_get();

    int artist_count = 0, album_artist_count = 0, album_count = 0;
    metadata_db_get_group_counts(&artist_count, &album_artist_count, &album_count);
    atomic_store(&album_thumb_gen_done_count, 0);
    atomic_store(&album_thumb_gen_total_count, album_count);
    atomic_store(&album_thumb_gen_cancel, false);
    int generation = atomic_fetch_add(&album_thumb_gen_generation, 1) + 1;
    atomic_store(&album_thumb_gen_active, true);
    DB_LOG("ART_CACHE", "start generation=%d albums=%d rss_kb=%ld",
           generation, album_count, db_log_rss_kb());

    pthread_attr_t attr;
    pthread_attr_t * attr_ptr = NULL;
    if (pthread_attr_init(&attr) == 0) {
        if (pthread_attr_setstacksize(&attr, ALBUM_COVER_DECODE_THREAD_STACK_SIZE) == 0) attr_ptr = &attr;
    }
    bool created = pthread_create(&album_thumb_gen_thread, attr_ptr, album_thumb_gen_thread_func,
                                   (void *) (intptr_t) generation) == 0;
    if (attr_ptr) pthread_attr_destroy(&attr);
    if (!created) {
        atomic_store(&album_thumb_gen_active, false);
        DB_LOG("ART_CACHE", "thread_create_failed generation=%d rss_kb=%ld", generation, db_log_rss_kb());
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
    album_lazy_job_started_ms = db_log_enabled() ? db_log_now_ms() : 0;
    /* See start_album_thumbnail_generation()'s own ALBUM_COVER_DECODE_THREAD_
     * STACK_SIZE comment -- same full JPEG/PNG cover decode, same fix. */
    pthread_attr_t attr;
    pthread_attr_t * attr_ptr = NULL;
    if (pthread_attr_init(&attr) == 0) {
        if (pthread_attr_setstacksize(&attr, ALBUM_COVER_DECODE_THREAD_STACK_SIZE) == 0) attr_ptr = &attr;
    }
    bool created = pthread_create(&album_thumbnail_thread, attr_ptr, album_thumbnail_thread_func, req) == 0;
    if (attr_ptr) pthread_attr_destroy(&attr);
    if (!created) {
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
    album_lazy_queued++;
    DB_LOG("ART_LAZY", "queued song=%lld row=%d queue_depth=%d generation=%d",
           (long long) song_id, logical_index, album_thumbnail_queue_count, album_thumbnail_generation);
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
    if (atomic_load(&album_thumb_gen_active))
        atomic_store(&album_thumb_gen_retry_pending, true);
    cancel_album_thumbnail_generation();
    atomic_store(&album_thumbnail_screen_active, true);
    album_thumbnail_active_list = list;
    album_thumbnail_scrolling = false;
    album_thumbnail_queue_count = 0;
    album_lazy_queued = album_lazy_completed = album_lazy_with_art = album_lazy_stale = 0;
    const char * diag_page = list == albums_list ? "albums" : "artist_albums";
    DB_LOG("ALBUMS_PAGE", "loaded page=%s request_to_loaded_ms=%llu rss_kb=%ld", diag_page,
           albums_page_open_requested_ms ? (unsigned long long) (db_log_now_ms() - albums_page_open_requested_ms) : 0ULL,
           db_log_rss_kb());
    albums_page_open_requested_ms = 0;
    if (list) compact_list_refresh_visible(list);
}

static void album_thumbnail_end_screen(lv_obj_t * list) {
    if (album_thumbnail_active_list != list) return;
    atomic_store(&album_thumbnail_screen_active, false);
    album_thumbnail_active_list = NULL;
    album_thumbnail_scrolling = false;
    album_thumbnail_queue_count = 0;
    DB_LOG("ALBUMS_PAGE", "unloaded page=%s queued=%u completed=%u with_art=%u stale=%u rss_kb=%ld",
           list == albums_list ? "albums" : "artist_albums", album_lazy_queued, album_lazy_completed,
           album_lazy_with_art, album_lazy_stale, db_log_rss_kb());
    /* Codec work cannot safely be cancelled. Invalidate and discard it
     * when it completes rather than ever repainting a hidden screen. */
    album_thumbnail_generation++;
}

static void album_thumbnail_poll_cb(lv_timer_t * timer) {
    /* Same reasoning as compact_list_poll_fetch_cb()'s own comment
     * (screen_builders.c): this timer's row-image/cache-eviction work
     * landing in the same tick as a live gesture's frame-present call is
     * exactly the kind of variable-cost interference that produces uneven
     * frame pacing mid-animation. Deferring costs nothing -- a landed
     * decode just sits ready a tick or two longer, and this timer keeps
     * rescheduling itself regardless. */
    if (gui_navigation_transition_in_progress()) return;
    if (!album_thumbnail_active) {
        start_next_album_thumbnail();
        if (!album_thumbnail_active && !atomic_load(&album_thumb_gen_active)) lv_timer_pause(timer);
        return;
    }
    if (!atomic_load(&album_thumbnail_done)) return;
    pthread_join(album_thumbnail_thread, NULL);
    /* Keep the slot busy until all result fields are consumed. Row refreshes
     * below can synchronously queue work and try to start the next worker. */

    bool result_applied = false;
    bool result_had_art = album_thumbnail_result_pixels != NULL;
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
        /* A failed decode (pixels == NULL) is only worth caching as a durable
         * "known, no art" entry if album_thumbnail_load_or_decode_ex() itself
         * already recorded it as PERMANENT (corrupt/oversized/genuinely
         * absent source). A TEMPORARY failure (memory admission, coordinator
         * busy, a cancelled/preempted decode) must NOT be cached here the
         * same way -- album_thumbnail_cache_find()'s only gate is `known`,
         * so caching it at all previously made a transient hiccup permanent
         * for this song for the rest of this cache slot's lifetime, with no
         * retry until unrelated LRU pressure happened to evict it. Leaving
         * the slot at its just-memset "unused" state (known=false) instead
         * lets queue_album_thumbnail() legitimately retry this song next
         * time it scrolls back into view. */
        bool commit_known = album_thumbnail_result_pixels != NULL;
        if (!commit_known && album_thumbnail_result_have_source_mtime) {
            artwork_fail_reason_t fail_reason = ARTWORK_FAIL_NONE;
            if (artwork_failure_cache_is_blocked(album_thumbnail_result_song_id,
                                                 album_thumbnail_result_source_mtime, &fail_reason) &&
                fail_reason == ARTWORK_FAIL_PERMANENT) {
                commit_known = true;
            }
        }
        if (commit_known) {
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
        }
        album_thumbnail_result_pixels = NULL;
        result_applied = true;
    }
    album_lazy_completed++;
    if (result_had_art) album_lazy_with_art++;
    if (!result_applied) album_lazy_stale++;
    DB_LOG("ART_LAZY", "result song=%lld row=%d applied=%d art=%d queue_depth=%d ui_wait_ms=%llu",
           (long long) album_thumbnail_result_song_id, album_thumbnail_result_logical_index,
           result_applied, result_had_art, album_thumbnail_queue_count,
           album_lazy_job_started_ms ? (unsigned long long) (db_log_now_ms() - album_lazy_job_started_ms) : 0ULL);
    free(album_thumbnail_result_pixels);
    album_thumbnail_result_pixels = NULL;
    if (result_applied && album_thumbnail_result_list == album_thumbnail_active_list)
        compact_list_refresh_item(album_thumbnail_result_list, album_thumbnail_result_logical_index);
    album_thumbnail_active = false;
    start_next_album_thumbnail();
    if (!album_thumbnail_active) lv_timer_pause(timer);
}

/* UI reloads and database replacement both invalidate every list pointer
 * carried by the lazy artwork request. Stop the warmer first, then join the
 * single visible-row decoder before any screen or DB generation is torn
 * down. Both paths are cooperatively bounded by the artwork timeout. */
static void quiesce_album_artwork_workers(void) {
    cancel_album_thumbnail_generation();
    reap_album_thumbnail_generation();
    atomic_store(&album_thumb_gen_retry_pending, false);

    album_thumbnail_generation++;
    album_thumbnail_queue_count = 0;
    album_thumbnail_active_list = NULL;
    atomic_store(&album_thumbnail_screen_active, false);

    if (album_thumbnail_active) {
        pthread_join(album_thumbnail_thread, NULL);
        album_thumbnail_active = false;
    }
    atomic_store(&album_thumbnail_done, false);
    free(album_thumbnail_result_pixels);
    album_thumbnail_result_pixels = NULL;
    album_thumbnail_result_list = NULL;
    if (album_thumbnail_poll_timer) lv_timer_pause(album_thumbnail_poll_timer);
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
        snprintf(out_rows[i].trailing_asset, sizeof(out_rows[i].trailing_asset),
                 "%s", "playing_plane/ic_more.png");
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

static group_song_entry_t * load_album_entries(const char * name, const char * album_artist,
                                                int song_count, int * out_count) {
    *out_count = 0;
    if (song_count <= 0) return NULL;
    group_song_entry_t * entries = calloc((size_t) song_count, sizeof(*entries));
    if (!entries) return NULL;
    song_row_t page[64];
    int n = 0;
    while (n < song_count) {
        int want = song_count - n;
        if (want > 64) want = 64;
        int got = metadata_db_get_album_songs(name, album_artist, n, page, want);
        if (got <= 0) break;
        for (int i = 0; i < got; i++) {
            char title[384];
            format_music_submenu_identity(&page[i], title, sizeof(title));
            entries[n + i].path = strdup(page[i].path);
            entries[n + i].title = strdup(title);
            if (!entries[n + i].path || !entries[n + i].title) {
                free_group_song_entries(entries, song_count);
                return NULL;
            }
        }
        n += got;
        if (got < want) break;
    }
    if (n <= 0) {
        free_group_song_entries(entries, song_count);
        return NULL;
    }
    *out_count = n;
    return entries;
}

static bool show_album_group(const group_row_t * group) {
    int count = 0;
    group_song_entry_t * entries = load_album_entries(group->name, group->album_artist,
                                                       group->song_count, &count);
    if (!entries) return false;
    show_music_group_songs(group->name, entries, count);
    free_group_song_entries(entries, count);
    group_songs_source_is_album = true;
    return true;
}

static void album_row_click_cb(int index) {
    index = search_remap_index(SEARCH_BINDING_ALBUMS, index);

    /* Resolve this specific (album, album_artist) pair at its current
     * display position via a single-row offset lookup (same pattern as All
     * Songs' own row-click resolution) -- disambiguated by album_artist too,
     * so two different artists sharing an album title never collide. */
    group_row_t group;
    if (metadata_db_get_albums_page_filtered(NULL, index, 1, &group) != 1) return;
    (void) show_album_group(&group);
}

/* Builds the Artists screen using a paged provider
 * (compact_list_set_paged_provider()) to load artist groups incrementally. */
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
    compact_list_set_trailing_click(albums_list, album_more_click_cb);
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
static lv_timer_t * az_index_visibility_timer;
static az_index_binding_t * az_index_visibility_binding;
static bool az_index_dragging;

#define AZ_INDEX_HIDE_DELAY_MS 900

static void az_index_visibility_timeout_cb(lv_timer_t * timer) {
    (void) timer;
    if (az_index_dragging) {
        lv_timer_reset(az_index_visibility_timer);
        return;
    }
    if (az_index_visibility_binding && az_index_visibility_binding->strip)
        lv_obj_add_flag(az_index_visibility_binding->strip, LV_OBJ_FLAG_HIDDEN);
    az_index_visibility_binding = NULL;
    lv_timer_pause(az_index_visibility_timer);
}

static void az_index_scroll_visibility_cb(lv_event_t * e) {
    lv_obj_t * list = lv_event_get_target(e);
    az_index_binding_t * binding = NULL;
    for (int i = 0; i < az_index_registered_count; ++i) {
        if (az_index_bindings[i].list == list) { binding = &az_index_bindings[i]; break; }
    }
    if (!binding) return;
    if (az_index_visibility_binding && az_index_visibility_binding != binding)
        lv_obj_add_flag(az_index_visibility_binding->strip, LV_OBJ_FLAG_HIDDEN);
    az_index_visibility_binding = binding;
    lv_obj_remove_flag(binding->strip, LV_OBJ_FLAG_HIDDEN);
    if (!az_index_visibility_timer) {
        az_index_visibility_timer = lv_timer_create(az_index_visibility_timeout_cb,
                                                     AZ_INDEX_HIDE_DELAY_MS, NULL);
    }
    lv_timer_set_period(az_index_visibility_timer, AZ_INDEX_HIDE_DELAY_MS);
    lv_timer_reset(az_index_visibility_timer);
    lv_timer_resume(az_index_visibility_timer);
}

static void reset_az_index_bindings(void) {
    if (az_index_visibility_timer) {
        lv_timer_delete(az_index_visibility_timer);
        az_index_visibility_timer = NULL;
    }
    az_index_visibility_binding = NULL;
    az_index_registered_count = 0;
}


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
    int32_t top = STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT;
    int32_t display_h = lv_display_get_vertical_resolution(lv_display_get_default());
    int32_t available_h = display_h - top - HOME_INDICATOR_BAND_HEIGHT;
    const lv_font_t * strip_font = &lv_font_montserrat_20;
    int32_t line_h = lv_font_get_line_height(strip_font);
    lv_obj_set_style_text_font(strip, strip_font, 0);
    lv_obj_set_width(strip, BOARD_SCALE_PX(30));
    lv_obj_set_style_pad_right(strip, BOARD_SCALE_PX(4), 0);
    lv_obj_add_style(strip, &style_theme_text_primary, 0);
    lv_obj_set_style_text_align(strip, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_bg_opa(strip, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(strip, 0, 0);
    lv_obj_remove_flag(strip, LV_OBJ_FLAG_SCROLLABLE);
    /* Stretched with extra line spacing to span close to the full list
     * height (27 lines * app_font_16's own line height) rather than
     * sitting bunched up near the top. */
    int32_t line_space = (available_h - line_h * 27) / 26;
    /* Negative line spacing is intentional on shorter panels: retaining
     * readable 20px glyphs is preferable to falling back to the old tiny
     * font, and the evenly compressed 27-line column still maps touches by
     * its final measured bounds. */
    if (line_space < -6) line_space = -6;
    if (line_space > 3) line_space = 3;
    lv_obj_set_style_text_line_space(strip, line_space, 0);
    /* Top edge (the "A") lines up with the list's own top edge; the
     * stretched height above lands the bottom ("#") close to the screen's
     * bottom corner, matching the list's own bottom edge -- the list
     * itself starts at exactly STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT and
     * runs flush to the screen bottom (see build_compact_list_screen()). */
    lv_obj_align(strip, LV_ALIGN_TOP_RIGHT, 0, top);
    lv_obj_add_flag(strip, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t * popup = lv_image_create(screen);
    lv_image_set_src(popup, asset_path("touch_list/a_z_result_bg.png"));
    lv_obj_center(popup);
    lv_obj_add_flag(popup, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t * popup_label = lv_label_create(popup);
    lv_obj_set_style_text_font(popup_label, &app_font_28, 0);
    lv_obj_add_style(popup_label, &style_theme_text_primary, 0);
    lv_obj_center(popup_label);

    az_index_bindings[az_index_registered_count++] = (az_index_binding_t){ screen, list, strip, popup, popup_label, db_kind };
    lv_obj_add_event_cb(list, az_index_scroll_visibility_cb, LV_EVENT_SCROLL_BEGIN, NULL);
    lv_obj_add_event_cb(list, az_index_scroll_visibility_cb, LV_EVENT_SCROLL, NULL);
    lv_obj_add_event_cb(list, az_index_scroll_visibility_cb, LV_EVENT_SCROLL_END, NULL);
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
        if (lv_obj_has_flag(b->strip, LV_OBJ_FLAG_HIDDEN)) return;

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
 * Debounces keystrokes via search_debounce_timer and offloads the database
 * search query to a worker pthread, keeping the UI responsive. In-memory
 * bindings search synchronously in search_apply_filter(). */
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
 * finished search job's hits and applies them to b->list. */
static void search_apply_results_to_list(search_binding_t * b, const metadata_db_search_hit_t * hits, int matched) {
    compact_list_item_t * items = malloc(sizeof(compact_list_item_t) * (size_t) (matched > 0 ? matched : 1));
    int * indices = malloc(sizeof(int) * (size_t) (matched > 0 ? matched : 1));
    char(*labels)[128] = malloc(sizeof(*labels) * (size_t) (matched > 0 ? matched : 1));
    if (!items || !indices || !labels) {
        free(items);
        free(indices);
        free(labels);
        return;
    }
    for (int i = 0; i < matched; i++) {
        snprintf(labels[i], sizeof(labels[i]), "%.127s", hits[i].label);
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

/* Rebuilds binding->list contents to entries matching `query` via
 * compact_list_set_items(). An empty query clears the list.
 * `filtered_indices` maps display rows back to unfiltered indices for
 * search_remap_index(), capped at SEARCH_RESULTS_MAX matches.
 *
 * db_backed bindings query metadata_db_search_names() via a background
 * worker (launch_search_job()). In-memory bindings are filtered synchronously. */
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
         * static items[] array. */
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

/* screen_gesture_event_cb()'s back-swipe hook for the Files screen: steps up
 * one directory instead of popping the screen, unless already at root. */
bool file_browser_back_if_not_root_for_screen(lv_obj_t * screen) {
    if (screen != files_screen || file_browser_at_root()) return false;
    file_browser_go_up();
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
 * right of the title row) and the search bar (hidden until search_btn is
 * tapped). */
void register_search(search_binding_id_t id, lv_obj_t * screen, lv_obj_t * list, az_index_name_of_t name_of,
                             const int * count_ptr, bool is_overlay_list, bool db_backed, metadata_db_az_kind_t db_kind,
                             compact_list_fetch_page_cb_t restore_fetch_page) {
    search_binding_t * b = &search_bindings[id];
    free(b->filtered_indices); /* re-registering (post-rescan rebuild) over a binding left mid-filter would otherwise leak this */
    free(b->filtered_labels);

    lv_obj_t * search_btn = build_top_right_icon_button(screen, asset_path("sub_back/btn_search.png"), NULL);
    lv_obj_add_event_cb(search_btn, search_btn_click_cb, LV_EVENT_CLICKED, (void *) (intptr_t) id);
    lv_obj_add_flag(search_btn, LV_OBJ_FLAG_GESTURE_BUBBLE); /* added after finalize_screen_navigation()'s one-time pass, needs this set explicitly -- see screen_gesture_event_cb()'s own comment */

    /* Sized to bg_search.png native dimensions (440x80) and aligned to
     * cover the title row. Includes close button and text input. */
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

/* ---- Playlists (Music submenu) --
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
        char title[384];
        if (metadata_db_get_song_by_path(paths[i], &row)) {
            format_song_identity(&row, title, sizeof(title));
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

/* Plays a song selected via the remote-control web UI within its intended
 * scope (album, playlist, or artist). Falls back to full-library queue if
 * no scope is provided or resolved. */
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
    if (recently_added_list) compact_list_set_now_playing(recently_added_list, recently_added_row >= 0 ? recently_added_row + 1 : -1);

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
    compact_list_set_paged_provider(recently_added_list, recently_added_fetch_page, NULL,
        (int) metadata_db_get_song_count() + 1);
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
    if (count && !entries) return;
    snprintf(playlist_m3u_name, sizeof(playlist_m3u_name), "%s", name);
    show_group_songs_editable(playlist_m3u_name, entries, count, m3u_path, false);
    free_group_song_entries(entries, count);
}

/* Remove-icon handler for an editable playlist's rows (see
 * populate_group_songs_rows() -- forward-declared near
 * group_songs_edit_m3u_path). Rewrites the underlying .m3u file, then
 * re-reads it back and redraws in place (no nav_push -- this stays on the
 * same group_songs_screen instance, just with the file's new contents)
 * rather than trusting the in-memory indices array to still match the file
 * after the rewrite. */
static void reload_edited_playlist(void) {
    char ** songs = NULL;
    int count = 0;
    if (!playlist_files_read(group_songs_edit_m3u_path, &songs, &count)) {
        show_error_toast("Cannot read playlist"); return;
    }
    group_song_entry_t * entries = count ? build_group_song_entries_from_paths(songs, count) : NULL;
    for (int i = 0; i < count; i++) free(songs[i]);
    free(songs);
    if (count && !entries) return;
    group_songs_file_stat_valid = stat(group_songs_edit_m3u_path, &group_songs_file_stat) == 0;
    set_group_songs_entries(entries, count);
    free_group_song_entries(entries, count);
    populate_group_songs_rows();
}

static void group_song_move_row_cb(lv_event_t * e) {
    int value = (int) (intptr_t) lv_event_get_user_data(e);
    int pos = value / 2;
    int to = pos + (value % 2 ? 1 : -1);
    if (!group_songs_edit_m3u_path || to < 0 || to >= group_songs_count) return;
    if (!group_playlist_unchanged()) { reload_edited_playlist(); show_info_toast("Playlist changed. Try again."); return; }
    if (!playlist_files_edit_entry(group_songs_edit_m3u_path, pos, to)) {
        show_error_toast("Cannot reorder playlist"); return;
    }
    reload_edited_playlist();
}

static void group_song_remove_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int pos = (int) (intptr_t) lv_event_get_user_data(e);
    if (!group_songs_edit_m3u_path || pos < 0 || pos >= group_songs_count) return;
    if (!group_playlist_unchanged()) { reload_edited_playlist(); show_info_toast("Playlist changed. Try again."); return; }
    if (!playlist_files_edit_entry(group_songs_edit_m3u_path, pos, -1)) {
        show_error_toast("Cannot remove entry"); return;
    }
    reload_edited_playlist();
    show_info_toast("Removed from playlist");
}

/* Suppresses the follow-up LV_EVENT_CLICKED event when a long press has
 * already opened the playlist context menu on a user-playlist row. */
static bool playlist_row_long_press_fired = false;

static void playlist_row_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (playlist_row_long_press_fired) { playlist_row_long_press_fired = false; return; }
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    if (index == 0) { show_favorites(); return; }
    if (index == 1) { show_most_played(); return; }
    if (index == 2) { open_queue_screen(); return; }
    if (index == 3) { open_recently_added_screen(); return; }
    int i = index - 4;
    if (i < 0 || i >= playlists_m3u_count) return;
    char ** songs = NULL;
    int count = 0;
    if (!playlist_files_read(playlists_m3u_paths[i], &songs, &count)) {
        show_error_toast("Playlist unavailable or unreadable"); return;
    }
    show_m3u_playlist(basename_of(playlists_m3u_paths[i]), playlists_m3u_paths[i], songs, count);
    for (int j = 0; j < count; j++) free(songs[j]);
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
    lv_obj_add_style(row, &pill_row_bg_style, 0);
    lv_obj_add_style(row, &list_row_pressed_style, LV_STATE_PRESSED);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * label = lv_label_create(row);
    lv_label_set_text(label, label_text);
    lv_obj_add_style(label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(label, &LIST_ROW_FONT, 0);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, LIST_ROW_LABEL_INSET, 0);
    configure_scrolling_row_label(label, LIST_ROW_WIDTH_WIDE - LIST_ROW_LABEL_INSET - BOARD_SCALE_PX(60));
    if (parent == playlists_list) {
        lv_obj_t * chevron = lv_label_create(row);
        lv_label_set_text(chevron, ">");
        lv_obj_add_style(chevron, &style_theme_text_muted, 0);
        lv_obj_set_style_text_font(chevron, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
        lv_obj_align(chevron, LV_ALIGN_RIGHT_MID, -GUI_TEXT_INSET, 0);
    }
    return row;
}

/* Long-press context menu (Rename/Delete) for a user-created .m3u row --
 * never wired onto the Favorites/Most Played/Queue/Recently Added rows (see
 * populate_playlists_screen() below), since none is backed by a real file
 * playlist_files_delete()/playlist_files_rename() could act on. */
static lv_obj_t * playlist_delete_popup, * playlist_delete_backdrop;
static lv_obj_t * playlist_context_menu_popup, * playlist_context_menu_backdrop;
static char playlist_action_path[PATH_MAX];

static void playlist_delete_cancel_cb(lv_event_t * e) {
    (void) e;
    if (playlist_delete_popup) lv_obj_add_flag(playlist_delete_popup, LV_OBJ_FLAG_HIDDEN);
    if (playlist_delete_backdrop) lv_obj_add_flag(playlist_delete_backdrop, LV_OBJ_FLAG_HIDDEN);
}

static void playlist_delete_confirm_cb(lv_event_t * e) {
    playlist_delete_cancel_cb(e);
    if (!playlist_files_delete(playlist_action_path)) {
        show_error_toast("Cannot delete playlist"); return;
    }
    metadata_db_playlist_delete_one(playlist_action_path);
    populate_playlists_screen();
    show_info_toast("Playlist deleted");
}

static void playlist_rename_done_cb(const char * name, void * data) {
    (void) data;
    char dest[PATH_MAX];
    if (!playlist_files_rename(playlist_action_path, name, dest, sizeof(dest))) {
        show_error_toast("Cannot rename: invalid name or file exists"); return;
    }
    metadata_db_playlist_delete_one(playlist_action_path);
    metadata_db_playlist_insert_one(dest);
    populate_playlists_screen();
    show_info_toast("Playlist renamed");
}

static void hide_playlist_context_menu_popup(void) {
    if (playlist_context_menu_popup) lv_obj_add_flag(playlist_context_menu_popup, LV_OBJ_FLAG_HIDDEN);
    if (playlist_context_menu_backdrop) lv_obj_add_flag(playlist_context_menu_backdrop, LV_OBJ_FLAG_HIDDEN);
}

static void playlist_context_menu_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_playlist_context_menu_popup();
}

static void playlist_context_menu_rename_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_playlist_context_menu_popup();
    show_text_entry("Rename Playlist", basename_of(playlist_action_path), false, false, playlist_rename_done_cb, NULL);
}

static void playlist_context_menu_delete_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_playlist_context_menu_popup();
    if (!playlist_delete_popup)
        playlist_delete_popup = build_confirm_popup("Delete playlist?", LV_LABEL_LONG_WRAP, NULL,
            "The playlist file will be deleted. Music files are kept.", "Delete",
            accent_lv_color(), playlist_delete_confirm_cb, NULL, "Cancel", accent_lv_color(),
            playlist_delete_cancel_cb, NULL, playlist_delete_cancel_cb, &playlist_delete_backdrop);
    lv_obj_remove_flag(playlist_delete_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(playlist_delete_popup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(playlist_delete_backdrop);
    lv_obj_move_foreground(playlist_delete_popup);
}

static void playlist_context_menu_cancel_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_playlist_context_menu_popup();
}

static void build_playlist_context_menu_popup(void) {
    static const menu_popup_row_t rows[] = {
        { "Rename Playlist", playlist_context_menu_rename_cb, false },
        { "Delete Playlist", playlist_context_menu_delete_cb, true },
        { "Cancel", playlist_context_menu_cancel_cb, false },
    };
    playlist_context_menu_popup = build_menu_popup(rows, (int) (sizeof(rows) / sizeof(rows[0])),
                                                    playlist_context_menu_backdrop_cb,
                                                    &playlist_context_menu_backdrop);
}

static void playlist_row_long_press_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_LONG_PRESSED) return;
    playlist_row_long_press_fired = true;
    int i = (int) (intptr_t) lv_event_get_user_data(e);
    if (i < 0 || i >= playlists_m3u_count) return;
    snprintf(playlist_action_path, sizeof(playlist_action_path), "%s", playlists_m3u_paths[i]);
    lv_obj_remove_flag(playlist_context_menu_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(playlist_context_menu_popup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(playlist_context_menu_backdrop);
    lv_obj_move_foreground(playlist_context_menu_popup);
}

static void populate_playlists_screen(void) {
    lv_obj_clean(playlists_list);

    for (int i = 0; i < playlists_m3u_count; i++) free(playlists_m3u_paths[i]);
    free(playlists_m3u_paths);
    playlists_m3u_paths = NULL;
    playlists_m3u_count = 0;
    /* Persistent cache of PLAYLISTS_DIR only -- see rescan_playlists(). */
    metadata_db_load_all_playlists(&playlists_m3u_paths, &playlists_m3u_count);

    lv_obj_t * create = add_playlist_row_base(playlists_list, "+ New Playlist");
    lv_obj_add_style(create, &style_theme_card_bg, 0);
    lv_obj_add_flag(create, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(create, new_playlist_row_cb, LV_EVENT_CLICKED, (void *) 1);
    build_list_section(playlists_list, "System playlists");

    /* Favorites/Most Played/Queue/Recently Added are never deletable/
     * renamable (none is a real .m3u file -- see playlist_row_click_cb()'s
     * index==0..3 special cases), so they get no long-press handler. */
    lv_obj_t * favorites_row = add_playlist_row_base(playlists_list, "Favorites");
    lv_obj_add_flag(favorites_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(favorites_row, playlist_row_click_cb, LV_EVENT_CLICKED, (void *) (intptr_t) 0);

    lv_obj_t * most_played_row = add_playlist_row_base(playlists_list, "Most Played");
    lv_obj_add_flag(most_played_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(most_played_row, playlist_row_click_cb, LV_EVENT_CLICKED, (void *) (intptr_t) 1);

    lv_obj_t * queue_row = add_playlist_row_base(playlists_list, "Queue");
    lv_obj_add_flag(queue_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(queue_row, playlist_row_click_cb, LV_EVENT_CLICKED, (void *) (intptr_t) 2);

    lv_obj_t * recently_added_row = add_playlist_row_base(playlists_list, "Recently Added");
    lv_obj_add_flag(recently_added_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(recently_added_row, playlist_row_click_cb, LV_EVENT_CLICKED, (void *) (intptr_t) 3);

    build_list_section(playlists_list, "User playlists");
    if (!playlists_m3u_count)
        build_list_message(playlists_list, "No user playlists", "Create a playlist above or copy one to the SD card's Playlists folder.");
    for (int i = 0; i < playlists_m3u_count; i++) {
        const char * display = playlists_m3u_paths[i];
        if (strncmp(display, PLAYLISTS_DIR "/", strlen(PLAYLISTS_DIR) + 1) == 0) display += strlen(PLAYLISTS_DIR) + 1;
        lv_obj_t * row = add_playlist_row_base(playlists_list, display);
        lv_obj_set_width(lv_obj_get_child(row, 0), LIST_ROW_WIDTH_WIDE - LIST_ROW_LABEL_INSET - BOARD_SCALE_PX(60));
        lv_label_set_long_mode(lv_obj_get_child(row, 0), LV_LABEL_LONG_DOT);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, playlist_row_click_cb, LV_EVENT_CLICKED, (void *) (intptr_t) (4 + i));
        lv_obj_add_event_cb(row, playlist_row_long_press_cb, LV_EVENT_LONG_PRESSED, (void *) (intptr_t) i);
    }
}

static lv_obj_t * build_playlists_screen(void) {
    lv_obj_t * title_label;
    lv_obj_t * scr = build_subsonic_list_screen("Playlists", &title_label, &playlists_list);

    /* Explicit cross-axis centering scoped to this screen also keeps rows
     * correct if it is ever hosted in a parent narrower than the display. */
    lv_obj_set_flex_align(playlists_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);

    return scr;
}

/* ---- CUE sheet track list (File Browser -> tap a .cue) ----
 * Allows browsing and jumping straight to any track within a single
 * audio rip (e.g. album.flac + album.cue). Tapping a track seeks the
 * shared audio file to that track's INDEX 01 offset (on_file_selected_at())
 * and begins playback from there. */
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

/* See scan_one_song_into_db()'s own comment on why this exists. Non-static
 * so crash_diag_handler() (main.c) can read it directly from a signal
 * handler -- no getter/mutex, a diagnostic breadcrumb doesn't need either,
 * and calling a function from a signal handler than just returns a global
 * would be no safer than reading the global itself. */
char g_scan_last_path[PATH_MAX] = "";
/* Published to the UI by library_rescan_done_flag's release/acquire pair. */
static bool library_rescan_succeeded;

static void * library_rescan_thread_func(void * arg) {
    (void) arg;
    install_thread_crash_altstack(); /* see its own comment (main.c) */
#ifdef __linux__
    struct sched_param sp = { 0 };
    sched_setscheduler(0, SCHED_BATCH, &sp);
    setpriority(PRIO_PROCESS, 0, 5);
#endif
    uint64_t started_ms = db_log_enabled() ? db_log_now_ms() : 0;
    DB_LOG("DB", "rescan_thread_begin rss_kb=%ld", db_log_rss_kb());
    library_scan_once();
    DB_LOG("DB", "rescan_thread_end songs=%lld elapsed_ms=%llu rss_kb=%ld",
           (long long) metadata_db_get_song_count(), (unsigned long long) (db_log_now_ms() - started_ms),
           db_log_rss_kb());
    atomic_store_explicit(&library_rescan_done_flag, true, memory_order_release); /* written last -- update_timer_cb only checks this flag */
    return NULL;
}

/* Controls automatic library rescanning on boot / SD mount. */
#define GUI_LIBRARY_AUTO_RESCAN_ENABLED true
bool gui_library_auto_rescan_enabled(void) {
    return GUI_LIBRARY_AUTO_RESCAN_ENABLED;
}

/* 4MB stack size configured for library rescan thread to accommodate deep
 * call chains through metadata parsers and tagcache indexing. */
#define LIBRARY_RESCAN_THREAD_STACK_SIZE (4 * 1024 * 1024)

void start_library_rescan(void) {
    /* Ignore request if a rescan is already running. */
    if (library_rescan_active) return;
    DB_LOG("DB", "rescan_requested existing_songs=%lld rss_kb=%ld",
           (long long) metadata_db_get_song_count(), db_log_rss_kb());
    /* A metadata parser child is already a meaningful peak on this 56 MiB
     * target. Do not overlap it with a previous warmer or lazy cover decode;
     * cancellation is cooperative and bounded by the artwork timeout. */
    quiesce_album_artwork_workers();
    atomic_store_explicit(&library_rescan_done_flag, false, memory_order_relaxed);
    library_rescan_active = true;
    library_rescan_token = gui_busy_show("Updating\nmusic database...", "");
    gui_busy_set_progress(library_rescan_token, 0);

    pthread_attr_t attr;
    pthread_attr_t * attr_ptr = NULL;
    if (pthread_attr_init(&attr) == 0) {
        if (pthread_attr_setstacksize(&attr, LIBRARY_RESCAN_THREAD_STACK_SIZE) == 0) attr_ptr = &attr;
    }
    bool created = pthread_create(&library_rescan_thread, attr_ptr, library_rescan_thread_func, NULL) == 0;
    if (attr_ptr) pthread_attr_destroy(&attr);
    if (!created) {
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
    }
    /* Purge screens being replaced from nav_stack before deleting them so a
     * subsequent nav_pop() does not pop a deleted screen -- a no-op for the
     * nav-stack part after nav_reset_to_home() above already cleared it,
     * but this ALSO clears gui_navigation.c's back_target_cache_screen if
     * it points at any of these (real use-after-free caught in review:
     * that cache can be covered by a screen one of these lists pushed, and
     * previously only the busy-screen branch ran this cleanup, leaving the
     * non-busy path free to delete a screen this cache still pointed at). */
    lv_obj_t * being_replaced[] = { all_songs_screen, artists_screen, albums_screen, album_artist_screen,
                                     recently_added_screen };
    gui_navigation_remove_screen_instances(being_replaced, (int)(sizeof(being_replaced) / sizeof(being_replaced[0])));

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
    reset_az_index_bindings();
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

/* Fast path for SD card reinsertion: loads the card's existing database
 * cache first for immediate library availability, followed by a background
 * rescan to detect any files modified while unmounted. */
static void reload_library_on_sd_reinsert(void) {
    playlist_files_refresh_async(PLAYLISTS_DIR);
    library_load_from_cache_only();
    refresh_library_screens_after_reload();
    if (metadata_db_get_song_count() > 0) {
        show_info_toast("Library loaded");
        start_album_thumbnail_generation();
    }
    if (gui_library_auto_rescan_enabled()) start_library_rescan();
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

    /* Deferred work is not an active worker/wakelock. Retry after the
     * failure-cache backoff, and resume after the user leaves Albums. */
    if (atomic_load(&album_thumb_gen_retry_pending) &&
        !atomic_load(&album_thumb_gen_active) && !album_thumbnail_active &&
        !atomic_load(&album_thumbnail_screen_active) && !library_rescan_active &&
        !sd_format_active && !audio_is_playing() &&
        lv_tick_elaps(album_thumb_gen_retry_tick) >= 15000 &&
#ifndef HOST_BUILD
        sd_card_root_is_mounted() &&
#endif
        artwork_check_memory_admission(ARTWORK_PRIO_WARMER, ALBUM_ART_METADATA_START_BYTES))
        start_album_thumbnail_generation();

    /* Keep the screen awake for the rescan progress display so the auto
     * screen-timeout does not trigger mid-rescan. */
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
    if (!library_rescan_succeeded) {
        gui_busy_hide(library_rescan_token);
        nav_reset_to_home();
        show_error_toast("Library update failed");
        return;
    }
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

#ifndef HOST_BUILD
/* Periodic polling for SD card mount state changes.
 *
 * When an unmounted card is detected, retries mounting. On transition to
 * mounted, reloads the database cache from the card. On transition to
 * unmounted, closes the database, clears library screens, and resets
 * the file browser to root. */
#define SD_CARD_MOUNT_POLL_SECONDS 5

/* Consecutive failed poll cycles (SD_CARD_MOUNT_POLL_SECONDS apart) before
 * the mount-failure popup shows -- 6 * 5s = 30s of genuinely stuck retries,
 * matching this file's own LIBRARY_SCAN_WALK_STALL_TIMEOUT_MS in spirit
 * (long enough that the kernel's own brief card-enumeration window right
 * after physical insertion can't false-positive, short enough the user
 * isn't left staring at an empty library wondering what's wrong). */
#define SD_MOUNT_FAIL_STREAK_THRESHOLD 6

bool sd_card_root_is_mounted(void) {
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
    /* Assume already mounted initially since mount_sd_card_if_needed() ran
     * at boot. */
    static bool was_mounted = true;
    /* See its own comment further down, where it's checked -- separate
     * one-shot guard for the boot-time-mount-race case was_mounted's own
     * confirmed-transition logic can silently miss. */
    static bool boot_library_recheck_done = false;
    static time_t last_check = 0;
    static int mount_fail_streak = 0;
    /* Require unmounted state across consecutive polls
     * (SD_UNMOUNT_CONFIRM_STREAK_THRESHOLD) before tearing down library
     * state to debounce brief transient unmounts. */
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
        gui_player_notify_sd_unmounted_immediate();
        mount_sd_card_if_needed();
        if (was_mounted) {
            /* Stop post-scan artwork reads/writes as soon as removal is
             * observed. Joining is deliberately deferred to the next
             * generator start/reap poll so this hotplug callback never
             * blocks the UI on an in-progress image decode. */
            cancel_album_thumbnail_generation();
            atomic_store(&album_thumb_gen_retry_pending, false);
            unmount_confirm_streak++;
            if (unmount_confirm_streak >= SD_UNMOUNT_CONFIRM_STREAK_THRESHOLD && !library_rescan_active) {
                gui_player_handle_sd_unmount();
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
    /* Every poll that observes the card mounted un-sticks the checkpoint
     * target, even for a transient blip that never reached the confirmed-
     * removal streak above. */
    gui_player_notify_sd_mounted();
    /* Reset file browser and reload fallback fonts before triggering the
     * rescan screen switch. */
    if (!was_mounted && !library_rescan_active) {
        file_browser_reset_to_root();
        fallback_font_on_sd_mounted();
        reload_library_on_sd_reinsert();
        gui_player_restore_sd_queue(false);
    } else if (!boot_library_recheck_done) {
        boot_library_recheck_done = true;
        fallback_font_on_sd_mounted();
        if (metadata_db_get_song_count() == 0 && !library_rescan_active) {
            file_browser_reset_to_root();
            reload_library_on_sd_reinsert();
        }
        /* Covers the boot-time mount race this whole branch exists for: the
         * card genuinely was NOT YET mounted when gui_init() ran its own
         * SD-restore attempt (both use the same sd_card_root_is_mounted()
         * check), but became mounted by this first poll. This is the only
         * other place a "the card actually showed up right after boot"
         * restore can happen --
         * boot_library_recheck_done's one-shot guard means this fires at
         * most once per run. */
        gui_player_restore_sd_queue(true);
    }
    was_mounted = true;
}
#else
bool sd_card_root_is_mounted(void) {
    struct stat st;
    return stat(MUSIC_ROOT_DIR, &st) == 0 && S_ISDIR(st.st_mode);
}

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

/* Displayed when persistent SD card mount failure is detected. */
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
    /* Null-check ensures safe teardown even if countdown popups were not built. */
    if (power_off_countdown_popup_backdrop) lv_obj_add_flag(power_off_countdown_popup_backdrop, LV_OBJ_FLAG_HIDDEN);
    if (power_off_countdown_popup) lv_obj_add_flag(power_off_countdown_popup, LV_OBJ_FLAG_HIDDEN);
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
    lv_obj_set_size(power_off_countdown_popup, BOARD_SCALE_PX(320), BOARD_SCALE_PX(280));
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
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, BOARD_SCALE_PX(20));
    lv_label_set_text(title, "Powering Off");

    power_off_countdown_label = lv_label_create(power_off_countdown_popup);
    lv_obj_add_style(power_off_countdown_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(power_off_countdown_label, gui_theme_font(GUI_FONT_ROLE_TITLE), 0);
    lv_obj_align(power_off_countdown_label, LV_ALIGN_CENTER, 0, BOARD_SCALE_PX(-10));
    lv_label_set_text_fmt(power_off_countdown_label, "%d", POWER_OFF_COUNTDOWN_SECONDS);

    lv_obj_t * cancel_row = lv_obj_create(power_off_countdown_popup);
    lv_obj_set_size(cancel_row, lv_pct(90), BOARD_SCALE_PX(56));
    lv_obj_align(cancel_row, LV_ALIGN_BOTTOM_MID, 0, BOARD_SCALE_PX(-20));
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
    /* metadata_db_get_group_counts() is a real DB query, worth skipping
     * outright (not just leaving DB_LOG to no-op on the result) since this
     * runs on every ordinary tap into Albums, not just during a scan. */
    if (db_log_enabled()) {
        int artist_count = 0, album_artist_count = 0, album_count = 0;
        metadata_db_get_group_counts(&artist_count, &album_artist_count, &album_count);
        albums_page_open_requested_ms = db_log_now_ms();
        DB_LOG("ALBUMS_PAGE", "open_requested albums=%d cache_worker_active=%d lazy_active=%d rss_kb=%ld",
               album_count, (int) atomic_load(&album_thumb_gen_active), (int) album_thumbnail_active,
               db_log_rss_kb());
    }
    nav_push(albums_screen);
}

static void album_artist_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(album_artist_screen);
}

static void playlists_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    populate_playlists_screen();
    nav_push(playlists_screen);
}


static void music_files_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(files_screen);
}

static void music_screen_playback_settings_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(gui_settings_get_music_screen());
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
    // icon size percentage is scaled using BOARD_SCALE_PX which is relative to device screen width, so that it look a similar size on all devices
    lv_obj_t * scr = build_launcher_menu_screen("Music", generic_back_cb, items, 6, BOARD_SCALE_PX(80), false,
                                                 &launcher_layout_config.music);
    /* Same real stock-firmware gear icon as the Queue screen's own Options
     * button (sub_back/set.png) -- build_top_right_icon_button() guarantees
     * it lands at exactly the same visual level as this screen's own back
     * arrow, on the opposite corner. Shortcuts straight to Music Settings
     * rather than the full Settings > Music Settings drill-down. */
    build_top_right_icon_button(scr, asset_path("sub_back/set.png"), music_screen_playback_settings_cb);
    finalize_screen_navigation(scr);
    return scr;
}

void gui_library_refresh_music_screen(void) {
    lv_obj_t * old = music_screen;
    lv_obj_t * fresh = build_music_screen();
    if (!fresh) return;
    music_screen = fresh;
    gui_navigation_replace_static_screen(1, old, fresh);
    if (old) lv_obj_del(old);
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
 * read-only storage on a real device. */

/* Drill-down target for Artists and Album Artists: tapping either shows the
 * artist's own albums (regrouping just their songs by album, via
 * build_groups_by_indices) rather than dumping straight into a flat song
 * list -- tapping an album from there lands on show_group_songs() same as
 * every other terminal list. One persistent screen shared by both callers,
 * same rebuild-in-place approach as group_songs_screen. */
static group_row_t * artist_albums_groups;
static int artist_albums_group_count;
/* Name/kind of the artist or album artist this screen is currently showing
 * -- set by show_artist_albums(), read back by artist_album_row_click_cb()'s
 * own "All Songs" row (index 0, see that function's own comment) to fetch
 * every song credited to them, flattened across all their albums. */
static char artist_albums_current_name[128];
static metadata_db_group_kind_t artist_albums_current_kind;

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
/* Sort scratch for artist_albums_show_all_songs() below -- carries the same
 * owned path/title an eventual group_song_entry_t needs, plus just enough
 * of each song's own tags to order the flattened list the same way a real
 * album listing would (see cmp_artist_song_sort_entry()'s own comment).
 * Freed after the final group_song_entry_t[] is built; its path/title
 * pointers are MOVED there, not copied again. */
typedef struct {
    char * path;
    char * title;
    char album[128];
    char album_artist[128];
    int32_t disc_number;
    int32_t track_number;
} artist_song_sort_entry_t;

/* Sorts songs by album, then album artist, then disc number, track number,
 * and file path. Grouping by both album and album artist prevents collision
 * between same-titled albums from different artists, while sorting by disc
 * and track preserves listening order within each album. */
static int cmp_artist_song_sort_entry(const void * a, const void * b) {
    const artist_song_sort_entry_t * ea = (const artist_song_sort_entry_t *) a;
    const artist_song_sort_entry_t * eb = (const artist_song_sort_entry_t *) b;
    int c = strcasecmp(ea->album, eb->album);
    if (c) return c;
    c = strcasecmp(ea->album_artist, eb->album_artist);
    if (c) return c;
    int32_t da = ea->disc_number > 0 ? ea->disc_number : 1;
    int32_t db = eb->disc_number > 0 ? eb->disc_number : 1;
    if (da != db) return da < db ? -1 : 1;
    int32_t ta = ea->track_number, tb = eb->track_number;
    if (ta > 0 || tb > 0) {
        if (ta <= 0) return 1;
        if (tb <= 0) return -1;
        if (ta != tb) return ta < tb ? -1 : 1;
    }
    return strcasecmp(ea->path, eb->path);
}

/* Fetches every song credited to artist_albums_current_name (flattened
 * across all its albums) and hands it to show_group_songs_take_ownership()
 * -- the "All Songs" row prepended at index 0 by show_artist_albums().
 *
 * Exact track count is queried via metadata_db_get_group_offset() /
 * metadata_db_get_groups_page() so buffer sizing matches accurately. */
#define ARTIST_ALBUMS_ALL_SONGS_CAP 4000

static bool artist_albums_show_all_songs(void) {
    int64_t offset = metadata_db_get_group_offset(artist_albums_current_kind, artist_albums_current_name, NULL);
    group_row_t artist_row;
    int real_total = (offset >= 0 && metadata_db_get_groups_page(artist_albums_current_kind, (int) offset, 1,
                                                                  &artist_row) == 1)
                          ? artist_row.song_count
                          : 0;
    if (real_total <= 0) return false;
    int total = real_total > ARTIST_ALBUMS_ALL_SONGS_CAP ? ARTIST_ALBUMS_ALL_SONGS_CAP : real_total;

    /* calloc, not malloc -- a failure partway through the fetch loop below
     * frees every one of these `total` slots unconditionally (see that
     * branch's own comment), relying on the not-yet-reached ones staying
     * NULL from this zero-init rather than holding garbage. */
    artist_song_sort_entry_t * sort_entries = calloc((size_t) total, sizeof(*sort_entries));
    if (!sort_entries) return false;

    int n = 0;
    bool failed = false;
    song_row_t page[64];
    while (n < total) {
        int want = total - n;
        if (want > 64) want = 64;
        int got = artist_albums_current_kind == METADATA_DB_GROUP_ALBUM_ARTIST
                      ? metadata_db_get_album_artist_songs(artist_albums_current_name, n, page, want)
                      : metadata_db_get_artist_songs(artist_albums_current_name, n, page, want);
        if (got <= 0) break;
        for (int i = 0; i < got; i++) {
            char title[192];
            format_music_submenu_identity(&page[i], title, sizeof(title));
            artist_song_sort_entry_t * dst = &sort_entries[n + i];
            dst->path = strdup(page[i].path);
            dst->title = strdup(title);
            snprintf(dst->album, sizeof(dst->album), "%s", page[i].tags.album);
            snprintf(dst->album_artist, sizeof(dst->album_artist), "%s", page[i].tags.album_artist);
            dst->disc_number = page[i].tags.disc_number;
            dst->track_number = page[i].tags.track_number;
            if (!dst->path || !dst->title) {
                failed = true;
                break;
            }
        }
        if (failed) break;
        n += got;
        if (got < want) break;
    }

    if (failed || n <= 0) {
        /* `total`, not `n` -- covers the current, partially-filled batch
         * (whatever this failure landed on) plus every never-reached slot,
         * all still NULL/zeroed from the calloc above. free(NULL) is a
         * no-op, so this is safe regardless of exactly how far the loop
         * got. */
        for (int i = 0; i < total; i++) {
            free(sort_entries[i].path);
            free(sort_entries[i].title);
        }
        free(sort_entries);
        return false;
    }

    /* Sort songs in disc and track order within each album across the artist's catalog. */
    qsort(sort_entries, (size_t) n, sizeof(*sort_entries), cmp_artist_song_sort_entry);

    /* Transfer ownership of path and title strings into a group_song_entry_t
     * array for show_group_songs_take_ownership() to avoid duplicate allocations. */
    group_song_entry_t * entries = malloc(sizeof(*entries) * (size_t) n);
    if (!entries) {
        for (int i = 0; i < n; i++) {
            free(sort_entries[i].path);
            free(sort_entries[i].title);
        }
        free(sort_entries);
        return false;
    }
    for (int i = 0; i < n; i++) {
        entries[i].path = sort_entries[i].path;
        entries[i].title = sort_entries[i].title;
    }
    free(sort_entries);

    /* Disclose truncation via a toast if the songs exceed the display cap. */
    if (real_total > ARTIST_ALBUMS_ALL_SONGS_CAP) {
        char msg[96];
        snprintf(msg, sizeof(msg), "Showing first %d of %d songs", n, real_total);
        show_info_toast(msg);
    }

    show_group_songs_take_ownership(artist_albums_current_name, entries, n);
    /* entries is now owned by group_songs_entries -- do not free here. */
    return true;
}

static void artist_album_row_click_cb(int index) {
    if (index == 0) {
        (void) artist_albums_show_all_songs();
        return;
    }
    int group_index = index - 1;
    if (group_index < 0 || group_index >= artist_albums_group_count) return;
    group_row_t * group = &artist_albums_groups[group_index];
    group_song_entry_t * entries = calloc((size_t) group->song_count, sizeof(*entries));
    int n = 0;
    song_row_t page[64];
    while (entries && n < group->song_count) {
        int want = group->song_count - n;
        if (want > 64) want = 64;
        int got = metadata_db_get_album_songs(group->name, group->album_artist, n, page, want);
        if (got <= 0) break;
        for (int i = 0; i < got; i++) {
            char title[192];
            format_music_submenu_identity(&page[i], title, sizeof(title));
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
    show_music_group_songs(group->name, entries, n);
    free_group_song_entries(entries, n);
    group_songs_source_is_album = true;
}

static void hide_collection_menu(void) {
    if (collection_menu_popup) lv_obj_add_flag(collection_menu_popup, LV_OBJ_FLAG_HIDDEN);
    if (collection_menu_backdrop) lv_obj_add_flag(collection_menu_backdrop, LV_OBJ_FLAG_HIDDEN);
    if (album_collection_menu_popup) lv_obj_add_flag(album_collection_menu_popup, LV_OBJ_FLAG_HIDDEN);
    if (album_collection_menu_backdrop) lv_obj_add_flag(album_collection_menu_backdrop, LV_OBJ_FLAG_HIDDEN);
}

static void collection_menu_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) hide_collection_menu();
}

static void show_collection_menu(bool album) {
    lv_obj_t * popup = album ? album_collection_menu_popup : collection_menu_popup;
    lv_obj_t * backdrop = album ? album_collection_menu_backdrop : collection_menu_backdrop;
    if (!popup || !backdrop) return;
    lv_obj_remove_flag(backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(popup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(backdrop);
    lv_obj_move_foreground(popup);
}

static bool play_current_group(play_mode_t mode) {
    if (group_songs_count <= 0) return false;
    char ** paths = calloc((size_t) group_songs_count, sizeof(*paths));
    if (!paths) return false;
    for (int i = 0; i < group_songs_count; i++) {
        paths[i] = strdup(group_songs_entries[i].path);
        if (!paths[i]) {
            for (int j = 0; j < group_songs_count; j++) free(paths[j]);
            free(paths);
            return false;
        }
    }
    int start = mode == PLAY_MODE_SHUFFLE ? rand() % group_songs_count : 0;
    gui_player_set_play_mode(mode);
    set_player_source_group_songs(start);
    on_file_selected(paths, group_songs_count, start);
    return true;
}

static bool load_collection_for_playback(void) {
    if (!collection_menu_is_album) {
        return artist_albums_show_all_songs();
    }
    group_row_t group = {0};
    snprintf(group.name, sizeof(group.name), "%s", collection_menu_name);
    snprintf(group.album_artist, sizeof(group.album_artist), "%s", collection_menu_album_artist);
    group.song_count = collection_menu_song_count;
    return show_album_group(&group);
}

static void collection_play_shuffled_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_collection_menu();
    if (load_collection_for_playback()) play_current_group(PLAY_MODE_SHUFFLE);
}

static void collection_play_sequential_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_collection_menu();
    if (load_collection_for_playback()) play_current_group(PLAY_MODE_SEQUENTIAL);
}

static bool collection_song_at(int offset, song_row_t * song) {
    if (collection_menu_is_album)
        return metadata_db_get_album_songs(collection_menu_name, collection_menu_album_artist,
                                            offset, song, 1) == 1;
    return (collection_menu_artist_kind == METADATA_DB_GROUP_ALBUM_ARTIST
                ? metadata_db_get_album_artist_songs(collection_menu_name, offset, song, 1)
                : metadata_db_get_artist_songs(collection_menu_name, offset, song, 1)) == 1;
}

static void collection_add_random_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_collection_menu();
    if (collection_menu_song_count <= 0) return;
    song_row_t song;
    if (collection_song_at(rand() % collection_menu_song_count, &song))
        gui_player_queue_add(song.path);
}

static void collection_add_album_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_collection_menu();
    int count = 0;
    group_song_entry_t * entries = load_album_entries(collection_menu_name,
                                                       collection_menu_album_artist,
                                                       collection_menu_song_count, &count);
    if (!entries) return;
    const char ** paths = malloc(sizeof(*paths) * (size_t) count);
    if (paths) {
        for (int i = 0; i < count; i++) paths[i] = entries[i].path;
        gui_player_queue_add_many(paths, count);
        free(paths);
    }
    free_group_song_entries(entries, count);
}

static void open_album_collection_menu(const group_row_t * group) {
    collection_menu_is_album = true;
    collection_menu_song_count = group->song_count;
    snprintf(collection_menu_name, sizeof(collection_menu_name), "%s", group->name);
    snprintf(collection_menu_album_artist, sizeof(collection_menu_album_artist), "%s", group->album_artist);
    show_collection_menu(true);
}

static void album_more_click_cb(int index) {
    index = search_remap_index(SEARCH_BINDING_ALBUMS, index);
    group_row_t group;
    if (metadata_db_get_albums_page_filtered(NULL, index, 1, &group) == 1)
        open_album_collection_menu(&group);
}

static void artist_album_more_click_cb(int index) {
    if (index > 0) {
        int group_index = index - 1;
        if (group_index >= 0 && group_index < artist_albums_group_count)
            open_album_collection_menu(&artist_albums_groups[group_index]);
        return;
    }
    int64_t offset = metadata_db_get_group_offset(artist_albums_current_kind,
                                                   artist_albums_current_name, NULL);
    group_row_t group;
    if (offset < 0 || metadata_db_get_groups_page(artist_albums_current_kind,
                                                   (int) offset, 1, &group) != 1) return;
    collection_menu_is_album = false;
    collection_menu_artist_kind = artist_albums_current_kind;
    collection_menu_song_count = group.song_count;
    snprintf(collection_menu_name, sizeof(collection_menu_name), "%s", artist_albums_current_name);
    collection_menu_album_artist[0] = '\0';
    show_collection_menu(false);
}

static void build_collection_menus(void) {
    const menu_popup_row_t rows[] = {
        { "Play all shuffled", collection_play_shuffled_cb, false },
        { "Play sequentially", collection_play_sequential_cb, false },
        { "Add a random song to queue", collection_add_random_cb, false },
    };
    const menu_popup_row_t album_rows[] = {
        { "Play all shuffled", collection_play_shuffled_cb, false },
        { "Play sequentially", collection_play_sequential_cb, false },
        { "Add a random song to queue", collection_add_random_cb, false },
        { "Add album to queue", collection_add_album_cb, false },
    };
    collection_menu_popup = build_menu_popup(rows, (int) (sizeof(rows) / sizeof(rows[0])),
                                               collection_menu_backdrop_cb,
                                               &collection_menu_backdrop);
    album_collection_menu_popup = build_menu_popup(album_rows,
                                                     (int) (sizeof(album_rows) / sizeof(album_rows[0])),
                                                     collection_menu_backdrop_cb,
                                                     &album_collection_menu_backdrop);
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
                /* +1 -- show_artist_albums() prepends an "All Songs" row at
                 * index 0, ahead of every artist_albums_groups[] entry. */
                match = i + 1;
                break;
            }
        }
    }

    compact_list_set_now_playing(artist_albums_list, match);
}

void show_artist_albums(const char * name, metadata_db_group_kind_t kind) {
    snprintf(artist_albums_current_name, sizeof(artist_albums_current_name), "%s", name);
    artist_albums_current_kind = kind;

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
    /* "All Songs" prepended at index 0 -- artist_album_row_click_cb() and
     * refresh_artist_albums_now_playing_indicator() both account for this
     * same +1 shift against artist_albums_groups[]. identity=0 (no real
     * song id) makes album_row_thumbnail_decorator() hide this row's cover
     * image via its own existing song_id<=0 fallback -- no decorator
     * changes needed. */
    compact_list_item_t * items = artist_albums_group_count > 0
        ? malloc(sizeof(*items) * (size_t) (artist_albums_group_count + 1)) : NULL;
    if (items) {
        items[0] = (compact_list_item_t){
            .label = "All Songs", .identity = 0, .trailing_asset = "playing_plane/ic_more.png"
        };
        for (int i = 0; i < artist_albums_group_count; i++) {
            items[i + 1] = (compact_list_item_t){
                .label = artist_albums_groups[i].name,
                .identity = artist_albums_groups[i].first_song_id,
                .trailing_asset = "playing_plane/ic_more.png"
            };
        }
        compact_list_set_items(artist_albums_list, items, artist_albums_group_count + 1);
        free(items);
    } else {
        compact_list_set_items(artist_albums_list, NULL, 0);
    }
    compact_list_set_row_decorator(artist_albums_list, album_row_thumbnail_decorator, NULL);
    compact_list_set_trailing_click(artist_albums_list, artist_album_more_click_cb);
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



/* Diagnostic logging helper for teardown and initialization steps. */
static void library_teardown_diag(const char * step);

void gui_library_init(void) {
    library_teardown_diag("az_index_drag_timer before");
    if (!az_index_drag_timer) az_index_drag_timer = lv_timer_create(poll_az_index_drag, LV_DEF_REFR_PERIOD, NULL);
    library_teardown_diag("build_files_screen before");
    files_screen = build_files_screen();
    library_teardown_diag("build_all_songs_screen before");
    all_songs_screen = build_all_songs_screen();
    library_teardown_diag("build_recently_added_screen before");
    recently_added_screen = build_recently_added_screen();
    library_teardown_diag("build_artists_screen before");
    artists_screen = build_artists_screen();
    library_teardown_diag("build_albums_screen before");
    albums_screen = build_albums_screen();
    library_teardown_diag("build_album_artist_screen before");
    album_artist_screen = build_album_artist_screen();
    library_teardown_diag("build_playlists_screen before");
    playlists_screen = build_playlists_screen();
    build_playlist_context_menu_popup();
    library_teardown_diag("build_cue_tracks_screen before");
    cue_tracks_screen = build_cue_tracks_screen();
    library_teardown_diag("build_group_songs_screen before");
    group_songs_screen = build_group_songs_screen();

    library_teardown_diag("build_compact_list_screen(Albums) before");
    artist_albums_screen = build_compact_list_screen("Albums", generic_back_cb, NULL, 0,
                                                      artist_album_row_click_cb, NULL,
                                                      &artist_albums_list, &artist_albums_title_label,
                                                      LIST_ROW_WIDTH_WIDE, true, accent_lv_color());
    library_teardown_diag("compact_list_set_row_height before");
    compact_list_set_row_height(artist_albums_list, MUSIC_LIST_ROW_HEIGHT);
    library_teardown_diag("artist_albums event cbs before");
    lv_obj_add_event_cb(artist_albums_screen, album_thumbnail_screen_loaded_cb,
                        LV_EVENT_SCREEN_LOADED, artist_albums_list);
    lv_obj_add_event_cb(artist_albums_screen, album_thumbnail_screen_unloaded_cb,
                        LV_EVENT_SCREEN_UNLOADED, artist_albums_list);
    lv_obj_add_event_cb(artist_albums_list, album_thumbnail_scroll_cb, LV_EVENT_SCROLL_BEGIN, NULL);
    lv_obj_add_event_cb(artist_albums_list, album_thumbnail_scroll_cb, LV_EVENT_SCROLL_END, NULL);
    library_teardown_diag("finalize_screen_navigation(artist_albums) before");
    finalize_screen_navigation(artist_albums_screen);
    library_teardown_diag("build_add_to_playlist_screen before");
    add_to_playlist_screen = build_add_to_playlist_screen();
    library_teardown_diag("build_music_screen before");
    music_screen = build_music_screen();

    /* Register A-Z index & search */
    library_teardown_diag("register_az_index(artists) before");
    register_az_index(artists_screen, artists_list, METADATA_DB_AZ_ARTIST);
    library_teardown_diag("register_az_index(albums) before");
    register_az_index(albums_screen, albums_list, METADATA_DB_AZ_ALBUM);
    library_teardown_diag("register_az_index(album_artist) before");
    register_az_index(album_artist_screen, album_artist_list, METADATA_DB_AZ_ALBUM_ARTIST);
    library_teardown_diag("register_az_index(all_songs) before");
    register_az_index(all_songs_screen, all_songs_list, METADATA_DB_AZ_ALL_SONGS);

    library_teardown_diag("register_search(artists) before");
    register_search(SEARCH_BINDING_ARTISTS, artists_screen, artists_list, NULL, NULL, false,
                     true, METADATA_DB_AZ_ARTIST, artists_fetch_page);
    library_teardown_diag("register_search(albums) before");
    register_search(SEARCH_BINDING_ALBUMS, albums_screen, albums_list, NULL, NULL, false,
                     true, METADATA_DB_AZ_ALBUM, albums_fetch_page);
    library_teardown_diag("register_search(album_artist) before");
    register_search(SEARCH_BINDING_ALBUM_ARTIST, album_artist_screen, album_artist_list, NULL,
                     NULL, false, true, METADATA_DB_AZ_ALBUM_ARTIST, album_artists_fetch_page);
    library_teardown_diag("register_search(all_songs) before");
    register_search(SEARCH_BINDING_ALL_SONGS, all_songs_screen, all_songs_list, NULL, NULL, false,
                     true, METADATA_DB_AZ_ALL_SONGS, all_songs_fetch_page);

    library_teardown_diag("build_compact_list_widget(files_search) before");
    files_search_list = build_compact_list_widget(files_screen, NULL, 0, files_search_row_click_cb, NULL, LIST_ROW_WIDTH_WIDE, false, lv_color_black());
    lv_obj_set_style_bg_opa(files_search_list, LV_OPA_COVER, 0);
    lv_obj_add_style(files_search_list, &style_theme_screen_bg, 0);
    lv_obj_add_flag(files_search_list, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(files_search_list, LV_OBJ_FLAG_GESTURE_BUBBLE);
    library_teardown_diag("enable_gesture_bubble_recursive(files_search_list) before");
    enable_gesture_bubble_recursive(files_search_list);

    library_teardown_diag("register_search(files) before");
    register_search(SEARCH_BINDING_FILES, files_screen, files_search_list, NULL, NULL, true,
                     true, METADATA_DB_AZ_ALL_SONGS, all_songs_fetch_page);

    library_teardown_diag("build_sd_mount_failed_popup before");
    build_sd_mount_failed_popup();
    library_teardown_diag("build_sd_format_confirm_popup before");
    build_sd_format_confirm_popup();
    library_teardown_diag("build_collection_menus before");
    build_collection_menus();
    library_teardown_diag("gui_library_init done");
}

/* For gui_reload.c's in-process UI reload -- deletes every screen this
 * module owns so gui_library_init() can rebuild them from a clean slate
 * without leaking the old objects. Deliberately does NOT touch
 * az_index_drag_timer (already guarded/reused correctly by gui_library_
 * init() itself) or the search_bindings[]/az-index registries -- register_
 * search()/register_az_index() already free and reassign their own prior
 * state on re-registration. sd_mount_failed_popup/sd_format_confirm_popup
 * and their backdrops are built directly on lv_layer_top() (see
 * build_confirm_popup()'s own comment), not as children of any of these
 * screens, so they need their own explicit deletion. */
/* Diagnostic logging helper writing teardown and init steps to reload_diag.log
 * with fsync per line to pinpoint failures during theme reload. */
static void library_teardown_diag(const char * step) {
    int fd = open("/data/mnt/sd_0/reload_diag.log", O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) return;
    char line[96];
    int len = snprintf(line, sizeof(line), "[pid=%ld]   gui_library_teardown: %s\n", (long) getpid(), step);
    if (len > 0) {
        if (len >= (int) sizeof(line)) len = (int) sizeof(line) - 1;
        write(fd, line, (size_t) len);
        fsync(fd);
    }
    close(fd);
}

void gui_library_teardown(void) {
    reset_az_index_bindings();
    if (playlist_start_popup) { lv_obj_delete(playlist_start_popup); playlist_start_popup = NULL; }
    if (playlist_start_backdrop) { lv_obj_delete(playlist_start_backdrop); playlist_start_backdrop = NULL; }
    if (playlist_delete_popup) { lv_obj_delete(playlist_delete_popup); playlist_delete_popup = NULL; }
    if (playlist_delete_backdrop) { lv_obj_delete(playlist_delete_backdrop); playlist_delete_backdrop = NULL; }
    if (playlist_context_menu_popup) { lv_obj_delete(playlist_context_menu_popup); playlist_context_menu_popup = NULL; }
    if (playlist_context_menu_backdrop) { lv_obj_delete(playlist_context_menu_backdrop); playlist_context_menu_backdrop = NULL; }
    /* poll_power_off_countdown() runs every tick from update_timer_cb,
     * which this reload never touches or pauses -- if a countdown is live
     * when a reload happens, deleting power_off_countdown_popup/backdrop
     * below without this would leave power_off_countdown_active true and
     * power_off_countdown_start_tick unchanged, so the countdown keeps
     * running against a freshly rebuilt (and therefore hidden-by-default)
     * popup with no visible warning at all, and still calls
     * idle_shutdown_now() once it reaches zero -- an invisible, surprise
     * power-off. cancel_power_off_countdown() is the same function Cancel/
     * the backdrop tap already uses, so this is exactly "the user cancelled
     * it," not a new code path. */
    library_teardown_diag("cancel_power_off_countdown before");
    cancel_power_off_countdown();
    library_teardown_diag("sd_mount_failed_popup before");
    if (sd_mount_failed_popup) { lv_obj_del(sd_mount_failed_popup); sd_mount_failed_popup = NULL; }
    library_teardown_diag("sd_mount_failed_popup_backdrop before");
    if (sd_mount_failed_popup_backdrop) { lv_obj_del(sd_mount_failed_popup_backdrop); sd_mount_failed_popup_backdrop = NULL; }
    library_teardown_diag("sd_format_confirm_popup before");
    if (sd_format_confirm_popup) { lv_obj_del(sd_format_confirm_popup); sd_format_confirm_popup = NULL; }
    library_teardown_diag("sd_format_confirm_popup_backdrop before");
    if (sd_format_confirm_popup_backdrop) { lv_obj_del(sd_format_confirm_popup_backdrop); sd_format_confirm_popup_backdrop = NULL; }
    library_teardown_diag("collection_menu_popup before");
    if (collection_menu_popup) { lv_obj_del(collection_menu_popup); collection_menu_popup = NULL; }
    library_teardown_diag("collection_menu_backdrop before");
    if (collection_menu_backdrop) { lv_obj_del(collection_menu_backdrop); collection_menu_backdrop = NULL; }
    library_teardown_diag("album_collection_menu_popup before");
    if (album_collection_menu_popup) { lv_obj_del(album_collection_menu_popup); album_collection_menu_popup = NULL; }
    library_teardown_diag("album_collection_menu_backdrop before");
    if (album_collection_menu_backdrop) {
        lv_obj_del(album_collection_menu_backdrop);
        album_collection_menu_backdrop = NULL;
    }
    library_teardown_diag("music_screen before");
    if (music_screen) { lv_obj_del(music_screen); music_screen = NULL; }
    library_teardown_diag("files_screen before");
    if (files_screen) { lv_obj_del(files_screen); files_screen = NULL; }
    library_teardown_diag("all_songs_screen before");
    if (all_songs_screen) { lv_obj_del(all_songs_screen); all_songs_screen = NULL; }
    library_teardown_diag("recently_added_screen before");
    if (recently_added_screen) { lv_obj_del(recently_added_screen); recently_added_screen = NULL; }
    library_teardown_diag("artists_screen before");
    if (artists_screen) { lv_obj_del(artists_screen); artists_screen = NULL; }
    library_teardown_diag("albums_screen before");
    if (albums_screen) { lv_obj_del(albums_screen); albums_screen = NULL; }
    library_teardown_diag("album_artist_screen before");
    if (album_artist_screen) { lv_obj_del(album_artist_screen); album_artist_screen = NULL; }
    library_teardown_diag("group_songs_screen before");
    if (group_songs_screen) { lv_obj_del(group_songs_screen); group_songs_screen = NULL; }
    library_teardown_diag("artist_albums_screen before");
    if (artist_albums_screen) { lv_obj_del(artist_albums_screen); artist_albums_screen = NULL; }
    library_teardown_diag("playlists_screen before");
    if (playlists_screen) { lv_obj_del(playlists_screen); playlists_screen = NULL; }
    library_teardown_diag("cue_tracks_screen before");
    if (cue_tracks_screen) { lv_obj_del(cue_tracks_screen); cue_tracks_screen = NULL; }
    library_teardown_diag("add_to_playlist_screen before");
    if (add_to_playlist_screen) { lv_obj_del(add_to_playlist_screen); add_to_playlist_screen = NULL; }
    /* The screens owned these children; clear the borrowed pointers with
     * their parents so non-NULL remains a valid liveness check. */
    files_search_list = NULL;
    all_songs_list = NULL;
    recently_added_list = NULL;
    artists_list = NULL;
    albums_list = NULL;
    album_artist_list = NULL;
    group_songs_list = NULL;
    group_songs_title_label = NULL;
    group_songs_edit_btn = NULL;
    group_songs_now_playing_bar = NULL;
    artist_albums_list = NULL;
    artist_albums_title_label = NULL;
    playlists_list = NULL;
    cue_tracks_list = NULL;
    cue_tracks_title_label = NULL;
    add_to_playlist_list = NULL;
    album_thumbnail_active_list = NULL;
    album_thumbnail_result_list = NULL;
    /* build_power_off_countdown_popup() -- called separately from gui_init()
     * (not from gui_library_init()), but its two lv_layer_top() objects are
     * still this file's own statics to own and delete. Left out, a visible
     * (if currently hidden) old popup/backdrop pair would linger above the
     * rebuilt UI forever, invisible only until the next power-off countdown
     * actually shows it. */
    library_teardown_diag("power_off_countdown_popup before");
    if (power_off_countdown_popup) { lv_obj_del(power_off_countdown_popup); power_off_countdown_popup = NULL; }
    library_teardown_diag("power_off_countdown_popup_backdrop before");
    if (power_off_countdown_popup_backdrop) {
        lv_obj_del(power_off_countdown_popup_backdrop);
        power_off_countdown_popup_backdrop = NULL;
    }
    power_off_countdown_label = NULL;
    library_teardown_diag("done");
}

void gui_library_resume_fast_timers(void) {
    if (az_index_drag_timer && lv_timer_get_paused(az_index_drag_timer)) {
        lv_timer_resume(az_index_drag_timer);
        lv_timer_ready(az_index_drag_timer);
    }
}

void gui_library_reset_drag_state(void) {
    if (az_index_dragging && az_index_active_binding) {
        lv_obj_add_flag(az_index_active_binding->popup, LV_OBJ_FLAG_HIDDEN);
        az_index_active_binding = NULL;
    }
    az_index_dragging = false;
    if (az_index_drag_timer) {
        lv_timer_pause(az_index_drag_timer);
    }
}

typedef struct {
    char root[600];
    char spool_path[PATH_MAX];
    atomic_bool done;
    bool ok;
    int count;
    atomic_int progress;
} scan_walk_work_t;

/* Binary length-prefixed spool: paths can contain whitespace and, unlike a
 * newline-delimited temporary file, this remains correct even for odd names.
 * The spool lives on the SD card, so discovery storage scales with library
 * size without consuming the device's RAM. */
static bool scan_spool_visit_cb(const char * path, void * user) {
    FILE * f = (FILE *) user;
    size_t n = strlen(path);
    if (n > UINT32_MAX) return false;
    uint32_t len = (uint32_t) n;
    return fwrite(&len, sizeof(len), 1, f) == 1 && fwrite(path, 1, n, f) == n;
}


#define LIBRARY_SCAN_FILE_TIMEOUT_MS 5000
#define LIBRARY_SCAN_WALK_STALL_TIMEOUT_MS 30000

static void * scan_walk_worker(void * arg) {
    scan_walk_work_t * w = (scan_walk_work_t *) arg;
    FILE * spool = fopen(w->spool_path, "wb");
    if (!spool) {
        w->ok = false;
        w->done = true;
        return NULL;
    }
    w->ok = file_browser_walk_all_songs_excluding_top_level(
        w->root, AUDIOBOOKS_LIBRARY_DIR_NAME, scan_spool_visit_cb, spool, &w->count, &w->progress);
    if (fclose(spool) != 0) w->ok = false;
    atomic_store_explicit(&w->done, true, memory_order_release);
    return NULL;
}

#define SCAN_WALK_THREAD_STACK_SIZE (1024 * 1024)
static bool scan_all_songs_with_timeout(const char * root, char * out_spool_path, size_t out_spool_size,
                                        int * out_count) {
    scan_walk_work_t * w = calloc(1, sizeof(*w));
    if (!w) return false;
    snprintf(w->root, sizeof(w->root), "%s", root);
    snprintf(w->spool_path, sizeof(w->spool_path), "%s/.open_hiby_scan_%ld_%p.tmp",
             root, (long) getpid(), (void *) w);

    pthread_attr_t attr;
    pthread_attr_t * attr_ptr = NULL;
    if (pthread_attr_init(&attr) == 0) {
        if (pthread_attr_setstacksize(&attr, SCAN_WALK_THREAD_STACK_SIZE) == 0) attr_ptr = &attr;
    }

    pthread_t thread;
    bool created = pthread_create(&thread, attr_ptr, scan_walk_worker, w) == 0;
    if (attr_ptr) pthread_attr_destroy(&attr);
    if (!created) {
        free(w);
        return false;
    }
    pthread_detach(thread);

    int last_seen_progress = 0;
    int stalled_ms = 0;
    for (;;) {
        if (atomic_load_explicit(&w->done, memory_order_acquire)) {
            bool ok = w->ok;
            if (ok) {
                snprintf(out_spool_path, out_spool_size, "%s", w->spool_path);
                *out_count = w->count;
            } else {
                remove(w->spool_path);
            }
            free(w);
            return ok;
        }
        int progress = w->progress;
        if (progress != last_seen_progress) {
            last_seen_progress = progress;
            stalled_ms = 0;
        } else {
            stalled_ms += 20;
            if (stalled_ms >= LIBRARY_SCAN_WALK_STALL_TIMEOUT_MS) break;
        }
        usleep(20000);
    }

    /* The worker may be in uninterruptible I/O. Do not touch/free its state.
     * Its uniquely named spool can be orphaned safely and removed on a later
     * maintenance pass; critically, it owns no tagcache lock or GUI memory. */
    fprintf(stderr, "Warning: scan of %s stalled with no progress for %ds (possible filesystem corruption)\n",
            root, LIBRARY_SCAN_WALK_STALL_TIMEOUT_MS / 1000);
    return false;
}

static bool scan_spool_read_path(FILE * f, char * path, size_t path_size) {
    uint32_t len = 0;
    if (fread(&len, sizeof(len), 1, f) != 1) return false;
    if (len == 0 || len >= path_size) {
        if (fseek(f, (long) len, SEEK_CUR) != 0) return false;
        path[0] = '\0';
        return true;
    }
    if (fread(path, 1, len, f) != len) return false;
    path[len] = '\0';
    return true;
}

typedef enum {
    SCAN_SONG_CACHED,   /* metadata_db_get() hit -- no parse attempted */
    SCAN_SONG_PARSED,   /* isolated read + DB upsert succeeded */
    SCAN_SONG_FAILED,   /* metadata_read_isolated() failed -- isolated child timeout/crash/malformed input */
    SCAN_SONG_NO_STAT,  /* stat() failed -- metadata was parsed but never upserted (no mtime/size to key the row by) */
} scan_song_result_t;

static scan_song_result_t scan_one_song_into_db(const char * path) {
    struct stat st;
    bool have_stat = stat(path, &st) == 0;
    int64_t mtime = have_stat ? (int64_t) st.st_mtime : 0;
    int64_t size = have_stat ? (int64_t) st.st_size : 0;

    cached_tags_t cached;
    if (have_stat && metadata_db_get(path, mtime, size, &cached)) return SCAN_SONG_CACHED;

    /* Record breadcrumb path prior to reading metadata so crash diagnostics
     * can report the specific file being parsed if an unhandled signal occurs. */
    snprintf(g_scan_last_path, sizeof(g_scan_last_path), "%s", path);

    track_metadata_t meta;
    if (!metadata_read_isolated(path, &meta, LIBRARY_SCAN_FILE_TIMEOUT_MS)) return SCAN_SONG_FAILED;

    cached_tags_t fresh;
    memset(&fresh, 0, sizeof(fresh));
    fresh.track_number = meta.has_track_number ? meta.track_number : -1;
    fresh.disc_number = meta.has_disc_number ? meta.disc_number : -1;
    snprintf(fresh.title, sizeof(fresh.title), "%s", meta.has_title ? meta.title : "");
    snprintf(fresh.artist, sizeof(fresh.artist), "%s", meta.has_artist ? meta.artist : "Unknown Artist");
    snprintf(fresh.album, sizeof(fresh.album), "%s", meta.has_album ? meta.album : "Unknown Album");
    const char * album_artist_value = meta.has_album_artist ? meta.album_artist
                                     : (meta.has_artist ? meta.artist : "Unknown Artist");
    snprintf(fresh.album_artist, sizeof(fresh.album_artist), "%s", album_artist_value);
    snprintf(fresh.genre, sizeof(fresh.genre), "%s", meta.has_genre ? meta.genre : "Unknown Genre");
    free(meta.picture_data);
    free(meta.lyrics); /* always NULL here (metadata_read_isolated() itself already frees/NULLs it before the pipe write), freeing defensively for symmetry */

    if (!have_stat) return SCAN_SONG_NO_STAT;
    metadata_db_put(path, mtime, size, &fresh);
    return SCAN_SONG_PARSED;
}

/* Overall scan progress, polled by update_timer_cb while library_rescan_active
 * is true to drive the "Updating music database..." screen's progress bar
 * (Settings > Update Music Database) -- purely cosmetic, for the user's
 * peace of mind that a rescan is actually moving rather than stuck, since
 * the underlying incremental scan (metadata_db.c's mtime/size cache) is
 * already fast on an unchanged library. _total is set once discovered_count
 * is known (library_scan_once(), before its spool-reading loop starts);
 * _done is advanced by that same loop, one file at a time (see
 * scan_one_song_into_db()'s own caller in library_scan_once()). */


/* Refreshes the persistent playlist cache (metadata_db.c) from
 * PLAYLISTS_DIR only (the SD card's Playlists folder), not a walk of the
 * whole music tree. Folded into library_scan_once() so it runs on Update
 * Music Database. Ordinary in-app create/delete update the cache with a
 * single insert/delete instead of calling this. */
void gui_library_poll_playlists(void) {
    static lv_obj_t * last_screen;
    static uint32_t last_check;
    static bool startup = true, was_storage;
    lv_obj_t * screen = lv_screen_active();
    bool visible = screen && (screen == playlists_screen || screen == add_to_playlist_screen);
    bool entered = visible && screen != last_screen;
    last_screen = screen;
    if (playlist_files_refresh_poll() && visible) {
        lv_obj_t * list = screen == playlists_screen ? playlists_list : add_to_playlist_list;
        int32_t scroll = lv_obj_get_scroll_y(list);
        if (screen == playlists_screen) populate_playlists_screen();
        else populate_add_to_playlist_screen();
        lv_obj_scroll_to_y(list, scroll, LV_ANIM_OFF);
    }
    if (!backlight_screen_is_on()) return;
#ifndef HOST_BUILD
    if (!sd_card_root_is_mounted()) return;
#endif
    uint32_t now = lv_tick_get();
    if (!startup && !entered && now - last_check < 5000) return;
    last_check = now;
    usb_mode_t mode;
    bool storage = usb_mode_control_cable_connected() && usb_mode_control_detect_current(&mode) && mode == USB_MODE_STORAGE;
    bool returned = was_storage && !storage;
    was_storage = storage;
    if (storage) return;
    if (screen == group_songs_screen && group_songs_edit_m3u_path && !group_playlist_unchanged()) {
        struct stat st;
        if (stat(group_songs_edit_m3u_path, &st) == 0) {
            int32_t scroll = lv_obj_get_scroll_y(group_songs_list);
            reload_edited_playlist();
            lv_obj_scroll_to_y(group_songs_list, scroll, LV_ANIM_OFF);
        }
    }
    if (startup || visible || returned) playlist_files_refresh_async(PLAYLISTS_DIR);
    startup = false;
}

static void rescan_playlists(void) {
    playlist_files_reconcile(PLAYLISTS_DIR);
}


void library_scan_once(void) {
    library_rescan_succeeded = false;
    bool db_logging = db_log_enabled();
    uint64_t scan_started_ms = db_logging ? db_log_now_ms() : 0;
    uint64_t phase_started_ms = scan_started_ms;
    library_scan_progress_done = 0;
    library_scan_progress_total = 0;

    DB_LOG("DB", "scan_begin root=%s rss_kb=%ld", MUSIC_ROOT_DIR, db_log_rss_kb());
    metadata_db_open();
    DB_LOG("DB", "db_open elapsed_ms=%llu songs=%lld rss_kb=%ld",
           (unsigned long long) (db_log_now_ms() - phase_started_ms), (long long) metadata_db_get_song_count(),
           db_log_rss_kb());
    phase_started_ms = db_logging ? db_log_now_ms() : 0;
    gui_books_rescan();
    DB_LOG("DB", "books_scan_end elapsed_ms=%llu rss_kb=%ld",
           (unsigned long long) (db_log_now_ms() - phase_started_ms), db_log_rss_kb());
    phase_started_ms = db_logging ? db_log_now_ms() : 0;
    rescan_playlists();
    DB_LOG("DB", "playlists_scan_end elapsed_ms=%llu rss_kb=%ld",
           (unsigned long long) (db_log_now_ms() - phase_started_ms), db_log_rss_kb());

    char spool_path[PATH_MAX] = {0};
    int discovered_count = 0;
    phase_started_ms = db_logging ? db_log_now_ms() : 0;
    if (!scan_all_songs_with_timeout(MUSIC_ROOT_DIR, spool_path, sizeof(spool_path), &discovered_count)) {
        DB_LOG("DB", "discover_failed elapsed_ms=%llu rss_kb=%ld",
               (unsigned long long) (db_log_now_ms() - phase_started_ms), db_log_rss_kb());
        return; /* preserve the last known-good in-memory + on-disk library */
    }
    DB_LOG("DB", "discover_end files=%d elapsed_ms=%llu spool=%s rss_kb=%ld",
           discovered_count, (unsigned long long) (db_log_now_ms() - phase_started_ms),
           spool_path, db_log_rss_kb());

    library_scan_progress_total = discovered_count;
    phase_started_ms = db_logging ? db_log_now_ms() : 0;
    metadata_db_begin_update();
    DB_LOG("DB", "update_begin files=%d rss_kb=%ld", discovered_count, db_log_rss_kb());

    FILE * spool = fopen(spool_path, "rb");
    if (!spool) {
        DB_LOG("DB", "spool_open_failed rss_kb=%ld path=%s", db_log_rss_kb(), spool_path);
        metadata_db_abort_update();
        remove(spool_path);
        return;
    }

    bool complete = true;
    char path[PATH_MAX];
    int done = 0;
    /* Cached once, not read fresh every song: db_log_now_ms() is a real
     * clock_gettime() syscall on this target (no vDSO), and DB_LOG's own
     * internal enabled-check can't save that cost since its arguments (the
     * elapsed-time expressions below) are already evaluated before the call
     * is even made. Skipping db_log_now_ms()/DB_LOG entirely when off is the
     * only way a 10,000+ song scan doesn't pay two syscalls per file even
     * with logging disabled (the default). */
    while (done < discovered_count) {
        if (!scan_spool_read_path(spool, path, sizeof(path))) {
            complete = false;
            break;
        }
        if (path[0] != '\0') {
            uint64_t file_started_ms = db_logging ? db_log_now_ms() : 0;
            /* No rss_kb here -- see db_log_rss_kb()'s own comment; RSS is
             * only worth the /proc read at phase boundaries, progress
             * intervals, slow files, and failures, not on every song. Plain
             * buffered logging, no per-file flush -- crash forensics for the
             * parent are already covered by g_scan_last_path (set inside
             * scan_one_song_into_db() itself, see its own comment) plus
             * main.c's crash handler, which fsyncs reload_diag.log; the
             * actual metadata parse also runs in an isolated child process
             * (metadata_read_isolated()), so a malformed file yields
             * SCAN_SONG_FAILED below rather than crashing this process. */
            if (db_logging) DB_LOG("DB_FILE", "file_begin index=%d total=%d path=%s", done, discovered_count, path);
            scan_song_result_t result = scan_one_song_into_db(path);
            if (db_logging) {
                uint64_t file_ms = db_log_now_ms() - file_started_ms;
                static const char * const scan_song_result_name[] = { "cached", "parsed", "failed", "no_stat" };
                DB_LOG("DB_FILE", "file_end index=%d result=%s elapsed_ms=%llu path=%s", done,
                       scan_song_result_name[result], (unsigned long long) file_ms, path);
                if (file_ms >= 250)
                    DB_LOG("DB", "slow_file index=%d result=%s elapsed_ms=%llu rss_kb=%ld path=%s", done,
                           scan_song_result_name[result], (unsigned long long) file_ms, db_log_rss_kb(), path);
            }
        }
        library_scan_progress_done = ++done;
        if ((done % 1000) == 0 || done == discovered_count)
            DB_LOG("DB", "tag_progress done=%d total=%d elapsed_ms=%llu rss_kb=%ld",
                   done, discovered_count, (unsigned long long) (db_log_now_ms() - phase_started_ms),
                   db_log_rss_kb());
    }
    fclose(spool);
    remove(spool_path);

    if (!complete) {
        DB_LOG("DB", "spool_read_incomplete done=%d total=%d rss_kb=%ld", done, discovered_count, db_log_rss_kb());
        metadata_db_abort_update();
        return;
    }

    bool committed = metadata_db_end_update();
    library_rescan_succeeded = committed;
    if (!committed)
        fprintf(stderr, "Warning: music database commit failed -- keeping last on-disk library\n");
    DB_LOG("DB", "commit_end ok=%d files=%d elapsed_ms=%llu songs=%lld rss_kb=%ld", committed,
           done, (unsigned long long) (db_log_now_ms() - phase_started_ms),
           (long long) metadata_db_get_song_count(), db_log_rss_kb());

    phase_started_ms = db_logging ? db_log_now_ms() : 0;
    library_load_from_cache_only();
    DB_LOG("DB", "reload_end songs=%lld elapsed_ms=%llu total_ms=%llu rss_kb=%ld",
           (long long) metadata_db_get_song_count(), (unsigned long long) (db_log_now_ms() - phase_started_ms),
           (unsigned long long) (db_log_now_ms() - scan_started_ms), db_log_rss_kb());
}

/* Boot-time equivalent of library_scan_once() that loads existing cached
 * metadata from the database without walking the filesystem or re-reading
 * file tags. The filesystem walk and tag extraction only run on explicit
 * user-triggered rescan. */
void library_load_from_cache_only(void) {
    library_scan_progress_done = 0;
    library_scan_progress_total = 0;

    /* Close first so a remounted card is not served from a still-open
     * handle against the previous (or empty unmounted) mount. */
    metadata_db_close();
    metadata_db_open();
}

bool gui_library_has_background_work(void) {
    return library_rescan_active || library_rescan_success_pending || album_thumbnail_active ||
           atomic_load(&album_thumb_gen_active) || sd_format_active || search_job_active;
}

bool gui_library_navigation_blocked(void) {
    /* These operations use a modal progress/completion screen. Thumbnail
     * decoding, post-scan cache warming, and search are deliberately absent:
     * they are cancellable/optional workers and do not own the active screen. */
    return library_rescan_active || library_rescan_success_pending || sd_format_active;
}

void gui_library_prepare_for_ui_reload(void) {
    quiesce_album_artwork_workers();

    if (search_debounce_timer) lv_timer_pause(search_debounce_timer);
    search_job_pending_valid = false;
    if (search_job_active) {
        pthread_join(search_job_thread, NULL);
        search_job_active = false;
    }
    atomic_store_explicit(&search_job_done_flag, false, memory_order_relaxed);
    search_job_for_binding = NULL;
    search_job_pending_binding = NULL;
}

void gui_library_cancel_background_work(void) {
    quiesce_album_artwork_workers();
    if (library_rescan_active) {
        pthread_join(library_rescan_thread, NULL);
        library_rescan_active = false;
        gui_busy_hide(library_rescan_token);
    }
    if (sd_format_active) {
        pthread_join(sd_format_thread, NULL);
        sd_format_active = false;
        gui_busy_hide(sd_format_token);
    }
    if (search_job_active) {
        pthread_join(search_job_thread, NULL);
        search_job_active = false;
    }
}


lv_obj_t * gui_library_get_music_screen(void) { return music_screen; }
lv_obj_t * gui_library_get_files_screen(void) { return files_screen; }
lv_obj_t * gui_library_get_all_songs_screen(void) { return all_songs_screen; }
lv_obj_t * gui_library_get_recently_added_screen(void) { return recently_added_screen; }
lv_obj_t * gui_library_get_artists_screen(void) { return artists_screen; }
lv_obj_t * gui_library_get_albums_screen(void) { return albums_screen; }
lv_obj_t * gui_library_get_album_artist_screen(void) { return album_artist_screen; }
lv_obj_t * gui_library_get_group_songs_screen(void) { return group_songs_screen; }
lv_obj_t * gui_library_get_playlists_screen(void) { return playlists_screen; }
