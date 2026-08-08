#include "subsonic_client.h"
#include "http_client.h"

#include "mbedtls/md5.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SUBSONIC_API_VERSION "1.16.1"
#define SUBSONIC_CLIENT_NAME "open_hiby_player"

static void url_encode(const char * in, char * out, size_t out_size) {
    static const char hex[] = "0123456789ABCDEF";
    size_t pos = 0;
    for (const unsigned char * p = (const unsigned char *) in; *p && pos + 4 < out_size; p++) {
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
            *p == '-' || *p == '_' || *p == '.' || *p == '~') {
            out[pos++] = (char) *p;
        } else {
            out[pos++] = '%';
            out[pos++] = hex[*p >> 4];
            out[pos++] = hex[*p & 0xF];
        }
    }
    out[pos] = '\0';
}

static void md5_hex(const char * data, size_t len, char out_hex[33]) {
    unsigned char digest[16];
    mbedtls_md5((const unsigned char *) data, len, digest);
    for (int i = 0; i < 16; i++) snprintf(out_hex + i * 2, 3, "%02x", digest[i]);
}

/* Not cryptographically strong, but the Subsonic token scheme only needs a
 * value that's different per request (so a captured token can't be
 * replayed indefinitely) -- srand() is seeded once from the current time
 * the first time this runs. */
