#ifndef DEBUG_LOG_H
#define DEBUG_LOG_H

#include <stdio.h>

/* Real-device incident: this app's own stderr diagnostic breadcrumbs
 * (Bluetooth/audio subsystem state changes, wedge-recovery attempts, ...)
 * are genuinely useful while actively debugging a live test build over ADB,
 * but have no reader at all in a real flashed deployment (nothing in
 * hiby_player.sh/S92_03_start_music_player redirects stdout/stderr
 * anywhere persistent) -- and if anything ever DOES end up capturing them
 * (a future init script change, a user's own debugging), some of these fire
 * repeatedly in a tight loop (e.g. audio_output.c's aplay-died-and-reopened
 * message during a rough Bluetooth reconnect), which is real, avoidable
 * wear on this device's limited flash storage for a real end user who will
 * never read them. Gated on TEST_BUILD_TAG -- the same flag that already
 * distinguishes a live test build (see Makefile's own `make target
 * TEST_BUILD_TAG=...` convention) from a real flash deployment -- so
 * production builds compile these out entirely rather than just suppress
 * them at runtime. */
#if defined(TEST_BUILD_TAG)
#define DBG_LOG(...) fprintf(stderr, __VA_ARGS__)
#else
#define DBG_LOG(...) ((void) 0)
#endif

#endif /* DEBUG_LOG_H */
