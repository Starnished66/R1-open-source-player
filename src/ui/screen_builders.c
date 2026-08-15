#include "screen_builders.h"
#include "assets.h"

#include <stdlib.h>
#include <string.h>

lv_style_t list_row_style;
lv_style_t list_row_pressed_style;

/* Theming: three shared, mutable-in-place bg_color styles, one per
 * "background category" the app has -- every screen root
 * (style_theme_screen_bg), every popup/EQ-card/slider-card
 * (style_theme_card_bg), and every list row (list_row_style itself,
 * reused directly -- no separate style needed, its bg_color just needs to
 * become live-mutable too). Declared here (not gui.c) since screen_builders.c
 * already owns list_row_style, the one existing precedent for a style
 * shared across both files; gui.c's plugin_manager bridge
 * (gui_plugin_set_background_color()) mutates whichever of these three a
 * plugin's plugin.set_background_color() call names, then
 * lv_obj_report_style_change()s it -- the exact same "shared style updated
 * in place + report_style_change" mechanism apply_accent_color() already
 * uses successfully for the app's existing accent color feature. Not
 * style_accent itself: that style already claims bg_color for its own
 * purpose (slider/switch fill), so reusing it here would wrongly tie
 * every screen's background to whatever accent color is picked. */
lv_style_t style_theme_screen_bg;
lv_style_t style_theme_card_bg;

void screen_builders_init_list_row_style(void) {
    lv_style_init(&list_row_style);
    lv_style_set_width(&list_row_style, LIST_ROW_WIDTH);
    lv_style_set_height(&list_row_style, LIST_ROW_HEIGHT);
    lv_style_set_radius(&list_row_style, LIST_ROW_RADIUS);
    lv_style_set_bg_color(&list_row_style, LIST_ROW_BG_COLOR);
    lv_style_set_bg_opa(&list_row_style, LV_OPA_COVER);
    lv_style_set_border_width(&list_row_style, 0);
    lv_style_set_pad_left(&list_row_style, LIST_ROW_LABEL_INSET);
    lv_style_set_pad_top(&list_row_style, (LIST_ROW_HEIGHT - lv_font_get_line_height(&LIST_ROW_FONT)) / 2);
    lv_style_set_text_color(&list_row_style, lv_color_make(230, 230, 230));
    lv_style_set_text_font(&list_row_style, &LIST_ROW_FONT);

    /* Real-device bug report: no visual feedback at all on touching a song
     * row (list_row_style has no LV_STATE_PRESSED override, so the row's
     * bg_color never changed) -- a plain tap had nothing confirming it
     * registered, and a long-press-in-progress looked identical to not
     * touching the screen at all until the context menu suddenly appeared.
     * Added as its own style (not folded into list_row_style itself) so it
     * can be attached with the LV_STATE_PRESSED selector specifically --
     * see build_compact_list_widget()/populate_group_songs_rows()'s own
     * lv_obj_add_style() calls. Noticeably lighter than LIST_ROW_BG_COLOR
     * (28,28,30) rather than a subtle tint, so it reads clearly even in a
     * quick tap, not just a held press. */
    lv_style_init(&list_row_pressed_style);
    lv_style_set_bg_color(&list_row_pressed_style, lv_color_make(60, 60, 64));

    /* Default values match this app's own existing look before theming
     * existed: true black for screens (every icon-grid/pill-list tile has
     * a transparent background, bg_opa=0, so this color always shows
     * through around/between them -- a near-black (18,18,22) created a
     * visible seam against real-device testing showing the icon assets'
     * own baked-in black canvas, hence pure black), and the popup/EQ-card
     * gray every one of those already converged on by real-device
     * feedback (see the EQ screen's own former EQ_CARD_COLOR comment
     * history in gui.c) for cards. gui.c keeps its own separate
     * SCREEN_BG_COLOR copy for gui_show_boot_splash(), which runs before
     * this function (and style_theme_screen_bg) ever does -- see that
     * function's own comment. */
    lv_style_init(&style_theme_screen_bg);
    lv_style_set_bg_color(&style_theme_screen_bg, lv_color_make(0, 0, 0));
    lv_style_init(&style_theme_card_bg);
    lv_style_set_bg_color(&style_theme_card_bg, lv_color_make(32, 32, 32));
}

/* Generous upper bound on rows for a 2-column icon grid -- every real
 * screen using build_icon_grid_screen tops out at 6 items (3 rows). */
#define ICON_GRID_MAX_ROWS 6

/* Every full icon-grid screen today (Home, Music, Wireless) has exactly 3
 * rows, and that's the row size every tile has been visually tuned against.
 * Rows are sized against this reference instead of "however many rows this
 * particular screen has" so a screen with fewer items (Stream Media: 4
 * items, 2 rows) gets normal-sized tiles with genuine leftover space below,
 * rather than 2 rows stretched to fill the same height 3 would -- confirmed
 * on real hardware as the cause of Stream Media reading as mostly empty. */
#define ICON_GRID_REFERENCE_ROWS 3

/* On-screen icon footprint (longest edge, in px) every icon-grid tile
 * targets, regardless of its source PNG's native resolution -- icons in the
 * "category" folder (Music, Home) are 100x100 and were the ones the old
 * flat lv_image_set_scale(img, 340) constant was tuned against
 * (100*340/256 = ~133px), but the "stream_media" icons and
 * wireless/dlna.png are natively 212x190, over 2x bigger. Applying that
 * same flat scale to those rendered them at ~280x252px -- nearly a whole
 * tile's height on its own -- which is what was pushing Stream Media's
 * row-2 labels out of their tiles entirely (confirmed on real hardware).
 * Scaling per-tile against each source image's actual size keeps every
 * icon the same size on screen instead. */
