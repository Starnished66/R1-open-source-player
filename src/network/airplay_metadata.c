#include "airplay_metadata.h"
#include "cover_decode.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef HOST_BUILD
  #include <poll.h>
#endif

#define LINE_MAX_LEN 512
#define ARTWORK_FILENAME_MAX 128
/* See airplay_bridge.c's own comment on the identical constant for why this
 * is 50ms rather than an original 200ms. */
#define POLL_INTERVAL_MS 50

typedef enum {
    META_STOPPED = 0,
    META_RUNNING,
    META_STOPPING,
} meta_state_t;

static pthread_mutex_t meta_mutex = PTHREAD_MUTEX_INITIALIZER;
static meta_state_t meta_state = META_STOPPED;
static bool restart_requested = false;
static volatile bool stop_requested = false;

static bool pending_valid = false;
static airplay_metadata_update_t pending;

/* Bumped by airplay_metadata_invalidate() (natural PCM session end, called
 * from airplay_bridge.c) and by airplay_metadata_stop() (full teardown) --
 * lets a decode already in flight when either happens recognize, once it
 * finishes, that the session it was decoding for is no longer current.
 * Artwork decoding (cover_decode_to_rgb565() inside publish_pending()
 * below) is synchronous and can take long enough for either of those to
 * complete entirely while one is still running on this thread -- without
 * this check, that stale result would still get published afterward and
 * could be consumed as if it belonged to whatever (if anything) replaced
 * it. Read by publish_pending() fresh at the START of each record it
 * handles, NOT captured once per run_session() call: this metadata reader's
 * own connection cycle (its outer while loop reopening after each writer
 * EOF) runs continuously across MANY PCM-bridge sessions without ever
 * returning, so a single stale capture taken once at the top of run_session()
 * would permanently block every later session's updates the first time any
 * one session ever ended. */
static unsigned int meta_generation = 0;

static void free_pending_locked(void) {
    free(pending.cover_pixels);
    memset(&pending, 0, sizeof(pending));
    pending_valid = false;
}

static void publish_pending(char * title, char * artist, char * album, char * artwork_file) {
    unsigned int my_generation;
    pthread_mutex_lock(&meta_mutex);
    my_generation = meta_generation;
    pthread_mutex_unlock(&meta_mutex);

    airplay_metadata_update_t upd = {0};
    snprintf(upd.title, sizeof(upd.title), "%s", title);
    snprintf(upd.artist, sizeof(upd.artist), "%s", artist);
    snprintf(upd.album, sizeof(upd.album), "%s", album);

    if (artwork_file[0]) {
        char path[sizeof(AIRPLAY_NOW_PLAYING_PATH) + ARTWORK_FILENAME_MAX + 8];
        /* now_playing and the cover file are siblings inside the same "-M"
         * meta-dir -- reuse AIRPLAY_NOW_PLAYING_PATH's own directory rather
         * than hardcoding "/tmp" a second time. */
        const char * dir_end = strrchr(AIRPLAY_NOW_PLAYING_PATH, '/');
        int dir_len = dir_end ? (int) (dir_end - AIRPLAY_NOW_PLAYING_PATH) : 0;
        snprintf(path, sizeof(path), "%.*s/%s", dir_len, AIRPLAY_NOW_PLAYING_PATH, artwork_file);

        FILE * f = fopen(path, "rb");
        if (f) {
            struct stat st;
            if (fstat(fileno(f), &st) == 0 && st.st_size > 0 && st.st_size < 16 * 1024 * 1024) {
                uint8_t * data = malloc((size_t) st.st_size);
                if (data && fread(data, 1, (size_t) st.st_size, f) == (size_t) st.st_size) {
                    uint16_t * pixels = NULL;
                    if (cover_decode_to_rgb565(data, (uint32_t) st.st_size, AIRPLAY_COVER_WIDTH,
                                                AIRPLAY_COVER_HEIGHT, &pixels)) {
                        upd.has_cover = true;
                        upd.cover_pixels = pixels;
                    }
                }
                free(data);
            }
            fclose(f);
        }
        /* A missing/unreadable/undecodable artwork file just means no cover
         * this update (upd.has_cover stays false) -- title/artist/album are
         * still worth publishing on their own. */
    }

    pthread_mutex_lock(&meta_mutex);
    if (my_generation != meta_generation) {
        /* A stop() (and possibly an already-started new session) happened
         * while this update was being decoded -- it belongs to a session
         * that's no longer current. Discard rather than publish, or the
         * new session's overlay could briefly (or, if it never gets its
         * own update, indefinitely) show this stale one instead. */
        pthread_mutex_unlock(&meta_mutex);
        free(upd.cover_pixels);
        return;
    }
    if (pending_valid) {
        /* GUI hasn't consumed the previous update yet (e.g. several rapid
         * track changes) -- free it rather than leak, same "newest wins"
         * policy as every other single-slot pending-result pattern in this
         * codebase. */
        free(pending.cover_pixels);
    }
    pending = upd;
    pending_valid = true;
    pthread_mutex_unlock(&meta_mutex);
}

