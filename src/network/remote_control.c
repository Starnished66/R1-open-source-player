#include "remote_control.h"
#include "metadata.h"
#include "playlist_files.h"

#include <arpa/inet.h>
#include <ctype.h>
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

/* Playback requests -- same edge-triggered "background thread sets a flag,
 * update_timer_cb consumes it" pattern as hw_buttons.c/bt_media_player.c's
 * transport buttons, guarded by the same mutex as the status snapshot. */
static bool request_play_pause = false;
static bool request_next = false;
static bool request_prev = false;
static bool request_mode_cycle = false;
static bool request_has_seek = false;
static int request_seek_seconds = 0;
static bool request_has_volume = false;
static int request_volume_percent = 0;
static bool request_has_play_index = false;
static int request_play_index = 0;
/* Real-device bug report: playing a song from the web UI always built the
 * playback queue from the whole library (alphabetical, whatever play_mode
 * happens to be), regardless of which Album/Playlist the song was actually
 * tapped from -- reads as "it ignores the list I picked from and just
 * shuffles/plays through everything" even though play_mode itself was
 * always honored correctly, just against the wrong (too-broad) queue. The
 * web UI already fetches each of these views with the exact same artist/
 * album_artist/album/playlist context (see build_library_json()'s own
 * filters and /api/playlists/songs), so /api/playback/play now accepts the
 * same params echoed back -- empty/absent means "whole library", same as
 * today. gui.c resolves these into the same artist_groups/album_groups/
 * album_artist_groups or .m3u file its own on-device Artist/Album/Playlist
 * screens already use, so a remote play scopes next/prev identically to
 * tapping that song on the device itself. */
static char request_play_playlist_name[128] = "";
static char request_play_artist_filter[128] = "";
static char request_play_album_artist_filter[128] = "";
static char request_play_album_filter[128] = "";

/* This server's own copy of gui.c's library title/artist strings, kept in
 * the exact same index order (see remote_control_sync_library() in the
 * header). Guarded by status_mutex. */
static char ** library_titles = NULL;
static char ** library_artists = NULL;
static char ** library_album_artists = NULL;
static char ** library_albums = NULL;
static char ** library_paths = NULL;
static int library_count = 0;
static char * cached_artists_json = NULL;
static char * cached_album_artists_json = NULL;
static char * cached_albums_json = NULL;

static void refresh_group_json_cache(void);

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

