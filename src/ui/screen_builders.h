#ifndef SCREEN_BUILDERS_H
#define SCREEN_BUILDERS_H

#include "lvgl/lvgl.h"
#include "fallback_font.h"
#include <stdbool.h>

/* Settings -> Font Size tier pointers -- see gui.c's own top-of-file
 * comment (search "ui_size_16") for the full story. Defined in gui.c,
 * set once by apply_font_size_tier() before any screen (including these
 * builders) runs. Only for fixed English UI chrome that never shows
 * metadata-derived text -- see fallback_font.h's app_font_16/22/28 for
 * the other category. */
extern const lv_font_t * ui_size_16;
extern const lv_font_t * ui_size_20;
extern const lv_font_t * ui_size_22;
extern const lv_font_t * ui_size_28;

/* Two screen layouts repeat across the real stock UI: an icon grid (Home
 * launcher, Music/Wireless/Stream Media submenus) and a scrollable list of
 * pill-shaped rows (System settings, Books). Both builders are pure layout
 * -- navigation (back button behavior, swipe gestures) is wired by the
 * caller afterwards, same as every other screen in gui.c, so this module
 * has no dependency on the nav stack. */

/* Shared header-band heights -- every screen, including the hand-built ones
 * in gui.c that don't go through a builder here (player, accent color, EQ,
 * text entry, group songs, subsonic lists), reserves these so its own
 * content never competes with the persistent status bar (clock/battery/
 * wifi, drawn separately on lv_layer_top()) for the same pixels. */
#define STATUS_BAR_CLEARANCE 48
#define TITLE_ROW_HEIGHT 64

/* Shared touch-list row geometry -- every tappable row-of-text list
 * (Artists/Albums/Album Artist/Genres/All Songs/group-songs drill-down,
 * Files, Wi-Fi/Bluetooth scan results, Subsonic browsing) uses these same
 * dimensions, rather than each screen picking its own, so the whole app
 * reads as one consistent list style. Sized up from the original stock-
 * asset-matched 448x60 (real-hardware feedback: too small/cramped to
 * reliably tap on the actual device). Since these no longer match any real
 * stock touch_list bg PNG asset's pixel dimensions, rows are now a plain
 * rounded rect (LIST_ROW_BG_COLOR/LIST_ROW_RADIUS) instead of that PNG --
 * LVGL's bg_image style draws a bg_image_src at its native size centered in
 * the object's box (see lv_draw_rect.c), not stretched to fill it, so
 * reusing the old asset at a bigger row size would've just centered the
 * original small pill inside a bigger empty box. LIST_ROW_BG_COLOR matches
 * that PNG's own fill color (sampled directly from the asset) so the look
 * carries over despite the switch. */
#define LIST_ROW_WIDTH 464
/* Wider rows for Files/Artists/Album Artist/Albums/All Songs/Playlists --
 * explicit user request for a bigger tap target (real-device feedback: the
 * default width read as cramped). NOT a flat +15%: this device's screen is
 * only 480px wide (SCREEN_WIDTH, main.c), and LIST_ROW_WIDTH already uses
 * 464 of those, so a literal 15% bump (533px) would overflow the physical
 * display by 53px. 476px is the practical max -- edge-to-edge minus a thin
 * 4px total margin -- confirmed with the user after flagging the overflow.
 * Rows still use the shared plain-rounded-rect list_row_style (see
 * build_compact_list_widget()'s per-row width override) rather than a new
 * style, so height/radius/color/font stay identical -- only the box widens,
 * and callers passing this must also make sure their row is horizontally
 * centered/positioned so it doesn't clip past the container's own edge
 * (see build_compact_list_widget()'s row_x and file_browser.c's/
 * build_playlists_screen()'s explicit flex-center alignment). */
#define LIST_ROW_WIDTH_WIDE 476
#define LIST_ROW_HEIGHT 84
#define LIST_ROW_RADIUS 16
#define LIST_ROW_BG_COLOR lv_color_make(28, 28, 30)
#define LIST_ROW_FONT app_font_22 /* see fallback_font.h -- same metrics as lv_font_montserrat_22, plus a non-Latin fallback */
#define LIST_ROW_LABEL_INSET 28

