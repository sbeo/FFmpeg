/*
 * ER BSAC decoder
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 * Error resilient bit sliced arithmetic coding (ER BSAC) decoder,
 * ISO/IEC 14496-3:2009 subclauses 4.4.2.6, 4.5.2.6 and 4.6.4.
 *
 * BSAC only replaces the noiseless coding of AAC: the side information and
 * the quantized spectrum are arithmetic coded in bit planes, spread over fine
 * grain scalability layers. This file decodes a bsac_raw_data_block() into
 * the dequantized spectrum of a ChannelElement; everything after that is done
 * by the regular AAC decoder.
 */

#include "libavutil/attributes.h"
#include "libavutil/common.h"
#include "libavutil/intfloat.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"

#include "libavcodec/aactab.h"
#include "libavcodec/cbrt_data.h"
#include "libavcodec/get_bits.h"

#include "aacdec.h"
#include "aacdec_bsac.h"
#include "aacdec_bsac_tab.h"

#include <stdio.h>
#include <stdlib.h>
static int bsac_hyp(int n)
{
    static int v[16] = { -1000 };
    if (v[0] == -1000) {
        const char *e = getenv("BSAC_HYP");
        for (int i = 0; i < 16; i++)
            v[i] = 0;
        v[0] = 0;
        if (e)
            for (int i = 0; i < 16 && *e; i++) {
                v[i] = strtol(e, (char **)&e, 10);
                if (*e == ',') e++;
            }
    }
    return v[n];
}

static int bsac_x(int n)
{
    static int v[16] = { -1000 };
    if (v[0] == -1000) {
        const char *e = getenv("BSAC_X");
        for (int i = 0; i < 16; i++)
            v[i] = 0;
        if (e)
            for (int i = 0; i < 16 && *e; i++) {
                v[i] = strtol(e, (char **)&e, 10);
                if (*e == ',') e++;
            }
    }
    return v[n];
}

#define BSAC_MAX_LAYERS   128
#define BSAC_MAX_CBANDS    32
#define BSAC_MAX_SFB       64
#define BSAC_P0_SIGN   0x2000
#define BSAC_ONE      16384

/* Extension types, Table 4.99 */
enum {
    EXT_BSAC_SBR_DATA         = 0x0,
    EXT_BSAC_SBR_DATA_CRC     = 0x1,
    EXT_BSAC_SAC_DATA         = 0x2,
    EXT_BSAC_CHANNEL_SBR_CRC  = 0xd,
    EXT_BSAC_CHANNEL_SBR      = 0xe,
    EXT_BSAC_CHANNEL          = 0xf,
};

typedef struct BSACArith {
    const uint8_t *buf;
    unsigned pos;       ///< read position in bits
    unsigned end;       ///< end of the segment in bits; zeros are read after it
    uint32_t value;
    uint32_t range;
    int est_cw_len;
} BSACArith;

typedef struct BSACContext {
    void *logctx;
    BSACArith ar;

    int nch;
    int fs;
    int frame_samples;              ///< 1024 or 960
    int win_len;                    ///< 128 or 120

    /* bsac_base_element(), bsac_header() */
    int frame_length;
    int header_length;
    int sba_mode;
    int top_layer;
    int base_snf_thr;
    int max_scalefactor[2];
    int base_band;
    int cband_si_type[2];
    int base_scf_model[2];
    int enh_scf_model[2];
    int max_sfb_si_len[2];

    /* general_header() */
    int window_sequence;
    int max_sfb;
    int num_window_groups;
    int window_group_length[8];
    int pns_data_present;
    int pns_start_sfb;
    int ms_mask_present;

    int group_offset[8];            ///< start of each group in the coefficient arrays
    int group_size[8];
    int swb_offset[8][BSAC_MAX_SFB + 1];
    uint8_t sfb_of_line[1024];      ///< scalefactor band of each line of a window

    /* fine grain layers */
    int slayer_size;
    int num_layers;                 ///< top_layer + slayer_size
    int cur_layer;
    uint8_t layer_group[BSAC_MAX_LAYERS + 8];
    int layer_start_index[BSAC_MAX_LAYERS];
    int layer_end_index[BSAC_MAX_LAYERS];
    int layer_start_cband[BSAC_MAX_LAYERS];
    int layer_end_cband[BSAC_MAX_LAYERS];
    int layer_start_sfb[BSAC_MAX_LAYERS];
    int layer_end_sfb[BSAC_MAX_LAYERS];
    int layer_si_maxlen[BSAC_MAX_LAYERS];
    int layer_bit_offset[BSAC_MAX_LAYERS + 1];
    int available_len[BSAC_MAX_LAYERS];
    uint8_t terminal_layer[BSAC_MAX_LAYERS];

    /* side information */
    uint8_t cband_si[2][8][BSAC_MAX_CBANDS];
    int scf[2][8][BSAC_MAX_SFB];
    uint8_t ms_used[8][BSAC_MAX_SFB];
    uint8_t stereo_info[8][BSAC_MAX_SFB];
    uint8_t noise_flag[2][8][BSAC_MAX_SFB];
    uint8_t noise_mode[8][BSAC_MAX_SFB];
    uint8_t stereo_side_info_coded[8][BSAC_MAX_SFB];
    int noise_nrg[2][8][BSAC_MAX_SFB];
    int is_position[8][BSAC_MAX_SFB];
    int noise_pcm_flag[2];
    int max_noise_energy[2];

    /* bit-sliced spectral data */
    int32_t sample[2][1024];
    int8_t cur_snf[2][1024];
    uint8_t sign_is_coded[2][1024];

    int l0_min_snf[2];
    int sign_bit;
    int warned_pns;
    int warned_is;
} BSACContext;

/* ---------------------------------------------------------------------- */
/* Arithmetic decoder, subclause 4.5.2.6.2.7                               */

static void ar_init(BSACArith *ar, const uint8_t *buf, unsigned start, unsigned end)
{
    ar->buf        = buf;
    ar->pos        = start;
    ar->end        = end;
    ar->value      = 0;
    ar->range      = 1;
    ar->est_cw_len = 30;
}

/* Read n <= 30 bits, the segment being followed by zeros. */
static uint32_t ar_read_bits(BSACArith *ar, int n)
{
    uint32_t v = 0;

    if (ar->pos < ar->end) {
        unsigned left = ar->end - ar->pos;
        uint64_t w = AV_RB64(ar->buf + (ar->pos >> 3)) << (ar->pos & 7);
        v = w >> (64 - n);
        if (left < n)
            v &= ~((1U << (n - left)) - 1);
    }
    ar->pos += n;
    return v;
}

static av_always_inline void ar_renorm_in(BSACArith *ar)
{
    if (ar->est_cw_len) {
        ar->range <<= ar->est_cw_len;
        ar->value   = (ar->value << ar->est_cw_len) |
                      ar_read_bits(ar, ar->est_cw_len);
    }
    ar->range >>= 14;
}

static av_always_inline void ar_renorm_out(BSACArith *ar)
{
    int len = 0;
    while (len < 16 && ar->range < (0x20000000U >> len))
        len++;
    ar->est_cw_len = len;
}

