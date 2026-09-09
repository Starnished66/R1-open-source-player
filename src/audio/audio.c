#include "audio.h"

/* Default (4096 bytes) is dr_flac's own general-purpose tuning -- its header
 * comment notes "diminishing returns after about 4KB" for a typical fast
 * onRead() implementation, but local playback here reads from an SD card,
 * where each refill is a real synchronous read on the audio thread, not a
 * memory copy. A 24-bit/96kHz FLAC can run 2.5-4Mbps compressed vs ~1Mbps
 * for 16-bit/44.1kHz, so the same 4KB cache holds proportionally less
 * playback time and empties (forcing a refill) proportionally more often
 * for exactly the high-resolution files a real user reported stutter on.
 * Quadrupling to 16KB trades a modest per-open-decoder RAM cost for fewer
 * mid-playback SD reads on the highest-bitrate files. Must be a multiple of
 * 8 per dr_flac.h's own requirement. */
#define DR_FLAC_BUFFER_SIZE 16384
#define DR_FLAC_IMPLEMENTATION
#include "dr_flac.h"
#define DR_MP3_IMPLEMENTATION
#include "dr_mp3.h"
#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"
#include "aiff_decoder.h"
#include "dsd_decoder.h"
#include "aac_decoder.h"
#include "alac_decoder.h"
#include "mp4_demux.h"
#include "ape_decoder.h"
#include "wma_decoder.h"
#include "opus_decoder.h"
#include "ogg_demux.h"
#include "vorbis_decoder.h"
#include "peq.h"
#include "http_stream.h"
#include "remote_track.h"

#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <unistd.h>
#include <limits.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "debug_log.h"
#include "audio_helpers.h"

/* Persistent cache for MP3 seek-point tables (mp3_seek_cache_load/_save
 * below) -- same on-SD, hidden-directory convention already used by
 * metadata_db.c's METADATA_DB_DIR and albumart.c's ALBUMART_DIR. */
#ifdef HOST_BUILD
  #define OPEN_HIBY_DIR "./.open_hiby_player"
#else
  #define OPEN_HIBY_DIR "/data/mnt/sd_0/.open_hiby_player"
#endif
#define MP3_SEEK_CACHE_DIR OPEN_HIBY_DIR "/mp3seek"

#ifdef HOST_BUILD
  #include <SDL2/SDL.h>
#else
  #include "audio_output.h"
#endif

#define NORMAL_CHUNK_FRAMES 4096
#define LOW_POWER_CHUNK_FRAMES 8192
#define MAX_CHUNK_FRAMES LOW_POWER_CHUNK_FRAMES

/* --- Decoder dispatch: dr_flac/dr_mp3/dr_wav all expose the same shape of
 * API (open, channels/sampleRate, read_pcm_frames_s16, seek_to_pcm_frame,
 * close) but aren't literally the same type, so this is just a small tagged
 * union picking the right calls rather than a new abstraction layer. */

typedef enum {
    DECODER_FLAC,
    DECODER_MP3,
    DECODER_WAV,
    DECODER_AIFF,
    DECODER_DSD,
    DECODER_AAC,
    DECODER_ALAC,
    DECODER_APE,
    DECODER_WMA,
    DECODER_OPUS,
    DECODER_VORBIS
} decoder_type_t;

typedef struct {
    decoder_type_t type;
    union {
        drflac * flac;
        drmp3 * mp3;
        drwav * wav;
        aiff_decoder_t * aiff;
        dsd_decoder_t * dsd;
        aac_decoder_t * aac;
        alac_decoder_t * alac;
        ape_decoder_t * ape;
        wma_decoder_t * wma;
        opus_decoder_wrap_t * opus;
        vorbis_decoder_wrap_t * vorbis;
    } as;
    unsigned int channels;
    unsigned int sample_rate;
    unsigned int source_sample_rate;
    unsigned int source_bit_depth;
    unsigned int bitrate_kbps;
    uint64_t total_frames;

    /* Local MP3 only. dr_mp3 never owns a bound seek table, so this tagged
     * decoder owns and frees it alongside the drmp3 instance. */
    drmp3_seek_point * mp3_seek_points;
    drmp3_uint32 mp3_seek_point_count;
    bool mp3_seek_index_attempted;

    /* Non-NULL only for a live network stream opened via a URL (see
     * is_stream_url() below) -- decoder_close() must also tear this down,
     * and decoder_seek() must refuse to seek while it's set (there's
     * nothing to seek back to on a live source). type is still DECODER_MP3
     * in that case: dr_mp3's decode/read calls don't care whether the
     * drmp3* was opened against a local file or a read callback, only
     * decoder_open()/decoder_close() need to know the difference. */
    http_stream_t * net_stream;
} decoder_t;

/* A stream URL is never dispatched by file extension (see decoder_open()
 * below) -- most internet radio URLs don't end in anything recognizable
 * anyway (an opaque mount point, or a query string). */
static bool is_stream_url(const char * path) {
    return strncasecmp(path, "http://", 7) == 0 || strncasecmp(path, "https://", 8) == 0;
}

/* A trailing "#.<ext>" on a stream URL is a local-only format hint (never
 * sent to the server -- http_conn_parse_url() strips it, see that
 * function's own comment): the '#' can't legally appear in a Subsonic/
 * plugin-built URL's own path or query otherwise (this app always
 * percent-encodes it via url_encode()), so a literal one is unambiguously
 * ours. Absent a hint, MP3 stays the default (unchanged from before this
 * hint existed, so plain internet-radio URLs work exactly as before). */
static const char * stream_format_hint(const char * url) {
    const char * frag = strrchr(url, '#');
    return frag ? frag + 1 : NULL;
}

static size_t stream_read_cb(void * user_data, void * buf, size_t bytes_to_read) {
    return http_stream_read((http_stream_t *) user_data, buf, bytes_to_read);
}

/* FLAC's onSeek is NOT optional (unlike dr_mp3's) -- drflac_open() itself
 * requires a non-NULL callback. Passing onTell=NULL below skips the only
 * place dr_flac would ever ask for an absolute/backward seek (the
 * SEEK_END-then-SEEK_SET-back file-size sanity check in
 * drflac__read_and_decode_metadata(), gated on "onTell != NULL && onSeek !=
 * NULL" -- confirmed by reading dr_flac.h directly, not assumed), so this
 * only ever needs to handle a DRFLAC_SEEK_CUR forward skip (used to skip
 * past metadata blocks this app doesn't care about -- PADDING, SEEKTABLE,
 * CUESHEET, PICTURE, VORBIS_COMMENT since onMeta is also NULL here). A live
 * stream can't seek backward at all, so anything else fails cleanly rather
 * than silently doing the wrong thing. */
static drflac_bool32 flac_stream_seek_cb(void * user_data, int offset, drflac_seek_origin origin) {
    if (origin != DRFLAC_SEEK_CUR || offset < 0) return DRFLAC_FALSE;
    uint8_t discard[1024];
    int remaining = offset;
    while (remaining > 0) {
        size_t take = (size_t) remaining < sizeof(discard) ? (size_t) remaining : sizeof(discard);
        if (http_stream_read((http_stream_t *) user_data, discard, take) != take) return DRFLAC_FALSE;
        remaining -= (int) take;
    }
    return DRFLAC_TRUE;
}

static bool decoder_open(decoder_t * dec, const char * path) {
    memset(dec, 0, sizeof(*dec));

    /* A remote-provider track (plugin.play_remote(), see remote_track.h):
     * path is the stable "remote://<provider>/<track_id>" synthetic key
     * used for Favorites/History/resume, never a fetchable URL itself --
     * the real, possibly time-limited fetch URL and verify_tls live in the
     * looked-up descriptor. Its codec is declared up front by the plugin
     * (already known from the catalog API, no server round trip needed to
     * find out), so this skips the extension/fragment-hint/Content-Type
     * sniffing below entirely -- that sniffing stays exactly as it was for
     * radio/Subsonic streams, which don't have a declared codec. */
    remote_track_meta_t remote_meta;
    bool remote = remote_track_path_is_remote(path) && remote_track_meta_copy_for_path(path, &remote_meta);
    if (remote_track_path_is_remote(path) && !remote) return false; /* stale/replaced queue entry */

    if (remote || is_stream_url(path)) {
        const char * hint = remote ? NULL : stream_format_hint(path);
        bool is_flac = remote ? (strcasecmp(remote_meta.codec, "flac") == 0)
                              : (hint && strcasecmp(hint, ".flac") == 0);
        bool is_aac = remote ? (strcasecmp(remote_meta.codec, "aac") == 0)
                             : (hint && (strcasecmp(hint, ".aac") == 0 || strcasecmp(hint, ".aacp") == 0));

        /* Live network stream -- MP3/FLAC use their callback-based decoder
         * APIs, while ADTS AAC uses aac_open_stream()'s incremental framing
         * path. FLAC's callback seek constraints are documented above;
         * dr_mp3 tolerates NULL onSeek/onTell, and live AAC never seeks or
         * prescans. Other formats still require a finite file/container.
         * Absent a recognized hint or AAC Content-Type, MP3 remains the
         * default for compatibility with ordinary internet-radio URLs. */
        dec->net_stream = http_stream_open(remote ? remote_meta.stream_url : path, remote ? remote_meta.verify_tls : true);
        if (!dec->net_stream) return false;

        if (!remote) {
            const char * content_type = http_stream_content_type(dec->net_stream);
            if (strncasecmp(content_type, "audio/aac", 9) == 0) is_aac = true;
        }

        if (is_aac) {
            dec->type = DECODER_AAC;
            dec->as.aac = aac_open_stream(stream_read_cb, dec->net_stream);
            if (!dec->as.aac) {
                http_stream_close(dec->net_stream);
                dec->net_stream = NULL;
                return false;
            }
            dec->channels = aac_get_channels(dec->as.aac);
            dec->sample_rate = aac_get_sample_rate(dec->as.aac);
            dec->source_sample_rate = dec->sample_rate;
            dec->source_bit_depth = remote ? remote_meta.bit_depth : 0;
            dec->bitrate_kbps = remote ? remote_meta.bitrate_kbps : 0;
            /* A remote track declares its own duration up front (the
             * plugin's catalog metadata) -- unlike plain internet radio,
             * there's no need to leave this at the unknown-duration 0
             * below; a plain stream URL (remote == false) still has no
             * such source and keeps today's behavior exactly. */
            dec->total_frames = (remote && remote_meta.duration_ms > 0)
                                     ? (uint64_t) ((double) remote_meta.duration_ms / 1000.0 * dec->sample_rate)
                                     : 0;
            return true;
        }

        if (is_flac) {
            dec->type = DECODER_FLAC;
            dec->as.flac = drflac_open(stream_read_cb, flac_stream_seek_cb, NULL, dec->net_stream, NULL);
            if (!dec->as.flac) {
                http_stream_close(dec->net_stream);
                dec->net_stream = NULL;
                return false;
            }
            dec->channels = dec->as.flac->channels;
            dec->sample_rate = dec->as.flac->sampleRate;
            dec->source_sample_rate = dec->sample_rate;
            dec->source_bit_depth = dec->as.flac->bitsPerSample;
            dec->bitrate_kbps = remote ? remote_meta.bitrate_kbps : 0;
            /* Unlike MP3, this is a REAL, authoritative count straight from
             * the STREAMINFO metadata block (near the top of the file, not
             * a full-stream prescan) -- safe to use for real, matching
             * local FLAC playback below. Gives a Subsonic FLAC stream a
             * correct duration/progress bar for free, unlike MP3 streams
             * (which have no equivalent authoritative source without a
             * full prescan, so those stay at the unknown-duration 0 below). */
            dec->total_frames = dec->as.flac->totalPCMFrameCount;
            return true;
        }

        dec->type = DECODER_MP3;
        dec->as.mp3 = malloc(sizeof(drmp3));
        if (!dec->as.mp3 || !drmp3_init(dec->as.mp3, stream_read_cb, NULL, NULL, NULL, dec->net_stream, NULL)) {
            free(dec->as.mp3);
            dec->as.mp3 = NULL;
            http_stream_close(dec->net_stream);
            dec->net_stream = NULL;
            return false;
        }
        dec->channels = dec->as.mp3->channels;
        dec->sample_rate = dec->as.mp3->sampleRate;
        dec->source_sample_rate = dec->sample_rate;
        dec->source_bit_depth = remote ? remote_meta.bit_depth : 0;
        dec->bitrate_kbps = remote ? remote_meta.bitrate_kbps : 0;
        /* See the AAC branch's own comment just above -- a remote track's
         * declared duration_ms substitutes for the prescan this never does
         * on a live stream; a plain MP3 stream URL (remote == false, e.g.
         * internet radio or a Subsonic stream) has no such source and
         * keeps today's unknown-duration-until-EOF behavior exactly. */
        dec->total_frames = (remote && remote_meta.duration_ms > 0)
                                 ? (uint64_t) ((double) remote_meta.duration_ms / 1000.0 * dec->sample_rate)
                                 : 0;
        return true;
    }

    const char * ext = strrchr(path, '.');
    if (!ext) return false;

    if (strcasecmp(ext, ".flac") == 0) {
        dec->type = DECODER_FLAC;
        dec->as.flac = drflac_open_file(path, NULL);
        if (!dec->as.flac) return false;
        dec->channels = dec->as.flac->channels;
        dec->sample_rate = dec->as.flac->sampleRate;
        dec->source_sample_rate = dec->sample_rate;
        dec->source_bit_depth = dec->as.flac->bitsPerSample;
        dec->total_frames = dec->as.flac->totalPCMFrameCount;
        return true;
    }

    if (strcasecmp(ext, ".mp3") == 0) {
        dec->type = DECODER_MP3;
        dec->as.mp3 = malloc(sizeof(drmp3));
        if (!dec->as.mp3 || !drmp3_init_file(dec->as.mp3, path, NULL)) {
            free(dec->as.mp3);
            dec->as.mp3 = NULL;
            return false;
        }
        dec->channels = dec->as.mp3->channels;
        dec->sample_rate = dec->as.mp3->sampleRate;
        dec->source_sample_rate = dec->sample_rate;
        dec->total_frames = drmp3_get_pcm_frame_count(dec->as.mp3);
        return true;
    }

    if (strcasecmp(ext, ".wav") == 0) {
        dec->type = DECODER_WAV;
        dec->as.wav = malloc(sizeof(drwav));
        if (!dec->as.wav || !drwav_init_file(dec->as.wav, path, NULL)) {
            free(dec->as.wav);
            dec->as.wav = NULL;
            return false;
        }
        dec->channels = dec->as.wav->channels;
        dec->sample_rate = dec->as.wav->sampleRate;
        dec->source_sample_rate = dec->sample_rate;
        dec->source_bit_depth = dec->as.wav->bitsPerSample;
        dec->total_frames = dec->as.wav->totalPCMFrameCount;
        return true;
    }

    if (strcasecmp(ext, ".aiff") == 0 || strcasecmp(ext, ".aif") == 0) {
        dec->type = DECODER_AIFF;
        dec->as.aiff = aiff_open_file(path);
        if (!dec->as.aiff) return false;
        dec->channels = aiff_get_channels(dec->as.aiff);
        dec->sample_rate = aiff_get_sample_rate(dec->as.aiff);
        dec->source_sample_rate = dec->sample_rate;
        dec->source_bit_depth = aiff_get_bits_per_sample(dec->as.aiff);
        dec->total_frames = aiff_get_total_pcm_frame_count(dec->as.aiff);
        return true;
    }

    if (strcasecmp(ext, ".dsf") == 0 || strcasecmp(ext, ".dff") == 0) {
        dec->type = DECODER_DSD;
        dec->as.dsd = dsd_open_file(path);
        if (!dec->as.dsd) return false;
        dec->channels = dsd_get_channels(dec->as.dsd);
        dec->sample_rate = dsd_get_pcm_sample_rate(dec->as.dsd); /* decimated PCM rate, not the raw DSD rate */
        dec->source_sample_rate = dsd_get_source_sample_rate(dec->as.dsd);
        dec->source_bit_depth = 1;
        dec->total_frames = dsd_get_total_pcm_frame_count(dec->as.dsd);
        return true;
    }

    if (strcasecmp(ext, ".aac") == 0) {
        dec->type = DECODER_AAC;
        dec->as.aac = aac_open_file(path);
        if (!dec->as.aac) return false;
        dec->channels = aac_get_channels(dec->as.aac);
        dec->sample_rate = aac_get_sample_rate(dec->as.aac);
        dec->source_sample_rate = dec->sample_rate;
        dec->total_frames = aac_get_total_pcm_frame_count(dec->as.aac);
        return true;
    }

    if (strcasecmp(ext, ".m4a") == 0 || strcasecmp(ext, ".m4b") == 0) {
        /* .m4a/.m4b is a container, not a codec -- peek which one is actually
         * inside (ALAC or AAC) before picking a decoder. The real decoders
         * each open their own mp4_demux_t; this one is just for the peek. */
        char fourcc[5];
        if (!mp4_demux_peek_codec(path, fourcc)) return false;

        if (strcmp(fourcc, "alac") == 0) {
            dec->type = DECODER_ALAC;
            dec->as.alac = alac_open_file(path);
            if (!dec->as.alac) return false;
            dec->channels = alac_get_channels(dec->as.alac);
            dec->sample_rate = alac_get_sample_rate(dec->as.alac);
            dec->source_sample_rate = dec->sample_rate;
            dec->source_bit_depth = alac_get_bit_depth(dec->as.alac);
            dec->total_frames = alac_get_total_pcm_frame_count(dec->as.alac);
            return true;
        }
        if (strcmp(fourcc, "mp4a") == 0) {
            dec->type = DECODER_AAC;
            dec->as.aac = aac_open_file_mp4(path);
            if (!dec->as.aac) return false;
            dec->channels = aac_get_channels(dec->as.aac);
            dec->sample_rate = aac_get_sample_rate(dec->as.aac);
            dec->source_sample_rate = dec->sample_rate;
            dec->total_frames = aac_get_total_pcm_frame_count(dec->as.aac);
            return true;
        }
        return false;
    }

    if (strcasecmp(ext, ".ape") == 0) {
        dec->type = DECODER_APE;
        dec->as.ape = ape_open_file(path);
        if (!dec->as.ape) return false;
        dec->channels = ape_get_channels(dec->as.ape);
        dec->sample_rate = ape_get_sample_rate(dec->as.ape);
        dec->source_sample_rate = dec->sample_rate;
        dec->source_bit_depth = ape_get_bits_per_sample(dec->as.ape);
        dec->total_frames = ape_get_total_pcm_frame_count(dec->as.ape);
        return true;
    }

    if (strcasecmp(ext, ".wma") == 0) {
        dec->type = DECODER_WMA;
        dec->as.wma = wma_open_file(path);
        if (!dec->as.wma) return false;
        dec->channels = wma_get_channels(dec->as.wma);
        dec->sample_rate = wma_get_sample_rate(dec->as.wma);
        dec->source_sample_rate = dec->sample_rate;
        dec->total_frames = wma_get_total_pcm_frame_count(dec->as.wma);
        return true;
    }

    if (strcasecmp(ext, ".opus") == 0) {
        dec->type = DECODER_OPUS;
        dec->as.opus = opus_open_file(path);
        if (!dec->as.opus) return false;
        dec->channels = opus_get_channels(dec->as.opus);
        dec->sample_rate = opus_get_sample_rate(dec->as.opus);
        dec->source_sample_rate = dec->sample_rate;
        dec->total_frames = opus_get_total_pcm_frame_count(dec->as.opus);
        return true;
    }

    if (strcasecmp(ext, ".ogg") == 0) {
        ogg_codec_t codec = ogg_detect_codec(path);
        if (codec == OGG_CODEC_OPUS) {
            dec->type = DECODER_OPUS;
            dec->as.opus = opus_open_file(path);
            if (!dec->as.opus) return false;
            dec->channels = opus_get_channels(dec->as.opus);
            dec->sample_rate = opus_get_sample_rate(dec->as.opus);
            dec->source_sample_rate = dec->sample_rate;
            dec->total_frames = opus_get_total_pcm_frame_count(dec->as.opus);
            return true;
        }
        if (codec == OGG_CODEC_VORBIS) {
            dec->type = DECODER_VORBIS;
            dec->as.vorbis = vorbis_open_file(path);
            if (!dec->as.vorbis) return false;
            dec->channels = vorbis_get_channels(dec->as.vorbis);
            dec->sample_rate = vorbis_get_sample_rate(dec->as.vorbis);
            dec->source_sample_rate = dec->sample_rate;
            dec->total_frames = vorbis_get_total_pcm_frame_count(dec->as.vorbis);
            return true;
        }
        return false;
    }

    return false;
}

