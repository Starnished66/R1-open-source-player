#include "gui_shell.h"
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
#include "screen_builders.h"
#include "metadata.h"
#include "audio.h"
#include "settings.h"
#include "assets.h"
#include "device_config.h"
#include "battery.h"
#include "charge_limiter.h"
#include "wifi_status.h"
#include "wifi_control.h"
#include "bluetooth_control.h"
#include "usb_audio_output.h"
#include "headphone_status.h"
#include "usb_dac_bridge.h"
#include "backlight.h"
#include "plugin_manager.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>

#define HOME_INDICATOR_BAND_HEIGHT 24
#define QUICK_DRAWER_HEIGHT 367
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

static int volume_warn_threshold_percent = -1;
static lv_obj_t * quick_drawer_wifi_icon = NULL;
static lv_obj_t * quick_drawer_bt_icon = NULL;

static lv_obj_t * quick_drawer = NULL;
static lv_obj_t * quick_drawer_motion_image = NULL;
static lv_draw_buf_t * quick_drawer_motion_buf = NULL;
static bool quick_drawer_bitmap_motion = false;
static bool quick_drawer_snapshot_dirty = true;
static bool quick_drawer_open = false;

#define QUICK_DRAWER_TRIGGER_ZONE 140

static void start_bt_dac_startup_reapply_if_needed(void);
static lv_obj_t * quick_drawer_brightness_track = NULL;
static lv_obj_t * quick_drawer_brightness_label = NULL;
static bool wifi_toggle_active = false;
static bool bt_toggle_active = false;

static void refresh_quick_drawer_brightness(void) {
    if (!quick_drawer_brightness_track) return;
    int brightness = backlight_get_percent();
    if (brightness < 0) brightness = current_settings.brightness_percent;
    lv_slider_set_value(quick_drawer_brightness_track, brightness, LV_ANIM_OFF);
    if (quick_drawer_brightness_label) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%d%%", brightness);
        lv_label_set_text(quick_drawer_brightness_label, buf);
    }
}

extern void player_transition_cache_async_cb(void * user_data);
static lv_obj_t * home_indicator_band = NULL;

static lv_obj_t * quick_drawer_title_label = NULL;
static lv_obj_t * quick_drawer_artist_label = NULL;
static lv_obj_t * quick_drawer_favorite_icon = NULL;
static lv_obj_t * quick_drawer_play_btn = NULL;
static lv_obj_t * quick_drawer_order_icon = NULL;

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

extern player_settings_t current_settings;
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
static lv_obj_t * battery_icon_fill_clip;
static lv_obj_t * battery_icon_fill_img;

/* Fill sprite (topbar/battery.png) bbox within its own 20x30 native canvas,
 * measured directly off the asset (alpha bbox: x 4-15, y 9-22) -- used to
 * clip it down from the bottom as a charge-level gauge in
 * refresh_battery_topbar(). Not derived at runtime since nothing else in
 * this codebase decodes PNG alpha to find sprite bounds; a fixed asset gets
 * a fixed constant, same as every other hand-placed topbar sprite here. */