/**
 * Decode a symbol with a cumulative frequency model (decode_symbol()).
 * The model is decreasing and ends with 0.
 */
static int ar_decode_symbol(BSACArith *ar, const uint16_t *cum_freq)
{
    uint32_t cum;
    int sym;

    ar_renorm_in(ar);
    cum = ar->value / ar->range;
    for (sym = 0; cum_freq[sym] > cum; sym++)
        ;
    ar->value -= ar->range * cum_freq[sym];
    ar->range *= (sym ? cum_freq[sym - 1] : BSAC_ONE) - cum_freq[sym];
    ar_renorm_out(ar);
    return sym;
}

/** Decode a binary symbol, p0 being the probability of 0 (decode_symbol2()). */
static int ar_decode_bit(BSACArith *ar, int p0)
{
    int bit;

    ar_renorm_in(ar);
    if (p0 * ar->range <= ar->value) {
        bit = 1;
        ar->value -= ar->range * p0;
        ar->range *= BSAC_ONE - p0;
    } else {
        bit = 0;
        ar->range *= p0;
    }
    ar_renorm_out(ar);
    return bit;
}

static int ar_cost(BSACContext *s, int before)
{
    int h = bsac_hyp(9);
    if (!h)
        return s->ar.est_cw_len;
    if (h == 2 && before == 30 && s->ar.pos - 30 == s->ar.end - s->ar.end) {}
    return before;
}

static int decode_symbol(BSACContext *s, const uint16_t *model)
{
    int before = s->ar.est_cw_len;
    int sym = ar_decode_symbol(&s->ar, model);
    if (bsac_hyp(11) != 1)
        s->available_len[s->cur_layer] -= ar_cost(s, before);
    return sym;
}

static int decode_bit(BSACContext *s, int p0)
{
    int *avail = &s->available_len[s->cur_layer];
    int bit;

    {
        int a = *avail + bsac_hyp(15);
        if (bsac_hyp(4) && a < 14 && a > 0 && (!s->sign_bit || !bsac_hyp(14)))
            p0 = av_clip(p0, ff_bsac_min_p0[a], ff_bsac_max_p0[a]);
    }
    {
        int before = s->ar.est_cw_len;
        bit = ar_decode_bit(&s->ar, p0);
        *avail -= ar_cost(s, before);
    }
    return bit;
}

static int layer_data_available(const BSACContext *s)
{
    return s->available_len[s->cur_layer] > bsac_hyp(7);
}

/* ---------------------------------------------------------------------- */
/* Headers                                                                 */

static int decode_general_header(AACDecContext *ac, BSACContext *s,
                                 ChannelElement *che, GetBitContext *gb)
{
    const MPEG4AudioConfig *m4ac = &ac->oc[1].m4ac;
    IndividualChannelStream *ics = &che->ch[0].ics;
    int ret;

    if (get_bits1(gb))
        av_log(s->logctx, AV_LOG_DEBUG, "BSAC reserved bit set\n");

    ics->window_sequence[1] = ics->window_sequence[0];
    ics->window_sequence[0] = get_bits(gb, 2);
    ics->use_kb_window[1]   = ics->use_kb_window[0];
    ics->use_kb_window[0]   = get_bits1(gb);
    ics->prev_num_window_groups = FFMAX(ics->num_window_groups, 1);
    ics->num_window_groups  = 1;
    ics->group_len[0]       = 1;
    ics->predictor_present  = 0;
    ics->ltp.present        = 0;

    if (ics->window_sequence[0] == EIGHT_SHORT_SEQUENCE) {
        ics->max_sfb = get_bits(gb, 4);
        for (int i = 0; i < 7; i++) {
            if (get_bits1(gb)) {
                ics->group_len[ics->num_window_groups - 1]++;
            } else {
                ics->num_window_groups++;
                ics->group_len[ics->num_window_groups - 1] = 1;
            }
        }
        ics->num_windows = 8;
        if (m4ac->frame_length_short) {
            ics->swb_offset = ff_swb_offset_120[m4ac->sampling_index];
            ics->num_swb    = ff_aac_num_swb_120[m4ac->sampling_index];
        } else {
            ics->swb_offset = ff_swb_offset_128[m4ac->sampling_index];
            ics->num_swb    = ff_aac_num_swb_128[m4ac->sampling_index];
        }
        ics->tns_max_bands = ff_tns_max_bands_128[m4ac->sampling_index];
    } else {
        ics->max_sfb     = get_bits(gb, 6);
        ics->num_windows = 1;
        if (m4ac->frame_length_short) {
            ics->swb_offset = ff_swb_offset_960[m4ac->sampling_index];
            ics->num_swb    = ff_aac_num_swb_960[m4ac->sampling_index];
        } else {
            ics->swb_offset = ff_swb_offset_1024[m4ac->sampling_index];
            ics->num_swb    = ff_aac_num_swb_1024[m4ac->sampling_index];
        }
        ics->tns_max_bands = ff_tns_max_bands_1024[m4ac->sampling_index];
    }
    if (ics->max_sfb > ics->num_swb) {
        av_log(s->logctx, AV_LOG_ERROR, "BSAC max_sfb %d exceeds %d\n",
               ics->max_sfb, ics->num_swb);
        ics->max_sfb = 0;
        return AVERROR_INVALIDDATA;
    }

    s->pns_data_present = get_bits1(gb);
    s->pns_start_sfb    = s->pns_data_present ? get_bits(gb, 6) : 0;
    s->ms_mask_present  = s->nch == 2 ? get_bits(gb, 2) : 0;

    if (s->nch == 2) {
        int kb = che->ch[1].ics.use_kb_window[0];
        che->ch[1].ics = *ics;
        che->ch[1].ics.use_kb_window[1] = kb;
    }

    for (int ch = 0; ch < s->nch; ch++) {
        SingleChannelElement *sce = &che->ch[ch];
        sce->tns.present = get_bits1(gb);
        if (sce->tns.present) {
            ret = ff_aac_decode_tns(ac, &sce->tns, gb, &sce->ics);
            if (ret < 0)
                return ret;
        }
        if (get_bits1(gb)) {
            avpriv_report_missing_feature(s->logctx, "BSAC LTP");
            return AVERROR_PATCHWELCOME;
        }
    }

    s->window_sequence   = ics->window_sequence[0];
    s->max_sfb           = ics->max_sfb;
    s->num_window_groups = ics->num_window_groups;
    for (int g = 0; g < ics->num_window_groups; g++)
        s->window_group_length[g] = ics->group_len[g];

    return 0;
}

static int decode_headers(AACDecContext *ac, BSACContext *s,
                          ChannelElement *che, GetBitContext *gb)
{
    int ret;

    s->frame_length  = get_bits(gb, 11);
    s->header_length = get_bits(gb, 4);
    s->sba_mode      = get_bits1(gb);
    s->top_layer     = get_bits(gb, 6);
    s->base_snf_thr  = get_bits(gb, 2);
    for (int ch = 0; ch < s->nch; ch++)
        s->max_scalefactor[ch] = get_bits(gb, 8);
    s->base_band = get_bits(gb, 5);
    for (int ch = 0; ch < s->nch; ch++) {
        s->cband_si_type[ch]  = get_bits(gb, 5);
        s->base_scf_model[ch] = get_bits(gb, 3);
        s->enh_scf_model[ch]  = get_bits(gb, 3);
        s->max_sfb_si_len[ch] = get_bits(gb, 4);
    }

    ret = decode_general_header(ac, s, che, gb);
    if (ret < 0)
        return ret;

    align_get_bits(gb);
    return 0;
}

