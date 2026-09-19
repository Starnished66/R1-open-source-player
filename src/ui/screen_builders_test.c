/* Headless layout tests using real LVGL objects and fonts.
 * Navigation, storage and asset lookup are isolated from device services. */
#include "screen_builders.h"
#include "gui_notifications.h"
#include "gui.h"
#include "gui_plugins.h"
#include "assets.h"
#include "transition_compositor.h"
#include "frosted_glass.h"
#include "topbar_icon_layout.h"
#include "lvgl/src/libs/lodepng/lodepng.h"
#include "settings.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>

lv_font_t app_font_16, app_font_20, app_font_22, app_font_28, app_font_lyrics;
player_settings_t current_settings;
const char * asset_path(const char * path) { (void)path; return NULL; }
const char * asset_path_plain(const char * path) { (void)path; return "/missing-test-asset"; }
static int category_asset_opens, category_asset_closes;
LV_DRAW_BUF_DEFINE(category_test_draw_buf, 1, 1, LV_COLOR_FORMAT_RGB565);
bool asset_decoded_image_open(asset_decoded_image_t * image, const char * path) {
    if (!image || !path || (strncmp(path, "test/", 5) != 0 && strncmp(path, "submenu/bg_", 11) != 0)) return false;
    memset(image, 0, sizeof(*image));
    image->decoder.decoded = &category_test_draw_buf;
    image->open = true;
    ++category_asset_opens;
    return true;
}
void asset_decoded_image_close(asset_decoded_image_t * image) {
    if (!image) return;
    if (image->open) ++category_asset_closes;
    memset(image, 0, sizeof(*image));
}
bool asset_decoded_gradient_open(asset_decoded_image_t * image, const char * path) {
    return asset_decoded_image_open(image, path);
}
const void * asset_decoded_image_source(const asset_decoded_image_t * image) {
    return image && image->open ? image->decoder.decoded : NULL;
}
void settings_save(const player_settings_t * settings) { (void)settings; }
void player_transition_mark_dirty(void) {}
void refresh_play_btn_icon(void) {}
void gui_shell_refresh_quick_drawer_toggle_accent(void) {}
static lv_obj_t * last_pushed;
void nav_push(lv_obj_t * screen) { last_pushed = screen; }
void nav_pop(void) {}
void generic_back_cb(lv_event_t * event) { (void)event; }
void finalize_screen_navigation(lv_obj_t * screen) { (void)screen; }
int gui_navigation_get_depth(void) { return 0; }
lv_obj_t * gui_navigation_get_screen_at(int index) { (void)index; return NULL; }
static int plugin_clicked_slot, plugin_clicked_row;
void plugin_manager_list_item_selected(int slot, int index) { plugin_clicked_slot = slot; plugin_clicked_row = index; }
void plugin_manager_settings_list_row_selected(int slot, int row) { plugin_clicked_slot = slot; plugin_clicked_row = row; }
static bool plugin_toggle_value;
void plugin_manager_settings_list_toggled(int slot, int row, bool value) {
    plugin_clicked_slot = slot; plugin_clicked_row = row; plugin_toggle_value = value;
}
void plugin_manager_settings_list_slid(int slot, int row, int value) { (void)slot; (void)row; (void)value; }
void register_swipe_dead_zone(lv_obj_t * obj) { (void)obj; }
void unregister_swipe_dead_zone(lv_obj_t * obj) { (void)obj; }
bool gui_navigation_transition_in_progress(void) { return false; }
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
static void check_translucent_snapshot(void) {
    lv_obj_t * panel = lv_obj_create(NULL);
    lv_obj_remove_style_all(panel);
    lv_obj_set_size(panel, 16, 16);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x071B33), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_70, 0);
    lv_obj_t * control = lv_obj_create(panel);
    lv_obj_remove_style_all(control);
    lv_obj_set_size(control, 4, 4);
    lv_obj_set_style_bg_color(control, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(control, LV_OPA_COVER, 0);
    lv_draw_buf_t * snapshot = lv_snapshot_take(panel, LV_COLOR_FORMAT_ARGB8888);
    assert(snapshot);
    const lv_color32_t * row = (const lv_color32_t *)(snapshot->data + 8 * snapshot->header.stride);
    /* Drawer snapshots retain straight alpha, not colors darkened against
     * black; children keep their own opacity instead of fading as a group. */
    assert(row[8].alpha == LV_OPA_70);
    assert(row[8].red == 7 && row[8].green == 27 && row[8].blue == 51);
    row = (const lv_color32_t *)(snapshot->data + snapshot->header.stride);
    assert(row[1].alpha == 255 && row[1].red == 255);
    lv_draw_buf_destroy(snapshot);
    lv_obj_delete(panel);
}

