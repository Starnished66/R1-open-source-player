/* Real on-device probe implementations for sd_ready.h's wait_for_sd_ready()
 * -- everything in this file touches actual files, mounts, subprocesses, or
 * a real clock, and is therefore never exercised by sd_ready_test.c (which
 * links only sd_ready.c against fake probes). scanner.c calls only
 * mount_sd_card_if_needed() below; everything else here is file-local. */
#include "sd_ready.h"
#include "scanner.h"
#include "subprocess.h"

#include <dirent.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ---- Timing policy ----
 *
 * SD_READY_SHORT_DEADLINE_MS matches this project's own long-established
 * normal-case bound (previously a flat SD_SETTLE_MAX_ATTEMPTS x
 * SD_SETTLE_DELAY_MS = 50 x 100ms = 5000ms loop) -- a card that is going to
 * mount at all overwhelmingly does so well inside this window, and a
 * genuinely absent card is given up on here exactly as before, with no
 * regression to the already-accepted no-card boot time.
 *
 * SD_READY_EXTENDED_DEADLINE_MS/SD_READY_HARD_DEADLINE_MS are new: reached
 * only when wait_for_sd_ready() has real evidence (sysfs, a device node, or
 * the mount itself) that something is still happening -- see sd_ready.c's
 * own top comment for exactly how that evidence gates each extension. A
 * card with zero evidence by the short deadline never reaches either of
 * these; boot is not slowed down for it.
 *
 * KNOWN, DELIBERATELY ACCEPTED LIMITATION: real_mmc_evidence_present()'s
 * sysfs `type` check (below) can only ever see a card AFTER the MMC core
 * has finished enumerating/identifying it -- a card whose own enumeration
 * (not mounting, not mdev publishing a node, the actual bus-level
 * identification step before either of those) takes longer than
 * SD_READY_SHORT_DEADLINE_MS is indistinguishable from no card at all, and
 * this build gives up at the short deadline exactly as before. Fixing this
 * fully would need either a physical card-detect signal (no generic,
 * safe-to-assume sysfs attribute for this exists across arbitrary MMC host
 * drivers, and there is no live device on hand to identify one specific to
 * this SoC) or raising the unconditional short deadline itself, which
 * would slow every card-absent boot by the same amount. This was evaluated
 * and the short deadline was deliberately kept unchanged -- see
 * sd_ready_test.c's own test_enumeration_slower_than_short_deadline_known_limitation()
 * for a test that locks this decision in rather than leaving it an
 * unnoticed gap. */
#define SD_READY_POLL_INTERVAL_MS 100
#define SD_READY_SHORT_DEADLINE_MS 5000
#define SD_READY_EXTENDED_DEADLINE_MS 15000
#define SD_READY_HARD_DEADLINE_MS 20000

/* Bounds how much EXTRA time this waits specifically for a candidate
 * executable to appear after a fresh mount, separate from (and much
 * smaller than) the mount-side deadlines above -- see sd_ready.h's own doc
 * comment on wait_for_sd_ready()'s exec_grace_ms parameter for why a card
 * with no alternate player on it (the common case) must not pay any of the
 * longer deadlines just because no exec ever appears. 300ms is three poll
 * ticks -- enough to ride out a directory-metadata settle immediately after
 * mount, not enough to be noticeable at boot. */
#define SD_READY_EXEC_GRACE_MS 300

/* True if a real filesystem is mounted at SD_MOUNT_POINT (its st_dev
 * differs from its parent's), as opposed to just an empty directory sitting
 * there -- same check as src/main.c's own sd_mount_point_mounted(), against
 * the fully-resolved path (see scanner.h's own comment on why). */
static bool real_mount_point_mounted(void * ctx) {
    (void) ctx;
    struct stat parent_st, mnt_st;
    if (stat("/usr/data/mnt", &parent_st) != 0) return false;
    if (stat(SD_MOUNT_POINT, &mnt_st) != 0) return false;
    return parent_st.st_dev != mnt_st.st_dev;
}

static bool real_path_exists(void * ctx, const char * path) {
    (void) ctx;
    return access(path, F_OK) == 0;
}

static bool real_path_is_executable(void * ctx, const char * path) {
    (void) ctx;
    struct stat st;
    return path && path[0] && stat(path, &st) == 0 && S_ISREG(st.st_mode) && access(path, X_OK) == 0;
}

