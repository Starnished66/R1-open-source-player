#ifndef DEBUG_LOG_H
#define DEBUG_LOG_H

#include <stdio.h>

/* Diagnostic logging gated on TEST_BUILD_TAG. Production builds compile
 * these out entirely. Some messages fire repeatedly in tight loops
 * (e.g. audio reconnect), so gating avoids unnecessary flash writes
 * on deployed hardware where there is no reader for stderr output. */
#if defined(TEST_BUILD_TAG)
#define DBG_LOG(...) fprintf(stderr, __VA_ARGS__)
#else
#define DBG_LOG(...) ((void) 0)
#endif

#endif /* DEBUG_LOG_H */
