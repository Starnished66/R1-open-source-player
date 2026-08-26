#include "gui.h"
#include "gui_library.h"
#include "gui_queue.h"
#include "gui_player.h"
#include "gui_plugins.h"
#include "gui_shell.h"
#include "gui_navigation.h"

#define PLAYLISTS_DIR MUSIC_ROOT_DIR "/Playlists"
#define SUBSONIC_STREAM_CACHE_DIR "/tmp/subsonic_stream_cache"
#include "gui_theme.h"
#include "gui_notifications.h"
#include "gui_settings.h"
#include "gui_network.h"
#include "gui_books.h"
#include "gui_text_input.h"
#include "gui_lyrics.h"
#include "gui_track_info.h"
#include "gui_subsonic.h"
#include "assets.h"
#include "backlight.h"
#include "debug_log.h"
#include "import_web.h"
#include "airplay_control.h"
#include "dlna_control.h"
#include "remote_control.h"
#include "battery.h"
#include "wifi_status.h"
#include "audio.h"
#include "file_browser.h"
#include "text_reader.h"
#include "hw_buttons.h"
#include "metadata.h"
#include "metadata_db.h"
#include "peq.h"
#include "screen_builders.h"
#include "settings.h"
#include "subsonic_client.h"
#include "http_client.h"
#include "cover_decode.h"
#include "albumart.h"
#include "lyrics.h"
#include "lyrics_layout.h"
#include "remote_track.h"
#include "transition_compositor.h"
#include "wifi_control.h"
#include "bluetooth_control.h"
#include "hiby_sys_server.h"
#ifndef HOST_BUILD
#include "bt_media_player.h"
#endif
#include "headphone_status.h"
#include "usb_audio_output.h"
#include "plugin_manager.h"
#include "led_control.h"
#include "charge_limiter.h"
#include "idle_shutdown.h"
#include "power_suspend.h"
#include "device_config.h"
#include "usb_mode_control.h"
#include "usb_dac_bridge.h"
#include "firmware_update.h"
#include "playlist_files.h"
#include "cue_parser.h"
#include "subprocess.h"
#include "timezone_data.h"
#include "timezone_apply.h"
#include "hostname_apply.h"

/* --- Theme API (gui_theme.h) --- */



/* --- Notification/Modal API (gui_notifications.h) --- */


gui_busy_handle_t gui_busy_show(const char * title, const char * msg);
void gui_busy_set_progress(gui_busy_handle_t handle, int percent);
void gui_busy_hide(gui_busy_handle_t handle);

gui_busy_handle_t wifi_connect_token = 0;
gui_busy_handle_t wifi_connect_saved_token = 0;
gui_busy_handle_t import_web_stop_token = 0;

#include "src/core/lv_obj.h"
#include "src/core/lv_obj_pos.h"
#include "src/core/lv_obj_style.h"
#include "src/core/lv_obj_style_gen.h"
#include "src/layouts/flex/lv_flex.h"
#include "src/misc/lv_area.h"
#include "src/draw/lv_draw_buf.h"
#include "src/others/snapshot/lv_snapshot.h"
#include "src/core/lv_refr.h"
#include "src/widgets/image/lv_image.h"
#include "src/drivers/display/fb/lv_linux_fbdev.h"
#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifdef UI_PERF_TRACE
uint64_t ui_perf_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000ULL + (uint64_t) ts.tv_nsec / 1000ULL;
}
#endif

#ifdef HOST_BUILD
  #define MUSIC_ROOT_DIR "./music"
#else
  /* SD card mount point, per the layout the stock hiby_player uses. */
  #define MUSIC_ROOT_DIR "/data/mnt/sd_0"

  /* main.c's own boot-checkpoint logger (see its own comment there) --
   * gui_init() below calls it directly since the still-unresolved cold-boot
   * hang has moved further into startup than main.c alone can see. Remove
   * alongside main.c's own copy once cold boot is confirmed working. */
  extern void boot_checkpoint(const char * step);

  /* main.c's own best-effort `mount -t vfat .../sd_0` retry (see its own
   * comment on why this needs to exist at all -- no hotplug mechanism for
   * the internal SD card slot exists anywhere on this firmware). Called
   * once at boot from main.c already; poll_sd_card_hotplug() below calls
   * it again periodically so re-inserting the card after boot eventually
   * gets mounted too, not just the very first insertion main.c catches. */
  extern void mount_sd_card_if_needed(void);
#endif

/* Books are deliberately isolated from the music-library root. A recursive
 * .txt search across an entire large SD card is both unexpectedly broad and
 * slow; only this dedicated directory participates in book discovery. */
#define BOOKS_ROOT_DIR MUSIC_ROOT_DIR "/Books"
#define AUDIOBOOKS_LIBRARY_DIR_NAME "Audiobooks"

/* Music browsing benefits from roomier touch targets and artwork, while
 * Settings and the rest of the app retain the denser shared 84px rows. */

/* ---- UI text size (Settings -> Font Size) ----
 *
 * Real-device feedback: the app's fixed text size (unchanged since the UI
 * rebuild) reads fine on some eyes, too small on others. Every label in
 * this codebase sets its own font via a plain per-object LVGL style
 * (lv_obj_set_style_text_font()) rather than a shared lv_style_t object
 * LVGL could bulk-refresh in place (the same trick app-wide accent color
 * uses, see lv_obj_report_style_change()'s own comment elsewhere in this
 * file) -- so these four pointers stand in for the four literal
 * lv_font_montserrat_NN sizes (16/20/22/28) this app used at every call
 * site that does NOT need fallback_font.h's non-Latin fallback chaining
 * (fixed English UI chrome -- button captions, section headers, settings
 * rows -- that will never show metadata-derived text). Deliberately named
 * ui_size_* rather than app_font_* -- fallback_font.h already declares
 * real (non-const) app_font_16/22/28 instances for the OTHER category
 * (metadata-derived text: song title/artist, drill-down screen titles,
 * ...), and those are handled separately, by resizing fallback_font.c's
 * own copies in place (see its own comment) rather than through these
 * pointers -- reusing that name here would silently collide.
 *
 * Set once by apply_font_size_tier(), called at the very top of
 * gui_init() before any screen is built. Changing the setting afterward
 * only takes effect on the next launch -- see settings.h's own comment on
 * font_size_tier for why a live re-style isn't practical here.
 *
 * Metadata-derived text (song title/artist, ...) is NOT covered by this
 * function -- that goes through fallback_font.h's app_font_16/22/28
 * instead, resized by its own fallback_font_init_early(tier) call
 * (gui_init() passes current_settings.font_size_tier there directly), so
 * both systems move together without this function reaching into that
 * one.
 *
 * Not static -- screen_builders.c's own handful of fixed-chrome literal
 * font sites (build_title(), pill-list row labels/chevron) need these
 * too, same cross-translation-unit pattern fallback_font.h's app_font_16/
 * 22/28 already use. Declared extern in screen_builders.h rather than a
 * new header of their own, since that's the one file already shared by
 * every screen-building call site on both sides. */
/* ui_size_* and apply_font_size_tier moved to gui_theme.c */

lv_obj_t * stream_media_screen;

void sync_player_topbar_visibility(lv_obj_t * screen);







/* The 480x320 panel behind the transport controls (title/artist/progress/
 * time/controls_row) -- built in build_player_screen(), but also targeted
 * by poll_cover_decode()/compute_reflection_bytes() below to swap its background
 * between the plain static buttom.png (no embedded art to reflect) and a
 * freshly generated per-track reflection, hence file-scope rather than a
 * local inside build_player_screen(). */
bool favorite_is_set = false;
/* Clock, top bar center: real topbar/N.png digit + topbar/colon.png
 * sprites, same asset family as the volume readout below, instead of an
 * lv_label -- switched from font text so it's pixel-identical in size/style
 * to the volume/headphone indicator rather than an approximate font-size
 * match (real-device feedback: "match the size of ... the volume and
 * headphone indicator"). Fixed HH:MM layout, always all 5 slots visible
 * (no leading-zero hiding the way the volume/battery readouts need, since
 * a clock always shows both digits of the hour). */
/* Settings -> System -> "24-Hour Clock" (current_settings.clock_24h). Extra
 * flex-row child after the 5 digit slots, hidden entirely in 24h mode --
 * topbar/am.png and topbar/pm.png are pre-existing theme assets, unused by
 * any code before this setting. clock_topbar_group is LV_SIZE_CONTENT and
 * center-aligned to the status bar band, so hiding/showing this slot
 * reflows and re-centers the whole clock automatically, same as any other
 * flex child visibility change in this file. */
/* Volume readout, far left of the status bar: real topbar/N.png digit
 * sprites (theme2 asset set) plus topbar/speaker.png and topbar/po.png
 * (headphone-out glyph), not a text label -- matches the stock player's own
 * top bar exactly (confirmed via a real-device screenshot of the stock
 * `hiby_player` binary: speaker icon, a red-recolored volume number, then a
 * headphone icon, all pinned to the left edge, with the clock centered
 * separately -- our previous layout had guessed a plain lv_label clock at
 * the left edge with the volume group trailing after it, which doesn't
 * match). Up to 3 digit slots for 0-100; unused leading slots are hidden
 * rather than left blank, so the flex row collapses the gap instead of
 * showing empty space before the first significant digit. volume_topbar_headphone
 * is shown/hidden by refresh_headphone_icon() based on real jack-detect
 * state (see headphone_status.h), not always-on. */

/* Loud-volume warning color threshold, read once at startup from the stock
 * firmware's own /usr/resource/config.json (see device_config.h) -- e.g. a
 * real R1 Pro had this set to 40 via the stock Settings screen. -1 means
 * "feature disabled" (file missing/host build, or vol_warn_enable=0 in the
 * config), in which case the volume digits always stay their native white
 * and never recolor red, regardless of level. */

/* Quick-access drawer mirrors of the persistent status bar / player-screen
 * widgets above -- kept in sync from the same single update sites (see
 * refresh_clock_label/refresh_battery_topbar/refresh_wifi_icon/
 * set_play_button_state/favorite_icon_event_cb/apply_track_metadata_to_ui)
 * rather than introducing a second, separately-polled source of truth.
 * NULL until build_quick_drawer() runs; every update site guards on that. */

/* Real-device bug report: the drawer's brightness slider/label only ever
 * reflected whatever backlight_get_percent() read back at build_quick_
 * drawer() time (app startup) -- turning the screen off and back on
 * restores the real prior brightness (see backlight_set_screen_on()'s own
 * comment) without this ever re-reading it, so the slider silently drifted
 * out of sync with the real screen brightness on every screen-off/on cycle,
 * not just the one the reset bug above also affected. Called once at build
 * time (build_quick_drawer() itself) and again every time the drawer opens
 * (open_quick_drawer()), so it's never stale by the time the user can
 * actually see it. */


/* Defined with the rest of the Subsonic streaming logic further down;
 * forward-declared here since polling for a finished background download
 * belongs alongside every other per-tick "did a background thing finish"
 * check in update_timer_cb (hw_buttons, audio_consume_track_advanced, ...). */
static void poll_dlna_control(void);
/* Defined alongside library_scan_once()'s own background-thread wrapper,
 * much further down -- forward-declared here since subsonic_library_
 * download_thread_func() (defined well before that point) needs to trigger
 * a rescan once its own batch download finishes. */
void start_library_rescan(void);
void poll_wifi_scan(void);
void poll_wifi_connect(void);
void poll_wifi_connect_saved(void);
void poll_wifi_disconnect(void);
void poll_wifi_forget(void);
void poll_bt_scan(void);
void poll_bt_connect(void);
void poll_bt_forget(void);

