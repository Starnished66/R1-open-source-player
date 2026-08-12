/*
 * WMA (v1/v2, "WMA Standard") decoder -- a from-scratch reimplementation of
 * the algorithm FFmpeg's libavcodec/wma.c + wmadec.c describe (the codec
 * was never officially published by Microsoft; FFmpeg's decoder is the
 * practical reference used to understand the bitstream, not vendored
 * code). See wma_decoder.h for scope and verification status.
 */

#include "wma_decoder.h"
#include "asf_demux.h"
#include "wma_tables.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define BLOCK_MIN_BITS 7
#define BLOCK_MAX_BITS 11
#define BLOCK_MAX_SIZE (1 << BLOCK_MAX_BITS)
#define BLOCK_NB_SIZES (BLOCK_MAX_BITS - BLOCK_MIN_BITS + 1)
#define HIGH_BAND_MAX_SIZE 16
#define NB_LSP_COEFS 10
#define MAX_CHANNELS 2
#define NOISE_TAB_SIZE 8192
#define LSP_POW_BITS 7
#define MAX_FRAMES_PER_SUPERFRAME 16

/* ---- bit reader: MSB-first, matching the ASF/WMA bitstream convention ---- */

typedef struct {
    const uint8_t * buf;
    uint32_t size_bits;
    uint32_t bit_pos;
} bitreader_t;

static void br_init(bitreader_t * b, const uint8_t * buf, uint32_t size_bytes) {
    b->buf = buf;
    b->size_bits = size_bytes * 8;
    b->bit_pos = 0;
}

static uint32_t br_bits_left(const bitreader_t * b) {
    return b->bit_pos >= b->size_bits ? 0 : b->size_bits - b->bit_pos;
}

/* Caller-guaranteed: buf has at least 4 bytes of readable padding past
 * size_bits/8 (wma_open_file allocates frame_buf with slack for this). */
static uint32_t br_get_bits(bitreader_t * b, int n) {
    uint32_t result = 0;
    int remaining = n;
    while (remaining > 0) {
        int take = remaining > 24 ? 24 : remaining;
        uint32_t byte_idx = b->bit_pos >> 3;
        uint32_t bit_off = b->bit_pos & 7;
        uint32_t v = ((uint32_t) b->buf[byte_idx] << 24) | ((uint32_t) b->buf[byte_idx + 1] << 16) |
                     ((uint32_t) b->buf[byte_idx + 2] << 8) | (uint32_t) b->buf[byte_idx + 3];
        v <<= bit_off;
        v >>= (32 - take);
        result = (result << take) | v;
        b->bit_pos += (uint32_t) take;
        remaining -= take;
    }
    return result;
}

static int br_get_bits1(bitreader_t * b) { return (int) br_get_bits(b, 1); }
static void br_skip_bits(bitreader_t * b, int n) { b->bit_pos += (uint32_t) n; }
static uint32_t br_get_bits_count(const bitreader_t * b) { return b->bit_pos; }
static void br_align(bitreader_t * b) { b->bit_pos = (b->bit_pos + 7) & ~7u; }

/* ---- generic prefix-code (Huffman) trie: build from explicit (code,bits)
 * pairs, or from a length-only list using the same sequential canonical
 * code assignment ASF/WMA's own tables were generated with ---- */

typedef struct {
    int child[2];
    int symbol; /* >= 0 only on leaves */
} huff_node_t;

typedef struct {
    huff_node_t * nodes;
    int count;
} huff_tree_t;

static int huff_new_node(huff_tree_t * t) {
    t->nodes[t->count].child[0] = -1;
    t->nodes[t->count].child[1] = -1;
    t->nodes[t->count].symbol = -1;
    return t->count++;
}

static void huff_insert(huff_tree_t * t, uint32_t code, int bits, int symbol) {
    int node = 0;
    for (int b = bits - 1; b >= 0; b--) {
        int bit = (code >> b) & 1;
        if (t->nodes[node].child[bit] < 0) t->nodes[node].child[bit] = huff_new_node(t);
        node = t->nodes[node].child[bit];
    }
    t->nodes[node].symbol = symbol;
}

static bool huff_build(huff_tree_t * t, const uint32_t * codes, const uint8_t * bits, int n) {
    t->nodes = malloc(sizeof(huff_node_t) * (size_t) (2 * n + 1));
    if (!t->nodes) return false;
    t->count = 0;
    huff_new_node(t); /* root */
    for (int i = 0; i < n; i++) huff_insert(t, codes[i], bits[i], i);
    return true;
}

static bool huff_build_from_lengths(huff_tree_t * t, const uint8_t * lens, const uint8_t * symbols, int offset,
                                     int n) {
    t->nodes = malloc(sizeof(huff_node_t) * (size_t) (2 * n + 1));
    if (!t->nodes) return false;
    t->count = 0;
    huff_new_node(t);
    uint32_t code = 0;
    for (int i = 0; i < n; i++) {
        int len = lens[i];
        uint32_t bits = code >> (32 - len);
        huff_insert(t, bits, len, (int) symbols[i] + offset);
        code += 1u << (32 - len);
    }
    return true;
}

static void huff_free(huff_tree_t * t) {
    free(t->nodes);
    t->nodes = NULL;
}

static int huff_decode(const huff_tree_t * t, bitreader_t * gb) {
    int node = 0;
    while (t->nodes[node].symbol < 0) {
        int bit = br_get_bits1(gb);
        node = t->nodes[node].child[bit];
        if (node < 0) return -1;
    }
    return t->nodes[node].symbol;
}

/* ---- coefficient run/level VLC (built once at open time from the
 * format-mandated tables in wma_tables.h) ---- */

typedef struct {
    huff_tree_t tree;
    uint16_t * run_table;
    float * level_table;
    int n;
} coef_vlc_ctx_t;

static bool coef_vlc_build(coef_vlc_ctx_t * ctx, const wma_coef_vlc_table_t * src) {
    if (!huff_build(&ctx->tree, src->huffcodes, src->huffbits, src->n)) return false;
    ctx->n = src->n;
    ctx->run_table = calloc((size_t) src->n, sizeof(uint16_t));
    ctx->level_table = calloc((size_t) src->n, sizeof(float));
    if (!ctx->run_table || !ctx->level_table) return false;
    int i = 2, level = 1, k = 0;
    while (i < src->n && k < src->max_level) {
        int l = src->levels[k++];
        for (int j = 0; j < l && i < src->n; j++) {
            ctx->run_table[i] = (uint16_t) j;
            ctx->level_table[i] = (float) level;
            i++;
        }
        level++;
    }
    return true;
}

