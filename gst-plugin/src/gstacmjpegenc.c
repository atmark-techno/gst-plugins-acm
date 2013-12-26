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

/**
 * SECTION:element-acmjpegenc
 *
 * Encodes jpeg images.
 *
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 * gst-launch-1.0 videotestsrc num-buffers=50 ! video/x-raw, framerate='(fraction)'5/1 ! acmjpegenc ! qtmux ! filesink location=mjpeg.mov
 * ]| a pipeline to mux 5 JPEG frames per second into a 10 sec. long motion jpeg
 * QuickTime.
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

#include "gstacmjpegenc.h"
#include "gstacmv4l2_util.h"
#include "gstacm_util.h"
#include "gstacm_debug.h"


/* エンコーダv4l2デバイスのドライバ名 */
#define DRIVER_NAME					"acm-jpegenc"

/* デバイスに確保するバッファ数	*/
#define DEFAULT_NUM_BUFFERS_IN			1
#define DEFAULT_NUM_BUFFERS_OUT			1

/* エンコーダー初期化パラメータのデフォルト値	*/
#define DEFAULT_VIDEO_DEVICE			"/dev/video0"
#define DEFAULT_JPEG_QUALITY			75
#define DEFAULT_X_OFFSET				0
#define DEFAULT_Y_OFFSET				0

/* select() の timeout 時間 */
#define SELECT_TIMEOUT_MSEC				1000

/* YUV422 semi planar (NV16) は GStreamerに定義が存在せず、
 * 以下のエラーとなってしまうため事実上使用不可  
 * "video-info.c:251:gst_video_info_from_caps: unknown format 'NV16' given"
 * ただし、GStreamer 1.2 では 
 *  GST_VIDEO_FORMAT_NV16: planar 4:2:2 YUV with interleaved UV plane
 * が定義されているので、GStreamer 1.2 へ移行の際は対応可能
 */
#define SUPPORT_NV16					0

/* デバッグログ出力フラグ		*/
#define DBG_LOG_PERF_CHAIN				0
#define DBG_LOG_PERF_SELECT_IN			0
#define DBG_LOG_PERF_PUSH				0
#define DBG_LOG_PERF_SELECT_OUT			0

/* select() による待ち時間の計測		*/
#define DBG_MEASURE_PERF_SELECT_IN		0
#define DBG_MEASURE_PERF_SELECT_OUT		0
#define DBG_MEASURE_PERF_HANDLE_FRAME	0
#define DBG_MEASURE_PERF_FINISH_FRAME	0

/* 入力・出力データのファイルへのダンプ	*/
#define DBG_DUMP_IN_BUF					0
#define DBG_DUMP_OUT_BUF				0


#if DBG_MEASURE_PERF_SELECT_IN
static double g_time_total_select_in =	0;
#endif
#if DBG_MEASURE_PERF_SELECT_OUT
static double g_time_total_select_out =	0;
#endif


/* private member	*/
struct _GstAcmJpegEncPrivate
{
	/* HW エンコーダの初期化済みフラグ	*/
	gboolean is_inited_encoder;

	/* QBUF(V4L2_BUF_TYPE_VIDEO_OUTPUT) 用カウンタ	*/
	gint num_inbuf_acquired;

	/* current frame	*/
	GstVideoCodecFrame *current_frame;
};

GST_DEBUG_CATEGORY_STATIC (acmjpegenc_debug);
#define GST_CAT_DEFAULT (acmjpegenc_debug)
GST_DEBUG_CATEGORY_EXTERN (GST_CAT_PERFORMANCE);

#define GST_ACMJPEGENC_GET_PRIVATE(obj)  \
	(G_TYPE_INSTANCE_GET_PRIVATE ((obj), GST_TYPE_ACMJPEGENC, \
		GstAcmJpegEncPrivate))

/* property */
enum
{
	PROP_0,
	PROP_DEVICE,
	PROP_QUALITY,
	PROP_X_OFFSET,
	PROP_Y_OFFSET,
};

/* pad template caps for source and sink pads.	*/
static GstStaticPadTemplate sink_template_factory =
/* 入力 : YUV420 semi planar or YUV422 semi planar 
 * NV12 == GST_VIDEO_FORMAT_NV12 : planar 4:2:0 YUV with interleaved UV plane
 * NV16 == GStreamerに定義が存在しない
 */
GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
	GST_STATIC_CAPS (
		"video/x-raw, "
		"format = (string) { NV12 }, "
		"width = (int) [ 16, 1920 ], "
		"height = (int) [ 16, 1080 ], "
		"framerate = (fraction) [ 0/1, MAX ] "
#if SUPPORT_NV16
		"; "
		"video/x-raw, "
		"format = (string) { NV16 }, "
		"width = (int) [ 16, 1920 ], "
		"height = (int) [ 16, 1080 ], "
		"framerate = (fraction) [ 0/1, MAX ] "
#endif
	)
);

static GstStaticPadTemplate src_template_factory =
GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (
		"image/jpeg, "
		"width = (int) [ 16, 1920 ], "
		"height = (int) [ 16, 1080 ], "
		"framerate = (fraction) [ 0/1, MAX ]"
	)
);


