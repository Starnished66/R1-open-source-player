#ifndef HIBY_TAGCACHE_QUERY_INDEX_H
#define HIBY_TAGCACHE_QUERY_INDEX_H

/* Immutable generation-local substring and membership postings. */
#define TC_QUERY_MAGIC 0x54435131u
#define TC_QUERY_VERSION 2u
#define TC_QUERY_VERSION_LEGACY 1u
#ifndef TC_QUERY_BLOCK_VALUES
#define TC_QUERY_BLOCK_VALUES 128u
#endif
#define TC_QUERY_BLOCK_MODE_DELTA 0u
#define TC_QUERY_BLOCK_MODE_RAW24 1u

enum tc_query_category {
    TC_QUERY_ARTIST = 1, TC_QUERY_ALBUM_ARTIST = 2, TC_QUERY_ALBUM = 3,
    TC_QUERY_ARTIST_ALBUMS = 4, TC_QUERY_ALBUM_ARTIST_ALBUMS = 5,
    TC_QUERY_SONG_GRAM = 16,
    TC_QUERY_GROUP0_GRAM = 18, TC_QUERY_GROUP1_GRAM = 19, TC_QUERY_GROUP2_GRAM = 20,
    TC_QUERY_ALBUM_NAME = 21
};
struct tc_query_tuple { uint32_t category, key, rank; };
struct tc_query_dir { uint32_t category, key; uint64_t start, count; };
struct tc_query_header {
    uint32_t magic, version, directory_count, reserved;
    uint64_t directory_offset, postings_offset, postings_count, file_size;
    int32_t generation, serial, commitid, entry_count, live_entries, group_count[3];
};
struct tc_query_writer {
    int fd;
    uint64_t bytes;
    size_t used;
    unsigned char data[32768];
};

static bool query_read(int fd, void *p, size_t n, uint64_t off) {
    if (off > INT64_MAX || (uint64_t)n > (uint64_t)INT64_MAX - off ||
        (uint64_t)(off_t)off != off) return false;
    return reader_read_at(fd, p, n, (off_t)off);
}
static bool query_writer_flush(struct tc_query_writer *w) {
    if (!w->used) return true;
    if (w->bytes > INT64_MAX || w->used > (uint64_t)INT64_MAX - w->bytes ||
        (uint64_t)(off_t)w->bytes != w->bytes ||
        !tc_sort_io_write(w->fd, w->data, w->used, (off_t)w->bytes)) return false;
    w->bytes += w->used;
    w->used = 0;
    return true;
}
static bool query_writer_add(struct tc_query_writer *w, const void *record, size_t size) {
    if (size > sizeof(w->data)) return false;
    if (w->used + size > sizeof(w->data) && !query_writer_flush(w)) return false;
    memcpy(w->data + w->used, record, size);
    w->used += size;
    return true;
}

/* Query postings are written as independently decodable blocks.  The raw
 * fallback keeps the v2 representation bounded even for adversarial deltas:
 * a block costs at most one mode byte plus three bytes per value. */
