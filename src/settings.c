#include "settings.h"
#include "debug_log.h"
#include "subprocess.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef HOST_BUILD
  #define SETTINGS_FILE_PATH "./open_hiby_player_settings.txt"
  #define SETTINGS_DIR_PATH "."
#else
  /* /usr/data is the device's persistent ubifs partition (survives reboots,
   * unlike /tmp) -- the same place the stock firmware keeps its own small
   * settings files (theme_id, region, etc). */
  #define SETTINGS_FILE_PATH "/usr/data/open_hiby_player_settings.txt"
  #define SETTINGS_DIR_PATH "/usr/data"
#endif

#define SETTINGS_TMP_FILE_PATH SETTINGS_FILE_PATH ".tmp"

const int SCREEN_TIMEOUT_STEPS[SCREEN_TIMEOUT_STEP_COUNT] = { 30, 60, 120, 300, 600, 1800 };
const int IDLE_SHUTDOWN_STEPS[IDLE_SHUTDOWN_STEP_COUNT] = { 10, 15, 30, 60, 120 };
const int SLEEP_TIMER_STEPS[SLEEP_TIMER_STEP_COUNT] = { 5, 10, 15, 20, 30 };

/* Nearest entry in SCREEN_TIMEOUT_STEPS to `seconds` -- used to snap a
 * hand-edited or pre-existing settings file value onto the slider's actual
 * choices rather than just clamping into [MIN, MAX], since an in-range but
 * off-step value (e.g. a leftover "81" from before the slider had discrete
 * steps) would otherwise never match any step the UI can display. */
static int nearest_screen_timeout_step(int seconds) {
    int best = SCREEN_TIMEOUT_STEPS[0];
    int best_diff = abs(seconds - best);
    for (int i = 1; i < SCREEN_TIMEOUT_STEP_COUNT; i++) {
        int diff = abs(seconds - SCREEN_TIMEOUT_STEPS[i]);
        if (diff < best_diff) {
            best_diff = diff;
            best = SCREEN_TIMEOUT_STEPS[i];
        }
    }
    return best;
}

/* Same snapping reasoning as nearest_screen_timeout_step() above. */
static int nearest_idle_shutdown_step(int minutes) {
    int best = IDLE_SHUTDOWN_STEPS[0];
    int best_diff = abs(minutes - best);
    for (int i = 1; i < IDLE_SHUTDOWN_STEP_COUNT; i++) {
        int diff = abs(minutes - IDLE_SHUTDOWN_STEPS[i]);
        if (diff < best_diff) {
            best_diff = diff;
            best = IDLE_SHUTDOWN_STEPS[i];
        }
    }
    return best;
}

/* Same snapping reasoning as nearest_screen_timeout_step() above. */
static int nearest_sleep_timer_step(int minutes) {
    int best = SLEEP_TIMER_STEPS[0];
    int best_diff = abs(minutes - best);
    for (int i = 1; i < SLEEP_TIMER_STEP_COUNT; i++) {
        int diff = abs(minutes - SLEEP_TIMER_STEPS[i]);
        if (diff < best_diff) {
            best_diff = diff;
            best = SLEEP_TIMER_STEPS[i];
        }
    }
    return best;
}

static void set_defaults(player_settings_t * out) {
    out->volume = 1.0f;
    out->last_track[0] = '\0';
    out->last_position = 0.0;
    out->auto_resume_enabled = true;
    out->accent_color = 0x2196F3; /* matches the app's existing default blue */
    out->crossfade_enabled = false;
    out->car_mode_enabled = false;
    out->subsonic_url[0] = '\0';
    out->subsonic_username[0] = '\0';
    out->subsonic_password[0] = '\0';
    out->subsonic_verify_tls = true;
    out->bt_volume_sync_enabled = true;
    out->bt_dac_mode_enabled = false;
    snprintf(out->bt_codec, sizeof(out->bt_codec), "auto");
    out->bt_hide_unnamed_devices = true;
    out->wifi_dac_mode_enabled = false;
    out->dlna_renderer_enabled = false;
    out->screen_timeout_enabled = true;
    out->screen_timeout_seconds = 60;
    out->led_indicator_enabled = true;
    out->charge_limiter_enabled = false; /* opt-in -- caps max charge at 85%, a real behavior change the user should choose, not a default surprise */
    out->idle_shutdown_enabled = false; /* opt-in, matches stock's own default */
    out->idle_shutdown_minutes = 30;
    out->idle_suspend_enabled = false;
    out->usb_mode = 0; /* USB_MODE_STORAGE -- see settings.h's own comment on why this is a plain int */
    out->play_mode = 0; /* PLAY_MODE_SEQUENTIAL */
    out->swipe_up_home_enabled = true;
    out->startup_volume_fixed_enabled = true; /* matches stock's own default */
    out->startup_volume_fixed_percent = 20;
    out->sleep_timer_minutes = 15;
    out->timezone[0] = '\0';
    out->font_size_tier = 0;
}

