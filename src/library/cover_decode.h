#ifndef COVER_DECODE_H
#define COVER_DECODE_H

#include <stdbool.h>
#include <stdint.h>
#include "artwork_coordinator.h"

#define MAX_DECODED_COVER_SIDE 1200   /* RGB888 after JPEG scale; PNG/BMP native */
#define MAX_JPEG_NATIVE_SIDE   4096   /* SOF0 sanity cap; tjpgd width is uint16_t */
#define MAX_PLAYER_COVER_SIDE  MAX_DECODED_COVER_SIDE
#define MAX_THUMBNAIL_COVER_SIDE MAX_DECODED_COVER_SIDE

/* Hard sanity ceiling on native PNG dimensions eligible for the streaming
 * decoder (cover_decode.c's decode_png_streaming()) -- a security/sanity
 * bound against a pathological/malicious dimension claim, not a
 * memory-workaround bound (the streaming decoder's own peak RAM is bounded
 * by the post-scale output size regardless of native size, so this can be
 * generous). Twice MAX_JPEG_NATIVE_SIDE, matching that same convention.
 * Shared with artwork_coordinator.c's admission estimate so the two can
 * never independently drift out of sync on what this decoder is allowed to
 * attempt (a prior mismatch -- estimator capped at 4096, decoder at 8192 --
 * silently made every 4097-8192px streaming-eligible PNG a permanent
 * LOW_MEMORY reject instead of ever reaching the decoder). */
#define MAX_PNG_STREAMING_NATIVE_SIDE 8192

typedef enum {
    COVER_DECODE_OK = 0,
    COVER_DECODE_FAIL_UNSUPPORTED,  /* Corrupt header or unsupported format */
    COVER_DECODE_FAIL_OVERSIZED,    /* Image dimensions exceed max cap */
    COVER_DECODE_FAIL_LOW_MEMORY,   /* Rejected by memory admission */
    COVER_DECODE_FAIL_BUSY,         /* Coordinator timeout / busy */
    COVER_DECODE_FAIL_CANCELLED,    /* Cancelled or preempted */
    COVER_DECODE_FAIL_ALLOC,        /* Malloc allocation failure */
} cover_decode_result_t;

static inline bool cover_decode_result_is_permanent(cover_decode_result_t res) {
    return (res == COVER_DECODE_FAIL_UNSUPPORTED || res == COVER_DECODE_FAIL_OVERSIZED);
}

static inline bool cover_decode_result_is_temporary(cover_decode_result_t res) {
    return (res == COVER_DECODE_FAIL_LOW_MEMORY || res == COVER_DECODE_FAIL_BUSY ||
            res == COVER_DECODE_FAIL_CANCELLED || res == COVER_DECODE_FAIL_ALLOC);
}

/* Largest tjpgd scale n in {0,1,2,3} such that cover-fit from (w>>n)×(h>>n)
 * into target_w×target_h never upscales. tjpgd output is floor(native/2^n). */
static inline uint8_t jpeg_scale_for_target(int native_w, int native_h, int target_w, int target_h) {
    uint8_t scale = 0;
    if (native_w <= 0 || native_h <= 0 || target_w <= 0 || target_h <= 0) return 0;
    for (uint8_t n = 1; n <= 3; n++) {
        int w = native_w >> n;
        int h = native_h >> n;
        if (w < 1 || h < 1) break;
        if (w < target_w || h < target_h) break;
        scale = n;
    }
    return scale;
}

/* True if this JPEG may be decoded for target_w x target_h: native within
 * MAX_JPEG_NATIVE_SIDE and post-scale RGB888 within MAX_DECODED_COVER_SIDE. */
static inline bool jpeg_decode_dims_ok(int native_w, int native_h, int target_w, int target_h) {
    if (native_w <= 0 || native_h <= 0 || native_w > MAX_JPEG_NATIVE_SIDE || native_h > MAX_JPEG_NATIVE_SIDE)
        return false;
    uint8_t n = jpeg_scale_for_target(native_w, native_h, target_w, target_h);
    int sw = native_w >> n, sh = native_h >> n;
    return sw > 0 && sh > 0 && sw <= MAX_DECODED_COVER_SIDE && sh <= MAX_DECODED_COVER_SIDE;
}

/* Cheap up-front JPEG header inspection, independent of tjpgd's jd_prepare()
 * (which rejects progressive/SOF2 outright and allocates Huffman tables even
 * for the baseline case). Walks markers (SOI -> skip APPn/COM/DQT/DHT/DRI ->
 * first SOFn) with no heap allocation, so baseline, progressive, and
 * tjpgd-incompatible-sampling baseline files can all be routed correctly
 * BEFORE committing to either decoder -- see is_progressive/
 * tjpgd_incompatible above. */