/* ---- decoder state ---- */

struct wma_decoder {
    asf_demux_t * demux;

    unsigned int channels;
    unsigned int sample_rate;
    uint16_t block_align;
    int version; /* 1 or 2 */

    int use_exp_vlc;
    int use_bit_reservoir;
    int use_variable_block_len;
    int use_noise_coding;

    int byte_offset_bits;

    int frame_len_bits;
    int frame_len;
    int nb_block_sizes;

    int coefs_start;
    int coefs_end[BLOCK_NB_SIZES];
    int exponent_sizes[BLOCK_NB_SIZES];
    uint16_t exponent_bands[BLOCK_NB_SIZES][25];
    int high_band_start[BLOCK_NB_SIZES];
    int exponent_high_sizes[BLOCK_NB_SIZES];
    int exponent_high_bands[BLOCK_NB_SIZES][HIGH_BAND_MAX_SIZE];

    float noise_mult;
    float * noise_table; /* NOISE_TAB_SIZE entries */
    int noise_index;

    float lsp_cos_table[BLOCK_MAX_SIZE];
    float lsp_pow_e_table[256];
    float lsp_pow_m_table1[1 << LSP_POW_BITS];
    float lsp_pow_m_table2[1 << LSP_POW_BITS];

    huff_tree_t exp_vlc;
    huff_tree_t hgain_vlc;
    coef_vlc_ctx_t coef_vlc[2];

    float * windows[BLOCK_NB_SIZES]; /* windows[bsize] has length frame_len>>bsize */

    /* per-block state, persists across calls (variable block length /
     * exponent-reuse bookkeeping) */
    int reset_block_lengths;
    int block_len_bits;
    int next_block_len_bits;
    int prev_block_len_bits;
    int block_len;
    int block_pos;

    uint8_t ms_stereo;
    uint8_t channel_coded[MAX_CHANNELS];
    int exponents_bsize[MAX_CHANNELS];
    int exponents_initialized[MAX_CHANNELS];
    float exponents[MAX_CHANNELS][BLOCK_MAX_SIZE];
    float max_exponent[MAX_CHANNELS];

    int high_band_coded[MAX_CHANNELS][HIGH_BAND_MAX_SIZE];
    int high_band_values[MAX_CHANNELS][HIGH_BAND_MAX_SIZE];

    float coefs1[MAX_CHANNELS][BLOCK_MAX_SIZE];
    float coefs[MAX_CHANNELS][BLOCK_MAX_SIZE];

    float output[BLOCK_MAX_SIZE * 2];
    float * frame_out[MAX_CHANNELS]; /* size 2*frame_len, persists for overlap-add */

    bitreader_t gb;

    /* bit-reservoir carry-over between ASF payloads */
    uint8_t * reservoir;
    uint32_t reservoir_capacity;
    uint32_t reservoir_len;
    int last_bitoffset;

    uint8_t * frame_buf; /* raw ASF payload staging, with read-ahead padding */
    uint32_t frame_buf_capacity;

    int16_t * pcm_buf; /* interleaved, MAX_FRAMES_PER_SUPERFRAME*frame_len*channels capacity */
    uint32_t pcm_buf_count;
    uint32_t pcm_buf_pos;

    uint64_t total_pcm_frames;
    uint64_t skip_frames_remaining; /* startup decoder delay (frame_len*2), same as FFmpeg's avctx->delay */
    bool eof;
    bool flush_done;
};

static int ilog2_floor(unsigned int v) {
    int r = 0;
    if (!v) return 0;
    while (v > 1) {
        v >>= 1;
        r++;
    }
    return r;
}

static int wma_total_gain_to_bits(int total_gain) {
    if (total_gain < 15) return 13;
    if (total_gain < 32) return 12;
    if (total_gain < 40) return 11;
    if (total_gain < 45) return 10;
    return 9;
}

/* ---- LSP-based exponent decoding (x^-0.25 curve fit, same approach Vorbis
 * uses) -- only exercised when the stream's flags select LSP over VLC+delta
 * exponent coding ---- */

static void wma_lsp_pow_tables_init(wma_decoder_t * s) {
    for (int i = 0; i < 256; i++) {
        int e = i - 126;
        s->lsp_pow_e_table[i] = exp2f((float) e * -0.25f);
    }
    float b = 1.0f;
    for (int i = (1 << LSP_POW_BITS) - 1; i >= 0; i--) {
        int m = (1 << LSP_POW_BITS) + i;
        float a = (float) m * (0.5f / (1 << LSP_POW_BITS));
        a = 1.0f / sqrtf(sqrtf(a));
        s->lsp_pow_m_table1[i] = 2 * a - b;
        s->lsp_pow_m_table2[i] = b - a;
        b = a;
    }
    double wdel = M_PI / s->frame_len;
    for (int i = 0; i < s->frame_len; i++) s->lsp_cos_table[i] = (float) (2.0 * cos(wdel * i));
}

static float wma_pow_m1_4(wma_decoder_t * s, float x) {
    union {
        float f;
        unsigned int v;
    } u, t;
    u.f = x;
    unsigned int e = u.v >> 23;
    unsigned int m = (u.v >> (23 - LSP_POW_BITS)) & ((1 << LSP_POW_BITS) - 1);
    t.v = ((u.v << LSP_POW_BITS) & ((1 << 23) - 1)) | (127u << 23);
    float a = s->lsp_pow_m_table1[m];
    float b = s->lsp_pow_m_table2[m];
    return s->lsp_pow_e_table[e & 0xFF] * (a + b * t.f);
}

