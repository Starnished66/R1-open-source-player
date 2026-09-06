#ifndef AIRPLAY_BRIDGE_H
#define AIRPLAY_BRIDGE_H

#include <stdbool.h>

/* Bridges shairport's "-o pipe" output into the shared audio_output module.
 * AirPlay 1/RAOP provides raw interleaved 16-bit signed-LE stereo PCM at 44100 Hz. */

/* Named FIFO shairport's "-o pipe -- AIRPLAY_FIFO_PATH" writes raw PCM to.
 * airplay_control_start() (re)creates this node before each shairport
 * spawn; this bridge only ever opens it for reading. */
#define AIRPLAY_FIFO_PATH "/tmp/airplay_audio.fifo"

/* Starts the bridge thread in LISTENING state. The thread waits for shairport
 * to open the FIFO and deliver PCM bytes before claiming the audio output device
 * and interrupting local playback. Reopens the FIFO and returns to LISTENING
 * across writer disconnects. Returns true on success, or false if thread creation
 * fails. */
bool airplay_bridge_start(void);

/* True only while shairport is actively delivering PCM data. Polled by the UI
 * to decide whether to display the AirPlay overlay. */
bool airplay_bridge_is_streaming(void);

/* Signals the bridge thread to stop asynchronously. The thread closes the FIFO,
 * releases the audio output, and transitions to BRIDGE_STOPPED. */
void airplay_bridge_stop(void);

/* True once the bridge thread has fully stopped and released audio_output.
 * Used to synchronously wait for the device to be freed before resuming local playback. */
bool airplay_bridge_is_stopped(void);

#endif /* AIRPLAY_BRIDGE_H */
