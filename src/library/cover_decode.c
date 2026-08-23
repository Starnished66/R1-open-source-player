#include "cover_decode.h"

#include "lvgl/src/libs/tjpgd/tjpgd.h"
#include "lvgl/src/libs/lodepng/lodepng.h"

#include <stdlib.h>
#include <string.h>
#include <limits.h>

/* Covers are immediately reduced to a small UI bitmap. Never let malformed
 * or unnecessarily huge source dimensions consume most of this device's RAM
 * merely to produce that thumbnail/player image. */
#define MAX_NATIVE_DECODE_BYTES (8U * 1024U * 1024U)

static bool rgb888_size_ok(size_t width, size_t height, size_t * out_bytes) {
    if (width == 0 || height == 0 || width > INT_MAX || height > INT_MAX) return false;
    if (width > SIZE_MAX / height || width * height > SIZE_MAX / 3U) return false;
    size_t bytes = width * height * 3U;
    if (bytes > MAX_NATIVE_DECODE_BYTES) return false;
    if (out_bytes) *out_bytes = bytes;
    return true;
}

/* LVGL's zoom/stretch transform only applies to fully-decoded ARGB8888
 * buffers; its tjpgd binding decodes JPEGs lazily, tile by tile, straight
 * into the draw pipeline, so the transform never touches JPEG album art.
 * This file decodes JPEG/PNG cover art itself and resizes it into an exact
 * target-sized RGB565 buffer before LVGL ever sees it, avoiding that gap. */

typedef struct {
    const uint8_t * data;
    uint32_t size;
    uint32_t pos;
    uint8_t * out_buf; /* RGB888, one row per decoded (possibly tjpgd-scaled) pixel */
    int out_w;
    int out_h;
} jpeg_ctx_t;

static size_t jpeg_mem_read(JDEC * jd, uint8_t * buff, size_t ndata) {
    jpeg_ctx_t * ctx = (jpeg_ctx_t *) jd->device;
    if (ctx->pos >= ctx->size) return 0;

    size_t avail = ctx->size - ctx->pos;
    size_t n = ndata < avail ? ndata : avail;
    if (buff) memcpy(buff, ctx->data + ctx->pos, n);
    ctx->pos += (uint32_t) n;
    return n;
}

static int jpeg_mem_output(JDEC * jd, void * bitmap, JRECT * rect) {
    jpeg_ctx_t * ctx = (jpeg_ctx_t *) jd->device;
    int w = rect->right - rect->left + 1;
    int h = rect->bottom - rect->top + 1;
    const uint8_t * src = (const uint8_t *) bitmap;

    for (int y = 0; y < h; y++) {
        int dy = rect->top + y;
        if (dy < 0 || dy >= ctx->out_h) continue;

        int copy_w = w;
        if (rect->left + copy_w > ctx->out_w) copy_w = ctx->out_w - rect->left;
        if (copy_w <= 0) continue;

        uint8_t * dst_row = ctx->out_buf + (size_t) dy * ctx->out_w * 3 + (size_t) rect->left * 3;
        const uint8_t * src_row = src + (size_t) y * w * 3;
        /* tjpgd's YCbCr-to-RGB conversion writes its three output bytes per
         * pixel in B, G, R order, not R, G, B; swap per-pixel here instead of
         * a straight memcpy to correct it. lodepng's PNG path already
         * returns standard R, G, B and needs no such swap. */
        for (int x = 0; x < copy_w; x++) {
            dst_row[x * 3 + 0] = src_row[x * 3 + 2]; /* R <- src B */
            dst_row[x * 3 + 1] = src_row[x * 3 + 1]; /* G <- src G */
            dst_row[x * 3 + 2] = src_row[x * 3 + 0]; /* B <- src R */
        }
    }
    return 1;
}

