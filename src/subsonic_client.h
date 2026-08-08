#ifndef SUBSONIC_CLIENT_H
#define SUBSONIC_CLIENT_H

#include <stdbool.h>
#include <stddef.h>

/* Connection details for one configured server. verify_tls false skips
 * HTTPS certificate verification -- an explicit, per-server opt-in for
 * self-signed servers (very common for a home-hosted Subsonic/Navidrome/
 * Airsonic instance), never a silent default; see http_client.h. */
typedef struct {
    char base_url[256];  /* e.g. "https://music.example.com:4040", no trailing slash, no "/rest" */
    char username[128];
    char password[128];
    bool verify_tls;
} subsonic_server_t;

typedef struct {
    char id[64];
    char name[128];
} subsonic_artist_t;

typedef struct {
    char id[64];
    char name[128];
    char artist[128];
} subsonic_album_t;

typedef struct {
    char id[64];
    char title[128];
    char artist[128];
    char suffix[16]; /* file extension (no dot), e.g. "mp3"/"flac" -- the
                       * downloaded temp file needs this so the existing
                       * decoder dispatch (by extension) picks the right one */
    int track;
    int duration_seconds;
} subsonic_song_t;

/* Checks the server is reachable and the credentials are accepted
 * (ping.view). Doesn't distinguish *why* it failed (network vs auth) --
 * that's a reasonable amount of detail for a first "Test Connection"
 * button; a real error message would need threading a string back, which
 * isn't done here. */
bool subsonic_ping(const subsonic_server_t * server);

/* getArtists.view, flattened out of its "index by first letter" grouping
 * into one array sorted the way the server already returns it (which is
 * alphabetical) -- the local library's own Artists screen doesn't group by
 * letter either, so this matches that for a consistent feel. Caller owns
 * *out_artists (free() the array itself; no nested allocations). */
bool subsonic_get_artists(const subsonic_server_t * server, subsonic_artist_t ** out_artists, int * out_count);

/* getArtist.view -- that artist's albums. */
bool subsonic_get_artist_albums(const subsonic_server_t * server, const char * artist_id,
                                 subsonic_album_t ** out_albums, int * out_count);

/* getAlbum.view -- that album's songs, in the order the server returns
 * them (already track-ordered). */
bool subsonic_get_album_songs(const subsonic_server_t * server, const char * album_id,
                               subsonic_song_t ** out_songs, int * out_count);

/* Builds the full stream.view URL (auth params included) for song_id --
 * pass straight to http_get_to_file() to download it before playback (see
 * README for why this project downloads-then-plays rather than decoding
 * a true network stream). */
void subsonic_build_stream_url(const subsonic_server_t * server, const char * song_id, char * out_url, size_t out_url_size);

#endif /* SUBSONIC_CLIENT_H */
