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

struct http_cancel_token; /* defined in http_client.h */

typedef enum {
    HTTP_CONN_OK = 0,
    HTTP_CONN_ERR_CANCELLED,
    HTTP_CONN_ERR_DNS,
    HTTP_CONN_ERR_SOCKET,
    HTTP_CONN_ERR_CONNECT,
    HTTP_CONN_ERR_CONNECT_TIMEOUT,
    HTTP_CONN_ERR_TLS_SETUP,
    HTTP_CONN_ERR_TLS_HANDSHAKE,
    HTTP_CONN_ERR_TLS_HANDSHAKE_TIMEOUT,
    HTTP_CONN_ERR_TLS_VERIFY
} http_conn_error_t;

/* Low-level HTTP(S) connection primitives shared by http_client and http_stream:
 * URL parsing, opening plain TCP or mbedTLS connections, and buffered
 * reading of lines and exact byte sequences. */

typedef struct {
    bool is_https;
    mbedtls_net_context net;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_x509_crt cacert;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_entropy_context entropy;
    bool ssl_initialized; /* only the https fields above need mbedtls_*_free() */
    bool last_read_timed_out;
    struct http_cancel_token * cancel_token;
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
bool http_conn_open_ex(http_conn_t * conn, const char * host, const char * port, bool https, bool verify_tls,
                        uint32_t connect_timeout_ms, uint32_t read_timeout_ms,
                        struct http_cancel_token * cancel, http_conn_error_t * out_error);

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

/* Like http_conn_read(), but consumes any remaining bytes in the reader's
 * internal buffer before reading directly from the connection. */
int http_conn_reader_read_some(http_conn_reader_t * r, uint8_t * buf, size_t len);

#endif /* HTTP_CONN_H */
