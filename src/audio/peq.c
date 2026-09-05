#include "peq.h"

#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef HOST_BUILD
  #define PEQ_FILE_PATH "./open_hiby_player_peq.txt"
#else
  #define PEQ_FILE_PATH "/usr/data/open_hiby_player_peq.txt"
#endif

#define PEQ_TMP_FILE_PATH PEQ_FILE_PATH ".tmp"

#define PEQ_MAX_CHANNELS 2

/* Direct Form I biquad: y[n] = b0*x[n] + b1*x[n-1] + b2*x[n-2]
 *                              - a1*y[n-1] - a2*y[n-2]
 * (a0 already normalized to 1). float, not double -- this is the actual
 * per-sample hot path (peq_process()'s inner loop below, up to
 * PEQ_NUM_BANDS cascaded biquads per sample per channel, continuously for
 * the whole track whenever the user has any band enabled), and single-
 * precision float's ~144dB dynamic range is already far beyond 16-bit
 * audio's own ~96dB -- standard practice for audio biquad EQs (e.g. JUCE's
 * dsp module defaults to float for exactly this filter family), not a real
 * quality tradeoff on this content. compute_band_coeffs() below still does
 * its own setup math in double (pow/cos/sin, once per band per change, not
 * per sample) and only narrows to float in the one assignment into this
 * struct. */
typedef struct {
    float b0, b1, b2, a1, a2;
} biquad_coeffs_t;

typedef struct {
    float x1, x2, y1, y2;
} biquad_state_t;

static peq_band_t bands[PEQ_NUM_BANDS];
static biquad_coeffs_t coeffs[PEQ_NUM_BANDS];
static biquad_state_t state[PEQ_NUM_BANDS][PEQ_MAX_CHANNELS];
static bool last_call_was_s32 = false;
static bool bypass = false;
static double preamp_db = 0.0;
static unsigned int coeffs_sample_rate = 0;
static bool coeffs_dirty = true;

/* Real bug caught in a third review pass: preamp_linear was still
 * recomputed via pow() on every single peq_process() call, unlike the
 * limiter's own attack/release coefficients right below (which got the
 * cached-and-invalidated treatment in the previous pass). Cached here and
 * kept in sync by every one of preamp_db's own write sites (peq_set_
 * preamp_db(), set_defaults(), and peq_load_from_path()'s profile-loading
 * parse loop -- that last one writes preamp_db directly rather than through
 * the setter, which is exactly why an earlier pass left this one
 * uncached: a cache only keyed off "did the setter run" would go stale on
 * every profile load. update_preamp_linear_cache() is the one recompute
 * point every write site calls afterward instead, so there's no unguarded
 * write path left. */
static float cached_preamp_linear = 1.0f;

static void update_preamp_linear_cache(void) {
    cached_preamp_linear = (float) pow(10.0, preamp_db / 20.0);
}

/* Peak limiter -- stereo-linked (one shared gain-reduction envelope across
 * channels, rather than independent per-channel), so a loud transient
 * doesn't shift the stereo image the way independent per-channel gain
 * reduction would. Engages only when the preamp/EQ stage's own output would
 * exceed full scale -- unboosted material never pulls limiter_gain below
 * 1.0, so normal EQ use (cuts, or boosts that stay within headroom) sounds
 * completely unaffected. Real-device bug report: the "Loudness Boost"
 * example plugin (raises this same preamp via plugin.eq_set_preamp())
 * distorted audibly at anything above a few dB of boost -- a bare linear-
 * gain-then-hard-clip signal chain (peq_process()'s previous behavior)
 * clips almost any normally-mastered track well before +12dB, the plugin's
 * own max. This replaces that flat clamp with real gain reduction; the
 * int16 clamp still in peq_process() below is now just a belt-and-
 * suspenders safety net for the one sample a no-lookahead limiter can't
 * react in time for (e.g. a sudden transient's very first sample), not the
 * primary anti-clipping mechanism. */
static float limiter_gain = 1.0f; /* current linear gain reduction, 1.0 = none */

/* One-pole envelope-follower time constants -- attack fast enough to catch
 * a sustained loud passage within a few samples (avoiding an audible "over"
 * before the limiter reacts), release slow enough that gain recovers
 * smoothly after the loud passage ends instead of audibly "pumping" on
 * every transient. Standard limiter defaults; protects manual EQ band
 * boosts too, not just the preamp slider a plugin might drive. Ceiling sits
 * a hair under true full scale (32767) so the envelope's own smoothing lag
 * has a small margin before the belt-and-suspenders clamp would ever need
 * to actually bite in normal operation. */
