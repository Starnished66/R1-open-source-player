#define _GNU_SOURCE
#include "metadata_db.h"

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Kept deliberately small: the Python front end streams one record at a time
 * and this process is the production metadata_db/tagcache writer. */
typedef struct {
    uint32_t path_len;
    int64_t mtime;
    int64_t size;
    uint16_t tag_len[5];
    int32_t track;
    int32_t disc;
} __attribute__((packed)) record_header_t;

void path_cache_drop(void) {}
void remote_state_drop(void) {}
bool remote_state_take(const char *p, int32_t *r, int32_t *c, int32_t *l) {
    (void)p; (void)r; (void)c; (void)l; return false;
}

static int read_full(FILE *in, void *buf, size_t size) {
    return fread(buf, 1, size, in) == size;
}

static int validate(const char *expected) {
    int64_t count = metadata_db_get_song_count();
    int artists = 0, album_artists = 0, albums = 0;
    metadata_db_get_group_counts(&artists, &album_artists, &albums);
    song_row_t songs[8];
    group_row_t groups[8];
    int song_page = metadata_db_get_songs_page_by_recency(0, 8, songs);
    int artist_page = metadata_db_get_groups_page(METADATA_DB_GROUP_ARTIST, 0, 8, groups);
    int album_page = metadata_db_get_groups_page(METADATA_DB_GROUP_ALBUM, 0, 8, groups);
    metadata_db_close();
    printf("songs=%" PRId64 " artists=%d album_artists=%d albums=%d song_page=%d artist_page=%d album_page=%d\n",
           count, artists, album_artists, albums, song_page, artist_page, album_page);
    return expected && count != strtoll(expected, NULL, 10) ? 5 :
           (count > 0 && song_page > 0 && artist_page >= 0 && album_page >= 0 ? 0 : 6);
}

int main(int argc, char **argv) {
    if (argc != 2 && argc != 3) return 2;
    FILE *in = fopen(argv[1], "rb");
    if (!in) { fprintf(stderr, "open spool: %s\n", strerror(errno)); return 2; }
    if (metadata_db_open() != METADATA_DB_LOAD_SUCCESS_FRESH) {
        fprintf(stderr, "staging directory is not empty or could not open\n");
        fclose(in); return 3;
    }
    metadata_db_begin_update();
    for (;;) {
        record_header_t h;
        size_t n = fread(&h, 1, sizeof(h), in);
        if (n == 0 && feof(in)) break;
        if (n != sizeof(h) || h.path_len == 0 || h.path_len >= 600) return 4;
        char path[600];
        if (!read_full(in, path, h.path_len)) return 4;
        path[h.path_len] = '\0';
        cached_tags_t tags;
        memset(&tags, 0, sizeof(tags));
        char *dst[5] = {tags.title, tags.artist, tags.album, tags.album_artist, tags.genre};
        for (int i = 0; i < 5; i++) {
            if (h.tag_len[i] >= sizeof(tags.title)) return 4;
            if (!read_full(in, dst[i], h.tag_len[i])) return 4;
            dst[i][h.tag_len[i]] = '\0';
        }
        tags.track_number = h.track;
        tags.disc_number = h.disc;
        metadata_db_put(path, h.mtime, h.size, &tags);
    }
    fclose(in);
    if (!metadata_db_end_update()) {
        fprintf(stderr, "metadata_db_end_update failed\n");
        metadata_db_close(); return 5;
    }
    /* Reopen the committed generation before reporting success. */
    metadata_db_load_outcome_t reopened = metadata_db_reload();
    if (reopened != METADATA_DB_LOAD_SUCCESS_NORMAL &&
        reopened != METADATA_DB_LOAD_SUCCESS_RECOVERED) {
        fprintf(stderr, "committed database did not reopen (outcome=%d)\n", reopened);
        metadata_db_close(); return 6;
    }
    return validate(argc == 3 ? argv[2] : NULL);
}
