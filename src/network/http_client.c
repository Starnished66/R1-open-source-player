#include "http_client.h"
#include "http_conn.h"
#include "debug_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <sys/socket.h>

typedef struct {
    /* Exactly one of these is set, matching which http_get_* the caller used. */
    uint8_t ** out_buffer;
    size_t * out_buffer_size;
    FILE * out_file;
    size_t buffer_capacity;
    size_t max_buffer_size; /* 0 for file sinks; buffered API responses are bounded */

    http_progress_cb_t progress_cb;
    void * progress_user_data;

    /* Optional -- NULL/0 if the caller doesn't care. Filled in from the
     * response's Content-Type header (before the body itself is touched),
     * for callers that need to pick a decoder/file extension from the
     * server's own declared type rather than guessing from the URL (a DLNA
     * SetAVTransportURI's media URL is typically an opaque token with no
     * extension at all). */
    char * out_content_type;
    size_t out_content_type_size;
} body_sink_t;

static bool sink_write(body_sink_t * sink, const uint8_t * data, size_t len, uint64_t total_written, uint64_t total_expected) {
    if (sink->out_file) {
        /* Review finding: this branch previously had NO size bound at all,
         * unlike the buffer branch below -- a malicious or misbehaving
         * server could write an unbounded amount to this device's limited
         * storage. max_buffer_size == 0 preserves the exact original
         * unlimited behavior for http_get_to_file()/http_get_to_file_ex()
         * (DLNA/cover-art downloads, which already trust their own
         * source and whose callers were not touched by this fix); a real,
         * nonzero bound is opted into by http_get_to_file_bounded() below,
         * used by plugin.download_file_async(). */
        if (sink->max_buffer_size > 0 && total_written > sink->max_buffer_size) return false;
        if (len > 0 && fwrite(data, 1, len, sink->out_file) != len) return false;
    } else {
        size_t old_size = *sink->out_buffer_size;
        if (len > sink->max_buffer_size || old_size > sink->max_buffer_size - len) return false;
        size_t needed = old_size + len;
        if (needed > sink->buffer_capacity) {
            size_t capacity = sink->buffer_capacity ? sink->buffer_capacity : 4096;
            while (capacity < needed) {
                size_t next = capacity <= sink->max_buffer_size / 2 ? capacity * 2 : sink->max_buffer_size;
                if (next <= capacity) return false;
                capacity = next;
            }
            uint8_t * grown = realloc(*sink->out_buffer, capacity);
            if (!grown) return false;
            *sink->out_buffer = grown;
            sink->buffer_capacity = capacity;
        }
        memcpy(*sink->out_buffer + old_size, data, len);
        *sink->out_buffer_size = old_size + len;
    }
    if (sink->progress_cb && !sink->progress_cb(total_written, total_expected, sink->progress_user_data)) return false;
    return true;
}

/* Reads the response body per RFC 7230 -- either exactly content_length
 * bytes, or (if is_chunked) a chunked-encoding stream, terminated by a
 * zero-size chunk. Any trailing headers after the last chunk are read and
 * discarded (rarely used in practice, but must not be left on the wire /
 * misparsed as a new response). */
static bool read_body(http_conn_reader_t * r, bool is_chunked, uint64_t content_length, body_sink_t * sink) {
    uint8_t chunk[HTTP_CONN_READ_CHUNK];

    if (is_chunked) {
        uint64_t total = 0;
        for (;;) {
            char size_line[64];
            if (!http_conn_reader_line(r, size_line, sizeof(size_line))) return false;
            char * semi = strchr(size_line, ';');
            if (semi) *semi = '\0';
            uint64_t chunk_size = strtoull(size_line, NULL, 16);
            if (chunk_size == 0) break;

            uint64_t remaining = chunk_size;
            while (remaining > 0) {
                size_t take = remaining < sizeof(chunk) ? (size_t) remaining : sizeof(chunk);
                if (!http_conn_reader_read_exact(r, chunk, take)) return false;
                total += take;
                if (!sink_write(sink, chunk, take, total, 0)) return false;
                remaining -= take;
            }

            char crlf[4];
            if (!http_conn_reader_line(r, crlf, sizeof(crlf))) return false; /* trailing CRLF after each chunk's data */
        }
        /* Trailing headers (if any) up to the final blank line. */
        char trailer[512];
        while (http_conn_reader_line(r, trailer, sizeof(trailer)) && trailer[0] != '\0') { }
        return true;
    }

    uint64_t remaining = content_length;
    uint64_t total = 0;
    while (remaining > 0) {
        size_t take = remaining < sizeof(chunk) ? (size_t) remaining : sizeof(chunk);
        if (!http_conn_reader_read_exact(r, chunk, take)) return false;
        total += take;
        if (!sink_write(sink, chunk, take, total, content_length)) return false;
        remaining -= take;
    }
    return true;
}

/* Writes exactly len bytes of data (headers or a POST body -- either way,
 * just a byte buffer to the wire), retrying short writes -- shared by
 * do_get()/do_post() below. */
static bool write_all(http_conn_t * conn, const uint8_t * data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        int n = http_conn_write(conn, data + sent, len - sent);
        if (n <= 0) {
            DBG_LOG("http_client: conn_write failed (n=%d) after %zu/%zu bytes\n", n, sent, len);
            return false;
        }
        sent += (size_t) n;
    }
    return true;
}

static bool read_response(http_conn_t * conn, int * out_status, body_sink_t * sink); /* defined below -- shared by do_get()/do_post() */

