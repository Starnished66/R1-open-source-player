#include "sd_ready.h"

#include <string.h>

/* ---- The asynchronous MMC/partition race this whole state machine exists
 * to ride out ----
 *
 * On this firmware, mounting the SD card is fundamentally a race against
 * three independent, differently-paced kernel/userspace steps that all have
 * to land before a filesystem is actually usable: the MMC host controller
 * has to detect and enumerate the card on the bus, mdev has to react to the
 * resulting uevent and mknod() the block-device node(s) under /dev, and
 * only then can a `mount` attempt against that node possibly succeed at
 * all. A partitioned card publishes /dev/mmcblk0 and /dev/mmcblk0p1
 * separately (the whole-disk node first, in practice, though nothing here
 * assumes a strict order) and a "superfloppy" partition-less card only ever
 * publishes /dev/mmcblk0. Slow/unusual cards can stretch any one of these
 * steps well past what a normal card needs, so a single short fixed
 * timeout either wastes time on every normal boot or fails outright on a
 * slow one -- see scanner.c's own top-of-file comment for the real-device
 * reports that motivated replacing a flat 5s loop with this adaptive one.
 *
 * The algorithm below never assumes a particular arrival order for the two
 * device nodes, keeps retrying actual mount attempts throughout its whole
 * window (a node existing is necessary but not sufficient -- the
 * filesystem underneath it can still take another moment, or never mount
 * at all), and extends its own deadline only when there is real evidence
 * (sysfs, a node, or the mount itself) that something is still happening --
 * never unconditionally, so a genuinely absent or dead card still gives up
 * within the same short window this app has always used. */

static sd_ready_stage_t stage_from_evidence(bool mmc_evidence, bool whole_node, bool partition_node, bool mounted,
                                            bool executable_ready) {
    if (executable_ready) return SD_READY_STAGE_EXEC_READY;
    if (mounted) return SD_READY_STAGE_MOUNTED;
    if (whole_node || partition_node) return SD_READY_STAGE_NODE_PRESENT;
    if (mmc_evidence) return SD_READY_STAGE_MMC_EVIDENCE;
    return SD_READY_STAGE_NONE;
}

