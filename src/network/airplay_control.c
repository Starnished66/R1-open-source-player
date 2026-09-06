#include "airplay_control.h"
#include "airplay_bridge.h"
#include "airplay_metadata.h"
#include "subprocess.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Captured on every airplay_control_start() so airplay_control_disconnect_
 * active_stream() can respawn shairport under the same advertised name
 * without its own caller (gui_player.c, which has no reason to know or
 * care what this device calls itself over AirPlay) needing to plumb it
 * through. */
static char last_device_name[64] = "";

/* Tracks whether AirPlay daemon and bridge are active. */
static bool control_active = false;

bool airplay_control_is_active(void) {
    return control_active;
}

bool airplay_control_start(const char * device_name) {
    snprintf(last_device_name, sizeof(last_device_name), "%s", device_name);

    /* Recreate the audio FIFO and clean up any stale now-playing FIFO. */
    unlink(AIRPLAY_FIFO_PATH);
    if (mkfifo(AIRPLAY_FIFO_PATH, 0666) != 0) {
        fprintf(stderr, "airplay_control: mkfifo(%s) failed -- AirPlay audio will not work this session\n",
                AIRPLAY_FIFO_PATH);
        control_active = false;
        return false;
    }
    unlink(AIRPLAY_NOW_PLAYING_PATH);

    if (!airplay_bridge_start()) {
        control_active = false;
        return false;
    }

    /* Metadata is non-essential; startup proceeds even if metadata reader fails. */
    if (!airplay_metadata_start()) {
        fprintf(stderr, "airplay_control: metadata reader failed to start -- AirPlay audio will still work, "
                        "no track info will be shown\n");
    }

    char * argv[] = { (char *) "shairport", (char *) "-a", (char *) device_name,
                       (char *) "-M", (char *) "/tmp", (char *) "-b", (char *) "160",
                       (char *) "-l", (char *) "/tmp/shairport.log",
                       (char *) "-o", (char *) "pipe",
                       (char *) "--", (char *) AIRPLAY_FIFO_PATH, NULL };
    if (!subprocess_spawn_daemon(argv)) {
        fprintf(stderr, "airplay_control: failed to spawn shairport -- AirPlay will not be discoverable this session\n");
        /* Roll back started threads and FIFO if shairport spawn fails. */
        airplay_bridge_stop();
        airplay_metadata_stop();
        unlink(AIRPLAY_FIFO_PATH);
        control_active = false;
        return false;
    }

    control_active = true;
    return true;
}

void airplay_control_stop(void) {
    char * argv[] = { (char *) "killall", (char *) "shairport", NULL };
    subprocess_run(argv, NULL, 0);
    airplay_bridge_stop();
    airplay_metadata_stop();
    unlink(AIRPLAY_FIFO_PATH);
    control_active = false;
}

bool airplay_control_disconnect_active_stream(void) {
    if (!airplay_bridge_is_streaming()) return false;
    /* last_device_name is only ever empty if this is called before AirPlay
     * was ever started at all this run, in which case airplay_bridge_is_
     * streaming() above could not have been true either -- defensive, not
     * a real expected path. */
    if (!last_device_name[0]) return false;

    /* Copy to a local buffer before stopping and restarting. */
    char device_name[sizeof(last_device_name)];
    snprintf(device_name, sizeof(device_name), "%s", last_device_name);

    airplay_control_stop();

    /* Wait for the bridge thread to release audio output before handing it
     * back to local playback, bounded to 2 seconds. */
    bool bridge_stopped = false;
    for (int waited_ms = 0; waited_ms < 2000; waited_ms += 20) {
        if (airplay_bridge_is_stopped()) {
            bridge_stopped = true;
            break;
        }
        usleep(20000);
    }
    if (!bridge_stopped) {
        fprintf(stderr, "airplay_control: bridge did not report stopped within timeout -- "
                        "proceeding to resume local playback anyway\n");
    }

    airplay_control_start(device_name);
    return true;
}
