#ifndef PLAYBACK_ORDER_H
#define PLAYBACK_ORDER_H
#include <string.h>

/* Remap a permutation after physical slots are inserted. The caller has
 * reserved count + added integers. This preserves the existing shuffle bag. */
static inline void playback_order_insert(int * order, int count, int physical, int at, int added) {
    for (int i = 0; i < count; i++) if (order[i] >= physical) order[i] += added;
    memmove(order + at + added, order + at, (size_t) (count - at) * sizeof(*order));
    for (int i = 0; i < added; i++) order[at + i] = physical + i;
}

static inline int playback_order_remove(int * order, int count, int physical) {
    int removed = -1;
    for (int i = 0; i < count; i++) if (order[i] == physical) { removed = i; break; }
    if (removed < 0) return -1;
    memmove(order + removed, order + removed + 1, (size_t) (count - removed - 1) * sizeof(*order));
    for (int i = 0; i < count - 1; i++) if (order[i] > physical) order[i]--;
    return removed;
}
#endif
