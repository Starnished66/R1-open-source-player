#ifndef APP_CLOCK_H
#define APP_CLOCK_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

void app_clock_init(bool automatic, int64_t manual_epoch, int64_t system_reference);
void app_clock_localtime(struct tm * out);
void app_clock_set_automatic(bool automatic);
void app_clock_set_local_time(int hour, int minute);
void app_clock_get_persistence(int64_t * manual_epoch, int64_t * system_reference);

#endif
