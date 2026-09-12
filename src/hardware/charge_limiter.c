#include "charge_limiter.h"
#include "board_config.h"
#include "battery.h"
#include "debug_log.h"

#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define CHARGE_LIMITER_ACTIVE 1
#define CHARGE_LIMITER_REEVALUATE_SECONDS 5
#define CHARGE_RETRY_SECONDS 1
#define AXP_BUS "/dev/i2c-0"
#define AXP_ADDR 0x34
#define AXP_REG_MODULE_EN 0x18
#define AXP_CHG_EN (1u << 1)
#define AXP_REG_STATUS 0x01
#define AXP_REG_VOLTAGE 0x64
#define AXP_REG_CURRENT 0x62
#define AXP_CURRENT_MASK 0x1Fu
/* REG62H[4:0] (icc) encoding, confirmed against the real X-Powers AXP2101
 * datasheet (not assumed): N<=8 selects N*25mA (0-200mA), N>8 selects
 * 200+(N-8)*100mA (200-1500mA). 0x08 (8) is therefore 200mA, NOT 500mA --
 * a pre-existing mistake this file had carried forward uncorrected. 500mA
 * is N=11: 200 + (11-8)*100 = 500. */
#define AXP_CURRENT_CAP 0x0Bu

#define MP_BUS "/dev/i2c-0"
#define MP_ADDR 0x4b
#define MP_REG_CURRENT 0x05
#define MP_REG_VOLTAGE 0x07
#define MP_CURRENT_MASK 0x7Fu
#define MP_CURRENT_CAP 0x04u
#define MP_VOLTAGE_MASK 0xFEu

/* R3 Pro II's dedicated charger IC, MP2731 -- its own stock boot script
 * (module_driver/mp2731.sh) sets "vbat_target=4400000" (4.4V), matching the
 * AXP2101's own 4.4V target on this board. 0xC8 (0b11001000) is this
 * register's documented 4.4V encoding (3.4V offset + the bit weights this
 * file's own MP_VOLTAGE_MASK selects: 640mV + 320mV + 40mV = 1.0V on top of
 * the 3.4V offset). */
#define MP_VOLTAGE_BASELINE 0xC8u

#ifndef CHARGE_LIMITER_BASELINE_PATH
#define CHARGE_LIMITER_BASELINE_PATH "/usr/data/open_hiby_charge_baseline.txt"
#endif
/* Bumped from 1: the persisted file used to also carry a captured voltage
 * baseline (removed -- voltage is a known per-board constant now, nothing
 * to capture or persist). A stale v1 file is safely ignored/reset rather
 * than misparsed against the new, shorter format. */
#define BASELINE_VERSION 2

typedef struct { bool valid; uint8_t current; } chip_baseline_t;
typedef struct {
    bool loaded;
    chip_baseline_t axp;
#if HAS_MP2731
    chip_baseline_t mp;
#endif
} baseline_t;

static baseline_t baseline;
static bool voltage_limited;

static bool before(const struct timespec *a, const struct timespec *b) {
    return a->tv_sec < b->tv_sec || (a->tv_sec == b->tv_sec && a->tv_nsec < b->tv_nsec);
}
static struct timespec plus_seconds(struct timespec t, int n) { t.tv_sec += n; return t; }
static void retry_at(struct timespec *deadline) {
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    *deadline = plus_seconds(now, CHARGE_RETRY_SECONDS);
}

