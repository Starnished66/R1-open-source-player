#pragma once
#include <lvgl/lvgl.h>

void gui_queue_init(void);
void open_queue_screen(void);
void populate_queue_screen(void);
void open_song_context_menu(const char * path);
void hide_song_context_menu_popup(void);
lv_obj_t * gui_queue_get_screen(void);
