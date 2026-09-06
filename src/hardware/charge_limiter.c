#include "charge_limiter.h"
#include "battery.h"
#include "debug_log.h"

#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

/* CHARGE_LIMITER_ACTIVE gates the AXP2101 i2c transactions.
 * Set to 0 to disable entirely; the limiter is independent of
 * battery.c's status polling. */
#define CHARGE_LIMITER_ACTIVE 1

#define AXP2101_I2C_BUS "/dev/i2c-0"
#define AXP2101_I2C_ADDR 0x34

/* module_en, X-Powers AXP2101 datasheet register map section 8.2 (addr
 * 0x18) -- bit 1 (chg_en) is the real charger master enable/disable
 * ("Cell Battery charge enable"), distinct from bit 3 (gauge_en, the fuel
 * gauge -- must be preserved, never touched here) and bit 0 (watchdog_en).
 * Always read-modify-write this register, never a blind write of just the
 * one bit's value -- a naive write would silently disable the fuel gauge
 * and/or watchdog too. */
#define AXP2101_REG_MODULE_EN 0x18
#define AXP2101_MODULE_EN_CHG_BIT (1u << 1)

/* comm_stat1, addr 0x01, bits[2:0] -- charging status straight from the
 * PMIC (000 tri-charge, 001 pre-charge, 010 constant-current, 011
 * constant-voltage, 100 charge done, 101 not charging). Read-only,
 * DBG_LOG'd around every throttle/restore call as the one live,
 * trustworthy way to confirm the write actually took effect -- see this
 * file's own history below for why nothing else here can be trusted at
 * face value. */
#define AXP2101_REG_CHG_STAT 0x01

/* ICC_CFG (0x62), bits [4:0]. Code 0x08 selects 500mA according to the
 * AXP2101 register table. Preserve the upper bits with a read-modify-write. */
#define AXP2101_REG_CHG_CURRENT 0x62
#define AXP2101_CHG_CURRENT_MASK 0x1Fu
#define AXP2101_CHG_CURRENT_500MA 0x08u

/* Disabling chg_en in REG_MODULE_EN turns off the charger state machine
 * across both constant-current and constant-voltage phases. */

#define CHARGE_LIMITER_STOP_PERCENT 85
#define CHARGE_LIMITER_TRIGGER_PERCENT 84
#define CHARGE_LIMITER_RESUME_PERCENT 82

/* Re-checks and re-applies at most this often rather than on every GUI tick.
 * Periodic re-application ensures any state changes (such as adapter replug)
 * are caught and addressed. */
#define CHARGE_LIMITER_REEVALUATE_SECONDS 5

/* Exported read-only UI state -- the desired hold, decided from the percent
 * thresholds before hardware writes are attempted. Used for intent checks
 * (e.g. idle-shutdown eligibility). Only GUI callbacks call this module, so no
 * cross-thread synchronization is needed. */
static bool limiter_holding = false;

/* True only once register readback confirms charging is disabled. Used by
 * LED indicators to avoid transient false positives during retries. */
static bool charger_confirmed_off = false;

#if CHARGE_LIMITER_ACTIVE
static bool axp2101_smbus_xfer(uint8_t reg, uint8_t * value, bool write) {
    int fd = open(AXP2101_I2C_BUS, O_RDWR);
    if (fd < 0) return false;

    bool ok = false;
    if (ioctl(fd, I2C_SLAVE_FORCE, AXP2101_I2C_ADDR) >= 0) {
        union i2c_smbus_data data;
        if (write) data.byte = *value;

        struct i2c_smbus_ioctl_data args = {
            .read_write = write ? I2C_SMBUS_WRITE : I2C_SMBUS_READ,
            .command = reg,
            .size = I2C_SMBUS_BYTE_DATA,
            .data = &data,
        };
        if (ioctl(fd, I2C_SMBUS, &args) >= 0) {
            if (!write) *value = data.byte;
            ok = true;
        }
    }
    close(fd);
    return ok;
}

static bool axp2101_read_reg(uint8_t reg, uint8_t * out) {
    return axp2101_smbus_xfer(reg, out, false);
}

static bool axp2101_write_reg(uint8_t reg, uint8_t value) {
    return axp2101_smbus_xfer(reg, &value, true);
}

static void log_chg_stat(const char * when) {
    uint8_t stat;
    if (!axp2101_read_reg(AXP2101_REG_CHG_STAT, &stat)) return;
    static const char * const names[8] = {
        "tri_charge", "pre_charge", "constant_current", "constant_voltage",
        "charge_done", "not_charging", "reserved", "reserved",
    };
    DBG_LOG("charge_limiter: chg_stat %s = %s (0x%02X)\n", when, names[stat & 0x07], stat & 0x07);
}