static void wma_lsp_to_curve(wma_decoder_t * s, float * out, float * val_max_ptr, int n, const float * lsp) {
    float val_max = 0;
    for (int i = 0; i < n; i++) {
        float p = 0.5f, q = 0.5f;
        float w = s->lsp_cos_table[i];
        for (int j = 1; j < NB_LSP_COEFS; j += 2) {
            q *= w - lsp[j - 1];
            p *= w - lsp[j];
        }
        p *= p * (2.0f - w);
        q *= q * (2.0f + w);
        float v = wma_pow_m1_4(s, p + q);
        if (v > val_max) val_max = v;
        out[i] = v;
    }
    *val_max_ptr = val_max;
}

static void wma_decode_exp_lsp(wma_decoder_t * s, int ch) {
    float lsp_coefs[NB_LSP_COEFS];
    for (int i = 0; i < NB_LSP_COEFS; i++) {
        int val = (i == 0 || i >= 8) ? (int) br_get_bits(&s->gb, 3) : (int) br_get_bits(&s->gb, 4);
        lsp_coefs[i] = wma_lsp_codebook[i][val];
    }
    wma_lsp_to_curve(s, s->exponents[ch], &s->max_exponent[ch], s->block_len, lsp_coefs);
}

/* pow(10, i/16) for i in -60..95, matching the AAC-scalefactor-style delta
 * range used by the exponent VLC below (computed, not table-copied --
 * a plain power-of-ten ladder is not bitstream-mandated data). */
static float wma_pow_tab[156];
static bool wma_pow_tab_ready = false;
static void wma_pow_tab_init(void) {
    if (wma_pow_tab_ready) return;
    for (int i = 0; i < 156; i++) wma_pow_tab[i] = powf(10.0f, (float) (i - 60) * 0.0625f);
    wma_pow_tab_ready = true;
}

static int wma_decode_exp_vlc(wma_decoder_t * s, int ch) {
    const uint16_t * ptr = s->exponent_bands[s->frame_len_bits - s->block_len_bits];
    float * q = s->exponents[ch];
    float * q_end = q + s->block_len;
    float max_scale = 0;
    int last_exp;

    if (s->version == 1) {
        last_exp = (int) br_get_bits(&s->gb, 5) + 10;
        float v = wma_pow_tab[last_exp + 60];
        max_scale = v;
        int n = *ptr++;
        for (int i = 0; i < n && q < q_end; i++) *q++ = v;
    } else {
        last_exp = 36;
    }

    while (q < q_end) {
        int code = huff_decode(&s->exp_vlc, &s->gb);
        if (code < 0) return -1;
        last_exp += code - 60;
        if ((unsigned) (last_exp + 60) >= 156) return -1;
        float v = wma_pow_tab[last_exp + 60];
        if (v > max_scale) max_scale = v;
        int n = *ptr++;
        for (int i = 0; i < n && q < q_end; i++) *q++ = v;
    }
    s->max_exponent[ch] = max_scale;
    return 0;
}

/* ---- spectral coefficient run/level decoding ---- */

static int wma_run_level_decode(bitreader_t * gb, const coef_vlc_ctx_t * vlc, float * ptr, int offset,
                                 int num_coefs, int block_len, int frame_len_bits, int coef_nb_bits) {
    const unsigned int coef_mask = (unsigned int) block_len - 1;
    for (; offset < num_coefs; offset++) {
        int code = huff_decode(&vlc->tree, gb);
        if (code < 0) return -1;
        if (code > 1) {
            offset += vlc->run_table[code];
            int sign_bit = br_get_bits1(gb);
            float level = vlc->level_table[code];
            ptr[(unsigned) offset & coef_mask] = sign_bit ? level : -level;
        } else if (code == 1) {
            break; /* EOB */
        } else {
            int level = (int) br_get_bits(gb, coef_nb_bits);
            offset += (int) br_get_bits(gb, frame_len_bits);
            int sign_bit = br_get_bits1(gb);
            ptr[(unsigned) offset & coef_mask] = sign_bit ? (float) level : -(float) level;
        }
    }
    return offset > num_coefs ? -1 : 0;
}

/* ---- IMDCT: direct type-IV DCT synthesis (standard MDCT reconstruction
 * formula, e.g. Vorbis spec 4.3.4), using an angle-recurrence instead of
 * calling cos() per sample/coefficient pair to keep it affordable on an
 * FPU-less-accelerated embedded target. block_len input coefficients ->
 * 2*block_len output samples. ---- */

static void wma_imdct(const float * coefs, float * out, int block_len) {
    int M = block_len;
    int N = 2 * M;
    double wdel = M_PI / (4.0 * M);
    double base_delta = 2.0 * wdel;
    double base = wdel * (1.0 + M);

    for (int n = 0; n < N; n++) {
        double delta = 2.0 * base;
        double cd = cos(delta), sd = sin(delta);
        double cur_c = cos(base), cur_s = sin(base);
        double sum = 0.0;
        for (int k = 0; k < M; k++) {
            sum += coefs[k] * cur_c;
            double nc = cur_c * cd - cur_s * sd;
            double ns = cur_c * sd + cur_s * cd;
            cur_c = nc;
            cur_s = ns;
        }
        out[n] = (float) -sum;
        base += base_delta;
    }
}

static void vec_fmul_add(float * dst, const float * a, const float * b, const float * c, int n) {
    for (int i = 0; i < n; i++) dst[i] = a[i] * b[i] + c[i];
}

static void vec_fmul_reverse(float * dst, const float * a, const float * b, int n) {
    for (int i = 0; i < n; i++) dst[i] = a[i] * b[n - 1 - i];
}

static void wma_window(wma_decoder_t * s, float * out) {
    const float * in = s->output;
    int block_len, bsize, n;

    if (s->block_len_bits <= s->prev_block_len_bits) {
        block_len = s->block_len;
        bsize = s->frame_len_bits - s->block_len_bits;
        vec_fmul_add(out, in, s->windows[bsize], out, block_len);
    } else {
        block_len = 1 << s->prev_block_len_bits;
        n = (s->block_len - block_len) / 2;
        bsize = s->frame_len_bits - s->prev_block_len_bits;
        vec_fmul_add(out + n, in + n, s->windows[bsize], out + n, block_len);
        memcpy(out + n + block_len, in + n + block_len, (size_t) n * sizeof(float));
    }

    out += s->block_len;
    in += s->block_len;

    if (s->block_len_bits <= s->next_block_len_bits) {
        block_len = s->block_len;
        bsize = s->frame_len_bits - s->block_len_bits;
        vec_fmul_reverse(out, in, s->windows[bsize], block_len);
    } else {
        block_len = 1 << s->next_block_len_bits;
        n = (s->block_len - block_len) / 2;
        bsize = s->frame_len_bits - s->next_block_len_bits;
        memcpy(out, in, (size_t) n * sizeof(float));
        vec_fmul_reverse(out + n, in + n, s->windows[bsize], block_len);
        memset(out + n + block_len, 0, (size_t) n * sizeof(float));
    }
}

