#include "catalog_source_cache.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define CATALOG_SOURCE_CACHE_REVISION_MAX (CATALOG_SOURCE_CACHE_REVISION_SIZE - 1)

typedef struct {
    bool valid;
    int64_t representative_id;
    uint64_t last_used;
    catalog_source_resolution_t resolution;
} catalog_source_cache_entry_t;

static pthread_mutex_t source_cache_mutex = PTHREAD_MUTEX_INITIALIZER;
static catalog_source_cache_entry_t source_cache[CATALOG_SOURCE_CACHE_CAPACITY];
static char source_cache_revision[CATALOG_SOURCE_CACHE_REVISION_SIZE];
static bool source_cache_revision_valid;
static uint64_t source_cache_clock;

static void source_cache_invalidate_locked(void) {
    memset(source_cache, 0, sizeof(source_cache));
    source_cache_clock = 0;
}

void catalog_source_cache_reset(void) {
    pthread_mutex_lock(&source_cache_mutex);
    source_cache_invalidate_locked();
    source_cache_revision[0] = '\0';
    source_cache_revision_valid = false;
    pthread_mutex_unlock(&source_cache_mutex);
}

bool catalog_source_cache_resolve(const char * revision, int64_t representative_id,
                                  catalog_source_cache_loader_t loader, void * context,
                                  catalog_source_resolution_t * out_resolution) {
    if (!revision || !revision[0] || strnlen(revision, CATALOG_SOURCE_CACHE_REVISION_SIZE) >
        CATALOG_SOURCE_CACHE_REVISION_MAX || representative_id <= 0 || !loader || !out_resolution)
        return false;

    pthread_mutex_lock(&source_cache_mutex);
    if (!source_cache_revision_valid || strcmp(source_cache_revision, revision) != 0) {
        source_cache_invalidate_locked();
        snprintf(source_cache_revision, sizeof(source_cache_revision), "%s", revision);
        source_cache_revision_valid = true;
    }

    for (size_t i = 0; i < CATALOG_SOURCE_CACHE_CAPACITY; i++) {
        if (source_cache[i].valid && source_cache[i].representative_id == representative_id) {
            source_cache[i].last_used = ++source_cache_clock;
            *out_resolution = source_cache[i].resolution;
            pthread_mutex_unlock(&source_cache_mutex);
            return true;
        }
    }

    catalog_source_resolution_t resolution;
    memset(&resolution, 0, sizeof(resolution));
    if (!loader(context, &resolution) || !resolution.cover_key[0] ||
        (resolution.has_sidecar && !resolution.sidecar_path[0])) {
        pthread_mutex_unlock(&source_cache_mutex);
        return false;
    }

    size_t slot = CATALOG_SOURCE_CACHE_CAPACITY;
    uint64_t oldest = UINT64_MAX;
    for (size_t i = 0; i < CATALOG_SOURCE_CACHE_CAPACITY; i++) {
        if (!source_cache[i].valid) {
            slot = i;
            break;
        }
        if (source_cache[i].last_used < oldest) {
            oldest = source_cache[i].last_used;
            slot = i;
        }
    }
    source_cache[slot].valid = true;
    source_cache[slot].representative_id = representative_id;
    source_cache[slot].last_used = ++source_cache_clock;
    source_cache[slot].resolution = resolution;
    *out_resolution = resolution;
    pthread_mutex_unlock(&source_cache_mutex);
    return true;
}
