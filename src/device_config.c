#include "device_config.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEVICE_CONFIG_PATH "/usr/resource/config.json"

bool device_config_get_volume_warn_threshold(int * out_percent) {
    FILE * f = fopen(DEVICE_CONFIG_PATH, "r");
    if (!f) return false;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) {
        fclose(f);
        return false;
    }

    char * buf = malloc((size_t) size + 1);
    if (!buf) {
        fclose(f);
        return false;
    }
    size_t read_bytes = fread(buf, 1, (size_t) size, f);
    fclose(f);
    buf[read_bytes] = '\0';

    cJSON * root = cJSON_ParseWithLength(buf, read_bytes);
    free(buf);
    if (!root) return false;

    /* config.json is a flat array of {"type": ..., ...} objects rather
     * than one top-level object -- confirmed on a real device dump. */
    bool found = false;
    cJSON * entry;
    cJSON_ArrayForEach(entry, root) {
        cJSON * type = cJSON_GetObjectItemCaseSensitive(entry, "type");
        if (!cJSON_IsString(type) || strcmp(type->valuestring, "volume") != 0) continue;

        cJSON * enable = cJSON_GetObjectItemCaseSensitive(entry, "vol_warn_enable");
        if (!cJSON_IsNumber(enable) || enable->valueint == 0) break;

        cJSON * warn_vol = cJSON_GetObjectItemCaseSensitive(entry, "warn_vol");
        cJSON * first = cJSON_IsArray(warn_vol) ? cJSON_GetArrayItem(warn_vol, 0) : NULL;
        cJSON * headset = first ? cJSON_GetObjectItemCaseSensitive(first, "headset") : NULL;
        if (cJSON_IsNumber(headset)) {
            *out_percent = headset->valueint;
            found = true;
        }
        break;
    }

    cJSON_Delete(root);
    return found;
}
