#include "transition_compositor.h"
#include "src/draw/lv_draw_buf.h"
#include <string.h>
#ifdef UI_PERF_TRACE
#include <stdio.h>
#include <time.h>
#endif

/* lv_linux_fbdev_*() (the actual page/stride/pan/handoff accessors this
 * compositor needs) are only ever DECLARED when LV_USE_LINUX_FBDEV is set
 * (lv_conf.h -- 0 on HOST_BUILD, which uses SDL instead) -- guard the
 * include and every call site so the host/SDL build still compiles,
 * always taking the fbdev-unavailable path (transition_compositor_begin()
 * returns false, gui.c's existing LVGL-object overlay is used unmodified
 * there, same as on real hardware if this driver ever changes to not
 * support pan-based double buffering). */
#if LV_USE_LINUX_FBDEV
#include "src/drivers/display/fb/lv_linux_fbdev.h"
#endif

/* TRANSITION_PERFORMANCE_PLAN.md Phase 3. Was disabled during the rewrite
 * that fixed the issues found in an earlier design (a leftover LVGL
 * overlay flashing on top of the composited frame, a NULL-pointer crash on
 * swipe release, a stationary top/bottom band that either blanked or
 * visibly flickered, and -- found in the fallback path, not this module,
 * but from the same root cause -- the persistent bars being drawn twice).
 * The rewrite addresses the actual root causes: full-frame sources with no
 * "margin" concept, a real fbdev external-composition handoff that keeps
 * LVGL's own buf_act synchronized, pan-failure-safe bookkeeping, and an
 * owned (never aliased) incoming/outgoing source. Now enabled for on-
 * device diagnostic testing (2026-08-22) -- set back to 1 immediately if
 * real-hardware testing finds anything wrong; the fallback path (this file
 * disabled) is independently confirmed correct on its own. */
#define COMPOSITOR_DISABLED 0

static bool compositor_active = false;
#if LV_USE_LINUX_FBDEV
static const lv_draw_buf_t * compositor_from;
static const lv_draw_buf_t * compositor_to;
static int32_t compositor_to_offset;
static int32_t compositor_width;
static int32_t compositor_height;
static uint32_t compositor_fb_stride;
#endif

#ifdef UI_PERF_TRACE
static uint64_t compositor_perf_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000ULL + (uint64_t) ts.tv_nsec / 1000ULL;
}
static uint64_t compositor_perf_frame_total_us;
static uint64_t compositor_perf_frame_max_us;
static unsigned compositor_perf_frame_count;
static unsigned compositor_perf_present_failures;
#endif

bool transition_compositor_available(void) {
#if COMPOSITOR_DISABLED || !LV_USE_LINUX_FBDEV
    return false; /* see COMPOSITOR_DISABLED's own comment further up this file */
#else
    return lv_linux_fbdev_get_stride(lv_display_get_default()) != 0;
#endif
}

