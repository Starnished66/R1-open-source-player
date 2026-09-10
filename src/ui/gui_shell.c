#include "gui_shell.h"
#include "app_clock.h"
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
#include "transition_compositor.h"
#include "metadata.h"
#include "db_log.h"
#include "audio.h"
#include "settings.h"
#include "assets.h"
#include "device_config.h"
#include "battery.h"
#include "charge_limiter.h"
#include "wifi_status.h"
#include "wifi_control.h"
#include "bluetooth_control.h"
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

static void start_bt_dac_startup_reapply_if_needed(void);
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
    return wifi_toggle_active ? wifi_toggle_target_enabled : wifi_control_is_enabled();
}

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
/* The group is initially built with three visible placeholder digits.
 * refresh_battery_topbar() only forces a flex reflow/re-anchor when the
 * real reading crosses a digit-count boundary, rather than adding layout
 * work to its ordinary 500 ms refresh path. */
static int battery_topbar_visible_digit_count = 3;

/* Fill sprite (topbar/battery.png) bbox within its own 20x30 native canvas,
 * measured directly off the asset (alpha bbox: x 4-15, y 9-22) and clipped
 * from the bottom as a charge-level gauge in refresh_battery_topbar(). Not
 * derived at runtime since nothing else in this codebase decodes PNG alpha
 * to find sprite bounds; a fixed asset gets a fixed constant. */
