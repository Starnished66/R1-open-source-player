#include "gui_lyrics.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <stdatomic.h>
#include "lvgl/lvgl.h"
#include "assets.h"
#include "gui.h"

#include "settings.h"
#include "metadata.h"
#include <limits.h>

#include "lyrics.h"
#include "lyrics_layout.h"
#include "fallback_font.h"

typedef struct {
    int tier;
    const char * label;
} lyrics_font_size_option_t;

#define LYRICS_BACKDROP_WIDTH 480
#define LYRICS_BACKDROP_HEIGHT 800
#define LYRICS_BACKDROP_WORK_WIDTH 60
#define LYRICS_BACKDROP_WORK_HEIGHT 100
#define LYRICS_BACKDROP_BLUR_RADIUS 5
#define LYRICS_BACKDROP_BLUR_PASSES 3
#define LYRICS_BACKDROP_DARKEN_NUM 9
#define LYRICS_BACKDROP_DARKEN_DEN 20
#define LYRICS_POOL_SIZE 20
#define LYRICS_ROW_WIDTH 440
#define LYRICS_ROW_GAP 24
#define LYRICS_ACTIVE_LINE_ANCHOR_Y 200
#define LYRICS_TOP_PAD LYRICS_ACTIVE_LINE_ANCHOR_Y
#define LYRICS_TIMER_PERIOD_MS 150
#define LYRICS_AUTO_FOLLOW_RESUME_MS 3000L
#define LYRICS_FONT_SIZE_OPTION_COUNT (sizeof(lyrics_font_size_options) / sizeof(lyrics_font_size_options[0]))

extern lv_font_t app_font_lyrics;
extern void enable_gesture_bubble_recursive(lv_obj_t * parent);
extern lv_obj_t * add_pill_row_base(lv_obj_t * parent, const char * text);
extern lv_color_t accent_lv_color(void);
extern lv_obj_t * build_subsonic_list_screen(const char * title, lv_obj_t ** out_title_label, lv_obj_t ** out_list);
extern void show_error_toast(const char * msg);
extern void gui_navigation_invalidate_font_snapshots(void);


extern lv_style_t style_theme_screen_bg;
extern lv_style_t style_theme_text_primary;
extern lv_style_t style_theme_text_secondary;
extern lv_style_t style_theme_row;
extern lv_style_t style_theme_text_muted;
extern lv_style_t style_theme_list_padding;
extern lv_style_t style_button_pressed;

extern void nav_push(lv_obj_t * screen);
extern void nav_pop(void);
extern void nav_reset_to_home(void);
extern void finalize_screen_navigation(lv_obj_t * screen);
extern void generic_back_cb(lv_event_t * e);

static lv_obj_t * lyrics_screen;
/* ---- Fullscreen lyrics view state -- see build_lyrics_screen() and the
 * async .lrc load / backdrop generation just above poll_cover_decode()'s
 * own section. All declared here because the async load/poll functions and
 * the screen builder that creates the LVGL objects live in different parts
 * of this file. ---- */
static lyrics_doc_t current_lyrics_doc;
static bool current_lyrics_doc_valid = false;
static int current_lyrics_doc_for_index = -1; /* gui_player_get_playlist_index() current_lyrics_doc was loaded for */
static int current_lyrics_doc_generation = -1;
static int lyrics_load_generation = 0;
/* Real-device request: embedded lyrics tags (ID3 USLT, FLAC/Opus LYRICS/
 * UNSYNCEDLYRICS, M4A "\xA9lyr") are usually plain unsynced text with no
 * [mm:ss.xx] timestamps -- despite the name, that's what "unsynced" means
 * for 3 of those 4 tag names. current_lyrics_doc_valid/current_lyrics_doc
 * above stay reserved for the synced case (an .lrc sidecar, or the less
 * common tagger that does embed real LRC text in one of those fields);
 * this is the parallel "just show the raw text as a static block, no
 * per-line highlight or auto-follow" case -- mutually exclusive with
 * current_lyrics_doc_valid, same as their lyrics_load_result_* counterparts
 * above. current_lyrics_plain_text is malloc'd, owned by these globals
 * once poll_lyrics_load() transfers it; freed there on the next load and
 * whenever current_lyrics_plain_mode is cleared. */
extern void box_blur_1d(const uint8_t * src, uint8_t * dst, int length, int stride, int radius);


typedef struct {
    uint8_t * cover_copy;
    int for_index;
    int lyrics_generation;
} lyrics_backdrop_request_t;


extern uint16_t rgb888_to_565_dithered(uint8_t r, uint8_t g, uint8_t b, int x, int y);
extern void audio_seek(double seconds);
extern double audio_get_position_seconds(void);

typedef struct {
    int generation;
    int for_index;
    char track_path[PATH_MAX];
} lyrics_load_request_t;
extern player_settings_t current_settings;
extern lv_font_t app_font_22;
extern void sync_player_topbar_visibility(lv_obj_t * screen);
static uint8_t bilerp_plane(const uint8_t * plane, int width, int height, uint32_t x_fp, uint32_t y_fp) {
    int x0 = (int) (x_fp >> 16), y0 = (int) (y_fp >> 16);
    int x1 = x0 + 1 < width ? x0 + 1 : x0;
    int y1 = y0 + 1 < height ? y0 + 1 : y0;
    uint32_t fx = x_fp & 0xffffu, fy = y_fp & 0xffffu;
    uint32_t top = ((uint32_t) plane[y0 * width + x0] * (65536u - fx) +
                    (uint32_t) plane[y0 * width + x1] * fx) >> 16;
    uint32_t bottom = ((uint32_t) plane[y1 * width + x0] * (65536u - fx) +
                       (uint32_t) plane[y1 * width + x1] * fx) >> 16;
    return (uint8_t) ((top * (65536u - fy) + bottom * fy) >> 16);
}

static void blur_plane_three_passes(uint8_t * plane, uint8_t * tmp, int width, int height) {
    for (int pass = 0; pass < LYRICS_BACKDROP_BLUR_PASSES; pass++) {
        for (int y = 0; y < height; y++) {
            box_blur_1d(plane + y * width, tmp + y * width, width, 1, LYRICS_BACKDROP_BLUR_RADIUS);
        }
        memcpy(plane, tmp, (size_t) width * height);
        for (int x = 0; x < width; x++) {
            box_blur_1d(plane + x, tmp + x, height, width, LYRICS_BACKDROP_BLUR_RADIUS);
        }
        memcpy(plane, tmp, (size_t) width * height);
    }
}

