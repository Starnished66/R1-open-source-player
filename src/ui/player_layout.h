#ifndef PLAYER_LAYOUT_H
#define PLAYER_LAYOUT_H

#include <stdbool.h>
#include <stdint.h>

/* Native-side config for plugin.set_player_layout() (PLUGINS.md), shared
 * between plugin_manager.c (parses/validates the Lua table into this) and
 * gui_player.c (the only reader). No LVGL dependency, same reasoning
 * home_layout.h and remote_track.h stay LVGL-free -- kept as a plain struct
 * passed across the plugin_manager.c/gui.c-family boundary.
 *
 * configured==false (plugin.set_player_layout() never called this boot, or
 * reset on deinit) means every other field here is meaningless -- gui_player.c
 * builds exactly its old hardcoded frosted-glass reflection layout in that
 * case, unchanged.
 *
 * This struct is NEVER written to disk anywhere -- it only ever holds
 * whatever the most recent plugin.set_player_layout() call in THIS process
 * passed in. Restarting discards it, so a plugin that wants persistence must
 * save its choice and reapply it from top-level script code on boot. */
typedef struct {
    bool configured;

    /* If true, skips cover-art reflection blurring entirely and renders
     * player_overlay_panel with a flat color fill (bg_color if has_bg_color
     * is set, otherwise falling back to style_theme_screen_bg / black). */
    bool flat;

    /* Flat background color (0xRRGGBB). Ignored if flat is false. */
    bool has_bg_color;
    uint32_t bg_color;

    /* Blur radius for the frosted glass background (default: 32).
     * Clamped at point of use in gui_player.c to 0..64. */
    bool has_blur_radius;
    int32_t blur_radius;

    /* Number of blur passes for horizontal and vertical separable blur
     * (default: 5). Clamped at point of use in gui_player.c to 0..16. */
    bool has_blur_passes;
    int32_t blur_passes;

    /* Darken multiplier fraction (num / den) applied to blurred RGB channels
     * (default: 1/2). Both must be specified together in Lua.
     * darken_num is clamped to 0..64 and darken_den is clamped to 1..64. */
    bool has_darken;
    int32_t darken_num;
    int32_t darken_den;
} player_layout_config_t;

extern player_layout_config_t player_layout_config;

#endif /* PLAYER_LAYOUT_H */
