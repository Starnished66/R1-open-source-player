#ifndef HIBY_TAGCACHE_QUERY_H
#define HIBY_TAGCACHE_QUERY_H

static bool query_nonempty(const char *s) { return s && s[0]; }

static int query_album_name_group(const char *name) {
    int lo = 0, hi = tagcache_group_count(TAGCACHE_GROUP_ALBUM);
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        tagcache_group_t group;
        if (!reader_group_at(TAGCACHE_GROUP_ALBUM, mid, &group)) return -1;
        if (ascii_casecmp(group.name, name) < 0) lo = mid + 1;
        else hi = mid;
    }
    tagcache_group_t group;
    return reader_group_at(TAGCACHE_GROUP_ALBUM, lo, &group) && !ascii_casecmp(group.name, name) ? lo : -1;
}

static void query_choose(tagcache_query_t *q, uint32_t category, int key) {
    uint64_t start = 0, count = 0;
    if (key >= 0) query_lookup(category, (uint32_t)key, &start, &count);
    if (!q->indexed || count < q->count) {
        q->indexed = true;
        q->start = start;
        q->count = count;
    }
}

void tagcache_query_begin(tagcache_query_t *q, int domain, const char *needle,
                         const char *artist, const char *album_artist, const char *album) {
    memset(q, 0, sizeof(*q));
    q->domain = domain;
    q->needle = needle;
    q->artist = artist;
    q->album_artist = album_artist;
    q->album = album;
    q->count = domain < 0 ? tagcache_live_count() : tagcache_group_count(domain);
    int filters = query_nonempty(artist) + query_nonempty(album_artist) + query_nonempty(album);
    q->exact = !query_nonempty(needle) && !filters;
    if (!db_open || reader_query_fd < 0) return;
    if (domain < 0) {
        if (query_nonempty(artist)) query_choose(q, TC_QUERY_ARTIST, reader_group_index(TAGCACHE_GROUP_ARTIST, artist, ""));
        if (query_nonempty(album_artist)) query_choose(q, TC_QUERY_ALBUM_ARTIST,
                                                       reader_group_index(TAGCACHE_GROUP_ALBUM_ARTIST, album_artist, ""));
        if (query_nonempty(album)) query_choose(q, TC_QUERY_ALBUM_NAME, query_album_name_group(album));
        q->exact = !query_nonempty(needle) && filters <= 1;
    }
    if (query_nonempty(needle)) {
        q->exact = strlen(needle) <= 3 &&
                   (domain >= 0 || (domain == TAGCACHE_QUERY_SONGS && filters == 0));
        uint64_t start = 0, count = 0;
        uint32_t category = domain < 0 ? TC_QUERY_SONG_GRAM : TC_QUERY_GROUP0_GRAM + (uint32_t)domain;
        query_needle(category, needle, &start, &count);
        if (!q->indexed || count < q->count) {
            q->indexed = true;
            q->start = start;
            q->count = count;
        }
    }
}

bool tagcache_query_next(tagcache_query_t *q, int32_t *rank) {
    if (!db_open || !q || !rank) return false;
    while (q->position < q->count) {
        int32_t candidate = (int32_t)q->position;
        if (q->indexed && !query_posting(q->start, q->position, &candidate)) return false;
        q->position++;
        if (q->exact) { *rank = candidate; return true; }
        if (q->domain >= 0) {
            tagcache_group_t group;
            if (!reader_group_at(q->domain, candidate, &group)) continue;
            if (query_nonempty(q->needle) && !ascii_casestr(group.name, q->needle)) continue;
        } else {
            unsigned fields = 0;
            if (query_nonempty(q->needle)) fields |= TAGCACHE_FIELD_TITLE | TAGCACHE_FIELD_ARTIST;
            if (query_nonempty(q->artist)) fields |= TAGCACHE_FIELD_ARTIST;
            if (query_nonempty(q->album_artist)) fields |= TAGCACHE_FIELD_ALBUM_ARTIST;
            if (query_nonempty(q->album)) fields |= TAGCACHE_FIELD_ALBUM;
            tagcache_song_t song;
            if (!tagcache_song_fields_at_title_rank(candidate, fields, &song)) continue;
            if (query_nonempty(q->needle) && !ascii_casestr(song.title, q->needle) &&
                (q->domain == TAGCACHE_QUERY_TITLES || !ascii_casestr(song.artist, q->needle))) continue;
            if (query_nonempty(q->artist) && !tagcache_artist_matches(song.artist, q->artist)) continue;
            if (query_nonempty(q->album_artist) && ascii_casecmp(song.album_artist, q->album_artist)) continue;
            if (query_nonempty(q->album) && ascii_casecmp(song.album, q->album)) continue;
        }
        *rank = candidate;
        return true;
    }
    return false;
}