/* Full radio suspend after a long stretch with the screen off and nothing
 * going on -- deliberately separate from (and much longer than) the
 * screen-timeout backlight-off state itself: that can fire as often as
 * every 30s from ordinary momentary inactivity during otherwise-continuous
 * use, and WiFi/Bluetooth reassociation has a real cost, so tying radio
 * suspend to the same short timer would punish a user who's still actively
 * using the device, just with the screen dimming between taps. Skipped
 * entirely while music is playing, either DAC receive mode is on (both need
 * their radio to stay up to receive), or the device is charging (no battery
 * pressure to justify the reassociation cost). Whichever radios were
 * actually on get suspended, and only those get resumed on wake -- a radio
 * the user already had off before sleeping stays off. */
#define RADIO_SUSPEND_DELAY_MS (10 * 60 * 1000)
static uint32_t screen_off_since_tick = 0;
static bool inactivity_dimmed = false;

/* Earliest possible inactivity age for the interactive UI. LVGL's display
 * inactivity timestamp can predate gui_init()'s splash -> Home transition
 * (and, on the target, lv_display_trigger_activity() alone has now been
 * observed not to reliably discard that startup age). Clamp the value used
 * by dimming/timeout to elapsed interactive time so startup can never spend
 * the user's timeout budget. After this age grows past LVGL's normally-reset
 * inactivity value the clamp becomes a no-op, preserving ordinary touch and
 * hardware-button timeout behavior. */
static uint32_t interactive_ui_start_tick = 0;
static bool interactive_ui_started = false;

void gui_reset_interactive_timeout_baseline(void) {
    interactive_ui_start_tick = lv_tick_get();
    interactive_ui_started = true;
    lv_display_trigger_activity(NULL);
}

/* The panel dominates power while lit. Dim well before the configured full
 * timeout, but keep enough time for reading and never override an explicit
 * user's screen-off duration. */
#define SCREEN_DIM_AFTER_MS 10000U
#define VISIBLE_STATUS_POLL_TICKS 4 /* 2 seconds at the 500 ms control tick */
static bool radios_suspended = false;
static bool wifi_was_on_before_suspend = false;
static bool bt_was_on_before_suspend = false;

/* Tracks whether audio was playing as of the last tick the screen was off,
 * so the radio-suspend/idle-shutdown clocks (both anchored on
 * screen_off_since_tick) can be restarted when playback actually stops --
 * see the reset logic where this is used, just below. */
static bool screen_off_playback_active = false;

/* Idle shutdown: a full poweroff (see idle_shutdown.h) after
 * current_settings.idle_shutdown_minutes with the screen off and nothing
 * going on -- same gating as radio-suspend above (not playing, not
 * charging, no DAC receive mode active) since none of those should ever be
 * interrupted by the device turning itself off. idle_shutdown_attempted
 * guards against retrying every single tick if idle_shutdown_now() doesn't
 * actually terminate the process for some reason (e.g. /sbin/poweroff
 * missing) -- reset on wake so a genuine idle stretch always gets a fresh
 * attempt rather than being permanently given up on after one failure. */
static bool idle_shutdown_attempted = false;


static bool shutdown_background_work_active(void) {
    return gui_library_has_background_work() || gui_subsonic_has_background_work() ||
           gui_network_has_background_work() || gui_lyrics_has_background_work() ||
           gui_player_has_background_work() || gui_shell_has_background_work() ||
           plugin_manager_has_background_work() || playlist_files_has_active_write();
}

/* Real-device bug report: waking from suspend needed two power-button
 * presses -- see the resume fixup below (right after power_suspend_now())
 * for the full mechanism. An earlier version of this comment attributed the
 * bug entirely to an unreset LVGL inactivity clock (now fixed by
 * resume_from_suspend_fixups()'s own lv_display_trigger_activity() call)
 * and treated this window as covering only a rare, secondary race -- real
 * diagnostic logging during a live repro instead found this window itself
 * to be the actual remaining cause. hw_buttons.c only sets its
 * short-tap-consumed flag on the button's RELEASE, not the initial press
 * that wakes the kernel (see its own handle_key_event(), value==0 branch) --
 * and a real capture measured 2350ms between this window being armed (right
 * as power_suspend_now() returns, at/near the press that woke the kernel)
 * and that same press's release finally being consumed, comfortably past
 * the old 1000ms budget. Once the window auto-expires "unused," that
 * release is read as a brand new deliberate toggle-off press instead of
 * being recognized as the wake press's own echo -- exactly the "press 1
 * wakes then immediately goes dark, press 2 actually wakes it" report.
 * Raised with real margin above that measured gap. Short grace window, not
 * a one-shot drain: the physical press that wakes the kernel is captured by
 * hw_buttons.c's own independent evdev reader thread, whose timing relative
 * to this (main) thread's own resume handling isn't guaranteed -- a drain
 * attempted too early could miss a flag that thread hadn't set yet. Any
 * power-button press consumed within this window of a resume is treated as
 * an echo of the wake press and silently discarded rather than toggling the
 * screen; a genuinely deliberate second press to go back to sleep right
 * after waking landing inside this now-longer window is a rarer, but
 * real, tradeoff accepted in exchange for the wake press itself no longer
 * routinely misfiring. */
#define RESUME_POWER_DRAIN_WINDOW_MS 3000
static uint32_t resumed_from_suspend_tick = 0;
static bool resumed_from_suspend_pending = false;
/* power_suspend_now() returns from inside update_timer_cb(), after that
 * callback already sampled screen_was_on/screen_on_now. Remember that
 * out-of-band wake so the next control tick still runs every ordinary
 * screen_just_woke refresh/status/radio-reset action. */
static bool force_screen_just_woke = false;

/* Keeps the three pieces of screen runtime state inseparable. Merely
 * enabling an indev does not restart its paused LVGL read timer, and merely
 * lighting the panel does not restart LVGL's paused refresh timer. Used by
 * both the ordinary on/off edge below and the special suspend-return path,
 * which changes backlight state too late in the current timer callback for
 * that ordinary edge detector to observe it. */
static void apply_screen_runtime_state(bool screen_on) {
    lv_indev_enable(NULL, screen_on);
    audio_set_low_power_mode(!screen_on);

    gui_shell_reset_drag_state();
    gui_library_reset_drag_state();

    lv_timer_t * refr_timer = lv_display_get_refr_timer(lv_display_get_default());
    if (refr_timer) {
        if (screen_on) lv_timer_resume(refr_timer);
        else lv_timer_pause(refr_timer);
    }

    int indev_timer_count = 0;
    for (lv_indev_t * indev = lv_indev_get_next(NULL); indev; indev = lv_indev_get_next(indev)) {
        lv_timer_t * read_timer = lv_indev_get_read_timer(indev);
        if (!read_timer) continue;
        if (screen_on) lv_timer_resume(read_timer);
        else lv_timer_pause(read_timer);
        indev_timer_count++;
    }

    if (screen_on) lv_async_call(full_redraw_async_cb, NULL);
#ifdef UI_PERF_TRACE
    printf("PERF screen_runtime screen_on=%d refr_timer=%d indev_timers=%d\n",
           screen_on, refr_timer != NULL, indev_timer_count);
#endif
#ifdef UI_GESTURE_TRACE
    printf("[GESTURE_TRACE] apply_screen_runtime_state: screen_on=%d\n", screen_on);
#endif
}

/* Everything a caller of power_suspend_now() needs to do immediately after
 * it returns, factored out so Car Mode (below) can share it with the
 * idle-shutdown "suspend instead of power off" path this was originally
 * written for -- see that call site's own comment for the full history of
 * each individual fixup (backlight/indev resync, the inactivity-timer
 * flash-on-then-off bug, the two-presses-to-wake bug). Doesn't call
 * power_suspend_now() itself: the two callers gate entry into suspend
 * differently (a screen-off idle timer vs. a charging-edge transition), so
 * each still makes that call directly, right before calling this. */
static void resume_from_suspend_fixups(void) {
    backlight_set_screen_on(true);
    apply_screen_runtime_state(true);
    force_screen_just_woke = true;
    lv_display_trigger_activity(NULL);
    resumed_from_suspend_tick = lv_tick_get();
    resumed_from_suspend_pending = true;
    DBG_LOG("resume: grace window armed at tick=%u\n", resumed_from_suspend_tick);
}

/* Defined with the rest of the All Songs screen, much further down --
 * forward-declared here since update_timer_cb() (just below) needs it for
 * the remote-control play-by-index consumer. */
/* Defined with the rest of the remote-control scoped-play machinery, much
 * further down -- forward-declared here since update_timer_cb() (just
 * below) needs it for the remote-control play-by-index consumer. */

/* Defined with the rest of the power-off countdown popup, much further
 * down -- forward-declared here since update_timer_cb() (just below) needs
 * them for the power-button long-press consumer. */

/* Defined alongside the rest of live search's async DB query, much further
 * down -- forward-declared here since update_timer_cb() (just below) polls
 * it every tick, same as poll_cover_decode()/poll_lyrics_load(). */

/* Settings -> Playback -> Play/Pause Button (current_settings.
 * play_pause_button_mode). Mode 2 needs to tell a single press apart from
 * the first half of a double-click, so it defers the decision behind a
 * short timer -- same reset-then-resume idiom as TEXT_ENTRY_MULTITAP_MS
 * further down in this file -- rather than acting immediately; modes 0 and
 * 1 fire with zero added latency since there's nothing to disambiguate.
 * Real-device incident: update_timer_cb only polls hw_buttons every 500ms
 * (see its own lv_timer_create() call), so a click that straddles two poll
 * windows can show up here up to ~500ms later than the physical press that
 * caused it -- on top of the real gap between the two clicks themselves.
 * hw_buttons_consume_play_pause() returning a count instead of a bool
 * (see hw_buttons.c) already makes a double-click landing inside a single
 * poll window resolve immediately with no timer involved at all; this
 * window only needs to cover the slower, poll-straddling case, hence the
 * headroom above the 500ms poll period itself. */
#define PLAY_PAUSE_DOUBLE_CLICK_MS 700
static lv_timer_t * play_pause_click_timer = NULL;
static int play_pause_click_count = 0;

static void physical_skip_prev_track(void) {
    gui_player_step_manual(-1);
}

static void play_pause_click_timeout_cb(lv_timer_t * timer) {
    (void) timer;
    lv_timer_pause(play_pause_click_timer);
    play_pause_click_count = 0;
    toggle_play_pause();
}

static void handle_physical_play_pause_press(void) {
    int mode = current_settings.play_pause_button_mode;
    if (mode == 1) {
        physical_skip_prev_track();
        return;
    }
    if (mode == 2) {
        if (!play_pause_click_timer) {
            play_pause_click_timer = lv_timer_create(play_pause_click_timeout_cb, PLAY_PAUSE_DOUBLE_CLICK_MS, NULL);
            lv_timer_pause(play_pause_click_timer);
        }
        play_pause_click_count++;
        if (play_pause_click_count >= 2) {
            lv_timer_pause(play_pause_click_timer);
            play_pause_click_count = 0;
            physical_skip_prev_track();
        } else {
            lv_timer_reset(play_pause_click_timer);
            lv_timer_resume(play_pause_click_timer);
        }
        return;
    }
    toggle_play_pause();
}

