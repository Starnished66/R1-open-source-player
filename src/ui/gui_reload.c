/* See gui_reload.h for what this does and does not touch.
 *
 * gui_soft_reload()'s sequence, and why the order matters:
 *
 *  1. gui_shell_reset_drag_state() first, before anything else -- cancels
 *     any interactive player-swipe/quick-drawer drag still in flight (its
 *     own slide_transition_cancel(&player_swipe_ctx) call touches from_scr/
 *     to_scr/img_from/img_to, all about to be deleted below; run any later
 *     and that becomes a use-after-free). A COMMITTED (non-interactive)
 *     slide animation has no such direct cancel path -- gui_reload_request()
 *     refuses to even schedule this function while one is still animating
 *     (see its own comment) rather than trying to tear down mid-transition.
 *  2. gui_library_prepare_for_ui_reload() joins artwork/search jobs that
 *     carry pointers into the screens about to be deleted. Database scans
 *     and SD formatting defer the reload before this sequence begins.
 *  3. reset_swipe_dead_zones() -- every native slider registers itself here
 *     once at startup and is never expected to unregister (see its own
 *     comment); without clearing it first, the sliders each module's
 *     _teardown() is about to delete would leave point_in_swipe_dead_zone()
 *     dereferencing freed pointers on the next swipe check.
 *  4. gui_navigation_teardown() -- resets the nav stack and destroys its own
 *     snapshot/transition-cache buffers. Does NOT delete any screen itself
 *     (each screen is owned and freed by its own module, right below).
 *  5. Every module's own _teardown(): lv_obj_del()s the screens/popups it
 *     owns, INCLUDING any recurring lv_timer_t its _init() unconditionally
 *     (re)creates (volume_popup_hide_timer, lyrics_timer, text_entry_
 *     multitap_timer) -- left running, the old (leaked) timer's own
 *     callback keeps firing forever and mutates whichever NEW timer the
 *     global variable it reads now points to, instead of the dead one that
 *     actually fired. Order among these modules doesn't matter -- none of
 *     them reference another module's screen directly (only via nav_stack/
 *     snapshot state, already cleared in step 4).
 *  6. lv_image_cache_drop(NULL) -- drops LVGL's entire image+header cache in
 *     one call (confirmed via lvgl/src/misc/cache/lv_image_cache.c), so
 *     every icon re-decodes from disk against current theme_overrides/
 *     content once screens are rebuilt below.
 *  7. plugin_manager_deinit() -- drains async HTTP/interval-timer/pending-
 *     text-input work still referencing a plugin's lua_State, then closes
 *     every plugin's L and zeroes every list-item/tile/callback/event-
 *     subscriber registry. Must run after every screen plugins could have
 *     attached a callback to is already gone (a plugin's on_click callback
 *     firing against a half-torn-down screen would be worse than firing
 *     against none at all).
 *  8. gui_theme_reload_styles() -- resets style globals to this app's own
 *     defaults BEFORE plugins reapply on top in step 9, mirroring gui_
 *     init()'s own boot order MINUS fallback_font_init_early(): that call
 *     is boot-only (always rebuilds every app_font_* slot with fallback
 *     faces excluded, relying on a separate one-time background load to
 *     add CJK/Korean/Thai support afterward) -- repeating it on a reload
 *     would silently drop that already-loaded fallback chain and leak the
 *     fallback faces it's replacing, for no benefit a theme/icon reload
 *     needs. Its own guard (see gui_theme.c) resets each lv_style_t before
 *     re-lv_style_init()-ing it, rather than leaking the previous
 *     properties allocation the way a bare second lv_style_init() call
 *     would (same fix applied to screen_builders_init_list_row_style()'s
 *     own 9 styles).
 *  9. plugin_manager_init() -- BEFORE any screen is rebuilt, same as real
 *     boot (gui_init() calls it before gui_player_init() etc. too): Books/
 *     Settings/Stream Media read the list-item/tile registries plugin_
 *     manager_init() populates AT BUILD TIME to append plugin-contributed
 *     rows, so calling it after rebuilding those screens (as an earlier
 *     version of this function mistakenly did) would build them with zero
 *     plugin rows every time.
 * 10. Rebuild every screen, same order gui_init() builds them in.
 * 11. gui_navigation_init() -- re-registers the snapshot screens and
 *     restores nav_stack/nav_depth to Home, loading it. No separate
 *     "restore navigation" step is needed; this already does it.
 *
 * Never touches: audio_init(), settings_load(), volume/backlight/LED/
 * charge-limiter/timezone reapply, hostname_apply(), usb_mode detection,
 * the network_modes_changed forcing block, library_load_from_cache_only(),
 * playlist_files_migrate_to_relative(), the Subsonic-cache auto-resume/Car
 * Mode last-track logic (documented crash history -- boot-only), update_
 * timer_cb (created once at real boot, already running continuously -- a
 * second lv_timer_create() here would duplicate it), or start_refresh_bt_
 * icon()/start_bt_dac_startup_reapply_if_needed() (Bluetooth-adjacent
 * startup side effects -- gui_shell_build_screens() deliberately excludes
 * both, unlike gui_shell_init()). */

