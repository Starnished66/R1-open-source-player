#include "screenshot.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#ifdef HOST_BUILD

#include "../ui/gui_notifications.h"

screenshot_result_t screenshot_start(void) {
    show_error_toast("Screenshots unavailable");
    return SCREENSHOT_RESULT_UNAVAILABLE;
}

const char * screenshot_result_reason(screenshot_result_t result) {
    (void) result;
    return "unavailable";
}

bool screenshot_poll_completion(screenshot_completion_t * out) {
    (void) out;
    return false;
}

bool screenshot_is_busy(void) {
    return false;
}

#else

#include "../hardware/backlight.h"
#include "../ui/gui_library.h"
#include "../ui/gui_network.h"
#include "../ui/gui_notifications.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/fb.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "../../lvgl/src/libs/lodepng/lodepng.h"

#define MUSIC_ROOT_DIR "/data/mnt/sd_0"
#define SCREENSHOT_DIR MUSIC_ROOT_DIR "/Screenshots"
#define SCREENSHOT_MAX_DIMENSION 8192U

static pthread_mutex_t screenshot_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool screenshot_busy;
static bool screenshot_done;
static screenshot_completion_t screenshot_completion;

typedef struct {
    uint16_t * pixels;
    unsigned width;
    unsigned height;
} screenshot_job_t;

static void screenshot_set_idle(void) {
    pthread_mutex_lock(&screenshot_mutex);
    screenshot_busy = false;
    screenshot_done = false;
    memset(&screenshot_completion, 0, sizeof(screenshot_completion));
    pthread_mutex_unlock(&screenshot_mutex);
}

static void screenshot_set_completion(bool saved, const char * path, const char * reason) {
    pthread_mutex_lock(&screenshot_mutex);
    screenshot_completion.saved = saved;
    snprintf(screenshot_completion.path, sizeof(screenshot_completion.path), "%s", path ? path : "");
    snprintf(screenshot_completion.reason, sizeof(screenshot_completion.reason), "%s", reason ? reason : "");
    screenshot_done = true;
    pthread_mutex_unlock(&screenshot_mutex);
}

static bool framebuffer_is_rgb565(const struct fb_var_screeninfo * var,
                                  const struct fb_fix_screeninfo * fix) {
    return var->bits_per_pixel == 16 && fix->visual == FB_VISUAL_TRUECOLOR &&
           var->red.offset == 11 && var->red.length == 5 && var->red.msb_right == 0 &&
           var->green.offset == 5 && var->green.length == 6 && var->green.msb_right == 0 &&
           var->blue.offset == 0 && var->blue.length == 5 && var->blue.msb_right == 0 &&
           var->transp.length == 0;
}

static void log_framebuffer_format(const struct fb_var_screeninfo * var,
                                   const struct fb_fix_screeninfo * fix) {
    fprintf(stderr,
            "screenshot: unsupported framebuffer format %ux%u bpp=%u visual=%u "
            "R=%u:%u:%u G=%u:%u:%u B=%u:%u:%u A=%u:%u line=%u yoffset=%u\n",
            var->xres, var->yres, var->bits_per_pixel, fix->visual,
            var->red.offset, var->red.length, var->red.msb_right,
            var->green.offset, var->green.length, var->green.msb_right,
            var->blue.offset, var->blue.length, var->blue.msb_right,
            var->transp.offset, var->transp.length, fix->line_length, var->yoffset);
}

/* One capture buffer for the process lifetime: a fresh ~750 KB contiguous
 * malloc per shot is exactly the request that can fail on this device under
 * decoder/download pressure. Only one capture is ever in flight (busy flag),
 * and the worker is done with it before the next capture can start. */
static uint16_t * capture_buffer;
static size_t capture_buffer_size;

/* Copies the visible page out of the mapped framebuffer. Not read()/pread():
 * the Ingenic fbdev driver returns 0 bytes for the first read after the panel
 * has been idle, which failed the capture on its first row. The mapping is
 * the same memory LVGL renders into, so it always reflects what is shown. */
