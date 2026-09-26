#define _GNU_SOURCE
#include "albumart.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static uint64_t legacy_thumbnail_key(const albumart_info_t * info) {
    const char * fields[] = { info->albumartist[0] ? info->albumartist : info->artist, info->album };
    uint64_t hash = UINT64_C(14695981039346656037);
    for (int i = 0; i < 2; i++) {
        const unsigned char * p = (const unsigned char *) fields[i];
        do { hash = (hash ^ *p) * UINT64_C(1099511628211); } while (*p++);
    }
    return hash;
}

static void write_file(const char * path, const char * text) {
    FILE * file = fopen(path, "w");
    assert(file);
    assert(fputs(text, file) >= 0);
    assert(fclose(file) == 0);
}

static bool path_exists(const char * path) {
    return access(path, F_OK) == 0;
}

int main(void) {
    char directory[] = "/tmp/compas-albumart-lookup-XXXXXX";
    char previous[512];
    assert(getcwd(previous, sizeof(previous)));
    assert(mkdtemp(directory));
    assert(chdir(directory) == 0);
    assert(mkdir(".open_hiby_player", 0755) == 0);
    assert(mkdir(".open_hiby_player/albumart", 0755) == 0);
    assert(mkdir(".compas", 0755) == 0);
    assert(mkdir(".compas/albumart", 0755) == 0);

    albumart_info_t info;
    memset(&info, 0, sizeof(info));
    snprintf(info.path, sizeof(info.path), "/music/album/track.flac");
    snprintf(info.artist, sizeof(info.artist), "Artist");
    snprintf(info.album, sizeof(info.album), "Album");
    uint64_t legacy_key = legacy_thumbnail_key(&info);
    uint64_t current_key = albumart_thumbnail_key(&info);
    char legacy[256], current[256];
    snprintf(legacy, sizeof(legacy), ".open_hiby_player/albumart/v2-%016llx.72x72.bmp",
             (unsigned long long) legacy_key);
    snprintf(current, sizeof(current), ".compas/albumart/v3-%016llx.72x72.bmp",
             (unsigned long long) current_key);
    write_file(legacy, "legacy-thumb\n");

    char found[512];
    assert(albumart_search_files(&info, ".72x72", found, sizeof(found)));
    assert(strstr(found, ".open_hiby_player/albumart/v2-") != NULL);

    write_file(current, "current-thumb\n");
    assert(albumart_search_files(&info, ".72x72", found, sizeof(found)));
    assert(strstr(found, ".compas/albumart/v3-") != NULL);

    albumart_info_t unrelated = info;
    snprintf(unrelated.album, sizeof(unrelated.album), "Other Album");
    uint64_t unrelated_key = albumart_thumbnail_key(&unrelated);
    char player_cache[256], artist_cache[256], artist_noart[256], artist_alias[256], unrelated_cache[256];
    snprintf(player_cache, sizeof(player_cache), ".open_hiby_player/albumart/v2-%016llx.480x480.bmp",
             (unsigned long long) legacy_key);
    snprintf(artist_cache, sizeof(artist_cache), ".compas/albumart/artist-v1-%016llx.72x72.bmp",
             (unsigned long long) albumart_artist_thumbnail_key("Artist"));
    snprintf(artist_noart, sizeof(artist_noart), ".compas/albumart/artist-v1-%016llx.72x72.noart",
             (unsigned long long) albumart_artist_thumbnail_key("Artist"));
    snprintf(artist_alias, sizeof(artist_alias), ".compas/albumart/artist-v1-%016llx-0.album",
             (unsigned long long) albumart_artist_thumbnail_key("Artist"));
    snprintf(unrelated_cache, sizeof(unrelated_cache), ".compas/albumart/v3-%016llx.72x72.bmp",
             (unsigned long long) unrelated_key);
    write_file(player_cache, "player-thumb\n");
    write_file(artist_cache, "artist-thumb\n");
    write_file(artist_noart, "negative\n");
    write_file(unrelated_cache, "unrelated-thumb\n");
    write_file(".compas/albumart/cover.jpg", "user file\n");
    write_file(".compas/albumart/v3-not-a-hash.72x72.bmp", "unrecognized\n");
    assert(albumart_artist_alias_store("Artist", 0, current_key, 1));
    assert(path_exists(artist_alias));
    /* A guest artist only reaches this album through its alias; a bystander
     * artist with its own "no art" marker is unrelated and must survive. */
    char guest_alias[256], guest_thumb[256], bystander_noart[256];
    snprintf(guest_alias, sizeof(guest_alias), ".compas/albumart/artist-v1-%016llx-1.album",
             (unsigned long long) albumart_artist_thumbnail_key("Guest"));
    snprintf(guest_thumb, sizeof(guest_thumb), ".compas/albumart/artist-v1-%016llx.72x72.bmp",
             (unsigned long long) albumart_artist_thumbnail_key("Guest"));
    snprintf(bystander_noart, sizeof(bystander_noart), ".compas/albumart/artist-v1-%016llx.72x72.noart",
             (unsigned long long) albumart_artist_thumbnail_key("Bystander"));
    assert(albumart_artist_alias_store("Guest", 1, current_key, 2));
    write_file(guest_thumb, "guest-thumb\n");
    write_file(bystander_noart, "negative\n");
    assert(albumart_delete_generated_cache_for_album(&info, NULL, 0));
    assert(!path_exists(current));
    assert(!path_exists(legacy));
    assert(!path_exists(player_cache));
    assert(!path_exists(artist_cache));
    assert(!path_exists(artist_noart));
    assert(!path_exists(artist_alias));
    assert(!path_exists(guest_alias));
    assert(!path_exists(guest_thumb));
    assert(path_exists(bystander_noart));
    assert(path_exists(unrelated_cache));
    assert(path_exists(".compas/albumart/cover.jpg"));
    assert(path_exists(".compas/albumart/v3-not-a-hash.72x72.bmp"));

    assert(albumart_delete_all_generated_caches());
    assert(!path_exists(unrelated_cache));
    assert(!path_exists(bystander_noart));
    assert(path_exists(".compas/albumart/cover.jpg"));
    assert(path_exists(".compas/albumart/v3-not-a-hash.72x72.bmp"));

    unlink(".compas/albumart/cover.jpg");
    unlink(".compas/albumart/v3-not-a-hash.72x72.bmp");
    assert(rmdir(".open_hiby_player/albumart") == 0);
    assert(rmdir(".open_hiby_player") == 0);

    /* A cache root replaced by a symlink is never followed. */
    assert(mkdir("elsewhere", 0755) == 0);
    assert(mkdir("elsewhere/albumart", 0755) == 0);
    write_file("elsewhere/albumart/v3-0123456789abcdef.72x72.bmp", "not ours\n");
    assert(symlink("elsewhere", ".open_hiby_player") == 0);
    (void) albumart_delete_all_generated_caches();
    assert(path_exists("elsewhere/albumart/v3-0123456789abcdef.72x72.bmp"));
    assert(unlink(".open_hiby_player") == 0);
    assert(unlink("elsewhere/albumart/v3-0123456789abcdef.72x72.bmp") == 0);
    assert(rmdir("elsewhere/albumart") == 0);
    assert(rmdir("elsewhere") == 0);
    assert(!albumart_is_generated_cache_file("/music/Album/cover.72x72.png"));

    assert(rmdir(".compas/albumart") == 0);
    assert(rmdir(".compas") == 0);
    assert(chdir(previous) == 0);
    assert(rmdir(directory) == 0);
    puts("albumart migration lookup: PASS");
    return 0;
}