#ifndef HOST_BUILD
static void run_session(void) {
    char title[AIRPLAY_META_TITLE_MAX] = "";
    char artist[AIRPLAY_META_ARTIST_MAX] = "";
    char album[AIRPLAY_META_ALBUM_MAX] = "";
    char artwork_file[ARTWORK_FILENAME_MAX] = "";
    bool have_any_field = false;

    char line[LINE_MAX_LEN];
    size_t line_len = 0;

    /* Same outer/inner loop shape as airplay_bridge.c's own reader thread --
     * one outer iteration per shairport connection/writer, re-opening fresh
     * after every EOF (writer closed) so a later reconnect is picked up
     * without needing this whole module restarted. */
    while (!stop_requested) {
        int fd = open(AIRPLAY_NOW_PLAYING_PATH, O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            if (stop_requested) break;
            usleep(POLL_INTERVAL_MS * 1000);
            continue;
        }

        line_len = 0;
        have_any_field = false;
        title[0] = artist[0] = album[0] = artwork_file[0] = '\0';

        while (!stop_requested) {
            struct pollfd pfd = { .fd = fd, .events = POLLIN };
            int pr = poll(&pfd, 1, POLL_INTERVAL_MS);
            if (pr <= 0) continue; /* timeout or poll() error -- recheck stop_requested */

            char c;
            ssize_t n = read(fd, &c, 1);
            if (n < 0) {
                if (errno == EAGAIN) continue;
                break; /* genuine read error -- close and retry fresh below */
            }
            if (n == 0) break; /* every writer closed -- shairport session ended */

            if (c == '\n') {
                line[line_len] = '\0';
                if (line_len == 0) {
                    /* Blank line -- record complete. */
                    if (have_any_field) {
                        publish_pending(title, artist, album, artwork_file);
                        have_any_field = false;
                        title[0] = artist[0] = album[0] = artwork_file[0] = '\0';
                    }
                } else {
                    char * eq = strchr(line, '=');
                    if (eq) {
                        *eq = '\0';
                        const char * key = line;
                        const char * val = eq + 1;
                        if (strcmp(key, "title") == 0) { snprintf(title, sizeof(title), "%s", val); have_any_field = true; }
                        else if (strcmp(key, "artist") == 0) { snprintf(artist, sizeof(artist), "%s", val); have_any_field = true; }
                        else if (strcmp(key, "album") == 0) { snprintf(album, sizeof(album), "%s", val); have_any_field = true; }
                        else if (strcmp(key, "artwork") == 0) { snprintf(artwork_file, sizeof(artwork_file), "%s", val); }
                        /* genre/comment/other keys: ignored, not surfaced anywhere in this app's UI. */
                    }
                }
                line_len = 0;
            } else if (line_len + 1 < sizeof(line)) {
                line[line_len++] = c;
            }
            /* A pathologically long line just gets truncated at LINE_MAX_LEN
             * rather than growing unbounded -- shairport's own fields are
             * short tag values, never anywhere near this. */
        }

        close(fd);
        if (stop_requested) break;
        /* Otherwise: writer disconnected normally -- loop back and wait for the next one. */
    }
}

