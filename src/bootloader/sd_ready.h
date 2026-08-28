#ifndef BOOTLOADER_SD_READY_H
#define BOOTLOADER_SD_READY_H

#include <stdbool.h>
#include <stdint.h>

/* Pure, dependency-injected SD-card-readiness state machine -- see
 * sd_ready.c's own top comment for the algorithm and sd_ready_real.c for the
 * real on-device probe implementations scanner.c actually uses. Split out
 * from scanner.c specifically so this logic can be exercised by
 * sd_ready_test.c (host-buildable, no real filesystem/mount/inotify access)
 * against scripted fake probes -- wait_for_sd_ready() itself never touches
 * a real file, clock, or subprocess directly, only through the probes
 * struct below, so the exact same algorithm runs under test as on the real
 * device. */

/* Every probe is called with `ctx` as its first argument -- the real
 * implementation (sd_ready_real.c) stashes its inotify fd there; a test's
 * fake implementation stashes whatever scripted state it needs (a simulated
 * clock, canned answers, call counters). wait_for_sd_ready() itself never
 * interprets `ctx`, just threads it through. */
typedef struct {
    void * ctx;

    /* True if `path` exists at all (any type) -- used for the two /dev
     * block-device node checks. Must not block. */
    bool (*path_exists)(void * ctx, const char * path);

    /* True if the SD mount point currently has a real filesystem mounted on
     * it (as opposed to an empty directory) -- checked both before and
     * after every try_mount() call, same as scanner.c's own pre-existing
     * sd_mount_point_mounted(). */
    bool (*mount_point_mounted)(void * ctx);

    /* Attempts to mount `device_node` at the SD mount point (vfat, then
     * exfat, then ntfs-3g, in turn) -- no return value; the caller always
     * re-checks mount_point_mounted() afterward, same convention this
     * project's mount helpers already use throughout (a failed attempt is
     * silent, not an error).
     *
     * `deadline_remaining_ms` is however much time is left before
     * wait_for_sd_ready()'s own hard deadline, at the moment of this call
     * -- an implementation that shells out to a mount helper (like
     * sd_ready_real.c's) MUST bound its own blocking calls to at most this,
     * re-measuring as it goes if it makes more than one; a mount helper
     * that can independently block far longer than this (the real
     * implementation's own subprocess_run() has a 15s default per-command
     * timeout, invisible to this deadline unless explicitly overridden)
     * defeats wait_for_sd_ready()'s whole hard-deadline guarantee -- this
     * is not merely advisory. A fake/test implementation that never
     * actually blocks may ignore this value entirely. */
    void (*try_mount)(void * ctx, const char * device_node, int64_t deadline_remaining_ms);

    /* Best-effort probe for evidence of a card that's weaker/earlier than a
     * /dev node existing -- see sd_ready_real.c's own doc comment for the
     * real implementation's exact heuristic (an MMC-core sysfs `type`
     * attribute check meant to exclude onboard eMMC/SDIO) and its
     * real-device-unverified status. Must not block; must never report
     * evidence it isn't reasonably confident in -- a false positive here
     * costs every no-card boot the same extension a real slow card gets,
     * while a false negative only ever costs back today's plain
     * short-deadline behavior. */
    bool (*mmc_evidence_present)(void * ctx);

    /* True if `path` exists, is a regular file, and passes access(X_OK) --
     * mirrors scanner.c's own pre-existing path_is_executable(). */
    bool (*path_is_executable)(void * ctx, const char * path);

    /* Current monotonic milliseconds, or a negative value if the clock
     * itself is unavailable/failed. wait_for_sd_ready() treats a negative
     * reading as fatal to any further waiting (see its own comment) rather
     * than looping on a clock it cannot trust. */
    int64_t (*monotonic_ms)(void * ctx);

    /* Waits up to approximately `ms` milliseconds, but may return sooner if
     * "woken" (the real implementation: an inotify event on /dev, when
     * available -- see sd_ready_real.c). Must handle its own internal EINTR
     * retry (remaining time, not a fresh `ms`) rather than returning early
     * on a signal -- wait_for_sd_ready() re-derives elapsed time from
     * monotonic_ms() on its own next loop iteration regardless, so this
     * only needs to avoid a tight spin, not guarantee an exact sleep
     * duration. Must not busy-wait. */
    void (*wait_ms)(void * ctx, int ms);
} sd_ready_probes_t;