static decoder_read_result_t decoder_read_s16(decoder_t * dec, uint64_t frames, int16_t * buf) {
    decoder_read_result_t res = { .frames = 0, .status = DECODER_READ_OK };
    if (!dec || !buf) {
        res.status = DECODER_READ_FATAL_ERROR;
        return res;
    }

    switch (dec->type) {
        case DECODER_FLAC: {
            uint64_t r = drflac_read_pcm_frames_s16(dec->as.flac, frames, buf);
            res.frames = r;
            res.status = (r > 0) ? DECODER_READ_OK : DECODER_READ_EOF;
            return res;
        }
        case DECODER_MP3: {
            uint64_t r = drmp3_read_pcm_frames_s16(dec->as.mp3, frames, buf);
            res.frames = r;
            res.status = (r > 0) ? DECODER_READ_OK : DECODER_READ_EOF;
            return res;
        }
        case DECODER_WAV: {
            uint64_t r = drwav_read_pcm_frames_s16(dec->as.wav, frames, buf);
            res.frames = r;
            res.status = (r > 0) ? DECODER_READ_OK : DECODER_READ_EOF;
            return res;
        }
        case DECODER_AIFF:
            return aiff_read_pcm_frames_s16(dec->as.aiff, frames, buf);
        case DECODER_DSD:
            return dsd_read_pcm_frames_s16(dec->as.dsd, frames, buf);
        case DECODER_AAC:
            return aac_read_pcm_frames_s16(dec->as.aac, frames, buf);
        case DECODER_ALAC:
            return alac_read_pcm_frames_s16(dec->as.alac, frames, buf);
        case DECODER_APE:
            return ape_read_pcm_frames_s16(dec->as.ape, frames, buf);
        case DECODER_WMA:
            return wma_read_pcm_frames_s16(dec->as.wma, frames, buf);
        case DECODER_OPUS:
            return opus_read_pcm_frames_s16(dec->as.opus, frames, buf);
        case DECODER_VORBIS:
            return vorbis_read_pcm_frames_s16(dec->as.vorbis, frames, buf);
    }
    res.status = DECODER_READ_FATAL_ERROR;
    return res;
}

#ifndef HOST_BUILD
static decoder_read_result_t decoder_read_s32(decoder_t * dec, uint64_t frames, int32_t * buf) {
    decoder_read_result_t res = { .frames = 0, .status = DECODER_READ_OK };
    if (!dec || !buf) {
        res.status = DECODER_READ_FATAL_ERROR;
        return res;
    }

    switch (dec->type) {
        case DECODER_FLAC: {
            uint64_t r = drflac_read_pcm_frames_s32(dec->as.flac, frames, buf);
            res.frames = r;
            res.status = (r > 0) ? DECODER_READ_OK : DECODER_READ_EOF;
            /* FLAC is left-justified (MSB at bit 31). Arithmetic right shift by 8
             * on signed int32_t so the sign bit replicates correctly, placing the
             * 24-bit sample into the low 24 bits for ALSA S24_LE. */
            size_t sample_count = (size_t) r * dec->channels;
            for (size_t i = 0; i < sample_count; i++) {
                buf[i] >>= 8;
            }
            return res;
        }
        case DECODER_WAV: {
            uint64_t r = drwav_read_pcm_frames_s32(dec->as.wav, frames, buf);
            res.frames = r;
            res.status = (r > 0) ? DECODER_READ_OK : DECODER_READ_EOF;
            /* WAV is left-justified (MSB at bit 31). Arithmetic right shift by 8
             * on signed int32_t so the sign bit replicates correctly, placing the
             * 24-bit sample into the low 24 bits for ALSA S24_LE. */
            size_t sample_count = (size_t) r * dec->channels;
            for (size_t i = 0; i < sample_count; i++) {
                buf[i] >>= 8;
            }
            return res;
        }
        case DECODER_ALAC:
            /* ALAC decodes internally to right-justified true-magnitude int32 for
             * 24-bit sources: DO NOT shift; already in S24_LE layout. */
            return alac_read_pcm_frames_s32(dec->as.alac, frames, buf);
        case DECODER_APE:
            /* APE decodes internally to right-justified true-magnitude int32 for
             * 24-bit sources: DO NOT shift; already in S24_LE layout. */
            return ape_read_pcm_frames_s32(dec->as.ape, frames, buf);
        case DECODER_AIFF:
            /* AIFF decodes internally to right-justified sign-extended int32 for
             * 24-bit sources: DO NOT shift; already in S24_LE layout. */
            return aiff_read_pcm_frames_s32(dec->as.aiff, frames, buf);
        default:
            /* Only the five lossless formats above have a wide read path.
             * Any other decoder reaching here is a caller bug. */
            res.status = DECODER_READ_FATAL_ERROR;
            return res;
    }
}
#endif

/* Every local (non-stream) MP3 is a candidate for a seek-point index
 * (see request_mp3_seek_index_locked()'s own `desired < 2` bailout for the
 * self-consistent floor: a file too short to place at least 2 points at
 * MP3_SEEK_INDEX_INTERVAL_SECONDS spacing just never gets an index,
 * brute-force seeking being cheap enough there anyway). Building always
 * happens off the playback thread (see mp3_index_worker()) so it can never
 * block audio output, including at a crossfade/gapless boundary. */
#define MP3_SEEK_INDEX_INTERVAL_SECONDS 2u
/* 24-byte drmp3_seek_point entries; caps a single track's table at ~196 KiB
 * (a ~4-hour recording at the interval above lands just under this, ~169
 * KiB) -- small next to this device's RAM budget, and this codebase's other
 * long-lived caches (album art, metadata DB) are already sized in that same
 * ballpark. */
#define MP3_SEEK_INDEX_MAX_POINTS 8192u

static bool mp3_needs_seek_index(const decoder_t * dec) {
    return dec && !dec->net_stream && dec->type == DECODER_MP3 && dec->sample_rate > 0 &&
           dec->total_frames >= (uint64_t) dec->sample_rate * MP3_SEEK_INDEX_INTERVAL_SECONDS * 2;
}

static bool decoder_seek(decoder_t * dec, uint64_t frame) {
    if (dec->net_stream) return false; /* live stream -- cannot seek */
    switch (dec->type) {
        case DECODER_FLAC:   return drflac_seek_to_pcm_frame(dec->as.flac, frame) != 0;
        case DECODER_MP3:
            /* Refuse only while an index attempt is still outstanding, so
             * the caller's defer-and-wait path (audio_thread_func's own
             * hold_for_mp3_seek) gets a chance to use the fast table instead
             * of racing it. Once mp3_seek_index_attempted is true (success
             * or a permanent failure), always proceed: drmp3_seek_to_pcm_
             * frame() uses the bound table if there is one, or falls back to
             * its own brute-force seek as a last resort otherwise -- never
             * silently drops the seek. */
            if (frame > 0 && mp3_needs_seek_index(dec) && !dec->mp3_seek_points &&
                !dec->mp3_seek_index_attempted) return false;
            return drmp3_seek_to_pcm_frame(dec->as.mp3, frame) != 0;
        case DECODER_WAV:    return drwav_seek_to_pcm_frame(dec->as.wav, frame) != 0;
        case DECODER_AIFF:   return aiff_seek_to_pcm_frame(dec->as.aiff, frame);
        case DECODER_DSD:    return dsd_seek_to_pcm_frame(dec->as.dsd, frame);
        case DECODER_AAC:    return aac_seek_to_pcm_frame(dec->as.aac, frame);
        case DECODER_ALAC:   return alac_seek_to_pcm_frame(dec->as.alac, frame);
        case DECODER_APE:    return ape_seek_to_pcm_frame(dec->as.ape, frame);
        case DECODER_WMA:    return wma_seek_to_pcm_frame(dec->as.wma, frame);
        case DECODER_OPUS:   return opus_seek_to_pcm_frame(dec->as.opus, frame);
        case DECODER_VORBIS: return vorbis_seek_to_pcm_frame(dec->as.vorbis, frame);
    }
    return false;
}

static void decoder_close(decoder_t * dec) {
    switch (dec->type) {
        case DECODER_FLAC:
            if (dec->as.flac) drflac_close(dec->as.flac);
            if (dec->net_stream) { http_stream_close(dec->net_stream); dec->net_stream = NULL; }
            break;
        case DECODER_MP3:
            if (dec->as.mp3) {
                drmp3_bind_seek_table(dec->as.mp3, 0, NULL);
                drmp3_uninit(dec->as.mp3);
                free(dec->as.mp3);
            }
            free(dec->mp3_seek_points);
            dec->mp3_seek_points = NULL;
            dec->mp3_seek_point_count = 0;
            if (dec->net_stream) { http_stream_close(dec->net_stream); dec->net_stream = NULL; }
            break;
        case DECODER_WAV:
            if (dec->as.wav) { drwav_uninit(dec->as.wav); free(dec->as.wav); }
            break;
        case DECODER_AIFF:
            if (dec->as.aiff) aiff_close(dec->as.aiff);
            break;
        case DECODER_DSD:
            if (dec->as.dsd) dsd_close(dec->as.dsd);
            break;
        case DECODER_AAC:
            if (dec->as.aac) aac_close(dec->as.aac);
            if (dec->net_stream) { http_stream_close(dec->net_stream); dec->net_stream = NULL; }
            break;
        case DECODER_ALAC:
            if (dec->as.alac) alac_close(dec->as.alac);
            break;
        case DECODER_APE:
            if (dec->as.ape) ape_close(dec->as.ape);
            break;
        case DECODER_WMA:
            if (dec->as.wma) wma_close(dec->as.wma);
            break;
        case DECODER_OPUS:
            if (dec->as.opus) opus_close(dec->as.opus);
            break;
        case DECODER_VORBIS:
            if (dec->as.vorbis) vorbis_close(dec->as.vorbis);
            break;
    }
}

/* --- Playback state ---
 *
 * A single playback thread lives for the app's whole lifetime (created once
 * by audio_init(), never joined) rather than one thread per track, so that
 * gapless and crossfade transitions between tracks never have to tear down
 * and reopen the output device or spin up a fresh thread. gui.c still owns
 * the playlist and its current index; this layer just plays "current" and,
 * optionally, knows about "next" (audio_set_next_track()) so it can prefetch
 * and, near current's natural end, either hand off seamlessly (gapless) or
 * blend the two (crossfade) entirely on its own -- no GUI round-trip.
 * Explicit user actions (tapping a different track, prev/next buttons, the
 * initial pick) always go through audio_play_file_at(), which interrupts
 * everything and restarts immediately with no fade, the same distinction
 * most real DAPs make between a manual skip and automatic progression. */

static pthread_t audio_thread;
static pthread_mutex_t audio_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t audio_cond = PTHREAD_COND_INITIALIZER;

static bool thread_started = false;

static bool have_current = false;   /* something loaded (playing or paused) */
static bool stop_requested = false;
static bool paused = false;
static bool track_finished = false; /* true EOF with no next track queued (or it failed to open) */
static bool track_advanced = false; /* thread moved on to the queued next track on its own */
static audio_error_t last_playback_error = AUDIO_ERROR_NONE;
static uint64_t last_playback_error_generation = 0;

static bool restart_requested = false;
static uint64_t next_track_generation = 0;
static char * restart_path = NULL;  /* owned; consumed by the thread on restart */
static double restart_start_seconds = 0.0;
static float restart_replaygain_linear = 1.0f;
static bool restart_replaygain_applied = false;

static char * next_path = NULL;     /* owned; staged by audio_set_next_track(), NULL = none queued */
static float next_replaygain_linear = 1.0f;
static bool next_replaygain_applied = false;

static bool crossfade_enabled = false;
#define CROSSFADE_SECONDS 3.0
#define MAX_CHANNELS 8

static float volume = 1.0f;      /* UI-facing 0.0-1.0 percent, what audio_get_volume() returns */
static float volume_gain = 1.0f; /* actual linear PCM multiplier the playback thread applies -- see audio_set_volume() */
static atomic_uint volume_set_generation = 0;
static pthread_once_t volume_request_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t volume_request_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t volume_request_cond = PTHREAD_COND_INITIALIZER;
static bool volume_request_worker_ready = false;
static bool volume_request_pending = false;
static float volume_requested_value = 1.0f;
static unsigned int volume_requested_generation = 0;
static bool low_power_mode = false;

/* Every explicit track start advances this generation. Pending seeks are
 * accepted only if they still belong to this exact track. */
static uint64_t playback_generation = 0;
static char * active_path = NULL; /* protected by audio_mutex */

/* Seeks are coalesced commands on the playback-owned decoder, so rapid taps
 * cannot stack decoder instances. */
static bool seek_pending = false;
static uint64_t seek_pending_frame = 0;
static uint64_t seek_pending_playback_generation = 0;
static bool seek_pending_is_percent = false;
static double seek_pending_percent = 0.0;

static uint64_t frames_played = 0;
static uint64_t current_total_frames = 0;
static unsigned int current_sample_rate = 0;
static audio_current_format_info_t current_format_info;
static uint64_t current_format_generation = 0;

typedef enum {
    MP3_INDEX_READY,
    MP3_INDEX_TRANSIENT_FAILURE,
    MP3_INDEX_PERMANENT_FAILURE
} mp3_index_outcome_t;

typedef struct mp3_index_job {
    char * path;
    uint64_t generation;
    unsigned int sample_rate;
    unsigned int channels;
    uint64_t stream_length;
    uint64_t stream_start_offset;
    uint32_t delay_frames;
    uint32_t padding_frames;
    uint64_t total_pcm_frames; /* already known from the Xing/Info tag (dec->total_frames) -- lets the
                                 * worker place points in one pass instead of re-deriving this via its
                                 * own full-file counting pass, see mp3_build_seek_points_cancellable() */
    drmp3_uint32 count;
    drmp3_seek_point * points;
    mp3_index_outcome_t outcome;
} mp3_index_job_t;

/* One scan-only decoder may exist at a time. It never touches the live
 * decoder; the playback thread adopts only its small completed table. */
static bool mp3_index_worker_active = false;
static mp3_index_job_t * mp3_index_result = NULL;
static bool mp3_seek_deferred = false;
static uint64_t mp3_seek_deferred_frame = 0;
static uint64_t mp3_seek_deferred_generation = 0;
static unsigned int mp3_seek_deferred_sample_rate = 0;
static unsigned int mp3_seek_retry_count = 0;
static uint64_t mp3_seek_retry_after_ms = 0;

/* Lock-free cancellation hint for the worker thread (see
 * mp3_index_should_cancel()): mirrors playback_generation (updated at the
 * same handful of call sites that bump it, and on audio_stop()) so the
 * single-pass scan loop can check "is my job still relevant" without taking
 * audio_mutex on every MP3 frame -- that would serialize it against the
 * real-time playback thread on every iteration, defeating the point of
 * running this off-thread at all. A stale read here only wastes a little
 * extra worker CPU/SD time before the next check catches up -- the actual
 * adopt-vs-discard decision (audio_thread_func's own generation-match check
 * on mp3_index_result) still happens correctly under the mutex regardless. */
static atomic_uint mp3_index_active_generation = 0;
static atomic_bool mp3_index_stop_flag = false;

static bool mp3_index_should_cancel(uint64_t job_generation) {
    if (atomic_load_explicit(&mp3_index_stop_flag, memory_order_relaxed)) return true;
    return atomic_load_explicit(&mp3_index_active_generation, memory_order_relaxed) != (unsigned int) job_generation;
}

#define MP3_SEEK_RETRY_MAX 3u
#define MP3_SEEK_RETRY_DELAY_MS 500u

static uint64_t monotonic_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t) now.tv_sec * 1000ULL + (uint64_t) now.tv_nsec / 1000000ULL;
}

/* audio_mutex must be held. A new user/resume target gets a fresh bounded
 * retry budget; worker retries preserve the same target and budget. */
static void defer_mp3_seek_locked(const decoder_t * dec, uint64_t generation,
                                  uint64_t frame) {
    mp3_seek_deferred = true;
    mp3_seek_deferred_frame = frame;
    mp3_seek_deferred_generation = generation;
    mp3_seek_deferred_sample_rate = dec->sample_rate;
    mp3_seek_retry_count = 0;
    mp3_seek_retry_after_ms = 0;
}

static void finish_mp3_deferred_seek_locked(void) {
    mp3_seek_deferred = false;
    mp3_seek_retry_count = 0;
    mp3_seek_retry_after_ms = 0;
}

static void free_mp3_index_job(mp3_index_job_t * job) {
    if (!job) return;
    free(job->points);
    free(job->path);
    free(job);
}

/* ---- Persistent on-disk seek-index cache -----------------------------
 * Keyed by a hash of the absolute path (same FNV-1a 64-bit convention as
 * albumart.c's thumbnail_key()); validated against the source file's size
 * and mtime (same trust level as albumart.c's own cache -- see its
 * source_mtime_of()/stored-vs-src check), not a full reopen+reparse. A
 * modified-but-same-size-and-mtime file is the one theoretical gap this
 * shares with every other mtime-keyed cache in this codebase; not treated
 * differently here. Saved only for a scan that ran to completion (see
 * mp3_index_worker()) -- never a cancelled partial one, which would
 * otherwise get permanently mistaken for the real, full index next load. */
#define MP3_SEEK_CACHE_MAGIC 0x33506d68u /* "hmp3" */
#define MP3_SEEK_CACHE_VERSION 2u

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint64_t file_size;
    int64_t file_mtime;
    uint32_t sample_rate;
    uint32_t channels;
    uint64_t stream_length;
    uint64_t stream_start_offset;
    uint32_t delay_frames;
    uint32_t padding_frames;
    uint32_t point_count;
    /* Spacing this table was built at -- checked on load so retuning
     * MP3_SEEK_INDEX_INTERVAL_SECONDS invalidates old on-disk tables
     * immediately instead of silently serving stale-density ones until a
     * file's own size/mtime happens to change. */
    uint32_t interval_seconds;
} mp3_seek_cache_header_t;

typedef struct {
    uint64_t seek_pos_in_bytes;
    uint64_t pcm_frame_index;
    uint16_t mp3_frames_to_discard;
    uint16_t pcm_frames_to_discard;
    uint32_t reserved;
} mp3_seek_cache_point_t; /* 24 bytes -- see MP3_SEEK_INDEX_MAX_POINTS's own comment */

static uint64_t mp3_seek_cache_hash_path(const char * path) {
    uint64_t hash = UINT64_C(14695981039346656037);
    const unsigned char * p = (const unsigned char *) path;
    if (*p) do { hash = (hash ^ *p) * UINT64_C(1099511628211); } while (*++p);
    return hash;
}

static bool mp3_seek_cache_path(const char * source_path, char * out, size_t out_size) {
    int n = snprintf(out, out_size, "%s/%016llx.idx", MP3_SEEK_CACHE_DIR,
                      (unsigned long long) mp3_seek_cache_hash_path(source_path));
    return n > 0 && (size_t) n < out_size;
}

static bool mp3_seek_cache_load(const char * source_path, const mp3_index_job_t * job,
                                drmp3_seek_point ** out_points, drmp3_uint32 * out_count) {
    struct stat st;
    if (stat(source_path, &st) != 0) return false;

    char path[PATH_MAX];
    if (!mp3_seek_cache_path(source_path, path, sizeof(path))) return false;

    FILE * f = fopen(path, "rb");
    if (!f) return false;

    mp3_seek_cache_header_t hdr;
    bool ok = fread(&hdr, 1, sizeof(hdr), f) == sizeof(hdr);
    if (ok) {
        ok = hdr.magic == MP3_SEEK_CACHE_MAGIC && hdr.version == MP3_SEEK_CACHE_VERSION &&
             hdr.file_size == (uint64_t) st.st_size && hdr.file_mtime == (int64_t) st.st_mtime &&
             hdr.sample_rate == job->sample_rate && hdr.channels == job->channels &&
             hdr.stream_length == job->stream_length && hdr.stream_start_offset == job->stream_start_offset &&
             hdr.delay_frames == job->delay_frames && hdr.padding_frames == job->padding_frames &&
             hdr.interval_seconds == MP3_SEEK_INDEX_INTERVAL_SECONDS &&
             hdr.point_count > 0 && hdr.point_count <= MP3_SEEK_INDEX_MAX_POINTS;
    }
    drmp3_seek_point * points = NULL;
    if (ok) {
        points = calloc(hdr.point_count, sizeof(*points));
        if (!points) ok = false;
    }
    for (uint32_t i = 0; ok && i < hdr.point_count; i++) {
        mp3_seek_cache_point_t raw;
        if (fread(&raw, 1, sizeof(raw), f) != sizeof(raw)) { ok = false; break; }
        points[i].seekPosInBytes = raw.seek_pos_in_bytes;
        points[i].pcmFrameIndex = raw.pcm_frame_index;
        points[i].mp3FramesToDiscard = (drmp3_uint16) raw.mp3_frames_to_discard;
        points[i].pcmFramesToDiscard = (drmp3_uint16) raw.pcm_frames_to_discard;
    }
    fclose(f);

    if (!ok) { free(points); return false; }
    *out_points = points;
    *out_count = hdr.point_count;
    return true;
}

/* Same mkstemp+fsync+rename+fsync-directory atomic-write pattern already
 * used by albumart.c's albumart_store_rgb565() / scanner_save_last_boot() --
 * best-effort: a failure here just means the next open scans again, no
 * different from a cold cache. */
static void mp3_seek_cache_save(const char * source_path, const mp3_index_job_t * job,
                                const drmp3_seek_point * points, drmp3_uint32 count) {
    if (count == 0) return;
    struct stat st;
    if (stat(source_path, &st) != 0) return;

    mkdir(OPEN_HIBY_DIR, 0755);
    mkdir(MP3_SEEK_CACHE_DIR, 0755);

    char path[PATH_MAX], tmp[PATH_MAX + 16];
    if (!mp3_seek_cache_path(source_path, path, sizeof(path))) return;
    if (snprintf(tmp, sizeof(tmp), "%s.tmp.XXXXXX", path) >= (int) sizeof(tmp)) return;

    int fd = mkstemp(tmp);
    if (fd < 0) return;
    FILE * f = fdopen(fd, "wb");
    if (!f) { close(fd); unlink(tmp); return; }

    mp3_seek_cache_header_t hdr = {
        .magic = MP3_SEEK_CACHE_MAGIC, .version = MP3_SEEK_CACHE_VERSION,
        .file_size = (uint64_t) st.st_size, .file_mtime = (int64_t) st.st_mtime,
        .sample_rate = job->sample_rate, .channels = job->channels,
        .stream_length = job->stream_length, .stream_start_offset = job->stream_start_offset,
        .delay_frames = job->delay_frames, .padding_frames = job->padding_frames,
        .point_count = count, .interval_seconds = MP3_SEEK_INDEX_INTERVAL_SECONDS,
    };
    bool ok = fwrite(&hdr, 1, sizeof(hdr), f) == sizeof(hdr);
    for (drmp3_uint32 i = 0; ok && i < count; i++) {
        mp3_seek_cache_point_t raw = {
            .seek_pos_in_bytes = points[i].seekPosInBytes, .pcm_frame_index = points[i].pcmFrameIndex,
            .mp3_frames_to_discard = points[i].mp3FramesToDiscard,
            .pcm_frames_to_discard = points[i].pcmFramesToDiscard, .reserved = 0,
        };
        ok = fwrite(&raw, 1, sizeof(raw), f) == sizeof(raw);
    }
    if (ok && fflush(f) != 0) ok = false;
    if (ok && fsync(fileno(f)) != 0) ok = false;
    if (fclose(f) != 0) ok = false;
    if (ok && rename(tmp, path) != 0) ok = false;
    if (ok) {
        int dfd = open(MP3_SEEK_CACHE_DIR, O_RDONLY | O_DIRECTORY);
        if (dfd >= 0) { fsync(dfd); close(dfd); }
    }
    if (!ok) unlink(tmp);
}