static void update_timer_cb(lv_timer_t * timer) {
    (void) timer;

    /* Physical volume/skip/play-pause buttons: applied here, on the one
     * thread allowed to touch LVGL widgets (see hw_buttons.h). Play/pause
     * is a count, not a bool -- this poll only runs every 500ms, and a real
     * double-click's two presses routinely land inside one poll window; a
     * count of 2 here dispatches immediately, twice in a row, letting
     * handle_physical_play_pause_press()'s own click-count state (mode 2)
     * see it as a same-tick double-click without waiting on its timer. */
    int played_paused_count = hw_buttons_consume_play_pause();
    for (int i = 0; i < played_paused_count; i++) {
        handle_physical_play_pause_press();
    }
    /* Real-device bug report: shuffle "sometimes shuffles, sometimes just
     * plays the next song," and physical-button skip never shuffled at all.
     * Root cause -- this block used to step playlist_index by a plain +-1
     * regardless of play mode, rather than going through
     * compute_manual_step_index() (the same shuffle-aware/repeat-all-wrap
     * logic the touchscreen Prev/Next buttons already used, further down in
     * next_btn_event_cb/prev_btn_event_cb) -- so a physical button press
     * always played the literal next track in playlist order no matter what
     * current_settings.play_mode was. In Shuffle mode specifically, the
     * *touchscreen* buttons genuinely did shuffle every time; the
     * "sometimes" in the report was almost certainly this exact
     * inconsistency between the two input paths, not real randomness in
     * the shuffle logic itself. */
    bool skipped_next = hw_buttons_consume_next();
    if (skipped_next) {
        gui_player_step_manual(1);
    }
    bool skipped_prev = hw_buttons_consume_prev();
    if (skipped_prev) {
        gui_player_step_manual(-1);
    }

#ifndef HOST_BUILD
    /* Bluetooth accessory's own play/pause/next/previous buttons -- same
     * "background thread sets a flag, this is the one thread allowed to
     * touch LVGL/playlist state" pattern as the physical hw_buttons above,
     * since bt_media_player.c's D-Bus dispatch runs on its own thread. Now
     * shares the same shuffle-aware compute_manual_step_index() stepping as
     * hw_buttons and the touchscreen buttons -- see the shuffle bug comment
     * just above; a remote's buttons should behave like every other
     * "skip" input, not bypass play mode entirely. */
    if (bt_media_player_consume_play_pause()) {
        toggle_play_pause();
    }
    if (bt_media_player_consume_next()) {
        gui_player_step_manual(1);
    }
    if (bt_media_player_consume_prev()) {
        gui_player_step_manual(-1);
    }
    /* Keeps PlaybackStatus accurate for whenever BlueZ/the accessory next
     * queries it -- cheap (no subprocess, just a mutex-protected bool),
     * safe to do unconditionally every tick rather than hunting down every
     * call site that can change play state. */
    bt_media_player_notify_playback_state(audio_is_playing());
#endif

    int volume_delta = hw_buttons_consume_volume_delta();
    if (volume_delta != 0) {
        int32_t new_percent = gui_player_get_volume_percent() + volume_delta;
        if (new_percent < 0) new_percent = 0;
        if (new_percent > 100) new_percent = 100;
        gui_player_set_volume_percent(new_percent);
        audio_set_volume((float) new_percent / 100.0f);
        current_settings.volume = (float) new_percent / 100.0f;
        settings_save(&current_settings);
        show_volume_popup(new_percent);
        refresh_volume_topbar(new_percent);
    }

    /* Phone remote-control (Phase 2, remote_control.h): same "background
     * thread sets a flag, this is the one thread allowed to touch LVGL/
     * audio state" pattern as hw_buttons/bt_media_player just above --
     * reuses the exact same shuffle-aware stepping and volume-persistence
     * shape as those, rather than a separate implementation. */
    if (remote_control_consume_play_pause()) {
        toggle_play_pause();
    }
    if (remote_control_consume_next()) {
        gui_player_step_manual(1);
    }
    if (remote_control_consume_prev()) {
        gui_player_step_manual(-1);
    }
    if (remote_control_consume_mode_cycle()) {
        cycle_play_mode();
    }
    int remote_seek_seconds;
    if (remote_control_consume_seek(&remote_seek_seconds)) {
        audio_seek((double) remote_seek_seconds);
    }
    int remote_volume_percent;
    if (remote_control_consume_volume(&remote_volume_percent)) {
        gui_player_set_volume_percent(remote_volume_percent);
        audio_set_volume((float) remote_volume_percent / 100.0f);
        current_settings.volume = (float) remote_volume_percent / 100.0f;
        settings_save(&current_settings);
        show_volume_popup(remote_volume_percent);
        refresh_volume_topbar(remote_volume_percent);
    }
    int64_t remote_queue_id;
    if (remote_control_consume_queue_index(&remote_queue_id)) {
        song_row_t remote_queue_row;
        if (metadata_db_get_song_by_id(remote_queue_id, &remote_queue_row)) queue_add_song(remote_queue_row.path);
    }
    int remote_queue_remove_offset;
    if (remote_control_consume_queue_remove(&remote_queue_remove_offset))
        queue_remove_song_at_offset(remote_queue_remove_offset);
    if (remote_control_consume_queue_clear()) queue_clear_pending();
    int64_t remote_play_id;
    char remote_play_playlist[128], remote_play_artist[128], remote_play_album_artist[128], remote_play_album[128];
    if (remote_control_consume_play_index(&remote_play_id, remote_play_playlist, sizeof(remote_play_playlist),
                                           remote_play_artist, sizeof(remote_play_artist), remote_play_album_artist,
                                           sizeof(remote_play_album_artist), remote_play_album,
                                           sizeof(remote_play_album))) {
        /* remote_play_id is a song id (metadata_db.c's rowid-based
         * song_row_t.id) -- resolve it to a path, then hand that straight to
         * play_remote_control_song() (defined with the rest of the
         * remote-control scoped-play machinery, much further down), which
         * resolves the path plus the playlist/artist/album context into the
         * right playlist and position via its own DB queries, falling back
         * to the whole library (All Songs, by title offset) when no scope
         * applies. */
        song_row_t remote_play_row;
        if (metadata_db_get_song_by_id(remote_play_id, &remote_play_row)) {
            play_remote_control_song(remote_play_row.path, remote_play_playlist, remote_play_artist,
                                      remote_play_album_artist, remote_play_album);
        }
    }

    /* Auto-stop on headphone-output loss: this hardware has no built-in
     * speaker, so a mid-playback disconnect (headphone jack pulled, or the
     * connected Bluetooth A2DP headphone drops) leaves nowhere for the
     * audio to go -- stop outright rather than leaving it silently playing
     * into nothing. Checked every tick, deliberately NOT gated on
     * screen_on_now below (background, screen-off playback needs the same
     * protection). headphone_is_connected() is a cheap sysfs read (no
     * subprocess); the A2DP half reuses whatever poll_refresh_bt_icon()
     * last found rather than re-querying bluealsa-cli here every tick --
     * that's already throttled to its own ~5s cadence (see
     * refresh_bt_icon_thread_func()), and a few seconds of extra latency on
     * just the Bluetooth half is an acceptable tradeoff against forking a
     * process every single tick.
     *
     * Target-only: HOST_BUILD has no real jack/BT hardware to detect at
     * all (headphone_is_connected() is unconditionally false there, see
     * headphone_status.h), so this would otherwise fire a spurious "stop"
     * on the very first tick after starting playback in the simulator --
     * there's no real disconnect to protect against on host, since the dev
     * machine's own speakers are the actual output regardless of this
     * app's simulated jack/BT state. */
#ifndef HOST_BUILD
    /* Real-device bug report: "Bluetooth dropping connection randomly."
     * refresh_bt_icon_result_a2dp_connected comes from a subprocess-backed
     * poll (bluealsa-cli list-pcms, gated on bt_control_is_powered()) that
     * this file's own Bluetooth wedge-recovery logic (see
     * bt_control_recover_wedged_daemon()'s doc comment) already documents,
     * with a real-device strace to back it up, as prone to a single false
     * "not connected"/"No default controller" reading during a normal,
     * several-second-long bluetoothd/bluealsa busy window -- not a genuine
     * disconnect. That existing recovery logic requires several CONSECUTIVE
     * failures (TIMEOUT_RECOVERY_THRESHOLD) before concluding it's a real
     * wedge, specifically to filter out that blip. This auto-stop check
     * used to have no such filter at all -- a single bad poll cut playback
     * immediately, which is a far more likely explanation for "random"
     * drops than the radio itself actually losing the connection.
     *
     * Debounced on wall-clock time (not a "how many ticks in a row" counter
     * -- refresh_bt_icon_result_a2dp_connected only actually changes value
     * once every ~5s poll cycle, so counting 500ms update_timer_cb ticks
     * would just recount the same stale reading many times over) rather
     * than the raw signal, and only the Bluetooth half: headphone_is_
     * connected() is a direct, instant sysfs read with no such ambiguity,
     * so a wired jack pull still pauses playback immediately, unchanged --
     * this debounce only ever gated the Bluetooth reading going into the
     * same shared output_connected/audio_toggle_pause() check below, which
     * a wired pull reaches exactly the same way BT does. */
#define BT_OUTPUT_DISCONNECT_DEBOUNCE_MS 12000
    {
        static uint32_t bt_disconnected_since_tick = 0; /* 0 = currently connected (or never sampled) */
        if (gui_shell_is_bt_audio_connected()) {
            bt_disconnected_since_tick = 0;
        } else if (bt_disconnected_since_tick == 0) {
            bt_disconnected_since_tick = lv_tick_get();
        }
        /* Fast path: bt_control_output_disconnect_watch_start() (started
         * alongside audio_set_bt_output() -- see poll_refresh_bt_icon())
         * catches a real disconnect via bluealsa's own D-Bus signal in well
         * under a second, instead of waiting on this debounce's full 12s (on
         * top of refresh_bt_icon_result_a2dp_connected's own ~5s poll
         * cadence) -- see bluetooth_control.h's own comment on why the
         * debounce itself still has to stay, as the fallback for if this
         * monitor subprocess dies or bluealsa doesn't emit the signal.
         * Edge-triggered (consumed once), so this only forces the debounced
         * read false for the one tick right after a real removal -- harmless
         * if refresh_bt_icon_result_a2dp_connected is still stale-true a tick
         * later (nothing auto-resumes playback off output_connected going
         * back to true, so there's no user-visible flicker, just the stop
         * below firing sooner than the plain poll+debounce alone would have). */
        bool bt_connected_debounced = !bt_control_output_disconnect_consume() &&
            (gui_shell_is_bt_audio_connected() ||
             lv_tick_elaps(bt_disconnected_since_tick) < BT_OUTPUT_DISCONNECT_DEBOUNCE_MS);

        static bool last_output_connected = true; /* starts true so nothing fires before any real state has been sampled */
        bool output_connected = headphone_is_connected() || bt_connected_debounced;
        /* Real-device bug report: this used to call audio_stop() -- not
         * audio_toggle_pause() -- on a disconnect. audio_stop() (audio.c)
         * doesn't just silence output: its playback thread fully unwinds
         * the current track (decoder_close(), free()s the path, have_current
         * = false), the same as if the user had never opened it. That left
         * no way to resume once reconnected -- confirmed by a live test
         * report ("stops the music, instead of pause, so it can't be
         * resumed later"). audio_toggle_pause() keeps the decoder/position
         * intact and is what toggle_play_pause() itself already uses for an
         * ordinary pause tap, so resuming afterward (headphones reconnected,
         * tap play) picks up exactly where it left off. Only fires while
         * actually playing -- audio_is_paused() removed from the trigger
         * condition entirely, since toggling pause on an ALREADY-paused
         * track would incorrectly resume it into a dead/disconnected
         * output, the opposite of what this is for. */
        bool was_playing = audio_is_playing();
        if (last_output_connected && !output_connected && was_playing) {
            DBG_LOG("gui: output disconnected while playing -- pausing playback\n");
            audio_toggle_pause();
            set_play_button_state(false);
            show_error_toast("Paused: headphones disconnected");
        }
        last_output_connected = output_connected;
    }
#endif

    /* Car Mode (Settings toggle, off by default). Matches the stock
     * firmware's own real behavior (confirmed by real-device report):
     * unplugging power (car ignition off) checkpoints position and powers
     * the device off; plugging power back in (ignition on) powers it back
     * on and auto-resumes the same track/position (see gui_init()'s
     * resume-on-launch block, below).
     *
     * Reworked back to a full poweroff (idle_shutdown_now()) rather than
     * power_suspend_now() -- an earlier version of this used suspend-to-RAM
     * for a near-instant resume with no boot splash, but real-device
     * testing found two problems with that neither had a fix: suspend
     * never actually woke back up when power returned (this board's
     * PMIC/kernel wakeup-source support for VBUS insert, as opposed to a
     * power-button press, couldn't be confirmed -- see this function's own
     * prior git history for the sysfs probe that would be needed), and
     * suspending with an actively-connected Bluetooth output could reboot
     * the device outright. A full poweroff sidesteps both: /sbin/poweroff
     * and a cold power-on are both already proven-reliable on this
     * hardware (idle_shutdown_now() itself already uses poweroff as its
     * own default, for the same jzfb suspend/resume reason -- see
     * idle_shutdown.h), at the cost of a real boot splash + library check
     * on every resume instead of an instant one.
     *
     * Edge-triggered on physical external power, not battery_is_charging().
     * The 85% limiter disables the PMIC charger while leaving the cable
     * connected, which can make battery status say "Discharging" and used
     * to trigger a shutdown/resume boot loop.  The physical-power API reads
     * non-battery power_supply "online" nodes and returns UNKNOWN on an
     * ambiguous/read-failure sample; UNKNOWN never counts as unplugging.
     *
     * A real removal must also persist for three consecutive 500ms control
     * ticks before it becomes an edge.  This filters driver churn while a
     * USB supply is settling without making ignition-off shutdown feel
     * substantially delayed.  charge_limiter_is_holding() is only a
     * fallback for an UNKNOWN physical sample: while holding, an ambiguous
     * read is conservatively kept on the powered side rather than allowed
     * to advance a destructive shutdown decision.
     *
     * Still gated on something actually being loaded
     * (audio_is_playing() || audio_is_paused()) -- no track loaded means
     * nothing to "continue playing," so no reason to force a shutdown
     * cycle. Target-only, same reasoning as the auto-stop block above. */
#ifndef HOST_BUILD
    {
        enum { CAR_POWER_REMOVAL_DEBOUNCE_POLLS = 3 };
        static bool power_sample_initialized = false;
        static bool last_external_power = false;
        static unsigned int power_absent_polls = 0;
        static bool car_shutdown_pending = false;

        if (!current_settings.car_mode_enabled) {
            /* Re-sample from a clean baseline if Car Mode is enabled later;
             * enabling it while already unplugged is not itself a removal
             * edge and must not immediately power the device off. */
            power_sample_initialized = false;
            power_absent_polls = 0;
            car_shutdown_pending = false;
        } else {
            /* Keep the sysfs directory/file reads completely out of the
             * normal 500ms control path when Car Mode is disabled. */
            battery_external_power_state_t power_state = battery_get_external_power_state();
            if (power_state == BATTERY_EXTERNAL_POWER_UNKNOWN && charge_limiter_is_holding())
                power_state = BATTERY_EXTERNAL_POWER_CONNECTED;

            if (power_state == BATTERY_EXTERNAL_POWER_UNKNOWN) {
                /* Consecutive means consecutive valid samples.  A sysfs
                 * failure is not evidence of either state and must not let
                 * two old off samples plus one later off sample confirm a
                 * removal.  An already-confirmed pending shutdown is left
                 * intact; only a positive connected sample cancels it. */
                power_absent_polls = 0;
            } else {
                bool external_power = power_state == BATTERY_EXTERNAL_POWER_CONNECTED;
                if (!power_sample_initialized) {
                    last_external_power = external_power;
                    power_sample_initialized = true;
                    power_absent_polls = 0;
                    DBG_LOG("car_mode: initial external power=%d\n", external_power);
                } else if (external_power) {
                    if (!last_external_power || power_absent_polls > 0 || car_shutdown_pending)
                        DBG_LOG("car_mode: external power present; removal cancelled\n");
                    last_external_power = true;
                    power_absent_polls = 0;
                    car_shutdown_pending = false;
                } else if (last_external_power) {
                    if (power_absent_polls < CAR_POWER_REMOVAL_DEBOUNCE_POLLS) power_absent_polls++;
                    if (power_absent_polls >= CAR_POWER_REMOVAL_DEBOUNCE_POLLS) {
                        last_external_power = false;
                        car_shutdown_pending = audio_is_playing() || audio_is_paused();
                        DBG_LOG("car_mode: external power removal confirmed; shutdown_pending=%d\n",
                                car_shutdown_pending);
                    }
                }
            }
        }

        if (car_shutdown_pending && !shutdown_background_work_active()) {
            current_settings.last_position = audio_get_position_seconds();
            settings_save(&current_settings);
            idle_shutdown_now(); /* full poweroff -- does not return, see idle_shutdown.h */
        }
    }
#endif

    /* Auto screen-timeout. Physical button presses don't go through LVGL's
     * own indev system at all (see hw_buttons.h's own comment on why this
     * needs a raw evdev thread), so lv_display_get_inactive_time() alone
     * would only track touches -- feed it button activity too via
     * lv_display_trigger_activity(), or a session spent purely skipping
     * tracks would still auto-sleep the screen despite being actively used.
     * This only delays the NEXT auto-sleep, though -- it deliberately does
     * NOT wake an already-off screen (see the power-button block below for
     * why: touches don't either, real-device feedback was that a screen
     * meant to be off shouldn't relight itself from a pocket touch/bump,
     * only a deliberate power-button press should turn it back on). */
    if (played_paused_count > 0 || skipped_next || skipped_prev || volume_delta != 0) {
        lv_display_trigger_activity(NULL);
    }

    bool screen_was_on = backlight_screen_is_on();

    /* The ONLY way the screen turns back on, whether it went off from the
     * auto-timeout below or a previous manual power-button press -- deliberately
     * not touch, and not the hw button presses above either, even though both
     * feed the same LVGL inactivity clock: real-device feedback was that
     * touching a screen that's meant to be off (auto-timeout or manual) was
     * turning it back on, which isn't the expected "screen off means off
     * until you explicitly wake it" behavior. Applied here rather than
     * straight from the hw_buttons reader thread so the backlight toggle and
     * the LVGL inactivity clock (used below for the auto-timeout check)
     * update atomically on this thread -- toggling the backlight from the
     * other thread without also resetting the inactivity clock left it stuck
     * expired, so the auto-timeout check just below immediately re-fired and
     * turned the screen straight back off on the very next tick, a
     * press-to-wake that looked like a frozen black screen. */
    if (resumed_from_suspend_pending && lv_tick_elaps(resumed_from_suspend_tick) >= RESUME_POWER_DRAIN_WINDOW_MS) {
        DBG_LOG("resume: grace window expired unused at tick=%u\n", lv_tick_get());
        resumed_from_suspend_pending = false;
    }
    if (hw_buttons_consume_power()) {
        DBG_LOG("resume: hw_buttons_consume_power() true at tick=%u, pending=%d\n", lv_tick_get(), resumed_from_suspend_pending);
        if (resumed_from_suspend_pending) {
            /* Echo of the press that woke the device from suspend -- see
             * resumed_from_suspend_pending's own comment. Swallowed once
             * (not the whole window's worth of presses): only the wake
             * press itself should ever land here, so there's nothing more
             * to drain after the first one arrives, and continuing to
             * suppress every press for the rest of the window would make a
             * deliberate quick re-sleep tap right after waking do nothing. */
            resumed_from_suspend_pending = false;
        } else {
            lv_display_trigger_activity(NULL);
            backlight_set_screen_on(!backlight_screen_is_on());
        }
    }
    if (hw_buttons_consume_power_long_press()) {
        /* Same resumed_from_suspend_pending guard as the short-tap consumer
         * just above -- a long hold that woke the device from suspend
         * shouldn't also immediately pop up a power-off countdown the
         * instant the screen comes back on. power_off_countdown_active's
         * own guard (inside start_power_off_countdown() isn't needed here
         * since hw_buttons.c already only fires this once per physical
         * press) still lets a *second* long-press restart the countdown
         * from the top while one is already showing, which is fine. */
        if (resumed_from_suspend_pending) {
            resumed_from_suspend_pending = false;
        } else {
            /* Real-device bug report: the countdown popped up invisibly if
             * the screen was already asleep (from timeout or a previous
             * short tap) when the hold crossed the long-press threshold --
             * lv_display_trigger_activity() alone only resets the
             * inactivity clock, it doesn't touch the backlight. Explicit
             * backlight_set_screen_on(true) here (unconditional, same
             * idiom as resume_from_suspend_fixups()'s own wake-up) makes
             * screen_on_now below come out true, which is what already
             * drives lv_indev_enable(NULL, true) further down -- so this
             * also re-enables touch for the Cancel button, not just the
             * backlight. */
            backlight_set_screen_on(true);
            lv_display_trigger_activity(NULL);
            start_power_off_countdown();
        }
    }
    poll_power_off_countdown();

    uint32_t screen_inactive_ms = lv_display_get_inactive_time(NULL);
    if (interactive_ui_started) {
        uint32_t interactive_age_ms = lv_tick_elaps(interactive_ui_start_tick);
        if (screen_inactive_ms > interactive_age_ms) screen_inactive_ms = interactive_age_ms;
    }
    bool screen_on_before_timeout = backlight_screen_is_on();
    /* Reading lyrics while a track is actually playing is real, continuous
     * screen use with little or no touch activity to keep resetting LVGL's
     * own indev-driven inactivity clock (screen_inactive_ms above) --
     * exempt this screen from both dimming and the full auto-timeout
     * entirely, the same lv_screen_active()-gated exclusion shape already
     * used for gui_network_get_bt_dac_overlay()/gui_network_get_usb_dac_overlay() elsewhere in
     * this file (e.g. poll_quick_drawer_drag()'s own gesture exclusions),
     * rather than trying to synthesize fake touch activity to fool the
     * shared clock. Paused, though, is no different from sitting on any
     * other screen not actively being read/watched -- real-device feedback
     * was explicit that timeout/dim/suspend should behave normally then,
     * not stay suppressed just because the lyrics view happens to still be
     * open. */
    bool lyrics_screen_active = lv_screen_active() == gui_lyrics_get_screen() && audio_is_playing();
    if (current_settings.screen_dimming_enabled && screen_on_before_timeout &&
        !inactivity_dimmed && !lyrics_screen_active && screen_inactive_ms >= SCREEN_DIM_AFTER_MS) {
        backlight_set_dimmed(true);
        inactivity_dimmed = true;
    } else if (screen_on_before_timeout && inactivity_dimmed &&
               (!current_settings.screen_dimming_enabled || lyrics_screen_active || screen_inactive_ms < SCREEN_DIM_AFTER_MS)) {
        backlight_set_dimmed(false);
        inactivity_dimmed = false;
    }
    if (current_settings.screen_timeout_enabled && backlight_screen_is_on() &&
        !lyrics_screen_active &&
        screen_inactive_ms >= (uint32_t) current_settings.screen_timeout_seconds * 1000) {
        backlight_set_screen_on(false);
        inactivity_dimmed = false;
    }

    /* Battery: skip every bit of UI-only refresh work below (label updates,
     * and especially the wpa_cli/bluetoothctl subprocess forks behind
     * refresh_wifi_icon()/refresh_bt_icon()) while nobody can see the
     * screen -- otherwise a music session left playing overnight with the
     * screen asleep still forks a bluetoothctl process every
     * WIFI_POLL_TICKS ticks for pixels nobody's looking at. Force one
     * refresh right on wake so nothing looks stale afterwards instead of
     * waiting up to WIFI_POLL_TICKS ticks to catch up. */
    bool screen_on_now = backlight_screen_is_on();
    bool screen_just_woke = screen_on_now && (!screen_was_on || force_screen_just_woke);
    if (screen_just_woke) force_screen_just_woke = false;

    /* Touch input, not just the backlight, needs to follow screen on/off --
     * real-device feedback: a touch on a screen that's meant to be off was
     * both relighting it (fixed above, by no longer waking on plain
     * activity) AND, independent of that, still being delivered to whatever
     * was underneath in the dark (a blind touch could still press a button
     * or navigate a screen the user can't see). Disabling every indev while
     * off blocks both: LVGL never processes the touch at all, so there's
     * nothing left to wake the screen OR act on. Physical hardware buttons
     * are unaffected -- hw_buttons.c reads them on its own raw evdev thread
     * outside LVGL's indev system entirely, so play/pause/skip/volume/power
     * keep working with the screen dark, matching a normal DAP. */
    if (screen_on_now != screen_was_on) {
        apply_screen_runtime_state(screen_on_now);

        /* NEXT_TODO_IMPLEMENTATION_PROMPT.md Task 1 -- pause LVGL's own
         * display-refresh timer while the backlight is off: nothing on
         * screen can be seen, so there is no reason to keep re-rendering
         * (or even re-checking for invalidated areas) at anywhere near its
         * normal cadence. update_timer_cb() (this very function) is NOT
         * this timer -- it's registered completely separately in
         * gui_init() and keeps running every 500ms regardless of this,
         * which is what still services hardware buttons, charging,
         * hotplug, and async-job completion while dark (see this
         * function's own body for the full list). Resumed the moment the
         * screen comes back on, together with invalidating the active
         * screen and both persistent layers so the very first visible
         * frame after waking is already fully correct (current track/
         * position, battery/charge, Wi-Fi/BT, volume, album art, topbar
         * visibility) rather than showing whatever was on screen right
         * before it went dark. The invalidation itself is deferred one
         * async tick (full_redraw_async_cb, shared with slide_transition_
         * anim_x_cb()'s own compositor-failure recovery -- see its own
         * comment), not called inline here -- this function is itself
         * already running from inside the SAME lv_timer_handler() pass
         * that would need to service that redraw, and forcing a
         * synchronous re-entrant refresh from inside a timer callback is
         * exactly the kind of reentrancy this codebase has already hit
         * real bugs from elsewhere -- lv_async_call() runs within this
         * same lv_timer_handler() invocation, just after all timers
         * finish, which is still effectively immediate. */
        /* Real-device finding: pausing the display-refresh timer alone did
         * NOT reduce main-loop wakeup frequency at all -- confirmed via
         * UI_PERF_TRACE, the loop kept waking ~65 times/second with the
         * screen off, just doing far less work per wakeup (avg handler time
         * dropped from ~250-500us to ~15-45us, but the WAKEUP ITSELF still
         * costs power regardless of how little work it does once awake).
         * Root cause: lv_indev_enable(NULL, false) above only suppresses
         * event dispatch -- each registered indev (the touchscreen here)
         * has its OWN separate periodic read timer (lv_indev_get_read_
         * timer(), created internally by LVGL alongside the indev itself)
         * that keeps polling on its own default ~16-33ms period regardless
         * of whether the indev is enabled. Pausing that too, for every
         * registered indev, is what actually lets the idle-cap change in
         * main.c matter. */
    }

    if (screen_just_woke) {
        inactivity_dimmed = false;
        idle_shutdown_attempted = false;
        if (radios_suspended) {
            gui_shell_resume_connections(wifi_was_on_before_suspend, bt_was_on_before_suspend);
            radios_suspended = false;
        }
    } else if (!screen_on_now) {
        bool playing_now_for_idle = audio_is_playing();
        if (screen_was_on) {
            /* Just went to sleep this tick -- the 10-minute countdown starts
             * from here, not from whenever the inactivity that caused it
             * began. */
            screen_off_since_tick = lv_tick_get();
        } else if (screen_off_playback_active && !playing_now_for_idle) {
            /* Real-device bug report: playback that continued past the
             * screen going dark (e.g. a long album) reaching the end of the
             * queue could suspend/poweroff the device on the very next tick
             * instead of waiting a full RADIO_SUSPEND_DELAY_MS/
             * idle_shutdown_minutes window -- because both clocks below are
             * measured from screen_off_since_tick (when the SCREEN went
             * off), which had already elapsed past the threshold while
             * audio_is_playing() was gating them off. Restart the clock the
             * moment playback actually stops, so the device only sleeps
             * after being genuinely idle (screen off AND silent) for the
             * configured duration, not merely screen-off. */
            screen_off_since_tick = lv_tick_get();
        }
        screen_off_playback_active = playing_now_for_idle;
        /* bt_is_powered_cached, not bt_control_is_powered(), deliberately --
         * the latter forks a process, and this condition is checked every
         * tick while the screen is off, not throttled like refresh_bt_icon()
         * is. The cached value is frozen at whatever it was when the screen
         * went dark, which is fine: nothing in this app can change BT power
         * state without a UI the user can't reach with the screen off. */
        if (!radios_suspended && !current_settings.wifi_dac_mode_enabled && !current_settings.bt_dac_mode_enabled &&
            !audio_is_playing() && !battery_is_charging() && !shutdown_background_work_active() &&
            lv_tick_elaps(screen_off_since_tick) >= RADIO_SUSPEND_DELAY_MS) {
            gui_shell_suspend_connections(&wifi_was_on_before_suspend, &bt_was_on_before_suspend);
            radios_suspended = true;
        }

        /* Deliberately independent of radios_suspended -- idle_shutdown_minutes
         * is a separate, normally-longer setting than RADIO_SUSPEND_DELAY_MS,
         * and shutdown should still happen on its own schedule even if radio
         * suspend is impossible right now (e.g. wifi_dac_mode_enabled) --
         * except it explicitly shares the same DAC-mode/playing/charging
         * gate, since none of those should ever be interrupted by the
         * device powering itself off out from under them. */
        if (!idle_shutdown_attempted && current_settings.idle_shutdown_enabled &&
            !current_settings.wifi_dac_mode_enabled && !current_settings.bt_dac_mode_enabled &&
            !audio_is_playing() && !battery_is_charging() && !shutdown_background_work_active() &&
            lv_tick_elaps(screen_off_since_tick) >= (uint32_t) current_settings.idle_shutdown_minutes * 60 * 1000) {
            if (current_settings.idle_suspend_enabled) {
                power_suspend_now();

                /* Real-device bug report: after resuming from suspend, the
                 * backlight itself came back on (the kernel's own fbdev/
                 * backlight power-notifier chain does that automatically in
                 * response to power_suspend_now()'s raw `/sys/class/
                 * graphics/fb0/blank` write) but the UI itself was never
                 * visible again -- confirmed root cause: that raw sysfs
                 * write completely bypasses this app's OWN screen-on state
                 * (backlight.c's screen_on static, only ever flipped by
                 * backlight_set_screen_on()) and this function's own
                 * indev-enable logic just above, which only reacts to a
                 * screen_on_now/screen_was_on transition it detects BETWEEN
                 * ticks -- but both are read from the exact same
                 * backlight_screen_is_on() call, so flipping that state
                 * mid-tick here (rather than waiting for a real transition
                 * to be observed across two separate ticks) would never be
                 * seen as an "edge" and that logic would never fire again.
                 *
                 * Also folds in one of two fixes for a related real-device
                 * report -- waking from suspend needed two power-button
                 * presses, the first visibly flashing the backlight on then
                 * straight back off. First cause: lv_display_get_inactive_time(NULL),
                 * read by the auto-screen-timeout check just below, is fed
                 * only by lv_display_trigger_activity() calls, none of which
                 * happen anywhere during the entire suspend duration (the
                 * whole app, including this timer, is frozen) -- so the
                 * instant the screen is force-enabled, that check could see
                 * an inactivity duration spanning the sleep and immediately
                 * flip the screen back off again on this same tick.
                 * Resetting it explicitly here (lv_display_trigger_activity()
                 * below) fixes that half. A second, independent cause
                 * remained even with that fix: RESUME_POWER_DRAIN_WINDOW_MS's
                 * own grace window (see its comment, right above
                 * resume_from_suspend_fixups()) was too short relative to
                 * how late the wake press's own RELEASE (not its down edge)
                 * gets consumed -- confirmed by real diagnostic logging
                 * during a live repro, not by inspection alone.
                 *
                 * See resume_from_suspend_fixups()'s own comment for why
                 * this is shared with Car Mode's own suspend call below. */
                resume_from_suspend_fixups();

                /* Unlike idle_shutdown_now() (which never returns --
                 * poweroff ends the process), this call CAN legitimately
                 * return without a real user wake (e.g. a spurious IRQ) --
                 * screen_just_woke won't fire to reset this flag in that
                 * case, so reset it here instead. Worst case if the device
                 * really did just wake for a moment is one immediate
                 * re-suspend on the next tick, not getting stuck awake
                 * indefinitely after a single spurious wake. */
                idle_shutdown_attempted = false;
            } else {
                idle_shutdown_attempted = true;
                idle_shutdown_now();
            }
        }
    }

    if (screen_on_now) {
        gui_shell_update_topbar(screen_just_woke);
    }

    /* Also unconditional on screen state -- charging happens with the
     * screen off far more often than on, so gating this the same way as the
     * topbar refreshes above would leave it capped at whatever percent the
     * screen happened to be on last. charge_limiter_poll() throttles its
     * own actual sysfs work internally, so calling it every tick here is
     * cheap. */
    charge_limiter_poll(current_settings.charge_limiter_enabled, false);
    safe_charging_poll(current_settings.safe_charging_enabled, false);
    led_control_poll(current_settings.led_indicator_enabled);

    if (current_settings.remote_control_enabled) {
        /* No separate now-playing metadata cache exists in this app beyond
         * what's already on screen -- song_title_label/song_folder_label
         * are this app's own single source of truth for title/artist (see
         * apply_track_metadata_to_ui()), so read them back rather than
         * standing up a second copy of the same state just for this.
         * Album isn't tracked anywhere after the initial metadata_read()
         * call, so it's left blank here -- a real gap, not an oversight,
         * see remote_control.h's own Phase 1 scope note. */
        const char * now_playing_path = gui_player_has_active_track()
                ? gui_player_get_current_track_path()
                : NULL;
        remote_control_notify_status(audio_is_playing(), audio_is_paused(), gui_player_get_now_playing_title(),
                                      gui_player_get_now_playing_folder(), "", now_playing_path,
                                      (int) audio_get_position_seconds(), (int) audio_get_duration_seconds(),
                                      audio_get_volume(), current_settings.play_mode);
    }

    /* Polling for in-flight async operations (wifi/bt connect, library scan,
     * subsonic sync, ...) keeps running regardless of screen state -- these
     * only do real work when a request the user already made is still
     * pending, so gating them on screen state would leave that operation
     * stuck until the user wakes the screen back up. */
    poll_subsonic_download();
    poll_subsonic_library_download();
    poll_dlna_control();
    poll_subsonic_connect();
    poll_subsonic_browse();
    poll_wifi_scan();
    poll_wifi_connect();
    poll_wifi_connect_saved();
    poll_wifi_disconnect();
    poll_wifi_forget();
    gui_shell_poll();
    plugin_manager_poll();
    poll_bt_scan();
    poll_bt_connect();
    poll_bt_forget();
    poll_library_rescan();
    poll_sd_format();
    poll_import_web_stop();
    poll_usb_mode_switch();
    poll_usb_storage_hotplug();
    poll_sd_card_hotplug();
    poll_cover_decode();
    gui_lyrics_poll_load();
    gui_lyrics_poll_backdrop();
    poll_search_job();

    uint64_t playback_err_gen = 0;
    audio_error_t playback_err = audio_consume_error_ex(&playback_err_gen);
    if (playback_err != AUDIO_ERROR_NONE) {
        gui_player_handle_playback_error_ex(playback_err, playback_err_gen);
    } else if (audio_consume_track_advanced()) {
        gui_player_handle_auto_advance();
    } else if (audio_consume_track_finished()) {
        gui_player_handle_track_finished();
    }
    gui_player_poll_confirmed_playback();
    gui_track_info_poll();

    /* All correctness-critical work above (buttons, queue transitions and
     * completion of requested background operations) still runs at 500 ms.
     * Everything below only redraws the player screen, so skip it while the
     * panel is physically off. This preserves hardware-button latency while
     * eliminating invisible slider/label rendering and asset I/O. */
    if (!backlight_screen_is_on()) return;

    if (!gui_player_has_active_track() || gui_player_is_seeking()) return;

    gui_player_update_progress();

    refresh_format_badge();
}



