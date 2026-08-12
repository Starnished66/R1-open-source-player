#include "fallback_font.h"
#include "debug_log.h"

lv_font_t app_font_16;
lv_font_t app_font_22;
lv_font_t app_font_28;

/* Remembered from fallback_font_init_early()'s own argument so
 * fallback_font_load_deferred() (called later, from a timer with no
 * arguments of its own -- see fallback_font_schedule_deferred_load())
 * knows which pixel sizes to rasterize the CJK/Korean/Thai TTFs at,
 * instead of the fixed 16/22/28 this used before the Font Size feature
 * existed. */
static int s_font_size_tier = 0;

/* Same three-tier mapping as gui.c's apply_font_size_tier(), restricted to
 * the three slots this file actually needs (there is no fallback-aware
 * equivalent of gui.c's 20px status-bar-only slot). lv_tiny_ttf_create_file()
 * rasterizes on demand at whatever pixel size it's asked for -- unlike the
 * LV_FONT_MONTSERRAT_NN bitmap fonts, there's no fixed set of "enabled"
 * sizes to worry about here. */
static void tier_pixel_sizes(int tier, int * out_16_slot, int * out_22_slot, int * out_28_slot) {
    switch (tier) {
        case 1: /* Medium */
            *out_16_slot = 20;
            *out_22_slot = 26;
            *out_28_slot = 32;
            break;
        case 2: /* BlindMF */
            *out_16_slot = 24;
            *out_22_slot = 34;
            *out_28_slot = 40;
            break;
        default: /* Small -- the original, unchanged sizing */
            *out_16_slot = 16;
            *out_22_slot = 22;
            *out_28_slot = 28;
            break;
    }
}

/* Real-device incident (see lv_conf.h's own LV_USE_FREETYPE comment for the
 * full history): an earlier attempt at this used FreeType directly against
 * the stock firmware's /usr/resource/fonts/default.otf ("Noto Sans S
 * Chinese" -- confirmed via fc-query to have full Cyrillic and Japanese
 * kana/kanji coverage, just no Arabic) and hung the boot logo twice, never
 * root-caused. This retry avoids that specific font entirely: default.otf
 * uses CFF (PostScript) outlines, which LVGL's lighter tiny_ttf renderer
 * (TrueType-only, no FreeType) can't read. assets/fonts/cjk_cyrillic.ttf is
 * an OFFLINE conversion of that same font -- subsetted to Cyrillic +
 * Japanese kana/punctuation/fullwidth forms (fontTools `subset`) and
 * re-drawn with TrueType outlines instead of CFF (fontTools `otf2ttf`,
 * cubic-to-quadratic curve conversion) -- checked into this repo as a build
 * artifact, not regenerated on-device.
 *
 * Kanji coverage is restricted to the 2,136-character Joyo kanji list
 * (Japan's official "common use" standard, 2010 revision) rather than the
 * full ~27,500-character Unicode CJK Unified Ideographs + Extension A
 * repertoire the original subset shipped with -- real-device repack testing
 * found the full-coverage version alone added ~9MB to the flash image
 * (barely compressible: font glyph outline data is already fairly dense),
 * pushing the repacked .upt over its 45MB size limit. The Joyo codepoint
 * list itself comes from x0213.org's own published character-code table
 * (https://x0213.org/joyo-kanji-code/, CC-equivalent "distribution
 * unlimited" terms), not hand-typed -- see the git history for the exact
 * regeneration steps. This is a real, known tradeoff: any song/artist/album
 * tag using a kanji outside Joyo (uncommon proper nouns, place names, older
 * readings) will show a blank glyph instead of falling through to a
 * different working font, unlike a genuinely missing character. Revisit if
 * that turns out to matter in practice -- JIS X 0208 (~6,355 kanji) is the
 * next step up in coverage for a few more MB, if the flash budget allows it
 * later.
 *
 * Arabic isn't covered by any font already on this device, and needs real
 * contextual letter-shaping besides just glyph coverage -- tracked
 * separately, out of scope here. */
#ifdef HOST_BUILD
  #define FALLBACK_FONT_ROOT "assets/fonts/"
  #define FALLBACK_FONT_FILE FALLBACK_FONT_ROOT "cjk_cyrillic.ttf"
  #define KOREAN_FONT_FILE FALLBACK_FONT_ROOT "korean.ttf"
  #define THAI_FONT_FILE FALLBACK_FONT_ROOT "thai.ttf"
