#include "input_device_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef bool (*input_device_match_fn)(const char * line, const void * context);

static bool match_exact_name(const char * line, const void * context) {
    const char * quoted_name = (const char *) context;
    return strstr(line, quoted_name) != NULL;
}

static bool match_containing(const char * line, const void * context) {
    const char * substring = (const char *) context;
    return strstr(line, "Name=\"") != NULL && strstr(line, substring) != NULL;
}

static bool scan_input_devices(input_device_match_fn match_fn, const void * context,
                               char * out_path, size_t out_size) {
    FILE * f = fopen("/proc/bus/input/devices", "r");
    if (!f) return false;

    char line[256];
    bool in_block = false;
    bool found = false;

    while (fgets(line, sizeof(line), f)) {
        if (match_fn(line, context)) {
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

bool find_input_device_by_name(const char * name, char * out_path, size_t out_size) {
    char quoted_name[128];
    snprintf(quoted_name, sizeof(quoted_name), "Name=\"%s\"", name);
    return scan_input_devices(match_exact_name, quoted_name, out_path, out_size);
}

bool find_input_device_containing(const char * substring, char * out_path, size_t out_size) {
    return scan_input_devices(match_containing, substring, out_path, out_size);
}