sd_ready_result_t wait_for_sd_ready(const sd_ready_probes_t * probes, const char * partition_node,
                                    const char * whole_disk_node, const char * const * exec_candidates,
                                    int exec_candidate_count, int64_t short_deadline_ms,
                                    int64_t extended_deadline_ms, int64_t hard_deadline_ms, int poll_interval_ms,
                                    int64_t exec_grace_ms) {
    sd_ready_result_t result;
    memset(&result, 0, sizeof(result));
    result.device_node_used = NULL;

    int64_t start = probes->monotonic_ms(probes->ctx);
    if (start < 0) {
        /* The clock itself is unusable -- there is no reliable way to
         * measure any of the three deadlines, and looping on a broken
         * clock risks either an instant busy-spin (every "elapsed" read
         * comes back the same bogus value) or an unbounded wait (it never
         * crosses a deadline it can't measure). Do exactly one best-effort
         * check and return immediately either way -- see wait_for_sd_ready()'s
         * own doc comment on monotonic_ms(); elapsed_ms stays negative so
         * the caller's logging can name this specific failure mode. */
        bool mounted = probes->mount_point_mounted(probes->ctx);
        bool exec_ready = false;
        if (mounted) {
            for (int i = 0; i < exec_candidate_count; i++) {
                if (probes->path_is_executable(probes->ctx, exec_candidates[i])) {
                    exec_ready = true;
                    break;
                }
            }
        }
        result.mounted = mounted;
        result.executable_ready = exec_ready;
        result.stage = stage_from_evidence(false, false, false, mounted, exec_ready);
        result.elapsed_ms = -1;
        return result;
    }

    /* Captured once, the first time `elapsed` crosses short_deadline_ms --
     * compared against the stage reached by the time `elapsed` crosses
     * extended_deadline_ms, below, to decide whether the extra time between
     * those two checkpoints was actually spent making progress. */
    sd_ready_stage_t stage_at_short_checkpoint = SD_READY_STAGE_NONE;
    bool short_checkpoint_done = false;
    bool extended_checkpoint_done = false;

    /* -1 until the first successful mount; used only to bound the small,
     * separate exec_grace_ms window below -- see this parameter's own doc
     * comment in sd_ready.h for why a missing executable does not, by
     * itself, ever extend the mount-side deadlines above. */
    int64_t mounted_at_ms = -1;

    for (;;) {
        int64_t now = probes->monotonic_ms(probes->ctx);
        if (now < 0) {
            /* The clock stopped being trustworthy partway through --
             * same reasoning as the start-of-function check above: stop
             * here with whatever was already established rather than
             * continuing to loop against an unmeasurable deadline. */
            result.elapsed_ms = -1;
            return result;
        }
        int64_t elapsed = now - start;

        bool mounted = probes->mount_point_mounted(probes->ctx);
        if (mounted) {
            if (mounted_at_ms < 0) mounted_at_ms = elapsed;
            result.mounted = true;

            for (int i = 0; i < exec_candidate_count; i++) {
                if (probes->path_is_executable(probes->ctx, exec_candidates[i])) {
                    result.executable_ready = true;
                    result.elapsed_ms = elapsed;
                    result.stage = SD_READY_STAGE_EXEC_READY;
                    return result;
                }
            }

            /* Mounted, but no candidate executable yet -- worth a short,
             * separate grace window (not the full remaining mount-side
             * deadline) in case a slow card's directory metadata just
             * hasn't settled yet immediately after mount. A card that
             * genuinely has no alternate player on it -- the overwhelming
             * common case -- gives up here almost immediately rather than
             * riding out whatever long deadline may have been granted for
             * the MOUNT itself. */
            if (elapsed - mounted_at_ms >= exec_grace_ms) {
                result.elapsed_ms = elapsed;
                result.stage = SD_READY_STAGE_MOUNTED;
                return result;
            }
        } else {
            /* Gate each candidate node on its own existence first -- an
             * attempt against a node that doesn't exist yet is pure wasted
             * work, and this loop already retries on its own schedule
             * regardless (see try_mount()'s own doc comment). Partition
             * node first, then whole-disk, matching this project's
             * existing mount-candidate order. */
            if (probes->path_exists(probes->ctx, partition_node)) {
                result.saw_partition_node = true;
                /* Re-measured right before the call (not reusing `elapsed`
                 * from the top of this iteration) -- a real try_mount() can
                 * itself consume real wall-clock time (see its own doc
                 * comment in sd_ready.h), so this must reflect whatever is
                 * ACTUALLY left of the hard deadline at the moment of the
                 * call, not what was left when this iteration started. */
                int64_t before = probes->monotonic_ms(probes->ctx);
                int64_t remaining = hard_deadline_ms - (before >= 0 ? before - start : elapsed);
                probes->try_mount(probes->ctx, partition_node, remaining);
                if (probes->mount_point_mounted(probes->ctx)) {
                    result.device_node_used = partition_node;
                    continue; /* re-enter the loop; the `mounted` branch above handles the rest */
                }
            }
            if (probes->path_exists(probes->ctx, whole_disk_node)) {
                result.saw_whole_node = true;
                int64_t before = probes->monotonic_ms(probes->ctx);
                int64_t remaining = hard_deadline_ms - (before >= 0 ? before - start : elapsed);
                probes->try_mount(probes->ctx, whole_disk_node, remaining);
                if (probes->mount_point_mounted(probes->ctx)) {
                    result.device_node_used = whole_disk_node;
                    continue;
                }
            }
            if (!result.saw_whole_node && !result.saw_partition_node && probes->mmc_evidence_present(probes->ctx)) {
                result.saw_mmc_evidence = true;
            }
        }

        sd_ready_stage_t current_stage = stage_from_evidence(result.saw_mmc_evidence, result.saw_whole_node,
                                                              result.saw_partition_node, result.mounted,
                                                              result.executable_ready);

        if (elapsed >= hard_deadline_ms) {
            result.elapsed_ms = elapsed;
            result.stage = current_stage;
            return result;
        }

        if (elapsed >= extended_deadline_ms && !extended_checkpoint_done) {
            extended_checkpoint_done = true;
            if (current_stage <= stage_at_short_checkpoint) {
                /* No further progress since the short-deadline checkpoint --
                 * whatever got us past the short deadline never developed
                 * into anything more. Stop now rather than riding out the
                 * rest of the hard-deadline budget for nothing. */
                result.elapsed_ms = elapsed;
                result.stage = current_stage;
                return result;
            }
        }

        if (elapsed >= short_deadline_ms && !short_checkpoint_done) {
            short_checkpoint_done = true;
            stage_at_short_checkpoint = current_stage;
            if (current_stage == SD_READY_STAGE_NONE) {
                /* No evidence at all by the short deadline -- same terminal
                 * timing this app has always used for a genuinely absent
                 * card; extending further would only delay boot for
                 * something with no sign of ever arriving. */
                result.elapsed_ms = elapsed;
                result.stage = current_stage;
                return result;
            }
        }

        /* Clamped to whatever's actually left of the hard deadline -- a
         * flat poll_interval_ms sleep here would let a check-then-sleep
         * cycle overshoot hard_deadline_ms by up to one full poll interval
         * (or, from inside the mounted/exec-grace path above, by up to
         * exec_grace_ms too): the `elapsed >= hard_deadline_ms` check
         * above only gets another chance to fire on the NEXT iteration's
         * top-of-loop clock read, after whatever this sleeps for has
         * already elapsed. Since the check above already confirmed
         * `elapsed < hard_deadline_ms` (or this line would never be
         * reached), `hard_deadline_ms - elapsed` here is always positive. */
        int64_t time_left_to_hard = hard_deadline_ms - elapsed;
        int sleep_ms = (time_left_to_hard < poll_interval_ms) ? (int) time_left_to_hard : poll_interval_ms;
        probes->wait_ms(probes->ctx, sleep_ms);
    }
}