/* ---------------------------------------------------------------------- */
/* Scalefactor bands, groups and fine grain layers                         */

static void init_bands(BSACContext *s, const IndividualChannelStream *ics)
{
    int off = 0;

    for (int g = 0; g < s->num_window_groups; g++) {
        int len = s->window_group_length[g];
        s->group_offset[g] = off;
        if (s->window_sequence == EIGHT_SHORT_SEQUENCE) {
            s->group_size[g] = len * s->win_len;
            for (int sfb = 0; sfb <= s->max_sfb; sfb++)
                s->swb_offset[g][sfb] = ics->swb_offset[sfb] * len;
        } else {
            s->group_size[g] = s->frame_samples;
            for (int sfb = 0; sfb <= s->max_sfb; sfb++)
                s->swb_offset[g][sfb] = ics->swb_offset[sfb];
        }
        off += s->group_size[g];
    }

    for (int sfb = 0; sfb < s->max_sfb; sfb++)
        for (int k = ics->swb_offset[sfb]; k < ics->swb_offset[sfb + 1]; k++)
            s->sfb_of_line[k] = sfb;
}

static int layer_inc(int fs, int end_index)
{
    switch (bsac_hyp(12)) {
    case 1: return end_index % 32 ? 8 : 12;
    case 2: return 16;
    case 3: return 8;
    case 4: return 12;
    case 5: return 32;
    }
    if (fs == 44100 || fs == 48000)
        return end_index % 32 ? 12 : 8;
    if (fs == 22050 || fs == 24000 || fs == 32000)
        return 16;
    if (fs == 11025 || fs == 12000 || fs == 16000)
        return 32;
    return 64;
}

/** Subclause 4.5.2.6.2.5 */
static int init_layers(BSACContext *s, int header_bits)
{
    const int is_short = s->window_sequence == EIGHT_SHORT_SEQUENCE;
    const int fs = s->fs;
    int end_index[8], end_cband[8], last_index[8], end_sfb[8];
    int ss = 0, nl, layer;
    int overflow;

    for (int g = 0; g < s->num_window_groups; g++) {
        if (is_short) {
            int e = s->base_band * 4 * s->window_group_length[g];
            if (fs == 44100 || fs == 48000) {
                if (e % 32 >= 16)
                    e = e / 32 * 32 + 20;
                else if (e % 32 >= 4)
                    e = e / 32 * 32 + 8;
            } else if (fs == 22050 || fs == 24000 || fs == 32000) {
                e = e / 16 * 16;
            } else if (fs == 11025 || fs == 12000 || fs == 16000) {
                e = e / 32 * 32;
            } else {
                e = e / 64 * 64;
            }
            end_cband[g] = (e + 31) / 32;
        } else {
            end_cband[g] = s->base_band;
        }
        ss += end_cband[g];
    }

    nl = ss + s->top_layer;
    if (nl > BSAC_MAX_LAYERS || !nl) {
        av_log(s->logctx, AV_LOG_ERROR, "Invalid number of BSAC layers %d\n", nl);
        return AVERROR_INVALIDDATA;
    }
    s->slayer_size = ss;
    s->num_layers  = nl;

    /* layer_group[] */
    memset(s->layer_group, 0, sizeof(s->layer_group));
    layer = 0;
    for (int g = 0; g < s->num_window_groups; g++)
        for (int cband = 1; cband <= end_cband[g]; cband++)
            s->layer_group[layer++] = g;
    layer = ss;
    for (int g = 0; g < s->num_window_groups; g++)
        for (int w = 0; w < s->window_group_length[g]; w++)
            s->layer_group[layer++] = g;
    for (layer = ss + 8; layer < nl; layer++)
        s->layer_group[layer] = s->layer_group[layer - 8];

    /* spectral range and coding bands of each layer */
    layer = 0;
    for (int g = 0; g < s->num_window_groups; g++) {
        for (int cband = 0; cband < end_cband[g]; cband++) {
            s->layer_start_cband[layer] = cband;
            s->layer_end_cband[layer]   = cband + 1;
            s->layer_start_index[layer] = cband * 32;
            s->layer_end_index[layer]   = (cband + 1) * 32;
            layer++;
        }
        end_index[g]  = end_cband[g] * 32;
        last_index[g] = s->swb_offset[g][s->max_sfb];
    }
    for (layer = ss; layer < nl; layer++) {
        int g = s->layer_group[layer];
        s->layer_start_index[layer] = end_index[g];
        end_index[g] += layer_inc(fs, end_index[g]);
        if (end_index[g] > last_index[g])
            end_index[g] = last_index[g];
        s->layer_end_index[layer]   = end_index[g];
        s->layer_start_cband[layer] = end_cband[g];
        end_cband[g] = s->layer_end_cband[layer] = (end_index[g] + 31) / 32;
    }

    /* scalefactor bands of each layer */
    for (int g = 0; g < s->num_window_groups; g++)
        end_sfb[g] = 0;
    for (layer = 0; layer < nl; layer++) {
        int g = s->layer_group[layer];
        s->layer_start_sfb[layer] = end_sfb[g];
        s->layer_end_sfb[layer]   = s->max_sfb;
        for (int sfb = 0; sfb < s->max_sfb; sfb++) {
            int e = bsac_x(0) == 2 ? s->layer_end_cband[layer] * 32 : s->layer_end_index[layer];
            if (bsac_x(0) == 4 ? e < s->swb_offset[g][sfb] : e <= s->swb_offset[g][sfb]) {
                s->layer_end_sfb[layer] = FFMIN(sfb + (bsac_x(0) & 1), s->max_sfb);
                break;
            }
        }
        end_sfb[g] = s->layer_end_sfb[layer];
    }

    /* maximum side information length of each layer */
    for (layer = 0; layer < nl; layer++) {
        int len = 0;
        for (int cband = s->layer_start_cband[layer];
             cband < s->layer_end_cband[layer]; cband++)
            for (int ch = 0; ch < s->nch; ch++)
                len += cband ? ff_bsac_cband_si_type[s->cband_si_type[ch]].max_cband_si_len
                             : 11;
        for (int sfb = s->layer_start_sfb[layer]; sfb < s->layer_end_sfb[layer]; sfb++)
            for (int ch = 0; ch < s->nch; ch++)
                len += s->max_sfb_si_len[ch] + 5;
        s->layer_si_maxlen[layer] = len;
    }

    /* bitstream offset and available length of each layer */
    for (layer = ss; layer <= nl; layer++) {
        /* The spec rounds nch * bitrate down to whole bytes; the encoders
         * round the per channel size, which differs by 8 bits at 48 kHz
         * and in the upper enhancement layers at 44.1 kHz. */
        int64_t bitrate = (layer - ss) * 1000 + 16000;
        int off = s->nch * (bitrate * s->frame_samples / fs / 8 * 8);
        if (bsac_x(6) == 2)
            off = s->nch * bitrate * s->frame_samples / fs / 8 * 8;
        s->layer_bit_offset[layer] = FFMIN(off, s->frame_length * 8);
    }
    for (layer = nl - 1; layer >= ss; layer--) {
        int off = s->layer_bit_offset[layer + 1] - s->layer_si_maxlen[layer];
        if (off < s->layer_bit_offset[layer])
            s->layer_bit_offset[layer] = off;
    }
    for (layer = ss - 1; layer >= 0; layer--)
        s->layer_bit_offset[layer] = s->layer_bit_offset[layer + 1] -
                                     s->layer_si_maxlen[layer];

    overflow = header_bits - s->layer_bit_offset[0];
    s->layer_bit_offset[0] = header_bits;
    if (overflow > 0) {
        for (layer = nl - 1; layer >= ss; layer--) {
            int size = s->layer_bit_offset[layer + 1] - s->layer_bit_offset[layer] -
                       s->layer_si_maxlen[layer];
            size = FFMAX(size, 0);
            if (size >= overflow) {
                size     = overflow;
                overflow = 0;
            } else {
                overflow -= size;
            }
            for (int m = 1; m <= layer; m++)
                s->layer_bit_offset[m] += size;
            if (overflow <= 0)
                break;
        }
    } else if (ss > 0 && bsac_x(3)) {
        int underflow = -overflow, v = bsac_x(3);
        int w[BSAC_MAX_LAYERS] = { 0 }, tw = 0, acc = 0;
        for (int m = 0; m < ss; m++) {
            w[m] = v == 1 ? 1 : v == 2 ? s->layer_si_maxlen[m] : v == 3 ? (m == 0) :
                   v == 4 ? (m == ss - 1) : v == 5 ? ss - m : m + 1;
            tw += w[m];
        }
        for (int m = 1; m < ss; m++) {
            int share;
            acc += w[m - 1];
            share = tw ? (int64_t)underflow * acc / tw : 0;
            if (v == 1 && bsac_x(4) == 1)
                share = underflow / ss * m + FFMAX(0, m - (ss - underflow % ss));
            s->layer_bit_offset[m] = s->layer_bit_offset[0];
            for (int k = 0; k < m; k++)
                s->layer_bit_offset[m] += s->layer_si_maxlen[k];
            s->layer_bit_offset[m] += share;
        }
    } else if (ss > 0) {
        int underflow = -overflow + bsac_x(5);
        for (int m = 1; m < ss; m++) {
            s->layer_bit_offset[m] = s->layer_bit_offset[m - 1] +
                                     s->layer_si_maxlen[m - 1] +
                                     underflow / ss +
                                     (bsac_hyp(2) == 0 ? m <= underflow % ss :
                                      bsac_hyp(2) == 1 ? m <  underflow % ss :
                                      bsac_hyp(2) == 2 ? 1 : 0);
        }
    }

    for (layer = 0; layer < nl; layer++)
        s->available_len[layer] = s->layer_bit_offset[layer + 1] -
                                  s->layer_bit_offset[layer];
    av_log(s->logctx, AV_LOG_DEBUG, "LAYINFO ov %d si0 %d si1 %d av0 %d av1 %d cst %d %d msl %d %d\n", overflow,
           s->layer_si_maxlen[0], s->layer_si_maxlen[1], s->available_len[0], s->available_len[1],
           s->cband_si_type[0], s->cband_si_type[1], s->max_sfb_si_len[0], s->max_sfb_si_len[1]);
    s->available_len[0] += bsac_hyp(5);
    {
        const char *e = getenv("BSAC_LD");
        while (e && *e) {
            int l = strtol(e, (char **)&e, 10), d;
            if (*e == ':') e++;
            d = strtol(e, (char **)&e, 10);
            if (*e == ',') e++;
            if (l >= 0 && l < nl) s->available_len[l] += d;
        }
    }

    /* segments of the segmented binary arithmetic coding, 4.6.4.6.3 */
    for (layer = 0; layer < nl - 1; layer++)
        s->terminal_layer[layer] = s->layer_start_cband[layer] !=
                                   s->layer_start_cband[layer + 1];
    s->terminal_layer[nl - 1] = 1;

    return 0;
}

