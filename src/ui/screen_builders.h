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
int32_t ui_list_row_width(void);
int32_t ui_list_row_width_wide(void);
#define LIST_ROW_WIDTH (ui_list_row_width())
/* Wider rows for the main library lists. Both widths are calculated from
 * the active panel: ordinary rows retain a 4px/side gutter at 480px while
 * wide rows retain 2px/side. The gutter grows gently on larger panels. */
#define LIST_ROW_WIDTH_WIDE (ui_list_row_width_wide())
#define LIST_ROW_HEIGHT 84
#define MUSIC_LIST_ROW_HEIGHT 100
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
/* LV_STATE_PRESSED-only bg_color override for a list_row_style row -- attach
 * with lv_obj_add_style(row, &list_row_pressed_style, LV_STATE_PRESSED),
 * see screen_builders_init_list_row_style()'s own comment on why this is a
 * separate style rather than folded into list_row_style itself. */
extern lv_style_t list_row_pressed_style;
/* LV_STATE_PRESSED-only tap indicator for a bare lv_image icon button (no
 * background to recolor the way list_row_pressed_style does) -- dims the
 * icon via image_recolor/image_recolor_opa rather than swapping to a
 * "_s" pressed-state asset, so it works uniformly regardless of which of an
 * icon's several possible source images (play/pause, loop/random/single,
 * favorited/not) happens to be showing right now. Attach with
 * lv_obj_add_style(icon, &icon_press_style, LV_STATE_PRESSED) to any
 * LV_OBJ_FLAG_CLICKABLE image -- LVGL enters/exits LV_STATE_PRESSED on its
 * own during a touch, no event callback needed. */
extern lv_style_t icon_press_style;

/* Enables the shared constant-speed row marquee with a 2-second pause. */
void row_label_enable_marquee(lv_obj_t * label);
/* Theming: shared, mutable bg_color styles for the app's other two
 * "background categories" -- attach with lv_obj_add_style(x, &style_theme_screen_bg, 0)
 * on every screen root, or &style_theme_card_bg on every popup/EQ-card/
 * slider-card. Initialized (with this app's original default colors)
 * alongside list_row_style by screen_builders_init_list_row_style() below
 * -- see its own comment for why these three specifically, and why not
 * gui.c's style_accent. gui.c's gui_plugin_set_background_color() is what
 * actually mutates these later, in response to a plugin's
 * plugin.set_background_color() call. */
extern lv_style_t style_theme_screen_bg;
extern lv_style_t style_theme_card_bg;
/* Same live-mutable-style mechanism, for text color instead of background:
 * "primary" (the app's dominant near-white text -- labels, titles, list
 * rows) and "muted" (secondary/disabled-ish gray text -- chevrons,
 * timestamps, subtitles). Deliberately NOT covering destructive-red text
 * (delete/reset confirmations) or accent-tinted text (already tracks the
 * user's accent color pick independently) -- those are semantically fixed,
 * not part of the light/dark background split. list_row_style's own
 * text_color is a separate literal (not one of these two), since rows
 * attach list_row_style directly rather than a second style object for
 * text; gui.c's gui_plugin_set_text_color() mutates list_row_style's
 * text_color too when slot is "primary" so rows stay in sync. */
extern lv_style_t style_theme_text_primary;
extern lv_style_t style_theme_text_muted;
void screen_builders_init_list_row_style(void);

typedef struct {
    const char * icon_asset;          /* e.g. "launcher/music.png" */
    const char * icon_asset_selected; /* "_s" variant, e.g. "launcher/music_s.png" */
    const char * label;
    lv_event_cb_t on_click;           /* fired on LV_EVENT_CLICKED, may be NULL */
    void * user_data;

    /* ---- Optional per-tile style overrides (plugin.set_home_layout()'s
     * per-tile config table, PLUGINS.md) -- every native tile leaves these
     * false/0 (a plain compound literal without designators for trailing
     * fields already defaults them that way), reproducing today's exact
     * look. Same trailing-fields-are-safe convention pill_list_item_t's own
     * plugin-row extensions below already established. ---- */
    bool has_bg_color;   uint32_t bg_color;   /* 0xRRGGBB */
    bool has_text_color; uint32_t text_color; /* 0xRRGGBB */
    bool has_radius;     int32_t radius;      /* px corner radius */
} icon_grid_item_t;

