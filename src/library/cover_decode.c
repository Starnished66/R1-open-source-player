#include "cover_decode.h"

#include "lvgl/src/libs/tjpgd/tjpgd.h"
#include "lvgl/src/libs/lodepng/lodepng.h"

#include <stdlib.h>
#include <string.h>
#include <limits.h>

#include "artwork_coordinator.h"
#include "debug_log.h"

static bool rgb888_size_ok(size_t width, size_t height, size_t max_side, size_t * out_bytes) {
    if (width == 0 || height == 0 || width > max_side || height > max_side) return false;
    if (width > SIZE_MAX / height || width * height > SIZE_MAX / 3U) return false;
    size_t bytes = width * height * 3U;
    if (bytes > (size_t) max_side * (size_t) max_side * 3U) return false;
    if (out_bytes) *out_bytes = bytes;
    return true;
}

typedef struct {
    const uint8_t * data;
    uint32_t size;
    uint32_t pos;
    uint8_t * out_buf; /* RGB888, one row per native decoded pixel */
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
        for (int x = 0; x < copy_w; x++) {
            dst_row[x * 3 + 0] = src_row[x * 3 + 2]; /* R <- src B */
            dst_row[x * 3 + 1] = src_row[x * 3 + 1]; /* G <- src G */
            dst_row[x * 3 + 2] = src_row[x * 3 + 0]; /* B <- src R */
        }
    }
    return 1;
}

static bool inspect_jpeg(const uint8_t * data, uint32_t size, int * out_w, int * out_h) {
    uint8_t workbuf[4096];
    JDEC jd;
    jpeg_ctx_t ctx = { .data = data, .size = size, .pos = 0, .out_buf = NULL, .out_w = 0, .out_h = 0 };

    if (jd_prepare(&jd, jpeg_mem_read, workbuf, sizeof(workbuf), &ctx) != JDR_OK) return false;
    *out_w = (int) jd.width;
    *out_h = (int) jd.height;
    return true;
}

static cover_decode_result_t decode_jpeg_rgb888(const uint8_t * data, uint32_t size, size_t max_side,
                                                int target_w, int target_h,
                                                uint8_t ** out_buf, int * out_w, int * out_h) {
    uint8_t workbuf[4096];
    JDEC jd;
    jpeg_ctx_t ctx = { .data = data, .size = size, .pos = 0, .out_buf = NULL, .out_w = 0, .out_h = 0 };

    if (jd_prepare(&jd, jpeg_mem_read, workbuf, sizeof(workbuf), &ctx) != JDR_OK) {
        return COVER_DECODE_FAIL_UNSUPPORTED;
    }

    if (!rgb888_size_ok(jd.width, jd.height, max_side, NULL)) {
        return COVER_DECODE_FAIL_OVERSIZED;
    }

    uint8_t scale = jpeg_scale_for_target((int) jd.width, (int) jd.height, target_w, target_h);
    int scaled_w = (int) (jd.width >> scale);
    int scaled_h = (int) (jd.height >> scale);
    size_t scaled_bytes = 0;
    if (!rgb888_size_ok((size_t) scaled_w, (size_t) scaled_h, max_side, &scaled_bytes)) {
        return COVER_DECODE_FAIL_OVERSIZED;
    }

    uint8_t * buf = calloc(1, scaled_bytes);
    if (!buf) {
        return COVER_DECODE_FAIL_ALLOC;
    }

    ctx.out_buf = buf;
    ctx.out_w = scaled_w;
    ctx.out_h = scaled_h;

    if (jd_decomp(&jd, jpeg_mem_output, scale) != JDR_OK) {
        free(buf);
        return COVER_DECODE_FAIL_UNSUPPORTED;
    }

    *out_buf = buf;
    *out_w = scaled_w;
    *out_h = scaled_h;
    return COVER_DECODE_OK;
}

static bool inspect_png(const uint8_t * data, uint32_t size, int * out_w, int * out_h) {
    unsigned w = 0, h = 0;
    LodePNGState state;
    lodepng_state_init(&state);
    unsigned inspect_error = lodepng_inspect(&w, &h, &state, data, size);
    lodepng_state_cleanup(&state);
    if (inspect_error != 0) return false;
    *out_w = (int) w;
    *out_h = (int) h;
    return true;
}

static cover_decode_result_t decode_png_rgb888(const uint8_t * data, uint32_t size, size_t max_side,
                                               uint8_t ** out_buf, int * out_w, int * out_h) {
    unsigned w = 0, h = 0;
    LodePNGState state;
    lodepng_state_init(&state);
    unsigned inspect_error = lodepng_inspect(&w, &h, &state, data, size);
    lodepng_state_cleanup(&state);
    if (inspect_error != 0) return COVER_DECODE_FAIL_UNSUPPORTED;
    if (!rgb888_size_ok(w, h, max_side, NULL)) return COVER_DECODE_FAIL_OVERSIZED;
    unsigned inspected_w = w, inspected_h = h;

    unsigned char * pixels = NULL;
    unsigned decode_error = lodepng_decode24(&pixels, &w, &h, data, size);
    if (decode_error == 83 /* LODEPNG_ERROR_OUT_OF_MEMORY */) {
        return COVER_DECODE_FAIL_ALLOC;
    }
    if (decode_error != 0 || !pixels) {
        return COVER_DECODE_FAIL_UNSUPPORTED;
    }
    if (w != inspected_w || h != inspected_h || !rgb888_size_ok(w, h, max_side, NULL)) {
        free(pixels);
        return COVER_DECODE_FAIL_UNSUPPORTED;
    }

    *out_buf = pixels;
    *out_w = (int) w;
    *out_h = (int) h;
    return COVER_DECODE_OK;
}