/* GObject base class method */
static void gst_acm_jpeg_enc_set_property (GObject * object, guint prop_id,
	const GValue * value, GParamSpec * pspec);
static void gst_acm_jpeg_enc_get_property (GObject * object, guint prop_id,
	GValue * value, GParamSpec * pspec);
static void gst_acm_jpeg_enc_finalize (GObject * object);
/* GstVideoEncoder base class method */
static gboolean gst_acm_jpeg_enc_open (GstVideoEncoder * enc);
static gboolean gst_acm_jpeg_enc_close (GstVideoEncoder * enc);
static gboolean gst_acm_jpeg_enc_start (GstVideoEncoder * enc);
static gboolean gst_acm_jpeg_enc_stop (GstVideoEncoder * enc);
static GstCaps *gst_acm_jpeg_enc_getcaps (GstVideoEncoder * enc,
	GstCaps * filter);
static gboolean gst_acm_jpeg_enc_propose_allocation (GstVideoEncoder * encoder,
	GstQuery * query);
static gboolean gst_acm_jpeg_enc_set_format (GstVideoEncoder * enc,
    GstVideoCodecState * state);
static gboolean gst_acm_jpeg_enc_reset (GstVideoEncoder * enc, gboolean hard);
static GstFlowReturn gst_acm_jpeg_enc_finish (GstVideoEncoder * enc);
static GstFlowReturn gst_acm_jpeg_enc_handle_frame (GstVideoEncoder * enc,
    GstVideoCodecFrame * frame);
static gboolean gst_acm_jpeg_enc_sink_event (GstVideoEncoder * enc,
	GstEvent *event);
/* GstAcmJpegEnc class method */
static gboolean gst_acm_jpeg_enc_init_encoder (GstAcmJpegEnc * me);
static gboolean gst_acm_jpeg_enc_cleanup_encoder (GstAcmJpegEnc * me);
static GstFlowReturn gst_acm_jpeg_enc_handle_in_frame_with_wait(
	GstAcmJpegEnc * me, GstBuffer *inbuf);
static GstFlowReturn gst_acm_jpeg_enc_handle_in_frame(GstAcmJpegEnc * me,
	GstBuffer *v4l2buf_in, GstBuffer *inbuf);
static GstFlowReturn gst_acm_jpeg_enc_handle_out_frame_with_wait(GstAcmJpegEnc * me);
static GstFlowReturn gst_acm_jpeg_enc_handle_out_frame(GstAcmJpegEnc * me,
	GstBuffer *v4l2buf_out);

#define gst_acm_jpeg_enc_parent_class parent_class
G_DEFINE_TYPE (GstAcmJpegEnc, gst_acm_jpeg_enc, GST_TYPE_VIDEO_ENCODER);


