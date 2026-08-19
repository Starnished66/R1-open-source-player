#ifndef HTTP_STREAM_H
#define HTTP_STREAM_H

#include <stdbool.h>
#include <stddef.h>

/* Live HTTP(S) stream reader (internet radio) -- unlike http_client.c's
 * http_get_*() family, which always treats a response as bounded (either a
 * known Content-Length or a chunked stream terminated by a final zero-size
 * chunk), this never assumes the body ends. A live stream typically sends
 * neither header at all and just keeps the connection open, pushing bytes
 * forever -- http_client.c's read_body() would treat that shape as an
 * already-finished, zero-byte body and return immediately; this module
 * exists specifically to handle it instead.
 *
 * http_stream_open() does the (bounded, quick) connect + request + header
 * parse synchronously, then spawns a dedicated thread that continuously
 * reads the body into an internal ring buffer -- decoupling network jitter
 * from whatever pace the caller reads at, since the caller (audio.c's
 * playback thread) also has to keep the ALSA output fed in real time and
 * can't tolerate a network read blocking it directly. */

typedef struct http_stream http_stream_t;

/* Opens url and starts the background pump thread. Returns NULL if the URL
 * can't be parsed, the connection/handshake fails, or the server's response
 * status isn't 2xx (after following at most five absolute HTTP(S)
 * redirects) -- everything after that (the connection dropping
 * mid-stream) is instead reported through http_stream_read()'s return
 * value, not here. verify_tls has the same meaning as http_client.c's own
 * (false skips certificate verification -- never the default). */
http_stream_t * http_stream_open(const char * url, bool verify_tls);

/* Blocking read: fills buf with exactly n bytes, waiting on the network as
 * needed, unless the stream ends (connection closed/error) or is closed
 * from another thread first -- in which case it returns whatever was
 * actually read before that happened (possibly 0). Same short-read-means-
 * done contract plain fread() has, which is what a dr_mp3-style raw onRead
 * callback is expected to honor -- dr_mp3 itself relies on getting exactly
 * the bytes it asked for except at genuine end of stream. */
size_t http_stream_read(http_stream_t * s, void * buf, size_t n);

/* Response Content-Type, if the server sent one ("" if not). Valid
 * immediately after http_stream_open() returns non-NULL. */
const char * http_stream_content_type(http_stream_t * s);

/* Stops the background thread and releases everything, including the
 * underlying connection. Blocks until the thread has actually exited (it
 * may be mid-read on the socket; this forces that read to unblock rather
 * than waiting for the far end to send more data or close on its own). */
void http_stream_close(http_stream_t * s);

#endif /* HTTP_STREAM_H */
