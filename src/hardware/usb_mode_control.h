#ifndef USB_MODE_CONTROL_H
#define USB_MODE_CONTROL_H

#include <stdbool.h>

/* The three USB device (gadget) modes supported by the hardware:
 * Storage (mass storage), DAC (USB Audio Class 2), and ADB. */
typedef enum {
    USB_MODE_STORAGE = 0,
    USB_MODE_DAC,
    USB_MODE_ADB,
} usb_mode_t;

/* Switches the USB gadget to `mode`. Tears down existing gadget configuration
 * before setting up the target mode. Blocking operation; call off the UI thread.
 *
 * Returns true only if the mode was applied and verified bound to the UDC. */
bool usb_mode_control_apply(usb_mode_t mode);

/* Inspects live configfs/sysfs gadget state to determine which mode is
 * ACTUALLY active right now, independent of whatever was last persisted to
 * settings -- current_settings.usb_mode is just a UI hint for which row to
 * pre-select (see settings.h's own comment: it deliberately isn't
 * re-applied to hardware on startup), so it can silently drift from
 * reality (a fresh boot, a switch made outside this app, or the corruption
 * usb_mode_control_apply() now defends against). Returns true and sets
 * *out_mode only when a gadget is unambiguously bound with its expected
 * function linked; returns false (leaving *out_mode untouched) if nothing
 * is clearly active, so callers can fall back to the persisted preference
 * rather than guessing. */
bool usb_mode_control_detect_current(usb_mode_t * out_mode);

/* Best-effort physical USB-power/cable presence. Used only to detect a new
 * connection and (re)bind the default Storage gadget; it does not claim to
 * distinguish a PC host from a charge-only adapter. */
bool usb_mode_control_cable_connected(void);

#endif /* USB_MODE_CONTROL_H */
