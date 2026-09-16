#define _POSIX_C_SOURCE 200809L
#include "track_probe.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    bool valid;
    audio_codec_t codec;
    double duration_seconds;
} track_probe_result_t;

struct track_probe_job {
    pthread_t thread;
    atomic_bool cancelled;
    atomic_bool done;
    atomic_size_t published;
    size_t count;
    size_t next;
    char * path_storage;
    const char * paths[TRACK_PROBE_MAX_TRACKS];
    track_probe_result_t results[TRACK_PROBE_MAX_TRACKS];
};

static void * track_probe_worker(void * arg) {
    track_probe_job_t * job = arg;
    for (size_t i = 0; i < job->count; i++) {
        if (atomic_load_explicit(&job->cancelled, memory_order_acquire)) break;
        audio_current_format_info_t info = {0};
        if (audio_probe_file_format(job->paths[i], &info) && info.valid) {
            job->results[i].valid = true;
            job->results[i].codec = info.codec;
            job->results[i].duration_seconds = info.duration_seconds;
        }
        /* Each slot is written once. The consumer's acquire load makes all
         * slots below published visible without taking a worker-held lock. */
        atomic_store_explicit(&job->published, i + 1, memory_order_release);
    }
    atomic_store_explicit(&job->done, true, memory_order_release);
    return NULL;
}

track_probe_job_t * track_probe_job_start(const char * const * paths, size_t count) {
    if (!paths || !count || count > TRACK_PROBE_MAX_TRACKS) return NULL;
    size_t lengths[TRACK_PROBE_MAX_TRACKS];
    size_t bytes = 0;
    for (size_t i = 0; i < count; i++) {
        if (!paths[i]) return NULL;
        size_t length = strnlen(paths[i], TRACK_PROBE_MAX_PATH_BYTES);
        if (length == TRACK_PROBE_MAX_PATH_BYTES) return NULL;
        lengths[i] = length + 1;
        bytes += lengths[i]; /* At most 200 * 2048 bytes. */
    }

    track_probe_job_t * job = calloc(1, sizeof(*job));
    if (!job) return NULL;
    job->path_storage = malloc(bytes);
    if (!job->path_storage) {
        free(job);
        return NULL;
    }
    job->count = count;
    atomic_init(&job->cancelled, false);
    atomic_init(&job->done, false);
    atomic_init(&job->published, 0);
    char * dst = job->path_storage;
    for (size_t i = 0; i < count; i++) {
        memcpy(dst, paths[i], lengths[i]);
        job->paths[i] = dst;
        dst += lengths[i];
    }
    if (pthread_create(&job->thread, NULL, track_probe_worker, job) != 0) {
        free(job->path_storage);
        free(job);
        return NULL;
    }
    return job;
}

void track_probe_job_cancel(track_probe_job_t * job) {
    if (job) atomic_store_explicit(&job->cancelled, true, memory_order_release);
}

bool track_probe_job_next(track_probe_job_t * job, size_t * index,
                          audio_current_format_info_t * out) {
    if (!job || !index || !out) return false;
    size_t published = atomic_load_explicit(&job->published, memory_order_acquire);
    if (job->next >= published) return false;
    const track_probe_result_t * result = &job->results[job->next];
    memset(out, 0, sizeof(*out));
    out->valid = result->valid;
    out->codec = result->codec;
    out->duration_seconds = result->duration_seconds;
    *index = job->next++;
    return true;
}

bool track_probe_job_done(track_probe_job_t * job) {
    return !job || atomic_load_explicit(&job->done, memory_order_acquire);
}

void track_probe_job_destroy(track_probe_job_t * job) {
    if (!job || !track_probe_job_done(job)) return;
    if (pthread_join(job->thread, NULL) != 0) return;
    free(job->path_storage);
    free(job);
}

void track_probe_job_cancel_and_destroy(track_probe_job_t * job) {
    if (!job) return;
    track_probe_job_cancel(job);
    if (pthread_join(job->thread, NULL) != 0) return;
    free(job->path_storage);
    free(job);
}
