#include "gui.h"
#include "gui_subsonic.h"
#include "settings.h"
#include "screen_builders.h"
#include "gui_text_input.h"
#include <stdio.h>

#include "assets.h"
#include "metadata.h"
#include "subsonic_client.h"
#include "device_config.h"
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>
#include "audio.h"
#include "wifi_status.h"

void register_search(search_binding_id_t id, lv_obj_t * screen, lv_obj_t * list, const char * (*name_of)(int), const int * count_ptr, bool is_overlay_list, bool db_backed, metadata_db_az_kind_t db_kind, compact_list_fetch_page_cb_t restore_fetch_page);


extern player_settings_t current_settings;
extern gui_busy_handle_t wifi_connect_saved_token;


#define SUBSONIC_STREAM_CACHE_DIR MUSIC_ROOT_DIR "/.subsonic_cache"
#define TITLE_LABEL_LEFT_INSET 76
#define TITLE_LABEL_DEFAULT_RIGHT_MARGIN 20

extern bool playlist_files_append(const char * path, const char * dest_path);
extern int search_remap_index(search_binding_id_t id, int list_index);
extern void nav_push(lv_obj_t * screen);
extern void nav_pop(void);
extern void gui_busy_set_progress(gui_busy_handle_t handle, int percent);
extern void start_library_rescan(void);
extern void finalize_screen_navigation(lv_obj_t * screen);
extern lv_obj_t * build_confirm_popup(const char * title_text, lv_label_long_mode_t title_long_mode, lv_obj_t ** out_title, const char * body_text, const char * confirm_text, lv_color_t confirm_color, lv_event_cb_t confirm_cb, lv_obj_t ** out_confirm_row, const char * cancel_text, lv_color_t cancel_color, lv_event_cb_t cancel_cb, lv_obj_t ** out_cancel_row, lv_event_cb_t backdrop_cb, lv_obj_t ** out_backdrop);
extern lv_color_t accent_lv_color(void);


#ifdef HOST_BUILD
  #define MUSIC_ROOT_DIR "./music"
#else
  #define MUSIC_ROOT_DIR "/data/mnt/sd_0"
#endif

#define PLAYLISTS_DIR MUSIC_ROOT_DIR "/Playlists"


typedef struct {
    char url[1536];
    char dest_path[512];
    bool verify_tls;
} download_request_t;

extern bool http_get_to_file(const char * url, bool verify_tls, const char * dest_path, const char * etag, char * new_etag_out);

typedef struct {
    subsonic_server_t server;
    subsonic_song_t * songs;
    int song_count;
    subsonic_album_t * albums_to_expand;
    int album_to_expand_count;
    char playlist_name[128]; 
} subsonic_library_download_request_t;

extern bool playlist_files_create(const char * dir, const char * name, const char * initial_file, const char * out_m3u_path, size_t out_m3u_path_size);

typedef struct {
    subsonic_server_t server;
} subsonic_connect_request_t;






typedef enum {
    SUBSONIC_BROWSE_ALBUM_SONGS,
    SUBSONIC_BROWSE_ARTIST_ALBUMS,
    SUBSONIC_BROWSE_PLAYLIST_SONGS,
    SUBSONIC_BROWSE_PLAYLISTS,
    SUBSONIC_BROWSE_ALL_ALBUMS,
} subsonic_browse_kind_t;

typedef struct {
    subsonic_browse_kind_t kind;
    subsonic_server_t server;
    char id[64];
    char title[128];
} subsonic_browse_request_t;


typedef enum { SUBSONIC_DOWNLOAD_PENDING_NONE, SUBSONIC_DOWNLOAD_PENDING_SONGS, SUBSONIC_DOWNLOAD_PENDING_ARTIST } subsonic_download_pending_t;


#include <sys/stat.h>

extern void clear_player_source(void);
extern void on_file_selected(char ** files, int count, int index);
extern void nav_remove_stack_slot(int depth);

static char download_dest_path[512];
static atomic_bool download_done_flag = false;
static bool download_success_flag = false;
static bool download_active = false;
static gui_busy_handle_t download_token = 0;

static gui_busy_handle_t subsonic_library_download_token = 0;

static gui_busy_handle_t subsonic_browse_token = 0;

static gui_busy_handle_t subsonic_connect_token = 0;

static lv_obj_t * subsonic_entry_screen;

lv_obj_t * build_subsonic_list_screen(const char * default_title, lv_obj_t ** out_title_label, lv_obj_t ** out_list);

subsonic_stream_song_meta_t * subsonic_stream_meta = NULL; /* parallel array, NULL when no Subsonic stream queue is loaded */

int subsonic_stream_meta_count = 0;

void poll_subsonic_download(void);

void poll_subsonic_library_download(void);

void poll_subsonic_connect(void);

void poll_subsonic_browse(void);

bool subsonic_library_download_active;

bool subsonic_connect_active;


static subsonic_server_t subsonic_server_from_settings(void) {
    subsonic_server_t server;
    snprintf(server.base_url, sizeof(server.base_url), "%s", current_settings.subsonic_url);
    snprintf(server.username, sizeof(server.username), "%s", current_settings.subsonic_username);
    snprintf(server.password, sizeof(server.password), "%s", current_settings.subsonic_password);
    server.verify_tls = current_settings.subsonic_verify_tls;
    return server;
}

static pthread_t download_thread;

static void * download_thread_func(void * arg) {
    download_request_t * req = (download_request_t *) arg;
    bool ok = http_get_to_file(req->url, req->verify_tls, req->dest_path, NULL, NULL);
    download_success_flag = ok;
    atomic_store_explicit(&download_done_flag, true, memory_order_release); /* written last -- update_timer_cb only checks this flag */
    free(req);
    return NULL;
}

static void start_subsonic_download(const char * url, bool verify_tls, const char * dest_path, const char * display_title) {
    snprintf(download_dest_path, sizeof(download_dest_path), "%s", dest_path);

    download_request_t * req = malloc(sizeof(*req));
    if (!req) return;
    snprintf(req->url, sizeof(req->url), "%s", url);
    snprintf(req->dest_path, sizeof(req->dest_path), "%s", dest_path);
    req->verify_tls = verify_tls;

    atomic_store_explicit(&download_done_flag, false, memory_order_relaxed);
    download_success_flag = false;
    download_active = true;

    download_token = gui_busy_show("Downloading", display_title);

        if (pthread_create(&download_thread, NULL, download_thread_func, req) != 0) {
        download_active = false;
        free(req);
        gui_busy_hide(download_token);
        show_error_toast("Thread launch failed");
    }
}

void poll_subsonic_download(void) {
    if (!download_active || !atomic_load_explicit(&download_done_flag, memory_order_acquire)) return;

    download_active = false;
    pthread_join(download_thread, NULL);
    bool success = download_success_flag;

    /* Real-device bug report: a downloaded Subsonic track needed a second
     * tap on the song to actually start playing -- same race already fixed
     * for Wi-Fi manual SSID entry (see text_entry_kb_event_cb's own comment
     * for the full mechanism). on_file_selected() -> play_track_at_from()
     * nav_push()es the player screen the instant playback starts; nav_pop()
     * below (leaving the "Downloading..." screen) is an ANIMATED transition
     * queued via screen_transition_slide(), whose completion callback
     * unconditionally lv_screen_load()s back to the song list once that
     * animation finishes, a moment after the player screen was already
     * showing -- yanking the UI back even though audio was, underneath,
     * genuinely already playing.
     *
     * Real-device follow-up: simply skipping nav_pop() when on_file_
     * selected() already navigated (an earlier version of this fix) did
     * stop the yank-back, but left subsonic_downloading_screen's own slot
     * sitting in the stack forever, one level below the player screen --
     * confirmed live as backing out of the player landing back on the now-
     * defunct "Downloading..." screen instead of the song list underneath
     * it. nav_remove_stack_slot() (see its own comment) splices that slot
     * out once it's clear the player screen already took its place,
     * rather than trying to avoid creating it in the first place -- the
     * push already happened by the time this code can tell whether it
     * did. */
    int depth_before = gui_navigation_get_depth();
    if (success) {
        char ** playlist = malloc(sizeof(char *));
        playlist[0] = strdup(download_dest_path);
        clear_player_source(); /* a streamed-then-downloaded single track has no on-device list to go back to */
        on_file_selected(playlist, 1, 0);
    }
    if (gui_navigation_get_depth() > depth_before) {
        nav_remove_stack_slot(depth_before - 1);
    } else {
        gui_busy_hide(download_token);
    }
    /* else (download failed, or play_track_at_from() bailed out early via
     * external_dac_block_reason() without navigating): silently stays
     * wherever nav_pop() landed -- no error-toast UI exists yet to explain
     * a failed download (network drop, server error mid-stream, disk
     * full, ...), same gap already noted for subsonic_connect_row_cb
     * above. */
}