#define LIMITER_ATTACK_SEC 0.001
#define LIMITER_RELEASE_SEC 0.050
#define LIMITER_CEILING 32760.0f
/* Scaled ceiling for 24-bit range (2^23 = 8388608 full-scale).
 * Maintains exact same headroom ratio (32760/32768 = 8386560/8388608 = 0.999755859375,
 * i.e. 32760 * 256 = 8386560.0f). */
#define LIMITER_CEILING_S32 8386560.0f

/* attack/release coefficients only actually depend on sample_rate (the two
 * time constants above are fixed), so they're computed alongside the biquad
 * coefficients in recompute_all_coeffs() below -- gated by the exact same
 * coeffs_dirty/coeffs_sample_rate check that function already correctly
 * maintains -- rather than via two fresh exp() calls on every single
 * peq_process() invocation regardless of whether anything actually changed
 * since the last one. Real-device concern caught in review: this is a
 * single-core MIPS target, and peq_process() runs on every decoded audio
 * buffer for as long as any EQ/preamp is active, not just once per track. */
static float cached_attack_coeff = 0.0f;
static float cached_release_coeff = 0.0f;

/* ISO-standard 10-band graphic EQ center frequencies -- matches the layout
 * of typical hardware DAP PEQ screens (band 0 a low shelf, band 9 a high
 * shelf, the rest peaking bells). */
static const double default_freqs[PEQ_NUM_BANDS] = {
    31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000
};

static void set_defaults(void) {
    for (int i = 0; i < PEQ_NUM_BANDS; i++) {
        bands[i].freq_hz = default_freqs[i];
        bands[i].gain_db = 0.0;
        bands[i].q = (i == 0 || i == PEQ_NUM_BANDS - 1) ? 0.2 : 0.7;
        bands[i].type = (i == 0) ? PEQ_TYPE_LOW_SHELF
                       : (i == PEQ_NUM_BANDS - 1) ? PEQ_TYPE_HIGH_SHELF
                       : PEQ_TYPE_PEAKING;
        bands[i].enabled = false;
    }
    bypass = false;
    preamp_db = 0.0;
    update_preamp_linear_cache();
}

