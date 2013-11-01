/* GStreamer
 *
 * unit test for acmaacenc
 *
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

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#include <gst/check/gstcheck.h>
#include <gst/audio/audio.h>

#include "utest_util.h"


/* 出力フォーマット ADIF のサポート	*/
#define SUPPORT_OUTPUT_FMT_ADIF		0

/* input channel */
enum {
	IN_CH_1,
	IN_CH_2,
};

/* encode to raw / ADTS / ADIF */
enum {
	FMT_UNKNOWN,
	FMT_RAW,
	FMT_ADTS,
#if SUPPORT_OUTPUT_FMT_ADIF
	FMT_ADIF,
#endif
};

/* For ease of programming we use globals to keep refs for our floating
 * src and sink pads we create; otherwise we always have to do get_pad,
 * get_peer, and then remove references in every test function */
static GstPad *mysrcpad, *mysinkpad;

#define SAMPLE_RATES	" 8000, " \
						"11025, " \
						"12000, " \
						"16000, " \
						"22050, " \
						"24000, " \
						"32000, " \
						"44100, " \
						"48000 "

#define AUDIO_1CH_CAPS_STRING "audio/x-raw, " \
	"format = (string) " GST_AUDIO_NE (S16) ", " \
	"layout = (string) interleaved, " \
	"rate = (int) 44100, " \
	"channels = (int) 1 "

#define AUDIO_2CH_CAPS_STRING "audio/x-raw, " \
	"format = (string) " GST_AUDIO_NE (S16) ", " \
	"layout = (string) interleaved, " \
	"rate = (int) 44100, " \
	"channels = (int) 2, " \
	"channel-mask = (bitmask) 3"

#define AAC_RAW_CAPS_STRING "audio/mpeg, " \
	"mpegversion = (int) 4, " \
	"rate = (int) {" SAMPLE_RATES "}, " \
	"channels = (int) [ 1, 2 ], " \
	"stream-format = \"raw\""

#define AAC_ADTS_CAPS_STRING "audio/mpeg, " \
	"mpegversion = (int) 4, " \
	"rate = (int) {" SAMPLE_RATES "}, " \
	"channels = (int) [ 1, 2 ], " \
	"stream-format = \"adts\""

#if SUPPORT_OUTPUT_FMT_ADIF
#define AAC_ADIF_CAPS_STRING "audio/mpeg, " \
	"mpegversion = (int) 4, " \
	"rate = (int) {" SAMPLE_RATES "}, " \
	"channels = (int) [ 1, 2 ], " \
	"stream-format = \"adif\""
#endif

/* output */
static GstStaticPadTemplate sinktemplate_adts = GST_STATIC_PAD_TEMPLATE ("sink",
	GST_PAD_SINK,
	GST_PAD_ALWAYS,
	GST_STATIC_CAPS (AAC_ADTS_CAPS_STRING));

/* output */
static GstStaticPadTemplate sinktemplate_raw = GST_STATIC_PAD_TEMPLATE ("sink",
	GST_PAD_SINK,
	GST_PAD_ALWAYS,
	GST_STATIC_CAPS (AAC_RAW_CAPS_STRING));

#if SUPPORT_OUTPUT_FMT_ADIF
/* output */
static GstStaticPadTemplate sinktemplate_adif = GST_STATIC_PAD_TEMPLATE ("sink",
	GST_PAD_SINK,
	GST_PAD_ALWAYS,
	GST_STATIC_CAPS (AAC_ADIF_CAPS_STRING));
#endif

/* input */
static GstStaticPadTemplate srctemplate_1ch = GST_STATIC_PAD_TEMPLATE ("src",
	GST_PAD_SRC,
	GST_PAD_ALWAYS,
	GST_STATIC_CAPS (AUDIO_1CH_CAPS_STRING));

static GstStaticPadTemplate srctemplate_2ch = GST_STATIC_PAD_TEMPLATE ("src",
	GST_PAD_SRC,
	GST_PAD_ALWAYS,
	GST_STATIC_CAPS (AUDIO_2CH_CAPS_STRING));


