#ifndef TIMEZONE_APPLY_H
#define TIMEZONE_APPLY_H

#include <stdbool.h>

/* Applies `iana_id` (e.g. "America/New_York" -- must match a TIMEZONE_TABLE
 * entry / a real /usr/share/zoneinfo file, see timezone_data.h) as the
 * active time zone immediately: setenv("TZ", iana_id, 1) + tzset(), so
 * every subsequent localtime_r()/strftime() in this process reflects it
 * without a restart. Also persists it for future boots by copying the real
 * zoneinfo file's bytes into /usr/data/localtime -- /etc/localtime is
 * already a symlink to that path in the stock firmware (confirmed via
 * hiby_player.sh's own comment: "the same trick used for
 * /usr/data/localtime"), so this is reusing an existing convention, not
 * inventing a new one. HOST_BUILD only does the in-process setenv/tzset
 * half -- there's no real /usr/data on a dev machine, and definitely no
 * touching the dev machine's own /etc/localtime. Returns false if the
 * target-only file copy failed (e.g. iana_id doesn't match a real zoneinfo
 * file); the in-process TZ env var is applied either way, best-effort. */
bool timezone_apply(const char * iana_id);

#endif /* TIMEZONE_APPLY_H */
