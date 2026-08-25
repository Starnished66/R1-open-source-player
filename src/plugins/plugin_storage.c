#include "plugin_storage.h"

#include "mbedtls/md5.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef HOST_BUILD
  #define PLUGIN_STORAGE_ROOT "./.open_hiby_player/plugins"
#else
  #define PLUGIN_STORAGE_ROOT "/usr/data/plugins"
#endif

#define PLUGIN_STORAGE_VALUE_MAX (256U * 1024U)
#define PLUGIN_STORAGE_KEYS_MAX 500
#define PLUGIN_STORAGE_PLUGIN_BYTES_MAX (2U * 1024U * 1024U)
#define PLUGIN_STORAGE_TOTAL_BYTES_MAX (8U * 1024U * 1024U)
#define PLUGIN_STORAGE_KEY_MAX 128
#define PLUGIN_STORAGE_ID_MAX 63
#define PLUGIN_STORAGE_CACHE_SLOTS 16
#define PLUGIN_STORAGE_RESCAN_EVERY 32
#define PLUGIN_STORAGE_MAGIC "PKV1"

static pthread_mutex_t storage_mutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    char plugin_id[PLUGIN_STORAGE_ID_MAX + 1];
    uint64_t bytes;
    int keys;
    bool loaded;
    int ops_since_rescan;
    uint32_t last_use;
} quota_slot_t;

static quota_slot_t quota_cache[PLUGIN_STORAGE_CACHE_SLOTS];
static uint64_t total_bytes_cache;
static bool total_loaded;
static int total_ops_since_rescan;
static uint32_t quota_use_clock;

static bool mkdir_p(const char * path, mode_t mode) {
    char buf[PATH_MAX];
    if (snprintf(buf, sizeof(buf), "%s", path) >= (int) sizeof(buf)) return false;
    size_t len = strlen(buf);
    if (len == 0) return false;
    if (buf[len - 1] == '/') buf[len - 1] = '\0';
    for (char * p = buf + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(buf, mode) != 0 && errno != EEXIST) return false;
        *p = '/';
    }
    return mkdir(buf, mode) == 0 || errno == EEXIST;
}

static bool id_ok(const char * plugin_id) {
    if (!plugin_id || !plugin_id[0] || strlen(plugin_id) > PLUGIN_STORAGE_ID_MAX) return false;
    for (const unsigned char * p = (const unsigned char *) plugin_id; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '.' || *p == '_' || *p == '-')) return false;
    }
    return true;
}

static bool key_ok(const char * key) {
    return key && key[0] && strlen(key) <= PLUGIN_STORAGE_KEY_MAX;
}

static void md5_hex(const char * a, const char * b, char out[33]) {
    unsigned char digest[16];
    unsigned char buf[PLUGIN_STORAGE_ID_MAX + PLUGIN_STORAGE_KEY_MAX + 8];
    size_t la = a ? strlen(a) : 0;
    size_t lb = b ? strlen(b) : 0;
    size_t n = 0;
    memcpy(buf + n, a ? a : "", la); n += la;
    buf[n++] = 0;
    memcpy(buf + n, b ? b : "", lb); n += lb;
    mbedtls_md5(buf, n, digest);
    for (int i = 0; i < 16; i++) snprintf(out + i * 2, 3, "%02x", digest[i]);
}

static bool plugin_dir(char * out, size_t out_size, const char * plugin_id) {
    char id_hash[33];
    md5_hex(plugin_id, "", id_hash);
    int n = snprintf(out, out_size, "%s/%s", PLUGIN_STORAGE_ROOT, id_hash);
    return n > 0 && (size_t) n < out_size;
}

static bool entry_path(char * out, size_t out_size, const char * plugin_id, const char * kind, const char * key) {
    char dir[PATH_MAX], key_hash[33];
    if (!plugin_dir(dir, sizeof(dir), plugin_id)) return false;
    md5_hex(kind, key, key_hash);
    int n = snprintf(out, out_size, "%s/%s/%s", dir, kind, key_hash);
    return n > 0 && (size_t) n < out_size;
}

