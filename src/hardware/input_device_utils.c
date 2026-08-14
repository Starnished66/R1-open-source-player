#include "input_device_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool find_input_device_by_name(const char * name, char * out_path, size_t out_size) {
    FILE * f = fopen("/proc/bus/input/devices", "r");
    if (!f) return false;

    char quoted_name[128];
    snprintf(quoted_name, sizeof(quoted_name), "Name=\"%s\"", name);

    char line[256];
    bool in_block = false;
    bool found = false;

    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, quoted_name)) {
            in_block = true;
        } else if (line[0] == '\n') {
            in_block = false;
        } else if (in_block && strstr(line, "Handlers=")) {
            char * event_str = strstr(line, "event");
            if (event_str) {
                snprintf(out_path, out_size, "/dev/input/event%d", atoi(event_str + 5));
                found = true;
                in_block = false;
            }
        }
    }

    fclose(f);
    return found;
}

bool find_input_device_containing(const char * substring, char * out_path, size_t out_size) {
    FILE * f = fopen("/proc/bus/input/devices", "r");
    if (!f) return false;

    char line[256];
    bool in_block = false;
    bool found = false;

    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "Name=\"") && strstr(line, substring)) {
            in_block = true;
        } else if (line[0] == '\n') {
            in_block = false;
        } else if (in_block && strstr(line, "Handlers=")) {
            char * event_str = strstr(line, "event");
            if (event_str) {
                snprintf(out_path, out_size, "/dev/input/event%d", atoi(event_str + 5));
                found = true;
                in_block = false;
            }
        }
    }

    fclose(f);
    return found;
}
