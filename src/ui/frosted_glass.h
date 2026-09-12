#ifndef FROSTED_GLASS_H
#define FROSTED_GLASS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Shared pixel-math primitives used as building blocks for both the player screen's
 * frosted glass reflection panel and the lyrics screen's fullscreen backdrop blur effects. */

void box_blur_1d(const uint8_t * src, uint8_t * dst, int length, int stride, int radius);

uint16_t rgb888_to_565_dithered(int r, int g, int b, int x, int y);

#ifdef __cplusplus
}
#endif

#endif /* FROSTED_GLASS_H */
