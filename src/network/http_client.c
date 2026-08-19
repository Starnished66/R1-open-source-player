#include "http_client.h"
#include "http_conn.h"
#include "debug_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

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

static bool do_get(const char * url, bool verify_tls, int * out_status, body_sink_t * sink) {
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

bool http_get_to_buffer(const char * url, bool verify_tls, int * out_status, uint8_t ** out_body, size_t * out_body_size) {
    *out_body = NULL;
    *out_body_size = 0;

    body_sink_t sink = { .out_buffer = out_body, .out_buffer_size = out_body_size, .out_file = NULL,
                          .buffer_capacity = 0, .max_buffer_size = 8 * 1024 * 1024,
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

bool http_post_to_buffer(const char * url, bool verify_tls, const char * content_type, const uint8_t * body,
                          size_t body_size, int * out_status, uint8_t ** out_body, size_t * out_body_size) {
    *out_body = NULL;
    *out_body_size = 0;

    body_sink_t sink = { .out_buffer = out_body, .out_buffer_size = out_body_size, .out_file = NULL,
                          .buffer_capacity = 0, .max_buffer_size = 8 * 1024 * 1024,
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