static bool decode_jpeg_rgb888(const uint8_t * data, uint32_t size,
                                uint8_t ** out_buf, int * out_w, int * out_h) {
    uint8_t workbuf[4096];
    JDEC jd;
    jpeg_ctx_t ctx = { .data = data, .size = size, .pos = 0, .out_buf = NULL, .out_w = 0, .out_h = 0 };

    if (jd_prepare(&jd, jpeg_mem_read, workbuf, sizeof(workbuf), &ctx) != JDR_OK) return false;

    /* tjpgd's built-in scale (0=1/1 .. 3=1/8) is used only as a memory-safety
     * fallback for pathologically large embedded art, not for its normal
     * purpose of shrinking the decode buffer: scale>0 output is corrupted in
     * this vendored build (the IDCT always produces full unscaled 8x8
     * blocks, but the RGB conversion loop is told a smaller, scale-reduced
     * rect for interior MCUs, so it reads pixel data at the wrong stride).
     * Always decode at native resolution and let resize_cover_fit()'s own
     * box filter do the downscaling, except above MAX_NATIVE_DECODE_BYTES,
     * where the native decode buffer itself would risk exhausting this
     * device's limited RAM -- only then fall back to tjpgd's scale,
     * accepting its known corruption as the lesser risk vs an OOM crash. */
    uint8_t scale = 0;
    size_t decoded_w = jd.width;
    size_t decoded_h = jd.height;
    size_t native_bytes = 0;
    while (scale < 3 && !rgb888_size_ok(decoded_w, decoded_h, &native_bytes)) {
        scale++;
        decoded_w = jd.width >> scale;
        decoded_h = jd.height >> scale;
    }

    if (!rgb888_size_ok(decoded_w, decoded_h, &native_bytes)) return false;

    uint8_t * buf = calloc(1, native_bytes);
    if (!buf) return false;

    ctx.out_buf = buf;
    ctx.out_w = (int) decoded_w;
    ctx.out_h = (int) decoded_h;

    if (jd_decomp(&jd, jpeg_mem_output, scale) != JDR_OK) {
        free(buf);
        return false;
    }

    *out_buf = buf;
    *out_w = (int) decoded_w;
    *out_h = (int) decoded_h;
    return true;
}

static bool decode_png_rgb888(const uint8_t * data, uint32_t size, uint8_t ** out_buf, int * out_w, int * out_h) {
    unsigned w = 0, h = 0;
    LodePNGState state;
    lodepng_state_init(&state);
    unsigned inspect_error = lodepng_inspect(&w, &h, &state, data, size);
    lodepng_state_cleanup(&state);
    if (inspect_error != 0 || !rgb888_size_ok(w, h, NULL)) return false;

    unsigned char * pixels = NULL;
    if (lodepng_decode24(&pixels, &w, &h, data, size) != 0 || !pixels) return false;

    *out_buf = pixels;
    *out_w = (int) w;
    *out_h = (int) h;
    return true;
}

