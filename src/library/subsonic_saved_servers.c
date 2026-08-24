#include "subsonic_saved_servers.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef HOST_BUILD
  #define SUBSONIC_SAVED_SERVERS_DIR "./.open_hiby_player"
#else
  /* Internal ubifs, same partition as settings.c -- Saved Servers must
   * survive an unmounted SD card. */
  #define SUBSONIC_SAVED_SERVERS_DIR "/usr/data/.open_hiby_player"
  /* Earlier copy of this file stored the TSV next to tagcache. Imported
   * once if the internal file is missing, then rewritten to DIR. */
  #define SUBSONIC_SAVED_SERVERS_FALLBACK_DIR "/data/mnt/sd_0/.open_hiby_player"
#endif

#define SUBSONIC_SAVED_SERVERS_FILE "subsonic_servers.tsv"
#define SUBSONIC_SAVED_SERVERS_MAX 64

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static subsonic_saved_server_t * entries;
static int entry_n;
static int entry_cap;
static bool loaded;

static int find_index(const char * url) {
    if (!url) return -1;
    for (int i = 0; i < entry_n; i++) {
        if (strcmp(entries[i].url, url) == 0) return i;
    }
    return -1;
}

static void free_all(void) {
    free(entries);
    entries = NULL;
    entry_n = 0;
    entry_cap = 0;
}

static bool append_parsed(const char * url, const char * username, const char * password, bool verify_tls) {
    if (!url || !url[0] || strlen(url) >= sizeof(entries[0].url) ||
        strlen(username) >= sizeof(entries[0].username) ||
        strlen(password) >= sizeof(entries[0].password))
        return false;
    if (entry_n >= SUBSONIC_SAVED_SERVERS_MAX) return false;
    if (entry_n >= entry_cap) {
        int cap = entry_cap ? entry_cap * 2 : 16;
        subsonic_saved_server_t * nent = realloc(entries, sizeof(*nent) * (size_t) cap);
        if (!nent) return false;
        entries = nent;
        entry_cap = cap;
    }
    snprintf(entries[entry_n].url, sizeof(entries[entry_n].url), "%s", url);
    snprintf(entries[entry_n].username, sizeof(entries[entry_n].username), "%s", username);
    snprintf(entries[entry_n].password, sizeof(entries[entry_n].password), "%s", password);
    entries[entry_n].verify_tls = verify_tls;
    entry_n++;
    return true;
}

/* One record per line, tab-separated: url\tusername\tpassword\tverify_tls\n.
 * A tab/newline embedded in any field would corrupt this framing -- rejected
 * at upsert time (subsonic_saved_servers_upsert() below) rather than risking
 * a silently misparsed file, since none of url/username/password legitimately
 * contain either character. */
static void parse_file(FILE * f) {
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
        if (n == 0) continue;

        char * url = line;
        char * tab1 = strchr(url, '\t');
        if (!tab1) continue;
        *tab1 = '\0';
        char * username = tab1 + 1;
        char * tab2 = strchr(username, '\t');
        if (!tab2) continue;
        *tab2 = '\0';
        char * password = tab2 + 1;
        char * tab3 = strchr(password, '\t');
        if (!tab3) continue;
        *tab3 = '\0';
        char * verify_str = tab3 + 1;
        bool verify_tls = verify_str[0] == '1';
        if (!url[0] || strlen(url) >= sizeof(entries[0].url) || strlen(username) >= sizeof(entries[0].username) ||
            strlen(password) >= sizeof(entries[0].password))
            continue;

        int existing = find_index(url);
        if (existing >= 0) {
            snprintf(entries[existing].username, sizeof(entries[existing].username), "%s", username);
            snprintf(entries[existing].password, sizeof(entries[existing].password), "%s", password);
            entries[existing].verify_tls = verify_tls;
            continue;
        }
        if (entry_n >= SUBSONIC_SAVED_SERVERS_MAX) break;
        if (!append_parsed(url, username, password, verify_tls)) break;
    }
}

static void load_from_dir(const char * dir) {
    char path[640];
    snprintf(path, sizeof(path), "%s/%s", dir, SUBSONIC_SAVED_SERVERS_FILE);
    FILE * f = fopen(path, "r");
    if (!f) return;
    parse_file(f);
    fclose(f);
}

static void save_file(void);

static void load_file(void) {
    free_all();
    loaded = true;
    load_from_dir(SUBSONIC_SAVED_SERVERS_DIR);
#ifdef SUBSONIC_SAVED_SERVERS_FALLBACK_DIR
    if (entry_n == 0) {
        load_from_dir(SUBSONIC_SAVED_SERVERS_FALLBACK_DIR);
        if (entry_n > 0) save_file();
    }
#endif
}