int32_t tagcache_query_count(tagcache_query_t *q) {
    if (q->exact) return (int32_t)(q->count - q->position);
    int32_t count = 0, rank;
    while (tagcache_query_next(q, &rank)) count++;
    return count;
}

void tagcache_query_skip(tagcache_query_t *q, int32_t count) {
    if (count <= 0) return;
    if (q->exact) {
        uint64_t remaining = q->count - q->position;
        q->position += (uint64_t)count < remaining ? (uint64_t)count : remaining;
    } else {
        int32_t rank;
        while (count-- > 0 && tagcache_query_next(q, &rank)) {}
    }
}

static bool query_album_has_group(int kind, const char *name, int album_rank) {
    tagcache_group_t group;
    if (!reader_group_at(TAGCACHE_GROUP_ALBUM, album_rank, &group)) return false;
    if (kind == TAGCACHE_GROUP_ALBUM_ARTIST) return !ascii_casecmp(name, group.album_artist);
    int32_t ids[32];
    for (int offset = 0; offset < group.song_count;) {
        int n = reader_group_ids(TAGCACHE_GROUP_ALBUM, album_rank, offset, ids, 32);
        if (n <= 0) return false;
        for (int i = 0; i < n; i++) {
            tagcache_song_t song;
            if (reader_song_fields(ids[i] - 1, TAGCACHE_FIELD_ARTIST, &song) && tagcache_artist_matches(song.artist, name)) return true;
        }
        offset += n;
    }
    return false;
}

static bool query_group_albums(int kind, const char *name, uint64_t *start, uint64_t *count) {
    *start = *count = 0;
    int group = reader_group_index(kind, name, "");
    return group >= 0 && query_lookup(kind == TAGCACHE_GROUP_ARTIST ? TC_QUERY_ARTIST_ALBUMS : TC_QUERY_ALBUM_ARTIST_ALBUMS,
                                      (uint32_t)group, start, count);
}

int tagcache_group_album_count(int kind, const char *name) {
    if (!db_open || !name || kind < 0 || kind > 1) return 0;
    if (reader_query_fd >= 0) {
        uint64_t start, count;
        query_group_albums(kind, name, &start, &count);
        return (int)count;
    }
    int count = 0, albums = tagcache_group_count(TAGCACHE_GROUP_ALBUM);
    for (int i = 0; i < albums; i++) if (query_album_has_group(kind, name, i)) count++;
    return count;
}

int tagcache_group_album_at(int kind, const char *name, int offset) {
    if (!db_open || !name || kind < 0 || kind > 1 || offset < 0) return -1;
    if (reader_query_fd >= 0) {
        uint64_t start, count;
        int32_t rank;
        return query_group_albums(kind, name, &start, &count) && (uint64_t)offset < count &&
               query_posting(start, (uint64_t)offset, &rank) ? rank : -1;
    }
    int albums = tagcache_group_count(TAGCACHE_GROUP_ALBUM);
    for (int i = 0; i < albums; i++) if (query_album_has_group(kind, name, i) && offset-- == 0) return i;
    return -1;
}

static int query_display_compare(const char *left, const char *right) {
    for (size_t i = 0; i < TAGCACHE_TAG_MAX - 1; i++) {
        unsigned char a = ascii_fold((unsigned char)left[i]);
        unsigned char b = ascii_fold((unsigned char)right[i]);
        if (a != b) return (int)a - (int)b;
        if (!a) return 0;
    }
    return 0;
}

int tagcache_group_album_offset(int kind, const char *name, const char *album, const char *album_artist) {
    if (!db_open || !name || !album || !album_artist || kind < 0 || kind > 1) return -1;
    int count = tagcache_group_album_count(kind, name), lo = 0, hi = count;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        int rank = tagcache_group_album_at(kind, name, mid);
        tagcache_group_t group;
        if (rank < 0 || !reader_group_at(TAGCACHE_GROUP_ALBUM, rank, &group)) return -1;
        if (query_display_compare(group.name, album) < 0) lo = mid + 1; else hi = mid;
    }
    for (; lo < count; lo++) {
        int rank = tagcache_group_album_at(kind, name, lo);
        tagcache_group_t group;
        if (rank < 0 || !reader_group_at(TAGCACHE_GROUP_ALBUM, rank, &group) ||
            query_display_compare(group.name, album)) break;
        if (!query_display_compare(group.album_artist, album_artist)) return lo;
    }
    return -1;
}

#endif
