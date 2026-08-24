#include "gui_queue.h"
#include "gui.h"
#include "gui_theme.h"
#include "gui_notifications.h"
#include "gui_library.h"
#include "screen_builders.h"
#include "metadata.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

lv_obj_t * queue_screen = NULL;
lv_obj_t * queue_list = NULL;

static lv_obj_t * song_context_menu_popup = NULL;
static lv_obj_t * song_context_menu_popup_backdrop = NULL;
static char song_context_menu_target_path[600] = "";

extern int playlist_index;
extern int playlist_count;
extern char ** playlist;
extern int queued_pending_count;
extern lv_style_t list_row_style;
extern lv_style_t list_row_pressed_style;
extern lv_style_t style_theme_text_muted;

extern void nav_push(lv_obj_t * screen);
extern void play_track_at(int target);
extern void get_display_names(const char * path, char * out_title, size_t title_sz, char * out_folder, size_t folder_sz);
extern void row_label_enable_marquee(lv_obj_t * label);
extern void queue_add_song(const char * path);
extern lv_obj_t * build_subsonic_list_screen(const char * title_text, lv_obj_t ** out_title_label, lv_obj_t ** out_list);

static void queue_row_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int offset = (int) (intptr_t) lv_event_get_user_data(e);
    if (playlist_index < 0) return;
    int target = playlist_index + 1 + offset;
    if (target < playlist_count) play_track_at(target);
}

void populate_queue_screen(void) {
    lv_obj_clean(queue_list);

    if (playlist_index < 0 || queued_pending_count <= 0) {
        lv_obj_t * label = lv_label_create(queue_list);
        lv_label_set_text(label, "Queue is empty");
        lv_obj_add_style(label, &style_theme_text_muted, 0);
        lv_obj_set_style_pad_left(label, 24, 0);
        return;
    }

    for (int i = 0; i < queued_pending_count; i++) {
        int idx = playlist_index + 1 + i;
        if (idx >= playlist_count) break;

        lv_obj_t * row = lv_label_create(queue_list);
        lv_obj_add_style(row, &list_row_style, 0);
        lv_obj_add_style(row, &list_row_pressed_style, LV_STATE_PRESSED);
        row_label_enable_marquee(row);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        char title[128], folder[128];
        get_display_names(playlist[idx], title, sizeof(title), folder, sizeof(folder));
        lv_label_set_text(row, title);

        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, queue_row_click_cb, LV_EVENT_CLICKED, (void *) (intptr_t) i);
    }
}

static lv_obj_t * build_queue_screen(void) {
    lv_obj_t * title_label;
    return build_subsonic_list_screen("Queue", &title_label, &queue_list);
}

void open_queue_screen(void) {
    populate_queue_screen();
    nav_push(queue_screen);
}

void hide_song_context_menu_popup(void) {
    if (song_context_menu_popup_backdrop) lv_obj_add_flag(song_context_menu_popup_backdrop, LV_OBJ_FLAG_HIDDEN);
    if (song_context_menu_popup) lv_obj_add_flag(song_context_menu_popup, LV_OBJ_FLAG_HIDDEN);
}

static void song_context_menu_popup_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_song_context_menu_popup();
}

static void song_context_menu_add_to_queue_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_song_context_menu_popup();
    if (song_context_menu_target_path[0] != '\0') queue_add_song(song_context_menu_target_path);
}

static void song_context_menu_add_to_playlist_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_song_context_menu_popup();
    if (song_context_menu_target_path[0] != '\0') open_add_to_playlist_for(song_context_menu_target_path);
}

static void song_context_menu_cancel_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_song_context_menu_popup();
}

void open_song_context_menu(const char * path) {
    snprintf(song_context_menu_target_path, sizeof(song_context_menu_target_path), "%s", path);
    if (song_context_menu_popup_backdrop) lv_obj_remove_flag(song_context_menu_popup_backdrop, LV_OBJ_FLAG_HIDDEN);
    if (song_context_menu_popup) lv_obj_remove_flag(song_context_menu_popup, LV_OBJ_FLAG_HIDDEN);
    if (song_context_menu_popup_backdrop) lv_obj_move_foreground(song_context_menu_popup_backdrop);
    if (song_context_menu_popup) lv_obj_move_foreground(song_context_menu_popup);
}

static void build_song_context_menu_popup(void) {
    static const menu_popup_row_t rows[] = {
        { "Add to Queue", song_context_menu_add_to_queue_cb, false },
        { "Add to Playlist", song_context_menu_add_to_playlist_cb, false },
        { "Cancel", song_context_menu_cancel_cb, false },
    };
    song_context_menu_popup = build_menu_popup(rows, (int) (sizeof(rows) / sizeof(rows[0])),
                                                song_context_menu_popup_backdrop_cb,
                                                &song_context_menu_popup_backdrop);
}

void gui_queue_init(void) {
    queue_screen = build_queue_screen();
    build_song_context_menu_popup();
}