static void sanitize_path_component(const char * in, char * out, size_t out_size) {
    size_t pos = 0;
    for (const char * p = in; *p && pos + 1 < out_size; p++) {
        char c = *p;
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
            c = '_';
        }
        out[pos++] = c;
    }
    out[pos] = '\0';
}

static pthread_t subsonic_library_download_thread;

bool subsonic_library_download_active = false;

static atomic_bool subsonic_library_download_done_flag = false;

static atomic_int subsonic_library_download_progress = 0; /* songs completed so far */

static atomic_int subsonic_library_download_total = 0;    /* 0 while still expanding an artist's albums (Mode B) -- see poll_subsonic_library_download() */

static int subsonic_library_download_success_count = 0;

static void * subsonic_library_download_thread_func(void * arg) {
    subsonic_library_download_request_t * req = (subsonic_library_download_request_t *) arg;

    subsonic_song_t * songs = req->songs; /* Mode A: the caller's owned copy; Mode B: NULL, built below */
    int song_count = req->song_count;

    if (req->albums_to_expand) {
        int capacity = 0;
        int count = 0;
        for (int i = 0; i < req->album_to_expand_count; i++) {
            subsonic_song_t * album_songs = NULL;
            int album_song_count = 0;
            if (subsonic_get_album_songs(&req->server, req->albums_to_expand[i].id, &album_songs, &album_song_count)) {
                if (count + album_song_count > capacity) {
                    capacity = (count + album_song_count) * 2;
                    songs = realloc(songs, sizeof(subsonic_song_t) * (size_t) capacity);
                }
                memcpy(songs + count, album_songs, sizeof(subsonic_song_t) * (size_t) album_song_count);
                count += album_song_count;
            }
            free(album_songs);
        }
        song_count = count;
        subsonic_library_download_total = song_count; /* was 0 until this expansion finished -- unblocks poll_subsonic_library_download()'s progress display */
        free(req->albums_to_expand);
    }

    char playlist_m3u_path[512] = "";
    bool playlist_first = true;
    int success_count = 0;

    for (int i = 0; i < song_count; i++) {
        subsonic_song_t * song = &songs[i];

        char safe_artist[160], safe_album[160], safe_title[160];
        sanitize_path_component(song->artist[0] ? song->artist : "Unknown Artist", safe_artist, sizeof(safe_artist));
        sanitize_path_component(song->album[0] ? song->album : "Unknown Album", safe_album, sizeof(safe_album));
        sanitize_path_component(song->title[0] ? song->title : "Unknown Title", safe_title, sizeof(safe_title));

        char artist_dir[1024], album_dir[1024], dest_path[1024];
        snprintf(artist_dir, sizeof(artist_dir), "%s/%.150s", MUSIC_ROOT_DIR, safe_artist);
        mkdir(artist_dir, 0755); /* no-op (EEXIST) if it's already there, same as every other mkdir() in this file */
        snprintf(album_dir, sizeof(album_dir), "%.500s/%.150s", artist_dir, safe_album);
        mkdir(album_dir, 0755);
        if (song->track > 0) {
            snprintf(dest_path, sizeof(dest_path), "%.650s/%02d - %.150s.%.32s", album_dir, (int)song->track, safe_title, song->suffix);
        } else {
            snprintf(dest_path, sizeof(dest_path), "%.650s/%.150s.%.32s", album_dir, safe_title, song->suffix);
        }

        char url[1536];
        subsonic_build_stream_url(&req->server, song->id, url, sizeof(url));
        bool ok = http_get_to_file(url, req->server.verify_tls, dest_path, NULL, NULL);

        if (ok) {
            success_count++;
            if (req->playlist_name[0] != '\0') {
                if (playlist_first) {
                    if (playlist_files_create(PLAYLISTS_DIR, req->playlist_name, dest_path, playlist_m3u_path,
                                              sizeof(playlist_m3u_path))) {
                        metadata_db_playlist_insert_one(playlist_m3u_path);
                    }
                    playlist_first = false;
                } else if (playlist_m3u_path[0] != '\0') {
                    playlist_files_append(playlist_m3u_path, dest_path);
                }
            }
        }

        atomic_store_explicit(&subsonic_library_download_progress, i + 1, memory_order_relaxed);
    }

    subsonic_library_download_success_count = success_count;
    free(songs);
    free(req);
    atomic_store_explicit(&subsonic_library_download_done_flag, true, memory_order_release); /* written last -- poll_subsonic_library_download() only checks this flag */
    return NULL;
}

static void start_subsonic_library_download(subsonic_song_t * songs, int song_count,
                                              subsonic_album_t * albums_to_expand, int album_to_expand_count,
                                              const char * playlist_name, const char * progress_label) {
    subsonic_library_download_request_t * req = malloc(sizeof(*req));
    if (!req) return;
    req->server = subsonic_server_from_settings();
    req->songs = songs;
    req->song_count = song_count;
    req->albums_to_expand = albums_to_expand;
    req->album_to_expand_count = album_to_expand_count;
    snprintf(req->playlist_name, sizeof(req->playlist_name), "%s", playlist_name ? playlist_name : "");

    atomic_store_explicit(&subsonic_library_download_progress, 0, memory_order_relaxed);
    atomic_store_explicit(&subsonic_library_download_total, albums_to_expand ? 0 : song_count, memory_order_relaxed); /* 0 = "still figuring out the total," see poll_subsonic_library_download() */
    subsonic_library_download_success_count = 0;
    atomic_store_explicit(&subsonic_library_download_done_flag, false, memory_order_relaxed);
    subsonic_library_download_active = true;

    subsonic_library_download_token = gui_busy_show(progress_label, "");
    gui_busy_set_progress(subsonic_library_download_token, 0);

        if (pthread_create(&subsonic_library_download_thread, NULL, subsonic_library_download_thread_func, req) != 0) {
        subsonic_library_download_active = false;
        free(req);
        gui_busy_hide(subsonic_library_download_token);
        show_error_toast("Thread launch failed");
    }
}

void poll_subsonic_library_download(void) {
    if (!subsonic_library_download_active) return;

    if (!atomic_load_explicit(&subsonic_library_download_done_flag, memory_order_acquire)) {
        int total = atomic_load_explicit(&subsonic_library_download_total, memory_order_relaxed);
        if (total > 0) {
            gui_busy_set_progress(subsonic_library_download_token, (atomic_load_explicit(&subsonic_library_download_progress, memory_order_relaxed) * 100) / total);
        }
        return;
    }

    subsonic_library_download_active = false;
    pthread_join(subsonic_library_download_thread, NULL);

    /* Leave this download's own use of the shared "please wait" screen
     * before either branch below -- start_library_rescan() pushes that same
     * screen again fresh for its own "Updating music database..." phase,
     * and would otherwise stack a second copy on top of this one still
     * sitting there. */
    nav_pop();

    if (subsonic_library_download_success_count > 0) {
        /* start_library_rescan() finishes with nav_reset_to_home(), same as
         * every other "the library just changed on disk" trigger in this
         * app (SD import, Wi-Fi import, manual rescan) -- no separate
         * success toast first, the label just switches straight from this
         * download's own progress text to the rescan's. */
        start_library_rescan();
    } else {
        show_error_toast("Download failed");
    }
}

