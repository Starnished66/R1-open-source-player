#include "remote_control.h"
#include "remote_control_mdns.h"
#include "remote_control_webapp.h"
#include "metadata.h"
#include "metadata_db.h"
#include "playlist_files.h"
#include "settings.h"
#include "audio.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/time.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* Port 8899 -- verified free on a real device; 4399 is already used by
 * Import via Wi-Fi's thttpd. */
#define REMOTE_CONTROL_PORT 8899

/* Same definition as gui.c's own MUSIC_ROOT_DIR/PLAYLISTS_DIR, duplicated
 * since gui.c doesn't expose either publicly. Must stay in sync. */
#ifdef HOST_BUILD
#define MUSIC_ROOT_DIR "./music"
#else
#define MUSIC_ROOT_DIR "/data/mnt/sd_0"
#endif
#define PLAYLISTS_DIR MUSIC_ROOT_DIR "/Playlists"

static pthread_mutex_t status_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool status_playing = false;
static bool status_paused = false;
static char status_title[256] = {0};
static char status_artist[256] = {0};
static char status_album[256] = {0};
static char status_path[512] = {0};
static int status_position_seconds = 0;
static int status_duration_seconds = 0;
static float status_volume = 0;
static int status_play_mode = 0;
/* Source format is copied into this API snapshot before status_mutex is
 * acquired. Never call into audio.c while holding status_mutex: audio and UI
 * paths may acquire their own locks in the opposite order. */
static char status_codec[32] = {0};
static unsigned int status_source_bit_depth = 0;
static unsigned int status_source_sample_rate = 0;

static const char * remote_codec_name(audio_codec_t codec) {
    switch (codec) {
        case AUDIO_CODEC_FLAC: return "FLAC";
        case AUDIO_CODEC_MP3: return "MP3";
        case AUDIO_CODEC_PCM: return "PCM";
        case AUDIO_CODEC_DSD: return "DSD";
        case AUDIO_CODEC_AAC: return "AAC";
        case AUDIO_CODEC_ALAC: return "ALAC";
        case AUDIO_CODEC_APE: return "APE";
        case AUDIO_CODEC_WMA: return "WMA";
        case AUDIO_CODEC_OPUS: return "Opus";
        case AUDIO_CODEC_VORBIS: return "Vorbis";
        case AUDIO_CODEC_UNKNOWN: break;
    }
    return "";
}

/* Playback requests -- edge-triggered flags consumed by update_timer_cb.
 * Guarded by status_mutex. */
static bool request_play_pause = false;
static bool request_next = false;
static bool request_prev = false;
static bool request_mode_cycle = false;
static bool request_has_play_mode = false;
static int request_play_mode = 0;
static bool request_has_seek = false;
static int request_seek_seconds = 0;
static bool request_has_volume = false;
static int request_volume_percent = 0;
static bool request_has_play_index = false;
static int64_t request_play_index = 0;
static bool request_has_queue_index = false;
static int64_t request_queue_index = 0;
static bool request_has_queue_remove = false;
static int request_queue_remove_offset = 0;
static uint64_t request_queue_remove_revision = 0;
static bool request_queue_clear = false;
static uint64_t request_queue_clear_revision = 0;
/* Scope filters echoed back by /api/playback/play to build the queue within
 * an album, artist, album-artist, or playlist view. Empty string means entire library. */
static char request_play_playlist_name[128] = "";
static char request_play_artist_filter[128] = "";
static char request_play_album_artist_filter[128] = "";
static char request_play_album_filter[128] = "";

/* Queue of song IDs (metadata_db song_row_t.id). Guarded by status_mutex. */
static int64_t * queue_song_ids = NULL;
static int queue_song_id_count = 0;
static uint64_t queue_snapshot_revision = 0;

static atomic_bool running = false;
static int listen_fd = -1;
static pthread_t listener_thread;
static pthread_mutex_t listener_mutex = PTHREAD_MUTEX_INITIALIZER;
static int active_client_fd = -1;

/* The PIN is shared by Wi-Fi and RFCOMM because both transports carry the
 * same HTTP request format. Keep reads and UI writes serialized. */
static pthread_mutex_t pin_mutex = PTHREAD_MUTEX_INITIALIZER;
static unsigned int pin_failures = 0;
static char pin_failed_candidates[5][REMOTE_CONTROL_PIN_MAX_LENGTH + 1];
static time_t pin_failure_window = 0;
static time_t pin_blocked_until = 0;

#define REMOTE_CONTROL_PIN_MIN_LENGTH 4

static bool remote_control_pin_is_valid(const char * pin) {
    if (!pin) return false;
    size_t len = strlen(pin);
    if (len < REMOTE_CONTROL_PIN_MIN_LENGTH || len > REMOTE_CONTROL_PIN_MAX_LENGTH) return false;
    for (size_t i = 0; i < len; i++) if (pin[i] < '0' || pin[i] > '9') return false;
    return true;
}

/* The fixed PIN earlier builds seeded as a placeholder for Android app
 * testing. Every device shipped that same value, so a saved copy is treated
 * as unset and replaced with a random PIN once. */
#define REMOTE_CONTROL_LEGACY_PLACEHOLDER_PIN "0000"
#define REMOTE_CONTROL_GENERATED_PIN_DIGITS 6

/* Fills out[] with a uniformly random REMOTE_CONTROL_GENERATED_PIN_DIGITS
 * PIN from the kernel CSPRNG. Rejection sampling keeps every PIN equally
 * likely (a plain modulo of a 32-bit value would favor low PINs). */
static bool remote_control_random_pin(char * out, size_t out_size) {
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    const uint32_t range = 1000000u;
    const uint32_t limit = UINT32_MAX - (UINT32_MAX % range);
    bool ok = false;
    for (int attempt = 0; attempt < 16 && !ok; attempt++) {
        uint32_t value = 0;
        size_t got = 0;
        while (got < sizeof(value)) {
            ssize_t n = read(fd, (uint8_t *) &value + got, sizeof(value) - got);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            got += (size_t) n;
        }
        if (got != sizeof(value)) break;
        if (value >= limit) continue;
        snprintf(out, out_size, "%0*u", REMOTE_CONTROL_GENERATED_PIN_DIGITS, (unsigned) (value % range));
        ok = true;
    }
    close(fd);
    return ok;
}

/* Caller holds pin_mutex. A new PIN invalidates everything learned about the
 * old one, including an active lockout. */
static void remote_control_store_pin_locked(const char * pin) {
    snprintf(current_settings.remote_control_pin, sizeof(current_settings.remote_control_pin), "%s", pin);
    pin_failures = 0;
    memset(pin_failed_candidates, 0, sizeof(pin_failed_candidates));
    pin_failure_window = 0;
    pin_blocked_until = 0;
    settings_save_async(&current_settings);
}

/* Generates the PIN once on first use and reuses it after that. A saved
 * 4-12 digit PIN is kept, except the old shared placeholder. Returns false
 * only when no PIN exists and the CSPRNG cannot be read; callers then refuse
 * to serve rather than fall back to a guessable value. */
static bool remote_control_ensure_pin(void) {
    pthread_mutex_lock(&pin_mutex);
    const char * saved = current_settings.remote_control_pin;
    if (remote_control_pin_is_valid(saved) && strcmp(saved, REMOTE_CONTROL_LEGACY_PLACEHOLDER_PIN) != 0) {
        pthread_mutex_unlock(&pin_mutex);
        return true;
    }

    char pin[REMOTE_CONTROL_PIN_MAX_LENGTH + 1];
    bool ok = remote_control_random_pin(pin, sizeof(pin));
    if (ok) remote_control_store_pin_locked(pin);
    pthread_mutex_unlock(&pin_mutex);
    return ok;
}

bool remote_control_generate_new_pin(void) {
    char pin[REMOTE_CONTROL_PIN_MAX_LENGTH + 1];
    pthread_mutex_lock(&pin_mutex);
    bool ok = remote_control_random_pin(pin, sizeof(pin));
    /* Never "regenerate" to the same value, which would look like a no-op. */
    if (ok && strcmp(pin, current_settings.remote_control_pin) == 0)
        ok = remote_control_random_pin(pin, sizeof(pin));
    if (ok) remote_control_store_pin_locked(pin);
    pthread_mutex_unlock(&pin_mutex);
    return ok;
}

void remote_control_get_pin(char * out_pin, size_t out_pin_size) {
    if (!out_pin || out_pin_size == 0) return;
    /* The UI shows the PIN before the server is ever enabled, so create it
     * here too rather than displaying an empty or placeholder value. */
    remote_control_ensure_pin();
    pthread_mutex_lock(&pin_mutex);
    snprintf(out_pin, out_pin_size, "%s", current_settings.remote_control_pin);
    pthread_mutex_unlock(&pin_mutex);
}

typedef enum {
    REMOTE_CONTROL_PIN_INVALID = 0,
    REMOTE_CONTROL_PIN_VALID = 1,
    REMOTE_CONTROL_PIN_RATE_LIMITED = 2
} remote_control_pin_result_t;

