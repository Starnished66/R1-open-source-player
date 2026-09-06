#ifndef USB_AUDIO_OUTPUT_H
#define USB_AUDIO_OUTPUT_H

#include <stdbool.h>
#include <stddef.h>

/* Detects an externally connected USB audio device (DAC/amp/DSP headphone)
 * enumerated by the snd-usb-audio driver. Writes the ALSA device string
 * ("plughw:<card>,0") into out. Returns false if no external device is connected.
 * Safe to call synchronously on the UI thread. */
bool usb_audio_output_is_connected(char * out, size_t out_size);

#endif /* USB_AUDIO_OUTPUT_H */
