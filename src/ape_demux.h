#ifndef APE_DEMUX_H
#define APE_DEMUX_H

#include <stdbool.h>
#include <stdint.h>

/* Minimal Monkey's Audio (.ape) container parser: reads the "new" (fileversion
 * >= 3980) descriptor+header+seektable layout only. That format has been
 * unchanged since 2007 (Monkey's Audio has kept the on-disk fileversion at
 * 3990 across all its "4.x"/"5.x"/"6.x"/"7.x" encoder releases since then),
 * so this covers the overwhelming majority of real .ape files. Pre-2007
 * files (fileversion < 3980, old header layout, pre-3810 bit-packed skip
 * table) and any leading junk/ID3v2 before the "MAC " tag are not handled.
 *
 * Modeled after ffmpeg's libavformat/ape.c (LGPL 2.1+), simplified to this
 * narrower scope -- see ape_decoder.h for the matching decode-side scope
 * note. */

typedef struct ape_demux ape_demux_t;

ape_demux_t * ape_demux_open(const char * path);

unsigned int ape_demux_get_channels(const ape_demux_t * d);
unsigned int ape_demux_get_sample_rate(const ape_demux_t * d);
unsigned int ape_demux_get_bits_per_sample(const ape_demux_t * d);
int ape_demux_get_fileversion(const ape_demux_t * d);
int ape_demux_get_compression_level(const ape_demux_t * d);
uint64_t ape_demux_get_total_samples(const ape_demux_t * d);

uint32_t ape_demux_get_frame_count(const ape_demux_t * d);
uint32_t ape_demux_get_blocks_per_frame(const ape_demux_t * d);
uint32_t ape_demux_get_final_frame_blocks(const ape_demux_t * d);

/* Reads frame_index's compressed bytes (already including its 0-3 byte skip
 * padding at the front, matching the on-disk layout) into buf. out_size is
 * always a multiple of 4 (the format requires 32-bit-word-aligned frame
 * data). Returns false if the index is out of range or doesn't fit buf_size. */
bool ape_demux_read_frame(ape_demux_t * d, uint32_t frame_index, uint8_t * buf, uint32_t buf_size, uint32_t * out_size);

/* Number of bytes at the front of the frame's data (after the above read)
 * that must be skipped before range-decoding starts -- word-alignment
 * padding introduced by where the frame happens to fall in the file. */
uint32_t ape_demux_get_frame_skip(const ape_demux_t * d, uint32_t frame_index);

void ape_demux_close(ape_demux_t * d);

#endif /* APE_DEMUX_H */
