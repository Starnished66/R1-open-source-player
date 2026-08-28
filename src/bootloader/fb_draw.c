#include "fb_draw.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/fb.h>

#include "lvgl/src/libs/tjpgd/tjpgd.h"

static int fb_fd = -1;
static uint16_t * fb_mem = NULL;
static size_t fb_mem_len = 0;
static int fb_stride_pixels = 0; /* line_length in PIXELS, not bytes -- see fb_open()'s own comment */
/* Pixel offset of the CURRENTLY DISPLAYED page's first pixel within
 * fb_mem, from vinfo.xoffset/yoffset -- see fb_open()'s own comment on why
 * this can be nonzero on this exact hardware. */
static int fb_base_offset_pixels = 0;

/* Compact (no stride padding) heap copy of the decoded background, for
 * fb_restore_background()'s fast per-frame blit -- see fb_draw.h's own
 * doc comment on why this exists instead of re-decoding the JPEG on every
 * redraw tick. NULL until fb_draw_background_jpeg() succeeds at least
 * once. Freed by fb_close() -- this bootloader stays alive as the
 * fork/waitpid supervisor for however long the chosen player runs (see
 * main.c's own run_player_supervised()), so this 768000-byte allocation
 * must not just be left pinned for that entire lifetime once the
 * framebuffer itself is done with it; that matters on this 56 MiB
 * device even though it's a supervisor process doing nothing but
 * waitpid() by that point. */
static uint16_t * bg_cache = NULL;

/* Every drawing primitive below (put_pixel/get_pixel, and therefore
 * everything built on them) targets THIS compact, no-stride, always-
 * (0,0)-based off-screen buffer -- never fb_mem directly. Real-device
 * finding: with drawing going straight to the visible, currently-scanned-
 * out page (the original design), every redraw's multiple separate steps
 * (restore background, title, countdown, cards' alpha-blended fills,
 * borders, text) were each independently visible mid-sequence, and the
 * repeated background-restore-then-redraw cycle every countdown tick
 * produced a real, visible flicker -- worse once card fills became
 * alpha-blended (more per-pixel work per frame, a longer partial-frame
 * window for the scanout to catch). fb_flush() is what actually copies
 * this to the real, visible fb_mem, in one tight per-row loop -- turning
 * many small, independently-visible writes into one fast bulk update. */
static uint16_t * back_buffer = NULL;

/* 5x7 dot-matrix font, uppercase + digits + space only. Deliberately not a
 * full ASCII table -- every string this bootloader ever draws is
 * hand-written UI text, known in full at the time this was written (see
 * main.c's own string literals), so the font only needs to cover the
 * characters those strings actually use. Each glyph is 7 bytes, one per
 * row top-to-bottom, bits 4..0 = leftmost..rightmost column. The ASCII-art
 * comment above each glyph is the source of truth this was transcribed
 * from -- if a character ever looks wrong on real hardware, compare
 * against its own comment first, not just the hex. */
