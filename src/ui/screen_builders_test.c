/* Headless layout/regression tests using real LVGL objects and fonts.
 * Navigation, storage and asset lookup are isolated from device services. */
#include "screen_builders.h"
#include "gui_notifications.h"
#include "gui.h"
#include "gui_plugins.h"
#include "lvgl/src/libs/lodepng/lodepng.h"
#include "settings.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>

lv_font_t app_font_16, app_font_20, app_font_22, app_font_28, app_font_lyrics;
player_settings_t current_settings;
const char * asset_path(const char * path) { (void)path; return NULL; }
const char * asset_path_plain(const char * path) { (void)path; return "/missing-test-asset"; }
void settings_save(const player_settings_t * settings) { (void)settings; }
void player_transition_mark_dirty(void) {}
void refresh_play_btn_icon(void) {}
void nav_push(lv_obj_t * screen) { (void)screen; }
void nav_pop(void) {}
void fallback_font_init_early(int tier, int lyrics) { (void)tier; (void)lyrics; }

static void fonts_reset(void) {
    app_font_16 = lv_font_montserrat_16;
    app_font_20 = lv_font_montserrat_20;
    app_font_22 = lv_font_montserrat_22;
    app_font_28 = lv_font_montserrat_28;
}

static void flush(lv_display_t * display, const lv_area_t * area, uint8_t * pixels) {
    (void)area; (void)pixels;
    lv_display_flush_ready(display);
}
static void noop(lv_event_t * event) { (void)event; }
static int clicks, long_presses, clicked_index;
static void clicked(int index) { ++clicks; clicked_index = index; }
static void long_pressed(int index) { ++long_presses; clicked_index = index; }

static void screenshot(lv_obj_t * screen, const char * name, int height) {
    lv_obj_update_layout(screen);
    lv_draw_buf_t * image = lv_snapshot_take(screen, LV_COLOR_FORMAT_RGB888);
    assert(image);
    unsigned width = image->header.w, rows = image->header.h;
    unsigned char * rgb = malloc(width * rows * 3);
    assert(rgb);
    for (unsigned y = 0; y < rows; ++y) {
        const unsigned char * src = image->data + y * image->header.stride;
        for (unsigned x = 0; x < width; ++x) {
            unsigned char * dst = rgb + (y * width + x) * 3;
            dst[0] = src[x * 3 + 2]; dst[1] = src[x * 3 + 1]; dst[2] = src[x * 3];
        }
    }
    char path[128];
    snprintf(path, sizeof(path), "S:build_ui_test/%s_%d.png", name, height);
    assert(lodepng_encode24_file(path, rgb, width, rows) == 0);
    free(rgb);
    lv_draw_buf_destroy(image);
}

static lv_obj_t * find_row(lv_obj_t * list, const char * title) {
    for (uint32_t i = 0; i < lv_obj_get_child_count(list); ++i) {
        lv_obj_t * row = lv_obj_get_child(list, i);
        if (lv_obj_check_type(row, &lv_label_class) &&
            !lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN) && !strcmp(lv_label_get_text(row), title)) return row;
    }
    return NULL;
}

static int fetch(void * context, int offset, int count, compact_list_page_row_t * rows) {
    (void)context;
    for (int i = 0; i < count; ++i) {
        /* Deliberately omit optional fields: legacy providers remain safe. */
        snprintf(rows[i].label, sizeof(rows[i].label), "Fetched %d", offset + i);
        rows[i].identity = offset + i;
    }
    return count;
}

