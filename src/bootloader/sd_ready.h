#ifndef BOOTLOADER_SD_READY_H
#define BOOTLOADER_SD_READY_H

#include <stdbool.h>
#include <stdint.h>

/* Dependency-injected SD card readiness state machine interface.
 * Implements hardware-independent readiness detection using pluggable probe callbacks. */

/* Callbacks for probing filesystem, device, and timing state. */
typedef struct {
    void * ctx;

    /* True if path exists. Must not block. */
    bool (*path_exists)(void * ctx, const char * path);

    /* True if SD mount point currently has a filesystem mounted. */
    bool (*mount_point_mounted)(void * ctx);

    /* Attempts to mount device_node at the SD mount point.
     * deadline_remaining_ms bounds any blocking operations to the remaining time limit. */
    void (*try_mount)(void * ctx, const char * device_node, int64_t deadline_remaining_ms);

    /* Probes sysfs for evidence of an attached MMC/SD card before block device nodes appear. */
    bool (*mmc_evidence_present)(void * ctx);

    /* True if path exists, is a regular file, and is executable. */
    bool (*path_is_executable)(void * ctx, const char * path);

    /* Current monotonic clock in milliseconds, or negative on clock error. */
    int64_t (*monotonic_ms)(void * ctx);

    /* Waits up to ms milliseconds or until woken by device events. Handles EINTR internally. */
    void (*wait_ms)(void * ctx, int ms);
} sd_ready_probes_t;

/* Readiness milestones used to decide deadline extensions and classify outcome. */
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

/* Mounts the SD card at SD_MOUNT_POINT if present, waiting for media readiness. */
void mount_sd_card_if_needed(void);

/* Runs the readiness state machine to completion, polling device nodes and mounting
 * within configured deadlines. Once mounted, waits up to exec_grace_ms for any
 * candidate executable in exec_candidates to become accessible. */
sd_ready_result_t wait_for_sd_ready(const sd_ready_probes_t * probes, const char * partition_node,
                                     const char * whole_disk_node, const char * const * exec_candidates,
                                     int exec_candidate_count, int64_t short_deadline_ms,
                                     int64_t extended_deadline_ms, int64_t hard_deadline_ms, int poll_interval_ms,
                                     int64_t exec_grace_ms);

#endif /* BOOTLOADER_SD_READY_H */