#include "gui_reload.h"

#include "lvgl/lvgl.h"

#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

#include "gui.h"
#include "gui_theme.h"
#include "gui_navigation.h"
#include "gui_shell.h"
#include "gui_player.h"
#include "gui_lyrics.h"
#include "gui_text_input.h"
#include "gui_subsonic.h"
#include "gui_library.h"
#include "gui_network.h"
#include "gui_settings.h"
#include "gui_books.h"
#include "gui_queue.h"
#include "gui_plugins.h"
#include "plugin_manager.h"
#include "gui_plugin_manage.h"
#include "gui_lock_screen.h"

/* Temporary investigation instrumentation for the "applying Wavy crashed
 * the device" report -- a real, reproducible SIGSEGV inside musl's free()
 * (dmesg: "invalid read access from 00000008", epc in get_meta, ra in
 * __libc_free), meaning something is passing a corrupted pointer to
 * free() somewhere in this sequence, not a plain NULL dereference in
 * application code. Append-only, fsync per line, so a crash mid-step
 * doesn't take the last few log lines down with it -- same reasoning the
 * bootloader's own boot_diag_log() uses. Brackets every single step so
 * the exact step (not just "somewhere in gui_soft_reload()") is known
 * from the log's last line once this reproduces again. */
#define RELOAD_DIAG_PATH "/data/mnt/sd_0/reload_diag.log"
static void reload_diag(const char * step) {
    int fd = open(RELOAD_DIAG_PATH, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) return;
    char line[160];
    int len = snprintf(line, sizeof(line), "[pid=%ld] %s\n", (long) getpid(), step);
    if (len > 0) {
        if (len >= (int) sizeof(line)) len = (int) sizeof(line) - 1;
        write(fd, line, (size_t) len);
        fsync(fd);
    }
    close(fd);
}

static int32_t reload_display_width(void) {
    lv_display_t * display = lv_display_get_default();
    int32_t width = display ? lv_display_get_horizontal_resolution(display) : BOARD_SCREEN_WIDTH;
    return width > 0 ? width : BOARD_SCREEN_WIDTH;
}

static int32_t reload_display_height(void) {
    lv_display_t * display = lv_display_get_default();
    int32_t height = display ? lv_display_get_vertical_resolution(display) : BOARD_SCREEN_HEIGHT;
    return height > 0 ? height : BOARD_SCREEN_HEIGHT;
}