static bool do_get_ex(const char * url, bool verify_tls, int * out_status, body_sink_t * sink,
                      uint32_t connect_timeout_ms, uint32_t read_timeout_ms, http_cancel_token_t * cancel) {
    bool is_https;
    char host[256], port[16], path[2048];
    if (!http_conn_parse_url(url, &is_https, host, sizeof(host), port, sizeof(port), path, sizeof(path))) {
        DBG_LOG("http_client: parse_url failed for '%s'\n", url);
        return false;
    }

    http_conn_t conn;
    http_conn_error_t conn_error;
    if (!http_conn_open_ex(&conn, host, port, is_https, verify_tls, connect_timeout_ms, read_timeout_ms,
                           cancel, &conn_error)) {
        DBG_LOG("http_client: conn_open failed for host='%s' port='%s' https=%d\n", host, port, is_https);
        http_conn_close(&conn);
        return false;
    }

    char request[2560];
    int req_len = snprintf(request, sizeof(request),
                            "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: open_hiby_player\r\nConnection: close\r\n\r\n",
                            path, host);
    if (req_len < 0 || (size_t) req_len >= sizeof(request)) {
        DBG_LOG("http_client: request too long for path='%s'\n", path);
        http_conn_close(&conn);
        return false;
    }

    if (!write_all(&conn, (const uint8_t *) request, (size_t) req_len)) {
        http_conn_close(&conn);
        return false;
    }

    bool ok = read_response(&conn, out_status, sink);
    http_conn_close(&conn);
    return ok;
}

static bool do_get(const char * url, bool verify_tls, int * out_status, body_sink_t * sink) {
    return do_get_ex(url, verify_tls, out_status, sink, 0, 0, NULL);
}

/* Same connection-handling shape as do_get() above, with a request body --
 * see http_post_to_buffer()'s own doc comment in http_client.h. Headers and
 * body are written as two separate write_all() calls rather than one
 * combined buffer, since a POST body's own size is unbounded (unlike the
 * fixed-size `request` header buffer GET already uses) -- concatenating
 * them into one buffer would mean either a second malloc'd copy of the
 * whole body or an arbitrary size cap neither GET nor POST currently has. */
