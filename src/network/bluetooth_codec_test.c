/* Host regression tests against the real BlueALSA 5 implementation. Unused
 * device code is discarded by gc-sections. */
#define _GNU_SOURCE
#include "bluetooth_control.c"
#include <assert.h>
#include <errno.h>

static const char * process_cmdline;
static size_t process_cmdline_size;
static bool process_present, check_lock;
static bool retain_process_after_kill;
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
    if (strcmp(needle, "bluealsad") == 0 && !retain_process_after_kill)
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

/* Same fixture as subprocess_run(): the production code niced this call
 * down, which changes the child's scheduling priority, nothing the test
 * fixture models. */
bool subprocess_run_low_priority(char * const argv[], char * out, size_t size) {
    return subprocess_run(argv, out, size);
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
    if (strcmp(argv[0], "/usr/bin/bluealsactl") == 0) {
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
                 "bluealsad");
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

FILE * __wrap_fopen(const char * path, const char * mode) {
    if (strncmp(path, "/proc/", 6) == 0) {
        assert(strcmp(path, "/proc/12345/cmdline") == 0);
        assert(strcmp(mode, "r") == 0);
        expect_locked();
        return fmemopen((void *)process_cmdline, process_cmdline_size, "r");
    }
    return __real_fopen(path, mode);
}

static void test_daemon_argv(void) {
    static const char source[] = "bluealsad\0-p\0a2dp-source\0--all-codecs\0";
    static const char sink[] = "bluealsad\0-p\0a2dp-sink\0";
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
            assert(strcmp(spawned[0], "/usr/bin/bluealsad") == 0);
            assert(strcmp(spawned[1], "-p") == 0);
            assert(strcmp(spawned[2], "a2dp-source") == 0);
            assert(strcmp(spawned[3], "--all-codecs") == 0);
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
        assert(strcmp(spawned[0], "/usr/bin/bluealsad") == 0);
        assert(strcmp(spawned[1], "-p") == 0);
        assert(strcmp(spawned[2], dac ? "a2dp-sink" : "a2dp-source") == 0);
        size_t n = 3;
        if (!dac) assert(strcmp(spawned[n++], "--all-codecs") == 0);
        assert(spawned_argc == n);
        assert(pthread_mutex_trylock(&bt_daemon_respawn_mutex) == 0);
        pthread_mutex_unlock(&bt_daemon_respawn_mutex);
    }
}

static void test_sbc_xq_lifecycle(void) {
    static const char modern_auto[] = "bluealsad\0-p\0a2dp-source\0";
    static const char modern_xq[] = "bluealsad\0-p\0a2dp-source\0--sbc-quality=xq\0";
    static const char modern_xq_split[] = "bluealsad\0--profile\0a2dp-source\0--sbc-quality\0xq\0";
    static const char modern_sink_profile[] = "bluealsad\0--profile=a2dp-sink\0";
    static const char modern_sink[] = "bluealsad\0-p\0a2dp-sink\0";

    bt_control_restore_codec_preference("sbc_xq");
    assert(strcmp(bt_control_get_playback_pcm(), "bluealsa:CODEC=SBC") == 0);
    /* Source reconciliation restarts stale quality settings and is stable
     * when the already-running source is configured correctly. */
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

    assert(bt_control_set_codec("sbc_xq"));
    assert(strcmp(bt_control_get_playback_pcm(), "bluealsa:CODEC=SBC") == 0);
    assert(!bt_control_set_codec("not-a-codec"));
    assert(strcmp(bt_control_get_playback_pcm(), "bluealsa:CODEC=SBC") == 0);
    bt_control_restore_codec_preference("auto");
    assert(strcmp(bt_control_get_playback_pcm(), "bluealsa") == 0);
    bt_control_restore_codec_preference("aptx");
    assert(strcmp(bt_control_get_playback_pcm(), "bluealsa:CODEC=aptX") == 0);
    bt_control_restore_codec_preference("aac");
    assert(strcmp(bt_control_get_playback_pcm(), "bluealsa:CODEC=AAC") == 0);
    bt_control_restore_codec_preference("ldac_hq");
    assert(strcmp(bt_control_get_playback_pcm(), "bluealsa:CODEC=LDAC") == 0);
    bt_control_restore_codec_preference("auto");
}

