#ifndef COVER_DECODE_H
#define COVER_DECODE_H

#include <stdbool.h>
#include <stdint.h>
#include "artwork_coordinator.h"

#define MAX_DECODED_COVER_SIDE 1200   /* RGB888 after JPEG scale; PNG/BMP native */
#define MAX_JPEG_NATIVE_SIDE   4096   /* SOF0 sanity cap; tjpgd width is uint16_t */
#define MAX_PLAYER_COVER_SIDE  MAX_DECODED_COVER_SIDE
#define MAX_THUMBNAIL_COVER_SIDE MAX_DECODED_COVER_SIDE

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

/* Decodes cover-art bytes (JPEG, PNG, or uncompressed 24/32-bit BMP) and resizes them
 * with a "cover fit" (scale to fully fill target_w x target_h, center-
 * cropping whichever dimension overflows -- same as a photo app's cover/
 * thumbnail mode) into a newly malloc()'d RGB565 buffer the caller owns and
 * must free(). JPEGs are decompressed at the largest tjpgd 1/2^n that still
 * covers the target, then cover-fitted; PNG/BMP decode at native size first.
 * JPEG native may be up to 4096px if scaled RGB888 <= 1200px; PNG/BMP still
 * reject native dimensions exceeding 1200px.
 * Serialized through the process-wide artwork decode coordinator with memory admission. */
bool cover_decode_to_rgb565(const uint8_t * data, uint32_t size, int target_w, int target_h,
                            uint16_t ** out_pixels);

/* Extended decode with explicit priority, cancellation callback, and structured failure code */
cover_decode_result_t cover_decode_to_rgb565_ex(const uint8_t * data, uint32_t size,
                                               int target_w, int target_h,
                                               artwork_priority_t prio,
                                               artwork_cancel_fn cancel_cb, void * user_data,
                                               uint16_t ** out_pixels);

#endif /* COVER_DECODE_H */
