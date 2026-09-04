#ifndef PEQ_H
#define PEQ_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* Parametric EQ: PEQ_NUM_BANDS independent biquad filters in cascade, each
 * with adjustable center frequency, gain, Q, and filter type (peaking /
 * low shelf / high shelf) -- standard RBJ Audio EQ Cookbook coefficients
 * (shelf filters parameterized by Q the same way the Web Audio API's
 * BiquadFilterNode does, rather than the cookbook's alternate S/slope
 * parameter, so every band exposes the same freq/gain/Q trio regardless of
 * type). Applied to interleaved S16 stereo (or mono) PCM in the audio
 * output path, with independent filter state per channel so left/right
 * don't interfere with each other. Band layout (10 bands, ISO standard
 * center frequencies, first/last as shelves) matches a typical hardware
 * DAP's own PEQ screen. */

#define PEQ_NUM_BANDS 10

typedef enum {
    PEQ_TYPE_PEAKING = 0,
    PEQ_TYPE_LOW_SHELF,
    PEQ_TYPE_HIGH_SHELF,
} peq_band_type_t;

typedef struct {
    double freq_hz;
    double gain_db;
    double q;
    peq_band_type_t type;
    bool enabled;
} peq_band_t;

void peq_init(void);

bool peq_get_bypass(void);
void peq_set_bypass(bool bypass);

double peq_get_preamp_db(void);
void peq_set_preamp_db(double db);

const peq_band_t * peq_get_band(int index);
/* Recomputes that band's filter coefficients from the new params -- takes
 * effect on the next processed buffer, no audible click beyond the normal
 * filter response change since biquad state (not just coefficients) is
 * preserved. */
void peq_set_band(int index, double freq_hz, double gain_db, double q);
void peq_set_band_type(int index, peq_band_type_t type);
void peq_set_band_enabled(int index, bool enabled);

/* In-place processing of an interleaved S16 buffer. No-op (fast path) if
 * bypassed or no bands are enabled and preamp is 0dB. */
void peq_process(int16_t * buf, size_t frame_count, int channels, unsigned int sample_rate);

/* In-place processing of an interleaved S24_LE/S32 buffer (low 24 bits active).
 * Uses scaled limiter ceiling (~8386560) and 24-bit clamp bounds. */
void peq_process_s32(int32_t * buf, size_t frame_count, int channels, unsigned int sample_rate);

/* Persistence, same temp-file-then-rename pattern as settings.c -- this
 * pair always targets the one fixed, always-current PEQ_FILE_PATH (loaded
 * at startup, saved on every change), distinct from the named-profile
 * pair below. */
void peq_load(void);
void peq_save(void);

/* Named profiles: save/load the exact same file format to/from an
 * arbitrary path (an SD card location the user picks a name for), so a EQ
 * setup can be kept around, shared, or swapped for a different one without
 * losing whatever's currently active in PEQ_FILE_PATH. Both return false on
 * failure (path unwritable/unreadable) rather than silently doing nothing,
 * since these are direct user actions ("Save Profile"/"Load Profile") that
 * need to report success or failure back to the UI, unlike peq_load()/
 * peq_save()'s own fire-and-forget startup/autosave use. */
bool peq_save_to_path(const char * path);
bool peq_load_from_path(const char * path);

/* Every *.peq file directly inside root (not recursive -- see peq.c's own
 * comment on why), sorted alphabetically, for a "Load Profile" file list.
 * Caller owns *out_paths (free each entry, then the array). Returns false
 * if root can't be read or has no *.peq files in it. */
bool peq_scan_profiles(const char * root, char *** out_paths, int * out_count);

/* Restores every band, the preamp, and bypass to their original defaults
 * (the same values peq_init() starts from before any saved file is loaded --
 * see peq.c's own set_defaults()). A full factory reset of the current PEQ,
 * distinct from loading a named profile. Caller refreshes UI widgets and
 * calls peq_save() afterward itself, same division of responsibility as
 * peq_load_from_path(). */
void peq_reset_to_defaults(void);

#endif /* PEQ_H */
