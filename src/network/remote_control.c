#include "remote_control.h"
#include "remote_control_webapp.h"
#include "metadata.h"
#include "metadata_db.h"
#include "playlist_files.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
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

/* Playback requests -- edge-triggered flags consumed by update_timer_cb.
 * Guarded by status_mutex. */
static bool request_play_pause = false;
static bool request_next = false;
static bool request_prev = false;
static bool request_mode_cycle = false;
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
static bool request_queue_clear = false;
/* Scope filters echoed back by /api/playback/play to build the queue within
 * an album, artist, album-artist, or playlist view. Empty string means entire library. */
static char request_play_playlist_name[128] = "";
static char request_play_artist_filter[128] = "";
static char request_play_album_artist_filter[128] = "";
static char request_play_album_filter[128] = "";

/* Queue of song IDs (metadata_db song_row_t.id). Guarded by status_mutex. */
static int64_t * queue_song_ids = NULL;
static int queue_song_id_count = 0;

static bool running = false;
static int listen_fd = -1;
static pthread_t listener_thread;

void remote_control_notify_status(bool playing, bool paused, const char * title, const char * artist,
                                   const char * album, const char * path, int position_seconds,
                                   int duration_seconds, float volume, int play_mode) {
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

bool remote_control_consume_queue_remove(int * out_offset) {
    pthread_mutex_lock(&status_mutex);
    bool result = request_has_queue_remove;
    if (result) {
        request_has_queue_remove = false;
        *out_offset = request_queue_remove_offset;
    }
    pthread_mutex_unlock(&status_mutex);
    return result;
}

bool remote_control_consume_queue_clear(void) {
    pthread_mutex_lock(&status_mutex);
    bool result = request_queue_clear;
    request_queue_clear = false;
    pthread_mutex_unlock(&status_mutex);
    return result;
}

void remote_control_sync_queue(const char * const * paths, int count) {
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
            for (int i = 0; i < count; i++) {
                if (rows[i].id != -1) ids[id_count++] = rows[i].id;
            }
            free(rows);
        }
    }

    pthread_mutex_lock(&status_mutex);
    free(queue_song_ids);
    queue_song_ids = ids;
    queue_song_id_count = id_count;
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

/* Minimal JSON string escaping -- handles quote, backslash, and control
 * characters, the only ones a real song/artist/album tag could plausibly
 * contain that would break the JSON. Not a general-purpose escaper. */