void gui_soft_reload(void) {
    reload_diag("gui_soft_reload: begin");
    int32_t screen_width = reload_display_width();
    int32_t screen_height = reload_display_height();

    reload_diag("gui_shell_reset_drag_state: before");
    gui_shell_reset_drag_state();
    reload_diag("gui_library_prepare_for_ui_reload: before");
    gui_library_prepare_for_ui_reload();
    reload_diag("reset_swipe_dead_zones: before");
    reset_swipe_dead_zones();
    reload_diag("gui_navigation_teardown: before");
    gui_navigation_teardown();

    reload_diag("gui_player_teardown: before");
    gui_player_teardown();
    reload_diag("gui_lyrics_teardown: before");
    gui_lyrics_teardown();
    reload_diag("gui_text_input_teardown: before");
    gui_text_input_teardown();
    reload_diag("gui_subsonic_teardown: before");
    gui_subsonic_teardown();
    reload_diag("gui_library_teardown: before");
    gui_library_teardown();
    reload_diag("gui_network_teardown: before");
    gui_network_teardown();
    reload_diag("gui_settings_teardown: before");
    gui_settings_teardown();
    reload_diag("gui_plugin_manage_teardown: before");
    gui_plugin_manage_teardown();
    reload_diag("gui_lock_screen_teardown: before");
    gui_lock_screen_teardown();
    reload_diag("gui_books_teardown: before");
    gui_books_teardown();
    reload_diag("gui_queue_teardown: before");
    gui_queue_teardown();
    reload_diag("gui_plugins_teardown: before");
    gui_plugins_teardown();
    reload_diag("gui_stream_media_teardown: before");
    gui_stream_media_teardown();
    reload_diag("gui_shell_teardown: before");
    gui_shell_teardown();
    gui_player_release_shared_assets();

    reload_diag("lv_image_cache_drop: before");
    lv_image_cache_drop(NULL);

    reload_diag("plugin_manager_deinit: before");
    plugin_manager_deinit();
    reload_diag("plugin_manager_deinit: after");

    reload_diag("gui_theme_reload_styles: before");
    gui_theme_reload_styles();

    reload_diag("plugin_manager_init: before");
    plugin_manager_init();
    reload_diag("plugin_manager_init: after");

    reload_diag("gui_player_init: before");
    gui_player_init((uint32_t) screen_width, (uint32_t) screen_height);
    reload_diag("gui_lyrics_init: before");
    gui_lyrics_init();
    reload_diag("gui_text_input_init: before");
    gui_text_input_init();
    reload_diag("gui_stream_media_rebuild: before");
    gui_stream_media_rebuild();
    reload_diag("gui_subsonic_init: before");
    gui_subsonic_init();
    reload_diag("gui_library_init: before");
    gui_library_init();
    reload_diag("gui_network_init: before");
    gui_network_init();
    reload_diag("gui_settings_init: before");
    gui_settings_init();
    reload_diag("gui_plugin_manage_init: before");
    gui_plugin_manage_init();
    reload_diag("gui_lock_screen_init: before");
    gui_lock_screen_init();
    reload_diag("gui_books_init: before");
    gui_books_init();
    reload_diag("build_power_off_countdown_popup: before");
    build_power_off_countdown_popup();
    reload_diag("gui_queue_init: before");
    gui_queue_init();
    reload_diag("gui_plugins_init: before");
    gui_plugins_init();
    reload_diag("gui_shell_build_screens: before");
    gui_shell_build_screens((uint32_t) screen_width, (uint32_t) screen_height);

    reload_diag("gui_navigation_init: before");
    gui_navigation_init();
    reload_diag("gui_soft_reload: end");
}

/* gui_reload_request()'s scheduling state -- see its own comment. */
static bool reload_scheduled = false;
static bool reload_in_progress = false;
static uint32_t last_reload_tick = 0;

/* How often the pending-reload timer rechecks gui_navigation_transition_in_
 * progress() while deferring -- NAV_ANIM_TIME_MS (gui_navigation.c) is on
 * the order of a couple hundred ms, so this converges in a handful of
 * ticks without meaningfully delaying the reload a user is waiting on. */
#define RELOAD_RETRY_PERIOD_MS 20
#define RELOAD_BUSY_RETRY_MS 250

