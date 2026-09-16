#include <assert.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <zlib.h>
#include <expat.h>
#include <alsa/asoundlib.h>
#include <sbc/sbc.h>

/* Run with the candidate image's libraries under qemu-mipsel. No devices,
 * system bus, or firmware files are opened or changed. */
int main(int argc, char **argv) {
    const unsigned char input[] = "HiBy base-image compression roundtrip";
    unsigned char packed[256], restored[256];
    uLongf packed_size = sizeof(packed), restored_size = sizeof(restored);
    assert(compress(packed, &packed_size, input, sizeof(input)) == Z_OK);
    assert(uncompress(restored, &restored_size, packed, packed_size) == Z_OK);
    assert(restored_size == sizeof(input) && !memcmp(restored, input, sizeof(input)));
    printf("zlib %s: compression roundtrip PASS\n", zlibVersion());
    XML_Parser parser = XML_ParserCreate(NULL);
    assert(parser);
    const char xml[] = "<album><title>R1 &amp; music</title></album>";
    assert(XML_Parse(parser, xml, sizeof(xml) - 1, XML_TRUE) == XML_STATUS_OK);
    XML_ParserFree(parser);
    printf("Expat %s: XML parsing PASS\n", XML_ExpatVersion());
    snd_config_t *config = NULL;
    assert(snd_config_top(&config) == 0 && config);
    assert(snd_config_delete(config) == 0);
    printf("ALSA %s: config allocation PASS\n", snd_asoundlib_version());
    sbc_t encoder, decoder;
    assert(sbc_init(&encoder, 0) == 0 && sbc_init(&decoder, 0) == 0);
    unsigned char pcm[4096] = {0}, frame[1024], decoded[4096];
    ssize_t written = 0;
    size_t decoded_size = 0, codesize = sbc_get_codesize(&encoder);
    assert(codesize <= sizeof(pcm));
    assert(sbc_encode(&encoder, pcm, codesize, frame, sizeof(frame), &written) > 0);
    assert(written > 0);
    assert(sbc_decode(&decoder, frame, written, decoded, sizeof(decoded), &decoded_size) > 0);
    assert(decoded_size == codesize);
    sbc_finish(&encoder);
    sbc_finish(&decoder);
    puts("SBC: frame encode/decode PASS");
    for (int i = 1; i < argc; i++) {
        void *handle = dlopen(argv[i], RTLD_NOW | RTLD_LOCAL);
        if (!handle) {
            fprintf(stderr, "Plugin load failed: %s: %s\n", argv[i], dlerror());
            return 1;
        }
        printf("Plugin eager binding PASS: %s\n", argv[i]);
        dlclose(handle);
    }
    return 0;
}