static void compute_band_coeffs(int index, unsigned int sample_rate) {
    const peq_band_t * band = &bands[index];
    double freq = band->freq_hz;
    /* Clamp so w0 stays comfortably inside (0, pi) regardless of what the
     * current track's sample rate is (e.g. DSD's decimated 352.8kHz output,
     * where a 16kHz band is nowhere near Nyquist, vs a hypothetical very
     * low sample rate where it could be). */
    double nyquist = (double) sample_rate / 2.0;
    if (freq > nyquist * 0.99) freq = nyquist * 0.99;
    if (freq < 1.0) freq = 1.0;

    double A = pow(10.0, band->gain_db / 40.0);
    double w0 = 2.0 * M_PI * freq / (double) sample_rate;
    double q = band->q > 0.01 ? band->q : 0.01;
    double cos_w0 = cos(w0);
    double sin_w0 = sin(w0);

    double b0, b1, b2, a0, a1, a2;

    if (band->type == PEQ_TYPE_LOW_SHELF || band->type == PEQ_TYPE_HIGH_SHELF) {
        /* Shelf filters parameterized directly by Q (same substitution the
         * Web Audio API's BiquadFilterNode uses for lowshelf/highshelf)
         * rather than the RBJ cookbook's alternate S/slope parameter, so
         * every band type shares the same Q control in the UI. */
        /* Real-device bug report: enabling the high-shelf band (or the
         * low-shelf band -- both use this same formula) with a boosted/cut
         * gain and a high enough Q muted the entire device, not just this
         * band, surviving even a disable until a full settings reset.
         * Root cause: this term goes negative for any gain != 0dB once Q is
         * large enough (e.g. +12dB with Q >= ~5, both reachable on the real
         * gain -12..+12dB / Q 0.1..10.0 sliders) -- sqrt() of a negative
         * number is NaN, and NaN in one band's biquad state poisons every
         * later band in the cascade and the final output permanently until
         * that state is reset, matching "disabling didn't visibly help
         * until a broader reset." Clamped to 0 (the RBJ cookbook shelf
         * formula's own well-defined floor -- alpha simply saturates
         * rather than going complex) rather than restricting the Q/gain
         * sliders themselves, so no currently-valid slider combination
         * changes behavior, only the ones that were already silently
         * broken. */
        double under_sqrt = (A + 1.0 / A) * (1.0 / q - 1.0) + 2.0;
        if (under_sqrt < 0.0) under_sqrt = 0.0;
        double alpha = (sin_w0 / 2.0) * sqrt(under_sqrt);
        double sqrt_A_2alpha = 2.0 * sqrt(A) * alpha;

        if (band->type == PEQ_TYPE_LOW_SHELF) {
            b0 = A * ((A + 1) - (A - 1) * cos_w0 + sqrt_A_2alpha);
            b1 = 2 * A * ((A - 1) - (A + 1) * cos_w0);
            b2 = A * ((A + 1) - (A - 1) * cos_w0 - sqrt_A_2alpha);
            a0 = (A + 1) + (A - 1) * cos_w0 + sqrt_A_2alpha;
            a1 = -2 * ((A - 1) + (A + 1) * cos_w0);
            a2 = (A + 1) + (A - 1) * cos_w0 - sqrt_A_2alpha;
        } else {
            b0 = A * ((A + 1) + (A - 1) * cos_w0 + sqrt_A_2alpha);
            b1 = -2 * A * ((A - 1) + (A + 1) * cos_w0);
            b2 = A * ((A + 1) + (A - 1) * cos_w0 - sqrt_A_2alpha);
            a0 = (A + 1) - (A - 1) * cos_w0 + sqrt_A_2alpha;
            a1 = 2 * ((A - 1) - (A + 1) * cos_w0);
            a2 = (A + 1) - (A - 1) * cos_w0 - sqrt_A_2alpha;
        }
    } else {
        /* Peaking (bell) */
        double alpha = sin_w0 / (2.0 * q);
        b0 = 1.0 + alpha * A;
        b1 = -2.0 * cos_w0;
        b2 = 1.0 - alpha * A;
        a0 = 1.0 + alpha / A;
        a1 = -2.0 * cos_w0;
        a2 = 1.0 - alpha / A;
    }

    coeffs[index].b0 = b0 / a0;
    coeffs[index].b1 = b1 / a0;
    coeffs[index].b2 = b2 / a0;
    coeffs[index].a1 = a1 / a0;
    coeffs[index].a2 = a2 / a0;
}

static void recompute_all_coeffs(unsigned int sample_rate) {
    for (int i = 0; i < PEQ_NUM_BANDS; i++) compute_band_coeffs(i, sample_rate);
    cached_attack_coeff = (float) exp(-1.0 / (LIMITER_ATTACK_SEC * (double) sample_rate));
    cached_release_coeff = (float) exp(-1.0 / (LIMITER_RELEASE_SEC * (double) sample_rate));
    coeffs_sample_rate = sample_rate;
    coeffs_dirty = false;
}

void peq_init(void) {
    set_defaults();
    memset(state, 0, sizeof(state));
    peq_load();
}

void peq_reset_to_defaults(void) {
    set_defaults();
    coeffs_dirty = true;
}

bool peq_get_bypass(void) { return bypass; }
void peq_set_bypass(bool b) { bypass = b; }

double peq_get_preamp_db(void) { return preamp_db; }
void peq_set_preamp_db(double db) {
    preamp_db = db;
    update_preamp_linear_cache();
}

const peq_band_t * peq_get_band(int index) {
    if (index < 0 || index >= PEQ_NUM_BANDS) return NULL;
    return &bands[index];
}

void peq_set_band(int index, double freq_hz, double gain_db, double q) {
    if (index < 0 || index >= PEQ_NUM_BANDS) return;
    bands[index].freq_hz = freq_hz;
    bands[index].gain_db = gain_db;
    bands[index].q = q;
    coeffs_dirty = true;
}

void peq_set_band_type(int index, peq_band_type_t type) {
    if (index < 0 || index >= PEQ_NUM_BANDS) return;
    bands[index].type = type;
    coeffs_dirty = true;
}

void peq_set_band_enabled(int index, bool enabled) {
    if (index < 0 || index >= PEQ_NUM_BANDS) return;
    bands[index].enabled = enabled;
}

