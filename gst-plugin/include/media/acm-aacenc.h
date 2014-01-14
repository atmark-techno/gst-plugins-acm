/*
 * acm-aacenc.h - header for AV Codec Middleware AAC Encoder Driver
 *
 * Copyright (C) 2013 Atmark Techno, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */
#if !defined(ARMADILLO_ACM_AACENC_H)
#define ARMADILLO_ACM_AACENC_H

#if defined(__KERNEL__)
#include <media/v4l2-device.h>
#include <linux/types.h>
#else
#include <stdint.h>
#include <stdbool.h>
#endif

enum acm_aac_bs_format {
	ACM_AAC_BS_FORMAT_ADIF = 1, /* Unsupported */
	ACM_AAC_BS_FORMAT_ADTS = 2,
	ACM_AAC_BS_FORMAT_RAW = 3,
};

enum acm_aac_channel_mode {
	ACM_AAC_CHANNEL_MODE_MONAURAL = 0,
	ACM_AAC_CHANNEL_MODE_STEREO = 1,
	ACM_AAC_CHANNEL_MODE_DUALMONAURAL = 2,
};

enum acm_aacenc_sampling_rate_idx {
	ACM_SAMPLING_RATE_96000 = 0,
	ACM_SAMPLING_RATE_88200 = 1,
	ACM_SAMPLING_RATE_64000 = 2,
	ACM_SAMPLING_RATE_48000 = 3,
	ACM_SAMPLING_RATE_44100 = 4,
	ACM_SAMPLING_RATE_32000 = 5,
	ACM_SAMPLING_RATE_24000 = 6,
	ACM_SAMPLING_RATE_22050 = 7,
	ACM_SAMPLING_RATE_16000 = 8,
	ACM_SAMPLING_RATE_12000 = 9,
	ACM_SAMPLING_RATE_11025 = 10,
	ACM_SAMPLING_RATE_8000  = 11,
};

#define V4L2_CID_CHANNEL_MODE	(V4L2_CID_PRIVATE_BASE + 0)
#define V4L2_CID_SAMPLE_RATE	(V4L2_CID_PRIVATE_BASE + 1)
#define V4L2_CID_BIT_RATE	(V4L2_CID_PRIVATE_BASE + 2)
#define V4L2_CID_ENABLE_CBR	(V4L2_CID_PRIVATE_BASE + 3)

#endif /* !defined(ARMADILLO_ACM_AACENC_H) */
