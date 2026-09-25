#ifndef CATALOG_SOURCE_CACHE_H
#define CATALOG_SOURCE_CACHE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 128 entries with a 600-byte path keep fixed storage below 96 KiB. */
#define CATALOG_SOURCE_CACHE_CAPACITY 128
#define CATALOG_SOURCE_CACHE_KEY_SIZE 64
#define CATALOG_SOURCE_CACHE_PATH_SIZE 600
#define CATALOG_SOURCE_CACHE_REVISION_SIZE 64

typedef struct {
    bool has_sidecar;
    char cover_key[CATALOG_SOURCE_CACHE_KEY_SIZE];
    char sidecar_path[CATALOG_SOURCE_CACHE_PATH_SIZE];
    int64_t stat_mtime;
    int64_t stat_size;
} catalog_source_resolution_t;

typedef bool (*catalog_source_cache_loader_t)(void * context,
                                               catalog_source_resolution_t * out_resolution);

/* Resolves each representative at most once while its revision remains in
 * the bounded cache. The loader runs under the cache mutex, so concurrent
 * misses cannot repeat a directory scan. Callers must release metadata DB
 * locks before invoking this function. */
bool catalog_source_cache_resolve(const char * revision, int64_t representative_id,
                                  catalog_source_cache_loader_t loader, void * context,
                                  catalog_source_resolution_t * out_resolution);

/* Clears the bounded cache. Exposed for focused self-tests and shutdown. */
void catalog_source_cache_reset(void);

#endif
