#include "cover_decode.h"

#include "lvgl/src/libs/tjpgd/tjpgd.h"
#include "lvgl/src/libs/lodepng/lodepng.h"
/* libjpeg fallback (decode_jpeg_libjpeg_rgb888() below) -- tjpgd above stays
 * the decoder for every ORDINARY baseline JPEG. Two cases route to libjpeg
 * instead: progressive (SOF2, which tjpgd rejects outright) and baseline
 * (SOF0) with a chroma sampling factor tjpgd's own minimal whitelist
 * rejects (e.g. vertical-only/4:4:0 subsampling). See jpeg_probe_t. */
#include <stdio.h>  /* jpeglib.h expects size_t/FILE to already be visible */
#include "jpeglib.h"
#include "jerror.h"

#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <setjmp.h>

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

/* Real coefficient-buffer estimate from actual SOF component sampling
 * factors -- see jpeg_probe_t's own doc comment (cover_decode.h) for the
 * MCU-rounding rationale. Never assumes 4:2:0; reads the real factors. */
static uint64_t jpeg_probe_coeff_bytes(int width, int height, int num_comp,
                                       const uint8_t * h_samp, const uint8_t * v_samp) {
    int h_max = 1, v_max = 1;
    for (int i = 0; i < num_comp; i++) {
        if (h_samp[i] > h_max) h_max = h_samp[i];
        if (v_samp[i] > v_max) v_max = v_samp[i];
    }
    if (h_max <= 0 || v_max <= 0) return 0;

    uint64_t mcus_per_row = ((uint64_t) width + (uint64_t) (8 * h_max) - 1) / (uint64_t) (8 * h_max);
    uint64_t mcus_per_col = ((uint64_t) height + (uint64_t) (8 * v_max) - 1) / (uint64_t) (8 * v_max);

    uint64_t total = 0;
    for (int i = 0; i < num_comp; i++) {
        uint64_t blocks_wide = mcus_per_row * (uint64_t) h_samp[i];
        uint64_t blocks_high = mcus_per_col * (uint64_t) v_samp[i];
        /* 64 coefficients/block * sizeof(int16_t) = 128 bytes/block. Checked
         * multiply order (blocks first, then *128) keeps every intermediate
         * well under uint64_t range for any width/height this ever sees --
         * SOF fields are 16-bit, so blocks_wide/high are individually
         * bounded near 8192 each, nowhere near overflow. */
        total += blocks_wide * blocks_high * 128ULL;
    }
    return total;
}

/* Mirrors lvgl/src/libs/tjpgd/tjpgd.c's own SOF0 sampling-factor whitelist
 * exactly (its jd_prepare() switch(marker) case 0xC0 handler): only 1 or 3
 * components; component 0 (Y) must be 4:4:4 (h=1,v=1), 4:2:0 (h=2,v=2), or
 * 4:2:2-horizontal (h=2,v=1); every other component (Cb/Cr) must be h=1,v=1.
 * Used only for SOF0 -- SOF2 is routed via is_progressive regardless. */
static bool tjpgd_supports_sof0_sampling(int num_comp, const uint8_t * h_samp, const uint8_t * v_samp) {
    if (num_comp != 1 && num_comp != 3) return false;
    bool y_ok = (h_samp[0] == 1 && v_samp[0] == 1) ||
               (h_samp[0] == 2 && v_samp[0] == 2) ||
               (h_samp[0] == 2 && v_samp[0] == 1);
    if (!y_ok) return false;
    for (int i = 1; i < num_comp; i++) {
        if (h_samp[i] != 1 || v_samp[i] != 1) return false;
    }
    return true;
}

