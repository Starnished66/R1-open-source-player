#include "bluetooth_control.h"
#include "debug_log.h"
#include "hiby_sys_server.h"
#include "subprocess.h"
#include "audio.h"

#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>

/* Guards bt_control_init_chip()/bt_control_enable()/bt_control_disable() --
 * the three functions that actually touch the physical chip (chip firmware
 * flash, or the D-Bus adapter power toggle). Real-device incident: BT ended
 * up permanently wedged (unrecoverable without a full reboot) after waking
 * from suspend, traced to gui.c's own pre-existing radio-suspend/restore
 * threads and power_suspend.c's post-resume restore thread both being able
 * to call into these with no coordination between the two modules -- two
 * concurrent chip operations racing each other, which this chip is already
 * documented (see bt_control_disable()'s own comment on bt_suspend/
 * bt_resume) not to tolerate. Held for the entire body of each of the three
 * functions below, not just the individual subprocess_run() calls, so a
 * caller can never observe or interleave with a partial chip operation from
 * another thread. */
static pthread_mutex_t bt_chip_mutex = PTHREAD_MUTEX_INITIALIZER;

static pthread_mutex_t bt_dac_info_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t bt_dac_monitor_mutex = PTHREAD_MUTEX_INITIALIZER;
static bt_dac_stream_info_t bt_dac_info;
static pthread_t bt_dac_info_thread;
static bool bt_dac_info_active;
static pid_t bt_dac_info_monitor_pid = -1;

void bt_control_get_dac_stream_info(bt_dac_stream_info_t * out) {
    if (!out) return;
    pthread_mutex_lock(&bt_dac_info_mutex);
    *out = bt_dac_info;
    pthread_mutex_unlock(&bt_dac_info_mutex);
}

static void bt_dac_info_store(const bt_dac_stream_info_t * info) {
    pthread_mutex_lock(&bt_dac_info_mutex);
    bt_dac_info = *info;
    pthread_mutex_unlock(&bt_dac_info_mutex);
}

static unsigned int pcm_format_bit_depth(const char * format) {
    if (!format) return 0;
    if (strstr(format, "S16") || strstr(format, "U16")) return 16;
    if (strstr(format, "S24") || strstr(format, "U24")) return 24;
    if (strstr(format, "S32") || strstr(format, "U32") || strstr(format, "FLOAT")) return 32;
    if (strstr(format, "S8") || strstr(format, "U8")) return 8;
    return 0;
}

static void copy_info_field(char * dst, size_t size, const char * text, const char * key) {
    const char * p = strstr(text, key);
    if (!p) return;
    p += strlen(key);
    while (*p == ' ' || *p == '\t') p++;
    size_t n = strcspn(p, "\r\n");
    if (n >= size) n = size - 1;
    memcpy(dst, p, n);
    dst[n] = '\0';
}

static void bt_dac_info_refresh(void) {
    bt_dac_stream_info_t info = {0};
    char list_out[4096];
    char * list_argv[] = { (char *) "bluealsa-cli", (char *) "list-pcms", NULL };
    if (!subprocess_run(list_argv, list_out, sizeof(list_out))) {
        bt_dac_info_store(&info);
        return;
    }
    char path[256] = {0};
    char * save = NULL;
    for (char * line = strtok_r(list_out, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
        if (strstr(line, "/a2dpsnk/source")) {
            snprintf(path, sizeof(path), "%s", line);
            break;
        }
    }
    if (!path[0]) {
        bt_dac_info_store(&info); /* PCM disappeared: clear stale data now. */
        return;
    }
    info.available = true;
    char info_out[2048];
    char * info_argv[] = { (char *) "bluealsa-cli", (char *) "info", path, NULL };
    if (subprocess_run(info_argv, info_out, sizeof(info_out))) {
        copy_info_field(info.codec, sizeof(info.codec), info_out, "Selected codec:");
        copy_info_field(info.pcm_format, sizeof(info.pcm_format), info_out, "Format:");
        const char * sampling = strstr(info_out, "Sampling:");
        const char * channels = strstr(info_out, "Channels:");
        const char * running = strstr(info_out, "Running:");
        if (sampling) (void) sscanf(sampling + strlen("Sampling:"), "%u", &info.sample_rate);
        if (channels) (void) sscanf(channels + strlen("Channels:"), "%u", &info.channels);
        if (running) {
            running += strlen("Running:");
            while (*running == ' ' || *running == '\t') running++;
            info.running = !strncmp(running, "true", 4) || !strncmp(running, "yes", 3) || *running == '1';
        }
        info.bit_depth = pcm_format_bit_depth(info.pcm_format);
    }
    bt_dac_info_store(&info);
}

static void * bt_dac_info_thread_func(void * arg) {
    (void) arg;
    char * argv[] = { (char *) "bluealsa-cli", (char *) "monitor", NULL };
    pid_t pid;
    int fd;
    if (!subprocess_popen(argv, &pid, &fd)) return NULL;

    /* Publish the child only while holding the same lock stop() uses. If a
     * stop landed in the small fork-to-publish window, terminate our own
     * child instead of entering fgets() after stop() already looked for it. */
    pthread_mutex_lock(&bt_dac_monitor_mutex);
    if (!bt_dac_info_active) {
        pthread_mutex_unlock(&bt_dac_monitor_mutex);
        subprocess_terminate(pid);
        close(fd);
        return NULL;
    }
    bt_dac_info_monitor_pid = pid;
    pthread_mutex_unlock(&bt_dac_monitor_mutex);

    FILE * f = fdopen(fd, "r");
    if (!f) {
        pthread_mutex_lock(&bt_dac_monitor_mutex);
        if (bt_dac_info_monitor_pid == pid) bt_dac_info_monitor_pid = -1;
        pthread_mutex_unlock(&bt_dac_monitor_mutex);
        subprocess_terminate(pid);
        close(fd);
        return NULL;
    }

    /* Subscribe first so changes occurring during this initial blocking
     * query remain queued in the monitor pipe and cannot be missed. */
    bt_dac_info_refresh();
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "PCMRemoved") && strstr(line, "/a2dpsnk/source")) {
            bt_dac_stream_info_t empty = {0};
            bt_dac_info_store(&empty);
        } else if (strstr(line, "PCM") || strstr(line, "/a2dpsnk/source") || strstr(line, "Codec") || strstr(line, "Running")) {
            bt_dac_info_refresh();
        }
    }
    fclose(f);
    pthread_mutex_lock(&bt_dac_monitor_mutex);
    if (bt_dac_info_monitor_pid == pid) bt_dac_info_monitor_pid = -1;
    pthread_mutex_unlock(&bt_dac_monitor_mutex);
    /* This thread owns reaping its monitor child. stop() only signals and
     * joins, avoiding two threads racing waitpid() on the same PID. */
    (void) waitpid(pid, NULL, 0);
    bt_dac_stream_info_t empty = {0};
    bt_dac_info_store(&empty);
    return NULL;
}

static void bt_dac_info_monitor_stop(void) {
    pthread_mutex_lock(&bt_dac_monitor_mutex);
    if (!bt_dac_info_active) {
        pthread_mutex_unlock(&bt_dac_monitor_mutex);
        bt_dac_stream_info_t empty = {0}; bt_dac_info_store(&empty); return;
    }
    bt_dac_info_active = false;
    pid_t pid = bt_dac_info_monitor_pid;
    pthread_mutex_unlock(&bt_dac_monitor_mutex);
    if (pid > 0) {
        kill(pid, SIGTERM);
        /* fgets() normally unblocks as soon as SIGTERM closes the pipe.
         * Keep the same one-second grace period as subprocess_terminate(),
         * but let the monitor thread itself reap the child. */
        for (int waited_ms = 0; waited_ms < 1000; waited_ms += 50) {
            pthread_mutex_lock(&bt_dac_monitor_mutex);
            bool still_same_child = bt_dac_info_monitor_pid == pid;
            pthread_mutex_unlock(&bt_dac_monitor_mutex);
            if (!still_same_child) break;
            usleep(50000);
        }
        pthread_mutex_lock(&bt_dac_monitor_mutex);
        bool still_same_child = bt_dac_info_monitor_pid == pid;
        pthread_mutex_unlock(&bt_dac_monitor_mutex);
        if (still_same_child) kill(pid, SIGKILL);
    }
    pthread_join(bt_dac_info_thread, NULL);
    pthread_mutex_lock(&bt_dac_monitor_mutex);
    bt_dac_info_monitor_pid = -1;
    pthread_mutex_unlock(&bt_dac_monitor_mutex);
    bt_dac_stream_info_t empty = {0};
    bt_dac_info_store(&empty);
}

static void bt_dac_info_monitor_start(void) {
    bt_dac_info_monitor_stop();
    pthread_mutex_lock(&bt_dac_monitor_mutex);
    bt_dac_info_active = true;
    if (pthread_create(&bt_dac_info_thread, NULL, bt_dac_info_thread_func, NULL) != 0) {
        bt_dac_info_active = false;
    }
    pthread_mutex_unlock(&bt_dac_monitor_mutex);
}

/* /etc/init.d/S80_bt_init always starts a second, independent dbus-daemon
 * (`--config-file=/usr/share/dbus-1/system.conf`) alongside the one
 * S30dbus already started at boot (`--system`), regardless of anything
 * this app does. Depending on which of the two buses a given client
 * (bluetoothd, bluealsa, bt-agent, or this file's own bluetoothctl calls)
 * lands on, they can lose visibility of each other -- e.g. an incoming
 * AVDTP connect gets rejected ("Authentication attempt without agent")
 * because bt-agent registered its pairing agent on the bus bluetoothd
 * wasn't listening on. A bus-reachability check alone doesn't catch this,
 * since either daemon individually still answers fine even with the
 * split-brain intact; counting dbus-daemon processes directly is what
 * actually detects it, and is cheap enough (a single `ps`) to call
 * unconditionally. */
