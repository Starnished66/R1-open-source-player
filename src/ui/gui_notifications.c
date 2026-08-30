#include "gui_notifications.h"
#include "gui_theme.h"
#include "gui.h"
#include "screen_builders.h"
#include <stdio.h>

/* Error toast */
static lv_obj_t * error_toast = NULL;
static lv_obj_t * error_toast_label = NULL;
static lv_timer_t * error_toast_hide_timer = NULL;

/* Info toast */
static lv_obj_t * info_toast = NULL;
static lv_obj_t * info_toast_label = NULL;
static lv_timer_t * info_toast_hide_timer = NULL;

/* Busy overlay */
static uint32_t gui_busy_current_token = 0;
static lv_obj_t * gui_busy_screen = NULL;
static lv_obj_t * gui_busy_label = NULL;
static lv_obj_t * gui_busy_progress_bar = NULL;

extern void nav_push(lv_obj_t * screen);
extern void nav_pop(void);

static void error_toast_hide_timer_cb(lv_timer_t * timer) {
    (void) timer;
    if (error_toast) lv_obj_add_flag(error_toast, LV_OBJ_FLAG_HIDDEN);
    if (error_toast_hide_timer) lv_timer_pause(error_toast_hide_timer);
}

static void build_error_toast(void) {
    lv_obj_t * top = lv_layer_top();

    error_toast = lv_obj_create(top);
    lv_obj_set_size(error_toast, 400, 70);
    lv_obj_align(error_toast, LV_ALIGN_CENTER, 0, -180);
    lv_obj_set_style_radius(error_toast, 16, 0);
    lv_obj_set_style_bg_color(error_toast, lv_color_make(40, 20, 20), 0);
    lv_obj_set_style_bg_opa(error_toast, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(error_toast, 0, 0);
    lv_obj_remove_flag(error_toast, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(error_toast, LV_OBJ_FLAG_HIDDEN);

    error_toast_label = lv_label_create(error_toast);
    lv_obj_set_width(error_toast_label, lv_pct(90));
    lv_label_set_long_mode(error_toast_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(error_toast_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(error_toast_label, lv_color_make(255, 200, 200), 0);
    lv_obj_set_style_text_font(error_toast_label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_obj_center(error_toast_label);

    error_toast_hide_timer = lv_timer_create(error_toast_hide_timer_cb, 2500, NULL);
    lv_timer_pause(error_toast_hide_timer);
}

void show_error_toast(const char * msg) {
    if (!error_toast_label) return;
    lv_label_set_text(error_toast_label, msg);
    lv_obj_remove_flag(error_toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(error_toast);
    lv_timer_reset(error_toast_hide_timer);
    lv_timer_resume(error_toast_hide_timer);
}

static void info_toast_hide_timer_cb(lv_timer_t * timer) {
    (void) timer;
    if (info_toast) lv_obj_add_flag(info_toast, LV_OBJ_FLAG_HIDDEN);
    if (info_toast_hide_timer) lv_timer_pause(info_toast_hide_timer);
}

static void build_info_toast(void) {
    lv_obj_t * top = lv_layer_top();

    info_toast = lv_obj_create(top);
    lv_obj_set_size(info_toast, 420, 140);
    lv_obj_align(info_toast, LV_ALIGN_CENTER, 0, -160);
    lv_obj_set_style_radius(info_toast, 16, 0);
    lv_obj_add_style(info_toast, &style_theme_card_bg, 0);
    lv_obj_set_style_bg_opa(info_toast, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(info_toast, 0, 0);
    lv_obj_remove_flag(info_toast, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(info_toast, LV_OBJ_FLAG_HIDDEN);

    info_toast_label = lv_label_create(info_toast);
    lv_obj_set_width(info_toast_label, lv_pct(90));
    lv_label_set_long_mode(info_toast_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(info_toast_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_style(info_toast_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(info_toast_label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_obj_center(info_toast_label);

    info_toast_hide_timer = lv_timer_create(info_toast_hide_timer_cb, 5000, NULL);
    lv_timer_pause(info_toast_hide_timer);
}

void show_info_toast(const char * msg) {
    show_info_toast_for(msg, 5000);
}

void show_info_toast_for(const char * msg, uint32_t duration_ms) {
    if (!info_toast_label) return;
    if (duration_ms < 100) duration_ms = 100;
    if (duration_ms > 30000) duration_ms = 30000;
    lv_label_set_text(info_toast_label, msg);
    lv_obj_remove_flag(info_toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(info_toast);
    lv_timer_set_period(info_toast_hide_timer, duration_ms);
    lv_timer_reset(info_toast_hide_timer);
    lv_timer_resume(info_toast_hide_timer);
}

gui_busy_handle_t gui_busy_show(const char * title, const char * msg) {
    gui_busy_current_token++;
    if (!gui_busy_screen) {
        gui_busy_screen = lv_obj_create(NULL);
        lv_obj_add_style(gui_busy_screen, &style_theme_screen_bg, 0);

        gui_busy_label = lv_label_create(gui_busy_screen);
        lv_obj_add_style(gui_busy_label, &style_theme_text_primary, 0);
        lv_obj_set_style_text_align(gui_busy_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(gui_busy_label, LV_ALIGN_CENTER, 0, -20);

        gui_busy_progress_bar = lv_bar_create(gui_busy_screen);
        lv_obj_set_size(gui_busy_progress_bar, 280, 14);
        lv_obj_align(gui_busy_progress_bar, LV_ALIGN_CENTER, 0, 30);
        lv_bar_set_range(gui_busy_progress_bar, 0, 100);
        lv_obj_add_style(gui_busy_progress_bar, gui_theme_accent_style(), LV_PART_INDICATOR);
    }
    
    if (msg && msg[0] != '\0') {
        lv_label_set_text_fmt(gui_busy_label, "%s\n%s", title, msg);
    } else {
        lv_label_set_text(gui_busy_label, title);
    }
    lv_obj_add_flag(gui_busy_progress_bar, LV_OBJ_FLAG_HIDDEN);
    
    if (lv_screen_active() != gui_busy_screen) {
        nav_push(gui_busy_screen);
    }
    return gui_busy_current_token;
}

void gui_busy_set_progress(gui_busy_handle_t handle, int percent) {
    if (handle != gui_busy_current_token || !gui_busy_screen) return;
    lv_obj_remove_flag(gui_busy_progress_bar, LV_OBJ_FLAG_HIDDEN);
    lv_bar_set_value(gui_busy_progress_bar, percent, LV_ANIM_OFF);
}

void gui_busy_hide(gui_busy_handle_t handle) {
    if (handle != gui_busy_current_token || !gui_busy_screen) return;
    if (lv_screen_active() == gui_busy_screen) {
        nav_pop();
    }
    gui_busy_current_token++; /* invalidate token */
}

void gui_notifications_init(void) {
    build_error_toast();
    build_info_toast();
}

lv_obj_t * gui_busy_get_screen(void) {
    return gui_busy_screen;
}

lv_obj_t * build_menu_popup(const menu_popup_row_t * rows, int row_count, lv_event_cb_t backdrop_cb,
                            lv_obj_t ** out_backdrop) {
    lv_obj_t * top = lv_layer_top();

    lv_obj_t * backdrop = lv_obj_create(top);
    lv_obj_set_size(backdrop, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(backdrop, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(backdrop, LV_OPA_50, 0);
    lv_obj_set_style_border_width(backdrop, 0, 0);
    lv_obj_remove_flag(backdrop, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(backdrop, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(backdrop, backdrop_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t * popup = lv_obj_create(top);
    lv_obj_set_width(popup, lv_pct(84));
    lv_obj_set_height(popup, LV_SIZE_CONTENT);
    lv_obj_align(popup, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(popup, 16, 0);
    lv_obj_add_style(popup, &style_theme_card_bg, 0);
    lv_obj_set_style_bg_opa(popup, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(popup, 0, 0);
    lv_obj_set_style_pad_all(popup, 20, 0);
    lv_obj_set_style_pad_row(popup, 10, 0);
    lv_obj_remove_flag(popup, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(popup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_flex_flow(popup, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(popup, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    for (int i = 0; i < row_count; i++) {
        lv_obj_t * row = lv_obj_create(popup);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(row, 14, 0);
        lv_obj_set_style_radius(row, 12, 0);
        lv_obj_set_style_bg_opa(row, 0, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, rows[i].cb, LV_EVENT_CLICKED, NULL);

        lv_obj_t * label = lv_label_create(row);
        lv_label_set_text(label, rows[i].label);
        lv_obj_set_style_text_color(label, rows[i].destructive ? lv_color_make(255, 120, 120) : accent_lv_color(), 0);
        lv_obj_set_style_text_font(label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
        lv_obj_center(label);
    }

    *out_backdrop = backdrop;
    return popup;
}
