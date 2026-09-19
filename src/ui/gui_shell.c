#include "gui_shell.h"
#include "app_clock.h"
#include "topbar_icon_layout.h"
#include "gui.h"
#include "gui_theme.h"
#include "gui_notifications.h"
#include "gui_library.h"
#include "gui_queue.h"
#include "gui_player.h"
#include "gui_plugins.h"
#include "gui_settings.h"
#include "gui_network.h"
#include "gui_lyrics.h"
#include "gui_track_info.h"
#include "gui_text_input.h"
#include "gui_navigation.h"
#include "gui_lock_screen.h"
#include "gesture_detector.h"
#include "screen_builders.h"
#include "fallback_font.h"
#include "transition_compositor.h"
#include "metadata.h"
#include "db_log.h"
#include "audio.h"
#include "hw_volume_coalesce.h"
#include "settings.h"
#include "assets.h"
#include "device_config.h"
#include "battery.h"
#include "charge_limiter.h"
#include "wifi_status.h"
#include "wifi_control.h"
#include "bluetooth_control.h"
#include "bluetooth_reconnect.h"
#ifndef HOST_BUILD
#include "bt_media_player.h"
#endif
#include "usb_audio_output.h"
#include "headphone_status.h"
#include "usb_dac_bridge.h"
#include "usb_mode_control.h"
#include "backlight.h"
#include "plugin_manager.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>

#define QUICK_DRAWER_ANIM_MS 200

static lv_obj_t * home_screen = NULL;
static lv_obj_t * dac_home_screen = NULL;
static lv_obj_t * status_bar_band = NULL;
extern player_settings_t current_settings;

static lv_obj_t * clock_topbar_group = NULL;
static lv_obj_t * clock_topbar_digit[5] = { NULL };
static lv_obj_t * clock_topbar_ampm = NULL;
static lv_obj_t * volume_topbar_group = NULL;
static lv_obj_t * volume_topbar_digit[3] = { NULL };
static lv_obj_t * volume_topbar_headphone = NULL;
static int volume_topbar_last_len = -1;
static char volume_topbar_last_digits[4] = "";

static int volume_warn_threshold_percent = -1;
static lv_obj_t * quick_drawer_wifi_icon = NULL;
static lv_obj_t * quick_drawer_bt_icon = NULL;

static lv_obj_t * quick_drawer = NULL;
static lv_obj_t * quick_drawer_brightness_icon = NULL;
static asset_decoded_image_t quick_drawer_bg_image;
static asset_decoded_image_t quick_drawer_brightness_image;
static lv_obj_t * quick_drawer_motion_image = NULL;
static lv_draw_buf_t * quick_drawer_motion_buf = NULL;
static bool quick_drawer_bitmap_motion = false;
static bool quick_drawer_direct_motion = false;
static int32_t quick_drawer_direct_y = 0;
static bool quick_drawer_snapshot_dirty = true;
static bool quick_drawer_open = false;

#define QUICK_DRAWER_TRIGGER_ZONE BOARD_SCALE_PX(140)
#define QUICK_DRAWER_TOGGLE_ICON_PX 84

static void start_bt_dac_startup_reapply_if_needed(void);
static void start_bt_source_codec_reconcile_if_needed(void);
static lv_obj_t * quick_drawer_brightness_track = NULL;
static lv_obj_t * quick_drawer_brightness_label = NULL;
static lv_timer_t * brightness_hw_apply_timer = NULL;
static int brightness_hw_pending = -1;
static bool brightness_drag_active = false;
static bool wifi_toggle_active = false;
/* While a manual toggle or the ordinary screen-off radio restore is in
 * flight, the requested state is the UI source of truth. wifi_on.sh
 * tears down and recreates wpa_supplicant, so its control socket
 * temporarily disappears during a cold enable; this prevents the UI
 * from bouncing on -> off -> on during the transition. */
static bool wifi_toggle_target_enabled = false;
static bool bt_toggle_active = false;
static bool bt_toggle_target_enabled = false;
static bool bt_toggle_followup_pending = false;
static bool bt_toggle_followup_target_enabled = false;

/* True only while the in-flight wifi_toggle_thread was kicked off by
 * gui_shell_suspend_connections() (the automatic idle-screen-off radio
 * power-save cycle) rather than the user's own quick_drawer_wifi_event_cb()
 * tap. Each of the three call sites that can start this thread (that one,
 * gui_shell_suspend_connections(), and gui_shell_resume_connections()) sets
 * this immediately before its own pthread_create(), so it always reflects
 * whichever attempt is actually in flight -- a failed launch never leaves a
 * stale value behind for a later, unrelated toggle to misread, since
 * wifi_toggle_active reverting to false means poll_wifi_toggle() never
 * consumes it for that failed attempt anyway.
 *
 * Consumed once by poll_wifi_toggle() to skip permanent cleanup of
 * DLNA and Remote Control during transient power-save radio suspend,
 * preserving their settings across screen-off sleep cycles. */
static bool wifi_toggle_is_radio_suspend = false;

/* A tap that arrives while a toggle is still in flight. wifi_on.sh/wifi_off.sh
 * take a couple of seconds, which is long enough to tap again, and dropping
 * that tap left the radio in the opposite state to the one the icon was
 * showing. Only the most recent request is kept: tapping twice settles on the
 * second target rather than replaying both, so the radio is never cycled just
 * to satisfy a request the user already changed their mind about. */
static bool wifi_toggle_queued = false;
static bool wifi_toggle_queued_target = false;

/* Read-only effective-Wi-Fi-state accessor for callers outside this file
 * (gui_network.c's Wi-Fi dependency guard for AirPlay/DLNA/Remote Control/
 * Import via Wi-Fi) that need the same "in-flight toggle counts as its
 * target state" logic refresh_wifi_icon() below already uses -- plain
 * wifi_control_is_enabled() can briefly still report the OLD hardware state
 * while wifi_toggle_active is true (see wifi_toggle_thread_func()'s own
 * comment), which would let a tap through for a moment right as Wi-Fi is
 * being turned off. Deliberately exposes only this bool, not wifi_toggle_
 * active/wifi_toggle_target_enabled themselves -- callers have no business
 * reading or driving this file's own toggle machinery directly. */
bool gui_shell_wifi_effective_enabled(void) {
    /* A queued request outranks the one in flight: it is the newer intent, it
     * is already what the icons show, and it is what the radio will be left
     * at. Without it here, any redraw between the tap and the relaunch (a
     * status refresh, a wake) would repaint the older target over it. */
    if (wifi_toggle_queued) return wifi_toggle_queued_target;
    return wifi_toggle_active ? wifi_toggle_target_enabled : wifi_control_is_enabled();
}

static void refresh_quick_drawer_brightness(void) {
    if (!quick_drawer_brightness_track) return;
    int brightness = backlight_get_percent();
    if (brightness < 0) brightness = current_settings.brightness_percent;
    lv_slider_set_value(quick_drawer_brightness_track, brightness, LV_ANIM_OFF);
    if (quick_drawer_brightness_label) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", brightness);
        lv_label_set_text(quick_drawer_brightness_label, buf);
    }
}

extern void player_transition_cache_async_cb(void * user_data);
static lv_obj_t * home_indicator_band = NULL;

static lv_obj_t * quick_drawer_title_label = NULL;
static lv_obj_t * quick_drawer_artist_label = NULL;
static lv_obj_t * quick_drawer_album_label = NULL;
static lv_obj_t * quick_drawer_format_label = NULL;
static lv_obj_t * quick_drawer_favorite_icon = NULL;
static lv_obj_t * quick_drawer_cover_img = NULL;
static lv_obj_t * quick_drawer_cover_frame = NULL;
static lv_obj_t * quick_drawer_play_btn = NULL;
static lv_obj_t * quick_drawer_order_icon = NULL;
static lv_obj_t * quick_drawer_volume_track = NULL;
static lv_obj_t * quick_drawer_volume_label = NULL;
/* Shared with gui_player.c's own volume popup via hw_volume_coalesce.h --
 * see that header's own comment for why this replaced a second, simpler,
 * worse copy of the same debounce (this used to call audio_request_
 * volume() directly and untethered on every drag tick, which is what made
 * this slider feel sluggish). Own instance: only one slider can be
 * mid-drag at a time (single touch), but there is no reason to share the
 * popup's own timer/pending state, only this code. */
static hw_volume_coalesce_t quick_drawer_volume_hv = { .pending = -1 };
/* 4 native row-1 toggles + 4 native row-2 ones (qd_toggle_t, declared further
 * down) + PLUGIN_MAX_QUICK_TOGGLES row-3 plugin tiles. Not expressed as
 * QD_TOGGLE_COUNT + ... because that enum is declared below this point. */
#define QUICK_DRAWER_TOGGLE_SLOTS (8 + PLUGIN_MAX_QUICK_TOGGLES)
static lv_obj_t * quick_drawer_toggle_state[QUICK_DRAWER_TOGGLE_SLOTS];

/* ---- Expanded area ----------------------------------------------------
 * The handle under the first toggle row drags down to reveal two more rows.
 * Both slider rails shift down by exactly the revealed height, and the
 * now-playing card hides for the duration -- there is no room for both.
 *
 * The rows live inside quick_drawer_expansion_box, whose HEIGHT is the
 * animation: LVGL clips children to their parent, so growing the box from
 * zero wipes the rows into view without touching their own geometry. One
 * resize per frame, rather than fading ~18 objects individually -- a
 * subtree opacity would force a composited layer every frame, which this
 * file already avoids for drawer motion (see quick_drawer_begin_bitmap_
 * motion()'s own snapshot reasoning). */
/* A toggle row is ~123px of content (84px icon + name + state caption), so
 * a 148 pitch leaves ~25px between rows where 128 left ~5. The taller panel
 * pays for it: fully expanded, the volume rail now ends at 724 against a
 * panel bottom of 785. */
#define QUICK_DRAWER_TOGGLE_ROW_PITCH BOARD_SCALE_PY(148)
/* 231, not 219: row 1's state caption ends at ~219, so the old value butted
 * row 2 straight against it with no gap at all. */
/* The now-playing card. Its cover frame fills it, so both read these. */
#define QUICK_DRAWER_CARD_W BOARD_SCALE_PX(413)
#define QUICK_DRAWER_CARD_H BOARD_SCALE_PY(354)

/* Row 1 of the quick toggles, and the expanded rows that follow it. */
#define QUICK_DRAWER_ROW1_TOP BOARD_SCALE_PY(71)
#define QUICK_DRAWER_EXPANSION_TOP BOARD_SCALE_PY(211)

static lv_obj_t * quick_drawer_expansion_box = NULL;
static lv_obj_t * quick_drawer_expansion_handle = NULL;
static lv_obj_t * quick_drawer_card = NULL;
static lv_obj_t * quick_drawer_airplay_icon = NULL;
static lv_obj_t * quick_drawer_dlna_icon = NULL;
static lv_obj_t * quick_drawer_gapless_icon = NULL;
static lv_obj_t * quick_drawer_rc_icon = NULL;
static lv_obj_t * quick_drawer_plugin_icon[PLUGIN_MAX_QUICK_TOGGLES];
static int quick_drawer_plugin_toggle_count = 0;
static int32_t quick_drawer_expansion_full = 0; /* height when fully open */
static int32_t quick_drawer_expansion_y = 0;    /* current, 0..full */
static bool quick_drawer_expanded = false;

/* Everything that slides down by the revealed height, captured at build time
 * with its own collapsed y so applying a shift is one lv_obj_set_y() per
 * entry and never re-derives geometry. lv_obj_set_y() is correct for the
 * lv_obj_align()-positioned entries too: align mode is stored separately and
 * stays put, so this just replaces the y offset it aligns by. */
#define QUICK_DRAWER_SHIFT_MAX 12
static struct {
    lv_obj_t * obj;
    int32_t base_y;
} quick_drawer_shift_objs[QUICK_DRAWER_SHIFT_MAX];
static int quick_drawer_shift_count = 0;

static void quick_drawer_register_shift_obj(lv_obj_t * obj, int32_t base_y) {
    if (!obj || quick_drawer_shift_count >= QUICK_DRAWER_SHIFT_MAX) return;
    quick_drawer_shift_objs[quick_drawer_shift_count].obj = obj;
    quick_drawer_shift_objs[quick_drawer_shift_count].base_y = base_y;
    quick_drawer_shift_count++;
}

static void quick_drawer_apply_expansion(int32_t y) {
    if (y < 0) y = 0;
    if (y > quick_drawer_expansion_full) y = quick_drawer_expansion_full;
    quick_drawer_expansion_y = y;
    if (quick_drawer_expansion_box) lv_obj_set_height(quick_drawer_expansion_box, y);
    for (int i = 0; i < quick_drawer_shift_count; i++)
        lv_obj_set_y(quick_drawer_shift_objs[i].obj, quick_drawer_shift_objs[i].base_y + y);
    /* Hidden the moment the expansion is off zero -- even a partly-shifted
     * volume rail already overlaps the card's top edge. */
    if (quick_drawer_card) {
        if (y > 0) lv_obj_add_flag(quick_drawer_card, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(quick_drawer_card, LV_OBJ_FLAG_HIDDEN);
    }
}

static void quick_drawer_expansion_anim_cb(void * var, int32_t v) {
    (void) var;
    quick_drawer_apply_expansion(v);
}

static void quick_drawer_expansion_anim_done_cb(lv_anim_t * a) {
    (void) a;
    quick_drawer_mark_snapshot_dirty();
}

/* Defined below, past the qd_toggle_t table it reads. */
static void refresh_quick_drawer_expansion_toggles(void);

static void quick_drawer_animate_expansion(bool expand) {
    if (!quick_drawer_expansion_box || quick_drawer_expansion_full <= 0) return;
    /* Re-read on the way open rather than when the drawer opens -- see
     * open_quick_drawer()'s own comment on why this must not run there. */
    if (expand) refresh_quick_drawer_expansion_toggles();
    quick_drawer_expanded = expand;
    int32_t target = expand ? quick_drawer_expansion_full : 0;
    lv_anim_delete(quick_drawer_expansion_box, quick_drawer_expansion_anim_cb);
    if (quick_drawer_expansion_y == target) {
        quick_drawer_mark_snapshot_dirty();
        return;
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, quick_drawer_expansion_box);
    lv_anim_set_values(&a, quick_drawer_expansion_y, target);
    lv_anim_set_duration(&a, QUICK_DRAWER_ANIM_MS);
    lv_anim_set_exec_cb(&a, quick_drawer_expansion_anim_cb);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&a, quick_drawer_expansion_anim_done_cb);
    lv_anim_start(&a);
}

/* Back to collapsed with no animation -- the drawer always opens in its
 * unexpanded state, so this runs on close rather than leaving the expansion
 * to reappear mid-slide the next time it is pulled down. */
static void quick_drawer_reset_expansion(void) {
    if (quick_drawer_expansion_box)
        lv_anim_delete(quick_drawer_expansion_box, quick_drawer_expansion_anim_cb);
    quick_drawer_expanded = false;
    quick_drawer_apply_expansion(0);
}

/* Restarts the now-playing marquees from offset 0, with their 2s wait
 * counted from now.
 *
 * lv_label_set_long_mode() deletes the running offset animations and zeroes
 * the offset even when the mode is unchanged, so re-setting it is the
 * restart. That matters because the scroll animation is otherwise created
 * once, when the track's metadata is set -- and the 2s delay rides on that
 * animation's act_time (lv_anim_set_delay() stores it as a negative
 * act_time, which lv_label.c's overwrite_anim_property() only copies while
 * act_time <= 0, i.e. only at creation). Without this the wait elapses
 * while the drawer is still closed and the title is already scrolling by
 * the time it is pulled down. */
static void quick_drawer_restart_marquee(void) {
    if (quick_drawer_title_label)
        lv_label_set_long_mode(quick_drawer_title_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    if (quick_drawer_artist_label)
        lv_label_set_long_mode(quick_drawer_artist_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
}

static void quick_drawer_expansion_handle_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    /* A drag that already moved the expansion consumes its own release in
     * poll_quick_drawer_drag(); only a genuine tap reaches here. */
    quick_drawer_animate_expansion(!quick_drawer_expanded);
}

static void quick_drawer_volume_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    int percent = (int) lv_slider_get_value(lv_event_get_target(e));
    if (code == LV_EVENT_PRESSED) {
        hw_volume_coalesce_drag_begin(&quick_drawer_volume_hv);
        return;
    }
    if (code == LV_EVENT_VALUE_CHANGED) {
        hw_volume_coalesce_drag_update(&quick_drawer_volume_hv, percent);
        if (quick_drawer_volume_label) lv_label_set_text_fmt(quick_drawer_volume_label, "%d", percent);
        refresh_volume_topbar(percent);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        hw_volume_coalesce_drag_end(&quick_drawer_volume_hv, percent);
        /* Authoritative settled value -- sync the Player screen's own
         * volume_slider mirror now, same as the popup's own release
         * handler does, instead of on every drag tick (its previous spot)
         * -- that also fired a cross-file lv_slider_set_value() call on
         * every tick, one of the other costs stacking on top of the
         * un-throttled audio call to make this feel sluggish. */
        gui_player_set_volume_percent(percent);
        current_settings.volume = (float) percent / 100.0f;
        /* Async, not sync, matching this same file's own
         * quick_drawer_brightness_changed_cb() release branch -- a fast
         * release must not block the UI thread on a synchronous file
         * write. */
        settings_save_async(&current_settings);
        quick_drawer_mark_snapshot_dirty();
    }
}

static void quick_drawer_refresh_volume(void) {
    if (!quick_drawer_volume_track || quick_drawer_volume_hv.drag_active) return;
    int percent = (int) gui_player_get_volume_percent();
    lv_slider_set_value(quick_drawer_volume_track, percent, LV_ANIM_OFF);
    if (quick_drawer_volume_label) lv_label_set_text_fmt(quick_drawer_volume_label, "%d", percent);
}

/* `text` overrides the usual "On"/"Off" caption -- a plugin tile can publish
 * its own pair (GainMode's "High"/"Low"). NULL keeps the default. */
static void quick_drawer_set_toggle_state_text(int index, bool enabled, const char * text) {
    if (index < 0 || index >= QUICK_DRAWER_TOGGLE_SLOTS) return;
    if (!quick_drawer_toggle_state[index]) return;
    lv_label_set_text(quick_drawer_toggle_state[index], text ? text : (enabled ? "On" : "Off"));
    lv_obj_set_style_text_color(quick_drawer_toggle_state[index],
                                enabled ? accent_lv_color() : lv_color_hex(0x8d918f), 0);
    quick_drawer_mark_snapshot_dirty();
}

static void quick_drawer_set_toggle_state(int index, bool enabled) {
    quick_drawer_set_toggle_state_text(index, enabled, NULL);
}

static void quick_drawer_fit_cover(void) {
    if (!quick_drawer_cover_img || !quick_drawer_cover_frame) return;
    lv_image_header_t header;
    if (lv_image_decoder_get_info(lv_image_get_src(quick_drawer_cover_img), &header) != LV_RESULT_OK ||
        header.w <= 0 || header.h <= 0) return;
    int32_t frame_w = lv_obj_get_width(quick_drawer_cover_frame);
    int32_t frame_h = lv_obj_get_height(quick_drawer_cover_frame);
    if (frame_w <= 0 || frame_h <= 0) {
        frame_w = QUICK_DRAWER_CARD_W;
        frame_h = QUICK_DRAWER_CARD_H;
    }
    lv_obj_set_size(quick_drawer_cover_img, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_image_set_inner_align(quick_drawer_cover_img, LV_IMAGE_ALIGN_DEFAULT);
    if (header.w >= frame_w && header.h >= frame_h) {
        /* Already covers the frame, so crop rather than rescale: the frosted
         * source is dithered for its own pixel grid and resampling it bands. */
        lv_image_set_scale(quick_drawer_cover_img, LV_SCALE_NONE);
    } else {
        uint32_t sx = ((uint32_t) frame_w * LV_SCALE_NONE + header.w - 1U) / header.w;
        uint32_t sy = ((uint32_t) frame_h * LV_SCALE_NONE + header.h - 1U) / header.h;
        lv_image_set_scale(quick_drawer_cover_img, sx > sy ? sx : sy);
    }
    lv_obj_update_layout(quick_drawer_cover_img);
    lv_obj_align(quick_drawer_cover_img, LV_ALIGN_CENTER, 0, 0);
}

extern lv_obj_t * gui_books_get_screen();
extern lv_obj_t * lyrics_screen;
extern lv_obj_t * radio_screen;
extern lv_obj_t * podcasts_screen;
extern lv_obj_t * gui_settings_get_screen();
extern lv_obj_t * gui_library_get_music_screen();
extern lv_obj_t * file_browser_screen;
extern lv_obj_t * gui_settings_get_eq_screen();
extern lv_obj_t * favorites_screen;
extern lv_obj_t * gui_library_get_playlists_screen();

extern bool favorite_is_set;
extern void nav_push(lv_obj_t * screen);
extern void nav_pop(void);
extern void nav_reset_to_home(void);
extern void finalize_screen_navigation(lv_obj_t * screen);
extern void generic_back_cb(lv_event_t * e);
extern void blend_overlay_onto_base(uint8_t * dst, const uint8_t * src_base, const uint8_t * src_overlay, int width, int height, int overlay_y);
extern void toggle_play_pause(void);
extern void play_track_at(int target);
extern int compute_manual_step_index(int index, int direction);
extern void cycle_play_mode(void);


static lv_obj_t * battery_topbar_group;
static lv_obj_t * battery_topbar_digit[3];
static lv_obj_t * battery_topbar_percent;
static lv_obj_t * battery_icon_frame;
/* The group is initially built with three visible placeholder digits.
 * refresh_battery_topbar() only forces a flex reflow/re-anchor when the
 * real reading crosses a digit-count boundary, rather than adding layout
 * work to its ordinary 500 ms refresh path. */
static int battery_topbar_visible_digit_count = 3;

static lv_obj_t * wifi_icon;
static lv_obj_t * bt_status_icon;
static lv_obj_t * a2dp_status_icon;
static lv_obj_t * usb_audio_status_icon;
static lv_obj_t * play_pause_status_icon;
static lv_obj_t * bt_codec_status_icon;

typedef enum {
    BT_CODEC_TYPE_NONE = 0,
    BT_CODEC_TYPE_SBC,
    BT_CODEC_TYPE_AAC,
    BT_CODEC_TYPE_APTX,
    BT_CODEC_TYPE_APTX_HD,
    BT_CODEC_TYPE_LDAC,
    BT_CODEC_TYPE_UAT,
    BT_CODEC_TYPE_COUNT
} bt_codec_type_t;

/* Normalizes input codec string by stripping non-alphanumeric chars and lowercasing,
 * then maps to a known codec type enum. */
static bt_codec_type_t bt_codec_identify(const char * codec) {
    if (!codec) return BT_CODEC_TYPE_NONE;

    char norm[16];
    int n = 0;
    for (int i = 0; codec[i] != '\0' && n < (int)sizeof(norm) - 1; i++) {
        unsigned char c = (unsigned char)codec[i];
        if (c >= 'A' && c <= 'Z') {
            norm[n++] = (char)(c + ('a' - 'A'));
        } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            norm[n++] = (char)c;
        }
    }
    norm[n] = '\0';

    if (n == 0) return BT_CODEC_TYPE_NONE;

    if (strcmp(norm, "sbc") == 0) return BT_CODEC_TYPE_SBC;
    if (strcmp(norm, "aac") == 0) return BT_CODEC_TYPE_AAC;
    if (strcmp(norm, "aptx") == 0) return BT_CODEC_TYPE_APTX;
    if (strcmp(norm, "aptxhd") == 0) return BT_CODEC_TYPE_APTX_HD;
    if (strcmp(norm, "ldac") == 0) return BT_CODEC_TYPE_LDAC;
    if (strcmp(norm, "uat") == 0) return BT_CODEC_TYPE_UAT;

    return BT_CODEC_TYPE_NONE;
}

/* Returns persistent asset path for a codec type, resolving asset_path()
 * exactly once across the entire process lifetime to prevent unbounded memory leaks. */
static const char * bt_codec_get_asset(bt_codec_type_t type) {
    static const char * assets[BT_CODEC_TYPE_COUNT] = { NULL };
    static bool initialized = false;
    if (!initialized) {
        assets[BT_CODEC_TYPE_SBC]     = asset_path("topbar/sbc.png");
        assets[BT_CODEC_TYPE_AAC]     = asset_path("topbar/aac.png");
        assets[BT_CODEC_TYPE_APTX]    = asset_path("topbar/aptx.png");
        assets[BT_CODEC_TYPE_APTX_HD] = asset_path("topbar/aptx_hd.png");
        assets[BT_CODEC_TYPE_LDAC]    = asset_path("topbar/ldac.png");
        assets[BT_CODEC_TYPE_UAT]     = asset_path("topbar/uat.png");
        initialized = true;
    }
    if (type > BT_CODEC_TYPE_NONE && type < BT_CODEC_TYPE_COUNT) {
        return assets[type];
    }
    return NULL;
}

static void sync_bt_codec_status_icon(void);

