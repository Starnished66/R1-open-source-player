#include "bluetooth_control.h"
#include "debug_log.h"
#include "hiby_sys_server.h"
#include "subprocess.h"
#include "audio.h"

#include <dirent.h>
#include <errno.h>
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

/* Guards bt_control_init_chip(), bt_control_enable(), and bt_control_disable()
 * so chip firmware operations and power toggles are strictly serialized across threads. */
static pthread_mutex_t bt_chip_mutex = PTHREAD_MUTEX_INITIALIZER;
/* Shared by fallback bring-up and explicit profile changes. */
static pthread_mutex_t bt_daemon_respawn_mutex = PTHREAD_MUTEX_INITIALIZER;
static atomic_bool sbc_xq_enabled = false;
static atomic_bool modern_soft_volume_requested = false;
static pthread_mutex_t bt_soft_volume_mutex = PTHREAD_MUTEX_INITIALIZER;
static char bt_soft_volume_applied_path[256];
#define BT_SOURCE_VOLUME_MAX 127

/* BlueALSA 5 renamed both executables.  Select the pair as one unit: using a
 * v5 daemon with the legacy client (or vice versa) leaves the control path
 * talking to the wrong CLI/API.  access(2) is intentionally used here rather
 * than probing with a command on hot paths. */
typedef struct {
    const char * daemon;
    const char * daemon_name;
    const char * ctl;
    bool modern;
} bluealsa_backend_t;

static bluealsa_backend_t bluealsa_backend(void) {
    if (access("/usr/bin/bluealsad", X_OK) == 0 &&
            access("/usr/bin/bluealsactl", X_OK) == 0) {
        return (bluealsa_backend_t) {
            "/usr/bin/bluealsad", "bluealsad", "/usr/bin/bluealsactl", true
        };
    }
    return (bluealsa_backend_t) { "bluealsa", "bluealsa", "bluealsa-cli", false };
}

static const char * bluealsa_ctl_name(void) {
    return bluealsa_backend().ctl;
}

void bt_control_restore_codec_preference(const char * codec) {
    atomic_store(&sbc_xq_enabled, codec && strcmp(codec, "sbc_xq") == 0);
}

const char * bt_control_get_playback_pcm(void) {
    return atomic_load(&sbc_xq_enabled) ? "bluealsa:CODEC=SBC" : "bluealsa";
}

static bool bluealsa_is_audio_pcm_path(const char * path) {
    return path && (strstr(path, "/a2dpsnk/source") != NULL ||
                    strstr(path, "/a2dpsrc/sink") != NULL);
}

/* BlueALSA 4's missing --a2dp-volume meant local/software volume.  BlueALSA
 * 5 defaults to native volume and has no --a2dp-soft-volume daemon option;
 * apply the equivalent v5 control command when each PCM appears. */
static void bluealsa_apply_soft_volume(const char * path) {
    bluealsa_backend_t backend = bluealsa_backend();
    if (!backend.modern || !bluealsa_is_audio_pcm_path(path)) return;
    pthread_mutex_lock(&bt_soft_volume_mutex);
    if (strcmp(bt_soft_volume_applied_path, path) == 0) {
        pthread_mutex_unlock(&bt_soft_volume_mutex);
        return;
    }
    snprintf(bt_soft_volume_applied_path, sizeof(bt_soft_volume_applied_path), "%s", path);
    char * argv[] = { (char *) backend.ctl, (char *) "soft-volume",
                      (char *) path,
                      (char *) (atomic_load(&modern_soft_volume_requested) ? "on" : "off"), NULL };
    int exit_code = -1;
    bool ok = subprocess_run_checked(argv, NULL, 0, 5000, &exit_code) && exit_code == 0;
    if (!ok) bt_soft_volume_applied_path[0] = '\0';
    pthread_mutex_unlock(&bt_soft_volume_mutex);
}

static void bluealsa_clear_soft_volume_path(const char * path) {
    pthread_mutex_lock(&bt_soft_volume_mutex);
    if (!path || strcmp(bt_soft_volume_applied_path, path) == 0)
        bt_soft_volume_applied_path[0] = '\0';
    pthread_mutex_unlock(&bt_soft_volume_mutex);
}

