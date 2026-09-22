#include "queue_resume.h"
#include "library_endian.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <math.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>

#define MAX_ENTRIES 1000000
static const char magic[8] = {'H','I','B','Y','Q','0','0','1'};

void queue_resume_free(queue_resume_t * s) {
    for (int i = 0; i < s->count; i++) free(s->paths ? s->paths[i] : NULL);
    free(s->paths); free(s->order); free(s->continuation);
    memset(s, 0, sizeof(*s));
}

static bool valid(const queue_resume_t * s) {
    return s->count > 0 && s->count <= MAX_ENTRIES && s->current >= 0 && s->current < s->count &&
        s->pending >= 0 && s->pending < s->count - s->current && s->mode >= 0 && s->mode <= 3 &&
        isfinite(s->position) && s->position >= 0 &&
        (!s->order || (s->shuffle_pos >= 0 && s->shuffle_pos < s->count));
}

static bool permutation(const int * order, int count) {
    if (!order) return true;
    unsigned char * seen = calloc((size_t) count, 1);
    if (!seen) return false;
    bool ok = true;
    for (int i = 0; i < count; i++) {
        if (order[i] < 0 || order[i] >= count || seen[order[i]]) { ok = false; break; }
        seen[order[i]] = 1;
    }
    free(seen); return ok;
}

bool queue_resume_write_at(int dirfd, const char * name, const queue_resume_t * s) {
    if (!valid(s) || !s->paths || !permutation(s->order, s->count) || !permutation(s->continuation, s->count)) return false;
    if (dirfd < 0 || !name || !name[0] || strchr(name, '/')) return false;
    char temp[NAME_MAX];
    int fd = -1;
    for (unsigned int attempt = 0; attempt < 100; attempt++) {
        int n = snprintf(temp, sizeof(temp), ".%s.%ld.%u.tmp", name, (long) getpid(), attempt);
        if (n < 0 || (size_t) n >= sizeof(temp)) return false;
        fd = openat(dirfd, temp, O_WRONLY | O_CREAT | O_EXCL | O_TRUNC, 0600);
        if (fd >= 0) break;
        if (errno != EEXIST) return false;
    }
    if (fd < 0) return false;
    FILE * f = fdopen(fd, "wb");
    if (!f) { close(fd); unlinkat(dirfd, temp, 0); return false; }
    int32_t header[] = {s->count, s->current, s->pending, s->mode, s->shuffle_pos, s->order != NULL, s->continuation != NULL};
    bool ok = fwrite(magic, 1, sizeof(magic), f) == sizeof(magic) &&
        fwrite(header, sizeof(header), 1, f) == 1 && fwrite(&s->position, sizeof(double), 1, f) == 1;
    for (int i = 0; ok && i < s->count; i++) {
        size_t n = s->paths[i] ? strlen(s->paths[i]) : 0;
        uint32_t length = (uint32_t) n;
        ok = n > 0 && n < PATH_MAX && fwrite(&length, sizeof(length), 1, f) == 1 && fwrite(s->paths[i], 1, n, f) == n;
    }
    if (ok && s->order) ok = fwrite(s->order, sizeof(int), (size_t) s->count, f) == (size_t) s->count;
    if (ok && s->continuation) ok = fwrite(s->continuation, sizeof(int), (size_t) s->count, f) == (size_t) s->count;
    if (ok) ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
    if (fclose(f) != 0) ok = false;
    if (ok) ok = renameat(dirfd, temp, dirfd, name) == 0;
    if (!ok) unlinkat(dirfd, temp, 0);
    else (void) fsync(dirfd);
    return ok;
}

bool queue_resume_write(const char * path, const queue_resume_t * s) {
    if (!path || !path[0]) return false;
    char dir[PATH_MAX], name[NAME_MAX];
    const char * slash = strrchr(path, '/');
    if (slash) {
        size_t n = (size_t) (slash - path);
        if (n >= sizeof(dir)) return false;
        if (n == 0) { dir[0] = '/'; dir[1] = 0; }
        else { memcpy(dir, path, n); dir[n] = 0; }
        if (snprintf(name, sizeof(name), "%s", slash + 1) >= (int) sizeof(name)) return false;
    } else {
        snprintf(dir, sizeof(dir), ".");
        if (snprintf(name, sizeof(name), "%s", path) >= (int) sizeof(name)) return false;
    }
    int dirfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dirfd < 0) return false;
    bool ok = queue_resume_write_at(dirfd, name, s);
    close(dirfd);
    return ok;
}

bool queue_resume_read(const char * path, queue_resume_t * s) {
    memset(s, 0, sizeof(*s));
    FILE * f = fopen(path, "rb");
    if (!f) return false;
    char signature[8]; int32_t h[7];
    bool ok = fread(signature, 1, 8, f) == 8 && memcmp(signature, magic, 8) == 0 &&
        fread(h, sizeof(h), 1, f) == 1 && fread(&s->position, sizeof(double), 1, f) == 1;
    if (ok) {
        s->count = h[0]; s->current = h[1]; s->pending = h[2]; s->mode = h[3]; s->shuffle_pos = h[4];
        ok = valid(s) && (h[5] == 0 || h[5] == 1) && (h[6] == 0 || h[6] == 1);
    }
    if (!ok) { fclose(f); memset(s, 0, sizeof(*s)); return false; }
    s->paths = calloc((size_t) s->count, sizeof(char *));
    ok = s->paths != NULL;
    size_t total = 0;
    for (int i = 0; ok && i < s->count; i++) {
        uint32_t n;
        ok = fread(&n, sizeof(n), 1, f) == 1 && n > 0 && n < PATH_MAX;
        if (!ok) break;
        total += n;
        if (total > 64 * 1024 * 1024) { ok = false; break; }
        s->paths[i] = malloc((size_t) n + 1);
        ok = s->paths[i] && fread(s->paths[i], 1, n, f) == n;
        if (ok) { s->paths[i][n] = 0; ok = strlen(s->paths[i]) == n; }
    }
    for (int j = 0; ok && j < 2; j++) {
        if (!h[5 + j]) continue;
        int ** order = j ? &s->continuation : &s->order;
        *order = malloc((size_t) s->count * sizeof(int));
        ok = *order && fread(*order, sizeof(int), (size_t) s->count, f) == (size_t) s->count && permutation(*order, s->count);
    }
    if (ok) ok = valid(s) && (!s->order || s->order[s->shuffle_pos] == s->current) && fgetc(f) == EOF && !ferror(f);
    fclose(f);
    if (!ok) queue_resume_free(s);
    return ok;
}