/* Every row-of-text list in the app (build_compact_list_screen below,
 * plus gui.c's show_group_songs()/populate_indexed_list(), all built by
 * hand against the LIST_ROW_* geometry above) used to set size/radius/
 * bg/border/pad/text-color/text-font as six-plus individual local style
 * properties on TWO objects per row (a container plus a child label).
 * Real-device feedback: opening a ~2000-song "All Songs" list took ~5
 * seconds, almost entirely construction cost. One shared lv_style_t
 * attached via lv_obj_add_style() is far cheaper per row than the same
 * properties set locally -- LVGL's local-style storage is a small
 * allocated array per object per property, where a shared style is just
 * one pointer appended to the object's style list. Merging the row+label
 * into a single lv_label (a label is a plain lv_obj subclass, so it can
 * be sized/styled/clicked exactly like the old container) halves the
 * object count on top of that. pad_top is baked in here (not computed
 * per row) to vertically center LIST_ROW_FONT's single-line text within
 * LIST_ROW_HEIGHT, since a label draws text from its own top edge by
 * default. Call screen_builders_init_list_row_style() once, before
 * building any list screen (gui_init() does this). */
extern lv_style_t list_row_style;
void screen_builders_init_list_row_style(void);

typedef struct {
    const char * icon_asset;          /* e.g. "launcher/music.png" */
    const char * icon_asset_selected; /* "_s" variant, e.g. "launcher/music_s.png" */
    const char * label;
    lv_event_cb_t on_click;           /* fired on LV_EVENT_CLICKED, may be NULL */
    void * user_data;
} icon_grid_item_t;

/* Titled screen: real back-arrow button (top-left, invokes back_btn_cb) and
 * a flex-wrap grid of icon tiles below, each swapping to its "_s" asset
 * while pressed for visual feedback. Caller owns navigation wiring
 * (nav_pop/nav_push, swipe gestures) via finalize_screen_navigation() after
 * this returns. icon_scale_percent scales ICON_GRID_TARGET_ICON_PX for this
 * screen only (100 = the shared default every other icon-grid screen uses;
 * pass a value above/below 100 to make just this screen's icons bigger or
 * smaller without affecting the others). */
lv_obj_t * build_icon_grid_screen(const char * title, lv_event_cb_t back_btn_cb,
                                   const icon_grid_item_t * items, int item_count,
                                   int32_t icon_scale_percent);

typedef enum {
    PILL_ACCESSORY_NONE = 0,
    PILL_ACCESSORY_CHEVRON, /* no matching real asset found -- plain ">" label */
    PILL_ACCESSORY_TOGGLE,  /* real settings/on.png + settings/off.png sprite swap */
} pill_accessory_type_t;

typedef struct {
    const char * label;
    pill_accessory_type_t accessory;
    bool toggle_initial_state;      /* only used when accessory == PILL_ACCESSORY_TOGGLE */
    lv_event_cb_t on_click;         /* fired on row LV_EVENT_CLICKED (NONE/CHEVRON rows) */
    lv_event_cb_t on_toggle_change; /* fired on LV_EVENT_VALUE_CHANGED (TOGGLE rows) */
    void * user_data;
} pill_list_item_t;

/* Titled screen: real back-arrow button and a vertically scrollable list of
 * touch_list/item_bg.png pill rows, each with a label and an optional
 * right-side chevron or toggle. toggle_accent_style is applied (via
 * lv_obj_add_style(), not read as a plain value) to every PILL_ACCESSORY_
 * TOGGLE row's on.png/off.png sprite -- a style pointer rather than a
 * resolved lv_color_t like build_compact_list_widget()'s own
 * now_playing_color, specifically because these pill-list screens are each
 * built once at startup and never rebuilt (see e.g.
 * build_timezone_region_screen()'s own comment), unlike the list screens
 * now_playing_color feeds -- a plain color captured once here would go
 * stale forever after the very next accent color change, where a shared,
 * in-place-updated style (gui.c's style_accent, kept live via
 * lv_obj_report_style_change()) doesn't. Caller owns the style object's
 * lifetime; screen_builders.c never reads its properties directly, only
 * attaches it (same "no visibility into gui.c's accent state" boundary as
 * now_playing_color's own doc comment describes). */
lv_obj_t * build_pill_list_screen(const char * title, lv_event_cb_t back_btn_cb,
                                   const pill_list_item_t * items, int item_count,
                                   lv_style_t * toggle_accent_style);

typedef struct {
    const char * label;
} compact_list_item_t;

