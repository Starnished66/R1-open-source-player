/* Host tests for JPEG cover decode: scale selection, tjpgd 1/2 1/4 1/8
 * output geometry and BGR order, cover_decode_to_rgb565_ex cover-fit, and
 * malformed/oversized rejection.
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

/* cover_decode.c also references lodepng; this JPEG-only test does not link
 * lodepng.c (it pulls the rest of LVGL). Stubs make PNG inspect/decode fail. */
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
    if (w) *w = 0;
    if (h) *h = 0;
    return 1;
}
unsigned lodepng_decode24(unsigned char ** out, unsigned * w, unsigned * h, const unsigned char * in, size_t insize) {
    (void) in;
    (void) insize;
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

int main(void) {
    test_jpeg_scale_for_target();
    test_tjpgd_scaled_geometry_and_bgr();
    test_cover_decode_red_each_scale();
    test_cover_decode_quadrants_each_scale();
    test_malformed_and_oversized();

    if (g_failures == 0) {
        fprintf(stderr, "All cover_decode_scale tests passed.\n");
        return 0;
    }
    fprintf(stderr, "%d cover_decode_scale test assertion(s) failed.\n", g_failures);
    return 1;
}
