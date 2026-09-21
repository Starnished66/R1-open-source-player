#ifndef HIBY_TAGCACHE_SORT_H
#define HIBY_TAGCACHE_SORT_H

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

/* The implementation deliberately keeps every working buffer inside this
 * fixed budget.  A run is stably sorted in memory, then merge passes use the
 * two files as alternating input and output streams. */
#define TC_SORT_BUFFER_BYTES (256u * 1024u)
#define TC_SORT_RUN_BYTES (64u * 1024u)
#define TC_SORT_MERGE_BYTES (32u * 1024u)
#define TC_SORT_OUTPUT_BYTES (64u * 1024u)

static bool tc_sort_io_read(int fd, void *buf, size_t len, off_t off) {
    unsigned char *p = (unsigned char *)buf;
    while (len != 0) {
        ssize_t n = pread(fd, p, len, off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        p += (size_t)n;
        len -= (size_t)n;
        off += (off_t)n;
    }
    return true;
}

static bool tc_sort_io_write(int fd, const void *buf, size_t len, off_t off) {
    const unsigned char *p = (const unsigned char *)buf;
    while (len != 0) {
        ssize_t n = pwrite(fd, p, len, off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        p += (size_t)n;
        len -= (size_t)n;
        off += (off_t)n;
    }
    return true;
}

static bool tc_sort_mul_off(int64_t a, size_t b, off_t *out) {
    if (a < 0 || (uintmax_t)a > (uintmax_t)INT64_MAX / (uintmax_t)b) return false;
    int64_t n = a * (int64_t)b;
    if ((off_t)n != n) return false;
    *out = (off_t)n;
    return true;
}

static bool tc_sort_stable_runsort(unsigned char *a, unsigned char *tmp, size_t n, size_t rs,
                                   int (*cmp)(const void *, const void *, void *), void *ctx) {
    size_t width;
    for (width = 1; width < n;) {
        size_t start;
        for (start = 0; start < n; start += width * 2) {
            size_t mid = start + width < n ? start + width : n;
            size_t end = mid + width < n ? mid + width : n;
            size_t i = start, j = mid, k = start;
            while (i < mid && j < end) {
                const unsigned char *left = a + i * rs;
                const unsigned char *right = a + j * rs;
                /* Taking the left item on equality makes each run stable. */
                if (cmp(left, right, ctx) <= 0) {
                    memcpy(tmp + k * rs, left, rs);
                    ++i;
                } else {
                    memcpy(tmp + k * rs, right, rs);
                    ++j;
                }
                ++k;
            }
            while (i < mid) {
                memcpy(tmp + k++ * rs, a + i++ * rs, rs);
            }
            while (j < end) {
                memcpy(tmp + k++ * rs, a + j++ * rs, rs);
            }
        }
        memcpy(a, tmp, n * rs);
        if (width > n / 2) break;
        width *= 2;
    }
    return true;
}

struct tc_sort_reader {
    int fd;
    size_t rs;
    unsigned char *buf;
    size_t cap;
    size_t pos;
    size_t have;
    int64_t next;
    int64_t end;
};

static bool tc_sort_reader_fill(struct tc_sort_reader *r) {
    size_t want;
    off_t off;
    if (r->next >= r->end) {
        r->pos = r->have = 0;
        return true;
    }
    want = (size_t)(r->end - r->next);
    if (want > r->cap) want = r->cap;
    if (!tc_sort_mul_off(r->next, r->rs, &off)) return false;
    if (!tc_sort_io_read(r->fd, r->buf, want * r->rs, off)) return false;
    r->pos = 0;
    r->have = want;
    r->next += (int64_t)want;
    return true;
}

static const unsigned char *tc_sort_reader_record(struct tc_sort_reader *r) {
    return r->buf + r->pos * r->rs;
}

static bool tc_sort_reader_advance(struct tc_sort_reader *r) {
    ++r->pos;
    if (r->pos == r->have) return tc_sort_reader_fill(r);
    return true;
}

static bool tc_sort_merge_pass(int in_fd, int out_fd, int64_t count, int64_t run_len, size_t rs,
                               unsigned char *left_buf, unsigned char *right_buf, size_t merge_cap,
                               unsigned char *out_buf, size_t out_cap,
                               int (*cmp)(const void *, const void *, void *), void *ctx) {
    int64_t left_start;
    size_t out_used = 0;
    int64_t out_record = 0;
    for (left_start = 0; left_start < count; left_start += run_len * 2) {
        int64_t mid = left_start + run_len < count ? left_start + run_len : count;
        int64_t right_end = mid + run_len < count ? mid + run_len : count;
        struct tc_sort_reader left = {in_fd, rs, left_buf, merge_cap, 0, 0, left_start, mid};
        struct tc_sort_reader right = {in_fd, rs, right_buf, merge_cap, 0, 0, mid, right_end};
        int64_t total = right_end - left_start;
        int64_t emitted;
        if (!tc_sort_reader_fill(&left) || !tc_sort_reader_fill(&right)) return false;
        for (emitted = 0; emitted < total; ++emitted) {
            const unsigned char *chosen;
            bool take_left = left.next - (int64_t)left.have + (int64_t)left.pos < mid;
            bool take_right = right.next - (int64_t)right.have + (int64_t)right.pos < right_end;
            if (!take_left) chosen = tc_sort_reader_record(&right);
            else if (!take_right) chosen = tc_sort_reader_record(&left);
            else if (cmp(tc_sort_reader_record(&left), tc_sort_reader_record(&right), ctx) <= 0)
                chosen = tc_sort_reader_record(&left);
            else
                chosen = tc_sort_reader_record(&right);
            memcpy(out_buf + out_used * rs, chosen, rs);
            ++out_used;
            if (chosen == tc_sort_reader_record(&left)) {
                if (!tc_sort_reader_advance(&left)) return false;
            } else if (!tc_sort_reader_advance(&right)) {
                return false;
            }
            if (out_used == out_cap) {
                off_t off;
                if (!tc_sort_mul_off(out_record, rs, &off) ||
                    !tc_sort_io_write(out_fd, out_buf, out_used * rs, off)) return false;
                out_record += (int64_t)out_used;
                out_used = 0;
            }
        }
    }
    if (out_used != 0) {
        off_t off;
        if (!tc_sort_mul_off(out_record, rs, &off) || !tc_sort_io_write(out_fd, out_buf, out_used * rs, off)) return false;
    }
    return true;
}

static bool tc_sort_copy(int in_fd, int out_fd, int64_t count, size_t rs, unsigned char *buf, size_t cap) {
    int64_t at = 0;
    while (at < count) {
        size_t n = (size_t)(count - at);
        off_t in_off, out_off;
        if (n > cap) n = cap;
        if (!tc_sort_mul_off(at, rs, &in_off) || !tc_sort_mul_off(at, rs, &out_off) ||
            !tc_sort_io_read(in_fd, buf, n * rs, in_off) || !tc_sort_io_write(out_fd, buf, n * rs, out_off)) return false;
        at += (int64_t)n;
    }
    return true;
}

static bool tc_sort_fd(int fd, size_t record_size, int64_t count,
                       int (*cmp)(const void *, const void *, void *), void *ctx, int scratch_fd) {
    size_t run_cap, merge_cap, out_cap;
    unsigned char *run = NULL, *tmp = NULL, *left = NULL, *right = NULL, *out = NULL;
    int in_fd = fd, out_fd = scratch_fd;
    int64_t run_len;
    bool ok = false;
    off_t total_bytes;
    if (record_size == 0 || count < 0 || cmp == NULL || fd == scratch_fd ||
        !tc_sort_mul_off(count, record_size, &total_bytes)) return false;
    run_cap = TC_SORT_RUN_BYTES / record_size;
    merge_cap = TC_SORT_MERGE_BYTES / record_size;
    out_cap = TC_SORT_OUTPUT_BYTES / record_size;
    if (run_cap == 0 || merge_cap == 0 || out_cap == 0) return false;
    run = (unsigned char *)malloc(run_cap * record_size);
    tmp = (unsigned char *)malloc(run_cap * record_size);
    left = (unsigned char *)malloc(merge_cap * record_size);
    right = (unsigned char *)malloc(merge_cap * record_size);
    out = (unsigned char *)malloc(out_cap * record_size);
    if (run == NULL || tmp == NULL || left == NULL || right == NULL || out == NULL) goto done;
    for (int64_t at = 0; at < count; at += (int64_t)run_cap) {
        size_t n = (size_t)(count - at);
        off_t off;
        if (n > run_cap) n = run_cap;
        if (!tc_sort_mul_off(at, record_size, &off) || !tc_sort_io_read(fd, run, n * record_size, off) ||
            !tc_sort_stable_runsort(run, tmp, n, record_size, cmp, ctx) ||
            !tc_sort_io_write(fd, run, n * record_size, off)) goto done;
    }
    run_len = (int64_t)run_cap;
    while (run_len < count) {
        if (ftruncate(out_fd, 0) != 0 ||
            !tc_sort_merge_pass(in_fd, out_fd, count, run_len, record_size, left, right, merge_cap, out, out_cap, cmp, ctx))
            goto done;
        in_fd = out_fd;
        out_fd = (out_fd == scratch_fd) ? fd : scratch_fd;
        if (run_len > count / 2) run_len = count;
        else run_len *= 2;
    }
    if (in_fd != fd && !tc_sort_copy(in_fd, fd, count, record_size, out, out_cap)) goto done;
    if (ftruncate(fd, total_bytes) != 0) goto done;
    ok = true;
done:
    free(out);
    free(right);
    free(left);
    free(tmp);
    free(run);
    return ok;
}

#endif
