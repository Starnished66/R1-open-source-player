#ifndef BOOTLOADER_FB_DRAW_H
#define BOOTLOADER_FB_DRAW_H

#include <stdbool.h>
#include <stdint.h>

#include "board_config.h"

/* Display resolution from board_config.h.
 * HiBy R1 is 480x800 RGB565; R3 Pro II is 480x720 RGB565.
 * fb_open() verifies the runtime framebuffer geometry against these dimensions. */
#define FB_WIDTH BOARD_SCREEN_WIDTH
#define FB_HEIGHT BOARD_SCREEN_HEIGHT

typedef uint16_t fb_color_t;

/* Opens /dev/fb0, mmaps it, and verifies its geometry/format actually
 * match FB_WIDTH/FB_HEIGHT/RGB565 via FBIOGET_VSCREENINFO -- a mismatch is
 * treated as fatal (returns false) rather than drawing garbage into a
 * differently-shaped or differently-packed buffer. Mirrors src/main.c's
 * own wait_for_fbdev_ready() retry shape (the device node can exist before
 * the underlying driver has actually settled). */
bool fb_open(void);

/* Unmaps and closes /dev/fb0. Must be called before execve()'ing into a
 * player -- see main.c's own doc comment on why this matters even though
 * execve() itself would drop the mapping regardless. */
void fb_close(void);

fb_color_t fb_rgb(uint8_t r, uint8_t g, uint8_t b);

void fb_fill(fb_color_t color);
void fb_fill_rect(int x, int y, int w, int h, fb_color_t color);

/* Alpha-blends color over whatever is already drawn there (e.g. the
 * background image) instead of overwriting it outright -- 0 leaves the
 * existing pixel untouched, 255 behaves exactly like fb_fill_rect(). For a
 * card whose background should read as "semi-transparent over the
 * artwork", not opaque. */
void fb_fill_rect_alpha(int x, int y, int w, int h, fb_color_t color, uint8_t alpha);

void fb_draw_rect_border(int x, int y, int w, int h, int thickness, fb_color_t color);

/* Uppercase letters, digits, space, and ':' only -- see fb_draw.c's own
 * font table doc comment for exactly why the character set is this
 * narrow. Any other byte is drawn as a blank cell rather than skipped, so
 * a typo in a caller's string is visible (a gap) instead of silently
 * shifting all following characters left. */
void fb_draw_text(int x, int y, const char * text, fb_color_t color);
int fb_text_width(const char * text);
int fb_text_height(void);

/* Copies the off-screen back buffer to the visible framebuffer.
 * Call after a complete frame has been drawn. */
void fb_flush(void);

/* Decodes a JPEG file straight into the off-screen drawing buffer at
 * (0,0), pixel for pixel, via tjpgd (already vendored for the main
 * player's own cover-art decode, lvgl/src/libs/tjpgd/ -- fully
 * standalone, no other LVGL dependency). Deliberately does NOT scale or
 * crop: this only exists to
 * draw /etc/logo1.jpeg, confirmed on-device to be exactly FB_WIDTH x
 * FB_HEIGHT already (this device's own stock boot splash asset) -- a
 * general-purpose image-fit pipeline is exactly the kind of scope this
 * bootloader's own design deliberately avoids pulling in. Returns false
 * (caller should fall back to a plain fb_fill()) if the file can't be
 * opened, isn't a JPEG tjpgd can parse, or its dimensions don't match the
 * screen exactly -- never scales/crops to fit as a fallback.
 *
 * On success, also caches the decoded frame (a plain heap copy, no
 * padding) so fb_restore_background() below can redraw it cheaply --
 * call this once, not on every menu redraw tick, or every tick pays a
 * full JPEG decode for no reason. */
bool fb_draw_background_jpeg(const char * path);

/* Fast per-frame redraw: blits the background fb_draw_background_jpeg()
 * cached, or fills with fallback_color if that was never called or
 * failed. This is what every subsequent draw_menu()-style redraw should
 * call instead of fb_fill() directly, so a countdown ticking every
 * ~100ms doesn't re-decode a JPEG that many times a second. */
void fb_restore_background(fb_color_t fallback_color);

#endif /* BOOTLOADER_FB_DRAW_H */