static const uint8_t * glyph_rows(char c) {
    static const uint8_t blank[7] = { 0, 0, 0, 0, 0, 0, 0 };

    switch (c) {
        /* .###.   #...#   #..##   #.#.#   ##..#   #...#   .###. */
        case '0': { static const uint8_t g[7] = { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E }; return g; }
        /* ..#..   .##..   ..#..   ..#..   ..#..   ..#..   .###. */
        case '1': { static const uint8_t g[7] = { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E }; return g; }
        /* .###.   #...#   ....#   ...#.   ..#..   .#...   ##### */
        case '2': { static const uint8_t g[7] = { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F }; return g; }
        /* .###.   #...#   ....#   ..##.   ....#   #...#   .###. */
        case '3': { static const uint8_t g[7] = { 0x0E, 0x11, 0x01, 0x06, 0x01, 0x11, 0x0E }; return g; }
        /* ...#.   ..##.   .#.#.   #..#.   #####   ...#.   ...#. */
        case '4': { static const uint8_t g[7] = { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 }; return g; }
        /* #####   #....   ####.   ....#   ....#   #...#   .###. */
        case '5': { static const uint8_t g[7] = { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E }; return g; }
        /* ..##.   .#...   #....   ####.   #...#   #...#   .###. */
        case '6': { static const uint8_t g[7] = { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E }; return g; }
        /* #####   ....#   ...#.   ..#..   .#...   .#...   .#... */
        case '7': { static const uint8_t g[7] = { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 }; return g; }
        /* .###.   #...#   #...#   .###.   #...#   #...#   .###. */
        case '8': { static const uint8_t g[7] = { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E }; return g; }
        /* .###.   #...#   #...#   .####   ....#   ...#.   .##.. */
        case '9': { static const uint8_t g[7] = { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C }; return g; }

        /* ..#..   .#.#.   #...#   #...#   #####   #...#   #...# */
        case 'A': { static const uint8_t g[7] = { 0x04, 0x0A, 0x11, 0x11, 0x1F, 0x11, 0x11 }; return g; }
        /* ####.   #...#   #...#   ####.   #...#   #...#   ####. */
        case 'B': { static const uint8_t g[7] = { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E }; return g; }
        /* .####   #....   #....   #....   #....   #....   .#### */
        case 'C': { static const uint8_t g[7] = { 0x0F, 0x10, 0x10, 0x10, 0x10, 0x10, 0x0F }; return g; }
        /* ####.   #...#   #...#   #...#   #...#   #...#   ####. */
        case 'D': { static const uint8_t g[7] = { 0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E }; return g; }
        /* #####   #....   #....   ####.   #....   #....   ##### */
        case 'E': { static const uint8_t g[7] = { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F }; return g; }
        /* .###.   ..#..   ..#..   ..#..   ..#..   ..#..   .###. */
        case 'I': { static const uint8_t g[7] = { 0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E }; return g; }
        /* #...#   #..#.   #.#..   ##...   #.#..   #..#.   #...# */
        case 'K': { static const uint8_t g[7] = { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 }; return g; }
        /* #....   #....   #....   #....   #....   #....   ##### */
        case 'L': { static const uint8_t g[7] = { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F }; return g; }
        /* #...#   ##.##   #.#.#   #...#   #...#   #...#   #...# */
        case 'M': { static const uint8_t g[7] = { 0x11, 0x1B, 0x15, 0x11, 0x11, 0x11, 0x11 }; return g; }
        /* #...#   ##..#   #.#.#   #..##   #...#   #...#   #...# */
        case 'N': { static const uint8_t g[7] = { 0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11 }; return g; }
        /* .###.   #...#   #...#   #...#   #...#   #...#   .###. */
        case 'O': { static const uint8_t g[7] = { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E }; return g; }
        /* ####.   #...#   #...#   ####.   #....   #....   #.... */
        case 'P': { static const uint8_t g[7] = { 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 }; return g; }
        /* ####.   #...#   #...#   ####.   #.#..   #..#.   #...# */
        case 'R': { static const uint8_t g[7] = { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 }; return g; }
        /* .####   #....   #....   .###.   ....#   ....#   ####. */
        case 'S': { static const uint8_t g[7] = { 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E }; return g; }
        /* #####   ..#..   ..#..   ..#..   ..#..   ..#..   ..#.. */
        case 'T': { static const uint8_t g[7] = { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 }; return g; }
        /* #...#   #...#   #...#   #...#   #...#   #...#   .###. */
        case 'U': { static const uint8_t g[7] = { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E }; return g; }
        /* #...#   #...#   #...#   #...#   #...#   .#.#.   ..#.. */
        case 'V': { static const uint8_t g[7] = { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04 }; return g; }
        /* #...#   #...#   .#.#.   ..#..   ..#..   ..#..   ..#.. */
        case 'Y': { static const uint8_t g[7] = { 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04 }; return g; }

        case ' ': return blank;
        default: return blank; /* unsupported char -- a visible gap, not a silent shift (see fb_draw.h) */
    }
}

#define GLYPH_COLS 5
#define GLYPH_ROWS 7
#define GLYPH_SPACING 1 /* blank column between characters */
#define GLYPH_CELL_W (GLYPH_COLS + GLYPH_SPACING)

/* RGB565 specifically, not just "some 16bpp format" -- bits_per_pixel==16
 * alone doesn't rule out RGB555/BGR565/ARGB4444, all also 16bpp. fb_rgb()
 * packs red at bits 15..11, green at 10..5, blue at 4..0; verifying the
 * driver reports that exact layout catches a wrong assumption here loudly
 * (fb_open() returning false) instead of silently drawing
 * correctly-shaped garbage in the wrong channels. */