static void ensure_loaded(void) {
    if (!loaded) load_file();
}

static void save_file(void) {
    mkdir(SUBSONIC_SAVED_SERVERS_DIR, 0755);
    char path[640], tmp[640];
    snprintf(path, sizeof(path), "%s/%s", SUBSONIC_SAVED_SERVERS_DIR, SUBSONIC_SAVED_SERVERS_FILE);
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int) sizeof(tmp)) return;
    FILE * f = fopen(tmp, "w");
    if (!f) return;
    bool ok = true;
    for (int i = 0; i < entry_n; i++) {
        if (fprintf(f, "%s\t%s\t%s\t%d\n", entries[i].url, entries[i].username, entries[i].password,
                    entries[i].verify_tls ? 1 : 0) < 0) {
            ok = false;
            break;
        }
    }
    if (fflush(f) != 0) ok = false;
    if (ok && fsync(fileno(f)) != 0) ok = false;
    if (fclose(f) != 0) ok = false;
    if (!ok) {
        unlink(tmp);
        return;
    }
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return;
    }
    int dfd = open(SUBSONIC_SAVED_SERVERS_DIR, O_RDONLY | O_DIRECTORY);
    if (dfd >= 0) {
        fsync(dfd);
        close(dfd);
    }
}

static bool field_ok(const char * s) {
    if (!s) return false;
    for (const char * p = s; *p; p++) {
        if (*p == '\t' || *p == '\n' || *p == '\r') return false;
    }
    return true;
}

static int cmp_url(const void * a, const void * b) {
    const subsonic_saved_server_t * sa = a;
    const subsonic_saved_server_t * sb = b;
    return strcasecmp(sa->url, sb->url);
}

void subsonic_saved_servers_upsert(const char * url, const char * username, const char * password, bool verify_tls) {
    subsonic_saved_server_t probe;
    if (!url || !url[0] || strlen(url) >= sizeof(probe.url)) return;
    if (!username) username = "";
    if (!password) password = "";
    if (strlen(username) >= sizeof(probe.username)) return;
    if (strlen(password) >= sizeof(probe.password)) return;
    if (!field_ok(url) || !field_ok(username) || !field_ok(password)) return;

    pthread_mutex_lock(&mu);
    ensure_loaded();
    int i = find_index(url);
    if (i < 0) {
        if (entry_n >= SUBSONIC_SAVED_SERVERS_MAX) {
            pthread_mutex_unlock(&mu);
            return;
        }
        if (entry_n >= entry_cap) {
            int cap = entry_cap ? entry_cap * 2 : 16;
            subsonic_saved_server_t * nent = realloc(entries, sizeof(*nent) * (size_t) cap);
            if (!nent) {
                pthread_mutex_unlock(&mu);
                return;
            }
            entries = nent;
            entry_cap = cap;
        }
        i = entry_n++;
        snprintf(entries[i].url, sizeof(entries[i].url), "%s", url);
        snprintf(entries[i].username, sizeof(entries[i].username), "%s", username);
        snprintf(entries[i].password, sizeof(entries[i].password), "%s", password);
        entries[i].verify_tls = verify_tls;
        save_file();
        pthread_mutex_unlock(&mu);
        return;
    }
    if (strcmp(entries[i].username, username) == 0 && strcmp(entries[i].password, password) == 0 &&
        entries[i].verify_tls == verify_tls) {
        pthread_mutex_unlock(&mu);
        return;
    }
    snprintf(entries[i].username, sizeof(entries[i].username), "%s", username);
    snprintf(entries[i].password, sizeof(entries[i].password), "%s", password);
    entries[i].verify_tls = verify_tls;
    save_file();
    pthread_mutex_unlock(&mu);
}

void subsonic_saved_servers_load(subsonic_saved_server_t ** out_rows, int * out_count) {
    *out_rows = NULL;
    *out_count = 0;
    pthread_mutex_lock(&mu);
    ensure_loaded();
    if (entry_n > 0) {
        subsonic_saved_server_t * copy = malloc(sizeof(*copy) * (size_t) entry_n);
        if (copy) {
            memcpy(copy, entries, sizeof(*copy) * (size_t) entry_n);
            qsort(copy, (size_t) entry_n, sizeof(*copy), cmp_url);
            *out_rows = copy;
            *out_count = entry_n;
        }
    }
    pthread_mutex_unlock(&mu);
}