static remote_control_pin_result_t remote_control_pin_check(const char * candidate) {
    bool matches = false;
    time_t now = time(NULL);
    pthread_mutex_lock(&pin_mutex);
    if (now < pin_blocked_until) {
        pthread_mutex_unlock(&pin_mutex);
        return REMOTE_CONTROL_PIN_RATE_LIMITED;
    }

    const char * expected = current_settings.remote_control_pin;
    size_t candidate_len = candidate ? strlen(candidate) : 0;
    size_t expected_len = strlen(expected);
    unsigned int diff = (unsigned int) (candidate_len ^ expected_len);
    size_t compare_len = candidate_len > expected_len ? candidate_len : expected_len;
    for (size_t i = 0; i < compare_len; i++) {
        unsigned char a = i < candidate_len ? (unsigned char) candidate[i] : 0;
        unsigned char b = i < expected_len ? (unsigned char) expected[i] : 0;
        diff |= (unsigned int) (a ^ b);
    }
    matches = diff == 0 && remote_control_pin_is_valid(candidate) &&
              remote_control_pin_is_valid(expected);
    if (matches) {
        pin_failures = 0;
        memset(pin_failed_candidates, 0, sizeof(pin_failed_candidates));
        pin_failure_window = 0;
    } else {
        if (now - pin_failure_window >= 60 || pin_failure_window == 0) {
            pin_failure_window = now;
            pin_failures = 0;
            memset(pin_failed_candidates, 0, sizeof(pin_failed_candidates));
        }
        bool already_counted = false;
        for (unsigned int i = 0; i < pin_failures; i++) {
            if (strcmp(pin_failed_candidates[i], candidate) == 0) {
                already_counted = true;
                break;
            }
        }
        if (!already_counted && pin_failures < 5) {
            snprintf(pin_failed_candidates[pin_failures], sizeof(pin_failed_candidates[pin_failures]), "%s", candidate);
            pin_failures++;
        }
        if (pin_failures >= 5) {
            pin_blocked_until = now + 60;
            pin_failures = 0;
            memset(pin_failed_candidates, 0, sizeof(pin_failed_candidates));
            pin_failure_window = 0;
        }
    }
    pthread_mutex_unlock(&pin_mutex);
    return matches ? REMOTE_CONTROL_PIN_VALID : REMOTE_CONTROL_PIN_INVALID;
}

void remote_control_notify_status(bool playing, bool paused, const char * title, const char * artist,
                                   const char * album, const char * path, int position_seconds,
                                   int duration_seconds, float volume, int play_mode) {
    /* Take the audio snapshot before status_mutex. Match its copied path
     * against this now-playing snapshot so a track transition cannot publish
     * the previous decoder's format beside the new track's metadata. */
    audio_current_format_info_t format = {0};
    bool format_matches_track = path && path[0] &&
        audio_get_current_format_info(&format) && format.valid && !format.is_stream &&
        strcmp(format.path, path) == 0;

    pthread_mutex_lock(&status_mutex);
    status_playing = playing;
    status_paused = paused;
    snprintf(status_title, sizeof(status_title), "%s", title ? title : "");
    snprintf(status_artist, sizeof(status_artist), "%s", artist ? artist : "");
    snprintf(status_album, sizeof(status_album), "%s", album ? album : "");
    snprintf(status_path, sizeof(status_path), "%s", path ? path : "");
    status_position_seconds = position_seconds;
    status_duration_seconds = duration_seconds;
    status_volume = volume;
    status_play_mode = play_mode;
    snprintf(status_codec, sizeof(status_codec), "%s",
             format_matches_track ? remote_codec_name(format.codec) : "");
    status_source_bit_depth = format_matches_track ? format.source_bit_depth : 0;
    status_source_sample_rate = format_matches_track ? format.source_sample_rate : 0;
    pthread_mutex_unlock(&status_mutex);
}

bool remote_control_consume_play_pause(void) {
    pthread_mutex_lock(&status_mutex);
    bool result = request_play_pause;
    request_play_pause = false;
    pthread_mutex_unlock(&status_mutex);
    return result;
}

bool remote_control_consume_next(void) {
    pthread_mutex_lock(&status_mutex);
    bool result = request_next;
    request_next = false;
    pthread_mutex_unlock(&status_mutex);
    return result;
}

bool remote_control_consume_prev(void) {
    pthread_mutex_lock(&status_mutex);
    bool result = request_prev;
    request_prev = false;
    pthread_mutex_unlock(&status_mutex);
    return result;
}

bool remote_control_consume_mode_cycle(void) {
    pthread_mutex_lock(&status_mutex);
    bool result = request_mode_cycle;
    request_mode_cycle = false;
    pthread_mutex_unlock(&status_mutex);
    return result;
}

bool remote_control_consume_play_mode(int * out_mode) {
    pthread_mutex_lock(&status_mutex);
    bool result = request_has_play_mode;
    if (result) {
        request_has_play_mode = false;
        if (out_mode) *out_mode = request_play_mode;
    }
    pthread_mutex_unlock(&status_mutex);
    return result;
}

bool remote_control_consume_seek(int * out_seconds) {
    pthread_mutex_lock(&status_mutex);
    bool result = request_has_seek;
    if (result) {
        request_has_seek = false;
        *out_seconds = request_seek_seconds;
    }
    pthread_mutex_unlock(&status_mutex);
    return result;
}

bool remote_control_consume_volume(int * out_percent) {
    pthread_mutex_lock(&status_mutex);
    bool result = request_has_volume;
    if (result) {
        request_has_volume = false;
        *out_percent = request_volume_percent;
    }
    pthread_mutex_unlock(&status_mutex);
    return result;
}

bool remote_control_consume_queue_index(int64_t * out_index) {
    pthread_mutex_lock(&status_mutex);
    bool result = request_has_queue_index;
    if (result) {
        request_has_queue_index = false;
        *out_index = request_queue_index;
    }
    pthread_mutex_unlock(&status_mutex);
    return result;
}

bool remote_control_consume_queue_remove(int * out_offset, uint64_t * out_revision) {
    pthread_mutex_lock(&status_mutex);
    bool result = request_has_queue_remove;
    if (result) {
        request_has_queue_remove = false;
        *out_offset = request_queue_remove_offset;
        *out_revision = request_queue_remove_revision;
    }
    pthread_mutex_unlock(&status_mutex);
    return result;
}

bool remote_control_consume_queue_clear(uint64_t * out_revision) {
    pthread_mutex_lock(&status_mutex);
    bool result = request_queue_clear;
    request_queue_clear = false;
    if (result) *out_revision = request_queue_clear_revision;
    pthread_mutex_unlock(&status_mutex);
    return result;
}

void remote_control_sync_queue(const char * const * paths, int count, uint64_t revision) {
    /* metadata_db_get_songs_by_paths() takes its own METADATA_DB_GUARD lock
     * internally -- must not be called while already holding status_mutex,
     * or a UI-thread caller blocked on METADATA_DB_GUARD (e.g. a rescan)
     * while THIS thread holds status_mutex waiting on that same lock could
     * deadlock against gui.c's own status_mutex use elsewhere. Resolved
     * before, not during, the locked section below. Batched (one prepared
     * statement reused across all `count` lookups) rather than a fresh
     * metadata_db_get_song_by_path() prepare/finalize per song -- see that
     * function's own doc comment. */
    int64_t * ids = count > 0 ? malloc(sizeof(int64_t) * (size_t) count) : NULL;
    int id_count = 0;
    if (ids) {
        song_row_t * rows = malloc(sizeof(song_row_t) * (size_t) count);
        if (rows) {
            metadata_db_get_songs_by_paths(paths, count, rows);
            for (int i = 0; i < count; i++) ids[id_count++] = rows[i].id;
            free(rows);
        }
    }

    pthread_mutex_lock(&status_mutex);
    free(queue_song_ids);
    queue_song_ids = ids;
    queue_song_id_count = id_count;
    queue_snapshot_revision = revision;
    pthread_mutex_unlock(&status_mutex);
}

bool remote_control_consume_play_index(int64_t * out_index, char * out_playlist, size_t playlist_size,
                                        char * out_artist, size_t artist_size, char * out_album_artist,
                                        size_t album_artist_size, char * out_album, size_t album_size) {
    pthread_mutex_lock(&status_mutex);
    bool result = request_has_play_index;
    if (result) {
        request_has_play_index = false;
        *out_index = request_play_index;
        snprintf(out_playlist, playlist_size, "%s", request_play_playlist_name);
        snprintf(out_artist, artist_size, "%s", request_play_artist_filter);
        snprintf(out_album_artist, album_artist_size, "%s", request_play_album_artist_filter);
        snprintf(out_album, album_size, "%s", request_play_album_filter);
    }
    pthread_mutex_unlock(&status_mutex);
    return result;
}

/* Escape JSON string delimiters and control bytes so metadata round-trips as
 * the same string when a client parses the response. */
