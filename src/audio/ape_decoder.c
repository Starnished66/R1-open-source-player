/*
 * Monkey's Audio decoder -- ported from FFmpeg's libavcodec/apedec.c
 * (LGPL 2.1+, Copyright (c) 2007 Benjamin Zores <ben@geexbox.org>, based
 * upon libdemac by Dave Chapman). See ape_decoder.h for the scope this port
 * covers and the important caveat about it being unverified against a real
 * file. Where structure/logic mirrors the original closely, comments below
 * are trimmed from the original FFmpeg source.
 */

#include "ape_decoder.h"
#include "ape_demux.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_CHANNELS 2
#define HISTORY_SIZE 512
#define PREDICTOR_ORDER 8
#define PREDICTOR_SIZE 50

#define YDELAYA (18 + PREDICTOR_ORDER * 4)
#define YDELAYB (18 + PREDICTOR_ORDER * 3)
#define XDELAYA (18 + PREDICTOR_ORDER * 2)
#define XDELAYB (18 + PREDICTOR_ORDER)

#define YADAPTCOEFFSA 18
#define XADAPTCOEFFSA 14
#define YADAPTCOEFFSB 10
#define XADAPTCOEFFSB 5

#define APE_FILTER_LEVELS 3

#define APE_FRAMECODE_MONO_SILENCE 1
#define APE_FRAMECODE_STEREO_SILENCE 3
#define APE_FRAMECODE_PSEUDO_STEREO 4

static const uint16_t ape_filter_orders[5][APE_FILTER_LEVELS] = {
    {  0,   0,    0 },
    { 16,   0,    0 },
    { 64,   0,    0 },
    { 32, 256,    0 },
    { 16, 256, 1280 },
};

static const uint8_t ape_filter_fracbits[5][APE_FILTER_LEVELS] = {
    {  0,  0,  0 },
    { 11,  0,  0 },
    { 11,  0,  0 },
    { 10, 13,  0 },
    { 11, 13, 15 },
};

typedef struct {
    int16_t * coeffs;
    int16_t * adaptcoeffs;
    int16_t * historybuffer;
    int16_t * delay;
    uint32_t avg;
} APEFilter;

typedef struct {
    uint32_t k;
    uint32_t ksum;
} APERice;

typedef struct {
    uint32_t low;
    uint32_t range;
    uint32_t help;
    unsigned int buffer;
} APERangecoder;

typedef struct {
    int64_t * buf;
    int64_t lastA[2];
    int64_t filterA[2];
    int64_t filterB[2];
    uint64_t coeffsA[2][4];
    uint64_t coeffsB[2][5];
    int64_t historybuffer[HISTORY_SIZE + PREDICTOR_SIZE];
} APEPredictor64;

struct ape_decoder {
    ape_demux_t * demux;

    int channels;
    int bps;
    int fileversion;
    int compression_level;
    int fset;

    int frameflags;

    APEPredictor64 predictor64;

    int32_t * decoded_buffer; /* [2][blocksperframe], interleaved as two blocks */
    int32_t * decoded[MAX_CHANNELS];
    int32_t * interim_buffer;
    int32_t * interim[MAX_CHANNELS];
    uint32_t max_frame_blocks;

    int16_t * filterbuf[APE_FILTER_LEVELS];

    APERangecoder rc;
    APERice riceX;
    APERice riceY;
    APEFilter filters[APE_FILTER_LEVELS][2];

    uint8_t * swapbuf;
    uint32_t swapbuf_capacity;
    uint8_t * ptr;
    uint8_t * data_end;

    int error;
    int interim_mode; /* 0 for 16-bit, -1 for 24-bit (matches ape_decode_init) */

    void (*entropy_decode_mono)(struct ape_decoder * ctx, int blockstodecode);
    void (*entropy_decode_stereo)(struct ape_decoder * ctx, int blockstodecode);

    uint32_t current_frame_index;
    uint32_t frame_samples_remaining; /* samples of the current physical frame not yet decoded into decoded[] */
    uint32_t carry_frames;            /* samples ready to be read out of decoded[] */
    uint32_t carry_read_pos;

    uint64_t total_pcm_frames;
    uint64_t current_pcm_frame;
};

/* ---- range decoder (verbatim port) ---- */

#define CODE_BITS 32
#define TOP_VALUE ((unsigned int) 1 << (CODE_BITS - 1))
#define SHIFT_BITS (CODE_BITS - 9)
#define EXTRA_BITS ((CODE_BITS - 2) % 8 + 1)
#define BOTTOM_VALUE (TOP_VALUE >> 8)

static inline void range_start_decoding(ape_decoder_t * ctx) {
    ctx->rc.buffer = *ctx->ptr++;
    ctx->rc.low = ctx->rc.buffer >> (8 - EXTRA_BITS);
    ctx->rc.range = (uint32_t) 1 << EXTRA_BITS;
}