bool current_lyrics_plain_mode = false;
static char * current_lyrics_plain_text = NULL;
static lv_obj_t * lyrics_backdrop_img;
static lv_obj_t * lyrics_list;
static lv_obj_t * lyrics_spacer;
static lv_obj_t * lyrics_empty_label;
static lv_obj_t * lyrics_plain_label; /* current_lyrics_plain_mode's own display -- see its own comment */
static lv_timer_t * lyrics_timer;
static uint8_t * current_lyrics_backdrop_bytes;
static lv_image_dsc_t current_lyrics_backdrop_dsc;
static void launch_lyrics_backdrop_decode(void); /* defined alongside build_lyrics_screen() below -- see poll_cover_decode()'s own use of it */
static pthread_t lyrics_load_thread;
static bool lyrics_load_active = false;
/* atomic_bool rather than a plain volatile bool -- audit finding: a plain
 * bool has no C11-recognized ordering between the worker thread's write
 * here and the UI thread's poll read, the same formally-a-data-race
 * pattern already fixed for compact_list_fetch_job_t.result_count
 * (screen_builders.c) elsewhere in this session's diff. Cost-free to make
 * consistent: plain assignment/comparison on an _Atomic-qualified object
 * already uses sequentially consistent ordering by default in C11, so
 * every read/write site below is unchanged syntactically. */
static atomic_bool lyrics_load_done_flag = false;
static int lyrics_load_result_generation;
static int lyrics_load_result_for_index;
static bool lyrics_load_result_ok;
static lyrics_doc_t lyrics_load_result_doc;
/* Set alongside lyrics_load_result_ok when a source (sidecar or embedded
 * tag) had text but no [mm:ss.xx] timestamps to parse into lyrics_load_
 * result_doc -- see lyrics_load_thread_func()'s own comment. Mutually
 * exclusive with lyrics_load_result_ok: exactly one of the two, or neither
 * (no lyrics found anywhere), is ever true for a given load. Owned by the
 * worker thread until the done flag flips, same as lyrics_load_result_doc. */
static bool lyrics_load_result_plain_mode;
static char * lyrics_load_result_plain_text; /* malloc'd, only meaningful when lyrics_load_result_plain_mode */
/* Bumped on every launch_lyrics_load() call (both a real track change and
 * the internal pending-relaunch below) -- the identity a finished result is
 * checked against in poll_lyrics_load(), not gui_player_get_playlist_index(). gui_player_get_playlist_index()
 * is a numeric SLOT, not a track identity: if the whole playlist is
 * replaced (a different album/Subsonic queue/etc. loaded) while a load is
 * still in flight, the new playlist's own track at that same slot number
 * can easily be a completely different song, and an index-only check would
 * wrongly accept the stale result for it. A strictly increasing counter
 * has no such collision. */
static bool lyrics_load_pending_valid = false;
static int lyrics_load_pending_for_index;
static char lyrics_load_pending_path[PATH_MAX];
static void * lyrics_load_thread_func(void * arg) {
    lyrics_load_request_t * req = (lyrics_load_request_t *) arg;
    lyrics_doc_t doc;
    bool ok = lyrics_load_sidecar(req->track_path, &doc);
    bool plain_mode = false;
    char * plain_text = NULL;

    /* Real-device request: fall back to the track's own embedded lyrics tag
     * (ID3 USLT, FLAC/Opus LYRICS/UNSYNCEDLYRICS, M4A "\xA9lyr" -- see
     * metadata_read()'s own doc comment) when there's no .lrc sidecar.
     * Independent metadata_read() call rather than sharing apply_track_
     * metadata_to_ui()'s own -- see that function's comment on why (this
     * thread already does its own file I/O regardless, so re-parsing tags
     * here is cheaper than plumbing a pointer through two independent async
     * paths that don't otherwise share state). Tries lrc parsing first
     * (some taggers do embed real [mm:ss.xx] text in these fields despite
     * "unsynced" being right there in 3 of the 4 tag names); anything with
     * text but no usable timestamp lines falls back to plain_mode, a static
     * unsynced block, rather than showing nothing. */
    if (!ok) {
        track_metadata_t meta;
        metadata_read(req->track_path, &meta);
        if (meta.lyrics) {
            if (lyrics_parse_buffer(meta.lyrics, strlen(meta.lyrics), &doc)) {
                ok = true;
            } else {
                plain_mode = true;
                plain_text = meta.lyrics;
                meta.lyrics = NULL; /* ownership transferred to plain_text -- don't free it below */
            }
        }
        free(meta.picture_data);
        free(meta.lyrics);
    }

    lyrics_load_result_generation = req->generation;
    lyrics_load_result_for_index = req->for_index;
    lyrics_load_result_ok = ok;
    if (ok) lyrics_load_result_doc = doc;
    lyrics_load_result_plain_mode = plain_mode;
    lyrics_load_result_plain_text = plain_text;
    free(req);
    atomic_store_explicit(&lyrics_load_done_flag, true, memory_order_release); /* written last -- poll_lyrics_load() only checks this flag */
    return NULL;
}
static void launch_lyrics_load(int for_index, const char * track_path) {
    lyrics_load_generation++;
    if (lyrics_load_active) {
        lyrics_load_pending_for_index = for_index;
        snprintf(lyrics_load_pending_path, sizeof(lyrics_load_pending_path), "%s", track_path);
        lyrics_load_pending_valid = true;
        return;
    }

    lyrics_load_request_t * req = malloc(sizeof(*req));
    if (!req) return;
    req->generation = lyrics_load_generation;
    req->for_index = for_index;
    snprintf(req->track_path, sizeof(req->track_path), "%s", track_path);
    atomic_store_explicit(&lyrics_load_done_flag, false, memory_order_relaxed);
    lyrics_load_active = true;
    if (pthread_create(&lyrics_load_thread, NULL, lyrics_load_thread_func, req) != 0) {
        free(req);
        lyrics_load_active = false;
    }
}
/* Called every tick from update_timer_cb, same as poll_cover_decode() --
 * regardless of whether the lyrics screen is actually open, so a load
 * finishes well before the user ever taps the album art. Deliberately does
 * not touch any lyrics-screen LVGL object directly (unlike poll_cover_
 * decode(), which owns gui_player_get_cover_img()/player_overlay_panel outright) -- open_
 * lyrics_screen() and lyrics_timer_cb() below independently notice when
 * current_lyrics_doc_for_index no longer matches what the pool was last
 * built from and resync from scratch at that point, so there is exactly
 * one place that reconciles the parsed doc with the on-screen pool. */