static bool do_post(const char * url, bool verify_tls, const char * content_type, const uint8_t * body,
                     size_t body_size, int * out_status, body_sink_t * sink) {
    bool is_https;
    char host[256], port[16], path[2048];
    if (!http_conn_parse_url(url, &is_https, host, sizeof(host), port, sizeof(port), path, sizeof(path))) {
        DBG_LOG("http_client: parse_url failed for '%s'\n", url);
        return false;
    }

    http_conn_t conn;
    if (!http_conn_open(&conn, host, port, is_https, verify_tls)) {
        DBG_LOG("http_client: conn_open failed for host='%s' port='%s' https=%d\n", host, port, is_https);
        http_conn_close(&conn);
        return false;
    }

    char request[2560];
    int req_len = snprintf(request, sizeof(request),
                            "POST %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: open_hiby_player\r\n"
                            "Content-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                            path, host, content_type, body_size);
    if (req_len < 0 || (size_t) req_len >= sizeof(request)) {
        DBG_LOG("http_client: request too long for path='%s'\n", path);
        http_conn_close(&conn);
        return false;
    }

    if (!write_all(&conn, (const uint8_t *) request, (size_t) req_len) ||
        (body_size > 0 && !write_all(&conn, body, body_size))) {
        http_conn_close(&conn);
        return false;
    }

    bool ok = read_response(&conn, out_status, sink);
    http_conn_close(&conn);
    return ok;
}

/* Reads a response (status line, headers, body) off an already-open
 * connection whose request has already been fully written -- shared tail
 * end of do_get()/do_post() below, since a GET and a POST response are read
 * identically once the request itself is on the wire. */
static bool read_response(http_conn_t * conn, int * out_status, body_sink_t * sink) {
    http_conn_reader_t reader;
    memset(&reader, 0, sizeof(reader));
    reader.conn = conn;

    char status_line[256];
    if (!http_conn_reader_line(&reader, status_line, sizeof(status_line))) {
        DBG_LOG("http_client: failed to read status line\n");
        return false;
    }
    DBG_LOG("http_client: status line: '%s'\n", status_line);

    int status = 0;
    /* "HTTP/1.1 200 OK" -- skip the version token, read the 3-digit code. */
    const char * sp = strchr(status_line, ' ');
    if (!sp) {
        DBG_LOG("http_client: malformed status line '%s'\n", status_line);
        return false;
    }
    status = atoi(sp + 1);

    bool is_chunked = false;
    uint64_t content_length = 0;
    char header_line[1024];
    while (http_conn_reader_line(&reader, header_line, sizeof(header_line)) && header_line[0] != '\0') {
        char * colon = strchr(header_line, ':');
        if (!colon) continue;
        *colon = '\0';
        const char * value = colon + 1;
        while (*value == ' ') value++;

        if (strcasecmp(header_line, "Content-Length") == 0) {
            content_length = strtoull(value, NULL, 10);
        } else if (strcasecmp(header_line, "Transfer-Encoding") == 0 && strcasecmp(value, "chunked") == 0) {
            is_chunked = true;
        } else if (strcasecmp(header_line, "Content-Type") == 0 && sink->out_content_type && sink->out_content_type_size > 0) {
            /* Strip a trailing "; charset=..." parameter, if any -- callers
             * only need the bare MIME type to pick a decoder/extension. */
            char * semi = strchr(value, ';');
            if (semi) *semi = '\0';
            snprintf(sink->out_content_type, sink->out_content_type_size, "%s", value);
        }
    }
    DBG_LOG("http_client: status=%d content_length=%llu chunked=%d\n", status,
            (unsigned long long) content_length, is_chunked);

    if (sink->out_buffer && !is_chunked && content_length > sink->max_buffer_size) {
        DBG_LOG("http_client: refusing oversized buffered response (%llu > %zu)\n",
                (unsigned long long) content_length, sink->max_buffer_size);
        return false;
    }

    if (!read_body(&reader, is_chunked, content_length, sink)) {
        DBG_LOG("http_client: read_body failed (chunked=%d content_length=%llu)\n", is_chunked,
                (unsigned long long) content_length);
        return false;
    }

    *out_status = status;
    return true;
}

bool http_get_to_buffer_limited(const char * url, bool verify_tls, size_t max_body_size, int * out_status,
                                uint8_t ** out_body, size_t * out_body_size) {
    *out_body = NULL;
    *out_body_size = 0;

    body_sink_t sink = { .out_buffer = out_body, .out_buffer_size = out_body_size, .out_file = NULL,
                          .buffer_capacity = 0, .max_buffer_size = max_body_size,
                          .progress_cb = NULL, .progress_user_data = NULL,
                          .out_content_type = NULL, .out_content_type_size = 0 };
    if (!do_get(url, verify_tls, out_status, &sink)) {
        free(*out_body);
        *out_body = NULL;
        *out_body_size = 0;
        return false;
    }
    return true;
}

bool http_get_to_buffer(const char * url, bool verify_tls, int * out_status, uint8_t ** out_body,
                         size_t * out_body_size) {
    return http_get_to_buffer_limited(url, verify_tls, 8 * 1024 * 1024, out_status, out_body, out_body_size);
}

bool http_post_to_buffer_limited(const char * url, bool verify_tls, const char * content_type, const uint8_t * body,
                                  size_t body_size, size_t max_body_size, int * out_status, uint8_t ** out_body,
                                  size_t * out_body_size) {
    *out_body = NULL;
    *out_body_size = 0;

    body_sink_t sink = { .out_buffer = out_body, .out_buffer_size = out_body_size, .out_file = NULL,
                          .buffer_capacity = 0, .max_buffer_size = max_body_size,
                          .progress_cb = NULL, .progress_user_data = NULL,
                          .out_content_type = NULL, .out_content_type_size = 0 };
    if (!do_post(url, verify_tls, content_type, body, body_size, out_status, &sink)) {
        free(*out_body);
        *out_body = NULL;
        *out_body_size = 0;
        return false;
    }
    return true;
}

bool http_post_to_buffer(const char * url, bool verify_tls, const char * content_type, const uint8_t * body,
                          size_t body_size, int * out_status, uint8_t ** out_body, size_t * out_body_size) {
    return http_post_to_buffer_limited(url, verify_tls, content_type, body, body_size, 8 * 1024 * 1024, out_status,
                                        out_body, out_body_size);
}

bool http_get_to_file(const char * url, bool verify_tls, const char * dest_path,
                       http_progress_cb_t progress_cb, void * progress_user_data) {
    return http_get_to_file_ex(url, verify_tls, dest_path, progress_cb, progress_user_data, NULL, 0);
}

bool http_get_to_file_ex(const char * url, bool verify_tls, const char * dest_path,
                          http_progress_cb_t progress_cb, void * progress_user_data,
                          char * out_content_type, size_t out_content_type_size) {
    if (out_content_type && out_content_type_size > 0) out_content_type[0] = '\0';

    FILE * f = fopen(dest_path, "wb");
    if (!f) return false;

    int status = 0;
    body_sink_t sink = { .out_buffer = NULL, .out_buffer_size = NULL, .out_file = f,
                          .buffer_capacity = 0, .max_buffer_size = 0,
                          .progress_cb = progress_cb, .progress_user_data = progress_user_data,
                          .out_content_type = out_content_type, .out_content_type_size = out_content_type_size };
    bool ok = do_get(url, verify_tls, &status, &sink);
    fclose(f);

    if (!ok || status < 200 || status >= 300) {
        remove(dest_path);
        return false;
    }
    return true;
}

#define HTTP_FILE_DOWNLOAD_DEFAULT_MAX_BYTES (2ULL * 1024 * 1024 * 1024)

bool http_get_to_file_bounded(const char * url, bool verify_tls, const char * dest_path, size_t max_body_size,
                               http_progress_cb_t progress_cb, void * progress_user_data) {
    FILE * f = fopen(dest_path, "wb");
    if (!f) return false;

    int status = 0;
    body_sink_t sink = { .out_buffer = NULL, .out_buffer_size = NULL, .out_file = f,
                          .buffer_capacity = 0,
                          .max_buffer_size = max_body_size > 0 ? max_body_size : HTTP_FILE_DOWNLOAD_DEFAULT_MAX_BYTES,
                          .progress_cb = progress_cb, .progress_user_data = progress_user_data,
                          .out_content_type = NULL, .out_content_type_size = 0 };
    bool ok = do_get(url, verify_tls, &status, &sink);
    fclose(f);

    if (!ok || status < 200 || status >= 300) {
        remove(dest_path);
        return false;
    }
    return true;
}

bool http_get_to_file_cancelable(const char * url, bool verify_tls, const char * dest_path,
                                  http_progress_cb_t progress_cb, void * progress_user_data,
                                  uint32_t connect_timeout_ms, uint32_t read_timeout_ms,
                                  http_cancel_token_t * cancel) {
    FILE * f = fopen(dest_path, "wb");
    if (!f) return false;

    int status = 0;
    body_sink_t sink = { .out_buffer = NULL, .out_buffer_size = NULL, .out_file = f,
                          .buffer_capacity = 0, .max_buffer_size = HTTP_FILE_DOWNLOAD_DEFAULT_MAX_BYTES,
                          .progress_cb = progress_cb, .progress_user_data = progress_user_data,
                          .out_content_type = NULL, .out_content_type_size = 0 };
    bool ok = do_get_ex(url, verify_tls, &status, &sink, connect_timeout_ms, read_timeout_ms, cancel);
    fclose(f);

    if (!ok || status < 200 || status >= 300) {
        remove(dest_path);
        return false;
    }
    return true;
}

/* ---- Extended request API (http_client.h's own comment explains why
 * this is new, separate code rather than a rework of do_get()/do_post()
 * above) ---- */

#define HTTP_REQUEST_DEFAULT_MAX_RESPONSE (2U * 1024U * 1024U)

/* Deliberately a separate copy of http_conn.c's own monotonic_now_ms()
 * rather than a new cross-file dependency for one 4-line helper. */
static uint64_t monotonic_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000ULL + (uint64_t) ts.tv_nsec / 1000000ULL;
}

void http_cancel_token_init(http_cancel_token_t * tok) {
    pthread_mutex_init(&tok->mutex, NULL);
    tok->fd = -1;
    tok->cancel_requested = false;
}

