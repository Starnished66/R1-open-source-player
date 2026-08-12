#include "dlna_control.h"
#include "subprocess.h"
#include "http_client.h"
#include "debug_log.h"

#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>

/* The undocumented socket dmrd's own gmrender-resurrect-derived code
 * expects its playback companion to be listening on -- see dlna_control.h
 * for how this was found (live traffic capture on a real device, not a
 * spec). Always under /usr/data (symlinked to /data on this firmware, see
 * fallback_font.c's own /usr/resource precedent for a similar "the real
 * path is a symlink target" note), the one writable partition. */
#define DMR_STREAMER_SOCKET_PATH "/data/dmr_streamer"

/* Real-device incident, first fix: this used to be under /tmp, which this
 * firmware mounts as tmpfs (RAM-backed, confirmed via `mount`) -- on a
 * device with only ~57MB total RAM and only a few MB typically free,
 * downloading any real-sized cast track (a FLAC can easily be tens of MB)
 * into /tmp filled memory directly and got the whole app OOM-killed by the
 * kernel partway through, which looked to the user like the device freezing
 * (confirmed via `dmesg`: "Out of memory: Kill process ... (hiby_player)").
 *
 * Real-device incident, second fix: moving it to /data (the same writable
 * UBIFS partition DMR_STREAMER_SOCKET_PATH above uses) fixed the OOM, but
 * that partition is only ~35.8MB total, most of it already used by the
 * firmware/app itself -- confirmed live: a real 61MB cast FLAC failed
 * partway through download (`read_body failed`) because /data simply
 * doesn't have room for a file that size, and even a middling one would
 * fight with test binaries or normal app growth for the same few MB. The
 * SD card (MUSIC_ROOT_DIR, same as gui.c's own Subsonic download-to-
 * library feature already uses for exactly this "need to write a real,
 * possibly-large audio file somewhere" need) has actual capacity instead.
 * A dot-prefixed subdirectory keeps it out of the library scan entirely --
 * file_browser.c's scan_all_songs_recursive() skips any dirent whose name
 * starts with '.', so a cast track never briefly shows up as a phantom
 * library entry. Falls back to failing the download gracefully (existing
 * !ok handling in dlna_download_thread_func()) if no SD card is mounted at
 * all, same as Subsonic download-to-library already does in that case --
 * this device genuinely has nowhere else with enough room for a real audio
 * file. */
#ifdef HOST_BUILD
#define DLNA_TEMP_DOWNLOAD_DIR "./music/.dlna_cast"
#else
#define DLNA_TEMP_DOWNLOAD_DIR "/data/mnt/sd_0/.dlna_cast"
#endif
#define DLNA_TEMP_DOWNLOAD_PATH DLNA_TEMP_DOWNLOAD_DIR "/dlna_track.download"

static pthread_mutex_t dlna_mutex = PTHREAD_MUTEX_INITIALIZER;

static bool running = false;
static int listen_fd = -1;
static pthread_t listener_thread;

/* Accumulated from set_uri:/set_meta: commands as they arrive, one at a
 * time, ahead of the play@N that actually triggers a download -- mirrors
 * dmrd's own "SetNextAVTransportURI queues these, Play consumes them"
 * flow. Guarded by dlna_mutex since the download thread reads a snapshot
 * of these when play@N fires. */
static char pending_uri[2048] = {0};
static char pending_title[256] = {0};
static char pending_artist[256] = {0};
static char pending_album[256] = {0};

static volatile bool stop_requested = false;

static bool track_ready = false;
static char ready_path[512] = {0};
static char ready_title[256] = {0};
static char ready_artist[256] = {0};
static char ready_album[256] = {0};
static char last_produced_path[512] = {0}; /* superseded on the next successful download, then removed */

/* Guarded by dlna_mutex like everything else above -- see dlna_control_
 * notify_status()'s own doc comment in the header. */
static bool status_playing = false;
static bool status_paused = false;

