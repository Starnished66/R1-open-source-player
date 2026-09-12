#include "frosted_glass.h"

/* 1D box blur via a sliding running sum -- O(length) regardless of radius,
 * rather than O(length * radius) from re-summing the whole window at every
 * pixel. `stride` is measured in elements, not bytes, so the same function
 * serves both passes of separable 2D blurs: 1 for a horizontal pass along
 * a contiguous row, `width` for a vertical pass down a column. Edges are
 * clamped (repeats the edge pixel) rather than wrapping or zero-padding,
 * so the blur doesn't darken/lighten near the image boundary. */
void box_blur_1d(const uint8_t * src, uint8_t * dst, int length, int stride, int radius) {
    if (length <= 0) return;
    int window = radius * 2 + 1;
    int sum = 0;
    for (int i = -radius; i <= radius; i++) {
        int idx = i < 0 ? 0 : (i >= length ? length - 1 : i);
        sum += src[idx * stride];
    }
    for (int i = 0; i < length; i++) {
        dst[i * stride] = (uint8_t) (sum / window);
        int add_idx = i + radius + 1;
        if (add_idx >= length) add_idx = length - 1;
        int rem_idx = i - radius;
        if (rem_idx < 0) rem_idx = 0;
        sum += src[add_idx * stride] - src[rem_idx * stride];
    }
}

/* RGB565 has only 32 red/blue and 64 green levels, so a strong blur exposes
 * broad contour bands even though all filtering above is done in 8-bit
 * planes. Ordered 8x8 dithering trades those coherent bands for a tiny,
 * stable sub-pixel texture. Stable (not random) matters because a changing
 * noise field would shimmer on every redraw. */
uint16_t rgb888_to_565_dithered(int r, int g, int b, int x, int y) {
    static const uint8_t bayer8[8][8] = {
        { 0,48,12,60, 3,51,15,63 }, { 32,16,44,28,35,19,47,31 },
        { 8,56, 4,52,11,59, 7,55 }, { 40,24,36,20,43,27,39,23 },
        { 2,50,14,62, 1,49,13,61 }, { 34,18,46,30,33,17,45,29 },
        { 10,58, 6,54, 9,57, 5,53 }, { 42,26,38,22,41,25,37,21 }
    };
    int threshold = bayer8[y & 7][x & 7];
    r += threshold / 8 - 4;
    g += threshold / 16 - 2;
    b += threshold / 8 - 4;
    if (r < 0) r = 0; else if (r > 255) r = 255;
    if (g < 0) g = 0; else if (g > 255) g = 255;
    if (b < 0) b = 0; else if (b > 255) b = 255;
    return (uint16_t) (((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}
