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

/**
 * SECTION:element-acmh264enc
 *
 * This element encodes raw video into H264 compressed data,
 * also otherwise known as MPEG-4 AVC (Advanced Video Codec).
 *
 * <refsect2>
 * <title>Example pipeline</title>
 * |[
 * gst-launch -v videotestsrc num-buffers=1000 ! acmh264enc ! \
 *   avimux ! filesink location=videotestsrc.avi
 * ]| This example pipeline will encode a test video source to H264 muxed in an
 * AVI container.
 * |[
 * gst-launch -v videotestsrc num-buffers=1000 ! tee name=t ! queue ! xvimagesink \
 *   t. ! queue ! acmh264enc ! fakesink
 * ]| This example pipeline will encode a test video source to H264 while
 * displaying the input material at the same time.
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <inttypes.h>
#include <errno.h>
#include <gst/video/gstvideometa.h>
#include <gst/video/gstvideopool.h>
#include <gst/pbutils/pbutils.h>
#include <media/acm-h264enc.h>

#include "gstacmh264enc.h"
#include "gstacmv4l2_util.h"
#include "gstacmdmabufmeta.h"
#include "gstacm_util.h"
#include "gstacm_debug.h"


/* ストライドは、入力の画像幅とイコールになるので、明示的に指定する必要はない */
#define USE_STRIDE_PROP					0

/* エンコーダv4l2デバイスのドライバ名 */
#define DRIVER_NAME						"acm-h264enc"

/* H264_ENCODER_RESULT で返される、ピクチャ種別 */
enum {
	PICTURE_TYPE_I	= 0,
	PICTURE_TYPE_P	= 1,
	PICTURE_TYPE_B	= 2,
};

/* AUD NAL ユニットサイズ	: startcode 4, header 1, payload 1	*/
#define AUD_NAL_SIZE					(4 + 2)

/* デバイスに確保するバッファ数	*/
#define DEFAULT_NUM_BUFFERS_IN			3
#define DEFAULT_NUM_BUFFERS_OUT			3

/* エンコーダー初期化パラメータのデフォルト値	*/
#define DEFAULT_VIDEO_DEVICE			"/dev/video0"
#if USE_STRIDE_PROP
#define DEFAULT_STRIDE					0
#endif
#define DEFAULT_BITRATE					8000000		/* default 8Mbps */
#define DEFAULT_MAX_FRAME_SIZE			0
#define DEFAULT_RATE_CONTROL_MODE		GST_ACMH264ENC_RATE_CONTROL_MODE_VBR_NON_SKIP
#define DEFAULT_FRAME_RATE_RESOLUTION	24000
#define DEFAULT_FRAME_RATE_TICK			800
#define DEFAULT_MAX_GOP_LENGTH			30	/* 推奨値		*/
#define DEFAULT_B_PIC_MODE				GST_ACMH264ENC_B_PIC_MODE_3_B_PIC
#define DEFAULT_X_OFFSET				0
#define DEFAULT_Y_OFFSET				0

/* select() の timeout 時間 */
#define SELECT_TIMEOUT_MSEC				1000

/* デバッグログ出力フラグ		*/
#define DBG_LOG_PERF_CHAIN				0
#define DBG_LOG_PERF_SELECT_IN			0
#define DBG_LOG_PERF_PUSH				0
#define DBG_LOG_PERF_SELECT_OUT			0
#define DBG_LOG_OUT_TIMESTAMP			0
#define DBG_LOG_IN_FRAME_LIST			0

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
struct _GstAcmH264EncPrivate
{
	/* HW エンコーダの初期化済みフラグ	*/
	gboolean is_inited_encoder;

	/* QBUF(V4L2_BUF_TYPE_VIDEO_OUTPUT) 用カウンタ	*/
	gint num_inbuf_acquired;
	gint num_inbuf_queued;

	/* DQBUF(V4L2_BUF_TYPE_VIDEO_CAPTURE) 用カウンタ	*/
	gint num_outbuf_dequeued;

	/* V4L2_BUF_TYPE_VIDEO_OUTPUT 側に入力したフレーム数と、
	 * V4L2_BUF_TYPE_VIDEO_CAPTURE 側から取り出したフレーム数の差分
	 */
	gint in_out_frame_count;
	
	/* プレエンコード回数	*/
	gint pre_encode_num;

	/* AUD + SPS + PPS */
	GstBuffer *spspps_buf;
	gint sps_len;
	gint pps_len;
	gsize spspps_size;

	/* 初回のフレームの送出済みフラグ */
	gboolean is_handled_1stframe_out;

	/* Bピクチャ無し時のプレエンコード用空バッファをQBUF済みフラグ	*/
	gboolean is_qbufed_null_when_non_bpic;

	/* V4L2_PIX_FMT_H264 or V4L2_PIX_FMT_H264_NO_SC 	*/
	gint output_format;

	/* list of incoming GstVideoCodecFrame	*/
	GList *in_frames;
};

GST_DEBUG_CATEGORY_STATIC (acmh264enc_debug);
#define GST_CAT_DEFAULT (acmh264enc_debug)
GST_DEBUG_CATEGORY_EXTERN (GST_CAT_PERFORMANCE);

#define GST_ACMH264ENC_GET_PRIVATE(obj)  \
	(G_TYPE_INSTANCE_GET_PRIVATE ((obj), GST_TYPE_ACMH264ENC, \
		GstAcmH264EncPrivate))

/* property */
enum
{
	PROP_0,
	PROP_DEVICE,
#if USE_STRIDE_PROP
	PROP_STRIDE,
#endif
	PROP_BIT_RATE,
	PROP_MAX_FRAME_SIZE,
	PROP_RATE_CONTROL_MODE,
	PROP_MAX_GOP_LENGTH,
	PROP_B_PIC_MODE,
	PROP_X_OFFSET,
	PROP_Y_OFFSET,
};

/* pad template caps for source and sink pads.	*/
// NV12 == GST_VIDEO_FORMAT_NV12 (YUV420 semi-planar)
static GstStaticPadTemplate sink_template_factory =
GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
	GST_STATIC_CAPS (
		"video/x-raw, "
		"format = (string)  { NV12 }, "
		"framerate = (fraction) [0, MAX], "
		"width = (int) [ 80, 1920 ], "
		"height = (int) [ 80, 1080 ]"
	)
);

static GstStaticPadTemplate src_template_factory =
GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (
		"video/x-h264, "
		"framerate = (fraction) [0/1, MAX], "
		"width = (int) [ 80, 1920 ], "
		"height = (int) [ 80, 1080 ], "
		"stream-format = (string) { avc, byte-stream }, "
		"alignment = (string) { au }, "
		"profile = (string) { baseline, constrained-baseline, main, high }"
	)
);

static gint
get_encoded_picture_type(GstAcmH264Enc * me, GstBuffer* buffer)
{
	GstAcmV4l2Meta *meta;
	gint pictureType;

	/* struct v4l2_buffer の reserved フィールドに、
	 * H264_ENCODER_RESULT の picture_type が返される
	 */
	meta = GST_ACM_V4L2_META_GET (buffer);
	g_assert (meta != NULL);

	pictureType = meta->vbuffer.reserved;

#if 0	/* for debug	*/
	switch (pictureType) {
	case PICTURE_TYPE_I:
		GST_INFO_OBJECT(me, "I PICTURE");
		break;
	case PICTURE_TYPE_P:
		GST_INFO_OBJECT(me, "P PICTURE");
		break;
	case PICTURE_TYPE_B:
		GST_INFO_OBJECT(me, "B PICTURE");
		break;
	default:
		g_assert_not_reached ();
		break;
	}
#endif

	return pictureType;
}

static unsigned long
get_capture_counter(GstAcmH264Enc * me, GstBuffer* buffer)
{
	GstAcmV4l2Meta *meta;

	/* struct v4l2_buffer の sequence フィールドに、
	 * H264_ENCODER_RESULT の キャプチャ順カウンタ が返される
	 */
	meta = GST_ACM_V4L2_META_GET (buffer);
	g_assert (meta != NULL);

#if 0	/* for debug	*/
	GST_INFO_OBJECT(me, "sequence : %lu (0x%08X)",
					meta->vbuffer.sequence, meta->vbuffer.sequence);
#endif

	return meta->vbuffer.sequence;
}

/* GObject base class method */
static void gst_acm_h264_enc_set_property (GObject * object, guint prop_id,
	const GValue * value, GParamSpec * pspec);
static void gst_acm_h264_enc_get_property (GObject * object, guint prop_id,
	GValue * value, GParamSpec * pspec);
static void gst_acm_h264_enc_finalize (GObject * object);
/* GstVideoEncoder base class method */
static gboolean gst_acm_h264_enc_open (GstVideoEncoder * enc);
static gboolean gst_acm_h264_enc_close (GstVideoEncoder * enc);
static gboolean gst_acm_h264_enc_start (GstVideoEncoder * enc);
static gboolean gst_acm_h264_enc_stop (GstVideoEncoder * enc);
static GstCaps *gst_acm_h264_enc_getcaps (GstVideoEncoder * encoder,
	GstCaps * filter);
static gboolean gst_acm_h264_enc_set_format (GstVideoEncoder * enc,
    GstVideoCodecState * state);
static gboolean gst_acm_h264_enc_reset (GstVideoEncoder * enc, gboolean hard);
static GstFlowReturn gst_acm_h264_enc_finish (GstVideoEncoder * enc);
static GstFlowReturn gst_acm_h264_enc_handle_frame (GstVideoEncoder * enc,
    GstVideoCodecFrame * frame);
static GstFlowReturn gst_acm_h264_enc_pre_push (GstVideoEncoder *enc,
	GstVideoCodecFrame * frame);
static gboolean gst_acm_h264_enc_sink_event (GstVideoEncoder * enc,
	GstEvent *event);
/* GstAcmH264Enc class method */
static gboolean gst_acm_h264_enc_init_encoder (GstAcmH264Enc * me);
static gboolean gst_acm_h264_enc_cleanup_encoder (GstAcmH264Enc * me);
static GstFlowReturn gst_acm_h264_enc_qbuf_null_in(GstAcmH264Enc * me);
static GstFlowReturn gst_acm_h264_enc_handle_in_frame_with_wait(GstAcmH264Enc * me,
	GstBuffer *inbuf);
static GstFlowReturn gst_acm_h264_enc_handle_in_frame(GstAcmH264Enc * me,
	GstBuffer *v4l2buf_in, GstBuffer *inbuf);
static GstFlowReturn gst_acm_h264_enc_get_spspps(GstAcmH264Enc * me);
static GstFlowReturn gst_acm_h264_enc_handle_out_frame_with_wait(GstAcmH264Enc * me);
static GstFlowReturn gst_acm_h264_enc_handle_out_frame(GstAcmH264Enc * me,
	GstBuffer *v4l2buf_out);
static GstBuffer* gst_acm_h264_enc_make_codec_data (GstAcmH264Enc * me);
static void gst_acm_h264_enc_push_frame (GstAcmH264Enc * me,
	GstVideoCodecFrame * frame);
static GstVideoCodecFrame *gst_acm_h264_enc_pop_frame (GstAcmH264Enc * me,
	guint32 frame_number);

#define gst_acm_h264_enc_parent_class parent_class
G_DEFINE_TYPE (GstAcmH264Enc, gst_acm_h264_enc, GST_TYPE_VIDEO_ENCODER);


