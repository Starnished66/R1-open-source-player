#include "metadata_db.h"
#include "utf8_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>

static void fail(const char * msg) {
    fprintf(stderr, "FAIL: %s\n", msg);
    exit(1);
}

int main(void) {
    const char * dir = "/tmp/ohp_tagcache_test";
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", dir, dir);
    if (system(cmd) != 0) fail("setup");
    if (chdir(dir) != 0) fail("chdir");

    metadata_db_open();
    if (metadata_db_get_song_count() != 0) fail("empty count");

    cached_tags_t t1 = { 0 }, t2 = { 0 }, t3 = { 0 };
    snprintf(t1.title, sizeof(t1.title), "%s", "Alpha");
    snprintf(t1.artist, sizeof(t1.artist), "%s", "Beatles");
    snprintf(t1.album, sizeof(t1.album), "%s", "Help");
    snprintf(t1.album_artist, sizeof(t1.album_artist), "%s", "Beatles");
    snprintf(t1.genre, sizeof(t1.genre), "%s", "Rock");
    snprintf(t2.title, sizeof(t2.title), "%s", "Beta");
    snprintf(t2.artist, sizeof(t2.artist), "%s", "Beatles");
    snprintf(t2.album, sizeof(t2.album), "%s", "Help");
    snprintf(t2.album_artist, sizeof(t2.album_artist), "%s", "Beatles");
    snprintf(t2.genre, sizeof(t2.genre), "%s", "Rock");
    snprintf(t3.title, sizeof(t3.title), "%s", "Gamma");
    snprintf(t3.artist, sizeof(t3.artist), "%s", "Queen");
    snprintf(t3.album, sizeof(t3.album), "%s", "Greatest Hits");
    snprintf(t3.album_artist, sizeof(t3.album_artist), "%s", "Queen");
    snprintf(t3.genre, sizeof(t3.genre), "%s", "Rock");

    metadata_db_begin_update();
    metadata_db_put("/music/a.flac", 100, 1000, &t1);
    metadata_db_put("/music/b.flac", 100, 1000, &t2);
    metadata_db_put("/music/c.flac", 100, 1000, &t3);
    metadata_db_end_update();

    if (metadata_db_get_song_count() != 3) fail("count after insert");

    song_row_t rows[8];
    int n = metadata_db_get_songs_page(NULL, 0, 8, rows);
    if (n != 3) fail("songs page size");
    if (strcmp(rows[0].tags.title, "Alpha") != 0) fail("title order");

    int artists = 0, aas = 0, albums = 0;
    metadata_db_get_group_counts(&artists, &aas, &albums);
    if (artists != 2) fail("artist count");
    if (albums != 2) fail("album count");

    cached_tags_t cached;
    if (!metadata_db_get("/music/a.flac", 100, 1000, &cached)) fail("lookup hit");
    if (metadata_db_get("/music/a.flac", 200, 1000, &cached)) fail("mtime mismatch should miss");

    metadata_db_begin_update();
    if (!metadata_db_get("/music/a.flac", 100, 1000, &cached)) fail("incremental hit a");
    if (!metadata_db_get("/music/b.flac", 100, 1000, &cached)) fail("incremental hit b");
    /* c not seen -- pruned */
    metadata_db_end_update();
    if (metadata_db_get_song_count() != 2) fail("prune");

    metadata_db_song_favorite_set("/music/a.flac", true);
    if (!metadata_db_song_favorite_is_set("/music/a.flac")) fail("favorite set");
    metadata_db_song_play_count_increment("/music/a.flac");
    metadata_db_song_play_count_increment("/music/a.flac");
    char ** top = NULL;
    int top_n = 0;
    metadata_db_load_top_played_songs(5, &top, &top_n);
    if (top_n != 1 || strcmp(top[0], "/music/a.flac") != 0) fail("top played");
    free(top[0]);
    free(top);

    n = metadata_db_search_songs("alp", rows, 8);
    if (n != 1) fail("search");

    metadata_db_close();
    metadata_db_open();
    if (metadata_db_get_song_count() != 2) fail("reload count");
    if (!metadata_db_song_favorite_is_set("/music/a.flac")) fail("favorite persist");
    song_row_t by_path;
    if (!metadata_db_get_song_by_path("/music/a.flac", &by_path)) fail("by path after reload");
    if (strcmp(by_path.tags.artist, "Beatles") != 0) fail("artist persist");

    group_row_t groups[8];
    n = metadata_db_get_albums_page_filtered(NULL, 0, 8, groups);
    if (n != 1) fail("albums after prune");
    n = metadata_db_get_album_songs("Help", "Beatles", 0, rows, 8);
    if (n != 2) fail("album songs");

    if (metadata_db_get_song_title_offset("/music/a.flac") < 0) fail("title offset");
    n = metadata_db_get_songs_page("Alpha", 1, 8, rows);
    if (n < 1) fail("keyset page");

    metadata_db_song_favorite_set("remote://plug/t1", true);
    if (!metadata_db_song_favorite_is_set("remote://plug/t1")) fail("remote favorite");
    metadata_db_song_play_count_increment("remote://plug/t1");
    metadata_db_song_favorite_set("/music/unscanned.flac", true);
    metadata_db_song_play_count_increment("/music/unscanned.flac");
    metadata_db_begin_update();
    if (!metadata_db_get("/music/a.flac", 100, 1000, &cached)) fail("keep a for migrate scan");
    if (!metadata_db_get("/music/b.flac", 100, 1000, &cached)) fail("keep b for migrate scan");
    metadata_db_put("/music/unscanned.flac", 100, 1000, &t1);
    metadata_db_end_update();
    if (!metadata_db_song_favorite_is_set("/music/unscanned.flac")) fail("migrated favorite");
    if (!metadata_db_song_favorite_is_set("remote://plug/t1")) fail("remote favorite after scan");
    {
        char ** favs = NULL;
        int fn = 0;
        metadata_db_load_favorite_songs(&favs, &fn);
        int saw_remote = 0, saw_migrated = 0;
        for (int i = 0; i < fn; i++) {
            if (strcmp(favs[i], "remote://plug/t1") == 0) saw_remote = 1;
            if (strcmp(favs[i], "/music/unscanned.flac") == 0) saw_migrated = 1;
            free(favs[i]);
        }
        free(favs);
        if (saw_remote) fail("remote listed in local favorites");
        if (!saw_migrated) fail("migrated path missing from favorites");

        char ** recent = NULL;
        int rn = 0;
        metadata_db_load_recently_added_songs(10, &recent, &rn);
        if (rn != 3) fail("recently added count");
        for (int i = 0; i < rn; i++) free(recent[i]);
        free(recent);
    }

    struct stat st;
    if (stat(".open_hiby_player/tagcache.gen", &st) != 0) fail("generation pointer");

    metadata_db_close();
    metadata_db_open();
    if (!metadata_db_song_favorite_is_set("remote://plug/t1")) fail("remote favorite reload");
    if (!metadata_db_song_favorite_is_set("/music/unscanned.flac")) fail("migrated favorite reload");

    setenv("TAGCACHE_FORCE_COMPACT", "1", 1);
    metadata_db_close();
    metadata_db_open();
    if (metadata_db_get_song_count() != 3) fail("compact reload count");
    if (!metadata_db_get_song_by_path("/music/a.flac", &by_path)) fail("compact by path");
    if (strcmp(by_path.tags.artist, "Beatles") != 0) fail("compact artist");
    if (metadata_db_get_song_title_offset("/music/a.flac") < 0) fail("compact title offset");
    unsetenv("TAGCACHE_FORCE_COMPACT");

    /* Untitled tracks store tag_length == 1. Compact mmap load used to
     * treat that as fatal and present an empty library. */
    {
        cached_tags_t untitled = { 0 };
        snprintf(untitled.artist, sizeof(untitled.artist), "%s", "Unknown Artist");
        snprintf(untitled.album, sizeof(untitled.album), "%s", "Unknown Album");
        snprintf(untitled.album_artist, sizeof(untitled.album_artist), "%s", "Unknown Artist");
        snprintf(untitled.genre, sizeof(untitled.genre), "%s", "Unknown Genre");
        metadata_db_begin_update();
        if (!metadata_db_get("/music/a.flac", 100, 1000, &cached)) fail("keep a for empty-title scan");
        if (!metadata_db_get("/music/b.flac", 100, 1000, &cached)) fail("keep b for empty-title scan");
        if (!metadata_db_get("/music/unscanned.flac", 100, 1000, &cached)) fail("keep unscanned for empty-title scan");
        metadata_db_put("/music/untitled.flac", 100, 1000, &untitled);
        if (!metadata_db_end_update()) fail("empty-title commit");
        if (metadata_db_get_song_count() != 4) fail("empty-title interned count");
        setenv("TAGCACHE_FORCE_COMPACT", "1", 1);
        metadata_db_close();
        metadata_db_open();
        if (metadata_db_get_song_count() != 4) fail("compact empty-title count");
        if (!metadata_db_get_song_by_path("/music/untitled.flac", &by_path)) fail("compact empty-title path");
        if (by_path.tags.title[0] != '\0') fail("compact empty-title value");
        unsetenv("TAGCACHE_FORCE_COMPACT");
    }

    /* tagcache.gen is the commit record.  A crash can leave structurally
     * readable database_*.tcd.gN files before the pointer is published;
     * boot must not promote those uncommitted scan results into the live
     * library (including database-backed rows under Playlists). */
    {
        if (unlink(".open_hiby_player/tagcache.gen") != 0) fail("unlink gen pointer");
        metadata_db_close();
        metadata_db_open();
        if (metadata_db_get_song_count() != 0) fail("uncommitted generation loaded without pointer");
        if (metadata_db_get_song_by_path("/music/a.flac", &by_path)) fail("uncommitted path visible");
        if (stat(".open_hiby_player/tagcache.gen", &st) == 0) fail("uncommitted generation pointer recreated");
    }

    {
        char * pl_a = "/tmp/ohp_tagcache_test/Playlists/a.m3u";
        char * pl_b = "/tmp/ohp_tagcache_test/Playlists/b.m3u";
        char * batch[] = { pl_a };
        metadata_db_playlist_replace_all(batch, 1);
        metadata_db_playlist_insert_one(pl_b);
        {
            FILE * poisoned = fopen(".open_hiby_player/playlists.list", "a");
            if (!poisoned) fail("open playlist cache poison");
            fprintf(poisoned, "/music/not-a-playlist.flac\n/music/Album Name\n");
            fclose(poisoned);
            metadata_db_close();
            metadata_db_open();
        }
        char ** paths = NULL;
        int pn = 0;
        metadata_db_load_all_playlists(&paths, &pn);
        if (pn != 2) fail("playlist cache count");
        metadata_db_playlist_delete_one(pl_a);
        for (int i = 0; i < pn; i++) free(paths[i]);
        free(paths);
        paths = NULL;
        pn = 0;
        metadata_db_load_all_playlists(&paths, &pn);
        if (pn != 1 || strcmp(paths[0], pl_b) != 0) fail("playlist cache delete");
        free(paths[0]);
        free(paths);

        metadata_db_subsonic_server_save("https://b.example", "u2", "p2", false);
        metadata_db_subsonic_server_save("https://a.example", "u", "p", true);
        metadata_db_subsonic_server_save("https://a.example", "u", "p-new", true);
        metadata_db_subsonic_server_save("https://bad.example\t", "u", "p", true);
        subsonic_server_row_t * srows = NULL;
        int sn = 0;
        metadata_db_load_subsonic_servers(&srows, &sn);
        if (sn != 2) fail("subsonic saved count");
        if (strcmp(srows[0].url, "https://a.example") != 0) fail("subsonic saved sort");
        if (strcmp(srows[0].password, "p-new") != 0) fail("subsonic saved upsert");
        if (srows[0].verify_tls != true) fail("subsonic saved tls");
        if (strcmp(srows[1].url, "https://b.example") != 0) fail("subsonic saved second");
        free(srows);
    }

    metadata_db_close();
    printf("ok\n");
    return 0;
}
