#pragma once
#include <lvgl/lvgl.h>
#include <stdint.h>
#include <stdbool.h>

typedef uint32_t gui_busy_handle_t;

void gui_notifications_init(void);

void show_error_toast(const char * msg);
void show_info_toast(const char * msg);

gui_busy_handle_t gui_busy_show(const char * title, const char * msg);
void gui_busy_set_progress(gui_busy_handle_t handle, int percent);
void gui_busy_hide(gui_busy_handle_t handle);
static inline void gui_busy_dismiss(gui_busy_handle_t handle) { gui_busy_hide(handle); }
lv_obj_t * gui_busy_get_screen(void);
