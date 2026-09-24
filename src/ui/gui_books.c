#include "gui_books.h"
#include "gui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <stdatomic.h>
#include "lvgl/lvgl.h"
#include "assets.h"
#include "screen_builders.h"
#include "metadata_db.h"
#include "text_reader.h"
#include "plugin_manager.h"

extern lv_style_t style_theme_screen_bg;
extern lv_style_t style_theme_text_primary;
extern lv_style_t style_theme_text_secondary;
extern lv_style_t style_theme_row;
extern lv_style_t style_theme_text_muted;
extern lv_style_t style_theme_list_padding;
extern lv_style_t style_button_pressed;

extern const char * basename_of(const char * path);

/* gui_font_role_t defined in gui_theme.h */

#define BOOKS_SCAN_TIMEOUT_MS 8000
extern void nav_push(lv_obj_t * screen);
extern void nav_pop(void);
extern void nav_reset_to_home(void);
extern void finalize_screen_navigation(lv_obj_t * screen);
extern void generic_back_cb(lv_event_t * e);
extern void show_error_toast(const char * msg);
extern void show_info_toast(const char * msg);

#ifdef HOST_BUILD
#define BOOKS_ROOT_DIR "./music/Books"
#else
#define BOOKS_ROOT_DIR "/data/mnt/sd_0/Books"
#endif

static lv_obj_t * books_screen = NULL;

static void rescan_books(void);
static void populate_books_files_screen(void);

static char * text_reader_current_content = NULL; /* owned; replaced (freed) on every new file opened */
static char text_reader_current_path[600] = ""; /* the currently open book's path -- what the favorite icon below toggles */

static lv_obj_t * books_files_screen = NULL;
static lv_obj_t * books_files_list;
static lv_obj_t * books_files_title_label;
/* Which data source populate_books_files_screen() reads from -- set right
 * before nav_push()ing books_files_screen by whichever row (Books or
 * Favorites) opened it, see books_files_row_cb()/books_favorites_row_cb()
 * below. One shared screen/list for both, same as the player screen's
 * transport buttons being reused by the quick drawer -- these are
 * structurally identical (a flat list of book rows, tap to open), just
 * sourced differently. */
static bool books_showing_favorites = false;

static lv_obj_t * text_reader_screen;
static lv_obj_t * text_reader_title_label;
static lv_obj_t * text_reader_scroll;
static lv_obj_t * text_reader_content_label;
static lv_obj_t * text_reader_favorite_icon;

typedef struct {
    char root[600];
    atomic_bool done;
    bool ok;
    char ** paths;
    int count;
} books_scan_work_t;

static void refresh_text_reader_favorite_icon(void) {
    bool is_favorite = text_reader_current_path[0] != '\0' && metadata_db_book_favorite_is_set(text_reader_current_path);
    lv_image_set_src(text_reader_favorite_icon,
                     asset_path(is_favorite ? "playing_plane/collect_in.png" : "playing_plane/collect_out.png"));
}

static void text_reader_favorite_icon_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (text_reader_current_path[0] == '\0') return;
    bool is_favorite = metadata_db_book_favorite_is_set(text_reader_current_path);
    metadata_db_book_favorite_set(text_reader_current_path, !is_favorite);
    refresh_text_reader_favorite_icon();
}

static void open_text_reader(const char * path) {
    free(text_reader_current_content);
    bool truncated = false;
    text_reader_current_content = text_reader_load(path, &truncated);

    if (!text_reader_current_content) {
        lv_label_set_text(text_reader_content_label, "Could not open this file.");
    } else if (truncated) {
        /* Prepend a plain-text note rather than reaching for a separate
         * toast/label widget -- simplest way to say "there's more" for a
         * feature this basic. */
        char * buf = malloc(strlen(text_reader_current_content) + 128);
        if (buf) {
            snprintf(buf, strlen(text_reader_current_content) + 128,
                     "[File truncated at %d KB -- showing the first part only]\n\n%s",
                     TEXT_READER_MAX_BYTES / 1024, text_reader_current_content);
            free(text_reader_current_content);
            text_reader_current_content = buf;
        }
        lv_label_set_text(text_reader_content_label, text_reader_current_content);
    } else {
        lv_label_set_text(text_reader_content_label, text_reader_current_content);
    }

    lv_label_set_text(text_reader_title_label, basename_of(path));
    snprintf(text_reader_current_path, sizeof(text_reader_current_path), "%s", path);
    refresh_text_reader_favorite_icon();
    lv_obj_scroll_to_y(text_reader_scroll, 0, LV_ANIM_OFF);
    nav_push(text_reader_screen);
}

