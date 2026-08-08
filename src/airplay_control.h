#ifndef AIRPLAY_CONTROL_H
#define AIRPLAY_CONTROL_H

/* AirPlay (1) receive mode -- reuses the stock firmware's own /usr/bin/
 * shairport binary (a vendor build of the well-known open-source AirPlay
 * receiver, confirmed on a real device to be directly wired up in
 * hiby_player itself -- it references airplay_turn_on/airplay_turn_off and
 * has a full dedicated now-playing screen -- so unlike Bluetooth's a2dp-sink
 * path, this one is a genuinely shipped, tested feature, not a latent
 * unused library capability). Invocation confirmed directly from
 * /usr/bin/shairport_on.sh and `shairport --help` on a real device:
 * `shairport -a <name> -o ot -M /tmp -b 160` -- "ot" is a custom
 * Ingenic-specific output backend (a real string in the binary, not a
 * standard shairport backend name), -M is a metadata/cover-art dump
 * directory, -b is the playback buffer fill threshold in frames. shairport
 * has its own embedded mDNS responder (tinysvcmdns), so no separate Avahi
 * dependency is needed. */

/* Starts shairport as a background daemon, advertised under `device_name`.
 * Blocking only for the brief process-spawn itself; call off the UI thread
 * isn't required but kept consistent with everything else here. */
void airplay_control_start(const char * device_name);

void airplay_control_stop(void);

#endif /* AIRPLAY_CONTROL_H */