static void populate_indexed_list(lv_obj_t * list, int count, const char * (*label_of)(int), lv_event_cb_t click_cb) {
    lv_obj_clean(list);
    for (int i = 0; i < count; i++) {
        /* One lv_label via the shared list_row_style, not a container +
         * child label each with their own local style properties -- see
         * list_row_style's own doc comment (screen_builders.h). */
        lv_obj_t * row = lv_label_create(list);
        lv_obj_add_style(row, &list_row_style, 0);
        lv_obj_add_style(row, &list_row_pressed_style, LV_STATE_PRESSED);
        row_label_enable_marquee(row);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_label_set_text(row, label_of(i));

        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, click_cb, LV_EVENT_CLICKED, (void *) (intptr_t) i);
    }
}

lv_obj_t * build_subsonic_list_screen(const char * default_title, lv_obj_t ** out_title_label, lv_obj_t ** out_list) {
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
    lv_obj_align(back_arrow, LV_ALIGN_CENTER, 0, BACK_ARROW_OPTICAL_Y_OFFSET);

    lv_obj_t * title_label = lv_label_create(scr);
    lv_label_set_text(title_label, default_title);
    lv_obj_add_style(title_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(title_label, &app_font_28, 0); /* later re-set to a real (possibly non-Latin) server artist/album/song name -- see fallback_font.h */
    /* Real-device bug report: a long artist/album/playlist name (this
     * label's whole reason for existing, per the comment above) centered
     * via LV_ALIGN_TOP_MID ran directly under the back button on the left
     * and, on a screen that also has a search/download button on the
     * right (see reserve_title_width_before()'s own callers below), under
     * that too. Left-aligned starting just past the back button, with a
     * generous default width (narrowed later, per-screen, by
     * reserve_title_width_before() once that screen's own right-side
     * button actually exists) and auto-scroll for whatever still overflows
     * -- same fix as gui_library_get_group_songs_screen()'s own title (build_group_songs_screen()). */
    int32_t scr_w = lv_display_get_horizontal_resolution(lv_display_get_default());
    lv_obj_set_width(title_label, scr_w - TITLE_LABEL_LEFT_INSET - TITLE_LABEL_DEFAULT_RIGHT_MARGIN);
    /* Real-device bug report: same missing-2s-pause bug/fix as group_songs_
     * screen's own title -- see build_group_songs_screen()'s comment. */
    row_label_enable_marquee(title_label);
    lv_obj_align(title_label, LV_ALIGN_TOP_LEFT, TITLE_LABEL_LEFT_INSET, STATUS_BAR_CLEARANCE + (TITLE_ROW_HEIGHT - 28) / 2);

    lv_obj_t * list = lv_obj_create(scr);
    lv_obj_set_size(list, lv_pct(100),
                    lv_display_get_vertical_resolution(lv_display_get_default()) - STATUS_BAR_CLEARANCE -
                        TITLE_ROW_HEIGHT);
    lv_obj_align(list, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(list, 0, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    /* Real-device bug report: same root cause as build_compact_list_widget()/
     * build_pill_list_screen()'s own fix (see build_compact_list_widget()'s
     * comment) -- every screen built through this shared function (resume
     * mode, font size, USB mode, play/pause button mode, sleep timer,
     * startup volume, the timezone city list, ...) had its rows shifted
     * right, clipped against the screen edge with a gap on the left. */
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER); /* see build_icon_grid_screen's comment in screen_builders.c */
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(list, 4, 0);
    lv_obj_set_style_pad_top(list, 4, 0);

    *out_title_label = title_label;
    *out_list = list;
    finalize_screen_navigation(scr);
    return scr;
}

static subsonic_artist_t * subsonic_artists_cache = NULL;

static int subsonic_artists_count = 0;

static subsonic_album_t * subsonic_albums_cache = NULL;

static int subsonic_albums_count = 0;

static subsonic_song_t * subsonic_songs_cache = NULL;

static int subsonic_songs_count = 0;

static subsonic_playlist_t * subsonic_playlists_cache = NULL;

static int subsonic_playlists_count = 0;

static lv_obj_t * subsonic_menu_screen;

static lv_obj_t * subsonic_menu_title_label;

static lv_obj_t * subsonic_menu_list;

static lv_obj_t * subsonic_artists_screen;

static lv_obj_t * subsonic_artists_title_label;

static lv_obj_t * subsonic_artists_list;

static lv_obj_t * subsonic_albums_screen;

static lv_obj_t * subsonic_albums_title_label;

static lv_obj_t * subsonic_albums_list;

static lv_obj_t * subsonic_albums_download_btn;

static lv_obj_t * subsonic_songs_screen;

static lv_obj_t * subsonic_songs_title_label;

static lv_obj_t * subsonic_songs_list;

static lv_obj_t * subsonic_songs_download_btn;

static lv_obj_t * subsonic_playlists_screen;

static lv_obj_t * subsonic_playlists_title_label;

static lv_obj_t * subsonic_playlists_list;

static lv_obj_t * subsonic_saved_servers_screen;

static lv_obj_t * subsonic_saved_servers_list;

static lv_obj_t * subsonic_new_connection_screen;

static char subsonic_albums_context_artist[128] = "";

static bool subsonic_songs_context_is_playlist = false;

static char subsonic_songs_context_playlist_name[128] = "";

static const char * subsonic_artist_label_of(int i) { return subsonic_artists_cache[i].name; }

static const char * subsonic_album_label_of(int i) { return subsonic_albums_cache[i].name; }

static const char * subsonic_song_label_of(int i) { return subsonic_songs_cache[i].title; }

static const char * subsonic_playlist_label_of(int i) { return subsonic_playlists_cache[i].name; }

static void subsonic_fill_stream_queue_entry(const subsonic_server_t * server, const subsonic_song_t * song,
                                              char ** new_playlist, subsonic_stream_song_meta_t * new_meta, int slot) {
    char url[1536];
    subsonic_build_stream_url(server, song->id, url, sizeof(url));
    /* The "#.<suffix>" appended here is a local-only hint consumed by
     * audio.c's decoder_open() (stream_format_hint()); http_conn_parse_url()
     * strips it before it ever reaches the actual HTTP request, so it has
     * no effect on the server-facing URL. */
    size_t len = strlen(url);
    snprintf(url + len, sizeof(url) - len, "#.%s", song->suffix);

    new_playlist[slot] = strdup(url);

    subsonic_stream_song_meta_t * m = &new_meta[slot];
    snprintf(m->url, sizeof(m->url), "%s", url);
    snprintf(m->title, sizeof(m->title), "%s", song->title);
    snprintf(m->artist, sizeof(m->artist), "%s", song->artist);
    snprintf(m->album, sizeof(m->album), "%s", song->album);
    snprintf(m->suffix, sizeof(m->suffix), "%s", song->suffix);
    m->track = song->track;
    m->disc = song->disc;
    m->duration_seconds = song->duration_seconds;
    m->sample_rate = song->sample_rate;
    m->bit_depth = song->bit_depth;
    m->channels = song->channels;
    m->bitrate_kbps = song->bitrate_kbps;
    if (song->cover_art[0]) {
        subsonic_build_cover_art_url(server, song->cover_art, m->cover_url, sizeof(m->cover_url));
    } else {
        m->cover_url[0] = '\0';
    }
    m->verify_tls = server->verify_tls;
}

static void subsonic_play_song_and_queue_rest(int index) {
    subsonic_song_t * song = &subsonic_songs_cache[index];

    subsonic_server_t server = subsonic_server_from_settings();

    /* mp3/flac plays directly off the stream URL -- no download, no wait --
     * and queues the rest of this album/playlist (subsonic_songs_cache is
     * already the full song list either way, see subsonic_songs_context_is_
     * playlist's own comment above) that's ALSO mp3/flac, in original order,
     * starting at the tapped song, so Prev/Next/auto-advance/Repeat/Shuffle
     * all work across the whole thing exactly like a local-library playlist.
     * A song in some other format is simply left out of this queue -- there's
     * no good way to background-download it without either stalling the
     * queue right there or building a much bigger hybrid stream+download
     * pipeline, and skipping it keeps every other song in the album/playlist
     * reachable instead of the queue silently dead-ending on it. See this
     * section's own top comment for why a non-streamable format tapped
     * directly still downloads first, as a single track, same as before. */
    if (strcasecmp(song->suffix, "mp3") == 0 || strcasecmp(song->suffix, "flac") == 0) {
        char ** new_playlist = malloc(sizeof(char *) * (size_t) subsonic_songs_count);
        subsonic_stream_song_meta_t * new_meta = malloc(sizeof(subsonic_stream_song_meta_t) * (size_t) subsonic_songs_count);
        int count = 0;
        int start_index = -1;

        for (int i = 0; i < subsonic_songs_count; i++) {
            subsonic_song_t * s = &subsonic_songs_cache[i];
            if (strcasecmp(s->suffix, "mp3") != 0 && strcasecmp(s->suffix, "flac") != 0) continue;
            subsonic_fill_stream_queue_entry(&server, s, new_playlist, new_meta, count);
            if (i == index) start_index = count;
            count++;
        }

        if (start_index < 0) {
            /* Shouldn't happen -- the tapped song itself was mp3/flac, so it
             * must have been included above -- but fail safely rather than
             * play the wrong track if this invariant is ever violated. */
            for (int i = 0; i < count; i++) free(new_playlist[i]);
            free(new_playlist);
            free(new_meta);
            return;
        }

        free(subsonic_stream_meta);
        subsonic_stream_meta = new_meta;
        subsonic_stream_meta_count = count;

        clear_player_source(); /* a streamed queue has no on-device list to go back to */
        on_file_selected(new_playlist, count, start_index);
        return;
    }

    char url[1536];
    subsonic_build_stream_url(&server, song->id, url, sizeof(url));
    mkdir(SUBSONIC_STREAM_CACHE_DIR, 0755); /* no-op (EEXIST) if it's already there */
    char dest[256];
    snprintf(dest, sizeof(dest), SUBSONIC_STREAM_CACHE_DIR "/stream.%s", song->suffix);

    start_subsonic_download(url, server.verify_tls, dest, song->title);
}

static void subsonic_song_row_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    subsonic_play_song_and_queue_rest(index);
}

static void subsonic_album_row_click_cb(int index);

static void subsonic_playlist_row_click_cb(lv_event_t * e);

static pthread_t subsonic_browse_thread;

static bool subsonic_browse_active = false;

static atomic_bool subsonic_browse_done_flag = false;

static volatile bool subsonic_browse_success_flag = false;

static subsonic_browse_kind_t subsonic_browse_result_kind;

static char subsonic_browse_result_title[128];

static subsonic_song_t * subsonic_browse_result_songs = NULL;

static subsonic_album_t * subsonic_browse_result_albums = NULL;

static subsonic_playlist_t * subsonic_browse_result_playlists = NULL;

static int subsonic_browse_result_count = 0;

static void * subsonic_browse_thread_func(void * arg) {
    subsonic_browse_request_t * req = (subsonic_browse_request_t *) arg;
    bool ok = false;
    int count = 0;

    switch (req->kind) {
        case SUBSONIC_BROWSE_ALBUM_SONGS:
            ok = subsonic_get_album_songs(&req->server, req->id, &subsonic_browse_result_songs, &count);
            break;
        case SUBSONIC_BROWSE_ARTIST_ALBUMS:
            ok = subsonic_get_artist_albums(&req->server, req->id, &subsonic_browse_result_albums, &count);
            break;
        case SUBSONIC_BROWSE_PLAYLIST_SONGS:
            ok = subsonic_get_playlist_songs(&req->server, req->id, &subsonic_browse_result_songs, &count);
            break;
        case SUBSONIC_BROWSE_PLAYLISTS:
            ok = subsonic_get_playlists(&req->server, &subsonic_browse_result_playlists, &count);
            break;
        case SUBSONIC_BROWSE_ALL_ALBUMS:
            ok = subsonic_get_all_albums(&req->server, &subsonic_browse_result_albums, &count);
            break;
    }

    subsonic_browse_result_kind = req->kind;
    snprintf(subsonic_browse_result_title, sizeof(subsonic_browse_result_title), "%s", req->title);
    subsonic_browse_result_count = count;
    subsonic_browse_success_flag = ok;
    atomic_store_explicit(&subsonic_browse_done_flag, true, memory_order_release); free(req);
    return NULL;
}

static void start_subsonic_browse(subsonic_browse_kind_t kind, const char * id, const char * title) {
    if (subsonic_browse_active) return;

    subsonic_browse_request_t * req = calloc(1, sizeof(*req));
    if (!req) return;
    req->kind = kind;
    req->server = subsonic_server_from_settings();
    if (id) snprintf(req->id, sizeof(req->id), "%s", id);
    if (title) snprintf(req->title, sizeof(req->title), "%s", title);

    subsonic_browse_result_songs = NULL;
    subsonic_browse_result_albums = NULL;
    subsonic_browse_result_playlists = NULL;
    subsonic_browse_result_count = 0;
    atomic_store_explicit(&subsonic_browse_done_flag, false, memory_order_relaxed);
    subsonic_browse_success_flag = false;
    subsonic_browse_active = true;

    subsonic_browse_token = gui_busy_show("Loading from server...", "");
    if (pthread_create(&subsonic_browse_thread, NULL, subsonic_browse_thread_func, req) != 0) {
        subsonic_browse_active = false;
        free(req);
        gui_busy_hide(subsonic_browse_token);
    }
}

void poll_subsonic_browse(void) {
    if (!subsonic_browse_active || !atomic_load_explicit(&subsonic_browse_done_flag, memory_order_acquire)) return;

    subsonic_browse_active = false;
    pthread_join(subsonic_browse_thread, NULL);
    bool success = subsonic_browse_success_flag;
    int depth_before = gui_navigation_get_depth();

    if (success) {
        switch (subsonic_browse_result_kind) {
            case SUBSONIC_BROWSE_ALBUM_SONGS:
            case SUBSONIC_BROWSE_PLAYLIST_SONGS:
                free(subsonic_songs_cache);
                subsonic_songs_cache = subsonic_browse_result_songs;
                subsonic_songs_count = subsonic_browse_result_count;
                subsonic_songs_context_is_playlist =
                    subsonic_browse_result_kind == SUBSONIC_BROWSE_PLAYLIST_SONGS;
                if (subsonic_songs_context_is_playlist) {
                    snprintf(subsonic_songs_context_playlist_name, sizeof(subsonic_songs_context_playlist_name), "%s",
                             subsonic_browse_result_title);
                }
                lv_label_set_text(subsonic_songs_title_label, subsonic_browse_result_title);
                lv_obj_clear_flag(subsonic_songs_download_btn, LV_OBJ_FLAG_HIDDEN);
                populate_indexed_list(subsonic_songs_list, subsonic_songs_count, subsonic_song_label_of,
                                      subsonic_song_row_click_cb);
                nav_push(subsonic_songs_screen);
                break;
            case SUBSONIC_BROWSE_ARTIST_ALBUMS:
            case SUBSONIC_BROWSE_ALL_ALBUMS:
                free(subsonic_albums_cache);
                subsonic_albums_cache = subsonic_browse_result_albums;
                subsonic_albums_count = subsonic_browse_result_count;
                if (subsonic_browse_result_kind == SUBSONIC_BROWSE_ARTIST_ALBUMS) {
                    snprintf(subsonic_albums_context_artist, sizeof(subsonic_albums_context_artist), "%s",
                             subsonic_browse_result_title);
                    lv_obj_clear_flag(subsonic_albums_download_btn, LV_OBJ_FLAG_HIDDEN);
                } else {
                    subsonic_albums_context_artist[0] = '\0';
                    lv_obj_add_flag(subsonic_albums_download_btn, LV_OBJ_FLAG_HIDDEN);
                }
                lv_label_set_text(subsonic_albums_title_label, subsonic_browse_result_title);
                {
                    /* getAlbumList2.view's own "every album" browse can return
                     * up to 500 rows (see subsonic_client.c's own size=500) --
                     * real widget-explosion risk populate_indexed_list() used
                     * to hit here, same class as the local library's pre-
                     * virtualization All Songs/Artists/Albums screens. */
                    compact_list_item_t * items =
                        malloc(sizeof(compact_list_item_t) * (size_t) (subsonic_albums_count > 0 ? subsonic_albums_count : 1));
                    for (int i = 0; i < subsonic_albums_count; i++) items[i] = (compact_list_item_t){ subsonic_album_label_of(i) };
                    compact_list_set_items(subsonic_albums_list, items, subsonic_albums_count);
                    free(items);
                }
                nav_push(subsonic_albums_screen);
                break;
            case SUBSONIC_BROWSE_PLAYLISTS:
                free(subsonic_playlists_cache);
                subsonic_playlists_cache = subsonic_browse_result_playlists;
                subsonic_playlists_count = subsonic_browse_result_count;
                populate_indexed_list(subsonic_playlists_list, subsonic_playlists_count, subsonic_playlist_label_of,
                                      subsonic_playlist_row_click_cb);
                nav_push(subsonic_playlists_screen);
                break;
        }
    } else {
        free(subsonic_browse_result_songs);
        free(subsonic_browse_result_albums);
        free(subsonic_browse_result_playlists);
    }

    if (gui_navigation_get_depth() > depth_before) nav_remove_stack_slot(depth_before - 1);
    else nav_pop();
}

static void subsonic_album_row_click_cb(int index) {
    index = search_remap_index(SEARCH_BINDING_SUBSONIC_ALBUMS, index);

    if (index < 0 || index >= subsonic_albums_count) return;
    start_subsonic_browse(SUBSONIC_BROWSE_ALBUM_SONGS, subsonic_albums_cache[index].id,
                          subsonic_albums_cache[index].name);
}

static void subsonic_artist_row_click_cb(int index) {
    index = search_remap_index(SEARCH_BINDING_SUBSONIC_ARTISTS, index);

    if (index < 0 || index >= subsonic_artists_count) return;
    start_subsonic_browse(SUBSONIC_BROWSE_ARTIST_ALBUMS, subsonic_artists_cache[index].id,
                          subsonic_artists_cache[index].name);
}

static void subsonic_playlist_row_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);

    if (index < 0 || index >= subsonic_playlists_count) return;
    start_subsonic_browse(SUBSONIC_BROWSE_PLAYLIST_SONGS, subsonic_playlists_cache[index].id,
                          subsonic_playlists_cache[index].name);
}

