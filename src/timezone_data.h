#ifndef TIMEZONE_DATA_H
#define TIMEZONE_DATA_H

typedef struct {
    const char * iana_id;       /* e.g. "America/New_York" -- matches a real
                                    /usr/share/zoneinfo/<iana_id> file on
                                    device, verified at generation time. */
    const char * display_name;  /* e.g. "New York (UTC-04:00)" -- offset is
                                    a static hint computed once at generation
                                    time from this device's own tzdata, not
                                    live; actual behavior after selection
                                    comes from the real zoneinfo file via
                                    tzset(), DST included. */
    int utc_offset_minutes;
} timezone_entry_t;

#define TIMEZONE_TABLE_COUNT 374
extern const timezone_entry_t TIMEZONE_TABLE[TIMEZONE_TABLE_COUNT];

#endif /* TIMEZONE_DATA_H */
