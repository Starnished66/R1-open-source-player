#ifndef LV_CONF_H
#define LV_CONF_H

/* Set to 1 to enable configuration content */
#define LV_CONF_SKIP 0

/* Color depth: 16-bit (RGB565) is standard for many MIPS LCD screens */
#define LV_COLOR_DEPTH 16

/* LVGL's own built-in default is 33ms (~30fps) -- a software-imposed cap
 * unrelated to any real panel/hardware limit (the fbdev double-buffer pan
 * mechanism this device uses has no inherent 30fps ceiling). Real-hardware
 * testing showed navigation animations reading choppy at the default, and
 * community testing on this same device reported successfully driving it
 * up to 40-60fps, so raise the ceiling here -- actual throughput is still
 * bounded by how fast the software renderer can push pixels on this CPU
 * (no GPU), this just stops LVGL from artificially waiting past that. */
#define LV_DEF_REFR_PERIOD 16

/* Memory management: Use standard C library functions */
#define LV_USE_STDLIB_MALLOC  LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING  LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB

/* Enable built-in fonts we want to use in our UI */
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_20 1 /* status bar text, 16*1.25 -- see build_status_bar() */
#define LV_FONT_MONTSERRAT_22 1 /* touch_list rows (Artists/Albums/Songs/...) -- see LIST_ROW_FONT */
#define LV_FONT_MONTSERRAT_28 1

/* Medium/BlindMF steps for Settings -> Font Size (see gui.c's
 * apply_font_size_tier()) -- each font_montserrat_NN.c file's actual glyph
 * data is compiled out entirely unless its own LV_FONT_MONTSERRAT_NN is
 * defined (the .o still gets built either way, an empty translation unit,
 * since the Makefile's own file list isn't conditional -- only the content
 * inside each file is), so these have to be enabled here or referencing
 * them would be a linker error, not just unused. */
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_26 1
#define LV_FONT_MONTSERRAT_30 1
#define LV_FONT_MONTSERRAT_32 1
#define LV_FONT_MONTSERRAT_34 1
#define LV_FONT_MONTSERRAT_40 1

/* LVGL's own unconfigured default is lv_font_montserrat_14 (see
 * lv_conf_internal.h) -- every label in this codebase that never sets an
 * explicit text_font style (roughly half of them: dialogs, toasts, misc
 * settings/about text, ...) silently inherited that undersized 14px font
 * (real-device feedback: "system wide font is too small"). Point it at 16,
 * the smallest size this app deliberately chooses elsewhere, so unstyled
 * text is at least as big as our own smallest intentional choice rather
 * than smaller than all of them.
 *
 * All 4 enabled sizes above were also regenerated (see
 * lvgl/scripts/built_in_font/built_in_font_gen.py and the vendored
 * Montserrat-Medium.ttf/FontAwesome woff it uses) with the Latin-1
 * Supplement range (0xA0-0xFF) added on top of LVGL's stock 0x20-0x7F
 * ASCII-only subset -- the stock bitmap fonts simply have no glyph data
 * for accented Latin characters at all (confirmed by reading each font's
 * own generation-command comment at the top of its .c file), which is why
 * Spanish/French/German/etc. text with accents was rendering blank glyphs
 * everywhere, not just in this default. */
#define LV_FONT_DEFAULT &lv_font_montserrat_16

/* Real Japanese/CJK text rendering was attempted here via a cross-compiled
 * FreeType rendering /usr/resource/fonts/default.otf as a fallback behind
 * Montserrat (LVGL's own bundled lv_font_simsun_16_cjk is only an
 * ~1000-glyph demo subset, not real coverage). Reverted: two live-device
 * boot attempts with it wired in both hung at the boot logo, most likely
 * because gui_init()'s auto-resume path can call into the CJK-fallback
 * lookup synchronously before the first frame renders, and there's no way
 * to get crash/hang diagnostics off this device to root-cause it further
 * right now. Left disabled -- see src/fallback_font.c for the retry: same
 * underlying font, but via LVGL's own lighter tiny_ttf renderer (below)
 * instead of FreeType, and genuinely deferred until after the first frame
 * renders this time (see fallback_font_load_deferred()'s own comment),
 * rather than reachable from any synchronous boot-path call. */
#define LV_USE_FREETYPE 0

/* See src/fallback_font.c -- default.otf (the stock firmware's own pan-
 * script font, confirmed to cover Cyrillic and Japanese kana/kanji) uses
 * CFF outlines, which this lighter TTF-only renderer can't read directly;
 * fallback_font.c's own comment covers the offline conversion this actually
 * loads instead. FILE_SUPPORT is needed since the font is loaded from disk
 * at runtime (via LVGL's own posix FS driver, same "S:" drive-letter
 * convention assets.c already uses), not baked into the binary as a C
 * array -- a 9.6MB array is not something this build wants to carry
 * unconditionally for every user, most of whom never see non-Latin titles. */
#define LV_USE_TINY_TTF 1
#if LV_USE_TINY_TTF
  #define LV_TINY_TTF_FILE_SUPPORT 1
#endif

/* Real UI assets (theme2 PNGs from the stock firmware) are loaded from
 * disk at runtime rather than pre-baked into C arrays, so we need a PNG
 * decoder and a filesystem driver LVGL can address by drive letter. */
#define LV_USE_LODEPNG 1
#define LV_USE_FS_POSIX 1
#define LV_FS_POSIX_LETTER 'S'
#define LV_FS_POSIX_PATH ""
#define LV_FS_POSIX_CACHE_SIZE 0

