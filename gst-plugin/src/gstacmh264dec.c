/* GStreamer ACM H264 Decoder plugin
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
 * SECTION:element-acmh264dec
 *
 * acmh264dec decodes H264 (MPEG-4) stream.
 *
 * <refsect2>
 * <title>Example launch lines</title>
 * |[
 * gst-launch filesrc location=example.mp4 ! qtdemux ! acmh264dec ! autovideosink
 * ]| Play h264 from mp4 file.
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <inttypes.h>
#include <sched.h>
#include <errno.h>
#include <gst/video/gstvideometa.h>
#include <gst/video/gstvideopool.h>
#define GST_USE_UNSTABLE_API
#include <gst/codecparsers/gsth264parser.h>

#include "gstacmh264dec.h"
#include "gstacmv4l2_util.h"
#include "gstacmdmabufmeta.h"


/* バッファプール内のバッファを no copy で down stream に push する	*/
#define DO_PUSH_POOLS_BUF			1

/* フレームのドロップは、sink 側で行う	*/
#define DO_FRAME_DROP				0

/* フィールド構造のインタレースに対応する	*/
#define SUPPORT_CODED_FIELD			1

/* defined at acm-driver/include/acm-h264dec.h	*/
	/* buffering_pic_cnt */
#define V4L2_CID_NR_BUFFERING_PICS		(V4L2_CID_PRIVATE_BASE + 1)
	/* VIO6の有効無効フラグ */
#define V4L2_CID_ENABLE_VIO				(V4L2_CID_PRIVATE_BASE + 2)
	/* frame rate (fps) */
#define V4L2_CID_FRAME_RATE				(V4L2_CID_PRIVATE_BASE + 3)

/* 確保するバッファ数	*/
#define DEFAULT_NUM_BUFFERS_IN			3
#define DEFAULT_NUM_BUFFERS_OUT			3
#define DEFAULT_NUM_BUFFERS_OUT_DMABUF	NUM_FB_DMABUF

/* デコーダ初期化パラメータのデフォルト値	*/
#define DEFAULT_VIDEO_DEVICE			"/dev/video0"
#define DEFAULT_OUT_WIDTH				0
#define DEFAULT_OUT_HEIGHT				0
#define DEFAULT_BUF_PIC_CNT				17
#define DEFAULT_OUT_FORMAT				GST_ACMH264DEC_OUT_FMT_RGB24
#define DEFAULT_OUT_VIDEO_FORMAT_STR	"RGB"
#define DEFAULT_ENABLE_VIO6				TRUE
#define DEFAULT_FRAME_STRIDE			0
#define DEFAULT_FRAME_X_OFFSET			0
#define DEFAULT_FRAME_Y_OFFSET			0

/* デコーダv4l2デバイスのドライバ名 */
#define DRIVER_NAME			"acm-h264dec"

/* select() の timeout */
#define SELECT_TIMEOUT_MSEC			1000

/* デバッグログ出力フラグ		*/
#define DBG_LOG_INTERLACED			0

/* 一度に刈り取る出力バッファの数 */
#define NUM_HANDLE_OUTBUF 2

struct _GstAcmH264DecPrivate
{
	/* V4L2_BUF_TYPE_VIDEO_OUTPUT 側に入力したフレーム数と、
	 * V4L2_BUF_TYPE_VIDEO_CAPTURE 側から取り出したフレーム数の差分
	 */
	volatile gint in_out_frame_count;

	struct v4l2_buffer input_vbuffer[DEFAULT_NUM_BUFFERS_IN];

	/* fbdev sink が dma-buf を使用する場合のアドレス保存		*/
	gboolean using_fb_dmabuf;
	gint num_fb_dmabuf;
	gint fb_dmabuf_index[NUM_FB_DMABUF];
	gint fb_dmabuf_fd[NUM_FB_DMABUF];

	/* ディスプレイ表示中のバッファは、次の表示を終えるまで、ref して
	 * 保持しておかないと、m2m デバイスに enqueue され、ディスプレイに表示中に、
	 * デコードデータを上書きされてしまい、画面が乱れてしまう
	 */
	GstBuffer* displaying_buf;

#if SUPPORT_CODED_FIELD
	/* NALユニットパーサ	*/
	GstH264NalParser *nalparser;
	/* progressive or interlaced ?	*/
	gboolean is_interlaced;
	guint nal_length_size;
	/* フィールド構造 or フレーム構造 ?	*/
	gboolean is_field_structure;
#endif
};

#if SUPPORT_CODED_FIELD
/* GstVideoCodecFrameFlags の拡張		*/
typedef enum
{
	/** トップフィールドを格納	*/
	GST_VIDEO_CODEC_FRAME_FLAG_TOP_FIELD			= (1<<24),
	/** ボトムフィールドを格納	*/
	GST_VIDEO_CODEC_FRAME_FLAG_BOTTOM_FIELD			= (1<<25),
	/** 両フィールドを格納			*/
	GST_VIDEO_CODEC_FRAME_FLAG_TOP_BOTTOM_FIELD		= (1<<26),
	/** フレームを格納			*/
	GST_VIDEO_CODEC_FRAME_FLAG_FRAME				= (1<<27),
} GstVideoCodecFrameFlagsEx;
#endif

GST_DEBUG_CATEGORY_STATIC (acmh264dec_debug);
#define GST_CAT_DEFAULT (acmh264dec_debug)
GST_DEBUG_CATEGORY_EXTERN (GST_CAT_PERFORMANCE);

#define GST_ACMH264DEC_GET_PRIVATE(obj)  \
	(G_TYPE_INSTANCE_GET_PRIVATE ((obj), GST_TYPE_ACMH264DEC, \
		GstAcmH264DecPrivate))

/* property */
enum
{
	PROP_0,
	PROP_DEVICE,
	PROP_FRAME_STRIDE,
	PROP_FRAME_X_OFFSET,
	PROP_FRAME_Y_OFFSET,
	PROP_BUF_PIC_CNT,
	PROP_ENABLE_VIO6,
};

/* pad template caps for source and sink pads.	*/
static GstStaticPadTemplate sink_template_factory =
GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
	GST_STATIC_CAPS (
		/* TSコンテナの場合、tsdemux からは直接リンクできず、h264parse を挟まなく
		 * てはならない。この場合、stream-format = avc, alignment = au に限定
		 * しないと SPS, PPS の解析をしてくれない。
		 */
		"video/x-h264, "
		"stream-format = (string)avc, "
		"alignment = (string)au, "
		"width  = (int)[80, 1920], "
		"height = (int)[80, 1080], "
		"framerate = (fraction) [ 0/1, MAX ] "
	)
);

// NV12 == GST_VIDEO_FORMAT_NV12
static GstStaticPadTemplate src_template_factory =
GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (
		"video/x-raw, "
		"format = (string) RGB16, "
		"width = (int)[80, 1920], "
		"height = (int)[80, 1080], "
		"framerate = (fraction) [ 0/1, MAX ]"
		"; "
		"video/x-raw, "
		"format = (string) RGB, "
		"width = (int)[80, 1920], "
		"height = (int)[80, 1080], "
		"framerate = (fraction) [ 0/1, MAX ]"
		"; "
		"video/x-raw, "
		"format = (string) RGBx, "
		"width = (int)[80, 1920], "
		"height = (int)[80, 1080], "
		"framerate = (fraction) [ 0/1, MAX ]"
		"; "
		"video/x-raw, "
		"format = (string) NV12, "
		"width = (int) [ 80, 1920 ], "
		"height = (int) [ 80, 1080 ], "
		"framerate = (fraction) [ 0/1, MAX ]"
	)
);

/* for debug */
static void
log_buf_status_of_input(GstAcmH264Dec * me)
{
	struct v4l2_buffer vbuffer;
	int index;
	int r = 0;

	GST_INFO_OBJECT (me, "BUF STATUS (OUTPUT)");

	for (index = 0; index < DEFAULT_NUM_BUFFERS_IN; index++) {
		memset(&vbuffer, 0, sizeof(struct v4l2_buffer));
		
		vbuffer.index	= index;
		vbuffer.type	= V4L2_BUF_TYPE_VIDEO_OUTPUT;
		vbuffer.memory = V4L2_MEMORY_USERPTR;
		r = gst_acm_v4l2_ioctl (me->video_fd, VIDIOC_QUERYBUF, &vbuffer);
		if (r < 0) {
			GST_ERROR_OBJECT (me,
							  "OUT: - Failed QUERYBUF: %s",
							  g_strerror (errno));
		}
		
		GST_INFO_OBJECT (me, "  index:     %u", vbuffer.index);
		GST_INFO_OBJECT (me, "  flags:     %08x", vbuffer.flags);
		if (V4L2_BUF_FLAG_QUEUED == (vbuffer.flags & V4L2_BUF_FLAG_QUEUED)) {
			GST_INFO_OBJECT (me, "  V4L2_BUF_FLAG_QUEUED");
		}
		if (V4L2_BUF_FLAG_DONE == (vbuffer.flags & V4L2_BUF_FLAG_DONE)) {
			GST_INFO_OBJECT (me, "  V4L2_BUF_FLAG_DONE");
		}
	}
}

/* GstVideoDecoder base class method */
static gboolean gst_acm_h264_dec_open (GstVideoDecoder * dec);
static gboolean gst_acm_h264_dec_close (GstVideoDecoder * dec);
static gboolean gst_acm_h264_dec_start (GstVideoDecoder * dec);
static gboolean gst_acm_h264_dec_stop (GstVideoDecoder * dec);
static gboolean gst_acm_h264_dec_set_format (GstVideoDecoder * dec,
    GstVideoCodecState * state);
static gboolean gst_acm_h264_dec_reset (GstVideoDecoder * dec, gboolean hard);
static GstFlowReturn gst_acm_h264_dec_finish (GstVideoDecoder * dec);
static GstFlowReturn gst_acm_h264_dec_parse (GstVideoDecoder *dec,
	GstVideoCodecFrame *frame, GstAdapter *adapter, gboolean at_eos);
static GstFlowReturn gst_acm_h264_dec_handle_frame (GstVideoDecoder * dec,
    GstVideoCodecFrame * frame);
static gboolean gst_acm_h264_dec_sink_event (GstVideoDecoder * dec,
	GstEvent *event);

static gboolean gst_acm_h264_dec_init_decoder (GstAcmH264Dec * me);
static gboolean gst_acm_h264_dec_cleanup_decoder (GstAcmH264Dec * me);
static GstFlowReturn gst_acm_h264_dec_handle_in_frame(GstAcmH264Dec * me,
	struct v4l2_buffer* v4l2buf_in, GstBuffer *inbuf, GstVideoCodecFrame * frame);
static GstFlowReturn gst_acm_h264_dec_handle_out_frame(GstAcmH264Dec * me,
	GstBuffer *v4l2buf_out, gboolean* is_eos);

static void gst_acm_h264_dec_set_property (GObject * object, guint prop_id,
	const GValue * value, GParamSpec * pspec);
static void gst_acm_h264_dec_get_property (GObject * object, guint prop_id,
	GValue * value, GParamSpec * pspec);
static void gst_acm_h264_dec_finalize (GObject * object);

#define gst_acm_h264_dec_parent_class parent_class
G_DEFINE_TYPE (GstAcmH264Dec, gst_acm_h264_dec, GST_TYPE_VIDEO_DECODER);