static void poll_lyrics_load(void) {
    if (!lyrics_load_active || !atomic_load_explicit(&lyrics_load_done_flag, memory_order_acquire)) return;
    lyrics_load_active = false;
    pthread_join(lyrics_load_thread, NULL);

    if (lyrics_load_result_generation == lyrics_load_generation) {
        if (current_lyrics_doc_valid) lyrics_doc_free(&current_lyrics_doc);
        current_lyrics_doc_valid = lyrics_load_result_ok;
        if (lyrics_load_result_ok) current_lyrics_doc = lyrics_load_result_doc;
        current_lyrics_doc_for_index = lyrics_load_result_for_index;
        current_lyrics_doc_generation = lyrics_load_result_generation;

        free(current_lyrics_plain_text);
        current_lyrics_plain_mode = lyrics_load_result_plain_mode;
        current_lyrics_plain_text = lyrics_load_result_plain_text;

        if (current_lyrics_doc_valid || current_lyrics_plain_mode)
            launch_lyrics_backdrop_decode();
    } else {
        if (lyrics_load_result_ok) lyrics_doc_free(&lyrics_load_result_doc); /* stale -- superseded by a later launch before this landed */
        free(lyrics_load_result_plain_text);
    }

    if (lyrics_load_pending_valid) {
        lyrics_load_pending_valid = false;
        launch_lyrics_load(lyrics_load_pending_for_index, lyrics_load_pending_path);
    }
}
static pthread_t lyrics_backdrop_thread;
static bool lyrics_backdrop_active = false;
static atomic_bool lyrics_backdrop_done_flag = false; /* atomic_bool, not volatile -- see lyrics_load_done_flag's own doc comment */
static uint8_t * lyrics_backdrop_result_bytes;
static int lyrics_backdrop_result_for_index = -1;
static int lyrics_backdrop_result_generation = -1;
static int lyrics_backdrop_active_for_index = -1;
static int lyrics_backdrop_active_generation = -1;
static int current_lyrics_backdrop_for_index = -1;
static int current_lyrics_backdrop_generation = -1;
/* Center-crop the square cover to the display's portrait aspect ratio,
 * sample it bilinearly into a half-resolution work image, apply three box
 * passes (a close bounded approximation of a Gaussian blur), then bilinear-
 * upscale while repacking.  This removes the old nearest-neighbor vertical
 * stretch and is both smoother and cheaper in RAM/CPU than blurring 480x800
 * directly. Pure pixel math: safe on the backdrop worker thread. */
static uint8_t * compute_lyrics_backdrop_bytes(const uint8_t * cover_bytes) {
    int w = LYRICS_BACKDROP_WORK_WIDTH, h = LYRICS_BACKDROP_WORK_HEIGHT;
    uint8_t * r = malloc((size_t) w * h);
    uint8_t * g = malloc((size_t) w * h);
    uint8_t * b = malloc((size_t) w * h);
    uint8_t * tmp = malloc((size_t) w * h);
    if (!r || !g || !b || !tmp) {
        free(r); free(g); free(b); free(tmp);
        return NULL;
    }

    const uint16_t * source = (const uint16_t *) cover_bytes;
    int crop_w = COVER_ART_HEIGHT * LYRICS_BACKDROP_WIDTH / LYRICS_BACKDROP_HEIGHT;
    int crop_x = (COVER_ART_WIDTH - crop_w) / 2;
    for (int y = 0; y < h; y++) {
        uint32_t sy_fp = h > 1 ? (uint32_t) (((uint64_t) y * (COVER_ART_HEIGHT - 1) << 16) / (h - 1)) : 0;
        int sy = (int) (sy_fp >> 16);
        int sy1 = sy + 1 < COVER_ART_HEIGHT ? sy + 1 : sy;
        uint32_t fy = sy_fp & 0xffffu;
        for (int x = 0; x < w; x++) {
            uint32_t sx_fp = (uint32_t) crop_x << 16;
            if (w > 1) sx_fp += (uint32_t) (((uint64_t) x * (crop_w - 1) << 16) / (w - 1));
            int sx = (int) (sx_fp >> 16);
            int sx1 = sx + 1 < COVER_ART_WIDTH ? sx + 1 : sx;
            uint32_t fx = sx_fp & 0xffffu;
            uint16_t pixels[4] = {
                source[sy * COVER_ART_WIDTH + sx], source[sy * COVER_ART_WIDTH + sx1],
                source[sy1 * COVER_ART_WIDTH + sx], source[sy1 * COVER_ART_WIDTH + sx1]
            };
            uint8_t * channels[3] = { r, g, b };
            for (int c = 0; c < 3; c++) {
                uint32_t values[4];
                for (int p = 0; p < 4; p++) {
                    if (c == 0) values[p] = ((pixels[p] >> 11) & 31) * 255 / 31;
                    else if (c == 1) values[p] = ((pixels[p] >> 5) & 63) * 255 / 63;
                    else values[p] = (pixels[p] & 31) * 255 / 31;
                }
                uint32_t top = (values[0] * (65536u - fx) + values[1] * fx) >> 16;
                uint32_t bottom = (values[2] * (65536u - fx) + values[3] * fx) >> 16;
                channels[c][y * w + x] = (uint8_t) ((top * (65536u - fy) + bottom * fy) >> 16);
            }
        }
    }

    blur_plane_three_passes(r, tmp, w, h);
    blur_plane_three_passes(g, tmp, w, h);
    blur_plane_three_passes(b, tmp, w, h);

    uint8_t * out_bytes = malloc((size_t) LYRICS_BACKDROP_WIDTH * LYRICS_BACKDROP_HEIGHT * 2);
    if (!out_bytes) {
        free(r); free(g); free(b); free(tmp);
        return NULL;
    }
    uint16_t * out = (uint16_t *) out_bytes;
    for (int y = 0; y < LYRICS_BACKDROP_HEIGHT; y++) {
        uint32_t y_fp = (uint32_t) (((uint64_t) y * (h - 1) << 16) / (LYRICS_BACKDROP_HEIGHT - 1));
        for (int x = 0; x < LYRICS_BACKDROP_WIDTH; x++) {
            uint32_t x_fp = (uint32_t) (((uint64_t) x * (w - 1) << 16) / (LYRICS_BACKDROP_WIDTH - 1));
            int rv = bilerp_plane(r, w, h, x_fp, y_fp) * LYRICS_BACKDROP_DARKEN_NUM / LYRICS_BACKDROP_DARKEN_DEN;
            int gv = bilerp_plane(g, w, h, x_fp, y_fp) * LYRICS_BACKDROP_DARKEN_NUM / LYRICS_BACKDROP_DARKEN_DEN;
            int bv = bilerp_plane(b, w, h, x_fp, y_fp) * LYRICS_BACKDROP_DARKEN_NUM / LYRICS_BACKDROP_DARKEN_DEN;
            out[y * LYRICS_BACKDROP_WIDTH + x] = rgb888_to_565_dithered(rv, gv, bv, x, y);
        }
    }
    free(r); free(g); free(b); free(tmp);
    return out_bytes;
}
static void * lyrics_backdrop_thread_func(void * arg) {
    lyrics_backdrop_request_t * req = (lyrics_backdrop_request_t *) arg;
    lyrics_backdrop_result_bytes = compute_lyrics_backdrop_bytes(req->cover_copy);
    lyrics_backdrop_result_for_index = req->for_index;
    lyrics_backdrop_result_generation = req->lyrics_generation;
    free(req->cover_copy);
    free(req);
    atomic_store_explicit(&lyrics_backdrop_done_flag, true, memory_order_release); /* written last, same contract as every other _done_flag in this file */
    return NULL;
}
/* Set by launch_lyrics_backdrop_decode() when a request arrives while a
 * generation is already running for a since-superseded track -- real bug
 * report: silently dropping that request (the old behavior) left the OLD
 * track's backdrop permanently stuck once the in-flight job's stale result
 * was applied by poll_lyrics_backdrop(), since nothing else ever retried
 * for the track actually playing now. poll_lyrics_backdrop() checks this
 * once the in-flight job lands and immediately kicks off a fresh
 * generation for whatever's current at that point. */