static bool parse_monitor_volume(const char * text, int * out) {
    if (!text || !out) return false;
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        char * end;
        unsigned long packed = strtoul(text, &end, 16);
        if (end == text || *end != '\0' || packed > UINT32_MAX) return false;
        *out = (int) ((packed >> 8) & 0xFF); /* legacy left channel byte */
        return true;
    }
    char * end;
    long value = strtol(text, &end, 10);
    if (end == text || (strcmp(end, "[M]") != 0 && *end != '\0') ||
            value < 0 || value > BT_SOURCE_VOLUME_MAX) return false;
    *out = (int) value; /* v5 prints one value per channel */
    return true;
}

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
    const char * ctl = bluealsa_ctl_name();
    char list_out[4096];
    char * list_argv[] = { (char *) ctl, (char *) "list-pcms", NULL };
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
        bluealsa_clear_soft_volume_path(NULL);
        bt_dac_info_store(&info); /* PCM disappeared: clear stale data now. */
        return;
    }
    bluealsa_apply_soft_volume(path);
    info.available = true;
    char info_out[2048];
    char * info_argv[] = { (char *) ctl, (char *) "info", path, NULL };
    if (subprocess_run(info_argv, info_out, sizeof(info_out))) {
        copy_info_field(info.codec, sizeof(info.codec), info_out, "Selected codec:");
        copy_info_field(info.pcm_format, sizeof(info.pcm_format), info_out, "Format:");
        const char * sampling = strstr(info_out, "Sampling:");
        if (!sampling) sampling = strstr(info_out, "Rate:");
        const char * channels = strstr(info_out, "Channels:");
        const char * running = strstr(info_out, "Running:");
        if (sampling) {
            const char * value = strchr(sampling, ':');
            if (value) (void) sscanf(value + 1, "%u", &info.sample_rate);
        }
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
    char * argv[] = { (char *) bluealsa_ctl_name(), (char *) "monitor", NULL };
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

static int count_process_exact(const char * name) {
    char out[4096];
    char * argv[] = { (char *) "ps", NULL };
    if (!subprocess_run(argv, out, sizeof(out))) return 0;

    int count = 0;
    char * line_save = NULL;
    char * line = strtok_r(out, "\n", &line_save);
    while (line) {
        char * field_save = NULL;
        char * field = strtok_r(line, " \t", &field_save);
        while (field) {
            const char * base = strrchr(field, '/');
            if (base) base++;
            else base = field;
            if (strcmp(base, name) == 0) {
                count++;
                break;
            }
            field = strtok_r(NULL, " \t", &field_save);
        }
        line = strtok_r(NULL, "\n", &line_save);
    }
    return count;
}

/* Reconcile only the backend daemon itself.  The stock startup scripts can
 * leave a source daemon running without the requested encoder quality; a
 * sink daemon is deliberately left alone because SBC-XQ is source-only. */
static bool bluealsa_needs_source_restart(const bluealsa_backend_t * backend, bool xq) {
    DIR * proc = opendir("/proc");
    if (!proc) return false;
    bool restart = false;
    struct dirent * entry;
    while ((entry = readdir(proc))) {
        if (entry->d_name[0] < '0' || entry->d_name[0] > '9') continue;
        char path[320];
        snprintf(path, sizeof(path), "/proc/%s/cmdline", entry->d_name);
        FILE * f = fopen(path, "r");
        if (!f) continue;
        char args[2048] = {0};
        size_t len = fread(args, 1, sizeof(args) - 1, f);
        fclose(f);
        if (!len) continue;
        const char * name = strrchr(args, '/');
        name = name ? name + 1 : args;
        if (strcmp(name, backend->daemon_name) != 0) continue;
        bool sink = false, active_xq = false;
        for (size_t pos = strlen(args) + 1; pos < len; ) {
            size_t arg_len = strlen(args + pos);
            const char * arg = args + pos;
            if (strcmp(arg, "a2dp-sink") == 0 ||
                    strcmp(arg, "--profile=a2dp-sink") == 0) sink = true;
            if (strcmp(arg, "--profile") == 0 && pos + arg_len + 1 < len &&
                    strcmp(args + pos + arg_len + 1, "a2dp-sink") == 0) sink = true;
            if (strcmp(arg, "--sbc-quality=xq") == 0 ||
                    (strcmp(arg, "--sbc-quality") == 0 &&
                     pos + arg_len + 1 < len &&
                     strcmp(args + pos + arg_len + 1, "xq") == 0)) active_xq = true;
            if (!arg_len) break;
            pos += arg_len + 1;
        }
        if (!sink && active_xq != xq) restart = true;
    }
    closedir(proc);
    return restart;
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

    /* Launch dbus-daemon in the background with --fork. */
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

    /* Ensure a single system D-Bus daemon is running before restarting bluetoothd. */
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

/* Grace period after startup to avoid falsely triggering daemon recovery while
 * Bluetooth chip initialization or bring-up is still in progress. */
#define BT_BOOT_RACE_GRACE_MS 15000

static uint32_t bt_control_monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t) (ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* Guards bt_control_is_powered() state and timeout counters against concurrent
 * polling from the GUI timer and toggle background threads. */
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
    pthread_mutex_lock(&bt_daemon_respawn_mutex);
    bluealsa_backend_t backend = bluealsa_backend();
    bool xq = atomic_load(&sbc_xq_enabled);
    bool must_restart = bluealsa_needs_source_restart(&backend, xq);
    if (must_restart)
        subprocess_kill_all_matching(backend.daemon_name);
    /* Killing detached daemons is asynchronous; the old process may still be
     * visible to an immediate ps snapshot. Once a mismatch was established,
     * respawn unconditionally instead of allowing that stale snapshot to
     * suppress the replacement. */
    if (!must_restart && count_process_exact(backend.daemon_name) > 0) {
        pthread_mutex_unlock(&bt_daemon_respawn_mutex);
        return;
    }
    char * argv[6];
    int argc = 0;
    argv[argc++] = (char *) backend.daemon;
    argv[argc++] = (char *) "-p";
    argv[argc++] = (char *) "a2dp-source";
    if (!backend.modern) argv[argc++] = (char *) "--a2dp-volume";
    if (xq) argv[argc++] = (char *) "--sbc-quality=xq";
    argv[argc] = NULL;
    subprocess_spawn_daemon(argv);
    pthread_mutex_unlock(&bt_daemon_respawn_mutex);
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

/* BlueZ 5.87 removed the legacy `paired-devices` command and added the
 * `Paired` property filter to `devices`.  Probe the CLI's own command help,
 * rather than coupling this to the BlueZ daemon version or filesystem paths.
 * Keep the mutex held while probing so concurrent callers cannot spawn two
 * capability probes.  A failed, empty, truncated, or unrecognized probe
 * deliberately leaves the cache empty, allowing a later poll to retry. */
typedef enum {
    BT_BLUETOOTHCTL_DEVICES_UNKNOWN,
    BT_BLUETOOTHCTL_DEVICES_MODERN,
    BT_BLUETOOTHCTL_DEVICES_LEGACY
} bt_bluetoothctl_devices_capability_t;

static pthread_mutex_t bt_bluetoothctl_devices_mutex = PTHREAD_MUTEX_INITIALIZER;
static bt_bluetoothctl_devices_capability_t bt_bluetoothctl_devices_capability =
    BT_BLUETOOTHCTL_DEVICES_UNKNOWN;

/* `bluetoothctl --help` prints first-level commands as tab-indented lines,
 * followed by a tab-separated description.  It intentionally does not print
 * the command argument specification, so detect command names only. */
static bool bt_bluetoothctl_help_has_command(const char * help, const char * command) {
    size_t command_len = strlen(command);
    const char * line = help;
    while (line && *line) {
        const char * end = strchr(line, '\n');
        const char * token = line;
        if (*token == '\t' && token[1] != '\t') {
            const char * token_end = token + 1;
            token++;
            while (*token_end && *token_end != '\t' && *token_end != ' ' &&
                    *token_end != '\r' && *token_end != '\n') token_end++;
            if ((size_t) (token_end - token) == command_len &&
                    strncmp(token, command, command_len) == 0) return true;
        }
        if (!end) break;
        line = end + 1;
    }
    return false;
}

static bool bt_control_paired_devices_argv_checked(char * argv[4], int timeout_ms,
                                                   bt_control_cancel_callback_t cancel_cb,
                                                   void * cancel_ctx) {
    pthread_mutex_lock(&bt_bluetoothctl_devices_mutex);

    if (cancel_cb && cancel_cb(cancel_ctx)) {
        pthread_mutex_unlock(&bt_bluetoothctl_devices_mutex);
        return false;
    }

    if (bt_bluetoothctl_devices_capability == BT_BLUETOOTHCTL_DEVICES_UNKNOWN) {
        /* 5.87 prints over 9 KiB including submenu commands. A 4 KiB
         * capture truncates it and would leave this probe retrying forever. */
        char help[16384] = {0};
        char * help_argv[] = { (char *) "bluetoothctl", (char *) "--help", NULL };
        int exit_code = -1;
        bool ran = timeout_ms > 0 ? subprocess_run_checked(help_argv, help, sizeof(help),
                                                            timeout_ms, &exit_code) && exit_code == 0 :
                                   subprocess_run(help_argv, help, sizeof(help));
        if (ran) {
            size_t help_len = strnlen(help, sizeof(help));
            bool complete = help_len > 0 && help_len < sizeof(help) - 1 &&
                            help[help_len - 1] == '\n';
            /* Prefer the legacy command if both names ever appear in a
             * vendor-customized help listing. */
            if (complete && bt_bluetoothctl_help_has_command(help, "paired-devices"))
                bt_bluetoothctl_devices_capability = BT_BLUETOOTHCTL_DEVICES_LEGACY;
            else if (complete && bt_bluetoothctl_help_has_command(help, "devices"))
                bt_bluetoothctl_devices_capability = BT_BLUETOOTHCTL_DEVICES_MODERN;
        }
    }

    switch (bt_bluetoothctl_devices_capability) {
    case BT_BLUETOOTHCTL_DEVICES_MODERN:
        argv[0] = (char *) "bluetoothctl";
        argv[1] = (char *) "devices";
        argv[2] = (char *) "Paired";
        argv[3] = NULL;
        break;
    case BT_BLUETOOTHCTL_DEVICES_LEGACY:
        argv[0] = (char *) "bluetoothctl";
        argv[1] = (char *) "paired-devices";
        argv[2] = NULL;
        break;
    default:
        pthread_mutex_unlock(&bt_bluetoothctl_devices_mutex);
        return false;
    }

    pthread_mutex_unlock(&bt_bluetoothctl_devices_mutex);
    return !cancel_cb || !cancel_cb(cancel_ctx);
}

static bool bt_control_paired_devices_argv(char * argv[4]) {
    return bt_control_paired_devices_argv_checked(argv, 0, NULL, NULL);
}

bool bt_control_is_connected(void) {
    char devices_buf[4096];
    char * devices_argv[4];
    if (!bt_control_paired_devices_argv(devices_argv) ||
            !subprocess_run(devices_argv, devices_buf, sizeof(devices_buf))) return false;

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

/* See the header comment. bt_bluetoothctl_devices_capability is written
 * under bt_bluetoothctl_devices_mutex by bt_control_paired_devices_argv_checked()'s
 * one-time probe, so it must be read under that same mutex too -- copy it out
 * and unlock before doing anything else, both because the mutex is plain
 * (non-recursive) and the LEGACY/UNKNOWN fallback below (bt_control_is_connected())
 * re-enters that exact probe and would self-deadlock if this still held it.
 *
 * Returns 1 (connected), 0 (not connected), or -1 if this cycle couldn't
 * determine either way. -1 covers two cases: capability still UNKNOWN and
 * bt_control_is_connected()'s own query failed, or capability is MODERN but
 * this specific `devices Connected` call failed. The latter deliberately
 * does NOT fall back to the O(N) per-paired-device path -- that fallback is
 * exactly the fork storm this function exists to avoid, and a transient
 * bluetoothctl hiccup is most likely during the same radio power-on window
 * that storm is worst in. Callers should keep the last known state on -1,
 * not treat it as "nothing connected". */
int bt_control_any_paired_connected(void) {
    pthread_mutex_lock(&bt_bluetoothctl_devices_mutex);
    bt_bluetoothctl_devices_capability_t capability = bt_bluetoothctl_devices_capability;
    pthread_mutex_unlock(&bt_bluetoothctl_devices_mutex);

    if (capability == BT_BLUETOOTHCTL_DEVICES_MODERN) {
        char out[512];
        char * argv[] = { (char *) "bluetoothctl", (char *) "devices", (char *) "Connected", NULL };
        if (subprocess_run(argv, out, sizeof(out))) return strstr(out, "Device ") != NULL ? 1 : 0;
        return -1;
    }
    return bt_control_is_connected() ? 1 : 0;
}

/* Returns paired and connected state for all paired devices.
 * Returns device count, or -1 if the underlying bluetoothctl call fails. */
int bt_control_list_paired_states(bt_device_t * out, int max_count) {
    char devices_buf[4096];
    char * devices_argv[4];
    if (!bt_control_paired_devices_argv(devices_argv) ||
            !subprocess_run(devices_argv, devices_buf, sizeof(devices_buf))) return -1;

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

/* Runs discovery restricted to the classic BR/EDR bearer using `scan.transport bredr`.
 * Dual-mode devices paired via classic BR/EDR reliably support A2DP/AVRCP audio. */
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
    pthread_mutex_lock(&bt_chip_mutex);

    char * pair_argv[] = { (char *) "bluetoothctl", (char *) "pair", (char *) mac, NULL };
    subprocess_run(pair_argv, out, sizeof(out)); /* no-op if already paired -- not fatal either way */

    char * trust_argv[] = { (char *) "bluetoothctl", (char *) "trust", (char *) mac, NULL };
    subprocess_run(trust_argv, out, sizeof(out));

    char * connect_argv[] = { (char *) "bluetoothctl", (char *) "connect", (char *) mac, NULL };
    bool ok = subprocess_run(connect_argv, out, sizeof(out)) && strstr(out, "Failed") == NULL;
    pthread_mutex_unlock(&bt_chip_mutex);
    return ok;
}

bool bt_control_disconnect(const char * mac) {
    char out[512];
    pthread_mutex_lock(&bt_chip_mutex);
    char * argv[] = { (char *) "bluetoothctl", (char *) "disconnect", (char *) mac, NULL };
    bool ok = subprocess_run(argv, out, sizeof(out)) && strstr(out, "Failed") == NULL;
    pthread_mutex_unlock(&bt_chip_mutex);
    return ok;
}

bool bt_control_forget(const char * mac) {
    char out[512];
    pthread_mutex_lock(&bt_chip_mutex);
    char * argv[] = { (char *) "bluetoothctl", (char *) "remove", (char *) mac, NULL };
    bool ok = subprocess_run(argv, out, sizeof(out)) && strstr(out, "Failed") == NULL;
    pthread_mutex_unlock(&bt_chip_mutex);
    return ok;
}

#define BT_RECONNECT_QUERY_TIMEOUT_MS 1500
#define BT_RECONNECT_CONNECT_TIMEOUT_MS 6500

static bool bt_reconnect_cancelled(bt_control_cancel_callback_t cancel_cb, void * cancel_ctx) {
    return cancel_cb && cancel_cb(cancel_ctx);
}

static bool bt_reconnect_valid_mac(const char * mac) {
    if (!mac || strlen(mac) != 17) return false;
    for (int i = 0; i < 17; i++) {
        if (i % 3 == 2) {
            if (mac[i] != ':') return false;
        } else if (!((mac[i] >= '0' && mac[i] <= '9') ||
                     (mac[i] >= 'A' && mac[i] <= 'F') ||
                     (mac[i] >= 'a' && mac[i] <= 'f'))) return false;
    }
    return true;
}

static bool bt_reconnect_run(char * const argv[], char * output, size_t output_size,
                             int timeout_ms, bt_control_cancel_callback_t cancel_cb,
                             void * cancel_ctx, int * exit_code) {
    if (bt_reconnect_cancelled(cancel_cb, cancel_ctx)) return false;
    bool ran = subprocess_run_checked(argv, output, output_size, timeout_ms, exit_code);
    if (!ran || (exit_code && *exit_code != 0)) return false;
    /* An incomplete snapshot cannot establish that another output is absent. */
    if (output && output_size && strnlen(output, output_size) >= output_size - 1) return false;
    return !bt_reconnect_cancelled(cancel_cb, cancel_ctx);
}

static bool bt_reconnect_info(const char * mac, char * output, size_t output_size,
                              bt_control_cancel_callback_t cancel_cb, void * cancel_ctx) {
    char * argv[] = { (char *) "bluetoothctl", (char *) "info", (char *) mac, NULL };
    int exit_code = -1;
    return bt_reconnect_run(argv, output, output_size, BT_RECONNECT_QUERY_TIMEOUT_MS,
                            cancel_cb, cancel_ctx, &exit_code);
}

static bool bt_reconnect_audio_output_connected(bt_control_cancel_callback_t cancel_cb,
                                                void * cancel_ctx, bool * query_ok) {
    char output[4096] = {0};
    char * argv[] = { (char *) bluealsa_ctl_name(), (char *) "list-pcms", NULL };
    int exit_code = -1;
    bool ok = bt_reconnect_run(argv, output, sizeof(output), BT_RECONNECT_QUERY_TIMEOUT_MS,
                               cancel_cb, cancel_ctx, &exit_code);
    if (query_ok) *query_ok = ok;
    if (!ok) return false;
    return strstr(output, "/a2dpsrc/sink") != NULL;
}

/* Do not call bt_control_is_powered() from the reconnect critical section:
 * that public status path has its own recovery behavior and can re-enter
 * chip/daemon lifecycle code. This is the private, bounded read-only query
 * needed by the worker and deliberately never powers the adapter on. */
static bool bt_reconnect_adapter_powered(bt_control_cancel_callback_t cancel_cb,
                                         void * cancel_ctx) {
    char output[2048] = {0};
    char * argv[] = { (char *) "bluetoothctl", (char *) "show", NULL };
    int exit_code = -1;
    return bt_reconnect_run(argv, output, sizeof(output), BT_RECONNECT_QUERY_TIMEOUT_MS,
                            cancel_cb, cancel_ctx, &exit_code) &&
           strstr(output, "Powered: yes") != NULL;
}

static bool bt_reconnect_info_line_is(const char * info, const char * key, const char * value) {
    size_t key_len = strlen(key);
    const char * line = info;
    while (line && *line) {
        while (*line == ' ' || *line == '\t') line++;
        if (strncmp(line, key, key_len) == 0) {
            const char * p = line + key_len;
            while (*p == ' ' || *p == '\t') p++;
            size_t value_len = strcspn(p, "\r\n");
            while (value_len && (p[value_len - 1] == ' ' || p[value_len - 1] == '\t')) value_len--;
            if (strlen(value) == value_len && strncmp(p, value, value_len) == 0) return true;
        }
        line = strchr(line, '\n');
        if (line) line++;
    }
    return false;
}

static bool bt_reconnect_info_has_uuid(const char * info, const char * uuid) {
    size_t key_len = strlen("UUID:");
    const char * line = info;
    while (line && *line) {
        while (*line == ' ' || *line == '\t') line++;
        if (strncmp(line, "UUID:", key_len) == 0) {
            const char * end = strchr(line, '\n');
            size_t line_len = end ? (size_t)(end - line) : strlen(line);
            const char * match = strstr(line + key_len, uuid);
            if (match && (size_t)(match - line) + strlen(uuid) <= line_len) return true;
        }
        line = strchr(line, '\n');
        if (line) line++;
    }
    return false;
}

static bool bt_reconnect_info_eligible(const char * info, bool * connected) {
    if (connected) *connected = false;
    if (!info || !bt_reconnect_info_line_is(info, "Paired:", "yes") ||
            !bt_reconnect_info_line_is(info, "Trusted:", "yes") ||
            bt_reconnect_info_line_is(info, "Blocked:", "yes") ||
            !bt_reconnect_info_has_uuid(info, "0000110b-0000-1000-8000-00805f9b34fb")) return false;
    if (connected) *connected = bt_reconnect_info_line_is(info, "Connected:", "yes");
    return true;
}

static bool bt_reconnect_disconnect_locked(const char * mac) {
    char output[512] = {0};
    char * argv[] = { (char *) "bluetoothctl", (char *) "disconnect", (char *) mac, NULL };
    int exit_code = -1;
    return subprocess_run_checked(argv, output, sizeof(output),
                                  BT_RECONNECT_QUERY_TIMEOUT_MS, &exit_code) &&
           exit_code == 0 && strstr(output, "Failed") == NULL;
}

bool bt_control_reconnect_paired(const char * preferred_mac,
                                 bt_control_cancel_callback_t cancel_cb,
                                 void * cancel_ctx) {
    if (preferred_mac && (!bt_reconnect_valid_mac(preferred_mac) ||
                          bt_reconnect_cancelled(cancel_cb, cancel_ctx))) return false;
    if (!bt_reconnect_adapter_powered(cancel_cb, cancel_ctx)) return false;
    bool pcm_query_ok = false;
    if (bt_reconnect_audio_output_connected(cancel_cb, cancel_ctx, &pcm_query_ok)) return true;
    if (!pcm_query_ok) return false;
    if (bt_reconnect_cancelled(cancel_cb, cancel_ctx)) return false;

    char target[18] = {0};
    char info[4096] = {0};
    bool was_connected = false;
    if (preferred_mac) {
        snprintf(target, sizeof(target), "%s", preferred_mac);
        if (!bt_reconnect_info(target, info, sizeof(info), cancel_cb, cancel_ctx) ||
                !bt_reconnect_info_eligible(info, &was_connected)) return false;
    } else {
        char devices[4096] = {0};
        char * devices_argv[4];
        if (!bt_control_paired_devices_argv_checked(devices_argv, BT_RECONNECT_QUERY_TIMEOUT_MS,
                                                     cancel_cb, cancel_ctx)) return false;
        int exit_code = -1;
        if (!bt_reconnect_run(devices_argv, devices, sizeof(devices),
                              BT_RECONNECT_QUERY_TIMEOUT_MS, cancel_cb, cancel_ctx, &exit_code)) return false;
        size_t devices_len = strnlen(devices, sizeof(devices));
        if (devices_len == 0 || devices_len >= sizeof(devices) - 1 || devices[devices_len - 1] != '\n') return false;
        unsigned int eligible = 0;
        char * save = NULL;
        for (char * line = strtok_r(devices, "\r\n", &save); line;
             line = strtok_r(NULL, "\r\n", &save)) {
            if (bt_reconnect_cancelled(cancel_cb, cancel_ctx)) return false;
            if (strncmp(line, "Device ", 7) != 0) continue;
            const char * mac = line + 7;
            const char * name = strchr(mac, ' ');
            char candidate[18];
            if (!name || (size_t)(name - mac) != 17) continue;
            memcpy(candidate, mac, 17);
            candidate[17] = '\0';
            if (!bt_reconnect_valid_mac(candidate)) continue;
            char candidate_info[4096] = {0};
            bool candidate_connected = false;
            if (!bt_reconnect_info(candidate, candidate_info, sizeof(candidate_info),
                                   cancel_cb, cancel_ctx)) return false;
            if (bt_reconnect_info_eligible(candidate_info, &candidate_connected)) {
                eligible++;
                snprintf(target, sizeof(target), "%s", candidate);
                snprintf(info, sizeof(info), "%s", candidate_info);
                was_connected = candidate_connected;
                if (eligible > 1) return false;
            }
        }
        if (eligible != 1) return false;
    }
    if (was_connected) return true;
    if (bt_reconnect_cancelled(cancel_cb, cancel_ctx)) return false;

    pthread_mutex_lock(&bt_chip_mutex);
    bool result = false;
    bool connected_after = false;
    if (!bt_reconnect_cancelled(cancel_cb, cancel_ctx) &&
            bt_reconnect_adapter_powered(cancel_cb, cancel_ctx) &&
            bt_reconnect_audio_output_connected(cancel_cb, cancel_ctx, &pcm_query_ok) == false &&
            pcm_query_ok) {
        /* Refresh under the chip lock so a connection made by another path
         * before lock acquisition is treated as preexisting. */
        char locked_info[4096] = {0};
        bool locked_connected = false;
        if (!bt_reconnect_info(target, locked_info, sizeof(locked_info), NULL, NULL) ||
                !bt_reconnect_info_eligible(locked_info, &locked_connected)) {
            pthread_mutex_unlock(&bt_chip_mutex);
            return false;
        }
        if (locked_connected) {
            pthread_mutex_unlock(&bt_chip_mutex);
            return true;
        }
        if (bt_reconnect_cancelled(cancel_cb, cancel_ctx)) {
            pthread_mutex_unlock(&bt_chip_mutex);
            return false;
        }
        char * connect_argv[] = { (char *) "bluetoothctl", (char *) "--timeout", (char *) "5",
                                  (char *) "connect", target, NULL };
        char connect_output[1024] = {0};
        int exit_code = -1;
        bool connect_ran = subprocess_run_checked(connect_argv, connect_output, sizeof(connect_output),
                                                  BT_RECONNECT_CONNECT_TIMEOUT_MS, &exit_code);
        bool connect_ok = connect_ran && exit_code == 0 && strstr(connect_output, "Failed") == NULL;
        /* Verify and, when cancellation raced the command, clean up using an
         * uncancelled bounded read. The callback is intentionally not used
         * for this cleanup observation. */
        if (bt_reconnect_info(target, info, sizeof(info), NULL, NULL))
            connected_after = bt_reconnect_info_eligible(info, &was_connected) && was_connected;
        if (bt_reconnect_cancelled(cancel_cb, cancel_ctx)) {
            (void) bt_reconnect_disconnect_locked(target);
        } else {
            result = connect_ok && connected_after;
        }
    }
    pthread_mutex_unlock(&bt_chip_mutex);
    return result;
}

/* Synchronizes volume between this player and connected a2dp-source accessories.
 * Maps AVRCP 0-127 linearly to the player's 0-100% volume. */
static bool find_source_pcm_path(char * out, size_t out_size) {
    char list_out[4096];
    char * argv[] = { (char *) bluealsa_ctl_name(), (char *) "list-pcms", NULL };
    if (!subprocess_run(argv, list_out, sizeof(list_out))) {
        /* Log failure if bluealsa-cli list-pcms fails or times out. */
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
            /* A source PCM can predate this monitor subscription, so apply
             * the v5 SoftVolume preference during discovery as well as on
             * PCMAdded. */
            bluealsa_apply_soft_volume(out);
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
    char * argv[] = { (char *) bluealsa_ctl_name(), (char *) "info", path, NULL };
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
static atomic_bool bt_source_vol_sync_active = false;
static bool bt_source_vol_sync_joinable;
static atomic_int bt_source_vol_pending_percent = -1;

/* Set by whichever direction writes/observes a value most recently, so the
 * other direction recognizes it as already in sync instead of re-writing
 * it right back, just shared between two directions here instead of one
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
    char * vol_argv[] = { (char *) bluealsa_ctl_name(), (char *) "volume", path, raw_str, raw_str, NULL };
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
    if (!atomic_load(&bt_source_vol_sync_active)) return NULL;

    char * argv[] = { (char *) bluealsa_ctl_name(), (char *) "monitor", (char *) "-p", NULL };
    pid_t pid;
    int read_fd;
    if (!subprocess_popen(argv, &pid, &read_fd)) {
        atomic_store(&bt_source_vol_sync_active, false);
        return NULL;
    }

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

        /* Bounded, not a blocking fgets() -- this loop also needs to notice
         * this app's OWN volume changing (UI slider, hardware buttons), which
         * has no fd to select() on, so it polls this monitor's pipe with a
         * short timeout instead of blocking on it indefinitely. 500ms is
         * imprecise for a "keep two volume controls in sync" feature (not
         * a latency-sensitive control path), matched on both sides of this
         * loop -- see the push check below. */
        struct pollfd pfd = { .fd = read_fd, .events = POLLIN };
        int pr = poll(&pfd, 1, 500);
        if (!atomic_load(&bt_source_vol_sync_active)) break;
        if (pr < 0 && errno != EINTR) break;
        if (pr > 0 && (pfd.revents & (POLLERR | POLLNVAL))) break;
        if (pr > 0 && (pfd.revents & POLLHUP) && !(pfd.revents & POLLIN)) break;

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
                int raw;
                char volume_text[64];
                if (sscanf(line_start, "PropertyChanged %255s Volume %63s", path, volume_text) == 2 &&
                    strstr(path, "/a2dpsrc/sink") != NULL &&
                    parse_monitor_volume(volume_text, &raw)) {
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
    subprocess_terminate(pid); /* worker owns and reaps its own child */
    atomic_store(&bt_source_vol_sync_active, false);
    return NULL;
}

bool bt_control_source_volume_sync_is_running(void) {
    return atomic_load(&bt_source_vol_sync_active);
}

void bt_control_source_volume_sync_start(void) {
    if (bt_source_vol_sync_joinable) {
        if (atomic_load(&bt_source_vol_sync_active)) return;
        pthread_join(bt_source_vol_sync_thread, NULL);
        bt_source_vol_sync_joinable = false;
    }
    bt_source_vol_sync_active = true;
    bt_source_vol_last_synced_raw = -1;
    atomic_store_explicit(&bt_source_vol_pending_percent, -1, memory_order_relaxed);
    if (pthread_create(&bt_source_vol_sync_thread, NULL, bt_source_vol_sync_thread_func, NULL) == 0)
        bt_source_vol_sync_joinable = true;
    else bt_source_vol_sync_active = false;
}

void bt_control_source_volume_sync_stop(void) {
    if (!bt_source_vol_sync_joinable) return;
    /* Cancellation is observed after any in-flight command and the bounded
     * pipe poll. This join belongs on the lifecycle worker, never LVGL. */
    bt_source_vol_sync_active = false;
    pthread_join(bt_source_vol_sync_thread, NULL);
    bt_source_vol_sync_joinable = false;
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
static atomic_bool bt_output_disconnect_active = false;
static bool bt_output_disconnect_joinable;
static atomic_bool bt_output_disconnect_flag = false;
static pthread_mutex_t bt_output_disconnect_state_mutex = PTHREAD_MUTEX_INITIALIZER;
static char bt_output_disconnect_confirmed_path[256];

#define BT_OUTPUT_DISCONNECT_MAX_ADDED_PATHS 16

static void bt_output_disconnect_process_line(const char * line, char * pending_path,
                                              uint32_t * pending_deadline,
                                              char added_paths[][256], int * added_count) {
    char path[256];
    if (sscanf(line, "PCMRemoved %255s", path) == 1) {
        if (strstr(path, "/a2dpsrc/sink") != NULL) {
            snprintf(pending_path, 256, "%s", path);
            *pending_deadline = bt_control_monotonic_ms() + BT_OUTPUT_RECONFIGURE_GRACE_MS;
            bluealsa_clear_soft_volume_path(path);
            DBG_LOG("bt_control: output_disconnect_watch: PCMRemoved %s -- pending disconnect\n", path);
        }
        return;
    }
    if (strncmp(line, "PCMAdded ", 9) != 0 || sscanf(line, "PCMAdded %255s", path) != 1) return;
    if (strcmp(pending_path, path) == 0) {
        pending_path[0] = '\0';
        *pending_deadline = 0;
        DBG_LOG("bt_control: output_disconnect_watch: PCMAdded %s -- cancelled pending disconnect\n", path);
    }
    /* A replacement can arrive after the deadline but before the UI consumes
     * the event. Cancel only the event for this exact PCM, under the same
     * short lock used by the UI consumer. Never hold it during control I/O. */
    pthread_mutex_lock(&bt_output_disconnect_state_mutex);
    if (strcmp(bt_output_disconnect_confirmed_path, path) == 0) {
        atomic_store(&bt_output_disconnect_flag, false);
        bt_output_disconnect_confirmed_path[0] = '\0';
    }
    pthread_mutex_unlock(&bt_output_disconnect_state_mutex);
    if (*added_count < BT_OUTPUT_DISCONNECT_MAX_ADDED_PATHS)
        snprintf(added_paths[(*added_count)++], 256, "%s", path);
    DBG_LOG("bt_control: output_disconnect_watch: %s\n", line);
}

static bool bt_output_disconnect_read_batch(int fd, char * buf, size_t * used,
                                            char * pending_path, uint32_t * pending_deadline,
                                            char added_paths[][256], int * added_count) {
    if (*used == 511) *used = 0;
    ssize_t n = read(fd, buf + *used, 511 - *used);
    if (n <= 0) return false;
    *used += (size_t)n;
    buf[*used] = '\0';
    char * line = buf;
    char * end;
    while ((end = strchr(line, '\n')) != NULL) {
        *end = '\0';
        bt_output_disconnect_process_line(line, pending_path, pending_deadline,
                                          added_paths, added_count);
        line = end + 1;
    }
    size_t remaining = *used - (size_t)(line - buf);
    memmove(buf, line, remaining);
    *used = remaining;
    return true;
}

static void bt_output_disconnect_publish_if_due(char * pending_path, uint32_t * pending_deadline) {
    if (!pending_path[0] || (int32_t)(bt_control_monotonic_ms() - *pending_deadline) < 0) return;
    pthread_mutex_lock(&bt_output_disconnect_state_mutex);
    if (atomic_load(&bt_output_disconnect_active) && !atomic_load(&bt_output_disconnect_flag)) {
        snprintf(bt_output_disconnect_confirmed_path, sizeof(bt_output_disconnect_confirmed_path), "%s", pending_path);
        atomic_store(&bt_output_disconnect_flag, true);
    }
    pthread_mutex_unlock(&bt_output_disconnect_state_mutex);
    DBG_LOG("bt_control: output_disconnect_watch: PCMRemoved %s -- flagging disconnect\n", pending_path);
    pending_path[0] = '\0';
    *pending_deadline = 0;
}

static void * bt_output_disconnect_thread_func(void * arg) {
    (void) arg;

    char * argv[] = { (char *) bluealsa_ctl_name(), (char *) "monitor", NULL };
    pid_t pid;
    int read_fd;
    if (!subprocess_popen(argv, &pid, &read_fd)) {
        DBG_LOG("bt_control: output_disconnect_watch: failed to spawn bluealsa-cli monitor\n");
        atomic_store(&bt_output_disconnect_active, false);
        return NULL;
    }
    DBG_LOG("bt_control: output_disconnect_watch: monitor started (pid %d)\n", (int) pid);

    char buf[512];
    size_t used = 0;
    char pending_path[256] = "";
    uint32_t pending_deadline = 0;
    char added_paths[BT_OUTPUT_DISCONNECT_MAX_ADDED_PATHS][256];
    int added_count = 0;
    while (atomic_load(&bt_output_disconnect_active)) {
        uint32_t now = bt_control_monotonic_ms();
        int timeout_ms = 100;
        if (pending_path[0]) {
            int32_t remaining = (int32_t)(pending_deadline - now);
            timeout_ms = remaining <= 0 ? 0 : (remaining < timeout_ms ? remaining : timeout_ms);
        }
        struct pollfd pfd = { .fd = read_fd, .events = POLLIN };
        int pr = poll(&pfd, 1, timeout_ms);
        if (!atomic_load(&bt_output_disconnect_active)) break;
        if (pr < 0) { if (errno == EINTR) continue; break; }
        bool terminal = false;
        if (pr > 0 && (pfd.revents & POLLIN))
            terminal = !bt_output_disconnect_read_batch(read_fd, buf, &used, pending_path,
                                                         &pending_deadline, added_paths, &added_count);
        if (!terminal) {
            struct pollfd drain = { .fd = read_fd, .events = POLLIN };
            while (atomic_load(&bt_output_disconnect_active) && poll(&drain, 1, 0) > 0) {
                if (!(drain.revents & POLLIN)) {
                    terminal = (drain.revents & (POLLERR | POLLNVAL | POLLHUP)) != 0;
                    break;
                }
                if (!bt_output_disconnect_read_batch(read_fd, buf, &used, pending_path,
                                                     &pending_deadline, added_paths, &added_count)) {
                    terminal = true;
                    break;
                }
                drain.revents = 0;
            }
        }
        if (!atomic_load(&bt_output_disconnect_active)) break;
        if (terminal || (pr > 0 && (pfd.revents & (POLLERR | POLLNVAL | POLLHUP)))) {
            /* A failed monitor may have missed the replacement. Leave an
             * unconfirmed removal to periodic polling rather than inventing
             * a disconnect after its event stream has ended. */
            break;
        }
        bt_output_disconnect_publish_if_due(pending_path, &pending_deadline);
        /* Do not let volume subprocesses delay reading a replacement while
         * a removal is pending. Keep the bounded queue until it settles. */
        if (!pending_path[0]) {
            for (int i = 0; i < added_count && atomic_load(&bt_output_disconnect_active); i++)
                bluealsa_apply_soft_volume(added_paths[i]);
            added_count = 0;
        }
    }

    DBG_LOG("bt_control: output_disconnect_watch: monitor exited (pid %d)\n", (int) pid);
    close(read_fd);
    subprocess_terminate(pid);
    atomic_store(&bt_output_disconnect_active, false);
    return NULL;
}

bool bt_control_output_disconnect_watch_is_running(void) {
    return atomic_load(&bt_output_disconnect_active);
}

void bt_control_output_disconnect_watch_start(void) {
    if (bt_output_disconnect_joinable) {
        if (atomic_load(&bt_output_disconnect_active)) return;
        pthread_join(bt_output_disconnect_thread, NULL);
        bt_output_disconnect_joinable = false;
    }
    pthread_mutex_lock(&bt_output_disconnect_state_mutex);
    bt_output_disconnect_active = true;
    bt_output_disconnect_flag = false;
    bt_output_disconnect_confirmed_path[0] = '\0';
    pthread_mutex_unlock(&bt_output_disconnect_state_mutex);
    if (pthread_create(&bt_output_disconnect_thread, NULL, bt_output_disconnect_thread_func, NULL) == 0)
        bt_output_disconnect_joinable = true;
    else bt_output_disconnect_active = false;
}

void bt_control_output_disconnect_watch_stop(void) {
    bt_output_disconnect_active = false;
    if (bt_output_disconnect_joinable) {
        pthread_join(bt_output_disconnect_thread, NULL);
        bt_output_disconnect_joinable = false;
    }
    pthread_mutex_lock(&bt_output_disconnect_state_mutex);
    atomic_store(&bt_output_disconnect_flag, false);
    bt_output_disconnect_confirmed_path[0] = '\0';
    pthread_mutex_unlock(&bt_output_disconnect_state_mutex);
}

bool bt_control_output_disconnect_consume(void) {
    pthread_mutex_lock(&bt_output_disconnect_state_mutex);
    bool disconnected = atomic_exchange(&bt_output_disconnect_flag, false);
    bt_output_disconnect_confirmed_path[0] = '\0';
    pthread_mutex_unlock(&bt_output_disconnect_state_mutex);
    return disconnected;
}

/* Recorded so bt_control_reapply_last_output_settings() (the wedge
 * recovery in bt_control_is_powered(), above) can restore this after a
 * bluetoothd restart -- a fresh bluetoothd/bt_resume-equivalent bring-up
 * always comes back up in the plain a2dp-source default, not whatever
 * profile was actually requested last. */
static bool last_applied_dac_mode_enabled = false;
static bool last_applied_volume_sync_enabled = false;
static bool output_settings_ever_applied = false;

#define BLUEALSA_STARTUP_LOG_PATH "/usr/data/bluealsa_startup.log"

/* Spawns bluealsa daemon, logging startup output if TEST_BUILD_TAG is defined. */
static bool spawn_bluealsa_and_verify(char * const argv[]) {
#if defined(TEST_BUILD_TAG)
    return subprocess_spawn_daemon_logged(argv, BLUEALSA_STARTUP_LOG_PATH);
#else
    return subprocess_spawn_daemon_logged(argv, NULL);
#endif
}

bool bt_control_apply_output_settings(bool dac_mode_enabled, bool volume_sync_enabled) {
    /* Serializes bluealsa/bt-agent respawns and updates last-applied settings
     * to protect against concurrent caller races. */
    pthread_mutex_lock(&bt_chip_mutex);
    pthread_mutex_lock(&bt_daemon_respawn_mutex);
    last_applied_dac_mode_enabled = dac_mode_enabled;
    last_applied_volume_sync_enabled = volume_sync_enabled;
    output_settings_ever_applied = true;
    atomic_store(&modern_soft_volume_requested, !volume_sync_enabled && bluealsa_backend().modern);
    bluealsa_clear_soft_volume_path(NULL);

    /* Single profile mode: runs a2dp-sink or a2dp-source. */
    /* Clean up existing instances before respawning daemons. */
    bt_dac_info_monitor_stop();

    bluealsa_backend_t backend = bluealsa_backend();
    subprocess_kill_all_matching(backend.daemon_name);
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
    argv[i++] = (char *) backend.daemon;
    argv[i++] = (char *) "-p";
    argv[i++] = dac_mode_enabled ? (char *) "a2dp-sink" : (char *) "a2dp-source";
    if (volume_sync_enabled && !backend.modern) argv[i++] = (char *) "--a2dp-volume";
    if (!dac_mode_enabled && atomic_load(&sbc_xq_enabled))
        argv[i++] = (char *) "--sbc-quality=xq";
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
        pthread_mutex_unlock(&bt_chip_mutex);
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
        /* Runs bluealsa-aplay with 200ms buffer (4 periods of 50ms) to ensure
         * exact frame boundaries at both 44.1kHz and 48kHz and reduce latency
         * while buffering against clock drift. */
        char * bluealsa_aplay_argv[] = { (char *) "bluealsa-aplay",
                                          (char *) "--pcm-buffer-time=200000",
                                          (char *) "--pcm-period-time=50000",
                                          NULL };
        subprocess_spawn_daemon(bluealsa_aplay_argv);
    } else {
        char * disc_argv[] = { (char *) "bluetoothctl", (char *) "discoverable", (char *) "off", NULL };
        subprocess_run(disc_argv, NULL, 0);
    }
    if (dac_mode_enabled) bt_dac_info_monitor_start();
    pthread_mutex_unlock(&bt_daemon_respawn_mutex);
    pthread_mutex_unlock(&bt_chip_mutex);
    return true;
}

static void bt_control_reapply_last_output_settings(void) {
    /* Read settings under lock to avoid racing concurrent updates,
     * then call bt_control_apply_output_settings outside the lock. */
    pthread_mutex_lock(&bt_daemon_respawn_mutex);
    bool ever_applied = output_settings_ever_applied;
    bool dac_mode = last_applied_dac_mode_enabled;
    bool volume_sync = last_applied_volume_sync_enabled;
    pthread_mutex_unlock(&bt_daemon_respawn_mutex);

    if (!ever_applied) {
        ensure_bluealsa_running();
        return;
    }
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
            fprintf(f, "            codec \"%s\"\n", strcmp(codec, "sbc_xq") == 0 ? "sbc" : codec);
        }
    }
    fprintf(f, "        }\n");
    fprintf(f, "    }\n");
    fprintf(f, "}\n");

    bool ok = !ferror(f);
    if (fclose(f) != 0) ok = false;
    if (ok) bt_control_restore_codec_preference(codec);
    return ok;
}