/* number of the buffers which push */
#define PUSH_BUFFERS	100

/* chain function of sink pad */
GstPadChainFunction g_sink_base_chain = NULL;

/* output data file path */
static char g_output_data_file_path[PATH_MAX];


/* setup */
static GstElement *
setup_acmaacenc (int inputCH, int encodeType)
{
	GstElement *acmaacenc;

	g_print ("setup_acmaacenc\n");
	acmaacenc = gst_check_setup_element ("acmaacenc");
	g_print ("pass : gst_check_setup_element()\n");

	switch(inputCH) {
	case IN_CH_1:
		mysrcpad = gst_check_setup_src_pad (acmaacenc, &srctemplate_1ch);
		break;
	case IN_CH_2:
		mysrcpad = gst_check_setup_src_pad (acmaacenc, &srctemplate_2ch);
		break;
	default:
		g_assert_not_reached ();
		break;
	}
	g_print ("pass : gst_check_setup_src_pad()\n");

	switch (encodeType) {
	case FMT_RAW:
		mysinkpad = gst_check_setup_sink_pad (acmaacenc, &sinktemplate_raw);
		break;
	case FMT_ADTS:
		mysinkpad = gst_check_setup_sink_pad (acmaacenc, &sinktemplate_adts);
		break;
#if SUPPORT_OUTPUT_FMT_ADIF
	case FMT_ADIF:
		mysinkpad = gst_check_setup_sink_pad (acmaacenc, &sinktemplate_adif);
		break;
#endif
	default:
		g_assert_not_reached ();
		break;
	}
	g_print ("pass : gst_check_setup_sink_pad()\n");

	gst_pad_set_active (mysrcpad, TRUE);
	gst_pad_set_active (mysinkpad, TRUE);
	
	return acmaacenc;
}

/* cleanup */
static void
cleanup_acmaacenc (GstElement * acmaacenc)
{
	g_print ("cleanup_acmaacenc\n");
	gst_element_set_state (acmaacenc, GST_STATE_NULL);
	
	gst_pad_set_active (mysrcpad, FALSE);
	gst_pad_set_active (mysinkpad, FALSE);
	gst_check_teardown_src_pad (acmaacenc);
	gst_check_teardown_sink_pad (acmaacenc);
	gst_check_teardown_element (acmaacenc);
}

/* property set / get */
GST_START_TEST (test_properties)
{
	GstElement *acmaacenc;
	gchar *device;
	gint bit_rate;
	gint enable_cbr;

	/* setup */
	acmaacenc = setup_acmaacenc (IN_CH_1, FMT_RAW);
	
	/* set and check properties */
	g_object_set (acmaacenc,
				  "device", 		"/dev/video1",
				  "bitrate", 		16000,
				  "enable-cbr", 	1,
				  NULL);
	g_object_get (acmaacenc,
				  "device", 		&device,
				  "bitrate", 		&bit_rate,
				  "enable-cbr", 	&enable_cbr,
				  NULL);
	fail_unless (g_str_equal (device, "/dev/video1"));
	fail_unless_equals_int (bit_rate, 16000);
	fail_unless_equals_int (enable_cbr, 1);
	g_free (device);
	device = NULL;
	
	/* new properties */
	g_object_set (acmaacenc,
				  "device", 		"/dev/video2",
				  "bitrate", 		288000,
				  "enable-cbr", 	0,
				  NULL);
	g_object_get (acmaacenc,
				  "device", 		&device,
				  "bitrate", 		&bit_rate,
				  "enable-cbr", 	&enable_cbr,
				  NULL);
	fail_unless (g_str_equal (device, "/dev/video2"));
	fail_unless_equals_int (bit_rate, 288000);
	fail_unless_equals_int (enable_cbr, 0);
	g_free (device);
	device = NULL;

	/* cleanup	*/
	cleanup_acmaacenc (acmaacenc);
}
GST_END_TEST;

