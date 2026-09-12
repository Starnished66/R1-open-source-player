#include "subprocess.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* Closes all file descriptors above stderr in the child after fork().
 * Many open()/pcm_open() calls in this codebase (including tinyalsa's
 * pcm_hw_open()) do not set O_CLOEXEC, so every fd is duplicated into
 * any forked child. Long-lived daemons spawned via exec can permanently
 * hold duplicates of, e.g., the ALSA PCM fd, causing EBUSY on every
 * subsequent pcm_open() in the parent even after its own handle is closed.
 * Closing inherited fds in the child right before exec protects every
 * subprocess call regardless of what the parent has open at the time.
 * Safe after fork(): the child is single-threaded at that point. */
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

/* Default subprocess execution timeout (15s) to ensure hung commands do not block the UI.
 * Commands requiring longer budgets (e.g. bt_init) call subprocess_run_timeout() directly. */
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

    int wait_status;
    while (waitpid(pid, &wait_status, 0) < 0 && errno == EINTR) {
        /* retry -- interrupted by a signal, not an indication the child is still running */
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

void subprocess_kill_all_matching(const char * needle) {
    char out[4096];
    char * argv[] = { (char *) "ps", NULL };
    if (!subprocess_run(argv, out, sizeof(out))) return;

    int pids[16];
    int pid_count = 0;
    char * line_save = NULL;
    char * line = strtok_r(out, "\n", &line_save);
    while (line && pid_count < 16) {
        if (strstr(line, needle)) {
            int pid = atoi(line);
            if (pid > 0) pids[pid_count++] = pid;
        }
        line = strtok_r(NULL, "\n", &line_save);
    }
    for (int i = 0; i < pid_count; i++) kill(pids[i], SIGKILL);
}