static int count_matching(const char * needle) {
    char out[4096];
    char * argv[] = { (char *) "ps", NULL };
    if (!subprocess_run(argv, out, sizeof(out))) return 0;

    int count = 0;
    char * line_save = NULL;
    char * line = strtok_r(out, "\n", &line_save);
    while (line) {
        if (strstr(line, needle)) count++;
        line = strtok_r(NULL, "\n", &line_save);
    }
    return count;
}

static bool dbus_system_bus_reachable(void) {
    char out[256];
    char * argv[] = { (char *) "dbus-send", (char *) "--system", (char *) "--print-reply",
                       (char *) "--dest=org.freedesktop.DBus", (char *) "/org/freedesktop/DBus",
                       (char *) "org.freedesktop.DBus.ListNames", NULL };
    return subprocess_run(argv, out, sizeof(out)) && strstr(out, "method return") != NULL;
}

static void ensure_single_dbus_daemon(void) {
    if (count_matching("dbus-daemon") <= 1 && dbus_system_bus_reachable()) return;

    subprocess_kill_all_matching("dbus-daemon");
    remove("/var/run/messagebus.pid"); /* stale pidfile blocks a fresh start otherwise */
    usleep(300000);

    /* Real-device incident: without --fork, this build of dbus-daemon
     * doesn't daemonize -- it stays in the foreground indefinitely. This
     * used to be launched with the blocking subprocess_run(), which waited
     * out its full 15s timeout (freezing whichever thread called this --
     * the main GUI thread, when reached via
     * bt_control_recover_wedged_daemon()) and then SIGKILLed the
     * dbus-daemon it had just started, leaving the system with no system
     * bus at all. Confirmed live: `dbus-daemon --system` alone stayed
     * foreground; `dbus-daemon --system --fork` returned immediately with
     * the daemon still running. subprocess_spawn_daemon() (fire and
     * forget, same as every other long-running daemon this file starts --
     * bluetoothd, bluealsa) is the correct call shape regardless of --fork,
     * but passing --fork too matches how a real dbus-daemon is meant to be
     * started and costs nothing. */
    char * argv[] = { (char *) "dbus-daemon", (char *) "--system", (char *) "--fork", NULL };
    subprocess_spawn_daemon(argv);
    usleep(300000);
}

/* Defined with the rest of the output-settings section, below -- restores
 * whatever bt_control_apply_output_settings() was last actually asked for
 * (DAC/a2dp-sink mode in particular). Needed by the wedge recovery further
 * down, which is defined earlier in the file than that section. */
static void bt_control_reapply_last_output_settings(void);

/* Cheap pre-check for whether a Bluetooth adapter exists before asking
 * bluetoothctl about it. With no hci0, `bluetoothctl show` doesn't fail
 * fast -- it can take close to subprocess_run()'s full 15s timeout to give
 * up, which would stall the UI thread since this is called every
 * update_timer_cb tick. `hciconfig` returns almost instantly (empty output
 * when no adapter exists), so the bluetoothctl-over-D-Bus round trip is
 * only attempted when there's actually an adapter to ask about. */
static bool bt_control_adapter_present(void) {
    char out[64];
    char * argv[] = { (char *) "hciconfig", NULL };
    if (!subprocess_run(argv, out, sizeof(out))) return false;
    return out[0] != '\0';
}

/* Once real A2DP audio starts flowing during Bluetooth DAC mode,
 * bluetoothd can end up in a state where `bluetoothctl show` reports "No
 * default controller available" even though hci0 is still genuinely up
 * (bt_control_adapter_present() already true) and bluealsa/bluealsa-aplay
 * may still be actively streaming through it -- bluetoothd itself doesn't
 * crash (same PID, still sleeping), its D-Bus interface just stops
 * answering correctly. The only recovery that works is restarting
 * bluetoothd, mirroring /usr/bin/bt_resume's own remediation: stop
 * bluetoothd, `hciconfig hci0 reset`, start bluetoothd again.
 * Rate-limited via last_recovery_attempt so a wedge that recovery doesn't
 * fix can't trigger a restart on every ~5s status poll indefinitely. */
#define WEDGED_RECOVERY_COOLDOWN_SECONDS 20

static void bt_control_recover_wedged_daemon(void) {
    static time_t last_recovery_attempt = 0;
    time_t now = time(NULL);
    if (now - last_recovery_attempt < WEDGED_RECOVERY_COOLDOWN_SECONDS) {
        DBG_LOG("bt_control: wedged daemon detected, but recovery on cooldown (%lds left)\n",
                (long) (WEDGED_RECOVERY_COOLDOWN_SECONDS - (now - last_recovery_attempt)));
        return;
    }
    last_recovery_attempt = now;

    DBG_LOG("bt_control: wedged daemon detected, attempting recovery\n");

    /* Real-device incident: found live while debugging this exact wedge --
     * two dbus-daemon --system processes were running simultaneously (the
     * same split-brain condition ensure_single_dbus_daemon() already
     * exists to fix at startup, see its own comment for the full history).
     * bluetoothctl and the freshly-restarted bluetoothd below can end up
     * on different buses in that state, which explains why bluetoothctl
     * show hung completely (full subprocess timeout, not even a fast
     * error) rather than returning a quick "No default controller"
     * response -- it was waiting for a reply from a bluetoothd that
     * wasn't listening on the bus it asked. Checked/fixed here too, not
     * just at startup, since real-device testing showed this condition
     * recurring mid-session, not only on a fresh boot. */
    ensure_single_dbus_daemon();

    subprocess_kill_all_matching("bluetoothd");
    usleep(500000);

    char * reset_argv[] = { (char *) "hciconfig", (char *) "hci0", (char *) "reset", NULL };
    bool reset_ok = subprocess_run(reset_argv, NULL, 0);
    usleep(500000);

    char * bluetoothd_argv[] = { (char *) "/usr/libexec/bluetooth/bluetoothd", (char *) "-E", (char *) "-C", NULL };
    bool spawn_ok = subprocess_spawn_daemon(bluetoothd_argv);
    usleep(500000); /* give it a moment to register the adapter before the next step touches it */

    /* A freshly-restarted bluetoothd comes up with the adapter powered off
     * by default; this recovery only ever runs because Bluetooth was
     * expected to be on, so leaving it off after "fixing" it would just be
     * a different flavor of the same failure. */
    bt_control_enable();

    /* Even with power restored, bluealsa is left at whatever bluetoothd's
     * restart left it as -- the stock a2dp-source default, not the
     * a2dp-sink profile Bluetooth DAC mode needs to receive audio. Without
     * this, the phone would reconnect fine but drop the instant it tried
     * to stream, since no sink profile was registered to receive it. */
    bt_control_reapply_last_output_settings();

    DBG_LOG("bt_control: recovery attempt done (hci0 reset=%d, bluetoothd spawn=%d)\n", reset_ok, spawn_ok);
}

/* A single subprocess_run() timeout on `bluetoothctl show` isn't proof of
 * a genuine bluetoothd wedge: bluealsa can legitimately spend ~15s in a
 * normal futex wait for the remote device to send Start after Open (some
 * phones open the transport, never start, then close it), during which
 * bluetoothd can be slow to answer unrelated D-Bus queries. Requiring
 * several CONSECUTIVE timeouts (polled every ~5s, so spanning well over
 * 15s) before triggering recovery filters out that normal window while
 * still catching a real wedge, which by definition doesn't self-resolve.
 * "No default controller available" (a fast, clean response, not a
 * timeout) is a different, unambiguous signal that there's no adapter at
 * all, so that one still recovers immediately without a threshold. */
#define TIMEOUT_RECOVERY_THRESHOLD 4

/* Bluetooth could visibly turn itself off then back on right after boot
 * with no user action. Cause: this app's first status poll can land
 * before bluetoothd finishes registering the adapter object even though
 * hci0 already exists at the kernel level, so `bluetoothctl show`
 * correctly reports "No default controller available" for a brief, normal
 * startup window -- not because anything is wedged. That string is
 * normally treated as an immediate wedge signal (no threshold, unlike the
 * timeout path above) since mid-session it reliably is one, but during
 * this boot window it isn't, and the resulting recovery (kill bluetoothd,
 * reset, respawn, re-power) is itself the visible disable-then-enable
 * cycle that was reported. Fixed by giving "No default controller
 * available" the same not-immediately-fatal treatment as a timeout for a
 * grace window after this process's first poll, after which it reverts to
 * triggering recovery immediately.
 *
 * Real-device bug report: Bluetooth still visibly flickered on then off
 * shortly after a real power-down/power-up, despite this grace window
 * already existing. Root cause: bt_control_init_chip()'s own comment above
 * documents cold chip-init (bt_resume, a real UART firmware flash) taking up
 * to ~10-13s, but this window was only 8000ms -- shorter than the
 * documented worst case, so a slower boot could legitimately still be
 * mid-init when the grace window expired, at which point the very next
 * "No default controller available" poll was treated as a genuine wedge and
 * triggered bt_control_recover_wedged_daemon() (which unconditionally
 * force-repowers Bluetooth), producing exactly the on-then-off flicker
 * reported. Widened to comfortably clear the documented worst case with
 * headroom, rather than just matching it exactly. */
#define BT_BOOT_RACE_GRACE_MS 15000

