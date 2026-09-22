#ifndef SD_CARD_IDENTITY_H
#define SD_CARD_IDENTITY_H

#include <errno.h>
#include <stdbool.h>
#include <sys/types.h>
#include <string.h>

/* A mounted sample replaces the stored card only when both device numbers
 * were observed and differ, or both CID strings are non-empty and differ.
 * A failed device reread is not a new device: pass have_current_device
 * false. A missing stored device is not a swap either. An empty CID read
 * is uncertain: it must not set a handoff or cancel a scan by itself. */
static inline bool sd_card_identity_replaced(bool mounted, bool was_mounted,
                                            bool have_last_identity,
                                            bool have_last_device, dev_t last_device,
                                            bool have_current_device, dev_t current_device,
                                            const char *last_cid, const char *current_cid) {
    if (!mounted || !was_mounted || !have_last_identity) return false;
    if (have_last_device && have_current_device && last_device != current_device) return true;
    return last_cid && last_cid[0] && current_cid && current_cid[0] &&
           strcmp(last_cid, current_cid) != 0;
}

/* A mounted sample can rearm identity tracking with either a device number or
 * a non-empty CID. An empty CID alone is an unknown sample and must not make
 * an old CID eligible for comparison on the next poll. */
static inline bool sd_card_identity_is_known(bool have_device, const char *cid) {
    return have_device || (cid && cid[0]);
}

/* A retry that finds an attached mount is not enough to declare a stale
 * mount: only a mount observed by the same sample and then disproved by its
 * kernel probe may bypass absence debounce. */
static inline bool sd_card_stale_mount_confirmed(bool mount_observed, bool mounted) {
    return mount_observed && !mounted;
}

/* Commit the mount-device baseline for a sample that is keeping this card.
 * Call after this sample's handoff has cleared was_mounted, and before
 * was_mounted is set for the next sample. Do not call on the uncertain-CID,
 * rescan, or unmounted returns — those must keep the previous baseline.
 * A failed reread must not store a zero device over a known one. When the
 * reread failed and this sample replaced the card, or this generation was
 * not already mounted, the old baseline is dropped so the next successful
 * read starts a new baseline instead of looking like another swap.
 * Returns whether a device baseline is known afterwards. */
static inline bool sd_card_remember_mounted_device(bool have_current_device, dev_t current_device,
                                                  bool replaced, bool was_mounted,
                                                  dev_t * last_device, bool have_last_device) {
    if (have_current_device) {
        *last_device = current_device;
        return true;
    }
    if (replaced || !was_mounted) return false;
    return have_last_device;
}

/* Result of reading one sysfs device node. UNKNOWN is an open or parse
 * failure, not evidence that the card was removed. */
typedef enum {
    SD_PROBE_MATCH = 0,
    SD_PROBE_DIFFERENT,
    SD_PROBE_ABSENT,
    SD_PROBE_UNKNOWN
} sd_probe_result_t;

/* True only when both probes resolved and neither is the mounted device.
 * A match, or a read we could not finish, is not proof of removal. */
static inline bool sd_card_probes_confirm_removed(sd_probe_result_t partition,
                                                 sd_probe_result_t disk) {
    if (partition == SD_PROBE_MATCH || disk == SD_PROBE_MATCH) return false;
    if (partition == SD_PROBE_UNKNOWN || disk == SD_PROBE_UNKNOWN) return false;
    return true;
}

/* stat() of the partition node and the whole-disk node. Confirmed absent
 * only when both are ENOENT. Any other errno leaves the question open. */
static inline bool sd_card_sysfs_stats_confirm_absent(int partition_rc, int partition_errno,
                                                     int disk_rc, int disk_errno) {
    if (partition_rc == 0 || disk_rc == 0) return false;
    return partition_errno == ENOENT && disk_errno == ENOENT;
}

/* Stale/forced removal confirms immediately. A plain miss confirms only
 * after absence_streak reaches the caller's threshold (the hotplug poll
 * passes the streak after counting this sample). */
static inline bool sd_card_absence_confirmed(bool stale_or_forced, int absence_streak,
                                            int confirm_threshold) {
    if (stale_or_forced) return true;
    return confirm_threshold > 0 && absence_streak >= confirm_threshold;
}

/* Sets the destructive handoff latch. Never clears it: a later sample of
 * the same card must not drop a removal or replacement that was already
 * confirmed. An unconfirmed miss passes removal_confirmed false. */
static inline bool sd_card_handoff_after_sample(bool handoff_pending, bool removal_confirmed,
                                                bool replaced) {
    return handoff_pending || removal_confirmed || replaced;
}

static inline bool sd_card_sample_cancels_scan(bool mounted, bool replaced, bool rescan_active) {
    return rescan_active && (!mounted || replaced);
}

#endif
