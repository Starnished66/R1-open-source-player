#ifndef USB_MODE_CONTROL_H
#define USB_MODE_CONTROL_H

#include <stdbool.h>

/* The three USB device (gadget) modes the stock firmware's own Settings >
 * USB device mode dropdown exposes -- confirmed via strings in the stock
 * hiby_player binary (system_if_usb_select_device()) and the matching
 * shell scripts it shells out to (see usb_mode_control.c). Storage is the
 * default: a fresh boot has no USB gadget bound at all (ADB's own
 * /etc/init.d/T90adb is T-prefixed, i.e. not auto-run by init), so nothing
 * is active until the app or the user explicitly picks a mode. */
typedef enum {
    USB_MODE_STORAGE = 0,
    USB_MODE_DAC,
    USB_MODE_ADB,
} usb_mode_t;

/* Switches the USB gadget to `mode`, reusing the stock firmware's own
 * /usr/bin/{adbon,adboff,uac_device_config.sh,usb_dev_mass_storage.sh} and
 * /etc/init.d/adb/S440adb scripts (confirmed present and byte-identical to
 * the squashfs-root reference dump on a real device) rather than
 * reimplementing USB gadget configfs setup. Always tears down every mode's
 * gadget first, not just whichever the caller thinks was previously
 * active -- each stop script already self-guards (exits immediately) if
 * its own gadget isn't currently up, so this is safe and side-steps ever
 * needing to trust in-app state against hardware state that doesn't
 * survive a reboot anyway. Blocking (several real configfs/sysfs writes
 * per script); call off the UI thread.
 *
 * Returns true only if the target's own start script exited 0 AND the
 * resulting gadget is actually verified bound to the UDC afterward -- a
 * script running to completion isn't proof it worked (its own
 * precondition checks can fail silently from the caller's point of view
 * otherwise). Also refuses to even attempt the switch (returns false
 * immediately) if leftover gadget state from an incomplete previous
 * teardown is still present, rather than starting on top of it -- see
 * usb_mode_control.c's own comment on why that matters (Storage and DAC
 * share the same "android0" configfs directory, and only DAC's stop path
 * fully removes it). */
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

#endif /* USB_MODE_CONTROL_H */
