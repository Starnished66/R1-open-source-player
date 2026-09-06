#include "usb_audio_output.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEV_SND_DIR "/dev/snd"

/* Card detection uses the raw ALSA device nodes in /dev/snd/ (pcmC<card>D<device>p).
 * Card 0 is the internal codec, so an externally connected USB DAC is identified
 * as the first pcmC<N>D0p node with N != 0. "plughw" wraps the raw card in ALSA's
 * rate/format conversion plugin to accommodate arbitrary external DAC sample rates. */
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