static bool query_compress_postings(int raw_fd, uint64_t count, int packed_fd, int offsets_fd,
                                    uint64_t *packed_size) {
    struct tc_query_writer packed = {.fd = packed_fd}, offsets = {.fd = offsets_fd};
    uint32_t values[TC_QUERY_BLOCK_VALUES];
    unsigned char encoded[1 + 4 + 5 * (TC_QUERY_BLOCK_VALUES - 1)];
    uint64_t blocks = count / TC_QUERY_BLOCK_VALUES + (count % TC_QUERY_BLOCK_VALUES != 0);
    uint64_t ordinal = 0;
    uint64_t raw_offset = 0;
    if (!query_writer_add(&offsets, &raw_offset, sizeof(raw_offset))) return false;
    for (uint64_t block = 0; block < blocks; block++) {
        uint64_t remaining = count - ordinal;
        size_t n = remaining > TC_QUERY_BLOCK_VALUES ? TC_QUERY_BLOCK_VALUES : (size_t)remaining;
        if (ordinal > INT64_MAX / sizeof(uint32_t) ||
            !query_read(raw_fd, values, n * sizeof(*values), ordinal * sizeof(*values))) return false;
        for (size_t i = 0; i < n; i++) if (values[i] > INT32_MAX) return false;

        size_t compressed = 0;
        encoded[compressed++] = TC_QUERY_BLOCK_MODE_DELTA;
        memcpy(encoded + compressed, &values[0], sizeof(values[0]));
        compressed += sizeof(values[0]);
        for (size_t i = 1; i < n; i++) {
            int64_t delta = (int64_t)values[i] - (int64_t)values[i - 1];
            uint64_t zigzag = delta >= 0 ? (uint64_t)delta * 2u : (uint64_t)(-delta) * 2u - 1u;
            do {
                if (compressed >= sizeof(encoded)) return false;
                unsigned char byte = (unsigned char)(zigzag & 0x7fu);
                zigzag >>= 7;
                encoded[compressed++] = (unsigned char)(byte | (zigzag ? 0x80u : 0));
            } while (zigzag);
        }
        bool raw24 = true;
        for (size_t i = 0; i < n; i++) if (values[i] > 0xffffffu) raw24 = false;
        size_t raw_size = 1 + n * 3;
        if (raw24 && compressed > raw_size) {
            encoded[0] = TC_QUERY_BLOCK_MODE_RAW24;
            compressed = 1;
            for (size_t i = 0; i < n; i++) {
                encoded[compressed++] = (unsigned char)values[i];
                encoded[compressed++] = (unsigned char)(values[i] >> 8);
                encoded[compressed++] = (unsigned char)(values[i] >> 16);
            }
        }
        if (!query_writer_add(&packed, encoded, compressed) ||
            packed.bytes > INT64_MAX || packed.used > (uint64_t)INT64_MAX - packed.bytes) return false;
        uint64_t block_offset = packed.bytes + packed.used;
        if (!query_writer_add(&offsets, &block_offset, sizeof(block_offset))) return false;
        ordinal += n;
    }
    if (!query_writer_flush(&packed) || !query_writer_flush(&offsets)) return false;
    if (packed_size) *packed_size = packed.bytes;
    return true;
}
static uint32_t query_gram_key(const unsigned char *s, size_t n) {
    uint32_t key = (uint32_t)n << 24;
    for (size_t i = 0; i < n; i++) key |= (uint32_t)ascii_fold(s[i]) << (8 * i);
    return key;
}
static int query_u32_cmp(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}
static int query_tuple_cmp(const void *a, const void *b, void *ctx) {
    (void)ctx;
    const struct tc_query_tuple *x = a, *y = b;
    if (x->category != y->category) return x->category < y->category ? -1 : 1;
    if (x->key != y->key) return x->key < y->key ? -1 : 1;
    return (x->rank > y->rank) - (x->rank < y->rank);
}
static bool query_emit(struct tc_query_writer *w, uint32_t category, uint32_t key, uint32_t rank) {
    struct tc_query_tuple tuple = {category, key, rank};
    return query_writer_add(w, &tuple, sizeof(tuple));
}
static bool query_emit_grams(struct tc_query_writer *w, uint32_t category, uint32_t rank, const char *value) {
    uint32_t grams[TAGCACHE_PATH_MAX * 3];
    size_t used = 0, length = strlen(value);
    for (size_t i = 0; i < length; i++)
        for (size_t size = 1; size <= 3 && i + size <= length; size++)
            grams[used++] = query_gram_key((const unsigned char *)value + i, size);
    qsort(grams, used, sizeof(*grams), query_u32_cmp);
    for (size_t i = 0; i < used; i++)
        if ((!i || grams[i] != grams[i - 1]) && !query_emit(w, category, grams[i], rank)) return false;
    return true;
}
static int query_build_album_key(const char *name) {
    int lo = 0, hi = reader_group_n[TAGCACHE_GROUP_ALBUM];
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        tagcache_group_t group;
        if (!reader_group_at(TAGCACHE_GROUP_ALBUM, mid, &group)) return -1;
        if (ascii_casecmp(group.name, name) < 0) lo = mid + 1; else hi = mid;
    }
    return lo;
}