static uint32_t bt_control_monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t) (ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* Guards bt_control_is_powered()'s own static bookkeeping below
 * (consecutive_timeouts, first_poll_seen/first_poll_tick) -- separate from
 * bt_chip_mutex above, not reused, since bt_control_is_powered() can itself
 * call into bt_control_recover_wedged_daemon() -> bt_control_enable(), which
 * takes bt_chip_mutex; reusing the same mutex here would self-deadlock.
 *
 * Real-device bug report: Bluetooth toggled off by the user sometimes
 * flipped back on by itself a moment later. Root cause: this function is
 * polled from gui.c's own periodic refresh_bt_icon() timer on the main
 * thread AND, concurrently, from inside bt_toggle_thread_func()'s own
 * post-toggle confirmation retry loop on a background thread -- with no
 * synchronization at all on the static counters below. Two callers racing
 * on a plain (non-atomic) consecutive_timeouts increment/reset could hit
 * TIMEOUT_RECOVERY_THRESHOLD from a lost update rather than genuine
 * consecutive timeouts, or interleave a stale first_poll_tick read -- either
 * way spuriously triggering bt_control_recover_wedged_daemon(), which
 * unconditionally force-repowers Bluetooth regardless of what the user just
 * asked for. Serializing the whole function (not just the counter updates)
 * is the simplest fix that can't leave the bookkeeping and the decision it
 * drives observing different, interleaved states. */
static pthread_mutex_t bt_status_mutex = PTHREAD_MUTEX_INITIALIZER;

static bool bt_control_is_powered_impl(void) {
    static int consecutive_timeouts = 0;
    static bool first_poll_seen = false;
    static uint32_t first_poll_tick = 0;

    if (!first_poll_seen) {
        first_poll_seen = true;
        first_poll_tick = bt_control_monotonic_ms();
    }

    if (!bt_control_adapter_present()) {
        DBG_LOG("bt_control: bt_control_is_powered: no adapter present\n");
        return false;
    }

    char out[2048];
    char * argv[] = { (char *) "bluetoothctl", (char *) "show", NULL };
    if (!subprocess_run(argv, out, sizeof(out))) {
        consecutive_timeouts++;
        DBG_LOG("bt_control: bt_control_is_powered: bluetoothctl show subprocess failed/timed out (%d/%d)\n",
                consecutive_timeouts, TIMEOUT_RECOVERY_THRESHOLD);
        if (consecutive_timeouts >= TIMEOUT_RECOVERY_THRESHOLD) {
            bt_control_recover_wedged_daemon();
            consecutive_timeouts = 0;
        }
        return false;
    }
    consecutive_timeouts = 0; /* any actual reply, even "not powered", means it isn't stuck */

    if (strstr(out, "Powered: yes") != NULL) return true;

    if (strstr(out, "No default controller available") != NULL) {
        uint32_t since_first_poll = bt_control_monotonic_ms() - first_poll_tick;
        if (since_first_poll < BT_BOOT_RACE_GRACE_MS) {
            DBG_LOG("bt_control: bt_control_is_powered: 'No default controller' %ums after first poll -- "
                    "within boot grace window, treating as still starting up, not a wedge\n",
                    (unsigned) since_first_poll);
        } else {
            bt_control_recover_wedged_daemon();
        }
    } else {
        DBG_LOG("bt_control: bt_control_is_powered: not powered (output: %.200s)\n", out);
    }
    return false;
}

bool bt_control_is_powered(void) {
    pthread_mutex_lock(&bt_status_mutex);
    bool result = bt_control_is_powered_impl();
    pthread_mutex_unlock(&bt_status_mutex);
    return result;
}

#define BT_INIT_TIMEOUT_MS 30000

/* Uses /usr/bin/bt_resume, not /usr/bin/bt_init -- the stock hiby_player
 * binary references bt_resume, bt_enable, bt_suspend, and `bt-device -l`
 * by exact path, never bt_init/bt_done/bluealsa_profile. bt_init reliably
 * creates a duplicate dbus-daemon; bt_resume avoids that since its own
 * dbus-daemon-startup lines are commented out in the script, so it just
 * uses whatever system dbus-daemon is already running. Otherwise
 * near-identical to bt_init: same chip detection/firmware flash, plus
 * pgrep guards around starting bluetoothd/bt-agent/bluealsa so it won't
 * double-start any of those if called again while they're still up. */
/* hci0 can come up already-present at the kernel level (module auto-load,
 * bluetoothd restoring a persisted "Powered" state) without bt_resume ever
 * having run this boot; in that case bluealsa never gets started, so
 * bluetoothd has no local A2DP source SDP record to offer and every
 * connect attempt fails with `a2dp-sink profile connect failed: Protocol
 * not available` regardless of the remote device. Since
 * bt_control_init_chip()'s early return is keyed purely on hci0 presence,
 * it was silently skipping bt_resume's own bluealsa-start step. This
 * ensures bluealsa specifically, every time, decoupled from whether hci0
 * needed a fresh bring-up; bt_control_apply_output_settings() still layers
 * DAC mode / --a2dp-volume on top via its own call sites once the user
 * touches a Bluetooth output setting. */
static void ensure_bluealsa_running(void) {
    if (count_matching("bluealsa") > 0) return;
    char * argv[] = { (char *) "bluealsa", (char *) "-p", (char *) "a2dp-source",
                       (char *) "--a2dp-volume", NULL };
    subprocess_spawn_daemon(argv);
}

bool bt_control_init_chip(void) {
    pthread_mutex_lock(&bt_chip_mutex);

    if (bt_control_adapter_present()) {
        ensure_bluealsa_running();
        pthread_mutex_unlock(&bt_chip_mutex);
        return true;
    }

    char out[512];
    char * bt_resume_argv[] = { (char *) "/usr/bin/bt_resume", NULL };
    subprocess_run_timeout(bt_resume_argv, out, sizeof(out), BT_INIT_TIMEOUT_MS);

    ensure_bluealsa_running(); /* belt-and-suspenders: bt_resume already starts it, but confirm rather than assume */
    bool result = bt_control_adapter_present();
    pthread_mutex_unlock(&bt_chip_mutex);
    return result;
}

/* /usr/bin/bt_enable -- matches hiby_player's own confirmed strings rather
 * than the bluetoothctl power on this function used before. It's
 * `bt-adapter --set Powered On` + `--set Discoverable On` together --
 * turning Bluetooth on for real also makes this device discoverable/
 * pairable by default, it isn't something that only happens during
 * Bluetooth DAC mode (see bt_control_apply_output_settings()'s own separate
 * discoverable toggle for that -- this and that are just two different call
 * sites setting the same underlying state). Confirmed live: on a device
 * where the chip is already flashed but the radio is administratively down
 * (the state the stock boot sequence itself leaves it in -- see
 * bt_control_init_chip()'s comment), bt_enable brings hci0 fully up in
 * about a second, no re-flash needed. */
void bt_control_enable(void) {
    pthread_mutex_lock(&bt_chip_mutex);
    char out[256];
    char * argv[] = { (char *) "/usr/bin/bt_enable", NULL };
    subprocess_run(argv, out, sizeof(out));
    pthread_mutex_unlock(&bt_chip_mutex);
}

/* /usr/bin/bt_disable, NOT /usr/bin/bt_suspend, despite bt_suspend being
 * the one actually referenced in hiby_player's strings (bt_disable isn't
 * referenced by the binary or anything else in the squashfs). Tried
 * bt_suspend first to match that evidence exactly, and confirmed live that
 * it's unsafe for a simple in-app toggle: it fully tears down the chip's
 * UART firmware link (kills brcm_patchram_plus/hciattach, rfkill block),
 * and re-flashing it back with bt_resume afterward reliably failed with
 * "Can't get device info: No such device" -- the same unrecoverable-
 * without-a-reboot failure this project already knew about from re-running
 * bt_init, this time confirmed even through the "correct" suspend-then-
 * resume pair. bt_suspend is presumably tied to the whole device's own
 * sleep/wake cycle, not a user-facing Bluetooth on/off switch. bt_disable
 * (`bt-adapter --set Discoverable Off` + `--set Powered Off`) only touches
 * the D-Bus adapter state, the same layer bt_enable operates at, so a
 * later bt_enable can bring it back in ~1s with no re-flash -- this is the
 * safe pairing. */
void bt_control_disable(void) {
    pthread_mutex_lock(&bt_chip_mutex);
    char out[256];
    char * argv[] = { (char *) "/usr/bin/bt_disable", NULL };
    subprocess_run(argv, out, sizeof(out));
    pthread_mutex_unlock(&bt_chip_mutex);
}

/* Parses "Paired: yes"/"Connected: yes" out of `bluetoothctl info <mac>`'s
 * output (see bluetooth_control.h -- confirmed exact field names/format
 * against a real device). */
static void query_device_state(const char * mac, bool * out_paired, bool * out_connected) {
    *out_paired = false;
    *out_connected = false;
    char out[2048];
    char * argv[] = { (char *) "bluetoothctl", (char *) "info", (char *) mac, NULL };
    if (!subprocess_run(argv, out, sizeof(out))) return;
    *out_paired = strstr(out, "Paired: yes") != NULL;
    *out_connected = strstr(out, "Connected: yes") != NULL;
}

bool bt_control_is_connected(void) {
    char devices_buf[4096];
    char * devices_argv[] = { (char *) "bluetoothctl", (char *) "paired-devices", NULL };
    if (!subprocess_run(devices_argv, devices_buf, sizeof(devices_buf))) return false;

    char * line_save = NULL;
    char * line = strtok_r(devices_buf, "\n", &line_save);
    while (line) {
        if (strncmp(line, "Device ", 7) == 0) {
            const char * mac_start = line + 7;
            const char * name_start = strchr(mac_start, ' ');
            if (name_start && (size_t) (name_start - mac_start) == 17) {
                char mac[18];
                memcpy(mac, mac_start, 17);
                mac[17] = '\0';
                bool paired, connected;
                query_device_state(mac, &paired, &connected);
                if (connected) return true;
            }
        }
        line = strtok_r(NULL, "\n", &line_save);
    }
    return false;
}

/* Real-device bug: the Bluetooth settings screen's own device list
 * (bt_scan_results[] in gui.c) only ever reflected paired/connected state
 * as of the last explicit scan (Rescan button, or opening the screen) --
 * unlike the top-bar icon, nothing refreshed it afterward, so a device
 * connecting/disconnecting elsewhere (or via this app's own now-working
 * Bluetooth audio output) never updated the list short of leaving and
 * re-entering the screen. Same underlying `bluetoothctl paired-devices` +
 * per-device `info` query bt_control_is_connected() already does, but
 * returning the full breakdown instead of collapsing it into one bool --
 * gui.c's periodic icon-refresh poll calls this once and derives its own
 * "is anything connected" from the result, instead of paying for this same
 * per-device loop twice every cycle. Returns how many entries were written
 * into out[] (capped at max_count, same convention as bt_control_scan()),
 * or -1 if the underlying `bluetoothctl paired-devices` call itself failed
 * (hung/killed by subprocess_run()'s own timeout) -- distinct from a
 * genuine "0 paired devices" so gui.c's own merge logic (see
 * poll_refresh_bt_icon()'s own comment on why this distinction matters)
 * can tell a real empty list from a query that didn't actually run, rather
 * than treating a transient subprocess hiccup as "nothing is paired
 * anymore" and wiping every device's known state for a cycle. Blocking;
 * call off the UI thread. */
int bt_control_list_paired_states(bt_device_t * out, int max_count) {
    char devices_buf[4096];
    char * devices_argv[] = { (char *) "bluetoothctl", (char *) "paired-devices", NULL };
    if (!subprocess_run(devices_argv, devices_buf, sizeof(devices_buf))) return -1;

    int count = 0;
    char * line_save = NULL;
    char * line = strtok_r(devices_buf, "\n", &line_save);
    while (line && count < max_count) {
        if (strncmp(line, "Device ", 7) == 0) {
            const char * mac_start = line + 7;
            const char * name_start = strchr(mac_start, ' ');
            if (name_start && (size_t) (name_start - mac_start) == 17) {
                memcpy(out[count].mac, mac_start, 17);
                out[count].mac[17] = '\0';
                snprintf(out[count].name, sizeof(out[count].name), "%s", name_start + 1);
                query_device_state(out[count].mac, &out[count].paired, &out[count].connected);
                count++;
            }
        }
        line = strtok_r(NULL, "\n", &line_save);
    }
    return count;
}

/* BlueZ real-device incident: a plain `bluetoothctl --timeout N scan on`
 * scans BOTH bearers at once (ControllerMode=dual in main.conf), and for a
 * dual-mode accessory that advertises over LE as well as classic (confirmed
 * on a real pair of earbuds -- companion-app features like "Find My Buds"
 * use LE, the actual A2DP/AVRCP audio path is classic BR/EDR only), whichever
 * bearer's advertisement/inquiry result lands first in bluetoothd wins the
 * device object's bearer identity for the *next* `pair`. When that's LE, the
 * result is an LE-only bond (an LTK, no classic link key) that looks
 * "Paired: yes" to a casual query but can never actually carry A2DP/AVRCP.
 * Confirmed live: bluetoothd's own periodic LE auto-reconnect attempt to
 * such a device then fails every time (`ATT bt_io_connect(): connect: No
 * route to host`), and the kernel eventually emits a real
 * MGMT_EV_DEVICE_UNPAIRED for it -- which BlueZ (src/device.c
 * device_set_unpaired()) treats as *fully* unpaired since there was never a
 * working classic bond to fall back on, wiping the stored keys and deleting
 * the device object outright (src/adapter.c remove_temp_devices()). That's
 * the actual root cause behind "forget device, then it vanishes for good
 * even though other devices can still see it": not a caching bug in this
 * app, but an LE-only bond silently going stale server-side.
 *
 * Fix: restrict discovery to the classic bearer only, via bluetoothctl's
 * `scan.transport bredr` submenu command. That command only takes effect
 * for a discovery *started in the same process* (BlueZ's SetDiscoveryFilter
 * is scoped to the calling D-Bus connection, and bluetoothctl's own filter
 * state is a plain in-process static, never persisted) -- so unlike every
 * other bluetoothctl call in this file, this needs one persistent
 * interactive session (`scan.transport bredr` + `scan on` + wait + `scan
 * off`, all through the same stdin pipe) rather than one-shot argv
 * invocations. The actual device list is still read back afterward via the
 * normal one-shot `bluetoothctl devices` call below -- that's fine, because
 * by then the device objects already exist server-side in bluetoothd with
 * the classic-only bearer identity this filter guaranteed. */
static void run_bredr_scan(int seconds) {
    pid_t pid;
    int write_fd;
    char * argv[] = { (char *) "bluetoothctl", NULL };
    if (!subprocess_popen_stdin(argv, &pid, &write_fd)) return;

    const char * start_cmds = "scan.transport bredr\nscan on\n";
    ssize_t ignored = write(write_fd, start_cmds, strlen(start_cmds));
    (void) ignored;

    sleep(seconds > 0 ? (unsigned int) seconds : 1);

    const char * stop_cmds = "scan off\nquit\n";
    ignored = write(write_fd, stop_cmds, strlen(stop_cmds));
    (void) ignored;

    close(write_fd);
    subprocess_terminate(pid); /* reaps it either way -- `quit` alone isn't guaranteed to have taken effect yet */
}

int bt_control_scan(int seconds, bt_device_t * out, int max_count) {
    run_bredr_scan(seconds); /* blocks for `seconds` -- that's the point */

    char devices_buf[8192];
    char * devices_argv[] = { (char *) "bluetoothctl", (char *) "devices", NULL };
    if (!subprocess_run(devices_argv, devices_buf, sizeof(devices_buf))) return 0;

    int count = 0;
    char * line_save = NULL;
    char * line = strtok_r(devices_buf, "\n", &line_save);
    while (line && count < max_count) {
        /* "Device XX:XX:XX:XX:XX:XX Some Name Here" */
        if (strncmp(line, "Device ", 7) == 0) {
            const char * mac_start = line + 7;
            const char * name_start = strchr(mac_start, ' ');
            if (name_start && (size_t) (name_start - mac_start) == 17) {
                memcpy(out[count].mac, mac_start, 17);
                out[count].mac[17] = '\0';
                snprintf(out[count].name, sizeof(out[count].name), "%s", name_start + 1);
                query_device_state(out[count].mac, &out[count].paired, &out[count].connected);
                count++;
            }
        }
        line = strtok_r(NULL, "\n", &line_save);
    }
    return count;
}

bool bt_control_connect(const char * mac) {
    char out[512];

    char * pair_argv[] = { (char *) "bluetoothctl", (char *) "pair", (char *) mac, NULL };
    subprocess_run(pair_argv, out, sizeof(out)); /* no-op if already paired -- not fatal either way */

    char * trust_argv[] = { (char *) "bluetoothctl", (char *) "trust", (char *) mac, NULL };
    subprocess_run(trust_argv, out, sizeof(out));

    char * connect_argv[] = { (char *) "bluetoothctl", (char *) "connect", (char *) mac, NULL };
    if (!subprocess_run(connect_argv, out, sizeof(out))) return false;
    return strstr(out, "Failed") == NULL;
}

bool bt_control_disconnect(const char * mac) {
    char out[512];
    char * argv[] = { (char *) "bluetoothctl", (char *) "disconnect", (char *) mac, NULL };
    if (!subprocess_run(argv, out, sizeof(out))) return false;
    return strstr(out, "Failed") == NULL;
}

bool bt_control_forget(const char * mac) {
    char out[512];
    char * argv[] = { (char *) "bluetoothctl", (char *) "remove", (char *) mac, NULL };
    if (!subprocess_run(argv, out, sizeof(out))) return false;
    return strstr(out, "Failed") == NULL;
}

/* Real-device finding: with --a2dp-volume on, bluealsa's SoftVolume applies
 * the phone's AVRCP absolute-volume value as a straight LINEAR PCM
 * amplitude gain -- confirmed with measured data across one phone's full
 * slider range (10%/20%/30%/50%/80%/100% phone volume produced raw values
 * 9/17/26/43/68/85, i.e. consistently ~0.85x phone% -- genuinely linear,
 * and this phone's own ceiling is 85, not the full 0-127 AVRCP range).
 * Human hearing perceives loudness logarithmically, so a linear amplitude
 * curve front-loads nearly the whole perceived range into the bottom half
 * of the slider (10% to 50% amplitude is already about +14dB) -- confirmed
 * on a real device as "maxed out by 50%, already too loud around 30%",
 * which is a real risk with sensitive headphones. There's no bluealsa flag
 * for this (checked --help). Fixed entirely from userspace, no need to
 * patch bluealsa itself: `bluealsa-cli monitor -p` streams every volume
 * change the phone sends as plain text
 * ("PropertyChanged <pcm-path> Volume 0xLLRR", confirmed live), and
 * `bluealsa-cli volume <pcm-path> <L> <R>` can write an arbitrary value
 * back -- this intercepts the phone's raw updates and rewrites them
 * through a proper dB taper before bluealsa ever applies them. */
/* Real-device incident: 40dB (a fairly standard "full range" figure for
 * consumer audio taper curves) turned out too aggressive here -- confirmed
 * live it compresses nearly the whole practical phone-slider range into a
 * narrow, barely-audible band (e.g. writing a mid-scale raw value of 64
 * settled, correctly per this formula, at a corrected 13 -- about 10% of
 * full scale), which reads as "stuck near-silent" even though it's
 * technically still responding. 20dB is a gentler, still-standard choice
 * that keeps meaningfully more of the corrected range in the audible
 * portion of the phone's slider. */
#define BT_VOLUME_CURVE_TAPER_DB 20.0
#define BT_VOLUME_RAW_MAX 127 /* AVRCP absolute volume's own 7-bit spec range -- what bluealsa's Volume property scale actually is, regardless of what any given phone's own slider happens to top out at (this one caps at 85) */

static pthread_t bt_volume_curve_thread;
static bool bt_volume_curve_active = false;
static pid_t bt_volume_curve_monitor_pid = -1;

static void * bt_volume_curve_thread_func(void * arg) {
    (void) arg;
    /* Starts at the full spec range rather than this phone's actual 85 --
     * unknown at startup, and erring toward treating a given raw value as a
     * SMALLER fraction of "the phone's max" makes the correction quieter
     * than strictly necessary until it self-calibrates upward on the first
     * observed value, rather than louder. Given the sensitive-headphones
     * motivation for this whole feature, quieter-until-calibrated is the
     * right direction to err in. */
    int observed_phone_max = BT_VOLUME_RAW_MAX;
    int last_written_raw = -1; /* our own echo from the write below -- see the loop */

    char * argv[] = { (char *) "bluealsa-cli", (char *) "monitor", (char *) "-p", NULL };
    pid_t pid;
    int read_fd;
    if (!subprocess_popen(argv, &pid, &read_fd)) return NULL;
    bt_volume_curve_monitor_pid = pid;

    FILE * f = fdopen(read_fd, "r");
    if (!f) {
        subprocess_terminate(pid);
        close(read_fd);
        return NULL;
    }

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char path[256];
        unsigned int raw_hex;
        if (sscanf(line, "PropertyChanged %255s Volume 0x%x", path, &raw_hex) != 2) continue;

        int raw = (int) ((raw_hex >> 8) & 0xFF); /* left byte -- left/right always move together for this stereo sync */
        if (raw == last_written_raw) continue; /* our own write below echoing back -- not a real phone-driven change */

        if (raw > observed_phone_max) observed_phone_max = raw;
        double fraction = (double) raw / (double) observed_phone_max;
        double gain = pow(10.0, (fraction - 1.0) * BT_VOLUME_CURVE_TAPER_DB / 20.0);
        int corrected = (int) (gain * BT_VOLUME_RAW_MAX + 0.5);
        if (corrected < 0) corrected = 0;
        if (corrected > BT_VOLUME_RAW_MAX) corrected = BT_VOLUME_RAW_MAX;

        /* Real-device incident: set BEFORE issuing the write below, not
         * after -- confirmed live that a single write triggers the monitor
         * to print the echoed PropertyChanged line TWICE, and since
         * subprocess_run() blocks on bluealsa-cli's own D-Bus call
         * completing (not on the separate, async PropertyChanged signal
         * actually reaching this process's monitor subprocess), there was
         * a real window where an echo could be read before
         * last_written_raw caught up -- misreading our own write as a new
         * phone-driven change and re-applying the (non-idempotent) dB
         * taper to its own output. That's not just a redundant correction:
         * iterating a nonlinear taper on its own result diverges fast,
         * confirmed on a real device as volume randomly swinging between
         * 0% and 100%. */
        last_written_raw = corrected;
        char corrected_str[8];
        snprintf(corrected_str, sizeof(corrected_str), "%d", corrected);
        char * vol_argv[] = { (char *) "bluealsa-cli", (char *) "volume", path, corrected_str, corrected_str, NULL };
        subprocess_run(vol_argv, NULL, 0);
    }

    fclose(f); /* also closes read_fd */
    return NULL;
}