void peq_process(int16_t * buf, size_t frame_count, int channels, unsigned int sample_rate) {
    /* Real bug caught in review: limiter_gain is continuous smoothing state
     * (same idea as the biquad state[] below), but neither of the two
     * inactivity paths reset it -- so a limiter that was mid-gain-reduction
     * (e.g. from a loud, heavily-boosted track) stayed frozen at that
     * reduced value across a bypass toggle or a preamp/all-bands-off period
     * with NO chance to recover toward unity (this function returns before
     * ever reaching the envelope-follower code below). Re-enabling
     * processing later would then start audibly quieter than intended until
     * the release envelope caught back up. Resetting here, at every path
     * that skips real processing, means the limiter always starts fresh
     * from unity the next time it's actually needed, regardless of what
     * state it was left in before. */
    if (bypass) {
        limiter_gain = 1.0f;
        return;
    }

    bool any_enabled = false;
    for (int i = 0; i < PEQ_NUM_BANDS; i++) {
        if (bands[i].enabled) { any_enabled = true; break; }
    }
    if (!any_enabled && preamp_db == 0.0) {
        limiter_gain = 1.0f;
        return;
    }

    if (last_call_was_s32) {
        memset(state, 0, sizeof(state));
        last_call_was_s32 = false;
    }

    if (coeffs_dirty || sample_rate != coeffs_sample_rate) recompute_all_coeffs(sample_rate);

    if (channels < 1) return;
    if (channels > PEQ_MAX_CHANNELS) channels = PEQ_MAX_CHANNELS;

    /* Cached (update_preamp_linear_cache(), kept in sync by every one of
     * preamp_db's own write sites -- see that function's own comment for
     * why an earlier pass left this uncached and why that reasoning didn't
     * hold up). float, not double, for the same reason biquad_coeffs_t/
     * biquad_state_t are float -- see their own comment. */
    float preamp_linear = cached_preamp_linear;

    float attack_coeff = cached_attack_coeff;
    float release_coeff = cached_release_coeff;

    for (size_t i = 0; i < frame_count; i++) {
        float frame_samples[PEQ_MAX_CHANNELS];
        float frame_peak = 0.0f;

        for (int ch = 0; ch < channels; ch++) {
            float sample = (float) buf[i * (size_t) channels + (size_t) ch] * preamp_linear;

            for (int band = 0; band < PEQ_NUM_BANDS; band++) {
                if (!bands[band].enabled) continue;

                biquad_coeffs_t * c = &coeffs[band];
                biquad_state_t * s = &state[band][ch];

                float x0 = sample;
                float y0 = c->b0 * x0 + c->b1 * s->x1 + c->b2 * s->x2
                         - c->a1 * s->y1 - c->a2 * s->y2;

                s->x2 = s->x1;
                s->x1 = x0;
                s->y2 = s->y1;
                s->y1 = y0;

                sample = y0;
            }

            /* NaN fails every comparison (including the clamps below), so
             * it would otherwise poison frame_peak/limiter_gain permanently
             * (both just track whatever's handed to them, with no clamp of
             * their own) -- real-device bug this session already root-
             * caused once for the shelf-filter sqrt() above: a bad
             * coefficient produced NaN that stuck around as a full, sticky
             * device mute. Caught here, before it can spread. */
            if (!isfinite(sample)) sample = 0.0f;
            frame_samples[ch] = sample;
            float abs_sample = sample < 0.0f ? -sample : sample;
            if (abs_sample > frame_peak) frame_peak = abs_sample;
        }

        /* Stereo-linked gain reduction: both channels of this frame share
         * one envelope, driven by whichever channel is louder, so a hard-
         * panned transient doesn't tug the gain (and therefore the
         * perceived stereo image) unevenly between L/R. */
        float desired_gain = frame_peak > LIMITER_CEILING ? LIMITER_CEILING / frame_peak : 1.0f;
        float coeff = desired_gain < limiter_gain ? attack_coeff : release_coeff;
        limiter_gain = coeff * limiter_gain + (1.0f - coeff) * desired_gain;

        for (int ch = 0; ch < channels; ch++) {
            float sample = frame_samples[ch] * limiter_gain;
            /* Belt-and-suspenders, not the primary anti-clipping mechanism
             * anymore -- see limiter_gain's own comment. A no-lookahead
             * limiter can't fully react to a single-sample transient before
             * it happens, so this is the real last line of defense for
             * that one edge case. */
            if (sample > 32767.0f) sample = 32767.0f;
            if (sample < -32768.0f) sample = -32768.0f;
            buf[i * (size_t) channels + (size_t) ch] = (int16_t) sample;
        }
    }
}

