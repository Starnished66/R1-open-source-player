#ifndef AIRPLAY_CONTROL_H
#define AIRPLAY_CONTROL_H

/* AirPlay (1) receive mode via the stock firmware's /usr/bin/shairport
 * binary. "-o ot" selects the Ingenic-specific output backend, "-M" sets
 * the metadata/cover-art dump directory, "-b 160" is the playback buffer
 * fill threshold in frames. shairport has its own embedded mDNS responder
 * (tinysvcmdns), so no separate Avahi dependency is needed. */

/* Starts shairport as a background daemon, advertised under `device_name`.
 * Blocking only for the brief process-spawn itself; call off the UI thread
 * isn't required but kept consistent with everything else here. */
void airplay_control_start(const char * device_name);

void airplay_control_stop(void);

#endif /* AIRPLAY_CONTROL_H */