/* Coarse, monotonically-meaningful readiness milestones -- used both to
 * decide whether to extend the deadline (see sd_ready.c) and to classify
 * the terminal outcome for logging (see sd_ready_real.c). Deliberately
 * collapses "whole-disk node" and "partition node" into one NODE_PRESENT
 * milestone for the progress/extension comparison (either is equally real
 * evidence a card responded), while the result struct below still records
 * which specific node(s) were actually seen for the log message. */
typedef enum {
    SD_READY_STAGE_NONE = 0,
    SD_READY_STAGE_MMC_EVIDENCE,
    SD_READY_STAGE_NODE_PRESENT,
    SD_READY_STAGE_MOUNTED,
    SD_READY_STAGE_EXEC_READY,
} sd_ready_stage_t;

typedef struct {
    sd_ready_stage_t stage;   /* highest milestone actually reached */
    bool saw_mmc_evidence;    /* sysfs reported an attached card at some point */
    bool saw_whole_node;      /* the whole-disk device node was seen to exist */
    bool saw_partition_node;  /* the partition device node was seen to exist */
    bool mounted;             /* a filesystem was successfully mounted */
    bool executable_ready;    /* mounted AND at least one exec_candidates[] entry passed path_is_executable() */
    const char * device_node_used; /* the device_node argument try_mount() was last called with when mounted became true; NULL if never mounted */
    int64_t elapsed_ms;       /* wall-clock time spent waiting, or -1 if monotonic_ms() itself never returned a usable reading */
} sd_ready_result_t;

/* Real on-device implementation (sd_ready_real.c) -- mounts the SD card at
 * SD_MOUNT_POINT (scanner.h) exactly like src/main.c's own
 * mount_sd_card_if_needed() does, but backed by wait_for_sd_ready() below
 * instead of a flat fixed-attempt loop. This is the ONLY entry point
 * scanner.c actually calls into this module; everything else here is
 * either the pure algorithm (this file) or file-local to sd_ready_real.c. */
void mount_sd_card_if_needed(void);

/* Runs the readiness state machine to completion (either full readiness or
 * a terminal give-up) and returns the outcome -- never blocks past
 * hard_deadline_ms of monotonic elapsed time (or a single best-effort pass
 * if the clock itself is unusable, see monotonic_ms() above). This holds
 * because every source of real blocking time is bounded against a freshly
 * re-read clock at the point it's about to happen, not against a value
 * cached earlier in the same iteration: try_mount()'s own
 * deadline_remaining_ms argument is recomputed immediately before each
 * call (see its own doc comment), and the final wait_ms() call each
 * iteration is itself clamped to whatever's actually left of
 * hard_deadline_ms rather than a flat poll_interval_ms (sd_ready.c) -- a
 * naive flat sleep there would let a check-then-sleep cycle overshoot by
 * up to one poll interval (or, from the executable-grace path, by up to
 * exec_grace_ms) before the next loop iteration got a chance to notice.
 * Deadlines and
 * poll_interval_ms are parameters, not baked-in constants, specifically so
 * sd_ready_test.c can use millisecond-scale fake deadlines and finish
 * instantly -- sd_ready_real.c passes the real production values (see its
 * own SD_READY_*_MS constants).
 *
 * partition_node/whole_disk_node: the two candidate device-node paths to
 * gate try_mount() attempts on (path_exists() checked before each attempt,
 * same as scanner.c's own pre-existing gating) -- parameters, not a
 * compiled-in constant, so sd_ready_test.c can use its own fake paths.
 *
 * exec_candidates/exec_candidate_count: paths checked via
 * probes->path_is_executable() once mounted -- ANY one being executable is
 * "ready" (an OR, matching this bootloader's own independent stock/update
 * alternates). exec_grace_ms bounds how much EXTRA time (beyond whatever
 * deadline was already in force) this waits specifically for one of those
 * paths to become accessible after a fresh mount, before giving up on the
 * executable specifically and returning stage=MOUNTED -- see sd_ready.c's
 * own comment on why this is deliberately small and separate from the
 * mount-readiness deadlines themselves (a card with no alternate player on
 * it at all, the overwhelmingly common case, must not pay any of the
 * longer mount-side deadlines just because no exec ever appears). */
sd_ready_result_t wait_for_sd_ready(const sd_ready_probes_t * probes, const char * partition_node,
                                     const char * whole_disk_node, const char * const * exec_candidates,
                                     int exec_candidate_count, int64_t short_deadline_ms,
                                     int64_t extended_deadline_ms, int64_t hard_deadline_ms, int poll_interval_ms,
                                     int64_t exec_grace_ms);

#endif /* BOOTLOADER_SD_READY_H */