/* Real-device finding: the phone's DLNA controller app queries state@
 * exactly once, immediately after sending play@ -- and never again,
 * apparently treating that single answer as the lasting transport state
 * (its later polling is get_volume only, not another state@). This app's
 * download-then-play architecture (see dlna_control.h's own reasoning)
 * means real playback only starts once the download finishes, well after
 * that single query -- so status_playing was still (correctly, at that
 * instant) false, the controller's UI never got told it was actually
 * playing, and the app looked "stuck" from the phone's side even though
 * audio was playing fine on the device itself. Confirmed live via a raw
 * command trace: play@0 immediately followed by state@0, replied to with
 * the real-but-premature "STOPPED", then the download proceeding
 * afterward with nothing left to tell the controller otherwise.
 *
 * Fixed by answering that one query optimistically: as soon as handle_play()
 * accepts a valid URI (matching what a real-time-streaming DLNA renderer's
 * own instant Play->PLAYING transition would look like from the
 * controller's side), state@ reports PLAYING immediately, before the
 * download has even started -- cleared once the real state actually
 * catches up (dlna_control_notify_status(playing=true)) or a Stop is
 * processed, so it doesn't linger past its purpose. */
static bool cast_intent_playing = false;

typedef struct {
    char uri[2048];
    char title[256];
    char artist[256];
    char album[256];
} dlna_download_request_t;

/* Maps a small, deliberately non-exhaustive set of real-world audio
 * Content-Type values to one of the extensions audio.c's decoder_open()
 * actually dispatches on (see its own strcasecmp(ext, ...) chain) -- a
 * DLNA SetAVTransportURI media URL is typically an opaque token with no
 * extension of its own (confirmed live: every real capture during this
 * feature's investigation had none), so the server's declared type is the
 * only signal available. Returns NULL (caller should give up rather than
 * guess) for anything not confidently one of the formats this app
 * actually supports -- e.g. the real capture that turned out to be a
 * miscast .lrc lyrics file came back as "application/octet-stream", which
 * intentionally maps to nothing here rather than being guessed as audio. */
static const char * extension_for_content_type(const char * content_type) {
    if (strcasecmp(content_type, "audio/flac") == 0 || strcasecmp(content_type, "audio/x-flac") == 0) return ".flac";
    if (strcasecmp(content_type, "audio/mpeg") == 0 || strcasecmp(content_type, "audio/mp3") == 0) return ".mp3";
    if (strcasecmp(content_type, "audio/wav") == 0 || strcasecmp(content_type, "audio/x-wav") == 0 ||
        strcasecmp(content_type, "audio/wave") == 0 || strcasecmp(content_type, "audio/vnd.wave") == 0) return ".wav";
    if (strcasecmp(content_type, "audio/aiff") == 0 || strcasecmp(content_type, "audio/x-aiff") == 0) return ".aiff";
    if (strcasecmp(content_type, "audio/aac") == 0 || strcasecmp(content_type, "audio/aacp") == 0) return ".aac";
    if (strcasecmp(content_type, "audio/mp4") == 0 || strcasecmp(content_type, "audio/x-m4a") == 0 ||
        strcasecmp(content_type, "audio/m4a") == 0) return ".m4a";
    if (strcasecmp(content_type, "audio/x-ape") == 0 || strcasecmp(content_type, "audio/ape") == 0) return ".ape";
    if (strcasecmp(content_type, "audio/x-ms-wma") == 0) return ".wma";
    return NULL;
}

