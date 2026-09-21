#ifndef HIBY_TAGCACHE_READER_LEGACY_H
#define HIBY_TAGCACHE_READER_LEGACY_H

/* Legacy queries retain fixed pages and traverse the read-only source files. */
#define LEGACY_PAGE 64
static struct { int32_t slots[LEGACY_PAGE]; int base, count; } legacy_orders[2];
typedef struct {
    char name[TAGCACHE_PATH_MAX], aa[TAGCACHE_PATH_MAX];
    int32_t first_id, count;
} legacy_group_t;
static struct { legacy_group_t rows[LEGACY_PAGE]; int base, count, total; } legacy_groups[3];

static struct { char key[TAGCACHE_PATH_MAX], value[TAGCACHE_PATH_MAX]; bool valid; } legacy_text_cache[64];

static bool reader_legacy_init(void) {
    reader_legacy_active = false;
    memset(legacy_text_cache, 0, sizeof(legacy_text_cache));
    reader_compact_order = !choose_intern_strings(ent_count);
    memset(legacy_orders, 0, sizeof(legacy_orders));
    memset(legacy_groups, 0, sizeof(legacy_groups));
    for (int k = 0; k < 3; k++) legacy_groups[k].total = -1;
    live_count = 0;
    for (int32_t slot = 0; slot < ent_count; slot++) {
        struct index_entry idx;
        tagcache_song_t song;
        if (!reader_index(slot, &idx)) return false;
        if (idx.flag & FLAG_DELETED) continue;
        if (!reader_song(slot, &song) || !song.path[0]) return false;
        live_count++;
    }
    reader_legacy_active = true;
    return true;
}

static void legacy_canonical_text(char *value) {
    if (!value[0]) return;
    unsigned bucket = fnv1a(value, strlen(value)) % 64;
    if (legacy_text_cache[bucket].valid && !strcmp(legacy_text_cache[bucket].key, value)) {
        snprintf(value, TAGCACHE_PATH_MAX, "%s", legacy_text_cache[bucket].value);
        return;
    }
    snprintf(legacy_text_cache[bucket].key, TAGCACHE_PATH_MAX, "%s", value);
    const int tags[] = {tag_title, tag_artist, tag_album, tag_albumartist, tag_genre};
    bool found = false;
    for (int t = reader_compact_order ? 1 : 0; t < 5 && !found; t++) {
        int tag = tags[t];
        for (off_t offset = sizeof(struct tagcache_header); (uint64_t)offset < reader_tag_size[tag];) {
            struct tagfile_entry te;
            char candidate[TAGCACHE_PATH_MAX];
            if (!reader_read_at(reader_tag_fd[tag], &te, sizeof(te), offset) || te.tag_length <= 0 ||
                te.tag_length > TAGCACHE_PATH_MAX ||
                (uint64_t)offset + sizeof(te) + te.tag_length > reader_tag_size[tag] ||
                !reader_read_at(reader_tag_fd[tag], candidate, (size_t)te.tag_length, offset + sizeof(te)) ||
                candidate[te.tag_length - 1] != '\0') return;
            if (!ascii_casecmp(candidate, value)) {
                snprintf(value, TAGCACHE_PATH_MAX, "%s", candidate); found = true; break;
            }
            offset += sizeof(te) + te.tag_length;
        }
    }
    snprintf(legacy_text_cache[bucket].value, TAGCACHE_PATH_MAX, "%s", value);
    legacy_text_cache[bucket].valid = true;
}


static int32_t legacy_find_path(const char *path) {
    if (!path || !*path) return -1;
    for (int32_t slot = 0; slot < ent_count; slot++) {
        struct index_entry idx;
        char value[TAGCACHE_PATH_MAX];
        if (!reader_index(slot, &idx)) return -1;
        if (idx.flag & FLAG_DELETED) continue;
        if (!reader_string(tag_filename, idx.tag_seek[tag_filename], slot, value, sizeof(value))) return -1;
        if (!strcmp(value, path)) return slot;
    }
    return -1;
}