#define BATTERY_FILL_W 12
#define BATTERY_FILL_H 14
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

    /* Left edge of the bar: speaker icon, red volume number, headphone-out
     * icon, all pinned left. A flex row lets hidden digit slots (see
     * refresh_volume_topbar()) collapse cleanly instead of leaving a gap. */
    volume_topbar_group = lv_obj_create(band);
    lv_obj_remove_style_all(volume_topbar_group);
    lv_obj_set_size(volume_topbar_group, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(volume_topbar_group, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(volume_topbar_group, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    /* No extra column padding -- each digit sprite already has ~1px of
     * transparent margin baked into its canvas on both edges (e.g.
     * topbar/9.png is a 14px-wide canvas with the glyph spanning x=1..13),
     * providing sufficient separation. */
    lv_obj_set_style_pad_column(volume_topbar_group, 0, 0);
    lv_obj_remove_flag(volume_topbar_group, LV_OBJ_FLAG_SCROLLABLE);

    /* Rendered at native asset resolution (LV_SCALE_NONE), matching
     * battery_icon/wifi_icon/bt_status_icon. */
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

    /* Headphone-out glyph (topbar/po.png) -- starts hidden and is shown
     * by refresh_headphone_icon() when a headphone/dongle is plugged in
     * (see headphone_status.h). */
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
    /* Uses topbar/usb.png for the topbar status row (distinct from
     * usb/usb.png used by the full-screen USB DAC mode overlay). */
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

    /* Negotiated Bluetooth codec indicator (e.g. sbc.png, aac.png, aptx.png,
     * aptx_hd.png, ldac.png, uat.png) -- rightmost in volume_topbar_group,
     * lowest priority on the left side. Shows when an A2DP source PCM is
     * connected and fits without colliding with clock_topbar_group.
     * Hidden by default. */
    bt_codec_status_icon = lv_image_create(volume_topbar_group);
    lv_image_set_src(bt_codec_status_icon, bt_codec_get_asset(BT_CODEC_TYPE_SBC));
    lv_image_set_scale(bt_codec_status_icon, LV_SCALE_NONE);
    lv_obj_add_flag(bt_codec_status_icon, LV_OBJ_FLAG_HIDDEN);

    /* Deliberately LAST, after every child exists -- done earlier, the
     * LV_SIZE_CONTENT group still had zero content size at that point, and
     * its later growth as children were added did NOT retroactively re-run
     * this alignment (confirmed on real hardware in an earlier round of
     * this same bug: the group ended up anchored low and out of vertical
     * sync with the rest of the bar). */
    lv_obj_align(volume_topbar_group, LV_ALIGN_LEFT_MID, BOARD_SCALE_PX(16), 0);

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
    lv_obj_align(battery_icon_frame, LV_ALIGN_RIGHT_MID, -BOARD_SCALE_PX(15), 0);

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
    lv_obj_align_to(battery_topbar_group, battery_icon_frame, LV_ALIGN_OUT_LEFT_MID, -BOARD_SCALE_PX(5), 0);

    wifi_icon = lv_image_create(band);
    lv_image_set_src(wifi_icon, asset_path("topbar/wifi_unconnect.png"));
    lv_image_set_scale(wifi_icon, LV_SCALE_NONE);
    lv_obj_align(wifi_icon, LV_ALIGN_RIGHT_MID, -BOARD_SCALE_PX(105), 0);
    lv_obj_add_flag(wifi_icon, LV_OBJ_FLAG_HIDDEN); /* shown by refresh_wifi_icon() once wifi_control_is_enabled() */

    bt_status_icon = lv_image_create(band);
    lv_image_set_src(bt_status_icon, asset_path("topbar/bluetooth.png"));
    lv_image_set_scale(bt_status_icon, LV_SCALE_NONE);
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
        lv_image_set_src(play_pause_status_icon, asset_path("topbar/play.png"));
    } else if (paused) {
        lv_obj_remove_flag(play_pause_status_icon, LV_OBJ_FLAG_HIDDEN);
        lv_image_set_src(play_pause_status_icon, asset_path("topbar/pause.png"));
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
        lv_obj_add_flag(battery_icon_fill_clip, LV_OBJ_FLAG_HIDDEN);
        lv_image_set_src(battery_icon_frame, asset_path("topbar/battery_bg.png"));
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

/* Resolve each of the ten digit assets once; asset_path() allocates. */
static const char * topbar_digit_asset_path(char digit_char) {
    static const char * cache[10] = { 0 };
    int d = digit_char - '0';
    if (d < 0 || d > 9) d = 0; /* defensive -- percent is already clamped 0-100 above, digits are always '0'-'9' */
    if (!cache[d]) {
        char asset[24];
        snprintf(asset, sizeof(asset), "topbar/%d.png", d);
        cache[d] = asset_path(asset);
    }
    return cache[d];
}

/* Called at startup and whenever the displayed volume changes. Skip
 * identical strings so LVGL does not re-decode unchanged digit images. */
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
    bool digits_changed = strcmp(volume_topbar_last_digits, digits) != 0;

    for (int i = 0; i < 3; i++) {
        int digit_index = i - (3 - len);
        if (digit_index < 0) {
            lv_obj_add_flag(volume_topbar_digit[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            if (digits_changed)
                lv_image_set_src(volume_topbar_digit[i], topbar_digit_asset_path(digits[digit_index]));
            lv_obj_remove_flag(volume_topbar_digit[i], LV_OBJ_FLAG_HIDDEN);
        }
        /* LV_OPA_TRANSP disables the recolor mix entirely, leaving the
         * sprite's own native white showing through -- simpler than
         * swapping between a white-recolor and a red-recolor style. */
        lv_obj_set_style_image_recolor(volume_topbar_digit[i], lv_color_make(255, 0, 0), 0);
        lv_obj_set_style_image_recolor_opa(volume_topbar_digit[i], warn ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
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
        lv_obj_align_to(widget, anchor, LV_ALIGN_OUT_LEFT_MID, -BOARD_SCALE_PX(8), 0);
        anchor = widget;
    }
}

/* The drawer's wifi icon reflects radio on/off (highlighted when enabled),
 * while the top bar icon indicates connection status and signal strength. */
static void refresh_wifi_icon(void) {
    /* Keep an in-flight enable visually enabled even before wlan0's
     * wpa_supplicant socket exists.  Association is still queried below,
     * so the topbar naturally advances from the existing disconnected
     * icon to signal strength without exposing an "enabling" state.  Once
     * poll_wifi_toggle() clears wifi_toggle_active, this immediately goes
     * back to the authoritative backend state and can still report a real
     * failure normally. */
    bool enabled = gui_shell_wifi_effective_enabled();
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

/* UI-thread owned Bluetooth audio state and disconnect generation latch */
static bool bt_is_a2dp_connected_ui = false;
static uint32_t bt_disconnect_epoch = 0;
static uint32_t bt_worker_launch_epoch = 0;

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
    bt_connected_codec_cached[0] = '\0';
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
    if (!refresh_bt_startup_readiness()) return;
    if (refresh_bt_icon_active) return; /* previous check still in flight -- same "ignore taps until it lands" pattern as everything else here */
    refresh_bt_icon_active = true;
    bt_worker_launch_epoch = bt_disconnect_epoch;
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
     * (either visible or already known to be hidden due to clock overlap), do nothing */
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

    /* Clock-overlap protection:
     * Determine whether displaying bt_codec_status_icon would make volume_topbar_group
     * reach into clock_topbar_group. Reserve a small visual margin (6px).
     * If it would overlap, hide only bt_codec_status_icon to preserve all higher-priority
     * indicators (volume, headphone, A2DP, USB, play/pause). */
    hidden_due_to_overlap = false;
    if (volume_topbar_group && clock_topbar_group) {
        lv_obj_update_layout(volume_topbar_group);
        lv_obj_update_layout(clock_topbar_group);
        int32_t left_right = lv_obj_get_x(volume_topbar_group) + lv_obj_get_width(volume_topbar_group);
        int32_t clock_left = lv_obj_get_x(clock_topbar_group);
        if (clock_left <= 0) clock_left = 200; /* safe fallback if layout not yet evaluated */
        if (left_right + 6 > clock_left) {
            lv_obj_add_flag(bt_codec_status_icon, LV_OBJ_FLAG_HIDDEN);
            lv_obj_update_layout(volume_topbar_group);
            hidden_due_to_overlap = true;
        }
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

    snprintf(bt_connected_mac_cached, sizeof(bt_connected_mac_cached), "%s", a2dp_connected ? refresh_bt_icon_result_mac : "");
    snprintf(bt_connected_codec_cached, sizeof(bt_connected_codec_cached), "%s", a2dp_connected ? refresh_bt_icon_result_codec : "");
    if (quick_drawer_bt_icon) {
        lv_image_set_src(quick_drawer_bt_icon, asset_path(display_powered ? "pull_down/bt_s.png" : "pull_down/bt.png"));
        quick_drawer_mark_snapshot_dirty();
    }

    if (!display_powered) {
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
    sync_topbar_status_icon_positions();
    lv_image_set_src(bt_status_icon, asset_path(refresh_bt_icon_result_connected ? "topbar/bluetooth.png" : "topbar/bluetooth_unconnect.png"));
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
    bool use_bt_output = refresh_bt_icon_result_connected && !current_settings.bt_dac_mode_enabled;
    audio_set_bt_output(use_bt_output);

    /* Volume synchronization with Bluetooth audio output devices. */
    if (use_bt_output && current_settings.bt_volume_sync_enabled) {
        bt_control_source_volume_sync_start();
    } else {
        bt_control_source_volume_sync_stop();
    }

    /* Output disconnect watcher for faster disconnection detection. */
    if (use_bt_output) {
        bt_control_output_disconnect_watch_start();
    } else {
        bt_control_output_disconnect_watch_stop();
    }

    /* Mirror Bluetooth output setting to USB DAC bridge when active. */
    usb_dac_bridge_set_bt_output(use_bt_output);
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
                     asset_path(current_settings.crossfade_enabled ? "pull_down/fade_s.png" : "pull_down/fade.png"));
    quick_drawer_mark_snapshot_dirty();
}

/* Settings > Music Settings > Playback's Crossfade toggle row is kept in sync
 * with quick drawer toggles via gui_settings_sync_crossfade_toggle(). */

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
    if (!quick_drawer_open)
        transition_compositor_discard_vertical_base();
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
        lv_obj_get_coords(obj, &area);
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
        drag_adjust_press_owned = active_object_is_drag_adjust_widget() ||
                                  point_in_swipe_dead_zone(p) ||
                                  quick_drawer_brightness_hit_test(p) ||
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
        /* Settle decision on release: any registered direction at the exact
         * moment of release wins, however slow; only a perfectly still
         * release (zero exit velocity) falls back to whichever side of the
         * halfway mark the panel was on. */
        quick_drawer_drag_tracking = false;
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

void quick_drawer_wifi_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (quick_drawer_wifi_long_press_fired) { /* see quick_drawer_wifi_long_press_cb()'s own comment */
        quick_drawer_wifi_long_press_fired = false;
        return;
    }
    if (wifi_toggle_active) return; /* already toggling -- ignore taps until it lands */
    bool wifi_will_be_enabled = !wifi_control_is_enabled();
    wifi_toggle_active = true;
    wifi_toggle_is_radio_suspend = false; /* a real user tap, not the idle radio-suspend cycle */
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

    /* Optimistically update the topbar Wi-Fi icon alongside the drawer icon.
     * Overwritten with authoritative state once the worker thread settles. */
    if (wifi_will_be_enabled) {
        lv_obj_remove_flag(wifi_icon, LV_OBJ_FLAG_HIDDEN);
        lv_image_set_src(wifi_icon, asset_path("topbar/wifi_unconnect.png"));
    } else {
        lv_obj_add_flag(wifi_icon, LV_OBJ_FLAG_HIDDEN);
    }
    sync_topbar_status_icon_positions();

    /* Optimistically rebuild Wi-Fi settings screen if visible so dependent rows
     * appear immediately rather than waiting for the backend script to complete. */
    /* Do not clean/rebuild wifi_list from inside the clicked row's own
     * event callback: doing so deletes the event target while LVGL is still
     * dispatching through it. gui_network_show_wifi_toggle_pending() defers
     * the optimistic rebuild by one UI turn; poll_wifi_toggle() performs the
     * authoritative rebuild once the worker settles. */
    if (gui_navigation_is_top(gui_network_get_wifi_screen()))
        gui_network_show_wifi_toggle_pending(wifi_will_be_enabled);

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
    if (!enabled && !was_radio_suspend) gui_network_handle_wifi_disabled();
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
    lv_image_set_src(quick_drawer_bt_icon, asset_path(powered ? "pull_down/bt_s.png" : "pull_down/bt.png"));
    quick_drawer_mark_snapshot_dirty();

    bt_disconnect_epoch++;
    bt_is_a2dp_connected_ui = false;
    bt_connected_codec_cached[0] = '\0';
    if (powered) {
        lv_obj_remove_flag(bt_status_icon, LV_OBJ_FLAG_HIDDEN);
        lv_image_set_src(bt_status_icon, asset_path("topbar/bluetooth_unconnect.png"));
    } else {
        lv_obj_add_flag(bt_status_icon, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_add_flag(a2dp_status_icon, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(bt_codec_status_icon, LV_OBJ_FLAG_HIDDEN);
    invalidate_bt_codec_status_cache();
    sync_topbar_status_icon_positions();

    bt_is_powered_cached = powered;
    if (gui_navigation_is_top(gui_network_get_bt_screen())) populate_bt_screen();
}

void quick_drawer_bt_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (quick_drawer_bt_long_press_fired) { /* see quick_drawer_bt_long_press_cb()'s own comment */
        quick_drawer_bt_long_press_fired = false;
        return;
    }
    bool bt_will_be_powered = !bt_is_powered_cached;

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

/* Reapplies persisted bt_dac_mode_enabled configuration asynchronously
 * at startup, launching the necessary bluealsa and bt-agent processes without
 * blocking the UI thread. */
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

/* Called once from gui_init(), only if bt_dac_mode_enabled was already true
 * at load time (a fresh toggle-on tap already goes through
 * bt_dac_toggle_cb() directly and doesn't need this). */
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
#define QUICK_DRAWER_PANEL1_TOP BOARD_SCALE_PX(59)
#define QUICK_DRAWER_PANEL1_BOTTOM BOARD_SCALE_PX(338)
#define QUICK_DRAWER_PANEL2_TOP BOARD_SCALE_PX(363)
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
            lv_label_set_text_fmt(quick_drawer_brightness_label, "%d%%", (int) percent);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        brightness_drag_active = false;
        brightness_hw_pending = (int) percent;
        brightness_hw_apply_pending();
        if (brightness_hw_apply_timer) lv_timer_pause(brightness_hw_apply_timer);
        if (quick_drawer_brightness_label)
            lv_label_set_text_fmt(quick_drawer_brightness_label, "%d%%", (int) percent);
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
    const void * drawer_bg = asset_decoded_image_open(&quick_drawer_bg_image, "pull_down/bg.png")
                           ? asset_decoded_image_source(&quick_drawer_bg_image) : NULL;
    lv_obj_set_style_bg_image_src(quick_drawer, drawer_bg ? drawer_bg : asset_path("pull_down/bg.png"), 0);
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
    lv_obj_align(quick_drawer_wifi_icon, LV_ALIGN_TOP_LEFT, BOARD_SCALE_PX(40), QUICK_DRAWER_PANEL1_TOP + BOARD_SCALE_PX(30));
    lv_obj_add_flag(quick_drawer_wifi_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(quick_drawer_wifi_icon, quick_drawer_wifi_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(quick_drawer_wifi_icon, quick_drawer_wifi_long_press_cb, LV_EVENT_LONG_PRESSED, NULL);

    quick_drawer_bt_icon = lv_image_create(quick_drawer);
    lv_image_set_src(quick_drawer_bt_icon, asset_path("pull_down/bt.png"));
    lv_obj_align(quick_drawer_bt_icon, LV_ALIGN_TOP_LEFT, BOARD_SCALE_PX(145), QUICK_DRAWER_PANEL1_TOP + BOARD_SCALE_PX(30));
    lv_obj_add_flag(quick_drawer_bt_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(quick_drawer_bt_icon, quick_drawer_bt_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(quick_drawer_bt_icon, quick_drawer_bt_long_press_cb, LV_EVENT_LONG_PRESSED, NULL);

    quick_drawer_sleep_icon = lv_image_create(quick_drawer);
    lv_image_set_src(quick_drawer_sleep_icon, asset_path("pull_down/sleep_switch.png"));
    lv_obj_align(quick_drawer_sleep_icon, LV_ALIGN_TOP_LEFT, BOARD_SCALE_PX(250), QUICK_DRAWER_PANEL1_TOP + BOARD_SCALE_PX(30));
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
    lv_obj_align(quick_drawer_sleep_label, LV_ALIGN_TOP_LEFT, BOARD_SCALE_PX(250), QUICK_DRAWER_PANEL1_TOP + BOARD_SCALE_PX(30) + BOARD_SCALE_PX(84) + BOARD_SCALE_PX(4));
    lv_obj_add_flag(quick_drawer_sleep_label, LV_OBJ_FLAG_HIDDEN);

    quick_drawer_crossfade_icon = lv_image_create(quick_drawer);
    lv_obj_align(quick_drawer_crossfade_icon, LV_ALIGN_TOP_LEFT, BOARD_SCALE_PX(355), QUICK_DRAWER_PANEL1_TOP + BOARD_SCALE_PX(30));
    lv_obj_add_flag(quick_drawer_crossfade_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(quick_drawer_crossfade_icon, quick_drawer_crossfade_event_cb, LV_EVENT_CLICKED, NULL);
    refresh_quick_drawer_crossfade_icon();

    /* Row 2: screen brightness -- real control, via the standard Linux
     * backlight sysfs class (backlight.h), no dedicated slider-track asset
     * in this theme so it reuses the (generic-looking) volume slider's own. */
    quick_drawer_brightness_icon = lv_image_create(quick_drawer);
    const void * brightness = asset_decoded_image_open(&quick_drawer_brightness_image, "pull_down/blk.png")
                            ? asset_decoded_image_source(&quick_drawer_brightness_image) : NULL;
    lv_image_set_src(quick_drawer_brightness_icon, brightness ? brightness : asset_path("pull_down/blk.png"));
    lv_obj_align(quick_drawer_brightness_icon, LV_ALIGN_TOP_LEFT, BOARD_SCALE_PX(40), QUICK_DRAWER_PANEL1_TOP + BOARD_SCALE_PX(174));

    quick_drawer_brightness_label = lv_label_create(quick_drawer);
    lv_obj_add_style(quick_drawer_brightness_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(quick_drawer_brightness_label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_obj_align(quick_drawer_brightness_label, LV_ALIGN_TOP_RIGHT, -BOARD_SCALE_PX(20), QUICK_DRAWER_PANEL1_TOP + BOARD_SCALE_PX(177));

    /* Dynamically sizes slider width based on the maximum width of the percentage
     * label ("100%") to prevent horizontal overlap when using larger font tiers. */
    int32_t brightness_label_max_w = lv_text_get_width("100%", 4, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    int32_t brightness_track_w = (w - BOARD_SCALE_PX(20) - brightness_label_max_w - BOARD_SCALE_PX(20)) - BOARD_SCALE_PX(90);
    if (brightness_track_w > BOARD_SCALE_PX(300)) brightness_track_w = BOARD_SCALE_PX(300); /* never wider than the original design */
    if (brightness_track_w < BOARD_SCALE_PX(120)) brightness_track_w = BOARD_SCALE_PX(120); /* sane floor so the track never collapses to nothing */

    quick_drawer_brightness_track = lv_slider_create(quick_drawer);
    lv_obj_set_size(quick_drawer_brightness_track, brightness_track_w, SLIDER_TRACK_HEIGHT);
    lv_obj_align(quick_drawer_brightness_track, LV_ALIGN_TOP_LEFT, BOARD_SCALE_PX(90), QUICK_DRAWER_PANEL1_TOP + BOARD_SCALE_PX(185));
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

    /* Mini now-playing card: track title, artist, and transport controls.
     * Sized to fit the second panel's bounds with a balanced bottom margin. */
    lv_obj_t * card = lv_obj_create(quick_drawer);
    lv_obj_set_size(card, BOARD_SCALE_PX(440), BOARD_SCALE_PX(330));
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, QUICK_DRAWER_PANEL2_TOP + BOARD_SCALE_PX(12));
    lv_obj_set_style_bg_opa(card, 0, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    /* Title and artist labels centered within explicit widths. */
    quick_drawer_title_label = lv_label_create(card);
    lv_label_set_text(quick_drawer_title_label, "No track loaded");
    lv_obj_add_style(quick_drawer_title_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(quick_drawer_title_label, gui_theme_font(GUI_FONT_ROLE_ROW), 0);
    lv_obj_set_width(quick_drawer_title_label, BOARD_SCALE_PX(392));
    lv_obj_set_style_text_align(quick_drawer_title_label, LV_TEXT_ALIGN_CENTER, 0);
    row_label_apply_bounded_height(quick_drawer_title_label, gui_theme_font(GUI_FONT_ROLE_ROW));
    row_label_enable_marquee(quick_drawer_title_label);
    lv_obj_align(quick_drawer_title_label, LV_ALIGN_TOP_MID, 0, BOARD_SCALE_PX(22));

    quick_drawer_artist_label = lv_label_create(card);
    lv_label_set_text(quick_drawer_artist_label, "");
    lv_obj_add_style(quick_drawer_artist_label, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(quick_drawer_artist_label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_obj_set_width(quick_drawer_artist_label, BOARD_SCALE_PX(392));
    lv_obj_set_style_text_align(quick_drawer_artist_label, LV_TEXT_ALIGN_CENTER, 0);
    row_label_apply_bounded_height(quick_drawer_artist_label, gui_theme_font(GUI_FONT_ROLE_BODY));
    row_label_enable_marquee(quick_drawer_artist_label);
    lv_obj_align(quick_drawer_artist_label, LV_ALIGN_TOP_MID, 0, BOARD_SCALE_PX(76));

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
    lv_obj_align(controls_row, LV_ALIGN_BOTTOM_MID, 0, -BOARD_SCALE_PX(8));
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
    lv_image_set_src(quick_drawer_play_btn, gui_player_play_btn_image_src(audio_is_playing()));
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
    struct tm tm_info;
    app_clock_localtime(&tm_info);
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
 * three root containers need an explicit lv_obj_del(): status_bar_band/
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
        lv_obj_del(quick_drawer_motion_image);
        quick_drawer_motion_image = NULL;
    }
    if (quick_drawer_motion_buf) {
        lv_draw_buf_destroy(quick_drawer_motion_buf);
        quick_drawer_motion_buf = NULL;
    }
    quick_drawer_bitmap_motion = false;
    if (quick_drawer) {
        lv_obj_del(quick_drawer);
        quick_drawer = NULL;
    }
    quick_drawer_brightness_icon = NULL;
    asset_decoded_image_close(&quick_drawer_bg_image);
    asset_decoded_image_close(&quick_drawer_brightness_image);
    if (status_bar_band) {
        lv_obj_del(status_bar_band);
        status_bar_band = NULL;
    }
    if (home_indicator_band) {
        lv_obj_del(home_indicator_band);
        home_indicator_band = NULL;
    }
    if (dac_home_screen) {
        lv_obj_del(dac_home_screen);
        dac_home_screen = NULL;
    }
    if (home_screen) {
        lv_obj_del(home_screen);
        home_screen = NULL;
    }
}

void gui_shell_refresh_static_assets(void) {
    if (!quick_drawer) return;
    asset_decoded_image_close(&quick_drawer_bg_image);
    asset_decoded_image_close(&quick_drawer_brightness_image);
    const void * bg = asset_decoded_image_open(&quick_drawer_bg_image, "pull_down/bg.png")
                    ? asset_decoded_image_source(&quick_drawer_bg_image) : NULL;
    const void * brightness = asset_decoded_image_open(&quick_drawer_brightness_image, "pull_down/blk.png")
                            ? asset_decoded_image_source(&quick_drawer_brightness_image) : NULL;
    lv_obj_set_style_bg_image_src(quick_drawer, bg ? bg : asset_path("pull_down/bg.png"), 0);
    if (quick_drawer_brightness_icon)
        lv_image_set_src(quick_drawer_brightness_icon,
                         brightness ? brightness : asset_path("pull_down/blk.png"));
    quick_drawer_mark_snapshot_dirty();
}

void gui_shell_refresh_home(void) {
    lv_obj_t * old = home_screen;
    lv_obj_t * fresh = build_home_screen();
    if (!fresh) return;
    home_screen = fresh;
    gui_navigation_replace_home(old, fresh);
    if (old) lv_obj_del(old);
}

void gui_shell_init(uint32_t screen_width, uint32_t screen_height) {
    gui_shell_build_screens(screen_width, screen_height);
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
        wifi_toggle_is_radio_suspend = false; /* restoring, not the suspend disable itself */
        atomic_store_explicit(&wifi_toggle_done_flag, false, memory_order_relaxed);
        wifi_toggle_target_enabled = true;
        if (pthread_create(&wifi_toggle_thread, NULL, wifi_toggle_thread_func, NULL) != 0)
            wifi_toggle_active = false;
    }
    if (bt_was_on && !bt_is_powered_cached && !bt_toggle_active) {
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
     * also releases a persisted Bluetooth-DAC reapply behind the same gate. */
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
