#ifndef HTTP_STREAM_H
#define HTTP_STREAM_H

#include <stdbool.h>
#include <stddef.h>

/* Live HTTP(S) stream reader for internet radio. Connects, negotiates headers,
 * and continuously streams incoming audio data into an internal ring buffer
 * via a background pump thread. */

typedef struct http_stream http_stream_t;

/* Opens url, parses response headers, and starts the background pump thread.
 * Returns NULL on network/parse failure or non-2xx response. */
http_stream_t * http_stream_open(const char * url, bool verify_tls);

/* Blocking read: fills buf with up to n bytes. Returns bytes read, or 0 on
 * stream end or error. */
size_t http_stream_read(http_stream_t * s, void * buf, size_t n);

/* Response Content-Type, if the server sent one ("" if not). Valid
 * immediately after http_stream_open() returns non-NULL. */
const char * http_stream_content_type(http_stream_t * s);

/* Stops the background thread, unblocks any pending read, and frees the stream. */
void http_stream_close(http_stream_t * s);

#endif /* HTTP_STREAM_H */