/* Titled screen: real back-arrow button (top-left, invokes back_btn_cb) and
 * a flex-wrap grid of icon tiles below, each swapping to its "_s" asset
 * while pressed for visual feedback. Caller owns navigation wiring
 * (nav_pop/nav_push, swipe gestures) via finalize_screen_navigation() after
 * this returns. icon_scale_percent scales ICON_GRID_TARGET_ICON_PX for this
 * screen only (100 = the shared default every other icon-grid screen uses;
 * pass a value above/below 100 to make just this screen's icons bigger or
 * smaller without affecting the others).
 *
 * label_inside_icon: false (every icon set except Wireless's) draws the
 * caption below the icon, in its own reserved row -- the right choice for
 * plain glyph-on-transparent assets (category/launcher/stream_media). true
 * (Wireless only) instead draws the caption inside the icon's own image,
 * near its bottom -- the wireless/ star.png assets are themselves pre-baked
 * "card + glyph + reserved caption band" graphics (confirmed by scanning
 * their actual pixels: every one of them has its glyph content end around
 * 65% down a much taller canvas, with flat card background filling the
 * rest), so drawing a second, separate caption below them left that
 * reserved band sitting empty -- real-device bug report: "black space in
 * the bottom that was meant for the Text to fit inside". */
/* tile_gap: px of visible space between adjacent tiles (0 = today's exact
 * flush-cell look with its thin divider lines -- every native caller passes
 * 0). A positive value insets each tile within its own already-computed grid
 * cell via a real LVGL margin (tile_gap/2 on every side) rather than
 * touching the row-height grid math at all, and suppresses the flush-cell
 * divider lines (redundant, and visually conflicting, once real gaps exist).
 * Only plugin.set_home_layout()'s options.tile_gap (PLUGINS.md, tile mode)
 * ever passes a nonzero value. */