static void subsonic_menu_artists_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    /* Already fetched/populated at connect time (poll_subsonic_connect()) --
     * no re-fetch needed here, matching every other "browse from cache"
     * screen entry in this app. */
    nav_push(subsonic_artists_screen);
}

static void subsonic_menu_playlists_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    start_subsonic_browse(SUBSONIC_BROWSE_PLAYLISTS, NULL, "Playlists");
}

static void subsonic_menu_albums_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    start_subsonic_browse(SUBSONIC_BROWSE_ALL_ALBUMS, NULL, "Albums");
}

static subsonic_download_pending_t subsonic_download_pending = SUBSONIC_DOWNLOAD_PENDING_NONE;

static lv_obj_t * subsonic_download_confirm_popup;

static lv_obj_t * subsonic_download_confirm_popup_backdrop;

static lv_obj_t * subsonic_download_confirm_title;

static void hide_subsonic_download_confirm_popup(void) {
    lv_obj_add_flag(subsonic_download_confirm_popup_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(subsonic_download_confirm_popup, LV_OBJ_FLAG_HIDDEN);
    subsonic_download_pending = SUBSONIC_DOWNLOAD_PENDING_NONE;
}

static void subsonic_download_confirm_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_subsonic_download_confirm_popup();
}

static void subsonic_download_confirm_cancel_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_subsonic_download_confirm_popup();
}