bool jpeg_probe(const uint8_t * data, uint32_t size, jpeg_probe_t * result) {
    memset(result, 0, sizeof(*result));
    if (!data || size < 4 || data[0] != 0xFF || data[1] != 0xD8) return false; /* not even a JPEG SOI */

    uint32_t pos = 2; /* invariant, maintained by every advance below: pos <= size */
    while (size - pos > 1) {
        if (data[pos] != 0xFF) return false; /* expected a marker, stream is malformed */
        uint8_t marker = data[pos + 1];
        pos += 2;
        /* JPEG allows arbitrary 0xFF fill bytes before the real marker code. */
        while (marker == 0xFF && pos < size) {
            marker = data[pos];
            pos++;
        }
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) continue; /* TEM, RSTn -- no length/payload */
        if (marker == 0xD9) return false; /* EOI reached, no SOF ever found */
        /* Every remaining-length check below is `size - pos` (never
         * `pos + N > size`) specifically because pos/size are both
         * attacker-influenced uint32_t: an addition-based check can wrap
         * around near UINT32_MAX and silently pass when it shouldn't.
         * Subtraction is safe here only because pos <= size is an
         * invariant maintained by every advance in this function -- never
         * introduce a new advance without re-checking that it holds. */
        if (size - pos < 2) return false; /* truncated before a length field */
        uint32_t seg_len = ((uint32_t) data[pos] << 8) | (uint32_t) data[pos + 1];
        if (seg_len < 2 || size - pos < seg_len) return false; /* malformed/truncated segment */

        bool is_sof = (marker >= 0xC0 && marker <= 0xCF) && marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
        if (is_sof) {
            /* Payload after the 2 length bytes: 1 precision, 2 height, 2
             * width, 1 component count, then 3 bytes per component. */
            uint32_t payload = pos + 2;
            if (seg_len < 8 || size - payload < 6) return false;
            int height = ((int) data[payload + 1] << 8) | (int) data[payload + 2];
            int width = ((int) data[payload + 3] << 8) | (int) data[payload + 4];
            int num_comp = data[payload + 5];
            if (width <= 0 || height <= 0 || num_comp <= 0 || num_comp > 4) return false;
            uint32_t comp_bytes = (uint32_t) num_comp * 3;
            /* The segment's OWN declared length must actually be big enough
             * for its own component table (per spec, seg_len == exactly
             * 8 + 3*num_comp for a well-formed SOF) -- without this, a SOF
             * with a too-short seg_len could still pass a plain file-size
             * bounds check by reading bytes that really belong to whatever
             * follows this segment, misinterpreting them as its own
             * component records. Algebraically, combined with the
             * `size - pos < seg_len` check above, this also guarantees
             * `size - payload >= 6 + comp_bytes` -- the explicit check just
             * below is deliberately redundant with that (cheap, and this is
             * adversarial-input-facing code: prefer an explicit second
             * guarantee over trusting the algebra alone). */
            if (seg_len < 8 + comp_bytes) return false;
            if (size - payload < 6 + comp_bytes) return false;

            /* We only ever support SOF0 (routes to tjpgd, or to the libjpeg
             * fallback if tjpgd_incompatible below) or SOF2 (always routes
             * to the libjpeg fallback) -- extended sequential (SOF1),
             * lossless (SOF3), differential (SOF5-7), and every arithmetic-
             * coded variant (SOF9-15, jdarith.c is deliberately not
             * vendored/linked) are all genuinely unsupported. */
            if (marker != 0xC0 && marker != 0xC2) {
                result->supported = false;
                return true;
            }

            uint8_t h_samp[4], v_samp[4];
            for (int i = 0; i < num_comp; i++) {
                uint8_t samp = data[payload + 6 + i * 3 + 1];
                h_samp[i] = (samp >> 4) & 0x0F;
                v_samp[i] = samp & 0x0F;
                /* JPEG/T.81 caps sampling factors at 1-4; the 4-bit nibble
                 * encoding allows up to 15, so a crafted file could claim a
                 * much larger value. Nothing downstream would overflow on
                 * that (width/height are already 16-bit-bounded, so the
                 * MCU-rounded block counts in jpeg_probe_coeff_bytes() stay
                 * bounded regardless), but there's no reason to accept an
                 * out-of-spec value silently just because the arithmetic
                 * happens to stay safe -- reject it explicitly instead. */
                if (h_samp[i] == 0 || h_samp[i] > 4 || v_samp[i] == 0 || v_samp[i] > 4) return false;
            }

            result->is_progressive = (marker == 0xC2);
            result->supported = true;
            result->native_w = width;
            result->native_h = height;
            result->coeff_bytes = result->is_progressive ?
                jpeg_probe_coeff_bytes(width, height, num_comp, h_samp, v_samp) : 0;
            result->tjpgd_incompatible = !result->is_progressive &&
                !tjpgd_supports_sof0_sampling(num_comp, h_samp, v_samp);
            return true;
        }

        pos += seg_len; /* not a SOF -- skip this segment (APPn/COM/DQT/DHT/DRI/...) */
    }
    return false; /* ran out of data before any SOF marker */
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

    if (!jpeg_decode_dims_ok((int) jd.width, (int) jd.height, target_w, target_h)) {
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

struct my_jpeg_error_mgr {
    struct jpeg_error_mgr pub;
    jmp_buf setjmp_buffer;
    cover_decode_result_t fail_code;
};

static void my_jpeg_error_exit(j_common_ptr cinfo) {
    struct my_jpeg_error_mgr * myerr = (struct my_jpeg_error_mgr *) cinfo->err;
    if (cinfo->err && cinfo->err->msg_code == JERR_OUT_OF_MEMORY) {
        myerr->fail_code = COVER_DECODE_FAIL_ALLOC;
    } else {
        myerr->fail_code = COVER_DECODE_FAIL_UNSUPPORTED;
    }
    longjmp(myerr->setjmp_buffer, 1);
}

/* Handles both progressive (SOF2) JPEGs and baseline (SOF0) JPEGs whose
 * chroma sampling tjpgd's own minimal whitelist rejects -- see jpeg_probe_t.
 * The actual decode logic below is not progressive-specific in any way; it
 * is a plain libjpeg header/scale/scanline-loop decode. A genuinely
 * sequential-multiscan SOF0 (legal JPEG, but rare, and NOT accounted for by
 * ARTWORK_FORMAT_JPEG_LIBJPEG_BASELINE's admission estimate -- see the
 * jpeg_has_multiple_scans() guard below) is rejected rather than decoded. */
static cover_decode_result_t decode_jpeg_libjpeg_rgb888(const uint8_t * data, uint32_t size, size_t max_side,
                                                         int target_w, int target_h,
                                                         uint8_t ** out_buf, int * out_w, int * out_h) {
    if (!out_buf || !out_w || !out_h) return COVER_DECODE_FAIL_UNSUPPORTED;
    *out_buf = NULL;
    *out_w = 0;
    *out_h = 0;
    if (!data || size == 0) return COVER_DECODE_FAIL_UNSUPPORTED;

    struct jpeg_decompress_struct cinfo;
    struct my_jpeg_error_mgr jerr;
    /* volatile: modified after setjmp() and read in the setjmp() recovery
     * block below (reached via longjmp() from deep inside libjpeg, e.g.
     * during jpeg_read_scanlines()) -- a non-volatile automatic variable's
     * value after a longjmp is unspecified by the C standard unless it was
     * never written between the setjmp() call and the longjmp(). */
    uint8_t * volatile raw_buf = NULL;

    memset(&cinfo, 0, sizeof(cinfo));
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = my_jpeg_error_exit;
    jerr.fail_code = COVER_DECODE_FAIL_UNSUPPORTED;

    if (setjmp(jerr.setjmp_buffer)) {
        if (raw_buf) {
            free(raw_buf);
            raw_buf = NULL;
        }
        jpeg_destroy_decompress(&cinfo);
        return jerr.fail_code;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, data, (size_t) size);

    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&cinfo);
        return COVER_DECODE_FAIL_UNSUPPORTED;
    }

    /* A baseline (SOF0) file can still be "sequential multiscan" -- its
     * first scan omits some components, so libjpeg still needs full
     * native-sized coefficient buffers to hold it together across scans,
     * exactly like true progressive. jpeg_has_multiple_scans() is true for
     * BOTH that case and genuine progressive; cinfo.progressive_mode
     * distinguishes them. Real progressive already has its coefficient
     * cost billed via jpeg_probe_coeff_bytes()/progressive_coeff_bytes
     * upstream -- this guard only rejects the OTHER case, which
     * ARTWORK_FORMAT_JPEG_LIBJPEG_BASELINE's admission estimate never
     * accounts for. Checked before any allocation. */
    if (!cinfo.progressive_mode && jpeg_has_multiple_scans(&cinfo)) {
        jpeg_destroy_decompress(&cinfo);
        return COVER_DECODE_FAIL_UNSUPPORTED;
    }

    /* Scaling: mirror baseline tjpgd policy for consistency across decoders */
    uint8_t n = jpeg_scale_for_target((int) cinfo.image_width, (int) cinfo.image_height,
                                      target_w, target_h);
    cinfo.scale_num = 1;
    cinfo.scale_denom = 1U << n;

    cinfo.out_color_space = JCS_RGB;
    cinfo.do_fancy_upsampling = FALSE;
    cinfo.do_block_smoothing = FALSE;
    cinfo.dct_method = JDCT_ISLOW;

    jpeg_calc_output_dimensions(&cinfo);

    int out_width = (int) cinfo.output_width;
    int out_height = (int) cinfo.output_height;

    size_t needed_bytes = 0;
    if (!rgb888_size_ok((size_t) out_width, (size_t) out_height, max_side, &needed_bytes)) {
        jpeg_destroy_decompress(&cinfo);
        return COVER_DECODE_FAIL_OVERSIZED;
    }

    raw_buf = malloc(needed_bytes);
    if (!raw_buf) {
        jpeg_destroy_decompress(&cinfo);
        return COVER_DECODE_FAIL_ALLOC;
    }

    /* jpeg_start_decompress handles multi-scan input consumption for progressive JPEG */
    (void) jpeg_start_decompress(&cinfo);

    while (cinfo.output_scanline < cinfo.output_height) {
        JSAMPROW row_ptr = (JSAMPROW) (raw_buf + (size_t) cinfo.output_scanline * (size_t) out_width * 3U);
        JDIMENSION lines_read = jpeg_read_scanlines(&cinfo, &row_ptr, 1);
        if (lines_read == 0) {
            free(raw_buf);
            jpeg_destroy_decompress(&cinfo);
            return COVER_DECODE_FAIL_UNSUPPORTED;
        }
    }

    (void) jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);

    *out_buf = raw_buf;
    *out_w = out_width;
    *out_h = out_height;
    return COVER_DECODE_OK;
}