#else
  /* /usr/resource is read-only at runtime (confirmed live: mkdir there
   * fails with "Read-only file system"), but that's exactly right for a
   * static asset this app ships with. Korean.ttf/Thai.ttf are the stock
   * firmware's OWN font files -- already TrueType (no CFF -> otf2ttf
   * conversion needed, unlike default.otf) and already present at
   * /usr/resource/fonts/ on every device this app runs on, so they're read
   * straight from there instead of bundling app-local copies: nothing to
   * subset, nothing to bake in at repack time, no flash space spent
   * duplicating files the device already ships. cjk_cyrillic.ttf HAD to
   * become our own file (default.otf's CFF outlines aren't loadable by
   * tiny_ttf at all, so subsetting doubled as the only way to also get a
   * TrueType-outline copy) -- staged into this same /usr/resource/fonts/
   * directory at repack time (see TESTING.md/README's repack workflow)
   * rather than a separate open_hiby_player-owned subfolder, so all four
   * fallback-relevant fonts (default.otf, Korean.ttf, Thai.ttf,
   * cjk_cyrillic.ttf) live in one place. Only the DEV MACHINE's staging
   * copy of the squashfs needs to be writable for this -- unlike /usr/data
   * (this app's actual writable partition, used for settings/metadata_db/
   * etc), nothing here needs to survive being *rewritten* at runtime, only
   * to exist after a flash. */
  #define FALLBACK_FONT_FILE "/usr/resource/fonts/cjk_cyrillic.ttf"
  #define KOREAN_FONT_FILE "/usr/resource/fonts/Korean.ttf"
  #define THAI_FONT_FILE "/usr/resource/fonts/Thai.ttf"
#endif
/* Chained as a second and third fallback level behind cjk_cyrillic (see the
 * .fallback wiring in fallback_font_load_deferred() below) rather than
 * merged into one file -- lv_font_get_glyph_dsc() (lvgl/src/font/lv_font.c)
 * walks the whole f->fallback chain with no depth limit, so separate files
 * chain just as well as one merged one. */

void fallback_font_init_early(int font_size_tier) {
    s_font_size_tier = font_size_tier;

    /* lv_font_montserrat_NN are const bitmap fonts pre-rasterized at a
     * fixed size each -- unlike the TTF fallbacks below, there's no
     * "resize this one" option, so each tier picks a different pre-baked
     * Montserrat size entirely (see lv_conf.h's LV_FONT_MONTSERRAT_NN
     * enables for the full set this needs). */
    switch (font_size_tier) {
        case 1: /* Medium */
            app_font_16 = lv_font_montserrat_20;
            app_font_22 = lv_font_montserrat_26;
            app_font_28 = lv_font_montserrat_32;
            break;
        case 2: /* BlindMF */
            app_font_16 = lv_font_montserrat_24;
            app_font_22 = lv_font_montserrat_34;
            app_font_28 = lv_font_montserrat_40;
            break;
        default: /* Small -- the original, unchanged sizing */
            app_font_16 = lv_font_montserrat_16;
            app_font_22 = lv_font_montserrat_22;
            app_font_28 = lv_font_montserrat_28;
            break;
    }
}

#ifndef HOST_BUILD
/* main.c's own boot-checkpoint logger -- see its own comment there and
 * gui.c's identical extern for why this is called directly rather than
 * through some shared header (the still-open cold-boot investigation this
 * exists for predates this file). Persists to /usr/data/boot_debug.log
 * even if the process hangs right after a call, unlike DBG_LOG alone --
 * exactly the diagnostic the previous FreeType attempt didn't have. */
extern void boot_checkpoint(const char * step);
#endif

/* The actual font load is the part suspected of hanging boot last time --
 * this app's own auto-resume path (gui_init(), before the first frame is
 * ever pushed to the display) can synchronously render a label holding a
 * non-Latin last-played track title, which would have called straight into
 * FreeType's font-load-and-rasterize path right there. This function is
 * therefore never called directly -- only scheduled via a one-shot lv_timer
 * from fallback_font_schedule_deferred_load() below, itself only called as
 * the very last statement in gui_init(), after the first frame is already
 * showing and any auto-resume playback has already started. Any
 * already-built label referencing &app_font_16/&app_font_22 picks up the
 * fallback automatically on its next redraw once this returns; nothing
 * needs to be rebuilt. */