static inline void range_dec_normalize(ape_decoder_t * ctx) {
    while (ctx->rc.range <= BOTTOM_VALUE) {
        ctx->rc.buffer <<= 8;
        if (ctx->ptr < ctx->data_end) {
            ctx->rc.buffer += *ctx->ptr;
            ctx->ptr++;
        } else {
            ctx->error = 1;
        }
        ctx->rc.low = (ctx->rc.low << 8) | ((ctx->rc.buffer >> 1) & 0xFF);
        ctx->rc.range <<= 8;
    }
}

static inline int range_decode_culfreq(ape_decoder_t * ctx, int tot_f) {
    range_dec_normalize(ctx);
    ctx->rc.help = ctx->rc.range / (uint32_t) tot_f;
    return (int) (ctx->rc.low / ctx->rc.help);
}

static inline int range_decode_culshift(ape_decoder_t * ctx, int shift) {
    range_dec_normalize(ctx);
    ctx->rc.help = ctx->rc.range >> shift;
    return (int) (ctx->rc.low / ctx->rc.help);
}

static inline void range_decode_update(ape_decoder_t * ctx, int sy_f, int lt_f) {
    ctx->rc.low -= ctx->rc.help * (uint32_t) lt_f;
    ctx->rc.range = ctx->rc.help * (uint32_t) sy_f;
}

static inline int range_decode_bits(ape_decoder_t * ctx, int n) {
    int sym = range_decode_culshift(ctx, n);
    range_decode_update(ctx, 1, sym);
    return sym;
}

#define MODEL_ELEMENTS 64

static const uint16_t counts_3970[22] = {
        0, 14824, 28224, 39348, 47855, 53994, 58171, 60926,
    62682, 63786, 64463, 64878, 65126, 65276, 65365, 65419,
    65450, 65469, 65480, 65487, 65491, 65493,
};
static const uint16_t counts_diff_3970[21] = {
    14824, 13400, 11124, 8507, 6139, 4177, 2755, 1756,
    1104, 677, 415, 248, 150, 89, 54, 31,
    19, 11, 7, 4, 2,
};
static const uint16_t counts_3980[22] = {
        0, 19578, 36160, 48417, 56323, 60899, 63265, 64435,
    64971, 65232, 65351, 65416, 65447, 65466, 65476, 65482,
    65485, 65488, 65490, 65491, 65492, 65493,
};
static const uint16_t counts_diff_3980[21] = {
    19578, 16582, 12257, 7906, 4576, 2366, 1170, 536,
    261, 119, 65, 31, 19, 10, 6, 3,
    3, 2, 1, 1, 1,
};

static inline int range_get_symbol(ape_decoder_t * ctx, const uint16_t counts[], const uint16_t counts_diff[]) {
    int symbol, cf;

    cf = range_decode_culshift(ctx, 16);

    if (cf > 65492) {
        symbol = cf - 65535 + 63;
        range_decode_update(ctx, 1, cf);
        if (cf > 65535) ctx->error = 1;
        return symbol;
    }
    for (symbol = 0; counts[symbol + 1] <= (uint16_t) cf; symbol++);
    range_decode_update(ctx, counts_diff[symbol], counts[symbol]);
    return symbol;
}

static inline void update_rice(APERice * rice, unsigned int x) {
    int lim = rice->k ? (1 << (rice->k + 4)) : 0;
    rice->ksum += ((x + 1) / 2) - ((rice->ksum + 16) >> 5);

    if (rice->ksum < (unsigned) lim)
        rice->k--;
    else if (rice->ksum >= (1u << (rice->k + 5)) && rice->k < 24)
        rice->k++;
}

static inline int ape_decode_value_3900(ape_decoder_t * ctx, APERice * rice) {
    unsigned int x, overflow;
    int tmpk;

    overflow = (unsigned) range_get_symbol(ctx, counts_3970, counts_diff_3970);

    if (overflow == (MODEL_ELEMENTS - 1)) {
        tmpk = range_decode_bits(ctx, 5);
        overflow = 0;
    } else {
        tmpk = (rice->k < 1) ? 0 : (int) rice->k - 1;
    }

    if (tmpk <= 16) {
        if (tmpk > 23) { ctx->error = 1; return 0; }
        x = (unsigned) range_decode_bits(ctx, tmpk);
    } else if (tmpk <= 31) {
        x = (unsigned) range_decode_bits(ctx, 16);
        x |= ((unsigned) range_decode_bits(ctx, tmpk - 16) << 16);
    } else {
        ctx->error = 1;
        return 0;
    }
    x += overflow << tmpk;

    update_rice(rice, x);
    return (int) (((x >> 1) ^ ((x & 1) - 1)) + 1);
}

