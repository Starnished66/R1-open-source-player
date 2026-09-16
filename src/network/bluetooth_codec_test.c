/* Host regression tests against the real implementation. Unused device code
 * is discarded by gc-sections; fopen redirects only the codec config path. */
#define _GNU_SOURCE
#include "bluetooth_control.c"
#include <assert.h>
#include <errno.h>

static char config_path[] = "/tmp/bluetooth-codec-test-XXXXXX";
static unsigned config_opens;
static enum { IO_OK, IO_OPEN_FAIL, IO_WRITE_FAIL, IO_CLOSE_FAIL } io_mode;
static const char * process_cmdline;
static size_t process_cmdline_size;
static bool process_present, check_lock;
static bool retain_process_after_kill;
static bool modern_daemon_installed, modern_ctl_installed;
static unsigned proc_index, kill_calls, spawn_calls, soft_volume_calls;
static unsigned soft_volume_checked_calls;
static bool soft_volume_fail;
static bool source_pcm_present;
static enum { BLUEZ_CLI_MODERN, BLUEZ_CLI_LEGACY, BLUEZ_CLI_EMPTY_HELP,
              BLUEZ_CLI_TRUNCATED_HELP } bluez_cli_mode;
static unsigned bluez_help_calls, bluez_help_failures, bluez_paired_calls;
static char soft_volume_state[8];
static char soft_volume_path[256];
static char discovered_source_path[256];
static char spawned[10][128];
static size_t spawned_argc;
static struct dirent proc_entry;

int access(const char * path, int mode) {
    assert(mode == X_OK);
    if (strcmp(path, "/usr/bin/bluealsad") == 0 ||
            strcmp(path, "/usr/bin/bluealsactl") == 0)
        return (strcmp(path, "/usr/bin/bluealsad") == 0 ? modern_daemon_installed : modern_ctl_installed) ? 0 : -1;
    assert(!"unexpected access path");
    return -1;
}

static void expect_locked(void) {
    if (check_lock) assert(pthread_mutex_trylock(&bt_daemon_respawn_mutex) == EBUSY);
}

DIR * __wrap_opendir(const char * path) {
    assert(strcmp(path, "/proc") == 0);
    expect_locked();
    proc_index = 0;
    return (DIR *)&proc_entry;
}

struct dirent * __wrap_readdir(DIR * dir) {
    assert(dir == (DIR *)&proc_entry);
    if (proc_index++ || !process_present) return NULL;
    strcpy(proc_entry.d_name, "12345");
    return &proc_entry;
}

int __wrap_closedir(DIR * dir) {
    assert(dir == (DIR *)&proc_entry);
    return 0;
}

static void capture_argv(char * const argv[]) {
    expect_locked();
    spawn_calls++;
    spawned_argc = 0;
    while (argv[spawned_argc]) {
        assert(spawned_argc < 10);
        assert(strlen(argv[spawned_argc]) < sizeof(spawned[0]));
        strcpy(spawned[spawned_argc], argv[spawned_argc]);
        spawned_argc++;
    }
}

void subprocess_kill_all_matching(const char * needle) {
    expect_locked();
    kill_calls++;
    if (strcmp(needle, modern_daemon_installed && modern_ctl_installed ? "bluealsad" : "bluealsa") == 0 &&
            !retain_process_after_kill)
        process_present = false;
}

bool subprocess_spawn_daemon(char * const argv[]) {
    capture_argv(argv);
    return true;
}

bool subprocess_spawn_daemon_logged(char * const argv[], const char * log_path) {
    (void)log_path;
    capture_argv(argv);
    /* Stop apply_output_settings after constructing the actual daemon argv. */
    return false;
}

bool subprocess_run_checked(char * const argv[], char * out, size_t size,
                            int timeout_ms, int * out_exit_code) {
    expect_locked();
    assert(strcmp(argv[0], "/usr/bin/bluealsactl") == 0);
    assert(strcmp(argv[1], "soft-volume") == 0);
    assert(out == NULL && size == 0 && timeout_ms == 5000);
    assert(out_exit_code);
    soft_volume_checked_calls++;
    snprintf(soft_volume_path, sizeof(soft_volume_path), "%s", argv[2]);
    snprintf(soft_volume_state, sizeof(soft_volume_state), "%s", argv[3]);
    *out_exit_code = soft_volume_fail ? 1 : 0;
    return true; /* Exercise callers that must inspect the exit status. */
}