#define BATTERY_FILL_W 12
#define BATTERY_FILL_H 14
static lv_obj_t * wifi_icon;
static lv_obj_t * bt_status_icon;
static lv_obj_t * a2dp_status_icon;
static lv_obj_t * usb_audio_status_icon;
static lv_obj_t * play_pause_status_icon;

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
    bool hide = current_settings.hide_player_topbar && (screen == gui_player_get_screen() || screen == gui_lyrics_get_screen());
    if (status_bar_band) {
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

    /* Centered on screen, not left-aligned -- confirmed against a real
     * stock-player screenshot: "02:45" sat at x=205-273 out of a 480px-wide
     * panel (center ~239, screen center is 240), not flush against the
     * left edge like our previous layout had it. Sprite digits (topbar/
     * N.png + colon.png), same as volume_topbar_group below, instead of an
     * lv_label -- keeps every top bar readout pixel-identical in size/style
     * rather than an lv_font approximating it. */
    clock_topbar_group = lv_obj_create(band);
    lv_obj_remove_style_all(clock_topbar_group);
    lv_obj_set_size(clock_topbar_group, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(clock_topbar_group, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(clock_topbar_group, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(clock_topbar_group, 0, 0);
    lv_obj_remove_flag(clock_topbar_group, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < 5; i++) {
        clock_topbar_digit[i] = lv_image_create(clock_topbar_group);
        lv_image_set_src(clock_topbar_digit[i], asset_path(i == 2 ? "topbar/colon.png" : "topbar/0.png"));
        lv_image_set_scale(clock_topbar_digit[i], LV_SCALE_NONE);
    }
    clock_topbar_ampm = lv_image_create(clock_topbar_group);
    lv_image_set_src(clock_topbar_ampm, asset_path("topbar/am.png"));
    lv_image_set_scale(clock_topbar_ampm, LV_SCALE_NONE);
    lv_obj_add_flag(clock_topbar_ampm, LV_OBJ_FLAG_HIDDEN); /* refresh_clock_label() unhides this if clock_24h is off */

    /* LAST, after every child exists -- see the matching comment on
     * volume_topbar_group's own align() call below for why (LV_SIZE_CONTENT
     * doesn't retroactively re-run an earlier alignment as children grow
     * it). refresh_clock_label() (called right after build_status_bar() in
     * gui_init) immediately overwrites these placeholder "0"/":" sprites
     * with the real time, so there's no visible flash of "00:00". */
    lv_obj_align(clock_topbar_group, LV_ALIGN_CENTER, 0, 0);

    /* Left edge of the bar, in the clock's old spot -- matches the stock
     * player's own layout (speaker icon, red volume number, headphone-out
     * icon, all pinned left, confirmed via real-device screenshot). A flex
     * row lets hidden digit slots (see refresh_volume_topbar()) collapse
     * cleanly instead of leaving a gap. */
    volume_topbar_group = lv_obj_create(band);
    lv_obj_remove_style_all(volume_topbar_group);
    lv_obj_set_size(volume_topbar_group, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(volume_topbar_group, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(volume_topbar_group, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    /* No extra column padding -- each digit sprite already has ~1px of
     * transparent margin baked into its own canvas on both edges (e.g.
     * topbar/9.png is a 14px-wide canvas with the glyph itself only
     * spanning x=1..13), which is enough separation on its own. Adding a
     * pad_column on top of that visibly widened the gap between digits at
     * this size (confirmed against real-device feedback: "space between
     * the numbers"). */
    lv_obj_set_style_pad_column(volume_topbar_group, 0, 0);
    lv_obj_remove_flag(volume_topbar_group, LV_OBJ_FLAG_SCROLLABLE);

    /* Rendered at native asset resolution (LV_SCALE_NONE), same as
     * battery_icon/wifi_icon/bt_status_icon below -- an earlier downscale
     * here (~0.65x) was based on a mismeasured comparison against the
     * clock text and came out looking too small (real-device feedback).
     * Re-measured directly against a real stock-player screenshot: the red
     * "93" glyph bbox was 21px tall vs the clock's 19px -- i.e. native size
     * is already the right size, no downscale needed. */
    lv_obj_t * volume_topbar_icon = lv_image_create(volume_topbar_group);
    lv_image_set_src(volume_topbar_icon, asset_path("topbar/speaker.png"));
    lv_image_set_scale(volume_topbar_icon, LV_SCALE_NONE);

    /* White by default (the sprite's own native color, no recolor style
     * applied at creation); refresh_volume_topbar() below switches each
     * digit to a flat (255,0,0) recolor once the level reaches
     * volume_warn_threshold_percent, matching the stock player's own
     * config-driven behavior (see device_config.h) instead of the flat
     * always-red guess from the previous round. */
    for (int i = 0; i < 3; i++) {
        volume_topbar_digit[i] = lv_image_create(volume_topbar_group);
        lv_image_set_src(volume_topbar_digit[i], asset_path("topbar/0.png"));
        lv_image_set_scale(volume_topbar_digit[i], LV_SCALE_NONE);
    }

    /* Headphone-out glyph (topbar/po.png, confirmed by pixel comparison
     * against a real-device screenshot) -- starts hidden and is only shown
     * by refresh_headphone_icon() once real jack-detect state says a
     * headphone/dongle is actually plugged in (see headphone_status.h),
     * not shown unconditionally like the previous round had it. */
    volume_topbar_headphone = lv_image_create(volume_topbar_group);
    lv_image_set_src(volume_topbar_headphone, asset_path("topbar/po.png"));
    lv_image_set_scale(volume_topbar_headphone, LV_SCALE_NONE);
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
    lv_image_set_src(a2dp_status_icon, asset_path("topbar/a2dp.png"));
    lv_image_set_scale(a2dp_status_icon, LV_SCALE_NONE);
    lv_obj_add_flag(a2dp_status_icon, LV_OBJ_FLAG_HIDDEN); /* shown by poll_refresh_bt_icon() once an A2DP source PCM exists */

    /* Same flex-collapse shape as the headphone/A2DP glyphs above -- shown/
     * hidden by poll_usb_audio_output() once an external USB audio device
     * (DAC/amp) is detected, entirely automatically, no Settings toggle
     * anywhere (unlike Storage/USB DAC/ADB in the manual USB Mode screen --
     * this is meant to feel like the wired headphone jack, not a mode you
     * switch into). */
    usb_audio_status_icon = lv_image_create(volume_topbar_group);
    /* topbar/usb.png, NOT usb/usb.png -- real-device bug report: the latter
     * is the big centered glyph the USB DAC mode overlay screen uses
     * (build_usb_dac_overlay_screen(), further down), a different asset
     * sized/styled for that full-screen context, not this small topbar
     * status row (which every other icon here -- topbar/a2dp.png,
     * topbar/play.png -- already correctly pulls from topbar/). */
    lv_image_set_src(usb_audio_status_icon, asset_path("topbar/usb.png"));
    lv_image_set_scale(usb_audio_status_icon, LV_SCALE_NONE);
    lv_obj_add_flag(usb_audio_status_icon, LV_OBJ_FLAG_HIDDEN);

    /* Rightmost in this row -- always after whichever headphone-output
     * glyph(s) above are currently shown, per the same flex-collapse
     * reasoning. play.png while actually playing, pause.png while paused,
     * hidden entirely when stopped/nothing loaded --
     * refresh_play_pause_topbar(). */
    play_pause_status_icon = lv_image_create(volume_topbar_group);
    lv_image_set_src(play_pause_status_icon, asset_path("topbar/play.png"));
    lv_image_set_scale(play_pause_status_icon, LV_SCALE_NONE);
    lv_obj_add_flag(play_pause_status_icon, LV_OBJ_FLAG_HIDDEN);

    /* Deliberately LAST, after every child exists -- done earlier, the
     * LV_SIZE_CONTENT group still had zero content size at that point, and
     * its later growth as children were added did NOT retroactively re-run
     * this alignment (confirmed on real hardware in an earlier round of
     * this same bug: the group ended up anchored low and out of vertical
     * sync with the rest of the bar). */
    lv_obj_align(volume_topbar_group, LV_ALIGN_LEFT_MID, 16, 0);

    /* Outline frame -- swapped between battery_bg.png (normal),
     * battery_charge_bg.png (charging, has its own baked-in bolt glyph) and
     * battery_low_bg.png (red, <5% and not charging) by
     * refresh_battery_topbar(). Previously this was a single static
     * "topbar/battery.png" (the plain fill rectangle below, with no outline
     * at all) that was never touched again after creation -- the icon never
     * reflected charge state or percentage at all, just a fixed white
     * square regardless of real battery level. */
    battery_icon_frame = lv_image_create(band);
    lv_image_set_src(battery_icon_frame, asset_path("topbar/battery_bg.png"));
    lv_image_set_scale(battery_icon_frame, LV_SCALE_NONE);
    lv_obj_align(battery_icon_frame, LV_ALIGN_RIGHT_MID, -15, 0);

    /* Charge-level gauge: a plain clipping container sized/positioned every
     * refresh to BATTERY_FILL_W x (BATTERY_FILL_H * percent/100), holding
     * the full-size fill sprite bottom-aligned inside it -- lv_obj clips
     * children to its own box by default (no LV_OBJ_FLAG_OVERFLOW_VISIBLE
     * set here), so shrinking the container's height reveals progressively
     * less of the sprite from the top down, same visual as a liquid gauge
     * draining towards the frame's terminal nub. Hidden outright while
     * charging (the charge_bg frame already shows its own bolt glyph) or
     * below 5% (the low frame should read as "empty", not partially
     * filled). */
    battery_icon_fill_clip = lv_obj_create(band);
    lv_obj_remove_style_all(battery_icon_fill_clip);
    lv_obj_set_size(battery_icon_fill_clip, BATTERY_FILL_W, BATTERY_FILL_H);
    lv_obj_remove_flag(battery_icon_fill_clip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align_to(battery_icon_fill_clip, battery_icon_frame, LV_ALIGN_BOTTOM_MID, 0, -8);

    battery_icon_fill_img = lv_image_create(battery_icon_fill_clip);
    lv_image_set_src(battery_icon_fill_img, asset_path("topbar/battery.png"));
    lv_image_set_scale(battery_icon_fill_img, LV_SCALE_NONE);
    lv_obj_align(battery_icon_fill_img, LV_ALIGN_BOTTOM_MID, 0, 7);

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
        battery_topbar_digit[i] = lv_image_create(battery_topbar_group);
        lv_image_set_src(battery_topbar_digit[i], asset_path("topbar/0.png"));
        lv_image_set_scale(battery_topbar_digit[i], LV_SCALE_NONE);
    }
    battery_topbar_percent = lv_image_create(battery_topbar_group);
    lv_image_set_src(battery_topbar_percent, asset_path("topbar/percent.png"));
    lv_image_set_scale(battery_topbar_percent, LV_SCALE_NONE);

    /* Anchored to battery_icon itself (not a hand-tuned x) rather than a
     * fixed band offset, since the group's own width varies with the
     * digit count (1-3) -- LAST, after every child exists, same reasoning
     * as volume_topbar_group's align() below. */
    lv_obj_align_to(battery_topbar_group, battery_icon_frame, LV_ALIGN_OUT_LEFT_MID, -5, 0);

    wifi_icon = lv_image_create(band);
    lv_image_set_src(wifi_icon, asset_path("topbar/wifi_unconnect.png"));
    lv_image_set_scale(wifi_icon, LV_SCALE_NONE);
    lv_obj_align(wifi_icon, LV_ALIGN_RIGHT_MID, -105, 0);
    lv_obj_add_flag(wifi_icon, LV_OBJ_FLAG_HIDDEN); /* shown by refresh_wifi_icon() once wifi_control_is_enabled() */

    bt_status_icon = lv_image_create(band);
    lv_image_set_src(bt_status_icon, asset_path("topbar/bluetooth.png"));
    lv_image_set_scale(bt_status_icon, LV_SCALE_NONE);
    lv_obj_align(bt_status_icon, LV_ALIGN_RIGHT_MID, -145, 0);
    lv_obj_add_flag(bt_status_icon, LV_OBJ_FLAG_HIDDEN); /* shown by refresh_bt_icon() once bt_control_is_powered() */
}

static void refresh_play_pause_topbar(void) {
    if (audio_is_playing()) {
        lv_obj_remove_flag(play_pause_status_icon, LV_OBJ_FLAG_HIDDEN);
        lv_image_set_src(play_pause_status_icon, asset_path("topbar/play.png"));
    } else if (audio_is_paused()) {
        lv_obj_remove_flag(play_pause_status_icon, LV_OBJ_FLAG_HIDDEN);
        lv_image_set_src(play_pause_status_icon, asset_path("topbar/pause.png"));
    } else {
        lv_obj_add_flag(play_pause_status_icon, LV_OBJ_FLAG_HIDDEN);
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

    /* The fuel gauge can recalibrate upward after charging is stopped, and
     * the kernel's preferred battery/status node remains stale at
     * "Charging" even while AXP2101 REG18 has chg_en cleared and REG01 says
     * not_charging. Without this limiter-aware presentation, battery.c's
     * direction filter walks the visible number from 85 toward that stale
     * raw value, making a working electrical cutoff look broken. Keep the
     * raw percentage untouched for charge_limiter_poll()'s hysteresis; only
     * cap what the 85%-limit UI promises to show while a hold is active. */
    bool charge_limiter_holding = charge_limiter_is_holding();
    if (charge_limiter_holding && percent > 85) percent = 85;

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
        lv_obj_add_flag(battery_icon_fill_clip, LV_OBJ_FLAG_HIDDEN);
        lv_image_set_src(battery_icon_frame, asset_path("topbar/battery_bg.png"));
        return;
    }
    if (percent > 100) percent = 100;

    /* Real-device bug report: the charging bolt disappeared as soon as the
     * 85% limiter was enabled, well before the battery actually got there.
     * Root cause: charge_limiter_holding tracks charge_limiter.c's own
     * hysteresis state, which flips true one point EARLY (at 84%, see
     * CHARGE_LIMITER_TRIGGER_PERCENT's own comment -- deliberate, to absorb
     * fuel-gauge lag before the real 85% cutoff) and, being sticky
     * hysteresis, can stay true from a previous session even once the
     * displayed percent has since dropped a little without crossing the
     * lower CHARGE_LIMITER_RESUME_PERCENT reset point. Neither case means
     * the battery has actually reached the 85% this UI promises -- only
     * suppress the charging icon once the displayed percent has genuinely
     * gotten there too, matching what's on screen rather than the internal
     * hysteresis flag alone. */
    bool limiter_capped_now = charge_limiter_holding && percent >= 85;
    bool charging = !limiter_capped_now && battery_is_charging();
    bool low = !charging && percent < 5;

    lv_image_set_src(battery_icon_frame,
                      asset_path(charging ? "topbar/battery_charge_bg.png"
                                 : low    ? "topbar/battery_low_bg.png"
                                          : "topbar/battery_bg.png"));

    /* Fill gauge only makes sense for the plain frame -- the charging frame
     * already carries its own bolt glyph, and "low" should read as visually
     * empty, not a sliver of fill. */
    if (charging || low) {
        lv_obj_add_flag(battery_icon_fill_clip, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(battery_icon_fill_clip, LV_OBJ_FLAG_HIDDEN);
        int fill_h = (BATTERY_FILL_H * percent + 50) / 100;
        if (fill_h < 1) fill_h = 1;
        if (fill_h > BATTERY_FILL_H) fill_h = BATTERY_FILL_H;
        lv_obj_set_height(battery_icon_fill_clip, fill_h);
        lv_obj_align_to(battery_icon_fill_clip, battery_icon_frame, LV_ALIGN_BOTTOM_MID, 0, -8);
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
            char asset[24];
            snprintf(asset, sizeof(asset), "topbar/%c.png", digits[digit_index]);
            lv_image_set_src(battery_topbar_digit[i], asset_path(asset));
            lv_obj_remove_flag(battery_topbar_digit[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/* Called once at startup (gui_init, with the volume loaded from settings),
 * every time the hardware volume buttons change the level
 * (update_timer_cb), and now also live while the user drags volume_popup_
 * track itself (volume_popup_track_event_cb below). */
void refresh_volume_topbar(int32_t percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    char digits[4];
    snprintf(digits, sizeof(digits), "%d", (int) percent);
    int len = (int) strlen(digits);

    /* volume_warn_threshold_percent is -1 when the feature is off (see its
     * declaration) -- guard it explicitly rather than just comparing
     * percent >= threshold, since percent >= -1 is always true. */
    bool warn = volume_warn_threshold_percent >= 0 && percent >= volume_warn_threshold_percent;

    for (int i = 0; i < 3; i++) {
        int digit_index = i - (3 - len);
        if (digit_index < 0) {
            lv_obj_add_flag(volume_topbar_digit[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            char asset[24];
            snprintf(asset, sizeof(asset), "topbar/%c.png", digits[digit_index]);
            lv_image_set_src(volume_topbar_digit[i], asset_path(asset));
            lv_obj_remove_flag(volume_topbar_digit[i], LV_OBJ_FLAG_HIDDEN);
        }
        /* LV_OPA_TRANSP disables the recolor mix entirely, leaving the
         * sprite's own native white showing through -- simpler than
         * swapping between a white-recolor and a red-recolor style. */
        lv_obj_set_style_image_recolor(volume_topbar_digit[i], lv_color_make(255, 0, 0), 0);
        lv_obj_set_style_image_recolor_opa(volume_topbar_digit[i], warn ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    }
}

/* Polled every timer tick alongside refresh_battery_topbar() -- like
 * battery.c's sysfs read, this is a single cheap fopen/fgets with no
 * subprocess fork, so it doesn't need wifi/bt's throttled polling. */
void refresh_headphone_icon(void) {
    if (headphone_is_connected()) {
        lv_obj_remove_flag(volume_topbar_headphone, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(volume_topbar_headphone, LV_OBJ_FLAG_HIDDEN);
    }
}

/* wpa_cli forks a process per call (see wifi_status.c), so this is only
 * polled every WIFI_POLL_TICKS timer ticks rather than every tick like the
 * clock/battery -- wifi signal doesn't change fast enough to need
 * sub-second polling anyway. */
#define WIFI_POLL_TICKS 10

/* Real-device bug report: wifi_icon and bt_status_icon originally each sat
 * at their own hand-tuned fixed offset from battery_icon_frame -- fine when
 * both or neither were showing, but with only one of the two radios on, the
 * other's now-hidden slot was left as a dead gap between the visible icon
 * and the battery percentage instead of the visible one sliding over to sit
 * right next to it. Fix: track which of the two is CURRENTLY closer to the
 * battery (order[0], the inner slot) vs. one slot further out (order[1]),
 * and re-derive it from scratch on every call rather than mutating an
 * existing arrangement in place -- simpler and can't drift out of sync with
 * the two icons' own hidden-flag state, the actual source of truth, which
 * is all this ever reads. Whichever of the two is currently visible AND was
 * already occupying a slot keeps it; a newly-visible icon takes whichever
 * slot (if any) is still free. This is what gives "closer to the battery"
 * its "whichever appeared first" ordering from the bug report: the icon
 * that was already on when the second one turns on keeps the inner slot
 * instead of being displaced, and the moment either disappears the survivor
 * (if any) is pulled into the inner slot so there's never a gap. The two
 * slots are positioned relative to battery_topbar_group/battery_icon_frame
 * (not a fixed offset), further down -- see that comment for how Settings >
 * Power > "Battery Percentage" folds into the same anchor logic. */
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
        lv_obj_align_to(widget, anchor, LV_ALIGN_OUT_LEFT_MID, -8, 0);
        anchor = widget;
    }
}

/* The drawer's own wifi icon just reflects radio-on/off (blue as soon as
 * enabled, real-device feedback: the connected-vs-just-enabled distinction
 * is a top-bar-only thing) -- the top bar icon keeps the finer-grained
 * enabled-vs-actually-associated-to-an-AP distinction below. */
static void refresh_wifi_icon(void) {
    bool enabled = wifi_control_is_enabled();
    if (quick_drawer_wifi_icon) {
        lv_image_set_src(quick_drawer_wifi_icon, asset_path(enabled ? "pull_down/wifi_s.png" : "pull_down/wifi.png"));
        quick_drawer_mark_snapshot_dirty();
    }

    if (!enabled) {
        lv_obj_add_flag(wifi_icon, LV_OBJ_FLAG_HIDDEN);
        sync_topbar_status_icon_positions();
        return;
    }
    lv_obj_remove_flag(wifi_icon, LV_OBJ_FLAG_HIDDEN);
    sync_topbar_status_icon_positions();

    int level;
    if (wifi_get_status(&level)) {
        char asset[40];
        snprintf(asset, sizeof(asset), "topbar/wifi_connect_%d.png", level);
        lv_image_set_src(wifi_icon, asset_path(asset));
    } else {
        lv_image_set_src(wifi_icon, asset_path("topbar/wifi_unconnect.png"));
    }
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

/* Real-device incident: on a genuine cold boot, hci0 briefly reads
 * "powered" right after S80_bt_init's own firmware flash (which leaves the
 * raw HCI device up as a side effect of attaching it), before bluetoothd
 * itself has even started, and forces it back off per its own
 * AutoEnable=false default (/etc/bluetooth/main.conf) once bluetoothd does
 * start -- a real, if variable-length (real-device measurements from ~5s
 * to ~16s depending on boot timing), on-then-off transition at the
 * hci0/bluetoothd level, not a UI caching bug -- see S80_bt_init's own
 * /usr/bin/bt_init for the actual sequence. This app's own status poll can
 * straddle that window and flash the topbar/drawer icon on, then off, a
 * few seconds apart.
 *
 * A plain time-boxed suppression window was tried here and removed
 * (2026-08-13): it only hid the transient from polls that happened to land
 * inside the window, so the same on-then-off flash still showed up right
 * after the window closed instead -- not an actual fix, just relocating
 * when the flicker becomes visible.
 *
 * The mask is now state-based rather than time-based: it remains active
 * until bt_init has written its tmpfs completion marker AND two subsequent
 * polls agree the adapter is off. A manual toggle ends it immediately, so
 * explicit user intent always wins. Starting the player later in the same
 * boot does not enable the mask because bt_init_ok already exists; resume
 * likewise never re-enters gui_init(). */
static bool bt_boot_suppress_enabled = false;
static unsigned int bt_boot_off_observations = 0;
#define BT_BOOT_OFF_OBSERVATIONS_REQUIRED 2

/* /usr/bin/bt_init's (stock, unmodified) very last line is
 * `mkdir -p /tmp; echo > /tmp/bt_init_ok`, right after its own real UART
 * chip firmware flash and everything else it does. /tmp is tmpfs on this
 * device, so this file can never be a stale leftover from a previous boot.
 *
 * Real-device incident: tapping Bluetooth on (quick_drawer_bt_event_cb()
 * below) while bt_init's own chip flash was still genuinely in progress
 * raced this app's own bt_control_init_chip() -> /usr/bin/bt_resume
 * against it -- confirmed live to actually wedge the chip (unrecoverable
 * without a full power cycle), the exact class of incident bt_chip_mutex's
 * own comment already documents (a userspace mutex in THIS app can't
 * protect against a SEPARATE process, bt_init, touching the same UART).
 * The display-suppression window above made the *cosmetic* flicker
 * disappear, but made this WORSE, not better: "graduate on tap" meant a
 * tap landing early now reliably reached the real toggle thread instead of
 * being naturally rate-limited by the flicker's own visibility. Checked in
 * quick_drawer_bt_event_cb() before it does anything real -- see its own
 * comment. */
#define BT_INIT_OK_FLAG_PATH "/tmp/bt_init_ok"

static bool bt_boot_suppress_active(void) {
    return bt_boot_suppress_enabled;
}

/* Moved up from the Bluetooth settings screen section further down (still
 * used there, see populate_bt_screen()) -- refresh_bt_icon_thread_func()
 * below needs to reference bt_scan_results directly, before its own
 * definition down there, to fix a real-device bug: the settings screen's
 * device list only ever reflected paired/connected state as of the last
 * explicit scan, never refreshed afterward unlike the top-bar icon (see
 * poll_refresh_bt_icon()'s own comment on the merge step below). */
#define BT_MAX_RESULTS 32
bt_device_t bt_scan_results[BT_MAX_RESULTS];
int bt_scan_result_count = 0;

/* Written by refresh_bt_icon_thread_func() below, merged into
 * bt_scan_results by poll_refresh_bt_icon() -- see its own comment. */
static bt_device_t bt_paired_states_result[BT_MAX_RESULTS];
static int bt_paired_states_count = 0;

/* Real-device incident: this used to call bt_control_is_powered()/
 * bt_control_is_connected() (bluetoothctl show / bluetoothctl info)
 * directly, synchronously, right here on the UI thread -- every call site
 * of what's now start_refresh_bt_icon() ran on that thread, including the
 * periodic ~5s poll. subprocess_run()'s own 15s timeout-and-kill exists
 * specifically because bluetoothctl show is known to hang under certain
 * Bluetooth states (see its doc comment) -- confirmed on a real device
 * that active A2DP audio streaming (Bluetooth DAC, phone actively playing)
 * is exactly such a state: bluetoothctl show hung, and since the periodic
 * poll re-issued another call as soon as (or before) the previous one's
 * bounded wait gave up, the whole UI stayed frozen for as long as the hang
 * persisted, not just one bounded 15s stall. Backgrounded the same way
 * every other slow Bluetooth operation in this file already is, so a hang
 * here can no longer block LVGL's own timer_handler() from running. */
static pthread_t refresh_bt_icon_thread;
static bool refresh_bt_icon_active = false;
static atomic_bool refresh_bt_icon_done_flag = false;
static bool refresh_bt_icon_result_powered = false;
static bool refresh_bt_icon_result_connected = false;
static bool refresh_bt_icon_result_a2dp_connected = false;
bool gui_shell_is_bt_audio_connected(void) { return refresh_bt_icon_result_a2dp_connected; }
static char refresh_bt_icon_result_mac[18] = "";
static char refresh_bt_icon_result_codec[32] = "";

static void * refresh_bt_icon_thread_func(void * arg) {
    (void) arg;
    bool powered = bt_control_is_powered();
    refresh_bt_icon_result_powered = powered;

    /* Same background thread/cadence as everything else here -- one more
     * subprocess call (bluealsa-cli list-pcms) alongside the bluetoothctl
     * calls below, not a separate poll loop. */
    refresh_bt_icon_result_a2dp_connected = powered && bt_control_is_a2dp_source_connected();

    /* Two more subprocess calls (bluealsa-cli info, reusing the same PCM
     * path lookup bt_control_is_a2dp_source_connected() just did) -- only
     * worth paying when something's actually A2DP-connected. Both left at
     * "" (not stale) when nothing is, so add_bt_device_row() never shows a
     * leftover codec line for a device that just disconnected. */
    refresh_bt_icon_result_mac[0] = '\0';
    refresh_bt_icon_result_codec[0] = '\0';
    if (refresh_bt_icon_result_a2dp_connected) {
        bt_control_get_connected_device_mac(refresh_bt_icon_result_mac, sizeof(refresh_bt_icon_result_mac));
        bt_control_get_connected_device_codec(refresh_bt_icon_result_codec, sizeof(refresh_bt_icon_result_codec));
    }

    if (powered) {
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
        bt_paired_states_count = 0;
        refresh_bt_icon_result_connected = false;
    }

    atomic_store_explicit(&refresh_bt_icon_done_flag, true, memory_order_release); /* written last -- poll_refresh_bt_icon only checks this flag */
    return NULL;
}

static void start_refresh_bt_icon(void) {
    if (refresh_bt_icon_active) return; /* previous check still in flight -- same "ignore taps until it lands" pattern as everything else here */
    refresh_bt_icon_active = true;
    atomic_store_explicit(&refresh_bt_icon_done_flag, false, memory_order_relaxed);
        if (pthread_create(&refresh_bt_icon_thread, NULL, refresh_bt_icon_thread_func, NULL) != 0) {
        refresh_bt_icon_active = false;
    }
}


static void poll_refresh_bt_icon(void) {
    if (!refresh_bt_icon_active || !atomic_load_explicit(&refresh_bt_icon_done_flag, memory_order_acquire)) return;
    refresh_bt_icon_active = false;
    pthread_join(refresh_bt_icon_thread, NULL);

    /* Real-device bug report: enabling Bluetooth from the quick drawer
     * flipped the drawer icon on (quick_drawer_bt_event_cb()'s own
     * optimistic flip), then back off, then back on again. Root cause:
     * this poll runs independently, on its own periodic cadence, of the
     * user's own tap-to-toggle -- if one lands mid-flight (turning
     * Bluetooth on for real can take ~10-13s cold), refresh_bt_icon_result_
     * powered still reflects the OLD, pre-toggle state, since bt_control_
     * is_powered() genuinely hasn't changed yet. The populate_bt_screen()
     * call further down was ALREADY guarded against exactly this race (see
     * its own comment) after an earlier, identical bug report about the
     * Bluetooth settings screen's own toggle row -- but that fix only
     * covered the settings screen, not bt_is_powered_cached itself or the
     * drawer icon below, which this same stale result was still freely
     * overwriting. Skipping the whole result application while
     * bt_toggle_active leaves the optimistic flip standing undisturbed
     * everywhere, not just on the settings screen, until poll_bt_toggle()'s
     * own follow-up start_refresh_bt_icon() call lands with the real,
     * settled state once the in-flight toggle actually completes. */
    if (bt_toggle_active) return;

    /* Do not graduate based on elapsed time. A worker can start its
     * bluetoothctl query during bt_init's transient powered interval and
     * deliver that stale "on" result after a timer expires, which is the
     * exact cold-boot flash this mask exists to prevent. Instead require
     * bt_init's tmpfs completion marker plus two settled "off" results.
     * Resume never enters this state, and a deliberate user tap clears it
     * immediately in quick_drawer_bt_event_cb(). */
    if (bt_boot_suppress_active()) {
        if (access(BT_INIT_OK_FLAG_PATH, F_OK) == 0 && !refresh_bt_icon_result_powered) {
            bt_boot_off_observations++;
            if (bt_boot_off_observations >= BT_BOOT_OFF_OBSERVATIONS_REQUIRED) {
                bt_boot_suppress_enabled = false;
            }
        } else {
            bt_boot_off_observations = 0;
        }
    }

    bool display_powered = refresh_bt_icon_result_powered;
    if (bt_boot_suppress_active()) display_powered = false;

    bt_is_powered_cached = display_powered;
    snprintf(bt_connected_mac_cached, sizeof(bt_connected_mac_cached), "%s", refresh_bt_icon_result_mac);
    snprintf(bt_connected_codec_cached, sizeof(bt_connected_codec_cached), "%s", refresh_bt_icon_result_codec);
    if (quick_drawer_bt_icon) {
        lv_image_set_src(quick_drawer_bt_icon, asset_path(display_powered ? "pull_down/bt_s.png" : "pull_down/bt.png"));
        quick_drawer_mark_snapshot_dirty();
    }

    if (!display_powered) {
        lv_obj_add_flag(bt_status_icon, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(a2dp_status_icon, LV_OBJ_FLAG_HIDDEN);
        sync_topbar_status_icon_positions();
        /* Bluetooth screen's own toggle row + everything gated on it reads
         * bt_is_powered_cached too -- only actually needs rebuilding while
         * that screen is the one on screen, see the comment below on the
         * other populate_bt_screen() call site for why. (No bt_toggle_active
         * check needed here anymore -- the whole function already returned
         * early above while a toggle's in flight, see that comment for the
         * real-device bug this used to only half-fix.) */
        if (gui_navigation_is_top(gui_network_get_bt_screen())) populate_bt_screen();
        return;
    }
    lv_obj_remove_flag(bt_status_icon, LV_OBJ_FLAG_HIDDEN);
    sync_topbar_status_icon_positions();
    lv_image_set_src(bt_status_icon, asset_path(refresh_bt_icon_result_connected ? "topbar/bluetooth.png" : "topbar/bluetooth_unconnect.png"));
    if (refresh_bt_icon_result_a2dp_connected) {
        lv_obj_remove_flag(a2dp_status_icon, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(a2dp_status_icon, LV_OBJ_FLAG_HIDDEN);
    }

    /* Real-device bug: the Bluetooth settings screen's device list kept
     * showing paired/connected state as of the last explicit scan forever
     * after -- populate_bt_screen() re-runs every poll tick already (right
     * below), but it only re-renders bt_scan_results, which nothing kept
     * fresh; only this function's own icon update above was ever current.
     * Update in place by MAC match rather than appending -- a device not
     * already in bt_scan_results (nothing scanned yet) still needs an
     * explicit Rescan, same as before; this only fixes staleness for
     * entries already on screen.
     *
     * Real-device bug #2 (found later): "Forget Device" (poll_bt_forget())
     * clears bt_scan_results[i].paired locally and immediately, but this
     * loop used to only ever UPDATE an entry that appeared in the fresh
     * bt_paired_states_result[] snapshot -- never explicitly clear one that
     * dropped OUT of it. A background poll that started (bt_control_
     * list_paired_states() takes several real subprocess round trips)
     * before the user hit Forget, but which HAPPENS to land afterward,
     * still carried the stale "still paired" snapshot -- reapplying it
     * here silently undid poll_bt_forget()'s own correct clear, confirmed
     * live as "forgot a device, it stopped showing up at all" (stuck
     * showing as still-paired, so filtered out of both list sections it
     * could sanely appear in). Iterating bt_scan_results and searching
     * bt_paired_states_result (inverted from the original nesting) instead
     * makes this poll's own result authoritative in BOTH directions: found
     * -> apply it, not found -> it's not paired, full stop, regardless of
     * whatever an even-more-stale direct clear or a previous poll left
     * behind.
     *
     * Guarded on bt_paired_states_count >= 0 (not -1, see
     * bt_control_list_paired_states()'s own doc comment): a failed query
     * means no fresh data at all this cycle, not "0 devices are paired" --
     * applying the "not found -> clear it" half on a failed query would
     * incorrectly wipe every device's real paired state over a transient
     * subprocess hiccup. Skipping the whole merge (leaving bt_scan_results
     * exactly as it was) just means this cycle contributes nothing, same as
     * if the poll simply hadn't run yet -- the next successful cycle
     * catches up normally. */
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
    /* Real-device bug: this used to call populate_bt_screen() unconditionally
     * every ~5s poll tick regardless of which screen was actually on
     * screen -- lv_obj_clean() + rebuilding every device row (icons,
     * labels, buttons) is real LVGL work, confirmed live as visible UI
     * tearing/animation stutter on OTHER screens (player, home, ...) the
     * whole time Bluetooth was on, not just while the Bluetooth screen was
     * open. bt_scan_results itself is still kept fresh above every cycle
     * regardless (cheap, no LVGL calls) -- only the actual widget rebuild is
     * gated, and open_bluetooth_screen() already calls populate_bt_screen()
     * itself once on entry, so the screen is never stale when the user
     * actually opens it. (No bt_toggle_active check needed here either --
     * same reasoning as the other populate_bt_screen() call site above,
     * this function's own !display_powered branch.) */
    if (gui_navigation_is_top(gui_network_get_bt_screen())) populate_bt_screen();

    /* Real-device bug: pairing/connecting Bluetooth headphones worked (this
     * poll's own refresh_bt_icon_result_connected went true), but no audio
     * ever played -- see audio_set_bt_output()'s doc comment in audio.h for
     * the root cause (this app's output was hardcoded to local hardware,
     * with no path to bluealsa at all). Gated on bt_dac_mode_enabled being
     * off: DAC mode runs bluealsa as a2dp-sink (receiving audio FROM a
     * phone), not a2dp-source, so there's no source profile for this app's
     * own playback to route into while DAC mode has that swapped out (see
     * bt_control_apply_output_settings()'s own comment on the two being
     * mutually exclusive). */
    bool use_bt_output = refresh_bt_icon_result_connected && !current_settings.bt_dac_mode_enabled;
    audio_set_bt_output(use_bt_output);

    /* Same gating as audio_set_bt_output() right above -- real-device bug
     * report: once Bluetooth output itself worked, the headphones' own
     * volume buttons had no effect on this app and vice versa. See
     * bt_control_source_volume_sync_start()'s own comment in
     * bluetooth_control.c for why this doesn't double-attenuate on top of
     * this app's own volume taper. */
    if (use_bt_output) {
        bt_control_source_volume_sync_start();
    } else {
        bt_control_source_volume_sync_stop();
    }

    /* Same gating again -- see bt_control_output_disconnect_watch_start()'s
     * own comment (bluetooth_control.h) for what this buys over the plain
     * ~5s poll below (refresh_bt_icon_result_a2dp_connected itself): a real
     * disconnect surfaces in well under a second instead of up to ~17s. */
    if (use_bt_output) {
        bt_control_output_disconnect_watch_start();
    } else {
        bt_control_output_disconnect_watch_stop();
    }

    /* Same gating again -- real-device bug report: "can't use bluetooth
     * headphones while on USB DAC". usb_dac_bridge.c's own output stream
     * used to always go straight to local hardware regardless of this;
     * now it shares the same audio_output module local playback uses (see
     * usb_dac_bridge_set_bt_output()'s own doc comment), so it needs the
     * same signal. Harmless to call when USB DAC mode isn't even active
     * (the bridge just isn't running, so this only updates a flag it'll
     * read next time it starts). */
    usb_dac_bridge_set_bt_output(use_bt_output);
}

/* Volume popup moved to gui_player.c */


/* Android-style home indicator: a small pill fixed to the bottom edge,
 * living on lv_layer_top() (drawn above every screen, same trick as the
 * status bar) so a swipe-up starting there is always caught by THIS object
 * instead of whatever scrollable list happens to be underneath it.
 *
 * Real-device bug report: plain swipe-up-anywhere (screen_gesture_event_cb()
 * above) didn't work on any screen with a scrollable list -- LVGL claims a
 * vertical drag as a list SCROLL before it ever escalates to a gesture (see
 * enable_gesture_bubble_recursive()'s own comment: that only affects whether
 * a completed gesture bubbles up, not whether one gets generated in the
 * first place on an object that can still scroll in that direction). A
 * horizontal swipe doesn't have this problem since none of these lists
 * scroll sideways, which is why back/forward navigation swipes were never
 * affected. Deliberately NOT shrinking every screen's own content height to
 * visually reserve this strip too (touches this many build_XXX_screen()
 * call sites for a first pass) -- it overlays the very bottom of scrollable
 * content instead, same tradeoff plenty of real apps make with a floating
 * gesture bar.
 *
 * Second real-device bug report: an LV_EVENT_GESTURE handler directly on
 * this band (the first attempt) never fired at all. Same root cause already
 * documented and fixed for the quick drawer's own edge-swipe (see
 * poll_quick_drawer_drag()'s own long comment): LVGL's gesture detection is
 * unreliable for this exact "small dedicated edge zone" shape of
 * interaction on real hardware. Tracking is done there instead, by polling
 * the indev's raw position every tick alongside the drawer's own drag
 * tracking (home_swipe_tracking/home_swipe_start_y/home_swipe_triggered,
 * declared there) -- sidesteps LVGL's hit-testing/gesture-escalation
 * machinery entirely, the same fix that made the drawer's own swipe
 * reliable. */

static void build_home_indicator_bar(void) {
    lv_obj_t * top = lv_layer_top();

    home_indicator_band = lv_obj_create(top);
    lv_obj_remove_style_all(home_indicator_band);
    lv_obj_set_size(home_indicator_band, lv_pct(100), HOME_INDICATOR_BAND_HEIGHT);
    lv_obj_align(home_indicator_band, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_remove_flag(home_indicator_band, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(home_indicator_band, LV_OBJ_FLAG_CLICKABLE); /* claims touches in this strip before any list underneath can -- the actual swipe-up trigger is poll_quick_drawer_drag()'s raw position polling, not a click/gesture event on this object */

    /* The visible pill itself -- plain light-gray rounded bar, matching
     * Android's own gesture-nav home indicator. */
    lv_obj_t * pill = lv_obj_create(home_indicator_band);
    lv_obj_remove_style_all(pill);
    lv_obj_set_size(pill, 120, 4);
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
 * subprocess), same cheap class of check as headphone_is_connected()'s own
 * direct sysfs read, so this is safe to call directly on the UI thread at
 * the same low cadence as the wifi/Bluetooth polls (see their own
 * WIFI_POLL_TICKS call site) rather than needing its own background
 * thread. Toast fires only on the actual connect transition (was_connected
 * tracked across calls), matching how "Paused: headphones disconnected"
 * only fires once per real disconnect rather than every poll tick. */
static void poll_usb_audio_output(void) {
    static bool was_connected = false;
    char alsa_device[32];
    bool connected = usb_audio_output_is_connected(alsa_device, sizeof(alsa_device));

    if (connected && !was_connected) show_error_toast("USB audio device detected");
    was_connected = connected;

    audio_set_usb_output(connected, alsa_device);
    if (connected) {
        lv_obj_remove_flag(usb_audio_status_icon, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(usb_audio_status_icon, LV_OBJ_FLAG_HIDDEN);
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

/* Real control now -- see crossfade_switch_event_cb (Settings > Crossfade)
 * for the other half of this same toggle; both read/write
 * current_settings.crossfade_enabled and both keep the OTHER one's icon/
 * switch state in sync (refresh_quick_drawer_crossfade_icon() /
 * sync_settings_crossfade_toggle()), so whichever one you use, the other
 * reflects it next time you look. Uses pull_down/fade.png/fade_s.png (a
 * real stock asset, dedicated to this -- confirmed by name, unlike
 * gain_h.png/gain_l.png this used to show, which was always a placeholder
 * for "output gain", never a real control). */
static lv_obj_t * quick_drawer_crossfade_icon;
void refresh_quick_drawer_crossfade_icon(void) {
    if (!quick_drawer_crossfade_icon) return;
    lv_image_set_src(quick_drawer_crossfade_icon,
                     asset_path(current_settings.crossfade_enabled ? "pull_down/fade_s.png" : "pull_down/fade.png"));
    quick_drawer_mark_snapshot_dirty();
}

/* Settings > Playback's own Crossfade toggle row (build_settings_playback_screen(),
 * captured via pill_list_item_t's out_toggle_img) -- that screen is built
 * once at gui_init() and never rebuilt, so its toggle's sprite/LV_STATE_CHECKED
 * only ever reflects whatever current_settings.crossfade_enabled was at
 * that one build time unless something explicitly pokes it afterward.
 * Real-device bug report: toggling crossfade from the quick drawer left
 * this row showing the old (now wrong) state the next time Settings >
 * Playback was opened -- the settings->drawer direction already worked
 * (crossfade_switch_event_cb calls refresh_quick_drawer_crossfade_icon()),
 * but nothing called the reverse. */


static void quick_drawer_crossfade_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    current_settings.crossfade_enabled = !current_settings.crossfade_enabled;
    audio_set_crossfade_enabled(current_settings.crossfade_enabled);
    settings_save(&current_settings);
    refresh_quick_drawer_crossfade_icon();
    gui_settings_sync_crossfade_toggle();
}

/* Defined later, alongside the rest of the transport-button wiring --
 * forward-declared here since poll_sleep_timer() below needs it on
 * expiry. */


/* Sleep timer: tapping this icon arms/disarms a real countdown (current
 * duration from current_settings.sleep_timer_minutes, configurable via
 * Settings > Sleep Timer) that pauses playback once it elapses -- see
 * poll_sleep_timer() (update_timer_cb) for the actual countdown/expiry
 * logic. quick_drawer_sleep_label shows the remaining time below the icon
 * while armed, per real-device feedback wanting a visible countdown, not
 * just an on/off glow -- hidden the rest of the time. Session-only state
 * (sleep_timer_active): arming isn't persisted, so a relaunch never resumes
 * a stale countdown from a previous session. */
static bool sleep_timer_active = false;
static uint32_t sleep_timer_start_tick = 0;
static lv_obj_t * quick_drawer_sleep_icon;
static lv_obj_t * quick_drawer_sleep_label;
static void quick_drawer_sleep_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    sleep_timer_active = !sleep_timer_active;
    if (sleep_timer_active) {
        sleep_timer_start_tick = lv_tick_get();
        lv_image_set_src(quick_drawer_sleep_icon, asset_path("pull_down/sleep_switch_s.png"));
        lv_label_set_text_fmt(quick_drawer_sleep_label, "%dm", current_settings.sleep_timer_minutes);
        lv_obj_remove_flag(quick_drawer_sleep_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_image_set_src(quick_drawer_sleep_icon, asset_path("pull_down/sleep_switch.png"));
        lv_obj_add_flag(quick_drawer_sleep_label, LV_OBJ_FLAG_HIDDEN);
    }
    quick_drawer_mark_snapshot_dirty();
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
        quick_drawer_mark_snapshot_dirty();
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
    lv_obj_set_y(quick_drawer_bitmap_motion ? quick_drawer_motion_image : quick_drawer, v);
}

static int32_t quick_drawer_motion_y(void) {
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
    lv_obj_set_y(quick_drawer_motion_image, lv_obj_get_y(quick_drawer));
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
    lv_obj_set_y(quick_drawer, quick_drawer_open ? 0 : -h);
    lv_obj_remove_flag(quick_drawer, LV_OBJ_FLAG_HIDDEN);
    if (quick_drawer_motion_image) {
        lv_obj_remove_flag(quick_drawer_motion_image, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(quick_drawer_motion_image, LV_OBJ_FLAG_HIDDEN);
    }
    quick_drawer_bitmap_motion = false;
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
    quick_drawer_begin_bitmap_motion();
    lv_obj_move_foreground(quick_drawer); /* above regular screens/volume popup while showing */
    /* ...but the status bar (clock/battery/wifi/bt) stays above THAT --
     * real-hardware feedback wanted it to stay visible/readable the whole
     * time the drawer is open, not get covered by it. quick_drawer's own
     * pull_down/bg.png is opaque black for the first ~59px anyway (measured
     * directly off the asset), so the status bar ends up sitting on that as
     * a backdrop rather than on anything from the screen underneath. */
    lv_obj_move_foreground(status_bar_band);
    /* Real-device bug report: "drawer animation is sluggish" -- root cause
     * was two (or more) of these animations running concurrently, not a
     * rendering-speed problem. lv_anim_start() only dedupes same-var/
     * same-exec_cb animations via its own early_apply path (see
     * remove_concurrent_anims() in lv_anim.c), which this never opts into
     * -- so a second open/close triggered before a prior 120ms animation
     * finished (an easy thing to do with a quick double-flick, or
     * re-grabbing the drawer to drag again right after a release-snap)
     * left BOTH animations alive, each calling quick_drawer_anim_y_cb with
     * its own diverging interpolated Y every tick and visibly fighting
     * each other -- indistinguishable from slow/janky rendering unless you
     * know to look for it. Explicitly cancelling any prior animation on
     * this exact (var, exec_cb) pair before starting a new one removes
     * that race entirely. */
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
static int32_t quick_drawer_drag_touch_start_y = 0;
static int32_t quick_drawer_drag_panel_start_y = 0;
static int32_t quick_drawer_last_velocity = 0;
#define QUICK_DRAWER_FLICK_VELOCITY 12 /* px/tick (~750px/s at the ~16ms poll rate) -- fast enough to read as an intentional flick */
#define QUICK_DRAWER_DRAG_DEADZONE 10 /* matches LVGL's own LV_INDEV_DEF_SCROLL_LIMIT -- see poll_quick_drawer_drag()'s comment */

/* Home-indicator swipe-up tracking -- see build_home_indicator_bar()'s own
 * comment for why this rides alongside the quick drawer's own drag tracking
 * in poll_quick_drawer_drag() below instead of an LV_EVENT_GESTURE handler.
 * Independent of quick_drawer_drag_tracking above (a press can only ever be
 * the start of one or the other, decided by where it lands). */
static bool home_swipe_tracking = false;
static int32_t home_swipe_start_y = 0;
static bool home_swipe_triggered = false; /* fires nav_reset_to_home() at most once per press, even if the finger keeps moving past the threshold */
#define HOME_SWIPE_UP_THRESHOLD 40 /* px the finger must move up from its start point before this counts as a swipe, not a stray tap in the band */

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
 * scroll, or the existing swipe-RIGHT-to-back gesture, still handled the
 * old event-based way since only entering the player needed this). */
static bool player_swipe_candidate = false;
static bool player_swipe_tracking = false;
static int32_t player_swipe_touch_start_x = 0;
static int32_t player_swipe_touch_start_y = 0;
static int32_t player_swipe_last_v = 0; /* last x actually applied to img_from, for per-tick velocity -- same idea as quick_drawer_last_velocity */
static int32_t player_swipe_last_velocity = 0;
static slide_transition_ctx_t * player_swipe_ctx = NULL;
#define PLAYER_SWIPE_DEADZONE 20 /* px before judging direction -- comfortably under LVGL's own ~50px built-in gesture threshold (LV_INDEV_DEF_GESTURE_LIMIT) so this always claims a genuine left-swipe before LVGL's own dormant gesture recognition would have */
#define PLAYER_SWIPE_FLICK_VELOCITY 12 /* same scale/reasoning as QUICK_DRAWER_FLICK_VELOCITY */

/* Forward declarations -- both fully built later in this file, needed here
 * so poll_quick_drawer_drag() below can exclude the home-swipe gesture
 * while either DAC overlay is active (see its own comment on why). */

/* Drives the quick drawer's open/close by directly following the finger's
 * raw Y position every tick -- "dynamic", per real-hardware feedback,
 * rather than an instant threshold-triggered animation -- snapping to fully
 * open or fully closed only once the finger actually lifts.
 *
 * Polled from its own dedicated fast lv_timer (see gui_init()) rather than
 * update_timer_cb's existing 500ms one, or driven by touch events, for two
 * separate reasons discovered in that order: first, LV_EVENT_PRESSED/
 * RELEASED/CLICKED/LONG_PRESSED are the only events LVGL ever dispatches to
 * an indev's own event list regardless of which object was actually hit
 * (confirmed directly in lv_indev.c's send_event()) -- LV_EVENT_PRESSING is
 * deliberately NOT among them, so a first attempt (an indev-wide
 * LV_EVENT_PRESSING handler) silently never fired at all, and the attempt
 * before THAT (the drawer's own LV_EVENT_GESTURE handler, relying on
 * enable_gesture_bubble_recursive()) got swallowed whenever the swipe
 * started on the 300px-wide brightness slider, deliberately excluded from
 * gesture-bubbling for the same reason every other screen excludes its own
 * sliders. Reading the indev's raw position directly instead sidesteps
 * hit-testing entirely. Second, once switched to polling, real-device
 * testing showed the drag still wasn't followed smoothly: it turned out
 * update_timer_cb's 500ms period is far slower than a typical swipe (well
 * under 300ms start to finish), so it was only ever sampling zero or one
 * point per gesture -- hence this gets its own ~60fps timer instead. */
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

/* Same drag-adjust widget set enable_gesture_bubble_recursive() excludes
 * from swipe-bubbling (sliders/switches/dropdowns/rollers), checked here
 * for the player-swipe candidate below -- that check is unrelated to
 * GESTURE_BUBBLE (this whole file's raw-indev-polling swipe detectors
 * don't go through LVGL's event/bubbling system at all, per this
 * function's own doc comment on why). Real-device feedback: dragging the
 * Idle Shutdown timeout slider leftward (its natural adjustment
 * direction) was randomly getting hijacked mid-drag into a "swipe to
 * player screen" transition once the horizontal movement crossed
 * PLAYER_SWIPE_DEADZONE, abandoning the slider adjustment -- this was
 * never about GESTURE_BUBBLE at all, it's a completely separate polling
 * loop with no per-widget exclusions of its own. lv_indev_get_active_obj()
 * is the object LVGL's own input processing most recently hit-tested a
 * press against, so this reflects whatever's actually under the finger
 * right now, not just this timer's own idea of screen layout. */
bool active_press_is_over_drag_adjust_widget(void) {
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

/* active_press_is_over_drag_adjust_widget() alone wasn't enough: real-
 * device feedback after that fix still showed a press starting on a
 * slider card's background -- near the slider but not precisely inside
 * its own hit-test box -- still hijacked into a player-swipe. Root cause:
 * every one of these cards is deliberately built WITHOUT
 * LV_OBJ_FLAG_CLICKABLE (matching finalize_screen_navigation()'s own
 * comment on plain lv_obj_create() objects), so a press on the card's
 * background hit-tests straight through to the screen itself --
 * lv_indev_get_active_obj() then returns the screen, indistinguishable
 * from a press on genuinely empty space that SHOULD navigate. Each of
 * these cards already marks itself as a swipe dead zone by having its
 * own LV_OBJ_FLAG_GESTURE_BUBBLE removed (for the separate GESTURE-event-
 * based back/down-swipe path) -- register_swipe_dead_zone() reuses that
 * same set of objects for this unrelated raw-polling path, checked by
 * raw point-in-rect instead of by hit-tested object identity so it
 * doesn't matter whether the card itself is clickable.
 *
 * Widened from 8 to 16: the original 8 slots are all native sliders (built
 * once at startup, live forever). plugin.show_settings_list()'s own slider
 * rows (gui_plugin_show_settings_list()) add real, bounded headroom on top
 * -- PLUGIN_SETTINGS_LIST_SCREEN_POOL_SIZE pool slots x PLUGIN_SETTINGS_
 * LIST_MAX_SLIDERS sliders each (plugin_manager.h) -- not unbounded growth,
 * since a pool slot's own slider cards are unregistered (see
 * unregister_swipe_dead_zone() below) before that slot is ever repopulated. */
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

bool point_in_swipe_dead_zone(lv_point_t p) {
    for (int i = 0; i < swipe_dead_zone_count; i++) {
        lv_obj_t * obj = swipe_dead_zones[i];
        if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) continue;
        if (lv_obj_get_screen(obj) != lv_screen_active()) continue;
        lv_area_t area;
        lv_obj_get_coords(obj, &area);
        if (p.x >= area.x1 && p.x <= area.x2 && p.y >= area.y1 && p.y <= area.y2) return true;
    }
    return false;
}

static bool player_swipe_press_excluded(lv_point_t p) {
    return active_press_is_over_drag_adjust_widget() || point_in_swipe_dead_zone(p);
}

static void poll_quick_drawer_drag(lv_timer_t * timer) {
    lv_indev_t * indev = find_pointer_indev();
    if (!indev) return;

    bool pressed = lv_indev_get_state(indev) == LV_INDEV_STATE_PRESSED;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    int32_t h = lv_display_get_vertical_resolution(lv_display_get_default());

    if (pressed && !quick_drawer_was_pressed) {
        /* Cancel any release-snap animation still in flight -- re-grabbing
         * the drawer right after a flick (within its 120ms animation
         * window) used to leave that old animation alive, fighting this
         * new drag's own direct lv_obj_set_y() calls every tick (same
         * underlying issue as open_quick_drawer()'s own comment on
         * concurrent animations). Harmless/cheap no-op when nothing's
         * actually animating. */
        lv_anim_delete(quick_drawer, quick_drawer_anim_y_cb);

        /* A new press just started. If the drawer's already open, any press
         * anywhere could be the start of a close-drag -- a plain tap on a
         * button/slider still works fine, since a tap has ~0 movement and
         * this only ever repositions the drawer by that same ~0 delta. If
         * it's closed, only a press starting near the very top edge
         * counts, so e.g. scrolling a list elsewhere never accidentally
         * drags it open. Panel start reference is the drawer's ACTUAL
         * current Y (not a fixed 0/-h assumption) -- with the animation
         * just cancelled above, that could be mid-flight anywhere between
         * fully open and fully closed, not only at one of the two
         * endpoints. */
        if (quick_drawer_open) {
            quick_drawer_drag_tracking = true;
            quick_drawer_drag_panel_start_y = quick_drawer_motion_y();
        } else if (p.y <= QUICK_DRAWER_TRIGGER_ZONE && !gui_library_has_background_work()) {
            /* !gui_library_has_background_work() -- same exclusion as the other rescan-
             * time guards below (search this file for library_rescan_active
             * for the rest of them); only blocks starting a NEW open-drag,
             * if the drawer somehow got dragged open right as a rescan
             * started, the quick_drawer_open branch above still lets it be
             * dragged closed again. */
            quick_drawer_drag_tracking = true;
            quick_drawer_drag_panel_start_y = quick_drawer_motion_y();
            lv_obj_move_foreground(quick_drawer); /* above regular screens/volume popup while dragging into view */
            lv_obj_move_foreground(status_bar_band); /* but the status bar stays above THAT -- see open_quick_drawer()'s comment */
        } else {
            quick_drawer_drag_tracking = false;
        }
        quick_drawer_drag_claimed = false;
        quick_drawer_drag_touch_start_y = p.y;

        /* Home-indicator swipe-up: only a press starting within the band
         * itself counts, matching the Android gesture-bar convention this
         * is modeled on (see build_home_indicator_bar()) -- a press
         * anywhere else on screen never triggers this, even if it later
         * moves upward (e.g. an aborted attempt to scroll a list).
         * Real-device bug report: with the drawer already open, ANY press
         * is presumed to be the start of a close-drag (see quick_drawer_open
         * branch just above) regardless of where on screen it starts --
         * competing for the same swipe-up gesture let a single upward drag
         * both close the drawer AND jump to Home. Excluded outright while
         * the drawer is open; the drawer itself always wins that gesture
         * there.
         * Real-device bug report: swiping up to Home while Bluetooth DAC
         * mode was active left DAC mode itself still on (bluealsa/bt-agent
         * still running, phone still "connected") with no way back to it
         * short of re-entering Settings and toggling DAC mode off and back
         * on -- only the DAC overlay's own back button is supposed to be
         * able to leave it, since that's the only path that actually tears
         * DAC mode down (see bt_dac_overlay_back_cb()). Excluded here the
         * same way the drawer already is; USB DAC mode has the identical
         * "only the back button exits" design (build_usb_dac_overlay_screen())
         * and shares the same gap fixed here.
         * Same exclusion for library_rescan_active (Settings > Update Music
         * Database) -- nav_reset_to_home() here would leave the rescan
         * thread running against arrays the home/library screens themselves
         * are about to read, same crash as the quick-drawer exclusion just
         * above. This makes the busy screen fully undismissable while a
         * rescan is in flight -- it has no swipe-to-back gesture wired
         * either (build_subsonic_downloading_screen() never calls
         * finalize_screen_navigation()), so this was the only remaining way
         * off it. */
        home_swipe_tracking = current_settings.swipe_up_home_enabled && !quick_drawer_open &&
                               lv_screen_active() != gui_network_get_bt_dac_overlay() &&
                               lv_screen_active() != gui_network_get_usb_dac_overlay() &&
                               lv_screen_active() != gui_lyrics_get_screen() &&
                               !gui_library_has_background_work() &&
                               p.y >= h - HOME_INDICATOR_BAND_HEIGHT;
        home_swipe_start_y = p.y;
        home_swipe_triggered = false;

        /* Player-swipe: eligible unless this exact press already got
         * claimed by the drawer-drag above (quick_drawer_drag_tracking),
         * the drawer is open (its own close-drag owns every press while
         * open), the player screen is already the one showing (nothing
         * to swipe TO), or the press actually started on a slider/switch/
         * dropdown/roller or one of the registered slider-card dead zones
         * (see player_swipe_press_excluded()'s own comment -- dragging a
         * slider leftward, or starting the drag on its card's background,
         * was getting mistaken for this gesture). Real direction isn't
         * knowable from a single point -- judged once real movement
         * accumulates, below -- so this only marks the press as a
         * CANDIDATE, not yet a confirmed drag. Also excluded while
         * library_rescan_active, same reasoning as the quick-drawer/
         * home-swipe exclusions above -- this is the last remaining swipe
         * gesture that could navigate off the rescan's busy screen. Also
         * excluded on gui_lyrics_get_screen() -- real-device feedback: triggering
         * this gesture from a screen that's already reached FROM the
         * player screen looked like a broken, looping transition; lyrics_
         * gesture_event_cb() is the only swipe this screen responds to. */
        player_swipe_candidate = !quick_drawer_drag_tracking && !quick_drawer_open &&
                                  lv_screen_active() != gui_player_get_screen() &&
                                  lv_screen_active() != gui_lyrics_get_screen() &&
                                  !gui_library_has_background_work() &&
                                  !player_swipe_press_excluded(p);
        player_swipe_touch_start_x = p.x;
        player_swipe_touch_start_y = p.y;
        player_swipe_tracking = false;
    }

    if (pressed && home_swipe_tracking && !home_swipe_triggered && home_swipe_start_y - p.y >= HOME_SWIPE_UP_THRESHOLD) {
        home_swipe_triggered = true;
        nav_reset_to_home();
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
                /* Real-device bug report: on a screen whose list is short
                 * enough to need no scrolling at all (confirmed case: the
                 * Settings home category list), a vertical swipe attempt
                 * lands here (ruled out as a player-swipe, since it isn't
                 * horizontal-left-dominant) with nothing scrollable to
                 * claim it either -- LVGL only cancels a pending click once
                 * some object's own lv_indev_get_scroll_obj() claims the
                 * drag as a real scroll, so a swipe that travelled well
                 * past PLAYER_SWIPE_DEADZONE but found nothing to scroll
                 * still resolved as a plain tap on whatever row the finger
                 * started on, firing that row's own action. Suppressing the
                 * eventual click here, same tool as the confirmed-swipe
                 * branch above, whenever the drag went far enough to
                 * clearly not be a stationary tap.
                 * Real-device bug report #2: this originally fired for ANY
                 * non-left-confirmed direction, including rightward -- which
                 * silently broke swipe-to-go-back (screen_gesture_event_cb's
                 * own LV_DIR_RIGHT handling), since lv_indev_wait_release()
                 * called this early (past this function's own 20px
                 * PLAYER_SWIPE_DEADZONE) pre-empted LVGL's own native
                 * gesture recognition before it could reach its ~50px
                 * LV_INDEV_DEF_GESTURE_LIMIT and fire the real
                 * LV_EVENT_GESTURE. Restricted to ady > adx (clearly
                 * vertical, matching the actual "tried to scroll" bug this
                 * fixes) so a horizontal drag -- rightward (back) or
                 * leftward-but-not-quite-dominant-yet -- is left completely
                 * alone here and keeps reaching LVGL's own gesture handling
                 * normally.
                 * Left alone either way (no suppression) when
                 * lv_indev_get_scroll_obj() IS set -- that's a real, working
                 * scroll already in progress, and forcing an early release
                 * there would cut its motion off mid-drag. */
                lv_indev_wait_release(indev);
            }
            player_swipe_candidate = false;
        }
    }

    if (pressed && player_swipe_tracking) {
        int32_t w = lv_display_get_horizontal_resolution(lv_display_get_default());
        int32_t v = p.x - player_swipe_touch_start_x;
        if (v > 0) v = 0;   /* never past fully-open (finger drifting back right of the start point just holds at 0) */
        if (v < -w) v = -w; /* never past fully-off (finger overshooting left of a full screen width) */
        player_swipe_last_velocity = v - player_swipe_last_v;
        player_swipe_last_v = v;
        slide_transition_anim_x_cb(player_swipe_ctx, v);
    }

    if (pressed && quick_drawer_drag_tracking) {
        /* Deadzone before actually moving the panel -- real-device feedback:
         * long-pressing a drawer icon (wifi/bt) wasn't opening its settings
         * screen at all. Root cause, confirmed by reading indev_proc_press()
         * in lv_indev.c: it re-hit-tests the SAME raw screen point on every
         * tick, and if that now resolves to a different object than the one
         * originally pressed, LVGL sends PRESS_LOST and resets the press --
         * killing the long-press timer before it can fire. Moving the whole
         * drawer (and everything on it, including the icon under the
         * finger) by even a couple of px in response to ordinary touch
         * jitter during a "held still" long-press was exactly triggering
         * that. 10px matches LVGL's own LV_INDEV_DEF_SCROLL_LIMIT -- the
         * same threshold it uses internally to tell a stationary press from
         * an intentional scroll/drag. Below it, the panel doesn't move at
         * all, so a tap or long-press on a child stays stable under the
         * finger; past it, the deadzone amount is subtracted so dragging
         * starts smoothly from zero rather than jumping. */
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
            quick_drawer_last_velocity = new_y - quick_drawer_motion_y();
            quick_drawer_anim_y_cb(quick_drawer, new_y);
        } else {
            quick_drawer_last_velocity = 0;
        }
    }

    if (!pressed && quick_drawer_was_pressed && quick_drawer_drag_tracking) {
        /* Finger lifted. A fast flick -- real-hardware feedback: "a quick
         * swap [that] goes back to closed"/"keeps open" -- often doesn't
         * travel far enough to cross the halfway rubber-band point before
         * the finger leaves, even though the user's intent was obvious from
         * how fast it moved. Falls back to the halfway position check only
         * for a slow/undecided drag that ends with little to no velocity. */
        quick_drawer_drag_tracking = false;
        bool snap_open;
        if (quick_drawer_last_velocity > QUICK_DRAWER_FLICK_VELOCITY) {
            snap_open = true; /* still moving down at release */
        } else if (quick_drawer_last_velocity < -QUICK_DRAWER_FLICK_VELOCITY) {
            snap_open = false; /* still moving up at release */
        } else {
            snap_open = quick_drawer_motion_y() > -h / 2;
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

    if (!pressed && quick_drawer_was_pressed && player_swipe_tracking) {
        /* Finger lifted mid-swipe. Same flick-vs-halfway decision as the
         * drawer's own release logic just above, just horizontal. */
        player_swipe_tracking = false;
        int32_t w = lv_display_get_horizontal_resolution(lv_display_get_default());
        /* player_swipe_last_v already holds exactly this value -- it's set
         * to the same v applied via slide_transition_anim_x_cb() every tick
         * just above, before either the LVGL-object or compositor path
         * consumes it. Reading it back here directly (instead of
         * lv_obj_get_x(player_swipe_ctx->img_from)) is required, not just
         * simpler, now that img_from can be NULL -- TRANSITION_PERFORMANCE_
         * PLAN.md Phase 3's compositor path skips creating it entirely (see
         * begin_slide_transition()'s own comment); real-device crash log
         * confirmed lv_obj_get_x(NULL) -> SIGSEGV (invalid read from 0x14,
         * lv_obj_get_x()'s own offset into a null lv_obj_t) the first time
         * this was reached under compositor mode. */
        int32_t current_v = player_swipe_last_v;
        bool commit;
        if (player_swipe_last_velocity < -PLAYER_SWIPE_FLICK_VELOCITY) {
            commit = true; /* still moving left fast at release */
        } else if (player_swipe_last_velocity > PLAYER_SWIPE_FLICK_VELOCITY) {
            commit = false; /* still moving back right fast at release */
        } else {
            commit = current_v < -w / 2; /* past halfway, slow/undecided release */
        }
        player_swipe_ctx->commit = commit;
        if (commit) {
            /* Stack bookkeeping mirroring nav_push()'s own -- the real
             * lv_screen_load() happens inside slide_transition_done_cb()
             * once this settle animation finishes, not here; nothing else
             * reads the nav stack before then, so only the bookkeeping
             * needs to be right immediately. */
            if (!gui_navigation_is_top(gui_player_get_screen())) {
                nav_push(gui_player_get_screen());
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

    quick_drawer_was_pressed = pressed;

    /* Every release-handling branch above (drawer snap, player-swipe
     * settle) has already run by this point in the same call that observed
     * the release -- nothing left to track until resume_fast_gesture_
     * timers_cb() wakes this again on the next press-down. See this
     * timer's own handle comment for why pausing (not just letting the
     * ~60fps tick keep firing and no-op) is what actually matters here. */
    if (!pressed) lv_timer_pause(timer);
}

/* Forward declarations -- defined later in this file (with the player
 * screen's own transport buttons, which they were originally written for),
 * but the quick drawer's mini now-playing card reuses them verbatim. */
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
 * Real-device incident: LVGL still sends LV_EVENT_CLICKED on release even
 * when LV_EVENT_LONG_PRESSED already fired earlier in that same press --
 * confirmed directly in lv_indev.c's indev_proc_release(), which doesn't
 * check whether a long-press was already sent before deciding a release
 * without enough movement counts as a click. Without this flag, a
 * long-press opened the settings screen AND, on release, the click handler
 * fired right behind it and toggled the radio -- e.g. long-pressing the
 * wifi icon opened Wi-Fi settings but also turned wifi off. Each click
 * handler below checks and clears its own flag first, skipping its toggle
 * entirely when the long-press already handled this press. */
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

/* Real tap-to-toggle for the wifi icon -- unlike quick_drawer_bt_event_cb
 * (a purely local sprite swap, "no real backend" per its own comment), wifi
 * actually has one (wifi_control_enable()/disable()), it just was never
 * wired to anything that could turn it back OFF: the only existing caller,
 * wifi_scan_thread_func(), only ever enables it (auto-enabling when the
 * Wi-Fi settings screen is opened), so wifi previously could only ever go
 * from off to on, matching the "stays on all the time" real-device report.
 * enable()/disable() each block for about a second, so this runs on its own
 * thread, polled the same way as every other background op in this file. */
static pthread_t wifi_toggle_thread;
static atomic_bool wifi_toggle_done_flag = false;
static bool wifi_toggle_target_enabled = false;

static void * wifi_toggle_thread_func(void * arg) {
    (void) arg;
    bool turning_on = wifi_toggle_target_enabled;
    if (turning_on) wifi_control_enable();
    else wifi_control_disable();

    /* Real-device bug: wifi_control_is_enabled() (a plain access() check on
     * wpa_supplicant's control socket) can still read the OLD state for a
     * moment right after wifi_on.sh/wifi_off.sh return -- the script
     * finishing doesn't guarantee the socket has actually been created/
     * removed yet. Confirmed live: the optimistic UI flip (quick_drawer_
     * wifi_event_cb) briefly reverted to the old state once poll_wifi_
     * toggle()'s own check landed too early against this still-settling
     * socket, then corrected itself again shortly after -- a visible
     * on/off/on bounce with no user action in between. Retrying here
     * instead of trusting the first read means poll_wifi_toggle()'s check
     * lands on the real, settled state instead. */
    for (int i = 0; i < 10 && wifi_control_is_enabled() != turning_on; i++) {
        usleep(300000);
    }

    atomic_store_explicit(&wifi_toggle_done_flag, true, memory_order_release); /* written last -- poll_wifi_toggle only checks this flag */
    return NULL;
}

/* populate_wifi_screen declared in gui.h */ /* defined with the rest of the Wi-Fi settings screen, below */

void quick_drawer_wifi_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (quick_drawer_wifi_long_press_fired) { /* see quick_drawer_wifi_long_press_cb()'s own comment */
        quick_drawer_wifi_long_press_fired = false;
        return;
    }
    if (wifi_toggle_active) return; /* already toggling -- ignore taps until it lands */
    bool wifi_will_be_enabled = !wifi_control_is_enabled();
    wifi_toggle_active = true;
    atomic_store_explicit(&wifi_toggle_done_flag, false, memory_order_relaxed);
    wifi_toggle_target_enabled = wifi_will_be_enabled;

    /* Optimistic sprite flip -- wifi_control_is_enabled() is a plain
     * access() check (see its own comment), not a subprocess spawn, so
     * it's cheap enough to call synchronously right here. The actual
     * radio toggle below can take a couple seconds; flipping the icon
     * immediately instead of waiting for poll_wifi_toggle() to confirm it
     * is what makes the tap read as instant. poll_wifi_toggle() still
     * re-reads the real state once the thread lands and corrects this if
     * the toggle unexpectedly failed. */
    lv_image_set_src(quick_drawer_wifi_icon, asset_path(wifi_will_be_enabled ? "pull_down/wifi_s.png" : "pull_down/wifi.png"));
    quick_drawer_mark_snapshot_dirty();

    /* Real-device bug report: the topbar Wi-Fi icon stayed frozen (hidden,
     * or showing whatever signal-strength sprite it had before toggling
     * off) until poll_wifi_toggle()'s own refresh_wifi_icon() call landed --
     * same delay-to-first-feedback bug already fixed for Bluetooth's own
     * topbar icon in quick_drawer_bt_event_cb(), same fix here: flip it
     * optimistically alongside the drawer icon just above. ON shows the
     * disconnected sprite (a fresh toggle-on can't be associated to an AP
     * yet); OFF hides it outright. refresh_wifi_icon() overwrites this with
     * the real, settled state (association status included) once
     * wifi_control_enable()/disable() actually finishes. */
    if (wifi_will_be_enabled) {
        lv_obj_remove_flag(wifi_icon, LV_OBJ_FLAG_HIDDEN);
        lv_image_set_src(wifi_icon, asset_path("topbar/wifi_unconnect.png"));
    } else {
        lv_obj_add_flag(wifi_icon, LV_OBJ_FLAG_HIDDEN);
    }
    sync_topbar_status_icon_positions();

    /* Optimistically rebuild the whole Wi-Fi settings screen too (not just
     * the toggle row) when that's the screen showing -- real-device
     * feedback: flipping just the row's own sprite left the Wi-Fi Info/
     * Manual SSID Entry/Memorized Networks rows (populate_wifi_screen()'s
     * own enabled-gated content) not appearing until wifi_control_enable()
     * actually finished (~1-3s), which read as the screen "taking a while"
     * even though the toggle itself looked instant. Passing the optimistic
     * wifi_will_be_enabled here (not a real wifi_control_is_enabled() call,
     * which would still read the pre-toggle state) makes the whole screen
     * -- toggle row and its gated content -- appear immediately, exactly
     * like the eventual real rebuild will look on success.
     * poll_wifi_toggle() still re-populates with the real, authoritative
     * state once wifi_control_enable()/disable() actually lands, correcting
     * this if the toggle unexpectedly failed. */
    /* Do not clean/rebuild wifi_list from inside the clicked row's own
     * event callback: doing so deletes the event target while LVGL is still
     * dispatching through it. poll_wifi_toggle() performs the authoritative
     * rebuild as soon as the worker settles. */
    if (pthread_create(&wifi_toggle_thread, NULL, wifi_toggle_thread_func, NULL) != 0) {
        wifi_toggle_active = false;
        refresh_wifi_icon();
    start_bt_dac_startup_reapply_if_needed();
        if (gui_navigation_is_top(gui_network_get_wifi_screen()))
            populate_wifi_screen(wifi_control_is_enabled());
    }
}

static void poll_wifi_toggle(void) {
    if (!wifi_toggle_active || !atomic_load_explicit(&wifi_toggle_done_flag, memory_order_acquire)) return;
    wifi_toggle_active = false;
    pthread_join(wifi_toggle_thread, NULL);
    bool enabled = wifi_control_is_enabled();
    refresh_wifi_icon(); /* re-reads the real state -- updates both the status bar and drawer icons */
    populate_wifi_screen(enabled); /* the Wi-Fi screen's own toggle row + everything gated on it needs the same refresh */
    if (enabled != wifi_toggle_target_enabled) show_error_toast("Wi-Fi failed to change state");
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

static void * bt_toggle_thread_func(void * arg) {
    (void) arg;
    bt_toggle_forced_dac_off = false;
    bool turning_on = !bt_control_is_powered();
    bool chip_wedged = false;
    if (!turning_on) {
        /* Real-device incident: turning Bluetooth off via the quick drawer
         * while Bluetooth DAC mode was still on left bluealsa/bt-agent
         * (spawned by bt_control_apply_output_settings() when DAC mode
         * turned on) running orphaned against an adapter that was about to
         * be powered off out from under them -- confirmed to corrupt
         * bluetoothd's own adapter registration (hci0 stayed up fine at
         * the kernel level, but bluetoothd stopped seeing it entirely,
         * "No default controller available", and Bluetooth couldn't be
         * re-enabled again until bluetoothd was manually restarted). Tear
         * DAC mode's processes down first, same call
         * bt_dac_leave_confirm_cb() uses, before disabling the radio. */
        if (current_settings.bt_dac_mode_enabled) {
            bt_control_apply_output_settings(false, current_settings.bt_volume_sync_enabled);
            bt_toggle_forced_dac_off = true;
        }
        bt_control_disable();
    } else {
        /* Real-device incident: enabling Bluetooth when bt_resume can't
         * actually bring hci0 up (a wedged BT chip -- confirmed live,
         * "Can't get device info: No such device" surviving repeated
         * bt_resume retries, needing a real power cycle to clear) used to
         * call bt_control_enable() anyway, adding a second full ~15s bounded
         * subprocess_run() wait against an adapter already known not to
         * exist -- with no busy screen at all (see quick_drawer_bt_event_cb's
         * own comment on why), the whole ~45s combined stall read to the
         * user as the device having frozen. Skipping the pointless second
         * wait here doesn't fix the underlying wedge (nothing in userspace
         * can), but at least stops doubling how long the unresponsive-
         * feeling wait lasts. */
        if (bt_control_init_chip()) bt_control_enable();
        else chip_wedged = true;
    }

    /* Real-device bug: bt_control_is_powered() (bluetoothctl show) can
     * still read the OLD state for a moment right after bt_control_enable()/
     * disable() return -- the command completing doesn't mean bluetoothd
     * has actually finished updating the adapter's reported Powered state
     * yet. Confirmed live: the optimistic UI flip (quick_drawer_bt_
     * event_cb) briefly reverted to the old state once poll_bt_toggle()'s
     * own start_refresh_bt_icon() check landed too early against this
     * still-settling state, then corrected itself again on the next
     * periodic poll a few seconds later -- a visible on/off/on bounce with
     * no user action in between. Retrying here instead of trusting the
     * first read means that check lands on the real, settled state
     * instead. Skipped entirely for the known-wedged-chip case
     * (chip_wedged) -- retrying there would just be the same pointless
     * wait the comment above already avoids, since the chip genuinely
     * isn't coming up. */
    if (!chip_wedged) {
        for (int i = 0; i < 5 && bt_control_is_powered() != turning_on; i++) {
            sleep(1);
        }
    }

    atomic_store_explicit(&bt_toggle_done_flag, true, memory_order_release); /* written last -- poll_bt_toggle only checks this flag */
    return NULL;
}

/* Real-device incident: a tap that landed while bt_init's own chip flash
 * was still genuinely in progress used to just be refused outright (see
 * BT_INIT_OK_FLAG_PATH's own comment for the actual chip-wedging risk that
 * guards against) -- functionally safe, but made the user re-tap
 * themselves once it settled, AND gave no visual feedback at all that
 * anything had registered (the drawer's own icon never flipped, unlike a
 * normal toggle). This queues the intent properly instead: waits for
 * BT_INIT_OK_FLAG_PATH, then waits for two real powered-off observations
 * before performing the exact same turn-on sequence bt_toggle_thread_func()'s
 * own turning-on path uses, automatically, no second tap needed. Waiting
 * for the settled-off state matters: bt_init_ok can become visible just
 * before the boot sequence's final disable propagates through bluetoothd,
 * and enabling in that gap lets the final disable erase the user's intent.
 * Deliberately
 * NOT the same thing as the unconditional-auto-enable-at-boot approach
 * tried and reverted earlier -- this only ever fires because the user
 * explicitly asked to turn Bluetooth on, just before it was safe to.
 *
 * Reuses bt_toggle_active/bt_toggle_thread/bt_toggle_done_flag/
 * poll_bt_toggle() -- the exact same in-flight-toggle bookkeeping
 * bt_toggle_thread_func() already uses -- rather than a separate parallel
 * flag, specifically so quick_drawer_bt_event_cb()'s own optimistic icon
 * flip (and populate_bt_screen() guard) apply here too automatically,
 * fixing the "no visual clue" gap. poll_bt_toggle()'s own
 * start_refresh_bt_icon() call at the end is exactly what's wanted here
 * too: a fresh real-state poll once this either succeeds or times out.
 * Capped at BT_BOOT_ENABLE_MAX_WAIT_MS so a genuinely wedged/never-
 * finishing bt_init doesn't leave this polling forever. */
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
                    if (bt_control_init_chip()) bt_control_enable();
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
        if (bt_control_init_chip()) bt_control_enable();
    }
    atomic_store_explicit(&bt_toggle_done_flag, true, memory_order_release); /* written last -- poll_bt_toggle only checks this flag */
    return NULL;
}

void quick_drawer_bt_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (quick_drawer_bt_long_press_fired) { /* see quick_drawer_bt_long_press_cb()'s own comment */
        quick_drawer_bt_long_press_fired = false;
        return;
    }
    if (bt_toggle_active) return; /* already toggling -- ignore taps until it lands */

    bool bt_will_be_powered = !bt_is_powered_cached;

    /* Real-device incident: turning Bluetooth ON before BT_INIT_OK_FLAG_PATH
     * exists raced this app's own bt_control_init_chip() against bt_init's
     * own still-in-progress UART chip firmware flash and genuinely wedged
     * the chip (unrecoverable without a full power cycle) -- see its own
     * comment. Turning OFF is always safe (bt_control_disable() is
     * D-Bus-only, no chip-level operation), so this only ever affects the
     * turning-on direction: bt_pending_now queues bt_toggle_thread_func()'s
     * usual work behind bt_pending_enable_thread_func() instead of running
     * it directly, but everything below (icon flip, cached value,
     * settings-screen refresh, bt_toggle_active bookkeeping) is identical
     * either way -- exactly the fix for the earlier version of this, which
     * showed no visual feedback at all for a tap that landed too early. */
    bool bt_pending_now = bt_will_be_powered && access(BT_INIT_OK_FLAG_PATH, F_OK) != 0;

    /* Ends the early-boot display suppression immediately on a real user
     * tap -- see bt_boot_suppress_active()'s own comment for why this
     * matters (a manual tap shouldn't be fighting a window that doesn't
     * know the user has acted). No-op once suppression has already ended
     * on its own. */
    bt_boot_suppress_enabled = false;
    bt_boot_off_observations = 0;

    bt_toggle_active = true;
    atomic_store_explicit(&bt_toggle_done_flag, false, memory_order_relaxed);

    /* Optimistic sprite flip, same reasoning as quick_drawer_wifi_event_cb's
     * own comment -- bt_is_powered_cached (kept fresh by
     * poll_refresh_bt_icon()) is a plain bool read, not the subprocess spawn
     * bt_control_is_powered() itself is, so it's safe to read synchronously
     * here. Turning on cold can take ~10-13s (bt_control_init_chip()), or
     * however long is left of bt_init's own run if bt_pending_now; this is
     * what makes the tap itself read as instant instead of the icon
     * sitting frozen until poll_bt_toggle() confirms the real state once
     * the thread lands. */
    lv_image_set_src(quick_drawer_bt_icon, asset_path(bt_will_be_powered ? "pull_down/bt_s.png" : "pull_down/bt.png"));
    quick_drawer_mark_snapshot_dirty();

    /* Real-device bug report: the topbar Bluetooth icon stayed frozen on
     * whatever it showed pre-toggle (e.g. still the "connected" sprite
     * after manually turning Bluetooth off) until poll_bt_toggle()'s own
     * follow-up start_refresh_bt_icon() subprocess round trip finally
     * landed -- a real, user-visible delay, not just the ~10-13s cold-boot
     * chip-init case: even turning OFF (bt_control_disable() is D-Bus-only,
     * no chip op, so the toggle thread itself finishes fast) still waited
     * on that separate re-check. Flip it here too, same as the drawer icon
     * just above: OFF hides it outright (nothing to be connected to), ON
     * shows the disconnected sprite since a fresh toggle-on can't have an
     * active connection yet -- poll_refresh_bt_icon() overwrites both with
     * the real, settled state once its own check lands (it already skips
     * doing so while bt_toggle_active, see its own comment). */
    if (bt_will_be_powered) {
        lv_obj_remove_flag(bt_status_icon, LV_OBJ_FLAG_HIDDEN);
        lv_image_set_src(bt_status_icon, asset_path("topbar/bluetooth_unconnect.png"));
        lv_obj_add_flag(a2dp_status_icon, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(bt_status_icon, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(a2dp_status_icon, LV_OBJ_FLAG_HIDDEN);
    }
    sync_topbar_status_icon_positions();

    /* Optimistically rebuild the whole Bluetooth settings screen too (not
     * just the toggle row) when that's the screen showing -- same
     * "screen takes a while to appear" real-device feedback as
     * quick_drawer_wifi_event_cb's own comment, actually worse here: the
     * real path is bt_toggle_thread_func() (up to ~10-13s cold) followed by
     * a SEPARATE start_refresh_bt_icon() subprocess round-trip
     * (poll_bt_toggle() doesn't call populate_bt_screen() itself -- see its
     * own comment) before bt_is_powered_cached actually catches up and the
     * screen naturally rebuilds. Setting bt_is_powered_cached directly here
     * is safe: poll_refresh_bt_icon() (the only other writer) skips its own
     * populate_bt_screen() call entirely while bt_toggle_active is true
     * (see its own comment on this exact race), so nothing overwrites this
     * until start_refresh_bt_icon()'s real result lands afterward and
     * correctly finalizes it. */
    bt_is_powered_cached = bt_will_be_powered;
    if (gui_navigation_is_top(gui_network_get_bt_screen())) populate_bt_screen();

    /* Runs fully in the background, same as the stock player -- no busy
     * screen. An earlier version pushed a "Turning on Bluetooth..."
     * interstitial here, shared with wifi/bt scan's own overlay -- see git
     * history if that's ever needed again, but it was also the source of a
     * real, repeatedly-hit stuck-screen bug (multiple uncoordinated users of
     * one shared overlay), which not having an overlay at all sidesteps
     * entirely. */
    if (pthread_create(&bt_toggle_thread, NULL, bt_pending_now ? bt_pending_enable_thread_func : bt_toggle_thread_func, NULL) != 0) {
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

/* Real-device incident: bt_dac_enable_row_cb() is the ONLY call site for
 * bt_control_apply_output_settings() -- current_settings.bt_dac_mode_enabled
 * is persisted to disk, so if it was left on at the end of a previous
 * session, this app's UI comes back up on next launch already showing the
 * toggle as on (and correctly blocking local playback via
 * external_dac_block_reason()), but the actual bluealsa/bt-agent processes
 * that make a Bluetooth DAC connection possible were never started this
 * boot -- confirmed on a real device: the toggle read "on", yet no device
 * could connect at all (bluealsa/bt-agent simply weren't running). This
 * reapplies the persisted state once at startup, mirroring
 * bt_toggle_thread_func() (chip init can take ~10-13s on a fresh boot, so
 * this can't block gui_init() / the UI thread). */
static pthread_t bt_dac_startup_reapply_thread;
static bool bt_dac_startup_reapply_active = false;
static atomic_bool bt_dac_startup_reapply_done_flag = false;

static void * bt_dac_startup_reapply_thread_func(void * arg) {
    (void) arg;
    bt_control_init_chip();
    bt_control_enable();
    bt_control_apply_output_settings(true, current_settings.bt_volume_sync_enabled);
    atomic_store_explicit(&bt_dac_startup_reapply_done_flag, true, memory_order_release); /* written last -- poll_bt_dac_startup_reapply only checks this flag */
    return NULL;
}

/* Called once from gui_init(), only if bt_dac_mode_enabled was already true
 * at load time (a fresh toggle-on tap already goes through
 * bt_dac_toggle_cb() directly and doesn't need this). */
static void start_bt_dac_startup_reapply_if_needed(void) {
    if (!current_settings.bt_dac_mode_enabled) return;
    bt_dac_startup_reapply_active = true;
    atomic_store_explicit(&bt_dac_startup_reapply_done_flag, false, memory_order_relaxed);
        if (pthread_create(&bt_dac_startup_reapply_thread, NULL, bt_dac_startup_reapply_thread_func, NULL) != 0) {
        bt_dac_startup_reapply_active = false;
    }
}


static void poll_bt_dac_startup_reapply(void) {
    if (!bt_dac_startup_reapply_active || !atomic_load_explicit(&bt_dac_startup_reapply_done_flag, memory_order_acquire)) return;
    bt_dac_startup_reapply_active = false;
    pthread_join(bt_dac_startup_reapply_thread, NULL);
    start_refresh_bt_icon();
}

/* Real-device incident: bt_dac_toggle_cb(), bt_volume_sync_toggle_cb(), and
 * airplay_toggle_cb() (when turning AirPlay on forces Bluetooth DAC off)
 * all called bt_control_apply_output_settings() directly from their LVGL
 * click handlers, i.e. on the UI thread -- despite that function's own doc
 * comment already saying "blocking, call off the UI thread". Confirmed on a
 * real device: it kills and respawns bluealsa/bt-agent/bluealsa-aplay with
 * two separate 500ms sleeps plus several subprocess spawns baked in, easily
 * 1-3+ seconds of pure blocking -- since lv_timer_handler() runs on this
 * same thread, that froze the entire UI (no redraws, no touch input at all)
 * for the whole duration, with whatever screen happened to be showing at
 * that instant stuck on screen looking unresponsive, no way to back out of
 * it, until the call finally returned. This backgrounds it the same way
 * every other slow Bluetooth/Wi-Fi operation in this file already is. */
static pthread_t bt_apply_output_settings_thread;
static bool bt_apply_output_settings_active = false;
static atomic_bool bt_apply_output_settings_done_flag = false;

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

/* pull_down/bg.png bakes in two fixed rounded panels (measured directly off
 * the asset: a vertical scan for opaque-color transitions at x=240 finds
 * flat color from y=59-338, a gap, then y=363-730; a horizontal scan finds
 * the fill spanning x=19-459 either way) -- everything below is positioned
 * against those measured bounds, not guessed, since anything placed outside
 * them draws on the plain black gap/margin around the panels instead of
 * inside the rounded box that's supposed to contain it (this is what was
 * actually wrong before: row 1's icons started at STATUS_BAR_CLEARANCE-8=40,
 * 19px above the real panel top of 59, and the card started at
 * STATUS_BAR_CLEARANCE+250=298, 65px above the real second panel's top of
 * 363). */
#define QUICK_DRAWER_PANEL1_TOP 59
#define QUICK_DRAWER_PANEL1_BOTTOM 338
#define QUICK_DRAWER_PANEL2_TOP 363

static void quick_drawer_brightness_changed_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * slider = (lv_obj_t *) lv_event_get_target(e);
    int32_t percent = lv_slider_get_value(slider);
    lv_obj_t * label = (lv_obj_t *) lv_event_get_user_data(e);

    if (code == LV_EVENT_VALUE_CHANGED) {
        backlight_set_normal_percent((int) percent); /* live feedback while dragging; also exits inactivity dim */
        lv_label_set_text_fmt(label, "%d%%", (int) percent);
        quick_drawer_mark_snapshot_dirty();
    } else if (code == LV_EVENT_RELEASED) {
        /* Only persist once the drag settles, not on every intermediate
         * tick -- same as volume_popup_track_event_cb/volume_slider_event_cb. */
        current_settings.brightness_percent = (int) percent;
        settings_save(&current_settings);
    }
}

static void build_quick_drawer(void) {
    int32_t w = lv_display_get_horizontal_resolution(lv_display_get_default());
    int32_t h = lv_display_get_vertical_resolution(lv_display_get_default());

    quick_drawer = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(quick_drawer);
    lv_obj_set_size(quick_drawer, w, h);
    lv_obj_set_pos(quick_drawer, 0, -h); /* fully off-screen above until opened */
    lv_obj_set_style_bg_image_src(quick_drawer, asset_path("pull_down/bg.png"), 0);
    lv_obj_set_style_bg_opa(quick_drawer, LV_OPA_COVER, 0);
    lv_obj_remove_flag(quick_drawer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(quick_drawer, LV_OBJ_FLAG_CLICKABLE); /* swallow touches to whatever's behind while open */

    /* Row 1: every toggle icon (Bluetooth / Wifi / sleep timer / output
     * gain) together in one row -- 4 icons x 84px + 5 gaps of 21px exactly
     * fills the panel's measured 440px content width (19 to 459). No clock,
     * no volume slider here anymore (clock duplicated the always-visible
     * main status bar; a second volume control duplicated the hardware
     * volume buttons' own popup) -- brightness (row 2 below) is the only
     * slider left in this drawer. */
    quick_drawer_wifi_icon = lv_image_create(quick_drawer);
    lv_image_set_src(quick_drawer_wifi_icon, asset_path("pull_down/wifi.png"));
    lv_obj_align(quick_drawer_wifi_icon, LV_ALIGN_TOP_LEFT, 40, QUICK_DRAWER_PANEL1_TOP + 30);
    lv_obj_add_flag(quick_drawer_wifi_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(quick_drawer_wifi_icon, quick_drawer_wifi_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(quick_drawer_wifi_icon, quick_drawer_wifi_long_press_cb, LV_EVENT_LONG_PRESSED, NULL);

    quick_drawer_bt_icon = lv_image_create(quick_drawer);
    lv_image_set_src(quick_drawer_bt_icon, asset_path("pull_down/bt.png"));
    lv_obj_align(quick_drawer_bt_icon, LV_ALIGN_TOP_LEFT, 145, QUICK_DRAWER_PANEL1_TOP + 30);
    lv_obj_add_flag(quick_drawer_bt_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(quick_drawer_bt_icon, quick_drawer_bt_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(quick_drawer_bt_icon, quick_drawer_bt_long_press_cb, LV_EVENT_LONG_PRESSED, NULL);

    quick_drawer_sleep_icon = lv_image_create(quick_drawer);
    lv_image_set_src(quick_drawer_sleep_icon, asset_path("pull_down/sleep_switch.png"));
    lv_obj_align(quick_drawer_sleep_icon, LV_ALIGN_TOP_LEFT, 250, QUICK_DRAWER_PANEL1_TOP + 30);
    lv_obj_add_flag(quick_drawer_sleep_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(quick_drawer_sleep_icon, quick_drawer_sleep_event_cb, LV_EVENT_CLICKED, NULL);

    /* Countdown while armed -- see quick_drawer_sleep_event_cb()/
     * poll_sleep_timer()'s own comments. Hidden until armed, centered under
     * the 84px-wide icon above it. */
    quick_drawer_sleep_label = lv_label_create(quick_drawer);
    lv_obj_add_style(quick_drawer_sleep_label, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(quick_drawer_sleep_label, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_set_width(quick_drawer_sleep_label, 84);
    lv_obj_set_style_text_align(quick_drawer_sleep_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(quick_drawer_sleep_label, LV_ALIGN_TOP_LEFT, 250, QUICK_DRAWER_PANEL1_TOP + 30 + 84 + 4);
    lv_obj_add_flag(quick_drawer_sleep_label, LV_OBJ_FLAG_HIDDEN);

    quick_drawer_crossfade_icon = lv_image_create(quick_drawer);
    lv_obj_align(quick_drawer_crossfade_icon, LV_ALIGN_TOP_LEFT, 355, QUICK_DRAWER_PANEL1_TOP + 30);
    lv_obj_add_flag(quick_drawer_crossfade_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(quick_drawer_crossfade_icon, quick_drawer_crossfade_event_cb, LV_EVENT_CLICKED, NULL);
    refresh_quick_drawer_crossfade_icon();

    /* Row 2: screen brightness -- real control, via the standard Linux
     * backlight sysfs class (backlight.h), no dedicated slider-track asset
     * in this theme so it reuses the (generic-looking) volume slider's own. */
    lv_obj_t * brightness_icon = lv_image_create(quick_drawer);
    lv_image_set_src(brightness_icon, asset_path("pull_down/blk.png"));
    lv_obj_align(brightness_icon, LV_ALIGN_TOP_LEFT, 40, QUICK_DRAWER_PANEL1_TOP + 174);

    quick_drawer_brightness_label = lv_label_create(quick_drawer);
    lv_obj_add_style(quick_drawer_brightness_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(quick_drawer_brightness_label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_obj_align(quick_drawer_brightness_label, LV_ALIGN_TOP_RIGHT, -20, QUICK_DRAWER_PANEL1_TOP + 177);

    /* Real-device bug report: the "NN%" label overlapped the slider --
     * first attempted by pushing the slider's own Y down to clear the
     * label's line height, but real-device feedback rejected that ("the
     * slider can't be lower than the icon, restore it to its default
     * position") -- brightness_icon (row 2's own visual anchor) sits at a
     * fixed Y, and the slider is meant to sit at a fixed, small offset
     * below it, not drift down with text size. The actual overlap was
     * horizontal, not vertical: the label is right-anchored and grows
     * LEFTWARD as its rendered text widens at bigger font tiers (BlindMF),
     * eventually reaching past the slider's fixed 300px-wide right edge.
     * Shrinking the slider's own WIDTH -- explicitly OK per that same
     * feedback ("it's ok to make the slider smaller") -- to always leave
     * room for the widest this label could ever render ("100%") fixes the
     * real, horizontal overlap while leaving both elements' Y positions
     * exactly as originally designed. */
    int32_t brightness_label_max_w = lv_text_get_width("100%", 4, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    int32_t brightness_track_w = (w - 20 - brightness_label_max_w - 20) - 90;
    if (brightness_track_w > 300) brightness_track_w = 300; /* never wider than the original design */
    if (brightness_track_w < 120) brightness_track_w = 120; /* sane floor so the track never collapses to nothing */

    quick_drawer_brightness_track = lv_slider_create(quick_drawer);
    lv_obj_set_size(quick_drawer_brightness_track, brightness_track_w, 12);
    lv_obj_align(quick_drawer_brightness_track, LV_ALIGN_TOP_LEFT, 90, QUICK_DRAWER_PANEL1_TOP + 185);
    /* Full 0-100 -- backlight.c now maps this logical range to its own safe
     * raw range internally (see backlight.h's own comment), so the slider
     * itself is free to show a clean, honest 0%-100% again. */
    lv_slider_set_range(quick_drawer_brightness_track, 0, 100);
    /* Initial value set below by refresh_quick_drawer_brightness() (also
     * called on every open_quick_drawer(), see its own comment) --
     * defined here just so it runs once at build time too, same as
     * every other quick-drawer widget's own initial state. */
    lv_obj_set_style_bg_color(quick_drawer_brightness_track, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_color(quick_drawer_brightness_track, lv_color_black(), LV_PART_KNOB);
    lv_obj_set_style_bg_image_src(quick_drawer_brightness_track, asset_path("volume/cursor.png"), LV_PART_KNOB);
    /* Real-device bug report: accent color didn't apply here -- see
     * apply_accent_color()'s own comment on why an image-art slider needs
     * bg_image_recolor, not just bg_color. */
    lv_obj_add_style(quick_drawer_brightness_track, gui_theme_accent_style(), LV_PART_INDICATOR);
    lv_obj_add_style(quick_drawer_brightness_track, gui_theme_accent_style(), LV_PART_KNOB);
    /* Real-device bug report: same left-edge gray sliver/root cause as
     * volume_popup_track's own fix -- see its comment. */
    configure_native_slider_rail(quick_drawer_brightness_track);
    lv_obj_set_style_bg_opa(quick_drawer_brightness_track, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_width(quick_drawer_brightness_track, 26, LV_PART_KNOB);
    lv_obj_set_style_height(quick_drawer_brightness_track, 26, LV_PART_KNOB);
    lv_obj_add_event_cb(quick_drawer_brightness_track, quick_drawer_brightness_changed_cb, LV_EVENT_ALL,
                         quick_drawer_brightness_label);
    refresh_quick_drawer_brightness();

    /* Mini now-playing card: real track title/artist/transport, reusing the
     * exact same callbacks as the player screen's own buttons. Transparent
     * -- it sits directly on the second bg.png panel (which already is a
     * rounded dark box) rather than drawing a second, slightly-differently
     * colored rounded rect on top of that one.
     *
     * Real-device feedback comparing against the stock drawer: this used
     * to be only 200px tall, well short of the second panel's own measured
     * 367px height (y=363-730, see build_quick_drawer()'s own panel-bounds
     * comment) -- leaving most of the panel empty and the transport row
     * sitting high up rather than low in the card like the stock
     * reference. 330 leaves a comparable margin above the panel's bottom
     * edge instead. */
    lv_obj_t * card = lv_obj_create(quick_drawer);
    lv_obj_set_size(card, 440, 330);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, QUICK_DRAWER_PANEL2_TOP + 12);
    lv_obj_set_style_bg_opa(card, 0, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    /* Title/artist centered (matching the stock drawer's own mini card,
     * confirmed via a real-device screenshot comparison -- this was
     * previously left-aligned with the favorite icon pinned separately in
     * the top-right corner, not part of the transport row at all). Both
     * need an explicit width for LV_TEXT_ALIGN_CENTER to have something to
     * center within. */
    quick_drawer_title_label = lv_label_create(card);
    lv_label_set_text(quick_drawer_title_label, "No track loaded");
    lv_obj_add_style(quick_drawer_title_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(quick_drawer_title_label, &app_font_16, 0); /* see song_title_label's own comment */
    lv_obj_set_width(quick_drawer_title_label, lv_pct(100));
    lv_obj_set_style_text_align(quick_drawer_title_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(quick_drawer_title_label, LV_ALIGN_TOP_MID, 0, 14);

    quick_drawer_artist_label = lv_label_create(card);
    lv_label_set_text(quick_drawer_artist_label, "");
    lv_obj_add_style(quick_drawer_artist_label, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(quick_drawer_artist_label, &app_font_16, 0);
    lv_obj_set_width(quick_drawer_artist_label, lv_pct(100));
    lv_obj_set_style_text_align(quick_drawer_artist_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(quick_drawer_artist_label, LV_ALIGN_TOP_MID, 0, 44);

    /* Transport row: order/prev/play/next/favorite, all five in one row --
     * matching the stock drawer exactly (shuffle-style icon leftmost,
     * favorite heart rightmost, same as the reference screenshot). This
     * copy of the order icon is a visual-only mirror of the main player
     * screen's own (see order_icon_event_cb) -- not independently
     * clickable, just kept in sync so the drawer doesn't show a stale mode. */
    lv_obj_t * controls_row = lv_obj_create(card);
    /* 84, not 70 -- btn_play.png/btn_pause.png are 84x84 (confirmed via the
     * actual asset files), and a shorter row was clipping the top/bottom of
     * that icon, confirmed on a real device. */
    lv_obj_set_size(controls_row, lv_pct(100), 84);
    lv_obj_align(controls_row, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_bg_opa(controls_row, 0, 0);
    lv_obj_set_style_border_width(controls_row, 0, 0);
    lv_obj_remove_flag(controls_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(controls_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(controls_row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    quick_drawer_order_icon = lv_image_create(controls_row);
    lv_image_set_src(quick_drawer_order_icon, asset_path(play_mode_icon_asset((play_mode_t) current_settings.play_mode)));

    lv_obj_t * prev_btn = lv_image_create(controls_row);
    lv_image_set_src(prev_btn, asset_path("playing_plane/btn_prev.png"));
    lv_obj_add_flag(prev_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(prev_btn, prev_btn_event_cb, LV_EVENT_CLICKED, NULL);

    quick_drawer_play_btn = lv_image_create(controls_row);
    lv_image_set_src(quick_drawer_play_btn, asset_path("playing_plane/btn_play.png"));
    lv_obj_add_flag(quick_drawer_play_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(quick_drawer_play_btn, play_btn_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t * next_btn = lv_image_create(controls_row);
    lv_image_set_src(next_btn, asset_path("playing_plane/btn_next.png"));
    lv_obj_add_flag(next_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(next_btn, next_btn_event_cb, LV_EVENT_CLICKED, NULL);

    quick_drawer_favorite_icon = lv_image_create(controls_row);
    lv_image_set_src(quick_drawer_favorite_icon, asset_path("playing_plane/collect_out.png"));
    lv_obj_add_flag(quick_drawer_favorite_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(quick_drawer_favorite_icon, favorite_icon_event_cb, LV_EVENT_CLICKED, NULL);

    /* Render the complex live control tree once while still under the boot
     * splash. Drag/snap motion uses this one opaque RGB565 image; controls
     * become live again the instant the motion settles. */
    quick_drawer_rebuild_snapshot();
}

void refresh_clock_label(void) {
    time_t now = time(NULL);
    struct tm tm_info;
    localtime_r(&now, &tm_info);
    char buf[8];
    /* %I (12h) zero-pads to 2 digits just like %H (24h) does -- "01".."12",
     * never a single digit -- so buf is always "HH:MM" (5 chars) either
     * way, mapping 1:1 onto the 5 fixed slots with no leading-slot-hiding
     * needed here, unlike the volume/battery readouts. */
    strftime(buf, sizeof(buf), current_settings.clock_24h ? "%H:%M" : "%I:%M", &tm_info);

    for (int i = 0; i < 5; i++) {
        char asset[24];
        if (buf[i] == ':') {
            snprintf(asset, sizeof(asset), "topbar/colon.png");
        } else {
            snprintf(asset, sizeof(asset), "topbar/%c.png", buf[i]);
        }
        lv_image_set_src(clock_topbar_digit[i], asset_path(asset));
    }

    if (current_settings.clock_24h) {
        lv_obj_add_flag(clock_topbar_ampm, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_image_set_src(clock_topbar_ampm, asset_path(tm_info.tm_hour < 12 ? "topbar/am.png" : "topbar/pm.png"));
        lv_obj_remove_flag(clock_topbar_ampm, LV_OBJ_FLAG_HIDDEN);
    }
}

void gui_shell_init(uint32_t screen_width, uint32_t screen_height) {
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
    start_bt_dac_startup_reapply_if_needed();
    refresh_play_pause_topbar();
#ifndef HOST_BUILD
    boot_checkpoint("start_refresh_bt_icon about to be called");
#endif
    start_refresh_bt_icon();
#ifndef HOST_BUILD
    boot_checkpoint("start_refresh_bt_icon done");
#endif
    refresh_headphone_icon();
    poll_usb_audio_output();

    if (!device_config_get_volume_warn_threshold(&volume_warn_threshold_percent)) {
        volume_warn_threshold_percent = -1;
    }
    refresh_volume_topbar((int32_t) (audio_get_volume() * 100.0f));

    quick_drawer_drag_timer = lv_timer_create(poll_quick_drawer_drag, LV_DEF_REFR_PERIOD, NULL);
}



void gui_shell_poll(void) {
    poll_usb_audio_output();
    poll_sleep_timer();
    poll_wifi_toggle();
    poll_bt_toggle();
    poll_bt_dac_startup_reapply();
    poll_bt_apply_output_settings();
    poll_refresh_bt_icon();
}

void gui_shell_resume_connections(bool wifi_was_on, bool bt_was_on) {
#ifndef HOST_BUILD
    if (wifi_was_on && !wifi_control_is_enabled() && !wifi_toggle_active) {
        wifi_toggle_active = true;
        atomic_store_explicit(&wifi_toggle_done_flag, false, memory_order_relaxed);
        wifi_toggle_target_enabled = true;
        if (pthread_create(&wifi_toggle_thread, NULL, wifi_toggle_thread_func, NULL) != 0)
            wifi_toggle_active = false;
    }
    if (bt_was_on && !bt_is_powered_cached && !bt_toggle_active) {
        bt_toggle_active = true;
        atomic_store_explicit(&bt_toggle_done_flag, false, memory_order_relaxed);
        if (pthread_create(&bt_toggle_thread, NULL, bt_toggle_thread_func, NULL) != 0) {
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
    *wifi_was_on = wifi_control_is_enabled();
    *bt_was_on = bt_is_powered_cached;
    if (*wifi_was_on && !wifi_toggle_active) {
        wifi_toggle_active = true;
        atomic_store_explicit(&wifi_toggle_done_flag, false, memory_order_relaxed);
        wifi_toggle_target_enabled = false;
        if (pthread_create(&wifi_toggle_thread, NULL, wifi_toggle_thread_func, NULL) != 0)
            wifi_toggle_active = false;
    }
    if (*bt_was_on && !bt_toggle_active) {
        bt_toggle_active = true;
        atomic_store_explicit(&bt_toggle_done_flag, false, memory_order_relaxed);
        if (pthread_create(&bt_toggle_thread, NULL, bt_toggle_thread_func, NULL) != 0)
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
    if (quick_drawer_drag_timer) lv_timer_resume(quick_drawer_drag_timer);
}

bool gui_shell_has_background_work(void) {
    return bt_toggle_active || bt_dac_startup_reapply_active || bt_apply_output_settings_active ||
           refresh_bt_icon_active || wifi_toggle_active;
}

void gui_shell_cancel_background_work(void) {
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


void gui_shell_update_quick_drawer_track(const char * title, const char * artist) {
    if (quick_drawer_title_label) {
        lv_label_set_text(quick_drawer_title_label, title ? title : "No track loaded");
        lv_label_set_text(quick_drawer_artist_label, artist ? artist : "");
        quick_drawer_mark_snapshot_dirty();
    }
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
        lv_image_set_src(quick_drawer_play_btn,
                         asset_path(is_playing ? "playing_plane/btn_pause.png" : "playing_plane/btn_play.png"));
        quick_drawer_mark_snapshot_dirty();
    }
}

void gui_shell_update_quick_drawer_play_mode(int mode) {
    if (quick_drawer_order_icon) {
        lv_image_set_src(quick_drawer_order_icon, asset_path(play_mode_icon_asset((play_mode_t) mode)));
        quick_drawer_mark_snapshot_dirty();
    }
}