/* Combination of a property */
static void
check_property_combination(gint sample_rate, gint bit_rate, GstFlowReturn result)
{
	GstElement *acmaacenc;
	GstBuffer *buffer;
	GstCaps *caps;
	GstBuffer *outbuffer;
	
	/* setup */
	acmaacenc = setup_acmaacenc (IN_CH_2, FMT_RAW);
	g_object_set (acmaacenc,
				  "bitrate",			bit_rate,
				  NULL);
	gst_element_set_state (acmaacenc, GST_STATE_PLAYING);
	
	/* make caps */
	caps = gst_caps_from_string (AUDIO_1CH_CAPS_STRING);
	gst_caps_set_simple (caps, "rate", G_TYPE_INT, sample_rate, NULL);
	fail_unless (gst_pad_set_caps (mysrcpad, caps));
	
	/* create buffer */
	fail_unless ((buffer = create_audio_buffer (caps)) != NULL);
	
	/* push buffer */
	fail_unless (gst_pad_push (mysrcpad, buffer) == result);
	
	/* release encoded data */
	if (g_list_length (buffers) > 0) {
		outbuffer = GST_BUFFER (buffers->data);
		buffers = g_list_remove (buffers, outbuffer);
		
		ASSERT_BUFFER_REFCOUNT (outbuffer, "outbuffer", 1);
		gst_buffer_unref (outbuffer);
		outbuffer = NULL;
	}
	
	/* cleanup */
	gst_caps_unref (caps);
	gst_element_set_state (acmaacenc, GST_STATE_NULL);
	cleanup_acmaacenc (acmaacenc);
}

