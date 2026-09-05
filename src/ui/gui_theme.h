#pragma once
#include <lvgl/lvgl.h>
#include <stdint.h>
#include <stdbool.h>

#define ACCENT_PALETTE_COUNT 16
/* Native defaults only: live surfaces/text continue to use mutable styles
 * so plugin palettes (including light themes) remain authoritative. */
#define GUI_COLOR_SCREEN 0x121418
#define GUI_COLOR_ROW 0x1C2026
#define GUI_COLOR_PANEL 0x252A32
#define GUI_COLOR_PRESSED 0x303743
#define GUI_COLOR_PRIMARY 0xF1F3F5
#define GUI_COLOR_SECONDARY 0xA8B0BC
#define GUI_COLOR_BORDER 0x343B46
#define GUI_ROW_GAP 8
#define GUI_TEXT_INSET 24
#define GUI_SETTINGS_ROW_HEIGHT 112
#define GUI_MUSIC_ROW_HEIGHT 112
/* Shared native-painted track thickness -- every slider except Player's
 * own progress_slider, which stays at a hardcoded 440x12 to match its
 * fixed-size progress_bg.png/progress.png art (gui_player.c's own comment
 * at that call site explains why; confirmed via the real asset files,
 * both exactly 440x12 -- LVGL doesn't stretch a bg_image to fit). */
#define SLIDER_TRACK_HEIGHT 16
/* Knob diameter is track height plus pad on both sides -- lv_slider.c's
 * position_knob() ignores LV_PART_KNOB width/height. 32px sits just above
 * the 16px track; 4px inward white border leaves a 24px accent disc. */
#define SLIDER_KNOB_SIZE 32
#define SLIDER_KNOB_BORDER_WIDTH 4
#define SLIDER_KNOB_PAD ((SLIDER_KNOB_SIZE - SLIDER_TRACK_HEIGHT) / 2)

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
/* For gui_reload.c's in-process UI reload -- see its own comment in
 * gui_theme.c for why this must never call fallback_font_init_early(). */
void gui_theme_reload_styles(void);
lv_style_t * gui_theme_accent_style(void);
lv_style_t * gui_theme_accent_knob_style(void);
lv_style_t * gui_theme_muted_text_style(void);
const lv_font_t * gui_theme_font(gui_font_role_t role);
lv_color_t accent_lv_color(void);
void apply_accent_color(uint32_t rgb);
void gui_theme_apply_accent(uint32_t rgb);
void gui_theme_update_surface_contrast(void);
void accent_swatch_event_cb(lv_event_t * e);