static bool inspect_png(const uint8_t * data, uint32_t size, int * out_w, int * out_h, uint32_t * out_bpp) {
    unsigned w = 0, h = 0;
    LodePNGState state;
    lodepng_state_init(&state);
    unsigned inspect_error = lodepng_inspect(&w, &h, &state, data, size);
    /* lodepng_inspect() already parsed and validated IHDR's bitdepth/colortype
     * (checkColorValidity()) before returning success -- lodepng_get_bpp()
     * reads that same populated state, no extra parsing. Needed by the
     * caller to bill decode_png_rgb888()'s real transient memory use
     * (artwork_estimate_decode_bytes()), which scales with the PNG's real
     * bit depth, not a flat assumption. */
    unsigned bpp = (inspect_error == 0) ? lodepng_get_bpp(&state.info_png.color) : 0;
    lodepng_state_cleanup(&state);
    if (inspect_error != 0) return false;
    *out_w = (int) w;
    *out_h = (int) h;
    if (out_bpp) *out_bpp = (uint32_t) bpp;
    return true;
}

/* Real ICC profiles (sRGB, Display P3, etc) this app will ever see attached
 * to actual cover art run a few KB to low hundreds of KB. LodePNG's own
 * default cap (max_icc_size, 16MB) exists only to stop a pathological
 * profile from inflating unbounded -- 16MB is still enough to blow past
 * artwork admission's own memory budget on this device, so cap it much
 * tighter here. This is decode-local, not a vendored-file change: iCCP
 * data is parsed but never used for cover art (no color management is
 * applied to decoded pixels), so a real profile larger than this is
 * rejected (COVER_DECODE_FAIL_UNSUPPORTED) with zero visible effect. */
