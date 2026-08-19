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

/* Real-device incident (2026-08-08): after this feature was active for a
 * while on a charging device, the status bar's battery percentage/charge
 * status stopped updating, and separately the whole UI went white/text-only
 * (asset loading failing). At the time this looked like it might be this
 * file's own repeated raw i2c smbus transactions against the AXP2101
 * somehow wedging the power_supply sysfs nodes battery.c reads from, so
 * this was reverted to fully inert as a precaution. Root-caused since:
 * battery.c's read_best_battery_status() had a real, unrelated file
 * descriptor leak (its "battery" fast path returned early without
 * closedir()) that leaked a few fds a second on every status-bar poll --
 * exhausting the process's fd limit in well under ten minutes, at which
 * point every open()/fopen() call in the whole process starts failing at
 * once (sysfs reads AND PNG asset loading, together, matching exactly what
 * was seen). Fixed in battery.c; confirmed independent of this file. Back
 * to active. */
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

/* Real-device incident (2026-08-07): charge_limiter previously throttled by
 * zeroing REG62 (ICC / "fast charge current", the constant-CURRENT phase
 * target) instead of this register -- documented at length in
 * charge_limiter.h's own history as the fix for an even earlier wrong
 * approach (a sysfs node wired to the wrong register entirely). REG62 was a
 * real, working control, just for the wrong phase of charging: per the
 * official AXP2101 datasheet section 7.7.3.3, once VBAT reaches the target
 * voltage (VREG) the charger enters constant-VOLTAGE (CV) mode and holds
 * output voltage constant while current tapers on its own -- REG62's
 * CC-phase current ceiling stops being the active constraint at that point,
 * so zeroing it has no effect on a charge cycle that's already in CV phase.
 * Confirmed as the actual bug: user report was charging stopping at
 * ~91-92% instead of the configured 85% -- consistent with the battery
 * already being in CV phase well before the 85% check ever fires, so the
 * charger just ran its own CV taper down to its own internal termination-
 * current threshold (ITERM, ~125mA default) and completed on its own,
 * ignoring REG62 entirely. chg_en (this register) disables the WHOLE
 * charger state machine, CC and CV phases alike -- confirmed against the
 * same datasheet section: "the charger is enabled when an adapter is
 * inserted" and "charger enable bit is reset to 1" (to exit battery-safe-
 * mode) are both described as controls over this exact bit, not REG62. */

#define CHARGE_LIMITER_STOP_PERCENT 85
#define CHARGE_LIMITER_TRIGGER_PERCENT 84
#define CHARGE_LIMITER_RESUME_PERCENT 82

/* Re-checks and re-applies at most this often, rather than on every 500ms
 * gui.c timer tick -- the i2c write itself is cheap, but there's no reason
 * to repeat it dozens of times a minute while nothing has changed.
 * Re-applying periodically (not just on threshold-crossing edges) also
 * covers the datasheet's own documented auto-re-enable-on-adapter-insert
 * behavior -- if the user unplugs/replugs the cable while already over the
 * limit, the PMIC may re-enable charging on its own, and the next poll
 * corrects it again within this interval.
 *
 * Was 30s; tightened after a real-device overshoot report (stopping at
 * 86-91% instead of 85%) -- the primary cause turned out to be
 * battery.c's own sensor-selection bug (see its history), not this
 * interval, but a shorter window still directly reduces how much the
 * battery can climb between "crossed 85%" and "we actually noticed and
 * disabled charging," on general principle, for whatever residual lag the
 * real fuel gauge itself has. The i2c read this costs every 5s is cheap
 * (a couple of register reads/writes, no different in kind from what
 * gui.c's own status-bar polling already does more often than this). */
#define CHARGE_LIMITER_REEVALUATE_SECONDS 5

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
    static bool limiter_holding = false;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (!force && last_apply.tv_sec != 0 && now.tv_sec - last_apply.tv_sec < CHARGE_LIMITER_REEVALUATE_SECONDS) return;
    last_apply = now;

    if (!enabled) {
        if (enable_charging()) limiter_holding = false;
        else last_apply.tv_sec -= CHARGE_LIMITER_REEVALUATE_SECONDS - 1; /* retry in ~1s */
        return;
    }

    int percent = battery_get_percent();
    if (percent < 0) return; /* no battery data (e.g. host build) -- nothing to act on */

    /* battery_get_percent() itself, not just the chg_stat register logged
     * inside disable_charging()/enable_charging(), so a live overshoot
     * report can distinguish "we correctly disabled right at 85%, the
     * fuel gauge itself later crept up" from "we were late/reading the
     * wrong sensor" -- exactly the ambiguity the 86%-vs-91% two-tester
     * report needed and didn't have visibility into before this. */
    DBG_LOG("charge_limiter: poll percent=%d (target=%d trigger=%d resume=%d holding=%d)\n",
            percent, CHARGE_LIMITER_STOP_PERCENT, CHARGE_LIMITER_TRIGGER_PERCENT,
            CHARGE_LIMITER_RESUME_PERCENT, limiter_holding);

    /* Stop one displayed percentage point early to absorb the real gauge's
     * normal lag, then hold the charger off through 83/84/85% bounce rather
     * than re-enabling it every time a noisy sample dips below 85. */
    if (percent >= CHARGE_LIMITER_TRIGGER_PERCENT) limiter_holding = true;
    else if (percent <= CHARGE_LIMITER_RESUME_PERCENT) limiter_holding = false;

    bool applied = limiter_holding ? disable_charging() : enable_charging();
    if (!applied) last_apply.tv_sec -= CHARGE_LIMITER_REEVALUATE_SECONDS - 1; /* retry in ~1s */
#endif
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
