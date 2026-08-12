#ifndef FALLBACK_FONT_H
#define FALLBACK_FONT_H

#include "lvgl/lvgl.h"

/* Non-Latin (Cyrillic, Japanese kana/kanji, Korean Hangul, Thai) text-rendering
 * support -- see
 * fallback_font.c for the full story (what these are, why separate global
 * font instances instead of styling every label individually, and the
 * real-device boot-hang history behind the deferred-loading design).
 *
 * app_font_16/app_font_22/app_font_28 are real (non-const) lv_font_t
 * instances, usable anywhere a plain "&lv_font_montserrat_16/22/28" would
 * be -- LIST_ROW_FONT (screen_builders.h) and every label that can ever
 * show metadata-derived text (song title/artist, quick-drawer mini-card,
 * artist/album/genre drill-down screen titles, Subsonic browsing titles,
 * the text reader's filename title) are pointed at these instead, so their
 * non-Latin fallback comes for free without touching every individual
 * label style call site. Real-device feedback caught two rounds of gaps
 * here already -- first the drawer/now-playing labels relying on
 * LV_FONT_DEFAULT instead of an explicit style, then several 28px screen
 * titles that show dynamic (metadata) text but were still pointed at the
 * plain const lv_font_montserrat_28 -- so if a *new* screen ever displays
 * metadata-derived text, it needs one of these three, not a raw
 * lv_font_montserrat_* size, even if that size already exists here.
 *
 * Despite the fixed "16/22/28" names, the actual point size each one
 * resolves to depends on the Settings -> Font Size tier passed into
 * fallback_font_init_early() below -- gui.c's own ui_size_16/22/28 (the
 * OTHER, non-fallback-aware category, for fixed English UI chrome) move
 * through the exact same three tiers in lockstep, just via a separate
 * mapping local to gui.c. See that file's own comment for why these two
 * systems stay separate instead of sharing one pointer set. */
extern lv_font_t app_font_16;
extern lv_font_t app_font_22;
extern lv_font_t app_font_28;

/* Copies the three Montserrat fonts' own metrics into the globals above,
 * with .fallback left NULL (identical rendering to plain Montserrat until
 * fallback_font_load_deferred() below runs) -- and sizes them per
 * font_size_tier (0=Small/1=Medium/2=BlindMF, see settings.h): Small uses
 * the original 16/22/28, Medium 20/26/32, BlindMF 24/34/40. Call once,
 * before gui_init() builds any screen -- every label/style that captures
 * &app_font_16/&app_font_22/&app_font_28 at construction time keeps that
 * same address for the app's whole lifetime, so filling in .fallback later
 * is enough to make already-built screens pick up the change on their next
 * redraw. The tier passed here is also remembered for
 * fallback_font_schedule_deferred_load() below, so the CJK/Korean/Thai
 * fallback fonts it loads match whatever size Montserrat itself was just
 * set to, instead of being hardcoded back to the Small sizes. */
void fallback_font_init_early(int font_size_tier);

/* Schedules the actual (heavier, and historically the risky part) font
 * load -- opens and rasterizes-on-demand from assets/fonts/cjk_cyrillic.ttf
 * via LVGL's tiny_ttf renderer, then wires the result into app_font_16/
 * app_font_22/app_font_28's own .fallback field -- on a one-shot lv_timer
 * instead of running it directly. Call once, as the very last thing
 * gui_init() does (after the screen is already showing and any auto-resume
 * playback has already started) -- see fallback_font.c's own comment on
 * why this can't be reachable from anywhere earlier on the startup path. */
void fallback_font_schedule_deferred_load(void);

#endif /* FALLBACK_FONT_H */
