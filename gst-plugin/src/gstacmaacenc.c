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

/**
 * SECTION:element-acmaacenc
 *
 * acmaacenc encodes raw audio to AAC (MPEG-4 part 3) streams.
 *
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 * gst-launch audiotestsrc wave=sine num-buffers=100 ! audioconvert ! acmaacenc ! matroskamux ! filesink location=sine.mkv
 * ]| Encode a sine beep as aac and write to matroska container.
 * |[
 * gst-launch filesrc location=abc.wav ! wavparse ! audioresample ! audioconvert ! acmaacenc ! filesink location=abc.aac
 * ]|
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <gst/audio/audio.h>
#include <media/acm-aacenc.h>

#include "gstacmaacenc.h"
#include "v4l2_util.h"
#include "gstacm_util.h"
#include "gstacm_debug.h"


/* エンコーダv4l2デバイスのドライバ名 */
#define DRIVER_NAME						"acm-aacenc"

/* 出力フォーマット ADIF のサポート	*/
#define SUPPORT_OUTPUT_FMT_ADIF			0

/* バッファプール内のバッファを no copy で down stream に push する	*/
#define DO_PUSH_POOLS_BUF				0	/* do copy */

/* デバイスに確保するバッファ数
 * プレエンコードに 2 フレーム必要なため、バッファ数は 3 以上必要
 */
#define DEFAULT_NUM_BUFFERS_IN			3
#define DEFAULT_NUM_BUFFERS_OUT			3

/* エンコーダー初期化パラメータのデフォルト値	*/
#define DEFAULT_VIDEO_DEVICE			"/dev/video0"
#define DEFAULT_BITRATE					64000
#define DEFAULT_OUT_FORMAT				ACM_AAC_BS_FORMAT_ADTS
#define DEFAULT_ENABLE_CBR				GST_ACMAACENC_ENABLE_CBR_FALSE /* VBR */

/* select() の timeout 時間 */
#define SELECT_TIMEOUT_MSEC				1000

/* 1フレームのサンプル数	*/
#define SAMPLES_PER_FRAME				1024

/* プレエンコード回数（現状固定値）	*/
#define PRE_ENC_NUM						2

/* プレエンコード用に確保する入力データサイズ 
 * 1024 sample x 2ch x 16bit x 2 buf
 */
#define PRE_ENC_BUF_SIZE				(SAMPLES_PER_FRAME * 2 * 2 * PRE_ENC_NUM)

/* デバッグログ出力フラグ		*/
#define DBG_LOG_PERF_CHAIN				0
#define DBG_LOG_PERF_SELECT_IN			0
#define DBG_LOG_PERF_PUSH				0
#define DBG_LOG_PERF_SELECT_OUT			0
#define DBG_LOG_OUT_TIMESTAMP			0

/* select() による待ち時間の計測		*/
#define DBG_MEASURE_PERF_SELECT_IN		0
#define DBG_MEASURE_PERF_SELECT_OUT		0
#define DBG_MEASURE_PERF_HANDLE_FRAME	0
#define DBG_MEASURE_PERF_FINISH_FRAME	0

/* 入力・出力データのファイルへのダンプ	*/
#define DBG_DUMP_IN_BUF					0
#define DBG_DUMP_OUT_BUF				0


#if DBG_MEASURE_PERF_SELECT_IN
static double g_time_total_select_in = 0;
#endif
#if DBG_MEASURE_PERF_SELECT_OUT
static double g_time_total_select_out = 0;
#endif


/* private member	*/
struct _GstAcmAacEncPrivate
{
	/* HW エンコーダの初期化済みフラグ	*/
	gboolean is_inited_encoder;

	/* QBUF(V4L2_BUF_TYPE_VIDEO_OUTPUT) 用カウンタ	*/
	gint num_inbuf_acquired;

	/* V4L2_BUF_TYPE_VIDEO_OUTPUT 側に入力したフレーム数と、
	 * V4L2_BUF_TYPE_VIDEO_CAPTURE 側から取り出したフレーム数の差分
	 */
	gint in_out_frame_count;

	/* プレエンコード回数	*/
	gint pre_encode_num;

	/* 入力フレーム用カウンタ	*/
	gint in_frame_count;
	
	/* プレエンコー用バッファ	*/
	GstBuffer * pre_encode_buf;
	gint pre_encode_buf_offset;
};

GST_DEBUG_CATEGORY_STATIC (acmaacenc_debug);
#define GST_CAT_DEFAULT acmaacenc_debug

#define GST_ACMAACENC_GET_PRIVATE(obj)  \
	(G_TYPE_INSTANCE_GET_PRIVATE ((obj), GST_TYPE_ACMAACENC, \
		GstAcmAacEncPrivate))

/* property */
enum
{
	PROP_0,
	PROP_DEVICE,
	PROP_BITRATE,
	PROP_ENABLE_CBR,
};

/* pad template caps for source and sink pads.	*/
#define SAMPLE_RATES	" 8000, " \
						"11025, " \
						"12000, " \
						"16000, " \
						"22050, " \
						"24000, " \
						"32000, " \
						"44100, " \
						"48000 " 

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (
		"audio/x-raw, "
		"format = (string) " GST_AUDIO_NE (S16) ", "
		"layout = (string) interleaved, "
		"rate = (int) {" SAMPLE_RATES "}, "
		"channels = (int) [ 1, 2 ] "
	)
);

static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
	GST_STATIC_CAPS (
		"audio/mpeg, "
		"mpegversion = (int) 4, "
		"channels = (int) [ 1, 2 ], "
		"rate = (int) {" SAMPLE_RATES "}, "
#if SUPPORT_OUTPUT_FMT_ADIF
		"stream-format = (string) { adts, raw, adif };"
#else
		"stream-format = (string) { adts, raw };"
#endif
		"audio/mpeg, "
		"mpegversion = (int) 2, "
		"channels = (int) [ 1, 2 ], "
		"rate = (int) {" SAMPLE_RATES "}, "
#if SUPPORT_OUTPUT_FMT_ADIF
		"stream-format = (string) { adts, raw, adif };"
#else
		"stream-format = (string) { adts, raw };"
#endif
	)
);

/* DecoderSpecificInfo (codec_data)	*/
static const int mpeg4audio_sample_rates[16] = {
    96000, 88200, 64000, 48000, 44100, 32000,
    24000, 22050, 16000, 12000, 11025, 8000, 7350,
};
static const unsigned char mpeg4audio_channels[16] = {
    0, 1, 2, 3, 4, 5, 6, 8,
};
#define MAX_DECODER_SPECIFIC_INFO_SIZE	7	/* LC + SBR + PS config */
#define AOT_AAC_LC 2	/* AudioObjectType : ISO/IEC 13818-7 : 2006 Low Complexity */
static gboolean
get_decoder_specific_info(GstAcmAacEnc * me,
	unsigned char *oData, unsigned long *ioLen)
{
    int i;
    int srate_idx = -1, ch_idx = -1;
    int window_size = 0;
	
	if (NULL == oData || NULL == ioLen
		|| *ioLen < MAX_DECODER_SPECIFIC_INFO_SIZE) {
		GST_ERROR_OBJECT (me, "invalid arg.");
		
		return FALSE;
	}

    for (i = 0; i < 16; i++) {
		if (mpeg4audio_sample_rates[i] == me->sample_rate) {
			srate_idx = i;
			break;
		}
    }
    for (i = 0; i < 16; i++) {
		if (mpeg4audio_channels[i] == me->channels) {
			ch_idx = i;
			break;
		}
    }

	if (srate_idx < 0) {
		GST_ERROR_OBJECT (me, "invalid sample rate : %d", me->sample_rate);
		return FALSE;
	}
	if (ch_idx < 0) {
		GST_ERROR_OBJECT (me, "invalid channels : %d", me->channels);
		return FALSE;
	}

	oData[0] = AOT_AAC_LC << 3 | srate_idx >> 1;
	oData[1] = srate_idx << 7 | ch_idx << 3 | window_size << 2;

	*ioLen = 2;
	return TRUE;
}

/*  ビットレートの推奨値 : R-MobileA1マルチメディアミドル 機能仕様書	*/
static gint
default_bit_rate (gint rate)
{
	if (46009 <= rate)
		return 70000;	// 48,000 Hz
	else if (37566 <= rate)
		return 64000;	// 44,100 Hz
	else if (27713 <= rate)
		return 64000;	// 32,000 Hz
	else if (23004 <= rate)
		return 48000;	// 24,000 Hz
	else if (18783 <= rate)
		return 44100;	// 22,050 Hz
	else if (13856 <= rate)
		return 51200;	// 16,000 Hz
	else if (11502 <= rate)
		return 38400;	// 12,000 Hz
	else if (9391 <= rate)
		return 35280;	// 11,025 Hz
	else
		return 25600;	// 8,000 Hz
}