bool remote_control_consume_play_index(int * out_index, char * out_playlist, size_t playlist_size,
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

static void free_library(void) {
    for (int i = 0; i < library_count; i++) {
        free(library_titles[i]);
        free(library_artists[i]);
        free(library_album_artists[i]);
        free(library_albums[i]);
        free(library_paths[i]);
    }
    free(library_titles);
    free(library_artists);
    free(library_album_artists);
    free(library_albums);
    free(library_paths);
    library_titles = NULL;
    library_artists = NULL;
    library_album_artists = NULL;
    library_albums = NULL;
    library_paths = NULL;
    library_count = 0;
    free(cached_artists_json);
    free(cached_album_artists_json);
    free(cached_albums_json);
    cached_artists_json = NULL;
    cached_album_artists_json = NULL;
    cached_albums_json = NULL;
}

void remote_control_sync_library(const char * const * titles, const char * const * artists,
                                  const char * const * album_artists, const char * const * albums,
                                  const char * const * paths, int count) {
    pthread_mutex_lock(&status_mutex);
    free_library();
    if (count > 0) {
        library_titles = malloc(sizeof(char *) * (size_t) count);
        library_artists = malloc(sizeof(char *) * (size_t) count);
        library_album_artists = malloc(sizeof(char *) * (size_t) count);
        library_albums = malloc(sizeof(char *) * (size_t) count);
        library_paths = malloc(sizeof(char *) * (size_t) count);
        for (int i = 0; i < count; i++) {
            library_titles[i] = strdup(titles[i] ? titles[i] : "");
            library_artists[i] = strdup(artists[i] && artists[i][0] ? artists[i] : "Unknown Artist");
            library_album_artists[i] = strdup(album_artists[i] && album_artists[i][0] ? album_artists[i] : "Unknown Artist");
            library_albums[i] = strdup(albums[i] && albums[i][0] ? albums[i] : "Unknown Album");
            library_paths[i] = strdup(paths[i] ? paths[i] : "");
        }
        library_count = count;
    }
    refresh_group_json_cache();
    pthread_mutex_unlock(&status_mutex);
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

/* Hand-rolled rather than strcasestr(), which isn't confirmed available on
 * this project's musl target libc. */
static bool contains_case_insensitive(const char * haystack, const char * needle) {
    if (!*needle) return true;
    size_t needle_len = strlen(needle);
    for (const char * h = haystack; *h; h++) {
        size_t i = 0;
        while (i < needle_len && h[i] && tolower((unsigned char) h[i]) == tolower((unsigned char) needle[i])) i++;
        if (i == needle_len) return true;
    }
    return false;
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

/* artist_filter/album_artist_filter/album_filter, each if non-empty,
 * restrict to songs whose corresponding tag exactly matches
 * (case-insensitive); query is still a substring match against
 * title/artist on top of whichever exact filters are active. */
static bool library_song_matches(int i, const char * query, const char * artist_filter,
                                  const char * album_artist_filter, const char * album_filter) {
    if (artist_filter[0] != '\0' && strcasecmp(library_artists[i], artist_filter) != 0) return false;
    if (album_artist_filter[0] != '\0' && strcasecmp(library_album_artists[i], album_artist_filter) != 0) return false;
    if (album_filter[0] != '\0' && strcasecmp(library_albums[i], album_filter) != 0) return false;
    if (query[0] == '\0') return true;
    return contains_case_insensitive(library_titles[i], query) || contains_case_insensitive(library_artists[i], query);
}

static void build_library_json(const char * query, const char * artist_filter, const char * album_artist_filter,
                                const char * album_filter, int offset, int limit, char * out, size_t out_size) {
    if (limit <= 0 || limit > LIBRARY_JSON_MAX_LIMIT) limit = LIBRARY_JSON_MAX_LIMIT;
    if (offset < 0) offset = 0;

    pthread_mutex_lock(&status_mutex);

    int total_matches = 0;
    for (int i = 0; i < library_count; i++) {
        if (library_song_matches(i, query, artist_filter, album_artist_filter, album_filter)) total_matches++;
    }

    json_builder_t json;
    json_builder_init(&json, out, out_size);
    json_builder_appendf(&json, "{\"total\":%d,\"songs\":[", total_matches);

    int matched_so_far = 0;
    int emitted = 0;
    for (int i = 0; i < library_count && emitted < limit && !json.truncated &&
                    json.capacity - json.length >= 700; i++) {
        if (!library_song_matches(i, query, artist_filter, album_artist_filter, album_filter)) continue;
        if (matched_so_far++ < offset) continue;

        char title_esc[300] = {0}, artist_esc[300] = {0};
        json_escape_append(title_esc, sizeof(title_esc), library_titles[i]);
        json_escape_append(artist_esc, sizeof(artist_esc), library_artists[i]);
        if (!json_builder_appendf(&json, "%s{\"index\":%d,\"title\":\"%s\",\"artist\":\"%s\"}",
                                  emitted > 0 ? "," : "", i, title_esc, artist_esc)) break;
        emitted++;
    }
    json.truncated = false; /* a rejected item left the prior JSON intact */
    json_builder_appendf(&json, "]}");

    pthread_mutex_unlock(&status_mutex);
}

/* Shared by /api/library/artists, /api/library/album_artists and
 * /api/library/albums below -- counts distinct values of a per-song string
 * field (optionally restricted to songs matching one or two exact filters
 * first, for the albums case), sorts by name, and emits {"name":...,
 * "count":...,"index":...} triples under the given JSON array key. index is
 * the library index of the first song seen with that value -- only the
 * albums list actually uses it (as a representative song to pull /api/art
 * thumbnail bytes from for that album), but it's cheap to always compute so
 * artists/album_artists just carry an unused field rather than needing a
 * second, near-identical function. O(n^2) in the number of distinct values
 * (linear scan to find-or-add each song's value) -- fine for a personal
 * library's artist/album count, and these endpoints are only hit when the
 * phone UI opens a browse view, not every tick. Recomputed per request
 * rather than mirroring gui.c's own artist_groups/album_groups/
 * album_artist_groups -- avoids a second cross-file array whose indexing
 * would have to be kept in sync with remote_control_sync_library()'s own
 * ordering guarantee, for a value (just names + counts + one index) cheap
 * enough to derive fresh every time. Caller already holds status_mutex. */
#define NAME_COUNTS_JSON_MAX 2000

static void build_name_counts_json(const char * const * field, const char * filter_a, const char * filter_b,
                                    const char * json_key, char * out, size_t out_size) {
    char (*names)[128] = malloc(sizeof(char[128]) * NAME_COUNTS_JSON_MAX);
    int * counts = malloc(sizeof(int) * NAME_COUNTS_JSON_MAX);
    int * first_index = malloc(sizeof(int) * NAME_COUNTS_JSON_MAX);
    int distinct = 0;

    for (int i = 0; i < library_count; i++) {
        if (filter_a && filter_a[0] != '\0' && strcasecmp(library_artists[i], filter_a) != 0 &&
            strcasecmp(library_album_artists[i], filter_a) != 0) {
            continue;
        }
        if (filter_b && filter_b[0] != '\0' && strcasecmp(library_albums[i], filter_b) != 0) continue;

        const char * name = field[i];
        int found = -1;
        for (int j = 0; j < distinct; j++) {
            if (strcasecmp(names[j], name) == 0) { found = j; break; }
        }
        if (found >= 0) {
            counts[found]++;
        } else if (distinct < NAME_COUNTS_JSON_MAX) {
            snprintf(names[distinct], sizeof(names[0]), "%s", name);
            counts[distinct] = 1;
            first_index[distinct] = i;
            distinct++;
        }
    }

    /* Simple insertion sort by name -- distinct is bounded by
     * NAME_COUNTS_JSON_MAX, not library_count, so this stays cheap even for
     * a large library. */
    for (int i = 1; i < distinct; i++) {
        char key_name[128];
        snprintf(key_name, sizeof(key_name), "%s", names[i]);
        int key_count = counts[i];
        int key_index = first_index[i];
        int j = i - 1;
        while (j >= 0 && strcasecmp(names[j], key_name) > 0) {
            memcpy(names[j + 1], names[j], sizeof(names[0]));
            counts[j + 1] = counts[j];
            first_index[j + 1] = first_index[j];
            j--;
        }
        snprintf(names[j + 1], sizeof(names[0]), "%s", key_name);
        counts[j + 1] = key_count;
        first_index[j + 1] = key_index;
    }

    json_builder_t json;
    json_builder_init(&json, out, out_size);
    json_builder_appendf(&json, "{\"%s\":[", json_key);
    for (int i = 0; i < distinct && !json.truncated && json.capacity - json.length >= 512; i++) {
        char name_esc[300] = {0};
        json_escape_append(name_esc, sizeof(name_esc), names[i]);
        if (!json_builder_appendf(&json, "%s{\"name\":\"%s\",\"count\":%d,\"index\":%d}",
                                  i > 0 ? "," : "", name_esc, counts[i], first_index[i])) break;
    }
    json.truncated = false;
    json_builder_appendf(&json, "]}");

    free(names);
    free(counts);
    free(first_index);
}

/* These three unfiltered groupings change only when the synchronized
 * library changes. Building them here turns repeated phone browsing from
 * an O(songs x distinct names) request into a bounded string copy. Filtered
 * album drill-downs remain request-specific and are derived below. */
static void refresh_group_json_cache(void) {
    const size_t capacity = 65536;
    if (library_count <= 0) return;
    cached_artists_json = malloc(capacity);
    cached_album_artists_json = malloc(capacity);
    cached_albums_json = malloc(capacity);
    if (cached_artists_json)
        build_name_counts_json((const char * const *) library_artists, NULL, NULL, "artists",
                               cached_artists_json, capacity);
    if (cached_album_artists_json)
        build_name_counts_json((const char * const *) library_album_artists, NULL, NULL, "artists",
                               cached_album_artists_json, capacity);
    if (cached_albums_json)
        build_name_counts_json((const char * const *) library_albums, NULL, NULL, "albums",
                               cached_albums_json, capacity);
}

static void build_artists_json(char * out, size_t out_size) {
    pthread_mutex_lock(&status_mutex);
    if (cached_artists_json) snprintf(out, out_size, "%s", cached_artists_json);
    else snprintf(out, out_size, "{\"artists\":[]}");
    pthread_mutex_unlock(&status_mutex);
}

static void build_album_artists_json(char * out, size_t out_size) {
    pthread_mutex_lock(&status_mutex);
    if (cached_album_artists_json) snprintf(out, out_size, "%s", cached_album_artists_json);
    else snprintf(out, out_size, "{\"artists\":[]}");
    pthread_mutex_unlock(&status_mutex);
}

/* artist_filter and album_artist_filter are mutually exclusive in practice
 * (GET /api/library/albums passes exactly one, matching the browse path the
 * request came from), but build_name_counts_json() treats filter_a as
 * "matches artist OR album_artist" so either works the same way. */
static void build_albums_json(const char * artist_filter, const char * album_artist_filter, char * out,
                               size_t out_size) {
    const char * filter = artist_filter[0] != '\0' ? artist_filter : album_artist_filter;
    pthread_mutex_lock(&status_mutex);
    if (filter[0] == '\0' && cached_albums_json) snprintf(out, out_size, "%s", cached_albums_json);
    else build_name_counts_json((const char * const *) library_albums, filter, NULL, "albums", out, out_size);
    pthread_mutex_unlock(&status_mutex);
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
    playlist_files_scan(MUSIC_ROOT_DIR, &paths, &count);

    size_t len = 0;
    len += (size_t) snprintf(out + len, out_size - len, "{\"playlists\":[");
    for (int i = 0; i < count && len + 300 < out_size; i++) {
        const char * slash = strrchr(paths[i], '/');
        const char * base = slash ? slash + 1 : paths[i];
        char name_esc[300] = {0};
        json_escape_append(name_esc, sizeof(name_esc), base);
        len += (size_t) snprintf(out + len, out_size - len, "%s{\"name\":\"%s\"}", i > 0 ? "," : "", name_esc);
    }
    snprintf(out + len, out_size - len, "]}");

    for (int i = 0; i < count; i++) free(paths[i]);
    free(paths);
}

/* Now Playing page -- polls /api/status every second, transport buttons
 * and a volume slider post to the Phase 2 endpoints below. Plain vanilla
 * HTML/CSS/JS, no framework or build step, matching this project's own
 * "no build tooling beyond what's already here" approach everywhere
 * else. The volume slider deliberately doesn't post on every drag tick
 * (see its own oninput/onchange split) -- only on release, so dragging it
 * doesn't flood the device with a request per pixel of movement. */
static const char * const NOW_PLAYING_HTML =
    "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>Now Playing</title><style>"
    "*{box-sizing:border-box}"
    ":root{--maxw:480px}"
    "body{background:#111;color:#eee;font-family:sans-serif;text-align:center;"
    "padding:clamp(20px,6vw,40px) 16px;margin:0}"
    "h1{font-size:1.4em;margin:0 0 4px}"
    "p{color:#aaa;margin:4px 0}"
    "#art{display:none;width:min(55vw,200px);height:min(55vw,200px);object-fit:cover;border-radius:12px;"
    "margin:0 auto 16px;background:#222}"
    "#bar{background:#333;border-radius:4px;height:6px;margin:24px auto;width:min(92vw,var(--maxw));"
    "overflow:hidden;cursor:pointer}"
    "#fill{background:#4da3ff;height:100%;width:0%}"
    "#time{color:#888;font-size:0.9em}"
    "#controls{margin:28px 0 16px;display:flex;justify-content:center;align-items:center;gap:clamp(12px,4vw,24px)}"
    "#controls button{background:#222;border:1px solid #444;color:#eee;border-radius:50%;"
    "width:clamp(48px,14vw,64px);height:clamp(48px,14vw,64px);display:flex;align-items:center;justify-content:center;"
    "padding:0;cursor:pointer}"
    "#controls button:active{background:#333}"
    "#controls svg{width:44%;height:44%;fill:currentColor}"
    "#volumeRow{display:flex;align-items:center;gap:10px;width:min(92vw,320px);margin:20px auto 0;"
    "color:#888;font-size:0.85em}"
    "#volumeRow input{flex:1}"
    "#search{display:block;width:min(92vw,var(--maxw));margin:24px auto 4px;padding:10px;border-radius:6px;"
    "border:1px solid #444;background:#1a1a1a;color:#eee;font-size:1em}"
    "#libHeader{width:min(92vw,var(--maxw));margin:16px auto 0;display:flex;justify-content:space-between;"
    "align-items:center;padding:8px 4px;color:#4da3ff;cursor:pointer}"
    "#libHeader #libTitle{color:#888;font-size:0.9em;overflow:hidden;text-overflow:ellipsis;"
    "white-space:nowrap;margin-left:12px}"
    "#content{width:min(92vw,var(--maxw));margin:8px auto 0}"
    "#content.menu{display:grid;grid-template-columns:repeat(2,1fr);gap:14px;margin-top:24px}"
    "#content:not(.menu){text-align:left;max-height:360px;overflow-y:auto}"
    ".tile{background:#1a1a1a;border:1px solid #333;border-radius:12px;padding:22px 8px;display:flex;"
    "flex-direction:column;align-items:center;gap:10px;cursor:pointer}"
    ".tile:active{background:#222}"
    ".tile img{width:40px;height:40px}"
    ".tile span{color:#eee;font-size:0.9em}"
    ".row{padding:10px 8px;border-bottom:1px solid #222;display:flex;align-items:center}"
    ".row:active{background:#222}"
    ".row .name{flex:1;color:#eee;cursor:pointer;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}"
    ".row .count{color:#888;font-size:0.85em;padding-left:8px}"
    ".row .thumb{width:40px;height:40px;border-radius:6px;object-fit:cover;margin-right:10px;"
    "background:#222;flex-shrink:0}"
    ".row .info{flex:1;min-width:0;cursor:pointer}"
    ".row .t{color:#eee;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}"
    ".row .a{color:#888;font-size:0.85em;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}"
    ".row .add{color:#4da3ff;font-size:1.4em;padding:0 10px;cursor:pointer}"
    "#more{color:#888;font-size:0.85em;padding:10px;cursor:pointer}"
    "</style></head><body>"
    "<img id=\"art\" alt=\"\">"
    "<h1 id=\"title\">--</h1><p id=\"artist\"></p><p id=\"album\"></p>"
    "<div id=\"bar\"><div id=\"fill\"></div></div><p id=\"time\"></p>"
    "<div id=\"controls\">"
    "<button onclick=\"post('/api/playback/prev')\">"
    "<svg viewBox=\"0 0 24 24\"><path d=\"M6 6h2v12H6zm3.5 6 8.5 6V6z\"/></svg></button>"
    "<button id=\"playbtn\" onclick=\"post('/api/playback/toggle')\">"
    "<svg viewBox=\"0 0 24 24\"><path d=\"M8 5v14l11-7z\"/></svg></button>"
    "<button onclick=\"post('/api/playback/next')\">"
    "<svg viewBox=\"0 0 24 24\"><path d=\"M6 18l8.5-6L6 6v12zM16 6v12h2V6h-2z\"/></svg></button>"
    "<button id=\"modebtn\" onclick=\"post('/api/playback/mode')\">"
    "<svg viewBox=\"0 0 24 24\"><rect x=\"4\" y=\"6\" width=\"16\" height=\"2.4\" rx=\"1.2\"/>"
    "<rect x=\"4\" y=\"11\" width=\"16\" height=\"2.4\" rx=\"1.2\"/>"
    "<rect x=\"4\" y=\"16\" width=\"10\" height=\"2.4\" rx=\"1.2\"/></svg></button>"
    "</div>"
    "<div id=\"volumeRow\"><span>Volume</span><input type=\"range\" id=\"volume\" min=\"0\" max=\"100\" value=\"0\">"
    "</div>"
    "<div id=\"libHeader\" style=\"display:none\">"
    "<span id=\"libBack\">&larr; Back</span><span id=\"libTitle\"></span></div>"
    "<input type=\"text\" id=\"search\" placeholder=\"Search library...\" style=\"display:none\">"
    "<div id=\"content\"></div>"
    "<script>"
    "var view='menu',artistKind=null,artistName=null,libOffset=0,libQuery='',playContext='';"
    "function esc(s){var d=document.createElement('div');d.textContent=s;return d.innerHTML;}"
    "function post(path){fetch(path,{method:'POST'}).catch(()=>{});}"
    "function addToPlaylist(index){"
    "var name=prompt('Add to playlist (existing name adds to it, new name creates it):');"
    "if(!name)return;"
    "fetch('/api/playlists').then(r=>r.json()).then(function(d){"
    "var exists=d.playlists.some(function(p){return p.name===(name+'.m3u');});"
    "var url=(exists?'/api/playlists/add':'/api/playlists')+'?name='+encodeURIComponent(name)+'&index='+index;"
    "fetch(url,{method:'POST'}).then(function(r){return r.text().then(function(t){"
    "alert(r.ok?(exists?'Added to '+name:'Created '+name):('Failed: '+t));});}).catch(()=>{});"
    "}).catch(()=>{});}"
    "function setHeader(visible,title){"
    "document.getElementById('libHeader').style.display=visible?'flex':'none';"
    "document.getElementById('libTitle').textContent=title||'';"
    "document.getElementById('search').style.display='none';}"
    "function renderRows(items,renderFn){"
    "var el=document.getElementById('content');el.className='';el.innerHTML='';"
    "items.forEach(function(item){el.appendChild(renderFn(item));});}"
    "function nameRow(name,count,onClick,thumbIndex){"
    "var row=document.createElement('div');row.className='row';"
    "if(thumbIndex!==undefined){"
    "var thumb=document.createElement('img');thumb.className='thumb';thumb.loading='lazy';thumb.alt='';"
    "thumb.onerror=function(){thumb.style.visibility='hidden';};"
    "thumb.src='/api/art?index='+thumbIndex;"
    "row.appendChild(thumb);}"
    "var nm=document.createElement('div');nm.className='name';nm.textContent=name;"
    "row.appendChild(nm);"
    "if(count!==undefined){var c=document.createElement('div');c.className='count';c.textContent=count;"
    "row.appendChild(c);}"
    "row.onclick=onClick;return row;}"
    "function songRow(s){"
    "var row=document.createElement('div');row.className='row';"
    "var thumb=document.createElement('img');thumb.className='thumb';thumb.loading='lazy';thumb.alt='';"
    "thumb.onerror=function(){thumb.style.visibility='hidden';};"
    "thumb.src='/api/art?index='+s.index;"
    "var info=document.createElement('div');info.className='info';"
    "info.innerHTML='<div class=\"t\">'+esc(s.title)+'</div><div class=\"a\">'+esc(s.artist)+'</div>';"
    "info.onclick=function(){post('/api/playback/play?index='+s.index+playContext);};"
    "var add=document.createElement('div');add.className='add';add.textContent='+';"
    "add.onclick=function(e){e.stopPropagation();addToPlaylist(s.index);};"
    "row.appendChild(thumb);row.appendChild(info);row.appendChild(add);return row;}"
    "var MENU_ITEMS=["
    "{icon:'all',label:'All Songs',onclick:function(){showAllSongs();}},"
    "{icon:'artist',label:'Artists',onclick:function(){showArtists();}},"
    "{icon:'album_artist',label:'Album Artists',onclick:function(){showAlbumArtists();}},"
    "{icon:'genre',label:'Playlists',onclick:function(){showPlaylists();}}];"
    "function showMenu(){"
    "view='menu';setHeader(false);"
    "var el=document.getElementById('content');el.className='menu';el.innerHTML='';"
    "MENU_ITEMS.forEach(function(m){"
    "var tile=document.createElement('div');tile.className='tile';"
    "var img=document.createElement('img');img.src='/assets/icon?name='+m.icon;img.alt='';"
    "var label=document.createElement('span');label.textContent=m.label;"
    "tile.appendChild(img);tile.appendChild(label);tile.onclick=m.onclick;"
    "el.appendChild(tile);});}"
    "function showArtists(){"
    "view='artists';setHeader(true,'Artists');"
    "fetch('/api/library/artists').then(r=>r.json()).then(function(d){"
    "renderRows(d.artists,function(a){return nameRow(a.name,a.count,function(){showArtistAlbums('artist',a.name);});"
    "});}).catch(()=>{});}"
    "function showAlbumArtists(){"
    "view='album_artists';setHeader(true,'Album Artists');"
    "fetch('/api/library/album_artists').then(r=>r.json()).then(function(d){"
    "renderRows(d.artists,function(a){return nameRow(a.name,a.count,"
    "function(){showArtistAlbums('album_artist',a.name);});});}).catch(()=>{});}"
    "function showPlaylists(){"
    "view='playlists';setHeader(true,'Playlists');"
    "fetch('/api/playlists').then(r=>r.json()).then(function(d){"
    "renderRows(d.playlists,function(p){var n=p.name.replace(/\\.m3u8?$/i,'');"
    "return nameRow(n,undefined,function(){showPlaylistSongs(n);});});}).catch(()=>{});}"
    "function showArtistAlbums(kind,name){"
    "view='artist_albums';artistKind=kind;artistName=name;setHeader(true,name);"
    "var url='/api/library/albums?'+(kind==='artist'?'artist=':'album_artist=')+encodeURIComponent(name);"
    "fetch(url).then(r=>r.json()).then(function(d){"
    "renderRows(d.albums,function(a){return nameRow(a.name,a.count,function(){showAlbumSongs(a.name);},a.index);});"
    "}).catch(()=>{});}"
    "function showAlbumSongs(album){"
    "view='album_songs';setHeader(true,album);"
    "var kindParam=(artistKind==='artist'?'artist=':'album_artist=')+encodeURIComponent(artistName);"
    "playContext='&'+kindParam+'&album='+encodeURIComponent(album);"
    "var url='/api/library?limit=200&'+kindParam+'&album='+encodeURIComponent(album);"
    "fetch(url).then(r=>r.json()).then(function(d){renderRows(d.songs,songRow);}).catch(()=>{});}"
    "function showPlaylistSongs(name){"
    "view='playlist_songs';setHeader(true,name);"
    "playContext='&playlist='+encodeURIComponent(name);"
    "fetch('/api/playlists/songs?name='+encodeURIComponent(name)).then(r=>r.json()).then(function(d){"
    "renderRows(d.songs,songRow);}).catch(()=>{});}"
    "function showAllSongs(){"
    "view='all_songs';setHeader(true,'All Songs');playContext='';"
    "var s=document.getElementById('search');s.style.display='block';s.value='';libQuery='';"
    "loadLibrary(true);}"
    "function loadLibrary(reset){"
    "if(reset){libOffset=0;var c=document.getElementById('content');c.className='';c.innerHTML='';}"
    "var url='/api/library?offset='+libOffset+'&limit=50&q='+encodeURIComponent(libQuery);"
    "fetch(url).then(r=>r.json()).then(d=>{"
    "var el=document.getElementById('content');"
    "var old=document.getElementById('more');if(old)old.remove();"
    "d.songs.forEach(function(s){el.appendChild(songRow(s));});"
    "libOffset+=d.songs.length;"
    "if(libOffset<d.total){"
    "var more=document.createElement('div');more.id='more';more.textContent='Load more ('+(d.total-libOffset)+' more)';"
    "more.onclick=function(){loadLibrary(false);};el.appendChild(more);}"
    "}).catch(()=>{});}"
    "document.getElementById('libBack').onclick=function(){"
    "if(view==='artist_albums'){if(artistKind==='artist')showArtists();else showAlbumArtists();}"
    "else if(view==='album_songs'){showArtistAlbums(artistKind,artistName);}"
    "else if(view==='playlist_songs'){showPlaylists();}"
    "else{showMenu();}};"
    "var searchTimer=null;"
    "document.getElementById('search').addEventListener('input',function(){"
    "libQuery=this.value;clearTimeout(searchTimer);searchTimer=setTimeout(function(){loadLibrary(true);},300);});"
    "showMenu();"
    "</script>"
    "<script>"
    "var dragging=false,lastArtKey='';"
    "var ICON_PLAY='<svg viewBox=\"0 0 24 24\"><path d=\"M8 5v14l11-7z\"/></svg>';"
    "var ICON_PAUSE='<svg viewBox=\"0 0 24 24\"><path d=\"M6 5h4v14H6zM14 5h4v14h-4z\"/></svg>';"
    /* Same one-button cycle as the player screen's own order_icon
     * (Sequential -> Repeat All -> Repeat One -> Shuffle), indexed by
     * play_mode_t's own int values -- see remote_control.h's own comment
     * on remote_control_notify_status()'s play_mode param. Repeat All/One
     * share a circle-with-gap-plus-arrowhead glyph (stroke-dasharray
     * leaves a small opening, the polygon caps it with an arrowhead);
     * Repeat One adds a "1". Shuffle is a plain crossing-lines glyph. */
    "var MODE_ICONS=["
    "'<svg viewBox=\"0 0 24 24\"><rect x=\"4\" y=\"6\" width=\"16\" height=\"2.4\" rx=\"1.2\"/>"
    "<rect x=\"4\" y=\"11\" width=\"16\" height=\"2.4\" rx=\"1.2\"/>"
    "<rect x=\"4\" y=\"16\" width=\"10\" height=\"2.4\" rx=\"1.2\"/></svg>',"
    "'<svg viewBox=\"0 0 24 24\"><circle cx=\"12\" cy=\"12\" r=\"8\" fill=\"none\" stroke=\"currentColor\" "
    "stroke-width=\"2\" stroke-dasharray=\"43 7\"/><polygon points=\"12,3 16,7 12,7\"/></svg>',"
    "'<svg viewBox=\"0 0 24 24\"><circle cx=\"12\" cy=\"12\" r=\"8\" fill=\"none\" stroke=\"currentColor\" "
    "stroke-width=\"2\" stroke-dasharray=\"43 7\"/><polygon points=\"12,3 16,7 12,7\"/>"
    "<text x=\"12\" y=\"16\" font-size=\"8\" text-anchor=\"middle\">1</text></svg>',"
    "'<svg viewBox=\"0 0 24 24\"><path d=\"M4 6l16 12M20 6 4 18\" stroke=\"currentColor\" "
    "stroke-width=\"2.2\" fill=\"none\" stroke-linecap=\"round\"/></svg>'];"
    "function fmt(s){s=Math.max(0,s|0);var m=(s/60)|0,r=s%60;return m+':'+(r<10?'0':'')+r;}"
    "function poll(){fetch('/api/status').then(r=>r.json()).then(d=>{"
    "document.getElementById('title').textContent=d.title||'(nothing playing)';"
    "document.getElementById('artist').textContent=d.artist||'';"
    "document.getElementById('album').textContent=d.album||'';"
    "window.lastDuration=d.duration_seconds;"
    "var pct=d.duration_seconds>0?(100*d.position_seconds/d.duration_seconds):0;"
    "document.getElementById('fill').style.width=pct+'%';"
    "document.getElementById('time').textContent=fmt(d.position_seconds)+' / '+fmt(d.duration_seconds);"
    "document.getElementById('playbtn').innerHTML=d.playing?ICON_PAUSE:ICON_PLAY;"
    "document.getElementById('modebtn').innerHTML=MODE_ICONS[d.play_mode]||MODE_ICONS[0];"
    "if(!dragging)document.getElementById('volume').value=Math.round(d.volume*100);"
    "var artKey=(d.title||'')+'|'+(d.artist||'');"
    "if(artKey!==lastArtKey){lastArtKey=artKey;var art=document.getElementById('art');"
    "if(d.title){art.onload=function(){art.style.display='block';};"
    "art.onerror=function(){art.style.display='none';};"
    "art.src='/api/art?t='+Date.now();}else{art.style.display='none';art.removeAttribute('src');}}"
    "}).catch(()=>{});}"
    "document.getElementById('bar').addEventListener('click',function(e){"
    "var r=this.getBoundingClientRect();var frac=(e.clientX-r.left)/r.width;"
    "fetch('/api/playback/seek?seconds='+Math.round(frac*(window.lastDuration||0)),{method:'POST'});});"
    "var vol=document.getElementById('volume');"
    "vol.addEventListener('input',function(){dragging=true;});"
    "vol.addEventListener('change',function(){dragging=false;post('/api/playback/volume?percent='+this.value);});"
    "poll();setInterval(poll,1000);"
    "</script></body></html>";

static void send_all(int fd, const char * data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = write(fd, data + sent, len - sent);
        if (n <= 0) return;
        sent += (size_t) n;
    }
}

/* Real-device incident: the play/pause button on the Now Playing page
 * didn't update after tapping a song from the library search results --
 * it only "caught up" once the user tapped the button itself. Root
 * cause: this response never sent any cache-control headers, and a phone
 * browser (unlike curl, which was used to verify the backend itself
 * responds correctly within 2s of a play request) caches GET responses
 * by default -- the page's own poll() was very likely re-displaying a
 * stale cached /api/status response instead of actually re-fetching it
 * every second. Every response from this server is dynamic and must
 * never be cached, including the static HTML/JS itself (a stale cached
 * copy across app updates would be its own, separate confusing bug). */
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

/* Pulls an integer query parameter (e.g. "seconds" out of
 * "/api/playback/seek?seconds=42") straight out of the request path --
 * this server never needs a request body, every Phase 2 action fits in
 * the URL, so there's no reason to also parse Content-Length/read a body
 * (one less thing that can be malformed). Returns false if the key isn't
 * present or has no valid integer value. */
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

/* GET /api/art (optionally ?index=N) -- serves a song's embedded cover art
 * as a raw image, re-extracted straight from the file on disk via the same
 * metadata_read() gui.c itself uses for cover_decode.c's LVGL image (not a
 * copy of gui.c's already-decoded pixel buffer -- simplest way to get
 * still-encoded JPEG/PNG bytes suitable for an HTTP image response is to
 * just read the file's own embedded picture again, not re-serialize a
 * decoded LVGL image back into a compressed format). Runs on the HTTP
 * thread directly (file I/O, no LVGL/audio state), same justification as
 * playlist mutation's own file I/O in handle_connection() -- the lookup of
 * which path to read is done under a short status_mutex lock, then
 * unlocked before the (slower) metadata_read() call. Resolves the path
 * either from a library index or (no index given) status_path, the
 * currently-playing file remote_control_notify_status() was last called
 * with. */
static bool resolve_art_source_path(bool have_index, int index, char * out_path, size_t out_path_size) {
    pthread_mutex_lock(&status_mutex);
    bool ok = false;
    if (have_index) {
        if (index >= 0 && index < library_count) {
            snprintf(out_path, out_path_size, "%s", library_paths[index]);
            ok = true;
        }
    } else if (status_path[0] != '\0') {
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
    int index = -1;
    bool have_index = query_param_int(path, "index", &index);

    char song_path[512] = {0};
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
    char name[128] = {0};
    if (!query_param_str(path, "name", name, sizeof(name)) || !playlist_name_is_safe(name)) {
        send_response(cfd, "400 Bad Request", "text/plain", "Bad Request");
        return;
    }

    char m3u_path[512];
    snprintf(m3u_path, sizeof(m3u_path), "%s/%s.m3u", PLAYLISTS_DIR, name);

    char ** paths = NULL;
    int count = 0;
    if (!playlist_files_read(m3u_path, &paths, &count)) {
        send_response(cfd, "404 Not Found", "text/plain", "Playlist not found");
        return;
    }

    char * json = malloc(65536);
    if (!json) {
        for (int i = 0; i < count; i++) free(paths[i]);
        free(paths);
        send_response(cfd, "500 Internal Server Error", "text/plain", "Out of memory");
        return;
    }

    pthread_mutex_lock(&status_mutex);
    size_t len = 0;
    len += (size_t) snprintf(json + len, 65536 - len, "{\"songs\":[");
    int emitted = 0;
    for (int i = 0; i < count && len + 256 < 65536; i++) {
        int found = -1;
        for (int j = 0; j < library_count; j++) {
            if (strcmp(library_paths[j], paths[i]) == 0) { found = j; break; }
        }
        if (found < 0) continue;

        char title_esc[300] = {0}, artist_esc[300] = {0};
        json_escape_append(title_esc, sizeof(title_esc), library_titles[found]);
        json_escape_append(artist_esc, sizeof(artist_esc), library_artists[found]);
        len += (size_t) snprintf(json + len, 65536 - len, "%s{\"index\":%d,\"title\":\"%s\",\"artist\":\"%s\"}",
                                  emitted > 0 ? "," : "", found, title_esc, artist_esc);
        emitted++;
    }
    snprintf(json + len, 65536 - len, "]}");
    pthread_mutex_unlock(&status_mutex);

    send_response(cfd, "200 OK", "application/json", json);

    free(json);
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
        } else if (strcmp(path_only, "/assets/icon") == 0) {
            handle_icon_request(cfd, path);
        } else if (strcmp(path_only, "/") == 0) {
            send_response(cfd, "200 OK", "text/html", NOW_PLAYING_HTML);
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
            int index = -1;
            bool have_name = query_param_str(path, "name", name, sizeof(name));
            bool have_index = query_param_int(path, "index", &index);

            char song_path[512] = {0};
            bool have_song = false;
            if (have_index) {
                pthread_mutex_lock(&status_mutex);
                if (index >= 0 && index < library_count) {
                    snprintf(song_path, sizeof(song_path), "%s", library_paths[index]);
                    have_song = true;
                }
                pthread_mutex_unlock(&status_mutex);
            }

            if (!have_name || !playlist_name_is_safe(name) || !have_song) {
                send_response(cfd, "400 Bad Request", "text/plain", "Bad Request");
            } else if (strcmp(path_only, "/api/playlists") == 0) {
                char created_path[512];
                bool created = playlist_files_create(PLAYLISTS_DIR, name, song_path, created_path, sizeof(created_path));
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
                int index;
                if (query_param_int(path, "index", &index) && index >= 0 && index < library_count) {
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

/* Real-device incident: this used to call a plain blocking accept() and
 * rely on remote_control_stop() closing listen_fd to unblock it (the same
 * pattern dlna_control_stop() uses for its own AF_UNIX listener) -- on
 * this device's kernel (4.4.94), closing a socket from another thread does
 * NOT reliably wake a thread already blocked in accept() on it for a plain
 * AF_INET/SOCK_STREAM socket. Confirmed live: toggling Remote Control off
 * froze the whole app -- the main/UI thread blocked forever in
 * pthread_join() waiting for this thread to exit, while this thread sat
 * forever in accept(), having never noticed the socket was closed. Fixed
 * by polling with a bounded timeout instead of blocking directly in
 * accept(), so this thread notices `running` went false within one poll
 * interval on its own, independent of whatever close()-from-another-
 * thread does or doesn't do on this kernel. */
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

    /* The listener thread notices `running` went false on its own, within
     * one ACCEPT_POLL_TIMEOUT_MS poll cycle (see listener_thread_func()'s
     * own comment on why this doesn't rely on closing listen_fd to
     * interrupt a blocking accept() the way dlna_control_stop() does for
     * its own listener) -- so this join is bounded to ~200ms, not
     * indefinite. Only close the fd after the thread has actually exited
     * and is guaranteed to no longer touch it. */
    pthread_join(listener_thread, NULL);
    if (listen_fd >= 0) {
        close(listen_fd);
        listen_fd = -1;
    }
}
