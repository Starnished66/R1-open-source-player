#include "gui_queue.h"
#include "gui.h"
#include "gui_player.h"
#include "gui_theme.h"
#include "gui_notifications.h"
#include "gui_library.h"
#include "gui_text_input.h"
#include "screen_builders.h"
#include "metadata.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

#define QUEUE_PAGE_SIZE 100
static int queue_page;
static uint64_t displayed_revision;
static bool queue_editing;
static lv_obj_t * queue_actions, * queue_actions_backdrop;
static void queue_actions_open(lv_event_t * e);
static int displayed_current = -1;

static void queue_row_click_cb(lv_event_t * e) {
    if (!gui_player_queue_select(displayed_revision, (int) (intptr_t) lv_event_get_user_data(e)))
        show_error_toast("Queue changed. Try again.");
}

static void queue_edit_cb(lv_event_t * e) {
    int value = (int) (intptr_t) lv_event_get_user_data(e);
    int index = value / 3, action = value % 3;
    int to = action == 2 ? -1 : index + (action == 0 ? -1 : 1);
    if (!gui_player_queue_edit(displayed_revision, index, to))
        show_error_toast("Cannot move this entry");
    populate_queue_screen();
}

static void queue_page_cb(lv_event_t * e) {
    queue_page += (int) (intptr_t) lv_event_get_user_data(e);
    populate_queue_screen();
    lv_obj_scroll_to_y(queue_list, 0, LV_ANIM_OFF);
}

static lv_obj_t * queue_label(const char * text) {
    lv_obj_t * row = lv_label_create(queue_list);
    lv_obj_add_style(row, &list_row_style, 0);
    lv_obj_add_style(row, &list_row_pressed_style, LV_STATE_PRESSED);
    lv_label_set_text(row, text);
    lv_label_set_long_mode(row, LV_LABEL_LONG_DOT);
    lv_obj_set_style_height(row, MUSIC_LIST_ROW_HEIGHT, 0);
    lv_obj_set_style_pad_top(row, strchr(text, '\n') ? 12 : 30, 0);
    return row;
}

void populate_queue_screen(void) {
    if (!queue_list) return;
    int * order = NULL, count, current;
    uint64_t revision;
    if (!gui_player_queue_snapshot(&order, &count, &current, &revision)) return;
    lv_obj_clean(queue_list);
    displayed_revision = revision;
    displayed_current = current;
    lv_obj_t * start = queue_label("Start playlist: Sequential / Shuffle");
    lv_obj_add_flag(start, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(start, queue_actions_open, LV_EVENT_CLICKED, NULL);
    if (!count) { queue_label("Queue is empty"); free(order); return; }
    if (queue_page < 0) queue_page = 0;
    if (queue_page >= count) queue_page = ((count - 1) / QUEUE_PAGE_SIZE) * QUEUE_PAGE_SIZE;
    if (queue_page > 0) {
        lv_obj_t * row = queue_label("Previous page");
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, queue_page_cb, LV_EVENT_CLICKED, (void *) (intptr_t) -QUEUE_PAGE_SIZE);
    }
    int end = queue_page + QUEUE_PAGE_SIZE;
    if (end > count) end = count;
    for (int i = queue_page; i < end; i++) {
        const char * path = gui_player_get_track_path_at(order[i]);
        char title[128], folder[128], text[320];
        get_display_names(path, title, sizeof(title), folder, sizeof(folder));
        const char * kind = i < current ? "Played" : i == current ? "Current" :
            i <= current + gui_player_get_queued_count() ? "Queued" : "Upcoming";
        snprintf(text, sizeof(text), "%d. %s\n%s  [%s]", i + 1, title, folder, kind);
        if (queue_editing && i > current) {
            lv_obj_t * row = lv_obj_create(queue_list);
            lv_obj_set_size(row, LIST_ROW_WIDTH, MUSIC_LIST_ROW_HEIGHT);
            lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_t * label = lv_label_create(row);
            lv_label_set_text(label, text);
            lv_obj_set_width(label, LIST_ROW_WIDTH - 180);
            lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
            lv_obj_align(label, LV_ALIGN_LEFT_MID, 0, 0);
            for (int a = 0; a < 3; a++) {
                lv_obj_t * button = lv_label_create(row);
                lv_label_set_text(button, a == 0 ? LV_SYMBOL_UP : a == 1 ? LV_SYMBOL_DOWN : LV_SYMBOL_TRASH);
                lv_obj_align(button, LV_ALIGN_RIGHT_MID, -110 + a * 50, 0);
                lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_set_ext_click_area(button, 12);
                lv_obj_add_event_cb(button, queue_edit_cb, LV_EVENT_CLICKED, (void *) (intptr_t) (i * 3 + a));
            }
        } else {
            lv_obj_t * row = queue_label(text);
            if (i == current) lv_obj_set_style_text_color(row, accent_lv_color(), 0);
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(row, queue_row_click_cb, LV_EVENT_CLICKED, (void *) (intptr_t) i);
        }
    }
    if (end < count) {
        lv_obj_t * row = queue_label("Next page");
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, queue_page_cb, LV_EVENT_CLICKED, (void *) (intptr_t) QUEUE_PAGE_SIZE);
    }
    free(order);
}