/*  enum acm_aacenc_sampling_rate_idx : acm-aacenc	*/
static gint
pcm_sample_rate_idx (gint rate)
{
	if (92017 <= rate)
		return ACM_SAMPLING_RATE_96000;	// 96,000Hz
	else if (75132 <= rate)
		return ACM_SAMPLING_RATE_88200;	// 88,200 Hz
	else if (55426 <= rate)
		return ACM_SAMPLING_RATE_64000;	// 64,000 Hz
	else if (46009 <= rate)
		return ACM_SAMPLING_RATE_48000;	// 48,000 Hz
	else if (37566 <= rate)
		return ACM_SAMPLING_RATE_44100;	// 44,100 Hz
	else if (27713 <= rate)
		return ACM_SAMPLING_RATE_32000;	// 32,000 Hz
	else if (23004 <= rate)
		return ACM_SAMPLING_RATE_24000;	// 24,000 Hz
	else if (18783 <= rate)
		return ACM_SAMPLING_RATE_22050;	// 22,050 Hz
	else if (13856 <= rate)
		return ACM_SAMPLING_RATE_16000;	// 16,000 Hz
	else if (11502 <= rate)
		return ACM_SAMPLING_RATE_12000;	// 12,000 Hz
	else if (9391 <= rate)
		return ACM_SAMPLING_RATE_11025;	// 11,025 Hz
	else
		return ACM_SAMPLING_RATE_8000;	// 8,000 Hz
}

/*  チャンネル数とチャンネルモードの対応 : acm-aacenc	*/
static gint
pcm_channel_mode (gint ch)
{
	if (1 == ch)
		return ACM_AAC_CHANNEL_MODE_MONAURAL;
	else if (2 == ch)
		return ACM_AAC_CHANNEL_MODE_STEREO;
	else
		return -1;	// error
}

/* string for "stream-format" caps	*/
static char*
stream_format_str(gint format)
{
	char* str = NULL;

	switch (format) {
	case ACM_AAC_BS_FORMAT_ADTS:
		str = "adts";
		break;
	case ACM_AAC_BS_FORMAT_RAW:
		str = "raw";
		break;
#if SUPPORT_OUTPUT_FMT_ADIF
	case ACM_AAC_BS_FORMAT_ADIF:
		str = "adif";
		break;
#endif
	default:
		str = "unknown";
		break;
	}

	return str;
}

/* channel position */
#define ACMAAC_ENC_MAX_CHANNELS	6
static const GstAudioChannelPosition
	aac_channel_positions[][ACMAAC_ENC_MAX_CHANNELS] = {
	{	/* 1 ch: Mono */
		GST_AUDIO_CHANNEL_POSITION_MONO
	},
	{	/* 2 ch: front left + front right (front stereo) */
		GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
		GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT
	},
	{	/* 3 ch: front center + front stereo */
        GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
        GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
        GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
	},
	{	/* 4 ch: front center + front stereo + back center */
        GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
        GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
        GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
		GST_AUDIO_CHANNEL_POSITION_REAR_CENTER
	},
	{	/* 5 ch: front center + front stereo + back stereo */
        GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
        GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
        GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
        GST_AUDIO_CHANNEL_POSITION_REAR_LEFT,
		GST_AUDIO_CHANNEL_POSITION_REAR_RIGHT
	},
	{	/* 6ch: front center + front stereo + back stereo + LFE */
        GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
        GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
        GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
        GST_AUDIO_CHANNEL_POSITION_REAR_LEFT,
        GST_AUDIO_CHANNEL_POSITION_REAR_RIGHT,
		GST_AUDIO_CHANNEL_POSITION_LFE1
	}
};

/* GObject base class method */
static void gst_acm_aac_enc_set_property (GObject * object,
	guint prop_id, const GValue * value, GParamSpec * pspec);
static void gst_acm_aac_enc_get_property (GObject * object,
	guint prop_id, GValue * value, GParamSpec * pspec);
static void gst_acm_aac_enc_finalize (GObject * object);
/* GstAudioEncoder base class method */
static gboolean gst_acm_aac_enc_open (GstAudioEncoder * enc);
static gboolean gst_acm_aac_enc_close (GstAudioEncoder * enc);
static gboolean gst_acm_aac_enc_start (GstAudioEncoder * enc);
static gboolean gst_acm_aac_enc_stop (GstAudioEncoder * enc);
static GstCaps *gst_acm_aac_enc_getcaps (GstAudioEncoder * enc,
	GstCaps * filter);
static gboolean gst_acm_aac_enc_set_format (GstAudioEncoder * enc,
	GstAudioInfo * info);
static GstFlowReturn gst_acm_aac_enc_handle_frame (GstAudioEncoder * enc,
    GstBuffer * buffer);
static GstFlowReturn gst_acm_aac_enc_pre_push (GstAudioEncoder *enc,
	GstBuffer **buffer);
static void gst_acm_aac_enc_flush (GstAudioEncoder * enc);
static gboolean gst_acm_aac_enc_sink_event (GstAudioEncoder * enc,
	GstEvent *event);
/* GstAcmAacEnc class method */
static gboolean gst_acm_aac_enc_init_encoder (GstAcmAacEnc * me);
static gboolean gst_acm_aac_enc_cleanup_encoder (GstAcmAacEnc * me);
static GstFlowReturn gst_acm_aac_enc_handle_in_frame_with_wait(
	GstAcmAacEnc * me, GstBuffer *inbuf);
static GstFlowReturn gst_acm_aac_enc_handle_in_frame(GstAcmAacEnc * me,
	GstBuffer *v4l2buf_in, GstBuffer *inbuf);
static GstFlowReturn gst_acm_aac_enc_handle_out_frame_with_wait(
	GstAcmAacEnc * me);
static GstFlowReturn gst_acm_aac_enc_handle_out_frame(GstAcmAacEnc * me,
	GstBuffer *v4l2buf_out);

#define gst_acm_aac_enc_parent_class parent_class
G_DEFINE_TYPE (GstAcmAacEnc, gst_acm_aac_enc, GST_TYPE_AUDIO_ENCODER);