static inline uint16_t rgb888_to_565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t) (((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

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

static uint32_t le32(const uint8_t * p) {
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

static int32_t le32s(const uint8_t * p) {
    return (int32_t) le32(p);
}

static uint16_t le16(const uint8_t * p) {
    return (uint16_t) (p[0] | (p[1] << 8));
}

static bool inspect_bmp(const uint8_t * data, uint32_t size, int * out_w, int * out_h) {
    if (size < 54 || data[0] != 'B' || data[1] != 'M') return false;
    int width = le32s(data + 18);
    int height_raw = le32s(data + 22);
    if (width <= 0 || height_raw == INT32_MIN) return false;
    int height = height_raw < 0 ? -height_raw : height_raw;
    if (height <= 0) return false;
    *out_w = width;
    *out_h = height;
    return true;
}

static cover_decode_result_t decode_bmp_rgb888(const uint8_t * data, uint32_t size, size_t max_side,
                                               uint8_t ** out_buf, int * out_w, int * out_h) {
    if (size < 54 || data[0] != 'B' || data[1] != 'M') return COVER_DECODE_FAIL_UNSUPPORTED;
    uint32_t off = le32(data + 10);
    uint32_t dib = le32(data + 14);
    if (dib < 40 || off < 14 + dib || off >= size) return COVER_DECODE_FAIL_UNSUPPORTED;
    int width = le32s(data + 18);
    int height_raw = le32s(data + 22);
    if (height_raw == INT32_MIN) return COVER_DECODE_FAIL_UNSUPPORTED;
    bool top_down = height_raw < 0;
    int height = top_down ? -height_raw : height_raw;
    uint16_t planes = le16(data + 26);
    uint16_t bits = le16(data + 28);
    uint32_t compression = le32(data + 30);
    if (width <= 0 || height <= 0 || planes != 1 || compression != 0) return COVER_DECODE_FAIL_UNSUPPORTED;
    if (bits != 24 && bits != 32) return COVER_DECODE_FAIL_UNSUPPORTED;
    size_t need;
    if (!rgb888_size_ok((size_t) width, (size_t) height, max_side, &need)) return COVER_DECODE_FAIL_OVERSIZED;
    int bpp = bits / 8;
    size_t row_bytes = (size_t) width * (size_t) bpp;
    size_t stride = (row_bytes + 3U) & ~(size_t) 3U;
    if ((uint64_t) off + (uint64_t) stride * (uint64_t) height > size) return COVER_DECODE_FAIL_UNSUPPORTED;
    uint8_t * buf = malloc(need);
    if (!buf) return COVER_DECODE_FAIL_ALLOC;
    for (int y = 0; y < height; y++) {
        int src_y = top_down ? y : (height - 1 - y);
        const uint8_t * src = data + off + (size_t) src_y * (size_t) stride;
        uint8_t * dst = buf + (size_t) y * (size_t) width * 3;
        for (int x = 0; x < width; x++) {
            dst[x * 3 + 0] = src[x * bpp + 2];
            dst[x * 3 + 1] = src[x * bpp + 1];
            dst[x * 3 + 2] = src[x * bpp + 0];
        }
    }
    *out_buf = buf;
    *out_w = width;
    *out_h = height;
    return COVER_DECODE_OK;
}

cover_decode_result_t cover_decode_to_rgb565_ex(const uint8_t * data, uint32_t size,
                                               int target_w, int target_h,
                                               artwork_priority_t prio,
                                               artwork_cancel_fn cancel_cb, void * user_data,
                                               uint16_t ** out_pixels) {
    if (!out_pixels) return COVER_DECODE_FAIL_UNSUPPORTED;
    *out_pixels = NULL;
    if (!data || size < 8 || target_w <= 0 || target_h <= 0) return COVER_DECODE_FAIL_UNSUPPORTED;
    if (cancel_cb && cancel_cb(user_data)) return COVER_DECODE_FAIL_CANCELLED;

    size_t max_side = (target_w <= 72 && target_h <= 72) ? MAX_THUMBNAIL_COVER_SIDE : MAX_PLAYER_COVER_SIDE;

    /* 1. Inspect dimensions before allocating any native buffer */
    int native_w = 0, native_h = 0;
    artwork_format_t fmt = ARTWORK_FORMAT_UNKNOWN;

    if (data[0] == 0xFF && data[1] == 0xD8) {
        fmt = ARTWORK_FORMAT_JPEG;
        if (!inspect_jpeg(data, size, &native_w, &native_h)) return COVER_DECODE_FAIL_UNSUPPORTED;
    } else if (data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G') {
        fmt = ARTWORK_FORMAT_PNG;
        if (!inspect_png(data, size, &native_w, &native_h)) return COVER_DECODE_FAIL_UNSUPPORTED;
    } else if (data[0] == 'B' && data[1] == 'M') {
        fmt = ARTWORK_FORMAT_BMP;
        if (!inspect_bmp(data, size, &native_w, &native_h)) return COVER_DECODE_FAIL_UNSUPPORTED;
    } else {
        return COVER_DECODE_FAIL_UNSUPPORTED;
    }

    if (native_w <= 0 || native_h <= 0) return COVER_DECODE_FAIL_UNSUPPORTED;
    if ((size_t) native_w > max_side || (size_t) native_h > max_side) {
        DBG_LOG("cover_decode: image rejected by dimension cap (%dx%d > max %zu)\n",
                native_w, native_h, max_side);
        return COVER_DECODE_FAIL_OVERSIZED;
    }

    /* 2. Estimate required memory and acquire decode slot from coordinator.
     * JPEG bills the post-scale RGB888 buffer, not native pixels. */
    int est_w = native_w, est_h = native_h;
    if (fmt == ARTWORK_FORMAT_JPEG) {
        uint8_t scale = jpeg_scale_for_target(native_w, native_h, target_w, target_h);
        est_w = native_w >> scale;
        est_h = native_h >> scale;
    }
    size_t est_bytes = artwork_estimate_decode_bytes(fmt, size, (size_t) est_w, (size_t) est_h,
                                                     (size_t) target_w, (size_t) target_h);
    uint32_t timeout_ms = (prio == ARTWORK_PRIO_PLAYER) ? 1000 : 300;

    artwork_acquire_result_t acq = artwork_coordinator_acquire(prio, est_bytes, timeout_ms,
                                                               cancel_cb, user_data);
    if (acq == ARTWORK_ACQUIRE_LOW_MEM) return COVER_DECODE_FAIL_LOW_MEMORY;
    if (acq == ARTWORK_ACQUIRE_BUSY) return COVER_DECODE_FAIL_BUSY;
    if (acq == ARTWORK_ACQUIRE_CANCELLED || acq == ARTWORK_ACQUIRE_SUSPENDED) return COVER_DECODE_FAIL_CANCELLED;
    if (acq != ARTWORK_ACQUIRE_OK) return COVER_DECODE_FAIL_BUSY;

    /* 3. Check for preemption right before starting expensive native decode */
    if (artwork_coordinator_should_yield(prio, cancel_cb, user_data)) {
        artwork_coordinator_release(prio);
        return COVER_DECODE_FAIL_CANCELLED;
    }

    uint8_t * native_buf = NULL;
    int decoded_w = 0, decoded_h = 0;
    cover_decode_result_t dec_res = COVER_DECODE_FAIL_UNSUPPORTED;

    if (fmt == ARTWORK_FORMAT_JPEG) {
        dec_res = decode_jpeg_rgb888(data, size, max_side, target_w, target_h,
                                     &native_buf, &decoded_w, &decoded_h);
    } else if (fmt == ARTWORK_FORMAT_PNG) {
        dec_res = decode_png_rgb888(data, size, max_side, &native_buf, &decoded_w, &decoded_h);
    } else if (fmt == ARTWORK_FORMAT_BMP) {
        dec_res = decode_bmp_rgb888(data, size, max_side, &native_buf, &decoded_w, &decoded_h);
    }

    if (dec_res != COVER_DECODE_OK || !native_buf || decoded_w <= 0 || decoded_h <= 0) {
        free(native_buf);
        artwork_coordinator_release(prio);
        return dec_res;
    }

    /* 4. Check for preemption right after native decode */
    if (artwork_coordinator_should_yield(prio, cancel_cb, user_data)) {
        free(native_buf);
        artwork_coordinator_release(prio);
        return COVER_DECODE_FAIL_CANCELLED;
    }

    /* 5. Perform fast cover-fit resize and immediately free native buffer */
    uint16_t * resized = resize_cover_fit(native_buf, decoded_w, decoded_h, target_w, target_h);
    free(native_buf);

    /* 6. Release coordinator decode slot */
    artwork_coordinator_release(prio);

    if (!resized) return COVER_DECODE_FAIL_ALLOC;

    *out_pixels = resized;
    return COVER_DECODE_OK;
}

bool cover_decode_to_rgb565(const uint8_t * data, uint32_t size, int target_w, int target_h,
                            uint16_t ** out_pixels) {
    artwork_priority_t prio = (target_w <= 72 && target_h <= 72) ? ARTWORK_PRIO_THUMBNAIL : ARTWORK_PRIO_PLAYER;
    cover_decode_result_t res = cover_decode_to_rgb565_ex(data, size, target_w, target_h,
                                                          prio, NULL, NULL, out_pixels);
    return (res == COVER_DECODE_OK);
}