static void random_salt(char * out, size_t out_size) {
    static bool seeded = false;
    if (!seeded) { srand((unsigned int) time(NULL)); seeded = true; }

    static const char alphabet[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    size_t n = out_size - 1;
    for (size_t i = 0; i < n; i++) out[i] = alphabet[rand() % (sizeof(alphabet) - 1)];
    out[n] = '\0';
}

/* u/t/s/v/c/f -- every Subsonic API call needs these six params. Token-based
 * auth (t=md5(password+salt), s=salt) is used instead of the deprecated
 * plain p=password scheme, so the password itself never goes on the wire. */
static void build_auth_query(const subsonic_server_t * server, char * out, size_t out_size) {
    char salt[17];
    random_salt(salt, sizeof(salt));

    char token_input[128 + sizeof(salt)];
    snprintf(token_input, sizeof(token_input), "%s%s", server->password, salt);
    char token[33];
    md5_hex(token_input, strlen(token_input), token);

    char user_enc[384];
    url_encode(server->username, user_enc, sizeof(user_enc));

    snprintf(out, out_size, "u=%s&t=%s&s=%s&v=%s&c=%s&f=json",
              user_enc, token, salt, SUBSONIC_API_VERSION, SUBSONIC_CLIENT_NAME);
}

/* Calls endpoint (e.g. "ping.view"), with extra_params (already
 * URL-encoded, may be NULL) appended alongside the auth params, and parses
 * the JSON response. Returns the "subsonic-response" object (still owned
 * by *out_root -- caller must cJSON_Delete(*out_root) when done with it)
 * on a status:"ok" response, or NULL on any network/parse/auth failure. */
static cJSON * api_request(const subsonic_server_t * server, const char * endpoint, const char * extra_params, cJSON ** out_root) {
    char auth[512];
    build_auth_query(server, auth, sizeof(auth));

    char url[1536];
    if (extra_params) {
        snprintf(url, sizeof(url), "%s/rest/%s?%s&%s", server->base_url, endpoint, auth, extra_params);
    } else {
        snprintf(url, sizeof(url), "%s/rest/%s?%s", server->base_url, endpoint, auth);
    }

    int status = 0;
    uint8_t * body = NULL;
    size_t body_size = 0;
    bool ok = http_get_to_buffer(url, server->verify_tls, &status, &body, &body_size);
    if (!ok || status != 200 || !body) {
        free(body);
        return NULL;
    }

    cJSON * root = cJSON_ParseWithLength((const char *) body, body_size);
    free(body);
    if (!root) return NULL;

    cJSON * resp = cJSON_GetObjectItemCaseSensitive(root, "subsonic-response");
    if (!resp) { cJSON_Delete(root); return NULL; }

    cJSON * status_item = cJSON_GetObjectItemCaseSensitive(resp, "status");
    if (!cJSON_IsString(status_item) || strcmp(status_item->valuestring, "ok") != 0) {
        cJSON_Delete(root);
        return NULL;
    }

    *out_root = root;
    return resp;
}

static const char * json_str(cJSON * obj, const char * key, const char * fallback) {
    cJSON * item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(item) ? item->valuestring : fallback;
}

static int json_int(cJSON * obj, const char * key, int fallback) {
    cJSON * item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(item) ? item->valueint : fallback;
}

bool subsonic_ping(const subsonic_server_t * server) {
    cJSON * root = NULL;
    cJSON * resp = api_request(server, "ping.view", NULL, &root);
    bool ok = (resp != NULL);
    if (root) cJSON_Delete(root);
    return ok;
}

bool subsonic_get_artists(const subsonic_server_t * server, subsonic_artist_t ** out_artists, int * out_count) {
    *out_artists = NULL;
    *out_count = 0;

    cJSON * root = NULL;
    cJSON * resp = api_request(server, "getArtists.view", NULL, &root);
    if (!resp) return false;

    cJSON * artists_obj = cJSON_GetObjectItemCaseSensitive(resp, "artists");
    cJSON * index_arr = cJSON_GetObjectItemCaseSensitive(artists_obj, "index");
    if (!cJSON_IsArray(index_arr)) { cJSON_Delete(root); return false; }

    int capacity = 0;
    int count = 0;
    subsonic_artist_t * artists = NULL;

    cJSON * index_item;
    cJSON_ArrayForEach(index_item, index_arr) {
        cJSON * artist_arr = cJSON_GetObjectItemCaseSensitive(index_item, "artist");
        if (!cJSON_IsArray(artist_arr)) continue;

        cJSON * artist_item;
        cJSON_ArrayForEach(artist_item, artist_arr) {
            if (count == capacity) {
                capacity = capacity ? capacity * 2 : 32;
                artists = realloc(artists, sizeof(subsonic_artist_t) * (size_t) capacity);
            }
            snprintf(artists[count].id, sizeof(artists[count].id), "%s", json_str(artist_item, "id", ""));
            snprintf(artists[count].name, sizeof(artists[count].name), "%s", json_str(artist_item, "name", "(unknown)"));
            count++;
        }
    }

    cJSON_Delete(root);
    *out_artists = artists;
    *out_count = count;
    return count > 0;
}

bool subsonic_get_artist_albums(const subsonic_server_t * server, const char * artist_id,
                                 subsonic_album_t ** out_albums, int * out_count) {
    *out_albums = NULL;
    *out_count = 0;

    char id_enc[256];
    url_encode(artist_id, id_enc, sizeof(id_enc));
    char params[300];
    snprintf(params, sizeof(params), "id=%s", id_enc);

    cJSON * root = NULL;
    cJSON * resp = api_request(server, "getArtist.view", params, &root);
    if (!resp) return false;

    cJSON * artist_obj = cJSON_GetObjectItemCaseSensitive(resp, "artist");
    cJSON * album_arr = cJSON_GetObjectItemCaseSensitive(artist_obj, "album");
    if (!cJSON_IsArray(album_arr)) { cJSON_Delete(root); return false; }

    int count = cJSON_GetArraySize(album_arr);
    subsonic_album_t * albums = malloc(sizeof(subsonic_album_t) * (size_t) (count > 0 ? count : 1));

    int i = 0;
    cJSON * album_item;
    cJSON_ArrayForEach(album_item, album_arr) {
        snprintf(albums[i].id, sizeof(albums[i].id), "%s", json_str(album_item, "id", ""));
        snprintf(albums[i].name, sizeof(albums[i].name), "%s", json_str(album_item, "name", "(unknown)"));
        snprintf(albums[i].artist, sizeof(albums[i].artist), "%s", json_str(album_item, "artist", ""));
        i++;
    }

    cJSON_Delete(root);
    *out_albums = albums;
    *out_count = count;
    return count > 0;
}

bool subsonic_get_album_songs(const subsonic_server_t * server, const char * album_id,
                               subsonic_song_t ** out_songs, int * out_count) {
    *out_songs = NULL;
    *out_count = 0;

    char id_enc[256];
    url_encode(album_id, id_enc, sizeof(id_enc));
    char params[300];
    snprintf(params, sizeof(params), "id=%s", id_enc);

    cJSON * root = NULL;
    cJSON * resp = api_request(server, "getAlbum.view", params, &root);
    if (!resp) return false;

    cJSON * album_obj = cJSON_GetObjectItemCaseSensitive(resp, "album");
    cJSON * song_arr = cJSON_GetObjectItemCaseSensitive(album_obj, "song");
    if (!cJSON_IsArray(song_arr)) { cJSON_Delete(root); return false; }

    int count = cJSON_GetArraySize(song_arr);
    subsonic_song_t * songs = malloc(sizeof(subsonic_song_t) * (size_t) (count > 0 ? count : 1));

    int i = 0;
    cJSON * song_item;
    cJSON_ArrayForEach(song_item, song_arr) {
        snprintf(songs[i].id, sizeof(songs[i].id), "%s", json_str(song_item, "id", ""));
        snprintf(songs[i].title, sizeof(songs[i].title), "%s", json_str(song_item, "title", "(unknown)"));
        snprintf(songs[i].artist, sizeof(songs[i].artist), "%s", json_str(song_item, "artist", ""));
        snprintf(songs[i].suffix, sizeof(songs[i].suffix), "%s", json_str(song_item, "suffix", "mp3"));
        songs[i].track = json_int(song_item, "track", 0);
        songs[i].duration_seconds = json_int(song_item, "duration", 0);
        i++;
    }

    cJSON_Delete(root);
    *out_songs = songs;
    *out_count = count;
    return count > 0;
}

void subsonic_build_stream_url(const subsonic_server_t * server, const char * song_id, char * out_url, size_t out_url_size) {
    char auth[512];
    build_auth_query(server, auth, sizeof(auth));

    char id_enc[256];
    url_encode(song_id, id_enc, sizeof(id_enc));

    snprintf(out_url, out_url_size, "%s/rest/stream.view?%s&id=%s", server->base_url, auth, id_enc);
}