typedef struct {
    bool is_progressive;  /* true only for SOF2 */
    bool supported;       /* false for arithmetic coding (SOF9-SOF15), lossless
                            * (SOF3), hierarchical, or a marker stream that never
                            * reached a SOF before running out of data/hitting a
                            * genuinely malformed marker -- caller must treat as
                            * COVER_DECODE_FAIL_UNSUPPORTED regardless of
                            * is_progressive in that case. */
    /* true only for a baseline (SOF0) JPEG whose per-component sampling
     * factors tjpgd's own minimal whitelist would reject -- anything other
     * than 4:4:4 (0x11), 4:2:0 (0x22), or 4:2:2-horizontal (0x21) for
     * component 0, non-0x11 for any other component, or a component count
     * other than 1 or 3 (see lvgl/src/libs/tjpgd/tjpgd.c's own SOF0
     * handler). Always false when is_progressive is true (that case is
     * already fully handled by is_progressive) or when !supported.
     * This is a ROUTING HINT, not a decode guarantee: a 2- or 4-component
     * file can set this true, but the libjpeg fallback's forced JCS_RGB
     * output does not implement CMYK/YCCK conversion, so such a file will
     * still fail to decode (cleanly) rather than succeed. */
    bool tjpgd_incompatible;
    int native_w;
    int native_h;
    /* Real coefficient-buffer size estimate for JPEG admission control -- 0
     * only if !supported. Computed unconditionally for every supported
     * SOF0/SOF2 (not just progressive) from the SOF marker's own component
     * sampling factors (never assumed/guessed): MCU-round each component's
     * block dimensions against the frame's own max sampling factors, 128
     * bytes (64 coefficients * sizeof(int16_t)) per 8x8 block, summed across
     * all components. This is the dominant, unavoidable memory cost of
     * progressive JPEG -- it scales with native (SOF) dimensions, not the
     * caller's requested output size, and no decoder (this one included)
     * can avoid materializing it. A baseline (SOF0) file gets this same
     * conservative worst-case estimate too, because a cheap header-only
     * probe can't tell single-scan and sequential-multiscan apart (that's a
     * property of the entropy-coded scan structure, not the frame header),
     * and multiscan needs the identical full-native coefficient buffer --
     * billing every baseline as if it might be multiscan is deliberately
     * conservative, not a bug (see decode_jpeg_libjpeg_rgb888()'s own
     * comment in cover_decode.c). */
    uint64_t coeff_bytes;
} jpeg_probe_t;

/* Returns false only if no SOF marker was ever reached (truncated/malformed
 * input before any header info was available) -- check result->supported
 * too: a true return with supported=false means a SOF WAS found but it's an
 * unsupported JPEG process (arithmetic coding, lossless, etc), not that the
 * file is truncated. On any false/unsupported return, *result's other
 * fields are zeroed/false and must not be used. */
bool jpeg_probe(const uint8_t * data, uint32_t size, jpeg_probe_t * result);

/* Decodes cover-art bytes (JPEG, PNG, or uncompressed 24/32-bit BMP) and resizes them
 * with a "cover fit" (scale to fully fill target_w x target_h, center-
 * cropping whichever dimension overflows -- same as a photo app's cover/
 * thumbnail mode) into a newly malloc()'d RGB565 buffer the caller owns and
 * must free(). JPEGs are decompressed at the largest 1/2^n that still covers
 * the target (tjpgd first for ordinary baseline, falling through to a
 * vendored libjpeg decoder if tjpgd itself fails on it; libjpeg is also
 * used directly, with no tjpgd attempt at all, for progressive/SOF2 and for
 * baseline with a chroma sampling factor tjpgd's own whitelist rejects --
 * see jpeg_probe_t), then cover-fitted; PNG/BMP decode at native size
 * first. JPEG native (baseline and
 * progressive alike) may be up to 4096px as long as scaled RGB888 <= 1200px
 * -- progressive's real, dimension-dependent coefficient-buffer cost is
 * billed separately through memory admission (coeff_bytes), not capped by
 * dimension alone; BMP still rejects native dimensions exceeding 1200px, and
 * so does PNG UNLESS it's non-interlaced, 8- or 16-bit, RGB/RGBA (color
 * type 2/6) -- that case instead streams (decode_png_streaming()) up to
 * MAX_PNG_STREAMING_NATIVE_SIDE (8192px) at bounded memory, since it can
 * downscale during decode the way JPEG already does; everything else
 * (interlaced, palette, grayscale, other bit depths) keeps the 1200px cap.
 * Serialized through the process-wide artwork decode coordinator with memory admission. */
bool cover_decode_to_rgb565(const uint8_t * data, uint32_t size, int target_w, int target_h,
                            uint16_t ** out_pixels);

/* Extended decode with explicit priority, cancellation callback, and structured failure code */
cover_decode_result_t cover_decode_to_rgb565_ex(const uint8_t * data, uint32_t size,
                                               int target_w, int target_h,
                                               artwork_priority_t prio,
                                               artwork_cancel_fn cancel_cb, void * user_data,
                                               uint16_t ** out_pixels);

/* Cover-fit an already-decoded RGB565 buffer into a newly malloc()'d RGB565
 * buffer the caller owns and must free(). Same cover-fit, center-crop,
 * area-average-downscale, and bilinear-upscale math as
 * cover_decode_to_rgb565_ex()'s internal resize -- pixel sampling is RGB565
 * packed instead of RGB888 triplets. Same-size (src_w==dst_w && src_h==dst_h)
 * copies pixels byte-for-byte. Returns NULL on invalid args or alloc failure. */
uint16_t * cover_resize_rgb565(const uint16_t * src, int src_w, int src_h,
                               int dst_w, int dst_h);

#endif /* COVER_DECODE_H */
