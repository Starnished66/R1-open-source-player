#include "gui_player.h"
#include "gui.h"
#include "gui_theme.h"
#include "gui_notifications.h"
#include "gui_library.h"
#include "gui_queue.h"
#include "gui_lyrics.h"
#include "gui_settings.h"
#include "gui_subsonic.h"
#include "http_client.h"
#include "screen_builders.h"
#include "metadata.h"
#include "metadata_db.h"
#include "cover_decode.h"
#include "albumart.h"
#include "audio.h"
#include "settings.h"
#include "assets.h"
#include "device_config.h"
#include "plugin_manager.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <pthread.h>
#include <unistd.h>
#include <limits.h>

#define VOLUME_POPUP_TIMEOUT_MS 3000
#define HOME_INDICATOR_BAND_HEIGHT 24

lv_obj_t * player_screen = NULL;
lv_obj_t * player_dismiss_btn = NULL;
lv_obj_t * player_overlay_panel = NULL;
lv_obj_t * cover_img = NULL;
lv_obj_t * song_folder_label = NULL;
lv_obj_t * song_quality_label = NULL;
lv_obj_t * song_bitrate_label = NULL;
lv_obj_t * song_track_label = NULL;
lv_obj_t * song_count_label = NULL;
lv_obj_t * song_title_label = NULL;
lv_obj_t * format_badge_label = NULL;
lv_obj_t * play_mode_img = NULL;
static lv_obj_t * order_icon = NULL;
lv_obj_t * favorite_icon = NULL;
lv_obj_t * play_btn = NULL;
lv_obj_t * prev_btn = NULL;
lv_obj_t * next_btn = NULL;
lv_obj_t * progress_slider = NULL;
lv_obj_t * progress_label = NULL;
lv_obj_t * duration_label = NULL;
lv_obj_t * volume_slider = NULL;

static lv_obj_t * pos_label = NULL;
static lv_obj_t * dur_label = NULL;

static int32_t displayed_progress_percent = -1;
static int displayed_position_second = -1;
static int displayed_duration_second = -1;

lv_obj_t * volume_popup = NULL;
lv_obj_t * volume_popup_backdrop = NULL;
lv_obj_t * more_menu_popup = NULL;
lv_obj_t * more_menu_popup_backdrop = NULL;

static lv_obj_t * volume_popup_track = NULL;
static lv_timer_t * volume_popup_hide_timer = NULL;
static lv_obj_t * delete_song_popup = NULL;
static lv_obj_t * delete_song_popup_backdrop = NULL;
static lv_obj_t * delete_song_popup_title = NULL;

static lv_image_dsc_t current_cover_dsc;

static uint8_t * current_cover_bytes = NULL;
static int current_cover_for_index = -1;

static uint8_t * current_reflection_bytes = NULL;
static lv_image_dsc_t current_reflection_dsc;

extern lv_obj_t * quick_drawer_title_label;
extern lv_obj_t * quick_drawer_artist_label;
extern lv_obj_t * quick_drawer_favorite_icon;
extern lv_obj_t * quick_drawer_play_btn;
extern lv_obj_t * volume_topbar_btn;
extern lv_obj_t * volume_topbar_label;
extern lv_obj_t * status_bar;
extern lv_obj_t * eq_screen;
extern player_settings_t current_settings;
extern bool favorite_is_set;
extern int * playlist_lazy_sort_order;

extern void nav_push(lv_obj_t * screen);
extern void nav_pop(void);
extern void finalize_screen_navigation(lv_obj_t * screen);
extern void generic_back_cb(lv_event_t * e);
extern void quick_drawer_mark_snapshot_dirty(void);
extern void clear_player_source(void);
extern void prev_btn_event_cb(lv_event_t * e);
extern void play_btn_event_cb(lv_event_t * e);
extern void next_btn_event_cb(lv_event_t * e);
extern void play_mode_tap_event_cb(lv_event_t * e);
extern void play_mode_long_press_cb(lv_event_t * e);
extern void slider_event_cb(lv_event_t * e);
extern void volume_slider_event_cb(lv_event_t * e);
extern const char * play_mode_icon_asset(play_mode_t mode);

void poll_volume_popup_timeout(void) {
    if (volume_popup && !lv_obj_has_flag(volume_popup, LV_OBJ_FLAG_HIDDEN)) {
        /* handled by timer */
    }
}


/* Transient volume popup (Task #28 / closes Task #17): built once, hidden,
 * on the top layer -- same reasoning as the status bar, but shown only for
 * a couple of seconds after the hardware volume buttons change the level,
 * then auto-hidden, instead of a permanently visible slider. */

static void volume_popup_hide_timer_cb(lv_timer_t * timer) {
    (void) timer;
    lv_obj_add_flag(volume_popup, LV_OBJ_FLAG_HIDDEN);
    lv_timer_pause(volume_popup_hide_timer);
}

/* Real-device feedback: the popup's own slider used to be display-only
 * (hw volume buttons the only way to change it) -- this makes it drag/
 * touch-able too, same PRESSED/VALUE_CHANGED/RELEASED shape as the player
 * screen's own volume_slider_event_cb, plus keeping that slider and the
 * topbar readout in sync since all three show the same value. */
static void volume_popup_track_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    int32_t percent = lv_slider_get_value(lv_event_get_target(e));

    if (code == LV_EVENT_PRESSED) {
        /* Stop the 1.5s auto-hide countdown while a finger's still on it --
         * otherwise a slow drag could get hidden out from under the user
         * mid-interaction. */
        lv_timer_pause(volume_popup_hide_timer);
    } else if (code == LV_EVENT_VALUE_CHANGED) {
        audio_set_volume((float) percent / 100.0f); /* live feedback while dragging */
        lv_slider_set_value(volume_slider, percent, LV_ANIM_OFF);
        refresh_volume_topbar(percent);
    } else if (code == LV_EVENT_RELEASED) {
        /* Only persist once the drag settles, not on every intermediate
         * tick -- same as volume_slider_event_cb. Resets the hide timer
         * fresh from here rather than leaving it paused. */
        current_settings.volume = (float) percent / 100.0f;
        settings_save(&current_settings);
        lv_timer_reset(volume_popup_hide_timer);
        lv_timer_resume(volume_popup_hide_timer);
    }
}

/* The stock rail sprites are 360 px wide. LVGL centers a background image
 * at its native size instead of scaling it to the part, so they protrude
 * from the 340 px volume rail and the dynamically narrower brightness rail.
 * Paint those two rails natively; keep cursor.png only for the knob. */
