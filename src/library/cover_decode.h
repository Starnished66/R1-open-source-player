#ifndef COVER_DECODE_H
#define COVER_DECODE_H

#include <stdbool.h>
#include <stdint.h>
#include "artwork_coordinator.h"

#define MAX_PLAYER_COVER_SIDE 1200
#define MAX_THUMBNAIL_COVER_SIDE 1200

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

/* Decodes cover-art bytes (JPEG, PNG, or uncompressed 24/32-bit BMP) and resizes them
 * with a "cover fit" (scale to fully fill target_w x target_h, center-
 * cropping whichever dimension overflows -- same as a photo app's cover/
 * thumbnail mode) into a newly malloc()'d RGB565 buffer the caller owns and
 * must free(). Sources wider or taller than max_side (1200px for player,
 * 800px for thumbnails) are rejected rather than decoded; this device
 * cannot hold a 2000–4000px RGB888 buffer next to the library and audio path.
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