static void * metadata_thread_func(void * arg) {
    (void) arg;

    /* Restart-in-place loop -- same reasoning and same bug fixed as
     * airplay_bridge.c's own bridge_thread_func(): airplay_metadata_start()
     * sets restart_requested instead of spawning a second thread when
     * called while this one is still in META_STOPPING, so a rapid
     * off-then-on can never leave a running session with no reader. */
    for (;;) {
        run_session();

        pthread_mutex_lock(&meta_mutex);
        if (restart_requested) {
            restart_requested = false;
            stop_requested = false;
            pthread_mutex_unlock(&meta_mutex);
            continue;
        }
        meta_state = META_STOPPED;
        pthread_mutex_unlock(&meta_mutex);
        break;
    }

    return NULL;
}
#endif

bool airplay_metadata_start(void) {
#ifndef HOST_BUILD
    pthread_mutex_lock(&meta_mutex);
    if (meta_state == META_RUNNING) {
        pthread_mutex_unlock(&meta_mutex);
        return true;
    }
    if (meta_state == META_STOPPING) {
        restart_requested = true;
        pthread_mutex_unlock(&meta_mutex);
        return true;
    }
    meta_state = META_RUNNING;
    stop_requested = false;
    restart_requested = false;
    pthread_mutex_unlock(&meta_mutex);

    pthread_t thread;
    if (pthread_create(&thread, NULL, metadata_thread_func, NULL) != 0) {
        pthread_mutex_lock(&meta_mutex);
        meta_state = META_STOPPED;
        pthread_mutex_unlock(&meta_mutex);
        fprintf(stderr, "airplay_metadata: pthread_create failed -- no track info during this AirPlay session\n");
        return false;
    }
    pthread_detach(thread);
    return true;
#else
    return true;
#endif
}

void airplay_metadata_stop(void) {
#ifndef HOST_BUILD
    pthread_mutex_lock(&meta_mutex);
    if (meta_state != META_RUNNING) {
        pthread_mutex_unlock(&meta_mutex);
        return;
    }
    meta_state = META_STOPPING;
    /* Drop whatever hasn't been consumed yet, and bump meta_generation so a
     * decode already in flight on the thread (started before this stop, not
     * yet at its own pthread_mutex_lock() in publish_pending()) is
     * recognized as stale once it does finish and gets discarded there
     * instead of published -- see meta_generation's own comment. Together
     * these mean a quick AirPlay off-then-on, or a kill-and-respawn via
     * airplay_control_disconnect_active_stream(), can never surface the
     * previous session's leftover title/art on the new one, whether it was
     * already sitting in `pending` or still being decoded when this ran. */
    free_pending_locked();
    meta_generation++;
    pthread_mutex_unlock(&meta_mutex);

    stop_requested = true;
    /* Fire-and-forget, same reasoning as airplay_bridge_stop() -- every
     * caller of airplay_control_stop() runs on the UI thread and none need
     * a synchronous exit guarantee. */
#endif
}

void airplay_metadata_invalidate(void) {
    /* Called by airplay_bridge.c the instant a PCM streaming session ends
     * on its own (phone disconnect, error) -- NOT just on a full
     * airplay_metadata_stop(). This module's own reader keeps running
     * continuously across many such PCM sessions without ever stopping
     * itself (its outer connection-retry loop has no notion of the
     * bridge's session boundaries at all), so without this, a decode still
     * in flight for the session that just ended would still get published
     * as if it were current, and a later reconnect could start out showing
     * it before its own metadata arrives. Same effect as the stop() half of
     * this (bump generation, drop whatever's already pending) without
     * actually stopping the reader thread itself -- AirPlay stays
     * discoverable/listening across the session boundary, only the stale
     * metadata is cleared. */
    pthread_mutex_lock(&meta_mutex);
    meta_generation++;
    free_pending_locked();
    pthread_mutex_unlock(&meta_mutex);
}

bool airplay_metadata_consume_update(airplay_metadata_update_t * out) {
    pthread_mutex_lock(&meta_mutex);
    if (!pending_valid) {
        pthread_mutex_unlock(&meta_mutex);
        return false;
    }
    *out = pending;
    pending_valid = false;
    memset(&pending, 0, sizeof(pending)); /* ownership of the pixel buffer just passed to *out -- don't also keep it here */
    pthread_mutex_unlock(&meta_mutex);
    return true;
}