bool subprocess_run(char * const argv[], char * out, size_t size) {
    expect_locked();
    if (strcmp(argv[0], "bluetoothctl") == 0) {
        assert(out && size);
        if (strcmp(argv[1], "--help") == 0) {
            assert(argv[2] == NULL);
            bluez_help_calls++;
            if (bluez_help_failures) {
                bluez_help_failures--;
                return false;
            }
            if (bluez_cli_mode == BLUEZ_CLI_EMPTY_HELP) {
                out[0] = '\0';
            } else if (bluez_cli_mode == BLUEZ_CLI_TRUNCATED_HELP) {
                snprintf(out, size, "Commands:\n\tdevices\t\tList available");
            } else {
                snprintf(out, size, "%s", bluez_cli_mode == BLUEZ_CLI_MODERN ?
                         "Commands:\n"
                         "\tlist\t\tList available controllers\n"
                         "\tshow\t\tController information\n"
                         "\tselect\t\tSelect default controller\n"
                         "\tdevices\t\tList available devices, with an optional property as the filter\n" :
                         "Commands:\n"
                         "\tlist\t\tList available controllers\n"
                         "\tshow\t\tController information\n"
                         "\tpaired-devices\tList paired devices\n");
                if (bluez_cli_mode == BLUEZ_CLI_MODERN) {
                    /* The real 5.87 --help is 9151 bytes; keep this test
                     * from accidentally validating an undersized capture. */
                    assert(size > 9151);
                    size_t len = strlen(out);
                    memset(out + len, ' ', 9150 - len);
                    out[9150] = '\n';
                    out[9151] = '\0';
                }
            }
            return true;
        }
        if (strcmp(argv[1], "devices") == 0) {
            assert(bluez_cli_mode == BLUEZ_CLI_MODERN);
            assert(strcmp(argv[2], "Paired") == 0 && argv[3] == NULL);
        } else if (strcmp(argv[1], "paired-devices") == 0) {
            assert(bluez_cli_mode == BLUEZ_CLI_LEGACY);
            assert(argv[2] == NULL);
        } else {
            assert(strcmp(argv[1], "info") == 0);
            assert(argv[2] && argv[3] == NULL);
            snprintf(out, size, "Device %s\n\tPaired: yes\n\tConnected: %s\n",
                     argv[2], strstr(argv[2], "02") ? "yes" : "no");
            return true;
        }
        bluez_paired_calls++;
        snprintf(out, size,
                 "Device AA:BB:CC:DD:EE:01 Headphones\n"
                 "Device AA:BB:CC:DD:EE:02 Backup Headphones\n");
        return true;
    }
    if (modern_daemon_installed && modern_ctl_installed && strcmp(argv[0], "/usr/bin/bluealsactl") == 0) {
        if (strcmp(argv[1], "list-pcms") == 0) {
            assert(out && size);
            snprintf(out, size, "%s", source_pcm_present ?
                     "/org/bluealsa/hci0/dev_source/a2dpsrc/sink\n" : "");
            return true;
        }
        assert(strcmp(argv[1], "soft-volume") == 0);
        assert(strcmp(argv[3], "on") == 0 || strcmp(argv[3], "off") == 0);
        soft_volume_calls++;
        snprintf(soft_volume_path, sizeof(soft_volume_path), "%s", argv[2]);
        snprintf(soft_volume_state, sizeof(soft_volume_state), "%s", argv[3]);
        return !soft_volume_fail;
    }
    assert(strcmp(argv[0], "ps") == 0);
    assert(out && size);
    if (process_present)
        snprintf(out, size, "12345 %s\n", process_cmdline ? process_cmdline :
                 (modern_daemon_installed && modern_ctl_installed ? "bluealsad" : "bluealsa"));
    else out[0] = '\0';
    return true;
}

bool subprocess_popen(char * const argv[], pid_t * pid, int * fd) {
    (void)argv; (void)pid; (void)fd;
    assert(!"unexpected monitor process");
    return false;
}