void peq_process_s32(int32_t * buf, size_t frame_count, int channels, unsigned int sample_rate) {
    if (bypass) {
        limiter_gain = 1.0f;
        return;
    }

    bool any_enabled = false;
    for (int i = 0; i < PEQ_NUM_BANDS; i++) {
        if (bands[i].enabled) { any_enabled = true; break; }
    }
    if (!any_enabled && preamp_db == 0.0) {
        limiter_gain = 1.0f;
        return;
    }

    if (!last_call_was_s32) {
        memset(state, 0, sizeof(state));
        last_call_was_s32 = true;
    }

    if (coeffs_dirty || sample_rate != coeffs_sample_rate) recompute_all_coeffs(sample_rate);

    if (channels < 1) return;
    if (channels > PEQ_MAX_CHANNELS) channels = PEQ_MAX_CHANNELS;

    float preamp_linear = cached_preamp_linear;
    float attack_coeff = cached_attack_coeff;
    float release_coeff = cached_release_coeff;

    for (size_t i = 0; i < frame_count; i++) {
        float frame_samples[PEQ_MAX_CHANNELS];
        float frame_peak = 0.0f;

        for (int ch = 0; ch < channels; ch++) {
            float sample = (float) buf[i * (size_t) channels + (size_t) ch] * preamp_linear;

            for (int band = 0; band < PEQ_NUM_BANDS; band++) {
                if (!bands[band].enabled) continue;

                biquad_coeffs_t * c = &coeffs[band];
                biquad_state_t * s = &state[band][ch];

                float x0 = sample;
                float y0 = c->b0 * x0 + c->b1 * s->x1 + c->b2 * s->x2
                         - c->a1 * s->y1 - c->a2 * s->y2;

                s->x2 = s->x1;
                s->x1 = x0;
                s->y2 = s->y1;
                s->y1 = y0;

                sample = y0;
            }

            if (!isfinite(sample)) sample = 0.0f;
            frame_samples[ch] = sample;
            float abs_sample = sample < 0.0f ? -sample : sample;
            if (abs_sample > frame_peak) frame_peak = abs_sample;
        }

        float desired_gain = frame_peak > LIMITER_CEILING_S32 ? LIMITER_CEILING_S32 / frame_peak : 1.0f;
        float coeff = desired_gain < limiter_gain ? attack_coeff : release_coeff;
        limiter_gain = coeff * limiter_gain + (1.0f - coeff) * desired_gain;

        for (int ch = 0; ch < channels; ch++) {
            float sample = frame_samples[ch] * limiter_gain;
            if (sample > 8388607.0f) sample = 8388607.0f;
            if (sample < -8388608.0f) sample = -8388608.0f;
            buf[i * (size_t) channels + (size_t) ch] = (int32_t) sample;
        }
    }
}

bool peq_load_from_path(const char * path) {
    FILE * f = fopen(path, "r");
    if (!f) return false;

    char line[128];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = '\0';

        char * eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        const char * key = line;
        const char * value = eq + 1;

        if (strcmp(key, "bypass") == 0) {
            bypass = (strcmp(value, "1") == 0);
            continue;
        }
        if (strcmp(key, "preamp") == 0) {
            preamp_db = atof(value);
            update_preamp_linear_cache();
            continue;
        }

        int index;
        char field[16];
        if (sscanf(key, "band%d_%15s", &index, field) == 2 && index >= 0 && index < PEQ_NUM_BANDS) {
            if (strcmp(field, "freq") == 0) bands[index].freq_hz = atof(value);
            else if (strcmp(field, "gain") == 0) bands[index].gain_db = atof(value);
            else if (strcmp(field, "q") == 0) bands[index].q = atof(value);
            else if (strcmp(field, "type") == 0) bands[index].type = (peq_band_type_t) atoi(value);
            else if (strcmp(field, "enabled") == 0) bands[index].enabled = (strcmp(value, "1") == 0);
        }
    }

    fclose(f);
    coeffs_dirty = true;
    return true;
}

