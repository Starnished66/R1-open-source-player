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
 * (a0 already normalized to 1). */
typedef struct {
    double b0, b1, b2, a1, a2;
} biquad_coeffs_t;

typedef struct {
    double x1, x2, y1, y2;
} biquad_state_t;

static peq_band_t bands[PEQ_NUM_BANDS];
static biquad_coeffs_t coeffs[PEQ_NUM_BANDS];
static biquad_state_t state[PEQ_NUM_BANDS][PEQ_MAX_CHANNELS];
static bool bypass = false;
static double preamp_db = 0.0;
static unsigned int coeffs_sample_rate = 0;
static bool coeffs_dirty = true;

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
        double alpha = (sin_w0 / 2.0) * sqrt((A + 1.0 / A) * (1.0 / q - 1.0) + 2.0);
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
void peq_set_preamp_db(double db) { preamp_db = db; }

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
    if (bypass) return;

    bool any_enabled = false;
    for (int i = 0; i < PEQ_NUM_BANDS; i++) {
        if (bands[i].enabled) { any_enabled = true; break; }
    }
    if (!any_enabled && preamp_db == 0.0) return;

    if (coeffs_dirty || sample_rate != coeffs_sample_rate) recompute_all_coeffs(sample_rate);

    if (channels < 1) return;
    if (channels > PEQ_MAX_CHANNELS) channels = PEQ_MAX_CHANNELS;

    double preamp_linear = pow(10.0, preamp_db / 20.0);

    for (size_t i = 0; i < frame_count; i++) {
        for (int ch = 0; ch < channels; ch++) {
            double sample = (double) buf[i * (size_t) channels + (size_t) ch] * preamp_linear;

            for (int band = 0; band < PEQ_NUM_BANDS; band++) {
                if (!bands[band].enabled) continue;

                biquad_coeffs_t * c = &coeffs[band];
                biquad_state_t * s = &state[band][ch];

                double x0 = sample;
                double y0 = c->b0 * x0 + c->b1 * s->x1 + c->b2 * s->x2
                          - c->a1 * s->y1 - c->a2 * s->y2;

                s->x2 = s->x1;
                s->x1 = x0;
                s->y2 = s->y1;
                s->y1 = y0;

                sample = y0;
            }

            if (sample > 32767.0) sample = 32767.0;
            if (sample < -32768.0) sample = -32768.0;
            buf[i * (size_t) channels + (size_t) ch] = (int16_t) sample;
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