/* Same true-black default screen_builders.c's own style_theme_screen_bg is
 * initialized with. Kept as a separate plain literal (not that shared
 * style) specifically for gui_show_boot_splash() below -- that function
 * runs from main.c before gui_init() (and screen_builders_init_list_row_style(),
 * which initializes style_theme_screen_bg) has ever run, so the shared
 * style would still be all-zero memory at that point. */
#define SCREEN_BG_COLOR lv_color_make(0, 0, 0)

/* Real device uptime (lv_tick_get() is CLOCK_MONOTONIC-backed, see main.c's
 * custom_tick_get()) at which gui_show_boot_splash() below was called --
 * read back by gui_init() near the end of its own setup to decide how much
 * longer, if any, the splash needs to stay up. 0 is never a real tick value
 * this far into boot, so it doubles as "splash was never shown". */
static uint32_t boot_splash_start_tick = 0;

/* Stock player's own boot image stays up at least this long -- see
 * gui_show_boot_splash()'s comment. Used by gui_init()'s own settle-wait,
 * further below.
 *
 * Deliberately does NOT wait for /etc/init.d/S80_bt_init's own
 * /usr/bin/bt_init to finish -- real-device confirmed requirement:
 * Bluetooth must read "off" right when the splash lifts and STAY off
 * (no auto-enable of any kind) until the user explicitly turns it on
 * themselves. Two earlier attempts at extending this wait to hide
 * bt_init's own boot-time Bluetooth behavior, and a later attempt at
 * having this app auto-enable Bluetooth once bt_init finished in the
 * background, were all tried and reverted -- this app now does neither:
 * it just leaves Bluetooth exactly where bt_init's own (patched) script
 * leaves it, off, and never touches it again until the user does. */
