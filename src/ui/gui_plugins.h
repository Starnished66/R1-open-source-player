#pragma once
#include <lvgl/lvgl.h>
#include <stdint.h>
#include <stdbool.h>
#include "plugin_manager.h"

void gui_plugins_init(void);
/* Deletes every pool screen this module owns so gui_reload.c's in-process
 * UI reload can call gui_plugins_init() again from a clean slate. */
void gui_plugins_teardown(void);
void configure_scrolling_row_label(lv_obj_t * label, int32_t width);