/* Fired when a row is tapped, with the index into the `items` array passed
 * to build_compact_list_screen() -- not an lv_event_cb_t, since the
 * virtualized row widgets underneath (see screen_builders.c) are reused
 * across many different indices over the list's lifetime, so there's no
 * single fixed LVGL event user_data to bind an index to at row-creation
 * time the way a one-widget-per-item list could. */
typedef void (*compact_list_click_cb_t)(int index);

/* Titled screen: real back-arrow button and a vertically scrollable,
 * virtualized list of compact 84px rows (no icon, no accessory -- just a
 * label). Virtualized because a flat list can be the entire local library
 * (thousands of songs): only a small pool of row widgets actually exists
 * at once, repositioned and relabeled as the list scrolls, rather than one
 * real LVGL object per item -- real-device feedback: building one row
 * object per song made opening "All Songs" against a ~2000-song library
 * visibly freeze for several seconds, almost entirely LVGL's own one-time
 * layout cost for that many objects. `items` (and the strings its `label`
 * pointers reference) must stay valid for the screen's entire lifetime --
 * this function keeps its own copy of the `items` array itself (freed
 * automatically when the screen is deleted), but not of the label strings
 * themselves, matching how every existing caller already sources labels
 * from long-lived backing arrays (song tags, artist/album group names)
 * that only go away alongside a full library rescan, at which point these
 * screens get torn down and rebuilt too. */
lv_obj_t * build_compact_list_screen(const char * title, lv_event_cb_t back_btn_cb,
                                      const compact_list_item_t * items, int item_count,
                                      compact_list_click_cb_t on_click, lv_obj_t ** out_list,
                                      int32_t row_width, bool enable_now_playing, lv_color_t now_playing_color);

/* The virtualized list widget itself (what build_compact_list_screen()
 * builds internally), with no screen/back-button/title wrapper -- for a
 * caller that already has its own screen and wants to attach a compact
 * list directly onto it (e.g. a search-results overlay layered on top of
 * an existing screen's own UI). Returns the list object directly (not a
 * screen) -- caller owns positioning it via lv_obj_align()/lv_obj_set_size().
 * row_width overrides list_row_style's own LIST_ROW_WIDTH per-instance (pass
 * LIST_ROW_WIDTH for the shared default, or LIST_ROW_WIDTH_WIDE to match a
 * widened parent screen -- e.g. the Files search overlay matching Files'
 * own now-wider rows). enable_now_playing/now_playing_color: see
 * compact_list_set_now_playing()'s own doc comment below -- pass false/
 * anything when a list has no now-playing concept (e.g. the Files search
 * overlay, the timezone city list). Color is threaded through as a
 * parameter rather than read directly (screen_builders.c has no visibility
 * into gui.c's current_settings.accent_color/accent_lv_color()). */
lv_obj_t * build_compact_list_widget(lv_obj_t * parent, const compact_list_item_t * items, int item_count,
                                      compact_list_click_cb_t on_click, int32_t row_width,
                                      bool enable_now_playing, lv_color_t now_playing_color);

/* Shows/moves/hides a thin accent-colored bar flush against the screen's
 * far-left edge (independent of row_width/any row's own inset -- this is a
 * "far left of the screen" indicator, not "far left of the row"), at the
 * same y as the row for `item_index` (an index into whatever `items` array
 * this list was built/last set with) -- item_index < 0 hides it. No-op if
 * this list wasn't built with enable_now_playing=true. Position tracks the
 * item directly (not the visible scroll window), so LVGL's own list
 * scrolling/clipping naturally shows or hides it as the matching row
 * scrolls in and out of view -- no separate scroll-event wiring needed. */
void compact_list_set_now_playing(lv_obj_t * list, int item_index);

/* Scrolls a build_compact_list_screen() list so item `index` lands at the
 * top of the viewport -- exposed as a semantic helper (rather than exposing
 * COMPACT_LIST_TOP_PAD/COMPACT_LIST_ROW_STRIDE themselves, private to
 * screen_builders.c) so callers like gui.c's A-Z browse index don't need to
 * know this list's internal row geometry. */
void compact_list_scroll_to_index(lv_obj_t * list, int index);

/* Swaps a build_compact_list_screen() list's contents in place -- used for
 * live search filtering -- without the delete()+rebuild this project
 * otherwise uses for changing a screen's list contents. */
void compact_list_set_items(lv_obj_t * list, const compact_list_item_t * items, int item_count);

#endif /* SCREEN_BUILDERS_H */