/* Import via Wi-Fi screen (gui.c): renders the device's own IP as a QR
 * code so a phone can jump straight to the upload page without typing an
 * address. */
#define LV_USE_QRCODE 1

/* LVGL's general-purpose image cache (decoded bitmaps, keyed by source
 * path) is compiled out entirely below this being 0 (confirmed by reading
 * lv_image_decoder.c/lv_image_cache.c: the whole cache lookup/store path is
 * `#if LV_CACHE_DEF_SIZE > 0`) -- meaning every single image draw call
 * re-runs the full PNG decode (open file, zlib inflate, pixel-format
 * convert) from scratch, even for an unchanged image drawn every frame.
 * Real-device profiling of the quick-drawer drag (dragging pull_down/bg.png,
 * 480x800 RGBA -> ~1.1MB decoded at this build's RGB565+alpha) showed
 * single-frame render times of 65-360ms; re-decoding that same ~1.1MB image
 * from a freshly-inflated PNG on every ~16ms drag tick, on a CPU with no
 * hardware zlib/PNG acceleration, is consistent with exactly that cost.
 * This device only has ~55MB RAM total (~19MB "available" per `free -m`),
 * so keep this modest -- just enough to hold what's on screen at once (the
 * drawer's background plus its handful of small icon PNGs), not a
 * everything-ever-shown cache. */
#define LV_CACHE_DEF_SIZE (4 * 1024 * 1024)

/* Embedded album art (FLAC METADATA_BLOCK_PICTURE / MP3 ID3v2 APIC) is most
 * often JPEG, decoded straight from the in-memory bytes metadata.c already
 * extracted -- LVGL's tjpgd decoder can only read "from data" via a
 * memory-backed filesystem, hence LV_USE_FS_MEMFS alongside it (confirmed
 * by reading lv_tjpgd.c's decoder_open: it wraps the buffer with
 * lv_fs_make_path_from_buffer() and opens that as if it were a real file). */
#define LV_USE_TJPGD 1
#define LV_USE_FS_MEMFS 1
#define LV_FS_MEMFS_LETTER 'M'

/* Screen-transition slides animate lv_screen_load_anim()'s real screen
 * objects by default, which on a GPU-less target means fully re-rendering
 * both the outgoing and incoming screen's whole widget tree on every single
 * animation frame -- the actual cause of choppy/tearing-prone navigation
 * found during real-hardware testing, not just a refresh-rate issue.
 * Snapshotting each screen ONCE into a static bitmap and sliding those two
 * bitmaps instead (see gui.c's screen_transition_slide()) turns "N frames x
 * 2 full re-renders" into "2 renders + N cheap bitmap blits". */
#define LV_USE_SNAPSHOT 1

/* Platform-specific driver settings */
#ifdef HOST_BUILD
  /* Host Simulation Settings (Arch Linux PC) */
  #define LV_USE_SDL 1
  #define LV_SDL_INCLUDE_PATH <SDL2/SDL.h>
  #define LV_USE_LINUX_FBDEV 0
  #define LV_USE_EVDEV 0
#else
  /* Target Build Settings (HiBy OS hardware) */
  #define LV_USE_SDL 0
  #define LV_USE_LINUX_FBDEV 1
  #define LV_USE_EVDEV 1

  /* This device's /dev/fb0 is double-buffered via panning (virtual height =
   * 2x the panel's real height; the visible half is selected by yoffset),
   * but LVGL's stock fbdev driver has no concept of that -- it always
   * lv_memcpy()s straight into whatever half happens to be on screen right
   * now, tearing constantly during animation. Patched lv_linux_fbdev.c
   * (search "pan_double_buffer") to instead render each frame into the
   * *other* half and flip via FBIOPAN_DISPLAY once it's complete, which
   * needs these two settings: DIRECT render mode (so LVGL always renders
   * into one persistent, caller-supplied buffer rather than an internal
   * scratch buffer it copies from) and BUFFER_COUNT=2 (one buffer per
   * physical half). See the driver for the fallback when the display
   * doesn't actually support this.
   *
   * Real-device incident: a large (480x480) directly-set raw image (album
   * cover art, see set_cover_art() in gui.c) renders as severe, reproducible
   * visual corruption -- confirmed NOT a data bug (a raw dump of the
   * decoded buffer straight from memory is correct every time, on this
   * exact target hardware) via FOUR separate ruled-out theories: LVGL's
   * image cache serving stale tiles, a missing lv_image_header_t.magic
   * value, a dirty-region-computation bug specific to a single large widget
   * update, and (tested by temporarily switching this define to
   * LV_DISPLAY_RENDER_MODE_PARTIAL, bypassing the pan_double_buffer patch
   * entirely in favor of LVGL's stock scratch-buffer-plus-memcpy flush)
   * the custom DIRECT-mode fbdev patch itself -- corruption was
   * byte-for-byte identical under PARTIAL mode too, so it is NOT specific
   * to this driver patch. Reverted back to DIRECT since PARTIAL bought
   * nothing but reintroduced tearing risk. The bug lives somewhere between
   * a provably-correct source buffer and the screen -- most likely in
   * LVGL's own software image-decode/blit path (lv_bin_decoder.c's
   * variable-image "use directly" path and/or lv_draw_sw_img.c) -- not yet
   * root-caused. See TESTING.md / project history for the full investigation
   * before adding a fifth theory to this list. */
  #define LV_LINUX_FBDEV_RENDER_MODE LV_DISPLAY_RENDER_MODE_DIRECT
  #define LV_LINUX_FBDEV_BUFFER_COUNT 2
#endif

#endif /* LV_CONF_H */
