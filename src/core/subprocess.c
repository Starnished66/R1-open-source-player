#include "subprocess.h"

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* Real-device incident: none of the open()/pcm_open() calls elsewhere in
 * this codebase (tinyalsa's pcm_hw_open() for the audio device notably
 * included) set O_CLOEXEC, so every fd this app has open at the moment any
 * of the fork() calls below happens gets duplicated into the child --
 * harmless for a short-lived one-shot command, but a real problem for any
 * child that goes on to exec a long-lived daemon of its own (confirmed on
 * a real device: switching USB modes while a song was playing forked
 * through /usr/bin/adbon -> S440adb -> adbserver.sh -> adbd, and adbd kept
 * running long after, permanently holding a duplicate of the ALSA PCM
 * device fd open -- every subsequent pcm_open() in this app failed with
 * "Resource busy" until that adbd was killed, even though the app's own
 * handle to the device had long since been closed. Rather than hunting
 * down and adding O_CLOEXEC to every current and future open()/socket()/
 * pcm_open() call in this codebase individually (easy to miss one, and
 * tinyalsa's is in vendored code this project doesn't otherwise patch),
 * this closes everything above stdin/stdout/stderr in the child right
 * before exec -- one fix that protects every subprocess call regardless of
 * what else the app happens to have open at the time. Safe to do after
 * fork(): the child is single-threaded at this point (fork() only
 * duplicates the calling thread), so there's no other thread that could
 * still be using one of these fds out from under it. */
static void close_inherited_fds(void) {
    DIR * dir = opendir("/proc/self/fd");
    if (!dir) return;
    int dir_fd = dirfd(dir);

    struct dirent * entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] < '0' || entry->d_name[0] > '9') continue; /* skip "." / ".." */
        int fd = atoi(entry->d_name);
        if (fd > STDERR_FILENO && fd != dir_fd) close(fd);
    }
    closedir(dir);
}

/* Real-device incident: bluetoothctl show hung indefinitely (bluetoothd/
 * hci0 in a bad state after an unclean reboot), and since subprocess_run()
 * is called directly from update_timer_cb's periodic poll (refresh_bt_icon,
 * every 5s) on the one thread driving the whole UI, that single hung child
 * process froze the entire device -- not just this app. subprocess_run()
 * previously had no bound on either the pipe read or the final waitpid(),
 * so a child that never writes/closes stdout or never exits blocked here
 * forever. This timeout is generous enough to never trip on any real,
 * healthy call at this default timeout -- the longest plain subprocess_run()
 * call left is a few seconds (bluetoothctl info/pair/trust/connect, wpa_cli
 * scan, etc). bt_control_scan()'s own scan window now runs through
 * subprocess_popen_stdin() instead (see run_bredr_scan() in
 * bluetooth_control.c -- needs one persistent bluetoothctl session so a
 * `scan.transport bredr` filter actually applies to the `scan on` that
 * follows it), so this shared timeout no longer bounds it; that call site
 * has its own sleep(seconds) instead. The one subprocess_run() call that
 * legitimately needs longer than this default (bt_control_init_chip()'s
 * /usr/bin/bt_init, whose own script sleeps for ~10-13s across chip
 * firmware flashing and service startup) uses subprocess_run_timeout()
 * directly with its own explicit budget instead of raising this shared
 * default. */
#define SUBPROCESS_TIMEOUT_MS 15000

bool subprocess_run(char * const argv[], char * out_buf, size_t out_buf_size) {
    return subprocess_run_timeout(argv, out_buf, out_buf_size, SUBPROCESS_TIMEOUT_MS);
}

bool subprocess_run_timeout(char * const argv[], char * out_buf, size_t out_buf_size, int timeout_ms) {
    return subprocess_run_checked(argv, out_buf, out_buf_size, timeout_ms, NULL);
}