void subprocess_terminate(pid_t pid) {
    (void)pid;
    assert(!"unexpected process termination");
}

int __wrap_pthread_create(pthread_t * thread, const pthread_attr_t * attr,
                          void * (*start)(void *), void * arg) {
    (void)thread; (void)attr; (void)start; (void)arg;
    assert(!"unexpected monitor thread");
    return EAGAIN;
}

FILE * __real_fopen(const char * path, const char * mode);

static ssize_t failing_write(void * cookie, const char * data, size_t size) {
    (void)cookie;
    (void)data;
    if (io_mode == IO_WRITE_FAIL) {
        errno = ENOSPC;
        return -1;
    }
    return (ssize_t)size;
}

static int failing_close(void * cookie) {
    (void)cookie;
    if (io_mode == IO_CLOSE_FAIL) {
        errno = EIO;
        return -1;
    }
    return 0;
}

FILE * __wrap_fopen(const char * path, const char * mode) {
    if (strncmp(path, "/proc/", 6) == 0) {
        assert(strcmp(path, "/proc/12345/cmdline") == 0);
        assert(strcmp(mode, "r") == 0);
        expect_locked();
        return fmemopen((void *)process_cmdline, process_cmdline_size, "r");
    }
    if (strcmp(path, "/usr/data/alsa.conf") != 0)
        return __real_fopen(path, mode);
    assert(strcmp(mode, "w") == 0);
    config_opens++;
    if (io_mode == IO_OPEN_FAIL) {
        errno = EACCES;
        return NULL;
    }
    if (io_mode != IO_OK) {
        cookie_io_functions_t ops = { .write = failing_write, .close = failing_close };
        FILE * f = fopencookie(NULL, "w", ops);
        assert(f);
        /* Force fprintf to observe write errors before fclose. */
        if (io_mode == IO_WRITE_FAIL) assert(setvbuf(f, NULL, _IONBF, 0) == 0);
        return f;
    }
    return __real_fopen(config_path, mode);
}

static void cleanup(void) {
    unlink(config_path);
}

static void expect_config(const char * codec, const char * quality) {
    char contents[1024];
    FILE * f = fopen(config_path, "r");
    assert(f);
    size_t n = fread(contents, 1, sizeof(contents) - 1, f);
    assert(!ferror(f) && feof(f));
    contents[n] = '\0';
    assert(fclose(f) == 0);
    assert(strstr(contents, "pcm.bt_alsa_sink {\n"));
    assert(strstr(contents, "type bluealsa\n"));
    assert(strstr(contents, "profile \"a2dp\"\n"));
    if (codec) {
        char line[80];
        snprintf(line, sizeof(line), "            codec \"%s\"\n", codec);
        assert(strstr(contents, line));
    } else {
        assert(!strstr(contents, "codec "));
    }
    if (quality) assert(strstr(contents, quality));
    else assert(!strstr(contents, "ldac_eqmid"));
}

static void test_daemon_argv(void) {
    static const char source[] = "/usr/bin/bluealsa\0-p\0a2dp-source\0";
    static const char sink[] = "bluealsa\0-p\0a2dp-sink\0";
    static const char other[] = "/usr/bin/bluealsa-aplay\0-p\0a2dp-source\0";
    const struct { const char * args; size_t size; bool present; } cases[] = {
        {source, sizeof(source), true},
        {sink, sizeof(sink), true},
        {other, sizeof(other), true},
        {source, sizeof(source), false},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        process_cmdline = cases[i].args;
        process_cmdline_size = cases[i].size;
        process_present = cases[i].present;
        kill_calls = spawn_calls = 0;
        check_lock = true;
        ensure_bluealsa_running();
        check_lock = false;
        assert(kill_calls == 0);
        assert(spawn_calls == (unsigned)(!cases[i].present || cases[i].args == other));
        if (spawn_calls) {
            assert(spawned_argc == 4);
            assert(strcmp(spawned[0], "bluealsa") == 0);
            assert(strcmp(spawned[1], "-p") == 0);
            assert(strcmp(spawned[2], "a2dp-source") == 0);
            assert(strcmp(spawned[3], "--a2dp-volume") == 0);
        }
        assert(pthread_mutex_trylock(&bt_daemon_respawn_mutex) == 0);
        pthread_mutex_unlock(&bt_daemon_respawn_mutex);
    }

    for (int dac = 0; dac < 2; dac++) for (int volume = 0; volume < 2; volume++) {
        kill_calls = spawn_calls = 0;
        check_lock = true;
        assert(!bt_control_apply_output_settings(dac, volume));
        check_lock = false;
        assert(spawn_calls == 1 && kill_calls == 3);
        assert(strcmp(spawned[0], "bluealsa") == 0);
        assert(strcmp(spawned[1], "-p") == 0);
        assert(strcmp(spawned[2], dac ? "a2dp-sink" : "a2dp-source") == 0);
        size_t n = 3;
        if (volume) assert(strcmp(spawned[n++], "--a2dp-volume") == 0);
        assert(spawned_argc == n);
        assert(pthread_mutex_trylock(&bt_daemon_respawn_mutex) == 0);
        pthread_mutex_unlock(&bt_daemon_respawn_mutex);
    }
}

