#include "audio.h"

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

#include "debug_log.h"
#include "audio_helpers.h"

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

static bool decoder_seek(decoder_t * dec, uint64_t frame) {
    if (dec->net_stream) return false; /* live stream -- cannot seek */
    switch (dec->type) {
        case DECODER_FLAC:   return drflac_seek_to_pcm_frame(dec->as.flac, frame) != 0;
        case DECODER_MP3:    return drmp3_seek_to_pcm_frame(dec->as.mp3, frame) != 0;
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
            if (dec->as.mp3) { drmp3_uninit(dec->as.mp3); free(dec->as.mp3); }
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
static bool low_power_mode = false;

/* Every explicit track start advances this generation. Background seek
 * preparation is accepted only if it still belongs to this exact track. */
static uint64_t playback_generation = 0;
static uint64_t seek_request_generation = 0;
static char * active_path = NULL; /* protected by audio_mutex */

typedef struct {
    decoder_t dec;
    uint64_t frame;
    uint64_t playback_generation;
    uint64_t seek_generation;
} prepared_seek_t;

/* Produced by a detached seek worker, consumed only by the playback
 * thread. A newer worker may replace an unconsumed result under the mutex. */
static prepared_seek_t * prepared_seek = NULL;

/* One seek worker at a time. Rapid progress-bar taps used to pthread_create
 * a new decoder_open()+decoder_seek() per release; generation only dropped
 * the finished result, so several FLAC opens ran together, saturated this
 * device, and the player UI stopped taking taps for ~10-15s. A newer
 * audio_seek() while the worker is busy just updates these fields; the
 * in-flight worker loops onto the latest target instead of stacking. */
static bool seek_worker_active = false;
static uint64_t seek_pending_frame = 0;
static uint64_t seek_pending_playback_generation = 0;

static uint64_t frames_played = 0;
static uint64_t current_total_frames = 0;
static unsigned int current_sample_rate = 0;
static audio_current_format_info_t current_format_info;
static uint64_t current_format_generation = 0;

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
    current_format_info.output_bit_depth = 16; /* decoder_read_s16() -> S16_LE on every output route */
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

static void * seek_worker_func(void * arg) {
    (void) arg;

    for (;;) {
        uint64_t frame, pgen, sgen;
        char * path;
        pthread_mutex_lock(&audio_mutex);
        if (!active_path || seek_pending_playback_generation != playback_generation) {
            seek_worker_active = false;
            pthread_mutex_unlock(&audio_mutex);
            return NULL;
        }
        path = strdup(active_path);
        if (!path) {
            seek_worker_active = false;
            pthread_mutex_unlock(&audio_mutex);
            return NULL;
        }
        frame = seek_pending_frame;
        pgen = seek_pending_playback_generation;
        sgen = seek_request_generation;
        pthread_mutex_unlock(&audio_mutex);

        prepared_seek_t * ready = calloc(1, sizeof(*ready));
        bool opened = ready && decoder_open(&ready->dec, path);
        free(path);
        if (opened) {
            pthread_mutex_lock(&audio_mutex);
            bool stale = (playback_generation != pgen) || (seek_request_generation != sgen);
            pthread_mutex_unlock(&audio_mutex);
            if (stale) {
                decoder_close(&ready->dec);
                free(ready);
                continue;
            }
            if (frame > ready->dec.total_frames) frame = ready->dec.total_frames;
            if (frame > 0 && !decoder_seek(&ready->dec, frame)) {
                decoder_close(&ready->dec);
                free(ready);
                continue;
            }
            ready->frame = frame;
            ready->playback_generation = pgen;
            ready->seek_generation = sgen;
        } else {
            free(ready);
            ready = NULL;
        }

        pthread_mutex_lock(&audio_mutex);
        bool still_current = ready &&
            playback_generation == pgen &&
            seek_request_generation == sgen &&
            !restart_requested && !stop_requested;
        if (still_current) {
            prepared_seek_t * superseded = prepared_seek;
            prepared_seek = ready;
            ready = NULL;
            seek_worker_active = false;
            pthread_cond_signal(&audio_cond);
            pthread_mutex_unlock(&audio_mutex);
            if (superseded) {
                decoder_close(&superseded->dec);
                free(superseded);
            }
            return NULL;
        }

        bool retry = (playback_generation == pgen) && (seek_request_generation != sgen);
        if (!retry) seek_worker_active = false;
        pthread_mutex_unlock(&audio_mutex);
        if (ready) {
            decoder_close(&ready->dec);
            free(ready);
        }
        if (!retry) return NULL;
    }
}

/* Real-device feedback: "noticeable sound hissing in quiet songs, not
 * related to the files (same songs sound clean in the stock player)".
 * Root cause: a plain `(int32_t) (float)` cast truncates toward zero
 * rather than rounding to the nearest integer, and it did so on every
 * single sample at every non-unity gain (which is almost always -- see
 * audio_set_volume()'s own comment, true silence is the only exact
 * 1.0x). For a quiet passage, sample magnitudes are already small, so
 * truncation's bias (always toward zero, i.e. always down in magnitude)
 * is a large fraction of the sample's own value rather than a rounding
 * error a full dynamic-range signal would completely mask -- exactly the
 * classic naive-gain-scaling recipe for audible quantization
 * distortion/hiss. lrintf() rounds to the nearest representable integer
 * (ties to even) instead of always truncating down. */
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

/* Closes and reopens a local decoder at last_confirmed_frame.
 * Only valid for finite local files (not net_stream). Returns true on
 * success; on failure dec is left in a closed/invalid state. */
static bool reopen_decoder_at(decoder_t * dec, const char * path,
                               uint64_t last_confirmed_frame) {
    decoder_close(dec);
    if (!decoder_open(dec, path)) return false;
    if (last_confirmed_frame > 0) {
        if (!decoder_seek(dec, last_confirmed_frame)) {
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
 * audio_output.h and its own top-of-file comment) -- all the actual
 * local-hardware-vs-Bluetooth routing, pacing, and failure-handling logic
 * that used to live here directly now lives there instead, shared with
 * usb_dac_bridge.c's own separate output stream. */
static bool ensure_device(unsigned int channels, unsigned int sample_rate) {
    bool ok = audio_output_ensure(channels, sample_rate);
    if (ok) {
        /* Real-device bug report: USB headphones had volume maxed out on
         * first boot no matter what the UI showed, only "fixed" by
         * pressing vol+/-. Root cause: audio_set_volume()'s USB
         * digital-taper branch (see its own comment) depends on
         * audio_output_is_usb_active(), which only reflects the truth once
         * open_device() has actually run for the current track -- any
         * audio_set_volume() call made before that (e.g. applying the
         * saved volume right after boot, before anything has ever played)
         * always sees it as false, pinning volume_gain at unity even
         * though the eventual real target is USB. Nothing re-derived it
         * once the real target became known. Re-checking here, right after
         * every ensure_device() call (already invoked every decode chunk
         * for the same class of target-changed-mid-stream reason -- see
         * this function's own caller comment on Bluetooth), and only
         * re-deriving on an actual change costs nothing extra the rest of
         * the time. */
        static bool last_gain_for_usb = false;
        bool now_usb = audio_output_is_usb_active();
        if (now_usb != last_gain_for_usb) {
            audio_set_volume(audio_get_volume());
            last_gain_for_usb = now_usb;
        }
    }
    return ok;
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
#endif /* !HOST_BUILD */

static void close_decoder_if_open(decoder_t * dec, bool * is_open) {
    if (*is_open) { decoder_close(dec); *is_open = false; }
}

/* Mixes n frames of buf_cur/buf_next into buf_out with a linear crossfade.
 * fade_start_frame is how many frames into the CROSSFADE_SECONDS window
 * this chunk's first frame falls, so the fade is continuous across chunk
 * boundaries rather than stepped. */
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

static void * audio_thread_func(void * arg) {
    (void) arg;

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
    /* Real-device bug report: plugging/unplugging headphones or an aux
     * cable produced an audible pop/static even with NOTHING ever played
     * this boot (no track loaded, ensure_device() never once called) --
     * confirmed NOT reproducible on the stock player, and confirmed via a
     * genuine fresh reboot test (not just residual state from earlier
     * playback this same boot). See HARDWARE_DRIVERS.md's "Audio subsystem"
     * section for the full investigation -- an ALSA-mixer-control-based fix
     * (muting "Mute Output" at startup) was tried and DISPROVEN by direct
     * testing: that control doesn't hold state from userspace at all on
     * this driver, under any condition. Root cause instead: this codec's
     * driver implements the standard ASoC .digital_mute DAI callback, and
     * the machine driver gates real chip power (cs43131_set_power())
     * through the standard DAPM bias-level state machine as streams
     * start/stop -- the SAME mechanism this file's own pause-time pop fix
     * (close_device() the instant pause is detected, further down this
     * loop) already relies on, and which HARDWARE_DRIVERS.md confirms is
     * genuinely effective. A track that's simply never been played yet has
     * never triggered that stop sequence even once, leaving the codec in
     * whatever raw state its own boot-time kernel probe left it in --
     * different from, and apparently less safe than, the state reached by
     * actually going through a real open-then-close cycle. Priming it once
     * here, before the very first real track ever loads, puts the codec
     * through that exact same proven-safe sequence early -- no audio is
     * written, so this is inaudible in itself. */
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

        if (start_seconds > 0.0) {
            uint64_t start_frame = (uint64_t) (start_seconds * (double) cur_dec.sample_rate);
            if (start_frame > cur_dec.total_frames) start_frame = cur_dec.total_frames;
            if (decoder_seek(&cur_dec, start_frame)) {
                cur_frames_played_local = start_frame;
            } else {
                DBG_LOG("audio: initial seek to frame %" PRIu64 " failed (%s), playing from start\n",
                        start_frame, safe_path_tail(cur_path_local));
                cur_frames_played_local = 0;
            }
        }

        if (!ensure_device(cur_dec.channels, cur_dec.sample_rate)) {
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
        unsigned int consecutive_decoder_errors = 0;
        unsigned int consecutive_nxt_decoder_errors = 0;

        for (;;) {
            pthread_mutex_lock(&audio_mutex);
#ifndef HOST_BUILD
            /* Real-device bug report: plugging/unplugging headphones or an
             * aux cable produced an audible pop/static -- confirmed NOT
             * reproducible on the stock player, and not reproducible here
             * either once actually stopped (close_device() below already
             * runs then) -- only while paused, with a track still loaded.
             * Root cause: this loop only ever waited here while paused,
             * never closing the PCM device (tinyalsa's pcm_open() from
             * audio_output.c stayed open the whole time, same struct pcm*
             * live from before the pause), leaving whatever this codec's
             * own DAPM/amp power state is tied to "playing" energized
             * indefinitely -- a physical jack insertion/removal on a live,
             * powered analog path is exactly what produces an audible pop,
             * confirmed by this app being the only thing different (the
             * headphone jack and codec hardware are identical either way).
             * Closed here, the instant a pause is detected -- reopened
             * automatically by ensure_device() a few lines below once
             * playback actually resumes, the same lazy reopen it already
             * does for every other reason the device might be closed. */
            if (paused && !stop_requested && !restart_requested) {
                float vol = volume_gain;
                pthread_mutex_unlock(&audio_mutex);
                if (cur_open && cur_dec.sample_rate > 0) {
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
            while (paused && !stop_requested && !restart_requested && prepared_seek == NULL) {
                pthread_cond_wait(&audio_cond, &audio_mutex);
            }
            bool do_stop = stop_requested;
            bool do_restart = restart_requested;
            prepared_seek_t * ready_seek = prepared_seek;
            prepared_seek = NULL;
            bool ready_seek_valid = ready_seek &&
                ready_seek->playback_generation == cur_generation &&
                ready_seek->seek_generation == seek_request_generation;
            bool xfade_on = crossfade_enabled;
            float vol = volume_gain;
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

            if (ready_seek && !ready_seek_valid) {
                decoder_close(&ready_seek->dec);
                free(ready_seek);
                ready_seek = NULL;
            }

#ifndef HOST_BUILD
            /* Real-device bug: connecting Bluetooth headphones mid-track (the
             * common case -- a user pairs while a track is already playing,
             * or this app auto-resumed playback before the GUI's connection
             * poll had even run once) never switched output, because the two
             * ensure_device() call sites below only run at a track boundary
             * (a fresh audio_play_file_at(), or the automatic handoff into a
             * queued next track). The requested Bluetooth target can change
             * at any moment from the GUI thread's poll, so it needs checking
             * every chunk here too, not just at those boundaries. Cheap:
             * audio_output_ensure() (which this delegates to) already only
             * reopens if the format OR the Bluetooth target actually
             * changed, so calling it unconditionally every chunk costs
             * nothing extra when nothing changed. Best-effort -- write_device()
             * below skips writing rather than crashing if this fails, and the
             * next iteration tries again. */
            ensure_device(cur_dec.channels, cur_dec.sample_rate);
#endif

            if (do_restart || do_stop) {
                if (ready_seek) {
                    decoder_close(&ready_seek->dec);
                    free(ready_seek);
                }
                /* Phase 4: Controlled transition ramp-down on manual stop/restart */
                if (cur_open && cur_dec.sample_rate > 0) {
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

            if (ready_seek) {
                /* Phase 4: Smooth transition on prepared seek swap */
                if (cur_open && cur_dec.sample_rate > 0) {
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
                decoder_close(&cur_dec);
                cur_dec = ready_seek->dec;
                cur_open = true;
                cur_frames_played_local = ready_seek->frame;
                free(ready_seek);
                ensure_device(cur_dec.channels, cur_dec.sample_rate);
                pthread_mutex_lock(&audio_mutex);
                frames_played = cur_frames_played_local;
                current_total_frames = cur_dec.total_frames;
                current_sample_rate = cur_dec.sample_rate;
                publish_current_format_locked(&cur_dec, cur_path_local,
                                              cur_replaygain_linear, cur_replaygain_applied);
                pthread_mutex_unlock(&audio_mutex);
                need_fade_in = true;
                consecutive_decoder_errors = 0;
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

                /* Blend window finished (cur fully consumed) -- promote next to current.
                 * Verify the snapshot's generation still matches what was armed --
                 * if not, another audio_set_next_track() replaced the queued track
                 * while we were in the crossfade window; discard the stale prefetch. */
                pthread_mutex_lock(&audio_mutex);
                bool snap_still_valid = (staged_next_generation == next_track_generation);
                if (snap_still_valid) {
                    decoder_close(&cur_dec);
                    cur_dec = nxt_dec;
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
                    seek_request_generation++;
                    cur_generation = playback_generation;
                    current_total_frames = cur_dec.total_frames;
                    current_sample_rate = cur_dec.sample_rate;
                    frames_played = cur_frames_played_local;
                    publish_current_format_locked(&cur_dec, cur_path_local,
                                                  cur_replaygain_linear, cur_replaygain_applied);
                    track_advanced = true;
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

                        bool device_ok = ensure_device(nxt_dec.channels, nxt_dec.sample_rate);
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
                        seek_request_generation++;
                        cur_generation = playback_generation;
                        current_total_frames = cur_dec.total_frames;
                        current_sample_rate = cur_dec.sample_rate;
                        frames_played = cur_frames_played_local;
                        publish_current_format_locked(&cur_dec, cur_path_local,
                                                      cur_replaygain_linear, cur_replaygain_applied);
                        track_advanced = true;
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
    seek_request_generation++;
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

void audio_seek(double seconds) {
    pthread_mutex_lock(&audio_mutex);
    if (!have_current || current_sample_rate == 0 || current_total_frames == 0 || !active_path) {
        pthread_mutex_unlock(&audio_mutex);
        return;
    }
    if (seconds < 0.0) seconds = 0.0;
    uint64_t frame = (uint64_t) (seconds * (double) current_sample_rate);
    if (frame > current_total_frames) frame = current_total_frames;
    seek_pending_frame = frame;
    seek_pending_playback_generation = playback_generation;
    seek_request_generation++;
    bool start_worker = !seek_worker_active;
    pthread_t worker;
    int create_result = 0;
    if (start_worker) {
        /* Keep the reservation and thread creation under the same lock.
         * Otherwise a concurrent seek can observe seek_worker_active, leave
         * its request for that worker, and then lose it if pthread_create()
         * fails and the reservation is cleared afterward. The new worker
         * simply waits on audio_mutex until this short operation completes. */
        seek_worker_active = true;
        create_result = pthread_create(&worker, NULL, seek_worker_func, NULL);
        if (create_result != 0)
            seek_worker_active = false;
    }
    pthread_mutex_unlock(&audio_mutex);

    if (start_worker && create_result == 0)
        pthread_detach(worker);
}

double audio_get_position_seconds(void) {
    pthread_mutex_lock(&audio_mutex);
    double result = (current_sample_rate != 0)
        ? (double) frames_played / (double) current_sample_rate
        : 0.0;
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

/* Human loudness perception is roughly logarithmic, so a raw linear
 * amplitude scale (gain == percent) crams nearly the entire perceptible
 * range into the bottom ~10-20% of the slider and makes the rest barely
 * distinguishable -- real-device feedback comparing against stock: "volume
 * on 2 corresponds to volume in 20 on the stock OS", i.e. stock isn't
 * linear either. This maps the UI's 0-100% onto MIN_VOLUME_DB..0dB
 * linearly in dB (the standard "audio taper" essentially all real volume
 * controls use) and converts that to the actual linear multiplier
 * apply_gain() needs. 0% is always true silence, not just -50dB.
 *
 * Real-device feedback, two rounds: first "setting it to 1 is equivalent of
 * setting it to 15/20 in the stock player" (low settings too loud), then --
 * after a first attempt at a fix that reshaped this into a curve via a
 * TAPER_EXPONENT < 1 on (1-percent), keeping it closer to the MIN_VOLUME_DB
 * floor for a larger share of the low end -- "volume from 1% to 20% is
 * almost the same with unnoticeable change", because that exponent
 * compressed the wrong thing: it reduced absolute loudness at low percents,
 * but it also compressed the dB *spacing* between adjacent low percents
 * (1% and 20% landed only ~5dB apart instead of the plain linear taper's
 * ~9.5dB), which is the opposite of what "too loud, and also want to still
 * be able to tell settings apart" calls for. Reverted the exponent entirely
 * -- back to a plain, equal-dB-per-percent linear taper, which is what
 * actually preserves even, distinguishable steps -- and widened the floor
 * itself instead (-50dB to -70dB) so a given low percent is genuinely
 * quieter in absolute terms without touching how much the curve moves per
 * percent point. At 1%: -69.3dB (was -49.5dB). At 20%: -56dB (was -40dB) --
 * a ~13dB gap between those two, comfortably audible, instead of the
 * exponent version's ~5dB. Still a best-effort widening pending real
 * side-by-side percent/step reference points against stock, not a precise
 * fit. */
#define MIN_VOLUME_DB (-70.0)
/* Real-device comparison against stock: stock's own max volume was audibly
 * louder than ours at the time, when this taper's 100% still meant exactly
 * 1.0x (0dB) digital gain with the hardware DAC's own "Left"/"Right Playback
 * Volume" registers believed to be unusable (raw 0, thought to be their only
 * usable setting). A digital-only +6dB boost zone at the top of the slider
 * was added to close that gap, then later removed once the hardware
 * registers turned out to be a real, working attenuator after all (see
 * volume_db_to_hw_raw()'s own comment) -- with hardware actually carrying
 * the taper down from raw 0 (its own loudest point, confirmed live to
 * already be louder than stock's own boosted max), there was no gap left
 * to close, and live listening confirmed the digital boost only added noise
 * without a worthwhile loudness gain on top of that. apply_gain() still
 * hard-clips any sample that would overflow int16 range rather than
 * wrapping, for replaygain or any other gain source that can exceed unity. */
/* Real-device stock-player calibration (2026-08-18): this app's 50% was
 * reported to match only about 27% on the stock player, and most of the
 * useful loudness arrived abruptly above 75%. Modeling that measured
 * mapping as stock = ours^k gives k=log(.27)/log(.50)=1.889; applying its
 * inverse (1/k ~= .529) before the existing equal-dB taper makes a UI value
 * represent approximately the same perceived position as stock. Rounded to
 * 0.53 rather than pretending the ear comparison has laboratory precision.
 *
 * Resulting anchors (versus the old linear-dB curve): 25% -36.4dB (was
 * -52.5), 50% -21.5dB (was -35), 75% -9.9dB (was -17.5), 100% 0dB. This
 * raises the previously unusable low/mid range while flattening dB spacing
 * near the top, eliminating the perceived >75% surge. 0% remains handled
 * separately as true silence, so pow(0, exponent) never compromises mute. */
#define VOLUME_CURVE_EXPONENT 0.53

static double calibrated_taper_db(double percent) {
    if (percent <= 0.0) return MIN_VOLUME_DB;
    if (percent >= 1.0) return 0.0;
    return MIN_VOLUME_DB * (1.0 - pow(percent, VOLUME_CURVE_EXPONENT));
}

/* Real-device investigation: applying this entire taper digitally (the only
 * option before this) shrinks the *used* range of a 16-bit PCM sample at low
 * volumes, which is audible as noise -- a real complaint ("noticeable noise
 * in the lower end"). The codec's own "Left"/"Right Playback Volume" ALSA
 * controls (raw 0-255, no TLV/dB scale published -- `amixer contents` shows
 * `access=rw------`, no R flag) turn out to be a real, working hardware
 * attenuator, despite `amixer cget` making them look broken: cget always
 * reports 0 no matter what was last written (confirmed live, same-shell
 * write-then-read-back), but the write itself demonstrably reaches the DAC
 * and audibly changes output level -- confirmed live, ear-to-speaker, at
 * several raw values with real playback running and both channels
 * independently verified. Raw 0 is the loudest point (no attenuation);
 * increasing raw attenuates further, reaching full silence around raw 230
 * (~90% of the register's range) -- also confirmed live.
 *
 * HW_VOLUME_DB_PER_STEP is an ESTIMATE, not a measured constant -- there is
 * no TLV, no datasheet, and no SPL meter available. It's derived from a
 * single live A/B: raw ~191 (75% of range) was reported as "barely
 * audible, matching stock's 5-10%", and this taper's own
 * the then-current linear taper at 7.5% worked out to about -64.75dB, giving roughly
 * 0.34dB per raw step. Treat this as a first-pass calibration that will
 * very likely need live tuning against real listening feedback, the same
 * way MIN_VOLUME_DB above needed two rounds before it matched
 * expectations. That history predates the later stock-player calibration
 * implemented by calibrated_taper_db() below; unlike the earlier guessed
 * exponent, the new exponent is derived from a measured 50%=stock-27%
 * anchor and deliberately corrects the opposite problem (low/mid range too
 * quiet, with loudness crowded above 75%). */
#define HW_VOLUME_DB_PER_STEP 0.34
#define HW_VOLUME_MAX_RAW 230 /* confirmed live: full silence by here */

static int volume_db_to_hw_raw(double db) {
    if (db >= 0.0) return 0; /* hardware's loudest point -- can't go past it */
    int raw = (int) lround(-db / HW_VOLUME_DB_PER_STEP);
    if (raw > HW_VOLUME_MAX_RAW) raw = HW_VOLUME_MAX_RAW;
    return raw;
}

/* Hardware carries the whole taper (see volume_db_to_hw_raw()'s own comment
 * for the real-device investigation behind this); digital gain stays
 * pinned at unity throughout, including 100%, which lands at raw 0 /
 * digital 1.0 -- the hardware's own loudest point. A digital boost mode
 * that pushed louder than that was tried and removed: raw 0 alone was
 * already confirmed live to be as loud as, or louder than, stock's own
 * "boosted" max, and adding digital gain on top of it only brought back
 * the same digital-attenuation-shaped noise this whole redesign exists to
 * avoid, for no worthwhile loudness gain. */
/* Real-device bug report: volume did nothing with a USB headphone/DAC
 * connected. Hardware attenuation (audio_output_set_hw_volume_raw() below)
 * only ever reaches this device's own internal codec -- USB output instead
 * pipes raw PCM to a separate `aplay -D plughw:<card>,0` process/ALSA card
 * that never touches that mixer (see audio_output_is_usb_active()'s own
 * comment in audio_output.h). The hardware write and unity-gain pin below
 * still run unconditionally first, exactly as before (a harmless no-op for
 * USB, and for Bluetooth too, and still correct for local output) -- only
 * for USB specifically is volume_gain then overwritten with a real digital
 * taper afterward. Deliberately NOT applied for Bluetooth: an earlier
 * attempt applied this same digital fallback whenever output wasn't local
 * (Bluetooth included) and was reverted after a real-device report of
 * double-attenuated (too quiet) Bluetooth audio -- Bluetooth volume is
 * already handled by a completely separate, working AVRCP-based mechanism
 * (bluetooth_control.c's bt_source_vol_sync_thread_func()) that has
 * nothing to do with this app's own PCM gain, so it must be left alone. */
void audio_set_volume(float new_volume) {
    if (new_volume < 0.0f) new_volume = 0.0f;
    if (new_volume > 1.0f) new_volume = 1.0f;
    pthread_mutex_lock(&audio_mutex);
    volume = new_volume;
    volume_gain = (new_volume <= 0.0f) ? 0.0f : 1.0f;
#ifndef HOST_BUILD
    int raw = (new_volume <= 0.0f) ? HW_VOLUME_MAX_RAW : volume_db_to_hw_raw(calibrated_taper_db((double) new_volume));
    audio_output_set_hw_volume_raw(raw, raw);
    if (audio_output_is_usb_active()) {
        volume_gain = (new_volume <= 0.0f) ? 0.0f : (float) pow(10.0, calibrated_taper_db((double) new_volume) / 20.0);
    }
#endif
    pthread_mutex_unlock(&audio_mutex);
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
