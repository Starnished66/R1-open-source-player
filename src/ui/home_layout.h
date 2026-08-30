#ifndef HOME_LAYOUT_H
#define HOME_LAYOUT_H

#include <stdbool.h>
#include <stdint.h>

/* Native-side config for plugin.set_home_layout() (PLUGINS.md), shared
 * between plugin_manager.c (parses/validates the Lua table into this) and
 * gui_settings.c's build_home_screen() (the only reader). No LVGL
 * dependency, same reasoning remote_track.h stays LVGL-free -- kept as a
 * plain struct passed across the plugin_manager.c/gui.c-family boundary,
 * like remote_track_meta_t is for plugin.play_remote().
 *
 * Home has 7 addressable native tiles, published as home_layout_tile_keys[]
 * (defined in gui_settings.c, the file that owns their native order) so
 * plugin_manager.c
 * can validate a Lua `key` string against it without a second, potentially-
 * drifting copy of the list. Since API 7, a plugin can also register its own
 * tile (plugin.register_home_tile(), PLUGINS.md) and a theme can reorder,
 * drop, or interleave any of these via set_home_layout()'s `options.order`
 * -- see home_layout_config_t's own comment below. */
#define HOME_LAYOUT_TILE_COUNT 7
#define HOME_LAYOUT_DEFAULT_TILE_COUNT 6
extern const char * const home_layout_tile_keys[HOME_LAYOUT_TILE_COUNT];

/* Upper bound on how many tiles Home can display at once (native + plugin-
 * registered, combined) -- sizes home_layout_config_t's order[]/tiles[]
 * below and gui_settings.c's build_home_screen() static item arrays.
 *
 * This is a HARD structural ceiling, not a generous round number:
 * build_icon_grid_screen() (screen_builders.c) allocates its row_dsc array
 * at exactly ICON_GRID_MAX_ROWS+1 (6+1) entries and its tile-placement loop
 * computes row = i / 2 with NO bounds check against item_count -- exceeding
 * ICON_GRID_MAX_ROWS*2 tiles in tile mode without also resizing that row_dsc
 * array corrupts adjacent grid state, not just overflows visually (see
 * screen_builders.c's own row_dsc/col_dsc comment for a prior bug in this
 * exact class). Never raise this constant without also updating
 * build_icon_grid_screen(). List mode (build_pill_list_screen()) has no such
 * ceiling -- it genuinely scrolls -- but shares this same bound for
 * simplicity; plugin_manager.c's l_plugin_set_home_layout() separately
 * rejects a tile-mode config whose order exceeds 6 items (build_icon_grid_
 * screen() cannot scroll at all), well below this array-sizing limit. */
#define HOME_LAYOUT_MAX_TILES 12

/* configured==false (plugin.set_home_layout() never called this boot) means
 * every other field here is meaningless -- build_home_screen() builds
 * exactly its old hardcoded layout in that case, unchanged.
 *
 * This struct is NEVER written to disk anywhere -- it only ever holds
 * whatever the most recent plugin.set_home_layout() call in THIS process
 * passed in. build_home_screen() reads it at startup and whenever
 * plugin.refresh_theme() replaces Home. Restarting still discards it, so a
 * plugin that wants persistence must save its choice and reapply it from
 * top-level script code on every boot -- see PLUGINS.md and
 * plugins_examples/Themes.lua. */
typedef struct {
    bool has_bg_color;   uint32_t bg_color;
    bool has_text_color; uint32_t text_color;
    bool has_radius;     int32_t radius;

    /* List mode only (PLUGINS.md); ignored in tile mode. 0/""/false-with-
     * has_*==false all mean "unset, keep this tile's native default for
     * this field" -- independent per field, so a plugin can override just
     * one tile's color without having to also specify its size. */
    int32_t height;
    int32_t width;
    char align[8];      /* "", "left", "center", "right" */
    bool has_accessory; bool accessory;
    char text_size[8];  /* "", "small", "medium", "large", "mono" */
    bool has_icon;      bool icon;
} home_tile_override_t;

typedef struct {
    bool configured;
    bool list_mode;   /* false = tile mode (today's icon grid), true = pill-list rows */
    int32_t tile_gap; /* tile mode only -- build_icon_grid_screen() clamps this to 0-64 */
    int32_t row_gap;  /* list mode only, 0 = keep build_pill_list_screen()'s native default of 6 -- clamped there to 0-84 */

    /* Display order -- position IS order, unlike tiles[] below. Each entry
     * is either one of the 7 native keys in home_layout_tile_keys[] or a
     * plugin.register_home_tile() id. A native key simply not mentioned
     * here is not shown at all -- this is how a theme drops a tile, not
     * just reorders one. An entry that resolves to neither a native key nor
     * a currently-registered plugin id (e.g. that plugin failed to load) is
     * skipped by build_home_screen(), logged, not a crash.
     *
     * order_count == 0 means "unconfigured" -- build_home_screen() falls
     * back to exactly today's fixed 6-native-tile order, so every existing
     * plugin/theme that never sets this keeps working unchanged. */
    char order[HOME_LAYOUT_MAX_TILES][40];
    int order_count;

    /* Per-tile style overrides -- keyed by name (native key or plugin tile
     * id) via linear scan, NOT by array position or by order[]'s position;
     * a tile can be styled here without appearing in order[] at all (native
     * default order applies), and reordering via order[] doesn't require
     * re-specifying style. */
    struct {
        char key[40];
        home_tile_override_t override;
    } tiles[HOME_LAYOUT_MAX_TILES];
    int tile_count;
} home_layout_config_t;

/* The live config -- defined in gui_plugins.c (written only by
 * gui_plugin_set_home_layout()), read directly by gui_settings.c's
 * build_home_screen(). Same "extern global declared in the shared header,
 * defined in the file that owns writing it" pattern as screen_builders.h's
 * own list_row_style/style_theme_* globals. */
extern home_layout_config_t home_layout_config;

#endif /* HOME_LAYOUT_H */
