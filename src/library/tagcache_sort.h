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

/* A single arena is reused for stable run formation and k-way merge passes. */
#define TC_SORT_BUFFER_BYTES (256u * 1024u)
#define TC_SORT_RUN_BYTES (64u * 1024u)
#define TC_SORT_MERGE_BYTES (32u * 1024u)
#define TC_SORT_OUTPUT_BYTES (64u * 1024u)
#define TC_SORT_LARGE_TOTAL_BYTES (2u * 1024u * 1024u)
#define TC_SORT_LARGE_RUN_BYTES (768u * 1024u)
#define TC_SORT_LARGE_MERGE_BYTES (128u * 1024u)
#define TC_SORT_LARGE_OUTPUT_BYTES (256u * 1024u)
#ifndef TC_SORT_AVAILABLE_BYTES
#define TC_SORT_AVAILABLE_BYTES() 0u
#endif

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
    if (a < 0 || b == 0) return false;
    if (a == 0) {
        *out = 0;
        return true;
    }
    if ((uintmax_t)b > (uintmax_t)INT64_MAX ||
        (uintmax_t)a > (uintmax_t)INT64_MAX / (uintmax_t)b) return false;
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

#define TC_SORT_MAX_FAN_IN 16

static bool tc_sort_heap_before(const int *heap, int a, int b, struct tc_sort_reader *readers,
                                int (*cmp)(const void *, const void *, void *), void *ctx) {
    int c = cmp(tc_sort_reader_record(&readers[heap[a]]), tc_sort_reader_record(&readers[heap[b]]), ctx);
    return c < 0 || (c == 0 && heap[a] < heap[b]);
}

static void tc_sort_heap_sift_up(int *heap, int at, struct tc_sort_reader *readers,
                                int (*cmp)(const void *, const void *, void *), void *ctx) {
    while (at > 0) {
        int parent = (at - 1) / 2;
        int tmp;
        if (!tc_sort_heap_before(heap, at, parent, readers, cmp, ctx)) break;
        tmp = heap[at];
        heap[at] = heap[parent];
        heap[parent] = tmp;
        at = parent;
    }
}

static void tc_sort_heap_sift_down(int *heap, int count, int at, struct tc_sort_reader *readers,
                                  int (*cmp)(const void *, const void *, void *), void *ctx) {
    for (;;) {
        int left = at * 2 + 1;
        int right = left + 1;
        int best = at;
        int tmp;
        if (left < count && tc_sort_heap_before(heap, left, best, readers, cmp, ctx)) best = left;
        if (right < count && tc_sort_heap_before(heap, right, best, readers, cmp, ctx)) best = right;
        if (best == at) break;
        tmp = heap[at];
        heap[at] = heap[best];
        heap[best] = tmp;
        at = best;
    }
}

