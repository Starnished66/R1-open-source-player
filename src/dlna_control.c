#include "dlna_control.h"
#include "subprocess.h"
#include "http_client.h"
#include "debug_log.h"

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

#define DLNA_TEMP_DOWNLOAD_PATH "/tmp/dlna_track.download"

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

    char final_path[512];
    snprintf(final_path, sizeof(final_path), "/tmp/dlna_track%s", ext);
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
        /* Never observed dmrd actually blocking on a missing reply here in
         * testing, but answering costs nothing and matches every other
         * get_-shaped command. */
        const char * reply = "STOPPED";
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
        stop_requested = true; /* consumed from the LVGL thread */
    }
    /* Everything else (set_meta:albumarturi:/duration:, and any command
     * this feature's scope doesn't cover) is deliberately ignored --
     * accepted and closed with no reply, same as dmrd's own apparent
     * tolerance for a peer that doesn't answer commands it doesn't
     * recognize. */

    close(cfd);
}

static void * listener_thread_func(void * arg) {
    (void) arg;
    for (;;) {
        int cfd = accept(listen_fd, NULL, NULL);
        if (cfd < 0) {
            if (!running) break; /* socket closed out from under us by dlna_control_stop() */
            continue;
        }
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

    /* Unblocks the accept() loop -- closing a socket a thread is blocked
     * in accept() on causes that call to return with an error on Linux,
     * which listener_thread_func() checks `running` after to know to exit
     * rather than looping forever on a now-invalid fd. */
    if (listen_fd >= 0) {
        close(listen_fd);
        listen_fd = -1;
    }
    pthread_join(listener_thread, NULL);
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
