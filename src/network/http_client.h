#ifndef HTTP_CLIENT_H
#define HTTP_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

#define HTTP_MAX_HEADERS 32
#define HTTP_HEADER_NAME_MAX 64
#define HTTP_HEADER_VALUE_MAX 768

#define HTTP_ERR_NONE ""
#define HTTP_ERR_CANCELLED "cancelled"
#define HTTP_ERR_DNS "dns"
#define HTTP_ERR_CONNECT "connect"
#define HTTP_ERR_CONNECT_TIMEOUT "connect_timeout"
#define HTTP_ERR_TLS "tls"
#define HTTP_ERR_TIMEOUT "timeout"
#define HTTP_ERR_MALFORMED "malformed"
#define HTTP_ERR_IO "io"
#define HTTP_ERR_RESPONSE_TOO_LARGE "response_too_large"
#define HTTP_ERR_INVALID_URL "invalid_url"
#define HTTP_ERR_INVALID_REQUEST "invalid_request"
#define HTTP_ERR_TOO_MANY_REDIRECTS "too_many_redirects"
#define HTTP_ERR_INSECURE_REDIRECT "insecure_redirect"

typedef struct http_cancel_token {
    pthread_mutex_t mutex;
    int fd;
    bool cancel_requested;
} http_cancel_token_t;

void http_cancel_token_init(http_cancel_token_t * tok);
void http_cancel_token_destroy(http_cancel_token_t * tok);
void http_cancel_token_cancel(http_cancel_token_t * tok);
bool http_cancel_token_is_cancelled(http_cancel_token_t * tok);

typedef enum {
    HTTP_METHOD_GET = 0,
    HTTP_METHOD_POST,
    HTTP_METHOD_PUT,
    HTTP_METHOD_PATCH,
    HTTP_METHOD_DELETE,
    HTTP_METHOD_HEAD
} http_method_t;

typedef struct {
    char name[HTTP_HEADER_NAME_MAX];
    char value[HTTP_HEADER_VALUE_MAX];
} http_header_t;

const char * http_headers_get(const http_header_t * headers, int count, const char * name);

typedef struct {
    char url[2048];
    http_method_t method;
    http_header_t headers[HTTP_MAX_HEADERS];
    int header_count;
    const uint8_t * body;
    size_t body_len;
    const char * content_type;
    bool verify_tls;
    uint32_t connect_timeout_ms;
    uint32_t read_timeout_ms;
    uint32_t total_timeout_ms;
    size_t max_response_bytes;
    int redirect_limit;
} http_request_t;

typedef struct {
    int status;
    http_header_t headers[HTTP_MAX_HEADERS];
    int header_count;
    uint8_t * body;
    size_t body_len;
    const char * error;
} http_response_t;

bool http_request_ex(const http_request_t * req_in, http_cancel_token_t * cancel, http_response_t * resp);
void http_response_free(http_response_t * resp);

/* One-shot GET request (used for the small JSON API responses -- Subsonic's
 * REST API is GET-only). Buffers the whole response body in memory, which
 * is fine for API calls but not for streaming a whole song (see
 * http_get_to_file below). *out_body is malloc'd; caller must free() it.
 * Returns false on any network/TLS-level failure (DNS, connect, handshake,
 * a malformed response) -- a real HTTP error status (404, 500, ...) still
 * returns true, since that's a valid response the caller should inspect
 * via *out_status. verify_tls false skips certificate verification, for
 * self-signed servers the user has explicitly opted to trust (see
 * subsonic_client.h) -- never the default. */
bool http_get_to_buffer(const char * url, bool verify_tls, int * out_status, uint8_t ** out_body, size_t * out_body_size);
bool http_get_to_buffer_limited(const char * url, bool verify_tls, size_t max_body_size, int * out_status,
                                uint8_t ** out_body, size_t * out_body_size);

/* Same contract as http_get_to_buffer() above (false on network/TLS-level
 * failure, true with *out_status set otherwise -- a 4xx/5xx is still a
 * "successful" request as far as this function's return value is
 * concerned), for a POST instead. body/body_size may be NULL/0 for an empty
 * body. content_type is sent as the Content-Type header verbatim -- pass
 * "application/x-www-form-urlencoded" for a plain key=value&key=value body,
 * "application/json" for a JSON body, etc.; this function doesn't encode or
 * validate body itself, just sends whatever bytes it's given with the
 * Content-Length they imply. *out_body is malloc'd; caller must free() it. */
bool http_post_to_buffer(const char * url, bool verify_tls, const char * content_type, const uint8_t * body,
                          size_t body_size, int * out_status, uint8_t ** out_body, size_t * out_body_size);
bool http_post_to_buffer_limited(const char * url, bool verify_tls, const char * content_type, const uint8_t * body,
                                  size_t body_size, size_t max_body_size, int * out_status, uint8_t ** out_body,
                                  size_t * out_body_size);

/* Optional progress callback for http_get_to_file: bytes_downloaded so far,
 * and total_bytes from the response's Content-Length header (0 if the
 * server didn't send one, e.g. chunked transfer-encoding -- callers should
 * treat 0 as "unknown total", not "already done"). Returning false aborts
 * the download (e.g. the user navigated away or hit stop). */
typedef bool (*http_progress_cb_t)(uint64_t bytes_downloaded, uint64_t total_bytes, void * user_data);

/* Streams the response body straight to dest_path (created fresh, or
 * truncated if it already exists) rather than buffering it in memory --
 * used for downloading a track to a local temp file before handing it to
 * the existing (local-file-only) decoder pipeline; see subsonic_client.h
 * for why streaming decode isn't done instead. */
bool http_get_to_file(const char * url, bool verify_tls, const char * dest_path,
                       http_progress_cb_t progress_cb, void * progress_user_data);

/* Like http_get_to_file(), but also reports the response's Content-Type
 * header (charset parameter stripped, if any) into out_content_type --
 * empty string if the server didn't send one. out_content_type/
 * out_content_type_size may be NULL/0 to skip this (equivalent to plain
 * http_get_to_file()). For a caller that needs to pick a decoder/file
 * extension from the server's own declared type rather than the URL,
 * which may be an opaque token with no extension at all (e.g. a DLNA
 * SetAVTransportURI media URL). */
bool http_get_to_file_ex(const char * url, bool verify_tls, const char * dest_path,
                          http_progress_cb_t progress_cb, void * progress_user_data,
                          char * out_content_type, size_t out_content_type_size);

/* Like http_get_to_file(), with a hard body-size cap. max_body_size 0 means
 * the internal default (2 GiB). Used by plugin.download_file_async(). */
bool http_get_to_file_bounded(const char * url, bool verify_tls, const char * dest_path, size_t max_body_size,
                               http_progress_cb_t progress_cb, void * progress_user_data);

/* Cancellable, bounded-wait variant for application-owned background jobs.
 * A Wi-Fi shutdown can interrupt a blocked socket through cancel; the two
 * stage timeouts ensure a lost peer cannot retain the job forever even when
 * no explicit cancellation occurs. */
bool http_get_to_file_cancelable(const char * url, bool verify_tls, const char * dest_path,
                                  http_progress_cb_t progress_cb, void * progress_user_data,
                                  uint32_t connect_timeout_ms, uint32_t read_timeout_ms,
                                  http_cancel_token_t * cancel);

#endif /* HTTP_CLIENT_H */