bool transition_compositor_begin(const lv_draw_buf_t * from, const lv_draw_buf_t * to, int32_t to_offset) {
#if COMPOSITOR_DISABLED || !LV_USE_LINUX_FBDEV
    (void) from; (void) to; (void) to_offset;
    return false;
#else
    if (compositor_active) return false; /* only one slide transition is ever in flight at once (see slide_transition_active in gui.c) -- never expected, guarded anyway */
    if (!from || !to) return false;
    if (from->header.cf != LV_COLOR_FORMAT_RGB565 || to->header.cf != LV_COLOR_FORMAT_RGB565) {
#ifdef UI_PERF_TRACE
        printf("PERF compositor begin_active=0 reason=cf from=%d to=%d\n", from->header.cf, to->header.cf);
#endif
        return false;
    }

    lv_display_t * disp = lv_display_get_default();
    uint32_t fb_stride = lv_linux_fbdev_get_stride(disp);
    if (fb_stride == 0) {
#ifdef UI_PERF_TRACE
        printf("PERF compositor begin_active=0 reason=no_pan_double_buffer\n");
#endif
        return false; /* pan-based double buffering not active on this display -- caller keeps its existing LVGL-object overlay */
    }

    int32_t w = lv_display_get_horizontal_resolution(disp);
    int32_t h = lv_display_get_vertical_resolution(disp);
    if (w <= 0 || h <= 0 || (to_offset != w && to_offset != -w)) {
#ifdef UI_PERF_TRACE
        printf("PERF compositor begin_active=0 reason=geometry w=%d h=%d to_offset=%d\n",
               w, h, to_offset);
#endif
        return false;
    }
    /* Debug-only invariants (TARGET ARCHITECTURE section D) -- never
     * expected to fail given gui.c's own construction of these buffers,
     * but a silent shape mismatch here would corrupt the framebuffer via
     * out-of-bounds row math, so assert loudly in diagnostic builds rather
     * than assume. */
#ifdef UI_PERF_TRACE
    LV_ASSERT(to_offset == w || to_offset == -w);
    LV_ASSERT(from->header.stride > 0 && to->header.stride > 0);
#endif
    if ((int32_t) from->header.w != w || (int32_t) from->header.h != h ||
        (int32_t) to->header.w != w || (int32_t) to->header.h != h) {
#ifdef UI_PERF_TRACE
        printf("PERF compositor begin_active=0 reason=shape w=%d h=%d from_w=%u from_h=%u to_w=%u to_h=%u\n",
               w, h, from->header.w, from->header.h, to->header.w, to->header.h);
#endif
        return false;
    }
    /* Each source must independently carry enough bytes for its own
     * tightly-packed w*h*2 RGB565 image, and each source's own stride must
     * be at least w*2 -- lv_draw_buf_goto_xy() below trusts each buffer's
     * OWN header.stride for row addressing, so a buffer that's the right
     * w/h but a smaller data_size or a too-small stride (e.g. a corrupt or
     * truncated snapshot) would still walk off its own end. 64-bit
     * arithmetic throughout: at this display's real size (480x800x2) the
     * product fits comfortably in 32 bits, but validation code should
     * never be the thing that overflows first. Real-device review finding
     * (2026-08-22): an earlier version validated `to` against a threshold
     * computed from `from`'s own stride instead of its own, silently
     * passing a mismatched `to` buffer whenever the two strides differed. */
    uint64_t min_stride = (uint64_t) (uint32_t) w * 2ULL;
    uint64_t from_need = (uint64_t) from->header.stride * (uint64_t) (uint32_t) h;
    uint64_t to_need = (uint64_t) to->header.stride * (uint64_t) (uint32_t) h;
    if ((uint64_t) from->header.stride < min_stride ||
        (uint64_t) to->header.stride < min_stride ||
        (uint64_t) from->data_size < from_need || (uint64_t) to->data_size < to_need) {
#ifdef UI_PERF_TRACE
        printf("PERF compositor begin_active=0 reason=data_size\n");
#endif
        return false;
    }

    if (!lv_linux_fbdev_begin_external_composition(disp)) {
#ifdef UI_PERF_TRACE
        printf("PERF compositor begin_active=0 reason=fbdev_handoff\n");
#endif
        return false;
    }

    compositor_from = from;
    compositor_to = to;
    compositor_to_offset = to_offset;
    compositor_width = w;
    compositor_height = h;
    compositor_fb_stride = fb_stride;
#ifdef UI_PERF_TRACE
    compositor_perf_frame_total_us = 0;
    compositor_perf_frame_max_us = 0;
    compositor_perf_frame_count = 0;
    compositor_perf_present_failures = 0;
#endif

    /* Nothing may touch buf_1/buf_2 (the two physical framebuffer pages)
     * through the normal LVGL render/flush pipeline while this compositor
     * is writing into them directly -- lv_display_enable_invalidation(false)
     * makes every _lv_inv_area() call (every lv_obj_invalidate*() anywhere
     * in the app, for the rest of this compositor session) a silent no-op,
     * so lv_timer_handler() has nothing queued to ever render or flush.
     * lv_snapshot_take() (used to build both full-frame transition
     * sources) is unaffected either way -- confirmed by reading
     * lv_snapshot.c: it draws into its own independently-allocated buffer,
     * never disp->buf_act. */
    lv_display_enable_invalidation(disp, false);
    compositor_active = true;
#ifdef UI_PERF_TRACE
    printf("PERF compositor begin_active=1 w=%d h=%d stride=%u to_offset=%d\n", w, h, fb_stride, to_offset);
#endif
    return true;
#endif /* COMPOSITOR_DISABLED || !LV_USE_LINUX_FBDEV */
}