static void show_subsonic_download_confirm_popup(subsonic_download_pending_t kind, const char * msg) {
    subsonic_download_pending = kind;
    lv_label_set_text(subsonic_download_confirm_title, msg);
    lv_obj_remove_flag(subsonic_download_confirm_popup_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(subsonic_download_confirm_popup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(subsonic_download_confirm_popup_backdrop);
    lv_obj_move_foreground(subsonic_download_confirm_popup);
}

static void subsonic_download_songs_now(void) {
    if (subsonic_songs_count == 0) return;

    subsonic_song_t * songs_copy = malloc(sizeof(subsonic_song_t) * (size_t) subsonic_songs_count);
    memcpy(songs_copy, subsonic_songs_cache, sizeof(subsonic_song_t) * (size_t) subsonic_songs_count);

    const char * playlist_name = subsonic_songs_context_is_playlist ? subsonic_songs_context_playlist_name : NULL;
    char label[192];
    snprintf(label, sizeof(label), "Downloading\n%s...", lv_label_get_text(subsonic_songs_title_label));
    start_subsonic_library_download(songs_copy, subsonic_songs_count, NULL, 0, playlist_name, label);
}

static void subsonic_download_songs_btn_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (subsonic_songs_count == 0) return;

    char msg[224];
    snprintf(msg, sizeof(msg), "Download \"%s\"?", lv_label_get_text(subsonic_songs_title_label));
    show_subsonic_download_confirm_popup(SUBSONIC_DOWNLOAD_PENDING_SONGS, msg);
}

static void subsonic_download_artist_now(void) {
    if (subsonic_albums_context_artist[0] == '\0' || subsonic_albums_count == 0) return;

    subsonic_album_t * albums_copy = malloc(sizeof(subsonic_album_t) * (size_t) subsonic_albums_count);
    memcpy(albums_copy, subsonic_albums_cache, sizeof(subsonic_album_t) * (size_t) subsonic_albums_count);

    char label[192];
    snprintf(label, sizeof(label), "Downloading\n%s...", subsonic_albums_context_artist);
    start_subsonic_library_download(NULL, 0, albums_copy, subsonic_albums_count, NULL, label);
}

static void subsonic_download_artist_btn_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (subsonic_albums_context_artist[0] == '\0' || subsonic_albums_count == 0) return;

    char msg[224];
    snprintf(msg, sizeof(msg), "Download every album from \"%s\"?", subsonic_albums_context_artist);
    show_subsonic_download_confirm_popup(SUBSONIC_DOWNLOAD_PENDING_ARTIST, msg);
}

static void subsonic_download_confirm_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    subsonic_download_pending_t kind = subsonic_download_pending;
    hide_subsonic_download_confirm_popup();
    if (kind == SUBSONIC_DOWNLOAD_PENDING_SONGS) subsonic_download_songs_now();
    else if (kind == SUBSONIC_DOWNLOAD_PENDING_ARTIST) subsonic_download_artist_now();
}

