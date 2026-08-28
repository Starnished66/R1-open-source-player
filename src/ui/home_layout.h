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
 * Home has exactly 6 tiles and no room for plugin-added ones (PLUGINS.md);
 * this only restyles the fixed six, never adds/removes a tile. Order here
 * matches build_home_screen()'s own native tile order, published as
 * home_layout_tile_keys[] (defined in gui_settings.c, the file that owns
 * that order) so plugin_manager.c can validate a Lua `key` string against
 * it without a second, potentially-drifting copy of the list. */
#define HOME_LAYOUT_TILE_COUNT 6
extern const char * const home_layout_tile_keys[HOME_LAYOUT_TILE_COUNT];

/* configured==false (plugin.set_home_layout() never called this boot) means
 * every other field here is meaningless -- build_home_screen() builds
 * exactly its old hardcoded layout in that case, unchanged. Only takes
 * effect on the NEXT build_home_screen() call, i.e. the next boot -- Home is
 * built once at startup and never rebuilt (same "load-time only" constraint
 * plugin.set_icon() already documents), so a call made after boot from
 * inside a callback just updates this struct for next time, silently, with
 * no error and no immediate visible effect. */
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
    char text_size[8];  /* "", "small", "medium", "large" */
    bool has_icon;      bool icon;
} home_tile_override_t;

typedef struct {
    bool configured;
    bool list_mode;   /* false = tile mode (today's icon grid), true = pill-list rows */
    int32_t tile_gap; /* tile mode only */
    int32_t row_gap;  /* list mode only, 0 = keep build_pill_list_screen()'s native default of 6 */
    home_tile_override_t tiles[HOME_LAYOUT_TILE_COUNT];
} home_layout_config_t;

/* The live config -- defined in gui_plugins.c (written only by
 * gui_plugin_set_home_layout()), read directly by gui_settings.c's
 * build_home_screen(). Same "extern global declared in the shared header,
 * defined in the file that owns writing it" pattern as screen_builders.h's
 * own list_row_style/style_theme_* globals. */
extern home_layout_config_t home_layout_config;

#endif /* HOME_LAYOUT_H */