bool subprocess_run_checked(char * const argv[], char * out_buf, size_t out_buf_size, int timeout_ms,
                             int * out_exit_code) {
    if (out_exit_code) *out_exit_code = -1;

    int pipefd[2] = { -1, -1 };
    if (out_buf && out_buf_size > 0) {
        if (pipe(pipefd) != 0) return false;
        out_buf[0] = '\0';
    }

    pid_t pid = fork();
    if (pid < 0) {
        if (pipefd[0] >= 0) close(pipefd[0]);
        if (pipefd[1] >= 0) close(pipefd[1]);
        return false;
    }

    if (pid == 0) {
        if (pipefd[1] >= 0) {
            close(pipefd[0]);
            dup2(pipefd[1], STDOUT_FILENO);
            close(pipefd[1]);
        } else {
            int devnull_out = open("/dev/null", O_WRONLY);
            if (devnull_out >= 0) { dup2(devnull_out, STDOUT_FILENO); close(devnull_out); }
        }
        int devnull_err = open("/dev/null", O_WRONLY);
        if (devnull_err >= 0) { dup2(devnull_err, STDERR_FILENO); close(devnull_err); }
        close_inherited_fds();
        execvp(argv[0], argv);
        _exit(127); /* execvp only returns on failure */
    }

    if (pipefd[1] >= 0) close(pipefd[1]);

    bool timed_out = false;
    if (pipefd[0] >= 0) {
        size_t total = 0;
        for (;;) {
            if (total + 1 >= out_buf_size) break;
            struct pollfd pfd = { .fd = pipefd[0], .events = POLLIN };
            int pr = poll(&pfd, 1, timeout_ms);
            if (pr <= 0) {
                timed_out = (pr == 0); /* 0 = timeout; <0 = poll() error, treat like EOF */
                break;
            }
            ssize_t n = read(pipefd[0], out_buf + total, out_buf_size - 1 - total);
            if (n <= 0) break;
            total += (size_t) n;
        }
        out_buf[total] = '\0';
        close(pipefd[0]);
    }

    if (timed_out) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        return false;
    }

    /* Bounds the final exit-wait too -- the read loop above only covers
     * stdout; a child that closes/never had stdout piped (out_buf == NULL)
     * but keeps running could still hang here forever otherwise. Polled in
     * small slices rather than a single blocking waitpid() since there's no
     * "wait with timeout" syscall. */
    for (int waited_ms = 0; waited_ms < timeout_ms; waited_ms += 50) {
        int status = 0;
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            if (out_exit_code && WIFEXITED(status)) *out_exit_code = WEXITSTATUS(status);
            return true;
        }
        usleep(50000);
    }
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    return false;
}

bool subprocess_spawn_daemon(char * const argv[]) {
    return subprocess_spawn_daemon_logged(argv, NULL);
}