bool peq_save_to_path(const char * path) {
    char tmp_path[520];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

    FILE * f = fopen(tmp_path, "w");
    if (!f) return false;

    fprintf(f, "bypass=%d\n", bypass ? 1 : 0);
    fprintf(f, "preamp=%.2f\n", preamp_db);
    for (int i = 0; i < PEQ_NUM_BANDS; i++) {
        fprintf(f, "band%d_freq=%.2f\n", i, bands[i].freq_hz);
        fprintf(f, "band%d_gain=%.2f\n", i, bands[i].gain_db);
        fprintf(f, "band%d_q=%.3f\n", i, bands[i].q);
        fprintf(f, "band%d_type=%d\n", i, (int) bands[i].type);
        fprintf(f, "band%d_enabled=%d\n", i, bands[i].enabled ? 1 : 0);
    }

    /* Do not report success until stdio and the underlying SD-card write
     * have both completed. fclose() errors (card removed/full/read-only)
     * were previously ignored and could produce a misleading "saved"
     * toast even though the temp file was incomplete. */
    bool write_ok = !ferror(f) && fflush(f) == 0 && fsync(fileno(f)) == 0;
    if (fclose(f) != 0) write_ok = false;
    if (!write_ok) {
        fprintf(stderr, "peq: failed writing '%s': %s\n", tmp_path, strerror(errno));
        unlink(tmp_path);
        return false;
    }

    /* rename(temp, target) is atomic and replaces target on normal POSIX
     * filesystems, so keep that as the fast path. Some FAT/VFAT firmware
     * combinations reject replacement when the destination already exists,
     * which made new PEQ profiles save correctly but prevented overwriting
     * profiles already on the SD card. Fall back to a recoverable two-step
     * replacement: move the existing profile aside, install the completed
     * temp file, then remove the backup. If installation fails, restore the
     * original so an attempted overwrite never destroys the user's preset. */
    if (rename(tmp_path, path) == 0) return true;
    int direct_rename_errno = errno;
    if (access(path, F_OK) != 0) {
        fprintf(stderr, "peq: rename '%s' -> '%s' failed: %s\n", tmp_path, path, strerror(direct_rename_errno));
        unlink(tmp_path);
        return false;
    }

    char backup_path[520];
    snprintf(backup_path, sizeof(backup_path), "%s.bak", path);
    unlink(backup_path); /* stale backup from an interrupted older attempt */
    if (rename(path, backup_path) != 0) {
        fprintf(stderr, "peq: could not stage existing profile '%s': %s\n", path, strerror(errno));
        unlink(tmp_path);
        return false;
    }
    if (rename(tmp_path, path) != 0) {
        int install_errno = errno;
        if (rename(backup_path, path) != 0)
            fprintf(stderr, "peq: CRITICAL: rollback '%s' failed: %s\n", path, strerror(errno));
        fprintf(stderr, "peq: could not install replacement '%s': %s\n", path, strerror(install_errno));
        unlink(tmp_path);
        return false;
    }
    unlink(backup_path);
    return true;
}

void peq_load(void) {
    peq_load_from_path(PEQ_FILE_PATH);
}

void peq_save(void) {
    peq_save_to_path(PEQ_FILE_PATH);
}

static bool is_peq_file(const char * name) {
    const char * ext = strrchr(name, '.');
    return ext && strcmp(ext, ".peq") == 0;
}

static int compare_paths(const void * a, const void * b) {
    const char * const * pa = (const char * const *) a;
    const char * const * pb = (const char * const *) b;
    return strcmp(*pa, *pb);
}

/* Flat (non-recursive) scan, unlike text_reader.c's recursive one -- named
 * profiles are meant to sit directly in one dedicated folder the user
 * manages, not scattered across the whole SD card. Caller owns *out_paths
 * (free each entry, then the array). Returns false if root can't be read
 * (including "doesn't exist yet" -- see peq_profiles_dir_ensure()) or has
 * no .peq files. */
bool peq_scan_profiles(const char * root, char *** out_paths, int * out_count) {
    DIR * dir = opendir(root);
    if (!dir) return false;

    char ** paths = NULL;
    int count = 0;
    int capacity = 0;

    struct dirent * de;
    while ((de = readdir(dir)) != NULL) {
        if (de->d_name[0] == '.') continue;
        if (!is_peq_file(de->d_name)) continue;

        char full_path[512];
        snprintf(full_path, sizeof(full_path), "%s/%s", root, de->d_name);

        struct stat st;
        if (stat(full_path, &st) != 0 || !S_ISREG(st.st_mode)) continue;

        if (count == capacity) {
            capacity = capacity ? capacity * 2 : 16;
            paths = realloc(paths, sizeof(char *) * (size_t) capacity);
        }
        paths[count] = strdup(full_path);
        count++;
    }
    closedir(dir);

    if (count == 0) {
        free(paths);
        return false;
    }

    qsort(paths, (size_t) count, sizeof(char *), compare_paths);
    *out_paths = paths;
    *out_count = count;
    return true;
}
