#ifndef USB_DAC_BRIDGE_H
#define USB_DAC_BRIDGE_H

#include <stdbool.h>

typedef struct {
    bool bridge_running;
    bool streaming;
    unsigned int input_sample_rate;
    unsigned int input_bit_depth;
    unsigned int output_sample_rate;
    unsigned int output_bit_depth;
} usb_dac_stream_info_t;

/* Thread-safe bridge snapshot. These are transport/path formats, not the
 * original host media resolution: the host may resample before USB output. */
void usb_dac_bridge_get_stream_info(usb_dac_stream_info_t * out);

/* Bridges the /dev/uac_sa character device (the raw PCM feed from the host PC
 * when the USB gadget is in UAC2 sound card mode) into the shared audio_output
 * module, routing to local hardware or connected Bluetooth output. */

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