/* Faithful single-pass port of dr_mp3's own drmp3_calculate_seek_points()
 * (identical DRMP3_SEEK_LEADING_MP3_FRAMES-based reservoir priming via the
 * same sliding frame_info[] window and drmp3__accumulate_running_pcm_frame_
 * count() sample-rate-conversion accounting it already uses internally --
 * both reused directly rather than reimplemented, since dr_mp3's own
 * implementation is compiled into this same translation unit) with three
 * differences: (1) it's given total_pcm_frames instead of re-deriving both
 * that AND a separate MP3-frame count via drmp3_get_mp3_and_pcm_frame_
 * count()'s own full-file counting pass, so this reads the file once, not
 * twice; (2) it checks mp3_index_should_cancel() once per MP3 frame and
 * returns whatever prefix was already placed, via *out_cancelled, instead of
 * always running to completion; (3) since it only ever runs against
 * mp3_index_worker()'s own throwaway `scan` decoder (never the live playback
 * one), there is no original position to restore on exit -- unlike the
 * general-purpose original, which must reseek back to wherever the caller's
 * decoder started, even on failure. */
static drmp3_uint32 mp3_build_seek_points_cancellable(drmp3 * scan, uint64_t total_pcm_frames,
                                                       drmp3_uint32 desired_count, uint64_t job_generation,
                                                       drmp3_seek_point * points, bool * out_cancelled) {
    *out_cancelled = false;
    if (desired_count == 0 || total_pcm_frames == 0) return 0;

    uint64_t pcm_frames_between_points = total_pcm_frames / ((uint64_t) desired_count + 1);
    if (pcm_frames_between_points == 0) return 0;
    if (!drmp3_seek_to_start_of_stream(scan)) return 0;

    drmp3__seeking_mp3_frame_info frame_info[DRMP3_SEEK_LEADING_MP3_FRAMES + 1];
    drmp3_uint64 running_pcm_frame_count = 0;
    float running_fractional = 0.0f;

    for (uint32_t i = 0; i < DRMP3_SEEK_LEADING_MP3_FRAMES + 1; i++) {
        frame_info[i].bytePos = scan->streamCursor - scan->dataSize;
        frame_info[i].pcmFrameIndex = running_pcm_frame_count;
        drmp3_uint32 pcm_in_frame = drmp3_decode_next_frame_ex(scan, NULL, NULL, NULL);
        if (pcm_in_frame == 0) return 0; /* fewer frames than the leading window -- too short to index */
        drmp3__accumulate_running_pcm_frame_count(scan, pcm_in_frame, &running_pcm_frame_count, &running_fractional);
    }

    uint64_t next_target = 0;
    drmp3_uint32 placed = 0;
    while (placed < desired_count) {
        if (mp3_index_should_cancel(job_generation)) { *out_cancelled = true; return placed; }
        next_target += pcm_frames_between_points;

        for (;;) {
            if (next_target < running_pcm_frame_count) {
                points[placed].seekPosInBytes = frame_info[0].bytePos;
                points[placed].pcmFrameIndex = next_target;
                points[placed].mp3FramesToDiscard = DRMP3_SEEK_LEADING_MP3_FRAMES;
                points[placed].pcmFramesToDiscard =
                    (drmp3_uint16) (next_target - frame_info[DRMP3_SEEK_LEADING_MP3_FRAMES - 1].pcmFrameIndex);
                break;
            }
            for (size_t i = 0; i < DRMP3_SEEK_LEADING_MP3_FRAMES; i++) frame_info[i] = frame_info[i + 1];
            frame_info[DRMP3_SEEK_LEADING_MP3_FRAMES].bytePos = scan->streamCursor - scan->dataSize;
            frame_info[DRMP3_SEEK_LEADING_MP3_FRAMES].pcmFrameIndex = running_pcm_frame_count;

            if (mp3_index_should_cancel(job_generation)) { *out_cancelled = true; return placed; }

            drmp3_uint32 pcm_in_frame = drmp3_decode_next_frame_ex(scan, NULL, NULL, NULL);
            if (pcm_in_frame == 0) {
                /* Ran out of frames before reaching this point's exact
                 * target (rounding, or total_pcm_frames from the Xing tag
                 * was too high) -- record a best-effort final point at the
                 * real last decoded position (running_pcm_frame_count), not
                 * next_target, which past this point no longer corresponds
                 * to anywhere actually reachable in the file. Then stop;
                 * there is nothing further to place. Not a cancellation. */
                points[placed].seekPosInBytes = frame_info[0].bytePos;
                points[placed].pcmFrameIndex = running_pcm_frame_count;
                points[placed].mp3FramesToDiscard = DRMP3_SEEK_LEADING_MP3_FRAMES;
                points[placed].pcmFramesToDiscard =
                    (drmp3_uint16) (running_pcm_frame_count - frame_info[DRMP3_SEEK_LEADING_MP3_FRAMES - 1].pcmFrameIndex);
                return placed + 1;
            }
            drmp3__accumulate_running_pcm_frame_count(scan, pcm_in_frame, &running_pcm_frame_count, &running_fractional);
        }
        placed++;
    }
    return placed;
}

static void * mp3_index_worker(void * arg) {
    mp3_index_job_t * job = arg;
    (void) nice(10); /* keep the single-core playback thread responsive */

    drmp3_seek_point * cached_points = NULL;
    drmp3_uint32 cached_count = 0;
    if (mp3_seek_cache_load(job->path, job, &cached_points, &cached_count)) {
        job->points = cached_points;
        job->count = cached_count;
        job->outcome = MP3_INDEX_READY;
        DBG_LOG("audio: MP3 seek-index loaded from cache (%u points, %s)\n",
                job->count, safe_path_tail(job->path));
    } else {
        struct timespec started, finished;
        clock_gettime(CLOCK_MONOTONIC, &started);

        job->points = calloc(job->count, sizeof(*job->points));
        drmp3 * scan = malloc(sizeof(*scan));
        if (!job->points || !scan) {
            free(scan);
            job->outcome = MP3_INDEX_TRANSIENT_FAILURE;
        } else {
            bool opened = drmp3_init_file(scan, job->path, NULL) != 0;
            bool same_file = opened && scan->sampleRate == job->sample_rate &&
                             scan->channels == job->channels &&
                             scan->streamLength == job->stream_length &&
                             scan->streamStartOffset == job->stream_start_offset &&
                             scan->delayInPCMFrames == job->delay_frames &&
                             scan->paddingInPCMFrames == job->padding_frames;
            if (!opened) {
                job->outcome = MP3_INDEX_TRANSIENT_FAILURE;
            } else if (!same_file) {
                job->outcome = MP3_INDEX_PERMANENT_FAILURE;
            } else {
                bool cancelled = false;
                drmp3_uint32 placed = mp3_build_seek_points_cancellable(
                    scan, job->total_pcm_frames, job->count, job->generation, job->points, &cancelled);
                job->count = placed;
                if (placed > 0 && !cancelled) {
                    job->outcome = MP3_INDEX_READY;
                    mp3_seek_cache_save(job->path, job, job->points, job->count);
                } else {
                    /* A cancelled scan is reported as transient regardless of
                     * how much progress it made -- it belongs to a track the
                     * playback thread has already moved past, so its partial
                     * result (still allocated in job->points for the
                     * diagnostics above) is never adopted, matching the
                     * generation check at the adoption site. */
                    job->outcome = cancelled ? MP3_INDEX_TRANSIENT_FAILURE : MP3_INDEX_PERMANENT_FAILURE;
                }
            }
            if (opened) drmp3_uninit(scan);
            free(scan);
        }

        clock_gettime(CLOCK_MONOTONIC, &finished);
        uint64_t elapsed_ms = (uint64_t) (finished.tv_sec - started.tv_sec) * 1000ULL;
        if (finished.tv_nsec >= started.tv_nsec)
            elapsed_ms += (uint64_t) (finished.tv_nsec - started.tv_nsec) / 1000000ULL;
        else
            elapsed_ms -= 1000ULL - (uint64_t) (started.tv_nsec - finished.tv_nsec) / 1000000ULL;
        DBG_LOG("audio: MP3 seek-index scan %s (%u points, %" PRIu64 " ms, %s)\n",
                job->outcome == MP3_INDEX_READY ? "ready" : "failed",
                job->count, elapsed_ms, safe_path_tail(job->path));
    }

    pthread_mutex_lock(&audio_mutex);
    mp3_index_worker_active = false;
    mp3_index_result = job;
    pthread_cond_signal(&audio_cond);
    pthread_mutex_unlock(&audio_mutex);
    return NULL;
}

/* audio_mutex must be held. Setup failures retain the target but are
 * retried only after a delay, preventing a low-memory failure from turning
 * the playback loop into an allocation/thread-creation spin. */
static bool request_mp3_seek_index_locked(decoder_t * dec, const char * path,
                                          uint64_t generation) {
    if (dec->mp3_seek_index_attempted) {
        finish_mp3_deferred_seek_locked();
        return false;
    }
    if (mp3_index_worker_active || mp3_index_result) return true;
    if (monotonic_ms() < mp3_seek_retry_after_ms) return true;

    uint64_t interval = (uint64_t) dec->sample_rate * MP3_SEEK_INDEX_INTERVAL_SECONDS;
    uint64_t desired = dec->total_frames / interval;
    if (desired < 2) {
        finish_mp3_deferred_seek_locked();
        return false;
    }
    if (desired > MP3_SEEK_INDEX_MAX_POINTS) desired = MP3_SEEK_INDEX_MAX_POINTS;

    /* Try the on-disk cache synchronously, right here on the playback
     * thread, before spawning a worker at all -- a hit is just a bounded
     * stat()+read (at most ~200 KiB, MP3_SEEK_INDEX_MAX_POINTS'-worth), and
     * this thread is already about to sit idle waiting on the defer/hold
     * mechanism regardless, so there is no real-time cost to paying that
     * bounded read here instead of via a pthread_create()'d worker plus a
     * full close_device()/reopen cycle. Without this, "replaying a song
     * should not require another scan" still paid a thread-hop and a
     * needless device toggle on every replay, sometimes slower in wall
     * time than the interval-based work it exists to avoid. A miss falls
     * through to the normal async scan unchanged. */
    mp3_index_job_t probe = {
        .sample_rate = dec->as.mp3->sampleRate, .channels = dec->as.mp3->channels,
        .stream_length = dec->as.mp3->streamLength, .stream_start_offset = dec->as.mp3->streamStartOffset,
        .delay_frames = dec->as.mp3->delayInPCMFrames, .padding_frames = dec->as.mp3->paddingInPCMFrames,
    };
    drmp3_seek_point * cached_points = NULL;
    drmp3_uint32 cached_count = 0;
    if (mp3_seek_cache_load(path, &probe, &cached_points, &cached_count)) {
        if (drmp3_bind_seek_table(dec->as.mp3, cached_count, cached_points)) {
            dec->mp3_seek_points = cached_points;
            dec->mp3_seek_point_count = cached_count;
            dec->mp3_seek_index_attempted = true;
            finish_mp3_deferred_seek_locked();
            DBG_LOG("audio: MP3 seek-index loaded from cache synchronously (%u points, %s)\n",
                    cached_count, safe_path_tail(path));
            return false; /* table is already bound -- nothing left to wait on */
        }
        free(cached_points);
    }

    mp3_index_job_t * job = calloc(1, sizeof(*job));
    if (!job || !(job->path = strdup(path))) {
        free_mp3_index_job(job);
        mp3_seek_retry_count++;
        mp3_seek_retry_after_ms = monotonic_ms() + MP3_SEEK_RETRY_DELAY_MS;
        DBG_LOG("audio: MP3 seek-index setup allocation failed, retry %u/%u (%s)\n",
                mp3_seek_retry_count, MP3_SEEK_RETRY_MAX, safe_path_tail(path));
        if (mp3_seek_retry_count >= MP3_SEEK_RETRY_MAX) {
            dec->mp3_seek_index_attempted = true;
            finish_mp3_deferred_seek_locked();
        }
        return mp3_seek_deferred;
    }
    job->generation = generation;
    job->sample_rate = dec->as.mp3->sampleRate;
    job->channels = dec->as.mp3->channels;
    job->stream_length = dec->as.mp3->streamLength;
    job->stream_start_offset = dec->as.mp3->streamStartOffset;
    job->delay_frames = dec->as.mp3->delayInPCMFrames;
    job->padding_frames = dec->as.mp3->paddingInPCMFrames;
    job->total_pcm_frames = dec->total_frames;
    job->count = (drmp3_uint32) desired;

    pthread_t worker;
    mp3_index_worker_active = true;
    if (pthread_create(&worker, NULL, mp3_index_worker, job) != 0) {
        mp3_index_worker_active = false;
        free_mp3_index_job(job);
        mp3_seek_retry_count++;
        mp3_seek_retry_after_ms = monotonic_ms() + MP3_SEEK_RETRY_DELAY_MS;
        DBG_LOG("audio: MP3 seek-index worker start failed, retry %u/%u (%s)\n",
                mp3_seek_retry_count, MP3_SEEK_RETRY_MAX, safe_path_tail(path));
        if (mp3_seek_retry_count >= MP3_SEEK_RETRY_MAX) {
            dec->mp3_seek_index_attempted = true;
            finish_mp3_deferred_seek_locked();
        }
        return mp3_seek_deferred;
    }
    pthread_detach(worker);
    return true;
}

static audio_codec_t public_codec_for_decoder(decoder_type_t type) {
    switch (type) {
        case DECODER_FLAC: return AUDIO_CODEC_FLAC;
        case DECODER_MP3: return AUDIO_CODEC_MP3;
        case DECODER_WAV:
        case DECODER_AIFF: return AUDIO_CODEC_PCM;
        case DECODER_DSD: return AUDIO_CODEC_DSD;
        case DECODER_AAC: return AUDIO_CODEC_AAC;
        case DECODER_ALAC: return AUDIO_CODEC_ALAC;
        case DECODER_APE: return AUDIO_CODEC_APE;
        case DECODER_WMA: return AUDIO_CODEC_WMA;
        case DECODER_OPUS: return AUDIO_CODEC_OPUS;
        case DECODER_VORBIS: return AUDIO_CODEC_VORBIS;
    }
    return AUDIO_CODEC_UNKNOWN;
}

bool audio_probe_file_format(const char * path, audio_current_format_info_t * out) {
    if (!path || !path[0] || !out || is_stream_url(path) || remote_track_path_is_remote(path)) return false;
    decoder_t dec;
    memset(&dec, 0, sizeof(dec));
    if (!decoder_open(&dec, path)) return false;
    memset(out, 0, sizeof(*out));
    out->valid = true;
    snprintf(out->path, sizeof(out->path), "%s", path);
    out->codec = public_codec_for_decoder(dec.type);
    out->source_sample_rate = dec.source_sample_rate ? dec.source_sample_rate : dec.sample_rate;
    out->source_bit_depth = dec.source_bit_depth;
    out->channels = dec.channels;
    out->bitrate_kbps = dec.bitrate_kbps;
    out->duration_seconds = dec.sample_rate && dec.total_frames
        ? (double) dec.total_frames / (double) dec.sample_rate : 0.0;
    out->is_dsd = dec.type == DECODER_DSD;
    decoder_close(&dec);
    return true;
}

#ifndef HOST_BUILD
/* Wide-path scratch buffers, allocated once in audio_thread_func(). The
 * primary buffer is sufficient for ordinary S24 playback; the other two are
 * optional and needed only when two wide tracks are crossfaded. Keeping those
 * availability checks separate prevents a failed optional allocation from
 * disabling otherwise-valid 24-bit playback. */
static int32_t * buf_cur_s32 = NULL;
static int32_t * buf_next_s32 = NULL;
static int32_t * buf_out_s32 = NULL;

static bool can_use_wide_path(const decoder_t * dec) {
    if (!dec) return false;
    if (dec->net_stream != NULL) return false;
    if (dec->source_bit_depth <= 16) return false;
    if (dec->type != DECODER_FLAC &&
        dec->type != DECODER_WAV &&
        dec->type != DECODER_ALAC &&
        dec->type != DECODER_APE &&
        dec->type != DECODER_AIFF) return false;
    if (!audio_output_is_local_requested()) return false;
    if (!buf_cur_s32) return false;
    return true;
}

static bool can_use_wide_crossfade(const decoder_t * cur, const decoder_t * next) {
    return can_use_wide_path(cur) && can_use_wide_path(next) &&
           buf_next_s32 != NULL && buf_out_s32 != NULL;
}
#endif

/* audio_mutex must be held. Publishing a copied scalar snapshot here keeps
 * the UI completely independent from the playback thread's stack-local
 * decoder_t and its short-lived gapless/crossfade prefetched decoder. */
static void publish_current_format_locked(const decoder_t * dec, const char * path,
                                          float replaygain_linear, bool replaygain_applied) {
    memset(&current_format_info, 0, sizeof(current_format_info));
    current_format_info.valid = true;
    if (path) snprintf(current_format_info.path, sizeof(current_format_info.path), "%s", path);
    current_format_info.codec = public_codec_for_decoder(dec->type);
    current_format_info.source_sample_rate = dec->source_sample_rate ? dec->source_sample_rate : dec->sample_rate;
    current_format_info.source_bit_depth = dec->source_bit_depth;
    current_format_info.output_sample_rate = dec->sample_rate;
#ifndef HOST_BUILD
    /* can_use_wide_path() alone is a prediction (source eligibility + what
     * output route is currently REQUESTED) -- audio_output_is_s24_active()
     * is the ground truth for what's actually open right now. They can
     * disagree: a route change to Bluetooth/USB, or an S24_LE hw_params
     * negotiation failure that fell back to S16_LE (see audio_output.c's
     * open_device()), both leave can_use_wide_path() true while the device
     * is actually running at S16_LE. Requiring both here means the UI never
     * claims 24-bit output that isn't really happening. At a crossfade
     * promotion (cur_dec = nxt_dec) this can under-report by one chunk's
     * worth of time, since the device's format for the newly-promoted track
     * isn't renegotiated until the next chunk's own ensure_device_format()
     * call -- self-corrects immediately once that runs, not worth special-
     * casing for a display value. */
    current_format_info.output_bit_depth = (can_use_wide_path(dec) && audio_output_is_s24_active()) ? 24 : 16;
#else
    current_format_info.output_bit_depth = 16; /* host simulator SDL playback is always 16-bit */
#endif
    current_format_info.channels = dec->channels;
    current_format_info.bitrate_kbps = dec->bitrate_kbps;
    current_format_info.duration_seconds = dec->sample_rate > 0 && dec->total_frames > 0
        ? (double) dec->total_frames / (double) dec->sample_rate : 0.0;
    current_format_info.is_stream = dec->net_stream != NULL;
    current_format_info.is_dsd = dec->type == DECODER_DSD;
    current_format_info.replaygain_applied = replaygain_applied;
    current_format_info.replaygain_applied_db = replaygain_applied && replaygain_linear > 0.0f
        ? 20.0 * log10((double) replaygain_linear) : 0.0;
    current_format_info.generation = ++current_format_generation;
}

static void clear_current_format_locked(void) {
    memset(&current_format_info, 0, sizeof(current_format_info));
    current_format_generation++;
}

#ifdef HOST_BUILD
static SDL_AudioDeviceID sdl_dev = 0;
static unsigned int device_channels = 0;
static unsigned int device_sample_rate = 0;
#endif

static double replaygain_to_linear(bool has_gain, double gain_db, bool has_peak, double peak) {
    double linear = has_gain ? pow(10.0, gain_db / 20.0) : 1.0;
    if (has_peak && peak > 0.0) {
        double max_linear = 1.0 / peak; /* clamp so the loudest sample can't clip */
        if (linear > max_linear) linear = max_linear;
    }
    /* Belt-and-suspenders alongside metadata.c's own parse_replaygain_gain()/
     * _peak() range limits: those already reject an absurd-but-finite
     * gain_db (e.g. a corrupted "1e308" tag) before it ever reaches this
     * function, but this is the one real choke point every gain value from
     * every tag format/call site funnels through before apply_gain()'s
     * lrintf() -- cheap enough to guard here too rather than trust every
     * current and future caller to have validated its own inputs. Falls
     * back to unity (no gain change) rather than propagating a non-finite
     * value into raw sample math. */
    if (!isfinite(linear)) linear = 1.0;
    return linear;
}

/* Applies linear gain scaling to int16 PCM samples. Uses lrintf() to round
 * to nearest integer to minimize quantization error in quiet passages. */
