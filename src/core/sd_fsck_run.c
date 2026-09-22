#include "sd_fsck_run.h"

#include "sd_fsck.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#ifndef HOST_BUILD

#include "subprocess.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

/* The repair thread must not call mount helpers. The UI thread observes
 * PHASE_REMOUNT and mounts the saved node, so two threads never mount at
 * once and a hung check is never killed under the filesystem. */
enum {
    PHASE_IDLE = 0,
    PHASE_FSCK = 1,
    PHASE_REMOUNT = 2,
};

#define SD_FSCK_LOG_PATH "/usr/data/sd_fsck.log"
#define SD_REPAIR_ATTEMPT_SLOTS 4

extern void boot_checkpoint(const char * step);

static atomic_int repair_phase;
static atomic_int repair_note;
static atomic_int repair_fsck_code = -1;
static sd_fsck_plan_t repair_plan;
static char repair_mount_point[SD_FSCK_MOUNT_BYTES];
static char repair_attempted[SD_REPAIR_ATTEMPT_SLOTS][SD_FSCK_ATTEMPT_BYTES];
static int repair_attempted_count;

static void post_note(sd_repair_note_t note) {
    atomic_store_explicit(&repair_note, (int) note, memory_order_release);
}

static bool tool_exists(const char * path, void * ctx) {
    (void) ctx;
    return access(path, X_OK) == 0;
}

static void read_card_cid(char * out, size_t out_size) {
    if (!out || out_size == 0) return;
    out[0] = '\0';
    FILE * file = fopen("/sys/class/block/mmcblk0/device/cid", "r");
    if (!file) return;
    if (!fgets(out, (int) out_size, file)) out[0] = '\0';
    fclose(file);
    char * end = strpbrk(out, "\r\n");
    if (end) *end = '\0';
}

static bool attempt_remembered(const char * key) {
    for (int i = 0; i < repair_attempted_count; i++) {
        if (strcmp(repair_attempted[i], key) == 0) return true;
    }
    return false;
}

static void remember_attempt(const char * key) {
    if (!key || !key[0] || attempt_remembered(key)) return;
    if (repair_attempted_count >= SD_REPAIR_ATTEMPT_SLOTS) return;
    snprintf(repair_attempted[repair_attempted_count], SD_FSCK_ATTEMPT_BYTES, "%s", key);
    repair_attempted_count++;
}

static bool read_mounts(char * buffer, size_t size) {
    FILE * file = fopen("/proc/mounts", "r");
    if (!file) return false;
    size_t got = fread(buffer, 1, size - 1, file);
    int error = ferror(file);
    fclose(file);
    if (error) return false;
    buffer[got] = '\0';
    return true;
}

static void log_repair_line(const char * text) {
    FILE * file = fopen(SD_FSCK_LOG_PATH, "a");
    if (!file) return;
    fputs(text, file);
    fputc('\n', file);
    fclose(file);
}

static void close_inherited_fds(void) {
    DIR * dir = opendir("/proc/self/fd");
    if (!dir) return;
    int dir_fd = dirfd(dir);
    struct dirent * entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] < '0' || entry->d_name[0] > '9') continue;
        int fd = atoi(entry->d_name);
        if (fd > STDERR_FILENO && fd != dir_fd) close(fd);
    }
    closedir(dir);
}

/* Waits until the checker exits. Killing it would leave the card mid-write. */
static int run_fsck(char * const argv[]) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        int log_fd = open(SD_FSCK_LOG_PATH, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (log_fd >= 0) {
            dup2(log_fd, STDOUT_FILENO);
            dup2(log_fd, STDERR_FILENO);
            close(log_fd);
        }
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            close(devnull);
        }
        close_inherited_fds();
        execv(argv[0], argv);
        _exit(127);
    }
    for (;;) {
        int status = 0;
        pid_t waited = waitpid(pid, &status, 0);
        if (waited == pid) {
            if (WIFEXITED(status)) return WEXITSTATUS(status);
            return -1;
        }
        if (waited < 0 && errno != EINTR) return -1;
    }
}

static void * repair_thread(void * arg) {
    (void) arg;
    int code = run_fsck(repair_plan.argv);
    atomic_store_explicit(&repair_fsck_code, code, memory_order_relaxed);
    atomic_store_explicit(&repair_phase, PHASE_REMOUNT, memory_order_release);
    return NULL;
}

static bool unmount_card(const char * mount_point) {
    if (!mount_point || !mount_point[0]) return false;
    sync();
    char * argv[] = { (char *) "umount", (char *) mount_point, NULL };
    int exit_code = -1;
    if (!subprocess_run_checked(argv, NULL, 0, 15000, &exit_code)) return false;
    return exit_code == 0;
}

bool sd_repair_in_progress(void) {
    int phase = atomic_load_explicit(&repair_phase, memory_order_acquire);
    return phase == PHASE_FSCK || phase == PHASE_REMOUNT;
}

bool sd_repair_needs_remount(void) {
    return atomic_load_explicit(&repair_phase, memory_order_acquire) == PHASE_REMOUNT;
}