static void fallback_font_load_deferred(lv_timer_t * timer) {
    lv_timer_delete(timer); /* one-shot -- never needs to fire again */

#ifndef HOST_BUILD
    boot_checkpoint("fallback_font_load_deferred entered");
#endif
    DBG_LOG("fallback_font: loading %s (tier=%d)\n", FALLBACK_FONT_FILE, s_font_size_tier);
    lv_tiny_ttf_init();

    int size_16, size_22, size_28;
    tier_pixel_sizes(s_font_size_tier, &size_16, &size_22, &size_28);

    lv_font_t * cjk_16 = lv_tiny_ttf_create_file("S:" FALLBACK_FONT_FILE, size_16);
    if (cjk_16) {
        app_font_16.fallback = cjk_16;
        DBG_LOG("fallback_font: %dpx loaded\n", size_16);
    } else {
        DBG_LOG("fallback_font: %dpx load failed\n", size_16);
    }
#ifndef HOST_BUILD
    boot_checkpoint("fallback_font: 16px done");
#endif

    lv_font_t * cjk_22 = lv_tiny_ttf_create_file("S:" FALLBACK_FONT_FILE, size_22);
    if (cjk_22) {
        app_font_22.fallback = cjk_22;
        DBG_LOG("fallback_font: %dpx loaded\n", size_22);
    } else {
        DBG_LOG("fallback_font: %dpx load failed\n", size_22);
    }
#ifndef HOST_BUILD
    boot_checkpoint("fallback_font: 22px done");
#endif

    lv_font_t * cjk_28 = lv_tiny_ttf_create_file("S:" FALLBACK_FONT_FILE, size_28);
    if (cjk_28) {
        app_font_28.fallback = cjk_28;
        DBG_LOG("fallback_font: %dpx loaded\n", size_28);
    } else {
        DBG_LOG("fallback_font: %dpx load failed\n", size_28);
    }
#ifndef HOST_BUILD
    boot_checkpoint("fallback_font: 28px done");
#endif

    /* Chained behind whichever cjk_* font actually loaded above (cjk_16/22/28
     * can be NULL if that load failed) -- chaining onto NULL would just leave
     * app_font_*'s own .fallback unset instead of forming the 3-level chain,
     * so each size falls back to app_font_* directly if cjk failed, same as
     * before Korean/Thai existed. */
    lv_font_t * kr_16 = lv_tiny_ttf_create_file("S:" KOREAN_FONT_FILE, size_16);
    lv_font_t * th_16 = lv_tiny_ttf_create_file("S:" THAI_FONT_FILE, size_16);
    if (kr_16) DBG_LOG("fallback_font: korean %dpx loaded\n", size_16); else DBG_LOG("fallback_font: korean %dpx load failed\n", size_16);
    if (th_16) DBG_LOG("fallback_font: thai %dpx loaded\n", size_16); else DBG_LOG("fallback_font: thai %dpx load failed\n", size_16);
    if (kr_16) kr_16->fallback = th_16;
    if (cjk_16) cjk_16->fallback = kr_16;
    else if (kr_16) app_font_16.fallback = kr_16;
#ifndef HOST_BUILD
    boot_checkpoint("fallback_font: korean/thai 16px done");
#endif

    lv_font_t * kr_22 = lv_tiny_ttf_create_file("S:" KOREAN_FONT_FILE, size_22);
    lv_font_t * th_22 = lv_tiny_ttf_create_file("S:" THAI_FONT_FILE, size_22);
    if (kr_22) DBG_LOG("fallback_font: korean %dpx loaded\n", size_22); else DBG_LOG("fallback_font: korean %dpx load failed\n", size_22);
    if (th_22) DBG_LOG("fallback_font: thai %dpx loaded\n", size_22); else DBG_LOG("fallback_font: thai %dpx load failed\n", size_22);
    if (kr_22) kr_22->fallback = th_22;
    if (cjk_22) cjk_22->fallback = kr_22;
    else if (kr_22) app_font_22.fallback = kr_22;
#ifndef HOST_BUILD
    boot_checkpoint("fallback_font: korean/thai 22px done");
#endif

    lv_font_t * kr_28 = lv_tiny_ttf_create_file("S:" KOREAN_FONT_FILE, size_28);
    lv_font_t * th_28 = lv_tiny_ttf_create_file("S:" THAI_FONT_FILE, size_28);
    if (kr_28) DBG_LOG("fallback_font: korean %dpx loaded\n", size_28); else DBG_LOG("fallback_font: korean %dpx load failed\n", size_28);
    if (th_28) DBG_LOG("fallback_font: thai %dpx loaded\n", size_28); else DBG_LOG("fallback_font: thai %dpx load failed\n", size_28);
    if (kr_28) kr_28->fallback = th_28;
    if (cjk_28) cjk_28->fallback = kr_28;
    else if (kr_28) app_font_28.fallback = kr_28;
#ifndef HOST_BUILD
    boot_checkpoint("fallback_font: korean/thai 28px done");
#endif
}

/* 500ms, not sooner -- gives LVGL's own timer handler + display flush at
 * least one full cycle to actually push the first frame to the panel
 * before this fires, on top of the one-cycle guarantee lv_timer_create()
 * already gives structurally (see this file's own top-of-function
 * comments). Cheap either way: this only registers the timer, it doesn't
 * run fallback_font_load_deferred() synchronously. */
#define FALLBACK_FONT_LOAD_DELAY_MS 500
void fallback_font_schedule_deferred_load(void) {
    lv_timer_create(fallback_font_load_deferred, FALLBACK_FONT_LOAD_DELAY_MS, NULL);
}