static void check_layout(int display_height) {
    lv_display_t * display = lv_display_create(480, display_height);
    static uint8_t pixels[480 * 40 * 4];
    lv_display_set_buffers(display, pixels, NULL, sizeof(pixels), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    fonts_reset();
    gui_theme_init();
    assert(ui_list_row_width() == 456 && ui_list_row_width_wide() == 464);
    lv_obj_t * list, * title;
    compact_list_item_t items[1000] = {0};
    for (int i = 0; i < 1000; ++i) items[i].label = "Song";
    items[0] = (compact_list_item_t){ .label = "Same title", .subtitle = "Artist A · Album A" };
    items[1] = (compact_list_item_t){ .label = "Same title\nArtist B · Album B" };
    items[2] = (compact_list_item_t){ .label = "Play All", .is_action = true };
    lv_obj_t * screen = build_compact_list_screen("Long library title", noop, items, 1000,
        clicked, long_pressed, &list, &title, LIST_ROW_WIDTH_WIDE, true, lv_color_hex(0x2196F3));
    lv_screen_load(screen);
    compact_list_set_row_height(list, MUSIC_LIST_ROW_HEIGHT);
    lv_obj_update_layout(screen);
    uint32_t pool_count = lv_obj_get_child_count(list);
    assert(pool_count < 30);
    assert(lv_color_to_u32(lv_obj_get_style_bg_color(screen, 0)) == lv_color_to_u32(lv_color_hex(GUI_COLOR_SCREEN)));
    lv_obj_t * first = find_row(list, "Same title");
    assert(first);
    clicks = long_presses = 0;
    lv_obj_send_event(first, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_send_event(first, LV_EVENT_CLICKED, NULL);
    assert(long_presses == 1 && clicks == 0 && clicked_index == 0);
    lv_obj_send_event(first, LV_EVENT_CLICKED, NULL);
    assert(clicks == 1 && clicked_index == 0);
    lv_obj_t * subtitle = lv_obj_get_child(first, 2);
    assert(!strcmp(lv_label_get_text(subtitle), "Artist A · Album A"));
    assert(lv_obj_get_style_text_font(subtitle, 0) == gui_theme_font(GUI_FONT_ROLE_SUBTEXT));
    assert(lv_obj_get_y(subtitle) >= lv_font_get_line_height(&app_font_22));
    assert(lv_obj_get_y(subtitle) + lv_obj_get_height(subtitle) + lv_obj_get_style_pad_top(first, 0) <= lv_obj_get_height(first));
    screenshot(screen, "library", display_height);
    build_top_right_icon_button(screen, NULL, noop);
    lv_obj_update_layout(screen);
    assert(lv_obj_get_width(title) == 480 - 76 - TITLE_ROW_HEIGHT - 12);

    lv_obj_t * music_host = lv_obj_create(screen);
    lv_obj_set_size(music_host, lv_pct(100), 150);
    lv_obj_align(music_host, LV_ALIGN_TOP_MID, 0, 110);
    lv_obj_set_style_pad_all(music_host, 0, 0);
    lv_obj_t * music_row = build_music_list_row(music_host,
        "72. A very long song title which must remain on one line",
        "Cure for Me (Vintage Culture remix) · New Demons", 112 + GUI_TEXT_INSET + 12);
    lv_obj_update_layout(music_row);
    lv_obj_t * music_title = lv_obj_get_child(music_row, 0);
    lv_obj_t * music_subtitle = lv_obj_get_child(music_row, 1);
    assert(lv_obj_get_height(music_row) >= GUI_MUSIC_ROW_HEIGHT);
    assert(lv_obj_get_y(music_title) + lv_obj_get_height(music_title) < lv_obj_get_y(music_subtitle));
    assert(lv_obj_get_style_text_font(music_subtitle, 0) == gui_theme_font(GUI_FONT_ROLE_BODY));
    lv_obj_t * state = lv_label_create(music_row);
    lv_label_set_text(state, "Upcoming");
    lv_obj_add_style(state, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(state, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_obj_set_width(state, 112);
    lv_obj_set_pos(state, LIST_ROW_WIDTH - GUI_TEXT_INSET - 112, 64);
    lv_obj_set_style_text_align(state, LV_TEXT_ALIGN_RIGHT, 0);
    row_label_apply_bounded_height(state, gui_theme_font(GUI_FONT_ROLE_BODY));
    lv_obj_update_layout(music_row);
    assert(lv_obj_get_x(music_title) + lv_obj_get_width(music_title) + 12 <= lv_obj_get_x(state));
    assert(lv_obj_get_x(music_subtitle) + lv_obj_get_width(music_subtitle) + 12 <= lv_obj_get_x(state));
    assert(lv_obj_get_x(state) + lv_obj_get_width(state) <= LIST_ROW_WIDTH - GUI_TEXT_INSET);
    screenshot(screen, "music_row", display_height);
    lv_obj_delete(music_host);

    compact_list_scroll_to_index(list, 500);
    lv_obj_update_layout(screen);
    compact_list_refresh_visible(list);
    assert(lv_obj_get_child_count(list) == pool_count);
    lv_obj_t * plain = find_row(list, "Song");
    assert(plain && lv_obj_has_flag(lv_obj_get_child(plain, 2), LV_OBJ_FLAG_HIDDEN));
    assert(lv_color_to_u32(lv_obj_get_style_bg_color(plain, 0)) == lv_color_to_u32(lv_color_hex(GUI_COLOR_ROW)));

    /* Oversized metrics model font-tier changes without replacing fonts or
     * reaching font/storage services. Verify geometry, not glyph coverage. */
    app_font_22.line_height = 60;
    app_font_16.line_height = 42;
    app_font_28.line_height = 54;
    screen_builders_refresh_font_geometry(NULL);
    screen_builders_refresh_font_geometry(screen);
    lv_obj_update_layout(screen);
    plain = find_row(list, "Song");
    assert(plain && lv_obj_get_height(plain) == ui_music_row_height());
    assert(lv_obj_get_y(title) == STATUS_BAR_CLEARANCE + (TITLE_ROW_HEIGHT - 54) / 2);
    assert(lv_obj_get_child_count(list) == pool_count);
    fonts_reset();
    screen_builders_refresh_font_geometry(NULL);
    screen_builders_refresh_font_geometry(screen);
    compact_list_set_row_height(list, 180);
    screen_builders_refresh_font_geometry(screen);
    assert(lv_obj_get_height(find_row(list, "Song")) == 180);
    compact_list_set_row_height(list, MUSIC_LIST_ROW_HEIGHT);

    compact_list_set_paged_provider(list, fetch, NULL, 1000);
    for (int i = 0; i < 100 && !find_row(list, "Fetched 0"); ++i) {
        usleep(2000); lv_tick_inc(10); lv_timer_handler();
    }
    lv_obj_t * fetched = find_row(list, "Fetched 0");
    assert(fetched && lv_obj_has_flag(lv_obj_get_child(fetched, 2), LV_OBJ_FLAG_HIDDEN));

    menu_popup_row_t rows[14];
    for (int i = 0; i < 14; ++i)
        rows[i] = (menu_popup_row_t){ "An unusually long menu choice that must wrap safely", noop, i == 3 };
    lv_obj_t * backdrop;
    lv_obj_t * popup = build_menu_popup(rows, 14, noop, &backdrop);
    lv_obj_remove_flag(popup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_update_layout(popup);
    assert(lv_obj_get_height(popup) <= display_height - 2 * STATUS_BAR_CLEARANCE);
    assert(lv_obj_get_scroll_bottom(popup) > 0);
    lv_obj_t * popup_row = lv_obj_get_child(popup, 0);
    lv_obj_t * popup_text = lv_obj_get_child(popup_row, 0);
    assert(lv_obj_get_width(popup_text) <= lv_obj_get_content_width(popup_row));
    assert(lv_obj_get_height(popup_row) >= lv_obj_get_height(popup_text) + 28);
    screenshot(lv_layer_top(), "popup", display_height);
    lv_obj_delete(popup);
    lv_obj_delete(backdrop);

    /* Palette mutation affects both primary and secondary live objects. */
    gui_plugin_set_background_color("list_row", 0xFFFFFF);
    gui_plugin_set_background_color("screen", 0xF0F0F0);
    gui_plugin_set_text_color("primary", 0x101010);
    gui_plugin_set_text_color("muted", 0x404040);
    lv_obj_add_state(fetched, LV_STATE_PRESSED);
    assert(lv_color_brightness(lv_obj_get_style_bg_color(fetched, 0)) < 255);
    assert(gui_theme_muted_text_style() == &style_theme_text_muted);
    lv_obj_remove_state(fetched, LV_STATE_PRESSED);
    screenshot(screen, "light_theme", display_height);
    compact_list_set_paged_provider(list, NULL, NULL, 0);
    compact_list_set_items(list, NULL, 0);
    screenshot(screen, "empty", display_height);

    gui_theme_reload_styles();
    pill_list_item_t settings[] = {
        { .label = "Playback settings", .accessory = PILL_ACCESSORY_CHEVRON },
        { .label = "Crossfade", .accessory = PILL_ACCESSORY_TOGGLE, .toggle_initial_state = true },
        { .label = "A very long plugin row with explicit sizing and colors", .row_height = 124,
          .row_width = 400, .has_bg_color = true, .bg_color = 0xE0E0E0,
          .has_text_color = true, .text_color = 0x101010, .has_radius = true, .radius = 4 },
    };
    lv_obj_t * settings_screen = build_pill_list_screen("Settings", noop, settings, 3, gui_theme_accent_style(), GUI_ROW_GAP);
    lv_screen_load(settings_screen);
    lv_tick_inc(500);
    lv_timer_handler();
    lv_obj_t * settings_list = lv_obj_get_child(settings_screen, 2);
    assert(lv_obj_get_height(lv_obj_get_child(settings_list, 0)) >= GUI_SETTINGS_ROW_HEIGHT);
    assert(lv_obj_get_height(lv_obj_get_child(settings_list, 1)) >= GUI_SETTINGS_ROW_HEIGHT);
    /* Explicit plugin height remains exact (the third row requests 124). */
    assert(lv_obj_get_height(lv_obj_get_child(settings_list, 2)) == 124);
    screenshot(settings_screen, "settings", display_height);

    lv_display_delete(display);
    printf("UI layout %dx%d: passed\n", 480, display_height);
}

int main(void) {
    lv_init();
    current_settings.accent_color = 0x2196F3;
    check_layout(800);
    check_layout(720);
    lv_deinit();
    return 0;
}