#define ICON_GRID_TARGET_ICON_PX 133

/* Tile inner padding and icon-to-label gap, both in px -- pulled out as
 * named constants since the manual absolute-positioning math below (see
 * its own comment) needs the exact same numbers used by the tile's own
 * pad_all style. */
#define ICON_GRID_TILE_PAD 8
#define ICON_GRID_ICON_LABEL_GAP 4

/* STATUS_BAR_CLEARANCE / TITLE_ROW_HEIGHT now live in screen_builders.h --
 * gui.c's hand-built screens (player, accent color, EQ, ...) need them too. */

static lv_obj_t * build_back_button(lv_obj_t * scr, lv_event_cb_t back_btn_cb) {
    /* Hitbox is deliberately larger than the visual icon -- real-hardware
     * testing showed taps aimed at this corner landing a few pixels outside
     * a tight 44x44 box, so the touch area is padded out generously while
     * the icon itself stays centered at its normal size. */
    lv_obj_t * btn = lv_obj_create(scr);
    lv_obj_set_size(btn, 64, 64);
    lv_obj_align(btn, LV_ALIGN_TOP_LEFT, 0, STATUS_BAR_CLEARANCE);
    lv_obj_set_style_bg_opa(btn, 0, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t * arrow = lv_image_create(btn);
    lv_image_set_src(arrow, asset_path("sub_back/btn_back.png"));
    lv_obj_center(arrow);

    if (back_btn_cb) lv_obj_add_event_cb(btn, back_btn_cb, LV_EVENT_CLICKED, NULL);
    return btn;
}

static lv_obj_t * build_title(lv_obj_t * scr, const char * title) {
    lv_obj_t * label = lv_label_create(scr);
    lv_label_set_text(label, title);
    /* Vertically centered within the TITLE_ROW_HEIGHT band below the
     * status bar (matches the back button's own 64px height). */
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, STATUS_BAR_CLEARANCE + (TITLE_ROW_HEIGHT - 28) / 2);
    lv_obj_set_style_text_color(label, lv_color_make(240, 240, 240), 0);
    lv_obj_set_style_text_font(label, ui_size_28, 0);
    return label;
}

/* ---- Icon grid ---- */

typedef struct {
    lv_obj_t * img;
    const char * normal_path;
    const char * selected_path;
} icon_tile_press_ctx_t;

static void icon_tile_press_event_cb(lv_event_t * e) {
    icon_tile_press_ctx_t * ctx = (icon_tile_press_ctx_t *) lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        lv_image_set_src(ctx->img, asset_path(ctx->selected_path));
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        lv_image_set_src(ctx->img, asset_path(ctx->normal_path));
    }
}