static void queue_actions_hide(lv_event_t * e) {
    (void) e;
    lv_obj_add_flag(queue_actions, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(queue_actions_backdrop, LV_OBJ_FLAG_HIDDEN);
}
static void queue_toggle_edit(lv_event_t * e) {
    queue_actions_hide(e); queue_editing = !queue_editing; populate_queue_screen();
}
static void queue_start(lv_event_t * e, bool shuffle) {
    queue_actions_hide(e);
    int * order = NULL, count, current;
    uint64_t revision;
    if (!gui_player_queue_snapshot(&order, &count, &current, &revision)) return;
    if (!count) { free(order); return; }
    char ** paths = calloc((size_t) count, sizeof(*paths));
    if (!paths) { free(order); return; }
    bool ok = true;
    for (int i = 0; i < count; i++) {
        const char * path = gui_player_get_track_path_at(order[i]);
        paths[i] = path ? strdup(path) : NULL;
        if (!paths[i]) { ok = false; break; }
    }
    free(order);
    if (!ok) { for (int i = 0; i < count; i++) free(paths[i]); free(paths); return; }
    gui_player_set_play_mode(shuffle ? PLAY_MODE_SHUFFLE : PLAY_MODE_SEQUENTIAL);
    unsigned int seed = (unsigned int) time(NULL) ^ lv_tick_get();
    int selected = shuffle ? (int) (rand_r(&seed) % (unsigned int) count) : 0;
    clear_player_source();
    on_file_selected(paths, count, selected);
}
static void queue_start_sequential(lv_event_t * e) { queue_start(e, false); }
static void queue_start_shuffle(lv_event_t * e) { queue_start(e, true); }
static void queue_clear_cb(lv_event_t * e) {
    queue_actions_hide(e); gui_player_queue_clear_upcoming(); populate_queue_screen();
}
static void queue_save_done(const char * name, void * data) {
    (void) data;
    show_info_toast(gui_player_queue_save_as(name) ? "Playlist saved" : "Cannot save: name exists, invalid, or streaming entries");
}
static void queue_save_cb(lv_event_t * e) {
    queue_actions_hide(e);
    show_text_entry("Save Queue as Playlist", "", false, false, queue_save_done, NULL);
}
static void queue_actions_open(lv_event_t * e) {
    (void) e;
    lv_obj_remove_flag(queue_actions_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(queue_actions, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(queue_actions_backdrop);
    lv_obj_move_foreground(queue_actions);
}

static lv_obj_t * build_queue_screen(void) {
    lv_obj_t * title;
    lv_obj_t * screen = build_subsonic_list_screen("Queue", &title, &queue_list);
    lv_obj_t * menu = lv_label_create(screen);
    lv_label_set_text(menu, "Options");
    lv_obj_align(menu, LV_ALIGN_TOP_RIGHT, -20, STATUS_BAR_CLEARANCE + 18);
    lv_obj_add_flag(menu, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(menu, 12);
    lv_obj_add_event_cb(menu, queue_actions_open, LV_EVENT_CLICKED, NULL);
    static const menu_popup_row_t rows[] = {
        { "Start sequentially", queue_start_sequential, false },
        { "Shuffle from a random song", queue_start_shuffle, false },
        { "Edit / Done", queue_toggle_edit, false },
        { "Clear Upcoming", queue_clear_cb, false },
        { "Save as Playlist", queue_save_cb, false },
        { "Cancel", queue_actions_hide, false },
    };
    queue_actions = build_menu_popup(rows, sizeof(rows) / sizeof(rows[0]), queue_actions_hide, &queue_actions_backdrop);
    return screen;
}

void open_queue_screen(void) {
    int * order = NULL, count, current;
    uint64_t revision;
    if (gui_player_queue_snapshot(&order, &count, &current, &revision)) {
        queue_page = current >= 0 ? (current / QUEUE_PAGE_SIZE) * QUEUE_PAGE_SIZE : 0;
        free(order);
    }
    queue_editing = false;
    populate_queue_screen();
    nav_push(queue_screen);
    int row = displayed_current - queue_page + 1 + (queue_page > 0 ? 1 : 0);
    lv_obj_update_layout(queue_list);
    if (row >= 0 && row < (int) lv_obj_get_child_count(queue_list))
        lv_obj_scroll_to_view(lv_obj_get_child(queue_list, row), LV_ANIM_OFF);
}

void gui_queue_poll(void) {
    static uint64_t saved_revision;
    static uint32_t last_checkpoint;
    uint32_t now = lv_tick_get();
    uint64_t revision = gui_player_queue_revision();
    if (queue_screen && lv_screen_active() == queue_screen && displayed_revision != revision) {
        int32_t scroll = lv_obj_get_scroll_y(queue_list);
        populate_queue_screen();
        lv_obj_scroll_to_y(queue_list, scroll, LV_ANIM_OFF);
    }
    if (now - last_checkpoint >= 30000 || (revision != saved_revision && now - last_checkpoint >= 1000)) {
        gui_player_queue_checkpoint();
        last_checkpoint = now; saved_revision = revision;
    }
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

static void song_context_menu_play_next_cb(lv_event_t * e) {
    hide_song_context_menu_popup();
    if (song_context_menu_target_path[0]) gui_player_queue_play_next(song_context_menu_target_path);
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
        { "Play Next", song_context_menu_play_next_cb, false },
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
    if (queue_actions) { lv_obj_delete(queue_actions); queue_actions = NULL; }
    if (queue_actions_backdrop) { lv_obj_delete(queue_actions_backdrop); queue_actions_backdrop = NULL; }
    if (song_context_menu_popup) { lv_obj_del(song_context_menu_popup); song_context_menu_popup = NULL; }
    if (song_context_menu_popup_backdrop) { lv_obj_del(song_context_menu_popup_backdrop); song_context_menu_popup_backdrop = NULL; }
    if (queue_screen) { lv_obj_del(queue_screen); queue_screen = NULL; }
    queue_list = NULL;
}