static void build_subsonic_download_confirm_popup(void) {
    subsonic_download_confirm_popup = build_confirm_popup(
        "", LV_LABEL_LONG_WRAP, &subsonic_download_confirm_title, NULL, "Download", accent_lv_color(),
        subsonic_download_confirm_cb, NULL, "Cancel", lv_color_make(160, 160, 160), subsonic_download_confirm_cancel_cb,
        NULL, subsonic_download_confirm_backdrop_cb, &subsonic_download_confirm_popup_backdrop);
}

static pthread_t subsonic_connect_thread;

bool subsonic_connect_active = false;

static atomic_bool subsonic_connect_done_flag = false;

static volatile bool subsonic_connect_success_flag = false;

static subsonic_server_t subsonic_connect_pending_server;

static void * subsonic_connect_thread_func(void * arg) {
    subsonic_connect_request_t * req = (subsonic_connect_request_t *) arg;

    bool ok = subsonic_ping(&req->server);
    if (ok) {
        free(subsonic_artists_cache);
        subsonic_artists_cache = NULL;
        subsonic_artists_count = 0;
        ok = subsonic_get_artists(&req->server, &subsonic_artists_cache, &subsonic_artists_count);
    }

    subsonic_connect_success_flag = ok;
    atomic_store_explicit(&subsonic_connect_done_flag, true, memory_order_release); /* written last -- poll_subsonic_connect only checks this flag */
    free(req);
    return NULL;
}

void poll_subsonic_connect(void) {
    if (!subsonic_connect_active || !atomic_load_explicit(&subsonic_connect_done_flag, memory_order_acquire)) return;

    subsonic_connect_active = false;
    pthread_join(subsonic_connect_thread, NULL);
    bool success = subsonic_connect_success_flag;

    /* Same nav_pop()-races-a-navigating-callback issue already fixed for
     * Wi-Fi manual SSID entry and the Subsonic download-then-play path --
     * see text_entry_kb_event_cb's own comment for the full mechanism, and
     * poll_subsonic_download()'s own comment for why skipping nav_pop()
     * alone isn't enough either (it leaves this screen's own stack slot
     * stuck underneath the artists screen forever) -- nav_remove_stack_
     * slot() splices it out instead, once it's clear the artists screen
     * already took its place. */
    int depth_before = gui_navigation_get_depth();
    if (success) {
        /* Becomes the active connection everything else in this app's
         * Subsonic browsing/download flow reads via subsonic_server_from_
         * settings() -- Saved Servers and New Connection both funnel
         * through this one function, so this is the one place that needs
         * to update it, regardless of which screen the user connected
         * from. Also persisted to the Saved Servers list itself (an
         * upsert, so reconnecting to an already-saved server with updated
         * credentials just refreshes it rather than duplicating it). */
        snprintf(current_settings.subsonic_url, sizeof(current_settings.subsonic_url), "%s",
                 subsonic_connect_pending_server.base_url);
        snprintf(current_settings.subsonic_username, sizeof(current_settings.subsonic_username), "%s",
                 subsonic_connect_pending_server.username);
        snprintf(current_settings.subsonic_password, sizeof(current_settings.subsonic_password), "%s",
                 subsonic_connect_pending_server.password);
        current_settings.subsonic_verify_tls = subsonic_connect_pending_server.verify_tls;
        metadata_db_subsonic_server_save(subsonic_connect_pending_server.base_url, subsonic_connect_pending_server.username,
                                          subsonic_connect_pending_server.password, subsonic_connect_pending_server.verify_tls);
        settings_subsonic_server_upsert(&current_settings, subsonic_connect_pending_server.base_url,
                                        subsonic_connect_pending_server.username, subsonic_connect_pending_server.password,
                                        subsonic_connect_pending_server.verify_tls);
        settings_save(&current_settings);

        /* Subsonic screen redesign: lands on a menu (Artists/Playlists/
         * Albums) rather than jumping straight into the artist list --
         * getArtists is still fetched right here as part of connecting
         * (below), so tapping Artists from the menu is instant with no
         * extra round trip; Playlists/Albums fetch lazily on their own tap,
         * same as every other drill-down in this screen already does.
         * getArtists.view has no size cap (unlike getAlbumList2's own
         * size=500) -- a large self-hosted server's whole artist list is a
         * real widget-explosion risk populate_indexed_list() used to hit
         * here, same class as the local library's pre-virtualization All
         * Songs/Artists/Albums screens. */
        {
            compact_list_item_t * items =
                malloc(sizeof(compact_list_item_t) * (size_t) (subsonic_artists_count > 0 ? subsonic_artists_count : 1));
            for (int i = 0; i < subsonic_artists_count; i++) items[i] = (compact_list_item_t){ subsonic_artist_label_of(i) };
            compact_list_set_items(subsonic_artists_list, items, subsonic_artists_count);
            free(items);
        }
        nav_push(subsonic_menu_screen);
    }
    if (gui_navigation_get_depth() > depth_before) {
        nav_remove_stack_slot(depth_before - 1);
    } else {
        gui_busy_hide(wifi_connect_saved_token);
    }
    /* else (failure): silently stays wherever nav_pop() landed -- same
     * documented gap as poll_subsonic_download above. */
}

/* Bug report: opening Subsonic (or attempting a connection from within it)
 * with no real Wi-Fi connection established just sat in the "Connecting to
 * server..." busy overlay until subsonic_client.h's own HTTP timeout, then
 * -- per this file's own documented gap just above -- silently landed back
 * wherever nav_pop() left off, with no indication anything failed. Unlike
 * the four Wireless tiles' own guard (gui_network.c's wifi_feature_guard(),
 * which only requires the RADIO to be on -- Wi-Fi ON but disconnected is
 * fine for those, since they're local-network features with their own
 * "connect first" in-screen state), Subsonic talks to a remote server, so
 * radio-on alone isn't enough: this checks wifi_get_status(), the same
 * "wpa_state=COMPLETED" real-association check the topbar's own Wi-Fi icon
 * uses, already an accepted synchronous-on-the-UI-thread call elsewhere in
 * this codebase (refresh_wifi_icon(), gui_shell.c) since it's a single fast
 * subprocess call, not a network round-trip of its own. Checked at the
 * tile (don't even open the entry screen) AND here in start_subsonic_
 * connect() (the actual single choke point both Saved Servers and New
 * Connection's own "Connect & Browse" already funnel through) -- the
 * screen can already be open from before Wi-Fi dropped, same reasoning as
 * every other Wi-Fi-dependent feature's defensive enable guard. */
static bool subsonic_wifi_connected_guard(void) {
    int level;
    if (wifi_get_status(&level)) return true;
    show_error_toast("Connect to Wi-Fi first");
    return false;
}

static void start_subsonic_connect(const subsonic_server_t * server) {
    if (!subsonic_wifi_connected_guard()) return;
    subsonic_connect_pending_server = *server;

    subsonic_connect_request_t * req = malloc(sizeof(*req));
    if (!req) return;
    req->server = *server;

    atomic_store_explicit(&subsonic_connect_done_flag, false, memory_order_relaxed);
    subsonic_connect_success_flag = false;
    subsonic_connect_active = true;

    subsonic_connect_token = gui_busy_show("Connecting to server...", "");

    if (pthread_create(&subsonic_connect_thread, NULL, subsonic_connect_thread_func, req) != 0) {
        free(req);
        subsonic_connect_active = false;
        gui_busy_dismiss(subsonic_connect_token);
        subsonic_connect_token = 0;
        show_info_toast("Failed to start connection");
    }
}

static subsonic_server_row_t * subsonic_saved_servers = NULL;

static int subsonic_saved_server_count = 0;