static void json_escape_append(char * out, size_t out_size, const char * in) {
    size_t len = strlen(out);
    for (const char * p = in; *p && len + 2 < out_size; p++) {
        unsigned char c = (unsigned char) *p;
        if (c == '"' || c == '\\') {
            if (len + 3 >= out_size) break;
            out[len++] = '\\';
            out[len++] = (char) c;
        } else if (c < 0x20) {
            static const char hex[] = "0123456789abcdef";
            if (len + 6 >= out_size) break;
            out[len++] = '\\';
            out[len++] = 'u';
            out[len++] = '0';
            out[len++] = '0';
            out[len++] = hex[c >> 4];
            out[len++] = hex[c & 0x0f];
        } else {
            out[len++] = (char) c;
        }
    }
    out[len] = '\0';
}

typedef struct {
    char * data;
    size_t capacity;
    size_t length;
    bool truncated;
} json_builder_t;

static void json_builder_init(json_builder_t * b, char * data, size_t capacity) {
    b->data = data;
    b->capacity = capacity;
    b->length = 0;
    b->truncated = capacity == 0;
    if (capacity > 0) data[0] = '\0';
}

static bool json_builder_appendf(json_builder_t * b, const char * format, ...) {
    if (b->truncated || b->length >= b->capacity) return false;
    size_t old_length = b->length;
    va_list ap;
    va_start(ap, format);
    int written = vsnprintf(b->data + b->length, b->capacity - b->length, format, ap);
    va_end(ap);
    if (written < 0 || (size_t) written >= b->capacity - b->length) {
        b->length = old_length;
        b->data[old_length] = '\0';
        b->truncated = true;
        return false;
    }
    b->length += (size_t) written;
    return true;
}

/* Decodes application/x-www-form-urlencoded query-string bytes (%XX and
 * "+" for space; both %20 and "+" are accepted for space). Bounded,
 * truncates rather than overflowing if the decoded result wouldn't fit. */
static void url_decode(const char * in, char * out, size_t out_size) {
    size_t len = 0;
    while (*in && len + 1 < out_size) {
        if (*in == '%' && in[1] && in[2]) {
            char hex[3] = { in[1], in[2], '\0' };
            char * end;
            long v = strtol(hex, &end, 16);
            if (end == hex + 2) {
                out[len++] = (char) v;
                in += 3;
                continue;
            }
        }
        out[len++] = (*in == '+') ? ' ' : *in;
        in++;
    }
    out[len] = '\0';
}

static bool url_decode_checked(const char * in, char * out, size_t out_size) {
    size_t len = 0;
    if (!out || out_size == 0) return false;
    while (*in) {
        if (len + 1 >= out_size) { out[0] = '\0'; return false; }
        if (*in == '%') {
            if (!in[1] || !in[2]) { out[0] = '\0'; return false; }
            bool first_is_hex = (in[1] >= '0' && in[1] <= '9') || (in[1] >= 'a' && in[1] <= 'f') ||
                                (in[1] >= 'A' && in[1] <= 'F');
            bool second_is_hex = (in[2] >= '0' && in[2] <= '9') || (in[2] >= 'a' && in[2] <= 'f') ||
                                 (in[2] >= 'A' && in[2] <= 'F');
            if (!first_is_hex || !second_is_hex) { out[0] = '\0'; return false; }
            char hex[3] = { in[1], in[2], '\0' };
            char * end;
            long v = strtol(hex, &end, 16);
            if (end != hex + 2 || v == 0) { out[0] = '\0'; return false; }
            out[len++] = (char)v;
            in += 3;
            continue;
        }
        out[len++] = (*in == '+') ? ' ' : *in;
        in++;
    }
    out[len] = '\0';
    return true;
}

static bool query_param_present(const char * path, const char * key) {
    const char * q = strchr(path, '?');
    if (!q) return false;
    q++;
    size_t key_len = strlen(key);
    while (*q) {
        if (strncmp(q, key, key_len) == 0 && q[key_len] == '=') return true;
        const char * amp = strchr(q, '&');
        if (!amp) break;
        q = amp + 1;
    }
    return false;
}

/* Pulls a raw (still percent-encoded) string query parameter's value.
 * Returns false (leaving out untouched) if absent or too long for out. */
static bool query_param_str(const char * path, const char * key, char * out, size_t out_size) {
    const char * q = strchr(path, '?');
    if (!q) return false;
    q++;

    size_t key_len = strlen(key);
    while (*q) {
        if (strncmp(q, key, key_len) == 0 && q[key_len] == '=') {
            const char * val = q + key_len + 1;
            const char * amp = strchr(val, '&');
            size_t val_len = amp ? (size_t) (amp - val) : strlen(val);
            if (val_len >= out_size) return false;
            memcpy(out, val, val_len);
            out[val_len] = '\0';
            return true;
        }
        const char * amp = strchr(q, '&');
        if (!amp) break;
        q = amp + 1;
    }
    return false;
}

/* Caps both the page size for build_library_json() and, indirectly, the
 * response size. */
#define LIBRARY_JSON_MAX_LIMIT 100

static void build_library_json(const char * query, const char * artist_filter, const char * album_artist_filter,
                                const char * album_filter, const char * genre_filter, int offset, int limit,
                                char * out, size_t out_size) {
    if (limit <= 0 || limit > LIBRARY_JSON_MAX_LIMIT) limit = LIBRARY_JSON_MAX_LIMIT;
    if (offset < 0) offset = 0;

    /* Query filtered songs directly from metadata_db. "index" in the response
     * is the persistent song id (song_row_t.id). */
    int64_t total_matches = metadata_db_count_songs_filtered(query, artist_filter, album_artist_filter, album_filter, genre_filter);

    json_builder_t json;
    json_builder_init(&json, out, out_size);
    json_builder_appendf(&json, "{\"total\":%lld,\"songs\":[", (long long) total_matches);

    /* Allocate rows on the heap to keep stack usage bounded. */
    song_row_t * rows = malloc(sizeof(song_row_t) * LIBRARY_JSON_MAX_LIMIT);
    int n = rows ? metadata_db_get_songs_filtered_page(query, artist_filter, album_artist_filter, album_filter,
                                                         genre_filter, offset, limit, rows)
                 : 0;
    for (int i = 0; i < n && !json.truncated; i++) {
        char display_title[128], title_esc[300] = {0}, artist_esc[300] = {0};
        metadata_db_song_display_title(&rows[i], display_title, sizeof(display_title));
        json_escape_append(title_esc, sizeof(title_esc), display_title);
        json_escape_append(artist_esc, sizeof(artist_esc), rows[i].tags.artist);
        if (!json_builder_appendf(&json, "%s{\"index\":%lld,\"title\":\"%s\",\"artist\":\"%s\"}", i > 0 ? "," : "",
                                  (long long) rows[i].id, title_esc, artist_esc)) break;
    }
    json.truncated = false; /* a rejected item left the prior JSON intact */
    json_builder_appendf(&json, "]}");
    free(rows);
}

/* Renders group_row_t entries into a JSON array {"name":..., "count":..., "index":..., "album_artist":...}.
 * index is a representative first song id from group_row_t.first_song_id. */
static void render_name_counts_json(const group_row_t * groups, int count, const char * json_key, char * out,
                                     size_t out_size) {
    json_builder_t json;
    json_builder_init(&json, out, out_size);
    json_builder_appendf(&json, "{\"%s\":[", json_key);
    size_t response_capacity = json.capacity;
    for (int i = 0; i < count && !json.truncated && json.capacity - json.length >= 3; i++) {
        char name_esc[METADATA_DB_TEXT_MAX * 6] = {0}, album_artist_esc[300] = {0};
        const char * name = strcmp(json_key, "genres") == 0 ? groups[i].genre_name : groups[i].name;
        json_escape_append(name_esc, sizeof(name_esc), name);
        /* album_artist is populated for album groups. */
        json_escape_append(album_artist_esc, sizeof(album_artist_esc), groups[i].album_artist);
        /* Keep room for the closing bracket, brace, and trailing NUL even if
         * the next full row does not fit in the response buffer. */
        json.capacity = response_capacity >= 2 ? response_capacity - 2 : response_capacity;
        bool appended = json_builder_appendf(&json,
            "%s{\"name\":\"%s\",\"count\":%d,\"index\":%lld,\"album_artist\":\"%s\"}",
            i > 0 ? "," : "", name_esc, groups[i].song_count,
            (long long) groups[i].first_song_id, album_artist_esc);
        json.capacity = response_capacity;
        if (!appended) break;
    }
    json.truncated = false;
    json_builder_appendf(&json, "]}");
}

/* Upper bound for groups queries. */
#define GROUPS_JSON_MAX 2000

static void build_artists_json(char * out, size_t out_size) {
    group_row_t * groups = malloc(sizeof(group_row_t) * GROUPS_JSON_MAX);
    int n = groups ? metadata_db_get_groups_page(METADATA_DB_GROUP_ARTIST, 0, GROUPS_JSON_MAX, groups) : 0;
    render_name_counts_json(groups, n, "artists", out, out_size);
    free(groups);
}

static void build_album_artists_json(char * out, size_t out_size) {
    group_row_t * groups = malloc(sizeof(group_row_t) * GROUPS_JSON_MAX);
    int n = groups ? metadata_db_get_groups_page(METADATA_DB_GROUP_ALBUM_ARTIST, 0, GROUPS_JSON_MAX, groups) : 0;
    render_name_counts_json(groups, n, "artists", out, out_size);
    free(groups);
}

