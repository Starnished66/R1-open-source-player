#ifndef ASSETS_H
#define ASSETS_H

#include "lvgl.h"

/* Real UI assets, straight from the stock HiBy firmware's own theme2 (dark)
 * resource pack -- not ours to redistribute, so nothing is vendored into
 * this repo. On target we just point at the firmware's own copy, already
 * present on every real R1. On host, the Makefile mirrors a local dev copy
 * into ./assets/theme2 (best-effort, gitignored) purely so the simulator
 * has something to show. */

/* Registers the PNG decoder and POSIX filesystem driver LVGL needs to load
 * images by path. Call once, before any screen references an asset. */
void assets_init(void);

/* Resolves a path like "launcher/music.png" to a full LVGL image source
 * string (drive letter + theme root + relative path). Returns a freshly
 * heap-allocated string every call, intentionally never freed -- some LVGL
 * setters (lv_obj_set_style_bg_image_src) store the raw pointer rather than
 * copying it, so a reused buffer goes stale as soon as another asset_path()
 * call happens anywhere else in the app. Screens are built once at startup
 * and live for the process's lifetime, so this is a bounded, one-time leak
 * per image reference -- consistent with other per-widget context structs
 * in this codebase that are likewise never freed. */
const char * asset_path(const char * relative_path);

/* Same override-root-then-stock-root resolution as asset_path(), but
 * returns a plain absolute filesystem path with no "S:" LVGL-driver
 * prefix -- for callers that add their own prefix/handling downstream
 * (e.g. pill_row_apply_icon()'s icon_asset contract, screen_builders.c)
 * rather than passing straight into an LVGL image-source setter. Same
 * heap-allocated-and-never-freed tradeoff as asset_path() above. */
const char * asset_path_plain(const char * relative_path);

/* Loads a small PNG into a process-lifetime variable image descriptor.
 * This avoids reopening immutable slider artwork on every redraw. Falls
 * back to NULL if the asset cannot be read. */
const lv_image_dsc_t * asset_png_memory(const char * relative_path);

/* Deletes every file plugin.set_icon() has ever written into
 * THEME_OVERRIDE_ROOT, restoring every icon to its real stock appearance --
 * a plugin's override otherwise persists on disk indefinitely (nothing
 * cleans it up on its own, not on boot and not when the plugin that wrote
 * it is later removed or disabled: asset_path() just keeps finding it and
 * preferring it over the stock asset forever). Scoped to exactly this one
 * directory, unlike settings_factory_reset()'s much broader wipe of
 * /usr/data -- this touches nothing else (settings, PEQ, Bluetooth
 * pairings, ...). A no-op on HOST_BUILD, where THEME_OVERRIDE_ROOT doesn't
 * exist (see asset_path()'s own HOST_BUILD branch). Screens are built once
 * at startup from whatever asset_path() resolved then (see this header's
 * own comment above), so -- same as Factory Reset/Font Size/Hostname --
 * this doesn't try to hot-apply the change; the caller is expected to
 * reboot afterward. */
void assets_reset_theme_overrides(void);

#endif /* ASSETS_H */