void http_cancel_token_destroy(http_cancel_token_t * tok) {
    pthread_mutex_destroy(&tok->mutex);
}

void http_cancel_token_cancel(http_cancel_token_t * tok) {
    if (!tok) return;
    /* Review finding: shutdown() previously ran AFTER releasing the
     * mutex, using an fd value already read while still holding it --
     * between that unlock and the shutdown() call below, http_conn.c's
     * own unregister-then-close (running under the same mutex, see its
     * own comment) could complete entirely: clear tok->fd AND actually
     * close the real fd, freeing that number for reuse by a completely
     * unrelated socket elsewhere in this app, which the shutdown() call
     * here would then hit instead. Reading fd and calling shutdown() on
     * it must be one atomic operation with respect to unregister-and-
     * close, not two separate steps split across an unlock -- both sides
     * now hold this same mutex for their ENTIRE critical operation
     * (including the actual shutdown()/close() syscall, not just the
     * bookkeeping around it), so they can never interleave: either this
     * shutdown() runs while the fd is still genuinely valid, or the
     * close() already ran and tok->fd already reads -1 by the time this
     * function gets the lock, correctly skipping shutdown() entirely. */
    pthread_mutex_lock(&tok->mutex);
    tok->cancel_requested = true;
    /* Same technique http_stream_close() already uses: force whatever
     * blocking recv()/connect() is in progress to return immediately
     * rather than waiting on the network. Safe to call even if fd is
     * still -1 (nothing connected yet -- the cancel_requested flag alone
     * makes the connect attempt itself bail out, see http_conn_open_ex()). */
    if (tok->fd >= 0) shutdown(tok->fd, SHUT_RDWR);
    pthread_mutex_unlock(&tok->mutex);
}

bool http_cancel_token_is_cancelled(http_cancel_token_t * tok) {
    if (!tok) return false;
    pthread_mutex_lock(&tok->mutex);
    bool cancelled = tok->cancel_requested;
    pthread_mutex_unlock(&tok->mutex);
    return cancelled;
}

const char * http_headers_get(const http_header_t * headers, int count, const char * name) {
    for (int i = 0; i < count; i++) {
        if (strcasecmp(headers[i].name, name) == 0) return headers[i].value;
    }
    return NULL;
}

/* Review finding: plugin-supplied header names/values (and the URL's own
 * path component) were copied straight into the request block with no
 * validation at all -- a value containing "\r\n" could inject an
 * arbitrary extra header or split the request into two. Rejected up
 * front (HTTP_ERR_INVALID_REQUEST), before any connection is even
 * attempted, rather than silently sanitized/truncated -- a plugin should
 * see a clear error for malformed input, not have it silently altered.
 * Name grammar loosely follows RFC 7230's "token" (reject CTLs, space,
 * DEL, and ':' specifically since that's the name/value separator);
 * value only rejects CR/LF/NUL, the actual injection vectors, not every
 * RFC-pedantic separator, since real header values legitimately contain
 * spaces, commas, etc. */
static bool http_header_name_is_valid(const char * name) {
    if (!name || !name[0]) return false;
    for (const unsigned char * p = (const unsigned char *) name; *p; p++) {
        if (*p < 0x21 || *p == 0x7F || *p == ':') return false;
    }
    return true;
}

static bool http_header_value_is_valid(const char * value) {
    if (!value) return false;
    for (const char * p = value; *p; p++) {
        if (*p == '\r' || *p == '\n' || *p == '\0') return false;
    }
    return true;
}

/* Same CR/LF check for the URL itself -- http_conn_parse_url() copies its
 * path component verbatim into the request line with no filtering, so a
 * URL containing a raw (not percent-encoded) CR/LF could inject headers
 * the exact same way a malicious header value could. Only ever needs
 * checking against plugin-supplied input (the top-level http_request_ex()
 * entry, once) -- a redirect hop's URL comes from a server's Location
 * header, which http_conn_reader_line() already cannot return containing
 * an embedded '\n' by construction (it's the line terminator), so
 * re-validating it on every hop would be redundant. */
static bool http_url_is_valid(const char * url) {
    if (!url || !url[0]) return false;
    for (const char * p = url; *p; p++) {
        if (*p == '\r' || *p == '\n') return false;
    }
    return true;
}

/* Origin comparison for redirect handling below -- scheme + host + port,
 * matching the browser/curl notion of "same origin" closely enough for
 * deciding whether it's safe to carry Authorization/Cookie across a
 * redirect hop. Host compared case-insensitively (DNS names aren't
 * case-sensitive); port as an exact string match (both already normalized
 * to a concrete port number by http_conn_parse_url(), never empty). */
static bool http_same_origin(bool https_a, const char * host_a, const char * port_a,
                              bool https_b, const char * host_b, const char * port_b) {
    return https_a == https_b && strcasecmp(host_a, host_b) == 0 && strcmp(port_a, port_b) == 0;
}

static bool http_header_name_is_sensitive(const char * name) {
    return strcasecmp(name, "Authorization") == 0 || strcasecmp(name, "Cookie") == 0 ||
           strcasecmp(name, "Cookie2") == 0 || strcasecmp(name, "Proxy-Authorization") == 0;
}

/* Review finding: redirect handling reused the complete original request
 * -- including Authorization/Cookie -- for ANY absolute http(s) redirect
 * target, so a provider redirecting to a different host would receive
 * the original bearer token. Strips credential-bearing headers from
 * `headers`/`*header_count` in place whenever the two origins differ;
 * same-origin redirects (the overwhelmingly common case -- a server
 * redirecting within its own domain) are unaffected. */
static void http_strip_sensitive_headers(http_header_t * headers, int * header_count) {
    int out = 0;
    for (int i = 0; i < *header_count; i++) {
        if (http_header_name_is_sensitive(headers[i].name)) continue;
        if (out != i) headers[out] = headers[i];
        out++;
    }
    *header_count = out;
}

