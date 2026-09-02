#include "plugin_disabled_list.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifdef HOST_BUILD
#define MUSIC_ROOT_DIR "./music"
#else
#define MUSIC_ROOT_DIR "/data/mnt/sd_0"
#endif
#define PLUGINS_DIR MUSIC_ROOT_DIR "/.plugins"
#define DISABLED_LIST_PATH PLUGINS_DIR "/.disabled"
#define DISABLED_LIST_TMP_PATH DISABLED_LIST_PATH ".tmp"

/* Independent of PLUGIN_MAX_FILES (16, the concurrently-*loaded* cap) --
 * more .lua files can exist on disk, disabled, than can ever be loaded at
 * once, so this needs its own generous ceiling. */
#define PLUGIN_DISABLED_LIST_MAX 64

static char disabled_names[PLUGIN_DISABLED_LIST_MAX][256];
static int disabled_count = 0;

static void strip_eol(char * s) {
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r')) {
        s[--len] = '\0';
    }
}

void plugin_disabled_list_load(void) {
    disabled_count = 0;
    FILE * f = fopen(DISABLED_LIST_PATH, "r");
    if (!f) return;
    char line[256];
    while (disabled_count < PLUGIN_DISABLED_LIST_MAX && fgets(line, sizeof(line), f)) {
        strip_eol(line);
        if (line[0] == '\0') continue;
        snprintf(disabled_names[disabled_count], sizeof(disabled_names[0]), "%s", line);
        disabled_count++;
    }
    fclose(f);
}

bool plugin_disabled_list_contains(const char * filename) {
    for (int i = 0; i < disabled_count; i++) {
        if (strcmp(disabled_names[i], filename) == 0) return true;
    }
    return false;
}

/* Same atomic-write discipline as settings.c's settings_write_file(): write
 * to a .tmp path, fsync the file, rename over the real path, then fsync the
 * containing directory too -- this device's UBIFS partition has confirmed
 * form of dropping recently-written-but-uncommitted metadata across
 * anything other than a clean shutdown.
 *
 * Every step's return is checked (a full/removed SD card can fail any of
 * fprintf/fflush/fsync/fclose) and the rename is skipped entirely on any
 * failure -- the original, still-valid DISABLED_LIST_PATH is left
 * untouched rather than replaced with a truncated .tmp file that happened
 * to still rename successfully. */
static bool write_disabled_list(void) {
    FILE * f = fopen(DISABLED_LIST_TMP_PATH, "w");
    if (!f) return false;

    bool ok = true;
    for (int i = 0; i < disabled_count; i++) {
        if (fprintf(f, "%s\n", disabled_names[i]) < 0) ok = false;
    }
    if (fflush(f) != 0) ok = false;
    if (ok && fsync(fileno(f)) != 0) ok = false;
    if (fclose(f) != 0) ok = false;

    if (!ok) {
        remove(DISABLED_LIST_TMP_PATH);
        return false;
    }

    if (rename(DISABLED_LIST_TMP_PATH, DISABLED_LIST_PATH) != 0) return false;

    /* Best-effort only, same as settings.c's own fsync_settings_dir() --
     * the rename() above already succeeded, so the new content is already
     * the correct, currently-visible state. A failure here only weakens
     * the guarantee that the rename survives a hard power loss before the
     * next background sync; it must NOT cause the caller to roll the
     * in-memory list back to the old data, which would make memory and
     * (already-renamed) disk actively disagree -- worse than the residual
     * durability risk itself. Logged so a real failure is at least visible
     * in diagnostics instead of silently swallowed. */
    int dir_fd = open(PLUGINS_DIR, O_RDONLY);
    if (dir_fd < 0) {
        fprintf(stderr, "[plugins] warning: could not open %s to fsync after rename: %s\n",
                PLUGINS_DIR, strerror(errno));
    } else {
        if (fsync(dir_fd) != 0) {
            fprintf(stderr, "[plugins] warning: fsync of %s failed: %s\n", PLUGINS_DIR, strerror(errno));
        }
        close(dir_fd);
    }
    return true;
}

/* On a failed write, rolls the in-memory list back to whatever's still
 * actually on disk instead of leaving it reflecting a change that was
 * never persisted -- a caller that reloads unconditionally on success would
 * otherwise show state that reverts itself on the next real boot. */
bool plugin_disabled_list_set(const char * filename, bool disabled) {
    int found = -1;
    for (int i = 0; i < disabled_count; i++) {
        if (strcmp(disabled_names[i], filename) == 0) { found = i; break; }
    }

    if (disabled) {
        if (found >= 0) return true; /* already disabled -- nothing to persist */
        if (disabled_count >= PLUGIN_DISABLED_LIST_MAX) return false;
        snprintf(disabled_names[disabled_count], sizeof(disabled_names[0]), "%s", filename);
        disabled_count++;
        if (write_disabled_list()) return true;
        disabled_count--; /* roll back the append */
        return false;
    }

    if (found < 0) return true; /* already enabled -- nothing to persist */

    char removed[256];
    snprintf(removed, sizeof(removed), "%s", disabled_names[found]);
    memmove(&disabled_names[found], &disabled_names[found + 1],
            (size_t) (disabled_count - found - 1) * sizeof(disabled_names[0]));
    disabled_count--;
    if (write_disabled_list()) return true;

    /* Roll back the removal -- position doesn't matter, contains() is an
     * unordered scan, and the failed write means the reordering was never
     * actually persisted anyway. */
    snprintf(disabled_names[disabled_count], sizeof(disabled_names[0]), "%s", removed);
    disabled_count++;
    return false;
}