GST_START_TEST (test_property_combination)
{
	/* 入力 PCM のサンプリング周波数に対する、ビットレート指定の範囲チェック */

	/* 8,000Hz : 16,000 － 48,000 */
	check_property_combination(8000, 16000, GST_FLOW_OK);
	check_property_combination(8000, 16000 + 1, GST_FLOW_OK);
	check_property_combination(8000, 48000, GST_FLOW_OK);
	check_property_combination(8000, 48000 - 1, GST_FLOW_OK);
#if 0	/* g_object_class_install_property() にて下限を指定済み */
	check_property_combination(8000, 16000 - 1, GST_FLOW_NOT_NEGOTIATED);
#endif
	check_property_combination(8000, 48000 + 1, GST_FLOW_NOT_NEGOTIATED);

	/* 11,025Hz : 22,000 － 66,150 */
	check_property_combination(11025, 22000, GST_FLOW_OK);
	check_property_combination(11025, 22000 + 1, GST_FLOW_OK);
	check_property_combination(11025, 66150, GST_FLOW_OK);
	check_property_combination(11025, 66150 - 1, GST_FLOW_OK);
	check_property_combination(11025, 22000 - 1, GST_FLOW_NOT_NEGOTIATED);
	check_property_combination(11025, 66150 + 1, GST_FLOW_NOT_NEGOTIATED);

	/* 12,000Hz : 24,000 － 72,000 */
	check_property_combination(12000, 24000, GST_FLOW_OK);
	check_property_combination(12000, 24000 + 1, GST_FLOW_OK);
	check_property_combination(12000, 72000, GST_FLOW_OK);
	check_property_combination(12000, 72000 - 1, GST_FLOW_OK);
	check_property_combination(12000, 24000 - 1, GST_FLOW_NOT_NEGOTIATED);
	check_property_combination(12000, 72000 + 1, GST_FLOW_NOT_NEGOTIATED);

	/* 16,000Hz : 23,250 － 96,000 */
	check_property_combination(16000, 23250, GST_FLOW_OK);
	check_property_combination(16000, 23250 + 1, GST_FLOW_OK);
	check_property_combination(16000, 96000, GST_FLOW_OK);
	check_property_combination(16000, 96000 - 1, GST_FLOW_OK);
	check_property_combination(16000, 23250 - 1, GST_FLOW_NOT_NEGOTIATED);
	check_property_combination(16000, 96000 + 1, GST_FLOW_NOT_NEGOTIATED);

	/* 22,050Hz : 32,000 － 132,300 */
	check_property_combination(22050, 32000, GST_FLOW_OK);
	check_property_combination(22050, 32000 + 1, GST_FLOW_OK);
	check_property_combination(22050, 132300, GST_FLOW_OK);
	check_property_combination(22050, 132300 - 1, GST_FLOW_OK);
	check_property_combination(22050, 32000 - 1, GST_FLOW_NOT_NEGOTIATED);
	check_property_combination(22050, 132300 + 1, GST_FLOW_NOT_NEGOTIATED);

	/* 24,000Hz : 35,000 － 144,000 */
	check_property_combination(24000, 35000, GST_FLOW_OK);
	check_property_combination(24000, 35000 + 1, GST_FLOW_OK);
	check_property_combination(24000, 144000, GST_FLOW_OK);
	check_property_combination(24000, 144000 - 1, GST_FLOW_OK);
	check_property_combination(24000, 35000 - 1, GST_FLOW_NOT_NEGOTIATED);
	check_property_combination(24000, 144000 + 1, GST_FLOW_NOT_NEGOTIATED);

	/* 32,000Hz : 46,500 － 192,000 */
	check_property_combination(32000, 46500, GST_FLOW_OK);
	check_property_combination(32000, 46500 + 1, GST_FLOW_OK);
	check_property_combination(32000, 192000, GST_FLOW_OK);
	check_property_combination(32000, 192000 - 1, GST_FLOW_OK);
	check_property_combination(32000, 46500 - 1, GST_FLOW_NOT_NEGOTIATED);
	check_property_combination(32000, 192000 + 1, GST_FLOW_NOT_NEGOTIATED);

	/* 44,100Hz : 64,000 － 264,600 */
	check_property_combination(44100, 64000, GST_FLOW_OK);
	check_property_combination(44100, 64000 + 1, GST_FLOW_OK);
	check_property_combination(44100, 264600, GST_FLOW_OK);
	check_property_combination(44100, 264600 - 1, GST_FLOW_OK);
	check_property_combination(44100, 64000 - 1, GST_FLOW_NOT_NEGOTIATED);
	check_property_combination(44100, 264600 + 1, GST_FLOW_NOT_NEGOTIATED);

	/* 48,000Hz : 70,000 － 288,000 */
	check_property_combination(48000, 70000, GST_FLOW_OK);
	check_property_combination(48000, 70000 + 1, GST_FLOW_OK);
	check_property_combination(48000, 288000, GST_FLOW_OK);
	check_property_combination(48000, 288000 - 1, GST_FLOW_OK);
	check_property_combination(48000, 70000 - 1, GST_FLOW_NOT_NEGOTIATED);
#if 0	/* g_object_class_install_property() にて上限を指定済み */
	check_property_combination(48000, 288000 + 1, GST_FLOW_NOT_NEGOTIATED);
#endif
}
GST_END_TEST;

