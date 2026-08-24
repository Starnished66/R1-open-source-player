#ifndef GUI_TEXT_INPUT_H
#define GUI_TEXT_INPUT_H

#include <stdbool.h>
#include "lvgl/lvgl.h"

typedef void (*text_entry_done_cb_t)(const char * text, void * user_data);

bool gui_text_input_init(void);

void show_text_entry(const char * title, const char * initial_text, bool is_password, bool numeric,
                     text_entry_done_cb_t done_cb, void * user_data);

void t9_keypad_attach(lv_obj_t * target_screen, lv_obj_t * textarea_parent, int32_t textarea_x, int32_t textarea_y,
                      int32_t textarea_width);
void t9_keypad_release(void);
void t9_keypad_dismiss_only(void);
bool t9_keypad_is_inline_active(void);
const char * t9_keypad_get_text(void);
int32_t t9_keypad_get_grid_y(void);

lv_obj_t * gui_text_input_get_screen(void);

#endif /* GUI_TEXT_INPUT_H */