/* Called from bt_control_apply_output_settings() -- see this section's own
 * comment for what this corrects and why. */
static void bt_volume_curve_stop(void) {
    if (!bt_volume_curve_active) return;
    bt_volume_curve_active = false;
    /* The thread is almost certainly blocked in fgets(), waiting on the
     * monitor subprocess's next line -- which may never come on its own.
     * Killing that subprocess here (not from inside the thread) forces
     * EOF, which is what actually unblocks fgets() and lets the thread
     * reach its own natural return. */
    if (bt_volume_curve_monitor_pid > 0) subprocess_terminate(bt_volume_curve_monitor_pid);
    pthread_join(bt_volume_curve_thread, NULL);
    bt_volume_curve_monitor_pid = -1;
}

static void bt_volume_curve_start(void) {
    bt_volume_curve_stop(); /* defensive -- never start a second one on top of an existing one */
    bt_volume_curve_active = true;
    pthread_create(&bt_volume_curve_thread, NULL, bt_volume_curve_thread_func, NULL);
}

/* ---- Source-direction (a2dp-source, this device streaming TO headphones)
 * AVRCP volume sync -- the mirror image of bt_volume_curve_start()/_stop()
 * above, which corrects the OTHER direction (a2dp-sink/DAC mode, a phone
 * streaming TO this device). Real-device bug report: once Bluetooth output
 * itself started working (see audio_set_bt_output()'s history in audio.h),
 * the headphones' own volume buttons had no effect on this app's playback
 * volume, and this app's own volume slider had no effect on whatever level
 * the headphones themselves show/remember.
 *
 * No dB reshaping needed here, unlike bt_volume_curve_thread_func above:
 * confirmed live via `bluealsa-cli info` that a fresh a2dp-source PCM's
 * SoftVolume is off by default (bluealsa passes PCM through unscaled), so
 * this app's own volume_percent_to_gain() taper in audio.c remains the
 * ONLY real gain stage regardless of what AVRCP's raw value says -- that
 * raw 0-127 number is purely a position indicator kept in sync with this
 * app's own 0.0-1.0 percent via a plain linear mapping, not a second
 * amplitude control. (If a future bluealsa version or a specific accessory
 * ever turns SoftVolume on by default for this PCM, this would start
 * double-attenuating -- re-check `bluealsa-cli info` first if music turns
 * out quieter than expected specifically over Bluetooth.) */