#define COVER_PNG_MAX_ICC_BYTES (1024UL * 1024UL)

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

    /* Inlined equivalent of lodepng_decode24() (LCT_RGB/8-bit output), not
     * the convenience wrapper, so max_icc_size can be capped -- see
     * COVER_PNG_MAX_ICC_BYTES's own comment. Also disables ancillary text
     * chunk storage, matching lodepng_decode_memory()'s own defaults (their
     * data is never used here either). */
    unsigned char * decoded = NULL;
    LodePNGState dec_state;
    lodepng_state_init(&dec_state);
    dec_state.info_raw.colortype = LCT_RGB;
    dec_state.info_raw.bitdepth = 8;
#ifdef LODEPNG_COMPILE_ANCILLARY_CHUNKS
    dec_state.decoder.read_text_chunks = 0;
    dec_state.decoder.remember_unknown_chunks = 0;
    dec_state.decoder.max_icc_size = COVER_PNG_MAX_ICC_BYTES;
#endif
    unsigned decode_error = lodepng_decode(&decoded, &w, &h, &dec_state, data, size);
    lodepng_state_cleanup(&dec_state);
    /* Our LVGL LodePNG fork returns a draw-buffer descriptor, not a raw
     * malloc buffer. RGB24 conversion writes packed RGB into its data field
     * (the descriptor's ARGB8888 stride is allocation capacity only). */
    lv_draw_buf_t * draw_buf = (lv_draw_buf_t *) decoded;
    if (decode_error == 83 /* LODEPNG_ERROR_OUT_OF_MEMORY */) {
        if (draw_buf) lv_draw_buf_destroy(draw_buf);
        return COVER_DECODE_FAIL_ALLOC;
    }
    if (decode_error != 0 || !draw_buf) {
        if (draw_buf) lv_draw_buf_destroy(draw_buf);
        return COVER_DECODE_FAIL_UNSUPPORTED;
    }
    size_t bytes = 0;
    if (w != inspected_w || h != inspected_h || !rgb888_size_ok(w, h, max_side, &bytes) ||
        !draw_buf->data || draw_buf->data_size < bytes) {
        lv_draw_buf_destroy(draw_buf);
        return COVER_DECODE_FAIL_UNSUPPORTED;
    }
    uint8_t * pixels = malloc(bytes);
    if (pixels) memcpy(pixels, draw_buf->data, bytes);
    lv_draw_buf_destroy(draw_buf);
    if (!pixels) return COVER_DECODE_FAIL_ALLOC;

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