/* ---------------------------------------------------------------------- */
/* Side information                                                        */

static int decode_layer_cband_si(BSACContext *s, int layer)
{
    const int g = s->layer_group[layer];

    for (int ch = 0; ch < s->nch; ch++) {
        const BSACCbandSiType *type = &ff_bsac_cband_si_type[s->cband_si_type[ch]];
        for (int cband = s->layer_start_cband[layer];
             cband < s->layer_end_cband[layer]; cband++) {
            int first = !cband;
            const uint16_t *model = !first ? ff_bsac_cband_si_models[type->model]
                                           : ff_bsac_cband_si_cband0_model;
            int largest = !first ? type->largest_cband_si : type->largest_cband0_si;
            int before = s->available_len[s->cur_layer];
            int si = decode_symbol(s, model);
            int start, end, msb;
            {
                int cost = before - s->available_len[s->cur_layer];
                int maxl = cband ? type->max_cband_si_len : 11;
                av_log(s->logctx, AV_LOG_DEBUG, "SICOST cb L%d c%d cost %d max %d %s\n", layer, cband, cost, maxl, cost > maxl ? "VIOL" : "ok");
            }

            if (si > largest) {
                av_log(s->logctx, AV_LOG_ERROR,
                       "BSAC cband_si %d exceeds %d (layer %d, cband %d)\n",
                       si, largest, layer, cband);
                return AVERROR_INVALIDDATA;
            }
            s->cband_si[ch][g][cband] = si;

            /* Set the current significance of the whole coding band. */
            msb   = si ? ff_bsac_prob_tables[si].msb : 0;
            start = s->group_offset[g] + cband * 32;
            end   = s->group_offset[g] + FFMIN((cband + 1) * 32, s->group_size[g]);
            for (int i = start; i < end; i++)
                s->cur_snf[ch][i] = msb;
        }
    }
    return 0;
}

static int decode_scf_diff(BSACContext *s, int ch, int layer)
{
    int model = layer < s->slayer_size ? s->base_scf_model[ch]
                                       : s->enh_scf_model[ch];
    if (bsac_hyp(1) == 1)
        model = layer < s->slayer_size ? s->enh_scf_model[ch] : s->base_scf_model[ch];
    if (!model)
        return 0;
    return decode_symbol(s, ff_bsac_scf_models[model]);
}

/** Whether all the coding bands covering a scalefactor band are zero. */
static int band_is_zero(const BSACContext *s, int ch, int g, int sfb)
{
    int first = s->swb_offset[g][sfb] >> 5;
    int last  = (s->swb_offset[g][sfb + 1] - 1) >> 5;

    for (int cband = first; cband <= last; cband++)
        if (s->cband_si[ch][g][cband])
            return 0;
    return 1;
}

