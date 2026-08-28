#ifndef FALLBACK_FONT_H
#define FALLBACK_FONT_H

#include "lvgl/lvgl.h"
#include "utf8_util.h"
#include <stdbool.h>
#include <stddef.h>

#define MAX_CUSTOM_FONTS_DISCOVERED 32
#define MAX_CUSTOM_FONT_FILE_SIZE (4 * 1024 * 1024) /* 4 MiB max for staging */

/* Stable global font handles usable anywhere a plain lv_font_t pointer is needed */
extern lv_font_t app_font_16;
extern lv_font_t app_font_20;
extern lv_font_t app_font_22;
extern lv_font_t app_font_28;
extern lv_font_t app_font_lyrics;

/* Initializes the font stack metrics early at startup (before screens are built) */
void fallback_font_init_early(int font_size_tier, int lyrics_font_size_tier);

/* Schedules deferred background loading of CJK/Korean/Thai fallbacks */
void fallback_font_schedule_deferred_load(void);

/* Forces immediate synchronous load/build of font stack (used in tests / live switch) */
void fallback_font_load_now(void);

/* Transactionally rebuilds only the four general-UI font handles for a
 * Settings -> Display -> Font Size tier.  The active custom face and, once
 * loaded, every multilingual fallback are required to succeed before the
 * handles are committed.  app_font_lyrics is deliberately left untouched. */
bool fallback_font_apply_size_tier(int tier);

/* Transactionally rebuilds only app_font_lyrics for a Settings -> Display
 * -> Lyrics Text Size tier (1 = Medium/32px, 2 = Large/40px). Mirrors
 * fallback_font_apply_size_tier()'s own shape but keyed the other way --
 * uses build_candidate_slot()'s reuse-aware primitive (same one
 * fallback_font_apply_custom() already uses) for all five font handles,
 * so the four general-UI handles (unchanged by this call) come back out
 * of the existing font registry at zero extra cost, and a lyrics size that
 * happens to coincide with the general Font Size tier's own 28px slot
 * (Medium = 32px, BlindMF = 40px) transparently reuses that same handle
 * instead of loading a duplicate -- no special-case overlap code needed,
 * the reuse lookup is keyed by (type, pixel size) regardless of which
 * slot asks first. */
bool fallback_font_apply_lyrics_size_tier(int tier);

/* Discovers custom TTF files under <SD>/Fonts */
int fallback_font_discover_custom(char out_names[][64], int max_count);

/* Stages, validates and applies a custom Latin TTF font (or "" / "Default"
 * for built-in Montserrat). Transactional: returns true on success; on
 * failure returns false and preserves the previous working font stack.
 * The caller owns the subsequent one-shot LVGL layout/invalidation pass. */
bool fallback_font_apply_custom(const char * custom_filename);

/* Called when an SD card mount is detected to retry staging/applying configured custom font */
void fallback_font_on_sd_mounted(void);

/* Validates whether a file is a supported TrueType outline font with printable Latin ASCII across all required sizes */
bool fallback_font_validate_file(const char * path);

/* Gets currently active custom font name ("Default" if built-in Montserrat) */
const char * fallback_font_get_custom_name(void);

/* Invalidates active screens, layers, lyrics, virtual lists, quick drawer, and transition snapshots */
void fallback_font_refresh_ui(void);

/* Safely truncates a UTF-8 string at a valid character boundary */
size_t utf8_truncate_safe(char * dst, const char * src, size_t max_bytes);

/* Validates and sanitizes a UTF-8 string, replacing invalid bytes with '?' */
void utf8_sanitize(char * str);

#endif /* FALLBACK_FONT_H */
