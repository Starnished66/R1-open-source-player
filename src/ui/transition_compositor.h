#ifndef TRANSITION_COMPOSITOR_H
#define TRANSITION_COMPOSITOR_H

#include "lvgl.h"

/* Direct RGB565 two-panel transition compositor -- TRANSITION_PERFORMANCE_
 * PLAN.md Phase 3. Renders the sliding-screen transition (see gui.c's
 * slide_transition_ctx_t/begin_slide_transition()) with plain scanline
 * memcpy()s straight into the inactive physical framebuffer page, panning
 * to it once each frame is fully composed, instead of asking LVGL's
 * software renderer to redraw two moving lv_image objects on every single
 * animation frame. Only usable when the fbdev driver's own pan-based double
 * buffering is active (see lv_linux_fbdev_begin_external_composition()) --
 * gui.c always keeps its existing LVGL-image-object overlay as the fallback
 * for when it isn't (host/SDL builds, or any future display driver), or
 * when any step here fails.
 *
 * Both from/to must be COMPLETE, full-screen, opaque RGB565 frames -- the
 * whole visible display, not just a bare screen-object snapshot. Neither
 * gui.c's live framebuffer copy (see lv_linux_fbdev_get_active_page()) nor
 * a flattened target-screen render (see build_flattened_transition_frame()
 * in gui.c) omit the persistent status bar / home-indicator content the
 * way a bare lv_snapshot_take(some_screen, ...) would -- this module has no
 * concept of "margins" to separately preserve; every row of both sources
 * slides together as one image, which is the only way either bar actually
 * moves correctly instead of staying stationary or getting blanked. */

/* Attempts to switch an already-built slide transition into direct-
 * framebuffer compositing mode. from/to must each be a full-screen,
 * complete (persistent-layer content included), opaque RGB565 draw buffer,
 * and MUST be independently owned -- never a live alias of real
 * framebuffer memory. to_offset is the same +width/-width convention
 * slide_transition_anim_x_cb() uses (the incoming frame's position relative
 * to the outgoing one).
 *
 * Real-device correctness requirement: the two physical framebuffer pages
 * ping-pong every single frame (this module writes into whichever is
 * currently INACTIVE, then presents it), so a live-aliased `from` would
 * become this module's OWN write target again a couple of frames later --
 * reading it as a source for one frame while it was simultaneously the
 * destination of a slightly different recent one is a genuine overlapping-
 * memcpy hazard, not just a staleness question. gui.c is responsible for
 * always duplicating (never aliasing) whenever this compositor might be
 * used -- see transition_compositor_available().
 *
 * On success: takes ownership of the fbdev driver's page presentation
 * (lv_linux_fbdev_begin_external_composition()) and disables LVGL
 * invalidation (lv_display_enable_invalidation()) for the duration -- the
 * caller must not create any LVGL overlay/image objects for this
 * transition at all (nothing would ever be drawn while this is active) and
 * must call transition_compositor_end() exactly once later, committed or
 * cancelled either way, before doing anything else that expects normal
 * LVGL rendering to work.
 *
 * Returns false (no state changed at all -- safe to just keep using the
 * existing LVGL-object overlay unmodified) if direct framebuffer access
 * isn't available on this display, a compositor session is already
 * active, or either buffer isn't a plain, full-screen, opaque RGB565
 * buffer of the expected shape. */
bool transition_compositor_begin(const lv_draw_buf_t * from, const lv_draw_buf_t * to, int32_t to_offset);

/* True if this display could potentially use the compositor at all (pan-
 * based double buffering active) -- doesn't guarantee a later
 * transition_compositor_begin() call will actually succeed (buffer shape is
 * still checked there), but is cheap and stable enough to call earlier, so
 * gui.c can decide whether its transition source buffers need to be owned
 * copies (see transition_compositor_begin()'s own comment on why a live
 * alias isn't safe here) before it has them in hand yet. */
bool transition_compositor_available(void);

/* Composes and presents exactly one frame at horizontal offset v (the
 * outgoing frame effectively at x=v, the incoming one at x=v+to_offset --
 * same convention slide_transition_anim_x_cb() uses). Every row from y=0
 * through height-1 is recomposited from `from`/`to` on every call -- no
 * row is ever skipped, re-stamped from a fixed source, or left untouched,
 * so the persistent status bar / home-indicator content (already baked
 * into both full-frame sources) slides exactly like the rest of the
 * screen.
 *
 * Returns false if no compositor session is currently active (transition_
 * compositor_begin() never succeeded, or transition_compositor_end()
 * already ran -- a harmless no-op call, e.g. a stray late animation
 * tick), OR if presenting this frame failed for real. The two cases are
 * NOT the same to the caller: a hard presentation failure means the
 * session has already been torn down internally (transition_compositor_
 * end() runs automatically -- LVGL invalidation restored, buf_act re-
 * synced) before this returns, and the physical display is now showing an
 * unknown intermediate frame with no further compositor frames coming.
 * The caller (gui.c's slide_transition_anim_x_cb()) MUST treat any false
 * return as "this transition cannot continue or be committed" and revert
 * its own ctx/navigation-stack bookkeeping accordingly -- continuing to
 * treat the gesture as live, or committing to the destination screen,
 * would leave GUI-level state disagreeing with what's actually on screen. */
bool transition_compositor_frame(int32_t v);

/* Vertical opaque-overlay variant used by the quick drawer. On opening,
 * the current physical display is copied into an owned stationary base.
 * That clean underlay can be retained while the drawer is open and reused
 * when closing; capturing the then-current framebuffer on close would
 * capture the full-screen drawer itself and reveal a duplicate drawer
 * underneath it. Set reuse_saved_base for a close/partially-open motion.
 * fixed_top_rows are refreshed from the current display at every begin so
 * the fixed status bar does not become stale while the drawer is open. */
bool transition_compositor_begin_vertical_overlay(const lv_draw_buf_t * overlay,
                                                  int32_t fixed_top_rows,
                                                  bool reuse_saved_base);
bool transition_compositor_vertical_overlay_frame(int32_t y);
/* Releases the retained drawer underlay once the drawer is fully closed. */
void transition_compositor_discard_vertical_base(void);

/* Re-enables LVGL invalidation, ends the fbdev driver's external-
 * composition session (synchronizing LVGL's own next render target with
 * the physically-visible page -- see lv_linux_fbdev_end_external_
 * composition()), and invalidates the active screen plus both persistent
 * layers (lv_layer_top()/lv_layer_sys()) so the very next
 * lv_timer_handler() tick redraws everything normally -- self-healing
 * whatever else may have changed (status bar clock, etc.) while
 * invalidation was off, on top of the transition's own already-correct
 * final on-screen frame. No-op if no compositor session is active. */
void transition_compositor_end(void);

/* True while a compositor session is active (between a successful begin()
 * and its matching end()) -- gui.c checks this to decide whether a given
 * transition's per-frame position update should go through
 * transition_compositor_frame() instead of the plain lv_obj_set_x() pair. */
bool transition_compositor_is_active(void);

#endif /* TRANSITION_COMPOSITOR_H */
