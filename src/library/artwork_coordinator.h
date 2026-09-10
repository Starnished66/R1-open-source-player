#ifndef ARTWORK_COORDINATOR_H
#define ARTWORK_COORDINATOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ARTWORK_PRIO_WARMER = 0,    /* Background thumbnail cache warmer */
    ARTWORK_PRIO_THUMBNAIL = 1, /* Visible album row on active screen */
    ARTWORK_PRIO_PLAYER = 2,    /* Current player cover art or lyrics backdrop */
} artwork_priority_t;

typedef enum {
    ARTWORK_FORMAT_UNKNOWN = 0,
    ARTWORK_FORMAT_JPEG,
    ARTWORK_FORMAT_PNG,
    ARTWORK_FORMAT_BMP,
    /* Progressive (SOF2) JPEG -- decoded via a separate vendored libjpeg
     * fallback (src/library/cover_decode.c), never tjpgd (which rejects
     * SOF2 outright). Its memory profile is fundamentally different from
     * baseline ARTWORK_FORMAT_JPEG: dominated by a coefficient buffer that
     * scales with NATIVE image dimensions, not the requested output size --
     * see artwork_estimate_decode_bytes()'s progressive_coeff_bytes
     * parameter. */
    ARTWORK_FORMAT_JPEG_PROGRESSIVE,
} artwork_format_t;

typedef enum {
    ARTWORK_ACQUIRE_OK = 0,      /* Acquired coordinator lock; caller MUST release */
    ARTWORK_ACQUIRE_BUSY,        /* Higher/equal priority active, or lock timeout */
    ARTWORK_ACQUIRE_LOW_MEM,     /* MemAvailable below safety reserve */
    ARTWORK_ACQUIRE_SUSPENDED,   /* Warmer suspended due to active playback or UI */
    ARTWORK_ACQUIRE_CANCELLED,   /* Cancelled before or during wait */
} artwork_acquire_result_t;

typedef enum {
    ARTWORK_FAIL_NONE = 0,
    ARTWORK_FAIL_PERMANENT,      /* Corrupt, unsupported, oversized, or missing art */
    ARTWORK_FAIL_TEMPORARY,      /* Low memory, coordinator busy, or cancelled */
} artwork_fail_reason_t;

/* Dynamic cancellation callback: evaluated live during multi-stage decode */
typedef bool (*artwork_cancel_fn)(void * user_data);

/* Returns current MemAvailable in bytes from /proc/meminfo (cached briefly). */
size_t system_get_mem_available_bytes(void);

/* Test hook: inject mock available memory (0 to disable mock) */
void system_set_mock_mem_available(size_t bytes);

/* Calculates estimated peak memory for a decode using format-specific overhead.
 * native_w/native_h are the POST-SCALE dimensions the decoder will actually
 * produce for ARTWORK_FORMAT_JPEG/JPEG_PROGRESSIVE (matching this JPEG
 * decode's own jpeg_scale_for_target() policy), and the true NATIVE (source)
 * dimensions for PNG/BMP, which don't scale during decode.
 * progressive_coeff_bytes is the real coefficient-buffer size from a prior
 * jpeg_probe() call -- ignored for every format except JPEG_PROGRESSIVE,
 * where it's the dominant cost (see jpeg_probe_t's own doc comment,
 * cover_decode.h, for why this can't be derived from native_w/native_h
 * alone: it depends on the SOF's true native dimensions and component
 * sampling factors, not the post-scale output size this function otherwise
 * bills). Pass 0 for every non-progressive-JPEG call.
 * png_native_bpp is the PNG's real IHDR bits-per-pixel (from
 * lodepng_get_bpp() on the inspected color mode) -- ignored for every
 * format except PNG, where the decoder's real transient workspace (the
 * inflated scanline buffer, live at the same time as the decoded pixel
 * buffer) scales with it directly: a 16-bit-per-channel RGBA PNG (64bpp)
 * needs a scanline row 8x wider than an 8-bit grayscale one at the same
 * pixel dimensions, not the flat "4 bytes/pixel" this used to assume
 * regardless of real bit depth. Pass 0 for every non-PNG call.
 * Returns estimated bytes, or SIZE_MAX on overflow / invalid dimensions. */
size_t artwork_estimate_decode_bytes(artwork_format_t fmt, size_t compressed_size,
                                     size_t native_w, size_t native_h,
                                     size_t target_w, size_t target_h,
                                     uint64_t progressive_coeff_bytes,
                                     uint32_t png_native_bpp);

/* Checks if MemAvailable satisfies (reserve + estimated_bytes). */
bool artwork_check_memory_admission(artwork_priority_t prio, size_t estimated_bytes);

/* The per-priority reserve artwork_check_memory_admission() itself uses --
 * exported as the single source of truth for src/library/metadata.c's own
 * child-process memory-ceiling use of the same reserve tiers. Unknown/
 * out-of-range values return the most conservative (WARMER) reserve. */
size_t artwork_reserve_bytes_for_priority(artwork_priority_t prio);

/* Attempts to acquire the exclusive decode slot for the given priority and estimated memory.
 * Strictly guarantees priority ordering: Player beats Thumbnail, Thumbnail beats Warmer.
 * On ARTWORK_ACQUIRE_OK, caller MUST call artwork_coordinator_release(). */
artwork_acquire_result_t artwork_coordinator_acquire(artwork_priority_t prio, size_t estimated_bytes,
                                                     uint32_t timeout_ms,
                                                     artwork_cancel_fn cancel_cb, void * user_data);

/* Releases the exclusive decode slot. */
void artwork_coordinator_release(artwork_priority_t prio);

/* Checks if an active lower-priority decode should yield/abandon because a higher-priority
 * request is pending or cancellation was requested. */
bool artwork_coordinator_should_yield(artwork_priority_t my_prio,
                                     artwork_cancel_fn cancel_cb, void * user_data);

/* --- Negative / Backoff failure cache --- */

/* Checks if an album (by song_id and source_mtime) is recorded as failed.
 * If source_mtime > cached mtime, returns false (source was updated). */
bool artwork_failure_cache_is_blocked(int64_t song_id, time_t source_mtime, artwork_fail_reason_t * out_reason);

/* Records an artwork failure for an album with its current source mtime. */
void artwork_failure_cache_record(int64_t song_id, time_t source_mtime, artwork_fail_reason_t reason);

/* Clears/invalidates the failure cache. */
void artwork_failure_cache_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* ARTWORK_COORDINATOR_H */
