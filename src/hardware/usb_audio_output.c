#include "usb_audio_output.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEV_SND_DIR "/dev/snd"

/* This device's kernel build has no /proc/asound at all (confirmed on real
 * hardware -- CONFIG_SND_PROC_FS isn't enabled, even the internal codec
 * doesn't show up there), so card detection has to go through the raw ALSA
 * device nodes in /dev/snd/ instead, which do exist (tinyalsa opens
 * /dev/snd/pcmC0D0p directly for the internal codec, confirmed working).
 * Playback device nodes are named pcmC<card>D<device>p -- card 0 is always
 * this device's own internal codec (confirmed real-device: pcmC0D0p is
 * present at boot with nothing else attached), so an externally connected
 * USB DAC/amp/DSP headphone is identified as the first pcmC<N>D0p node with
 * N != 0. "plughw" (not "hw") wraps the raw card in ALSA's own rate/format
 * conversion plugin, same reasoning as bluealsa's own pcm.bluealsa "type
 * plug" wrapping (see audio_output.c's own comment) -- an arbitrary
 * external DAC isn't guaranteed to natively support whatever rate this app
 * happens to be decoding at. */
bool usb_audio_output_is_connected(char * out, size_t out_size) {
    DIR * d = opendir(DEV_SND_DIR);
    if (!d) return false;

    int card_index = -1;
    struct dirent * entry;
    while ((entry = readdir(d)) != NULL) {
        const char * name = entry->d_name;
        if (strncmp(name, "pcmC", 4) != 0) continue;
        const char * suffix = strchr(name + 4, 'D');
        if (!suffix || strcmp(suffix, "D0p") != 0) continue;
        int idx = atoi(name + 4);
        if (idx != 0) {
            card_index = idx;
            break;
        }
    }
    closedir(d);

    if (card_index < 0) return false;
    snprintf(out, out_size, "plughw:%d,0", card_index);
    return true;
}