static void build_genres_json(char * out, size_t out_size) {
    group_row_t * groups = malloc(sizeof(group_row_t) * GROUPS_JSON_MAX);
    int n = groups ? metadata_db_get_groups_page(METADATA_DB_GROUP_GENRE, 0, GROUPS_JSON_MAX, groups) : 0;
    render_name_counts_json(groups, n, "genres", out, out_size);
    free(groups);
}

/* Build album groups matching artist_filter or album_artist_filter. */
static void build_albums_json(const char * artist_filter, const char * album_artist_filter, char * out,
                               size_t out_size) {
    const char * filter = artist_filter[0] != '\0' ? artist_filter : album_artist_filter;
    group_row_t * groups = malloc(sizeof(group_row_t) * GROUPS_JSON_MAX);
    int n = groups ? metadata_db_get_albums_page_filtered(filter, 0, GROUPS_JSON_MAX, groups) : 0;
    render_name_counts_json(groups, n, "albums", out, out_size);
    free(groups);
}

static void build_status_json(char * out, size_t out_size) {
    char title_esc[512] = {0}, artist_esc[512] = {0}, album_esc[512] = {0};
    pthread_mutex_lock(&status_mutex);
    json_escape_append(title_esc, sizeof(title_esc), status_title);
    json_escape_append(artist_esc, sizeof(artist_esc), status_artist);
    json_escape_append(album_esc, sizeof(album_esc), status_album);
    snprintf(out, out_size,
             "{\"playing\":%s,\"paused\":%s,\"title\":\"%s\",\"artist\":\"%s\",\"album\":\"%s\","
             "\"position_seconds\":%d,\"duration_seconds\":%d,\"volume\":%.2f,\"play_mode\":%d,"
             "\"codec\":\"%s\",\"source_bit_depth\":%u,\"source_sample_rate\":%u}",
             status_playing ? "true" : "false", status_paused ? "true" : "false", title_esc, artist_esc, album_esc,
             status_position_seconds, status_duration_seconds, (double) status_volume, status_play_mode,
             status_codec, status_source_bit_depth, status_source_sample_rate);
    pthread_mutex_unlock(&status_mutex);
}

/* The playlist `name` becomes a filesystem path component
 * (PLAYLISTS_DIR "/" name ".m3u", see playlist_files_create()) -- unlike
 * gui.c's own in-app equivalent (new_playlist_name_done_cb(), whose name
 * comes from a UI text field only the device's own user can type into),
 * this is reachable by authenticated remote clients, so it is validated
 * rather than trusted the way playlist_files_create()'s
 * own doc comment says its callers must. Rejects empty names, anything
 * containing a path separator, and anything containing ".." (defense in
 * depth against path traversal even though a bare ".." alone couldn't
 * escape PLAYLISTS_DIR without a "/" too). */
#define PLAYLIST_NAME_MAX_BYTES 127

static bool playlist_name_is_safe(const char * name) {
    if (!name || name[0] == '\0') return false;
    if (strchr(name, '/') || strchr(name, '\\')) return false;
    if (strstr(name, "..")) return false;
    return true;
}

static void build_playlists_json(char * out, size_t out_size) {
    char ** paths = NULL;
    int count = 0;
    playlist_files_scan(PLAYLISTS_DIR, &paths, &count);

    /* Build JSON array of playlists using json_builder_t with bounds checking. */
    json_builder_t json;
    json_builder_init(&json, out, out_size);
    json_builder_appendf(&json,
                            "{\"playlists\":[{\"name\":\"Favorites\",\"key\":\"@favorites\",\"internal\":true,\"writable\":false},"
                            "{\"name\":\"Most Played\",\"key\":\"@most_played\",\"internal\":true,\"writable\":false}");
    size_t playlists_dir_len = strlen(PLAYLISTS_DIR);
    for (int i = 0; i < count && !json.truncated; i++) {
        /* The API routes resolve user playlists as
         * PLAYLISTS_DIR/<key>.m3u. Advertise only files that exact lookup can
         * reach: direct children with the canonical lowercase extension. */
        if (strncmp(paths[i], PLAYLISTS_DIR, playlists_dir_len) != 0 ||
            paths[i][playlists_dir_len] != '/') continue;
        const char * base = paths[i] + playlists_dir_len + 1;
        if (strchr(base, '/') != NULL) continue;
        size_t base_len = strlen(base);
        if (base_len <= 4 || strcmp(base + base_len - 4, ".m3u") != 0) continue;

        size_t key_len = base_len - 4;
        char key[256] = {0};
        /* Playback context and playlist lookup use 128-byte decoded names. */
        if (key_len >= sizeof(key) || key_len > PLAYLIST_NAME_MAX_BYTES) continue;
        memcpy(key, base, key_len);
        if (!playlist_name_is_safe(key)) continue;

        char name_esc[1600] = {0}, key_esc[1600] = {0};
        json_escape_append(name_esc, sizeof(name_esc), base);
        json_escape_append(key_esc, sizeof(key_esc), key);
        if (!json_builder_appendf(&json, ",{\"name\":\"%s\",\"key\":\"%s\",\"internal\":false,\"writable\":true}",
                                  name_esc, key_esc)) break;
    }
    json.truncated = false;
    json_builder_appendf(&json, "]}");

    for (int i = 0; i < count; i++) free(paths[i]);
    free(paths);
}

static void build_queue_json(char * out, size_t out_size, int offset, int limit) {
    /* Snapshot queue_song_ids under status_mutex, then resolve IDs via
     * metadata_db_get_songs_by_ids() outside the lock. */
    pthread_mutex_lock(&status_mutex);
    int total = queue_song_id_count;
    uint64_t revision = queue_snapshot_revision;
    if (offset > total) offset = total;
    int count = total - offset;
    if (count > limit) count = limit;
    int64_t * ids = NULL;
    if (count > 0) {
        ids = malloc(sizeof(int64_t) * (size_t) count);
        if (ids) memcpy(ids, queue_song_ids + offset, sizeof(int64_t) * (size_t) count);
        else count = 0;
    }
    pthread_mutex_unlock(&status_mutex);

    song_row_t * rows = count > 0 ? malloc(sizeof(song_row_t) * (size_t) count) : NULL;
    if (count > 0 && !rows) count = 0;
    for (int i = 0; rows && i < count; i++) rows[i].id = -1;
    int resolved_count = 0;
    int64_t * resolved_ids = count > 0 ? malloc(sizeof(int64_t) * (size_t) count) : NULL;
    int * resolved_at = count > 0 ? malloc(sizeof(int) * (size_t) count) : NULL;
    if (resolved_ids && resolved_at) {
        for (int i = 0; i < count; i++) if (ids[i] >= 0) {
            resolved_ids[resolved_count] = ids[i];
            resolved_at[resolved_count++] = i;
        }
        if (resolved_count) {
            song_row_t * resolved = malloc(sizeof(song_row_t) * (size_t) resolved_count);
            if (resolved) {
                metadata_db_get_songs_by_ids(resolved_ids, resolved_count, resolved);
                for (int i = 0; i < resolved_count; i++) rows[resolved_at[i]] = resolved[i];
                free(resolved);
            }
        }
    }

    /* Build JSON array of queue songs using json_builder_t with bounds checking. */
    json_builder_t json;
    json_builder_init(&json, out, out_size);
    json_builder_appendf(&json, "{\"total\":%d,\"offset\":%d,\"revision\":%llu,\"songs\":[", total, offset,
                         (unsigned long long) revision);
    for (int i = 0; i < count && !json.truncated; i++) {
        if (ids[i] < 0 || !rows || rows[i].id == -1) {
            if (!json_builder_appendf(&json, "%s{\"offset\":%d,\"index\":-1,\"title\":\"Unknown track\",\"artist\":\"\"}", i ? "," : "", offset + i)) break;
            continue;
        }
        char display_title[128], title[512] = {0}, artist[512] = {0};
        metadata_db_song_display_title(&rows[i], display_title, sizeof(display_title));
        json_escape_append(title, sizeof(title), display_title);
        json_escape_append(artist, sizeof(artist), rows[i].tags.artist);
        if (!json_builder_appendf(&json, "%s{\"offset\":%d,\"index\":%lld,\"title\":\"%s\",\"artist\":\"%s\"}",
                                  i ? "," : "", offset + i, (long long) rows[i].id, title, artist)) break;
    }
    json.truncated = false;
    json_builder_appendf(&json, "]}");
    free(rows);
    free(ids);
    free(resolved_ids);
    free(resolved_at);
}


static void send_all(int fd, const char * data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = write(fd, data + sent, len - sent);
        if (n <= 0) return;
        sent += (size_t) n;
    }
}

/* Sends an HTTP response with no-cache headers. */
static void send_response(int fd, const char * status_line, const char * content_type, const char * body) {
    char header[320];
    int header_len = snprintf(header, sizeof(header),
                               "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                               "Cache-Control: no-store, no-cache, must-revalidate\r\nPragma: no-cache\r\n"
                               "Connection: close\r\n\r\n",
                               status_line, content_type, strlen(body));
    send_all(fd, header, (size_t) header_len);
    send_all(fd, body, strlen(body));
}