static void test_sbc_xq_lifecycle(void) {
    static const char legacy_auto[] = "bluealsa\0-p\0a2dp-source\0";
    static const char legacy_xq[] = "bluealsa\0-p\0a2dp-source\0--sbc-quality=xq\0";
    static const char legacy_sink_profile[] = "bluealsa\0--profile=a2dp-sink\0";
    static const char legacy_sink[] = "bluealsa\0-p\0a2dp-sink\0";
    static const char modern_auto[] = "bluealsad\0-p\0a2dp-source\0";
    static const char modern_xq[] = "bluealsad\0-p\0a2dp-source\0--sbc-quality=xq\0";
    static const char modern_xq_split[] = "bluealsad\0--profile\0a2dp-source\0--sbc-quality\0xq\0";
    static const char modern_sink_profile[] = "bluealsad\0--profile=a2dp-sink\0";
    static const char modern_sink[] = "bluealsad\0-p\0a2dp-sink\0";

    /* Legacy source: stale quality is reconciled, and the matching daemon is
     * stable across repeated background bring-up checks. */
    modern_daemon_installed = modern_ctl_installed = false;
    bt_control_restore_codec_preference("sbc_xq");
    assert(strcmp(bt_control_get_playback_pcm(), "bluealsa:CODEC=SBC") == 0);
    process_cmdline = legacy_auto; process_cmdline_size = sizeof(legacy_auto);
    process_present = true; retain_process_after_kill = true; kill_calls = spawn_calls = 0;
    ensure_bluealsa_running();
    assert(kill_calls == 1 && spawn_calls == 1 && spawned_argc == 5);
    assert(strcmp(spawned[0], "bluealsa") == 0);
    assert(strcmp(spawned[3], "--a2dp-volume") == 0);
    assert(strcmp(spawned[4], "--sbc-quality=xq") == 0);
    retain_process_after_kill = false;
    process_cmdline = legacy_xq; process_cmdline_size = sizeof(legacy_xq);
    process_present = true; kill_calls = spawn_calls = 0;
    ensure_bluealsa_running();
    assert(kill_calls == 0 && spawn_calls == 0);
    process_cmdline = legacy_sink_profile; process_cmdline_size = sizeof(legacy_sink_profile);
    process_present = true; kill_calls = spawn_calls = 0;
    ensure_bluealsa_running();
    assert(kill_calls == 0 && spawn_calls == 0);
    process_cmdline = legacy_sink; process_cmdline_size = sizeof(legacy_sink);
    process_present = true; kill_calls = spawn_calls = 0;
    ensure_bluealsa_running();
    assert(kill_calls == 0 && spawn_calls == 0);
    /* Profile changes must keep SBC-XQ source-only in the legacy daemon. */
    kill_calls = spawn_calls = 0;
    assert(!bt_control_apply_output_settings(false, true));
    assert(spawn_calls == 1 && kill_calls == 3 && spawned_argc == 5);
    assert(strcmp(spawned[3], "--a2dp-volume") == 0);
    assert(strcmp(spawned[4], "--sbc-quality=xq") == 0);
    kill_calls = spawn_calls = 0;
    assert(!bt_control_apply_output_settings(true, false));
    assert(spawn_calls == 1 && kill_calls == 3 && spawned_argc == 3);

    /* Modern source: same reconciliation, without the legacy volume flag. */
    modern_daemon_installed = modern_ctl_installed = true;
    process_cmdline = modern_auto; process_cmdline_size = sizeof(modern_auto);
    process_present = true; retain_process_after_kill = true; kill_calls = spawn_calls = 0;
    ensure_bluealsa_running();
    assert(kill_calls == 1 && spawn_calls == 1 && spawned_argc == 4);
    assert(strcmp(spawned[0], "/usr/bin/bluealsad") == 0);
    assert(strcmp(spawned[3], "--sbc-quality=xq") == 0);
    retain_process_after_kill = false;
    process_cmdline = modern_xq; process_cmdline_size = sizeof(modern_xq);
    process_present = true; kill_calls = spawn_calls = 0;
    ensure_bluealsa_running();
    assert(kill_calls == 0 && spawn_calls == 0);
    process_cmdline = modern_xq_split; process_cmdline_size = sizeof(modern_xq_split);
    process_present = true; kill_calls = spawn_calls = 0;
    ensure_bluealsa_running();
    assert(kill_calls == 0 && spawn_calls == 0);
    process_cmdline = modern_sink_profile; process_cmdline_size = sizeof(modern_sink_profile);
    process_present = true; kill_calls = spawn_calls = 0;
    ensure_bluealsa_running();
    assert(kill_calls == 0 && spawn_calls == 0);

    /* Modern profile changes use the modern daemon and still keep XQ off the
     * receiver/sink profile. */
    kill_calls = spawn_calls = 0;
    assert(!bt_control_apply_output_settings(false, false));
    assert(spawn_calls == 1 && kill_calls == 3 && spawned_argc == 4);
    assert(strcmp(spawned[0], "/usr/bin/bluealsad") == 0);
    assert(strcmp(spawned[3], "--sbc-quality=xq") == 0);
    kill_calls = spawn_calls = 0;
    assert(!bt_control_apply_output_settings(true, false));
    assert(spawn_calls == 1 && kill_calls == 3 && spawned_argc == 3);
    assert(strcmp(spawned[0], "/usr/bin/bluealsad") == 0);
    process_cmdline = modern_sink; process_cmdline_size = sizeof(modern_sink);
    process_present = true; kill_calls = spawn_calls = 0;
    ensure_bluealsa_running();
    assert(kill_calls == 0 && spawn_calls == 0);

    /* A failed config write must not change the active preference. */
    io_mode = IO_OK;
    assert(bt_control_set_codec("sbc_xq"));
    assert(strcmp(bt_control_get_playback_pcm(), "bluealsa:CODEC=SBC") == 0);
    io_mode = IO_WRITE_FAIL;
    assert(!bt_control_set_codec("auto"));
    assert(strcmp(bt_control_get_playback_pcm(), "bluealsa:CODEC=SBC") == 0);
    io_mode = IO_OK;
    bt_control_restore_codec_preference("auto");
    assert(strcmp(bt_control_get_playback_pcm(), "bluealsa") == 0);
    modern_daemon_installed = modern_ctl_installed = false;
}

