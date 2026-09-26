#ifndef METADATA_REFRESH_TARGETS_H
#define METADATA_REFRESH_TARGETS_H

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

static bool metadata_refresh_should_skip_commit(int changed_count, bool row_deleted, bool row_inserted) {
    return changed_count == 0 && !row_deleted && !row_inserted;
}

static int metadata_refresh_target_path_compare(const void * a, const void * b) {
    const char * const * left = a;
    const char * const * right = b;
    return strcmp(*left, *right);
}

/* Copies, sorts, and removes duplicate paths. Returns the number of unique
 * owned strings, or zero on invalid input or allocation failure. */
static int metadata_refresh_copy_unique_paths(const char * const * paths, int count, char *** out_paths) {
    if (!out_paths) return 0;
    *out_paths = NULL;
    if (!paths || count <= 0) return 0;
    char **copied = calloc((size_t)count, sizeof(*copied));
    if (!copied) return 0;
    for (int i = 0; i < count; i++) {
        if (!paths[i] || !paths[i][0] || !(copied[i] = strdup(paths[i]))) {
            for (int j = 0; j < count; j++) free(copied[j]);
            free(copied);
            return 0;
        }
    }
    qsort(copied, (size_t)count, sizeof(*copied), metadata_refresh_target_path_compare);
    int unique = 0;
    for (int i = 0; i < count; i++) {
        if (unique > 0 && strcmp(copied[unique - 1], copied[i]) == 0) {
            free(copied[i]);
            continue;
        }
        copied[unique++] = copied[i];
    }
    *out_paths = copied;
    return unique;
}

#endif
