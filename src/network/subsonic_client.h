#ifndef SUBSONIC_CLIENT_H
#define SUBSONIC_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include "http_client.h"

/* Connection details for one configured server. verify_tls controls HTTPS
 * certificate verification. */
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
    char album[128];
    char suffix[16];    /* File extension without leading dot (e.g. "mp3", "flac") */
    char cover_art[64]; /* getCoverArt.view id parameter, empty if omitted */
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

/* Checks server reachability and credentials via ping.view. */
bool subsonic_ping(const subsonic_server_t * server, http_cancel_token_t * cancel);

/* Fetches all artists via getArtists.view. Caller owns *out_artists. */
bool subsonic_get_artists(const subsonic_server_t * server, subsonic_artist_t ** out_artists, int * out_count,
                           http_cancel_token_t * cancel);

/* Fetches albums for an artist via getArtist.view. Caller owns *out_albums. */
bool subsonic_get_artist_albums(const subsonic_server_t * server, const char * artist_id,
                                 subsonic_album_t ** out_albums, int * out_count, http_cancel_token_t * cancel);

/* Fetches songs for an album via getAlbum.view. Caller owns *out_songs. */
bool subsonic_get_album_songs(const subsonic_server_t * server, const char * album_id,
                               subsonic_song_t ** out_songs, int * out_count, http_cancel_token_t * cancel);

/* Fetches albums alphabetically via getAlbumList2.view (capped at 500). Caller owns *out_albums. */
bool subsonic_get_all_albums(const subsonic_server_t * server, subsonic_album_t ** out_albums, int * out_count,
                              http_cancel_token_t * cancel);

/* Fetches playlists via getPlaylists.view. Caller owns *out_playlists. */
bool subsonic_get_playlists(const subsonic_server_t * server, subsonic_playlist_t ** out_playlists, int * out_count,
                             http_cancel_token_t * cancel);

/* Fetches songs for a playlist via getPlaylist.view. Caller owns *out_songs. */
bool subsonic_get_playlist_songs(const subsonic_server_t * server, const char * playlist_id,
                                  subsonic_song_t ** out_songs, int * out_count, http_cancel_token_t * cancel);

/* Builds the stream.view URL including auth parameters for song_id. */
void subsonic_build_stream_url(const subsonic_server_t * server, const char * song_id, char * out_url, size_t out_url_size);

/* Builds the getCoverArt.view URL including auth parameters for cover_art_id. */
void subsonic_build_cover_art_url(const subsonic_server_t * server, const char * cover_art_id, char * out_url, size_t out_url_size);

#endif /* SUBSONIC_CLIENT_H */
