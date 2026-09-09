/* Host tests for JPEG cover decode: jpeg_probe() SOF walking, scale
 * selection, tjpgd 1/2 1/4 1/8 output geometry and BGR order,
 * cover_decode_to_rgb565_ex cover-fit, and malformed/oversized rejection.
 *
 * Build/run: `make cover_decode_scale_test` */
#include "cover_decode.h"
#include "cover_decode_jpeg_fixtures.h"
#include "lvgl/src/libs/tjpgd/tjpgd.h"
#include "lvgl/src/libs/lodepng/lodepng.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

bool audio_is_playing(void) {
    return false;
}

/* Model the LVGL fork's descriptor ownership contract without linking LVGL.
 * This exercises the adapter, not PNG decompression itself. */
static bool mock_png;
static unsigned mock_png_error;
static unsigned mock_png_destroyed;
void lv_draw_buf_destroy(lv_draw_buf_t * buf) {
    free(buf->data);
    free(buf);
    mock_png_destroyed++;
}
void lodepng_state_init(LodePNGState * state) {
    memset(state, 0, sizeof(*state));
}
void lodepng_state_cleanup(LodePNGState * state) {
    (void) state;
}
unsigned lodepng_inspect(unsigned * w, unsigned * h, LodePNGState * state, const unsigned char * in, size_t insize) {
    (void) state;
    (void) in;
    (void) insize;
    if (mock_png) {
        *w = *h = 2;
        return 0;
    }
    if (w) *w = 0;
    if (h) *h = 0;
    return 1;
}
unsigned lodepng_get_bpp(const LodePNGColorMode * info) {
    (void) info;
    return 32; /* arbitrary fixed value; this mock build never links real lodepng color-mode logic */
}
unsigned lodepng_decode(unsigned char ** out, unsigned * w, unsigned * h, LodePNGState * state,
                        const unsigned char * in, size_t insize) {
    (void) state;
    (void) in;
    (void) insize;
    if (mock_png) {
        lv_draw_buf_t * buf = calloc(1, sizeof(*buf));
        buf->data_size = 16;
        buf->data = calloc(1, buf->data_size);
        /* Packed RGB24, despite the allocation's four-byte-per-pixel size. */
        for (int i = 0; i < 4; i++) buf->data[i * 3] = 255;
        *out = (unsigned char *) buf;
        *w = *h = 2;
        return mock_png_error;
    }
    if (out) *out = NULL;
    if (w) *w = 0;
    if (h) *h = 0;
    return 1;
}

static int g_failures = 0;
static const char * g_current_test = "";