/* check caps	*/
static void
check_caps(int inputCH, int encodeType)
{
	GstElement *acmaacenc;
	GstCaps *srccaps;
    GstCaps *outcaps;
	GstBuffer *inbuffer, *outbuffer;
	int i, num_buffers;
	const gint nbuffers = 3;
	int ch_type = 0;
	gchar* aac_type = NULL;

	/* setup */
	acmaacenc = setup_acmaacenc (inputCH, encodeType);
	fail_unless (gst_element_set_state (acmaacenc, GST_STATE_PLAYING)
				 == GST_STATE_CHANGE_SUCCESS, "could not set to playing");

	switch (encodeType) {
	case FMT_RAW:
		aac_type = "raw";
		break;
	case FMT_ADTS:
		aac_type = "adts";
		break;
	default:
		g_assert_not_reached ();
		break;
	}

	/* set src caps */
	switch (inputCH) {
	case IN_CH_1:
		srccaps = gst_caps_from_string (AUDIO_1CH_CAPS_STRING);
		ch_type = 1;
		break;
	case IN_CH_2:
		srccaps = gst_caps_from_string (AUDIO_2CH_CAPS_STRING);
		ch_type = 2;
		break;
	default:
		g_assert_not_reached ();
		break;
	}
	gst_pad_set_caps (mysrcpad, srccaps);

	/* corresponds to audio buffer mentioned in the caps */
	/* 1024 sample x 16bit x channel */
	inbuffer = gst_buffer_new_and_alloc (1024 * nbuffers * 2 * ch_type);
	/* makes valgrind's memcheck happier */
	gst_buffer_memset (inbuffer, 0, 0, 1024 * nbuffers * 2 * ch_type);
	GST_BUFFER_TIMESTAMP (inbuffer) = 0;
	ASSERT_BUFFER_REFCOUNT (inbuffer, "inbuffer", 1);
	fail_unless (gst_pad_push (mysrcpad, inbuffer) == GST_FLOW_OK);
	
	/* send eos to have all flushed if needed */
	fail_unless (gst_pad_push_event (mysrcpad, gst_event_new_eos ()) == TRUE);
	
	num_buffers = g_list_length (buffers);
	g_print ("num_buffers : %d\n", num_buffers);
	fail_unless (num_buffers == nbuffers);

	/* check the out caps */
	outcaps = gst_pad_get_current_caps (mysinkpad);
	g_print ("outcaps : %p\n", outcaps);
	{
		GstStructure *s;
		const GValue *sf, *ch;
		const gchar *stream_format;
		gint channels;

		fail_unless (outcaps != NULL);

		GST_INFO ("outcaps %" GST_PTR_FORMAT "\n", outcaps);
		s = gst_caps_get_structure (outcaps, 0);
		fail_unless (s != NULL);
		fail_if (!gst_structure_has_name (s, "audio/mpeg"));
		sf = gst_structure_get_value (s, "stream-format");
		fail_unless (sf != NULL);
		fail_unless (G_VALUE_HOLDS_STRING (sf));
		stream_format = g_value_get_string (sf);
		fail_unless (stream_format != NULL);
		if (strcmp (stream_format, aac_type) == 0) {
			ch = gst_structure_get_value (s, "channels");
			fail_unless (ch != NULL);
			channels = g_value_get_int (ch);
			fail_unless (channels == ch_type);
		}
		else {
			fail_if (TRUE, "unexpected stream-format in caps: %s", stream_format);
		}
	}

	/* clean up buffers */
	for (i = 0; i < num_buffers; ++i) {
		outbuffer = GST_BUFFER (buffers->data);
		fail_if (outbuffer == NULL);
		
		buffers = g_list_remove (buffers, outbuffer);
		
		ASSERT_BUFFER_REFCOUNT (outbuffer, "outbuffer", 1);
		gst_buffer_unref (outbuffer);
		outbuffer = NULL;
	}

	/* cleanup */
	cleanup_acmaacenc (acmaacenc);
	gst_caps_unref (srccaps);
    gst_caps_unref (outcaps);
	if (buffers) {
		g_list_free (buffers);
		buffers = NULL;
	}
}

GST_START_TEST (test_check_caps)
{
	check_caps(IN_CH_1, FMT_RAW);
	check_caps(IN_CH_1, FMT_RAW);
	check_caps(IN_CH_2, FMT_ADTS);
	check_caps(IN_CH_2, FMT_ADTS);
}
GST_END_TEST;