static void books_file_row_cb(lv_event_t * e) {
    const char * path = (const char *) lv_event_get_user_data(e);
    open_text_reader(path);
}

static void * books_scan_worker(void * arg) {
    books_scan_work_t * w = (books_scan_work_t *) arg;
    w->ok = text_reader_scan_txt_files(w->root, &w->paths, &w->count);
    atomic_store_explicit(&w->done, true, memory_order_release);
    return NULL;
}

/* Starts a detached scan of root. The worker owns the returned item until
 * done is set; a caller that stops waiting before then must leak it (see
 * scan_all_songs_with_timeout()'s own comment). NULL if it could not start. */
static books_scan_work_t * books_scan_start(const char * root) {
    books_scan_work_t * w = calloc(1, sizeof(*w));
    if (!w) return NULL;
    snprintf(w->root, sizeof(w->root), "%s", root);

    pthread_t thread;
    if (pthread_create(&thread, NULL, books_scan_worker, w) != 0) {
        free(w);
        return NULL;
    }
    pthread_detach(thread);
    return w;
}

/* Publishes a finished scan to the book cache and frees it. A scan that
 * found no .txt files (including a missing Books folder) clears the list. */
static void books_scan_commit(books_scan_work_t * w) {
    metadata_db_book_replace_all(w->paths, w->count);
    for (int i = 0; i < w->count; i++) free(w->paths[i]);
    free(w->paths);
    free(w);
}

static void rescan_books(void) {
    books_scan_work_t * w = books_scan_start(BOOKS_ROOT_DIR);
    for (int waited_ms = 0; w && waited_ms < BOOKS_SCAN_TIMEOUT_MS; waited_ms += 20) {
        if (atomic_load_explicit(&w->done, memory_order_acquire)) {
            books_scan_commit(w);
            return;
        }
        usleep(20000);
    }
    if (w) fprintf(stderr, "Warning: timed out scanning %s for .txt files (possible filesystem corruption) -- treating as empty\n", BOOKS_ROOT_DIR);
    metadata_db_book_replace_all(NULL, 0);
}

/* Books screen refresh icon: the same scan as rescan_books(), polled from an
 * LVGL timer so the UI thread never sleeps on the SD card. */
#define BOOKS_REFRESH_POLL_MS 100
static lv_obj_t * books_refresh_icon;
static lv_timer_t * books_refresh_timer;
static books_scan_work_t * books_refresh_work;
static uint32_t books_refresh_started;

static void books_refresh_finish(void) {
    if (books_refresh_timer) { lv_timer_delete(books_refresh_timer); books_refresh_timer = NULL; }
    books_refresh_work = NULL;
    set_header_refresh_action_busy(books_refresh_icon, false);
}

static void books_refresh_timer_cb(lv_timer_t * timer) {
    (void) timer;
    books_scan_work_t * w = books_refresh_work;
    if (!w) { books_refresh_finish(); return; }
    if (atomic_load_explicit(&w->done, memory_order_acquire)) {
        books_scan_commit(w);
        books_refresh_finish();
        if (lv_screen_active() == books_files_screen) populate_books_files_screen();
        show_info_toast("Books refreshed");
        return;
    }
    if (lv_tick_elaps(books_refresh_started) < BOOKS_SCAN_TIMEOUT_MS) return;
    /* Same policy as rescan_books(); the worker still owns w. */
    fprintf(stderr, "Warning: timed out scanning %s for .txt files (possible filesystem corruption) -- treating as empty\n", BOOKS_ROOT_DIR);
    metadata_db_book_replace_all(NULL, 0);
    books_refresh_finish();
    if (lv_screen_active() == books_files_screen) populate_books_files_screen();
    show_error_toast("Could not read the Books folder");
}

static void books_refresh_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || books_refresh_work) return;
    books_refresh_work = books_scan_start(BOOKS_ROOT_DIR);
    if (!books_refresh_work) {
        show_error_toast("Could not refresh books");
        return;
    }
    books_refresh_started = lv_tick_get();
    books_refresh_timer = lv_timer_create(books_refresh_timer_cb, BOOKS_REFRESH_POLL_MS, NULL);
    set_header_refresh_action_busy(books_refresh_icon, true);
}

