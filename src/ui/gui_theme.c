#include "gui_theme.h"
#include "gui.h"
#include "settings.h"
#include "screen_builders.h"
#include "fallback_font.h"
#include <stdio.h>

static lv_style_t style_accent;
static lv_style_t style_accent_knob;

lv_style_t * gui_theme_accent_style(void) { return &style_accent; }
lv_style_t * gui_theme_accent_knob_style(void) { return &style_accent_knob; }
lv_style_t * gui_theme_muted_text_style(void) { return &style_theme_text_muted; }

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
extern void refresh_play_btn_icon(void);

const lv_font_t * gui_theme_font(gui_font_role_t role) {
    switch (role) {
        case GUI_FONT_ROLE_TITLE:   return &app_font_28;
        case GUI_FONT_ROLE_ROW:     return &app_font_22;
        case GUI_FONT_ROLE_BODY:    return &app_font_20;
        case GUI_FONT_ROLE_SUBTEXT: return &app_font_16;
        case GUI_FONT_ROLE_STATUS:  return &app_font_16;
        default:                    return &app_font_20;
    }
}

lv_color_t accent_lv_color(void) {
    return lv_color_hex(current_settings.accent_color);
}

void gui_theme_update_surface_contrast(void) {
    lv_style_value_t text, row, card;
    if (lv_style_get_prop(&style_theme_text_primary, LV_STYLE_TEXT_COLOR, &text) != LV_STYLE_RES_FOUND ||
        lv_style_get_prop(&list_row_style, LV_STYLE_BG_COLOR, &row) != LV_STYLE_RES_FOUND ||
        lv_style_get_prop(&style_theme_card_bg, LV_STYLE_BG_COLOR, &card) != LV_STYLE_RES_FOUND) return;
    /* Contrast follows the plugin's own foreground, so a light palette
     * darkens on press instead of flashing a hardcoded charcoal surface. */
    lv_style_set_bg_color(&list_row_pressed_style, lv_color_mix(text.color, row.color, 32));
    lv_style_set_border_color(&style_theme_card_bg, lv_color_mix(text.color, card.color, 24));
    lv_obj_report_style_change(&list_row_pressed_style);
    lv_obj_report_style_change(&style_theme_card_bg);
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

    lv_style_set_bg_color(&style_accent_knob, lv_color_hex(rgb));
    lv_obj_report_style_change(&style_accent_knob);

    settings_save(&current_settings);
    player_transition_mark_dirty();
    /* Play/pause art is a white disc with a baked-in cyan glyph -- LVGL
     * image_recolor would tint the disc too, so the glyph is rewritten in
     * decoded pixels (see refresh_play_btn_icon()). */
    refresh_play_btn_icon();
}

void accent_swatch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    uint32_t rgb = (uint32_t) (intptr_t) lv_event_get_user_data(e);
    gui_theme_apply_accent(rgb);

    for (size_t i = 0; i < ACCENT_PALETTE_COUNT; i++) {
        lv_obj_set_style_border_width(accent_swatches[i], accent_palette[i] == rgb ? 4 : 0, 0);
    }
}

/* Shared by gui_theme_init() (real boot) and gui_theme_reload_styles()
 * (gui_reload.c's in-process UI reload) -- everything EXCEPT
 * fallback_font_init_early(), which must never run a second time (see
 * gui_theme_reload_styles()'s own comment). */
static void init_style_objects(void) {
    /* gui_reload.c's in-process UI reload calls this a second (or Nth) time,
     * and lv_style_init() on a style that already has properties leaks the
     * old values_and_props allocation instead of freeing it (LVGL's own
     * header comment on lv_style_init()). Skip the reset on the very first
     * call, when these are still freshly zero-initialized statics. */
    static bool already_initialized = false;
    if (already_initialized) {
        lv_style_reset(&style_accent);
        lv_style_reset(&style_accent_knob);
    }
    already_initialized = true;

    lv_style_init(&style_accent);
    lv_style_set_bg_color(&style_accent, accent_lv_color());
    lv_style_set_text_color(&style_accent, accent_lv_color());
    lv_style_set_bg_image_recolor(&style_accent, accent_lv_color());
    lv_style_set_bg_image_recolor_opa(&style_accent, LV_OPA_COVER);
    lv_style_set_image_recolor(&style_accent, accent_lv_color());
    lv_style_set_image_recolor_opa(&style_accent, LV_OPA_80);

    lv_style_init(&style_accent_knob);
    lv_style_set_bg_color(&style_accent_knob, accent_lv_color());
    lv_style_set_bg_opa(&style_accent_knob, LV_OPA_COVER);
    lv_style_set_border_color(&style_accent_knob, lv_color_white());
    lv_style_set_border_width(&style_accent_knob, SLIDER_KNOB_BORDER_WIDTH);
    lv_style_set_border_opa(&style_accent_knob, LV_OPA_COVER);
    lv_style_set_radius(&style_accent_knob, LV_RADIUS_CIRCLE);
    lv_style_set_pad_all(&style_accent_knob, SLIDER_KNOB_PAD);

    screen_builders_init_list_row_style();
}

void gui_theme_init(void) {
    init_style_objects();
    fallback_font_init_early(current_settings.font_size_tier, current_settings.lyrics_font_size_tier);
    screen_builders_refresh_font_geometry(NULL);
}

/* For gui_reload.c's in-process UI reload -- everything gui_theme_init()
 * does EXCEPT fallback_font_init_early(). That function is boot-only, by
 * design: it always rebuilds every app_font_* slot with include_fallbacks
 * = false (src/ui/fallback_font.c), deferring the CJK/Korean/Thai fallback
 * face load to run later, once, in the background
 * (fallback_font_schedule_deferred_load()/fallback_font_load_now()). A
 * reload re-running it would silently drop that already-loaded fallback
 * chain (breaking non-Latin glyphs for the rest of the session) AND leak
 * the fallback faces already in s_loaded_faces[], since fallback_font_init_
 * early() replaces that table without freeing what it's replacing -- real
 * memory, not just a stale pointer, since these are loaded TTF/OTF font
 * files. A theme/icon reload has no reason to touch fonts at all, so this
 * just skips that call entirely rather than trying to reconstruct
 * fallback_font.c's own s_fallback_loaded state from outside it. */
void gui_theme_reload_styles(void) {
    init_style_objects();
    screen_builders_refresh_font_geometry(NULL);
}


void gui_theme_register_accent_swatch(int index, lv_obj_t * swatch) {
    if (index >= 0 && index < ACCENT_PALETTE_COUNT) {
        accent_swatches[index] = swatch;
    }
}