static inline int ape_decode_value_3990(ape_decoder_t * ctx, APERice * rice) {
    unsigned int x, overflow, pivot;
    int base;

    pivot = rice->ksum >> 5;
    if (pivot < 1) pivot = 1;

    overflow = (unsigned) range_get_symbol(ctx, counts_3980, counts_diff_3980);

    if (overflow == (MODEL_ELEMENTS - 1)) {
        overflow = (unsigned) range_decode_bits(ctx, 16) << 16;
        overflow |= (unsigned) range_decode_bits(ctx, 16);
    }

    if (pivot < 0x10000) {
        base = range_decode_culfreq(ctx, (int) pivot);
        range_decode_update(ctx, 1, base);
    } else {
        int base_hi = (int) pivot, base_lo;
        int bbits = 0;

        while (base_hi & ~0xFFFF) {
            base_hi >>= 1;
            bbits++;
        }
        base_hi = range_decode_culfreq(ctx, base_hi + 1);
        range_decode_update(ctx, 1, base_hi);
        base_lo = range_decode_culfreq(ctx, 1 << bbits);
        range_decode_update(ctx, 1, base_lo);

        base = (base_hi << bbits) + base_lo;
    }

    x = (unsigned) base + overflow * pivot;

    update_rice(rice, x);
    return (int) (((x >> 1) ^ ((x & 1) - 1)) + 1);
}

static void entropy_decode_mono_3900(ape_decoder_t * ctx, int blockstodecode) {
    int32_t * decoded0 = ctx->decoded[0];
    while (blockstodecode--) *decoded0++ = ape_decode_value_3900(ctx, &ctx->riceY);
}

static void entropy_decode_stereo_3930(ape_decoder_t * ctx, int blockstodecode) {
    int32_t * decoded0 = ctx->decoded[0];
    int32_t * decoded1 = ctx->decoded[1];
    while (blockstodecode--) {
        *decoded0++ = ape_decode_value_3900(ctx, &ctx->riceY);
        *decoded1++ = ape_decode_value_3900(ctx, &ctx->riceX);
    }
}

static void entropy_decode_mono_3990(ape_decoder_t * ctx, int blockstodecode) {
    int32_t * decoded0 = ctx->decoded[0];
    while (blockstodecode--) *decoded0++ = ape_decode_value_3990(ctx, &ctx->riceY);
}

static void entropy_decode_stereo_3990(ape_decoder_t * ctx, int blockstodecode) {
    int32_t * decoded0 = ctx->decoded[0];
    int32_t * decoded1 = ctx->decoded[1];
    while (blockstodecode--) {
        *decoded0++ = ape_decode_value_3990(ctx, &ctx->riceY);
        *decoded1++ = ape_decode_value_3990(ctx, &ctx->riceX);
    }
}

static int init_entropy_decoder(ape_decoder_t * ctx) {
    if (ctx->data_end - ctx->ptr < 6) return -1;

    uint32_t crc = ((uint32_t) ctx->ptr[0] << 24) | ((uint32_t) ctx->ptr[1] << 16) |
                   ((uint32_t) ctx->ptr[2] << 8) | (uint32_t) ctx->ptr[3];
    ctx->ptr += 4;

    ctx->frameflags = 0;
    if (crc & 0x80000000) {
        crc &= ~0x80000000u;
        if (ctx->data_end - ctx->ptr < 6) return -1;
        ctx->frameflags = (int) (((uint32_t) ctx->ptr[0] << 24) | ((uint32_t) ctx->ptr[1] << 16) |
                                  ((uint32_t) ctx->ptr[2] << 8) | (uint32_t) ctx->ptr[3]);
        ctx->ptr += 4;
    }
    (void) crc; /* CRC verification intentionally not implemented -- see README */

    ctx->riceX.k = 10;
    ctx->riceX.ksum = (1u << ctx->riceX.k) * 16;
    ctx->riceY.k = 10;
    ctx->riceY.ksum = (1u << ctx->riceY.k) * 16;

    ctx->ptr++; /* first byte of input is ignored */
    range_start_decoding(ctx);

    return 0;
}

static const int64_t initial_coeffs_3930_64bit[4] = { 360, 317, -109, 98 };

static void init_predictor_decoder(ape_decoder_t * ctx) {
    APEPredictor64 * p = &ctx->predictor64;
    memset(p->historybuffer, 0, PREDICTOR_SIZE * sizeof(*p->historybuffer));
    p->buf = p->historybuffer;

    memcpy(p->coeffsA[0], initial_coeffs_3930_64bit, sizeof(initial_coeffs_3930_64bit));
    memcpy(p->coeffsA[1], initial_coeffs_3930_64bit, sizeof(initial_coeffs_3930_64bit));
    memset(p->coeffsB, 0, sizeof(p->coeffsB));

    p->filterA[0] = p->filterA[1] = 0;
    p->filterB[0] = p->filterB[1] = 0;
    p->lastA[0] = p->lastA[1] = 0;
}

