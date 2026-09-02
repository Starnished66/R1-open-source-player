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

/* Resolves a path like "launcher/music.png" to a stable interned LVGL image
 * source string (drive letter + theme root + relative path). The caller
 * does not free it. Repeated calls/reloads reuse the same resolved string;
 * stock and override paths may each have one entry for an asset. */
const char * asset_path(const char * relative_path);

/* Same override-root-then-stock-root resolution as asset_path(), but
 * returns a plain absolute filesystem path with no "S:" LVGL-driver
 * prefix -- for callers that add their own prefix/handling downstream
 * (e.g. pill_row_apply_icon()'s icon_asset contract, screen_builders.c)
 * rather than passing straight into an LVGL image-source setter. Returned
 * strings have the same stable interned lifetime as asset_path(). */
const char * asset_path_plain(const char * relative_path);

/* Loads a small PNG into an owned image descriptor. This avoids reopening
 * immutable slider artwork on every redraw. Falls back to NULL if the asset
 * cannot be read; release a successful result with asset_png_memory_free()
 * after every LVGL object referencing it has been deleted. */
const lv_image_dsc_t * asset_png_memory(const char * relative_path);
void asset_png_memory_free(const lv_image_dsc_t * image);

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
