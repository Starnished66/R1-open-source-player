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
#include "gui.h"

extern lv_style_t style_theme_screen_bg;
extern lv_style_t style_theme_text_primary;
extern lv_style_t style_theme_text_secondary;
extern lv_style_t style_theme_row;
extern lv_style_t style_theme_text_muted;
extern lv_style_t style_theme_list_padding;
extern lv_style_t style_button_pressed;

extern const char * basename_of(const char * path);
extern lv_obj_t * build_subsonic_list_screen(const char * title_text, lv_obj_t ** out_title_label, lv_obj_t ** out_list);

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

static bool books_scan_txt_files_with_timeout(const char * root, char *** out_paths, int * out_count) {
    books_scan_work_t * w = calloc(1, sizeof(*w));
    snprintf(w->root, sizeof(w->root), "%s", root);

    pthread_t thread;
    if (pthread_create(&thread, NULL, books_scan_worker, w) != 0) {
        free(w);
        return false;
    }
    pthread_detach(thread); /* never joined either way -- see scan_all_songs_with_timeout()'s own comment */

    for (int waited_ms = 0; waited_ms < BOOKS_SCAN_TIMEOUT_MS; waited_ms += 20) {
        if (atomic_load_explicit(&w->done, memory_order_acquire)) {
            bool ok = w->ok;
            *out_paths = w->paths;
            *out_count = w->count;
            free(w);
            return ok;
        }
        usleep(20000);
    }

    fprintf(stderr, "Warning: timed out scanning %s for .txt files (possible filesystem corruption) -- treating as empty\n", root);
    return false;
}

static void rescan_books(void) {
    char ** paths = NULL;
    int count = 0;
    books_scan_txt_files_with_timeout(BOOKS_ROOT_DIR, &paths, &count);

    metadata_db_book_replace_all(paths, count);

    for (int i = 0; i < count; i++) free(paths[i]);
    free(paths);
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
    /* Real-device bug report: this screen was slow to open every time -- a
     * live recursive readdir()+stat() walk of the whole SD card on every
     * visit (see books_scan_txt_files_with_timeout()'s own comment). Reads
     * from the persistent book cache now instead (metadata_db.c), kept
     * fresh only by rescan_books() (folded into Settings > Update Music
     * Database) -- this screen itself never touches the filesystem or the
     * stock db at all anymore. */
    if (books_showing_favorites) {
        metadata_db_load_favorite_books(&paths, &count);
    } else {
        metadata_db_load_all_books(&paths, &count);
    }

    if (count == 0) {
        lv_obj_t * label = lv_label_create(books_files_list);
        lv_label_set_text(label, books_showing_favorites ? "No favorites yet" : "No .txt files found");
        lv_obj_add_style(label, &style_theme_text_muted, 0);
        lv_obj_set_style_pad_left(label, 24, 0);
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
    return scr;
}

static lv_obj_t * build_text_reader_screen(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &style_theme_screen_bg, 0);

    lv_obj_t * back_btn = lv_obj_create(scr);
    lv_obj_set_size(back_btn, 64, 64);
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, 0, STATUS_BAR_CLEARANCE);
    lv_obj_set_style_bg_opa(back_btn, 0, 0);
    lv_obj_set_style_border_width(back_btn, 0, 0);
    lv_obj_remove_flag(back_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(back_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back_btn, generic_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * back_arrow = lv_image_create(back_btn);
    lv_image_set_src(back_arrow, asset_path("sub_back/btn_back.png"));
    lv_obj_center(back_arrow);

    /* Favorite toggle, top-right -- same collect_in/collect_out asset pair
     * and click-to-toggle shape as the player screen's own favorite_icon,
     * just persisted for real here (metadata_db_book_favorite_set()) rather
     * than that one's purely in-memory, never-saved toggle. */
    text_reader_favorite_icon = lv_image_create(scr);
    lv_image_set_src(text_reader_favorite_icon, asset_path("playing_plane/collect_out.png"));
    lv_obj_align(text_reader_favorite_icon, LV_ALIGN_TOP_RIGHT, -20, STATUS_BAR_CLEARANCE + (TITLE_ROW_HEIGHT - 28) / 2);
    lv_obj_add_flag(text_reader_favorite_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(text_reader_favorite_icon, 16);
    lv_obj_add_event_cb(text_reader_favorite_icon, text_reader_favorite_icon_event_cb, LV_EVENT_CLICKED, NULL);

    text_reader_title_label = lv_label_create(scr);
    lv_label_set_text(text_reader_title_label, "");
    lv_obj_align(text_reader_title_label, LV_ALIGN_TOP_MID, 0, STATUS_BAR_CLEARANCE + (TITLE_ROW_HEIGHT - 28) / 2);
    lv_obj_add_style(text_reader_title_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(text_reader_title_label, &app_font_28, 0); /* shows the (possibly non-Latin) file's own name -- see fallback_font.h */
    /* Narrower than build_subsonic_list_screen()'s equivalent titles (70%
     * -- this one has both the back button AND the favorite icon eating
     * into its available width) -- same width-capped dot-scroll treatment
     * as the player screen's own title label either way. */
    lv_obj_set_width(text_reader_title_label, lv_pct(55));
    lv_label_set_long_mode(text_reader_title_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(text_reader_title_label, LV_TEXT_ALIGN_CENTER, 0);

    text_reader_scroll = lv_obj_create(scr);
    lv_obj_set_size(text_reader_scroll, lv_pct(100),
                    lv_display_get_vertical_resolution(lv_display_get_default()) - STATUS_BAR_CLEARANCE -
                        TITLE_ROW_HEIGHT);
    lv_obj_align(text_reader_scroll, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(text_reader_scroll, 0, 0);
    lv_obj_set_style_border_width(text_reader_scroll, 0, 0);
    lv_obj_set_scroll_dir(text_reader_scroll, LV_DIR_VER);
    lv_obj_set_style_pad_all(text_reader_scroll, 16, 0);

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
    items[0] = (pill_list_item_t){ "Books", PILL_ACCESSORY_CHEVRON, false, books_files_row_cb, NULL, NULL };
    items[1] = (pill_list_item_t){ "Favorites", PILL_ACCESSORY_CHEVRON, false, books_favorites_row_cb, NULL, NULL };

    int count = 2;
    int plugin_count = plugin_manager_get_books_list_item_count();
    for (int i = 0; i < plugin_count && i < PLUGIN_MAX_BOOKS_LIST_ITEMS; i++) {
        pill_list_item_t item = {
            plugin_manager_get_books_list_item_label(i), PILL_ACCESSORY_CHEVRON, false,
            plugin_books_list_item_click_cb, NULL, (void *) (intptr_t) i
        };
        const char * text_size = NULL;
        plugin_manager_get_books_list_item_options(i, &item.icon_asset, &item.row_height, &item.row_width, &text_size);
        item.text_size = text_size ? text_size : "medium"; /* see pill_row_resolve_text_size()'s own comment on why plugin rows always supply a non-NULL default */
        items[count++] = item;
    }

    lv_obj_t * scr = build_pill_list_screen("Books", generic_back_cb, items, count, gui_theme_accent_style(), 6);
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
    if (books_files_screen) { lv_obj_del(books_files_screen); books_files_screen = NULL; }
    if (text_reader_screen) { lv_obj_del(text_reader_screen); text_reader_screen = NULL; }
    if (books_screen) { lv_obj_del(books_screen); books_screen = NULL; }
}

void gui_books_rescan(void) {
    rescan_books();
}

void gui_books_show(void) {
    nav_push(books_screen);
}

lv_obj_t * gui_books_get_screen(void) {
    return books_screen;
}
