/*
 * ER BSAC decoder tables
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

#ifndef AVCODEC_AAC_AACDEC_BSAC_TAB_H
#define AVCODEC_AAC_AACDEC_BSAC_TAB_H

#include <stdint.h>

typedef struct BSACCbandSiType {
    uint8_t max_cband_si_len;
    uint8_t largest_cband0_si;   ///< largest cband_si of the 0th coding band
    uint8_t largest_cband_si;    ///< largest cband_si of the other coding bands
    uint8_t model;               ///< index into ff_bsac_cband_si_models
} BSACCbandSiType;

/**
 * Probabilities of the "0" symbol for the bit-sliced data of a coding band.
 * Sub-tables are indexed by the significance relative to the MSB plane
 * (0 = MSB, up to nb_levels - 1 = "others").
 */
typedef struct BSACProbTable {
    uint8_t msb;                     ///< MSB plane
    uint8_t nb_levels;
    const uint16_t *zero[5];         ///< the decoded higher bits are zero
    const uint16_t *nonzero[5];      ///< the decoded higher bits are non-zero
} BSACProbTable;

extern const BSACCbandSiType ff_bsac_cband_si_type[32];

extern const uint16_t *const ff_bsac_scf_models[8];
extern const uint16_t *const ff_bsac_cband_si_models[7];
extern const uint16_t *const ff_bsac_cband_si_cband0_model;
extern const uint16_t *const ff_bsac_ms_used_model;
extern const uint16_t *const ff_bsac_stereo_info_model;
extern const uint16_t *const ff_bsac_noise_flag_model;
extern const uint16_t *const ff_bsac_noise_mode_model;

extern const uint16_t ff_bsac_min_p0[14];
extern const uint16_t ff_bsac_max_p0[14];

extern const uint8_t ff_bsac_prob_pos[4][8][16];

extern const BSACProbTable ff_bsac_prob_tables[23];

#endif /* AVCODEC_AAC_AACDEC_BSAC_TAB_H */