static inline int APESIGN(int64_t x) { return (x < 0) - (x > 0); }

static inline int64_t predictor_update_filter(APEPredictor64 * p, const int64_t decoded, const int filter,
                                              const int delayA, const int delayB,
                                              const int adaptA, const int adaptB,
                                              int interim_mode) {
    int64_t predictionA, predictionB;
    int32_t sign;

    p->buf[delayA] = p->lastA[filter];
    p->buf[adaptA] = APESIGN(p->buf[delayA]);
    p->buf[delayA - 1] = p->buf[delayA] - (uint64_t) p->buf[delayA - 1];
    p->buf[adaptA - 1] = APESIGN(p->buf[delayA - 1]);

    predictionA = p->buf[delayA] * (int64_t) p->coeffsA[filter][0] +
                  p->buf[delayA - 1] * (int64_t) p->coeffsA[filter][1] +
                  p->buf[delayA - 2] * (int64_t) p->coeffsA[filter][2] +
                  p->buf[delayA - 3] * (int64_t) p->coeffsA[filter][3];

    p->buf[delayB] = p->filterA[filter ^ 1] - ((int64_t) (p->filterB[filter] * 31ULL) >> 5);
    p->buf[adaptB] = APESIGN(p->buf[delayB]);
    p->buf[delayB - 1] = p->buf[delayB] - (uint64_t) p->buf[delayB - 1];
    p->buf[adaptB - 1] = APESIGN(p->buf[delayB - 1]);
    p->filterB[filter] = p->filterA[filter ^ 1];

    predictionB = p->buf[delayB] * (int64_t) p->coeffsB[filter][0] +
                  p->buf[delayB - 1] * (int64_t) p->coeffsB[filter][1] +
                  p->buf[delayB - 2] * (int64_t) p->coeffsB[filter][2] +
                  p->buf[delayB - 3] * (int64_t) p->coeffsB[filter][3] +
                  p->buf[delayB - 4] * (int64_t) p->coeffsB[filter][4];

    if (interim_mode < 1) {
        predictionA = (int32_t) predictionA;
        predictionB = (int32_t) predictionB;
        p->lastA[filter] = (int32_t) (decoded + (uint64_t) ((int32_t) (predictionA + (predictionB >> 1)) >> 10));
    } else {
        p->lastA[filter] = decoded + (((int64_t) ((uint64_t) predictionA + (uint64_t) (predictionB >> 1))) >> 10);
    }
    p->filterA[filter] = p->lastA[filter] + ((int64_t) (p->filterA[filter] * 31ULL) >> 5);

    sign = APESIGN(decoded);
    p->coeffsA[filter][0] += (uint64_t) (p->buf[adaptA] * sign);
    p->coeffsA[filter][1] += (uint64_t) (p->buf[adaptA - 1] * sign);
    p->coeffsA[filter][2] += (uint64_t) (p->buf[adaptA - 2] * sign);
    p->coeffsA[filter][3] += (uint64_t) (p->buf[adaptA - 3] * sign);
    p->coeffsB[filter][0] += (uint64_t) (p->buf[adaptB] * sign);
    p->coeffsB[filter][1] += (uint64_t) (p->buf[adaptB - 1] * sign);
    p->coeffsB[filter][2] += (uint64_t) (p->buf[adaptB - 2] * sign);
    p->coeffsB[filter][3] += (uint64_t) (p->buf[adaptB - 3] * sign);
    p->coeffsB[filter][4] += (uint64_t) (p->buf[adaptB - 4] * sign);

    return p->filterA[filter];
}

static void ape_apply_filters(ape_decoder_t * ctx, int32_t * decoded0, int32_t * decoded1, int count);

