#ifndef GUI_LYRICS_H
#define GUI_LYRICS_H
#include <stdbool.h>
#include "lvgl/lvgl.h"

void gui_lyrics_init(void);
lv_obj_t * gui_lyrics_get_screen(void);
lv_obj_t * gui_lyrics_get_font_size_screen(void);

void gui_lyrics_poll_load(void);
void gui_lyrics_poll_backdrop(void);
void gui_lyrics_on_cover_changed(int current_playlist_index);
void gui_lyrics_load_track(int index, const char * path);
void gui_lyrics_open_screen(void);
void lyrics_font_size_settings_row_cb(lv_event_t * e);

#endif

bool gui_lyrics_has_background_work(void);
void gui_lyrics_cancel_background_work(void);
