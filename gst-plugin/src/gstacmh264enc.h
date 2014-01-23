/* GStreamer ACM H264 Encoder plugin
 * Copyright (C) 2013 Atmark Techno, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */


#ifndef __GST_ACMH264ENC_H__
#define __GST_ACMH264ENC_H__

#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/video/gstvideoencoder.h>
#include "gstacmv4l2bufferpool.h"

G_BEGIN_DECLS

#define GST_TYPE_ACMH264ENC \
	(gst_acm_h264_enc_get_type())
#define GST_ACMH264ENC(obj) \
	(G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_ACMH264ENC,GstAcmH264Enc))
#define GST_ACMH264ENC_CLASS(klass) \
	(G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_ACMH264ENC,GstAcmH264EncClass))
#define GST_IS_ACMH264ENC(obj) \
	(G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_ACMH264ENC))
#define GST_IS_ACMH264ENC_CLASS(klass) \
	(G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_ACMH264ENC))


/* 入力画素数	*/
#define GST_ACMH264ENC_WIDTH_MIN		80
#define GST_ACMH264ENC_WIDTH_MAX		1920
#define GST_ACMH264ENC_HEIGHT_MIN		80
#define GST_ACMH264ENC_HEIGHT_MAX		1080
// #define GST_ACMH264ENC_STRIDE_MIN		GST_ACMH264ENC_WIDTH_MIN
// #define GST_ACMH264ENC_STRIDE_MAX		4096
#define GST_ACMH264ENC_X_OFFSET_MIN		0
#define GST_ACMH264ENC_X_OFFSET_MAX		1920
#define GST_ACMH264ENC_Y_OFFSET_MIN		0
#define GST_ACMH264ENC_Y_OFFSET_MAX		1080

/* インタレースモード	*/
enum {
	GST_ACMH264ENC_INTERLACE_MODE_UNKNOWN			= -1,
	GST_ACMH264ENC_INTERLACE_MODE_PROGRESSIVE		= 0,
	GST_ACMH264ENC_INTERLACE_MODE_INTERLACE_TOP		= 1,
	GST_ACMH264ENC_INTERLACE_MODE_INTERLACE_BOTTOM	= 2,
};

/* 目標ビットレート		*/
#define GST_ACMH264ENC_BITRATE_MIN		16000
#define GST_ACMH264ENC_BITRATE_MAX		40000000

/* 1フレームあたりの最大エンコードサイズ */
#define GST_ACMH264ENC_MAXFRAMESIZE_MIN		(GST_ACMH264ENC_BITRATE_MIN / 8)
#define GST_ACMH264ENC_MAXFRAMESIZE_MAX		5000000

/* レート制御モード	*/
enum {
	GST_ACMH264ENC_RATE_CONTROL_MODE_UNKNOWN		= -1,
	GST_ACMH264ENC_RATE_CONTROL_MODE_CBR_SKIP		= 0,
	GST_ACMH264ENC_RATE_CONTROL_MODE_CBR_NON_SKIP	= 1,
	GST_ACMH264ENC_RATE_CONTROL_MODE_VBR_NON_SKIP	= 2,
};

/* フレームレート解像度		*/
#define GST_ACMH264ENC_FRAME_RATE_RESOLUTION_MIN		1
#define GST_ACMH264ENC_FRAME_RATE_RESOLUTION_MAX		240000

/* フレーム時間間隔		*/
#define GST_ACMH264ENC_FRAME_RATE_TICK_MIN		1
#define GST_ACMH264ENC_FRAME_RATE_TICK_MAX		4004

/* 最大 GOP 長		*/
#define GST_ACMH264ENC_MAX_GOP_LENGTH_MIN		0
#define GST_ACMH264ENC_MAX_GOP_LENGTH_MAX		120

/* 1フレームあたりの最大エンコードサイズ(バイト)		*/
#define GST_ACMH264ENC_MAX_FRAME_SIZE_MAX		5000000

/* Bピクチャモード		*/
enum {
	GST_ACMH264ENC_B_PIC_MODE_UNKNOWN = -1,
	GST_ACMH264ENC_B_PIC_MODE_0_B_PIC = 0,
	GST_ACMH264ENC_B_PIC_MODE_1_B_PIC = 1,
	GST_ACMH264ENC_B_PIC_MODE_2_B_PIC = 2,
	GST_ACMH264ENC_B_PIC_MODE_3_B_PIC = 3,
};

/* クラス定義		*/
typedef struct _GstAcmH264EncPrivate GstAcmH264EncPrivate;

typedef struct _GstAcmH264Enc {
	GstVideoEncoder element;

	/* input video caps */
	gint input_width;	/* 80〜1920画素	*/
	gint input_height;	/* 80〜1080画素	*/
	gint32 input_format;

	/* video state */
	GstVideoCodecState *input_state;

	/* output video caps */
	gint output_width;
	gint output_height;

	/* ACM common */
	/* the video device */
	char *videodev;					/* property */
	gint video_fd;
	/* buffer pool */
	GstAcmV4l2BufferPool* pool_in;
	GstAcmV4l2BufferPool* pool_out;

	/* ACM h264enc */
#if 0	/* USE_STRIDE_PROP	*/
	gint stride;					/* property */
#endif
#if 0	/* 入力はプログレッシブのみに制限		*/
	gint interlace_mode;
#endif
	gint32 bit_rate;				/* property */
	gint32 max_frame_size;			/* property */
	gint rate_control_mode;			/* property */
	gint32 frame_rate_resolution;
	gint32 frame_rate_tick;
	gint max_GOP_length;			/* property */
	gint B_pic_mode;				/* property */
	gint32 x_offset;				/* property */
	gint32 y_offset;				/* property */

	/*< private >*/
	GstAcmH264EncPrivate *priv;
} GstAcmH264Enc;

typedef struct _GstAcmH264EncClass {
	GstVideoEncoderClass parent_class;
} GstAcmH264EncClass;

GType gst_acm_h264_enc_get_type(void);

G_END_DECLS

#endif /* __GST_ACMH264ENC_H__ */


/*
 * End of file
 */
