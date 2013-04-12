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

/**
 * SECTION:element-rtoaacdec
 *
 * rtoaacdec decodes AAC (MPEG-4 part 3) stream.
 *
 * <refsect2>
 * <title>Example launch lines</title>
 * |[
 * gst-launch filesrc location=example.mp4 ! qtdemux ! rtoaacdec ! audioconvert ! audioresample ! autoaudiosink
 * ]| Play aac from mp4 file.
 * |[
 * gst-launch filesrc location=example.adts ! rtoaacdec ! audioconvert ! audioresample ! autoaudiosink
 * ]| Play standalone aac bitstream.
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <sys/time.h>
#include <sched.h>
#include <gst/audio/audio.h>

#include "gstrtoaacdec.h"
#include "v4l2_util.h"


/* バッファプール内のバッファを no copy で down stream に push する	*/
#define DO_PUSH_POOLS_BUF			1

/* チャンネル数のプロパティ指定は意味ない	*/
#define ENABLE_CHANNEL_PROPERTY		0

/* ドライバ側にEOS通知を行う		*/
#define NOTIFY_EOS_TO_DRIVER		0

#if NOTIFY_EOS_TO_DRIVER
# define V4L2_CID_EOS				(V4L2_CID_PRIVATE_BASE + 0) /* notify EOS */
#endif

/* defined at acm-driver/include/acm-aacdec.h	*/
struct acm_aacdec_private_format {
	int bs_format;
	int pcm_format;
	uint32_t max_channel;
	bool down_mix;
	int down_mix_mode;
	int compliance;
	int sampling_rate_idx;
};

/* 確保するバッファ数	*/
#define DEFAULT_NUM_BUFFERS_IN			12
#define DEFAULT_NUM_BUFFERS_OUT			12

/* デコーダ初期化パラメータのデフォルト値	*/
#define DEFAULT_VIDEO_DEVICE		"/dev/video0"
#define DEFAULT_ALLOW_MIXDOWN		FALSE
#define DEFAULT_MIXDOWN_MODE		GST_RTOAACDEC_MIXDOWN_MODE_STEREO
#define DEFAULT_COMPLIANT_STANDARD	GST_RTOAACDEC_MIXDOWN_COMPLIANT_ISO
#define DEFAULT_PCM_FORMAT			GST_RTOAACDEC_PCM_FMT_INTERLEAVED
#define DEFAULT_MAX_CHANNEL			6

/* select() の timeout */
#define SELECT_TIMEOUT_MSEC			400

/* デバッグログ出力フラグ		*/
#define DBG_LOG_PERF_CHAIN			0
#define DBG_LOG_PERF_SELECT_IN		0
#define DBG_LOG_PERF_PUSH			0
#define DBG_LOG_PERF_SELECT_OUT		0


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


GST_DEBUG_CATEGORY_STATIC (rtoaacdec_debug);
#define GST_CAT_DEFAULT rtoaacdec_debug

/* property */
enum
{
	PROP_0,
	PROP_DEVICE,
	PROP_ALLOW_MIXDOWN,
	PROP_MIXDOWN_MODE,
	PROP_COMPLIANT_STANDARD,
	PROP_PCM_FORMAT,
#if ENABLE_CHANNEL_PROPERTY
	PROP_MAX_CHANNEL,
#endif
};

/* pad template caps for source and sink pads.	*/
static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("audio/mpeg, "
		"mpegversion = (int) 2; "
        "audio/mpeg, "
		"mpegversion = (int) 4, stream-format = (string) { raw, adts }")
    );

/* 16-bit per channel (interleaved or non interleaved). */
static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
	GST_STATIC_CAPS ("audio/x-raw, "
		"format = (string) " GST_AUDIO_NE(S16) ", "
		"rate = (int) [ 8000, 96000 ], "
		"channels = (int) [ 1, 8 ]")
    );

static int AACSamplingFrequency[16] = {
	96000, 88200, 64000, 48000,
	44100, 32000, 24000, 22050,
	16000, 12000, 11025, 8000,
	0, 0, 0, 0
};

/* Copied from vorbis; the particular values used don't matter */
static GstAudioChannelPosition channelpositions[][6] = {
	{	/* Mono */
		GST_AUDIO_CHANNEL_POSITION_MONO},
	{	/* Stereo */
        GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
		GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT},
	{	/* Stereo + Centre */
        GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
        GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
		GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER},
	{	/* Quadraphonic */
        GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
        GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
        GST_AUDIO_CHANNEL_POSITION_REAR_LEFT,
        GST_AUDIO_CHANNEL_POSITION_REAR_RIGHT,
	},
	{	/* Stereo + Centre + rear stereo */
        GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
        GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
        GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
        GST_AUDIO_CHANNEL_POSITION_REAR_LEFT,
        GST_AUDIO_CHANNEL_POSITION_REAR_RIGHT,
	},
	{	/* Full 5.1 Surround */
        GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
        GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
        GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
        GST_AUDIO_CHANNEL_POSITION_LFE1,
        GST_AUDIO_CHANNEL_POSITION_REAR_LEFT,
        GST_AUDIO_CHANNEL_POSITION_REAR_RIGHT,
	}
};

/*  サンプリング周波数パラメータ : R-MobileA1マルチメディアミドル 機能仕様書 Ver.0.14	*/
static gint
aac_rate_idx (gint rate)
{
	if (92017 <= rate)
		return 0;	// 96,000Hz
	else if (75132 <= rate)
		return 1;	// 88,200 Hz
	else if (55426 <= rate)
		return 2;	// 64,000 Hz
	else if (46009 <= rate)
		return 3;	// 48,000 Hz
	else if (37566 <= rate)
		return 4;	// 44,100 Hz
	else if (27713 <= rate)
		return 5;	// 32,000 Hz
	else if (23004 <= rate)
		return 6;	// 24,000 Hz
	else if (18783 <= rate)
		return 7;	// 22,050 Hz
	else if (13856 <= rate)
		return 8;	// 16,000 Hz
	else if (11502 <= rate)
		return 9;	// 12,000 Hz
	else if (9391 <= rate)
		return 10;	// 11,025 Hz
	else
		return 11;	// 8,000 Hz
}

static gboolean gst_rto_aac_dec_open (GstAudioDecoder * dec);
static gboolean gst_rto_aac_dec_close (GstAudioDecoder * dec);
static gboolean gst_rto_aac_dec_start (GstAudioDecoder * dec);
static gboolean gst_rto_aac_dec_stop (GstAudioDecoder * dec);
static gboolean gst_rto_aac_dec_set_format (GstAudioDecoder * dec,
	GstCaps * caps);
static gboolean gst_rto_aac_dec_update_caps (GstRtoAacDec * me);
static gboolean gst_rto_aac_dec_parse (GstAudioDecoder * dec,
	GstAdapter * adapter, gint * offset, gint * length);
static GstFlowReturn gst_rto_aac_dec_handle_frame (GstAudioDecoder * dec,
    GstBuffer * buffer);
static void gst_rto_aac_dec_flush (GstAudioDecoder * dec, gboolean hard);
static gboolean gst_rto_aac_dec_sink_event (GstAudioDecoder * dec,
	GstEvent *event);

static gboolean gst_rto_aac_dec_init_decoder (GstRtoAacDec * me);
static gboolean gst_rto_aac_dec_cleanup_decoder (GstRtoAacDec * me);
static GstFlowReturn gst_rto_aac_dec_handle_in_frame(GstRtoAacDec * me,
	GstBuffer *v4l2buf_in, GstBuffer *inbuf);