static bool capture_visible_framebuffer(uint16_t ** out_pixels, unsigned * out_width,
                                        unsigned * out_height) {
    *out_pixels = NULL;
    *out_width = 0;
    *out_height = 0;

    int fd = open("/dev/fb0", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        perror("screenshot: open /dev/fb0");
        return false;
    }

    struct fb_var_screeninfo var;
    struct fb_fix_screeninfo fix;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &var) != 0 ||
        ioctl(fd, FBIOGET_FSCREENINFO, &fix) != 0) {
        perror("screenshot: read framebuffer info");
        close(fd);
        return false;
    }

    if (!framebuffer_is_rgb565(&var, &fix)) {
        log_framebuffer_format(&var, &fix);
        close(fd);
        return false;
    }

    uint64_t row_bytes_64 = (uint64_t) var.xres * sizeof(uint16_t);
    uint64_t right_edge = ((uint64_t) var.xoffset + var.xres) * sizeof(uint16_t);
    uint64_t bottom_row = (uint64_t) var.yoffset + var.yres;
    uint64_t last_byte = bottom_row * fix.line_length;
    if (var.xres == 0 || var.yres == 0 || var.xres > SCREENSHOT_MAX_DIMENSION ||
        var.yres > SCREENSHOT_MAX_DIMENSION || row_bytes_64 > SIZE_MAX / var.yres ||
        row_bytes_64 > SIZE_MAX || right_edge > fix.line_length ||
        bottom_row > var.yres_virtual || last_byte > fix.smem_len || fix.smem_len == 0) {
        fprintf(stderr, "screenshot: invalid framebuffer dimensions or visible area\n");
        close(fd);
        return false;
    }

    size_t row_bytes = (size_t) row_bytes_64;
    size_t want = (size_t) var.xres * var.yres * sizeof(uint16_t);
    if (!capture_buffer || capture_buffer_size != want) {
        free(capture_buffer);
        capture_buffer = (uint16_t *) malloc(want);
        capture_buffer_size = capture_buffer ? want : 0;
    }
    if (!capture_buffer) {
        fprintf(stderr, "screenshot: framebuffer copy allocation failed (%zu bytes)\n", want);
        close(fd);
        return false;
    }

    void * map = mmap(NULL, fix.smem_len, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (map == MAP_FAILED) {
        perror("screenshot: mmap /dev/fb0");
        return false;
    }
    const uint8_t * base = (const uint8_t *) map;
    size_t x_offset = (size_t) var.xoffset * sizeof(uint16_t);
    for (unsigned y = 0; y < var.yres; y++) {
        size_t source_offset = ((size_t) var.yoffset + y) * fix.line_length + x_offset;
        memcpy(capture_buffer + (size_t) y * var.xres, base + source_offset, row_bytes);
    }
    munmap(map, fix.smem_len);

    *out_pixels = capture_buffer;
    *out_width = var.xres;
    *out_height = var.yres;
    return true;
}

static bool write_all(int fd, const unsigned char * data, size_t size) {
    size_t done = 0;
    while (done < size) {
        ssize_t count = write(fd, data + done, size - done);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        done += (size_t) count;
    }
    return true;
}

static bool save_png_atomically(const unsigned char * png, size_t png_size,
                                char * out_path, size_t out_path_size,
                                char * out_reason, size_t out_reason_size) {
    struct stat st;
    if (mkdir(SCREENSHOT_DIR, 0755) != 0 && errno != EEXIST) {
        snprintf(out_reason, out_reason_size, "directory_failed");
        perror("screenshot: create Screenshots directory");
        return false;
    }
    if (stat(SCREENSHOT_DIR, &st) != 0 || !S_ISDIR(st.st_mode)) {
        snprintf(out_reason, out_reason_size, "directory_failed");
        fprintf(stderr, "screenshot: Screenshots path is not a directory\n");
        return false;
    }

    time_t now = time(NULL);
    struct tm local;
    if (now == (time_t) -1 || !localtime_r(&now, &local)) {
        snprintf(out_reason, out_reason_size, "timestamp_failed");
        return false;
    }

    char timestamp[32];
    int timestamp_len = snprintf(timestamp, sizeof(timestamp), "%04d%02d%02d_%02d%02d%02d",
                                 local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                                 local.tm_hour, local.tm_min, local.tm_sec);
    if (timestamp_len < 0 || (size_t) timestamp_len >= sizeof(timestamp)) {
        snprintf(out_reason, out_reason_size, "timestamp_failed");
        return false;
    }

    struct timespec precise_now = {0};
    clock_gettime(CLOCK_REALTIME, &precise_now);
    char temporary[SCREENSHOT_PATH_MAX];
    int temp_len = snprintf(temporary, sizeof(temporary), "%s/.Compas_%s_%ld_%09ld.tmp",
                            SCREENSHOT_DIR, timestamp, (long) getpid(), precise_now.tv_nsec);
    if (temp_len < 0 || (size_t) temp_len >= sizeof(temporary)) {
        snprintf(out_reason, out_reason_size, "path_too_long");
        return false;
    }

    int fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0) {
        snprintf(out_reason, out_reason_size, "write_failed");
        perror("screenshot: create temporary file");
        return false;
    }

    bool written = write_all(fd, png, png_size);
    if (!written || fsync(fd) != 0) {
        snprintf(out_reason, out_reason_size, "write_failed");
        perror("screenshot: write or sync temporary file");
        close(fd);
        unlink(temporary);
        return false;
    }
    if (close(fd) != 0) {
        snprintf(out_reason, out_reason_size, "write_failed");
        perror("screenshot: close temporary file");
        unlink(temporary);
        return false;
    }

    for (unsigned suffix = 1; suffix < 1000000; suffix++) {
        int path_len;
        if (suffix == 1) {
            path_len = snprintf(out_path, out_path_size, "%s/Compas_%s.png", SCREENSHOT_DIR, timestamp);
        } else {
            path_len = snprintf(out_path, out_path_size, "%s/Compas_%s_%u.png",
                                SCREENSHOT_DIR, timestamp, suffix);
        }
        if (path_len < 0 || (size_t) path_len >= out_path_size) {
            snprintf(out_reason, out_reason_size, "path_too_long");
            unlink(temporary);
            return false;
        }
        if (lstat(out_path, &st) == 0) continue;
        if (errno != ENOENT) {
            snprintf(out_reason, out_reason_size, "write_failed");
            perror("screenshot: inspect destination");
            unlink(temporary);
            return false;
        }
        if (rename(temporary, out_path) == 0) {
            int dir_fd = open(SCREENSHOT_DIR, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            if (dir_fd >= 0) {
                if (fsync(dir_fd) != 0) perror("screenshot: sync Screenshots directory");
                close(dir_fd);
            }
            return true;
        }
        if (errno == EEXIST) continue;
        snprintf(out_reason, out_reason_size, "write_failed");
        perror("screenshot: publish PNG");
        unlink(temporary);
        return false;
    }

    snprintf(out_reason, out_reason_size, "name_exhausted");
    unlink(temporary);
    return false;
}

