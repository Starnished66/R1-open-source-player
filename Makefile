# Makefile for open_hiby_player

# Board selector -- `make target BOARD=r3proii` (default r1 if unset). Gates
# BOARD_DEFINE (-DBOARD_R1 / -DBOARD_R3PROII, consumed by src/core/
# board_config.h) and suffixes every output binary/object directory below so
# switching boards can never silently relink stale objects built with the
# OTHER board's defines -- Make has no flag-change tracking of its own, only
# file-mtime tracking, so two boards sharing one build_target/ would be a
# real, silent-corruption hazard without this.
BOARD ?= r1
ifeq ($(filter $(BOARD),r1 r3proii),)
$(error Unknown BOARD '$(BOARD)' -- expected r1 or r3proii)
endif
BOARD_DEFINE = -DBOARD_$(shell echo $(BOARD) | tr a-z A-Z)

# Object directories -- same r1-stays-bare reasoning as HOST_BIN/TARGET_BIN
# below. Every build_target/ or build_host/ path elsewhere in this file is
# written through these two variables, never the literal string, so a board
# switch can't accidentally reuse the other board's stale .o files.
ifeq ($(BOARD),r1)
BUILD_TARGET_DIR = build_target
BUILD_HOST_DIR = build_host
else
BUILD_TARGET_DIR = build_target_$(BOARD)
BUILD_HOST_DIR = build_host_$(BOARD)
endif

# Target executables -- r1 keeps its exact original unsuffixed names (every
# downstream consumer -- the Test2 repack workflow, CI, TESTING.md -- expects
# these exact filenames), other boards get a distinct suffix so a r3proii
# build can never be mistaken for or silently overwrite an r1 one.
ifeq ($(BOARD),r1)
HOST_BIN = open_hiby_player_host
TARGET_BIN = open_hiby_player_target
else
HOST_BIN = open_hiby_player_host_$(BOARD)
TARGET_BIN = open_hiby_player_target_$(BOARD)
endif

# Compiler and Linker configuration
CC = gcc
CXX = g++
CROSS_CC = mipsel-linux-musl-gcc
CROSS_CXX = mipsel-linux-musl-g++
CROSS_STRIP = mipsel-linux-musl-strip

# Dependency directory names
LVGL_DIR = lvgl
DR_LIBS_DIR = dr_libs
TINYALSA_DIR = tinyalsa
FAAD2_DIR = faad2
ALAC_DIR = alac
OPUS_DIR = opus
MBEDTLS_DIR = mbedtls
CJSON_DIR = cJSON
DBUS_DIR = dbus
LIBEXECINFO_DIR = libexecinfo
LUA_DIR = lua
# stb_vorbis (public domain, github.com/nothings/stb) -- a single 192KB file
# rather than a whole cloned tree like every other dependency above, so it's
# just committed directly here instead of getting its own bootstrap-clone
# block (which would need to sparse-clone one file out of the much larger,
# otherwise-unrelated stb monorepo for no real benefit over committing it).
STB_VORBIS_DIR = stb_vorbis

