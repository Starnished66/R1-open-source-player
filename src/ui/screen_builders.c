#include "screen_builders.h"
#include "gui_theme.h"
#include "gui_plugins.h"
#include "assets.h"
#include "debug_log.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef HOST_BUILD
  #define MUSIC_ROOT_DIR "./music"
#else
  /* SD card mount point -- each file that needs it defines its own copy
   * rather than sharing a header, same convention gui.c/audio.c/
   * plugin_manager.c already use. Needed here for pill_row_apply_icon()'s
   * own relative-icon-path resolution -- see its own comment. */
  #define MUSIC_ROOT_DIR "/data/mnt/sd_0"
#endif

lv_style_t list_row_style;
lv_style_t list_row_pressed_style;
lv_style_t icon_press_style;
static lv_style_t row_marquee_style;
static lv_anim_t row_marquee_anim;

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

/* Same mechanism, for text color -- see screen_builders.h's own comment on
 * style_theme_text_primary/style_theme_text_muted for why destructive-red
 * and accent-tinted text are deliberately excluded. */
lv_style_t style_theme_text_primary;
lv_style_t style_theme_text_muted;

/* Runtime geometry replaces the original panel-specific 448/464/476px
 * constants.  Small symmetric edge gutters preserve the pill silhouette
 * while allowing the same screens to fill a wider sibling device. */
static int32_t active_display_width(void) {
    lv_display_t * display = lv_display_get_default();
    int32_t width = display ? lv_display_get_horizontal_resolution(display) : 480;
    return width > 0 ? width : 480;
}

int32_t ui_list_row_width(void) {
    int32_t width = active_display_width();
    int32_t gutter = width / 120;
    if (gutter < 4) gutter = 4;
    if (gutter > 12) gutter = 12;
    return width > gutter * 2 ? width - gutter * 2 : width;
}

int32_t ui_list_row_width_wide(void) {
    int32_t width = active_display_width();
    int32_t gutter = width / 240;
    if (gutter < 2) gutter = 2;
    if (gutter > 8) gutter = 8;
    return width > gutter * 2 ? width - gutter * 2 : width;
}

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

    /* Real-device bug report: the player screen's icon buttons (favorite,
     * play mode order, more-menu) gave no feedback at all on touch, unlike
     * every list row (list_row_pressed_style above) and the prev/next
     * transport buttons (their own pressed-asset swap). Recolor-toward-black
     * at partial opacity darkens whatever's currently drawn without needing
     * a dedicated "_s" asset for every possible source image an icon might
     * be showing. */
    lv_style_init(&icon_press_style);
    lv_style_set_image_recolor(&icon_press_style, lv_color_make(0, 0, 0));
    lv_style_set_image_recolor_opa(&icon_press_style, LV_OPA_40);

    lv_anim_init(&row_marquee_anim);
    lv_anim_set_delay(&row_marquee_anim, 2000);
    lv_anim_set_repeat_delay(&row_marquee_anim, 2000);
    lv_style_init(&row_marquee_style);
    lv_style_set_anim(&row_marquee_style, &row_marquee_anim);
    /* LVGL's label default is 40px/s capped at 10 seconds. That cap makes
     * very long titles accelerate while modest overflows remain slower.
     * A lower constant speed with a much larger cap keeps the perceived
     * rate uniform across both cases. */
    lv_style_set_anim_duration(&row_marquee_style, lv_anim_speed_clamped(30, 600, 30000));

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

    /* Defaults match this app's own dominant literal text colors before
     * theming existed -- (230,230,230) is the single most common primary
     * text literal across gui.c, (160,160,160) a representative value from
     * the muted tier's (140-200,...) range. */
    lv_style_init(&style_theme_text_primary);
    lv_style_set_text_color(&style_theme_text_primary, lv_color_make(230, 230, 230));
    lv_style_init(&style_theme_text_muted);
    lv_style_set_text_color(&style_theme_text_muted, lv_color_make(160, 160, 160));
}