static void check_drawer_blend(void) {
    /* Independently padded rows; include transparent, opaque, and partial
     * alpha. Padding must remain untouched in the destination. */
    uint16_t destination[2][4] = {{0x1234, 0, 0xffff, 0xabcd},
                                  {0x4321, 0, 0xffff, 0xdcba}};
    const uint8_t source[2][16] = {
        {0,0,0,0, 0,0,255,255, 0,0,0,128, 1,2,3,4},
        {0,0,0,0, 0,255,0,255, 0,0,0,128, 5,6,7,8},
    };
    transition_compositor_blend_argb8888_over_rgb565((uint8_t *)destination, sizeof(destination[0]),
                                                   &source[0][0], sizeof(source[0]), 3, 2);
    assert(destination[0][0] == 0x1234 && destination[1][0] == 0x4321);
    assert(destination[0][1] == 0xf800 && destination[1][1] == 0x07e0);
    assert(destination[0][2] == 0x7bef && destination[1][2] == 0x7bef);
    assert(destination[0][3] == 0xabcd && destination[1][3] == 0xdcba);
}

static void check_plugin_menu_rows(void) {
    gui_plugins_init();
    const char * labels[] = {"A long station or book name that needs a marquee", "Second item"};
    const char * icons[] = {"/missing-icon.png", "/missing-icon.png"};
    int slot = gui_plugin_show_list("Plugin", labels, icons, NULL, 0, 0, 0, 2);
    lv_obj_update_layout(last_pushed);
    lv_obj_t * list = lv_obj_get_child(last_pushed, 2);
    lv_obj_t * row = lv_obj_get_child(list, 1);
    assert(lv_obj_get_height(row) == BOARD_SCALE_PX(96));
    lv_obj_t * background = lv_obj_get_child(row, 0);
    assert(lv_obj_check_type(background, &lv_image_class));
    assert(!lv_obj_has_flag(background, LV_OBJ_FLAG_HIDDEN));
    gui_plugin_set_background_color("list_row", 0xFFFFFF);
    assert(lv_obj_has_flag(background, LV_OBJ_FLAG_HIDDEN));
    gui_plugin_set_background_color("list_row", GUI_COLOR_ROW);
    assert(!lv_obj_has_flag(background, LV_OBJ_FLAG_HIDDEN));
    lv_obj_send_event(row, LV_EVENT_CLICKED, NULL);
    assert(plugin_clicked_slot == slot && plugin_clicked_row == 1);
    assert(lv_obj_get_style_outline_width(row, 0) == 3);
    /* Explicit sizes still win over the new native icon-row defaults. */
    gui_plugin_show_list("Sized", labels, icons, NULL, 120, 360, -1, 2);
    lv_obj_update_layout(last_pushed);
    row = lv_obj_get_child(lv_obj_get_child(last_pushed, 2), 0);
    assert(lv_obj_get_width(row) == 360 && lv_obj_get_height(row) == 120);
    gui_plugin_show_list("Plain", labels, NULL, NULL, 0, 0, -1, 2);
    row = lv_obj_get_child(lv_obj_get_child(last_pushed, 2), 0);
    assert(lv_obj_check_type(row, &lv_label_class));

    int types[] = {PLUGIN_SETTINGS_ROW_TAP, PLUGIN_SETTINGS_ROW_TOGGLE};
    bool toggles[] = {false, false};
    int values[] = {0, 0};
    int32_t sizes[] = {0, 0};
    const char * fonts[] = {NULL, NULL};
    slot = gui_plugin_show_settings_list("Book", types, labels, toggles, values, values,
                                         values, icons, sizes, sizes, fonts, 2);
    list = lv_obj_get_child(last_pushed, 2);
    lv_obj_send_event(lv_obj_get_child(list, 0), LV_EVENT_CLICKED, NULL);
    assert(plugin_clicked_slot == slot && plugin_clicked_row == 0);
    lv_obj_send_event(lv_obj_get_child(list, 1), LV_EVENT_CLICKED, NULL);
    assert(plugin_clicked_slot == slot && plugin_clicked_row == 1 && plugin_toggle_value);
    /* Toggle rebuilds must retain both rows and their callbacks. */
    assert(lv_obj_get_child_count(list) == 2);
    lv_obj_send_event(lv_obj_get_child(list, 1), LV_EVENT_CLICKED, NULL);
    assert(!plugin_toggle_value);
    gui_plugins_teardown();
    last_pushed = NULL;
}
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