bool settings_load(player_settings_t * out) {
    set_defaults(out);

    FILE * f = fopen(SETTINGS_FILE_PATH, "r");
    if (!f) return false;

    char line[600];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = '\0';

        char * eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        const char * key = line;
        const char * value = eq + 1;

        if (strcmp(key, "volume") == 0) {
            out->volume = (float) atof(value);
        } else if (strcmp(key, "last_track") == 0) {
            snprintf(out->last_track, sizeof(out->last_track), "%s", value);
        } else if (strcmp(key, "last_position") == 0) {
            out->last_position = atof(value);
        } else if (strcmp(key, "auto_resume") == 0) {
            out->auto_resume_enabled = (strcmp(value, "1") == 0);
        } else if (strcmp(key, "accent_color") == 0) {
            out->accent_color = (uint32_t) strtoul(value, NULL, 16);
        } else if (strcmp(key, "crossfade") == 0) {
            out->crossfade_enabled = (strcmp(value, "1") == 0);
        } else if (strcmp(key, "car_mode_enabled") == 0) {
            out->car_mode_enabled = (strcmp(value, "1") == 0);
        } else if (strcmp(key, "subsonic_url") == 0) {
            snprintf(out->subsonic_url, sizeof(out->subsonic_url), "%s", value);
        } else if (strcmp(key, "subsonic_username") == 0) {
            snprintf(out->subsonic_username, sizeof(out->subsonic_username), "%s", value);
        } else if (strcmp(key, "subsonic_password") == 0) {
            snprintf(out->subsonic_password, sizeof(out->subsonic_password), "%s", value);
        } else if (strcmp(key, "subsonic_verify_tls") == 0) {
            out->subsonic_verify_tls = (strcmp(value, "1") == 0);
        } else if (strcmp(key, "bt_volume_sync") == 0) {
            out->bt_volume_sync_enabled = (strcmp(value, "1") == 0);
        } else if (strcmp(key, "bt_dac_mode") == 0) {
            out->bt_dac_mode_enabled = (strcmp(value, "1") == 0);
        } else if (strcmp(key, "bt_codec") == 0) {
            snprintf(out->bt_codec, sizeof(out->bt_codec), "%s", value);
        } else if (strcmp(key, "bt_hide_unnamed_devices") == 0) {
            out->bt_hide_unnamed_devices = (strcmp(value, "1") == 0);
        } else if (strcmp(key, "wifi_dac_mode") == 0) {
            out->wifi_dac_mode_enabled = (strcmp(value, "1") == 0);
        } else if (strcmp(key, "dlna_renderer_enabled") == 0) {
            out->dlna_renderer_enabled = (strcmp(value, "1") == 0);
        } else if (strcmp(key, "screen_timeout_enabled") == 0) {
            out->screen_timeout_enabled = (strcmp(value, "1") == 0);
        } else if (strcmp(key, "screen_timeout_seconds") == 0) {
            out->screen_timeout_seconds = atoi(value);
        } else if (strcmp(key, "led_indicator_enabled") == 0) {
            out->led_indicator_enabled = (strcmp(value, "1") == 0);
        } else if (strcmp(key, "charge_limiter_enabled") == 0) {
            out->charge_limiter_enabled = (strcmp(value, "1") == 0);
        } else if (strcmp(key, "idle_shutdown_enabled") == 0) {
            out->idle_shutdown_enabled = (strcmp(value, "1") == 0);
        } else if (strcmp(key, "idle_shutdown_minutes") == 0) {
            out->idle_shutdown_minutes = atoi(value);
        } else if (strcmp(key, "idle_suspend_enabled") == 0) {
            out->idle_suspend_enabled = (strcmp(value, "1") == 0);
        } else if (strcmp(key, "usb_mode") == 0) {
            out->usb_mode = atoi(value);
        } else if (strcmp(key, "play_mode") == 0) {
            out->play_mode = atoi(value);
        } else if (strcmp(key, "swipe_up_home_enabled") == 0) {
            out->swipe_up_home_enabled = (strcmp(value, "1") == 0);
        } else if (strcmp(key, "startup_volume_fixed_enabled") == 0) {
            out->startup_volume_fixed_enabled = (strcmp(value, "1") == 0);
        } else if (strcmp(key, "startup_volume_fixed_percent") == 0) {
            out->startup_volume_fixed_percent = atoi(value);
        } else if (strcmp(key, "sleep_timer_minutes") == 0) {
            out->sleep_timer_minutes = atoi(value);
        } else if (strcmp(key, "timezone") == 0) {
            snprintf(out->timezone, sizeof(out->timezone), "%s", value);
        } else if (strcmp(key, "font_size_tier") == 0) {
            out->font_size_tier = atoi(value);
        }
    }

    if (out->usb_mode < 0 || out->usb_mode > 2) out->usb_mode = 0; /* defensive re-clamp, same reasoning as screen_timeout_seconds -- the settings file is plaintext and could be hand-edited out of range */
    if (out->play_mode < 0 || out->play_mode > 3) out->play_mode = 0;
    if (out->font_size_tier < 0 || out->font_size_tier > 2) out->font_size_tier = 0;
    if (out->startup_volume_fixed_percent < 0 || out->startup_volume_fixed_percent > 100) out->startup_volume_fixed_percent = 20;

    out->screen_timeout_seconds = nearest_screen_timeout_step(out->screen_timeout_seconds);
    out->idle_shutdown_minutes = nearest_idle_shutdown_step(out->idle_shutdown_minutes);
    out->sleep_timer_minutes = nearest_sleep_timer_step(out->sleep_timer_minutes);

    fclose(f);
    return true;
}

