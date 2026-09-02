#include "artwork_coordinator.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "audio.h"
#include "debug_log.h"

/* --- System MemAvailable Helper with Short Cache --- */

#define MEMINFO_CACHE_TTL_MS 50ULL

static uint64_t get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000ULL + (uint64_t) ts.tv_nsec / 1000000ULL;
}

static size_t mock_mem_available_bytes = 0;

void system_set_mock_mem_available(size_t bytes) {
    mock_mem_available_bytes = bytes;
}

size_t system_get_mem_available_bytes(void) {
    if (mock_mem_available_bytes > 0) {
        return mock_mem_available_bytes;
    }

    static pthread_mutex_t mem_lock = PTHREAD_MUTEX_INITIALIZER;
    static uint64_t last_check_ms = 0;
    static size_t cached_mem_bytes = 0;

    uint64_t now = get_time_ms();

    pthread_mutex_lock(&mem_lock);
    if (now - last_check_ms < MEMINFO_CACHE_TTL_MS && last_check_ms != 0) {
        size_t result = cached_mem_bytes;
        pthread_mutex_unlock(&mem_lock);
        return result;
    }

    FILE * f = fopen("/proc/meminfo", "r");
    if (!f) {
#ifdef HOST_BUILD
        /* On host simulator without /proc/meminfo, return a healthy default */
        cached_mem_bytes = 64U * 1024U * 1024U;
        last_check_ms = now;
        pthread_mutex_unlock(&mem_lock);
        return cached_mem_bytes;
#else
        /* On target, failure to read /proc/meminfo must fail safe: return 0
         * so optional thumbnail and warmer jobs are refused rather than crashing. */
        cached_mem_bytes = 0;
        last_check_ms = now;
        pthread_mutex_unlock(&mem_lock);
        return 0;
#endif
    }

    char line[128];
    unsigned long mem_avail_kb = 0;
    unsigned long mem_free_kb = 0;
    unsigned long buffers_kb = 0;
    unsigned long cached_kb = 0;
    bool found_avail = false;

    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "MemAvailable: %lu kB", &mem_avail_kb) == 1) {
            found_avail = true;
            break;
        } else if (sscanf(line, "MemFree: %lu kB", &mem_free_kb) == 1) {
        } else if (sscanf(line, "Buffers: %lu kB", &buffers_kb) == 1) {
        } else if (sscanf(line, "Cached: %lu kB", &cached_kb) == 1) {
        }
    }
    fclose(f);

    if (found_avail) {
        cached_mem_bytes = (size_t) mem_avail_kb * 1024U;
    } else if (mem_free_kb > 0) {
        cached_mem_bytes = (size_t) (mem_free_kb + buffers_kb + cached_kb) * 1024U;
    } else {
        cached_mem_bytes = 0;
    }

    last_check_ms = now;
    size_t result = cached_mem_bytes;
    pthread_mutex_unlock(&mem_lock);
    return result;
}

/* --- Overflow-Safe Format-Aware Memory Estimation --- */

size_t artwork_estimate_decode_bytes(artwork_format_t fmt, size_t compressed_size,
                                     size_t native_w, size_t native_h,
                                     size_t target_w, size_t target_h) {
    if (native_w == 0 || native_h == 0 || target_w == 0 || target_h == 0) return SIZE_MAX;
    if (native_w > 4096 || native_h > 4096 || target_w > 1024 || target_h > 1024) return SIZE_MAX;

    /* Native RGB888 output buffer */
    uint64_t native_bytes = (uint64_t) native_w * (uint64_t) native_h * 3ULL;
    /* Resized target RGB565 buffer */
    uint64_t target_bytes = (uint64_t) target_w * (uint64_t) target_h * 2ULL;

    /* Decoder internal workspace by format:
     * - PNG: LodePNG transiently inflates zlib stream and allocates intermediate
     *   raw scanline buffers with filter bytes (~4 bytes/pixel) + zlib window.
     * - JPEG: tjpgd streams 8x8 MCU blocks into destination; minimal ~32KB buffer.
     *   native_w/h for JPEG is the post-scale RGB888 size, not the source pixel size.
     * - BMP: uncompressed linear stream; ~16KB overhead. */
    uint64_t decoder_workspace = 64ULL * 1024ULL;
    if (fmt == ARTWORK_FORMAT_PNG) {
        decoder_workspace = (uint64_t) native_w * (uint64_t) native_h * 4ULL + (128ULL * 1024ULL);
    } else if (fmt == ARTWORK_FORMAT_JPEG) {
        decoder_workspace = 32ULL * 1024ULL;
    } else if (fmt == ARTWORK_FORMAT_BMP) {
        decoder_workspace = 16ULL * 1024ULL;
    }

    uint64_t total = (uint64_t) compressed_size + native_bytes + target_bytes + decoder_workspace;
    if (total > (uint64_t) SIZE_MAX) return SIZE_MAX;
    return (size_t) total;
}