static void apply_gain(int16_t * buf, size_t sample_count, float gain) {
    /* Fast path: no scaling needed. A positive ReplayGain adjustment can
     * push this above 1.0f, so this can't just be ">= 0.999f". */
    if (gain > 0.999f && gain < 1.001f) return;
    for (size_t i = 0; i < sample_count; i++) {
        long scaled = lrintf((float) buf[i] * gain);
        if (scaled > 32767) scaled = 32767;
        if (scaled < -32768) scaled = -32768;
        buf[i] = (int16_t) scaled;
    }
}

#ifndef HOST_BUILD
static void apply_gain_s32(int32_t * buf, size_t sample_count, float gain) {
    if (gain > 0.999f && gain < 1.001f) return;
    for (size_t i = 0; i < sample_count; i++) {
        float scaled_f = (float) buf[i] * gain;
        if (scaled_f > 8388607.0f) {
            buf[i] = 8388607;
        } else if (scaled_f < -8388608.0f) {
            buf[i] = -8388608;
        } else {
            buf[i] = (int32_t) lrintf(scaled_f);
        }
    }
}
#endif

/* Closes and reopens a local decoder at last_confirmed_frame.
 * Only valid for finite local files (not net_stream). Returns true on
 * success; on failure dec is left in a closed/invalid state. */
static bool reopen_decoder_at(decoder_t * dec, const char * path,
                               uint64_t last_confirmed_frame) {
    drmp3_seek_point * saved_mp3_points = dec->mp3_seek_points;
    drmp3_uint32 saved_mp3_count = dec->mp3_seek_point_count;
    bool saved_mp3_index_attempted = dec->mp3_seek_index_attempted;
    unsigned int saved_mp3_rate = saved_mp3_points ? dec->as.mp3->sampleRate : 0;
    unsigned int saved_mp3_channels = saved_mp3_points ? dec->as.mp3->channels : 0;
    uint64_t saved_mp3_length = saved_mp3_points ? dec->as.mp3->streamLength : 0;
    uint64_t saved_mp3_offset = saved_mp3_points ? dec->as.mp3->streamStartOffset : 0;
    uint32_t saved_mp3_delay = saved_mp3_points ? dec->as.mp3->delayInPCMFrames : 0;
    uint32_t saved_mp3_padding = saved_mp3_points ? dec->as.mp3->paddingInPCMFrames : 0;
    dec->mp3_seek_points = NULL;
    dec->mp3_seek_point_count = 0;
    decoder_close(dec);
    if (!decoder_open(dec, path)) {
        free(saved_mp3_points);
        return false;
    }
    if (saved_mp3_points) {
        if (dec->type != DECODER_MP3 ||
            dec->as.mp3->sampleRate != saved_mp3_rate ||
            dec->as.mp3->channels != saved_mp3_channels ||
            dec->as.mp3->streamLength != saved_mp3_length ||
            dec->as.mp3->streamStartOffset != saved_mp3_offset ||
            dec->as.mp3->delayInPCMFrames != saved_mp3_delay ||
            dec->as.mp3->paddingInPCMFrames != saved_mp3_padding ||
            !drmp3_bind_seek_table(dec->as.mp3, saved_mp3_count, saved_mp3_points)) {
            free(saved_mp3_points);
            decoder_close(dec);
            return false;
        }
        dec->mp3_seek_points = saved_mp3_points;
        dec->mp3_seek_point_count = saved_mp3_count;
        dec->mp3_seek_index_attempted = true;
    } else if (dec->type == DECODER_MP3 && saved_mp3_index_attempted) {
        /* No table was ever bound and indexing had already permanently given
         * up before this reopen -- decoder_open() just memset the decoder
         * fresh, losing that state. Restore it purely so the normal
         * playback-loop retry/eager paths don't re-scan a file already
         * known not to be indexable; the seek below does not depend on this
         * flag either way (see the DECODER_MP3 branch just under). */
        dec->mp3_seek_index_attempted = true;
    }
    if (last_confirmed_frame > 0) {
        /* This is an internal recovery/fallback seek (premature-EOF retry,
         * or the caller's own failed-seek reopen), not the interactive
         * playback-loop path -- it must always land now, regardless of
         * whether an index attempt is still outstanding for this decoder.
         * decoder_seek()'s defer-until-indexed gate exists for that other
         * path; bypass it here for MP3 and seek directly, exactly as
         * decoder_seek() itself does once an index attempt has resolved
         * (drmp3_seek_to_pcm_frame() uses a bound table if one exists, or
         * falls back to its own brute-force seek otherwise). Without this,
         * every reopen while a background scan is still running for this
         * track (i.e. saved_mp3_points is NULL and attempted was never set)
         * would be refused outright, since mp3_needs_seek_index() covers
         * ordinary track lengths. */
        bool seek_ok = dec->type == DECODER_MP3
            ? drmp3_seek_to_pcm_frame(dec->as.mp3, last_confirmed_frame) != 0
            : decoder_seek(dec, last_confirmed_frame);
        if (!seek_ok) {
            decoder_close(dec);
            return false;
        }
    }
    return true;
}