/* Bounds a single subprocess_run()-class call to whatever's left of the
 * caller's own overall deadline -- subprocess_run() itself defaults to a
 * 15s-per-command timeout (SUBPROCESS_TIMEOUT_MS, subprocess.c), which is
 * completely invisible to wait_for_sd_ready()'s own hard-deadline math: a
 * single hung `mount`/`ntfs-3g` invocation could otherwise burn up to 15s
 * on its own, and real_try_mount() below makes up to three such calls per
 * node -- more than enough to blow straight through the 20s hard ceiling
 * regardless of what the state machine thinks its deadline is. Capped at
 * SD_READY_MOUNT_ATTEMPT_TIMEOUT_MS too, not just clamped to whatever's
 * left, so a single fs-type probe can't eat the entire remaining budget
 * and starve the other two -- returns 0 (meaning "don't even try") once
 * the budget is exhausted. */
#define SD_READY_MOUNT_ATTEMPT_TIMEOUT_MS 3000

static int bounded_attempt_timeout_ms(int64_t deadline_remaining_ms) {
    if (deadline_remaining_ms <= 0) return 0;
    if (deadline_remaining_ms > SD_READY_MOUNT_ATTEMPT_TIMEOUT_MS) return SD_READY_MOUNT_ATTEMPT_TIMEOUT_MS;
    return (int) deadline_remaining_ms;
}

/* Same three-filesystem-in-turn attempt as src/main.c's own
 * try_mount_sd_device_node() -- ntfs-3g over vfat/exfat is not something
 * this bootloader has any independent basis for; it's copied because the
 * real player's own comment there explains a real, already-debugged reason
 * (no usable in-kernel ntfs path on this kernel/BusyBox combination) that
 * still applies here.
 *
 * `deadline_remaining_ms` is wait_for_sd_ready()'s own remaining time
 * before its hard deadline (sd_ready.h's try_mount() doc comment) -- each
 * of the three subprocess_run_timeout() calls below re-measures its own
 * elapsed time and re-clamps against whatever's ACTUALLY left, so the
 * three attempts combined can never exceed the budget the caller handed
 * in, only ever spend less. A node whose mount command genuinely hangs on
 * every fs type this bounds to at most deadline_remaining_ms total, not
 * three independent 15s (or even 3s) timeouts stacked back to back. */
static void real_try_mount(void * ctx, const char * device_node, int64_t deadline_remaining_ms) {
    struct timespec t0;
    if (clock_gettime(CLOCK_MONOTONIC, &t0) != 0) {
        /* Can't measure elapsed time at all -- an unchecked failure here
         * would leave t0 uninitialized and every timeout computed below
         * undefined, exactly the class of bug this function exists to
         * avoid (a mount attempt with no real bound). Skip this attempt
         * entirely rather than guess; the caller's own outer loop retries
         * on its next poll tick regardless, and wait_for_sd_ready()'s own
         * monotonic_ms() check will stop the whole wait shortly if the
         * clock is genuinely, persistently broken. */
        return;
    }

    for (int fs = 0; fs < 3; fs++) {
        struct timespec now;
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return; /* same reasoning -- stop rather than compute from garbage */
        int64_t spent_ms =
            (now.tv_sec - t0.tv_sec) * 1000L + (now.tv_nsec - t0.tv_nsec) / 1000000L;
        int timeout_ms = bounded_attempt_timeout_ms(deadline_remaining_ms - spent_ms);
        if (timeout_ms <= 0) return; /* no budget left -- stop trying further fs types */

        if (fs == 0) {
            char * vfat_argv[] = {
                (char *) "mount", (char *) "-t", (char *) "vfat", (char *) "-o",
                (char *) "rw,relatime,fmask=0022,dmask=0022,codepage=936,iocharset=utf8,shortname=mixed",
                (char *) device_node, (char *) SD_MOUNT_POINT, NULL };
            subprocess_run_timeout(vfat_argv, NULL, 0, timeout_ms);
        } else if (fs == 1) {
            char * exfat_argv[] = { (char *) "mount", (char *) "-t", (char *) "exfat", (char *) "-o",
                                     (char *) "rw,relatime", (char *) device_node, (char *) SD_MOUNT_POINT, NULL };
            subprocess_run_timeout(exfat_argv, NULL, 0, timeout_ms);
        } else {
            char * ntfs_argv[] = { (char *) "/usr/bin/ntfs-3g", (char *) "-o",
                                    (char *) "rw,relatime,big_writes,umask=0022",
                                    (char *) device_node, (char *) SD_MOUNT_POINT, NULL };
            subprocess_run_timeout(ntfs_argv, NULL, 0, timeout_ms);
        }
        if (real_mount_point_mounted(ctx)) return;
    }
}

