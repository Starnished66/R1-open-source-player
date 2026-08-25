#include <assert.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "artwork_coordinator.h"
#include "cover_decode.h"

/* Mock audio_is_playing for test harness if not linked with full audio.c */
static atomic_bool mock_audio_playing = false;
bool __attribute__((weak)) audio_is_playing(void) {
    return atomic_load(&mock_audio_playing);
}

/* --- Test 1: Format-Aware & Overflow-Safe Memory Estimation --- */

static void test_format_aware_memory_estimation(void) {
    printf("Testing format-aware and overflow-safe memory estimation...\n");

    /* JPEG: 800x800 native, 72x72 target */
    size_t est_jpeg = artwork_estimate_decode_bytes(ARTWORK_FORMAT_JPEG, 50 * 1024, 800, 800, 72, 72);
    /* 50KB + (800*800*3 = 1920000) + (72*72*2 = 10368) + 32KB + 64KB = ~2077568 bytes (~1.98 MiB) */
    assert(est_jpeg > 1900000 && est_jpeg < 2200000);

    /* PNG: 800x800 native, 72x72 target */
    size_t est_png = artwork_estimate_decode_bytes(ARTWORK_FORMAT_PNG, 50 * 1024, 800, 800, 72, 72);
    /* 50KB + (800*800*3 = 1920000) + (800*800*4 = 2560000) [workspace] + (72*72*2 = 10368) + 128KB = ~4700000 bytes (~4.5 MiB) */
    assert(est_png > 4400000 && est_png < 5000000);
    assert(est_png > est_jpeg); /* PNG workspace must be budgeted materially higher */

    /* BMP: 800x800 native, 72x72 target */
    size_t est_bmp = artwork_estimate_decode_bytes(ARTWORK_FORMAT_BMP, 50 * 1024, 800, 800, 72, 72);
    assert(est_bmp > 1900000 && est_bmp < est_jpeg);

    /* Zero dimensions must return SIZE_MAX */
    assert(artwork_estimate_decode_bytes(ARTWORK_FORMAT_JPEG, 100, 0, 800, 72, 72) == SIZE_MAX);
    assert(artwork_estimate_decode_bytes(ARTWORK_FORMAT_PNG, 100, 800, 0, 72, 72) == SIZE_MAX);
    assert(artwork_estimate_decode_bytes(ARTWORK_FORMAT_JPEG, 100, 800, 800, 0, 72) == SIZE_MAX);

    /* Out of bounds / overflow dimensions must return SIZE_MAX */
    assert(artwork_estimate_decode_bytes(ARTWORK_FORMAT_JPEG, 100, 5000, 5000, 72, 72) == SIZE_MAX);
    assert(artwork_estimate_decode_bytes(ARTWORK_FORMAT_PNG, 100, 800, 800, 2000, 2000) == SIZE_MAX);

    printf("  -> format-aware memory estimation passed.\n");
}

/* --- Test 2: Deterministic Mock Memory Admission Check --- */

static void test_deterministic_memory_admission(void) {
    printf("Testing deterministic memory admission reserves...\n");

    /* Case A: Low available memory (5 MiB available) */
    system_set_mock_mem_available(5 * 1024 * 1024);

    /* Player reserve is 2 MiB -> 3 MiB usable -> 2.5 MiB request should pass */
    assert(artwork_check_memory_admission(ARTWORK_PRIO_PLAYER, (size_t) (2.5 * 1024 * 1024)) == true);
    /* Player 3.5 MiB request exceeds 3 MiB usable -> should fail */
    assert(artwork_check_memory_admission(ARTWORK_PRIO_PLAYER, (size_t) (3.5 * 1024 * 1024)) == false);

    /* Thumbnail reserve is 4 MiB -> 1 MiB usable -> 2 MiB request should fail */
    assert(artwork_check_memory_admission(ARTWORK_PRIO_THUMBNAIL, (size_t) (2 * 1024 * 1024)) == false);
    assert(artwork_check_memory_admission(ARTWORK_PRIO_THUMBNAIL, (size_t) (0.5 * 1024 * 1024)) == true);

    /* Warmer reserve is 8 MiB -> 5 MiB < 8 MiB -> all warmer requests must fail */
    assert(artwork_check_memory_admission(ARTWORK_PRIO_WARMER, 1024) == false);

    /* Case B: Plentiful available memory (32 MiB available) */
    system_set_mock_mem_available(32 * 1024 * 1024);
    assert(artwork_check_memory_admission(ARTWORK_PRIO_WARMER, 4 * 1024 * 1024) == true);

    /* Reset mock memory */
    system_set_mock_mem_available(0);

    printf("  -> deterministic memory admission passed.\n");
}