/* Real-device bug reports: settings (most visibly volume, since hardware
 * volume buttons call this on every single press -- see gui.c's
 * update_timer_cb) reset back to defaults after a reboot on some devices,
 * inconsistently. Root cause: this used to just fclose() the tmp file and
 * rename() it over the real one, with no fsync anywhere -- fclose() only
 * flushes stdio's own userspace buffer into the kernel page cache, it
 * doesn't force that page to actual flash, and neither does rename(). This
 * device's UBIFS partition has been separately confirmed (see TESTING.md's
 * own note on losing a file deletion across an unclean shutdown) to drop
 * recently-written-but-not-yet-committed metadata across anything other than
 * a clean shutdown -- and a clean, UI-driven shutdown isn't how most users of
 * a physical-button DAP actually power one of these off; holding the power
 * button is. That made this a routine case, not a rare edge one, and volume
 * being saved (and lost) more often than anything else made it the most
 * visibly broken setting even though every field here was equally at risk.
 * fsync() on the tmp file's own fd before close ensures its *contents* are
 * durable; a rename() being applied is itself just a directory-metadata
 * change, so the containing directory's own fd needs its own fsync()
 * afterward for the rename itself to survive an unclean shutdown -- the
 * standard atomic-durable-replace recipe (write tmp -> fsync tmp -> rename
 * -> fsync directory). */
static void fsync_settings_dir(void) {
    int dir_fd = open(SETTINGS_DIR_PATH, O_RDONLY);
    if (dir_fd < 0) return;
    fsync(dir_fd);
    close(dir_fd);
}

