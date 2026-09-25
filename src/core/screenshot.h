#ifndef SCREENSHOT_H
#define SCREENSHOT_H

#include <stdbool.h>
#include <stddef.h>

#define SCREENSHOT_PATH_MAX 512
#define SCREENSHOT_REASON_MAX 48

typedef enum {
    SCREENSHOT_RESULT_STARTED = 0,
    SCREENSHOT_RESULT_SCREEN_OFF,
    SCREENSHOT_RESULT_NO_CARD,
    SCREENSHOT_RESULT_USB_STORAGE,
    SCREENSHOT_RESULT_BUSY,
    SCREENSHOT_RESULT_FRAMEBUFFER,
    SCREENSHOT_RESULT_WORKER_START,
    SCREENSHOT_RESULT_UNAVAILABLE,
} screenshot_result_t;

typedef struct {
    bool saved;
    char path[SCREENSHOT_PATH_MAX];
    char reason[SCREENSHOT_REASON_MAX];
} screenshot_completion_t;

/* Call from the UI thread. Copies the currently visible framebuffer before
 * starting its PNG worker. Start failures that should be visible show an
 * error toast here; screen-off and busy requests are silent. */
screenshot_result_t screenshot_start(void);
const char * screenshot_result_reason(screenshot_result_t result);

/* Poll from the UI thread. Completion is consumed once, and the service is
 * no longer busy after this returns true. */
bool screenshot_poll_completion(screenshot_completion_t * out);
bool screenshot_is_busy(void);

#endif /* SCREENSHOT_H */
