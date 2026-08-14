#ifndef OGG_DEMUX_H
#define OGG_DEMUX_H

#include <stdbool.h>
#include <stdint.h>

/* Minimal Ogg container demuxer, scoped to exactly what Ogg-Opus (RFC 7845)
 * playback needs: page/segment parsing, the OpusHead/OpusTags header
 * packets, and sequential audio-packet reads with a granule-position seek
 * index. Deliberately narrower than a general-purpose Ogg library (libogg):
 * a single file, a single logical audio bitstream, no chaining, no
 * multiplexed secondary streams -- this project hand-writes its container
 * demuxers (asf_demux.c/h for WMA, mp4_demux.c/h for ALAC/AAC, ape_demux.c/h
 * for APE) rather than vendoring general machinery for a comparatively
 * simple bitstream format. Page 0 of the file is assumed to be the
 * OpusHead page (required by RFC 7845 Section 3), so unlike asf_demux.c
 * (which scans for the first audio-typed Stream Properties Object) this
 * doesn't scan for the stream -- files that don't follow the spec's
 * required layout aren't supported. */

typedef struct ogg_demux ogg_demux_t;

ogg_demux_t * ogg_demux_open(const char * path);

/* OpusHead fields (RFC 7845 Section 5.1). Channel Mapping Family must be 0
 * (single-stream mono/stereo) -- multistream Opus files fail to open. */
unsigned int ogg_demux_get_opus_channels(const ogg_demux_t * d);
uint16_t     ogg_demux_get_opus_pre_skip(const ogg_demux_t * d);   /* samples @ 48kHz to discard from decode start */
int16_t      ogg_demux_get_opus_output_gain_q78(const ogg_demux_t * d);

/* Granule position of the last page in the file -- the total sample count
 * (at 48kHz, inclusive of pre-skip) per RFC 7845's EOS-page convention.
 * Informational, same duration-estimate caveat as asf_demux's Play
 * Duration field. */
uint64_t ogg_demux_get_total_granule(const ogg_demux_t * d);

/* OpusTags (RFC 7845 Section 5.2, same layout as a Vorbis comment header).
 * vendor_string may be NULL if the file's tags were missing/malformed --
 * parsing OpusTags is best-effort and never fails ogg_demux_open() on its
 * own (only OpusHead is required). Comment strings are raw "KEY=VALUE"
 * pairs, NUL-terminated for convenience but not guaranteed free of
 * embedded NULs -- out_len gives the real length. */
const char * ogg_demux_get_vendor_string(const ogg_demux_t * d);
unsigned int ogg_demux_get_comment_count(const ogg_demux_t * d);
const char * ogg_demux_get_comment(const ogg_demux_t * d, unsigned int index, uint32_t * out_len);

/* Reads the next Opus packet (audio data only -- OpusHead/OpusTags are
 * already consumed by ogg_demux_open()) into buf, transparently
 * reassembling packets that span multiple Ogg pages. Returns the packet's
 * size, or 0 at end of stream, on a corrupt page (CRC mismatch), or if the
 * packet doesn't fit in buf_size. */
uint32_t ogg_demux_read_packet(ogg_demux_t * d, uint8_t * buf, uint32_t buf_size);

/* Approximate seek: repositions the read cursor to the indexed page nearest
 * at-or-before target_granule and resets packet-reassembly state so the
 * next ogg_demux_read_packet() resumes cleanly at a fresh packet boundary
 * (the index only records packet-boundary pages, never a page mid-way
 * through a continued packet, so no partial-packet data is ever silently
 * dropped or duplicated). Returns the granule position actually landed on
 * (<= target_granule) -- the caller (opus_decoder.c) uses the difference to
 * know how many decoded PCM frames still need discarding to reach the
 * exact requested sample, the same two-layer coarse-seek-then-discard
 * strategy wma_decoder.c already uses for WMA. libopus's own decoder state
 * (SILK LSF/pitch history, CELT's MDCT overlap buffer) still needs an
 * explicit OPUS_RESET_STATE from the caller after this -- this function
 * only repositions the container-level read cursor. */
uint64_t ogg_demux_seek_near_granule(ogg_demux_t * d, uint64_t target_granule);

void ogg_demux_close(ogg_demux_t * d);

#endif /* OGG_DEMUX_H */
