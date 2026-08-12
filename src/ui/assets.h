#ifndef ASSETS_H
#define ASSETS_H

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

#endif /* ASSETS_H */