/* ---- per-block decode: returns 1 if this was the last block of the
 * frame, 0 if more blocks follow, -1 on unrecoverable bitstream error ---- */

static int wma_decode_block(wma_decoder_t * s) {
    int channels = (int) s->channels;
    int bsize, total_gain, coef_nb_bits;
    int nb_coefs[MAX_CHANNELS];

    if (s->use_variable_block_len) {
        int n = ilog2_floor((unsigned) (s->nb_block_sizes - 1)) + 1;
        if (s->reset_block_lengths) {
            s->reset_block_lengths = 0;
            int v = (int) br_get_bits(&s->gb, n);
            if (v >= s->nb_block_sizes) return -1;
            s->prev_block_len_bits = s->frame_len_bits - v;
            v = (int) br_get_bits(&s->gb, n);
            if (v >= s->nb_block_sizes) return -1;
            s->block_len_bits = s->frame_len_bits - v;
        } else {
            s->prev_block_len_bits = s->block_len_bits;
            s->block_len_bits = s->next_block_len_bits;
        }
        int v = (int) br_get_bits(&s->gb, n);
        if (v >= s->nb_block_sizes) return -1;
        s->next_block_len_bits = s->frame_len_bits - v;
    } else {
        s->next_block_len_bits = s->frame_len_bits;
        s->prev_block_len_bits = s->frame_len_bits;
        s->block_len_bits = s->frame_len_bits;
    }

    if (s->frame_len_bits - s->block_len_bits >= s->nb_block_sizes) return -1;

    s->block_len = 1 << s->block_len_bits;
    if (s->block_pos + s->block_len > s->frame_len) return -1;

    if (channels == 2) s->ms_stereo = (uint8_t) br_get_bits1(&s->gb);
    int any_coded = 0;
    for (int ch = 0; ch < channels; ch++) {
        int a = br_get_bits1(&s->gb);
        s->channel_coded[ch] = (uint8_t) a;
        any_coded |= a;
    }

    bsize = s->frame_len_bits - s->block_len_bits;

    if (!any_coded) goto next;

    total_gain = 1;
    for (;;) {
        if (br_bits_left(&s->gb) < 7) return -1;
        int a = (int) br_get_bits(&s->gb, 7);
        total_gain += a;
        if (a != 127) break;
    }
    coef_nb_bits = wma_total_gain_to_bits(total_gain);

    {
        int n = s->coefs_end[bsize] - s->coefs_start;
        for (int ch = 0; ch < channels; ch++) nb_coefs[ch] = n;
    }

    if (s->use_noise_coding) {
        for (int ch = 0; ch < channels; ch++) {
            if (s->channel_coded[ch]) {
                int n = s->exponent_high_sizes[bsize];
                for (int i = 0; i < n; i++) {
                    int a = br_get_bits1(&s->gb);
                    s->high_band_coded[ch][i] = a;
                    if (a) nb_coefs[ch] -= s->exponent_high_bands[bsize][i];
                }
            }
        }
        for (int ch = 0; ch < channels; ch++) {
            if (s->channel_coded[ch]) {
                int n = s->exponent_high_sizes[bsize];
                int val = (int) 0x80000000;
                for (int i = 0; i < n; i++) {
                    if (s->high_band_coded[ch][i]) {
                        if (val == (int) 0x80000000) {
                            val = (int) br_get_bits(&s->gb, 7) - 19;
                        } else {
                            int code = huff_decode(&s->hgain_vlc, &s->gb);
                            if (code < 0) return -1;
                            val += code;
                        }
                        s->high_band_values[ch][i] = val;
                    }
                }
            }
        }
    }

    if (s->block_len_bits == s->frame_len_bits || br_get_bits1(&s->gb)) {
        for (int ch = 0; ch < channels; ch++) {
            if (s->channel_coded[ch]) {
                int ret = s->use_exp_vlc ? wma_decode_exp_vlc(s, ch) : (wma_decode_exp_lsp(s, ch), 0);
                if (ret < 0) return -1;
                s->exponents_bsize[ch] = bsize;
                s->exponents_initialized[ch] = 1;
            }
        }
    }
    for (int ch = 0; ch < channels; ch++) {
        if (s->channel_coded[ch] && !s->exponents_initialized[ch]) return -1;
    }

    for (int ch = 0; ch < channels; ch++) {
        if (s->channel_coded[ch]) {
            int tindex = (ch == 1 && s->ms_stereo) ? 1 : 0;
            float * ptr = s->coefs1[ch];
            memset(ptr, 0, (size_t) s->block_len * sizeof(float));
            if (wma_run_level_decode(&s->gb, &s->coef_vlc[tindex], ptr, 0, nb_coefs[ch], s->block_len,
                                      s->frame_len_bits, coef_nb_bits) < 0)
                return -1;
        }
        if (s->version == 1 && channels >= 2) br_align(&s->gb);
    }

    {
        int n4 = s->block_len / 2;
        float mdct_norm = 1.0f / (float) n4;
        if (s->version == 1) mdct_norm *= sqrtf((float) n4);

        for (int ch = 0; ch < channels; ch++) {
            if (!s->channel_coded[ch]) continue;

            float * coefs1 = s->coefs1[ch];
            const float * exponents = s->exponents[ch];
            int esize = s->exponents_bsize[ch];
            float mult = powf(10.0f, (float) total_gain * 0.05f) / s->max_exponent[ch];
            mult *= mdct_norm;
            float * coefs = s->coefs[ch];

            if (s->use_noise_coding) {
                float mult1 = mult;
                for (int i = 0; i < s->coefs_start; i++) {
                    *coefs++ = s->noise_table[s->noise_index] * exponents[(i << bsize) >> esize] * mult1;
                    s->noise_index = (s->noise_index + 1) & (NOISE_TAB_SIZE - 1);
                }

                int n1 = s->exponent_high_sizes[bsize];
                float exp_power[HIGH_BAND_MAX_SIZE];
                const float * exp_ptr = exponents + ((s->high_band_start[bsize] << bsize) >> esize);
                int last_high_band = 0;
                for (int j = 0; j < n1; j++) {
                    int n = s->exponent_high_bands[bsize][j];
                    if (s->high_band_coded[ch][j]) {
                        float e2 = 0;
                        for (int i = 0; i < n; i++) {
                            float v = exp_ptr[(i << bsize) >> esize];
                            e2 += v * v;
                        }
                        exp_power[j] = e2 / (float) n;
                        last_high_band = j;
                    }
                    exp_ptr += (n << bsize) >> esize;
                }

                exp_ptr = exponents + ((s->coefs_start << bsize) >> esize);
                for (int j = -1; j < n1; j++) {
                    int n = (j < 0) ? (s->high_band_start[bsize] - s->coefs_start) : s->exponent_high_bands[bsize][j];
                    if (j >= 0 && s->high_band_coded[ch][j]) {
                        float mult1 = sqrtf(exp_power[j] / exp_power[last_high_band]);
                        mult1 *= powf(10.0f, (float) s->high_band_values[ch][j] * 0.05f);
                        mult1 /= (s->max_exponent[ch] * s->noise_mult);
                        mult1 *= mdct_norm;
                        for (int i = 0; i < n; i++) {
                            float noise = s->noise_table[s->noise_index];
                            s->noise_index = (s->noise_index + 1) & (NOISE_TAB_SIZE - 1);
                            *coefs++ = noise * exp_ptr[(i << bsize) >> esize] * mult1;
                        }
                    } else {
                        for (int i = 0; i < n; i++) {
                            float noise = s->noise_table[s->noise_index];
                            s->noise_index = (s->noise_index + 1) & (NOISE_TAB_SIZE - 1);
                            *coefs++ = (*coefs1++ + noise) * exp_ptr[(i << bsize) >> esize] * mult;
                        }
                    }
                    exp_ptr += (n << bsize) >> esize;
                }

                int n = s->block_len - s->coefs_end[bsize];
                mult1 = mult * exp_ptr[(-(1 << bsize)) >> esize];
                for (int i = 0; i < n; i++) {
                    *coefs++ = s->noise_table[s->noise_index] * mult1;
                    s->noise_index = (s->noise_index + 1) & (NOISE_TAB_SIZE - 1);
                }
            } else {
                for (int i = 0; i < s->coefs_start; i++) *coefs++ = 0.0f;
                int n = nb_coefs[ch];
                for (int i = 0; i < n; i++) *coefs++ = coefs1[i] * exponents[(i << bsize) >> esize] * mult;
                n = s->block_len - s->coefs_end[bsize];
                for (int i = 0; i < n; i++) *coefs++ = 0.0f;
            }
        }
    }

    if (s->ms_stereo && s->channel_coded[1]) {
        if (!s->channel_coded[0]) {
            memset(s->coefs[0], 0, (size_t) s->block_len * sizeof(float));
            s->channel_coded[0] = 1;
        }
        for (int i = 0; i < s->block_len; i++) {
            float a = s->coefs[0][i], b = s->coefs[1][i];
            s->coefs[0][i] = a + b;
            s->coefs[1][i] = a - b;
        }
    }

next:
    for (int ch = 0; ch < channels; ch++) {
        int n4 = s->block_len / 2;
        if (s->channel_coded[ch]) {
            wma_imdct(s->coefs[ch], s->output, s->block_len);
        } else if (!(s->ms_stereo && ch == 1)) {
            memset(s->output, 0, sizeof(s->output));
        }
        int index = (s->frame_len / 2) + s->block_pos - n4;
        wma_window(s, &s->frame_out[ch][index]);
    }

    s->block_pos += s->block_len;
    return s->block_pos >= s->frame_len ? 1 : 0;
}

