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
 * Returns estimated bytes, or SIZE_MAX on overflow / invalid dimensions. */
size_t artwork_estimate_decode_bytes(artwork_format_t fmt, size_t compressed_size,
                                     size_t native_w, size_t native_h,
                                     size_t target_w, size_t target_h);

/* Checks if MemAvailable satisfies (reserve + estimated_bytes). */
bool artwork_check_memory_admission(artwork_priority_t prio, size_t estimated_bytes);

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