/* --- Memory Admission Check --- */

#define MEM_RESERVE_PLAYER    (2U * 1024U * 1024U)  /* 2 MiB reserve for player cover */
#define MEM_RESERVE_THUMBNAIL (4U * 1024U * 1024U)  /* 4 MiB reserve for visible thumbnails */
#define MEM_RESERVE_WARMER    (8U * 1024U * 1024U)  /* 8 MiB reserve for background warmer */

bool artwork_check_memory_admission(artwork_priority_t prio, size_t estimated_bytes) {
    if (estimated_bytes == SIZE_MAX) return false;

    size_t reserve = MEM_RESERVE_WARMER;
    if (prio == ARTWORK_PRIO_PLAYER) reserve = MEM_RESERVE_PLAYER;
    else if (prio == ARTWORK_PRIO_THUMBNAIL) reserve = MEM_RESERVE_THUMBNAIL;

    size_t available = system_get_mem_available_bytes();
    if (available < reserve) return false;
    return (available - reserve) >= estimated_bytes;
}

/* --- Process-Wide Artwork Decode Coordinator --- */

static pthread_mutex_t coord_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t coord_cond = PTHREAD_COND_INITIALIZER;

static bool is_decoding = false;
static artwork_priority_t active_prio = ARTWORK_PRIO_WARMER;
static int pending_player_count = 0;
static int pending_thumb_count = 0;

artwork_acquire_result_t artwork_coordinator_acquire(artwork_priority_t prio, size_t estimated_bytes,
                                                     uint32_t timeout_ms,
                                                     artwork_cancel_fn cancel_cb, void * user_data) {
    if (cancel_cb && cancel_cb(user_data)) return ARTWORK_ACQUIRE_CANCELLED;

    /* Check memory admission first before waiting */
    if (!artwork_check_memory_admission(prio, estimated_bytes)) {
        DBG_LOG("artwork_coord: memory admission rejected (prio=%d est=%zu avail=%zu)\n",
                prio, estimated_bytes, system_get_mem_available_bytes());
        return ARTWORK_ACQUIRE_LOW_MEM;
    }

    pthread_mutex_lock(&coord_lock);

    /* Background warmer is suspended if audio is playing or higher priority is pending/active */
    if (prio == ARTWORK_PRIO_WARMER) {
        if (audio_is_playing()) {
            pthread_mutex_unlock(&coord_lock);
            return ARTWORK_ACQUIRE_SUSPENDED;
        }
        if (pending_player_count > 0 || pending_thumb_count > 0 || is_decoding) {
            pthread_mutex_unlock(&coord_lock);
            return ARTWORK_ACQUIRE_BUSY;
        }
    }

    if (prio == ARTWORK_PRIO_PLAYER) pending_player_count++;
    else if (prio == ARTWORK_PRIO_THUMBNAIL) pending_thumb_count++;

    uint64_t start_ms = get_time_ms();
    uint64_t deadline_ms = start_ms + (uint64_t) timeout_ms;

    /* Strict priority predicate: lower priorities must wait while higher priorities are pending */
    while (is_decoding ||
           (prio == ARTWORK_PRIO_THUMBNAIL && pending_player_count > 0) ||
           (prio == ARTWORK_PRIO_WARMER && (pending_player_count > 0 || pending_thumb_count > 0))) {

        if (cancel_cb && cancel_cb(user_data)) {
            if (prio == ARTWORK_PRIO_PLAYER) pending_player_count--;
            else if (prio == ARTWORK_PRIO_THUMBNAIL) pending_thumb_count--;
            pthread_mutex_unlock(&coord_lock);
            return ARTWORK_ACQUIRE_CANCELLED;
        }

        /* If higher priority arrives while a lower priority is running, wake waiters */
        if (prio > active_prio && is_decoding) {
            pthread_cond_broadcast(&coord_cond);
        }

        uint64_t now = get_time_ms();
        if (now >= deadline_ms) {
            if (prio == ARTWORK_PRIO_PLAYER) pending_player_count--;
            else if (prio == ARTWORK_PRIO_THUMBNAIL) pending_thumb_count--;
            pthread_mutex_unlock(&coord_lock);
            return ARTWORK_ACQUIRE_BUSY;
        }

        uint64_t wait_ms = deadline_ms - now;
        if (wait_ms > 50) wait_ms = 50; /* Poll cancel_cb every 50ms */

        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        uint64_t nsec = (uint64_t) ts.tv_nsec + wait_ms * 1000000ULL;
        ts.tv_sec += (time_t) (nsec / 1000000000ULL);
        ts.tv_nsec = (long) (nsec % 1000000000ULL);

        pthread_cond_timedwait(&coord_cond, &coord_lock, &ts);
    }

    if (prio == ARTWORK_PRIO_PLAYER) pending_player_count--;
    else if (prio == ARTWORK_PRIO_THUMBNAIL) pending_thumb_count--;

    /* Re-verify memory admission right before taking the slot */
    if (!artwork_check_memory_admission(prio, estimated_bytes)) {
        pthread_mutex_unlock(&coord_lock);
        return ARTWORK_ACQUIRE_LOW_MEM;
    }

    is_decoding = true;
    active_prio = prio;
    pthread_mutex_unlock(&coord_lock);
    return ARTWORK_ACQUIRE_OK;
}

