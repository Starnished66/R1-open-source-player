#include "gui_navigation.h"
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
#include "gui_shell.h"
#include "gui_books.h"
#include "screen_builders.h"
#include "metadata.h"
#include "audio.h"
#include "settings.h"
#include "assets.h"
#include "device_config.h"
#include "transition_compositor.h"

static lv_obj_t * nav_stack[NAV_STACK_MAX] = { NULL };
static int nav_depth = 0;
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern player_settings_t current_settings;

extern lv_obj_t * gui_shell_get_home_screen();

extern lv_obj_t * gui_library_get_music_screen();
extern lv_obj_t * stream_media_screen;
extern lv_obj_t * gui_network_get_wireless_screen();
extern lv_obj_t * gui_shell_get_dac_home_screen();
extern lv_obj_t * gui_player_get_screen();
extern lv_obj_t * gui_settings_get_about_screen();
extern lv_obj_t * gui_settings_get_screen();
extern lv_obj_t * gui_settings_get_system_screen();

extern void sync_player_topbar_visibility(lv_obj_t * screen);
extern void open_quick_drawer(void);
extern void close_quick_drawer(void);
extern bool point_in_swipe_dead_zone(lv_point_t p);
extern bool active_press_is_over_drag_adjust_widget(void);


/* Forward navigation slides the current screen out to the left as the new
 * one slides in from the right (matching a left-swipe gesture); back
 * navigation is the mirror of that. */
#define NAV_ANIM_TIME_MS 165 /* 220ms * 0.75, per real-hardware feedback */

/* lv_screen_load_anim() animates the real screen OBJECTS, which on this
 * GPU-less MIPS target meant fully re-rendering both the outgoing and
 * incoming screen's whole widget tree on every single animation frame --
 * confirmed via real-hardware testing to be the actual cause of choppy,
 * tearing-prone navigation (not just a refresh-rate/vsync issue). Instead,
 * take ONE snapshot of each screen as a static bitmap up front, and slide
 * those two bitmaps on a top-layer overlay -- "N frames x 2 full re-renders"
 * becomes "2 renders + N cheap bitmap blits". The real target screen is
 * only actually made active once the slide finishes. */

/* Forward declarations -- real (first) tentative definitions further down
 * this file, alongside the code that actually builds/positions each of
 * these. A plain `static T * x;` with no initializer is a tentative
 * definition in C; declaring the same one twice in one translation unit is
 * fine and merges into a single variable, so this is just an ordering fix,
 * not a second/shadow copy -- build_flattened_transition_frame() below
 * needs all three, and is used by register_static_snapshot() just below
 * it, which is itself defined earlier in the file than any of the real
 * declarations. */

/* Alpha-blends one row of an ARGB8888 source over an RGB565 destination
 * row, `count` pixels wide -- shared by build_flattened_transition_frame()
 * below for compositing a persistent lv_layer_top() band (status bar,
 * home indicator) onto a screen-only RGB565 base. LVGL's ARGB8888 memory
 * layout is B,G,R,A per pixel (see lv_color32_t in lv_color.h), not the
 * A,R,G,B its name suggests. */
