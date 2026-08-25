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

/* Discovers custom TTF files under <SD>/Fonts */
int fallback_font_discover_custom(char out_names[][64], int max_count);

/* Stages, validates and applies a custom Latin TTF font (or "" / "Default" for built-in Montserrat).
 * Transactional: returns true on success; on failure returns false and preserves previous working font stack. */
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
