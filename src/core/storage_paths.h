#ifndef STORAGE_PATHS_H
#define STORAGE_PATHS_H

/* Persistent application data moved to this namespace for the next release.
 * These are the canonical paths for new writes. Compatibility code elsewhere
 * retains narrowly scoped legacy literals for boot, database, and fallback
 * handling. The metadata database already lives in .compas, so all of these
 * paths share that directory without treating the database as disposable. */
#define COMPAS_STORAGE_DIR ".compas"
#define LEGACY_STORAGE_DIR ".open_hiby_player"

#ifdef HOST_BUILD
#define INTERNAL_COMPAS_DIR "./.compas"
#define INTERNAL_LEGACY_DIR "./.open_hiby_player"
#define SETTINGS_FILE_PATH "./.compas/settings.txt"
#define SETTINGS_LEGACY_FILE_PATH "./open_hiby_player_settings.txt"
#define PEQ_FILE_PATH "./.compas/peq.txt"
#define PEQ_LEGACY_FILE_PATH "./open_hiby_player_peq.txt"
#define SUBSONIC_COMPAS_DIR "./.compas"
#define SUBSONIC_LEGACY_DIR "./.open_hiby_player"
#define PLUGIN_STORAGE_ROOT "./.compas/plugins"
#define SD_COMPAS_ROOT "./.compas"
#else
#define INTERNAL_COMPAS_DIR "/usr/data/.compas"
#define INTERNAL_LEGACY_DIR "/usr/data/.open_hiby_player"
#define SETTINGS_FILE_PATH "/usr/data/.compas/settings.txt"
#define SETTINGS_LEGACY_FILE_PATH "/usr/data/open_hiby_player_settings.txt"
#define PEQ_FILE_PATH "/usr/data/.compas/peq.txt"
#define PEQ_LEGACY_FILE_PATH "/usr/data/open_hiby_player_peq.txt"
#define SUBSONIC_COMPAS_DIR "/usr/data/.compas"
#define SUBSONIC_LEGACY_DIR "/usr/data/.open_hiby_player"
#define PLUGIN_STORAGE_ROOT "/usr/data/.compas/plugins"
#define SD_COMPAS_ROOT "/data/mnt/sd_0/.compas"
#endif

#ifdef HOST_BUILD
#define SD_LEGACY_ROOT "./.open_hiby_player"
#else
#define SD_LEGACY_ROOT "/data/mnt/sd_0/.open_hiby_player"
#endif

/* Safe, bounded migration invoked after each SD mount. */
void storage_migrate_legacy_data(void);

#endif