static bool legacy_order_song(bool recency, int32_t rank, tagcache_song_t *out) {
    if (rank < 0 || rank >= live_count) return false;
    int k = recency ? 1 : 0;
    if (rank < legacy_orders[k].base) legacy_orders[k].base = legacy_orders[k].count = 0;
    reader_compact_sort = reader_compact_order;
    reader_sort_failed = false;
    while (rank >= legacy_orders[k].base + legacy_orders[k].count) {
        int32_t after = -1;
        if (legacy_orders[k].count) after = legacy_orders[k].slots[legacy_orders[k].count - 1];
        legacy_orders[k].base += legacy_orders[k].count;
        legacy_orders[k].count = 0;
        for (int32_t slot = 0; slot < ent_count; slot++) {
            struct index_entry idx;
            if (!reader_index(slot, &idx)) { reader_sort_failed = true; break; }
            if (idx.flag & FLAG_DELETED) continue;
            if (after >= 0 && reader_compare_order(&slot, &after, &recency) <= 0) continue;
            int at = 0, count = legacy_orders[k].count;
            while (at < count && reader_compare_order(&legacy_orders[k].slots[at], &slot, &recency) < 0) at++;
            if (at == LEGACY_PAGE) continue;
            if (count < LEGACY_PAGE) count++;
            memmove(&legacy_orders[k].slots[at + 1], &legacy_orders[k].slots[at],
                    (size_t)(count - at - 1) * sizeof(int32_t));
            legacy_orders[k].slots[at] = slot;
            legacy_orders[k].count = count;
        }
        if (reader_sort_failed || !legacy_orders[k].count) { reader_compact_sort = false; return false; }
    }
    reader_compact_sort = false;
    return !out || reader_song(legacy_orders[k].slots[rank - legacy_orders[k].base], out);
}

static int32_t legacy_rank_of(int32_t slot, bool recency) {
    struct index_entry target;
    if (!reader_index(slot, &target) || (target.flag & FLAG_DELETED)) return -1;
    int32_t rank = 0;
    reader_compact_sort = reader_compact_order;
    reader_sort_failed = false;
    for (int32_t i = 0; i < ent_count; i++) {
        struct index_entry idx;
        if (!reader_index(i, &idx)) { reader_sort_failed = true; break; }
        if (!(idx.flag & FLAG_DELETED) && reader_compare_order(&i, &slot, &recency) < 0) rank++;
    }
    reader_compact_sort = false;
    return reader_sort_failed ? -1 : rank;
}

static int legacy_group_compare(const legacy_group_t *a, const legacy_group_t *b) {
    int c = ascii_casecmp(a->name, b->name);
    return c ? c : ascii_casecmp(a->aa, b->aa);
}

static bool legacy_group_page(int kind, bool restart) {
    legacy_group_t after = {0};
    bool have_after = !restart && legacy_groups[kind].count > 0;
    if (have_after) after = legacy_groups[kind].rows[legacy_groups[kind].count - 1];
    legacy_groups[kind].base = restart ? 0 : legacy_groups[kind].base + legacy_groups[kind].count;
    legacy_groups[kind].count = 0;
    for (int32_t slot = 0; slot < ent_count; slot++) {
        struct index_entry idx;
        tagcache_song_t song;
        if (!reader_index(slot, &idx)) return false;
        if (idx.flag & FLAG_DELETED) continue;
        if (!reader_song(slot, &song)) return false;
        char names[TAGCACHE_ARTIST_SPLIT_MAX][TAGCACHE_PATH_MAX];
        int n = kind == TAGCACHE_GROUP_ARTIST ? split_artist(song.artist, names) : 1;
        for (int o = 0; o < n; o++) {
            legacy_group_t key = { .first_id = slot + 1, .count = 1 };
            const char *name = kind == TAGCACHE_GROUP_ARTIST ? names[o] :
                               kind == TAGCACHE_GROUP_ALBUM_ARTIST ? song.album_artist : song.album;
            snprintf(key.name, sizeof(key.name), "%.*s", TAGCACHE_PATH_MAX - 1, name);
            if (kind == TAGCACHE_GROUP_ARTIST) legacy_canonical_text(key.name);
            if (kind == TAGCACHE_GROUP_ALBUM) snprintf(key.aa, sizeof(key.aa), "%s", song.album_artist);
            if (have_after && legacy_group_compare(&key, &after) <= 0) continue;
            int at = 0, count = legacy_groups[kind].count;
            while (at < count && legacy_group_compare(&legacy_groups[kind].rows[at], &key) < 0) at++;
            if (at < count && !legacy_group_compare(&legacy_groups[kind].rows[at], &key)) {
                legacy_groups[kind].rows[at].count++;
                continue;
            }
            if (at == LEGACY_PAGE) continue;
            if (count < LEGACY_PAGE) count++;
            memmove(&legacy_groups[kind].rows[at + 1], &legacy_groups[kind].rows[at],
                    (size_t)(count - at - 1) * sizeof(key));
            legacy_groups[kind].rows[at] = key;
            legacy_groups[kind].count = count;
        }
    }
    if (legacy_groups[kind].count < LEGACY_PAGE)
        legacy_groups[kind].total = legacy_groups[kind].base + legacy_groups[kind].count;
    return true;
}