static void subsonic_saved_server_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    subsonic_server_row_t * row = &subsonic_saved_servers[index];

    subsonic_server_t server;
    snprintf(server.base_url, sizeof(server.base_url), "%s", row->url);
    snprintf(server.username, sizeof(server.username), "%s", row->username);
    snprintf(server.password, sizeof(server.password), "%s", row->password);
    server.verify_tls = row->verify_tls;
    start_subsonic_connect(&server);
}

static const char * subsonic_saved_server_label_of(int i) { return subsonic_saved_servers[i].url; }

static void populate_subsonic_saved_servers_screen(void) {
    free(subsonic_saved_servers);
    subsonic_saved_servers = NULL;
    subsonic_saved_server_count = 0;
    metadata_db_load_subsonic_servers(&subsonic_saved_servers, &subsonic_saved_server_count);

    lv_obj_clean(subsonic_saved_servers_list);
    if (subsonic_saved_server_count == 0) {
        lv_obj_t * label = lv_label_create(subsonic_saved_servers_list);
        lv_label_set_text(label, "No saved servers yet");
        lv_obj_add_style(label, &style_theme_text_muted, 0);
        lv_obj_set_style_pad_left(label, 24, 0);
        return;
    }
    populate_indexed_list(subsonic_saved_servers_list, subsonic_saved_server_count, subsonic_saved_server_label_of,
                          subsonic_saved_server_row_cb);
}

static lv_obj_t * build_subsonic_saved_servers_screen(void) {
    lv_obj_t * title_label; /* unused after build -- title never changes */
    return build_subsonic_list_screen("Saved Servers", &title_label, &subsonic_saved_servers_list);
}

static void subsonic_saved_servers_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    populate_subsonic_saved_servers_screen();
    nav_push(subsonic_saved_servers_screen);
}

static subsonic_server_t subsonic_new_conn_form = { .verify_tls = true };

static lv_obj_t * subsonic_new_connection_list;

static void populate_subsonic_new_connection_screen(void); /* defined below, after the row callbacks it references */

static void subsonic_new_conn_url_entry_done(const char * text, void * user_data) {
    (void) user_data;
    snprintf(subsonic_new_conn_form.base_url, sizeof(subsonic_new_conn_form.base_url), "%s", text);
    populate_subsonic_new_connection_screen();
}

static void subsonic_new_conn_url_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    show_text_entry("Server URL (e.g. https://myserver:4040)", subsonic_new_conn_form.base_url, false, false,
                    subsonic_new_conn_url_entry_done, NULL);
}

static void subsonic_new_conn_username_entry_done(const char * text, void * user_data) {
    (void) user_data;
    snprintf(subsonic_new_conn_form.username, sizeof(subsonic_new_conn_form.username), "%s", text);
    populate_subsonic_new_connection_screen();
}

static void subsonic_new_conn_username_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    show_text_entry("Username", subsonic_new_conn_form.username, false, false, subsonic_new_conn_username_entry_done, NULL);
}

static void subsonic_new_conn_password_entry_done(const char * text, void * user_data) {
    (void) user_data;
    snprintf(subsonic_new_conn_form.password, sizeof(subsonic_new_conn_form.password), "%s", text);
    populate_subsonic_new_connection_screen();
}

static void subsonic_new_conn_password_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    show_text_entry("Password", subsonic_new_conn_form.password, true, false, subsonic_new_conn_password_entry_done, NULL);
}

static void subsonic_new_conn_verify_tls_toggle_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    subsonic_new_conn_form.verify_tls = !subsonic_new_conn_form.verify_tls;
    populate_subsonic_new_connection_screen();
}

static void subsonic_new_conn_connect_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    start_subsonic_connect(&subsonic_new_conn_form);
}

static void populate_subsonic_new_connection_screen(void) {
    lv_obj_clean(subsonic_new_connection_list);

    char url_text[300];
    snprintf(url_text, sizeof(url_text), "Server URL: %s",
             subsonic_new_conn_form.base_url[0] ? subsonic_new_conn_form.base_url : "Not set");
    add_pill_chevron_row(subsonic_new_connection_list, url_text, subsonic_new_conn_url_row_cb);

    add_pill_toggle_row(subsonic_new_connection_list, "Verify server certificate", subsonic_new_conn_form.verify_tls,
                        subsonic_new_conn_verify_tls_toggle_cb);

    char username_text[160];
    snprintf(username_text, sizeof(username_text), "Username: %s",
             subsonic_new_conn_form.username[0] ? subsonic_new_conn_form.username : "Not set");
    add_pill_chevron_row(subsonic_new_connection_list, username_text, subsonic_new_conn_username_row_cb);

    add_pill_chevron_row(subsonic_new_connection_list, subsonic_new_conn_form.password[0] ? "Password: Set" : "Password: Not set",
                         subsonic_new_conn_password_row_cb);

    add_pill_chevron_row(subsonic_new_connection_list, "Connect & Browse", subsonic_new_conn_connect_row_cb);
}

static lv_obj_t * build_subsonic_new_connection_screen(void) {
    lv_obj_t * title_label; /* unused after build -- title never changes */
    return build_subsonic_list_screen("New Connection", &title_label, &subsonic_new_connection_list);
}

static void subsonic_new_connection_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    /* Only the text fields reset -- verify_tls deliberately carries over
     * between visits (no widget-desync risk here anymore, unlike the old
     * build_pill_list_screen()-based version -- this screen's rows are
     * fully rebuilt by populate_subsonic_new_connection_screen() below on
     * every visit, so there's nothing stale left to desync from). */
    subsonic_new_conn_form.base_url[0] = '\0';
    subsonic_new_conn_form.username[0] = '\0';
    subsonic_new_conn_form.password[0] = '\0';
    populate_subsonic_new_connection_screen();
    nav_push(subsonic_new_connection_screen);
}

static lv_obj_t * build_subsonic_entry_screen(void) {
    static pill_list_item_t items[2];
    items[0] = (pill_list_item_t){ "Saved Servers", PILL_ACCESSORY_CHEVRON, false, subsonic_saved_servers_row_cb, NULL, NULL };
    items[1] = (pill_list_item_t){ "New Connection", PILL_ACCESSORY_CHEVRON, false, subsonic_new_connection_row_cb, NULL, NULL };
    lv_obj_t * scr = build_pill_list_screen("Subsonic", generic_back_cb, items, 2, gui_theme_accent_style(), 6);
    finalize_screen_navigation(scr);
    return scr;
}

void subsonic_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!subsonic_wifi_connected_guard()) return;
    nav_push(subsonic_entry_screen);
}