#define BT_SOURCE_VOLUME_MAX 127

static bool find_source_pcm_path(char * out, size_t out_size) {
    char list_out[4096];
    char * argv[] = { (char *) "bluealsa-cli", (char *) "list-pcms", NULL };
    if (!subprocess_run(argv, list_out, sizeof(list_out))) {
        /* Real-device bug report: BT "hangs" (needs disable/enable +
         * reconnect to recover) sometime after reconnecting and starting a
         * new track -- not yet reproduced/confirmed, but bt_control_is_
         * powered()'s own wedge detection only watches `bluetoothctl show`;
         * a bluealsa-cli call timing out/failing here was previously
         * completely silent, so if bluealsa itself (not bluetoothd) is
         * what's actually wedging, there was nothing in the log to show it.
         * Logged, not auto-recovered -- this alone doesn't yet know it's a
         * genuine wedge vs. a normal transient "nothing connected right
         * now" (see this function's own callers), so it shouldn't trigger
         * bt_control_recover_wedged_daemon() itself without more evidence
         * that this is the actual failure mode. */
        DBG_LOG("bt_control: find_source_pcm_path: bluealsa-cli list-pcms failed/timed out\n");
        return false;
    }

    char * line_save = NULL;
    char * line = strtok_r(list_out, "\n", &line_save);
    while (line) {
        /* "a2dpsrc" (not "a2dpsnk") distinguishes this from a DAC-mode PCM
         * if one ever coexisted; "/sink" is bluealsa's own role name for
         * the PCM WE write into (confirmed live: `bluealsa-cli list-pcms`
         * on a real connected device returned exactly
         * "/org/bluealsa/hci0/dev_XX_XX_XX_XX_XX_XX/a2dpsrc/sink"). */
        if (strstr(line, "/a2dpsrc/sink") != NULL) {
            snprintf(out, out_size, "%s", line);
            return true;
        }
        line = strtok_r(NULL, "\n", &line_save);
    }
    return false;
}

/* Public wrapper around the same check find_source_pcm_path() above already
 * does for the AVRCP volume-sync feature -- a "/a2dpsrc/sink" PCM only
 * exists in bluealsa's own list once a real audio-capable accessory has
 * actually negotiated the A2DP sink role with this device acting as
 * a2dp-source, which is a stronger signal than bt_control_is_connected()/
 * bt_control_list_paired_states() (those report ANY paired device with an
 * active connection, which could be a non-audio BLE peripheral with no A2DP
 * profile at all). Used for the topbar's "BT headphone connected" icon
 * (gui.c) -- same subprocess cost as everything else here, call off the UI
 * thread. */
bool bt_control_is_a2dp_source_connected(void) {
    char path[256];
    return find_source_pcm_path(path, sizeof(path));
}

bool bt_control_get_connected_device_mac(char * out, size_t out_size) {
    char path[256];
    if (!find_source_pcm_path(path, sizeof(path))) return false;

    const char * dev = strstr(path, "dev_");
    if (!dev) return false;
    dev += 4; /* skip "dev_" */

    char mac[18];
    if (strlen(dev) < 17) return false; /* "XX_XX_XX_XX_XX_XX" */
    for (int i = 0; i < 17; i++) mac[i] = (dev[i] == '_') ? ':' : dev[i];
    mac[17] = '\0';

    snprintf(out, out_size, "%s", mac);
    return true;
}

bool bt_control_get_connected_device_codec(char * out, size_t out_size) {
    char path[256];
    if (!find_source_pcm_path(path, sizeof(path))) return false;

    char info_out[2048];
    char * argv[] = { (char *) "bluealsa-cli", (char *) "info", path, NULL };
    if (!subprocess_run(argv, info_out, sizeof(info_out))) return false;

    /* "Selected codec: AAC" -- confirmed live via `bluealsa-cli info
     * <pcm-path>` (also reports "Available codecs: SBC AAC", but that's
     * every codec the accessory advertised support for, not what's
     * actually in use right now). */
    const char * line = strstr(info_out, "Selected codec:");
    if (!line) return false;
    line += strlen("Selected codec:");
    while (*line == ' ') line++;

    char codec[32];
    int i = 0;
    while (line[i] != '\0' && line[i] != '\n' && line[i] != '\r' && i < (int) sizeof(codec) - 1) {
        codec[i] = line[i];
        i++;
    }
    codec[i] = '\0';
    if (i == 0) return false;

    snprintf(out, out_size, "%s", codec);
    return true;
}

