#include "http_client.h"
#include "ca_bundle.h"
#include "debug_log.h"

#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/x509_crt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define READ_CHUNK 4096

/* Plain TCP for http://, mbedTLS-wrapped for https:// -- both go through
 * the same conn_read()/conn_write() so the request-building and
 * response-parsing code below doesn't need to know which one it has. */
typedef struct {
    bool is_https;
    mbedtls_net_context net;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_x509_crt cacert;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_entropy_context entropy;
    bool ssl_initialized; /* only the https fields above need mbedtls_*_free() */
} http_conn_t;

static bool parse_url(const char * url, bool * out_https, char * host, size_t host_size,
                       char * port, size_t port_size, char * path, size_t path_size) {
    const char * p;
    if (strncasecmp(url, "https://", 8) == 0) { *out_https = true; p = url + 8; }
    else if (strncasecmp(url, "http://", 7) == 0) { *out_https = false; p = url + 7; }
    else return false;

    const char * host_start = p;
    const char * host_end = p;
    while (*host_end && *host_end != ':' && *host_end != '/') host_end++;
    size_t host_len = (size_t) (host_end - host_start);
    if (host_len == 0 || host_len >= host_size) return false;
    memcpy(host, host_start, host_len);
    host[host_len] = '\0';

    p = host_end;
    if (*p == ':') {
        p++;
        const char * port_start = p;
        while (*p && *p != '/') p++;
        size_t port_len = (size_t) (p - port_start);
        if (port_len == 0 || port_len >= port_size) return false;
        memcpy(port, port_start, port_len);
        port[port_len] = '\0';
    } else {
        snprintf(port, port_size, "%s", *out_https ? "443" : "80");
    }

    if (*p == '\0') {
        snprintf(path, path_size, "/");
    } else {
        snprintf(path, path_size, "%s", p);
    }
    return true;
}

