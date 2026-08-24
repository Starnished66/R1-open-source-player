#ifndef SUBSONIC_SAVED_SERVERS_H
#define SUBSONIC_SAVED_SERVERS_H

#include <stdbool.h>
#include <stdint.h>

/* Small path-cache-adjacent sidecar for gui.c's "Saved Servers" Subsonic
 * setup flow -- a multi-field, URL-keyed upsert record, unlike the plain
 * path lists path_cache.c holds (a server profile isn't a path) and
 * unlike remote_state.c's accumulate-by-play-count shape (this is a
 * replace-on-save profile, not a counter). Same on-disk discipline as
 * both of those: own mutex, lazy-loaded TSV file, atomic temp+rename+
 * fsync writes. url is the natural unique key (a real server only has
 * one) -- saving the same URL again replaces the existing row rather
 * than creating a duplicate. Password is stored in plain text, matching
 * this app's existing single-server settings.c field -- no new exposure,
 * there is no secret-storage mechanism to upgrade to here.
 *
 * Lives on the internal partition (/usr/data), not the SD card, so the
 * list still works with the card unmounted. settings.c also mirrors the
 * same profiles into open_hiby_player_settings.txt for one-file backup. */

typedef struct {
    char url[256];
    char username[128];
    char password[128];
    bool verify_tls;
} subsonic_saved_server_t;

void subsonic_saved_servers_upsert(const char * url, const char * username, const char * password, bool verify_tls);

/* Caller-owned array, sorted alphabetically by url (case-insensitive) --
 * *out_rows is NULL and *out_count is 0 if there are none. */
void subsonic_saved_servers_load(subsonic_saved_server_t ** out_rows, int * out_count);

#endif /* SUBSONIC_SAVED_SERVERS_H */