static bool query_build(int output_fd) {
    struct tc_query_writer tuples = {.fd = -1}, directory = {.fd = -1}, postings = {.fd = -1};
    int scratch = -1;
    int packed_fd = -1, offsets_fd = -1;
    bool ok = false;
    tuples.fd = reader_create_temp();
    directory.fd = reader_create_temp();
    postings.fd = reader_create_temp();
    scratch = reader_create_temp();
    if (tuples.fd < 0 || directory.fd < 0 || postings.fd < 0 || scratch < 0) goto done;
    for (int32_t slot = 0; slot < ent_count; slot++) {
        struct index_entry idx;
        if (!reader_index(slot, &idx)) goto done;
        if (idx.flag & FLAG_DELETED) continue;
        tagcache_song_t song;
        if (!reader_song_fields(slot, TAGCACHE_FIELD_TITLE | TAGCACHE_FIELD_ARTIST |
                                     TAGCACHE_FIELD_ALBUM | TAGCACHE_FIELD_ALBUM_ARTIST, &song)) goto done;
        int32_t rank = reader_rank_of(slot, false);
        int aa = reader_group_index(TAGCACHE_GROUP_ALBUM_ARTIST, song.album_artist, "");
        int album = reader_group_index(TAGCACHE_GROUP_ALBUM, song.album, song.album_artist);
        int album_name = query_build_album_key(song.album);
        if (rank < 0 || aa < 0 || album < 0 || album_name < 0 ||
            !query_emit(&tuples, TC_QUERY_ALBUM_ARTIST, aa, rank) ||
            !query_emit(&tuples, TC_QUERY_ALBUM_NAME, album_name, rank) ||
            !query_emit(&tuples, TC_QUERY_ALBUM_ARTIST_ALBUMS, aa, album) ||
            !query_emit_grams(&tuples, TC_QUERY_SONG_GRAM, rank, song.title) ||
            !query_emit_grams(&tuples, TC_QUERY_SONG_GRAM, rank, song.artist)) goto done;
        char names[TAGCACHE_ARTIST_SPLIT_MAX][TAGCACHE_PATH_MAX];
        int count = split_artist(song.artist, names);
        for (int i = 0; i < count; i++) {
            int artist = reader_group_index(TAGCACHE_GROUP_ARTIST, names[i], "");
            if (artist < 0 || !query_emit(&tuples, TC_QUERY_ARTIST, artist, rank) ||
                !query_emit(&tuples, TC_QUERY_ARTIST_ALBUMS, artist, album)) goto done;
        }
    }
    for (int kind = 0; kind < 3; kind++) {
        for (int i = 0; i < reader_group_n[kind]; i++) {
            tagcache_group_t group;
            if (!reader_group_at(kind, i, &group) ||
                !query_emit_grams(&tuples, TC_QUERY_GROUP0_GRAM + kind, i, group.name)) goto done;
        }
    }
    if (!query_writer_flush(&tuples)) goto done;
    uint64_t tuple_count = tuples.bytes / sizeof(struct tc_query_tuple);
    if (tuple_count > INT64_MAX ||
        !tc_sort_fd(tuples.fd, sizeof(struct tc_query_tuple), (int64_t)tuple_count, query_tuple_cmp, NULL, scratch)) goto done;
    unsigned char input[32760];
    struct tc_sort_reader stream = {tuples.fd, sizeof(struct tc_query_tuple), input,
        sizeof(input) / sizeof(struct tc_query_tuple), 0, 0, 0, (int64_t)tuple_count};
    if (!tc_sort_reader_fill(&stream)) goto done;
    struct tc_query_tuple previous = {0};
    struct tc_query_dir entry = {0};
    bool have = false;
    uint64_t post = 0, dirs = 0;
    for (uint64_t i = 0; i < tuple_count; i++) {
        struct tc_query_tuple tuple;
        memcpy(&tuple, tc_sort_reader_record(&stream), sizeof(tuple));
        if (!have || query_tuple_cmp(&tuple, &previous, NULL)) {
            if (!have || tuple.category != previous.category || tuple.key != previous.key) {
                if (have) {
                    entry.count = post - entry.start;
                    if (!query_writer_add(&directory, &entry, sizeof(entry))) goto done;
                    dirs++;
                }
                entry.category = tuple.category;
                entry.key = tuple.key;
                entry.start = post;
            }
            if (!query_writer_add(&postings, &tuple.rank, sizeof(tuple.rank))) goto done;
            post++;
            previous = tuple;
            have = true;
        }
        if (!tc_sort_reader_advance(&stream)) goto done;
    }
    if (have) {
        entry.count = post - entry.start;
        if (!query_writer_add(&directory, &entry, sizeof(entry))) goto done;
        dirs++;
    }
    if (dirs > UINT32_MAX || !query_writer_flush(&directory) || !query_writer_flush(&postings)) goto done;
    close(tuples.fd);
    tuples.fd = -1;
    close(scratch);
    scratch = -1;
    packed_fd = reader_create_temp();
    offsets_fd = reader_create_temp();
    if (packed_fd < 0 || offsets_fd < 0) goto done;
    uint64_t packed_bytes;
    if (!query_compress_postings(postings.fd, post, packed_fd, offsets_fd, &packed_bytes)) goto done;
    struct tc_query_header header = {0};
    header.magic = TC_QUERY_MAGIC;
    header.version = TC_QUERY_VERSION;
    header.directory_count = (uint32_t)dirs;
    header.directory_offset = sizeof(header);
    header.postings_offset = sizeof(header) + directory.bytes;
    header.postings_count = post;
    struct stat offsets_stat;
    if (fstat(offsets_fd, &offsets_stat) != 0 || offsets_stat.st_size < 0 ||
        (uint64_t)offsets_stat.st_size > UINT64_MAX - header.postings_offset ||
        (uint64_t)offsets_stat.st_size > UINT64_MAX - header.postings_offset - packed_bytes) goto done;
    header.file_size = header.postings_offset + (uint64_t)offsets_stat.st_size + packed_bytes;
    header.generation = disk_gen;
    header.serial = master_serial;
    header.commitid = master_commitid;
    header.entry_count = ent_count;
    header.live_entries = live_count;
    for (int k = 0; k < 3; k++) header.group_count[k] = reader_group_n[k];
    ok = write_fully(output_fd, &header, sizeof(header)) &&
         tc_index_copy_fd(directory.fd, output_fd, NULL) && tc_index_copy_fd(offsets_fd, output_fd, NULL) &&
         tc_index_copy_fd(packed_fd, output_fd, NULL);
done:
    if (tuples.fd >= 0) close(tuples.fd);
    if (directory.fd >= 0) close(directory.fd);
    if (postings.fd >= 0) close(postings.fd);
    if (packed_fd >= 0) close(packed_fd);
    if (offsets_fd >= 0) close(offsets_fd);
    if (scratch >= 0) close(scratch);
    return ok;
}

