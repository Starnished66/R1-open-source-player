#ifndef HIBY_TAGCACHE_SCAN_INTERN_H
#define HIBY_TAGCACHE_SCAN_INTERN_H

/* Disk-backed canonical tag table used while a scan appends to staging tag
 * files.  The table stores one reference for each case-insensitive spelling
 * and therefore has no song-sized RAM component. */
#define TC_SCAN_INTERN_SLOTS (8u * 1024u * 1024u)
#define TC_SCAN_INTERN_MASK (TC_SCAN_INTERN_SLOTS - 1u)

struct tc_scan_intern_cell {
    uint32_t hash;
    int32_t seek, tag;
};

static int tc_scan_intern_fd = -1;
static bool tc_scan_intern_ready;
static void scan_intern_close(void);

static uint32_t tc_scan_intern_hash(const char *s) {
    uint32_t h = 2166136261u;
    for (; s && *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + ('a' - 'A'));
        h ^= c; h *= 16777619u;
    }
    return h ? h : 1u;
}

static bool tc_scan_intern_read(int tag, int32_t seek, char *out, size_t cap) {
    struct tagfile_entry te;
    if (tag < 0 || tag >= TAG_COUNT || seek < (int32_t)sizeof(struct tagcache_header) ||
        !out || cap == 0 || reader_tag_fd[tag] < 0 ||
        (uintmax_t)(uint32_t)seek + sizeof(te) > reader_tag_size[tag] ||
        pread(reader_tag_fd[tag], &te, sizeof(te), seek) != (ssize_t)sizeof(te) || te.tag_length <= 0 ||
        (size_t)te.tag_length > cap || (uintmax_t)(uint32_t)seek + sizeof(te) + (size_t)te.tag_length > reader_tag_size[tag] ||
        pread(reader_tag_fd[tag], out, (size_t)te.tag_length, (off_t)seek + sizeof(te)) != te.tag_length)
        return false;
    return memchr(out, '\0', (size_t)te.tag_length - 1) == NULL && out[te.tag_length - 1] == '\0';
}

static bool tc_scan_intern_equal_ci(const char *a, const char *b) {
    while (*a && *b) {
        unsigned char ca = (unsigned char)*a++, cb = (unsigned char)*b++;
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb + ('a' - 'A'));
        if (ca != cb) return false;
    }
    return *a == *b;
}

static int tc_scan_intern_probe(const char *value, uint32_t *slot_out, char *canonical, size_t cap) {
    uint32_t hash = tc_scan_intern_hash(value);
    for (uint32_t n = 0; n < TC_SCAN_INTERN_SLOTS; n++) {
        uint32_t slot = (hash + n) & TC_SCAN_INTERN_MASK;
        struct tc_scan_intern_cell cell;
        if (!reader_read_at(tc_scan_intern_fd, &cell, sizeof(cell), (off_t)slot * sizeof(cell))) return -1;
        if (!cell.hash) { if (slot_out) *slot_out = slot; return 0; }
        if (cell.hash != hash) continue;
        if (!tc_scan_intern_read(cell.tag, cell.seek, canonical, cap)) return -1;
        if (tc_scan_intern_equal_ci(canonical, value)) {
            if (slot_out) *slot_out = slot;
            return 1;
        }
    }
    return -1;
}

static bool scan_intern_init(bool include_titles) {
    scan_intern_close();
    tc_scan_intern_fd = reader_create_temp();
    if (tc_scan_intern_fd < 0 || ftruncate(tc_scan_intern_fd, (off_t)TC_SCAN_INTERN_SLOTS * sizeof(struct tc_scan_intern_cell)) != 0) {
        scan_intern_close(); return false;
    }
    tc_scan_intern_ready = true;
    const int tags[] = {tag_title, tag_artist, tag_album, tag_albumartist, tag_genre};
    for (size_t ti = 0; ti < sizeof(tags) / sizeof(tags[0]); ti++) {
        int tag = tags[ti];
        if (tag == tag_title && !include_titles) continue;
        for (off_t pos = sizeof(struct tagcache_header); pos + (off_t)sizeof(struct tagfile_entry) <= (off_t)reader_tag_size[tag];) {
            struct tagfile_entry te;
            if (pread(reader_tag_fd[tag], &te, sizeof(te), pos) != (ssize_t)sizeof(te) || te.tag_length <= 0 ||
                pos + (off_t)sizeof(te) + te.tag_length > (off_t)reader_tag_size[tag]) return false;
            char value[TAGCACHE_PATH_MAX];
            if ((size_t)te.tag_length > sizeof(value) || !tc_scan_intern_read(tag, (int32_t)pos, value, sizeof(value))) return false;
            uint32_t slot;
            char canonical[TAGCACHE_PATH_MAX];
            int found = tc_scan_intern_probe(value, &slot, canonical, sizeof(canonical));
            if (found < 0) return false;
            if (!found) {
                struct tc_scan_intern_cell cell = {tc_scan_intern_hash(value), (int32_t)pos, tag};
                if (pwrite(tc_scan_intern_fd, &cell, sizeof(cell), (off_t)slot * sizeof(cell)) != (ssize_t)sizeof(cell)) return false;
            }
            pos += sizeof(te) + te.tag_length;
        }
    }
    return true;
}

static bool scan_intern_find(const char *value, char *out, size_t out_size) {
    if (!tc_scan_intern_ready || !value || !out || out_size == 0) return false;
    uint32_t slot; char canonical[TAGCACHE_PATH_MAX];
    int found = tc_scan_intern_probe(value, &slot, canonical, sizeof(canonical));
    if (found < 0) update_failed = true;
    if (found != 1) return false;
    if (strlen(canonical) >= out_size) return false;
    snprintf(out, out_size, "%s", canonical);
    return true;
}

static bool scan_intern_insert(int tag, int32_t seek) {
    if (!tc_scan_intern_ready || tag < 0 || tag >= TAG_COUNT || seek < 0) return false;
    char value[TAGCACHE_PATH_MAX], canonical[TAGCACHE_PATH_MAX];
    if (!tc_scan_intern_read(tag, seek, value, sizeof(value))) return false;
    uint32_t slot;
    int found = tc_scan_intern_probe(value, &slot, canonical, sizeof(canonical));
    if (found < 0) return false;
    if (found == 1) return true;
    struct tc_scan_intern_cell cell = {tc_scan_intern_hash(value), seek, tag};
    return pwrite(tc_scan_intern_fd, &cell, sizeof(cell), (off_t)slot * sizeof(cell)) == (ssize_t)sizeof(cell);
}

static void scan_intern_close(void) {
    if (tc_scan_intern_fd >= 0) close(tc_scan_intern_fd);
    tc_scan_intern_fd = -1;
    tc_scan_intern_ready = false;
}

#endif