/* Review finding: standard redirect method/body semantics weren't applied
 * at all -- every hop blindly resent the original method and body. 303
 * always becomes GET with no body; 301/302 conventionally rewrite POST to
 * GET with no body (matching curl's own default and every major browser,
 * even though RFC 7231 technically permits preserving the method); 307/308
 * and any non-POST method on 301/302 preserve both exactly. */
static void http_apply_redirect_method_semantics(http_request_t * req, int status) {
    if (status == 303 || ((status == 301 || status == 302) && req->method == HTTP_METHOD_POST)) {
        req->method = HTTP_METHOD_GET;
        req->body = NULL;
        req->body_len = 0;
    }
}

/* Clamps a per-stage timeout to whatever's left of the request's overall
 * total_timeout_ms budget -- review finding: total_timeout_ms was only
 * ever checked between hops/body chunks, so it bounded neither connect,
 * TLS setup, the request upload, nor a response-header read stuck waiting
 * on read_timeout_ms's own (possibly much larger, or unset) budget.
 * absolute_deadline_ms == 0 means no total-timeout was requested at all,
 * in which case the per-stage value passes through completely unchanged.
 * A deadline already in the past returns 1 (not 0, which would mean "no
 * timeout" to http_conn_open_ex) so the very next operation fails almost
 * immediately instead of blocking. This still isn't a single hermetic
 * deadline enforced on every syscall (SO_RCVTIMEO is a per-call timeout,
 * re-armed fresh for each read, so many small reads could in principle
 * still add up past the budget) -- but connect/handshake/every read now
 * genuinely cannot individually exceed the remaining budget, which is
 * the substantive part of the gap. */
static uint32_t http_effective_timeout_ms(uint32_t requested_ms, uint64_t absolute_deadline_ms) {
    if (absolute_deadline_ms == 0) return requested_ms;
    uint64_t now = monotonic_now_ms();
    if (now >= absolute_deadline_ms) return 1;
    uint64_t remaining = absolute_deadline_ms - now;
    if (requested_ms == 0 || (uint64_t) requested_ms > remaining) {
        return remaining > 0xFFFFFFFFULL ? 0xFFFFFFFFU : (uint32_t) remaining;
    }
    return requested_ms;
}

/* Review finding: http_conn_open_ex()'s old bool-plus-out_timed_out result
 * couldn't distinguish DNS failure, TLS setup failure, certificate
 * verification failure, or a non-timeout handshake failure from a plain
 * connect failure -- nearly everything collapsed into HTTP_ERR_CONNECT.
 * Translates the real http_conn_error_t (http_conn.h) into the stable
 * HTTP_ERR_* string a plugin actually sees. */
static const char * http_conn_error_to_str(http_conn_error_t e) {
    switch (e) {
        case HTTP_CONN_OK: return HTTP_ERR_NONE;
        case HTTP_CONN_ERR_CANCELLED: return HTTP_ERR_CANCELLED;
        case HTTP_CONN_ERR_DNS: return HTTP_ERR_DNS;
        case HTTP_CONN_ERR_SOCKET: return HTTP_ERR_CONNECT;
        case HTTP_CONN_ERR_CONNECT: return HTTP_ERR_CONNECT;
        case HTTP_CONN_ERR_CONNECT_TIMEOUT: return HTTP_ERR_CONNECT_TIMEOUT;
        case HTTP_CONN_ERR_TLS_SETUP: return HTTP_ERR_TLS;
        case HTTP_CONN_ERR_TLS_HANDSHAKE: return HTTP_ERR_TLS;
        case HTTP_CONN_ERR_TLS_HANDSHAKE_TIMEOUT: return HTTP_ERR_TIMEOUT;
        case HTTP_CONN_ERR_TLS_VERIFY: return HTTP_ERR_TLS;
    }
    return HTTP_ERR_CONNECT;
}

static const char * http_method_to_string(http_method_t m) {
    switch (m) {
        case HTTP_METHOD_GET: return "GET";
        case HTTP_METHOD_POST: return "POST";
        case HTTP_METHOD_PUT: return "PUT";
        case HTTP_METHOD_PATCH: return "PATCH";
        case HTTP_METHOD_DELETE: return "DELETE";
        case HTTP_METHOD_HEAD: return "HEAD";
    }
    return "GET";
}

/* Total elapsed-time cap across body reads, implemented via the SAME
 * per-chunk progress-callback hook sink_write() already calls (and
 * already lets a caller abort by returning false) -- no change needed to
 * read_body()/sink_write() themselves. Checked once per chunk, so this
 * can only fire between chunks, not preempt mid-chunk -- matches
 * http_client.h's own "checked between body chunks, not a hard
 * preemption" documentation for total_timeout_ms exactly. */
typedef struct {
    uint64_t deadline_ms; /* 0 = no deadline */
} total_timeout_ctx_t;

static bool total_timeout_progress_cb(uint64_t downloaded, uint64_t total, void * user_data) {
    (void) downloaded;
    (void) total;
    const total_timeout_ctx_t * ctx = (const total_timeout_ctx_t *) user_data;
    return ctx->deadline_ms == 0 || monotonic_now_ms() < ctx->deadline_ms;
}

/* Builds and sends the request line + headers (+ body, if any) on an
 * already-open connection. Caller-supplied headers (req->headers[]) take
 * precedence for Host/User-Agent/Content-Type/Content-Length/Connection
 * over this function's own defaults for each -- checked case-
 * insensitively via http_headers_get() -- so a plugin that wants a custom
 * User-Agent isn't silently overridden. */