/* Collapse ".", "..", and duplicate slashes without requiring the path to
 * exist -- realpath() cannot be used for a file that has not been created
 * yet, and a naive "/plugins/" substring match would also hit the SD card
 * .plugins/ script directory. */
static bool normalize_path(const char * in, char * out, size_t out_size) {
    if (!in || !in[0] || out_size < 2) return false;
    char abs[PATH_MAX];
    if (in[0] == '/') {
        if (snprintf(abs, sizeof(abs), "%s", in) >= (int) sizeof(abs)) return false;
    } else {
        char cwd[PATH_MAX];
        if (!getcwd(cwd, sizeof(cwd))) return false;
        if (snprintf(abs, sizeof(abs), "%s/%s", cwd, in) >= (int) sizeof(abs)) return false;
    }

    char * stack[PATH_MAX / 2];
    int nstack = 0;
    char * p = abs;
    while (*p == '/') p++;
    while (*p) {
        char * start = p;
        while (*p && *p != '/') p++;
        size_t seglen = (size_t) (p - start);
        char * next = p;
        while (*next == '/') next++;
        if (seglen == 1 && start[0] == '.') {
            p = next;
            continue;
        }
        if (seglen == 2 && start[0] == '.' && start[1] == '.') {
            if (nstack > 0) nstack--;
            p = next;
            continue;
        }
        start[seglen] = '\0';
        if (nstack >= (int) (sizeof(stack) / sizeof(stack[0]))) return false;
        stack[nstack++] = start;
        p = next;
    }

    size_t used = 0;
    out[used++] = '/';
    out[used] = '\0';
    for (int i = 0; i < nstack; i++) {
        size_t seglen = strlen(stack[i]);
        if (used + (used > 1 ? 1 : 0) + seglen >= out_size) return false;
        if (used > 1) out[used++] = '/';
        memcpy(out + used, stack[i], seglen);
        used += seglen;
        out[used] = '\0';
    }
    return true;
}

static bool path_is_under(const char * path, const char * root) {
    size_t n = strlen(root);
    if (n == 0) return false;
    if (n == 1 && root[0] == '/') return path[0] == '/';
    if (strncmp(path, root, n) != 0) return false;
    return path[n] == '\0' || path[n] == '/';
}

/* Follow one existing path component. realpath() refuses dangling
 * symlinks (ENOENT if the target is missing), but io.open("w") through
 * that same symlink still creates the target -- so readlink the link
 * itself and lexically join. */
static bool resolve_existing_component(const char * path, char * out, size_t out_size, int depth) {
    if (depth > 32) return false;
    struct stat st;
    if (lstat(path, &st) != 0) return false;
    if (S_ISLNK(st.st_mode)) {
        char target[PATH_MAX];
        ssize_t nr = readlink(path, target, sizeof(target) - 1);
        if (nr <= 0) return false;
        target[nr] = '\0';
        char joined[PATH_MAX];
        if (target[0] == '/') {
            if (snprintf(joined, sizeof(joined), "%s", target) >= (int) sizeof(joined)) return false;
        } else {
            char dir[PATH_MAX];
            if (snprintf(dir, sizeof(dir), "%s", path) >= (int) sizeof(dir)) return false;
            char * slash = strrchr(dir, '/');
            if (!slash) return false;
            if (slash == dir) slash[1] = '\0';
            else *slash = '\0';
            if (snprintf(joined, sizeof(joined), "%s/%s", dir, target) >= (int) sizeof(joined)) return false;
        }
        char norm[PATH_MAX];
        if (!normalize_path(joined, norm, sizeof(norm))) return false;
        if (lstat(norm, &st) == 0)
            return resolve_existing_component(norm, out, out_size, depth + 1);
        if (snprintf(out, out_size, "%s", norm) >= (int) out_size) return false;
        return true;
    }
    char resolved[PATH_MAX];
    if (!realpath(path, resolved)) return false;
    if (snprintf(out, out_size, "%s", resolved) >= (int) out_size) return false;
    return true;
}

/* Resolve the longest existing prefix, following symlinks (including
 * dangling ones). Needed so a lexical name outside PLUGIN_STORAGE_ROOT
 * that lands in that tree -- a shipped symlink, or /proc/self/root --
 * is still reserved. */