/* Reads /sys/class/mmc_host/<host>/<child>/type, which the Linux MMC core
 * exposes for every enumerated card (drivers/mmc/core/bus.c's type_show(),
 * a long-standing, stable attribute -- not a HiBy-specific or guessed
 * path) and returns whether it reads exactly "SD". A real SD/SDHC/SDXC
 * card reports "SD"; eMMC reports "MMC"; an SDIO card (e.g. an onboard
 * Wi-Fi chip riding the same MMC subsystem) reports "SDIO" and, unlike
 * either memory-card type, never has this attribute mean the same thing a
 * plain read would suggest without checking the string. Returns false --
 * never guesses -- if the attribute can't be read at all. */
static bool child_reports_sd_type(const char * host_path, const char * child_name) {
    char type_path[700]; /* generous vs. host_path's own 300 + an arbitrary child_name (NAME_MAX=255) + "/type" */
    snprintf(type_path, sizeof(type_path), "%s/%s/type", host_path, child_name);
    FILE * f = fopen(type_path, "r");
    if (!f) return false;

    char buf[16];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0) return false;
    buf[n] = '\0';

    char * newline = strchr(buf, '\n');
    if (newline) *newline = '\0';
    return strcmp(buf, "SD") == 0;
}

/* Evidence a removable SD card has been detected by some MMC host,
 * earlier/weaker than a /dev block node existing -- the node itself only
 * appears once mdev reacts to the resulting uevent and mknod()s it
 * (scanner.c's own top comment), one step later than the kernel enumerating
 * the card and registering its mmcN:xxxx device object in sysfs. Without
 * this, a card whose enumeration alone (independent of mount, independent
 * of mdev) takes longer than the short deadline was indistinguishable from
 * no card at all -- the two device-node checks alone can only ever notice
 * evidence that already implies a node exists.
 *
 * Gated on child_reports_sd_type() above specifically because an earlier
 * version of this probe accepted ANY "mmcN:xxxx" child as evidence, which
 * a permanently-attached eMMC or SDIO Wi-Fi chip (either would enumerate
 * and appear here on every single boot, with or without an SD card
 * inserted) would satisfy unconditionally -- silently regressing every
 * no-card boot from the short 5s deadline to the extended 15s one. The
 * "type" check is standard, well-documented MMC-core kernel behavior, but
 * this exact sysfs layout and attribute value have NOT been confirmed on
 * this specific device/kernel -- real-device verification (read every
 * "type" file under /sys/class/mmc_host, one host/child directory level at
 * a time, with the SD card both inserted and removed) is still needed. If
 * the attribute is ever absent or spelled
 * differently than expected here, this simply under-reports (falls back to
 * "no evidence", i.e. today's plain 5s-then-give-up behavior) rather than
 * over-reporting -- the safe direction to be wrong in, unlike the earlier
 * any-child version. */
static bool real_mmc_evidence_present(void * ctx) {
    (void) ctx;
    DIR * hosts = opendir("/sys/class/mmc_host");
    if (!hosts) return false;

    bool found = false;
    struct dirent * host;
    while (!found && (host = readdir(hosts)) != NULL) {
        if (host->d_name[0] == '.') continue;

        char host_path[300];
        snprintf(host_path, sizeof(host_path), "/sys/class/mmc_host/%s", host->d_name);
        DIR * host_dir = opendir(host_path);
        if (!host_dir) continue;

        size_t host_name_len = strlen(host->d_name);
        struct dirent * child;
        while ((child = readdir(host_dir)) != NULL) {
            if (strncmp(child->d_name, host->d_name, host_name_len) == 0 && child->d_name[host_name_len] == ':' &&
                child_reports_sd_type(host_path, child->d_name)) {
                found = true;
                break;
            }
        }
        closedir(host_dir);
    }
    closedir(hosts);
    return found;
}

