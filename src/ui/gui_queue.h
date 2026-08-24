#pragma once
#include <lvgl/lvgl.h>
#include <stdbool.h>

extern lv_obj_t * queue_screen;
extern lv_obj_t * queue_list;

void gui_queue_init(void);
void open_queue_screen(void);
void populate_queue_screen(void);
void open_song_context_menu(const char * path);
void hide_song_context_menu_popup(void);