void row_label_enable_marquee(lv_obj_t * label) {
    lv_obj_add_style(label, &row_marquee_style, LV_PART_MAIN);
    lv_label_set_long_mode(label, LV_LABEL_LONG_SCROLL_CIRCULAR);
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

/* 76 (64px back button + 12px breathing room) / 20 -- same values as
 * gui.c's own TITLE_LABEL_LEFT_INSET/TITLE_LABEL_DEFAULT_RIGHT_MARGIN
 * (private to that file, so duplicated here rather than shared through the
 * header for one constant pair) used by every other screen's own
 * left-aligned, marquee-capable title (build_group_songs_screen(),
 * build_subsonic_list_screen(), build_files_screen(), ...). Kept in sync by
 * hand -- if either changes, check the other. */
#define BUILD_TITLE_LEFT_INSET 76
#define BUILD_TITLE_RIGHT_MARGIN 20

static lv_obj_t * build_title(lv_obj_t * scr, const char * title) {
    lv_obj_t * label = lv_label_create(scr);
    lv_label_set_text(label, title);
    /* Real-device bug report: a screen whose title gets set to something
     * genuinely long at runtime (build_compact_list_screen()'s own title,
     * e.g. a Subsonic artist/album name once you drill into it) had no
     * marquee at all and instead just rendered centered, overflowing both
     * edges -- this helper never gave the label a bounded width or
     * scrolling long_mode to begin with, unlike every other screen's own
     * title (see the INSET/MARGIN comment above). Every existing caller
     * passing a short, static title (Settings, System, Stream Media, ...)
     * just shifts from centered to left-aligned, matching how those other
     * screens already look -- no functional difference for a title that
     * already fit. */
    lv_obj_set_width(label, lv_display_get_horizontal_resolution(lv_display_get_default()) -
                                 BUILD_TITLE_LEFT_INSET - BUILD_TITLE_RIGHT_MARGIN);
    /* Vertically centered within the TITLE_ROW_HEIGHT band below the
     * status bar (matches the back button's own 64px height). */
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, BUILD_TITLE_LEFT_INSET, STATUS_BAR_CLEARANCE + (TITLE_ROW_HEIGHT - 28) / 2);
    lv_obj_add_style(label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(label, gui_theme_font(GUI_FONT_ROLE_TITLE), 0);
    row_label_enable_marquee(label);
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
                                   int32_t icon_scale_percent, bool label_inside_icon, int32_t tile_gap) {
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
        /* Real LVGL grid margin, not a bigger ICON_GRID_TILE_PAD -- STRETCH
         * alignment already subtracts margin from the tile's own computed
         * size (lv_grid.c's get_margin_hor/ver), so this shrinks the tile
         * visually within its still-untouched cell instead of touching
         * row_h/col width math (see this function's own row_h history
         * comment for why that math is fragile). tile_gap/2 per side so the
         * gap BETWEEN two adjacent tiles equals tile_gap. */
        if (tile_gap > 0) lv_obj_set_style_margin_all(tile, tile_gap / 2, 0);
        lv_obj_set_style_bg_opa(tile, 0, 0);
        /* Per-tile style overrides (plugin.set_home_layout()'s per-tile
         * config) -- every native tile leaves these unset, so this changes
         * nothing about today's transparent-background tiles. */
        if (item->has_bg_color) {
            lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(tile, lv_color_hex(item->bg_color), 0);
        }
        if (item->has_radius) lv_obj_set_style_radius(tile, item->radius, 0);
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
        lv_obj_add_style(label, &style_theme_text_primary, 0);
        /* Bumped from montserrat_16 -- real-device feedback: the main
         * menu/submenu text still read too small even after the app-wide
         * LV_FONT_DEFAULT bump, since these tiles always set an explicit
         * font and so never fell back to that default. montserrat_20
         * matches the top bar's own text size rather than inventing a new
         * in-between size. */
        lv_obj_set_style_text_font(label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
        if (item->has_text_color) lv_obj_set_style_text_color(label, lv_color_hex(item->text_color), 0);
        /* Real-device bug report: a caption longer than a tile's own width
         * ("Remote Control", "Import via Wi-Fi") had no width/wrap set at
         * all, so it just overflowed straight across into the next tile
         * (or off the right edge of the screen) instead of wrapping or
         * clipping. lv_pct(100) resolves against `label`'s own parent
         * (`tile`), matching whatever width the grid layout actually gave
         * this cell. */
        lv_obj_set_width(label, lv_pct(100));
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);

        /* tile_gap shrinks the tile's own STRETCH-computed height by that
         * much (its margin_top+margin_bottom) below row_h -- see the
         * tile_gap margin comment above -- so it has to come off here too,
         * or icon/label placement would overflow the tile's real (now
         * smaller) bottom edge. */
        int32_t available_h = row_h - tile_gap - 2 * ICON_GRID_TILE_PAD;
        if (label_inside_icon) {
            /* See this function's own header comment (screen_builders.h) --
             * these assets already reserve their own caption band near the
             * bottom of the image, so there's no separate label row to
             * budget space for: the icon alone gets centered in the full
             * available height, bigger than the below-icon layout would
             * allow. */
            int32_t top_offset = (available_h - target_icon_h) / 2;
            if (top_offset < 0) top_offset = 0;
            lv_obj_align(img_wrap, LV_ALIGN_TOP_MID, 0, top_offset);
            /* ~83% down the icon's own scaled height -- matches where the
             * reserved band actually starts, per a pixel scan of every
             * wireless/*.png asset (glyph content ends around 60-68% down
             * their shared 190px-tall native canvas; centering the label a
             * bit past that, within the remaining flat-background band,
             * comfortably clears every icon's glyph regardless of its
             * exact bottom edge). */
            int32_t label_cy = top_offset + (int32_t) (((int64_t) target_icon_h * 83) / 100);
            lv_obj_align(label, LV_ALIGN_TOP_MID, 0, label_cy - lv_font_get_line_height(gui_theme_font(GUI_FONT_ROLE_BODY)) / 2);
        } else {
            /* Real bug caught in review: the label above is LV_LABEL_LONG_
             * WRAP (genuinely multi-line capable, see its own comment), but
             * this layout budgeted only a single line_h -- a caption long
             * enough to actually wrap (more likely at a bigger font tier,
             * or just a longer label like "Remote Control" at a narrow
             * tile width) extended past its own reserved band and could
             * overlap the next tile below it. Bounding the label's own
             * height at exactly 2 lines (rather than leaving it auto-size
             * to however much text it holds) means a 3rd line clips
             * instead of pushing the overlap even further -- two lines
             * comfortably covers every real label this screen shows. */
            int32_t line_h = lv_font_get_line_height(gui_theme_font(GUI_FONT_ROLE_BODY));
            int32_t label_h = line_h * 2;
            lv_obj_set_height(label, label_h);
            int32_t content_h = target_icon_h + ICON_GRID_ICON_LABEL_GAP + label_h;
            int32_t top_offset = (available_h - content_h) / 2;
            if (top_offset < 0) top_offset = 0;
            lv_obj_align(img_wrap, LV_ALIGN_TOP_MID, 0, top_offset);
            lv_obj_align(label, LV_ALIGN_TOP_MID, 0, top_offset + target_icon_h + ICON_GRID_ICON_LABEL_GAP);
        }

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
     * COVER independently, so only the line sprite itself shows.
     *
     * Skipped entirely when tile_gap > 0: a real visible gap between tiles
     * already separates them, and these flush-cell divider lines would
     * either run pointlessly through empty background space or, on a
     * plugin-colored tile, visually clash with a color/radius that already
     * contradicts the flush-grid look these lines were built for. */
    if (tile_gap == 0 && col_count > 1) {
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

    for (int r = 0; tile_gap == 0 && r < row_count - 1; r++) {
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

void pill_row_apply_icon(lv_obj_t * row, lv_obj_t * label, const char * icon_path, int32_t icon_px,
                          lv_align_t align, int32_t x, int32_t y) {
    if (!icon_path) return;

    /* Real-device bug report: a plugin's icon path "didn't load" -- root
     * cause was PLUGINS.md's own documented "absolute path" requirement
     * being non-obvious/easy to miss; the actual plugin passed a relative
     * one ("./headphones.png"), which LVGL's POSIX fs driver (below) opens
     * relative to the app's own process working directory, nowhere near
     * the SD card. A relative path is also a completely reasonable thing
     * for a plugin author to reach for ("a file sitting next to my own
     * script"), so rather than only fixing the docs, resolve any icon_path
     * that doesn't start with '/' against <sd_root>/.plugins/ -- the
     * folder every plugin's own .lua file already lives in, matching that
     * exact "next to my script" expectation. An already-absolute path
     * (e.g. plugin.sd_root() .. "/..." or a real stock theme2 asset,
     * plugins_examples/PlaybackExtras.lua's own reference usage) is passed
     * through untouched, unaffected by this. */
    char resolved[600];
    if (icon_path[0] == '/') {
        snprintf(resolved, sizeof(resolved), "%s", icon_path);
    } else {
        snprintf(resolved, sizeof(resolved), "%s/.plugins/%s", MUSIC_ROOT_DIR, icon_path);
    }

    /* "S:" is LVGL's own POSIX-filesystem driver letter (lv_conf.h) --
     * unlike asset_path(), this doesn't prefix THEME_ROOT, so it opens
     * resolved verbatim as a real absolute path (e.g. a file on the SD
     * card, not this app's own theme directory). */
    char src[600];
    snprintf(src, sizeof(src), "S:%s", resolved);

    /* lv_image_set_scale() only changes what's DRAWN -- the widget's own
     * coordinate box stays at the source's native (unscaled) size, same
     * reason build_icon_grid_screen() above never aligns/sizes an
     * lv_image directly either. Left as its native size, `img` would (a)
     * paint its own default background across that full native box, not
     * just the visibly-scaled content -- a real-device bug report: a
     * near-black launcher icon rendered as a solid black square wildly
     * bigger than the intended 64px target -- and (b) throw off `label`'s
     * own re-alignment math below, which needs to know the icon's REAL
     * on-screen width. Same fix as build_icon_grid_screen(): a
     * fully-style-stripped wrapper explicitly sized to the true scaled
     * (target_w x target_h) dimensions, with `img` centered inside it --
     * the wrapper both clips any of img's own oversized background and
     * gives align/label-offset math a box that matches what's actually
     * visible. */
    lv_obj_t * img_wrap = lv_obj_create(row);
    lv_obj_remove_style_all(img_wrap);
    lv_obj_remove_flag(img_wrap, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(img_wrap, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t * img = lv_image_create(img_wrap);
    lv_image_set_src(img, src);

    /* Same scale-to-target-px-on-screen formula as build_icon_grid_screen()
     * above -- read the source's native size, compute the zoom that lands
     * it at icon_px regardless of the source file's own resolution. 256 =
     * LV_SCALE_NONE (100%), used as a safe fallback if the header can't be
     * read (a missing/corrupt file just renders at native size rather than
     * erroring -- a plugin icon is cosmetic, not worth failing the whole
     * row over). */
    lv_image_header_t header;
    int32_t scale = 256;
    int32_t native_w = icon_px, native_h = icon_px;
    if (lv_image_decoder_get_info(src, &header) == LV_RESULT_OK) {
        int32_t native_max = header.w > header.h ? header.w : header.h;
        if (native_max > 0) scale = (int32_t) (((int64_t) icon_px * 256) / native_max);
        native_w = header.w;
        native_h = header.h;
    }
    lv_image_set_scale(img, scale);
    int32_t target_w = (int32_t) (((int64_t) native_w * scale) / 256);
    int32_t target_h = (int32_t) (((int64_t) native_h * scale) / 256);
    lv_obj_set_size(img_wrap, target_w, target_h);
    lv_obj_center(img);
    lv_obj_align(img_wrap, align, x, y);

    /* Re-align, not re-inset -- label may already be non-default-aligned
     * (e.g. a caller that already moved it for its own reasons); this always
     * wins as the final word on the label's position, matching every other
     * call site's own single lv_obj_align() call. icon_px (not target_w) is
     * the reserved gap -- icon_px is always the longer edge, so this never
     * underestimates the icon's real width even for a non-square source. */
    lv_obj_align(label, align, x + icon_px + 12, y);
}

const lv_font_t * pill_row_resolve_text_size(const char * text_size) {
    if (!text_size) return gui_theme_font(GUI_FONT_ROLE_BODY);
    if (strcmp(text_size, "small") == 0) return &app_font_16;
    if (strcmp(text_size, "medium") == 0) return &app_font_22;
    if (strcmp(text_size, "large") == 0) return &app_font_28;
    /* 8x16 monospace bitmap font (lv_conf.h's LV_FONT_UNSCII_16) -- ASCII-only,
     * see that flag's own comment. Plugin rows only (plugin.set_home_layout()'s
     * text_size = "mono", PLUGINS.md). */
    if (strcmp(text_size, "mono") == 0) return &lv_font_unscii_16;
    return gui_theme_font(GUI_FONT_ROLE_BODY);
}

int32_t pill_row_default_width(void) {
    return ui_list_row_width();
}

lv_obj_t * build_pill_list_screen(const char * title, lv_event_cb_t back_btn_cb,
                                   const pill_list_item_t * items, int item_count,
                                   lv_style_t * toggle_accent_style, int32_t row_gap) {
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
    /* Real-device bug report: same root cause as build_compact_list_widget()'s
     * own fix (see that function's comment) -- this container never zeroed
     * its own left/right padding either, so every row (already close to
     * full screen width at the wider font tiers) rendered visibly shifted
     * right, clipping against the screen edge with a gap on the left. Flex
     * centering alone doesn't cancel this out: the row is centered within
     * the padded content box, not the screen, so asymmetric slack left
     * after that centering (this box is narrower than the screen once
     * padding is subtracted) still reads as an off-center row. */
    lv_obj_set_style_pad_all(list, 0, 0);
    /* Vertical-only -- see the matching comment in build_icon_grid_screen. */
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    /* track_cross_place (3rd arg) -- not cross_place (2nd arg) -- is what
     * actually centers a narrower-than-container row here: with FLEX_FLOW_
     * COLUMN and no wrap, LVGL's single track shrinks to its widest child
     * rather than filling the container's full cross size, so cross_place=
     * CENTER (an item's placement *within* its track) is a no-op once the
     * track already equals that item's own width -- it's track_cross_place
     * (the track's own placement within the container) that determines
     * whether narrower rows sit flush-left or centered. Was START: every
     * row narrower than the container (any plugin-resized row, and even
     * native rows by their few-px native/container gutter) rendered pinned
     * to the left edge with all slack on the right -- barely visible at
     * near-full native widths, glaring at a deliberately narrow
     * plugin.set_home_layout()/register_list_item() width (real-device
     * report: a themed list's cards read "stuck to the left edge"). CENTER
     * here centers the track itself, which is what actually centers every
     * row inside it, native or plugin-resized alike. */
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(list, row_gap, 0);
    lv_obj_set_style_pad_top(list, 4, 0);

    for (int i = 0; i < item_count; i++) {
        const pill_list_item_t * item = &items[i];

        /* 0 (every existing native row's compound literal leaves this
         * unset) means "default" -- 124px, PNG pill sprite, exactly today's
         * look. A plugin-resized row instead gets a plain rounded-rect fill
         * -- see PILL_ROW_HEIGHT_MIN's own comment in screen_builders.h for
         * why the PNG sprite can't just stretch to a new height. */
        int32_t height = 124;
        int32_t width = pill_row_default_width();
        /* has_bg_color/has_radius also force the plain-fill path below --
         * item_bg.png is a raster sprite, it can't be recolored/reshaped,
         * same reasoning row_height/row_width already forced this path for. */
        bool resized = width != 448 || item->row_height > 0 || item->row_width > 0 ||
                       item->has_bg_color || item->has_radius;
        if (item->row_height > 0) {
            height = item->row_height;
            if (height < PILL_ROW_HEIGHT_MIN) height = PILL_ROW_HEIGHT_MIN;
            if (height > PILL_ROW_HEIGHT_MAX) height = PILL_ROW_HEIGHT_MAX;
        }
        if (item->row_width > 0) {
            width = item->row_width;
            if (width < PILL_ROW_WIDTH_MIN) width = PILL_ROW_WIDTH_MIN;
            if (width > PILL_ROW_WIDTH_MAX) width = PILL_ROW_WIDTH_MAX;
        }

        lv_obj_t * row = lv_obj_create(list);
        lv_obj_set_size(row, width, height);
        /* item_bg.png is a rounded-rect sprite with transparent corners --
         * without an explicit black bg_color here, LVGL's own default
         * object background (light gray/white) shows through at those
         * corners, since bg_opa=COVER is needed for the image itself to
         * draw at all and also fills the object's full square bounding
         * box underneath it. */
        lv_obj_add_style(row, &style_theme_screen_bg, 0);
        if (resized) {
            lv_obj_set_style_radius(row, item->has_radius ? item->radius : LIST_ROW_RADIUS, 0);
            lv_obj_set_style_bg_color(row, item->has_bg_color ? lv_color_hex(item->bg_color) : LIST_ROW_BG_COLOR, 0);
        } else {
            lv_obj_set_style_bg_image_src(row, asset_path("touch_list/item_bg.png"), 0);
        }
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t * label = lv_label_create(row);
        lv_label_set_text(label, item->label);
        lv_obj_add_style(label, &style_theme_text_primary, 0);
        /* Bumped from montserrat_16 -- see the matching comment on the icon
         * grid's own label above. text_size NULL (every native row) ->
         * gui_theme_font(GUI_FONT_ROLE_BODY), unchanged from before this field existed -- see
         * pill_row_resolve_text_size()'s own comment. */
        lv_obj_set_style_text_font(label, pill_row_resolve_text_size(item->text_size), 0);
        if (item->has_text_color) lv_obj_set_style_text_color(label, lv_color_hex(item->text_color), 0);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 24, 0);
        pill_row_apply_icon(row, label, item->icon_asset, PILL_ROW_ICON_PX_DEFAULT, LV_ALIGN_LEFT_MID, 24, 0);

        /* Keep long labels inside their own row instead of letting the
         * label's content-sized box extend over the accessory or following
         * row. LV_LABEL_LONG_SCROLL_CIRCULAR is inert when the text fits and
         * becomes a single-line marquee only when it does not. */
        int32_t label_left = 24 + (item->icon_asset ? PILL_ROW_ICON_PX_DEFAULT + 12 : 0);
        int32_t accessory_space = item->accessory == PILL_ACCESSORY_TOGGLE ? 112
                                  : item->accessory == PILL_ACCESSORY_CHEVRON ? 60 : 24;
        int32_t label_width = width - label_left - accessory_space;
        if (label_width < 40) label_width = 40;
        lv_obj_set_width(label, label_width);
        const lv_font_t * label_font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
        lv_obj_set_height(label, label_font ? lv_font_get_line_height(label_font) : 24);
        row_label_enable_marquee(label);
        /* NULL/"left" (every native row) -> unchanged default. "center"/
         * "right" only shift where the text sits inside its own already-
         * reserved box (label_width above) -- pill_row_apply_icon()'s icon
         * placement is untouched either way. plugin.set_home_layout() only
         * (PLUGINS.md), list mode. */
        lv_text_align_t text_align = LV_TEXT_ALIGN_LEFT;
        if (item->text_align) {
            if (strcmp(item->text_align, "center") == 0) text_align = LV_TEXT_ALIGN_CENTER;
            else if (strcmp(item->text_align, "right") == 0) text_align = LV_TEXT_ALIGN_RIGHT;
        }
        lv_obj_set_style_text_align(label, text_align, 0);

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
                lv_obj_add_style(chevron, &style_theme_text_muted, 0);
                lv_obj_set_style_text_font(chevron, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
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

/* Paged mode -- see compact_list_set_paged_provider()'s own doc comment
 * (screen_builders.h, also where compact_list_fetch_page_cb_t/COMPACT_LIST_
 * LABEL_MAX are declared). One cached page comfortably covers the visible
 * pool plus generous overscan without re-fetching on every single-row
 * scroll -- matches the original DB-load scoping doc's own "64-128 records
 * per cached page" recommendation. Labels are copied here (fixed-size
 * buffers, not pointers) since a paged fetch's source rows (song_row_t/
 * group_row_t) are only ever short-lived stack values -- there is no
 * long-lived backing array to point into the way eager mode's items[] can. */
#define COMPACT_LIST_PAGE_CACHE_SIZE 128

typedef struct {
    compact_list_item_t * items; /* owned copy -- see build_compact_list_screen()'s own doc comment. Eager mode only (fetch_page == NULL); untouched/stale in paged mode. */
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
    int32_t row_height;
    int32_t row_stride;
    int window_start; /* index currently shown by rows[0]; -1 forces the first update to actually run */
    int refresh_index; /* >=0 requests a targeted repaint without disturbing the recycled window */
    lv_obj_t * rows[COMPACT_LIST_POOL_SIZE];
    lv_obj_t * leading_images[COMPACT_LIST_POOL_SIZE];
    lv_obj_t * trailing_images[COMPACT_LIST_POOL_SIZE];
    void * row_ctx[COMPACT_LIST_POOL_SIZE]; /* opaque to this struct, freed alongside it -- see compact_list_row_ctx_t below */
    int row_logical_index[COMPACT_LIST_POOL_SIZE]; /* item currently represented by each recycled slot; -1 when unused */
    lv_obj_t * spacer; /* repositioned by compact_list_set_items() when item_count changes */
    lv_obj_t * now_playing_bar; /* NULL if this list wasn't built with enable_now_playing -- see compact_list_set_now_playing() */
    int now_playing_index; /* -1 = nothing playing/matching in this list */

    /* NULL fetch_page = eager mode (default, existing behavior above
     * unchanged). Non-NULL = paged mode: item_count still holds the
     * logical total (from compact_list_set_paged_provider()'s own
     * total_count arg), but rows are fetched into cache_labels[] on demand
     * as the list scrolls, never all materialized at once. */
    compact_list_fetch_page_cb_t fetch_page;
    void * provider_ctx;
    int cache_start; /* logical offset of cache_labels[0]; -1 = cache empty/invalid, forces a fetch on next window update */
    int cache_count; /* how many of cache_labels[] are valid (less than COMPACT_LIST_PAGE_CACHE_SIZE only at the tail of the list) */
    compact_list_page_row_t cache_rows[COMPACT_LIST_PAGE_CACHE_SIZE];
    compact_list_row_decorator_cb_t row_decorator;
    void * row_decorator_ctx;

    /* Background page fetch -- see compact_list_ensure_cache()'s own
     * comment for why this moved off the scroll-event/UI thread. NULL
     * when no fetch is currently in flight for this list; at most one at a
     * time (see that function's own reasoning). pending_job_started_at is
     * only meaningful while pending_job is non-NULL. poll_timer is created
     * once per list (build_compact_list_widget()) and paused whenever
     * nothing is pending, so it costs nothing while idle. */
    struct compact_list_fetch_job_s * pending_job;
    struct timespec pending_job_started_at;
    lv_timer_t * poll_timer;
} compact_list_virtual_data_t;

/* Bar width only -- height always matches LIST_ROW_HEIGHT (see
 * compact_list_set_now_playing()). Thin enough to read as an accent stripe,
 * not a second column competing with the row's own content. */
#define COMPACT_LIST_NOW_PLAYING_BAR_WIDTH 5

typedef struct {
    compact_list_virtual_data_t * data;
    int pool_slot;
    int logical_index;
} compact_list_row_ctx_t;

static void compact_list_row_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    compact_list_row_ctx_t * ctx = (compact_list_row_ctx_t *) lv_event_get_user_data(e);
    if (ctx->data->long_press_fired) { /* see compact_list_row_long_press_cb()'s own comment */
        ctx->data->long_press_fired = false;
        return;
    }
    int index = ctx->logical_index;
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
    int index = ctx->logical_index;
    if (index < 0 || index >= ctx->data->item_count) return;
    ctx->data->on_long_press(index);
}

/* How often compact_list_poll_fetch_cb() below checks a pending job for
 * completion -- a DB fetch takes at least several ms, so polling every
 * frame (like the 60fps drag timers elsewhere in this file's sibling gui.c)
 * would be pure waste; this is still fast enough that the delay between a
 * fetch actually landing and the list repainting is imperceptible. */
#define COMPACT_LIST_FETCH_POLL_MS 50

/* Real-device precedent: a stuck SD-card read can land in an uninterruptible
 * kernel wait that nothing but abandoning it escapes -- see gui.c's own
 * LIBRARY_SCAN_FILE_TIMEOUT_MS comment for the same class of fault on this
 * hardware (confirmed there via /proc/<pid>/task/<tid>/status+wchan showing
 * the thread parked in __bread_gfp after a card came back from an unclean
 * unmount). Same budget here, same tolerance: past this, compact_list_poll_
 * fetch_cb() stops waiting on the job and detaches it instead of joining,
 * leaking one thread and one small allocation for the rare, genuinely-
 * corrupted-card case rather than ever blocking scrolling on it. */
#define COMPACT_LIST_FETCH_TIMEOUT_MS 5000L

/* One in-flight background page fetch -- heap-allocated separately from
 * compact_list_virtual_data_t (not embedded in it) so it can safely outlive
 * the list/data if the read is still running when the list is deleted, or
 * handed a new provider, before this one finishes (see compact_list_set_
 * paged_provider()'s/compact_list_set_items()'s own handling, and compact_
 * list_delete_event_cb()'s). The worker thread below touches only its own
 * fields here via fetch_page()'s own metadata_db.c call (thread-safe on its
 * own METADATA_DB_GUARD) -- never any LVGL object or compact_list_virtual_
 * data_t field -- so there is no cross-thread UI-safety concern; the poll
 * timer is the only place a finished job's result is ever applied, and it
 * always runs on the UI thread like every other LVGL timer callback. */
typedef struct compact_list_fetch_job_s {
    pthread_t thread;
    compact_list_fetch_page_cb_t fetch_page;
    void * provider_ctx;
    int offset;
    int count;
    compact_list_page_row_t rows[COMPACT_LIST_PAGE_CACHE_SIZE];
    /* -1 until the worker thread finishes; written last, the only field the
     * poll timer reads to decide the job is done. atomic_int rather than a
     * plain volatile int -- analyzer finding: pthread_join() (called once
     * result_count>=0 is observed) already makes everything AFTER that
     * observation fully synchronized, including labels[]/refs, but the
     * read/write of result_count ITSELF has no C11-recognized ordering
     * between the two threads otherwise, which is formally a data race
     * regardless of what happens later. A plain int (word-sized, written
     * once, no read-modify-write) is safe in practice on real hardware
     * either way -- this is the exact same poll-a-flag idiom used
     * throughout the rest of this codebase (cover_decode_done_flag,
     * lyrics_load_done_flag, etc.) -- but atomic_int makes this specific,
     * already-flagged instance formally correct at no behavioral cost:
     * plain assignment/comparison on an _Atomic-qualified object already
     * uses sequentially consistent ordering by default in C11, so every
     * existing read/write site below is unchanged syntactically. */
    atomic_int result_count;
    /* Two owners share this job's lifetime: the worker thread's own
     * execution, and whichever UI-thread path eventually resolves it
     * (compact_list_poll_fetch_cb()'s normal completion, or compact_list_
     * abandon_job() below). Each releases its own reference exactly once
     * via compact_list_job_release(); whichever release brings this to 0
     * -- which one that is depends on real timing and isn't knowable in
     * advance -- does the actual free(). A plain bool flag here (an
     * earlier version of this fix used one) is NOT safe for this: the
     * worker's check-then-free and the UI thread's write-then-detach are
     * two independent read/write pairs on the SAME field with no ordering
     * between them, so either side could free the job while the other is
     * still about to touch it (real analyzer-caught bug, not just a
     * theoretical concern -- pthread_detach(job->thread) reading `job`
     * after the worker had already freed it, or the opposite: worker sees
     * `false` and returns without freeing at all, leaking it anyway).
     * An atomic refcount makes "who does the free" an unambiguous, race-
     * free question. */
    atomic_int refs;
} compact_list_fetch_job_t;

/* Releases one of a job's two references -- see compact_list_fetch_job_s's
 * own doc comment. atomic_fetch_sub() returns the value from BEFORE the
 * decrement, so `== 1` means this call just brought it from 1 to 0: the
 * other side already released, and this call is the second/last one, so
 * it's the one responsible for the actual free(). */
static void compact_list_job_release(compact_list_fetch_job_t * job) {
    if (atomic_fetch_sub(&job->refs, 1) == 1) free(job);
}

static void * compact_list_fetch_worker(void * arg) {
    compact_list_fetch_job_t * job = (compact_list_fetch_job_t *) arg;
    job->result_count = job->fetch_page(job->provider_ctx, job->offset, job->count, job->rows);
    compact_list_job_release(job); /* the worker's own reference -- job may already be freed by the other side by the time this call returns */
    return NULL;
}

/* Abandons a job without waiting on it -- pthread_join() would block for as
 * long as the underlying read takes, which is exactly what this whole
 * background-fetch scheme exists to never do again on the UI/scroll
 * thread. Real-device/analyzer finding: every abandon path used to just
 * detach and never free the job struct at all, tolerated as a "rare,
 * bounded" leak for the original 5s-stuck-read timeout case -- but this is
 * also called from compact_list_cancel_pending_job() below on every
 * ordinary list/search switch, not just that rare case, so the leak was
 * far from rare in practice. Two paths now: if the job has ALREADY
 * finished (result_count landed) but just hasn't been polled/applied yet,
 * join it directly -- pthread_join() is effectively instant once
 * result_count is set, the same reasoning compact_list_poll_fetch_cb()'s
 * own normal-completion path already relies on. Only a job that's
 * genuinely still running falls through to detach; either way, this
 * releases the UI-side reference afterward, safe to do unconditionally
 * since pthread_detach()/pthread_join() above already ran while this
 * thread's own reference was still guaranteed outstanding (the refcount
 * can't have reached 0 without this release, so job->thread was still
 * valid to read either way). */
static void compact_list_abandon_job(compact_list_fetch_job_t * job) {
    if (job->result_count >= 0) {
        pthread_join(job->thread, NULL);
    } else {
        pthread_detach(job->thread);
    }
    compact_list_job_release(job);
}

/* Abandons data->pending_job (if any) and clears it -- called whenever a
 * list is about to point at a different provider/items[] entirely (compact_
 * list_set_items()/compact_list_set_paged_provider() below), so a fetch
 * still running against the OLD provider can never land later and overwrite
 * cache_labels[] with data for a list the caller has already replaced. The
 * abandoned job's result, if it ever arrives, is simply never looked at
 * again, but the allocation itself is still safely reclaimed via
 * compact_list_job_release() -- see compact_list_abandon_job()'s own
 * comment -- just triggered by "provider changed" instead of a timeout. */
static void compact_list_cancel_pending_job(compact_list_virtual_data_t * data) {
    if (!data->pending_job) return;
    compact_list_abandon_job(data->pending_job);
    data->pending_job = NULL;
    if (data->poll_timer) lv_timer_pause(data->poll_timer);
}

/* Centers a COMPACT_LIST_PAGE_CACHE_SIZE fetch around `first` with the same
 * overscan compact_list_ensure_cache() uses for both the sync first fill
 * and the later async scroll misses. Returns the row count to request
 * (0 at the empty-list edge); writes the logical start into *out_start. */
static int compact_list_page_range(const compact_list_virtual_data_t * data, int first, int * out_start) {
    int fetch_start = first - (COMPACT_LIST_PAGE_CACHE_SIZE - COMPACT_LIST_POOL_SIZE) / 2;
    if (fetch_start < 0) fetch_start = 0;
    int max_start = data->item_count - COMPACT_LIST_PAGE_CACHE_SIZE;
    if (max_start < 0) max_start = 0;
    if (fetch_start > max_start) fetch_start = max_start;

    int want = COMPACT_LIST_PAGE_CACHE_SIZE;
    if (fetch_start + want > data->item_count) want = data->item_count - fetch_start;
    if (want < 0) want = 0;
    *out_start = fetch_start;
    return want;
}

/* Paged mode only -- kicks off a background refetch of cache_rows[] via
 * fetch_page() if the current cache doesn't already cover [first,
 * first+POOL_SIZE) AND no fetch is currently in flight for this list.
 * Centers the new cache page around `first` with generous overscan on both
 * sides so a scroll continuing in the same direction doesn't immediately
 * fall back out of the cached range and re-fetch again next frame. One
 * fetch_page() call, not one per row -- COMPACT_LIST_PAGE_CACHE_SIZE rows at
 * once.
 *
     * Initial fills use this same worker path as scroll misses: even a bounded
     * first page can fault slow SD-backed mmap pages and must never block LVGL.
     * At most one fetch is ever in flight
 * per list (the `pending_job` check below) -- a second cache miss arriving
 * while one's already running just waits for it, rather than piling up
 * concurrent reads against the same list; the next scroll tick (or
 * compact_list_poll_fetch_cb() once it lands) tries again if the window
 * has moved further since. */
static void compact_list_ensure_cache(lv_obj_t * list, compact_list_virtual_data_t * data, int first) {
    (void) list;
    int window_end = first + COMPACT_LIST_POOL_SIZE;
    bool covered = data->cache_start >= 0 && first >= data->cache_start &&
                    window_end <= data->cache_start + data->cache_count;
    if (covered) return;
    if (data->pending_job) return;

    int fetch_start = 0;
    int want = compact_list_page_range(data, first, &fetch_start);

    if (want == 0) {
        data->cache_count = 0;
        data->cache_start = fetch_start;
        return;
    }

    compact_list_fetch_job_t * job = malloc(sizeof(*job));
    if (!job) return; /* next scroll tick retries */
    job->fetch_page = data->fetch_page;
    job->provider_ctx = data->provider_ctx;
    job->offset = fetch_start;
    job->count = want;
    atomic_init(&job->result_count, -1);
    atomic_init(&job->refs, 2); /* worker's own reference + whichever UI-side path resolves it -- see compact_list_fetch_job_s's own doc comment */
    if (pthread_create(&job->thread, NULL, compact_list_fetch_worker, job) != 0) {
        DBG_LOG("compact_list: pthread_create FAILED offset=%d want=%d\n", fetch_start, want);
        free(job);
        return; /* next scroll tick retries */
    }

    data->pending_job = job;
    clock_gettime(CLOCK_MONOTONIC, &data->pending_job_started_at);
    if (data->poll_timer) lv_timer_resume(data->poll_timer);
}

/* Repositions/relabels the row pool so it covers the range of items
 * actually scrolled into (or near) view -- called once up front to
 * populate the initial view, then again on every LV_EVENT_SCROLL. Cheap
 * even though it touches every pool row on each call: COMPACT_LIST_POOL_SIZE
 * label-text-and-position updates is nothing like the cost of the
 * thousands of real LVGL objects this replaces, and the early return when
 * the window hasn't actually moved (the overwhelmingly common case between
 * scroll events on this device's touch sampling rate) skips even that --
 * except in paged mode, where a stale cache (e.g. right after compact_
 * list_set_paged_provider() refreshes the data with window_start reset to
 * -1) must still be checked even if the visible window itself hasn't
 * moved, so the cache check runs before that early return. */
static void compact_list_update_window(lv_obj_t * list, compact_list_virtual_data_t * data) {
#ifdef UI_PERF_TRACE
    struct timespec perf_start_ts;
    clock_gettime(CLOCK_MONOTONIC, &perf_start_ts);
#endif
    int32_t scroll_y = lv_obj_get_scroll_y(list);
    if (scroll_y < 0) scroll_y = 0;
    int first = (int) (scroll_y / data->row_stride) - COMPACT_LIST_POOL_SIZE / 4;
    if (first < 0) first = 0;
    int max_first = data->item_count - COMPACT_LIST_POOL_SIZE;
    if (max_first < 0) max_first = 0;
    if (first > max_first) first = max_first;

    if (data->fetch_page) compact_list_ensure_cache(list, data, first);

    int targeted_index = data->refresh_index;
    data->refresh_index = -1;
    if (first == data->window_start && targeted_index < 0) return;
    bool force_repaint = data->window_start < 0;
    data->window_start = first;

    /* Keep rows tied to logical_index % POOL_SIZE.  Advancing the window by
     * one row then repaints only the one slot entering at the far edge,
     * instead of relabelling/redecorating all 20 rows on every scroll tick.
     * A forced refresh (window_start == -1) still repaints the whole window
     * after a page fetch, theme change, or explicit refresh request. */
    int window_end = first + COMPACT_LIST_POOL_SIZE;
    for (int slot = 0; slot < COMPACT_LIST_POOL_SIZE; slot++) {
        int old_index = data->row_logical_index[slot];
        if (old_index < first || old_index >= window_end || old_index >= data->item_count) {
            lv_obj_add_flag(data->rows[slot], LV_OBJ_FLAG_HIDDEN);
            data->row_logical_index[slot] = -1;
            ((compact_list_row_ctx_t *) data->row_ctx[slot])->logical_index = -1;
        }
    }

    for (int index = first; index < window_end && index < data->item_count; index++) {
        int slot = index % COMPACT_LIST_POOL_SIZE;
        lv_obj_t * row = data->rows[slot];
        if (!force_repaint && data->row_logical_index[slot] == index && index != targeted_index) continue;
        data->row_logical_index[slot] = index;
        ((compact_list_row_ctx_t *) data->row_ctx[slot])->logical_index = index;
        lv_obj_set_y(row, COMPACT_LIST_TOP_PAD + index * data->row_stride);
        const char * label = NULL;
        int64_t identity = 0;
        const char * trailing_asset = NULL;
        if (data->fetch_page) {
            int cache_idx = index - data->cache_start;
            bool cached = cache_idx >= 0 && cache_idx < data->cache_count;
            compact_list_page_row_t * info = cached ? &data->cache_rows[cache_idx] : NULL;
            /* A cache miss used to paint a visible card with an empty label
             * and (for Albums) placeholder art. Names then arrived after the
             * async fetch -- and after pad_left/image changes had already
             * laid the still-empty text out, so the real title waited on a
             * later image refresh to become visible. Hide the slot until
             * the page lands instead of showing that empty card. */
            if (!info) {
                lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
                data->row_logical_index[slot] = -1;
                ((compact_list_row_ctx_t *) data->row_ctx[slot])->logical_index = -1;
                continue;
            }
            label = info->label;
            identity = info->identity;
            trailing_asset = info->trailing_asset[0] ? info->trailing_asset : NULL;
        } else {
            label = data->items[index].label;
            identity = data->items[index].identity;
            trailing_asset = data->items[index].trailing_asset;
        }
        lv_obj_remove_flag(row, LV_OBJ_FLAG_HIDDEN);
        lv_obj_t * trailing = data->trailing_images[slot];
        if (trailing_asset && trailing_asset[0]) {
            lv_image_set_src(trailing, asset_path(trailing_asset));
            lv_obj_remove_flag(trailing, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_pad_right(row, 70, 0);
        } else {
            lv_obj_add_flag(trailing, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_pad_right(row, LIST_ROW_LABEL_INSET, 0);
        }
        /* Pad/image first: lv_label_set_text() measures against the current
         * content box, and the album-art decorator's pad_left=100 is what
         * keeps the title to the right of the 72px cover. Setting the text
         * before that pad made the name lay out under the image until a
         * later thumbnail refresh invalidated the label. */
        if (data->row_decorator)
            data->row_decorator(list, row, data->leading_images[slot], index, slot,
                                identity, data->row_decorator_ctx);
        lv_label_set_text(row, label ? label : "");

        /* The row itself is a label, so LVGL aligns child images against
         * its padded content box rather than against the visible card.
         * Accessory padding changes per row (84px for album art, 70px for
         * a quality badge); without compensating for it the image is
         * shifted inward by that same amount and overlaps the text. Keep
         * the visual insets relative to the physical row instead. The Y
         * correction likewise cancels list_row_style's asymmetric text
         * padding so both images remain vertically centred in the card. */
        int32_t pad_left = lv_obj_get_style_pad_left(row, LV_PART_MAIN);
        int32_t pad_right = lv_obj_get_style_pad_right(row, LV_PART_MAIN);
        int32_t pad_top = lv_obj_get_style_pad_top(row, LV_PART_MAIN);
        int32_t pad_bottom = lv_obj_get_style_pad_bottom(row, LV_PART_MAIN);
        lv_obj_align(data->leading_images[slot], LV_ALIGN_LEFT_MID,
                     14 - pad_left, (pad_top - pad_bottom) / -2);
        lv_obj_align(data->trailing_images[slot], LV_ALIGN_RIGHT_MID,
                     pad_right - 14, (pad_top - pad_bottom) / -2);
    }
#ifdef UI_PERF_TRACE
    struct timespec perf_end_ts;
    clock_gettime(CLOCK_MONOTONIC, &perf_end_ts);
    long perf_us = (perf_end_ts.tv_sec - perf_start_ts.tv_sec) * 1000000L +
                   (perf_end_ts.tv_nsec - perf_start_ts.tv_nsec) / 1000L;
    if (perf_us >= 1000) {
        printf("PERF compact_window us=%ld first=%d paged=%d items=%d\n",
               perf_us, first, data->fetch_page != NULL, data->item_count);
    }
#endif
}

/* Checks data->pending_job (set by compact_list_ensure_cache() above) for
 * completion, applies the result and repaints once it lands, and pauses
 * itself again once there's nothing left to watch -- see this timer's own
 * creation comment (build_compact_list_widget()) for why it's per-list and
 * starts paused rather than a single always-on global timer. */
static void compact_list_poll_fetch_cb(lv_timer_t * timer) {
    lv_obj_t * list = (lv_obj_t *) lv_timer_get_user_data(timer);
    compact_list_virtual_data_t * data = (compact_list_virtual_data_t *) lv_obj_get_user_data(list);
    compact_list_fetch_job_t * job = data->pending_job;
    if (!job) {
        lv_timer_pause(timer);
        return;
    }

    if (job->result_count >= 0) {
        pthread_join(job->thread, NULL); /* already returned by the time result_count landed -- instant, not a real wait */
        data->pending_job = NULL;
        memcpy(data->cache_rows, job->rows, sizeof(data->cache_rows));
        data->cache_count = job->result_count;
        data->cache_start = job->offset;
        compact_list_job_release(job); /* the UI-side reference -- see compact_list_fetch_job_s's own doc comment */

        data->window_start = -1; /* force a repaint even if the visible offset didn't move meanwhile */
        compact_list_update_window(list, data);
        /* Real bug caught here: compact_list_update_window() above calls
         * compact_list_ensure_cache() against the list's CURRENT live
         * scroll position, not the one this job was fetched for -- if the
         * user kept scrolling while this fetch was in flight, that call
         * can itself spawn a brand-new pending_job and resume this exact
         * timer for it, right before this line would unconditionally pause
         * it again -- silently orphaning the new job forever (it finishes,
         * but nothing is left polling it). Only pause if nothing new got
         * queued during that repaint. */
        if (!data->pending_job) lv_timer_pause(timer);
        return;
    }

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long elapsed_ms = (now.tv_sec - data->pending_job_started_at.tv_sec) * 1000L +
                      (now.tv_nsec - data->pending_job_started_at.tv_nsec) / 1000000L;
    if (elapsed_ms >= COMPACT_LIST_FETCH_TIMEOUT_MS) {
        /* See COMPACT_LIST_FETCH_TIMEOUT_MS's own comment -- a genuinely
         * stuck read, not just a slow one. Give up waiting; the next scroll
         * tick starts a fresh job once this is cleared. Rows stay blank for
         * whatever range this would have filled until then. */
        compact_list_abandon_job(job);
        data->pending_job = NULL;
        lv_timer_pause(timer);
    }
}

static void compact_list_scroll_event_cb(lv_event_t * e) {
    lv_obj_t * list = lv_event_get_target(e);
    compact_list_update_window(list, (compact_list_virtual_data_t *) lv_obj_get_user_data(list));
}

static void compact_list_delete_event_cb(lv_event_t * e) {
    compact_list_virtual_data_t * data = (compact_list_virtual_data_t *) lv_obj_get_user_data(lv_event_get_target(e));
    if (data->poll_timer) lv_timer_delete(data->poll_timer);
    if (data->pending_job) {
        /* Short bounded wait, not indefinite -- the overwhelmingly common
         * case is a fetch that's already done or lands within a handful of
         * ms, in which case this joins and frees normally below; a
         * genuinely slow/stuck one falls through to the same detach-and-
         * abandon tolerance compact_list_poll_fetch_cb()'s own timeout
         * uses, rather than blocking screen teardown/navigation on it. */
        compact_list_fetch_job_t * job = data->pending_job;
        for (int i = 0; i < 20 && job->result_count < 0; i++) usleep(10000);
        if (job->result_count >= 0) {
            pthread_join(job->thread, NULL);
            compact_list_job_release(job); /* the UI-side reference -- see compact_list_fetch_job_s's own doc comment */
        } else {
            compact_list_abandon_job(job);
        }
    }
    for (int slot = 0; slot < COMPACT_LIST_POOL_SIZE; slot++) free(data->row_ctx[slot]);
    free(data->items);
    free(data);
}

void compact_list_scroll_to_index(lv_obj_t * list, int index) {
    compact_list_virtual_data_t * data = (compact_list_virtual_data_t *) lv_obj_get_user_data(list);
    lv_obj_scroll_to_y(list, COMPACT_LIST_TOP_PAD + index * data->row_stride, LV_ANIM_OFF);
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
    lv_obj_set_y(data->now_playing_bar, COMPACT_LIST_TOP_PAD + item_index * data->row_stride);
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

    compact_list_cancel_pending_job(data);
    data->fetch_page = NULL; /* switch back to eager mode -- see this field's own doc comment */
    data->provider_ctx = NULL;
    data->cache_start = -1;
    data->cache_count = 0;

    free(data->items);
    data->items = NULL;
    if (item_count > 0) {
        data->items = malloc(sizeof(compact_list_item_t) * (size_t) item_count);
        memcpy(data->items, items, sizeof(compact_list_item_t) * (size_t) item_count);
    }
    data->item_count = item_count;
    data->window_start = -1; /* force compact_list_update_window() below to actually repaint */

    int32_t total_height = COMPACT_LIST_TOP_PAD + item_count * data->row_stride;
    lv_obj_set_pos(data->spacer, 0, total_height > 0 ? total_height - 1 : 0);

    /* A shorter result set than the previous scroll position would
     * otherwise leave the view showing blank space past the new (shorter)
     * scrollable range. */
    lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF);
    compact_list_update_window(list, data);
}

/* Switches a list to (or refreshes) paged mode -- see this function's own
 * declaration (screen_builders.h) for the full contract. Mirrors compact_
 * list_set_items() above (same window_start reset / spacer resize / scroll-
 * to-top / repaint shape), but points the list at a fetch_page() provider
 * instead of copying a caller-owned items[] array -- rows are cached and
 * fetched on demand from compact_list_update_window() as the list scrolls,
 * never all materialized at once. Pass fetch_page == NULL to switch back to
 * eager mode (the list then shows whatever compact_list_set_items() last
 * gave it, defaulting to empty if that was never called). */
void compact_list_set_paged_provider(lv_obj_t * list, compact_list_fetch_page_cb_t fetch_page, void * ctx,
                                      int total_count) {
    compact_list_virtual_data_t * data = (compact_list_virtual_data_t *) lv_obj_get_user_data(list);

    compact_list_cancel_pending_job(data);
    data->fetch_page = fetch_page;
    data->provider_ctx = ctx;
    data->cache_start = -1; /* forces compact_list_ensure_cache() to actually fetch on next window update */
    data->cache_count = 0;
    /* NULL fetch_page: leave item_count as whatever compact_list_set_items()
     * last set (0 if never called) -- don't clobber it here. */
    if (fetch_page) data->item_count = total_count;
    data->window_start = -1; /* force compact_list_update_window() below to actually repaint */

    int32_t total_height = COMPACT_LIST_TOP_PAD + data->item_count * data->row_stride;
    lv_obj_set_pos(data->spacer, 0, total_height > 0 ? total_height - 1 : 0);

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
    /* Real-device bug report: every row in every compact-list screen
     * (Artists/Album Artist/All Songs, the Files search overlay, the
     * timezone city list, ...) sat shifted well to the right, leaving a
     * large gap on the left and clipping against the right edge. Root
     * cause: rows below are positioned with an absolute lv_obj_set_pos(row,
     * row_x, ...), which places them relative to this container's own
     * CONTENT area -- but `list` never zeroed its padding, so it was still
     * carrying LVGL's default object theme padding on all four sides,
     * silently offsetting every row_x (computed assuming zero padding) by
     * that same amount on top of its own value. */
    lv_obj_set_style_pad_all(list, 0, 0);
    /* Vertical-only -- see the matching comment in build_icon_grid_screen.
     * No flex flow here (unlike the old implementation) -- every row is
     * positioned by hand as part of the virtualization scheme below. */
    lv_obj_set_scroll_dir(list, LV_DIR_VER);

    compact_list_virtual_data_t * data = malloc(sizeof(*data));
    data->item_count = item_count;
    data->on_click = on_click;
    data->on_long_press = on_long_press;
    data->long_press_fired = false;
    data->row_height = LIST_ROW_HEIGHT;
    data->row_stride = COMPACT_LIST_ROW_STRIDE;
    data->window_start = -1;
    data->refresh_index = -1;
    data->items = NULL;
    if (item_count > 0) {
        data->items = malloc(sizeof(compact_list_item_t) * (size_t) item_count);
        memcpy(data->items, items, sizeof(compact_list_item_t) * (size_t) item_count);
    }
    data->fetch_page = NULL;
    data->provider_ctx = NULL;
    data->cache_start = -1;
    data->cache_count = 0;
    data->pending_job = NULL;
    data->poll_timer = NULL; /* created below, once `list` itself exists */
    data->row_decorator = NULL;
    data->row_decorator_ctx = NULL;

    /* Virtual rows are positioned absolutely, so flex cross alignment does
     * not center them.  Center every width explicitly. */
    int32_t row_x = (lv_display_get_horizontal_resolution(lv_display_get_default()) - row_width) / 2;
    if (row_x < 0) row_x = 0;

    for (int slot = 0; slot < COMPACT_LIST_POOL_SIZE; slot++) {
        lv_obj_t * row = lv_label_create(list);
        lv_obj_add_style(row, &list_row_style, 0);
        lv_obj_add_style(row, &list_row_pressed_style, LV_STATE_PRESSED);
        if (row_width != LIST_ROW_WIDTH) lv_obj_set_style_width(row, row_width, 0); /* local override -- see this param's own doc comment (screen_builders.h) */
        /* This label is also the fixed-size row/card. Explicit scroll long
         * mode prevents LVGL's default wrapping while preserving the full
         * LIST_ROW_HEIGHT background and touch target. */
        row_label_enable_marquee(row);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(row, row_x, COMPACT_LIST_TOP_PAD + slot * data->row_stride);
        lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN); /* shown by the initial window update below once it has real content */
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        data->rows[slot] = row;
        data->row_logical_index[slot] = -1;

        lv_obj_t * leading = lv_image_create(row);
        lv_obj_align(leading, LV_ALIGN_LEFT_MID, 14, 0);
        lv_obj_remove_flag(leading, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(leading, LV_OBJ_FLAG_HIDDEN);
        data->leading_images[slot] = leading;

        lv_obj_t * trailing = lv_image_create(row);
        lv_obj_align(trailing, LV_ALIGN_RIGHT_MID, -14, 0);
        lv_obj_remove_flag(trailing, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(trailing, LV_OBJ_FLAG_HIDDEN);
        data->trailing_images[slot] = trailing;

        compact_list_row_ctx_t * ctx = malloc(sizeof(*ctx));
        ctx->data = data;
        ctx->pool_slot = slot;
        ctx->logical_index = -1;
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
    int32_t total_height = COMPACT_LIST_TOP_PAD + item_count * data->row_stride;
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
        lv_obj_set_size(bar, COMPACT_LIST_NOW_PLAYING_BAR_WIDTH, data->row_height);
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

    /* Paged mode only ever needs this once a background fetch is actually
     * in flight (compact_list_ensure_cache() resumes it) -- created paused
     * so it costs nothing for the lifetime of an eager-mode list, or a
     * paged list between fetches. */
    data->poll_timer = lv_timer_create(compact_list_poll_fetch_cb, COMPACT_LIST_FETCH_POLL_MS, list);
    lv_timer_pause(data->poll_timer);

    compact_list_update_window(list, data); /* populate the initially-visible rows */

    return list;
}

void compact_list_set_row_decorator(lv_obj_t * list, compact_list_row_decorator_cb_t cb, void * ctx) {
    compact_list_virtual_data_t * data = (compact_list_virtual_data_t *) lv_obj_get_user_data(list);
    data->row_decorator = cb;
    data->row_decorator_ctx = ctx;
    compact_list_refresh_visible(list);
}

void compact_list_refresh_visible(lv_obj_t * list) {
    compact_list_virtual_data_t * data = (compact_list_virtual_data_t *) lv_obj_get_user_data(list);
    data->window_start = -1;
    compact_list_update_window(list, data);
}

void compact_list_refresh_item(lv_obj_t * list, int logical_index) {
    compact_list_virtual_data_t * data = (compact_list_virtual_data_t *) lv_obj_get_user_data(list);
    if (!data || logical_index < 0 || logical_index >= data->item_count) return;
    int slot = logical_index % COMPACT_LIST_POOL_SIZE;
    if (data->row_logical_index[slot] != logical_index) return; /* no longer visible */
    data->refresh_index = logical_index;
    compact_list_update_window(list, data);
}

void compact_list_set_row_height(lv_obj_t * list, int32_t row_height) {
    compact_list_virtual_data_t * data = (compact_list_virtual_data_t *) lv_obj_get_user_data(list);
    if (!data || row_height < 48 || row_height == data->row_height) return;

    data->row_height = row_height;
    data->row_stride = row_height + 4;
    int32_t text_pad_top = (row_height - lv_font_get_line_height(&LIST_ROW_FONT)) / 2;
    if (text_pad_top < 0) text_pad_top = 0;
    for (int slot = 0; slot < COMPACT_LIST_POOL_SIZE; slot++) {
        lv_obj_set_style_height(data->rows[slot], row_height, LV_PART_MAIN);
        lv_obj_set_style_pad_top(data->rows[slot], text_pad_top, LV_PART_MAIN);
        lv_obj_set_y(data->rows[slot], COMPACT_LIST_TOP_PAD + slot * data->row_stride);
    }
    if (data->now_playing_bar) lv_obj_set_height(data->now_playing_bar, row_height);
    int32_t total_height = COMPACT_LIST_TOP_PAD + data->item_count * data->row_stride;
    lv_obj_set_pos(data->spacer, 0, total_height > 0 ? total_height - 1 : 0);
    if (data->now_playing_bar && data->now_playing_index >= 0)
        lv_obj_set_y(data->now_playing_bar,
                     COMPACT_LIST_TOP_PAD + data->now_playing_index * data->row_stride);
    data->window_start = -1;
    compact_list_update_window(list, data);
}

lv_obj_t * build_compact_list_screen(const char * title, lv_event_cb_t back_btn_cb,
                                      const compact_list_item_t * items, int item_count,
                                      compact_list_click_cb_t on_click, compact_list_click_cb_t on_long_press,
                                      lv_obj_t ** out_list, lv_obj_t ** out_title_label, int32_t row_width,
                                      bool enable_now_playing, lv_color_t now_playing_color) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &style_theme_screen_bg, 0);

    if (back_btn_cb) build_back_button(scr, back_btn_cb);
    lv_obj_t * title_label = build_title(scr, title);
    if (out_title_label) *out_title_label = title_label;

    lv_obj_t * list = build_compact_list_widget(scr, items, item_count, on_click, on_long_press, row_width,
                                                 enable_now_playing, now_playing_color);

    if (out_list) *out_list = list;
    return scr;
}

lv_obj_t * add_pill_row_base(lv_obj_t * parent, const char * label_text) {
    lv_obj_t * row = lv_obj_create(parent);
    int32_t row_width = pill_row_default_width();
    lv_obj_set_size(row, row_width, 124);
    lv_obj_add_style(row, &style_theme_screen_bg, 0);
    if (row_width == 448) {
        lv_obj_set_style_bg_image_src(row, asset_path("touch_list/item_bg.png"), 0);
    } else {
        lv_obj_set_style_radius(row, LIST_ROW_RADIUS, 0);
        lv_obj_set_style_bg_color(row, LIST_ROW_BG_COLOR, 0);
    }
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * label = lv_label_create(row);
    lv_label_set_text(label, label_text);
    lv_obj_add_style(label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(label, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 24, 0);
    configure_scrolling_row_label(label, row_width - 136);
    return row;
}

lv_obj_t * add_pill_toggle_row(lv_obj_t * parent, const char * label_text, bool checked, lv_event_cb_t on_click) {
    lv_obj_t * row = add_pill_row_base(parent, label_text);

    lv_obj_t * toggle_img = lv_image_create(row);
    lv_image_set_src(toggle_img, asset_path(checked ? "settings/on.png" : "settings/off.png"));
    lv_obj_align(toggle_img, LV_ALIGN_RIGHT_MID, -20, 0);
    if (checked) {
        lv_obj_set_style_image_recolor(toggle_img, accent_lv_color(), 0);
        lv_obj_set_style_image_recolor_opa(toggle_img, LV_OPA_80, 0);
    }

    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    if (on_click) lv_obj_add_event_cb(row, on_click, LV_EVENT_CLICKED, NULL);
    return row;
}

lv_obj_t * add_pill_chevron_row(lv_obj_t * parent, const char * label_text, lv_event_cb_t on_click) {
    lv_obj_t * row = add_pill_row_base(parent, label_text);

    lv_obj_t * chevron = lv_label_create(row);
    lv_label_set_text(chevron, ">");
    lv_obj_add_style(chevron, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(chevron, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_align(chevron, LV_ALIGN_RIGHT_MID, -20, 0);

    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    if (on_click) lv_obj_add_event_cb(row, on_click, LV_EVENT_CLICKED, NULL);
    return row;
}

lv_obj_t * add_section_header(lv_obj_t * parent, const char * text) {
    lv_obj_t * label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_add_style(label, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(label, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_set_style_pad_top(label, 12, 0);
    lv_obj_set_style_pad_left(label, 24, 0);
    return label;
}