static int64_t real_monotonic_ms(void * ctx) {
    (void) ctx;
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return -1;
    return (int64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ctx for the two probes above that need real per-run state -- currently
 * just the optional inotify fd (-1 whenever inotify itself, or watching
 * /dev specifically, isn't available; see mount_sd_card_if_needed() below).
 * Deliberately minimal: everything else real_*() needs is either a global
 * constant (SD_MOUNT_POINT) or passed as a plain argument. */
typedef struct {
    int inotify_fd;
} real_probe_ctx_t;

/* Waits up to `ms`, woken early by any /dev inotify event when available --
 * see sd_ready.h's own wait_ms() doc comment for why the caller never
 * depends on this actually firing (a plain timed poll() with no fds, which
 * this degrades to when inotify_fd < 0, is just as correct, only less
 * immediately responsive to a node appearing mid-interval). poll(2)
 * explicitly supports nfds == 0 as a plain timed wait, so this is one code
 * path either way, not two. */
static void real_wait_ms(void * ctx_v, int ms) {
    real_probe_ctx_t * ctx = ctx_v;
    if (ms <= 0) return;

    /* Zero-initialized even though only meaningfully populated in the
     * nfds==1 branch below -- with nfds==0, poll(2) is only ever specified
     * to return 0 (timeout) or -1 (error), never a positive count, so
     * `pfd.revents` is never actually read in that case (see the
     * short-circuited `nfds && ...` check below); zeroing it anyway costs
     * nothing and keeps that guarantee visible to static analysis too,
     * rather than relying on a cross-variable invariant it can't see. */
    struct pollfd pfd = { 0 };
    int nfds = 0;
    if (ctx->inotify_fd >= 0) {
        pfd.fd = ctx->inotify_fd;
        pfd.events = POLLIN;
        nfds = 1;
    }

    /* have_clock==false (clock_gettime() itself failed) means the EINTR
     * retry math below can't be trusted -- computing "elapsed" from an
     * uninitialized wait_start would be undefined. Fall back to a single
     * flat poll() with no bounded retry in that case: an EINTR then just
     * ends the wait early rather than risking a loop with no reliable way
     * to know how much time is actually left. */
    struct timespec wait_start;
    bool have_clock = clock_gettime(CLOCK_MONOTONIC, &wait_start) == 0;
    int remaining = ms;

    for (;;) {
        int rc = poll(nfds ? &pfd : NULL, (nfds_t) nfds, remaining);
        if (rc < 0) {
            if (errno == EINTR && have_clock) {
                /* An unrelated signal interrupted the wait -- retry with
                 * whatever time is actually left, rather than either
                 * returning immediately (which the caller would otherwise
                 * have no way to distinguish from a real event/timeout) or
                 * restarting the full `ms` (which could stack into an
                 * effectively unbounded wait under a busy signal source).
                 * wait_for_sd_ready() re-derives its own elapsed time from
                 * monotonic_ms() on every loop iteration regardless, so
                 * this only needs to avoid a tight re-spin, not guarantee
                 * an exact duration. */
                struct timespec now;
                if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return; /* clock stopped mid-wait -- same reasoning as above */
                long elapsed_ms = (now.tv_sec - wait_start.tv_sec) * 1000L +
                                   (now.tv_nsec - wait_start.tv_nsec) / 1000000L;
                remaining = ms - (int) elapsed_ms;
                if (remaining <= 0) return;
                continue;
            }
            return; /* EINTR with no working clock, or some other poll() failure -- nothing more to usefully do than stop waiting */
        }
        if (rc > 0 && nfds && (pfd.revents & POLLIN)) {
            /* Drain every pending event -- the caller re-checks all
             * readiness state from scratch on its next loop iteration
             * regardless of which specific name changed, so nothing here
             * needs to actually parse an inotify_event. Not draining would
             * leave the fd permanently "readable", turning every future
             * call here into a zero-wait busy loop instead of a real
             * wait. */
            char buf[512];
            while (read(ctx->inotify_fd, buf, sizeof(buf)) > 0) { }
        }
        return; /* event arrived, or the timeout elapsed -- either way, let the caller re-check */
    }
}

/* One line per terminal outcome (requirement: distinguish these in
 * logging) -- derived from the same evidence wait_for_sd_ready() already
 * collected, not a separate re-probe. Printed to stderr, same convention
 * every other bootloader diagnostic in this file uses (no dedicated log
 * file the way the real player's boot_checkpoint() has). */
static void log_sd_ready_outcome(const sd_ready_result_t * r) {
    const char * prefix = "open_hiby_bootloader: SD readiness";

    if (r->elapsed_ms < 0) {
        fprintf(stderr, "%s: monotonic clock unavailable -- proceeded with a single best-effort check "
                        "(mounted=%d, executable_ready=%d)\n",
                prefix, r->mounted, r->executable_ready);
        return;
    }

    if (r->executable_ready) {
        fprintf(stderr, "%s: ready after %lldms via %s\n", prefix, (long long) r->elapsed_ms,
                r->device_node_used ? r->device_node_used : "(unknown device)");
    } else if (r->mounted) {
        fprintf(stderr, "%s: filesystem mounted after %lldms (via %s) but no configured executable was found\n",
                prefix, (long long) r->elapsed_ms, r->device_node_used ? r->device_node_used : "(unknown device)");
    } else if (r->saw_whole_node || r->saw_partition_node) {
        if (r->saw_partition_node) {
            fprintf(stderr, "%s: device node present after %lldms but the filesystem never mounted\n", prefix,
                    (long long) r->elapsed_ms);
        } else {
            fprintf(stderr,
                    "%s: whole-disk node present after %lldms but no partition node ever appeared and the "
                    "filesystem never mounted\n",
                    prefix, (long long) r->elapsed_ms);
        }
    } else if (r->saw_mmc_evidence) {
        fprintf(stderr, "%s: MMC host reported a card after %lldms but no block-device node ever appeared\n", prefix,
                (long long) r->elapsed_ms);
    } else {
        fprintf(stderr, "%s: no MMC/card evidence after %lldms\n", prefix, (long long) r->elapsed_ms);
    }
}

/* Mirrors src/main.c's own mount_sd_card_if_needed() (mount point creation,
 * already-mounted short-circuit, partition-node-then-whole-disk fallback
 * for a partition-less "superfloppy" card) -- see scanner.h's own doc
 * comment for why a bootloader needs its own copy of this at all. Now backed
 * by wait_for_sd_ready() (sd_ready.c) instead of a flat fixed-attempt loop --
 * see that file's own top comment for the algorithm, and the
 * SD_READY_*_MS constants above for this build's actual timing policy. */
void mount_sd_card_if_needed(void) {
    mkdir("/usr/data/mnt", 0755);
    mkdir(SD_MOUNT_POINT, 0755);

    real_probe_ctx_t ctx;
    ctx.inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (ctx.inotify_fd >= 0 && inotify_add_watch(ctx.inotify_fd, "/dev", IN_CREATE) < 0) {
        /* /dev itself somehow not watchable -- fall back to plain polling,
         * same as inotify_init1() failing outright below. Either way this
         * is purely a responsiveness optimization: real_wait_ms() above
         * degrades cleanly to a plain timed poll() with inotify_fd < 0, so
         * nothing here needs to be treated as fatal or even logged as more
         * than informational. */
        close(ctx.inotify_fd);
        ctx.inotify_fd = -1;
    }

    sd_ready_probes_t probes = {
        .ctx = &ctx,
        .path_exists = real_path_exists,
        .mount_point_mounted = real_mount_point_mounted,
        .try_mount = real_try_mount,
        .mmc_evidence_present = real_mmc_evidence_present,
        .path_is_executable = real_path_is_executable,
        .monotonic_ms = real_monotonic_ms,
        .wait_ms = real_wait_ms,
    };

    const char * exec_candidates[] = { SD_STOCK_PLAYER_PATH, SD_UPDATE_PLAYER_PATH };

    sd_ready_result_t result =
        wait_for_sd_ready(&probes, SD_DEVICE_NODE_PARTITION, SD_DEVICE_NODE_WHOLE_DISK, exec_candidates, 2,
                          SD_READY_SHORT_DEADLINE_MS, SD_READY_EXTENDED_DEADLINE_MS, SD_READY_HARD_DEADLINE_MS,
                          SD_READY_POLL_INTERVAL_MS, SD_READY_EXEC_GRACE_MS);

    if (ctx.inotify_fd >= 0) close(ctx.inotify_fd);

    log_sd_ready_outcome(&result);
}