/* Expand RGB565 to 8-bit by left-shifting into the MSBs. Round-trip through
 * rgb888_to_565() is lossless for those 5/6 bits; bit-replication would not
 * change the packed result after the packer's 0xF8/0xFC masks. */
static inline void rgb565_to_888(uint16_t p, uint8_t * r, uint8_t * g, uint8_t * b) {
    *r = (uint8_t) ((p >> 11) << 3);
    *g = (uint8_t) (((p >> 5) & 0x3F) << 2);
    *b = (uint8_t) ((p & 0x1F) << 3);
}

static void bilinear_sample_rgb565(const uint16_t * src, int src_w, int src_h, float fx, float fy,
                                   uint8_t * out_r, uint8_t * out_g, uint8_t * out_b) {
    if (fx < 0) fx = 0;
    if (fy < 0) fy = 0;
    if (fx > src_w - 1) fx = (float) (src_w - 1);
    if (fy > src_h - 1) fy = (float) (src_h - 1);

    int x0 = (int) fx, y0 = (int) fy;
    int x1 = x0 + 1 < src_w ? x0 + 1 : x0;
    int y1 = y0 + 1 < src_h ? y0 + 1 : y0;
    float tx = fx - x0, ty = fy - y0;

    uint8_t p00[3], p10[3], p01[3], p11[3];
    rgb565_to_888(src[(size_t) y0 * src_w + x0], &p00[0], &p00[1], &p00[2]);
    rgb565_to_888(src[(size_t) y0 * src_w + x1], &p10[0], &p10[1], &p10[2]);
    rgb565_to_888(src[(size_t) y1 * src_w + x0], &p01[0], &p01[1], &p01[2]);
    rgb565_to_888(src[(size_t) y1 * src_w + x1], &p11[0], &p11[1], &p11[2]);

    for (int c = 0; c < 3; c++) {
        float top = p00[c] * (1.0f - tx) + p10[c] * tx;
        float bot = p01[c] * (1.0f - tx) + p11[c] * tx;
        float v = top * (1.0f - ty) + bot * ty;
        uint8_t out = (uint8_t) (v + 0.5f);
        if (c == 0) *out_r = out; else if (c == 1) *out_g = out; else *out_b = out;
    }
}

