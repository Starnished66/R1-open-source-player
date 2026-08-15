#include "http_conn.h"
#include "ca_bundle.h"

#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>

bool http_conn_parse_url(const char * url, bool * out_https, char * host, size_t host_size,
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

    /* A "#fragment" is purely client-side (RFC 3986) and must never be sent
     * to the server -- strip it before it becomes part of the request path.
     * (audio.c's decoder_open() deliberately relies on this: it reads a
     * "#.<ext>" suffix off a stream URL as a local-only format hint before
     * ever handing the URL to this function, so the fragment must not leak
     * into the actual HTTP request line.) */
    const char * frag = strchr(p, '#');
    size_t p_len = frag ? (size_t) (frag - p) : strlen(p);

    if (p_len == 0) {
        snprintf(path, path_size, "/");
    } else {
        snprintf(path, path_size, "%.*s", (int) p_len, p);
    }
    return true;
}

bool http_conn_open(http_conn_t * conn, const char * host, const char * port, bool https, bool verify_tls) {
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

int http_conn_write(http_conn_t * conn, const uint8_t * data, size_t len) {
    if (conn->is_https) return mbedtls_ssl_write(&conn->ssl, data, len);
    return mbedtls_net_send(&conn->net, data, len);
}

int http_conn_read(http_conn_t * conn, uint8_t * buf, size_t len) {
    int n = conn->is_https ? mbedtls_ssl_read(&conn->ssl, buf, len) : mbedtls_net_recv(&conn->net, buf, len);
    if (n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return 0;
    return n;
}

void http_conn_close(http_conn_t * conn) {
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

static int reader_fill(http_conn_reader_t * r) {
    r->pos = 0;
    int n = http_conn_read(r->conn, r->buf, sizeof(r->buf));
    r->len = (n > 0) ? (size_t) n : 0;
    return n;
}

bool http_conn_reader_line(http_conn_reader_t * r, char * out, size_t out_size) {
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

bool http_conn_reader_read_exact(http_conn_reader_t * r, uint8_t * out, size_t n) {
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

int http_conn_reader_read_some(http_conn_reader_t * r, uint8_t * buf, size_t len) {
    if (r->pos < r->len) {
        size_t avail = r->len - r->pos;
        size_t take = avail < len ? avail : len;
        memcpy(buf, r->buf + r->pos, take);
        r->pos += take;
        return (int) take;
    }
    return http_conn_read(r->conn, buf, len);
}