#define BOOT_SPLASH_MIN_DISPLAY_MS 3000

/* Task #44 (stock-UX request): the stock firmware holds its own boot image
 * on screen for a few seconds after the very first kernel/bootloader logo
 * (S11jpeg_display_shell), before its player is interactive -- this app had
 * no equivalent, racing straight into screen-building and Bluetooth polling
 * within ~2s of process start. Called from main.c immediately after the
 * framebuffer is ready, before any of gui_init()'s own (much heavier) setup
 * work, so this is the very first thing this app ever paints, and so the
 * elapsed-time math in gui_init()'s own wait (see boot_splash_start_tick's
 * use below) is measured from as close to true process start as possible.
 *
 * Uses asset_path("boot_animation/en/0.png") rather than a hardcoded path:
 * this app's own THEME_ROOT (theme2) has no boot_animation asset at all --
 * only theme1 does, and that file is the stock "HIBY" wordmark, which this
 * project deliberately doesn't ship (see 89d7ca6d9, "rename app off the
 * HiBy trademark"). asset_path()'s existing THEME_OVERRIDE_ROOT check
 * (assets.c) means a non-trademarked replacement dropped at
 * /usr/data/theme_overrides/boot_animation/en/0.png is picked up
 * automatically with no code change or reflash. Until that file exists,
 * the underlying decode simply fails and lv_image renders nothing --
 * SCREEN_BG_COLOR alone still gives a clean black screen instead of
 * whatever the framebuffer previously held, so this is safe either way.
 *
 * Real-device bug report: the status bar (build_status_bar(), and every
 * other overlay this app builds the same way -- popups, quick drawer,
 * volume popup) parents itself onto lv_layer_top(), LVGL's global overlay
 * layer that renders above whichever screen is active *regardless* of
 * lv_screen_load() -- it's not scoped to gui_shell_get_home_screen() at all. Since
 * gui_init() builds the status bar (and starts populating its icons) while
 * this splash is still the loaded screen, it was appearing on top of the
 * splash within about a second of boot, well before gui_init()'s own
 * settle-wait even finished -- reading as "the splash barely showed" even
 * though the splash screen object itself stayed loaded for the full
 * BOOT_SPLASH_MIN_DISPLAY_MS (confirmed via boot_debug.log timestamps).
 * Hiding the whole top layer here and revealing it once gui_init()'s wait
 * is done (see its own call site further below) keeps every one of those
 * overlays off-screen for exactly as long as the splash itself is up,
 * without needing to touch each overlay builder individually. */
