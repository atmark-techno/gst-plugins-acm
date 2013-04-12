/* GStreamer RTO H264 Decoder plugin
 * Copyright (C) 2013 Kazunari Ohtsuka <<user@hostname.org>>
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


#ifndef __GST_RTOH264DEC_H__
#define __GST_RTOH264DEC_H__

#include <semaphore.h>
#include <linux/videodev2.h>
#include <gst/gst.h>
#include <gst/gstatomicqueue.h>
#include <gst/video/video.h>
#include <gst/video/gstvideodecoder.h>
#include "gstv4l2bufferpool.h"

G_BEGIN_DECLS

#define GST_TYPE_RTOH264DEC \
  (gst_rto_h264_dec_get_type())
#define GST_RTOH264DEC(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_RTOH264DEC,GstRtoH264Dec))
#define GST_RTOH264DEC_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_RTOH264DEC,GstRtoH264DecClass))
#define GST_IS_RTOH264DEC(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_RTOH264DEC))
#define GST_IS_RTOH264DEC_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_RTOH264DEC))


/* 入力フォーマット	*/
#define GST_RTOH264DEC_IN_FMT_UNKNOWN	0xFFFFFFFF
	/* ES (with NAL start code)	*/
#define GST_RTOH264DEC_IN_FMT_ES		V4L2_PIX_FMT_H264
	/* MP4 (without start code) */
#define GST_RTOH264DEC_IN_FMT_MP4		V4L2_PIX_FMT_H264_NO_SC	

/* 参照フレーム数		*/
#define GST_RTOH264DEC_FMEM_NUM_MIN		2
#define GST_RTOH264DEC_FMEM_NUM_MAX		32

/* 中間バッファのピクチャ数	*/
#define GST_RTOH264DEC_BUF_PIC_CNT_MIN	1
#define GST_RTOH264DEC_BUF_PIC_CNT_MAX	144

/* 入出力画素数	*/
#define GST_RTOH264DEC_WIDTH_MIN		0	/* 80 */
#define GST_RTOH264DEC_WIDTH_MAX		1920
#define GST_RTOH264DEC_HEIGHT_MIN		0	/* 80 */
#define GST_RTOH264DEC_HEIGHT_MAX		1080

/* 出力フォーマット	*/
#define GST_RTOH264DEC_OUT_FMT_UNKNOWN	0xFFFFFFFF
#define GST_RTOH264DEC_OUT_FMT_YUV420	V4L2_PIX_FMT_YUV420
#define GST_RTOH264DEC_OUT_FMT_RGB32	V4L2_PIX_FMT_RGB32
#define GST_RTOH264DEC_OUT_FMT_RGB24	V4L2_PIX_FMT_RGB24

#define MAX_SIZE_SPSPPS		1024

/* クラス定義		*/
typedef struct _GstRtoH264DecPrivate GstRtoH264DecPrivate;

typedef struct _GstRtoH264Dec {
	GstVideoDecoder element;

	/* input video caps */
	guint width;	/* 80〜1920画素	*/
	guint height;	/* 80〜1080画素	*/
	/* codec data	*/
	gint spspps_size;
	guchar spspps[MAX_SIZE_SPSPPS];

	/* video state */
	GstVideoCodecState *input_state;
	GstVideoCodecState *output_state;

	/* デコード終了通知用		*/
	gboolean is_eos_received;
	gboolean is_decode_all_done;
	sem_t sem_decode_all_done;
	gboolean is_do_stop;

	/* output video caps */
	guint out_width;
	guint out_height;
	char *out_video_fmt_str;
	GstVideoFormat out_video_fmt;
	gboolean do_RGB24_to_RGB16;

	/* RTO common */
	/* the video device */
	char *videodev;
	gint video_fd;
	/* 初回データ処理済みフラグ	*/
	gboolean is_handled_1stframe;
	/* buffer pool */
	GstV4l2BufferPool* pool_out;
	/* QBUF(V4L2_BUF_TYPE_VIDEO_OUTPUT) 用カウンタ	*/
	gint num_inbuf_acquired;
	gint num_inbuf_queued;

	/* RTO h264dec */
	/* 参照フレーム数
	 * 2 ~ 32
	 */
	guint fmem_num;

	/* 中間バッファのピクチャ数(=10)
	 * 1 ~ 144
	 */
	guint buffering_pic_cnt;

	/* フレームレート(=未使用)
	 */
//	guint frame_rate;

	/* 入力フォーマット(= GST_RTOH264DEC_IN_FMT_MP4 固定)
	 * GST_RTOH264DEC_IN_FMT_ES: NALスタートモード(=未使用)
	 * GST_RTOH264DEC_IN_FMT_MP4：sps,pps付加、サイズ情報モード
	 */
	guint32 input_format;

	/* 出力フォーマット
	 * GST_RTOH264DEC_OUT_FMT_YUV420
	 * GST_RTOH264DEC_OUT_FMT_RGB32
	 * GST_RTOH264DEC_OUT_FMT_RGB24
	 */
	guint32 output_format;
	
	/* VIO6の有効無効フラグ
	 * 0 : 無効
	 * 1 : 有効
	 */
	gboolean enable_vio6;

	/* fbdev sink が dma-buf を使用する場合のアドレス保存		*/
	gboolean using_fb_dmabuf;
	gint num_fb_dmabuf;
	gint fb_dmabuf_fd[MAX_NUM_DMABUF];
	gint fb_dmabuf_index[MAX_NUM_DMABUF];

	/*< private >*/
	GstRtoH264DecPrivate *priv;
	GstPadChainFunction base_chain;
} GstRtoH264Dec;

typedef struct _GstRtoH264DecClass {
	GstVideoDecoderClass parent_class;
} GstRtoH264DecClass;

GType gst_rto_h264_dec_get_type(void);

G_END_DECLS

#endif /* __GST_RTOH264DEC_H__ */


/*
 * End of file
 */