static int wma_decode_frame(wma_decoder_t * s, int16_t * pcm_out) {
    s->block_pos = 0;
    for (;;) {
        int ret = wma_decode_block(s);
        if (ret < 0) return ret;
        if (ret) break;
    }
    for (unsigned int ch = 0; ch < s->channels; ch++) {
        for (int i = 0; i < s->frame_len; i++) {
            float samp = s->frame_out[ch][i];
            long iv = lrintf(samp);
            if (iv > 32767) iv = 32767;
            else if (iv < -32768) iv = -32768;
            pcm_out[i * (int) s->channels + (int) ch] = (int16_t) iv;
        }
        memmove(s->frame_out[ch], s->frame_out[ch] + s->frame_len, (size_t) s->frame_len * sizeof(float));
    }
    return 0;
}

/* ---- superframe (one ASF payload's worth of bytes, exactly block_align)
 * decode: handles the optional shared bit-reservoir across payloads ---- */

static int wma_decode_superframe(wma_decoder_t * s, const uint8_t * buf, uint32_t buf_size, int16_t * pcm_out,
                                  int * out_nb_frames) {
    *out_nb_frames = 0;
    br_init(&s->gb, buf, buf_size);

    int nb_frames;
    if (s->use_bit_reservoir) {
        br_skip_bits(&s->gb, 4);
        nb_frames = (int) br_get_bits(&s->gb, 4) - (s->reservoir_len == 0 ? 1 : 0);
        if (nb_frames <= 0) {
            /* Degenerate/rare case (not enough frame data yet to complete
             * even one frame) -- rather than replicate FFmpeg's own
             * unusual byte/bit accounting here (untestable without a real
             * sample exercising it), just drop the reservoir and continue
             * with the next payload; costs at most one frame of audio on
             * a malformed/unusual stream. */
            s->reservoir_len = 0;
            return 0;
        }
    } else {
        nb_frames = 1;
    }
    if (nb_frames > MAX_FRAMES_PER_SUPERFRAME) return -1;

    int samples_offset = 0;

    if (s->use_bit_reservoir) {
        int bit_offset = (int) br_get_bits(&s->gb, s->byte_offset_bits + 3);
        if ((uint32_t) bit_offset > br_bits_left(&s->gb)) return -1;

        if (s->reservoir_len > 0) {
            if (s->reservoir_len + (uint32_t) ((bit_offset + 7) >> 3) > s->reservoir_capacity) return -1;
            uint8_t * q = s->reservoir + s->reservoir_len;
            int len = bit_offset;
            while (len > 7) {
                *q++ = (uint8_t) br_get_bits(&s->gb, 8);
                len -= 8;
            }
            if (len > 0) *q++ = (uint8_t) (br_get_bits(&s->gb, len) << (8 - len));
            uint32_t stitched_bytes = (uint32_t) (q - s->reservoir);
            memset(q, 0, 8); /* read-ahead safety padding for br_get_bits */

            bitreader_t stitched;
            br_init(&stitched, s->reservoir, stitched_bytes);
            stitched.size_bits = s->reservoir_len * 8 + (uint32_t) bit_offset;
            if (s->last_bitoffset > 0) br_skip_bits(&stitched, s->last_bitoffset);

            bitreader_t saved = s->gb;
            s->gb = stitched;
            if (wma_decode_frame(s, pcm_out + (size_t) samples_offset * s->channels) < 0) return -1;
            s->gb = saved;
            samples_offset += s->frame_len;
            nb_frames--;
        }

        int pos = bit_offset + 4 + 4 + s->byte_offset_bits + 3;
        if ((uint32_t) pos >= s->reservoir_capacity * 8 || (uint32_t) pos > buf_size * 8) return -1;
        br_init(&s->gb, buf + (pos >> 3), buf_size - (uint32_t) (pos >> 3));
        int align = pos & 7;
        if (align > 0) br_skip_bits(&s->gb, align);

        s->reset_block_lengths = 1;
        for (int i = 0; i < nb_frames; i++) {
            if (wma_decode_frame(s, pcm_out + (size_t) samples_offset * s->channels) < 0) return -1;
            samples_offset += s->frame_len;
        }

        int pos2 = (int) br_get_bits_count(&s->gb) + ((bit_offset + 4 + 4 + s->byte_offset_bits + 3) & ~7);
        s->last_bitoffset = pos2 & 7;
        pos2 >>= 3;
        int len = (int) buf_size - pos2;
        if (len < 0 || (uint32_t) len > s->reservoir_capacity) return -1;
        memcpy(s->reservoir, buf + pos2, (size_t) len);
        s->reservoir_len = (uint32_t) len;
    } else {
        if (wma_decode_frame(s, pcm_out) < 0) return -1;
        samples_offset += s->frame_len;
    }

    *out_nb_frames = samples_offset / s->frame_len;
    return 0;
}