void gui_shell_set_status_bar_screen_context(lv_obj_t * screen) {
    if (!status_bar_band) return;

    /* Library/settings screens already provide a stable background behind
     * the persistent status icons.  Player and Lyrics intentionally draw
     * edge-to-edge artwork, which can be nearly white and make those icons
     * disappear, so give only those two screens a neutral translucent
     * backing.  Keeping this on the persistent band (rather than either
     * screen) also lets transition snapshots composite the same treatment. */
    bool over_artwork = screen == gui_player_get_screen() || screen == gui_lyrics_get_screen();
    lv_obj_set_style_bg_color(status_bar_band, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(status_bar_band, over_artwork ? LV_OPA_50 : LV_OPA_TRANSP,
                            LV_PART_MAIN);
}

void sync_player_topbar_visibility(lv_obj_t * screen) {
    /* Settings > Display > "Hide Player/Lyrics Top Bar" -- hides the global
     * status bar while the Player or its fullscreen Lyrics view is active;
     * every other screen keeps its status bar as normal regardless of this
     * setting. player_dismiss_btn (Player's own standalone back arrow) is
     * additionally tied to the same setting, Player-only -- when the
     * status bar is hidden there's no other visible way back short of the
     * swipe/hardware-button gesture, matching the immersive intent; Lyrics
     * has no equivalent standalone back button of its own. Real, live
     * object state here is allowed to reflect "whatever the user last
     * navigated to" -- correctness for the Phase 2 transition CACHE (built
     * while Player is inactive, so this function's own object-flag state
     * can't be trusted for it) is handled independently by
     * build_flattened_transition_frame()'s own temporary-flag-then-restore
     * approach, not by this function. */
    bool hide = (current_settings.hide_player_topbar && (screen == gui_player_get_screen() || screen == gui_lyrics_get_screen())) ||
                screen == gui_lock_screen_get_screen();
    /* The quick drawer wins while it is open. It deliberately keeps the
     * status bar above itself (see open_quick_drawer()), which does nothing
     * when the Player has hidden it outright -- the drawer would slide down
     * over an immersive Player with no clock or battery on it at all. Any
     * path that re-syncs while the drawer is open gets the same answer, so
     * navigating away from a drawer long-press cannot leave it stuck on. */
    if (quick_drawer_open && screen != gui_lock_screen_get_screen()) hide = false;
    if (status_bar_band) {
        gui_shell_set_status_bar_screen_context(screen);
        if (hide) lv_obj_add_flag(status_bar_band, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(status_bar_band, LV_OBJ_FLAG_HIDDEN);
    }
    gui_player_sync_topbar_visibility(screen);

    /* player_transition_rebuild_cache() (see its own doc comment) refuses to
     * run while gui_player_get_screen() is still the active one -- which, since
     * whatever marked the cache dirty (track change, cover art, play/pause,
     * accent color) almost always happens WHILE the user is looking at the
     * Player screen, is exactly the state the cache is usually dirtied in.
     * Its own lv_async_call() only ever fires once, right after being
     * scheduled, so without this it would stay permanently dirty from that
     * point on -- confirmed on-device (every "PERF transition" line showing
     * player_cache=0 cache_dirty=1, never once actually using the cache).
     * This function already runs as the last step of every real navigation
     * (nav_push/nav_pop/screen_transition_slide's cut fallback/
     * slide_transition_done_cb's commit/nav_reset_to_home), i.e. exactly
     * "after the Player has settled" -- so retrying here, once per actual
     * screen change away from Player, is the natural moment. */
    if (screen != gui_player_get_screen() && player_transition_cache_is_dirty())
        lv_async_call(player_transition_cache_async_cb, NULL);
}

static void style_topbar_text(lv_obj_t * label, const lv_font_t * font) {
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_opa(label, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(label, 0, 0);
}

/* Lucide status assets share a real 24x24 canvas. Render their visible glyph
 * at 28px while retaining the existing 32px layout slot: this gives the
 * status bar more visual weight without consuming any additional horizontal
 * space in the crowded 480px layout. Keep this geometry independent of the
 * accessibility font tier. */
static void layout_lucide_topbar_image(lv_obj_t * image) {
    lv_image_set_inner_align(image, LV_IMAGE_ALIGN_CENTER);
    lv_image_set_scale(image, (BOARD_SCALE_PX(28) * LV_SCALE_NONE + 12) / 24);
    lv_obj_set_size(image, BOARD_SCALE_PX(32), STATUS_BAR_CLEARANCE);
    lv_obj_set_style_translate_y(image, 0, 0);
    lv_image_set_offset_x(image, 0);
    lv_image_set_offset_y(image, 0);
}

/* Battery specifically renders larger than every other Lucide status icon --
 * real-device feedback found the shared 28px size too small to read charge
 * level at a glance. Widens the slot by the same amount the glyph grows
 * (38px glyph in a 42px slot, same ~4px padding the shared 28-in-32 sizing
 * already used) rather than just overscaling within the old 32px slot, so
 * the glyph doesn't visually crowd its own slot edges. battery_icon_frame
 * stays anchored via LV_ALIGN_RIGHT_MID (unaffected by width), so this
 * only grows the icon leftward, away from the screen edge -- and since
 * battery_topbar_group's own position is align_to()'d against this object
 * AFTER this runs (build_status_bar()) and this function always produces
 * the same fixed size on every later refresh_battery_topbar() call too,
 * that one-time anchor never goes stale. */
static void layout_battery_topbar_image(lv_obj_t * image) {
    layout_lucide_topbar_image(image);
    lv_image_set_scale(image, (BOARD_SCALE_PX(38) * LV_SCALE_NONE + 12) / 24);
    lv_obj_set_size(image, BOARD_SCALE_PX(42), STATUS_BAR_CLEARANCE);
}

/* Theme2 status sprites share a 30 px canvas, but their visible alpha bounds
 * vary substantially. Measure the solid glyph (alpha >= 128), not faint
 * antialiasing fringes: these made Wi-Fi/Bluetooth appear undersized.
 * Scale the measured glyph to 22px while keeping its slot on the common
 * status-band centerline, independent of transparent canvas padding. */
static void normalize_topbar_image(lv_obj_t * image, int left, int top,
                                   int width, int height) {
    topbar_icon_layout(image, left, top, width, height, STATUS_BAR_CLEARANCE);
}

static void build_status_bar(void) {
    lv_obj_t * bar = lv_layer_top();

    /* Every plain lv_obj_create() gets LV_OBJ_FLAG_SCROLLABLE by default
     * (confirmed in lv_obj.c's base constructor), including layer_top
     * itself -- nothing ever removes it since we only ever add children to
     * this layer, never scroll it. Left alone, lv_indev_find_scroll_obj()
     * walks the pressed object's FULL ancestor chain (see lv_indev_scroll.c)
     * looking for a scrollable object with overflow, and can end up
     * "claiming" a touch as a scroll of layer_top instead of delivering it
     * as a normal press/click/gesture to whatever real widget was actually
     * touched (this is exactly what silently broke the quick-drawer's
     * swipe-up-to-close gesture, and is a very plausible cause of the
     * drawer's on-screen buttons appearing unresponsive on the real
     * touchscreen -- a real finger tap always has a few px of jitter, unlike
     * a synthetic zero-movement click, and that's enough to trigger this
     * scroll-vs-click arbitration). */
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    /* A dedicated band sized exactly to STATUS_BAR_CLEARANCE, with every
     * status bar element vertically MID-aligned within it, rather than
     * each element aligned to the full-screen top layer with a small
     * hand-tuned Y offset -- the old per-element offsets (1, -3) put
     * everything within a few px of the true screen top regardless of how
     * tall STATUS_BAR_CLEARANCE actually was, so shrinking the clearance
     * left all the real content hugging y=0 with dead space below it
     * instead of using the newly smaller band evenly. */
    lv_obj_t * band = lv_obj_create(bar);
    status_bar_band = band;
    lv_obj_remove_style_all(band);
    lv_obj_set_size(band, lv_pct(100), STATUS_BAR_CLEARANCE);
    lv_obj_set_pos(band, 0, 0);
    lv_obj_remove_flag(band, LV_OBJ_FLAG_SCROLLABLE);

    /* Centered clock -- use a fixed 24px face so the clock,
     * volume and battery readouts share one natural, accessibility-independent
     * text size. */
    clock_topbar_group = lv_obj_create(band);
    lv_obj_remove_style_all(clock_topbar_group);
    lv_obj_set_size(clock_topbar_group, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(clock_topbar_group, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(clock_topbar_group, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(clock_topbar_group, 0, 0);
    lv_obj_remove_flag(clock_topbar_group, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < 5; i++) {
        clock_topbar_digit[i] = lv_label_create(clock_topbar_group);
        lv_label_set_text(clock_topbar_digit[i], i == 2 ? ":" : "0");
        style_topbar_text(clock_topbar_digit[i], &lv_font_montserrat_24);
        /* Optical correction for the digits, without moving the volume anchor. */
        lv_obj_set_style_translate_y(clock_topbar_digit[i], BOARD_SCALE_PX(2), 0);
    }
    clock_topbar_ampm = lv_label_create(clock_topbar_group);
    lv_label_set_text(clock_topbar_ampm, "AM");
    style_topbar_text(clock_topbar_ampm, &lv_font_montserrat_16);
    lv_obj_set_style_translate_y(clock_topbar_ampm, BOARD_SCALE_PX(2), 0);
    lv_obj_add_flag(clock_topbar_ampm, LV_OBJ_FLAG_HIDDEN); /* refresh_clock_label() unhides this if clock_24h is off */

    /* LAST, after every child exists -- see the matching comment on
     * volume_topbar_group's own align() call below for why (LV_SIZE_CONTENT
     * doesn't retroactively re-run an earlier alignment as children grow
     * it). refresh_clock_label() (called right after build_status_bar() in
     * gui_init) immediately overwrites these placeholder "0"/":" sprites
     * with the real time, so there's no visible flash of "00:00". */
    lv_obj_align(clock_topbar_group, LV_ALIGN_CENTER, 0, 0);

    /* Icon row: speaker icon, red volume number, headphone-out icon, etc.
     * Anchored at the left margin independently of the centered clock.
     * A flex row lets hidden digit slots (see
     * refresh_volume_topbar()) collapse cleanly instead of leaving a gap. */
    volume_topbar_group = lv_obj_create(band);
    lv_obj_remove_style_all(volume_topbar_group);
    /* Keep the row's vertical center stable as status icons appear.  Some
     * normalized image canvases are taller than the initially-visible 30 px
     * speaker (for example po.png becomes 38 px).  With content height,
     * unhiding one grows this already-aligned group downward because
     * lv_obj_align_to() does not continuously re-center it. */
    lv_obj_set_size(volume_topbar_group, LV_SIZE_CONTENT, STATUS_BAR_CLEARANCE);
    lv_obj_set_flex_flow(volume_topbar_group, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(volume_topbar_group, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    /* No extra column padding -- each digit sprite already has ~1px of
     * transparent margin baked into its canvas on both edges (e.g.
     * topbar/9.png is a 14px-wide canvas with the glyph spanning x=1..13),
     * providing sufficient separation. */
    lv_obj_set_style_pad_column(volume_topbar_group, 0, 0);
    lv_obj_remove_flag(volume_topbar_group, LV_OBJ_FLAG_SCROLLABLE);

    /* Normalize against the sprite's measured visible glyph bounds. */
    lv_obj_t * volume_topbar_icon = lv_image_create(volume_topbar_group);
    lv_image_set_src(volume_topbar_icon, asset_path("topbar/lucide_volume_2.png"));
    layout_lucide_topbar_image(volume_topbar_icon);

    /* White by default (the sprite's own native color, no recolor style
     * applied at creation); refresh_volume_topbar() below switches each
     * digit to a flat (255,0,0) recolor once the level reaches
     * volume_warn_threshold_percent, matching the stock player's own
     * config-driven behavior (see device_config.h) instead of the flat
     * always-red guess from the previous round. */
    for (int i = 0; i < 3; i++) {
        volume_topbar_digit[i] = lv_label_create(volume_topbar_group);
        lv_label_set_text(volume_topbar_digit[i], "0");
        style_topbar_text(volume_topbar_digit[i], &lv_font_montserrat_24);
    }

    /* Headphone-out glyph (topbar/po.png) -- starts hidden and is shown
     * by refresh_headphone_icon() when a headphone/dongle is plugged in
     * (see headphone_status.h). */
    volume_topbar_headphone = lv_image_create(volume_topbar_group);
    lv_image_set_src(volume_topbar_headphone, asset_path("topbar/lucide_headphones.png"));
    layout_lucide_topbar_image(volume_topbar_headphone);
    lv_obj_add_flag(volume_topbar_headphone, LV_OBJ_FLAG_HIDDEN);

    /* Same flex row as the headphone-jack glyph above, not a separate fixed
     * position -- shown/hidden independently by its own real A2DP state
     * (poll_refresh_bt_icon()), so it naturally sits right next to the jack
     * glyph when both a wired and a Bluetooth output are connected at once,
     * or takes the jack glyph's spot on its own when only Bluetooth is (the
     * flex row's own hidden-children-collapse behavior, already relied on
     * by the volume digit slots above, does this for free -- no manual
     * "replace" logic needed). */
    a2dp_status_icon = lv_image_create(volume_topbar_group);
    lv_image_set_src(a2dp_status_icon, asset_path("topbar/lucide_audio_lines.png"));
    layout_lucide_topbar_image(a2dp_status_icon);
    lv_obj_add_flag(a2dp_status_icon, LV_OBJ_FLAG_HIDDEN); /* shown by poll_refresh_bt_icon() once an A2DP source PCM exists */

    /* Same flex-collapse shape as the headphone/A2DP glyphs above -- shown/
     * hidden by poll_usb_audio_output() once an external USB audio device
     * (DAC/amp) is detected, entirely automatically, no Settings toggle
     * anywhere (unlike Storage/USB DAC/ADB in the manual USB Mode screen --
     * this is meant to feel like the wired headphone jack, not a mode you
     * switch into). */
    usb_audio_status_icon = lv_image_create(volume_topbar_group);
    /* Uses topbar/usb.png for the topbar status row (distinct from
     * usb/usb.png used by the full-screen USB DAC mode overlay). */
    lv_image_set_src(usb_audio_status_icon, asset_path("topbar/lucide_usb.png"));
    layout_lucide_topbar_image(usb_audio_status_icon);
    lv_obj_add_flag(usb_audio_status_icon, LV_OBJ_FLAG_HIDDEN);

    /* Rightmost in this row -- always after whichever headphone-output
     * glyph(s) above are currently shown, per the same flex-collapse
     * reasoning. play.png while actually playing, pause.png while paused,
     * hidden entirely when stopped/nothing loaded --
     * refresh_play_pause_topbar(). */
    play_pause_status_icon = lv_image_create(volume_topbar_group);
    lv_image_set_src(play_pause_status_icon, asset_path("topbar/lucide_play.png"));
    layout_lucide_topbar_image(play_pause_status_icon);
    lv_obj_add_flag(play_pause_status_icon, LV_OBJ_FLAG_HIDDEN);

    /* Negotiated Bluetooth codec indicator (e.g. sbc.png, aac.png, aptx.png,
     * aptx_hd.png, ldac.png, uat.png) -- rightmost in volume_topbar_group,
     * lowest priority on the left side. Shows when an A2DP source PCM is
     * connected and fits without colliding with clock_topbar_group.
     * Hidden by default. */
    bt_codec_status_icon = lv_image_create(volume_topbar_group);
    lv_image_set_src(bt_codec_status_icon, bt_codec_get_asset(BT_CODEC_TYPE_SBC));
    normalize_topbar_image(bt_codec_status_icon, 0, 0, 48, 30);
    lv_obj_add_flag(bt_codec_status_icon, LV_OBJ_FLAG_HIDDEN);

    lv_obj_align(volume_topbar_group, LV_ALIGN_LEFT_MID, BOARD_SCALE_PX(16), 0);

    /* One Lucide image owns the complete battery silhouette. Swapping whole
     * states avoids the old independently-rounded frame/fill/terminal seams. */
    battery_icon_frame = lv_image_create(band);
    lv_image_set_src(battery_icon_frame, asset_path("topbar/lucide_battery.png"));
    layout_battery_topbar_image(battery_icon_frame);
    lv_obj_align(battery_icon_frame, LV_ALIGN_RIGHT_MID, -BOARD_SCALE_PX(15), 0);

    /* Sprite digits (topbar/N.png + percent.png), same treatment as the
     * clock/volume readouts above -- up to 3 digit slots (0-100, same
     * leading-slot-hiding scheme as volume_topbar_digit) plus a trailing
     * percent sign. The whole group is hidden outright when the real
     * percent is unknown (battery_get_percent() < 0, e.g. host with no
     * /sys/class/power_supply) -- same "icon only, no fake reading" honesty
     * the old blank-text label had. */
    battery_topbar_group = lv_obj_create(band);
    lv_obj_remove_style_all(battery_topbar_group);
    lv_obj_set_size(battery_topbar_group, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(battery_topbar_group, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(battery_topbar_group, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(battery_topbar_group, 0, 0);
    lv_obj_remove_flag(battery_topbar_group, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < 3; i++) {
        battery_topbar_digit[i] = lv_label_create(battery_topbar_group);
        lv_label_set_text(battery_topbar_digit[i], "0");
        style_topbar_text(battery_topbar_digit[i], &lv_font_montserrat_24);
    }
    battery_topbar_percent = lv_label_create(battery_topbar_group);
    lv_label_set_text(battery_topbar_percent, "%");
    style_topbar_text(battery_topbar_percent, &lv_font_montserrat_24);

    /* Anchored to battery_icon itself (not a hand-tuned x) rather than a
     * fixed band offset, since the group's own width varies with the
     * digit count (1-3) -- LAST, after every child exists, same reasoning
     * as volume_topbar_group's align() below. */
    lv_obj_align_to(battery_topbar_group, battery_icon_frame, LV_ALIGN_OUT_LEFT_MID, -BOARD_SCALE_PX(5), 0);

    wifi_icon = lv_image_create(band);
    lv_image_set_src(wifi_icon, asset_path("topbar/lucide_wifi_off.png"));
    layout_lucide_topbar_image(wifi_icon);
    lv_obj_align(wifi_icon, LV_ALIGN_RIGHT_MID, -BOARD_SCALE_PX(105), 0);
    lv_obj_add_flag(wifi_icon, LV_OBJ_FLAG_HIDDEN); /* shown by refresh_wifi_icon() once wifi_control_is_enabled() */

    bt_status_icon = lv_image_create(band);
    lv_image_set_src(bt_status_icon, asset_path("topbar/lucide_bluetooth.png"));
    layout_lucide_topbar_image(bt_status_icon);
    lv_obj_align(bt_status_icon, LV_ALIGN_RIGHT_MID, -BOARD_SCALE_PX(145), 0);
    lv_obj_add_flag(bt_status_icon, LV_OBJ_FLAG_HIDDEN); /* shown by refresh_bt_icon() once bt_control_is_powered() */
}

static void refresh_play_pause_topbar(void) {
    bool playing = audio_is_playing();
    bool paused = !playing && audio_is_paused();
    bool was_hidden = lv_obj_has_flag(play_pause_status_icon, LV_OBJ_FLAG_HIDDEN);
    bool should_hide = !playing && !paused;

    if (playing) {
        lv_obj_remove_flag(play_pause_status_icon, LV_OBJ_FLAG_HIDDEN);
        lv_image_set_src(play_pause_status_icon, asset_path("topbar/lucide_play.png"));
        layout_lucide_topbar_image(play_pause_status_icon);
    } else if (paused) {
        lv_obj_remove_flag(play_pause_status_icon, LV_OBJ_FLAG_HIDDEN);
        lv_image_set_src(play_pause_status_icon, asset_path("topbar/lucide_pause.png"));
        layout_lucide_topbar_image(play_pause_status_icon);
    } else {
        lv_obj_add_flag(play_pause_status_icon, LV_OBJ_FLAG_HIDDEN);
    }

    if (was_hidden != should_hide) {
        sync_bt_codec_status_icon();
    }
}

/* Defined further down (near wifi_icon/bt_status_icon's own setup) --
 * forward-declared here so refresh_battery_topbar() below can re-run it
 * whenever battery_topbar_group's own visibility might have changed
 * (unknown percent, or Settings > Power > "Battery Percentage" toggling),
 * since that group is one of the two anchors that logic positions the
 * wifi/bt topbar icons against. */
static void sync_topbar_status_icon_positions(void);

void refresh_battery_topbar(void) {
    int percent = battery_get_display_percent();

    /* battery_icon_frame (the outline + fill gauge) is always shown --
     * current_settings.show_battery_percent (Settings > Power > "Battery
     * Percentage") only ever hides the "NN%" digit readout below, never the
     * icon itself. Edge-triggered (compares against the group's own current
     * hidden-flag rather than setting it unconditionally every call) since
     * this whole function runs every tick the screen is on -- re-syncing
     * the wifi/bt icon positions that often, on every tick, for a flag that
     * only ever changes on a battery-unplugged/replugged edge or a Settings
     * toggle, would be pure churn. */
    bool percent_should_show = percent >= 0 && current_settings.show_battery_percent;
    bool percent_was_shown = !lv_obj_has_flag(battery_topbar_group, LV_OBJ_FLAG_HIDDEN);
    if (percent_should_show != percent_was_shown) {
        if (percent_should_show) lv_obj_remove_flag(battery_topbar_group, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(battery_topbar_group, LV_OBJ_FLAG_HIDDEN);
        sync_topbar_status_icon_positions();
    }

    if (percent < 0) {
        lv_image_set_src(battery_icon_frame, asset_path("topbar/lucide_battery.png"));
        lv_obj_set_style_image_recolor_opa(battery_icon_frame, LV_OPA_TRANSP, 0);
        return;
    }
    if (percent > 100) percent = 100;

    /* charge_limiter_is_holding() (voltage cap applied) is true for the
     * entire time the limiter is on and charging is active, independent of
     * percentage -- not a useful "suppress the bolt" signal by itself.
     * charge_limiter_is_confirmed_off() only goes true once charging has
     * actually tapered off/completed under the cap (same physically-
     * verified signal led_control.c uses for the charge-complete LED), so
     * the bolt stays lit for as long as current is genuinely still
     * flowing. */
    bool limiter_capped_now = charge_limiter_is_confirmed_off();
    bool charging = !limiter_capped_now && battery_is_charging();
    bool low = !charging && percent < 5;

    const char * battery_asset = charging ? "topbar/lucide_battery_charging.png" :
                                 percent >= 85 ? "topbar/lucide_battery_full.png" :
                                 percent >= 40 ? "topbar/lucide_battery_medium.png" :
                                 percent >= 10 ? "topbar/lucide_battery_low.png" :
                                                 "topbar/lucide_battery.png";
    lv_image_set_src(battery_icon_frame, asset_path(battery_asset));
    layout_battery_topbar_image(battery_icon_frame);
    if (low) {
        lv_obj_set_style_image_recolor(battery_icon_frame, lv_color_make(255, 64, 64), 0);
        lv_obj_set_style_image_recolor_opa(battery_icon_frame, LV_OPA_COVER, 0);
    } else {
        lv_obj_set_style_image_recolor_opa(battery_icon_frame, LV_OPA_TRANSP, 0);
    }

    /* Same leading-slot-hiding scheme as refresh_volume_topbar(). */
    char digits[4];
    snprintf(digits, sizeof(digits), "%d", percent);
    int len = (int) strlen(digits);

    for (int i = 0; i < 3; i++) {
        int digit_index = i - (3 - len);
        if (digit_index < 0) {
            lv_obj_add_flag(battery_topbar_digit[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            char digit[2] = { digits[digit_index], '\0' };
            lv_label_set_text(battery_topbar_digit[i], digit);
            lv_obj_remove_flag(battery_topbar_digit[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* battery_topbar_group is right-anchored beside the battery frame, but
     * LV_SIZE_CONTENT changing after leading digit sprites are hidden does
     * not replay that earlier alignment automatically.  Without this
     * edge-triggered re-anchor, a two-digit reading retained one invisible
     * 14px slot's worth of gap (and a one-digit reading retained two).
     * Force layout only at 9<->10 / 99<->100 and on the first non-3-digit
     * reading, then move Wi-Fi/Bluetooth with their corrected anchor. */
    if (len != battery_topbar_visible_digit_count) {
        battery_topbar_visible_digit_count = len;
        lv_obj_update_layout(battery_topbar_group);
        lv_obj_align_to(battery_topbar_group, battery_icon_frame, LV_ALIGN_OUT_LEFT_MID, -BOARD_SCALE_PX(5), 0);
        sync_topbar_status_icon_positions();
    }
}

/* Called at startup and whenever the displayed volume changes. Skip
 * identical strings so LVGL does not re-decode unchanged digit images. */
void refresh_volume_topbar(int32_t percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    if (quick_drawer_volume_track && !quick_drawer_volume_hv.drag_active) {
        lv_slider_set_value(quick_drawer_volume_track, percent, LV_ANIM_OFF);
        if (quick_drawer_volume_label) lv_label_set_text_fmt(quick_drawer_volume_label, "%d", percent);
    }

    char digits[4];
    snprintf(digits, sizeof(digits), "%d", (int) percent);
    int len = (int) strlen(digits);

    /* volume_warn_threshold_percent is -1 when the feature is off (see its
     * declaration) -- guard it explicitly rather than just comparing
     * percent >= threshold, since percent >= -1 is always true. */
    bool warn = volume_warn_threshold_percent >= 0 && percent >= volume_warn_threshold_percent;
    bool digits_changed = strcmp(volume_topbar_last_digits, digits) != 0;

    for (int i = 0; i < 3; i++) {
        int digit_index = i - (3 - len);
        if (digit_index < 0) {
            lv_obj_add_flag(volume_topbar_digit[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            if (digits_changed) {
                char digit[2] = { digits[digit_index], '\0' };
                lv_label_set_text(volume_topbar_digit[i], digit);
            }
            lv_obj_remove_flag(volume_topbar_digit[i], LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_set_style_text_color(volume_topbar_digit[i],
                                    warn ? lv_color_make(255, 0, 0) : lv_color_white(), 0);
    }
    if (digits_changed)
        snprintf(volume_topbar_last_digits, sizeof(volume_topbar_last_digits), "%s", digits);
    if (volume_topbar_last_len != len) {
        volume_topbar_last_len = len;
        sync_bt_codec_status_icon();
    }
}

/* Polled every timer tick alongside refresh_battery_topbar() -- like
 * battery.c's sysfs read, this is a single cheap fopen/fgets with no
 * subprocess fork, so it doesn't need wifi/bt's throttled polling. */
void refresh_headphone_icon(void) {
    bool connected = get_headphone_state() != HEADPHONE_STATE_NONE;
    bool was_hidden = lv_obj_has_flag(volume_topbar_headphone, LV_OBJ_FLAG_HIDDEN);
    if (connected) {
        lv_obj_remove_flag(volume_topbar_headphone, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(volume_topbar_headphone, LV_OBJ_FLAG_HIDDEN);
    }
    if (was_hidden == connected) {
        sync_bt_codec_status_icon();
    }
}

/* wpa_cli forks a process per call (see wifi_status.c), so this is only
 * polled every WIFI_POLL_TICKS timer ticks rather than every tick like the
 * clock/battery -- wifi signal doesn't change fast enough to need
 * sub-second polling anyway. */
#define WIFI_POLL_TICKS 10

/* Derives topbar icon order (wifi_icon and bt_status_icon) relative to
 * battery_topbar_group/battery_icon_frame. Tracks which icon occupies the
 * inner slot (closer to battery) versus the outer slot, avoiding gaps
 * when one of the radios is disabled. Whichever icon was already visible
 * keeps the inner slot; newly-visible icons take any remaining free slot.
 * The two slots are positioned relative to
 * battery_topbar_group/battery_icon_frame. */
typedef enum {
    TOPBAR_STATUS_ICON_NONE = 0,
    TOPBAR_STATUS_ICON_WIFI,
    TOPBAR_STATUS_ICON_BT,
} topbar_status_icon_t;

static topbar_status_icon_t topbar_status_icon_order[2] = { TOPBAR_STATUS_ICON_NONE, TOPBAR_STATUS_ICON_NONE };

static void sync_topbar_status_icon_positions(void) {
    bool wifi_visible = !lv_obj_has_flag(wifi_icon, LV_OBJ_FLAG_HIDDEN);
    bool bt_visible = !lv_obj_has_flag(bt_status_icon, LV_OBJ_FLAG_HIDDEN);

    topbar_status_icon_t new_order[2] = { TOPBAR_STATUS_ICON_NONE, TOPBAR_STATUS_ICON_NONE };
    int slot = 0;
    /* Existing occupants first, in their current order, so an icon that's
     * still visible never moves slots just because the other one's
     * visibility also happened to change on this same call. */
    for (int i = 0; i < 2 && slot < 2; i++) {
        topbar_status_icon_t icon = topbar_status_icon_order[i];
        if ((icon == TOPBAR_STATUS_ICON_WIFI && wifi_visible) || (icon == TOPBAR_STATUS_ICON_BT && bt_visible)) {
            new_order[slot++] = icon;
        }
    }
    /* Then any newly-visible icon not already placed above, oldest-checked
     * (wifi) first -- only matters when both go from hidden to visible on
     * the exact same call, an arbitrary but stable tiebreak. */
    if (wifi_visible && new_order[0] != TOPBAR_STATUS_ICON_WIFI && new_order[1] != TOPBAR_STATUS_ICON_WIFI && slot < 2) {
        new_order[slot++] = TOPBAR_STATUS_ICON_WIFI;
    }
    if (bt_visible && new_order[0] != TOPBAR_STATUS_ICON_BT && new_order[1] != TOPBAR_STATUS_ICON_BT && slot < 2) {
        new_order[slot++] = TOPBAR_STATUS_ICON_BT;
    }
    topbar_status_icon_order[0] = new_order[0];
    topbar_status_icon_order[1] = new_order[1];

    /* Chained anchoring, not fixed offsets -- Settings > Power > "Battery
     * Percentage" (current_settings.show_battery_percent) lets the "NN%"
     * readout be turned off entirely (battery_topbar_group hidden by
     * refresh_battery_topbar() in that case, battery_icon_frame itself
     * always stays visible -- see its own comment). When the percentage is
     * showing, the inner slot sits left of battery_topbar_group, same gap
     * that group's own anchor to battery_icon_frame already uses; when it's
     * off, the inner slot moves in to sit left of battery_icon_frame
     * directly, closing the gap the percentage would otherwise have left. */
    lv_obj_t * anchor = (current_settings.show_battery_percent && !lv_obj_has_flag(battery_topbar_group, LV_OBJ_FLAG_HIDDEN))
                             ? battery_topbar_group
                             : battery_icon_frame;
    for (int i = 0; i < 2; i++) {
        lv_obj_t * widget = topbar_status_icon_order[i] == TOPBAR_STATUS_ICON_WIFI  ? wifi_icon
                            : topbar_status_icon_order[i] == TOPBAR_STATUS_ICON_BT ? bt_status_icon
                                                                                    : NULL;
        if (!widget) continue;
        /* Slots already include 4px on each side of the visible glyph. */
        lv_obj_align_to(widget, anchor, LV_ALIGN_OUT_LEFT_MID, i == 0 ? -BOARD_SCALE_PX(4) : 0, 0);
        anchor = widget;
    }
}

/* The drawer's wifi icon reflects radio on/off (highlighted when enabled),
 * while the top bar icon indicates connection status and signal strength. */
static pthread_t wifi_status_thread;
static bool wifi_status_active;
static atomic_bool wifi_status_done;
static bool wifi_status_result_connected;
static int wifi_status_result_level;
static bool wifi_status_connected;
static int wifi_status_level;
static unsigned wifi_status_generation;
static unsigned wifi_status_job_generation;
static bool wifi_status_enabled;

static void * wifi_status_thread_func(void * arg) {
    (void)arg;
    int level = 0;
    wifi_status_result_connected = wifi_get_status(&level);
    wifi_status_result_level = level;
    atomic_store_explicit(&wifi_status_done, true, memory_order_release);
    return NULL;
}

/* UI-thread only: stale replies from a previous radio state are discarded. */
static void wifi_status_observe_enabled(bool enabled) {
    if (enabled == wifi_status_enabled) return;
    wifi_status_enabled = enabled;
    ++wifi_status_generation;
    wifi_status_connected = false;
    wifi_status_level = 0;
}

/* The drawer's four "on" toggle icons are a filled circle in the stock
 * #009FF6 with a near-white glyph on top. LVGL image_recolor mixes every
 * pixel toward the recolor colour, so gui_theme_accent_style() would tint
 * the glyph as well -- the same COVER-flattening problem gui_player.c
 * documents for btn_play.png/btn_pause.png. Rewrite the circle in decoded
 * pixels instead, exactly as recolor_play_btn_accent() does there: keep the
 * glyph (and every anti-aliased step toward it) by mixing the accent toward
 * white in proportion to how white the source pixel already was. A pixel's
 * minimum channel is that measure -- 0 across the saturated stock blue, 223+
 * inside the near-white glyph -- so the circle lands on the exact accent and
 * the glyph stays the glyph. */
typedef enum {
    QD_TOGGLE_WIFI = 0,
    QD_TOGGLE_BT,
    QD_TOGGLE_SLEEP,
    QD_TOGGLE_CROSSFADE,
    /* Row 2, revealed by the expansion handle -- see build_quick_drawer()'s
     * own comment on the expanded area. */
    QD_TOGGLE_AIRPLAY,
    QD_TOGGLE_DLNA,
    QD_TOGGLE_GAPLESS,
    QD_TOGGLE_RC,
    QD_TOGGLE_COUNT
} qd_toggle_t;

static const char * const qd_toggle_on_asset[QD_TOGGLE_COUNT] = {
    "pull_down/wifi_s.png",
    "pull_down/bt_s.png",
    "pull_down/sleep_switch_s.png",
    "pull_down/fade_s.png",
    "pull_down/airplay_s.png",
    "pull_down/dlna_s.png",
    "pull_down/gapless_play_s.png",
    "pull_down/hibylink_s.png",
};
static const char * const qd_toggle_off_asset[QD_TOGGLE_COUNT] = {
    "pull_down/wifi.png",
    "pull_down/bt.png",
    "pull_down/sleep_switch.png",
    "pull_down/fade.png",
    "pull_down/airplay.png",
    "pull_down/dlna.png",
    "pull_down/gapless_play.png",
    "pull_down/hibylink.png",
};
static asset_decoded_image_t qd_toggle_on_img[QD_TOGGLE_COUNT];

/* Row 3's tiles come from plugin.register_quick_toggle() rather than the
 * fixed table above, so their "on" artwork is decoded per registered toggle
 * (same accent recolor, just a runtime icon path). */
static asset_decoded_image_t qd_plugin_toggle_on_img[PLUGIN_MAX_QUICK_TOGGLES];

static void recolor_toggle_on_accent(lv_draw_buf_t * buf, lv_color_t accent) {
    if (!buf || !buf->data || !buf->header.w || !buf->header.h ||
        buf->header.cf != LV_COLOR_FORMAT_ARGB8888) return;
    uint32_t w = buf->header.w;
    uint32_t h = buf->header.h;
    uint32_t stride = buf->header.stride;
    for (uint32_t y = 0; y < h; y++) {
        lv_color32_t * row = (lv_color32_t *) (buf->data + y * stride);
        for (uint32_t x = 0; x < w; x++) {
            if (row[x].alpha == 0) continue; /* outside the circle -- leave transparent */
            uint8_t t = row[x].red;
            if (row[x].green < t) t = row[x].green;
            if (row[x].blue < t) t = row[x].blue;
            row[x].red   = (uint8_t) (accent.red   + ((uint16_t) (255 - accent.red)   * t) / 255);
            row[x].green = (uint8_t) (accent.green + ((uint16_t) (255 - accent.green) * t) / 255);
            row[x].blue  = (uint8_t) (accent.blue  + ((uint16_t) (255 - accent.blue)  * t) / 255);
        }
    }
}

static void load_quick_drawer_toggle_on_images(void) {
    lv_color_t accent = accent_lv_color();
    for (int i = 0; i < QD_TOGGLE_COUNT; i++) {
        asset_decoded_image_close(&qd_toggle_on_img[i]);
        if (asset_decoded_image_open(&qd_toggle_on_img[i], qd_toggle_on_asset[i]))
            recolor_toggle_on_accent((lv_draw_buf_t *) qd_toggle_on_img[i].decoder.decoded, accent);
    }
    int plugin_count = plugin_manager_get_quick_toggle_count();
    if (plugin_count > PLUGIN_MAX_QUICK_TOGGLES) plugin_count = PLUGIN_MAX_QUICK_TOGGLES;
    for (int i = 0; i < PLUGIN_MAX_QUICK_TOGGLES; i++) {
        asset_decoded_image_close(&qd_plugin_toggle_on_img[i]);
        if (i >= plugin_count) continue;
        if (asset_decoded_image_open(&qd_plugin_toggle_on_img[i],
                                     plugin_manager_get_quick_toggle_icon_selected(i)))
            recolor_toggle_on_accent((lv_draw_buf_t *) qd_plugin_toggle_on_img[i].decoder.decoded, accent);
    }
}

/* Off state stays the stock grey circle (nothing accent-coloured about it),
 * so only the on state goes through the decoded copy -- with the plain asset
 * path as the fallback if that decode ever failed. */
static const void * quick_drawer_toggle_src(qd_toggle_t t, bool on) {
    if (!on) return asset_path(qd_toggle_off_asset[t]);
    const void * src = asset_decoded_image_source(&qd_toggle_on_img[t]);
    return src ? src : (const void *) asset_path(qd_toggle_on_asset[t]);
}

/* quick_drawer_toggle_src()'s twin for a row-3 plugin tile, whose two icon
 * paths come from the registration rather than a compile-time table. */
static const void * quick_drawer_plugin_toggle_src(int index, bool on) {
    if (!on) return asset_path(plugin_manager_get_quick_toggle_icon(index));
    const void * src = (index >= 0 && index < PLUGIN_MAX_QUICK_TOGGLES)
                           ? asset_decoded_image_source(&qd_plugin_toggle_on_img[index])
                           : NULL;
    return src ? src : (const void *) asset_path(plugin_manager_get_quick_toggle_icon_selected(index));
}

/* Re-reads every expanded-row toggle from its authoritative source. Called
 * when the drawer opens rather than pushed to on change: three of these are
 * network services whose real state can move underneath us (a Wi-Fi drop
 * disables all of them via gui_network_handle_wifi_disabled()), and the
 * plugin tiles are documented as read-on-open. */
static void refresh_quick_drawer_expansion_toggles(void) {
    if (!quick_drawer_expansion_box) return;

    bool airplay = current_settings.wifi_dac_mode_enabled;
    if (quick_drawer_airplay_icon)
        lv_image_set_src(quick_drawer_airplay_icon, quick_drawer_toggle_src(QD_TOGGLE_AIRPLAY, airplay));
    quick_drawer_set_toggle_state(QD_TOGGLE_AIRPLAY, airplay);

    bool dlna = current_settings.dlna_renderer_enabled;
    if (quick_drawer_dlna_icon)
        lv_image_set_src(quick_drawer_dlna_icon, quick_drawer_toggle_src(QD_TOGGLE_DLNA, dlna));
    quick_drawer_set_toggle_state(QD_TOGGLE_DLNA, dlna);

    bool gapless = current_settings.gapless_enabled;
    if (quick_drawer_gapless_icon)
        lv_image_set_src(quick_drawer_gapless_icon, quick_drawer_toggle_src(QD_TOGGLE_GAPLESS, gapless));
    quick_drawer_set_toggle_state(QD_TOGGLE_GAPLESS, gapless);

    bool rc = current_settings.remote_control_enabled;
    if (quick_drawer_rc_icon)
        lv_image_set_src(quick_drawer_rc_icon, quick_drawer_toggle_src(QD_TOGGLE_RC, rc));
    quick_drawer_set_toggle_state(QD_TOGGLE_RC, rc);

    for (int i = 0; i < quick_drawer_plugin_toggle_count; i++) {
        bool on = plugin_manager_get_quick_toggle_value(i);
        if (quick_drawer_plugin_icon[i])
            lv_image_set_src(quick_drawer_plugin_icon[i], quick_drawer_plugin_toggle_src(i, on));
        quick_drawer_set_toggle_state_text(QD_TOGGLE_COUNT + i, on,
                                           plugin_manager_get_quick_toggle_state_text(i, on));
    }
}

static void quick_drawer_airplay_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_network_toggle_airplay();
    refresh_quick_drawer_expansion_toggles();
}

static void quick_drawer_dlna_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_network_toggle_dlna();
    refresh_quick_drawer_expansion_toggles();
}

void gui_shell_refresh_quick_drawer_expansion_toggles(void) {
    refresh_quick_drawer_expansion_toggles();
}

static void quick_drawer_gapless_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    /* Shared with the Settings switch so the crossfade implication, the
     * re-arm and both toggles' refreshes happen identically from either --
     * see gui_player_set_gapless_enabled(). */
    gui_player_set_gapless_enabled(!current_settings.gapless_enabled);
}

static void quick_drawer_rc_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_network_toggle_remote_control();
    refresh_quick_drawer_expansion_toggles();
}

static void quick_drawer_plugin_toggle_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    if (index < 0 || index >= quick_drawer_plugin_toggle_count) return;
    plugin_manager_quick_toggle_set(index, !plugin_manager_get_quick_toggle_value(index));
    refresh_quick_drawer_expansion_toggles();
}

static void render_wifi_icon(void) {
    /* Keep an in-flight enable visually enabled even before wlan0's
     * wpa_supplicant socket exists.  Association is still queried below,
     * so the topbar naturally advances from the existing disconnected
     * icon to signal strength without exposing an "enabling" state.  Once
     * poll_wifi_toggle() clears wifi_toggle_active, this immediately goes
     * back to the authoritative backend state and can still report a real
     * failure normally. */
    bool enabled = gui_shell_wifi_effective_enabled();
    if (quick_drawer_wifi_icon) {
        lv_image_set_src(quick_drawer_wifi_icon, quick_drawer_toggle_src(QD_TOGGLE_WIFI, enabled));
        quick_drawer_mark_snapshot_dirty();
    }
    quick_drawer_set_toggle_state(0, enabled);

    if (!enabled) {
        lv_obj_add_flag(wifi_icon, LV_OBJ_FLAG_HIDDEN);
        sync_topbar_status_icon_positions();
        return;
    }
    lv_obj_remove_flag(wifi_icon, LV_OBJ_FLAG_HIDDEN);
    sync_topbar_status_icon_positions();

    int level = wifi_status_level;
    if (wifi_status_connected) {
        static const char * const assets[] = {
            "topbar/lucide_wifi_zero.png", "topbar/lucide_wifi_low.png",
            "topbar/lucide_wifi_high.png", "topbar/lucide_wifi.png",
        };
        if (level < 0) level = 0;
        if (level > 3) level = 3;
        lv_image_set_src(wifi_icon, asset_path(assets[level]));
    } else {
        lv_image_set_src(wifi_icon, asset_path("topbar/lucide_wifi_off.png"));
    }
    layout_lucide_topbar_image(wifi_icon);
}

static void poll_wifi_status(void) {
    wifi_status_observe_enabled(gui_shell_wifi_effective_enabled());
    if (!wifi_status_active ||
        !atomic_load_explicit(&wifi_status_done, memory_order_acquire)) return;
    pthread_join(wifi_status_thread, NULL); /* worker has already finished its query */
    wifi_status_active = false;
    if (wifi_status_enabled && wifi_status_job_generation == wifi_status_generation) {
        wifi_status_connected = wifi_status_result_connected;
        wifi_status_level = wifi_status_result_level;
        render_wifi_icon();
    }
}

static void refresh_wifi_icon(void) {
    poll_wifi_status();
    render_wifi_icon();
    /* Preserve the existing ~5s refresh cadence, with at most one query. */
    if (!wifi_status_enabled || wifi_status_active || wifi_toggle_active) return;
    wifi_status_job_generation = wifi_status_generation;
    atomic_store_explicit(&wifi_status_done, false, memory_order_relaxed);
    if (pthread_create(&wifi_status_thread, NULL, wifi_status_thread_func, NULL) == 0)
        wifi_status_active = true;
}

/* Same treatment as refresh_wifi_icon(): the drawer's bt icon just reflects
 * powered-on/off, blue as soon as enabled -- only the top bar distinguishes
 * powered-but-nothing-paired from actually-connected, via
 * bt_control_is_connected() (checks each paired device's "Connected: yes"
 * state via bluetoothctl info, cheap -- no discovery scan). */
/* Mirrors current_settings.bt_dac_mode_enabled's real-world effect --
 * external_dac_block_reason() reads this (see its own comment) instead of
 * calling bt_control_is_powered() itself, since that's a subprocess spawn
 * (bluetoothctl show, potentially several seconds when Bluetooth actually is
 * powered on) and the block check runs on every play-button tap. Kept fresh
 * by refresh_bt_icon()'s existing periodic poll and by poll_bt_toggle()'s
 * immediate refresh after a manual toggle, rather than adding a new
 * subprocess call to the play hot path. */
bool bt_is_powered_cached = false;

/* The connected A2DP accessory's own MAC + live-negotiated codec, kept
 * fresh by the same background refresh_bt_icon_thread_func() poll as
 * bt_is_powered_cached above -- add_bt_device_row() (Bluetooth screen) uses
 * these to know which paired-device row is the actual A2DP-audio one (not
 * just "connected" -- a non-audio BLE peripheral could be connected too)
 * and what to print on its second line. Empty when nothing's A2DP-connected. */
char bt_connected_mac_cached[18] = "";
char bt_connected_codec_cached[32] = "";
unsigned int bt_connected_rate_cached = 0;

/* /usr/bin/bt_init's last line creates /tmp/bt_init_ok once chip firmware
 * flash and initialization complete. Because /tmp is tmpfs, this flag is
 * never stale from a prior boot.
 * Checked before attempting Bluetooth toggles to prevent concurrent UART
 * access while the system initialization script is running. */
#define BT_INIT_OK_FLAG_PATH "/tmp/bt_init_ok"

/* Bluetooth status is unknown, not off, while the stock asynchronous
 * S80_bt_init job is still flashing/attaching the controller.  No status
 * subprocess may start before its tmpfs completion marker appears: doing so
 * captures the real temporary powered state and flashes the icon on the first
 * Home frames.  This latch is polled with access() from the existing 500ms UI
 * timer; once true it stays true for this process lifetime and normal
 * authoritative background polling begins immediately. */
static bool bt_startup_ready = false;

static bool refresh_bt_startup_readiness(void) {
    if (!bt_startup_ready && access(BT_INIT_OK_FLAG_PATH, F_OK) == 0)
        bt_startup_ready = true;
    return bt_startup_ready;
}

/* Armed only by an app-driven enable path, never by the periodic boot-state
 * poll itself. poll_refresh_bt_icon() consumes it once that existing poll
 * reports an authoritative powered state. This adds no subprocesses to the
 * toggle worker, and hci0's transient boot-time powered window cannot arm
 * AVRCP by itself. */
static atomic_bool bt_media_player_enable_pending = false;

static void mark_bt_media_player_enable_pending(void) {
    atomic_store_explicit(&bt_media_player_enable_pending, true, memory_order_release);
}

/* Shared Bluetooth device scan results, referenced by both
 * refresh_bt_icon_thread_func() and the Bluetooth settings screen
 * (populate_bt_screen()). */
#define BT_MAX_RESULTS 32
bt_device_t bt_scan_results[BT_MAX_RESULTS];
int bt_scan_result_count = 0;

/* Written by refresh_bt_icon_thread_func() below, merged into
 * bt_scan_results by poll_refresh_bt_icon() -- see its own comment. */
static bt_device_t bt_paired_states_result[BT_MAX_RESULTS];
static int bt_paired_states_count = 0;

/* Bluetooth status checks (bt_control_is_powered / bt_control_is_connected)
 * run asynchronously in a worker thread to prevent bluetoothctl subprocess
 * execution from blocking the UI thread during streaming or slow state
 * transitions. */
static pthread_t refresh_bt_icon_thread;
static bool refresh_bt_icon_active = false;
static atomic_bool refresh_bt_icon_done_flag = false;
static bool refresh_bt_icon_result_powered = false;
static bool refresh_bt_icon_result_connected = false;
static bool refresh_bt_icon_result_a2dp_connected = false;
static char refresh_bt_icon_result_mac[18] = "";
static char refresh_bt_icon_result_codec[32] = "";
static unsigned int refresh_bt_icon_result_rate = 0;

/* UI-thread owned Bluetooth audio state and disconnect generation latch */
static bool bt_is_a2dp_connected_ui = false;
static uint32_t bt_disconnect_epoch = 0;
static uint32_t bt_worker_launch_epoch = 0;
static uint32_t bt_preference_epoch = 0;
static uint32_t bt_worker_preference_epoch = 0;

/* Snapshotted on the UI thread in start_refresh_bt_icon(), before the worker
 * launches -- gui_navigation_is_top()/gui_network_get_bt_screen() touch the
 * navigation stack and must not be called from refresh_bt_icon_thread_func()
 * itself (a background thread), same reasoning as every other UI-state
 * value this worker reads via a snapshot rather than a live getter. */
static bool bt_worker_bt_screen_visible = false;

/* One persisted-device reconnect attempt is armed per authoritative
 * powered-on cycle. A link loss alone must not re-arm it; only a later
 * off->on observation (or the initial powered-on observation at startup)
 * starts a new cycle. */
static bool bt_reconnect_observed_powered = false;
static bool bt_reconnect_observed_power_valid = false;
static bool bt_reconnect_pending = false;
static bool bt_reconnect_cycle_suppressed = false;
static uint32_t bt_power_status_generation = 0;

static void cancel_bt_reconnect_for_cycle(bool suppress_cycle) {
    bt_reconnect_cancel();
    bt_reconnect_pending = false;
    if (suppress_cycle) bt_reconnect_cycle_suppressed = true;
}

void gui_shell_cancel_bt_reconnect(void) {
    /* Invalidate preference updates from an in-flight refresh. Manual
     * actions (especially Forget) must not let a pre-action positive A2DP
     * result repopulate the remembered preference when that worker returns.
     * Keep this separate from the audio-route epoch: cancelling a reconnect
     * is not evidence that the current headphones disconnected. */
    bt_preference_epoch++;
    cancel_bt_reconnect_for_cycle(true);
}

/* Forward declaration: route teardown is also required by immediate
 * disconnect/power-off paths, before the next authoritative poll lands. */
static void clear_bt_audio_route_now(void);

bool gui_shell_is_bt_audio_connected(void) { return bt_is_a2dp_connected_ui; }

static bool last_codec_eligible = false;
static bt_codec_type_t last_codec_type = BT_CODEC_TYPE_NONE;
static uint32_t last_codec_layout_sig = 0;
static bool hidden_due_to_overlap = false;

static void invalidate_bt_codec_status_cache(void) {
    last_codec_eligible = false;
    last_codec_type = BT_CODEC_TYPE_NONE;
    last_codec_layout_sig = 0;
    hidden_due_to_overlap = false;
}

void gui_shell_notify_bt_audio_disconnected(void) {
    bt_disconnect_epoch++;
    bt_is_a2dp_connected_ui = false;
    bt_connected_mac_cached[0] = '\0';
    bt_connected_codec_cached[0] = '\0';
    bt_connected_rate_cached = 0;
    clear_bt_audio_route_now();
    if (a2dp_status_icon) lv_obj_add_flag(a2dp_status_icon, LV_OBJ_FLAG_HIDDEN);
    if (bt_codec_status_icon) lv_obj_add_flag(bt_codec_status_icon, LV_OBJ_FLAG_HIDDEN);
    invalidate_bt_codec_status_cache();
    sync_bt_codec_status_icon();
}

static void * refresh_bt_icon_thread_func(void * arg) {
    (void) arg;
    bool powered = bt_control_is_powered();
    refresh_bt_icon_result_powered = powered;

    /* Same background thread/cadence as everything else here -- one more
     * subprocess call (bluealsactl list-pcms) alongside the bluetoothctl
     * calls below, not a separate poll loop. */
    refresh_bt_icon_result_a2dp_connected = powered && bt_control_is_a2dp_source_connected();
    if (powered && !refresh_bt_icon_result_a2dp_connected) {
        /* A sample-rate change recreates the BlueALSA PCM (observed: 90 ms
         * from removal to replacement). A poll in that gap must not clear
         * the route or stop the disconnect watcher. Confirm absence after
         * the same grace interval as the watcher, entirely off the UI thread. */
        usleep(BT_OUTPUT_RECONFIGURE_GRACE_MS * 1000U);
        refresh_bt_icon_result_a2dp_connected = bt_control_is_a2dp_source_connected();
    }

    /* Two more subprocess calls (bluealsactl info, reusing the same PCM
     * path lookup bt_control_is_a2dp_source_connected() just did) -- only
     * worth paying when something's actually A2DP-connected. Both left at
     * "" (not stale) when nothing is, so add_bt_device_row() never shows a
     * leftover codec line for a device that just disconnected. */
    refresh_bt_icon_result_mac[0] = '\0';
    refresh_bt_icon_result_codec[0] = '\0';
    refresh_bt_icon_result_rate = 0;
    if (refresh_bt_icon_result_a2dp_connected) {
        bt_control_get_connected_device_mac(refresh_bt_icon_result_mac, sizeof(refresh_bt_icon_result_mac));
        refresh_bt_icon_result_rate = 0;
        bt_control_get_connected_device_stream(refresh_bt_icon_result_codec, sizeof(refresh_bt_icon_result_codec),
                                              &refresh_bt_icon_result_rate);
    }

    if (powered) {
        if (bt_worker_bt_screen_visible) {
            /* bt_control_list_paired_states(), not bt_control_is_connected() --
             * same per-device `bluetoothctl info` cost either way, but this also
             * hands back the full breakdown poll_refresh_bt_icon() merges into
             * bt_scan_results below, instead of throwing it away. -1 (the query
             * itself failed, not "genuinely 0 paired") is normalized to 0 here
             * for the any_connected scan below (an empty loop either way), but
             * poll_refresh_bt_icon() checks the raw value separately before
             * treating "nothing here" as authoritative -- see its own comment. */
            bt_paired_states_count = bt_control_list_paired_states(bt_paired_states_result, BT_MAX_RESULTS);
            bool any_connected = false;
            for (int i = 0; i < bt_paired_states_count; i++) {
                if (bt_paired_states_result[i].connected) { any_connected = true; break; }
            }
            refresh_bt_icon_result_connected = any_connected;
        } else {
            /* Bluetooth screen isn't open, so nothing reads the per-device
             * breakdown this cycle -- skip the O(paired devices) fork loop
             * entirely and ask for just the boolean the topbar icon actually
             * needs. -1 (not -- as opposed to 0 devices/none connected --
             * refreshed this cycle) makes poll_refresh_bt_icon() skip the
             * bt_scan_results merge below and keep whatever it last had,
             * same "skip merge, retain current state" convention the -1
             * query-failure case above already relies on. */
            bt_paired_states_count = -1;
            int any_connected_now = bt_control_any_paired_connected();
            /* -1 means this cycle couldn't tell (see the .c file's own
             * comment) -- leave refresh_bt_icon_result_connected at its
             * last known value instead of guessing "nothing connected". */
            if (any_connected_now >= 0) refresh_bt_icon_result_connected = any_connected_now != 0;
        }
    } else {
        bt_paired_states_count = 0;
        refresh_bt_icon_result_connected = false;
    }

    atomic_store_explicit(&refresh_bt_icon_done_flag, true, memory_order_release); /* written last -- poll_refresh_bt_icon only checks this flag */
    return NULL;
}

static void start_refresh_bt_icon(void) {
    if (!refresh_bt_startup_readiness()) return;
    if (refresh_bt_icon_active) return; /* previous check still in flight -- same "ignore taps until it lands" pattern as everything else here */
    refresh_bt_icon_active = true;
    bt_worker_launch_epoch = bt_disconnect_epoch;
    bt_worker_preference_epoch = bt_preference_epoch;
    bt_worker_bt_screen_visible = gui_navigation_is_top(gui_network_get_bt_screen());
    atomic_store_explicit(&refresh_bt_icon_done_flag, false, memory_order_relaxed);
        if (pthread_create(&refresh_bt_icon_thread, NULL, refresh_bt_icon_thread_func, NULL) != 0) {
        refresh_bt_icon_active = false;
    }
}


/* Updates the negotiated Bluetooth A2DP codec badge in the topbar.
 * Fully edge-triggered: caches eligibility, codec type, and neighboring
 * layout visibility to avoid redundant lv_image_set_src() or layout recomputations
 * during periodic polling when state is unchanged. */
static void sync_bt_codec_status_icon(void) {
    if (!bt_codec_status_icon) return;

    bt_codec_type_t codec_type = bt_codec_identify(bt_connected_codec_cached);
    bool eligible = bt_is_powered_cached && bt_is_a2dp_connected_ui &&
                    (codec_type != BT_CODEC_TYPE_NONE);

    bool currently_hidden = lv_obj_has_flag(bt_codec_status_icon, LV_OBJ_FLAG_HIDDEN);

    if (!eligible) {
        if (last_codec_eligible || !currently_hidden || hidden_due_to_overlap) {
            lv_obj_add_flag(bt_codec_status_icon, LV_OBJ_FLAG_HIDDEN);
            invalidate_bt_codec_status_cache();
        }
        return;
    }

    /* Compute layout signature of all neighboring elements that affect horizontal width */
    bool hp_vis = volume_topbar_headphone && !lv_obj_has_flag(volume_topbar_headphone, LV_OBJ_FLAG_HIDDEN);
    bool a2dp_vis = a2dp_status_icon && !lv_obj_has_flag(a2dp_status_icon, LV_OBJ_FLAG_HIDDEN);
    bool usb_vis = usb_audio_status_icon && !lv_obj_has_flag(usb_audio_status_icon, LV_OBJ_FLAG_HIDDEN);
    bool pp_vis = play_pause_status_icon && !lv_obj_has_flag(play_pause_status_icon, LV_OBJ_FLAG_HIDDEN);
    bool ampm_vis = clock_topbar_ampm && !lv_obj_has_flag(clock_topbar_ampm, LV_OBJ_FLAG_HIDDEN);
    int vol_digits_vis = 0;
    for (int i = 0; i < 3; i++) {
        if (volume_topbar_digit[i] && !lv_obj_has_flag(volume_topbar_digit[i], LV_OBJ_FLAG_HIDDEN)) {
            vol_digits_vis++;
        }
    }

    uint32_t layout_sig = (hp_vis ? 1 : 0) |
                          (a2dp_vis ? 2 : 0) |
                          (usb_vis ? 4 : 0) |
                          (pp_vis ? 8 : 0) |
                          (ampm_vis ? 16 : 0) |
                          ((uint32_t)(vol_digits_vis & 0x7) << 5);

    /* If eligibility, codec type, and layout factors haven't changed, and the badge is in its steady state
     * (either visible or already known to be hidden due to right-side overlap), do nothing */
    if (last_codec_eligible && last_codec_type == codec_type && last_codec_layout_sig == layout_sig) {
        if (hidden_due_to_overlap || !currently_hidden) {
            return;
        }
    }

    last_codec_eligible = true;
    last_codec_layout_sig = layout_sig;

    if (last_codec_type != codec_type) {
        last_codec_type = codec_type;
        const char * asset = bt_codec_get_asset(codec_type);
        if (asset) {
            lv_image_set_src(bt_codec_status_icon, asset);
        }
    }

    lv_obj_remove_flag(bt_codec_status_icon, LV_OBJ_FLAG_HIDDEN);

    /* Hide the optional codec badge when the left row would reach the
     * centered clock or the right-side indicators. */
    hidden_due_to_overlap = false;
    if (volume_topbar_group) {
        lv_obj_update_layout(volume_topbar_group);
        int32_t left_right = lv_obj_get_x(volume_topbar_group) + lv_obj_get_width(volume_topbar_group);
        int32_t right_boundary = LV_COORD_MAX;
        if (clock_topbar_group) {
            lv_obj_update_layout(clock_topbar_group);
            right_boundary = lv_obj_get_x(clock_topbar_group);
        }
        if (bt_status_icon && !lv_obj_has_flag(bt_status_icon, LV_OBJ_FLAG_HIDDEN)) {
            int32_t x = lv_obj_get_x(bt_status_icon);
            if (x < right_boundary) right_boundary = x;
        }
        if (wifi_icon && !lv_obj_has_flag(wifi_icon, LV_OBJ_FLAG_HIDDEN)) {
            int32_t x = lv_obj_get_x(wifi_icon);
            if (x < right_boundary) right_boundary = x;
        }
        if (battery_topbar_group) {
            lv_obj_update_layout(battery_topbar_group);
            int32_t x = lv_obj_get_x(battery_topbar_group);
            if (x < right_boundary) right_boundary = x;
        }
        if (right_boundary != LV_COORD_MAX && left_right + 6 > right_boundary) {
            lv_obj_add_flag(bt_codec_status_icon, LV_OBJ_FLAG_HIDDEN);
            lv_obj_update_layout(volume_topbar_group);
            hidden_due_to_overlap = true;
        }
    }
}

/* Only this lifecycle worker starts/stops the monitors. A rapid reconnect
 * updates the desired state without making LVGL join a live monitor. */
static pthread_t bt_monitor_lifecycle_thread;
static bool bt_monitor_lifecycle_active;
static atomic_bool bt_monitor_lifecycle_done;
static bool bt_monitor_want_output, bt_monitor_want_volume;
static bool bt_monitor_job_output, bt_monitor_job_volume;
static bool bt_monitor_applied_output, bt_monitor_applied_volume;
static bool bt_monitor_refresh_requested;

static void poll_bt_monitor_lifecycle(void);

static void clear_bt_audio_route_now(void) {
    audio_set_bt_output(false);
    usb_dac_bridge_set_bt_output(false);
    bt_monitor_want_output = false;
    bt_monitor_want_volume = false;
    /* A desired/applied mismatch already schedules the required stop after an
     * in-flight lifecycle job completes. Only force a retry when no lifecycle
     * job is active but a monitor is still actually running; repeated
     * authoritative-off polls must not spawn redundant stop workers. */
    bt_monitor_refresh_requested = !bt_monitor_lifecycle_active &&
        (bt_control_source_volume_sync_is_running() ||
         bt_control_output_disconnect_watch_is_running());
    poll_bt_monitor_lifecycle();
}

static void * bt_monitor_lifecycle_worker(void * unused) {
    (void)unused;
    if (bt_monitor_job_volume) bt_control_source_volume_sync_start();
    else bt_control_source_volume_sync_stop();
    if (bt_monitor_job_output) bt_control_output_disconnect_watch_start();
    else bt_control_output_disconnect_watch_stop();
    atomic_store_explicit(&bt_monitor_lifecycle_done, true, memory_order_release);
    return NULL;
}

static void poll_bt_monitor_lifecycle(void) {
    if (bt_monitor_lifecycle_active) {
        if (!atomic_load_explicit(&bt_monitor_lifecycle_done, memory_order_acquire)) return;
        pthread_join(bt_monitor_lifecycle_thread, NULL);
        bt_monitor_lifecycle_active = false;
        bt_monitor_applied_output = bt_monitor_job_output;
        bt_monitor_applied_volume = bt_monitor_job_volume;
    }
    if (!bt_monitor_refresh_requested && bt_monitor_want_output == bt_monitor_applied_output &&
        bt_monitor_want_volume == bt_monitor_applied_volume) return;
    bt_monitor_job_output = bt_monitor_want_output;
    bt_monitor_job_volume = bt_monitor_want_volume;
    atomic_store_explicit(&bt_monitor_lifecycle_done, false, memory_order_relaxed);
    if (pthread_create(&bt_monitor_lifecycle_thread, NULL, bt_monitor_lifecycle_worker, NULL) == 0) {
        bt_monitor_lifecycle_active = true;
        bt_monitor_refresh_requested = false;
    }
}

static void poll_refresh_bt_icon(void) {
    if (!refresh_bt_icon_active || !atomic_load_explicit(&refresh_bt_icon_done_flag, memory_order_acquire)) return;
    refresh_bt_icon_active = false;
    pthread_join(refresh_bt_icon_thread, NULL);

    /* While a manual toggle is in-flight (bt_toggle_active), ignore the
     * background poll result to prevent overwriting the optimistic state
     * with stale pre-toggle hardware readings. */
    if (bt_toggle_active) return;

    bool display_powered = refresh_bt_icon_result_powered;

    bool powered_rising = display_powered &&
        (!bt_reconnect_observed_power_valid || !bt_reconnect_observed_powered);
    if (!display_powered) {
        /* An authoritative power-off ends the old cycle. Clear suppression so
         * the next real power-on can arm a fresh, single reconnect attempt. */
        cancel_bt_reconnect_for_cycle(false);
        bt_reconnect_observed_power_valid = true;
        bt_reconnect_observed_powered = false;
        bt_reconnect_cycle_suppressed = false;
    } else {
        bt_reconnect_observed_power_valid = true;
        bt_reconnect_observed_powered = true;
        if (powered_rising && !bt_reconnect_cycle_suppressed &&
            !current_settings.bt_dac_mode_enabled)
            bt_reconnect_pending = true;
    }

#ifndef HOST_BUILD
    if (display_powered &&
        atomic_exchange_explicit(&bt_media_player_enable_pending, false, memory_order_acq_rel))
        bt_media_player_init();
#endif

    /* Discard stale A2DP/codec results if a disconnect occurred while or after the worker launched */
    bool a2dp_connected = display_powered && refresh_bt_icon_result_a2dp_connected;
    if (bt_worker_launch_epoch != bt_disconnect_epoch) {
        a2dp_connected = false;
    }

    bt_is_powered_cached = display_powered;
    bt_is_a2dp_connected_ui = a2dp_connected;
    bt_power_status_generation++;

    char previous_mac[sizeof(bt_connected_mac_cached)];
    snprintf(previous_mac, sizeof(previous_mac), "%s", bt_connected_mac_cached);
    snprintf(bt_connected_mac_cached, sizeof(bt_connected_mac_cached), "%s", a2dp_connected ? refresh_bt_icon_result_mac : "");
    if (strcmp(previous_mac, bt_connected_mac_cached) != 0) {
        /* The right transport rate belongs to the accessory, so the effective
         * one follows whatever is connected: 44.1 kHz headphones and a 96 kHz
         * LDAC speaker each keep their own. Only on a real change, so a
         * rate being applied right now is not overwritten by a routine
         * refresh that happens to see the link mid-cycle. */
        bt_control_set_sample_rate(settings_bt_rate_for(&current_settings, bt_connected_mac_cached));
        gui_network_notify_bt_device_changed();
    }
    snprintf(bt_connected_codec_cached, sizeof(bt_connected_codec_cached), "%s", a2dp_connected ? refresh_bt_icon_result_codec : "");
    bt_connected_rate_cached = a2dp_connected ? refresh_bt_icon_result_rate : 0;
    if (quick_drawer_bt_icon) {
        lv_image_set_src(quick_drawer_bt_icon, quick_drawer_toggle_src(QD_TOGGLE_BT, display_powered));
        quick_drawer_mark_snapshot_dirty();
    }
    quick_drawer_set_toggle_state(1, display_powered);

    if (!display_powered) {
        clear_bt_audio_route_now();
        lv_obj_add_flag(bt_status_icon, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(a2dp_status_icon, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(bt_codec_status_icon, LV_OBJ_FLAG_HIDDEN);
        invalidate_bt_codec_status_cache();
        sync_topbar_status_icon_positions();
        /* Bluetooth screen's own toggle row + everything gated on it reads
         * bt_is_powered_cached too -- rebuilt only while that screen is visible. */
        if (gui_navigation_is_top(gui_network_get_bt_screen())) populate_bt_screen();
        return;
    }
    lv_obj_remove_flag(bt_status_icon, LV_OBJ_FLAG_HIDDEN);
    lv_image_set_src(bt_status_icon, asset_path(refresh_bt_icon_result_connected ?
                     "topbar/lucide_bluetooth.png" : "topbar/lucide_bluetooth_off.png"));
    layout_lucide_topbar_image(bt_status_icon);
    sync_topbar_status_icon_positions();
    if (a2dp_connected) {
        lv_obj_remove_flag(a2dp_status_icon, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(a2dp_status_icon, LV_OBJ_FLAG_HIDDEN);
    }
    sync_bt_codec_status_icon();

    /* Update scan results in-place by MAC match against the fresh paired states
     * snapshot from the background poll. If bt_paired_states_count is negative,
     * the query failed, so skip merge to retain current state. */
    if (bt_paired_states_count >= 0) {
        for (int j = 0; j < bt_scan_result_count; j++) {
            bool found = false;
            for (int i = 0; i < bt_paired_states_count; i++) {
                if (strcmp(bt_scan_results[j].mac, bt_paired_states_result[i].mac) == 0) {
                    bt_scan_results[j].paired = bt_paired_states_result[i].paired;
                    bt_scan_results[j].connected = bt_paired_states_result[i].connected;
                    found = true;
                    break;
                }
            }
            if (!found) {
                bt_scan_results[j].paired = false;
                bt_scan_results[j].connected = false;
            }
        }
    }
    /* Rebuild Bluetooth screen only when it is currently on top to avoid
     * UI stutter on other screens. */
    if (gui_navigation_is_top(gui_network_get_bt_screen())) populate_bt_screen();

    /* Route audio to Bluetooth when connected, unless Bluetooth DAC mode is
     * enabled (which runs bluealsa as an A2DP sink rather than source). */
    bool use_bt_output = a2dp_connected && !current_settings.bt_dac_mode_enabled;
    audio_set_bt_output(use_bt_output);

    /* Volume synchronization with Bluetooth audio output devices. */
    bt_monitor_want_volume = use_bt_output && current_settings.bt_volume_sync_enabled;
    bt_monitor_want_output = use_bt_output;
    /* Retry missing monitors, without creating a thread on every healthy
     * radio poll. Failed starts and unexpected EOF are both retried here. */
    bt_monitor_refresh_requested =
        (bt_monitor_want_volume && !bt_control_source_volume_sync_is_running()) ||
        (bt_monitor_want_output && !bt_control_output_disconnect_watch_is_running());
    poll_bt_monitor_lifecycle();

    /* Mirror Bluetooth output setting to USB DAC bridge when active. */
    usb_dac_bridge_set_bt_output(use_bt_output);

    /* Persist only a verified A2DP snapshot from this refresh worker. The
     * epoch check above prevents a stale pre-disconnect result from becoming
     * the remembered output device. Disconnections intentionally retain the
     * preference until the user forgets it. */
    if (a2dp_connected && bt_worker_preference_epoch == bt_preference_epoch &&
        refresh_bt_icon_result_mac[0] &&
        strcmp(current_settings.bt_last_output_mac, refresh_bt_icon_result_mac) != 0 &&
        strlen(refresh_bt_icon_result_mac) == 17) {
        snprintf(current_settings.bt_last_output_mac,
                 sizeof(current_settings.bt_last_output_mac), "%s", refresh_bt_icon_result_mac);
        settings_save_async(&current_settings);
    }
}

/* Volume popup moved to gui_player.c */


/* Android-style home indicator: a small pill fixed to the bottom edge,
 * living on lv_layer_top() (drawn above every screen) so a swipe-up
 * starting there is caught by this object.
 * Position tracking is handled via raw coordinate polling in
 * poll_quick_drawer_drag() / gesture_home_state_is_eligible(). */

static void build_home_indicator_bar(void) {
    lv_obj_t * top = lv_layer_top();

    home_indicator_band = lv_obj_create(top);
    lv_obj_remove_style_all(home_indicator_band);
    /* Match the clickable object to the complete raw-coordinate press-down
     * surface accepted by gesture_home_state_poll(): the normal 24px band
     * plus HOME_SWIPE_HIT_EXTRA_PX.  The band remains visually transparent;
     * only the centered pill below is drawn. */
    lv_obj_set_size(home_indicator_band, lv_pct(100),
                    HOME_INDICATOR_BAND_HEIGHT + HOME_SWIPE_HIT_EXTRA_PX);
    lv_obj_align(home_indicator_band, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_remove_flag(home_indicator_band, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(home_indicator_band, LV_OBJ_FLAG_CLICKABLE); /* claims touches in this strip before any list underneath can -- the actual swipe-up trigger is poll_quick_drawer_drag()'s raw position polling, not a click/gesture event on this object */

    /* The visible pill itself -- plain light-gray rounded bar, matching
     * Android's own gesture-nav home indicator. */
    lv_obj_t * pill = lv_obj_create(home_indicator_band);
    lv_obj_remove_style_all(pill);
    lv_obj_set_size(pill, BOARD_SCALE_PX(120), BOARD_SCALE_PX(4));
    lv_obj_align(pill, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(pill, lv_color_make(220, 220, 220), 0);
    lv_obj_set_style_bg_opa(pill, LV_OPA_60, 0);
    lv_obj_set_style_radius(pill, 2, 0);
    lv_obj_remove_flag(pill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(pill, LV_OBJ_FLAG_CLICKABLE); /* purely visual -- home_indicator_band above is what's actually clickable */

    lv_obj_add_flag(home_indicator_band, current_settings.swipe_up_home_enabled ? 0 : LV_OBJ_FLAG_HIDDEN);
}

/* Generic transient error/status toast, top layer, auto-hides after 2.5s --
 * same shape as volume_popup above. Reusable anywhere a background op can
 * fail with something worth telling the user about; nothing like this
 * existed before (see poll_subsonic_download()'s and
 * subsonic_connect_row_cb's own "no error-toast UI exists yet" notes) --
 * first real use is Wi-Fi/Bluetooth connect failures. */
/* error_toast moved to gui_notifications.c */

/* Fully automatic, no Settings entry -- meant to feel like the wired
 * headphone jack (refresh_headphone_icon() above), not a mode the user
 * switches into (unlike Storage/USB DAC/ADB in the manual USB Mode
 * screen). usb_audio_output_is_connected() is a plain /proc file read (no
 * subprocess), same cheap class of check as get_headphone_state()'s own
 * direct sysfs reads, so this is safe to call directly on the UI thread at
 * the same low cadence as the wifi/Bluetooth polls (see their own
 * WIFI_POLL_TICKS call site) rather than needing its own background
 * thread. Toast fires only on the actual connect transition (was_connected
 * tracked across calls), matching how "Paused: headphones disconnected"
 * only fires once per real disconnect rather than every poll tick. */
static void poll_usb_audio_output(void) {
    static bool was_connected = false;
    char alsa_device[32];
    /* When USB_MODE_DAC is active, the USB port operates in gadget mode rather
     * than host mode, so external host-accessory audio devices cannot be
     * connected. Bypassing detection while in USB_MODE_DAC prevents false
     * positives from redirecting audio output away from the DAC bridge. */
    bool connected = current_settings.usb_mode != USB_MODE_DAC &&
                      usb_audio_output_is_connected(alsa_device, sizeof(alsa_device));

    if (connected && !was_connected) show_error_toast("USB audio device detected");
    was_connected = connected;

    audio_set_usb_output(connected, alsa_device);
    bool was_hidden = lv_obj_has_flag(usb_audio_status_icon, LV_OBJ_FLAG_HIDDEN);
    if (connected) {
        lv_obj_remove_flag(usb_audio_status_icon, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(usb_audio_status_icon, LV_OBJ_FLAG_HIDDEN);
    }
    if (was_hidden == connected) {
        sync_bt_codec_status_icon();
    }
}

/* Neutral-styled sibling of show_error_toast() -- that one's red color
 * scheme and short 2.5s/400x70 sizing fit a brief failure message, not a
 * longer explanatory one (first use: Car Mode's own explanation on
 * enabling). Bigger box for wrapping, 5s so there's time to actually read
 * it, no error coloring since nothing failed. */
/* info_toast moved to gui_notifications.c */



/* Quick-access drawer (Android-style notification-shade convention): slides
 * down over the whole screen from a swipe-down starting near the status
 * bar. Real pull_down/ theme2 assets throughout. Every row-1 icon (Wifi/
 * Bluetooth, mirroring the same wifi_status.c/bluetooth_control.c state as
 * the main status bar; crossfade; sleep timer) and the now-playing card
 * (real playback state, reusing the exact same callbacks as the player
 * screen's own transport buttons) are backed by real functionality.
 * QUICK_DRAWER_ANIM_MS/TRIGGER_ZONE are defined earlier, alongside
 * screen_gesture_event_cb, which needs the latter for its
 * swipe-down-near-the-top-edge check. */

/* Crossfade toggle control. Synchronized bidirectionally with Settings > Crossfade
 * (refresh_quick_drawer_crossfade_icon() / gui_settings_sync_crossfade_toggle()).
 * Uses pull_down/fade.png and pull_down/fade_s.png. */
static lv_obj_t * quick_drawer_crossfade_icon;
void refresh_quick_drawer_crossfade_icon(void) {
    if (!quick_drawer_crossfade_icon) return;
    lv_image_set_src(quick_drawer_crossfade_icon,
                     quick_drawer_toggle_src(QD_TOGGLE_CROSSFADE, current_settings.crossfade_enabled));
    quick_drawer_set_toggle_state(3, current_settings.crossfade_enabled);
    quick_drawer_mark_snapshot_dirty();
}

/* Settings > Music Settings > Playback's Crossfade toggle row is kept in sync
 * with quick drawer toggles via gui_settings_sync_crossfade_toggle(). */

static void quick_drawer_crossfade_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    /* Shared with the Settings switch so the gapless implication is applied
     * identically from both -- see gui_player_set_crossfade_enabled(). */
    gui_player_set_crossfade_enabled(!current_settings.crossfade_enabled);
    gui_settings_sync_crossfade_toggle();
}

/* Defined later, alongside the rest of the transport-button wiring --
 * forward-declared here since poll_sleep_timer() below needs it on
 * expiry. */


/* Sleep timer: arms/disarms countdown from current_settings.sleep_timer_minutes.
 * quick_drawer_sleep_label displays remaining time while armed.
 * Arming state is session-only and not persisted across restarts. */
static bool sleep_timer_active = false;
static uint32_t sleep_timer_start_tick = 0;
static lv_obj_t * quick_drawer_sleep_icon;
static lv_obj_t * quick_drawer_sleep_label;
/* Shared by the drawer icon's own click handler and the Settings > Sleep
 * Timer toggle (quick_drawer_sleep_timer_set_active(), gui_settings.c) --
 * both need the exact same icon/label/tick bookkeeping, and both then call
 * gui_settings_sync_sleep_timer_toggle() themselves afterward to keep the
 * OTHER one's widget in sync (same bidirectional pattern as crossfade --
 * see refresh_quick_drawer_crossfade_icon()'s own comment). poll_sleep_
 * timer()'s own expiry path below updates the drawer icon/label inline
 * instead of calling this (it needs the pause-playback side effect too),
 * but still calls that same sync afterward. */
static void apply_sleep_timer_active(bool active) {
    sleep_timer_active = active;
    quick_drawer_set_toggle_state(2, sleep_timer_active);
    if (sleep_timer_active) {
        sleep_timer_start_tick = lv_tick_get();
        lv_image_set_src(quick_drawer_sleep_icon, quick_drawer_toggle_src(QD_TOGGLE_SLEEP, true));
        lv_label_set_text_fmt(quick_drawer_sleep_label, "%dm", current_settings.sleep_timer_minutes);
        /* The measured drawer reserves the state row for "On"/"Off"; keep
         * the minute countdown internal so it cannot collide with that row. */
        lv_obj_add_flag(quick_drawer_sleep_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_image_set_src(quick_drawer_sleep_icon, asset_path("pull_down/sleep_switch.png"));
        lv_obj_add_flag(quick_drawer_sleep_label, LV_OBJ_FLAG_HIDDEN);
        quick_drawer_set_toggle_state(2, false);
    }
    quick_drawer_mark_snapshot_dirty();
}

bool quick_drawer_sleep_timer_is_active(void) {
    return sleep_timer_active;
}

/* For Settings > Sleep Timer's "Show Time Remaining" button (build_sleep_
 * timer_screen(), gui_settings.c) -- same total_ms/elapsed_ms math as
 * poll_sleep_timer() below, just returning the value instead of acting on
 * it. 0 when not armed, never negative. */
int quick_drawer_sleep_timer_remaining_seconds(void) {
    if (!sleep_timer_active) return 0;
    uint32_t total_ms = (uint32_t) current_settings.sleep_timer_minutes * 60000;
    uint32_t elapsed_ms = lv_tick_elaps(sleep_timer_start_tick);
    if (elapsed_ms >= total_ms) return 0;
    return (int) ((total_ms - elapsed_ms) / 1000);
}

/* Settings > Sleep Timer enable toggle synchronization. */
void quick_drawer_sleep_timer_set_active(bool active) {
    apply_sleep_timer_active(active);
}

static void quick_drawer_sleep_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    apply_sleep_timer_active(!sleep_timer_active);
    gui_settings_sync_sleep_timer_toggle();
}

/* Called every update_timer_cb tick (500ms). Cheap no-op when not armed. */
static void poll_sleep_timer(void) {
    if (!sleep_timer_active) return;

    uint32_t total_ms = (uint32_t) current_settings.sleep_timer_minutes * 60000;
    uint32_t elapsed_ms = lv_tick_elaps(sleep_timer_start_tick);

    if (elapsed_ms >= total_ms) {
        sleep_timer_active = false;
        if (audio_is_playing()) toggle_play_pause(); /* pause, not stop -- resumable, same as any other pause */
        lv_image_set_src(quick_drawer_sleep_icon, asset_path("pull_down/sleep_switch.png"));
        lv_obj_add_flag(quick_drawer_sleep_label, LV_OBJ_FLAG_HIDDEN);
        quick_drawer_set_toggle_state(2, false);
        quick_drawer_mark_snapshot_dirty();
        gui_settings_sync_sleep_timer_toggle(); /* Settings' toggle must not keep showing armed once expiry disarmed it */
        return;
    }

    /* Round up so the label never shows "0m" for the last, still-live
     * sub-minute stretch -- counts down 15,14,...,1 then disarms above
     * rather than ever displaying a misleading zero. */
    int remaining_min = (int) ((total_ms - elapsed_ms + 59999) / 60000);
    lv_label_set_text_fmt(quick_drawer_sleep_label, "%dm", remaining_min);
    quick_drawer_mark_snapshot_dirty();
}

static void quick_drawer_anim_y_cb(void * var, int32_t v) {
    (void) var;
    if (quick_drawer_direct_motion) {
        quick_drawer_direct_y = v;
        if (transition_compositor_vertical_overlay_frame(v)) return;

        /* A failed framebuffer present tears the compositor session down
         * itself. Continue the same gesture through the already-built LVGL
         * bitmap rather than dropping or snapping the drawer. */
        quick_drawer_direct_motion = false;
        lv_obj_set_y(quick_drawer_motion_image, v);
        lv_obj_add_flag(quick_drawer_motion_image, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(quick_drawer_motion_image, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(quick_drawer_motion_image);
        lv_obj_move_foreground(status_bar_band);
        return;
    }
    lv_obj_set_y(quick_drawer_bitmap_motion ? quick_drawer_motion_image : quick_drawer, v);
}

static int32_t quick_drawer_motion_y(void) {
    if (quick_drawer_direct_motion) return quick_drawer_direct_y;
    return lv_obj_get_y(quick_drawer_bitmap_motion ? quick_drawer_motion_image : quick_drawer);
}

static void quick_drawer_rebuild_snapshot(void) {
    if (!quick_drawer || quick_drawer_bitmap_motion) return;
    lv_draw_buf_t * fresh = lv_snapshot_take(quick_drawer, LV_COLOR_FORMAT_RGB565);
    if (!fresh) return;
    if (!quick_drawer_motion_image) {
        quick_drawer_motion_image = lv_image_create(lv_layer_top());
        lv_obj_add_flag(quick_drawer_motion_image, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_image_set_src(quick_drawer_motion_image, NULL);
    }
    if (quick_drawer_motion_buf) lv_draw_buf_destroy(quick_drawer_motion_buf);
    quick_drawer_motion_buf = fresh;
    lv_image_set_src(quick_drawer_motion_image, quick_drawer_motion_buf);
    quick_drawer_snapshot_dirty = false;
}

static void quick_drawer_snapshot_async_cb(void * unused) {
    (void) unused;
    if (quick_drawer_snapshot_dirty && !quick_drawer_bitmap_motion)
        quick_drawer_rebuild_snapshot();
}

void quick_drawer_mark_snapshot_dirty(void) {
    quick_drawer_snapshot_dirty = true;
    if (quick_drawer && !quick_drawer_bitmap_motion)
        lv_async_call(quick_drawer_snapshot_async_cb, NULL);
}

static bool quick_drawer_begin_bitmap_motion(void) {
    if (quick_drawer_bitmap_motion) return true;
    /* Never lv_snapshot_take() on the drag/animation tick: a full-panel
     * RGB565 snapshot is a multi-millisecond hitch on this SoC and was
     * the "dragging the drawer feels slow" report. Use a buffer already
     * built while idle, or follow the live panel. */
    if (quick_drawer_snapshot_dirty || !quick_drawer_motion_buf || !quick_drawer_motion_image)
        return false;
    int32_t initial_y = lv_obj_get_y(quick_drawer);
    quick_drawer_direct_y = initial_y;
    int32_t fixed_top = (status_bar_band && !lv_obj_has_flag(status_bar_band, LV_OBJ_FLAG_HIDDEN))
                        ? lv_obj_get_height(status_bar_band)
                        : 0;
    int32_t h = lv_display_get_vertical_resolution(lv_display_get_default());
    bool reuse_underlay = initial_y > -h;
#if defined(UI_PERF_TRACE) || defined(UI_GESTURE_TRACE)
    printf("[DRAWER_TRACE] begin_bitmap_motion: initial_y=%d, status_bar_band=%p, hidden=%d, fixed_top=%d, reuse_underlay=%d\n",
           (int)initial_y, (void*)status_bar_band,
           status_bar_band ? lv_obj_has_flag(status_bar_band, LV_OBJ_FLAG_HIDDEN) : -1,
           (int)fixed_top, (int)reuse_underlay);
#endif
    if (transition_compositor_begin_vertical_overlay(quick_drawer_motion_buf, fixed_top,
                                                     reuse_underlay)) {
        lv_obj_add_flag(quick_drawer, LV_OBJ_FLAG_HIDDEN);
        quick_drawer_bitmap_motion = true;
        quick_drawer_direct_motion = true;
        if (transition_compositor_vertical_overlay_frame(initial_y)) return true;
        /* The begin succeeded but the first present did not. Its failure
         * path has already restored LVGL; fall through to bitmap motion. */
        quick_drawer_direct_motion = false;
    }
    lv_obj_set_y(quick_drawer_motion_image, initial_y);
    lv_obj_add_flag(quick_drawer_motion_image, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(quick_drawer_motion_image, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(quick_drawer_motion_image);
    lv_obj_move_foreground(status_bar_band);
    lv_obj_add_flag(quick_drawer, LV_OBJ_FLAG_HIDDEN);
    quick_drawer_bitmap_motion = true;
    return true;
}

static void quick_drawer_finish_bitmap_motion(void) {
    int32_t h = lv_display_get_vertical_resolution(lv_display_get_default());
    if (quick_drawer_direct_motion) transition_compositor_end();
    quick_drawer_direct_motion = false;
    lv_obj_set_y(quick_drawer, quick_drawer_open ? 0 : -h);
    lv_obj_remove_flag(quick_drawer, LV_OBJ_FLAG_HIDDEN);
    if (quick_drawer_motion_image) {
        lv_obj_remove_flag(quick_drawer_motion_image, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(quick_drawer_motion_image, LV_OBJ_FLAG_HIDDEN);
    }
    quick_drawer_bitmap_motion = false;
    if (!quick_drawer_open) {
        transition_compositor_discard_vertical_base();
        /* Collapse here, off-screen, rather than on the next open: the
         * snapshot was last rebuilt while the drawer was still expanded, so
         * collapsing at open time would leave that stale expanded image to
         * animate in and visibly pop to collapsed once motion finished.
         * Doing it now lets the rebuild below produce a clean collapsed
         * snapshot while nothing is visible. */
        if (quick_drawer_expansion_y != 0) {
            quick_drawer_reset_expansion();
            quick_drawer_snapshot_dirty = true;
        }
        /* Park the marquees at offset 0 while off-screen so the snapshot
         * rebuilt below captures them unscrolled -- otherwise it freezes
         * whatever mid-scroll position they happened to be at, and the next
         * open animates that stale image before the live labels take over. */
        quick_drawer_restart_marquee();
    }
    if (quick_drawer_snapshot_dirty || quick_drawer_open)
        lv_async_call(quick_drawer_snapshot_async_cb, NULL);
}

static void quick_drawer_anim_done_cb(lv_anim_t * a) {
    (void) a;
    quick_drawer_finish_bitmap_motion();
}

void open_quick_drawer(void) {
    if (quick_drawer_open) return;
    quick_drawer_open = true;
    refresh_quick_drawer_brightness(); /* see its own comment -- keeps the slider from showing a stale pre-screen-off value */
    quick_drawer_refresh_volume();
    /* Always opens collapsed. The expanded rows' state is deliberately NOT
     * re-read here: refreshing them marks the snapshot dirty, which would
     * make quick_drawer_begin_bitmap_motion() below bail into live-tree
     * motion on every single open -- exactly the hitch its own comment
     * documents. They are invisible at this point anyway, so the read
     * happens when the expansion actually starts instead. */
    quick_drawer_reset_expansion();
    /* Both labels are already parked at offset 0 by the close path below,
     * so this adds no visible jump -- it just starts the 2s wait now, on
     * open, which is where the wait is meant to be measured from. */
    quick_drawer_restart_marquee();
    quick_drawer_begin_bitmap_motion();
    lv_obj_move_foreground(quick_drawer); /* above regular screens/volume popup while showing */
    /* ...but the status bar (clock/battery/wifi/bt) stays above THAT --
     * real-hardware feedback wanted it to stay visible/readable the whole
     * time the drawer is open, not get covered by it. quick_drawer's own
     * pull_down/bg.png is opaque black for the first ~59px anyway (measured
     * directly off the asset), so the status bar ends up sitting on that as
     * a backdrop rather than on anything from the screen underneath. */
    lv_obj_move_foreground(status_bar_band);
    /* Moving it forward is not enough on an immersive Player, where it is
     * flagged hidden; re-syncing now that quick_drawer_open is set reveals
     * it for as long as the drawer is down. */
    sync_player_topbar_visibility(lv_screen_active());
    /* Cancel any prior animation on this exact (var, exec_cb) pair before
     * starting a new one to prevent concurrent animations from fighting. */
    lv_anim_delete(quick_drawer, quick_drawer_anim_y_cb);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, quick_drawer);
    lv_anim_set_values(&a, quick_drawer_motion_y(), 0);
    lv_anim_set_duration(&a, QUICK_DRAWER_ANIM_MS);
    lv_anim_set_exec_cb(&a, quick_drawer_anim_y_cb);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&a, quick_drawer_anim_done_cb);
    lv_anim_start(&a);
}

void close_quick_drawer(void) {
    if (!quick_drawer_open) return;
    quick_drawer_open = false;
    /* Hidden again on the way out, so it leaves with the drawer rather than
     * popping out once the slide finishes. */
    sync_player_topbar_visibility(lv_screen_active());
    int32_t h = lv_display_get_vertical_resolution(lv_display_get_default());
    lv_anim_delete(quick_drawer, quick_drawer_anim_y_cb); /* see open_quick_drawer()'s own comment on why */
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, quick_drawer);
    quick_drawer_begin_bitmap_motion();
    lv_anim_set_values(&a, quick_drawer_motion_y(), -h);
    lv_anim_set_duration(&a, QUICK_DRAWER_ANIM_MS);
    lv_anim_set_exec_cb(&a, quick_drawer_anim_y_cb);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&a, quick_drawer_anim_done_cb);
    lv_anim_start(&a);
}


/* Handle stashed by gui_init() at creation time -- see poll_quick_drawer_
 * drag()'s own comment on why this timer runs at LV_DEF_REFR_PERIOD (~60fps)
 * instead of update_timer_cb's shared 500ms one. Paused by poll_quick_
 * drawer_drag() itself the instant nothing's pressed (so a ~60fps timer
 * doesn't sit registered forever, capping how long main()'s own idle
 * usleep() between lv_timer_handler() calls can ever be -- real cost even
 * though each individual idle tick barely does anything) and resumed by
 * resume_fast_gesture_timers_cb() (registered on the pointer indev, next to
 * this timer's own creation in gui_init()) the instant a new press begins
 * anywhere -- LV_EVENT_PRESSED is the one indev event LVGL dispatches
 * regardless of hit target (see poll_quick_drawer_drag()'s own doc comment
 * on why that's reliable here but LV_EVENT_PRESSING isn't). */
static lv_timer_t * quick_drawer_drag_timer = NULL;
static bool quick_drawer_drag_tracking = false;
static bool quick_drawer_drag_claimed = false;
static bool quick_drawer_was_pressed = false;
static bool drag_adjust_press_owned = false;
static int32_t quick_drawer_drag_touch_start_y = 0;
static int32_t quick_drawer_drag_panel_start_y = 0;
static int32_t quick_drawer_last_velocity = 0;
#define QUICK_DRAWER_DRAG_DEADZONE 10 /* matches LVGL's own LV_INDEV_DEF_SCROLL_LIMIT -- see poll_quick_drawer_drag()'s comment */

/* Swipe-up-to-Home tracking -- same live per-tick overlay as player_swipe_*
 * below, but vertical and sliding up from the bottom edge. Eligibility
 * (band/overlay/lyrics/lock-screen exclusions) is still gesture_detector.h's
 * gesture_home_state_is_eligible() -- gui_lock_screen.c's own independent
 * swipe-up-to-dismiss still uses gesture_home_state_poll()/state_t directly,
 * so that machinery stays in place; this only replaces how gui_shell.c
 * itself consumes eligibility, trading a fixed post-threshold instant cut
 * for the same candidate/tracking live-drag shape as the other two gestures. */
static bool home_swipe_candidate = false;
static bool home_swipe_tracking = false;
/* Real UI_PERF_TRACE data showed begin_slide_transition_ex() (17-34ms) and
 * the first compositor/overlay frame's own present (another ~16ms) both
 * landing in the SAME poll_quick_drawer_drag() tick as the deadzone
 * confirm -- a single tick blocking 33-50ms worst case, felt as a stall-
 * then-jump right when the gesture starts. Set true only at the instant
 * tracking begins; the tracking block below checks and clears it to skip
 * presenting a frame that same tick, deferring frame 0 to the next poll
 * tick instead. */
static bool home_swipe_just_confirmed = false;
static int32_t home_swipe_touch_start_x = 0;
static int32_t home_swipe_touch_start_y = 0;
static int32_t home_swipe_last_v = 0;
static int32_t home_swipe_last_velocity = 0;
static slide_transition_ctx_t * home_swipe_ctx = NULL;
#define HOME_SWIPE_DEADZONE 20 /* same scale/reasoning as PLAYER_SWIPE_DEADZONE/BACK_SWIPE_DEADZONE */

/* Swipe-left-to-player tracking -- same "raw indev polling, own dedicated
 * fast timer" reasoning as poll_quick_drawer_drag()'s own doc comment,
 * replacing the old LV_EVENT_GESTURE-based instant cut (see
 * screen_gesture_event_cb()'s own comment on why that couldn't just be
 * left running alongside this). Unlike the drawer's drag (claimed
 * instantly, by which zone the press started in) or the home-swipe
 * (claimed instantly, by starting inside a fixed band), this can start
 * ANYWHERE on screen -- matching the gesture it replaces -- so which
 * press this is can't be decided at press-down; it's provisional
 * (player_swipe_candidate) until enough movement accumulates to judge
 * direction, then either confirmed (player_swipe_tracking, the overlay
 * gets built and starts following the finger) or abandoned, letting the
 * press fall through as whatever else it actually was (a tap, a vertical
 * scroll, or a rightward back-swipe -- that one has its own live-tracking
 * state machine below. screen_gesture_event_cb()'s event-based right-swipe
 * remains the fallback for presses that state machine rejects). */
static bool player_swipe_candidate = false;
static bool player_swipe_tracking = false;
/* Same one-tick present deferral as home_swipe_just_confirmed above. */
static bool player_swipe_just_confirmed = false;
static int32_t player_swipe_touch_start_x = 0;
static int32_t player_swipe_touch_start_y = 0;
static int32_t player_swipe_last_v = 0; /* last sampled x (not necessarily presented -- see player_swipe_just_confirmed's deferred tick), for per-tick velocity -- same idea as quick_drawer_last_velocity */
static int32_t player_swipe_last_velocity = 0;
static slide_transition_ctx_t * player_swipe_ctx = NULL;
#define PLAYER_SWIPE_DEADZONE 20 /* px before judging direction -- comfortably under LVGL's own ~50px built-in gesture threshold (LV_INDEV_DEF_GESTURE_LIMIT) so this always claims a genuine left-swipe before LVGL's own dormant gesture recognition would have */

/* Swipe-right-to-go-back -- same live per-tick overlay as player_swipe_*
 * above, mirrored in sign. Provisional until BACK_SWIPE_DEADZONE so a
 * leftward player-swipe or a vertical scroll can still claim the press.
 * screen_gesture_event_cb()'s LV_DIR_RIGHT -> nav_pop() path stays as
 * the fallback for presses this candidate rejects (excluded screens,
 * dead zones, depth == 1). Unlike player-swipe's own left-swipe (which has
 * no competing consumer -- screen_gesture_event_cb only ever acts on
 * LV_DIR_RIGHT), this candidate genuinely races LVGL's own native gesture
 * recognition for the exact same direction: on real hardware, a fast swipe
 * can cross LVGL's own internal gesture threshold and dispatch
 * LV_EVENT_GESTURE before this poll-based BACK_SWIPE_DEADZONE confirms on
 * its own next tick, so wait_release() alone does not reliably win that
 * race (confirmed via on-device logging -- both fired for the same
 * continued drag, each independently acting on directory depth). back_swipe_
 * owns_press below is the actual mutual-exclusion mechanism: latched true
 * at press-down whenever this press is eligible at all (regardless of
 * whether the deadzone ever confirms a direction), and checked by
 * gui_shell_back_swipe_owns_press() so screen_gesture_event_cb can
 * unconditionally stand down for the whole press rather than trust timing. */
static bool back_swipe_candidate = false;
static bool back_swipe_owns_press = false;
static bool back_swipe_tracking = false;
/* Same one-tick present deferral as home_swipe_just_confirmed above. */
static bool back_swipe_just_confirmed = false;
static int32_t back_swipe_touch_start_x = 0;
static int32_t back_swipe_touch_start_y = 0;
static int32_t back_swipe_last_v = 0;
static int32_t back_swipe_last_velocity = 0;
static slide_transition_ctx_t * back_swipe_ctx = NULL;
static lv_obj_t * back_swipe_target_scr = NULL;
#define BACK_SWIPE_DEADZONE 20 /* same scale/reasoning as PLAYER_SWIPE_DEADZONE */

bool gui_shell_back_swipe_owns_press(void) {
    return back_swipe_owns_press;
}

/* Forward declarations -- both fully built later in this file, needed here
 * so poll_quick_drawer_drag() below can exclude the home-swipe gesture
 * while either DAC overlay is active (see its own comment on why). */

/* Drives the quick drawer's open/close by following the finger's raw Y position
 * every tick, snapping to fully open or closed when the finger lifts.
 *
 * Polled from its own dedicated ~60fps lv_timer (see gui_init()). Reading
 * raw indev coordinates directly avoids widget hit-test interception
 * (e.g. over the brightness slider), and the ~60fps polling rate provides
 * responsive tracking throughout quick swipes. */
/* Not just lv_indev_get_next(NULL) -- the target build only ever registers
 * the one touchscreen indev, but the host simulator also registers a
 * keyboard indev (see main.c's lv_sdl_keyboard_create()), and there's no
 * guarantee which one comes back first. Explicitly finding the
 * pointer-type one is correct on both. Shared by every raw-touch-polling
 * timer in this file (poll_quick_drawer_drag(), poll_az_index_drag()) --
 * see poll_quick_drawer_drag()'s own doc comment for why polling raw indev
 * state is necessary here at all instead of LVGL's own touch events. */
lv_indev_t * find_pointer_indev(void) {
    for (lv_indev_t * candidate = lv_indev_get_next(NULL); candidate; candidate = lv_indev_get_next(candidate)) {
        if (lv_indev_get_type(candidate) == LV_INDEV_TYPE_POINTER) return candidate;
    }
    return NULL;
}

static lv_indev_read_cb_t s_orig_pointer_read_cb = NULL;
static lv_indev_t * s_hooked_indev = NULL;
static lv_indev_state_t s_last_raw_pointer_state = LV_INDEV_STATE_RELEASED;
static bool s_require_release_after_wake = true;

static void wrapped_pointer_read_cb(lv_indev_t * indev, lv_indev_data_t * data) {
    if (s_orig_pointer_read_cb) {
        s_orig_pointer_read_cb(indev, data);
    }

    if (!backlight_screen_is_on()) {
        data->state = LV_INDEV_STATE_RELEASED;
        s_last_raw_pointer_state = LV_INDEV_STATE_RELEASED;
        s_require_release_after_wake = true;
        return;
    }

    if (s_require_release_after_wake) {
        if (data->state == LV_INDEV_STATE_PRESSED) {
            /* Finger was held down across the wake transition; suppress until released */
#ifdef UI_GESTURE_TRACE
            if (s_last_raw_pointer_state != LV_INDEV_STATE_PRESSED) {
                printf("[GESTURE_TRACE] raw pointer: suppressing held touch across wake at (%d, %d)\n",
                       (int)data->point.x, (int)data->point.y);
            }
#endif
            data->state = LV_INDEV_STATE_RELEASED;
            s_last_raw_pointer_state = LV_INDEV_STATE_RELEASED;
            return;
        } else {
            /* Physical release observed: establish clean baseline and arm subsequent presses */
            s_require_release_after_wake = false;
#ifdef UI_GESTURE_TRACE
            printf("[GESTURE_TRACE] raw pointer: release baseline established after wake\n");
#endif
        }
    }

    if (data->state == LV_INDEV_STATE_PRESSED &&
        s_last_raw_pointer_state != LV_INDEV_STATE_PRESSED) {
#ifdef UI_GESTURE_TRACE
        printf("[GESTURE_TRACE] raw pointer press edge detected at (%d, %d)\n",
               (int)data->point.x, (int)data->point.y);
#endif
        gui_shell_resume_fast_timers();
        gui_library_resume_fast_timers();
    }
    s_last_raw_pointer_state = data->state;
}

static void indev_pressed_event_cb(lv_event_t * e) {
    (void) e;
#ifdef UI_GESTURE_TRACE
    printf("[GESTURE_TRACE] indev LV_EVENT_PRESSED callback fired\n");
#endif
    gui_shell_resume_fast_timers();
    gui_library_resume_fast_timers();
}

void gui_shell_install_indev_hooks(lv_indev_t * indev) {
    if (!indev) indev = find_pointer_indev();
    if (!indev) {
#ifdef UI_GESTURE_TRACE
        printf("[GESTURE_TRACE] indev hook: pointer indev not found\n");
#endif
        return;
    }
    if (s_hooked_indev == indev) {
        return;
    }

    lv_indev_read_cb_t cur_read_cb = lv_indev_get_read_cb(indev);
    if (cur_read_cb && cur_read_cb != wrapped_pointer_read_cb) {
        s_orig_pointer_read_cb = cur_read_cb;
        lv_indev_set_read_cb(indev, wrapped_pointer_read_cb);
#ifdef UI_GESTURE_TRACE
        printf("[GESTURE_TRACE] indev hook: wrapped pointer read_cb for indev %p\n", (void*)indev);
#endif
    }
    lv_indev_add_event_cb(indev, indev_pressed_event_cb, LV_EVENT_PRESSED, NULL);
    s_hooked_indev = indev;
}

/* Checks whether the active pressed object or any of its parents is an
 * interactive drag-adjust widget (slider, switch, dropdown, roller) so that
 * horizontal drag adjustments are not intercepted by the swipe-to-player
 * detector. The press owner is latched to prevent fast drags from slipping
 * outside widget bounds. */
static bool active_object_is_drag_adjust_widget(void) {
    lv_obj_t * act = lv_indev_get_active_obj();
    while (act) {
        if (lv_obj_check_type(act, &lv_slider_class) ||
            lv_obj_check_type(act, &lv_switch_class) ||
            lv_obj_check_type(act, &lv_dropdown_class) ||
            lv_obj_check_type(act, &lv_roller_class)) {
            return true;
        }
        act = lv_obj_get_parent(act);
    }
    return false;
}

bool active_press_is_over_drag_adjust_widget(void) {
    return drag_adjust_press_owned || active_object_is_drag_adjust_widget();
}

/* Checks point coordinates against registered dead zones. Prevents presses
 * starting on non-clickable card containers near sliders from triggering
 * swipe transitions. Capacity accommodates native sliders and dynamic
 * plugin settings list sliders. */
#define SWIPE_DEAD_ZONE_MAX 16
static lv_obj_t * swipe_dead_zones[SWIPE_DEAD_ZONE_MAX];
static int swipe_dead_zone_count = 0;

void register_swipe_dead_zone(lv_obj_t * obj) {
    if (swipe_dead_zone_count < SWIPE_DEAD_ZONE_MAX) swipe_dead_zones[swipe_dead_zone_count++] = obj;
}

/* Compact-remove by pointer identity -- pairs with register_swipe_dead_zone()
 * above for objects that DON'T live forever (unlike every native slider
 * card, which registers once at startup and never needs to unregister). A
 * plugin.show_settings_list() pool slot's slider cards are deleted and
 * recreated on every call that reuses that slot (lv_obj_clean(), see
 * gui_plugin_show_settings_list()) -- calling this for each of a slot's own
 * previously-registered cards BEFORE that lv_obj_clean() runs is required,
 * not just tidy: point_in_swipe_dead_zone()'s own lv_obj_get_screen(obj) !=
 * lv_screen_active() guard still needs `obj` to be a live pointer to
 * dereference, so leaving a freed card's pointer in this array would be a
 * use-after-free on the next swipe check, not a graceful skip. No-op if obj
 * isn't currently registered. */
void unregister_swipe_dead_zone(lv_obj_t * obj) {
    for (int i = 0; i < swipe_dead_zone_count; i++) {
        if (swipe_dead_zones[i] == obj) {
            swipe_dead_zones[i] = swipe_dead_zones[swipe_dead_zone_count - 1];
            swipe_dead_zone_count--;
            return;
        }
    }
}

/* For gui_reload.c's in-process UI reload -- every native slider card
 * registers itself here once at startup and, per register_swipe_dead_zone()'s
 * own comment, is never expected to unregister because it "lives forever."
 * A reload breaks that assumption: it deletes every native slider and
 * builds fresh ones, but without this, the OLD (now-freed) pointers stay in
 * the array forever -- point_in_swipe_dead_zone() would dereference freed
 * memory on the very next swipe check, and every reload would also append
 * the NEW cards on top without ever clearing the old slots, filling
 * SWIPE_DEAD_ZONE_MAX permanently after just a few reloads. Only clears the
 * array (these are borrowed pointers, not owned -- nothing here to free);
 * every screen rebuilt after this call re-registers its own sliders fresh. */
void reset_swipe_dead_zones(void) {
    swipe_dead_zone_count = 0;
}

bool point_in_swipe_dead_zone(lv_point_t p) {
    for (int i = 0; i < swipe_dead_zone_count; i++) {
        lv_obj_t * obj = swipe_dead_zones[i];
        if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) continue;
        if (lv_obj_get_screen(obj) != lv_screen_active()) continue;
        lv_area_t area;
        /* lv_obj_get_click_area(), not lv_obj_get_coords() -- a native
         * slider's raw box can be a few px tall (e.g. the player's progress
         * bar) while lv_obj_set_ext_click_area() widens what LVGL itself
         * treats as a touch on it. Using the raw box here left a real,
         * visually-on-the-slider press just outside the registered dead
         * zone free to start a back-swipe instead. Falls back to the raw
         * box automatically when an object has no ext click area set. */
        lv_obj_get_click_area(obj, &area);
        if (p.x >= area.x1 && p.x <= area.x2 && p.y >= area.y1 && p.y <= area.y2) return true;
    }
    return false;
}

static bool player_swipe_press_excluded(lv_point_t p) {
    return active_press_is_over_drag_adjust_widget() || point_in_swipe_dead_zone(p);
}

static bool quick_drawer_brightness_hit_test(lv_point_t point) {
    if (!quick_drawer_open || !quick_drawer_brightness_track) return false;
    lv_area_t area;
    lv_obj_get_coords(quick_drawer_brightness_track, &area);
    lv_area_increase(&area, BOARD_SCALE_PX(44), BOARD_SCALE_PX(44)); /* matches build_quick_drawer()'s hit area */
    return point.x >= area.x1 && point.x <= area.x2 &&
           point.y >= area.y1 && point.y <= area.y2;
}

/* quick_drawer_brightness_hit_test()'s own twin for the volume slider --
 * this one was missing entirely, which was the real cause of "dragging the
 * volume slider sometimes stops tracking / triggers a swipe": without it,
 * drag_adjust_press_owned below fell back to active_object_is_drag_adjust_
 * widget() alone, which reads lv_indev_get_active_obj() -- state LVGL's own
 * indev processing updates independently of this timer's own 16ms poll, so
 * on some press-downs (this timer racing ahead of that update, more likely
 * under a fast repeated drag) it read stale state and saw no slider owning
 * the press. Ownership then fell through to quick_drawer_drag_tracking /
 * the swipe detectors instead, at which point the slider stopped
 * following the finger and the drawer itself (or a swipe-up) took over.
 * A direct, coordinate-based check has no such race. */
static bool quick_drawer_volume_hit_test(lv_point_t point) {
    if (!quick_drawer_open || !quick_drawer_volume_track) return false;
    lv_area_t area;
    lv_obj_get_coords(quick_drawer_volume_track, &area);
    lv_area_increase(&area, BOARD_SCALE_PX(44), BOARD_SCALE_PX(44)); /* matches its own ext_click_area */
    return point.x >= area.x1 && point.x <= area.x2 &&
           point.y >= area.y1 && point.y <= area.y2;
}

/* The expansion drag owns everything above the now-playing card, not just
 * the handle, so the target is the whole toggle area rather than an 8px
 * pill. Must be in the same press-down ownership chain: without it a
 * downward drag here reads as an ordinary drawer drag and closes the
 * drawer instead of expanding it -- the exact failure the volume rail
 * already hit once. The two rails are excluded because their own hit areas
 * overlap this region and a press inside them is a slider drag; dragging
 * from the card downwards still closes the whole drawer. */
static bool quick_drawer_expansion_region_hit_test(lv_point_t point) {
    if (!quick_drawer_open || !quick_drawer_expansion_handle) return false;
    if (quick_drawer_expansion_full <= 0) return false;
    if (quick_drawer_brightness_hit_test(point)) return false;
    if (quick_drawer_volume_hit_test(point)) return false;
    lv_area_t handle;
    lv_obj_get_coords(quick_drawer_expansion_handle, &handle);
    return point.y <= handle.y2;
}


static bool quick_drawer_expansion_dragging = false;
static bool quick_drawer_expansion_moved = false;
static int32_t quick_drawer_expansion_drag_start_y = 0;
static int32_t quick_drawer_expansion_drag_start_value = 0;

static void poll_quick_drawer_drag(lv_timer_t * timer) {
    lv_indev_t * indev = find_pointer_indev();
    if (!indev) return;

    bool pressed = lv_indev_get_state(indev) == LV_INDEV_STATE_PRESSED;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    int32_t h = lv_display_get_vertical_resolution(lv_display_get_default());

    if (pressed && !quick_drawer_was_pressed) {
        /* Gesture ownership is decided once at press-down. A fast slider
         * drag may leave its bounds, but it remains a slider drag until lift. */
        quick_drawer_expansion_dragging = quick_drawer_expansion_region_hit_test(p);
        quick_drawer_expansion_moved = false;
        if (quick_drawer_expansion_dragging) {
            lv_anim_delete(quick_drawer_expansion_box, quick_drawer_expansion_anim_cb);
            quick_drawer_expansion_drag_start_y = p.y;
            quick_drawer_expansion_drag_start_value = quick_drawer_expansion_y;
            /* Same read-on-the-way-open as the animated path, for a drag
             * that reveals the rows without going through it. */
            if (quick_drawer_expansion_y == 0) refresh_quick_drawer_expansion_toggles();
        }

        drag_adjust_press_owned = active_object_is_drag_adjust_widget() ||
                                  point_in_swipe_dead_zone(p) ||
                                  quick_drawer_brightness_hit_test(p) ||
                                  quick_drawer_volume_hit_test(p) ||
                                  quick_drawer_expansion_dragging ||
                                  gui_player_volume_control_hit_test(p);
    }

    if (pressed && !quick_drawer_was_pressed) {
        /* Cancel any release-snap animation still in flight to prevent it
         * from fighting a newly started drag. */
        lv_anim_delete(quick_drawer, quick_drawer_anim_y_cb);

        /* Use the drawer's current Y so interrupted animations continue
         * naturally. Adjustment widgets keep ownership of their drags. */
        if (drag_adjust_press_owned) {
            quick_drawer_drag_tracking = false;
        } else if (quick_drawer_open) {
            quick_drawer_drag_tracking = true;
            quick_drawer_drag_panel_start_y = quick_drawer_motion_y();
            quick_drawer_last_velocity = 0; /* fresh drag, no leftover direction from a previous completed one -- same reset as home/player/back-swipe's own tracking-start */
        } else if (p.y <= QUICK_DRAWER_TRIGGER_ZONE && !gui_library_navigation_blocked() &&
                   lv_screen_active() != gui_lock_screen_get_screen()) {
            /* gui_library_navigation_blocked() only covers modal library
             * operations that own the active screen. Optional artwork/search
             * workers must not disable normal navigation. This only blocks a NEW open-drag;
             * if the drawer somehow got dragged open right as a rescan
             * started, the quick_drawer_open branch above still lets it be
             * dragged closed again. */
            quick_drawer_drag_tracking = true;
            quick_drawer_drag_panel_start_y = quick_drawer_motion_y();
            quick_drawer_last_velocity = 0; /* fresh drag -- see the other branch's own comment */
            lv_obj_move_foreground(quick_drawer); /* above regular screens/volume popup while dragging into view */
            lv_obj_move_foreground(status_bar_band); /* but the status bar stays above THAT -- see open_quick_drawer()'s comment */
        } else {
            quick_drawer_drag_tracking = false;
        }
        quick_drawer_drag_claimed = false;
        quick_drawer_drag_touch_start_y = p.y;

        gesture_home_config_t home_cfg;
        home_cfg.swipe_up_home_enabled = current_settings.swipe_up_home_enabled;
        home_cfg.quick_drawer_open = quick_drawer_open;
        home_cfg.is_bt_dac_overlay = (lv_screen_active() == gui_network_get_bt_dac_overlay());
        home_cfg.is_usb_dac_overlay = (lv_screen_active() == gui_network_get_usb_dac_overlay());
        home_cfg.is_lyrics_screen = (lv_screen_active() == gui_lyrics_get_screen());
        home_cfg.is_lock_screen = (lv_screen_active() == gui_lock_screen_get_screen());
        home_cfg.has_background_work = gui_library_navigation_blocked();
        home_cfg.screen_height = h;
        /* Slightly expand only the raw press-down target. The overlay band and
         * its visible pill retain their existing dimensions. */
        home_cfg.band_height = HOME_INDICATOR_BAND_HEIGHT + HOME_SWIPE_HIT_EXTRA_PX;

        /* gesture_home_config_t's fields are shared with gui_lock_screen.c's
         * own independent use of gesture_home_state_is_eligible(), which
         * doesn't need these -- same reasoning as back-swipe's own
         * exclusions just above: text-entry, Import via Wi-Fi, and the busy
         * overlay all skip finalize_screen_navigation() because leaving
         * them needs teardown (in-progress input, import_web_stop(),
         * modal ownership) that a stack-only reset to Home would bypass.
         *
         * Deliberately NOT gated on drag_adjust_press_owned (unlike player-
         * swipe/back-swipe below): home_indicator_band already sits on
         * lv_layer_top(), CLICKABLE, sized to this exact band, so nothing on
         * the active screen can genuinely receive a press inside this strip.
         * point_in_swipe_dead_zone() is purely coordinate-based with no
         * z-order awareness, so it was vetoing this gesture whenever some
         * OTHER, actually-covered widget's registered dead zone happened to
         * overlap this band's coordinates -- e.g. a bottom-aligned EQ screen
         * or a plugin slider card extending into the last ~31px of the
         * screen. Keep this ungated so the indicator band's own hitbox keeps
         * real priority over whatever is positioned underneath it. */
        home_swipe_candidate = gesture_home_state_is_eligible(&home_cfg, p.y) &&
                                lv_screen_active() != gui_text_input_get_screen() &&
                                lv_screen_active() != gui_network_get_import_wifi_screen() &&
                                lv_screen_active() != gui_busy_get_screen();
        home_swipe_touch_start_x = p.x;
        home_swipe_touch_start_y = p.y;
        home_swipe_tracking = false;

#ifdef UI_GESTURE_TRACE
        printf("[GESTURE_TRACE] poll: press-down at (%d, %d), res_h=%d\n", (int)p.x, (int)p.y, (int)h);
        printf("[GESTURE_TRACE] poll: home_swipe eval: enabled=%d, tracking=%d\n",
               current_settings.swipe_up_home_enabled, home_swipe_candidate);
#endif

        /* Player-swipe: eligible unless claimed by the drawer drag, the drawer
         * is open, the player screen is already active, or the press started on
         * a drag-adjust widget/dead-zone. Excluded on lyrics, track info,
         * lock screen, and while library navigation is blocked. Candidate only
         * until sufficient displacement accumulates to determine gesture direction. */
        player_swipe_candidate = !quick_drawer_drag_tracking && !quick_drawer_open &&
                                  lv_screen_active() != gui_player_get_screen() &&
                                  lv_screen_active() != gui_lyrics_get_screen() &&
                                  lv_screen_active() != gui_track_info_get_screen() &&
                                  lv_screen_active() != gui_lock_screen_get_screen() &&
                                  !gui_library_navigation_blocked() &&
                                  !player_swipe_press_excluded(p);
        player_swipe_touch_start_x = p.x;
        player_swipe_touch_start_y = p.y;
        player_swipe_tracking = false;

        /* Depth > 1 is the stack-pop precondition. Lyrics owns its own
         * right-swipe (lyrics_gesture_event_cb -> close_lyrics_screen).
         * Lock, text-entry, both DAC overlays, Import via Wi-Fi, and the
         * busy overlay skip finalize_screen_navigation() -- a live pop
         * here would bypass their leave-confirmation, teardown, or
         * modal-ownership. Same drawer/dead-zone/drag-adjust exclusions
         * as player-swipe: a slider drag must never become a back-swipe.
         * Lyrics is NOT excluded -- it now uses this same live-tracking
         * swipe-back (reveal-style, exiting to Player) instead of its own
         * lyrics_gesture_event_cb()'s plain nav_pop(); that handler still
         * exists for the auto-close-on-track-change-with-no-lyrics path,
         * but stands down for a user swipe once this candidate owns the
         * press (see gui_shell_back_swipe_owns_press()). */
        back_swipe_candidate = !quick_drawer_drag_tracking && !quick_drawer_open &&
                                gui_navigation_get_depth() > 1 &&
                                lv_screen_active() != gui_lock_screen_get_screen() &&
                                lv_screen_active() != gui_text_input_get_screen() &&
                                lv_screen_active() != gui_network_get_usb_dac_overlay() &&
                                lv_screen_active() != gui_network_get_bt_dac_overlay() &&
                                lv_screen_active() != gui_network_get_import_wifi_screen() &&
                                lv_screen_active() != gui_busy_get_screen() &&
                                !gui_library_navigation_blocked() &&
                                !player_swipe_press_excluded(p);
        back_swipe_touch_start_x = p.x;
        back_swipe_touch_start_y = p.y;
        back_swipe_tracking = false;
        back_swipe_owns_press = back_swipe_candidate;
        DB_LOG("GESTURE", "back_swipe_candidate=%d screen=%s depth=%d",
               back_swipe_candidate,
               lv_screen_active() == gui_library_get_files_screen() ? "files" : "other",
               gui_navigation_get_depth());
    }

    if (pressed && home_swipe_candidate && !home_swipe_tracking && !player_swipe_tracking && !back_swipe_tracking) {
        int32_t dx = p.x - home_swipe_touch_start_x;
        int32_t dy = p.y - home_swipe_touch_start_y;
        int32_t adx = dx < 0 ? -dx : dx;
        int32_t ady = dy < 0 ? -dy : dy;
        if (adx >= HOME_SWIPE_DEADZONE || ady >= HOME_SWIPE_DEADZONE) {
            if (dy < 0 && ady > adx) {
                /* EXPERIMENTAL reveal=true (see gui_navigation.h's own
                 * comment on slide_transition_ctx_t's reveal field): Home
                 * stays static, uncovered as the current screen slides up
                 * over it, instead of both panels moving together. A/B
                 * test against the two-panel style back-swipe/player-swipe
                 * still use -- not yet settled as the final behavior. */
                home_swipe_ctx = begin_slide_transition_ex(gui_shell_get_home_screen(), true, true, true);
                if (home_swipe_ctx) {
                    /* No navigation decision exists until release. A
                     * compositor failure during the live drag therefore
                     * recovers to from_scr and leaves the stack untouched. */
                    home_swipe_ctx->commit = false;
                    home_swipe_tracking = true;
                    home_swipe_just_confirmed = true;
                    home_swipe_last_v = 0;
                    home_swipe_last_velocity = 0;
#ifdef UI_GESTURE_TRACE
                    printf("[GESTURE_TRACE] poll: home_swipe TRIGGERED (start_y=%d, cur_y=%d)\n",
                           (int)home_swipe_touch_start_y, (int)p.y);
#endif
                    lv_indev_wait_release(indev);
                }
            } else if (adx > ady && !lv_indev_get_scroll_obj(indev)) {
                /* Same non-scrollable drag tap suppression as player-swipe/
                 * back-swipe, mirrored for a horizontal drag ruling out a
                 * vertical home-swipe. */
                lv_indev_wait_release(indev);
            }
            home_swipe_candidate = false;
        }
    }

    if (pressed && player_swipe_candidate && !player_swipe_tracking) {
        int32_t dx = p.x - player_swipe_touch_start_x;
        int32_t dy = p.y - player_swipe_touch_start_y;
        int32_t adx = dx < 0 ? -dx : dx;
        int32_t ady = dy < 0 ? -dy : dy;
        if (adx >= PLAYER_SWIPE_DEADZONE || ady >= PLAYER_SWIPE_DEADZONE) {
            /* Enough movement to judge direction. Horizontal-left-dominant
             * confirms it; anything else (vertical, or rightward) rules it
             * out for good -- either way, stop re-checking every tick. */
            if (dx < 0 && adx > ady) {
                player_swipe_ctx = begin_slide_transition(gui_player_get_screen(), true); /* see begin_slide_transition()'s own comment -- both sources are always owned copies now */
                if (player_swipe_ctx) {
                    /* No navigation decision exists until release. A
                     * compositor failure during the live drag therefore
                     * recovers to from_scr and leaves the stack untouched. */
                    player_swipe_ctx->commit = false;
                    player_swipe_tracking = true;
                    player_swipe_just_confirmed = true;
                    player_swipe_last_v = 0;
                    player_swipe_last_velocity = 0;
                    /* Same reasoning as nav_pop()'s own lv_indev_wait_release()
                     * call -- the overlay just created sits directly under
                     * this still-down finger, and without this, the eventual
                     * release would hit whatever's now underneath at that
                     * coordinate instead (a real screen swap mid-press, same
                     * PRESS_LOST-adjacent class of bug already found and
                     * fixed once for the drawer's own icons). */
                    lv_indev_wait_release(indev);
                }
            } else if (ady > adx && !lv_indev_get_scroll_obj(indev)) {
                /* If vertical drag exceeds deadzone on a non-scrollable screen,
                 * suppress the pending tap via lv_indev_wait_release() to prevent
                 * accidental row activation when an attempted scroll cannot occur. */
                lv_indev_wait_release(indev);
            }
            player_swipe_candidate = false;
        }
    }

    if (pressed && back_swipe_candidate && !back_swipe_tracking && !player_swipe_tracking) {
        int32_t dx = p.x - back_swipe_touch_start_x;
        int32_t dy = p.y - back_swipe_touch_start_y;
        int32_t adx = dx < 0 ? -dx : dx;
        int32_t ady = dy < 0 ? -dy : dy;
        if (adx >= BACK_SWIPE_DEADZONE || ady >= BACK_SWIPE_DEADZONE) {
            if (dx > 0 && adx > ady) {
                lv_obj_t * active = lv_screen_active();
                /* Search-open and Files-not-at-root consume a right-swipe
                 * as in-screen back (close the bar / step up a directory)
                 * rather than a stack pop. Dispatch those here so this
                 * path cannot steal them into a slide; wait_release so
                 * the still-down finger cannot also fire
                 * screen_gesture_event_cb's leftover fallback (which
                 * would then nav_pop after search already closed). */
                bool consumed_in_place = search_close_if_active_for_screen(active) ||
                                         file_browser_back_if_not_root_for_screen(active);
                DB_LOG("GESTURE", "back_swipe confirm screen=%s in_place=%d",
                       active == gui_library_get_files_screen() ? "files" : "other", consumed_in_place);
                if (consumed_in_place) {
                    lv_indev_wait_release(indev);
                } else {
                    back_swipe_target_scr = gui_navigation_get_screen_at(gui_navigation_get_depth() - 2);
                    if (back_swipe_target_scr) {
                        /* Reveal-style everywhere except leaving the Player
                         * screen itself, which keeps the original two-panel
                         * slide -- see ISSUES.md's swipe-up-to-Home to-do
                         * entry for the same reveal field, still an active
                         * A/B test rather than settled for every gesture. */
                        bool reveal = active != gui_player_get_screen();
                        back_swipe_ctx = begin_slide_transition_ex(back_swipe_target_scr, false, false, reveal);
                        DB_LOG("GESTURE", "back_swipe slide target=%p ctx=%p",
                               (void *) back_swipe_target_scr, (void *) back_swipe_ctx);
                        if (back_swipe_ctx) {
                            /* No navigation decision exists until release.
                             * A compositor failure during the live drag
                             * therefore recovers to from_scr and leaves
                             * the stack untouched. */
                            back_swipe_ctx->commit = false;
                            back_swipe_tracking = true;
                            back_swipe_just_confirmed = true;
                            back_swipe_last_v = 0;
                            back_swipe_last_velocity = 0;
                            lv_indev_wait_release(indev);
                        }
                    }
                }
            } else if (ady > adx && !lv_indev_get_scroll_obj(indev)) {
                /* Same non-scrollable vertical-drag tap suppression as
                 * player-swipe. Harmless if that path already called
                 * wait_release on this press. */
                lv_indev_wait_release(indev);
            }
            back_swipe_candidate = false;
        }
    }

    if (pressed && home_swipe_tracking) {
        int32_t v = p.y - home_swipe_touch_start_y;
        if (v > 0) v = 0;  /* never past fully-closed (finger drifting back down just holds at 0) */
        if (v < -h) v = -h; /* never past fully-off (finger overshooting up of a full screen height) */
        /* Sticky velocity: only overwrite on an actual per-tick move. A real
         * finger decelerates to a near-zero last-tick delta right before it
         * lifts (that's how a deliberate slow drag naturally ends), so
         * clobbering the last direction with that final stall/zero tick would
         * make the halfway-position fallback fire on most slow drags, not
         * just genuinely-still releases. Holding the last nonzero delta
         * across a zero-delta tick is what makes "continue in the last
         * registered direction" mean the last real movement, not the last
         * sample. */
        int32_t home_swipe_delta = v - home_swipe_last_v;
        if (home_swipe_delta != 0) home_swipe_last_velocity = home_swipe_delta;
        home_swipe_last_v = v;
        /* last_v always stays current every tick, and last_velocity holds
         * the last real (nonzero) direction (a release landing on this
         * exact tick must still see the accurate last direction/halfway
         * state) --
         * only the frame PRESENT is skipped, on the same tick begin_slide_
         * transition_ex() ran on. See home_swipe_just_confirmed's own
         * comment at its declaration. */
        if (home_swipe_just_confirmed) {
            home_swipe_just_confirmed = false;
        } else {
            slide_transition_anim_x_cb(home_swipe_ctx, v);
        }
    }

    if (pressed && player_swipe_tracking) {
        int32_t w = lv_display_get_horizontal_resolution(lv_display_get_default());
        int32_t v = p.x - player_swipe_touch_start_x;
        if (v > 0) v = 0;   /* never past fully-open (finger drifting back right of the start point just holds at 0) */
        if (v < -w) v = -w; /* never past fully-off (finger overshooting left of a full screen width) */
        /* Sticky velocity -- see home_swipe's own comment on this same
         * pattern just above. */
        int32_t player_swipe_delta = v - player_swipe_last_v;
        if (player_swipe_delta != 0) player_swipe_last_velocity = player_swipe_delta;
        player_swipe_last_v = v;
        if (player_swipe_just_confirmed) {
            player_swipe_just_confirmed = false;
        } else {
            slide_transition_anim_x_cb(player_swipe_ctx, v);
        }
    }

    if (pressed && back_swipe_tracking) {
        int32_t w = lv_display_get_horizontal_resolution(lv_display_get_default());
        int32_t v = p.x - back_swipe_touch_start_x;
        if (v < 0) v = 0;  /* never past fully-closed (finger drifting back left of the start point just holds at 0) */
        if (v > w) v = w;  /* never past fully-off (finger overshooting right of a full screen width) */
        /* Sticky velocity -- see home_swipe's own comment on this same
         * pattern just above. */
        int32_t back_swipe_delta = v - back_swipe_last_v;
        if (back_swipe_delta != 0) back_swipe_last_velocity = back_swipe_delta;
        back_swipe_last_v = v;
        if (back_swipe_just_confirmed) {
            back_swipe_just_confirmed = false;
        } else {
            slide_transition_anim_x_cb(back_swipe_ctx, v);
        }
    }

    /* Expansion drag: tracks the finger 1:1 between collapsed and fully
     * open. Runs before the drawer's own drag block below, which cannot be
     * active at the same time -- drag_adjust_press_owned took this press. */
    if (pressed && quick_drawer_expansion_dragging) {
        int32_t raw_delta = p.y - quick_drawer_expansion_drag_start_y;
        if (!quick_drawer_expansion_moved &&
            (raw_delta > QUICK_DRAWER_DRAG_DEADZONE || raw_delta < -QUICK_DRAWER_DRAG_DEADZONE)) {
            /* Past the deadzone this is a drag, not a tap on the handle --
             * suppress the release so quick_drawer_expansion_handle_cb()
             * cannot also fire and immediately undo the snap below. */
            quick_drawer_expansion_moved = true;
            lv_indev_wait_release(indev);
        }
        if (quick_drawer_expansion_moved) {
            int32_t adjusted = raw_delta > 0 ? raw_delta - QUICK_DRAWER_DRAG_DEADZONE
                                             : raw_delta + QUICK_DRAWER_DRAG_DEADZONE;
            quick_drawer_apply_expansion(quick_drawer_expansion_drag_start_value + adjusted);
        }
    }

    if (!pressed && quick_drawer_was_pressed && quick_drawer_expansion_dragging) {
        quick_drawer_expansion_dragging = false;
        if (quick_drawer_expansion_moved) {
            if (quick_drawer_expansion_drag_start_value == 0 &&
                p.y < quick_drawer_expansion_drag_start_y) {
                /* Nothing was expanded to collapse, so an upward drag here is
                 * the drawer's own close gesture rather than a row drag. */
                close_quick_drawer();
            } else {
                /* Snap to whichever end the drag ended nearer. A tap that never
                 * left the deadzone falls through to the handle's own CLICKED
                 * handler instead. */
                quick_drawer_animate_expansion(quick_drawer_expansion_y * 2 >= quick_drawer_expansion_full);
            }
            quick_drawer_expansion_moved = false;
        }
    }

    if (pressed && quick_drawer_drag_tracking) {
        /* Deadzone before moving panel: suppresses minor jitter during taps
         * and long-presses so child widgets (icons) do not receive PRESS_LOST.
         * Beyond QUICK_DRAWER_DRAG_DEADZONE (10px), the deadzone is subtracted
         * so drag motion starts smoothly from zero. */
        int32_t raw_delta = p.y - quick_drawer_drag_touch_start_y;
        if (raw_delta > QUICK_DRAWER_DRAG_DEADZONE || raw_delta < -QUICK_DRAWER_DRAG_DEADZONE) {
            int32_t adjusted_delta = raw_delta > 0 ? raw_delta - QUICK_DRAWER_DRAG_DEADZONE
                                                    : raw_delta + QUICK_DRAWER_DRAG_DEADZONE;
            int32_t new_y = quick_drawer_drag_panel_start_y + adjusted_delta;
            if (new_y > 0) new_y = 0;
            if (new_y < -h) new_y = -h;
            /* Past the deadzone this is a drag, not a tap. The live drawer
             * does not cover the list while opening (it starts off-screen),
             * and bitmap motion hides the real panel behind a snapshot --
             * without wait_release(), LVGL re-hit-tests the still-down
             * finger onto whatever row is now underneath and fires CLICKED
             * on release. Same tool as the player-swipe path above. */
            if (!quick_drawer_drag_claimed) {
                quick_drawer_drag_claimed = true;
                lv_indev_wait_release(indev);
            }
            /* Per-tick velocity, in case the finger lifts mid-flick (see the
             * release branch below) -- a plain position delta rather than
             * lv_indev_get_vect() so it's driven by the exact same samples
             * this function already reads, not a second/possibly-
             * differently-timed source. */
            if (!quick_drawer_bitmap_motion) quick_drawer_begin_bitmap_motion();
            /* Sticky velocity -- see home_swipe's own comment on this same
             * pattern above. A zero-delta tick (finger momentarily still, or
             * settled back within the deadzone below) must not clobber the
             * last real direction, since a deliberate slow drag naturally
             * decelerates to near-zero right before it lifts. */
            int32_t quick_drawer_delta = new_y - quick_drawer_motion_y();
            if (quick_drawer_delta != 0) quick_drawer_last_velocity = quick_drawer_delta;
            quick_drawer_anim_y_cb(quick_drawer, new_y);
        }
    }

    if (!pressed && quick_drawer_was_pressed && quick_drawer_drag_tracking) {
        quick_drawer_drag_tracking = false;
        /* Unclaimed taps need no settle animation. An interrupted bitmap
         * motion still needs settling even if this press never became a drag. */
        if (quick_drawer_drag_claimed || quick_drawer_bitmap_motion) {
            /* Settle decision on release: any registered direction at the exact
             * moment of release wins, however slow; only a perfectly still
             * release (zero exit velocity) falls back to whichever side of the
             * halfway mark the panel was on. */
            bool snap_open;
            if (quick_drawer_last_velocity > 0) {
                snap_open = true; /* still moving down (toward open) at release, however slowly */
            } else if (quick_drawer_last_velocity < 0) {
                snap_open = false; /* still moving up (toward closed) at release, however slowly */
            } else {
                snap_open = quick_drawer_motion_y() > -h / 2; /* perfectly still at release -- fall back to position */
            }
            /* open_quick_drawer()/close_quick_drawer() animate from the
             * drawer's CURRENT (mid-drag) position, so forcing quick_drawer_open
             * to the opposite state first just defeats their own early-return
             * guard rather than fighting the animation. */
            if (snap_open) {
                quick_drawer_open = false;
                open_quick_drawer();
            } else {
                quick_drawer_open = true;
                close_quick_drawer();
            }
        }
    }

    if (!pressed && quick_drawer_was_pressed && home_swipe_tracking) {
        home_swipe_tracking = false;
        int32_t current_v = home_swipe_last_v;
        bool commit;
        if (home_swipe_last_velocity < 0) {
            commit = true; /* still moving up (toward Home) at release, however slowly */
        } else if (home_swipe_last_velocity > 0) {
            commit = false; /* still moving back down (toward cancel) at release, however slowly */
        } else {
            commit = current_v < -h / 2; /* perfectly still at release -- fall back to position */
        }
        home_swipe_ctx->commit = commit;
        if (commit) {
            /* Stack-only bookkeeping -- actual lv_screen_load() waits for
             * slide_transition_done_cb(). Cancel leaves the stack
             * untouched, matching player-swipe/back-swipe's own cancel path. */
            nav_reset_to_home_stack_only();
        }
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, home_swipe_ctx);
        lv_anim_set_user_data(&a, home_swipe_ctx);
        lv_anim_set_values(&a, current_v, commit ? -h : 0);
        lv_anim_set_duration(&a, QUICK_DRAWER_ANIM_MS);
        lv_anim_set_exec_cb(&a, slide_transition_anim_x_cb);
        lv_anim_set_completed_cb(&a, slide_transition_done_cb);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_start(&a);
        home_swipe_ctx = NULL;
    }

    if (!pressed && quick_drawer_was_pressed && player_swipe_tracking) {
        /* Finger lifted mid-swipe. Same direction-vs-halfway decision as the
         * drawer's own release logic just above, just horizontal. */
        player_swipe_tracking = false;
        int32_t w = lv_display_get_horizontal_resolution(lv_display_get_default());
        /* player_swipe_last_v stores the last sampled offset, updated every
         * tracking tick regardless of whether that tick actually presented
         * a frame (see player_swipe_just_confirmed). Reads it directly
         * rather than inspecting img_from, which is NULL when direct-
         * framebuffer compositing is active. */
        int32_t current_v = player_swipe_last_v;
        bool commit;
        if (player_swipe_last_velocity < 0) {
            commit = true; /* still moving left (toward Player) at release, however slowly */
        } else if (player_swipe_last_velocity > 0) {
            commit = false; /* still moving back right (toward cancel) at release, however slowly */
        } else {
            commit = current_v < -w / 2; /* perfectly still at release -- fall back to position */
        }
        player_swipe_ctx->commit = commit;
        if (commit) {
            /* Stack-only bookkeeping -- actual lv_screen_load() is deferred
             * until slide_transition_done_cb() runs when the settle animation
             * completes. nav_push_stack_only() updates the nav stack without
             * prematurely triggering screen load events. */
            if (!gui_navigation_is_top(gui_player_get_screen())) {
                nav_push_stack_only(gui_player_get_screen());
            }
        }
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, player_swipe_ctx);
        lv_anim_set_user_data(&a, player_swipe_ctx);
        lv_anim_set_values(&a, current_v, commit ? -w : 0);
        lv_anim_set_duration(&a, QUICK_DRAWER_ANIM_MS); /* short settle, same duration class as the drawer's own release-snap */
        lv_anim_set_exec_cb(&a, slide_transition_anim_x_cb);
        lv_anim_set_completed_cb(&a, slide_transition_done_cb);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_start(&a);
        player_swipe_ctx = NULL;
    }

    if (!pressed && quick_drawer_was_pressed && back_swipe_tracking) {
        back_swipe_tracking = false;
        int32_t w = lv_display_get_horizontal_resolution(lv_display_get_default());
        int32_t current_v = back_swipe_last_v;
        bool commit;
        if (back_swipe_last_velocity > 0) {
            commit = true; /* still moving right (toward Back) at release, however slowly */
        } else if (back_swipe_last_velocity < 0) {
            commit = false; /* still moving back left (toward cancel) at release, however slowly */
        } else {
            commit = current_v > w / 2; /* perfectly still at release -- fall back to position */
        }
        back_swipe_ctx->commit = commit;
        if (commit) {
            /* Stack-only bookkeeping -- actual lv_screen_load() waits for
             * slide_transition_done_cb(). Cancel leaves the stack
             * untouched, matching player-swipe's own cancel path. */
            nav_pop_stack_only();
            /* Lyrics' own timer/backdrop teardown, normally done by
             * close_lyrics_screen() -- skipped entirely on cancel, since a
             * cancelled swipe leaves the user back on Lyrics with both
             * still needed. */
            if (back_swipe_ctx->from_scr == gui_lyrics_get_screen()) gui_lyrics_prepare_exit();
        }
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, back_swipe_ctx);
        lv_anim_set_user_data(&a, back_swipe_ctx);
        lv_anim_set_values(&a, current_v, commit ? w : 0);
        lv_anim_set_duration(&a, QUICK_DRAWER_ANIM_MS);
        lv_anim_set_exec_cb(&a, slide_transition_anim_x_cb);
        lv_anim_set_completed_cb(&a, slide_transition_done_cb);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_start(&a);
        back_swipe_ctx = NULL;
        back_swipe_target_scr = NULL;
    }

    if (!pressed && quick_drawer_was_pressed) {
#ifdef UI_GESTURE_TRACE
        printf("[GESTURE_TRACE] poll: release observed (home_tracking=%d, drawer_tracking=%d, player_tracking=%d, back_tracking=%d)\n",
               home_swipe_tracking, quick_drawer_drag_tracking, player_swipe_tracking, back_swipe_tracking);
#endif
        player_swipe_candidate = false;
        back_swipe_candidate = false;
        back_swipe_owns_press = false;
        home_swipe_candidate = false;
    }

    quick_drawer_was_pressed = pressed;

    /* Every release-handling branch above (drawer snap, player-swipe
     * settle) has already run by this point in the same call that observed
     * the release -- nothing left to track until resume_fast_gesture_
     * timers_cb() wakes this again on the next press-down. See this
     * timer's own handle comment for why pausing (not just letting the
     * ~60fps tick keep firing and no-op) is what actually matters here. */
    if (!pressed) {
        drag_adjust_press_owned = false;
#ifdef UI_GESTURE_TRACE
        printf("[GESTURE_TRACE] poll: timer self-paused\n");
#endif
        lv_timer_pause(timer);
    }
}

/* Forward declarations -- defined later in this file with player screen transport buttons. */
void favorite_icon_event_cb(lv_event_t * e);
void prev_btn_event_cb(lv_event_t * e);
void play_btn_event_cb(lv_event_t * e);
void next_btn_event_cb(lv_event_t * e);
const char * basename_of(const char * path);
/* Defined much later, alongside the rest of the new Wi-Fi/Bluetooth
 * screens -- long-pressing the drawer's wifi/bt icons opens the real
 * settings screen for that radio, matching Android's quick-settings
 * convention (tap toggles, long-press opens the full screen). */

/* Long-press handlers for the drawer's wifi/bt icons -- hides the drawer
 * instantly (no slide-out animation; the settings screen navigation is
 * about to slide in over it anyway) then opens the real settings screen.
 *
 * LVGL still sends LV_EVENT_CLICKED on release even after LV_EVENT_LONG_PRESSED
 * fired. The long_press_fired flags ensure click handlers do not inadvertently
 * toggle radios when a long-press has already opened settings. */
static bool quick_drawer_wifi_long_press_fired = false;
static bool quick_drawer_bt_long_press_fired = false;

static void quick_drawer_wifi_long_press_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_LONG_PRESSED) return;
    quick_drawer_wifi_long_press_fired = true;
    quick_drawer_open = false;
    quick_drawer_finish_bitmap_motion();
    open_wifi_screen();
}

static void quick_drawer_bt_long_press_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_LONG_PRESSED) return;
    quick_drawer_bt_long_press_fired = true;
    quick_drawer_open = false;
    quick_drawer_finish_bitmap_motion();
    open_bluetooth_screen();
}

/* Tap-to-toggle handler for Wi-Fi. Runs asynchronously in a background thread
 * to avoid blocking the UI while enabling or disabling the radio. */
static pthread_t wifi_toggle_thread;
static atomic_bool wifi_toggle_done_flag = false;

static void * wifi_toggle_thread_func(void * arg) {
    (void) arg;
    bool turning_on = wifi_toggle_target_enabled;
    if (turning_on) wifi_control_enable();
    else wifi_control_disable();

    /* Wait for control socket to settle to avoid reading stale state immediately
     * after wifi_on.sh/wifi_off.sh execution. */
    for (int i = 0; i < 10 && wifi_control_is_enabled() != turning_on; i++) {
        usleep(300000);
    }

    atomic_store_explicit(&wifi_toggle_done_flag, true, memory_order_release); /* written last -- poll_wifi_toggle only checks this flag */
    return NULL;
}

/* populate_wifi_screen declared in gui.h */ /* defined with the rest of the Wi-Fi settings screen, below */

/* Everything that makes a tap read as instant, applied before the radio has
 * actually changed. wifi_control_is_enabled() is a plain access() check (see
 * its own comment), not a subprocess spawn, so the surrounding state reads
 * here are cheap enough to do synchronously. The real toggle takes a couple
 * of seconds; poll_wifi_toggle() re-reads the hardware once the worker lands
 * and corrects all of this if the toggle failed. */
static void wifi_toggle_apply_optimistic_ui(bool will_be_enabled) {
    lv_image_set_src(quick_drawer_wifi_icon, quick_drawer_toggle_src(QD_TOGGLE_WIFI, will_be_enabled));
    quick_drawer_mark_snapshot_dirty();

    if (will_be_enabled) {
        lv_obj_remove_flag(wifi_icon, LV_OBJ_FLAG_HIDDEN);
        lv_image_set_src(wifi_icon, asset_path("topbar/lucide_wifi_off.png"));
        layout_lucide_topbar_image(wifi_icon);
    } else {
        lv_obj_add_flag(wifi_icon, LV_OBJ_FLAG_HIDDEN);
    }
    sync_topbar_status_icon_positions();

    /* Do not clean/rebuild wifi_list from inside the clicked row's own event
     * callback: that deletes the event target while LVGL is still dispatching
     * through it. gui_network_show_wifi_toggle_pending() defers the rebuild by
     * one UI turn; poll_wifi_toggle() performs the authoritative one. */
    if (gui_navigation_is_top(gui_network_get_wifi_screen()))
        gui_network_show_wifi_toggle_pending(will_be_enabled);
}

void quick_drawer_wifi_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (quick_drawer_wifi_long_press_fired) { /* see quick_drawer_wifi_long_press_cb()'s own comment */
        quick_drawer_wifi_long_press_fired = false;
        return;
    }
    if (wifi_toggle_active) {
        /* Queue rather than drop: the in-flight attempt owns the radio until
         * it settles, but the user's newest intent still has to survive.
         * poll_wifi_toggle() launches this once the current attempt lands,
         * and only if it actually disagrees with the settled state. */
        wifi_toggle_queued_target = !gui_shell_wifi_effective_enabled();
        wifi_toggle_queued = true;
        wifi_toggle_apply_optimistic_ui(wifi_toggle_queued_target);
        return;
    }
    bool wifi_will_be_enabled = !wifi_control_is_enabled();
    wifi_toggle_active = true;
    wifi_toggle_is_radio_suspend = false; /* a real user tap, not the idle radio-suspend cycle */
    atomic_store_explicit(&wifi_toggle_done_flag, false, memory_order_relaxed);
    wifi_toggle_target_enabled = wifi_will_be_enabled;

    wifi_toggle_apply_optimistic_ui(wifi_will_be_enabled);

    if (pthread_create(&wifi_toggle_thread, NULL, wifi_toggle_thread_func, NULL) != 0) {
        wifi_toggle_active = false;
        refresh_wifi_icon();
        start_bt_dac_startup_reapply_if_needed();
        gui_network_wifi_toggle_completed(wifi_control_is_enabled());
    }
}

static void poll_wifi_toggle(void) {
    if (!wifi_toggle_active || !atomic_load_explicit(&wifi_toggle_done_flag, memory_order_acquire)) return;
    wifi_toggle_active = false;
    pthread_join(wifi_toggle_thread, NULL);
    bool enabled = wifi_control_is_enabled();
    refresh_wifi_icon(); /* re-reads the real state -- updates both the status bar and drawer icons */
    gui_network_wifi_toggle_completed(enabled); /* authoritative rows + scan state */
    if (enabled != wifi_toggle_target_enabled) show_error_toast("Wi-Fi failed to change state");

    /* Only shut AirPlay/DLNA/Remote Control/Import down once the disable is
     * AUTHORITATIVELY confirmed via the real wifi_control_is_enabled() read
     * above, not merely because wifi_toggle_target_enabled asked for OFF --
     * a failed disable (enabled != wifi_toggle_target_enabled, toast just
     * above) must leave those features running exactly as they were.
     *
     * Also excludes the automatic idle radio-suspend disable (wifi_toggle_
     * is_radio_suspend, set by gui_shell_suspend_connections()) -- that one
     * shares this exact same toggle-thread/poll mechanism as the user's own
     * tap, but is a transient, self-reversing power-save blip (gui_shell_
     * resume_connections() brings the radio back the moment the screen
     * wakes), not a deliberate "turn Wi-Fi off" the user asked for. Running
     * the permanent cleanup for it would stop DLNA/Remote Control (AirPlay/
     * BT DAC are already excluded from ever reaching radio suspend at all,
     * via gui.c's own radios_suspended gate) and permanently clear their
     * persisted settings every time the screen idles, with no way for the
     * plain radio-restore afterward to ever turn them back on. See wifi_
     * toggle_is_radio_suspend's own comment above for the full reasoning. */
    bool was_radio_suspend = wifi_toggle_is_radio_suspend;
    wifi_toggle_is_radio_suspend = false;

    /* A queued request that disagrees with where the radio actually landed is
     * relaunched here, once the previous attempt has fully released it. */
    bool relaunch = wifi_toggle_queued && wifi_toggle_queued_target != enabled;
    wifi_toggle_queued = false;

    /* Runs for a queued re-enable too, so a disable behaves the same whether
     * the user reverses it quickly or slowly. The features this tears down
     * are gated on Wi-Fi being connected rather than merely enabled, so an
     * off state that is about to be undone has nothing live to take away. */
    if (!enabled && !was_radio_suspend) gui_network_handle_wifi_disabled();

    if (relaunch) {
        wifi_toggle_active = true;
        wifi_toggle_is_radio_suspend = false;
        atomic_store_explicit(&wifi_toggle_done_flag, false, memory_order_relaxed);
        wifi_toggle_target_enabled = wifi_toggle_queued_target;
        wifi_toggle_apply_optimistic_ui(wifi_toggle_queued_target);
        if (pthread_create(&wifi_toggle_thread, NULL, wifi_toggle_thread_func, NULL) != 0) {
            wifi_toggle_active = false;
            refresh_wifi_icon();
            gui_network_wifi_toggle_completed(wifi_control_is_enabled());
        }
    }
}

/* Same real tap-to-toggle treatment for Bluetooth, mirroring the wifi
 * mechanism above -- bluetoothctl's power on/off each block for about a
 * second, so this runs on its own thread too. Turning ON additionally
 * brings up the chip first via bt_control_init_chip() if it isn't already
 * (no-op once hci0 exists) -- see that function's own comment for why this,
 * not just bluetoothctl power on, is what actually makes the toggle work at
 * all on a fresh boot. */
static pthread_t bt_toggle_thread;
static atomic_bool bt_toggle_done_flag = false;


/* Set by bt_toggle_thread_func() when disabling Bluetooth while DAC mode
 * was on -- consumed by poll_bt_toggle() to turn the setting off and close
 * the DAC overlay screen if it's the one currently showing. */
static bool bt_toggle_forced_dac_off = false;

/* Encoded pointer values avoid allocating a one-bool thread argument. NULL
 * remains available for legacy/inferred callers, though all current launch
 * sites pass an explicit target so the worker never needs a potentially
 * 15-second bluetoothctl query just to decide which operation to perform. */
#define BT_TOGGLE_TARGET_ON  ((void *) (intptr_t) 1)
#define BT_TOGGLE_TARGET_OFF ((void *) (intptr_t) 2)

static void * bt_toggle_target_arg(bool enabled) {
    return enabled ? BT_TOGGLE_TARGET_ON : BT_TOGGLE_TARGET_OFF;
}

static void * bt_toggle_thread_func(void * arg) {
    bt_toggle_forced_dac_off = false;
    bool turning_on;
    if (arg == BT_TOGGLE_TARGET_ON) turning_on = true;
    else if (arg == BT_TOGGLE_TARGET_OFF) turning_on = false;
    else turning_on = !bt_control_is_powered();
    bool chip_wedged = false;
    if (!turning_on) {
        atomic_store_explicit(&bt_media_player_enable_pending, false, memory_order_release);
        /* If Bluetooth DAC mode is active, tear down its processes
         * (bluealsa/bt-agent) before powering down the radio to prevent
         * orphaned processes from corrupting bluetoothd adapter registration. */
        if (current_settings.bt_dac_mode_enabled) {
            bt_control_apply_output_settings(false, current_settings.bt_volume_sync_enabled);
            bt_toggle_forced_dac_off = true;
        }
        bt_control_disable();
    } else {
        /* If chip initialization fails, skip calling bt_control_enable() to
         * avoid redundant timeouts against a non-existent controller. */
        if (bt_control_init_chip()) {
            bt_control_enable();
            mark_bt_media_player_enable_pending();
        } else chip_wedged = true;
    }

    /* Do not confirm via bt_control_is_powered() here. Each status query has
     * a legitimate 15-second timeout; after resume, Bluetooth was already
     * usable while several such confirmations kept bt_toggle_active true
     * for ~30 seconds and caused every disable tap to be discarded. Give
     * bluetoothd one short propagation interval, then let the existing
     * asynchronous authoritative refresh confirm/correct the optimistic UI.
     * A queued opposite request can start as soon as this worker is reaped. */
    if (!chip_wedged) usleep(500000);

    atomic_store_explicit(&bt_toggle_done_flag, true, memory_order_release); /* written last -- poll_bt_toggle only checks this flag */
    return NULL;
}

/* Queues Bluetooth enable intent if requested while bt_init is still running.
 * Waits for BT_INIT_OK_FLAG_PATH and stable off state before executing
 * the chip initialization and enable sequence, providing immediate visual
 * feedback via optimistic icon updates. */
#define BT_BOOT_ENABLE_MAX_WAIT_MS 30000
#define BT_BOOT_ENABLE_POLL_INTERVAL_MS 300
#define BT_BOOT_ENABLE_OFF_OBSERVATIONS_REQUIRED 2

static void * bt_pending_enable_thread_func(void * arg) {
    (void) arg;
    uint32_t waited_ms = 0;
    unsigned int off_observations = 0;
    bool init_finished = false;
    while (waited_ms < BT_BOOT_ENABLE_MAX_WAIT_MS) {
        if (access(BT_INIT_OK_FLAG_PATH, F_OK) == 0) {
            init_finished = true;
            if (!bt_control_is_powered()) {
                off_observations++;
                if (off_observations >= BT_BOOT_ENABLE_OFF_OBSERVATIONS_REQUIRED) {
                    if (bt_control_init_chip()) {
                        bt_control_enable();
                        mark_bt_media_player_enable_pending();
                    }
                    break;
                }
            } else {
                off_observations = 0;
            }
        }
        usleep(BT_BOOT_ENABLE_POLL_INTERVAL_MS * 1000);
        waited_ms += BT_BOOT_ENABLE_POLL_INTERVAL_MS;
    }
    /* If initialization finished but its state never produced two clean
     * off samples before the bounded wait elapsed, assert the requested
     * final state once anyway. Never do this without bt_init_ok: that would
     * reintroduce the unsafe concurrent UART initialization race. */
    if (init_finished && off_observations < BT_BOOT_ENABLE_OFF_OBSERVATIONS_REQUIRED) {
        if (bt_control_init_chip()) {
            bt_control_enable();
            mark_bt_media_player_enable_pending();
        }
    }
    atomic_store_explicit(&bt_toggle_done_flag, true, memory_order_release); /* written last -- poll_bt_toggle only checks this flag */
    return NULL;
}

static void show_optimistic_bt_state(bool powered) {
    lv_image_set_src(quick_drawer_bt_icon, quick_drawer_toggle_src(QD_TOGGLE_BT, powered));
    quick_drawer_mark_snapshot_dirty();

    bt_disconnect_epoch++;
    bt_is_a2dp_connected_ui = false;
    bt_connected_mac_cached[0] = '\0';
    bt_connected_codec_cached[0] = '\0';
    if (powered) {
        lv_obj_remove_flag(bt_status_icon, LV_OBJ_FLAG_HIDDEN);
        lv_image_set_src(bt_status_icon, asset_path("topbar/lucide_bluetooth_off.png"));
        layout_lucide_topbar_image(bt_status_icon);
    } else {
        lv_obj_add_flag(bt_status_icon, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_add_flag(a2dp_status_icon, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(bt_codec_status_icon, LV_OBJ_FLAG_HIDDEN);
    invalidate_bt_codec_status_cache();
    sync_topbar_status_icon_positions();

    bt_is_powered_cached = powered;
    if (!powered) clear_bt_audio_route_now();
    if (gui_navigation_is_top(gui_network_get_bt_screen())) populate_bt_screen();
}

void quick_drawer_bt_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (quick_drawer_bt_long_press_fired) { /* see quick_drawer_bt_long_press_cb()'s own comment */
        quick_drawer_bt_long_press_fired = false;
        return;
    }
    bool bt_will_be_powered = !bt_is_powered_cached;

    if (!bt_will_be_powered) {
        /* Explicit OFF is user intent immediately, before the slow toggle
         * worker has produced its next authoritative snapshot. */
        gui_shell_cancel_bt_reconnect();
        bt_reconnect_observed_power_valid = true;
        bt_reconnect_observed_powered = false;
    } else {
        /* A rapid user OFF->ON is a new requested cycle even if the polling
         * cadence did not happen to observe the intermediate OFF state. */
        bt_reconnect_observed_power_valid = true;
        bt_reconnect_observed_powered = false;
        bt_reconnect_cycle_suppressed = false;
    }

    /* Last intent wins while a slow resume/enable is still finishing. The
     * running operation cannot be safely interrupted while it owns the chip
     * mutex, but an opposite tap is remembered and launched immediately
     * after it completes instead of being silently discarded. Repeated taps
     * collapse back to the in-flight target when appropriate. */
    if (bt_toggle_active) {
        bt_toggle_followup_target_enabled = bt_will_be_powered;
        bt_toggle_followup_pending = (bt_will_be_powered != bt_toggle_target_enabled);
        show_optimistic_bt_state(bt_will_be_powered);
        return;
    }

    /* If turning on before /tmp/bt_init_ok exists, queue behind
     * bt_pending_enable_thread_func() to wait for initialization to complete.
     * Disabling is D-Bus-only and safe to run directly. */
    bool bt_pending_now = bt_will_be_powered && access(BT_INIT_OK_FLAG_PATH, F_OK) != 0;

    bt_toggle_active = true;
    bt_toggle_target_enabled = bt_will_be_powered;
    bt_toggle_followup_pending = false;
    atomic_store_explicit(&bt_toggle_done_flag, false, memory_order_relaxed);

    /* Optimistically update topbar and drawer icons, and rebuild the Bluetooth
     * settings screen if visible. Confirmed by authoritative poll on completion. */
    show_optimistic_bt_state(bt_will_be_powered);

    /* Runs fully in the background, same as the stock player -- no busy
     * screen. */
    void * (*thread_func)(void *) = bt_pending_now ? bt_pending_enable_thread_func : bt_toggle_thread_func;
    void * thread_arg = bt_pending_now ? NULL : bt_toggle_target_arg(bt_will_be_powered);
    if (pthread_create(&bt_toggle_thread, NULL, thread_func, thread_arg) != 0) {
        bt_toggle_active = false;
        bt_is_powered_cached = !bt_will_be_powered;
        if (gui_navigation_is_top(gui_network_get_bt_screen())) populate_bt_screen();
        show_info_toast("Failed to toggle Bluetooth");
    }
}

static void poll_bt_toggle(void) {
    if (!bt_toggle_active || !atomic_load_explicit(&bt_toggle_done_flag, memory_order_acquire)) return;
    bt_toggle_active = false;
    pthread_join(bt_toggle_thread, NULL);

    if (bt_toggle_forced_dac_off) {
        current_settings.bt_dac_mode_enabled = false;
        settings_save(&current_settings);
        /* Bluetooth just got disabled out from under DAC mode -- if its
         * overlay is the screen currently showing, staying on it is
         * meaningless (there's no Bluetooth left to receive audio over),
         * so close it automatically instead of leaving a "Bluetooth DAC
         * mode" screen up with nothing backing it. */
        if (lv_screen_active() == gui_network_get_bt_dac_overlay()) nav_pop();
    }

    if (bt_toggle_followup_pending) {
        bool target = bt_toggle_followup_target_enabled;
        bt_toggle_followup_pending = false;
        bt_toggle_target_enabled = target;
        bt_toggle_active = true;
        atomic_store_explicit(&bt_toggle_done_flag, false, memory_order_relaxed);
        if (pthread_create(&bt_toggle_thread, NULL, bt_toggle_thread_func,
                           bt_toggle_target_arg(target)) == 0)
            return;
        bt_toggle_active = false;
        show_info_toast("Failed to toggle Bluetooth");
    }

    /* start_refresh_bt_icon() only starts the background check -- it
     * hasn't updated bt_is_powered_cached yet by the time this returns, so
     * populate_bt_screen() (which now reads that cache, not a fresh
     * bt_control_is_powered() call -- see its own comment) can't be called
     * here too or the Bluetooth screen's toggle row would show stale
     * (pre-toggle) state until something else happens to repopulate it.
     * poll_refresh_bt_icon() calls populate_bt_screen() itself once the
     * cache is actually fresh. */
    start_refresh_bt_icon(); /* re-reads the real state -- updates the status bar/drawer icons and (once done) the Bluetooth screen's toggle row */
}

/* Applies bt_dac_mode_enabled configuration asynchronously during the
 * startup window, launching the necessary bluealsa and bt-agent processes
 * without blocking the UI thread. */
static pthread_t bt_dac_startup_reapply_thread;
static bool bt_dac_startup_reapply_active = false;
static bool bt_dac_startup_reapply_started = false;
static atomic_bool bt_dac_startup_reapply_done_flag = false;

static void * bt_dac_startup_reapply_thread_func(void * arg) {
    (void) arg;
    bt_control_init_chip();
    bt_control_enable();
    mark_bt_media_player_enable_pending();
    bt_control_apply_output_settings(true, current_settings.bt_volume_sync_enabled);
    atomic_store_explicit(&bt_dac_startup_reapply_done_flag, true, memory_order_release); /* written last -- poll_bt_dac_startup_reapply only checks this flag */
    return NULL;
}

/* Runs at most once, and only if DAC mode was switched on before the
 * Bluetooth startup gate opened. bt_dac_mode_enabled is not persisted, so
 * this never fires on a normal boot; a toggle-on tap after that gate goes
 * through bt_dac_toggle_cb() directly and doesn't need this. */
static void start_bt_dac_startup_reapply_if_needed(void) {
    if (!current_settings.bt_dac_mode_enabled || bt_dac_startup_reapply_started ||
        !refresh_bt_startup_readiness()) return;
    bt_dac_startup_reapply_started = true;
    bt_dac_startup_reapply_active = true;
    atomic_store_explicit(&bt_dac_startup_reapply_done_flag, false, memory_order_relaxed);
    if (pthread_create(&bt_dac_startup_reapply_thread, NULL, bt_dac_startup_reapply_thread_func, NULL) != 0) {
        bt_dac_startup_reapply_active = false;
        bt_dac_startup_reapply_started = false;
    }
}


static void poll_bt_dac_startup_reapply(void) {
    if (!bt_dac_startup_reapply_active || !atomic_load_explicit(&bt_dac_startup_reapply_done_flag, memory_order_acquire)) return;
    bt_dac_startup_reapply_active = false;
    pthread_join(bt_dac_startup_reapply_thread, NULL);
    start_refresh_bt_icon();
}

/* Asynchronously executes bt_control_apply_output_settings() on a background
 * thread to avoid blocking the UI thread with process restarts (bluealsa,
 * bt-agent, bluealsa-aplay) and sleeps. */
static pthread_t bt_apply_output_settings_thread;
static bool bt_apply_output_settings_active = false;
static atomic_bool bt_apply_output_settings_done_flag = false;

static pthread_t bt_source_codec_reconcile_thread;
static bool bt_source_codec_reconcile_active = false;
static bool bt_source_codec_reconcile_started = false;
static bool bt_source_codec_reconcile_succeeded = false;
static bool bt_source_codec_reconcile_retry_pending = true;
static uint32_t bt_source_codec_reconcile_failed_generation = 0;
static atomic_bool bt_source_codec_reconcile_done_flag = false;

static void * bt_source_codec_reconcile_thread_func(void * arg) {
    (void) arg;
    bool success = false;
    if (!current_settings.bt_dac_mode_enabled && bt_control_is_powered())
        success = bt_control_reconcile_source_settings();
    bt_source_codec_reconcile_succeeded = success;
    bt_source_codec_reconcile_retry_pending = !success;
    atomic_store_explicit(&bt_source_codec_reconcile_done_flag, true, memory_order_release);
    return NULL;
}

static void start_bt_source_codec_reconcile_if_needed(void) {
    if (bt_source_codec_reconcile_active || bt_source_codec_reconcile_succeeded ||
        current_settings.bt_dac_mode_enabled ||
        !refresh_bt_startup_readiness()) return;
    /* The first attempt is allowed to query the authoritative state even if
     * the cached status is still false. After a transient failure, wait for a
     * newer completed status refresh before retrying; this avoids a retry
     * storm while still recovering when Bluetooth becomes ready later. A
     * deferred reconcile is different: while the cached A2DP link remains
     * up, retrying every newer status generation would just repeat the
     * connected-accessory guard. Wait for the cached link to go away before
     * trying again, so the next attempt can actually restart the daemon. */
    if (bt_source_codec_reconcile_started &&
        (!bt_source_codec_reconcile_retry_pending ||
         bt_source_codec_reconcile_failed_generation == bt_power_status_generation ||
         !bt_is_powered_cached ||
         gui_shell_is_bt_audio_connected())) return;
    bt_source_codec_reconcile_started = true;
    bt_source_codec_reconcile_active = true;
    atomic_store_explicit(&bt_source_codec_reconcile_done_flag, false, memory_order_relaxed);
    if (pthread_create(&bt_source_codec_reconcile_thread, NULL,
                       bt_source_codec_reconcile_thread_func, NULL) != 0) {
        bt_source_codec_reconcile_active = false;
        bt_source_codec_reconcile_retry_pending = true;
        bt_source_codec_reconcile_failed_generation = bt_power_status_generation;
    }
}

static void poll_bt_source_codec_reconcile(void) {
    if (!bt_source_codec_reconcile_active ||
        !atomic_load_explicit(&bt_source_codec_reconcile_done_flag, memory_order_acquire)) return;
    bt_source_codec_reconcile_active = false;
    pthread_join(bt_source_codec_reconcile_thread, NULL);
    if (!bt_source_codec_reconcile_succeeded)
        bt_source_codec_reconcile_failed_generation = bt_power_status_generation;
}

static void poll_bt_reconnect(void) {
    bt_reconnect_poll();
    if (!bt_reconnect_pending || bt_reconnect_cycle_suppressed ||
        bt_reconnect_busy() || !bt_reconnect_observed_powered ||
        current_settings.bt_dac_mode_enabled || bt_toggle_active ||
        bt_source_codec_reconcile_active ||
        bt_dac_startup_reapply_active || bt_apply_output_settings_active)
        return;

    /* Consume the arm before starting. A failed start or exhausted worker is
     * deliberately not retried until a new authoritative power cycle. */
    bt_reconnect_pending = false;
    bt_reconnect_start(current_settings.bt_last_output_mac[0] ?
                       current_settings.bt_last_output_mac : NULL);
}

typedef struct {
    bool dac_mode_enabled;
    bool volume_sync_enabled;
} bt_apply_output_settings_request_t;

static void * bt_apply_output_settings_thread_func(void * arg) {
    bt_apply_output_settings_request_t * req = (bt_apply_output_settings_request_t *) arg;
    bt_control_apply_output_settings(req->dac_mode_enabled, req->volume_sync_enabled);
    free(req);
    atomic_store_explicit(&bt_apply_output_settings_done_flag, true, memory_order_release); /* written last -- poll_bt_apply_output_settings only checks this flag */
    return NULL;
}

/* Silently ignores overlap (another apply already in flight) rather than
 * queuing -- same "ignore taps until it lands" treatment as
 * quick_drawer_bt_event_cb()/quick_drawer_wifi_event_cb() use for their own
 * slow operations, and the current_settings values the caller already wrote
 * before calling this are what the eventually-scheduled apply would use
 * anyway once the in-flight one finishes and the screen is re-populated. */
void start_bt_apply_output_settings(bool dac_mode_enabled, bool volume_sync_enabled) {
    if (bt_apply_output_settings_active) return;
    bt_apply_output_settings_request_t * req = malloc(sizeof(*req));
    if (!req) return;
    gui_shell_cancel_bt_reconnect();
    req->dac_mode_enabled = dac_mode_enabled;
    req->volume_sync_enabled = volume_sync_enabled;
    atomic_store_explicit(&bt_apply_output_settings_done_flag, false, memory_order_relaxed);
    bt_apply_output_settings_active = true;
        if (pthread_create(&bt_apply_output_settings_thread, NULL, bt_apply_output_settings_thread_func, req) != 0) {
        bt_apply_output_settings_active = false;
        free(req);
    }
}


static void poll_bt_apply_output_settings(void) {
    if (!bt_apply_output_settings_active || !atomic_load_explicit(&bt_apply_output_settings_done_flag, memory_order_acquire)) return;
    bt_apply_output_settings_active = false;
    pthread_join(bt_apply_output_settings_thread, NULL);
    populate_bt_dac_screen(); /* the DAC screen's own toggle rows need the post-apply state */
}

#define BRIGHTNESS_HW_APPLY_INTERVAL_MS 50

static void brightness_hw_apply_pending(void) {
    int pending = brightness_hw_pending;
    if (pending < 0) return;
    brightness_hw_pending = -1;
    backlight_request_normal_percent(pending);
}

static void brightness_hw_apply_timer_cb(lv_timer_t * timer) {
    (void) timer;
    brightness_hw_apply_pending();
    if (brightness_hw_apply_timer && !brightness_drag_active && brightness_hw_pending < 0)
        lv_timer_pause(brightness_hw_apply_timer);
}

static void quick_drawer_brightness_changed_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * slider = (lv_obj_t *) lv_event_get_target(e);
    int32_t percent = lv_slider_get_value(slider);

    if (code == LV_EVENT_PRESSED) {
        brightness_drag_active = true;
        if (brightness_hw_apply_timer) {
            lv_timer_reset(brightness_hw_apply_timer);
            lv_timer_resume(brightness_hw_apply_timer);
        }
    } else if (code == LV_EVENT_VALUE_CHANGED) {
        brightness_hw_pending = (int) percent;
        if (quick_drawer_brightness_label)
            lv_label_set_text_fmt(quick_drawer_brightness_label, "%d", (int) percent);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        brightness_drag_active = false;
        brightness_hw_pending = (int) percent;
        brightness_hw_apply_pending();
        if (brightness_hw_apply_timer) lv_timer_pause(brightness_hw_apply_timer);
        if (quick_drawer_brightness_label)
            lv_label_set_text_fmt(quick_drawer_brightness_label, "%d", (int) percent);
        current_settings.brightness_percent = (int) percent;
        settings_save_async(&current_settings);
        quick_drawer_mark_snapshot_dirty(); /* one rebuild, now that the label has settled at its final value */
    }
}

static void build_quick_drawer(void) {
    int32_t w = lv_display_get_horizontal_resolution(lv_display_get_default());
    int32_t h = lv_display_get_vertical_resolution(lv_display_get_default());

    quick_drawer = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(quick_drawer);
    lv_obj_set_size(quick_drawer, w, h);
    lv_obj_set_pos(quick_drawer, 0, -h); /* fully off-screen above until opened */
    lv_obj_set_style_bg_color(quick_drawer, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(quick_drawer, LV_OPA_COVER, 0);
    lv_obj_remove_flag(quick_drawer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(quick_drawer, LV_OBJ_FLAG_CLICKABLE); /* swallow touches to whatever's behind while open */

    /* The reference uses one continuous rounded drawer surface.  Keep the
     * panel independent of the vendor backdrop asset so every native 480x800
     * coordinate remains explicit and stable across theme revisions. */
    lv_obj_t * drawer_panel = lv_obj_create(quick_drawer);
    lv_obj_remove_style_all(drawer_panel);
    lv_obj_set_pos(drawer_panel, BOARD_SCALE_PX(19), BOARD_SCALE_PY(59));
    /* 726, not 683: the panel used to stop at y=742 on an 800px screen and
     * leave 58px of dead space below it. Ends at 785 now, keeping the same
     * ~15px bottom margin the sides already use. */
    lv_obj_set_size(drawer_panel, BOARD_SCALE_PX(442), BOARD_SCALE_PY(726));
    lv_obj_set_style_bg_color(drawer_panel, lv_color_hex(0x0d130f), 0);
    lv_obj_set_style_bg_opa(drawer_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(drawer_panel, BOARD_SCALE_PX(30), 0);
    lv_obj_remove_flag(drawer_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_background(drawer_panel);

    quick_drawer_expansion_handle = lv_obj_create(quick_drawer);
    lv_obj_t * expansion_handle = quick_drawer_expansion_handle;
    lv_obj_remove_style_all(expansion_handle);
    lv_obj_set_pos(expansion_handle, BOARD_SCALE_PX(210), BOARD_SCALE_PY(219));
    lv_obj_set_size(expansion_handle, BOARD_SCALE_PX(59), BOARD_SCALE_PY(8));
    lv_obj_set_style_bg_color(expansion_handle, lv_color_hex(0x4a4d4b), 0);
    lv_obj_set_style_bg_opa(expansion_handle, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(expansion_handle, BOARD_SCALE_PX(4), 0);
    /* A bar this thin is far too small a touch target on its own; the drag
     * itself is claimed by coordinate hit-test in poll_quick_drawer_drag()
     * over the same padded rectangle this advertises. */
    lv_obj_set_ext_click_area(expansion_handle, BOARD_SCALE_PX(30));
    lv_obj_add_flag(expansion_handle, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(expansion_handle, quick_drawer_expansion_handle_cb, LV_EVENT_CLICKED, NULL);

    /* Brightness first, volume second -- swapped from the reference's own
     * order at the user's request. Only the container Y (and the matching
     * icon/track/label Y offsets below) moved; every X position, size, and
     * per-row delta stays exactly as measured before. */
    lv_obj_t * brightness_container = lv_obj_create(quick_drawer);
    lv_obj_remove_style_all(brightness_container);
    lv_obj_set_pos(brightness_container, BOARD_SCALE_PX(34), BOARD_SCALE_PY(238));
    lv_obj_set_size(brightness_container, BOARD_SCALE_PX(413), BOARD_SCALE_PY(72));
    lv_obj_set_style_bg_color(brightness_container, lv_color_hex(0x151b17), 0);
    lv_obj_set_style_bg_opa(brightness_container, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(brightness_container, BOARD_SCALE_PX(23), 0);

    lv_obj_t * volume_container = lv_obj_create(quick_drawer);
    lv_obj_remove_style_all(volume_container);
    lv_obj_set_pos(volume_container, BOARD_SCALE_PX(34), BOARD_SCALE_PY(325));
    lv_obj_set_size(volume_container, BOARD_SCALE_PX(413), BOARD_SCALE_PY(73));
    lv_obj_set_style_bg_color(volume_container, lv_color_hex(0x151b17), 0);
    lv_obj_set_style_bg_opa(volume_container, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(volume_container, BOARD_SCALE_PX(23), 0);

    /* Decode the four "on" icons with the current accent before anything
     * asks for one below -- quick_drawer_toggle_src() falls back to the
     * plain (stock-blue) asset while these are unloaded. */
    load_quick_drawer_toggle_on_images();

    /* Row 1: every toggle icon (Bluetooth / Wifi / sleep timer / output
     * gain) together in one row -- 4 icons x 84px + 5 gaps of 21px exactly
     * fills the panel's measured 440px content width (19 to 459). No clock,
     * no volume slider here anymore (clock duplicated the always-visible
     * main status bar; a second volume control duplicated the hardware
     * volume buttons' own popup) -- brightness (row 2 below) is the only
     * slider left in this drawer. */
    quick_drawer_wifi_icon = lv_image_create(quick_drawer);
    lv_image_set_src(quick_drawer_wifi_icon, asset_path("pull_down/wifi.png"));
    lv_obj_align(quick_drawer_wifi_icon, LV_ALIGN_TOP_LEFT, BOARD_SCALE_PX(39), QUICK_DRAWER_ROW1_TOP);
    lv_obj_add_flag(quick_drawer_wifi_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(quick_drawer_wifi_icon, quick_drawer_wifi_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(quick_drawer_wifi_icon, quick_drawer_wifi_long_press_cb, LV_EVENT_LONG_PRESSED, NULL);

    quick_drawer_bt_icon = lv_image_create(quick_drawer);
    lv_image_set_src(quick_drawer_bt_icon, asset_path("pull_down/bt.png"));
    lv_obj_align(quick_drawer_bt_icon, LV_ALIGN_TOP_LEFT, BOARD_SCALE_PX(143), QUICK_DRAWER_ROW1_TOP);
    lv_obj_add_flag(quick_drawer_bt_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(quick_drawer_bt_icon, quick_drawer_bt_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(quick_drawer_bt_icon, quick_drawer_bt_long_press_cb, LV_EVENT_LONG_PRESSED, NULL);

    quick_drawer_sleep_icon = lv_image_create(quick_drawer);
    lv_image_set_src(quick_drawer_sleep_icon, asset_path("pull_down/sleep_switch.png"));
    lv_obj_align(quick_drawer_sleep_icon, LV_ALIGN_TOP_LEFT, BOARD_SCALE_PX(251), QUICK_DRAWER_ROW1_TOP);
    lv_obj_add_flag(quick_drawer_sleep_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(quick_drawer_sleep_icon, quick_drawer_sleep_event_cb, LV_EVENT_CLICKED, NULL);

    /* Countdown while armed -- see quick_drawer_sleep_event_cb()/
     * poll_sleep_timer()'s own comments. Hidden until armed, centered under
     * the 84px-wide icon above it. */
    quick_drawer_sleep_label = lv_label_create(quick_drawer);
    lv_obj_add_style(quick_drawer_sleep_label, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(quick_drawer_sleep_label, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_set_width(quick_drawer_sleep_label, BOARD_SCALE_PX(84));
    lv_obj_set_style_text_align(quick_drawer_sleep_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(quick_drawer_sleep_label, LV_ALIGN_TOP_LEFT, BOARD_SCALE_PX(250), QUICK_DRAWER_ROW1_TOP + QUICK_DRAWER_TOGGLE_ICON_PX + 2);
    lv_obj_add_flag(quick_drawer_sleep_label, LV_OBJ_FLAG_HIDDEN);

    quick_drawer_crossfade_icon = lv_image_create(quick_drawer);
    lv_obj_align(quick_drawer_crossfade_icon, LV_ALIGN_TOP_LEFT, BOARD_SCALE_PX(358), QUICK_DRAWER_ROW1_TOP);
    lv_obj_add_flag(quick_drawer_crossfade_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(quick_drawer_crossfade_icon, quick_drawer_crossfade_event_cb, LV_EVENT_CLICKED, NULL);
    refresh_quick_drawer_crossfade_icon();

    /* Captions and volume rail complete the reference drawer's first panel.
     * Fixed &lv_font_montserrat_12, not gui_theme_font(GUI_FONT_ROLE_SUBTEXT)
     * -- that role scales with Settings > Font Size (app_font_16 underneath),
     * which is right for reading text but was already too big to fit "Bluetooth"
     * in this 84px-wide slot below its icon even at the default tier, let alone
     * BlindMF. Fixed and non-scaling, same "accessibility-independent" choice
     * build_status_bar() already makes for its own clock/volume/battery faces. */
    const char * names[] = { "Wi-Fi", "Bluetooth", "Sleep", "Crossfade" };
    const int label_x[] = { 39, 143, 251, 358 };
    for (int i = 0; i < 4; i++) {
        lv_obj_t * name = lv_label_create(quick_drawer);
        lv_label_set_text(name, names[i]);
        lv_obj_add_style(name, &style_theme_text_primary, 0);
        lv_obj_set_style_text_font(name, &lv_font_montserrat_12, 0);
        lv_obj_set_width(name, BOARD_SCALE_PX(84));
        lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(name, LV_ALIGN_TOP_LEFT, BOARD_SCALE_PX(label_x[i]), QUICK_DRAWER_ROW1_TOP + QUICK_DRAWER_TOGGLE_ICON_PX + 2);
        quick_drawer_toggle_state[i] = lv_label_create(quick_drawer);
        lv_obj_set_width(quick_drawer_toggle_state[i], BOARD_SCALE_PX(84));
        lv_obj_set_style_text_align(quick_drawer_toggle_state[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(quick_drawer_toggle_state[i], &lv_font_montserrat_12, 0);
        lv_obj_align(quick_drawer_toggle_state[i], LV_ALIGN_TOP_LEFT, BOARD_SCALE_PX(label_x[i]), QUICK_DRAWER_ROW1_TOP + QUICK_DRAWER_TOGGLE_ICON_PX + 24);
        lv_label_set_text(quick_drawer_toggle_state[i], "Off");
        lv_obj_set_style_text_color(quick_drawer_toggle_state[i], lv_color_hex(0x8d918f), 0);
    }
    quick_drawer_set_toggle_state(0, gui_shell_wifi_effective_enabled());
    quick_drawer_set_toggle_state(1, bt_control_is_powered());
    quick_drawer_set_toggle_state(2, sleep_timer_active);
    quick_drawer_set_toggle_state(3, current_settings.crossfade_enabled);

    /* ---- Expanded rows 2 and 3, inside the clipping box ----------------
     * Same 4-column grid and same caption geometry as row 1 above, just
     * offset by whole QUICK_DRAWER_TOGGLE_ROW_PITCH steps and expressed
     * relative to the box's own top rather than the drawer's. Row 3 exists
     * only when a plugin registered a quick toggle, so with none installed
     * the drawer expands by one row instead of two. */
    quick_drawer_expansion_box = lv_obj_create(quick_drawer);
    lv_obj_remove_style_all(quick_drawer_expansion_box);
    lv_obj_set_pos(quick_drawer_expansion_box, 0, QUICK_DRAWER_EXPANSION_TOP);
    lv_obj_set_width(quick_drawer_expansion_box, BOARD_SCALE_PX(480));
    lv_obj_set_height(quick_drawer_expansion_box, 0);
    lv_obj_remove_flag(quick_drawer_expansion_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(quick_drawer_expansion_box, 0, 0);

    quick_drawer_plugin_toggle_count = plugin_manager_get_quick_toggle_count();
    if (quick_drawer_plugin_toggle_count > PLUGIN_MAX_QUICK_TOGGLES)
        quick_drawer_plugin_toggle_count = PLUGIN_MAX_QUICK_TOGGLES;

    /* "RC", not "HiBy Link": the stock asset is hibylink.png but what it
     * actually drives here is remote_control.h's own phone web UI, and the
     * full name does not fit an 84px slot at any font tier. */
    static const char * const row2_names[] = { "AirPlay", "DLNA", "Gapless", "RC" };
    for (int i = 0; i < 4; i++) {
        int32_t x = BOARD_SCALE_PX(label_x[i]);
        lv_obj_t * icon = lv_image_create(quick_drawer_expansion_box);
        lv_obj_set_pos(icon, x, 0);
        lv_obj_add_flag(icon, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t * name = lv_label_create(quick_drawer_expansion_box);
        lv_label_set_text(name, row2_names[i]);
        lv_obj_add_style(name, &style_theme_text_primary, 0);
        lv_obj_set_style_text_font(name, &lv_font_montserrat_12, 0);
        lv_obj_set_width(name, BOARD_SCALE_PX(84));
        lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(name, x, QUICK_DRAWER_TOGGLE_ICON_PX + 2);

        int slot = QD_TOGGLE_AIRPLAY + i;
        quick_drawer_toggle_state[slot] = lv_label_create(quick_drawer_expansion_box);
        lv_obj_set_width(quick_drawer_toggle_state[slot], BOARD_SCALE_PX(84));
        lv_obj_set_style_text_align(quick_drawer_toggle_state[slot], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(quick_drawer_toggle_state[slot], &lv_font_montserrat_12, 0);
        lv_obj_set_pos(quick_drawer_toggle_state[slot], x, QUICK_DRAWER_TOGGLE_ICON_PX + 24);
        lv_label_set_text(quick_drawer_toggle_state[slot], "Off");
        lv_obj_set_style_text_color(quick_drawer_toggle_state[slot], lv_color_hex(0x8d918f), 0);

        switch (slot) {
            case QD_TOGGLE_AIRPLAY:
                quick_drawer_airplay_icon = icon;
                lv_obj_add_event_cb(icon, quick_drawer_airplay_event_cb, LV_EVENT_CLICKED, NULL);
                break;
            case QD_TOGGLE_DLNA:
                quick_drawer_dlna_icon = icon;
                lv_obj_add_event_cb(icon, quick_drawer_dlna_event_cb, LV_EVENT_CLICKED, NULL);
                break;
            case QD_TOGGLE_GAPLESS:
                quick_drawer_gapless_icon = icon;
                lv_obj_add_event_cb(icon, quick_drawer_gapless_event_cb, LV_EVENT_CLICKED, NULL);
                break;
            case QD_TOGGLE_RC:
                quick_drawer_rc_icon = icon;
                lv_obj_add_event_cb(icon, quick_drawer_rc_event_cb, LV_EVENT_CLICKED, NULL);
                break;
            default: break;
        }
        lv_image_set_src(icon, quick_drawer_toggle_src((qd_toggle_t) slot, false));
    }

    for (int i = 0; i < quick_drawer_plugin_toggle_count; i++) {
        int32_t x = BOARD_SCALE_PX(label_x[i]);
        int32_t row_y = QUICK_DRAWER_TOGGLE_ROW_PITCH;
        bool on = plugin_manager_get_quick_toggle_value(i);

        lv_obj_t * icon = lv_image_create(quick_drawer_expansion_box);
        lv_image_set_src(icon, quick_drawer_plugin_toggle_src(i, on));
        lv_obj_set_pos(icon, x, row_y);
        lv_obj_add_flag(icon, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(icon, quick_drawer_plugin_toggle_event_cb, LV_EVENT_CLICKED,
                            (void *) (intptr_t) i);
        quick_drawer_plugin_icon[i] = icon;

        lv_obj_t * name = lv_label_create(quick_drawer_expansion_box);
        lv_label_set_text(name, plugin_manager_get_quick_toggle_label(i));
        lv_obj_add_style(name, &style_theme_text_primary, 0);
        lv_obj_set_style_text_font(name, &lv_font_montserrat_12, 0);
        lv_obj_set_width(name, BOARD_SCALE_PX(84));
        lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(name, x, row_y + QUICK_DRAWER_TOGGLE_ICON_PX + 2);

        int slot = QD_TOGGLE_COUNT + i;
        quick_drawer_toggle_state[slot] = lv_label_create(quick_drawer_expansion_box);
        lv_obj_set_width(quick_drawer_toggle_state[slot], BOARD_SCALE_PX(84));
        lv_obj_set_style_text_align(quick_drawer_toggle_state[slot], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(quick_drawer_toggle_state[slot], &lv_font_montserrat_12, 0);
        lv_obj_set_pos(quick_drawer_toggle_state[slot], x, row_y + QUICK_DRAWER_TOGGLE_ICON_PX + 24);
        quick_drawer_set_toggle_state_text(slot, on, plugin_manager_get_quick_toggle_state_text(i, on));
    }

    quick_drawer_expansion_full =
        QUICK_DRAWER_TOGGLE_ROW_PITCH * (quick_drawer_plugin_toggle_count > 0 ? 2 : 1);

    /* Row 1: screen brightness -- real control, via the standard Linux
     * backlight sysfs class (backlight.h), no dedicated slider-track asset
     * in this theme so it reuses the (generic-looking) volume slider's own.
     * Swapped ahead of volume below at the user's request -- Y offsets
     * moved to brightness_container's new top (268: deltas 21/29/25
     * unchanged), volume's moved to what used to be brightness's slot. */
    quick_drawer_brightness_icon = lv_image_create(quick_drawer);
    const void * brightness = asset_decoded_image_open(&quick_drawer_brightness_image, "pull_down/blk.png")
                            ? asset_decoded_image_source(&quick_drawer_brightness_image) : NULL;
    lv_image_set_src(quick_drawer_brightness_icon, brightness ? brightness : asset_path("pull_down/blk.png"));
    lv_obj_align(quick_drawer_brightness_icon, LV_ALIGN_TOP_LEFT, BOARD_SCALE_PX(55), BOARD_SCALE_PY(259));

    /* Fixed size, so the percentage does not follow the Font Size tier, and
     * centred on its own track rather than pinned to a hardcoded Y. */
    int32_t slider_pct_h = lv_font_get_line_height(&app_font_player_meta);
    int32_t slider_pct_dy = (SLIDER_TRACK_HEIGHT - slider_pct_h) / 2;

    quick_drawer_brightness_label = lv_label_create(quick_drawer);
    lv_obj_add_style(quick_drawer_brightness_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(quick_drawer_brightness_label, &app_font_player_meta, 0);
    lv_obj_align(quick_drawer_brightness_label, LV_ALIGN_TOP_RIGHT, -BOARD_SCALE_PX(52),
                 BOARD_SCALE_PY(267) + slider_pct_dy);

    /* Dynamically sizes slider width based on the maximum width of the percentage
     * label ("100") to prevent horizontal overlap. */
    lv_point_t brightness_label_size;
    lv_text_get_size(&brightness_label_size, "100", &app_font_player_meta,
                     0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int32_t brightness_label_max_w = brightness_label_size.x;
    /* The percentage is anchored at x=428. Leave a 16px visual gap before
     * its actual text box and keep the knob's overhang inside that gap. */
    int32_t brightness_track_w = BOARD_SCALE_PX(428) - brightness_label_max_w - BOARD_SCALE_PX(16) - BOARD_SCALE_PX(103);
    if (brightness_track_w > BOARD_SCALE_PX(300)) brightness_track_w = BOARD_SCALE_PX(300);
    if (brightness_track_w < BOARD_SCALE_PX(120)) brightness_track_w = BOARD_SCALE_PX(120); /* sane floor so the track never collapses to nothing */

    quick_drawer_brightness_track = lv_slider_create(quick_drawer);
    lv_obj_set_size(quick_drawer_brightness_track, brightness_track_w, SLIDER_TRACK_HEIGHT);
    lv_obj_align(quick_drawer_brightness_track, LV_ALIGN_TOP_LEFT, BOARD_SCALE_PX(103), BOARD_SCALE_PY(267));
    /* Full 0-100 -- backlight.c now maps this logical range to its own safe
     * raw range internally (see backlight.h's own comment), so the slider
     * itself is free to show a clean, honest 0%-100% again. */
    lv_slider_set_range(quick_drawer_brightness_track, 0, 100);
    /* Initial value set below by refresh_quick_drawer_brightness() (also
     * called on every open_quick_drawer(), see its own comment) --
     * defined here just so it runs once at build time too, same as
     * every other quick-drawer widget's own initial state. */
    lv_obj_set_style_bg_color(quick_drawer_brightness_track, lv_color_black(), LV_PART_MAIN);
    lv_obj_add_style(quick_drawer_brightness_track, gui_theme_accent_style(), LV_PART_INDICATOR);
    lv_obj_add_style(quick_drawer_brightness_track, gui_theme_accent_knob_style(), LV_PART_KNOB);
    /* Configures rail styling to prevent visual artifacts on the track edge. */
    configure_native_slider_rail(quick_drawer_brightness_track);
    lv_obj_set_style_bg_opa(quick_drawer_brightness_track, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_width(quick_drawer_brightness_track, SLIDER_KNOB_SIZE, LV_PART_KNOB);
    lv_obj_set_style_height(quick_drawer_brightness_track, SLIDER_KNOB_SIZE, LV_PART_KNOB);
    lv_obj_add_event_cb(quick_drawer_brightness_track, quick_drawer_brightness_changed_cb,
                        LV_EVENT_ALL, NULL);
    if (!brightness_hw_apply_timer) {
        brightness_hw_apply_timer = lv_timer_create(brightness_hw_apply_timer_cb,
                                                    BRIGHTNESS_HW_APPLY_INTERVAL_MS, NULL);
        if (brightness_hw_apply_timer) lv_timer_pause(brightness_hw_apply_timer);
    }

    /* Stock's drawer gives this control an explicit 436x100 touch rectangle.
     * Its neighboring icon and percentage are display-only, so matching that
     * generous vertical capture area does not steal another control's tap. */
    lv_obj_set_ext_click_area(quick_drawer_brightness_track, BOARD_SCALE_PX(44));

    refresh_quick_drawer_brightness();

    /* Row 2: volume, in what used to be brightness's slot (see the "Row 1"
     * comment above for why these two swapped). */
    lv_obj_t * volume_icon = lv_image_create(quick_drawer);
    lv_image_set_src(volume_icon, asset_path("volume/vol.png"));
    lv_obj_align(volume_icon, LV_ALIGN_TOP_LEFT, BOARD_SCALE_PX(55), BOARD_SCALE_PY(346));
    quick_drawer_volume_track = lv_slider_create(quick_drawer);
    lv_obj_set_size(quick_drawer_volume_track, brightness_track_w, SLIDER_TRACK_HEIGHT);
    lv_obj_align(quick_drawer_volume_track, LV_ALIGN_TOP_LEFT, BOARD_SCALE_PX(103), BOARD_SCALE_PY(354));
    lv_slider_set_range(quick_drawer_volume_track, 0, 100);
    lv_obj_set_style_bg_color(quick_drawer_volume_track, lv_color_black(), LV_PART_MAIN);
    lv_obj_add_style(quick_drawer_volume_track, gui_theme_accent_style(), LV_PART_INDICATOR);
    lv_obj_add_style(quick_drawer_volume_track, gui_theme_accent_knob_style(), LV_PART_KNOB);
    configure_native_slider_rail(quick_drawer_volume_track);
    /* Its sibling brightness track (just above) already had all of these --
     * this one was missing every one of them, leaving a smaller, harder-to-
     * grab knob and a touch-catch area confined to the bare track box
     * instead of the same forgiving margin, which is the real reason
     * dragging this one felt worse than the popup's own volume slider
     * (that mismatch, not the audio-request throttling already fixed
     * earlier, was the remaining gap). */
    lv_obj_set_style_bg_opa(quick_drawer_volume_track, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_width(quick_drawer_volume_track, SLIDER_KNOB_SIZE, LV_PART_KNOB);
    lv_obj_set_style_height(quick_drawer_volume_track, SLIDER_KNOB_SIZE, LV_PART_KNOB);
    lv_obj_set_ext_click_area(quick_drawer_volume_track, BOARD_SCALE_PX(44));
    lv_obj_add_event_cb(quick_drawer_volume_track, quick_drawer_volume_event_cb, LV_EVENT_ALL, NULL);
    lv_slider_set_value(quick_drawer_volume_track, gui_player_get_volume_percent(), LV_ANIM_OFF);
    quick_drawer_volume_label = lv_label_create(quick_drawer);
    lv_obj_add_style(quick_drawer_volume_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(quick_drawer_volume_label, &app_font_player_meta, 0);
    lv_obj_align(quick_drawer_volume_label, LV_ALIGN_TOP_RIGHT, -BOARD_SCALE_PX(52),
                 BOARD_SCALE_PY(354) + slider_pct_dy);
    lv_label_set_text_fmt(quick_drawer_volume_label, "%d", gui_player_get_volume_percent());

    /* Everything below the expansion box slides down by whatever height it
     * currently has. Registered with the collapsed y each object was just
     * built at, so quick_drawer_apply_expansion() never has to re-derive
     * any of this geometry. Both slider hit tests read live
     * lv_obj_get_coords(), so they follow these automatically. */
    quick_drawer_shift_count = 0;
    quick_drawer_register_shift_obj(quick_drawer_expansion_handle, BOARD_SCALE_PY(219));
    quick_drawer_register_shift_obj(brightness_container, BOARD_SCALE_PY(238));
    quick_drawer_register_shift_obj(quick_drawer_brightness_icon, BOARD_SCALE_PY(259));
    quick_drawer_register_shift_obj(quick_drawer_brightness_label, BOARD_SCALE_PY(267) + slider_pct_dy);
    quick_drawer_register_shift_obj(quick_drawer_brightness_track, BOARD_SCALE_PY(267));
    quick_drawer_register_shift_obj(volume_container, BOARD_SCALE_PY(325));
    quick_drawer_register_shift_obj(volume_icon, BOARD_SCALE_PY(346));
    quick_drawer_register_shift_obj(quick_drawer_volume_label, BOARD_SCALE_PY(354) + slider_pct_dy);
    quick_drawer_register_shift_obj(quick_drawer_volume_track, BOARD_SCALE_PY(354));

    /* Mini now-playing card: cover thumbnail (left), track title/artist
     * (right of it), and transport controls (bottom row). Sized to fit the
     * second panel's bounds with a balanced bottom margin. style_theme_card_bg
     * is the same theme-aware panel-surface-plus-border style used for cards
     * elsewhere (build_setting_slider_card(), popups) -- gives this card a
     * lighter-than-backdrop fill and a subtle, theme-mixed border outline
     * instead of sitting fully transparent over the drawer's own black/gray
     * backdrop artwork. */
    quick_drawer_card = lv_obj_create(quick_drawer);
    lv_obj_t * card = quick_drawer_card;
    lv_obj_remove_style_all(card);
    lv_obj_set_pos(card, BOARD_SCALE_PX(34), BOARD_SCALE_PY(411));
    /* Grows downward into the space the taller panel just freed (441..765,
     * 20px above the panel's new bottom edge). The cover/title/artist block
     * at the top is untouched; the extra height goes to the transport row. */
    lv_obj_set_size(card, QUICK_DRAWER_CARD_W, QUICK_DRAWER_CARD_H);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x111712), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, BOARD_SCALE_PX(24), 0);
    lv_obj_set_style_clip_corner(card, true, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    /* Cover thumbnail -- reuses whatever the Player screen already decoded
     * (gui_player_get_current_cover_dsc(), same accessor gui_lock_screen.c
     * already relies on for its own small cover display) rather than a
     * fresh disk read/decode. COVER_ART_WIDTH is the known native size of
     * both that buffer and its default-cover fallback PNG, so one fixed
     * scale factor is correct for either source; gui_shell_refresh_quick_
     * drawer_cover() (called on every track/cover update) fills the src in. */
    quick_drawer_cover_frame = lv_obj_create(card);
    lv_obj_remove_style_all(quick_drawer_cover_frame);
    lv_obj_set_size(quick_drawer_cover_frame, QUICK_DRAWER_CARD_W, QUICK_DRAWER_CARD_H);
    lv_obj_align(quick_drawer_cover_frame, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_remove_flag(quick_drawer_cover_frame, LV_OBJ_FLAG_SCROLLABLE);
    quick_drawer_cover_img = lv_image_create(quick_drawer_cover_frame);
    lv_obj_remove_flag(quick_drawer_cover_img, LV_OBJ_FLAG_CLICKABLE);
    lv_image_set_src(quick_drawer_cover_img, asset_path("playing_plane/default_cover_565.png"));
    quick_drawer_fit_cover();

    int32_t title_h = row_label_bounded_height(&app_font_player_title);
    int32_t artist_h = row_label_bounded_height(&app_font_player_meta);
    int32_t text_gap = BOARD_SCALE_PX(4);
    int32_t text_top = BOARD_SCALE_PY(40);
    int32_t controls_bottom = BOARD_SCALE_PY(24);

    lv_obj_t * band = lv_obj_create(card);
    lv_obj_remove_style_all(band);
    lv_obj_set_size(band, lv_pct(100), lv_pct(100));
    lv_obj_align(band, LV_ALIGN_TOP_MID, 0, 0);
    /* No tint: the frosted source is already darkened, and blending a scrim
     * over it re-quantizes the dithered RGB565 into visible bands. */
    lv_obj_set_style_bg_opa(band, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(band, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    int32_t text_width = QUICK_DRAWER_CARD_W - 2 * BOARD_SCALE_PX(15);

    quick_drawer_title_label = lv_label_create(band);
    lv_label_set_text(quick_drawer_title_label, "No track loaded");
    lv_obj_add_style(quick_drawer_title_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(quick_drawer_title_label, &app_font_player_title, 0);
    lv_obj_set_width(quick_drawer_title_label, text_width);
    lv_obj_set_style_text_align(quick_drawer_title_label, LV_TEXT_ALIGN_CENTER, 0);
    row_label_apply_bounded_height(quick_drawer_title_label, &app_font_player_title);
    row_label_enable_marquee(quick_drawer_title_label);
    lv_obj_align(quick_drawer_title_label, LV_ALIGN_TOP_MID, 0, text_top);

    quick_drawer_artist_label = lv_label_create(band);
    lv_label_set_text(quick_drawer_artist_label, "");
    lv_obj_add_style(quick_drawer_artist_label, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(quick_drawer_artist_label, &app_font_player_meta, 0);
    lv_obj_set_width(quick_drawer_artist_label, text_width);
    lv_obj_set_style_text_align(quick_drawer_artist_label, LV_TEXT_ALIGN_CENTER, 0);
    row_label_apply_bounded_height(quick_drawer_artist_label, &app_font_player_meta);
    row_label_enable_marquee(quick_drawer_artist_label);
    lv_obj_align(quick_drawer_artist_label, LV_ALIGN_TOP_MID, 0, text_top + title_h + text_gap);

    quick_drawer_album_label = lv_label_create(band);
    lv_label_set_text(quick_drawer_album_label, "");
    lv_obj_add_style(quick_drawer_album_label, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(quick_drawer_album_label, &app_font_player_meta, 0);
    lv_obj_set_width(quick_drawer_album_label, text_width);
    lv_obj_set_style_text_align(quick_drawer_album_label, LV_TEXT_ALIGN_CENTER, 0);
    row_label_apply_bounded_height(quick_drawer_album_label, &app_font_player_meta);
    row_label_enable_marquee(quick_drawer_album_label);
    lv_obj_align(quick_drawer_album_label, LV_ALIGN_TOP_MID, 0,
                 text_top + title_h + text_gap + artist_h + text_gap);

    quick_drawer_format_label = lv_label_create(band);
    lv_label_set_text(quick_drawer_format_label, "");
    lv_obj_add_style(quick_drawer_format_label, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(quick_drawer_format_label, &app_font_player_meta, 0);
    lv_obj_set_style_text_align(quick_drawer_format_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_radius(quick_drawer_format_label, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(quick_drawer_format_label, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(quick_drawer_format_label, LV_OPA_20, 0);
    lv_obj_set_style_pad_hor(quick_drawer_format_label, BOARD_SCALE_PX(14), 0);
    lv_obj_set_style_pad_ver(quick_drawer_format_label, BOARD_SCALE_PY(4), 0);
    lv_obj_align(quick_drawer_format_label, LV_ALIGN_TOP_MID, 0,
                 text_top + title_h + text_gap + artist_h + text_gap + artist_h
                 + BOARD_SCALE_PY(12));

    /* Transport row: order/prev/play/next/favorite, all five in one row --
     * matching the stock drawer exactly (shuffle-style icon leftmost,
     * favorite heart rightmost, same as the reference screenshot). This
     * copy of the order icon is a visual-only mirror of the main player
     * screen's own (see order_icon_event_cb) -- not independently
     * clickable, just kept in sync so the drawer doesn't show a stale mode. */
    lv_obj_t * controls_row = lv_obj_create(band);
    /* 84, not 70 -- btn_play.png/btn_pause.png are 84x84 (confirmed via the
     * actual asset files), and a shorter row was clipping the top/bottom of
     * that icon, confirmed on a real device. */
    lv_obj_set_size(controls_row, lv_pct(100), BOARD_SCALE_PY(86));
    lv_obj_align(controls_row, LV_ALIGN_BOTTOM_MID, 0, -controls_bottom);
    lv_obj_set_style_bg_opa(controls_row, 0, 0);
    lv_obj_set_style_border_width(controls_row, 0, 0);
    lv_obj_set_style_pad_all(controls_row, 0, 0);
    lv_obj_remove_flag(controls_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t * prev_btn = lv_image_create(controls_row);
    lv_image_set_src(prev_btn, asset_path("playing_plane/btn_prev.png"));
    lv_obj_set_pos(prev_btn, BOARD_SCALE_PX(84), BOARD_SCALE_PY(23));
    lv_obj_set_ext_click_area(prev_btn, BOARD_SCALE_PX(4));
    lv_obj_add_flag(prev_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(prev_btn, prev_btn_event_cb, LV_EVENT_CLICKED, NULL);

    quick_drawer_play_btn = lv_image_create(controls_row);
    lv_image_set_src(quick_drawer_play_btn, gui_player_play_btn_image_src(audio_is_playing()));
    lv_obj_set_pos(quick_drawer_play_btn, BOARD_SCALE_PX(164), (BOARD_SCALE_PY(86) - 84) / 2);
    lv_image_set_scale(quick_drawer_play_btn, (BOARD_SCALE_PY(86) * LV_SCALE_NONE + 42) / 84);
    lv_obj_add_flag(quick_drawer_play_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(quick_drawer_play_btn, play_btn_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t * next_btn = lv_image_create(controls_row);
    lv_image_set_src(next_btn, asset_path("playing_plane/btn_next.png"));
    lv_obj_set_pos(next_btn, BOARD_SCALE_PX(290), BOARD_SCALE_PY(23));
    lv_obj_set_ext_click_area(next_btn, BOARD_SCALE_PX(4));
    lv_obj_add_flag(next_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(next_btn, next_btn_event_cb, LV_EVENT_CLICKED, NULL);

    gui_shell_refresh_quick_drawer_cover();

    /* Render the complex live control tree once while still under the boot
     * splash. Drag/snap motion uses this one opaque RGB565 image; controls
     * become live again the instant the motion settles. */
    quick_drawer_rebuild_snapshot();
}

void refresh_clock_label(void) {
    struct tm tm_info;
    app_clock_localtime(&tm_info);
    char buf[8];
    /* %I (12h) zero-pads to 2 digits just like %H (24h) does -- "01".."12",
     * never a single digit -- so buf is always "HH:MM" (5 chars) either
     * way, mapping 1:1 onto the 5 fixed slots with no leading-slot-hiding
     * needed here, unlike the volume/battery readouts. */
    strftime(buf, sizeof(buf), current_settings.clock_24h ? "%H:%M" : "%I:%M", &tm_info);

    for (int i = 0; i < 5; i++) {
        char glyph[2] = { buf[i], '\0' };
        lv_label_set_text(clock_topbar_digit[i], glyph);
    }

    bool ampm_was_hidden = lv_obj_has_flag(clock_topbar_ampm, LV_OBJ_FLAG_HIDDEN);
    if (current_settings.clock_24h) {
        lv_obj_add_flag(clock_topbar_ampm, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(clock_topbar_ampm, tm_info.tm_hour < 12 ? "AM" : "PM");
        lv_obj_remove_flag(clock_topbar_ampm, LV_OBJ_FLAG_HIDDEN);
    }
    /* Center alignment follows the clock's width automatically. Recheck
     * room for the optional codec badge when AM/PM changes that width. */
    if (ampm_was_hidden != lv_obj_has_flag(clock_topbar_ampm, LV_OBJ_FLAG_HIDDEN)) {
        sync_bt_codec_status_icon();
    }
}

/* Split out of gui_shell_init() so gui_reload.c's in-process UI reload can
 * rebuild Home/the status bar/the quick drawer without also re-triggering
 * start_bt_dac_startup_reapply_if_needed()/start_refresh_bt_icon() below --
 * those are Bluetooth-adjacent startup side effects (the latter spawns a
 * background pthread with no idempotency guard) a reload must never repeat.
 * gui_shell_init() itself (below) still calls this first, then those two,
 * in the exact same order as before this split -- boot behavior unchanged. */
void gui_shell_build_screens(uint32_t screen_width, uint32_t screen_height) {
    (void) screen_width;
    (void) screen_height;
    dac_home_screen = build_dac_home_screen();
    home_screen = build_home_screen();
    build_status_bar();
    build_home_indicator_bar();
    build_quick_drawer();
    refresh_clock_label();
    refresh_battery_topbar();
    refresh_wifi_icon();
    refresh_play_pause_topbar();
    refresh_headphone_icon();
    poll_usb_audio_output();

    if (!device_config_get_volume_warn_threshold(&volume_warn_threshold_percent)) {
        volume_warn_threshold_percent = -1;
    }
    refresh_volume_topbar((int32_t) (audio_get_volume() * 100.0f));

    gui_shell_reset_drag_state();
    if (!quick_drawer_drag_timer) {
        quick_drawer_drag_timer = lv_timer_create(poll_quick_drawer_drag, LV_DEF_REFR_PERIOD, NULL);
    }
    gui_shell_install_indev_hooks(NULL);
}

/* Deletes every screen/top-layer object gui_shell.c itself owns -- for
 * gui_reload.c's in-process UI reload, so gui_shell_build_screens() can
 * rebuild these from a clean slate without leaking the old objects. Only
 * three root containers need an explicit lv_obj_delete(): status_bar_band/
 * home_indicator_band/quick_drawer own every other status-bar/quick-drawer
 * child (clock digits, battery icon, wifi/bt icons, sliders, ...) as an
 * LVGL child, so deleting the root recursively frees them -- no need to
 * null each child pointer individually, since build_status_bar()/build_
 * home_indicator_bar()/build_quick_drawer() reassign every one of them
 * again immediately after, with nothing running in between that could read
 * a stale pointer. quick_drawer_motion_image is the one exception -- a
 * transient drag-snapshot object created directly under lv_layer_top(),
 * not as quick_drawer's own child, so it needs its own explicit delete
 * when a mid-drag reload catches it still present. */
void gui_shell_teardown(void) {
    if (quick_drawer_brightness_track && lv_slider_is_dragged(quick_drawer_brightness_track)) {
        int percent = (int) lv_slider_get_value(quick_drawer_brightness_track);
        brightness_hw_pending = percent;
        current_settings.brightness_percent = percent;
        settings_save(&current_settings);
    }
    brightness_hw_apply_pending();
    brightness_drag_active = false;
    if (brightness_hw_apply_timer) {
        lv_timer_delete(brightness_hw_apply_timer);
        brightness_hw_apply_timer = NULL;
    }
    brightness_hw_pending = -1;
    volume_topbar_last_len = -1;
    volume_topbar_last_digits[0] = '\0';
    if (quick_drawer_motion_image) {
        lv_obj_delete(quick_drawer_motion_image);
        quick_drawer_motion_image = NULL;
    }
    if (quick_drawer_motion_buf) {
        lv_draw_buf_destroy(quick_drawer_motion_buf);
        quick_drawer_motion_buf = NULL;
    }
    quick_drawer_bitmap_motion = false;
    /* hw_volume_coalesce_teardown() can't re-read a live slider itself (it
     * doesn't know which widget owns the drag) -- caller-side step, same
     * as gui_player_teardown()'s own equivalent for the volume popup, done
     * here while quick_drawer_volume_track still exists (just below). */
    if (quick_drawer_volume_hv.drag_active && quick_drawer_volume_track)
        hw_volume_coalesce_drag_update(&quick_drawer_volume_hv, (int) lv_slider_get_value(quick_drawer_volume_track));
    hw_volume_coalesce_teardown(&quick_drawer_volume_hv);
    if (quick_drawer) {
        lv_obj_delete(quick_drawer);
        quick_drawer = NULL;
    }
    quick_drawer_brightness_icon = NULL;
    quick_drawer_cover_img = NULL; /* child of quick_drawer -- already deleted by the delete above */
    quick_drawer_cover_frame = NULL;
    quick_drawer_volume_track = NULL;
    quick_drawer_volume_label = NULL;
    for (int i = 0; i < QUICK_DRAWER_TOGGLE_SLOTS; i++) quick_drawer_toggle_state[i] = NULL;
    /* All children of quick_drawer, already deleted with it just above --
     * cleared so a rebuild (gui_reload.c) cannot act on a dangling one, and
     * so the expansion state machine no-ops until it is rebuilt. */
    quick_drawer_expansion_box = NULL;
    quick_drawer_expansion_handle = NULL;
    quick_drawer_card = NULL;
    quick_drawer_airplay_icon = NULL;
    quick_drawer_dlna_icon = NULL;
    quick_drawer_rc_icon = NULL;
    for (int i = 0; i < PLUGIN_MAX_QUICK_TOGGLES; i++) quick_drawer_plugin_icon[i] = NULL;
    quick_drawer_plugin_toggle_count = 0;
    quick_drawer_shift_count = 0;
    quick_drawer_expansion_full = 0;
    quick_drawer_expansion_y = 0;
    quick_drawer_expanded = false;
    quick_drawer_expansion_dragging = false;
    quick_drawer_expansion_moved = false;
    asset_decoded_image_close(&quick_drawer_bg_image);
    asset_decoded_image_close(&quick_drawer_brightness_image);
    for (int i = 0; i < QD_TOGGLE_COUNT; i++) asset_decoded_image_close(&qd_toggle_on_img[i]);
    for (int i = 0; i < PLUGIN_MAX_QUICK_TOGGLES; i++) asset_decoded_image_close(&qd_plugin_toggle_on_img[i]);
    if (status_bar_band) {
        lv_obj_delete(status_bar_band);
        status_bar_band = NULL;
    }
    if (home_indicator_band) {
        lv_obj_delete(home_indicator_band);
        home_indicator_band = NULL;
    }
    if (dac_home_screen) {
        lv_obj_delete(dac_home_screen);
        dac_home_screen = NULL;
    }
    if (home_screen) {
        lv_obj_delete(home_screen);
        home_screen = NULL;
    }
}

void gui_shell_refresh_static_assets(void) {
    if (!quick_drawer) return;
    asset_decoded_image_close(&quick_drawer_brightness_image);
    const void * brightness = asset_decoded_image_open(&quick_drawer_brightness_image, "pull_down/blk.png")
                            ? asset_decoded_image_source(&quick_drawer_brightness_image) : NULL;
    if (quick_drawer_brightness_icon)
        lv_image_set_src(quick_drawer_brightness_icon,
                         brightness ? brightness : asset_path("pull_down/blk.png"));
    /* The four "on" toggle icons are decoded copies too, so a changed asset
     * on disk only reaches them through a re-decode -- and that re-decode
     * frees the buffers the widgets currently point at, so this must also
     * re-point them, which is exactly what this does. */
    gui_shell_refresh_quick_drawer_toggle_accent();
    quick_drawer_mark_snapshot_dirty();
}

void gui_shell_refresh_home(void) {
    lv_obj_t * old = home_screen;
    lv_obj_t * fresh = build_home_screen();
    if (!fresh) return;
    home_screen = fresh;
    gui_navigation_replace_home(old, fresh);
    if (old) lv_obj_delete(old);
}

void gui_shell_init(uint32_t screen_width, uint32_t screen_height) {
    gui_shell_build_screens(screen_width, screen_height);
    start_bt_source_codec_reconcile_if_needed();
    start_bt_dac_startup_reapply_if_needed();
#ifndef HOST_BUILD
    boot_checkpoint("start_refresh_bt_icon about to be called");
#endif
    start_refresh_bt_icon();
#ifndef HOST_BUILD
    boot_checkpoint("start_refresh_bt_icon done");
#endif
}



void gui_shell_poll(void) {
    poll_wifi_status();
    poll_usb_audio_output();
    poll_sleep_timer();
    poll_wifi_toggle();
    poll_bt_toggle();
    start_bt_source_codec_reconcile_if_needed();
    poll_bt_source_codec_reconcile();
    poll_bt_dac_startup_reapply();
    poll_bt_apply_output_settings();
    poll_refresh_bt_icon();
    start_bt_source_codec_reconcile_if_needed();
    poll_bt_monitor_lifecycle();
    poll_bt_reconnect();
}

void gui_shell_resume_connections(bool wifi_was_on, bool bt_was_on) {
#ifndef HOST_BUILD
    if (wifi_was_on && !wifi_control_is_enabled() && !wifi_toggle_active) {
        wifi_toggle_active = true;
        wifi_toggle_is_radio_suspend = false; /* restoring, not the suspend disable itself */
        atomic_store_explicit(&wifi_toggle_done_flag, false, memory_order_relaxed);
        wifi_toggle_target_enabled = true;
        if (pthread_create(&wifi_toggle_thread, NULL, wifi_toggle_thread_func, NULL) != 0)
            wifi_toggle_active = false;
    }
    if (bt_was_on && !bt_is_powered_cached && !bt_toggle_active) {
        /* Resume is a new power-on cycle even if no status poll ran while
         * suspended to observe the intermediate OFF state. */
        bt_reconnect_observed_power_valid = true;
        bt_reconnect_observed_powered = false;
        bt_reconnect_cycle_suppressed = false;
        bt_toggle_active = true;
        bt_toggle_target_enabled = true;
        bt_toggle_followup_pending = false;
        atomic_store_explicit(&bt_toggle_done_flag, false, memory_order_relaxed);
        if (pthread_create(&bt_toggle_thread, NULL, bt_toggle_thread_func, BT_TOGGLE_TARGET_ON) != 0) {
            bt_toggle_active = false;
        }
    }
#else
    (void) wifi_was_on;
    (void) bt_was_on;
#endif
}

void gui_shell_suspend_connections(bool * wifi_was_on, bool * bt_was_on) {
#ifndef HOST_BUILD
    gui_shell_cancel_bt_reconnect();
    *wifi_was_on = wifi_control_is_enabled();
    *bt_was_on = bt_is_powered_cached;
    if (*wifi_was_on && !wifi_toggle_active) {
        wifi_toggle_active = true;
        wifi_toggle_is_radio_suspend = true; /* transient power-save disable, not a user-requested one -- see its own comment */
        atomic_store_explicit(&wifi_toggle_done_flag, false, memory_order_relaxed);
        wifi_toggle_target_enabled = false;
        if (pthread_create(&wifi_toggle_thread, NULL, wifi_toggle_thread_func, NULL) != 0)
            wifi_toggle_active = false;
    }
    if (*bt_was_on && !bt_toggle_active) {
        bt_toggle_active = true;
        bt_toggle_target_enabled = false;
        bt_toggle_followup_pending = false;
        atomic_store_explicit(&bt_toggle_done_flag, false, memory_order_relaxed);
        if (pthread_create(&bt_toggle_thread, NULL, bt_toggle_thread_func, BT_TOGGLE_TARGET_OFF) != 0)
            bt_toggle_active = false;
    }
#else
    (void) wifi_was_on;
    (void) bt_was_on;
#endif
}

#define VISIBLE_STATUS_POLL_TICKS 4
#define WIFI_POLL_TICKS 10

static int visible_status_poll_tick_counter = 0;
static int wifi_poll_tick_counter = 0;

void gui_shell_update_topbar(bool screen_just_woke) {
    /* Cheap startup-only marker check.  This runs every 500ms so the first
     * authoritative Bluetooth refresh begins promptly when bt_init finishes,
     * without delaying the rest of the UI or polling BlueZ prematurely.  It
     * also releases a pending Bluetooth-DAC apply behind the same gate. */
    if (!bt_startup_ready) {
        start_bt_dac_startup_reapply_if_needed();
        start_refresh_bt_icon();
    }

    if (screen_just_woke || ++visible_status_poll_tick_counter >= VISIBLE_STATUS_POLL_TICKS) {
        visible_status_poll_tick_counter = 0;
        refresh_clock_label();
        refresh_battery_topbar();
        refresh_headphone_icon();
        poll_usb_audio_output();
    }
    refresh_play_pause_topbar();
    if (screen_just_woke || ++wifi_poll_tick_counter >= WIFI_POLL_TICKS) {
        wifi_poll_tick_counter = 0;
        refresh_wifi_icon();
        start_bt_dac_startup_reapply_if_needed();
        start_refresh_bt_icon();
    }
}

void gui_shell_resume_fast_timers(void) {
    if (quick_drawer_drag_timer && lv_timer_get_paused(quick_drawer_drag_timer)) {
#ifdef UI_GESTURE_TRACE
        printf("[GESTURE_TRACE] resume_fast_timers: quick_drawer_drag_timer\n");
#endif
        lv_timer_resume(quick_drawer_drag_timer);
        lv_timer_ready(quick_drawer_drag_timer);
    }
}

void gui_shell_reset_drag_state(void) {
#ifdef UI_GESTURE_TRACE
    printf("[GESTURE_TRACE] reset_drag_state called (was_pressed=%d, home_tracking=%d, drawer_tracking=%d, player_tracking=%d)\n",
           quick_drawer_was_pressed, home_swipe_tracking, quick_drawer_drag_tracking, player_swipe_tracking);
#endif
    quick_drawer_was_pressed = false;
    quick_drawer_drag_tracking = false;
    drag_adjust_press_owned = false;
    quick_drawer_drag_claimed = false;
    quick_drawer_last_velocity = 0;

    home_swipe_candidate = false;
    home_swipe_tracking = false;
    home_swipe_just_confirmed = false;
    if (home_swipe_ctx) {
        slide_transition_cancel(&home_swipe_ctx);
    }

    /* Cancel any active drawer animation or motion and restore deterministic closed state */
    lv_anim_delete(quick_drawer, quick_drawer_anim_y_cb);
    if (quick_drawer_bitmap_motion || quick_drawer_direct_motion) {
        quick_drawer_open = false;
        quick_drawer_finish_bitmap_motion();
    }

    player_swipe_candidate = false;
    player_swipe_tracking = false;
    player_swipe_just_confirmed = false;
    if (player_swipe_ctx) {
        slide_transition_cancel(&player_swipe_ctx);
    }
    back_swipe_candidate = false;
    back_swipe_owns_press = false;
    back_swipe_tracking = false;
    back_swipe_just_confirmed = false;
    back_swipe_target_scr = NULL;
    if (back_swipe_ctx) {
        slide_transition_cancel(&back_swipe_ctx);
    }
    s_last_raw_pointer_state = LV_INDEV_STATE_RELEASED;
    s_require_release_after_wake = true;
    if (quick_drawer_drag_timer) {
        lv_timer_pause(quick_drawer_drag_timer);
    }
}

void gui_shell_player_swipe_recover(void * ctx) {
    slide_transition_ctx_t * sctx = (slide_transition_ctx_t *) ctx;
    if (sctx == player_swipe_ctx) player_swipe_ctx = NULL;
    player_swipe_tracking = false;
    player_swipe_candidate = false;
    player_swipe_just_confirmed = false;
    if (sctx == back_swipe_ctx) back_swipe_ctx = NULL;
    back_swipe_tracking = false;
    back_swipe_candidate = false;
    back_swipe_owns_press = false;
    back_swipe_just_confirmed = false;
    back_swipe_target_scr = NULL;
    /* home_swipe_ctx is never driven through the compositor by its OWN
     * begin_slide_transition_ex() call (vertical=true skips that), but
     * transition_compositor_is_active() is a single global flag shared with
     * close_quick_drawer()'s own vertical-overlay compositor session --
     * quick_drawer_open already flips false the instant that close starts,
     * well before its ~200ms animation (and that compositor session) ends,
     * so a home-swipe confirmed in that window still sees the drawer's
     * session as "active" on its very first tick, hits a compositor mode
     * mismatch, and lands here with sctx == home_swipe_ctx even though
     * home_swipe never touched the compositor itself. */
    if (sctx == home_swipe_ctx) home_swipe_ctx = NULL;
    home_swipe_tracking = false;
    home_swipe_candidate = false;
    home_swipe_just_confirmed = false;
}

bool gui_shell_has_background_work(void) {
    return bt_toggle_active || bt_dac_startup_reapply_active || bt_apply_output_settings_active ||
           bt_source_codec_reconcile_active || refresh_bt_icon_active || wifi_toggle_active ||
           wifi_status_active || bt_monitor_lifecycle_active ||
           bt_reconnect_busy();
}

void gui_shell_cancel_background_work(void) {
    bt_reconnect_shutdown();
    bt_reconnect_pending = false;
    if (bt_monitor_lifecycle_active) {
        pthread_join(bt_monitor_lifecycle_thread, NULL);
        bt_monitor_lifecycle_active = false;
    }
    /* Explicit teardown may wait; ordinary UI interactions never do. */
    bt_control_source_volume_sync_stop();
    bt_control_output_disconnect_watch_stop();
    bt_monitor_want_output = bt_monitor_want_volume = false;
    bt_monitor_applied_output = bt_monitor_applied_volume = false;
    bt_monitor_refresh_requested = false;
    if (wifi_status_active) {
        pthread_join(wifi_status_thread, NULL);
        wifi_status_active = false;
    }
    ++wifi_status_generation;
    wifi_status_connected = false;
    if (bt_toggle_active) {
        pthread_join(bt_toggle_thread, NULL);
        bt_toggle_active = false;
    }
    if (wifi_toggle_active) {
        pthread_join(wifi_toggle_thread, NULL);
        wifi_toggle_active = false;
    }
    if (bt_dac_startup_reapply_active) {
        pthread_join(bt_dac_startup_reapply_thread, NULL);
        bt_dac_startup_reapply_active = false;
    }
    if (bt_apply_output_settings_active) {
        pthread_join(bt_apply_output_settings_thread, NULL);
        bt_apply_output_settings_active = false;
    }
    if (bt_source_codec_reconcile_active) {
        pthread_join(bt_source_codec_reconcile_thread, NULL);
        bt_source_codec_reconcile_active = false;
    }
    if (refresh_bt_icon_active) {
        pthread_join(refresh_bt_icon_thread, NULL);
        refresh_bt_icon_active = false;
    }
}


lv_obj_t * gui_shell_get_home_screen(void) { return home_screen; }
lv_obj_t * gui_shell_get_dac_home_screen(void) { return dac_home_screen; }


lv_obj_t * gui_shell_get_status_bar_band(void) {
    return status_bar_band;
}

lv_obj_t * gui_shell_get_home_indicator_band(void) {
    return home_indicator_band;
}

void gui_shell_set_home_indicator_visible(bool visible) {
    if (home_indicator_band) {
        if (visible) lv_obj_remove_flag(home_indicator_band, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(home_indicator_band, LV_OBJ_FLAG_HIDDEN);
    }
}


void gui_shell_update_quick_drawer_track(const char * title, const char * artist,
                                         const char * album) {
    if (quick_drawer_title_label) {
        const char * want_title = title ? title : "No track loaded";
        const char * want_artist = artist ? artist : "";
        const char * cur_title = lv_label_get_text(quick_drawer_title_label);
        const char * want_album = album ? album : "";
        const char * cur_artist = lv_label_get_text(quick_drawer_artist_label);
        const char * cur_album = quick_drawer_album_label
                               ? lv_label_get_text(quick_drawer_album_label) : NULL;
        /* Only touch a label whose text actually changed: lv_label_set_text()
         * restarts LV_LABEL_LONG_SCROLL_CIRCULAR from the beginning, so
         * re-setting identical text would keep resetting the marquee (and
         * its 2s wait) instead of letting it scroll. Same guard gui_player.c
         * already applies to its own now-playing labels. */
        bool changed = false;
        if (!cur_title || strcmp(cur_title, want_title) != 0) {
            lv_label_set_text(quick_drawer_title_label, want_title);
            changed = true;
        }
        if (!cur_artist || strcmp(cur_artist, want_artist) != 0) {
            lv_label_set_text(quick_drawer_artist_label, want_artist);
            changed = true;
        }
        if (quick_drawer_album_label && (!cur_album || strcmp(cur_album, want_album) != 0)) {
            lv_label_set_text(quick_drawer_album_label, want_album);
            changed = true;
        }
        if (changed) quick_drawer_mark_snapshot_dirty();
    }
    gui_shell_refresh_quick_drawer_cover();
}

void gui_shell_update_quick_drawer_format(const char * text) {
    if (!quick_drawer_format_label) return;
    const char * want = text ? text : "";
    const char * cur = lv_label_get_text(quick_drawer_format_label);
    if (cur && strcmp(cur, want) == 0) return;
    lv_label_set_text(quick_drawer_format_label, want);
    quick_drawer_mark_snapshot_dirty();
}

void gui_shell_refresh_quick_drawer_cover(void) {
    if (!quick_drawer_cover_img) return;
    const lv_image_dsc_t * cover = gui_player_get_current_cover_dsc();
    (void) cover;
    const lv_image_dsc_t * frost = gui_player_get_current_reflection_dsc();
    if (frost && frost->data) {
        lv_image_set_src(quick_drawer_cover_img, frost);
        lv_obj_remove_flag(quick_drawer_cover_img, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(quick_drawer_cover_img, LV_OBJ_FLAG_HIDDEN);
    }
    quick_drawer_fit_cover();
    quick_drawer_mark_snapshot_dirty();
}

void gui_shell_update_quick_drawer_favorite(bool is_favorite) {
    if (quick_drawer_favorite_icon) {
        lv_image_set_src(quick_drawer_favorite_icon,
                         asset_path(is_favorite ? "playing_plane/collect_in.png" : "playing_plane/collect_out.png"));
        quick_drawer_mark_snapshot_dirty();
    }
}

void gui_shell_update_quick_drawer_play_state(bool is_playing) {
    if (quick_drawer_play_btn) {
        lv_image_set_src(quick_drawer_play_btn, gui_player_play_btn_image_src(is_playing));
        quick_drawer_mark_snapshot_dirty();
    }
}

void gui_shell_update_quick_drawer_play_mode(int mode) {
    if (quick_drawer_order_icon) {
        lv_image_set_src(quick_drawer_order_icon, asset_path(play_mode_icon_asset((play_mode_t) mode)));
        quick_drawer_mark_snapshot_dirty();
    }
}

void gui_shell_refresh_quick_drawer_toggle_accent(void) {
    load_quick_drawer_toggle_on_images();
    /* Re-point every widget at the buffers just decoded -- load_...() freed
     * the previous ones, so anything still holding those would be reading
     * released memory on its next draw. Same reason refresh_play_btn_icon()
     * re-sets its own widgets after reloading. State comes from the same
     * authoritative reads build_quick_drawer() uses for the captions. */
    if (quick_drawer_wifi_icon)
        lv_image_set_src(quick_drawer_wifi_icon,
                         quick_drawer_toggle_src(QD_TOGGLE_WIFI, gui_shell_wifi_effective_enabled()));
    if (quick_drawer_bt_icon)
        lv_image_set_src(quick_drawer_bt_icon,
                         quick_drawer_toggle_src(QD_TOGGLE_BT, bt_control_is_powered()));
    if (quick_drawer_sleep_icon)
        lv_image_set_src(quick_drawer_sleep_icon,
                         quick_drawer_toggle_src(QD_TOGGLE_SLEEP, sleep_timer_active));
    if (quick_drawer_crossfade_icon)
        lv_image_set_src(quick_drawer_crossfade_icon,
                         quick_drawer_toggle_src(QD_TOGGLE_CROSSFADE, current_settings.crossfade_enabled));
    /* The expanded rows are subject to the exact same hazard -- load_...()
     * re-decoded their "on" buffers too (including every plugin tile's), so
     * any of them currently showing an on-state would otherwise still point
     * at freed memory. Their off-state icons are plain asset paths and were
     * never at risk, which is also why the inert Gapless tile needs nothing
     * here. */
    refresh_quick_drawer_expansion_toggles();
    quick_drawer_mark_snapshot_dirty();
}