static bool tc_sort_merge_pass(int in_fd, int out_fd, int64_t count, int64_t run_len, size_t rs,
                               unsigned char *input_bufs, size_t input_cap, int fan_in,
                               unsigned char *out_buf, size_t out_cap,
                               int (*cmp)(const void *, const void *, void *), void *ctx) {
    int64_t group_start = 0;
    int64_t out_record = 0;
    size_t out_used = 0;
    while (group_start < count) {
        struct tc_sort_reader readers[TC_SORT_MAX_FAN_IN];
        int heap[TC_SORT_MAX_FAN_IN];
        int heap_count = 0;
        int i;
        for (i = 0; i < fan_in && group_start < count; i++) {
            int64_t n = count - group_start;
            if (n > run_len) n = run_len;
            readers[i] = (struct tc_sort_reader){
                in_fd, rs, input_bufs + (size_t)i * input_cap * rs, input_cap,
                0, 0, group_start, group_start + n
            };
            group_start += n;
            if (!tc_sort_reader_fill(&readers[i])) return false;
            if (readers[i].pos < readers[i].have) {
                heap[heap_count] = i;
                tc_sort_heap_sift_up(heap, heap_count, readers, cmp, ctx);
                heap_count++;
            }
        }
        while (heap_count != 0) {
            int chosen_index = heap[0];
            const unsigned char *chosen = tc_sort_reader_record(&readers[chosen_index]);
            memcpy(out_buf + out_used * rs, chosen, rs);
            out_used++;
            if (!tc_sort_reader_advance(&readers[chosen_index])) return false;
            if (readers[chosen_index].pos == readers[chosen_index].have) {
                heap_count--;
                if (heap_count != 0) heap[0] = heap[heap_count];
            }
            if (heap_count != 0)
                tc_sort_heap_sift_down(heap, heap_count, 0, readers, cmp, ctx);
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
        if (!tc_sort_mul_off(out_record, rs, &off) ||
            !tc_sort_io_write(out_fd, out_buf, out_used * rs, off)) return false;
    }
    return true;
}

static bool tc_sort_power_reaches(int base, int exponent, int64_t target) {
    int64_t value = 1;
    for (int i = 0; i < exponent; i++) {
        if (value > target / base) return true;
        value *= base;
    }
    return value >= target;
}

static bool tc_sort_fd(int fd, size_t record_size, int64_t count,
                       int (*cmp)(const void *, const void *, void *), void *ctx, int scratch_fd) {
    size_t budget, run_cap, run_bytes, out_cap, out_bytes, input_records;
    size_t preferred_records, fan_cap, input_cap;
    unsigned char *arena = NULL, *run, *tmp, *out, *input_bufs;
    int64_t run_count, run_len;
    int fan_in = 0, passes = 0;
    int run_fd, in_fd, out_fd;
    bool ok = false;
    bool large = false;
    off_t total_bytes;
    if (record_size == 0 || count < 0 || cmp == NULL || fd == scratch_fd ||
        !tc_sort_mul_off(count, record_size, &total_bytes)) return false;
    const char *disable_large = getenv("TAGCACHE_DISABLE_LARGE_SORT");
    large = (!disable_large || disable_large[0] == '\0' || disable_large[0] == '0') &&
            TC_SORT_AVAILABLE_BYTES() >= (size_t) TC_SORT_LARGE_TOTAL_BYTES + (16u * 1024u * 1024u);
    budget = large ? TC_SORT_LARGE_TOTAL_BYTES : TC_SORT_BUFFER_BYTES;
    arena = (unsigned char *)malloc(budget);
    if (arena == NULL) {
        if (!large) goto done;
        large = false;
        budget = TC_SORT_BUFFER_BYTES;
        arena = (unsigned char *)malloc(budget);
        if (arena == NULL) goto done;
    }

    /* Keep the old record-size limits even though merge buffers now share an arena. */
    if ((large ? TC_SORT_LARGE_MERGE_BYTES : TC_SORT_MERGE_BYTES) / record_size == 0) goto done;
    run_cap = (budget / 2u) / record_size;
    out_cap = (large ? TC_SORT_LARGE_OUTPUT_BYTES : TC_SORT_OUTPUT_BYTES) / record_size;
    if (run_cap == 0 || out_cap == 0) goto done;
    run_bytes = run_cap * record_size;
    out_bytes = out_cap * record_size;
    input_records = (budget - out_bytes) / record_size;
    if (input_records < 2) goto done;

    if (count == 0) {
        if (ftruncate(fd, total_bytes) != 0) goto done;
        ok = true;
        goto done;
    }
    run_count = count / (int64_t)run_cap + (count % (int64_t)run_cap != 0);
    if (run_count > 1) {
        preferred_records = 4096u / record_size + (4096u % record_size != 0);
        if (preferred_records < 4) preferred_records = 4;
        fan_cap = input_records / preferred_records;
        if (fan_cap < 2) fan_cap = input_records;
        if (fan_cap > TC_SORT_MAX_FAN_IN) fan_cap = TC_SORT_MAX_FAN_IN;
        if (run_count < (int64_t)fan_cap) fan_cap = (size_t)run_count;
        if (fan_cap < 2) goto done;
        while (!tc_sort_power_reaches((int)fan_cap, passes, run_count)) passes++;
        for (fan_in = 2; fan_in <= (int)fan_cap; fan_in++)
            if (tc_sort_power_reaches(fan_in, passes, run_count)) break;
        if (fan_in > (int)fan_cap) goto done;
        input_cap = input_records / (size_t)fan_in;
        if (input_cap == 0) goto done;
    } else {
        input_cap = 0;
    }

    run = arena;
    tmp = arena + run_bytes;
    out = arena;
    input_bufs = arena + out_bytes;
    run_fd = (passes & 1) ? scratch_fd : fd;
    if (run_fd != fd && ftruncate(run_fd, 0) != 0) goto done;
    for (int64_t at = 0; at < count;) {
        size_t n = (size_t)(count - at);
        off_t off;
        if (n > run_cap) n = run_cap;
        if (!tc_sort_mul_off(at, record_size, &off) || !tc_sort_io_read(fd, run, n * record_size, off) ||
            !tc_sort_stable_runsort(run, tmp, n, record_size, cmp, ctx) ||
            !tc_sort_io_write(run_fd, run, n * record_size, off)) goto done;
        at += (int64_t)n;
    }
    in_fd = run_fd;
    out_fd = run_fd == fd ? scratch_fd : fd;
    run_len = (int64_t)run_cap;
    for (int pass = 0; pass < passes; pass++) {
        if (ftruncate(out_fd, 0) != 0 ||
            !tc_sort_merge_pass(in_fd, out_fd, count, run_len, record_size, input_bufs, input_cap,
                                fan_in, out, out_cap, cmp, ctx))
            goto done;
        {
            int swap_fd = in_fd;
            in_fd = out_fd;
            out_fd = swap_fd;
        }
        if (run_len > count / fan_in) run_len = count;
        else run_len *= fan_in;
    }
    if (in_fd != fd) goto done;
    if (ftruncate(fd, total_bytes) != 0) goto done;
    ok = true;
done:
    free(arena);
    return ok;
}

#endif