#define CHECK(cond)                                                                                                  \
    do {                                                                                                             \
        if (!(cond)) {                                                                                               \
            fprintf(stderr, "FAIL [%s] %s:%d: %s\n", g_current_test, __FILE__, __LINE__, #cond);                     \
            g_failures++;                                                                                            \
        }                                                                                                            \
    } while (0)

#define BEGIN_TEST(name)                                                                                             \
    do {                                                                                                             \
        g_current_test = name;                                                                                       \
        fprintf(stderr, "-- %s\n", name);                                                                            \
    } while (0)

static void test_jpeg_scale_for_target(void) {
    BEGIN_TEST("jpeg_scale_for_target");
    CHECK(jpeg_scale_for_target(1200, 1200, 480, 480) == 1);
    CHECK(jpeg_scale_for_target(1200, 1200, 72, 72) == 3);
    CHECK(jpeg_scale_for_target(800, 800, 480, 480) == 0);
    CHECK(jpeg_scale_for_target(1000, 600, 480, 480) == 0);
    CHECK(jpeg_scale_for_target(72, 72, 72, 72) == 0);
    CHECK(jpeg_scale_for_target(480, 480, 480, 480) == 0);
    CHECK(jpeg_scale_for_target(150, 150, 72, 72) == 1);
    CHECK(jpeg_scale_for_target(0, 100, 480, 480) == 0);
    CHECK(jpeg_scale_for_target(1001, 1001, 480, 480) == 1);
    CHECK(jpeg_scale_for_target(128, 128, 128, 128) == 0);
    CHECK(jpeg_scale_for_target(128, 128, 64, 64) == 1);
    CHECK(jpeg_scale_for_target(128, 128, 32, 32) == 2);
    CHECK(jpeg_scale_for_target(128, 128, 16, 16) == 3);

    CHECK(jpeg_decode_dims_ok(2000, 2000, 480, 480) == true);   /* 1/4 -> 500 */
    CHECK(jpeg_decode_dims_ok(4000, 4000, 480, 480) == true);   /* 1/8 -> 500 */
    CHECK(jpeg_decode_dims_ok(4000, 4000, 72, 72)   == true);
    CHECK(jpeg_decode_dims_ok(4000, 500, 480, 480)  == false);  /* scale 0, 4000 > 1200 */
    CHECK(jpeg_decode_dims_ok(20000, 20000, 72, 72) == false);  /* native > 4096, and/or scaled 2500 > 1200 */
    CHECK(jpeg_decode_dims_ok(4096, 4096, 480, 480) == true);   /* 1/8 -> 512 */
    CHECK(jpeg_decode_dims_ok(4097, 4097, 480, 480) == false);  /* native cap */
}

typedef struct {
    const uint8_t * data;
    uint32_t size;
    uint32_t pos;
    int max_x;
    int max_y;
    int tiles;
    uint8_t first_b, first_g, first_r;
    int got_first;
} tjpg_rec_t;

static size_t rec_read(JDEC * jd, uint8_t * buff, size_t ndata) {
    tjpg_rec_t * ctx = (tjpg_rec_t *) jd->device;
    if (ctx->pos >= ctx->size) return 0;
    size_t avail = ctx->size - ctx->pos;
    size_t n = ndata < avail ? ndata : avail;
    if (buff) memcpy(buff, ctx->data + ctx->pos, n);
    ctx->pos += (uint32_t) n;
    return n;
}

static int rec_out(JDEC * jd, void * bitmap, JRECT * rect) {
    tjpg_rec_t * ctx = (tjpg_rec_t *) jd->device;
    int right = (int) rect->right + 1;
    int bottom = (int) rect->bottom + 1;
    if (right > ctx->max_x) ctx->max_x = right;
    if (bottom > ctx->max_y) ctx->max_y = bottom;
    ctx->tiles++;
    if (!ctx->got_first) {
        const uint8_t * p = (const uint8_t *) bitmap;
        ctx->first_b = p[0];
        ctx->first_g = p[1];
        ctx->first_r = p[2];
        ctx->got_first = 1;
    }
    (void) jd;
    return 1;
}

static void tjpgd_decomp(const uint8_t * data, uint32_t size, uint8_t scale, tjpg_rec_t * rec) {
    memset(rec, 0, sizeof(*rec));
    rec->data = data;
    rec->size = size;
    uint8_t work[4096];
    JDEC jd;
    CHECK(jd_prepare(&jd, rec_read, work, sizeof(work), rec) == JDR_OK);
    CHECK((int) jd.width == 128);
    CHECK((int) jd.height == 128);
    CHECK(jd_decomp(&jd, rec_out, scale) == JDR_OK);
}

static int is_tjpgd_red_bgr(uint8_t b, uint8_t g, uint8_t r) {
    return r >= 200 && g <= 30 && b <= 30;
}

static void test_tjpgd_scaled_geometry_and_bgr(void) {
    BEGIN_TEST("tjpgd_scaled_geometry_and_bgr");
    tjpg_rec_t rec;
    tjpgd_decomp(jpeg_red_128, jpeg_red_128_size, 0, &rec);
    CHECK(rec.max_x == 128 && rec.max_y == 128);
    CHECK(rec.tiles > 0);
    CHECK(rec.got_first);
    CHECK(is_tjpgd_red_bgr(rec.first_b, rec.first_g, rec.first_r));

    tjpgd_decomp(jpeg_red_128, jpeg_red_128_size, 1, &rec);
    CHECK(rec.max_x == 64 && rec.max_y == 64);
    CHECK(is_tjpgd_red_bgr(rec.first_b, rec.first_g, rec.first_r));

    tjpgd_decomp(jpeg_red_128, jpeg_red_128_size, 2, &rec);
    CHECK(rec.max_x == 32 && rec.max_y == 32);
    CHECK(is_tjpgd_red_bgr(rec.first_b, rec.first_g, rec.first_r));

    tjpgd_decomp(jpeg_red_128, jpeg_red_128_size, 3, &rec);
    CHECK(rec.max_x == 16 && rec.max_y == 16);
    CHECK(is_tjpgd_red_bgr(rec.first_b, rec.first_g, rec.first_r));
}

static void rgb565_split(uint16_t p, int * r, int * g, int * b) {
    *r = (int) (p >> 11);
    *g = (int) ((p >> 5) & 63);
    *b = (int) (p & 31);
}

static int is_red565(uint16_t p) {
    int r, g, b;
    rgb565_split(p, &r, &g, &b);
    return r >= 24 && g <= 12 && b <= 6;
}

static int is_green565(uint16_t p) {
    int r, g, b;
    rgb565_split(p, &r, &g, &b);
    return g >= 48 && r <= 8 && b <= 6;
}

static int is_blue565(uint16_t p) {
    int r, g, b;
    rgb565_split(p, &r, &g, &b);
    return b >= 24 && r <= 8 && g <= 12;
}

static int is_white565(uint16_t p) {
    int r, g, b;
    rgb565_split(p, &r, &g, &b);
    return r >= 24 && g >= 48 && b >= 24;
}

static uint16_t sample(const uint16_t * px, int side, int x, int y) {
    return px[(size_t) y * (size_t) side + (size_t) x];
}

static void test_cover_decode_red_each_scale(void) {
    BEGIN_TEST("cover_decode_red_each_scale");
    system_set_mock_mem_available(64U * 1024U * 1024U);
    const int sides[] = {128, 64, 32, 16};
    for (int i = 0; i < 4; i++) {
        int side = sides[i];
        uint16_t * pixels = NULL;
        cover_decode_result_t res = cover_decode_to_rgb565_ex(jpeg_red_128, jpeg_red_128_size, side, side,
                                                              ARTWORK_PRIO_PLAYER, NULL, NULL, &pixels);
        CHECK(res == COVER_DECODE_OK);
        CHECK(pixels != NULL);
        if (pixels) {
            CHECK(is_red565(sample(pixels, side, side / 2, side / 2)));
            CHECK(is_red565(sample(pixels, side, 0, 0)));
            CHECK(is_red565(sample(pixels, side, side - 1, side - 1)));
        }
        free(pixels);
    }
}

static void test_cover_decode_quadrants_each_scale(void) {
    BEGIN_TEST("cover_decode_quadrants_each_scale");
    system_set_mock_mem_available(64U * 1024U * 1024U);
    const int sides[] = {128, 64, 32, 16};
    for (int i = 0; i < 4; i++) {
        int side = sides[i];
        uint16_t * pixels = NULL;
        cover_decode_result_t res = cover_decode_to_rgb565_ex(jpeg_quad_128, jpeg_quad_128_size, side, side,
                                                              ARTWORK_PRIO_PLAYER, NULL, NULL, &pixels);
        CHECK(res == COVER_DECODE_OK);
        CHECK(pixels != NULL);
        if (pixels) {
            int q = side / 4;
            CHECK(is_red565(sample(pixels, side, q, q)));
            CHECK(is_green565(sample(pixels, side, side - 1 - q, q)));
            CHECK(is_blue565(sample(pixels, side, q, side - 1 - q)));
            CHECK(is_white565(sample(pixels, side, side - 1 - q, side - 1 - q)));
        }
        free(pixels);
    }
}

static bool patch_sof0_size(uint8_t * jpeg, uint32_t size, uint16_t w, uint16_t h) {
    for (uint32_t i = 0; i + 9 < size; i++) {
        if (jpeg[i] == 0xFF && jpeg[i + 1] == 0xC0) {
            jpeg[i + 5] = (uint8_t) (h >> 8);
            jpeg[i + 6] = (uint8_t) h;
            jpeg[i + 7] = (uint8_t) (w >> 8);
            jpeg[i + 8] = (uint8_t) w;
            return true;
        }
    }
    return false;
}

static void test_malformed_and_oversized(void) {
    BEGIN_TEST("malformed_and_oversized");
    system_set_mock_mem_available(64U * 1024U * 1024U);
    uint16_t * pixels = NULL;

    const uint8_t garbage[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    CHECK(cover_decode_to_rgb565_ex(garbage, sizeof(garbage), 72, 72, ARTWORK_PRIO_PLAYER, NULL, NULL, &pixels) ==
          COVER_DECODE_FAIL_UNSUPPORTED);
    CHECK(pixels == NULL);

    const uint8_t truncated_soi[] = {0xFF, 0xD8, 0xFF, 0xC0};
    CHECK(cover_decode_to_rgb565_ex(truncated_soi, sizeof(truncated_soi), 72, 72, ARTWORK_PRIO_PLAYER, NULL, NULL,
                                    &pixels) == COVER_DECODE_FAIL_UNSUPPORTED);

    CHECK(cover_decode_to_rgb565_ex(jpeg_red_128, 12, 72, 72, ARTWORK_PRIO_PLAYER, NULL, NULL, &pixels) ==
          COVER_DECODE_FAIL_UNSUPPORTED);

    uint8_t oversized[2048];
    CHECK(jpeg_red_128_size <= sizeof(oversized));

    /* 4000x500 target 480x480: scale is 0 because 500>>1 = 250 < 480, so native side 4000 > 1200 -> OVERSIZED */
    memcpy(oversized, jpeg_red_128, jpeg_red_128_size);
    CHECK(patch_sof0_size(oversized, jpeg_red_128_size, 4000, 500));
    CHECK(cover_decode_to_rgb565_ex(oversized, jpeg_red_128_size, 480, 480, ARTWORK_PRIO_PLAYER, NULL, NULL, &pixels) ==
          COVER_DECODE_FAIL_OVERSIZED);
    CHECK(pixels == NULL);

    /* 20000x20000 target 72x72: exceeds native cap 4096 and scaled (2500) > 1200 -> OVERSIZED */
    memcpy(oversized, jpeg_red_128, jpeg_red_128_size);
    CHECK(patch_sof0_size(oversized, jpeg_red_128_size, 20000, 20000));
    CHECK(cover_decode_to_rgb565_ex(oversized, jpeg_red_128_size, 72, 72, ARTWORK_PRIO_PLAYER, NULL, NULL, &pixels) ==
          COVER_DECODE_FAIL_OVERSIZED);
    CHECK(pixels == NULL);

    /* 2000x2000 target 72x72: scale 1/8 -> 250 <= 1200, dims are OK, so not OVERSIZED.
     * It fails later in decomp because scan data is still 128x128 fixture bytes. */
    memcpy(oversized, jpeg_red_128, jpeg_red_128_size);
    CHECK(patch_sof0_size(oversized, jpeg_red_128_size, 2000, 2000));
    cover_decode_result_t res_2k = cover_decode_to_rgb565_ex(oversized, jpeg_red_128_size, 72, 72,
                                                             ARTWORK_PRIO_PLAYER, NULL, NULL, &pixels);
    CHECK(res_2k != COVER_DECODE_FAIL_OVERSIZED);
    CHECK(pixels == NULL);
}

static void expect_zeroed_probe(const jpeg_probe_t * p) {
    CHECK(p->is_progressive == false);
    CHECK(p->supported == false);
    CHECK(p->native_w == 0);
    CHECK(p->native_h == 0);
    CHECK(p->coeff_bytes == 0);
}

static void test_jpeg_probe(void) {
    BEGIN_TEST("jpeg_probe");
    jpeg_probe_t p;

    /* (a) Existing 128x128 SOF0 baseline, 4:4:4. coeff_bytes is 0 for baseline. */
    memset(&p, 0xff, sizeof(p));
    CHECK(jpeg_probe(jpeg_red_128, jpeg_red_128_size, &p) == true);
    CHECK(p.supported == true);
    CHECK(p.is_progressive == false);
    CHECK(p.native_w == 128);
    CHECK(p.native_h == 128);
    CHECK(p.coeff_bytes == 0);

    /* (b) Real 48x40 SOF2, 4:2:0 (Y 2x2, Cb 1x1, Cr 1x1).
     *
     * Hand-verified coeff_bytes from jpeg_probe_t's documented formula
     * (MCU-round each component against Hmax/Vmax, 128 bytes per 8x8 block):
     *
     *   Hmax=2, Vmax=2  =>  MCU is (8*2) x (8*2) = 16 x 16
     *   mcus_per_row = ceil(48 / 16) = (48 + 16 - 1) / 16 = 3
     *   mcus_per_col = ceil(40 / 16) = (40 + 16 - 1) / 16 = 3
     *     (40 is not a multiple of 16: 2*16=32, remainder 8, so 3 MCU rows)
     *   Y  blocks: (3*2) x (3*2) = 6 x 6 = 36  * 128 = 4608
     *   Cb blocks: (3*1) x (3*1) = 3 x 3 =  9  * 128 = 1152
     *   Cr blocks: (3*1) x (3*1) = 3 x 3 =  9  * 128 = 1152
     *   coeff_bytes = 4608 + 1152 + 1152 = 6912
     *
     * Naive per-component ceil(w/8)*ceil(h/8) without MCU-rounding the
     * frame would give Y=6*5=30 blocks (3840) + 9 + 9 -> 6144, so 6912
     * is not an accident of copy-pasting the implementation's output. */
    memset(&p, 0xff, sizeof(p));
    CHECK(jpeg_probe(jpeg_prog_420_48x40, jpeg_prog_420_48x40_size, &p) == true);
    CHECK(p.supported == true);
    CHECK(p.is_progressive == true);
    CHECK(p.native_w == 48);
    CHECK(p.native_h == 40);
    CHECK(p.coeff_bytes == 6912ULL);

    /* (b2) End-to-end: the same real progressive JPEG actually decodes
     * successfully through decode_jpeg_progressive_rgb888() (not just
     * probed) -- both at a target smaller than native (exercises the
     * scale_num/scale_denom path) and at a target equal to native (no
     * scaling, exercises the 1:1 path). Real pixel content isn't checked
     * here (that's what a full JPEG decoder's own test suite is for) --
     * this specifically confirms the vendored-libjpeg wiring itself (mem
     * source, error manager, multi-scan jpeg_start_decompress(), scanline
     * loop, cleanup) produces a real, correctly-dimensioned RGB565 buffer
     * for a real progressive file, not just that header inspection works.
     * Explicit mock here (not relying on a later test function's own call
     * elsewhere in this file) so this decode's admission check is
     * deterministic regardless of the host machine's real free memory or
     * test execution order. */
    system_set_mock_mem_available(64U * 1024U * 1024U);
    uint16_t * prog_pixels = NULL;
    CHECK(cover_decode_to_rgb565_ex(jpeg_prog_420_48x40, jpeg_prog_420_48x40_size,
                                    16, 16, ARTWORK_PRIO_PLAYER, NULL, NULL, &prog_pixels) ==
          COVER_DECODE_OK);
    CHECK(prog_pixels != NULL);
    free(prog_pixels);

    prog_pixels = NULL;
    CHECK(cover_decode_to_rgb565_ex(jpeg_prog_420_48x40, jpeg_prog_420_48x40_size,
                                    48, 40, ARTWORK_PRIO_PLAYER, NULL, NULL, &prog_pixels) ==
          COVER_DECODE_OK);
    CHECK(prog_pixels != NULL);
    free(prog_pixels);

    /* (c) Synthetic SOF2 1600x100 4:2:0 -- probe succeeds; cover_decode
     * rejects native progressive side > MAX_DECODED_COVER_SIDE (1200).
     *
     *   Hmax=2, Vmax=2  =>  MCU 16 x 16
     *   mcus_per_row = ceil(1600 / 16) = 100
     *   mcus_per_col = ceil(100 / 16)  = (100 + 16 - 1) / 16 = 7
     *   Y  blocks: (100*2) x (7*2) = 200 x 14 = 2800 * 128 = 358400
     *   Cb blocks: (100*1) x (7*1) = 100 x  7 =  700 * 128 =  89600
     *   Cr blocks:                                          89600
     *   coeff_bytes = 358400 + 89600 + 89600 = 537600 */
    memset(&p, 0xff, sizeof(p));
    CHECK(jpeg_probe(jpeg_prog_oversized_1600x100, jpeg_prog_oversized_1600x100_size, &p) == true);
    CHECK(p.supported == true);
    CHECK(p.is_progressive == true);
    CHECK(p.native_w == 1600);
    CHECK(p.native_h == 100);
    CHECK(p.coeff_bytes == 537600ULL);

    system_set_mock_mem_available(64U * 1024U * 1024U);
    uint16_t * pixels = NULL;
    CHECK(cover_decode_to_rgb565_ex(jpeg_prog_oversized_1600x100, jpeg_prog_oversized_1600x100_size,
                                    480, 480, ARTWORK_PRIO_PLAYER, NULL, NULL, &pixels) ==
          COVER_DECODE_FAIL_OVERSIZED);
    CHECK(pixels == NULL);

    /* (d) SOF9 arithmetic sequential: SOF is reached, but unsupported. */
    memset(&p, 0xff, sizeof(p));
    CHECK(jpeg_probe(jpeg_sof9_arith_16, jpeg_sof9_arith_16_size, &p) == true);
    expect_zeroed_probe(&p);

    pixels = NULL;
    CHECK(cover_decode_to_rgb565_ex(jpeg_sof9_arith_16, jpeg_sof9_arith_16_size, 16, 16,
                                    ARTWORK_PRIO_PLAYER, NULL, NULL, &pixels) ==
          COVER_DECODE_FAIL_UNSUPPORTED);
    CHECK(pixels == NULL);

    /* (e) SOI present but no SOF -- reuse the existing truncated patterns
     * from test_malformed_and_oversized() rather than a new fixture. */
    const uint8_t truncated_soi[] = {0xFF, 0xD8, 0xFF, 0xC0};
    memset(&p, 0xff, sizeof(p));
    CHECK(jpeg_probe(truncated_soi, sizeof(truncated_soi), &p) == false);
    expect_zeroed_probe(&p);

    memset(&p, 0xff, sizeof(p));
    CHECK(jpeg_probe(jpeg_red_128, 12, &p) == false);
    expect_zeroed_probe(&p);

    const uint8_t garbage[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    memset(&p, 0xff, sizeof(p));
    CHECK(jpeg_probe(garbage, sizeof(garbage), &p) == false);
    expect_zeroed_probe(&p);
}

static void test_png_draw_buffer_ownership(void) {
    BEGIN_TEST("png_draw_buffer_ownership");
    const uint8_t signature[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
    mock_png = true;
    const unsigned errors[] = {0, 83, 1};
    const cover_decode_result_t expected[] = {
        COVER_DECODE_OK, COVER_DECODE_FAIL_ALLOC, COVER_DECODE_FAIL_UNSUPPORTED
    };
    for (unsigned i = 0; i < 3; i++) {
        mock_png_error = errors[i];
        uint16_t * pixels = NULL;
        unsigned destroyed = mock_png_destroyed;
        CHECK(cover_decode_to_rgb565_ex(signature, sizeof(signature), 2, 2,
              ARTWORK_PRIO_PLAYER, NULL, NULL, &pixels) == expected[i]);
        CHECK(mock_png_destroyed == destroyed + 1);
        if (i == 0) {
            CHECK(pixels != NULL);
            if (pixels) for (int p = 0; p < 4; p++) CHECK(pixels[p] == 0xf800);
        } else CHECK(pixels == NULL);
        free(pixels);
    }
    mock_png = false;
}

static uint16_t rgb565_pack8(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t) (((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static void test_cover_resize_rgb565(void) {
    BEGIN_TEST("cover_resize_rgb565");

    CHECK(cover_resize_rgb565(NULL, 4, 4, 2, 2) == NULL);

    /* Same-size is a byte-identical copy (memcpy fast path). The area-average
     * path at scale=1.0 is NOT identity -- inclusive floor windows average a
     * 2x2 neighborhood -- so this would fail without the fast path. */
    {
        const int side = 8;
        uint16_t src[64];
        for (int y = 0; y < side; y++) {
            for (int x = 0; x < side; x++) {
                src[(size_t) y * side + x] = (uint16_t) ((x << 8) | y);
            }
        }
        uint16_t * dst = cover_resize_rgb565(src, side, side, side, side);
        CHECK(dst != NULL);
        CHECK(dst != src);
        if (dst) {
            CHECK(memcmp(dst, src, sizeof(src)) == 0);
        }
        free(dst);
    }

    /* Uniform 4x4 red down to 2x2: every output pixel is that same red.
     * 0xF800 unpacks to (248,0,0); area-average of identical pixels is
     * itself; rgb888_to_565 packs back to 0xF800. */
    {
        uint16_t src[16];
        for (int i = 0; i < 16; i++) src[i] = 0xF800;
        uint16_t * dst = cover_resize_rgb565(src, 4, 4, 2, 2);
        CHECK(dst != NULL);
        if (dst) {
            CHECK(dst[0] == 0xF800);
            CHECK(dst[1] == 0xF800);
            CHECK(dst[2] == 0xF800);
            CHECK(dst[3] == 0xF800);
        }
        free(dst);
    }

    /* 2x2 of four known colors down to 1x1: area-average of all four.
     *
     * scale = 0.5, crop = 0; inclusive windows clamp to the full 2x2.
     * Unpacked 8-bit (left-shift expand):
     *   red   0xF800 -> (248, 0, 0)
     *   green 0x07E0 -> (0, 252, 0)
     *   blue  0x001F -> (0, 0, 248)
     *   white 0xFFFF -> (248, 252, 248)
     * Average: r=(248+0+0+248)/4=124, g=(0+252+0+252)/4=126, b=(0+0+248+248)/4=124
     * Pack: ((124&0xF8)<<8) | ((126&0xFC)<<3) | (124>>3) = 0x7BEF. */
    {
        uint16_t src[4] = {0xF800, 0x07E0, 0x001F, 0xFFFF};
        uint16_t * dst = cover_resize_rgb565(src, 2, 2, 1, 1);
        CHECK(dst != NULL);
        if (dst) {
            CHECK(dst[0] == rgb565_pack8(124, 126, 124));
            CHECK(dst[0] == 0x7BEF);
        }
        free(dst);
    }

    /* Non-square cover-fit center-crop: 8x4 -> 4x4.
     * scale_w=0.5, scale_h=1.0, scale=max=1.0, crop_x=(8-4)/2=2, crop_y=0.
     * Output columns sample source x starting at 2. Fill col 0 red, cols 1-6
     * green, col 7 blue -- the cropped window (x=2..6) is all green, so every
     * output pixel is green. A left-aligned crop would leak red at x=0; a
     * stretch/letterbox would mix the red/blue edges. */
    {
        uint16_t src[8 * 4];
        for (int y = 0; y < 4; y++) {
            for (int x = 0; x < 8; x++) {
                uint16_t c = 0x07E0;
                if (x == 0) c = 0xF800;
                if (x == 7) c = 0x001F;
                src[(size_t) y * 8 + x] = c;
            }
        }
        uint16_t * dst = cover_resize_rgb565(src, 8, 4, 4, 4);
        CHECK(dst != NULL);
        if (dst) {
            CHECK(is_green565(dst[0]));
            CHECK(is_green565(dst[3]));
            CHECK(is_green565(dst[4 * 2 + 2]));
            CHECK(is_green565(dst[4 * 3 + 0]));
            CHECK(is_green565(dst[4 * 3 + 3]));
            for (int i = 0; i < 16; i++) CHECK(dst[i] == 0x07E0);
        }
        free(dst);
    }

    /* Matching crop on the other axis: 4x8 -> 4x4 discards top/bottom edges. */
    {
        uint16_t src[4 * 8];
        for (int y = 0; y < 8; y++) {
            for (int x = 0; x < 4; x++) {
                uint16_t c = 0x07E0;
                if (y == 0) c = 0xF800;
                if (y == 7) c = 0x001F;
                src[(size_t) y * 4 + x] = c;
            }
        }
        uint16_t * dst = cover_resize_rgb565(src, 4, 8, 4, 4);
        CHECK(dst != NULL);
        if (dst) {
            for (int i = 0; i < 16; i++) CHECK(dst[i] == 0x07E0);
        }
        free(dst);
    }
}

int main(void) {
    test_png_draw_buffer_ownership();
    test_jpeg_probe();
    test_jpeg_scale_for_target();
    test_tjpgd_scaled_geometry_and_bgr();
    test_cover_decode_red_each_scale();
    test_cover_decode_quadrants_each_scale();
    test_malformed_and_oversized();
    test_cover_resize_rgb565();

    if (g_failures == 0) {
        fprintf(stderr, "All cover_decode_scale tests passed.\n");
        return 0;
    }
    fprintf(stderr, "%d cover_decode_scale test assertion(s) failed.\n", g_failures);
    return 1;
}