static void test_modern_argv(void) {
    static const char source[] = "bluealsad\0-p\0a2dp-source\0";
    modern_daemon_installed = true;
    modern_ctl_installed = true;
    process_cmdline = source;
    process_cmdline_size = sizeof(source);
    process_present = false;
    kill_calls = spawn_calls = 0;
    ensure_bluealsa_running();
    assert(kill_calls == 0 && spawn_calls == 1);
    assert(spawned_argc == 3);
    assert(strcmp(spawned[0], "/usr/bin/bluealsad") == 0);
    assert(strcmp(spawned[1], "-p") == 0);
    assert(strcmp(spawned[2], "a2dp-source") == 0);

    soft_volume_calls = 0;
    soft_volume_checked_calls = 0;
    atomic_store(&modern_soft_volume_requested, true);
    bluealsa_apply_soft_volume("/org/bluealsa/hci0/dev_old/a2dpsnk/source");
    assert(soft_volume_checked_calls == 1);
    assert(soft_volume_calls == 0);
    assert(strcmp(soft_volume_state, "on") == 0);
    bluealsa_apply_soft_volume("/org/bluealsa/hci0/dev_old/a2dpsnk/source");
    assert(soft_volume_checked_calls == 1);
    bluealsa_apply_soft_volume("/org/bluealsa/hci0/dev_new/a2dpsnk/source");
    assert(soft_volume_checked_calls == 2);
    bluealsa_clear_soft_volume_path("/org/bluealsa/hci0/dev_old/a2dpsnk/source");
    bluealsa_apply_soft_volume("/org/bluealsa/hci0/dev_new/a2dpsnk/source");
    assert(soft_volume_checked_calls == 2); /* stale PCMRemoved must not evict new PCM */
    atomic_store(&modern_soft_volume_requested, false);
    bluealsa_clear_soft_volume_path(NULL);
    bluealsa_apply_soft_volume("/org/bluealsa/hci0/dev_new/a2dpsnk/source");
    assert(soft_volume_checked_calls == 3 && strcmp(soft_volume_state, "off") == 0);
    soft_volume_fail = true;
    bluealsa_clear_soft_volume_path(NULL);
    bluealsa_apply_soft_volume("/org/bluealsa/hci0/dev_new/a2dpsnk/source");
    assert(soft_volume_checked_calls == 4);
    soft_volume_fail = false;
    bluealsa_apply_soft_volume("/org/bluealsa/hci0/dev_new/a2dpsnk/source");
    assert(soft_volume_checked_calls == 5); /* nonzero exit must be retried */
    bluealsa_apply_soft_volume("/org/bluealsa/hci0/dev_receiver/a2dpsrc/sink");
    assert(soft_volume_checked_calls == 6 && strcmp(soft_volume_state, "off") == 0);
    source_pcm_present = true;
    bluealsa_clear_soft_volume_path(NULL);
    assert(find_source_pcm_path(discovered_source_path, sizeof(discovered_source_path)));
    assert(strcmp(discovered_source_path, "/org/bluealsa/hci0/dev_source/a2dpsrc/sink") == 0);
    assert(soft_volume_checked_calls == 7); /* initial discovery applies SoftVolume */

    assert(!bt_control_apply_output_settings(false, true));
    assert(!atomic_load(&modern_soft_volume_requested));
    kill_calls = spawn_calls = 0;
    assert(!bt_control_apply_output_settings(false, false));
    assert(spawn_calls == 1 && kill_calls == 3);
    assert(atomic_load(&modern_soft_volume_requested));
    assert(strcmp(spawned[0], "/usr/bin/bluealsad") == 0);
    assert(strcmp(spawned[1], "-p") == 0);
    assert(strcmp(spawned[2], "a2dp-source") == 0);
    modern_daemon_installed = modern_ctl_installed = false;
}