lv_obj_t * build_icon_grid_screen(const char * title, lv_event_cb_t back_btn_cb,
                                   const icon_grid_item_t * items, int item_count,
                                   int32_t icon_scale_percent) {
    int32_t target_icon_px = (ICON_GRID_TARGET_ICON_PX * icon_scale_percent) / 100;

    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &style_theme_screen_bg, 0);

    /* NULL back_btn_cb means "this screen has nothing to go back to" (the
     * Home launcher) -- skip the arrow entirely rather than drawing a dead
     * button. NULL title similarly means "no header" -- the real stock
     * launcher has no title text at all above its icon grid (confirmed
     * against a real-device screenshot), just a small gap below the status
     * bar before the grid starts, unlike every other icon-grid screen
     * (Stream Media, Wireless, ...) which does have one. */
    if (back_btn_cb) build_back_button(scr, back_btn_cb);
    if (title) build_title(scr, title);

    /* A true 2-column grid with equal-sized cells and thin divider lines
     * between them, matching a real reference screenshot of the stock
     * launcher -- the earlier flex-wrap layout packed tiles at their native
     * content size with even spacing around them, which reads noticeably
     * smaller/looser than the stock grid's large, evenly-divided cells. */
    int col_count = 2;
    int row_count = (item_count + col_count - 1) / col_count;

    /* Heap-allocated (never freed) rather than static -- lv_obj_set_grid_dsc_array()
     * below stores the pointer, not a copy, and this function is called once
     * per icon-grid screen (Music, Stream Media, Wireless, Home, ...). A
     * static array here would mean every later call overwrites the same
     * memory a still-alive earlier grid's dsc pointer refers to -- harmless
     * back when every row was LV_GRID_FR(1) (any screen's overwrite was a
     * no-op), but once rows became per-screen fixed pixel heights (see
     * ICON_GRID_REFERENCE_ROWS above) this silently corrupted every
     * previously-built grid's row heights to whichever screen was built
     * last (Home), confirmed on real hardware and via lv_refr_now()
     * before/after checks: Music's own row height read back correctly
     * right after being built, then changed to Home's the moment a real
     * redraw ran after all screens existed. Same pattern applies to every
     * other per-widget context struct in this codebase that's deliberately
     * never freed (screens live for the process's lifetime). */
    int32_t * col_dsc = lv_malloc(3 * sizeof(int32_t));
    col_dsc[0] = LV_GRID_FR(1);
    col_dsc[1] = LV_GRID_FR(1);
    col_dsc[2] = LV_GRID_TEMPLATE_LAST;

    /* Grid height is the screen height minus whichever header bands this
     * particular screen actually has (status bar always; title row only
     * when a title/back button was requested) -- not a flat percentage.
     * Music/Stream Media/Wireless used to get lv_pct(78) here while Home
     * got lv_pct(92) purely because 78/92 had been tuned by eye rather
     * than computed from the header's real pixel height, so titled
     * screens' cells came out visibly smaller than Home's despite the
     * same 2-column layout -- and later, once the status bar itself grew,
     * their title text started overlapping it outright. This keeps every
     * icon-grid screen's cells the same size AND guarantees the header
     * never overlaps the grid, regardless of which bands are present. */
    int32_t scr_h = lv_display_get_vertical_resolution(lv_display_get_default());
    int32_t header_h = STATUS_BAR_CLEARANCE + (title ? TITLE_ROW_HEIGHT : 0);

    /* Rows are a fixed pixel height (see ICON_GRID_REFERENCE_ROWS above)
     * rather than LV_GRID_FR(1) shares of the full available height -- FR
     * shares divide *whatever's available* evenly among however many rows
     * exist, so a 2-row screen got the same total height as a 3-row one,
     * just stretched across fewer, oversized cells. */
    int32_t row_h = (scr_h - header_h) / ICON_GRID_REFERENCE_ROWS;

    int32_t * row_dsc = lv_malloc((ICON_GRID_MAX_ROWS + 1) * sizeof(int32_t));
    for (int r = 0; r < row_count && r < ICON_GRID_MAX_ROWS; r++) row_dsc[r] = row_h;
    row_dsc[row_count < ICON_GRID_MAX_ROWS ? row_count : ICON_GRID_MAX_ROWS] = LV_GRID_TEMPLATE_LAST;

    lv_obj_t * grid = lv_obj_create(scr);
    lv_obj_set_size(grid, lv_pct(100), row_count * row_h);
    lv_obj_align(grid, LV_ALIGN_TOP_MID, 0, header_h);
    lv_obj_set_style_bg_opa(grid, 0, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_style_pad_all(grid, 0, 0);
    /* lv_obj_create()'s default style has a nonzero row/column gap, which
     * silently ate into row_h above (grid height was sized as an exact
     * row_count*row_h multiple, with no room left for that gap) -- harmless
     * under the old LV_GRID_FR(1) rows since those always divided whatever
     * height the grid was given, gap included, but with fixed-pixel rows it
     * pushed the last row past the grid's own bottom edge and clipped it
     * (confirmed on real hardware: Stream Media/Wireless's last row lost its
     * labels). The divider border lines are the only separation these tiles
     * were ever meant to have. */
    lv_obj_set_style_pad_row(grid, 0, 0);
    lv_obj_set_style_pad_column(grid, 0, 0);
    /* Vertical-only -- real-hardware testing showed a horizontal swipe
     * (meant as the app-wide back gesture) getting captured as a scroll
     * attempt instead of escalating to LV_EVENT_GESTURE, since a plain
     * lv_obj_create() defaults to scrollable in every direction. */
    lv_obj_set_scroll_dir(grid, LV_DIR_VER);
    lv_obj_set_grid_dsc_array(grid, col_dsc, row_dsc);
    lv_obj_set_layout(grid, LV_LAYOUT_GRID);

    for (int i = 0; i < item_count; i++) {
        const icon_grid_item_t * item = &items[i];
        int col = i % col_count;
        int row = i / col_count;

        lv_obj_t * tile = lv_obj_create(grid);
        lv_obj_set_grid_cell(tile, LV_GRID_ALIGN_STRETCH, col, 1, LV_GRID_ALIGN_STRETCH, row, 1);
        lv_obj_set_style_bg_opa(tile, 0, 0);
        /* The default theme's "card" style puts a ~1px border on every
         * plain lv_obj_create() -- harmless back when per-tile borders were
         * hand-drawn here (every tile set its own border_width explicitly,
         * see the real divider lines added below at the grid level), but
         * left un-zeroed now it shows as a light border around all four
         * edges of every tile, confirmed on a real-device screenshot. */
        lv_obj_set_style_border_width(tile, 0, 0);
        lv_obj_set_style_pad_all(tile, ICON_GRID_TILE_PAD, 0);
        lv_obj_remove_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);

        /* Icon and label are positioned with explicit pixel offsets rather
         * than flex auto-centering -- flex's cross-axis centering here
         * turned out to recompute differently between a plain layout pass
         * and the real draw/refresh pass (reproduced consistently with
         * lv_refr_now() before/after checks: the *reported* coordinates
         * right after layout didn't match what actually got painted, off
         * by dozens of pixels, only for the grid's last row), so the whole
         * tile's content is placed by hand here instead of trusting flex
         * to settle on a stable answer. */
        lv_obj_t * img_wrap = lv_obj_create(tile);
        lv_obj_remove_style_all(img_wrap);
        lv_obj_remove_flag(img_wrap, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(img_wrap, LV_OBJ_FLAG_CLICKABLE); /* let taps fall through to tile's own click handler */

        lv_obj_t * img = lv_image_create(img_wrap);
        const char * icon_full_path = asset_path(item->icon_asset);
        lv_image_set_src(img, icon_full_path);

        lv_image_header_t icon_header;
        int32_t icon_scale = (340 * icon_scale_percent) / 100; /* fallback: matches the historical flat scale (at 100%) if the header can't be read */
        int32_t icon_w = 100, icon_h = 100; /* fallback: matches the "category" folder icons' native size */
        if (lv_image_decoder_get_info(icon_full_path, &icon_header) == LV_RESULT_OK) {
            int32_t native_max = icon_header.w > icon_header.h ? icon_header.w : icon_header.h;
            if (native_max > 0) icon_scale = (int32_t) (((int64_t) target_icon_px * 256) / native_max);
            icon_w = icon_header.w;
            icon_h = icon_header.h;
        }
        lv_image_set_scale(img, icon_scale);
        int32_t target_icon_w = (int32_t) (((int64_t) icon_w * icon_scale) / 256);
        int32_t target_icon_h = (int32_t) (((int64_t) icon_h * icon_scale) / 256);
        lv_obj_set_size(img_wrap, target_icon_w, target_icon_h);
        lv_obj_center(img);

        lv_obj_t * label = lv_label_create(tile);
        lv_label_set_text(label, item->label);
        lv_obj_set_style_text_color(label, lv_color_make(220, 220, 220), 0);
        /* Bumped from montserrat_16 -- real-device feedback: the main
         * menu/submenu text still read too small even after the app-wide
         * LV_FONT_DEFAULT bump, since these tiles always set an explicit
         * font and so never fell back to that default. montserrat_20
         * matches the top bar's own text size rather than inventing a new
         * in-between size. */
        lv_obj_set_style_text_font(label, ui_size_20, 0);

        int32_t label_h = lv_font_get_line_height(ui_size_20);
        int32_t content_h = target_icon_h + ICON_GRID_ICON_LABEL_GAP + label_h;
        int32_t available_h = row_h - 2 * ICON_GRID_TILE_PAD;
        int32_t top_offset = (available_h - content_h) / 2;
        if (top_offset < 0) top_offset = 0;
        lv_obj_align(img_wrap, LV_ALIGN_TOP_MID, 0, top_offset);
        lv_obj_align(label, LV_ALIGN_TOP_MID, 0, top_offset + target_icon_h + ICON_GRID_ICON_LABEL_GAP);

        if (item->icon_asset_selected) {
            icon_tile_press_ctx_t * ctx = malloc(sizeof(icon_tile_press_ctx_t));
            ctx->img = img;
            ctx->normal_path = item->icon_asset;
            ctx->selected_path = item->icon_asset_selected;
            lv_obj_add_event_cb(tile, icon_tile_press_event_cb, LV_EVENT_PRESSED, ctx);
            lv_obj_add_event_cb(tile, icon_tile_press_event_cb, LV_EVENT_RELEASED, ctx);
            lv_obj_add_event_cb(tile, icon_tile_press_event_cb, LV_EVENT_PRESS_LOST, ctx);
        }

        if (item->on_click) lv_obj_add_event_cb(tile, item->on_click, LV_EVENT_CLICKED, item->user_data);
    }

    /* Divider lines between cells, drawn from the stock firmware's own
     * launcher/hor_line.png and launcher/ver_line.png sprites (thin, flat
     * near-black bars, confirmed via a real stock-player screenshot to be
     * exactly what separates its launcher grid cells) instead of hand-drawn
     * LVGL borders. Drawn once at the grid level rather than per-tile,
     * since one continuous line crosses every column/row boundary --
     * ver_line.png's own native height (749px) matches a full 3-row
     * reference grid, and bg_image_tiled means a shorter grid (fewer rows)
     * still tiles the source's flat color cleanly rather than clipping or
     * stretching it. LV_OBJ_FLAG_IGNORE_LAYOUT keeps the grid's own GRID
     * layout from trying to auto-place these into an unassigned cell, since
     * they're not registered via lv_obj_set_grid_cell(). Same bg_opa-vs-
     * bg_image_opa split used for the volume popup: bg_opa stays
     * transparent so no rect fill draws, while bg_image_opa defaults to
     * COVER independently, so only the line sprite itself shows. */
    if (col_count > 1) {
        lv_obj_t * vline = lv_obj_create(grid);
        lv_obj_remove_style_all(vline);
        lv_obj_add_flag(vline, LV_OBJ_FLAG_IGNORE_LAYOUT);
        lv_obj_remove_flag(vline, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(vline, 1, row_count * row_h);
        lv_obj_align(vline, LV_ALIGN_TOP_MID, 0, 0);
        lv_obj_set_style_bg_opa(vline, LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_image_src(vline, asset_path("launcher/ver_line.png"), 0);
        lv_obj_set_style_bg_image_tiled(vline, true, 0);
    }

    for (int r = 0; r < row_count - 1; r++) {
        lv_obj_t * hline = lv_obj_create(grid);
        lv_obj_remove_style_all(hline);
        lv_obj_add_flag(hline, LV_OBJ_FLAG_IGNORE_LAYOUT);
        lv_obj_remove_flag(hline, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(hline, lv_pct(100), 1);
        lv_obj_align(hline, LV_ALIGN_TOP_LEFT, 0, (r + 1) * row_h);
        lv_obj_set_style_bg_opa(hline, LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_image_src(hline, asset_path("launcher/hor_line.png"), 0);
        lv_obj_set_style_bg_image_tiled(hline, true, 0);
    }

    return scr;
}

/* ---- Pill list ---- */

typedef struct {
    lv_obj_t * toggle_img;
} pill_toggle_ctx_t;

static void pill_toggle_row_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    pill_toggle_ctx_t * ctx = (pill_toggle_ctx_t *) lv_event_get_user_data(e);
    lv_obj_t * img = ctx->toggle_img;

    bool now_checked = !lv_obj_has_state(img, LV_STATE_CHECKED);
    if (now_checked) lv_obj_add_state(img, LV_STATE_CHECKED);
    else lv_obj_clear_state(img, LV_STATE_CHECKED);
    lv_image_set_src(img, asset_path(now_checked ? "settings/on.png" : "settings/off.png"));

    /* Real toggle rows only visually differ by which sprite is shown -- the
     * VALUE_CHANGED event lets callers read state the same way they already
     * do for lv_switch (lv_obj_has_state(target, LV_STATE_CHECKED)). */
    lv_obj_send_event(img, LV_EVENT_VALUE_CHANGED, NULL);
}

lv_obj_t * build_pill_list_screen(const char * title, lv_event_cb_t back_btn_cb,
                                   const pill_list_item_t * items, int item_count,
                                   lv_style_t * toggle_accent_style) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &style_theme_screen_bg, 0);

    /* NULL back_btn_cb means "this screen has nothing to go back to" (the
     * Home launcher) -- skip the arrow entirely rather than drawing a dead
     * button. */
    if (back_btn_cb) build_back_button(scr, back_btn_cb);
    build_title(scr, title);

    lv_obj_t * list = lv_obj_create(scr);
    lv_obj_set_size(list, lv_pct(100),
                    lv_display_get_vertical_resolution(lv_display_get_default()) - STATUS_BAR_CLEARANCE -
                        TITLE_ROW_HEIGHT);
    lv_obj_align(list, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(list, 0, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    /* Vertical-only -- see the matching comment in build_icon_grid_screen. */
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(list, 6, 0);
    lv_obj_set_style_pad_top(list, 4, 0);

    for (int i = 0; i < item_count; i++) {
        const pill_list_item_t * item = &items[i];

        lv_obj_t * row = lv_obj_create(list);
        lv_obj_set_size(row, 448, 124);
        /* item_bg.png is a rounded-rect sprite with transparent corners --
         * without an explicit black bg_color here, LVGL's own default
         * object background (light gray/white) shows through at those
         * corners, since bg_opa=COVER is needed for the image itself to
         * draw at all and also fills the object's full square bounding
         * box underneath it. */
        lv_obj_add_style(row, &style_theme_screen_bg, 0);
        lv_obj_set_style_bg_image_src(row, asset_path("touch_list/item_bg.png"), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t * label = lv_label_create(row);
        lv_label_set_text(label, item->label);
        lv_obj_set_style_text_color(label, lv_color_make(230, 230, 230), 0);
        /* Bumped from montserrat_16 -- see the matching comment on the icon
         * grid's own label above; the row is a fixed 124px tall so this
         * doesn't need any layout math adjustment like the grid did. */
        lv_obj_set_style_text_font(label, ui_size_20, 0);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 24, 0);

        if (item->accessory == PILL_ACCESSORY_TOGGLE) {
            lv_obj_t * toggle_img = lv_image_create(row);
            lv_image_set_src(toggle_img, asset_path(item->toggle_initial_state ? "settings/on.png" : "settings/off.png"));
            lv_obj_align(toggle_img, LV_ALIGN_RIGHT_MID, -20, 0);
            if (item->toggle_initial_state) lv_obj_add_state(toggle_img, LV_STATE_CHECKED);
            /* LV_STATE_CHECKED selector, not unconditional -- otherwise the
             * recolor hits both the ON and OFF sprite alike, making the
             * toggle unreadable in either state (real-device bug report).
             * pill_toggle_row_event_cb() above already keeps this state in
             * sync with which sprite is showing on every tap. */
            if (toggle_accent_style) lv_obj_add_style(toggle_img, toggle_accent_style, LV_STATE_CHECKED);

            if (item->out_toggle_img) *item->out_toggle_img = toggle_img;

            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            pill_toggle_ctx_t * ctx = malloc(sizeof(pill_toggle_ctx_t));
            ctx->toggle_img = toggle_img;
            lv_obj_add_event_cb(row, pill_toggle_row_event_cb, LV_EVENT_CLICKED, ctx);

            if (item->on_toggle_change) {
                lv_obj_add_event_cb(toggle_img, item->on_toggle_change, LV_EVENT_VALUE_CHANGED, item->user_data);
            }
        } else {
            if (item->accessory == PILL_ACCESSORY_CHEVRON) {
                /* No matching real chevron asset found in theme2 at this
                 * screen scale -- plain text is the honest fallback rather
                 * than forcing a mismatched sprite. */
                lv_obj_t * chevron = lv_label_create(row);
                lv_label_set_text(chevron, ">");
                lv_obj_set_style_text_color(chevron, lv_color_make(140, 140, 140), 0);
                lv_obj_set_style_text_font(chevron, ui_size_20, 0);
                lv_obj_align(chevron, LV_ALIGN_RIGHT_MID, -20, 0);
            }

            if (item->on_click) {
                lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_add_event_cb(row, item->on_click, LV_EVENT_CLICKED, item->user_data);
            }
        }
    }

    return scr;
}

/* ---- Compact list (virtualized) ---- */

/* Matches the old flex layout's own spacing (pad_top=4, pad_gap=4) --
 * explicit here instead since a virtualized list positions every row by
 * hand (lv_obj_set_pos), not through LVGL's flex engine. */
#define COMPACT_LIST_ROW_STRIDE (LIST_ROW_HEIGHT + 4)
#define COMPACT_LIST_TOP_PAD 4

/* Real row objects that exist at once, reused (repositioned + relabeled)
 * as the list scrolls, regardless of how many items the list actually
 * has -- generous enough to cover the visible viewport (~8 rows at this
 * screen height) plus overscan on both sides so a fast flick doesn't
 * visibly pop rows in/out, without ever approaching the thousands-of-
 * objects cost this whole scheme exists to avoid. */
#define COMPACT_LIST_POOL_SIZE 20

typedef struct {
    compact_list_item_t * items; /* owned copy -- see build_compact_list_screen()'s own doc comment */
    int item_count;
    compact_list_click_cb_t on_click;
    compact_list_click_cb_t on_long_press; /* NULL for a list with no long-press action (e.g. Artists/Albums name rows) */
    /* Real-device incident: LVGL still sends LV_EVENT_CLICKED on release
     * even when LV_EVENT_LONG_PRESSED already fired earlier in that same
     * press (lv_indev.c's indev_proc_release() doesn't check) -- same root
     * cause as quick_drawer_wifi_long_press_cb's own doc comment in gui.c,
     * fixed the same way: the long-press handler sets this, the click
     * handler checks-and-clears it first and skips entirely if set. One
     * flag for the whole list, not per-row -- this is a single-touch
     * device, only one row can plausibly be mid-press at a time. */
    bool long_press_fired;
    int window_start; /* index currently shown by rows[0]; -1 forces the first update to actually run */
    lv_obj_t * rows[COMPACT_LIST_POOL_SIZE];
    void * row_ctx[COMPACT_LIST_POOL_SIZE]; /* opaque to this struct, freed alongside it -- see compact_list_row_ctx_t below */
    lv_obj_t * spacer; /* repositioned by compact_list_set_items() when item_count changes */
    lv_obj_t * now_playing_bar; /* NULL if this list wasn't built with enable_now_playing -- see compact_list_set_now_playing() */
    int now_playing_index; /* -1 = nothing playing/matching in this list */
} compact_list_virtual_data_t;

/* Bar width only -- height always matches LIST_ROW_HEIGHT (see
 * compact_list_set_now_playing()). Thin enough to read as an accent stripe,
 * not a second column competing with the row's own content. */
#define COMPACT_LIST_NOW_PLAYING_BAR_WIDTH 5

typedef struct {
    compact_list_virtual_data_t * data;
    int pool_slot;
} compact_list_row_ctx_t;

static void compact_list_row_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    compact_list_row_ctx_t * ctx = (compact_list_row_ctx_t *) lv_event_get_user_data(e);
    if (ctx->data->long_press_fired) { /* see compact_list_row_long_press_cb()'s own comment */
        ctx->data->long_press_fired = false;
        return;
    }
    int index = ctx->data->window_start + ctx->pool_slot;
    if (index < 0 || index >= ctx->data->item_count) return; /* this pool slot is currently a hidden, contentless row (list shorter than the pool) -- shouldn't be reachable, guarded anyway */
    ctx->data->on_click(index);
}

/* Same index resolution as compact_list_row_click_cb() above, for the
 * optional long-press action (song context menu -- Add to Queue/Add to
 * Playlist) -- only ever attached (see the row-pool setup below) when the
 * caller actually passed an on_long_press, so this is never invoked for a
 * list that doesn't have one. */
static void compact_list_row_long_press_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_LONG_PRESSED) return;
    compact_list_row_ctx_t * ctx = (compact_list_row_ctx_t *) lv_event_get_user_data(e);
    ctx->data->long_press_fired = true;
    int index = ctx->data->window_start + ctx->pool_slot;
    if (index < 0 || index >= ctx->data->item_count) return;
    ctx->data->on_long_press(index);
}