lv_obj_t * build_icon_grid_screen(const char * title, lv_event_cb_t back_btn_cb,
                                   const icon_grid_item_t * items, int item_count,
                                   int32_t icon_scale_percent, bool label_inside_icon, int32_t tile_gap);

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
    /* Optional (NULL for every row that doesn't need it): on a
     * PILL_ACCESSORY_TOGGLE row, *out_toggle_img is set to the toggle's own
     * lv_image_t once built, so a caller can update its sprite/LV_STATE_CHECKED
     * later from somewhere else entirely (e.g. a quick-drawer icon that
     * mirrors the same underlying setting) -- these screens are built once
     * at startup and never rebuilt, so nothing else keeps this row's visual
     * state in sync with the setting on its own. See gui.c's
     * settings_crossfade_toggle_img for the motivating real-device bug
     * (drawer/settings crossfade toggles falling out of sync). */
    lv_obj_t ** out_toggle_img;

    /* ---- Plugin-row extensions (plugin.register_list_item()'s optional
     * `options` table, PLUGINS.md) -- every existing native row across every
     * build_pill_list_screen() caller leaves these NULL/0 (a plain compound
     * literal without designators for trailing fields already defaults them
     * that way), which reproduces today's exact rendering: PNG pill
     * background, 124px height, ui_size_20 label font. Only a
     * plugin-appended row (gui.c's build_settings_*_screen() plugin-row
     * loops) ever sets these. ---- */

    /* Raw absolute filesystem path (e.g. "/data/mnt/sd_0/.plugins/icon.png"),
     * NOT a theme2-relative asset path -- a plugin's icon lives on the SD
     * card, not in this app's own theme directory. See pill_row_apply_icon()
     * below. NULL = no icon, today's exact layout. */
    const char * icon_asset;

    /* 0 = default (124px, PNG pill sprite). A non-zero value (clamped to
     * PILL_ROW_HEIGHT_MIN..MAX) resizes the row AND switches its background
     * from the PNG sprite to a plain rounded-rect fill -- see
     * PILL_ROW_HEIGHT_MIN's own comment for why the PNG can't just stretch. */
    int32_t row_height;

    /* 0 = the native 448px width. Non-zero values are clamped to the
     * plugin-safe range below and keep the row centered in its list. */
    int32_t row_width;

    /* NULL = this call site's own default (native rows: ui_size_20,
     * unchanged; plugin rows: gui.c always supplies a non-NULL default
     * before this reaches here -- see pill_row_resolve_text_size()'s own
     * comment). "small"/"medium"/"large" -> app_font_16/22/28
     * (fallback_font.h) -- the fallback-capable family, since plugin row
     * text (unlike native chrome) may be non-Latin. Validated by the Lua
     * boundary in plugin_manager.c; this layer trusts it's already one of
     * the three recognized values or NULL. */
    const char * text_size;

    /* ---- Optional per-tile style overrides, same fields/meaning as
     * icon_grid_item_t's own (plugin.set_home_layout()'s per-tile config
     * table, PLUGINS.md) -- every existing row leaves these false/0,
     * reproducing today's exact rendering. When has_bg_color/has_radius is
     * set, the row is forced onto the same plain rounded-rect fill path
     * row_height/row_width already use instead of the PNG pill sprite (a
     * raster asset can't be recolored), using these values in place of
     * LIST_ROW_BG_COLOR/LIST_ROW_RADIUS. ---- */
    bool has_bg_color;   uint32_t bg_color;
    bool has_text_color; uint32_t text_color;
    bool has_radius;     int32_t radius;

    /* NULL/"left" (default) = today's exact layout (label left-aligned,
     * starting right after any icon -- unaffected by this field). "center"/
     * "right" re-align the label alone within the row's own remaining width;
     * pill_row_apply_icon()'s icon placement is never touched by this.
     * plugin.set_home_layout() only, list mode only (PLUGINS.md) -- no other
     * caller sets this. */
    const char * text_align;
} pill_list_item_t;

/* Row-height bounds for a plugin-resized pill row (register_list_item()'s
 * `height` option), a plugin-resized show_list() row (gui_plugin_show_list()'s
 * own `height` option), or a plugin.set_home_layout() list-mode tile
 * (PLUGINS.md) -- shared range for all three, silently clamped rather than
 * erroring (same convention plugin.set_interval()'s own minimum-clamp uses).
 * Below PILL_ROW_ICON_PX_DEFAULT (64px): real-device request for genuinely
 * compact rows (a dense retro/pixel-menu look, PLUGINS.md's set_home_layout()
 * examples) -- a row with BOTH an icon and a height under 64px will clip the
 * icon's top/bottom against the row bounds (lv_obj_create()'s default
 * overflow clipping, never made visible on the host build since none of its
 * launcher icon assets actually decode there); an icon-less row (no
 * icon_asset, e.g. every plugin.set_home_layout() list tile) never hits
 * this. Ceiling keeps one row from dominating the screen. */
#define PILL_ROW_HEIGHT_MIN 48
#define PILL_ROW_HEIGHT_MAX 220
#define PILL_ROW_WIDTH_MIN 240
#define PILL_ROW_WIDTH_MAX (ui_list_row_width_wide())

/* Native width derived from the active display. */
int32_t pill_row_default_width(void);

/* On-screen icon footprint (longest edge, in px) a plugin row's icon targets
 * -- same scaling formula as build_icon_grid_screen()'s own
 * ICON_GRID_TARGET_ICON_PX, just a smaller target since this sits inside a
 * single row rather than a whole tile. Fits a 124px-tall default pill row
 * with real clearance (124-64=60px, 30px top/bottom). */
#define PILL_ROW_ICON_PX_DEFAULT 64