const char * sd_repair_device(void) { return repair_plan.device; }

bool sd_repair_current_readonly(bool * readonly) {
    if (readonly) *readonly = false;
    char * mounts = malloc(16384);
    if (!mounts) return false;
    bool loaded = read_mounts(mounts, 16384);
    sd_mount_info_t info;
    bool found = loaded && sd_fsck_parse_mounts(mounts, &info) && info.found;
    free(mounts);
    if (!found) return false;
    if (readonly) *readonly = info.readonly;
    return true;
}

void sd_repair_complete(bool mounted, bool readonly) {
    int code = atomic_load_explicit(&repair_fsck_code, memory_order_relaxed);
    char line[160];
    snprintf(line, sizeof(line), "fsck exit %d, mounted=%d, readonly=%d", code, mounted, readonly);
    log_repair_line(line);
    if (!mounted) post_note(SD_REPAIR_NOTE_FAILED);
    else if (!readonly) post_note(SD_REPAIR_NOTE_REPAIRED);
    else if (code == 127 || code < 0) post_note(SD_REPAIR_NOTE_FAILED);
    else post_note(SD_REPAIR_NOTE_STILL_READONLY);
    boot_checkpoint(mounted && !readonly ? "sd repair remounted read-write" : "sd repair left card unusable");
    atomic_store_explicit(&repair_phase, PHASE_IDLE, memory_order_release);
}

sd_repair_note_t sd_repair_take_note(void) {
    return (sd_repair_note_t) atomic_exchange_explicit(&repair_note, SD_REPAIR_NOTE_NONE, memory_order_acq_rel);
}

sd_repair_kick_result_t sd_readonly_repair_kick(void (* release_handles)(void)) {
    sd_repair_kick_result_t result = { false, false };
    if (sd_repair_in_progress()) return result;

    char * mounts = malloc(16384);
    if (!mounts) return result;
    bool loaded = read_mounts(mounts, 16384);
    sd_mount_info_t info;
    bool parsed = loaded && sd_fsck_parse_mounts(mounts, &info);
    free(mounts);
    if (!parsed || !info.found || !info.readonly) return result;
    if (!sd_fsck_device_allowed(info.device)) return result;

    char cid[64];
    read_card_cid(cid, sizeof(cid));
    char key[SD_FSCK_ATTEMPT_BYTES];
    sd_repair_attempt_key(info.device, cid, key, sizeof(key));
    if (attempt_remembered(key)) return result;

    const char * tool = sd_fsck_select_tool(info.kind, tool_exists, NULL);
    if (!tool || !sd_fsck_plan(info.kind, tool, info.device, &repair_plan)) {
        remember_attempt(key);
        post_note(SD_REPAIR_NOTE_FAILED);
        boot_checkpoint("sd repair skipped, no checker for this filesystem");
        return result;
    }
    snprintf(repair_mount_point, sizeof(repair_mount_point), "%s", info.mount_point);

    bool unmounted = unmount_card(repair_mount_point);
    if (!unmounted && release_handles) {
        release_handles();
        result.released_handles = true;
        unmounted = unmount_card(repair_mount_point);
    }
    if (!unmounted) {
        /* A busy card with nobody to close is retried on a later poll.
         * After the files are closed, another failure is final so the
         * poll does not tear the library down every cycle. */
        if (result.released_handles) {
            remember_attempt(key);
            post_note(SD_REPAIR_NOTE_FAILED);
            boot_checkpoint("sd repair unmount failed");
        }
        return result;
    }

    remember_attempt(key);
    atomic_store_explicit(&repair_fsck_code, -1, memory_order_relaxed);
    atomic_store_explicit(&repair_phase, PHASE_FSCK, memory_order_release);
    pthread_t thread;
    if (pthread_create(&thread, NULL, repair_thread, NULL) != 0) {
        /* The card is already unmounted. Ask the UI thread to mount it
         * again instead of leaving it detached. */
        atomic_store_explicit(&repair_phase, PHASE_REMOUNT, memory_order_release);
        boot_checkpoint("sd repair thread failed");
        return result;
    }
    pthread_detach(thread);
    post_note(SD_REPAIR_NOTE_STARTED);
    boot_checkpoint("sd repair started");
    char line[320];
    snprintf(line, sizeof(line), "checking %s (%s) with %s", repair_plan.device, info.fstype, repair_plan.tool);
    log_repair_line(line);
    result.started = true;
    return result;
}

#else

bool sd_repair_in_progress(void) { return false; }
bool sd_repair_needs_remount(void) { return false; }
const char * sd_repair_device(void) { return ""; }
bool sd_repair_current_readonly(bool * readonly) {
    if (readonly) *readonly = false;
    return false;
}
void sd_repair_complete(bool mounted, bool readonly) { (void) mounted; (void) readonly; }
sd_repair_note_t sd_repair_take_note(void) { return SD_REPAIR_NOTE_NONE; }
sd_repair_kick_result_t sd_readonly_repair_kick(void (* release_handles)(void)) {
    (void) release_handles;
    sd_repair_kick_result_t result = { false, false };
    return result;
}

#endif