/* AAC encode	*/
static GstFlowReturn
test_encode_sink_chain(GstPad * pad, GstObject * parent, GstBuffer * buf)
{
	size_t size;
	void *p;
	char file[PATH_MAX];
	static gint nOutputBuffers = 0;
	GstBuffer *outbuffer;
	GstMapInfo map;
	GstFlowReturn ret;
	
	ret = g_sink_base_chain(pad, parent, buf);
	
	/* check outputed buffer */
	if (g_list_length (buffers) > 0) {
		++nOutputBuffers;
		
		outbuffer = GST_BUFFER (buffers->data);
		fail_if (outbuffer == NULL);
		fail_unless (GST_IS_BUFFER (outbuffer));
		
		sprintf(file, g_output_data_file_path, nOutputBuffers);
		g_print("%s\n", file);
		get_data(file, &size, &p);
		
//		g_print("size %d : %d\n", gst_buffer_get_size (outbuffer), size);
		fail_unless (gst_buffer_get_size (outbuffer) == size);
		gst_buffer_map (outbuffer, &map, GST_MAP_READ);
		fail_unless (0 == memcmp(p, map.data, size));
		gst_buffer_unmap (outbuffer, &map);

		fail_unless (0 == munmap(p, size));
		
		buffers = g_list_remove (buffers, outbuffer);
		
		ASSERT_BUFFER_REFCOUNT (outbuffer, "outbuffer", 1);
		gst_buffer_unref (outbuffer);
		outbuffer = NULL;
	}
	
	return ret;
}

static void
input_buffers(int num_bufs, char* data_path)
{
	size_t size;
	void *p;
	char file[PATH_MAX];
	GstBuffer *inbuffer;
	gint nInputBuffers = 0;

	while (TRUE) {
		if (++nInputBuffers < num_bufs) {
			sprintf(file, data_path, nInputBuffers);
			g_print("%s\n", file);
			
			get_data(file, &size, &p);
			
			inbuffer = gst_buffer_new_and_alloc (size);
			gst_buffer_fill (inbuffer, 0, p, size);
			
			fail_unless (0 == munmap(p, size));
			
			GST_BUFFER_TIMESTAMP (inbuffer) = 0;
			ASSERT_BUFFER_REFCOUNT (inbuffer, "inbuffer", 1);
			
			fail_unless (gst_pad_push (mysrcpad, inbuffer) == GST_FLOW_OK);
			
//			gst_buffer_unref (inbuffer);
		}
		
		if (nInputBuffers == num_bufs) {
			/* push EOS event */
			fail_unless (gst_pad_push_event (mysrcpad,
				gst_event_new_eos ()) == TRUE);
			
			break;
		}
	}
}

GST_START_TEST (test_encode_prop01)
{
	GstElement *acmaacenc;
	GstCaps *srccaps;

	/* encode test 1
	 * 1 channel input
	 * ADTS format
	 */

	/* setup */
	acmaacenc = setup_acmaacenc (IN_CH_1, FMT_ADTS);
	fail_unless (gst_element_set_state (acmaacenc, GST_STATE_PLAYING)
				 == GST_STATE_CHANGE_SUCCESS, "could not set to playing");

	g_sink_base_chain = GST_PAD_CHAINFUNC (mysinkpad);
	gst_pad_set_chain_function (mysinkpad,
		GST_DEBUG_FUNCPTR (test_encode_sink_chain));

	strcpy(g_output_data_file_path, "data/aac_enc/propset01/aac_%03d.data");

	/* set src caps */
	srccaps = gst_caps_from_string (AUDIO_1CH_CAPS_STRING);
	gst_pad_set_caps (mysrcpad, srccaps);

	/* input buffers */
	input_buffers(PUSH_BUFFERS, "data/aac_enc/input01/pcm_%03d.data");

	/* cleanup */
	cleanup_acmaacenc (acmaacenc);
	g_list_free (buffers);
	buffers = NULL;
	gst_caps_unref (srccaps);
}
GST_END_TEST;

