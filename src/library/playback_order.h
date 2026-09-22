#ifndef PLAYBACK_ORDER_H
#define PLAYBACK_ORDER_H
#include <stdbool.h>
#include <stdlib.h>
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

/* Drop one physical slot from a bag whose length matches the playlist.
 * Returns the removed bag index, or -1 without mutating on a stale bag. */
static inline int playback_order_note_removed(int *order, int *order_count, int *pos,
                                             int physical, int playlist_count) {
    if (!order || !order_count || *order_count != playlist_count || playlist_count <= 0) return -1;
    int removed = playback_order_remove(order, playlist_count, physical);
    if (removed < 0) return -1;
    if (pos && removed <= *pos) (*pos)--;
    (*order_count)--;
    return removed;
}

/* Move path pointers and optional ranks into display order. Pointers are
 * moved, not copied. Fails without writing either output when a display
 * index is repeated or allocation fails. */
static inline bool playback_order_resequence(const int *display, int count, char **paths,
                                            const int *ranks, char ***out_paths, int **out_ranks) {
    if (!out_paths || !out_ranks || count < 0) return false;
    *out_paths = NULL;
    *out_ranks = NULL;
    if (count == 0) return true;
    if (!paths) return false;
    unsigned char *seen = display ? calloc((size_t) count, 1) : NULL;
    char **next_paths = calloc((size_t) count, sizeof(*next_paths));
    int *next_ranks = ranks ? malloc((size_t) count * sizeof(*next_ranks)) : NULL;
    if ((display && !seen) || !next_paths || (ranks && !next_ranks)) {
        free(seen);
        free(next_paths);
        free(next_ranks);
        return false;
    }
    for (int i = 0; i < count; i++) {
        int src = display ? display[i] : i;
        if (src < 0 || src >= count || (seen && seen[src])) {
            free(seen);
            free(next_paths);
            free(next_ranks);
            return false;
        }
        if (seen) seen[src] = 1;
        next_paths[i] = paths[src];
        if (next_ranks) next_ranks[i] = ranks[src];
    }
    free(seen);
    *out_paths = next_paths;
    *out_ranks = next_ranks;
    return true;
}
#endif