bool subprocess_spawn_daemon_logged(char * const argv[], const char * log_path) {
    pid_t pid = fork();
    if (pid < 0) return false;

    if (pid == 0) {
        /* First child: fork again then exit immediately -- the grandchild
         * (the actual daemon) gets reparented to init, which reaps it when
         * it eventually exits, so it never becomes a zombie under this
         * process either. */
        pid_t pid2 = fork();
        if (pid2 == 0) {
            setsid();
            int devnull_in = open("/dev/null", O_RDONLY);
            if (devnull_in >= 0) {
                dup2(devnull_in, STDIN_FILENO);
                close(devnull_in);
            }
            /* log_path (truncated fresh each spawn, not appended -- callers
             * that retry a spawn want just the latest attempt's output, not
             * a growing file mixing every previous one) instead of
             * /dev/null when the caller wants to inspect what the daemon
             * actually printed on startup, e.g. to detect and retry a known
             * flaky initialization failure that doesn't surface any other
             * way. */
            int out_fd = log_path ? open(log_path, O_WRONLY | O_CREAT | O_TRUNC, 0644) : open("/dev/null", O_WRONLY);
            if (out_fd < 0) out_fd = open("/dev/null", O_WRONLY);
            if (out_fd >= 0) {
                dup2(out_fd, STDOUT_FILENO);
                dup2(out_fd, STDERR_FILENO);
                close(out_fd);
            }
            close_inherited_fds();
            execvp(argv[0], argv);
            _exit(127); /* execvp only returns on failure */
        }
        _exit(0);
    }

    /* Bounded, not a plain blocking waitpid() -- the first child only does
     * one more fork() then exits immediately, which should be near-instant,
     * but fork() itself has no userspace timeout, and confirmed via
     * persistent boot-checkpoint logging on a real device that a
     * structurally identical unbounded wait (in a startup D-Bus-cleanup
     * function since removed entirely -- see bluetooth_control.h's comment
     * above bt_control_is_powered() for the full story) hung indefinitely
     * at exactly this kind of call on a genuine cold boot -- plausibly
     * memory pressure from dbus/wifi/bluetooth all still starting up at
     * once on this device's very limited RAM stalling the inner fork().
     * Every caller of
     * this function (bluetoothd/bt-agent/bluealsa-aplay respawns, AirPlay's
     * shairport, the local-import HTTP server) is reachable from a
     * real-device incident's recovery path or the startup path, so this is
     * fixed once here rather than requiring every caller to know to guard
     * against it individually. Polled in small slices, same pattern as
     * subprocess_run_checked()'s own tail wait and subprocess_terminate().
     * If the first child doesn't exit in time, stop waiting on it anyway --
     * it and the real daemon it's spawning keep running regardless, this
     * was only ever reaping bookkeeping, not the actual work. */
    for (int waited_ms = 0; waited_ms < SUBPROCESS_TIMEOUT_MS; waited_ms += 50) {
        if (waitpid(pid, NULL, WNOHANG) == pid) break;
        usleep(50000);
    }
    return true;
}

bool subprocess_popen(char * const argv[], pid_t * out_pid, int * out_read_fd) {
    int pipefd[2];
    if (pipe(pipefd) != 0) return false;

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return false;
    }

    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        int devnull_in = open("/dev/null", O_RDONLY);
        if (devnull_in >= 0) { dup2(devnull_in, STDIN_FILENO); close(devnull_in); }
        int devnull_err = open("/dev/null", O_WRONLY);
        if (devnull_err >= 0) { dup2(devnull_err, STDERR_FILENO); close(devnull_err); }
        close_inherited_fds();
        execvp(argv[0], argv);
        _exit(127); /* execvp only returns on failure */
    }

    close(pipefd[1]);
    *out_pid = pid;
    *out_read_fd = pipefd[0];
    return true;
}

bool subprocess_popen_stdin(char * const argv[], pid_t * out_pid, int * out_write_fd) {
    int pipefd[2];
    if (pipe(pipefd) != 0) return false;

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return false;
    }

    if (pid == 0) {
        close(pipefd[1]);
        dup2(pipefd[0], STDIN_FILENO);
        close(pipefd[0]);
        int devnull_out = open("/dev/null", O_WRONLY);
        if (devnull_out >= 0) { dup2(devnull_out, STDOUT_FILENO); close(devnull_out); }
        int devnull_err = open("/dev/null", O_WRONLY);
        if (devnull_err >= 0) { dup2(devnull_err, STDERR_FILENO); close(devnull_err); }
        close_inherited_fds();
        execvp(argv[0], argv);
        _exit(127); /* execvp only returns on failure */
    }

    close(pipefd[0]);
    *out_pid = pid;
    *out_write_fd = pipefd[1];
    return true;
}

void subprocess_terminate(pid_t pid) {
    kill(pid, SIGTERM);
    for (int waited_ms = 0; waited_ms < 1000; waited_ms += 50) {
        int status = 0;
        if (waitpid(pid, &status, WNOHANG) == pid) return;
        usleep(50000);
    }
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
}
