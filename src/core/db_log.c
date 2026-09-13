#include "db_log.h"

#include <stdio.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <pthread.h>
#include <time.h>
#include <sys/stat.h>
#include <errno.h>

#ifdef HOST_BUILD
#define DB_LOG_DIR "./music/.logs"
#else
#define DB_LOG_DIR "/data/mnt/sd_0/.logs"
#endif
#define DB_LOG_PATH DB_LOG_DIR "/database_artwork.log"
#define DB_LOG_ROTATED_PATH DB_LOG_DIR "/database_artwork.log.1"

/* A full library scan can emit thousands of lines (two-plus per song). A
 * write()/fsync-equivalent syscall to the SD card on every single line would
 * add real, measurable I/O overhead (and flash wear) to something a user may
 * leave enabled for normal use, not just a one-off debug session -- so this
 * batches DB_LOG_FLUSH_INTERVAL lines through libc's own buffering into one
 * real write instead of one per line, at the cost of losing at most that
 * many not-yet-flushed lines on a hard crash/power loss. That's an
 * acceptable loss for a diagnostic log -- real crash forensics (the file
 * being parsed when a crash happens) already has its own durable path via
 * g_scan_last_path + main.c's crash handler, which fsyncs reload_diag.log
 * independently of this module. db_log_set_enabled(false) flushes and
 * closes immediately so nothing buffered is lost and no stale handle
 * lingers. */
#define DB_LOG_FLUSH_INTERVAL 20

/* Current log file size at which it's rotated to .log.1 (overwriting any
 * previous one) -- keeps leaving this enabled indefinitely from slowly
 * consuming the SD card. Checked only alongside the periodic flush above,
 * not every line. */
#define DB_LOG_MAX_BYTES (2L * 1024 * 1024)

static atomic_bool db_log_is_enabled = false;
static pthread_mutex_t db_log_mutex = PTHREAD_MUTEX_INITIALIZER;
static FILE * db_log_file;
static unsigned db_log_lines_since_flush;

/* Must be called with db_log_mutex held and db_log_file non-NULL. */
static void db_log_rotate_if_needed_locked(void) {
    db_log_rotate_file_if_needed_locked(&db_log_file, DB_LOG_PATH, DB_LOG_ROTATED_PATH, DB_LOG_MAX_BYTES);
}

void db_log_set_enabled(bool enabled) {
    bool was_enabled = atomic_exchange(&db_log_is_enabled, enabled);
    if (was_enabled && !enabled) {
        pthread_mutex_lock(&db_log_mutex);
        if (db_log_file) {
            fflush(db_log_file);
            fclose(db_log_file);
            db_log_file = NULL;
            db_log_lines_since_flush = 0;
        }
        pthread_mutex_unlock(&db_log_mutex);
    }
}

bool db_log_enabled(void) {
    return atomic_load(&db_log_is_enabled);
}

uint64_t db_log_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000ULL + (uint64_t) ts.tv_nsec / 1000000ULL;
}

long db_log_rss_kb(void) {
    if (!db_log_enabled()) return -1;
    FILE * f = fopen("/proc/self/status", "r");
    if (!f) return -1;
    char line[160];
    long rss = -1;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "VmRSS: %ld kB", &rss) == 1) break;
    }
    fclose(f);
    return rss;
}

void db_log(const char * area, const char * fmt, ...) {
    if (!db_log_enabled()) return;
    pthread_mutex_lock(&db_log_mutex);
    /* Recheck under the lock: db_log_set_enabled(false) may have run (and
     * closed db_log_file) between the unlocked check above and acquiring
     * this mutex -- without this, that race would reopen and write through
     * a file logging was just told to stop touching. */
    if (!db_log_enabled()) {
        pthread_mutex_unlock(&db_log_mutex);
        return;
    }
    if (!db_log_file) {
        if (mkdir(DB_LOG_DIR, 0755) != 0 && errno != EEXIST) {
            pthread_mutex_unlock(&db_log_mutex);
            return;
        }
        db_log_file = fopen(DB_LOG_PATH, "a");
        if (!db_log_file) {
            pthread_mutex_unlock(&db_log_mutex);
            return;
        }
        /* Default (full) buffering -- see DB_LOG_FLUSH_INTERVAL's own
         * comment above for why this isn't line-buffered/flushed-per-line. */
    }

    fprintf(db_log_file, "[%s] t=%llu ", area, (unsigned long long) db_log_now_ms());
    va_list ap;
    va_start(ap, fmt);
    vfprintf(db_log_file, fmt, ap);
    va_end(ap);
    fputc('\n', db_log_file);
    if (++db_log_lines_since_flush >= DB_LOG_FLUSH_INTERVAL) {
        fflush(db_log_file);
        db_log_lines_since_flush = 0;
        db_log_rotate_if_needed_locked();
    }
    pthread_mutex_unlock(&db_log_mutex);
}
