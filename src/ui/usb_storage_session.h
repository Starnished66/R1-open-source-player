#ifndef USB_STORAGE_SESSION_H
#define USB_STORAGE_SESSION_H

#include <stdbool.h>

typedef struct {
    bool host_seen;
} usb_storage_session_t;

/* Latch host enumeration for the whole cable session. Controller state can
 * disappear before the unplug poll, or change during suspend/mode switches.
 * Returns true exactly once when a confirmed Storage session is unplugged.
 * A selected/bound gadget alone is not evidence of a computer connection. */
static inline bool usb_storage_session_poll(usb_storage_session_t * session,
                                            bool connected, bool storage_configured) {
    if (connected) {
        session->host_seen |= storage_configured;
        return false;
    }
    bool ended = session->host_seen;
    session->host_seen = false;
    return ended;
}

#endif