static bool send_request_ex(http_conn_t * conn, const http_request_t * req, const char * host, const char * path) {
    /* Defense in depth -- http_request_ex() already validates every
     * plugin-supplied header once at the top level (see its own comment),
     * so this should never actually trigger; kept here anyway as the
     * authoritative point where headers actually reach the wire, in case
     * some future caller reaches this function a different way. */
    for (int i = 0; i < req->header_count; i++) {
        if (!http_header_name_is_valid(req->headers[i].name) || !http_header_value_is_valid(req->headers[i].value)) {
            return false;
        }
    }

    char header_block[8192];
    size_t pos = 0;
    int n = snprintf(header_block, sizeof(header_block), "%s %s HTTP/1.1\r\n", http_method_to_string(req->method), path);
    if (n <= 0 || (size_t) n >= sizeof(header_block)) return false;
    pos = (size_t) n;

    bool has_host = http_headers_get(req->headers, req->header_count, "Host") != NULL;
    bool has_ua = http_headers_get(req->headers, req->header_count, "User-Agent") != NULL;
    bool has_ct = http_headers_get(req->headers, req->header_count, "Content-Type") != NULL;
    bool has_cl = http_headers_get(req->headers, req->header_count, "Content-Length") != NULL;
    bool has_conn = http_headers_get(req->headers, req->header_count, "Connection") != NULL;

#define APPEND_HDR(...) do { \
        n = snprintf(header_block + pos, sizeof(header_block) - pos, __VA_ARGS__); \
        if (n <= 0 || (size_t) n >= sizeof(header_block) - pos) return false; \
        pos += (size_t) n; \
    } while (0)

    if (!has_host) APPEND_HDR("Host: %s\r\n", host);
    if (!has_ua) APPEND_HDR("User-Agent: open_hiby_player\r\n");
    if (!has_conn) APPEND_HDR("Connection: close\r\n");
    if (!has_ct && req->content_type && req->content_type[0]) APPEND_HDR("Content-Type: %s\r\n", req->content_type);
    if (!has_cl && req->body && req->body_len > 0) APPEND_HDR("Content-Length: %zu\r\n", req->body_len);

    for (int i = 0; i < req->header_count; i++) {
        APPEND_HDR("%s: %s\r\n", req->headers[i].name, req->headers[i].value);
    }
    APPEND_HDR("\r\n");
#undef APPEND_HDR

    if (!write_all(conn, (const uint8_t *) header_block, pos)) return false;
    if (req->body && req->body_len > 0 && !write_all(conn, req->body, req->body_len)) return false;
    return true;
}

/* Like read_response() (shared by do_get()/do_post() above), but captures
 * every response header (bounded by HTTP_MAX_HEADERS) into resp instead
 * of only the 3 read_response() cares about, and separately reports a
 * 3xx's Location header for the redirect-following loop below. Reuses
 * read_body()/sink_write() completely unchanged for the body itself --
 * capturing more headers doesn't change body framing at all. */
static bool read_response_ex(http_conn_t * conn, http_response_t * resp, body_sink_t * sink,
                              char * out_location, size_t out_location_size, bool head_only) {
    http_conn_reader_t reader;
    memset(&reader, 0, sizeof(reader));
    reader.conn = conn;

    char status_line[256];
    if (!http_conn_reader_line(&reader, status_line, sizeof(status_line))) {
        /* Review finding: every read failure reported the same generic
         * error, indistinguishable from a genuine SO_RCVTIMEO read
         * timeout. conn->last_read_timed_out (http_conn.c) is set on
         * every http_conn_read() call, success or failure -- check it
         * immediately after any read-based failure, here and below. */
        resp->error = conn->last_read_timed_out ? HTTP_ERR_TIMEOUT : HTTP_ERR_MALFORMED;
        return false;
    }

    const char * sp = strchr(status_line, ' ');
    if (!sp) {
        resp->error = HTTP_ERR_MALFORMED;
        return false;
    }
    resp->status = atoi(sp + 1);

    bool is_chunked = false;
    uint64_t content_length = 0;
    if (out_location && out_location_size > 0) out_location[0] = '\0';

    /* Review-adjacent finding, caught while fixing the above: the
     * original "while (read_line() && line[0] != '\0')" loop condition
     * could not distinguish "read a genuine blank line (normal end of
     * headers)" from "the read itself failed" -- a connection dropping
     * mid-header-read was silently swallowed, falling through to try
     * reading a body anyway with whatever partial header state existed
     * so far, rather than being reported as a failure at all. Tracks
     * which case ended the loop explicitly. */
    char header_line[1024];
    bool headers_ok = true;
    for (;;) {
        if (!http_conn_reader_line(&reader, header_line, sizeof(header_line))) { headers_ok = false; break; }
        if (header_line[0] == '\0') break; /* genuine blank line -- normal end of headers */

        char * colon = strchr(header_line, ':');
        if (!colon) continue;
        *colon = '\0';
        const char * value = colon + 1;
        while (*value == ' ') value++;

        if (strcasecmp(header_line, "Content-Length") == 0) {
            content_length = strtoull(value, NULL, 10);
        } else if (strcasecmp(header_line, "Transfer-Encoding") == 0 && strcasecmp(value, "chunked") == 0) {
            is_chunked = true;
        } else if (strcasecmp(header_line, "Location") == 0 && out_location && out_location_size > 0) {
            snprintf(out_location, out_location_size, "%s", value);
        }

        /* Review finding: this used to snprintf() unconditionally,
         * silently truncating a name/value too long for the fixed
         * buffers (also what the compiler's own -Wformat-truncation
         * warning was flagging). A truncated response header can look
         * like a complete, valid value to a plugin when it isn't (a
         * cut-off token or signed URL, for instance) -- DROP the header
         * entirely instead if it doesn't fit, rather than hand back
         * something silently corrupted. Real header names are
         * practically always well under HTTP_HEADER_NAME_MAX; a value
         * this long is rarer but real (a long Set-Cookie, for instance),
         * and simply won't appear in the table rather than appearing
         * wrong. */
        if (resp->header_count < HTTP_MAX_HEADERS) {
            size_t name_len = strlen(header_line);
            size_t value_len = strlen(value);
            if (name_len < sizeof(resp->headers[0].name) && value_len < sizeof(resp->headers[0].value)) {
                memcpy(resp->headers[resp->header_count].name, header_line, name_len + 1);
                memcpy(resp->headers[resp->header_count].value, value, value_len + 1);
                resp->header_count++;
            }
        }
    }
    if (!headers_ok) {
        resp->error = conn->last_read_timed_out ? HTTP_ERR_TIMEOUT : HTTP_ERR_MALFORMED;
        return false;
    }

    if (head_only) return true; /* no body to read for HEAD, regardless of what headers claim */

    if (!is_chunked && content_length > sink->max_buffer_size) {
        resp->error = HTTP_ERR_RESPONSE_TOO_LARGE;
        return false;
    }

    if (!read_body(&reader, is_chunked, content_length, sink)) {
        /* Review finding: unlike the status-line and header-loop failures
         * just above in this same function, this branch never checked
         * conn->last_read_timed_out -- a read_timeout_ms-triggered
         * SO_RCVTIMEO firing mid-body-read (headers arrived fine, the body
         * then stalled) was still unconditionally reported as io_error,
         * exactly the bug already fixed for the status-line/header cases.
         * Both real values are still overwritten by the caller when they
         * apply (HTTP_ERR_CANCELLED for a cancel token, HTTP_ERR_TIMEOUT
         * for a total_timeout_ms deadline that fired via the progress
         * callback -- see http_request_ex_internal()'s own comment on why
         * that second case needs a separate check: last_read_timed_out is
         * false there too, since the underlying read that triggered the
         * callback already succeeded). */
        resp->error = conn->last_read_timed_out ? HTTP_ERR_TIMEOUT : HTTP_ERR_IO;
        return false;
    }
    return true;
}