bool transition_compositor_frame(int32_t v) {
    if (!compositor_active) return false; /* always false when disabled/unavailable -- begin() above never sets it */
#if COMPOSITOR_DISABLED || !LV_USE_LINUX_FBDEV
    (void) v;
    return false;
#else
#ifdef UI_PERF_TRACE
    uint64_t perf_frame_start_us = compositor_perf_now_us();
#endif
    lv_display_t * disp = lv_display_get_default();
    /* Re-queried every frame, not cached at begin() -- the inactive half
     * flips every time a frame is presented below, and writing into
     * whatever is CURRENTLY inactive (not whichever page happened to be
     * inactive when the gesture started) is exactly what keeps this
     * tear-free, the same guarantee the driver's own normal flush_cb() pan
     * branch relies on for an ordinary LVGL-rendered frame. */
    uint8_t * dest_page = (uint8_t *) lv_linux_fbdev_get_inactive_page(disp);
    if (!dest_page) {
        /* Shouldn't happen once begin() has already confirmed pan-based
         * double buffering is active, but never assume mid-session --
         * treat exactly like a present failure below (hard abort) rather
         * than silently writing through a null pointer or leaving the
         * session in an ambiguous state. */
        transition_compositor_end();
        return false;
    }

    int32_t w = compositor_width;
    int32_t from_x = v;
    int32_t to_x = v + compositor_to_offset;

    /* Intersect each source's current on-screen span with the visible
     * [0, w) screen area -- handles every offset (0, -w/+w, any
     * intermediate value, and a clamped overshoot past either end)
     * uniformly, with no separate forward/backward special-casing: the two
     * spans are always exactly adjacent (to_offset is always +-w), so
     * together they cover the full destination row with no gap or overlap.
     * At v==0, from_start=0/from_end=w and to_start=to_end (nothing) --
     * i.e. the outgoing source reproduces byte-for-byte; at v==-to_offset
     * the reverse holds for the incoming source -- the exact boundary
     * invariants TARGET ARCHITECTURE section D calls for. */
    int32_t from_start = from_x < 0 ? 0 : from_x;
    int32_t from_end = (from_x + w > w) ? w : from_x + w;
    if (from_end < from_start) from_end = from_start;
    int32_t to_start = to_x < 0 ? 0 : to_x;
    int32_t to_end = (to_x + w > w) ? w : to_x + w;
    if (to_end < to_start) to_end = to_start;

    /* RGB565 was validated in begin(), so every pixel is exactly two bytes
     * and neither source has an indexed-color palette prefix. Compute each
     * visible span's first-row address once and walk all three buffers by
     * their independent strides. The old loop called lv_draw_buf_goto_xy()
     * for each visible source on every scanline, repeating format-size and
     * y*stride calculations up to twice per row even though only y changed. */
    const size_t from_copy_bytes = (size_t) (from_end - from_start) * 2U;
    const size_t to_copy_bytes = (size_t) (to_end - to_start) * 2U;
    const uint8_t * from_row = (const uint8_t *) compositor_from->data;
    const uint8_t * to_row = (const uint8_t *) compositor_to->data;
    if (from_copy_bytes) from_row += (size_t) (from_start - from_x) * 2U;
    if (to_copy_bytes) to_row += (size_t) (to_start - to_x) * 2U;
    uint8_t * dest_row = dest_page;

#ifdef UI_PERF_TRACE
    /* begin() already proves stride*height is inside data_size. These
     * tighter frame-specific checks document that the selected horizontal
     * spans also end within every row before the raw-pointer fast path is
     * entered. */
    LV_ASSERT(!from_copy_bytes ||
              ((uint64_t) (uint32_t) (from_start - from_x) * 2ULL + from_copy_bytes <=
               compositor_from->header.stride));
    LV_ASSERT(!to_copy_bytes ||
              ((uint64_t) (uint32_t) (to_start - to_x) * 2ULL + to_copy_bytes <=
               compositor_to->header.stride));
    LV_ASSERT(!from_copy_bytes ||
              ((uint64_t) (uint32_t) from_start * 2ULL + from_copy_bytes <= compositor_fb_stride));
    LV_ASSERT(!to_copy_bytes ||
              ((uint64_t) (uint32_t) to_start * 2ULL + to_copy_bytes <= compositor_fb_stride));
#endif

    /* Every row, y=0 through height-1 -- no top/bottom margin is ever
     * skipped or separately re-stamped (see this module's own header
     * comment on why: both `from` and `to` are already complete, full-
     * screen frames with the persistent status bar/home-indicator content
     * baked in, so compositing the whole frame uniformly is what makes
     * those bars slide correctly instead of staying stationary). */
    for (int32_t y = 0; y < compositor_height; y++) {
        if (from_copy_bytes)
            memcpy(dest_row + (size_t) from_start * 2U, from_row, from_copy_bytes);
        if (to_copy_bytes)
            memcpy(dest_row + (size_t) to_start * 2U, to_row, to_copy_bytes);
        from_row += compositor_from->header.stride;
        to_row += compositor_to->header.stride;
        dest_row += compositor_fb_stride;
    }

    /* Only commit the just-composed page to the screen once every row has
     * been written. A present failure means the just-composed frame was
     * never actually shown -- the physical display is now in an unknown
     * relationship to this module's own from/to sources, and continuing to
     * composite further frames on top of that could commit GUI-level
     * navigation state (see gui.c's slide_transition_anim_x_cb()) to a
     * screen the display never actually finished transitioning to. Ending
     * the session immediately (restoring LVGL invalidation and re-
     * synchronizing buf_act) and reporting failure to the caller is the
     * only choice that can't leave the driver/LVGL and GUI-level state
     * disagreeing about what's on screen -- gui.c's own caller is
     * responsible for reverting whatever navigation-stack/screen-load
     * decision it was about to make. Never observed on this hardware in
     * testing, but treated as a hard stop rather than a dropped frame. */
    if (!lv_linux_fbdev_present_external_page(disp)) {
#ifdef UI_PERF_TRACE
        compositor_perf_present_failures++;
        printf("PERF compositor present_failed v=%d\n", v);
#endif
        transition_compositor_end();
        return false;
    }
#ifdef UI_PERF_TRACE
    uint64_t perf_frame_us = compositor_perf_now_us() - perf_frame_start_us;
    compositor_perf_frame_total_us += perf_frame_us;
    if (perf_frame_us > compositor_perf_frame_max_us) compositor_perf_frame_max_us = perf_frame_us;
    compositor_perf_frame_count++;
#endif
    return true;
#endif /* COMPOSITOR_DISABLED || !LV_USE_LINUX_FBDEV */
}

