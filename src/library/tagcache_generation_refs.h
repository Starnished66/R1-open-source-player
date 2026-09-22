#ifndef HIBY_TAGCACHE_GENERATION_REFS_H
#define HIBY_TAGCACHE_GENERATION_REFS_H

#include <stdint.h>

/* A generation may reuse an unchanged persisted tag file from an older
 * generation.  References are deliberately flat: a source always names the
 * physical generation containing the file, never another refs record. */
#define TAGCACHE_REFS_MAGIC 0x54435231u /* TCR1 */
#define TAGCACHE_REFS_VERSION 1u
#define TAGCACHE_REFS_COUNT 10u

struct tagcache_generation_refs {
    uint32_t magic;
    uint32_t version;
    int32_t generation;
    int32_t source[TAGCACHE_REFS_COUNT];
    uint32_t checksum;
};

#endif