static inline uint16_t rgb888_to_565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t) (((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

/* Bilinear sample at fractional source coordinates (fx, fy), clamped to the
 * source bounds -- for the upscale path below. Interpolates between the 4
 * nearest source pixels rather than picking one, the standard fix for
 * magnification (box-filter's own footprint degrades to a single pixel once
 * it's narrower than one source pixel, which is exactly the upscale case). */
static void bilinear_sample(const uint8_t * src, int src_w, int src_h, float fx, float fy,
                             uint8_t * out_r, uint8_t * out_g, uint8_t * out_b) {
    if (fx < 0) fx = 0;
    if (fy < 0) fy = 0;
    if (fx > src_w - 1) fx = (float) (src_w - 1);
    if (fy > src_h - 1) fy = (float) (src_h - 1);

    int x0 = (int) fx, y0 = (int) fy;
    int x1 = x0 + 1 < src_w ? x0 + 1 : x0;
    int y1 = y0 + 1 < src_h ? y0 + 1 : y0;
    float tx = fx - x0, ty = fy - y0;

    const uint8_t * p00 = src + ((size_t) y0 * src_w + x0) * 3;
    const uint8_t * p10 = src + ((size_t) y0 * src_w + x1) * 3;
    const uint8_t * p01 = src + ((size_t) y1 * src_w + x0) * 3;
    const uint8_t * p11 = src + ((size_t) y1 * src_w + x1) * 3;

    for (int c = 0; c < 3; c++) {
        float top = p00[c] * (1.0f - tx) + p10[c] * tx;
        float bot = p01[c] * (1.0f - tx) + p11[c] * tx;
        float v = top * (1.0f - ty) + bot * ty;
        uint8_t out = (uint8_t) (v + 0.5f);
        if (c == 0) *out_r = out; else if (c == 1) *out_g = out; else *out_b = out;
    }
}

/* "Cover fit": scale so the source fully covers the target box (the larger
 * of the two axis scale ratios), then center-crop whichever dimension
 * overflows -- the same behavior as a photo app's cover/thumbnail mode.
 *
 * Downscaling uses a box filter (area-average of every source pixel whose
 * center falls in the destination pixel's inverse-mapped footprint) rather
 * than nearest-neighbor, since embedded art is usually far larger than the
 * on-screen art box. Upscaling uses bilinear interpolation instead, since a
 * box filter's footprint degrades to nearest-neighbor once it's narrower
 * than one source pixel -- the common case, since most embedded art is
 * smaller than this app's on-screen art box. */
static uint16_t * resize_cover_fit(const uint8_t * src, int src_w, int src_h, int dst_w, int dst_h) {
    uint16_t * dst = malloc((size_t) dst_w * dst_h * sizeof(uint16_t));
    if (!dst) return NULL;

    float scale_w = (float) dst_w / (float) src_w;
    float scale_h = (float) dst_h / (float) src_h;
    float scale = scale_w > scale_h ? scale_w : scale_h;
    bool upscaling = scale > 1.0f;

    float scaled_w = src_w * scale;
    float scaled_h = src_h * scale;
    float crop_x = (scaled_w - dst_w) / 2.0f;
    float crop_y = (scaled_h - dst_h) / 2.0f;

    for (int dy = 0; dy < dst_h; dy++) {
        uint16_t * dst_row = dst + (size_t) dy * dst_w;

        if (upscaling) {
            /* Sample at the destination pixel's center, mapped back into
             * source space -- the standard convention (+0.5 forward,
             * -0.5 back) that keeps the interpolation aligned on both axes
             * instead of shifted by half a source pixel. */
            float fy = ((dy + 0.5f) + crop_y) / scale - 0.5f;
            for (int dx = 0; dx < dst_w; dx++) {
                float fx = ((dx + 0.5f) + crop_x) / scale - 0.5f;
                uint8_t r, g, b;
                bilinear_sample(src, src_w, src_h, fx, fy, &r, &g, &b);
                dst_row[dx] = rgb888_to_565(r, g, b);
            }
            continue;
        }

        int sy0 = (int) ((dy + crop_y) / scale);
        int sy1 = (int) ((dy + 1 + crop_y) / scale);
        if (sy0 < 0) sy0 = 0;
        if (sy0 >= src_h) sy0 = src_h - 1;
        if (sy1 >= src_h) sy1 = src_h - 1;
        if (sy1 < sy0) sy1 = sy0;

        for (int dx = 0; dx < dst_w; dx++) {
            int sx0 = (int) ((dx + crop_x) / scale);
            int sx1 = (int) ((dx + 1 + crop_x) / scale);
            if (sx0 < 0) sx0 = 0;
            if (sx0 >= src_w) sx0 = src_w - 1;
            if (sx1 >= src_w) sx1 = src_w - 1;
            if (sx1 < sx0) sx1 = sx0;

            uint32_t r_sum = 0, g_sum = 0, b_sum = 0, count = 0;
            for (int sy = sy0; sy <= sy1; sy++) {
                const uint8_t * row = src + (size_t) sy * src_w * 3;
                for (int sx = sx0; sx <= sx1; sx++) {
                    const uint8_t * p = row + (size_t) sx * 3;
                    r_sum += p[0];
                    g_sum += p[1];
                    b_sum += p[2];
                    count++;
                }
            }
            dst_row[dx] = rgb888_to_565((uint8_t) (r_sum / count), (uint8_t) (g_sum / count), (uint8_t) (b_sum / count));
        }
    }
    return dst;
}

bool cover_decode_to_rgb565(const uint8_t * data, uint32_t size, int target_w, int target_h, uint16_t ** out_pixels) {
    if (!data || size < 8 || target_w <= 0 || target_h <= 0) return false;

    uint8_t * native_buf = NULL;
    int native_w = 0, native_h = 0;
    bool ok;

    if (data[0] == 0xFF && data[1] == 0xD8) {
        ok = decode_jpeg_rgb888(data, size, &native_buf, &native_w, &native_h);
    } else if (data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G') {
        ok = decode_png_rgb888(data, size, &native_buf, &native_w, &native_h);
    } else {
        return false;
    }

    if (!ok || !native_buf) return false;
    if (native_w <= 0 || native_h <= 0) {
        free(native_buf);
        return false;
    }

    uint16_t * resized = resize_cover_fit(native_buf, native_w, native_h, target_w, target_h);
    free(native_buf);
    if (!resized) return false;

    *out_pixels = resized;
    return true;
}