static GstFlowReturn gst_rto_aac_dec_handle_out_frame(GstRtoAacDec * me,
	GstBuffer *v4l2buf_out, gboolean* is_eos);

static void gst_rto_aac_dec_set_property (GObject * object,
	guint prop_id, const GValue * value, GParamSpec * pspec);
static void gst_rto_aac_dec_get_property (GObject * object,
	guint prop_id, GValue * value, GParamSpec * pspec);
static void gst_rto_aac_dec_finalize (GObject * object);

static GstFlowReturn gst_rto_aac_dec_chain (GstPad * pad,
	GstObject * parent, GstBuffer * buf);

#define gst_rto_aac_dec_parent_class parent_class
G_DEFINE_TYPE (GstRtoAacDec, gst_rto_aac_dec, GST_TYPE_AUDIO_DECODER);


static void
gst_rto_aac_dec_set_property (GObject * object, guint prop_id,
							   const GValue * value, GParamSpec * pspec)
{
	GstRtoAacDec *me = GST_RTOAACDEC (object);
	
	switch (prop_id) {
	case PROP_DEVICE:
		/* デバイスファイル名	*/
		if (me->videodev) {
			g_free (me->videodev);
		}
		me->videodev = g_value_dup_string (value);
		break;
	case PROP_ALLOW_MIXDOWN:
		me->allow_mixdown = g_value_get_boolean (value);
		break;
	case PROP_MIXDOWN_MODE:
		me->mixdown_mode = g_value_get_int (value);
		break;
	case PROP_COMPLIANT_STANDARD:
		me->compliant_standard = g_value_get_int (value);
		break;
	case PROP_PCM_FORMAT:
		me->pcm_format = g_value_get_int (value);
		break;
#if ENABLE_CHANNEL_PROPERTY
	case PROP_MAX_CHANNEL:
		me->out_channels = g_value_get_uint (value);
		break;
#endif
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
gst_rto_aac_dec_get_property (GObject * object, guint prop_id,
							   GValue * value, GParamSpec * pspec)
{
	GstRtoAacDec *me = GST_RTOAACDEC (object);
	
	switch (prop_id) {
	case PROP_DEVICE:
		g_value_set_string (value, me->videodev);
		break;
	case PROP_ALLOW_MIXDOWN:
		g_value_set_boolean (value, me->allow_mixdown);
		break;
	case PROP_MIXDOWN_MODE:
		g_value_set_int (value, me->mixdown_mode);
		break;
	case PROP_COMPLIANT_STANDARD:
		g_value_set_int (value, me->compliant_standard);
		break;
	case PROP_PCM_FORMAT:
		g_value_set_int (value, me->pcm_format);
		break;
#if ENABLE_CHANNEL_PROPERTY
	case PROP_MAX_CHANNEL:
		g_value_set_uint (value, me->out_channels);
		break;
#endif
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
gst_rto_aac_dec_class_init (GstRtoAacDecClass * klass)
{
	GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
	GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
	GstAudioDecoderClass *base_class = GST_AUDIO_DECODER_CLASS (klass);

	gobject_class->set_property = gst_rto_aac_dec_set_property;
	gobject_class->get_property = gst_rto_aac_dec_get_property;
	gobject_class->finalize = gst_rto_aac_dec_finalize;

	g_object_class_install_property (gobject_class, PROP_DEVICE,
		g_param_spec_string ("device", "device",
			"The video device eg: /dev/video0",
			DEFAULT_VIDEO_DEVICE, G_PARAM_READWRITE));

	g_object_class_install_property (gobject_class, PROP_ALLOW_MIXDOWN,
		g_param_spec_boolean ("allow-mixdown", "Allow Mixdown",
			"FALSE: do not mix down, TRUE: do mix down",
			DEFAULT_ALLOW_MIXDOWN, G_PARAM_READWRITE));

	g_object_class_install_property (gobject_class, PROP_MIXDOWN_MODE,
		g_param_spec_int ("mixdown-mode", "Mixdown Mode",
			"0: mix down to stereo, 1: mix down to mono",
			GST_RTOAACDEC_MIXDOWN_MODE_STEREO, GST_RTOAACDEC_MIXDOWN_MODE_MONO,
			DEFAULT_MIXDOWN_MODE, G_PARAM_READWRITE));

	g_object_class_install_property (gobject_class, PROP_COMPLIANT_STANDARD,
		g_param_spec_int ("compliant-standard", "Compliant Standard",
			"0:ISO/IEC13818-7, ISO/IEC14496-3, 1: ARIB STD-B21",
			GST_RTOAACDEC_MIXDOWN_COMPLIANT_ISO, GST_RTOAACDEC_MIXDOWN_COMPLIANT_ARIB,
			DEFAULT_COMPLIANT_STANDARD, G_PARAM_READWRITE));

	g_object_class_install_property (gobject_class, PROP_PCM_FORMAT,
		g_param_spec_int ("pcm-format", "PCM Format",
			"0: interleaved, 1:non interleaved",
			GST_RTOAACDEC_PCM_FMT_INTERLEAVED, GST_RTOAACDEC_PCM_FMT_NON_INTERLEAVED,
			DEFAULT_PCM_FORMAT, G_PARAM_READWRITE));

#if ENABLE_CHANNEL_PROPERTY
	g_object_class_install_property (gobject_class, PROP_MAX_CHANNEL,
		g_param_spec_uint ("max-channel", "Max Channel",
			"number of output channel",
			GST_RTOAACDEC_MAX_CHANNEL_MIN, GST_RTOAACDEC_MAX_CHANNEL_MAX,
			DEFAULT_MAX_CHANNEL, G_PARAM_READWRITE));
#endif

	gst_element_class_add_pad_template (element_class,
			gst_static_pad_template_get (&src_template));
	gst_element_class_add_pad_template (element_class,
			gst_static_pad_template_get (&sink_template));
	
	gst_element_class_set_static_metadata (element_class,
			"RTO AAC audio decoder", "Codec/Decoder/Audio",
			"RTO MPEG-4 AAC decoder", "atmark techno");
	
	base_class->open = GST_DEBUG_FUNCPTR (gst_rto_aac_dec_open);
	base_class->close = GST_DEBUG_FUNCPTR (gst_rto_aac_dec_close);
	base_class->start = GST_DEBUG_FUNCPTR (gst_rto_aac_dec_start);
	base_class->stop = GST_DEBUG_FUNCPTR (gst_rto_aac_dec_stop);
	base_class->set_format = GST_DEBUG_FUNCPTR (gst_rto_aac_dec_set_format);
	base_class->parse = GST_DEBUG_FUNCPTR (gst_rto_aac_dec_parse);
	base_class->handle_frame = GST_DEBUG_FUNCPTR (gst_rto_aac_dec_handle_frame);
	base_class->flush = GST_DEBUG_FUNCPTR (gst_rto_aac_dec_flush);
	base_class->sink_event = GST_DEBUG_FUNCPTR (gst_rto_aac_dec_sink_event);
}

static void
gst_rto_aac_dec_init (GstRtoAacDec * me)
{
	me->samplerate = 0;
	me->channels = 0;
	me->packetised = FALSE;

	me->is_eos_received = FALSE;
	me->is_decode_all_done = FALSE;
	me->is_do_stop = FALSE;

	me->out_samplerate = 0;
	me->out_channels = 0;

	me->video_fd = -1;
	me->is_handled_1stframe = FALSE;
	me->pool_in = NULL;
	me->pool_out = NULL;
	me->num_inbuf_acquired = 0;
	
	/* property	*/
	me->videodev = NULL;
	me->input_format = GST_RTOAACDEC_IN_FMT_UNKNOWN;
	me->allow_mixdown = DEFAULT_ALLOW_MIXDOWN;
	me->mixdown_mode = DEFAULT_MIXDOWN_MODE;
	me->compliant_standard = DEFAULT_COMPLIANT_STANDARD;
	me->pcm_format = DEFAULT_PCM_FORMAT;

	/* retrieve and intercept base class chain. */
	me->base_chain = GST_PAD_CHAINFUNC (GST_AUDIO_DECODER_SINK_PAD (me));
	gst_pad_set_chain_function (GST_AUDIO_DECODER_SINK_PAD (me),
								GST_DEBUG_FUNCPTR (gst_rto_aac_dec_chain));
}

static void
gst_rto_aac_dec_finalize (GObject * object)
{
	GstRtoAacDec *me = GST_RTOAACDEC (object);
	
    GST_INFO_OBJECT (me, "AACDEC FINALIZE");

	/* プロパティを保持するため、gst_rto_aac_dec_close() 内で free してはいけない	*/
	if (me->videodev) {
		g_free(me->videodev);
		me->videodev = NULL;
	}

	G_OBJECT_CLASS (parent_class)->finalize (object);
}

static gboolean
gst_rto_aac_dec_open (GstAudioDecoder * dec)
{
	GstRtoAacDec *me = GST_RTOAACDEC (dec);
	
	/* プロパティとしてセットされていなければ、デフォルト値を設定		*/
	if (NULL == me->videodev) {
		me->videodev = g_strdup (DEFAULT_VIDEO_DEVICE);
	}

	GST_INFO_OBJECT (me, "AACDEC OPEN RTO DECODER. (%s)", me->videodev);
	
	/* デバイスファイルオープン	*/
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
gst_rto_aac_dec_close (GstAudioDecoder * dec)
{
	GstRtoAacDec *me = GST_RTOAACDEC (dec);
	
	GST_INFO_OBJECT (me, "AACDEC CLOSE RTO DECODER. (%s)", me->videodev);

	/* close device	*/
	if (me->video_fd > 0) {
		gst_v4l2_close(me->videodev, me->video_fd);
		me->video_fd = -1;
	}
	
	return TRUE;
}

/*	global setup	*/
static gboolean
gst_rto_aac_dec_start (GstAudioDecoder * dec)
{
	GstRtoAacDec *me = GST_RTOAACDEC (dec);
	
	GST_INFO_OBJECT (me, "AACDEC START");
	
	/* call upon legacy upstream byte support (e.g. seeking) */
	gst_audio_decoder_set_estimate_rate (dec, TRUE);
	/* never mind a few errors */
	gst_audio_decoder_set_max_errors (dec, 10);
	/* don't bother us with flushing */
	gst_audio_decoder_set_drainable (dec, FALSE);

	return TRUE;
}

/*	end of all processing	*/
static gboolean
gst_rto_aac_dec_stop (GstAudioDecoder * dec)
{
	GstRtoAacDec *me = GST_RTOAACDEC (dec);
	
	GST_INFO_OBJECT (me, "AACDEC STOP");
	
	/* クリーンアップ処理	*/
	gst_rto_aac_dec_cleanup_decoder (me);

	/* プロパティ以外の変数を再初期化		*/
	me->samplerate = 0;
	me->channels = 0;
	me->packetised = FALSE;
	
	me->is_eos_received = FALSE;
	me->is_decode_all_done = FALSE;
	me->is_do_stop = FALSE;

	me->out_samplerate = 0;
	me->out_channels = 0;
	
	me->is_handled_1stframe = FALSE;
	me->pool_in = NULL;
	me->pool_out = NULL;
	me->num_inbuf_acquired = 0;

	return TRUE;
}

/*	format of input audio data	*/
static gboolean
gst_rto_aac_dec_set_format (GstAudioDecoder * dec, GstCaps * caps)
{
	GstRtoAacDec *me = GST_RTOAACDEC (dec);
	gboolean ret = TRUE;
	GstStructure *structure = gst_caps_get_structure (caps, 0);
	GstBuffer *buf = NULL;
	const GValue *value = NULL;
	GstMapInfo map;
	guint8 *cdata = NULL;
	gsize csize = 0;
	const gchar *stream_format = NULL;
    gint samplerate = 0;
    gint channels = 0;
	
	GST_INFO_OBJECT (me, "AACDEC SET FORMAT: %" GST_PTR_FORMAT, caps);
	
	/* Assume raw stream */
	me->packetised = FALSE;
	
	/* ATDS or RAW ?	*/
	stream_format = gst_structure_get_string (structure, "stream-format");
	GST_INFO_OBJECT(me, "stream format = %s",
					(NULL == stream_format ? "null" : stream_format));
	if (NULL == stream_format){
		me->input_format = GST_RTOAACDEC_IN_FMT_RAW; /* Assume raw */
	}
	else {
		if (g_str_equal(stream_format, "raw")){
			me->input_format = GST_RTOAACDEC_IN_FMT_RAW; /* RAW */
		}
		else if (g_str_equal(stream_format, "adts")){
			me->input_format = GST_RTOAACDEC_IN_FMT_ADTS; /* ADTS */
		}
		else {
			/* ADIF (およびその他の形式) は未サポート		*/
			GST_ELEMENT_ERROR (me, STREAM, DECODE, (NULL),
				("stream format : %s is unsupported.", stream_format));
			return FALSE;
		}
	}
	
	/* channels / samplerate */
	if (gst_structure_get_int (structure, "channels", &channels)) {
		me->channels = channels;
		GST_INFO_OBJECT (me, "channels : %d", me->channels);
	}
	if (gst_structure_get_int (structure, "rate", &samplerate)) {
		me->samplerate = samplerate;
		GST_INFO_OBJECT (me, "samplerate : %d", me->samplerate);
	}
	
	/* analize codec_data */
	if ((value = gst_structure_get_value (structure, "codec_data"))) {
		/* We have codec data, means packetised stream */
		me->packetised = TRUE;
		
		buf = gst_value_get_buffer (value);
		g_return_val_if_fail (buf != NULL, FALSE);
		
		gst_buffer_map (buf, &map, GST_MAP_READ);
		cdata = map.data;
		csize = map.size;
		
		if (csize < 2) {
			goto wrong_length;
		}
		
		GST_INFO_OBJECT (me,
						 "codec_data: object_type=%d, sample_rate=%d, channels=%d",
						 ((cdata[0] & 0xf8) >> 3),
						 (((cdata[0] & 0x07) << 1) | ((cdata[1] & 0x80) >> 7)),
						 ((cdata[1] & 0x78) >> 3));
		
		me->samplerate = AACSamplingFrequency[
						(((cdata[0] & 0x07) << 1) | ((cdata[1] & 0x80) >> 7))];
		me->channels = ((cdata[1] & 0x78) >> 3);
		
		GST_INFO_OBJECT (me, "codec_data init: channels=%u, rate=%u",
						  me->channels, (guint32) me->samplerate);
		
		gst_buffer_unmap (buf, &map);

#if 0	/* for debug	*/
		{
			gint i;
			GstMapInfo map_debug;
			FILE* fp = fopen("_codec_data.data", "w");
			if (fp) {
				gst_buffer_map (buf, &map_debug, GST_MAP_READ);
				for (i = 0; i < map_debug.size; i++) {
#if 0
					fprintf(fp, "0x%02X, ", map_debug.data[i]);
					if((i+1) % 8 == 0)
						fprintf(fp, "\n");
#else
					fputc(map_debug.data[i], fp);
#endif
				}
				gst_buffer_unmap (buf, &map_debug);
				fclose(fp);
			}
		}
#endif
	}
	else if ((value = gst_structure_get_value (structure, "framed"))
			 && g_value_get_boolean (value) == TRUE) {
		me->packetised = TRUE;
		GST_INFO_OBJECT (me, "we have packetized audio");
	}
	else {
		GST_INFO_OBJECT (me, "we have non packetized audio");
	}
	
	/* 出力フォーマットの決定	
	 * 入力が 1 チャンネルの時は、出力も 1 チャンネルになる
	 * 3 チャンネル以上は非イタリーブ形式になる
	 * [ミドル機能仕様書 v0.11 - ３.３.音声デコード仕様]
	 */
	me->out_samplerate = me->samplerate;
	if (0 == me->out_channels) {
		/* プロパティ設定がなければ、入力と同じチャンネル数	*/
		me->out_channels = me->channels;
	}
	if (me->out_channels < me->channels){
		/* デコード中に、指定したチャンネル数＋１を検出場合はエラーになるので、合わせる	*/
		me->out_channels = me->channels;
	}
	if (me->allow_mixdown) {
		if (GST_RTOAACDEC_MIXDOWN_MODE_STEREO == me->mixdown_mode) {
			me->out_channels = 2;
		}
		else if (GST_RTOAACDEC_MIXDOWN_MODE_MONO == me->mixdown_mode) {
			me->out_channels = 1;
		}
	}
	if (me->out_channels >= 3) {
		/* 3 チャンネル以上は非イタリーブ形式になる */
		GST_INFO_OBJECT (me, "output PCM is NON_INTERLEAVED (ch of input:%d)",
						 me->channels);
		me->pcm_format = GST_RTOAACDEC_PCM_FMT_NON_INTERLEAVED;
	}
	
	if (! gst_rto_aac_dec_update_caps (me)) {
		goto negotiation_failed;
	}

	/* デコーダ初期化	*/
	if (! gst_rto_aac_dec_init_decoder(me)) {
		goto init_failed;
	}

out:
	return ret;
	
	/* ERRORS */
wrong_length:
	{
		GST_ELEMENT_ERROR (me, STREAM, DECODE, (NULL),
			("codec_data less than 2 bytes long"));
		gst_object_unref (me);
		gst_buffer_unmap (buf, &map);
		ret = FALSE;
		goto out;
	}
negotiation_failed:
	{
		GST_ELEMENT_ERROR (me, CORE, NEGOTIATION, (NULL),
			("Setting caps on source pad failed"));
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

static gboolean
gst_rto_aac_dec_update_caps (GstRtoAacDec * me)
{
	gboolean ret = TRUE;
	GstAudioInfo ainfo;
	
	GST_DEBUG_OBJECT (me, "Update src caps");
	
	if (G_LIKELY (gst_pad_has_current_caps (GST_AUDIO_DECODER_SRC_PAD (me)))) {
		GST_DEBUG_OBJECT (me, "src pad has current caps.");
		return TRUE;
	}
	
	GST_INFO_OBJECT (me, "set caps to src pad.");
    GST_INFO_OBJECT (me, " channels=%d, rate=%d, %s",
					 me->out_channels, me->out_samplerate,
					 (GST_RTOAACDEC_PCM_FMT_INTERLEAVED == me->pcm_format
					  ? "interleaved" : "non interleaved"));
	gst_audio_info_init (&ainfo);
	gst_audio_info_set_format (&ainfo, GST_AUDIO_FORMAT_S16,
							   me->samplerate, me->out_channels,
							   channelpositions[me->out_channels - 1]);
	if (GST_RTOAACDEC_PCM_FMT_NON_INTERLEAVED == me->pcm_format) {
		/* 非インタリーブ	*/
		ainfo.layout = GST_AUDIO_LAYOUT_NON_INTERLEAVED;
	}

	ret = gst_audio_decoder_set_output_format (GST_AUDIO_DECODER (me), &ainfo);
	if (! ret) {
		GST_ELEMENT_ERROR (me, CORE, NEGOTIATION, (NULL),
			("failed set output format"));
		return FALSE;
	}
	ret = gst_audio_decoder_negotiate(GST_AUDIO_DECODER (me));
	if (! ret) {
		GST_ELEMENT_ERROR (me, CORE, NEGOTIATION, (NULL),
			("failed src caps negotiate"));
		return FALSE;
	}

	return ret;
}

/*
 * Find syncpoint in ADTS/ADIF stream. Doesn't work for raw,
 * packetized streams. Be careful when calling.
 * Returns FALSE on no-sync, fills offset/length if one/two
 * syncpoints are found, only returns TRUE when it finds two
 * subsequent syncpoints (similar to mp3 typefinding in
 * gst/typefind/) for ADTS because 12 bits isn't very reliable.
 */
static gboolean
gst_rto_aac_dec_sync (GstRtoAacDec * me, const guint8 * data, guint size, gboolean next,
    gint * off, gint * length)
{
	guint n = 0;
	gint snc;
	gboolean ret = FALSE;
	guint len = 0;
	
	GST_DEBUG_OBJECT (me, "Finding syncpoint");
	
	/* check for too small a buffer */
	if (size < 3) {
		goto exit;
	}
	
	for (n = 0; n < size - 3; n++) {
		snc = GST_READ_UINT16_BE (&data[n]);
		if ((snc & 0xfff6) == 0xfff0) {			
			/* we have an ADTS syncpoint. Parse length and find
			 * next syncpoint. */
			GST_DEBUG_OBJECT (me,
					"Found one ADTS syncpoint at offset 0x%x, tracing next...", n);
			
			if (size - n < 5) {
				GST_DEBUG_OBJECT (me, "Not enough data to parse ADTS header");
				break;
			}
			
			len = ((data[n + 3] & 0x03) << 11) |
			(data[n + 4] << 3) | ((data[n + 5] & 0xe0) >> 5);
			if (n + len + 2 >= size) {
				GST_DEBUG_OBJECT (me,
					"Frame size %d, next frame is not within reach", len);
				if (next) {
					break;
				}
				else if (n + len <= size) {
					GST_DEBUG_OBJECT (me, "but have complete frame and no next frame; "
						"accept ADTS syncpoint at offset 0x%x (framelen %u)", n, len);
					ret = TRUE;
					break;
				}
			}
			
			snc = GST_READ_UINT16_BE (&data[n + len]);
			if ((snc & 0xfff6) == 0xfff0) {
				GST_DEBUG_OBJECT (me,
					"Found ADTS syncpoint at offset 0x%x (framelen %u)", n, len);
				ret = TRUE;
				break;
			}
			
			GST_DEBUG_OBJECT (me, "No next frame found... (should be at 0x%x)",
							n + len);
		}
		else if (! memcmp (&data[n], "ADIF", 4)) {
			/* we have an ADIF syncpoint. 4 bytes is enough. */
			GST_DEBUG_OBJECT (me, "Found ADIF syncpoint at offset 0x%x", n);
			ret = TRUE;
			break;
		}
	}
	
exit:
	
	*off = n;
	
	if (ret) {
		*length = len;
	}
	else {
		GST_DEBUG_OBJECT (me, "Found no syncpoint");
	}
	
	return ret;
}

static GstFlowReturn
gst_rto_aac_dec_parse (GstAudioDecoder * dec, GstAdapter * adapter,
    gint * offset, gint * length)
{
	GstRtoAacDec *me = GST_RTOAACDEC (dec);
	const guint8 *data = NULL;
	guint size = 0;
	gboolean sync = FALSE, eos = FALSE;
	
	size = gst_adapter_available (adapter);
	g_return_val_if_fail (size > 0, GST_FLOW_ERROR);
	
	gst_audio_decoder_get_parse_state (dec, &sync, &eos);

	if (me->packetised) {
		*offset = 0;
		*length = size;
		
		return GST_FLOW_OK;
	}
	else {
		gboolean ret;
		
		data = gst_adapter_map (adapter, size);
		ret = gst_rto_aac_dec_sync (me, data, size, !eos, offset, length);
		gst_adapter_unmap (adapter);
		
		return (ret ? GST_FLOW_OK : GST_FLOW_EOS);
	}
}

static GstFlowReturn
gst_rto_aac_dec_chain (GstPad * pad, GstObject * parent, GstBuffer * buf)
{
	GstRtoAacDec *me = GST_RTOAACDEC (parent);
	GstFlowReturn ret = GST_FLOW_OK;
	
	/* first frame
	 * gst_rto_aac_dec_set_format() が呼ばれて、デコーダの初期化を行う
	 * 念のため入力フォーマットをチェック
	 */
	if (! me->is_handled_1stframe) {
#if 1
		/* 初回データのヘッダをチェック	*/
		GstMapInfo map;
		gst_buffer_map (buf, &map, GST_MAP_READ);
		if (map.size >= 4) {
			if (map.data[0] == 'A' && map.data[1] == 'D'
				&& map.data[2] == 'I' && map.data[3] == 'F') {
				/* ADIF type header : 未サポート	*/
				gst_buffer_unmap (buf, &map);
				
				GST_ELEMENT_ERROR (me, STREAM, DECODE, (NULL),
								   ("ADIF type is unsupported"));
				return GST_FLOW_ERROR;
			}
			else if (map.data[0] == 0xff && (map.data[1] >> 4) == 0xf) {
				/* ADTS type header */
				me->input_format = GST_RTOAACDEC_IN_FMT_ADTS;
				GST_INFO_OBJECT (me, "ADTS type detected");
			}
			else {
				me->input_format = GST_RTOAACDEC_IN_FMT_RAW;
				GST_INFO_OBJECT (me, "RAW type detected");
			}
		}
		else {
			me->input_format = GST_RTOAACDEC_IN_FMT_RAW;
			GST_INFO_OBJECT (me, "RAW type detected");
		}
		gst_buffer_unmap (buf, &map);
#endif
		me->is_handled_1stframe = TRUE;
	}

	/* call gst_audio_decoder_chain() */
	ret = me->base_chain (pad, parent, buf);
	
	return ret;
}

static GstFlowReturn
gst_rto_aac_dec_handle_frame (GstAudioDecoder * dec, GstBuffer * buffer)
{
	GstRtoAacDec *me = GST_RTOAACDEC (dec);
	GstFlowReturn ret = GST_FLOW_OK;
	int r = 0;
	fd_set write_fds;
	fd_set read_fds;
	struct timeval tv;
	GstBuffer *v4l2buf_out = NULL;
	GstBuffer *v4l2buf_in = NULL;
#if DBG_MEASURE_PERF_HANDLE_FRAME
	static double interval_time_start = 0, interval_time_end = 0;
#endif
#if DBG_MEASURE_PERF_SELECT_IN || DBG_MEASURE_PERF_SELECT_OUT
	double time_start, time_end;
#endif

#if DBG_LOG_PERF_CHAIN
	GST_INFO_OBJECT (me, "# AACDEC-CHAIN HANDLE FRMAE START");
#endif

	/* no fancy draining */
	if (G_UNLIKELY (! buffer)) {
		GST_DEBUG_OBJECT (me, "AACDEC HANDLE FRMAE NULL.");

		goto out;
	}

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

	GST_DEBUG_OBJECT (me, "AACDEC HANDLE FRMAE - size:%d, ref:%d, flags:%d ...",
					  gst_buffer_get_size(buffer),
					  GST_OBJECT_REFCOUNT_VALUE(buffer),
					  GST_BUFFER_FLAGS(buffer));


	/* Seek が行われた際は、gst_audio_decoder_reset() により、dec->priv->frames が、
	 * 空になる。これにより、本関数の引数 buffer は、gst_audio_decoder_finish_frame()
	 * を call した際に、unref される。そのため、本関数の処理中は、ref して保持しておく必要
	 * がある。
	 */
	gst_buffer_ref(buffer);

#if 0	/* for debug	*/
	{
		static int index = 0;
		gint i;
		GstMapInfo map_debug;
		char fileNameBuf[1024];
		sprintf(fileNameBuf, "aac_%03d.data", ++index);
		
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

	/* 出力 : REQBUF 分入力した後は、デコード済みデータができるのを待って取り出す	*/
	if (me->num_inbuf_acquired >= me->pool_in->num_buffers) {
		do {
			/* dequeue buffer	*/
#if DBG_LOG_PERF_PUSH
			GST_INFO_OBJECT (me, "AACDEC-PUSH DQBUF START");
#endif
			ret = gst_v4l2_buffer_pool_dqbuf (me->pool_out, &v4l2buf_out);
			if (GST_FLOW_DQBUF_EAGAIN == ret) {
#if DBG_LOG_PERF_PUSH
				GST_INFO_OBJECT (me, "AACDEC-PUSH DQBUF EAGAIN");
#endif

				/* 読み込みできる状態になるまで待ってから読み込む		*/
#if DBG_LOG_PERF_SELECT_OUT
				GST_INFO_OBJECT(me, "wait until enable dqbuf (pool_out)");
				gst_v4l2_buffer_pool_log_buf_status(me->pool_out);
#endif
#if DBG_LOG_PERF_PUSH
				GST_INFO_OBJECT (me, "AACDEC-PUSH SELECT START");
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
				} while (r == -1 && (errno == EINTR || errno == EAGAIN));
#if DBG_LOG_PERF_PUSH
				GST_INFO_OBJECT (me, "AACDEC-PUSH SELECT END");
#endif

				if (r > 0) {
#if DBG_LOG_PERF_PUSH
					GST_INFO_OBJECT (me, "AACDEC-PUSH DQBUF RESTART");
#endif
					ret = gst_v4l2_buffer_pool_dqbuf(me->pool_out, &v4l2buf_out);
					if (GST_FLOW_OK != ret) {
						GST_ERROR_OBJECT (me, "gst_v4l2_buffer_pool_dqbuf() returns %s",
										  gst_flow_get_name (ret));
						goto dqbuf_failed;
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
			else if (GST_FLOW_OK != ret) {
				GST_ERROR_OBJECT (me, "gst_v4l2_buffer_pool_dqbuf() returns %s",
								  gst_flow_get_name (ret));
				goto dqbuf_failed;
			}
#if DBG_LOG_PERF_PUSH
			GST_INFO_OBJECT (me, "AACDEC-PUSH DQBUF END");
#endif

#if DBG_LOG_PERF_PUSH
			GST_INFO_OBJECT (me, "AACDEC-PUSH HANDLE OUT START");
#endif
			ret = gst_rto_aac_dec_handle_out_frame(me, v4l2buf_out, NULL);
			if (GST_FLOW_OK != ret) {
				goto handle_out_failed;
			}
#if DBG_LOG_PERF_PUSH
			GST_INFO_OBJECT (me, "AACDEC-PUSH HANDLE OUT END");
#endif
		} while (FALSE);
	}

	/* 入力		*/
	/* dequeue buffer	*/
	if (me->num_inbuf_acquired < me->pool_in->num_buffers) {
		GST_DEBUG_OBJECT(me, "acquire_buffer");
		ret = gst_v4l2_buffer_pool_acquire_buffer(
			GST_BUFFER_POOL_CAST (me->pool_in), &v4l2buf_in, NULL);
		if (GST_FLOW_OK != ret) {
			GST_ERROR_OBJECT (me, "gst_v4l2_buffer_pool_acquire_buffer() returns %s",
							  gst_flow_get_name (ret));
			goto no_buffer;
		}
		
		me->num_inbuf_acquired++;
	}
	else {
#if DBG_LOG_PERF_CHAIN
		GST_INFO_OBJECT (me, "AACDEC-CHAIN DQBUF START");
#endif

		GST_DEBUG_OBJECT(me, "dqbuf (not acquire_buffer)");
		ret = gst_v4l2_buffer_pool_dqbuf(me->pool_in, &v4l2buf_in);
		if (GST_FLOW_DQBUF_EAGAIN == ret) {
#if DBG_LOG_PERF_SELECT_IN
			GST_INFO_OBJECT(me, "wait until enable dqbuf (pool_in)");
			gst_v4l2_buffer_pool_log_buf_status(me->pool_out);
#endif
			/* 書き込みができる状態になるまで待ってから書き込む		*/
#if DBG_LOG_PERF_CHAIN
			GST_INFO_OBJECT (me, "AACDEC-CHAIN SELECT START");
#endif
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
			} while (r == -1 && (errno == EINTR || errno == EAGAIN));
#if DBG_LOG_PERF_CHAIN
			GST_INFO_OBJECT (me, "AACDEC-CHAIN SELECT END");
#endif

			if (r > 0) {
				ret = gst_v4l2_buffer_pool_dqbuf(me->pool_in, &v4l2buf_in);
				if (GST_FLOW_OK != ret) {
					GST_ERROR_OBJECT (me, "gst_v4l2_buffer_pool_dqbuf() returns %s",
									  gst_flow_get_name (ret));
					goto dqbuf_failed;
				}
			}
			else if (r < 0) {
				goto select_failed;
			}
			else if (0 == r) {
				goto select_timeout;
			}
		}
		else if (GST_FLOW_OK != ret) {
			GST_ERROR_OBJECT (me, "gst_v4l2_buffer_pool_dqbuf() returns %s",
							  gst_flow_get_name (ret));
			goto dqbuf_failed;
		}
#if DBG_LOG_PERF_CHAIN
		GST_INFO_OBJECT (me, "AACDEC-CHAIN DQBUF END");
#endif
	}
	ret = gst_rto_aac_dec_handle_in_frame(me, v4l2buf_in, buffer);
	if (GST_FLOW_OK != ret) {
		goto handle_in_failed;
	}

#if 0
	/* 出力:現時点で読み込み可能なデコード済みデータを全て取り出す	*/
	do {
		/* dequeue buffer	*/
		ret = gst_v4l2_buffer_pool_dqbuf (me->pool_out, &v4l2buf_out);
		if (GST_FLOW_OK != ret) {
			if (GST_FLOW_DQBUF_EAGAIN == ret) {
				ret = GST_FLOW_OK;
				break;
			}
			
			GST_ERROR_OBJECT (me, "gst_v4l2_buffer_pool_dqbuf() returns %s",
							  gst_flow_get_name (ret));
			goto dqbuf_failed;
		}
		
		ret = gst_rto_aac_dec_handle_out_frame(me, v4l2buf_out, NULL);
		if (GST_FLOW_OK != ret) {
			goto handle_out_failed;
		}
	} while (TRUE);
#endif

out:
#if DBG_LOG_PERF_CHAIN
	GST_INFO_OBJECT (me, "# AACDEC-CHAIN HANDLE FRMAE END");
#endif
#if 0	/* for debug	*/
	GST_INFO_OBJECT(me, "inbuf size=%d, ref:%d",
					 gst_buffer_get_size(buffer),
					 GST_OBJECT_REFCOUNT_VALUE(buffer));
#endif
	gst_buffer_unref(buffer);

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
		GST_ERROR_OBJECT (me, "pool_in - buffers:%d, allocated:%d, queued:%d",
						  me->pool_in->num_buffers,
						  me->pool_in->num_allocated,
						  me->pool_in->num_queued);
		
		GST_ERROR_OBJECT (me, "pool_out - buffers:%d, allocated:%d, queued:%d",
						  me->pool_out->num_buffers,
						  me->pool_out->num_allocated,
						  me->pool_out->num_queued);
		GST_ELEMENT_ERROR (me, STREAM, DECODE, (NULL),
			("timeout with select()"));
		ret = GST_FLOW_ERROR;
		goto out;
	}
no_buffer:
	{
		GST_ELEMENT_ERROR (me, RESOURCE, FAILED, (NULL),
			("could not allocate buffer"));
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
		goto out;
	}
#if 0
negotiation_failed:
	{
		GST_ELEMENT_ERROR (me, CORE, NEGOTIATION, (NULL),
			("Setting caps on source pad failed"));
		ret = GST_FLOW_ERROR;
		goto out;
	}
#endif
}

static void
gst_rto_aac_dec_flush (GstAudioDecoder * dec, gboolean hard)
{
	GstRtoAacDec *me = GST_RTOAACDEC (dec);
	GST_INFO_OBJECT (me, "AACDEC FLUSH");

	/* do nothing	*/
}

static gboolean
gst_rto_aac_dec_sink_event (GstAudioDecoder * dec, GstEvent *event)
{
	GstRtoAacDec *me = GST_RTOAACDEC (dec);
	gboolean ret = FALSE;

	GST_DEBUG_OBJECT (me, "RECEIVED EVENT (%d)", GST_EVENT_TYPE(event));
	switch (GST_EVENT_TYPE(event)) {
	case GST_EVENT_CAPS:
	{
		GstCaps * caps = NULL;

		gst_event_parse_caps (event, &caps);
		GST_INFO_OBJECT (me, "AACDEC received GST_EVENT_CAPS: %" GST_PTR_FORMAT,
						 caps);

		ret = GST_AUDIO_DECODER_CLASS (parent_class)->sink_event(dec, event);
		break;
	}
	case GST_EVENT_EOS:
	{
		gboolean isEOS = FALSE;
		GstBuffer *v4l2buf_out = NULL;
#if NOTIFY_EOS_TO_DRIVER
		int r = 0;
#endif

		GST_INFO_OBJECT (me, "AACDEC received GST_EVENT_EOS");

#if NOTIFY_EOS_TO_DRIVER
		/* EOS をデバイスに通知	*/
		r = v4l2_ioctl(me->video_fd, V4L2_CID_EOS, NULL);
		if (r < 0) {
			goto notify_eos_failed;
		}
#endif

		/* デバイス側に溜まっているデータを取り出して、down stream へ流す	*/
		GST_INFO_OBJECT (me, "delay len : %d",
						 gst_audio_decoder_get_delay(dec));
		while (gst_audio_decoder_get_delay(dec) > 0) {
			do {
				/* dequeue buffer	*/
				ret = gst_v4l2_buffer_pool_dqbuf (me->pool_out, &v4l2buf_out);
				if (GST_FLOW_OK != ret) {
					if (GST_FLOW_DQBUF_EAGAIN == ret) {
//						GST_WARNING_OBJECT (me, "DQBUF_EAGAIN after EOS");
						break;
					}
					
					GST_ERROR_OBJECT (me, "gst_v4l2_buffer_pool_dqbuf() returns %s",
									  gst_flow_get_name (ret));
					goto dqbuf_failed;
				}
				
				ret = gst_rto_aac_dec_handle_out_frame(me, v4l2buf_out, &isEOS);
				if (GST_FLOW_OK != ret) {
					goto handle_out_failed;
				}
				if (isEOS) {
					break;
				}
			} while (FALSE);
		}

		ret = GST_AUDIO_DECODER_CLASS (parent_class)->sink_event(dec, event);
		break;
	}
	case GST_EVENT_STREAM_START:
		GST_DEBUG_OBJECT (me, "received GST_EVENT_STREAM_START");
		/* break;	*/
	case GST_EVENT_SEGMENT:
		GST_DEBUG_OBJECT (me, "received GST_EVENT_SEGMENT");
		/* break;	*/
	default:
		ret = GST_AUDIO_DECODER_CLASS (parent_class)->sink_event(dec, event);
		break;
	}
	
out:
	return ret;

dqbuf_failed:
	{
		GST_ELEMENT_ERROR (me, STREAM, DECODE, (NULL),
			("could not dequeue buffer. %d (%s)", errno, g_strerror (errno)));
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
		ret = FALSE;
		goto out;
	}
#if NOTIFY_EOS_TO_DRIVER
notify_eos_failed:
	{
		GST_ELEMENT_ERROR (me, STREAM, DECODE, (NULL),
			("Failed to notufy EOS to device.(%s)", g_strerror (errno)));
		ret = FALSE;
		goto out;
	}
#endif
}

static gboolean
gst_rto_aac_dec_init_decoder (GstRtoAacDec * me)
{
	gboolean ret = TRUE;
	enum v4l2_buf_type type;
	int r;
	GstCaps *sinkCaps, *srcCaps;
	GstV4l2InitParam v4l2InitParam;
	struct v4l2_format fmt;
	struct acm_aacdec_private_format* pfmt;

	GST_DEBUG_OBJECT (me, "AACDEC INITIALIZE RTO DECODER...");

	/* デコード初期化パラメータセット		*/
	GST_INFO_OBJECT (me,
		 "AACDEC INIT PARAM : bs_format=%d, channels=%d, "
		 "mixdown=%d, mixdown_mode=%d, compliant=%d, "
		 "pcm_format=%d, rate_idx=%d (%d Hz)",
		 me->input_format, me->channels,
		 me->allow_mixdown, me->mixdown_mode,
		 me->compliant_standard, me->pcm_format,
		 aac_rate_idx(me->out_samplerate), me->out_samplerate);
	memset(&fmt, 0, sizeof(struct v4l2_format));
	pfmt = (struct acm_aacdec_private_format*)fmt.fmt.raw_data;
	pfmt->bs_format = me->input_format;
	pfmt->pcm_format = me->pcm_format;
	pfmt->max_channel = me->channels;
	if (me->allow_mixdown) {
		pfmt->down_mix = GST_RTOAACDEC_ALLOW_MIXDOWN;
	}
	else {
		pfmt->down_mix = GST_RTOAACDEC_NOT_ALLOW_MIXDOWN;
	}
	pfmt->down_mix_mode = me->mixdown_mode;
	pfmt->compliance = me->compliant_standard;
	pfmt->sampling_rate_idx = aac_rate_idx(me->out_samplerate);

	/* Set format for output (decoder input), capture (decoder output)  */
	fmt.type = V4L2_BUF_TYPE_PRIVATE;
	r = v4l2_ioctl(me->video_fd, VIDIOC_S_FMT, &fmt);
	if (r < 0) {
		goto set_init_param_failed;
	}

	/* バッファプールのセットアップ	*/
	if (NULL == me->pool_in) {
		memset(&v4l2InitParam, 0, sizeof(GstV4l2InitParam));
		v4l2InitParam.video_fd = me->video_fd;
		v4l2InitParam.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
		v4l2InitParam.mode = GST_V4L2_IO_MMAP;
		/* 1フレーム(1024サンプル) x 16bit x チャンネル数分を確保 */
		v4l2InitParam.sizeimage = 1024 * 2 * me->channels;
		GST_DEBUG_OBJECT (me, "in frame size : %d", v4l2InitParam.sizeimage);
		v4l2InitParam.init_num_buffers = DEFAULT_NUM_BUFFERS_IN;
		sinkCaps = gst_caps_from_string ("audio/mpeg");
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
		/* 1フレーム(出力)の最大サイズ	(2048 word, 16-bit, 8 channels) */
		v4l2InitParam.sizeimage = 2048 * 2 * me->out_channels;
		GST_DEBUG_OBJECT (me, "out frame size : %d", v4l2InitParam.sizeimage);
		v4l2InitParam.init_num_buffers = DEFAULT_NUM_BUFFERS_OUT;
		srcCaps = gst_caps_from_string ("audio/x-raw");
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
	GST_INFO_OBJECT (me, "AACDEC STREAMON");
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
gst_rto_aac_dec_cleanup_decoder (GstRtoAacDec * me)
{
	enum v4l2_buf_type type;
	int r;
	
	GST_INFO_OBJECT (me, "AACDEC CLEANUP RTO DECODER...");
	
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
	GST_INFO_OBJECT (me, "AACDEC STREAMOFF");
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
gst_rto_aac_dec_handle_in_frame(GstRtoAacDec * me,
	GstBuffer *v4l2buf_in, GstBuffer *inbuf)
{
	GstFlowReturn ret = GST_FLOW_OK;
	GstMapInfo map;
	gsize inputDataSize = 0;
	
	GST_DEBUG_OBJECT(me, "inbuf size=%d", gst_buffer_get_size(inbuf));

	/* 入力データをコピー	*/
#if DBG_LOG_PERF_CHAIN
	GST_INFO_OBJECT (me, "AACDEC-CHAIN COPY START");
#endif
	gst_buffer_map(inbuf, &map, GST_MAP_READ);
	inputDataSize = map.size;
	gst_buffer_fill(v4l2buf_in, 0, map.data, map.size);
	gst_buffer_unmap(inbuf, &map);
//    gst_buffer_resize (v4l2buf_in, 0, gst_buffer_get_size(inbuf));
	GST_DEBUG_OBJECT(me, "v4l2buf_in size:%d, input_size:%d",
					 gst_buffer_get_size(v4l2buf_in), inputDataSize);
#if DBG_LOG_PERF_CHAIN
	GST_INFO_OBJECT (me, "AACDEC-CHAIN COPY END");
#endif

	/* enqueue buffer	*/
#if DBG_LOG_PERF_CHAIN
	GST_INFO_OBJECT (me, "AACDEC-CHAIN QBUF START");
#endif
	ret = gst_v4l2_buffer_pool_qbuf (me->pool_in, v4l2buf_in, inputDataSize);
	if (GST_FLOW_OK != ret) {
		GST_ERROR_OBJECT (me, "gst_v4l2_buffer_pool_qbuf() returns %s",
						 gst_flow_get_name (ret));
		goto qbuf_failed;
	}
#if DBG_LOG_PERF_CHAIN
	GST_INFO_OBJECT (me, "AACDEC-CHAIN QBUF END");
#endif

out:
	return ret;
	
	/* ERRORS */
qbuf_failed:
	{
		GST_ELEMENT_ERROR (me, STREAM, DECODE, (NULL),
			("could not queue buffer. %d (%s)", errno, g_strerror (errno)));
		ret = GST_FLOW_ERROR;
		goto out;
	}
}

static GstFlowReturn
gst_rto_aac_dec_handle_out_frame(GstRtoAacDec * me,
	GstBuffer *v4l2buf_out, gboolean* is_eos)
{
	GstFlowReturn ret = GST_FLOW_OK;
	GstBuffer *outbuf = NULL;
#if DO_PUSH_POOLS_BUF
	GstBufferPoolAcquireParams acquireParam;
#endif
#if DBG_MEASURE_PERF_FINISH_FRAME
	static double interval_time_start = 0, interval_time_end = 0;
	double time_start = 0, time_end = 0;
#endif

	/* 出力引数初期化	*/
	if (NULL != is_eos) {
		*is_eos = FALSE;
	}

	GST_DEBUG_OBJECT(me, "v4l2buf_out size=%d", gst_buffer_get_size(v4l2buf_out));

#if 0	/* for debug	*/
	{
		static int index = 0;
		gint i;
		GstMapInfo map_debug;
		char fileNameBuf[1024];
		sprintf(fileNameBuf, "pcm_%03d.data", ++index);

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

#if NOTIFY_EOS_TO_DRIVER
	/* check EOS	*/
	if (0 == gst_buffer_get_size(v4l2buf_out)) {
		/* もう、デバイス側に、デコードデータは無い	*/
		GST_INFO_OBJECT(me, "all decoded on the device");

		if (NULL != is_eos) {
			*is_eos = TRUE;
		}

		return ret;
	}
#endif

#if DO_PUSH_POOLS_BUF

	/* sink エレメントに push したバッファ (v4l2buf_out) は、sink エレメント側で、unref
	 * される事により、QBUF されるが、sink エレメント側の処理が遅く、mem2mem デバイスへの
	 * キューイングが枯渇状態にある時は、push せず、mem2mem デバイスに QBUF で戻す。
	 */
	if (me->pool_out->num_queued <= 0) {
#if 0	/* drop は、sink に任せる		*/
		GST_WARNING_OBJECT(me, "SKIP FINISH FRAME (rendering too late)");
		/* an queue the buffer again */
		ret = gst_v4l2_buffer_pool_qbuf (
				me->pool_out, v4l2buf_out, gst_buffer_get_size(v4l2buf_out));
		if (GST_FLOW_OK != ret) {
			GST_ERROR_OBJECT (me, "gst_v4l2_buffer_pool_qbuf() returns %s",
							 gst_flow_get_name (ret));
			goto qbuf_failed;
		}
		
		return ret;
#else
		GST_WARNING_OBJECT(me, "AACDEC rendering too late");
#endif
	}
	
	outbuf = v4l2buf_out;

//	GST_DEBUG_OBJECT(me, "outbuf size=%d, ref:%d",
//		gst_buffer_get_size(outbuf), GST_OBJECT_REFCOUNT_VALUE(outbuf));

	GST_DEBUG_OBJECT(me, "AACDEC FINISH FRAME : %p", outbuf);

	/* gs_buffer_unref() により、QBUF されるようにする	*/
#if 0	/* for debug	*/
	if (outbuf->pool) {
		GST_DEBUG_OBJECT(me, "outbuf->pool : %p (ref:%d), pool_out : %p (ref:%d)",
						 outbuf->pool, GST_OBJECT_REFCOUNT_VALUE(outbuf->pool),
						 me->pool_out, GST_OBJECT_REFCOUNT_VALUE(me->pool_out));
	}
	else {
		GST_DEBUG_OBJECT(me, "outbuf->pool is NULL");
	}
#endif

	/* GstBufferPool::priv::outstanding をインクリメントするためのダミー呼び出し。
	 * pool の deactivate の際、outstanding == 0 でないと、解放および unmap されないため
	 */
#if DBG_LOG_PERF_PUSH
	GST_INFO_OBJECT (me, "AACDEC-PUSH SET POOL START");
#endif
	acquireParam.flags = GST_BUFFER_POOL_ACQUIRE_FLAG_LAST;
	ret = gst_buffer_pool_acquire_buffer(GST_BUFFER_POOL_CAST(me->pool_out),
										 &outbuf, &acquireParam);
	if (GST_FLOW_OK != ret) {
		GST_ERROR_OBJECT (me, "gst_buffer_pool_acquire_buffer() returns %s",
						  gst_flow_get_name (ret));
		goto acquire_buffer_failed;
	}

	GST_DEBUG_OBJECT(me, "outbuf->pool : %p (ref:%d), pool_out : %p (ref:%d)",
					 outbuf->pool, GST_OBJECT_REFCOUNT_VALUE(outbuf->pool),
					 me->pool_out, GST_OBJECT_REFCOUNT_VALUE(me->pool_out));
#if DBG_LOG_PERF_PUSH
	GST_INFO_OBJECT (me, "AACDEC-PUSH SET POOL END");
#endif

	/* src pad へ push	*/
#if DBG_LOG_PERF_PUSH
	GST_INFO_OBJECT (me, "AACDEC-PUSH gst_audio_decoder_finish_frame START");
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
	ret = gst_audio_decoder_finish_frame (GST_AUDIO_DECODER(me), outbuf, 1);
	if (GST_FLOW_OK != ret) {
		GST_WARNING_OBJECT (me, "gst_audio_decoder_finish_frame() returns %s",
							gst_flow_get_name (ret));
		goto finish_frame_failed;
	}
#if DBG_MEASURE_PERF_FINISH_FRAME
	time_end = gettimeofday_sec();
//	if ((time_end - time_start) > 0.022) {
		GST_INFO_OBJECT(me, "finish_frame()   (ms): %10.10f",
						(time_end - time_start)*1e+3);
//	}
#endif
#if DBG_LOG_PERF_PUSH
	GST_INFO_OBJECT (me, "AACDEC-PUSH gst_audio_decoder_finish_frame END");
#endif

#else	/* DO_PUSH_POOLS_BUF */
	
	/* 出力バッファをアロケート	*/
	outbuf = gst_buffer_copy(v4l2buf_out);
	if (NULL == outbuf) {
		goto allocate_outbuf_failed;
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
#if DBG_LOG_PERF_PUSH
	GST_INFO_OBJECT (me, "AACDEC-PUSH gst_audio_decoder_finish_frame START");
#endif
	ret = gst_audio_decoder_finish_frame (GST_AUDIO_DECODER(me), outbuf, 1);
#if DBG_LOG_PERF_PUSH
	GST_INFO_OBJECT (me, "AACDEC-PUSH gst_audio_decoder_finish_frame END");
#endif
	if (GST_FLOW_OK != ret) {
		GST_ERROR_OBJECT (me, "gst_audio_decoder_finish_frame() returns %s",
						  gst_flow_get_name (ret));
		goto finish_frame_failed;
	}

#endif	/* DO_PUSH_POOLS_BUF */
	
out:
	return ret;
	
	/* ERRORS */
#if ! DO_PUSH_POOLS_BUF
qbuf_failed:
	{
		GST_ELEMENT_ERROR (me, STREAM, DECODE, (NULL),
			("could not queue buffer. %d (%s)", errno, g_strerror (errno)));
		ret = GST_FLOW_ERROR;
		goto out;
	}
#endif
finish_frame_failed:
	{
		if (GST_FLOW_NOT_LINKED == ret || GST_FLOW_FLUSHING == ret) {
			GST_WARNING_OBJECT (me,
				"failed gst_audio_decoder_finish_frame() - not link or flushing");
			
//			ret = GST_FLOW_ERROR;
			goto out;
		}

		GST_ELEMENT_ERROR (me, STREAM, DECODE, (NULL),
			("failed gst_audio_decoder_finish_frame()"));
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
	GST_DEBUG_CATEGORY_INIT (rtoaacdec_debug, "rtoaacdec", 0, "AAC decoding");

	return gst_element_register (plugin, "rtoaacdec",
							   GST_RANK_PRIMARY, GST_TYPE_RTOAACDEC);
}

GST_PLUGIN_DEFINE (
	GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    rtoaacdec,
    "RTO AAC Decoder",
    plugin_init,
	VERSION,
	"GPL",
	"GStreamer",
	"http://gstreamer.net/"
);

/*
 * End of file
 */
