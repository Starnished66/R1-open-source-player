#ifndef HTTP_CLIENT_H
#define HTTP_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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

#endif /* HTTP_CLIENT_H */