static pthread_t bt_source_vol_sync_thread;
static bool bt_source_vol_sync_active = false;
static pid_t bt_source_vol_sync_monitor_pid = -1;
static atomic_int bt_source_vol_pending_percent = -1;

/* Set by whichever direction writes/observes a value most recently, so the
 * other direction recognizes it as already in sync instead of re-writing
 * it right back -- same reasoning as bt_volume_curve_thread_func's own
 * last_written_raw, just shared between two directions here instead of one
 * direction echoing itself. */
static int bt_source_vol_last_synced_raw = -1;

static void bt_source_push_app_volume_if_changed(float * last_synced_app_percent) {
    float current_percent = audio_get_volume();
    if (current_percent == *last_synced_app_percent) return;

    int raw = (int) (current_percent * (float) BT_SOURCE_VOLUME_MAX + 0.5f);
    if (raw < 0) raw = 0;
    if (raw > BT_SOURCE_VOLUME_MAX) raw = BT_SOURCE_VOLUME_MAX;

    if (raw == bt_source_vol_last_synced_raw) {
        *last_synced_app_percent = current_percent;
        return;
    }

    char path[256];
    if (!find_source_pcm_path(path, sizeof(path))) return;

    char raw_str[8];
    snprintf(raw_str, sizeof(raw_str), "%d", raw);
    char * vol_argv[] = { (char *) "bluealsa-cli", (char *) "volume", path, raw_str, raw_str, NULL };
    if (!subprocess_run(vol_argv, NULL, 0)) return;

    bt_source_vol_last_synced_raw = raw;
    *last_synced_app_percent = current_percent;
    hiby_sys_server_report_volume((int) (current_percent * 100.0f + 0.5f));
}

static void * bt_source_vol_sync_thread_func(void * arg) {
    (void) arg;

    /* Establish the already-visible player value before subscribing to
     * bluealsa property changes. Starting the monitor first can queue the
     * accessory's remembered pre-sync value, which would then overwrite
     * the app even if we push immediately afterward. */
    float last_synced_app_percent = -1.0f;
    bt_source_push_app_volume_if_changed(&last_synced_app_percent);

    char * argv[] = { (char *) "bluealsa-cli", (char *) "monitor", (char *) "-p", NULL };
    pid_t pid;
    int read_fd;
    if (!subprocess_popen(argv, &pid, &read_fd)) return NULL;
    bt_source_vol_sync_monitor_pid = pid;

    char buf[512];
    size_t buf_len = 0;

    while (bt_source_vol_sync_active) {
        /* Push app-driven state BEFORE reading monitor events. In
         * particular, the first iteration must make the already-visible
         * player percentage authoritative before bluealsa's monitor can
         * report the accessory's remembered value; doing this afterward
         * let that startup event silently change audio_get_volume() while
         * the slider/topbar still showed the old app value. */
        bt_source_push_app_volume_if_changed(&last_synced_app_percent);

        /* Bounded, not a blocking fgets() like bt_volume_curve_thread_func
         * above -- this loop also needs to notice this app's OWN volume
         * changing (UI slider, hardware buttons), which has no fd to
         * select() on, so it polls this monitor's pipe with a short
         * timeout instead of blocking on it indefinitely. 500ms is
         * imprecise for a "keep two volume controls in sync" feature (not
         * a latency-sensitive control path), matched on both sides of this
         * loop -- see the push check below. */
        struct pollfd pfd = { .fd = read_fd, .events = POLLIN };
        int pr = poll(&pfd, 1, 500);

        if (pr > 0 && (pfd.revents & POLLIN)) {
            if (buf_len >= sizeof(buf) - 1) buf_len = 0; /* defensive -- a line this long can't be a real PropertyChanged message, drop and resync */
            ssize_t n = read(read_fd, buf + buf_len, sizeof(buf) - 1 - buf_len);
            if (n <= 0) break; /* monitor subprocess died or its pipe closed */
            buf_len += (size_t) n;
            buf[buf_len] = '\0';

            char * line_start = buf;
            char * newline;
            while ((newline = memchr(line_start, '\n', buf_len - (size_t) (line_start - buf))) != NULL) {
                *newline = '\0';
                char path[256];
                unsigned int raw_hex;
                if (sscanf(line_start, "PropertyChanged %255s Volume 0x%x", path, &raw_hex) == 2 &&
                    strstr(path, "/a2dpsrc/sink") != NULL) {
                    int raw = (int) ((raw_hex >> 8) & 0xFF); /* left byte -- left/right always move together for this stereo sync */
                    if (raw != bt_source_vol_last_synced_raw) {
                        bt_source_vol_last_synced_raw = raw;
                        last_synced_app_percent = (float) raw / (float) BT_SOURCE_VOLUME_MAX;
                        audio_set_volume(last_synced_app_percent);
                        int percent = (raw * 100 + BT_SOURCE_VOLUME_MAX / 2) /
                                      BT_SOURCE_VOLUME_MAX;
                        atomic_store_explicit(&bt_source_vol_pending_percent, percent,
                                              memory_order_release);
                    }
                }
                line_start = newline + 1;
            }
            size_t remaining = buf_len - (size_t) (line_start - buf);
            memmove(buf, line_start, remaining);
            buf_len = remaining;
        }

    }

    close(read_fd);
    return NULL;
}

void bt_control_source_volume_sync_start(void) {
    if (bt_source_vol_sync_active) return; /* already running -- matches bt_volume_curve_start()'s own idempotency, just without the defensive re-stop since callers here are expected not to double-start (see gui.c's own gating) */
    bt_source_vol_sync_active = true;
    bt_source_vol_last_synced_raw = -1;
    atomic_store_explicit(&bt_source_vol_pending_percent, -1, memory_order_relaxed);
    pthread_create(&bt_source_vol_sync_thread, NULL, bt_source_vol_sync_thread_func, NULL);
}

void bt_control_source_volume_sync_stop(void) {
    if (!bt_source_vol_sync_active) return;
    bt_source_vol_sync_active = false; /* checked at the top of the thread's own loop -- the poll() timeout above (<=500ms) is what actually lets it notice and exit, not this flag alone */
    if (bt_source_vol_sync_monitor_pid > 0) subprocess_terminate(bt_source_vol_sync_monitor_pid);
    pthread_join(bt_source_vol_sync_thread, NULL);
    bt_source_vol_sync_monitor_pid = -1;
    atomic_store_explicit(&bt_source_vol_pending_percent, -1, memory_order_release);
}

bool bt_control_source_volume_sync_consume_percent(int * out_percent) {
    int percent = atomic_exchange_explicit(&bt_source_vol_pending_percent, -1,
                                           memory_order_acq_rel);
    if (percent < 0) return false;
    if (out_percent) *out_percent = percent;
    return true;
}

/* See bt_control_output_disconnect_watch_start()'s own doc comment
 * (bluetooth_control.h) for what this is and why. Plain `monitor`, no `-p`
 * needed -- confirmed via `strings` on the real bluealsa-cli binary that
 * PCMAdded/PCMRemoved come from its base InterfacesAdded/InterfacesRemoved
 * subscription (path_namespace='/org/bluealsa'), which is always active;
 * `-p` only adds the separate PropertyChanged stream (Volume/Codec/Running/
 * SoftVolume) the two volume-sync threads above already use -- this doesn't
 * need any of that. "/a2dpsrc/sink" matches find_source_pcm_path()'s own
 * naming exactly: the PCM this app itself writes local playback into
 * (audio_output.c's `aplay -D bluealsa`), not any other PCM bluealsa might
 * have (e.g. one from DAC mode). */
static pthread_t bt_output_disconnect_thread;
static bool bt_output_disconnect_active = false;
static pid_t bt_output_disconnect_monitor_pid = -1;
static volatile bool bt_output_disconnect_flag = false;

static void * bt_output_disconnect_thread_func(void * arg) {
    (void) arg;

    char * argv[] = { (char *) "bluealsa-cli", (char *) "monitor", NULL };
    pid_t pid;
    int read_fd;
    if (!subprocess_popen(argv, &pid, &read_fd)) {
        DBG_LOG("bt_control: output_disconnect_watch: failed to spawn bluealsa-cli monitor\n");
        return NULL;
    }
    bt_output_disconnect_monitor_pid = pid;
    DBG_LOG("bt_control: output_disconnect_watch: monitor started (pid %d)\n", (int) pid);

    FILE * f = fdopen(read_fd, "r");
    if (!f) {
        subprocess_terminate(pid);
        close(read_fd);
        return NULL;
    }

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char path[256];
        if (sscanf(line, "PCMRemoved %255s", path) == 1) {
            if (strstr(path, "/a2dpsrc/sink") != NULL) {
                DBG_LOG("bt_control: output_disconnect_watch: PCMRemoved %s -- flagging disconnect\n", path);
                bt_output_disconnect_flag = true;
            }
        } else if (strncmp(line, "PCMAdded ", 9) == 0) {
            /* Diagnostic only -- real-device bug report: BT "hangs" some
             * time after reconnecting and starting a new track. If a
             * reconnect's own AVDTP renegotiation briefly tears down and
             * recreates this PCM (PCMRemoved immediately followed by
             * PCMAdded, not a genuine physical disconnect), that would
             * already be visible here as a spurious flag right after
             * reconnecting -- logged so a future capture can show whether
             * that's what's happening, without changing behavior yet. */
            DBG_LOG("bt_control: output_disconnect_watch: %s", line);
        }
    }

    DBG_LOG("bt_control: output_disconnect_watch: monitor exited (pid %d)\n", (int) pid);
    fclose(f); /* also closes read_fd -- reached once bt_control_output_disconnect_watch_stop() kills the monitor subprocess and fgets() sees EOF, same as bt_volume_curve_stop()'s identical shutdown pattern above */
    return NULL;
}