static void test_partial_modern_install(void) {
    static const char source[] = "bluealsa\0-p\0a2dp-source\0";
    process_cmdline = source;
    process_cmdline_size = sizeof(source);
    for (int missing = 0; missing < 2; missing++) {
        modern_daemon_installed = missing == 1;
        modern_ctl_installed = missing == 0;
        process_present = false;
        spawn_calls = kill_calls = 0;
        ensure_bluealsa_running();
        assert(spawn_calls == 1);
        assert(strcmp(spawned[0], "bluealsa") == 0);
        assert(strcmp(spawned[3], "--a2dp-volume") == 0);
    }
}

static void test_bluez_paired_devices(void) {
    bt_device_t devices[2] = {0};

    /* A failed, empty, or truncated help probe must not poison the cache or
     * fall back blindly to a command the current bluetoothctl may not implement. */
    bt_bluetoothctl_devices_capability = BT_BLUETOOTHCTL_DEVICES_UNKNOWN;
    bluez_cli_mode = BLUEZ_CLI_EMPTY_HELP;
    bluez_help_calls = bluez_help_failures = bluez_paired_calls = 0;
    assert(bt_control_list_paired_states(devices, 2) == -1);
    assert(bluez_help_calls == 1 && bluez_paired_calls == 0);
    bluez_cli_mode = BLUEZ_CLI_TRUNCATED_HELP;
    assert(bt_control_list_paired_states(devices, 2) == -1);
    assert(bluez_help_calls == 2 && bluez_paired_calls == 0);
    bluez_cli_mode = BLUEZ_CLI_LEGACY;
    bluez_help_failures = 1;
    assert(bt_control_list_paired_states(devices, 2) == -1);
    assert(bluez_help_calls == 3 && bluez_paired_calls == 0);
    assert(bt_control_list_paired_states(devices, 2) == 2);
    assert(bluez_help_calls == 4 && bluez_paired_calls == 1);
    assert(strcmp(devices[0].mac, "AA:BB:CC:DD:EE:01") == 0);
    assert(strcmp(devices[0].name, "Headphones") == 0);
    assert(devices[0].paired && !devices[0].connected);
    assert(bt_control_is_connected());
    assert(bluez_help_calls == 4 && bluez_paired_calls == 2);

    /* 5.87's help marker selects `devices Paired`, and the successful
     * capability remains cached across both paired-list callers. */
    bt_bluetoothctl_devices_capability = BT_BLUETOOTHCTL_DEVICES_UNKNOWN;
    bluez_cli_mode = BLUEZ_CLI_MODERN;
    bluez_help_calls = bluez_paired_calls = 0;
    assert(bt_control_list_paired_states(devices, 2) == 2);
    assert(bluez_help_calls == 1 && bluez_paired_calls == 1);
    assert(strcmp(devices[1].name, "Backup Headphones") == 0);
    assert(devices[1].paired && devices[1].connected);
    assert(bt_control_list_paired_states(devices, 2) == 2);
    assert(bluez_help_calls == 1 && bluez_paired_calls == 2);
}