static void check_compact_home_glow(int display_height) {
    icon_grid_item_t items[6];
    for (int i = 0; i < 6; ++i)
        items[i] = (icon_grid_item_t){ .icon_asset = "missing.png", .label = "Home",
                                      .icon_glow_color = 0xF5B457 };
    lv_obj_t * screen = build_icon_grid_screen(NULL, NULL, items, 6, 100, false, 0);
    lv_obj_update_layout(screen);
    lv_obj_t * grid = lv_obj_get_child(screen, lv_obj_get_child_count(screen) - 1);
    lv_obj_t * tile = lv_obj_get_child(grid, 0);
    assert(lv_obj_get_child_count(tile) == 2); /* no changes to caption/icon layout */
    lv_area_t tile_area, icon_area;
    lv_obj_get_coords(tile, &tile_area);
    lv_obj_get_coords(lv_obj_get_child(tile, 0), &icon_area);
    lv_draw_buf_t * shot = lv_snapshot_take(screen, LV_COLOR_FORMAT_RGB888);
    assert(shot);
    int32_t y = (icon_area.y1 + icon_area.y2) / 2;
    int32_t x = (icon_area.x1 + icon_area.x2) / 2;
    const uint8_t * center = shot->data + y * shot->header.stride + x * 3;
    const uint8_t * edge = shot->data + y * shot->header.stride + tile_area.x2 * 3;
    const uint8_t * neighbor = edge + 3;
    /* Missing icons isolate the glow. It must be visible near the icon,
     * yet leave adjacent tile edges identical to the shared background. */
    assert(memcmp(center, edge, 3) != 0);
    assert(memcmp(edge, neighbor, 3) == 0);
    lv_draw_buf_destroy(shot);
    screenshot(screen, "compact_home_glow", display_height);
    lv_obj_delete(screen);
}

static lv_obj_t * pill_list_first_label(lv_obj_t * screen) {
    lv_obj_t * list = lv_obj_get_child(screen, 2);
    lv_obj_t * row = list ? lv_obj_get_child(list, 0) : NULL;
    return row ? lv_obj_get_child(row, 0) : NULL;
}