static void predictor_decode_stereo_3950(ape_decoder_t * ctx, int count) {
    APEPredictor64 * p_default = &ctx->predictor64;
    APEPredictor64 p_interim;
    int lcount = count;
    int num_passes = 1;

    ape_apply_filters(ctx, ctx->decoded[0], ctx->decoded[1], count);
    if (ctx->interim_mode == -1) {
        p_interim = *p_default;
        num_passes++;
        memcpy(ctx->interim[0], ctx->decoded[0], sizeof(*ctx->interim[0]) * (size_t) count);
        memcpy(ctx->interim[1], ctx->decoded[1], sizeof(*ctx->interim[1]) * (size_t) count);
    }

    for (int pass = 0; pass < num_passes; pass++) {
        int32_t *decoded0, *decoded1;
        int interim_mode = ctx->interim_mode > 0 || pass;
        APEPredictor64 * p;

        if (pass) {
            p = &p_interim;
            decoded0 = ctx->interim[0];
            decoded1 = ctx->interim[1];
        } else {
            p = p_default;
            decoded0 = ctx->decoded[0];
            decoded1 = ctx->decoded[1];
        }
        p->buf = p->historybuffer;

        count = lcount;
        while (count--) {
            int64_t a0 = predictor_update_filter(p, *decoded0, 0, YDELAYA, YDELAYB, YADAPTCOEFFSA, YADAPTCOEFFSB, interim_mode);
            int64_t a1 = predictor_update_filter(p, *decoded1, 1, XDELAYA, XDELAYB, XADAPTCOEFFSA, XADAPTCOEFFSB, interim_mode);
            *decoded0++ = (int32_t) a0;
            *decoded1++ = (int32_t) a1;
            if (num_passes > 1) {
                int64_t left = a1 - (uint64_t) (a0 / 2);
                int64_t right = left + (uint64_t) a0;
                int64_t aleft = left < 0 ? -left : left;
                int64_t aright = right < 0 ? -right : right;
                if ((aleft < aright ? aleft : aright) < -(1 << 23)) {
                    ctx->interim_mode = !interim_mode;
                    break;
                }
            }

            p->buf++;
            if (p->buf == p->historybuffer + HISTORY_SIZE) {
                memmove(p->historybuffer, p->buf, PREDICTOR_SIZE * sizeof(*p->historybuffer));
                p->buf = p->historybuffer;
            }
        }
    }
    if (num_passes > 1 && ctx->interim_mode > 0) {
        memcpy(ctx->decoded[0], ctx->interim[0], sizeof(*ctx->interim[0]) * (size_t) lcount);
        memcpy(ctx->decoded[1], ctx->interim[1], sizeof(*ctx->interim[1]) * (size_t) lcount);
        *p_default = p_interim;
        p_default->buf = p_default->historybuffer;
    }
}

static void predictor_decode_mono_3950(ape_decoder_t * ctx, int count) {
    APEPredictor64 * p = &ctx->predictor64;
    int32_t * decoded0 = ctx->decoded[0];
    int64_t predictionA, currentA, A;
    int32_t sign;

    ape_apply_filters(ctx, ctx->decoded[0], NULL, count);

    currentA = p->lastA[0];

    while (count--) {
        A = *decoded0;

        p->buf[YDELAYA] = currentA;
        p->buf[YDELAYA - 1] = p->buf[YDELAYA] - (uint64_t) p->buf[YDELAYA - 1];

        predictionA = p->buf[YDELAYA] * (int64_t) p->coeffsA[0][0] +
                      p->buf[YDELAYA - 1] * (int64_t) p->coeffsA[0][1] +
                      p->buf[YDELAYA - 2] * (int64_t) p->coeffsA[0][2] +
                      p->buf[YDELAYA - 3] * (int64_t) p->coeffsA[0][3];

        currentA = A + (uint64_t) (predictionA >> 10);

        p->buf[YADAPTCOEFFSA] = APESIGN(p->buf[YDELAYA]);
        p->buf[YADAPTCOEFFSA - 1] = APESIGN(p->buf[YDELAYA - 1]);

        sign = APESIGN(A);
        p->coeffsA[0][0] += (uint64_t) (p->buf[YADAPTCOEFFSA] * sign);
        p->coeffsA[0][1] += (uint64_t) (p->buf[YADAPTCOEFFSA - 1] * sign);
        p->coeffsA[0][2] += (uint64_t) (p->buf[YADAPTCOEFFSA - 2] * sign);
        p->coeffsA[0][3] += (uint64_t) (p->buf[YADAPTCOEFFSA - 3] * sign);

        p->buf++;
        if (p->buf == p->historybuffer + HISTORY_SIZE) {
            memmove(p->historybuffer, p->buf, PREDICTOR_SIZE * sizeof(*p->historybuffer));
            p->buf = p->historybuffer;
        }

        p->filterA[0] = currentA + (uint64_t) ((int64_t) (p->filterA[0] * 31U) >> 5);
        *(decoded0++) = (int32_t) p->filterA[0];
    }

    p->lastA[0] = currentA;
}

static void do_init_filter(APEFilter * f, int16_t * buf, int order) {
    f->coeffs = buf;
    f->historybuffer = buf + order;
    f->delay = f->historybuffer + order * 2;
    f->adaptcoeffs = f->historybuffer + order;

    memset(f->historybuffer, 0, (size_t) (order * 2) * sizeof(*f->historybuffer));
    memset(f->coeffs, 0, (size_t) order * sizeof(*f->coeffs));
    f->avg = 0;
}

static void init_filter(APEFilter * f, int16_t * buf, int order) {
    do_init_filter(&f[0], buf, order);
    do_init_filter(&f[1], buf + order * 3 + HISTORY_SIZE, order);
}