static bool resolve_nearest_existing_ancestor(const char * path, char * out, size_t out_size) {
    char buf[PATH_MAX];
    if (!path || !path[0] || snprintf(buf, sizeof(buf), "%s", path) >= (int) sizeof(buf)) return false;
    size_t n = strlen(buf);
    while (n > 1 && buf[n - 1] == '/') buf[--n] = '\0';
    for (;;) {
        if (resolve_existing_component(buf, out, out_size, 0)) return true;
        char * slash = strrchr(buf, '/');
        if (!slash) return false;
        if (slash == buf) {
            buf[1] = '\0';
            return resolve_existing_component(buf, out, out_size, 0);
        }
        *slash = '\0';
    }
}

bool plugin_storage_path_is_reserved(const char * path) {
    if (!path || !path[0]) return false;
    char path_n[PATH_MAX], root_n[PATH_MAX];
    char path_r[PATH_MAX], root_r[PATH_MAX];
    if (!normalize_path(PLUGIN_STORAGE_ROOT, root_n, sizeof(root_n))) return false;
    if (!normalize_path(path, path_n, sizeof(path_n))) return false;
    if (path_is_under(path_n, root_n)) return true;

    /* Never collapse a not-yet-created reserved root to its nearest existing
     * ancestor. On this firmware /data resolves through /usr/data, so doing
     * that while /usr/data/plugins did not yet exist broadened the reserved
     * tree to all of /usr/data -- including the mounted SD card at
     * /usr/data/mnt/sd_0. Keep the exact normalized root until it exists;
     * path_r still follows existing/dangling symlinks and therefore catches
     * an external path that actually resolves into that exact tree. */
    if (!resolve_existing_component(root_n, root_r, sizeof(root_r), 0)) {
        snprintf(root_r, sizeof(root_r), "%s", root_n);
    }
    if (!resolve_nearest_existing_ancestor(path_n, path_r, sizeof(path_r))) return false;
    return path_is_under(path_r, root_r);
}

static uint32_t le32(const unsigned char * p) {
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

static void put_le32(unsigned char * p, uint32_t v) {
    p[0] = (unsigned char) v;
    p[1] = (unsigned char) (v >> 8);
    p[2] = (unsigned char) (v >> 16);
    p[3] = (unsigned char) (v >> 24);
}

static bool fsync_parent(const char * path) {
    char dir[PATH_MAX];
    if (snprintf(dir, sizeof(dir), "%s", path) >= (int) sizeof(dir)) return false;
    char * slash = strrchr(dir, '/');
    if (!slash) return false;
    if (slash == dir) slash[1] = '\0';
    else *slash = '\0';
    int fd = open(dir, O_RDONLY | O_DIRECTORY);
    if (fd < 0) return false;
    bool ok = fsync(fd) == 0;
    close(fd);
    return ok;
}

static bool durable_replace(const char * dest, const void * data, size_t len, mode_t mode) {
    char tmp[PATH_MAX];
    int n = snprintf(tmp, sizeof(tmp), "%s.tmp", dest);
    if (n < 0 || (size_t) n >= sizeof(tmp)) return false;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0) return false;
    const unsigned char * bytes = data;
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, bytes + off, len - off);
        if (w <= 0) {
            close(fd);
            unlink(tmp);
            return false;
        }
        off += (size_t) w;
    }
    bool ok = fsync(fd) == 0;
    if (close(fd) != 0) ok = false;
    if (!ok) {
        unlink(tmp);
        return false;
    }
    if (rename(tmp, dest) != 0) {
        unlink(tmp);
        return false;
    }
    /* File is already visible. A failed directory fsync still returns
     * false so the Lua binding can report "written, durability not
     * confirmed" as PLUGINS.md documents. */
    if (!fsync_parent(dest)) return false;
    return true;
}

static bool pack_record(const char * key, const char * value, size_t value_len, unsigned char ** out, size_t * out_len) {
    size_t key_len = strlen(key);
    size_t total = 4 + 4 + key_len + value_len;
    unsigned char * buf = malloc(total);
    if (!buf) return false;
    memcpy(buf, PLUGIN_STORAGE_MAGIC, 4);
    put_le32(buf + 4, (uint32_t) key_len);
    memcpy(buf + 8, key, key_len);
    if (value_len) memcpy(buf + 8 + key_len, value, value_len);
    *out = buf;
    *out_len = total;
    return true;
}

