#pragma once
#include <lvgl/lvgl.h>
#include <stdint.h>
#include <stdbool.h>
#include "plugin_manager.h"

void gui_plugins_init(void);
void configure_scrolling_row_label(lv_obj_t * label, int32_t width);