static bool lyrics_backdrop_regenerate_pending = false;
/* No queuing beyond the single pending flag above (never more than one
 * generation queued behind the active one) -- unlike cover art's own
 * cover_decode_pending, a slightly-stale intermediate backdrop for a
 * moment isn't a visible correctness bug, and this is called far less
 * often (screen-open/track-change-while-open, not every track change).
 * Also skips if gui_player's decoded cover isn't ready yet (rare: tapped
 * during the brief cover-decode window). poll_cover_decode() notifies this
 * module once it becomes available, so that attempt is retried without
 * blocking the tap. */
static void launch_lyrics_backdrop_decode(void) {
    if (current_lyrics_doc_generation != lyrics_load_generation ||
        current_lyrics_doc_for_index != gui_player_get_playlist_index() ||
        (!current_lyrics_doc_valid && !current_lyrics_plain_mode)) return;
    if (current_lyrics_backdrop_for_index == gui_player_get_playlist_index() &&
        current_lyrics_backdrop_generation == lyrics_load_generation) return;
    if (lyrics_backdrop_active) {
        if (lyrics_backdrop_active_for_index == gui_player_get_playlist_index() &&
            lyrics_backdrop_active_generation == lyrics_load_generation) return;
        lyrics_backdrop_regenerate_pending = true;
        return;
    }
    lyrics_backdrop_request_t * req = malloc(sizeof(*req));
    if (!req) return;
    req->cover_copy = malloc((size_t) COVER_ART_WIDTH * COVER_ART_HEIGHT * 2);
    if (!req->cover_copy) { free(req); return; }
    req->for_index = gui_player_get_playlist_index();
    if (!gui_player_copy_cover_rgb565(req->for_index, req->cover_copy,
                                     (size_t) COVER_ART_WIDTH * COVER_ART_HEIGHT * 2)) {
        free(req->cover_copy);
        free(req);
        return;
    }
    req->lyrics_generation = lyrics_load_generation;

    atomic_store_explicit(&lyrics_backdrop_done_flag, false, memory_order_relaxed);
    lyrics_backdrop_active = true;
    lyrics_backdrop_active_for_index = req->for_index;
    lyrics_backdrop_active_generation = req->lyrics_generation;
    if (pthread_create(&lyrics_backdrop_thread, NULL, lyrics_backdrop_thread_func, req) != 0) {
        free(req->cover_copy);
        free(req);
        lyrics_backdrop_active = false;
    }
}
/* Applies a finished backdrop generation to lyrics_backdrop_img -- called
 * from lyrics_timer_cb() (only ticking while the lyrics screen is open)
 * rather than update_timer_cb(), so it resolves within one 150 ms tick of
 * becoming ready instead of waiting up to 500 ms. Same lv_image_dsc_t
 * construction care as current_cover_dsc/current_reflection_dsc in poll_
 * cover_decode() above -- memset to 0 first, then explicitly set header.
 * magic, or LVGL's bin decoder silently corrupts the color format (see that
 * function's own real-device-incident comment for the full story). */
static void poll_lyrics_backdrop(void) {
    if (!lyrics_backdrop_active || !atomic_load_explicit(&lyrics_backdrop_done_flag, memory_order_acquire)) return;
    lyrics_backdrop_active = false;
    pthread_join(lyrics_backdrop_thread, NULL);

    bool result_current = lyrics_backdrop_result_for_index == gui_player_get_playlist_index() &&
                          lyrics_backdrop_result_generation == lyrics_load_generation;
    if (lyrics_backdrop_result_bytes && result_current) { /* NULL = allocation failure -- keep whatever backdrop (or plain black) is already showing */
        free(current_lyrics_backdrop_bytes);
        current_lyrics_backdrop_bytes = lyrics_backdrop_result_bytes;
        lyrics_backdrop_result_bytes = NULL;
        current_lyrics_backdrop_for_index = lyrics_backdrop_result_for_index;
        current_lyrics_backdrop_generation = lyrics_backdrop_result_generation;

        memset(&current_lyrics_backdrop_dsc, 0, sizeof(current_lyrics_backdrop_dsc));
        current_lyrics_backdrop_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        current_lyrics_backdrop_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
        current_lyrics_backdrop_dsc.header.w = LYRICS_BACKDROP_WIDTH;
        current_lyrics_backdrop_dsc.header.h = LYRICS_BACKDROP_HEIGHT;
        current_lyrics_backdrop_dsc.header.stride = LYRICS_BACKDROP_WIDTH * 2;
        current_lyrics_backdrop_dsc.data = current_lyrics_backdrop_bytes;
        current_lyrics_backdrop_dsc.data_size = (uint32_t) LYRICS_BACKDROP_WIDTH * LYRICS_BACKDROP_HEIGHT * 2;
        lv_image_set_src(lyrics_backdrop_img, &current_lyrics_backdrop_dsc);
        if (lv_screen_active() == lyrics_screen)
            lv_obj_remove_flag(lyrics_backdrop_img, LV_OBJ_FLAG_HIDDEN);
    } else {
        free(lyrics_backdrop_result_bytes);
        lyrics_backdrop_result_bytes = NULL;
    }

    /* Checked regardless of the success/failure branch above -- see this
     * flag's own doc comment; a request that arrived while the job that
     * just landed was still running must not be lost either way. */
    if (lyrics_backdrop_regenerate_pending) {
        lyrics_backdrop_regenerate_pending = false;
        launch_lyrics_backdrop_decode();
    }
}
static void open_lyrics_screen(void); /* defined alongside build_lyrics_screen() below */
static lv_obj_t * lyrics_rows[LYRICS_POOL_SIZE];
/* Prefix positions for the current document: offsets[i] is row i's Y and
 * offsets[count] is the bottom after its final gap. Long lines can wrap to
 * an arbitrary number of visual lines (up to LYRICS_MAX_LINE_BYTES), so a
 * fixed "three lines ought to fit" stride still allowed real text to paint
 * into its neighbor. This table costs at most ~16KB for 4000 lines and
 * makes rendering, hit mapping and auto-follow share the exact measured
 * geometry. */