static bool unpack_record(const unsigned char * buf, size_t len, char * key_out, size_t key_out_size,
                          char ** value_out, size_t * value_len) {
    if (len < 8 || memcmp(buf, PLUGIN_STORAGE_MAGIC, 4) != 0) return false;
    uint32_t key_len = le32(buf + 4);
    if (key_len == 0 || key_len > PLUGIN_STORAGE_KEY_MAX || 8 + key_len > len) return false;
    if (key_len + 1 > key_out_size) return false;
    memcpy(key_out, buf + 8, key_len);
    key_out[key_len] = '\0';
    size_t vlen = len - 8 - key_len;
    if (value_out) {
        char * v = malloc(vlen + 1);
        if (!v) return false;
        if (vlen) memcpy(v, buf + 8 + key_len, vlen);
        v[vlen] = '\0';
        *value_out = v;
    }
    if (value_len) *value_len = vlen;
    return true;
}

static bool read_file(const char * path, unsigned char ** out, size_t * out_len) {
    *out = NULL;
    *out_len = 0;
    FILE * f = fopen(path, "rb");
    if (!f) return false;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return false;
    }
    long sz = ftell(f);
    if (sz < 0 || (size_t) sz > PLUGIN_STORAGE_VALUE_MAX + PLUGIN_STORAGE_KEY_MAX + 16) {
        fclose(f);
        return false;
    }
    rewind(f);
    unsigned char * buf = malloc((size_t) sz + 1);
    if (!buf) {
        fclose(f);
        return false;
    }
    bool ok = fread(buf, 1, (size_t) sz, f) == (size_t) sz;
    fclose(f);
    if (!ok) {
        free(buf);
        return false;
    }
    buf[sz] = '\0';
    *out = buf;
    *out_len = (size_t) sz;
    return true;
}

static uint64_t dir_usage(const char * dir, int * out_files) {
    uint64_t bytes = 0;
    int files = 0;
    DIR * d = opendir(dir);
    if (!d) {
        if (out_files) *out_files = 0;
        return 0;
    }
    struct dirent * ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        char path[PATH_MAX];
        if (snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name) >= (int) sizeof(path)) continue;
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            int nested = 0;
            bytes += dir_usage(path, &nested);
            files += nested;
        } else if (S_ISREG(st.st_mode)) {
            bytes += (uint64_t) st.st_size;
            files++;
        }
    }
    closedir(d);
    if (out_files) *out_files = files;
    return bytes;
}

static void quota_touch(quota_slot_t * slot) {
    quota_use_clock++;
    if (quota_use_clock == 0) {
        for (int i = 0; i < PLUGIN_STORAGE_CACHE_SLOTS; i++) quota_cache[i].last_use = 0;
        quota_use_clock = 1;
    }
    slot->last_use = quota_use_clock;
}

static quota_slot_t * quota_slot(const char * plugin_id, bool create) {
    int empty = -1;
    int lru = -1;
    for (int i = 0; i < PLUGIN_STORAGE_CACHE_SLOTS; i++) {
        if (quota_cache[i].loaded && strcmp(quota_cache[i].plugin_id, plugin_id) == 0) {
            quota_touch(&quota_cache[i]);
            return &quota_cache[i];
        }
        if (!quota_cache[i].loaded) {
            if (empty < 0) empty = i;
        } else if (lru < 0 || quota_cache[i].last_use < quota_cache[lru].last_use) {
            lru = i;
        }
    }
    if (!create) return NULL;
    int idx = empty >= 0 ? empty : (lru >= 0 ? lru : 0);
    memset(&quota_cache[idx], 0, sizeof(quota_cache[idx]));
    snprintf(quota_cache[idx].plugin_id, sizeof(quota_cache[idx].plugin_id), "%s", plugin_id);
    quota_cache[idx].loaded = true;
    quota_touch(&quota_cache[idx]);
    return &quota_cache[idx];
}

