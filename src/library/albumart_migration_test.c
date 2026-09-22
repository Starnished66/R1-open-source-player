#define _GNU_SOURCE
#include "albumart.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void write_file(const char * path, const char * text) {
    FILE * file = fopen(path, "w");
    assert(file);
    assert(fputs(text, file) >= 0);
    assert(fclose(file) == 0);
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
    uint64_t key = albumart_debug_thumbnail_key(&info);
    char legacy[256], current[256];
    snprintf(legacy, sizeof(legacy), ".open_hiby_player/albumart/v2-%016llx.72x72.bmp",
             (unsigned long long) key);
    snprintf(current, sizeof(current), ".compas/albumart/v2-%016llx.72x72.bmp",
             (unsigned long long) key);
    write_file(legacy, "legacy-thumb\n");

    char found[512];
    assert(albumart_search_files(&info, ".72x72", found, sizeof(found)));
    assert(strstr(found, ".open_hiby_player/albumart/v2-") != NULL);

    write_file(current, "current-thumb\n");
    assert(albumart_search_files(&info, ".72x72", found, sizeof(found)));
    assert(strstr(found, ".compas/albumart/v2-") != NULL);

    unlink(legacy);
    unlink(current);
    assert(rmdir(".open_hiby_player/albumart") == 0);
    assert(rmdir(".open_hiby_player") == 0);
    assert(rmdir(".compas/albumart") == 0);
    assert(rmdir(".compas") == 0);
    assert(chdir(previous) == 0);
    assert(rmdir(directory) == 0);
    puts("albumart migration lookup: PASS");
    return 0;
}