static lyrics_layout_t lyrics_layout;
static int lyrics_window_start = -1; /* index currently shown by lyrics_rows[0]; -1 forces the first update to actually run */
static int lyrics_pool_synced_for_index = -1; /* current_lyrics_doc_for_index the pool/spacer were last built from */
static bool lyrics_auto_follow = true;
static int lyrics_last_centered_index = -2; /* -2 = "never centered yet", distinct from -1 (a real "before the first line" position) */
static struct timespec lyrics_last_manual_scroll_at;
/* Only current_lyrics_doc_for_index == gui_player_get_playlist_index() counts as "this
 * doc is for the track actually playing" -- a load still in flight, one
 * that finished for a since-superseded track, or simply having no track
 * playing at all, are all "no active line" here, matching poll_lyrics_
 * load()'s own staleness check above. */
static int lyrics_current_active_index(void) {
    if (!current_lyrics_doc_valid || current_lyrics_doc_for_index != gui_player_get_playlist_index() || gui_player_get_playlist_index() < 0) return -1;
    return lyrics_find_line_at(&current_lyrics_doc, audio_get_position_seconds());
}
static int32_t lyrics_line_y(int index) {
    return lyrics_layout_y(&lyrics_layout, index);
}
static int32_t lyrics_line_height(int index) {
    return lyrics_layout_height(&lyrics_layout, index);
}
static void lyrics_build_line_offsets(void) {
    lyrics_layout_build(&lyrics_layout, current_lyrics_doc_valid ? &current_lyrics_doc : NULL,
                        &app_font_lyrics, LYRICS_ROW_WIDTH, LYRICS_ROW_GAP, LYRICS_TOP_PAD);
}
static int lyrics_first_line_at_y(int32_t y, int count) {
    return lyrics_layout_first_at_y(&lyrics_layout, y, count);
}
/* Re-syncs the row pool/spacer/empty-state to whatever current_lyrics_doc
 * currently holds -- called on every open (open_lyrics_screen() below) and
 * whenever lyrics_timer_cb() notices the doc changed while already open
 * (a track change while the user is looking at this screen). Always resets
 * scroll to the top and re-enables auto-follow, so both a fresh open and a
 * new track start from the same, predictable state rather than wherever a
 * previous track's manual scroll happened to leave it. */
static void lyrics_reset_pool(void) {
    int count = current_lyrics_doc_valid ? current_lyrics_doc.count : 0;
    lyrics_build_line_offsets();

    /* current_lyrics_plain_mode -- see its own doc comment: a single static
     * text block, no per-line pool/highlight/auto-follow at all. Handled as
     * its own branch, entirely separate from the synced count==0/count>0
     * cases below, since "0 synced lines" (count==0) still needs the
     * "No synchronized lyrics found" empty state when plain mode ISN'T
     * active either (genuinely no lyrics anywhere). */
    if (current_lyrics_plain_mode && current_lyrics_plain_text) {
        lv_obj_add_flag(lyrics_empty_label, LV_OBJ_FLAG_HIDDEN);
        for (int slot = 0; slot < LYRICS_POOL_SIZE; slot++) lv_obj_add_flag(lyrics_rows[slot], LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(lyrics_plain_label, current_lyrics_plain_text);
        lv_obj_remove_flag(lyrics_plain_label, LV_OBJ_FLAG_HIDDEN);

        /* LV_LABEL_LONG_WRAP's real rendered height depends on the text
         * just set and isn't known until a layout pass runs -- unlike the
         * synced case's fixed per-line stride, this can't be computed from
         * a line count. lv_obj_update_layout() forces that pass immediately
         * so lv_obj_get_height() below reflects the real wrapped height,
         * not a stale one from whatever text was showing before. */
        lv_obj_update_layout(lyrics_plain_label);
        int32_t total_height = LYRICS_TOP_PAD + lv_obj_get_height(lyrics_plain_label) + LYRICS_TOP_PAD;
        lv_obj_set_pos(lyrics_spacer, 0, total_height > 0 ? total_height - 1 : 0);
    } else {
        lv_obj_add_flag(lyrics_plain_label, LV_OBJ_FLAG_HIDDEN);

        if (count == 0) {
            lv_obj_remove_flag(lyrics_empty_label, LV_OBJ_FLAG_HIDDEN);
            for (int slot = 0; slot < LYRICS_POOL_SIZE; slot++) lv_obj_add_flag(lyrics_rows[slot], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(lyrics_empty_label, LV_OBJ_FLAG_HIDDEN);
        }

        int32_t total_height = count > 0 ? lyrics_line_y(count) + LYRICS_TOP_PAD : LYRICS_TOP_PAD;
        lv_obj_set_pos(lyrics_spacer, 0, total_height > 0 ? total_height - 1 : 0);
    }

    lv_obj_scroll_to_y(lyrics_list, 0, LV_ANIM_OFF);
    lyrics_window_start = -1;
    lyrics_pool_synced_for_index = current_lyrics_doc_for_index;
    lyrics_auto_follow = true;
    lyrics_last_centered_index = -2;
}
/* Repositions/relabels the row pool to cover the range of lines actually
 * scrolled into (or near) view, and recolors every visible row for the
 * current active_index -- called on every scroll event and every lyrics_
 * timer_cb() tick. Unlike compact_list_update_window(), this can't early-
 * return purely on "the window hasn't moved": the active line's highlight
 * can change (playback advancing) with the view sitting perfectly still. */
static void lyrics_update_window(int active_index) {
    int count = current_lyrics_doc_valid ? current_lyrics_doc.count : 0;
    if (count == 0) return; /* nothing to position -- empty state already shown by lyrics_reset_pool() */

    int32_t scroll_y = lv_obj_get_scroll_y(lyrics_list);
    if (scroll_y < 0) scroll_y = 0;
    int first = lyrics_first_line_at_y(scroll_y, count) - LYRICS_POOL_SIZE / 4;
    if (first < 0) first = 0;
    int max_first = count - LYRICS_POOL_SIZE;
    if (max_first < 0) max_first = 0;
    if (first > max_first) first = max_first;

    bool window_moved = first != lyrics_window_start;
    if (window_moved) lyrics_window_start = first;

    for (int slot = 0; slot < LYRICS_POOL_SIZE; slot++) {
        int index = first + slot;
        lv_obj_t * row = lyrics_rows[slot];
        if (index >= count) {
            lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(row, LV_OBJ_FLAG_HIDDEN);
        if (window_moved) {
            lv_obj_set_y(row, lyrics_line_y(index));
            lv_obj_set_height(row, lyrics_line_height(index));
            lv_label_set_text(row, current_lyrics_doc.lines[index].text);
        }
        lv_obj_set_style_text_color(row, index == active_index ? accent_lv_color() : lv_color_make(150, 150, 150), 0);
    }
}
/* Tap a line to seek playback to it -- pool_slot (this row's fixed slot in
 * lyrics_rows[], passed as the event's user_data, same (void *)(intptr_t)
 * idiom plugin_interval_timer_cb uses elsewhere in this file) plus lyrics_
 * window_start resolves to the actual doc line index, same as compact_
 * list_row_click_cb()'s own index resolution. Re-enables auto-follow (in
 * case it was suspended) and forces an immediate recenter on the next
 * lyrics_timer_cb() tick, so the view snaps straight to the tapped line
 * and keeps following from there rather than leaving the old scroll
 * position sitting stale until audio_get_position_seconds() catches up. */
static void lyrics_row_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int slot = (int) (intptr_t) lv_event_get_user_data(e);
    int index = lyrics_window_start + slot;
    if (!current_lyrics_doc_valid || index < 0 || index >= current_lyrics_doc.count) return;

    audio_seek(current_lyrics_doc.lines[index].time_ms / 1000.0);
    lyrics_auto_follow = true;
    lyrics_last_centered_index = -2;
}
/* The ONLY way out of this screen -- see build_lyrics_screen()'s own header
 * comment for why this isn't the shared screen_gesture_event_cb() (no
 * animation here, and no left-swipe-to-player handling to speak of since
 * poll_quick_drawer_drag() already excludes lyrics_screen from that
 * gesture entirely). Manipulates nav_depth/nav_stack directly, the same
 * bookkeeping nav_pop() itself does, just finishing with a plain
 * lv_screen_load() instead of the animated screen_transition_slide(). */
static void lyrics_gesture_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_GESTURE) return;
    lv_indev_t * indev = lv_indev_active();
    if (!indev || lv_indev_get_gesture_dir(indev) != LV_DIR_RIGHT) return;

    lv_indev_wait_release(indev); /* same reasoning as screen_gesture_event_cb's own comment -- avoid a phantom tap landing on the player screen under the still-down finger */
    lv_timer_pause(lyrics_timer);
    /* Real bug caught in review: the backdrop (~768KB RGB565, LYRICS_
     * BACKDROP_WIDTH x HEIGHT x 2) stayed allocated for the rest of the
     * app's runtime after leaving this screen -- not a leak (poll_lyrics_
     * backdrop() already frees the previous one before replacing it on the
     * NEXT open), but a needless standing hold on a device with ~56MB total
     * RAM. Freed here instead, on the one confirmed way out of this screen
     * (see this function's own header comment). lyrics_backdrop_img is
     * re-hidden so a stale freed pointer in current_lyrics_backdrop_dsc
     * can't get redrawn before the next open's own launch_lyrics_backdrop_
     * decode() lands a fresh one -- open_lyrics_screen() already falls back
     * to a plain dark background for that brief window regardless (see
     * launch_lyrics_backdrop_decode()'s own comment), so this costs nothing
     * new on re-entry beyond that already-accepted, already-documented gap. */
    lv_obj_add_flag(lyrics_backdrop_img, LV_OBJ_FLAG_HIDDEN);
    nav_pop();
}
/* Records a manual scroll only when driven by an actual finger-press (same
 * lv_indev_get_state()==LV_INDEV_STATE_PRESSED precedent used elsewhere in
 * this file), not the programmatic lv_obj_scroll_to_y() call lyrics_timer_
 * cb() itself makes to re-center on the active line -- otherwise every
 * auto-follow re-center would immediately look like a manual scroll and
 * suspend itself. */
