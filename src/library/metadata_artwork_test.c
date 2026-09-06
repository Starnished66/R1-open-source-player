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
        apply_vorbis_comment_field(&meta, "LYRICS=hello", 12);
        assert(mode ? meta.lyrics == NULL : meta.lyrics != NULL);
        free(meta.lyrics);
    }
    fclose(f);
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
    puts("Artwork-only metadata and memory admission tests passed");
    return 0;
}