/* Minimum gap enforced between the END of one reload and the START of the
 * next -- NOT what stops a plugin that unconditionally calls plugin.
 * reload_ui() from its own top-level script code from looping (that's
 * reload_in_progress below: plugin_manager_init(), step 9 above, re-runs
 * top-level code as part of the SAME reload that's still running, and
 * reload_in_progress drops that nested request outright rather than
 * queuing it, so it costs one wasted extra reload the first time such a
 * plugin loads, never a loop). This cooldown instead rate-limits separate,
 * non-nested back-to-back requests -- e.g. a plugin.set_interval() timer
 * callback that (mistakenly) calls this on every tick -- which reload_in_
 * progress alone doesn't touch, since each of those firings happens after
 * the previous reload has already fully finished. A well-behaved plugin
 * only calls this in response to an actual user action (see PLUGINS.md's
 * own example), which no real interaction repeats faster than this anyway
 * -- refusing outright (not queuing) is the correct failure mode for a
 * plugin bug here, same reasoning as plugin_call()'s own per-call time
 * budget refusing a runaway `while true do end` instead of letting it
 * degrade some other way. */
#define RELOAD_COOLDOWN_MS 250

static void reload_trigger_cb(lv_timer_t * timer) {
    if (gui_navigation_transition_in_progress()) return;
    if (gui_library_navigation_blocked()) {
        /* A database scan can last minutes; polling it at the transition
         * cadence would waste UI-thread time throughout the scan. */
        lv_timer_set_period(timer, RELOAD_BUSY_RETRY_MS);
        return;
    }
    lv_timer_del(timer);
    reload_scheduled = false;
    /* Set BEFORE gui_soft_reload(), not after -- plugin_manager_init()
     * inside it re-runs every plugin's top-level code, and top-level code
     * that unconditionally calls plugin.reload_ui() would otherwise call
     * gui_reload_request() again while reload_scheduled is already false
     * (cleared above) and last_reload_tick is still stale from BEFORE this
     * reload started -- both guards below would wrongly let a second
     * reload through, scheduled to fire immediately after this one
     * finishes. reload_in_progress is the one guard that's actually true
     * for the entire duration of the call it needs to block requests
     * during. */
    reload_in_progress = true;
    gui_soft_reload();
    reload_in_progress = false;
    last_reload_tick = lv_tick_get();
}

void gui_reload_request(void) {
    if (reload_in_progress || reload_scheduled) return; /* coalesce -- one pending/running reload covers any others requested meanwhile */
    if (last_reload_tick != 0 && lv_tick_elaps(last_reload_tick) < RELOAD_COOLDOWN_MS) {
        LV_LOG_WARN("gui_reload_request: ignored -- another reload finished under %dms ago "
                     "(a plugin calling plugin.reload_ui() unconditionally from its own top-level "
                     "code would look exactly like this, since that code re-runs on every reload)",
                     RELOAD_COOLDOWN_MS);
        return;
    }
    reload_scheduled = true;
    lv_timer_create(reload_trigger_cb, RELOAD_RETRY_PERIOD_MS, NULL);
}

static bool theme_refresh_scheduled = false;

static void theme_refresh_cb(lv_timer_t * timer) {
    if (reload_in_progress || reload_scheduled) {
        lv_timer_del(timer);
        theme_refresh_scheduled = false;
        return;
    }
    if (gui_navigation_transition_in_progress()) return;
    lv_timer_del(timer);
    theme_refresh_scheduled = false;
    lv_image_cache_drop(NULL);
    gui_player_refresh_static_assets();
    gui_shell_refresh_static_assets();
    /* Drop old bases before replacement. Each replacement registers one
     * fresh base; invalidating afterwards would immediately destroy those
     * three new full-screen buffers and allocate them a second time in the
     * async rebuild, creating avoidable peak memory pressure during rapid
     * theme switching. */
    gui_navigation_invalidate_theme_snapshots();
    gui_shell_refresh_home();
    gui_library_refresh_music_screen();
    gui_stream_media_refresh();
    gui_network_refresh_wireless_screen();
    quick_drawer_mark_snapshot_dirty();
    player_transition_mark_dirty();
    lv_obj_invalidate(lv_screen_active());
    lv_obj_invalidate(lv_layer_top());
}

void gui_theme_refresh_request(void) {
    if (reload_in_progress || reload_scheduled || theme_refresh_scheduled) return;
    theme_refresh_scheduled = lv_timer_create(theme_refresh_cb, RELOAD_RETRY_PERIOD_MS, NULL) != NULL;
}
