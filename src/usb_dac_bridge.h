#ifndef USB_DAC_BRIDGE_H
#define USB_DAC_BRIDGE_H

#include <stdbool.h>

/* Bridges the vendor /dev/uac_sa character device (the raw PCM feed from a
 * connected PC once the USB gadget is in UAC2 "sound card" mode -- see
 * usb_mode_control.c) into the shared audio_output module (see
 * audio_output.h) -- the same local-hardware-or-Bluetooth output audio.c's
 * own playback thread uses -- so USB DAC mode actually produces sound
 * instead of just enumerating as a silent sound card, and (real-device bug
 * report: "can't use bluetooth headphones while on USB DAC") can route that
 * sound to a connected Bluetooth accessory the same way local file
 * playback already can.
 *
 * /dev/uac_sa's exact ioctl protocol is undocumented and wasn't recoverable
 * (no kernel .ko to disassemble -- the f_uac_sa function is built into the
 * kernel image -- and live-tracing the stock player's own use of it wasn't
 * possible: ADB and the DAC gadget can't be up on the USB port at the same
 * time on this device, and this device's adbd doesn't support `adb tcpip`
 * to move ADB off USB). This works without it: open the device, read() raw
 * frames, write them to the output -- no ioctl call at all. Getting there
 * needed two empirical corrections against uac_device_config.sh's own
 * gadget config, which turned out not to describe the driver's actual
 * behavior (see usb_dac_bridge.c's top comment for exactly what and how
 * each was confirmed): a channel fixup (one of the two interleaved 16-bit
 * slots is always noise, not the declared stereo pair) and the true frame
 * rate (measured at ~96kHz on a real device, not the declared 48kHz).
 * EPERM on read() until the host actually arms the streaming interface is
 * also handled (retried, not fatal) -- see EPERM_RETRY_LIMIT in the .c. */

/* Starts the bridge thread. Stops local playback first (audio_stop()) to
 * free the shared hw:0,0 device. Safe to call again while already running
 * (no-op). Must only be called after usb_mode_control_apply(USB_MODE_DAC)
 * has brought the gadget up. */
void usb_dac_bridge_start(void);

/* Stops the bridge thread and closes both the uac_sa fd and the output
 * device, if running. Safe to call when not running (no-op). Blocks until
 * the thread has actually exited. */
void usb_dac_bridge_stop(void);

/* Routes the bridge's own output to a connected Bluetooth accessory
 * instead of local hardware, or back again -- mirrors audio_set_bt_output()
 * exactly (see its own doc comment in audio.h), just for this bridge's
 * separate output stream instead of local file playback. Call from the
 * same place gui.c's poll_refresh_bt_icon() already calls
 * audio_set_bt_output(), so both stay in sync with actual Bluetooth
 * connection state regardless of which one happens to be active. Only
 * takes effect on the bridge's next internal reopen check (within one
 * read() cycle of /dev/uac_sa, not instant) if the bridge is currently
 * running; harmless to call when it isn't (just updates the flag
 * audio_output.c reads on the next start). */
void usb_dac_bridge_set_bt_output(bool enabled);

#endif /* USB_DAC_BRIDGE_H */
