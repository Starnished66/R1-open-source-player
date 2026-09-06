#ifndef AIRPLAY_CONTROL_H
#define AIRPLAY_CONTROL_H

#include <stdbool.h>

/* AirPlay (1) receive mode via the stock firmware's /usr/bin/shairport binary.
 * Outputs PCM audio to AIRPLAY_FIFO_PATH (-o pipe), read by airplay_bridge.c/h
 * and written to this app's audio_output. Embedded mDNS responder (tinysvcmdns)
 * handles discovery. */

/* Starts shairport as a background daemon, advertised under `device_name`,
 * and starts the FIFO bridge (airplay_bridge_start()). Recreates AIRPLAY_FIFO_PATH
 * before launching. If mkfifo() or airplay_bridge_start() fails, shairport is not
 * started and false is returned. Returns false on spawn failure. */
bool airplay_control_start(const char * device_name);

/* Kills shairport, stops the bridge thread, and removes the FIFO node. */
void airplay_control_stop(void);

/* Returns true if shairport and the bridge are currently running. */
bool airplay_control_is_active(void);

/* If an AirPlay stream is actively playing, disconnects it by stopping and
 * restarting shairport, yielding the audio output back to local playback while
 * remaining discoverable. Returns true if a stream was active and interrupted. */
bool airplay_control_disconnect_active_stream(void);

#endif /* AIRPLAY_CONTROL_H */
