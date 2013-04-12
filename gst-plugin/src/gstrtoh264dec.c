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

/**
 * SECTION:element-rtoh264dec
 *
 * rtoh264dec decodes H264 (MPEG-4) stream.
 *
 * <refsect2>
 * <title>Example launch lines</title>
 * |[
 * gst-launch filesrc location=example.mp4 ! qtdemux ! rtoh264dec ! autovideosink
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
#include <gst/video/gstvideometa.h>
#include <gst/video/gstvideopool.h>

#include "gstrtoh264dec.h"
#include "v4l2_util.h"
#include "gstrtodmabufmeta.h"


/* バッファプール内のバッファを no copy で down stream に push する	*/
#define DO_PUSH_POOLS_BUF			1

/* フレームのドロップは、sink 側で行う	*/
#define DO_FRAME_DROP				0

/* ES (with NAL start code), MP4 (without start code) をサポート
 * (R-MobileA1マルチメディアミドル 機能仕様書 Ver.0.14)
 */
#define SUPPORT_MP4_ONLY			0

#define V4L2_CID_NR_REFERENCE_FRAMES	(V4L2_CID_PRIVATE_BASE + 0) /* fmem_num */
#define V4L2_CID_NR_BUFFERING_PICS		(V4L2_CID_PRIVATE_BASE + 1) /* buffering_pic_cnt */
#define V4L2_CID_ENABLE_VIO				(V4L2_CID_PRIVATE_BASE + 2) /* VIO6の有効無効フラグ:未実装 */

/* 確保するバッファ数	*/
#define DEFAULT_NUM_BUFFERS_IN			3
#define DEFAULT_NUM_BUFFERS_OUT			3
#define DEFAULT_NUM_BUFFERS_OUT_DMABUF	NUM_FB_DMABUF

/* デコーダ初期化パラメータのデフォルト値	*/
#define DEFAULT_VIDEO_DEVICE			"/dev/video0"
#define DEFAULT_OUT_WIDTH				0
#define DEFAULT_OUT_HEIGHT				0
#define DEFAULT_FMEM_NUM				17
#define DEFAULT_BUF_PIC_CNT				17
#define DEFAULT_OUT_FORMAT				GST_RTOH264DEC_OUT_FMT_RGB24
#define DEFAULT_OUT_VIDEO_FORMAT_STR	"RGB"
#define DEFAULT_ENABLE_VIO6				TRUE

/* select() の timeout */
#define SELECT_TIMEOUT_MSEC			400

/* RGB24 -> RGB16 (for LCD) MACRO */
#define RGB24_TO_RGB16(r, g, b) (((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3))

/* デバッグログ出力フラグ		*/
#define DBG_LOG_PERF_CHAIN			0
#define DBG_LOG_PERF_PUSH			0


/* select() による待ち時間の計測		*/
#define DBG_MEASURE_PERF				0
#if DBG_MEASURE_PERF
# define DBG_MEASURE_PERF_SELECT_IN		0
# define DBG_MEASURE_PERF_SELECT_OUT	0
# define DBG_MEASURE_PERF_HANDLE_FRAME	0
# define DBG_MEASURE_PERF_FINISH_FRAME	0
#endif

#if DBG_MEASURE_PERF
static double
gettimeofday_sec()
{
	struct timeval t;
	
	gettimeofday(&t, NULL);
	return (double)t.tv_sec + (double)t.tv_usec * 1e-6;
}
#endif
#if DBG_MEASURE_PERF_SELECT_IN
static double g_time_total_select_in = 0;
#endif
#if DBG_MEASURE_PERF_SELECT_OUT
static double g_time_total_select_out = 0;
#endif


struct _GstRtoH264DecPrivate
{
	/* V4L2_BUF_TYPE_VIDEO_OUTPUT 側に入力したフレーム数と、
	 * V4L2_BUF_TYPE_VIDEO_CAPTURE 側から取り出したフレーム数の差分
	 */
	volatile gint in_out_frame_count;

	struct v4l2_buffer input_vbuffer[DEFAULT_NUM_BUFFERS_IN];

	/* ディスプレイ表示中のバッファは、次の表示を終えるまで、ref して
	 * 保持しておかないと、m2m デバイスに enqueue され、ディスプレイに表示中に、
	 * デコードデータを上書きされてしまい、画面が乱れてしまう
	 */
	GstBuffer* displying_buf;
};

GST_DEBUG_CATEGORY_STATIC (rtoh264dec_debug);
#define GST_CAT_DEFAULT (rtoh264dec_debug)
GST_DEBUG_CATEGORY_EXTERN (GST_CAT_PERFORMANCE);

#define GST_RTOH264DEC_GET_PRIVATE(obj)  \
	(G_TYPE_INSTANCE_GET_PRIVATE ((obj), GST_TYPE_RTOH264DEC, \
		GstRtoH264DecPrivate))

/* property */
enum
{
	PROP_0,
	PROP_DEVICE,
	PROP_OUT_WIDTH,
	PROP_OUT_HEIGHT,
	PROP_FMEM_NUM,
	PROP_BUF_PIC_CNT,
	PROP_OUT_FORMAT,
	PROP_ENABLE_VIO6,
};

/* pad template caps for source and sink pads.	*/
static GstStaticPadTemplate sink_template_factory =
GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
	GST_STATIC_CAPS (
#if 0
		"video/x-h264, "
		"stream-format = (string) { avc, byte-stream }, "
		"alignment = (string) { au, nal }, "
		"width  = (int)[80, 1920], "
		"height = (int)[80, 1080], "
		"framerate = (fraction) [ 0/1, MAX ] "
#else
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
#endif
	)
);