static void blend_argb8888_row_over_rgb565(uint16_t * dst, const uint8_t * src_argb, int32_t count) {
    for (int32_t x = 0; x < count; x++) {
        uint8_t b = src_argb[x * 4 + 0];
        uint8_t g = src_argb[x * 4 + 1];
        uint8_t r = src_argb[x * 4 + 2];
        uint8_t a = src_argb[x * 4 + 3];
        if (a == 0) continue; /* fully transparent -- leave the destination pixel untouched */
        if (a == 255) {
            dst[x] = (uint16_t) (((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
            continue;
        }
        uint16_t old = dst[x];
        uint8_t old_r = (uint8_t) (((old >> 11) & 0x1F) << 3);
        uint8_t old_g = (uint8_t) (((old >> 5) & 0x3F) << 2);
        uint8_t old_b = (uint8_t) ((old & 0x1F) << 3);
        uint8_t nr = (uint8_t) (((uint32_t) r * a + (uint32_t) old_r * (255 - a)) / 255);
        uint8_t ng = (uint8_t) (((uint32_t) g * a + (uint32_t) old_g * (255 - a)) / 255);
        uint8_t nb = (uint8_t) (((uint32_t) b * a + (uint32_t) old_b * (255 - a)) / 255);
        dst[x] = (uint16_t) (((nr >> 3) << 11) | ((ng >> 2) << 5) | (nb >> 3));
    }
}

/* Snapshots `overlay_obj` (an lv_layer_top() child -- status_bar_band or
 * home_indicator_band, never a whole layer, see build_flattened_transition_
 * frame()'s own comment on why) as ARGB8888 and alpha-blends it onto
 * `base` at overlay_obj's own real on-screen coordinates. No-op (leaves
 * base untouched) if the snapshot itself fails -- caller still gets a
 * usable, if incomplete, frame rather than none at all. */
static void blend_overlay_onto_base(lv_draw_buf_t * base, lv_obj_t * overlay_obj) {
    lv_area_t coords;
    lv_obj_get_coords(overlay_obj, &coords);
    int32_t ow = lv_area_get_width(&coords);
    int32_t oh = lv_area_get_height(&coords);
    if (ow <= 0 || oh <= 0) return;

    lv_draw_buf_t * overlay = lv_snapshot_take(overlay_obj, LV_COLOR_FORMAT_ARGB8888);
    if (!overlay) return;

    int32_t base_w = (int32_t) base->header.w;
    int32_t base_h = (int32_t) base->header.h;
    for (int32_t y = 0; y < oh; y++) {
        int32_t dy = coords.y1 + y;
        if (dy < 0 || dy >= base_h) continue;
        int32_t dx0 = coords.x1;
        int32_t count = ow;
        int32_t sx0 = 0;
        if (dx0 < 0) { sx0 = -dx0; count += dx0; dx0 = 0; }
        if (dx0 + count > base_w) count = base_w - dx0;
        if (count <= 0) continue;
        const uint8_t * srow = (const uint8_t *) lv_draw_buf_goto_xy(overlay, (uint32_t) sx0, (uint32_t) y);
        uint16_t * drow = (uint16_t *) lv_draw_buf_goto_xy(base, (uint32_t) dx0, (uint32_t) dy);
        blend_argb8888_row_over_rgb565(drow, srow, count);
    }
    lv_draw_buf_destroy(overlay);
}

/* Screen-only RGB565 base for target_screen -- player_dismiss_btn's hidden
 * flag (Player's own standalone back arrow, part of gui_player_get_screen()'s own
 * subtree, not a separate layer) is temporarily forced to its TARGET-state
 * value (based on current_settings.hide_player_topbar, never on whichever
 * live screen happens to be active right now) before snapshotting, then
 * restored immediately after -- safe because nothing here ever yields to a
 * real lv_timer_handler()/lv_refr_now() refresh pass in between, so the
 * live UI never actually renders the temporary state to the real screen.
 * Deliberately does NOT include the persistent status bar / home-indicator
 * content -- see blend_persistent_bars() below for why that's kept
 * separate rather than baked in here. Caller owns the returned buffer;
 * returns NULL on snapshot failure. */
static lv_draw_buf_t * snapshot_screen_base(lv_obj_t * target_screen) {
    if (!target_screen) return NULL;

    lv_obj_t * dismiss_btn = gui_player_get_dismiss_btn();
    bool dismiss_target_hidden = current_settings.hide_player_topbar && target_screen == gui_player_get_screen();
    bool dismiss_touched = (dismiss_btn != NULL && target_screen == gui_player_get_screen());
    bool dismiss_was_hidden = false;
    if (dismiss_touched) {
        dismiss_was_hidden = lv_obj_has_flag(dismiss_btn, LV_OBJ_FLAG_HIDDEN);
        if (dismiss_target_hidden) lv_obj_add_flag(dismiss_btn, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(dismiss_btn, LV_OBJ_FLAG_HIDDEN);
    }

    lv_draw_buf_t * base = lv_snapshot_take(target_screen, LV_COLOR_FORMAT_RGB565);

    if (dismiss_touched) {
        if (dismiss_was_hidden) lv_obj_add_flag(dismiss_btn, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(dismiss_btn, LV_OBJ_FLAG_HIDDEN);
    }
    return base;
}

/* Blends whichever persistent lv_layer_top() bands would actually be
 * visible on target_screen (status_bar_band per Settings > Display > "Hide
 * Player/Lyrics Top Bar", home_indicator_band per Settings > Display >
 * "Swipe Up for Home") onto an existing owned RGB565 base, mutating it in
 * place. TRANSITION_PERFORMANCE_PLAN.md Phase 3 target architecture,
 * section B -- kept as a separate, cheap, always-fresh step from the base
 * snapshot itself (snapshot_screen_base() above) specifically so a CACHED
 * base (the static-screen cache or the Phase 2 player cache, both below)
 * never bakes in stale clock/battery/wifi/home-indicator content: real-
 * device review finding -- baking bars into a snapshot taken once (the
 * static cache, built at startup) or infrequently (the player cache, only
 * rebuilt on track/art/play-state changes, not every second) meant a
 * transition could show a visibly wrong/outdated clock or a home-indicator
 * bar whose visibility had since been toggled off. Blending fresh at
 * transition time instead means the persistent-bar content is never more
 * than a few lines of code away from what a real, current LVGL render of
 * lv_layer_top() would show. Deliberately does NOT snapshot lv_layer_top()
 * as a whole: that would also capture transient overlays (the volume
 * popup, quick-drawer motion image, a DAC overlay) that may happen to be
 * present at the wrong moment -- only these two named, permanent bands are
 * ever composited in. status_bar_band's hidden flag is temporarily forced
 * to its target-state value (same reasoning/safety as snapshot_screen_
 * base()'s own comment on player_dismiss_btn); home_indicator_band's
 * current live flag already IS its correct target-state value (its
 * visibility is unrelated to which screen is active), so it needs no such
 * forcing. */
static void blend_persistent_bars(lv_draw_buf_t * base, lv_obj_t * target_screen) {
    bool topbar_target_hidden = current_settings.hide_player_topbar &&
                                 (target_screen == gui_player_get_screen() || target_screen == gui_lyrics_get_screen());
    lv_obj_t * sb = gui_shell_get_status_bar_band();
    lv_obj_t * hb = gui_shell_get_home_indicator_band();
    if (sb) {
        bool status_was_hidden = lv_obj_has_flag(sb, LV_OBJ_FLAG_HIDDEN);
        if (!topbar_target_hidden) {
            if (status_was_hidden) lv_obj_remove_flag(sb, LV_OBJ_FLAG_HIDDEN);
            blend_overlay_onto_base(base, sb);
        }
        if (status_was_hidden) lv_obj_add_flag(sb, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(sb, LV_OBJ_FLAG_HIDDEN);
    }
    if (hb && !lv_obj_has_flag(hb, LV_OBJ_FLAG_HIDDEN)) {
        blend_overlay_onto_base(base, hb);
    }
}

/* Builds a complete, opaque, full-screen RGB565 frame representing exactly
 * what the display should look like once target_screen is the active
 * screen -- a fresh screen-only base (snapshot_screen_base()) with the
 * persistent bars blended fresh on top (blend_persistent_bars()). Used as
 * begin_slide_transition()'s synchronous fallback when neither the static-
 * screen cache nor the Phase 2 player cache has a base available -- the
 * cached-base cases dup their own cached base and call
 * blend_persistent_bars() directly instead of going through this, so the
 * bars are still always fresh even when the (comparatively expensive)
 * screen content itself comes from a cache. Caller owns the returned
 * buffer (lv_draw_buf_destroy() it when done); returns NULL on allocation/
 * snapshot failure. */
static lv_draw_buf_t * build_flattened_transition_frame(lv_obj_t * target_screen) {
    lv_draw_buf_t * base = snapshot_screen_base(target_screen);
    if (!base) return NULL;
    blend_persistent_bars(base, target_screen);
    return base;
}

/* A deliberately narrow set of screens are pure static content -- fixed
 * icon-grid/pill-list items baked in at build time, no toggles, no
 * per-item state, nothing a timer ever touches. Their screen-only content
 * is rendered ONCE at startup and reused forever, since re-rendering it
 * before every single visit was the last remaining cost in an otherwise-
 * cheap transition -- the persistent status bar/home-indicator content is
 * deliberately NOT part of this cached base (see blend_persistent_bars()'s
 * own comment on why baking bars into a cache built once at startup goes
 * stale); begin_slide_transition() blends them in fresh every time it uses
 * this cache. Anything with actual dynamic content (player screen, file/
 * song lists, Settings' toggles, the accent-color picker's selection ring,
 * Subsonic status screens) is deliberately left out and always rendered
 * fresh. */
#define STATIC_SNAPSHOT_SCREEN_COUNT 9
static lv_obj_t * static_snapshot_screen[STATIC_SNAPSHOT_SCREEN_COUNT];
static lv_draw_buf_t * static_snapshot_buf[STATIC_SNAPSHOT_SCREEN_COUNT];

static lv_draw_buf_t * get_static_snapshot(lv_obj_t * scr) {
    for (int i = 0; i < STATIC_SNAPSHOT_SCREEN_COUNT; i++) {
        if (static_snapshot_screen[i] == scr) return static_snapshot_buf[i];
    }
    return NULL;
}

void register_static_snapshot(int index, lv_obj_t * scr) {
    static_snapshot_screen[index] = scr;
    static_snapshot_buf[index] = snapshot_screen_base(scr);
}

/* Player-screen transition-frame cache -- TRANSITION_PERFORMANCE_PLAN.md
 * Phase 2. gui_player_get_screen() is dynamic (track metadata/art/play-state, so it
 * can't just be baked in once like the STATIC_SNAPSHOT_SCREEN_COUNT screens
 * above), but it's also the single most common transition target (every
 * player-swipe and most Home/Now-Playing navigations) and the one whose
 * on-demand lv_snapshot_take() cost is actually visible: confirmed via
 * on-device UI_PERF_TRACE profiling, that snapshot alone typically costs
 * 10-14ms and spiked to ~84ms on a cold cache the first time it ran. This
 * mirrors quick_drawer_mark_snapshot_dirty()/quick_drawer_rebuild_snapshot()
 * above -- the exact same "own an independent bitmap, invalidate lazily,
 * rebuild once via lv_async_call() during the next idle pass instead of on
 * the touch-down path" shape, just for a full-screen target instead of the
 * drawer panel. Rebuilt off the gesture path entirely: at track start, once
 * cover art actually finishes decoding, on play/pause icon changes, on
 * accent-color changes, and when the hide-topbar setting changes -- NOT on
 * every per-second progress-bar tick (deliberately -- see the plan's own
 * Phase 2 requirement 2), so the cached frame's progress fill can be up to
 * a few seconds stale. Always safe to use regardless of staleness: this is
 * an independent owned copy, never aliased to any live widget's own memory,
 * so nothing about "how stale" it is can make it unsafe to display, only
 * slightly out of date -- see begin_slide_transition()'s own use of it.
 * Screen-only base -- deliberately does NOT include the persistent status
 * bar/home-indicator content, for the same staleness reason
 * blend_persistent_bars() explains: this cache is only rebuilt on the
 * dirty triggers above, not every second, so baking in the clock/battery/
 * wifi here would go visibly stale between rebuilds. begin_slide_
 * transition() blends those bars in fresh from this cached base every
 * time it's used. */
static lv_draw_buf_t * player_transition_cache_buf = NULL;
bool player_transition_cache_dirty = true;

static void player_transition_rebuild_cache(void) {
    /* Nothing to gain while gui_player_get_screen() is already the live screen --
     * no transition can ever target the screen already on display, and
     * re-snapshotting it here would just be wasted work on every one of
     * its own dynamic updates (track change, progress tick via the other
     * dirty triggers, etc.) while the user is actually looking at it. */
    if (!gui_player_get_screen() || lv_screen_active() == gui_player_get_screen()) return;
#ifdef UI_PERF_TRACE
    uint64_t perf_start_us = ui_perf_now_us();
#endif
    lv_draw_buf_t * fresh = snapshot_screen_base(gui_player_get_screen());
    if (!fresh) return; /* OOM -- keep whatever's cached (stale beats nothing); still tried again on the next dirty notification */
    if (player_transition_cache_buf) lv_draw_buf_destroy(player_transition_cache_buf);
    player_transition_cache_buf = fresh;
    player_transition_cache_dirty = false;
#ifdef UI_PERF_TRACE
    printf("PERF player_cache rebuild_us=%llu\n", (unsigned long long) (ui_perf_now_us() - perf_start_us));
#endif
}

void player_transition_cache_async_cb(void * unused) {
    (void) unused;
    /* Re-checked here, not just at the mark_dirty() call site: several
     * dirty notifications can each schedule their own async callback
     * before the first one runs (lv_async_call() doesn't dedupe by
     * callback/data), so every call after the first one that actually
     * rebuilds is a cheap no-op instead of a redundant re-snapshot. */
    if (player_transition_cache_dirty) player_transition_rebuild_cache();
}

/* Called from every site where something visible on gui_player_get_screen()'s own
 * subtree changes -- see this function's own doc comment on the cache
 * above for the current full list of call sites. Deliberately NOT called
 * from the routine per-second progress-bar update. */
void player_transition_mark_dirty(void) {
    player_transition_cache_dirty = true;
    if (gui_player_get_screen()) lv_async_call(player_transition_cache_async_cb, NULL);
}

/* slide_transition_ctx_t defined in gui.h */

static bool slide_transition_active = false;

/* Forward declarations -- real (first) tentative/actual definitions
 * further down this file, alongside poll_quick_drawer_drag()'s own
 * player-swipe state. slide_transition_anim_x_cb()'s compositor-failure
 * abort path below needs to clear these directly: a hard compositor
 * failure can happen mid-drag, and by the time it's detected, transition_
 * compositor_frame() has already torn down the compositor session itself
 * (LVGL invalidation restored, buf_act re-synced) -- what's left is purely
 * this file's OWN gesture-tracking bookkeeping, which only these three
 * variables (plus the ctx/anim cleanup already shared with the normal
 * commit/cancel path) actually hold. */
static bool player_swipe_candidate;
static bool player_swipe_tracking;
static slide_transition_ctx_t * player_swipe_ctx;

/* Shared by two unrelated callers -- both need "the next real
 * lv_timer_handler() pass redraws literally everything" without calling
 * lv_refr_now() synchronously from inside a callback that's itself already
 * running from within an lv_timer_handler() pass (a real reentrancy risk,
 * not just a style preference): slide_transition_anim_x_cb()'s compositor-
 * failure recovery (see its own comment), and update_timer_cb()'s screen-
 * wake handling (NEXT_TODO_IMPLEMENTATION_PROMPT.md Task 1) -- the async
 * call runs within that same lv_timer_handler() invocation, just after all
 * timers finish, which is still effectively immediate either way. */
void full_redraw_async_cb(void * unused) {
    (void) unused;
    lv_obj_invalidate(lv_screen_active());
    lv_obj_invalidate(lv_layer_top());
    lv_obj_invalidate(lv_layer_sys());
}

void slide_transition_anim_x_cb(void * var, int32_t v) {
    slide_transition_ctx_t * ctx = (slide_transition_ctx_t *) var;
    /* TRANSITION_PERFORMANCE_PLAN.md Phase 3 -- when begin_slide_transition()
     * below handed this transition off to the direct-framebuffer compositor,
     * img_from/img_to are never drawn at all (LVGL invalidation is disabled
     * for the whole session), so driving them with lv_obj_set_x() here would
     * be pure wasted work: skip straight to compositing this frame instead.
     * Every caller of this function (screen_transition_slide()'s own
     * lv_anim_t, poll_quick_drawer_drag()'s per-tick interactive drag, and
     * its release settle animation) goes through this one shared path, so
     * all three get compositor support with no changes of their own. */
    if (transition_compositor_is_active()) {
        if (!transition_compositor_frame(v)) {
            /* Hard presentation failure. transition_compositor_frame()
             * already restored normal fbdev/LVGL ownership. Recover to a
             * deterministic logical screen: a fixed transition or a
             * released/committed gesture keeps its destination (the nav
             * stack has already been updated); an in-progress or cancelled
             * gesture returns to the source (and has made no stack change).
             * The deferred invalidation avoids a re-entrant lv_refr_now()
             * from inside an animation/timer callback. */
            lv_anim_delete(ctx, slide_transition_anim_x_cb); /* safe no-op if ctx isn't driven by a real lv_anim_t (the raw per-tick interactive-drag path isn't) */
            lv_obj_t * recovery_scr = ctx->commit ? ctx->to_scr : ctx->from_scr;
            lv_screen_load(recovery_scr);
            sync_player_topbar_visibility(recovery_scr);
            lv_async_call(full_redraw_async_cb, NULL);
            if (ctx->overlay) lv_obj_delete(ctx->overlay);
            if (ctx->buf_from_owned) lv_draw_buf_destroy(ctx->buf_from);
            if (ctx->buf_to_owned) lv_draw_buf_destroy(ctx->buf_to);
            if (player_swipe_ctx == ctx) player_swipe_ctx = NULL;
            player_swipe_tracking = false;
            player_swipe_candidate = false;
            lv_free(ctx);
            slide_transition_active = false;
        }
        return;
    }
    lv_obj_set_x(ctx->img_from, v);
    lv_obj_set_x(ctx->img_to, v + ctx->to_offset);
}

void slide_transition_done_cb(lv_anim_t * a) {
    slide_transition_ctx_t * ctx = (slide_transition_ctx_t *) lv_anim_get_user_data(a);
    lv_obj_t * final_scr = ctx->commit ? ctx->to_scr : ctx->from_scr;
    if (ctx->commit) {
        lv_screen_load(ctx->to_scr);
    }
    /* The fallback owns flattened full-screen images, including both
     * persistent bands, so their live layer objects stay hidden throughout
     * the slide. Restore the status bar for the screen actually selected
     * and restore the screen-independent home-indicator setting now. */
    sync_player_topbar_visibility(final_scr);
    lv_obj_t * hb_sup = gui_shell_get_home_indicator_band();
    if (ctx->fallback_bands_suppressed && hb_sup) {
        if (ctx->home_indicator_was_hidden)
            lv_obj_add_flag(hb_sup, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_remove_flag(hb_sup, LV_OBJ_FLAG_HIDDEN);
    }
    if (ctx->overlay) lv_obj_delete(ctx->overlay); /* deletes img_from/img_to too -- NULL when this transition was handed to the compositor instead (see begin_slide_transition()'s own comment on why the overlay is skipped entirely there, not just left undrawn) */
    if (ctx->buf_from_owned) lv_draw_buf_destroy(ctx->buf_from);
    if (ctx->buf_to_owned) lv_draw_buf_destroy(ctx->buf_to);
    transition_compositor_end(); /* no-op if the compositor was never activated for this transition */
    lv_free(ctx);
    slide_transition_active = false;
}

/* Moved up from its own neighborhood further down (build_home_indicator_bar())
 * -- a plain #define has no location dependency on where home_indicator_band
 * itself is built, and blend_overlay_onto_base() (used by
 * build_flattened_transition_frame() above) reads that object's own real
 * on-screen coordinates directly rather than this constant, so nothing
 * below actually requires this specific placement anymore; left here since
 * moving it back offers no benefit either. */
#define HOME_INDICATOR_BAND_HEIGHT 34

/* Shared setup for both screen_transition_slide()'s own fixed-duration
 * path and the interactive (finger-driven) player-swipe further down
 * (poll_quick_drawer_drag()'s player_swipe_* state) -- snapshotting
 * from_scr/to_scr and building the two-image overlay is identical either
 * way; only how the resulting ctx gets ANIMATED afterward differs (one
 * fixed lv_anim_t vs. driven directly from live touch position, then a
 * short commit/cancel settle animation). Returns NULL if to_scr is
 * already active, a transition is already in flight, or a snapshot
 * failed (OOM) -- the caller should fall back to an instant
 * lv_screen_load(to_scr) in every one of those cases (screen_transition_slide()
 * does; the interactive path just abandons the gesture and lets it fall
 * through as whatever else it might have been, since there's no
 * "genuinely already there" case to fall back to for a still-in-progress
 * drag). ctx->commit defaults to true, matching every existing caller
 * (only the interactive path ever sets it false, on a cancelled drag).
 *
 * Both transition sources are always OWNED, independent copies now
 * (TRANSITION_PERFORMANCE_PLAN.md Phase 3) -- never a live alias of real
 * framebuffer memory, and never a direct reference to a cache buffer that
 * something else could destroy out from under this transition. See this
 * function's own body for the two separate real-device reasons: (1) a
 * live-aliased outgoing frame bled through live redraws happening on
 * from_scr during a held drag ("swiping to enter the player causes the
 * main menu to flicker"); (2) once the direct-framebuffer compositor is
 * involved, an aliased source becomes the compositor's OWN write target
 * again a couple of frames later (the two physical pages ping-pong every
 * frame), an overlapping-memcpy hazard, not just a staleness question. */
slide_transition_ctx_t * begin_slide_transition(lv_obj_t * to_scr, bool forward) {
#ifdef UI_PERF_TRACE
    uint64_t perf_begin_us = ui_perf_now_us();
    uint64_t perf_to_done_us;
    uint64_t perf_drain_done_us;
    uint64_t perf_from_done_us;
#endif
    lv_obj_t * from_scr = lv_screen_active();
    if (from_scr == to_scr || slide_transition_active) return NULL;

    lv_display_t * disp = lv_display_get_default();
    int32_t w = lv_display_get_horizontal_resolution(disp);
    int32_t h = lv_display_get_vertical_resolution(disp);

    slide_transition_ctx_t * ctx = lv_malloc(sizeof(*ctx));
    if (!ctx) return NULL;
    ctx->commit = true;
    ctx->buf_from_owned = true;
    ctx->buf_to_owned = true;
    ctx->fallback_bands_suppressed = false;
    ctx->home_indicator_was_hidden = true;

    /* Incoming source prepared FIRST, before the outgoing physical-page
     * capture below -- TRANSITION_PERFORMANCE_PLAN.md Phase 3 real-device
     * review finding. Always a COMPLETE flattened frame -- screen content
     * plus whichever persistent bars would really be visible there, never
     * a bare screen-only snapshot. The (comparatively expensive) screen
     * content itself is pre-baked for the fixed STATIC_SNAPSHOT_SCREEN_COUNT
     * screens or the Phase 2 player-transition cache when available (each
     * DUPLICATED here, never referenced directly -- the player cache can
     * be destroyed and replaced by an lv_async_call() rebuild firing mid-
     * gesture, a real use-after-free risk if referenced instead of
     * copied), else captured fresh synchronously; the persistent bars
     * (status bar/home indicator) are always blended in fresh right here,
     * regardless of which base was used -- see blend_persistent_bars()'s
     * own comment on why baking them into either cache would go stale. */
    lv_draw_buf_t * buf_to;
#ifdef UI_PERF_TRACE
    bool used_player_cache = false;
#endif
    lv_draw_buf_t * cached_base = get_static_snapshot(to_scr);
    if (!cached_base && to_scr == gui_player_get_screen() && player_transition_cache_buf) {
        cached_base = player_transition_cache_buf;
#ifdef UI_PERF_TRACE
        used_player_cache = true;
#endif
    }
    if (cached_base) {
        buf_to = lv_draw_buf_dup(cached_base);
        if (buf_to) blend_persistent_bars(buf_to, to_scr);
    } else {
        buf_to = build_flattened_transition_frame(to_scr);
    }
#ifdef UI_PERF_TRACE
    perf_to_done_us = ui_perf_now_us();
#endif

    /* Drain any already-queued LVGL rendering NOW, before capturing the
     * outgoing physical page -- TRANSITION_PERFORMANCE_PLAN.md Phase 3
     * real-device review finding: capturing the physical page first and
     * draining afterward (the earlier design, inside transition_
     * compositor_begin()) meant a pending redraw could still pan to a
     * DIFFERENT physical page after the capture, leaving frame zero of the
     * animation showing an already-stale image that visibly jumped
     * backward once the real (post-drain) state caught up. This driver's
     * flush_cb() is fully synchronous (no deferred/async flush
     * completion), so one lv_refr_now() call is guaranteed to fully
     * render AND flush everything pending before returning -- nothing can
     * still be "in flight" by the time the capture below runs.
     * transition_compositor_begin() itself no longer does this drain --
     * doing it here, before capture, is what actually matters; doing it
     * again inside begin() (after capture) would be too late. */
    lv_refr_now(disp);
#ifdef UI_PERF_TRACE
    perf_drain_done_us = ui_perf_now_us();
#endif

    /* Outgoing source: an owned copy of the REAL physical scanout page,
     * captured AFTER the drain above -- TRANSITION_PERFORMANCE_PLAN.md
     * Phase 3 fix. lv_linux_fbdev_get_active_page() is the fbdev driver's
     * own accessor for whichever physical half is actually being scanned
     * out right now; LVGL's generic lv_display_get_buf_active() (disp->
     * buf_act) is simply whichever buffer LVGL itself last rendered into
     * -- normally the same thing, but not a driver-level guarantee in
     * DIRECT double-buffered mode, so it's only the fallback here (host/
     * SDL builds, or if fbdev pan-based double buffering isn't active on
     * this display). */
    lv_draw_buf_t * buf_from = NULL;
#if LV_USE_LINUX_FBDEV
    {
        const void * phys_active = lv_linux_fbdev_get_active_page(disp);
        uint32_t fb_stride = lv_linux_fbdev_get_stride(disp);
        if (phys_active && fb_stride != 0) {
            lv_draw_buf_t phys_desc;
            memset(&phys_desc, 0, sizeof(phys_desc));
            phys_desc.header.magic = LV_IMAGE_HEADER_MAGIC;
            phys_desc.header.cf = LV_COLOR_FORMAT_RGB565;
            phys_desc.header.w = (uint32_t) w;
            phys_desc.header.h = (uint32_t) h;
            phys_desc.header.stride = fb_stride;
            phys_desc.data = (uint8_t *) phys_active; /* lv_draw_buf_dup() below only reads this -- never written through phys_desc itself */
            phys_desc.data_size = fb_stride * (uint32_t) h;
            buf_from = lv_draw_buf_dup(&phys_desc);
        }
    }
#endif
    if (!buf_from) {
        lv_draw_buf_t * active_buf = lv_display_get_buf_active(disp);
        buf_from = active_buf ? lv_draw_buf_dup(active_buf) : lv_snapshot_take(from_scr, LV_COLOR_FORMAT_RGB565);
    }
#ifdef UI_PERF_TRACE
    perf_from_done_us = ui_perf_now_us();
#endif
    if (!buf_from || !buf_to) {
        /* Snapshot failed (e.g. OOM) -- caller falls back to an instant cut
         * rather than crash or get stuck mid-navigation/mid-drag. */
        if (buf_from && ctx->buf_from_owned) lv_draw_buf_destroy(buf_from);
        if (buf_to && ctx->buf_to_owned) lv_draw_buf_destroy(buf_to);
        lv_free(ctx);
        return NULL;
    }

    int32_t to_offset = forward ? w : -w;
    ctx->buf_from = buf_from;
    ctx->buf_to = buf_to;
    ctx->from_scr = from_scr;
    ctx->to_scr = to_scr;
    ctx->to_offset = to_offset;

    slide_transition_active = true;

    /* TRANSITION_PERFORMANCE_PLAN.md Phase 3 -- try to hand this transition
     * off to the direct-framebuffer compositor BEFORE creating any LVGL
     * overlay/image objects, not after. Real-device bug (earlier design):
     * creating those objects first queued their own initial-draw
     * invalidation via the normal, still-enabled path, which then got
     * rendered and flushed for real on the next lv_timer_handler() tick
     * regardless of disabling invalidation moments later -- visible as a
     * leftover static image flashing on top of the compositor's own
     * correctly-sliding frames. Skipping the overlay entirely when the
     * compositor takes over removes the problem at its root; transition_
     * compositor_begin() itself also drains any OTHER already-queued LVGL
     * rendering (lv_refr_now()) before disabling invalidation, so nothing
     * unrelated is left stranded in the queue either. */
    if (transition_compositor_begin(buf_from, buf_to, to_offset)) {
        ctx->overlay = NULL;
        ctx->img_from = NULL;
        ctx->img_to = NULL;
    } else {
        /* buf_from/buf_to already contain the persistent bands. Hide the
         * live layer copies for the whole fallback animation so they move
         * exactly once as part of those flattened frames instead of being
         * drawn a second time, stationary, above the sliding images. */
        ctx->fallback_bands_suppressed = true;
        lv_obj_t * hb_sl = gui_shell_get_home_indicator_band();
        lv_obj_t * sb_sl = gui_shell_get_status_bar_band();
        if (hb_sl) {
            ctx->home_indicator_was_hidden =
                lv_obj_has_flag(hb_sl, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(hb_sl, LV_OBJ_FLAG_HIDDEN);
        }
        if (sb_sl)
            lv_obj_add_flag(sb_sl, LV_OBJ_FLAG_HIDDEN);

        lv_obj_t * overlay = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(overlay);
        lv_obj_set_size(overlay, lv_pct(100), lv_pct(100));
        lv_obj_set_pos(overlay, 0, 0);
        lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE); /* swallow touches while the slide is in flight */
        /* Keep transient top-layer UI such as the volume popup above the
         * transition. The persistent bands themselves are hidden above. */
        lv_obj_move_to_index(overlay, 0);

        lv_obj_t * img_from = lv_image_create(overlay);
        lv_image_set_src(img_from, buf_from);
        lv_obj_set_pos(img_from, 0, 0);

        lv_obj_t * img_to = lv_image_create(overlay);
        lv_image_set_src(img_to, buf_to);
        lv_obj_set_pos(img_to, to_offset, 0);

        ctx->overlay = overlay;
        ctx->img_from = img_from;
        ctx->img_to = img_to;
    }
#ifdef UI_PERF_TRACE
    uint64_t perf_end_us = ui_perf_now_us();
    printf("PERF transition begin_us=%llu to_us=%llu drain_us=%llu from_us=%llu setup_us=%llu from_owned=%d to_owned=%d player_cache=%d cache_dirty=%d compositor=%d\n",
           (unsigned long long) (perf_end_us - perf_begin_us),
           (unsigned long long) (perf_to_done_us - perf_begin_us),
           (unsigned long long) (perf_drain_done_us - perf_to_done_us),
           (unsigned long long) (perf_from_done_us - perf_drain_done_us),
           (unsigned long long) (perf_end_us - perf_from_done_us),
           ctx->buf_from_owned, ctx->buf_to_owned,
           used_player_cache, player_transition_cache_dirty, transition_compositor_is_active());
#endif
    return ctx;
}

static void screen_transition_slide(lv_obj_t * to_scr, bool forward) {
    slide_transition_ctx_t * ctx = begin_slide_transition(to_scr, forward);
    if (!ctx) {
        lv_screen_load(to_scr);
        sync_player_topbar_visibility(to_scr);
        return;
    }

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, ctx);
    lv_anim_set_user_data(&a, ctx);
    lv_anim_set_values(&a, 0, -ctx->to_offset);
    lv_anim_set_duration(&a, NAV_ANIM_TIME_MS);
    lv_anim_set_exec_cb(&a, slide_transition_anim_x_cb);
    lv_anim_set_completed_cb(&a, slide_transition_done_cb);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

void nav_push(lv_obj_t * scr) {
#ifdef UI_PERF_TRACE
    uint64_t perf_start_us = ui_perf_now_us();
#endif
    if (nav_depth > 0 && nav_stack[nav_depth - 1] == scr) {
        lv_screen_load(scr); /* already the active screen -- nothing to push */
        sync_player_topbar_visibility(scr);
        return;
    }
    if (nav_depth < NAV_STACK_MAX) {
        nav_stack[nav_depth++] = scr;
    }
    /* Live A/B test: forward navigation (entering a submenu) now cuts
     * instantly instead of sliding -- screen_transition_slide() is still
     * used by nav_pop() below, so backing out still animates. */
    lv_screen_load(scr);
    sync_player_topbar_visibility(scr);
#ifdef UI_PERF_TRACE
    printf("PERF nav_push load_us=%llu depth=%d\n",
           (unsigned long long) (ui_perf_now_us() - perf_start_us), nav_depth);
#endif
}

void nav_pop(void) {
    if (nav_depth > 1) nav_depth--;
    /* Keep the outgoing screen's bars untouched until its physical frame
     * has been captured. The transition completion/cut-fallback path
     * applies the destination state at the actual screen handoff. */
    screen_transition_slide(nav_stack[nav_depth - 1], false);
}

/* Splices the stack slot at `index` out entirely (shifting everything
 * above it down by one), with no screen load of any kind -- used when a
 * transient interstitial screen (Wi-Fi/Subsonic's "Connecting..."/
 * "Downloading..." screen, or a chained show_text_entry() call) has
 * already been left behind by something that pushed a DIFFERENT screen on
 * top of it instead of returning to it. Real-device bug report: a
 * downloaded Subsonic track played fine (see poll_subsonic_download()'s
 * own comment on the race this and its sibling nav_pop()-skipping fixes
 * solve), but backing out of the player afterward landed back on the now-
 * defunct "Downloading..." screen instead of the song list underneath it
 * -- because skipping nav_pop() to avoid yanking the player screen away
 * left that interstitial's own slot sitting in the stack forever, one
 * level below wherever things actually ended up. This removes exactly
 * that stale slot after the fact, once it's clear something else already
 * took its place, so a later Back walks back through where the user
 * really came from instead of a resolved, no-longer-relevant waiting
 * screen. Purely bookkeeping -- whatever's currently on screen was
 * already loaded by the nav_push() that grew the stack past `index` in
 * the first place, so nothing here should touch the display. */
void nav_remove_stack_slot(int index) {
    for (int i = index; i < nav_depth - 1; i++) nav_stack[i] = nav_stack[i + 1];
    if (nav_depth > 0) nav_depth--;
}

/* Collapses the whole nav stack back to Home -- used after a library rescan,
 * since any deeper screen (Artists/Albums/group songs/...) may be showing
 * rows built from the pre-rescan data and would otherwise still be reachable
 * via back-navigation. */
void nav_reset_to_home(void) {
    nav_depth = 1;
    nav_stack[0] = gui_shell_get_home_screen();
    lv_screen_load(gui_shell_get_home_screen());
    sync_player_topbar_visibility(gui_shell_get_home_screen());
}

/* Shared back-button handler for every screen built via the reusable
 * icon-grid/pill-list builders -- passed directly as their back_btn_cb. */
void generic_back_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_pop();
}

/* Sets LV_OBJ_FLAG_GESTURE_BUBBLE on every descendant of `obj` so a swipe
 * started anywhere inside a screen (over a label, button, or scrollable
 * list) bubbles up to the screen itself to be handled as app-wide
 * navigation. Deliberately does NOT recurse into drag-to-adjust widgets
 * (sliders/switches/dropdowns/rollers) -- those consume horizontal drags
 * themselves (e.g. seeking the progress bar), and letting a big drag on one
 * of those also fire a navigation swipe would be surprising. */
void enable_gesture_bubble_recursive(lv_obj_t * obj) {
    uint32_t count = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < count; i++) {
        lv_obj_t * child = lv_obj_get_child(obj, i);
        if (lv_obj_check_type(child, &lv_slider_class) ||
            lv_obj_check_type(child, &lv_switch_class) ||
            lv_obj_check_type(child, &lv_dropdown_class) ||
            lv_obj_check_type(child, &lv_roller_class)) {
            continue;
        }
        lv_obj_add_flag(child, LV_OBJ_FLAG_GESTURE_BUBBLE);
        enable_gesture_bubble_recursive(child);
    }
}

/* Defined later alongside the rest of the quick-access drawer, but needed
 * here for the swipe-down-from-the-top-edge trigger below. */
/* Defined later alongside player_swipe_press_excluded()'s own raw-polling
 * dead-zone machinery -- needed here too, by screen_gesture_event_cb()
 * below, see its own comment. */
#define QUICK_DRAWER_ANIM_MS 120 /* real-hardware feedback: 200 felt slow for the post-release snap */
#define QUICK_DRAWER_TRIGGER_ZONE 140 /* swipe-down must start within this many px of the top edge to open it */

/* Global swipe handling for back/forward nav. Swipe left-to-right (finger
 * drags rightward) = go back, matching the standard back gesture shown in
 * the reference photos. Swipe right-to-left (finger drags leftward) = jump
 * straight to the player screen from anywhere, matching the real device's
 * "now playing" shortcut. Registered per-screen in
 * finalize_screen_navigation(), which bubbles correctly via
 * enable_gesture_bubble_recursive() (see its own comment).
 *
 * The quick-access drawer's open/close used to also be driven from here
 * (swipe-down/up as instant, threshold-triggered LV_EVENT_GESTURE actions),
 * but that's now poll_quick_drawer_drag()'s job instead -- see its own
 * comment for why: both the GESTURE-bubbling approach here and an
 * indev-wide LV_EVENT_PRESSING attempt turned out unreliable on real
 * hardware for a surface as densely covered in its own interactive
 * children (icons, a 300px-wide slider) as the drawer is. */
/* Defined in the search-binding section below -- true (and closes it)
 * if `screen` had an active inline search; forward-declared here so the
 * back-swipe gesture can close search first instead of popping straight
 * past it to the previous screen, same convention as a back button/gesture
 * dismissing an open search box before it navigates anywhere. */

static void screen_gesture_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_GESTURE) return;
    lv_indev_t * indev = lv_indev_active();
    if (!indev) return;

    /* Real-device bug report: dragging the player screen's own seek bar
     * (progress_slider) still triggered the back-swipe. Widening its
     * ext_click_area (see progress_slider's own comment) wasn't enough on
     * its own -- LVGL's gesture recognition runs throughout a slider drag
     * regardless (sliders clear LV_OBJ_FLAG_SCROLLABLE in their
     * constructor, so the generic scroll_obj early-exit indev_gesture()
     * relies on elsewhere never applies to one), and real-device testing
     * showed this still reaching here rather than staying resolved to the
     * slider itself the way GESTURE_BUBBLE exclusion (enable_gesture_
     * bubble_recursive()) was expected to guarantee. Reusing the same two
     * checks player_swipe_press_excluded() already combines for the
     * separate raw-polling player-swipe path: active_press_is_over_drag_
     * adjust_widget() (hit-tested object identity -- covers a press that
     * lands squarely on progress_slider) plus point_in_swipe_dead_zone()
     * (raw point-in-rect against the registered dead-zone list --
     * progress_slider is registered there too, see its own
     * register_swipe_dead_zone() call, covering a press that lands just
     * off it) closes the gap regardless of exactly which part of LVGL's
     * own gesture-bubbling chain let it through. */
    lv_point_t gesture_press_point;
    lv_indev_get_point(indev, &gesture_press_point);
    if (active_press_is_over_drag_adjust_widget() || point_in_swipe_dead_zone(gesture_press_point)) return;

    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (dir == LV_DIR_RIGHT) {
        if (!search_close_if_active_for_screen(lv_screen_active())) {
            nav_pop();
        }
        /* The finger is still down mid-gesture when the screen swaps out
         * from under it -- without this, its eventual release gets
         * delivered to whatever object now sits at that same coordinate on
         * the NEW screen, firing an unwanted click there (real-hardware
         * testing: swiping back from a submenu landed a phantom tap on
         * whatever Home tile happened to be under the finger). Telling the
         * indev to disregard everything until the next physical release
         * stops that bleed-through. */
        lv_indev_wait_release(indev);
    }
    /* Swipe-left-to-player used to be handled here too (instant
     * nav_push(gui_player_get_screen()), no animation) -- real-device bug report:
     * entering the player didn't follow the finger the way the quick
     * drawer's own drag does. Replaced with a live, finger-driven version
     * in poll_quick_drawer_drag() (see its own player_swipe_* state),
     * which claims the press well before LVGL's own ~50px built-in gesture
     * threshold (LV_INDEV_DEF_GESTURE_LIMIT) would ever fire this handler
     * for LV_DIR_LEFT -- left here removed rather than merely unreachable,
     * since leaving it live risked a second, redundant nav_push() firing
     * behind the new interactive one, and creating an overlay object under
     * an already-moving finger is exactly the kind of thing that corrupts
     * LVGL's own press tracking (see the PRESS_LOST history on the
     * drawer's own icons, quick_drawer_wifi_long_press_cb's comment) if
     * anything else is still independently reacting to the same gesture.
     *
     * Swipe-up-to-Home is deliberately NOT handled here -- real-device
     * feedback: this fired from anywhere on screen, including a drag that
     * started well above the home indicator band and only incidentally
     * ended up moving upward (e.g. an aborted attempt to scroll a list up).
     * home_indicator_gesture_cb() (see build_home_indicator_bar()) is the
     * only path to nav_reset_to_home() now -- it only ever fires for a drag
     * that actually started within the reserved bottom strip, matching the
     * Android gesture-bar convention this is modeled on. */
}

/* Finishing touch every build_XXX_screen() calls just before returning: wire
 * up the swipe gestures generically instead of repeating this per screen. */
void finalize_screen_navigation(lv_obj_t * scr) {
    /* lv_obj_hit_test() -- and therefore all press/drag/gesture detection --
     * bails out immediately for any object lacking LV_OBJ_FLAG_CLICKABLE
     * (confirmed by reading lv_obj_hit_test() in lv_obj_pos.c). A plain
     * lv_obj_create(NULL) screen doesn't have that flag by default, so a
     * swipe starting on bare screen background (anywhere not already
     * covered by a clickable child) would otherwise never even register as
     * a press, let alone a gesture. */
    lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
    /* Every inner container built by this codebase already has SCROLLABLE
     * removed, but the screen root itself never did -- left scrollable (the
     * lv_obj default), a drag is first consumed as a scroll attempt (visible
     * on real hardware as the whole screen dragging/rubber-banding) and only
     * escalates to a real LV_EVENT_GESTURE in narrower cases, which is why
     * swipe-to-go-back wasn't firing reliably. None of these screens use
     * their own root as a scroll viewport -- scrolling, where it exists,
     * always happens on a dedicated inner list/container instead. */
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    enable_gesture_bubble_recursive(scr);
    /* LV_EVENT_PRESSED is deliberately NOT registered here -- see the
     * indev-level registration in gui_init() below for why. */
    lv_obj_add_event_cb(scr, screen_gesture_event_cb, LV_EVENT_GESTURE, NULL);
}



void gui_navigation_init(void) {
    register_static_snapshot(0, gui_shell_get_home_screen());
    register_static_snapshot(1, gui_library_get_music_screen());
    register_static_snapshot(2, stream_media_screen);
    register_static_snapshot(3, gui_network_get_wireless_screen());
    register_static_snapshot(4, gui_books_get_screen());
    register_static_snapshot(5, gui_settings_get_about_screen());
    register_static_snapshot(6, gui_settings_get_screen());
    register_static_snapshot(7, gui_settings_get_system_screen());
    register_static_snapshot(8, gui_shell_get_dac_home_screen());

    nav_stack[0] = gui_shell_get_home_screen();
    nav_depth = 1;
    lv_screen_load(gui_shell_get_home_screen());
}



int gui_navigation_get_depth(void) {
    return nav_depth;
}

lv_obj_t * gui_navigation_get_top_screen(void) {
    return (nav_depth > 0) ? nav_stack[nav_depth - 1] : NULL;
}

lv_obj_t * gui_navigation_get_screen_at(int index) {
    if (index < 0 || index >= nav_depth) return NULL;
    return nav_stack[index];
}

bool gui_navigation_is_top(lv_obj_t * screen) {
    return (nav_depth > 0 && nav_stack[nav_depth - 1] == screen);
}

void gui_navigation_remove_screen_instances(lv_obj_t ** screens, int count) {
    for (int j = 0; j < count; j++) {
        for (int i = nav_depth - 1; i >= 0; i--) {
            if (nav_stack[i] == screens[j]) {
                nav_remove_stack_slot(i);
            }
        }
    }
}

void gui_navigation_replace_top(lv_obj_t * new_screen) {
    if (nav_depth > 0) {
        nav_stack[nav_depth - 1] = new_screen;
    }
}

void gui_navigation_pop_to_depth(int target_depth) {
    if (target_depth >= 1 && target_depth < nav_depth) {
        nav_depth = target_depth;
    }
}
