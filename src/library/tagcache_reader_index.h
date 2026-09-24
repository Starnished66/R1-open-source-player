/* Seekable derived orders and memberships use generation-local files. */
static void reader_close_indexes(void);
static int32_t legacy_find_path(const char *path);
static bool legacy_order_song(bool recency, int32_t rank, tagcache_song_t *out);
static int32_t legacy_rank_of(int32_t slot, bool recency);
static int legacy_group_count(int kind);
static bool legacy_group_at(int kind, int index, tagcache_group_t *out);
static int legacy_group_index(int kind, const char *name, const char *aa);
static int legacy_group_ids(int kind, int gi, int offset, int32_t *ids, int max);

#define TC_INDEX_MAGIC 0x54434931u
#define TC_INDEX_VERSION 4u
static bool reader_build_indexes(void);

static size_t mem_available_bytes(void) {
    FILE * f = fopen("/proc/meminfo", "r");
    if (f) {
        char line[160];
        size_t avail = 0, memfree = 0, cached = 0;
        while (fgets(line, sizeof(line), f)) {
            unsigned long v = 0;
            if (sscanf(line, "MemAvailable: %lu", &v) == 1) avail = (size_t) v * 1024u;
            else if (sscanf(line, "MemFree: %lu", &v) == 1) memfree = (size_t) v * 1024u;
            else if (sscanf(line, "Cached: %lu", &v) == 1) cached = (size_t) v * 1024u;
        }
        fclose(f);
        if (avail) return avail;
        return memfree + cached;
    }
#ifdef _SC_AVPHYS_PAGES
    {
        long pages = sysconf(_SC_AVPHYS_PAGES);
        long sz = sysconf(_SC_PAGESIZE);
        if (pages > 0 && sz > 0) return (size_t) pages * (size_t) sz;
    }
#endif
    return 256ull * 1024ull * 1024ull;
}

static bool choose_intern_strings(int32_t n) {
    const char * env = getenv("TAGCACHE_FORCE_COMPACT");
    if (env && env[0] && env[0] != '0') return false;
    env = getenv("TAGCACHE_FORCE_INTERN");
    if (env && env[0] && env[0] != '0') return true;
    if (n < 0) n = 0;
    size_t avail = mem_available_bytes();
    size_t full = (size_t) n * 200ull + 3ull * 1024ull * 1024ull;
    return full + (16ull * 1024ull * 1024ull) <= avail;
}

struct tc_index_meta {
    uint32_t magic;
    uint32_t version;
    int32_t generation, serial, commitid;
    int32_t entry_count;
    int32_t live_entries;
    int32_t group_count[3];
    uint64_t size[18];
};

static const char *tc_index_suffixes[18] = {
    ".title", ".recency", ".path", ".rank0", ".rank1",
    ".group0", ".group1", ".group2", ".member0", ".member1", ".member2",
    ".compact_recency", ".compact_rank", ".compact_member0", ".compact_member1", ".compact_member2", ".artist_names", ".compact_artist_names"
};

static void tc_index_name(char *out, size_t out_size, int32_t gen, const char *suffix) {
    snprintf(out, out_size, "%s/database_idx.tcd.g%d.i%s", db_dir, gen, suffix + 1);
}

static bool tc_index_copy_fd(int from, int to, uint64_t *size_out) {
    unsigned char buf[32768];
    off_t off = 0;
    uint64_t total = 0;
    for (;;) {
        ssize_t n = pread(from, buf, sizeof(buf), off);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return false;
        if (n == 0) break;
        size_t done = 0;
        while (done < (size_t)n) {
            ssize_t w = write(to, buf + done, (size_t)n - done);
            if (w < 0 && errno == EINTR) continue;
            if (w <= 0) return false;
            done += (size_t)w;
        }
        off += n;
        total += (uint64_t)n;
    }
    if (size_out) *size_out = total;
    return true;
}

/* Writes the derived reader files for a generation.  The caller must invoke
 * this before publishing the generation pointer. */
