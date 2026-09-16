#ifndef TRACK_PROBE_H
#define TRACK_PROBE_H

#include <stddef.h>
#include "audio.h"

#define TRACK_PROBE_MAX_TRACKS 200
#define TRACK_PROBE_MAX_PATH_BYTES 2048

typedef struct track_probe_job track_probe_job_t;

/* Copies all paths before starting one worker. Accepts 1..200 non-NULL
 * paths of at most 2047 bytes each (excluding NUL). Returns NULL for invalid
 * input or allocation/thread creation failure. Caller may release its paths
 * immediately on return. No probing or waiting for file I/O happens here. */
track_probe_job_t * track_probe_job_start(const char * const * paths, size_t count);

/* Nonblocking cooperative cancellation: the current probe, if any, finishes
 * and publishes its result; subsequent files are skipped. NULL is harmless. */
void track_probe_job_cancel(track_probe_job_t * job);

/* Nonblocking single-consumer poll. Returns true for the next published
 * result and its original zero-based index. Only valid, codec and
 * duration_seconds are retained; all other output fields are zero. A failed
 * probe still publishes a completely zero/invalid result. False leaves
 * outputs and the cursor unchanged (including for NULL arguments).
 * Results already published remain available after cancellation/completion. */
bool track_probe_job_next(track_probe_job_t * job, size_t * index,
                          audio_current_format_info_t * out);

/* Acquire-load completion; does not imply all published results were read.
 * NULL is considered done. */
bool track_probe_job_done(track_probe_job_t * job);

/* Joins and frees ONLY when done; NULL or an unfinished job is a safe no-op.
 * An unfinished job remains owned by the caller: poll done and retry later.
 * The join after done only reaps the exiting worker, never waits for probing.
 * One owner must serialize next/cancel/destroy; no call may race destruction.
 * Never uses pthread_cancel and has no UI dependencies. */
void track_probe_job_destroy(track_probe_job_t * job);

/* BLOCKING final-shutdown cleanup only: cancel, join even if unfinished, and
 * free. May wait for the current file probe. Never use for navigation, page
 * reloads or UI polling. NULL is harmless; no other calls may race this. */
void track_probe_job_cancel_and_destroy(track_probe_job_t * job);

#endif