/* Computes sum(coeffs[i]*delay[i]) while updating coeffs[i] += adaptcoeffs[i]*mul,
 * matching FFmpeg's lossless_audiodsp scalarproduct_and_madd_int16. */
static int32_t scalarproduct_and_madd_int16(int16_t * v1, const int16_t * v2, const int16_t * v3, int order, int mul) {
    int32_t res = 0;
    for (int i = 0; i < order; i++) {
        res += v1[i] * v2[i];
        v1[i] = (int16_t) (v1[i] + v3[i] * mul);
    }
    return res;
}

static inline int16_t clip_int16(int32_t v) {
    if (v < -32768) return -32768;
    if (v > 32767) return 32767;
    return (int16_t) v;
}

static void do_apply_filter(ape_decoder_t * ctx, APEFilter * f, int32_t * data, int count, int order, int fracbits) {
    (void) ctx;
    int res;
    unsigned absres;

    while (count--) {
        int sign = APESIGN(*data);
        res = scalarproduct_and_madd_int16(f->coeffs, f->delay - order, f->adaptcoeffs - order, order, sign);
        res = (int) (((int64_t) res + (1LL << (fracbits - 1))) >> fracbits);
        res += *data;
        *data++ = res;

        *f->delay++ = clip_int16(res);

        absres = (unsigned) (res < 0 ? -res : res);
        if (absres)
            *f->adaptcoeffs = (int16_t) (APESIGN(res) *
                (8 << ((absres > f->avg * 3ULL) + (absres > (f->avg + f->avg / 3)))));
        else
            *f->adaptcoeffs = 0;

        f->avg += (uint32_t) ((int) (absres - f->avg) / 16);

        f->adaptcoeffs[-1] = (int16_t) (f->adaptcoeffs[-1] >> 1);
        f->adaptcoeffs[-2] = (int16_t) (f->adaptcoeffs[-2] >> 1);
        f->adaptcoeffs[-8] = (int16_t) (f->adaptcoeffs[-8] >> 1);

        f->adaptcoeffs++;

        if (f->delay == f->historybuffer + HISTORY_SIZE + (order * 2)) {
            memmove(f->historybuffer, f->delay - (order * 2), (size_t) (order * 2) * sizeof(*f->historybuffer));
            f->delay = f->historybuffer + order * 2;
            f->adaptcoeffs = f->historybuffer + order;
        }
    }
}

static void apply_filter(ape_decoder_t * ctx, APEFilter * f, int32_t * data0, int32_t * data1, int count, int order, int fracbits) {
    do_apply_filter(ctx, &f[0], data0, count, order, fracbits);
    if (data1) do_apply_filter(ctx, &f[1], data1, count, order, fracbits);
}

static void ape_apply_filters(ape_decoder_t * ctx, int32_t * decoded0, int32_t * decoded1, int count) {
    for (int i = 0; i < APE_FILTER_LEVELS; i++) {
        if (!ape_filter_orders[ctx->fset][i]) break;
        apply_filter(ctx, ctx->filters[i], decoded0, decoded1, count,
                     ape_filter_orders[ctx->fset][i], ape_filter_fracbits[ctx->fset][i]);
    }
}

static int init_frame_decoder(ape_decoder_t * ctx) {
    if (init_entropy_decoder(ctx) < 0) return -1;
    init_predictor_decoder(ctx);

    for (int i = 0; i < APE_FILTER_LEVELS; i++) {
        if (!ape_filter_orders[ctx->fset][i]) break;
        init_filter(ctx->filters[i], ctx->filterbuf[i], ape_filter_orders[ctx->fset][i]);
    }
    return 0;
}

static void ape_unpack_mono(ape_decoder_t * ctx, int count) {
    if (ctx->frameflags & APE_FRAMECODE_STEREO_SILENCE) {
        /* decoded_buffer is a plain malloc, not zeroed -- without this, a
         * silence frame following a non-silent one would replay stale
         * samples left over from the previous frame instead of silence. */
        memset(ctx->decoded[0], 0, (size_t) count * sizeof(*ctx->decoded[0]));
        if (ctx->channels == 2) memset(ctx->decoded[1], 0, (size_t) count * sizeof(*ctx->decoded[1]));
        return;
    }

    ctx->entropy_decode_mono(ctx, count);
    if (ctx->error) return;

    predictor_decode_mono_3950(ctx, count);

    if (ctx->channels == 2) memcpy(ctx->decoded[1], ctx->decoded[0], (size_t) count * sizeof(*ctx->decoded[1]));
}