static bool query_validate(int fd, uint64_t file_size) {
    struct tc_query_header h;
    if (fd < 0 || !query_read(fd, &h, sizeof(h), 0) || h.magic != TC_QUERY_MAGIC ||
        (h.version != TC_QUERY_VERSION_LEGACY && h.version != TC_QUERY_VERSION) ||
        h.generation != disk_gen || h.serial != master_serial || h.commitid != master_commitid ||
        h.entry_count != ent_count || h.live_entries != live_count || h.group_count[0] != reader_group_n[0] ||
        h.group_count[1] != reader_group_n[1] || h.group_count[2] != reader_group_n[2] ||
        h.directory_offset != sizeof(h) ||
        h.postings_offset != sizeof(h) + (uint64_t)h.directory_count * sizeof(struct tc_query_dir) ||
        h.file_size != file_size || h.file_size > INT64_MAX || h.postings_offset > h.file_size ||
        h.directory_count > h.postings_count) return false;
    if (h.version == TC_QUERY_VERSION_LEGACY)
        return h.postings_count == (h.file_size - h.postings_offset) / sizeof(uint32_t) &&
               (h.file_size - h.postings_offset) % sizeof(uint32_t) == 0;
    uint64_t blocks = h.postings_count / TC_QUERY_BLOCK_VALUES +
                      (h.postings_count % TC_QUERY_BLOCK_VALUES != 0);
    if (blocks == UINT64_MAX) return false;
    uint64_t table_entries = blocks + 1;
    if (table_entries > UINT64_MAX / sizeof(uint64_t)) return false;
    uint64_t table_bytes = table_entries * sizeof(uint64_t);
    if (table_bytes > h.file_size - h.postings_offset) return false;
    uint64_t payload = h.file_size - h.postings_offset - table_bytes;
    if (blocks > UINT64_MAX / 3 || h.postings_count > UINT64_MAX - 3 * blocks) return false;
    uint64_t minimum = h.postings_count + 3 * blocks;
    if (h.postings_count > (UINT64_MAX - blocks) / 3) return false;
    uint64_t maximum = 3 * h.postings_count + blocks;
    if (payload < minimum || payload > maximum) return false;
    uint64_t first, last;
    if (!query_read(fd, &first, sizeof(first), h.postings_offset) ||
        h.postings_offset > INT64_MAX - blocks * sizeof(uint64_t) ||
        !query_read(fd, &last, sizeof(last), h.postings_offset + blocks * sizeof(uint64_t))) return false;
    return first == 0 && last == payload;
}

