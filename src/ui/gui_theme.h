#pragma once
#include <lvgl/lvgl.h>
#include <stdint.h>
#include <stdbool.h>

#define ACCENT_PALETTE_COUNT 16

typedef enum {
    GUI_FONT_ROLE_TITLE,
    GUI_FONT_ROLE_ROW,
    GUI_FONT_ROLE_BODY,
    GUI_FONT_ROLE_SUBTEXT,
    GUI_FONT_ROLE_STATUS
} gui_font_role_t;

extern const uint32_t accent_palette[ACCENT_PALETTE_COUNT];
void gui_theme_register_accent_swatch(int index, lv_obj_t * swatch);


void gui_theme_init(void);
lv_style_t * gui_theme_accent_style(void);
lv_style_t * gui_theme_muted_text_style(void);
const lv_font_t * gui_theme_font(gui_font_role_t role);
lv_color_t accent_lv_color(void);
void apply_accent_color(uint32_t rgb);
void gui_theme_apply_accent(uint32_t rgb);
void apply_font_size_tier(int tier);
void accent_swatch_event_cb(lv_event_t * e);
