#include "charge_limiter.h"
#include "debug_log.h"  // TODO: fix false-positive "'debug_log.h' file not found" warning in IDE

#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

/* CHARGE_LIMITER_ACTIVE gates the AXP2101 i2c transactions.
 * Set to 0 to disable entirely; the limiter is independent of
 * battery.c's status polling. */
#define CHARGE_LIMITER_ACTIVE 1

#define MP2731_I2C_BUS "/dev/i2c-0"
#define MP2731_I2C_ADDR 0x4b

/* TODO: description */
#define MP2731_REG_CHARGE_CURRENT_REGULATION 0x05
#define MP2731_REG_CHARGE_VOLTAGE_REGULATION 0x07

/* MP2731 Fast Charge Current:
 * This sets the fast charge current. It has a 320mA offset and a 320mA to 4520mA range
 *
 * | Bit | Meaning |
 * |-----|---------|
 * | 6   | 2560mA  |
 * | 5   | 1280mA  |
 * | 4   | 640mA   |
 * | 3   | 320mA   |
 * | 2   | 160mA   |
 * | 1   | 80mA    |
 * | 0   | 40mA    |
*/
#define MP2731_FAST_CHARGE_CURRENT_MASK 0b01111111  // for CHARGE_CURRENT_REGULATION register

/* 480mA is the closest value at or below the AXP2101's 500mA safe-charging
 * cap that the MP2731's 40mA step size can express (320mA offset + bit2's
 * 160mA = 480mA). */
#define MP2731_FAST_CHARGE_CURRENT_480MA 0b00000100

/* MP2731 Battery Regulation Voltage (Max Charge Voltage):
 * This sets the battery regulation voltage. It has a 3.4V offset and a 3.4V to 4.67V range
 *
 * | Bit | Meaning |
 * |-----|---------|
 * | 7   | 640mV   |
 * | 6   | 320mV   |
 * | 5   | 160mV   |
 * | 4   | 80mV    |
 * | 3   | 40mV    |
 * | 2   | 20mV    |
 * | 1   | 10mV    |
*/
#define MP2731_BATTERY_REGULATION_VOLTAGE_MASK 0b11111110  // for CHARGE_VOLTAGE_REGULATION register

#define AXP2101_I2C_BUS "/dev/i2c-0"
#define AXP2101_I2C_ADDR 0x34

/* Register 0x64 on the AXP2101 is for CV Charge Voltage Settings.
 * Only the 3 low bits are used. The 3 low bits define the charge voltage
 * limit. */