// NV12 == GST_VIDEO_FORMAT_NV12
static GstStaticPadTemplate src_template_factory =
GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (
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
		"format = (string) RGB16, "
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


/* GstVideoDecoder base class method */
static gboolean gst_rto_h264_dec_open (GstVideoDecoder * dec);
static gboolean gst_rto_h264_dec_close (GstVideoDecoder * dec);
static gboolean gst_rto_h264_dec_start (GstVideoDecoder * dec);
static gboolean gst_rto_h264_dec_stop (GstVideoDecoder * dec);
static gboolean gst_rto_h264_dec_set_format (GstVideoDecoder * dec,
    GstVideoCodecState * state);
static gboolean gst_rto_h264_dec_reset (GstVideoDecoder * dec, gboolean hard);
static GstFlowReturn gst_rto_h264_dec_finish (GstVideoDecoder * dec);
static GstFlowReturn gst_rto_h264_dec_parse (GstVideoDecoder *dec,
	GstVideoCodecFrame *frame, GstAdapter *adapter, gboolean at_eos);
static GstFlowReturn gst_rto_h264_dec_handle_frame (GstVideoDecoder * dec,
    GstVideoCodecFrame * frame);
static gboolean gst_rto_h264_dec_sink_event (GstVideoDecoder * dec,
	GstEvent *event);

static gboolean gst_rto_h264_dec_init_decoder (GstRtoH264Dec * me);
static gboolean gst_rto_h264_dec_cleanup_decoder (GstRtoH264Dec * me);
static GstFlowReturn gst_rto_h264_dec_handle_in_frame(GstRtoH264Dec * me,
	GstBuffer *inbuf);
static GstFlowReturn gst_rto_h264_dec_handle_out_frame(GstRtoH264Dec * me,
	gboolean* is_eos);

static void gst_rto_h264_dec_set_property (GObject * object, guint prop_id,
	const GValue * value, GParamSpec * pspec);
static void gst_rto_h264_dec_get_property (GObject * object, guint prop_id,
	GValue * value, GParamSpec * pspec);
static void gst_rto_h264_dec_finalize (GObject * object);

#define gst_rto_h264_dec_parent_class parent_class
G_DEFINE_TYPE (GstRtoH264Dec, gst_rto_h264_dec, GST_TYPE_VIDEO_DECODER);


static void
gst_rto_h264_dec_set_property (GObject * object, guint prop_id,
							  const GValue * value, GParamSpec * pspec)
{
	GstRtoH264Dec *me = GST_RTOH264DEC (object);

	switch (prop_id) {
	case PROP_DEVICE:
		/* デバイスファイル名	*/
		if (me->videodev) {
			g_free (me->videodev);
		}
		me->videodev = g_value_dup_string (value);
		break;
	case PROP_OUT_WIDTH:
		me->out_width = g_value_get_uint (value);
		break;
	case PROP_OUT_HEIGHT:
		me->out_height = g_value_get_uint (value);
		break;
	case PROP_FMEM_NUM:
		me->fmem_num = g_value_get_uint (value);
		break;
	case PROP_BUF_PIC_CNT:
		me->buffering_pic_cnt = g_value_get_uint (value);
		break;
	case PROP_OUT_FORMAT:
		me->do_RGB24_to_RGB16 = FALSE;
		if (me->out_video_fmt_str) {
			g_free (me->out_video_fmt_str);
		}
		me->out_video_fmt_str = g_value_dup_string (value);
		if (g_str_equal (me->out_video_fmt_str, "NV12")) {
			me->out_video_fmt = GST_VIDEO_FORMAT_NV12;
			me->output_format = GST_RTOH264DEC_OUT_FMT_YUV420;
		}
		else if (g_str_equal (me->out_video_fmt_str, "RGBx")) {
			me->out_video_fmt = GST_VIDEO_FORMAT_RGBx;
			me->output_format = GST_RTOH264DEC_OUT_FMT_RGB32;
		}
		else if (g_str_equal (me->out_video_fmt_str, "RGB")) {
			me->out_video_fmt = GST_VIDEO_FORMAT_RGB;
			me->output_format = GST_RTOH264DEC_OUT_FMT_RGB24;
		}
		else if (g_str_equal (me->out_video_fmt_str, "RGB16")) {
			me->out_video_fmt = GST_VIDEO_FORMAT_RGB16;
			/* RGB24 で HWデコードを行い、element 側で RGB16 に変換する	*/
			me->output_format = GST_RTOH264DEC_OUT_FMT_RGB24;
			me->do_RGB24_to_RGB16 = TRUE;
		}
		else {
			G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		}
		break;
	case PROP_ENABLE_VIO6:
		me->enable_vio6 = g_value_get_boolean (value);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
gst_rto_h264_dec_get_property (GObject * object, guint prop_id,
	GValue * value, GParamSpec * pspec)
{
	GstRtoH264Dec *me = GST_RTOH264DEC (object);
	
	switch (prop_id) {
	case PROP_DEVICE:
		g_value_set_string (value, me->videodev);
		break;
	case PROP_OUT_WIDTH:
		g_value_set_uint (value, me->out_width);
		break;
	case PROP_OUT_HEIGHT:
		g_value_set_uint (value, me->out_height);
		break;
	case PROP_FMEM_NUM:
		g_value_set_uint (value, me->fmem_num);
		break;
	case PROP_BUF_PIC_CNT:
		g_value_set_uint (value, me->buffering_pic_cnt);
		break;
	case PROP_OUT_FORMAT:
		g_value_set_string (value, me->out_video_fmt_str);
		break;
	case PROP_ENABLE_VIO6:
		g_value_set_boolean (value, me->enable_vio6);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
gst_rto_h264_dec_class_init (GstRtoH264DecClass * klass)
{
	GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
	GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
	GstVideoDecoderClass *video_decoder_class = GST_VIDEO_DECODER_CLASS (klass);

	g_type_class_add_private (klass, sizeof (GstRtoH264DecPrivate));

	gobject_class->set_property = gst_rto_h264_dec_set_property;
	gobject_class->get_property = gst_rto_h264_dec_get_property;
	gobject_class->finalize = gst_rto_h264_dec_finalize;
	
	g_object_class_install_property (gobject_class, PROP_DEVICE,
		g_param_spec_string ("device", "device",
			"The video device eg: /dev/video0",
			NULL, G_PARAM_READWRITE));

	g_object_class_install_property (gobject_class, PROP_OUT_WIDTH,
		g_param_spec_uint ("width", "Width",
			"Width of output video. (0 is unspecified)",
		GST_RTOH264DEC_WIDTH_MIN, GST_RTOH264DEC_WIDTH_MAX,
		DEFAULT_OUT_WIDTH, G_PARAM_READWRITE));

	g_object_class_install_property (gobject_class, PROP_OUT_HEIGHT,
		g_param_spec_uint ("height", "Height",
			"Height of output video. (0 is unspecified)",
		GST_RTOH264DEC_HEIGHT_MIN, GST_RTOH264DEC_HEIGHT_MAX,
		DEFAULT_OUT_HEIGHT, G_PARAM_READWRITE));

	g_object_class_install_property (gobject_class, PROP_FMEM_NUM,
		g_param_spec_uint ("fmem-num", "Fmem Num",
			"Number of reference frame",
			GST_RTOH264DEC_FMEM_NUM_MIN, GST_RTOH264DEC_FMEM_NUM_MAX,
			DEFAULT_FMEM_NUM, G_PARAM_READWRITE));

	g_object_class_install_property (gobject_class, PROP_BUF_PIC_CNT,
		g_param_spec_uint ("buf-pic-cnt", "Buffering Pic Cnt",
			"Number of buffering picture",
			GST_RTOH264DEC_BUF_PIC_CNT_MIN, GST_RTOH264DEC_BUF_PIC_CNT_MAX,
			DEFAULT_BUF_PIC_CNT, G_PARAM_READWRITE));

	g_object_class_install_property (gobject_class, PROP_OUT_FORMAT,
		g_param_spec_string ("format", "Output Format",
			"RGB, RGB16, NV12, RGBx",
			"RGB", G_PARAM_READWRITE));

	g_object_class_install_property (gobject_class, PROP_ENABLE_VIO6,
		g_param_spec_boolean ("enable-vio6", "Enable VIO6",
			"FALSE: disable, TRUE: enable",
			DEFAULT_ENABLE_VIO6, G_PARAM_READWRITE));

	gst_element_class_add_pad_template (element_class,
			gst_static_pad_template_get (&src_template_factory));
	gst_element_class_add_pad_template (element_class,
			gst_static_pad_template_get (&sink_template_factory));

	gst_element_class_set_static_metadata (element_class,
			"RTO H264 video decoder", "Codec/Decoder/Video",
			"RTO H.264/AVC decoder", "atmark techno");

	video_decoder_class->open = GST_DEBUG_FUNCPTR (gst_rto_h264_dec_open);
	video_decoder_class->close = GST_DEBUG_FUNCPTR (gst_rto_h264_dec_close);
	video_decoder_class->start = GST_DEBUG_FUNCPTR (gst_rto_h264_dec_start);
	video_decoder_class->stop = GST_DEBUG_FUNCPTR (gst_rto_h264_dec_stop);
	video_decoder_class->reset = GST_DEBUG_FUNCPTR (gst_rto_h264_dec_reset);
	video_decoder_class->set_format = GST_DEBUG_FUNCPTR (gst_rto_h264_dec_set_format);
	video_decoder_class->parse = GST_DEBUG_FUNCPTR (gst_rto_h264_dec_parse);
	video_decoder_class->handle_frame =
		GST_DEBUG_FUNCPTR (gst_rto_h264_dec_handle_frame);
	video_decoder_class->finish = GST_DEBUG_FUNCPTR (gst_rto_h264_dec_finish);
	video_decoder_class->sink_event =
		GST_DEBUG_FUNCPTR (gst_rto_h264_dec_sink_event);
}

static void
gst_rto_h264_dec_init (GstRtoH264Dec * me)
{
	me->priv = GST_RTOH264DEC_GET_PRIVATE (me);

	me->width = 0;
	me->height = 0;
	me->spspps_size = 0;

	me->input_state = NULL;
	me->output_state = NULL;

	me->is_eos_received = FALSE;
	me->is_decode_all_done = FALSE;
	me->is_do_stop = FALSE;

	me->out_width = DEFAULT_OUT_WIDTH;
	me->out_height = DEFAULT_OUT_HEIGHT;
	me->out_video_fmt_str = NULL;
	me->out_video_fmt = GST_VIDEO_FORMAT_UNKNOWN;
	me->do_RGB24_to_RGB16 = FALSE;

	me->video_fd = -1;
	me->is_handled_1stframe = FALSE;
	me->pool_out = NULL;
	me->num_inbuf_acquired = 0;

	/* property	*/
	me->videodev = NULL;
	me->fmem_num = DEFAULT_FMEM_NUM;
	me->buffering_pic_cnt = DEFAULT_BUF_PIC_CNT;
	me->input_format = GST_RTOH264DEC_IN_FMT_UNKNOWN;
	me->output_format = GST_RTOH264DEC_OUT_FMT_UNKNOWN;
	me->enable_vio6 = DEFAULT_ENABLE_VIO6;

	/* If the input is packetized, then the parse method will not be called. */
	gst_video_decoder_set_packetized (GST_VIDEO_DECODER (me), TRUE);
}

static void
gst_rto_h264_dec_finalize (GObject * object)
{
	GstRtoH264Dec *me = GST_RTOH264DEC (object);

	GST_INFO_OBJECT (me, "H264DEC FINALIZE");

	/* プロパティを保持するため、gst_rto_h264_dec_close() 内で free してはいけない	*/
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
gst_rto_h264_dec_open (GstVideoDecoder * dec)
{
	GstRtoH264Dec *me = GST_RTOH264DEC (dec);

	/* プロパティとしてセットされていなければ、デフォルト値を設定		*/
	if (NULL == me->videodev) {
		me->videodev = g_strdup (DEFAULT_VIDEO_DEVICE);
	}

	GST_INFO_OBJECT (me, "H264DEC OPEN RTO DECODER. (%s)", me->videodev);
	
	/* デバイスファイルオープン	*/
	GST_INFO_OBJECT (me, "Trying to open device %s", me->videodev);
	if (! gst_v4l2_open (me->videodev, &(me->video_fd), TRUE)) {
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
gst_rto_h264_dec_close (GstVideoDecoder * dec)
{
	GstRtoH264Dec *me = GST_RTOH264DEC (dec);

	GST_INFO_OBJECT (me, "H264DEC CLOSE RTO DECODER. (%s)", me->videodev);
	
	/* close device	*/
	if (me->video_fd > 0) {
		gst_v4l2_close(me->videodev, me->video_fd);
		me->video_fd = -1;
	}

	return TRUE;
}

static gboolean
gst_rto_h264_dec_start (GstVideoDecoder * dec)
{
	gint i;
	GstRtoH264Dec *me = GST_RTOH264DEC (dec);
	
	GST_INFO_OBJECT (me, "H264DEC START");

	/* never mind a few errors */
	gst_video_decoder_set_max_errors (dec, 20);

	g_atomic_int_set (&(me->priv->in_out_frame_count), 0);

	/* プロパティ以外の変数を再初期化		*/
	me->width = 0;
	me->height = 0;
	me->spspps_size = 0;
	
	me->is_eos_received = FALSE;
	me->is_decode_all_done = FALSE;
	me->is_do_stop = FALSE;
	
	me->is_handled_1stframe = FALSE;
	me->num_inbuf_queued = 0;

	me->using_fb_dmabuf = FALSE;
	me->num_fb_dmabuf = 0;
	for (i = 0; i < MAX_NUM_DMABUF; i++) {
		me->fb_dmabuf_fd[i] = -1;
		me->fb_dmabuf_index[i] = -1;
	}

	me->priv->displying_buf = NULL;

	return TRUE;
}

static gboolean
gst_rto_h264_dec_stop (GstVideoDecoder * dec)
{
	GstRtoH264Dec *me = GST_RTOH264DEC (dec);
	gint i = 0;

	GST_INFO_OBJECT (me, "H264DEC STOP");
	
	if (me->input_state) {
		gst_video_codec_state_unref (me->input_state);
		me->input_state = NULL;
	}
	if (me->output_state) {
		gst_video_codec_state_unref (me->output_state);
		me->output_state = NULL;
	}

	if (me->priv->displying_buf) {
		gst_buffer_unref(me->priv->displying_buf);
		me->priv->displying_buf = NULL;
	}

	/* クリーンアップ処理	*/
	gst_rto_h264_dec_cleanup_decoder (me);

	/* プロパティ以外の変数を再初期化		*/
	me->width = 0;
	me->height = 0;
	me->spspps_size = 0;

	me->is_eos_received = FALSE;
	me->is_decode_all_done = FALSE;
	me->is_do_stop = FALSE;

	me->is_handled_1stframe = FALSE;
	me->num_inbuf_acquired = 0;

	me->using_fb_dmabuf = FALSE;
	me->num_fb_dmabuf = 0;
	for (i = 0; i < MAX_NUM_DMABUF; i++) {
		me->fb_dmabuf_fd[i] = -1;
		me->fb_dmabuf_index[i] = -1;
	}

	return TRUE;
}

static gboolean
gst_rto_h264_dec_analize_codecdata(GstRtoH264Dec *me, GstBuffer * codec_data)
{
	unsigned int sps_size, pps_size;
	unsigned int spses_size = 0, ppses_size = 0;
	unsigned int sps_num, pps_num;
	int counter = 5;
	int i, j;
	GstMapInfo map;

	gst_buffer_map(codec_data, &map, GST_MAP_READ);

#if 0	/* for debug	*/
	{
		FILE* fp = fopen("codec_data.txt", "w");
		if (fp) {
			for (i = 0; i < map.size; i++) {
				fprintf(fp, "0x%02X, ", map.data[i]);
				if((i+1) % 8 == 0)
					fprintf(fp, "\n");
			}
			fclose(fp);
	}
	}
#endif
	
	/* get sps_num */
	sps_num = *(unsigned char *)(map.data + counter) & 0x1f;
	counter++;
	
	sps_size = 0;
	spses_size = 0;
	/* get sps process */
	for(i = 0; i < sps_num; i++){
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
	
	pps_size = 0;
	ppses_size = 0;
	/* get pps process */
	for (i = 0; i < pps_num; i++) {
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
#if 0	/* for debug	*/
	for (i = 0; i < me->spspps_size; i++) {
		GST_DEBUG_OBJECT(me, "[%d] : %02x ", i, me->spspps[i]);
	}
#endif
	gst_buffer_unmap(codec_data, &map);

	return TRUE;
}

static gboolean
gst_rto_h264_dec_set_format (GstVideoDecoder * dec, GstVideoCodecState * state)
{
	GstRtoH264Dec *me = GST_RTOH264DEC (dec);
	gboolean ret = TRUE;
	GstVideoInfo *vinfo;
	GstStructure *structure = NULL;
	const gchar *alignment = NULL;

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
			me->input_format = GST_RTOH264DEC_IN_FMT_MP4;
		}
#if SUPPORT_MP4_ONLY
		else {
			GST_ELEMENT_ERROR (me, STREAM, DECODE, (NULL),
							   ("ES (with NAL start code) is unsupported"));
			return FALSE;
		}
#else
		else if (g_str_equal(alignment, "nal")) {
			me->input_format = GST_RTOH264DEC_IN_FMT_ES;
		}
#endif
	}
	else {
		me->input_format = GST_RTOH264DEC_IN_FMT_MP4;
	}

	me->width = vinfo->width;
	me->height = vinfo->height;
	if (DEFAULT_OUT_WIDTH == me->out_width) {
		me->out_width = me->width;
	}
	if (DEFAULT_OUT_HEIGHT == me->out_height) {
		me->out_height = me->height;
	}

	/* analize codecdata */
	if (state->codec_data) {
		gst_rto_h264_dec_analize_codecdata(me, state->codec_data);

#if 0	/* for debug	*/
		{
			gint i;
			GstMapInfo map_debug;
			FILE* fp = fopen("_codec_data.data", "w");
			if (fp) {
				gst_buffer_map (state->codec_data, &map_debug, GST_MAP_READ);
				for (i = 0; i < map_debug.size; i++) {
#if 0
					fprintf(fp, "0x%02X, ", map_debug.data[i]);
					if((i+1) % 8 == 0)
						fprintf(fp, "\n");
#else
					fputc(map_debug.data[i], fp);
#endif
				}
				gst_buffer_unmap (state->codec_data, &map_debug);
				fclose(fp);
			}
		}
#endif
	}

	/* Creates a new output state with the specified fmt, width and height
	 * and Negotiate with downstream elements.
	 */
	g_assert (me->output_state == NULL);
	me->output_state = gst_video_decoder_set_output_state (GST_VIDEO_DECODER (me),
		me->out_video_fmt, me->out_width, me->out_height, me->input_state);
	GST_INFO_OBJECT (me,
		"H264DEC OUT FORMAT - info: fmt:%" GST_FOURCC_FORMAT "(%d), %d x %d, %d/%d",
		GST_FOURCC_ARGS (me->input_format), me->do_RGB24_to_RGB16,
		me->output_state->info.width, me->output_state->info.height,
		me->output_state->info.fps_n, me->output_state->info.fps_d);
	GST_INFO_OBJECT (me, "H264DEC OUT FORMAT - caps: %" GST_PTR_FORMAT,
					 me->output_state->caps);
	GST_INFO_OBJECT (me, "H264DEC OUT FORMAT - codec_data: %p",
					 me->output_state->codec_data);
	if (! gst_video_decoder_negotiate (GST_VIDEO_DECODER (me))) {
		goto negotiate_failed;
	}

#if 0	/* for debug	*/
	{
		GstCaps *src_caps = NULL;
		GstCaps *sink_caps = NULL;
		src_caps = gst_pad_get_current_caps (GST_VIDEO_DECODER_SRC_PAD (me));
		GST_INFO_OBJECT (me, "current src caps: %" GST_PTR_FORMAT, src_caps);
		sink_caps = gst_pad_get_current_caps (GST_VIDEO_DECODER_SINK_PAD (me));
		GST_INFO_OBJECT (me, "current sink caps: %" GST_PTR_FORMAT, sink_caps);
	}
#endif

#if 1
	{
		/* sink からDMABUF情報取得		*/
		GstQuery *customQuery = NULL;
		GstStructure *callStructure;
		const GstStructure *resStructure;
		gint fd = -1;
		guint i;

		for (i = 0; i < MAX_NUM_DMABUF; i++) {
			callStructure = gst_structure_new ("GstRtoDmabufQuery",
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
				|| ! gst_structure_has_name (resStructure, "GstRtoDmabufQuery")) {
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
				me->using_fb_dmabuf = TRUE;
				me->num_fb_dmabuf++;
				me->fb_dmabuf_fd[i] = fd;
				me->fb_dmabuf_index[i] = i;
			}
			else {
				/* non use dms-buf	*/
				break;
			}
		}
	}
#endif

	/* デコーダ初期化	*/
	if (! gst_rto_h264_dec_init_decoder(me)) {
		goto init_failed;
	}

out:
	return ret;

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
gst_rto_h264_dec_reset (GstVideoDecoder * dec, gboolean hard)
{
	GstRtoH264Dec *me = GST_RTOH264DEC (dec);

	GST_INFO_OBJECT (me, "H264DEC RESET %s", hard ? "hard" : "soft");

	return TRUE;
}

/*
 * Optional. Called to request subclass to dispatch any pending remaining data 
 * (e.g. at EOS).
 */
static GstFlowReturn
gst_rto_h264_dec_finish (GstVideoDecoder * dec)
{
	GstRtoH264Dec *me = GST_RTOH264DEC (dec);

	GST_INFO_OBJECT (me, "H264DEC FINISH");

	/* do nothing	*/

	return GST_FLOW_OK;
}

static GstFlowReturn
gst_rto_h264_dec_parse (GstVideoDecoder *dec, GstVideoCodecFrame *frame,
					   GstAdapter *adapter, gboolean at_eos)
{
	GstRtoH264Dec *me = GST_RTOH264DEC (dec);
	
	GST_INFO_OBJECT (me, "H264DEC PARSE at_eos:%d", at_eos);
	
	return GST_FLOW_OK;
}

static GstFlowReturn
gst_rto_h264_dec_handle_frame (GstVideoDecoder * dec,
    GstVideoCodecFrame * frame)
{
	GstRtoH264Dec *me = GST_RTOH264DEC (dec);
	GstBuffer *buffer = frame->input_buffer;
	GstFlowReturn ret = GST_FLOW_OK;
	int r = 0;
	fd_set write_fds;
	fd_set read_fds;
	struct timeval tv;
#if DBG_MEASURE_PERF_HANDLE_FRAME
	static double interval_time_start = 0, interval_time_end = 0;
#endif
#if DBG_MEASURE_PERF_SELECT_IN || DBG_MEASURE_PERF_SELECT_OUT
	double time_start, time_end;
#endif

#if DBG_LOG_PERF_CHAIN
	GST_INFO_OBJECT (me, "# H264DEC-CHAIN HANDLE FRMAE START");
#endif

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

    GST_DEBUG_OBJECT (me, "H264DEC HANDLE FRMAE - size:%d, ref:%d ...",
					  gst_buffer_get_size(buffer),
					  GST_OBJECT_REFCOUNT_VALUE(frame->input_buffer));
	GST_DEBUG_OBJECT (me, "received frame no:%d, timestamp:%"
					  GST_TIME_FORMAT ", duration:%" GST_TIME_FORMAT,
					  frame->system_frame_number,
					  GST_TIME_ARGS (frame->pts), GST_TIME_ARGS (frame->duration));

	/* Seek が行われた際は、gst_video_decoder_reset() により、dec->priv->frames が、
	 * 空になる。
	 * これにより、本関数の引数 frame->input_buffer は、gst_video_decoder_finish_frame()
	 * を call した際に、unref される。そのため、本関数の処理中は、ref して保持しておく必要
	 * がある。
	 */
	gst_buffer_ref(frame->input_buffer);

#if 0	/* for debug	*/
	{
		static int index = 0;
		gint i;
		GstMapInfo map_debug;
		char fileNameBuf[1024];
		sprintf(fileNameBuf, "h264_%03d.data", ++index);
		
		FILE* fp = fopen(fileNameBuf, "w");
		if (fp) {
			gst_buffer_map (buffer, &map_debug, GST_MAP_READ);
			for (i = 0; i < map_debug.size; i++) {
#if 0
				fprintf(fp, "0x%02X, ", map_debug.data[i]);
				if((i+1) % 8 == 0)
					fprintf(fp, "\n");
#else
				fputc(map_debug.data[i], fp);
#endif
			}
			gst_buffer_unmap (buffer, &map_debug);
			fclose(fp);
		}
	}
#endif

	/* first frame */
	if (! me->is_handled_1stframe) {
		if (0 == me->spspps_size) {
			GST_INFO_OBJECT(me, "could not insert SPS/PPS to frame");

			/* 初回の入力		*/
			ret = gst_rto_h264_dec_handle_in_frame(me, buffer);
			if (GST_FLOW_OK != ret) {
				goto handle_in_failed;
			}
			g_atomic_int_inc (&(me->priv->in_out_frame_count));
			me->num_inbuf_queued++;
		}
		else {
			GstMapInfo map;
			GstMapInfo map_dst;

			/* SPS/PPS の挿入 */
			gst_buffer_map (frame->input_buffer, &map, GST_MAP_READ);
			GST_INFO_OBJECT(me, "insert SPS/PPS to frame");
			
			buffer = gst_buffer_new_allocate (NULL,
				gst_buffer_get_size(frame->input_buffer) + me->spspps_size, NULL);
			if (NULL == buffer) {
				GST_ELEMENT_ERROR (me, STREAM, DECODE, (NULL),
					("no mem with gst_buffer_new_allocate()"));
				ret = GST_FLOW_ERROR;
			}
			
			gst_buffer_map (buffer, &map_dst, GST_MAP_WRITE);
			/* SPS/PPS コピー */
			memcpy(map_dst.data, me->spspps, me->spspps_size);
			/* フレームデータ コピー */
			memcpy(map_dst.data + me->spspps_size, map.data, map.size);
			gst_buffer_unmap (buffer, &map_dst);
			gst_buffer_unmap (frame->input_buffer, &map);
			
			/* 初回の入力		*/
			ret = gst_rto_h264_dec_handle_in_frame(me, buffer);
			if (GST_FLOW_OK != ret) {
				goto handle_in_failed;
			}
			g_atomic_int_inc (&(me->priv->in_out_frame_count));
			me->num_inbuf_queued++;
			
			gst_buffer_unref(buffer);
			buffer = NULL;
		}

		me->is_handled_1stframe = TRUE;
		goto out;
	}

	/* 出力 : 参照フレーム数分入力した後は、デコード済みデータができるのを待って取り出す	*/
	if (me->num_inbuf_queued >= me->fmem_num) {
//		GST_INFO_OBJECT(me, "me->num_inbuf_queued : %d", me->num_inbuf_queued);
		do {
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
			} while (r == -1 && (errno == EINTR || errno == EAGAIN));
			if (r > 0 /* && FD_ISSET(me->video_fd, &read_fds) */) {
				ret = gst_rto_h264_dec_handle_out_frame(me, NULL);
				if (GST_FLOW_OK != ret) {
					if (GST_FLOW_FLUSHING == ret) {
						GST_DEBUG_OBJECT(me, "FLUSHING - continue.");

						/* エラーとせず、m2mデバイスへのデータ入力は行う	*/
						break;
					}
					goto handle_out_failed;
				}
			}
			else if (r < 0) {
				goto select_failed;
			}
			else if (0 == r) {
				/* timeoutしたらエラー	*/
				GST_ERROR_OBJECT (me, "select() for output is timeout");
				goto select_timeout;
			}
		} while (FALSE);
	}

	/* 入力:書き込みができる状態になるまで待ってから書き込む		*/
	{
#if DBG_LOG_PERF_CHAIN
		GST_INFO_OBJECT (me, "H264DEC-CHAIN SELECT IN START");
#endif
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
		} while (r == -1 && (errno == EINTR || errno == EAGAIN));
#if DBG_LOG_PERF_CHAIN
		GST_INFO_OBJECT (me, "H264DEC-CHAIN SELECT IN END");
#endif

		if (r > 0 /* && FD_ISSET(me->video_fd, &write_fds) */) {
			ret = gst_rto_h264_dec_handle_in_frame(me, buffer);
			if (GST_FLOW_OK != ret) {
				goto handle_in_failed;
			}
			g_atomic_int_inc (&(me->priv->in_out_frame_count));
			if (me->num_inbuf_queued <= me->fmem_num) {
				me->num_inbuf_queued++;
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

out:
#if DBG_LOG_PERF_CHAIN
	GST_INFO_OBJECT (me, "# H264DEC-CHAIN HANDLE FRMAE END");
#endif
#if 0	/* for debug	*/
	GST_INFO_OBJECT(me, "inbuf size=%d, ref:%d",
					gst_buffer_get_size(frame->input_buffer),
					GST_OBJECT_REFCOUNT_VALUE(frame->input_buffer));
#endif
	gst_buffer_unref(frame->input_buffer);

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
#if 0
		GST_ERROR_OBJECT (me, "pool_in - buffers:%d, allocated:%d, queued:%d",
						  me->pool_in->num_buffers,
						  me->pool_in->num_allocated,
						  me->pool_in->num_queued);
#endif
		GST_ERROR_OBJECT (me, "pool_out - buffers:%d, allocated:%d, queued:%d",
						  me->pool_out->num_buffers,
						  me->pool_out->num_allocated,
						  me->pool_out->num_queued);

		GST_ELEMENT_ERROR (me, STREAM, DECODE, (NULL),
			("timeout with select()"));
		ret = GST_FLOW_ERROR;
		goto out;
	}
handle_in_failed:
	{
		GST_ELEMENT_ERROR (me, STREAM, DECODE, (NULL),
						   ("failed handle in"));
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
		goto out;
	}
}

static gboolean
gst_rto_h264_dec_sink_event (GstVideoDecoder * dec, GstEvent *event)
{
	GstRtoH264Dec *me = GST_RTOH264DEC (dec);
	gboolean ret = FALSE;

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
		GstBuffer* eosBuffer = NULL;
		GstBuffer *v4l2buf_out = NULL;

		GST_INFO_OBJECT (me, "H264DEC received GST_EVENT_EOS");

		/* EOS の際は、0x00, 0x00, 0x00, 0x01, 0x0B を通知する */
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
			ret = gst_rto_h264_dec_handle_in_frame(me, eosBuffer);
			gst_buffer_unref(eosBuffer);
			
			if (GST_FLOW_OK != ret) {
				goto handle_in_failed;
			}
		}
		else if (r < 0) {
			gst_buffer_unref(eosBuffer);
			goto select_failed;
		}
		else if (0 == r) {
			/* timeoutしたらエラー	*/
			GST_INFO_OBJECT(me, "select() eos buf timeout");
			goto select_timeout;
		}

		/* デバイス側に溜まっているデータを取り出して、down stream へ流す	*/
		while (NULL != (frame = gst_video_decoder_get_oldest_frame(
									GST_VIDEO_DECODER (me)))) {
			gst_video_codec_frame_unref(frame);

			do {
				FD_ZERO(&read_fds);
				FD_SET(me->video_fd, &read_fds);
				tv.tv_sec = 0;
				tv.tv_usec = SELECT_TIMEOUT_MSEC * 1000;
				r = select(me->video_fd + 1, &read_fds, NULL, NULL, &tv);
				GST_DEBUG_OBJECT(me, "After select for read(EOS). r=%d", r);
			} while (r == -1 && (errno == EINTR || errno == EAGAIN));
			if (r > 0 /* && FD_ISSET(me->video_fd, &read_fds) */) {
				ret = gst_rto_h264_dec_handle_out_frame(me, &isEOS);
				if (GST_FLOW_OK != ret) {
					goto handle_out_failed;
				}
				if (isEOS) {
					break;
				}
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

		/* Seek した場合に、m2mデバイス側にデコード済みデータが残ってしまい、
		 * これを取り出しきらないと、クリーンアップ時、VIDIOC_STREAMOFF が 
		 * timeout でエラーしてしまう。
		 */
		GST_INFO_OBJECT(me, "in_out_frame_count : %d",
						g_atomic_int_get(&(me->priv->in_out_frame_count)));
		while (g_atomic_int_get(&(me->priv->in_out_frame_count)) > 0) {
			do {
				FD_ZERO(&read_fds);
				FD_SET(me->video_fd, &read_fds);
				tv.tv_sec = 0;
				tv.tv_usec = SELECT_TIMEOUT_MSEC * 1000;
				r = select(me->video_fd + 1, &read_fds, NULL, NULL, &tv);
				GST_DEBUG_OBJECT(me, "After select for read(EOS). r=%d", r);
			} while (r == -1 && (errno == EINTR || errno == EAGAIN));
			if (r > 0 /* && FD_ISSET(me->video_fd, &read_fds) */) {
				ret = gst_v4l2_buffer_pool_dqbuf (me->pool_out, &v4l2buf_out);
				if (GST_FLOW_OK != ret) {
					goto dqbuf_failed;
				}
				ret = gst_v4l2_buffer_pool_qbuf(me->pool_out, v4l2buf_out,
						gst_buffer_get_size(v4l2buf_out));
				if (GST_FLOW_OK != ret) {
					goto qbuf_failed;
				}
				g_atomic_int_dec_and_test (&(me->priv->in_out_frame_count));
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
gst_rto_h264_dec_init_decoder (GstRtoH264Dec * me)
{
	gboolean ret = TRUE;
	enum v4l2_buf_type type;
	int r;
	GstCaps *srcCaps;
	GstV4l2InitParam v4l2InitParam;
	struct v4l2_format fmt;
	struct v4l2_control ctrl;
	gint i = 0;

	GST_INFO_OBJECT (me, "H264DEC INITIALIZE RTO DECODER...");

	/* 入力バッファサイズ	*/
	guint in_frame_size = me->width * me->height * 3;
	/* 出力バッファサイズ
	 * 再生途中で表示画像サイズを変えられる事を考慮し、出力画像サイズではなく、
	 * 入力画像の画素数を元に計算する
	 */
	guint out_frame_size = 0;
	switch (me->output_format) {
	case GST_RTOH264DEC_OUT_FMT_YUV420:
		out_frame_size = me->width * me->height * 2;
		break;
	case GST_RTOH264DEC_OUT_FMT_RGB32:
		out_frame_size = me->width * me->height * 4;
		break;
	case GST_RTOH264DEC_OUT_FMT_RGB24:
		out_frame_size = me->width * me->height * 3;
		break;
	default:
		g_assert_not_reached ();
		break;
	}
	GST_INFO_OBJECT (me, "in_frame_size:%u, out_frame_size:%u",
					 in_frame_size, out_frame_size);

	/* デコード初期化パラメータセット		*/
	GST_INFO_OBJECT (me, "H264DEC INIT PARAM:");
	GST_INFO_OBJECT (me, " fmem_num:%u", me->fmem_num);
	GST_INFO_OBJECT (me, " buffering_pic_cnt:%u", me->buffering_pic_cnt);
	GST_INFO_OBJECT (me, " x_pic_size:%u", me->width);
	GST_INFO_OBJECT (me, " y_pic_size:%u", me->height);
	GST_INFO_OBJECT (me, " x_out_size:%u", me->out_width);
	GST_INFO_OBJECT (me, " y_out_size:%u", me->out_height);
	GST_INFO_OBJECT (me, " input_format:%" GST_FOURCC_FORMAT,
					 GST_FOURCC_ARGS (me->input_format));
	GST_INFO_OBJECT (me, " output_format:%" GST_FOURCC_FORMAT,
					 GST_FOURCC_ARGS (me->output_format));
	/* fmem_num */
	ctrl.id = V4L2_CID_NR_REFERENCE_FRAMES;
	ctrl.value = me->fmem_num;
	r = v4l2_ioctl(me->video_fd, VIDIOC_S_CTRL, &ctrl);
	if (r < 0) {
		goto set_init_param_failed;
	}
	/* buffering_pic_cnt */
	ctrl.id = V4L2_CID_NR_BUFFERING_PICS;
	ctrl.value = me->buffering_pic_cnt;
	r = v4l2_ioctl(me->video_fd, VIDIOC_S_CTRL, &ctrl);
	if (r < 0) {
		goto set_init_param_failed;
	}
	/* VIO6の有効無効フラグ : 未実装 */
	ctrl.id = V4L2_CID_ENABLE_VIO;
	ctrl.value = me->enable_vio6;
	r = v4l2_ioctl(me->video_fd, VIDIOC_S_CTRL, &ctrl);
	if (r < 0) {
		goto set_init_param_failed;
	}
	
	/* Set format for output (decoder input) */
	fmt.type 			= V4L2_BUF_TYPE_VIDEO_OUTPUT;
	fmt.fmt.pix.width	= me->width;
	fmt.fmt.pix.height	= me->height;
	fmt.fmt.pix.pixelformat = me->input_format;
	fmt.fmt.pix.field	= V4L2_FIELD_NONE;
	
	r = v4l2_ioctl(me->video_fd, VIDIOC_S_FMT, &fmt);
	if (r < 0) {
		goto set_init_param_failed;
	}

	/* Set format for capture (decoder output) */
	fmt.type			= V4L2_BUF_TYPE_VIDEO_CAPTURE;
	fmt.fmt.pix.width	= me->out_width;
	fmt.fmt.pix.height	= me->out_height;
	fmt.fmt.pix.pixelformat = me->output_format;
	fmt.fmt.pix.field	= V4L2_FIELD_NONE;
	
	r = v4l2_ioctl(me->video_fd, VIDIOC_S_FMT, &fmt);
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

		if (v4l2_ioctl (me->video_fd, VIDIOC_REQBUFS, &breq) < 0) {
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
			r = v4l2_ioctl (me->video_fd,
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
		memset(&v4l2InitParam, 0, sizeof(GstV4l2InitParam));
		if (me->using_fb_dmabuf) {
			v4l2InitParam.video_fd = me->video_fd;
			v4l2InitParam.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
			v4l2InitParam.mode = GST_V4L2_IO_DMABUF;
			v4l2InitParam.sizeimage = out_frame_size;
			v4l2InitParam.init_num_buffers = DEFAULT_NUM_BUFFERS_OUT_DMABUF;
			
			v4l2InitParam.num_fb_dmabuf = me->num_fb_dmabuf;
			for (i = 0; i < me->num_fb_dmabuf; i++) {
				v4l2InitParam.fb_dmabuf_fd[i] = me->fb_dmabuf_fd[i];
				v4l2InitParam.fb_dmabuf_index[i] = me->fb_dmabuf_index[i];
			}
		}
		else {
			v4l2InitParam.video_fd = me->video_fd;
			v4l2InitParam.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
			v4l2InitParam.mode = GST_V4L2_IO_MMAP;
			v4l2InitParam.sizeimage = out_frame_size;
			v4l2InitParam.init_num_buffers = DEFAULT_NUM_BUFFERS_OUT;
		}
		srcCaps = gst_caps_from_string ("video/x-raw");
		me->pool_out = gst_v4l2_buffer_pool_new(&v4l2InitParam, srcCaps);
		gst_caps_unref(srcCaps);
		if (! me->pool_out) {
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
gst_rto_h264_dec_cleanup_decoder (GstRtoH264Dec * me)
{
	enum v4l2_buf_type type;
	int r = 0;
	
	GST_INFO_OBJECT (me, "H264DEC CLEANUP RTO DECODER...");
	
	/* バッファプールのクリーンアップ	*/
	if (me->pool_out) {
		GST_DEBUG_OBJECT (me, "deactivating pool_out");
		GST_INFO_OBJECT (me, "pool_out - buffers:%d, allocated:%d, queued:%d",
						  me->pool_out->num_buffers,
						  me->pool_out->num_allocated,
						  me->pool_out->num_queued);
#if 0	/* for debug	*/
		gst_v4l2_buffer_pool_log_buf_status(me->pool_out);
#endif
		gst_buffer_pool_set_active (GST_BUFFER_POOL_CAST (me->pool_out), FALSE);
		gst_object_unref (me->pool_out);
		me->pool_out = NULL;
	}

	/* STREAMOFF */
	GST_INFO_OBJECT (me, "H264DEC STREAMOFF");
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

	return TRUE;
	
	/* ERRORS */
stop_failed:
	{
		GST_ELEMENT_ERROR (me, STREAM, DECODE, (NULL),
			("error with STREAMOFF %d (%s)", errno, g_strerror (errno)));
		return FALSE;
	}
}

static GstFlowReturn
gst_rto_h264_dec_handle_in_frame(GstRtoH264Dec * me, GstBuffer *inbuf)
{
	GstFlowReturn ret = GST_FLOW_OK;
	GstMapInfo map;
	int r;
	struct v4l2_buffer* pVBuffer = NULL;

	GST_DEBUG_OBJECT(me, "inbuf size=%d", gst_buffer_get_size(inbuf));

	/* dequeue buffer	*/
#if DBG_LOG_PERF_CHAIN
	GST_INFO_OBJECT (me, "H264DEC-CHAIN DQBUF START");
#endif
	if (me->num_inbuf_acquired < DEFAULT_NUM_BUFFERS_IN) {
		GST_INFO_OBJECT(me, "acquire_buffer : %d", me->num_inbuf_acquired);

		pVBuffer = &(me->priv->input_vbuffer[me->num_inbuf_acquired]);

		me->num_inbuf_acquired++;
	}
	else {
		pVBuffer = &(me->priv->input_vbuffer[0]);
		memset (pVBuffer, 0x00, sizeof (struct v4l2_buffer));
		pVBuffer->type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
		pVBuffer->memory = V4L2_MEMORY_USERPTR;
		r = v4l2_ioctl (me->video_fd, VIDIOC_DQBUF, pVBuffer);
		if (r < 0) {
			goto dqbuf_failed;
		}
	}
#if DBG_LOG_PERF_CHAIN
	GST_INFO_OBJECT (me, "H264DEC-CHAIN DQBUF END");
#endif

	/* 入力データを設定	*/
	gst_buffer_map(inbuf, &map, GST_MAP_READ);
	/* 入力データサイズを設定		*/
	pVBuffer->bytesused = map.size;
	pVBuffer->length = map.size;
	
	/* 入力データを設定		*/
	pVBuffer->m.userptr = (unsigned long)map.data;
	
#if 0	/* for debug */
	GST_INFO_OBJECT(me, "VIDIOC_QBUF %p, bytesused=%d, userptr=%p",
					pVBuffer, pVBuffer->bytesused, pVBuffer->m.userptr);
	GST_INFO_OBJECT (me, "  index:     %u", pVBuffer->index);
	GST_INFO_OBJECT (me, "  type:      %d", pVBuffer->type);
	GST_INFO_OBJECT (me, "  bytesused: %u", pVBuffer->bytesused);
	GST_INFO_OBJECT (me, "  flags:     %08x", pVBuffer->flags);
	if (V4L2_BUF_FLAG_QUEUED == (pVBuffer->flags & V4L2_BUF_FLAG_QUEUED)) {
		GST_INFO_OBJECT (me, "  V4L2_BUF_FLAG_QUEUED");
	}
	if (V4L2_BUF_FLAG_DONE == (pVBuffer->flags & V4L2_BUF_FLAG_DONE)) {
		GST_INFO_OBJECT (me, "  V4L2_BUF_FLAG_DONE");
	}
	GST_INFO_OBJECT (me, "  field:     %d", pVBuffer->field);
	GST_INFO_OBJECT (me, "  memory:    %d", pVBuffer->memory);
	GST_INFO_OBJECT (me, "  length:    %u", pVBuffer->length);
#endif

	/* enqueue buffer	*/
#if DBG_LOG_PERF_CHAIN
	GST_INFO_OBJECT (me, "H264DEC-CHAIN QBUF START");
#endif
	r = v4l2_ioctl (me->video_fd, VIDIOC_QBUF, pVBuffer);
	gst_buffer_unmap(inbuf, &map);
	if (r < 0) {
		goto qbuf_failed;
	}
#if DBG_LOG_PERF_CHAIN
	GST_INFO_OBJECT (me, "H264DEC-CHAIN QBUF END");
#endif

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
dqbuf_failed:
	{
		GST_ELEMENT_ERROR (me, STREAM, DECODE, (NULL),
			("could not dequeue buffer %d (%s)", errno, g_strerror (errno)));
		ret = GST_FLOW_ERROR;
		goto out;
	}
}

static GstFlowReturn
gst_rto_h264_dec_handle_out_frame(GstRtoH264Dec * me, gboolean* is_eos)
{
	GstFlowReturn ret = GST_FLOW_OK;
	GstBuffer *v4l2buf_out = NULL;
#if DO_PUSH_POOLS_BUF
	GstBufferPoolAcquireParams acquireParam;
#else
	GstMapInfo map;
#endif
#if DO_FRAME_DROP
	GstClockTimeDiff deadline;
#endif
	GstVideoCodecFrame *frame = NULL;
#if DBG_MEASURE_PERF_FINISH_FRAME
	static double interval_time_start = 0, interval_time_end = 0;
	double time_start = 0, time_end = 0;
#endif

	/* 出力引数初期化	*/
	if (NULL != is_eos) {
		*is_eos = FALSE;
	}

	/* dequeue buffer	*/
#if DBG_LOG_PERF_PUSH
	GST_INFO_OBJECT (me, "H264DEC-PUSH DQBUF START");
#endif
	ret = gst_v4l2_buffer_pool_dqbuf (me->pool_out, &v4l2buf_out);
	if (GST_FLOW_OK != ret) {
		GST_ERROR_OBJECT (me, "gst_v4l2_buffer_pool_dqbuf() returns %s",
						  gst_flow_get_name (ret));
		goto dqbuf_failed;
	}
#if DBG_LOG_PERF_PUSH
	GST_INFO_OBJECT (me, "H264DEC-PUSH DQBUF END");
#endif

	g_atomic_int_dec_and_test (&(me->priv->in_out_frame_count));

	GST_DEBUG_OBJECT(me, "v4l2buf_out size=%d", gst_buffer_get_size(v4l2buf_out));

	if (! me->using_fb_dmabuf) {
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

#if 0	/* for debug	*/
	{
		static int index = 0;
		gint i;
		GstMapInfo map_debug;
		char fileNameBuf[1024];
		sprintf(fileNameBuf, "rgb_%03d.data", ++index);
		
		FILE* fp = fopen(fileNameBuf, "w");
		if (fp) {
			gst_buffer_map (v4l2buf_out, &map_debug, GST_MAP_READ);
			for (i = 0; i < map_debug.size; i++) {
#if 0
				fprintf(fp, "0x%02X, ", map_debug.data[i]);
				if((i+1) % 8 == 0)
					fprintf(fp, "\n");
#else
				fputc(map_debug.data[i], fp);
#endif
			}
			gst_buffer_unmap (v4l2buf_out, &map_debug);
			fclose(fp);
		}
	}
#endif

	/* dequeue frame	*/
	frame = gst_video_decoder_get_oldest_frame(GST_VIDEO_DECODER (me));
	if (! frame) {
		goto no_frame;
	}
	gst_video_codec_frame_unref(frame);

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

		frame->output_buffer = v4l2buf_out;
		GST_DEBUG_OBJECT(me, "outbuf size=%d, ref:%d",
						 gst_buffer_get_size(v4l2buf_out),
						 GST_OBJECT_REFCOUNT_VALUE(v4l2buf_out));

		GST_DEBUG_OBJECT(me, "H264DEC FINISH FRAME:%p", v4l2buf_out);
		GST_DEBUG_OBJECT (me, "pool_out->num_queued is %d",
						  me->pool_out->num_queued);

		/* gs_buffer_unref() により、QBUF されるようにする	*/
#if 0	/* for debug	*/
		if (v4l2buf_out->pool) {
			GST_DEBUG_OBJECT(me,
				"outbuf->pool : %p (ref:%d), pool_out : %p (ref:%d)",
				v4l2buf_out->pool, GST_OBJECT_REFCOUNT_VALUE(v4l2buf_out->pool),
				me->pool_out, GST_OBJECT_REFCOUNT_VALUE(me->pool_out));
		}
		else {
			GST_DEBUG_OBJECT(me, "outbuf->pool is NULL");
		}
#endif

		/* GstBufferPool::priv::outstanding をインクリメントするためのダミー呼び出し。
		 * pool の deactivate の際、outstanding == 0 でないと、解放および unmap されないため
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
		
		/* src pad へ push	*/
#if DBG_LOG_PERF_PUSH
		GST_INFO_OBJECT (me, "H264DEC-PUSH gst_video_decoder_finish_frame START");
#endif
#if DBG_MEASURE_PERF_FINISH_FRAME
		interval_time_end = gettimeofday_sec();
		if (interval_time_start > 0) {
//			if ((interval_time_end - interval_time_start) > 0.022) {
			GST_INFO_OBJECT(me, "finish_frame() at(ms) : %10.10f",
							(interval_time_end - interval_time_start)*1e+3);
//			}
		}
		interval_time_start = gettimeofday_sec();
		time_start = gettimeofday_sec();
#endif
		displayingBuf = gst_buffer_ref(frame->output_buffer);
		ret = gst_video_decoder_finish_frame (GST_VIDEO_DECODER (me), frame);
		if (me->priv->displying_buf) {
			gst_buffer_unref(me->priv->displying_buf);
			me->priv->displying_buf = NULL;
		}
		if (GST_FLOW_OK != ret) {
			GST_WARNING_OBJECT (me, "gst_video_decoder_finish_frame() returns %s",
								gst_flow_get_name (ret));
			
			gst_buffer_unref(displayingBuf);
			displayingBuf = NULL;

			goto finish_frame_failed;
		}
		me->priv->displying_buf = displayingBuf;
#if DBG_MEASURE_PERF_FINISH_FRAME
		time_end = gettimeofday_sec();
//		if ((time_end - time_start) > 0.022) {
		GST_INFO_OBJECT(me, "finish_frame() : %10.10f", time_end - time_start);
//		}
#endif
#if DBG_LOG_PERF_PUSH
		GST_INFO_OBJECT (me, "H264DEC-PUSH gst_video_decoder_finish_frame END");
#endif
		GST_DEBUG_OBJECT(me, "output_segment.rate :%f",
						 (GST_VIDEO_DECODER(me))->output_segment.rate);

		GST_DEBUG_OBJECT (me, "pool_out->num_queued is %d",
						  me->pool_out->num_queued);
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
		gst_buffer_fill(frame->output_buffer,
						0, map.data, gst_buffer_get_size(v4l2buf_out));
		gst_buffer_unmap (v4l2buf_out, &map);
		gst_buffer_resize(frame->output_buffer, 0, gst_buffer_get_size(v4l2buf_out));

		GST_DEBUG_OBJECT(me, "outbuf size=%d",
						 gst_buffer_get_size(frame->output_buffer));
	}
	
	/* enqueue buffer	*/
	ret = gst_v4l2_buffer_pool_qbuf (
			me->pool_out, v4l2buf_out, gst_buffer_get_size(v4l2buf_out));
	if (GST_FLOW_OK != ret) {
		GST_ERROR_OBJECT (me, "gst_v4l2_buffer_pool_qbuf() returns %s",
						  gst_flow_get_name (ret));
		goto qbuf_failed;
	}
	
	/* src pad へ push	*/
    if (deadline >= 0) {
		GST_INFO_OBJECT(me, "H264DEC FINISH FRAME:%p", outbuf);
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
		ret = gst_v4l2_buffer_pool_qbuf (
			me->pool_out, v4l2buf_out, gst_buffer_get_size(v4l2buf_out));
		if (GST_FLOW_OK != ret) {
			GST_ERROR_OBJECT (me, "gst_v4l2_buffer_pool_qbuf() returns %s",
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
dqbuf_failed:
	{
		GST_ELEMENT_ERROR (me, STREAM, DECODE, (NULL),
			("could not dequeue buffer. %d (%s)", errno, g_strerror (errno)));
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
#else
allocate_outbuf_failed:
	{
		GST_ELEMENT_ERROR (me, RESOURCE, FAILED, (NULL),
						   ("failed allocate outbuf"));
		ret = GST_FLOW_ERROR;
		goto out;
	}
#endif
}

static gboolean
plugin_init (GstPlugin * plugin)
{
	GST_DEBUG_CATEGORY_INIT (rtoh264dec_debug, "rtoh264dec", 0, "H264 decoding");

	return gst_element_register (plugin, "rtoh264dec",
								 GST_RANK_PRIMARY, GST_TYPE_RTOH264DEC);
}

GST_PLUGIN_DEFINE (
	GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    rtoh264dec,
    "RTO H264 Decoder",
	plugin_init,
	VERSION,
	"GPL",
	"GStreamer",
	"http://gstreamer.net/"
);


/*
 * End of file
 */