void settings_save(const player_settings_t * settings) {
    DBG_LOG("settings_save: called (idle_suspend_enabled=%d)\n", settings->idle_suspend_enabled ? 1 : 0);
    FILE * f = fopen(SETTINGS_TMP_FILE_PATH, "w");
    if (!f) return;

    fprintf(f, "volume=%.3f\n", (double) settings->volume);
    fprintf(f, "last_track=%s\n", settings->last_track);
    fprintf(f, "last_position=%.3f\n", settings->last_position);
    fprintf(f, "auto_resume=%d\n", settings->auto_resume_enabled ? 1 : 0);
    fprintf(f, "accent_color=%06X\n", (unsigned int) (settings->accent_color & 0xFFFFFF));
    fprintf(f, "crossfade=%d\n", settings->crossfade_enabled ? 1 : 0);
    fprintf(f, "car_mode_enabled=%d\n", settings->car_mode_enabled ? 1 : 0);
    fprintf(f, "subsonic_url=%s\n", settings->subsonic_url);
    fprintf(f, "subsonic_username=%s\n", settings->subsonic_username);
    fprintf(f, "subsonic_password=%s\n", settings->subsonic_password);
    fprintf(f, "subsonic_verify_tls=%d\n", settings->subsonic_verify_tls ? 1 : 0);
    fprintf(f, "bt_volume_sync=%d\n", settings->bt_volume_sync_enabled ? 1 : 0);
    fprintf(f, "bt_dac_mode=%d\n", settings->bt_dac_mode_enabled ? 1 : 0);
    fprintf(f, "bt_codec=%s\n", settings->bt_codec);
    fprintf(f, "bt_hide_unnamed_devices=%d\n", settings->bt_hide_unnamed_devices ? 1 : 0);
    fprintf(f, "wifi_dac_mode=%d\n", settings->wifi_dac_mode_enabled ? 1 : 0);
    fprintf(f, "dlna_renderer_enabled=%d\n", settings->dlna_renderer_enabled ? 1 : 0);
    fprintf(f, "screen_timeout_enabled=%d\n", settings->screen_timeout_enabled ? 1 : 0);
    fprintf(f, "screen_timeout_seconds=%d\n", settings->screen_timeout_seconds);
    fprintf(f, "led_indicator_enabled=%d\n", settings->led_indicator_enabled ? 1 : 0);
    fprintf(f, "charge_limiter_enabled=%d\n", settings->charge_limiter_enabled ? 1 : 0);
    fprintf(f, "idle_shutdown_enabled=%d\n", settings->idle_shutdown_enabled ? 1 : 0);
    fprintf(f, "idle_shutdown_minutes=%d\n", settings->idle_shutdown_minutes);
    fprintf(f, "idle_suspend_enabled=%d\n", settings->idle_suspend_enabled ? 1 : 0);
    fprintf(f, "usb_mode=%d\n", settings->usb_mode);
    fprintf(f, "play_mode=%d\n", settings->play_mode);
    fprintf(f, "swipe_up_home_enabled=%d\n", settings->swipe_up_home_enabled ? 1 : 0);
    fprintf(f, "startup_volume_fixed_enabled=%d\n", settings->startup_volume_fixed_enabled ? 1 : 0);
    fprintf(f, "startup_volume_fixed_percent=%d\n", settings->startup_volume_fixed_percent);
    fprintf(f, "sleep_timer_minutes=%d\n", settings->sleep_timer_minutes);
    fprintf(f, "timezone=%s\n", settings->timezone);
    fprintf(f, "font_size_tier=%d\n", settings->font_size_tier);

    fflush(f);
    fsync(fileno(f));
    fclose(f);
    rename(SETTINGS_TMP_FILE_PATH, SETTINGS_FILE_PATH);
    fsync_settings_dir();
}

void settings_factory_reset(void) {
    DBG_LOG("settings_factory_reset: called\n");
#if defined(TEST_BUILD_TAG)
    int rc1 = remove(SETTINGS_FILE_PATH);
    DBG_LOG("settings_factory_reset: remove(%s) rc=%d errno=%d\n", SETTINGS_FILE_PATH, rc1, rc1 == 0 ? 0 : errno);
    int rc2 = remove(SETTINGS_TMP_FILE_PATH); /* stray leftover from an interrupted settings_save(), if any -- harmless to attempt even when it doesn't exist */
    DBG_LOG("settings_factory_reset: remove(%s) rc=%d errno=%d\n", SETTINGS_TMP_FILE_PATH, rc2, rc2 == 0 ? 0 : errno);
#else
    remove(SETTINGS_FILE_PATH);
    remove(SETTINGS_TMP_FILE_PATH); /* stray leftover from an interrupted settings_save(), if any -- harmless to attempt even when it doesn't exist */
#endif

    /* Reboots immediately, same as idle_shutdown_now()'s /sbin/poweroff and
     * firmware_update_enter_recovery()'s /sbin/reboot -- this function owns
     * the whole destructive action end-to-end rather than leaving the
     * reboot to whatever UI code called it, matching both of those. See
     * this function's own doc comment in settings.h for why a reboot,
     * not a live re-apply, is how the reset settings actually take
     * effect. subprocess_run() just fails to find /sbin/reboot on the host
     * build (same as idle_shutdown_now()'s own host-build comment) --
     * harmless there, so this isn't guarded behind #ifndef HOST_BUILD. */
    char * reboot_argv[] = { (char *) "/sbin/reboot", NULL };
    subprocess_run(reboot_argv, NULL, 0);
    DBG_LOG("settings_factory_reset: subprocess_run(/sbin/reboot) returned, still alive\n");
}
