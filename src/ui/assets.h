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

/* Keeps one PNG decoded as an LVGL draw buffer until close. Use for large,
 * immutable artwork that is redrawn on every animation/drag frame; unlike
 * the global image cache, this cannot be evicted by unrelated screen art. */
typedef struct {
    lv_image_decoder_dsc_t decoder;
    char * path;
    bool open;
} asset_decoded_image_t;

bool asset_decoded_image_open(asset_decoded_image_t * image, const char * relative_path);
void asset_decoded_image_close(asset_decoded_image_t * image);
const void * asset_decoded_image_source(const asset_decoded_image_t * image);

#endif /* ASSETS_H */