static bool reader_write_indexes(int32_t gen) {
    int src[11] = { reader_title_fd, reader_recency_fd, reader_path_fd,
                    reader_rank_fd[0], reader_rank_fd[1],
                    reader_group_fd[0], reader_group_fd[1], reader_group_fd[2],
                    reader_members_fd[0], reader_members_fd[1], reader_members_fd[2] };
    struct tc_index_meta meta;
    memset(&meta, 0, sizeof(meta));
    meta.magic = TC_INDEX_MAGIC;
    meta.version = TC_INDEX_VERSION;
    meta.generation = gen;
    meta.serial = master_serial;
    meta.commitid = master_commitid;
    meta.entry_count = ent_count;
    meta.live_entries = live_count;
    for (int k = 0; k < 3; k++) meta.group_count[k] = reader_group_n[k];
    char path[640], tmp[680];
    snprintf(path, sizeof(path), "%s/database_idx.tcd.g%d.imeta", db_dir, gen);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    int mfd = open(tmp, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (mfd < 0) return false;
    bool ok = true;
    for (int i = 0; i < 11 && ok; i++) {
        if (src[i] < 0) { ok = false; break; }
        char name[640], name_tmp[680];
        tc_index_name(name, sizeof(name), gen, tc_index_suffixes[i]);
        snprintf(name_tmp, sizeof(name_tmp), "%s.tmp", name);
        int out = open(name_tmp, O_CREAT | O_TRUNC | O_WRONLY, 0644);
        if (out < 0) { ok = false; break; }
        ok = tc_index_copy_fd(src[i], out, &meta.size[i]);
        if (ok && fsync(out) != 0) ok = false;
        if (close(out) != 0) ok = false;
        if (ok && rename(name_tmp, name) != 0) ok = false;
        if (!ok) unlink(name_tmp);
    }
    if (ok) {
        char name[640];
        tc_index_name(name, sizeof(name), gen, tc_index_suffixes[16]);
        int out = open(name, O_CREAT | O_TRUNC | O_WRONLY, 0644);
        ok = out >= 0 && tc_index_copy_fd(reader_artist_names_fd, out, &meta.size[16]);
        if (out >= 0 && !close_synced(out)) ok = false;
    }
    if (ok) {
        char name[640];
        tc_index_name(name, sizeof(name), gen, ".query");
        int out = open(name, O_CREAT | O_TRUNC | O_RDWR, 0644);
        ok = out >= 0 && query_build(out);
        if (out >= 0 && !close_synced(out)) ok = false;
    }
    if (ok) {
        reader_compact_sort = true;
        ok = reader_build_indexes();
        reader_compact_sort = false;
    }
    int compact_src[5] = { reader_recency_fd, reader_rank_fd[1],
        reader_members_fd[0], reader_members_fd[1], reader_members_fd[2] };
    for (int i = 0; ok && i < 5; i++) {
        char name[640];
        tc_index_name(name, sizeof(name), gen, tc_index_suffixes[11 + i]);
        int out = open(name, O_CREAT | O_TRUNC | O_WRONLY, 0644);
        if (out < 0) { ok = false; break; }
        ok = tc_index_copy_fd(compact_src[i], out, &meta.size[11 + i]);
        if (!close_synced(out)) ok = false;
    }
    if (ok) {
        char name[640];
        tc_index_name(name, sizeof(name), gen, tc_index_suffixes[17]);
        int out = open(name, O_CREAT | O_TRUNC | O_WRONLY, 0644);
        ok = out >= 0 && tc_index_copy_fd(reader_artist_names_fd, out, &meta.size[17]);
        if (out >= 0 && !close_synced(out)) ok = false;
    }
    if (ok) ok = write_fully(mfd, &meta, sizeof(meta));
    if (ok) ok = fsync(mfd) == 0;
    if (close(mfd) != 0) ok = false;
    if (ok) ok = rename(tmp, path) == 0;
    if (!ok) {
        unlink(tmp);
        char query_path[640];
        tc_index_name(query_path, sizeof(query_path), gen, ".query");
        unlink(query_path);
        for (int i = 0; i < 18; i++) {
            char name[640];
            tc_index_name(name, sizeof(name), gen, tc_index_suffixes[i]);
            unlink(name);
        }
    }
    return ok;
}

/* Returns 1 for a valid persisted index, 0 when it is absent (legacy
 * generation), and -1 when the index is present but malformed. */
static int reader_open_indexes(int32_t gen) {
    struct tc_index_meta meta;
    char path[640];
    snprintf(path, sizeof(path), "%s/database_idx.tcd.g%d.imeta", db_dir, gen);
    int mfd = open(path, O_RDONLY);
    if (mfd < 0) return errno == ENOENT ? 0 : -1;
    struct stat metadata_stat;
    if (fstat(mfd, &metadata_stat) != 0 || metadata_stat.st_size != (off_t)sizeof(meta) ||
        !reader_read_at(mfd, &meta, sizeof(meta), 0)) {
        close(mfd);
        return -1;
    }
    close(mfd);
    if (meta.magic != TC_INDEX_MAGIC || (meta.version != 3u && meta.version != TC_INDEX_VERSION) ||
        meta.generation != gen || meta.serial != master_serial || meta.commitid != master_commitid ||
        meta.entry_count != ent_count || meta.live_entries < 0 || meta.live_entries > ent_count)
        return -1;
    if (meta.size[0] != (uint64_t)meta.live_entries * 4 || meta.size[1] != meta.size[0] ||
        meta.size[2] != (uint64_t)meta.live_entries * 8 ||
        meta.size[3] != (uint64_t)ent_count * 4 || meta.size[4] != meta.size[3]) return -1;
    for (int k = 0; k < 3; k++) {
        uint64_t member_max = (uint64_t)meta.live_entries * (k == TAGCACHE_GROUP_ARTIST ? TAGCACHE_ARTIST_SPLIT_MAX : 1);
        if (meta.group_count[k] < 0 || (uint64_t)meta.group_count[k] > member_max ||
            meta.size[5 + k] != (uint64_t)meta.group_count[k] * 20 ||
            meta.size[8 + k] % 4 || meta.size[8 + k] < (uint64_t)meta.live_entries * 4 ||
            meta.size[8 + k] > member_max * 4) return -1;
    }
    if (meta.size[11] != meta.size[1] || meta.size[12] != meta.size[4] ||
        meta.size[13] != meta.size[8] || meta.size[14] != meta.size[9] || meta.size[15] != meta.size[10]) return -1;
    for (int i = 16; i < 18; i++)
        if (meta.size[i] < (uint64_t)meta.group_count[0] * 5 ||
            meta.size[i] > (uint64_t)meta.group_count[0] * (TAGCACHE_PATH_MAX + 4)) return -1;
    reader_compact_order = !choose_intern_strings(ent_count);
    for (int i = 0; i < 18; i++) {
        struct stat component_stat;
        tc_index_name(path, sizeof(path), gen, tc_index_suffixes[i]);
        if (stat(path, &component_stat) != 0 || (uint64_t)component_stat.st_size != meta.size[i]) return -1;
    }
    int *dst[11] = { &reader_title_fd, &reader_recency_fd, &reader_path_fd,
                     &reader_rank_fd[0], &reader_rank_fd[1],
                     &reader_group_fd[0], &reader_group_fd[1], &reader_group_fd[2],
                     &reader_members_fd[0], &reader_members_fd[1], &reader_members_fd[2] };
    for (int i = 0; i < 11; i++) {
        int component = i;
        if (reader_compact_order) {
            if (i == 1) component = 11;
            else if (i == 4) component = 12;
            else if (i >= 8) component = i + 5;
        }
        tc_index_name(path, sizeof(path), gen, tc_index_suffixes[component]);
        int fd = open(path, O_RDONLY);
        struct stat st;
        if (fd < 0 || fstat(fd, &st) != 0 || (uint64_t)st.st_size != meta.size[i]) {
            if (fd >= 0) close(fd);
            reader_close_indexes();
            return -1;
        }
        *dst[i] = fd;
    }
    tc_index_name(path, sizeof(path), gen, tc_index_suffixes[reader_compact_order ? 17 : 16]);
    reader_artist_names_fd = open(path, O_RDONLY);
    if (reader_artist_names_fd < 0) { reader_close_indexes(); return -1; }
    live_count = meta.live_entries;
    for (int k = 0; k < 3; k++) reader_group_n[k] = meta.group_count[k];
    if (meta.version >= 4u) {
        struct stat st;
        tc_index_name(path, sizeof(path), gen, ".query");
        reader_query_fd = open(path, O_RDONLY);
        if (reader_query_fd < 0 || fstat(reader_query_fd, &st) != 0 ||
            !query_validate(reader_query_fd, (uint64_t)st.st_size)) {
            reader_close_indexes();
            return -1;
        }
    }
    return 1;
}

static void reader_unlink_indexes(int32_t gen) {
    char path[640], tmp[680];
    for (int i = 0; i < 20; i++) {
        tc_index_name(path, sizeof(path), gen, i == 19 ? ".query" : i == 18 ? ".meta" : tc_index_suffixes[i]);
        unlink(path);
        snprintf(tmp, sizeof(tmp), "%s.tmp", path);
        unlink(tmp);
    }
}

static int reader_create_temp(void) {
    char path[640];
    int n = snprintf(path, sizeof(path), "%s/.tagcache-work-XXXXXX", db_dir);
    if (n < 0 || (size_t) n >= sizeof(path)) return -1;
    int fd = mkstemp(path);
    if (fd >= 0 && unlink(path) != 0) { close(fd); return -1; }
    return fd;
}

static void reader_close_indexes(void) {
    int *single[] = {&reader_query_fd, &reader_title_fd, &reader_recency_fd, &reader_path_fd, &reader_artist_names_fd};
    for (size_t i = 0; i < sizeof(single) / sizeof(single[0]); i++) {
        if (*single[i] >= 0) close(*single[i]);
        *single[i] = -1;
    }
    for (int k = 0; k < 3; k++) {
        if (reader_group_fd[k] >= 0) close(reader_group_fd[k]);
        if (reader_members_fd[k] >= 0) close(reader_members_fd[k]);
        reader_group_fd[k] = reader_members_fd[k] = -1;
        reader_group_n[k] = 0;
    }
    for (int k = 0; k < 2; k++) {
        if (reader_rank_fd[k] >= 0) close(reader_rank_fd[k]);
        reader_rank_fd[k] = -1;
    }
    reader_query_cache_valid = false;
    reader_query_cache_block = 0;
}

static int split_artist(const char *raw, char names[TAGCACHE_ARTIST_SPLIT_MAX][TAGCACHE_PATH_MAX]) {
    if (!raw) raw = "";
    int n = 0;
    const char *p = raw;
    while (*p && n < TAGCACHE_ARTIST_SPLIT_MAX) {
        while (*p && strchr(";/", *p)) p++;
        const char *start = p;
        while (*p && !strchr(";/", *p)) p++;
        const char *end = p;
        while (end > start && (unsigned char) end[-1] <= ' ') end--;
        while (start < end && (unsigned char) *start <= ' ') start++;
        if (end <= start) continue;
        size_t len = (size_t) (end - start);
        if (len >= TAGCACHE_TAG_MAX) len = TAGCACHE_TAG_MAX - 1;
        memcpy(names[n], start, len);
        names[n][len] = '\0';
        bool duplicate = false;
        for (int i = 0; i < n; i++) if (ascii_casecmp(names[i], names[n]) == 0) duplicate = true;
        if (!duplicate) n++;
    }
    if (!n) { snprintf(names[0], TAGCACHE_PATH_MAX, "%s", raw); n = 1; }
    return n;
}

bool tagcache_artist_matches(const char *raw, const char *name) {
    char names[TAGCACHE_ARTIST_SPLIT_MAX][TAGCACHE_PATH_MAX];
    int n = split_artist(raw, names);
    for (int i = 0; i < n; i++) if (ascii_casecmp(names[i], name ? name : "") == 0) return true;
    return false;
}

void tagcache_artist_primary(const char *raw, char *out, size_t size) {
    if (!out || !size) return;
    char names[TAGCACHE_ARTIST_SPLIT_MAX][TAGCACHE_PATH_MAX];
    split_artist(raw, names);
    snprintf(out, size, "%s", names[0]);
}

static void reader_reset_cache(void) {
    for (int i = 0; i < READER_SORT_CACHE_ROWS; i++) {
        reader_sort_cache[i].valid = false;
        reader_sort_cache[i].loaded = 0;
    }
}

static void reader_song_rebase(tagcache_song_t *s) {
    s->path = s->path_storage; s->title = s->title_storage;
    s->artist = s->artist_storage; s->album = s->album_storage;
    s->album_artist = s->album_artist_storage; s->genre = s->genre_storage;
}

static bool reader_sort_song_fields(int32_t slot, unsigned fields, tagcache_song_t *out) {
    unsigned bucket = (unsigned) slot % READER_SORT_CACHE_ROWS;
    const char *disabled = getenv("TAGCACHE_TEST_DISABLE_READER_CACHE");
    if (disabled && atoi(disabled)) {
        if (!reader_song_fields(slot, fields, out)) {
            reader_sort_failed = true;
            return false;
        }
        return true;
    }
    bool hit = reader_sort_cache[bucket].valid && reader_sort_cache[bucket].slot == slot;
    unsigned loaded = hit ? reader_sort_cache[bucket].loaded : 0;
    if (!hit || (loaded & fields) != fields) {
        unsigned requested = loaded | fields;
        tagcache_song_t song;
        if (!reader_song_fields(slot, requested, &song)) {
            reader_sort_failed = true;
            return false;
        }
        if (requested & TAGCACHE_FIELD_ALBUM_ARTIST) requested |= TAGCACHE_FIELD_ARTIST;
        reader_sort_cache[bucket].song = song;
        reader_song_rebase(&reader_sort_cache[bucket].song);
        reader_sort_cache[bucket].slot = slot;
        reader_sort_cache[bucket].loaded = requested;
        reader_sort_cache[bucket].valid = true;
    }
    *out = reader_sort_cache[bucket].song;
    reader_song_rebase(out);
    return true;
}

typedef struct { uint32_t hash; int32_t slot; } reader_path_key_t;
typedef struct { int32_t slot, ordinal; } reader_member_t;
typedef struct { reader_member_t representative; int32_t first_id, count, start; } reader_group_record_t;
typedef struct { int32_t slot; unsigned char title[TAGCACHE_SORT_TEXT_PREFIX]; } reader_title_sort_t;
typedef struct {
    int32_t slot, first_seen;
    unsigned char path[TAGCACHE_SORT_PATH_PREFIX];
} reader_recency_sort_t;
typedef struct {
    reader_member_t member;
    unsigned char name[TAGCACHE_SORT_TEXT_PREFIX], aa[TAGCACHE_SORT_TEXT_PREFIX];
    union {
        unsigned char album[TAGCACHE_SORT_TEXT_PREFIX];
        struct { int32_t disc, track; } album_order;
    } secondary;
    unsigned char path[TAGCACHE_SORT_PATH_PREFIX];
} reader_member_sort_t;

_Static_assert(offsetof(reader_recency_sort_t, path) == 8, "recency path offset changed");
_Static_assert(sizeof(reader_recency_sort_t) == 72, "recency sort layout changed");
_Static_assert(offsetof(reader_member_sort_t, path) == 128, "member path offset changed");
_Static_assert(sizeof(reader_member_sort_t) == 192, "member sort layout changed");

#define READER_TEMP_BATCH_BYTES (64u * 1024u)

static int reader_compare_path(const void *a, const void *b, void *context) {
    (void) context;
    const reader_path_key_t *ka = a, *kb = b;
    if (ka->hash != kb->hash) return ka->hash < kb->hash ? -1 : 1;
    return (ka->slot > kb->slot) - (ka->slot < kb->slot);
}

static int reader_compare_title_sort(const void *a, const void *b, void *context) {
    (void) context;
    const reader_title_sort_t *ka = a, *kb = b;
    bool full_compare;
    int c = tagcache_sort_prefix_compare(ka->title, kb->title, sizeof(ka->title), &full_compare);
    if (!c && full_compare) {
        tagcache_song_t sa, sb;
        if (!reader_sort_song_fields(ka->slot, TAGCACHE_FIELD_TITLE, &sa) ||
            !reader_sort_song_fields(kb->slot, TAGCACHE_FIELD_TITLE, &sb)) {
            reader_sort_failed = true;
            return (ka->slot > kb->slot) - (ka->slot < kb->slot);
        }
        c = ascii_casecmp(sa.title, sb.title);
    }
    return c ? c : (ka->slot > kb->slot) - (ka->slot < kb->slot);
}

static int reader_compare_recency_sort(const void *a, const void *b, void *context) {
    (void) context;
    const reader_recency_sort_t *ka = a, *kb = b;
    if (ka->first_seen != kb->first_seen) return ka->first_seen > kb->first_seen ? -1 : 1;
    if (!reader_compact_sort) {
        bool full_compare;
        int c = tagcache_sort_prefix_compare(ka->path, kb->path, sizeof(ka->path), &full_compare);
        if (!c && full_compare) {
            tagcache_song_t sa, sb;
            if (!reader_sort_song_fields(ka->slot, TAGCACHE_FIELD_PATH, &sa) ||
                !reader_sort_song_fields(kb->slot, TAGCACHE_FIELD_PATH, &sb)) {
                reader_sort_failed = true;
                return (ka->slot > kb->slot) - (ka->slot < kb->slot);
            }
            c = ascii_casecmp(sa.path, sb.path);
        }
        if (c) return c;
    }
    return (ka->slot > kb->slot) - (ka->slot < kb->slot);
}

static int reader_compare_order(const void *a, const void *b, void *context) {
    bool recency = *(bool *) context;
    int32_t ia = *(const int32_t *) a, ib = *(const int32_t *) b;
    tagcache_song_t sa, sb;
    unsigned fields = recency ? 0 : TAGCACHE_FIELD_TITLE;
    if (!reader_sort_song_fields(ia, fields, &sa) || !reader_sort_song_fields(ib, fields, &sb)) {
        reader_sort_failed = true;
        return (ia > ib) - (ia < ib);
    }
    int c;
    if (recency && sa.first_seen != sb.first_seen) return sa.first_seen > sb.first_seen ? -1 : 1;
    if (recency) {
        if (reader_compact_sort) c = 0;
        else {
            /* Paths are needed only when first_seen ties. */
            if (!reader_sort_song_fields(ia, TAGCACHE_FIELD_PATH, &sa) ||
                !reader_sort_song_fields(ib, TAGCACHE_FIELD_PATH, &sb)) {
                reader_sort_failed = true;
                return (ia > ib) - (ia < ib);
            }
            c = ascii_casecmp(sa.path, sb.path);
        }
    } else c = ascii_casecmp(sa.title, sb.title);
    return c ? c : (ia > ib) - (ia < ib);
}

static void reader_member_names(int kind, const reader_member_t *member, const tagcache_song_t *song,
                                char *name, char *aa) {
    aa[0] = '\0';
    if (kind == TAGCACHE_GROUP_ARTIST) {
        char names[TAGCACHE_ARTIST_SPLIT_MAX][TAGCACHE_PATH_MAX];
        int n = split_artist(song->artist, names);
        snprintf(name, TAGCACHE_PATH_MAX, "%s", member->ordinal >= 0 && member->ordinal < n ? names[member->ordinal] : "");
    } else if (kind == TAGCACHE_GROUP_ALBUM_ARTIST) {
        snprintf(name, TAGCACHE_PATH_MAX, "%s", song->album_artist);
    } else {
        snprintf(name, TAGCACHE_PATH_MAX, "%s", song->album);
        snprintf(aa, TAGCACHE_PATH_MAX, "%s", song->album_artist);
    }
}

static unsigned reader_member_name_field(int kind) {
    return kind == TAGCACHE_GROUP_ARTIST ? TAGCACHE_FIELD_ARTIST :
        kind == TAGCACHE_GROUP_ALBUM_ARTIST ? TAGCACHE_FIELD_ALBUM_ARTIST : TAGCACHE_FIELD_ALBUM;
}

static bool reader_member_sort_string(int kind, const reader_member_t *member, unsigned field,
                                      bool album_artist, char *out) {
    tagcache_song_t song;
    if (!reader_sort_song_fields(member->slot, field, &song)) {
        reader_sort_failed = true;
        return false;
    }
    char name[TAGCACHE_PATH_MAX], aa[TAGCACHE_PATH_MAX];
    reader_member_names(kind, member, &song, name, aa);
    snprintf(out, TAGCACHE_PATH_MAX, "%s", album_artist ? aa : name);
    return true;
}

static bool reader_make_member_sort(int kind, const reader_member_t *member,
                                    const tagcache_song_t *song, reader_member_sort_t *key) {
    char name[TAGCACHE_PATH_MAX], aa[TAGCACHE_PATH_MAX];
    memset(key, 0, sizeof(*key));
    key->member = *member;
    reader_member_names(kind, member, song, name, aa);
    if (!tagcache_sort_prefix(name, key->name, sizeof(key->name)) ||
        !tagcache_sort_prefix(aa, key->aa, sizeof(key->aa))) {
        reader_sort_failed = true;
        return false;
    }
    if (kind == TAGCACHE_GROUP_ALBUM) {
        key->secondary.album_order.disc = song->disc_number > 0 ? song->disc_number : 1;
        key->secondary.album_order.track = song->track_number;
    } else if (!tagcache_sort_prefix(song->album, key->secondary.album, sizeof(key->secondary.album))) {
        reader_sort_failed = true;
        return false;
    }
    if (!reader_compact_sort && !tagcache_sort_prefix(song->path, key->path, sizeof(key->path))) {
        reader_sort_failed = true;
        return false;
    }
    return true;
}

static int reader_compare_member_sort(const void *a, const void *b, void *context) {
    int kind = *(const int *)context;
    const reader_member_sort_t *ka = a, *kb = b;
    bool full_compare;
    int c = tagcache_sort_prefix_compare(ka->name, kb->name, sizeof(ka->name), &full_compare);
    if (!c && full_compare) {
        char na[TAGCACHE_PATH_MAX], nb[TAGCACHE_PATH_MAX];
        if (!reader_member_sort_string(kind, &ka->member, reader_member_name_field(kind), false, na) ||
            !reader_member_sort_string(kind, &kb->member, reader_member_name_field(kind), false, nb)) {
            reader_sort_failed = true;
            return (ka->member.slot > kb->member.slot) - (ka->member.slot < kb->member.slot);
        }
        c = ascii_casecmp(na, nb);
    }
    if (c) return c;

    c = tagcache_sort_prefix_compare(ka->aa, kb->aa, sizeof(ka->aa), &full_compare);
    if (!c && full_compare) {
        char aa[TAGCACHE_PATH_MAX], ab[TAGCACHE_PATH_MAX];
        if (!reader_member_sort_string(kind, &ka->member, TAGCACHE_FIELD_ALBUM_ARTIST, true, aa) ||
            !reader_member_sort_string(kind, &kb->member, TAGCACHE_FIELD_ALBUM_ARTIST, true, ab)) {
            reader_sort_failed = true;
            return (ka->member.slot > kb->member.slot) - (ka->member.slot < kb->member.slot);
        }
        c = ascii_casecmp(aa, ab);
    }
    if (c) return c;

    if (kind == TAGCACHE_GROUP_ALBUM) {
        int32_t da = ka->secondary.album_order.disc, db = kb->secondary.album_order.disc;
        if (da != db) return da < db ? -1 : 1;
        int32_t ta = ka->secondary.album_order.track, tb = kb->secondary.album_order.track;
        if (ta > 0 || tb > 0) {
            if (ta <= 0) return 1;
            if (tb <= 0) return -1;
            if (ta != tb) return ta < tb ? -1 : 1;
        }
    } else {
        c = tagcache_sort_prefix_compare(ka->secondary.album, kb->secondary.album,
                                         sizeof(ka->secondary.album), &full_compare);
        if (!c && full_compare) {
            tagcache_song_t sa, sb;
            if (!reader_sort_song_fields(ka->member.slot, TAGCACHE_FIELD_ALBUM, &sa) ||
                !reader_sort_song_fields(kb->member.slot, TAGCACHE_FIELD_ALBUM, &sb)) {
                reader_sort_failed = true;
                return (ka->member.slot > kb->member.slot) - (ka->member.slot < kb->member.slot);
            }
            c = ascii_casecmp(sa.album, sb.album);
        }
        if (c) return c;
    }

    if (!reader_compact_sort) {
        c = tagcache_sort_prefix_compare(ka->path, kb->path, sizeof(ka->path), &full_compare);
        if (!c && full_compare) {
            tagcache_song_t sa, sb;
            if (!reader_sort_song_fields(ka->member.slot, TAGCACHE_FIELD_PATH, &sa) ||
                !reader_sort_song_fields(kb->member.slot, TAGCACHE_FIELD_PATH, &sb)) {
                reader_sort_failed = true;
                return (ka->member.slot > kb->member.slot) - (ka->member.slot < kb->member.slot);
            }
            c = ascii_casecmp(sa.path, sb.path);
        }
        if (c) return c;
    }
    return (ka->member.slot > kb->member.slot) - (ka->member.slot < kb->member.slot);
}

static int reader_compare_member(const void *a, const void *b, void *context) {
    (void) context;
    const reader_member_t *ma = a, *mb = b;
    tagcache_song_t sa, sb;
    unsigned fields = TAGCACHE_FIELD_ALBUM;
    if (reader_sort_kind == TAGCACHE_GROUP_ARTIST) fields |= TAGCACHE_FIELD_ARTIST;
    else fields |= TAGCACHE_FIELD_ALBUM_ARTIST;
    if (!reader_sort_song_fields(ma->slot, fields, &sa) ||
        !reader_sort_song_fields(mb->slot, fields, &sb)) {
        reader_sort_failed = true;
        return (ma->slot > mb->slot) - (ma->slot < mb->slot);
    }
    char na[TAGCACHE_PATH_MAX], aa[TAGCACHE_PATH_MAX], nb[TAGCACHE_PATH_MAX], ab[TAGCACHE_PATH_MAX];
    reader_member_names(reader_sort_kind, ma, &sa, na, aa);
    reader_member_names(reader_sort_kind, mb, &sb, nb, ab);
    int c = ascii_casecmp(na, nb);
    if (!c) c = ascii_casecmp(aa, ab);
    if (c) return c;
    if (reader_sort_kind == TAGCACHE_GROUP_ALBUM) {
        int da = sa.disc_number > 0 ? sa.disc_number : 1, db = sb.disc_number > 0 ? sb.disc_number : 1;
        if (da != db) return da < db ? -1 : 1;
        int ta = sa.track_number, tb = sb.track_number;
        if (ta > 0 || tb > 0) {
            if (ta <= 0) return 1;
            if (tb <= 0) return -1;
            if (ta != tb) return ta < tb ? -1 : 1;
        }
    } else {
        c = ascii_casecmp(sa.album, sb.album);
        if (c) return c;
    }
    if (!reader_compact_sort) {
        /* Path is only needed after every earlier member key ties. */
        if (!reader_sort_song_fields(ma->slot, TAGCACHE_FIELD_PATH, &sa) ||
            !reader_sort_song_fields(mb->slot, TAGCACHE_FIELD_PATH, &sb)) {
            reader_sort_failed = true;
            return (ma->slot > mb->slot) - (ma->slot < mb->slot);
        }
        c = ascii_casecmp(sa.path, sb.path);
    } else c = 0;
    return c ? c : (ma->slot > mb->slot) - (ma->slot < mb->slot);
}

static bool reader_write_group(int kind, const reader_group_record_t *record) {
    reader_group_record_t group = *record;
    if (kind == TAGCACHE_GROUP_ARTIST) {
        tagcache_song_t song;
        char name[TAGCACHE_PATH_MAX], aa[TAGCACHE_PATH_MAX], canonical[TAGCACHE_PATH_MAX];
        unsigned fields = TAGCACHE_FIELD_ALBUM_ARTIST;
        if (kind == TAGCACHE_GROUP_ALBUM) fields |= TAGCACHE_FIELD_ALBUM;
        if (!reader_song_fields(group.representative.slot, fields, &song)) return false;
        reader_member_names(kind, &group.representative, &song, name, aa);
        const char *value = scan_intern_find(name, canonical, sizeof(canonical)) ? canonical : name;
        if (update_failed) return false;
        int32_t length = (int32_t)strlen(value) + 1;
        off_t position = lseek(reader_artist_names_fd, 0, SEEK_END);
        if (position < 0 || position > INT32_MAX - length - (int32_t)sizeof(length) ||
            !write_fully(reader_artist_names_fd, &length, sizeof(length)) ||
            !write_fully(reader_artist_names_fd, value, (size_t)length)) return false;
        group.representative = (reader_member_t){ (int32_t)position, -1 };
    }
    if (!tc_sort_io_write(reader_group_fd[kind], &group, sizeof(group),
                          (off_t)reader_group_n[kind] * sizeof(group))) return false;
    reader_group_n[kind]++;
    return true;
}

static bool reader_build_groups(int kind, int scratch) {
    int keys = reader_create_temp();
    int sort_keys = reader_create_temp();
    reader_group_fd[kind] = reader_create_temp();
    reader_members_fd[kind] = reader_create_temp();
    bool ok = keys >= 0 && sort_keys >= 0 && reader_group_fd[kind] >= 0 && reader_members_fd[kind] >= 0;
    size_t sort_record_size = reader_compact_sort ? offsetof(reader_member_sort_t, path) : sizeof(reader_member_sort_t);
    size_t sort_batch_capacity = READER_TEMP_BATCH_BYTES / sort_record_size;
    unsigned char *batch = NULL;
    if (ok && sort_batch_capacity == 0) ok = false;
    size_t batch_used = 0;
    int64_t count = 0;
    unsigned fields = kind == TAGCACHE_GROUP_ARTIST ? TAGCACHE_FIELD_ARTIST | TAGCACHE_FIELD_ALBUM :
        kind == TAGCACHE_GROUP_ALBUM_ARTIST ? TAGCACHE_FIELD_ALBUM_ARTIST | TAGCACHE_FIELD_ALBUM :
        TAGCACHE_FIELD_ALBUM | TAGCACHE_FIELD_ALBUM_ARTIST;
    if (!reader_compact_sort) fields |= TAGCACHE_FIELD_PATH;
    reader_sort_kind = kind;
    reader_sort_failed = false;
    for (int32_t slot = 0; ok && slot < ent_count; slot++) {
        struct index_entry idx;
        ok = reader_index(slot, &idx);
        if (!ok || (idx.flag & FLAG_DELETED)) continue;
        tagcache_song_t song;
        ok = reader_sort_song_fields(slot, fields, &song);
        if (!ok) break;
        int n = 1;
        if (kind == TAGCACHE_GROUP_ARTIST) {
            char names[TAGCACHE_ARTIST_SPLIT_MAX][TAGCACHE_PATH_MAX];
            n = split_artist(song.artist, names);
        }
        for (int j = 0; ok && j < n; j++) {
            reader_member_t member = {slot, j};
            reader_member_sort_t key;
            ok = reader_make_member_sort(kind, &member, &song, &key);
            if (ok && batch == NULL) {
                batch = (unsigned char *)malloc(READER_TEMP_BATCH_BYTES);
                if (batch == NULL) ok = false;
            }
            if (ok) {
                memcpy(batch + batch_used * sort_record_size, &key, sort_record_size);
                batch_used++;
                count++;
                if (batch_used == sort_batch_capacity) {
                    ok = tc_sort_io_write(sort_keys, batch, batch_used * sort_record_size,
                                          (off_t)(count - (int64_t)batch_used) * (off_t)sort_record_size);
                    if (ok) batch_used = 0;
                }
            }
        }
    }
    if (ok && batch_used != 0)
        ok = tc_sort_io_write(sort_keys, batch, batch_used * sort_record_size,
                              (off_t)(count - (int64_t)batch_used) * (off_t)sort_record_size);
    free(batch);
    batch = NULL;
    if (ok) ok = tc_sort_fd(sort_keys, sort_record_size, count,
                            reader_compare_member_sort, &kind, scratch) && !reader_sort_failed;
    if (ok && count != 0) {
        batch = (unsigned char *)malloc(READER_TEMP_BATCH_BYTES);
        if (batch == NULL) ok = false;
    }
    for (int64_t at = 0; ok && at < count;) {
        size_t n = (size_t)(count - at);
        size_t capacity = READER_TEMP_BATCH_BYTES / sort_record_size;
        if (n > capacity) n = capacity;
        ok = stats_read_at(sort_keys, batch, n * sort_record_size, (off_t)at * (off_t)sort_record_size) == STATS_READ_OK;
        if (ok) {
            for (size_t j = 0; j < n; j++) {
                reader_member_t member;
                memcpy(&member, batch + j * sort_record_size, sizeof(member));
                memcpy(batch + j * sizeof(member), &member, sizeof(member));
            }
            ok = tc_sort_io_write(keys, batch, n * sizeof(reader_member_t),
                                  (off_t)at * (off_t)sizeof(reader_member_t));
        }
        at += (int64_t)n;
    }
    reader_group_record_t group = {0};
    char previous[TAGCACHE_PATH_MAX] = "", previous_aa[TAGCACHE_PATH_MAX] = "";
    unsigned name_fields = kind == TAGCACHE_GROUP_ARTIST ? TAGCACHE_FIELD_ARTIST :
        kind == TAGCACHE_GROUP_ALBUM_ARTIST ? TAGCACHE_FIELD_ALBUM_ARTIST :
        TAGCACHE_FIELD_ALBUM | TAGCACHE_FIELD_ALBUM_ARTIST;
    size_t member_batch_capacity = READER_TEMP_BATCH_BYTES / sizeof(reader_member_t);
    for (int64_t at = 0; ok && at < count;) {
        size_t n = (size_t)(count - at);
        if (n > member_batch_capacity) n = member_batch_capacity;
        ok = stats_read_at(keys, batch, n * sizeof(reader_member_t),
                           (off_t)at * (off_t)sizeof(reader_member_t)) == STATS_READ_OK;
        for (size_t j = 0; ok && j < n; j++) {
            reader_member_t member;
            tagcache_song_t song;
            char name[TAGCACHE_PATH_MAX], aa[TAGCACHE_PATH_MAX];
            memcpy(&member, batch + j * sizeof(member), sizeof(member));
            ok = reader_sort_song_fields(member.slot, name_fields, &song);
            if (!ok) break;
            reader_member_names(kind, &member, &song, name, aa);
            if (!group.count || ascii_casecmp(previous, name) || ascii_casecmp(previous_aa, aa)) {
                if (group.count) ok = reader_write_group(kind, &group);
                group = (reader_group_record_t) {member, member.slot + 1, 0, (int32_t)(at + (int64_t)j)};
                snprintf(previous, sizeof(previous), "%s", name);
                snprintf(previous_aa, sizeof(previous_aa), "%s", aa);
            }
            group.count++;
            if (member.slot + 1 < group.first_id) {
                group.first_id = member.slot + 1;
                group.representative = member;
            }
            int32_t id = member.slot + 1;
            memcpy(batch + j * sizeof(id), &id, sizeof(id));
        }
        if (ok) ok = tc_sort_io_write(reader_members_fd[kind], batch, n * sizeof(int32_t),
                                      (off_t)at * (off_t)sizeof(int32_t));
        at += (int64_t)n;
    }
    if (ok && group.count) ok = reader_write_group(kind, &group);
    free(batch);
    if (keys >= 0) close(keys);
    if (sort_keys >= 0) close(sort_keys);
    return ok;
}

static bool reader_build_indexes(void) {
    reader_close_indexes();
    reader_reset_cache();
    if (!scan_intern_init(!reader_compact_sort)) return false;
    reader_artist_names_fd = reader_create_temp();
    reader_title_fd = reader_create_temp();
    reader_recency_fd = reader_create_temp();
    reader_path_fd = reader_create_temp();
    int title_keys = reader_create_temp();
    int recency_keys = reader_create_temp();
    int scratch = reader_create_temp();
    bool ok = reader_artist_names_fd >= 0 && reader_title_fd >= 0 && reader_recency_fd >= 0 && reader_path_fd >= 0 &&
              title_keys >= 0 && recency_keys >= 0 && scratch >= 0;
    size_t title_record_size = sizeof(reader_title_sort_t);
    size_t recency_record_size = reader_compact_sort ? offsetof(reader_recency_sort_t, path) : sizeof(reader_recency_sort_t);
    size_t path_record_size = sizeof(reader_path_key_t);
    size_t decorate_capacity = READER_TEMP_BATCH_BYTES / (title_record_size + recency_record_size + path_record_size);
    unsigned char *batch = NULL;
    if (ok && decorate_capacity == 0) ok = false;
    size_t batch_used = 0;
    live_count = 0;
    reader_sort_failed = false;
    for (int32_t slot = 0; ok && slot < ent_count; slot++) {
        struct index_entry idx;
        ok = reader_index(slot, &idx);
        if (!ok || (idx.flag & FLAG_DELETED)) continue;
        tagcache_song_t song;
        ok = reader_sort_song_fields(slot, TAGCACHE_FIELD_TITLE | TAGCACHE_FIELD_PATH, &song);
        if (!ok || !song.path[0]) { reader_sort_failed = true; ok = false; break; }
        reader_title_sort_t title = {0};
        reader_recency_sort_t recency = {0};
        title.slot = slot;
        recency.slot = slot;
        recency.first_seen = idx.tag_seek[tag_commitid];
        if (!tagcache_sort_prefix(song.title, title.title, sizeof(title.title)) ||
            (!reader_compact_sort && !tagcache_sort_prefix(song.path, recency.path, sizeof(recency.path)))) {
            reader_sort_failed = true;
            ok = false;
            break;
        }
        reader_path_key_t path = {fnv1a(song.path, strlen(song.path)), slot};
        if (batch == NULL) {
            batch = (unsigned char *)malloc(READER_TEMP_BATCH_BYTES);
            if (batch == NULL) { ok = false; break; }
        }
        memcpy(batch + batch_used * title_record_size, &title, title_record_size);
        unsigned char *recency_batch = batch + decorate_capacity * title_record_size;
        unsigned char *path_batch = recency_batch + decorate_capacity * recency_record_size;
        memcpy(recency_batch + batch_used * recency_record_size, &recency, recency_record_size);
        memcpy(path_batch + batch_used * path_record_size, &path, path_record_size);
        batch_used++;
        live_count++;
        if (batch_used == decorate_capacity) {
            off_t first = (off_t)(live_count - (int32_t)batch_used);
            ok = tc_sort_io_write(title_keys, batch, batch_used * title_record_size,
                                  first * (off_t)title_record_size) &&
                 tc_sort_io_write(recency_keys, recency_batch, batch_used * recency_record_size,
                                  first * (off_t)recency_record_size) &&
                 tc_sort_io_write(reader_path_fd, path_batch, batch_used * path_record_size,
                                  first * (off_t)path_record_size);
            if (ok) batch_used = 0;
        }
    }
    if (ok && batch_used != 0) {
        unsigned char *recency_batch = batch + decorate_capacity * title_record_size;
        unsigned char *path_batch = recency_batch + decorate_capacity * recency_record_size;
        off_t first = (off_t)(live_count - (int32_t)batch_used);
        ok = tc_sort_io_write(title_keys, batch, batch_used * title_record_size,
                              first * (off_t)title_record_size) &&
             tc_sort_io_write(recency_keys, recency_batch, batch_used * recency_record_size,
                              first * (off_t)recency_record_size) &&
             tc_sort_io_write(reader_path_fd, path_batch, batch_used * path_record_size,
                              first * (off_t)path_record_size);
    }
    free(batch);
    batch = NULL;
    if (ok) ok = tc_sort_fd(title_keys, title_record_size, live_count,
                            reader_compare_title_sort, NULL, scratch) && !reader_sort_failed;
    if (ok) ok = tc_sort_fd(recency_keys, recency_record_size, live_count,
                            reader_compare_recency_sort, NULL, scratch) && !reader_sort_failed;
    if (ok) ok = tc_sort_fd(reader_path_fd, sizeof(reader_path_key_t), live_count, reader_compare_path, NULL, scratch);
    if (ok && live_count != 0) {
        batch = (unsigned char *)malloc(READER_TEMP_BATCH_BYTES);
        if (batch == NULL) ok = false;
    }
    size_t writeback_capacity = READER_TEMP_BATCH_BYTES / (title_record_size + recency_record_size);
    unsigned char *title_batch = batch;
    unsigned char *recency_batch = batch ? batch + writeback_capacity * title_record_size : NULL;
    for (int32_t rank = 0; ok && rank < live_count;) {
        size_t n = (size_t)(live_count - rank);
        if (n > writeback_capacity) n = writeback_capacity;
        ok = stats_read_at(title_keys, title_batch, n * title_record_size,
                           (off_t)rank * (off_t)title_record_size) == STATS_READ_OK &&
             stats_read_at(recency_keys, recency_batch, n * recency_record_size,
                           (off_t)rank * (off_t)recency_record_size) == STATS_READ_OK;
        if (ok) {
            for (size_t j = 0; j < n; j++) {
                int32_t slot;
                memcpy(&slot, title_batch + j * title_record_size, sizeof(slot));
                memcpy(title_batch + j * sizeof(slot), &slot, sizeof(slot));
                memcpy(&slot, recency_batch + j * recency_record_size, sizeof(slot));
                memcpy(recency_batch + j * sizeof(slot), &slot, sizeof(slot));
            }
            ok = tc_sort_io_write(reader_title_fd, title_batch, n * sizeof(int32_t),
                                  (off_t)rank * (off_t)sizeof(int32_t)) &&
                 tc_sort_io_write(reader_recency_fd, recency_batch, n * sizeof(int32_t),
                                  (off_t)rank * (off_t)sizeof(int32_t));
        }
        rank += (int32_t)n;
    }
    for (int kind = 0; ok && kind < 2; kind++) {
        reader_rank_fd[kind] = reader_create_temp();
        ok = reader_rank_fd[kind] >= 0 && ftruncate(reader_rank_fd[kind], (off_t) ent_count * sizeof(int32_t)) == 0;
        for (int32_t rank = 0; ok && rank < live_count; rank++) {
            int32_t slot, stored = rank + 1;
            ok = stats_read_at(kind ? reader_recency_fd : reader_title_fd, &slot, sizeof(slot),
                               (off_t) rank * sizeof(slot)) == STATS_READ_OK &&
                 tc_sort_io_write(reader_rank_fd[kind], &stored, sizeof(stored), (off_t) slot * sizeof(stored));
        }
    }
    for (int kind = 0; ok && kind < 3; kind++) ok = reader_build_groups(kind, scratch);
    if (scratch >= 0) close(scratch);
    if (title_keys >= 0) close(title_keys);
    if (recency_keys >= 0) close(recency_keys);
    free(batch);
    reader_reset_cache();
    if (!ok) reader_close_indexes();
    return ok;
}

static int32_t reader_find_path(const char *path) {
    if (reader_legacy_active) return legacy_find_path(path);
    uint32_t hash = fnv1a(path, strlen(path));
    int32_t lo = 0, hi = live_count;
    reader_path_key_t key;
    while (lo < hi) {
        int32_t mid = lo + (hi - lo) / 2;
        if (stats_read_at(reader_path_fd, &key, sizeof(key), (off_t) mid * sizeof(key)) != STATS_READ_OK) return -1;
        if (key.hash < hash) lo = mid + 1; else hi = mid;
    }
    for (; lo < live_count; lo++) {
        if (stats_read_at(reader_path_fd, &key, sizeof(key), (off_t) lo * sizeof(key)) != STATS_READ_OK || key.hash != hash)
            break;
        struct index_entry idx;
        char value[TAGCACHE_PATH_MAX];
        if (!reader_index(key.slot, &idx) ||
            !reader_string(tag_filename, idx.tag_seek[tag_filename], key.slot, value, sizeof(value))) return -1;
        if (!strcmp(value, path)) return key.slot;
    }
    return -1;
}

static bool reader_order_song(int fd, bool recency, int32_t rank, tagcache_song_t *out) {
    if (reader_legacy_active) return legacy_order_song(recency, rank, out);
    int32_t slot;
    return rank >= 0 && rank < live_count &&
           stats_read_at(fd, &slot, sizeof(slot), (off_t) rank * sizeof(slot)) == STATS_READ_OK &&
           (out ? reader_song(slot, out) : true);
}

static bool reader_group_at(int kind, int index, tagcache_group_t *out) {
    if (reader_legacy_active) return legacy_group_at(kind, index, out);
    if (kind < 0 || kind > 2 || index < 0 || index >= reader_group_n[kind]) return false;
    reader_group_record_t group;
    if (stats_read_at(reader_group_fd[kind], &group, sizeof(group), (off_t) index * sizeof(group)) != STATS_READ_OK)
        return false;
    if (!out) return true;
    if (kind == TAGCACHE_GROUP_ARTIST) {
        int32_t length;
        if (group.representative.ordinal != -1 || group.representative.slot < 0 ||
            !reader_read_at(reader_artist_names_fd, &length, sizeof(length), group.representative.slot) ||
            length <= 0 || (size_t)length > sizeof(out->name_storage) ||
            !reader_read_at(reader_artist_names_fd, out->name_storage, (size_t)length,
                            (off_t)group.representative.slot + sizeof(length)) ||
            out->name_storage[length - 1] != '\0' || memchr(out->name_storage, '\0', (size_t)length - 1)) return false;
        out->album_artist_storage[0] = '\0';
    } else {
        tagcache_song_t song;
        unsigned fields = TAGCACHE_FIELD_ALBUM_ARTIST;
        if (kind == TAGCACHE_GROUP_ALBUM) fields |= TAGCACHE_FIELD_ALBUM;
        if (!reader_song_fields(group.representative.slot, fields, &song)) return false;
        reader_member_names(kind, &group.representative, &song, out->name_storage, out->album_artist_storage);
    }
    out->name = out->name_storage;
    out->album_artist = out->album_artist_storage;
    out->song_count = group.count;
    out->first_song_id = group.first_id;
    return true;
}

static int reader_group_index(int kind, const char *name, const char *aa) {
    if (reader_legacy_active) return legacy_group_index(kind, name, aa);
    if (kind < 0 || kind > 2) return -1;
    int lo = 0, hi = reader_group_n[kind];
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        tagcache_group_t group;
        if (!reader_group_at(kind, mid, &group)) return -1;
        int c = ascii_casecmp(group.name, name ? name : "");
        if (!c && kind == TAGCACHE_GROUP_ALBUM) c = ascii_casecmp(group.album_artist, aa ? aa : "");
        if (!c) return mid;
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    return -1;
}

static int reader_group_ids(int kind, int group_index, int offset, int32_t *ids, int max) {
    if (reader_legacy_active) return legacy_group_ids(kind, group_index, offset, ids, max);
    if (kind < 0 || kind > 2 || group_index < 0 || group_index >= reader_group_n[kind] || !ids || max <= 0) return 0;
    if (offset < 0) offset = 0;
    reader_group_record_t group;
    if (stats_read_at(reader_group_fd[kind], &group, sizeof(group), (off_t) group_index * sizeof(group)) != STATS_READ_OK)
        return 0;
    if (offset >= group.count) return 0;
    int n = group.count - offset;
    if (n > max) n = max;
    if (stats_read_at(reader_members_fd[kind], ids, (size_t) n * sizeof(*ids),
                      ((off_t) group.start + offset) * sizeof(*ids)) != STATS_READ_OK) return 0;
    return n;
}

static int32_t reader_rank_of(int32_t slot, bool recency) {
    if (reader_legacy_active) return legacy_rank_of(slot, recency);
    int32_t rank;
    if (slot < 0 || slot >= ent_count || stats_read_at(reader_rank_fd[recency ? 1 : 0], &rank, sizeof(rank),
                                                    (off_t) slot * sizeof(rank)) != STATS_READ_OK) return -1;
    return rank - 1;
}