# Self-bootstrap: clone dependencies if they don't exist yet before evaluating variables
ifeq ($(wildcard $(LVGL_DIR)),)
$(info Cloning LVGL v9.1.0...)
$(shell git clone --depth 1 -b v9.1.0 https://github.com/lvgl/lvgl.git)
endif

# This project's own LVGL checkout (gitignored -- real upstream source,
# not ours to redistribute) carries three categories of local
# customization that upstream v9.1.0 does not have, all needed for a
# clean GitHub clone to both LINK and BEHAVE like this developer's tree
# (see ISSUES.md's "clean GitHub clones cannot link the transition
# compositor" entry -- fixing only the first category still leaves a
# clean-cloned build behaviorally different, which is exactly the mistake
# that entry warns against):
#
#   1. fbdev compositor (patches/lvgl_fbdev_compositor.patch): six
#      functions (lv_linux_fbdev_get_active_page/get_inactive_page/
#      get_stride/begin_external_composition/present_external_page/
#      end_external_composition) that transition_compositor.c and
#      gui_navigation.c call to drive pan-flip transition animations
#      directly against the two physical framebuffer pages. Missing
#      entirely from upstream -- a clean clone fails to link with
#      "undefined reference to `lv_linux_fbdev_get_active_page'" (and the
#      other five) the moment either caller is linked in.
#   2. runtime fixes (patches/lvgl_runtime_fixes.patch): five small
#      genuine upstream bugs/limitations this app hit in practice, not
#      feature work -- lv_tiny_ttf_init()/_deinit() missing a null-guard (a
#      second init call, or deinit after a failed init, double-destroys/
#      leaks the shared font cache) plus two error paths in
#      lv_tiny_ttf_create() that leaked an open font-file handle;
#      _lv_cache_lru_rb.c's drop_all_cb() destroyed the cache's red-black
#      tree without reinitializing it, so any cache use after a full clear
#      (e.g. a settings change that invalidates cached UI bitmaps) operated
#      on a destroyed tree; tjpgdcnf.h's JD_USE_SCALE was left at the
#      upstream default of 0, disabling TJpgDec's own output downscaling
#      that this app's album-art path uses to decode cover JPEGs at the
#      largest 2^n that still covers the target, then cover-fit, instead of
#      full-size-then-software-resize (see ISSUES.md's Albums-screen
#      overheat/OOM entry); tjpgd.c's jd_mcu_output was also missing ChaN's
#      1/2 and 1/4 MCU averaging and 1/8 DC RGB path, so scale!=0 was unusable
#      until restored here (BGR order, matching this copy's full-MCU loop);
#      lv_tiny_ttf_init() also hardcoded its shared rasterized-glyph LRU
#      cache to 128 entries -- fine for this app's default Montserrat
#      tiers, which are pre-baked bitmap fonts (lv_font_montserrat_*, see
#      the generated-fonts category below) that never touch this cache at
#      all, but the moment Settings > Display > Font applies a custom SD
#      card .ttf, every one of app_font_16/20/22/28/lyrics (fallback_font.c)
#      becomes a runtime-rasterized tiny_ttf instance sharing this one
#      global cache -- a scrolling list cycling through more distinct
#      glyphs than that across those sizes evicts and re-rasterizes
#      (STB truetype software rasterization) on nearly every frame, which
#      is exactly the real-device report ("selecting an SD card font lags
#      the device, scroll lists become slow"). Bumped to 512: each cached
#      glyph is a small LV_COLOR_FORMAT_A8 bitmap sized to its own glyph
#      box (tiny_ttf_cache_create_cb()), so worst case (512 of the largest
#      BlindMF-tier glyphs) is still under ~1MB against this device's
#      ~19MB available RAM, comfortably covering several font sizes' full
#      alphanumeric+punctuation working sets at once without the earlier
#      128-entry thrashing. lodepng.c's decodeGeneric() sized the buffer it
#      unfilters a PNG's native-depth scanlines into as a hardcoded 4 bytes/
#      pixel (an ARGB8888-shaped stride), but postProcessScanlines() writes
#      that buffer at the PNG's own native bpp -- any source with more than
#      32 bits per pixel (16-bit-per-channel RGBA is 64bpp) overflowed that
#      buffer by exactly the difference, corrupting the heap. Real-device
#      report: decoding a stock 16-bit RGBA icon (copied in from an R3 Pro
#      II firmware dump for the in-progress port) silently corrupted the
#      heap, surfacing later as a segfault inside an unrelated free() call
#      and a reboot-on-crash loop. Fixed by sizing that buffer's stride to
#      the larger of the native row width and the ARGB8888 row width.
#   3. generated fonts (patches/lvgl_generated_fonts/, copied in whole
#      rather than diffed): ten Montserrat .c files regenerated with an
#      expanded lv_font_conv codepoint range (Latin-1 Supplement +
#      Latin Extended-A + typographic punctuation added to upstream's
#      bare ASCII range -- each font file's own header comment records
#      the exact lv_font_conv invocation) so real-world artist/album
#      metadata with accented characters (a Spanish name, Latin Extended-A
#      characters like "a" with a macron, ...) or curly quotes/en-dashes
#      renders correctly instead of showing tofu boxes or nothing at all.
#      Originally only the four "Small" Font Size tier sizes (16/20/22/28,
#      see fallback_font.c's get_montserrat_font_for_px()) were regenerated
#      -- a real-device bug report ("Spanish accents/Latin Extended-A
#      characters not rendered by the default font") traced this to the
#      Medium/BlindMF tiers' six sizes (24/26/30/32/34/40, lv_conf.h's own
#      comment on LV_FONT_MONTSERRAT_24 etc.) still being pristine upstream
#      LVGL fonts with the stock bare-ASCII-plus-degree-sign range (`-r
#      0x20-0x7F,0xB0,0x2022`, confirmed by reading their own pre-patch
#      header comments) -- regenerated the same way to close the gap
#      regardless of which Font Size tier is active. These are
#      machine-generated hex-array files with no stable diff context
#      (every glyph/kerning table shifts on any range change), so unlike
#      the two patches above they are tracked and restored as whole-file
#      copies rather than a text diff -- diffing them would be both far
#      larger than the files themselves and fragile to patch fuzz.
#
# LVGL_PINNED_COMMIT is v9.1.0's tag commit (the tag object peels to this
# -- `git ls-remote https://github.com/lvgl/lvgl.git refs/tags/
# v9.1.0^{}`), checked before touching anything so none of the three
# categories above can silently apply to a different LVGL revision and
# produce a corrupted tree.
#
# All three are applied/verified by one real stamp-file TARGET
# (LVGL_PATCH_STAMP below, wired as a NORMAL prerequisite -- not
# order-only -- on every object pattern rule that compiles either LVGL or
# this app's own sources). Two independent reasons neither the stamp nor
# the normal-prerequisite choice can be relaxed: (1) app code needs the
# patched lv_linux_fbdev.h visible at ITS OWN compile time too, not just
# LVGL's -- without real prototypes, an implicit declaration of a
# pointer-returning function is undefined behavior on the mipsel target,
# not just a warning; (2) this project has no per-file #include
# dependency tracking (no -MMD/.d generation anywhere in this Makefile),
# so if a future revision of any of the three categories changes what
# callers see, only a NORMAL prerequisite forces every object --
# including ones some earlier build already compiled and cached -- to be
# reconsidered against the new stamp timestamp; an order-only prerequisite
# only guarantees the stamp exists before a first build, not that
# already-built objects get rebuilt when it changes later, silently
# reintroducing stale-ABI object files.
#
# This is a real stamp-file target rather than another parse-time
# $(shell ...) block like the clones above because a parse-time $(shell
# git clone ...) call's exit status is never checked by Make -- a failed
# clone would silently fall through to a confusing "missing symbols for
# half of LVGL" failure much further downstream. A real target's recipe
# can fail loudly and stop the build exactly where the real problem is,
# which matters more here than for the plain clones above because this
# step has far more ways to fail on its own (wrong LVGL revision, a patch
# doesn't apply, a partially-patched tree, a patch updated since last
# applied, a generated font file that matches neither pristine nor the
# tracked golden copy) than a plain clone does.
LVGL_PATCH := patches/lvgl_fbdev_compositor.patch
LVGL_RUNTIME_FIXES_PATCH := patches/lvgl_runtime_fixes.patch
LVGL_GENERATED_FONTS_DIR := patches/lvgl_generated_fonts
LVGL_GENERATED_FONTS := lv_font_montserrat_16.c lv_font_montserrat_20.c lv_font_montserrat_22.c lv_font_montserrat_24.c lv_font_montserrat_26.c lv_font_montserrat_28.c lv_font_montserrat_30.c lv_font_montserrat_32.c lv_font_montserrat_34.c lv_font_montserrat_40.c
LVGL_PINNED_COMMIT := e1c0b21b2723d391b885de4b2ee5cc997eccca91
LVGL_FBDEV_C := $(LVGL_DIR)/src/drivers/display/fb/lv_linux_fbdev.c
LVGL_FBDEV_H := $(LVGL_DIR)/src/drivers/display/fb/lv_linux_fbdev.h
LVGL_TINY_TTF := $(LVGL_DIR)/src/libs/tiny_ttf/lv_tiny_ttf.c
LVGL_TJPGDCNF := $(LVGL_DIR)/src/libs/tjpgd/tjpgdcnf.h
LVGL_LRU_RB := $(LVGL_DIR)/src/misc/cache/_lv_cache_lru_rb.c
LVGL_LODEPNG := $(LVGL_DIR)/src/libs/lodepng/lodepng.c
LVGL_FONT_TARGETS := $(LVGL_GENERATED_FONTS:%=$(LVGL_DIR)/src/font/%)
LVGL_FONT_GOLDEN := $(LVGL_GENERATED_FONTS:%=$(LVGL_GENERATED_FONTS_DIR)/%)
LVGL_PATCH_STAMP := $(LVGL_DIR)/.lvgl_fbdev_patch_applied
# "Already applied?" for the two patches is decided by `patch --dry-run
# --reverse` against the CURRENT patch file (see the stamp recipe below)
# -- an exact byte-for-byte check of real file content against exactly
# what that patch produces, not a heuristic. That one check already means
# a revised patch against an already-patched tree is correctly detected
# as needing re-evaluation (the old content won't reverse-apply cleanly
# against the new patch text) rather than being waved through because a
# function name happened to still be present. `--force --fuzz=0` on every
# invocation is deliberate, not decoration: `patch`'s own default
# heuristics silently defeat this exact-match guarantee otherwise --
# `-t`/`--batch` INCREASES auto-detection of "this looks reversed, flip
# it" per near GNU patch's own manual, which was caught live during
# testing giving a false "already applied" verdict on a tree where only
# one of two files actually had the change; and any nonzero --fuzz lets a
# hunk land at the wrong offset with reduced context confidence instead
# of failing outright. LVGL_FBDEV_SYMBOLS below is only a cheap secondary
# sanity check run once, right after a fresh forward-apply of the fbdev
# patch specifically, checked against BOTH files (a declaration in the
# header with no matching definition in the .c file is exactly how the
# original undefined-reference bug could resurface despite looking
# "patched"). The generated-font files use a different, simpler check
# (see the recipe) since they are whole-file copies, not diffs.
LVGL_FBDEV_SYMBOLS := lv_linux_fbdev_get_active_page lv_linux_fbdev_get_inactive_page \
                      lv_linux_fbdev_get_stride lv_linux_fbdev_begin_external_composition \
                      lv_linux_fbdev_present_external_page lv_linux_fbdev_end_external_composition

# dr_flac (single-header FLAC decoder, used on both host and target)
ifeq ($(wildcard $(DR_LIBS_DIR)),)
$(info Cloning dr_libs (dr_flac)...)
$(shell git clone --depth 1 https://github.com/mackron/dr_libs.git)
endif

# tinyalsa (minimal ALSA userspace library, used on target only for audio output)
ifeq ($(wildcard $(TINYALSA_DIR)),)
$(info Cloning tinyalsa...)
$(shell git clone --depth 1 https://github.com/tinyalsa/tinyalsa.git)
endif

# FAAD2 (AAC decoder, GPLv2 -- see LICENSE and README before assuming this
# is fine for a derived project of yours)
ifeq ($(wildcard $(FAAD2_DIR)),)
$(info Cloning FAAD2...)
$(shell git clone --depth 1 https://github.com/knik0/faad2.git)
endif

# ALAC (Apple Lossless decoder, Apache 2.0 -- Apple's own reference decoder,
# mirrored by mikebrady)
ifeq ($(wildcard $(ALAC_DIR)),)
$(info Cloning ALAC...)
$(shell git clone --depth 1 https://github.com/mikebrady/alac.git)
endif

# libopus (BSD-3-Clause) -- SILK+CELT hybrid decoder for .opus files. Ogg
# container framing (page/segment parsing, OpusHead/OpusTags) is hand-parsed
# in-tree (ogg_demux.c/h) rather than vendoring libogg too, matching this
# project's existing preference for hand-written container demuxers
# (asf_demux.c/h, mp4_demux.c/h, ape_demux.c/h) over a general-purpose demux
# library for a comparatively simple bitstream format.
ifeq ($(wildcard $(OPUS_DIR)),)
$(info Cloning libopus v1.5.2...)
$(shell git clone --depth 1 -b v1.5.2 https://github.com/xiph/opus.git)
endif

# mbedTLS (Apache 2.0 -- TLS for network streaming, subsonic_client.c). The
# real R1 firmware has no usable CA bundle (/etc/ssl/certs is empty) and its
# own libcurl/OpenSSL .so's are glibc-built and unreachable from a static
# musl binary anyway (musl's dlopen() refuses to run at all from a static
# executable -- confirmed against the real device; that's also why AAC
# ended up statically linking FAAD2 instead of dlopen()ing libfdk-aac.so.2),
# so this is vendored and cross-compiled the same way as every other
# dependency here rather than relying on anything already on the device.
ifeq ($(wildcard $(MBEDTLS_DIR)),)
$(info Cloning mbedTLS v3.6.2...)
$(shell git clone --depth 1 -b v3.6.2 https://github.com/Mbed-TLS/mbedtls.git)
endif

# cJSON (MIT, single .c/.h pair) -- both Subsonic (?f=json) and Jellyfin
# APIs speak JSON, so this avoids needing an XML parser at all.
ifeq ($(wildcard $(CJSON_DIR)),)
$(info Cloning cJSON...)
$(shell git clone --depth 1 https://github.com/DaveGamble/cJSON.git)
endif

# libdbus (AFL-2.1/GPL-2+ dual-licensed) -- target only, for the AVRCP
# transport-button service (bt_media_player.c): registers this app as a
# BlueZ org.bluez.MediaPlayer1 so a connected Bluetooth accessory's own
# play/pause/next/previous buttons control playback, which needs a real
# D-Bus service (responding to incoming method calls) rather than the
# one-shot `bluetoothctl`/`dbus-send`/`bluealsa-cli` invocations everything
# else in bluetooth_control.c uses -- no CLI tool can host a service object.
#
# Vendored and cross-compiled from source rather than dynamically linking
# against the device's own /usr/lib/libdbus-1.so.3 (confirmed present):
# that .so is built against the device's real glibc 2.22 (Ingenic vendor
# SDK, 2017), while this whole project targets mipsel-linux-musl -- musl
# and glibc are not ABI-compatible, and (per the mbedTLS comment above)
# dlopen() from a static musl binary doesn't work on this device anyway.
# Assembling a compatible glibc sysroot to link against the real .so
# properly was explored and abandoned as its own substantial, uncertain
# side project; vendoring is the same architecture every other dependency
# here already uses for exactly this class of problem (FAAD2 instead of
# dlopen()ing the device's own libfdk-aac.so.2).
#
# dbus/dbus-arch-deps.h and dbus/config.h are normally generated by
# libdbus's own CMake build from configure-time type/feature detection,
# which this project doesn't run -- see dbus_vendor_config/ (hand-written
# replacements, with their own comments on each value) and the DBUS_SRCS/
# DBUS_CFLAGS below (which exclude Windows-only sources, the dbus-daemon
# itself, service-activation spawning, and the nonce-tcp transport this
# app never uses -- confirmed via a real link+run test on the actual
# device before wiring this into the main build).
ifeq ($(wildcard $(DBUS_DIR)),)
$(info Cloning libdbus 1.16.2...)
$(shell git clone --depth 1 -b dbus-1.16.2 https://gitlab.freedesktop.org/dbus/dbus.git $(DBUS_DIR))
endif

# A modern, BSD-licensed clone of the backtrace facility found in GNU libc.
# This library is primarily intended for porting Linux code to BSD platforms
# and other systems that don't have built-in backtrace support -- needed here
# because the target's musl libc (unlike host glibc) ships no <execinfo.h>/
# backtrace() at all, and main.c's SIGSEGV handler calls backtrace()/
# backtrace_symbols_fd() unconditionally on both builds.
ifeq ($(wildcard $(LIBEXECINFO_DIR)),)
$(info Cloning libexecinfo...)
$(shell git clone --depth 1 https://github.com/fam007e/libexecinfo.git $(LIBEXECINFO_DIR))
endif
# getreturnaddr()/getframeaddr() (used by execinfo.c's backtrace()) aren't
# committed as a .c file upstream -- their own Makefile generates
# stacktraverse.c from gen.py at build time (one __builtin_return_address(N)/
# __builtin_frame_address(N) case per depth, since those GCC builtins only
# accept compile-time-constant N), so this does the same thing here.
ifeq ($(wildcard $(LIBEXECINFO_DIR)/stacktraverse.c),)
$(info Generating libexecinfo/stacktraverse.c...)
$(shell python3 $(LIBEXECINFO_DIR)/gen.py --max-depth 128 --output $(LIBEXECINFO_DIR)/stacktraverse.c >/dev/null)
endif

# Lua (MIT) -- the plugin scripting engine (src/plugins/plugin_manager.c):
# each file under the SD card's .plugins/ folder gets its own lua_State,
# with a small C API table (plugin.*) exposed for registering Home-screen
# tiles and driving playback, so third-party plugins are plain text .lua
# files rather than compiled code. Vendored as the official source tarball
# (library sources only -- LUA_SRCS below excludes lua.c/luac.c, the
# standalone interpreter/compiler CLI mains, since this is an embedded
# library build) rather than a system package, matching every other
# dependency here targeting mipsel-linux-musl.
ifeq ($(wildcard $(LUA_DIR)),)
$(info Fetching Lua 5.5.1 source...)
$(shell curl -fsSL -o /tmp/lua-5.5.1.tar.gz https://www.lua.org/ftp/lua-5.5.1.tar.gz && tar -xzf /tmp/lua-5.5.1.tar.gz -C /tmp && mv /tmp/lua-5.5.1 $(LUA_DIR) && rm /tmp/lua-5.5.1.tar.gz)
endif

# UI assets (theme2 -- real PNGs from the stock HiBy firmware, not ours to
# redistribute). On target these are read straight from the firmware's own
# /usr/resource/litegui/theme2 -- nothing to fetch. On host, best-effort
# mirror them from wherever the firmware happens to be unpacked locally on
# this dev machine, purely for the simulator to have something to show;
# missing assets just mean blank images on host, not a build failure.
ASSETS_DIR = assets
UNPACKED_THEME2 = ../../Hiby-R1-Mod/unpack_pack/squashfs-root/usr/resource/litegui/theme2
ifeq ($(wildcard $(ASSETS_DIR)/theme2),)
ifneq ($(wildcard $(UNPACKED_THEME2)),)
$(info Mirroring theme2 UI assets for the host simulator...)
$(shell mkdir -p $(ASSETS_DIR) && cp -r $(UNPACKED_THEME2) $(ASSETS_DIR)/theme2)
else
$(warning theme2 assets not found at $(UNPACKED_THEME2) -- host simulator will show blank images; target build is unaffected)
endif
endif

# Compile flags
# -DLV_CONF_INCLUDE_SIMPLE=1 is required to include lv_conf.h as "lv_conf.h"
# src/*/ dirs: so a quoted #include "foo.h" in one category (e.g. src/ui/gui.c src/ui/gui_subsonic.c src/ui/gui_settings.c src/ui/gui_network.c src/ui/gui_theme.c src/ui/gui_notifications.c src/ui/gui_library.c src/ui/gui_queue.c src/ui/gui_player.c src/ui/gui_plugins.c src/ui/gui_shell.c src/ui/gui_navigation.c src/ui/gui_books.c src/ui/gui_text_input.c src/ui/gui_lyrics.c
# including "audio.h") still resolves via the compiler's -I fallback search
# even though foo.h now lives in a different category folder -- no #include
# statements needed changing when src/ was reorganized into subfolders.
# -MMD -MP: real-device/real-build bug report -- this project had NO
# per-file #include dependency tracking at all (see LVGL_PATCH_STAMP's own
# comment further up, which documents the one narrow case someone already
# got bitten by and hand-worked-around). Concretely: changing a widely-
# included header (e.g. screen_builders.h's STATUS_BAR_CLEARANCE) and
# rebuilding reported success, but every .c file that only #includes that
# header -- without itself having changed -- kept its stale, pre-change
# object file, since Make's default dependency is just "object newer than
# its own .c", with no idea a header it read also matters. The fix silently
# never reached most of the app (built fine, wrong binary) until a full
# `make clean` was manually forced. -MMD makes each compile emit a matching
# .d file listing every header that translation unit actually included;
# -MP adds a phony target for each of those headers too, so a header being
# DELETED (not just edited) doesn't leave a dangling prerequisite that
# breaks the build outright. The -include of build_target/build_host's own
# *.d files near the bottom of this Makefile is what actually feeds these
# back in on the next invocation -- this flag alone does nothing without
# that companion include.
CFLAGS = -O3 -g -Wall -MMD -MP -I. -Isrc/audio -Isrc/network -Isrc/library -Isrc/hardware -Isrc/ui -Isrc/core -Isrc/plugins -I$(LVGL_DIR) -I$(DR_LIBS_DIR) -I$(FAAD2_DIR)/include -I$(ALAC_DIR)/codec -I$(MBEDTLS_DIR)/include -I$(CJSON_DIR) -I$(OPUS_DIR)/include -I$(LUA_DIR)/src -I$(STB_VORBIS_DIR) -DLV_CONF_INCLUDE_SIMPLE=1
CXXFLAGS = $(filter-out -Wall,$(CFLAGS)) -std=c++11
HOST_CFLAGS = $(CFLAGS) -DHOST_BUILD=1 $(BOARD_DEFINE) $(shell sdl2-config --cflags)
HOST_CXXFLAGS = $(CXXFLAGS) -DHOST_BUILD=1 $(BOARD_DEFINE) $(shell sdl2-config --cflags)
# Optional: `make target TEST_BUILD_TAG=test25_avrcp` -- replaces the release
# label in About with a conspicuous test-build identity, so a binary running
# on the device can be identified without checking timestamps or hashes.
ifneq ($(TEST_BUILD_TAG),)
TEST_BUILD_TAG_DEFINE = -DTEST_BUILD_TAG=\"$(TEST_BUILD_TAG)\"
endif

# Human-facing version shown by About and plugin.get_app_info(). CI sets this
# to its release/artifact title (for example "Weekly Beta 2026-09-02"). Keep
# it separate from BUILD_STAMP: the bootloader parses that fixed-width stamp
# to compare internal and SD-card builds, whereas this label may contain spaces
# and must match the GitHub release name exactly. The single quotes keep the
# entire C string define one shell argument even when RELEASE_LABEL has spaces.
ifneq ($(RELEASE_LABEL),)
RELEASE_LABEL_DEFINE = -DAPP_RELEASE_LABEL='"$(RELEASE_LABEL)"'
endif

# Optional low-overhead timing diagnostics for real-device UI profiling.
# Kept out of normal/release builds unless explicitly requested with
# `make target UI_PERF_TRACE=1`.
ifneq ($(UI_PERF_TRACE),)
UI_PERF_TRACE_DEFINE = -DUI_PERF_TRACE=1
endif

ifneq ($(UI_GESTURE_TRACE),)
UI_GESTURE_TRACE_DEFINE = -DUI_GESTURE_TRACE=1
endif

# Outlines the player screen's transport-row icons (mode/prev/play/next/more)
# in distinct colors at their real click hit-test boundary -- including
# lv_obj_set_ext_click_area()'s invisible padding, not just each icon's own
# drawn size -- so a real-device hitbox/overlap question can be answered by
# looking at the screen instead of reasoning about flex-gap math. Kept out
# of normal/release builds unless explicitly requested with
# `make target UI_HITBOX_DEBUG=1`.
ifneq ($(UI_HITBOX_DEBUG),)
UI_HITBOX_DEBUG_DEFINE = -DUI_HITBOX_DEBUG=1
endif

# Always defined (no flag needed) for the machine-facing build identifier used
# by plugin.get_app_info() and the bootloader version comparison. Re-evaluated
# on every `make target` invocation, so it reflects when this binary was built,
# not when the Makefile was last edited.
BUILD_STAMP_DEFINE = -DBUILD_STAMP=\"$(shell date +%Y-%m-%d_%H:%M)\"

# -I$(LIBEXECINFO_DIR) precedes the rest so its execinfo.h is what main.c's
# #include <execinfo.h> resolves to on target: musl (unlike host's glibc)
# ships no execinfo.h/backtrace() of its own, which is what main.c's SIGSEGV
# handler needs. Host build doesn't need this -- glibc already provides it.
TARGET_CFLAGS = $(CFLAGS) -I$(LIBEXECINFO_DIR) -I$(LVGL_DIR)/src -I$(TINYALSA_DIR)/include -Idbus_vendor_config -I$(DBUS_DIR) $(BOARD_DEFINE) $(TEST_BUILD_TAG_DEFINE) $(RELEASE_LABEL_DEFINE) $(UI_PERF_TRACE_DEFINE) $(UI_GESTURE_TRACE_DEFINE) $(UI_HITBOX_DEBUG_DEFINE) $(BUILD_STAMP_DEFINE)
TARGET_CXXFLAGS = $(CXXFLAGS) -I$(LIBEXECINFO_DIR) -I$(LVGL_DIR)/src -I$(TINYALSA_DIR)/include -Idbus_vendor_config -I$(DBUS_DIR) $(BOARD_DEFINE) $(TEST_BUILD_TAG_DEFINE) $(RELEASE_LABEL_DEFINE) $(UI_PERF_TRACE_DEFINE) $(UI_GESTURE_TRACE_DEFINE) $(UI_HITBOX_DEBUG_DEFINE) $(BUILD_STAMP_DEFINE)
TINYALSA_CFLAGS = -O3 -g -Wall -I$(TINYALSA_DIR)/include -I$(TINYALSA_DIR)/src
# DBUS_COMPILATION/DBUS_STATIC_BUILD: libdbus's own headers gate some
# declarations on these (matching how its own build always defines them
# for an in-tree build). _GNU_SOURCE: several of libdbus's own sysdeps
# files need this for `environ`, getresuid()/getresgid(), and SO_PEERCRED
# (struct ucred) -- confirmed live via mipsel-linux-musl-gcc failing
# without it on exactly those symbols.
DBUS_CFLAGS = -O2 -Wall -D_GNU_SOURCE -DDBUS_COMPILATION -DDBUS_STATIC_BUILD -Idbus_vendor_config -I$(DBUS_DIR)
# Matches FAAD2's own CMakeLists's non-fixed-point build config
FAAD2_DEFINES = -DHAVE_INTTYPES_H=1 -DHAVE_MEMCPY=1 -DHAVE_STRING_H=1 -DHAVE_STRINGS_H=1 \
                -DHAVE_SYS_STAT_H=1 -DHAVE_SYS_TYPES_H=1 -DHAVE_LRINTF=1 -DAPPLY_DRC -DPACKAGE_VERSION=\"2.11.2\"
FAAD2_CFLAGS = -O3 -g -Wall -I$(FAAD2_DIR)/include -I$(FAAD2_DIR)/libfaad $(FAAD2_DEFINES)
# EndianPortable.c (vendored, unmodified) only auto-detects little-endian for
# __i386__/__x86_64__/Win32 -- mipsel is little-endian too but isn't in that
# list, so without this define it silently skips byte-swapping the ALAC
# magic cookie's sampleRate field on the target build (confirmed on real
# hardware: sample_rate came out as 1152122880 instead of 44100, exactly the
# byte-reversed value). Defining it here is a no-op on host (x86_64 already
# self-detects) and the actual fix on target.
ALAC_DEFINES = -DTARGET_RT_LITTLE_ENDIAN=1
ALAC_CFLAGS = -O3 -g -Wall -I$(ALAC_DIR)/codec $(ALAC_DEFINES)
ALAC_CXXFLAGS = -O3 -g -I$(ALAC_DIR)/codec -std=c++11 $(ALAC_DEFINES)
# No configure/cmake step is run for libopus here (same as FAAD2/ALAC above)
# -- these mirror libopus's own CMakeLists.txt default (non-fixed-point,
# non-custom-modes, no runtime CPU/SIMD detection) build config instead of
# guessing. VAR_ARRAYS selects C99 variable-length-array stack allocation
# (celt/stack_alloc.h requires exactly one of VAR_ARRAYS/USE_ALLOCA/
# NONTHREADSAFE_PSEUDOSTACK to be defined) -- this is CMake's own default
# whenever the compiler supports VLAs, true for mipsel-linux-musl-gcc same
# as any other GCC. HAVE_LRINTF/HAVE_LRINT select the fast C99 lrintf()/
# lrint() rounding path in celt/float_cast.h over its slower portable
# fallback -- available since the target isn't built with -ansi/-std=c89.
# FIXED_POINT is deliberately left undefined: this target already relies on
# float pervasively (WMA's hand-written decoder, FAAD2's own non-fixed-point
# mode) with no soft-float workaround flags anywhere in this Makefile, so
# the floating-point build -- simpler to get right under this no-configure
# constraint than fixed-point's Q-format SILK API -- is the correct choice.
OPUS_DEFINES = -DOPUS_BUILD -DVAR_ARRAYS -DHAVE_LRINTF=1 -DHAVE_LRINT=1
OPUS_CFLAGS = -O3 -g -Wall -I$(OPUS_DIR)/include -I$(OPUS_DIR)/celt -I$(OPUS_DIR)/silk -I$(OPUS_DIR)/silk/float $(OPUS_DEFINES)
# No configure step (luaconf.h auto-detects a POSIX/Linux target off the
# compiler's own predefined __linux__/__unix__ macros, which musl's cross
# compiler still defines for a Linux target -- same as every other
# no-configure dependency in this Makefile). No LUA_USE_DLOPEN: package
# C-module loading needs dlopen(), which doesn't work from a static musl
# binary anyway (see the mbedTLS comment above); loadlib.c's own fallback
# stub handles that absence cleanly, and plugins load as plain .lua text
# via luaL_loadfile(), never as compiled C modules.
LUA_CFLAGS = -O2 -g -Wall -I$(LUA_DIR)/src

# Link flags
HOST_LDFLAGS = $(shell sdl2-config --libs) -lpthread -lm
TARGET_LDFLAGS = -static -no-pie -lpthread -lm

# Source files -- organized under src/ by category: audio/ (playback engine +
# format decoders/demuxers), network/ (wifi/bluetooth/dlna/remote-control/
# streaming), library/ (metadata/file browsing/playlists), hardware/ (device
# control), ui/ (gui/screens/assets/fonts), core/ (settings, subprocess,
# misc). main.c stays at src/ root as the entry point.
APP_SRCS = src/main.c src/ui/gui.c src/ui/gui_subsonic.c src/ui/gui_settings.c src/ui/gui_network.c src/ui/gui_theme.c src/ui/gui_notifications.c src/ui/gui_library.c src/ui/gui_queue.c src/ui/gui_player.c src/ui/gui_track_info.c src/ui/gui_plugins.c src/ui/gui_shell.c src/ui/gui_navigation.c src/ui/gui_books.c src/ui/gui_text_input.c src/ui/gui_lyrics.c src/ui/gui_reload.c src/audio/audio.c src/library/file_browser.c src/hardware/hw_buttons.c src/hardware/input_device_utils.c src/library/metadata.c src/library/metadata_db.c src/core/settings.c src/core/app_version.c src/audio/aiff_decoder.c src/audio/dsd_filter.c src/audio/dsd_decoder.c src/audio/aac_decoder.c src/audio/mp4_demux.c src/audio/ape_demux.c src/audio/ape_decoder.c src/audio/peq.c src/ui/assets.c src/ui/screen_builders.c src/hardware/battery.c src/network/wifi_status.c src/network/ca_bundle.c src/network/http_conn.c src/network/http_client.c src/network/http_stream.c src/network/subsonic_client.c src/library/cover_decode.c src/library/lyrics.c src/audio/asf_demux.c src/audio/wma_decoder.c src/audio/ogg_demux.c src/audio/opus_decoder.c src/audio/vorbis_decoder.c src/library/cue_parser.c src/ui/fallback_font.c \
src/core/subprocess.c src/network/wifi_control.c src/network/bluetooth_control.c src/network/hiby_sys_server.c src/hardware/backlight.c src/network/import_web.c src/network/airplay_control.c src/network/airplay_bridge.c src/network/airplay_metadata.c src/hardware/headphone_status.c src/hardware/device_config.c src/hardware/led_control.c src/hardware/charge_limiter.c src/core/idle_shutdown.c src/hardware/power_suspend.c src/core/text_reader.c src/hardware/usb_mode_control.c src/hardware/usb_dac_bridge.c src/hardware/usb_audio_output.c src/core/firmware_update.c src/library/playlist_files.c src/core/timezone_data.c src/core/timezone_apply.c src/core/hostname_apply.c src/network/dlna_control.c src/network/remote_control.c src/plugins/plugin_manager.c
APP_SRCS += src/ui/lyrics_layout.c src/ui/transition_compositor.c
APP_SRCS += src/plugins/plugin_json.c src/plugins/plugin_storage.c src/plugins/plugin_disabled_list.c
APP_SRCS += src/ui/gui_plugin_manage.c src/ui/gui_lock_screen.c
APP_SRCS += src/library/remote_track.c
APP_SRCS += src/library/queue_resume.c
APP_SRCS += src/library/albumart.c src/library/tagcache.c src/library/path_cache.c src/library/remote_state.c src/library/subsonic_saved_servers.c src/library/artwork_coordinator.c
APP_SRCS += src/core/utf8_util.c src/core/app_clock.c
APP_SRCS += src/ui/gesture_detector.c
APP_CXX_SRCS = src/audio/alac_decoder.cpp
LVGL_SRCS = $(shell find $(LVGL_DIR)/src -type f -name '*.c')
TINYALSA_SRCS = $(shell find $(TINYALSA_DIR)/src -type f -name '*.c')
FAAD2_SRCS = $(shell find $(FAAD2_DIR)/libfaad -type f -name '*.c')
# Decoder-only ALAC sources (the repo also ships an encoder we don't need)
ALAC_C_SRCS = $(ALAC_DIR)/codec/ag_dec.c $(ALAC_DIR)/codec/dp_dec.c $(ALAC_DIR)/codec/matrix_dec.c \
              $(ALAC_DIR)/codec/ALACBitUtilities.c $(ALAC_DIR)/codec/EndianPortable.c
ALAC_CXX_SRCS = $(ALAC_DIR)/codec/ALACDecoder.cpp
# libopus source set, verified against this checkout's own opus_sources.mk/
# celt_sources.mk/silk_sources.mk and CMakeLists.txt (not guessed): base
# OPUS_SOURCES + OPUS_SOURCES_FLOAT (the float-build analysis/mlp files) +
# CELT_SOURCES + SILK_SOURCES + SILK_SOURCES_FLOAT. Excludes celt/silk's
# x86/arm/mips SIMD subdirectories and silk/fixed (fixed-point only, unused
# since FIXED_POINT is left undefined above) -- none of those are pulled in
# by the base CMake build either without explicit RTCD/fixed-point options,
# which this Makefile doesn't set. The dnn/ (LPCNet-based DRED/OSCE/Deep
# PLC) directory is excluded entirely -- confirmed via CMakeLists.txt that
# those sources are only added when OPUS_DRED/OPUS_OSCE/OPUS_DEEP_PLC are
# explicitly enabled, all off by default, so this stays a plain SILK+CELT
# decoder build with no separate DNN component to vendor.
OPUS_SRCS = $(filter-out %/repacketizer_demo.c %/opus_demo.c %/opus_compare.c %/opus_custom_demo.c, \
              $(shell find $(OPUS_DIR)/src $(OPUS_DIR)/celt $(OPUS_DIR)/silk -maxdepth 1 -type f -name '*.c')) \
            $(shell find $(OPUS_DIR)/silk/float -maxdepth 1 -type f -name '*.c')
MBEDTLS_SRCS = $(shell find $(MBEDTLS_DIR)/library -type f -name '*.c')
CJSON_SRCS = $(CJSON_DIR)/cJSON.c
# stb_vorbis.c is its own complete translation unit (the real implementation,
# compiled exactly once here); stb_vorbis.h is a header-only shim other .c
# files include instead -- see that file's own comment.
STB_VORBIS_SRCS = $(STB_VORBIS_DIR)/stb_vorbis.c
# Library sources only (verified against this checkout's own doc/readme.html
# file list) -- excludes lua.c/luac.c, the standalone interpreter/compiler
# CLI mains, since this is an embedded library build.
LUA_SRCS = $(addprefix $(LUA_DIR)/src/, lapi.c lcode.c lctype.c ldebug.c ldo.c ldump.c lfunc.c lgc.c \
             llex.c lmem.c lobject.c lopcodes.c lparser.c lstate.c lstring.c ltable.c ltm.c lundump.c \
             lvm.c lzio.c lauxlib.c lbaselib.c lcorolib.c ldblib.c liolib.c lmathlib.c loadlib.c \
             loslib.c lstrlib.c ltablib.c lutf8lib.c linit.c)
# Excludes (all confirmed via a real link+run test on the actual device
# before wiring this in): Windows/WinCE sources (irrelevant, this is
# Linux-only), dbus-server*.c (DBusServer is for LISTENING for incoming
# connections -- what dbus-daemon itself does; this app is purely a
# CLIENT of the already-running system bus, registering an object on an
# existing connection, never accepting one), dbus-spawn-unix.c (service
# activation -- this app never launches a D-Bus service on demand),
# dbus-test*.c (libdbus's own test harness), dbus-uuidgen.c (the
# `dbus-uuidgen` command-line tool's own main(), not library code),
# dbus-pollable-set-epoll.c (using the plain poll()-based variant instead,
# see dbus_vendor_config/config.h -- one less file, no scale need for
# epoll's advantage over poll() for this app's handful of fds),
# dbus-message-util.c (fuzzing/test helper, not needed at runtime).
DBUS_SRCS = $(filter-out %-win.c %-win32.c %wince-glue.c $(DBUS_DIR)/dbus/dbus-server%.c \
              $(DBUS_DIR)/dbus/dbus-spawn-unix.c $(DBUS_DIR)/dbus/dbus-test%.c \
              $(DBUS_DIR)/dbus/dbus-uuidgen.c $(DBUS_DIR)/dbus/dbus-pollable-set-epoll.c \
              $(DBUS_DIR)/dbus/dbus-message-util.c, \
              $(shell find $(DBUS_DIR)/dbus -maxdepth 1 -name '*.c'))
# bt_media_player.c (AVRCP transport-button service, see the DBUS_DIR
# section above) needs libdbus, so it's target-only -- not part of
# APP_SRCS (shared with the host simulator build, which has no Bluetooth
# stack to talk to anyway). Its call sites in gui.c/bluetooth_control.c
# are guarded with #ifndef HOST_BUILD for the same reason audio.c's own
# Bluetooth-output code is.
TARGET_ONLY_APP_SRCS = src/network/bt_media_player.c src/audio/audio_output.c
# Target-only (see TARGET_CFLAGS's own comment on why): provides
# backtrace()/backtrace_symbols_fd() for main.c's SIGSEGV handler on musl,
# which has no execinfo.h/backtrace() of its own.
LIBEXECINFO_SRCS = $(LIBEXECINFO_DIR)/execinfo.c $(LIBEXECINFO_DIR)/stacktraverse.c

# Object files
HOST_OBJS = $(APP_SRCS:src/%.c=$(BUILD_HOST_DIR)/%.o) $(APP_CXX_SRCS:src/%.cpp=$(BUILD_HOST_DIR)/%.o) \
            $(LVGL_SRCS:$(LVGL_DIR)/%.c=$(BUILD_HOST_DIR)/lvgl/%.o) $(FAAD2_SRCS:$(FAAD2_DIR)/libfaad/%.c=$(BUILD_HOST_DIR)/faad2/%.o) \
            $(ALAC_C_SRCS:$(ALAC_DIR)/codec/%.c=$(BUILD_HOST_DIR)/alac/%.o) $(ALAC_CXX_SRCS:$(ALAC_DIR)/codec/%.cpp=$(BUILD_HOST_DIR)/alac/%.o) \
            $(MBEDTLS_SRCS:$(MBEDTLS_DIR)/library/%.c=$(BUILD_HOST_DIR)/mbedtls/%.o) $(CJSON_SRCS:$(CJSON_DIR)/%.c=$(BUILD_HOST_DIR)/cjson/%.o) \
            $(OPUS_SRCS:$(OPUS_DIR)/%.c=$(BUILD_HOST_DIR)/opus/%.o) \
            $(STB_VORBIS_SRCS:$(STB_VORBIS_DIR)/%.c=$(BUILD_HOST_DIR)/stb_vorbis/%.o) \
            $(LUA_SRCS:$(LUA_DIR)/src/%.c=$(BUILD_HOST_DIR)/lua/%.o)
TARGET_OBJS = $(APP_SRCS:src/%.c=$(BUILD_TARGET_DIR)/%.o) $(APP_CXX_SRCS:src/%.cpp=$(BUILD_TARGET_DIR)/%.o) \
              $(TARGET_ONLY_APP_SRCS:src/%.c=$(BUILD_TARGET_DIR)/%.o) \
              $(LVGL_SRCS:$(LVGL_DIR)/%.c=$(BUILD_TARGET_DIR)/lvgl/%.o) $(TINYALSA_SRCS:$(TINYALSA_DIR)/%.c=$(BUILD_TARGET_DIR)/tinyalsa/%.o) \
              $(FAAD2_SRCS:$(FAAD2_DIR)/libfaad/%.c=$(BUILD_TARGET_DIR)/faad2/%.o) \
              $(ALAC_C_SRCS:$(ALAC_DIR)/codec/%.c=$(BUILD_TARGET_DIR)/alac/%.o) $(ALAC_CXX_SRCS:$(ALAC_DIR)/codec/%.cpp=$(BUILD_TARGET_DIR)/alac/%.o) \
              $(MBEDTLS_SRCS:$(MBEDTLS_DIR)/library/%.c=$(BUILD_TARGET_DIR)/mbedtls/%.o) $(CJSON_SRCS:$(CJSON_DIR)/%.c=$(BUILD_TARGET_DIR)/cjson/%.o) \
              $(DBUS_SRCS:$(DBUS_DIR)/dbus/%.c=$(BUILD_TARGET_DIR)/dbus/%.o) \
              $(OPUS_SRCS:$(OPUS_DIR)/%.c=$(BUILD_TARGET_DIR)/opus/%.o) \
              $(STB_VORBIS_SRCS:$(STB_VORBIS_DIR)/%.c=$(BUILD_TARGET_DIR)/stb_vorbis/%.o) \
              $(LUA_SRCS:$(LUA_DIR)/src/%.c=$(BUILD_TARGET_DIR)/lua/%.o) \
              $(LIBEXECINFO_SRCS:$(LIBEXECINFO_DIR)/%.c=$(BUILD_TARGET_DIR)/libexecinfo/%.o)

.PHONY: all host target bootloader sd_ready_test cover_decode_scale_test clean compile_commands.json FORCE_VERSION

# Default target builds for host simulation and generates compile commands for IDE
all: host compile_commands.json

# Build for host (Arch Linux PC)
host: $(HOST_BIN)

$(HOST_BIN): $(HOST_OBJS)
	$(CXX) -o $@ $(HOST_OBJS) $(HOST_LDFLAGS)
	@echo "Host build complete: Run './$(HOST_BIN)' to start the simulator."

# See LVGL_PATCH's own comment further up for why this exists. Depends on
# the two fbdev source files themselves (so editing/reverting either one
# by hand invalidates the stamp via a normal newer-than-target check) and
# on the tracked patch file (so a future change to the patch itself also
# invalidates it), not just on $(LVGL_DIR) existing.
$(LVGL_PATCH_STAMP): $(LVGL_FBDEV_C) $(LVGL_FBDEV_H) $(LVGL_PATCH) \
                     $(LVGL_TINY_TTF) $(LVGL_TJPGDCNF) $(LVGL_LRU_RB) $(LVGL_LODEPNG) $(LVGL_RUNTIME_FIXES_PATCH) \
                     $(LVGL_FONT_TARGETS) $(LVGL_FONT_GOLDEN)
	@set -e; \
	actual_commit=$$(git -C $(LVGL_DIR) rev-parse HEAD 2>/dev/null || echo ""); \
	if [ "$$actual_commit" != "$(LVGL_PINNED_COMMIT)" ]; then \
	  echo "ERROR: $(LVGL_DIR) is at commit '$$actual_commit', not the pinned LVGL v9.1.0 commit $(LVGL_PINNED_COMMIT)."; \
	  echo "       This repo's LVGL patches/golden files are only known to apply cleanly to that exact revision."; \
	  echo "       Remove $(LVGL_DIR) and re-run make to re-clone the pinned tag, or update"; \
	  echo "       LVGL_PINNED_COMMIT and the patches/golden files together if intentionally bumping LVGL."; \
	  exit 1; \
	fi; \
	if patch -p1 -d $(LVGL_DIR) --force --fuzz=0 --dry-run --reverse < $(LVGL_PATCH) >/dev/null 2>&1; then \
	  echo "LVGL fbdev compositor patch already applied in $(LVGL_DIR) and matches $(LVGL_PATCH) exactly, skipping."; \
	elif patch -p1 -d $(LVGL_DIR) --force --fuzz=0 --dry-run < $(LVGL_PATCH) >/dev/null 2>&1; then \
	  echo "Applying $(LVGL_PATCH) to $(LVGL_DIR)..."; \
	  patch -p1 -d $(LVGL_DIR) --force --fuzz=0 < $(LVGL_PATCH) || { \
	    echo "ERROR: $(LVGL_PATCH) failed to apply to $(LVGL_DIR) even though a dry run just succeeded --"; \
	    echo "       investigate before building (disk full, read-only checkout, concurrent modification)."; \
	    exit 1; \
	  }; \
	  present=0; total=$$(echo $(LVGL_FBDEV_SYMBOLS) | wc -w); \
	  for sym in $(LVGL_FBDEV_SYMBOLS); do \
	    grep -q "$$sym" $(LVGL_FBDEV_H) && grep -q "$$sym" $(LVGL_FBDEV_C) && present=$$((present + 1)); \
	  done; \
	  if [ "$$present" -ne "$$total" ]; then \
	    echo "ERROR: patch applied but only $$present/$$total compositor symbols have both a declaration"; \
	    echo "       (in $(LVGL_FBDEV_H)) and a definition (in $(LVGL_FBDEV_C)) afterward."; \
	    exit 1; \
	  fi; \
	  echo "LVGL fbdev compositor patch applied successfully."; \
	else \
	  echo "ERROR: $(LVGL_DIR) matches the pinned commit $(LVGL_PINNED_COMMIT) but its fbdev driver files are"; \
	  echo "       neither a pristine match for $(LVGL_PATCH) nor an exact match for its already-applied"; \
	  echo "       result -- partially patched, hand-modified, or patched against a since-updated"; \
	  echo "       $(LVGL_PATCH). Remove $(LVGL_DIR) and re-run make to start from a clean pinned checkout."; \
	  exit 1; \
	fi; \
	if patch -p1 -d $(LVGL_DIR) --force --fuzz=0 --dry-run --reverse < $(LVGL_RUNTIME_FIXES_PATCH) >/dev/null 2>&1; then \
	  echo "LVGL runtime-fixes patch already applied in $(LVGL_DIR) and matches $(LVGL_RUNTIME_FIXES_PATCH) exactly, skipping."; \
	elif patch -p1 -d $(LVGL_DIR) --force --fuzz=0 --dry-run < $(LVGL_RUNTIME_FIXES_PATCH) >/dev/null 2>&1; then \
	  echo "Applying $(LVGL_RUNTIME_FIXES_PATCH) to $(LVGL_DIR)..."; \
	  patch -p1 -d $(LVGL_DIR) --force --fuzz=0 < $(LVGL_RUNTIME_FIXES_PATCH) || { \
	    echo "ERROR: $(LVGL_RUNTIME_FIXES_PATCH) failed to apply to $(LVGL_DIR) even though a dry run just succeeded --"; \
	    echo "       investigate before building (disk full, read-only checkout, concurrent modification)."; \
	    exit 1; \
	  }; \
	  echo "LVGL runtime-fixes patch applied successfully."; \
	else \
	  echo "ERROR: $(LVGL_DIR) matches the pinned commit $(LVGL_PINNED_COMMIT) but tiny_ttf/tjpgd/lru-rb/lodepng are"; \
	  echo "       neither a pristine match for $(LVGL_RUNTIME_FIXES_PATCH) nor an exact match for its"; \
	  echo "       already-applied result. Remove $(LVGL_DIR) and re-run make to start from a clean checkout."; \
	  exit 1; \
	fi; \
	for f in $(LVGL_GENERATED_FONTS); do \
	  target=$(LVGL_DIR)/src/font/$$f; golden=$(LVGL_GENERATED_FONTS_DIR)/$$f; \
	  if cmp -s "$$target" "$$golden"; then \
	    echo "LVGL generated font $$f already matches $$golden, skipping."; \
	  elif git -C $(LVGL_DIR) diff --quiet -- src/font/$$f 2>/dev/null; then \
	    echo "Installing $$golden over $$target..."; \
	    cp "$$golden" "$$target"; \
	  else \
	    echo "ERROR: $$target matches neither the pristine pinned-commit content nor $$golden --"; \
	    echo "       hand-modified or generated from a different source. Remove $(LVGL_DIR) and re-run"; \
	    echo "       make to start from a clean pinned checkout."; \
	    exit 1; \
	  fi; \
	done; \
	touch $@

$(BUILD_HOST_DIR)/%.o: src/%.c $(LVGL_PATCH_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(HOST_CFLAGS) -c $< -o $@

$(BUILD_HOST_DIR)/%.o: src/%.cpp $(LVGL_PATCH_STAMP)
	@mkdir -p $(dir $@)
	$(CXX) $(HOST_CXXFLAGS) -c $< -o $@

$(BUILD_HOST_DIR)/lvgl/%.o: $(LVGL_DIR)/%.c $(LVGL_PATCH_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(HOST_CFLAGS) -c $< -o $@

$(BUILD_HOST_DIR)/faad2/%.o: $(FAAD2_DIR)/libfaad/%.c
	@mkdir -p $(dir $@)
	$(CC) $(FAAD2_CFLAGS) -c $< -o $@

$(BUILD_HOST_DIR)/alac/%.o: $(ALAC_DIR)/codec/%.c
	@mkdir -p $(dir $@)
	$(CC) $(ALAC_CFLAGS) -c $< -o $@

$(BUILD_HOST_DIR)/alac/%.o: $(ALAC_DIR)/codec/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(ALAC_CXXFLAGS) -c $< -o $@

$(BUILD_HOST_DIR)/mbedtls/%.o: $(MBEDTLS_DIR)/library/%.c
	@mkdir -p $(dir $@)
	$(CC) $(HOST_CFLAGS) -c $< -o $@

$(BUILD_HOST_DIR)/cjson/%.o: $(CJSON_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(HOST_CFLAGS) -c $< -o $@

$(BUILD_HOST_DIR)/stb_vorbis/%.o: $(STB_VORBIS_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(HOST_CFLAGS) -c $< -o $@

$(BUILD_HOST_DIR)/opus/%.o: $(OPUS_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(OPUS_CFLAGS) -c $< -o $@

$(BUILD_HOST_DIR)/lua/%.o: $(LUA_DIR)/src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(LUA_CFLAGS) -DHOST_BUILD=1 -c $< -o $@

# Build for target (MIPS HiBy Device)
target: $(TARGET_BIN)

$(TARGET_BIN): $(TARGET_OBJS)
	$(CROSS_CXX) -o $(BUILD_TARGET_DIR)/$(TARGET_BIN)_unstripped $(TARGET_OBJS) $(TARGET_LDFLAGS)
	$(CROSS_STRIP) -s -o $@ $(BUILD_TARGET_DIR)/$(TARGET_BIN)_unstripped
	@echo "Target build complete: File ready at '$(TARGET_BIN)'"

# Standalone boot selector -- see src/bootloader/main.c's own top comment.
# Deliberately its own tiny static binary, not linked against LVGL/the main
# TARGET_OBJS: input_device_utils.c, subprocess.c, and tjpgd.c (for the
# /etc/logo1.jpeg background -- see fb_draw.c's own doc comment) are pulled
# in directly (all already dependency-free -- see their own files) rather
# than reusing TARGET_OBJS's build rule, so this never accidentally drags
# in the rest of the player/LVGL.
# Same r1-stays-bare reasoning as HOST_BIN/TARGET_BIN (below the BOARD
# selector block near the top of this file) -- without this, `make
# bootloader BOARD=r3proii` would silently overwrite the R1 bootloader
# sitting in the working directory, since (unlike the player binary and its
# object directory) this target's own output name was never suffixed.
ifeq ($(BOARD),r1)
BOOTLOADER_BIN = open_hiby_bootloader
else
BOOTLOADER_BIN = open_hiby_bootloader_$(BOARD)
endif
BOOTLOADER_SRCS = src/bootloader/main.c src/bootloader/fb_draw.c src/bootloader/input.c \
                  src/bootloader/scanner.c src/bootloader/sd_ready.c src/bootloader/sd_ready_real.c \
                  src/hardware/input_device_utils.c src/core/subprocess.c \
                  lvgl/src/libs/tjpgd/tjpgd.c
# -ffunction-sections/-fdata-sections + -Wl,--gc-sections: standard, safe
# combination that lets the linker drop unused functions/data at the
# granularity of individual symbols instead of whole .o files -- the only
# thing this bootloader intentionally over-links (subprocess.c, for the
# mount helper calls; input_device_utils.c) is small, but neither is used
# in full, so this actually earns its keep here rather than being cargo-cult.
BOOTLOADER_CFLAGS = -O2 -Wall -I. -Isrc/bootloader -Isrc/hardware -Isrc/core $(BOARD_DEFINE) -ffunction-sections -fdata-sections

bootloader:
	@mkdir -p $(BUILD_TARGET_DIR)
	$(CROSS_CC) $(BOOTLOADER_CFLAGS) -static -no-pie $(BOOTLOADER_SRCS) -o $(BUILD_TARGET_DIR)/$(BOOTLOADER_BIN)_unstripped -Wl,--gc-sections
	$(CROSS_STRIP) -s -o $(BOOTLOADER_BIN) $(BUILD_TARGET_DIR)/$(BOOTLOADER_BIN)_unstripped
	@echo "Bootloader build complete: File ready at '$(BOOTLOADER_BIN)'"

# Host-buildable unit tests for sd_ready.c's pure wait_for_sd_ready() state
# machine (see sd_ready_test.c's own top comment) -- plain host gcc, no
# cross toolchain, no dependency on the rest of the bootloader (scanner.c,
# subprocess.c, sd_ready_real.c's real mount/inotify/sysfs probes are never
# linked into this binary). Not part of `all`: run explicitly with
# `make sd_ready_test` after touching sd_ready.c or its header.
sd_ready_test:
	@mkdir -p $(BUILD_TARGET_DIR)
	$(CC) -O0 -g -Wall -Isrc/bootloader src/bootloader/sd_ready.c src/bootloader/sd_ready_test.c \
	    -o $(BUILD_TARGET_DIR)/sd_ready_test
	./$(BUILD_TARGET_DIR)/sd_ready_test

.PHONY: playlist-selftest
PLAYLIST_TEST_SANITIZERS ?=
playlist-selftest:
	@mkdir -p $(BUILD_TARGET_DIR)
	$(CC) -O1 -g -Wall -Wextra $(PLAYLIST_TEST_SANITIZERS) -ffunction-sections -fdata-sections -Isrc/library \
	    src/library/playlist_test.c src/library/playlist_files.c src/library/queue_resume.c \
	    -Wl,--gc-sections -lpthread -lm -o $(BUILD_TARGET_DIR)/playlist_test
	./$(BUILD_TARGET_DIR)/playlist_test

# Host-buildable tests for JPEG scale selection, tjpgd 1/2 1/4 1/8 decode,
# cover_decode_to_rgb565_ex, and malformed/oversized rejection. Links the
# real decoder + tjpgd + artwork coordinator; audio_is_playing and lodepng
# are stubbed in the test (JPEG-only). Not part of `all`.
cover_decode_scale_test:
	@mkdir -p $(BUILD_TARGET_DIR)
	$(CC) -O0 -g -Wall -DHOST_BUILD=1 -DLV_CONF_INCLUDE_SIMPLE=1 \
	    -I. -Isrc/library -Isrc/core -Isrc/audio -Ilvgl \
	    src/library/cover_decode_scale_test.c src/library/cover_decode.c \
	    src/library/artwork_coordinator.c lvgl/src/libs/tjpgd/tjpgd.c \
	    -lpthread -o $(BUILD_TARGET_DIR)/cover_decode_scale_test
	./$(BUILD_TARGET_DIR)/cover_decode_scale_test

$(BUILD_TARGET_DIR)/%.o: src/%.c $(LVGL_PATCH_STAMP)
	@mkdir -p $(dir $@)
	$(CROSS_CC) $(TARGET_CFLAGS) -c $< -o $@

# A release-label or build-stamp change is a Make variable change, not a source
# timestamp change. Rebuild this tiny unit every invocation so incremental
# builds cannot retain an earlier release identity; it also guarantees the
# bootloader scanner always has one current BUILD_STAMP to find.
FORCE_VERSION:

$(BUILD_TARGET_DIR)/core/app_version.o: src/core/app_version.c $(LVGL_PATCH_STAMP) FORCE_VERSION
	@mkdir -p $(dir $@)
	$(CROSS_CC) $(TARGET_CFLAGS) -c $< -o $@

$(BUILD_TARGET_DIR)/%.o: src/%.cpp $(LVGL_PATCH_STAMP)
	@mkdir -p $(dir $@)
	$(CROSS_CXX) $(TARGET_CXXFLAGS) -c $< -o $@

$(BUILD_TARGET_DIR)/lvgl/%.o: $(LVGL_DIR)/%.c $(LVGL_PATCH_STAMP)
	@mkdir -p $(dir $@)
	$(CROSS_CC) $(TARGET_CFLAGS) -c $< -o $@

$(BUILD_TARGET_DIR)/tinyalsa/%.o: $(TINYALSA_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CROSS_CC) $(TINYALSA_CFLAGS) -c $< -o $@

$(BUILD_TARGET_DIR)/faad2/%.o: $(FAAD2_DIR)/libfaad/%.c
	@mkdir -p $(dir $@)
	$(CROSS_CC) $(FAAD2_CFLAGS) -c $< -o $@

$(BUILD_TARGET_DIR)/alac/%.o: $(ALAC_DIR)/codec/%.c
	@mkdir -p $(dir $@)
	$(CROSS_CC) $(ALAC_CFLAGS) -c $< -o $@

$(BUILD_TARGET_DIR)/alac/%.o: $(ALAC_DIR)/codec/%.cpp
	@mkdir -p $(dir $@)
	$(CROSS_CXX) $(ALAC_CXXFLAGS) -c $< -o $@

$(BUILD_TARGET_DIR)/mbedtls/%.o: $(MBEDTLS_DIR)/library/%.c
	@mkdir -p $(dir $@)
	$(CROSS_CC) $(TARGET_CFLAGS) -c $< -o $@

$(BUILD_TARGET_DIR)/cjson/%.o: $(CJSON_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CROSS_CC) $(TARGET_CFLAGS) -c $< -o $@

$(BUILD_TARGET_DIR)/stb_vorbis/%.o: $(STB_VORBIS_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CROSS_CC) $(TARGET_CFLAGS) -c $< -o $@

$(BUILD_TARGET_DIR)/dbus/%.o: $(DBUS_DIR)/dbus/%.c
	@mkdir -p $(dir $@)
	$(CROSS_CC) $(DBUS_CFLAGS) -c $< -o $@

$(BUILD_TARGET_DIR)/opus/%.o: $(OPUS_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CROSS_CC) $(OPUS_CFLAGS) -c $< -o $@

$(BUILD_TARGET_DIR)/lua/%.o: $(LUA_DIR)/src/%.c
	@mkdir -p $(dir $@)
	$(CROSS_CC) $(LUA_CFLAGS) -c $< -o $@

$(BUILD_TARGET_DIR)/libexecinfo/%.o: $(LIBEXECINFO_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CROSS_CC) -O2 -g -Wall -Wno-frame-address -I$(LIBEXECINFO_DIR) -c $< -o $@

# Generate compile_commands.json for Zed/clangd LSP autofill and hover popups
compile_commands.json:
	@python3 generate_compile_commands.py

clean:
	rm -rf build_host build_host_* build_target build_target_* \
	    open_hiby_player_host open_hiby_player_host_* \
	    open_hiby_player_target open_hiby_player_target_* \
	    compile_commands.json compile_flags.txt

# Companion to -MMD -MP in CFLAGS above (see that comment for the real-
# device/real-build incident this exists to prevent) -- pulls in every
# already-generated .d file for whichever single board this invocation is
# actually building (BUILD_TARGET_DIR/BUILD_HOST_DIR are already resolved
# to that board's own suffixed directory by this point), so Make sees each
# object's real header dependencies, not just its own .c file. `-include`
# (not `include`) so a completely fresh checkout -- no .d files generated
# yet -- doesn't fail the very first build attempting to read files that
# don't exist; find's own 2>/dev/null covers the same case for when the
# directory itself doesn't exist yet either. Must stay at the end of the
# file: every .d this generates references targets/variables (BUILD_
# TARGET_DIR, BUILD_HOST_DIR) that need to already be fully resolved.
-include $(shell find $(BUILD_TARGET_DIR) $(BUILD_HOST_DIR) -name '*.d' 2>/dev/null)