void transition_compositor_end(void) {
    if (!compositor_active) return;
    compositor_active = false;
#if !(COMPOSITOR_DISABLED) && LV_USE_LINUX_FBDEV
    lv_display_t * disp = lv_display_get_default();
    /* Ends fbdev's own external-composition session FIRST -- this is what
     * re-synchronizes LVGL's next render target (buf_act) with whichever
     * physical page is actually inactive right now, so it must happen
     * before invalidation comes back on and anything can render again. */
    lv_linux_fbdev_end_external_composition(disp);
    lv_display_enable_invalidation(disp, true);
#ifdef UI_PERF_TRACE
    printf("PERF compositor end frames=%u avg_us=%llu max_us=%llu present_failures=%u\n",
           compositor_perf_frame_count,
           (unsigned long long) (compositor_perf_frame_count ? compositor_perf_frame_total_us / compositor_perf_frame_count : 0),
           (unsigned long long) compositor_perf_frame_max_us,
           compositor_perf_present_failures);
#endif
    /* Catches up anything that changed elsewhere while invalidation was
     * disabled (status bar clock, etc.) on top of the transition's own
     * already-correct final on-screen frame -- the next lv_timer_handler()
     * tick (moments away, same main loop) does the actual redraw; no need
     * to force one synchronously here. */
    lv_obj_invalidate(lv_screen_active());
    lv_obj_invalidate(lv_layer_top());
    lv_obj_invalidate(lv_layer_sys());
#endif
}

bool transition_compositor_is_active(void) {
    return compositor_active;
}