static void * dlna_download_thread_func(void * arg) {
    dlna_download_request_t * req = (dlna_download_request_t *) arg;

    mkdir(DLNA_TEMP_DOWNLOAD_DIR, 0755); /* ignore EEXIST -- same best-effort pattern as playlist_files_create() */

    char content_type[128];
    bool ok = http_get_to_file_ex(req->uri, true, DLNA_TEMP_DOWNLOAD_PATH, NULL, NULL,
                                   content_type, sizeof(content_type));
    if (!ok) {
        DBG_LOG("dlna_control: download failed for '%s'\n", req->uri);
        remove(DLNA_TEMP_DOWNLOAD_PATH);
        free(req);
        return NULL;
    }

    const char * ext = extension_for_content_type(content_type);
    if (!ext) {
        DBG_LOG("dlna_control: unrecognized/non-audio Content-Type '%s', not playing\n", content_type);
        remove(DLNA_TEMP_DOWNLOAD_PATH);
        free(req);
        return NULL;
    }

    /* Same reasoning as DLNA_TEMP_DOWNLOAD_PATH above -- rename() also
     * requires the source and destination to be on the same filesystem, so
     * this must stay in step with wherever that constant points. */
    char final_path[512];
    snprintf(final_path, sizeof(final_path), DLNA_TEMP_DOWNLOAD_DIR "/dlna_track%s", ext);
    if (rename(DLNA_TEMP_DOWNLOAD_PATH, final_path) != 0) {
        DBG_LOG("dlna_control: rename to '%s' failed\n", final_path);
        remove(DLNA_TEMP_DOWNLOAD_PATH);
        free(req);
        return NULL;
    }

    pthread_mutex_lock(&dlna_mutex);
    if (last_produced_path[0] != '\0' && strcmp(last_produced_path, final_path) != 0) {
        remove(last_produced_path);
    }
    snprintf(last_produced_path, sizeof(last_produced_path), "%s", final_path);
    snprintf(ready_path, sizeof(ready_path), "%s", final_path);
    snprintf(ready_title, sizeof(ready_title), "%s", req->title);
    snprintf(ready_artist, sizeof(ready_artist), "%s", req->artist);
    snprintf(ready_album, sizeof(ready_album), "%s", req->album);
    track_ready = true; /* written last -- consumers only check this flag */
    pthread_mutex_unlock(&dlna_mutex);

    free(req);
    return NULL;
}

static void handle_play(void) {
    dlna_download_request_t * req = malloc(sizeof(*req));
    pthread_mutex_lock(&dlna_mutex);
    snprintf(req->uri, sizeof(req->uri), "%s", pending_uri);
    snprintf(req->title, sizeof(req->title), "%s", pending_title);
    snprintf(req->artist, sizeof(req->artist), "%s", pending_artist);
    snprintf(req->album, sizeof(req->album), "%s", pending_album);
    pthread_mutex_unlock(&dlna_mutex);

    if (req->uri[0] == '\0') {
        free(req);
        return;
    }

    pthread_mutex_lock(&dlna_mutex);
    cast_intent_playing = true;
    pthread_mutex_unlock(&dlna_mutex);

    /* Detached, one-shot -- this listener thread must stay free to keep
     * accepting/answering the frequent get_volume polls a real DLNA
     * controller app sends throughout playback (confirmed live: every few
     * seconds), which a multi-second FLAC download would otherwise block
     * behind. */
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&t, &attr, dlna_download_thread_func, req);
    pthread_attr_destroy(&attr);
}