static void send_pin_rate_limited_response(int fd) {
    static const char body[] = "{\"error\":\"rate_limited\",\"code\":\"pin_rate_limited\",\"retryAfterSeconds\":60}";
    char header[320];
    int header_len = snprintf(header, sizeof(header),
                              "HTTP/1.1 429 Too Many Requests\r\nContent-Type: application/json\r\n"
                              "Content-Length: %zu\r\nRetry-After: 60\r\nConnection: close\r\n\r\n",
                              sizeof(body) - 1);
    send_all(fd, header, (size_t) header_len);
    send_all(fd, body, sizeof(body) - 1);
}

/* Pulls an integer query parameter from the request path. Returns false if
 * absent or invalid. */
static bool query_param_int(const char * path, const char * key, int * out_value) {
    const char * q = strchr(path, '?');
    if (!q) return false;
    q++;

    size_t key_len = strlen(key);
    while (*q) {
        if (strncmp(q, key, key_len) == 0 && q[key_len] == '=') {
            char * end;
            long v = strtol(q + key_len + 1, &end, 10);
            if (end == q + key_len + 1) return false; /* no digits at all */
            *out_value = (int) v;
            return true;
        }
        const char * amp = strchr(q, '&');
        if (!amp) break;
        q = amp + 1;
    }
    return false;
}

/* Pulls a 64-bit integer query parameter from the request path. */
static bool query_param_int64(const char * path, const char * key, int64_t * out_value) {
    const char * q = strchr(path, '?');
    if (!q) return false;
    q++;

    size_t key_len = strlen(key);
    while (*q) {
        if (strncmp(q, key, key_len) == 0 && q[key_len] == '=') {
            char * end;
            long long v = strtoll(q + key_len + 1, &end, 10);
            if (end == q + key_len + 1) return false; /* no digits at all */
            *out_value = (int64_t) v;
            return true;
        }
        const char * amp = strchr(q, '&');
        if (!amp) break;
        q = amp + 1;
    }
    return false;
}

/* Serves a song's embedded cover art as a raw image. Resolves the path from
 * song id or from the currently playing file if no index is provided. */
static bool resolve_art_source_path(bool have_index, int64_t index, char * out_path, size_t out_path_size) {
    if (have_index) {
        song_row_t row;
        if (!metadata_db_get_song_by_id(index, &row)) return false;
        snprintf(out_path, out_path_size, "%s", row.path);
        return true;
    }
    pthread_mutex_lock(&status_mutex);
    bool ok = false;
    if (status_path[0] != '\0') {
        snprintf(out_path, out_path_size, "%s", status_path);
        ok = true;
    }
    pthread_mutex_unlock(&status_mutex);
    return ok;
}

static void send_response_binary(int fd, const char * status_line, const char * content_type, const uint8_t * data,
                                  size_t len) {
    char header[320];
    int header_len = snprintf(header, sizeof(header),
                               "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                               "Cache-Control: no-store, no-cache, must-revalidate\r\nPragma: no-cache\r\n"
                               "Connection: close\r\n\r\n",
                               status_line, content_type, len);
    send_all(fd, header, (size_t) header_len);
    send_all(fd, (const char *) data, len);
}

/* Sniffs just enough of the embedded picture's own magic bytes to pick a
 * Content-Type -- metadata_read() hands back raw still-encoded bytes with
 * no separate format tag, and the two containers this app's own tag
 * readers ever extract art from (FLAC METADATA_BLOCK_PICTURE, MP3 ID3v2
 * APIC) are always JPEG or PNG in practice. Falls back to JPEG (the
 * overwhelmingly common case) rather than rejecting an unrecognized-but-
 * real image outright. */
static const char * sniff_image_content_type(const uint8_t * data, uint32_t size) {
    if (size >= 8 && data[0] == 0x89 && data[1] == 0x50 && data[2] == 0x4E && data[3] == 0x47) return "image/png";
    return "image/jpeg";
}

static void handle_art_request(int cfd, const char * path) {
    int64_t index = -1;
    bool have_index = query_param_int64(path, "index", &index);

    char song_path[600] = {0};
    if (!resolve_art_source_path(have_index, index, song_path, sizeof(song_path))) {
        send_response(cfd, "404 Not Found", "text/plain", "No track");
        return;
    }

    track_metadata_t meta;
    memset(&meta, 0, sizeof(meta));
    metadata_read(song_path, &meta);

    if (meta.picture_data && meta.picture_size > 0) {
        send_response_binary(cfd, "200 OK", sniff_image_content_type(meta.picture_data, meta.picture_size),
                              meta.picture_data, meta.picture_size);
    } else {
        send_response(cfd, "404 Not Found", "text/plain", "No art");
    }
    free(meta.picture_data);
    free(meta.lyrics); /* only needed picture_data here */
}

/* Same definition as gui.c's own assets.c THEME_ROOT -- duplicated for the
 * same reason MUSIC_ROOT_DIR/PLAYLISTS_DIR are (assets.c doesn't expose it,
 * and this file already has its own copy of MUSIC_ROOT_DIR/PLAYLISTS_DIR
 * for the same reason). Only the stock, always-present theme2/category/
 * icons are served here -- no THEME_OVERRIDE_ROOT fallback, since none of
 * the whitelisted names below need one. */
#ifdef HOST_BUILD
#define THEME_ROOT "assets/theme2/"
#else
#define THEME_ROOT "/usr/resource/litegui/theme2/"
#endif

/* Fixed whitelist, not an arbitrary caller-supplied filename. Use the same bundled submenu icons
 * as build_music_screen() in gui_library.c. */
static const struct {
    const char * name;
    const char * relative_path;
} CATEGORY_ICONS[] = {
    { "all", "submenu/all_songs.png" },
    { "artist", "submenu/artists.png" },
    { "album_artist", "submenu/album_artist.png" },
    { "genre", "submenu/playlists.png" },
};

static uint8_t * read_whole_file(const char * path, size_t * out_size) {
    FILE * f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long size = ftell(f);
    if (size < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }

    uint8_t * buf = malloc((size_t) size);
    if (!buf) { fclose(f); return NULL; }
    size_t read_bytes = fread(buf, 1, (size_t) size, f);
    fclose(f);
    if (read_bytes != (size_t) size) {
        free(buf);
        return NULL;
    }
    *out_size = (size_t) size;
    return buf;
}

static void handle_icon_request(int cfd, const char * path) {
    char name[32] = {0};
    if (!query_param_str(path, "name", name, sizeof(name))) {
        send_response(cfd, "400 Bad Request", "text/plain", "Missing name");
        return;
    }

    const char * relative = NULL;
    for (size_t i = 0; i < sizeof(CATEGORY_ICONS) / sizeof(CATEGORY_ICONS[0]); i++) {
        if (strcmp(CATEGORY_ICONS[i].name, name) == 0) {
            relative = CATEGORY_ICONS[i].relative_path;
            break;
        }
    }
    if (!relative) {
        send_response(cfd, "404 Not Found", "text/plain", "Unknown icon");
        return;
    }

    char full_path[320];
    snprintf(full_path, sizeof(full_path), THEME_ROOT "%s", relative);
    size_t size;
    uint8_t * data = read_whole_file(full_path, &size);
    if (!data) {
        send_response(cfd, "404 Not Found", "text/plain", "Icon not found");
        return;
    }
    send_response_binary(cfd, "200 OK", "image/png", data, size);
    free(data);
}

/* GET /api/playlists/songs?name=NAME -- one playlist's songs, in file
 * order, in the same {"index":N,"title":...,"artist":...} shape
 * /api/library uses so the phone UI can render both with the same row
 * code. Entries whose path no longer matches anything in the synced
 * library are silently skipped -- same behavior as gui.c's own
 * rebuild_playlist_m3u_group() (find_song_index_by_path() returning -1 just
 * drops that entry), not an error condition worth surfacing. */
