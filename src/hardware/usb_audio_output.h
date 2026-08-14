#ifndef USB_AUDIO_OUTPUT_H
#define USB_AUDIO_OUTPUT_H

#include <stdbool.h>
#include <stddef.h>

/* Detects an externally connected USB audio device (DAC/amp/DSP headphone)
 * enumerated by the kernel's own snd-usb-audio driver once this device's
 * USB-C port negotiates USB HOST role -- see the USB Audio Output plan
 * (~/.claude/plans/modular-wiggling-creek.md at the time this was written,
 * real-device investigation into this device's dwc2 dual-role controller)
 * for the full story. Writes an ALSA device string ("plughw:<card>,0")
 * usable directly with `aplay -D` into out -- audio_output.c only ever
 * needs the final device string, same shape as how it already resolves
 * "bluealsa" as a literal string rather than a raw card index, since which
 * card index a USB DAC actually enumerates as isn't fixed the way
 * bluealsa's own ALSA plugin name is. Returns false (out left untouched)
 * if nothing's connected. Safe to call directly on the UI thread every
 * poll tick -- a plain /dev/snd directory scan, no subprocess spawn, same
 * class of cheap synchronous operation as headphone_status.c's own direct
 * sysfs read. */
bool usb_audio_output_is_connected(char * out, size_t out_size);

#endif /* USB_AUDIO_OUTPUT_H */