void gui_show_boot_splash(void) {
    boot_splash_start_tick = lv_tick_get();

    lv_obj_add_flag(lv_layer_top(), LV_OBJ_FLAG_HIDDEN);

    lv_obj_t * scr = lv_obj_create(NULL);
    /* Plain literal, not style_theme_screen_bg -- this runs from main.c
     * before gui_init() (and its lv_style_init(&style_theme_screen_bg))
     * has ever run, so that style object would still be all-zero memory
     * here. */
    lv_obj_set_style_bg_color(scr, SCREEN_BG_COLOR, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

    lv_obj_t * img = lv_image_create(scr);
    lv_image_set_src(img, asset_path("boot_animation/en/0.png"));
    lv_obj_center(img);

    lv_screen_load(scr);
    lv_timer_handler(); /* force an immediate render -- nothing else pumps the loop until main.c's own main loop starts */
}


/* build_files_screen moved to gui_library.c */



const char * basename_of(const char * path) {
    const char * slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}
/* group_song_entries moved to gui_library.c */


const char * gui_plugin_get_play_mode(void) {
    switch ((play_mode_t) current_settings.play_mode) {
        case PLAY_MODE_REPEAT_ALL: return "repeat_all";
        case PLAY_MODE_REPEAT_ONE: return "repeat_one";
        case PLAY_MODE_SHUFFLE:    return "shuffle";
        case PLAY_MODE_SEQUENTIAL:
        default:                   return "sequential";
    }
    return "sequential";
}

const char * gui_plugin_get_current_track_path(void) {
    return gui_player_has_active_track() ? gui_player_get_current_track_path() : NULL;
}

char ** gui_plugin_get_artist_albums(const char * artist, int * out_count) {
    *out_count = 0;
    int64_t count64 = metadata_db_count_albums_for_group(METADATA_DB_GROUP_ARTIST, artist);
    if (count64 <= 0 || count64 > INT_MAX) return NULL;
    int album_count = (int) count64;
    group_row_t * rows = malloc(sizeof(*rows) * (size_t) album_count);
    char ** names = calloc((size_t) album_count, sizeof(*names));
    if (!rows || !names) { free(rows); free(names); return NULL; }
    int n = metadata_db_get_albums_for_group(METADATA_DB_GROUP_ARTIST, artist, 0, album_count, rows);
    for (int i = 0; i < n; i++) {
        names[i] = strdup(rows[i].name);
        if (!names[i]) {
            for (int j = 0; j < i; j++) free(names[j]);
            free(names);
            free(rows);
            return NULL;
        }
    }
    free(rows);
    *out_count = n;
    return names;
}

static char ** load_plugin_album_paths(const char * artist, const char * album, int * out_count) {
    *out_count = 0;
    int64_t count64 = metadata_db_count_songs_filtered(NULL, artist, NULL, album);
    if (count64 <= 0 || count64 > INT_MAX) return NULL;
    int count = (int) count64;
    char ** paths = calloc((size_t) count, sizeof(*paths));
    if (!paths) return NULL;
    song_row_t rows[64];
    int loaded = 0;
    while (loaded < count) {
        int want = count - loaded;
        if (want > 64) want = 64;
        int got = metadata_db_get_songs_filtered_page(NULL, artist, NULL, album, loaded, want, rows);
        if (got <= 0) break;
        for (int i = 0; i < got; i++) {
            paths[loaded + i] = strdup(rows[i].path);
            if (!paths[loaded + i]) {
                for (int j = 0; j < loaded + i; j++) free(paths[j]);
                free(paths);
                return NULL;
            }
        }
        loaded += got;
        if (got < want) break;
    }
    *out_count = loaded;
    return paths;
}

char ** gui_plugin_get_album_tracks(const char * artist, const char * album, int * out_count) {
    *out_count = 0;
    return load_plugin_album_paths(artist, album, out_count);
}

char ** gui_plugin_get_next_album_tracks(const char * artist, const char * current_album, int * out_count) {
    *out_count = 0;
    int64_t count64 = metadata_db_count_albums_for_group(METADATA_DB_GROUP_ARTIST, artist);
    if (count64 <= 1 || count64 > INT_MAX) return NULL;
    group_row_t rows[32];
    int offset = 0;
    while (offset < count64) {
        int want = (int) (count64 - offset);
        if (want > 32) want = 32;
        int got = metadata_db_get_albums_for_group(METADATA_DB_GROUP_ARTIST, artist, offset, want, rows);
        if (got <= 0) break;
        for (int i = 0; i < got; i++) {
            if (strcasecmp(rows[i].name, current_album) != 0) continue;
            if (i + 1 < got) return load_plugin_album_paths(artist, rows[i + 1].name, out_count);
            group_row_t next;
            if (metadata_db_get_albums_for_group(METADATA_DB_GROUP_ARTIST, artist, offset + got, 1, &next) == 1)
                return load_plugin_album_paths(artist, next.name, out_count);
            return NULL;
        }
        offset += got;
        if (got < want) break;
    }
    return NULL;
}

/* Plugin library API moved to gui_plugins.c */

/* A stuck read on a corrupted SD card block (confirmed on a real device via
 * /proc/<pid>/task/<tid>/status+wchan showing the main thread parked in
 * __bread_gfp, a kernel block-device read, after the card came back from an
 * unclean unmount) lands the calling thread in an uninterruptible (D-state)
 * kernel wait. That can't be interrupted by a signal, alarm(), or
 * pthread_cancel() -- the only way to bound it is to run the risky call on
 * its own throwaway thread and simply stop waiting on it if it doesn't
 * finish in time. On timeout, the thread and its heap-allocated work struct
 * below are deliberately never joined or freed: the worker may still be
 * blocked in the kernel indefinitely, joining it would just reintroduce the
 * exact hang this exists to avoid, and freeing memory it might still write
 * to would be a use-after-free. This leaks one thread and one small
 * allocation per genuinely stuck path for the life of the process, which
 * only happens on real filesystem corruption -- a bounded cost, and far
 * better than the whole UI freezing. */
/* Real-device bug report: a library of a few thousand tracks spread across
 * many subfolders (nested Artist/Album directories on a plain SD card
 * labeled "Music") scanned as completely empty -- no error shown anywhere
 * a real deployment could surface one (stderr has no reader outside a
 * TEST_BUILD_TAG debug session, see debug_log.h), just a silently empty
 * library. Root cause: this used to be a flat wall-clock budget for the
 * ENTIRE recursive walk (file_browser.c's scan_all_songs_recursive()) --
 * fine for the genuine-corruption case it was built for (a single lstat()
 * stuck in an uninterruptible kernel wait, same reasoning as
 * LIBRARY_SCAN_FILE_TIMEOUT_MS above), but it couldn't tell that apart from
 * a large, healthy, deeply-nested library on a slow card just legitimately
 * taking longer than the flat budget to finish readdir()+lstat() on every
 * entry -- the whole scan was discarded either way. Now a stall timeout
 * instead (see the progress-counter poll loop below): as long as
 * file_browser_scan_all_songs() keeps making forward progress (any real
 * directory entry examined, not just playable files -- see its own
 * progress parameter's doc comment in file_browser.h), no matter how long
 * the walk takes in total, it's never treated as stuck. Only a stretch of
 * this many ms with zero new progress -- the actual "something is
 * genuinely wedged" signal -- gives up. */

/* Library scan moved to gui_library.c */

/* Search bindings and the All Songs, Recently Added, and grouped-song
 * implementations moved to gui_library.c. */


/* ---- Subsonic-compatible network streaming ----
 *
 * mp3/flac songs (the two decoders audio.c can open against a true network
 * stream -- see decoder_open()'s own comment in audio.c) play directly off
 * the server via subsonic_song_row_click_cb() below, no local file at all.
 * Every other format (aac/ogg/wma/m4a/ape/opus/wav/aiff/dsf -- whatever
 * this server's own transcoding settings hand back) still downloads the
 * whole track first via the mechanism right below, before handing it to the
 * existing local-file-only decoder for that format: retrofitting every
 * remaining decoder here (the vendored AIFF/DSD/AAC/ALAC/APE ones, all
 * built around a plain seekable FILE*) for streaming reads is a much bigger
 * project than this round's scope. The downloaded copy lands on the SD card
 * (SUBSONIC_STREAM_CACHE_DIR below), not /tmp -- see that macro's own
 * comment for why. */


/* Background download thread + a "done" flag polled from update_timer_cb --
 * the same shape as audio.c's own track_finished flag, for the same reason:
 * LVGL isn't thread-safe, so the thread can't touch any screen/widget
 * state directly. */
static volatile bool download_success_flag = false;

typedef struct {
    char url[1536];
    char dest_path[512];
    bool verify_tls;
} download_request_t;




/* DLNA/UPnP-AV cast-and-play -- see dlna_control.h for the full mechanism
 * (dmrd relays SetAVTransportURI/Play/Stop over an undocumented socket;
 * this module downloads the cast URL and hands it to the normal local
 * playback pipeline, same download-then-play shape as Subsonic streaming
 * just above, and the same reason: no decoder here reads a true network
 * stream). Relies on the downloaded file's own embedded tags for Now
 * Playing display via the standard on_file_selected() path, same as
 * Subsonic, rather than the richer DIDL-Lite title/artist/album
 * dlna_control_consume_ready_track() also provides -- real casts observed
 * during this feature's investigation had matching embedded tags, and
 * threading an override path through apply_track_metadata_to_ui() for a
 * case that may not come up in practice wasn't worth the added risk this
 * round. No "please wait" screen the way subsonic downloads get one --
 * a cast is user-initiated from another device, not from a tap on this
 * screen, so there's no local navigation context to hold open while it
 * downloads. */
static void poll_dlna_control(void) {
    char path[512], title[256], artist[256], album[256];
    if (dlna_control_consume_ready_track(path, sizeof(path), title, sizeof(title),
                                          artist, sizeof(artist), album, sizeof(album))) {
        char ** playlist = malloc(sizeof(char *));
        playlist[0] = strdup(path);
        clear_player_source(); /* cast from another device -- no on-device list to go back to */
        on_file_selected(playlist, 1, 0);
    }
    if (dlna_control_consume_stop_requested()) {
        audio_stop();
        set_play_button_state(false);
        plugin_manager_notify_stopped();
    }
    dlna_control_notify_status(audio_is_playing(), audio_is_paused());
}



static lv_obj_t * build_stream_media_screen(void) {
    static icon_grid_item_t items[1 + PLUGIN_MAX_STREAM_TILES];
    items[0] = (icon_grid_item_t){ "stream_media/subsonic.png", "stream_media/subsonic_s.png", "Subsonic", subsonic_tile_cb, NULL };

    int count = 1;
    int plugin_count = plugin_manager_get_stream_tile_count();
    for (int i = 0; i < plugin_count && i < PLUGIN_MAX_STREAM_TILES; i++) {
        items[count++] = (icon_grid_item_t){
            plugin_manager_get_stream_tile_icon(i), plugin_manager_get_stream_tile_icon_selected(i),
            plugin_manager_get_stream_tile_label(i), plugin_stream_tile_click_cb, (void *) (intptr_t) i
        };
    }

    lv_obj_t * scr = build_icon_grid_screen("Stream Media", generic_back_cb, items, count, 100, false, 0);
    finalize_screen_navigation(scr);
    return scr;
}


void gui_init(uint32_t screen_width, uint32_t screen_height) {
#ifndef HOST_BUILD
    boot_checkpoint("gui_init entered");
#endif
    settings_load(&current_settings);
#ifndef HOST_BUILD
    boot_checkpoint("settings_load done");
#endif
    /* Must run before anything could turn Wi-Fi/Bluetooth on and trigger
     * wifi_on.sh/bt_init's own one-time read of the file this bind-mounts
     * over -- see hostname_apply()'s own comment. */
    gui_theme_init();
    gui_notifications_init();
    hostname_apply(current_settings.hostname);

    /* Must run before any screen below captures a gui_theme_font(GUI_FONT_ROLE_SUBTEXT)/20/22/28
     * pointer into its own style -- see this function's own doc comment. */

    /* Correct the persisted USB mode against live gadget state before
     * anything else reads it (external_dac_block_reason() in particular --
     * a stale usb_mode==USB_MODE_DAC left over from a previous run would
     * otherwise wrongly block local playback from the moment the app
     * starts). Hardware USB gadget state doesn't survive a reboot, but this
     * binary can also be killed and relaunched without a reboot (as it was
     * repeatedly during development), which is exactly when the persisted
     * value and reality can disagree. */
    usb_mode_t detected_usb_mode;
    if (usb_mode_control_detect_current(&detected_usb_mode)) current_settings.usb_mode = (int) detected_usb_mode;

    /* current_settings.volume itself is left untouched here even when the
     * fixed-startup path below is taken -- it keeps tracking "last used"
     * independently (still updated normally by volume_slider_event_cb's
     * RELEASED case during the session), so turning startup_volume_fixed_enabled
     * back off later resumes from wherever the slider was really last left,
     * not from stale fixed-mode state. */
    audio_set_volume(current_settings.startup_volume_fixed_enabled
                          ? (float) current_settings.startup_volume_fixed_percent / 100.0f
                          : current_settings.volume); /* picked up below when the volume slider reads audio_get_volume() */
    audio_set_crossfade_enabled(current_settings.crossfade_enabled);

    /* Real-device bug report: brightness jumped to an arbitrary value after
     * a real power-down/power-up -- root cause, nothing in this app applied
     * ANY brightness at startup before this, so the screen just came up at
     * whatever raw value the kernel/bootloader itself left the backlight
     * sysfs attribute at. See settings.h's own comment on brightness_percent. */
    backlight_set_normal_percent(current_settings.brightness_percent);

    led_control_apply(current_settings.led_indicator_enabled);
    charge_limiter_poll(current_settings.charge_limiter_enabled, true);
    safe_charging_poll(current_settings.safe_charging_enabled, true);
    if (current_settings.timezone[0] != '\0') timezone_apply(current_settings.timezone);

    /* Reapply persisted external-DAC state -- see
     * start_bt_dac_startup_reapply_if_needed()'s own comment for the real
     * incident this fixes. AirPlay's equivalent (wifi_dac_mode_enabled) has
     * the same shape of bug but no slow chip-init step, so it's cheap enough
     * to just call directly here rather than needing its own background
     * thread. */
    if (current_settings.wifi_dac_mode_enabled) {
        char name[64];
        get_device_name(name, sizeof(name));
        airplay_control_start(name);
    }
    if (current_settings.dlna_renderer_enabled) dlna_control_start();
    if (current_settings.remote_control_enabled) remote_control_start();

    /* The quick drawer's own open/close drag tracking is polled from
     * update_timer_cb instead (poll_quick_drawer_drag()) -- see its comment
     * for why event-based approaches (both indev-wide LV_EVENT_PRESSED/
     * PRESSING and per-object LV_EVENT_GESTURE bubbling) proved unreliable
     * here specifically. */

    /* LV_OPA_80, not LV_OPA_COVER -- see apply_accent_color()'s own comment
     * on this same property for why COVER flattens on.png's handle and
     * track into one indistinguishable solid color. Mirrored here since
     * this is the initial setup at boot, before apply_accent_color() might
     * ever run again. */



    /* Discovers plugin rows/tiles by loading and running every .lua file
     * under <SD card>/.plugins/ -- run early, well before Books, Settings,
     * or Stream Media (the current plugin entry points, see build_books_
     * screen()/build_settings_screen()/build_stream_media_screen()) could
     * plausibly be reached. */
    plugin_manager_init();
#ifndef HOST_BUILD
    boot_checkpoint("pre-screen-build setup done");
#endif

    gui_player_init(screen_width, screen_height);
#ifndef HOST_BUILD
    boot_checkpoint("build_player_screen done");
#endif
    gui_lyrics_init();
    /* Converts any pre-existing absolute-path playlist entries (everything
     * written before playlist_files_append()/_create() started writing
     * relative ones) to relative -- see playlist_files_migrate_to_relative()'s
     * own comment. Runs before the first read of any playlist below. Host-only
     * exclusion matches migrate_old_db_if_needed()'s own -- ./music/Playlists
     * locally is dev test fixtures, not real user data. */
#ifndef HOST_BUILD
    playlist_files_migrate_to_relative(PLAYLISTS_DIR);
#endif
    library_load_from_cache_only();
#ifndef HOST_BUILD
    boot_checkpoint("library_load_from_cache_only done");
#endif
    /* No whole-library load anywhere in this boot path, on purpose --
     * remote_control.c queries metadata_db.c directly (its own METADATA_DB_
     * GUARD) rather than needing a synced copy of the library, and each of
     * the five build_*_screen() calls above already activates its own
     * paged provider against the DB internally (see build_all_songs_
     * screen()'s own comment), so there's nothing left that would need a
     * whole-library array at boot for any reason, Remote Control enabled
     * or not. */
/* A-Z index & search registered in gui_library_init */


/* files_search and artist_albums initialized in gui_library_init */
    gui_text_input_init();
    stream_media_screen = build_stream_media_screen();


    /* Artists/Albums, unlike the rest of this file's ~25 build_subsonic_
     * list_screen() screens, can genuinely scale with library size --
     * getArtists.view has no size cap and getAlbumList2.view's own "every
     * album" browse returns up to 500 (see subsonic_client.c) -- so these
     * two use the same virtualized compact_list build_all_songs_screen()/
     * build_artists_screen()/etc. already do, not the plain flex list every
     * other (inherently small/bounded) settings/browse screen in this file
     * shares. Built empty (item_count 0); populated later via compact_list_
     * set_items() once a real fetch actually completes (poll_subsonic_
     * connect()/poll_subsonic_browse()), same lazy-population shape as the
     * local library screens. enable_now_playing is false for both -- a
     * Subsonic artist/album row has no local playlist_index to highlight
     * against. */
    gui_subsonic_init();

    /* Artists/Albums, unlike the rest of this file's ~25 build_subsonic_
     * list_screen() screens, can genuinely scale with library size --
     * getArtists.view has no size cap and getAlbumList2.view's own "every
     * album" browse returns up to 500 (see subsonic_client.c) -- so these
     * two use the same virtualized compact_list build_all_songs_screen()/
     * build_artists_screen()/etc. already do, not the plain flex list every
     * other (inherently small/bounded) settings/browse screen in this file
     * shares. Built empty (item_count 0); populated later via compact_list_
     * set_items() once a real fetch actually completes (poll_subsonic_
     * connect()/poll_subsonic_browse()), same lazy-population shape as the
     * local library screens. enable_now_playing is false for both -- a
     * Subsonic artist/album row has no local playlist_index to highlight
     * against. */

    /* Top-of-screen Download buttons -- see subsonic_download_artist_btn_cb()/
     * subsonic_download_songs_btn_cb() for what each actually downloads.
     * Added directly onto the already-built screens (same pattern as
     * build_wifi_screen()'s Rescan button) rather than threading a new
     * parameter through build_subsonic_list_screen() itself, which ~20
     * other, unrelated screens also share. Hidden by default -- shown only
     * when the screen's own click handler determines it's actually
     * applicable (an Artist page for the albums one; always for the songs
     * one, whether it's an album or a playlist). */
    /* Real-device bug report: the "Download" text button started the
     * download immediately with no confirmation (see subsonic_download_
     * songs_btn_cb()'s/subsonic_download_artist_btn_cb()'s own comment for
     * that half of the fix) and, separately, was asked to become an icon
     * instead of a text label -- a plain downward-arrow-into-a-tray glyph
     * (stream_media/download.png, a new asset this app adds via the
     * THEME_OVERRIDE_ROOT mechanism, same as stream_media/subsonic.png's
     * own precedent -- there's no stock icon for a Subsonic-only feature)
     * rather than reusing an LVGL built-in symbol font glyph, which would
     * have clashed with this app's own consistently hand-drawn icon set. */

    /* Same live search as Artists/Albums/Album Artist/All Songs (see the
     * search_binding_t infra above), extended to Subsonic's own Artists and
     * Albums lists -- now virtualized compact_lists too (see their own
     * build_compact_list_screen() comment above), so search_apply_filter()
     * repopulates them via compact_list_set_items() same as every other
     * binding, no special-casing needed here anymore. No A-Z index -- same
     * reasoning as Files (search-only), and unlike Files this isn't even
     * alphabetically sorted (server order). Registered once here only --
     * unlike the local-library bindings, these screens are never rebuilt (no
     * equivalent of a library rescan), so there's no second registration
     * site to mirror this at. */
    gui_library_init();
    gui_network_init();
    gui_settings_init();
    gui_books_init();
    build_power_off_countdown_popup();
    gui_queue_init();
    gui_plugins_init();
    gui_shell_init(screen_width, screen_height);
    gui_navigation_init();
#ifndef HOST_BUILD
    boot_checkpoint("all screens built");
#endif



#ifndef HOST_BUILD
    /* Holds the boot splash (gui_show_boot_splash(), called from main.c
     * before any of this function's own work) on screen for at least
     * BOOT_SPLASH_MIN_DISPLAY_MS of real boot time, piggybacking on
     * whatever this function's own setup (library load, screen building)
     * already consumed -- only adds *additional* wait if that finished
     * faster, so this isn't always a flat tax on boot time. This also
     * delays start_refresh_bt_icon() (called above) by the same margin.
     *
     * Deliberately NOT waiting here for bt_init to finish -- see
     * BOOT_SPLASH_MIN_DISPLAY_MS's own comment, further up, for what was
     * tried and reverted, and why Bluetooth is left alone (off, and
     * staying off) rather than this app touching it at all during boot. */
    while (boot_splash_start_tick != 0 &&
           lv_tick_get() - boot_splash_start_tick < BOOT_SPLASH_MIN_DISPLAY_MS) {
        uint32_t wait_ms = lv_timer_handler();
        if (wait_ms > 50) wait_ms = 50;
        usleep(wait_ms * 1000);
    }
    boot_checkpoint("boot splash settle wait done");

    /* Reveals the status bar/popups/quick drawer -- see gui_show_boot_
     * splash()'s own comment for why these were hidden in the first place
     * (they all live on lv_layer_top(), which paints above any screen
     * unconditionally, splash included). */
    lv_obj_remove_flag(lv_layer_top(), LV_OBJ_FLAG_HIDDEN);
#endif

    /* gui_shell_get_home_screen() is the permanent root of the nav stack -- nav_pop() never
     * goes past it. Load it first so there's always something valid on
     * screen even before any auto-resume logic below runs. */


    /* The screen-timeout clock belongs to the interactive UI, not startup.
     * update_timer_cb used to be created before the splash settle loop, so
     * LVGL counted library/screen construction and the visible splash as
     * user inactivity; with a short timeout it could switch the panel off
     * immediately after (or even during) the splash. Establish the activity
     * baseline at the exact splash -> Home transition, then start runtime
     * polling. The fast gesture timers likewise have no work while the
     * splash is the only visible screen -- and self-pause again (see their
     * own handle comments) the moment the very first tick after this finds
     * nothing pressed, rather than running at ~60fps for the rest of the
     * app's life regardless of whether anyone's touching the screen: with
     * no other timer registered below LV_DEF_REFR_PERIOD, these two used to
     * be the sole reason main()'s own usleep(lv_timer_handler()) could never
     * sleep longer than one frame, forever, including idle/screen-off
     * playback. resume_fast_gesture_timers_cb() wakes both again the
     * instant a new press begins, on whichever indev is the real
     * touchscreen (find_pointer_indev() is safe to call here -- the target
     * build's touch indev is already registered by main.c well before
     * gui_init() runs). */
    gui_reset_interactive_timeout_baseline();
    lv_timer_create(update_timer_cb, 500, NULL);
    lv_indev_t * gesture_indev = find_pointer_indev();
    gui_shell_install_indev_hooks(gesture_indev);
#ifndef HOST_BUILD
    boot_checkpoint("lv_screen_load(gui_shell_get_home_screen()) done");
#endif

    /* Real-device incident (2026-08-08): auto-resuming into a Subsonic
     * track (cached locally at SUBSONIC_STREAM_CACHE_DIR/stream.<suffix>,
     * see that macro's own comment further down -- last_track points at
     * that cache file, not a live stream URL) crashed and rebooted the
     * device on a cold boot with no Wi-Fi connected yet. Root cause was
     * never fully pinned down -- candidates included the cache file being
     * a leftover partial/corrupt download from a session killed mid-stream,
     * or something else on this path unexpectedly touching network state
     * -- so this whole path was disabled outright rather than resuming
     * into a file of unknown safety on every single cold boot.
     *
     * Car Mode's rework back to poweroff+auto-resume (see its own comment
     * above, in the update_timer_cb Car Mode block) needs this path
     * working again, so it's re-enabled here with the guard that was
     * actually missing before: last_track is skipped if it's inside
     * SUBSONIC_STREAM_CACHE_DIR rather than a real library file. This can't
     * rule out every possible cause of the original crash, but it removes
     * the one concretely different thing about a Subsonic-cache resume
     * versus a normal library-file resume -- this same path already worked
     * fine for ordinary local files before that incident -- so it's the
     * correct first fix to try rather than leaving auto-resume disabled
     * for everyone over one still-unexplained edge case.
     *
     * This block was, for a long time, gated on car_mode_enabled only, NOT
     * the separate general "resume on every launch" setting -- that one had
     * no UI toggle at all since this incident (its Settings row was removed
     * entirely, not just left inert -- a live-looking toggle for a feature
     * that silently did nothing read as a regression) and defaulted to true,
     * so wiring it back in here unconditionally would have silently
     * auto-resumed on EVERY cold boot for every user, not just those who
     * deliberately opted into Car Mode's specific docking routine.
     *
     * Now reintroduced as Settings -> Playback -> Resume Last Track
     * (player_settings_t.resume_mode), opt-in and defaulting to off (0),
     * handled in the separate `else if` below so it can share this same
     * Subsonic-cache guard without duplicating it, while staying fully
     * independent of Car Mode's own always-on, headphone-gated resume. */
    if (current_settings.car_mode_enabled && current_settings.last_track[0] != '\0' &&
        strncmp(current_settings.last_track, SUBSONIC_STREAM_CACHE_DIR, strlen(SUBSONIC_STREAM_CACHE_DIR)) != 0) {
#ifndef HOST_BUILD
        /* Real-device incident: a device Car Mode shut down (unplugged
         * while playing) that was later found with its wired headphones
         * ALSO disconnected, then manually powered on with no external
         * power connected at all, boot-looped on every attempt -- only
         * recoverable by pulling the SD card (so this whole block's
         * file_browser_build_playlist_for_path() call below fails to find
         * last_track and the resume is skipped entirely) and then
         * disabling Car Mode by hand. This is the same general class of
         * problem as the 2026-08-08 Subsonic-cache incident above (an
         * auto-resume at boot crashing/hanging for a reason never fully
         * pinned down) -- rather than resume blind into a state already
         * twice confirmed dangerous, refuse outright whenever there's no
         * headphone jack connected (this app's own docking routine's own
         * assumption: Car Mode expects to resume into a car's own wired
         * aux/dock connection, not open air) and disable Car Mode so a
         * user who hits this doesn't land back in the exact same trap on
         * their very next boot too, before they've had any chance to
         * investigate. headphone_is_connected() is a cheap synchronous
         * sysfs read (no D-Bus/Bluetooth involved) -- safe to call this
         * early in boot, unlike a Bluetooth connectivity check (see the
         * "no startup-time Bluetooth cleanup" comment in main.c). */
        if (!headphone_is_connected()) {
            current_settings.car_mode_enabled = false;
            settings_save(&current_settings);
            show_info_toast("Car Mode disabled: no headphone detected at boot");
        } else
#endif
        {
            char ** resume_playlist;
            int resume_count, resume_index;
            if (build_saved_resume_playlist(&resume_playlist, &resume_count, &resume_index)) {
                if (install_saved_resume_playlist(resume_playlist, resume_count)) {
                    /* play_track_at_from() itself nav_push()es gui_player_get_screen() on top
                     * of the seeded root, so a back-swipe from the resumed player
                     * correctly lands back on the home screen. */
                    play_track_at_from(resume_index, current_settings.last_position);
                }
            }
        }
    } else if (current_settings.resume_mode != 0 && current_settings.last_track[0] != '\0' &&
               strncmp(current_settings.last_track, SUBSONIC_STREAM_CACHE_DIR, strlen(SUBSONIC_STREAM_CACHE_DIR)) != 0) {
        /* General-purpose "Resume Last Track" (Settings -> Playback),
         * independent of Car Mode's own dedicated docking-routine resume
         * above -- reuses the exact same Subsonic-cache guard (the one
         * concretely identified cause of the 2026-08-08 crash-reboot-loop
         * incident) but deliberately has none of Car Mode's own
         * headphone-presence requirement, which is specific to its docking
         * assumption and doesn't apply to a normal user just picking this
         * setting up in Settings. */
        char ** resume_playlist;
        int resume_count, resume_index;
        if (build_saved_resume_playlist(&resume_playlist, &resume_count, &resume_index)) {
            if (install_saved_resume_playlist(resume_playlist, resume_count)) {
                if (current_settings.resume_mode == 2) {
                    /* Do not open ALSA at all until the user presses Play.  On
                     * a headphone-less boot, start-then-pause could lose the
                     * race to an output-open failure and consume this queue. */
                    prepare_deferred_resume(resume_index, current_settings.last_position);
                } else play_track_at_from(resume_index, current_settings.last_position);
            }
        }
    }
#ifndef HOST_BUILD
    boot_checkpoint("gui_init returning");
#endif

    /* Deliberately the very last thing gui_init() does -- see
     * fallback_font_schedule_deferred_load()'s own doc comment for why this
     * can't just load the font directly here or anywhere earlier in this
     * function (the auto-resume path just above is exactly the kind of
     * pre-first-frame call site that hung boot on the previous attempt at
     * this). */
    fallback_font_schedule_deferred_load();
}


void gui_deinit(void) {
    gui_library_cancel_background_work();
    gui_subsonic_cancel_background_work();
    gui_network_cancel_background_work();
    gui_lyrics_cancel_background_work();
    gui_player_cancel_background_work();
    gui_shell_cancel_background_work();
}
