#ifndef TOPBAR_ICON_LAYOUT_H
#define TOPBAR_ICON_LAYOUT_H

#include "lvgl.h"
#include "board_config.h"

/* Solid artwork bounds (alpha >= 128), excluding transparent sprite padding.
 * Keep the object itself centered on the status band: moving it with style
 * translate_y also moves the reference used to align the next status icon. */
static inline void topbar_icon_layout(lv_obj_t *image, int left, int top,
                                      int width, int height, int band_height) {
    int target = BOARD_SCALE_PX(22);
    int padding = BOARD_SCALE_PX(4);
    int scale = (target * LV_SCALE_NONE + height / 2) / height;
    int visible_width = (width * scale + LV_SCALE_NONE / 2) / LV_SCALE_NONE;
    lv_image_set_inner_align(image, LV_IMAGE_ALIGN_TOP_LEFT);
    lv_image_set_pivot(image, 0, 0);
    lv_image_set_scale(image, scale);
    lv_obj_set_size(image, visible_width + 2 * padding, band_height);
    lv_obj_set_style_translate_y(image, 0, 0);
    int x = padding - (left * scale + LV_SCALE_NONE / 2) / LV_SCALE_NONE;
    int y = (band_height - target) / 2 - (top * scale + LV_SCALE_NONE / 2) / LV_SCALE_NONE;
    if (lv_image_get_offset_x(image) != x) lv_image_set_offset_x(image, x);
    if (lv_image_get_offset_y(image) != y) lv_image_set_offset_y(image, y);
}

#endif