/* --- Test 3: Competing Waiters Strict Priority Ordering --- */

typedef struct {
    artwork_priority_t prio;
    size_t bytes;
    uint32_t timeout_ms;
    artwork_acquire_result_t result;
    int order;
} waiter_thread_arg_t;

static atomic_int finish_order_counter = 0;

static void * waiter_test_thread(void * arg) {
    waiter_thread_arg_t * w = (waiter_thread_arg_t *) arg;
    w->result = artwork_coordinator_acquire(w->prio, w->bytes, w->timeout_ms, NULL, NULL);
    if (w->result == ARTWORK_ACQUIRE_OK) {
        w->order = atomic_fetch_add(&finish_order_counter, 1) + 1;
        usleep(30000); /* Hold slot for 30ms */
        artwork_coordinator_release(w->prio);
    }
    return NULL;
}

static void test_competing_waiters_priority_ordering(void) {
    printf("Testing competing waiters strict priority ordering...\n");

    system_set_mock_mem_available(32 * 1024 * 1024);
    atomic_store(&finish_order_counter, 0);

    /* 1. Main thread locks slot as Warmer */
    artwork_acquire_result_t lock_res = artwork_coordinator_acquire(ARTWORK_PRIO_WARMER, 1024 * 1024, 100, NULL, NULL);
    assert(lock_res == ARTWORK_ACQUIRE_OK);

    /* 2. Launch competing waiters: first Thumbnail, then Player */
    waiter_thread_arg_t w_thumb = { .prio = ARTWORK_PRIO_THUMBNAIL, .bytes = 1024 * 1024, .timeout_ms = 1000, .order = 0 };
    waiter_thread_arg_t w_player = { .prio = ARTWORK_PRIO_PLAYER, .bytes = 1024 * 1024, .timeout_ms = 1000, .order = 0 };

    pthread_t th_thumb, th_player;
    pthread_create(&th_thumb, NULL, waiter_test_thread, &w_thumb);
    usleep(10000); /* Let thumbnail waiter enter queue first */
    pthread_create(&th_player, NULL, waiter_test_thread, &w_player);
    usleep(10000); /* Let player waiter enter queue second */

    /* 3. Release initial warmer lock */
    artwork_coordinator_release(ARTWORK_PRIO_WARMER);

    /* 4. Wait for both threads to finish */
    pthread_join(th_thumb, NULL);
    pthread_join(th_player, NULL);

    assert(w_player.result == ARTWORK_ACQUIRE_OK);
    assert(w_thumb.result == ARTWORK_ACQUIRE_OK);

    /* Player MUST have acquired the slot BEFORE Thumbnail (order 1 vs order 2) */
    assert(w_player.order == 1);
    assert(w_thumb.order == 2);

    system_set_mock_mem_available(0);
    printf("  -> competing waiters priority ordering passed.\n");
}

/* --- Test 4: Dynamic Cancellation During Wait --- */

typedef struct {
    atomic_bool cancel;
    artwork_acquire_result_t result;
} cancel_thread_arg_t;

static bool dynamic_cancel_cb(void * user_data) {
    cancel_thread_arg_t * c = (cancel_thread_arg_t *) user_data;
    return atomic_load(&c->cancel);
}

static void * cancel_test_thread(void * arg) {
    cancel_thread_arg_t * c = (cancel_thread_arg_t *) arg;
    c->result = artwork_coordinator_acquire(ARTWORK_PRIO_THUMBNAIL, 1024 * 1024, 1000,
                                           dynamic_cancel_cb, c);
    return NULL;
}