static bool conn_open(http_conn_t * conn, const char * host, const char * port, bool https, bool verify_tls) {
    memset(conn, 0, sizeof(*conn));
    conn->is_https = https;
    mbedtls_net_init(&conn->net);

    if (mbedtls_net_connect(&conn->net, host, port, MBEDTLS_NET_PROTO_TCP) != 0) return false;

    if (!https) return true;

    mbedtls_ssl_init(&conn->ssl);
    mbedtls_ssl_config_init(&conn->conf);
    mbedtls_x509_crt_init(&conn->cacert);
    mbedtls_ctr_drbg_init(&conn->ctr_drbg);
    mbedtls_entropy_init(&conn->entropy);
    conn->ssl_initialized = true;

    if (mbedtls_ctr_drbg_seed(&conn->ctr_drbg, mbedtls_entropy_func, &conn->entropy, NULL, 0) != 0) return false;

    if (mbedtls_ssl_config_defaults(&conn->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                     MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
        return false;
    }
    mbedtls_ssl_conf_rng(&conn->conf, mbedtls_ctr_drbg_random, &conn->ctr_drbg);

    if (verify_tls) {
        if (mbedtls_x509_crt_parse(&conn->cacert, ca_bundle_pem, ca_bundle_pem_len) < 0) return false;
        mbedtls_ssl_conf_ca_chain(&conn->conf, &conn->cacert, NULL);
        mbedtls_ssl_conf_authmode(&conn->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    } else {
        /* Explicit, per-connection opt-out (subsonic_client.h's
         * "trust this self-signed server" setting) -- never a silent
         * default. */
        mbedtls_ssl_conf_authmode(&conn->conf, MBEDTLS_SSL_VERIFY_NONE);
    }

    if (mbedtls_ssl_setup(&conn->ssl, &conn->conf) != 0) return false;
    if (mbedtls_ssl_set_hostname(&conn->ssl, host) != 0) return false;
    mbedtls_ssl_set_bio(&conn->ssl, &conn->net, mbedtls_net_send, mbedtls_net_recv, NULL);

    int ret;
    while ((ret = mbedtls_ssl_handshake(&conn->ssl)) != 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) return false;
    }

    if (verify_tls && mbedtls_ssl_get_verify_result(&conn->ssl) != 0) return false;
    return true;
}

static int conn_write(http_conn_t * conn, const uint8_t * data, size_t len) {
    if (conn->is_https) return mbedtls_ssl_write(&conn->ssl, data, len);
    return mbedtls_net_send(&conn->net, data, len);
}

/* Returns bytes read (>0), 0 on clean EOF, <0 on error -- same shape as
 * mbedtls_net_recv/ssl_read themselves, so callers can treat both
 * transports identically. */
static int conn_read(http_conn_t * conn, uint8_t * buf, size_t len) {
    int n = conn->is_https ? mbedtls_ssl_read(&conn->ssl, buf, len) : mbedtls_net_recv(&conn->net, buf, len);
    if (n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return 0;
    return n;
}

static void conn_close(http_conn_t * conn) {
    if (conn->is_https) {
        if (conn->ssl_initialized) {
            mbedtls_ssl_close_notify(&conn->ssl);
            mbedtls_ssl_free(&conn->ssl);
            mbedtls_ssl_config_free(&conn->conf);
            mbedtls_x509_crt_free(&conn->cacert);
            mbedtls_ctr_drbg_free(&conn->ctr_drbg);
            mbedtls_entropy_free(&conn->entropy);
        }
    }
    mbedtls_net_free(&conn->net);
}

/* Small buffered reader over conn_read(), since header/chunk-size parsing
 * needs to read a line at a time but conn_read() only hands back whatever
 * arrived on the wire. */
typedef struct {
    http_conn_t * conn;
    uint8_t buf[READ_CHUNK];
    size_t len;
    size_t pos;
} buffered_reader_t;

static int reader_fill(buffered_reader_t * r) {
    r->pos = 0;
    int n = conn_read(r->conn, r->buf, sizeof(r->buf));
    r->len = (n > 0) ? (size_t) n : 0;
    return n;
}

/* Reads one CRLF- or LF-terminated line (the line itself, without the
 * terminator) into out. Returns false on EOF/error before any terminator
 * was found. */
static bool reader_line(buffered_reader_t * r, char * out, size_t out_size) {
    size_t out_pos = 0;
    for (;;) {
        if (r->pos >= r->len) {
            int n = reader_fill(r);
            if (n <= 0) return false;
        }
        uint8_t c = r->buf[r->pos++];
        if (c == '\n') {
            if (out_pos > 0 && out[out_pos - 1] == '\r') out_pos--;
            out[out_pos] = '\0';
            return true;
        }
        if (out_pos + 1 < out_size) out[out_pos++] = (char) c;
    }
}

static bool reader_read_exact(buffered_reader_t * r, uint8_t * out, size_t n) {
    size_t got = 0;
    while (got < n) {
        if (r->pos >= r->len) {
            int rn = reader_fill(r);
            if (rn <= 0) return false;
        }
        size_t avail = r->len - r->pos;
        size_t want = n - got;
        size_t take = avail < want ? avail : want;
        memcpy(out + got, r->buf + r->pos, take);
        r->pos += take;
        got += take;
    }
    return true;
}

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
static bool read_body(buffered_reader_t * r, bool is_chunked, uint64_t content_length, body_sink_t * sink) {
    uint8_t chunk[READ_CHUNK];

    if (is_chunked) {
        uint64_t total = 0;
        for (;;) {
            char size_line[64];
            if (!reader_line(r, size_line, sizeof(size_line))) return false;
            char * semi = strchr(size_line, ';');
            if (semi) *semi = '\0';
            uint64_t chunk_size = strtoull(size_line, NULL, 16);
            if (chunk_size == 0) break;

            uint64_t remaining = chunk_size;
            while (remaining > 0) {
                size_t take = remaining < sizeof(chunk) ? (size_t) remaining : sizeof(chunk);
                if (!reader_read_exact(r, chunk, take)) return false;
                total += take;
                if (!sink_write(sink, chunk, take, total, 0)) return false;
                remaining -= take;
            }

            char crlf[4];
            if (!reader_line(r, crlf, sizeof(crlf))) return false; /* trailing CRLF after each chunk's data */
        }
        /* Trailing headers (if any) up to the final blank line. */
        char trailer[512];
        while (reader_line(r, trailer, sizeof(trailer)) && trailer[0] != '\0') { }
        return true;
    }

    uint64_t remaining = content_length;
    uint64_t total = 0;
    while (remaining > 0) {
        size_t take = remaining < sizeof(chunk) ? (size_t) remaining : sizeof(chunk);
        if (!reader_read_exact(r, chunk, take)) return false;
        total += take;
        if (!sink_write(sink, chunk, take, total, content_length)) return false;
        remaining -= take;
    }
    return true;
}

static bool do_get(const char * url, bool verify_tls, int * out_status, body_sink_t * sink) {
    bool is_https;
    char host[256], port[16], path[2048];
    if (!parse_url(url, &is_https, host, sizeof(host), port, sizeof(port), path, sizeof(path))) {
        DBG_LOG("http_client: parse_url failed for '%s'\n", url);
        return false;
    }

    http_conn_t conn;
    if (!conn_open(&conn, host, port, is_https, verify_tls)) {
        DBG_LOG("http_client: conn_open failed for host='%s' port='%s' https=%d\n", host, port, is_https);
        conn_close(&conn);
        return false;
    }

    char request[2560];
    int req_len = snprintf(request, sizeof(request),
                            "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: open_hiby_player\r\nConnection: close\r\n\r\n",
                            path, host);
    if (req_len < 0 || (size_t) req_len >= sizeof(request)) {
        DBG_LOG("http_client: request too long for path='%s'\n", path);
        conn_close(&conn);
        return false;
    }

    size_t sent = 0;
    while (sent < (size_t) req_len) {
        int n = conn_write(&conn, (const uint8_t *) request + sent, (size_t) req_len - sent);
        if (n <= 0) {
            DBG_LOG("http_client: conn_write failed (n=%d) after %zu/%d bytes\n", n, sent, req_len);
            conn_close(&conn);
            return false;
        }
        sent += (size_t) n;
    }

    buffered_reader_t reader;
    memset(&reader, 0, sizeof(reader));
    reader.conn = &conn;

    char status_line[256];
    if (!reader_line(&reader, status_line, sizeof(status_line))) {
        DBG_LOG("http_client: failed to read status line from host='%s'\n", host);
        conn_close(&conn);
        return false;
    }
    DBG_LOG("http_client: status line: '%s'\n", status_line);

    int status = 0;
    /* "HTTP/1.1 200 OK" -- skip the version token, read the 3-digit code. */
    const char * sp = strchr(status_line, ' ');
    if (!sp) {
        DBG_LOG("http_client: malformed status line '%s'\n", status_line);
        conn_close(&conn);
        return false;
    }
    status = atoi(sp + 1);

    bool is_chunked = false;
    uint64_t content_length = 0;
    char header_line[1024];
    while (reader_line(&reader, header_line, sizeof(header_line)) && header_line[0] != '\0') {
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
    conn_close(&conn);
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