/* Repositions/relabels the row pool so it covers the range of items
 * actually scrolled into (or near) view -- called once up front to
 * populate the initial view, then again on every LV_EVENT_SCROLL. Cheap
 * even though it touches every pool row on each call: COMPACT_LIST_POOL_SIZE
 * label-text-and-position updates is nothing like the cost of the
 * thousands of real LVGL objects this replaces, and the early return when
 * the window hasn't actually moved (the overwhelmingly common case between
 * scroll events on this device's touch sampling rate) skips even that. */
static void compact_list_update_window(lv_obj_t * list, compact_list_virtual_data_t * data) {
    int32_t scroll_y = lv_obj_get_scroll_y(list);
    if (scroll_y < 0) scroll_y = 0;
    int first = (int) (scroll_y / COMPACT_LIST_ROW_STRIDE) - COMPACT_LIST_POOL_SIZE / 4;
    if (first < 0) first = 0;
    int max_first = data->item_count - COMPACT_LIST_POOL_SIZE;
    if (max_first < 0) max_first = 0;
    if (first > max_first) first = max_first;

    if (first == data->window_start) return;
    data->window_start = first;

    for (int slot = 0; slot < COMPACT_LIST_POOL_SIZE; slot++) {
        int index = first + slot;
        lv_obj_t * row = data->rows[slot];
        if (index >= data->item_count) {
            lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(row, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_y(row, COMPACT_LIST_TOP_PAD + index * COMPACT_LIST_ROW_STRIDE);
        lv_label_set_text(row, data->items[index].label);
    }
}

static void compact_list_scroll_event_cb(lv_event_t * e) {
    lv_obj_t * list = lv_event_get_target(e);
    compact_list_update_window(list, (compact_list_virtual_data_t *) lv_obj_get_user_data(list));
}

static void compact_list_delete_event_cb(lv_event_t * e) {
    compact_list_virtual_data_t * data = (compact_list_virtual_data_t *) lv_obj_get_user_data(lv_event_get_target(e));
    for (int slot = 0; slot < COMPACT_LIST_POOL_SIZE; slot++) free(data->row_ctx[slot]);
    free(data->items);
    free(data);
}

void compact_list_scroll_to_index(lv_obj_t * list, int index) {
    lv_obj_scroll_to_y(list, COMPACT_LIST_TOP_PAD + index * COMPACT_LIST_ROW_STRIDE, LV_ANIM_OFF);
}

void compact_list_set_now_playing(lv_obj_t * list, int item_index) {
    compact_list_virtual_data_t * data = (compact_list_virtual_data_t *) lv_obj_get_user_data(list);
    if (!data->now_playing_bar) return; /* this list wasn't built with enable_now_playing */

    data->now_playing_index = item_index;
    if (item_index < 0 || item_index >= data->item_count) {
        lv_obj_add_flag(data->now_playing_bar, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(data->now_playing_bar, LV_OBJ_FLAG_HIDDEN);
    /* y matches a row's own position exactly (see the row pool's own
     * lv_obj_set_y() in compact_list_update_window()) -- x is fixed at 0
     * (this list's own left edge, i.e. the screen's far left, see this
     * function's own doc comment in screen_builders.h), set once at
     * creation and never touched again since it doesn't depend on scroll
     * position. LVGL's own scroll clipping hides it exactly like any other
     * child once it scrolls out of the visible viewport -- no need to track
     * the pool's current window here at all. */
    lv_obj_set_y(data->now_playing_bar, COMPACT_LIST_TOP_PAD + item_index * COMPACT_LIST_ROW_STRIDE);
}

/* Swaps a build_compact_list_screen() list's contents in place -- e.g. a
 * live search filter -- without the lv_obj_delete()+rebuild this project
 * otherwise always used for that (see poll_library_rescan()'s own
 * rebuild-from-scratch precedent in gui.c). Copies `items` the same way
 * build_compact_list_screen() itself does (caller doesn't need to keep the
 * array or its strings alive afterward, though the strings themselves
 * still need to outlive this call the same way the original construction-
 * time ones do -- see build_compact_list_screen()'s own doc comment). */
void compact_list_set_items(lv_obj_t * list, const compact_list_item_t * items, int item_count) {
    compact_list_virtual_data_t * data = (compact_list_virtual_data_t *) lv_obj_get_user_data(list);

    free(data->items);
    data->items = NULL;
    if (item_count > 0) {
        data->items = malloc(sizeof(compact_list_item_t) * (size_t) item_count);
        memcpy(data->items, items, sizeof(compact_list_item_t) * (size_t) item_count);
    }
    data->item_count = item_count;
    data->window_start = -1; /* force compact_list_update_window() below to actually repaint */

    int32_t total_height = COMPACT_LIST_TOP_PAD + item_count * COMPACT_LIST_ROW_STRIDE;
    lv_obj_set_pos(data->spacer, 0, total_height > 0 ? total_height - 1 : 0);

    /* A shorter result set than the previous scroll position would
     * otherwise leave the view showing blank space past the new (shorter)
     * scrollable range. */
    lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF);
    compact_list_update_window(list, data);
}

/* The virtualized list widget itself, with no screen/back-button/title
 * wrapper -- factored out of build_compact_list_screen() (which now just
 * calls this against a fresh screen) so a caller that already has its own
 * screen can attach a compact list directly onto it, e.g. gui.c's Files
 * search results overlay, layered on top of build_files_screen()'s own
 * folder-browsing UI rather than needing a whole separate screen. */
lv_obj_t * build_compact_list_widget(lv_obj_t * parent, const compact_list_item_t * items, int item_count,
                                      compact_list_click_cb_t on_click, compact_list_click_cb_t on_long_press,
                                      int32_t row_width, bool enable_now_playing, lv_color_t now_playing_color) {
    lv_obj_t * list = lv_obj_create(parent);
    lv_obj_set_size(list, lv_pct(100),
                    lv_display_get_vertical_resolution(lv_display_get_default()) - STATUS_BAR_CLEARANCE -
                        TITLE_ROW_HEIGHT);
    lv_obj_align(list, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(list, 0, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    /* Vertical-only -- see the matching comment in build_icon_grid_screen.
     * No flex flow here (unlike the old implementation) -- every row is
     * positioned by hand as part of the virtualization scheme below. */
    lv_obj_set_scroll_dir(list, LV_DIR_VER);

    compact_list_virtual_data_t * data = malloc(sizeof(*data));
    data->item_count = item_count;
    data->on_click = on_click;
    data->on_long_press = on_long_press;
    data->long_press_fired = false;
    data->window_start = -1;
    data->items = NULL;
    if (item_count > 0) {
        data->items = malloc(sizeof(compact_list_item_t) * (size_t) item_count);
        memcpy(data->items, items, sizeof(compact_list_item_t) * (size_t) item_count);
    }

    /* Rows are pinned at x=0 by default (LIST_ROW_WIDTH already leaves an
     * unused right-side margin at this screen's width) -- only centered
     * when a caller overrides row_width, so a widened row can't overhang
     * past the list's own right edge instead of just eating into that
     * margin. Left at 0 for the default width so every existing caller's
     * layout (e.g. the timezone city list) is untouched. */
    int32_t row_x = 0;
    if (row_width != LIST_ROW_WIDTH) {
        row_x = (lv_display_get_horizontal_resolution(lv_display_get_default()) - row_width) / 2;
        if (row_x < 0) row_x = 0;
    }

    for (int slot = 0; slot < COMPACT_LIST_POOL_SIZE; slot++) {
        lv_obj_t * row = lv_label_create(list);
        lv_obj_add_style(row, &list_row_style, 0);
        lv_obj_add_style(row, &list_row_pressed_style, LV_STATE_PRESSED);
        if (row_width != LIST_ROW_WIDTH) lv_obj_set_style_width(row, row_width, 0); /* local override -- see this param's own doc comment (screen_builders.h) */
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(row, row_x, COMPACT_LIST_TOP_PAD + slot * COMPACT_LIST_ROW_STRIDE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN); /* shown by the initial window update below once it has real content */
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        data->rows[slot] = row;

        compact_list_row_ctx_t * ctx = malloc(sizeof(*ctx));
        ctx->data = data;
        ctx->pool_slot = slot;
        data->row_ctx[slot] = ctx;
        lv_obj_add_event_cb(row, compact_list_row_click_cb, LV_EVENT_CLICKED, ctx);
        if (on_long_press) lv_obj_add_event_cb(row, compact_list_row_long_press_cb, LV_EVENT_LONG_PRESSED, ctx);
    }

    /* 1x1 invisible spacer at the bottom of the FULL virtual list -- not
     * hidden (a hidden object doesn't count toward scrollable content
     * size, see lv_obj_get_scroll_bottom()), just visually imperceptible.
     * This alone is what gives `list` the correct scroll range for all
     * item_count rows even though only COMPACT_LIST_POOL_SIZE real row
     * objects ever exist. */
    lv_obj_t * spacer = lv_obj_create(list);
    lv_obj_set_size(spacer, 1, 1);
    lv_obj_set_style_bg_opa(spacer, 0, 0);
    lv_obj_set_style_border_width(spacer, 0, 0);
    lv_obj_remove_flag(spacer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(spacer, LV_OBJ_FLAG_CLICKABLE);
    int32_t total_height = COMPACT_LIST_TOP_PAD + item_count * COMPACT_LIST_ROW_STRIDE;
    lv_obj_set_pos(spacer, 0, total_height > 0 ? total_height - 1 : 0);
    data->spacer = spacer;

    /* Created after the row pool (and therefore drawn on top of it, LVGL
     * z-orders siblings by creation order) so it isn't hidden behind a
     * row's own opaque background. Fixed at x=0 -- the list's own left
     * edge, i.e. the physical screen's far left -- regardless of row_x/
     * row_width above; see compact_list_set_now_playing()'s own doc
     * comment (screen_builders.h) for why this is deliberately NOT tied to
     * the row's own (possibly inset/centered) position. */
    data->now_playing_bar = NULL;
    data->now_playing_index = -1;
    if (enable_now_playing) {
        lv_obj_t * bar = lv_obj_create(list);
        lv_obj_remove_style_all(bar);
        lv_obj_set_size(bar, COMPACT_LIST_NOW_PLAYING_BAR_WIDTH, LIST_ROW_HEIGHT);
        lv_obj_set_style_bg_color(bar, now_playing_color, 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(bar, COMPACT_LIST_NOW_PLAYING_BAR_WIDTH / 2, 0);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_pos(bar, 0, COMPACT_LIST_TOP_PAD);
        lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN); /* shown by compact_list_set_now_playing() once something's actually playing */
        data->now_playing_bar = bar;
    }

    lv_obj_set_user_data(list, data);
    lv_obj_add_event_cb(list, compact_list_scroll_event_cb, LV_EVENT_SCROLL, NULL);
    lv_obj_add_event_cb(list, compact_list_delete_event_cb, LV_EVENT_DELETE, NULL);

    compact_list_update_window(list, data); /* populate the initially-visible rows */

    return list;
}

lv_obj_t * build_compact_list_screen(const char * title, lv_event_cb_t back_btn_cb,
                                      const compact_list_item_t * items, int item_count,
                                      compact_list_click_cb_t on_click, compact_list_click_cb_t on_long_press,
                                      lv_obj_t ** out_list, int32_t row_width, bool enable_now_playing,
                                      lv_color_t now_playing_color) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &style_theme_screen_bg, 0);

    if (back_btn_cb) build_back_button(scr, back_btn_cb);
    build_title(scr, title);

    lv_obj_t * list = build_compact_list_widget(scr, items, item_count, on_click, on_long_press, row_width,
                                                 enable_now_playing, now_playing_color);

    if (out_list) *out_list = list;
    return scr;
}
