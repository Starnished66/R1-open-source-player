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

extern lv_style_t style_accent;
extern lv_style_t style_muted_text;

extern const uint32_t accent_palette[ACCENT_PALETTE_COUNT];
extern lv_obj_t * accent_swatches[ACCENT_PALETTE_COUNT];

extern const lv_font_t * ui_size_16;
extern const lv_font_t * ui_size_20;
extern const lv_font_t * ui_size_22;
extern const lv_font_t * ui_size_28;

void gui_theme_init(void);
const lv_font_t * gui_theme_font(gui_font_role_t role);
lv_color_t accent_lv_color(void);
void apply_accent_color(uint32_t rgb);
void gui_theme_apply_accent(uint32_t rgb);
void apply_font_size_tier(int tier);
void accent_swatch_event_cb(lv_event_t * e);
