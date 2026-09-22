#include "sd_card_identity.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    dev_t known = (dev_t) 17;

    /* A failed second mount stat reports no device, never device zero. */
    assert(!sd_card_identity_replaced(true, true, true, true, known,
                                      false, 0, "CID-A", "CID-A"));
    dev_t baseline = known;
    assert(sd_card_remember_mounted_device(false, 0, false, true,
                                           &baseline, true));
    assert(baseline == known);

    /* The same card remains stable after the mount stat recovers. */
    assert(!sd_card_identity_replaced(true, true, true, true, known,
                                      true, known, "CID-A", "CID-A"));

    /* A verified device or CID change still identifies a real replacement. */
    assert(sd_card_identity_replaced(true, true, true, true, known,
                                     true, (dev_t) 18, "CID-A", "CID-A"));
    assert(sd_card_identity_replaced(true, true, true, true, known,
                                     false, 0, "CID-A", "CID-B"));

    /* After confirmed removal, an empty CID must not rearm the old CID-A.
     * A known device still provides a useful baseline while CID reads fail. */
    assert(!sd_card_identity_is_known(false, ""));
    assert(!sd_card_identity_is_known(false, NULL));
    assert(sd_card_identity_is_known(true, ""));
    assert(sd_card_identity_is_known(false, "CID-B"));
    assert(!sd_card_identity_replaced(true, true, false, false, 0,
                                      false, 0, "CID-A", "CID-B"));

    /* A failed first mount observation followed by a successful retry is
     * inconclusive and must take the ordinary two-poll debounce path. */
    assert(!sd_card_stale_mount_confirmed(false, false));
    assert(!sd_card_stale_mount_confirmed(false, true));
    assert(sd_card_stale_mount_confirmed(true, false));

    puts("PASS: transient mount and identity uncertainty do not cause false swaps");
    return 0;
}
