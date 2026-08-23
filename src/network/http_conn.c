#include "http_conn.h"
#include "http_client.h" /* for struct http_cancel_token's real definition */
#include "ca_bundle.h"

#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netdb.h>
#include <unistd.h>

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

static uint64_t monotonic_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000ULL + (uint64_t) ts.tv_nsec / 1000000ULL;
}

static bool cancel_requested_conn(struct http_cancel_token * cancel) {
    if (!cancel) return false;
    pthread_mutex_lock(&cancel->mutex);
    bool requested = cancel->cancel_requested;
    pthread_mutex_unlock(&cancel->mutex);
    return requested;
}

/* Registers fd into cancel (compare-and-set: only succeeds if the token
 * hasn't already been cancelled). Returns false if cancellation raced us
 * -- caller should abandon fd immediately in that case. */
static bool cancel_register(struct http_cancel_token * cancel, int fd) {
    if (!cancel) return true;
    pthread_mutex_lock(&cancel->mutex);
    bool already = cancel->cancel_requested;
    if (!already) cancel->fd = fd;
    pthread_mutex_unlock(&cancel->mutex);
    return !already;
}

/* Review finding (round 2): clearing cancel->fd and closing fd as two
 * SEPARATE steps -- even with the compare-before-clear above, and even
 * with http_cancel_token_cancel() itself now reading fd and calling
 * shutdown() atomically under the same mutex -- still leaves the
 * unregister (lock/clear/unlock) and the actual close() as two
 * independent operations. Belt and suspenders, matching the reviewer's
 * own explicit recommendation: hold cancel's mutex for the unregister
 * AND the real close() together, as one atomic operation, exactly
 * mirroring http_cancel_token_cancel()'s own read-fd-and-shutdown being
 * one atomic operation under the same mutex. Between the two, the fd can
 * never be observed by a canceller in a state where it's already closed
 * but the token doesn't know it yet, or vice versa. */
static void cancel_unregister_and_close(struct http_cancel_token * cancel, int fd) {
    if (!cancel) {
        close(fd);
        return;
    }
    pthread_mutex_lock(&cancel->mutex);
    if (cancel->fd == fd) cancel->fd = -1;
    close(fd);
    pthread_mutex_unlock(&cancel->mutex);
}

/* Equivalent to mbedtls_net_connect(), bounded by connect_timeout_ms (0 =
 * no timeout, waits indefinitely -- poll()'s own -1 timeout, not a
 * fallback to mbedtls_net_connect() anymore, see below) via a non-blocking
 * connect()+poll(). mbedtls_net_connect() itself has no connect-timeout
 * parameter in this vendored version (confirmed directly from mbedtls/
 * library/net_sockets.c, not assumed), and offers no way to register a
 * cancel token either, which is why this always uses its own connect path
 * now rather than only when a timeout is requested -- review finding:
 * previously, connect_timeout_ms == 0 fell through to plain
 * mbedtls_net_connect(), which could not be cancelled at all during
 * connect. Registers EVERY candidate socket into `cancel` immediately
 * after socket() creation, before connect()/poll(), so a cancel during
 * either is caught, not just during a later blocked read. Applies the
 * SAME timeout budget to each resolved address in turn (not a single
 * shared deadline split across them) -- a deliberate simplification: real
 * hostnames here resolve to one or two addresses, and the worst case
 * (every address individually timing out) is still a bounded, small
 * multiple of connect_timeout_ms, not unbounded. */
static http_conn_error_t net_connect_timeout(mbedtls_net_context * ctx, const char * host, const char * port,
                                              uint32_t connect_timeout_ms, struct http_cancel_token * cancel) {
    if (cancel_requested_conn(cancel)) return HTTP_CONN_ERR_CANCELLED;

    /* DNS resolution itself is a single blocking libc call with no
     * portable way to interrupt from another thread in this codebase (no
     * async resolver here) -- a cancel requested while blocked here is
     * only noticed once getaddrinfo() returns. Documented honestly in
     * http_conn.h rather than claimed as covered. */
    struct addrinfo hints, * addr_list, * cur;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    if (getaddrinfo(host, port, &hints, &addr_list) != 0) return HTTP_CONN_ERR_DNS;

    if (cancel_requested_conn(cancel)) { freeaddrinfo(addr_list); return HTTP_CONN_ERR_CANCELLED; }

    http_conn_error_t result = HTTP_CONN_ERR_CONNECT;
    for (cur = addr_list; cur != NULL; cur = cur->ai_next) {
        int fd = socket(cur->ai_family, cur->ai_socktype, cur->ai_protocol);
        if (fd < 0) { result = HTTP_CONN_ERR_SOCKET; continue; }

        if (!cancel_register(cancel, fd)) {
            close(fd);
            result = HTTP_CONN_ERR_CANCELLED;
            break;
        }

        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        bool connected = connect(fd, cur->ai_addr, cur->ai_addrlen) == 0;
        if (!connected && errno == EINPROGRESS) {
            struct pollfd pfd = { .fd = fd, .events = POLLOUT, .revents = 0 };
            /* poll()'s own -1 means "wait indefinitely" -- exactly
             * connect_timeout_ms == 0's intended meaning, and shutdown()
             * (from a cancel) still unblocks it immediately either way. */
            int pr = poll(&pfd, 1, connect_timeout_ms > 0 ? (int) connect_timeout_ms : -1);
            if (pr == 0) {
                cancel_unregister_and_close(cancel, fd);
                result = HTTP_CONN_ERR_CONNECT_TIMEOUT;
                continue;
            }
            if (pr < 0) {
                bool cancelled = cancel_requested_conn(cancel);
                cancel_unregister_and_close(cancel, fd);
                result = cancelled ? HTTP_CONN_ERR_CANCELLED : HTTP_CONN_ERR_CONNECT;
                continue;
            }
            int so_error = 0;
            socklen_t so_len = sizeof(so_error);
            connected = getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &so_len) == 0 && so_error == 0;
        }
        if (!connected) {
            bool cancelled = cancel_requested_conn(cancel);
            cancel_unregister_and_close(cancel, fd);
            result = cancelled ? HTTP_CONN_ERR_CANCELLED : HTTP_CONN_ERR_CONNECT;
            continue;
        }

        fcntl(fd, F_SETFL, flags); /* back to blocking for the rest of this connection's life */
        ctx->fd = fd;
        result = HTTP_CONN_OK;
        break;
    }

    freeaddrinfo(addr_list);
    return result;
}

