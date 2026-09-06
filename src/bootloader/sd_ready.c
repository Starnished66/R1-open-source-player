#include "sd_ready.h"

#include <string.h>

/* Handles SD card detection and mounting. Polls for device node creation,
 * retries mounting candidates, and extends timeout deadlines when detection
 * evidence (MMC sysfs entry or block device node) is observed. */

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
        /* Perform a single best-effort check if monotonic clock is unavailable. */
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

    /* Record stage at short deadline to determine if progress was made by extended deadline. */
    sd_ready_stage_t stage_at_short_checkpoint = SD_READY_STAGE_NONE;
    bool short_checkpoint_done = false;
    bool extended_checkpoint_done = false;

    /* Timestamp when mount succeeded, used to bound executable grace window. */
    int64_t mounted_at_ms = -1;

    for (;;) {
        int64_t now = probes->monotonic_ms(probes->ctx);
        if (now < 0) {
            /* Stop waiting if clock becomes unavailable during polling. */
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
            /* Attempt mounting partition node first, then whole-disk node if present. */
            if (probes->path_exists(probes->ctx, partition_node)) {
                result.saw_partition_node = true;
                /* Compute remaining deadline time for mount attempt. */
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
                /* Stop if no stage advancement occurred between checkpoints. */
                result.elapsed_ms = elapsed;
                result.stage = current_stage;
                return result;
            }
        }

        if (elapsed >= short_deadline_ms && !short_checkpoint_done) {
            short_checkpoint_done = true;
            stage_at_short_checkpoint = current_stage;
            if (current_stage == SD_READY_STAGE_NONE) {
                /* Exit if no card evidence observed before short deadline. */
                result.elapsed_ms = elapsed;
                result.stage = current_stage;
                return result;
            }
        }

        /* Sleep until next poll interval, bounded by remaining hard deadline. */
        int64_t time_left_to_hard = hard_deadline_ms - elapsed;
        int sleep_ms = (time_left_to_hard < poll_interval_ms) ? (int) time_left_to_hard : poll_interval_ms;
        probes->wait_ms(probes->ctx, sleep_ms);
    }
}