int main(void) {
    /* A locking regression must fail promptly rather than hang the target. */
    alarm(10);
    int fd = mkstemp(config_path);
    assert(fd >= 0);
    assert(close(fd) == 0);
    assert(atexit(cleanup) == 0);

    int monitor_volume = -1;
    assert(parse_monitor_volume("0x7f7f", &monitor_volume) && monitor_volume == 127);
    assert(parse_monitor_volume("127", &monitor_volume) && monitor_volume == 127);
    assert(parse_monitor_volume("127[M]", &monitor_volume) && monitor_volume == 127);
    assert(!parse_monitor_volume("127junk", &monitor_volume));
    assert(!parse_monitor_volume("128", &monitor_volume));
    assert(!parse_monitor_volume("0x7f7fjunk", &monitor_volume));

    const char * ordinary[] = { "auto", "sbc", "aac", "ldac", "ldac_hq", "ldac_sq", NULL };
    for (size_t i = 0; ordinary[i]; i++) {
        assert(bt_control_set_codec(ordinary[i]));
        if (strncmp(ordinary[i], "ldac", 4) == 0)
            expect_config("ldac", strcmp(ordinary[i], "ldac_hq") == 0 ?
                          "ldac_eqmid \"LDAC_HQ\"" : "ldac_eqmid \"LDAC_SQ\"");
        else expect_config(strcmp(ordinary[i], "auto") == 0 ? NULL : ordinary[i], NULL);
    }

    for (int failure = IO_OPEN_FAIL; failure <= IO_CLOSE_FAIL; failure++) {
        io_mode = IO_OK;
        assert(bt_control_set_codec("auto"));
        io_mode = failure;
        unsigned before = config_opens;
        assert(!bt_control_set_codec("aac"));
        assert(config_opens == before + 1);
    }
    io_mode = IO_OK;
    assert(bt_control_set_codec("auto"));
    expect_config(NULL, NULL);
    test_daemon_argv();
    test_sbc_xq_lifecycle();
    test_modern_argv();
    test_partial_modern_install();
    test_bluez_paired_devices();
    alarm(0);
    puts("bluetooth-codec-selftest: PASS (config, I/O failures, daemon detection/argv, BlueALSA5, BlueZ compatibility)");
    return 0;
}
