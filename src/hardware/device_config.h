#ifndef DEVICE_CONFIG_H
#define DEVICE_CONFIG_H

#include <stdbool.h>

/* Reads the stock firmware's own /usr/resource/config.json -- the same
 * file the stock hiby_player binary itself reads its "loud volume turns
 * the readout red" threshold from (confirmed by inspecting a real R1 Pro's
 * copy: a top-level {"type":"volume", "vol_warn_enable":.., "warn_vol":
 * [{"headset":N}]} entry). Returns false when the file doesn't exist (host
 * build), can't be parsed, or vol_warn_enable is 0 -- callers should treat
 * false as "never show the warning color," not fall back to a guessed
 * default threshold. On success, *out_percent is the headset warn_vol
 * value (the R1 Pro has no other output type). */
bool device_config_get_volume_warn_threshold(int * out_percent);

#endif