static void
gst_acm_aac_enc_set_property (GObject * object, guint prop_id,
							   const GValue * value, GParamSpec * pspec)
{
	GstAcmAacEnc *me = GST_ACMAACENC (object);
	
	switch (prop_id) {
	case PROP_DEVICE:
		if (me->videodev) {
			g_free (me->videodev);
		}
		me->videodev = g_value_dup_string (value);
		break;
	case PROP_BITRATE:
		me->output_bit_rate = g_value_get_int (value);
		break;
	case PROP_ENABLE_CBR:
		me->enable_cbr = g_value_get_int (value);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
gst_acm_aac_enc_get_property (GObject * object, guint prop_id,
							   GValue * value, GParamSpec * pspec)
{
	GstAcmAacEnc *me = GST_ACMAACENC (object);
	
	switch (prop_id) {
	case PROP_DEVICE:
		g_value_set_string (value, me->videodev);
		break;
	case PROP_BITRATE:
		g_value_set_int (value, me->output_bit_rate);
		break;
	case PROP_ENABLE_CBR:
		g_value_set_int (value, me->enable_cbr);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
gst_acm_aac_enc_class_init (GstAcmAacEncClass * klass)
{
	GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
	GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
	GstAudioEncoderClass *base_class = GST_AUDIO_ENCODER_CLASS (klass);

	g_type_class_add_private (klass, sizeof (GstAcmAacEncPrivate));

	gobject_class->set_property = gst_acm_aac_enc_set_property;
	gobject_class->get_property = gst_acm_aac_enc_get_property;
	gobject_class->finalize = gst_acm_aac_enc_finalize;

	g_object_class_install_property (gobject_class, PROP_DEVICE,
		g_param_spec_string ("device", "device",
			"The video device eg: /dev/video0 "
			"default device is calculate from driver name.",
			DEFAULT_VIDEO_DEVICE, G_PARAM_READWRITE));

	g_object_class_install_property (gobject_class, PROP_BITRATE,
		g_param_spec_int ("bitrate", "Bitrate (bps)",
			"Average Bitrate (ABR) in bits/sec."
			"default bitrate is calculate from sample rate.",
			GST_ACMAACENC_BITRATE_MIN, GST_ACMAACENC_BITRATE_MAX,
			DEFAULT_BITRATE, G_PARAM_READWRITE));

	g_object_class_install_property (gobject_class, PROP_ENABLE_CBR,
		g_param_spec_int ("enable-cbr", "Enable CBR",
			"0: VBR, 1:CBR",
			GST_ACMAACENC_ENABLE_CBR_FALSE, GST_ACMAACENC_ENABLE_CBR_TRUE,
			DEFAULT_ENABLE_CBR, G_PARAM_READWRITE));

	gst_element_class_add_pad_template (element_class,
			gst_static_pad_template_get (&src_template));
	gst_element_class_add_pad_template (element_class,
			gst_static_pad_template_get (&sink_template));
	
	gst_element_class_set_static_metadata (element_class,
			"ACM AAC audio encoder", "Codec/Encoder/Audio",
			"ACM MPEG-2/4 AAC encoder", "atmark techno");
	
	base_class->open = GST_DEBUG_FUNCPTR (gst_acm_aac_enc_open);
	base_class->close = GST_DEBUG_FUNCPTR (gst_acm_aac_enc_close);
	base_class->start = GST_DEBUG_FUNCPTR (gst_acm_aac_enc_start);
	base_class->stop = GST_DEBUG_FUNCPTR (gst_acm_aac_enc_stop);
	base_class->getcaps = GST_DEBUG_FUNCPTR (gst_acm_aac_enc_getcaps);
	base_class->set_format = GST_DEBUG_FUNCPTR (gst_acm_aac_enc_set_format);
	base_class->handle_frame = GST_DEBUG_FUNCPTR (gst_acm_aac_enc_handle_frame);
	base_class->pre_push = GST_DEBUG_FUNCPTR (gst_acm_aac_enc_pre_push);
	base_class->flush = GST_DEBUG_FUNCPTR (gst_acm_aac_enc_flush);
	base_class->sink_event = GST_DEBUG_FUNCPTR (gst_acm_aac_enc_sink_event);
}

static void
gst_acm_aac_enc_init (GstAcmAacEnc * me)
{
	me->priv = GST_ACMAACENC_GET_PRIVATE (me);

	me->channels = -1;
	me->sample_rate = -1;

	me->mpegversion = -1;

	me->video_fd = -1;
	me->pool_in = NULL;
	me->pool_out = NULL;

	me->priv->is_inited_encoder = FALSE;
	me->priv->num_inbuf_acquired = 0;
	me->priv->in_out_frame_count = 0;
	me->priv->pre_encode_num = 0;
	me->priv->in_frame_count = 0;
	me->priv->pre_encode_buf = NULL;
	me->priv->pre_encode_buf_offset = 0;

	/* property	*/
	me->videodev = NULL;
	me->output_bit_rate = -1; /* not DEFAULT_BITRATE */
	me->output_format = -1; /* not DEFAULT_OUT_FORMAT */
	me->enable_cbr = -1; /* not DEFAULT_ENABLE_CBR */
}

static void
gst_acm_aac_enc_finalize (GObject * object)
{
	GstAcmAacEnc *me = GST_ACMAACENC (object);
	
    GST_INFO_OBJECT (me, "AACENC FINALIZE");

	/* プロパティとして保持するため、gst_acm_aac_enc_close() 内で free してはいけない	*/
	if (me->videodev) {
		g_free(me->videodev);
		me->videodev = NULL;
	}

	G_OBJECT_CLASS (parent_class)->finalize (object);
}

static gboolean
gst_acm_aac_enc_open (GstAudioEncoder * enc)
{
	GstAcmAacEnc *me = GST_ACMAACENC (enc);
	
	/* プロパティとしてセットされていなければ、デバイスを検索
	 * 他のプロパティは、gst_acm_aac_enc_set_format() で設定
	 */
	if (NULL == me->videodev) {
		me->videodev = gst_v4l2_getdev(DRIVER_NAME);
	}

	GST_INFO_OBJECT (me, "AACENC OPEN ACM ENCODER. (%s)", me->videodev);
	
	/* open device	*/
	GST_INFO_OBJECT (me, "Trying to open device %s", me->videodev);
	if (! gst_v4l2_open (me->videodev, &(me->video_fd), TRUE)) {
        GST_ELEMENT_ERROR (me, RESOURCE, NOT_FOUND, (NULL),
			("Failed open device %s. (%s)", me->videodev, g_strerror (errno)));
		return FALSE;
	}
	GST_INFO_OBJECT (me, "Opened device '%s' successfully", me->videodev);

	return TRUE;
}

static gboolean
gst_acm_aac_enc_close (GstAudioEncoder * enc)
{
	GstAcmAacEnc *me = GST_ACMAACENC (enc);
	
	GST_INFO_OBJECT (me, "AACENC CLOSE ACM ENCODER. (%s)", me->videodev);

	/* close device	*/
	if (me->video_fd > 0) {
		gst_v4l2_close(me->videodev, me->video_fd);
		me->video_fd = -1;
	}
	
	return TRUE;
}

/*	global setup	*/
static gboolean
gst_acm_aac_enc_start (GstAudioEncoder * enc)
{
	GstAcmAacEnc *me = GST_ACMAACENC (enc);
	
	GST_INFO_OBJECT (me, "AACENC START");

	/* プロパティ以外の変数を再初期化		*/
	me->channels = -1;
	me->sample_rate = -1;
	
	me->mpegversion = -1;

	me->pool_in = NULL;
	me->pool_out = NULL;

	me->priv->is_inited_encoder = FALSE;
	me->priv->num_inbuf_acquired = 0;
	me->priv->in_out_frame_count = 0;
	me->priv->pre_encode_num = 0;
	me->priv->in_frame_count = 0;
	me->priv->pre_encode_buf
		= gst_buffer_new_allocate(NULL, PRE_ENC_BUF_SIZE, NULL);
	if (NULL == me->priv->pre_encode_buf) {
		GST_ERROR_OBJECT (me, "Out of memory");

		return FALSE;
	}
	me->priv->pre_encode_buf_offset = 0;

	return TRUE;
}

/*	end of all processing	*/
static gboolean
gst_acm_aac_enc_stop (GstAudioEncoder * enc)
{
	GstAcmAacEnc *me = GST_ACMAACENC (enc);
	
	GST_INFO_OBJECT (me, "AACENC STOP");
	
	/* cleanup encoder	*/
	gst_acm_aac_enc_cleanup_encoder (me);

	if (me->priv->pre_encode_buf) {
		gst_buffer_unref(me->priv->pre_encode_buf);
		me->priv->pre_encode_buf = NULL;
	}

	return TRUE;
}

static GstCaps *
gst_acm_aac_enc_getcaps (GstAudioEncoder * enc, GstCaps * filter)
{
	static volatile gsize sinkcaps = 0;

	if (g_once_init_enter (&sinkcaps)) {
		GstCaps *tmp = gst_caps_new_empty ();
		GstStructure *s, *t;
		gint i, c;
		static const int rates[] = {
			8000, 11025, 12000, 16000, 22050, 24000,
			32000, 44100, 48000
		};
		GValue rates_arr = { 0, };
		GValue tmp_v = { 0, };

		g_value_init (&rates_arr, GST_TYPE_LIST);
		g_value_init (&tmp_v, G_TYPE_INT);
		for (i = 0; i < G_N_ELEMENTS (rates); i++) {
			g_value_set_int (&tmp_v, rates[i]);
			gst_value_list_append_value (&rates_arr, &tmp_v);
		}
		g_value_unset (&tmp_v);

		s = gst_structure_new ("audio/x-raw",
							   "format", G_TYPE_STRING, GST_AUDIO_NE (S16),
							   "layout", G_TYPE_STRING, "interleaved", NULL);
		gst_structure_set_value (s, "rate", &rates_arr);

		for (i = 1; i <= 6; i++) {
			guint64 channel_mask = 0;
			t = gst_structure_copy (s);

			gst_structure_set (t, "channels", G_TYPE_INT, i, NULL);
			if (i > 1) {
				for (c = 0; c < i; c++) {
					channel_mask |=
					G_GUINT64_CONSTANT (1) << aac_channel_positions[i - 1][c];
				}

				gst_structure_set (t,
					"channel-mask", GST_TYPE_BITMASK, channel_mask, NULL);
			}
			gst_caps_append_structure (tmp, t);
		}
		gst_structure_free (s);
		g_value_unset (&rates_arr);

		GST_INFO_OBJECT (enc, "Generated sinkcaps: %" GST_PTR_FORMAT, tmp);

		g_once_init_leave (&sinkcaps, (gsize) tmp);
	}

	return gst_audio_encoder_proxy_getcaps (enc, (GstCaps *) sinkcaps, filter);
}

/*	format of input audio data	*/
static gboolean
gst_acm_aac_enc_set_format (GstAudioEncoder * enc, GstAudioInfo * info)
{
	GstAcmAacEnc *me = GST_ACMAACENC (enc);
	gboolean ret = TRUE;
	GstCaps *caps = NULL;
	GstCaps *srccaps = NULL;
	
	GST_INFO_OBJECT (me, "AACENC SET FORMAT: %" GST_PTR_FORMAT, info);

	/* channels and sample_rate */
	me->channels = info->channels;
	GST_INFO_OBJECT (me, "channels : %d", me->channels);
	me->sample_rate = info->rate;
	GST_INFO_OBJECT (me, "sample_rate : %d", me->sample_rate);

	/* negotiate stream format */
	me->mpegversion = 4;	/* default setup */
	caps = gst_pad_get_allowed_caps (GST_AUDIO_ENCODER_SRC_PAD (me));
	GST_INFO_OBJECT (me, "allowed src caps: %" GST_PTR_FORMAT, caps);
	if (caps && gst_caps_get_size (caps) > 0) {
		GstStructure *s = gst_caps_get_structure (caps, 0);
		const gchar *str = NULL;
		gint i = 4;

		if ((str = gst_structure_get_string (s, "stream-format"))) {
			if (0 == strcmp (str, "adts")) {
				GST_INFO_OBJECT (me, "use ADTS format for output");
				me->output_format = ACM_AAC_BS_FORMAT_ADTS;
			} else if (0 == strcmp (str, "raw")) {
				GST_INFO_OBJECT (me, "use RAW format for output");
				me->output_format = ACM_AAC_BS_FORMAT_RAW;
			}
#if SUPPORT_OUTPUT_FMT_ADIF
			else if (0 == strcmp (str, "adif")) {
				GST_INFO_OBJECT (me, "use ADIF format for output");
				me->output_format = ACM_AAC_BS_FORMAT_ADIF;
			}
#endif
			else {
				GST_WARNING_OBJECT (me,
					"unknown stream-format: %s, use ADTS format for output", str);
				me->output_format = ACM_AAC_BS_FORMAT_ADTS;
			}
		}

		if (! gst_structure_get_int (s, "mpegversion", &i) || i == 4) {
			me->mpegversion = 4;
		} else {
			me->mpegversion = 2;
		}
	}
	if (caps) {
		gst_caps_unref (caps);
	}
	
	/* エンコードパラメータの決定	*/
	if (me->output_bit_rate <= 0) {
		me->output_bit_rate = default_bit_rate(info->rate);
	}
	if (me->output_format < /* not <= */ 0) {
		me->output_format = DEFAULT_OUT_FORMAT;
	}
	if (me->enable_cbr < /* not <= */ 0) {
		me->enable_cbr = DEFAULT_ENABLE_CBR;
	}

	/* プレエンコード回数の決定	*/
	me->priv->pre_encode_num = PRE_ENC_NUM;	// 現状固定値

	/* now create a caps for it all */
	srccaps = gst_caps_new_simple ("audio/mpeg",
		"mpegversion", G_TYPE_INT, me->mpegversion,
		"channels", G_TYPE_INT, me->channels,
		"rate", G_TYPE_INT, me->sample_rate,
		"stream-format", G_TYPE_STRING, stream_format_str(me->output_format),
		NULL);

	/* DecoderSpecificInfo is only available for mpegversion = 4 */
	if (4 == me->mpegversion) {
		guint8 config[MAX_DECODER_SPECIFIC_INFO_SIZE];
		gulong config_len = MAX_DECODER_SPECIFIC_INFO_SIZE;
		
		/* get the config string */
		GST_DEBUG_OBJECT (me, "retrieving decoder specific info");

		if (get_decoder_specific_info (me, config, &config_len)) {
			GstBuffer *codec_data;
			
			/* copy it into a buffer */
			codec_data = gst_buffer_new_and_alloc (config_len);
			gst_buffer_fill (codec_data, 0, config, config_len);
			
			/* add to caps */
			gst_caps_set_simple (srccaps,
				"codec_data", GST_TYPE_BUFFER, codec_data, NULL);
			
			gst_buffer_unref (codec_data);
		}
		else {
			goto failed_get_decoder_specific_info;
		}
	}

	GST_INFO_OBJECT (me, "src pad caps: %" GST_PTR_FORMAT, srccaps);

	ret = gst_audio_encoder_set_output_format (GST_AUDIO_ENCODER (me), srccaps);
	gst_caps_unref (srccaps);
	if (! ret) {
		goto set_output_format_failed;
	}
	if (! gst_audio_encoder_negotiate (enc)) {
		goto negotiate_faile;
	}

#if 0	/* for debug	*/
	GST_INFO_OBJECT (me, "current src caps: %" GST_PTR_FORMAT,
					 gst_pad_get_current_caps (GST_AUDIO_ENCODER_SRC_PAD (me)));
	GST_INFO_OBJECT (me, "current sink caps: %" GST_PTR_FORMAT,
					 gst_pad_get_current_caps (GST_AUDIO_ENCODER_SINK_PAD (me)));
#endif

	/* ビットレートの範囲チェック	*/
	if (46009 <= me->sample_rate) {
		// 48,000 Hz
		if (me->output_bit_rate < 70000 || 288000 < me->output_bit_rate) {
			goto invalid_bitrate;
		}
	}
	else if (37566 <= me->sample_rate) {
		// 44,100 Hz
		if (me->output_bit_rate < 64000 || 264600 < me->output_bit_rate) {
			goto invalid_bitrate;
		}
	}
	else if (27713 <= me->sample_rate) {
		// 32,000 Hz
		if (me->output_bit_rate < 46500 || 192000 < me->output_bit_rate) {
			goto invalid_bitrate;
		}
	}
	else if (23004 <= me->sample_rate) {
		// 24,000 Hz
		if (me->output_bit_rate < 35000 || 144000 < me->output_bit_rate) {
			goto invalid_bitrate;
		}
	}
	else if (18783 <= me->sample_rate) {
		// 22,050 Hz
		if (me->output_bit_rate < 32000 || 132300 < me->output_bit_rate) {
			goto invalid_bitrate;
		}
	}
	else if (13856 <= me->sample_rate) {
		// 16,000 Hz
		if (me->output_bit_rate < 23250 || 96000 < me->output_bit_rate) {
			goto invalid_bitrate;
		}
	}
	else if (11502 <= me->sample_rate) {
		// 12,000 Hz
		if (me->output_bit_rate < 24000 || 72000 < me->output_bit_rate) {
			goto invalid_bitrate;
		}
	}
	else if (9391 <= me->sample_rate) {
		// 11,025 Hz
		if (me->output_bit_rate < 22000 || 66150 < me->output_bit_rate) {
			goto invalid_bitrate;
		}
	}
	else {
		// 8,000 Hz
		if (me->output_bit_rate < 16000 || 48000 < me->output_bit_rate) {
			goto invalid_bitrate;
		}
	}

	/* initialize HW encoder	*/
	if (! gst_acm_aac_enc_init_encoder(me)) {
		goto init_failed;
	}

	/* report needs to base class
	 * HW encoder needs 1024 sample per input
	 */
	gst_audio_encoder_set_frame_samples_min (enc, SAMPLES_PER_FRAME);
	gst_audio_encoder_set_frame_samples_max (enc, SAMPLES_PER_FRAME);
	gst_audio_encoder_set_frame_max (enc, 1);

out:
	return ret;

	/* ERRORS */
set_output_format_failed:
	{
		GST_ELEMENT_ERROR (me, STREAM, ENCODE, (NULL),
			("Failed to gst_audio_encoder_set_output_format()"));
		ret = FALSE;
		goto out;
	}
invalid_bitrate:
	{
		GST_ELEMENT_ERROR (me, STREAM, ENCODE, (NULL),
			("Invalid bitrate (%d) at samplerate %d Hz",
			 me->output_bit_rate, me->sample_rate));
		ret = FALSE;
		goto out;
	}
failed_get_decoder_specific_info:
	{
		GST_ELEMENT_ERROR (me, STREAM, ENCODE, (NULL),
			("Failed to get decoder specific info"));
		ret = FALSE;
		goto out;
	}
init_failed:
	{
		GST_ELEMENT_ERROR (me, STREAM, ENCODE, (NULL),
			("Failed to init encoder from stream"));
		ret = FALSE;
		goto out;
	}
negotiate_faile:
	{
		GST_ELEMENT_ERROR (me, STREAM, ENCODE, (NULL),
			("Failed to negotiate with downstream elements"));
		ret = FALSE;
		goto out;
	}
}

static GstFlowReturn
gst_acm_aac_enc_handle_frame (GstAudioEncoder * enc, GstBuffer * buffer)
{
	GstAcmAacEnc *me = GST_ACMAACENC (enc);
	GstFlowReturn flowRet = GST_FLOW_OK;
#if DBG_MEASURE_PERF_HANDLE_FRAME
	static double interval_time_start = 0, interval_time_end = 0;
#endif

#if DBG_LOG_PERF_CHAIN
	GST_INFO_OBJECT (me, "# AACENC-CHAIN HANDLE FRMAE START");
#endif

	/* we don't deal with squeezing remnants, so simply discard those */
	if (G_UNLIKELY (buffer == NULL)) {
		GST_WARNING_OBJECT (me, "AACENC HANDLE FRMAE : handled NULL buffer.");

		goto out;
	}

	GST_DEBUG_OBJECT (me, "AACENC HANDLE FRMAE - size:%d, ref:%d, flags:%d ...",
					  gst_buffer_get_size(buffer),
					  GST_OBJECT_REFCOUNT_VALUE(buffer),
					  GST_BUFFER_FLAGS(buffer));

#if DBG_MEASURE_PERF_HANDLE_FRAME
	interval_time_end = gettimeofday_sec();
	if (interval_time_start > 0) {
		GST_INFO_OBJECT(me, "handle_frame() at(ms) : %10.10f",
						(interval_time_end - interval_time_start)*1e+3);
	}
	interval_time_start = gettimeofday_sec();
#endif

#if DBG_DUMP_IN_BUF		/* for debug	*/
	dump_input_buf(buffer);
#endif

	/* 初回の入力のみ、プレエンコード（2フレーム）分データをセットしてqbufする	*/
	if (me->priv->in_frame_count <= me->priv->pre_encode_num) {
		GstMapInfo map;

		me->priv->in_frame_count++;

		if (me->priv->in_frame_count < me->priv->pre_encode_num) {
			/* 1 フレーム目は保持するだけ	*/
			GST_DEBUG_OBJECT (me, "frame:%d - save pre_encode_buf",
							  me->priv->in_frame_count);

			gst_buffer_map (buffer, &map, GST_MAP_READ);
			gst_buffer_fill (me->priv->pre_encode_buf, 0, map.data, map.size);
			me->priv->pre_encode_buf_offset += map.size;
			gst_buffer_unmap (buffer, &map);

			me->priv->in_out_frame_count++;

			goto out;
		}
		else if (me->priv->in_frame_count == me->priv->pre_encode_num) {
			/* 1 フレーム目と2 フレーム目のデータを合わせて、デバイスへ入力する	*/
			GST_DEBUG_OBJECT (me, "frame:%d - append pre_encode_buf",
							  me->priv->in_frame_count);

			gst_buffer_map (buffer, &map, GST_MAP_READ);
			gst_buffer_fill (me->priv->pre_encode_buf,
				me->priv->pre_encode_buf_offset, map.data, map.size);
			gst_buffer_unmap (buffer, &map);
			gst_buffer_resize(me->priv->pre_encode_buf, 0,
				me->priv->pre_encode_buf_offset + gst_buffer_get_size(buffer));
			
			buffer = me->priv->pre_encode_buf;
		}
		else {
			/* プレエンコード用バッファを解放		*/
			GST_DEBUG_OBJECT (me, "frame:%d - delete pre_encode_buf",
							  me->priv->in_frame_count);

			if (me->priv->pre_encode_buf) {
				gst_buffer_unref(me->priv->pre_encode_buf);
				me->priv->pre_encode_buf = NULL;
			}
		}
	}

	/* 出力 : REQBUF 分入力した後は、エンコード済みデータができるのを待って取り出す	*/
	if (me->priv->num_inbuf_acquired >= me->pool_in->num_buffers) {
		flowRet = gst_acm_aac_enc_handle_out_frame_with_wait(me);
		if (GST_FLOW_OK != flowRet) {
			goto out;
		}
	}

	/* 入力		*/
	if (me->priv->num_inbuf_acquired < me->pool_in->num_buffers) {
		GstBuffer *v4l2buf_in = NULL;

		GST_DEBUG_OBJECT(me, "acquire_buffer");
		flowRet = gst_v4l2_buffer_pool_acquire_buffer(
			GST_BUFFER_POOL_CAST (me->pool_in), &v4l2buf_in, NULL);
		if (GST_FLOW_OK != flowRet) {
			GST_ERROR_OBJECT (me, "gst_v4l2_buffer_pool_acquire_buffer() returns %s",
							  gst_flow_get_name (flowRet));
			goto no_buffer;
		}
		
		me->priv->num_inbuf_acquired++;

		flowRet = gst_acm_aac_enc_handle_in_frame(me, v4l2buf_in, buffer);
		if (GST_FLOW_OK != flowRet) {
			goto out;
		}
		me->priv->in_out_frame_count++;
	}
	else {
		flowRet = gst_acm_aac_enc_handle_in_frame_with_wait(me, buffer);
		if (GST_FLOW_OK != flowRet) {
			goto out;
		}
	}

out:
#if DBG_LOG_PERF_CHAIN
	GST_INFO_OBJECT (me, "# AACENC-CHAIN HANDLE FRMAE END");
#endif
	return flowRet;

	/* ERRORS */
no_buffer:
	{
		GST_ELEMENT_ERROR (me, RESOURCE, FAILED, (NULL),
			("could not allocate buffer"));
		flowRet = GST_FLOW_ERROR;
		goto out;
	}
}

static GstFlowReturn
gst_acm_aac_enc_pre_push (GstAudioEncoder *enc, GstBuffer **buffer)
{
#if DBG_LOG_OUT_TIMESTAMP
	GstAcmAacEnc *me = GST_ACMAACENC (enc);

	GST_INFO_OBJECT (me, "size:%d", gst_buffer_get_size(*buffer));
	GST_INFO_OBJECT (me, "DTS:%" GST_TIME_FORMAT,
		GST_TIME_ARGS( GST_BUFFER_DTS (*buffer) ));
	GST_INFO_OBJECT (me, "PTS:%" GST_TIME_FORMAT,
		GST_TIME_ARGS( GST_BUFFER_PTS (*buffer) ));
	GST_INFO_OBJECT (me, "duration:%" GST_TIME_FORMAT,
		GST_TIME_ARGS( GST_BUFFER_DURATION (*buffer) ));
#endif

	return GST_FLOW_OK;
}

static void
gst_acm_aac_enc_flush (GstAudioEncoder * enc)
{
	GstAcmAacEnc *me = GST_ACMAACENC (enc);
	GST_INFO_OBJECT (me, "AACENC FLUSH");

	/* do nothing	*/
}

static gboolean
gst_acm_aac_enc_sink_event (GstAudioEncoder * enc, GstEvent *event)
{
	GstAcmAacEnc *me = GST_ACMAACENC (enc);
	gboolean ret = FALSE;
	GstFlowReturn flowRet = GST_FLOW_OK;

	GST_DEBUG_OBJECT (me, "RECEIVED EVENT (%d)", GST_EVENT_TYPE(event));
	switch (GST_EVENT_TYPE(event)) {
	case GST_EVENT_CAPS:
	{
		GstCaps * caps = NULL;

		gst_event_parse_caps (event, &caps);
		GST_INFO_OBJECT (me, "AACENC received GST_EVENT_CAPS: %" GST_PTR_FORMAT,
						 caps);

		ret = GST_AUDIO_ENCODER_CLASS (parent_class)->sink_event(enc, event);
		break;
	}
	case GST_EVENT_EOS:
	{
		gint i;
		GstBuffer *dummyBuf = NULL;

		GST_INFO_OBJECT (me, "AACENC received GST_EVENT_EOS");

		/* 終了時、サイズ0のバッファをプレエンコード分（2回）qbufする		*/
		dummyBuf = gst_buffer_new_allocate(NULL, 0, NULL);
		for (i = 0; i < me->priv->pre_encode_num; i++) {
			flowRet = gst_acm_aac_enc_handle_out_frame_with_wait(me);
			if (GST_FLOW_OK != flowRet) {
				goto handle_out_failed;
			}
			
			flowRet = gst_acm_aac_enc_handle_in_frame_with_wait(me, dummyBuf);
			if (GST_FLOW_OK != flowRet) {
				goto handle_in_failed;
			}
		}
		gst_buffer_unref(dummyBuf);
		dummyBuf = NULL;

		/* デバイス側に溜まっているデータを取り出して、down stream へ流す	*/
		GST_INFO_OBJECT(me, "in_out_frame_count : %d",
						me->priv->in_out_frame_count);
		while (me->priv->in_out_frame_count > 0) {
			flowRet = gst_acm_aac_enc_handle_out_frame_with_wait(me);
			if (GST_FLOW_OK != flowRet) {
				goto handle_out_failed;
			}
		}
		GST_INFO_OBJECT(me, "remaining processing done. in_out_frame_count : %d",
						me->priv->in_out_frame_count);

		ret = GST_AUDIO_ENCODER_CLASS (parent_class)->sink_event(enc, event);
		break;
	}
	case GST_EVENT_STREAM_START:
		GST_DEBUG_OBJECT (me, "received GST_EVENT_STREAM_START");
		/* break;	*/
	case GST_EVENT_SEGMENT:
		GST_DEBUG_OBJECT (me, "received GST_EVENT_SEGMENT");
		/* break;	*/
	default:
		ret = GST_AUDIO_ENCODER_CLASS (parent_class)->sink_event(enc, event);
		break;
	}
	
out:
	return ret;

handle_out_failed:
	{
		if (GST_FLOW_NOT_LINKED == flowRet || GST_FLOW_FLUSHING == flowRet) {
			GST_WARNING_OBJECT (me, "failed handle out - not link or flushing");
			goto out;
		}
		
		GST_ELEMENT_ERROR (me, STREAM, ENCODE, (NULL),
			("failed handle out"));
		ret = FALSE;
		goto out;
	}
handle_in_failed:
	{
		GST_ELEMENT_ERROR (me, STREAM, ENCODE, (NULL),
			("failed handle in"));
		ret = FALSE;
		goto out;
	}
}

static gboolean
gst_acm_aac_enc_init_encoder (GstAcmAacEnc * me)
{
	gboolean ret = TRUE;
	enum v4l2_buf_type type;
	int r;
	GstCaps *sinkCaps, *srcCaps;
	GstV4l2InitParam v4l2InitParam;
	struct v4l2_format fmt;
	struct v4l2_control ctrl;
	guint in_buf_size, out_buf_size;

	GST_DEBUG_OBJECT (me, "AACENC INITIALIZE ACM ENCODER...");

	/* buffer size : 1024 sample x 16bit x channel	*/
	in_buf_size = SAMPLES_PER_FRAME * 2 * me->channels;
	out_buf_size = in_buf_size;

	/* setup encode parameter		*/
	GST_INFO_OBJECT (me,
		"AACENC INIT PARAM : channels=%d (%d ch), "
		"sample_rate=%d (%d Hz), bit_rate=%d, output_format=%d, enable_cbr=%d",
		pcm_channel_mode(me->channels), me->channels,
		pcm_sample_rate_idx(me->sample_rate), me->sample_rate,
		me->output_bit_rate, me->output_format, me->enable_cbr);

	/* output_format  */
	memset(&fmt, 0, sizeof(struct v4l2_format));
	fmt.type = V4L2_BUF_TYPE_PRIVATE;
	fmt.fmt.raw_data[0] = me->output_format;
	r = v4l2_ioctl(me->video_fd, VIDIOC_S_FMT, &fmt);
	if (r < 0) {
		GST_ERROR_OBJECT(me, "failed ioctl - VIDIOC_S_FMT");
		goto set_init_param_failed;
	}
	/* channel_mode */
	ctrl.id = V4L2_CID_CHANNEL_MODE;
	ctrl.value = pcm_channel_mode(me->channels);
	r = v4l2_ioctl(me->video_fd, VIDIOC_S_CTRL, &ctrl);
	if (r < 0) {
		GST_ERROR_OBJECT(me, "failed ioctl - V4L2_CID_CHANNEL_MODE");
		goto set_init_param_failed;
	}
	/* sample_rate */
	ctrl.id = V4L2_CID_SAMPLE_RATE;
	ctrl.value = pcm_sample_rate_idx(me->sample_rate);
	r = v4l2_ioctl(me->video_fd, VIDIOC_S_CTRL, &ctrl);
	if (r < 0) {
		GST_ERROR_OBJECT(me, "failed ioctl - V4L2_CID_SAMPLE_RATE");
		goto set_init_param_failed;
	}
	/* bit_rate */
	ctrl.id = V4L2_CID_BIT_RATE;
	ctrl.value = me->output_bit_rate;
	r = v4l2_ioctl(me->video_fd, VIDIOC_S_CTRL, &ctrl);
	if (r < 0) {
		GST_ERROR_OBJECT(me, "failed ioctl - V4L2_CID_BIT_RATE");
		goto set_init_param_failed;
	}
	/* enable_cbr */
	ctrl.id = V4L2_CID_ENABLE_CBR;
	ctrl.value = me->enable_cbr;
	r = v4l2_ioctl(me->video_fd, VIDIOC_S_CTRL, &ctrl);
	if (r < 0) {
		goto set_init_param_failed;
	}

	/* setup buffer pool	*/
	if (NULL == me->pool_in) {
		memset(&v4l2InitParam, 0, sizeof(GstV4l2InitParam));
		v4l2InitParam.video_fd = me->video_fd;
		v4l2InitParam.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
		v4l2InitParam.mode = GST_V4L2_IO_MMAP;
		v4l2InitParam.sizeimage = in_buf_size;
		v4l2InitParam.init_num_buffers = DEFAULT_NUM_BUFFERS_IN;
		sinkCaps = gst_caps_from_string ("audio/x-raw");
		me->pool_in = gst_v4l2_buffer_pool_new(&v4l2InitParam, sinkCaps);
		gst_caps_unref(sinkCaps);
		if (! me->pool_in) {
			goto buffer_pool_new_failed;
		}
	}
	
	if (NULL == me->pool_out) {
		memset(&v4l2InitParam, 0, sizeof(GstV4l2InitParam));
		v4l2InitParam.video_fd = me->video_fd;
		v4l2InitParam.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		v4l2InitParam.mode = GST_V4L2_IO_MMAP;
		v4l2InitParam.sizeimage = out_buf_size;
		v4l2InitParam.init_num_buffers = DEFAULT_NUM_BUFFERS_OUT;
		srcCaps = gst_caps_from_string ("audio/mpeg");
		me->pool_out = gst_v4l2_buffer_pool_new(&v4l2InitParam, srcCaps);
		gst_caps_unref(srcCaps);
		if (! me->pool_out) {
			goto buffer_pool_new_failed;
		}
	}

	/* and activate */
	gst_buffer_pool_set_active (GST_BUFFER_POOL_CAST(me->pool_in), TRUE);
	gst_buffer_pool_set_active (GST_BUFFER_POOL_CAST(me->pool_out), TRUE);
	
	GST_DEBUG_OBJECT (me, "pool_in - buffers:%d, allocated:%d, queued:%d",
					  me->pool_in->num_buffers,
					  me->pool_in->num_allocated,
					  me->pool_in->num_queued);
	
	GST_DEBUG_OBJECT (me, "pool_out - buffers:%d, allocated:%d, queued:%d",
					  me->pool_out->num_buffers,
					  me->pool_out->num_allocated,
					  me->pool_out->num_queued);
	
	/* STREAMON */
	GST_INFO_OBJECT (me, "AACENC STREAMON");
	type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	r = v4l2_ioctl(me->video_fd, VIDIOC_STREAMON, &type);
	if (r < 0) {
        goto start_failed;
	}
	GST_DEBUG_OBJECT(me, "STREAMON CAPTURE - ret:%d", r);

	type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	r = v4l2_ioctl(me->video_fd, VIDIOC_STREAMON, &type);
	if (r < 0) {
        goto start_failed;
	}
	GST_DEBUG_OBJECT(me, "STREAMON OUTPUT - ret:%d", r);

	me->priv->is_inited_encoder = TRUE;

out:
	return ret;
	
	/* ERRORS */
set_init_param_failed:
	{
		GST_ELEMENT_ERROR (me, STREAM, ENCODE, (NULL),
			("Failed to set encoder init param.(%s)", g_strerror (errno)));
		ret = FALSE;
		goto out;
	}
buffer_pool_new_failed:
	{
		GST_ELEMENT_ERROR (me, STREAM, ENCODE, (NULL),
			("Could not map buffers from device '%s'", me->videodev));
		ret = FALSE;
		goto out;
	}
start_failed:
	{
		GST_ELEMENT_ERROR (me, STREAM, ENCODE, (NULL),
			("error with STREAMON %d (%s)", errno, g_strerror (errno)));
		ret = FALSE;
		goto out;
	}
}

static gboolean
gst_acm_aac_enc_cleanup_encoder (GstAcmAacEnc * me)
{
	gboolean ret = TRUE;
	enum v4l2_buf_type type;
	int r;
	
	GST_INFO_OBJECT (me, "AACENC CLEANUP ACM ENCODER...");
	
	/* バッファプールのクリーンアップ	*/
	if (me->pool_in) {
		GST_DEBUG_OBJECT (me, "deactivating pool_in");
		GST_INFO_OBJECT (me, "pool_in - buffers:%d, allocated:%d, queued:%d",
						  me->pool_in->num_buffers,
						  me->pool_in->num_allocated,
						  me->pool_in->num_queued);

		gst_buffer_pool_set_active (GST_BUFFER_POOL_CAST (me->pool_in), FALSE);
		gst_object_unref (me->pool_in);
		me->pool_in = NULL;
	}
	
	if (me->pool_out) {
		GST_DEBUG_OBJECT (me, "deactivating pool_out");
		GST_INFO_OBJECT (me, "pool_out - buffers:%d, allocated:%d, queued:%d",
						  me->pool_out->num_buffers,
						  me->pool_out->num_allocated,
						  me->pool_out->num_queued);

		gst_buffer_pool_set_active (GST_BUFFER_POOL_CAST (me->pool_out), FALSE);
		gst_object_unref (me->pool_out);
		me->pool_out = NULL;
	}
	
	/* STREAMOFF */
	if (me->priv->is_inited_encoder) {
		GST_INFO_OBJECT (me, "AACENC STREAMOFF");
		type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
		r = v4l2_ioctl (me->video_fd, VIDIOC_STREAMOFF, &type);
		if (r < 0) {
			goto stop_failed;
		}
		GST_DEBUG_OBJECT(me, "STREAMOFF OUTPUT - ret:%d", r);

		type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		r = v4l2_ioctl (me->video_fd, VIDIOC_STREAMOFF, &type);
		if (r < 0) {
			goto stop_failed;
		}
		GST_DEBUG_OBJECT(me, "STREAMOFF CAPTURE - ret:%d", r);
	}

	me->priv->is_inited_encoder = FALSE;

out:
	return ret;
	
	/* ERRORS */
stop_failed:
	{
		GST_ELEMENT_ERROR (me, STREAM, ENCODE, (NULL),
			("error with STREAMOFF %d (%s)", errno, g_strerror (errno)));
		ret = FALSE;
		goto out;
	}
}

static GstFlowReturn
gst_acm_aac_enc_handle_in_frame_with_wait(GstAcmAacEnc * me, GstBuffer *inbuf)
{
	GstFlowReturn flowRet = GST_FLOW_OK;
	GstBuffer *v4l2buf_in = NULL;
	fd_set write_fds;
	struct timeval tv;
	int r = 0;
#if DBG_MEASURE_PERF_SELECT_IN
	double time_start, time_end;
#endif

	GST_DEBUG_OBJECT(me, "dqbuf (not acquire_buffer)");
	flowRet = gst_v4l2_buffer_pool_dqbuf(me->pool_in, &v4l2buf_in);
	if (GST_FLOW_DQBUF_EAGAIN == flowRet) {
#if DBG_LOG_PERF_SELECT_IN
		GST_INFO_OBJECT(me, "wait until enable dqbuf (pool_in)");
		gst_v4l2_buffer_pool_log_buf_status(me->pool_out);
#endif
		/* 書き込みができる状態になるまで待ってから書き込む		*/
		do {
			FD_ZERO(&write_fds);
			FD_SET(me->video_fd, &write_fds);
			/* no timeout	*/
			tv.tv_sec = 30;
			tv.tv_usec = 0;
#if DBG_MEASURE_PERF_SELECT_IN
			time_start = gettimeofday_sec();
#endif
			r = select(me->video_fd + 1, NULL, &write_fds, NULL, &tv);
#if DBG_MEASURE_PERF_SELECT_IN
			time_end = gettimeofday_sec();
			GST_INFO_OBJECT(me, " select() in %10.10f", time_end - time_start);
			g_time_total_select_in += (time_end - time_start);
			GST_INFO_OBJECT(me, " total %10.10f", g_time_total_select_in);
#endif
		} while (r < 0 && (EINTR == errno || EAGAIN == errno));
		if (r > 0) {
			flowRet = gst_v4l2_buffer_pool_dqbuf(me->pool_in, &v4l2buf_in);
			if (GST_FLOW_OK != flowRet) {
				GST_ERROR_OBJECT (me, "gst_v4l2_buffer_pool_dqbuf() returns %s",
								  gst_flow_get_name (flowRet));
				goto dqbuf_failed;
			}
		}
		else if (r < 0) {
			goto select_failed;
		}
		else if (0 == r) {
			GST_ERROR_OBJECT(me, "select() for input is timeout");
			goto select_timeout;
		}
	}
	else if (GST_FLOW_OK != flowRet) {
		GST_ERROR_OBJECT (me, "gst_v4l2_buffer_pool_dqbuf() returns %s",
						  gst_flow_get_name (flowRet));
		
		goto dqbuf_failed;
	}

	flowRet = gst_acm_aac_enc_handle_in_frame(me, v4l2buf_in, inbuf);
	if (GST_FLOW_OK != flowRet) {
		goto handle_in_failed;
	}

	if (gst_buffer_get_size(inbuf) > 0) {
		/* サイズ 0 のバッファはダミーなのでカウントしない	*/
		me->priv->in_out_frame_count++;
	}

out:
	return flowRet;

	/* ERRORS */
select_failed:
	{
		GST_ELEMENT_ERROR (me, STREAM, ENCODE, (NULL),
			("error with select() %d (%s)", errno, g_strerror (errno)));
		flowRet = GST_FLOW_ERROR;
		goto out;
	}
select_timeout:
	{
		GST_ERROR_OBJECT (me, "pool_in - buffers:%d, queued:%d",
						  me->pool_in->num_buffers, me->pool_in->num_queued);
		
		GST_ERROR_OBJECT (me, "pool_out - buffers:%d, queued:%d",
						  me->pool_out->num_buffers, me->pool_out->num_queued);
		GST_ELEMENT_ERROR (me, STREAM, ENCODE, (NULL),
			("timeout with select()"));
		flowRet = GST_FLOW_ERROR;
		goto out;
	}
dqbuf_failed:
	{
		GST_ELEMENT_ERROR (me, STREAM, ENCODE, (NULL),
			("could not dequeue buffer. %d (%s)", errno, g_strerror (errno)));
		flowRet = GST_FLOW_ERROR;
		goto out;
	}
handle_in_failed:
	{
		GST_ELEMENT_ERROR (me, STREAM, ENCODE, (NULL),
			("failed handle in"));
		flowRet = GST_FLOW_ERROR;
		goto out;
	}
}

static GstFlowReturn
gst_acm_aac_enc_handle_in_frame(GstAcmAacEnc * me,
	GstBuffer *v4l2buf_in, GstBuffer *inbuf)
{
	GstFlowReturn flowRet = GST_FLOW_OK;
	GstMapInfo map;
	gsize inputDataSize = 0;
	
	GST_DEBUG_OBJECT(me, "inbuf size=%d", gst_buffer_get_size(inbuf));

	/* 入力データをコピー	*/
	gst_buffer_map(inbuf, &map, GST_MAP_READ);
	inputDataSize = map.size;
	if (0 == inputDataSize) {
		gst_buffer_resize(v4l2buf_in, 0, 0);
	}
	else {
		gst_buffer_fill(v4l2buf_in, 0, map.data, map.size);
	}
	gst_buffer_unmap(inbuf, &map);
	GST_DEBUG_OBJECT(me, "v4l2buf_in size:%d, input_size:%d",
					 gst_buffer_get_size(v4l2buf_in), inputDataSize);

	/* enqueue buffer	*/
	flowRet = gst_v4l2_buffer_pool_qbuf (me->pool_in, v4l2buf_in, inputDataSize);
	if (GST_FLOW_OK != flowRet) {
		GST_ERROR_OBJECT (me, "gst_v4l2_buffer_pool_qbuf() returns %s",
						 gst_flow_get_name (flowRet));
		goto qbuf_failed;
	}

out:
	return flowRet;
	
	/* ERRORS */
qbuf_failed:
	{
		GST_ELEMENT_ERROR (me, STREAM, ENCODE, (NULL),
			("could not queue buffer. %d (%s)", errno, g_strerror (errno)));
		flowRet = GST_FLOW_ERROR;
		goto out;
	}
}

static GstFlowReturn
gst_acm_aac_enc_handle_out_frame_with_wait(GstAcmAacEnc * me)
{
	GstFlowReturn flowRet = GST_FLOW_OK;
	GstBuffer *v4l2buf_out = NULL;
	fd_set read_fds;
	struct timeval tv;
	int r = 0;
#if DBG_MEASURE_PERF_SELECT_OUT
	double time_start, time_end;
#endif

	/* dequeue buffer	*/
	flowRet = gst_v4l2_buffer_pool_dqbuf (me->pool_out, &v4l2buf_out);
	if (GST_FLOW_DQBUF_EAGAIN == flowRet) {
		/* 読み込みできる状態になるまで待ってから読み込む		*/
#if DBG_LOG_PERF_SELECT_OUT
		GST_INFO_OBJECT(me, "wait until enable dqbuf (pool_out)");
		gst_v4l2_buffer_pool_log_buf_status(me->pool_out);
#endif
		do {
			FD_ZERO(&read_fds);
			FD_SET(me->video_fd, &read_fds);
			tv.tv_sec = 0;
			tv.tv_usec = SELECT_TIMEOUT_MSEC * 1000;
#if DBG_MEASURE_PERF_SELECT_OUT
			time_start = gettimeofday_sec();
#endif
			r = select(me->video_fd + 1, &read_fds, NULL, NULL, &tv);
#if DBG_MEASURE_PERF_SELECT_OUT
			time_end = gettimeofday_sec();
			GST_INFO_OBJECT(me, " select() out : %10.10f", time_end - time_start);
			g_time_total_select_out += (time_end - time_start);
			GST_INFO_OBJECT(me, " total %10.10f", g_time_total_select_out);
#endif
		} while (r < 0 && (EINTR == errno || EAGAIN == errno));
		if (r > 0) {
			flowRet = gst_v4l2_buffer_pool_dqbuf(me->pool_out, &v4l2buf_out);
			if (GST_FLOW_OK != flowRet) {
				GST_ERROR_OBJECT (me, "gst_v4l2_buffer_pool_dqbuf() returns %s",
								  gst_flow_get_name (flowRet));
				goto dqbuf_failed;
			}
		}
		else if (r < 0) {
			goto select_failed;
		}
		else if (0 == r) {
			/* timeoutしたらエラー	*/
			GST_ERROR_OBJECT(me, "select() for output is timeout");
			goto select_timeout;
		}
	}
	else if (GST_FLOW_OK != flowRet) {
		GST_ERROR_OBJECT (me, "gst_v4l2_buffer_pool_dqbuf() returns %s",
						  gst_flow_get_name (flowRet));
		goto dqbuf_failed;
	}

	flowRet = gst_acm_aac_enc_handle_out_frame(me, v4l2buf_out);
	if (GST_FLOW_OK != flowRet) {
		goto handle_out_failed;
	}

	me->priv->in_out_frame_count--;

out:
	return flowRet;

	/* ERRORS */
select_failed:
	{
		GST_ELEMENT_ERROR (me, STREAM, ENCODE, (NULL),
			("error with select() %d (%s)", errno, g_strerror (errno)));
		flowRet = GST_FLOW_ERROR;
		goto out;
	}
select_timeout:
	{
		GST_ERROR_OBJECT (me, "pool_in - buffers:%d, queued:%d",
						  me->pool_in->num_buffers, me->pool_in->num_queued);
		
		GST_ERROR_OBJECT (me, "pool_out - buffers:%d, queued:%d",
						  me->pool_out->num_buffers, me->pool_out->num_queued);
		GST_ELEMENT_ERROR (me, STREAM, ENCODE, (NULL),
			("timeout with select()"));
		flowRet = GST_FLOW_ERROR;
		goto out;
	}
dqbuf_failed:
	{
		GST_ELEMENT_ERROR (me, STREAM, ENCODE, (NULL),
			("could not dequeue buffer. %d (%s)", errno, g_strerror (errno)));
		flowRet = GST_FLOW_ERROR;
		goto out;
	}
handle_out_failed:
	{
		if (GST_FLOW_NOT_LINKED == flowRet || GST_FLOW_FLUSHING == flowRet) {
			GST_WARNING_OBJECT (me, "failed handle out - not link or flushing");
			goto out;
		}
		
		GST_ELEMENT_ERROR (me, STREAM, ENCODE, (NULL),
			("failed handle out"));
		goto out;
	}
}

static GstFlowReturn
gst_acm_aac_enc_handle_out_frame(GstAcmAacEnc * me, GstBuffer *v4l2buf_out)
{
	GstFlowReturn flowRet = GST_FLOW_OK;
	GstBuffer *outbuf = NULL;
#if DO_PUSH_POOLS_BUF
	GstBufferPoolAcquireParams acquireParam;
#endif
#if DBG_MEASURE_PERF_FINISH_FRAME
	static double interval_time_start = 0, interval_time_end = 0;
	double time_start = 0, time_end = 0;
#endif

	GST_DEBUG_OBJECT(me, "AACENC HANDLE OUT FRAME : %p", v4l2buf_out);
	GST_DEBUG_OBJECT(me, "v4l2buf_out size=%d, ref:%d",
					 gst_buffer_get_size(v4l2buf_out),
					 GST_OBJECT_REFCOUNT_VALUE(v4l2buf_out));
	GST_DEBUG_OBJECT(me, "pool_out->num_queued : %d", me->pool_out->num_queued);

#if DBG_DUMP_OUT_BUF	/* for debug	*/
	dump_output_buf(v4l2buf_out);
#endif

#if DO_PUSH_POOLS_BUF

	/* gst_buffer_unref() により、デバイスに、QBUF されるようにする。
	 * output_buffer->pool に、me->pool_out を直接セットせず、
	 * GstBufferPool::priv::outstanding をインクリメントするために、
	 * gst_buffer_pool_acquire_buffer() を call する。
	 * そうしないと、pool の deactivate の際、outstanding == 0 でないと、
	 * buffer の解放が行われないため
	 */
	acquireParam.flags = GST_BUFFER_POOL_ACQUIRE_FLAG_LAST;
	flowRet = gst_buffer_pool_acquire_buffer(GST_BUFFER_POOL_CAST(me->pool_out),
										 &v4l2buf_out, &acquireParam);
	if (GST_FLOW_OK != flowRet) {
		GST_ERROR_OBJECT (me, "gst_buffer_pool_acquire_buffer() returns %s",
						  gst_flow_get_name (flowRet));
		goto acquire_buffer_failed;
	}

	GST_DEBUG_OBJECT(me, "outbuf->pool : %p (ref:%d), pool_out : %p (ref:%d)",
					 v4l2buf_out->pool, GST_OBJECT_REFCOUNT_VALUE(v4l2buf_out->pool),
					 me->pool_out, GST_OBJECT_REFCOUNT_VALUE(me->pool_out));

	/* down stream へ push するバッファの生成	*/
	outbuf = v4l2buf_out;

	/* down stream へ バッファを push	*/
#if DBG_LOG_PERF_PUSH
	GST_INFO_OBJECT (me, "AACENC-PUSH finish_frame START");
#endif
#if DBG_MEASURE_PERF_FINISH_FRAME
	interval_time_end = gettimeofday_sec();
	if (interval_time_start > 0) {
//		if ((interval_time_end - interval_time_start) > 0.022) {
			GST_INFO_OBJECT(me, "finish_frame() at(ms) : %10.10f",
							(interval_time_end - interval_time_start)*1e+3);
//		}
	}
	interval_time_start = gettimeofday_sec();
	time_start = gettimeofday_sec();
#endif
	flowRet = gst_audio_encoder_finish_frame (GST_AUDIO_ENCODER(me),
				outbuf, SAMPLES_PER_FRAME);
#if DBG_MEASURE_PERF_FINISH_FRAME
	time_end = gettimeofday_sec();
	GST_INFO_OBJECT(me, "finish_frame()   (ms): %10.10f",
						(time_end - time_start)*1e+3);
#endif
#if DBG_LOG_PERF_PUSH
	GST_INFO_OBJECT (me, "AACENC-PUSH finish_frame END");
#endif
	if (GST_FLOW_OK != flowRet) {
		GST_WARNING_OBJECT (me, "gst_audio_encoder_finish_frame() returns %s",
							gst_flow_get_name (flowRet));
		goto finish_frame_failed;
	}

#else	/* DO_PUSH_POOLS_BUF */
	
	/* 出力バッファをアロケート	*/
	outbuf = gst_buffer_copy(v4l2buf_out);
	if (NULL == outbuf) {
		goto allocate_outbuf_failed;
	}

	/* enqueue buffer	*/
	flowRet = gst_v4l2_buffer_pool_qbuf (
			me->pool_out, v4l2buf_out, gst_buffer_get_size(v4l2buf_out));
	if (GST_FLOW_OK != flowRet) {
		GST_ERROR_OBJECT (me, "gst_v4l2_buffer_pool_qbuf() returns %s",
						  gst_flow_get_name (flowRet));
		goto qbuf_failed;
	}
	
	/* down stream へ バッファを push	*/
#if DBG_LOG_PERF_PUSH
	GST_INFO_OBJECT (me, "AACENC-PUSH finish_frame START");
#endif
	flowRet = gst_audio_encoder_finish_frame (GST_AUDIO_ENCODER(me),
				outbuf, SAMPLES_PER_FRAME);
#if DBG_LOG_PERF_PUSH
	GST_INFO_OBJECT (me, "AACENC-PUSH finish_frame END");
#endif
	if (GST_FLOW_OK != flowRet) {
		GST_ERROR_OBJECT (me, "gst_audio_encoder_finish_frame() returns %s",
						  gst_flow_get_name (flowRet));
		goto finish_frame_failed;
	}

#endif	/* DO_PUSH_POOLS_BUF */

out:
	return flowRet;
	
	/* ERRORS */
#if ! DO_PUSH_POOLS_BUF
qbuf_failed:
	{
		GST_ELEMENT_ERROR (me, STREAM, ENCODE, (NULL),
			("could not queue buffer. %d (%s)", errno, g_strerror (errno)));
		flowRet = GST_FLOW_ERROR;
		goto out;
	}
#endif
finish_frame_failed:
	{
		if (GST_FLOW_NOT_LINKED == flowRet || GST_FLOW_FLUSHING == flowRet) {
			GST_WARNING_OBJECT (me,
				"failed gst_audio_encoder_finish_frame() - not link or flushing");
			
//			flowRet = GST_FLOW_ERROR;
			goto out;
		}

		GST_ELEMENT_ERROR (me, STREAM, ENCODE, (NULL),
			("failed gst_audio_encoder_finish_frame()"));
//		flowRet = GST_FLOW_ERROR;
		goto out;
	}
#if DO_PUSH_POOLS_BUF
acquire_buffer_failed:
	{
		GST_ELEMENT_ERROR (me, STREAM, ENCODE, (NULL),
			("failed gst_buffer_pool_acquire_buffer()"));
		flowRet = GST_FLOW_ERROR;
		goto out;
	}
#else
allocate_outbuf_failed:
	{
		GST_ELEMENT_ERROR (me, RESOURCE, FAILED, (NULL),
						   ("failed allocate outbuf"));
		flowRet = GST_FLOW_ERROR;
		goto out;
	}
#endif
}

static gboolean
plugin_init (GstPlugin * plugin)
{
	GST_DEBUG_CATEGORY_INIT (acmaacenc_debug, "acmaacenc", 0, "AAC encoding");

	return gst_element_register (plugin, "acmaacenc",
							   GST_RANK_PRIMARY, GST_TYPE_ACMAACENC);
}

GST_PLUGIN_DEFINE (
	GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    acmaacenc,
    "ACM AAC Encoder",
    plugin_init,
	VERSION,
	"LGPL",
	"GStreamer ACM Plugins",
	"http://armadillo.atmark-techno.com/"
);

/*
 * End of file
 */