/* Adds a plugin-supplied icon to an already-built row and re-aligns `label`
 * to start after it -- shared by build_pill_list_screen() below (native
 * pill rows) and gui.c's own plugin-row builders (add_pill_row_base()'s
 * pill-toggle/-chevron rows, the settings-list slider row, gui_plugin_
 * show_list()'s icon rows), so the icon-loading/scaling logic exists in
 * exactly one place rather than four. No-op if icon_path is NULL --
 * `label`'s alignment is left completely untouched in that case, preserving
 * every existing call site's exact layout. `icon_px` is the on-screen
 * target size (longest edge); pass PILL_ROW_ICON_PX_DEFAULT unless a caller
 * has a specific reason not to. `align`/`x`/`y` are the alignment to apply
 * to BOTH the icon and (offset by icon_px + a 12px gap) the label -- most
 * rows are a single vertically-centered line (LV_ALIGN_LEFT_MID), but the
 * settings-list slider row's label sits top-left instead (its value label
 * and slider fill the rest of the card), so this isn't hardcoded. See this
 * file's own top-of-block comment for why icon_path is a raw filesystem
 * path, not a theme2-relative one. */
void pill_row_apply_icon(lv_obj_t * row, lv_obj_t * label, const char * icon_path, int32_t icon_px,
                          lv_align_t align, int32_t x, int32_t y);

/* Resolves a plugin's chosen text-size tier to the correspondingly-sized
 * fallback-capable font -- NULL returns ui_size_20 (build_pill_list_screen()'s
 * own native-row default; every plugin call site is responsible for
 * supplying its OWN non-NULL default -- e.g. "medium" -- before calling
 * this, precisely so NULL unambiguously means "a genuine native row" here,
 * never "a plugin row that happened not to set text_size"). "small"/
 * "medium"/"large" -> &app_font_16/22/28 (fallback_font.h). Any other
 * string (shouldn't happen -- plugin_manager.c validates at the Lua
 * boundary) falls back to ui_size_20 rather than dereferencing garbage. */
const lv_font_t * pill_row_resolve_text_size(const char * text_size);

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
/* row_gap: vertical spacing between rows, in px (every existing caller
 * passes 6, today's exact hardcoded value -- see build_pill_list_screen()'s
 * own history). Only plugin.set_home_layout()'s options.row_gap
 * (PLUGINS.md, list mode) ever passes anything else. */
lv_obj_t * build_pill_list_screen(const char * title, lv_event_cb_t back_btn_cb,
                                   const pill_list_item_t * items, int item_count,
                                   lv_style_t * toggle_accent_style, int32_t row_gap);

typedef struct {
    const char * label;
    int64_t identity;              /* optional stable DB/item identity for row decorators */
    const char * trailing_asset;   /* optional theme-relative badge asset */
} compact_list_item_t;

/* Fixed size of a paged-mode row label buffer (compact_list_fetch_page_cb_t
 * below) -- matches cached_tags_t.title's own size (metadata_db.h), the
 * largest display string a paged provider actually needs to carry. */
#define COMPACT_LIST_LABEL_MAX 128

typedef struct {
    char label[COMPACT_LIST_LABEL_MAX];
    int64_t identity;
    char trailing_asset[64];
} compact_list_page_row_t;

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
/* on_long_press: fired on LV_EVENT_LONG_PRESSED, same index-resolution
 * shape as on_click -- NULL for a list whose rows aren't individual songs
 * (Artists/Albums/Album Artist name rows, the Time Zone city list), so
 * there's nothing sensible to long-press into a context menu. Passed
 * straight through to build_compact_list_widget() below. */
/* out_title_label: NULL if the caller never needs to change the title text
 * after construction (every existing caller so far) -- pass a real pointer
 * only when the screen's title changes at runtime (e.g. a Subsonic list
 * screen reused for different artists/albums, whose title reflects
 * whichever one is currently shown). */