void bt_control_output_disconnect_watch_start(void) {
    if (bt_output_disconnect_active) return;
    bt_output_disconnect_active = true;
    bt_output_disconnect_flag = false;
    pthread_create(&bt_output_disconnect_thread, NULL, bt_output_disconnect_thread_func, NULL);
}

void bt_control_output_disconnect_watch_stop(void) {
    if (!bt_output_disconnect_active) return;
    bt_output_disconnect_active = false;
    DBG_LOG("bt_control: output_disconnect_watch: stopping (pid %d)\n", (int) bt_output_disconnect_monitor_pid);
    if (bt_output_disconnect_monitor_pid > 0) subprocess_terminate(bt_output_disconnect_monitor_pid);
    pthread_join(bt_output_disconnect_thread, NULL);
    bt_output_disconnect_monitor_pid = -1;
}

bool bt_control_output_disconnect_consume(void) {
    if (!bt_output_disconnect_flag) return false;
    bt_output_disconnect_flag = false;
    return true;
}

/* Recorded so bt_control_reapply_last_output_settings() (the wedge
 * recovery in bt_control_is_powered(), above) can restore this after a
 * bluetoothd restart -- a fresh bluetoothd/bt_resume-equivalent bring-up
 * always comes back up in the plain a2dp-source default, not whatever
 * profile was actually requested last. */
static bool last_applied_dac_mode_enabled = false;
static bool last_applied_volume_sync_enabled = false;
static bool output_settings_ever_applied = false;

/* Serializes bt_control_apply_output_settings() against itself across the
 * two independent threads that can call it (see that function's own
 * comment for the real-device race this fixes) -- a plain, non-recursive
 * mutex is safe here specifically because bt_control_recover_wedged_
 * daemon() never holds it while calling into bt_control_apply_output_
 * settings() (it only reaches that indirectly, via bt_control_reapply_
 * last_output_settings(), and never locks this mutex itself). */
static pthread_mutex_t bt_daemon_respawn_mutex = PTHREAD_MUTEX_INITIALIZER;

#define BLUEALSA_STARTUP_LOG_PATH "/usr/data/bluealsa_startup.log"

/* Real-device incident: bluealsa's own D-Bus query to BlueZ's
 * ObjectManager (GetManagedObjects, used to discover the adapter/devices
 * at startup) timed out in one specific capture ("Couldn't get managed
 * objects: Timeout was reached"). An earlier version of this function
 * blocked here for 800ms to check the log for that before letting
 * bt_control_apply_output_settings() continue on to starting bt-agent/
 * bluealsa-aplay/discoverable, retrying the spawn if it found the error --
 * but a later real-device regression (audio that used to actually play
 * stopped working, connections dropping instead) traced back to timing
 * changes in this exact function versus the version last confirmed
 * working, and that 800ms block delaying everything after it was the
 * clearest concrete difference. Still logs bluealsa's startup output for
 * visibility, but no longer blocks or retries on it -- matches the timing
 * of the plain fire-and-forget spawn this replaced. Only actually writes
 * the log file on a TEST_BUILD_TAG build (see debug_log.h's own comment on
 * why) -- a real flash deployment has nobody to read it and no reason to
 * spend flash writes creating it on every single profile switch. */
static bool spawn_bluealsa_and_verify(char * const argv[]) {
#if defined(TEST_BUILD_TAG)
    return subprocess_spawn_daemon_logged(argv, BLUEALSA_STARTUP_LOG_PATH);
#else
    return subprocess_spawn_daemon_logged(argv, NULL);
#endif
}