static void handle_playlist_songs_request(int cfd, const char * path) {
    char raw_name[128] = {0}, name[128] = {0};
    if (!query_param_str(path, "name", raw_name, sizeof(raw_name))) {
        send_response(cfd, "400 Bad Request", "text/plain", "Bad Request");
        return;
    }
    url_decode(raw_name, name, sizeof(name));
    if (!playlist_name_is_safe(name)) {
        send_response(cfd, "400 Bad Request", "text/plain", "Bad Request");
        return;
    }

    char ** paths = NULL;
    int count = 0;
    bool loaded = false;
    if (strcmp(name, "@favorites") == 0 || strcmp(name, "Favorites") == 0) {
        metadata_db_load_favorite_songs(&paths, &count);
        loaded = true; /* an empty built-in playlist is valid */
    } else if (strcmp(name, "@most_played") == 0 || strcmp(name, "Most Played") == 0) {
        metadata_db_load_top_played_songs(20, &paths, &count);
        loaded = true;
    } else {
        char m3u_path[512];
        snprintf(m3u_path, sizeof(m3u_path), "%s/%s.m3u", PLAYLISTS_DIR, name);
        loaded = playlist_files_read(m3u_path, &paths, &count);
    }
    if (!loaded) {
        send_response(cfd, "404 Not Found", "text/plain", "Playlist not found");
        return;
    }

    char * json_buf = malloc(65536);
    if (!json_buf) {
        for (int i = 0; i < count; i++) free(paths[i]);
        free(paths);
        send_response(cfd, "500 Internal Server Error", "text/plain", "Out of memory");
        return;
    }

    /* Resolve playlist file paths against metadata_db and serialize to JSON
     * using json_builder_t with bounds checking. */
    json_builder_t json;
    json_builder_init(&json, json_buf, 65536);
    json_builder_appendf(&json, "{\"songs\":[");
    int emitted = 0;
    for (int i = 0; i < count && !json.truncated; i++) {
        song_row_t row;
        if (!metadata_db_get_song_by_path(paths[i], &row)) continue;

        char display_title[128], title_esc[300] = {0}, artist_esc[300] = {0};
        metadata_db_song_display_title(&row, display_title, sizeof(display_title));
        json_escape_append(title_esc, sizeof(title_esc), display_title);
        json_escape_append(artist_esc, sizeof(artist_esc), row.tags.artist);
        if (!json_builder_appendf(&json, "%s{\"index\":%lld,\"title\":\"%s\",\"artist\":\"%s\"}",
                                  emitted > 0 ? "," : "", (long long) row.id, title_esc, artist_esc)) break;
        emitted++;
    }
    json.truncated = false;
    json_builder_appendf(&json, "]}");

    send_response(cfd, "200 OK", "application/json", json_buf);

    free(json_buf);
    for (int i = 0; i < count; i++) free(paths[i]);
    free(paths);
}

/* Reads and discards the request up to the blank line ending the headers
 * (this server never needs a request body -- see query_param_int()'s own
 * comment), keeping only the first line to parse the method/path out of.
 * Bounded read -- see REQUEST_LINE_MAX -- matches this project's general
 * "bound anything read from a socket" posture (http_client.c's own
 * response parsing does the same). */
#define REQUEST_LINE_MAX 4096
#define REQUEST_HEADERS_MAX 4096

