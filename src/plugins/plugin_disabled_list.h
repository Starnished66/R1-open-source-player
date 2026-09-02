#ifndef PLUGIN_DISABLED_LIST_H
#define PLUGIN_DISABLED_LIST_H

#include <stdbool.h>

/* Persisted, filename-keyed list of plugins the user has toggled off via the
 * "Manage Plugins" settings screen (gui_plugin_manage.c). Filename, not the
 * runtime plugin.define() id, is the key -- a disabled plugin's top-level
 * script never runs, so its id (if it declares one at all) is never seen.
 * Backed by <SD card>/.plugins/.disabled, one filename per line. Called from
 * plugin_manager_init() itself (every invocation, including a soft reload)
 * rather than a separate boot call site, since plugin_disabled_list_set()
 * always keeps the on-disk file authoritative -- re-reading it on every init
 * is harmless and keeps this module self-contained. */
void plugin_disabled_list_load(void);

bool plugin_disabled_list_contains(const char * filename);

/* Adds/removes `filename` from the disabled list and persists the result
 * immediately (atomic tmp-file + fsync + rename + directory fsync, same
 * discipline as settings.c's settings_write_file()). Returns false only if
 * the write itself failed -- in that case the in-memory list is rolled back
 * to match what's still actually on disk, so a caller can't observe a
 * change that was never persisted. */
bool plugin_disabled_list_set(const char * filename, bool disabled);

#endif /* PLUGIN_DISABLED_LIST_H */
