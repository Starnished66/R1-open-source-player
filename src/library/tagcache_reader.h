#ifndef HIBY_TAGCACHE_READER_H
#define HIBY_TAGCACHE_READER_H

/* Fixed-memory access to the active master and tag files. Included after the
 * on-disk structs and validation helpers in tagcache.c. */

static void reader_close(void) {
    if (!reader_fds_ready) {
        for (int i = 0; i < TAG_COUNT; ++i) reader_tag_fd[i] = -1;
        reader_master_fd = -1;
        reader_fds_ready = true;
        return;
    }
    if (reader_master_fd >= 0) close(reader_master_fd);
    for (int i = 0; i < TAG_COUNT; ++i) {
        if (reader_tag_fd[i] >= 0) close(reader_tag_fd[i]);
        reader_tag_fd[i] = -1;
        reader_tag_size[i] = 0;
    }
    reader_master_fd = -1;
}

static bool reader_read_at(int fd, void *buf, size_t size, off_t off) {
    unsigned char *p = (unsigned char *)buf;
    while (size != 0) {
        ssize_t n = pread(fd, p, size, off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        p += (size_t)n;
        size -= (size_t)n;
        off += (off_t)n;
    }
    return true;
}

static const int reader_persist_tags[] = {
    tag_artist, tag_album, tag_genre, tag_albumartist, tag_title, tag_filename
};

static bool reader_open(int32_t gen) {
    char path[640];
    struct stat st;
    struct master_header mh;
    reader_close();
    if (gen < 0) return false;
    { char name[80]; master_file_name(name, sizeof(name), gen); db_path(path, sizeof(path), name); }
    reader_master_fd = open(path, O_RDONLY);
    if (reader_master_fd < 0 || fstat(reader_master_fd, &st) != 0 ||
        !reader_read_at(reader_master_fd, &mh, sizeof(mh), 0) ||
        !validate_master_header(&mh, (size_t)st.st_size)) {
        reader_close();
        return false;
    }
    reader_requires_indexes = mh.tch.magic == TAGCACHE_INDEXED_MAGIC;
    ent_count = mh.tch.entry_count;
    live_count = 0;
    master_serial = mh.serial;
    master_commitid = mh.commitid;
    disk_gen = gen;
    for (size_t i = 0; i < sizeof(reader_persist_tags) / sizeof(reader_persist_tags[0]); ++i) {
        int tag = reader_persist_tags[i];
        char name[80];
        tag_file_name(name, sizeof(name), tag, gen);
        db_path(path, sizeof(path), name);
        int fd = open(path, O_RDONLY);
        struct tagcache_header th;
        if (fd < 0 || fstat(fd, &st) != 0 ||
            !reader_read_at(fd, &th, sizeof(th), 0) || !validate_tag_header(&th, (size_t)st.st_size)) {
            if (fd >= 0) close(fd);
            reader_close();
            return false;
        }
        reader_tag_fd[tag] = fd;
        reader_tag_size[tag] = (size_t)st.st_size;
    }
    return true;
}

static bool reader_index(int32_t slot, struct index_entry *out) {
    if (!reader_fds_ready || reader_master_fd < 0 || !out || slot < 0 || slot >= ent_count) return false;
    for (int attempt = 0; attempt < 8; attempt++) {
        unsigned epoch = atomic_load_explicit(&numeric_epoch, memory_order_acquire);
        if (!reader_read_at(reader_master_fd, out, sizeof(*out),
                            (off_t)sizeof(struct master_header) + (off_t)slot * sizeof(*out))) return false;
        if (READER == &committed_reader) numeric_overlay(slot, out);
        if (READER != &committed_reader ||
            epoch == atomic_load_explicit(&numeric_epoch, memory_order_acquire)) return true;
    }
    return false;
}

static bool reader_string(int tag, int32_t seek, int32_t slot, char *out, size_t out_size) {
    struct tagfile_entry te;
    size_t file_size;
    bool per_slot = tag == tag_filename || tag == tag_title;
    if (!reader_fds_ready || tag < 0 || tag >= TAG_COUNT || reader_tag_fd[tag] < 0 || !out || out_size == 0 ||
        seek < (int32_t)sizeof(struct tagcache_header)) return false;
    file_size = reader_tag_size[tag];
    if ((uintmax_t)(uint32_t)seek + sizeof(te) > file_size ||
        !reader_read_at(reader_tag_fd[tag], &te, sizeof(te), seek) || te.tag_length <= 0 ||
        (size_t)te.tag_length > out_size || (uintmax_t)(uint32_t)seek + sizeof(te) + (size_t)te.tag_length > file_size ||
        (per_slot && te.idx_id != slot) ||
        !reader_read_at(reader_tag_fd[tag], out, (size_t)te.tag_length,
                        (off_t)seek + (off_t)sizeof(te))) return false;
    return !memchr(out, '\0', (size_t)te.tag_length - 1) && out[te.tag_length - 1] == '\0';
}

static bool reader_song_fields(int32_t slot, unsigned fields, tagcache_song_t *out) {
    struct index_entry idx;
    bool incomplete = false;
    if (!out || !reader_index(slot, &idx) || (idx.flag & FLAG_DELETED)) return false;
    memset(out, 0, sizeof(*out));
    out->id = slot + 1;
    out->flags = idx.flag;
    out->mtime = idx.tag_seek[tag_mtime];
    out->size = idx.tag_seek[tag_lastoffset];
    out->first_seen = idx.tag_seek[tag_commitid];
    out->playcount = idx.tag_seek[tag_playcount];
    out->last_played = idx.tag_seek[tag_lastplayed];
    out->rating = idx.tag_seek[tag_rating];
    out->disc_number = idx.tag_seek[tag_discnumber];
    out->track_number = idx.tag_seek[tag_tracknumber];
    if (((fields & TAGCACHE_FIELD_PATH) && !reader_string(tag_filename, idx.tag_seek[tag_filename], slot, out->path_storage, sizeof(out->path_storage))) ||
        ((fields & TAGCACHE_FIELD_TITLE) && !reader_string(tag_title, idx.tag_seek[tag_title], slot, out->title_storage, sizeof(out->title_storage))) ||
        ((fields & (TAGCACHE_FIELD_ARTIST | TAGCACHE_FIELD_ALBUM_ARTIST)) && !reader_string(tag_artist, idx.tag_seek[tag_artist], -1, out->artist_storage, sizeof(out->artist_storage)))) return false;
    if ((fields & TAGCACHE_FIELD_ALBUM) && !reader_string(tag_album, idx.tag_seek[tag_album], -1, out->album_storage, sizeof(out->album_storage))) {
        out->album_storage[0] = '\0'; incomplete = true;
    }
    if ((fields & TAGCACHE_FIELD_ALBUM_ARTIST) && !reader_string(tag_albumartist, idx.tag_seek[tag_albumartist], -1, out->album_artist_storage,
                       sizeof(out->album_artist_storage))) {
        snprintf(out->album_artist_storage, sizeof(out->album_artist_storage), "%s", out->artist_storage); incomplete = true;
    }
    if ((fields & TAGCACHE_FIELD_GENRE) && !reader_string(tag_genre, idx.tag_seek[tag_genre], -1, out->genre_storage, sizeof(out->genre_storage))) {
        out->genre_storage[0] = '\0'; incomplete = true;
    }
    if (incomplete) out->flags |= FLAG_TAGS_INCOMPLETE;
    out->path = out->path_storage; out->title = out->title_storage; out->artist = out->artist_storage;
    out->album = out->album_storage; out->album_artist = out->album_artist_storage; out->genre = out->genre_storage;
    if (reader_legacy_active) {
        if ((fields & TAGCACHE_FIELD_TITLE) && !reader_compact_order) legacy_canonical_text(out->title_storage);
        if (fields & (TAGCACHE_FIELD_ARTIST | TAGCACHE_FIELD_ALBUM_ARTIST)) legacy_canonical_text(out->artist_storage);
        if (fields & TAGCACHE_FIELD_ALBUM) legacy_canonical_text(out->album_storage);
        if (fields & TAGCACHE_FIELD_ALBUM_ARTIST) legacy_canonical_text(out->album_artist_storage);
        if (fields & TAGCACHE_FIELD_GENRE) legacy_canonical_text(out->genre_storage);
    }
    return true;
}

static bool reader_song(int32_t slot, tagcache_song_t *out) {
    return reader_song_fields(slot, TAGCACHE_FIELD_ALL, out);
}

#endif
