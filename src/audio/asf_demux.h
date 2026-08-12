#ifndef ASF_DEMUX_H
#define ASF_DEMUX_H

#include <stdbool.h>
#include <stdint.h>

/* Minimal ASF (Advanced Systems Format) demuxer: enough to locate the
 * single audio stream's codec config and hand back complete, reassembled
 * "media objects" (one compressed WMA frame each) for feeding to a WMA
 * decoder one at a time. ASF's container structure itself is from
 * Microsoft's own published specification (unlike WMA's actual codec
 * algorithm, which was never officially documented) -- no video, no
 * multi-stream selection beyond "first audio stream found", no seeking,
 * no error-correction-data handling beyond skipping it. */

typedef struct asf_demux asf_demux_t;

asf_demux_t * asf_demux_open(const char * path);

/* WAVE_FORMAT_* codec tag from the Stream Properties Object: 0x0160 = WMA1,
 * 0x0161 = WMA2 ("WMA Standard", by far the common case), 0x0162 = WMA Pro,
 * 0x0163 = WMA Lossless. */
uint16_t asf_demux_get_codec_id(const asf_demux_t * d);

unsigned int asf_demux_get_channels(const asf_demux_t * d);
unsigned int asf_demux_get_sample_rate(const asf_demux_t * d);
uint32_t asf_demux_get_byte_rate(const asf_demux_t * d);
uint16_t asf_demux_get_block_align(const asf_demux_t * d);
uint16_t asf_demux_get_bits_per_sample(const asf_demux_t * d);

/* WMA-specific "codec data" trailing the WAVEFORMATEX-style stream header --
 * the "encode options" bitfield the decoder needs to know which optional
 * coding tools (mid/side stereo, etc.) this file's encoder turned on.
 * Points into the demuxer's own buffer; valid until asf_demux_close(). */
const uint8_t * asf_demux_get_codec_data(const asf_demux_t * d, uint32_t * out_size);

/* Play Duration from the File Properties Object, in 100-nanosecond units
 * (an author-supplied estimate, not exact -- fine for a seek-bar/duration
 * display but not sample-accurate). */
uint64_t asf_demux_get_duration_100ns(const asf_demux_t * d);

uint64_t asf_demux_get_packet_count(const asf_demux_t * d);

/* Approximate seek: jumps to the given packet index (clamped to the
 * declared packet count) and resets frame-reassembly state. WMA frames
 * within a packet still need the decoder's own state (bit reservoir,
 * block-length history) reset by the caller after this -- this only
 * repositions the container-level read cursor. */
void asf_demux_seek_to_packet(asf_demux_t * d, uint64_t packet_index);

/* Reads the next complete media object (one compressed WMA frame, expected
 * to be exactly asf_demux_get_block_align() bytes for audio) into buf.
 * Transparently reassembles objects fragmented across multiple ASF packets
 * and skips payloads belonging to any other stream. Returns false at end
 * of stream or if the object doesn't fit in buf_size. */
bool asf_demux_read_frame(asf_demux_t * d, uint8_t * buf, uint32_t buf_size, uint32_t * out_size);

void asf_demux_close(asf_demux_t * d);

#endif /* ASF_DEMUX_H */