uint16_t * cover_resize_rgb565(const uint16_t * src, int src_w, int src_h, int dst_w, int dst_h) {
    if (!src || src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) return NULL;

    /* Same-size must copy, not fall through to the area-average path:
     * at scale=1.0 that path's inclusive floor windows (sx1 = dx+1) average
     * a 2x2 neighborhood, which is not identity. R1's player-cache and
     * on-screen cover are both 480x480, so this is the hot path there. */
    if (src_w == dst_w && src_h == dst_h) {
        size_t bytes = (size_t) dst_w * (size_t) dst_h * sizeof(uint16_t);
        uint16_t * dst = malloc(bytes);
        if (!dst) return NULL;
        memcpy(dst, src, bytes);
        return dst;
    }

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
                bilinear_sample_rgb565(src, src_w, src_h, fx, fy, &r, &g, &b);
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
                const uint16_t * row = src + (size_t) sy * src_w;
                for (int sx = sx0; sx <= sx1; sx++) {
                    uint8_t r, g, b;
                    rgb565_to_888(row[sx], &r, &g, &b);
                    r_sum += r;
                    g_sum += g;
                    b_sum += b;
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
    /* uint64_t, not uint32_t: "14 + dib" wraps for dib near UINT32_MAX
     * (e.g. dib=0xFFFFFFFF wraps to 13), which would falsely satisfy
     * "off < 14+dib" for a small, otherwise-plausible off and defeat this
     * check's actual intent (pixel data must start after a validly-sized
     * DIB header). Not reachable as a real out-of-bounds read today (off
     * itself is still bounds-checked below against the real pixel-data
     * extent, and dib is never used for anything else), but the check
     * should mean what it says. */
    if (dib < 40 || (uint64_t) off < 14ULL + (uint64_t) dib || off >= size) return COVER_DECODE_FAIL_UNSUPPORTED;
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

/* Ceiling-based equivalent of jpeg_scale_for_target()'s scaled dims for the
 * SAME chosen scale factor -- libjpeg's own jpeg_core_output_dimensions()
 * (jpeg/jdinput.c) rounds UP via jdiv_round_up(), not down like tjpgd's
 * floor(native/2^n) (see jpeg_scale_for_target()'s own doc comment). Used
 * only for the two libjpeg-routed formats (JPEG_PROGRESSIVE,
 * JPEG_LIBJPEG_BASELINE) so their admission estimate/dimension cap matches
 * what libjpeg will actually produce, rather than tjpgd's rounding. */
static void jpeg_libjpeg_scaled_dims(int native_w, int native_h, int target_w, int target_h,
                                     int * out_w, int * out_h) {
    uint8_t n = jpeg_scale_for_target(native_w, native_h, target_w, target_h);
    int denom = 1 << n;
    *out_w = (native_w + denom - 1) / denom;
    *out_h = (native_h + denom - 1) / denom;
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
    uint64_t progressive_coeff_bytes = 0;
    uint32_t png_native_bpp = 0;

    if (data[0] == 0xFF && data[1] == 0xD8) {
        /* jpeg_probe() (not inspect_jpeg()/tjpgd's jd_prepare()) makes the
         * routing decision here: ordinary baseline (SOF0) still goes
         * through tjpgd completely unchanged below via inspect_jpeg();
         * progressive (SOF2) and baseline with a tjpgd-incompatible
         * sampling factor both route to the separate libjpeg fallback --
         * see jpeg_probe_t's own doc comment (cover_decode.h) for why
         * tjpgd's own probe can't be reused for this (it rejects SOF2 and
         * unusual sampling outright, with no dimensions). */
        jpeg_probe_t probe;
        if (!jpeg_probe(data, size, &probe) || !probe.supported) return COVER_DECODE_FAIL_UNSUPPORTED;
        if (probe.is_progressive) {
            fmt = ARTWORK_FORMAT_JPEG_PROGRESSIVE;
            native_w = probe.native_w;
            native_h = probe.native_h;
            progressive_coeff_bytes = probe.coeff_bytes;
        } else if (probe.tjpgd_incompatible) {
            /* Baseline JPEG, but a chroma-subsampling combination tjpgd's
             * own minimal SOF0 whitelist rejects (e.g. vertical-only/4:4:0
             * subsampling -- valid JPEG, just rare). Native dimensions come
             * from jpeg_probe()'s own SOF parse, not inspect_jpeg()/tjpgd's
             * jd_prepare() -- that would fail for exactly the same reason
             * this whole case exists. */
            fmt = ARTWORK_FORMAT_JPEG_LIBJPEG_BASELINE;
            native_w = probe.native_w;
            native_h = probe.native_h;
        } else {
            fmt = ARTWORK_FORMAT_JPEG;
            if (!inspect_jpeg(data, size, &native_w, &native_h)) return COVER_DECODE_FAIL_UNSUPPORTED;
        }
    } else if (data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G') {
        fmt = ARTWORK_FORMAT_PNG;
        if (!inspect_png(data, size, &native_w, &native_h, &png_native_bpp)) return COVER_DECODE_FAIL_UNSUPPORTED;
    } else if (data[0] == 'B' && data[1] == 'M') {
        fmt = ARTWORK_FORMAT_BMP;
        if (!inspect_bmp(data, size, &native_w, &native_h)) return COVER_DECODE_FAIL_UNSUPPORTED;
    } else {
        return COVER_DECODE_FAIL_UNSUPPORTED;
    }

    if (native_w <= 0 || native_h <= 0) return COVER_DECODE_FAIL_UNSUPPORTED;
    if (fmt == ARTWORK_FORMAT_JPEG) {
        if (!jpeg_decode_dims_ok(native_w, native_h, target_w, target_h)) {
            DBG_LOG("cover_decode: JPEG rejected by dimension cap (%dx%d target %dx%d)\n",
                    native_w, native_h, target_w, target_h);
            return COVER_DECODE_FAIL_OVERSIZED;
        }
    } else if (fmt == ARTWORK_FORMAT_JPEG_PROGRESSIVE) {
        /* Strict cap, no scale-loophole: baseline's jpeg_decode_dims_ok()
         * allows native up to MAX_JPEG_NATIVE_SIDE (4096) as long as the
         * POST-SCALE output fits, because tjpgd never materializes more
         * than a flat ~32KB workspace regardless of source size. Progressive
         * has no equivalent escape hatch -- its coefficient buffer scales
         * with NATIVE dimensions no matter how small the requested target
         * is, so native itself must stay within MAX_DECODED_COVER_SIDE. A
         * 4096px progressive cover would need ~50-100MB of coefficients
         * alone, categorically impossible on this device -- permanently
         * reject rather than retry, it will never become decodable. */
        if (native_w > MAX_DECODED_COVER_SIDE || native_h > MAX_DECODED_COVER_SIDE) {
            DBG_LOG("cover_decode: progressive JPEG rejected by native dimension cap (%dx%d > %d)\n",
                    native_w, native_h, MAX_DECODED_COVER_SIDE);
            return COVER_DECODE_FAIL_OVERSIZED;
        }
    } else if (fmt == ARTWORK_FORMAT_JPEG_LIBJPEG_BASELINE) {
        /* Same lenient policy as tjpgd's own baseline cap (native up to
         * MAX_JPEG_NATIVE_SIDE as long as post-scale fits) -- genuinely
         * single-scan libjpeg decode has no native-dimension-scaling
         * coefficient cost the way progressive does (decode_jpeg_libjpeg_
         * rgb888()'s own jpeg_has_multiple_scans() guard rejects the one
         * baseline case that WOULD have that cost), so progressive's
         * stricter native-only cap doesn't apply here. Post-scale uses
         * libjpeg's own ceiling rounding, not tjpgd's floor. */
        if (native_w > MAX_JPEG_NATIVE_SIDE || native_h > MAX_JPEG_NATIVE_SIDE) {
            DBG_LOG("cover_decode: libjpeg-baseline JPEG rejected by native dimension cap (%dx%d > %d)\n",
                    native_w, native_h, MAX_JPEG_NATIVE_SIDE);
            return COVER_DECODE_FAIL_OVERSIZED;
        }
        int sw, sh;
        jpeg_libjpeg_scaled_dims(native_w, native_h, target_w, target_h, &sw, &sh);
        if (sw > MAX_DECODED_COVER_SIDE || sh > MAX_DECODED_COVER_SIDE) {
            DBG_LOG("cover_decode: libjpeg-baseline JPEG rejected by post-scale cap (%dx%d > %d)\n",
                    sw, sh, MAX_DECODED_COVER_SIDE);
            return COVER_DECODE_FAIL_OVERSIZED;
        }
    } else {
        if ((size_t) native_w > max_side || (size_t) native_h > max_side) {
            DBG_LOG("cover_decode: image rejected by dimension cap (%dx%d > max %zu)\n",
                    native_w, native_h, max_side);
            return COVER_DECODE_FAIL_OVERSIZED;
        }
    }

    /* 2. Estimate required memory and acquire decode slot from coordinator.
     * JPEG (baseline and progressive alike) bills the post-scale RGB888
     * buffer via est_w/est_h -- progressive's real, dimension-dependent
     * coefficient cost is billed separately via progressive_coeff_bytes,
     * computed from the SOF's true native size, not this post-scale size. */
    int est_w = native_w, est_h = native_h;
    if (fmt == ARTWORK_FORMAT_JPEG) {
        uint8_t scale = jpeg_scale_for_target(native_w, native_h, target_w, target_h);
        est_w = native_w >> scale;
        est_h = native_h >> scale;
    } else if (fmt == ARTWORK_FORMAT_JPEG_PROGRESSIVE || fmt == ARTWORK_FORMAT_JPEG_LIBJPEG_BASELINE) {
        jpeg_libjpeg_scaled_dims(native_w, native_h, target_w, target_h, &est_w, &est_h);
    }
    size_t est_bytes = artwork_estimate_decode_bytes(fmt, size, (size_t) est_w, (size_t) est_h,
                                                     (size_t) target_w, (size_t) target_h,
                                                     progressive_coeff_bytes, png_native_bpp);
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
    } else if (fmt == ARTWORK_FORMAT_JPEG_PROGRESSIVE || fmt == ARTWORK_FORMAT_JPEG_LIBJPEG_BASELINE) {
        dec_res = decode_jpeg_libjpeg_rgb888(data, size, max_side, target_w, target_h,
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