void configure_native_slider_rail(lv_obj_t * slider) {
    lv_obj_set_style_bg_image_src(slider, NULL, LV_PART_MAIN);
    lv_obj_set_style_bg_image_src(slider, NULL, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_make(132, 134, 132), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
}

static void build_volume_popup(void) {
    lv_obj_t * top = lv_layer_top();

    volume_popup = lv_obj_create(top);
    lv_obj_set_size(volume_popup, 440, 60);
    lv_obj_align(volume_popup, LV_ALIGN_TOP_MID, 0, STATUS_BAR_CLEARANCE + 12);
    lv_obj_set_style_bg_opa(volume_popup, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_image_src(volume_popup, asset_path("volume/bg.png"), 0);
    lv_obj_set_style_border_width(volume_popup, 0, 0);
    lv_obj_set_style_pad_all(volume_popup, 0, 0);
    lv_obj_remove_flag(volume_popup, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(volume_popup, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t * speaker_icon = lv_image_create(volume_popup);
    lv_image_set_src(speaker_icon, asset_path("volume/vol.png"));
    lv_obj_align(speaker_icon, LV_ALIGN_LEFT_MID, 20, 0);

    volume_popup_track = lv_slider_create(volume_popup);
    lv_obj_set_size(volume_popup_track, 340, 12);
    lv_obj_align(volume_popup_track, LV_ALIGN_RIGHT_MID, -20, 0);
    lv_slider_set_range(volume_popup_track, 0, 100);
    lv_obj_set_style_bg_opa(volume_popup_track, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(volume_popup_track, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(volume_popup_track, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_bg_image_src(volume_popup_track, asset_path("volume/cursor.png"), LV_PART_KNOB);
    lv_obj_add_style(volume_popup_track, &style_accent, LV_PART_INDICATOR);
    lv_obj_add_style(volume_popup_track, &style_accent, LV_PART_KNOB);
    configure_native_slider_rail(volume_popup_track);
    lv_obj_set_style_width(volume_popup_track, 30, LV_PART_KNOB);
    lv_obj_set_style_height(volume_popup_track, 30, LV_PART_KNOB);
    lv_obj_add_event_cb(volume_popup_track, volume_popup_track_event_cb, LV_EVENT_ALL, NULL);

    volume_popup_hide_timer = lv_timer_create(volume_popup_hide_timer_cb, 1500, NULL);
    lv_timer_pause(volume_popup_hide_timer);
}

/* Feature request: reuse the stock firmware's own "frosted mirror" look --
 * a heavily blurred, darkened, vertically-mirrored copy of the album art
 * filling the panel behind the transport controls (player_overlay_panel,
 * built in build_player_screen()), rather than that panel's plain flat
 * buttom.png background. Confirmed via investigation that the stock
 * firmware itself doesn't ship this as a static asset (every buttom.png
 * across both themes is a flat opaque solid color, no gradient or baked-in
 * art) and no now-playing layout JSON was available to inspect directly --
 * so this is generated fresh here, from the same per-track RGB565 buffer
 * cover_decode_to_rgb565() already decodes, rather than reverse-engineered
 * from an asset that doesn't exist in this codebase's copy of the firmware. */
#define REFLECTION_WIDTH 480
#define REFLECTION_HEIGHT 320
#define REFLECTION_BLUR_RADIUS 32
#define REFLECTION_BLUR_PASSES 5
/* Kept as an integer ratio (channel * NUM / DEN) rather than a float --
 * matches this codebase's general preference for integer arithmetic on the
 * embedded target, and there's no accuracy need here that would justify a
 * float. 1/4 reads as "mostly faded into black" without the panel going
 * fully flat. */
#define REFLECTION_DARKEN_NUM 1
#define REFLECTION_DARKEN_DEN 2

/* 1D box blur via a sliding running sum -- O(length) regardless of radius,
 * rather than O(length * radius) from re-summing the whole window at every
 * pixel. `stride` is measured in elements, not bytes, so the same function
 * serves both passes of the separable 2D blur generate_reflection() below
 * does: 1 for a horizontal pass along a contiguous row, `width` for a
 * vertical pass down a column. Edges are clamped (repeats the edge pixel)
 * rather than wrapping or zero-padding, so the blur doesn't darken/lighten
 * near the image boundary. */
void box_blur_1d(const uint8_t * src, uint8_t * dst, int length, int stride, int radius) {
    int window = radius * 2 + 1;
    int sum = 0;
    for (int i = -radius; i <= radius; i++) {
        int idx = i < 0 ? 0 : (i >= length ? length - 1 : i);
        sum += src[idx * stride];
    }
    for (int i = 0; i < length; i++) {
        dst[i * stride] = (uint8_t) (sum / window);
        int add_idx = i + radius + 1;
        if (add_idx >= length) add_idx = length - 1;
        int rem_idx = i - radius;
        if (rem_idx < 0) rem_idx = 0;
        sum += src[add_idx * stride] - src[rem_idx * stride];
    }
}

/* RGB565 has only 32 red/blue and 64 green levels, so a strong blur exposes
 * broad contour bands even though all filtering above is done in 8-bit
 * planes. Ordered 8x8 dithering trades those coherent bands for a tiny,
 * stable sub-pixel texture. Stable (not random) matters because a changing
 * noise field would shimmer on every redraw. */
uint16_t rgb888_to_565_dithered(int r, int g, int b, int x, int y) {
    static const uint8_t bayer8[8][8] = {
        { 0,48,12,60, 3,51,15,63 }, { 32,16,44,28,35,19,47,31 },
        { 8,56, 4,52,11,59, 7,55 }, { 40,24,36,20,43,27,39,23 },
        { 2,50,14,62, 1,49,13,61 }, { 34,18,46,30,33,17,45,29 },
        { 10,58, 6,54, 9,57, 5,53 }, { 42,26,38,22,41,25,37,21 }
    };
    int threshold = bayer8[y & 7][x & 7];
    r += threshold / 8 - 4;
    g += threshold / 16 - 2;
    b += threshold / 8 - 4;
    if (r < 0) r = 0; else if (r > 255) r = 255;
    if (g < 0) g = 0; else if (g > 255) g = 255;
    if (b < 0) b = 0; else if (b > 255) b = 255;
    return (uint16_t) (((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

/* Pure pixel math -- no LVGL/shared-state touch, so it's safe to call from
 * a background thread (see the async cover-decode pipeline below). Builds
 * the reflection from a given COVER_ART_WIDTH x COVER_ART_HEIGHT RGB565
 * buffer and returns a freshly malloc'd REFLECTION_WIDTH x REFLECTION_HEIGHT
 * RGB565 buffer, or NULL on allocation failure. Caller owns the result. */
static uint8_t * compute_reflection_bytes(const uint8_t * cover_bytes) {
    int w = REFLECTION_WIDTH, h = REFLECTION_HEIGHT;
    uint8_t * r = malloc((size_t) w * h);
    uint8_t * g = malloc((size_t) w * h);
    uint8_t * b = malloc((size_t) w * h);
    uint8_t * tmp = malloc((size_t) w * h);
    if (!r || !g || !b || !tmp) {
        free(r); free(g); free(b); free(tmp);
        return NULL;
    }

    /* Center-crop the square cover to this wide panel. The previous bottom-
     * strip mirror preserved recognizable hard shapes even after blurring,
     * which looked like a smeared reflection rather than frosted glass. */
    for (int y = 0; y < h; y++) {
        int src_y = (COVER_ART_HEIGHT - h) / 2 + y;
        const uint16_t * src_row = (const uint16_t *) (cover_bytes + (size_t) src_y * COVER_ART_WIDTH * 2);
        for (int x = 0; x < w; x++) {
            uint16_t px = src_row[x];
            r[y * w + x] = (uint8_t) (((px >> 11) & 0x1F) * 255 / 31);
            g[y * w + x] = (uint8_t) (((px >> 5) & 0x3F) * 255 / 63);
            b[y * w + x] = (uint8_t) ((px & 0x1F) * 255 / 31);
        }
    }

    /* Separable box blur: one horizontal pass (per row) then one vertical
     * pass (per column), on each channel independently -- a full 2D blur
     * at a fraction of the cost of a real 2D kernel. Heavy (radius 16)
     * deliberately, per the "frosted glass" look this is going for rather
     * than a sharp mirror image. */
    for (int pass = 0; pass < REFLECTION_BLUR_PASSES; pass++) {
        for (int y = 0; y < h; y++) box_blur_1d(r + y * w, tmp + y * w, w, 1, REFLECTION_BLUR_RADIUS);
        memcpy(r, tmp, (size_t) w * h);
        for (int x = 0; x < w; x++) box_blur_1d(r + x, tmp + x, h, w, REFLECTION_BLUR_RADIUS);
        memcpy(r, tmp, (size_t) w * h);
    }
    for (int pass = 0; pass < REFLECTION_BLUR_PASSES; pass++) {
        for (int y = 0; y < h; y++) box_blur_1d(g + y * w, tmp + y * w, w, 1, REFLECTION_BLUR_RADIUS);
        memcpy(g, tmp, (size_t) w * h);
        for (int x = 0; x < w; x++) box_blur_1d(g + x, tmp + x, h, w, REFLECTION_BLUR_RADIUS);
        memcpy(g, tmp, (size_t) w * h);
    }
    for (int pass = 0; pass < REFLECTION_BLUR_PASSES; pass++) {
        for (int y = 0; y < h; y++) box_blur_1d(b + y * w, tmp + y * w, w, 1, REFLECTION_BLUR_RADIUS);
        memcpy(b, tmp, (size_t) w * h);
        for (int x = 0; x < w; x++) box_blur_1d(b + x, tmp + x, h, w, REFLECTION_BLUR_RADIUS);
        memcpy(b, tmp, (size_t) w * h);
    }

    uint8_t * out_bytes = malloc((size_t) w * h * 2);
    if (!out_bytes) {
        free(r); free(g); free(b); free(tmp);
        return NULL;
    }

    /* Darken (mostly faded into player_overlay_panel's black background)
     * and repack into RGB565, same channel layout current_cover_dsc uses. */
    uint16_t * out = (uint16_t *) out_bytes;
    for (int i = 0; i < w * h; i++) {
        int rv = (r[i] * REFLECTION_DARKEN_NUM) / REFLECTION_DARKEN_DEN;
        int gv = (g[i] * REFLECTION_DARKEN_NUM) / REFLECTION_DARKEN_DEN;
        int bv = (b[i] * REFLECTION_DARKEN_NUM) / REFLECTION_DARKEN_DEN;
        out[i] = rgb888_to_565_dithered(rv, gv, bv, i % w, i / w);
    }
    free(r); free(g); free(b); free(tmp);
    return out_bytes;
}

/* Real-device bug report: entering the player screen right after picking a
 * song left its on-screen play/pause/next/prev buttons unresponsive for a
 * couple of seconds. Root cause: apply_track_metadata_to_ui() used to
 * decode the embedded cover art (cover_decode_to_rgb565(), a full native-
 * resolution JPEG/PNG decode -- see cover_decode.c's own comment on why
 * this can't use tjpgd's faster scaled decode) and run the reflection blur
 * above synchronously, on the UI thread, before play_track_at_from() ever
 * reached nav_push(player_screen) -- easily 1-3+ seconds of pure blocking
 * on this hardware for a large embedded image, during which
 * lv_timer_handler() never runs, so nothing redraws and no touch input is
 * processed at all. Exact same shape of bug (and fix) as the real-device
 * incident documented at bt_apply_output_settings_thread_func()'s own
 * comment above -- backgrounded here the same way: launch_cover_decode()
 * kicks off the decode+blur on a pthread (cover_decode_to_rgb565() and
 * compute_reflection_bytes() touch no LVGL/shared state, so this is safe),
 * and poll_cover_decode() (called from update_timer_cb every tick) applies
 * the result to the actual widgets once it's ready -- so nav_push() and
 * audio_play_file_at() in play_track_at_from() now run immediately, with
 * the cover art/reflection catching up a moment later instead of blocking
 * everything else on the way there. */
typedef struct {
    int for_index;
    uint8_t * picture_data; /* owned; NULL if the track has no embedded art */
    uint32_t picture_size;

    /* Set instead of picture_data/picture_size for a Subsonic stream's cover
     * art (see launch_cover_decode_from_url()) -- there's no local file to
     * have already extracted embedded art from, so the compressed image
     * bytes are fetched here, on the same background thread, before falling
     * into the same decode path local art already uses. Empty string for
     * every other caller. */
    char stream_url[1536];
    bool stream_verify_tls;
    /* Local track whose directory should be searched when embedded art is
     * absent. The lookup and file read stay on this worker thread. Artist/
     * album come from the already-parsed track (or the DB), never from a
     * second metadata_read() -- that re-parse OOMs on huge ID3/APIC tags. */
    char local_track_path[PATH_MAX];
    char artist[128];
    char album[128];
    char album_artist[128];
} cover_decode_request_t;

static pthread_t cover_decode_thread;
static bool cover_decode_active = false;
static atomic_bool cover_decode_done_flag = false;

/* Result fields, written by cover_decode_thread_func() on the background
 * thread and consumed (freed or applied) by poll_cover_decode() on the main
 * thread once cover_decode_done_flag is seen true -- never touched by both
 * at once, same "background thread writes then sets the flag last, main
 * thread only reads after seeing the flag" contract as every other _done_
 * flag in this file. */
static int cover_decode_result_for_index;
static bool cover_decode_result_ok;
static uint16_t * cover_decode_result_pixels;    /* COVER_ART_WIDTH x COVER_ART_HEIGHT RGB565, owned */
static uint8_t * cover_decode_result_reflection; /* REFLECTION_WIDTH x REFLECTION_HEIGHT RGB565, owned */

static void launch_cover_decode(int for_index, uint8_t * picture_data, uint32_t picture_size);

/* Holds at most one superseded request -- see launch_cover_decode()'s own
 * comment on why a track change that arrives while a decode is already in
 * flight can't just be dropped (that would leave the cover permanently
 * stuck showing an earlier track's art). */
static bool cover_decode_pending_valid = false;
static cover_decode_request_t cover_decode_pending;

/* Compressed sidecar cap. cover_decode.c also rejects sources above
 * 1200x1200 before allocating RGB888, so a 4000px cover.jpg that still
 * fits in 4 MiB compressed cannot OOM the ~56 MiB target. Oversized art
 * falls back to the default image. */
#define EXTERNAL_COVER_MAX_BYTES (4U * 1024U * 1024U)

static void albumart_info_from_path_tags(const char * track_path, const char * artist, const char * album,
                                          const char * album_artist, albumart_info_t * info) {
    memset(info, 0, sizeof(*info));
    snprintf(info->path, sizeof(info->path), "%s", track_path ? track_path : "");
    snprintf(info->artist, sizeof(info->artist), "%s", artist ? artist : "");
    snprintf(info->album, sizeof(info->album), "%s", album ? album : "");
    snprintf(info->albumartist, sizeof(info->albumartist), "%s", album_artist ? album_artist : "");
}

void albumart_info_from_song_row(const song_row_t * song, albumart_info_t * info) {
    albumart_info_from_path_tags(song->path, song->tags.artist, song->tags.album, song->tags.album_artist, info);
}

/* Rockbox albumart.c search: sized file first, then generic cover/folder
 * next to the track, then MUSIC_ROOT_DIR/.open_hiby_player/albumart/<artist>-<album>.
 * Tags are supplied by the caller (already-parsed track or DB row). This
 * must not open the audio file. */
static bool load_external_cover(const char * track_path, const char * artist, const char * album,
                                const char * album_artist, uint8_t ** out_data, uint32_t * out_size) {
    albumart_info_t info;
    albumart_info_from_path_tags(track_path, artist, album, album_artist, &info);
    char found[PATH_MAX];
    if (!albumart_find(&info, found, sizeof(found), COVER_ART_WIDTH, COVER_ART_HEIGHT)) return false;
    return albumart_load_file(found, out_data, out_size, EXTERNAL_COVER_MAX_BYTES);
}

static void * cover_decode_thread_func(void * arg) {
    cover_decode_request_t * req = (cover_decode_request_t *) arg;

    if (req->stream_url[0] != '\0') {
        int status = 0;
        uint8_t * body = NULL;
        size_t body_size = 0;
        if (http_get_to_buffer(req->stream_url, req->stream_verify_tls, &status, &body, &body_size) && status == 200) {
            req->picture_data = body;
            req->picture_size = (uint32_t) body_size;
        } else {
            free(body);
            /* Left NULL/0 -- decode below no-ops, same as "no embedded art"
             * for a local file. No error surfaced beyond that; a failed
             * cover-art fetch isn't worth blocking or retrying playback
             * over. */
        }
    } else if (!req->picture_data && req->local_track_path[0]) {
        load_external_cover(req->local_track_path, req->artist, req->album, req->album_artist,
                            &req->picture_data, &req->picture_size);
    }

    uint16_t * pixels = NULL;
    bool ok = req->picture_data && req->picture_size > 0 &&
              cover_decode_to_rgb565(req->picture_data, req->picture_size, COVER_ART_WIDTH, COVER_ART_HEIGHT, &pixels);
    free(req->picture_data); /* the compressed bytes are no longer needed once decoded (or decode failed) */

    uint8_t * reflection = ok ? compute_reflection_bytes((const uint8_t *) pixels) : NULL;

    cover_decode_result_for_index = req->for_index;
    cover_decode_result_ok = ok;
    cover_decode_result_pixels = pixels;
    cover_decode_result_reflection = reflection;
    free(req);
    atomic_store_explicit(&cover_decode_done_flag, true, memory_order_release); /* written last -- poll_cover_decode() only checks this flag */
    return NULL;
}

/* Takes ownership of picture_data (a malloc()'d buffer from metadata_read(),
 * or NULL). If a decode is already running, this doesn't launch a second
 * one (cover_decode_to_rgb565()/compute_reflection_bytes() are reentrant --
 * no shared state -- but there's no value in two decodes racing when only
 * the newest one's result will ever get applied) -- it instead replaces
 * cover_decode_pending (freeing whatever was queued there before), and
 * poll_cover_decode() launches that pending request the moment the active
 * one finishes. A rapid sequence of track changes only ever actually
 * decodes the first and the final one, which is exactly the set of results
 * that matters. */
static void launch_cover_decode_req(cover_decode_request_t r) {
    if (cover_decode_active) {
        free(cover_decode_pending.picture_data);
        cover_decode_pending = r;
        cover_decode_pending_valid = true;
        return;
    }

    cover_decode_request_t * req = malloc(sizeof(*req));
    if (!req) {
        /* Audit finding: this used to dereference req unconditionally --
         * on this RAM-constrained device, a malloc() this size (the
         * struct embeds a 1536-byte stream_url and a PATH_MAX local_track_
         * path) can genuinely fail under memory pressure, and this runs on
         * essentially every track change, not just a deliberate low-memory
         * test. Free the request's own owned picture_data (same ownership
         * contract every other caller relies on) and just skip this
         * decode -- cover art staying stale for one track is a far better
         * outcome than crashing the whole app. */
        free(r.picture_data);
        return;
    }
    *req = r;
    atomic_store_explicit(&cover_decode_done_flag, false, memory_order_relaxed);
    cover_decode_active = true;
    if (pthread_create(&cover_decode_thread, NULL, cover_decode_thread_func, req) != 0) {
        free(req->picture_data);
        free(req);
        cover_decode_active = false;
    }
}

static void launch_cover_decode(int for_index, uint8_t * picture_data, uint32_t picture_size) {
    cover_decode_request_t r = { .for_index = for_index, .picture_data = picture_data, .picture_size = picture_size,
                                  .stream_url = "", .stream_verify_tls = false };
    launch_cover_decode_req(r);
}

static void launch_cover_decode_from_track(int for_index, const char * track_path, const char * artist,
                                            const char * album, const char * album_artist) {
    cover_decode_request_t r = { .for_index = for_index, .picture_data = NULL, .picture_size = 0,
                                  .stream_url = "", .stream_verify_tls = false };
    snprintf(r.local_track_path, sizeof(r.local_track_path), "%s", track_path ? track_path : "");
    snprintf(r.artist, sizeof(r.artist), "%s", artist ? artist : "");
    snprintf(r.album, sizeof(r.album), "%s", album ? album : "");
    snprintf(r.album_artist, sizeof(r.album_artist), "%s", album_artist ? album_artist : "");
    launch_cover_decode_req(r);
}

/* Subsonic streaming's own art source -- see cover_decode_request_t's own
 * comment. url is subsonic_build_cover_art_url()'s output, fetched on the
 * same background thread that would otherwise be decoding already-local
 * bytes, so a slow/flaky connection can't block the UI here either, same
 * reasoning as launch_cover_decode() itself. */
static void launch_cover_decode_from_url(int for_index, const char * url, bool verify_tls) {
    cover_decode_request_t r = { .for_index = for_index, .picture_data = NULL, .picture_size = 0,
                                  .stream_verify_tls = verify_tls };
    snprintf(r.stream_url, sizeof(r.stream_url), "%s", url);
    launch_cover_decode_req(r);
}


/* Called every tick from update_timer_cb. Applies the finished decode to
 * cover_img/player_overlay_panel -- unless playlist_index has already moved
 * on to a different track by the time this lands (another launch_cover_
 * decode() call superseded it via cover_decode_pending, in which case that
 * one is now either running or about to be), in which case the result is
 * just discarded rather than briefly flashing a stale track's art. */
void poll_cover_decode(void) {
    if (!cover_decode_active || !atomic_load_explicit(&cover_decode_done_flag, memory_order_acquire)) return;
    cover_decode_active = false;
    pthread_join(cover_decode_thread, NULL);

    if (cover_decode_result_for_index != playlist_index) {
        free(cover_decode_result_pixels);
        free(cover_decode_result_reflection);
    } else if (!cover_decode_result_ok) {
        free(current_cover_bytes);
        current_cover_bytes = NULL;
        current_cover_for_index = -1;
        lv_image_set_src(cover_img, asset_path("playing_plane/default_cover_565.png"));
        /* No in-memory raw bitmap to reflect for the static placeholder
         * cover -- reset the panel back to its plain background rather
         * than leaving a stale reflection from whatever track played
         * before this one. */
        free(current_reflection_bytes);
        current_reflection_bytes = NULL;
        lv_obj_set_style_bg_image_src(player_overlay_panel, asset_path("playing_plane/buttom.png"), 0);
        player_transition_mark_dirty(); /* cover_img just changed to the placeholder -- see the cache's own doc comment */
    } else {
        free(current_cover_bytes);
        current_cover_bytes = (uint8_t *) cover_decode_result_pixels;
        current_cover_for_index = cover_decode_result_for_index;
        free(current_reflection_bytes);
        current_reflection_bytes = cover_decode_result_reflection;

        /* Real-device incident: "severe noise/corruption" on the actual on-
         * screen cover art, root-caused via a raw dump of current_cover_bytes
         * taken the instant cover_decode_to_rgb565() returns (before this
         * point) -- a correct, undamaged image every time, on the real
         * target hardware, not just a host-build test, which ruled out the
         * decode pipeline entirely. An intermediate theory (LVGL's image
         * cache, keyed on the source pointer, serving stale tiles since
         * current_cover_dsc is a reused static struct) turned out to be
         * wrong too -- lv_image_cache_drop() before every reassignment made
         * no difference, and the same corruption showed up on a completely
         * different track's art, not just a stale leftover from the
         * previous one.
         *
         * The real bug: lv_image_header_t.magic (lv_image_dsc.h) must be
         * LV_IMAGE_HEADER_MAGIC -- lv_bin_decoder.c (the decoder this hand-
         * constructed raw descriptor actually goes through) treats any other
         * value as an old-format header from before the magic field existed,
         * and "fixes it up" in place via `header->cf = header->magic;
         * header->magic = LV_IMAGE_HEADER_MAGIC;`. memset()ing the whole
         * descriptor to 0 before setting cf/w/h/stride left magic at 0, so
         * this quirks-mode shim fired on literally every track change,
         * silently overwriting our just-set LV_COLOR_FORMAT_RGB565 with 0
         * right before the image ever got drawn -- corrupting the color
         * format used to interpret every pixel, not the pixel data itself,
         * which is exactly why the underlying buffer always dumped correctly
         * but the screen never showed it right. */
        memset(&current_cover_dsc, 0, sizeof(current_cover_dsc));
        current_cover_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        current_cover_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
        current_cover_dsc.header.w = COVER_ART_WIDTH;
        current_cover_dsc.header.h = COVER_ART_HEIGHT;
        current_cover_dsc.header.stride = COVER_ART_WIDTH * 2;
        current_cover_dsc.data = current_cover_bytes;
        current_cover_dsc.data_size = (uint32_t) COVER_ART_WIDTH * COVER_ART_HEIGHT * 2;
        lv_image_set_src(cover_img, &current_cover_dsc);

        /* Same LV_IMAGE_HEADER_MAGIC requirement as current_cover_dsc above. */
        memset(&current_reflection_dsc, 0, sizeof(current_reflection_dsc));
        current_reflection_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        current_reflection_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
        current_reflection_dsc.header.w = REFLECTION_WIDTH;
        current_reflection_dsc.header.h = REFLECTION_HEIGHT;
        current_reflection_dsc.header.stride = REFLECTION_WIDTH * 2;
        current_reflection_dsc.data = current_reflection_bytes;
        current_reflection_dsc.data_size = (uint32_t) REFLECTION_WIDTH * REFLECTION_HEIGHT * 2;
        lv_obj_set_style_bg_image_src(player_overlay_panel, &current_reflection_dsc, 0);

        /* Bug report: the lyrics screen's blurred backdrop could still show
         * the PREVIOUS track's art even after its own regenerate-pending
         * retry ran, because that retry read whatever current_cover_bytes
         * held at that moment -- and this decode (the one that actually
         * updates current_cover_bytes for the new track) can easily still
         * be in flight when the retry fires, since the two run on
         * independent async pipelines with no ordering between them. This
         * is the one place current_cover_bytes is ever updated for a new
         * track, so triggering the refresh from here (rather than from
         * lyrics_timer_cb()'s own earlier, opportunistic attempt) is the
         * only way to guarantee it always runs against the CORRECT art.
         * launch_lyrics_backdrop_decode() itself already no-ops if a
         * generation happens to already be running (marking it pending
         * instead, same as any other caller), so this is safe to call
         * unconditionally whenever the lyrics screen is open. */
        gui_lyrics_on_cover_changed(playlist_index);
        player_transition_mark_dirty(); /* cover_img/player_overlay_panel's reflection just changed -- see the cache's own doc comment */
    }

    cover_decode_result_pixels = NULL;
    cover_decode_result_reflection = NULL;

    if (cover_decode_pending_valid) {
        cover_decode_pending_valid = false;
        launch_cover_decode_req(cover_decode_pending); /* not launch_cover_decode() -- must carry stream_url too, see that field's own comment */
        /* Real-device incident: ownership of picture_data just passed into
         * launch_cover_decode_req() above (either into a freshly malloc'd
         * req, or back into this same cover_decode_pending if a decode was
         * somehow still active) -- but this struct still holds a copy of
         * that same pointer. Left as-is, a rapid next track change landing
         * before the handed-off decode finishes would call
         * launch_cover_decode_req() again, see cover_decode_active still
         * true, and free(cover_decode_pending.picture_data) a buffer the
         * in-flight decode thread already owns and will free itself --
         * double free, reproduced by rapidly pressing Next/Prev. Clearing
         * the pointer here (not inside launch_cover_decode_req(), which
         * can't tell "just consumed" apart from "still needs freeing" for
         * its own r argument) makes this copy stop looking like it owns
         * the buffer. */
        cover_decode_pending.picture_data = NULL;
        cover_decode_pending.picture_size = 0;
    }
}

/* ---- Lyrics: async .lrc load ------------------------------------------
 * Deliberately independent from the cover-decode pipeline above, despite
 * both running off the UI thread and both being triggered from the same
 * track-change call site (apply_track_metadata_to_ui() below): this job
 * only reads a small text sidecar (lyrics_load_sidecar(), bounded at
 * LYRICS_MAX_FILE_BYTES) rather than decoding/blurring image data, and
 * runs eagerly on every track change (not lazily like the backdrop below)
 * so lyrics are already parsed by the time the user taps the album art.
 * Same "background thread writes then sets a volatile done flag last, poll
 * function on the UI thread only reads after seeing it" contract as
 * cover_decode_done_flag above, and the same "at most one job in flight,
 * a track change arriving mid-load replaces the pending request rather
 * than queuing" shape as launch_cover_decode_req()'s cover_decode_pending. ---- */







/* ---- Lyrics: fullscreen blurred backdrop ------------------------------
 * Generated lazily -- only when the lyrics screen actually opens (see
 * open_lyrics_screen()) -- rather than eagerly per track change like cover
 * art/reflection above: most tracks' lyrics view is never opened, and this
 * needs nothing the async load above produces, just the already-decoded
 * current_cover_bytes. A private copy is taken up front (cover_copy below)
 * rather than reading current_cover_bytes directly from the background
 * thread, since poll_cover_decode() can free and replace that pointer out
 * from under a still-running backdrop job on a rapid track change. ---- */

/* Final brightness is baked in before RGB565 dithering. This used to be a
 * 3/4-bright backdrop followed by LVGL's 40%-black alpha overlay (net 45%),
 * but that second RGB565 blend re-quantized the already-dithered image and
 * brought visible bands back. 9/20 preserves the same net brightness in a
 * single quantization step. */












void favorite_icon_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (playlist_index < 0 || playlist_index >= playlist_count) return;

    favorite_is_set = !favorite_is_set;
    metadata_db_song_favorite_set(playlist_path_at(playlist_index), favorite_is_set);
    lv_image_set_src(favorite_icon, asset_path(favorite_is_set ? "playing_plane/collect_in.png" : "playing_plane/collect_out.png"));
    if (quick_drawer_favorite_icon) {
        lv_image_set_src(quick_drawer_favorite_icon,
                         asset_path(favorite_is_set ? "playing_plane/collect_in.png" : "playing_plane/collect_out.png"));
        quick_drawer_mark_snapshot_dirty();
    }
}

void arm_next_track_for_audio(int index);

/* Shared by the player screen's own order_icon tap and the Remote Control
 * web UI's mode-cycle button (remote_control_consume_mode_cycle()) -- same
 * single 4-state cycle either way, so both surfaces stay in sync and
 * neither reimplements the mode-advance/persist/re-arm logic. */
void cycle_play_mode(void) {
    play_mode_t mode = (play_mode_t) current_settings.play_mode;
    mode = (play_mode_t) ((mode + 1) % 4);
    current_settings.play_mode = (int) mode;
    settings_save(&current_settings);

    lv_image_set_src(order_icon, asset_path(play_mode_icon_asset(mode)));
    if (quick_drawer_order_icon) {
        lv_image_set_src(quick_drawer_order_icon, asset_path(play_mode_icon_asset(mode)));
        quick_drawer_mark_snapshot_dirty();
    }

    /* What comes after the current track changes with the mode (e.g.
     * entering Shuffle picks a random next instead of index+1) -- re-arm the
     * gapless/crossfade target immediately rather than waiting for whatever
     * was armed under the old mode to turn out wrong. */
    if (playlist_index >= 0) arm_next_track_for_audio(playlist_index);
}

static void order_icon_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    cycle_play_mode();
}

/* refresh_now_playing_indicators() is defined down with the compact-list
 * screens it updates (Artists/Albums/All Songs/group songs) -- forward-
 * declared here since apply_track_metadata_to_ui() below is the single
 * place a real "this track started playing" event is known to have
 * happened, for every playback source (local library, Group Songs, Files,
 * .m3u playlists). */

/* Shared by both an explicit track pick (play_track_at_from) and the audio
 * thread autonomously advancing into a queued next track on its own
 * (on_track_auto_advanced) -- title/folder/art/format-badge/progress-reset
 * are identical either way. Returns the metadata it read (out_meta) so
 * callers that also need ReplayGain (play_track_at_from, to hand it to
 * audio_play_file_at) don't have to read the file's tags a second time. */
/* Set by subsonic_song_row_click_cb() (gui.c, further down) right before a
 * streamed mp3/flac Subsonic queue starts playing -- metadata_read() can't
 * read tags from a network URL the way it does a local file, but the real
 * title/artist/album (and where to fetch cover art) for every song in the
 * queue are already known from the API responses that built it, so they're
 * stashed here, one entry per playlist[] slot (same indexing, built and
 * freed together), and matched by exact URL string in apply_track_metadata_
 * to_ui() below -- the string match (not just the index) is what makes it
 * safe to leave this array up indefinitely rather than needing to track
 * every other place a new, unrelated playlist might start: subsonic_build_
 * stream_url() bakes a fresh random salt into every URL it builds, so a
 * later, different playlist's paths can never coincidentally match one of
 * these even if this array outlives its own playlist (the array is only
 * ever replaced, on the next Subsonic streaming tap, not proactively freed
 * when some other playback source starts). Forward-declared here (rather
 * than moving apply_track_metadata_to_ui() itself) since the Subsonic click
 * handler that WRITES these lives much further down this file, alongside
 * the rest of the Subsonic screens. */

void apply_track_metadata_to_ui(int index, track_metadata_t * out_meta) {
    /* Resolved once -- this is playlist_index's first real touch on every
     * track-start (play_track_at_from()/on_track_auto_advanced() both call
     * this immediately after setting playlist_index), so it's also where a
     * lazy All-Songs slot actually gets strdup'd. Every other playlist[index]
     * use below reuses this same resolved pointer rather than re-deriving
     * it (playlist_path_at() is idempotent/cheap on a re-call either way,
     * but there's no reason to). */
    const char * path = playlist_path_at(index);

    char title[128];
    char folder[128];
    get_display_names(path, title, sizeof(title), folder, sizeof(folder));

    bool is_subsonic_stream = subsonic_stream_meta && index < subsonic_stream_meta_count &&
                               strcmp(path, subsonic_stream_meta[index].url) == 0;
    remote_track_meta_t remote_meta;
    bool is_remote_track = remote_track_meta_copy_for_path(path, &remote_meta);

    if (is_remote_track) {
        /* No file to open for a remote track -- unlike the Subsonic branch
         * below, this also fixes the ReplayGain gap Subsonic has today
         * (that branch never calls metadata_read(), so has_replaygain stays
         * false): the plugin declares replaygain_db up front from its own
         * catalog metadata, fed into resolve_replaygain() the same as any
         * local track's tag would be. */
        memset(out_meta, 0, sizeof(*out_meta));
        if (remote_meta.title[0]) {
            snprintf(out_meta->title, sizeof(out_meta->title), "%s", remote_meta.title);
            out_meta->has_title = true;
        }
        if (remote_meta.artist[0]) {
            snprintf(out_meta->artist, sizeof(out_meta->artist), "%s", remote_meta.artist);
            out_meta->has_artist = true;
        }
        if (remote_meta.album[0]) {
            snprintf(out_meta->album, sizeof(out_meta->album), "%s", remote_meta.album);
            out_meta->has_album = true;
        }
        if (remote_meta.has_replaygain) {
            out_meta->has_replaygain = true;
            out_meta->replaygain_gain_db = remote_meta.replaygain_db;
        }
    } else if (is_subsonic_stream) {
        memset(out_meta, 0, sizeof(*out_meta));
        if (subsonic_stream_meta[index].title[0]) {
            snprintf(out_meta->title, sizeof(out_meta->title), "%s", subsonic_stream_meta[index].title);
            out_meta->has_title = true;
        }
        if (subsonic_stream_meta[index].artist[0]) {
            snprintf(out_meta->artist, sizeof(out_meta->artist), "%s", subsonic_stream_meta[index].artist);
            out_meta->has_artist = true;
        }
        if (subsonic_stream_meta[index].album[0]) {
            snprintf(out_meta->album, sizeof(out_meta->album), "%s", subsonic_stream_meta[index].album);
            out_meta->has_album = true;
        }
    } else {
        metadata_read(path, out_meta);
    }

    snprintf(now_playing_path, sizeof(now_playing_path), "%s", path);
    refresh_now_playing_indicators();

    const char * title_text = out_meta->has_title ? out_meta->title : title;
    const char * folder_text = out_meta->has_artist ? out_meta->artist : folder;

    lv_label_set_text(song_title_label, title_text);
    lv_label_set_text(song_folder_label, folder_text);
    if (quick_drawer_title_label) {
        lv_label_set_text(quick_drawer_title_label, title_text);
        lv_label_set_text(quick_drawer_artist_label, folder_text);
        quick_drawer_mark_snapshot_dirty();
    }
    refresh_format_badge();
    if (is_remote_track && remote_meta.artwork_url[0]) {
        launch_cover_decode_from_url(index, remote_meta.artwork_url, remote_meta.verify_tls);
    } else if (is_subsonic_stream && subsonic_stream_meta[index].cover_url[0]) {
        launch_cover_decode_from_url(index, subsonic_stream_meta[index].cover_url, subsonic_stream_meta[index].verify_tls);
    } else if (is_remote_track) {
        /* No artwork_url and nothing local to fall back to -- unlike the
         * plain "no embedded picture" case below, launch_cover_decode_
         * from_track() must not be tried: path is the synthetic remote://
         * key, not a real file. */
    } else if (out_meta->picture_data && out_meta->picture_size > 0) {
        launch_cover_decode(index, out_meta->picture_data, out_meta->picture_size); /* embedded art has priority; takes ownership */
    } else {
        free(out_meta->picture_data);
        out_meta->picture_data = NULL;
        launch_cover_decode_from_track(index, path, out_meta->artist, out_meta->album, out_meta->album_artist);
    }

    /* No local sidecar (or embedded tag) makes sense for a Subsonic stream
     * or remote-provider URL -- same reasoning cover art uses a server-
     * supplied cover URL instead of a local-file lookup for those. A
     * streamed track just never gets lyrics in this first release. */
    if (!is_subsonic_stream && !is_remote_track) {
        gui_lyrics_load_track(index, path);
    } else {
        gui_lyrics_load_track(index, NULL);
    }
    /* out_meta->lyrics (populated by the metadata_read() call above, if this
     * track has embedded lyrics) is never read here -- launch_lyrics_load()
     * just above does its own independent metadata_read() on a background
     * thread instead of sharing this one (see lyrics_load_thread_func()'s
     * own comment for why: this function runs synchronously on the UI
     * thread at every track change, and re-parsing tags a second time in
     * the background is cheaper than plumbing a malloc'd pointer through a
     * separate async load path that already does its own file I/O anyway).
     * Always free it here so every caller's stack-local track_metadata_t
     * doesn't leak it. */
    free(out_meta->lyrics);
    out_meta->lyrics = NULL;

    favorite_is_set = metadata_db_song_favorite_is_set(path);
    const char * favorite_icon_asset = favorite_is_set ? "playing_plane/collect_in.png" : "playing_plane/collect_out.png";
    lv_image_set_src(favorite_icon, asset_path(favorite_icon_asset));
    if (quick_drawer_favorite_icon) {
        lv_image_set_src(quick_drawer_favorite_icon, asset_path(favorite_icon_asset));
        quick_drawer_mark_snapshot_dirty();
    }

    /* Once per real "this track started playing" event -- apply_track_
     * metadata_to_ui() is called exactly here for both an explicit pick
     * (play_track_at_from) and a gapless auto-advance (on_track_
     * auto_advanced), never on a repeat UI refresh of the same still-
     * playing track, so this can't double-count. Backs the Most Played
     * auto-generated playlist (Music > Playlists). */
    metadata_db_song_play_count_increment(path);

    /* The real position/duration come from audio_get_*_seconds() once the
     * decoder's opened -- the timer picks that up within its next tick. */
    lv_slider_set_value(progress_slider, 0, LV_ANIM_OFF);
    displayed_progress_percent = 0;
    displayed_position_second = -1;
    displayed_duration_second = -1;
    lv_label_set_text(pos_label, "0:00");
    lv_label_set_text(dur_label, "0:00");

    lv_label_set_text_fmt(song_count_label, "%d/%d", index + 1, playlist_count);
    player_transition_mark_dirty(); /* title/artist/format badge/progress reset above all just changed player_screen's own content -- see the cache's own doc comment */
}

/* Resolves which of a track's own ReplayGain fields to actually hand to
 * audio.c, per Settings -> Playback -> ReplayGain's mode: Off (no gain at
 * all), Per Track (the default -- normalizes every track to the same
 * perceived loudness), or Per Album (preserves intentional relative
 * loudness differences between tracks on the same album). Falls back to
 * track gain when Per Album is selected but this particular file has no
 * album-level tag -- most taggers only write one or the other depending on
 * whether the rip was tagged as a whole album or track-at-a-time, and
 * silently playing unnormalized instead of falling back to whatever IS
 * available would be a worse outcome than just using track gain here. */
void resolve_replaygain(const track_metadata_t * meta, bool * out_has_gain, double * out_gain_db,
                                bool * out_has_peak, double * out_peak) {
    int mode = current_settings.replaygain_mode;
    if (mode == 2 && meta->has_replaygain_album) {
        *out_has_gain = true;
        *out_gain_db = meta->replaygain_album_gain_db;
        if (meta->has_replaygain_album_peak) {
            *out_has_peak = true;
            *out_peak = meta->replaygain_album_peak;
        } else {
            *out_has_peak = meta->has_replaygain_peak;
            *out_peak = meta->replaygain_peak;
        }
        return;
    }
    bool use_track = mode != 0;
    *out_has_gain = use_track && meta->has_replaygain;
    *out_gain_db = meta->replaygain_gain_db;
    *out_has_peak = use_track && meta->has_replaygain_peak;
    *out_peak = meta->replaygain_peak;
}

/* ---- Delete confirmation popup ---- */

static void hide_delete_song_popup(void) {
    lv_obj_add_flag(delete_song_popup_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(delete_song_popup, LV_OBJ_FLAG_HIDDEN);
}

static void delete_song_popup_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_delete_song_popup();
}

static void delete_song_cancel_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_delete_song_popup();
}

static void delete_song_confirm_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_delete_song_popup();
    if (playlist_index < 0 || playlist_index >= playlist_count) return;

    char * to_delete = strdup(playlist_path_at(playlist_index));
    int del_index = playlist_index;

    /* What to play next is decided BEFORE touching the file or the array --
     * whatever ends up sitting at del_index once the deleted entry is
     * removed (i.e. what used to be right after it), or the new last track
     * if the deleted one was last. Doesn't try to honor Shuffle/Repeat here
     * -- a delete is a one-off structural change to the queue, not a
     * "what's next" playback decision. */
    free(playlist[del_index]);
    for (int i = del_index; i < playlist_count - 1; i++) playlist[i] = playlist[i + 1];
    /* Kept in lockstep -- see playlist_lazy_sort_order's own comment. */
    if (playlist_lazy_sort_order) {
        for (int i = del_index; i < playlist_count - 1; i++) playlist_lazy_sort_order[i] = playlist_lazy_sort_order[i + 1];
    }
    playlist_count--;

    if (playlist_count == 0) {
        audio_stop();
        plugin_manager_notify_stopped();
        free(playlist);
        playlist = NULL;
        playlist_index = -1;
        free(playlist_lazy_sort_order);
        playlist_lazy_sort_order = NULL;
        clear_player_source();
        set_play_button_state(false);
        lv_label_set_text(song_title_label, "No track loaded");
        nav_pop(); /* nothing left to show on the player screen */
    } else {
        int new_index = (del_index < playlist_count) ? del_index : playlist_count - 1;
        play_track_at(new_index);
    }

    unlink(to_delete); /* only after playback has moved off it */
    free(to_delete);
    show_error_toast("Song deleted");
}

/* Defined much further down, alongside build_import_rescan_popup() (the
 * shared "are you sure?" 2-button popup shape's own doc comment) --
 * forward-declared here since this and every other popup builder before
 * that point in the file needs it. */
lv_obj_t * build_confirm_popup(const char * title_text, lv_label_long_mode_t title_long_mode,
                                       lv_obj_t ** out_title, const char * body_text, const char * confirm_text,
                                       lv_color_t confirm_color, lv_event_cb_t confirm_cb, lv_obj_t ** out_confirm_row,
                                       const char * cancel_text, lv_color_t cancel_color, lv_event_cb_t cancel_cb,
                                       lv_obj_t ** out_cancel_row, lv_event_cb_t backdrop_cb, lv_obj_t ** out_backdrop);

/* Defined alongside build_confirm_popup() above -- see its own doc comment
 * for why this is a separate shared shape (an N-row menu, not a yes/no
 * confirmation) and why both are forward-declared here. */
/* menu_popup_row_t and build_menu_popup declared in gui.h */

static void build_delete_song_popup(void) {
    /* LV_LABEL_LONG_DOT, not WRAP -- this title's text is set later
     * (delete_song_confirm_prompt() below) to "Delete <filename>?..." with
     * an arbitrary-length real filename spliced in, so it needs to
     * truncate rather than potentially wrap across several lines. */
    delete_song_popup = build_confirm_popup("", LV_LABEL_LONG_DOT, &delete_song_popup_title, NULL, "Delete",
                                             lv_color_make(255, 120, 120), delete_song_confirm_cb, NULL, "Cancel",
                                             accent_lv_color(), delete_song_cancel_cb, NULL,
                                             delete_song_popup_backdrop_cb, &delete_song_popup_backdrop);
}

/* ---- The "more" 3-row menu itself ---- */

void hide_more_menu_popup(void) {
    lv_obj_add_flag(more_menu_popup_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(more_menu_popup, LV_OBJ_FLAG_HIDDEN);
}

static void more_menu_popup_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_more_menu_popup();
}

/* Defined further down, right after populate_group_songs_rows() -- needs
 * group_songs_screen/list/indices/count/title_label and
 * compact_list_scroll_to_index()/file_browser_navigate_to() all already
 * in scope, none of which are declared yet this early in the file.
 * Forward-declared here so build_more_menu_popup()'s rows table (right
 * below) can wire it up as a row's click handler. */

static void more_menu_add_to_playlist_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_more_menu_popup();
    if (playlist_index < 0) return;
    open_add_to_playlist_for(playlist_path_at(playlist_index));
}

static void more_menu_queue_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_more_menu_popup();
    open_queue_screen();
}

static void more_menu_eq_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_more_menu_popup();
    nav_push(eq_screen);
}

static void more_menu_delete_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_more_menu_popup();
    if (playlist_index < 0 || playlist_index >= playlist_count) return;

    lv_label_set_text_fmt(delete_song_popup_title, "Delete %s?\nThis cannot be undone.", basename_of(playlist_path_at(playlist_index)));
    lv_obj_remove_flag(delete_song_popup_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(delete_song_popup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(delete_song_popup_backdrop);
    lv_obj_move_foreground(delete_song_popup);
}

static void more_icon_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (playlist_index < 0) return;

    lv_obj_remove_flag(more_menu_popup_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(more_menu_popup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(more_menu_popup_backdrop);
    lv_obj_move_foreground(more_menu_popup);
}

static void build_more_menu_popup(void) {
    static const menu_popup_row_t rows[] = {
        { "List", more_menu_list_cb, false },
        { "Queue", more_menu_queue_cb, false },
        { "Add to Playlist", more_menu_add_to_playlist_cb, false },
        { "EQ", more_menu_eq_cb, false },
        { "Delete", more_menu_delete_cb, true },
    };
    more_menu_popup = build_menu_popup(rows, (int) (sizeof(rows) / sizeof(rows[0])), more_menu_popup_backdrop_cb,
                                        &more_menu_popup_backdrop);
}

/* ---- Song long-press context menu: Add to Queue / Add to Playlist / Cancel
/* Song context menu moved to gui_queue.c */

static void cover_img_tap_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_lyrics_open_screen();
}


typedef struct {
    lv_obj_t * img;
    const char * normal_path;
    const char * pressed_path;
} transport_btn_ctx_t;

static void transport_btn_press_event_cb(lv_event_t * e) {
    transport_btn_ctx_t * ctx = (transport_btn_ctx_t *) lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        lv_image_set_src(ctx->img, asset_path(ctx->pressed_path));
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        lv_image_set_src(ctx->img, asset_path(ctx->normal_path));
    }
}

static void library_btn_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_pop();
}

static lv_timer_t * pending_progress_seek_timer = NULL;
static double pending_progress_seek_seconds = 0.0;
static bool user_seeking = false;

static void cancel_pending_progress_seek(void) {
    if (pending_progress_seek_timer) {
        lv_timer_delete(pending_progress_seek_timer);
        pending_progress_seek_timer = NULL;
    }
}

static void pending_progress_seek_timer_cb(lv_timer_t * timer) {
    (void) timer;
    pending_progress_seek_timer = NULL;
    audio_seek(pending_progress_seek_seconds);
}

static void progress_slider_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * slider = lv_event_get_target(e);

    if (code == LV_EVENT_PRESSED) {
        user_seeking = true;
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        double duration = audio_get_duration_seconds();
        int32_t percent = lv_slider_get_value(slider);
        pending_progress_seek_seconds = duration * ((double) percent / 100.0);
        cancel_pending_progress_seek();
        pending_progress_seek_timer = lv_timer_create(pending_progress_seek_timer_cb, 150, NULL);
        lv_timer_set_repeat_count(pending_progress_seek_timer, 1);
        user_seeking = false;
    }
}


static lv_obj_t * build_player_screen(uint32_t screen_width, uint32_t screen_height) {
    (void) screen_width;
    (void) screen_height;
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_make(0, 0, 0), 0);

    /* Full-bleed album art (real per-track art is Task #16 -- this is the
     * firmware's own default cover placeholder, 480x480, top-aligned) plus
     * a matching 480x320 gradient panel that exactly fills the remaining
     * screen height below it (480 + 320 = 800), giving the seamless
     * art-fades-to-dark backdrop from the reference photo without needing
     * any distortion/stretching of the square art. */
    cover_img = lv_image_create(scr);
    lv_image_set_src(cover_img, asset_path("playing_plane/default_cover_565.png"));
    lv_obj_align(cover_img, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_add_flag(cover_img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(cover_img, cover_img_tap_cb, LV_EVENT_CLICKED, NULL);

    player_overlay_panel = lv_obj_create(scr);
    lv_obj_t * overlay = player_overlay_panel; /* short local alias, rest of this function was written against this name */
    lv_obj_set_size(overlay, 480, 320);
    lv_obj_align(overlay, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_image_src(overlay, asset_path("playing_plane/buttom.png"), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_set_style_radius(overlay, 0, 0);
    lv_obj_remove_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(overlay, 16, 0);
    /* Extra bottom padding, on top of pad_all's 16 -- SPACE_BETWEEN below
     * packs controls_row (the transport row: prev/play/next) flush against
     * this panel's own bottom padding edge, which otherwise put it directly
     * under the home indicator bar (see build_home_indicator_bar()) and its
     * swipe-up hit zone, confirmed overlapping on a real screenshot. Only
     * the bottom side changes -- top/left/right stay at the plain 16 set
     * above. */
    lv_obj_set_style_pad_bottom(overlay, 16 + HOME_INDICATOR_BAND_HEIGHT, 0);
    lv_obj_set_flex_flow(overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(overlay, 6, 0);
    /* Without an explicit main-axis alignment, flex defaults to packing
     * children at the top, leaving the rest of this 320px panel empty below
     * the transport row -- SPACE_BETWEEN spreads title/artist/progress/time/
     * controls out to fill the whole panel instead, controls_row landing at
     * the very bottom edge. */
    lv_obj_set_flex_align(overlay, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* Dismiss affordance over the album art, top-left -- same left-pointing
     * back arrow as every other screen's back button, for a consistent
     * back-button convention across the app. */
    /* Hitbox is deliberately larger than the visual icon (64x64 vs the
     * icon's native size) -- real-hardware testing showed taps aimed at this
     * corner landing a handful of pixels below a tight 44x44 box (finger
     * imprecision on a small corner target), so the touch area is padded out
     * generously while the icon itself stays centered at its normal size. */
    lv_obj_t * dismiss_btn = lv_obj_create(scr);
    player_dismiss_btn = dismiss_btn;
    lv_obj_set_size(dismiss_btn, 64, 64);
    lv_obj_align(dismiss_btn, LV_ALIGN_TOP_LEFT, 0, STATUS_BAR_CLEARANCE);
    lv_obj_set_style_bg_opa(dismiss_btn, 0, 0);
    lv_obj_set_style_border_width(dismiss_btn, 0, 0);
    lv_obj_remove_flag(dismiss_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(dismiss_btn, LV_OBJ_FLAG_CLICKABLE);
    /* Visibility toggled dynamically by sync_player_topbar_visibility() per
     * Settings > Display > "Hide Player/Lyrics Top Bar" -- visible here by
     * default (its normal, initial-build state) whenever the setting is
     * off. See build_flattened_transition_frame() for how the Phase 2
     * transition cache captures this button's TARGET-state visibility
     * correctly even while Player is inactive, without relying on this
     * live object's own current flag value. */
    lv_obj_add_event_cb(dismiss_btn, library_btn_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * dismiss_arrow = lv_image_create(dismiss_btn);
    lv_image_set_src(dismiss_arrow, asset_path("sub_back/btn_back.png"));
    lv_obj_center(dismiss_arrow);

    /* Title row: song title (left) + favorite icon (right) -- matches the
     * reference layout, where the 3-dot "more" menu lives in the transport
     * row below instead (repeat/prev/play/next/more), not up here. */
    lv_obj_t * title_row = lv_obj_create(overlay);
    lv_obj_set_size(title_row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(title_row, 0, 0);
    lv_obj_set_style_border_width(title_row, 0, 0);
    lv_obj_set_style_pad_all(title_row, 0, 0);
    lv_obj_remove_flag(title_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(title_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(title_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    song_title_label = lv_label_create(title_row);
    lv_label_set_text(song_title_label, "No track loaded");
    lv_obj_add_style(song_title_label, &style_theme_text_primary, 0);
    /* Explicit rather than relying on LV_FONT_DEFAULT -- see fallback_font.h,
     * this is one of the handful of labels that needs the non-Latin
     * fallback but was never otherwise styled. */
    lv_obj_set_style_text_font(song_title_label, &app_font_16, 0);
    /* Real-device bug report: a long song title just grew title_row's flex
     * child past the favorite icon instead of stopping at it -- unlike
     * row_label_enable_marquee()'s usual callers (list rows), this label had
     * no bounded width for LVGL's circular long mode to scroll within, so it
     * rendered at its full unclipped text width and overlapped the icon
     * next to it. flex_grow gives it exactly the row's remaining width
     * (title_row's width minus the icon), same as any other flex-grow
     * child, which is all LV_LABEL_LONG_SCROLL_CIRCULAR needs to know it
     * overflows and should marquee -- same shared style/2s pause as every
     * other scrolling row label in the app (row_marquee_anim). */
    lv_obj_set_flex_grow(song_title_label, 1);
    row_label_enable_marquee(song_title_label);

    favorite_icon = lv_image_create(title_row);
    lv_image_set_src(favorite_icon, asset_path("playing_plane/collect_out.png"));
    lv_obj_add_flag(favorite_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(favorite_icon, favorite_icon_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_style(favorite_icon, &icon_press_style, LV_STATE_PRESSED); /* see icon_press_style's own comment */

    /* Artist row: artist (left) + format/quality badge (right). */
    lv_obj_t * artist_row = lv_obj_create(overlay);
    lv_obj_set_size(artist_row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(artist_row, 0, 0);
    lv_obj_set_style_border_width(artist_row, 0, 0);
    lv_obj_set_style_pad_all(artist_row, 0, 0);
    lv_obj_remove_flag(artist_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(artist_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(artist_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    song_folder_label = lv_label_create(artist_row);
    lv_label_set_text(song_folder_label, "");
    lv_obj_add_style(song_folder_label, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(song_folder_label, &app_font_16, 0); /* see song_title_label's own comment above */

    format_badge_label = lv_label_create(artist_row);
    lv_label_set_text(format_badge_label, "");
    lv_obj_add_style(format_badge_label, &style_theme_text_muted, 0);

    /* Real seek bar: progress_bg/progress/cursor are all sized to their own
     * fixed 440x12 (track) / 30x30 (knob) pixel art, so the slider is given
     * those exact dimensions rather than a percentage width -- stretching
     * would blur/tile the asset. */
    progress_slider = lv_slider_create(overlay);
    lv_obj_set_size(progress_slider, 440, 12);
    lv_obj_align(progress_slider, LV_ALIGN_TOP_MID, 0, 0);
    lv_slider_set_range(progress_slider, 0, 100);
    lv_slider_set_value(progress_slider, 0, LV_ANIM_OFF);
    const lv_image_dsc_t * progress_bg_mem = asset_png_memory("playing_plane/progress_bg.png");
    const lv_image_dsc_t * progress_mem = asset_png_memory("playing_plane/progress.png");
    const lv_image_dsc_t * cursor_mem = asset_png_memory("playing_plane/cursor.png");
    lv_obj_set_style_bg_image_src(progress_slider, progress_bg_mem ? (const void *) progress_bg_mem : asset_path("playing_plane/progress_bg.png"), LV_PART_MAIN);
    lv_obj_set_style_bg_image_src(progress_slider, progress_mem ? (const void *) progress_mem : asset_path("playing_plane/progress.png"), LV_PART_INDICATOR);
    lv_obj_set_style_bg_image_src(progress_slider, cursor_mem ? (const void *) cursor_mem : asset_path("playing_plane/cursor.png"), LV_PART_KNOB);
    /* Real-device bug report: accent color didn't apply here -- see
     * apply_accent_color()'s own comment on why an image-art slider needs
     * bg_image_recolor, not just bg_color. */
    lv_obj_add_style(progress_slider, &style_accent, LV_PART_INDICATOR);
    lv_obj_add_style(progress_slider, &style_accent, LV_PART_KNOB);
    /* Keep the dedicated playing-plane art here: unlike the reused 360px
     * volume rail sprites, these assets match this progress rail's design. */
    lv_obj_set_style_radius(progress_slider, 0, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(progress_slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(progress_slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(progress_slider, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_width(progress_slider, 30, LV_PART_KNOB);
    lv_obj_set_style_height(progress_slider, 30, LV_PART_KNOB);
    /* Real-device bug report: seeking (swiping across the bar) sometimes
     * triggered the app-wide back-swipe gesture instead. Root cause: unlike
     * every other draggable slider in this file (screen_timeout_slider,
     * startup_volume_slider, sleep_timer_slider, idle_shutdown_slider, ...,
     * all of which call this with 20), this 12px-tall bar never widened its
     * touch target past LVGL's tiny built-in default (LV_DPX(8), set in
     * lv_slider_constructor). A touch landing just off that thin band missed
     * the slider's hit-test area entirely and fell through to `overlay`
     * behind it, which -- unlike the slider -- does carry
     * LV_OBJ_FLAG_GESTURE_BUBBLE (see enable_gesture_bubble_recursive()), so
     * the drag bubbled up to screen_gesture_event_cb() as a real navigation
     * swipe. */
    lv_obj_set_ext_click_area(progress_slider, 20);
    lv_obj_add_event_cb(progress_slider, progress_slider_event_cb, LV_EVENT_ALL, NULL);
    /* See screen_gesture_event_cb()'s own comment -- covers a press that
     * lands just off the slider's own hit-test box (still within
     * ext_click_area's reach for a tap, but a fast swipe's start point can
     * land outside even that) from being hijacked into a back-swipe. */
    register_swipe_dead_zone(progress_slider);

    lv_obj_t * time_row = lv_obj_create(overlay);
    lv_obj_set_size(time_row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(time_row, 0, 0);
    lv_obj_set_style_border_width(time_row, 0, 0);
    lv_obj_set_style_pad_all(time_row, 0, 0);
    lv_obj_remove_flag(time_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(time_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(time_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    pos_label = lv_label_create(time_row);
    lv_label_set_text(pos_label, "0:00");
    lv_obj_add_style(pos_label, &style_theme_text_muted, 0);

    dur_label = lv_label_create(time_row);
    lv_label_set_text(dur_label, "0:00");
    lv_obj_add_style(dur_label, &style_theme_text_muted, 0);

    /* "N/M" position within the current queue -- centered, between the
     * progress bar and the transport row below it. */
    song_count_label = lv_label_create(overlay);
    lv_label_set_text(song_count_label, "");
    lv_obj_add_style(song_count_label, &style_theme_text_muted, 0);

    /* Transport row: prev / play-pause / next, centered. */
    lv_obj_t * controls_row = lv_obj_create(overlay);
    lv_obj_set_size(controls_row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(controls_row, 0, 0);
    lv_obj_set_style_border_width(controls_row, 0, 0);
    lv_obj_set_style_pad_all(controls_row, 0, 0);
    lv_obj_set_style_pad_top(controls_row, 10, 0);
    lv_obj_remove_flag(controls_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(controls_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(controls_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(controls_row, 36, 0);

    /* Play-mode icon (sequential/repeat/shuffle) -- leftmost, matching the
     * reference layout (repeat / prev / play / next / more). Tapping cycles
     * Sequential -> Repeat All -> Repeat One -> Shuffle (order_icon_event_cb). */
    order_icon = lv_image_create(controls_row);
    lv_image_set_src(order_icon, asset_path(play_mode_icon_asset((play_mode_t) current_settings.play_mode)));
    lv_obj_add_flag(order_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(order_icon, 16);
    lv_obj_add_event_cb(order_icon, order_icon_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_style(order_icon, &icon_press_style, LV_STATE_PRESSED); /* see icon_press_style's own comment */

    lv_obj_t * prev_btn = lv_image_create(controls_row);
    lv_image_set_src(prev_btn, asset_path("playing_plane/btn_prev.png"));
    lv_obj_add_flag(prev_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(prev_btn, prev_btn_event_cb, LV_EVENT_CLICKED, NULL);
    transport_btn_ctx_t * prev_ctx = malloc(sizeof(transport_btn_ctx_t));
    *prev_ctx = (transport_btn_ctx_t){ prev_btn, "playing_plane/btn_prev.png", "playing_plane/btn_prev_s.png" };
    lv_obj_add_event_cb(prev_btn, transport_btn_press_event_cb, LV_EVENT_PRESSED, prev_ctx);
    lv_obj_add_event_cb(prev_btn, transport_btn_press_event_cb, LV_EVENT_RELEASED, prev_ctx);
    lv_obj_add_event_cb(prev_btn, transport_btn_press_event_cb, LV_EVENT_PRESS_LOST, prev_ctx);

    play_btn = lv_image_create(controls_row);
    lv_image_set_src(play_btn, asset_path("playing_plane/btn_play.png"));
    lv_obj_add_flag(play_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(play_btn, play_btn_event_cb, LV_EVENT_CLICKED, NULL);
    /* Not transport_btn_ctx_t's fixed normal/pressed asset-swap -- this
     * icon's own "normal" image already alternates between btn_play.png and
     * btn_pause.png depending on playback state (set_play_button_state()),
     * so a fixed pressed_path would flash the wrong artwork half the time.
     * icon_press_style dims whichever of the two is currently showing
     * instead. */
    lv_obj_add_style(play_btn, &icon_press_style, LV_STATE_PRESSED);

    lv_obj_t * next_btn = lv_image_create(controls_row);
    lv_image_set_src(next_btn, asset_path("playing_plane/btn_next.png"));
    lv_obj_add_flag(next_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(next_btn, next_btn_event_cb, LV_EVENT_CLICKED, NULL);
    transport_btn_ctx_t * next_ctx = malloc(sizeof(transport_btn_ctx_t));
    *next_ctx = (transport_btn_ctx_t){ next_btn, "playing_plane/btn_next.png", "playing_plane/btn_next_s.png" };
    lv_obj_add_event_cb(next_btn, transport_btn_press_event_cb, LV_EVENT_PRESSED, next_ctx);
    lv_obj_add_event_cb(next_btn, transport_btn_press_event_cb, LV_EVENT_RELEASED, next_ctx);
    lv_obj_add_event_cb(next_btn, transport_btn_press_event_cb, LV_EVENT_PRESS_LOST, next_ctx);

    /* 3-dot "more" menu -- rightmost, matching the reference layout. Opens
     * more_menu_popup (Add to Playlist / EQ / Delete). */
    lv_obj_t * more_icon = lv_image_create(controls_row);
    lv_image_set_src(more_icon, asset_path("playing_plane/ic_more.png"));
    lv_obj_add_flag(more_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(more_icon, 16);
    lv_obj_add_event_cb(more_icon, more_icon_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_style(more_icon, &icon_press_style, LV_STATE_PRESSED); /* see icon_press_style's own comment */

    /* Volume is controlled via hardware buttons (see update_timer_cb) and,
     * per the real device, shown only as a transient overlay rather than a
     * permanently visible slider (Task #28) -- kept alive here, just
     * invisible, so the existing hw-button volume logic keeps working
     * unchanged until that overlay lands. */
    volume_slider = lv_slider_create(scr);
    lv_obj_add_flag(volume_slider, LV_OBJ_FLAG_HIDDEN);
    lv_slider_set_range(volume_slider, 0, 100);
    lv_slider_set_value(volume_slider, (int32_t) (audio_get_volume() * 100.0f), LV_ANIM_OFF);
    lv_obj_add_event_cb(volume_slider, volume_slider_event_cb, LV_EVENT_ALL, NULL);

    finalize_screen_navigation(scr);
    return scr;
}




void show_volume_popup(int32_t percent) {
    if (!volume_popup || !volume_popup_track) return;
    lv_slider_set_value(volume_popup_track, percent, LV_ANIM_OFF);
    lv_obj_remove_flag(volume_popup, LV_OBJ_FLAG_HIDDEN);
    if (volume_popup_hide_timer) {
        lv_timer_reset(volume_popup_hide_timer);
        lv_timer_resume(volume_popup_hide_timer);
    }
}

void refresh_format_badge(void) {
    if (playlist_index < 0) {
        if (format_badge_label) lv_label_set_text(format_badge_label, "");
        return;
    }

    const char * path = playlist_path_at(playlist_index);

    remote_track_meta_t remote_meta;
    bool is_remote_track = remote_track_meta_copy_for_path(path, &remote_meta);
    char ext[16] = "";
    if (is_remote_track && remote_meta.codec[0]) {
        size_t i = 0;
        for (const char * p = remote_meta.codec; *p && i < sizeof(ext) - 1; p++, i++) {
            ext[i] = (char) toupper((unsigned char) *p);
        }
        ext[i] = ' ';
    } else if (!is_remote_track) {
        const char * dot = strrchr(path, '.');
        if (dot) {
            size_t i = 0;
            for (const char * p = dot + 1; *p && i < sizeof(ext) - 1; p++, i++) {
                ext[i] = (char) toupper((unsigned char) *p);
            }
            ext[i] = ' ';
        }
    }

    unsigned int sample_rate = audio_get_sample_rate();
    if (format_badge_label) {
        if (sample_rate > 0) {
            lv_label_set_text_fmt(format_badge_label, "%s  %.1fkHz", ext, sample_rate / 1000.0);
        } else {
            lv_label_set_text(format_badge_label, ext);
        }
    }
}


void gui_player_init(uint32_t screen_width, uint32_t screen_height) {
    build_volume_popup();
    build_delete_song_popup();
    build_more_menu_popup();
    player_screen = build_player_screen(screen_width, screen_height);
}


static void format_time(double seconds, char * buf, size_t buf_size) {
    if (seconds < 0) seconds = 0;
    int total = (int) seconds;
    snprintf(buf, buf_size, "%d:%02d", total / 60, total % 60);
}

void gui_player_update_progress(void) {
    double position = audio_get_position_seconds();
    double duration = audio_get_duration_seconds();

    if (duration > 0 && progress_slider) {
        int32_t percent = (int32_t) ((position / duration) * 100.0);
        if (percent != displayed_progress_percent) {
            displayed_progress_percent = percent;
            lv_slider_set_value(progress_slider, percent, LV_ANIM_OFF);
        }
    }

    int position_second = (int) position;
    int duration_second = (int) duration;
    if (pos_label && position_second != displayed_position_second) {
        char pos_str[16];
        displayed_position_second = position_second;
        format_time(position, pos_str, sizeof(pos_str));
        lv_label_set_text(pos_label, pos_str);
    }
    if (dur_label && duration_second != displayed_duration_second) {
        char dur_str[16];
        displayed_duration_second = duration_second;
        format_time(duration, dur_str, sizeof(dur_str));
        lv_label_set_text(dur_label, dur_str);
    }
}
