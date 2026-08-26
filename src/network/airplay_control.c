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

/* True only across a fully successful airplay_control_start() until the
 * next airplay_control_stop() (or a later start's own failure) -- lets
 * airplay_control_is_active() detect a restart that failed OUTSIDE the
 * normal toggle path (airplay_control_disconnect_active_stream() doesn't
 * itself have access to current_settings/show_error_toast, see its own
 * comment) so the UI layer can notice and self-correct on its own next
 * poll, same as it already would for a failure on the toggle path itself. */
static bool control_active = false;

bool airplay_control_is_active(void) {
    return control_active;
}

bool airplay_control_start(const char * device_name) {
    snprintf(last_device_name, sizeof(last_device_name), "%s", device_name);

    /* Unlink first rather than relying on mkfifo()'s own EEXIST: a leftover
     * node from a previous run/crash could in principle be some other file
     * type at this path, and unconditionally starting from a fresh node
     * costs two cheap syscalls either way. AIRPLAY_NOW_PLAYING_PATH is NOT
     * created here -- shairport itself creates that FIFO on startup (inside
     * its own "-M" meta-dir, confirmed via `strings`); unlinking any stale
     * leftover first is still worth doing defensively, same reasoning as
     * the audio FIFO.
     *
     * mkfifo() failing means shairport would have nothing to write PCM to
     * even if it spawned fine, so there is no point spawning it at all --
     * bail out here rather than advertising a device that can never
     * produce sound. */
    unlink(AIRPLAY_FIFO_PATH);
    if (mkfifo(AIRPLAY_FIFO_PATH, 0666) != 0) {
        fprintf(stderr, "airplay_control: mkfifo(%s) failed -- AirPlay audio will not work this session\n",
                AIRPLAY_FIFO_PATH);
        control_active = false;
        return false;
    }
    unlink(AIRPLAY_NOW_PLAYING_PATH);

    if (!airplay_bridge_start()) {
        /* No reader thread means nothing would ever drain the FIFO -- same
         * "not worth advertising" reasoning as the mkfifo() failure above. */
        control_active = false;
        return false;
    }

    /* Metadata is non-essential -- audio still works with no track info
     * displayed, so a failure here is logged only and startup proceeds. */
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
        /* Roll back whichever of the bridge/metadata threads did start --
         * otherwise they'd sit running forever with no shairport process to
         * ever feed them. */
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

    /* Copy to a local buffer before stopping/restarting: airplay_control_
     * start() below does snprintf(last_device_name, ..., "%s", device_name),
     * and passing last_device_name itself as device_name would make source
     * and destination the same buffer -- undefined behavior on an
     * overlapping snprintf(), not just a style nit. */
    char device_name[sizeof(last_device_name)];
    snprintf(device_name, sizeof(device_name), "%s", last_device_name);

    airplay_control_stop();

    /* airplay_bridge_stop() is fire-and-forget (see its own comment) -- the
     * dying bridge thread can still be mid audio_output_ensure()/_write() on
     * the shared device for up to roughly its own poll interval afterward.
     * Unlike a plain settings-screen toggle-off, THIS caller's caller is
     * about to hand the device straight to local playback the moment this
     * function returns (gui_player.c's play_track_at_from_internal()/
     * toggle_play_pause()), so there is something here that actually needs
     * the device back before proceeding -- wait for airplay_bridge_is_
     * stopped(), bounded so a stuck bridge can never hang local playback
     * indefinitely. Same order of magnitude and the same justification as
     * the bridge's own symmetric wait when it takes the device the OTHER
     * direction (run_session()'s up to 2000ms waiting for audio_is_playing()
     * /_is_paused() to clear) -- not a redesign of audio_output.c's lock-
     * free ownership model, just closing this one specific, synchronously-
     * reachable window using the same tool already used going the other
     * way. */
    bool bridge_stopped = false;
    for (int waited_ms = 0; waited_ms < 2000; waited_ms += 20) {
        if (airplay_bridge_is_stopped()) {
            bridge_stopped = true;
            break;
        }
        usleep(20000);
    }
    if (!bridge_stopped) {
        /* The bridge thread would have to be stuck for a full 2s past its
         * own ~50ms poll interval for this to trigger -- not expected in
         * practice, but this is a genuine residual race, not a guarantee:
         * local playback proceeds anyway rather than hanging indefinitely
         * on a wedged bridge, so it's still possible (if this ever actually
         * fires) for it to briefly race the still-retiring bridge thread's
         * last audio_output call. Logged rather than silently swallowed so
         * that exceptional case is at least diagnosable if it ever shows up
         * in the field. */
        fprintf(stderr, "airplay_control: bridge did not report stopped within timeout -- "
                        "proceeding to resume local playback anyway\n");
    }

    airplay_control_start(device_name);
    return true;
}
