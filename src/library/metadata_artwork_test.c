/* Focused parser regression tests. Include the implementation to exercise
 * helper-only mode without exporting parser internals. Link with gc-sections
 * so unrelated container decoders are not required by this host test. */
#include "metadata.c"
#include "albumart.h"
#include <assert.h>
#include <sys/stat.h>

bool audio_is_playing(void) { return false; }

static void frame(FILE * f, const char * id, const unsigned char * data, size_t size) {
    unsigned char header[10] = {0};
    memcpy(header, id, 4);
    header[4] = size >> 24; header[5] = size >> 16;
    header[6] = size >> 8; header[7] = size;
    assert(fwrite(header, 1, sizeof(header), f) == sizeof(header));
    assert(fwrite(data, 1, size, f) == size);
}

int main(void) {
    const unsigned char lyrics[] = {0, 'e', 'n', 'g', 0, 'h', 'i'};
    const unsigned char picture[] = {0, 'i','m','a','g','e','/','j','p','e','g',0,3,0, 1,2,3,4};
    unsigned size = 20 + sizeof(lyrics) + sizeof(picture);
    unsigned char header[10] = {'I','D','3',3,0,0,0,0,0,0};
    header[9] = size; /* fixture < 128 bytes */
    FILE * f = tmpfile();
    assert(f);
    assert(fwrite(header, 1, sizeof(header), f) == sizeof(header));
    frame(f, "USLT", lyrics, sizeof(lyrics));
    frame(f, "APIC", picture, sizeof(picture));
    for (int mode = 0; mode < 2; mode++) {
        artwork_only = mode != 0;
        rewind(f);
        track_metadata_t meta = {0};
        assert(read_id3v2(f, &meta, true));
        assert(meta.picture_size == 4 && meta.picture_data);
        assert(memcmp(meta.picture_data, "\1\2\3\4", 4) == 0);
        assert(mode ? meta.lyrics == NULL : meta.lyrics != NULL);
        free(meta.picture_data); free(meta.lyrics);
        memset(&meta, 0, sizeof(meta));
        apply_vorbis_comment_field(&meta, "LYRICS=hello", 12, true);
        assert(mode ? meta.lyrics == NULL : meta.lyrics != NULL);
        free(meta.lyrics);
    }
    fclose(f);

    /* include_blobs=false must suppress vorbis-comment lyrics too (Opus/OGG
     * Vorbis share this matcher with FLAC's full-parse callback) -- this is
     * what metadata_read_without_artwork() relies on for those formats. */
    {
        track_metadata_t no_blobs_meta = {0};
        apply_vorbis_comment_field(&no_blobs_meta, "LYRICS=hello", 12, false);
        assert(no_blobs_meta.lyrics == NULL);
    }

    /* Lyrics-only ID3 reads retain USLT but stream past APIC without copying
     * it; the ordinary no-blob mode skips both payloads. */
    artwork_only = false;
    lyrics_only = true;
    f = tmpfile();
    assert(f);
    assert(fwrite(header, 1, sizeof(header), f) == sizeof(header));
    frame(f, "USLT", lyrics, sizeof(lyrics));
    frame(f, "APIC", picture, sizeof(picture));
    rewind(f);
    track_metadata_t lyrics_meta = {0};
    assert(read_id3v2(f, &lyrics_meta, true));
    assert(lyrics_meta.lyrics && strcmp(lyrics_meta.lyrics, "hi") == 0);
    assert(!lyrics_meta.picture_data && lyrics_meta.picture_size == 0);
    free(lyrics_meta.lyrics);
    rewind(f);
    track_metadata_t plain_meta = {0};
    lyrics_only = false;
    (void)read_id3v2(f, &plain_meta, false);
    assert(!plain_meta.lyrics && !plain_meta.picture_data);
    fclose(f);

    /* Bounded FLAC comment walker: text and ReplayGain survive, lyrics are
     * mode-dependent, and a large trailing PICTURE block is never loaded. */
    char flac_path[] = "/tmp/hiby-flac-meta-XXXXXX";
    int flac_fd = mkstemp(flac_path);
    assert(flac_fd >= 0);
    FILE * flac = fdopen(flac_fd, "wb");
    assert(flac);
    const char * fields[] = { "TITLE=walker", "REPLAYGAIN_TRACK_GAIN=-6.0 dB", "LYRICS=la-la" };
    unsigned comment_size = 4 + 4;
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) comment_size += 4 + strlen(fields[i]);
    unsigned char * comments = calloc(1, comment_size);
    assert(comments);
    unsigned p = 4; /* empty vendor */
    comments[p] = 3; /* three comments, little-endian */
    p += 4;
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        unsigned n = strlen(fields[i]);
        comments[p++] = n; comments[p++] = n >> 8; comments[p++] = n >> 16; comments[p++] = n >> 24;
        memcpy(comments + p, fields[i], n); p += n;
    }
    assert(fwrite("fLaC", 1, 4, flac) == 4);
    unsigned char block_header[4] = { 4, comment_size >> 16, comment_size >> 8, comment_size };
    assert(fwrite(block_header, 1, 4, flac) == 4);
    assert(fwrite(comments, 1, comment_size, flac) == comment_size);
    free(comments);
    /* Last block is a deliberately oversized PICTURE header with no body. */
    unsigned char picture_header[4] = { 0x86, 0xff, 0xff, 0xff };
    assert(fwrite(picture_header, 1, 4, flac) == 4);
    fclose(flac);
    track_metadata_t flac_meta = {0};
    lyrics_only = true;
    read_flac_text_metadata(flac_path, &flac_meta, true);
    assert(flac_meta.has_title && strcmp(flac_meta.title, "walker") == 0);
    assert(flac_meta.has_replaygain && flac_meta.lyrics && strcmp(flac_meta.lyrics, "la-la") == 0);
    assert(!flac_meta.picture_data && flac_meta.picture_size == 0);
    free(flac_meta.lyrics);
    unlink(flac_path);
    lyrics_only = false;

    /* An ID3v2 tag prepended before "fLaC" (non-standard, but real -- some
     * taggers write this) must be skipped, mirroring dr_flac's own
     * drflac_init() ID3-skip loop. Without it every text tag is lost, not
     * just the album art, for such a file in no-artwork/lyrics-only mode. */
    char flac_id3_path[] = "/tmp/hiby-flac-id3-meta-XXXXXX";
    int flac_id3_fd = mkstemp(flac_id3_path);
    assert(flac_id3_fd >= 0);
    FILE * flac_id3 = fdopen(flac_id3_fd, "wb");
    assert(flac_id3);
    const char * id3_field = "TITLE=prefixed";
    unsigned id3_n = strlen(id3_field);
    unsigned id3_comment_size = 4 + 4 + 4 + id3_n;
    unsigned char * id3_comments = calloc(1, id3_comment_size);
    assert(id3_comments);
    unsigned id3_p = 4; /* empty vendor */
    id3_comments[id3_p] = 1; /* one comment */
    id3_p += 4;
    id3_comments[id3_p++] = id3_n; id3_comments[id3_p++] = id3_n >> 8;
    id3_comments[id3_p++] = id3_n >> 16; id3_comments[id3_p++] = id3_n >> 24;
    memcpy(id3_comments + id3_p, id3_field, id3_n);

    unsigned char id3_header[10] = { 'I', 'D', '3', 3, 0, 0, 0, 0, 0, 4 }; /* synchsafe size = 4 */
    assert(fwrite(id3_header, 1, sizeof(id3_header), flac_id3) == sizeof(id3_header));
    unsigned char id3_padding[4] = {0, 0, 0, 0};
    assert(fwrite(id3_padding, 1, sizeof(id3_padding), flac_id3) == sizeof(id3_padding));
    assert(fwrite("fLaC", 1, 4, flac_id3) == 4);
    unsigned char id3_block_header[4] = { 4 | 0x80, id3_comment_size >> 16, id3_comment_size >> 8, id3_comment_size };
    assert(fwrite(id3_block_header, 1, 4, flac_id3) == 4);
    assert(fwrite(id3_comments, 1, id3_comment_size, flac_id3) == id3_comment_size);
    free(id3_comments);
    fclose(flac_id3);

    track_metadata_t flac_id3_meta = {0};
    read_flac_text_metadata(flac_id3_path, &flac_id3_meta, false);
    assert(flac_id3_meta.has_title && strcmp(flac_id3_meta.title, "prefixed") == 0);
    unlink(flac_id3_path);

    /* At the observed ~19.3 MiB, small artwork is admitted; a copy that
     * would consume the reserve is still refused. */
    system_set_mock_mem_available(19744U * 1024U);
    assert(artwork_check_memory_admission(ARTWORK_PRIO_WARMER, 1024U * 1024U));
    assert(artwork_check_memory_admission(ARTWORK_PRIO_WARMER, 128U * 1024U));
    assert(!artwork_check_memory_admission(ARTWORK_PRIO_WARMER, 12U * 1024U * 1024U));
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        assert(metadata_artwork_limit_memory());
        void * small = malloc(128U * 1024U);
        assert(small);
        free(small);
        void * excessive = malloc(64U * 1024U * 1024U);
        assert(!excessive && errno == ENOMEM);
        _exit(0);
    }
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    char path[] = "/tmp/hiby-art-read-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    assert(write(fd, "cover", 5) == 5);
    close(fd);
    uint8_t * bytes = NULL;
    uint32_t length = 0;
    system_set_mock_mem_available(8U * 1024U * 1024U + 4);
    assert(albumart_load_file_ex(path, &bytes, &length, 100, ARTWORK_PRIO_WARMER) == ALBUMART_LOAD_TEMPORARY);
    assert(!bytes && length == 0 && access(path, F_OK) == 0);
    /* Same file is allowed at player priority, which has a smaller reserve. */
    assert(albumart_load_file_ex(path, &bytes, &length, 100, ARTWORK_PRIO_PLAYER) == ALBUMART_LOAD_OK);
    assert(length == 5 && memcmp(bytes, "cover", 5) == 0);
    free(bytes);
    system_set_mock_mem_available(8U * 1024U * 1024U + 5);
    assert(albumart_load_file_ex(path, &bytes, &length, 100, ARTWORK_PRIO_WARMER) == ALBUMART_LOAD_OK);
    free(bytes);
    assert(albumart_load_file_ex(path, &bytes, &length, 4, ARTWORK_PRIO_WARMER) == ALBUMART_LOAD_INVALID);
    assert(!bytes && length == 0);
    unlink(path);
    assert(albumart_load_file_ex(path, &bytes, &length, 100, ARTWORK_PRIO_WARMER) == ALBUMART_LOAD_TEMPORARY);
    assert(mkfifo(path, 0600) == 0);
    assert(albumart_load_file_ex(path, &bytes, &length, 100, ARTWORK_PRIO_WARMER) == ALBUMART_LOAD_INVALID);
    unlink(path);

    /* The player-sized persistent cache uses the same atomic BMP writer as
     * thumbnails.  Exercise the ALBUMART_PLAYER_CACHE_SIZE geometry and
     * freshness marker so a cache generated by the warmer is discoverable
     * by Playing Now. */
    char source_path[] = "/tmp/hiby-art-source-XXXXXX";
    int source_fd = mkstemp(source_path);
    assert(source_fd >= 0);
    assert(write(source_fd, "source", 6) == 6);
    close(source_fd);
    char original_cwd[PATH_MAX];
    assert(getcwd(original_cwd, sizeof(original_cwd)));
    char cache_root[] = "/tmp/hiby-art-cache-XXXXXX";
    assert(mkdtemp(cache_root));
    assert(chdir(cache_root) == 0);
    albumart_info_t info = {0};
    snprintf(info.path, sizeof(info.path), "%s", source_path);
    snprintf(info.artist, sizeof(info.artist), "test-artist");
    snprintf(info.album, sizeof(info.album), "test-album");
    uint16_t * player_pixels = calloc((size_t) ALBUMART_PLAYER_CACHE_SIZE * ALBUMART_PLAYER_CACHE_SIZE,
                                      sizeof(*player_pixels));
    assert(player_pixels);
    assert(albumart_store_rgb565(&info, ALBUMART_PLAYER_CACHE_SIZE, ALBUMART_PLAYER_CACHE_SIZE,
                                 player_pixels));
    char cached_path[PATH_MAX];
    assert(albumart_sized_thumb_fresh(&info, ALBUMART_PLAYER_CACHE_SIZE,
                                     ALBUMART_PLAYER_CACHE_SIZE, cached_path, sizeof(cached_path)));
    assert(albumart_generated_cache_fresh(&info, ALBUMART_PLAYER_CACHE_SIZE,
                                         ALBUMART_PLAYER_CACHE_SIZE, cached_path, sizeof(cached_path)));
    assert(albumart_store_rgb565(&info, 0, ALBUMART_PLAYER_CACHE_SIZE, player_pixels) == false);
    free(player_pixels);
    unlink(cached_path);
    rmdir("./.open_hiby_player/albumart");
    rmdir("./.open_hiby_player");
    assert(chdir(original_cwd) == 0);
    rmdir(cache_root);
    unlink(source_path);
    puts("Artwork-only metadata and memory admission tests passed");
    return 0;
}
