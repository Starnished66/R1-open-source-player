#ifndef HTTP_CONN_H
#define HTTP_CONN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/x509_crt.h"

/* Low-level HTTP(S) connection primitives, shared by http_client.c's
 * bounded one-shot GET requests and http_stream.c's unbounded live-stream
 * reader -- URL parsing, opening a plain-TCP-or-mbedTLS connection, and a
 * small buffered line/exact-byte reader on top of it (header/chunk-size
 * parsing needs to read a line at a time, but http_conn_read() only ever
 * hands back whatever happened to already be sitting on the wire). Neither
 * caller needs to know which transport it has -- both go through the same
 * http_conn_read()/http_conn_write(). Originally lived only inside
 * http_client.c; pulled out here once http_stream.c needed the same
 * connect/request/header-parse logic for its own unbounded body read. */

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

/* Splits a http(s):// URL into host/port/path. port is filled with the
 * scheme's default (80/443) if the URL didn't specify one. Returns false on
 * an unrecognized scheme or a component too long for the given buffer. */
bool http_conn_parse_url(const char * url, bool * out_https, char * host, size_t host_size,
                          char * port, size_t port_size, char * path, size_t path_size);

/* Connects (plain TCP for !https, TLS-handshaked for https) to host:port.
 * verify_tls false skips certificate verification, for a self-signed server
 * the caller has explicitly opted to trust -- never the default. Returns
 * false on any connect/handshake/verification failure; the caller must
 * still call http_conn_close() either way, to release whatever TLS state
 * did get initialized before the failure. */
bool http_conn_open(http_conn_t * conn, const char * host, const char * port, bool https, bool verify_tls);

int http_conn_write(http_conn_t * conn, const uint8_t * data, size_t len);

/* Returns bytes read (>0), 0 on clean EOF, <0 on error -- same shape as
 * mbedtls_net_recv/ssl_read themselves, so callers can treat both
 * transports identically. */
int http_conn_read(http_conn_t * conn, uint8_t * buf, size_t len);

void http_conn_close(http_conn_t * conn);

#define HTTP_CONN_READ_CHUNK 4096

typedef struct {
    http_conn_t * conn;
    uint8_t buf[HTTP_CONN_READ_CHUNK];
    size_t len;
    size_t pos;
} http_conn_reader_t;

/* Reads one CRLF- or LF-terminated line (the line itself, without the
 * terminator) into out. Returns false on EOF/error before any terminator
 * was found. */
bool http_conn_reader_line(http_conn_reader_t * r, char * out, size_t out_size);

/* Reads exactly n bytes into out, blocking across multiple underlying
 * reads as needed. Returns false on EOF/error before n bytes arrived. */
bool http_conn_reader_read_exact(http_conn_reader_t * r, uint8_t * out, size_t n);

/* Like http_conn_read(), but reader-aware: serves bytes already sitting in
 * r's internal buffer first (e.g. left over from a prior http_conn_reader_
 * line() call that read further than the line itself -- a real risk for
 * TLS, where mbedtls_ssl_read() hands back a whole decrypted record at
 * once, routinely bundling response headers together with the start of
 * the body in a single underlying read) before falling back to a fresh
 * http_conn_read() once the buffer is empty. Without this, a caller that
 * switches from line-based reading (headers) to raw http_conn_read() (the
 * body) silently drops whatever body bytes had already been buffered --
 * this is exactly that switch, done safely. Same return contract as
 * http_conn_read() (bytes read, 0 on clean EOF, <0 on error). */
int http_conn_reader_read_some(http_conn_reader_t * r, uint8_t * buf, size_t len);

#endif /* HTTP_CONN_H */