static void free_user_data_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_DELETE) {
        void * data = lv_event_get_user_data(e);
        free(data);
    }
}

static void populate_books_files_screen(void) {
    lv_obj_clean(books_files_list);
    lv_label_set_text(books_files_title_label, books_showing_favorites ? "Favorites" : "Books");

    char ** paths;
    int count;
    /* Reads from the persistent book cache (metadata_db.c), populated
     * during library scan, rather than performing an SD card directory
     * walk. */
    if (books_showing_favorites) {
        metadata_db_load_favorite_books(&paths, &count);
    } else {
        metadata_db_load_all_books(&paths, &count);
    }

    if (count == 0) {
        lv_obj_t * label = lv_label_create(books_files_list);
        lv_label_set_text(label, books_showing_favorites ? "No favorites yet" : "No .txt files found");
        lv_obj_add_style(label, &style_theme_text_muted, 0);
        lv_obj_set_style_pad_left(label, BOARD_SCALE_PX(24), 0);
        free(paths);
        return;
    }

    for (int i = 0; i < count; i++) {
        lv_obj_t * row = lv_obj_create(books_files_list);
        lv_obj_set_size(row, LIST_ROW_WIDTH, LIST_ROW_HEIGHT);
        lv_obj_set_style_radius(row, LIST_ROW_RADIUS, 0);
        lv_obj_set_style_bg_color(row, LIST_ROW_BG_COLOR, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t * label = lv_label_create(row);
        lv_label_set_text(label, basename_of(paths[i]));
        lv_obj_add_style(label, &style_theme_text_primary, 0);
        lv_obj_set_style_text_font(label, &LIST_ROW_FONT, 0);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, LIST_ROW_LABEL_INSET, 0);

        /* paths[i] itself becomes the row's user_data -- ownership passes
         * to the row's event callback closure for as long as this screen
         * exists (the array is only ever rebuilt by lv_obj_clean() above,
         * which destroys these rows and their callbacks together, so
         * there's no dangling-pointer window). The char** array holding
         * them is freed here since each element's ownership already moved
         * to its row. */
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, books_file_row_cb, LV_EVENT_CLICKED, paths[i]);
        lv_obj_add_event_cb(row, free_user_data_event_cb, LV_EVENT_DELETE, paths[i]);
    }
    free(paths);
}

static void books_files_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    books_showing_favorites = false;
    populate_books_files_screen();
    nav_push(books_files_screen);
}

static void books_favorites_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    books_showing_favorites = true;
    populate_books_files_screen();
    nav_push(books_files_screen);
}

static lv_obj_t * build_books_files_screen(void) {
    lv_obj_t * scr = build_subsonic_list_screen("Books", &books_files_title_label, &books_files_list);
    books_refresh_icon = build_header_refresh_action(scr, books_refresh_cb);
    return scr;
}

