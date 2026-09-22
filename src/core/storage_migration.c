#define _GNU_SOURCE
#include "storage_migration.h"
#include "storage_paths.h"

#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>

#define MIGRATION_MAX_ENTRIES 256
#include <sys/stat.h>
#include <unistd.h>

/* TEMPORARY MIGRATION (remove after the compatibility window). The bounded
 * top-level enumeration moves app-owned regular files/directories in O(1),
 * without walking large cache trees. Metadata database files are excluded:
 * metadata_db.c owns their replay and archive lifecycle. Bootloader stock /
 * update artifacts and transient install files stay in the legacy namespace
 * because older bootloaders still discover those exact names. */
static bool excluded_entry(const char * name) {
    if (!name || !name[0]) return true;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return true;
    if (strcmp(name, "hiby_player") == 0 || strcmp(name, "open_hiby_player") == 0) return true;
    /* albumart is a cache directory. A pre-created empty .compas/albumart
     * must not hide the legacy tree, so it is merged on its own below. */
    if (strcmp(name, "albumart") == 0) return true;
    if (strncmp(name, "database_", 9) == 0 || strncmp(name, "tagcache.gen", 12) == 0) return true;
    if (strncmp(name, "migration.", 10) == 0 || strncmp(name, "migration_", 10) == 0) return true;
    if (strstr(name, ".tmp") || strstr(name, ".installing")) return true;
    return false;
}

static bool dot_name(const char * name) {
    return strcmp(name, ".") == 0 || strcmp(name, "..") == 0;
}