void artwork_coordinator_release(artwork_priority_t prio) {
    (void) prio;
    pthread_mutex_lock(&coord_lock);
    is_decoding = false;
    pthread_cond_broadcast(&coord_cond);
    pthread_mutex_unlock(&coord_lock);
}

bool artwork_coordinator_should_yield(artwork_priority_t my_prio,
                                     artwork_cancel_fn cancel_cb, void * user_data) {
    if (cancel_cb && cancel_cb(user_data)) return true;
    if (my_prio == ARTWORK_PRIO_PLAYER) return false; /* Player never yields to lower priority */

    pthread_mutex_lock(&coord_lock);
    bool yield = false;
    if (my_prio == ARTWORK_PRIO_WARMER) {
        yield = (pending_player_count > 0 || pending_thumb_count > 0 || audio_is_playing());
    } else if (my_prio == ARTWORK_PRIO_THUMBNAIL) {
        yield = (pending_player_count > 0);
    }
    pthread_mutex_unlock(&coord_lock);
    return yield;
}

/* --- Negative / Backoff Failure Cache with mtime Invalidation --- */

#define FAILURE_CACHE_SIZE 256
#define TEMPORARY_BACKOFF_MS 10000ULL /* 10 seconds backoff for temporary failures */

typedef struct {
    int64_t song_id;
    time_t source_mtime;
    artwork_fail_reason_t reason;
    uint64_t timestamp_ms;
} artwork_failure_entry_t;

static artwork_failure_entry_t failure_cache[FAILURE_CACHE_SIZE];
static pthread_mutex_t failure_cache_lock = PTHREAD_MUTEX_INITIALIZER;

bool artwork_failure_cache_is_blocked(int64_t song_id, time_t source_mtime, artwork_fail_reason_t * out_reason) {
    if (song_id <= 0) return true;

    uint64_t now = get_time_ms();
    pthread_mutex_lock(&failure_cache_lock);
    size_t idx = (size_t) song_id % FAILURE_CACHE_SIZE;
    if (failure_cache[idx].song_id == song_id && failure_cache[idx].reason != ARTWORK_FAIL_NONE) {
        /* If source file was modified after the failure was recorded, invalidate entry */
        if (source_mtime > 0 && source_mtime > failure_cache[idx].source_mtime) {
            failure_cache[idx].reason = ARTWORK_FAIL_NONE;
            pthread_mutex_unlock(&failure_cache_lock);
            return false;
        }

        if (failure_cache[idx].reason == ARTWORK_FAIL_PERMANENT) {
            if (out_reason) *out_reason = ARTWORK_FAIL_PERMANENT;
            pthread_mutex_unlock(&failure_cache_lock);
            return true;
        } else if (failure_cache[idx].reason == ARTWORK_FAIL_TEMPORARY) {
            if (now - failure_cache[idx].timestamp_ms < TEMPORARY_BACKOFF_MS) {
                if (out_reason) *out_reason = ARTWORK_FAIL_TEMPORARY;
                pthread_mutex_unlock(&failure_cache_lock);
                return true;
            }
            /* Temporary backoff expired -- allow retry */
            failure_cache[idx].reason = ARTWORK_FAIL_NONE;
        }
    }
    pthread_mutex_unlock(&failure_cache_lock);
    return false;
}

void artwork_failure_cache_record(int64_t song_id, time_t source_mtime, artwork_fail_reason_t reason) {
    if (song_id <= 0 || reason == ARTWORK_FAIL_NONE) return;

    uint64_t now = get_time_ms();
    pthread_mutex_lock(&failure_cache_lock);
    size_t idx = (size_t) song_id % FAILURE_CACHE_SIZE;
    failure_cache[idx].song_id = song_id;
    failure_cache[idx].source_mtime = source_mtime;
    failure_cache[idx].reason = reason;
    failure_cache[idx].timestamp_ms = now;
    pthread_mutex_unlock(&failure_cache_lock);
}

void artwork_failure_cache_clear(void) {
    pthread_mutex_lock(&failure_cache_lock);
    memset(failure_cache, 0, sizeof(failure_cache));
    pthread_mutex_unlock(&failure_cache_lock);
}
