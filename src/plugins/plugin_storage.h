#ifndef PLUGIN_STORAGE_H
#define PLUGIN_STORAGE_H

#include <stdbool.h>
#include <stddef.h>

/* Namespaced per-plugin key/value files on the internal partition
 * (/usr/data/plugins on target, never the SD card). plugin.storage and
 * plugin.secrets share this tree; hashed directory/file names keep a
 * plugin id + key from becoming a path component.
 *
 * Threat model: this device has no hardware keystore. "Secrets" means
 * owner-only 0600/0700 files plus plugin_storage_path_is_reserved() on
 * every Lua io/os path and every native plugin.* path argument. That
 * guard is the real isolation: every plugin runs in the same process
 * and user, so file mode bits alone would not stop io.open() of another
 * plugin's secrets file. Neither layer defends against root or physical
 * access. This is not encryption.
 *
 * Quotas (combined storage + secrets per plugin): 256 KiB per value,
 * 500 keys, 2 MiB; 8 MiB across all plugins. Tracked in an in-memory
 * cache seeded from a directory scan on first touch. */

bool plugin_storage_path_is_reserved(const char * path);

bool plugin_storage_get(const char * plugin_id, const char * key, char ** out_value, size_t * out_len);
bool plugin_storage_set(const char * plugin_id, const char * key, const char * value, size_t value_len);
bool plugin_storage_delete(const char * plugin_id, const char * key);
int plugin_storage_list(const char * plugin_id, const char * prefix, char *** out_keys);

bool plugin_secrets_set(const char * plugin_id, const char * key, const char * value, size_t value_len);
bool plugin_secrets_exists(const char * plugin_id, const char * key);
bool plugin_secrets_delete(const char * plugin_id, const char * key);

#endif /* PLUGIN_STORAGE_H */