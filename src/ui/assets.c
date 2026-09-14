#include "assets.h"
#include "frosted_glass.h"

#include "lvgl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef HOST_BUILD
  #define THEME_ROOT "assets/theme2/"
#else
  /* Present on every real R1 as part of the stock firmware itself. */
  #define THEME_ROOT "/usr/resource/litegui/theme2/"
  /* Writable override, checked first by asset_path(), for custom assets
   * not present in the read-only THEME_ROOT squashfs pack. /usr/data is the
   * persistent writable partition. */
  #define THEME_OVERRIDE_ROOT "/usr/data/theme_overrides/"
#endif

void assets_init(void) {
    lv_lodepng_init();
    lv_fs_posix_init();
    lv_tjpgd_init();
    lv_fs_memfs_init();
}

typedef struct asset_path_entry {
    struct asset_path_entry * next;
    char value[];
} asset_path_entry_t;

static asset_path_entry_t * asset_paths;

/* LVGL keeps raw path pointers in some styles. Intern the fully resolved
 * value so the pointer remains stable without leaking another strdup on
 * every soft UI rebuild. The set is bounded by native asset names times
 * the two possible roots (stock/override), not by reload count. */
static const char * asset_path_intern(const char * value) {
    for (asset_path_entry_t * e = asset_paths; e; e = e->next) {
        if (strcmp(e->value, value) == 0) return e->value;
    }
    size_t len = strlen(value);
    asset_path_entry_t * e = malloc(sizeof(*e) + len + 1);
    if (!e) return NULL;
    memcpy(e->value, value, len + 1);
    e->next = asset_paths;
    asset_paths = e;
    return e->value;
}

const char * asset_path(const char * relative_path) {
    char buf[320];
#ifndef HOST_BUILD
    /* One access() check, at screen-build time only (every caller resolves
     * its own path once when the screen is constructed, never per-frame),
     * so the extra syscall is negligible -- see THEME_OVERRIDE_ROOT's own
     * comment for why this exists at all. */
    snprintf(buf, sizeof(buf), THEME_OVERRIDE_ROOT "%s", relative_path);
    if (access(buf, R_OK) == 0) {
        snprintf(buf, sizeof(buf), "S:" THEME_OVERRIDE_ROOT "%s", relative_path);
        return asset_path_intern(buf);
    }
#endif
    snprintf(buf, sizeof(buf), "S:" THEME_ROOT "%s", relative_path);
    return asset_path_intern(buf);
}

const char * asset_path_plain(const char * relative_path) {
    char buf[320];
#ifndef HOST_BUILD
    snprintf(buf, sizeof(buf), THEME_OVERRIDE_ROOT "%s", relative_path);
    if (access(buf, R_OK) == 0) {
        return asset_path_intern(buf);
    }
    snprintf(buf, sizeof(buf), THEME_ROOT "%s", relative_path);
#else
    /* THEME_ROOT is a relative path on host ("assets/theme2/", resolved
     * against the process's own CWD, same as MUSIC_ROOT_DIR's own "./music"
     * elsewhere) -- fine for asset_path()'s "S:"-prefixed LVGL image source
     * (LVGL's POSIX fs driver just opens it via the process's normal CWD),
     * but this function's whole point is a path callers like pill_row_
     * apply_icon() can tell apart from a plugin-relative one purely by
     * whether it starts with '/' -- so unlike asset_path(), this needs a
     * genuinely absolute path even on host. */
    char cwd[256];
    if (getcwd(cwd, sizeof(cwd))) {
        snprintf(buf, sizeof(buf), "%s/" THEME_ROOT "%s", cwd, relative_path);
    } else {
        snprintf(buf, sizeof(buf), THEME_ROOT "%s", relative_path);
    }
#endif
    return asset_path_intern(buf);
}

const lv_image_dsc_t * asset_png_memory(const char * relative_path) {
    const char * resolved = asset_path(relative_path);
    const char * path = resolved;
    if (path[0] && path[1] == ':') path += 2; /* strip LVGL's POSIX drive prefix */

    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0 || st.st_size > 1024 * 1024) return NULL;
    FILE * f = fopen(path, "rb");
    if (!f) return NULL;
    uint8_t * data = malloc((size_t) st.st_size);
    bool ok = data && fread(data, 1, (size_t) st.st_size, f) == (size_t) st.st_size;
    fclose(f);
    if (!ok) { free(data); return NULL; }

    lv_image_dsc_t * dsc = calloc(1, sizeof(*dsc));
    if (!dsc) { free(data); return NULL; }
    dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc->data = data;
    dsc->data_size = (uint32_t) st.st_size;
    return dsc;
}

void asset_png_memory_free(const lv_image_dsc_t * image) {
    if (!image) return;
    free((void *) image->data);
    free((void *) image);
}

bool asset_decoded_image_open(asset_decoded_image_t * image, const char * relative_path) {
    if (!image || !relative_path) return false;
    asset_decoded_image_close(image);

    const char * resolved = asset_path(relative_path);
    image->path = resolved ? strdup(resolved) : NULL;
    if (!image->path) return false;

    lv_image_decoder_args_t args = { .no_cache = true };
    if (lv_image_decoder_open(&image->decoder, image->path, &args) != LV_RESULT_OK ||
        !image->decoder.decoded) {
        free(image->path);
        memset(image, 0, sizeof(*image));
        return false;
    }
    image->open = true;
    return true;
}

void asset_decoded_image_close(asset_decoded_image_t * image) {
    if (!image) return;
    if (image->open) lv_image_decoder_close(&image->decoder);
    free(image->path);
    memset(image, 0, sizeof(*image));
}

bool asset_decoded_gradient_open(asset_decoded_image_t * image, const char * relative_path) {
    if (!asset_decoded_image_open(image, relative_path)) return false;
    const lv_draw_buf_t * buffer = image->decoder.decoded;
    if (buffer->header.cf != LV_COLOR_FORMAT_ARGB8888) return true;
    /* Keep the decoder's owned ARGB buffer and its alpha plane. Expanding
     * the dithered 565 result back to 888 makes the eventual framebuffer
     * conversion exact without changing LVGL's image/decoder ownership. */
    for (uint32_t y = 0; y < buffer->header.h; ++y) {
        lv_color32_t * row = (lv_color32_t *) (buffer->data + y * buffer->header.stride);
        for (uint32_t x = 0; x < buffer->header.w; ++x) {
            uint16_t pixel = rgb888_to_565_dithered(row[x].red, row[x].green, row[x].blue, x, y);
            uint8_t r = (pixel >> 11) & 31, g = (pixel >> 5) & 63, b = pixel & 31;
            row[x].red = (r << 3) | (r >> 2);
            row[x].green = (g << 2) | (g >> 4);
            row[x].blue = (b << 3) | (b >> 2);
        }
    }
    return true;
}

const void * asset_decoded_image_source(const asset_decoded_image_t * image) {
    return image && image->open ? image->decoder.decoded : NULL;
}
