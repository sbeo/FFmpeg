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

#ifndef AVCODEC_AAC_AACDEC_BSAC_H
#define AVCODEC_AAC_AACDEC_BSAC_H

#include "aacdec.h"

int ff_aac_bsac_init(AACDecContext *ac);

void ff_aac_bsac_close(AACDecContext *ac);

/**
 * Decode one bsac_raw_data_block() into the spectral coefficients of che,
 * including M/S and intensity stereo.
 *
 * @param size  number of bytes available starting at buf
 * @return number of bytes consumed (frame_length) or a negative error code
 */
int ff_aac_bsac_decode_frame(AACDecContext *ac, ChannelElement *che,
                             const uint8_t *buf, int size);

#endif /* AVCODEC_AAC_AACDEC_BSAC_H */
