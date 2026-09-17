#ifndef FROSTED_GLASS_H
#define FROSTED_GLASS_H

#include <stdint.h>
#include <lvgl/lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Shared pixel-math primitives used as building blocks for both the player screen's
 * frosted glass reflection panel and the lyrics screen's fullscreen backdrop blur effects. */

void box_blur_1d(const uint8_t * src, uint8_t * dst, int length, int stride, int radius);

uint16_t rgb888_to_565_dithered(int r, int g, int b, int x, int y);
/* Non-periodic, coordinate-stable variant for full-screen frosted artwork. */
uint16_t rgb888_to_565_spatial_dithered(int r, int g, int b, int x, int y);

/* Full pipeline: snapshot source_screen, downsample into a work_width x
 * work_height buffer, run a separable box blur (blur_radius, blur_passes)
 * per RGB channel, then bilinear-upscale to out_width x out_height,
 * darkening by darken_num/darken_den and spatially dithering into RGB565 at
 * each destination pixel. Used for live "whatever's behind this popup"
 * backdrops (power action overlay, quick drawer) as opposed to the player/
 * lyrics screens' own album-art blur, which have their own orchestration
 * (different source, different configurability needs -- see their own
 * comments). Synchronous and one-shot; caller owns the returned
 * out_width * out_height * 2 byte buffer (free() it), NULL on a failed
 * snapshot or allocation. */
uint8_t * frosted_glass_blur_screen_rgb565(lv_obj_t * source_screen,
                                           int work_width, int work_height,
                                           int blur_radius, int blur_passes,
                                           int darken_num, int darken_den,
                                           int out_width, int out_height);

#ifdef __cplusplus
}
#endif

#endif /* FROSTED_GLASS_H */