static void handle_connection(int cfd) {
    if (!remote_control_ensure_pin()) {
        send_response(cfd, "503 Service Unavailable", "text/plain", "PIN generation unavailable");
        return;
    }
    char buf[REQUEST_LINE_MAX];
    size_t n = 0;
    bool line_complete = false;
    /* Stream transports (including RFCOMM) may deliver the request line in
     * several reads. Read through LF, bounded by REQUEST_LINE_MAX and an idle
     * timeout. */
    while (n + 1 < sizeof(buf)) {
        struct pollfd pfd = { .fd = cfd, .events = POLLIN, .revents = 0 };
        int ready = poll(&pfd, 1, 2000);
        if (ready <= 0) {
            if (ready < 0 && errno == EINTR) continue;
            if (ready == 0) send_response(cfd, "408 Request Timeout", "text/plain", "Request Timeout");
            return;
        }
        char c;
        ssize_t nr = read(cfd, &c, 1);
        if (nr <= 0) return; /* caller owns fd cleanup */
        buf[n++] = c;
        if (c == '\n') {
            line_complete = true;
            break;
        }
    }
    if (!line_complete) {
        send_response(cfd, "414 URI Too Long", "text/plain", "Request line too long");
        return;
    }
    buf[n] = '\0';

    /* Consume the headers before replying. Leaving them unread when closing a
     * TCP socket can reset the connection and discard the response. This API
     * has no request-body endpoints, so only the bounded header section is
     * consumed. Accept CRLF or LF line endings. */
    size_t header_bytes = 0;
    bool headers_complete = false;
    char pin_header[REMOTE_CONTROL_PIN_MAX_LENGTH + 1] = {0};
    bool pin_header_seen = false;
    bool pin_header_invalid = false;
    char header_line[512];
    size_t header_line_len = 0;
    while (header_bytes < REQUEST_HEADERS_MAX) {
        struct pollfd pfd = { .fd = cfd, .events = POLLIN, .revents = 0 };
        int ready = poll(&pfd, 1, 2000);
        if (ready <= 0) {
            if (ready < 0 && errno == EINTR) continue;
            send_response(cfd, ready == 0 ? "408 Request Timeout" : "400 Bad Request",
                          "text/plain", ready == 0 ? "Request Timeout" : "Bad Request");
            return;
        }
        char c;
        ssize_t nr = read(cfd, &c, 1);
        if (nr <= 0) return;
        header_bytes++;
        if (c == '\n') {
            if (header_line_len && header_line[header_line_len - 1] == '\r') header_line_len--;
            header_line[header_line_len] = '\0';
            if (header_line_len == 0) {
                headers_complete = true;
                break;
            }
            static const char pin_name[] = "X-Compas-PIN:";
            if (header_line_len >= sizeof(pin_name) - 1 &&
                strncasecmp(header_line, pin_name, sizeof(pin_name) - 1) == 0) {
                const char * value = header_line + sizeof(pin_name) - 1;
                while (*value == ' ' || *value == '\t') value++;
                size_t value_len = strlen(value);
                while (value_len && (value[value_len - 1] == ' ' || value[value_len - 1] == '\t')) value_len--;
                if (pin_header_seen || value_len == 0 || value_len >= sizeof(pin_header)) {
                    pin_header_invalid = true;
                } else {
                    memcpy(pin_header, value, value_len);
                    pin_header[value_len] = '\0';
                    pin_header_seen = true;
                }
            }
            header_line_len = 0;
        } else if (header_line_len + 1 < sizeof(header_line)) {
            header_line[header_line_len++] = c;
        } else {
            send_response(cfd, "431 Request Header Fields Too Large", "text/plain", "Request headers too large");
            return;
        }
    }
    if (!headers_complete) {
        send_response(cfd, "431 Request Header Fields Too Large", "text/plain", "Request headers too large");
        return;
    }

    char method[8] = {0};
    char path[REQUEST_LINE_MAX] = {0};
    if (sscanf(buf, "%7s %4095s", method, path) != 2) {
        send_response(cfd, "400 Bad Request", "text/plain", "Bad Request");
        return;
    }

    /* Path only, no query string, for the exact-match routes below --
     * every GET route is a fixed path, only the POST /api/playback routes
     * ever have a "?...". */
    char path_only[REQUEST_LINE_MAX];
    snprintf(path_only, sizeof(path_only), "%s", path);
    char * q = strchr(path_only, '?');
    if (q) *q = '\0';

    /* The v1 namespace is an alias layer over the original routes. Keep
     * path_only bounded and require the slash after the version so that
     * similarly named paths (for example /api/v10) never match. */
    bool api_v1 = strncmp(path_only, "/api/v1/", sizeof("/api/v1/") - 1) == 0;
    if (api_v1) {
        if (strcmp(path_only, "/api/v1/capabilities") == 0 && strcmp(method, "GET") == 0) {
            static const char capabilities[] =
                "{\"version\":1,\"authRequired\":true,\"authHeader\":\"X-Compas-PIN\","
                "\"transports\":[\"wifi\",\"bluetooth_classic_rfcomm\"],\"features\":["
                "\"status\",\"playback_controls\",\"queue\",\"library_browse\","
                "\"playlists\",\"album_art\"]}";
            send_response(cfd, "200 OK", "application/json", capabilities);
            return;
        }
        /* Remove only "/v1" so "/api/v1/status" routes as "/api/status". */
        memmove(path_only + sizeof("/api") - 1, path_only + sizeof("/api/v1") - 1,
                strlen(path_only + sizeof("/api/v1") - 1) + 1);
    }

    /* Only the shell and capabilities handshake are public. All library,
     * status, artwork, playback, playlist, queue, and asset requests require
     * the same PIN header over both Wi-Fi and Bluetooth RFCOMM. */
    bool public_request = strcmp(method, "GET") == 0 &&
                          (strcmp(path_only, "/") == 0 ||
                           strcmp(path, "/api/v1/capabilities") == 0 ||
                           strcmp(path, "/api/capabilities") == 0);
    if (!public_request) {
        remote_control_pin_result_t pin_result = REMOTE_CONTROL_PIN_INVALID;
        if (!pin_header_invalid && pin_header_seen) pin_result = remote_control_pin_check(pin_header);
        if (pin_result == REMOTE_CONTROL_PIN_RATE_LIMITED) {
            send_pin_rate_limited_response(cfd);
            return;
        }
        if (pin_result != REMOTE_CONTROL_PIN_VALID) {
            send_response(cfd, "401 Unauthorized", "application/json",
                          "{\"error\":\"unauthorized\",\"code\":\"pin_required\"}");
            return;
        }
    }

    if (strcmp(method, "GET") == 0) {
        if (strcmp(path_only, "/api/status") == 0) {
            char json[1600];
            build_status_json(json, sizeof(json));
            send_response(cfd, "200 OK", "application/json", json);
        } else if (strcmp(path_only, "/api/library") == 0) {
            int offset = 0, limit = 50;
            char query_raw[128] = {0}, query[128] = {0};
            char artist_raw[128] = {0}, artist_filter[128] = {0};
            char album_artist_raw[128] = {0}, album_artist_filter[128] = {0};
            char album_raw[128] = {0}, album_filter[128] = {0};
            char genre_raw[METADATA_DB_TEXT_MAX * 3] = {0};
            char genre_filter[METADATA_DB_TEXT_MAX] = {0};
            query_param_int(path, "offset", &offset);
            query_param_int(path, "limit", &limit);
            bool query_ok = (!query_param_present(path, "q") || query_param_str(path, "q", query_raw, sizeof(query_raw))) &&
                            (!query_param_present(path, "artist") || query_param_str(path, "artist", artist_raw, sizeof(artist_raw))) &&
                            (!query_param_present(path, "album_artist") || query_param_str(path, "album_artist", album_artist_raw, sizeof(album_artist_raw))) &&
                            (!query_param_present(path, "album") || query_param_str(path, "album", album_raw, sizeof(album_raw)));
            if (!query_ok) {
                send_response(cfd, "400 Bad Request", "application/json",
                              "{\"error\":\"query_parameter_too_long\"}");
                return;
            }
            if (query_param_str(path, "q", query_raw, sizeof(query_raw))) {
                url_decode(query_raw, query, sizeof(query));
            }
            if (query_param_str(path, "artist", artist_raw, sizeof(artist_raw))) {
                url_decode(artist_raw, artist_filter, sizeof(artist_filter));
            }
            if (query_param_str(path, "album_artist", album_artist_raw, sizeof(album_artist_raw))) {
                url_decode(album_artist_raw, album_artist_filter, sizeof(album_artist_filter));
            }
            if (query_param_str(path, "album", album_raw, sizeof(album_raw))) {
                url_decode(album_raw, album_filter, sizeof(album_filter));
            }
            if (query_param_present(path, "genre") &&
                (!query_param_str(path, "genre", genre_raw, sizeof(genre_raw)) ||
                 !url_decode_checked(genre_raw, genre_filter, sizeof(genre_filter)))) {
                send_response(cfd, "400 Bad Request", "application/json",
                              "{\"error\":\"invalid_genre_filter\"}");
                return;
            }
            char * json = malloc(65536);
            if (json) {
                build_library_json(query, artist_filter, album_artist_filter, album_filter, genre_filter, offset, limit, json,
                                    65536);
                send_response(cfd, "200 OK", "application/json", json);
                free(json);
            } else {
                send_response(cfd, "500 Internal Server Error", "text/plain", "Out of memory");
            }
        } else if (strcmp(path_only, "/api/library/artists") == 0) {
            char * json = malloc(65536);
            if (json) {
                build_artists_json(json, 65536);
                send_response(cfd, "200 OK", "application/json", json);
                free(json);
            } else {
                send_response(cfd, "500 Internal Server Error", "text/plain", "Out of memory");
            }
        } else if (strcmp(path_only, "/api/library/album_artists") == 0) {
            char * json = malloc(65536);
            if (json) {
                build_album_artists_json(json, 65536);
                send_response(cfd, "200 OK", "application/json", json);
                free(json);
            } else {
                send_response(cfd, "500 Internal Server Error", "text/plain", "Out of memory");
            }
        } else if (strcmp(path_only, "/api/library/genres") == 0) {
            char * json = malloc(65536);
            if (json) {
                build_genres_json(json, 65536);
                send_response(cfd, "200 OK", "application/json", json);
                free(json);
            } else {
                send_response(cfd, "500 Internal Server Error", "text/plain", "Out of memory");
            }
        } else if (strcmp(path_only, "/api/library/albums") == 0) {
            char artist_raw[128] = {0}, artist_filter[128] = {0};
            char album_artist_raw[128] = {0}, album_artist_filter[128] = {0};
            bool query_ok = (!query_param_present(path, "artist") || query_param_str(path, "artist", artist_raw, sizeof(artist_raw))) &&
                            (!query_param_present(path, "album_artist") || query_param_str(path, "album_artist", album_artist_raw, sizeof(album_artist_raw)));
            if (!query_ok) {
                send_response(cfd, "400 Bad Request", "application/json",
                              "{\"error\":\"query_parameter_too_long\"}");
                return;
            }
            if (query_param_str(path, "artist", artist_raw, sizeof(artist_raw))) {
                url_decode(artist_raw, artist_filter, sizeof(artist_filter));
            }
            if (query_param_str(path, "album_artist", album_artist_raw, sizeof(album_artist_raw))) {
                url_decode(album_artist_raw, album_artist_filter, sizeof(album_artist_filter));
            }
            char * json = malloc(65536);
            if (json) {
                build_albums_json(artist_filter, album_artist_filter, json, 65536);
                send_response(cfd, "200 OK", "application/json", json);
                free(json);
            } else {
                send_response(cfd, "500 Internal Server Error", "text/plain", "Out of memory");
            }
        } else if (strcmp(path_only, "/api/art") == 0) {
            handle_art_request(cfd, path);
        } else if (strcmp(path_only, "/api/playlists") == 0) {
            /* Heap, not stack: this runs on the remote listener / RFCOMM worker
             * thread (default 128 KiB stack), and keeping large JSON buffers off
             * the frame keeps handle_connection well clear of stack exhaustion
             * on the deep metadata_db query paths below. */
            char * json = malloc(8192);
            if (json) {
                build_playlists_json(json, 8192);
                send_response(cfd, "200 OK", "application/json", json);
                free(json);
            } else {
                send_response(cfd, "500 Internal Server Error", "text/plain", "Out of memory");
            }
        } else if (strcmp(path_only, "/api/playlists/songs") == 0) {
            handle_playlist_songs_request(cfd, path);
        } else if (strcmp(path_only, "/api/queue") == 0) {
            int offset = 0, limit = 25;
            if (query_param_int(path, "offset", &offset) && offset < 0) offset = 0;
            if (query_param_int(path, "limit", &limit) && limit < 1) limit = 25;
            if (limit > 25) limit = 25;
            /* Heap, not stack: a 64 KiB frame here on a 128 KiB worker-thread
             * stack left too little headroom for the metadata_db query paths
             * build_queue_json() reaches. */
            char * json = malloc(65536);
            if (json) {
                build_queue_json(json, 65536, offset, limit);
                send_response(cfd, "200 OK", "application/json", json);
                free(json);
            } else {
                send_response(cfd, "500 Internal Server Error", "text/plain", "Out of memory");
            }
        } else if (strcmp(path_only, "/assets/icon") == 0) {
            handle_icon_request(cfd, path);
        } else if (strcmp(path_only, "/") == 0) {
            send_response(cfd, "200 OK", "text/html", REMOTE_CONTROL_APP_HTML);
        } else {
            send_response(cfd, "404 Not Found", "text/plain", "Not Found");
        }
    } else if (strcmp(method, "POST") == 0) {
        /* Playlist mutation (create / add-to) is handled separately below,
         * outside the mutex-guarded flag-setting block -- it does real file
         * I/O (playlist_files_create()/_append()/_contains(), potentially
         * slow SD card writes), which must never happen while holding
         * status_mutex or it would stall gui.c's own per-tick status-push/
         * consume calls on the main thread for the duration of that I/O.
         * Only the small "which song, by index" lookup needs the lock. */
        if (strcmp(path_only, "/api/playlists") == 0 || strcmp(path_only, "/api/playlists/add") == 0) {
            char raw_name[REQUEST_LINE_MAX] = {0};
            char name[REQUEST_LINE_MAX] = {0};
            int64_t index = -1;
            bool have_name = query_param_str(path, "name", raw_name, sizeof(raw_name));
            if (have_name) {
                url_decode(raw_name, name, sizeof(name));
                /* A decoded NUL would make validation see only a prefix. */
                for (const char * p = raw_name; p[0] && p[1] && p[2]; p++) {
                    if (p[0] == '%' && p[1] == '0' && p[2] == '0') {
                        have_name = false;
                        break;
                    }
                }
            }
            bool have_index = query_param_int64(path, "index", &index);

            char song_path[600] = {0};
            bool have_song = false;
            if (have_index) {
                song_row_t row;
                if (metadata_db_get_song_by_id(index, &row)) {
                    snprintf(song_path, sizeof(song_path), "%s", row.path);
                    have_song = true;
                }
            }

            if (!have_name || strlen(name) > PLAYLIST_NAME_MAX_BYTES ||
                !playlist_name_is_safe(name) || !have_song) {
                send_response(cfd, "400 Bad Request", "text/plain", "Bad Request");
            } else if (strcmp(path_only, "/api/playlists") == 0) {
                char created_path[512];
                bool created = playlist_files_create(PLAYLISTS_DIR, name, song_path, created_path, sizeof(created_path));
                if (created) metadata_db_playlist_insert_one(created_path);
                send_response(cfd, created ? "200 OK" : "500 Internal Server Error", "text/plain",
                               created ? "OK" : "Failed to create playlist");
            } else {
                char m3u_path[512];
                int path_len = snprintf(m3u_path, sizeof(m3u_path), "%s/%s.m3u", PLAYLISTS_DIR, name);
                if (path_len < 0 || (size_t) path_len >= sizeof(m3u_path)) {
                    send_response(cfd, "400 Bad Request", "text/plain", "Bad Request");
                } else if (playlist_files_contains(m3u_path, song_path)) {
                    send_response(cfd, "200 OK", "text/plain", "Song already in playlist");
                } else {
                    bool added = playlist_files_append(m3u_path, song_path);
                    send_response(cfd, added ? "200 OK" : "500 Internal Server Error", "text/plain",
                                   added ? "OK" : "Failed to add to playlist");
                }
            }
        } else {
            bool ok = true;
            bool conflict = false;
            pthread_mutex_lock(&status_mutex);
            if (strcmp(path_only, "/api/playback/toggle") == 0) {
                request_play_pause = true;
            } else if (strcmp(path_only, "/api/playback/next") == 0) {
                request_next = true;
            } else if (strcmp(path_only, "/api/playback/prev") == 0) {
                request_prev = true;
            } else if (strcmp(path_only, "/api/playback/mode") == 0) {
                char mode_raw[32] = {0};
                bool mode_present = query_param_str(path, "mode", mode_raw, sizeof(mode_raw));
                if (!mode_present) {
                    request_mode_cycle = true;
                } else {
                    char * end = NULL;
                    errno = 0;
                    long mode = strtol(mode_raw, &end, 10);
                    if (errno == 0 && end != mode_raw && *end == '\0' && mode >= 0 && mode <= 3) {
                        request_has_play_mode = true;
                        request_play_mode = (int) mode;
                    } else {
                        ok = false;
                    }
                }
            } else if (strcmp(path_only, "/api/playback/seek") == 0) {
                int seconds;
                if (query_param_int(path, "seconds", &seconds) && seconds >= 0) {
                    request_has_seek = true;
                    request_seek_seconds = seconds;
                } else {
                    ok = false;
                }
            } else if (strcmp(path_only, "/api/playback/volume") == 0) {
                int percent;
                if (query_param_int(path, "percent", &percent) && percent >= 0 && percent <= 100) {
                    request_has_volume = true;
                    request_volume_percent = percent;
                } else {
                    ok = false;
                }
            } else if (strcmp(path_only, "/api/playback/play") == 0) {
                int64_t index;
                if (query_param_int64(path, "index", &index) && index >= 0) {
                    request_has_play_index = true;
                    request_play_index = index;

                    char raw[128];
                    request_play_playlist_name[0] = '\0';
                    request_play_artist_filter[0] = '\0';
                    request_play_album_artist_filter[0] = '\0';
                    request_play_album_filter[0] = '\0';
                    if (query_param_str(path, "playlist", raw, sizeof(raw))) {
                        url_decode(raw, request_play_playlist_name, sizeof(request_play_playlist_name));
                    }
                    if (query_param_str(path, "artist", raw, sizeof(raw))) {
                        url_decode(raw, request_play_artist_filter, sizeof(request_play_artist_filter));
                    }
                    if (query_param_str(path, "album_artist", raw, sizeof(raw))) {
                        url_decode(raw, request_play_album_artist_filter, sizeof(request_play_album_artist_filter));
                    }
                    if (query_param_str(path, "album", raw, sizeof(raw))) {
                        url_decode(raw, request_play_album_filter, sizeof(request_play_album_filter));
                    }
                } else {
                    ok = false;
                }
            } else if (strcmp(path_only, "/api/playback/queue") == 0) {
                int64_t index;
                if (query_param_int64(path, "index", &index) && index >= 0) {
                    request_has_queue_index = true;
                    request_queue_index = index;
                } else {
                    ok = false;
                }
            } else if (strcmp(path_only, "/api/queue/remove") == 0) {
                int offset;
                int64_t revision;
                if (query_param_int(path, "offset", &offset) && offset >= 0 &&
                    query_param_int64(path, "revision", &revision) && revision >= 0) {
                    if ((uint64_t) revision != queue_snapshot_revision || offset >= queue_song_id_count) {
                        ok = false;
                        conflict = true;
                    } else {
                        request_has_queue_remove = true;
                        request_queue_remove_offset = offset;
                        request_queue_remove_revision = (uint64_t) revision;
                    }
                } else ok = false;
            } else if (strcmp(path_only, "/api/queue/clear") == 0) {
                int64_t revision;
                if (query_param_int64(path, "revision", &revision) && revision >= 0) {
                    if ((uint64_t) revision != queue_snapshot_revision) {
                        ok = false;
                        conflict = true;
                    } else {
                        request_queue_clear = true;
                        request_queue_clear_revision = (uint64_t) revision;
                    }
                } else ok = false;
            } else {
                ok = false;
            }
            pthread_mutex_unlock(&status_mutex);

            if (ok) {
                send_response(cfd, "200 OK", "text/plain", "OK");
            } else if (conflict) {
                send_response(cfd, "409 Conflict", "text/plain", "Queue changed; reload it and retry");
            } else {
                send_response(cfd, "404 Not Found", "text/plain", "Not Found");
            }
        }
    } else {
        send_response(cfd, "405 Method Not Allowed", "text/plain", "Method Not Allowed");
    }

}