static void json_escape_append(char * out, size_t out_size, const char * in) {
    size_t len = strlen(out);
    for (const char * p = in; *p && len + 2 < out_size; p++) {
        unsigned char c = (unsigned char) *p;
        if (c == '"' || c == '\\') {
            if (len + 3 >= out_size) break;
            out[len++] = '\\';
            out[len++] = (char) c;
        } else if (c < 0x20) {
            continue; /* drop control characters rather than \u-escape them -- none are expected in real tags */
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

/* Pulls a raw (still percent-encoded) string query parameter's value.
 * Returns false (leaving out untouched) if the key isn't present. */
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
            if (val_len >= out_size) val_len = out_size - 1;
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
                                const char * album_filter, int offset, int limit, char * out, size_t out_size) {
    if (limit <= 0 || limit > LIBRARY_JSON_MAX_LIMIT) limit = LIBRARY_JSON_MAX_LIMIT;
    if (offset < 0) offset = 0;

    /* Query filtered songs directly from metadata_db. "index" in the response
     * is the persistent song id (song_row_t.id). */
    int64_t total_matches = metadata_db_count_songs_filtered(query, artist_filter, album_artist_filter, album_filter);

    json_builder_t json;
    json_builder_init(&json, out, out_size);
    json_builder_appendf(&json, "{\"total\":%lld,\"songs\":[", (long long) total_matches);

    /* Allocate rows on the heap to keep stack usage bounded. */
    song_row_t * rows = malloc(sizeof(song_row_t) * LIBRARY_JSON_MAX_LIMIT);
    int n = rows ? metadata_db_get_songs_filtered_page(query, artist_filter, album_artist_filter, album_filter,
                                                         offset, limit, rows)
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
    for (int i = 0; i < count && !json.truncated && json.capacity - json.length >= 512; i++) {
        char name_esc[300] = {0}, album_artist_esc[300] = {0};
        json_escape_append(name_esc, sizeof(name_esc), groups[i].name);
        /* album_artist is populated for album groups. */
        json_escape_append(album_artist_esc, sizeof(album_artist_esc), groups[i].album_artist);
        if (!json_builder_appendf(&json, "%s{\"name\":\"%s\",\"count\":%d,\"index\":%lld,\"album_artist\":\"%s\"}",
                                  i > 0 ? "," : "", name_esc, groups[i].song_count,
                                  (long long) groups[i].first_song_id, album_artist_esc)) break;
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
             "\"position_seconds\":%d,\"duration_seconds\":%d,\"volume\":%.2f,\"play_mode\":%d}",
             status_playing ? "true" : "false", status_paused ? "true" : "false", title_esc, artist_esc, album_esc,
             status_position_seconds, status_duration_seconds, (double) status_volume, status_play_mode);
    pthread_mutex_unlock(&status_mutex);
}

/* The playlist `name` becomes a filesystem path component
 * (PLAYLISTS_DIR "/" name ".m3u", see playlist_files_create()) -- unlike
 * gui.c's own in-app equivalent (new_playlist_name_done_cb(), whose name
 * comes from a UI text field only the device's own user can type into),
 * this is reachable by anyone on the network with no authentication, so
 * it's validated here rather than trusted the way playlist_files_create()'s
 * own doc comment says its callers must. Rejects empty names, anything
 * containing a path separator, and anything containing ".." (defense in
 * depth against path traversal even though a bare ".." alone couldn't
 * escape PLAYLISTS_DIR without a "/" too). */
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
    for (int i = 0; i < count && !json.truncated; i++) {
        const char * slash = strrchr(paths[i], '/');
        const char * base = slash ? slash + 1 : paths[i];
        char name_esc[300] = {0};
        json_escape_append(name_esc, sizeof(name_esc), base);
        if (!json_builder_appendf(&json, ",{\"name\":\"%s\",\"internal\":false,\"writable\":true}", name_esc)) break;
    }
    json.truncated = false;
    json_builder_appendf(&json, "]}");

    for (int i = 0; i < count; i++) free(paths[i]);
    free(paths);
}

static void build_queue_json(char * out, size_t out_size) {
    /* Snapshot queue_song_ids under status_mutex, then resolve IDs via
     * metadata_db_get_songs_by_ids() outside the lock. */
    pthread_mutex_lock(&status_mutex);
    int count = queue_song_id_count;
    int64_t * ids = NULL;
    if (count > 0) {
        ids = malloc(sizeof(int64_t) * (size_t) count);
        if (ids) memcpy(ids, queue_song_ids, sizeof(int64_t) * (size_t) count);
        else count = 0;
    }
    pthread_mutex_unlock(&status_mutex);

    song_row_t * rows = count > 0 ? malloc(sizeof(song_row_t) * (size_t) count) : NULL;
    if (count > 0 && !rows) count = 0;
    if (rows) metadata_db_get_songs_by_ids(ids, count, rows);

    /* Build JSON array of queue songs using json_builder_t with bounds checking. */
    json_builder_t json;
    json_builder_init(&json, out, out_size);
    json_builder_appendf(&json, "{\"songs\":[");
    for (int i = 0; i < count && !json.truncated; i++) {
        if (rows[i].id == -1) continue;
        char display_title[128], title[512] = {0}, artist[512] = {0};
        metadata_db_song_display_title(&rows[i], display_title, sizeof(display_title));
        json_escape_append(title, sizeof(title), display_title);
        json_escape_append(artist, sizeof(artist), rows[i].tags.artist);
        if (!json_builder_appendf(&json, "%s{\"offset\":%d,\"index\":%lld,\"title\":\"%s\",\"artist\":\"%s\"}",
                                  i ? "," : "", i, (long long) rows[i].id, title, artist)) break;
    }
    json.truncated = false;
    json_builder_appendf(&json, "]}");
    free(rows);
    free(ids);
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

/* Fixed whitelist, not an arbitrary caller-supplied filename -- this server
 * has no authentication, so GET /assets/icon?name= must never be able to
 * read anything outside this exact set. Matches build_music_screen()'s own
 * icon choices in gui.c exactly (including reusing category/genre.png for
 * "Playlists", same as the in-app Music menu does -- no dedicated playlist
 * icon exists in the stock theme pack, see that function's own comment). */
static const struct {
    const char * name;
    const char * relative_path;
} CATEGORY_ICONS[] = {
    { "all", "category/all.png" },
    { "artist", "category/artist.png" },
    { "album_artist", "category/album_artist.png" },
    { "genre", "category/genre.png" },
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
#define REQUEST_LINE_MAX 512

static void handle_connection(int cfd) {
    char buf[REQUEST_LINE_MAX];
    ssize_t n = read(cfd, buf, sizeof(buf) - 1);
    if (n <= 0) {
        close(cfd);
        return;
    }
    buf[n] = '\0';

    char method[8] = {0};
    char path[256] = {0};
    sscanf(buf, "%7s %255s", method, path);

    /* Path only, no query string, for the exact-match routes below --
     * every GET route is a fixed path, only the POST /api/playback routes
     * ever have a "?...". */
    char path_only[256];
    snprintf(path_only, sizeof(path_only), "%s", path);
    char * q = strchr(path_only, '?');
    if (q) *q = '\0';

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
            query_param_int(path, "offset", &offset);
            query_param_int(path, "limit", &limit);
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
            char * json = malloc(65536);
            if (json) {
                build_library_json(query, artist_filter, album_artist_filter, album_filter, offset, limit, json,
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
        } else if (strcmp(path_only, "/api/library/albums") == 0) {
            char artist_raw[128] = {0}, artist_filter[128] = {0};
            char album_artist_raw[128] = {0}, album_artist_filter[128] = {0};
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
            char json[8192];
            build_playlists_json(json, sizeof(json));
            send_response(cfd, "200 OK", "application/json", json);
        } else if (strcmp(path_only, "/api/playlists/songs") == 0) {
            handle_playlist_songs_request(cfd, path);
        } else if (strcmp(path_only, "/api/queue") == 0) {
            char json[16384];
            build_queue_json(json, sizeof(json));
            send_response(cfd, "200 OK", "application/json", json);
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
            char name[128] = {0};
            int64_t index = -1;
            bool have_name = query_param_str(path, "name", name, sizeof(name));
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

            if (!have_name || !playlist_name_is_safe(name) || !have_song) {
                send_response(cfd, "400 Bad Request", "text/plain", "Bad Request");
            } else if (strcmp(path_only, "/api/playlists") == 0) {
                char created_path[512];
                bool created = playlist_files_create(PLAYLISTS_DIR, name, song_path, created_path, sizeof(created_path));
                if (created) metadata_db_playlist_insert_one(created_path);
                send_response(cfd, created ? "200 OK" : "500 Internal Server Error", "text/plain",
                               created ? "OK" : "Failed to create playlist");
            } else {
                char m3u_path[512];
                snprintf(m3u_path, sizeof(m3u_path), "%s/%s.m3u", PLAYLISTS_DIR, name);
                if (playlist_files_contains(m3u_path, song_path)) {
                    send_response(cfd, "200 OK", "text/plain", "Song already in playlist");
                } else {
                    bool added = playlist_files_append(m3u_path, song_path);
                    send_response(cfd, added ? "200 OK" : "500 Internal Server Error", "text/plain",
                                   added ? "OK" : "Failed to add to playlist");
                }
            }
        } else {
            bool ok = true;
            pthread_mutex_lock(&status_mutex);
            if (strcmp(path_only, "/api/playback/toggle") == 0) {
                request_play_pause = true;
            } else if (strcmp(path_only, "/api/playback/next") == 0) {
                request_next = true;
            } else if (strcmp(path_only, "/api/playback/prev") == 0) {
                request_prev = true;
            } else if (strcmp(path_only, "/api/playback/mode") == 0) {
                request_mode_cycle = true;
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
                if (query_param_int(path, "offset", &offset) && offset >= 0) {
                    request_has_queue_remove = true;
                    request_queue_remove_offset = offset;
                } else ok = false;
            } else if (strcmp(path_only, "/api/queue/clear") == 0) {
                request_queue_clear = true;
            } else {
                ok = false;
            }
            pthread_mutex_unlock(&status_mutex);

            if (ok) {
                send_response(cfd, "200 OK", "text/plain", "OK");
            } else {
                send_response(cfd, "404 Not Found", "text/plain", "Not Found");
            }
        }
    } else {
        send_response(cfd, "405 Method Not Allowed", "text/plain", "Method Not Allowed");
    }

    close(cfd);
}

/* Timeout for poll() on listen_fd so listener_thread_func periodically checks
 * running and terminates cleanly on stop. */
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

void remote_control_start(void) {
    if (running) return;

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) return;

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
        return;
    }
    if (listen(listen_fd, 4) < 0) {
        close(listen_fd);
        listen_fd = -1;
        return;
    }

    running = true;
    pthread_create(&listener_thread, NULL, listener_thread_func, NULL);
}

void remote_control_stop(void) {
    if (!running) return;
    running = false;

    /* Wait for listener thread to exit after noticing running = false. */
    pthread_join(listener_thread, NULL);
    if (listen_fd >= 0) {
        close(listen_fd);
        listen_fd = -1;
    }
}
