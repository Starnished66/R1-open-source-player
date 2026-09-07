#ifndef DB_LOG_H
#define DB_LOG_H

#include <stdbool.h>
#include <stdint.h>

/* Runtime-toggleable, file-backed diagnostic log for the library database
 * scan and album art cache pipelines (Settings -> About -> Developer
 * Options -> "Enable database logging"). Writes timestamped, appended lines
 * to .logs/database_artwork.log on the SD card (rotated to .log.1 past
 * DB_LOG_MAX_BYTES, see db_log.c), buffered and flushed every few lines
 * rather than per line to keep enabling it from adding real per-file SD
 * write overhead to a scan. db_log()/db_log_rss_kb() check the enabled flag
 * first and return immediately when off, so leaving calls in per-file/
 * per-song log sites costs one atomic load when disabled. Turning logging
 * off flushes and closes the file (not just flushes it), so a later SD
 * removal/reinsertion can't leave this writing through a stale handle. */
void db_log_set_enabled(bool enabled);
bool db_log_enabled(void);

void db_log(const char * area, const char * fmt, ...);
/* Guard at the call site so logging-only arguments (RSS reads, clock calls,
 * DB queries) are not evaluated at all in the default disabled mode. The
 * implementation deliberately rechecks after taking its mutex as well, to
 * cover a concurrent disable between this check and the actual write. */
#define DB_LOG(area, fmt, ...) \
    do { \
        if (db_log_enabled()) db_log((area), (fmt), ##__VA_ARGS__); \
    } while (0)

/* Monotonic milliseconds, for elapsed-time fields between two DB_LOG calls. */
uint64_t db_log_now_ms(void);

/* Current process RSS in KB, or -1 if unavailable or logging is disabled.
 * Opens and parses /proc/self/status -- cheap in isolation, but only worth
 * calling at phase boundaries, progress intervals, slow files, and
 * failures, never in a tight per-song/per-file loop (thousands of calls
 * during a large scan add up even at microseconds each). */
long db_log_rss_kb(void);

#endif /* DB_LOG_H */