static void handle_connection(int cfd) {
    unsigned char buf[4096];
    ssize_t n = read(cfd, buf, sizeof(buf) - 1);
    if (n <= 0) { close(cfd); return; }
    buf[n] = '\0';
    DBG_LOG("dlna_control: recv '%.100s'\n", (const char *) buf);

    if ((size_t) n >= 10 && memcmp(buf, "get_volume", 10) == 0) {
        /* Real-device finding: dmrd's own GetVolume response construction
         * doesn't reliably reflect this reply back to the controller
         * regardless of its value (silent, no logged error on dmrd's
         * side) -- see dlna_control.h's own comment. Still answered so
         * dmrd's request doesn't hang; the exact value is otherwise
         * inconsequential given that bug. */
        const char * reply = "100";
        write(cfd, reply, strlen(reply));
    } else if ((size_t) n >= 8 && memcmp(buf, "get_mute", 8) == 0) {
        const char * reply = "0";
        write(cfd, reply, strlen(reply));
    } else if ((size_t) n >= 6 && memcmp(buf, "state@", 6) == 0) {
        /* Real-device bug report: a cast played correctly on this device but
         * the phone's own DLNA controller UI never showed it as playing --
         * this used to always reply "STOPPED" here regardless of actual
         * state. dmrd relays this from its own UPnP AVTransport
         * GetTransportInfo, whose CurrentTransportState argument is a fixed
         * enumeration (UPnP AVTransport:1 spec) -- PLAYING/PAUSED_PLAYBACK/
         * STOPPED are the three this app can actually distinguish (no
         * TRANSITIONING/RECORDING/etc. state exists in this app's own
         * playback model). status_playing/status_paused are pushed from
         * gui.c every tick via dlna_control_notify_status(), mirroring
         * audio_is_playing()/audio_is_paused() exactly. */
        pthread_mutex_lock(&dlna_mutex);
        const char * reply = (status_playing || cast_intent_playing) ? "PLAYING"
                              : status_paused                         ? "PAUSED_PLAYBACK"
                                                                       : "STOPPED";
        pthread_mutex_unlock(&dlna_mutex);
        DBG_LOG("dlna_control: replying to state@ with '%s'\n", reply);
        write(cfd, reply, strlen(reply));
    } else if ((size_t) n > 8 && memcmp(buf, "set_uri:", 8) == 0) {
        pthread_mutex_lock(&dlna_mutex);
        snprintf(pending_uri, sizeof(pending_uri), "%s", buf + 8);
        pthread_mutex_unlock(&dlna_mutex);
    } else if ((size_t) n > 16 && memcmp(buf, "set_meta:title:", 15) == 0) {
        pthread_mutex_lock(&dlna_mutex);
        snprintf(pending_title, sizeof(pending_title), "%s", buf + 15);
        pthread_mutex_unlock(&dlna_mutex);
    } else if ((size_t) n > 17 && memcmp(buf, "set_meta:artist:", 16) == 0) {
        pthread_mutex_lock(&dlna_mutex);
        snprintf(pending_artist, sizeof(pending_artist), "%s", buf + 16);
        pthread_mutex_unlock(&dlna_mutex);
    } else if ((size_t) n > 16 && memcmp(buf, "set_meta:album:", 15) == 0) {
        pthread_mutex_lock(&dlna_mutex);
        snprintf(pending_album, sizeof(pending_album), "%s", buf + 15);
        pthread_mutex_unlock(&dlna_mutex);
    } else if ((size_t) n >= 5 && memcmp(buf, "play@", 5) == 0) {
        handle_play();
    } else if ((size_t) n >= 4 && memcmp(buf, "stop", 4) == 0) {
        pthread_mutex_lock(&dlna_mutex);
        cast_intent_playing = false;
        pthread_mutex_unlock(&dlna_mutex);
        stop_requested = true; /* consumed from the LVGL thread */
    }
    /* Everything else (set_meta:albumarturi:/duration:, and any command
     * this feature's scope doesn't cover) is deliberately ignored --
     * accepted and closed with no reply, same as dmrd's own apparent
     * tolerance for a peer that doesn't answer commands it doesn't
     * recognize. */

    close(cfd);
}

/* Real-device incident (2026-08-11): this used to call a plain blocking
 * accept() and rely on dlna_control_stop() closing listen_fd to unblock it
 * -- the exact same pattern remote_control.c's own listener used to use,
 * fixed there for the identical reason (see that file's own
 * ACCEPT_POLL_TIMEOUT_MS comment): on this device's kernel, closing a
 * socket from another thread does NOT reliably wake a thread already
 * blocked in accept() on it. Confirmed live here too -- disabling DLNA
 * froze the whole app (main thread stuck in dlna_control_stop()'s
 * pthread_join(), this thread stuck forever in accept()), needing a hard
 * power-cycle to recover; this file had been explicitly flagged as sharing
 * remote_control.c's own pre-fix bug but was left unfixed until it actually
 * happened. Same fix: poll with a bounded timeout instead of blocking
 * directly in accept(), so this thread notices `running` went false within
 * one poll interval on its own, independent of whatever close()-from-
 * another-thread does or doesn't do on this kernel. */
