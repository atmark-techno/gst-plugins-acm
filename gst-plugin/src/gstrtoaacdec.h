/* GStreamer RTO AAC Decoder plugin
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

#ifndef __GST_RTOAACDEC_H__
#define __GST_RTOAACDEC_H__

#include <semaphore.h>
#include <gst/gst.h>
#include <gst/audio/gstaudiodecoder.h>
#include "gstv4l2bufferpool.h"

G_BEGIN_DECLS

#define GST_TYPE_RTOAACDEC \
  (gst_rto_aac_dec_get_type ())
#define GST_RTOAACDEC(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST ((obj), GST_TYPE_RTOAACDEC, GstRtoAacDec))
#define GST_RTOAACDEC_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST ((klass), GST_TYPE_RTOAACDEC, GstRtoAacDecClass))
#define GST_IS_RTOAACDEC(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE ((obj), GST_TYPE_RTOAACDEC))
#define GST_IS_RTOAACDEC_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE ((klass), GST_TYPE_RTOAACDEC))


/* 入力フォーマット : R-MobileA1マルチメディアミドル 機能仕様書 Ver.0.14		*/
typedef enum {
	GST_RTOAACDEC_IN_FMT_UNKNOWN	= -1,
	GST_RTOAACDEC_IN_FMT_ADIF    	= 1,	/* ADIF 未サポート	*/
	GST_RTOAACDEC_IN_FMT_ADTS    	= 2,	/* ADTS形式			*/
	GST_RTOAACDEC_IN_FMT_RAW     	= 3,	/* Raw形式			*/
} GstRtoAacDecInFmt;

/* ミックスダウンの可否	 : R-MobileA1マルチメディアミドル 機能仕様書 Ver.0.14	*/
#define GST_RTOAACDEC_NOT_ALLOW_MIXDOWN			0
#define GST_RTOAACDEC_ALLOW_MIXDOWN				1

/* ミックスダウンモード	 : R-MobileA1マルチメディアミドル 機能仕様書 Ver.0.14	*/
#define GST_RTOAACDEC_MIXDOWN_MODE_STEREO		0
#define GST_RTOAACDEC_MIXDOWN_MODE_MONO			1

/* ミックスダウン時の準拠規格 : R-MobileA1マルチメディアミドル 機能仕様書 Ver.0.14	*/
#define GST_RTOAACDEC_MIXDOWN_COMPLIANT_ISO		0
#define GST_RTOAACDEC_MIXDOWN_COMPLIANT_ARIB	1

/* PCM出力フォーマット : R-MobileA1マルチメディアミドル 機能仕様書 Ver.0.14		*/
#define GST_RTOAACDEC_PCM_FMT_INTERLEAVED		0
#define GST_RTOAACDEC_PCM_FMT_NON_INTERLEAVED	1

/* 最大チャンネル数		*/
#define GST_RTOAACDEC_MAX_CHANNEL_MIN			1
#define GST_RTOAACDEC_MAX_CHANNEL_MAX			6

/* クラス定義		*/
typedef struct _GstRtoAacDecPrivate GstRtoAacDecPrivate;

typedef struct _GstRtoAacDec {
	GstAudioDecoder element;
	
	/* input audio caps */
	guint      samplerate; /* sample rate    */
	guint      channels;   /* number of channels  */

	/* We must differentiate between raw and packetised streams */
	gboolean packetised;	/* フレーム単位の入力か？	*/
	
	/* デコード終了通知用		*/
	gboolean is_eos_received;
	gboolean is_decode_all_done;
	sem_t sem_decode_all_done;
	gboolean is_do_stop;

	/* output audio caps */
	guint	out_samplerate;
	guint	out_channels;

	/* RTO common */
	/* the video device */
	char *videodev;
	gint video_fd;
	/* 初回データ処理済みフラグ	*/
	gboolean is_handled_1stframe;
	/* buffer pool */
	GstV4l2BufferPool* pool_in;
	GstV4l2BufferPool* pool_out;
	/* QBUF(V4L2_BUF_TYPE_VIDEO_OUTPUT) 用カウンタ	*/
	gint num_inbuf_acquired;
	
	/* RTO aacdec */
	/* 入力フォーマット
	 * ADTS or RAW
	 */
	GstRtoAacDecInFmt input_format;
	
	/* ミックスダウンの可否
	 * 0：ミックスダウンを行わない
	 * 1：ミックスダウン処理を行い、ミックスダウンしたPCMデータを出力
	 */
	gboolean allow_mixdown;

	/* ミックスダウンモード
	 * 0：ステレオにミックスダウン
	 * 1：モノラルにミックスダウン
	 */
	gint	mixdown_mode;

	/* ミックスダウン時の準拠規格
	 * 0：ISO/IEC13818-7、ISO/IEC14496-3準拠
	 * 1：ARIB STD-B21準拠
	 */
	gint 	compliant_standard;

	/* PCM出力フォーマット
	 * 0：インタリーブ
	 * 1：非インタリーブ
	 */
	gint 	pcm_format;

	/*< private >*/
	GstRtoAacDecPrivate *priv;
	GstPadChainFunction base_chain;
} GstRtoAacDec;

typedef struct _GstRtoAacDecClass {
  GstAudioDecoderClass parent_class;
} GstRtoAacDecClass;

GType gst_rto_aac_dec_get_type(void);

G_END_DECLS

#endif /* __GST_RTOAACDEC_H__ */

/*
 * End of file
 */