static void
gst_acm_h264_enc_set_property (GObject * object, guint prop_id,
							  const GValue * value, GParamSpec * pspec)
{
	GstAcmH264Enc *me = GST_ACMH264ENC (object);

	switch (prop_id) {
	case PROP_DEVICE:
		if (me->videodev) {
			g_free (me->videodev);
		}
		me->videodev = g_value_dup_string (value);
		break;
#if USE_STRIDE_PROP
	case PROP_STRIDE:
		me->stride = g_value_get_int (value);
		break;
#endif
	case PROP_BIT_RATE:
		me->bit_rate = g_value_get_int (value);
		break;
	case PROP_MAX_FRAME_SIZE:
		me->max_frame_size = g_value_get_int (value);
		break;
	case PROP_RATE_CONTROL_MODE:
		me->rate_control_mode = g_value_get_int (value);
		break;
	case PROP_MAX_GOP_LENGTH:
		me->max_GOP_length = g_value_get_int (value);
		break;
	case PROP_B_PIC_MODE:
		me->B_pic_mode = g_value_get_int (value);
		break;
	case PROP_X_OFFSET:
		me->x_offset = g_value_get_int (value);
		break;
	case PROP_Y_OFFSET:
		me->y_offset = g_value_get_int (value);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
gst_acm_h264_enc_get_property (GObject * object, guint prop_id,
	GValue * value, GParamSpec * pspec)
{
	GstAcmH264Enc *me = GST_ACMH264ENC (object);
	
	switch (prop_id) {
	case PROP_DEVICE:
		g_value_set_string (value, me->videodev);
		break;
#if USE_STRIDE_PROP
	case PROP_STRIDE:
		g_value_set_int (value, me->stride);
		break;
#endif
	case PROP_BIT_RATE:
		g_value_set_int (value, me->bit_rate);
		break;
	case PROP_MAX_FRAME_SIZE:
		g_value_set_int (value, me->max_frame_size);
		break;
	case PROP_RATE_CONTROL_MODE:
		g_value_set_int (value, me->rate_control_mode);
		break;
	case PROP_MAX_GOP_LENGTH:
		g_value_set_int (value, me->max_GOP_length);
		break;
	case PROP_B_PIC_MODE:
		g_value_set_int (value, me->B_pic_mode);
		break;
	case PROP_X_OFFSET:
		g_value_set_int (value, me->x_offset);
		break;
	case PROP_Y_OFFSET:
		g_value_set_int (value, me->y_offset);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
gst_acm_h264_enc_class_init (GstAcmH264EncClass * klass)
{
	GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
	GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
	GstVideoEncoderClass *video_encoder_class = GST_VIDEO_ENCODER_CLASS (klass);

	g_type_class_add_private (klass, sizeof (GstAcmH264EncPrivate));

	gobject_class->set_property = gst_acm_h264_enc_set_property;
	gobject_class->get_property = gst_acm_h264_enc_get_property;
	gobject_class->finalize = gst_acm_h264_enc_finalize;
	
	g_object_class_install_property (gobject_class, PROP_DEVICE,
		g_param_spec_string ("device", "device",
			"The video device eg: /dev/video0. "
			"default device is calculate from driver name.",
			DEFAULT_VIDEO_DEVICE, G_PARAM_READWRITE));

#if USE_STRIDE_PROP
	g_object_class_install_property (gobject_class, PROP_STRIDE,
		g_param_spec_int ("stride", "Stride",
			"Stride of output video. (0 is unspecified)",
			DEFAULT_STRIDE /* GST_ACMH264ENC_STRIDE_MIN */, GST_ACMH264ENC_STRIDE_MAX,
			DEFAULT_STRIDE, G_PARAM_READWRITE));
#endif

	g_object_class_install_property (gobject_class, PROP_BIT_RATE,
		g_param_spec_int ("bitrate", "Bitrate (bps)",
			"Average Bitrate (ABR) in bits/sec. ",
			GST_ACMH264ENC_BITRATE_MIN, GST_ACMH264ENC_BITRATE_MAX,
			DEFAULT_BITRATE, G_PARAM_READWRITE | G_PARAM_LAX_VALIDATION));

	g_object_class_install_property (gobject_class, PROP_MAX_FRAME_SIZE,
		g_param_spec_int ("max-frame-size", "Max Frame Size",
			"Max frame encode size in bytes. "
			"0 is unspecified (calculate automatically).",
			GST_ACMH264ENC_MAXFRAMESIZE_MIN, GST_ACMH264ENC_MAXFRAMESIZE_MAX,
			DEFAULT_MAX_FRAME_SIZE, G_PARAM_READWRITE | G_PARAM_LAX_VALIDATION));

	g_object_class_install_property (gobject_class, PROP_RATE_CONTROL_MODE,
		g_param_spec_int ("rate-control-mode", "Rate control mode",
			"0:CBR (with skip picture), 1:CDR (with non skip picture), 2:VBR. ",
			GST_ACMH264ENC_RATE_CONTROL_MODE_CBR_SKIP, GST_ACMH264ENC_RATE_CONTROL_MODE_VBR_NON_SKIP,
			DEFAULT_RATE_CONTROL_MODE, G_PARAM_READWRITE));

	g_object_class_install_property (gobject_class, PROP_MAX_GOP_LENGTH,
		g_param_spec_int ("max-gop-length", "Max GOP length",
			"Max GOP length.",
			GST_ACMH264ENC_MAX_GOP_LENGTH_MIN, GST_ACMH264ENC_MAX_GOP_LENGTH_MAX,
			DEFAULT_MAX_GOP_LENGTH, G_PARAM_READWRITE | G_PARAM_LAX_VALIDATION));

	g_object_class_install_property (gobject_class, PROP_B_PIC_MODE,
		g_param_spec_int ("b-pic-mode", "B picture mode",
			"0:not use B picture, 1:use 1 frame, 2:use 2 frame, 3:use 3 frame.",
			GST_ACMH264ENC_B_PIC_MODE_0_B_PIC, GST_ACMH264ENC_B_PIC_MODE_3_B_PIC,
			DEFAULT_B_PIC_MODE, G_PARAM_READWRITE));

	g_object_class_install_property (gobject_class, PROP_X_OFFSET,
		g_param_spec_int ("x-offset", "X Offset",
			"X Offset of output video.",
			GST_ACMH264ENC_X_OFFSET_MIN, GST_ACMH264ENC_X_OFFSET_MAX,
			DEFAULT_X_OFFSET, G_PARAM_READWRITE | G_PARAM_LAX_VALIDATION));
	
	g_object_class_install_property (gobject_class, PROP_Y_OFFSET,
		g_param_spec_int ("y-offset", "Y Offset",
			"Y Offset of output video.",
			GST_ACMH264ENC_Y_OFFSET_MIN, GST_ACMH264ENC_Y_OFFSET_MAX,
			DEFAULT_Y_OFFSET, G_PARAM_READWRITE | G_PARAM_LAX_VALIDATION));

	gst_element_class_add_pad_template (element_class,
			gst_static_pad_template_get (&src_template_factory));
	gst_element_class_add_pad_template (element_class,
			gst_static_pad_template_get (&sink_template_factory));

	gst_element_class_set_static_metadata (element_class,
			"ACM H264 video encoder", "Codec/Encoder/Video",
			"ACM H.264/AVC encoder", "atmark techno");

	video_encoder_class->open = GST_DEBUG_FUNCPTR (gst_acm_h264_enc_open);
	video_encoder_class->close = GST_DEBUG_FUNCPTR (gst_acm_h264_enc_close);
	video_encoder_class->start = GST_DEBUG_FUNCPTR (gst_acm_h264_enc_start);
	video_encoder_class->stop = GST_DEBUG_FUNCPTR (gst_acm_h264_enc_stop);
	video_encoder_class->reset = GST_DEBUG_FUNCPTR (gst_acm_h264_enc_reset);
	video_encoder_class->getcaps = GST_DEBUG_FUNCPTR (gst_acm_h264_enc_getcaps);
	video_encoder_class->set_format =
		GST_DEBUG_FUNCPTR (gst_acm_h264_enc_set_format);
	video_encoder_class->handle_frame =
		GST_DEBUG_FUNCPTR (gst_acm_h264_enc_handle_frame);
	video_encoder_class->pre_push = GST_DEBUG_FUNCPTR (gst_acm_h264_enc_pre_push);
	video_encoder_class->finish = GST_DEBUG_FUNCPTR (gst_acm_h264_enc_finish);
	video_encoder_class->sink_event =
		GST_DEBUG_FUNCPTR (gst_acm_h264_enc_sink_event);
}

static void
gst_acm_h264_enc_init (GstAcmH264Enc * me)
{
	me->priv = GST_ACMH264ENC_GET_PRIVATE (me);

	me->input_width = -1;
	me->input_height = -1;
	me->input_format = -1;

	me->input_state = NULL;

	me->video_fd = -1;
	me->pool_in = NULL;
	me->pool_out = NULL;

	me->output_width = -1;
	me->output_height = -1;

	me->priv->is_inited_encoder = FALSE;
	me->priv->num_inbuf_acquired = 0;
	me->priv->num_inbuf_queued = 0;
	me->priv->num_outbuf_dequeued = 0;
	me->priv->in_out_frame_count = 0;
	me->priv->pre_encode_num = 0;
	me->priv->spspps_buf = NULL;
	me->priv->spspps_size = 0;
	me->priv->sps_len = 0;
	me->priv->pps_len = 0;
	me->priv->is_handled_1stframe_out = FALSE;
	me->priv->is_qbufed_null_when_non_bpic = FALSE;
	me->priv->output_format = V4L2_PIX_FMT_H264_NO_SC;
	me->priv->in_frames = NULL;

	/* property	*/
	me->videodev = NULL;
#if USE_STRIDE_PROP
	me->stride = -1;
#endif
	me->bit_rate = DEFAULT_BITRATE;
	me->max_frame_size = DEFAULT_MAX_FRAME_SIZE;
	me->rate_control_mode = DEFAULT_RATE_CONTROL_MODE;
	me->frame_rate_resolution = -1;
	me->frame_rate_tick = -1;
	me->max_GOP_length = DEFAULT_MAX_GOP_LENGTH;
	me->B_pic_mode = DEFAULT_B_PIC_MODE;
	me->x_offset = DEFAULT_X_OFFSET;
	me->y_offset = DEFAULT_Y_OFFSET;
}

static void
gst_acm_h264_enc_finalize (GObject * object)
{
	GstAcmH264Enc *me = GST_ACMH264ENC (object);

	GST_INFO_OBJECT (me, "H264ENC FINALIZE");

	/* プロパティとして保持するため、gst_acm_h264_enc_close() 内で free してはいけない	*/
	if (me->videodev) {
		g_free(me->videodev);
		me->videodev = NULL;
	}

	G_OBJECT_CLASS (parent_class)->finalize (object);
}

static gboolean
gst_acm_h264_enc_open (GstVideoEncoder * enc)
{
	GstAcmH264Enc *me = GST_ACMH264ENC (enc);

	/* プロパティとしてセットされていなければ、デバイスを検索
	 * 他のプロパティは、gst_acm_h264_enc_set_format() で設定
	 */
	if (NULL == me->videodev) {
		me->videodev = gst_acm_v4l2_getdev(DRIVER_NAME);
	}

	GST_INFO_OBJECT (me, "H264ENC OPEN ACM ENCODER. (%s)", me->videodev);
	
	/* open device	*/
	GST_INFO_OBJECT (me, "Trying to open device %s", me->videodev);
	if (! gst_acm_v4l2_open (me->videodev, &(me->video_fd), TRUE)) {
        GST_ELEMENT_ERROR (me, RESOURCE, NOT_FOUND, (NULL),
			("Failed open device %s. (%s)", me->videodev, g_strerror (errno)));
		return FALSE;
	}
	GST_INFO_OBJECT (me, "Opened device '%s' successfully", me->videodev);

	return TRUE;
}

static gboolean
gst_acm_h264_enc_close (GstVideoEncoder * enc)
{
	GstAcmH264Enc *me = GST_ACMH264ENC (enc);

	GST_INFO_OBJECT (me, "H264ENC CLOSE ACM ENCODER. (%s)", me->videodev);
	
	/* close device	*/
	if (me->video_fd > 0) {
		gst_acm_v4l2_close(me->videodev, me->video_fd);
		me->video_fd = -1;
	}

	return TRUE;
}

static gboolean
gst_acm_h264_enc_start (GstVideoEncoder * enc)
{
	GstAcmH264Enc *me = GST_ACMH264ENC (enc);
	
	GST_INFO_OBJECT (me, "H264ENC START");

	/* プロパティ以外の変数を再初期化		*/
	me->input_width = -1;
	me->input_height = -1;
	me->input_format = -1;

	me->input_state = NULL;

	me->pool_in = NULL;
	me->pool_out = NULL;

	me->priv->is_inited_encoder = FALSE;
	me->priv->num_inbuf_acquired = 0;
	me->priv->num_inbuf_queued = 0;
	me->priv->num_outbuf_dequeued = 0;
	me->priv->in_out_frame_count = 0;
	me->priv->pre_encode_num = 0;
	me->priv->spspps_buf = NULL;
	me->priv->spspps_size = 0;
	me->priv->sps_len = 0;
	me->priv->pps_len = 0;
	me->priv->is_handled_1stframe_out = FALSE;
	me->priv->is_qbufed_null_when_non_bpic = FALSE;
	me->priv->output_format = V4L2_PIX_FMT_H264_NO_SC;
	me->priv->in_frames = NULL;

	return TRUE;
}

static gboolean
gst_acm_h264_enc_stop (GstVideoEncoder * enc)
{
	GstAcmH264Enc *me = GST_ACMH264ENC (enc);

	GST_INFO_OBJECT (me, "H264ENC STOP");

	/* cleanup encoder	*/
	gst_acm_h264_enc_cleanup_encoder (me);

	if (me->input_state) {
		gst_video_codec_state_unref (me->input_state);
		me->input_state = NULL;
	}
	if (me->priv->spspps_buf) {
		gst_buffer_unref (me->priv->spspps_buf);
		me->priv->spspps_buf = NULL;
		me->priv->spspps_size = 0;
	}

	/* cleanup frame list	*/
	if (me->priv->in_frames) {
		GST_INFO_OBJECT (me, "LIST length : %u", g_list_length(me->priv->in_frames));
		g_list_foreach (me->priv->in_frames, (GFunc) gst_video_codec_frame_unref, NULL);
		g_list_free (me->priv->in_frames);
		me->priv->in_frames = NULL;
	}
#if DBG_LOG_IN_FRAME_LIST
	else {
		GST_INFO_OBJECT (me, "LIST is NULL");
	}
#endif

	return TRUE;
}

static GstBuffer*
gst_acm_h264_enc_make_codec_data (GstAcmH264Enc * me)
{
	/* ISO/IEC 14496-15 - 5.2.4 Decoder configuration information
	 * class AVCDecoderConfigurationRecord
	 * unsigned int(8) configurationVersion = 1;
	 * unsigned int(8) AVCProfileIndication;
	 * unsigned int(8) profile_compatibility;
	 * unsigned int(8) AVCLevelIndication;
	 * bit(6) reserved = ‘111111’b;
	 * unsigned int(2) lengthSizeMinusOne;
	 * bit(3) reserved = ‘111’b;
	 * unsigned int(5) numOfSequenceParameterSets;
	 * for (i=0; i< numOfSequenceParameterSets; i++) {
	 *   unsigned int(16) sequenceParameterSetLength ;
	 *   bit(8*sequenceParameterSetLength) sequenceParameterSetNALUnit;
	 * }
	 * unsigned int(8) numOfPictureParameterSets;
	 * for (i=0; i< numOfPictureParameterSets; i++) {
	 *   unsigned int(16) pictureParameterSetLength;
	 *   bit(8*pictureParameterSetLength) pictureParameterSetNALUnit;
	 * }
	 */
	GstBuffer *cdataBuf = NULL;
	GstFlowReturn flowRet = GST_FLOW_OK;
	GstMapInfo spsppsMap, cdataMap;
	guint8 *spsStart = NULL, *ppsStart = NULL;
	int spsSize = 0, ppsSize = 0;
	guint8 *cursor = NULL;
	int index;

	/* get SPS/PPS from HW encoder */
	flowRet = gst_acm_h264_enc_get_spspps(me);
	if (GST_FLOW_OK != flowRet) {
		goto failed_get_spspps;
	}

#if 0	// for debug
	dump_output_buf(me->priv->spspps_buf);
#endif

	gst_buffer_map(me->priv->spspps_buf, &spsppsMap, GST_MAP_READ);
	cursor = spsppsMap.data;

	// SPS と PPS のサイズを取得
	//  start code 4byte
	g_assert (0x00 == spsppsMap.data[0] && 0x00 == spsppsMap.data[1]
			  && 0x00 == spsppsMap.data[2] && 0x01 == spsppsMap.data[3]);
	cursor += 4;
	//  AUD 2Byte
	g_assert (0x09 == (cursor[0] & 0x1F));
	cursor += 2;
	//  start code 4byte
	g_assert (0x00 == cursor[0] && 0x00 == cursor[1]
			  && 0x00 == cursor[2] && 0x01 == cursor[3]);
	cursor += 4;
	//  SPS ?byte
	spsStart = cursor;
	g_assert (0x07 == (cursor[0] & 0x1F));
	//  次の start code までのサイズが SPS のサイズ
	for (;;) {
		if (0x00 == cursor[0] && 0x00 == cursor[1]
			&& 0x00 == cursor[2] && 0x01 == cursor[3]) {
			spsSize = cursor - spsStart;
			GST_INFO_OBJECT (me, "SPS size : %d", spsSize);

			break;
		}

		++cursor;
	}
	// start code 4byte
	g_assert (0x00 == cursor[0] && 0x00 == cursor[1]
			  && 0x00 == cursor[2] && 0x01 == cursor[3]);
	cursor += 4;
	// PPS ?byte
	ppsStart = cursor;
	g_assert (0x08 == (cursor[0] & 0x1F));
	// start code 以降が PPS
	cursor = spsppsMap.data + spsppsMap.size;
	ppsSize = cursor - ppsStart;
	GST_INFO_OBJECT (me, "PPS size : %d", ppsSize);
	g_assert (ppsSize == spsppsMap.size - (ppsStart - spsppsMap.data));

	// codec_data の作成
	cdataBuf = gst_buffer_new_allocate(NULL,
		5 + 1 + 2 + spsSize + 1 + 2 + ppsSize, NULL);
	g_assert(NULL != cdataBuf);

	gst_buffer_map(cdataBuf, &cdataMap, GST_MAP_WRITE);

	cdataMap.data[0] = 1;				/* AVC Decoder Configuration Record ver. 1 */
	cdataMap.data[1] = spsStart[1 + 0];	/* profile_idc                             */
	cdataMap.data[2] = spsStart[1 + 1];	/* profile_compability                     */
	cdataMap.data[3] = spsStart[1 + 2];	/* level_idc                               */
	cdataMap.data[4] = 0xfc | (4 - 1);	/* nal_length_size_minus1                  */
	
	index = 5;
	
	cdataMap.data[index++] = 0xe0 | 1;	/* number of SPSs */
	GST_WRITE_UINT16_BE (cdataMap.data + index, spsSize); /* SPS Length */
	index += 2;
	memcpy(cdataMap.data + index, spsStart, spsSize); /* SPS */
	index += spsSize;
	
	cdataMap.data[index++] = 1;         /* number of PPSs */
	GST_WRITE_UINT16_BE (cdataMap.data + index, ppsSize); /* PPS Length */
	index += 2;
	memcpy(cdataMap.data + index, ppsStart, ppsSize); /* PPS */
	index += ppsSize;
	g_assert(index == cdataMap.size);

	gst_buffer_unmap(me->priv->spspps_buf, &spsppsMap);
	gst_buffer_unmap(cdataBuf, &cdataMap);

	me->priv->sps_len = spsSize;
	me->priv->pps_len = ppsSize;

out:
	return cdataBuf;

	/* ERRORS */
failed_get_spspps:
	{
		GST_ELEMENT_ERROR (me, STREAM, ENCODE, (NULL),
						   ("Failed get codec data from HW encoder"));
		if (cdataBuf) {
			gst_buffer_unref(cdataBuf);
			cdataBuf = NULL;
		}
		goto out;
	}
}

static GstCaps *
gst_acm_h264_enc_getcaps (GstVideoEncoder * encoder, GstCaps * filter)
{
	GstCaps *caps;
#if 0
	GstCaps *ret;
#endif

	caps = gst_caps_from_string ("video/x-raw, "
								 "format = (string) { NV12 }, "
								 "framerate = (fraction) [0, MAX], "
								 "width = (int) [ 80, 1920 ], "
								 "height = (int) [ 80, 1080 ]");
#if 0	/* 出力サイズを入力サイズと異なったものを指定した場合、リンクできなくなる */
	ret = gst_video_encoder_proxy_getcaps (encoder, caps, filter);
	gst_caps_unref (caps);
	
	return ret;
#else
	return caps;
#endif
}

static gboolean
gst_acm_h264_enc_set_format (GstVideoEncoder * enc, GstVideoCodecState * state)
{
	GstAcmH264Enc *me = GST_ACMH264ENC (enc);
	gboolean ret = TRUE;
	GstVideoInfo *vinfo;
	GstStructure *structure;
	const GValue *format;
	const gchar *formatStr;
	GstCaps *outcaps;
	GstMapInfo map;
	GstVideoCodecState *output_state;
	GstBuffer *cdataBuf = NULL;
	GstCaps *peercaps;

	vinfo = &(state->info);

	GST_INFO_OBJECT (me, "H264ENC SET FORMAT - info: %d x %d, %d/%d",
					 vinfo->width, vinfo->height, vinfo->fps_n, vinfo->fps_d);
	GST_INFO_OBJECT (me, "H264ENC SET FORMAT - caps: %" GST_PTR_FORMAT, state->caps);
	GST_INFO_OBJECT (me, "H264ENC SET FORMAT - codec_data: %p", state->codec_data);

	/* Save input state to be used as reference for output state */
	if (me->input_state) {
		gst_video_codec_state_unref (me->input_state);
	}
	me->input_state = gst_video_codec_state_ref (state);

	/* video info */
	me->input_width = GST_VIDEO_INFO_WIDTH (vinfo);
	me->input_height = GST_VIDEO_INFO_HEIGHT (vinfo);
	/* input format		*/
	structure = gst_caps_get_structure (state->caps, 0);
	format = gst_structure_get_value (structure, "format");
	if (NULL == format) {
		GST_ERROR_OBJECT (me, "caps has no format");
		goto illegal_caps;
	}
	formatStr = g_value_get_string (format);
	if (g_str_equal (formatStr, "NV12")) {
		me->input_format = V4L2_PIX_FMT_NV12;
	}
	else {
		GST_ERROR_OBJECT (me, "not support format");
		goto illegal_caps;
	}

	/* 画像サイズチェック （HWエンコーダーの制限事項）	*/
	if (me->input_width < GST_ACMH264ENC_WIDTH_MIN
		|| me->input_width > GST_ACMH264ENC_WIDTH_MAX) {
		GST_ERROR_OBJECT (me, "not support image width.");
		
		goto not_support_video_size;
	}
	if (me->input_height < GST_ACMH264ENC_HEIGHT_MIN
		|| me->input_height > GST_ACMH264ENC_HEIGHT_MAX) {
		GST_ERROR_OBJECT (me, "not support image height.");
		
		goto not_support_video_size;
	}
	/* H.264エンコーダーが32バイトアラインメントされたメモリからしかデータを読み出せない
	 * 入力画像幅×入力画像高さは、32の倍数であること 
	 * 入力画像幅と入力画像高さはそれぞれ2の倍数であること
	 */
	if (0 != (me->input_width * me->input_height) % 32) {
		GST_ERROR_OBJECT (me, "not support image size.");
		
		goto not_support_video_size;
	}
	if (0 != (me->input_width & 0x01)) {
		GST_ERROR_OBJECT (me, "not support image width.");
		
		goto not_support_video_size;
	}
	if (0 != (me->input_height & 0x01)) {
		GST_ERROR_OBJECT (me, "not support image height.");
		
		goto not_support_video_size;
	}

	/* エンコードパラメータの決定	*/
	peercaps = gst_pad_get_allowed_caps (GST_VIDEO_ENCODER_SRC_PAD (me));
	GST_INFO_OBJECT (me, "H264ENC SET FORMAT - allowed caps: %" GST_PTR_FORMAT, peercaps);
	if (peercaps && gst_caps_get_size (peercaps) > 0) {
		guint i = 0, n = 0;
		const gchar *str = NULL;

		n = gst_caps_get_size (peercaps);
		GST_INFO_OBJECT (me, "peer allowed caps (%u structure(s)) are %"
						  GST_PTR_FORMAT, n, peercaps);

		for (i = 0; i < n; i++) {
			GstStructure *s = gst_caps_get_structure (peercaps, i);
			if (gst_structure_has_name (s, "video/x-h264")) {
				gint width, height;
				
				if (gst_structure_get_int (s, "width", &width)) {
					GST_INFO_OBJECT (me, "H264ENC SET FORMAT - output width: %d", width);
					me->output_width = width;
				}
				
				if (gst_structure_get_int (s, "height", &height)) {
					GST_INFO_OBJECT (me, "H264ENC SET FORMAT - output height: %d", height);
					me->output_height = height;
				}
			}
			if ((str = gst_structure_get_string (s, "stream-format"))) {
				if (0 == strcmp (str, "avc")) {
					GST_INFO_OBJECT (me, "use avc format for output");
					me->priv->output_format = V4L2_PIX_FMT_H264_NO_SC;
				}
				else if (0 == strcmp (str, "byte-stream")) {
					GST_INFO_OBJECT (me, "use byte-stream format for output");
					me->priv->output_format = V4L2_PIX_FMT_H264;
				}
				else {
					GST_WARNING_OBJECT (me,
						"unknown stream-format: %s, use avc format for output", str);
					me->priv->output_format = V4L2_PIX_FMT_H264_NO_SC;
				}
			}
		}
	}
	if (me->output_width < /* not <= */ 0) {
		me->output_width = me->input_width;
	}
	if (me->output_height < /* not <= */ 0) {
		me->output_height = me->input_height;
	}

#if USE_STRIDE_PROP
	if (me->stride <= 0) {
		me->stride = me->input_width;
	}
	if (me->stride < me->input_width) {
		GST_WARNING_OBJECT (me, "stride: %u is less than video width: %u",
							me->stride, me->input_width);
		me->stride = me->input_width;
	}
#endif
	if (me->max_frame_size <= 0) {
		switch (me->rate_control_mode) {
		case GST_ACMH264ENC_RATE_CONTROL_MODE_CBR_SKIP:
		case GST_ACMH264ENC_RATE_CONTROL_MODE_CBR_NON_SKIP:
			/* (bit_rate / 3) * 4 / 8 バイト (固定ビットレート制御設定時) (130%) */
			me->max_frame_size = (me->bit_rate / 3) * 4 / 8;
			break;
		case GST_ACMH264ENC_RATE_CONTROL_MODE_VBR_NON_SKIP:
			/* bit_rate * 2 / 8 バイト (可変ビットレート制御設定時) (200%) */
			me->max_frame_size = me->bit_rate * 2 / 8;
			break;
		default:
			g_assert_not_reached ();
			break;
		}
	}
	/* max_frame_size : bit_rate ~ 5,000,000 Byte */
	if (me->max_frame_size < (me->bit_rate / 8) /* byte */) {
		GST_WARNING_OBJECT (me,
			"max-frame-size: %u is less than bitrate: %u (%u byte), force to %u",
			me->max_frame_size, me->bit_rate, me->bit_rate / 8, (me->bit_rate / 8));
		me->max_frame_size = (me->bit_rate / 8); /* byte */
	}
	if (me->max_frame_size > GST_ACMH264ENC_MAXFRAMESIZE_MAX) {
		GST_WARNING_OBJECT (me,
			"max-frame-size: %u is more than %u, force to %u",
			me->max_frame_size, GST_ACMH264ENC_MAXFRAMESIZE_MAX,
			GST_ACMH264ENC_MAXFRAMESIZE_MAX);
		me->max_frame_size = GST_ACMH264ENC_MAXFRAMESIZE_MAX;
	}
	if (me->frame_rate_resolution < /* not <= */ 0) {
		me->frame_rate_resolution = vinfo->fps_n;
	}
	if (me->frame_rate_tick < /* not <= */ 0) {
		me->frame_rate_tick = vinfo->fps_d;
	}

#if USE_STRIDE_PROP
	/* ストライドのチェック */
	if (0 != (me->stride * 3 / 2) % 16) {
		/* ストライド（バイト数）は 16 の倍数でなければならない */
		GST_ERROR_OBJECT (me, "stride (bytes) must be a multiple of 16.");

		goto invalid_stride;
	}
#endif

	/* b-pic-mode > 0 の場合は、rate-control-mode は、1 または 2 のみ設定可能 */
	if ((me->B_pic_mode > GST_ACMH264ENC_B_PIC_MODE_0_B_PIC)
		&& (GST_ACMH264ENC_RATE_CONTROL_MODE_CBR_SKIP == me->rate_control_mode)) {
		GST_ERROR_OBJECT (me,
			"force rate-control-mode to %d, because (b-pic-mode > 0)",
			GST_ACMH264ENC_RATE_CONTROL_MODE_CBR_NON_SKIP);

		me->rate_control_mode = GST_ACMH264ENC_RATE_CONTROL_MODE_CBR_NON_SKIP;
	}

	/* 最大 GOP 長のチェック */
	if (me->max_GOP_length <= me->B_pic_mode) {
		/* B_pic_mode以下で設定した場合はパラメータエラーとなる */
		if (0 == me->max_GOP_length
			&& GST_ACMH264ENC_B_PIC_MODE_0_B_PIC == me->B_pic_mode) {
			/* max_GOP_length:0, B_pic_mode:0 の場合は、エラーせず先頭のみIピクチャ */
			
			/* not error */
		}
		else {
			GST_ERROR_OBJECT (me,
				"max-gop-length: %u must be greater than b-pic-mode: %u",
				me->max_GOP_length, me->B_pic_mode);
			
			goto invalid_max_GOP_length;
		}
	}

	/* オフセットの境界チェック
	 * 入力オフセット = src.width * y_offset + x_offset
	 * 入力オフセットは32の倍数であること。
	 * src.width * y_offset / 2 + x_offsetも同様に32の倍数であること
	 */
	if (me->x_offset + me->output_width > me->input_width) {
		GST_WARNING_OBJECT (me, "x_offset: %u is illegal", me->x_offset);
		me->x_offset = me->input_width - me->output_width;
	}
	if (me->y_offset + me->output_height > me->input_height) {
		GST_WARNING_OBJECT (me, "y_offset: %u is illegal", me->y_offset);
		me->y_offset = me->input_height - me->output_height;
	}
	if (0 != (me->input_width * me->y_offset + me->x_offset) % 32) {
		GST_ERROR_OBJECT (me, "offset must be a multiple of 32.");
		
		goto invalid_offset;
	}
	if (0 != (me->input_width * me->y_offset / 2 + me->x_offset) % 32) {
		GST_ERROR_OBJECT (me, "offset must be a multiple of 32.");
		
		goto invalid_offset;
	}

	/* プレエンコード回数の決定	*/
	switch (me->B_pic_mode) {
	case GST_ACMH264ENC_B_PIC_MODE_0_B_PIC:
		me->priv->pre_encode_num = 1;
		break;
	case GST_ACMH264ENC_B_PIC_MODE_1_B_PIC:
		me->priv->pre_encode_num = 1;
		break;
	case GST_ACMH264ENC_B_PIC_MODE_2_B_PIC:
		me->priv->pre_encode_num = 2;
		break;
	case GST_ACMH264ENC_B_PIC_MODE_3_B_PIC:
		me->priv->pre_encode_num = 3;
		break;
	default:
		g_assert_not_reached ();
		break;
	}

	/* initialize HW encoder	*/
	if (! gst_acm_h264_enc_init_encoder(me)) {
		goto init_failed;
	}

	/* make codec_data */
	cdataBuf = gst_acm_h264_enc_make_codec_data(me);
	g_assert(NULL != cdataBuf);

	/* Creates a new output state with the specified fmt, width and height
	 * and Negotiate with downstream elements.
	 */
	outcaps = gst_caps_new_empty_simple ("video/x-h264");
	structure = gst_caps_get_structure (outcaps, 0);

	gst_caps_set_simple (outcaps, "codec_data", GST_TYPE_BUFFER, cdataBuf, NULL);
	if (V4L2_PIX_FMT_H264_NO_SC == me->priv->output_format) {
    	gst_structure_set (structure, "stream-format", G_TYPE_STRING, "avc", NULL);
		gst_structure_set (structure, "alignment", G_TYPE_STRING, "au", NULL);
	}
	else if (V4L2_PIX_FMT_H264 == me->priv->output_format) {
    	gst_structure_set (structure, "stream-format", G_TYPE_STRING, "byte-stream", NULL);
	}
	gst_structure_set (structure, "framerate", GST_TYPE_FRACTION,
					   me->frame_rate_resolution, me->frame_rate_tick, NULL);

	// level と profile
	gst_buffer_map(me->priv->spspps_buf, &map, GST_MAP_READ);
	//    startcode 4 byte, AUD 2 byte, startcode 4 byte, SPS (header 1 byte)
	gst_codec_utils_h264_caps_set_level_and_profile (
		outcaps, map.data + 10 + 1, me->priv->sps_len);
	gst_buffer_unmap(me->priv->spspps_buf, &map);

	GST_VIDEO_INFO_WIDTH (vinfo) = me->output_width;
	GST_VIDEO_INFO_HEIGHT (vinfo) = me->output_height;
	output_state = gst_video_encoder_set_output_state (enc, outcaps, state);
	GST_VIDEO_INFO_WIDTH (vinfo) = me->input_width;
	GST_VIDEO_INFO_HEIGHT (vinfo) = me->input_height;

	if (! gst_video_encoder_negotiate (enc)) {
		goto negotiate_faile;
	}

	GST_INFO_OBJECT (me,
		"H264ENC OUT FORMAT - info: fmt:%" GST_FOURCC_FORMAT ", %d x %d, %d/%d",
		GST_FOURCC_ARGS (me->input_format),
		output_state->info.width, output_state->info.height,
		output_state->info.fps_n, output_state->info.fps_d);
	GST_INFO_OBJECT (me, "H264ENC OUT FORMAT - caps: %" GST_PTR_FORMAT,
					 output_state->caps);
	GST_INFO_OBJECT (me, "H264ENC OUT FORMAT - codec_data: %p",
					 output_state->codec_data);
	gst_video_codec_state_unref (output_state);

#if 0	/* for debug	*/
	GST_INFO_OBJECT (me, "current src caps: %" GST_PTR_FORMAT,
		gst_pad_get_current_caps (GST_VIDEO_ENCODER_SRC_PAD (me)));
	GST_INFO_OBJECT (me, "current sink caps: %" GST_PTR_FORMAT,
		gst_pad_get_current_caps (GST_VIDEO_ENCODER_SINK_PAD (me)));
#endif

	/* set latency */
	if (vinfo->fps_n) {
		GstClockTime latency;

		/* set latency of 2 frames */
		latency = gst_util_uint64_scale(GST_SECOND, GST_VIDEO_INFO_FPS_D(vinfo) * 2, GST_VIDEO_INFO_FPS_N (vinfo));
		
		GST_INFO_OBJECT (me, "set latency to %" GST_TIME_FORMAT ,
						 GST_TIME_ARGS (latency));
		
		gst_video_encoder_set_latency (enc, latency, latency);
	}
	else {
		/* We can't do live as we don't know our latency */
		gst_video_encoder_set_latency (enc, 0, GST_CLOCK_TIME_NONE);

		GST_INFO_OBJECT (me, "set no latency");
	}

out:
	if (cdataBuf) {
		gst_buffer_unref(cdataBuf);
		cdataBuf = NULL;
	}
	return ret;

	/* ERRORS */
illegal_caps:
	{
		GST_ELEMENT_ERROR (me, STREAM, ENCODE, (NULL),
			("illegal caps"));
		ret = FALSE;
		goto out;
	}
not_support_video_size:
	{
		GST_ELEMENT_ERROR (me, STREAM, ENCODE, (NULL),
			("input video size is not support."));
		ret = FALSE;
		goto out;
	}
#if USE_STRIDE_PROP
invalid_stride:
	{
		GST_ELEMENT_ERROR (me, STREAM, ENCODE, (NULL),
			("invalid stride."));
		ret = FALSE;
		goto out;
	}
#endif
invalid_max_GOP_length:
	{
		GST_ELEMENT_ERROR (me, STREAM, ENCODE, (NULL),
			("invalid max-gop-length."));
		ret = FALSE;
		goto out;
	}
invalid_offset:
	{
		GST_ELEMENT_ERROR (me, STREAM, ENCODE, (NULL),
			("invalid offset."));
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

/*
 * Optional. Allows subclass (encoder) to perform post-seek semantics reset.
 */
static gboolean
gst_acm_h264_enc_reset (GstVideoEncoder * enc, gboolean hard)
{
	GstAcmH264Enc *me = GST_ACMH264ENC (enc);

	GST_INFO_OBJECT (me, "H264ENC RESET %s", hard ? "hard" : "soft");

	return TRUE;
}

/*
 * Optional. Called to request subclass to dispatch any pending remaining data 
 * (e.g. at EOS).
 */
static GstFlowReturn
gst_acm_h264_enc_finish (GstVideoEncoder * enc)
{
	GstAcmH264Enc *me = GST_ACMH264ENC (enc);

	GST_INFO_OBJECT (me, "H264ENC FINISH");

	/* do nothing	*/

	return GST_FLOW_OK;
}

static GstFlowReturn
gst_acm_h264_enc_handle_frame (GstVideoEncoder * enc,
    GstVideoCodecFrame * frame)
{
	GstAcmH264Enc *me = GST_ACMH264ENC (enc);
	GstFlowReturn flowRet = GST_FLOW_OK;
#if DBG_MEASURE_PERF_HANDLE_FRAME
	static double interval_time_start = 0, interval_time_end = 0;
#endif

#if DBG_LOG_PERF_CHAIN
	GST_INFO_OBJECT (me, "# H264ENC-CHAIN HANDLE FRMAE START");
#endif
#if 0	/* for debug	*/
	GST_INFO_OBJECT (me, "H264ENC HANDLE FRMAE - frame no:%d, pts:%"
					 GST_TIME_FORMAT ", duration:%" GST_TIME_FORMAT
					 ", dts:%" GST_TIME_FORMAT
					 ", size:%d, ref:%d",
					 frame->system_frame_number,
					 GST_TIME_ARGS (frame->pts), GST_TIME_ARGS (frame->duration),
					 GST_TIME_ARGS (frame->dts), gst_buffer_get_size(frame->input_buffer),
					 GST_OBJECT_REFCOUNT_VALUE(frame->input_buffer));
	GST_INFO_OBJECT (me, "DTS:%" GST_TIME_FORMAT,
					 GST_TIME_ARGS( GST_BUFFER_DTS (frame->input_buffer) ));
	GST_INFO_OBJECT (me, "PTS:%" GST_TIME_FORMAT,
					 GST_TIME_ARGS( GST_BUFFER_PTS (frame->input_buffer) ));
	GST_INFO_OBJECT (me, "duration:%" GST_TIME_FORMAT,
					 GST_TIME_ARGS( GST_BUFFER_DURATION (frame->input_buffer) ));
#endif
#if DBG_MEASURE_PERF_HANDLE_FRAME
	interval_time_end = gettimeofday_sec();
	if (interval_time_start > 0) {
		GST_INFO_OBJECT(me, "handle_frame() at(ms) : %10.10f",
			(interval_time_end - interval_time_start)*1e+3);
	}
	interval_time_start = gettimeofday_sec();
#endif

#if DBG_DUMP_IN_BUF		/* for debug	*/
	dump_input_buf(frame->input_buffer);
#endif

	/* Bピクチャ含む場合の、PTS参照用に、リストに保持	*/
	if (GST_ACMH264ENC_B_PIC_MODE_0_B_PIC != me->B_pic_mode) {
		gst_acm_h264_enc_push_frame (me, frame);
	}

	/* 出力 : プレエンコード分入力した後は、エンコード済みデータができるのを待って取り出す	*/
	if (me->priv->num_inbuf_queued >= me->priv->pre_encode_num) {
//		GST_INFO_OBJECT(me, "me->num_inbuf_queued : %d", me->num_inbuf_queued);
		flowRet = gst_acm_h264_enc_handle_out_frame_with_wait(me);
		if (GST_FLOW_OK != flowRet) {
			goto out;
		}
	}

	/* B_pic_mode == 0 の時は、ダミーのプレエンコード用に、空バッファを QBUF する */
	if (GST_ACMH264ENC_B_PIC_MODE_0_B_PIC == me->B_pic_mode
		&& ! me->priv->is_qbufed_null_when_non_bpic) {
		GST_INFO_OBJECT(me, "B pic mode is %d, then qbuf null buffer", me->B_pic_mode);

		flowRet = gst_acm_h264_enc_qbuf_null_in(me);
		if (GST_FLOW_OK != flowRet) {
			GST_ERROR_OBJECT (me, "failed enqueue null buffer");
			goto out;
		}

		me->priv->is_qbufed_null_when_non_bpic = TRUE;
	}

	/* 入力		*/
	if (me->priv->num_inbuf_acquired < me->pool_in->num_buffers) {
		GstBuffer *v4l2buf_in = NULL;

		GST_INFO_OBJECT(me, "acquire_buffer : %d", me->priv->num_inbuf_acquired);
		
		flowRet = gst_acm_v4l2_buffer_pool_acquire_buffer(
				GST_BUFFER_POOL_CAST (me->pool_in), &v4l2buf_in, NULL);
		if (GST_FLOW_OK != flowRet) {
			GST_ERROR_OBJECT (me, "gst_acm_v4l2_buffer_pool_acquire_buffer() returns %s",
							  gst_flow_get_name (flowRet));
			goto no_buffer;
		}
		me->priv->num_inbuf_acquired++;

		flowRet = gst_acm_h264_enc_handle_in_frame(me, v4l2buf_in, frame->input_buffer);
		if (GST_FLOW_OK != flowRet) {
			goto out;
		}
		me->priv->in_out_frame_count++;
	}
	else {
		flowRet = gst_acm_h264_enc_handle_in_frame_with_wait(me,
					frame->input_buffer);
		if (GST_FLOW_OK != flowRet) {
			goto out;
		}
	}
	if (me->priv->num_inbuf_queued <= me->priv->pre_encode_num) {
		me->priv->num_inbuf_queued++;
	}

out:
#if DBG_LOG_PERF_CHAIN
	GST_INFO_OBJECT (me, "# H264ENC-CHAIN HANDLE FRMAE END");
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
gst_acm_h264_enc_pre_push (GstVideoEncoder *enc, GstVideoCodecFrame *frame)
{
#if DBG_LOG_OUT_TIMESTAMP
	GstAcmH264Enc *me = GST_ACMH264ENC (enc);
	GstBuffer *buffer = frame->output_buffer;
	
//	GST_INFO_OBJECT (me, "size:%d", gst_buffer_get_size(buffer));
	GST_INFO_OBJECT (me, "DTS:%" GST_TIME_FORMAT,
					 GST_TIME_ARGS( GST_BUFFER_DTS (buffer) ));
	GST_INFO_OBJECT (me, "PTS:%" GST_TIME_FORMAT,
					 GST_TIME_ARGS( GST_BUFFER_PTS (buffer) ));
	GST_INFO_OBJECT (me, "duration:%" GST_TIME_FORMAT,
					 GST_TIME_ARGS( GST_BUFFER_DURATION (buffer) ));
#endif
	
	return GST_FLOW_OK;
}

static gboolean
gst_acm_h264_enc_sink_event (GstVideoEncoder * enc, GstEvent *event)
{
	GstAcmH264Enc *me = GST_ACMH264ENC (enc);
	gboolean ret = FALSE;
	GstFlowReturn flowRet = GST_FLOW_OK;

	GST_DEBUG_OBJECT (me, "RECEIVED EVENT (%d)", GST_EVENT_TYPE(event));
	switch (GST_EVENT_TYPE(event)) {
	case GST_EVENT_CAPS:
	{
		GstCaps * caps = NULL;

		gst_event_parse_caps (event, &caps);
		GST_INFO_OBJECT (me, "H264ENC received GST_EVENT_CAPS: %" GST_PTR_FORMAT,
						 caps);

		ret = GST_VIDEO_ENCODER_CLASS (parent_class)->sink_event(enc, event);
		break;
	}
	case GST_EVENT_EOS:
	{
		gint i;
		gint flush_encode_num = me->B_pic_mode + 1;

		GST_INFO_OBJECT (me, "H264ENC received GST_EVENT_EOS");

		/* 入力側にサイズ0のバッファがqbufされた時点でエンコード終了とみなし、
		 * フラッシュが開始される。B_pic_modeに応じた数だけサイズ0のバッファを入力する。
		 * (同じ数だけエンコード結果が格納された出力バッファがdqbufできる。)
		 */
		GST_INFO_OBJECT (me, "flush_encode_num : %d", flush_encode_num);
		for (i = 0; i < flush_encode_num; i++) {
			flowRet = gst_acm_h264_enc_handle_out_frame_with_wait(me);
			if (GST_FLOW_OK != flowRet) {
				goto handle_out_failed;
			}

			flowRet = gst_acm_h264_enc_qbuf_null_in(me);
			if (GST_FLOW_OK != flowRet) {
				GST_ERROR_OBJECT (me, "failed enqueue null buffer");
				ret = FALSE;
				goto out;
			}
		}

		/* デバイス側に溜まっているデータを取り出して、down stream へ流す	*/
		GST_INFO_OBJECT (me, "in_out_frame_count : %d",
						 me->priv->in_out_frame_count);
		while (me->priv->in_out_frame_count > 0) {
			flowRet = gst_acm_h264_enc_handle_out_frame_with_wait(me);
			if (GST_FLOW_OK != flowRet) {
				goto handle_out_failed;
			}
		}

		ret = GST_VIDEO_ENCODER_CLASS (parent_class)->sink_event(enc, event);
		break;
	}
	case GST_EVENT_STREAM_START:
		GST_DEBUG_OBJECT (me, "received GST_EVENT_STREAM_START");
		/* break;	*/
	case GST_EVENT_SEGMENT:
		GST_DEBUG_OBJECT (me, "received GST_EVENT_SEGMENT");
		/* break;	*/
	default:
		ret = GST_VIDEO_ENCODER_CLASS (parent_class)->sink_event(enc, event);
		break;
	}

out:
	return ret;

	/* ERRORS */
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
}

static gboolean
gst_acm_h264_enc_init_encoder (GstAcmH264Enc * me)
{
	gboolean ret = TRUE;
	enum v4l2_buf_type type;
	int r;
	GstCaps *sinkCaps, *srcCaps;
	GstAcmV4l2InitParam v4l2InitParam;
	struct v4l2_format fmt;
	struct v4l2_control ctrl;
	guint offset;
	guint in_buf_size, out_buf_size;

	GST_INFO_OBJECT (me, "H264ENC INITIALIZE ACM ENCODER...");

	/* buffer size : YUV420	*/
	in_buf_size = me->input_width * me->input_height * 3 / 2;
	out_buf_size = me->output_width * me->input_height * 3 / 2;
	GST_INFO_OBJECT (me, "in_buf_size:%u, out_buf_size:%u",
					 in_buf_size, out_buf_size);

	/* offset	*/
	offset = me->input_width * me->y_offset + me->x_offset;

	/* setup encode parameter		*/
	GST_INFO_OBJECT (me, "H264ENC INIT PARAM:");
	GST_INFO_OBJECT (me, " input_width:%d", me->input_width);
	GST_INFO_OBJECT (me, " input_height:%d", me->input_height);
	GST_INFO_OBJECT (me, " output_width:%d", me->output_width);
	GST_INFO_OBJECT (me, " output_height:%d", me->output_height);
#if USE_STRIDE_PROP
	GST_INFO_OBJECT (me, " stride:%d", me->stride);
#endif
	GST_INFO_OBJECT (me, " input_format:%" GST_FOURCC_FORMAT,
					 GST_FOURCC_ARGS (me->input_format));
	GST_INFO_OBJECT (me, " bit_rate:%d", me->bit_rate);
	GST_INFO_OBJECT (me, " max_frame_size:%d", me->max_frame_size);
	GST_INFO_OBJECT (me, " rate_control_mode:%d", me->rate_control_mode);
	GST_INFO_OBJECT (me, " frame_rate_resolution:%d", me->frame_rate_resolution);
	GST_INFO_OBJECT (me, " frame_rate_tick:%d", me->frame_rate_tick);
	GST_INFO_OBJECT (me, " max_GOP_length:%d", me->max_GOP_length);
	GST_INFO_OBJECT (me, " B_pic_mode:%d", me->B_pic_mode);
	GST_INFO_OBJECT (me, " x_offset:%d", me->x_offset);
	GST_INFO_OBJECT (me, " y_offset:%d", me->y_offset);
	GST_INFO_OBJECT (me, " offset:%d", offset);

	/* Set format for output (encoder input) */
	memset(&fmt, 0, sizeof(struct v4l2_format));
	fmt.type 			= V4L2_BUF_TYPE_VIDEO_OUTPUT;
	fmt.fmt.pix.width	= me->input_width;
	fmt.fmt.pix.height	= me->input_height;
	fmt.fmt.pix.pixelformat = me->input_format;
	fmt.fmt.pix.field	= V4L2_FIELD_NONE;
	//fmt.fmt.pix.bytesperline = me->input_width * 3 / 2;	/* YUV420 */
	//fmt.fmt.pix.sizeimage = fmt.fmt.pix.bytesperline * me->input_height;
	fmt.fmt.pix.priv = offset;
	r = gst_acm_v4l2_ioctl(me->video_fd, VIDIOC_S_FMT, &fmt);
	if (r < 0) {
		GST_ERROR_OBJECT(me, "failed ioctl - VIDIOC_S_FMT (OUTPUT)");
		goto set_init_param_failed;
	}

	/* Set format for capture (encoder output) */
	memset(&fmt, 0, sizeof(struct v4l2_format));
	fmt.type			= V4L2_BUF_TYPE_VIDEO_CAPTURE;
	fmt.fmt.pix.width	= me->output_width;
	fmt.fmt.pix.height	= me->output_height;
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_H264;
	fmt.fmt.pix.field	= V4L2_FIELD_NONE;
	//fmt.fmt.pix.bytesperline = me->output_width * 3 / 2;	/* YUV420 */
	//fmt.fmt.pix.sizeimage = fmt.fmt.pix.bytesperline * me->output_height;
	//fmt.fmt.pix.priv = 0;
	r = gst_acm_v4l2_ioctl(me->video_fd, VIDIOC_S_FMT, &fmt);
	if (r < 0) {
		GST_ERROR_OBJECT(me, "failed ioctl - VIDIOC_S_FMT (CAPTURE)");
		goto set_init_param_failed;
	}

	/* bit_rate */
	ctrl.id = V4L2_CID_TARGET_BIT_RATE;
	ctrl.value = me->bit_rate;
	r = gst_acm_v4l2_ioctl(me->video_fd, VIDIOC_S_CTRL, &ctrl);
	if (r < 0) {
		GST_ERROR_OBJECT(me, "failed ioctl - V4L2_CID_TARGET_BIT_RATE");
		goto set_init_param_failed;
	}
	/* max_frame_size */
	ctrl.id = V4L2_CID_MAX_FRAME_SIZE;
	ctrl.value = me->max_frame_size;
	r = gst_acm_v4l2_ioctl(me->video_fd, VIDIOC_S_CTRL, &ctrl);
	if (r < 0) {
		GST_ERROR_OBJECT(me, "failed ioctl - V4L2_CID_MAX_FRAME_SIZE");
		goto set_init_param_failed;
	}
	/* rate_control_mode */
	ctrl.id = V4L2_CID_RATE_CONTROL_MODE;
	ctrl.value = me->rate_control_mode;
	r = gst_acm_v4l2_ioctl(me->video_fd, VIDIOC_S_CTRL, &ctrl);
	if (r < 0) {
		GST_ERROR_OBJECT(me, "failed ioctl - V4L2_CID_RATE_CONTROL_MODE");
		goto set_init_param_failed;
	}
	/* frame_rate_resolution */
	ctrl.id = V4L2_CID_FRAME_RATE_RESOLUTION;
	ctrl.value = me->frame_rate_resolution;
	r = gst_acm_v4l2_ioctl(me->video_fd, VIDIOC_S_CTRL, &ctrl);
	if (r < 0) {
		GST_ERROR_OBJECT(me, "failed ioctl - V4L2_CID_FRAME_RATE_RESOLUTION");
		goto set_init_param_failed;
	}
	/* frame_rate_tick */
	ctrl.id = V4L2_CID_FRAME_RATE_TICK;
	ctrl.value = me->frame_rate_tick;
	r = gst_acm_v4l2_ioctl(me->video_fd, VIDIOC_S_CTRL, &ctrl);
	if (r < 0) {
		GST_ERROR_OBJECT(me, "failed ioctl - V4L2_CID_FRAME_RATE_TICK");
		goto set_init_param_failed;
	}
	/* max_GOP_length */
	ctrl.id = V4L2_CID_MAX_GOP_LENGTH;
	ctrl.value = me->max_GOP_length;
	r = gst_acm_v4l2_ioctl(me->video_fd, VIDIOC_S_CTRL, &ctrl);
	if (r < 0) {
		GST_ERROR_OBJECT(me, "failed ioctl - V4L2_CID_MAX_GOP_LENGTH");
		goto set_init_param_failed;
	}
	/* B_pic_modeの指定 */
	ctrl.id = V4L2_CID_B_PIC_MODE;
	ctrl.value = me->B_pic_mode;
	r = gst_acm_v4l2_ioctl(me->video_fd, VIDIOC_S_CTRL, &ctrl);
	if (r < 0) {
		GST_ERROR_OBJECT(me, "failed ioctl - V4L2_CID_B_PIC_MODE");
		goto set_init_param_failed;
	}

	/* setup buffer pool	*/
	if (NULL == me->pool_in) {
		memset(&v4l2InitParam, 0, sizeof(GstAcmV4l2InitParam));
		v4l2InitParam.video_fd = me->video_fd;
		v4l2InitParam.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
		v4l2InitParam.mode = GST_ACM_V4L2_IO_MMAP;
		v4l2InitParam.sizeimage = in_buf_size;
		v4l2InitParam.init_num_buffers = DEFAULT_NUM_BUFFERS_IN;
		sinkCaps = gst_caps_from_string ("video/x-raw");
		me->pool_in = gst_acm_v4l2_buffer_pool_new(&v4l2InitParam, sinkCaps);
		gst_caps_unref(sinkCaps);
		if (! me->pool_in) {
			goto buffer_pool_new_failed;
		}
	}
	
	if (NULL == me->pool_out) {
		memset(&v4l2InitParam, 0, sizeof(GstAcmV4l2InitParam));
		v4l2InitParam.video_fd = me->video_fd;
		v4l2InitParam.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		v4l2InitParam.mode = GST_ACM_V4L2_IO_MMAP;
		v4l2InitParam.sizeimage = out_buf_size;
		v4l2InitParam.init_num_buffers = DEFAULT_NUM_BUFFERS_OUT;
		srcCaps = gst_caps_from_string ("video/x-h264");
		me->pool_out = gst_acm_v4l2_buffer_pool_new(&v4l2InitParam, srcCaps);
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

	GST_INFO_OBJECT (me, "pool_out - buffers:%d, allocated:%d, queued:%d",
					  me->pool_out->num_buffers,
					  me->pool_out->num_allocated,
					  me->pool_out->num_queued);
	
	/* STREAMON */
	GST_INFO_OBJECT (me, "H264ENC STREAMON");
	type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	r = gst_acm_v4l2_ioctl(me->video_fd, VIDIOC_STREAMON, &type);
	if (r < 0) {
        goto start_failed;
	}
	GST_DEBUG_OBJECT(me, "STREAMON CAPTURE - ret:%d", r);
	
	type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	r = gst_acm_v4l2_ioctl(me->video_fd, VIDIOC_STREAMON, &type);
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
gst_acm_h264_enc_cleanup_encoder (GstAcmH264Enc * me)
{
	gboolean ret = TRUE;
	enum v4l2_buf_type type;
	int r = 0;
	
	GST_INFO_OBJECT (me, "H264ENC CLEANUP ACM ENCODER...");
	
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
#if 0	/* for debug	*/
		gst_acm_v4l2_buffer_pool_log_buf_status(me->pool_out);
#endif
		gst_buffer_pool_set_active (GST_BUFFER_POOL_CAST (me->pool_out), FALSE);
		gst_object_unref (me->pool_out);
		me->pool_out = NULL;
	}

	/* STREAMOFF */
	if (me->priv->is_inited_encoder) {
		GST_INFO_OBJECT (me, "H264ENC STREAMOFF");
		type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
		r = gst_acm_v4l2_ioctl (me->video_fd, VIDIOC_STREAMOFF, &type);
		if (r < 0) {
			goto stop_failed;
		}
		GST_DEBUG_OBJECT(me, "STREAMOFF OUTPUT - ret:%d", r);

		type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		r = gst_acm_v4l2_ioctl (me->video_fd, VIDIOC_STREAMOFF, &type);
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
gst_acm_h264_enc_qbuf_null_in(GstAcmH264Enc * me)
{
	/* サイズ 0 のバッファを OUTPUT 側に QBUF する	*/
	GstFlowReturn flowRet = GST_FLOW_OK;
	GstBuffer *dummyBuf = NULL;

	dummyBuf = gst_buffer_new_allocate(NULL, 0, NULL);
	g_assert(NULL != dummyBuf);

	if (me->priv->num_inbuf_acquired < me->pool_in->num_buffers) {
		GstBuffer *v4l2buf_in = NULL;
		
		GST_INFO_OBJECT(me, "acquire_buffer : %d", me->priv->num_inbuf_acquired);
		
		flowRet = gst_acm_v4l2_buffer_pool_acquire_buffer(
			GST_BUFFER_POOL_CAST (me->pool_in), &v4l2buf_in, NULL);
		if (GST_FLOW_OK != flowRet) {
			GST_ERROR_OBJECT (me, "gst_acm_v4l2_buffer_pool_acquire_buffer() returns %s",
							  gst_flow_get_name (flowRet));
			goto no_buffer;
		}
		me->priv->num_inbuf_acquired++;
		
		flowRet = gst_acm_h264_enc_handle_in_frame(me, v4l2buf_in, dummyBuf);
		if (GST_FLOW_OK != flowRet) {
			goto out;
		}
#if 0	/* ここではカウントしてはいけない */
		me->priv->in_out_frame_count++;
#endif
	}
	else {
		flowRet = gst_acm_h264_enc_handle_in_frame_with_wait(me, dummyBuf);
		if (GST_FLOW_OK != flowRet) {
			goto out;
		}
	}

#if 0	/* ここではカウントしてはいけない */
	if (me->priv->num_inbuf_queued <= me->priv->pre_encode_num) {
		me->priv->num_inbuf_queued++;
	}
#endif

out:
	if (dummyBuf) {
		gst_buffer_unref(dummyBuf);
		dummyBuf = NULL;
	}

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
gst_acm_h264_enc_handle_in_frame_with_wait(GstAcmH264Enc * me, GstBuffer *inbuf)
{
	GstFlowReturn flowRet = GST_FLOW_OK;
	GstBuffer *v4l2buf_in = NULL;
	int r = 0;
	fd_set write_fds;
	struct timeval tv;
#if DBG_MEASURE_PERF_SELECT_IN
	double time_start, time_end;
#endif

	GST_DEBUG_OBJECT(me, "dqbuf (not acquire_buffer)");
	flowRet = gst_acm_v4l2_buffer_pool_dqbuf(me->pool_in, &v4l2buf_in);
	if (GST_FLOW_DQBUF_EAGAIN == flowRet) {
#if DBG_LOG_PERF_SELECT_IN
		GST_INFO_OBJECT(me, "wait until enable dqbuf (pool_in)");
		gst_acm_v4l2_buffer_pool_log_buf_status(me->pool_out);
#endif
		/* 書き込みができる状態になるまで待ってから書き込む		*/
		do {
			FD_ZERO(&write_fds);
			FD_SET(me->video_fd, &write_fds);
			/* no timeout	*/
			tv.tv_sec = 10;
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
			flowRet = gst_acm_v4l2_buffer_pool_dqbuf(me->pool_in, &v4l2buf_in);
			if (GST_FLOW_OK != flowRet) {
				GST_ERROR_OBJECT (me, "gst_acm_v4l2_buffer_pool_dqbuf() returns %s",
								  gst_flow_get_name (flowRet));
				goto dqbuf_failed;
			}
		}
		else if (r < 0) {
			goto select_failed;
		}
		else if (0 == r) {
			GST_ERROR_OBJECT (me, "select() for input is timeout");
			goto select_timeout;
		}
	}
	else if (GST_FLOW_OK != flowRet) {
		goto dqbuf_failed;
	}

	flowRet = gst_acm_h264_enc_handle_in_frame(me, v4l2buf_in, inbuf);
	if (GST_FLOW_OK != flowRet) {
		goto handle_in_failed;
	}

	if (gst_buffer_get_size(inbuf) > 0) {
		/* サイズ 0 のバッファはダミー（プレエンコード or フラッシュ）なのでカウントしない	*/
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
		gst_acm_v4l2_buffer_pool_log_buf_status(me->pool_in);
		
		GST_ERROR_OBJECT (me, "pool_out - buffers:%d, queued:%d",
						  me->pool_out->num_buffers, me->pool_out->num_queued);
		gst_acm_v4l2_buffer_pool_log_buf_status(me->pool_out);
		
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
gst_acm_h264_enc_handle_in_frame(GstAcmH264Enc * me,
	GstBuffer *v4l2buf_in, GstBuffer *inbuf)
{
	GstFlowReturn flowRet = GST_FLOW_OK;
	GstMapInfo map;
	gsize inputDataSize = 0;

	GST_DEBUG_OBJECT(me, "inbuf size=%" G_GSIZE_FORMAT, gst_buffer_get_size(inbuf));

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
	GST_DEBUG_OBJECT(me, "v4l2buf_in size:%" G_GSIZE_FORMAT ", input_size:%" G_GSIZE_FORMAT,
			 gst_buffer_get_size(v4l2buf_in), inputDataSize);

	/* enqueue buffer	*/
	flowRet = gst_acm_v4l2_buffer_pool_qbuf (me->pool_in, v4l2buf_in, inputDataSize);
	if (GST_FLOW_OK != flowRet) {
		GST_ERROR_OBJECT (me, "gst_acm_v4l2_buffer_pool_qbuf() returns %s",
						  gst_flow_get_name (flowRet));
		goto qbuf_failed;
	}

out:
	return flowRet;
	
	/* ERRORS */
qbuf_failed:
	{
		GST_ELEMENT_ERROR (me, STREAM, ENCODE, (NULL),
			("could not queue buffer %d (%s)", errno, g_strerror (errno)));
		flowRet = GST_FLOW_ERROR;
		goto out;
	}
}

static GstFlowReturn
gst_acm_h264_enc_get_spspps(GstAcmH264Enc * me)
{
	GstFlowReturn flowRet = GST_FLOW_OK;
	GstBuffer *v4l2buf_out = NULL;
	int r = 0;
	fd_set read_fds;
	struct timeval tv;

	/* 1回目のCAPTURE(出力)側バッファのDQ時にSPS/PPSをセットしたバッファが返る。*/

	/* CAPTURE側から DQBUF するためには、まずOUTPUT側に QBUF する必要がある */
	flowRet = gst_acm_h264_enc_qbuf_null_in(me);
	if (GST_FLOW_OK != flowRet) {
		GST_ERROR_OBJECT (me, "failed enqueue null buffer");
		goto out;
	}

	/* dequeue buffer	*/
	flowRet = gst_acm_v4l2_buffer_pool_dqbuf (me->pool_out, &v4l2buf_out);
	if (GST_FLOW_DQBUF_EAGAIN == flowRet) {
		/* 読み込みできる状態になるまで待ってから読み込む		*/
		do {
			FD_ZERO(&read_fds);
			FD_SET(me->video_fd, &read_fds);
			tv.tv_sec = 0;
			tv.tv_usec = SELECT_TIMEOUT_MSEC * 1000;
			r = select(me->video_fd + 1, &read_fds, NULL, NULL, &tv);
		} while (r < 0 && (EINTR == errno || EAGAIN == errno));
		if (r > 0) {
			flowRet = gst_acm_v4l2_buffer_pool_dqbuf(me->pool_out, &v4l2buf_out);
			if (GST_FLOW_OK != flowRet) {
				GST_ERROR_OBJECT (me, "gst_acm_v4l2_buffer_pool_dqbuf() returns %s",
								  gst_flow_get_name (flowRet));
				goto dqbuf_failed;
			}
		}
		else if (r < 0) {
			goto select_failed;
		}
		else if (0 == r) {
			/* timeoutしたらエラー	*/
			GST_INFO_OBJECT(me, "select() for output is timeout");
			goto select_timeout;
		}
	}
	else if (GST_FLOW_OK != flowRet) {
		GST_ERROR_OBJECT (me, "gst_acm_v4l2_buffer_pool_dqbuf() returns %s",
						  gst_flow_get_name (flowRet));
		goto dqbuf_failed;
	}

	/* SPSPPS の内容をコピー */
	me->priv->spspps_buf = gst_buffer_copy(v4l2buf_out);
	me->priv->spspps_size = gst_buffer_get_size(me->priv->spspps_buf);

	/* QBUF して戻す	*/
	flowRet = gst_acm_v4l2_buffer_pool_qbuf (
		me->pool_out, v4l2buf_out, gst_buffer_get_size(v4l2buf_out));
	if (GST_FLOW_OK != flowRet) {
		GST_ERROR_OBJECT (me, "gst_acm_v4l2_buffer_pool_qbuf() returns %s",
						  gst_flow_get_name (flowRet));
		goto qbuf_failed;
	}

	/* SPS/PPS なので、in_out_frame_count は変更しない */

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
		gst_acm_v4l2_buffer_pool_log_buf_status(me->pool_in);
		
		GST_ERROR_OBJECT (me, "pool_out - buffers:%d, queued:%d",
						  me->pool_out->num_buffers, me->pool_out->num_queued);
		gst_acm_v4l2_buffer_pool_log_buf_status(me->pool_out);
		
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
qbuf_failed:
	{
		GST_ELEMENT_ERROR (me, STREAM, ENCODE, (NULL),
			("could not queue buffer. %d (%s)", errno, g_strerror (errno)));
		flowRet = GST_FLOW_ERROR;
		goto out;
	}
}

static GstFlowReturn
gst_acm_h264_enc_handle_out_frame_with_wait(GstAcmH264Enc * me)
{
	GstFlowReturn flowRet = GST_FLOW_OK;
	GstBuffer *v4l2buf_out = NULL;
	int r = 0;
	fd_set read_fds;
	struct timeval tv;
#if DBG_MEASURE_PERF_SELECT_OUT
	double time_start, time_end;
#endif

	/* dequeue buffer	*/
	flowRet = gst_acm_v4l2_buffer_pool_dqbuf (me->pool_out, &v4l2buf_out);
	if (GST_FLOW_DQBUF_EAGAIN == flowRet) {
		
		/* 読み込みできる状態になるまで待ってから読み込む		*/
#if DBG_LOG_PERF_SELECT_OUT
		GST_INFO_OBJECT(me, "wait until enable dqbuf (pool_out)");
		gst_acm_v4l2_buffer_pool_log_buf_status(me->pool_out);
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
			flowRet = gst_acm_v4l2_buffer_pool_dqbuf(me->pool_out, &v4l2buf_out);
			if (GST_FLOW_OK != flowRet) {
				GST_ERROR_OBJECT (me, "gst_acm_v4l2_buffer_pool_dqbuf() returns %s",
								  gst_flow_get_name (flowRet));
				goto dqbuf_failed;
			}
		}
		else if (r < 0) {
			goto select_failed;
		}
		else if (0 == r) {
			/* timeoutしたらエラー	*/
			GST_INFO_OBJECT(me, "select() for output is timeout");
			goto select_timeout;
		}
	}
	else if (GST_FLOW_OK != flowRet) {
		GST_ERROR_OBJECT (me, "gst_acm_v4l2_buffer_pool_dqbuf() returns %s",
						  gst_flow_get_name (flowRet));
		goto dqbuf_failed;
	}

	/* プレエンコード時、B_pic_modeに応じた回数だけ、出力側のバッファがサイズ0でdqbufされる。
	 * このバッファは無視して、qbuf して戻す。
	 */
	if (me->priv->num_outbuf_dequeued < me->priv->pre_encode_num) {
		GST_DEBUG_OBJECT(me,
			"skip finish frame - num_outbuf_dequeued : %d, pre_encode_num : %d",
			me->priv->num_outbuf_dequeued, me->priv->pre_encode_num);
		g_assert (0 == gst_buffer_get_size(v4l2buf_out));
		
		flowRet = gst_acm_v4l2_buffer_pool_qbuf (
			me->pool_out, v4l2buf_out, gst_buffer_get_size(v4l2buf_out));
		if (GST_FLOW_OK != flowRet) {
			GST_ERROR_OBJECT (me, "gst_acm_v4l2_buffer_pool_qbuf() returns %s",
							  gst_flow_get_name (flowRet));
			goto qbuf_failed;
		}
		
		me->priv->num_outbuf_dequeued++;
		
		goto out;
	}

	/* g_assert (0 != gst_buffer_get_size(v4l2buf_out));
	 * ピクチャスキップが発生したときに、出力側のバッファサイズが0になる事がある
	 */
	flowRet = gst_acm_h264_enc_handle_out_frame(me, v4l2buf_out);
	if (GST_FLOW_OK != flowRet) {
		if (GST_FLOW_FLUSHING == flowRet) {
			GST_DEBUG_OBJECT(me, "FLUSHING - continue.");
			
			/* エラーとせず、m2mデバイスへのデータ入力は行う	*/
			goto out;
		}
		goto handle_out_failed;
	}

	/* ピクチャスキップ発生時の 0 サイズバッファもカウントする。
	 * 入力したフレーム数 == サイズ 0 のフレーム数 + サイズ 非 0 のフレーム数
	 */
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
		gst_acm_v4l2_buffer_pool_log_buf_status(me->pool_in);
		
		GST_ERROR_OBJECT (me, "pool_out - buffers:%d, queued:%d",
						  me->pool_out->num_buffers, me->pool_out->num_queued);
		gst_acm_v4l2_buffer_pool_log_buf_status(me->pool_out);
		
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
qbuf_failed:
	{
		GST_ELEMENT_ERROR (me, STREAM, ENCODE, (NULL),
			("could not queue buffer. %d (%s)", errno, g_strerror (errno)));
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
		flowRet = GST_FLOW_ERROR;
		goto out;
	}
}

static GstFlowReturn
gst_acm_h264_enc_handle_out_frame(GstAcmH264Enc * me,
	GstBuffer *v4l2buf_out)
{
	GstFlowReturn flowRet = GST_FLOW_OK;
	GstMapInfo map;
	gint pictureType = -1;
	gsize encodedSize = 0;
	gsize outputSize = 0;
	GstVideoCodecFrame *frame = NULL;
	unsigned long captCounter = 0;
	GstVideoCodecFrame *pts_frame = NULL;	// Bピクチャがある場合の PTS 取得用
#if DBG_MEASURE_PERF_FINISH_FRAME
	static double interval_time_start = 0, interval_time_end = 0;
	double time_start = 0, time_end = 0;
#endif

	GST_DEBUG_OBJECT(me, "H264ENC HANDLE OUT FRAME : %p", v4l2buf_out);
	GST_DEBUG_OBJECT(me, "v4l2buf_out size=%" G_GSIZE_FORMAT ", ref:%d",
			 gst_buffer_get_size(v4l2buf_out),
			 GST_OBJECT_REFCOUNT_VALUE(v4l2buf_out));
	GST_DEBUG_OBJECT(me, "pool_out->num_queued : %d", me->pool_out->num_queued);

	pictureType = get_encoded_picture_type(me, v4l2buf_out);
	encodedSize = gst_buffer_get_size(v4l2buf_out);

	/* dequeue frame	*/
	frame = gst_video_encoder_get_oldest_frame(GST_VIDEO_ENCODER (me));
	if (! frame) {
		goto no_frame;
	}
	gst_video_codec_frame_unref(frame);

	/* ピクチャスキップ対応		*/
	if (0 == encodedSize) {
		/* ピクチャスキップが発生したときに、出力側のバッファサイズが0になることがある */
		GST_WARNING_OBJECT (me, "skipping frame %" GST_TIME_FORMAT,
						  GST_TIME_ARGS (frame->pts));

		/* enqueue buffer	*/
		flowRet = gst_acm_v4l2_buffer_pool_qbuf (
			me->pool_out, v4l2buf_out, gst_buffer_get_size(v4l2buf_out));
		if (GST_FLOW_OK != flowRet) {
			GST_ERROR_OBJECT (me, "gst_acm_v4l2_buffer_pool_qbuf() returns %s",
							  gst_flow_get_name (flowRet));
			goto qbuf_failed;
		}
		
		/* drop buffer	*/
		frame->output_buffer = NULL;
		flowRet = gst_video_encoder_finish_frame (GST_VIDEO_ENCODER (me), frame);
		if (GST_FLOW_OK != flowRet) {
			GST_ERROR_OBJECT (me, "gst_video_encoder_finish_frame() returns %s",
							  gst_flow_get_name (flowRet));
			goto finish_frame_failed;
		}

		goto out;
	}

#if 0	/* for debug	*/
	dump_input_buf(v4l2buf_out);
#endif

	/* is key frame ? */
	if (PICTURE_TYPE_I == pictureType) {
		GST_VIDEO_CODEC_FRAME_SET_SYNC_POINT (frame);
	}

	/* PTS	*/
	if (GST_ACMH264ENC_B_PIC_MODE_0_B_PIC != me->B_pic_mode) {
		captCounter = get_capture_counter(me, v4l2buf_out);
		pts_frame = gst_acm_h264_enc_pop_frame(me, captCounter);
		if (NULL == pts_frame) {
			GST_ERROR_OBJECT (me, "failed get frame by capture counter %lu (0x%lx)",
					  captCounter, captCounter);

			goto no_frame_by_capture_counter;
		}
#if 0	/* for debug	*/
		GST_INFO_OBJECT (me, "Got frame by capt counter %lu (0x%x)",
						 captCounter, captCounter);
		GST_INFO_OBJECT (me, "oldest DTS:%" GST_TIME_FORMAT,
						 GST_TIME_ARGS( frame->dts ));
		GST_INFO_OBJECT (me, "oldest DTS:%" GST_TIME_FORMAT,
						 GST_TIME_ARGS( GST_BUFFER_DTS (frame->input_buffer) ));
		GST_INFO_OBJECT (me, "oldest PTS:%" GST_TIME_FORMAT,
						 GST_TIME_ARGS( frame->pts ));
		GST_INFO_OBJECT (me, "capt   DTS:%" GST_TIME_FORMAT,
						 GST_TIME_ARGS( pts_frame->dts ));
		GST_INFO_OBJECT (me, "capt   PTS:%" GST_TIME_FORMAT,
						 GST_TIME_ARGS( pts_frame->pts ));
#endif
		/* gst_video_encoder_finish_frame() での実装により、DTS を明示的にセットする必要有り */
		frame->dts = frame->abidata.ABI.ts;
		frame->pts = pts_frame->pts;
		g_assert(1 == pts_frame->ref_count);
		gst_video_codec_frame_unref(pts_frame);
		pts_frame = NULL;
	}

	/* 出力バッファをアロケート	*/
	switch (me->priv->output_format) {
	case V4L2_PIX_FMT_H264_NO_SC:
		if (! me->priv->is_handled_1stframe_out) {
			// SLICE only -> SLICE only (start code -> NALU size)
			outputSize = encodedSize;
		}
		else {
			// AUD + SLICE -> SLICE only (start code -> NALU size)
			outputSize = encodedSize - AUD_NAL_SIZE;
		}
		break;
	case V4L2_PIX_FMT_H264:
		if (! me->priv->is_handled_1stframe_out) {
			// SLICE only -> AUD-SPS-PPS-SLICE
			g_assert(NULL != me->priv->spspps_buf);
			outputSize = me->priv->spspps_size + encodedSize;
		}
		else {
			// AUD + SLICE -> AUD + SLICE
			outputSize = encodedSize;
		}
		break;
	default:
		g_assert_not_reached ();
		break;
	}
#if 0	/* for debug	*/
	GST_INFO_OBJECT(me, "encoded=%lu, output=%lu", encodedSize, outputSize);
#endif
	flowRet = gst_video_encoder_allocate_output_frame (
		GST_VIDEO_ENCODER (me), frame, outputSize);
	if (GST_FLOW_OK != flowRet) {
		GST_ERROR_OBJECT (me, "gst_video_encoder_allocate_output_frame() returns %s",
						  gst_flow_get_name (flowRet));
		goto failed_allocate_output_frame;
	}

	/* エンコード済みデータをコピー	*/
	gst_buffer_map (v4l2buf_out, &map, GST_MAP_READ);
	GST_DEBUG_OBJECT(me, "copy buf size=%" G_GSIZE_FORMAT, map.size);
	if (! me->priv->is_handled_1stframe_out) {
		// SLICE only
		if (V4L2_PIX_FMT_H264_NO_SC == me->priv->output_format) {
			GstMapInfo dstMap;

			gst_buffer_fill(frame->output_buffer, 0, map.data, map.size);
			/* スタートコードを、NALU サイズで書き換える	*/
			gst_buffer_map (frame->output_buffer, &dstMap, GST_MAP_WRITE);
			GST_WRITE_UINT32_BE (dstMap.data, (outputSize - sizeof(unsigned long)));
			gst_buffer_unmap (frame->output_buffer, &dstMap);
		}
		else if (V4L2_PIX_FMT_H264 == me->priv->output_format) {
			/* 初回フレームには、SPS/PPSを挿入する必要あり
			 * NAL 構成を AUD-SPS-PPS-SLICE にする
			 */
			GstMapInfo dstMap;
			GstMapInfo spsppsMap;

			gst_buffer_map (frame->output_buffer, &dstMap, GST_MAP_WRITE);
			gst_buffer_map (me->priv->spspps_buf, &spsppsMap, GST_MAP_READ);
			GST_INFO_OBJECT(me, "insert SPS/PPS to frame");
			g_assert(me->priv->spspps_size == spsppsMap.size);

			/* SPS/PPS コピー */
			memcpy(dstMap.data, spsppsMap.data, spsppsMap.size);
			/* フレームデータ コピー */
			memcpy(dstMap.data + me->priv->spspps_size, map.data, map.size);
			
			gst_buffer_unmap (frame->output_buffer, &dstMap);
			gst_buffer_unmap (me->priv->spspps_buf, &spsppsMap);
		}

		me->priv->is_handled_1stframe_out = TRUE;
	}
	else {
		// AUD + SLICE
		if (V4L2_PIX_FMT_H264_NO_SC == me->priv->output_format) {
			GstMapInfo dstMap;

			/* AUD は除き、 SLICE の スタートコードを、NAL サイズで書き換える	*/
			gst_buffer_fill(frame->output_buffer, 0,
				map.data + AUD_NAL_SIZE, map.size - AUD_NAL_SIZE);

			gst_buffer_map (frame->output_buffer, &dstMap, GST_MAP_WRITE);
			GST_WRITE_UINT32_BE (dstMap.data, (outputSize - sizeof(unsigned long)));
			gst_buffer_unmap (frame->output_buffer, &dstMap);
		}
		else if (V4L2_PIX_FMT_H264 == me->priv->output_format) {
			/* AUD + SLICE の NAL構成をそのままコピー	*/
			gst_buffer_fill(frame->output_buffer, 0, map.data, map.size);
		}
	}
	gst_buffer_unmap (v4l2buf_out, &map);
	gst_buffer_resize(frame->output_buffer, 0, outputSize);

	GST_DEBUG_OBJECT(me, "outbuf size=%" G_GSIZE_FORMAT,
			 gst_buffer_get_size(frame->output_buffer));

#if DBG_DUMP_OUT_BUF	/* for debug	*/
	dump_output_buf(frame->output_buffer);
#endif

	/* enqueue buffer	*/
	flowRet = gst_acm_v4l2_buffer_pool_qbuf (
			me->pool_out, v4l2buf_out, gst_buffer_get_size(v4l2buf_out));
	if (GST_FLOW_OK != flowRet) {
		GST_ERROR_OBJECT (me, "gst_acm_v4l2_buffer_pool_qbuf() returns %s",
						  gst_flow_get_name (flowRet));
		goto qbuf_failed;
	}

	/* down stream へ バッファを push	*/
#if DBG_LOG_PERF_PUSH
	GST_INFO_OBJECT (me, "H264ENC-PUSH finish_frame START");
#endif
	flowRet = gst_video_encoder_finish_frame (GST_VIDEO_ENCODER (me), frame);
#if DBG_LOG_PERF_PUSH
	GST_INFO_OBJECT (me, "H264ENC-PUSH finish_frame END");
#endif
	if (GST_FLOW_OK != flowRet) {
		GST_ERROR_OBJECT (me, "gst_video_encoder_finish_frame() returns %s",
						  gst_flow_get_name (flowRet));
		goto finish_frame_failed;
	}

out:
	return flowRet;

	/* ERRORS */
no_frame:
	{
//		GST_ELEMENT_ERROR (me, STREAM, ENCODE, (NULL),
//			("display buffer does not have a valid frame"));
//		flowRet = GST_FLOW_ERROR;
		GST_WARNING_OBJECT (me, "no more frame to process");
		flowRet = GST_FLOW_OK;
		
		/* 正しく解放されるように、QBUF する		*/
		flowRet = gst_acm_v4l2_buffer_pool_qbuf (
			me->pool_out, v4l2buf_out, gst_buffer_get_size(v4l2buf_out));
		if (GST_FLOW_OK != flowRet) {
			GST_ERROR_OBJECT (me, "gst_acm_v4l2_buffer_pool_qbuf() returns %s",
							  gst_flow_get_name (flowRet));
			goto qbuf_failed;
		}
		goto out;
	}
no_frame_by_capture_counter:
	{
		GST_ELEMENT_ERROR (me, STREAM, ENCODE, (NULL),
			("not found frame for assign PTS."));
		flowRet = GST_FLOW_ERROR;
		goto out;
	}
failed_allocate_output_frame:
	{
		GST_ELEMENT_ERROR (me, STREAM, ENCODE, (NULL),
			("could not allocate output buffer."));
		flowRet = GST_FLOW_ERROR;
		goto out;
	}
qbuf_failed:
	{
		GST_ELEMENT_ERROR (me, STREAM, ENCODE, (NULL),
			("could not queue buffer. %d (%s)", errno, g_strerror (errno)));
		flowRet = GST_FLOW_ERROR;
		goto out;
	}
finish_frame_failed:
	{
		if (GST_FLOW_NOT_LINKED == flowRet || GST_FLOW_FLUSHING == flowRet) {
			GST_WARNING_OBJECT (me,
				"failed gst_video_encoder_finish_frame() - not link or flushing");

//			flowRet = GST_FLOW_ERROR;
			goto out;
		}

		GST_ELEMENT_ERROR (me, STREAM, ENCODE, (NULL),
			("failed gst_video_encoder_finish_frame()"));
//		flowRet = GST_FLOW_ERROR;
		goto out;
	}
}

static void
gst_acm_h264_enc_push_frame (GstAcmH264Enc * me, GstVideoCodecFrame * frame)
{
	GstVideoCodecFrame *cp_frame;

	/* frame のコピーを作成	*/
	cp_frame = g_slice_new0 (GstVideoCodecFrame);
	g_assert(NULL != cp_frame);
	cp_frame->ref_count = 1;
	cp_frame->system_frame_number = frame->system_frame_number;
	cp_frame->presentation_frame_number = frame->presentation_frame_number;
	cp_frame->pts = frame->pts;
	cp_frame->dts = frame->dts;
	cp_frame->duration = frame->duration;
	cp_frame->abidata.ABI.ts = frame->abidata.ABI.ts;

	/* リストに追加		*/
	me->priv->in_frames = g_list_append (me->priv->in_frames, cp_frame);
#if DBG_LOG_IN_FRAME_LIST
	GST_INFO_OBJECT(me, "LIST APPEND frame : %lu", cp_frame->system_frame_number);
#endif
}

static GstVideoCodecFrame *
gst_acm_h264_enc_pop_frame (GstAcmH264Enc * me, guint32 frame_number)
{
	GstVideoCodecFrame *frame = NULL;
	GList *link;
	GList *g;
	GstVideoCodecFrame *tmp = NULL;
	
	GST_DEBUG_OBJECT (me, "frame_number : %u", frame_number);
	
	/* 該当フレームを検索	*/
	for (g = me->priv->in_frames; g; g = g->next) {
		tmp = g->data;
		
		if (tmp->system_frame_number == frame_number) {
			frame = tmp;
			break;
		}
	}
	
	/* 見つからなければ、キャプチャ順カウンタの折り返しを考慮して検索	*/
	if (NULL == frame) {
		GST_WARNING_OBJECT (me, "failed get frame by capture counter %u (0x%x)",
				    frame_number, frame_number);
		// SH からのキャプチャ順カウンタは、0x7FFFFFFE で折り返す
		frame_number += 0x7FFFFFFF;
		GST_WARNING_OBJECT (me, "try get frame by capture counter %u (0x%x)",
				    frame_number, frame_number);
		
		for (g = me->priv->in_frames; g; g = g->next) {
			tmp = g->data;
			
			if (tmp->system_frame_number == frame_number) {
				frame = tmp;
				break;
			}
		}
	}
	
	/* 見つかったらリストから取り出し		*/
	if (frame) {
		link = g_list_find (me->priv->in_frames, frame);
		if (link) {
#if DBG_LOG_IN_FRAME_LIST
			GST_INFO_OBJECT(me, "LIST POP frame : %lu", frame->system_frame_number);
#endif
			me->priv->in_frames = g_list_delete_link (me->priv->in_frames, link);
		}
	}
	
	return frame;
}

static gboolean
plugin_init (GstPlugin * plugin)
{
	GST_DEBUG_CATEGORY_INIT (acmh264enc_debug, "acmh264enc", 0, "H264 encoding");

	return gst_element_register (plugin, "acmh264enc",
								 GST_RANK_PRIMARY, GST_TYPE_ACMH264ENC);
}

GST_PLUGIN_DEFINE (
	GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    acmh264enc,
    "ACM H264 Encoder",
	plugin_init,
	VERSION,
	"LGPL",
	"GStreamer ACM Plugins",
	"http://armadillo.atmark-techno.com/"
);


/*
 * End of file
 */