static void
gst_acm_jpeg_enc_set_property (GObject * object, guint prop_id,
	const GValue * value, GParamSpec * pspec)
{
	GstAcmJpegEnc *me = GST_ACMJPEGENC (object);

	switch (prop_id) {
	case PROP_DEVICE:
		if (me->videodev) {
			g_free (me->videodev);
		}
		me->videodev = g_value_dup_string (value);
		break;
	case PROP_QUALITY:
		me->jpeg_quality = g_value_get_int (value);
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
gst_acm_jpeg_enc_get_property (GObject * object, guint prop_id,
	GValue * value, GParamSpec * pspec)
{
	GstAcmJpegEnc *me = GST_ACMJPEGENC (object);
	
	switch (prop_id) {
	case PROP_DEVICE:
		g_value_set_string (value, me->videodev);
		break;
	case PROP_QUALITY:
		g_value_set_int (value, me->jpeg_quality);
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
gst_acm_jpeg_enc_class_init (GstAcmJpegEncClass * klass)
{
	GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
	GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
	GstVideoEncoderClass *video_encoder_class = GST_VIDEO_ENCODER_CLASS (klass);

	g_type_class_add_private (klass, sizeof (GstAcmJpegEncPrivate));

	gobject_class->set_property = gst_acm_jpeg_enc_set_property;
	gobject_class->get_property = gst_acm_jpeg_enc_get_property;
	gobject_class->finalize = gst_acm_jpeg_enc_finalize;
	
	g_object_class_install_property (gobject_class, PROP_DEVICE,
		g_param_spec_string ("device", "device",
			"The video device eg: /dev/video0 "
			"default device is calculate from driver name.",
			DEFAULT_VIDEO_DEVICE, G_PARAM_READWRITE));

	g_object_class_install_property (gobject_class, PROP_QUALITY,
		g_param_spec_int ("quality", "Quality",
			"Quality of JPEG encoding",
			GST_ACMJPEGENC_QUALITY_MIN, GST_ACMJPEGENC_QUALITY_MAX,
			DEFAULT_JPEG_QUALITY, G_PARAM_READWRITE));

	g_object_class_install_property (gobject_class, PROP_X_OFFSET,
		g_param_spec_int ("x-offset", "X Offset",
			"X Offset of output image. (0 is unspecified)",
			GST_ACMJPEGENC_X_OFFSET_MIN, GST_ACMJPEGENC_X_OFFSET_MAX,
			DEFAULT_X_OFFSET, G_PARAM_READWRITE));

	g_object_class_install_property (gobject_class, PROP_Y_OFFSET,
		g_param_spec_int ("y-offset", "Y Offset",
			"Y Offset of output image. (0 is unspecified)",
			GST_ACMJPEGENC_Y_OFFSET_MIN, GST_ACMJPEGENC_Y_OFFSET_MAX,
			DEFAULT_Y_OFFSET, G_PARAM_READWRITE));

	gst_element_class_add_pad_template (element_class,
			gst_static_pad_template_get (&src_template_factory));
	gst_element_class_add_pad_template (element_class,
			gst_static_pad_template_get (&sink_template_factory));

	gst_element_class_set_static_metadata (element_class,
			"ACM Jpeg encoder", "Codec/Encoder/Image",
			"Encode images in JPEG format", "atmark techno");

	video_encoder_class->open = GST_DEBUG_FUNCPTR (gst_acm_jpeg_enc_open);
	video_encoder_class->close = GST_DEBUG_FUNCPTR (gst_acm_jpeg_enc_close);
	video_encoder_class->start = GST_DEBUG_FUNCPTR (gst_acm_jpeg_enc_start);
	video_encoder_class->stop = GST_DEBUG_FUNCPTR (gst_acm_jpeg_enc_stop);
	video_encoder_class->reset = GST_DEBUG_FUNCPTR (gst_acm_jpeg_enc_reset);
	video_encoder_class->getcaps = GST_DEBUG_FUNCPTR (gst_acm_jpeg_enc_getcaps);
	video_encoder_class->propose_allocation =
		GST_DEBUG_FUNCPTR (gst_acm_jpeg_enc_propose_allocation);
	video_encoder_class->set_format = GST_DEBUG_FUNCPTR (gst_acm_jpeg_enc_set_format);
	video_encoder_class->handle_frame =
		GST_DEBUG_FUNCPTR (gst_acm_jpeg_enc_handle_frame);
	video_encoder_class->finish = GST_DEBUG_FUNCPTR (gst_acm_jpeg_enc_finish);
	video_encoder_class->sink_event =
		GST_DEBUG_FUNCPTR (gst_acm_jpeg_enc_sink_event);
}

static void
gst_acm_jpeg_enc_init (GstAcmJpegEnc * me)
{
	me->priv = GST_ACMJPEGENC_GET_PRIVATE (me);

	me->input_width = -1;
	me->input_height = -1;
	me->input_format = -1;

	me->input_state = NULL;

	me->output_width = -1;
	me->output_height = -1;

	me->video_fd = -1;
	me->pool_in = NULL;
	me->pool_out = NULL;

	me->priv->is_inited_encoder = FALSE;
	me->priv->num_inbuf_acquired = 0;
	me->priv->current_frame = NULL;

	/* property	*/
	me->videodev = NULL;
	me->jpeg_quality = -1;
	me->x_offset = -1;
	me->y_offset = -1;
}

static void
gst_acm_jpeg_enc_finalize (GObject * object)
{
	GstAcmJpegEnc *me = GST_ACMJPEGENC (object);

	GST_INFO_OBJECT (me, "JPEGENC FINALIZE");

	/* プロパティとして保持するため、gst_acm_jpeg_enc_close() 内で free してはいけない	*/
	if (me->videodev) {
		g_free(me->videodev);
		me->videodev = NULL;
	}

	G_OBJECT_CLASS (parent_class)->finalize (object);
}

static gboolean
gst_acm_jpeg_enc_open (GstVideoEncoder * enc)
{
	GstAcmJpegEnc *me = GST_ACMJPEGENC (enc);

	/* プロパティとしてセットされていなければ、デバイスを検索
	 * 他のプロパティは、gst_acm_jpeg_enc_set_format() で設定
	 */
	if (NULL == me->videodev) {
		me->videodev = gst_acm_v4l2_getdev(DRIVER_NAME);
	}

	GST_INFO_OBJECT (me, "JPEGENC OPEN ACM ENCODER. (%s)", me->videodev);

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
gst_acm_jpeg_enc_close (GstVideoEncoder * enc)
{
	GstAcmJpegEnc *me = GST_ACMJPEGENC (enc);

	GST_INFO_OBJECT (me, "JPEGENC CLOSE ACM ENCODER. (%s)", me->videodev);

	/* close device	*/
	if (me->video_fd > 0) {
		gst_acm_v4l2_close(me->videodev, me->video_fd);
		me->video_fd = -1;
	}

	return TRUE;
}

static gboolean
gst_acm_jpeg_enc_start (GstVideoEncoder * enc)
{
	GstAcmJpegEnc *me = GST_ACMJPEGENC (enc);

	GST_INFO_OBJECT (me, "JPEGENC START");

	/* プロパティ以外の変数を再初期化		*/
	me->input_width = -1;
	me->input_height = -1;
	me->input_format = -1;

	me->input_state = NULL;

	me->output_width = -1;
	me->output_height = -1;

	me->pool_in = NULL;
	me->pool_out = NULL;

	me->priv->is_inited_encoder = FALSE;
	me->priv->num_inbuf_acquired = 0;
	me->priv->current_frame = NULL;

	return TRUE;
}

static gboolean
gst_acm_jpeg_enc_stop (GstVideoEncoder * enc)
{
	GstAcmJpegEnc *me = GST_ACMJPEGENC (enc);

	GST_INFO_OBJECT (me, "JPEGENC STOP");

	/* cleanup encoder	*/
	gst_acm_jpeg_enc_cleanup_encoder (me);

	if (me->input_state) {
		gst_video_codec_state_unref (me->input_state);
		me->input_state = NULL;
	}

	return TRUE;
}

static GstCaps *
gst_acm_jpeg_enc_getcaps (GstVideoEncoder * enc, GstCaps * filter)
{
	GstCaps *caps;
#if 0
	GstCaps *ret;
#endif
	
	caps = gst_caps_from_string ("video/x-raw, "
								 "format = (string) { NV12 }, "
								 "width = (int) [ 16, 1920 ], "
								 "height = (int) [ 16, 1080 ], "
								 "framerate = (fraction) [0, MAX]");
	
#if 0	/* 出力サイズを入力サイズと異なったものを指定した場合、リンクできなくなる */
	ret = gst_video_encoder_proxy_getcaps (encoder, caps, filter);
	gst_caps_unref (caps);
	
	return ret;
#else
	return caps;
#endif
}

static gboolean
gst_acm_jpeg_enc_propose_allocation (GstVideoEncoder * encoder, GstQuery * query)
{
	gst_query_add_allocation_meta (query, GST_VIDEO_META_API_TYPE, NULL);

	return GST_VIDEO_ENCODER_CLASS (parent_class)->propose_allocation (
		encoder, query);
}

static gboolean
gst_acm_jpeg_enc_set_format (GstVideoEncoder * enc, GstVideoCodecState * state)
{
	GstAcmJpegEnc *me = GST_ACMJPEGENC (enc);
	gboolean ret = TRUE;
	GstVideoInfo *vinfo;
	GstStructure *structure;
	const GValue *format;
	const gchar *formatStr;
	GstCaps *outcaps;
	GstVideoCodecState *output_state;
	GstCaps *peercaps;

	vinfo = &(state->info);

	GST_INFO_OBJECT (me, "JPEGENC SET FORMAT - info: %d x %d, %d/%d",
					 vinfo->width, vinfo->height, vinfo->fps_n, vinfo->fps_d);
	GST_INFO_OBJECT (me, "JPEGENC SET FORMAT - caps: %" GST_PTR_FORMAT, state->caps);
	GST_INFO_OBJECT (me, "JPEGENC SET FORMAT - codec_data: %p", state->codec_data);

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
#if SUPPORT_NV16
	else if (g_str_equal (formatStr, "NV16")) {
		me->input_format = V4L2_PIX_FMT_NV16;
	}
#endif
	else {
		GST_ERROR_OBJECT (me, "not support format");
		goto illegal_caps;
	}

	/* 入力画像サイズチェック （HWエンコーダーの制限事項）
	 * width : 8pixel の倍数
	 * height : YUV420 の場合 16pixel の倍数、YUV422 の場合 8pixel の倍数
	 */
	if (me->input_width < GST_ACMJPEGENC_WIDTH_MIN
		|| me->input_width > GST_ACMJPEGENC_WIDTH_MAX) {
		GST_ERROR_OBJECT (me, "not support image width.");
		
		goto not_support_image_size;
	}
	if (me->input_height < GST_ACMJPEGENC_HEIGHT_MIN
		|| me->input_height > GST_ACMJPEGENC_HEIGHT_MAX) {
		GST_ERROR_OBJECT (me, "not support image height.");
		
		goto not_support_image_size;
	}
	if (0 != me->input_width % 8) {
		GST_ERROR_OBJECT (me, "image width must be a multiple of 8 pixels.");

		goto not_support_image_size;
	}
	if (V4L2_PIX_FMT_NV12 == me->input_format) {
		if (0 != me->input_height % 16) {
			GST_ERROR_OBJECT (me,
				"image height must be a multiple of 16 pixels. (when YUV420 format)");
			
			goto not_support_image_size;
		}
	}
#if SUPPORT_NV16
	else if (V4L2_PIX_FMT_NV16 == me->input_format) {
		if (0 != me->input_height % 8) {
			GST_ERROR_OBJECT (me,
				"image height must be a multiple of 8 pixels. (when YUV422 format)");
			
			goto not_support_image_size;
		}
	}
#endif

	/* エンコードパラメータの決定	*/
	peercaps = gst_pad_get_allowed_caps (GST_VIDEO_ENCODER_SRC_PAD (me));
	GST_INFO_OBJECT (me, "H264ENC SET FORMAT - allowed caps: %" GST_PTR_FORMAT, peercaps);
	if (peercaps && gst_caps_get_size (peercaps) > 0) {
		guint i = 0, n = 0;
		
		n = gst_caps_get_size (peercaps);
		GST_INFO_OBJECT (me, "peer allowed caps (%u structure(s)) are %"
						 GST_PTR_FORMAT, n, peercaps);
		
		for (i = 0; i < n; i++) {
			GstStructure *s = gst_caps_get_structure (peercaps, i);
			if (gst_structure_has_name (s, "image/jpeg")) {
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
		}
	}
	if (me->jpeg_quality < /* not <= */ 0) {
		me->jpeg_quality = DEFAULT_JPEG_QUALITY;
	}
	if (me->output_width <= 0) {
		me->output_width = me->input_width;
	}
	if (me->output_height <= 0) {
		me->output_height = me->input_height;
	}
	if (me->x_offset < /* not <= */ 0) {
		me->x_offset = DEFAULT_X_OFFSET;
	}
	if (me->y_offset < /* not <= */ 0) {
		me->y_offset = DEFAULT_Y_OFFSET;
	}

	/* オフセットの境界チェック	*/
	if (me->x_offset + me->output_width > me->input_width) {
		GST_WARNING_OBJECT (me, "x_offset: %u is illegal", me->x_offset);
		me->x_offset = me->input_width - me->output_width;
	}
	if (me->y_offset + me->output_height > me->input_height) {
		GST_WARNING_OBJECT (me, "y_offset: %u is illegal", me->y_offset);
		me->y_offset = me->input_height - me->output_height;
	}
	{
		/* オフセットを適用した入力画像サイズチェック （HWエンコーダーの制限事項）
		 * width : 8pixel の倍数
		 * height : YUV420 の場合 16pixel の倍数、YUV422 の場合 8pixel の倍数
		 */
		gint off_width = me->input_width - me->x_offset;
		gint off_height = me->input_height - me->y_offset;

		if (0 != off_width % 8) {
			GST_ERROR_OBJECT (me,
				"input image width (with x-offset) must be a multiple of 8 pixels.");
			
			goto invalid_offset;
		}
		if (V4L2_PIX_FMT_NV12 == me->input_format) {
			if (0 != off_height % 16) {
				GST_ERROR_OBJECT (me,
					"input image height (with y-offset) must be a multiple of 16 pixels. (when YUV420 format)");
				
				goto invalid_offset;
			}
		}
#if SUPPORT_NV16
		else if (V4L2_PIX_FMT_NV16 == me->input_format) {
			if (0 != off_height % 8) {
				GST_ERROR_OBJECT (me,
					"image height (with y-offset) must be a multiple of 8 pixels. (when YUV422 format)");
				
				goto invalid_offset;
			}
		}
#endif
	}

	/* 出力画像幅および高さ : 4の倍数であること	*/
	if (me->x_offset > 0 || me->y_offset > 0) {
		if (0 != me->output_width % 4) {
			GST_ERROR_OBJECT (me,
				"output width must be a multiple of 4 pixels.");

			goto invalid_offset;
		}
		if (0 != me->output_height % 4) {
			GST_ERROR_OBJECT (me,
				"output height must be a multiple of 4 pixels.");

			goto invalid_offset;
		}
	}

	/* Creates a new output state with the specified fmt, width and height
	 * and Negotiate with downstream elements.
	 */
	outcaps = gst_caps_new_empty_simple ("image/jpeg");
	structure = gst_caps_get_structure (outcaps, 0);
    gst_structure_set (structure, "width", G_TYPE_INT, me->output_width, NULL);
    gst_structure_set (structure, "height", G_TYPE_INT, me->output_height, NULL);

	GST_VIDEO_INFO_WIDTH (vinfo) = me->output_width;
	GST_VIDEO_INFO_HEIGHT (vinfo) = me->output_height;
	output_state = gst_video_encoder_set_output_state (enc, outcaps, state);
	GST_VIDEO_INFO_WIDTH (vinfo) = me->input_width;
	GST_VIDEO_INFO_HEIGHT (vinfo) = me->input_height;

	if (! gst_video_encoder_negotiate (enc)) {
		goto negotiate_faile;
	}

	GST_INFO_OBJECT (me,
		"JPEGENC OUT FORMAT - info: %d x %d, %d/%d",
		output_state->info.width, output_state->info.height,
		output_state->info.fps_n, output_state->info.fps_d);
	GST_INFO_OBJECT (me, "JPEGENC OUT FORMAT - caps: %" GST_PTR_FORMAT,
					 output_state->caps);
	GST_INFO_OBJECT (me, "JPEGENC OUT FORMAT - codec_data: %p",
					 output_state->codec_data);
	gst_video_codec_state_unref (output_state);

#if 0	/* for debug	*/
	GST_INFO_OBJECT (me, "current src caps: %" GST_PTR_FORMAT,
		gst_pad_get_current_caps (GST_VIDEO_ENCODER_SRC_PAD (me)));
	GST_INFO_OBJECT (me, "current sink caps: %" GST_PTR_FORMAT,
		gst_pad_get_current_caps (GST_VIDEO_ENCODER_SINK_PAD (me)));
#endif

	/* initialize HW encoder	*/
	if (! gst_acm_jpeg_enc_init_encoder(me)) {
		goto init_failed;
	}

out:
	return ret;

	/* ERRORS */
illegal_caps:
	{
		GST_ELEMENT_ERROR (me, STREAM, ENCODE, (NULL),
			("illegal caps"));
		ret = FALSE;
		goto out;
	}
not_support_image_size:
	{
		GST_ELEMENT_ERROR (me, STREAM, ENCODE, (NULL),
			("input image size is not support."));
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
			("failed to init encoder from stream"));
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
gst_acm_jpeg_enc_reset (GstVideoEncoder * enc, gboolean hard)
{
	GstAcmJpegEnc *me = GST_ACMJPEGENC (enc);

	GST_INFO_OBJECT (me, "JPEGENC RESET %s", hard ? "hard" : "soft");

	return TRUE;
}

/*
 * Optional. Called to request subclass to dispatch any pending remaining data 
 * (e.g. at EOS).
 */
static GstFlowReturn
gst_acm_jpeg_enc_finish (GstVideoEncoder * enc)
{
	GstAcmJpegEnc *me = GST_ACMJPEGENC (enc);

	GST_INFO_OBJECT (me, "JPEGENC FINISH");

	/* do nothing	*/

	return GST_FLOW_OK;
}

static GstFlowReturn
gst_acm_jpeg_enc_handle_frame (GstVideoEncoder * enc,
    GstVideoCodecFrame * frame)
{
	GstAcmJpegEnc *me = GST_ACMJPEGENC (enc);
	GstFlowReturn flowRet = GST_FLOW_OK;
#if DBG_MEASURE_PERF_HANDLE_FRAME
	static double interval_time_start = 0, interval_time_end = 0;
#endif

#if DBG_LOG_PERF_CHAIN
	GST_INFO_OBJECT (me, "# JPEGENC-CHAIN HANDLE FRMAE START");
#endif
	GST_DEBUG_OBJECT (me, "JPEGENC HANDLE FRMAE - frame no:%d, timestamp:%"
					  GST_TIME_FORMAT ", duration:%" GST_TIME_FORMAT,
					  ", size:%d, ref:%d",
					  frame->system_frame_number,
					  GST_TIME_ARGS (frame->pts), GST_TIME_ARGS (frame->duration),
					  gst_buffer_get_size(frame->input_buffer),
					  GST_OBJECT_REFCOUNT_VALUE(frame->input_buffer));

#if DBG_MEASURE_PERF_HANDLE_FRAME
	interval_time_end = gettimeofday_sec();
	if (interval_time_start > 0) {
//		if ((interval_time_end - interval_time_start) > 0.022) {
			GST_INFO_OBJECT(me, "handle_frame() at(ms) : %10.10f",
							(interval_time_end - interval_time_start)*1e+3);
//		}
	}
	interval_time_start = gettimeofday_sec();
#endif

#if DBG_DUMP_IN_BUF		/* for debug	*/
	dump_input_buf(frame->input_buffer);
#endif

	me->priv->current_frame = frame;

	/* 入力		*/
	if (me->priv->num_inbuf_acquired < me->pool_in->num_buffers) {
		GstBuffer* v4l2buf_in = NULL;

		GST_INFO_OBJECT(me, "acquire_buffer : %d", me->priv->num_inbuf_acquired);

		flowRet = gst_acm_v4l2_buffer_pool_acquire_buffer(
				GST_BUFFER_POOL_CAST (me->pool_in), &v4l2buf_in, NULL);
		if (GST_FLOW_OK != flowRet) {
			GST_ERROR_OBJECT (me, "gst_acm_v4l2_buffer_pool_acquire_buffer() returns %s",
							  gst_flow_get_name (flowRet));
			goto no_buffer;
		}

		me->priv->num_inbuf_acquired++;

		flowRet = gst_acm_jpeg_enc_handle_in_frame(me, v4l2buf_in,
					frame->input_buffer);
		if (GST_FLOW_OK != flowRet) {
			goto out;
		}
	}
	else {
		flowRet = gst_acm_jpeg_enc_handle_in_frame_with_wait(me,
					frame->input_buffer);
		if (GST_FLOW_OK != flowRet) {
			goto out;
		}
	}

	/* 出力 : エンコード済みデータができるのを待って取り出す	*/
	flowRet = gst_acm_jpeg_enc_handle_out_frame_with_wait(me);
	if (GST_FLOW_OK != flowRet) {
		goto out;
	}

out:
#if DBG_LOG_PERF_CHAIN
	GST_INFO_OBJECT (me, "# JPEGENC-CHAIN HANDLE FRMAE END");
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

static gboolean
gst_acm_jpeg_enc_sink_event (GstVideoEncoder * enc, GstEvent *event)
{
	GstAcmJpegEnc *me = GST_ACMJPEGENC (enc);
	gboolean ret = FALSE;

	GST_DEBUG_OBJECT (me, "RECEIVED EVENT (%d)", GST_EVENT_TYPE(event));
	switch (GST_EVENT_TYPE(event)) {
	case GST_EVENT_CAPS:
	{
		GstCaps * caps = NULL;

		gst_event_parse_caps (event, &caps);
		GST_INFO_OBJECT (me, "JPEGENC received GST_EVENT_CAPS: %" GST_PTR_FORMAT,
						 caps);

		ret = GST_VIDEO_ENCODER_CLASS (parent_class)->sink_event(enc, event);
		break;
	}
	case GST_EVENT_EOS:
	{
		GST_INFO_OBJECT (me, "JPEGENC received GST_EVENT_EOS");

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

	return ret;
}

static gboolean
gst_acm_jpeg_enc_init_encoder (GstAcmJpegEnc * me)
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

	GST_INFO_OBJECT (me, "JPEGENC INITIALIZE ACM ENCODER...");

	/* buffer size : YUV422 or YUV420	*/
	in_buf_size = me->input_width * me->input_height * 3;
	out_buf_size = me->input_width * me->input_height * 3;

	/* offset	*/
	offset = me->input_width * me->y_offset + me->x_offset;

	/* setup encode parameter		*/
	GST_INFO_OBJECT (me, "JPEGENC INIT PARAM:");
	GST_INFO_OBJECT (me, " input_width:%d", me->input_width);
	GST_INFO_OBJECT (me, " input_height:%d", me->input_height);
	GST_INFO_OBJECT (me, " input_format:%" GST_FOURCC_FORMAT,
					 GST_FOURCC_ARGS (me->input_format));
	GST_INFO_OBJECT (me, " jpeg_quality:%d", me->jpeg_quality);
	GST_INFO_OBJECT (me, " output_width:%d", me->output_width);
	GST_INFO_OBJECT (me, " output_height:%d", me->output_height);
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
#if 0	// not need
	if (V4L2_PIX_FMT_NV16 == me->input_format) {
		fmt.fmt.pix.bytesperline = me->input_width * 2;
	}
	else if (V4L2_PIX_FMT_NV12 == me->input_format) {
		fmt.fmt.pix.bytesperline = me->input_width * 3 / 2;
	}
	fmt.fmt.pix.sizeimage = fmt.fmt.pix.bytesperline * me->input_height;
#endif
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
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_JPEG;
	fmt.fmt.pix.field	= V4L2_FIELD_NONE;
#if 0	// not need
	fmt.fmt.pix.bytesperline = me->input_width * 2;
	fmt.fmt.pix.sizeimage = fmt.fmt.pix.bytesperline * me->input_height;
#endif
	r = gst_acm_v4l2_ioctl(me->video_fd, VIDIOC_S_FMT, &fmt);
	if (r < 0) {
		GST_ERROR_OBJECT(me, "failed ioctl - VIDIOC_S_FMT (CAPTURE)");
		goto set_init_param_failed;
	}

	/* Compression quality */
	ctrl.id = V4L2_CID_JPEG_COMPRESSION_QUALITY;
	ctrl.value = me->jpeg_quality;
	r = gst_acm_v4l2_ioctl(me->video_fd, VIDIOC_S_CTRL, &ctrl);
	if (r < 0) {
		GST_ERROR_OBJECT(me, "failed ioctl - V4L2_CID_JPEG_COMPRESSION_QUALITY");
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
		srcCaps = gst_caps_from_string ("image/jpeg");
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
	GST_INFO_OBJECT (me, "JPEGENC STREAMON");
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
gst_acm_jpeg_enc_cleanup_encoder (GstAcmJpegEnc * me)
{
	gboolean ret = TRUE;
	enum v4l2_buf_type type;
	int r = 0;

	GST_INFO_OBJECT (me, "JPEGENC CLEANUP ACM ENCODER...");

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
		GST_INFO_OBJECT (me, "JPEGENC STREAMOFF");
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
gst_acm_jpeg_enc_handle_in_frame_with_wait(GstAcmJpegEnc * me, GstBuffer *inbuf)
{
	GstFlowReturn flowRet = GST_FLOW_OK;
	GstBuffer* v4l2buf_in = NULL;
	fd_set write_fds;
	struct timeval tv;
	int r = 0;
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
		GST_ERROR_OBJECT (me, "gst_acm_v4l2_buffer_pool_dqbuf() returns %s",
						  gst_flow_get_name (flowRet));
		
		goto dqbuf_failed;
	}

	flowRet = gst_acm_jpeg_enc_handle_in_frame(me, v4l2buf_in, inbuf);
	if (GST_FLOW_OK != flowRet) {
		goto handle_in_failed;
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
gst_acm_jpeg_enc_handle_in_frame(GstAcmJpegEnc * me,
	GstBuffer *v4l2buf_in, GstBuffer *inbuf)
{
	GstFlowReturn flowRet = GST_FLOW_OK;
	GstMapInfo map;
	gsize inputDataSize = 0;

	GST_DEBUG_OBJECT(me, "inbuf size=%d", gst_buffer_get_size(inbuf));

	/* 入力データをコピー	*/
	gst_buffer_map(inbuf, &map, GST_MAP_READ);
	inputDataSize = map.size;
	gst_buffer_fill(v4l2buf_in, 0, map.data, map.size);
	gst_buffer_unmap(inbuf, &map);
	GST_DEBUG_OBJECT(me, "v4l2buf_in size:%d, input_size:%d",
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
gst_acm_jpeg_enc_handle_out_frame_with_wait(GstAcmJpegEnc * me)
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

	flowRet = gst_acm_jpeg_enc_handle_out_frame(me, v4l2buf_out);
	if (GST_FLOW_OK != flowRet) {
		if (GST_FLOW_FLUSHING == flowRet) {
			GST_DEBUG_OBJECT(me, "FLUSHING - continue.");
			
			/* エラーとしない	*/
			goto out;
		}
		goto handle_out_failed;
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
gst_acm_jpeg_enc_handle_out_frame(GstAcmJpegEnc * me,
	GstBuffer *v4l2buf_out)
{
	GstFlowReturn flowRet = GST_FLOW_OK;
	GstMapInfo map;
	gsize encodedSize = 0;
#if DBG_MEASURE_PERF_FINISH_FRAME
	static double interval_time_start = 0, interval_time_end = 0;
	double time_start = 0, time_end = 0;
#endif

	GST_DEBUG_OBJECT(me, "JPEGENC HANDLE OUT FRAME : %p", v4l2buf_out);
	GST_DEBUG_OBJECT(me, "v4l2buf_out size=%d, ref:%d",
					 gst_buffer_get_size(v4l2buf_out),
					 GST_OBJECT_REFCOUNT_VALUE(v4l2buf_out));
	GST_DEBUG_OBJECT(me, "pool_out->num_queued : %d", me->pool_out->num_queued);

#if DBG_DUMP_OUT_BUF	/* for debug	*/
	dump_output_buf(v4l2buf_out);
#endif

	encodedSize = gst_buffer_get_size(v4l2buf_out);
	if (0 == encodedSize) {
		GST_ERROR_OBJECT (me, "encoded size is zero!");
		goto failed_allocate_output_frame;
	}

	/* 出力バッファをアロケート	*/
	flowRet = gst_video_encoder_allocate_output_frame (
		GST_VIDEO_ENCODER (me), me->priv->current_frame, encodedSize);
	if (GST_FLOW_OK != flowRet) {
		GST_ERROR_OBJECT (me, "gst_video_encoder_allocate_output_frame() returns %s",
						  gst_flow_get_name (flowRet));
		goto failed_allocate_output_frame;
	}
	GST_DEBUG_OBJECT(me, "outbuf :%p", me->priv->current_frame->output_buffer);

	/* 出力データをコピー	*/
	gst_buffer_map (v4l2buf_out, &map, GST_MAP_READ);
	GST_DEBUG_OBJECT(me, "copy buf size=%d", map.size);
	gst_buffer_fill(me->priv->current_frame->output_buffer, 0, map.data, map.size);
	gst_buffer_unmap (v4l2buf_out, &map);
	gst_buffer_resize(me->priv->current_frame->output_buffer, 0, encodedSize);

	GST_DEBUG_OBJECT(me, "outbuf size=%d",
					 gst_buffer_get_size(me->priv->current_frame->output_buffer));

	/* enqueue buffer	*/
	flowRet = gst_acm_v4l2_buffer_pool_qbuf (me->pool_out, v4l2buf_out, encodedSize);
	if (GST_FLOW_OK != flowRet) {
		GST_ERROR_OBJECT (me, "gst_acm_v4l2_buffer_pool_qbuf() returns %s",
						  gst_flow_get_name (flowRet));
		goto qbuf_failed;
	}

	/* down stream へ バッファを push	*/
#if DBG_LOG_PERF_PUSH
	GST_INFO_OBJECT (me, "JPEGENC-PUSH finish_frame START");
#endif
	GST_VIDEO_CODEC_FRAME_SET_SYNC_POINT (me->priv->current_frame);
	flowRet = gst_video_encoder_finish_frame (
		GST_VIDEO_ENCODER (me), me->priv->current_frame);
	me->priv->current_frame = NULL;
#if DBG_LOG_PERF_PUSH
	GST_INFO_OBJECT (me, "JPEGENC-PUSH finish_frame END");
#endif
	if (GST_FLOW_OK != flowRet) {
		GST_ERROR_OBJECT (me, "gst_video_encoder_finish_frame() returns %s",
						  gst_flow_get_name (flowRet));
		goto finish_frame_failed;
	}

out:
	return flowRet;

	/* ERRORS */
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

static gboolean
plugin_init (GstPlugin * plugin)
{
	GST_DEBUG_CATEGORY_INIT (acmjpegenc_debug, "acmjpegenc", 0, "JPEG encoding");

	return gst_element_register (plugin, "acmjpegenc",
		GST_RANK_PRIMARY, GST_TYPE_ACMJPEGENC);
}

GST_PLUGIN_DEFINE (
	GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    acmjpegenc,
    "ACM JPEG Encoder",
	plugin_init,
	VERSION,
	"LGPL",
	"GStreamer ACM Plugins",
	"http://armadillo.atmark-techno.com/"
);


/*
 * End of file
 */
