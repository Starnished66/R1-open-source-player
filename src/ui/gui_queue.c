#include "gui_queue.h"
#include "gui.h"
#include "gui_player.h"
#include "gui_theme.h"
#include "gui_notifications.h"
#include "gui_library.h"
#include "screen_builders.h"
#include "metadata.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static lv_obj_t * queue_screen = NULL;
static lv_obj_t * queue_list = NULL;

static lv_obj_t * song_context_menu_popup = NULL;
static lv_obj_t * song_context_menu_popup_backdrop = NULL;
static char song_context_menu_target_path[600] = "";

extern lv_style_t list_row_style;
extern lv_style_t list_row_pressed_style;
extern lv_style_t style_theme_text_muted;

extern void nav_push(lv_obj_t * screen);
extern void get_display_names(const char * path, char * out_title, size_t title_sz, char * out_folder, size_t folder_sz);
extern void row_label_enable_marquee(lv_obj_t * label);
extern lv_obj_t * build_subsonic_list_screen(const char * title_text, lv_obj_t ** out_title_label, lv_obj_t ** out_list);

lv_obj_t * gui_queue_get_screen(void) {
    return queue_screen;
}

static void queue_row_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int offset = (int) (intptr_t) lv_event_get_user_data(e);
    int p_index = gui_player_get_playlist_index();
    if (p_index < 0) return;
    int target = p_index + 1 + offset;
    if (target < gui_player_get_playlist_count()) {
        gui_player_play_at(target);
    }
}

void populate_queue_screen(void) {
    if (!queue_list) return;
    lv_obj_clean(queue_list);

    int queued_count = gui_player_get_queued_count();
    if (!gui_player_has_active_track() || queued_count <= 0) {
        lv_obj_t * label = lv_label_create(queue_list);
        lv_label_set_text(label, "Queue is empty");
        lv_obj_add_style(label, &style_theme_text_muted, 0);
        lv_obj_set_style_pad_left(label, 24, 0);
        return;
    }

    for (int i = 0; i < queued_count; i++) {
        const char * track_path = gui_player_get_queued_path_at(i);
        if (!track_path || track_path[0] == '\0') break;

        lv_obj_t * row = lv_label_create(queue_list);
        lv_obj_add_style(row, &list_row_style, 0);
        lv_obj_add_style(row, &list_row_pressed_style, LV_STATE_PRESSED);
        row_label_enable_marquee(row);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        char title[128], folder[128];
        get_display_names(track_path, title, sizeof(title), folder, sizeof(folder));
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
    if (song_context_menu_target_path[0] != '\0') gui_player_queue_add(song_context_menu_target_path);
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
    snprintf(song_context_menu_target_path, sizeof(song_context_menu_target_path), "%s", path ? path : "");
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

/* For gui_reload.c's in-process UI reload -- deletes every screen/popup this
 * module owns so gui_queue_init() can rebuild them from a clean slate
 * without leaking the old objects. song_context_menu_popup/backdrop are
 * built via build_menu_popup() directly on lv_layer_top() (same shape as
 * build_confirm_popup(), see its own comment), not as children of
 * queue_screen, so they need their own explicit deletion. */
void gui_queue_teardown(void) {
    if (song_context_menu_popup) { lv_obj_del(song_context_menu_popup); song_context_menu_popup = NULL; }
    if (song_context_menu_popup_backdrop) { lv_obj_del(song_context_menu_popup_backdrop); song_context_menu_popup_backdrop = NULL; }
    if (queue_screen) { lv_obj_del(queue_screen); queue_screen = NULL; }
}
