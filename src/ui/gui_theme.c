#include "gui_theme.h"
#include "gui.h"
#include "settings.h"
#include "screen_builders.h"
#include "fallback_font.h"
#include <stdio.h>

static lv_style_t style_accent;
static lv_style_t style_muted_text;

lv_style_t * gui_theme_accent_style(void) { return &style_accent; }
lv_style_t * gui_theme_muted_text_style(void) { return &style_muted_text; }

static const lv_font_t * ui_size_16 = &lv_font_montserrat_16;
static const lv_font_t * ui_size_20 = &lv_font_montserrat_20;
static const lv_font_t * ui_size_22 = &lv_font_montserrat_22;
static const lv_font_t * ui_size_28 = &lv_font_montserrat_28;

const uint32_t accent_palette[ACCENT_PALETTE_COUNT] = {
    0x2196F3, /* blue (default) */
    0x4CAF50, /* green */
    0xF44336, /* red */
    0xFF9800, /* orange */
    0x9C27B0, /* purple */
    0x009688, /* teal */
    0xE91E63, /* pink */
    0xE0E0E0, /* light gray */
    0xFFEB3B, /* yellow */
    0x00BCD4, /* cyan */
    0x3F51B5, /* indigo */
    0xFFC107, /* amber */
    0xCDDC39, /* lime */
    0x795548, /* brown */
    0x607D8B, /* blue gray */
    0xFFFFFF, /* white */
};

static lv_obj_t * accent_swatches[ACCENT_PALETTE_COUNT];

extern player_settings_t current_settings;
extern void settings_save(const player_settings_t * s);
extern void player_transition_mark_dirty(void);

void apply_font_size_tier(int tier) {
    switch (tier) {
        case 1: /* Medium */
            ui_size_16 = &lv_font_montserrat_20;
            ui_size_20 = &lv_font_montserrat_24;
            ui_size_22 = &lv_font_montserrat_26;
            ui_size_28 = &lv_font_montserrat_32;
            break;
        case 2: /* Large */
            ui_size_16 = &lv_font_montserrat_24;
            ui_size_20 = &lv_font_montserrat_30;
            ui_size_22 = &lv_font_montserrat_34;
            ui_size_28 = &lv_font_montserrat_40;
            break;
        default: /* Small (0) */
            ui_size_16 = &lv_font_montserrat_16;
            ui_size_20 = &lv_font_montserrat_20;
            ui_size_22 = &lv_font_montserrat_22;
            ui_size_28 = &lv_font_montserrat_28;
            break;
    }
}

const lv_font_t * gui_theme_font(gui_font_role_t role) {
    switch (role) {
        case GUI_FONT_ROLE_TITLE:   return ui_size_28;
        case GUI_FONT_ROLE_ROW:     return ui_size_22;
        case GUI_FONT_ROLE_BODY:    return ui_size_20;
        case GUI_FONT_ROLE_SUBTEXT: return ui_size_16;
        case GUI_FONT_ROLE_STATUS:  return ui_size_16;
        default:                    return ui_size_20;
    }
}

lv_color_t accent_lv_color(void) {
    return lv_color_hex(current_settings.accent_color);
}

void apply_accent_color(uint32_t rgb) {
    gui_theme_apply_accent(rgb);
}

void gui_theme_apply_accent(uint32_t rgb) {
    current_settings.accent_color = rgb;
    lv_style_set_bg_color(&style_accent, lv_color_hex(rgb));
    lv_style_set_text_color(&style_accent, lv_color_hex(rgb));
    lv_style_set_bg_image_recolor(&style_accent, lv_color_hex(rgb));
    lv_style_set_bg_image_recolor_opa(&style_accent, LV_OPA_COVER);
    lv_style_set_image_recolor(&style_accent, lv_color_hex(rgb));
    lv_style_set_image_recolor_opa(&style_accent, LV_OPA_80);
    lv_obj_report_style_change(&style_accent);
    settings_save(&current_settings);
    player_transition_mark_dirty();
}

void accent_swatch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    uint32_t rgb = (uint32_t) (intptr_t) lv_event_get_user_data(e);
    gui_theme_apply_accent(rgb);

    for (size_t i = 0; i < ACCENT_PALETTE_COUNT; i++) {
        lv_obj_set_style_border_width(accent_swatches[i], accent_palette[i] == rgb ? 4 : 0, 0);
    }
}

void gui_theme_init(void) {
    apply_font_size_tier(current_settings.font_size_tier);

    lv_style_init(&style_accent);
    lv_style_set_bg_color(&style_accent, accent_lv_color());
    lv_style_set_text_color(&style_accent, accent_lv_color());
    lv_style_set_bg_image_recolor(&style_accent, accent_lv_color());
    lv_style_set_bg_image_recolor_opa(&style_accent, LV_OPA_COVER);
    lv_style_set_image_recolor(&style_accent, accent_lv_color());
    lv_style_set_image_recolor_opa(&style_accent, LV_OPA_80);

    lv_style_init(&style_muted_text);
    lv_style_set_text_color(&style_muted_text, lv_color_make(220, 220, 220));

    fallback_font_init_early(current_settings.font_size_tier, current_settings.lyrics_font_size_tier);
    screen_builders_init_list_row_style();
}


void gui_theme_register_accent_swatch(int index, lv_obj_t * swatch) {
    if (index >= 0 && index < ACCENT_PALETTE_COUNT) {
        accent_swatches[index] = swatch;
    }
}