static bool query_v2_offset(const struct tc_query_header *h, uint64_t index, uint64_t *offset) {
    uint64_t blocks = h->postings_count / TC_QUERY_BLOCK_VALUES +
                      (h->postings_count % TC_QUERY_BLOCK_VALUES != 0);
    if (index > blocks || index > INT64_MAX / sizeof(uint64_t) ||
        h->postings_offset > INT64_MAX - index * sizeof(uint64_t)) return false;
    return query_read(reader_query_fd, offset, sizeof(*offset),
                      h->postings_offset + index * sizeof(uint64_t));
}

static bool query_decode_v2_block(const struct tc_query_header *h, uint64_t block,
                                  uint32_t *values, size_t n) {
    uint64_t begin, end;
    uint64_t blocks = h->postings_count / TC_QUERY_BLOCK_VALUES +
                      (h->postings_count % TC_QUERY_BLOCK_VALUES != 0);
    unsigned char encoded[1 + 4 + 5 * (TC_QUERY_BLOCK_VALUES - 1)];
    if (block >= blocks || !query_v2_offset(h, block, &begin) || !query_v2_offset(h, block + 1, &end) ||
        end < begin || end - begin > sizeof(encoded)) return false;
    uint64_t table_entries = blocks + 1;
    if (table_entries > UINT64_MAX / sizeof(uint64_t)) return false;
    uint64_t table_bytes = table_entries * sizeof(uint64_t);
    uint64_t data_bytes = h->file_size - h->postings_offset;
    if (table_bytes > data_bytes || begin > data_bytes - table_bytes || end > data_bytes - table_bytes) return false;
    size_t length = (size_t)(end - begin);
    uint64_t data_offset = h->postings_offset + table_bytes;
    if (data_offset > INT64_MAX - begin || !query_read(reader_query_fd, encoded, length, data_offset + begin) ||
        length < 1) return false;
    unsigned char mode = encoded[0];
    if (mode == TC_QUERY_BLOCK_MODE_RAW24) {
        if (length != 1 + n * 3) return false;
        for (size_t i = 0; i < n; i++) {
            size_t at = 1 + i * 3;
            uint32_t value = (uint32_t)encoded[at] | ((uint32_t)encoded[at + 1] << 8) |
                             ((uint32_t)encoded[at + 2] << 16);
            if (value > INT32_MAX) return false;
            values[i] = value;
        }
        return true;
    }
    if (mode != TC_QUERY_BLOCK_MODE_DELTA || length < 5 || n == 0) return false;
    uint32_t first;
    memcpy(&first, encoded + 1, sizeof(first));
    if (first > INT32_MAX) return false;
    values[0] = first;
    size_t at = 5;
    for (size_t i = 1; i < n; i++) {
        uint64_t zigzag = 0;
        unsigned shift = 0;
        bool done = false;
        for (unsigned byte = 0; byte < 5; byte++) {
            if (at >= length) return false;
            unsigned char part = encoded[at++];
            if (byte == 4 && ((part & 0x80u) || (part & 0x70u))) return false;
            zigzag |= (uint64_t)(part & 0x7fu) << shift;
            if (!(part & 0x80u)) { done = true; break; }
            shift += 7;
        }
        if (!done) return false;
        int64_t delta = (zigzag & 1u) ? -(int64_t)((zigzag >> 1) + 1u) : (int64_t)(zigzag >> 1);
        int64_t value = (int64_t)values[i - 1] + delta;
        if (value < 0 || value > INT32_MAX) return false;
        values[i] = (uint32_t)value;
    }
    return at == length;
}
static bool query_lookup(uint32_t category, uint32_t key, uint64_t *start, uint64_t *count) {
    struct tc_query_header h;
    if (reader_query_fd < 0 || !query_read(reader_query_fd, &h, sizeof(h), 0)) return false;
    uint32_t lo = 0, hi = h.directory_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        struct tc_query_dir d;
        if (!query_read(reader_query_fd, &d, sizeof(d), h.directory_offset + (uint64_t)mid * sizeof(d))) return false;
        if (d.category < category || (d.category == category && d.key < key)) lo = mid + 1; else hi = mid;
    }
    if (lo >= h.directory_count) return false;
    struct tc_query_dir d;
    if (!query_read(reader_query_fd, &d, sizeof(d), h.directory_offset + (uint64_t)lo * sizeof(d)) ||
        d.category != category || d.key != key || d.start > h.postings_count ||
        d.count > h.postings_count - d.start) return false;
    *start = d.start;
    *count = d.count;
    return true;
}
static bool query_posting(uint64_t start, uint64_t offset, int32_t *rank) {
    struct tc_query_header h;
    uint32_t value;
    if (reader_query_fd < 0 || !rank || !query_read(reader_query_fd, &h, sizeof(h), 0) ||
        start > h.postings_count || offset >= h.postings_count - start) return false;
    uint64_t ordinal = start + offset;
    if (h.version == TC_QUERY_VERSION_LEGACY) {
        if (ordinal > INT64_MAX / sizeof(value) ||
            h.postings_offset > INT64_MAX - ordinal * sizeof(value) ||
            !query_read(reader_query_fd, &value, sizeof(value), h.postings_offset + ordinal * sizeof(value)) ||
            value > INT32_MAX) return false;
    } else {
        uint64_t block = ordinal / TC_QUERY_BLOCK_VALUES;
        size_t within = (size_t)(ordinal % TC_QUERY_BLOCK_VALUES);
        uint64_t count = h.postings_count - block * TC_QUERY_BLOCK_VALUES;
        if (count > TC_QUERY_BLOCK_VALUES) count = TC_QUERY_BLOCK_VALUES;
        if (!reader_query_cache_valid || reader_query_cache_block != block) {
            reader_query_cache_valid = false;
            if (!query_decode_v2_block(&h, block, reader_query_cache_values, (size_t)count)) return false;
            reader_query_cache_block = block;
            reader_query_cache_valid = true;
        }
        value = reader_query_cache_values[within];
    }
    *rank = (int32_t)value;
    return true;
}
static bool query_needle(uint32_t domain, const char *needle, uint64_t *start, uint64_t *count) {
    if (!needle || !needle[0]) return false;
    size_t length = strlen(needle), gram_length = length < 3 ? length : 3;
    uint64_t best_count = UINT64_MAX, best_start = 0;
    for (size_t i = 0; i + gram_length <= length; i++) {
        uint32_t key = query_gram_key((const unsigned char *)needle + i, gram_length);
        uint64_t current_start, current_count;
        if (!query_lookup(domain, key, &current_start, &current_count)) {
            *start = *count = 0;
            return true;
        }
        if (current_count < best_count) {
            best_count = current_count;
            best_start = current_start;
        }
    }
    *start = best_start;
    *count = best_count;
    return true;
}

#endif