static void test_modern_argv(void) {
    static const char source[] = "bluealsad\0-p\0a2dp-source\0";
    process_cmdline = source;
    process_cmdline_size = sizeof(source);
    process_present = false;
    kill_calls = spawn_calls = 0;
    ensure_bluealsa_running();
    assert(kill_calls == 0 && spawn_calls == 1);
    assert(spawned_argc == 4);
    assert(strcmp(spawned[0], "/usr/bin/bluealsad") == 0);
    assert(strcmp(spawned[1], "-p") == 0);
    assert(strcmp(spawned[2], "a2dp-source") == 0);
    assert(strcmp(spawned[3], "--all-codecs") == 0);

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

/* 44.1 kHz is the one transport rate the daemon can be told to negotiate, and
 * it is the default, so the argument has to reach the source daemon and stay
 * off the sink. */
static void test_force_audio_cd_argv(void) {
    bt_control_restore_codec_preference("auto");
    bt_control_set_sample_rate(44100);

    process_cmdline = NULL; process_cmdline_size = 0;
    process_present = false; kill_calls = spawn_calls = 0;
    ensure_bluealsa_running();
    assert(spawn_calls == 1 && spawned_argc == 5);
    assert(strcmp(spawned[2], "a2dp-source") == 0);
    assert(strcmp(spawned[3], "--a2dp-force-audio-cd") == 0);
    assert(strcmp(spawned[4], "--all-codecs") == 0);

    /* A daemon already carrying the argument is left alone. */
    static const char forced[] = "/usr/bin/bluealsad\0-p\0a2dp-source\0--a2dp-force-audio-cd\0--all-codecs\0";
    process_cmdline = forced; process_cmdline_size = sizeof(forced);
    process_present = true; kill_calls = spawn_calls = 0;
    ensure_bluealsa_running();
    assert(kill_calls == 0 && spawn_calls == 0);

    /* Automatic must drop it again, which is a real argv mismatch. Nothing is
     * connected, so correcting it is allowed to restart the daemon. */
    bt_control_set_sample_rate(0);
    source_pcm_present = false;
    process_cmdline = forced; process_cmdline_size = sizeof(forced);
    process_present = true; retain_process_after_kill = true; kill_calls = spawn_calls = 0;
    ensure_bluealsa_running();
    assert(kill_calls == 1 && spawn_calls == 1 && spawned_argc == 4);
    assert(strcmp(spawned[3], "--all-codecs") == 0);
    retain_process_after_kill = false;

    /* Same mismatch with an accessory connected must NOT kill the daemon:
     * that would drop a live A2DP link just to correct an argument. */
    source_pcm_present = true;
    process_cmdline = forced; process_cmdline_size = sizeof(forced);
    process_present = true; kill_calls = spawn_calls = 0;
    ensure_bluealsa_running();
    assert(kill_calls == 0 && spawn_calls == 0);
    source_pcm_present = false;

    /* DAC mode never forces the rate: the phone picks it. */
    bt_control_set_sample_rate(44100);
    kill_calls = spawn_calls = 0;
    assert(!bt_control_apply_output_settings(true, false));
    assert(spawn_calls == 1 && spawned_argc == 3);
    assert(strcmp(spawned[2], "a2dp-sink") == 0);
    bt_control_set_sample_rate(0);
}

int main(void) {
    /* A locking regression must fail promptly rather than hang the target. */
    alarm(10);

    int monitor_volume = -1;
    assert(parse_monitor_volume("0x7f7f", &monitor_volume) && monitor_volume == 127);
    assert(parse_monitor_volume("127", &monitor_volume) && monitor_volume == 127);
    assert(parse_monitor_volume("127[M]", &monitor_volume) && monitor_volume == 127);
    assert(!parse_monitor_volume("127junk", &monitor_volume));
    assert(!parse_monitor_volume("128", &monitor_volume));
    assert(!parse_monitor_volume("0x7f7fjunk", &monitor_volume));

    assert(bt_control_set_codec("auto"));
    /* The existing argv assertions below cover codec arguments, so the rate
     * is parked on Automatic to keep them positional. A dedicated case for
     * the 44.1 kHz argument follows them. */
    bt_control_set_sample_rate(0);
    test_daemon_argv();
    test_sbc_xq_lifecycle();
    test_modern_argv();
    test_force_audio_cd_argv();
    test_bluez_paired_devices();
    alarm(0);
    puts("bluetooth-codec-selftest: PASS (BlueALSA 5 daemon/codec behavior, soft volume, BlueZ compatibility)");
    return 0;
}