void gui_subsonic_init(void) {
    subsonic_entry_screen = build_subsonic_entry_screen();
    subsonic_saved_servers_screen = build_subsonic_saved_servers_screen();
    subsonic_new_connection_screen = build_subsonic_new_connection_screen();

    /* Subsonic screen redesign: the menu (Artists/Playlists/Albums) that's
     * now the first screen after connecting -- see poll_subsonic_connect().
     * Rows built once here, not repopulated per visit, since this list
     * never changes. */
    subsonic_menu_screen = build_subsonic_list_screen("Subsonic", &subsonic_menu_title_label, &subsonic_menu_list);
    {
        lv_obj_t * artists_row = add_pill_row_base(subsonic_menu_list, "Artists");
        lv_obj_set_style_text_font(lv_obj_get_child(artists_row, 0), gui_theme_font(GUI_FONT_ROLE_BODY), 0);
        lv_obj_add_flag(artists_row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(artists_row, subsonic_menu_artists_row_cb, LV_EVENT_CLICKED, NULL);

        lv_obj_t * playlists_row = add_pill_row_base(subsonic_menu_list, "Playlists");
        lv_obj_set_style_text_font(lv_obj_get_child(playlists_row, 0), gui_theme_font(GUI_FONT_ROLE_BODY), 0);
        lv_obj_add_flag(playlists_row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(playlists_row, subsonic_menu_playlists_row_cb, LV_EVENT_CLICKED, NULL);

        lv_obj_t * albums_row = add_pill_row_base(subsonic_menu_list, "Albums");
        lv_obj_set_style_text_font(lv_obj_get_child(albums_row, 0), gui_theme_font(GUI_FONT_ROLE_BODY), 0);
        lv_obj_add_flag(albums_row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(albums_row, subsonic_menu_albums_row_cb, LV_EVENT_CLICKED, NULL);
    }

    subsonic_artists_screen = build_compact_list_screen("Artists", generic_back_cb, NULL, 0, subsonic_artist_row_click_cb,
                                                          NULL, &subsonic_artists_list, &subsonic_artists_title_label,
                                                          LIST_ROW_WIDTH_WIDE, false, lv_color_black());
    subsonic_albums_screen = build_compact_list_screen("Albums", generic_back_cb, NULL, 0, subsonic_album_row_click_cb,
                                                         NULL, &subsonic_albums_list, &subsonic_albums_title_label,
                                                         LIST_ROW_WIDTH_WIDE, false, lv_color_black());
    /* Real-device bug report: swipe-back not working inside Subsonic's
     * Artists/Albums submenus. Unlike build_subsonic_list_screen() (used by
     * the sibling Songs/Playlists submenus just below), build_compact_list_
     * screen() does not call finalize_screen_navigation() itself -- every
     * other caller (gui_library.c's own Artists/Albums/All Songs/...) calls
     * it explicitly afterward, but these two never did. Without it, the
     * screen root never gets LV_OBJ_FLAG_CLICKABLE, keeps LV_OBJ_FLAG_
     * SCROLLABLE, and never gets the LV_EVENT_GESTURE handler registered --
     * so a swipe never even registers as a gesture here, let alone
     * triggers Back. */
    finalize_screen_navigation(subsonic_artists_screen);
    finalize_screen_navigation(subsonic_albums_screen);
    subsonic_songs_screen = build_subsonic_list_screen("Songs", &subsonic_songs_title_label, &subsonic_songs_list);
    subsonic_playlists_screen = build_subsonic_list_screen("Playlists", &subsonic_playlists_title_label, &subsonic_playlists_list);

    subsonic_albums_download_btn = lv_image_create(subsonic_albums_screen);
    lv_image_set_src(subsonic_albums_download_btn, asset_path("stream_media/download.png"));
    lv_obj_set_style_image_recolor(subsonic_albums_download_btn, accent_lv_color(), 0);
    lv_obj_set_style_image_recolor_opa(subsonic_albums_download_btn, LV_OPA_COVER, 0);
    lv_obj_align(subsonic_albums_download_btn, LV_ALIGN_TOP_RIGHT, -87, STATUS_BAR_CLEARANCE + (TITLE_ROW_HEIGHT - 34) / 2);
    lv_obj_set_ext_click_area(subsonic_albums_download_btn, 16);
    lv_obj_add_flag(subsonic_albums_download_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(subsonic_albums_download_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(subsonic_albums_download_btn, subsonic_download_artist_btn_cb, LV_EVENT_CLICKED, NULL);
    reserve_title_width_before(subsonic_albums_title_label, subsonic_albums_download_btn);

    subsonic_songs_download_btn = lv_image_create(subsonic_songs_screen);
    lv_image_set_src(subsonic_songs_download_btn, asset_path("stream_media/download.png"));
    lv_obj_set_style_image_recolor(subsonic_songs_download_btn, accent_lv_color(), 0);
    lv_obj_set_style_image_recolor_opa(subsonic_songs_download_btn, LV_OPA_COVER, 0);
    lv_obj_align(subsonic_songs_download_btn, LV_ALIGN_TOP_RIGHT, -20, STATUS_BAR_CLEARANCE + (TITLE_ROW_HEIGHT - 34) / 2);
    lv_obj_set_ext_click_area(subsonic_songs_download_btn, 16);
    lv_obj_add_flag(subsonic_songs_download_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(subsonic_songs_download_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(subsonic_songs_download_btn, subsonic_download_songs_btn_cb, LV_EVENT_CLICKED, NULL);
    reserve_title_width_before(subsonic_songs_title_label, subsonic_songs_download_btn);

    build_subsonic_download_confirm_popup();

    register_search(SEARCH_BINDING_SUBSONIC_ARTISTS, subsonic_artists_screen, subsonic_artists_list, subsonic_artist_label_of,
                     &subsonic_artists_count, false, false, METADATA_DB_AZ_ALL_SONGS, NULL);
    register_search(SEARCH_BINDING_SUBSONIC_ALBUMS, subsonic_albums_screen, subsonic_albums_list, subsonic_album_label_of,
                     &subsonic_albums_count, false, false, METADATA_DB_AZ_ALL_SONGS, NULL);
}

/* For gui_reload.c's in-process UI reload -- deletes every screen this
 * module owns so gui_subsonic_init() can rebuild them from a clean slate
 * without leaking the old objects. subsonic_download_confirm_popup and its
 * backdrop are built directly on lv_layer_top() (see build_confirm_popup()'s
 * own comment), not as children of any of these screens, so they need their
 * own explicit deletion. re-running gui_subsonic_init() also re-runs
 * register_search() for the two virtualized lists below, which already
 * frees its own prior state on re-registration (gui_library.c) -- nothing
 * extra needed here for that. */
void gui_subsonic_teardown(void) {
    if (subsonic_download_confirm_popup) { lv_obj_del(subsonic_download_confirm_popup); subsonic_download_confirm_popup = NULL; }
    if (subsonic_download_confirm_popup_backdrop) {
        lv_obj_del(subsonic_download_confirm_popup_backdrop);
        subsonic_download_confirm_popup_backdrop = NULL;
    }
    if (subsonic_entry_screen) { lv_obj_del(subsonic_entry_screen); subsonic_entry_screen = NULL; }
    if (subsonic_saved_servers_screen) { lv_obj_del(subsonic_saved_servers_screen); subsonic_saved_servers_screen = NULL; }
    if (subsonic_new_connection_screen) { lv_obj_del(subsonic_new_connection_screen); subsonic_new_connection_screen = NULL; }
    if (subsonic_menu_screen) { lv_obj_del(subsonic_menu_screen); subsonic_menu_screen = NULL; }
    if (subsonic_artists_screen) { lv_obj_del(subsonic_artists_screen); subsonic_artists_screen = NULL; }
    if (subsonic_albums_screen) { lv_obj_del(subsonic_albums_screen); subsonic_albums_screen = NULL; }
    if (subsonic_songs_screen) { lv_obj_del(subsonic_songs_screen); subsonic_songs_screen = NULL; }
    if (subsonic_playlists_screen) { lv_obj_del(subsonic_playlists_screen); subsonic_playlists_screen = NULL; }
}

bool gui_subsonic_has_background_work(void) {
    return download_active || subsonic_library_download_active || subsonic_connect_active || subsonic_browse_active;
}

void gui_subsonic_cancel_background_work(void) {
    if (subsonic_connect_active) {
        pthread_join(subsonic_connect_thread, NULL);
        subsonic_connect_active = false;
        gui_busy_hide(subsonic_connect_token);
    }
    if (download_active) {
        pthread_join(download_thread, NULL);
        download_active = false;
        gui_busy_hide(download_token);
    }
    if (subsonic_library_download_active) {
        pthread_join(subsonic_library_download_thread, NULL);
        subsonic_library_download_active = false;
        gui_busy_hide(subsonic_library_download_token);
    }
    if (subsonic_browse_active) {
        pthread_join(subsonic_browse_thread, NULL);
        subsonic_browse_active = false;
    }
}
