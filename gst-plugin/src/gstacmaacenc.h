/* GStreamer ACM AAC Encoder plugin
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

#ifndef __GST_ACMAACENC_H__
#define __GST_ACMAACENC_H__

#include <gst/gst.h>
#include <gst/audio/gstaudioencoder.h>
#include "gstacmv4l2bufferpool.h"

G_BEGIN_DECLS

#define GST_TYPE_ACMAACENC \
	(gst_acm_aac_enc_get_type ())
#define GST_ACMAACENC(obj) \
	(G_TYPE_CHECK_INSTANCE_CAST ((obj), GST_TYPE_ACMAACENC, GstAcmAacEnc))
#define GST_ACMAACENC_CLASS(klass) \
	(G_TYPE_CHECK_CLASS_CAST ((klass), GST_TYPE_ACMAACENC, GstAcmAacEncClass))
#define GST_IS_ACMAACENC(obj) \
	(G_TYPE_CHECK_INSTANCE_TYPE ((obj), GST_TYPE_ACMAACENC))
#define GST_IS_ACMAACENC_CLASS(klass) \
	(G_TYPE_CHECK_CLASS_TYPE ((klass), GST_TYPE_ACMAACENC))


/* Bitrate		*/
#define GST_ACMAACENC_BITRATE_MIN		16000
#define GST_ACMAACENC_BITRATE_MAX		288000

/* CBR Flag		*/
enum {
	GST_ACMAACENC_ENABLE_CBR_UNKNOWN	= -1,
	GST_ACMAACENC_ENABLE_CBR_FALSE		= 0,
	GST_ACMAACENC_ENABLE_CBR_TRUE		= 1,
};

/* クラス定義		*/
typedef struct _GstAcmAacEncPrivate GstAcmAacEncPrivate;

typedef struct _GstAcmAacEnc {
	GstAudioEncoder element;

	/* input audio caps */
	gint channels;   	/* number of channels  */
	gint sample_rate; 	/* sample rate    */

	/* negotiate */
	gint mpegversion;

	/* ACM common */
	/* the video device */
	char *videodev;					/* property */
	gint video_fd;
	/* buffer pool */
	GstAcmV4l2BufferPool* pool_in;
	GstAcmV4l2BufferPool* pool_out;
	
	/* ACM aacenc */
	gint output_bit_rate;			/* property */
	gint output_format;				/* non property */
	gint enable_cbr;				/* property */
	gboolean dual_monaural;			/* property */

	/*< private >*/
	GstAcmAacEncPrivate *priv;
} GstAcmAacEnc;

typedef struct _GstAcmAacEncClass {
	GstAudioEncoderClass parent_class;
} GstAcmAacEncClass;

GType gst_acm_aac_enc_get_type(void);

G_END_DECLS

#endif /* __GST_ACMAACENC_H__ */

/*
 * End of file
 */