static void * screenshot_worker(void * arg) {
    screenshot_job_t * job = (screenshot_job_t *) arg;
    size_t pixel_count = (size_t) job->width * job->height;
    size_t rgb_size = pixel_count * 3;
    unsigned char * rgb = (unsigned char *) malloc(rgb_size);
    char saved_path[SCREENSHOT_PATH_MAX] = {0};
    char failure_reason[SCREENSHOT_REASON_MAX] = "encode_failed";

    if (!rgb) {
        free(job);
        screenshot_set_completion(false, "", "out_of_memory");
        return NULL;
    }

    for (size_t i = 0; i < pixel_count; i++) {
        uint16_t pixel = job->pixels[i];
        unsigned red = (pixel >> 11) & 0x1f;
        unsigned green = (pixel >> 5) & 0x3f;
        unsigned blue = pixel & 0x1f;
        rgb[i * 3] = (unsigned char) ((red << 3) | (red >> 2));
        rgb[i * 3 + 1] = (unsigned char) ((green << 2) | (green >> 4));
        rgb[i * 3 + 2] = (unsigned char) ((blue << 3) | (blue >> 2));
    }
    /* job->pixels is the persistent capture buffer; not freed here. */

    unsigned char * png = NULL;
    size_t png_size = 0;
    unsigned encode_error = lodepng_encode24(&png, &png_size, rgb, job->width, job->height);
    free(rgb);
    if (encode_error != 0) {
        fprintf(stderr, "screenshot: PNG encoding failed: %u\n", encode_error);
        lv_free(png);
        free(job);
        screenshot_set_completion(false, "", "encode_failed");
        return NULL;
    }

    bool saved = save_png_atomically(png, png_size, saved_path, sizeof(saved_path),
                                     failure_reason, sizeof(failure_reason));
    lv_free(png);
    free(job);
    screenshot_set_completion(saved, saved ? saved_path : "", saved ? "" : failure_reason);
    return NULL;
}

/* Confirmation flash, the way phones do it: straight to full white, then a
 * short ease-out fade. It lives on the system layer, above popups, toasts and
 * the pull-down drawer, and is not clickable so a touch during the fade still
 * reaches the page underneath. Shown only after the framebuffer copy, so it
 * never ends up in the image. */
#define SCREENSHOT_FLASH_FADE_MS 240

static void screenshot_flash_opa_cb(void * var, int32_t value) {
    lv_obj_set_style_bg_opa((lv_obj_t *) var, (lv_opa_t) value, 0);
}

static void screenshot_flash_done_cb(lv_anim_t * anim) {
    lv_obj_t * flash = (lv_obj_t *) anim->var;
    if (flash && lv_obj_is_valid(flash)) lv_obj_delete(flash);
}