lv_obj_t * build_compact_list_screen(const char * title, lv_event_cb_t back_btn_cb,
                                      const compact_list_item_t * items, int item_count,
                                      compact_list_click_cb_t on_click, compact_list_click_cb_t on_long_press,
                                      lv_obj_t ** out_list, lv_obj_t ** out_title_label, int32_t row_width,
                                      bool enable_now_playing, lv_color_t now_playing_color);

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
                                      compact_list_click_cb_t on_click, compact_list_click_cb_t on_long_press,
                                      int32_t row_width, bool enable_now_playing, lv_color_t now_playing_color);

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

/* ---- Paged mode -- Rockbox-style DB-backed lists. An alternative to
 * compact_list_set_items() for a list whose full content is too large to
 * ever materialize at once (the local library's own All Songs/Artists/
 * Albums/Album Artist screens): instead of a caller-owned items[] array,
 * rows are fetched on demand, a bounded page at a time, from a callback --
 * the list still only ever holds ~20 real LVGL row objects (same
 * virtualization as eager mode), but now the underlying DATA is bounded
 * too, not just the widget count. ---- */

/* Fills out_labels[0..count) with the display label for logical rows
 * [offset, offset+count) (0-indexed into the full, currently-sorted/
 * filtered result set) -- e.g. a thin wrapper around metadata_db_get_
 * songs_page()/get_groups_page() that formats each row's own display
 * title/name into the fixed COMPACT_LIST_LABEL_MAX-sized buffer. Returns
 * the number of rows actually written (less than `count` only at the very
 * end of the result set -- ctx knows its own real total already via
 * whatever populated it, this isn't how the list learns the total; see
 * compact_list_set_paged_provider()'s own total_count argument for that).
 * Called from the LVGL/main thread only (same thread every other compact_
 * list callback runs on), so it's safe to call straight into metadata_db.c
 * (its own METADATA_DB_GUARD) or any other main-thread-only state. */
typedef int (*compact_list_fetch_page_cb_t)(void * ctx, int offset, int count,
                                             compact_list_page_row_t out_rows[]);

/* Optional visible-row decoration hook. Called only on the LVGL thread for
 * the fixed recycled row pool. `leading_image` is owned by the list and may
 * be assigned a static asset or a caller-owned descriptor whose lifetime
 * exceeds the assignment. */
typedef void (*compact_list_row_decorator_cb_t)(lv_obj_t * list, lv_obj_t * row,
                                                lv_obj_t * leading_image, int logical_index,
                                                int pool_slot, int64_t identity, void * ctx);
void compact_list_set_row_decorator(lv_obj_t * list, compact_list_row_decorator_cb_t cb, void * ctx);
void compact_list_refresh_visible(lv_obj_t * list);
void compact_list_refresh_all(void);
/* Re-runs label/accessory decoration for one currently visible logical row.
 * A row that has already scrolled out is intentionally ignored. */
void compact_list_refresh_item(lv_obj_t * list, int logical_index);

/* Changes only this compact list's row height. Useful for denser/roomier
 * feature areas without changing Settings and every other shared row. */
void compact_list_set_row_height(lv_obj_t * list, int32_t row_height);

/* Switches list to (or refreshes) paged mode: total_count is the logical
 * row count (drives the scrollbar range and every fetch_page() bound
 * above) -- call again (passing the same fetch_page/ctx) after the
 * underlying data changes (e.g. a library rescan, or a live search's
 * filter changing) to refresh the count and invalidate the row cache.
 * Pass fetch_page == NULL to switch back to eager mode -- the list then
 * shows whatever compact_list_set_items() last gave it (empty if that was
 * never called); total_count/ctx are ignored in that case. */
void compact_list_set_paged_provider(lv_obj_t * list, compact_list_fetch_page_cb_t fetch_page, void * ctx,
                                      int total_count);

#endif /* SCREEN_BUILDERS_H */

lv_obj_t * add_pill_row_base(lv_obj_t * parent, const char * label_text);
lv_obj_t * add_pill_toggle_row(lv_obj_t * parent, const char * label_text, bool checked, lv_event_cb_t on_click);
lv_obj_t * add_pill_chevron_row(lv_obj_t * parent, const char * label_text, lv_event_cb_t on_click);
lv_obj_t * add_section_header(lv_obj_t * parent, const char * text);
