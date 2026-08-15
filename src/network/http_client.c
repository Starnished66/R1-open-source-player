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
        uint8_t * grown = realloc(*sink->out_buffer, old_size + len);
        if (!grown) return false;
        memcpy(grown + old_size, data, len);
        *sink->out_buffer = grown;
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

    size_t sent = 0;
    while (sent < (size_t) req_len) {
        int n = http_conn_write(&conn, (const uint8_t *) request + sent, (size_t) req_len - sent);
        if (n <= 0) {
            DBG_LOG("http_client: conn_write failed (n=%d) after %zu/%d bytes\n", n, sent, req_len);
            http_conn_close(&conn);
            return false;
        }
        sent += (size_t) n;
    }

    http_conn_reader_t reader;
    memset(&reader, 0, sizeof(reader));
    reader.conn = &conn;

    char status_line[256];
    if (!http_conn_reader_line(&reader, status_line, sizeof(status_line))) {
        DBG_LOG("http_client: failed to read status line from host='%s'\n", host);
        http_conn_close(&conn);
        return false;
    }
    DBG_LOG("http_client: status line: '%s'\n", status_line);

    int status = 0;
    /* "HTTP/1.1 200 OK" -- skip the version token, read the 3-digit code. */
    const char * sp = strchr(status_line, ' ');
    if (!sp) {
        DBG_LOG("http_client: malformed status line '%s'\n", status_line);
        http_conn_close(&conn);
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

    bool ok = read_body(&reader, is_chunked, content_length, sink);
    http_conn_close(&conn);
    if (!ok) {
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
