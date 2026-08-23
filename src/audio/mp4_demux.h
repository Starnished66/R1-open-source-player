#ifndef MP4_DEMUX_H
#define MP4_DEMUX_H

#include <stdbool.h>
#include <stdint.h>

/* Minimal MP4/ISO-BMFF demuxer: enough to locate the single audio track's
 * codec config (from stsd) and per-sample file offsets/sizes (from
 * stsz/stco/co64/stsc), for feeding compressed samples to a codec decoder
 * one at a time. No video, no multi-track selection beyond "first audio
 * track found", no editing/fragmented-MP4 support. */

typedef struct mp4_demux mp4_demux_t;

mp4_demux_t * mp4_demux_open(const char * path);

/* Codec fourcc only ("alac"/"mp4a"). Opens and closes a demux; long files
 * stay compact so this is safe as an extension-dispatch peek. */
bool mp4_demux_peek_codec(const char * path, char out_fourcc[5]);

/* 4-character codec identifier from the sample description, e.g. "alac" or
 * "mp4a" (AAC). Not null-terminated by convention, so this returns exactly
 * 4 bytes; out_fourcc must have room for 5 (this null-terminates for
 * convenience). */
void mp4_demux_get_codec_fourcc(const mp4_demux_t * d, char out_fourcc[5]);

/* Raw bytes of the codec-specific config box contents (the ALAC magic
 * cookie, or the esds descriptor payload for AAC) -- handed to the actual
 * codec decoder's init function as-is. */
const uint8_t * mp4_demux_get_codec_config(const mp4_demux_t * d, uint32_t * out_size);

unsigned int mp4_demux_get_channels(const mp4_demux_t * d);
unsigned int mp4_demux_get_sample_rate(const mp4_demux_t * d);
uint32_t mp4_demux_get_sample_count(const mp4_demux_t * d);

/* PCM frames represented by one demuxed "sample" (i.e. one compressed
 * access unit) -- e.g. 4096 for typical ALAC, 1024 for AAC-LC. Assumes a
 * constant value throughout the track (true for the vast majority of real
 * files); the last sample may represent fewer. */
uint32_t mp4_demux_get_frames_per_sample(const mp4_demux_t * d);

/* Exact total PCM frame count for the whole track, summed from every stts
 * entry (not just the uniform frames_per_sample above) -- correctly
 * accounts for the last sample usually representing fewer frames than a
 * full access unit. Use this for duration/buffer-sizing; use
 * mp4_demux_get_frames_per_sample() only for per-call output buffer bounds. */
uint64_t mp4_demux_get_total_pcm_frame_count(const mp4_demux_t * d);

/* Reads sample_index's compressed bytes into buf (caller-provided,
 * buf_size bytes). Returns false if the index is out of range or the
 * sample doesn't fit in buf_size. */
bool mp4_demux_read_sample(mp4_demux_t * d, uint32_t sample_index, uint8_t * buf, uint32_t buf_size, uint32_t * out_size);

void mp4_demux_close(mp4_demux_t * d);

#endif /* MP4_DEMUX_H */