static void lyrics_scroll_event_cb(lv_event_t * e) {
    (void) e;
    lv_indev_t * indev = lv_indev_active();
    if (indev && lv_indev_get_state(indev) == LV_INDEV_STATE_PRESSED) {
        lyrics_auto_follow = false;
        clock_gettime(CLOCK_MONOTONIC, &lyrics_last_manual_scroll_at);
    }
    lyrics_update_window(lyrics_current_active_index());
}
static void lyrics_timer_cb(lv_timer_t * timer) {
    (void) timer;
    poll_lyrics_backdrop();

    if (current_lyrics_doc_for_index != lyrics_pool_synced_for_index) {
        lyrics_reset_pool();
        launch_lyrics_backdrop_decode(); /* track changed while this screen is open -- refresh the backdrop too */
    }

    /* current_lyrics_plain_mode -- see its own doc comment: a static text
     * block has no active line to track, no auto-follow-with-playback to
     * suspend/resume, and no per-line pool to reposition. lyrics_reset_
     * pool() above already did everything this screen needs for it. */
    if (current_lyrics_plain_mode) return;

    bool was_auto_follow = lyrics_auto_follow;
    lv_indev_t * indev = lv_indev_active();
    bool pressed = indev && lv_indev_get_state(indev) == LV_INDEV_STATE_PRESSED;
    if (pressed) {
        lyrics_auto_follow = false;
    } else if (!lyrics_auto_follow) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long idle_ms = (now.tv_sec - lyrics_last_manual_scroll_at.tv_sec) * 1000L +
                       (now.tv_nsec - lyrics_last_manual_scroll_at.tv_nsec) / 1000000L;
        if (idle_ms >= LYRICS_AUTO_FOLLOW_RESUME_MS) lyrics_auto_follow = true;
    }
    /* Auto-follow just resumed after being suspended -- force a recenter
     * below even if active_index hasn't changed since the view may have
     * drifted away from it while suspended. */
    if (lyrics_auto_follow && !was_auto_follow) lyrics_last_centered_index = -2;

    int active_index = lyrics_current_active_index();

    if (lyrics_auto_follow && active_index >= 0 && active_index != lyrics_last_centered_index) {
        int32_t target = lyrics_line_y(active_index) - LYRICS_ACTIVE_LINE_ANCHOR_Y;
        if (target < 0) target = 0;
        lv_obj_scroll_to_y(lyrics_list, target, LV_ANIM_ON);
        lyrics_last_centered_index = active_index;
    }

    lyrics_update_window(active_index);
}
static void open_lyrics_screen(void) {
    launch_lyrics_backdrop_decode();
    if (current_lyrics_backdrop_bytes &&
        current_lyrics_backdrop_for_index == gui_player_get_playlist_index() &&
        current_lyrics_backdrop_generation == lyrics_load_generation)
        lv_obj_remove_flag(lyrics_backdrop_img, LV_OBJ_FLAG_HIDDEN);
    lyrics_reset_pool();
    lv_timer_resume(lyrics_timer);
    nav_push(lyrics_screen);
    lyrics_timer_cb(NULL); /* one immediate tick so the view isn't blank for up to LYRICS_TIMER_PERIOD_MS after opening */
}
static lv_obj_t * build_lyrics_screen(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_make(0, 0, 0), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lyrics_backdrop_img = lv_image_create(scr);
    lv_obj_set_size(lyrics_backdrop_img, LYRICS_BACKDROP_WIDTH, LYRICS_BACKDROP_HEIGHT);
    lv_obj_align(lyrics_backdrop_img, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_add_flag(lyrics_backdrop_img, LV_OBJ_FLAG_HIDDEN); /* shown by poll_lyrics_backdrop() once a real backdrop exists */
    lv_obj_remove_flag(lyrics_backdrop_img, LV_OBJ_FLAG_CLICKABLE);

    /* Kept as a transparent stacking/click-through layer for layout
     * compatibility. Darkness is baked into the backdrop before its final
     * dithered RGB565 conversion; alpha-blending black here would quantize
     * it a second time and recreate visible color bands. */
    lv_obj_t * dark_overlay = lv_obj_create(scr);
    lv_obj_set_size(dark_overlay, 480, 800);
    lv_obj_align(dark_overlay, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(dark_overlay, lv_color_make(0, 0, 0), 0);
    lv_obj_set_style_bg_opa(dark_overlay, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(dark_overlay, 0, 0);
    lv_obj_set_style_radius(dark_overlay, 0, 0);
    lv_obj_remove_flag(dark_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(dark_overlay, LV_OBJ_FLAG_CLICKABLE);

    lyrics_empty_label = lv_label_create(scr);
    lv_label_set_text(lyrics_empty_label, "No synchronized lyrics found");
    lv_obj_set_style_text_font(lyrics_empty_label, &app_font_22, 0);
    lv_obj_set_style_text_color(lyrics_empty_label, lv_color_make(200, 200, 200), 0);
    lv_obj_center(lyrics_empty_label);
    lv_obj_add_flag(lyrics_empty_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(lyrics_empty_label, LV_OBJ_FLAG_CLICKABLE);

    /* The scrollable content object itself -- transparent so the backdrop/
     * overlay behind it show through. The pooled row labels below each get
     * their own LV_EVENT_CLICKED handler (lyrics_row_click_cb -- tap a
     * line to seek to it); this object only listens for LV_EVENT_SCROLL,
     * to track manual-scroll-vs-auto-follow. */
    lyrics_list = lv_obj_create(scr);
    lv_obj_set_size(lyrics_list, 480, 800);
    lv_obj_align(lyrics_list, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_opa(lyrics_list, 0, 0);
    lv_obj_set_style_border_width(lyrics_list, 0, 0);
    lv_obj_set_style_radius(lyrics_list, 0, 0);
    lv_obj_set_style_pad_all(lyrics_list, 0, 0);
    lv_obj_set_scroll_dir(lyrics_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(lyrics_list, LV_SCROLLBAR_MODE_OFF);

    for (int slot = 0; slot < LYRICS_POOL_SIZE; slot++) {
        lv_obj_t * row = lv_label_create(lyrics_list);
        lv_obj_set_width(row, LYRICS_ROW_WIDTH);
        lv_obj_set_height(row, lv_font_get_line_height(&app_font_lyrics));
        lv_label_set_long_mode(row, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(row, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(row, &app_font_lyrics, 0); /* own separate size, not app_font_28 -- see fallback_font.h */
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(row, 20, LYRICS_TOP_PAD);
        lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN); /* shown by lyrics_update_window() once it has real content */
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE); /* tap to seek -- lyrics_row_click_cb() */
        lv_obj_add_event_cb(row, lyrics_row_click_cb, LV_EVENT_CLICKED, (void *) (intptr_t) slot);
        lyrics_rows[slot] = row;
    }

    /* current_lyrics_plain_mode's own display -- a single wrapped label
     * showing the whole embedded-tag text as a static block (no per-line
     * highlight, no auto-follow-with-playback), for the common case where
     * an embedded lyrics tag has no [mm:ss.xx] timestamps to drive the
     * synced view above. Plain LVGL scrolling on lyrics_list (already
     * scrollable) is enough here -- unlike the synced rows, there's no
     * windowing/virtualization needed for a single object. */
    lyrics_plain_label = lv_label_create(lyrics_list);
    lv_obj_set_width(lyrics_plain_label, 440);
    lv_label_set_long_mode(lyrics_plain_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(lyrics_plain_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(lyrics_plain_label, &app_font_lyrics, 0);
    lv_obj_set_style_text_color(lyrics_plain_label, lv_color_make(230, 230, 230), 0);
    lv_obj_remove_flag(lyrics_plain_label, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(lyrics_plain_label, LV_OBJ_FLAG_CLICKABLE); /* static text -- no tap-to-seek, there's no timing to seek to */
    lv_obj_set_pos(lyrics_plain_label, 20, LYRICS_TOP_PAD);
    lv_obj_add_flag(lyrics_plain_label, LV_OBJ_FLAG_HIDDEN); /* shown by lyrics_reset_pool() when current_lyrics_plain_mode */

    /* 1x1 invisible spacer at the bottom of the FULL virtual list -- not
     * hidden (a hidden object doesn't count toward scrollable content
     * size), just visually imperceptible. Same technique as compact_list's
     * own spacer (screen_builders.c). */
    lyrics_spacer = lv_obj_create(lyrics_list);
    lv_obj_set_size(lyrics_spacer, 1, 1);
    lv_obj_set_style_bg_opa(lyrics_spacer, 0, 0);
    lv_obj_set_style_border_width(lyrics_spacer, 0, 0);
    lv_obj_remove_flag(lyrics_spacer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(lyrics_spacer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(lyrics_spacer, 0, 0);

    lv_obj_add_event_cb(lyrics_list, lyrics_scroll_event_cb, LV_EVENT_SCROLL, NULL);

    /* Right-swipe-to-exit -- see lyrics_gesture_event_cb()'s own doc
     * comment for why this is a dedicated handler rather than finalize_
     * screen_navigation()'s shared one. Attached to scr (not lyrics_list)
     * and bubbled up the same way finalize_screen_navigation() does for
     * every other screen, via enable_gesture_bubble_recursive() -- a drag
     * starting on lyrics_list (or one of its pooled row labels) still needs
     * to reach this handler once LVGL judges it a horizontal gesture rather
     * than a vertical scroll. */
    lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
    enable_gesture_bubble_recursive(scr);
    lv_obj_add_event_cb(scr, lyrics_gesture_event_cb, LV_EVENT_GESTURE, NULL);

    /* Created paused -- only ticks while this screen is actually open
     * (resumed by open_lyrics_screen(), paused by lyrics_gesture_event_cb()),
     * so it costs nothing the rest of the time. Not the existing 500 ms
     * update_timer_cb: that one must keep running at its own cadence
     * regardless of whether this screen is open, and 150 ms is needed here
     * for a scroll/highlight cadence that actually looks smooth. */
    lyrics_timer = lv_timer_create(lyrics_timer_cb, LYRICS_TIMER_PERIOD_MS, NULL);
    lv_timer_pause(lyrics_timer);

    return scr;
}
static const lyrics_font_size_option_t lyrics_font_size_options[] = {
    { 1, "Medium" }, { 2, "Large" },
};
static lv_obj_t * lyrics_font_size_screen;
static lv_obj_t * lyrics_font_size_list;
static void lyrics_font_size_option_row_cb(lv_event_t * e);
static void populate_lyrics_font_size_screen(void) {
    lv_obj_clean(lyrics_font_size_list);
    for (size_t i = 0; i < LYRICS_FONT_SIZE_OPTION_COUNT; i++) {
        bool selected = current_settings.lyrics_font_size_tier == lyrics_font_size_options[i].tier;
        lv_obj_t * row = add_pill_row_base(lyrics_font_size_list, lyrics_font_size_options[i].label);
        lv_obj_set_style_border_width(row, selected ? 3 : 0, 0);
        lv_obj_set_style_border_color(row, accent_lv_color(), 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, lyrics_font_size_option_row_cb, LV_EVENT_CLICKED, (void *) (intptr_t) i);
    }
}
/* Live-apply, same black-mask-behind-a-one-shot-timer shape as the general
 * Font Size selector (font_size_option_row_cb()/font_size_apply_timer_cb(),
 * gui_network.c) -- no more "Restart Now?" popup/real reboot. Deliberately
 * lighter than that one's own post-apply refresh: app_font_lyrics is
 * consumed exclusively by this screen's own row pool (gui_lyrics_refresh_
 * layout() already resyncs it, including live descender-free row heights,
 * pixel-perfect), so there's no Settings-pill-row geometry or nav-stack
 * sweep to do here, unlike a general Font Size change. */
static void lyrics_font_size_apply_timer_cb(lv_timer_t * timer) {
    lv_obj_t * mask = (lv_obj_t *) lv_timer_get_user_data(timer);
    int target = (int) (intptr_t) lv_obj_get_user_data(mask) - 1;
    lv_timer_delete(timer);
    if (!fallback_font_apply_lyrics_size_tier(target)) {
        lv_obj_delete(mask);
        show_error_toast("Could not apply lyrics text size");
        return;
    }

    gui_navigation_invalidate_font_snapshots();
    current_settings.lyrics_font_size_tier = target;
    settings_save(&current_settings);
    gui_lyrics_refresh_layout();
    populate_lyrics_font_size_screen();

    lv_obj_delete(mask);
}

static void lyrics_font_size_option_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    int target = lyrics_font_size_options[index].tier;
    if (target == current_settings.lyrics_font_size_tier) return;

    lv_obj_t * mask = lv_obj_create(lv_layer_sys());
    lv_obj_set_user_data(mask, (void *) (intptr_t) (target + 1));
    lv_display_t * display = lv_display_get_default();
    lv_obj_set_size(mask,
                    lv_display_get_horizontal_resolution(display),
                    lv_display_get_vertical_resolution(display));
    lv_obj_align(mask, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(mask, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(mask, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(mask, 0, 0);
    lv_obj_set_style_radius(mask, 0, 0);
    lv_obj_set_style_pad_all(mask, 0, 0);
    lv_obj_remove_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_foreground(mask);
    lv_obj_invalidate(mask);

    /* Leave enough time for the normal refresh timer to paint the mask;
     * synchronous lv_refr_now() here would re-enter rendering from an input
     * callback -- see font_size_option_row_cb()'s own matching comment
     * (gui_network.c) for why. */
    lv_timer_create(lyrics_font_size_apply_timer_cb, 35, mask);
}
static lv_obj_t * build_lyrics_font_size_screen(void) {
    lv_obj_t * title_label; /* unused after build -- title never changes */
    return build_subsonic_list_screen("Lyrics Text Size", &title_label, &lyrics_font_size_list);
}
static void open_lyrics_font_size_screen(void) {
    populate_lyrics_font_size_screen();
    nav_push(lyrics_font_size_screen);
}
void lyrics_font_size_settings_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    open_lyrics_font_size_screen();
}


extern void player_transition_mark_dirty(void);
extern bool audio_is_playing(void);
extern const char * asset_path(const char * name);

void gui_lyrics_init(void) {
    lyrics_screen = build_lyrics_screen();
    lyrics_font_size_screen = build_lyrics_font_size_screen();
}

/* For gui_reload.c's in-process UI reload -- deletes both screens this
 * module owns so gui_lyrics_init() can rebuild them from a clean slate
 * without leaking the old objects. */
void gui_lyrics_teardown(void) {
    /* Unguarded lv_timer_create() at this screen's own build site -- same
     * leaked-old-timer hazard as gui_player_teardown()'s volume_popup_hide_
     * timer, see its own comment. */
    if (lyrics_timer) { lv_timer_del(lyrics_timer); lyrics_timer = NULL; }
    if (lyrics_screen) { lv_obj_del(lyrics_screen); lyrics_screen = NULL; }
    if (lyrics_font_size_screen) { lv_obj_del(lyrics_font_size_screen); lyrics_font_size_screen = NULL; }
}

lv_obj_t * gui_lyrics_get_screen(void) {
    return lyrics_screen;
}

lv_obj_t * gui_lyrics_get_font_size_screen(void) {
    return lyrics_font_size_screen;
}

void gui_lyrics_poll_load(void) {
    poll_lyrics_load();
}

void gui_lyrics_poll_backdrop(void) {
    poll_lyrics_backdrop();
}

void gui_lyrics_on_cover_changed(int current_playlist_index) {
    if (current_lyrics_doc_generation == lyrics_load_generation &&
        current_lyrics_doc_for_index == current_playlist_index &&
        (current_lyrics_doc_valid || current_lyrics_plain_mode)) {
        launch_lyrics_backdrop_decode();
    }
}

void gui_lyrics_load_track(int index, const char * path) {
    if (current_lyrics_doc_valid) {
        lyrics_doc_free(&current_lyrics_doc);
        current_lyrics_doc_valid = false;
    }
    if (current_lyrics_plain_mode) {
        free(current_lyrics_plain_text);
        current_lyrics_plain_text = NULL;
        current_lyrics_plain_mode = false;
    }
    current_lyrics_doc_for_index = index;
    if (path) launch_lyrics_load(index, path);
}

void gui_lyrics_open_screen(void) {
    open_lyrics_screen();
}

bool gui_lyrics_has_background_work(void) {
    return lyrics_load_active || lyrics_backdrop_active;
}

void gui_lyrics_cancel_background_work(void) {
    lyrics_load_generation++;
    if (lyrics_load_active) {
        pthread_join(lyrics_load_thread, NULL);
        lyrics_load_active = false;
    }
    if (lyrics_backdrop_active) {
        pthread_join(lyrics_backdrop_thread, NULL);
        lyrics_backdrop_active = false;
    }
}

void gui_lyrics_refresh_layout(void) {
    if (!lyrics_screen) return;
    lyrics_reset_pool();
    if (!lv_obj_has_flag(lyrics_screen, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_invalidate(lyrics_screen);
    }
}