/* ---- init (mirrors ff_wma_init's parameter derivation) ---- */

wma_decoder_t * wma_open_file(const char * path) {
    asf_demux_t * demux = asf_demux_open(path);
    if (!demux) return NULL;

    uint16_t codec_id = asf_demux_get_codec_id(demux);
    if (codec_id != 0x0160 && codec_id != 0x0161) {
        asf_demux_close(demux);
        return NULL; /* WMA Pro (0x162) / Lossless (0x163) out of scope */
    }

    wma_decoder_t * s = calloc(1, sizeof(*s));
    if (!s) {
        asf_demux_close(demux);
        return NULL;
    }
    s->demux = demux;
    s->channels = asf_demux_get_channels(demux);
    s->sample_rate = asf_demux_get_sample_rate(demux);
    s->block_align = asf_demux_get_block_align(demux);
    s->version = (codec_id == 0x0160) ? 1 : 2;

    if (s->channels < 1 || s->channels > MAX_CHANNELS || s->sample_rate == 0 || s->block_align == 0) {
        wma_close(s);
        return NULL;
    }
    int channels = (int) s->channels;

    uint32_t codec_data_size = 0;
    const uint8_t * codec_data = asf_demux_get_codec_data(demux, &codec_data_size);
    uint16_t flags2 = 0;
    if (s->version == 1 && codec_data_size >= 4) flags2 = (uint16_t) (codec_data[2] | (codec_data[3] << 8));
    else if (s->version == 2 && codec_data_size >= 6) flags2 = (uint16_t) (codec_data[4] | (codec_data[5] << 8));

    s->use_exp_vlc = flags2 & 0x0001;
    s->use_bit_reservoir = flags2 & 0x0002;
    s->use_variable_block_len = flags2 & 0x0004;

    for (int i = 0; i < MAX_CHANNELS; i++) s->max_exponent[i] = 1.0f;

    if (s->sample_rate <= 16000) s->frame_len_bits = 9;
    else if (s->sample_rate <= 22050 || (s->sample_rate <= 32000 && s->version == 1)) s->frame_len_bits = 10;
    else s->frame_len_bits = 11;
    s->next_block_len_bits = s->frame_len_bits;
    s->prev_block_len_bits = s->frame_len_bits;
    s->block_len_bits = s->frame_len_bits;
    s->frame_len = 1 << s->frame_len_bits;

    double bit_rate = (double) asf_demux_get_byte_rate(demux) * 8.0;

    if (s->use_variable_block_len) {
        int nb = ((flags2 >> 3) & 3) + 1;
        if (bit_rate / channels >= 32000) nb += 2;
        int nb_max = s->frame_len_bits - BLOCK_MIN_BITS;
        if (nb > nb_max) nb = nb_max;
        s->nb_block_sizes = nb + 1;
    } else {
        s->nb_block_sizes = 1;
    }
    if (s->nb_block_sizes > BLOCK_NB_SIZES) s->nb_block_sizes = BLOCK_NB_SIZES;

    s->use_noise_coding = 1;
    double high_freq = s->sample_rate * 0.5;
    int sample_rate1 = (int) s->sample_rate;
    if (s->version == 2) {
        if (sample_rate1 >= 44100) sample_rate1 = 44100;
        else if (sample_rate1 >= 22050) sample_rate1 = 22050;
        else if (sample_rate1 >= 16000) sample_rate1 = 16000;
        else if (sample_rate1 >= 11025) sample_rate1 = 11025;
        else if (sample_rate1 >= 8000) sample_rate1 = 8000;
    }

    double bps = bit_rate / (channels * (double) s->sample_rate);
    s->byte_offset_bits = ilog2_floor((unsigned) (bps * s->frame_len / 8.0 + 0.5)) + 2;

    double bps1 = bps;
    if (channels == 2) bps1 = bps * 1.6;
    if (sample_rate1 == 44100) {
        if (bps1 >= 0.61) s->use_noise_coding = 0;
        else high_freq *= 0.4;
    } else if (sample_rate1 == 22050) {
        if (bps1 >= 1.16) s->use_noise_coding = 0;
        else if (bps1 >= 0.72) high_freq *= 0.7;
        else high_freq *= 0.6;
    } else if (sample_rate1 == 16000) {
        high_freq *= (bps > 0.5) ? 0.5 : 0.3;
    } else if (sample_rate1 == 11025) {
        high_freq *= 0.7;
    } else if (sample_rate1 == 8000) {
        if (bps <= 0.625) high_freq *= 0.5;
        else if (bps > 0.75) s->use_noise_coding = 0;
        else high_freq *= 0.65;
    } else {
        if (bps >= 0.8) high_freq *= 0.75;
        else if (bps >= 0.6) high_freq *= 0.6;
        else high_freq *= 0.5;
    }

    /* scale factor band sizes per MDCT block size */
    s->coefs_start = (s->version == 1) ? 3 : 0;
    for (int k = 0; k < s->nb_block_sizes; k++) {
        int block_len = s->frame_len >> k;
        int n;
        if (s->version == 1) {
            int lpos = 0, i;
            for (i = 0; i < 25; i++) {
                int a = wma_critical_freqs[i];
                int b = (int) s->sample_rate;
                int pos = ((block_len * 2 * a) + (b >> 1)) / b;
                if (pos > block_len) pos = block_len;
                s->exponent_bands[0][i] = (uint16_t) (pos - lpos);
                if (pos >= block_len) {
                    i++;
                    break;
                }
                lpos = pos;
            }
            s->exponent_sizes[0] = i;
        } else {
            const uint8_t * const * table = NULL;
            int a = s->frame_len_bits - BLOCK_MIN_BITS - k;
            if (a < 3) {
                if (s->sample_rate >= 44100) table = exponent_band_44100;
                else if (s->sample_rate >= 32000) table = exponent_band_32000;
                else if (s->sample_rate >= 22050) table = exponent_band_22050;
            }
            if (table) {
                const uint8_t * row = table[a];
                n = row[0];
                for (int i = 0; i < n; i++) s->exponent_bands[k][i] = row[1 + i];
                s->exponent_sizes[k] = n;
            } else {
                int j = 0, lpos = 0;
                for (int i = 0; i < 25; i++) {
                    int freq_a = wma_critical_freqs[i];
                    int b = (int) s->sample_rate;
                    int pos = ((block_len * 2 * freq_a) + (b << 1)) / (4 * b);
                    pos <<= 2;
                    if (pos > block_len) pos = block_len;
                    if (pos > lpos) s->exponent_bands[k][j++] = (uint16_t) (pos - lpos);
                    if (pos >= block_len) break;
                    lpos = pos;
                }
                s->exponent_sizes[k] = j;
            }
        }

        s->coefs_end[k] = (s->frame_len - (s->frame_len * 9) / 100) >> k;
        s->high_band_start[k] = (int) ((block_len * 2 * high_freq) / s->sample_rate + 0.5);
        n = s->exponent_sizes[k];
        int j = 0, pos = 0;
        for (int i = 0; i < n; i++) {
            int start = pos;
            pos += s->exponent_bands[k][i];
            int end = pos;
            if (start < s->high_band_start[k]) start = s->high_band_start[k];
            if (end > s->coefs_end[k]) end = s->coefs_end[k];
            if (end > start) s->exponent_high_bands[k][j++] = end - start;
        }
        s->exponent_high_sizes[k] = j;
    }

    /* sine windows, one per active block size */
    for (int k = 0; k < s->nb_block_sizes; k++) {
        int n = s->frame_len >> k;
        s->windows[k] = malloc(sizeof(float) * (size_t) n);
        if (!s->windows[k]) {
            wma_close(s);
            return NULL;
        }
        for (int i = 0; i < n; i++) s->windows[k][i] = sinf(((float) i + 0.5f) * (float) (M_PI / (2.0 * n)));
    }

    s->reset_block_lengths = 1;

    if (s->use_noise_coding) {
        s->noise_mult = s->use_exp_vlc ? 0.02f : 0.04f;
        s->noise_table = malloc(sizeof(float) * NOISE_TAB_SIZE);
        if (!s->noise_table) {
            wma_close(s);
            return NULL;
        }
        uint32_t seed = 1;
        double norm = (1.0 / (double) (1LL << 31)) * sqrt(3.0) * s->noise_mult;
        for (int i = 0; i < NOISE_TAB_SIZE; i++) {
            seed = seed * 314159u + 1u;
            s->noise_table[i] = (float) ((double) (int32_t) seed * norm);
        }
    }

    int coef_vlc_table = 2;
    if (s->sample_rate >= 32000) {
        if (bps1 < 0.72) coef_vlc_table = 0;
        else if (bps1 < 1.16) coef_vlc_table = 1;
    }
    if (!coef_vlc_build(&s->coef_vlc[0], &wma_coef_vlc_tables[coef_vlc_table * 2]) ||
        !coef_vlc_build(&s->coef_vlc[1], &wma_coef_vlc_tables[coef_vlc_table * 2 + 1])) {
        wma_close(s);
        return NULL;
    }

    if (s->use_noise_coding) {
        if (!huff_build_from_lengths(&s->hgain_vlc, &wma_hgain_hufftab[0][1], &wma_hgain_hufftab[0][0], -18, 37)) {
            wma_close(s);
            return NULL;
        }
    }

    wma_pow_tab_init();
    if (s->use_exp_vlc) {
        if (!huff_build(&s->exp_vlc, wma_exp_huffcodes, wma_exp_huffbits, 121)) {
            wma_close(s);
            return NULL;
        }
    } else {
        wma_lsp_pow_tables_init(s);
    }

    for (int ch = 0; ch < MAX_CHANNELS; ch++) {
        s->frame_out[ch] = calloc((size_t) 2 * s->frame_len, sizeof(float));
        if (!s->frame_out[ch]) {
            wma_close(s);
            return NULL;
        }
    }

    s->reservoir_capacity = (uint32_t) s->block_align * 2u + 256u;
    s->reservoir = calloc(1, s->reservoir_capacity + 16);
    s->frame_buf_capacity = (uint32_t) s->block_align + 16;
    s->frame_buf = calloc(1, s->frame_buf_capacity);
    s->pcm_buf = calloc((size_t) MAX_FRAMES_PER_SUPERFRAME * (size_t) s->frame_len * s->channels, sizeof(int16_t));
    if (!s->reservoir || !s->frame_buf || !s->pcm_buf) {
        wma_close(s);
        return NULL;
    }

    uint64_t duration_100ns = asf_demux_get_duration_100ns(demux);
    s->total_pcm_frames = duration_100ns * s->sample_rate / 10000000ull;
    s->skip_frames_remaining = (uint64_t) s->frame_len * 2;

    return s;
}