static void check_unvisited_font_geometry(void) {
    fonts_reset();
    pill_list_item_t items[] = {
        { .label = "Audio", .accessory = PILL_ACCESSORY_CHEVRON },
    };
    lv_obj_t * visited = build_pill_list_screen("Display", noop, items, 1,
                                               gui_theme_accent_style(), GUI_ROW_GAP, 100);
    lv_obj_t * unvisited = build_pill_list_screen("Music Settings", noop, items, 1,
                                                 gui_theme_accent_style(), GUI_ROW_GAP, 100);
    lv_screen_load(visited);
    lv_obj_update_layout(visited);
    lv_obj_update_layout(unvisited);
    lv_obj_t * visited_label = pill_list_first_label(visited);
    lv_obj_t * unvisited_label = pill_list_first_label(unvisited);
    assert(visited_label && unvisited_label);
    int32_t old_h = lv_obj_get_height(unvisited_label);
    app_font_20.line_height = 48;
    int32_t want = row_label_bounded_height(&app_font_20);
    assert(want > old_h);

    /* Per-root refresh is still single-screen: the unvisited Music Settings
     * analogue must not move until the all-screens walk runs. */
    screen_builders_refresh_font_geometry(NULL);
    screen_builders_refresh_font_geometry(visited);
    lv_obj_update_layout(unvisited);
    assert(lv_obj_get_height(unvisited_label) == old_h);

    screen_builders_refresh_all_font_geometry();
    lv_obj_update_layout(visited);
    lv_obj_update_layout(unvisited);
    assert(lv_obj_get_height(visited_label) == want);
    assert(lv_obj_get_height(unvisited_label) == want);
    fonts_reset();
    lv_obj_delete(unvisited);
    lv_obj_delete(visited);
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

static atomic_int late_fetch_release;
static atomic_int late_fetch_finished;
static void compact_noop_click(int index) { (void)index; }

static int gated_fetch(void * context, int offset, int count, compact_list_page_row_t * rows) {
    (void)context;
    while (!atomic_load(&late_fetch_release)) usleep(1000);
    snprintf(rows[0].label, sizeof(rows[0].label), "Late fetch %d", offset);
    atomic_store(&late_fetch_finished, 1);
    return count > 0 ? 1 : 0;
}

static void check_compact_list_delete_does_not_wait(void) {
    compact_list_item_t item = { .label = "Pending" };
    lv_obj_t * host = lv_obj_create(NULL);
    lv_obj_t * list = build_compact_list_widget(host, &item, 1, compact_noop_click, NULL,
                                                 LIST_ROW_WIDTH, false, lv_color_black());
    atomic_store(&late_fetch_release, 0);
    atomic_store(&late_fetch_finished, 0);
    compact_list_set_paged_provider(list, gated_fetch, NULL, 100);

    struct timespec started, finished;
    clock_gettime(CLOCK_MONOTONIC, &started);
    lv_obj_delete(host);
    clock_gettime(CLOCK_MONOTONIC, &finished);
    long elapsed_ms = (finished.tv_sec - started.tv_sec) * 1000L +
                      (finished.tv_nsec - started.tv_nsec) / 1000000L;
    assert(elapsed_ms < 150);

    /* The detached worker must still be able to finish against its own job
     * storage after the list/data have been freed. */
    atomic_store(&late_fetch_release, 1);
    for (int i = 0; i < 200 && !atomic_load(&late_fetch_finished); ++i) usleep(1000);
    assert(atomic_load(&late_fetch_finished));
}

/* LVGL 9.5's hidden-flag removal dirties both the object and its parent.
 * Root screens and display layers have no parent, so that path must tolerate
 * NULL while preserving ordinary child layout invalidation.  This mirrors
 * gui_show_boot_splash()/gui_init(), which hide and later reveal layer_top. */
static void check_root_hidden_flag_layout_invalidation(void) {
    lv_obj_t * top = lv_layer_top();
    bool top_was_hidden = lv_obj_has_flag(top, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(top, LV_OBJ_FLAG_HIDDEN);
    assert(lv_obj_has_flag(top, LV_OBJ_FLAG_HIDDEN));
    lv_obj_remove_flag(top, LV_OBJ_FLAG_HIDDEN);
    assert(!lv_obj_has_flag(top, LV_OBJ_FLAG_HIDDEN));
    if (top_was_hidden) lv_obj_add_flag(top, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t * root = lv_obj_create(NULL);
    lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
    assert(lv_obj_has_flag(root, LV_OBJ_FLAG_HIDDEN));
    lv_obj_remove_flag(root, LV_OBJ_FLAG_HIDDEN);
    assert(!lv_obj_has_flag(root, LV_OBJ_FLAG_HIDDEN));

    lv_obj_t * row = lv_obj_create(root);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_t * first = lv_obj_create(row);
    lv_obj_t * second = lv_obj_create(row);
    lv_obj_set_size(first, 20, 10);
    lv_obj_set_size(second, 30, 10);
    lv_obj_update_layout(root);
    int32_t second_x_shown = lv_obj_get_x(second);
    lv_obj_add_flag(first, LV_OBJ_FLAG_HIDDEN);
    lv_obj_update_layout(root);
    assert(lv_obj_get_x(second) < second_x_shown);
    lv_obj_remove_flag(first, LV_OBJ_FLAG_HIDDEN);
    lv_obj_update_layout(root);
    assert(lv_obj_get_x(second) == second_x_shown);

    lv_obj_delete(root);
}

static void check_topbar_icon_slots(void) {
    const int bounds[][4] = {
        {5, 6, 19, 18}, {6, 7, 16, 14}, {3, 7, 22, 12}, {6, 5, 16, 21},
        {9, 8, 13, 14}, {8, 8, 12, 14}, {4, 7, 20, 16}, {9, 7, 10, 16}, {8, 7, 13, 16}
    };
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_remove_style_all(screen);
    lv_obj_t *root = lv_obj_create(screen);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, 480, STATUS_BAR_CLEARANCE);
    lv_obj_t *previous = NULL;
    for (size_t i = 0; i < sizeof(bounds) / sizeof(bounds[0]); ++i) {
        lv_obj_t *icon = lv_image_create(root);
        topbar_icon_layout(icon, bounds[i][0], bounds[i][1], bounds[i][2], bounds[i][3], STATUS_BAR_CLEARANCE);
        if (previous) lv_obj_align_to(icon, previous, LV_ALIGN_OUT_RIGHT_MID, 0, 0);
        else lv_obj_align(icon, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_update_layout(root);
        assert(lv_obj_get_height(icon) == STATUS_BAR_CLEARANCE);
        assert(lv_obj_get_y(icon) == 0);
        assert(lv_obj_get_style_translate_y(icon, 0) == 0);
        if (previous) assert(lv_obj_get_x(icon) == lv_obj_get_x(previous) + lv_obj_get_width(previous));
        int scale = lv_image_get_scale(icon);
        int glyph_top = lv_image_get_offset_y(icon) + (bounds[i][1] * scale + 128) / 256;
        assert(glyph_top == (STATUS_BAR_CLEARANCE - BOARD_SCALE_PX(22)) / 2);
        int glyph_left = lv_image_get_offset_x(icon) + (bounds[i][0] * scale + 128) / 256;
        assert(glyph_left == BOARD_SCALE_PX(4));
        previous = icon;
    }
    /* Inner align_to already applies the reference object's border inset. */
    lv_obj_t *frame = lv_obj_create(root);
    lv_obj_remove_style_all(frame);
    lv_obj_set_size(frame, BOARD_SCALE_PX(36), BOARD_SCALE_PX(22));
    lv_obj_set_style_border_width(frame, BOARD_SCALE_PX(2), 0);
    lv_obj_set_pos(frame, 400, 5);
    lv_obj_t *fill = lv_obj_create(root);
    lv_obj_remove_style_all(fill);
    lv_obj_set_size(fill, BOARD_SCALE_PX(36) - 2 * BOARD_SCALE_PX(2),
                         BOARD_SCALE_PX(22) - 2 * BOARD_SCALE_PX(2));
    lv_obj_align_to(fill, frame, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_update_layout(root);
    assert(lv_obj_get_x(fill) == lv_obj_get_x(frame) + BOARD_SCALE_PX(2));
    assert(lv_obj_get_x(fill) + lv_obj_get_width(fill)
           == lv_obj_get_x(frame) + lv_obj_get_width(frame) - BOARD_SCALE_PX(2));
    lv_obj_delete(screen);
}

static void check_layout(int display_height) {
    lv_display_t * display = lv_display_create(480, display_height);
    static uint8_t pixels[480 * 40 * 4];
    lv_display_set_buffers(display, pixels, NULL, sizeof(pixels), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    check_root_hidden_flag_layout_invalidation();
    check_topbar_icon_slots();
    fonts_reset();
    gui_theme_init();
    assert(ui_list_row_width() == 480 && ui_list_row_width_wide() == 480);
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
    lv_obj_t * action = build_top_right_icon_button(screen, NULL, noop);
    lv_obj_update_layout(screen);
    assert(lv_obj_get_width(title) == 480 - 76 - TITLE_ROW_HEIGHT - 12);
    lv_obj_t * back = lv_obj_get_child(screen, 0);
    assert(lv_obj_get_y(back) == STATUS_BAR_CLEARANCE);
    assert(lv_obj_get_y(action) == lv_obj_get_y(back));
    assert(lv_obj_get_height(back) == TITLE_ROW_HEIGHT);
    assert(lv_obj_get_height(action) == TITLE_ROW_HEIGHT);
    assert(lv_obj_get_width(action) == TITLE_ROW_HEIGHT);
    assert(lv_obj_get_x(action) == 480 - TITLE_ROW_HEIGHT);
    /* Missing assets in this harness have zero size; give both icons a
     * geometry so their shared center can be tested without filesystem IO. */
    lv_obj_t * back_icon = lv_obj_get_child(back, 0);
    lv_obj_t * action_icon = lv_obj_get_child(action, 0);
    lv_obj_set_size(back_icon, 24, 24);
    lv_obj_set_size(action_icon, 32, 32);
    lv_obj_update_layout(screen);
    assert(lv_obj_get_y(back_icon) + 12 == TITLE_ROW_HEIGHT / 2);
    assert(lv_obj_get_y(action_icon) + 16 == TITLE_ROW_HEIGHT / 2);
    assert(lv_obj_get_y(title) == STATUS_BAR_CLEARANCE +
        (TITLE_ROW_HEIGHT - lv_font_get_line_height(gui_theme_font(GUI_FONT_ROLE_TITLE))) / 2);
    lv_obj_t * text_action = lv_label_create(screen);
    lv_label_set_text(text_action, "Rescan");
    align_screen_header_action(text_action, 20);
    for (int h = 24; h <= 48; h += 24) {
        lv_obj_set_height(text_action, h);
        lv_obj_update_layout(screen);
        lv_area_t area;
        lv_obj_get_coords(text_action, &area);
        assert(area.y1 + h / 2 == STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT / 2);
    }
    lv_obj_delete(text_action);

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
    check_compact_list_delete_does_not_wait();

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
    lv_obj_t * settings_screen = build_pill_list_screen("Settings", noop, settings, 3, gui_theme_accent_style(), GUI_ROW_GAP, 100);
    lv_screen_load(settings_screen);
    lv_tick_inc(500);
    lv_timer_handler();
    lv_obj_t * settings_list = lv_obj_get_child(settings_screen, 2);
    assert(lv_obj_get_height(lv_obj_get_child(settings_list, 0)) >= GUI_SETTINGS_ROW_HEIGHT);
    assert(lv_obj_get_height(lv_obj_get_child(settings_list, 1)) >= GUI_SETTINGS_ROW_HEIGHT);
    /* Explicit plugin height remains exact (the third row requests 124). */
    assert(lv_obj_get_height(lv_obj_get_child(settings_list, 2)) == 124);
    screenshot(settings_screen, "settings", display_height);

    icon_grid_item_t categories[6];
    for (int i = 0; i < 6; ++i) {
        categories[i] = (icon_grid_item_t) {
            .icon_asset = "test/icon.png", .label = "Category",
            .bg_image = "test/background.png",
        };
    }
    category_asset_opens = category_asset_closes = 0;
    lv_obj_t * category_screen = build_category_menu_screen("Categories", noop, categories, 6, NULL);
    assert(category_screen);
    lv_obj_update_layout(category_screen);
    lv_obj_t * category_list = lv_obj_get_child(category_screen, 2);
    lv_obj_t * category_row = lv_obj_get_child(category_list, 0);
    /* Six-item menus (Wireless) keep the standard category-row height and
     * rely on the shared list's scrolling rather than shrinking their rows. */
    assert(lv_obj_get_height(category_row) == BOARD_SCALE_PX(112));
    assert(category_asset_opens == 2);
    /* Deleting a child emits a bubbled DELETE event. The row context must
     * survive it and be released only by the row's own DELETE event. */
    lv_obj_t * category_icon = lv_obj_get_child(category_row, lv_obj_get_child_count(category_row) - 1);
    assert(lv_obj_check_type(category_icon, &lv_image_class));
    lv_obj_delete(category_icon);
    assert(category_asset_closes == 0);
    /* A second live screen borrows the same decoded assets; deleting the
     * first must not invalidate the second screen's images. */
    lv_obj_t * other_category_screen = build_category_menu_screen("Other", noop, categories, 6, NULL);
    assert(category_asset_opens == 2);
    lv_obj_delete(category_screen);
    assert(category_asset_closes == 0);
    lv_obj_delete(other_category_screen);
    assert(category_asset_closes == 2);
    category_screen = build_category_menu_screen("Categories", noop, categories, 6, NULL);
    assert(category_screen && category_asset_opens == 4);
    lv_obj_delete(category_screen);
    assert(category_asset_closes == 4);

    check_unvisited_font_geometry();
    check_plugin_menu_rows();
    check_compact_home_glow(display_height);
    check_translucent_snapshot();
    lv_display_delete(display);
    printf("UI layout %dx%d: passed\n", 480, display_height);
}

int main(void) {
    int light[4] = {0};
    for (int y = 0; y < 128; ++y) for (int x = 0; x < 128; ++x) {
        uint16_t p = rgb888_to_565_spatial_dithered(128, 128, 128, x, y);
        assert(p == rgb888_to_565_spatial_dithered(128, 128, 128, x, y));
        assert((p >> 11) == 15 || (p >> 11) == 16);
        if ((p >> 11) == 16) ++light[(y & 1) * 2 + (x & 1)];
        assert(rgb888_to_565_spatial_dithered(0, 0, 0, x, y) == 0);
        assert(rgb888_to_565_spatial_dithered(255, 255, 255, x, y) == 0xffff);
    }
    /* Each parity class should have both levels, not a checkerboard whose
     * bright and dark values are locked to alternating physical pixels. */
    for (int i = 0; i < 4; ++i) assert(light[i] > 1800 && light[i] < 2300);
    lv_init();
    check_drawer_blend();
    current_settings.accent_color = 0x2196F3;
    check_layout(800);
    check_layout(720);
    lv_deinit();
    return 0;
}
