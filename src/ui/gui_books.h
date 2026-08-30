#ifndef GUI_BOOKS_H
#define GUI_BOOKS_H

#include <stdbool.h>
#include "lvgl/lvgl.h"

bool gui_books_init(void);
/* Deletes every screen this module owns so gui_reload.c's in-process UI
 * reload can call gui_books_init() again from a clean slate. */
void gui_books_teardown(void);
void gui_books_rescan(void);
void gui_books_show(void);
lv_obj_t * gui_books_get_screen(void);
void gui_books_home_tile_cb(lv_event_t * e);

#endif /* GUI_BOOKS_H */