static void
gst_acm_h264_dec_set_property (GObject * object, guint prop_id,
							  const GValue * value, GParamSpec * pspec)
{
	GstAcmH264Dec *me = GST_ACMH264DEC (object);

	switch (prop_id) {
	case PROP_DEVICE:
		/* デバイスファイル名	*/
		if (me->videodev) {
			g_free (me->videodev);
		}
		me->videodev = g_value_dup_string (value);
		break;
	case PROP_BUF_PIC_CNT:
		me->buffering_pic_cnt = g_value_get_uint (value);
		break;
	case PROP_ENABLE_VIO6:
		me->enable_vio6 = g_value_get_boolean (value);
		break;
	case PROP_FRAME_STRIDE:
		me->frame_stride = g_value_get_uint (value);
		break;
	case PROP_FRAME_X_OFFSET:
		me->frame_x_offset = g_value_get_uint (value);
		break;
	case PROP_FRAME_Y_OFFSET:
		me->frame_y_offset = g_value_get_uint (value);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
gst_acm_h264_dec_get_property (GObject * object, guint prop_id,
	GValue * value, GParamSpec * pspec)
{
	GstAcmH264Dec *me = GST_ACMH264DEC (object);
	
	switch (prop_id) {
	case PROP_DEVICE:
		g_value_set_string (value, me->videodev);
		break;
	case PROP_BUF_PIC_CNT:
		g_value_set_uint (value, me->buffering_pic_cnt);
		break;
	case PROP_ENABLE_VIO6:
		g_value_set_boolean (value, me->enable_vio6);
		break;
	case PROP_FRAME_STRIDE:
		g_value_set_uint (value, me->frame_stride);
		break;
	case PROP_FRAME_X_OFFSET:
		g_value_set_uint (value, me->frame_x_offset);
		break;
	case PROP_FRAME_Y_OFFSET:
		g_value_set_uint (value, me->frame_y_offset);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
gst_acm_h264_dec_class_init (GstAcmH264DecClass * klass)
{
	GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
	GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
	GstVideoDecoderClass *video_decoder_class = GST_VIDEO_DECODER_CLASS (klass);

	g_type_class_add_private (klass, sizeof (GstAcmH264DecPrivate));

	gobject_class->set_property = gst_acm_h264_dec_set_property;
	gobject_class->get_property = gst_acm_h264_dec_get_property;
	gobject_class->finalize = gst_acm_h264_dec_finalize;
	
	g_object_class_install_property (gobject_class, PROP_DEVICE,
		g_param_spec_string ("device", "device",
			"The video device eg: /dev/video0",
			NULL, G_PARAM_READWRITE));

	g_object_class_install_property (gobject_class, PROP_FRAME_STRIDE,
		g_param_spec_uint ("stride", "Stride",
			"Stride of output video. (0 is unspecified)",
		GST_ACMH264DEC_STRIDE_MIN, GST_ACMH264DEC_STRIDE_MAX,
		DEFAULT_FRAME_STRIDE, G_PARAM_READWRITE));

	g_object_class_install_property (gobject_class, PROP_FRAME_X_OFFSET,
		g_param_spec_uint ("x-offset", "X Offset",
			"X Offset of output video. (0 is unspecified)",
		GST_ACMH264DEC_X_OFFSET_MIN, GST_ACMH264DEC_X_OFFSET_MAX,
		DEFAULT_FRAME_X_OFFSET, G_PARAM_READWRITE));

	g_object_class_install_property (gobject_class, PROP_FRAME_Y_OFFSET,
		g_param_spec_uint ("y-offset", "Y Offset",
			"Y Offset of output video. (0 is unspecified)",
		GST_ACMH264DEC_Y_OFFSET_MIN, GST_ACMH264DEC_Y_OFFSET_MAX,
		DEFAULT_FRAME_Y_OFFSET, G_PARAM_READWRITE));

	g_object_class_install_property (gobject_class, PROP_BUF_PIC_CNT,
		g_param_spec_uint ("buf-pic-cnt", "Buffering Pic Cnt",
			"Number of buffering picture",
			GST_ACMH264DEC_BUF_PIC_CNT_MIN, GST_ACMH264DEC_BUF_PIC_CNT_MAX,
			DEFAULT_BUF_PIC_CNT, G_PARAM_READWRITE));

	g_object_class_install_property (gobject_class, PROP_ENABLE_VIO6,
		g_param_spec_boolean ("enable-vio6", "Enable VIO6",
			"FALSE: disable, TRUE: enable",
			DEFAULT_ENABLE_VIO6, G_PARAM_READWRITE));

	gst_element_class_add_pad_template (element_class,
			gst_static_pad_template_get (&src_template_factory));
	gst_element_class_add_pad_template (element_class,
			gst_static_pad_template_get (&sink_template_factory));

	gst_element_class_set_static_metadata (element_class,
			"ACM H264 video decoder", "Codec/Decoder/Video",
			"ACM H.264/AVC decoder", "Atmark Techno, Inc.");

	video_decoder_class->open = GST_DEBUG_FUNCPTR (gst_acm_h264_dec_open);
	video_decoder_class->close = GST_DEBUG_FUNCPTR (gst_acm_h264_dec_close);
	video_decoder_class->start = GST_DEBUG_FUNCPTR (gst_acm_h264_dec_start);
	video_decoder_class->stop = GST_DEBUG_FUNCPTR (gst_acm_h264_dec_stop);
	video_decoder_class->reset = GST_DEBUG_FUNCPTR (gst_acm_h264_dec_reset);
	video_decoder_class->set_format = GST_DEBUG_FUNCPTR (gst_acm_h264_dec_set_format);
	video_decoder_class->parse = GST_DEBUG_FUNCPTR (gst_acm_h264_dec_parse);
	video_decoder_class->handle_frame =
		GST_DEBUG_FUNCPTR (gst_acm_h264_dec_handle_frame);
	video_decoder_class->finish = GST_DEBUG_FUNCPTR (gst_acm_h264_dec_finish);
	video_decoder_class->sink_event =
		GST_DEBUG_FUNCPTR (gst_acm_h264_dec_sink_event);
}

static void
gst_acm_h264_dec_init (GstAcmH264Dec * me)
{
	me->priv = GST_ACMH264DEC_GET_PRIVATE (me);

	me->width = 0;
	me->height = 0;
	me->frame_rate = 0;
	me->spspps_size = 0;

	me->input_state = NULL;
	me->output_state = NULL;

	me->out_width = DEFAULT_OUT_WIDTH;
	me->out_height = DEFAULT_OUT_HEIGHT;
	me->out_video_fmt_str = NULL;
	me->out_video_fmt = GST_VIDEO_FORMAT_UNKNOWN;

	me->video_fd = -1;
	me->is_handled_1stframe = FALSE;
	me->pool_out = NULL;
	me->num_inbuf_acquired = 0;
	me->is_got_decoded_1stframe = FALSE;

	/* property	*/
	me->videodev = NULL;
	me->buffering_pic_cnt = DEFAULT_BUF_PIC_CNT;
	me->input_format = GST_ACMH264DEC_IN_FMT_UNKNOWN;
	me->output_format = GST_ACMH264DEC_OUT_FMT_UNKNOWN;
	me->enable_vio6 = DEFAULT_ENABLE_VIO6;
	me->frame_stride = DEFAULT_FRAME_STRIDE;
	me->frame_x_offset = DEFAULT_FRAME_X_OFFSET;
	me->frame_y_offset = DEFAULT_FRAME_Y_OFFSET;

#if SUPPORT_CODED_FIELD
	me->priv->nalparser = NULL;
	me->priv->is_interlaced = FALSE;
	me->priv->nal_length_size = 4;
	me->priv->is_field_structure = FALSE;
#endif

	/* If the input is packetized, then the parse method will not be called. */
	gst_video_decoder_set_packetized (GST_VIDEO_DECODER (me), TRUE);
}

static void
gst_acm_h264_dec_finalize (GObject * object)
{
	GstAcmH264Dec *me = GST_ACMH264DEC (object);

	GST_INFO_OBJECT (me, "H264DEC FINALIZE");

	/* プロパティを保持するため、gst_acm_h264_dec_close() 内で free してはいけない	*/
	if (me->videodev) {
		g_free(me->videodev);
		me->videodev = NULL;
	}
	if (me->out_video_fmt_str) {
		g_free(me->out_video_fmt_str);
		me->out_video_fmt_str = NULL;
	}

	G_OBJECT_CLASS (parent_class)->finalize (object);
}

static gboolean
gst_acm_h264_dec_open (GstVideoDecoder * dec)
{
	GstAcmH264Dec *me = GST_ACMH264DEC (dec);

	/* プロパティとしてセットされていなければ、デバイスを検索		*/
	if (NULL == me->videodev)
		me->videodev = gst_acm_v4l2_getdev(DRIVER_NAME);

	if (!me->videodev) {
		GST_ELEMENT_ERROR(me, RESOURCE, NOT_FOUND,
				  ("Unable to find any device node for %s driver under /dev", DRIVER_NAME),
				  ("Perhaps, you need to load ACM firmware or no decoder is selected.\n"
				   "Check /sys/devices/platform/acm.0/codec"));
		return FALSE;
	}

	GST_INFO_OBJECT (me, "H264DEC OPEN ACM DECODER. (%s)", me->videodev);
	
	/* デバイスファイルオープン	*/
	GST_INFO_OBJECT (me, "Trying to open device %s", me->videodev);
	if (! gst_acm_v4l2_open (me->videodev, &(me->video_fd), TRUE)) {
        GST_ELEMENT_ERROR (me, RESOURCE, NOT_FOUND, (NULL),
			("Failed open device %s. (%s)", me->videodev, g_strerror (errno)));
		return FALSE;
	}
	GST_INFO_OBJECT (me, "Opened device '%s' successfully", me->videodev);

	/* デフォルト値設定	*/
	if (NULL == me->out_video_fmt_str) {
		me->out_video_fmt_str = g_strdup (DEFAULT_OUT_VIDEO_FORMAT_STR);
		me->out_video_fmt = GST_VIDEO_FORMAT_RGB;
		me->output_format = DEFAULT_OUT_FORMAT;
	}

	return TRUE;
}

static gboolean
gst_acm_h264_dec_close (GstVideoDecoder * dec)
{
	GstAcmH264Dec *me = GST_ACMH264DEC (dec);

	GST_INFO_OBJECT (me, "H264DEC CLOSE ACM DECODER. (%s)", me->videodev);
	
	/* close device	*/
	if (me->video_fd > 0) {
		gst_acm_v4l2_close(me->videodev, me->video_fd);
		me->video_fd = -1;
	}

	return TRUE;
}

static gboolean
gst_acm_h264_dec_start (GstVideoDecoder * dec)
{
	gint i;
	GstAcmH264Dec *me = GST_ACMH264DEC (dec);
	
	GST_INFO_OBJECT (me, "H264DEC START");

	/* never mind a few errors */
	gst_video_decoder_set_max_errors (dec, 20);
	me->priv->in_out_frame_count = 0;

	/* プロパティ以外の変数を再初期化		*/
	me->width = 0;
	me->height = 0;
	me->spspps_size = 0;

	me->is_handled_1stframe = FALSE;
	me->num_inbuf_acquired = 0;
	me->is_got_decoded_1stframe = FALSE;

	me->priv->using_fb_dmabuf = FALSE;
	me->priv->num_fb_dmabuf = 0;
	for (i = 0; i < NUM_FB_DMABUF; i++) {
		me->priv->fb_dmabuf_index[i] = -1;
		me->priv->fb_dmabuf_fd[i] = -1;
	}

	me->priv->displaying_buf = NULL;

#if SUPPORT_CODED_FIELD
	me->priv->nalparser = gst_h264_nal_parser_new ();
	if (NULL == me->priv->nalparser) {
		GST_ERROR_OBJECT (me, "Out of memory");
		
		return FALSE;
	}
	me->priv->is_interlaced = FALSE;
	me->priv->nal_length_size = 4;
	me->priv->is_field_structure = FALSE;
#endif

	return TRUE;
}

static gboolean
gst_acm_h264_dec_stop (GstVideoDecoder * dec)
{
	GstAcmH264Dec *me = GST_ACMH264DEC (dec);

	GST_INFO_OBJECT (me, "H264DEC STOP");
	
	if (me->input_state) {
		gst_video_codec_state_unref (me->input_state);
		me->input_state = NULL;
	}
	if (me->output_state) {
		gst_video_codec_state_unref (me->output_state);
		me->output_state = NULL;
	}

	if (me->priv->displaying_buf) {
		gst_buffer_unref(me->priv->displaying_buf);
		me->priv->displaying_buf = NULL;
	}

	/* クリーンアップ処理	*/
	gst_acm_h264_dec_cleanup_decoder (me);

#if SUPPORT_CODED_FIELD
	if (me->priv->nalparser) {
		gst_h264_nal_parser_free (me->priv->nalparser);
		me->priv->nalparser = NULL;
	}
#endif

	return TRUE;
}

static gboolean
gst_acm_h264_dec_analyze_codecdata(GstAcmH264Dec *me, GstBuffer * codec_data)
{
	unsigned int sps_size, pps_size;
	unsigned int spses_size = 0, ppses_size = 0;
	unsigned int sps_num, pps_num;
	int counter = 5;
	int i, j;
	GstMapInfo map;
#if SUPPORT_CODED_FIELD
	GstH264NalUnit nalu;
	GstH264ParserResult parseres;
	guint offset = 0;
	GstH264SPS sps = { 0, };
	GstH264PPS pps = { 0, };
#endif

	gst_buffer_map(codec_data, &map, GST_MAP_READ);

	/* parse the avcC data */
    if (map.size < 8) {
		gst_buffer_unmap (codec_data, &map);
		goto avcc_too_small;
    }

#if SUPPORT_CODED_FIELD
	me->priv->nal_length_size = (map.data[4] & 0x03) + 1;
    GST_INFO_OBJECT (me, "nal length size %u", me->priv->nal_length_size);
#endif

	/* get sps_num */
	sps_num = *(unsigned char *)(map.data + counter) & 0x1f;
	counter++;

#if SUPPORT_CODED_FIELD
	offset = 6;
#endif
	sps_size = 0;
	spses_size = 0;
	/* get sps process */
	for(i = 0; i < sps_num; i++){
#if SUPPORT_CODED_FIELD
		parseres = gst_h264_parser_identify_nalu_avc (me->priv->nalparser,
					map.data, offset, map.size, 2, &nalu);
		if (GST_H264_PARSER_OK != parseres) {
			gst_buffer_unmap (codec_data, &map);
			goto avcc_too_small;
		}

		parseres = gst_h264_parser_parse_sps (me->priv->nalparser, &nalu, &sps, TRUE);
		if (GST_H264_PARSER_OK != parseres) {
			GST_WARNING_OBJECT (me, "failed to parse SPS:");
		}

		if (0 == sps.frame_mbs_only_flag) {
			GST_INFO_OBJECT (me, "SPS - INTERLACED SEQUENCE");
			me->priv->is_interlaced = TRUE;
		}
		else {
			GST_INFO_OBJECT (me, "SPS - NON INTERLACED SEQUENCE");
		}

		offset = nalu.offset + nalu.size;
#endif

		/* get sps position and size */
		sps_size = (*(unsigned char *)(map.data + counter) << 8)
					| *(unsigned char *)(map.data + counter + 1);
		counter += 2;

		/* copy sps size to me->spspps */
		for (j = 0; j < 4; j++) {
			me->spspps[spses_size + j] = (unsigned char)(sps_size << (8 * (3 - j)));
		}
		/* copy sps data to me->spspps */
		for (j = 0; j < sps_size; j++){
			me->spspps[spses_size + 4 + j] = *(unsigned char *)(map.data + counter);
			counter++;
		}
		spses_size += sps_size + 4;
	}
	
	/* get pps_num */
	pps_num = *(unsigned char *)(map.data + counter);
	counter++;

#if SUPPORT_CODED_FIELD
	offset++;
#endif

	pps_size = 0;
	ppses_size = 0;
	/* get pps process */
	for (i = 0; i < pps_num; i++) {
#if SUPPORT_CODED_FIELD
		parseres = gst_h264_parser_identify_nalu_avc (me->priv->nalparser,
						map.data, offset, map.size, 2, &nalu);
		if (GST_H264_PARSER_OK != parseres) {
			gst_buffer_unmap (codec_data, &map);
			goto avcc_too_small;
		}

		parseres = gst_h264_parser_parse_pps (me->priv->nalparser, &nalu, &pps);
		if (GST_H264_PARSER_OK != parseres) {
			GST_WARNING_OBJECT (me, "failed to parse PPS:");
		}
		offset = nalu.offset + nalu.size;
#endif

		/* get last pps position and size */
		pps_size = (*(unsigned char *)(map.data + counter) << 8)
					| *(unsigned char *)(map.data + counter + 1);
		counter += 2;

		/* copy pps size to me->spspps */
		for (j = 0; j < 4; j++) {
			me->spspps[spses_size + ppses_size + j]
				= (unsigned char)(pps_size << (8 * (3 - j)));
		}
		/* copy pps data to me->spspps */
		for(j = 0 ; j < pps_size ; j++){
			me->spspps[spses_size + ppses_size + 4 + j]
				= *(unsigned char *)(map.data + counter);
			counter++;
		}
		ppses_size += pps_size + 4;
	}
	
	me->spspps_size = spses_size + ppses_size;

	GST_INFO_OBJECT(me, "spspps_size = %d (spses_size = %d , ppses_size = %d)",
					me->spspps_size, spses_size, ppses_size);
	gst_buffer_unmap(codec_data, &map);

	return TRUE;

	/* ERRORS */
avcc_too_small:
	{
		GST_ELEMENT_ERROR (me, STREAM, DECODE, (NULL),
			("avcC size %" G_GSIZE_FORMAT " < 8", map.size));
		return FALSE;
	}
}

static gboolean
gst_acm_h264_dec_set_format (GstVideoDecoder * dec, GstVideoCodecState * state)
{
	GstAcmH264Dec *me = GST_ACMH264DEC (dec);
	gboolean ret = TRUE;
	GstVideoInfo *vinfo;
	GstStructure *structure = NULL;
	const gchar *alignment = NULL;
	gint screen_width = 0;
	gint screen_height = 0;
	GstCaps *peercaps;

	vinfo = &(state->info);

	GST_INFO_OBJECT (me, "H264DEC SET FORMAT - info: %d x %d, %d/%d",
					 vinfo->width, vinfo->height, vinfo->fps_n, vinfo->fps_d);
	GST_INFO_OBJECT (me, "H264DEC SET FORMAT - caps: %" GST_PTR_FORMAT, state->caps);
	GST_INFO_OBJECT (me, "H264DEC SET FORMAT - codec_data: %p", state->codec_data);

	/* Save input state to be used as reference for output state */
	if (me->input_state) {
		gst_video_codec_state_unref (me->input_state);
	}
	me->input_state = gst_video_codec_state_ref (state);

	/* video info */
	structure = gst_caps_get_structure (state->caps, 0);
	alignment = gst_structure_get_string (structure, "alignment");
	GST_INFO_OBJECT (me, "H264DEC SET FORMAT - alignment: %s",
					 (NULL == alignment ? "null" : alignment));
	if (NULL != alignment) {
		if (g_str_equal(alignment, "au")) {
			me->input_format = GST_ACMH264DEC_IN_FMT_MP4;
		}
		else if (g_str_equal(alignment, "nal")) {
			me->input_format = GST_ACMH264DEC_IN_FMT_ES;
		}
	}
	else {
		me->input_format = GST_ACMH264DEC_IN_FMT_MP4;
	}

	peercaps = gst_pad_get_allowed_caps (GST_VIDEO_DECODER_SRC_PAD (me));
	GST_INFO_OBJECT (me, "H264DEC SET FORMAT - allowed caps: %" GST_PTR_FORMAT, peercaps);
	if (peercaps && gst_caps_get_size (peercaps) > 0) {
		GstStructure *capsStructure = gst_caps_get_structure (peercaps, 0);
		const gchar *s;
		gint i;

		if (gst_structure_get_int (capsStructure, "width", &i)) {
			GST_INFO_OBJECT (me, "H264DEC SET FORMAT - output width: %d", i);
			me->out_width = i;
		}

		if (gst_structure_get_int (capsStructure, "height", &i)) {
			GST_INFO_OBJECT (me, "H264DEC SET FORMAT - output height: %d", i);
			me->out_height = i;
		}

		if ((s = gst_structure_get_string (capsStructure, "format"))) {
			GST_INFO_OBJECT (me, "H264DEC SET FORMAT - output format: %s", s);
			me->out_video_fmt_str = g_strdup (s);

			if (g_str_equal (s, "NV12")) {
				me->out_video_fmt = GST_VIDEO_FORMAT_NV12;
				me->output_format = GST_ACMH264DEC_OUT_FMT_YUV420;
			}
			else if (g_str_equal (s, "RGBx")) {
				me->out_video_fmt = GST_VIDEO_FORMAT_RGBx;
				me->output_format = GST_ACMH264DEC_OUT_FMT_RGB32;
			}
			else if (g_str_equal (s, "RGB")) {
				me->out_video_fmt = GST_VIDEO_FORMAT_RGB;
				me->output_format = GST_ACMH264DEC_OUT_FMT_RGB24;
			}
			else if (g_str_equal (s, "RGB16")) {
				me->out_video_fmt = GST_VIDEO_FORMAT_RGB16;
				me->output_format = GST_ACMH264DEC_OUT_FMT_RGB565;
			}
		}
	}

	me->width = vinfo->width;
	me->height = vinfo->height;
	if (DEFAULT_OUT_WIDTH == me->out_width) {
		me->out_width = me->width;
	}
	if (DEFAULT_OUT_HEIGHT == me->out_height) {
		me->out_height = me->height;
	}
	me->frame_rate = vinfo->fps_n / vinfo->fps_d;

	/* analyze codecdata */
	if (state->codec_data) {
		gst_acm_h264_dec_analyze_codecdata(me, state->codec_data);
	}

	do {
		/* sink からスクリーン情報取得		*/
		GstQuery *customQuery = NULL;
		GstStructure *callStructure;
		const GstStructure *resStructure;
		
		callStructure = gst_structure_new ("GstAcmFBDevScreenInfoQuery",
										   "screen_width", G_TYPE_INT, 0,
										   "screen_height", G_TYPE_INT, 0,
										   NULL);
		customQuery = gst_query_new_custom(GST_QUERY_CUSTOM, callStructure);
		
		
		if (! gst_pad_peer_query (GST_VIDEO_DECODER_SRC_PAD(me), customQuery)) {
			GST_WARNING_OBJECT (me, "refused screen info query from peer src pad");
			break;
		}
		
		resStructure = gst_query_get_structure (customQuery);
		if (resStructure == NULL
			|| ! gst_structure_has_name (resStructure, "GstAcmFBDevScreenInfoQuery")) {
			GST_ERROR_OBJECT (me, "query is invalid");
			return FALSE;
		}
		
		if (! gst_structure_get_int (resStructure, "screen_width", &screen_width)) {
			GST_ERROR_OBJECT (me, "failed gst_structure_get_int()");
			return FALSE;
		}
		if (! gst_structure_get_int (resStructure, "screen_height", &screen_height)) {
			GST_ERROR_OBJECT (me, "failed gst_structure_get_int()");
			return FALSE;
		}

		gst_query_unref (customQuery);
		/* gst_structure_free(callStructure); */ /* 必要ない	*/
	} while(FALSE);
	GST_INFO_OBJECT (me, "screen info %d x %d", screen_width, screen_height);

	/* ストライド、オフセットの境界チェック	*/
	if (DEFAULT_FRAME_STRIDE == me->frame_stride) {
		me->frame_stride = me->out_width;
	}
	else if (me->frame_stride < me->out_width) {
		GST_WARNING_OBJECT (me, "stride: %u is less than video width: %u",
							me->frame_stride, me->out_width);
		me->frame_stride = me->out_width;
	}
	if (me->frame_x_offset + me->out_width > me->frame_stride) {
		GST_WARNING_OBJECT (me, "x_offset: %u is illegal", me->frame_x_offset);
		me->frame_x_offset = me->frame_stride - me->out_width;
	}
	/* sink から、スクリーン情報が得られた場合のみ、y_offset のチェックを行う */
	if (screen_height > 0) {
		if (me->frame_y_offset + me->out_height > screen_height) {
			GST_WARNING_OBJECT (me, "y_offset: %u is illegal", me->frame_y_offset);
			me->frame_y_offset = screen_height - me->out_height;
		}
	}

	/* Creates a new output state with the specified fmt, width and height
	 * and Negotiate with downstream elements.
	 */
	g_assert (me->output_state == NULL);
	me->output_state = gst_video_decoder_set_output_state (GST_VIDEO_DECODER (me),
		me->out_video_fmt, me->out_width, me->out_height, me->input_state);
	if (G_UNLIKELY (me->output_state->caps == NULL))
		me->output_state->caps = gst_video_info_to_caps (&(me->output_state->info));
#if 1
	structure = gst_caps_get_structure (me->output_state->caps, 0);
	gst_structure_set (structure,
					   "stride", G_TYPE_INT, me->frame_stride,
					   "x-offset", G_TYPE_INT, me->frame_x_offset,
					   "y-offset", G_TYPE_INT, me->frame_y_offset,
					   NULL);
#endif
	GST_INFO_OBJECT (me,
		"H264DEC OUT FORMAT - info: fmt:%" GST_FOURCC_FORMAT ", %d x %d, %d/%d",
		GST_FOURCC_ARGS (me->input_format),
		me->output_state->info.width, me->output_state->info.height,
		me->output_state->info.fps_n, me->output_state->info.fps_d);
	GST_INFO_OBJECT (me, "H264DEC OUT FORMAT - caps: %" GST_PTR_FORMAT,
					 me->output_state->caps);
	GST_INFO_OBJECT (me, "H264DEC OUT FORMAT - codec_data: %p",
					 me->output_state->codec_data);
	if (! gst_video_decoder_negotiate (GST_VIDEO_DECODER (me))) {
		goto negotiate_failed;
	}

	{
		/* sink からDMABUF情報取得		*/
		GstQuery *customQuery = NULL;
		GstStructure *callStructure;
		const GstStructure *resStructure;
		gint fd = -1;
		guint i;

		for (i = 0; i < NUM_FB_DMABUF; i++) {
			callStructure = gst_structure_new ("GstAcmFBDevDmaBufQuery",
							   "index", G_TYPE_INT, i,
							   "fd", G_TYPE_INT, -1,
								NULL);
			customQuery = gst_query_new_custom(GST_QUERY_CUSTOM, callStructure);
			
			
			if (! gst_pad_peer_query (GST_VIDEO_DECODER_SRC_PAD(me), customQuery)) {
				GST_WARNING_OBJECT (me, "refused dma buf query from peer src pad");
				break;
			}
			
			resStructure = gst_query_get_structure (customQuery);
			if (resStructure == NULL
				|| ! gst_structure_has_name (resStructure, "GstAcmFBDevDmaBufQuery")) {
				GST_ERROR_OBJECT (me, "query is invalid");
				return FALSE;
			}
			
			if (! gst_structure_get_int (resStructure, "fd", &fd)) {
				GST_ERROR_OBJECT (me, "failed gst_structure_get_int()");
				return FALSE;
			}

			gst_query_unref (customQuery);
			/* gst_structure_free(callStructure); */ /* 必要ない	*/
			
			if (-1 != fd) {
				GST_INFO_OBJECT (me, "dmabuf fd[%d] is %d", i, fd);
				me->priv->using_fb_dmabuf = TRUE;
				me->priv->num_fb_dmabuf++;
				me->priv->fb_dmabuf_index[i] = i;
				me->priv->fb_dmabuf_fd[i] = fd;
			}
			else {
				/* non use dma-buf	*/
				break;
			}
		}
	}

	/* デコーダ初期化	*/
	if (! gst_acm_h264_dec_init_decoder(me)) {
		goto init_failed;
	}

out:
	return ret;

	/* ERRORS */
negotiate_failed:
	{
		GST_ELEMENT_ERROR (me, CORE, NEGOTIATION, (NULL),
			("failed src caps negotiate"));
		ret = FALSE;
		goto out;
	}
init_failed:
	{
		GST_ELEMENT_ERROR (me, STREAM, DECODE, (NULL),
			("Failed to init decoder from stream"));
		ret = FALSE;
		goto out;
	}
}

/*
 * Optional. Allows subclass (decoder) to perform post-seek semantics reset.
 */
static gboolean
gst_acm_h264_dec_reset (GstVideoDecoder * dec, gboolean hard)
{
	GstAcmH264Dec *me = GST_ACMH264DEC (dec);

	GST_INFO_OBJECT (me, "H264DEC RESET %s", hard ? "hard" : "soft");

	return TRUE;
}

/*
 * Optional. Called to request subclass to dispatch any pending remaining data 
 * (e.g. at EOS).
 */
static GstFlowReturn
gst_acm_h264_dec_finish (GstVideoDecoder * dec)
{
	GstAcmH264Dec *me = GST_ACMH264DEC (dec);

	GST_INFO_OBJECT (me, "H264DEC FINISH");

	/* do nothing	*/

	return GST_FLOW_OK;
}

static GstFlowReturn
gst_acm_h264_dec_parse (GstVideoDecoder *dec, GstVideoCodecFrame *frame,
					   GstAdapter *adapter, gboolean at_eos)
{
	GstAcmH264Dec *me = GST_ACMH264DEC (dec);
	
	GST_INFO_OBJECT (me, "H264DEC PARSE at_eos:%d", at_eos);
	
	return GST_FLOW_OK;
}

static gboolean
gst_acm_h264_dec_parse_nal(GstAcmH264Dec *me, GstVideoCodecFrame * frame)
{
	GstMapInfo map_parse;
	GstH264ParserResult parse_res;
	GstH264NalUnit nalu;
	const guint nl = me->priv->nal_length_size;
	gboolean isFoundSlice = FALSE;
	GstH264SliceHdr slice;
	gboolean isFrame = FALSE;
	gboolean hasTopField = FALSE;
	gboolean hasBottomField = FALSE;

	gst_buffer_map (frame->input_buffer, &map_parse, GST_MAP_READ);

	parse_res = gst_h264_parser_identify_nalu_avc (me->priv->nalparser,
					map_parse.data, 0, map_parse.size, nl, &nalu);

	while (parse_res == GST_H264_PARSER_OK) {
#if DBG_LOG_INTERLACED
		GST_INFO_OBJECT (me, "processing nal of type %u, offset %d, size %u",
						 nalu.type, nalu.offset, nalu.size);
#endif
		switch (nalu.type) {
		case GST_H264_NAL_SLICE:
		case GST_H264_NAL_SLICE_DPA:
		case GST_H264_NAL_SLICE_DPB:
		case GST_H264_NAL_SLICE_DPC:
		case GST_H264_NAL_SLICE_IDR:
			parse_res = gst_h264_parser_parse_slice_hdr (
							me->priv->nalparser, &nalu, &slice, FALSE, FALSE);
			if (GST_H264_PARSER_OK == parse_res) {
#if DBG_LOG_INTERLACED
				GST_INFO_OBJECT (me, "slice type: %u, frame_num: %d",
								 slice.type, slice.frame_num);
				GST_INFO_OBJECT (me, "field_pic_flag: %d, bottom_field_flag: %d",
								  slice.field_pic_flag, slice.bottom_field_flag);
#endif
				if (0 == slice.field_pic_flag) {
					isFrame = TRUE;
				}
				else {
					if (slice.bottom_field_flag) {
						hasBottomField = TRUE;
					}
					else {
						hasTopField = TRUE;
					}
				}
			}
			isFoundSlice = TRUE;
			break;
		default:
			break;
		}
		
		parse_res = gst_h264_parser_identify_nalu_avc (me->priv->nalparser,
						map_parse.data, nalu.offset + nalu.size, map_parse.size,
						nl, &nalu);
	}
	
	if (! isFoundSlice) {
		GST_WARNING_OBJECT (me, "NOT FOUND SLICE");
	}
	
	/* スライス種別を記録	*/
	if (isFrame) {
#if DBG_LOG_INTERLACED
		GST_INFO_OBJECT (me, "FRAME_FLAG_FRAME");
#endif
		GST_VIDEO_CODEC_FRAME_FLAG_SET(frame, GST_VIDEO_CODEC_FRAME_FLAG_FRAME);
	}
	if (hasTopField && hasBottomField) {
#if DBG_LOG_INTERLACED
		GST_INFO_OBJECT (me, "FRAME_FLAG_TOP_BOTTOM_FIELD");
#endif
		GST_VIDEO_CODEC_FRAME_FLAG_SET(frame,
			GST_VIDEO_CODEC_FRAME_FLAG_TOP_BOTTOM_FIELD);
	}
	else {
		if (hasTopField) {
#if DBG_LOG_INTERLACED
			GST_INFO_OBJECT (me, "FRAME_FLAG_TOP_FIELD");
#endif
			GST_VIDEO_CODEC_FRAME_FLAG_SET(frame,
				GST_VIDEO_CODEC_FRAME_FLAG_TOP_FIELD);
		}
		if (hasBottomField) {
#if DBG_LOG_INTERLACED
			GST_INFO_OBJECT (me, "FRAME_FLAG_BOTTOM_FIELD");
#endif
			GST_VIDEO_CODEC_FRAME_FLAG_SET(frame,
				GST_VIDEO_CODEC_FRAME_FLAG_BOTTOM_FIELD);
		}
	}

	/* フィールド構造であれば frame rate を半分にして down stream に伝える	*/
	if ( (hasTopField && ! hasBottomField) || (! hasTopField && hasBottomField) ) {
		if (! me->priv->is_field_structure) {
			GstVideoCodecState *oldOutputState = me->output_state;
			GST_WARNING_OBJECT (me, "do change frame rate");

			oldOutputState->info.fps_d *= 2;
			me->output_state = gst_video_decoder_set_output_state (
				GST_VIDEO_DECODER (me), me->out_video_fmt,
				me->out_width, me->out_height, oldOutputState);

			gst_video_codec_state_unref (oldOutputState);

			if (! gst_video_decoder_negotiate (GST_VIDEO_DECODER (me))) {
				goto negotiate_failed;
			}

			me->priv->is_field_structure = TRUE;
		}
	}

	gst_buffer_unmap (frame->input_buffer, &map_parse);
	
	return TRUE;

	/* ERRORS */
negotiate_failed:
	{
		GST_ELEMENT_ERROR (me, CORE, NEGOTIATION, (NULL),
			("failed src caps negotiate"));
		return FALSE;
	}
}

static struct v4l2_buffer *get_v4l2buf_in(GstAcmH264Dec *me)
{
	struct v4l2_buffer *v4l2buf_in;

	GST_INFO_OBJECT(me, "acquire_buffer : %d", me->num_inbuf_acquired);
	v4l2buf_in = &(me->priv->input_vbuffer[me->num_inbuf_acquired]);
	me->num_inbuf_acquired++;

	return v4l2buf_in;
}

static GstFlowReturn
gst_acm_h264_dec_handle_frame (GstVideoDecoder * dec,
    GstVideoCodecFrame * frame)
{
	GstAcmH264Dec *me = GST_ACMH264DEC (dec);
	GstFlowReturn ret = GST_FLOW_OK;
	int r = 0;
	int i;
	fd_set write_fds;
	fd_set read_fds;
	struct timeval tv;
	GstBuffer *v4l2buf_out = NULL;
	struct v4l2_buffer* v4l2buf_in = NULL;
	guint32 bytesused = 0;
	gboolean handled_inframe = FALSE;

#if SUPPORT_CODED_FIELD
	if (me->priv->is_interlaced) {
		gst_acm_h264_dec_parse_nal(me, frame);
	}
#endif

	/* first frame */
	if (! me->is_handled_1stframe) {
		if (0 == me->spspps_size) {
			GST_INFO_OBJECT(me, "could not insert SPS/PPS to frame");

			v4l2buf_in = get_v4l2buf_in(me);

			ret = gst_acm_h264_dec_handle_in_frame(me, v4l2buf_in, frame->input_buffer, frame);
			if (GST_FLOW_OK != ret) {
				goto handle_in_failed;
			}
			me->priv->in_out_frame_count++;
		}
		else {
			GstMapInfo spspps_map;
			GstMemory *spspps_mem;

			/* SPS/PPS の挿入 */
			GST_INFO_OBJECT(me, "insert SPS/PPS to frame");

			spspps_mem = gst_allocator_alloc(NULL, me->spspps_size, NULL);
			if (NULL == spspps_mem) {
				GST_ELEMENT_ERROR(me, STREAM, DECODE, (NULL),
					("no mem with gst_allocator_alloc()"));
				ret = GST_FLOW_ERROR;
			}
			gst_memory_map(spspps_mem, &spspps_map, GST_MAP_WRITE);
			memcpy(spspps_map.data, me->spspps, me->spspps_size);
			gst_memory_unmap(spspps_mem, &spspps_map);

			frame->input_buffer = gst_buffer_make_writable(frame->input_buffer);
			gst_buffer_prepend_memory(frame->input_buffer, spspps_mem);

			/* 初回の入力		*/
			v4l2buf_in = get_v4l2buf_in(me);

			ret = gst_acm_h264_dec_handle_in_frame(me, v4l2buf_in, frame->input_buffer, frame);
			if (GST_FLOW_OK != ret) {
				goto handle_in_failed;
			}
			me->priv->in_out_frame_count++;
		}

		me->is_handled_1stframe = TRUE;
		goto out;
	}


	if (me->num_inbuf_acquired < DEFAULT_NUM_BUFFERS_IN) {
		v4l2buf_in = get_v4l2buf_in(me);
		ret = gst_acm_h264_dec_handle_in_frame(me, v4l2buf_in, frame->input_buffer, frame);
		if (GST_FLOW_OK != ret) {
			goto handle_in_failed;
		}
		goto out;
	}

	while(handled_inframe != TRUE){

		FD_ZERO(&read_fds);
		FD_ZERO(&write_fds);
		FD_SET(me->video_fd, &read_fds);
		FD_SET(me->video_fd, &write_fds);
		tv.tv_sec = 10;
		tv.tv_usec = 0;
		r = select(me->video_fd +1, &read_fds, &write_fds, NULL, &tv);

		if (r == -1 && (errno == EINTR || errno == EAGAIN)) {
			continue;
		}
		else if (r < 0) {
			goto select_failed;
		}
		else if (r == 0) {
			GST_INFO_OBJECT(me, "select() timeout");
			goto select_timeout;
		}

		if (FD_ISSET(me->video_fd, &write_fds)){

			v4l2buf_in = &(me->priv->input_vbuffer[0]);
			memset (v4l2buf_in, 0x00, sizeof (struct v4l2_buffer));
			v4l2buf_in->type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
			v4l2buf_in->memory = V4L2_MEMORY_USERPTR;
			r = gst_acm_v4l2_ioctl (me->video_fd, VIDIOC_DQBUF, v4l2buf_in);
			if (r < 0) {
				goto dqbuf_failed;
			}

			ret = gst_acm_h264_dec_handle_in_frame(me, v4l2buf_in, frame->input_buffer, frame);
			if (GST_FLOW_OK != ret) {
				goto handle_in_failed;
			}
			me->priv->in_out_frame_count++;
			handled_inframe = TRUE;
		}

		if (FD_ISSET(me->video_fd, &read_fds)){

			for (i = 0; i < NUM_HANDLE_OUTBUF; i++) {

				ret = gst_acm_v4l2_buffer_pool_dqbuf_ex(me->pool_out,&v4l2buf_out, &bytesused);
				if (GST_FLOW_DQBUF_EAGAIN == ret) {
					/* decoded frame is not available. do it later */
					ret = GST_FLOW_OK;
					continue;
				}
				else if (GST_FLOW_OK != ret) {
					goto dqbuf_failed;
				}

				/* H.264のMMCO(Memory Management Control Operation)の機能で、
				 * DPB(Decoded Picture Buffer)から削除される場合がある。
				 * この際、出力不可フラグが設定され、bytesused がゼロになる。
				 * この出力は、down stream に流さず、無視する。
				 */
				if (0 == bytesused) {
					GST_WARNING_OBJECT(me, "drop frame by bytesused(0)");
					gst_acm_v4l2_buffer_pool_qbuf(me->pool_out,
						v4l2buf_out, gst_buffer_get_size(v4l2buf_out));
					continue;
				}
				me->priv->in_out_frame_count--;

				ret = gst_acm_h264_dec_handle_out_frame(me, v4l2buf_out, NULL);
				if (GST_FLOW_OK != ret) {
					if (GST_FLOW_FLUSHING == ret) {
						GST_DEBUG_OBJECT(me, "FLUSHING - continue.");

						/* エラーとせず、m2mデバイスへのデータ入力は行う	*/
					}
					else {
						goto handle_out_failed;
					}
				}
			}
		}
	}

out:
	return ret;
	
	/* ERRORS */
select_failed:
	{
		GST_ELEMENT_ERROR (me, STREAM, DECODE, (NULL),
			("error with select() %d (%s)", errno, g_strerror (errno)));
		ret = GST_FLOW_ERROR;
		goto out;
	}
select_timeout:
	{
		GST_ERROR_OBJECT (me, "pool_out - buffers:%d, allocated:%d, queued:%d",
						  me->pool_out->num_buffers,
						  me->pool_out->num_allocated,
						  me->pool_out->num_queued);

		gst_acm_v4l2_buffer_pool_log_buf_status(me->pool_out);
		
		log_buf_status_of_input(me);

		GST_ELEMENT_ERROR (me, STREAM, DECODE, (NULL),
			("timeout with select()"));
		ret = GST_FLOW_ERROR;
		goto out;
	}
dqbuf_failed:
	{
		GST_ELEMENT_ERROR (me, STREAM, DECODE, (NULL),
			("could not dequeue buffer. %d (%s)", errno, g_strerror (errno)));
		ret = GST_FLOW_ERROR;
		goto out;
	}
handle_in_failed:
	{
		GST_ELEMENT_ERROR (me, STREAM, DECODE, (NULL),
			("failed handle in"));
		ret = GST_FLOW_ERROR;
		goto out;
	}
handle_out_failed:
	{
		if (GST_FLOW_NOT_LINKED == ret || GST_FLOW_FLUSHING == ret) {
			GST_WARNING_OBJECT (me, "failed handle out - not link or flushing");
			goto out;
		}

		GST_ELEMENT_ERROR (me, STREAM, DECODE, (NULL),
			("failed handle out"));
		ret = GST_FLOW_ERROR;
		goto out;
	}
}

static gboolean
gst_acm_h264_dec_sink_event (GstVideoDecoder * dec, GstEvent *event)
{
	GstAcmH264Dec *me = GST_ACMH264DEC (dec);
	gboolean ret = FALSE;
	GstBuffer* eosBuffer = NULL;

	GST_DEBUG_OBJECT (me, "RECEIVED EVENT (%d)", GST_EVENT_TYPE(event));
	switch (GST_EVENT_TYPE(event)) {
	case GST_EVENT_CAPS:
	{
		GstCaps * caps = NULL;
		
		gst_event_parse_caps (event, &caps);
		GST_INFO_OBJECT (me, "H264DEC received GST_EVENT_CAPS: %" GST_PTR_FORMAT,
						 caps);
		
		ret = GST_VIDEO_DECODER_CLASS (parent_class)->sink_event(dec, event);
		break;
	}
	case GST_EVENT_EOS:
	{
		int r = 0;
		fd_set read_fds;
		fd_set write_fds;
		struct timeval tv;
		gboolean isEOS = FALSE;
		GstVideoCodecFrame *frame = NULL;
		GstMapInfo map;
		GstBuffer *v4l2buf_out = NULL;
		struct v4l2_buffer* v4l2buf_in = NULL;
		guint32 bytesused = 0;

		GST_INFO_OBJECT (me, "H264DEC received GST_EVENT_EOS");

		/* EOS の際は、0x00, 0x00, 0x00, 0x01, 0x0B を enqueue する */
		GST_INFO_OBJECT(me, "Enqueue EOS buffer.");
		
		eosBuffer = gst_buffer_new_allocate(NULL, 5, NULL);
		gst_buffer_map (eosBuffer, &map, GST_MAP_READ);
		
		map.data[0] = 0x00;
		map.data[1] = 0x00;
		map.data[2] = 0x00;
		map.data[3] = 0x01;
		map.data[4] = 0x0B;
		
		gst_buffer_unmap (eosBuffer, &map);
		
		do {
			FD_ZERO(&write_fds);
			FD_SET(me->video_fd, &write_fds);
			tv.tv_sec = 0;
			tv.tv_usec = SELECT_TIMEOUT_MSEC * 1000;
			r = select(me->video_fd + 1, NULL, &write_fds, NULL, &tv);
			GST_DEBUG_OBJECT(me, "After select for write. r=%d", r);
		} while (r == -1 && (errno == EINTR || errno == EAGAIN));
		if (r > 0 /* && FD_ISSET(me->video_fd, &write_fds) */) {
			v4l2buf_in = &(me->priv->input_vbuffer[0]);
			memset (v4l2buf_in, 0x00, sizeof (struct v4l2_buffer));
			v4l2buf_in->type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
			v4l2buf_in->memory = V4L2_MEMORY_USERPTR;
			r = gst_acm_v4l2_ioctl (me->video_fd, VIDIOC_DQBUF, v4l2buf_in);
			if (r < 0) {
				goto dqbuf_failed;
			}

			ret = gst_acm_h264_dec_handle_in_frame(me, v4l2buf_in, eosBuffer, NULL);
			
			if (GST_FLOW_OK != ret) {
				goto handle_in_failed;
			}
		}
		else if (r < 0) {
			goto select_failed;
		}
		else if (0 == r) {
			/* timeoutしたらエラー	*/
			GST_INFO_OBJECT(me, "select() eos buf timeout");
			goto select_timeout;
		}

		/* デバイス側に溜まっているデータを取り出して、down stream へ流す	*/
		GST_INFO_OBJECT(me, "in_out_frame_count : %d",
						me->priv->in_out_frame_count);
		while (NULL != (frame = gst_video_decoder_get_oldest_frame(
									GST_VIDEO_DECODER (me)))) {
			gst_video_codec_frame_unref(frame);

			do {
				do {
					FD_ZERO(&read_fds);
					FD_SET(me->video_fd, &read_fds);
					tv.tv_sec = 0;
					tv.tv_usec = SELECT_TIMEOUT_MSEC * 1000;
					r = select(me->video_fd + 1, &read_fds, NULL, NULL, &tv);
					GST_DEBUG_OBJECT(me, "After select for read. r=%d", r);
				} while (r == -1 && (errno == EINTR || errno == EAGAIN));
				if (r < 0) {
					goto select_failed;
				}
				else if (0 == r) {
					/* timeoutしたらエラー	*/
					GST_INFO_OBJECT(me, "select() for output is timeout");
					goto select_timeout;
				}

				/* dequeue buffer	*/
				ret = gst_acm_v4l2_buffer_pool_dqbuf_ex (me->pool_out,
						&v4l2buf_out, &bytesused);
				if (GST_FLOW_OK != ret) {
					if (GST_FLOW_DQBUF_EAGAIN == ret) {
//						GST_WARNING_OBJECT (me, "DQBUF_EAGAIN after EOS");
						break;
					}
					
					GST_ERROR_OBJECT (me, "gst_acm_v4l2_buffer_pool_dqbuf() returns %s",
									  gst_flow_get_name (ret));
					goto dqbuf_failed;
				}

				/* H.264のMMCO(Memory Management Control Operation)の機能で、
				 * DPB(Decoded Picture Buffer)から削除される場合がある。
				 * この際、出力不可フラグが設定され、bytesused がゼロになる。
				 * この出力は、down stream に流さず、無視する。
				 */
				if (0 == bytesused) {
					GST_WARNING_OBJECT(me, "drop frame(3) by bytesused(0) at %d",
						frame->system_frame_number);
					gst_acm_v4l2_buffer_pool_qbuf(me->pool_out,
						v4l2buf_out, gst_buffer_get_size(v4l2buf_out));
					
					continue;
				}

				me->priv->in_out_frame_count--;
				ret = gst_acm_h264_dec_handle_out_frame(me, v4l2buf_out, &isEOS);
				if (GST_FLOW_OK != ret) {
					goto handle_out_failed;
				}
				if (isEOS) {
					break;
				}
			} while (FALSE);
		}

		/* Seek した場合に、m2mデバイス側にデコード済みデータが残ってしまい、
		 * これを取り出しきらないと、クリーンアップ時、VIDIOC_STREAMOFF が 
		 * timeout でエラーしてしまう。
		 */
		GST_INFO_OBJECT(me, "in_out_frame_count : %d",
						me->priv->in_out_frame_count);
		while (
			   me->priv->in_out_frame_count > 0
			   ) {
			do {
				FD_ZERO(&read_fds);
				FD_SET(me->video_fd, &read_fds);
				tv.tv_sec = 0;
				tv.tv_usec = SELECT_TIMEOUT_MSEC * 1000;
				r = select(me->video_fd + 1, &read_fds, NULL, NULL, &tv);
				GST_DEBUG_OBJECT(me, "After select for read(EOS). r=%d", r);
			} while (r == -1 && (errno == EINTR || errno == EAGAIN));
			if (r > 0 /* && FD_ISSET(me->video_fd, &read_fds) */) {
				ret = gst_acm_v4l2_buffer_pool_dqbuf (me->pool_out, &v4l2buf_out);
				if (GST_FLOW_OK != ret) {
					goto dqbuf_failed;
				}
				ret = gst_acm_v4l2_buffer_pool_qbuf(me->pool_out, v4l2buf_out,
						gst_buffer_get_size(v4l2buf_out));
				if (GST_FLOW_OK != ret) {
					goto qbuf_failed;
				}
				me->priv->in_out_frame_count--;
			}
			else if (r < 0) {
				goto select_failed;
			}
			else if (0 == r) {
				/* timeoutしたらエラー	*/
				GST_INFO_OBJECT(me, "select() out timeout");
				goto select_timeout;
			}
		}

		ret = GST_VIDEO_DECODER_CLASS (parent_class)->sink_event(dec, event);
		break;
	}
	case GST_EVENT_STREAM_START:
		GST_DEBUG_OBJECT (me, "received GST_EVENT_STREAM_START");
		/* break;	*/
	case GST_EVENT_SEGMENT:
		GST_DEBUG_OBJECT (me, "received GST_EVENT_SEGMENT");
		/* break;	*/
	default:
		ret = GST_VIDEO_DECODER_CLASS (parent_class)->sink_event(dec, event);
		break;
	}
	
out:
	if(eosBuffer)
		gst_buffer_unref(eosBuffer);
	return ret;
	
select_failed:
	{
		GST_ELEMENT_ERROR (me, STREAM, DECODE, (NULL),
			("error with select() %d (%s)", errno, g_strerror (errno)));
		ret = FALSE;
		goto out;
	}
select_timeout:
	{
		GST_ERROR_OBJECT (me, "pool_out - buffers:%d, allocated:%d, queued:%d",
						  me->pool_out->num_buffers,
						  me->pool_out->num_allocated,
						  me->pool_out->num_queued);
		GST_ELEMENT_ERROR (me, STREAM, DECODE, (NULL),
			("timeout with select()"));
		ret = FALSE;
		goto out;
	}
handle_out_failed:
	{
		if (GST_FLOW_NOT_LINKED == ret || GST_FLOW_FLUSHING == ret) {
			GST_WARNING_OBJECT (me, "failed handle out - not link or flushing");
			goto out;
		}

		GST_ELEMENT_ERROR (me, STREAM, DECODE, (NULL),
			("failed handle out"));
		ret = FALSE;
		goto out;
	}
handle_in_failed:
	{
		GST_ELEMENT_ERROR (me, STREAM, DECODE, (NULL),
			("failed handle in"));
		ret = FALSE;
		goto out;
	}
qbuf_failed:
	{
		GST_ELEMENT_ERROR (me, STREAM, DECODE, (NULL),
			("could not queue buffer. %d (%s)", errno, g_strerror (errno)));
		ret = FALSE;
	   goto out;
	}
dqbuf_failed:
	{
		GST_ELEMENT_ERROR (me, STREAM, DECODE, (NULL),
			("could not dequeue buffer. %d (%s)", errno, g_strerror (errno)));
		ret = FALSE;
		goto out;
	}
}

static gboolean
gst_acm_h264_dec_init_decoder (GstAcmH264Dec * me)
{
	gboolean ret = TRUE;
	enum v4l2_buf_type type;
	int r;
	GstCaps *srcCaps;
	GstAcmV4l2InitParam v4l2InitParam;
	struct v4l2_format fmt;
	struct v4l2_control ctrl;
	gint i = 0;
	guint bytesperline = 0;
	guint offset = 0;

	GST_INFO_OBJECT (me, "H264DEC INITIALIZE ACM DECODER...");

	/* 入力バッファサイズ	*/
	guint in_frame_size = me->width * me->height * 3;
	/* 出力バッファサイズ
	 * 再生途中で表示画像サイズを変えられる事を考慮し、出力画像サイズではなく、
	 * 入力画像の画素数を元に計算する
	 */
	guint out_frame_size = 0;
	switch (me->output_format) {
	case GST_ACMH264DEC_OUT_FMT_YUV420:
		out_frame_size = me->width * me->height * 2;
		break;
	case GST_ACMH264DEC_OUT_FMT_RGB32:
		out_frame_size = me->width * me->height * 4;
		break;
	case GST_ACMH264DEC_OUT_FMT_RGB24:
		out_frame_size = me->width * me->height * 3;
		break;
	case GST_ACMH264DEC_OUT_FMT_RGB565:
		out_frame_size = me->width * me->height * 2;
		break;
	default:
		g_assert_not_reached ();
		break;
	}
	GST_INFO_OBJECT (me, "in_frame_size:%u, out_frame_size:%u",
					 in_frame_size, out_frame_size);

	/* ストライド、オフセット	*/
	switch (me->output_format) {
	case GST_ACMH264DEC_OUT_FMT_YUV420:
		bytesperline = me->frame_stride * 2;
		offset = (me->frame_y_offset * bytesperline) + (me->frame_x_offset * 2);
		break;
	case GST_ACMH264DEC_OUT_FMT_RGB32:
		bytesperline = me->frame_stride * 4;
		offset = (me->frame_y_offset * bytesperline) + (me->frame_x_offset * 4);
		break;
	case GST_ACMH264DEC_OUT_FMT_RGB24:
		bytesperline = me->frame_stride * 3;
		offset = (me->frame_y_offset * bytesperline) + (me->frame_x_offset * 3);
		break;
	case GST_ACMH264DEC_OUT_FMT_RGB565:
		bytesperline = me->frame_stride * 2;
		offset = (me->frame_y_offset * bytesperline) + (me->frame_x_offset * 2);
		break;
	default:
		g_assert_not_reached ();
		break;
	}

	/* デコード初期化パラメータセット		*/
	GST_INFO_OBJECT (me, "H264DEC INIT PARAM:");
	GST_INFO_OBJECT (me, " buffering_pic_cnt:%u", me->buffering_pic_cnt);
	GST_INFO_OBJECT (me, " enable_vio6:%u", me->enable_vio6);
	GST_INFO_OBJECT (me, " frame_rate:%u", me->frame_rate);
	GST_INFO_OBJECT (me, " x_pic_size:%u", me->width);
	GST_INFO_OBJECT (me, " y_pic_size:%u", me->height);
	GST_INFO_OBJECT (me, " x_out_size:%u", me->out_width);
	GST_INFO_OBJECT (me, " y_out_size:%u", me->out_height);
	GST_INFO_OBJECT (me, " stride:%u", me->frame_stride);
	GST_INFO_OBJECT (me, " bytesperline:%u", bytesperline);
	GST_INFO_OBJECT (me, " x_offset:%u", me->frame_x_offset);
	GST_INFO_OBJECT (me, " y_offset:%u", me->frame_y_offset);
	GST_INFO_OBJECT (me, " offset:%u", offset);
	GST_INFO_OBJECT (me, " input_format:%" GST_FOURCC_FORMAT,
					 GST_FOURCC_ARGS (me->input_format));
	GST_INFO_OBJECT (me, " output_format:%" GST_FOURCC_FORMAT,
					 GST_FOURCC_ARGS (me->output_format));
	/* buffering_pic_cnt */
	ctrl.id = V4L2_CID_NR_BUFFERING_PICS;
	ctrl.value = me->buffering_pic_cnt;
	r = gst_acm_v4l2_ioctl(me->video_fd, VIDIOC_S_CTRL, &ctrl);
	if (r < 0) {
		goto set_init_param_failed;
	}
	/* VIO6の有効無効フラグ */
	ctrl.id = V4L2_CID_ENABLE_VIO;
	ctrl.value = me->enable_vio6;
	r = gst_acm_v4l2_ioctl(me->video_fd, VIDIOC_S_CTRL, &ctrl);
	if (r < 0) {
		goto set_init_param_failed;
	}
	/* フレームレート */
	ctrl.id = V4L2_CID_FRAME_RATE;
	ctrl.value = me->frame_rate;
	r = gst_acm_v4l2_ioctl(me->video_fd, VIDIOC_S_CTRL, &ctrl);
	if (r < 0) {
		goto set_init_param_failed;
	}

	/* Set format for output (decoder input) */
	fmt.type 			= V4L2_BUF_TYPE_VIDEO_OUTPUT;
	fmt.fmt.pix.width	= me->width;
	fmt.fmt.pix.height	= me->height;
	fmt.fmt.pix.pixelformat = me->input_format;
	fmt.fmt.pix.field	= V4L2_FIELD_NONE;
	fmt.fmt.pix.bytesperline = bytesperline;
	fmt.fmt.pix.priv	= offset;

	r = gst_acm_v4l2_ioctl(me->video_fd, VIDIOC_S_FMT, &fmt);
	if (r < 0) {
		goto set_init_param_failed;
	}

	/* Set format for capture (decoder output) */
	fmt.type			= V4L2_BUF_TYPE_VIDEO_CAPTURE;
	fmt.fmt.pix.width	= me->out_width;
	fmt.fmt.pix.height	= me->out_height;
	fmt.fmt.pix.pixelformat = me->output_format;
	fmt.fmt.pix.field	= V4L2_FIELD_NONE;
	fmt.fmt.pix.bytesperline = bytesperline;
	fmt.fmt.pix.priv	= offset;

	r = gst_acm_v4l2_ioctl(me->video_fd, VIDIOC_S_FMT, &fmt);
	if (r < 0) {
		goto set_init_param_failed;
	}

	/* バッファプールのセットアップ	*/
	{
		struct v4l2_requestbuffers breq;

		memset (&breq, 0, sizeof (struct v4l2_requestbuffers));
		breq.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
		breq.count = DEFAULT_NUM_BUFFERS_IN;
		breq.memory = V4L2_MEMORY_USERPTR;

		if (gst_acm_v4l2_ioctl (me->video_fd, VIDIOC_REQBUFS, &breq) < 0) {
			goto reqbufs_failed;
		}
		GST_INFO_OBJECT (me, "VIDIOC_REQBUFS");
		GST_INFO_OBJECT (me, " count:  %u", breq.count);
		GST_INFO_OBJECT (me, " type:   %d", breq.type);
		GST_INFO_OBJECT (me, " memory: %d", breq.memory);
		if (DEFAULT_NUM_BUFFERS_IN != breq.count) {
			GST_ERROR_OBJECT (me, "ONLY %u BUFFERS ALLOCATED", breq.count);
			
			goto reqbufs_failed;
		}

		for (i = 0; i < DEFAULT_NUM_BUFFERS_IN; i++) {
			memset(&(me->priv->input_vbuffer[i]), 0, sizeof(struct v4l2_buffer));
			
			GST_INFO_OBJECT (me, "VIDIOC_QUERYBUF");
			me->priv->input_vbuffer[i].index	= i;
			me->priv->input_vbuffer[i].type	= V4L2_BUF_TYPE_VIDEO_OUTPUT;
			me->priv->input_vbuffer[i].memory = V4L2_MEMORY_USERPTR;
			r = gst_acm_v4l2_ioctl (me->video_fd,
							VIDIOC_QUERYBUF, &(me->priv->input_vbuffer[i]));
			if (r < 0) {
				goto  querybuf_failed;
			}
			GST_INFO_OBJECT (me, "  index:     %u",
							 me->priv->input_vbuffer[i].index);
			GST_INFO_OBJECT (me, "  bytesused: %u",
							 me->priv->input_vbuffer[i].bytesused);
			GST_INFO_OBJECT (me, "  flags:     %08x",
							 me->priv->input_vbuffer[i].flags);
			GST_INFO_OBJECT (me, "  length:    %u",
							 me->priv->input_vbuffer[i].length);
		}
	}
	
	if (NULL == me->pool_out) {
		memset(&v4l2InitParam, 0, sizeof(GstAcmV4l2InitParam));
		if (me->priv->using_fb_dmabuf) {
			v4l2InitParam.video_fd = me->video_fd;
			v4l2InitParam.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
			v4l2InitParam.mode = GST_ACM_V4L2_IO_DMABUF;
			v4l2InitParam.sizeimage = out_frame_size;
			v4l2InitParam.init_num_buffers = DEFAULT_NUM_BUFFERS_OUT_DMABUF;
			
			v4l2InitParam.num_fb_dmabuf = me->priv->num_fb_dmabuf;
			for (i = 0; i < me->priv->num_fb_dmabuf; i++) {
				v4l2InitParam.fb_dmabuf_index[i] = me->priv->fb_dmabuf_index[i];
				v4l2InitParam.fb_dmabuf_fd[i] = me->priv->fb_dmabuf_fd[i];
			}
		}
		else {
			v4l2InitParam.video_fd = me->video_fd;
			v4l2InitParam.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
			v4l2InitParam.mode = GST_ACM_V4L2_IO_MMAP;
			v4l2InitParam.sizeimage = out_frame_size;
			v4l2InitParam.init_num_buffers = DEFAULT_NUM_BUFFERS_OUT;
		}
		srcCaps = gst_caps_from_string ("video/x-raw");
		me->pool_out = gst_acm_v4l2_buffer_pool_new(&v4l2InitParam, srcCaps);
		gst_caps_unref(srcCaps);
		if (! me->pool_out) {
			goto buffer_pool_new_failed;
		}
		if (1 == me->pool_out->num_buffers) {
			/* バッファが 1つしか確保できない場合は動作しない */
			goto buffer_pool_new_failed;
		}
	}
	
	/* and activate */
	gst_buffer_pool_set_active (GST_BUFFER_POOL_CAST(me->pool_out), TRUE);

	GST_INFO_OBJECT (me, "pool_out - buffers:%d, allocated:%d, queued:%d",
					  me->pool_out->num_buffers,
					  me->pool_out->num_allocated,
					  me->pool_out->num_queued);
	
	/* STREAMON */
	GST_INFO_OBJECT (me, "H264DEC STREAMON");
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
	
out:
	return ret;
	
	/* ERRORS */
set_init_param_failed:
	{
		GST_ELEMENT_ERROR (me, STREAM, DECODE, (NULL),
			("Failed to set decoder init param.(%s)", g_strerror (errno)));
		ret = FALSE;
		goto out;
	}
reqbufs_failed:
	{
		GST_ELEMENT_ERROR (me, STREAM, DECODE, (NULL),
			("error requesting %d buffers: %s",
			DEFAULT_NUM_BUFFERS_IN, g_strerror (errno)));
		ret = FALSE;
		goto out;
	}
querybuf_failed:
	{
		GST_ELEMENT_ERROR (me, STREAM, DECODE, (NULL),
			("Failed QUERYBUF: %s", g_strerror (errno)));
		ret = FALSE;
		goto out;
	}
buffer_pool_new_failed:
	{
		GST_ELEMENT_ERROR (me, STREAM, DECODE, (NULL),
			("Could not map buffers from device '%s'", me->videodev));
		ret = FALSE;
		goto out;
	}
start_failed:
	{
		GST_ELEMENT_ERROR (me, STREAM, DECODE, (NULL),
			("error with STREAMON %d (%s)", errno, g_strerror (errno)));
		ret = FALSE;
		goto out;
	}
}

static gboolean
gst_acm_h264_dec_cleanup_decoder (GstAcmH264Dec * me)
{
	enum v4l2_buf_type type;
	int r = 0;
	
	GST_INFO_OBJECT (me, "H264DEC CLEANUP ACM DECODER...");
	
	/* バッファプールのクリーンアップ	*/
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
	GST_INFO_OBJECT (me, "H264DEC STREAMOFF");
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

	return TRUE;
	
	/* ERRORS */
stop_failed:
	{
		GST_ELEMENT_ERROR (me, STREAM, DECODE, (NULL),
			("error with STREAMOFF %d (%s)", errno, g_strerror (errno)));
		return FALSE;
	}
}

static void
gst_acm_h264_dec_user_data_free(GstMemory * mem)
{
	if (mem) {
		GST_DEBUG("Releasing memory %p", mem);
		gst_memory_unref(mem);
	}
}

static GstFlowReturn
gst_acm_h264_dec_handle_in_frame(GstAcmH264Dec * me,
	struct v4l2_buffer* v4l2buf_in, GstBuffer *inbuf, GstVideoCodecFrame * frame)
{
	GstFlowReturn ret = GST_FLOW_OK;
	GstMapInfo map;
	GstMemory * merged_mem;
	gboolean handle_single_mem = (gst_buffer_n_memory(inbuf) == 1);
	int r;

	GST_DEBUG_OBJECT(me, "inbuf size=%" G_GSIZE_FORMAT, gst_buffer_get_size(inbuf));

	/* 入力データを設定	*/
	if (handle_single_mem) {
		gst_buffer_map(inbuf, &map, GST_MAP_READ);
	} else {
		if (!frame) {
			GST_ERROR_OBJECT (me, "inbuf contains multiple frames, but frame is NULL");
			goto qbuf_failed;
		}

		/* merged_mem will be released via _gst_video_codec_frame_free */
		merged_mem = gst_buffer_get_all_memory(inbuf);
		gst_memory_map(merged_mem, &map, GST_MAP_READ);
		gst_video_codec_frame_set_user_data(frame, merged_mem,
				(GDestroyNotify) gst_acm_h264_dec_user_data_free);
	}

	/* 入力データサイズを設定		*/
	v4l2buf_in->bytesused = map.size;
	v4l2buf_in->length = map.size;
	
	/* 入力データを設定		*/
	v4l2buf_in->m.userptr = (unsigned long)map.data;
	
	/* enqueue buffer	*/
	r = gst_acm_v4l2_ioctl (me->video_fd, VIDIOC_QBUF, v4l2buf_in);
	if (handle_single_mem)
		gst_buffer_unmap(inbuf, &map);
	else
		gst_memory_unmap(merged_mem, &map);

	if (r < 0) {
		goto qbuf_failed;
	}

out:
	return ret;
	
	/* ERRORS */
qbuf_failed:
	{
		GST_ELEMENT_ERROR (me, STREAM, DECODE, (NULL),
			("could not queue buffer %d (%s)", errno, g_strerror (errno)));
		ret = GST_FLOW_ERROR;
		goto out;
	}
}

static GstFlowReturn
gst_acm_h264_dec_handle_out_frame(GstAcmH264Dec * me,
	GstBuffer *v4l2buf_out, gboolean* is_eos)
{
	GstFlowReturn ret = GST_FLOW_OK;
#if DO_PUSH_POOLS_BUF
	GstBufferPoolAcquireParams acquireParam;
#else
	GstMapInfo map;
#endif
#if DO_FRAME_DROP
	GstClockTimeDiff deadline;
#endif
	GstVideoCodecFrame *frame = NULL;

	/* 出力引数初期化	*/
	if (NULL != is_eos) {
		*is_eos = FALSE;
	}

	GST_DEBUG_OBJECT(me, "v4l2buf_out size=%" G_GSIZE_FORMAT, gst_buffer_get_size(v4l2buf_out));

	if (! me->priv->using_fb_dmabuf) {
		/* check EOS	*/
		if (0 == gst_buffer_get_size(v4l2buf_out)) {
			/* もう、デバイス側に、デコードデータは無い	*/
			GST_INFO_OBJECT(me, "all decoded on the device");
			
			if (NULL != is_eos) {
				*is_eos = TRUE;
			}
			
			return ret;
		}
	}

	/* dequeue frame	*/
	frame = gst_video_decoder_get_oldest_frame(GST_VIDEO_DECODER (me));
	if (! frame) {
		goto no_frame;
	}
	gst_video_codec_frame_unref(frame);

#if SUPPORT_CODED_FIELD
#if DBG_LOG_INTERLACED
	GST_INFO_OBJECT(me, "handle_out_frame (%u)", frame->system_frame_number);
#endif
	if (me->priv->is_interlaced) {
		if (GST_VIDEO_CODEC_FRAME_FLAG_IS_SET(frame,
				GST_VIDEO_CODEC_FRAME_FLAG_TOP_FIELD)
			|| GST_VIDEO_CODEC_FRAME_FLAG_IS_SET(frame,
				GST_VIDEO_CODEC_FRAME_FLAG_BOTTOM_FIELD)) {
				/* 2入力 1出力なので、drop する		*/
				GST_INFO_OBJECT(me, "drop frame by field structre (%u)",
								frame->system_frame_number);
				gst_video_decoder_drop_frame (GST_VIDEO_DECODER (me), frame);

				/* 次のフレームを取得	*/
				frame = gst_video_decoder_get_oldest_frame(GST_VIDEO_DECODER (me));
				if (! frame) {
					goto no_frame;
				}
				gst_video_codec_frame_unref(frame);

				me->priv->in_out_frame_count--;
			}
	}
#endif

#if DO_PUSH_POOLS_BUF

#if DO_FRAME_DROP
	deadline = gst_video_decoder_get_max_decode_time (GST_VIDEO_DECODER (me), frame);
//	GST_INFO_OBJECT(me, "deadline : %" GST_TIME_FORMAT, GST_TIME_ARGS (deadline));
    if (deadline < 0) {
		GST_WARNING_OBJECT (me, "Skipping late frame (%f s past deadline)",
						 (double) -deadline / GST_SECOND);
		frame->output_buffer = v4l2buf_out;
		gst_video_decoder_drop_frame (GST_VIDEO_DECODER (me), frame);
    }
	else
#endif
	{
		GstBuffer* displayingBuf = NULL;

		GST_DEBUG_OBJECT(me, "outbuf size=%" G_GSIZE_FORMAT ", ref:%d",
				 gst_buffer_get_size(v4l2buf_out),
				 GST_OBJECT_REFCOUNT_VALUE(v4l2buf_out));

		GST_DEBUG_OBJECT(me, "H264DEC FINISH FRAME:%p", v4l2buf_out);
		GST_DEBUG_OBJECT (me, "pool_out->num_queued is %d",
						  me->pool_out->num_queued);

		/* gst_buffer_unref() により、デバイスに、QBUF されるようにする。
		 * output_buffer->pool に、me->pool_out を直接セットせず、
		 * GstBufferPool::priv::outstanding をインクリメントするために、
		 * gst_buffer_pool_acquire_buffer() を call する。
		 * そうしないと、pool の deactivate の際、outstanding == 0 でないと、
		 * buffer の解放が行われないため
		 */
		acquireParam.flags = GST_BUFFER_POOL_ACQUIRE_FLAG_LAST;
		ret = gst_buffer_pool_acquire_buffer(GST_BUFFER_POOL_CAST(me->pool_out),
											 &v4l2buf_out, &acquireParam);
		if (GST_FLOW_OK != ret) {
			GST_ERROR_OBJECT (me, "gst_buffer_pool_acquire_buffer() returns %s",
							  gst_flow_get_name (ret));
			goto acquire_buffer_failed;
		}

		GST_DEBUG_OBJECT(me,
			 "outbuf->pool : %p (ref:%d), pool_out : %p (ref:%d)",
			 v4l2buf_out->pool, GST_OBJECT_REFCOUNT_VALUE(v4l2buf_out->pool),
			 me->pool_out, GST_OBJECT_REFCOUNT_VALUE(me->pool_out));
		
		GST_DEBUG_OBJECT(me, "frame->ref_count :%d", frame->ref_count);

		/* down stream へ push するバッファの生成	*/
		if (GST_VIDEO_DECODER(me)->input_segment.rate > 0.0) {
			frame->output_buffer = v4l2buf_out;
		}
		else {
			/* 巻き戻し再生の際は、GstVideoDecoder 側でキューイングされるので、
			 * バッファを allocate しないと、デバイスの CAPTURE 側キューが枯渇する
			 */
			frame->output_buffer = gst_buffer_copy(v4l2buf_out);
			if (NULL == frame->output_buffer) {
				goto allocate_outbuf_failed;
			}

			if (me->priv->using_fb_dmabuf) {
				/* デバイスへ戻す	*/
				gst_buffer_unref(v4l2buf_out);
				v4l2buf_out = NULL;
			}
		}

		/* down stream へ バッファを push	*/
		if (! me->priv->using_fb_dmabuf) {
			if (GST_VIDEO_DECODER(me)->input_segment.rate > 0.0) {
				displayingBuf = gst_buffer_ref(v4l2buf_out);
			}
			else {
				displayingBuf = v4l2buf_out;
			}
		}

#if 1	/* 2013-06-05 */
		/* Bピクチャを含む場合、demux より入力されたバッファは、DTS順であり、PTS順とは異なる
		 * HWデコーダのVCP1はBピクチャのリオーダリングをした後出力しているので、出力結果の
		 * 並べ替えは不要。PTSをDTSで置き換える事で送出バッファのタイムスタンプを修正する
		 */
		frame->pts = frame->dts;
		GST_BUFFER_PTS(frame->input_buffer) = GST_BUFFER_DTS(frame->input_buffer);
#endif
		ret = gst_video_decoder_finish_frame (GST_VIDEO_DECODER (me), frame);
		if (GST_FLOW_OK != ret) {
			GST_WARNING_OBJECT (me, "gst_video_decoder_finish_frame() returns %s",
								gst_flow_get_name (ret));
			
			if (displayingBuf) {
				gst_buffer_unref(displayingBuf);
				displayingBuf = NULL;
			}
			
			goto finish_frame_failed;
		}

		/* ディスプレイ表示中のバッファは ref して保持し、次のバッファを表示した後、
		 * unref してデバイスに queue する。
		 * ただし、sink が、acmfbdevsink の場合は、sink 側で、ref しているので、
		 * デコーダ側で、表示中バッファを保持する必要はない。
		 */
		if (! me->priv->using_fb_dmabuf) {
			if (NULL != me->priv->displaying_buf) {
				gst_buffer_unref(me->priv->displaying_buf);
				me->priv->displaying_buf = NULL;
			}
			me->priv->displaying_buf = displayingBuf;
		}
	}

#else	/* DO_PUSH_POOLS_BUF */

#if DO_FRAME_DROP
	deadline = gst_video_decoder_get_max_decode_time (GST_VIDEO_DECODER (me), frame);
    if (deadline < 0) {
		GST_WARNING_OBJECT (me, "Skipping late frame (%f s past deadline)",
						(double) -deadline / GST_SECOND);
		gst_video_decoder_drop_frame (GST_VIDEO_DECODER (me), frame);
    }
	else
#endif
	{
		/* 出力バッファをアロケート	*/
		ret = gst_video_decoder_allocate_output_frame (
									GST_VIDEO_DECODER (me), frame);
		if (GST_FLOW_OK != ret) {
			GST_ERROR_OBJECT (me, "gst_video_decoder_allocate_output_frame() returns %s",
							  gst_flow_get_name (ret));
			goto allocate_outbuf_failed;
		}
		GST_DEBUG_OBJECT(me, "outbuf :%p", frame->output_buffer);

		/* 出力データをコピー	*/
		gst_buffer_map (v4l2buf_out, &map, GST_MAP_READ);
		GST_DEBUG_OBJECT(me, "copy buf size=%d", map.size);
		gst_buffer_fill(frame->output_buffer, 0, map.data, map.size);
		gst_buffer_unmap (v4l2buf_out, &map);
		gst_buffer_resize(frame->output_buffer, 0, gst_buffer_get_size(v4l2buf_out));

		GST_DEBUG_OBJECT(me, "outbuf size=%d",
						 gst_buffer_get_size(frame->output_buffer));
	}
	/* enqueue buffer	*/
	ret = gst_acm_v4l2_buffer_pool_qbuf (
			me->pool_out, v4l2buf_out, gst_buffer_get_size(v4l2buf_out));
	if (GST_FLOW_OK != ret) {
		GST_ERROR_OBJECT (me, "gst_acm_v4l2_buffer_pool_qbuf() returns %s",
						  gst_flow_get_name (ret));
		goto qbuf_failed;
	}

	/* down stream へ バッファを push	*/
#if DO_FRAME_DROP
    if (deadline >= 0)
#endif
	{
		GST_INFO_OBJECT(me, "H264DEC FINISH FRAME:%p", frame->output_buffer);
		ret = gst_video_decoder_finish_frame (GST_VIDEO_DECODER (me), frame);
		if (GST_FLOW_OK != ret) {
			GST_ERROR_OBJECT (me, "gst_video_decoder_finish_frame() returns %s",
							  gst_flow_get_name (ret));
			goto finish_frame_failed;
		}
	}
#endif	/* DO_PUSH_POOLS_BUF */
	
out:
	return ret;
	
	/* ERRORS */
no_frame:
	{
//		GST_ELEMENT_ERROR (me, STREAM, DECODE, (NULL),
//			("display buffer does not have a valid frame"));
//		ret = GST_FLOW_ERROR;
		GST_WARNING_OBJECT (me, "no more frame to process");
		ret = GST_FLOW_OK;
		if (NULL != is_eos) {
			*is_eos = TRUE;
		}
		
		/* 正しく解放されるように、QBUF する		*/
		ret = gst_acm_v4l2_buffer_pool_qbuf (
			me->pool_out, v4l2buf_out, gst_buffer_get_size(v4l2buf_out));
		if (GST_FLOW_OK != ret) {
			GST_ERROR_OBJECT (me, "gst_acm_v4l2_buffer_pool_qbuf() returns %s",
							  gst_flow_get_name (ret));
			goto qbuf_failed;
		}
		goto out;
	}
qbuf_failed:
	{
		GST_ELEMENT_ERROR (me, STREAM, DECODE, (NULL),
			("could not queue buffer. %d (%s)", errno, g_strerror (errno)));
		ret = GST_FLOW_ERROR;
		goto out;
	}
finish_frame_failed:
	{
		if (GST_FLOW_NOT_LINKED == ret || GST_FLOW_FLUSHING == ret) {
			GST_WARNING_OBJECT (me,
				"failed gst_video_decoder_finish_frame() - not link or flushing");
			
//			ret = GST_FLOW_ERROR;
			goto out;
		}

		GST_ELEMENT_ERROR (me, STREAM, DECODE, (NULL),
			("failed gst_video_decoder_finish_frame()"));
//		ret = GST_FLOW_ERROR;
		goto out;
	}
#if DO_PUSH_POOLS_BUF
acquire_buffer_failed:
	{
		GST_ELEMENT_ERROR (me, STREAM, DECODE, (NULL),
						   ("failed gst_buffer_pool_acquire_buffer()"));
		ret = GST_FLOW_ERROR;
		goto out;
	}
#endif
allocate_outbuf_failed:
	{
		GST_ELEMENT_ERROR (me, RESOURCE, FAILED, (NULL),
						   ("failed allocate outbuf"));
		ret = GST_FLOW_ERROR;
		goto out;
	}
}

static gboolean
plugin_init (GstPlugin * plugin)
{
	GST_DEBUG_CATEGORY_INIT (acmh264dec_debug, "acmh264dec", 0, "H264 decoding");

	return gst_element_register (plugin, "acmh264dec",
								 GST_RANK_PRIMARY, GST_TYPE_ACMH264DEC);
}

GST_PLUGIN_DEFINE (
	GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    acmh264dec,
    "ACM H264 Decoder",
	plugin_init,
	VERSION,
	"LGPL",
	"GStreamer ACM Plugins",
	"http://armadillo.atmark-techno.com/"
);


/*
 * End of file
 */
