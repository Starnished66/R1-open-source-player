#pragma once
#include <lvgl/lvgl.h>
#include <stdint.h>
#include <stdbool.h>

typedef uint32_t gui_busy_handle_t;

void gui_notifications_init(void);

void show_error_toast(const char * msg);
void show_info_toast(const char * msg);
void show_info_toast_for(const char * msg, uint32_t duration_ms);
bool gui_notifications_toast_visible(void);

gui_busy_handle_t gui_busy_show(const char * title, const char * msg);
void gui_busy_set_progress(gui_busy_handle_t handle, int percent);
/* Status line under the progress bar; empty or NULL hides it. */
void gui_busy_set_detail(gui_busy_handle_t handle, const char * text);
void gui_busy_hide(gui_busy_handle_t handle);
static inline void gui_busy_dismiss(gui_busy_handle_t handle) { gui_busy_hide(handle); }
lv_obj_t * gui_busy_get_screen(void);