#if CHARGE_LIMITER_ACTIVE
static bool xfer(const char *bus, uint8_t addr, uint8_t reg, uint8_t *value, bool write) {
    int fd = open(bus, O_RDWR); if (fd < 0) return false;
    bool ok = false;
    if (ioctl(fd, I2C_SLAVE_FORCE, addr) >= 0) {
        union i2c_smbus_data data;
        if (write) data.byte = *value;
        struct i2c_smbus_ioctl_data args = {
            .read_write = write ? I2C_SMBUS_WRITE : I2C_SMBUS_READ,
            .command = reg, .size = I2C_SMBUS_BYTE_DATA, .data = &data
        };
        if (ioctl(fd, I2C_SMBUS, &args) >= 0) {
            if (!write) *value = data.byte;
            ok = true;
        }
    }
    close(fd); return ok;
}
static bool axp_read(uint8_t r, uint8_t *v) { return xfer(AXP_BUS, AXP_ADDR, r, v, false); }
static bool axp_write(uint8_t r, uint8_t v) { return xfer(AXP_BUS, AXP_ADDR, r, &v, true); }
#if HAS_MP2731
static bool mp_read(uint8_t r, uint8_t *v) { return xfer(MP_BUS, MP_ADDR, r, v, false); }
static bool mp_write(uint8_t r, uint8_t v) { return xfer(MP_BUS, MP_ADDR, r, &v, true); }
#endif

/* Only the charge-CURRENT registers need a captured baseline: unlike
 * voltage (see board_config.h's AXP_VOLTAGE_BASELINE and MP_VOLTAGE_BASELINE
 * above), neither
 * board's stock boot script pins a specific charge-current register value
 * -- it is whatever the chip's own factory/OTP default leaves it at, so
 * there is no known constant to hardcode and a live pre-cap read remains
 * the only source of truth. */
/* A missing, stale-version, or unparsable file is NOT a capture failure --
 * it just means there is no usable persisted baseline yet, exactly like a
 * fresh device. Always returns true; capture_baseline() falls through to a
 * live register read for whatever didn't load. Getting this wrong once
 * already happened: an earlier revision returned false on a version
 * mismatch, and capture_baseline() treated that as fatal forever (never
 * attempting a live read, so safe_charging_poll() could never apply or
 * restore the current cap for the rest of that boot) -- exactly the
 * failure mode BASELINE_VERSION's own bump from 1 to 2 would have
 * triggered on any device that had run the older format. */
static void load_baseline(void) {
    FILE *f = fopen(CHARGE_LIMITER_BASELINE_PATH, "r");
    if (!f) return;
    unsigned ver, ai;
    bool ok = fscanf(f, "version=%u axp_current=%u", &ver, &ai) == 2 &&
              ver == BASELINE_VERSION && ai <= AXP_CURRENT_MASK;
    if (ok) { baseline.axp.valid = true; baseline.axp.current = (uint8_t)ai; }
#if HAS_MP2731
    unsigned mi;
    ok = ok && fscanf(f, " mp_current=%u", &mi) == 1 && mi <= MP_CURRENT_MASK;
    if (ok) { baseline.mp.valid = true; baseline.mp.current = (uint8_t)mi; }
#endif
    fclose(f);
    if (!ok) { memset(&baseline, 0, sizeof(baseline)); unlink(CHARGE_LIMITER_BASELINE_PATH); }
}

static bool save_baseline(void) {
    char tmp[sizeof(CHARGE_LIMITER_BASELINE_PATH) + 32];
    snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", CHARGE_LIMITER_BASELINE_PATH, (long)getpid());
    FILE *f = fopen(tmp, "w"); if (!f) return false;
    fprintf(f, "version=%u axp_current=%u", BASELINE_VERSION, baseline.axp.current);
#if HAS_MP2731
    fprintf(f, " mp_current=%u", baseline.mp.current);
#endif
    fputc('\n', f);
    if (fflush(f) != 0 || fsync(fileno(f)) != 0 || fclose(f) != 0) { unlink(tmp); return false; }
    if (rename(tmp, CHARGE_LIMITER_BASELINE_PATH) != 0) { unlink(tmp); return false; }
    char path[sizeof(CHARGE_LIMITER_BASELINE_PATH)];
    strncpy(path, CHARGE_LIMITER_BASELINE_PATH, sizeof(path)); path[sizeof(path) - 1] = '\0';
    char *slash = strrchr(path, '/');
    if (slash) { *slash = '\0'; int fd = open(path, O_RDONLY | O_DIRECTORY); if (fd >= 0) { fsync(fd); close(fd); } }
    return true;
}