static void quota_seed(const char * plugin_id) {
    quota_slot_t * slot = quota_slot(plugin_id, true);
    if (slot->loaded && slot->ops_since_rescan > 0 && slot->ops_since_rescan < PLUGIN_STORAGE_RESCAN_EVERY) return;
    char dir[PATH_MAX];
    if (!plugin_dir(dir, sizeof(dir), plugin_id)) return;
    int keys = 0;
    slot->bytes = dir_usage(dir, &keys);
    slot->keys = keys;
    slot->ops_since_rescan = 0;
    if (!total_loaded || total_ops_since_rescan >= PLUGIN_STORAGE_RESCAN_EVERY) {
        int ignored = 0;
        total_bytes_cache = dir_usage(PLUGIN_STORAGE_ROOT, &ignored);
        total_loaded = true;
        total_ops_since_rescan = 0;
    }
}

static bool kv_get(const char * plugin_id, const char * kind, const char * key, char ** out_value, size_t * out_len) {
    *out_value = NULL;
    *out_len = 0;
    if (!id_ok(plugin_id) || !key_ok(key)) return false;
    char path[PATH_MAX];
    if (!entry_path(path, sizeof(path), plugin_id, kind, key)) return false;
    unsigned char * buf = NULL;
    size_t len = 0;
    if (!read_file(path, &buf, &len)) return false;
    char stored_key[PLUGIN_STORAGE_KEY_MAX + 1];
    bool ok = unpack_record(buf, len, stored_key, sizeof(stored_key), out_value, out_len);
    free(buf);
    return ok;
}

static bool kv_set(const char * plugin_id, const char * kind, const char * key, const char * value, size_t value_len) {
    if (!id_ok(plugin_id) || !key_ok(key) || !value) return false;
    if (value_len > PLUGIN_STORAGE_VALUE_MAX) return false;

    char dir[PATH_MAX], path[PATH_MAX], kind_dir[PATH_MAX];
    if (!plugin_dir(dir, sizeof(dir), plugin_id)) return false;
    if (snprintf(kind_dir, sizeof(kind_dir), "%s/%s", dir, kind) >= (int) sizeof(kind_dir)) return false;
    if (!mkdir_p(kind_dir, 0700)) return false;
    if (!entry_path(path, sizeof(path), plugin_id, kind, key)) return false;

    unsigned char * packed = NULL;
    size_t packed_len = 0;
    if (!pack_record(key, value, value_len, &packed, &packed_len)) return false;

    struct stat st;
    uint64_t replacing = 0;
    bool existed = (stat(path, &st) == 0 && S_ISREG(st.st_mode));
    if (existed) {
        replacing = (uint64_t) st.st_size;
        if (replacing == packed_len) {
            unsigned char * existing = NULL;
            size_t existing_len = 0;
            if (read_file(path, &existing, &existing_len) && existing_len == packed_len &&
                memcmp(existing, packed, packed_len) == 0) {
                free(existing);
                free(packed);
                return true;
            }
            free(existing);
        }
    }

    pthread_mutex_lock(&storage_mutex);
    quota_seed(plugin_id);
    quota_slot_t * slot = quota_slot(plugin_id, true);
    uint64_t new_plugin = slot->bytes - replacing + packed_len;
    uint64_t new_total = total_bytes_cache - replacing + packed_len;
    bool over = (!existed && slot->keys >= PLUGIN_STORAGE_KEYS_MAX) ||
                new_plugin > PLUGIN_STORAGE_PLUGIN_BYTES_MAX ||
                new_total > PLUGIN_STORAGE_TOTAL_BYTES_MAX;
    if (over) {
        pthread_mutex_unlock(&storage_mutex);
        free(packed);
        return false;
    }

    bool durable = durable_replace(path, packed, packed_len, 0600);
    if (durable || access(path, F_OK) == 0) {
        slot->bytes = slot->bytes - replacing + packed_len;
        if (!existed) slot->keys++;
        total_bytes_cache = total_bytes_cache - replacing + packed_len;
        slot->ops_since_rescan++;
        total_ops_since_rescan++;
    }
    pthread_mutex_unlock(&storage_mutex);
    free(packed);
    return durable;
}