static void ape_unpack_stereo(ape_decoder_t * ctx, int count) {
    if ((ctx->frameflags & APE_FRAMECODE_STEREO_SILENCE) == APE_FRAMECODE_STEREO_SILENCE) {
        memset(ctx->decoded[0], 0, (size_t) count * sizeof(*ctx->decoded[0]));
        memset(ctx->decoded[1], 0, (size_t) count * sizeof(*ctx->decoded[1]));
        return;
    }

    ctx->entropy_decode_stereo(ctx, count);
    if (ctx->error) return;

    predictor_decode_stereo_3950(ctx, count);

    int32_t * decoded0 = ctx->decoded[0];
    int32_t * decoded1 = ctx->decoded[1];
    while (count--) {
        unsigned left = (unsigned) *decoded1 - (unsigned) (*decoded0 / 2);
        unsigned right = left + (unsigned) *decoded0;
        *(decoded0++) = (int32_t) left;
        *(decoded1++) = (int32_t) right;
    }
}

static bool ensure_swapbuf_capacity(ape_decoder_t * ctx, uint32_t needed) {
    if (needed <= ctx->swapbuf_capacity) return true;
    uint8_t * buf = realloc(ctx->swapbuf, needed);
    if (!buf) return false;
    ctx->swapbuf = buf;
    ctx->swapbuf_capacity = needed;
    return true;
}

/* Decodes physical frame frame_index in full into ctx->decoded[0..channels),
 * ready to be read out via the carry buffer. Mirrors ape_decode_frame, but
 * always decodes the whole frame in one call rather than in
 * blocks_per_loop-sized chunks (fine -- one frame is at most 73728*2 = ~590KB
 * of int32 samples per channel, an acceptable one-shot allocation). */
static bool decode_physical_frame(ape_decoder_t * ctx, uint32_t frame_index) {
    uint32_t skip = ape_demux_get_frame_skip(ctx->demux, frame_index);
    uint32_t raw_size;

    if (!ensure_swapbuf_capacity(ctx, ctx->max_frame_blocks * 8 + 64)) return false;
    if (!ape_demux_read_frame(ctx->demux, frame_index, ctx->swapbuf, ctx->swapbuf_capacity, &raw_size)) return false;

    /* Byte-swap the whole frame as 32-bit words -- a quirk of how Monkey's
     * Audio's bitstream is laid out, matching FFmpeg's bswap_buf call. */
    uint32_t words = raw_size / 4;
    uint32_t * w = (uint32_t *) ctx->swapbuf;
    for (uint32_t i = 0; i < words; i++) {
        uint32_t v = w[i];
        w[i] = ((v & 0xFF) << 24) | ((v & 0xFF00) << 8) | ((v & 0xFF0000) >> 8) | ((v & 0xFF000000) >> 24);
    }

    ctx->ptr = ctx->swapbuf;
    ctx->data_end = ctx->swapbuf + raw_size;
    if (skip > 3 || (uint32_t) (ctx->data_end - ctx->ptr) < skip) return false;
    ctx->ptr += skip;

    if (init_frame_decoder(ctx) < 0) return false;

    uint32_t nblocks = (frame_index == ape_demux_get_frame_count(ctx->demux) - 1)
                           ? ape_demux_get_final_frame_blocks(ctx->demux)
                           : ape_demux_get_blocks_per_frame(ctx->demux);

    ctx->error = 0;
    if (ctx->channels == 1 || (ctx->frameflags & APE_FRAMECODE_PSEUDO_STEREO))
        ape_unpack_mono(ctx, (int) nblocks);
    else
        ape_unpack_stereo(ctx, (int) nblocks);

    if (ctx->error) return false;

    ctx->carry_frames = nblocks;
    ctx->carry_read_pos = 0;
    ctx->current_frame_index = frame_index;
    return true;
}