static bool capture_baseline(void) {
    if (baseline.loaded) return baseline.axp.valid;
    load_baseline();
    uint8_t v;
    if (!baseline.axp.valid) {
        if (!axp_read(AXP_REG_CURRENT, &v)) return false;
        baseline.axp.current = v & AXP_CURRENT_MASK; baseline.axp.valid = true;
    }
#if HAS_MP2731
    if (!baseline.mp.valid) {
        if (!mp_read(MP_REG_CURRENT, &v)) return false;
        baseline.mp.current = v & MP_CURRENT_MASK; baseline.mp.valid = true;
    }
#endif
    if (!save_baseline()) { memset(&baseline, 0, sizeof(baseline)); return false; }
    baseline.loaded = true; return true;
}

static bool set_axp_voltage(uint8_t target) {
    uint8_t v; if (!axp_read(AXP_REG_VOLTAGE, &v)) return false;
    if ((v & 7u) == target) return true;
    if (!axp_write(AXP_REG_VOLTAGE, target) || !axp_read(AXP_REG_VOLTAGE, &v)) return false;
    return (v & 7u) == target;
}
/* "Cap" never raises the current beyond target -- if some other setting
 * already left it lower than our cap, leave it alone rather than bumping
 * it up. "Exact" (restore) always converges to target regardless of
 * direction -- a captured baseline that was higher than the cap must be
 * writable back UP to that value, not just left at the still-capped one. */
static bool set_axp_current_capped(uint8_t target) {
    uint8_t v; if (!axp_read(AXP_REG_CURRENT, &v)) return false;
    if ((v & AXP_CURRENT_MASK) <= target) return true;
    uint8_t desired = (v & (uint8_t)~AXP_CURRENT_MASK) | target;
    if (!axp_write(AXP_REG_CURRENT, desired) || !axp_read(AXP_REG_CURRENT, &v)) return false;
    return (v & AXP_CURRENT_MASK) == target;
}
static bool set_axp_current_exact(uint8_t target) {
    uint8_t v; if (!axp_read(AXP_REG_CURRENT, &v)) return false;
    if ((v & AXP_CURRENT_MASK) == target) return true;
    uint8_t desired = (v & (uint8_t)~AXP_CURRENT_MASK) | target;
    if (!axp_write(AXP_REG_CURRENT, desired) || !axp_read(AXP_REG_CURRENT, &v)) return false;
    return (v & AXP_CURRENT_MASK) == target;
}
static bool enable_axp_charger(void) {
    uint8_t v; if (!axp_read(AXP_REG_MODULE_EN, &v)) return false;
    if (v & AXP_CHG_EN) return true;
    v |= AXP_CHG_EN;
    if (!axp_write(AXP_REG_MODULE_EN, v) || !axp_read(AXP_REG_MODULE_EN, &v)) return false;
    return (v & AXP_CHG_EN) != 0;
}
#if HAS_MP2731
static bool set_mp_voltage(uint8_t target) {
    uint8_t v; if (!mp_read(MP_REG_VOLTAGE, &v)) return false;
    uint8_t desired = (v & (uint8_t)~MP_VOLTAGE_MASK) | target;
    if ((v & MP_VOLTAGE_MASK) == target) return true;
    if (!mp_write(MP_REG_VOLTAGE, desired) || !mp_read(MP_REG_VOLTAGE, &v)) return false;
    return (v & MP_VOLTAGE_MASK) == target;
}
static bool set_mp_current_capped(uint8_t target) {
    uint8_t v; if (!mp_read(MP_REG_CURRENT, &v)) return false;
    if ((v & MP_CURRENT_MASK) <= target) return true;
    uint8_t desired = (v & (uint8_t)~MP_CURRENT_MASK) | target;
    if (!mp_write(MP_REG_CURRENT, desired) || !mp_read(MP_REG_CURRENT, &v)) return false;
    return (v & MP_CURRENT_MASK) == target;
}
static bool set_mp_current_exact(uint8_t target) {
    uint8_t v; if (!mp_read(MP_REG_CURRENT, &v)) return false;
    if ((v & MP_CURRENT_MASK) == target) return true;
    uint8_t desired = (v & (uint8_t)~MP_CURRENT_MASK) | target;
    if (!mp_write(MP_REG_CURRENT, desired) || !mp_read(MP_REG_CURRENT, &v)) return false;
    return (v & MP_CURRENT_MASK) == target;
}
#endif
#endif

