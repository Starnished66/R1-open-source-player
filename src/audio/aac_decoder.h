#ifndef AAC_DECODER_H
#define AAC_DECODER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* AAC (AAC-LC/HE-AAC) decoding via FAAD2 (github.com/knik0/faad2),
 * statically linked -- vendored like dr_libs/tinyalsa. FAAD2 is GPLv2 (see
 * the project LICENSE and README for what that means for this project).
 *
 * We initially looked at dlopen()'ing the R1's own system libfdk-aac.so.2 at
 * runtime instead (avoiding shipping any AAC decoder code ourselves), but
 * that doesn't work: our target binary is statically linked against musl
 * (chosen to dodge the device's ancient glibc), and musl's dlopen() is a
 * hard no-op for any statically-linked binary -- confirmed empirically via
 * QEMU against the real device library, not just documentation.
 *
 * Two source framings are supported, since they need different decoder
 * init/framing but share everything else:
 *  - aac_open_file(): raw ADTS-framed .aac files (self-delimiting sync
 *    headers, no container).
 *  - aac_open_file_mp4(): AAC ("mp4a") stored inside an MP4/M4A container,
 *    via mp4_demux.h -- the frames themselves have no ADTS headers there,
 *    they're delimited by the container's sample table instead. */

typedef struct aac_decoder aac_decoder_t;
typedef size_t (*aac_stream_read_cb_t)(void * user_data, void * buf, size_t bytes_to_read);

aac_decoder_t * aac_open_file(const char * path);
aac_decoder_t * aac_open_file_mp4(const char * path);
/* Opens an unbounded ADTS-framed AAC source. The callback follows fread's
 * contract: return fewer than requested bytes only at true end/error. Live
 * streams have no duration and cannot seek. */
aac_decoder_t * aac_open_stream(aac_stream_read_cb_t read_cb, void * user_data);

unsigned int aac_get_channels(const aac_decoder_t * dec);
unsigned int aac_get_sample_rate(const aac_decoder_t * dec);
uint64_t aac_get_total_pcm_frame_count(const aac_decoder_t * dec);

uint64_t aac_read_pcm_frames_s16(aac_decoder_t * dec, uint64_t frames_to_read, int16_t * buffer_out);
bool aac_seek_to_pcm_frame(aac_decoder_t * dec, uint64_t frame_index);

void aac_close(aac_decoder_t * dec);

#endif /* AAC_DECODER_H */