/* Handle one HTTP request on an already connected stream socket. The caller
 * retains ownership of fd and is responsible for closing it. */
void remote_control_handle_stream(int fd) {
    if (fd >= 0) handle_connection(fd);
}

/* Timeout for poll() on listen_fd so listener_thread_func periodically checks
 * running and terminates cleanly on stop. */
#define ACCEPT_POLL_TIMEOUT_MS 200

static void * listener_thread_func(void * arg) {
    (void) arg;
    while (atomic_load_explicit(&running, memory_order_acquire)) {
        pthread_mutex_lock(&listener_mutex);
        int fd = listen_fd;
        pthread_mutex_unlock(&listener_mutex);
        if (fd < 0) break;
        struct pollfd pfd = { .fd = fd, .events = POLLIN, .revents = 0 };
        int pr = poll(&pfd, 1, ACCEPT_POLL_TIMEOUT_MS);
        if (pr <= 0) continue; /* Timeout or interrupted -- re-check running */
        if (!atomic_load_explicit(&running, memory_order_acquire)) break;

        int cfd = accept(fd, NULL, NULL);
        if (cfd < 0) continue;
        struct timeval timeout = { .tv_sec = 1, .tv_usec = 0 };
        setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        pthread_mutex_lock(&listener_mutex);
        if (!atomic_load_explicit(&running, memory_order_acquire)) {
            pthread_mutex_unlock(&listener_mutex);
            close(cfd);
            break;
        }
        active_client_fd = cfd;
        pthread_mutex_unlock(&listener_mutex);
        handle_connection(cfd);
        pthread_mutex_lock(&listener_mutex);
        if (active_client_fd == cfd) {
            active_client_fd = -1;
            close(cfd);
        }
        pthread_mutex_unlock(&listener_mutex);
    }
    return NULL;
}

void remote_control_start(void) {
    if (!remote_control_ensure_pin()) return;
    pthread_mutex_lock(&listener_mutex);
    if (atomic_load_explicit(&running, memory_order_acquire)) {
        pthread_mutex_unlock(&listener_mutex);
        return;
    }

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        pthread_mutex_unlock(&listener_mutex);
        return;
    }

    int reuse = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(REMOTE_CONTROL_PORT);

    if (bind(listen_fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        close(listen_fd);
        listen_fd = -1;
        pthread_mutex_unlock(&listener_mutex);
        return;
    }
    if (listen(listen_fd, 4) < 0) {
        close(listen_fd);
        listen_fd = -1;
        pthread_mutex_unlock(&listener_mutex);
        return;
    }

    atomic_store_explicit(&running, true, memory_order_release);
    if (pthread_create(&listener_thread, NULL, listener_thread_func, NULL) != 0) {
        atomic_store_explicit(&running, false, memory_order_release);
        close(listen_fd);
        listen_fd = -1;
    } else {
        /* DNS-SD runs on its own best-effort worker. A missing Wi-Fi link or
         * occupied multicast socket never affects the HTTP listener. */
        remote_control_mdns_start();
    }
    pthread_mutex_unlock(&listener_mutex);
}

void remote_control_stop(void) {
    remote_control_mdns_stop();
    pthread_mutex_lock(&listener_mutex);
    if (!atomic_load_explicit(&running, memory_order_acquire)) {
        pthread_mutex_unlock(&listener_mutex);
        return;
    }
    atomic_store_explicit(&running, false, memory_order_release);
    /* Interrupt poll(), accept(), and a client read before joining. The
     * client handler owns close(); shutdown only wakes a concurrent read. */
    int stopped_listen_fd = listen_fd;
    if (stopped_listen_fd >= 0) shutdown(stopped_listen_fd, SHUT_RDWR);
    if (active_client_fd >= 0) shutdown(active_client_fd, SHUT_RDWR);
    pthread_mutex_unlock(&listener_mutex);

    pthread_join(listener_thread, NULL);
    pthread_mutex_lock(&listener_mutex);
    if (listen_fd == stopped_listen_fd && listen_fd >= 0) {
        close(listen_fd);
        listen_fd = -1;
    }
    pthread_mutex_unlock(&listener_mutex);
}