void charge_limiter_poll(bool enabled, bool force) {
#if !CHARGE_LIMITER_ACTIVE
    (void)enabled; (void)force;
#else
    static struct timespec next_run, retry_deadline;
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    if (!force && next_run.tv_sec && before(&now, &next_run) && (retry_deadline.tv_sec == 0 || before(&now, &retry_deadline))) return;
    next_run = plus_seconds(now, CHARGE_LIMITER_REEVALUATE_SECONDS);
    /* No capture_baseline() call here -- voltage restoration uses the known
     * per-board constants above, not a captured value (current-only, see
     * capture_baseline()'s own comment). */
    bool ok;
    if (enabled) {
        ok = set_axp_voltage(3u); /* 4.2V cap */
#if HAS_MP2731
        ok = set_mp_voltage(0xA0u) && ok; /* MP2731's matching 4.2V encoding */
#endif
        if (ok) ok = enable_axp_charger();
        voltage_limited = ok;
    } else {
        ok = set_axp_voltage(AXP_VOLTAGE_BASELINE);
#if HAS_MP2731
        ok = set_mp_voltage(MP_VOLTAGE_BASELINE) && ok;
#endif
        if (ok) ok = enable_axp_charger();
        voltage_limited = false;
    }
    if (ok) retry_deadline.tv_sec = 0; else retry_at(&retry_deadline);
#endif
}

bool charge_limiter_is_holding(void) { return voltage_limited; }

bool charge_limiter_is_confirmed_off(void) {
#if CHARGE_LIMITER_ACTIVE
    if (!voltage_limited) return false;
#if HAS_MP2731
    /* On R3 Pro II, the MP2731 -- not the AXP2101 -- is the chip actually
     * driving the charge cycle (see battery.c's own comment on why its
     * "battery" status node is unreliable there for the same reason).
     * battery_is_full() already reads the MP2731-corrected status node, so
     * it is the primary, not a fallback: trusting AXP2101's own REG01 here
     * first risks a false "done" if the AXP2101's charge state machine
     * settles independently of what the MP2731 is actually doing. */
    return battery_is_full();
#else
    uint8_t status;
    return axp_read(AXP_REG_STATUS, &status) && (status & 7u) == 4;
#endif
#endif
    return false;
}

void safe_charging_poll(bool enabled, bool force) {
#if !CHARGE_LIMITER_ACTIVE
    (void)enabled; (void)force;
#else
    static struct timespec next_run, retry_deadline;
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    if (!force && next_run.tv_sec && before(&now, &next_run) && (retry_deadline.tv_sec == 0 || before(&now, &retry_deadline))) return;
    next_run = plus_seconds(now, CHARGE_LIMITER_REEVALUATE_SECONDS);
    if (!capture_baseline()) { retry_at(&retry_deadline); return; }
    bool ok = true;
    if (enabled) {
        ok = set_axp_current_capped(AXP_CURRENT_CAP);
#if HAS_MP2731
        ok = set_mp_current_capped(MP_CURRENT_CAP) && ok;
#endif
    } else {
        if (baseline.axp.current > AXP_CURRENT_CAP) ok = set_axp_current_exact(baseline.axp.current) && ok;
#if HAS_MP2731
        if (baseline.mp.current > MP_CURRENT_CAP) ok = set_mp_current_exact(baseline.mp.current) && ok;
#endif
    }
    if (ok) retry_deadline.tv_sec = 0; else retry_at(&retry_deadline);
#endif
}
