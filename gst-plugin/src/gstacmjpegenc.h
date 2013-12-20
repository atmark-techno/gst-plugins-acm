/* GStreamer ACM JPEG Encoder plugin
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


#ifndef __GST_ACMJPEGENC_H__
#define __GST_ACMJPEGENC_H__

#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/video/gstvideoencoder.h>
#include "gstacmv4l2bufferpool.h"

G_BEGIN_DECLS

#define GST_TYPE_ACMJPEGENC \
	(gst_acm_jpeg_enc_get_type())
#define GST_ACMJPEGENC(obj) \
	(G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_ACMJPEGENC,GstAcmJpegEnc))
#define GST_ACMJPEGENC_CLASS(klass) \
	(G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_ACMJPEGENC,GstAcmJpegEncClass))
#define GST_IS_ACMJPEGENC(obj) \
	(G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_ACMJPEGENC))
#define GST_IS_ACMJPEGENC_CLASS(klass) \
	(G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_ACMJPEGENC))


/* 入出力画素数	*/
#define GST_ACMJPEGENC_WIDTH_MIN		16
#define GST_ACMJPEGENC_WIDTH_MAX		1920
#define GST_ACMJPEGENC_HEIGHT_MIN		16
#define GST_ACMJPEGENC_HEIGHT_MAX		1080
#define GST_ACMJPEGENC_X_OFFSET_MIN		0
#define GST_ACMJPEGENC_X_OFFSET_MAX		1920
#define GST_ACMJPEGENC_Y_OFFSET_MIN		0
#define GST_ACMJPEGENC_Y_OFFSET_MAX		1080

/* JPEG品質 		*/
#define GST_ACMJPEGENC_QUALITY_MIN		0
#define GST_ACMJPEGENC_QUALITY_MAX		100

/* クラス定義		*/
typedef struct _GstAcmJpegEncPrivate GstAcmJpegEncPrivate;

typedef struct _GstAcmJpegEnc {
	GstVideoEncoder element;

	/* input video caps */
	gint input_width;	/* 16〜1920画素	*/
	gint input_height;	/* 16〜1080画素	*/
	gint input_format;

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

	/* ACM jpegenc */
	gint jpeg_quality;				/* property */
	gint32 x_offset;				/* property */
	gint32 y_offset;				/* property */

	/*< private >*/
	GstAcmJpegEncPrivate *priv;
} GstAcmJpegEnc;

typedef struct _GstAcmJpegEncClass {
	GstVideoEncoderClass parent_class;
} GstAcmJpegEncClass;

GType gst_acm_jpeg_enc_get_type(void);

G_END_DECLS

#endif /* __GST_ACMJPEGENC_H__ */


/*
 * End of file
 */
