/* Real on-device probe implementations for wait_for_sd_ready(). */
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

/* Timing thresholds for SD card readiness detection. Short deadline is used
 * when no card evidence is detected; extended and hard deadlines apply when
 * MMC sysfs activity or device nodes are observed. */
#define SD_READY_POLL_INTERVAL_MS 100
#define SD_READY_SHORT_DEADLINE_MS 5000
#define SD_READY_EXTENDED_DEADLINE_MS 15000
#define SD_READY_HARD_DEADLINE_MS 20000

/* Grace period to wait for candidate executables after mounting. */
#define SD_READY_EXEC_GRACE_MS 300

/* Returns true if a filesystem is mounted at SD_MOUNT_POINT. */
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
    return scanner_path_is_executable(path);
}

/* Per-command timeout cap for individual filesystem mount invocations. */
#define SD_READY_MOUNT_ATTEMPT_TIMEOUT_MS 3000

static int bounded_attempt_timeout_ms(int64_t deadline_remaining_ms) {
    if (deadline_remaining_ms <= 0) return 0;
    if (deadline_remaining_ms > SD_READY_MOUNT_ATTEMPT_TIMEOUT_MS) return SD_READY_MOUNT_ATTEMPT_TIMEOUT_MS;
    return (int) deadline_remaining_ms;
}

/* Attempts to mount device_node at SD_MOUNT_POINT using vfat, exfat, and ntfs-3g in sequence.
 * Timeouts are bounded by deadline_remaining_ms. */
static void real_try_mount(void * ctx, const char * device_node, int64_t deadline_remaining_ms) {
    struct timespec t0;
    if (clock_gettime(CLOCK_MONOTONIC, &t0) != 0) {
        /* Skip attempt if monotonic clock is unavailable. */
        return;
    }

    for (int fs = 0; fs < 3; fs++) {
        struct timespec now;
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return;
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

/* Reads the MMC core sysfs "type" attribute for a detected card, returning
 * true only if it matches "SD" (filtering out MMC and SDIO devices). */
static bool child_reports_sd_type(const char * host_path, const char * child_name) {
    char type_path[700];
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

/* Checks /sys/class/mmc_host for an enumerated MMC device reporting type "SD". */
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

/* Context structure holding runtime probe state (inotify file descriptor). */
typedef struct {
    int inotify_fd;
} real_probe_ctx_t;

/* Waits up to ms milliseconds, returning early on inotify events from /dev. */
static void real_wait_ms(void * ctx_v, int ms) {
    real_probe_ctx_t * ctx = ctx_v;
    if (ms <= 0) return;

    /* Initialize pollfd structure. */
    struct pollfd pfd = { 0 };
    int nfds = 0;
    if (ctx->inotify_fd >= 0) {
        pfd.fd = ctx->inotify_fd;
        pfd.events = POLLIN;
        nfds = 1;
    }

    /* Record wait start timestamp if monotonic clock is available. */
    struct timespec wait_start;
    bool have_clock = clock_gettime(CLOCK_MONOTONIC, &wait_start) == 0;
    int remaining = ms;

    for (;;) {
        int rc = poll(nfds ? &pfd : NULL, (nfds_t) nfds, remaining);
        if (rc < 0) {
            if (errno == EINTR && have_clock) {
                /* Retry poll with remaining duration on EINTR. */
                struct timespec now;
                if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return;
                long elapsed_ms = (now.tv_sec - wait_start.tv_sec) * 1000L +
                                   (now.tv_nsec - wait_start.tv_nsec) / 1000000L;
                remaining = ms - (int) elapsed_ms;
                if (remaining <= 0) return;
                continue;
            }
            return;
        }
        if (rc > 0 && nfds && (pfd.revents & POLLIN)) {
            /* Drain pending inotify events. */
            char buf[512];
            while (read(ctx->inotify_fd, buf, sizeof(buf)) > 0) { }
        }
        return;
    }
}

/* Logs SD readiness result and diagnostics to stderr. */
static void log_sd_ready_outcome(const sd_ready_result_t * r) {
    const char * prefix = "open_hiby_bootloader: SD readiness";

    if (r->elapsed_ms < 0) {
        fprintf(stderr, "%s: monotonic clock unavailable -- proceeded with a single best-effort check "
                        "(mounted=%d, executable_ready=%d)\n",
                prefix, r->mounted, r->executable_ready);
        return;
    }

    switch (r->stage) {
    case SD_READY_STAGE_EXEC_READY:
        fprintf(stderr, "%s: ready after %lldms via %s\n", prefix, (long long) r->elapsed_ms,
                r->device_node_used ? r->device_node_used : "(unknown device)");
        break;
    case SD_READY_STAGE_MOUNTED:
        fprintf(stderr, "%s: filesystem mounted after %lldms (via %s) but no configured executable was found\n",
                prefix, (long long) r->elapsed_ms, r->device_node_used ? r->device_node_used : "(unknown device)");
        break;
    case SD_READY_STAGE_NODE_PRESENT:
        if (r->saw_partition_node) {
            fprintf(stderr, "%s: device node present after %lldms but the filesystem never mounted\n", prefix,
                    (long long) r->elapsed_ms);
        } else {
            fprintf(stderr,
                    "%s: whole-disk node present after %lldms but no partition node ever appeared and the "
                    "filesystem never mounted\n",
                    prefix, (long long) r->elapsed_ms);
        }
        break;
    case SD_READY_STAGE_MMC_EVIDENCE:
        fprintf(stderr, "%s: MMC host reported a card after %lldms but no block-device node ever appeared\n", prefix,
                (long long) r->elapsed_ms);
        break;
    case SD_READY_STAGE_NONE:
    default:
        fprintf(stderr, "%s: no MMC/card evidence after %lldms\n", prefix, (long long) r->elapsed_ms);
        break;
    }
}

/* Creates mount points, initializes inotify monitoring, and executes the SD
 * card readiness state machine to mount media before booting. */
void mount_sd_card_if_needed(void) {
    mkdir("/usr/data/mnt", 0755);
    mkdir(SD_MOUNT_POINT, 0755);

    real_probe_ctx_t ctx;
    ctx.inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (ctx.inotify_fd >= 0 && inotify_add_watch(ctx.inotify_fd, "/dev", IN_CREATE) < 0) {
        /* Fall back to timed polling if /dev cannot be watched with inotify. */
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