static int decode_layer_sfb_si(BSACContext *s, int layer)
{
    const int g = s->layer_group[layer];

    for (int ch = 0; ch < s->nch; ch++) {
        for (int sfb = s->layer_start_sfb[layer]; sfb < s->layer_end_sfb[layer]; sfb++) {
            const int pns = s->pns_data_present && sfb >= s->pns_start_sfb;
            int before = s->available_len[s->cur_layer];

            if (s->nch == 1) {
                if (pns)
                    s->noise_flag[0][g][sfb] = decode_symbol(s, ff_bsac_noise_flag_model);
            } else if (!s->stereo_side_info_coded[g][sfb]) {
                if (s->ms_mask_present != 2) {
                    if (s->ms_mask_present == 1)
                        s->ms_used[g][sfb] = decode_symbol(s, ff_bsac_ms_used_model);
                    else if (s->ms_mask_present == 3)
                        s->stereo_info[g][sfb] = decode_symbol(s, ff_bsac_stereo_info_model);
                    if (pns) {
                        s->noise_flag[0][g][sfb] = decode_symbol(s, ff_bsac_noise_flag_model);
                        s->noise_flag[1][g][sfb] = decode_symbol(s, ff_bsac_noise_flag_model);
                        if (s->ms_mask_present == 3 && s->stereo_info[g][sfb] == 3 &&
                            s->noise_flag[0][g][sfb] && s->noise_flag[1][g][sfb])
                            s->noise_mode[g][sfb] = decode_symbol(s, ff_bsac_noise_mode_model);
                    }
                }
                s->stereo_side_info_coded[g][sfb] = 1;
            }

            if (s->noise_flag[ch][g][sfb]) {
                /* The arithmetic models of the noise energies are not
                 * specified; use the scalefactor model. */
                if (!s->warned_pns) {
                    avpriv_request_sample(s->logctx, "BSAC PNS");
                    s->warned_pns = 1;
                }
                if (s->noise_pcm_flag[ch]) {
                    int v = 0;
                    for (int i = 0; i < 9; i++)
                        v = (v << 1) | decode_bit(s, BSAC_P0_SIGN);
                    s->max_noise_energy[ch] = v;
                    s->noise_pcm_flag[ch] = 0;
                }
                s->noise_nrg[ch][g][sfb] = s->max_noise_energy[ch] -
                                           decode_scf_diff(s, ch, layer);
            } else if (s->stereo_info[g][sfb] >= 2 && ch == 1) {
                int idx;
                if (!s->warned_is) {
                    avpriv_request_sample(s->logctx, "BSAC intensity stereo");
                    s->warned_is = 1;
                }
                idx = decode_scf_diff(s, ch, layer);
                s->is_position[g][sfb] = idx & 1 ? -((idx + 1) >> 1) : idx >> 1;
            } else if (bsac_hyp(11) >= 2 && (bsac_hyp(11) != 4 || layer >= s->slayer_size) && band_is_zero(s, ch, g, sfb)) {
                s->scf[ch][g][sfb] = bsac_hyp(11) == 2 ? 0 : s->max_scalefactor[ch];
            } else {
                int d = decode_scf_diff(s, ch, layer);
                int scf = s->max_scalefactor[ch] - d;
                av_log(s->logctx, AV_LOG_TRACE, "SCFDIFF L%d m%d d%d\n", layer,
                       layer < s->slayer_size ? s->base_scf_model[ch] : s->enh_scf_model[ch], d);
                if (scf < 0) {
                    av_log(s->logctx, AV_LOG_ERROR, "BSAC scalefactor %d out of range\n", scf);
                    return AVERROR_INVALIDDATA;
                }
                s->scf[ch][g][sfb] = scf;
            }
            {
                int cost = before - s->available_len[s->cur_layer];
                int maxl = s->max_sfb_si_len[ch] + 5;
                av_log(s->logctx, AV_LOG_DEBUG, "SICOST sf L%d s%d cost %d max %d %s\n", layer, sfb, cost, maxl, cost > maxl ? "VIOL" : "ok");
            }
        }
    }
    return 0;
}

/* ---------------------------------------------------------------------- */
/* Bit-sliced spectral data, subclause 4.6.4.2                             */

static int sliced_bit_p0(const BSACContext *s, int ch, int g, int i, int snf)
{
    const int32_t *smp = s->sample[ch] + s->group_offset[g];
    const BSACProbTable *pt = &ff_bsac_prob_tables[s->cband_si[ch][g][i >> 5]];
    int level = FFMIN(pt->msb - snf, pt->nb_levels - 1);
    int higher = FFABS(smp[i]) >> snf;
    const int v = bsac_hyp(10);

    if (higher) {
        int idx = higher > 15 ? 15 : higher - 1;
        if (v == 1)
            idx = FFMIN(higher, 15);
        return pt->nonzero[level][FFMIN(idx, (1 << level) - 1 < 15 ? (1 << level) - 1 : 15)];
    } else {
        const int a = i & 3;
        const int32_t *q = smp + i - a;
        int hgfe = 0, bcd = 0, pos;

        for (int m = 0; m < 4; m++) {
            int hb = v == 5 ? !!(FFABS(q[m]) >> (snf - 1)) : !!(FFABS(q[m]) >> snf);
            if (v == 5 && m >= a) hb = !!(FFABS(q[m]) >> snf);
            if (v == 3)
                hgfe |= hb << m;
            else
                hgfe |= hb << (3 - m);
        }
        for (int m = 1; m <= a; m++) {
            int bit = v == 4 ? !!(FFABS(smp[i - m]) >> (snf - 1))
                             : (FFABS(smp[i - m]) >> (snf - 1)) & 1;
            if (v == 2)
                bcd |= bit << (3 - m);
            else
                bcd |= bit << (m - 1);
        }
        pos = ff_bsac_prob_pos[a][bcd & 7][hgfe];
        if (pos == 0xff || (!level && pos >= 15))
            pos = 0;
        return pt->zero[level][pos];
    }
}