#define AXP2101_REG_CV_CHARGE_VOLTAGE_SETTING 0x64

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
static bool smbus_xfer(char * i2c_bus, uint8_t i2c_addr, uint8_t reg, uint8_t * value, bool write) {
    int fd = open(i2c_bus, O_RDWR);
    if (fd < 0) return false;

    bool ok = false;
    if (ioctl(fd, I2C_SLAVE_FORCE, i2c_addr) >= 0) {
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

// TODO: define enum for registers (separate enums for axp2101 and mp2731)
static bool axp2101_read_reg(uint8_t reg, uint8_t * out) {
    return smbus_xfer(AXP2101_I2C_BUS, AXP2101_I2C_ADDR, reg, out, false);
}

static bool axp2101_write_reg(uint8_t reg, uint8_t value) {
    return smbus_xfer(AXP2101_I2C_BUS, AXP2101_I2C_ADDR, reg, &value, true);
}

static bool mp2731_read_reg(uint8_t reg, uint8_t * out) {
    return smbus_xfer(MP2731_I2C_BUS, MP2731_I2C_ADDR, reg, out, false);
}

static bool mp2731_write_reg(uint8_t reg, uint8_t value) {
    return smbus_xfer(MP2731_I2C_BUS, MP2731_I2C_ADDR, reg, &value, true);
}

static void log_chg_stat(const char * when) {
    uint8_t stat;
    if (!axp2101_read_reg(AXP2101_REG_CHG_STAT, &stat)) return;
    static const char * const names[8] = {
        "tri_charge", "pre_charge", "constant_current", "constant_voltage",
        "charge_done", "not_charging", "reserved", "reserved",
    };
    DBG_LOG("charge_limiter: chg_stat %s = %s (0x%02X)\n", when, names[stat & 0x07], stat & 0x07);

    if (!axp2101_read_reg(AXP2101_REG_CV_CHARGE_VOLTAGE_SETTING, &stat)) return;
    printf("charge_limiter: CV Charge Voltage Setting = %d\n", stat);
}

enum AXP2101_CHARGE_VOLTAGE_LIMIT {
	AXP2101_CHARGE_VOLTAGE_LIMIT_4V    = 1,
	AXP2101_CHARGE_VOLTAGE_LIMIT_4_1V  = 2,
	AXP2101_CHARGE_VOLTAGE_LIMIT_4_2V  = 3,
	AXP2101_CHARGE_VOLTAGE_LIMIT_4_35V = 4,
	AXP2101_CHARGE_VOLTAGE_LIMIT_4_4V  = 5,
};

enum MP2731_CHARGE_VOLTAGE_LIMIT {
	MP2731_CHARGE_VOLTAGE_LIMIT_4V    = 0b01111000,
	MP2731_CHARGE_VOLTAGE_LIMIT_4_1V  = 0b10001100,
	MP2731_CHARGE_VOLTAGE_LIMIT_4_2V  = 0b10100000,
	MP2731_CHARGE_VOLTAGE_LIMIT_4_35V = 0b10111110,
	MP2731_CHARGE_VOLTAGE_LIMIT_4_4V  = 0b11001000,
};

// Function to set AXP2101 charge_voltage_limit
// Returns true on success, false on failure
/*
 * Charge Voltage Limit Value Definition:
 * 000 -> reserved
 * 001 -> 4.0V
 * 010 -> 4.1V
 * 011 -> 4.2V
 * 100 -> 4.35V
 * 101 -> 4.4V
 * 11X -> reserved
 *
 * Source: https://files.waveshare.com/wiki/common/X-power-AXP2101_SWcharge_V1.0.pdf
 */
// NOTE: the low 3 bits of register 0x64 are the only ones used in that byte. So it is safe to always write 0 to the other bits.
static bool axp2101_set_charge_voltage_limit(enum AXP2101_CHARGE_VOLTAGE_LIMIT value) {
	// return failure if trying to set an invalid value (reserved)
	if (value == 0 || value >= 6) {
		return false;
	}

	uint8_t stat;

	// read current setting
	if (!axp2101_read_reg(AXP2101_REG_CV_CHARGE_VOLTAGE_SETTING, &stat)) return false;
	printf("charge_limiter: CV Charge Voltage Setting = %d\n", stat);

	// if value already correct, exit early
	if (stat == value) return true;

	// write the new setting (no write happens if value was already correct)
	if (!axp2101_write_reg(AXP2101_REG_CV_CHARGE_VOLTAGE_SETTING, value)) return false;
	printf("charge_limiter: set charge voltage limit value to %d\n", value);

	// check for failed write
	axp2101_read_reg(AXP2101_REG_CV_CHARGE_VOLTAGE_SETTING, &stat);
	printf("charge_limiter: checked charge voltage limit value: %d\n", value);
	if (stat != value) return false;

	return true;
}

// function to set the AXP2101 fast charge current limit
// value is a raw register value already masked to AXP2101_CHG_CURRENT_MASK
static bool axp2101_set_charge_current_limit(uint8_t value) {
	// return failure if trying to set a bit that's not in the mask
	if ((value & (~AXP2101_CHG_CURRENT_MASK)) != 0) {
		return false;
	}

	uint8_t stat;

	// read current setting
	if (!axp2101_read_reg(AXP2101_REG_CHG_CURRENT, &stat)) return false;
	DBG_LOG("safe_charging: Read AXP2101 Charge Current Value: %d\n", stat);

	// if value already correct, exit early
	if ((stat & AXP2101_CHG_CURRENT_MASK) == value) return true;

	uint8_t desired = (stat & (uint8_t) ~AXP2101_CHG_CURRENT_MASK) | value;

	// write the new setting
	if (!axp2101_write_reg(AXP2101_REG_CHG_CURRENT, desired)) return false;
	DBG_LOG("safe_charging: Set AXP2101 Charge Current Value: %d\n", desired);

	// check for failed write
	if (!axp2101_read_reg(AXP2101_REG_CHG_CURRENT, &stat)) return false;
	DBG_LOG("safe_charging: Checked AXP2101 Charge Current Value: %d\n", stat);
	if ((stat & AXP2101_CHG_CURRENT_MASK) != value) return false;

	return true;
}

// function to set the mp2731 charge voltage limit
// reference the comment above MP2731_BATTERY_REGULATION_VOLTAGE_MASK to see what each input value means
static bool mp2731_set_charge_voltage_limit(enum MP2731_CHARGE_VOLTAGE_LIMIT value) {
	// return failure if trying to set a bit that's not in the mask
	if ((value & (~MP2731_BATTERY_REGULATION_VOLTAGE_MASK)) != 0) {
		return false;
	}

	// extra safety, not really needed. just applying the mask (even though we've already filtered out inputs that dont match the mask)
	value = value & MP2731_BATTERY_REGULATION_VOLTAGE_MASK;

	uint8_t stat;

	// read current setting
	if (!mp2731_read_reg(MP2731_REG_CHARGE_VOLTAGE_REGULATION, &stat)) return false;
	DBG_LOG("charge_limiter: Read MP2731 Charge Voltage Regulation Value: %d\n", stat);

	// if value already correct, exit early
	if (stat == value) return true;

	// write the new setting (no write happens if value was already correct)
	if (!mp2731_write_reg(MP2731_REG_CHARGE_VOLTAGE_REGULATION, value)) return false;
	DBG_LOG("charge_limiter: Set MP2731 Charge Voltage Regulation Value: %d\n", value);

	// check for failed write
	mp2731_read_reg(MP2731_REG_CHARGE_VOLTAGE_REGULATION, &stat);
	DBG_LOG("charge_limiter: Checked MP2731 Charge Voltage Regulation Value: %d\n", value);
	if (stat != value) return false;

	return true;
}

// function to set the mp2731 fast charge current limit
// value is a raw register value already masked to MP2731_FAST_CHARGE_CURRENT_MASK
static bool mp2731_set_charge_current_limit(uint8_t value) {
	// return failure if trying to set a bit that's not in the mask
	if ((value & (~MP2731_FAST_CHARGE_CURRENT_MASK)) != 0) {
		return false;
	}

	uint8_t stat;

	// read current setting
	if (!mp2731_read_reg(MP2731_REG_CHARGE_CURRENT_REGULATION, &stat)) return false;
	DBG_LOG("safe_charging: Read MP2731 Charge Current Regulation Value: %d\n", stat);

	// if value already correct, exit early
	if ((stat & MP2731_FAST_CHARGE_CURRENT_MASK) == value) return true;

	uint8_t desired = (stat & (uint8_t) ~MP2731_FAST_CHARGE_CURRENT_MASK) | value;

	// write the new setting
	if (!mp2731_write_reg(MP2731_REG_CHARGE_CURRENT_REGULATION, desired)) return false;
	DBG_LOG("safe_charging: Set MP2731 Charge Current Regulation Value: %d\n", desired);

	// check for failed write
	if (!mp2731_read_reg(MP2731_REG_CHARGE_CURRENT_REGULATION, &stat)) return false;
	DBG_LOG("safe_charging: Checked MP2731 Charge Current Regulation Value: %d\n", stat);
	if ((stat & MP2731_FAST_CHARGE_CURRENT_MASK) != value) return false;

	return true;
}
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
        // set back to default 4.4V charge voltage
        if (!axp2101_set_charge_voltage_limit(AXP2101_CHARGE_VOLTAGE_LIMIT_4_4V)) {
        	last_apply.tv_sec -= CHARGE_LIMITER_REEVALUATE_SECONDS - 1; /* retry in ~1s */
        }

        // TODO: make sure the mp2731 code doesn't cause bad stuff on the R1
        if (!mp2731_set_charge_voltage_limit(MP2731_CHARGE_VOLTAGE_LIMIT_4_4V)) {
        	last_apply.tv_sec -= CHARGE_LIMITER_REEVALUATE_SECONDS - 1; /* retry in ~1s */
        }

        return;
    }

    // set to 4.2V charge voltage
    if (!axp2101_set_charge_voltage_limit(AXP2101_CHARGE_VOLTAGE_LIMIT_4_2V)) {
    	last_apply.tv_sec -= CHARGE_LIMITER_REEVALUATE_SECONDS - 1; /* retry in ~1s */
    }

    if (!mp2731_set_charge_voltage_limit(MP2731_CHARGE_VOLTAGE_LIMIT_4V)) {
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
    /* Captured the first time the cap is applied this run, so disabling can
     * restore the exact pre-cap values rather than a guessed "default"
     * register value. */
    static bool current_saved = false;
    static uint8_t saved_axp2101_current;
    static uint8_t saved_mp2731_current;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (!force && last_apply.tv_sec != 0 &&
        now.tv_sec - last_apply.tv_sec < CHARGE_LIMITER_REEVALUATE_SECONDS) return;

    if (!enabled) {
        if (!current_saved) return; /* cap was never applied this run; nothing to restore */
        last_apply = now;

        bool axp_ok = axp2101_set_charge_current_limit(saved_axp2101_current);
        bool mp_ok = mp2731_set_charge_current_limit(saved_mp2731_current);
        if (!axp_ok || !mp_ok) {
            last_apply.tv_sec -= CHARGE_LIMITER_REEVALUATE_SECONDS - 1; /* retry in ~1s */
            return;
        }
        current_saved = false;
        return;
    }
    last_apply = now;

    if (!current_saved) {
        uint8_t stat;
        if (axp2101_read_reg(AXP2101_REG_CHG_CURRENT, &stat)) saved_axp2101_current = stat & AXP2101_CHG_CURRENT_MASK;
        if (mp2731_read_reg(MP2731_REG_CHARGE_CURRENT_REGULATION, &stat)) saved_mp2731_current = stat & MP2731_FAST_CHARGE_CURRENT_MASK;
        current_saved = true;
    }

    // cap to 500mA charge current
    if (!axp2101_set_charge_current_limit(AXP2101_CHG_CURRENT_500MA)) {
    	last_apply.tv_sec -= CHARGE_LIMITER_REEVALUATE_SECONDS - 1; /* retry in ~1s */
    }

    if (!mp2731_set_charge_current_limit(MP2731_FAST_CHARGE_CURRENT_480MA)) {
    	last_apply.tv_sec -= CHARGE_LIMITER_REEVALUATE_SECONDS - 1; /* retry in ~1s */
    }
#endif
}
