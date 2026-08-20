#ifndef SUBPROCESS_H
#define SUBPROCESS_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

/* Runs argv[0] with the given arguments directly via fork+execvp -- no
 * shell is ever involved, so arguments containing shell metacharacters
 * (as a user-typed Wi-Fi SSID/password legitimately might) can never be
 * misinterpreted or escape into command injection, unlike building a
 * string and handing it to popen()/system(). argv must be NULL-terminated.
 * Captures combined stdout (stderr is discarded) into out_buf, always
 * NUL-terminated; out_buf/out_buf_size may be NULL/0 to just run the
 * command and wait for it to exit. Returns true if the process could be
 * started and ran to completion (regardless of its exit code -- callers
 * that care should parse out_buf). */
bool subprocess_run(char * const argv[], char * out_buf, size_t out_buf_size);

/* Like subprocess_run(), but with an explicit timeout instead of the
 * built-in 15s default -- for the rare legitimate call that needs longer
 * (e.g. bt_control_init_chip()'s /usr/bin/bt_init invocation, whose own
 * script sleeps for a good ~10-13s across chip firmware flashing and
 * service startup before it's done). */
bool subprocess_run_timeout(char * const argv[], char * out_buf, size_t out_buf_size, int timeout_ms);

/* Like subprocess_run_timeout(), but also reports the child's exit status
 * via *out_exit_code (0-255) -- for callers that need to distinguish a
 * script's own explicit failure (e.g. exiting 1 on a precondition check)
 * from success, not just "did it run to completion" (out_exit_code may be
 * NULL if the caller doesn't care, same as subprocess_run_timeout()).
 * *out_exit_code is set to -1 up front and left there if the process never
 * got to exit normally (spawn failure, timeout-kill). */
bool subprocess_run_checked(char * const argv[], char * out_buf, size_t out_buf_size, int timeout_ms,
                             int * out_exit_code);

/* Like subprocess_run(), but for a long-running daemon meant to outlive
 * this call (bluealsa, bluealsa-aplay) rather than a one-shot command --
 * double-forks (the standard SysV daemonize idiom) so the actual process
 * gets reparented to init and never becomes a zombie under us. No output
 * capture; stdin/stdout/stderr all go to /dev/null. Doesn't wait for the
 * daemon to actually finish starting up -- callers that need the service
 * ready immediately (e.g. before connecting to its D-Bus name) should add
 * their own short delay after calling this. */
bool subprocess_spawn_daemon(char * const argv[]);

/* Like subprocess_spawn_daemon(), but the daemon's stdout/stderr go to
 * log_path (truncated fresh, not appended) instead of /dev/null -- for a
 * caller that needs to inspect what the daemon actually printed on
 * startup (e.g. to detect and retry a flaky initialization failure that
 * doesn't surface any other way). log_path may be NULL, in which case
 * this behaves exactly like subprocess_spawn_daemon(). */
bool subprocess_spawn_daemon_logged(char * const argv[], const char * log_path);

/* For a long-running child whose stdout the caller wants to read
 * continuously as it's produced (e.g. `bluealsa-cli monitor`, which streams
 * D-Bus property-change lines indefinitely rather than exiting) -- unlike
 * subprocess_run(), doesn't buffer/wait for anything itself. A single fork
 * (not the daemonizing double-fork subprocess_spawn_daemon() uses), so the
 * child stays this process's direct child and *out_pid remains valid for
 * subprocess_terminate() later. stdin/stderr go to /dev/null; *out_read_fd
 * is the readable end of the child's stdout pipe (caller must close() it,
 * normally right after subprocess_terminate()). Returns false (nothing to
 * clean up) if the fork/pipe setup itself failed. */
bool subprocess_popen(char * const argv[], pid_t * out_pid, int * out_read_fd);

/* Mirror of subprocess_popen(), but the pipe runs the other direction: the
 * caller gets a writable fd connected to the child's stdin, for a
 * long-running child that consumes a continuous stream (e.g. `aplay`
 * reading raw PCM). stdout/stderr go to /dev/null. Same single-fork
 * (non-daemonizing) shape as subprocess_popen(), for the same reason: the
 * caller needs a live pid to pass to subprocess_terminate() later. Returns
 * false (nothing to clean up) if the fork/pipe setup itself failed. */
bool subprocess_popen_stdin(char * const argv[], pid_t * out_pid, int * out_write_fd);

/* Stops a child started by subprocess_popen()/subprocess_popen_stdin():
 * SIGTERM, a brief grace period polled via WNOHANG, then SIGKILL if it's
 * still alive -- and reaps it either way, so callers never need their own
 * waitpid(). */
void subprocess_terminate(pid_t pid);

/* Finds every process whose `ps` command line contains `needle` (a plain
 * substring match, not a pattern) and SIGKILLs them all -- for a daemon/
 * script this app doesn't itself own the pid of (started detached via
 * subprocess_spawn_daemon(), by a stock shell script, or otherwise outside
 * this process's own child list), where subprocess_terminate()'s pid-based
 * approach doesn't apply. Originally private to bluetooth_control.c (its
 * own bt-agent/bluealsa/dbus-daemon cleanup); shared here once wifi_control.c
 * needed the identical "kill any stray instance before starting a fresh
 * one" shape for udhcpc. Capped at 16 matches with margin to spare. */
void subprocess_kill_all_matching(const char * needle);

#endif /* SUBPROCESS_H */
