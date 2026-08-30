#ifndef LAUNCHER_LAYOUT_H
#define LAUNCHER_LAYOUT_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool list_mode;
    int32_t row_gap, height, width;
    bool has_bg_color; uint32_t bg_color;
    bool has_text_color; uint32_t text_color;
    bool has_radius; int32_t radius;
    char align[8], text_size[8];
    bool has_accessory, accessory, has_icon, icon;
} launcher_menu_layout_t;

typedef struct {
    launcher_menu_layout_t music, stream_media, wireless;
} launcher_layout_config_t;

extern launcher_layout_config_t launcher_layout_config;

#endif