static bool is_rgb565_layout(const struct fb_var_screeninfo * vinfo) {
    return vinfo->red.offset == 11 && vinfo->red.length == 5 &&
           vinfo->green.offset == 5 && vinfo->green.length == 6 &&
           vinfo->blue.offset == 0 && vinfo->blue.length == 5;
}

/* Same retry shape as src/main.c's own wait_for_fbdev_ready() -- the
 * device node can exist before the underlying driver has actually
 * settled, and a bootloader runs even earlier in boot than the main
 * player (which already needed this same retry). fb_draw.h's own doc
 * comment promises this; an earlier version of this function opened once
 * and gave up immediately, silently falling back to booting the persisted
 * default with no menu shown on what was actually just a transient,
 * recoverable startup race. */
#define FB_READY_MAX_ATTEMPTS 50
#define FB_READY_DELAY_MS 100

bool fb_open(void) {
    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;
    bool ready = false;

    for (int attempt = 0; attempt < FB_READY_MAX_ATTEMPTS; attempt++) {
        fb_fd = open("/dev/fb0", O_RDWR);
        if (fb_fd >= 0) {
            if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo) == 0 && ioctl(fb_fd, FBIOGET_FSCREENINFO, &finfo) == 0) {
                ready = true;
                break;
            }
            close(fb_fd);
            fb_fd = -1;
        }
        usleep(FB_READY_DELAY_MS * 1000);
    }
    if (!ready) {
        perror("fb_draw: /dev/fb0 not ready");
        return false;
    }

    /* Confirmed geometry/format (see fb_draw.h's own comment) -- refusing
     * to draw into anything else is deliberate: a bootloader guessing
     * wrong about pixel format would silently corrupt the display instead
     * of failing loudly, and main.c's own fast-path (no menu needed) still
     * works fine even if this returns false. */
    if (vinfo.xres != FB_WIDTH || vinfo.yres != FB_HEIGHT || vinfo.bits_per_pixel != 16 || !is_rgb565_layout(&vinfo)) {
        fprintf(stderr, "fb_draw: unexpected fb format %ux%u @ %ubpp r%u:%u g%u:%u b%u:%u (expected %dx%d RGB565)\n",
                vinfo.xres, vinfo.yres, vinfo.bits_per_pixel, vinfo.red.offset, vinfo.red.length,
                vinfo.green.offset, vinfo.green.length, vinfo.blue.offset, vinfo.blue.length, FB_WIDTH, FB_HEIGHT);
        close(fb_fd);
        fb_fd = -1;
        return false;
    }

    fb_mem_len = finfo.smem_len;
    fb_mem = mmap(NULL, fb_mem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
    if (fb_mem == MAP_FAILED) {
        perror("fb_draw: mmap");
        fb_mem = NULL;
        close(fb_fd);
        fb_fd = -1;
        return false;
    }

    /* line_length is in BYTES and may exceed xres*2 (row padding) --
     * everything below indexes fb_mem as uint16_t*, so this converts once
     * up front rather than dividing by 2 at every single pixel write. */
    fb_stride_pixels = (int) (finfo.line_length / sizeof(uint16_t));

    /* This panel supports panned double-buffering (confirmed by reading
     * lvgl/src/drivers/display/fb/lv_linux_fbdev.c directly: it toggles
     * vinfo.yoffset between 0 and vinfo.yres when yres_virtual >=
     * yres*2). Whichever page the player last left visible is whatever
     * vinfo.yoffset/xoffset already are right now -- writing at a fixed
     * offset 0 would silently draw into the CURRENTLY INVISIBLE page if
     * that happened to be page 1, with no error and nothing ever
     * appearing on screen. */
    fb_base_offset_pixels = (int) vinfo.yoffset * fb_stride_pixels + (int) vinfo.xoffset;

    /* Validate the full drawable range this offset implies actually fits
     * inside the mapped region before ever writing to it -- a driver
     * reporting a yoffset/xoffset that doesn't leave room for a full
     * FB_WIDTH x FB_HEIGHT image would otherwise write past fb_mem_len. */
    size_t last_pixel_index = (size_t) fb_base_offset_pixels + (size_t) (FB_HEIGHT - 1) * fb_stride_pixels + (FB_WIDTH - 1);
    if ((last_pixel_index + 1) * sizeof(uint16_t) > fb_mem_len) {
        fprintf(stderr, "fb_draw: current page offset (yoffset=%u xoffset=%u) leaves no room for a full frame "
                        "in a %zu-byte mapping\n",
                vinfo.yoffset, vinfo.xoffset, fb_mem_len);
        munmap(fb_mem, fb_mem_len);
        fb_mem = NULL;
        close(fb_fd);
        fb_fd = -1;
        return false;
    }

    /* See back_buffer's own doc comment -- everything drawn goes here
     * first, never straight to fb_mem. A failed allocation here means
     * nothing can be drawn at all, same severity as the geometry/mmap
     * failures above -- fail fb_open() itself rather than returning true
     * and having every draw call silently no-op against a NULL buffer. */
    back_buffer = malloc((size_t) FB_WIDTH * FB_HEIGHT * sizeof(uint16_t));
    if (!back_buffer) {
        perror("fb_draw: back buffer malloc");
        munmap(fb_mem, fb_mem_len);
        fb_mem = NULL;
        close(fb_fd);
        fb_fd = -1;
        return false;
    }

    return true;
}