static bool kv_delete(const char * plugin_id, const char * kind, const char * key) {
    if (!id_ok(plugin_id) || !key_ok(key)) return false;
    char path[PATH_MAX];
    if (!entry_path(path, sizeof(path), plugin_id, kind, key)) return false;
    struct stat st;
    uint64_t old_size = 0;
    bool existed = (stat(path, &st) == 0 && S_ISREG(st.st_mode));
    if (existed) old_size = (uint64_t) st.st_size;
    if (unlink(path) != 0 && errno != ENOENT) return false;
    bool durable = fsync_parent(path);
    pthread_mutex_lock(&storage_mutex);
    quota_seed(plugin_id);
    quota_slot_t * slot = quota_slot(plugin_id, true);
    if (existed) {
        if (slot->bytes >= old_size) slot->bytes -= old_size;
        if (slot->keys > 0) slot->keys--;
        if (total_bytes_cache >= old_size) total_bytes_cache -= old_size;
        slot->ops_since_rescan++;
        total_ops_since_rescan++;
    }
    pthread_mutex_unlock(&storage_mutex);
    return durable;
}

bool plugin_storage_get(const char * plugin_id, const char * key, char ** out_value, size_t * out_len) {
    return kv_get(plugin_id, "storage", key, out_value, out_len);
}

bool plugin_storage_set(const char * plugin_id, const char * key, const char * value, size_t value_len) {
    return kv_set(plugin_id, "storage", key, value, value_len);
}

bool plugin_storage_delete(const char * plugin_id, const char * key) {
    return kv_delete(plugin_id, "storage", key);
}

int plugin_storage_list(const char * plugin_id, const char * prefix, char *** out_keys) {
    *out_keys = NULL;
    if (!id_ok(plugin_id)) return -1;
    if (!prefix) prefix = "";
    char dir[PATH_MAX];
    if (!plugin_dir(dir, sizeof(dir), plugin_id)) return -1;
    char storage_dir[PATH_MAX];
    if (snprintf(storage_dir, sizeof(storage_dir), "%s/storage", dir) >= (int) sizeof(storage_dir)) return -1;
    DIR * d = opendir(storage_dir);
    if (!d) return 0;
    char ** keys = NULL;
    int count = 0, cap = 0;
    struct dirent * ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        char path[PATH_MAX];
        if (snprintf(path, sizeof(path), "%s/%s", storage_dir, ent->d_name) >= (int) sizeof(path)) continue;
        unsigned char * buf = NULL;
        size_t len = 0;
        if (!read_file(path, &buf, &len)) continue;
        char key[PLUGIN_STORAGE_KEY_MAX + 1];
        bool ok = unpack_record(buf, len, key, sizeof(key), NULL, NULL);
        free(buf);
        if (!ok) continue;
        if (strncmp(key, prefix, strlen(prefix)) != 0) continue;
        if (count >= cap) {
            int next = cap ? cap * 2 : 8;
            char ** grown = realloc(keys, sizeof(*grown) * (size_t) next);
            if (!grown) {
                for (int i = 0; i < count; i++) free(keys[i]);
                free(keys);
                closedir(d);
                return -1;
            }
            keys = grown;
            cap = next;
        }
        keys[count] = strdup(key);
        if (!keys[count]) {
            for (int i = 0; i < count; i++) free(keys[i]);
            free(keys);
            closedir(d);
            return -1;
        }
        count++;
    }
    closedir(d);
    *out_keys = keys;
    return count;
}

bool plugin_secrets_set(const char * plugin_id, const char * key, const char * value, size_t value_len) {
    return kv_set(plugin_id, "secrets", key, value, value_len);
}

bool plugin_secrets_exists(const char * plugin_id, const char * key) {
    char path[PATH_MAX];
    if (!id_ok(plugin_id) || !key_ok(key)) return false;
    if (!entry_path(path, sizeof(path), plugin_id, "secrets", key)) return false;
    return access(path, F_OK) == 0;
}

bool plugin_secrets_delete(const char * plugin_id, const char * key) {
    return kv_delete(plugin_id, "secrets", key);
}