bool bt_control_apply_output_settings(bool dac_mode_enabled, bool volume_sync_enabled) {
    /* Real-device concurrency finding: this function and bt_control_recover_
     * wedged_daemon() (via bt_control_reapply_last_output_settings() at the
     * end of its own recovery sequence) each independently kill-then-respawn
     * bluealsa/bt-agent, with nothing previously stopping both from doing so
     * at once -- e.g. a wedge detected on the ~5s connection poll at the
     * same moment the user (or a BT disconnect's own auto-stop path) touches
     * a DAC-mode/output-setting toggle. That's the exact "9 accumulated
     * bt-agent processes" failure mode subprocess_kill_all_matching() below was
     * originally added to fix (see its own comment), just reachable through
     * two separate call sites racing instead of one call site respawning
     * without cleanup. bt_daemon_respawn_mutex below serializes them.
     * Both call sites already run on their own background thread (gui.c's
     * start_bt_apply_output_settings() thread wrapper for this function;
     * refresh_bt_icon_thread_func() for the wedge-recovery path), never the
     * UI thread, so blocking here on the other one finishing can't freeze
     * the UI -- it just makes one wait briefly instead of interleaving with
     * the other's kill/respawn sequence.
     *
     * Audit finding: last_applied_dac_mode_enabled/last_applied_volume_
     * sync_enabled/output_settings_ever_applied used to be written BEFORE
     * this lock, while bt_control_reapply_last_output_settings() (the
     * wedge-recovery path mentioned above) read them with no lock at all.
     * A toggle landing at the same moment as a wedge-triggered reapply
     * could observe an inconsistent combination of the two flags. Writing
     * them under the same mutex that already serializes the two call
     * sites' actual daemon respawns closes this with no new lock needed. */
    pthread_mutex_lock(&bt_daemon_respawn_mutex);
    last_applied_dac_mode_enabled = dac_mode_enabled;
    last_applied_volume_sync_enabled = volume_sync_enabled;
    output_settings_ever_applied = true;

    /* Sink and source are mutually exclusive here, matching the real
     * firmware's own /usr/bin/bluealsa_profile script exactly (confirmed by
     * reading it directly): it always runs bluealsa with a SINGLE `-p`
     * profile, never `-p a2dp-source -p a2dp-sink` together. An earlier
     * version of this code ran both simultaneously (to keep our own
     * outgoing-audio-to-headphones path alive at the same time), which is
     * not a combination the stock firmware itself ever exercises --
     * plausibly implicated in a real-device freeze once actual audio
     * started flowing. Since local playback is already blocked while DAC
     * mode is on (see play_track_at_from()/toggle_play_pause() in gui.c),
     * there's nothing that would need the source profile at the same time
     * anyway. */
    /* bluealsa_profile also kills and respawns bt-agent on every profile
     * switch (source uses a PIN file, sink doesn't) -- confirmed by reading
     * the script directly. An earlier version of this function left
     * whatever bt-agent bt_resume/bt_init originally started running
     * untouched across profile switches instead of matching that.
     *
     * Real-device incident: this used to shell out to `killall bluealsa` /
     * `killall aplay` / `killall bluealsa-aplay` / `killall bt-agent`
     * directly, same as kill_stock_player_if_running() originally did for
     * hiby_player -- and just as unreliably: confirmed live with NINE
     * accumulated bt-agent processes after repeated DAC-mode/volume-sync
     * toggles, each one apparently surviving `killall` and each new toggle
     * spawning yet another on top. Nine agents simultaneously fighting to
     * register with bluetoothd over D-Bus is a real, confirmed cause of
     * broader Bluetooth instability (agent registration churn, D-Bus
     * congestion), not just leaked processes. subprocess_kill_all_matching() (see its
     * own comment) is the same ps-output-based approach already proven
     * reliable for hiby_player/dbus-daemon cleanup. "bluealsa" as the
     * search string also matches bluealsa-aplay (it contains "bluealsa" as
     * a substring), so this covers both in one pass. */
    /* Stopped unconditionally, even when about to turn DAC mode back on --
     * the PCM path it was watching is about to become stale the moment
     * bluealsa restarts below anyway, and bt_volume_curve_start() gets
     * called again further down when relevant. */
    bt_volume_curve_stop();
    bt_dac_info_monitor_stop();

    subprocess_kill_all_matching("bluealsa");
    subprocess_kill_all_matching("aplay");
    subprocess_kill_all_matching("bt-agent");
    /* usleep(500000) delay TEMPORARILY REMOVED for the same live A/B test
     * as the codec restriction and bluealsa-aplay below -- the stock
     * player's own bluealsa_profile script has zero delay between the
     * kills and respawning bluealsa/bt-agent (both backgrounded with a
     * bare `&`, script just ends), and was just confirmed working
     * flawlessly. */

    char * argv[9];
    int i = 0;
    argv[i++] = (char *) "/usr/bin/bluealsa";
    argv[i++] = (char *) "-p";
    argv[i++] = dac_mode_enabled ? (char *) "a2dp-sink" : (char *) "a2dp-source";
    if (volume_sync_enabled) argv[i++] = (char *) "--a2dp-volume";
    /* Codec restriction (-c SBC -c AAC, excluding LDAC) TEMPORARILY REMOVED
     * for a live A/B test: the stock player's own bluealsa_profile script
     * runs with no codec restriction at all and was just confirmed on a
     * real device to work flawlessly over a real phone connection, while
     * this app's DAC mode (with the restriction) has been consistently
     * failing to ever reach AVDTP Start on the same phone. This is the one
     * concrete command-line difference between the two, worth testing in
     * isolation before assuming it's unrelated to the LDAC/SoftVolume
     * distortion issue that motivated it in the first place -- this file
     * isn't under version control yet, so if this needs restoring later,
     * it was `-c` "SBC" `-c` "AAC" appended to argv right after
     * --a2dp-volume, only when dac_mode_enabled. */
    argv[i] = NULL;
    if (!spawn_bluealsa_and_verify(argv)) {
        pthread_mutex_unlock(&bt_daemon_respawn_mutex);
        return false;
    }

    if (dac_mode_enabled) {
        char * agent_argv[] = { (char *) "bt-agent", (char *) "-c", (char *) "NoInputNoOutput", NULL };
        subprocess_spawn_daemon(agent_argv);
    } else {
        char * agent_argv[] = { (char *) "bt-agent", (char *) "-c", (char *) "NoInputNoOutput",
                                 (char *) "-p", (char *) "/usr/data/pin.conf", NULL };
        subprocess_spawn_daemon(agent_argv);
    }

    if (dac_mode_enabled) {
        /* Real-device incident: plain `aplay -D bluealsa` (against the
         * generic pre-configured PCM plugin at /etc/alsa/conf.d/
         * 20-bluealsa.conf) needs that PCM to already resolve to an active
         * A2DP stream at the moment aplay opens it -- but DAC mode gets
         * toggled on before any phone has actually connected, so aplay just
         * exits immediately with nothing to play. bluealsa then has no
         * consumer at all once a phone DOES connect and start streaming --
         * confirmed on a real device to freeze it.
         *
         * bluealsa-aplay is the right tool after all (an earlier version of
         * this function tried it with a made-up -p flag it doesn't have,
         * confirmed via --help, and wrongly concluded the binary itself was
         * unusable): --help also shows A2DP is already its default profile
         * (no flag needed at all -- only --profile-sco opts into the
         * alternate SCO profile), and run with no MAC address it explicitly
         * defaults to "any/empty MAC ... allow[ing] connections from any
         * Bluetooth device", staying up and dynamically attaching whenever
         * one actually starts streaming rather than needing one to already
         * be attached at launch. That's exactly the long-running,
         * connection-order-independent behavior this needs.
         *
         * Real-user bug report #1: audio played through DAC mode noticeably
         * lags the video on the connected phone/PC (e.g. watching a video
         * with the R1 as the Bluetooth headphone/DAC). --pcm-buffer-time/
         * --pcm-period-time (confirmed present via `bluealsa-aplay --help`
         * on-device, v4.1.1; values in microseconds, standard ALSA tool
         * convention) tighten the local playback buffer downstream of
         * bluealsa's own Bluetooth receive/decode -- pure local ALSA
         * output buffering, cannot touch codec negotiation or AVDTP
         * connection setup. Deliberately NOT a codec restriction (e.g.
         * excluding LDAC, which has notably higher codec-level latency
         * than SBC/AAC) -- that was already tried for this exact bluealsa
         * invocation and reverted after it was confirmed, on a real phone,
         * to consistently fail to ever reach AVDTP Start (see the -c SBC
         * -c AAC removal comment above, in this same function). Standard
         * A2DP still has ~100-250ms of inherent encode/transmit/decode
         * latency baked in on the SOURCE phone/PC's side regardless (this
         * hardware/bluealsa build has no aptX-LL/aptX-Adaptive support
         * either, confirmed via `bluealsa --help`'s codec list) -- this
         * trims the one stage of the pipeline actually within this
         * device's control, not a guarantee of frame-perfect sync.
         *
         * Real-user bug report #2: DAC-mode audio plays cleanly for about
         * an hour, then chops/pitches, then the Bluetooth session drops
         * (reconnecting brings it back to normal until the next ~hour
         * mark). The 100ms/25ms values chosen for bug report #1 above
         * left only a 100ms local playback buffer to absorb clock drift
         * between the phone's Bluetooth stack and this SoC's audio
         * crystal -- at a typical 25-50ppm mismatch that buffer empties
         * or fills (ALSA XRUN) in roughly 30-70 minutes, matching the
         * reported chop/pitch-warble window. Each XRUN is audible: on
         * EPIPE, bluealsa-aplay's write loop calls snd_pcm_prepare() and
         * retries rather than exiting (confirmed against the upstream
         * BlueALSA v4.1.1 source, utils/aplay/aplay.c, the EPIPE case in the
         * snd_pcm_writei() retry loop) -- it does not itself drop the
         * daemon or the Bluetooth session; what turns sustained XRUN
         * retrying into an actual AVDTP/ACL drop is not confirmed here.
         *
         * --pcm-period-time must also be chosen to land on an exact
         * integer frame count at whatever rate the phone actually
         * negotiates, not just 44.1kHz: bluealsa-aplay's own docs
         * (doc/bluealsa-aplay.1.rst, "--pcm-period-time") warn that a
         * period time producing fractional frames causes rate-conversion
         * rounding error and worsens sync drift, worse with small
         * periods, and give period times that are multiples of 10000us
         * as the example fix since those land on exact frame counts at
         * both common Bluetooth rates (441 frames at 44.1kHz, 480 at
         * 48kHz per 10000us). An earlier version of this fix used
         * 46440us/185760us (2048 frames/4 periods, exact only at
         * 44.1kHz -- 2229.12 frames at 48kHz, a common negotiated A2DP
         * rate, so it would reintroduce exactly the drift this change is
         * trying to remove) and overstated its buffer-depth gain as
         * "roughly quadruples" when 185760/100000 is 1.86x. 50000us
         * period / 200000us buffer (4 periods) is exact at both 44.1kHz
         * (2205 frames) and 48kHz (2400 frames) and is 2x the old 100ms
         * buffer -- a smaller, verifiable claim, not a guess at how much
         * headroom it buys on real hardware. Do not go back toward
         * 25ms/100ms, the direction that broke, or reintroduce a period
         * time that is not an exact frame count at 48kHz. */
        char * bluealsa_aplay_argv[] = { (char *) "bluealsa-aplay",
                                          (char *) "--pcm-buffer-time=200000",
                                          (char *) "--pcm-period-time=50000",
                                          NULL };
        subprocess_spawn_daemon(bluealsa_aplay_argv);

        /* bt_volume_curve_start() is DISABLED for now -- see its own
         * comment (above bt_control_apply_output_settings()) for the
         * feature itself, but real-device testing found a problem with the
         * whole approach that a tuning tweak can't fix: writing a
         * corrected value back into bluealsa's own Volume property (the
         * same property --a2dp-volume syncs bidirectionally with the
         * phone) causes the PHONE to push its own uncorrected value right
         * back moments later, since AVRCP absolute volume is a two-way
         * sync and the phone doesn't know about our correction -- a real
         * tug-of-war, confirmed live, that settles into the volume getting
         * stuck oscillating in a narrow band regardless of what's actually
         * set on the phone. Needs a redesign that applies the correction
         * somewhere that isn't itself synced back to the phone (e.g. the
         * separate hardware ALSA mixer -- though a first attempt at that
         * found cset on it not sticking, needs more investigation) before
         * this can be safely re-enabled. Until then this intentionally
         * falls back to bluealsa's own plain linear SoftVolume -- not
         * ideal (see the taper comment), but predictable, unlike the
         * broken correction attempt. */

        /* Explicit `bluetoothctl discoverable/pairable on` calls
         * TEMPORARILY REMOVED for the same live A/B test as the codec
         * restriction, the delays, and bluealsa-aplay above -- the stock
         * player's own bluealsa_profile script never calls these at all
         * (confirmed by reading it directly), and was just confirmed
         * working flawlessly. Whatever makes this device discoverable/
         * pairable when the stock player enters DAC mode must happen
         * somewhere else (its own native bt-adapter/D-Bus calls baked into
         * the binary, not this script) -- if removing this breaks
         * pairing, that's itself useful information about where that
         * actually needs to happen. */
    } else {
        char * disc_argv[] = { (char *) "bluetoothctl", (char *) "discoverable", (char *) "off", NULL };
        subprocess_run(disc_argv, NULL, 0);
    }
    if (dac_mode_enabled) bt_dac_info_monitor_start();
    pthread_mutex_unlock(&bt_daemon_respawn_mutex);
    return true;
}

static void bt_control_reapply_last_output_settings(void) {
    /* Audit finding: these three globals used to be read here with no lock
     * at all -- see bt_control_apply_output_settings()'s own comment on
     * the write side of this same race. Snapshot under the lock, then call
     * outside it (bt_control_apply_output_settings() acquires this same
     * mutex itself -- calling it while still holding the lock here would
     * self-deadlock). */
    pthread_mutex_lock(&bt_daemon_respawn_mutex);
    bool ever_applied = output_settings_ever_applied;
    bool dac_mode = last_applied_dac_mode_enabled;
    bool volume_sync = last_applied_volume_sync_enabled;
    pthread_mutex_unlock(&bt_daemon_respawn_mutex);

    if (!ever_applied) return; /* this session never asked for a particular profile -- nothing to restore */
    bt_control_apply_output_settings(dac_mode, volume_sync);
}

bool bt_control_set_codec(const char * codec) {
    FILE * f = fopen("/usr/data/alsa.conf", "w");
    if (!f) return false;

    fprintf(f, "pcm.bt_alsa_sink {\n");
    fprintf(f, "    type plug\n");
    fprintf(f, "    slave {\n");
    fprintf(f, "        pcm {\n");
    fprintf(f, "            type bluealsa\n");
    fprintf(f, "            device XX:XX:XX:XX:XX:XX\n");
    fprintf(f, "            profile \"a2dp\"\n");
    if (strcmp(codec, "auto") != 0) {
        if (strncmp(codec, "ldac", 4) == 0) {
            fprintf(f, "            codec \"ldac\"\n");
            fprintf(f, "            ldac_eqmid \"%s\"\n", strcmp(codec, "ldac_hq") == 0 ? "LDAC_HQ" : "LDAC_SQ");
        } else {
            fprintf(f, "            codec \"%s\"\n", codec);
        }
    }
    fprintf(f, "        }\n");
    fprintf(f, "    }\n");
    fprintf(f, "}\n");

    fclose(f);
    return true;
}