#ifdef HOST_BUILD
static bool open_device(unsigned int channels, unsigned int sample_rate) {
    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = (int) sample_rate;
    want.format = AUDIO_S16SYS;
    want.channels = (Uint8) channels;
    want.samples = NORMAL_CHUNK_FRAMES;

    sdl_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (sdl_dev == 0) {
        fprintf(stderr, "audio: SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        return false;
    }
    SDL_PauseAudioDevice(sdl_dev, 0);
    device_channels = channels;
    device_sample_rate = sample_rate;
    return true;
}

static void close_device(void) {
    if (sdl_dev != 0) { SDL_CloseAudioDevice(sdl_dev); sdl_dev = 0; }
    device_channels = 0;
    device_sample_rate = 0;
}

static bool ensure_device(unsigned int channels, unsigned int sample_rate) {
    bool device_open = (sdl_dev != 0);
    if (device_open && device_channels == channels && device_sample_rate == sample_rate) return true;
    close_device();
    return open_device(channels, sample_rate);
}

static void write_device(const int16_t * buf, uint64_t frames, unsigned int channels) {
    SDL_QueueAudio(sdl_dev, buf, (Uint32) (frames * channels * sizeof(int16_t)));

    /* Throttle so the decode loop doesn't race ahead and queue the whole file at once */
    Uint32 max_queued_bytes = device_sample_rate * channels * sizeof(int16_t);
    while (SDL_GetQueuedAudioSize(sdl_dev) > max_queued_bytes) {
        pthread_mutex_lock(&audio_mutex);
        bool stop_now = stop_requested || restart_requested;
        pthread_mutex_unlock(&audio_mutex);
        if (stop_now) break;
        usleep(10000);
    }
}
#else
/* Target build: thin wrappers around the shared audio_output module (see
 * audio_output.h) for output routing and format configuration. */
static bool ensure_device_format(unsigned int channels, unsigned int sample_rate, bool want_s24) {
    /* Decoder-fed local playback requests standard battery-tuned local buffer. */
    bool ok = audio_output_ensure(channels, sample_rate, false, want_s24);
    if (ok) {
        /* Reapply volume if USB output state changed so digital volume taper
         * is correctly activated or deactivated. */
        static bool last_gain_for_usb = false;
        bool now_usb = audio_output_is_usb_active();
        if (now_usb != last_gain_for_usb) {
            audio_set_volume(audio_get_volume());
            last_gain_for_usb = now_usb;
        }
    }
    return ok;
}

static bool ensure_device(unsigned int channels, unsigned int sample_rate) {
    return ensure_device_format(channels, sample_rate, false);
}

static void close_device(void) {
    audio_output_close();
}


/* Writes a PCM batch to the output device with bounded close+reopen
 * retries. Returns true when all frames were successfully delivered.
 * Checks stop/restart between retries so the playback thread stays
 * controllable, unless allow_during_stop_restart is true for controlled
 * transition ramps (e.g. pause/stop/seek ramp-downs before powering down).
 * Tracks exact delivered frame count to avoid replaying audio on retry.
 *
 * Target build only: the HOST_BUILD SDL path uses write_device() directly
 * (SDL never needs close+reopen recovery the way tinyalsa/aplay can). */
#define OUTPUT_WRITE_RETRIES 3
#define OUTPUT_WRITE_RETRY_SLEEP_US 20000  /* 20 ms */

static write_result_t write_device_with_retry_ex(const int16_t * buf, uint64_t frames,
                                                unsigned int channels,
                                                unsigned int sample_rate,
                                                const char * path,
                                                uint64_t * out_delivered_frames,
                                                bool allow_during_stop_restart) {
    uint64_t delivered = 0;
    if (out_delivered_frames) *out_delivered_frames = 0;

    if (!ensure_device(channels, sample_rate)) {
        return WRITE_RESULT_FAILED;
    }

    for (int attempt = 0; attempt <= OUTPUT_WRITE_RETRIES; attempt++) {
        pthread_mutex_lock(&audio_mutex);
        bool abort = should_abort_write_retry(allow_during_stop_restart, stop_requested, restart_requested);
        pthread_mutex_unlock(&audio_mutex);
        if (abort) {
            if (out_delivered_frames) *out_delivered_frames = delivered;
            return WRITE_RESULT_ABORTED;
        }

        if (attempt > 0) {
            close_device();
            usleep(OUTPUT_WRITE_RETRY_SLEEP_US);
            if (!ensure_device(channels, sample_rate)) {
                DBG_LOG("audio: output reopen failed on retry %d (%s)\n",
                        attempt, safe_path_tail(path));
                continue;
            }
            DBG_LOG("audio: output reopened for retry %d (%s)\n",
                    attempt, safe_path_tail(path));
        }

        const int16_t * cur_buf = buf + (size_t) (delivered * channels);
        uint64_t remaining_frames = frames - delivered;
        uint64_t written_this_call = 0;
        bool ok = audio_output_write(cur_buf, remaining_frames, channels, &written_this_call);
        delivered += written_this_call;
        if (out_delivered_frames) *out_delivered_frames = delivered;

        if (ok || delivered >= frames) return WRITE_RESULT_OK;

        DBG_LOG("audio: output write failed attempt %d/%d (delivered %" PRIu64 "/%" PRIu64 " frames) (%s)\n",
                attempt + 1, OUTPUT_WRITE_RETRIES + 1, delivered, frames, safe_path_tail(path));
    }
    DBG_LOG("audio: output recovery exhausted (%s)\n", safe_path_tail(path));
    if (out_delivered_frames) *out_delivered_frames = delivered;
    return WRITE_RESULT_FAILED;
}

static inline write_result_t write_device_with_retry(const int16_t * buf, uint64_t frames,
                                                     unsigned int channels,
                                                     unsigned int sample_rate,
                                                     const char * path,
                                                     uint64_t * out_delivered_frames) {
    return write_device_with_retry_ex(buf, frames, channels, sample_rate, path, out_delivered_frames, false);
}

static inline write_result_t write_device_transition_ramp(const int16_t * buf, uint64_t frames,
                                                          unsigned int channels,
                                                          unsigned int sample_rate,
                                                          const char * path,
                                                          uint64_t * out_delivered_frames) {
    return write_device_with_retry_ex(buf, frames, channels, sample_rate, path, out_delivered_frames, true);
}

/* fallback_buf must have room for at least frames*channels int16_t samples
 * (buf_cur, already allocated at MAX_CHUNK_FRAMES*MAX_CHANNELS and otherwise
 * idle while a chunk is being decoded/written through this s32 path, is what
 * every real caller passes -- see write_device_with_retry_s32()'s own
 * caller). Used only if the device turns out not to actually be local S24_LE
 * when it's time to write -- see the ground-truth check inside the loop. */
static write_result_t write_device_with_retry_s32_ex(const int32_t * buf, uint64_t frames,
                                                    unsigned int channels,
                                                    unsigned int sample_rate,
                                                    const char * path,
                                                    int16_t * fallback_buf,
                                                    uint64_t * out_delivered_frames,
                                                    bool allow_during_stop_restart) {
    uint64_t delivered = 0;
    if (out_delivered_frames) *out_delivered_frames = 0;

    if (!ensure_device_format(channels, sample_rate, true)) {
        return WRITE_RESULT_FAILED;
    }

    for (int attempt = 0; attempt <= OUTPUT_WRITE_RETRIES; attempt++) {
        pthread_mutex_lock(&audio_mutex);
        bool abort = should_abort_write_retry(allow_during_stop_restart, stop_requested, restart_requested);
        pthread_mutex_unlock(&audio_mutex);
        if (abort) {
            if (out_delivered_frames) *out_delivered_frames = delivered;
            return WRITE_RESULT_ABORTED;
        }

        if (attempt > 0) {
            close_device();
            usleep(OUTPUT_WRITE_RETRY_SLEEP_US);
            if (!ensure_device_format(channels, sample_rate, true)) {
                DBG_LOG("audio: output reopen (s32) failed on retry %d (%s)\n",
                        attempt, safe_path_tail(path));
                continue;
            }
            DBG_LOG("audio: output reopened (s32) for retry %d (%s)\n",
                    attempt, safe_path_tail(path));
        }

        const int32_t * cur_buf = buf + (size_t) (delivered * channels);
        uint64_t remaining_frames = frames - delivered;
        uint64_t written_this_call = 0;
        bool ok;
        if (audio_output_is_s24_active()) {
            ok = audio_output_write_s24(cur_buf, remaining_frames, channels, &written_this_call);
        } else {
            /* ensure_device_format() above only REQUESTED S24_LE -- this is the
             * ground truth for what actually opened. A route change to
             * Bluetooth/USB landing between the request and this write (want_s24
             * is silently irrelevant to those paths), or an S24_LE hw_params
             * negotiation failure that fell back to S16_LE (open_device()'s own
             * fallback), both leave this false even though the request above
             * just returned true. audio_output_write_s24() itself now refuses
             * this combination rather than corrupting samples (see its own
             * guard), so retrying it here would only exhaust the budget and
             * fail the whole track for no reason. Narrow this remaining portion
             * to S16 and deliver it through the ordinary, already-correct path
             * instead -- checked fresh on every attempt, not just once, so a
             * route/format change mid-retry is caught too. */
            size_t sample_count = (size_t) remaining_frames * channels;
            for (size_t i = 0; i < sample_count; i++) {
                fallback_buf[i] = (int16_t) (cur_buf[i] >> 8);
            }
            ok = audio_output_write(fallback_buf, remaining_frames, channels, &written_this_call);
        }
        delivered += written_this_call;
        if (out_delivered_frames) *out_delivered_frames = delivered;

        if (ok || delivered >= frames) return WRITE_RESULT_OK;

        DBG_LOG("audio: output write (s32) failed attempt %d/%d (delivered %" PRIu64 "/%" PRIu64 " frames) (%s)\n",
                attempt + 1, OUTPUT_WRITE_RETRIES + 1, delivered, frames, safe_path_tail(path));
    }
    DBG_LOG("audio: output recovery (s32) exhausted (%s)\n", safe_path_tail(path));
    if (out_delivered_frames) *out_delivered_frames = delivered;
    return WRITE_RESULT_FAILED;
}

static inline write_result_t write_device_with_retry_s32(const int32_t * buf, uint64_t frames,
                                                         unsigned int channels,
                                                         unsigned int sample_rate,
                                                         const char * path,
                                                         int16_t * fallback_buf,
                                                         uint64_t * out_delivered_frames) {
    return write_device_with_retry_s32_ex(buf, frames, channels, sample_rate, path, fallback_buf, out_delivered_frames, false);
}
#endif /* !HOST_BUILD */

static void close_decoder_if_open(decoder_t * dec, bool * is_open) {
    if (*is_open) { decoder_close(dec); *is_open = false; }
}

/* Mixes n frames of buf_cur/buf_next into buf_out with a linear crossfade.
 * fade_start_frame is how many frames into the CROSSFADE_SECONDS window
 * this chunk's first frame falls, so the fade is continuous across chunk
 * boundaries rather than stepped.
 *
 * int16_t crossfade blending path. Used when either the current or next track
 * is 16-bit. When both tracks qualify for S24_LE wide output, mix_crossfade_s32()
 * is used instead to avoid reopening ALSA at S16_LE. */
static void mix_crossfade(const int16_t * buf_cur, const int16_t * buf_next, int16_t * buf_out,
                           uint64_t n, unsigned int channels, uint64_t fade_start_frame, uint64_t crossfade_frames) {
    for (uint64_t k = 0; k < n; k++) {
        float fade_next = (float) (fade_start_frame + k) / (float) crossfade_frames;
        if (fade_next > 1.0f) fade_next = 1.0f;
        if (fade_next < 0.0f) fade_next = 0.0f;
        float fade_cur = 1.0f - fade_next;
        for (unsigned int ch = 0; ch < channels; ch++) {
            size_t idx = (size_t) k * channels + ch;
            float mixed = (float) buf_cur[idx] * fade_cur + (float) buf_next[idx] * fade_next;
            if (mixed > 32767.0f) mixed = 32767.0f;
            if (mixed < -32768.0f) mixed = -32768.0f;
            buf_out[idx] = (int16_t) mixed;
        }
    }
}

#ifndef HOST_BUILD
/* Same as mix_crossfade() but for the S24_LE wide path's int32_t buffers
 * (right-justified 24-in-32). Only ever called when BOTH cur_dec and
 * nxt_dec have already been confirmed S24_LE-eligible -- see this
 * function's own caller. */
static void mix_crossfade_s32(const int32_t * buf_cur, const int32_t * buf_next, int32_t * buf_out,
                              uint64_t n, unsigned int channels, uint64_t fade_start_frame, uint64_t crossfade_frames) {
    for (uint64_t k = 0; k < n; k++) {
        float fade_next = (float) (fade_start_frame + k) / (float) crossfade_frames;
        if (fade_next > 1.0f) fade_next = 1.0f;
        if (fade_next < 0.0f) fade_next = 0.0f;
        float fade_cur = 1.0f - fade_next;
        for (unsigned int ch = 0; ch < channels; ch++) {
            size_t idx = (size_t) k * channels + ch;
            float mixed = (float) buf_cur[idx] * fade_cur + (float) buf_next[idx] * fade_next;
            if (mixed > 8388607.0f) mixed = 8388607.0f;
            if (mixed < -8388608.0f) mixed = -8388608.0f;
            buf_out[idx] = (int32_t) mixed;
        }
    }
}
#endif

static void * audio_thread_func(void * arg) {
    (void) arg;
    /* A modest priority boost, not full SCHED_FIFO: real-time scheduling
     * would risk priority inversion against audio_mutex (also taken by the
     * UI thread for format/volume reads), which could make things worse
     * under contention rather than better. nice(-5) just makes this thread
     * win CFS scheduling contests against normal-priority UI/decode work
     * (library scans/MP3 indexing already deliberately lower their own
     * priority the same way, see mp3_index_worker's nice(10) above) without
     * that inversion risk. Best-effort: fails silently without CAP_SYS_NICE
     * (e.g. an unprivileged host dev build), leaving the thread at its
     * inherited default priority. Matters most at high sample rates, where
     * the ALSA buffer covers less wall-clock time and a UI-caused scheduling
     * delay is more likely to outlast it -- see open_device()'s own comment
     * on why period_size is now rate-scaled for the same reason. */
    (void) nice(-5);

    decoder_t cur_dec;
    bool cur_open = false;
    char * cur_path_local = NULL;
    uint64_t cur_frames_played_local = 0;
    float cur_replaygain_linear = 1.0f;
    bool cur_replaygain_applied = false;

    decoder_t nxt_dec;
    bool nxt_open = false;
    bool nxt_format_matches = false;
    float nxt_replaygain_linear_local = 1.0f;
    bool nxt_replaygain_applied_local = false;
    uint64_t nxt_frames_consumed = 0;

    int16_t * buf_cur = malloc((size_t) MAX_CHUNK_FRAMES * MAX_CHANNELS * sizeof(int16_t));
    int16_t * buf_next = malloc((size_t) MAX_CHUNK_FRAMES * MAX_CHANNELS * sizeof(int16_t));
    int16_t * buf_out = malloc((size_t) MAX_CHUNK_FRAMES * MAX_CHANNELS * sizeof(int16_t));
#ifndef HOST_BUILD
    /* File-scope statics so the eligibility gates can verify allocation.
     * A missing primary buffer disables all wide playback; missing optional
     * blend buffers disable only wide crossfade. */
    buf_cur_s32 = malloc((size_t) MAX_CHUNK_FRAMES * MAX_CHANNELS * sizeof(int32_t));
    /* Only used for a wide (both-sides-S24_LE) crossfade blend -- see
     * mix_crossfade_s32()'s own caller below. Idle the rest of the time,
     * same as buf_cur_s32 is idle outside the plain wide-playback path. */
    buf_next_s32 = malloc((size_t) MAX_CHUNK_FRAMES * MAX_CHANNELS * sizeof(int32_t));
    buf_out_s32 = malloc((size_t) MAX_CHUNK_FRAMES * MAX_CHANNELS * sizeof(int32_t));
#endif

#ifndef HOST_BUILD
    /* Prime the ALSA audio device open-and-close once on startup so the codec
     * driver enters a safe low-power bias state, preventing jack insertion/removal pops
     * before the first track plays. */
    if (ensure_device(2, 44100)) close_device();
#endif

    for (;;) {
        /* Idle until there's a track to (re)start. */
        pthread_mutex_lock(&audio_mutex);
        while (!restart_requested) {
            pthread_cond_wait(&audio_cond, &audio_mutex);
        }
        restart_requested = false;
        free(cur_path_local);
        cur_path_local = restart_path; restart_path = NULL; /* ownership transferred */
        double start_seconds = restart_start_seconds;
        cur_replaygain_linear = restart_replaygain_linear;
        cur_replaygain_applied = restart_replaygain_applied;
        uint64_t cur_generation = playback_generation;
        pthread_mutex_unlock(&audio_mutex);

        close_decoder_if_open(&nxt_dec, &nxt_open);
        nxt_format_matches = false;
        if (cur_open) { decoder_close(&cur_dec); cur_open = false; }

        if (!cur_path_local || !decoder_open(&cur_dec, cur_path_local)) {
            if (cur_path_local) fprintf(stderr, "audio: failed to open '%s'\n", cur_path_local);
            pthread_mutex_lock(&audio_mutex);
            have_current = false;
            clear_current_format_locked();
            last_playback_error = AUDIO_ERROR_DECODER_FAILED;
            last_playback_error_generation = cur_generation;
            pthread_mutex_unlock(&audio_mutex);
            continue;
        }
        cur_open = true;
        cur_frames_played_local = 0;
        bool initial_mp3_seek_deferred = false;

        /* Proactively request an index for every eligible MP3 right at open,
         * not just when resuming at a non-zero position -- a cache hit (see
         * mp3_seek_cache_load()) resolves instantly, and even a fresh scan is
         * usually done well before the user taps the progress bar, since it
         * runs on its own thread and never blocks playback starting at frame
         * 0 here. No defer/hold for this fire-and-forget request -- only an
         * actual seek target (below, or a later interactive seek) holds
         * output waiting for it. */
        if (mp3_needs_seek_index(&cur_dec)) {
            pthread_mutex_lock(&audio_mutex);
            request_mp3_seek_index_locked(&cur_dec, cur_path_local, cur_generation);
            pthread_mutex_unlock(&audio_mutex);
        }

        if (!cur_dec.net_stream && isfinite(start_seconds) && start_seconds > 0.0) {
            double bounded_seconds = start_seconds;
            double duration_seconds = (double) cur_dec.total_frames / (double) cur_dec.sample_rate;
            if (bounded_seconds > duration_seconds) bounded_seconds = duration_seconds;
            uint64_t start_frame = (uint64_t) (bounded_seconds * (double) cur_dec.sample_rate);
            if (mp3_needs_seek_index(&cur_dec) && !cur_dec.mp3_seek_points && !cur_dec.mp3_seek_index_attempted) {
                pthread_mutex_lock(&audio_mutex);
                defer_mp3_seek_locked(&cur_dec, cur_generation, start_frame);
                pthread_mutex_unlock(&audio_mutex);
                initial_mp3_seek_deferred = true;
            }
            if (initial_mp3_seek_deferred) {
                DBG_LOG("audio: initial MP3 seek deferred until index is ready (%s)\n",
                        safe_path_tail(cur_path_local));
            } else if (decoder_seek(&cur_dec, start_frame)) {
                /* Once an index attempt has resolved either way,
                 * decoder_seek() proceeds via the table if one was bound, or
                 * brute force as a last resort if indexing permanently
                 * failed -- never silently skipped (see its own comment). */
                cur_frames_played_local = start_frame;
            } else {
                DBG_LOG("audio: initial seek to frame %" PRIu64 " failed (%s), playing from start\n",
                        start_frame, safe_path_tail(cur_path_local));
                /* Seeking can mutate a decoder before reporting failure
                 * (notably AAC and Opus). A clean reopen makes the promised
                 * start-from-zero fallback real and prevents a bad persisted
                 * resume position from crashing again on every boot. */
                if (!reopen_decoder_at(&cur_dec, cur_path_local, 0)) {
                    cur_open = false;
                    pthread_mutex_lock(&audio_mutex);
                    have_current = false;
                    clear_current_format_locked();
                    last_playback_error = AUDIO_ERROR_DECODER_FAILED;
                    last_playback_error_generation = cur_generation;
                    pthread_mutex_unlock(&audio_mutex);
                    continue;
                }
                cur_frames_played_local = 0;
            }
        }

#ifndef HOST_BUILD
        /* Reset S24 probe cache before ensuring format so each new track
         * can negotiate S24_LE output. */
        audio_output_reset_s24_probe();
        bool initial_wide = can_use_wide_path(&cur_dec);
        if (!ensure_device_format(cur_dec.channels, cur_dec.sample_rate, initial_wide)) {
#else
        if (!ensure_device(cur_dec.channels, cur_dec.sample_rate)) {
#endif
            decoder_close(&cur_dec);
            cur_open = false;
            pthread_mutex_lock(&audio_mutex);
            have_current = false;
            clear_current_format_locked();
            last_playback_error = AUDIO_ERROR_OUTPUT_FAILED;
            last_playback_error_generation = cur_generation;
            pthread_mutex_unlock(&audio_mutex);
            continue;
        }
#ifndef HOST_BUILD
        if (initial_mp3_seek_deferred) close_device();
#endif

        pthread_mutex_lock(&audio_mutex);
        have_current = true;
        frames_played = cur_frames_played_local;
        current_total_frames = cur_dec.total_frames;
        current_sample_rate = cur_dec.sample_rate;
        publish_current_format_locked(&cur_dec, cur_path_local,
                                      cur_replaygain_linear, cur_replaygain_applied);
        pthread_mutex_unlock(&audio_mutex);

        bool should_restart = false;
        bool was_stopped = false;
        bool ended_with_no_next = false;
        bool need_fade_in = true;
        bool mp3_seek_output_held = initial_mp3_seek_deferred;
        unsigned int consecutive_decoder_errors = 0;
        unsigned int consecutive_nxt_decoder_errors = 0;

        for (;;) {
            pthread_mutex_lock(&audio_mutex);
#ifndef HOST_BUILD
            /* Close the audio output device while paused so the analog amplifier
             * is de-energized, preventing audible pops on jack insertion/removal.
             * Output is automatically reopened via ensure_device() upon resume. */
            if (paused && !stop_requested && !restart_requested) {
                float vol = volume_gain;
                pthread_mutex_unlock(&audio_mutex);
                if (!mp3_seek_output_held && cur_open && cur_dec.sample_rate > 0) {
                    uint64_t rf = calculate_ramp_frames(cur_dec.sample_rate);
                    decoder_read_result_t r_fade = decoder_read_s16(&cur_dec, rf, buf_cur);
                    if (r_fade.frames > 0) {
                        apply_gain(buf_cur, (size_t) r_fade.frames * cur_dec.channels, cur_replaygain_linear);
                        peq_process(buf_cur, (size_t) r_fade.frames, (int) cur_dec.channels, cur_dec.sample_rate);
                        apply_gain(buf_cur, (size_t) r_fade.frames * cur_dec.channels, vol);
                        apply_ramp(buf_cur, r_fade.frames, cur_dec.channels, 1.0f, 0.0f);
                        uint64_t delivered = 0;
                        write_device_transition_ramp(buf_cur, r_fade.frames, cur_dec.channels,
                                                     cur_dec.sample_rate, cur_path_local, &delivered);
                        cur_frames_played_local += delivered;
                    }
                }
                close_device();
                need_fade_in = true;
                pthread_mutex_lock(&audio_mutex);
            }
#endif
            while (paused && !stop_requested && !restart_requested && !seek_pending &&
                   !mp3_index_result) {
                pthread_cond_wait(&audio_cond, &audio_mutex);
            }
            bool do_stop = stop_requested;
            bool do_restart = restart_requested;
            bool do_seek = false;
            uint64_t seek_frame = 0;

            mp3_index_job_t * completed_index = mp3_index_result;
            mp3_index_result = NULL;
            if (completed_index && completed_index->generation == cur_generation &&
                cur_dec.type == DECODER_MP3) {
                bool adopted = completed_index->outcome == MP3_INDEX_READY &&
                    drmp3_bind_seek_table(cur_dec.as.mp3, completed_index->count,
                                          completed_index->points);
                if (adopted) {
                    cur_dec.mp3_seek_points = completed_index->points;
                    cur_dec.mp3_seek_point_count = completed_index->count;
                    completed_index->points = NULL;
                    cur_dec.mp3_seek_index_attempted = true;
                    if (mp3_seek_deferred &&
                        mp3_seek_deferred_generation == cur_generation) {
                        do_seek = true;
                        seek_frame = mp3_seek_deferred_frame;
                    }
                } else {
                    /* Retry/exhaustion bookkeeping applies to every worker
                     * failure, not only ones with a seek waiting on them --
                     * an eager (non-deferred) scan that fails the same way
                     * must still count toward MP3_SEEK_RETRY_MAX, or nothing
                     * ever sets mp3_seek_index_attempted and the eager-retry
                     * branch below re-spawns a fresh worker every chunk. */
                    bool exhausted = !(completed_index->outcome == MP3_INDEX_TRANSIENT_FAILURE &&
                                       ++mp3_seek_retry_count < MP3_SEEK_RETRY_MAX);
                    if (!exhausted) {
                        mp3_seek_retry_after_ms = monotonic_ms() + MP3_SEEK_RETRY_DELAY_MS;
                    } else {
                        cur_dec.mp3_seek_index_attempted = true;
                        /* Giving up on indexing must not also give up on
                         * the seek itself -- decoder_seek() now permits a
                         * brute-force fallback once attempted is true
                         * (see its own comment), so apply the deferred
                         * target through the normal do_seek path below
                         * instead of silently abandoning it. */
                        if (mp3_seek_deferred && mp3_seek_deferred_generation == cur_generation) {
                            do_seek = true;
                            seek_frame = mp3_seek_deferred_frame;
                        }
                        finish_mp3_deferred_seek_locked();
                    }
                }
            }

            if (seek_pending && seek_pending_playback_generation == cur_generation) {
                do_seek = true;
                seek_frame = seek_pending_is_percent
                    ? (uint64_t) ((double) cur_dec.total_frames * (seek_pending_percent / 100.0))
                    : seek_pending_frame;
            }
            if (seek_pending) seek_pending = false;

            if (do_seek && seek_frame > 0 && mp3_needs_seek_index(&cur_dec) &&
                !cur_dec.mp3_seek_points && !cur_dec.mp3_seek_index_attempted) {
                /* Index attempt still outstanding -- hold this seek until it
                 * resolves instead of racing it with a brute-force seek.
                 * Once mp3_seek_index_attempted is true (below, or a prior
                 * permanent failure), decoder_seek() itself now falls back to
                 * brute force rather than this ever silently dropping a tap
                 * (see decoder_seek()'s own comment). */
                defer_mp3_seek_locked(&cur_dec, cur_generation, seek_frame);
                request_mp3_seek_index_locked(&cur_dec, cur_path_local, cur_generation);
                /* A hard setup failure (allocation or thread-create) inside
                 * the call above can immediately exhaust the retry budget
                 * and flip mp3_seek_index_attempted true right here -- when
                 * that happens, apply this seek via brute force now instead
                 * of dropping it (do_seek/seek_frame already hold the right
                 * target); otherwise hold as usual while the worker runs. */
                do_seek = cur_dec.mp3_seek_index_attempted;
            } else if (!do_seek && mp3_seek_deferred &&
                       mp3_seek_deferred_generation == cur_generation &&
                       !mp3_index_worker_active && !cur_dec.mp3_seek_index_attempted &&
                       mp3_needs_seek_index(&cur_dec)) {
                uint64_t retry_deferred_frame = mp3_seek_deferred_frame;
                request_mp3_seek_index_locked(&cur_dec, cur_path_local, cur_generation);
                if (cur_dec.mp3_seek_index_attempted) {
                    /* Same give-up-must-not-drop-the-seek fix as above, for
                     * the ongoing-retry path rather than the just-deferred
                     * one. */
                    do_seek = true;
                    seek_frame = retry_deferred_frame;
                }
            } else if (!do_seek && !mp3_seek_deferred && !cur_dec.mp3_seek_index_attempted &&
                       !cur_dec.mp3_seek_points && mp3_needs_seek_index(&cur_dec)) {
                /* Eager (non-blocking) retry for the proactive request made
                 * at track-open/promotion time: that single attempt no-ops
                 * rather than queuing a second job if the previous track's
                 * worker or unconsumed result was still occupying the one
                 * scan slot (request_mp3_seek_index_locked()'s own busy
                 * guard). Retrying here every chunk until it actually
                 * queues a job, or attempted/points become set, costs only
                 * a few field checks under a mutex already held. */
                request_mp3_seek_index_locked(&cur_dec, cur_path_local, cur_generation);
            }
            bool hold_for_mp3_seek = mp3_seek_deferred &&
                                     mp3_seek_deferred_generation == cur_generation &&
                                     !do_seek;
            bool xfade_on = crossfade_enabled;
            float vol = volume_gain;
            bool remain_paused = paused;
            uint64_t chunk_frames = low_power_mode ? LOW_POWER_CHUNK_FRAMES : NORMAL_CHUNK_FRAMES;

            /* Phase 3: Stack-buffered owned snapshot of next track metadata.
             * Completely eliminates steady-state heap allocations during playback. */
            next_track_snapshot_t staged_next = {0};
            if (next_path != NULL) {
                size_t len = strlen(next_path);
                if (len < sizeof(staged_next.path)) {
                    memcpy(staged_next.path, next_path, len + 1);
                    staged_next.valid = true;
                    staged_next.replaygain_linear = next_replaygain_linear;
                    staged_next.replaygain_applied = next_replaygain_applied;
                    staged_next.generation = next_track_generation;
                }
            }
            const char * staged_next_path = staged_next.valid ? staged_next.path : NULL;
            float staged_next_replaygain = staged_next.replaygain_linear;
            bool staged_next_replaygain_applied = staged_next.replaygain_applied;
            uint64_t staged_next_generation = staged_next.generation;
            pthread_mutex_unlock(&audio_mutex);
            free_mp3_index_job(completed_index);

#ifndef HOST_BUILD
            /* Check device format and output target every chunk so mid-track route changes
             * (such as Bluetooth connection/disconnection) take effect immediately. */
            if (!hold_for_mp3_seek) {
                bool is_blending = (xfade_on && staged_next_path != NULL && nxt_open && nxt_format_matches);
                /* Request S24_LE if both sides of a crossfade are S24_LE-eligible,
                 * avoiding unnecessary close+reopen cycles. */
                bool blend_can_be_wide = is_blending && can_use_wide_crossfade(&cur_dec, &nxt_dec);
                bool use_wide = blend_can_be_wide || (!is_blending && can_use_wide_path(&cur_dec));
                ensure_device_format(cur_dec.channels, cur_dec.sample_rate, use_wide);
                /* use_wide is only the REQUEST; audio_output_is_s24_active() (checked
                 * right after ensure_device_format() actually ran) is the ground truth
                 * -- see its own doc comment for why these can disagree (route race,
                 * hw_params negotiation failure). The decode/process/write branch
                 * decision below re-derives this same combined check independently
                 * rather than trusting this one, since a route change could still land
                 * in the gap between here and there; this copy only drives the display
                 * value. */
                pthread_mutex_lock(&audio_mutex);
                if (current_format_info.valid) {
                    current_format_info.output_bit_depth = (use_wide && audio_output_is_s24_active()) ? 24 : 16;
                }
                pthread_mutex_unlock(&audio_mutex);
            }
#endif

            if (do_restart || do_stop) {
                /* Phase 4: Controlled transition ramp-down on manual stop/restart */
                if (!mp3_seek_output_held && cur_open && cur_dec.sample_rate > 0) {
                    uint64_t rf = calculate_ramp_frames(cur_dec.sample_rate);
                    decoder_read_result_t r_fade = decoder_read_s16(&cur_dec, rf, buf_cur);
                    if (r_fade.frames > 0) {
                        apply_gain(buf_cur, (size_t) r_fade.frames * cur_dec.channels, cur_replaygain_linear);
                        peq_process(buf_cur, (size_t) r_fade.frames, (int) cur_dec.channels, cur_dec.sample_rate);
                        apply_gain(buf_cur, (size_t) r_fade.frames * cur_dec.channels, vol);
                        apply_ramp(buf_cur, r_fade.frames, cur_dec.channels, 1.0f, 0.0f);
#ifndef HOST_BUILD
                        uint64_t delivered = 0;
                        write_device_transition_ramp(buf_cur, r_fade.frames, cur_dec.channels,
                                                     cur_dec.sample_rate, cur_path_local, &delivered);
#else
                        write_device(buf_cur, r_fade.frames, cur_dec.channels);
#endif
                    }
                }
                if (do_restart) should_restart = true;
                if (do_stop) was_stopped = true;
                break;
            }

            if (hold_for_mp3_seek) {
                if (!mp3_seek_output_held) {
                    close_decoder_if_open(&nxt_dec, &nxt_open);
                    nxt_format_matches = false;
#ifndef HOST_BUILD
                    close_device();
#endif
                    mp3_seek_output_held = true;
                    need_fade_in = true;
                }
                /* Worker completion and transport commands signal audio_cond.
                 * The timeout only services a delayed transient retry. */
                struct timespec wake;
                clock_gettime(CLOCK_REALTIME, &wake);
                wake.tv_nsec += 100000000L;
                if (wake.tv_nsec >= 1000000000L) {
                    wake.tv_sec++;
                    wake.tv_nsec -= 1000000000L;
                }
                pthread_mutex_lock(&audio_mutex);
                if (mp3_seek_deferred &&
                    mp3_seek_deferred_generation == cur_generation &&
                    !stop_requested && !restart_requested && !seek_pending &&
                    !mp3_index_result)
                    pthread_cond_timedwait(&audio_cond, &audio_mutex, &wake);
                pthread_mutex_unlock(&audio_mutex);
                continue;
            }

            /* A permanent failure or exhausted transient retry resumes from
             * the confirmed decoder cursor, with the same fade-in used after
             * a successful seek. */
            if (remain_paused && !do_seek)
                continue;
            if (mp3_seek_output_held && !do_seek)
                mp3_seek_output_held = false;

            if (do_seek) {
                /* Network decoders are forward-only. Keep this guard at the
                 * owner thread as well as the public API: a seek may be
                 * queued while a newly requested stream is still opening,
                 * before current_format_info identifies it as a stream. */
                if (cur_dec.net_stream) {
                    DBG_LOG("audio: ignoring seek on network stream (%s)\n",
                            safe_path_tail(cur_path_local));
                    continue;
                }

                /* Smooth seek on the playback-owned decoder. Indexed MP3s
                 * reach this point only after their bounded table is ready. */
                if (!mp3_seek_output_held && cur_open && cur_dec.sample_rate > 0) {
                    uint64_t rf = calculate_ramp_frames(cur_dec.sample_rate);
                    decoder_read_result_t r_fade = decoder_read_s16(&cur_dec, rf, buf_cur);
                    if (r_fade.frames > 0) {
                        apply_gain(buf_cur, (size_t) r_fade.frames * cur_dec.channels, cur_replaygain_linear);
                        peq_process(buf_cur, (size_t) r_fade.frames, (int) cur_dec.channels, cur_dec.sample_rate);
                        apply_gain(buf_cur, (size_t) r_fade.frames * cur_dec.channels, vol);
                        apply_ramp(buf_cur, r_fade.frames, cur_dec.channels, 1.0f, 0.0f);
#ifndef HOST_BUILD
                        uint64_t delivered = 0;
                        write_device_transition_ramp(buf_cur, r_fade.frames, cur_dec.channels,
                                                     cur_dec.sample_rate, cur_path_local, &delivered);
#else
                        write_device(buf_cur, r_fade.frames, cur_dec.channels);
#endif
                    }
                }

                close_decoder_if_open(&nxt_dec, &nxt_open);
                nxt_format_matches = false;
                if (seek_frame > cur_dec.total_frames) seek_frame = cur_dec.total_frames;
                if (decoder_seek(&cur_dec, seek_frame)) {
                    cur_frames_played_local = seek_frame;
                    pthread_mutex_lock(&audio_mutex);
                    frames_played = cur_frames_played_local;
                    if (mp3_seek_deferred_generation == cur_generation)
                        finish_mp3_deferred_seek_locked();
                    pthread_mutex_unlock(&audio_mutex);
                    mp3_seek_output_held = remain_paused;
                    need_fade_in = true;
                    consecutive_decoder_errors = 0;
                } else {
                    DBG_LOG("audio: seek to frame %" PRIu64 " failed (%s)\n",
                            seek_frame, safe_path_tail(cur_path_local));
                    pthread_mutex_lock(&audio_mutex);
                    if (mp3_seek_deferred_generation == cur_generation)
                        finish_mp3_deferred_seek_locked();
                    pthread_mutex_unlock(&audio_mutex);
                    /* Some codecs reset internal state before they can know
                     * the seek will fail (AAC closes/recreates its handle,
                     * Opus moves the demux cursor before discarding). Restore
                     * the last confirmed position sequentially so playback
                     * never continues through a half-mutated decoder. */
                    if (!reopen_decoder_at(&cur_dec, cur_path_local,
                                           cur_frames_played_local)) {
                        cur_open = false;
                        pthread_mutex_lock(&audio_mutex);
                        last_playback_error = AUDIO_ERROR_DECODER_FAILED;
                        last_playback_error_generation = cur_generation;
                        have_current = false;
                        clear_current_format_locked();
                        paused = false;
                        pthread_mutex_unlock(&audio_mutex);
                        should_restart = false;
                        was_stopped = false;
                        ended_with_no_next = false;
                        goto inner_loop_done;
                    }
                    cur_open = true;
                    mp3_seek_output_held = remain_paused;
                    need_fade_in = true;
                    consecutive_decoder_errors = 0;
                }
                continue; /* re-check pause/restart state before decoding */
            }

            uint64_t frames_remaining = (cur_dec.total_frames > cur_frames_played_local)
                ? cur_dec.total_frames - cur_frames_played_local : 0;
            uint64_t crossfade_frames = (uint64_t) (CROSSFADE_SECONDS * (double) cur_dec.sample_rate);
            bool in_blend_window = xfade_on && staged_next_path != NULL &&
                                   frames_remaining > 0 && frames_remaining <= crossfade_frames;

            if (in_blend_window && !nxt_open) {
                if (decoder_open(&nxt_dec, staged_next_path)) {
                    nxt_open = true;
                    nxt_frames_consumed = 0;
                    nxt_replaygain_linear_local = staged_next_replaygain;
                    nxt_replaygain_applied_local = staged_next_replaygain_applied;
                    nxt_format_matches = (nxt_dec.channels == cur_dec.channels && nxt_dec.sample_rate == cur_dec.sample_rate);
                } else {
                    DBG_LOG("audio: failed to prefetch next track (%s)\n",
                            safe_path_tail(staged_next_path));
                }
            }

            if (in_blend_window && nxt_open && nxt_format_matches) {
                unsigned int channels = cur_dec.channels;
                uint64_t want = (frames_remaining < chunk_frames) ? frames_remaining : chunk_frames;

#ifndef HOST_BUILD
                if (can_use_wide_crossfade(&cur_dec, &nxt_dec) && audio_output_is_s24_active()) {
                    /* Both sides of this crossfade qualify for S24_LE and the
                     * device is active at S24_LE -- blend in the wide (int32_t) domain. */
                    decoder_read_result_t r_cur = decoder_read_s32(&cur_dec, want, buf_cur_s32);
                    uint64_t n_cur = r_cur.frames;

                    if (r_cur.status == DECODER_READ_FATAL_ERROR) {
                        DBG_LOG("audio: crossfade fatal decode error (%s)\n", safe_path_tail(cur_path_local));
                        close_decoder_if_open(&nxt_dec, &nxt_open);
                        nxt_format_matches = false;
                        pthread_mutex_lock(&audio_mutex);
                        last_playback_error = AUDIO_ERROR_DECODER_FAILED;
                        last_playback_error_generation = cur_generation;
                        have_current = false;
                        clear_current_format_locked();
                        paused = false;
                        pthread_mutex_unlock(&audio_mutex);
                        should_restart = false;
                        was_stopped = false;
                        ended_with_no_next = false;
                        goto inner_loop_done;
                    }

                    /* Handle recoverable decode errors using consecutive error counts,
                     * matching the plain-playback retry path rather than prematurely
                     * ending the crossfade window. */
                    if (r_cur.status == DECODER_READ_RECOVERABLE_ERROR) {
                        consecutive_decoder_errors++;
                        DBG_LOG("audio: crossfade recoverable decode error #%u (%s)\n",
                                consecutive_decoder_errors, safe_path_tail(cur_path_local));
                        if (consecutive_decoder_errors >= 10) {
                            DBG_LOG("audio: crossfade consecutive recoverable errors exceeded limit (%s)\n",
                                    safe_path_tail(cur_path_local));
                            close_decoder_if_open(&nxt_dec, &nxt_open);
                            nxt_format_matches = false;
                            pthread_mutex_lock(&audio_mutex);
                            last_playback_error = AUDIO_ERROR_DECODER_FAILED;
                            last_playback_error_generation = cur_generation;
                            have_current = false;
                            clear_current_format_locked();
                            paused = false;
                            pthread_mutex_unlock(&audio_mutex);
                            should_restart = false;
                            was_stopped = false;
                            ended_with_no_next = false;
                            goto inner_loop_done;
                        }
                        continue;
                    } else if (r_cur.status == DECODER_READ_OK) {
                        consecutive_decoder_errors = 0;
                    }

                    if (n_cur == 0 && frames_remaining > 0 && r_cur.status == DECODER_READ_EOF) {
                        bool is_stream = (cur_dec.net_stream != NULL);
                        if (is_premature_eof(cur_frames_played_local, cur_dec.total_frames, is_stream)) {
                            bool recovered = false;
                            for (int dr = 0; dr < 3; dr++) {
                                pthread_mutex_lock(&audio_mutex);
                                bool abort = stop_requested || restart_requested;
                                pthread_mutex_unlock(&audio_mutex);
                                if (abort) break;

                                DBG_LOG("audio: crossfade premature EOF at frame %" PRIu64 "/%" PRIu64
                                        ", reopen attempt %d (%s)\n",
                                        cur_frames_played_local, cur_dec.total_frames, dr + 1,
                                        safe_path_tail(cur_path_local));
                                usleep(50000);

                                if (!reopen_decoder_at(&cur_dec, cur_path_local, cur_frames_played_local)) {
                                    DBG_LOG("audio: crossfade decoder reopen failed on attempt %d\n", dr + 1);
                                    cur_open = false;
                                    continue;
                                }
                                cur_open = true;
                                r_cur = decoder_read_s32(&cur_dec, want, buf_cur_s32);
                                n_cur = r_cur.frames;
                                if (n_cur > 0) { recovered = true; break; }
                            }
                            if (!recovered) {
                                DBG_LOG("audio: crossfade decoder recovery exhausted (%s)\n",
                                        safe_path_tail(cur_path_local));
                                close_decoder_if_open(&nxt_dec, &nxt_open);
                                nxt_format_matches = false;
                                pthread_mutex_lock(&audio_mutex);
                                last_playback_error = AUDIO_ERROR_DECODER_FAILED;
                                last_playback_error_generation = cur_generation;
                                have_current = false;
                                clear_current_format_locked();
                                paused = false;
                                pthread_mutex_unlock(&audio_mutex);
                                should_restart = false;
                                was_stopped = false;
                                ended_with_no_next = false;
                                goto inner_loop_done;
                            }
                        }
                    }

                    if (n_cur > 0) {
                        decoder_read_result_t r_next = decoder_read_s32(&nxt_dec, n_cur, buf_next_s32);
                        bool nxt_failed = false;

                        if (r_next.status == DECODER_READ_FATAL_ERROR || (r_next.status == DECODER_READ_EOF && r_next.frames == 0)) {
                            nxt_failed = true;
                        } else if (r_next.status == DECODER_READ_RECOVERABLE_ERROR) {
                            consecutive_nxt_decoder_errors++;
                            DBG_LOG("audio: next track recoverable decode error (%u/10) (%s)\n",
                                    consecutive_nxt_decoder_errors, safe_path_tail(staged_next_path));
                            if (consecutive_nxt_decoder_errors >= 10) {
                                DBG_LOG("audio: next track consecutive recoverable errors exceeded limit, cancelling blend (%s)\n",
                                        safe_path_tail(staged_next_path));
                                nxt_failed = true;
                            }
                        } else if (r_next.status == DECODER_READ_OK) {
                            consecutive_nxt_decoder_errors = 0;
                        }

                        if (nxt_failed) {
                            DBG_LOG("audio: next track crossfade decode failed (status=%d), cancelling crossfade (%s)\n",
                                    (int) r_next.status, safe_path_tail(staged_next_path));
                            close_decoder_if_open(&nxt_dec, &nxt_open);
                            nxt_format_matches = false;
                            consecutive_nxt_decoder_errors = 0;

                            /* IMPORTANT: Do NOT discard buf_cur_s32! Output the decoded frames of
                             * cur_dec as unblended audio and advance cur_frames_played_local to
                             * prevent skips and maintain accurate timeline sync. buf_cur (s16) is
                             * idle in this wide branch -- reused as write_device_with_retry_s32()'s
                             * narrow-fallback scratch buffer, see that function's own comment. */
                            apply_gain_s32(buf_cur_s32, (size_t) n_cur * channels, cur_replaygain_linear);
                            peq_process_s32(buf_cur_s32, (size_t) n_cur, (int) channels, cur_dec.sample_rate);
                            apply_gain_s32(buf_cur_s32, (size_t) n_cur * channels, vol);

                            if (need_fade_in) {
                                uint64_t rf = calculate_ramp_frames(cur_dec.sample_rate);
                                uint64_t in_frames = (n_cur < rf) ? n_cur : rf;
                                apply_ramp_s32(buf_cur_s32, in_frames, channels, 0.0f, 1.0f);
                                need_fade_in = false;
                            }

                            uint64_t delivered = 0;
                            write_result_t wr = write_device_with_retry_s32(buf_cur_s32, n_cur, channels,
                                                                             cur_dec.sample_rate, cur_path_local,
                                                                             buf_cur, &delivered);
                            cur_frames_played_local += delivered;
                            frames_remaining -= (delivered < frames_remaining) ? delivered : frames_remaining;
                            pthread_mutex_lock(&audio_mutex);
                            frames_played = cur_frames_played_local;
                            pthread_mutex_unlock(&audio_mutex);

                            if (wr == WRITE_RESULT_ABORTED) {
                                continue;
                            } else if (wr == WRITE_RESULT_FAILED) {
                                DBG_LOG("audio: crossfade fallback output failure (%s)\n", safe_path_tail(cur_path_local));
                                pthread_mutex_lock(&audio_mutex);
                                last_playback_error = AUDIO_ERROR_OUTPUT_FAILED;
                                last_playback_error_generation = cur_generation;
                                have_current = false;
                                clear_current_format_locked();
                                paused = false;
                                pthread_mutex_unlock(&audio_mutex);
                                should_restart = false;
                                was_stopped = false;
                                ended_with_no_next = false;
                                goto inner_loop_done;
                            }
                            continue;
                        }

                        uint64_t n_next = r_next.frames;
                        nxt_frames_consumed += n_next;
                        if (n_next < n_cur) {
                            memset(buf_next_s32 + (size_t) n_next * channels, 0, (size_t) (n_cur - n_next) * channels * sizeof(int32_t));
                        }

                        apply_gain_s32(buf_cur_s32, (size_t) n_cur * channels, cur_replaygain_linear);
                        apply_gain_s32(buf_next_s32, (size_t) n_cur * channels, nxt_replaygain_linear_local);

                        uint64_t fade_start_frame = crossfade_frames - frames_remaining;
                        mix_crossfade_s32(buf_cur_s32, buf_next_s32, buf_out_s32, n_cur, channels, fade_start_frame, crossfade_frames);

                        peq_process_s32(buf_out_s32, (size_t) n_cur, (int) channels, cur_dec.sample_rate);
                        apply_gain_s32(buf_out_s32, (size_t) n_cur * channels, vol);

                        if (need_fade_in) {
                            uint64_t rf = calculate_ramp_frames(cur_dec.sample_rate);
                            uint64_t in_frames = (n_cur < rf) ? n_cur : rf;
                            apply_ramp_s32(buf_out_s32, in_frames, channels, 0.0f, 1.0f);
                            need_fade_in = false;
                        }

                        uint64_t delivered = 0;
                        write_result_t wr = write_device_with_retry_s32(buf_out_s32, n_cur, channels,
                                                                         cur_dec.sample_rate, cur_path_local,
                                                                         buf_out, &delivered);
                        cur_frames_played_local += delivered;
                        frames_remaining -= (delivered < frames_remaining) ? delivered : frames_remaining;
                        pthread_mutex_lock(&audio_mutex);
                        frames_played = cur_frames_played_local;
                        pthread_mutex_unlock(&audio_mutex);

                        if (wr == WRITE_RESULT_ABORTED) {
                            continue;
                        } else if (wr == WRITE_RESULT_FAILED) {
                            DBG_LOG("audio: crossfade output failure, abandoning blend (%s)\n",
                                    safe_path_tail(cur_path_local));
                            close_decoder_if_open(&nxt_dec, &nxt_open);
                            nxt_format_matches = false;
                            pthread_mutex_lock(&audio_mutex);
                            last_playback_error = AUDIO_ERROR_OUTPUT_FAILED;
                            last_playback_error_generation = cur_generation;
                            have_current = false;
                            clear_current_format_locked();
                            paused = false;
                            pthread_mutex_unlock(&audio_mutex);
                            should_restart = false;
                            was_stopped = false;
                            ended_with_no_next = false;
                            goto inner_loop_done;
                        }

                        if (frames_remaining > 0) {
                            continue; /* more of cur left in the window, keep blending */
                        }
                    }
                } else
#endif
                {
                decoder_read_result_t r_cur = decoder_read_s16(&cur_dec, want, buf_cur);
                uint64_t n_cur = r_cur.frames;

                if (r_cur.status == DECODER_READ_FATAL_ERROR) {
                    DBG_LOG("audio: crossfade fatal decode error (%s)\n", safe_path_tail(cur_path_local));
                    close_decoder_if_open(&nxt_dec, &nxt_open);
                    nxt_format_matches = false;
                    pthread_mutex_lock(&audio_mutex);
                    last_playback_error = AUDIO_ERROR_DECODER_FAILED;
                    last_playback_error_generation = cur_generation;
                    have_current = false;
                    clear_current_format_locked();
                    paused = false;
                    pthread_mutex_unlock(&audio_mutex);
                    should_restart = false;
                    was_stopped = false;
                    ended_with_no_next = false;
                    goto inner_loop_done;
                }

                /* See the wide (S32) branch's identical comment above --
                 * this recoverable-error handling was missing from this
                 * narrow branch too (present in the plain-playback path,
                 * absent here), letting a transient decode hiccup fall
                 * through as if cur_dec had reached a legitimate EOF. */
                if (r_cur.status == DECODER_READ_RECOVERABLE_ERROR) {
                    consecutive_decoder_errors++;
                    DBG_LOG("audio: crossfade recoverable decode error #%u (%s)\n",
                            consecutive_decoder_errors, safe_path_tail(cur_path_local));
                    if (consecutive_decoder_errors >= 10) {
                        DBG_LOG("audio: crossfade consecutive recoverable errors exceeded limit (%s)\n",
                                safe_path_tail(cur_path_local));
                        close_decoder_if_open(&nxt_dec, &nxt_open);
                        nxt_format_matches = false;
                        pthread_mutex_lock(&audio_mutex);
                        last_playback_error = AUDIO_ERROR_DECODER_FAILED;
                        last_playback_error_generation = cur_generation;
                        have_current = false;
                        clear_current_format_locked();
                        paused = false;
                        pthread_mutex_unlock(&audio_mutex);
                        should_restart = false;
                        was_stopped = false;
                        ended_with_no_next = false;
                        goto inner_loop_done;
                    }
                    continue;
                } else if (r_cur.status == DECODER_READ_OK) {
                    consecutive_decoder_errors = 0;
                }

                if (n_cur == 0 && frames_remaining > 0 && r_cur.status == DECODER_READ_EOF) {
                    bool is_stream = (cur_dec.net_stream != NULL);
                    if (is_premature_eof(cur_frames_played_local, cur_dec.total_frames, is_stream)) {
                        bool recovered = false;
                        for (int dr = 0; dr < 3; dr++) {
                            pthread_mutex_lock(&audio_mutex);
                            bool abort = stop_requested || restart_requested;
                            pthread_mutex_unlock(&audio_mutex);
                            if (abort) break;

                            DBG_LOG("audio: crossfade premature EOF at frame %" PRIu64 "/%" PRIu64
                                    ", reopen attempt %d (%s)\n",
                                    cur_frames_played_local, cur_dec.total_frames, dr + 1,
                                    safe_path_tail(cur_path_local));
                            usleep(50000);

                            if (!reopen_decoder_at(&cur_dec, cur_path_local, cur_frames_played_local)) {
                                DBG_LOG("audio: crossfade decoder reopen failed on attempt %d\n", dr + 1);
                                cur_open = false;
                                continue;
                            }
                            cur_open = true;
                            r_cur = decoder_read_s16(&cur_dec, want, buf_cur);
                            n_cur = r_cur.frames;
                            if (n_cur > 0) { recovered = true; break; }
                        }
                        if (!recovered) {
                            DBG_LOG("audio: crossfade decoder recovery exhausted (%s)\n",
                                    safe_path_tail(cur_path_local));
                            close_decoder_if_open(&nxt_dec, &nxt_open);
                            nxt_format_matches = false;
                            pthread_mutex_lock(&audio_mutex);
                            last_playback_error = AUDIO_ERROR_DECODER_FAILED;
                            last_playback_error_generation = cur_generation;
                            have_current = false;
                            clear_current_format_locked();
                            paused = false;
                            pthread_mutex_unlock(&audio_mutex);
                            should_restart = false;
                            was_stopped = false;
                            ended_with_no_next = false;
                            goto inner_loop_done;
                        }
                    }
                }

                if (n_cur > 0) {
                    decoder_read_result_t r_next = decoder_read_s16(&nxt_dec, n_cur, buf_next);
                    bool nxt_failed = false;

                    if (r_next.status == DECODER_READ_FATAL_ERROR || (r_next.status == DECODER_READ_EOF && r_next.frames == 0)) {
                        nxt_failed = true;
                    } else if (r_next.status == DECODER_READ_RECOVERABLE_ERROR) {
                        consecutive_nxt_decoder_errors++;
                        DBG_LOG("audio: next track recoverable decode error (%u/10) (%s)\n",
                                consecutive_nxt_decoder_errors, safe_path_tail(staged_next_path));
                        if (consecutive_nxt_decoder_errors >= 10) {
                            DBG_LOG("audio: next track consecutive recoverable errors exceeded limit, cancelling blend (%s)\n",
                                    safe_path_tail(staged_next_path));
                            nxt_failed = true;
                        }
                    } else if (r_next.status == DECODER_READ_OK) {
                        consecutive_nxt_decoder_errors = 0;
                    }

                    if (nxt_failed) {
                        DBG_LOG("audio: next track crossfade decode failed (status=%d), cancelling crossfade (%s)\n",
                                (int) r_next.status, safe_path_tail(staged_next_path));
                        close_decoder_if_open(&nxt_dec, &nxt_open);
                        nxt_format_matches = false;
                        consecutive_nxt_decoder_errors = 0;

                        /* IMPORTANT: Do NOT discard buf_cur! Output the decoded frames of cur_dec
                         * as unblended audio and advance cur_frames_played_local to prevent skips
                         * and maintain accurate timeline sync. */
                        apply_gain(buf_cur, (size_t) n_cur * channels, cur_replaygain_linear);
                        peq_process(buf_cur, (size_t) n_cur, (int) channels, cur_dec.sample_rate);
                        apply_gain(buf_cur, (size_t) n_cur * channels, vol);

                        if (need_fade_in) {
                            uint64_t rf = calculate_ramp_frames(cur_dec.sample_rate);
                            uint64_t in_frames = (n_cur < rf) ? n_cur : rf;
                            apply_ramp(buf_cur, in_frames, channels, 0.0f, 1.0f);
                            need_fade_in = false;
                        }

#ifndef HOST_BUILD
                        uint64_t delivered = 0;
                        write_result_t wr = write_device_with_retry(buf_cur, n_cur, channels,
                                                                     cur_dec.sample_rate, cur_path_local,
                                                                     &delivered);
                        cur_frames_played_local += delivered;
                        frames_remaining -= (delivered < frames_remaining) ? delivered : frames_remaining;
                        pthread_mutex_lock(&audio_mutex);
                        frames_played = cur_frames_played_local;
                        pthread_mutex_unlock(&audio_mutex);

                        if (wr == WRITE_RESULT_ABORTED) {
                            continue;
                        } else if (wr == WRITE_RESULT_FAILED) {
                            DBG_LOG("audio: crossfade fallback output failure (%s)\n", safe_path_tail(cur_path_local));
                            pthread_mutex_lock(&audio_mutex);
                            last_playback_error = AUDIO_ERROR_OUTPUT_FAILED;
                            last_playback_error_generation = cur_generation;
                            have_current = false;
                            clear_current_format_locked();
                            paused = false;
                            pthread_mutex_unlock(&audio_mutex);
                            should_restart = false;
                            was_stopped = false;
                            ended_with_no_next = false;
                            goto inner_loop_done;
                        }
#else
                        write_device(buf_cur, n_cur, channels);
                        cur_frames_played_local += n_cur;
                        frames_remaining -= (n_cur < frames_remaining) ? n_cur : frames_remaining;
                        pthread_mutex_lock(&audio_mutex);
                        frames_played = cur_frames_played_local;
                        pthread_mutex_unlock(&audio_mutex);
#endif
                        continue;
                    }

                    uint64_t n_next = r_next.frames;
                    nxt_frames_consumed += n_next;
                    if (n_next < n_cur) {
                        memset(buf_next + (size_t) n_next * channels, 0, (size_t) (n_cur - n_next) * channels * sizeof(int16_t));
                    }

                    apply_gain(buf_cur, (size_t) n_cur * channels, cur_replaygain_linear);
                    apply_gain(buf_next, (size_t) n_cur * channels, nxt_replaygain_linear_local);

                    uint64_t fade_start_frame = crossfade_frames - frames_remaining;
                    mix_crossfade(buf_cur, buf_next, buf_out, n_cur, channels, fade_start_frame, crossfade_frames);

                    peq_process(buf_out, (size_t) n_cur, (int) channels, cur_dec.sample_rate);
                    apply_gain(buf_out, (size_t) n_cur * channels, vol);

                    if (need_fade_in) {
                        uint64_t rf = calculate_ramp_frames(cur_dec.sample_rate);
                        uint64_t in_frames = (n_cur < rf) ? n_cur : rf;
                        apply_ramp(buf_out, in_frames, channels, 0.0f, 1.0f);
                        need_fade_in = false;
                    }

#ifndef HOST_BUILD
                    uint64_t delivered = 0;
                    write_result_t wr = write_device_with_retry(buf_out, n_cur, channels,
                                                                 cur_dec.sample_rate, cur_path_local,
                                                                 &delivered);
                    cur_frames_played_local += delivered;
                    frames_remaining -= (delivered < frames_remaining) ? delivered : frames_remaining;
                    pthread_mutex_lock(&audio_mutex);
                    frames_played = cur_frames_played_local;
                    pthread_mutex_unlock(&audio_mutex);

                    if (wr == WRITE_RESULT_ABORTED) {
                        continue;
                    } else if (wr == WRITE_RESULT_FAILED) {
                        DBG_LOG("audio: crossfade output failure, abandoning blend (%s)\n",
                                safe_path_tail(cur_path_local));
                        close_decoder_if_open(&nxt_dec, &nxt_open);
                        nxt_format_matches = false;
                        pthread_mutex_lock(&audio_mutex);
                        last_playback_error = AUDIO_ERROR_OUTPUT_FAILED;
                        last_playback_error_generation = cur_generation;
                        have_current = false;
                        clear_current_format_locked();
                        paused = false;
                        pthread_mutex_unlock(&audio_mutex);
                        should_restart = false;
                        was_stopped = false;
                        ended_with_no_next = false;
                        goto inner_loop_done;
                    }
#else
                    write_device(buf_out, n_cur, channels);
                    cur_frames_played_local += n_cur;
                    frames_remaining -= n_cur;
                    pthread_mutex_lock(&audio_mutex);
                    frames_played = cur_frames_played_local;
                    pthread_mutex_unlock(&audio_mutex);
#endif

                    if (frames_remaining > 0) {
                        continue; /* more of cur left in the window, keep blending */
                    }
                }
                } /* end narrow (S16) crossfade branch */

                /* Blend window finished (cur fully consumed) -- promote next to current.
                 * Verify the snapshot's generation still matches what was armed --
                 * if not, another audio_set_next_track() replaced the queued track
                 * while we were in the crossfade window; discard the stale prefetch. */
                pthread_mutex_lock(&audio_mutex);
                bool snap_still_valid = (staged_next_generation == next_track_generation);
                if (snap_still_valid) {
                    decoder_close(&cur_dec);
                    cur_dec = nxt_dec;
#ifndef HOST_BUILD
                    /* Reset S24 probe cache on track promotion so the new track
                     * can negotiate S24_LE output. */
                    audio_output_reset_s24_probe();
#endif
                    nxt_open = false;
                    nxt_format_matches = false;
                    cur_frames_played_local = nxt_frames_consumed;
                    cur_replaygain_linear = nxt_replaygain_linear_local;
                    cur_replaygain_applied = nxt_replaygain_applied_local;
                    free(cur_path_local);
                    cur_path_local = next_path; next_path = NULL;
                    free(active_path);
                    active_path = cur_path_local ? strdup(cur_path_local) : NULL;
                    playback_generation++;
                    atomic_store_explicit(&mp3_index_active_generation, (unsigned int) playback_generation, memory_order_relaxed);
                    atomic_store_explicit(&mp3_index_stop_flag, false, memory_order_relaxed);
                    /* A deferred seek belongs to the track that just ended --
                     * leaving it set would block the new track's own eager
                     * index request (its "not deferred" retry branch) and
                     * misdirect the stale-generation deferred-retry branch. */
                    finish_mp3_deferred_seek_locked();
                    seek_pending = false;
                    cur_generation = playback_generation;
                    current_total_frames = cur_dec.total_frames;
                    current_sample_rate = cur_dec.sample_rate;
                    frames_played = cur_frames_played_local;
                    publish_current_format_locked(&cur_dec, cur_path_local,
                                                  cur_replaygain_linear, cur_replaygain_applied);
                    track_advanced = true;
                    if (mp3_needs_seek_index(&cur_dec))
                        request_mp3_seek_index_locked(&cur_dec, cur_path_local, cur_generation);
                    pthread_mutex_unlock(&audio_mutex);
                } else {
                    /* Stale prefetch: a newer next track was armed -- close
                     * nxt_dec and let the current track continue to its own EOF. */
                    pthread_mutex_unlock(&audio_mutex);
                    close_decoder_if_open(&nxt_dec, &nxt_open);
                    nxt_format_matches = false;
                    DBG_LOG("audio: stale crossfade prefetch discarded (%s)\n",
                            safe_path_tail(staged_next_path));
                }
                continue;
            }

            /* Not blending (crossfade off, not yet near the end, or the next
             * track's format doesn't match) -- plain single-source playback. */
#ifndef HOST_BUILD
            /* can_use_wide_path() alone is source-side eligibility only --
             * whether the device is ACTUALLY open as local S24_LE right now
             * can differ (a route change to Bluetooth/USB landed after this
             * chunk's own ensure_device_format() call above, or that call's
             * S24_LE negotiation failed and silently fell back to S16_LE;
             * see audio_output_is_s24_active()'s own doc comment). Requiring
             * both here, at the single point that decides whether this whole
             * chunk decodes/processes/writes through the s32 path, means a
             * stale or failed wide-path prediction falls through to the
             * ordinary s16 path below instead of reaching
             * audio_output_write_s24(), which would otherwise correctly
             * refuse the write (active_target != OUTPUT_TARGET_LOCAL) and
             * exhaust this chunk's retry budget into a hard playback failure. */
            if (can_use_wide_path(&cur_dec) && audio_output_is_s24_active()) {
                decoder_read_result_t r_cur = decoder_read_s32(&cur_dec, chunk_frames, buf_cur_s32);
                uint64_t n_cur = r_cur.frames;

                if (r_cur.status == DECODER_READ_RECOVERABLE_ERROR) {
                    consecutive_decoder_errors++;
                    DBG_LOG("audio: recoverable decoder error #%u at frame %" PRIu64 " (%s)\n",
                            consecutive_decoder_errors, cur_frames_played_local, safe_path_tail(cur_path_local));
                    if (consecutive_decoder_errors >= 10) {
                        DBG_LOG("audio: consecutive recoverable errors exceeded limit (%s)\n",
                                safe_path_tail(cur_path_local));
                        r_cur.status = DECODER_READ_FATAL_ERROR;
                    }
                } else if (r_cur.status == DECODER_READ_OK) {
                    consecutive_decoder_errors = 0;
                }

                if (r_cur.status == DECODER_READ_FATAL_ERROR) {
                    DBG_LOG("audio: fatal decoder error at frame %" PRIu64 "/%" PRIu64 " (%s)\n",
                            cur_frames_played_local, cur_dec.total_frames, safe_path_tail(cur_path_local));
                    pthread_mutex_lock(&audio_mutex);
                    last_playback_error = AUDIO_ERROR_DECODER_FAILED;
                    last_playback_error_generation = cur_generation;
                    have_current = false;
                    clear_current_format_locked();
                    paused = false;
                    pthread_mutex_unlock(&audio_mutex);
                    should_restart = false;
                    was_stopped = false;
                    ended_with_no_next = false;
                    goto inner_loop_done;
                }

                if (n_cur == 0 && r_cur.status == DECODER_READ_EOF) {
                    /* Zero-frame read: check whether this is a premature EOF for a
                     * finite local file, or a genuine (or live-stream) end. */
                    bool is_stream = (cur_dec.net_stream != NULL);
                    if (is_premature_eof(cur_frames_played_local, cur_dec.total_frames, is_stream)) {
                        /* Attempt bounded reopen+seek recovery */
                        bool recovered = false;
                        for (int dr = 0; dr < 3; dr++) {
                            pthread_mutex_lock(&audio_mutex);
                            bool abort = stop_requested || restart_requested;
                            pthread_mutex_unlock(&audio_mutex);
                            if (abort) break;

                            DBG_LOG("audio: premature EOF at frame %" PRIu64 "/%" PRIu64
                                    ", reopen attempt %d (%s)\n",
                                    cur_frames_played_local, cur_dec.total_frames, dr + 1,
                                    safe_path_tail(cur_path_local));
                            usleep(50000); /* 50 ms backoff */

                            if (!reopen_decoder_at(&cur_dec, cur_path_local,
                                                   cur_frames_played_local)) {
                                DBG_LOG("audio: decoder reopen failed on attempt %d\n", dr + 1);
                                cur_open = false; /* dec is now closed */
                                continue;
                            }
                            cur_open = true;
                            r_cur = decoder_read_s32(&cur_dec, chunk_frames, buf_cur_s32);
                            n_cur = r_cur.frames;
                            if (n_cur > 0) { recovered = true; break; }
                        }
                        if (!recovered) {
                            DBG_LOG("audio: decoder recovery exhausted at %" PRIu64 "/%" PRIu64
                                    " (%s)\n",
                                    cur_frames_played_local, cur_dec.total_frames,
                                    safe_path_tail(cur_path_local));
                            pthread_mutex_lock(&audio_mutex);
                            last_playback_error = AUDIO_ERROR_DECODER_FAILED;
                            last_playback_error_generation = cur_generation;
                            have_current = false;
                            clear_current_format_locked();
                            paused = false;
                            pthread_mutex_unlock(&audio_mutex);
                            should_restart = false;
                            was_stopped = false;
                            ended_with_no_next = false;
                            goto inner_loop_done;
                        }
                        /* n_cur > 0 now -- fall through to normal processing */
                    }

                    if (n_cur == 0) {
                        /* True EOF (or live stream end). If a next track is queued, hand
                         * off to it -- seamlessly if the format matches (device stays open),
                         * or with a brief reopen if it doesn't. */
                        if (!nxt_open && staged_next_path != NULL) {
                            if (decoder_open(&nxt_dec, staged_next_path)) {
                                nxt_open = true;
                                nxt_frames_consumed = 0;
                                nxt_replaygain_linear_local = staged_next_replaygain;
                                nxt_replaygain_applied_local = staged_next_replaygain_applied;
                            } else {
                                DBG_LOG("audio: failed to open next track (%s)\n",
                                        safe_path_tail(staged_next_path));
                            }
                        }

                        if (nxt_open) {
                            pthread_mutex_lock(&audio_mutex);
                            bool snap_still_valid = (staged_next_generation == next_track_generation);
                            if (!snap_still_valid) {
                                /* Stale next track: queued track was replaced while playing. Discard nxt_dec. */
                                pthread_mutex_unlock(&audio_mutex);
                                close_decoder_if_open(&nxt_dec, &nxt_open);
                                nxt_format_matches = false;
                                DBG_LOG("audio: stale gapless prefetch discarded (%s)\n",
                                        safe_path_tail(staged_next_path));
                                ended_with_no_next = true;
                                break;
                            }
                            pthread_mutex_unlock(&audio_mutex);

                            /* Gapless handoff to a different decoder -- see the
                             * initial-open call site's identical comment on why
                             * this must run before ensure_device_format(). */
                            audio_output_reset_s24_probe();
                            bool next_wide = can_use_wide_path(&nxt_dec);
                            bool device_ok = ensure_device_format(nxt_dec.channels, nxt_dec.sample_rate, next_wide);
                            decoder_close(&cur_dec);
                            cur_dec = nxt_dec;
                            cur_open = true;
                            nxt_open = false;
                            cur_frames_played_local = nxt_frames_consumed;
                            cur_replaygain_linear = nxt_replaygain_linear_local;
                            cur_replaygain_applied = nxt_replaygain_applied_local;
                            free(cur_path_local);

                            pthread_mutex_lock(&audio_mutex);
                            cur_path_local = next_path; next_path = NULL;
                            free(active_path);
                            active_path = cur_path_local ? strdup(cur_path_local) : NULL;
                            playback_generation++;
                            atomic_store_explicit(&mp3_index_active_generation, (unsigned int) playback_generation, memory_order_relaxed);
                            atomic_store_explicit(&mp3_index_stop_flag, false, memory_order_relaxed);
                            finish_mp3_deferred_seek_locked();
                            seek_pending = false;
                            cur_generation = playback_generation;
                            current_total_frames = cur_dec.total_frames;
                            current_sample_rate = cur_dec.sample_rate;
                            frames_played = cur_frames_played_local;
                            publish_current_format_locked(&cur_dec, cur_path_local,
                                                          cur_replaygain_linear, cur_replaygain_applied);
                            track_advanced = true;
                            if (mp3_needs_seek_index(&cur_dec))
                                request_mp3_seek_index_locked(&cur_dec, cur_path_local, cur_generation);
                            pthread_mutex_unlock(&audio_mutex);

                            if (!device_ok) { ended_with_no_next = true; break; }
                            continue;
                        }

                        ended_with_no_next = true;
                        break;
                    }
                }

                apply_gain_s32(buf_cur_s32, (size_t) n_cur * cur_dec.channels, cur_replaygain_linear);
                peq_process_s32(buf_cur_s32, (size_t) n_cur, (int) cur_dec.channels, cur_dec.sample_rate);
                apply_gain_s32(buf_cur_s32, (size_t) n_cur * cur_dec.channels, vol);

                if (need_fade_in && n_cur > 0) {
                    uint64_t rf = calculate_ramp_frames(cur_dec.sample_rate);
                    uint64_t in_frames = (n_cur < rf) ? n_cur : rf;
                    for (uint64_t i = 0; i < in_frames; i++) {
                        float t = (in_frames > 1) ? (float) i / (float) (in_frames - 1) : 1.0f;
                        for (unsigned int ch = 0; ch < cur_dec.channels; ch++) {
                            size_t idx = (size_t) i * cur_dec.channels + ch;
                            float val = (float) buf_cur_s32[idx] * t;
                            int32_t s = (int32_t) (val + (val >= 0.0f ? 0.5f : -0.5f));
                            if (s > 8388607) s = 8388607;
                            else if (s < -8388608) s = -8388608;
                            buf_cur_s32[idx] = s;
                        }
                    }
                    need_fade_in = false;
                }

                uint64_t delivered = 0;
                /* buf_cur is idle here -- this chunk decoded into buf_cur_s32
                 * instead -- so it doubles as the S16 fallback scratch buffer
                 * if the device turns out not to actually be local S24_LE by
                 * write time (see write_device_with_retry_s32_ex()'s own
                 * comment). */
                write_result_t wr = write_device_with_retry_s32(buf_cur_s32, n_cur, cur_dec.channels,
                                                                 cur_dec.sample_rate, cur_path_local,
                                                                 buf_cur, &delivered);
                cur_frames_played_local += delivered;
                pthread_mutex_lock(&audio_mutex);
                frames_played = cur_frames_played_local;
                pthread_mutex_unlock(&audio_mutex);

                if (wr == WRITE_RESULT_ABORTED) {
                    continue;
                } else if (wr == WRITE_RESULT_FAILED) {
                    DBG_LOG("audio: output failure, stopping (%s)\n",
                            safe_path_tail(cur_path_local));
                    pthread_mutex_lock(&audio_mutex);
                    last_playback_error = AUDIO_ERROR_OUTPUT_FAILED;
                    last_playback_error_generation = cur_generation;
                    have_current = false;
                    clear_current_format_locked();
                    paused = false;
                    pthread_mutex_unlock(&audio_mutex);
                    should_restart = false;
                    was_stopped = false;
                    ended_with_no_next = false;
                    goto inner_loop_done;
                }
            } else
#endif
            {
            decoder_read_result_t r_cur = decoder_read_s16(&cur_dec, chunk_frames, buf_cur);
            uint64_t n_cur = r_cur.frames;

            if (r_cur.status == DECODER_READ_RECOVERABLE_ERROR) {
                consecutive_decoder_errors++;
                DBG_LOG("audio: recoverable decoder error #%u at frame %" PRIu64 " (%s)\n",
                        consecutive_decoder_errors, cur_frames_played_local, safe_path_tail(cur_path_local));
                if (consecutive_decoder_errors >= 10) {
                    DBG_LOG("audio: consecutive recoverable errors exceeded limit (%s)\n",
                            safe_path_tail(cur_path_local));
                    r_cur.status = DECODER_READ_FATAL_ERROR;
                }
            } else if (r_cur.status == DECODER_READ_OK) {
                consecutive_decoder_errors = 0;
            }

            if (r_cur.status == DECODER_READ_FATAL_ERROR) {
                DBG_LOG("audio: fatal decoder error at frame %" PRIu64 "/%" PRIu64 " (%s)\n",
                        cur_frames_played_local, cur_dec.total_frames, safe_path_tail(cur_path_local));
                pthread_mutex_lock(&audio_mutex);
                last_playback_error = AUDIO_ERROR_DECODER_FAILED;
                last_playback_error_generation = cur_generation;
                have_current = false;
                clear_current_format_locked();
                paused = false;
                pthread_mutex_unlock(&audio_mutex);
                should_restart = false;
                was_stopped = false;
                ended_with_no_next = false;
                goto inner_loop_done;
            }

            if (n_cur == 0 && r_cur.status == DECODER_READ_EOF) {
                /* Zero-frame read: check whether this is a premature EOF for a
                 * finite local file, or a genuine (or live-stream) end. */
                bool is_stream = (cur_dec.net_stream != NULL);
                if (is_premature_eof(cur_frames_played_local, cur_dec.total_frames, is_stream)) {
                    /* Attempt bounded reopen+seek recovery */
                    bool recovered = false;
                    for (int dr = 0; dr < 3; dr++) {
                        pthread_mutex_lock(&audio_mutex);
                        bool abort = stop_requested || restart_requested;
                        pthread_mutex_unlock(&audio_mutex);
                        if (abort) break;

                        DBG_LOG("audio: premature EOF at frame %" PRIu64 "/%" PRIu64
                                ", reopen attempt %d (%s)\n",
                                cur_frames_played_local, cur_dec.total_frames, dr + 1,
                                safe_path_tail(cur_path_local));
                        usleep(50000); /* 50 ms backoff */

                        if (!reopen_decoder_at(&cur_dec, cur_path_local,
                                               cur_frames_played_local)) {
                            DBG_LOG("audio: decoder reopen failed on attempt %d\n", dr + 1);
                            cur_open = false; /* dec is now closed */
                            continue;
                        }
                        cur_open = true;
                        r_cur = decoder_read_s16(&cur_dec, chunk_frames, buf_cur);
                        n_cur = r_cur.frames;
                        if (n_cur > 0) { recovered = true; break; }
                    }
                    if (!recovered) {
                        DBG_LOG("audio: decoder recovery exhausted at %" PRIu64 "/%" PRIu64
                                " (%s)\n",
                                cur_frames_played_local, cur_dec.total_frames,
                                safe_path_tail(cur_path_local));
                        pthread_mutex_lock(&audio_mutex);
                        last_playback_error = AUDIO_ERROR_DECODER_FAILED;
                        last_playback_error_generation = cur_generation;
                        have_current = false;
                        clear_current_format_locked();
                        paused = false;
                        pthread_mutex_unlock(&audio_mutex);
                        should_restart = false;
                        was_stopped = false;
                        ended_with_no_next = false;
                        goto inner_loop_done;
                    }
                    /* n_cur > 0 now -- fall through to normal processing */
                }

                if (n_cur == 0) {
                    /* True EOF (or live stream end). If a next track is queued, hand
                     * off to it -- seamlessly if the format matches (device stays open),
                     * or with a brief reopen if it doesn't. */
                    if (!nxt_open && staged_next_path != NULL) {
                        if (decoder_open(&nxt_dec, staged_next_path)) {
                            nxt_open = true;
                            nxt_frames_consumed = 0;
                            nxt_replaygain_linear_local = staged_next_replaygain;
                            nxt_replaygain_applied_local = staged_next_replaygain_applied;
                        } else {
                            DBG_LOG("audio: failed to open next track (%s)\n",
                                    safe_path_tail(staged_next_path));
                        }
                    }

                    if (nxt_open) {
                        pthread_mutex_lock(&audio_mutex);
                        bool snap_still_valid = (staged_next_generation == next_track_generation);
                        if (!snap_still_valid) {
                            /* Stale next track: queued track was replaced while playing. Discard nxt_dec. */
                            pthread_mutex_unlock(&audio_mutex);
                            close_decoder_if_open(&nxt_dec, &nxt_open);
                            nxt_format_matches = false;
                            DBG_LOG("audio: stale gapless prefetch discarded (%s)\n",
                                    safe_path_tail(staged_next_path));
                            ended_with_no_next = true;
                            break;
                        }
                        pthread_mutex_unlock(&audio_mutex);

#ifndef HOST_BUILD
                        /* Gapless handoff to a different decoder -- see the
                         * initial-open call site's identical comment on why
                         * this must run before ensure_device_format(). */
                        audio_output_reset_s24_probe();
                        bool next_wide = can_use_wide_path(&nxt_dec);
                        bool device_ok = ensure_device_format(nxt_dec.channels, nxt_dec.sample_rate, next_wide);
#else
                        bool device_ok = ensure_device(nxt_dec.channels, nxt_dec.sample_rate);
#endif
                        decoder_close(&cur_dec);
                        cur_dec = nxt_dec;
                        cur_open = true;
                        nxt_open = false;
                        cur_frames_played_local = nxt_frames_consumed;
                        cur_replaygain_linear = nxt_replaygain_linear_local;
                        cur_replaygain_applied = nxt_replaygain_applied_local;
                        free(cur_path_local);

                        pthread_mutex_lock(&audio_mutex);
                        cur_path_local = next_path; next_path = NULL;
                        free(active_path);
                        active_path = cur_path_local ? strdup(cur_path_local) : NULL;
                        playback_generation++;
                        atomic_store_explicit(&mp3_index_active_generation, (unsigned int) playback_generation, memory_order_relaxed);
                        atomic_store_explicit(&mp3_index_stop_flag, false, memory_order_relaxed);
                        finish_mp3_deferred_seek_locked();
                        seek_pending = false;
                        cur_generation = playback_generation;
                        current_total_frames = cur_dec.total_frames;
                        current_sample_rate = cur_dec.sample_rate;
                        frames_played = cur_frames_played_local;
                        publish_current_format_locked(&cur_dec, cur_path_local,
                                                      cur_replaygain_linear, cur_replaygain_applied);
                        track_advanced = true;
                        if (mp3_needs_seek_index(&cur_dec))
                            request_mp3_seek_index_locked(&cur_dec, cur_path_local, cur_generation);
                        pthread_mutex_unlock(&audio_mutex);

                        if (!device_ok) { ended_with_no_next = true; break; }
                        continue;
                    }

                    ended_with_no_next = true;
                    break;
                }
            }

            apply_gain(buf_cur, (size_t) n_cur * cur_dec.channels, cur_replaygain_linear);
            peq_process(buf_cur, (size_t) n_cur, (int) cur_dec.channels, cur_dec.sample_rate);
            apply_gain(buf_cur, (size_t) n_cur * cur_dec.channels, vol);

            if (need_fade_in && n_cur > 0) {
                uint64_t rf = calculate_ramp_frames(cur_dec.sample_rate);
                uint64_t in_frames = (n_cur < rf) ? n_cur : rf;
                apply_ramp(buf_cur, in_frames, cur_dec.channels, 0.0f, 1.0f);
                need_fade_in = false;
            }

#ifndef HOST_BUILD
            uint64_t delivered = 0;
            write_result_t wr = write_device_with_retry(buf_cur, n_cur, cur_dec.channels,
                                                         cur_dec.sample_rate, cur_path_local,
                                                         &delivered);
            cur_frames_played_local += delivered;
            pthread_mutex_lock(&audio_mutex);
            frames_played = cur_frames_played_local;
            pthread_mutex_unlock(&audio_mutex);

            if (wr == WRITE_RESULT_ABORTED) {
                continue;
            } else if (wr == WRITE_RESULT_FAILED) {
                DBG_LOG("audio: output failure, stopping (%s)\n",
                        safe_path_tail(cur_path_local));
                pthread_mutex_lock(&audio_mutex);
                last_playback_error = AUDIO_ERROR_OUTPUT_FAILED;
                last_playback_error_generation = cur_generation;
                have_current = false;
                clear_current_format_locked();
                paused = false;
                pthread_mutex_unlock(&audio_mutex);
                should_restart = false;
                was_stopped = false;
                ended_with_no_next = false;
                goto inner_loop_done;
            }
#else
            write_device(buf_cur, n_cur, cur_dec.channels);
            cur_frames_played_local += n_cur;
            pthread_mutex_lock(&audio_mutex);
            frames_played = cur_frames_played_local;
            pthread_mutex_unlock(&audio_mutex);
#endif
            } /* end plain single-source block */

        }
        inner_loop_done: /* error-path goto target: skip break-flag handling */

        if (should_restart) continue; /* reopen at the top of the outer loop */

        close_decoder_if_open(&nxt_dec, &nxt_open);
        if (cur_open) { decoder_close(&cur_dec); cur_open = false; }
        close_device();
        free(cur_path_local);
        cur_path_local = NULL;

        pthread_mutex_lock(&audio_mutex);
        /* have_current was already cleared in the error gotos above; for
         * normal break paths (stop, natural EOF) it still needs clearing. */
        have_current = false;
        clear_current_format_locked();
        paused = false;
        if (ended_with_no_next) track_finished = true;
        if (was_stopped) stop_requested = false;
        pthread_mutex_unlock(&audio_mutex);
    }

    free(buf_cur);
    free(buf_next);
    free(buf_out);
#ifndef HOST_BUILD
    free(buf_cur_s32);
    free(buf_next_s32);
    free(buf_out_s32);
#endif
    return NULL;
}

void audio_init(void) {
#ifdef HOST_BUILD
    SDL_InitSubSystem(SDL_INIT_AUDIO);
#endif
    peq_init();

    if (!thread_started) {
        thread_started = true;
        pthread_create(&audio_thread, NULL, audio_thread_func, NULL);
    }
}

void audio_play_file_at(const char * path, double start_seconds,
                         bool has_replaygain, double replaygain_gain_db,
                         bool has_replaygain_peak, double replaygain_peak) {
    pthread_mutex_lock(&audio_mutex);
    free(restart_path);
    restart_path = strdup(path);
    restart_start_seconds = start_seconds;
    restart_replaygain_linear = (float) replaygain_to_linear(has_replaygain, replaygain_gain_db, has_replaygain_peak, replaygain_peak);
    restart_replaygain_applied = has_replaygain;
    free(next_path);
    next_path = NULL; /* the caller re-arms this via audio_set_next_track() right after */
    restart_requested = true;
    stop_requested = false;
    paused = false;
    track_finished = false;
    track_advanced = false;
    last_playback_error = AUDIO_ERROR_NONE;
    last_playback_error_generation = 0;
    playback_generation++;
    atomic_store_explicit(&mp3_index_active_generation, (unsigned int) playback_generation, memory_order_relaxed);
    atomic_store_explicit(&mp3_index_stop_flag, false, memory_order_relaxed);
    finish_mp3_deferred_seek_locked();
    seek_pending = false;
    free(active_path);
    active_path = strdup(path);
    clear_current_format_locked();
    pthread_cond_signal(&audio_cond);
    pthread_mutex_unlock(&audio_mutex);
}

void audio_set_next_track(const char * path, bool has_replaygain, double replaygain_gain_db,
                           bool has_replaygain_peak, double replaygain_peak) {
    pthread_mutex_lock(&audio_mutex);
    free(next_path);
    next_path = path ? strdup(path) : NULL;
    next_replaygain_linear = (float) replaygain_to_linear(has_replaygain, replaygain_gain_db, has_replaygain_peak, replaygain_peak);
    next_replaygain_applied = has_replaygain;
    /* Increment generation so any in-flight snapshot the playback thread
     * already took (for a crossfade prefetch) is detected as stale and
     * discarded when the blend window arrives. */
    next_track_generation++;
    pthread_mutex_unlock(&audio_mutex);
}

void audio_set_crossfade_enabled(bool enabled) {
    pthread_mutex_lock(&audio_mutex);
    crossfade_enabled = enabled;
    pthread_mutex_unlock(&audio_mutex);
}

void audio_set_low_power_mode(bool enabled) {
    pthread_mutex_lock(&audio_mutex);
    low_power_mode = enabled;
    pthread_mutex_unlock(&audio_mutex);
}

void audio_set_bt_output(bool enabled) {
#ifndef HOST_BUILD
    /* No lock: audio_output.c's own bt_requested flag is only ever read by
     * whichever single thread currently owns the output device (this
     * app's own playback thread, or usb_dac_bridge.c's bridge thread --
     * see audio_output.h's own comment on why only one is ever active at
     * a time) -- a stale read here just means the output switch lands on
     * the next audio_output_ensure() call instead of immediately, not a
     * correctness bug. */
    audio_output_set_bt_requested(enabled);
#else
    (void) enabled; /* host build has no Bluetooth output path -- SDL only */
#endif
}

void audio_set_usb_output(bool enabled, const char * alsa_device) {
#ifndef HOST_BUILD
    /* Same no-lock reasoning as audio_set_bt_output() right above. */
    audio_output_set_usb_requested(enabled, alsa_device);
#else
    (void) enabled;
    (void) alsa_device; /* host build has no USB audio-host output path -- SDL only */
#endif
}

void audio_toggle_pause(void) {
    pthread_mutex_lock(&audio_mutex);
    if (!have_current) {
        pthread_mutex_unlock(&audio_mutex);
        return;
    }
    paused = !paused;
    bool now_paused = paused;
    pthread_cond_signal(&audio_cond);
    pthread_mutex_unlock(&audio_mutex);

#ifdef HOST_BUILD
    if (sdl_dev != 0) {
        SDL_PauseAudioDevice(sdl_dev, now_paused ? 1 : 0);
    }
#else
    (void) now_paused;
#endif
}

void audio_stop(void) {
    pthread_mutex_lock(&audio_mutex);
    stop_requested = true;
    atomic_store_explicit(&mp3_index_stop_flag, true, memory_order_relaxed);
    pthread_cond_signal(&audio_cond);
    pthread_mutex_unlock(&audio_mutex);
}

bool audio_is_playing(void) {
    pthread_mutex_lock(&audio_mutex);
    bool result = have_current && !paused;
    pthread_mutex_unlock(&audio_mutex);
    return result;
}

bool audio_is_paused(void) {
    pthread_mutex_lock(&audio_mutex);
    bool result = have_current && paused;
    pthread_mutex_unlock(&audio_mutex);
    return result;
}

/* Publish a coalesced command and wake the playback thread. Always called
 * with audio_mutex held and returns with it unlocked. */
static void finish_seek_request_and_unlock(void) {
    seek_pending = true;
    pthread_cond_broadcast(&audio_cond);
    pthread_mutex_unlock(&audio_mutex);
}

static void request_seek_and_unlock(uint64_t frame) {
    if (frame > current_total_frames) frame = current_total_frames;
    seek_pending_frame = frame;
    seek_pending_is_percent = false;
    seek_pending_playback_generation = playback_generation;
    finish_seek_request_and_unlock();
}

void audio_seek(double seconds) {
    pthread_mutex_lock(&audio_mutex);
    if (!have_current || current_sample_rate == 0 || current_total_frames == 0 || !active_path ||
        (current_format_info.valid && current_format_info.is_stream)) {
        pthread_mutex_unlock(&audio_mutex);
        return;
    }
    if (!isfinite(seconds)) {
        pthread_mutex_unlock(&audio_mutex);
        return;
    }
    if (seconds < 0.0) seconds = 0.0;
    double duration = (double) current_total_frames / (double) current_sample_rate;
    if (seconds > duration) seconds = duration;
    uint64_t frame = (uint64_t) (seconds * (double) current_sample_rate);
    request_seek_and_unlock(frame);
}

/* Seeks to a percentage of the currently playing track. Stores the percentage
 * alongside playback_generation so the audio thread converts it using the
 * correct decoder metadata for that generation. */
void audio_seek_percent(double percent) {
    pthread_mutex_lock(&audio_mutex);
    if (!active_path || (current_format_info.valid && current_format_info.is_stream)) {
        pthread_mutex_unlock(&audio_mutex);
        return;
    }
    if (!isfinite(percent)) {
        pthread_mutex_unlock(&audio_mutex);
        return;
    }
    if (percent < 0.0) percent = 0.0;
    if (percent > 100.0) percent = 100.0;
    seek_pending_percent = percent;
    seek_pending_is_percent = true;
    seek_pending_playback_generation = playback_generation;
    finish_seek_request_and_unlock();
}

double audio_get_position_seconds(void) {
    pthread_mutex_lock(&audio_mutex);
    double result = (current_sample_rate != 0)
        ? (double) frames_played / (double) current_sample_rate
        : 0.0;
    pthread_mutex_unlock(&audio_mutex);
    return result;
}

double audio_get_resume_position_seconds(void) {
    pthread_mutex_lock(&audio_mutex);
    double result;
    if (mp3_seek_deferred && mp3_seek_deferred_generation == playback_generation &&
        mp3_seek_deferred_sample_rate > 0) {
        result = (double) mp3_seek_deferred_frame /
                 (double) mp3_seek_deferred_sample_rate;
    } else {
        result = current_sample_rate != 0
            ? (double) frames_played / (double) current_sample_rate : 0.0;
    }
    pthread_mutex_unlock(&audio_mutex);
    return result;
}

double audio_get_duration_seconds(void) {
    pthread_mutex_lock(&audio_mutex);
    double result = (current_sample_rate != 0)
        ? (double) current_total_frames / (double) current_sample_rate
        : 0.0;
    pthread_mutex_unlock(&audio_mutex);
    return result;
}

unsigned int audio_get_sample_rate(void) {
    pthread_mutex_lock(&audio_mutex);
    unsigned int result = current_sample_rate;
    pthread_mutex_unlock(&audio_mutex);
    return result;
}

bool audio_get_current_format_info(audio_current_format_info_t * out) {
    if (!out) return false;
    pthread_mutex_lock(&audio_mutex);
    bool valid = have_current && current_format_info.valid;
    if (valid) *out = current_format_info;
    else memset(out, 0, sizeof(*out));
    pthread_mutex_unlock(&audio_mutex);
    return valid;
}

/* Maps UI volume (0-100%) to a logarithmic audio taper in dB (MIN_VOLUME_DB..0dB),
 * converted to a linear multiplier for apply_gain(). 0% is true silence. */
#define MIN_VOLUME_DB (-70.0)

/* Curve exponent applied before the equal-dB taper to calibrate volume progression
 * across the 0-100% range. */
#define VOLUME_CURVE_EXPONENT 0.53

static double calibrated_taper_db(double percent) {
    if (percent <= 0.0) return MIN_VOLUME_DB;
    if (percent >= 1.0) return 0.0;
    return MIN_VOLUME_DB * (1.0 - pow(percent, VOLUME_CURVE_EXPONENT));
}

/* Hardware volume mapping: the codec's "Left"/"Right Playback Volume" ALSA controls
 * provide hardware attenuation. Raw 0 is maximum volume (0dB attenuation), with
 * higher raw values providing increasing attenuation up to full mute. */
#define HW_VOLUME_DB_PER_STEP 0.34
#define HW_VOLUME_MAX_RAW 230 /* full attenuation / silence */

static int volume_db_to_hw_raw(double db) {
    if (db >= 0.0) return 0; /* hardware's loudest point -- can't go past it */
    int raw = (int) lround(-db / HW_VOLUME_DB_PER_STEP);
    if (raw > HW_VOLUME_MAX_RAW) raw = HW_VOLUME_MAX_RAW;
    return raw;
}

/* Plugin-supplied alternative to volume_db_to_hw_raw()/calibrated_taper_db()
 * allowing plugins to define custom UI-volume to hardware-register mappings.
 *
 * LIVE state read by compute_hw_raw(). Mutated only by audio_set_custom_hw_volume_curve()
 * and audio_commit_hw_volume_curve(). */
static bool custom_hw_curve_active = false;
static uint8_t custom_hw_curve[HW_VOLUME_CURVE_LEN];

/* STAGED state scratch space used during plugin (re)load transactions,
 * committed to LIVE state via audio_commit_hw_volume_curve(). */
static bool staged_hw_curve_active = false;
static uint8_t staged_hw_curve[HW_VOLUME_CURVE_LEN];

/* For local output, hardware attenuation is used and digital volume_gain remains 1.0.
 * For USB DAC output, hardware mixer controls do not apply to the USB stream, so
 * digital attenuation is applied instead. Bluetooth volume is managed separately
 * via AVRCP.
 *
 * Shared helper for volume calculation. Caller must hold audio_mutex. */
static int compute_hw_raw(float vol) {
    if (custom_hw_curve_active) {
        /* Use plugin curve index 0 for mute when vol <= 0.0f. */
        int idx = (vol <= 0.0f) ? 0 : (int) lround((double) vol * 100.0);
        if (idx < 0) idx = 0;
        if (idx > 100) idx = 100;
        return custom_hw_curve[idx];
    }
    return (vol <= 0.0f) ? HW_VOLUME_MAX_RAW : volume_db_to_hw_raw(calibrated_taper_db((double) vol));
}

/* Apply only if no newer synchronous or queued request has superseded this
 * generation. The check and state update share audio_mutex, so a worker
 * that races a release-time audio_set_volume() cannot restore an older
 * drag sample afterward. */
static void audio_apply_volume(float new_volume, unsigned int generation) {
    if (new_volume < 0.0f) new_volume = 0.0f;
    if (new_volume > 1.0f) new_volume = 1.0f;
    pthread_mutex_lock(&audio_mutex);
    if (atomic_load(&volume_set_generation) != generation) {
        pthread_mutex_unlock(&audio_mutex);
        return;
    }
    volume = new_volume;
    volume_gain = (new_volume <= 0.0f) ? 0.0f : 1.0f;
#ifndef HOST_BUILD
    int raw = compute_hw_raw(new_volume);
    audio_output_request_hw_volume_raw(raw, raw);
    if (audio_output_is_usb_active()) {
        volume_gain = (new_volume <= 0.0f) ? 0.0f : (float) pow(10.0, calibrated_taper_db((double) new_volume) / 20.0);
    }
#endif
    pthread_mutex_unlock(&audio_mutex);
}

void audio_set_volume(float new_volume) {
    unsigned int generation = atomic_fetch_add(&volume_set_generation, 1) + 1;
    audio_apply_volume(new_volume, generation);
}

void audio_stage_custom_hw_volume_curve(bool active, const uint8_t * curve) {
    pthread_mutex_lock(&audio_mutex);
    staged_hw_curve_active = active;
    if (active) memcpy(staged_hw_curve, curve, sizeof(staged_hw_curve));
    pthread_mutex_unlock(&audio_mutex);
}

void audio_get_staged_hw_volume_curve_state(bool * active_out, uint8_t * curve_out) {
    pthread_mutex_lock(&audio_mutex);
    *active_out = staged_hw_curve_active;
    memcpy(curve_out, staged_hw_curve, sizeof(staged_hw_curve));
    pthread_mutex_unlock(&audio_mutex);
}

/* Installs staged as the new live state and writes hardware once to
 * match, both under the same lock -- see this function's own doc comment
 * in audio.h for why the install and the write must be atomic together,
 * not two separate locked steps. */
void audio_commit_hw_volume_curve(void) {
    pthread_mutex_lock(&audio_mutex);
    custom_hw_curve_active = staged_hw_curve_active;
    if (staged_hw_curve_active) memcpy(custom_hw_curve, staged_hw_curve, sizeof(custom_hw_curve));
#ifndef HOST_BUILD
    int raw = compute_hw_raw(volume);
    audio_output_request_hw_volume_raw(raw, raw);
#endif
    pthread_mutex_unlock(&audio_mutex);
}

/* Reapplies immediately at whatever volume is currently authoritative, so
 * a curve switch is audible right away rather than waiting for the next
 * slider touch -- for the normal "an already-loaded, running plugin
 * changes its curve" case only, see this function's own doc comment in
 * audio.h. Mutates the LIVE pair directly, under one lock alongside the
 * write -- NOT via audio_stage_custom_hw_volume_curve()/
 * audio_commit_hw_volume_curve(), which operate on the separate STAGED
 * pair a plugin (re)load transaction uses; this path runs outside any
 * such transaction; touching STAGED here would leave it holding a value
 * that doesn't belong to (and could leak into) whatever transaction
 * starts next. Deliberately NOT implemented via audio_set_volume():
 * that bumps volume_set_generation as a brand-new synchronous command,
 * which would silently invalidate a newer audio_request_volume() (an
 * in-flight slider-drag sample) already queued on the worker thread by
 * the time it runs -- this only ever touches the hardware register, not
 * that generation counter. */
void audio_set_custom_hw_volume_curve(const uint8_t * curve) {
    pthread_mutex_lock(&audio_mutex);
    custom_hw_curve_active = (curve != NULL);
    if (curve) memcpy(custom_hw_curve, curve, sizeof(custom_hw_curve));
#ifndef HOST_BUILD
    int raw = compute_hw_raw(volume);
    audio_output_request_hw_volume_raw(raw, raw);
#endif
    pthread_mutex_unlock(&audio_mutex);
}

static void * volume_request_worker_main(void * unused) {
    (void) unused;
    for (;;) {
        pthread_mutex_lock(&volume_request_mutex);
        while (!volume_request_pending)
            pthread_cond_wait(&volume_request_cond, &volume_request_mutex);
        float requested = volume_requested_value;
        unsigned int generation = volume_requested_generation;
        volume_request_pending = false;
        pthread_mutex_unlock(&volume_request_mutex);
        audio_apply_volume(requested, generation);
    }
    return NULL;
}

static void start_volume_request_worker(void) {
    pthread_t thread;
    if (pthread_create(&thread, NULL, volume_request_worker_main, NULL) == 0) {
        pthread_detach(thread);
        volume_request_worker_ready = true;
    }
}

void audio_request_volume(float new_volume) {
    if (new_volume < 0.0f) new_volume = 0.0f;
    if (new_volume > 1.0f) new_volume = 1.0f;
    unsigned int generation = atomic_fetch_add(&volume_set_generation, 1) + 1;
    pthread_once(&volume_request_once, start_volume_request_worker);
    if (!volume_request_worker_ready) {
        audio_apply_volume(new_volume, generation);
        return;
    }
    pthread_mutex_lock(&volume_request_mutex);
    volume_requested_value = new_volume;
    volume_requested_generation = generation;
    volume_request_pending = true;
    pthread_cond_signal(&volume_request_cond);
    pthread_mutex_unlock(&volume_request_mutex);
}

float audio_get_volume(void) {
    pthread_mutex_lock(&audio_mutex);
    float result = volume;
    pthread_mutex_unlock(&audio_mutex);
    return result;
}

bool audio_consume_track_finished(void) {
    pthread_mutex_lock(&audio_mutex);
    bool result = track_finished;
    track_finished = false;
    pthread_mutex_unlock(&audio_mutex);
    return result;
}

bool audio_consume_track_advanced(void) {
    pthread_mutex_lock(&audio_mutex);
    bool result = track_advanced;
    track_advanced = false;
    pthread_mutex_unlock(&audio_mutex);
    return result;
}

audio_error_t audio_consume_error_ex(uint64_t * out_generation) {
    pthread_mutex_lock(&audio_mutex);
    audio_error_t err = last_playback_error;
    if (out_generation) *out_generation = last_playback_error_generation;
    last_playback_error = AUDIO_ERROR_NONE;
    last_playback_error_generation = 0;
    pthread_mutex_unlock(&audio_mutex);
    return err;
}

audio_error_t audio_consume_error(void) {
    return audio_consume_error_ex(NULL);
}

uint64_t audio_get_playback_generation(void) {
    pthread_mutex_lock(&audio_mutex);
    uint64_t gen = playback_generation;
    pthread_mutex_unlock(&audio_mutex);
    return gen;
}