static lv_obj_t * build_text_reader_screen(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &style_theme_screen_bg, 0);

    text_reader_title_label = build_screen_header(scr, "", generic_back_cb, NULL, NULL);
    lv_obj_t * favorite_button = build_top_right_icon_button(scr,
        asset_path("playing_plane/collect_out.png"), text_reader_favorite_icon_event_cb);
    text_reader_favorite_icon = lv_obj_get_child(favorite_button, 0);

    text_reader_scroll = lv_obj_create(scr);
    lv_obj_set_size(text_reader_scroll, lv_pct(100),
                    lv_display_get_vertical_resolution(lv_display_get_default()) - STATUS_BAR_CLEARANCE -
                        TITLE_ROW_HEIGHT);
    lv_obj_align(text_reader_scroll, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(text_reader_scroll, 0, 0);
    lv_obj_set_style_border_width(text_reader_scroll, 0, 0);
    lv_obj_set_scroll_dir(text_reader_scroll, LV_DIR_VER);
    lv_obj_set_style_pad_all(text_reader_scroll, BOARD_SCALE_PX(16), 0);

    text_reader_content_label = lv_label_create(text_reader_scroll);
    lv_label_set_long_mode(text_reader_content_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(text_reader_content_label, lv_pct(100));
    lv_obj_add_style(text_reader_content_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(text_reader_content_label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_label_set_text(text_reader_content_label, "");

    finalize_screen_navigation(scr);
    /* Same reasoning as every other scrollable-content screen in this
     * file: a drag inside the text itself should scroll the text, not
     * bubble up as an app-wide swipe. */
    lv_obj_remove_flag(text_reader_scroll, LV_OBJ_FLAG_GESTURE_BUBBLE);
    return scr;
}

static void plugin_books_list_item_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    plugin_manager_books_list_item_clicked(index);
}

static lv_obj_t * build_books_screen(void) {
    static pill_list_item_t items[2 + PLUGIN_MAX_BOOKS_LIST_ITEMS];
    lv_obj_t * native_rows[2 + PLUGIN_MAX_BOOKS_LIST_ITEMS] = { NULL };
    items[0] = (pill_list_item_t){ "Books", PILL_ACCESSORY_CHEVRON, false, books_files_row_cb, NULL, NULL };
    items[1] = (pill_list_item_t){ "Favorites", PILL_ACCESSORY_CHEVRON, false, books_favorites_row_cb, NULL, NULL };

    int count = 2;
    count = append_plugin_list_rows(items, count, PLUGIN_MAX_BOOKS_LIST_ITEMS,
                                    plugin_manager_get_books_list_item_count,
                                    plugin_manager_get_books_list_item_label,
                                    plugin_manager_get_books_list_item_options,
                                    plugin_books_list_item_click_cb);

    for (int i = 0; i < count; ++i) items[i].out_row = &native_rows[i];
    int icon_percent = (BOARD_SCALE_PX(44) * 100 + PILL_ROW_ICON_PX_DEFAULT - 1) / PILL_ROW_ICON_PX_DEFAULT;
    lv_obj_t * scr = build_pill_list_screen("Books", generic_back_cb, items, count, gui_theme_accent_style(), GUI_ROW_GAP, icon_percent);
    if (native_rows[0]) decorate_category_row(native_rows[0], "submenu/books.png", NULL);
    if (native_rows[1]) decorate_category_row(native_rows[1], "submenu/favorites.png", NULL);
    for (int i = 2; i < count; ++i) {
        if (!native_rows[i]) continue;
        if (items[i].icon_asset) {
            lv_obj_t * label = lv_obj_get_child(native_rows[i], 0);
            lv_obj_t * icon = lv_obj_get_child(native_rows[i], 1);
            lv_obj_align(icon, LV_ALIGN_LEFT_MID, BOARD_SCALE_PX(28), 0);
            lv_obj_align(label, LV_ALIGN_LEFT_MID, BOARD_SCALE_PX(96), 0);
            lv_obj_update_layout(native_rows[i]);
            configure_scrolling_row_label(label, lv_obj_get_width(native_rows[i]) - BOARD_SCALE_PX(96) - 60);
        }
        decorate_category_row(native_rows[i], NULL, NULL);
    }
    for (int i = 0; i < count; ++i) items[i].out_row = NULL;
    finalize_screen_navigation(scr);
    return scr;
}

void gui_books_home_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(books_screen);
}


bool gui_books_init(void) {
    books_files_screen = build_books_files_screen();
    text_reader_screen = build_text_reader_screen();
    books_screen = build_books_screen();
    return true;
}

/* For gui_reload.c's in-process UI reload -- deletes every screen this
 * module owns so gui_books_init() can rebuild them from a clean slate
 * without leaking the old objects. */
void gui_books_teardown(void) {
    /* An in-flight refresh is abandoned: freed if already finished, else left
     * to its detached worker. */
    if (books_refresh_work && atomic_load_explicit(&books_refresh_work->done, memory_order_acquire)) {
        for (int i = 0; i < books_refresh_work->count; i++) free(books_refresh_work->paths[i]);
        free(books_refresh_work->paths);
        free(books_refresh_work);
    }
    books_refresh_finish();
    books_refresh_icon = NULL;
    if (books_files_screen) { lv_obj_delete(books_files_screen); books_files_screen = NULL; }
    if (text_reader_screen) { lv_obj_delete(text_reader_screen); text_reader_screen = NULL; }
    if (books_screen) { lv_obj_delete(books_screen); books_screen = NULL; }
}

void gui_books_rescan(void) {
    rescan_books();
}

lv_obj_t * gui_books_get_screen(void) {
    return books_screen;
}
