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

/* Unix domain socket where dmrd expects its playback companion to listen. */
#define DMR_STREAMER_SOCKET_PATH "/data/dmr_streamer"

/* Temporary download directory and file path on the SD card. Uses a dot-prefixed
 * directory so recursive library scans ignore it. */
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

/* Accumulated from set_uri: and set_meta: commands before play@ triggers
 * download. Guarded by dlna_mutex. */
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
static char last_produced_path[512] = {0}; /* Superseded on the next successful download, then removed */

/* Guarded by dlna_mutex. Updated via dlna_control_notify_status(). */
static bool status_playing = false;
static bool status_paused = false;

/* Optimistic playback state flag set upon receiving play@ to immediately report
 * PLAYING to controller queries while the track downloads in the background.
 * Cleared once real playback starts or stop is requested. */
static bool cast_intent_playing = false;

typedef struct {
    char uri[2048];
    char title[256];
    char artist[256];
    char album[256];
} dlna_download_request_t;

/* Maps supported audio Content-Type MIME strings to file extensions recognized
 * by decoder_open(). Returns NULL for unrecognized types. */
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
    if (strcasecmp(content_type, "audio/opus") == 0) return ".opus";
    /* Generic Ogg-container MIME types map to .ogg for Vorbis decoding. */
    if (strcasecmp(content_type, "audio/ogg") == 0 || strcasecmp(content_type, "application/ogg") == 0) return ".ogg";
    return NULL;
}

static void * dlna_download_thread_func(void * arg) {
    dlna_download_request_t * req = (dlna_download_request_t *) arg;

    mkdir(DLNA_TEMP_DOWNLOAD_DIR, 0755); /* Ignore EEXIST */

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

    /* Target path with extension corresponding to the detected Content-Type. */
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
    track_ready = true; /* Written last after path and metadata are updated */
    pthread_mutex_unlock(&dlna_mutex);

    free(req);
    return NULL;
}

static void handle_play(void) {
    dlna_download_request_t * req = malloc(sizeof(*req));
    if (!req) return;
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

    /* Run download in a detached thread to prevent blocking socket polling. */
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
        /* Reply to get_volume with nominal full volume. */
        const char * reply = "100";
        write(cfd, reply, strlen(reply));
    } else if ((size_t) n >= 8 && memcmp(buf, "get_mute", 8) == 0) {
        const char * reply = "0";
        write(cfd, reply, strlen(reply));
    } else if ((size_t) n >= 6 && memcmp(buf, "state@", 6) == 0) {
        /* Report transport state (PLAYING, PAUSED_PLAYBACK, or STOPPED). */
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
        stop_requested = true; /* Consumed by LVGL/main thread */
    }
    /* Other commands are ignored without reply. */

    close(cfd);
}

/* Timeout for accept polling so listener_thread_func periodically checks the
 * running flag and terminates cleanly on stop. */
#define ACCEPT_POLL_TIMEOUT_MS 200

static void * listener_thread_func(void * arg) {
    (void) arg;
    while (running) {
        struct pollfd pfd = { .fd = listen_fd, .events = POLLIN, .revents = 0 };
        int pr = poll(&pfd, 1, ACCEPT_POLL_TIMEOUT_MS);
        if (pr <= 0) continue; /* Timeout or interrupted -- re-check running */
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
    chmod(DMR_STREAMER_SOCKET_PATH, 0777); /* Allow dmrd process to connect */

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

    /* Wait for listener thread to exit after noticing running = false. */
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
        /* Real playback started; clear optimistic intent flag. */
        cast_intent_playing = false;
    }
    pthread_mutex_unlock(&dlna_mutex);
}