#define ACCEPT_POLL_TIMEOUT_MS 200

static void * listener_thread_func(void * arg) {
    (void) arg;
    while (running) {
        struct pollfd pfd = { .fd = listen_fd, .events = POLLIN, .revents = 0 };
        int pr = poll(&pfd, 1, ACCEPT_POLL_TIMEOUT_MS);
        if (pr <= 0) continue; /* timeout or interrupted -- just re-check running and poll again */
        if (!running) break;

        int cfd = accept(listen_fd, NULL, NULL);
        if (cfd < 0) continue;
        handle_connection(cfd);
    }
    return NULL;
}

void dlna_control_start(void) {
    if (running) return;

    char * dmrd_argv[] = { (char *) "/usr/bin/dmrd", NULL };
    subprocess_spawn_daemon(dmrd_argv);

    unlink(DMR_STREAMER_SOCKET_PATH);

    listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd < 0) return;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", DMR_STREAMER_SOCKET_PATH);

    if (bind(listen_fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        close(listen_fd);
        listen_fd = -1;
        return;
    }
    chmod(DMR_STREAMER_SOCKET_PATH, 0777); /* dmrd connects as a different, unprivileged-by-default process */

    if (listen(listen_fd, 4) < 0) {
        close(listen_fd);
        listen_fd = -1;
        return;
    }

    running = true;
    stop_requested = false;
    pthread_create(&listener_thread, NULL, listener_thread_func, NULL);
}

void dlna_control_stop(void) {
    if (!running) return;
    running = false;

    /* The listener thread notices `running` went false on its own, within
     * one ACCEPT_POLL_TIMEOUT_MS poll cycle (see listener_thread_func()'s
     * own comment on why this doesn't rely on closing listen_fd to
     * interrupt a blocking accept()) -- so this join is bounded to ~200ms,
     * not indefinite. Only close the fd after the thread has actually
     * exited and is guaranteed to no longer touch it. */
    pthread_join(listener_thread, NULL);
    if (listen_fd >= 0) {
        close(listen_fd);
        listen_fd = -1;
    }
    unlink(DMR_STREAMER_SOCKET_PATH);

    char * argv[] = { (char *) "killall", (char *) "dmrd", NULL };
    subprocess_run(argv, NULL, 0);
}

bool dlna_control_consume_ready_track(char * out_path, size_t path_size,
                                       char * out_title, size_t title_size,
                                       char * out_artist, size_t artist_size,
                                       char * out_album, size_t album_size) {
    pthread_mutex_lock(&dlna_mutex);
    bool result = track_ready;
    if (result) {
        track_ready = false;
        snprintf(out_path, path_size, "%s", ready_path);
        snprintf(out_title, title_size, "%s", ready_title);
        snprintf(out_artist, artist_size, "%s", ready_artist);
        snprintf(out_album, album_size, "%s", ready_album);
    }
    pthread_mutex_unlock(&dlna_mutex);
    return result;
}

bool dlna_control_consume_stop_requested(void) {
    if (!stop_requested) return false;
    stop_requested = false;
    return true;
}

void dlna_control_notify_status(bool playing, bool paused) {
    pthread_mutex_lock(&dlna_mutex);
    status_playing = playing;
    status_paused = paused;
    if (playing) {
        /* Real playback has caught up -- the optimistic flag has done its
         * job (see its declaration comment) and can stop overriding
         * status_paused/status_playing transitions from here on. */
        cast_intent_playing = false;
    }
    pthread_mutex_unlock(&dlna_mutex);
}