static bool cancel_requested(http_cancel_token_t * cancel) {
    if (!cancel) return false;
    pthread_mutex_lock(&cancel->mutex);
    bool requested = cancel->cancel_requested;
    pthread_mutex_unlock(&cancel->mutex);
    return requested;
}

/* NOTE: req is intentionally non-const here (unlike the public
 * http_request_ex() entry point) -- redirect handling below needs to
 * mutate method/body/headers across hops (stripped credentials, rewritten
 * method per standard redirect semantics). http_request_ex() itself makes
 * a local, mutable copy of the caller's request precisely so the caller's
 * own struct is never touched. */
static bool http_request_ex_internal(http_request_t * req, http_cancel_token_t * cancel, http_response_t * resp,
                                      const char * url, int redirects_left, uint64_t absolute_deadline_ms) {
    if (cancel_requested(cancel)) {
        resp->error = HTTP_ERR_CANCELLED;
        return false;
    }
    if (absolute_deadline_ms != 0 && monotonic_now_ms() >= absolute_deadline_ms) {
        resp->error = HTTP_ERR_TIMEOUT;
        return false;
    }

    bool is_https;
    char host[256], port[16], path[2048];
    if (!http_conn_parse_url(url, &is_https, host, sizeof(host), port, sizeof(port), path, sizeof(path))) {
        DBG_LOG("http_client: http_request_ex parse_url failed for '%s'\n", url);
        resp->error = HTTP_ERR_INVALID_URL;
        return false;
    }

    http_conn_t conn;
    http_conn_error_t conn_error = HTTP_CONN_OK;
    uint32_t eff_connect_timeout = http_effective_timeout_ms(req->connect_timeout_ms, absolute_deadline_ms);
    uint32_t eff_read_timeout = http_effective_timeout_ms(req->read_timeout_ms, absolute_deadline_ms);
    if (!http_conn_open_ex(&conn, host, port, is_https, req->verify_tls,
                            eff_connect_timeout, eff_read_timeout, cancel, &conn_error)) {
        DBG_LOG("http_client: http_request_ex conn_open failed for host='%s' port='%s' https=%d (conn_error=%d)\n",
                host, port, is_https, (int) conn_error);
        http_conn_close(&conn);
        resp->error = http_conn_error_to_str(conn_error);
        return false;
    }

    if (!send_request_ex(&conn, req, host, path)) {
        http_conn_close(&conn);
        resp->error = cancel_requested(cancel) ? HTTP_ERR_CANCELLED : HTTP_ERR_IO;
        return false;
    }

    size_t max_bytes = req->max_response_bytes > 0 ? req->max_response_bytes : HTTP_REQUEST_DEFAULT_MAX_RESPONSE;
    uint8_t * body = NULL;
    size_t body_size = 0;
    total_timeout_ctx_t timeout_ctx = { .deadline_ms = absolute_deadline_ms };
    body_sink_t sink = { .out_buffer = &body, .out_buffer_size = &body_size, .out_file = NULL,
                          .buffer_capacity = 0, .max_buffer_size = max_bytes,
                          .progress_cb = absolute_deadline_ms != 0 ? total_timeout_progress_cb : NULL,
                          .progress_user_data = &timeout_ctx,
                          .out_content_type = NULL, .out_content_type_size = 0 };

    char location[1024];
    bool head_only = req->method == HTTP_METHOD_HEAD;
    bool ok = read_response_ex(&conn, resp, &sink, location, sizeof(location), head_only);
    http_conn_close(&conn);

    if (!ok) {
        free(body);
        /* Review-adjacent finding, same root cause as the read-timeout
         * fix above: a body read aborted by total_timeout_progress_cb()
         * (sink_write()'s progress-callback hook, not a real socket
         * failure -- the underlying read that triggered this callback
         * already succeeded) left conn.last_read_timed_out false, since
         * nothing about the actual socket read failed. Detect that case
         * the same way cancellation is already detected here: check the
         * condition directly, since read_response_ex() has no other way
         * to signal "the progress callback said stop" distinctly from a
         * real I/O error. */
        if (cancel_requested(cancel)) resp->error = HTTP_ERR_CANCELLED;
        else if (absolute_deadline_ms != 0 && monotonic_now_ms() >= absolute_deadline_ms) resp->error = HTTP_ERR_TIMEOUT;
        return false;
    }

    bool is_redirect = resp->status >= 300 && resp->status < 400 && location[0] != '\0' &&
                        (strncasecmp(location, "http://", 7) == 0 || strncasecmp(location, "https://", 8) == 0);

    if (is_redirect && redirects_left > 0) {
        bool new_is_https;
        char new_host[256], new_port[16], new_path[2048];
        if (!http_conn_parse_url(location, &new_is_https, new_host, sizeof(new_host), new_port, sizeof(new_port),
                                  new_path, sizeof(new_path))) {
            /* Shouldn't happen -- is_redirect already confirmed an
             * http(s):// prefix -- but never follow a Location this
             * function can't itself parse safely. */
            free(body);
            resp->error = HTTP_ERR_INVALID_URL;
            return false;
        }

        /* Review finding: refuse an https -> http downgrade while the
         * caller asked for TLS verification -- a provider redirecting a
         * bearer-token-bearing request to plain HTTP would otherwise send
         * that token in the clear. This is deliberately a hard failure,
         * not a silent strip-and-continue -- a downgrade this severe
         * should be visible to the caller, not quietly worked around. */
        if (is_https && !new_is_https && req->verify_tls) {
            free(body);
            DBG_LOG("http_client: http_request_ex refusing https->http downgrade redirect to '%s'\n", location);
            resp->error = HTTP_ERR_INSECURE_REDIRECT;
            return false;
        }

        /* Review finding: Authorization/Cookie/etc. were previously
         * carried unconditionally to ANY absolute redirect target,
         * including a different host entirely -- a real credential leak
         * for exactly the kind of bearer-token API this app's own plugin
         * foundation is built around. Strip them the moment the origin
         * changes; a same-origin redirect (by far the common case) is
         * unaffected. */
        if (!http_same_origin(is_https, host, port, new_is_https, new_host, new_port)) {
            DBG_LOG("http_client: http_request_ex cross-origin redirect (%s:%s -> %s:%s) -- stripping credential headers\n",
                    host, port, new_host, new_port);
            http_strip_sensitive_headers(req->headers, &req->header_count);
        }

        http_apply_redirect_method_semantics(req, resp->status);

        free(body);
        DBG_LOG("http_client: http_request_ex following redirect status=%d to '%s'\n", resp->status, location);
        memset(resp, 0, sizeof(*resp)); /* each hop gets its own fresh header list, not an accumulation across redirects */
        return http_request_ex_internal(req, cancel, resp, location, redirects_left - 1, absolute_deadline_ms);
    }

    /* redirects_left != req->redirect_limit means at least one hop has
     * already happened in this call chain and the budget genuinely ran
     * out mid-chain -- a real error (also what protects against a
     * malicious/broken server's redirect loop). But if this is still the
     * very FIRST call and the caller's redirect_limit was simply 0 from
     * the start, that is a deliberate "don't follow, let me see it"
     * request -- return the 3xx itself as an ordinary successful
     * transport result instead (matching curl's own --max-redirs 0
     * convention), not an error. */
    if (is_redirect && redirects_left != req->redirect_limit) {
        free(body);
        resp->error = HTTP_ERR_TOO_MANY_REDIRECTS;
        return false;
    }

    resp->body = body;
    resp->body_len = body_size;
    resp->error = HTTP_ERR_NONE;
    return true;
}