static void decode_spectral_data(BSACContext *s, int start_g, int end_g,
                                 const int *start_index, const int *end_index,
                                 int thr_snf)
{
    int maxsnf = 0;

    if (!layer_data_available(s))
        return;

    for (int g = start_g; g < end_g; g++)
        for (int ch = 0; ch < s->nch; ch++)
            for (int i = start_index[g]; i < end_index[g]; i++)
                maxsnf = FFMAX(maxsnf, s->cur_snf[ch][s->group_offset[g] + i]);

    if (maxsnf <= thr_snf && av_log_get_level() >= AV_LOG_DEBUG)
        av_log(s->logctx, AV_LOG_DEBUG, "EXHAUST L%d avail %d range %d-%d\n", s->cur_layer,
               s->available_len[s->cur_layer], start_g, end_g);
    for (int snf = maxsnf; snf > thr_snf; snf--) {
        for (int g = start_g; g < end_g; g++) {
            for (int i = start_index[g]; i < end_index[g]; i++) {
                for (int ch = 0; ch < s->nch; ch++) {
                    const int k = s->group_offset[g] + i;
                    int32_t *smp = &s->sample[ch][k];

                    if (s->cur_snf[ch][k] < snf)
                        continue;

                    if (!*smp || s->sign_is_coded[ch][k]) {
                        if (s->cur_layer >= 10 && s->cur_layer <= 12 && av_log_get_level() >= AV_LOG_TRACE)
                            av_log(s->logctx, AV_LOG_TRACE, "SYM L%d snf %d i %d ch %d p0 %04x avail %d\n", s->cur_layer, snf, i, ch, sliced_bit_p0(s, ch, g, i, snf), s->available_len[s->cur_layer]);
                        if (decode_bit(s, sliced_bit_p0(s, ch, g, i, snf))) {
                            int32_t bit = 1 << (snf - 1);
                            *smp += *smp < 0 ? -bit : bit;
                        }
                    }
                    if (*smp && !s->sign_is_coded[ch][k]) {
                        if (!layer_data_available(s)) {
                            if (s->cur_layer == 0) av_log(s->logctx, AV_LOG_DEBUG, "STOP sign avail %d\n", s->available_len[0]);
                            return;
                        }
                        s->sign_bit = 1;
                        if (decode_bit(s, BSAC_P0_SIGN))
                            *smp = -*smp;
                        s->sign_bit = 0;
                        s->sign_is_coded[ch][k] = 1;
                    }
                    s->cur_snf[ch][k]--;
                    if (!layer_data_available(s)) {
                        if (s->cur_layer == 0) av_log(s->logctx, AV_LOG_DEBUG, "STOP bit avail %d\n", s->available_len[0]);
                        return;
                    }
                }
            }
        }
    }
}

static void decode_layer_spectra(BSACContext *s, int layer)
{
    int start_index[8], end_index[8];
    int g = s->layer_group[layer];

    start_index[g] = s->layer_start_index[layer];
    end_index[g]   = s->layer_end_index[layer];
    if (bsac_x(2) == 1 && layer >= s->slayer_size)
        end_index[g] = FFMIN(s->layer_end_cband[layer] * 32, s->group_size[g]);
    if (bsac_x(2) == 2 && layer >= s->slayer_size && s->layer_start_cband[layer] == s->layer_end_cband[layer])
        return;
    if (bsac_x(2) >= 3 && layer >= s->slayer_size)
        return;
    decode_spectral_data(s, g, g + 1, start_index, end_index,
                         layer < s->slayer_size ? s->base_snf_thr : 0);
}

static void decode_lower_spectra(BSACContext *s, int layer)
{
    int start_index[8] = { 0 }, end_index[8] = { 0 };

    if (bsac_x(1) == 1 && layer >= s->slayer_size)
        return;
    for (int play = 0; play < layer + (bsac_x(1) == 3 || bsac_x(2) == 4); play++)
        end_index[s->layer_group[play]] = bsac_x(1) == 2 ? s->layer_end_cband[play] * 32 :
                                           s->layer_end_index[play];
    if (bsac_x(1) == 4 && layer >= s->slayer_size)
        end_index[s->layer_group[layer]] = s->layer_start_cband[s->slayer_size] * 32;
    av_log(s->logctx, AV_LOG_DEBUG, "LOWER L%d avail %d\n", layer, s->available_len[s->cur_layer]);
    decode_spectral_data(s, 0, s->num_window_groups, start_index, end_index, 0);
    if (s->available_len[s->cur_layer] > 0)
        av_log(s->logctx, AV_LOG_DEBUG, "LOWDONE L%d left %d\n", layer, s->available_len[s->cur_layer]);
}

static void decode_higher_spectra(BSACContext *s, int layer)
{
    int start_index[8], end_index[8];

    for (int nlay = layer + 1; nlay < s->num_layers; nlay++) {
        int g = s->layer_group[nlay];
        start_index[g] = s->layer_start_index[nlay];
        end_index[g]   = s->layer_end_index[nlay];
        decode_spectral_data(s, g, g + 1, start_index, end_index, 0);
    }
}

static int decode_layer_element(BSACContext *s, int layer)
{
    int ret;

    s->cur_layer = layer;
    if ((ret = decode_layer_cband_si(s, layer)) < 0)
        return ret;
    if ((ret = decode_layer_sfb_si(s, layer)) < 0)
        return ret;

    decode_layer_spectra(s, layer);
    if (!s->sba_mode) {
        decode_lower_spectra(s, layer);
    } else if (s->terminal_layer[layer]) {
        decode_lower_spectra(s, layer);
        decode_higher_spectra(s, layer);
    }
    return 0;
}

static unsigned segment_end(const BSACContext *s, int layer, unsigned buf_bits)
{
    if (s->sba_mode)
        while (layer < s->num_layers - 1 && !s->terminal_layer[layer])
            layer++;
    else
        layer = s->num_layers - 1;
    return FFMIN(s->layer_bit_offset[layer + 1], buf_bits);
}

static void decode_layers(BSACContext *s, const uint8_t *buf, unsigned buf_bits)
{
    for (int layer = 0; layer < s->num_layers; layer++) {
        if (layer >= s->slayer_size && s->layer_bit_offset[layer] >= buf_bits)
            break;
        if (!layer || (s->sba_mode && s->terminal_layer[layer - 1]) ||
            (bsac_hyp(13) == 1 && layer == s->slayer_size)) {
            ar_init(&s->ar, buf, s->layer_bit_offset[layer],
                    segment_end(s, layer, buf_bits));
            /* The spec subtracts one termination bit here; the KBS sample
             * is only decoded correctly without it. */
            if (bsac_hyp(3))
                s->available_len[layer]--;
        }
        if (decode_layer_element(s, layer) < 0)
            break;
        if (layer == 0 && av_log_get_level() >= AV_LOG_DEBUG) {
            int mn[2] = { 99, 99 }, nz[2] = { 0 };
            for (int ch = 0; ch < s->nch; ch++)
                for (int i = 0; i < 32; i++) {
                    mn[ch] = FFMIN(mn[ch], s->cur_snf[ch][i]);
                    nz[ch] += !!s->sample[ch][i];
                }
            s->l0_min_snf[0] = mn[0];
            s->l0_min_snf[1] = mn[1];
            av_log(s->logctx, AV_LOG_DEBUG, "L0INFO si %d %d minsnf %d %d nz %d %d ms %d bm %d %d msl %d %d cst %d %d av1 %d tns %d %d msf %d %d\n",
                   s->cband_si[0][0][0], s->cband_si[1][0][0], mn[0], mn[1], nz[0], nz[1],
                   s->ms_mask_present, s->base_scf_model[0], s->base_scf_model[1],
                   s->max_sfb_si_len[0], s->max_sfb_si_len[1], s->cband_si_type[0], s->cband_si_type[1],
                   s->available_len[1], 0, 0, s->max_scalefactor[0], s->max_scalefactor[1]);
        }
        if (av_log_get_level() >= AV_LOG_DEBUG && (layer == s->slayer_size - 1 || layer == s->slayer_size)) {
            int lv[2][8] = { { 0 } };
            for (int ch = 0; ch < s->nch; ch++)
                for (int i = 0; i < s->layer_end_index[layer]; i++) {
                    int si = s->cband_si[ch][0][i >> 5];
                    if (!si) continue;
                    lv[ch][FFMIN(ff_bsac_prob_tables[si].msb - s->cur_snf[ch][i], 7)]++;
                }
            av_log(s->logctx, AV_LOG_DEBUG, "DEPTH L%d %d %d %d %d %d %d %d %d\n", layer,
                   lv[0][0]+lv[1][0], lv[0][1]+lv[1][1], lv[0][2]+lv[1][2], lv[0][3]+lv[1][3],
                   lv[0][4]+lv[1][4], lv[0][5]+lv[1][5], lv[0][6]+lv[1][6], lv[0][7]+lv[1][7]);
        }
        if (bsac_hyp(6) && layer + 1 >= bsac_hyp(6))
            break;
        av_log(s->logctx, AV_LOG_TRACE, "BSAC layer %2d: off %4d..%4d si_max %3d left %4d arpos %4d\n",
               layer, s->layer_bit_offset[layer], s->layer_bit_offset[layer + 1],
               s->layer_si_maxlen[layer], s->available_len[layer], (int)s->ar.pos - 30);
        if (layer + 1 == s->slayer_size && (bsac_hyp(13) == 2 || (bsac_hyp(13) == 3 && s->available_len[layer] < 0)))
            continue;
        if (layer + 1 < s->num_layers && (s->available_len[layer] > 0 || !bsac_hyp(8)))
            s->available_len[layer + 1] += s->available_len[layer];
    }
}