static bool set_charging_enabled(bool enabled) {
    uint8_t module_en;
    if (!axp2101_read_reg(AXP2101_REG_MODULE_EN, &module_en)) return false;

    uint8_t desired = enabled ? (module_en | AXP2101_MODULE_EN_CHG_BIT)
                              : (module_en & (uint8_t) ~AXP2101_MODULE_EN_CHG_BIT);
    if (desired != module_en && !axp2101_write_reg(AXP2101_REG_MODULE_EN, desired)) return false;

    uint8_t readback;
    if (!axp2101_read_reg(AXP2101_REG_MODULE_EN, &readback)) return false;
    bool confirmed = (readback & AXP2101_MODULE_EN_CHG_BIT) ==
                     (enabled ? AXP2101_MODULE_EN_CHG_BIT : 0);
    DBG_LOG("charge_limiter: charger %s readback reg18=0x%02X -> %s\n",
            enabled ? "enable" : "disable", readback, confirmed ? "confirmed" : "FAILED");
    log_chg_stat(enabled ? "after restore" : "after throttle");
    return confirmed;
}

static bool disable_charging(void) { return set_charging_enabled(false); }
static bool enable_charging(void) { return set_charging_enabled(true); }
#endif

void charge_limiter_poll(bool enabled, bool force) {
#if !CHARGE_LIMITER_ACTIVE
    (void) enabled;
    (void) force;
    return;
#else
    static struct timespec last_apply;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (!force && last_apply.tv_sec != 0 && now.tv_sec - last_apply.tv_sec < CHARGE_LIMITER_REEVALUATE_SECONDS) return;
    last_apply = now;

    if (!enabled) {
        if (enable_charging()) {
            limiter_holding = false;
            charger_confirmed_off = false;
        } else {
            last_apply.tv_sec -= CHARGE_LIMITER_REEVALUATE_SECONDS - 1; /* retry in ~1s */
        }
        return;
    }

    int percent = battery_get_percent();
    if (percent < 0) return; /* no battery data (e.g. host build) -- nothing to act on */

    /* Log battery percent and threshold state. */
    DBG_LOG("charge_limiter: poll percent=%d (target=%d trigger=%d resume=%d holding=%d)\n",
            percent, CHARGE_LIMITER_STOP_PERCENT, CHARGE_LIMITER_TRIGGER_PERCENT,
            CHARGE_LIMITER_RESUME_PERCENT, limiter_holding);

    /* Trigger one percentage point early to absorb gauge lag, with hysteresis
     * to avoid repeatedly cycling charging on noisy readings. */
    if (percent >= CHARGE_LIMITER_TRIGGER_PERCENT) limiter_holding = true;
    else if (percent <= CHARGE_LIMITER_RESUME_PERCENT) limiter_holding = false;

    bool applied = limiter_holding ? disable_charging() : enable_charging();
    if (applied) {
        /* Update confirmed state only upon register confirmation. */
        charger_confirmed_off = limiter_holding;
    } else {
        last_apply.tv_sec -= CHARGE_LIMITER_REEVALUATE_SECONDS - 1; /* retry in ~1s */
    }
#endif
}

bool charge_limiter_is_holding(void) {
    return limiter_holding;
}

bool charge_limiter_is_confirmed_off(void) {
    return charger_confirmed_off;
}

void safe_charging_poll(bool enabled, bool force) {
#if !CHARGE_LIMITER_ACTIVE
    (void) enabled;
    (void) force;
#else
    static struct timespec last_apply;
    if (!enabled) return; /* Off means leave the PMIC unchanged. */

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (!force && last_apply.tv_sec != 0 &&
        now.tv_sec - last_apply.tv_sec < CHARGE_LIMITER_REEVALUATE_SECONDS) return;
    last_apply = now;

    uint8_t current;
    if (!axp2101_read_reg(AXP2101_REG_CHG_CURRENT, &current)) {
        last_apply.tv_sec -= CHARGE_LIMITER_REEVALUATE_SECONDS - 1;
        return;
    }
    uint8_t desired = (current & (uint8_t) ~AXP2101_CHG_CURRENT_MASK) |
                      AXP2101_CHG_CURRENT_500MA;
    if (desired != current && !axp2101_write_reg(AXP2101_REG_CHG_CURRENT, desired)) {
        last_apply.tv_sec -= CHARGE_LIMITER_REEVALUATE_SECONDS - 1;
        return;
    }

    uint8_t readback = 0;
    bool confirmed = axp2101_read_reg(AXP2101_REG_CHG_CURRENT, &readback) &&
                     (readback & AXP2101_CHG_CURRENT_MASK) == AXP2101_CHG_CURRENT_500MA;
    DBG_LOG("safe_charging: 500mA cap reg62 0x%02X -> 0x%02X (%s)\n",
            current, readback, confirmed ? "confirmed" : "FAILED");
    if (!confirmed) last_apply.tv_sec -= CHARGE_LIMITER_REEVALUATE_SECONDS - 1;
#endif
}