/* Review finding: nothing capped connect/read/total_timeout_ms or
 * redirect_limit at the C level at all -- the Lua binding
 * (plugin_manager.c) independently clamps what a plugin can request, but
 * this library function is the authoritative entry point regardless of
 * caller (a future native caller, e.g. an authenticated audio-stream
 * reader, could pass whatever it likes otherwise). An enormous
 * redirect_limit could also drive http_request_ex_internal()'s recursion
 * deep enough to exhaust the C stack; capping it here bounds that
 * regardless of what any caller passes. These are a defensive backstop,
 * not the primary UX-facing limit -- see plugin_manager.c's own,
 * separately-enforced (and separately documented) bounds for the actual
 * plugin-facing ones. */
#define HTTP_REQUEST_MAX_TIMEOUT_MS (5U * 60U * 1000U) /* 5 minutes */
#define HTTP_REQUEST_MAX_REDIRECTS 10

bool http_request_ex(const http_request_t * req_in, http_cancel_token_t * cancel, http_response_t * resp) {
    memset(resp, 0, sizeof(*resp));
    resp->error = HTTP_ERR_NONE;
    if (!http_url_is_valid(req_in->url)) {
        resp->error = req_in->url[0] ? HTTP_ERR_INVALID_REQUEST : HTTP_ERR_INVALID_URL;
        return false;
    }
    for (int i = 0; i < req_in->header_count; i++) {
        if (!http_header_name_is_valid(req_in->headers[i].name) || !http_header_value_is_valid(req_in->headers[i].value)) {
            resp->error = HTTP_ERR_INVALID_REQUEST;
            return false;
        }
    }

    /* Local, mutable copy -- http_request_ex_internal() strips headers /
     * rewrites method/body across redirect hops, and must never do so on
     * the caller's own struct. */
    http_request_t req = *req_in;
    if (req.connect_timeout_ms > HTTP_REQUEST_MAX_TIMEOUT_MS) req.connect_timeout_ms = HTTP_REQUEST_MAX_TIMEOUT_MS;
    if (req.read_timeout_ms > HTTP_REQUEST_MAX_TIMEOUT_MS) req.read_timeout_ms = HTTP_REQUEST_MAX_TIMEOUT_MS;
    if (req.total_timeout_ms > HTTP_REQUEST_MAX_TIMEOUT_MS) req.total_timeout_ms = HTTP_REQUEST_MAX_TIMEOUT_MS;
    if (req.redirect_limit > HTTP_REQUEST_MAX_REDIRECTS) req.redirect_limit = HTTP_REQUEST_MAX_REDIRECTS;
    if (req.redirect_limit < 0) req.redirect_limit = 0;

    uint64_t deadline = req.total_timeout_ms > 0 ? monotonic_now_ms() + req.total_timeout_ms : 0;
    return http_request_ex_internal(&req, cancel, resp, req.url, req.redirect_limit, deadline);
}

void http_response_free(http_response_t * resp) {
    if (!resp) return;
    free(resp->body);
    memset(resp, 0, sizeof(*resp));
}
