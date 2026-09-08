#pragma once
#include <lvgl/lvgl.h>
#include <stdint.h>
#include <stdbool.h>

#define NAV_STACK_MAX 16
#define STATIC_SNAPSHOT_SCREEN_COUNT 9

typedef struct {
    lv_obj_t * overlay;
    lv_obj_t * img_from;
    lv_obj_t * img_to;
    lv_draw_buf_t * buf_from;
    lv_draw_buf_t * buf_to;
    bool buf_from_owned;
    bool buf_to_owned;
    bool fallback_bands_suppressed;
    bool home_indicator_was_hidden;
    int32_t to_offset;
    lv_obj_t * from_scr;
    lv_obj_t * to_scr;
    bool forward;
    bool commit;
    /* True for a vertical (Y-axis) slide -- e.g. swipe-up-to-Home. Set only
     * by begin_slide_transition_ex(); every existing horizontal caller goes
     * through begin_slide_transition(), which always leaves this false.
     * slide_transition_anim_x_cb() checks this to drive lv_obj_set_y()
     * instead of lv_obj_set_x(), and to skip the direct-framebuffer
     * compositor entirely for a vertical slide (that compositor's fast path
     * is horizontal-only; a vertical slide always uses the LVGL-overlay
     * fallback branch, same as any horizontal slide when the compositor is
     * unavailable). */
    bool vertical;
    /* EXPERIMENTAL (swipe-up-to-Home A/B test, see PROJECT_STATUS notes) --
     * true keeps img_to fixed at (0,0) for the whole gesture instead of
     * moving it with the offset, so the destination reads as already in
     * place, revealed as img_from slides away over it, rather than the two
     * panels sliding together. Only ever set true by home-swipe's own
     * begin_slide_transition_ex() call; every other caller leaves it false. */
    bool reveal;
} slide_transition_ctx_t;

int gui_navigation_get_depth(void);
lv_obj_t * gui_navigation_get_top_screen(void);
lv_obj_t * gui_navigation_get_screen_at(int index);
bool gui_navigation_is_top(lv_obj_t * screen);
void gui_navigation_remove_screen_instances(lv_obj_t ** screens, int count);
void gui_navigation_replace_top(lv_obj_t * new_screen);
void gui_navigation_replace_home(lv_obj_t * old_screen, lv_obj_t * new_screen);
void gui_navigation_replace_static_screen(int snapshot_index, lv_obj_t * old_screen, lv_obj_t * new_screen);
void gui_navigation_pop_to_depth(int target_depth);
bool player_transition_cache_is_dirty(void);

void gui_navigation_init(void);
/* Called by gui_reload.c's in-process UI reload before and after rebuilding screens. */
void gui_navigation_teardown(void);
bool gui_navigation_transition_in_progress(void);
void nav_push(lv_obj_t * scr);
/* Stack-only counterpart to nav_push() above -- pushes onto the nav stack
 * without calling lv_screen_load()/sync_player_topbar_visibility()/
 * sync_home_indicator_visibility(). Used by callers that have their own
 * settle animation in flight and must not load the screen until the
 * animation's completion callback runs (e.g. gui_shell.c interactive
 * player-swipe commit path and slide_transition_done_cb()). */
void nav_push_stack_only(lv_obj_t * scr);
void nav_pop(void);
/* General form of nav_pop() -- see begin_slide_transition_ex()'s own doc
 * comment for what forward/vertical/reveal mean. nav_pop() is a thin
 * wrapper calling this with (false, false, false), i.e. the existing
 * horizontal back-slide every other caller already gets unchanged. */
void nav_pop_ex(bool forward, bool vertical, bool reveal);
/* Stack-only counterpart to nav_pop() -- decrements nav_depth (if > 1)
 * without loading any screen or touching topbar/home-indicator state. Used
 * by a caller with its own settle animation in flight (an interactive
 * back-swipe), matching nav_push_stack_only()'s own reasoning: the actual
 * lv_screen_load() must wait for slide_transition_done_cb(). */
void nav_pop_stack_only(void);
void nav_remove_stack_slot(int index);
void nav_reset_to_home(void);
/* Stack-only counterpart to nav_reset_to_home() -- collapses nav_stack back
 * to Home without loading any screen, for the same reason nav_pop_stack_only()
 * exists (an interactive swipe-up-to-Home settle animation in flight). */
void nav_reset_to_home_stack_only(void);
void generic_back_cb(lv_event_t * e);
void enable_gesture_bubble_recursive(lv_obj_t * obj);
void finalize_screen_navigation(lv_obj_t * scr);

slide_transition_ctx_t * begin_slide_transition(lv_obj_t * to_scr, bool forward);
/* Full form -- vertical=true for a Y-axis slide (e.g. swipe-up-to-Home).
 * begin_slide_transition() is a thin wrapper calling this with
 * vertical=false, reveal=false; every existing horizontal caller is
 * unaffected. For a vertical slide, `forward` still selects which side the
 * destination starts from (true = from the bottom, matching an upward
 * swipe), matching `forward`'s existing horizontal meaning (true =
 * destination starts from the right). reveal=true keeps the destination
 * image fixed in place instead of moving it with the offset -- see
 * slide_transition_ctx_t's own `reveal` field comment. */
slide_transition_ctx_t * begin_slide_transition_ex(lv_obj_t * to_scr, bool forward, bool vertical, bool reveal);
void slide_transition_anim_x_cb(void * var, int32_t v);
void slide_transition_done_cb(lv_anim_t * a);
void slide_transition_cancel(slide_transition_ctx_t ** pctx);
void player_transition_cache_async_cb(void * unused);
void player_transition_mark_dirty(void);
void register_static_snapshot(int index, lv_obj_t * scr);
void gui_navigation_invalidate_font_snapshots(void);
void gui_navigation_invalidate_theme_snapshots(void);

void full_redraw_async_cb(void * unused);
