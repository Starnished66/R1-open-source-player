#include "bluetooth_reconnect.h"
#include "bluetooth_control.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    unsigned int generation;
    char preferred_mac[18];
} bt_reconnect_job_t;

static pthread_mutex_t reconnect_state_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t reconnect_thread;
static bool reconnect_joinable;
static atomic_bool reconnect_active = false;
static atomic_bool reconnect_done = false;
static atomic_uint reconnect_generation = 0;

static bool reconnect_valid_mac(const char * mac) {
    if (!mac || strlen(mac) != 17) return false;
    for (int i = 0; i < 17; i++) {
        if (i % 3 == 2) {
            if (mac[i] != ':') return false;
        } else if (!((mac[i] >= '0' && mac[i] <= '9') ||
                     (mac[i] >= 'A' && mac[i] <= 'F') ||
                     (mac[i] >= 'a' && mac[i] <= 'f'))) return false;
    }
    return true;
}

static bool reconnect_cancelled(void * ctx) {
    const bt_reconnect_job_t * job = ctx;
    return !job || atomic_load_explicit(&reconnect_generation, memory_order_acquire) != job->generation;
}

static void * reconnect_thread_func(void * arg) {
    bt_reconnect_job_t job = *(const bt_reconnect_job_t *) arg;
    free(arg);
    bool cancelled = reconnect_cancelled(&job);
    for (unsigned int attempt = 0; !cancelled && attempt < 3; attempt++) {
        if (attempt > 0) {
            /* The backend owns the command timeout; this sleep is deliberately
             * interruptible so cancel never waits out a full retry interval. */
            for (unsigned int waited = 0; waited < 2000 && !cancelled; waited += 50) {
                usleep(50000);
                cancelled = reconnect_cancelled(&job);
            }
            if (cancelled) break;
        }
        if (bt_control_reconnect_paired(job.preferred_mac[0] ? job.preferred_mac : NULL,
                                        reconnect_cancelled, &job))
            break;
        cancelled = reconnect_cancelled(&job);
    }
    atomic_store_explicit(&reconnect_done, true, memory_order_release);
    return NULL;
}

bool bt_reconnect_start(const char * preferred_mac) {
    if (preferred_mac && !reconnect_valid_mac(preferred_mac)) return false;
    pthread_mutex_lock(&reconnect_state_mutex);
    if (atomic_load_explicit(&reconnect_active, memory_order_acquire)) {
        pthread_mutex_unlock(&reconnect_state_mutex);
        return false;
    }

    bt_reconnect_job_t * job = calloc(1, sizeof(*job));
    if (!job) {
        pthread_mutex_unlock(&reconnect_state_mutex);
        return false;
    }
    job->generation = atomic_fetch_add_explicit(&reconnect_generation, 1, memory_order_acq_rel) + 1;
    if (preferred_mac) memcpy(job->preferred_mac, preferred_mac, sizeof(job->preferred_mac));
    atomic_store_explicit(&reconnect_done, false, memory_order_relaxed);
    atomic_store_explicit(&reconnect_active, true, memory_order_release);
    reconnect_joinable = true;
    if (pthread_create(&reconnect_thread, NULL, reconnect_thread_func, job) != 0) {
        free(job);
        reconnect_joinable = false;
        atomic_store_explicit(&reconnect_active, false, memory_order_release);
        pthread_mutex_unlock(&reconnect_state_mutex);
        return false;
    }
    pthread_mutex_unlock(&reconnect_state_mutex);
    return true;
}

void bt_reconnect_cancel(void) {
    atomic_fetch_add_explicit(&reconnect_generation, 1, memory_order_acq_rel);
}

void bt_reconnect_poll(void) {
    if (!atomic_load_explicit(&reconnect_active, memory_order_acquire) ||
            !atomic_load_explicit(&reconnect_done, memory_order_acquire)) return;

    pthread_mutex_lock(&reconnect_state_mutex);
    if (reconnect_joinable) {
        /* Hold the state lock through join so shutdown/poll cannot join the
         * same pthread concurrently, and start cannot replace its handle. */
        pthread_join(reconnect_thread, NULL);
        reconnect_joinable = false;
        atomic_store_explicit(&reconnect_active, false, memory_order_release);
    }
    pthread_mutex_unlock(&reconnect_state_mutex);
}

bool bt_reconnect_busy(void) {
    return atomic_load_explicit(&reconnect_active, memory_order_acquire);
}

void bt_reconnect_shutdown(void) {
    bt_reconnect_cancel();
    pthread_mutex_lock(&reconnect_state_mutex);
    if (reconnect_joinable) {
        pthread_join(reconnect_thread, NULL);
        reconnect_joinable = false;
        atomic_store_explicit(&reconnect_active, false, memory_order_release);
    }
    pthread_mutex_unlock(&reconnect_state_mutex);
}