GST_START_TEST (test_encode_prop02)
{
	GstElement *acmaacenc;
	GstCaps *srccaps;
	
	/* encode test 2
	 * 1 channel input
	 * RAW format
	 */
	
	/* setup */
	acmaacenc = setup_acmaacenc (IN_CH_1, FMT_RAW);
	fail_unless (gst_element_set_state (acmaacenc, GST_STATE_PLAYING)
				 == GST_STATE_CHANGE_SUCCESS, "could not set to playing");
	
	g_sink_base_chain = GST_PAD_CHAINFUNC (mysinkpad);
	gst_pad_set_chain_function (mysinkpad,
								GST_DEBUG_FUNCPTR (test_encode_sink_chain));
	
	strcpy(g_output_data_file_path, "data/aac_enc/propset02/aac_%03d.data");
	
	/* set src caps */
	srccaps = gst_caps_from_string (AUDIO_1CH_CAPS_STRING);
	gst_pad_set_caps (mysrcpad, srccaps);
	
	/* input buffers */
	input_buffers(PUSH_BUFFERS, "data/aac_enc/input01/pcm_%03d.data");
	
	/* cleanup */
	cleanup_acmaacenc (acmaacenc);
	g_list_free (buffers);
	buffers = NULL;
	gst_caps_unref (srccaps);
}
GST_END_TEST;

#if SUPPORT_OUTPUT_FMT_ADIF
GST_START_TEST (test_encode_prop03)
{
	GstElement *acmaacenc;
	GstCaps *srccaps;
	
	/* encode test 3
	 * 1 channel input
	 * ADIF format
	 */
	
	/* setup */
	acmaacenc = setup_acmaacenc (IN_CH_1, FMT_ADIF);
	fail_unless (gst_element_set_state (acmaacenc, GST_STATE_PLAYING)
				 == GST_STATE_CHANGE_SUCCESS, "could not set to playing");
	
	g_sink_base_chain = GST_PAD_CHAINFUNC (mysinkpad);
	gst_pad_set_chain_function (mysinkpad,
								GST_DEBUG_FUNCPTR (test_encode_sink_chain));
	
	strcpy(g_output_data_file_path, "data/aac_enc/propset03/aac_%03d.data");
	
	/* set src caps */
	srccaps = gst_caps_from_string (AUDIO_1CH_CAPS_STRING);
	gst_pad_set_caps (mysrcpad, srccaps);
	
	/* input buffers */
	input_buffers(PUSH_BUFFERS, "data/aac_enc/input01/pcm_%03d.data");

	/* cleanup */
	cleanup_acmaacenc (acmaacenc);
	g_list_free (buffers);
	buffers = NULL;
	gst_caps_unref (srccaps);
}
GST_END_TEST;
#endif

GST_START_TEST (test_encode_prop11)
{
	GstElement *acmaacenc;
	GstCaps *srccaps;
	
	/* encode test 11
	 * 2 channel input
	 * ADTS format
	 */
	
	/* setup */
	acmaacenc = setup_acmaacenc (IN_CH_2, FMT_ADTS);
	fail_unless (gst_element_set_state (acmaacenc, GST_STATE_PLAYING)
				 == GST_STATE_CHANGE_SUCCESS, "could not set to playing");
	
	g_sink_base_chain = GST_PAD_CHAINFUNC (mysinkpad);
	gst_pad_set_chain_function (mysinkpad,
								GST_DEBUG_FUNCPTR (test_encode_sink_chain));
	
	strcpy(g_output_data_file_path, "data/aac_enc/propset11/aac_%03d.data");
	
	/* set src caps */
	srccaps = gst_caps_from_string (AUDIO_2CH_CAPS_STRING);
	gst_pad_set_caps (mysrcpad, srccaps);
	
	/* input buffers */
	input_buffers(PUSH_BUFFERS, "data/aac_enc/input02/pcm_%03d.data");
	
	/* cleanup */
	cleanup_acmaacenc (acmaacenc);
	g_list_free (buffers);
	buffers = NULL;
	gst_caps_unref (srccaps);
}
GST_END_TEST;

