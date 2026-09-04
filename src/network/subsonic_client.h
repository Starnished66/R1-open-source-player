#ifndef SUBSONIC_CLIENT_H
#define SUBSONIC_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include "http_client.h"

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
    char album[128]; /* used to lay out downloaded files as Artist/Album/Song -- see subsonic_download_to_library() */
    char suffix[16]; /* file extension (no dot), e.g. "mp3"/"flac" -- the
                       * downloaded temp file needs this so the existing
                       * decoder dispatch (by extension) picks the right one */
    char cover_art[64]; /* getCoverArt.view's own id param -- a DIFFERENT
                          * value from id above (confirmed against a real
                          * server, not assumed: id="7yjzu1an..." vs.
                          * cover_art="mf-7yjzu1an..._212a3c00" for the same
                          * song), empty if the server didn't send one */
    int track;
    int disc;
    int duration_seconds;
    unsigned int sample_rate;
    unsigned int bit_depth;
    unsigned int channels;
    unsigned int bitrate_kbps;
} subsonic_song_t;

typedef struct {
    char id[64];
    char name[128];
} subsonic_playlist_t;

/* Checks the server is reachable and the credentials are accepted
 * (ping.view). Doesn't distinguish *why* it failed (network vs auth) --
 * that's a reasonable amount of detail for a first "Test Connection"
 * button; a real error message would need threading a string back, which
 * isn't done here. */
bool subsonic_ping(const subsonic_server_t * server, http_cancel_token_t * cancel);

/* getArtists.view, flattened out of its "index by first letter" grouping
 * into one array sorted the way the server already returns it (which is
 * alphabetical) -- the local library's own Artists screen doesn't group by
 * letter either, so this matches that for a consistent feel. Caller owns
 * *out_artists (free() the array itself; no nested allocations). */
bool subsonic_get_artists(const subsonic_server_t * server, subsonic_artist_t ** out_artists, int * out_count,
                           http_cancel_token_t * cancel);

/* getArtist.view -- that artist's albums. */
bool subsonic_get_artist_albums(const subsonic_server_t * server, const char * artist_id,
                                 subsonic_album_t ** out_albums, int * out_count, http_cancel_token_t * cancel);

/* getAlbum.view -- that album's songs, in the order the server returns
 * them (already track-ordered). */
bool subsonic_get_album_songs(const subsonic_server_t * server, const char * album_id,
                               subsonic_song_t ** out_songs, int * out_count, http_cancel_token_t * cancel);

/* getAlbumList2.view (type=alphabeticalByName) -- every album in the whole
 * library, not scoped to one artist (unlike subsonic_get_artist_albums()
 * above). Capped at 500 results (the Subsonic API's own per-call max, no
 * paging implemented here) -- reasonable for the libraries this app has
 * been tested against, but a genuinely huge server library could be
 * truncated; not expected to matter in practice for a portable player's
 * use case. Caller owns *out_albums. */
bool subsonic_get_all_albums(const subsonic_server_t * server, subsonic_album_t ** out_albums, int * out_count,
                              http_cancel_token_t * cancel);

/* getPlaylists.view -- every playlist visible to this user. Caller owns
 * *out_playlists. */
bool subsonic_get_playlists(const subsonic_server_t * server, subsonic_playlist_t ** out_playlists, int * out_count,
                             http_cancel_token_t * cancel);

/* getPlaylist.view -- that playlist's songs, in playlist order. Same
 * subsonic_song_t shape as subsonic_get_album_songs() (a playlist entry and
 * an album's song are the same "Child" element in the Subsonic API). */
bool subsonic_get_playlist_songs(const subsonic_server_t * server, const char * playlist_id,
                                  subsonic_song_t ** out_songs, int * out_count, http_cancel_token_t * cancel);

/* Builds the full stream.view URL (auth params included) for song_id.
 * mp3/flac songs get played directly against this URL (see decoder_open()
 * in audio.c); every other format is still downloaded first via
 * http_get_to_file() before playback -- see subsonic_song_row_click_cb()
 * in gui.c for which path a given song takes. */
void subsonic_build_stream_url(const subsonic_server_t * server, const char * song_id, char * out_url, size_t out_url_size);

/* Builds the full getCoverArt.view URL (auth params included) for
 * cover_art_id (subsonic_song_t.cover_art, NOT the song's own id -- these
 * are different values, see that field's own comment). Returns real
 * JPEG/PNG image bytes when fetched (http_get_to_buffer()), suitable for
 * the same cover_decode_to_rgb565() path local files' embedded art already
 * goes through. */
void subsonic_build_cover_art_url(const subsonic_server_t * server, const char * cover_art_id, char * out_url, size_t out_url_size);

#endif /* SUBSONIC_CLIENT_H */
