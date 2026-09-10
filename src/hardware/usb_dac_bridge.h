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
 * original host media resolution: the host may resample before USB output.
 * output_sample_rate is the bridge's own live measurement of the real
 * incoming rate (different hosts have been observed to genuinely negotiate
 * different rates with this gadget), not a fixed constant -- it starts at
 * input_sample_rate before the bridge's first measurement window completes
 * and can change again later if it does. */
void usb_dac_bridge_get_stream_info(usb_dac_stream_info_t * out);

/* Bridges the /dev/uac_sa character device (the raw PCM feed from the host PC
 * when the USB gadget is in UAC2 sound card mode) into the shared audio_output
 * module, routing to local hardware or connected Bluetooth output. */

/* Starts the bridge workers. Stops local playback first (audio_stop()) to
 * free the shared hw:0,0 device. Safe to call again while already running
 * (no-op). Must only be called after usb_mode_control_apply(USB_MODE_DAC)
 * has brought the gadget up. Start/stop calls are serialized. Restarting after
 * a timed-out stop waits for the old workers to be joined before reusing any
 * resources, and can block until a stalled output recovers. */
void usb_dac_bridge_start(void);

/* Requests both workers to stop and immediately clears the UI running/streaming
 * state once this serialized lifecycle operation begins. Normally joins both
 * workers, which close the uac_sa fd and output device. Waits up to about 3 seconds
 * for worker completion; a stalled worker may remain alive with its resources
 * retained for a later start/stop to reap safely. Repeated stop calls retry
 * cleanup. Waiting for an overlapping lifecycle operation is not time-bounded. */
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

/* Enables or disables persistent debug logging to the SD card for the USB DAC
 * bridge. Gated by the same developer toggle as the library database log
 * (Settings -> About -> Developer Options -> "Enable database logging").
 * On disable, immediately flushes and closes the log file. */
void usb_dac_bridge_set_debug_log_enabled(bool enabled);

#endif /* USB_DAC_BRIDGE_H */