GST_START_TEST (test_encode_prop12)
{
	GstElement *acmaacenc;
	GstCaps *srccaps;
	
	/* encode test 12
	 * 2 channel input
	 * RAW format
	 */
	
	/* setup */
	acmaacenc = setup_acmaacenc (IN_CH_2, FMT_RAW);
	fail_unless (gst_element_set_state (acmaacenc, GST_STATE_PLAYING)
				 == GST_STATE_CHANGE_SUCCESS, "could not set to playing");
	
	g_sink_base_chain = GST_PAD_CHAINFUNC (mysinkpad);
	gst_pad_set_chain_function (mysinkpad,
								GST_DEBUG_FUNCPTR (test_encode_sink_chain));
	
	strcpy(g_output_data_file_path, "data/aac_enc/propset12/aac_%03d.data");
	
	/* set src caps */
	srccaps = gst_caps_from_string (AUDIO_2CH_CAPS_STRING);
	gst_pad_set_caps (mysrcpad, srccaps);
	
	/* input buffers */
	input_buffers(PUSH_BUFFERS, "data/aac_enc/input02/pcm_%03d.data");
	
	/* cleanup */
	cleanup_acmaacenc (acmaacenc);
	g_list_free (buffers);
	buffers = NULL;
	gst_caps_unref (srccaps);
}
GST_END_TEST;

#if SUPPORT_OUTPUT_FMT_ADIF
GST_START_TEST (test_encode_prop13)
{
	GstElement *acmaacenc;
	GstCaps *srccaps;
	
	/* encode test 13
	 * 2 channel input
	 * ADIF format
	 */
	
	/* setup */
	acmaacenc = setup_acmaacenc (IN_CH_2, FMT_ADIF);
	fail_unless (gst_element_set_state (acmaacenc, GST_STATE_PLAYING)
				 == GST_STATE_CHANGE_SUCCESS, "could not set to playing");
	
	g_sink_base_chain = GST_PAD_CHAINFUNC (mysinkpad);
	gst_pad_set_chain_function (mysinkpad,
								GST_DEBUG_FUNCPTR (test_encode_sink_chain));
	
	strcpy(g_output_data_file_path, "data/aac_enc/propset13/aac_%03d.data");
	
	/* set src caps */
	srccaps = gst_caps_from_string (AUDIO_2CH_CAPS_STRING);
	gst_pad_set_caps (mysrcpad, srccaps);
	
	/* input buffers */
	input_buffers(PUSH_BUFFERS, "data/aac_enc/input02/pcm_%03d.data");
	
	/* cleanup */
	cleanup_acmaacenc (acmaacenc);
	g_list_free (buffers);
	buffers = NULL;
	gst_caps_unref (srccaps);
}
GST_END_TEST;
#endif

static Suite *
acmaacenc_suite (void)
{
	Suite *s = suite_create ("acmaacenc");
	TCase *tc_chain = tcase_create ("general");

	tcase_set_timeout (tc_chain, 0);

	suite_add_tcase (s, tc_chain);

	tcase_add_test (tc_chain, test_properties);
	tcase_add_test (tc_chain, test_property_combination);

	tcase_add_test (tc_chain, test_check_caps);

	tcase_add_test (tc_chain, test_encode_prop01);
	tcase_add_test (tc_chain, test_encode_prop02);
#if SUPPORT_OUTPUT_FMT_ADIF
	tcase_add_test (tc_chain, test_encode_prop03);
#endif

	tcase_add_test (tc_chain, test_encode_prop11);
	tcase_add_test (tc_chain, test_encode_prop12);
#if SUPPORT_OUTPUT_FMT_ADIF
	tcase_add_test (tc_chain, test_encode_prop13);
#endif

	return s;
}

int
main (int argc, char **argv)
{
	int nf;
	
	Suite *s = acmaacenc_suite ();
	SRunner *sr = srunner_create (s);
	
	gst_check_init (&argc, &argv);
	
	srunner_run_all (sr, CK_NORMAL);
	nf = srunner_ntests_failed (sr);
	srunner_free (sr);
	
	return nf;
}

/*
 * End of file
 */