static bool directory_is_empty(int dirfd) {
    int fd = openat(dirfd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return false;
    DIR * dir = fdopendir(fd);
    if (!dir) { close(fd); return false; }
    bool empty = true;
    struct dirent * entry;
    while ((entry = readdir(dir)) != NULL) {
        if (!dot_name(entry->d_name)) { empty = false; break; }
    }
    closedir(dir);
    return empty;
}

/* Move cache entries that the destination does not already have. An existing
 * destination file is left untouched and the legacy copy is kept, so neither
 * version is deleted. Directory renames while readdir is in progress can skip
 * names, so the names are collected before anything is renamed. */
static void merge_cache_entries(int srcfd, int dstfd, int depth) {
    if (depth > 8) return;
    int scanfd = openat(srcfd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (scanfd < 0) return;
    DIR * dir = fdopendir(scanfd);
    if (!dir) { close(scanfd); return; }
    size_t count = 0, cap = 0;
    char ** names = NULL;
    struct dirent * entry;
    while ((entry = readdir(dir)) != NULL) {
        if (dot_name(entry->d_name)) continue;
        if (count == cap) {
            size_t next = cap ? cap * 2 : 64;
            char ** grown = realloc(names, next * sizeof(*names));
            if (!grown) break;
            names = grown;
            cap = next;
        }
        names[count] = strdup(entry->d_name);
        if (!names[count]) break;
        count++;
    }
    closedir(dir);
    for (size_t i = 0; i < count; i++) {
        const char * name = names[i];
        if (!name) continue;
        struct stat src_st, dst_st;
        if (fstatat(srcfd, name, &src_st, AT_SYMLINK_NOFOLLOW) != 0) continue;
        if (!S_ISREG(src_st.st_mode) && !S_ISDIR(src_st.st_mode)) continue;
        if (fstatat(dstfd, name, &dst_st, AT_SYMLINK_NOFOLLOW) == 0) {
            if (S_ISDIR(src_st.st_mode) && S_ISDIR(dst_st.st_mode)) {
                int child_src = openat(srcfd, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
                int child_dst = openat(dstfd, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
                if (child_src >= 0 && child_dst >= 0) merge_cache_entries(child_src, child_dst, depth + 1);
                if (child_src >= 0) close(child_src);
                if (child_dst >= 0) close(child_dst);
            }
            continue;
        }
        if (errno != ENOENT) continue;
        (void) renameat(srcfd, name, dstfd, name);
    }
    for (size_t i = 0; i < count; i++) free(names[i]);
    free(names);
}

static void migrate_albumart_cache(int oldroot, int newroot) {
    struct stat src_st, dst_st;
    if (fstatat(oldroot, "albumart", &src_st, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISDIR(src_st.st_mode)) return;
    if (fstatat(newroot, "albumart", &dst_st, AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno == ENOENT) (void) renameat(oldroot, "albumart", newroot, "albumart");
        return;
    }
    if (!S_ISDIR(dst_st.st_mode)) return;
    int dstfd = openat(newroot, "albumart", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (dstfd < 0) return;
    if (directory_is_empty(dstfd)) {
        close(dstfd);
        if (unlinkat(newroot, "albumart", AT_REMOVEDIR) == 0 &&
            renameat(oldroot, "albumart", newroot, "albumart") == 0)
            return;
        dstfd = openat(newroot, "albumart", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (dstfd < 0) return;
    }
    int srcfd = openat(oldroot, "albumart", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (srcfd < 0) { close(dstfd); return; }
    merge_cache_entries(srcfd, dstfd, 0);
    bool emptied = directory_is_empty(srcfd);
    close(srcfd);
    close(dstfd);
    if (emptied) (void) unlinkat(oldroot, "albumart", AT_REMOVEDIR);
}

void storage_migrate_legacy_data_at(int rootfd) {
    if (rootfd < 0) return;
    int oldfd = openat(rootfd, LEGACY_STORAGE_DIR, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (oldfd < 0) return;
    /* mkdirat is deliberately best-effort: on a read-only card we can still
     * inspect the legacy directory, but must not claim migration succeeded. */
    (void) mkdirat(rootfd, COMPAS_STORAGE_DIR, 0755);
    int newfd = openat(rootfd, COMPAS_STORAGE_DIR, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (newfd < 0) { close(oldfd); return; }
    migrate_albumart_cache(oldfd, newfd);
    int scanfd = dup(oldfd);
    DIR * dir = scanfd >= 0 ? fdopendir(scanfd) : NULL;
    if (!dir) { if (scanfd >= 0) close(scanfd); close(newfd); close(oldfd); return; }
    char (*names)[NAME_MAX + 1] = calloc(MIGRATION_MAX_ENTRIES, sizeof(*names));
    if (!names) { closedir(dir); close(newfd); close(oldfd); return; }
    size_t name_count = 0;
    struct dirent * entry;
    while (name_count < MIGRATION_MAX_ENTRIES && (entry = readdir(dir)) != NULL) {
        const char * name = entry->d_name;
        if (excluded_entry(name)) continue;
        struct stat old_st;
        if (fstatat(oldfd, name, &old_st, AT_SYMLINK_NOFOLLOW) != 0) continue;
        if (!S_ISREG(old_st.st_mode) && !S_ISDIR(old_st.st_mode)) continue;
        struct stat new_st;
        if (fstatat(newfd, name, &new_st, AT_SYMLINK_NOFOLLOW) == 0) continue;
        if (errno != ENOENT) continue;
        snprintf(names[name_count++], NAME_MAX + 1, "%s", name);
    }
    /* Stop reading before mutation. If the cap was reached, the remaining
     * entries stay in the legacy directory and are retried on the next boot. */
    closedir(dir);
    for (size_t i = 0; i < name_count; i++) {
        const char * name = names[i];
        struct stat new_st;
        if (fstatat(newfd, name, &new_st, AT_SYMLINK_NOFOLLOW) == 0) continue;
        if (errno != ENOENT) continue;
        /* renameat stays relative to the pinned root's child descriptors and
         * cannot cross over to a replacement card during hot-swap. */
        (void) renameat(oldfd, name, newfd, name);
    }
    free(names);
    close(newfd);
    close(oldfd);
}

static void move_file_if_absent(const char * old_path, const char * new_path, const char * new_dir) {
    struct stat old_st, new_st;
    if (lstat(old_path, &old_st) != 0) return;
    if (lstat(new_path, &new_st) == 0 || errno != ENOENT) return;
    (void) mkdir(new_dir, 0755);
    (void) rename(old_path, new_path);
}

void storage_migrate_internal_data(void) {
    move_file_if_absent(SETTINGS_LEGACY_FILE_PATH, SETTINGS_FILE_PATH, INTERNAL_COMPAS_DIR);
    move_file_if_absent(PEQ_LEGACY_FILE_PATH, PEQ_FILE_PATH, INTERNAL_COMPAS_DIR);
#ifndef HOST_BUILD
    move_file_if_absent("/usr/data/plugins", "/usr/data/.compas/plugins", INTERNAL_COMPAS_DIR);
#endif
}

void storage_migrate_legacy_data(void) {
#ifdef HOST_BUILD
    int rootfd = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    storage_migrate_legacy_data_at(rootfd);
    if (rootfd >= 0) close(rootfd);
#else
    /* The caller has already confirmed the SD mount. This descriptor pins the
     * card for the complete bounded merge operation. */
    int rootfd = open("/data/mnt/sd_0", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    storage_migrate_legacy_data_at(rootfd);
    if (rootfd >= 0) close(rootfd);
#endif
}