void fb_close(void) {
    if (fb_mem) {
        munmap(fb_mem, fb_mem_len);
        fb_mem = NULL;
    }
    if (fb_fd >= 0) {
        close(fb_fd);
        fb_fd = -1;
    }
    /* See bg_cache's/back_buffer's own doc comments -- this process
     * outlives the framebuffer being open (it stays alive as the player
     * supervisor), so neither must just be left pinned once the fb itself
     * is closed. */
    free(bg_cache);
    bg_cache = NULL;
    free(back_buffer);
    back_buffer = NULL;
}

fb_color_t fb_rgb(uint8_t r, uint8_t g, uint8_t b) {
    return (fb_color_t) (((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

/* Targets back_buffer -- see that variable's own doc comment for why
 * nothing draws straight to fb_mem anymore. back_buffer has no stride
 * padding (it's this bootloader's own private, always-(0,0)-based
 * layout), so indexing is a plain y*FB_WIDTH+x, no fb_base_offset_pixels
 * involved here -- that offset only matters at the actual fb_flush()
 * blit out to the real, possibly-panned mmap'd framebuffer. */
static inline void put_pixel(int x, int y, fb_color_t color) {
    if (!back_buffer || x < 0 || y < 0 || x >= FB_WIDTH || y >= FB_HEIGHT) return;
    back_buffer[(size_t) y * FB_WIDTH + x] = color;
}

static inline fb_color_t get_pixel(int x, int y) {
    if (!back_buffer || x < 0 || y < 0 || x >= FB_WIDTH || y >= FB_HEIGHT) return 0;
    return back_buffer[(size_t) y * FB_WIDTH + x];
}

void fb_fill(fb_color_t color) {
    fb_fill_rect(0, 0, FB_WIDTH, FB_HEIGHT, color);
}

void fb_fill_rect(int x, int y, int w, int h, fb_color_t color) {
    if (w <= 0 || h <= 0) return;
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w > FB_WIDTH ? FB_WIDTH : x + w;
    int y1 = y + h > FB_HEIGHT ? FB_HEIGHT : y + h;
    for (int py = y0; py < y1; py++) {
        for (int px = x0; px < x1; px++) put_pixel(px, py, color);
    }
}

/* Blends `color` over whatever is already on screen -- alpha 0 leaves the
 * existing pixel untouched, 255 is equivalent to fb_fill_rect(). Blends in
 * RGB565's own 5/6/5 precision directly (no round-trip through 8-bit per
 * channel) -- plenty for a UI overlay effect, and avoids fb_rgb()'s own
 * packing/unpacking for every pixel of every card, every redraw tick. */
void fb_fill_rect_alpha(int x, int y, int w, int h, fb_color_t color, uint8_t alpha) {
    if (w <= 0 || h <= 0) return;
    if (alpha == 255) { fb_fill_rect(x, y, w, h, color); return; }
    if (alpha == 0) return;

    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w > FB_WIDTH ? FB_WIDTH : x + w;
    int y1 = y + h > FB_HEIGHT ? FB_HEIGHT : y + h;

    uint16_t cr = (color >> 11) & 0x1F, cg = (color >> 5) & 0x3F, cb = color & 0x1F;
    uint16_t inv_alpha = 255 - alpha;

    for (int py = y0; py < y1; py++) {
        for (int px = x0; px < x1; px++) {
            fb_color_t bg = get_pixel(px, py);
            uint16_t br = (bg >> 11) & 0x1F, bgc = (bg >> 5) & 0x3F, bb = bg & 0x1F;
            uint8_t r = (uint8_t) ((cr * alpha + br * inv_alpha) / 255);
            uint8_t g = (uint8_t) ((cg * alpha + bgc * inv_alpha) / 255);
            uint8_t b = (uint8_t) ((cb * alpha + bb * inv_alpha) / 255);
            put_pixel(px, py, (fb_color_t) ((r << 11) | (g << 5) | b));
        }
    }
}

void fb_draw_rect_border(int x, int y, int w, int h, int thickness, fb_color_t color) {
    if (thickness <= 0) return;
    fb_fill_rect(x, y, w, thickness, color);                       /* top */
    fb_fill_rect(x, y + h - thickness, w, thickness, color);       /* bottom */
    fb_fill_rect(x, y, thickness, h, color);                       /* left */
    fb_fill_rect(x + w - thickness, y, thickness, h, color);       /* right */
}

/* scale=3 keeps each font pixel a visible 3x3 block on this 480x800 panel
 * without needing anti-aliasing -- a plain, blocky look is fine for a boot
 * menu and matches the deliberately minimal font above. */
#define GLYPH_SCALE 3

void fb_draw_text(int x, int y, const char * text, fb_color_t color) {
    int cursor_x = x;
    for (const char * p = text; *p; p++) {
        const uint8_t * rows = glyph_rows(*p);
        for (int row = 0; row < GLYPH_ROWS; row++) {
            uint8_t bits = rows[row];
            for (int col = 0; col < GLYPH_COLS; col++) {
                if (bits & (1u << (GLYPH_COLS - 1 - col))) {
                    fb_fill_rect(cursor_x + col * GLYPH_SCALE, y + row * GLYPH_SCALE,
                                GLYPH_SCALE, GLYPH_SCALE, color);
                }
            }
        }
        cursor_x += GLYPH_CELL_W * GLYPH_SCALE;
    }
}

int fb_text_width(const char * text) {
    return (int) strlen(text) * GLYPH_CELL_W * GLYPH_SCALE;
}

int fb_text_height(void) {
    return GLYPH_ROWS * GLYPH_SCALE;
}

void fb_flush(void) {
    /* The actual visible update -- one tight per-row copy from back_buffer
     * (no stride) into the real, possibly-panned fb_mem (see fb_open()'s
     * own comment on fb_base_offset_pixels/fb_stride_pixels). Everything
     * drawn since the last call to this landed only in back_buffer, never
     * visible on screen until now -- see back_buffer's own doc comment on
     * why every draw call site still calls this exactly where it always
     * did (no main.c changes needed for this fix). */
    if (!fb_mem || !back_buffer) return;
    for (int y = 0; y < FB_HEIGHT; y++) {
        memcpy(fb_mem + (size_t) fb_base_offset_pixels + (size_t) y * fb_stride_pixels,
              back_buffer + (size_t) y * FB_WIDTH, (size_t) FB_WIDTH * sizeof(uint16_t));
    }
}

typedef struct {
    FILE * f;
} jpeg_file_ctx_t;

/* tjpgd calls this with buff==NULL to mean "skip ndata bytes without
 * needing them" (confirmed by reading tjpgd.c directly -- it does this
 * for JPEG segments it has no use for), not just to fill a caller buffer
 * -- must fseek() rather than treat NULL as an error. */
static size_t jpeg_file_read(JDEC * jd, uint8_t * buff, size_t ndata) {
    jpeg_file_ctx_t * ctx = (jpeg_file_ctx_t *) jd->device;
    if (!buff) return fseek(ctx->f, (long) ndata, SEEK_CUR) == 0 ? ndata : 0;
    return fread(buff, 1, ndata, ctx->f);
}

/* JD_FORMAT is 0 in tjpgdcnf.h (shared with the main player's own cover-art
 * decode) -- bitmap here is already plain RGB888, 3 bytes/pixel, no BGR
 * swap needed (that swap in cover_decode.c's own output callback is for a
 * DIFFERENT reason specific to its own pipeline -- this one is required
 * regardless of any downstream pipeline: tjpgd's own conversion loop
 * writes bytes in B,G,R order despite JD_FORMAT's own comment calling it
 * "RGB888"; cover_decode.c's jpeg_mem_output() already swaps indices 0
 * and 2 for exactly this, and without the same swap here every baseline
 * JPEG this draws has its red and blue channels swapped). Writes straight
 * into the framebuffer via put_pixel() (which already accounts for
 * fb_base_offset_pixels) instead of an intermediate buffer -- there is no
 * resize/crop step to justify one, see fb_draw_background_jpeg()'s own
 * doc comment on why. */
static int jpeg_file_output_to_fb(JDEC * jd, void * bitmap, JRECT * rect) {
    (void) jd;
    int w = rect->right - rect->left + 1;
    int h = rect->bottom - rect->top + 1;
    const uint8_t * src = (const uint8_t *) bitmap;

    for (int y = 0; y < h; y++) {
        int dy = rect->top + y;
        for (int x = 0; x < w; x++) {
            int dx = rect->left + x;
            const uint8_t * p = src + (size_t) (y * w + x) * 3;
            put_pixel(dx, dy, fb_rgb(p[2], p[1], p[0])); /* R <- src[2], G <- src[1], B <- src[0] -- see this function's own comment */
        }
    }
    return 1;
}

bool fb_draw_background_jpeg(const char * path) {
    FILE * f = fopen(path, "rb");
    if (!f) {
        perror("fb_draw: fopen background jpeg");
        return false;
    }

    jpeg_file_ctx_t ctx = { .f = f };
    uint8_t workbuf[4096];
    JDEC jd;

    JRESULT prep_res = jd_prepare(&jd, jpeg_file_read, workbuf, sizeof(workbuf), &ctx);
    if (prep_res != JDR_OK) {
        fprintf(stderr, "fb_draw: jd_prepare(%s) failed: %d\n", path, (int) prep_res);
        fclose(f);
        return false;
    }
    /* Exact match only -- see this function's own doc comment on why this
     * deliberately doesn't scale/crop to fit instead of refusing. */
    if ((int) jd.width != FB_WIDTH || (int) jd.height != FB_HEIGHT) {
        fprintf(stderr, "fb_draw: background jpeg %s is %ux%u, expected %dx%d\n",
                path, jd.width, jd.height, FB_WIDTH, FB_HEIGHT);
        fclose(f);
        return false;
    }

    JRESULT decomp_res = jd_decomp(&jd, jpeg_file_output_to_fb, 0);
    bool ok = decomp_res == JDR_OK;
    if (!ok) fprintf(stderr, "fb_draw: jd_decomp(%s) failed: %d\n", path, (int) decomp_res);
    fclose(f);

    if (ok) {
        /* jpeg_file_output_to_fb() decoded into back_buffer (see put_pixel()'s
         * own doc comment) -- both it and bg_cache are the same compact,
         * no-stride layout now, so caching it is one plain memcpy, not a
         * per-row loop against fb_mem's own (possibly padded) stride. */
        free(bg_cache); /* replace any previous cache rather than leaking it -- harmless if this is ever called twice */
        bg_cache = malloc((size_t) FB_WIDTH * FB_HEIGHT * sizeof(uint16_t));
        if (bg_cache) memcpy(bg_cache, back_buffer, (size_t) FB_WIDTH * FB_HEIGHT * sizeof(uint16_t));
        /* A failed malloc here just means fb_restore_background() falls
         * back to a flat fill on every subsequent redraw -- this frame,
         * the one just decoded, still stays on screen until the next
         * fb_flush() actually happens (not blanked immediately), so this
         * isn't a visible regression even under memory pressure. */
    }
    return ok;
}

void fb_restore_background(fb_color_t fallback_color) {
    if (!bg_cache) {
        fb_fill(fallback_color);
        return;
    }
    /* Same compact layout as back_buffer -- one plain memcpy, no stride
     * math (that only applies at the actual fb_flush() blit to fb_mem). */
    memcpy(back_buffer, bg_cache, (size_t) FB_WIDTH * FB_HEIGHT * sizeof(uint16_t));
}
