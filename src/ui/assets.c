#include "assets.h"

#include "debug_log.h"
#include "lvgl.h"
#include "subprocess.h"

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
  /* Writable override, checked first by asset_path() below, for an asset
   * THIS app adds that has no stock equivalent (e.g. stream_media/
   * subsonic.png -- Subsonic isn't a stock HiBy feature, so there's no
   * "real" copy under THEME_ROOT to point at the way every other icon in
   * this file does). THEME_ROOT itself is the stock firmware's own
   * read-only squashfs resource pack -- this app can reference files
   * already there, but can never add new ones to it directly (same
   * read-only-rootfs constraint the Makefile's own libldacdec.so/mbedTLS
   * comments document for shared libraries). /usr/data is this device's
   * one persistent, writable partition, same convention settings.c and
   * metadata_db.c already use it for. A file only needs to exist here
   * once a proper repack bakes it into THEME_ROOT for real -- at that
   * point this override is simply never found and silently falls through,
   * same as the LD_LIBRARY_PATH fallback main.c sets up for exactly this
   * "workaround until the next real reflash" shape of problem. */
  #include <unistd.h>
  #define THEME_OVERRIDE_ROOT "/usr/data/theme_overrides/"
#endif

void assets_init(void) {
    lv_lodepng_init();
    lv_fs_posix_init();
    lv_tjpgd_init();
    lv_fs_memfs_init();
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
        return strdup(buf);
    }
#endif
    snprintf(buf, sizeof(buf), "S:" THEME_ROOT "%s", relative_path);
    /* Heap-allocated fresh on every call, deliberately never freed here --
     * some callers (lv_image_set_src) copy the string themselves, but
     * others (lv_obj_set_style_bg_image_src) just store the raw pointer, so
     * a single reused static buffer would go stale under them (confirmed:
     * every touch_list row ended up showing whatever the *last* asset_path()
     * call anywhere in the app had written, once rendering caught up with
     * construction). Screens are built once and live for the process's
     * lifetime, same as the rest of this UI's per-tile/per-row context
     * structs, so the bounded, one-time leak per image reference is the
     * same accepted tradeoff already made elsewhere in this codebase. */
    return strdup(buf);
}

const char * asset_path_plain(const char * relative_path) {
    char buf[320];
#ifndef HOST_BUILD
    snprintf(buf, sizeof(buf), THEME_OVERRIDE_ROOT "%s", relative_path);
    if (access(buf, R_OK) == 0) {
        return strdup(buf);
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
    return strdup(buf); /* same heap-allocated-and-never-freed tradeoff as asset_path() above */
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

void assets_reset_theme_overrides(void) {
#ifndef HOST_BUILD
    DBG_LOG("assets_reset_theme_overrides: rm -rf %s\n", THEME_OVERRIDE_ROOT);
    char * rm_argv[] = { (char *) "/bin/rm", (char *) "-rf", (char *) THEME_OVERRIDE_ROOT, NULL };
    subprocess_run(rm_argv, NULL, 0);
#endif
}