unsigned int wma_get_channels(const wma_decoder_t * dec) { return dec->channels; }
unsigned int wma_get_sample_rate(const wma_decoder_t * dec) { return dec->sample_rate; }
uint64_t wma_get_total_pcm_frame_count(const wma_decoder_t * dec) { return dec->total_pcm_frames; }

/* At EOF, frame_out still holds one un-emitted frame's worth of tail audio
 * (the final block's right-half overlap, never combined with a "next"
 * block since there isn't one) -- FFmpeg's decoder flushes this explicitly
 * when fed an empty final packet; do the same directly from frame_out. */
static void wma_flush_tail(wma_decoder_t * dec) {
    for (unsigned int ch = 0; ch < dec->channels; ch++) {
        for (int i = 0; i < dec->frame_len; i++) {
            long iv = lrintf(dec->frame_out[ch][i]);
            if (iv > 32767) iv = 32767;
            else if (iv < -32768) iv = -32768;
            dec->pcm_buf[i * (int) dec->channels + (int) ch] = (int16_t) iv;
        }
    }
    dec->pcm_buf_count = (uint32_t) dec->frame_len;
    dec->pcm_buf_pos = 0;
}

static bool wma_fill_more_pcm(wma_decoder_t * dec) {
    for (;;) {
        uint32_t got = 0;
        if (!asf_demux_read_frame(dec->demux, dec->frame_buf, dec->frame_buf_capacity - 16, &got)) {
            dec->eof = true;
            if (!dec->flush_done) {
                dec->flush_done = true;
                wma_flush_tail(dec);
                return true;
            }
            return false;
        }
        memset(dec->frame_buf + got, 0, 8);
        uint32_t use_size = got < dec->block_align ? got : dec->block_align;
        if (use_size == 0) continue;

        int nb_frames = 0;
        int ret = wma_decode_superframe(dec, dec->frame_buf, use_size, dec->pcm_buf, &nb_frames);
        if (ret < 0) continue; /* skip corrupt superframe */
        if (nb_frames > 0) {
            dec->pcm_buf_count = (uint32_t) nb_frames * (uint32_t) dec->frame_len;
            dec->pcm_buf_pos = 0;
            return true;
        }
    }
}