static void screenshot_flash(void) {
    lv_obj_t * flash = lv_obj_create(lv_layer_sys());
    if (!flash) return;
    lv_obj_remove_style_all(flash);
    lv_obj_set_size(flash, lv_pct(100), lv_pct(100));
    lv_obj_set_pos(flash, 0, 0);
    lv_obj_set_style_bg_color(flash, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(flash, LV_OPA_COVER, 0);
    lv_obj_remove_flag(flash, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(flash, LV_OBJ_FLAG_CLICKABLE);

    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, flash);
    lv_anim_set_exec_cb(&anim, screenshot_flash_opa_cb);
    lv_anim_set_values(&anim, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_duration(&anim, SCREENSHOT_FLASH_FADE_MS);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&anim, screenshot_flash_done_cb);
    lv_anim_start(&anim);
}

const char * screenshot_result_reason(screenshot_result_t result) {
    switch (result) {
        case SCREENSHOT_RESULT_STARTED: return "started";
        case SCREENSHOT_RESULT_SCREEN_OFF: return "screen_off";
        case SCREENSHOT_RESULT_NO_CARD: return "no_card";
        case SCREENSHOT_RESULT_USB_STORAGE: return "usb_storage";
        case SCREENSHOT_RESULT_BUSY: return "busy";
        case SCREENSHOT_RESULT_FRAMEBUFFER: return "framebuffer_unavailable";
        case SCREENSHOT_RESULT_WORKER_START: return "worker_start_failed";
        case SCREENSHOT_RESULT_UNAVAILABLE: return "unavailable";
    }
    return "unavailable";
}

screenshot_result_t screenshot_start(void) {
    if (!backlight_screen_is_on()) return SCREENSHOT_RESULT_SCREEN_OFF;

    pthread_mutex_lock(&screenshot_mutex);
    if (screenshot_busy) {
        pthread_mutex_unlock(&screenshot_mutex);
        return SCREENSHOT_RESULT_BUSY;
    }
    pthread_mutex_unlock(&screenshot_mutex);

    if (!sd_card_root_is_mounted()) {
        show_error_toast("Screenshot needs an SD card");
        return SCREENSHOT_RESULT_NO_CARD;
    }
    if (gui_network_usb_storage_session_active()) {
        show_error_toast("Disconnect USB storage first");
        return SCREENSHOT_RESULT_USB_STORAGE;
    }

    pthread_mutex_lock(&screenshot_mutex);
    if (screenshot_busy) {
        pthread_mutex_unlock(&screenshot_mutex);
        return SCREENSHOT_RESULT_BUSY;
    }
    screenshot_busy = true;
    screenshot_done = false;
    memset(&screenshot_completion, 0, sizeof(screenshot_completion));
    pthread_mutex_unlock(&screenshot_mutex);

    uint16_t * pixels = NULL;
    unsigned width = 0, height = 0;
    if (!capture_visible_framebuffer(&pixels, &width, &height)) {
        screenshot_set_idle();
        show_error_toast("Screenshot failed (framebuffer)");
        return SCREENSHOT_RESULT_FRAMEBUFFER;
    }

    screenshot_job_t * job = (screenshot_job_t *) malloc(sizeof(*job));
    if (!job) {
        screenshot_set_idle();
        show_error_toast("Screenshot failed (worker)");
        return SCREENSHOT_RESULT_WORKER_START;
    }
    job->pixels = pixels;
    job->width = width;
    job->height = height;

    pthread_t worker;
    int rc = pthread_create(&worker, NULL, screenshot_worker, job);
    if (rc != 0) {
        fprintf(stderr, "screenshot: worker thread start failed: %s\n", strerror(rc));
        free(job);
        screenshot_set_idle();
        show_error_toast("Screenshot failed (worker)");
        return SCREENSHOT_RESULT_WORKER_START;
    }
    pthread_detach(worker);
    screenshot_flash();
    return SCREENSHOT_RESULT_STARTED;
}

bool screenshot_poll_completion(screenshot_completion_t * out) {
    pthread_mutex_lock(&screenshot_mutex);
    bool ready = screenshot_done;
    if (ready) {
        if (out) *out = screenshot_completion;
        screenshot_done = false;
        screenshot_busy = false;
        memset(&screenshot_completion, 0, sizeof(screenshot_completion));
    }
    pthread_mutex_unlock(&screenshot_mutex);
    return ready;
}

bool screenshot_is_busy(void) {
    pthread_mutex_lock(&screenshot_mutex);
    bool busy = screenshot_busy;
    pthread_mutex_unlock(&screenshot_mutex);
    return busy;
}

#endif /* HOST_BUILD */