static void test_dynamic_cancellation_during_wait(void) {
    printf("Testing dynamic cancellation during coordinator wait...\n");

    system_set_mock_mem_available(32 * 1024 * 1024);

    /* 1. Main thread locks slot */
    assert(artwork_coordinator_acquire(ARTWORK_PRIO_PLAYER, 1024 * 1024, 100, NULL, NULL) == ARTWORK_ACQUIRE_OK);

    /* 2. Spawn thread waiting on slot */
    cancel_thread_arg_t c = { .cancel = false, .result = ARTWORK_ACQUIRE_OK };
    pthread_t th;
    pthread_create(&th, NULL, cancel_test_thread, &c);

    usleep(50000); /* Wait 50ms while thread is blocked in cond_timedwait */

    /* 3. Signal cancellation dynamically */
    atomic_store(&c.cancel, true);

    pthread_join(th, NULL);

    /* Acquirer must have aborted and returned ARTWORK_ACQUIRE_CANCELLED */
    assert(c.result == ARTWORK_ACQUIRE_CANCELLED);

    artwork_coordinator_release(ARTWORK_PRIO_PLAYER);
    system_set_mock_mem_available(0);

    printf("  -> dynamic cancellation passed.\n");
}

/* --- Test 5: Negative / Backoff Failure Cache with mtime --- */

static void test_negative_failure_cache_with_mtime(void) {
    printf("Testing negative and backoff failure cache with source mtime...\n");

    artwork_failure_cache_clear();

    artwork_fail_reason_t reason = ARTWORK_FAIL_NONE;
    time_t t1 = 1000;
    time_t t2 = 2000;

    assert(!artwork_failure_cache_is_blocked(101, t1, &reason));

    /* Record permanent failure for song 101 at mtime t1 */
    artwork_failure_cache_record(101, t1, ARTWORK_FAIL_PERMANENT);
    assert(artwork_failure_cache_is_blocked(101, t1, &reason));
    assert(reason == ARTWORK_FAIL_PERMANENT);

    /* If source file is updated to mtime t2, block must be bypassed */
    assert(!artwork_failure_cache_is_blocked(101, t2, &reason));

    /* Record temporary failure for song 202 */
    artwork_failure_cache_record(202, t1, ARTWORK_FAIL_TEMPORARY);
    assert(artwork_failure_cache_is_blocked(202, t1, &reason));
    assert(reason == ARTWORK_FAIL_TEMPORARY);

    /* Cache clear removes all entries */
    artwork_failure_cache_clear();
    assert(!artwork_failure_cache_is_blocked(101, t1, &reason));
    assert(!artwork_failure_cache_is_blocked(202, t1, &reason));

    printf("  -> negative failure cache with mtime passed.\n");
}

/* --- Test 6: Structured Result Classification --- */

static void test_structured_result_classification(void) {
    printf("Testing structured decode result classification...\n");

    assert(cover_decode_result_is_permanent(COVER_DECODE_FAIL_UNSUPPORTED) == true);
    assert(cover_decode_result_is_permanent(COVER_DECODE_FAIL_OVERSIZED) == true);
    assert(cover_decode_result_is_permanent(COVER_DECODE_FAIL_LOW_MEMORY) == false);
    assert(cover_decode_result_is_permanent(COVER_DECODE_FAIL_BUSY) == false);
    assert(cover_decode_result_is_permanent(COVER_DECODE_FAIL_CANCELLED) == false);

    assert(cover_decode_result_is_temporary(COVER_DECODE_FAIL_LOW_MEMORY) == true);
    assert(cover_decode_result_is_temporary(COVER_DECODE_FAIL_BUSY) == true);
    assert(cover_decode_result_is_temporary(COVER_DECODE_FAIL_CANCELLED) == true);
    assert(cover_decode_result_is_temporary(COVER_DECODE_FAIL_ALLOC) == true);
    assert(cover_decode_result_is_temporary(COVER_DECODE_FAIL_UNSUPPORTED) == false);
    assert(cover_decode_result_is_permanent(COVER_DECODE_FAIL_ALLOC) == false);

    printf("  -> structured decode result classification passed.\n");
}

int main(void) {
    printf("=== artwork_coordinator_selftest starting ===\n");

    test_format_aware_memory_estimation();
    test_deterministic_memory_admission();
    test_competing_waiters_priority_ordering();
    test_dynamic_cancellation_during_wait();
    test_negative_failure_cache_with_mtime();
    test_structured_result_classification();

    printf("=== artwork_coordinator_selftest: ALL TESTS PASSED ===\n");
    return 0;
}
