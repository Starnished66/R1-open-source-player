#ifndef GUI_LYRICS_H
#define GUI_LYRICS_H
#include <stdbool.h>
#include "lvgl/lvgl.h"

void gui_lyrics_init(void);
/* Deletes both screens this module owns so gui_reload.c's in-process UI
 * reload can call gui_lyrics_init() again from a clean slate. */
void gui_lyrics_teardown(void);
lv_obj_t * gui_lyrics_get_screen(void);
lv_obj_t * gui_lyrics_get_font_size_screen(void);

void gui_lyrics_poll_load(void);
void gui_lyrics_poll_backdrop(void);
void gui_lyrics_on_cover_changed(int current_playlist_index);
void gui_lyrics_load_track(int index, const char * path);
void gui_lyrics_open_screen(void);
void lyrics_font_size_settings_row_cb(lv_event_t * e);

bool gui_lyrics_has_background_work(void);
void gui_lyrics_cancel_background_work(void);
void gui_lyrics_refresh_layout(void);

#endif