/* ---------------------------------------------------------------------- */
/* Reconstruction                                                          */

static av_always_inline int lcg_random(unsigned previous_val)
{
    union { unsigned u; int s; } v = { previous_val * 1664525u + 1013904223 };
    return v.s;
}

static float pow43(int q)
{
    int a = FFABS(q);
    float v = a < (1 << 13) ? av_int2float(ff_cbrt_tab[a]) : a * cbrtf(a);
    return q < 0 ? -v : v;
}

static void reconstruct(AACDecContext *ac, BSACContext *s, ChannelElement *che)
{
    const int win_len = s->window_sequence == EIGHT_SHORT_SEQUENCE ? s->win_len
                                                                   : s->frame_samples;
    const int max_sfb = s->max_sfb;

    for (int ch = 0; ch < s->nch; ch++) {
        SingleChannelElement *sce = &che->ch[ch];
        IndividualChannelStream *ics = &sce->ics;
        float *coef = sce->coeffs;
        int win = 0;

        for (int g = 0; g < s->num_window_groups; g++) {
            for (int sfb = 0; sfb < max_sfb; sfb++) {
                const int idx = g * max_sfb + sfb;
                if (s->noise_flag[ch][g][sfb] &&
                    !(s->nch == 2 && (s->ms_used[g][sfb] ||
                                      s->stereo_info[g][sfb] == 1 ||
                                      s->stereo_info[g][sfb] == 2))) {
                    sce->band_type[idx] = NOISE_BT;
                    sce->sfo[idx]       = av_clip(s->noise_nrg[ch][g][sfb], -100, 155);
                } else if (ch == 1 && s->stereo_info[g][sfb] >= 2) {
                    sce->band_type[idx] = s->stereo_info[g][sfb] == 2 ? INTENSITY_BT
                                                                      : INTENSITY_BT2;
                    sce->sfo[idx]       = av_clip(s->is_position[g][sfb], -155, 100) - 100;
                } else {
                    sce->band_type[idx] = 1;
                    sce->sfo[idx]       = av_clip(s->scf[ch][g][sfb], 0, 255) - 100;
                }
            }
        }
        ac->dsp.dequant_scalefactors(sce);

        memset(coef, 0, sizeof(sce->coeffs));
        for (int g = 0; g < s->num_window_groups; g++) {
            const int len = s->window_group_length[g];
            const int32_t *smp = s->sample[ch] + s->group_offset[g];
            const int n = len * win_len;

            for (int i = 0; i < n; i++) {
                /* Groups of 4 lines are interleaved over the windows of the group. */
                int w = len > 1 ? (i >> 2) % len : 0;
                int k = len > 1 ? (i >> 2) / len * 4 + (i & 3) : i;
                int sfb, band;

                if (!smp[i] || k >= ics->swb_offset[max_sfb])
                    continue;
                sfb  = s->sfb_of_line[k];
                band = sce->band_type[g * max_sfb + sfb];
                if (band == NOISE_BT || band == INTENSITY_BT || band == INTENSITY_BT2)
                    continue;
                coef[(win + w) * 128 + k] = pow43(smp[i]) *
                    ff_aac_pow2sf_tab[sce->sfo[g * max_sfb + sfb] + POW_SF2_ZERO];
            }

            /* perceptual noise substitution */
            for (int sfb = 0; sfb < max_sfb; sfb++) {
                const int idx = g * max_sfb + sfb;
                const int off = ics->swb_offset[sfb];
                const int off_len = ics->swb_offset[sfb + 1] - off;
                if (sce->band_type[idx] != NOISE_BT)
                    continue;
                for (int w = 0; w < len; w++) {
                    float *cfo = coef + (win + w) * 128 + off;
                    float scale;
                    if (ch == 1 && che->ch[0].band_type[idx] == NOISE_BT &&
                        s->noise_mode[g][sfb]) {
                        /* correlated noise: reuse the left channel noise */
                        const float *l = che->ch[0].coeffs + (win + w) * 128 + off;
                        float sign = s->noise_mode[g][sfb] == 2 ? -1.0f : 1.0f;
                        float el = ac->fdsp->scalarproduct_float(l, l, off_len);
                        scale = el > 0 ? sign * -sce->sf[idx] / sqrtf(el) : 0;
                        ac->fdsp->vector_fmul_scalar(cfo, l, scale, off_len);
                        continue;
                    }
                    for (int k = 0; k < off_len; k++) {
                        ac->random_state = lcg_random(ac->random_state);
                        cfo[k] = ac->random_state;
                    }
                    scale = -sce->sf[idx] /
                            sqrtf(ac->fdsp->scalarproduct_float(cfo, cfo, off_len));
                    ac->fdsp->vector_fmul_scalar(cfo, cfo, scale, off_len);
                }
            }
            win += len;
        }
    }

    if (s->nch == 2) {
        int ms_present = 0;

        che->max_sfb_ste = max_sfb;
        for (int g = 0; g < s->num_window_groups; g++) {
            for (int sfb = 0; sfb < max_sfb; sfb++) {
                int ms = s->ms_mask_present == 2 ||
                         (s->ms_mask_present == 1 && s->ms_used[g][sfb]) ||
                         (s->ms_mask_present == 3 && s->stereo_info[g][sfb] == 1);
                che->ms_mask[g * max_sfb + sfb] = ms;
                ms_present |= ms;
            }
        }
        if (ms_present)
            ac->dsp.apply_mid_side_stereo(ac, che);
        ac->dsp.apply_intensity_stereo(ac, che, 0);
    }
}

