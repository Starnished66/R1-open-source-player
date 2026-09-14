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
static unsigned proc_index, kill_calls, spawn_calls;
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
    if (strcmp(needle, "bluealsa") == 0) process_present = false;
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

bool subprocess_run(char * const argv[], char * out, size_t size) {
    expect_locked();
    assert(strcmp(argv[0], "ps") == 0);
    assert(out && size);
    snprintf(out, size, "%s", process_present ? "12345 bluealsa\n" : "");
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

static void expect_pcm(const char * expected) {
    assert(strcmp(bt_control_get_playback_pcm(), expected) == 0);
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
    assert(!strstr(contents, "sbc_xq"));
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

static void test_restart_and_argv(void) {
    static const char source[] = "/usr/bin/bluealsa\0-p\0a2dp-source\0";
    static const char xq_source[] = "bluealsa\0-p\0a2dp-source\0--sbc-quality=xq\0";
    static const char sink[] = "bluealsa\0-p\0a2dp-sink\0";
    static const char xq_sink[] = "bluealsa\0-p\0a2dp-sink\0--sbc-quality=xq\0";
    static const char other[] = "/usr/bin/bluealsa-aplay\0-p\0a2dp-source\0";
    const struct { const char * args; size_t size; bool xq, restart; } cases[] = {
        {source, sizeof(source), true, true},
        {xq_source, sizeof(xq_source), false, true},
        {source, sizeof(source), false, false},
        {xq_source, sizeof(xq_source), true, false},
        {sink, sizeof(sink), true, false},
        {sink, sizeof(sink), false, false},
        {xq_sink, sizeof(xq_sink), false, false},
        {other, sizeof(other), true, false},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        process_cmdline = cases[i].args;
        process_cmdline_size = cases[i].size;
        process_present = true;
        assert(bluealsa_needs_source_restart(cases[i].xq) == cases[i].restart);
        if (cases[i].args == other) continue; /* ps stub models bluealsa only. */
        bt_control_restore_codec_preference(cases[i].xq ? "sbc_xq" : "auto");
        kill_calls = spawn_calls = 0;
        check_lock = true;
        ensure_bluealsa_running();
        check_lock = false;
        assert(kill_calls == (unsigned)cases[i].restart);
        assert(spawn_calls == (unsigned)cases[i].restart);
        if (cases[i].restart) {
            assert(spawned_argc == (cases[i].xq ? 5u : 4u));
            assert(strcmp(spawned[0], "bluealsa") == 0);
            assert(strcmp(spawned[1], "-p") == 0);
            assert(strcmp(spawned[2], "a2dp-source") == 0);
            assert(strcmp(spawned[3], "--a2dp-volume") == 0);
            if (cases[i].xq) assert(strcmp(spawned[4], "--sbc-quality=xq") == 0);
        }
        assert(pthread_mutex_trylock(&bt_daemon_respawn_mutex) == 0);
        pthread_mutex_unlock(&bt_daemon_respawn_mutex);
    }

    for (int dac = 0; dac < 2; dac++) for (int volume = 0; volume < 2; volume++) {
        for (int xq = 0; xq < 2; xq++) {
            bt_control_restore_codec_preference(xq ? "sbc_xq" : "auto");
            kill_calls = spawn_calls = 0;
            check_lock = true;
            assert(!bt_control_apply_output_settings(dac, volume));
            check_lock = false;
            assert(spawn_calls == 1 && kill_calls == 3);
            assert(strcmp(spawned[0], "/usr/bin/bluealsa") == 0);
            assert(strcmp(spawned[1], "-p") == 0);
            assert(strcmp(spawned[2], dac ? "a2dp-sink" : "a2dp-source") == 0);
            size_t n = 3;
            if (volume) assert(strcmp(spawned[n++], "--a2dp-volume") == 0);
            if (xq && !dac) assert(strcmp(spawned[n++], "--sbc-quality=xq") == 0);
            assert(spawned_argc == n);
            assert(pthread_mutex_trylock(&bt_daemon_respawn_mutex) == 0);
            pthread_mutex_unlock(&bt_daemon_respawn_mutex);
        }
    }
}

int main(void) {
    /* A locking regression must fail promptly rather than hang the target. */
    alarm(10);
    int fd = mkstemp(config_path);
    assert(fd >= 0);
    assert(close(fd) == 0);
    assert(atexit(cleanup) == 0);

    expect_pcm("bluealsa");
    const char * ordinary[] = { "auto", "sbc", "aac", "ldac", "ldac_hq", "ldac_sq", NULL };
    for (size_t i = 0; i < sizeof(ordinary) / sizeof(ordinary[0]); i++) {
        unsigned before = config_opens;
        bt_control_restore_codec_preference("sbc_xq");
        expect_pcm("bluealsa:CODEC=SBC");
        bt_control_restore_codec_preference(ordinary[i]);
        expect_pcm("bluealsa");
        assert(config_opens == before); /* Restore must not rewrite configuration. */
    }

    for (size_t i = 0; ordinary[i]; i++) {
        assert(bt_control_set_codec("sbc_xq"));
        expect_pcm("bluealsa:CODEC=SBC");
        expect_config("sbc", NULL);
        assert(bt_control_set_codec(ordinary[i]));
        expect_pcm("bluealsa");
        if (strncmp(ordinary[i], "ldac", 4) == 0)
            expect_config("ldac", strcmp(ordinary[i], "ldac_hq") == 0 ?
                          "ldac_eqmid \"LDAC_HQ\"" : "ldac_eqmid \"LDAC_SQ\"");
        else expect_config(strcmp(ordinary[i], "auto") == 0 ? NULL : ordinary[i], NULL);
    }

    for (int failure = IO_OPEN_FAIL; failure <= IO_CLOSE_FAIL; failure++) {
        for (int xq = 0; xq < 2; xq++) {
            io_mode = IO_OK;
            assert(bt_control_set_codec(xq ? "sbc_xq" : "auto"));
            io_mode = failure;
            unsigned before = config_opens;
            assert(!bt_control_set_codec(xq ? "aac" : "sbc_xq"));
            assert(config_opens == before + 1);
            expect_pcm(xq ? "bluealsa:CODEC=SBC" : "bluealsa");
        }
    }
    io_mode = IO_OK;
    assert(bt_control_set_codec("auto"));
    expect_pcm("bluealsa");
    expect_config(NULL, NULL);
    test_restart_and_argv();
    alarm(0);
    puts("bluetooth-codec-selftest: PASS (restore, config, I/O failures, restart decisions, serialized fallback, daemon argv)");
    return 0;
}