bool http_conn_open_ex(http_conn_t * conn, const char * host, const char * port, bool https, bool verify_tls,
                        uint32_t connect_timeout_ms, uint32_t read_timeout_ms,
                        struct http_cancel_token * cancel, http_conn_error_t * out_error) {
    http_conn_error_t local_error = HTTP_CONN_OK;
    if (!out_error) out_error = &local_error;
    *out_error = HTTP_CONN_OK;

    memset(conn, 0, sizeof(*conn));
    conn->is_https = https;
    conn->cancel_token = cancel;
    mbedtls_net_init(&conn->net);

    *out_error = net_connect_timeout(&conn->net, host, port, connect_timeout_ms, cancel);
    if (*out_error != HTTP_CONN_OK) return false;

    if (read_timeout_ms > 0) {
        struct timeval tv = { .tv_sec = (time_t) (read_timeout_ms / 1000), .tv_usec = (suseconds_t) (read_timeout_ms % 1000) * 1000 };
        setsockopt(conn->net.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(conn->net.fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }

    if (!https) return true;

    mbedtls_ssl_init(&conn->ssl);
    mbedtls_ssl_config_init(&conn->conf);
    mbedtls_x509_crt_init(&conn->cacert);
    mbedtls_ctr_drbg_init(&conn->ctr_drbg);
    mbedtls_entropy_init(&conn->entropy);
    conn->ssl_initialized = true;

    if (mbedtls_ctr_drbg_seed(&conn->ctr_drbg, mbedtls_entropy_func, &conn->entropy, NULL, 0) != 0) {
        *out_error = HTTP_CONN_ERR_TLS_SETUP;
        return false;
    }

    if (mbedtls_ssl_config_defaults(&conn->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                     MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
        *out_error = HTTP_CONN_ERR_TLS_SETUP;
        return false;
    }
    mbedtls_ssl_conf_rng(&conn->conf, mbedtls_ctr_drbg_random, &conn->ctr_drbg);

    if (verify_tls) {
        if (mbedtls_x509_crt_parse(&conn->cacert, ca_bundle_pem, ca_bundle_pem_len) < 0) {
            *out_error = HTTP_CONN_ERR_TLS_SETUP;
            return false;
        }
        mbedtls_ssl_conf_ca_chain(&conn->conf, &conn->cacert, NULL);
        mbedtls_ssl_conf_authmode(&conn->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    } else {
        /* Explicit, per-connection opt-out (subsonic_client.h's
         * "trust this self-signed server" setting) -- never a silent
         * default. */
        mbedtls_ssl_conf_authmode(&conn->conf, MBEDTLS_SSL_VERIFY_NONE);
    }

    if (mbedtls_ssl_setup(&conn->ssl, &conn->conf) != 0) {
        *out_error = HTTP_CONN_ERR_TLS_SETUP;
        return false;
    }
    if (mbedtls_ssl_set_hostname(&conn->ssl, host) != 0) {
        *out_error = HTTP_CONN_ERR_TLS_SETUP;
        return false;
    }
    mbedtls_ssl_set_bio(&conn->ssl, &conn->net, mbedtls_net_send, mbedtls_net_recv, NULL);

    /* Correction to the comment this replaced: it assumed a SO_RCVTIMEO
     * timeout surfaces as MBEDTLS_ERR_SSL_WANT_READ. On this fd it does
     * not -- see http_conn_read()'s own comment for the full explanation
     * (net_would_block() only ever fires for a non-blocking fd, and this
     * connection is deliberately restored to blocking after connect).
     * An EAGAIN here instead makes mbedtls_ssl_handshake() surface a
     * real, non-WANT_READ/WRITE error from its underlying f_recv -- which
     * would previously have been misreported as a bare handshake failure
     * on the very first timeout, never even reaching the wall-clock
     * check below. Check errno for EAGAIN/EWOULDBLOCK the same way
     * http_conn_read() does, immediately after the call, and treat it as
     * the equivalent of WANT_READ so the deadline logic below actually
     * runs. Retrying unconditionally on would-block (as the original,
     * timeout-less version of this loop did) would silently turn a real
     * socket timeout into an infinite retry loop instead of a bounded
     * failure -- track wall-clock elapsed time explicitly and give up
     * once it exceeds read_timeout_ms, only when a timeout was actually
     * requested; read_timeout_ms == 0 keeps the original
     * unconditional-retry behavior exactly. A cancel-driven shutdown()
     * during this loop instead surfaces as a clean EOF (recv() returning
     * 0) from the transport, which mbedTLS turns into a real (non-would-
     * block) handshake error -- already correctly handled by the
     * existing "not would-block" branch. */
    uint64_t handshake_deadline = read_timeout_ms > 0 ? monotonic_now_ms() + read_timeout_ms : 0;
    int ret;
    for (;;) {
        errno = 0;
        ret = mbedtls_ssl_handshake(&conn->ssl);
        if (ret == 0) break;
        bool would_block = (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) ||
                            (errno == EAGAIN || errno == EWOULDBLOCK);
        if (!would_block) {
            *out_error = cancel_requested_conn(cancel) ? HTTP_CONN_ERR_CANCELLED : HTTP_CONN_ERR_TLS_HANDSHAKE;
            return false;
        }
        if (handshake_deadline != 0 && monotonic_now_ms() >= handshake_deadline) {
            *out_error = HTTP_CONN_ERR_TLS_HANDSHAKE_TIMEOUT;
            return false;
        }
    }

    if (verify_tls && mbedtls_ssl_get_verify_result(&conn->ssl) != 0) {
        *out_error = HTTP_CONN_ERR_TLS_VERIFY;
        return false;
    }
    return true;
}

bool http_conn_open(http_conn_t * conn, const char * host, const char * port, bool https, bool verify_tls) {
    return http_conn_open_ex(conn, host, port, https, verify_tls, 0, 0, NULL, NULL);
}

int http_conn_write(http_conn_t * conn, const uint8_t * data, size_t len) {
    if (conn->is_https) return mbedtls_ssl_write(&conn->ssl, data, len);
    return mbedtls_net_send(&conn->net, data, len);
}

int http_conn_read(http_conn_t * conn, uint8_t * buf, size_t len) {
    errno = 0;
    int n = conn->is_https ? mbedtls_ssl_read(&conn->ssl, buf, len) : mbedtls_net_recv(&conn->net, buf, len);
    if (n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
        conn->last_read_timed_out = false;
        return 0;
    }
    /* Correction to the comment this replaced: that comment assumed a
     * SO_RCVTIMEO timeout on our socket surfaces as MBEDTLS_ERR_SSL_
     * WANT_READ/WRITE. It does not, for the sockets this code actually
     * uses. mbedtls/library/net_sockets.c's net_would_block() explicitly
     * documents "on a blocking socket this function always returns 0" --
     * and net_connect_timeout() (this file) deliberately restores the fd
     * to blocking mode after the non-blocking connect/poll dance
     * ("back to blocking for the rest of this connection's life"). So on
     * every read after connect, an EAGAIN from SO_RCVTIMEO makes
     * mbedtls_net_recv() fall through past its own would-block check and
     * return the generic MBEDTLS_ERR_NET_RECV_FAILED instead --
     * indistinguishable from a real I/O error by return code alone.
     * mbedtls's own would-block check reads errno into a local, restores
     * it unchanged, and returns without a further syscall in between --
     * so errno is still exactly what the failing read()/recv() left it
     * as when we get back here. Check it directly rather than trusting a
     * WANT_READ/WRITE code this configuration can never actually
     * produce; the WANT_READ/WRITE check is kept too since it's what a
     * hypothetical future non-blocking caller of this same function
     * would actually see. Updated on every call, not just failures, so a
     * stale true from an earlier read can never leak into a later,
     * unrelated failure's diagnosis. */
    conn->last_read_timed_out = (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) ||
                                 (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
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
    /* Review finding (round 2): unregistering (compare-and-clear) and
     * THEN separately calling mbedtls_net_free() (the real close) was
     * still two non-atomic steps, even with http_cancel_token_cancel()
     * itself already fixed to read fd and shutdown() atomically -- hold
     * the SAME mutex across both the unregister and the actual close
     * here too, exactly mirroring the other side, so the two can never
     * interleave in a way that lets a cancel() observe a half-updated
     * state (token still says a since-closed-and-possibly-reused fd is
     * valid). Without a cancel_token at all, this is just the original,
     * unsynchronized close -- nothing to serialize against. */
    struct http_cancel_token * cancel = conn->cancel_token;
    if (cancel) {
        pthread_mutex_lock(&cancel->mutex);
        if (cancel->fd == conn->net.fd) cancel->fd = -1;
        mbedtls_net_free(&conn->net);
        pthread_mutex_unlock(&cancel->mutex);
    } else {
        mbedtls_net_free(&conn->net);
    }
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