/* ---------------------------------------------------------------------- */

static void reset_frame_state(BSACContext *s)
{
    memset(s->cband_si,               0, sizeof(s->cband_si));
    memset(s->scf,                    0, sizeof(s->scf));
    memset(s->ms_used,                0, sizeof(s->ms_used));
    memset(s->stereo_info,            0, sizeof(s->stereo_info));
    memset(s->noise_flag,             0, sizeof(s->noise_flag));
    memset(s->noise_mode,             0, sizeof(s->noise_mode));
    memset(s->stereo_side_info_coded, 0, sizeof(s->stereo_side_info_coded));
    memset(s->noise_nrg,              0, sizeof(s->noise_nrg));
    memset(s->is_position,            0, sizeof(s->is_position));
    memset(s->sample,                 0, sizeof(s->sample));
    memset(s->cur_snf,                0, sizeof(s->cur_snf));
    memset(s->sign_is_coded,          0, sizeof(s->sign_is_coded));
    s->noise_pcm_flag[0] = s->noise_pcm_flag[1] = 1;
}

int ff_aac_bsac_decode_frame(AACDecContext *ac, ChannelElement *che,
                             const uint8_t *buf, int size)
{
    BSACContext *s = ac->bsac;
    const MPEG4AudioConfig *m4ac = &ac->oc[1].m4ac;
    GetBitContext gb;
    unsigned buf_bits;
    int header_bits, ret;

    if (size < 2)
        return AVERROR_INVALIDDATA;

    s->logctx        = ac->avctx;
    s->nch           = m4ac->chan_config == 2 ? 2 : 1;
    s->fs            = m4ac->sample_rate;
    s->frame_samples = m4ac->frame_length_short ? 960 : 1024;
    s->win_len       = s->frame_samples / 8;

    if ((ret = init_get_bits8(&gb, buf, size)) < 0)
        return ret;
    ret = decode_headers(ac, s, che, &gb);
    if (ret < 0)
        return ret;
    if (s->frame_length > size) {
        av_log(s->logctx, AV_LOG_WARNING, "BSAC frame truncated (%d > %d)\n",
               s->frame_length, size);
    } else {
        size = s->frame_length;
    }
    if (s->ms_mask_present && s->nch != 2)
        return AVERROR_INVALIDDATA;

    header_bits = get_bits_count(&gb);
    if (s->header_length < 15 && header_bits != (s->header_length + 7) * 8)
        av_log(s->logctx, AV_LOG_WARNING, "BSAC header length mismatch: %d != %d\n",
               header_bits >> 3, s->header_length + 7);

    init_bands(s, &che->ch[0].ics);
    reset_frame_state(s);
    ret = init_layers(s, header_bits);
    if (ret < 0)
        return ret;

    buf_bits = size * 8;
    decode_layers(s, buf, buf_bits);

    if (av_log_get_level() >= AV_LOG_DEBUG) {
        int ok = 0, bad = 0;
        for (int ch = 0; ch < s->nch; ch++)
            for (int g = 0; g < s->num_window_groups; g++)
                for (int c = 0; c * 32 < s->group_size[g]; c++) {
                    int si = s->cband_si[ch][g][c], msb, done = 1, mx = 0;
                    if (!si)
                        continue;
                    msb = ff_bsac_prob_tables[si].msb;
                    for (int i = c * 32; i < c * 32 + 32; i++) {
                        int k = s->group_offset[g] + i;
                        if (s->cur_snf[ch][k] >= msb)
                            done = 0;
                        mx = FFMAX(mx, FFABS(s->sample[ch][k]));
                    }
                    if (done) {
                        int top = 0;
                        while ((1 << top) <= mx) top++;
                        av_log(s->logctx, AV_LOG_DEBUG, "SITOP c%d si %d top %d ms %d bm %d%d ch %d l0started %d\n", c, si, top, s->ms_mask_present, s->base_scf_model[0], s->base_scf_model[1], ch, s->l0_min_snf[ch] < msb);
                        if (mx >= 1 << (msb - 1)) ok++; else { bad++; av_log(s->logctx, AV_LOG_DEBUG, "MSBBAD ch%d c%d si%d mx%d:", ch, c, si, mx);
                        for (int i = c * 32; i < c * 32 + 32; i++) av_log(s->logctx, AV_LOG_DEBUG, " %d", s->sample[ch][i]);
                        av_log(s->logctx, AV_LOG_DEBUG, "\n"); }
                    }
                }
        static int fcnt;
        av_log(s->logctx, AV_LOG_DEBUG, "FRAME %d\n", fcnt++);
        av_log(s->logctx, AV_LOG_DEBUG, "MSBCHECK ok %d bad %d ms %d tns %d%d scfm %d%d%d%d hl %d\n", ok, bad, s->ms_mask_present, che->ch[0].tns.present, che->ch[1].tns.present, s->base_scf_model[0], s->enh_scf_model[0], s->base_scf_model[1], s->enh_scf_model[1], s->header_length);
    }
    if (av_log_get_level() >= AV_LOG_TRACE) {
        for (int ch = 0; ch < s->nch; ch++) {
            char line[4096];
            int n = 0;
            n += snprintf(line + n, sizeof(line) - n, "ch%d cband_si:", ch);
            for (int c = 0; c < 16; c++)
                n += snprintf(line + n, sizeof(line) - n, " %d", s->cband_si[ch][0][c]);
            n += snprintf(line + n, sizeof(line) - n, " | scf:");
            for (int sfb = 0; sfb < s->max_sfb; sfb++)
                n += snprintf(line + n, sizeof(line) - n, " %d", s->scf[ch][0][sfb]);
            av_log(s->logctx, AV_LOG_TRACE, "%s\n", line);
            n = 0;
            for (int i = 288; i < 384; i++)
                n += snprintf(line + n, sizeof(line) - n, " %d", s->sample[ch][i]);
            av_log(s->logctx, AV_LOG_TRACE, "ch%d q:%s\n", ch, line);
        }
        if (s->nch == 2) {
            char line[256];
            int n = 0;
            for (int sfb = 0; sfb < s->max_sfb; sfb++)
                n += snprintf(line + n, sizeof(line) - n, "%d", s->ms_used[0][sfb]);
            av_log(s->logctx, AV_LOG_TRACE, "ms_mask_present %d ms_used %s\n", s->ms_mask_present, line);
        }
    }

    {
        static FILE *dump;
        if (!dump && getenv("BSAC_DUMP"))
            dump = fopen(getenv("BSAC_DUMP"), "wb");
        if (dump)
            fwrite(s->sample, sizeof(s->sample), 1, dump);
    }

    reconstruct(ac, s, che);

    return size;
}

av_cold int ff_aac_bsac_init(AACDecContext *ac)
{
    if (!ac->bsac) {
        ac->bsac = av_mallocz(sizeof(*ac->bsac));
        if (!ac->bsac)
            return AVERROR(ENOMEM);
    }
    return 0;
}

av_cold void ff_aac_bsac_close(AACDecContext *ac)
{
    av_freep(&ac->bsac);
}
