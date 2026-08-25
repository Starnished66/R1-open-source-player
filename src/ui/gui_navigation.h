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
} slide_transition_ctx_t;

int gui_navigation_get_depth(void);
lv_obj_t * gui_navigation_get_top_screen(void);
lv_obj_t * gui_navigation_get_screen_at(int index);
bool gui_navigation_is_top(lv_obj_t * screen);
void gui_navigation_remove_screen_instances(lv_obj_t ** screens, int count);
void gui_navigation_replace_top(lv_obj_t * new_screen);
void gui_navigation_pop_to_depth(int target_depth);
bool player_transition_cache_is_dirty(void);

void gui_navigation_init(void);
void nav_push(lv_obj_t * scr);
void nav_pop(void);
void nav_remove_stack_slot(int index);
void nav_reset_to_home(void);
void generic_back_cb(lv_event_t * e);
void enable_gesture_bubble_recursive(lv_obj_t * obj);
void finalize_screen_navigation(lv_obj_t * scr);

slide_transition_ctx_t * begin_slide_transition(lv_obj_t * to_scr, bool forward);
void slide_transition_anim_x_cb(void * var, int32_t v);
void slide_transition_done_cb(lv_anim_t * a);
void slide_transition_cancel(slide_transition_ctx_t ** pctx);
void player_transition_cache_async_cb(void * unused);
void player_transition_mark_dirty(void);
void register_static_snapshot(int index, lv_obj_t * scr);

void full_redraw_async_cb(void * unused);
