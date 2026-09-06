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

/* Sends a one-shot GET request and buffers the response body into heap memory.
 * *out_body is dynamically allocated and must be freed by caller.
 * Returns true if an HTTP response was received (inspect *out_status). */
bool http_get_to_buffer(const char * url, bool verify_tls, int * out_status, uint8_t ** out_body, size_t * out_body_size);
bool http_get_to_buffer_limited(const char * url, bool verify_tls, size_t max_body_size, int * out_status,
                                uint8_t ** out_body, size_t * out_body_size);

/* Sends a POST request with the given Content-Type and payload.
 * *out_body is dynamically allocated and must be freed by caller. */
bool http_post_to_buffer(const char * url, bool verify_tls, const char * content_type, const uint8_t * body,
                          size_t body_size, int * out_status, uint8_t ** out_body, size_t * out_body_size);
bool http_post_to_buffer_limited(const char * url, bool verify_tls, const char * content_type, const uint8_t * body,
                                  size_t body_size, size_t max_body_size, int * out_status, uint8_t ** out_body,
                                  size_t * out_body_size);

/* Progress callback for downloads: bytes received so far, and total bytes
 * from Content-Length (0 if unknown). Returning false cancels the download. */
typedef bool (*http_progress_cb_t)(uint64_t bytes_downloaded, uint64_t total_bytes, void * user_data);

/* Streams the response body directly to a local file at dest_path. */
bool http_get_to_file(const char * url, bool verify_tls, const char * dest_path,
                      http_progress_cb_t progress_cb, void * progress_user_data);

/* Like http_get_to_file(), but also populates out_content_type with the
 * response Content-Type (empty string if omitted). */
bool http_get_to_file_ex(const char * url, bool verify_tls, const char * dest_path,
                          http_progress_cb_t progress_cb, void * progress_user_data,
                          char * out_content_type, size_t out_content_type_size);

/* Like http_get_to_file(), with an explicit maximum body size limit. */
bool http_get_to_file_bounded(const char * url, bool verify_tls, const char * dest_path, size_t max_body_size,
                               http_progress_cb_t progress_cb, void * progress_user_data);

/* Cancellable download with explicit connection and read timeouts. */
bool http_get_to_file_cancelable(const char * url, bool verify_tls, const char * dest_path,
                                  http_progress_cb_t progress_cb, void * progress_user_data,
                                  uint32_t connect_timeout_ms, uint32_t read_timeout_ms,
                                  http_cancel_token_t * cancel);

#endif /* HTTP_CLIENT_H */