uint64_t wma_read_pcm_frames_s16(wma_decoder_t * dec, uint64_t frames_to_read, int16_t * buffer_out) {
    uint64_t produced = 0;
    while (produced < frames_to_read) {
        if (dec->pcm_buf_pos >= dec->pcm_buf_count) {
            if (!wma_fill_more_pcm(dec)) break;
        }
        if (dec->skip_frames_remaining > 0) {
            uint32_t avail_skip = dec->pcm_buf_count - dec->pcm_buf_pos;
            uint64_t skip_now = dec->skip_frames_remaining < avail_skip ? dec->skip_frames_remaining : avail_skip;
            dec->pcm_buf_pos += (uint32_t) skip_now;
            dec->skip_frames_remaining -= skip_now;
            continue;
        }
        uint32_t avail = dec->pcm_buf_count - dec->pcm_buf_pos;
        uint64_t want = frames_to_read - produced;
        uint32_t take = (uint32_t) (want < avail ? want : avail);
        memcpy(buffer_out + produced * dec->channels, dec->pcm_buf + (size_t) dec->pcm_buf_pos * dec->channels,
               (size_t) take * dec->channels * sizeof(int16_t));
        dec->pcm_buf_pos += take;
        produced += take;
    }
    return produced;
}

bool wma_seek_to_pcm_frame(wma_decoder_t * dec, uint64_t frame_index) {
    uint64_t total_packets = asf_demux_get_packet_count(dec->demux);
    if (total_packets == 0 || dec->total_pcm_frames == 0) return false;
    uint64_t packet_index = frame_index * total_packets / dec->total_pcm_frames;
    asf_demux_seek_to_packet(dec->demux, packet_index);

    dec->reservoir_len = 0;
    dec->last_bitoffset = 0;
    dec->reset_block_lengths = 1;
    dec->block_pos = 0;
    for (int ch = 0; ch < MAX_CHANNELS; ch++) {
        memset(dec->frame_out[ch], 0, (size_t) 2 * dec->frame_len * sizeof(float));
        dec->exponents_initialized[ch] = 0;
    }
    dec->pcm_buf_count = 0;
    dec->pcm_buf_pos = 0;
    dec->skip_frames_remaining = (uint64_t) dec->frame_len * 2;
    dec->eof = false;
    dec->flush_done = false;
    return true;
}

void wma_close(wma_decoder_t * dec) {
    if (!dec) return;
    if (dec->demux) asf_demux_close(dec->demux);
    huff_free(&dec->exp_vlc);
    huff_free(&dec->hgain_vlc);
    for (int i = 0; i < 2; i++) {
        huff_free(&dec->coef_vlc[i].tree);
        free(dec->coef_vlc[i].run_table);
        free(dec->coef_vlc[i].level_table);
    }
    for (int i = 0; i < BLOCK_NB_SIZES; i++) free(dec->windows[i]);
    for (int i = 0; i < MAX_CHANNELS; i++) free(dec->frame_out[i]);
    free(dec->noise_table);
    free(dec->reservoir);
    free(dec->frame_buf);
    free(dec->pcm_buf);
    free(dec);
}