static int legacy_group_count(int kind) {
    if (kind < 0 || kind > 2) return 0;
    while (legacy_groups[kind].total < 0)
        if (!legacy_group_page(kind, false)) return 0;
    return legacy_groups[kind].total;
}

static bool legacy_group_at(int kind, int index, tagcache_group_t *out) {
    if (kind < 0 || kind > 2 || index < 0 ||
        (legacy_groups[kind].total >= 0 && index >= legacy_groups[kind].total)) return false;
    if (index < legacy_groups[kind].base && !legacy_group_page(kind, true)) return false;
    while (index >= legacy_groups[kind].base + legacy_groups[kind].count) {
        if (!legacy_group_page(kind, false) || !legacy_groups[kind].count) return false;
    }
    if (out) {
        legacy_group_t *g = &legacy_groups[kind].rows[index - legacy_groups[kind].base];
        snprintf(out->name_storage, sizeof(out->name_storage), "%s", g->name);
        snprintf(out->album_artist_storage, sizeof(out->album_artist_storage), "%s", g->aa);
        out->name = out->name_storage; out->album_artist = out->album_artist_storage;
        out->song_count = g->count; out->first_song_id = g->first_id;
    }
    return true;
}

static int legacy_group_index(int kind, const char *name, const char *aa) {
    tagcache_group_t g;
    for (int i = 0; legacy_group_at(kind, i, &g); i++) {
        int c = ascii_casecmp(g.name, name ? name : "");
        if (!c && kind == TAGCACHE_GROUP_ALBUM) c = ascii_casecmp(g.album_artist, aa ? aa : "");
        if (!c) return i;
        if (c > 0) break;
    }
    return -1;
}

static int legacy_group_ids(int kind, int gi, int offset, int32_t *ids, int max) {
    tagcache_group_t group;
    if (!ids || max <= 0 || !legacy_group_at(kind, gi, &group)) return 0;
    if (offset < 0) offset = 0;
    if (offset >= group.song_count) return 0;
    reader_member_t page[LEGACY_PAGE], after = {-1, 0};
    int base = 0, written = 0;
    reader_sort_kind = kind; reader_compact_sort = reader_compact_order; reader_sort_failed = false;
    while (written < max) {
        int count = 0;
        for (int32_t slot = 0; slot < ent_count; slot++) {
            struct index_entry idx;
            tagcache_song_t song;
            if (!reader_index(slot, &idx)) { reader_sort_failed = true; break; }
            if (idx.flag & FLAG_DELETED) continue;
            if (!reader_song(slot, &song)) { reader_sort_failed = true; break; }
            reader_member_t member = {slot, 0};
            if (kind == TAGCACHE_GROUP_ARTIST) {
                char names[TAGCACHE_ARTIST_SPLIT_MAX][TAGCACHE_PATH_MAX];
                int n = split_artist(song.artist, names), o;
                for (o = 0; o < n && ascii_casecmp(names[o], group.name); o++) {}
                if (o == n) continue;
                member.ordinal = o;
            } else if (kind == TAGCACHE_GROUP_ALBUM_ARTIST) {
                if (ascii_casecmp(song.album_artist, group.name)) continue;
            } else if (ascii_casecmp(song.album, group.name) || ascii_casecmp(song.album_artist, group.album_artist)) continue;
            if (after.slot >= 0 && reader_compare_member(&member, &after, NULL) <= 0) continue;
            int at = 0;
            while (at < count && reader_compare_member(&page[at], &member, NULL) < 0) at++;
            if (at == LEGACY_PAGE) continue;
            if (count < LEGACY_PAGE) count++;
            memmove(page + at + 1, page + at, (size_t)(count - at - 1) * sizeof(*page));
            page[at] = member;
        }
        if (reader_sort_failed || !count) break;
        for (int i = 0; i < count && written < max; i++) if (base + i >= offset) ids[written++] = page[i].slot + 1;
        after = page[count - 1]; base += count;
        if (count < LEGACY_PAGE) break;
    }
    reader_compact_sort = false;
    return reader_sort_failed ? 0 : written;
}
#endif