ape_decoder_t * ape_open_file(const char * path) {
    ape_demux_t * demux = ape_demux_open(path);
    if (!demux) return NULL;

    int fileversion = ape_demux_get_fileversion(demux);
    int compression_level = ape_demux_get_compression_level(demux);
    unsigned int channels = ape_demux_get_channels(demux);
    unsigned int bps = ape_demux_get_bits_per_sample(demux);

    if (compression_level % 1000 || compression_level > 5000 || compression_level == 0) {
        ape_demux_close(demux);
        return NULL;
    }
    int fset = compression_level / 1000 - 1;

    ape_decoder_t * ctx = calloc(1, sizeof(*ctx));
    ctx->demux = demux;
    ctx->channels = (int) channels;
    ctx->bps = (int) bps;
    ctx->fileversion = fileversion;
    ctx->compression_level = compression_level;
    ctx->fset = fset;
    ctx->interim_mode = (bps == 24) ? -1 : 0;

    for (int i = 0; i < APE_FILTER_LEVELS; i++) {
        if (!ape_filter_orders[fset][i]) break;
        size_t elems = (size_t) (ape_filter_orders[fset][i] * 3 + HISTORY_SIZE) * 2;
        ctx->filterbuf[i] = malloc(elems * sizeof(int16_t));
        if (!ctx->filterbuf[i]) { ape_close(ctx); return NULL; }
    }

    if (fileversion < 3990) {
        ctx->entropy_decode_mono = entropy_decode_mono_3900;
        ctx->entropy_decode_stereo = entropy_decode_stereo_3930;
    } else {
        ctx->entropy_decode_mono = entropy_decode_mono_3990;
        ctx->entropy_decode_stereo = entropy_decode_stereo_3990;
    }

    ctx->max_frame_blocks = ape_demux_get_blocks_per_frame(demux);
    uint32_t aligned = (ctx->max_frame_blocks + 7u) & ~7u;
    ctx->decoded_buffer = malloc(2 * (size_t) aligned * sizeof(int32_t));
    if (!ctx->decoded_buffer) { ape_close(ctx); return NULL; }
    ctx->decoded[0] = ctx->decoded_buffer;
    ctx->decoded[1] = ctx->decoded_buffer + aligned;

    if (ctx->channels == 2 && ctx->interim_mode == -1) {
        ctx->interim_buffer = malloc(2 * (size_t) aligned * sizeof(int32_t));
        if (!ctx->interim_buffer) { ape_close(ctx); return NULL; }
        ctx->interim[0] = ctx->interim_buffer;
        ctx->interim[1] = ctx->interim_buffer + aligned;
    }

    ctx->total_pcm_frames = ape_demux_get_total_samples(demux);

    if (!decode_physical_frame(ctx, 0)) {
        ape_close(ctx);
        return NULL;
    }

    return ctx;
}

unsigned int ape_get_channels(const ape_decoder_t * dec) { return (unsigned int) dec->channels; }
unsigned int ape_get_sample_rate(const ape_decoder_t * dec) { return ape_demux_get_sample_rate(dec->demux); }
unsigned int ape_get_bits_per_sample(const ape_decoder_t * dec) { return (unsigned int) dec->bps; }
uint64_t ape_get_total_pcm_frame_count(const ape_decoder_t * dec) { return dec->total_pcm_frames; }

uint64_t ape_read_pcm_frames_s16(ape_decoder_t * dec, uint64_t frames_to_read, int16_t * buffer_out) {
    uint64_t frames_written = 0;
    int shift = (dec->bps == 24) ? 8 : 0;

    while (frames_written < frames_to_read) {
        if (dec->carry_read_pos >= dec->carry_frames) {
            uint32_t next_frame = dec->current_frame_index + 1;
            if (next_frame >= ape_demux_get_frame_count(dec->demux)) break;
            if (!decode_physical_frame(dec, next_frame)) break;
        }

        uint64_t available = dec->carry_frames - dec->carry_read_pos;
        uint64_t to_copy = frames_to_read - frames_written;
        if (to_copy > available) to_copy = available;

        for (uint64_t i = 0; i < to_copy; i++) {
            uint32_t src_pos = dec->carry_read_pos + (uint32_t) i;
            for (int ch = 0; ch < dec->channels; ch++) {
                int32_t v = dec->decoded[ch][src_pos];
                buffer_out[(frames_written + i) * (uint64_t) dec->channels + (uint64_t) ch] = clip_int16(v >> shift);
            }
        }

        dec->carry_read_pos += (uint32_t) to_copy;
        frames_written += to_copy;
        dec->current_pcm_frame += to_copy;
    }

    return frames_written;
}

bool ape_seek_to_pcm_frame(ape_decoder_t * dec, uint64_t frame_index) {
    if (frame_index > dec->total_pcm_frames) frame_index = dec->total_pcm_frames;

    uint32_t blocksperframe = ape_demux_get_blocks_per_frame(dec->demux);
    uint32_t target_frame = blocksperframe > 0 ? (uint32_t) (frame_index / blocksperframe) : 0;
    uint32_t frame_count = ape_demux_get_frame_count(dec->demux);
    if (target_frame >= frame_count) target_frame = frame_count > 0 ? frame_count - 1 : 0;

    if (!decode_physical_frame(dec, target_frame)) return false;

    uint64_t frame_start = (uint64_t) target_frame * blocksperframe;
    uint64_t within = frame_index > frame_start ? frame_index - frame_start : 0;
    if (within > dec->carry_frames) within = dec->carry_frames;

    dec->carry_read_pos = (uint32_t) within;
    dec->current_pcm_frame = frame_start + within;
    return true;
}

void ape_close(ape_decoder_t * dec) {
    if (!dec) return;
    for (int i = 0; i < APE_FILTER_LEVELS; i++) free(dec->filterbuf[i]);
    free(dec->decoded_buffer);
    free(dec->interim_buffer);
    free(dec->swapbuf);
    if (dec->demux) ape_demux_close(dec->demux);
    free(dec);
}
