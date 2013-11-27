/* GStreamer
 *
 * unit test for acmh264enc
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

#include "utest_util.h"

#define USE_STRIDE_PROP					0


/* For ease of programming we use globals to keep refs for our floating
 * src and sink pads we create; otherwise we always have to do get_pad,
 * get_peer, and then remove references in every test function */
static GstPad *mysrcpad, *mysinkpad;

#define VIDEO_CAPS_STRING "video/x-raw, " \
	"format = (string) NV12, " \
	"width = (int) 320, " \
	"height = (int) 240, " \
	"framerate = (fraction) 30/1"

/* MP4 */
#define AVC_CAPS_STRING "video/x-h264, " \
	"width = (int) [ 80, 1920 ], " \
	"height = (int) [ 80, 1080 ], " \
	"framerate = (fraction) 30/1, " \
	"stream-format = (string) avc, " \
	"alignment = (string) au"
/* TS */
#define BS_CAPS_STRING "video/x-h264, " \
	"width = (int) [ 80, 1920 ], " \
	"height = (int) [ 80, 1080 ], " \
	"framerate = (fraction) 30/1, " \
	"stream-format = (string) byte-stream "
/* for use offset */
#define STRIDE_CAPS_STRING "video/x-h264, " \
	"width = (int) 160, " \
	"height = (int) 120, " \
	"framerate = (fraction) 30/1, " \
	"stream-format = (string) byte-stream "

/* output */
static GstStaticPadTemplate sinktemplate_avc = GST_STATIC_PAD_TEMPLATE ("sink",
	GST_PAD_SINK,
	GST_PAD_ALWAYS,
	GST_STATIC_CAPS (AVC_CAPS_STRING));

static GstStaticPadTemplate sinktemplate_bs = GST_STATIC_PAD_TEMPLATE ("sink",
	GST_PAD_SINK,
	GST_PAD_ALWAYS,
	GST_STATIC_CAPS (BS_CAPS_STRING));

static GstStaticPadTemplate sinktemplate_stride = GST_STATIC_PAD_TEMPLATE ("sink",
	GST_PAD_SINK,
	GST_PAD_ALWAYS,
	GST_STATIC_CAPS (STRIDE_CAPS_STRING));

/* input */
static GstStaticPadTemplate srctemplate = GST_STATIC_PAD_TEMPLATE ("src",
	GST_PAD_SRC,
	GST_PAD_ALWAYS,
	GST_STATIC_CAPS (VIDEO_CAPS_STRING));

/* number of the buffers which push */
#define PUSH_BUFFERS	100

/* chain function of sink pad */
static GstPadChainFunction g_sink_base_chain = NULL;

/* output data file path */
static char g_output_data_file_path[PATH_MAX];


/* setup */
static GstElement *
setup_acmh264enc (GstStaticPadTemplate *sinktempl)
{
	GstElement *acmh264enc;
	
	g_print ("setup_acmh264enc\n");
	acmh264enc = gst_check_setup_element ("acmh264enc");
	g_print ("pass : gst_check_setup_element()\n");
	mysrcpad = gst_check_setup_src_pad (acmh264enc, &srctemplate);
	g_print ("pass : gst_check_setup_src_pad()\n");
	mysinkpad = gst_check_setup_sink_pad (acmh264enc, sinktempl);
	g_print ("pass : gst_check_setup_sink_pad()\n");

	gst_pad_set_active (mysrcpad, TRUE);
	gst_pad_set_active (mysinkpad, TRUE);
	
	return acmh264enc;
}

/* cleanup */
static void
cleanup_acmh264enc (GstElement * acmh264enc)
{
	g_print ("cleanup_acmh264enc\n");
	gst_element_set_state (acmh264enc, GST_STATE_NULL);
	g_print ("pass : gst_element_set_state()\n");

	gst_pad_set_active (mysrcpad, FALSE);
	g_print ("pass : gst_pad_set_active(mysrcpad)\n");
	gst_pad_set_active (mysinkpad, FALSE);
	g_print ("pass : gst_pad_set_active(mysinkpad)\n");
	gst_check_teardown_src_pad (acmh264enc);
	g_print ("pass : gst_check_teardown_src_pad()\n");
	gst_check_teardown_sink_pad (acmh264enc);
	g_print ("pass : gst_check_teardown_sink_pad()\n");
	gst_check_teardown_element (acmh264enc);
	g_print ("pass : gst_check_teardown_element()\n");
}

/* property set / get */
GST_START_TEST (test_properties)
{
	GstElement *acmh264enc;
	gchar *device;
#if USE_STRIDE_PROP
	gint stride;
#endif
	gint32 bit_rate;
	gint32 max_frame_size;
	gint rate_control_mode;
	gint max_GOP_length;
	gint B_pic_mode;
	gint x_offfset;
	gint y_offset;

	/* setup */
	acmh264enc = setup_acmh264enc (&sinktemplate_avc);
	
	/* set and check properties */
	g_object_set (acmh264enc,
				  "device", 				"/dev/video1",
#if USE_STRIDE_PROP
				  "stride", 				1024,
#endif
				  "bitrate", 				16000,
				  "max-frame-size",			2,
				  "rate-control-mode", 		0,
				  "max-gop-length",			0,
				  "b-pic-mode",				0,
				  "x-offset",				160,
				  "y-offset",				160,
				  NULL);
	g_object_get (acmh264enc,
				  "device", 				&device,
#if USE_STRIDE_PROP
				  "stride", 				&stride,
#endif
				  "bitrate", 				&bit_rate,
				  "max-frame-size", 		&max_frame_size,
				  "rate-control-mode", 		&rate_control_mode,
				  "max-gop-length", 		&max_GOP_length,
				  "b-pic-mode", 			&B_pic_mode,
				  "x-offset", 				&x_offfset,
				  "y-offset", 				&y_offset,
				  NULL);
	fail_unless (g_str_equal (device, "/dev/video1"));
#if USE_STRIDE_PROP
	fail_unless_equals_int (stride, 1024);
#endif
	fail_unless_equals_int (bit_rate, 16000);
	fail_unless_equals_int (max_frame_size, 2);
	fail_unless_equals_int (rate_control_mode, 0);
	fail_unless_equals_int (max_GOP_length, 0);
	fail_unless_equals_int (B_pic_mode, 0);
	fail_unless_equals_int (x_offfset, 160);
	fail_unless_equals_int (y_offset, 160);
	g_free (device);
	device = NULL;

	/* new properties */
	g_object_set (acmh264enc,
				  "device", 				"/dev/video2",
#if USE_STRIDE_PROP
				  "stride", 				1920,
#endif
				  "bitrate", 				40000000,
				  "max-frame-size",			5,
				  "rate-control-mode", 		2,
				  "max-gop-length",			120,
				  "b-pic-mode",				3,
				  "x-offset",				0,
				  "y-offset",				0,
				  NULL);
	g_object_get (acmh264enc,
				  "device", 				&device,
#if USE_STRIDE_PROP
				  "stride", 				&stride,
#endif
				  "bitrate", 				&bit_rate,
				  "max-frame-size", 		&max_frame_size,
				  "rate-control-mode", 		&rate_control_mode,
				  "max-gop-length", 		&max_GOP_length,
				  "b-pic-mode", 			&B_pic_mode,
				  "x-offset", 				&x_offfset,
				  "y-offset", 				&y_offset,
				  NULL);
	fail_unless (g_str_equal (device, "/dev/video2"));
#if USE_STRIDE_PROP
	fail_unless_equals_int (stride, 1920);
#endif
	fail_unless_equals_int (bit_rate, 40000000);
	fail_unless_equals_int (max_frame_size, 5);
	fail_unless_equals_int (rate_control_mode, 2);
	fail_unless_equals_int (max_GOP_length, 120);
	fail_unless_equals_int (B_pic_mode, 3);
	fail_unless_equals_int (x_offfset, 0);
	fail_unless_equals_int (y_offset, 0);
	g_free (device);
	device = NULL;

	/* cleanup	*/
	cleanup_acmh264enc (acmh264enc);
}
GST_END_TEST;

/* Combination of a property */
static void
check_property_comb_gop(gint max_GOP_length, gint B_pic_mode, GstFlowReturn result)
{
	GstElement *acmh264enc;
	GstBuffer *buffer;
	GstCaps *caps;
	GstBuffer *outbuffer;
	
	/* setup */
	acmh264enc = setup_acmh264enc (&sinktemplate_avc);
	g_object_set (acmh264enc,
				  "max-gop-length",			max_GOP_length,
				  "b-pic-mode",				B_pic_mode,
				  NULL);
	gst_element_set_state (acmh264enc, GST_STATE_PLAYING);
	
	/* make caps */
	caps = gst_caps_from_string (VIDEO_CAPS_STRING);	
	fail_unless (gst_pad_set_caps (mysrcpad, caps));

	/* create buffer */
	fail_unless ((buffer = create_video_buffer (caps)) != NULL);
	
	/* push buffer */
	fail_unless (gst_pad_push (mysrcpad, buffer) == result);

	/* release encoded data */
	if (g_list_length (buffers) > 0) {
		g_print ("num_buffers : %d\n", g_list_length (buffers));
		outbuffer = GST_BUFFER (buffers->data);
		buffers = g_list_remove (buffers, outbuffer);
		
		ASSERT_BUFFER_REFCOUNT (outbuffer, "outbuffer", 1);
		gst_buffer_unref (outbuffer);
		outbuffer = NULL;
	}
	
	/* cleanup */
	gst_caps_unref (caps);
	gst_element_set_state (acmh264enc, GST_STATE_NULL);
	cleanup_acmh264enc (acmh264enc);
}

GST_START_TEST (test_property_comb_gop)
{
	/* max_GOP_length:0, B_pic_mode:0 の場合を除いて、
	 * max_GOP_length は、B_pic_mode より大きくなくてはならない
	 */
	check_property_comb_gop(0, 0, GST_FLOW_OK);
	check_property_comb_gop(0, 1, GST_FLOW_NOT_NEGOTIATED);
	check_property_comb_gop(0, 2, GST_FLOW_NOT_NEGOTIATED);
	check_property_comb_gop(0, 3, GST_FLOW_NOT_NEGOTIATED);

	check_property_comb_gop(1, 0, GST_FLOW_OK);
	check_property_comb_gop(1, 1, GST_FLOW_NOT_NEGOTIATED);
	check_property_comb_gop(1, 2, GST_FLOW_NOT_NEGOTIATED);
	check_property_comb_gop(1, 3, GST_FLOW_NOT_NEGOTIATED);

	check_property_comb_gop(2, 0, GST_FLOW_OK);
	check_property_comb_gop(2, 1, GST_FLOW_OK);
	check_property_comb_gop(2, 2, GST_FLOW_NOT_NEGOTIATED);
	check_property_comb_gop(2, 3, GST_FLOW_NOT_NEGOTIATED);

	check_property_comb_gop(3, 0, GST_FLOW_OK);
	check_property_comb_gop(3, 1, GST_FLOW_OK);
	check_property_comb_gop(3, 2, GST_FLOW_OK);
	check_property_comb_gop(3, 3, GST_FLOW_NOT_NEGOTIATED);

	check_property_comb_gop(15, 0, GST_FLOW_OK);
	check_property_comb_gop(15, 1, GST_FLOW_OK);
	check_property_comb_gop(15, 2, GST_FLOW_OK);
	check_property_comb_gop(15, 3, GST_FLOW_OK);
}
GST_END_TEST;

static void
check_property_comb_input_size(gint width, gint height, GstFlowReturn result)
{
	GstElement *acmh264enc;
	GstBuffer *buffer;
	GstCaps *caps;
	GstBuffer *outbuffer;
	
	/* setup */
	acmh264enc = setup_acmh264enc (&sinktemplate_avc);
	g_object_set (acmh264enc,
				  "max-gop-length",			1,
				  "b-pic-mode",				0,
				  NULL);
	gst_element_set_state (acmh264enc, GST_STATE_PLAYING);
	
	/* make caps */
	caps = gst_caps_from_string (VIDEO_CAPS_STRING);
	gst_caps_set_simple (caps,
						 "width", G_TYPE_INT, width,
						 "height", G_TYPE_INT, height,
						 NULL);
	fail_unless (gst_pad_set_caps (mysrcpad, caps));
	
	/* create buffer */
	fail_unless ((buffer = create_video_buffer (caps)) != NULL);
	
	/* push buffer */
	fail_unless (gst_pad_push (mysrcpad, buffer) == result);
	
	/* release encoded data */
	if (g_list_length (buffers) > 0) {
		g_print ("num_buffers : %d\n", g_list_length (buffers));
		outbuffer = GST_BUFFER (buffers->data);
		buffers = g_list_remove (buffers, outbuffer);
		
		ASSERT_BUFFER_REFCOUNT (outbuffer, "outbuffer", 1);
		gst_buffer_unref (outbuffer);
		outbuffer = NULL;
	}
	
	/* cleanup */
	gst_caps_unref (caps);
	gst_element_set_state (acmh264enc, GST_STATE_NULL);
	cleanup_acmh264enc (acmh264enc);
}

GST_START_TEST (test_property_comb_input_size)
{
	/* 入力画像幅×入力画像高さは、32の倍数であること
	 * 入力画像幅と入力画像高さはそれぞれ2の倍数であること
	 */
	check_property_comb_input_size(320, 240, GST_FLOW_OK);
	check_property_comb_input_size(330, 100, GST_FLOW_NOT_NEGOTIATED);
	check_property_comb_input_size(321, 240, GST_FLOW_NOT_NEGOTIATED);
	check_property_comb_input_size(320, 241, GST_FLOW_NOT_NEGOTIATED);
}
GST_END_TEST;

static void
check_property_comb_offset(gint x_offset, gint y_offset,
	GstStaticPadTemplate *sinktempl, GstFlowReturn result)
{
	GstElement *acmh264enc;
	GstBuffer *buffer;
	GstCaps *caps;
	GstBuffer *outbuffer;

	/* setup */
	acmh264enc = setup_acmh264enc (sinktempl);
	g_object_set (acmh264enc,
				  "max-gop-length",			1,
				  "b-pic-mode",				0,
				  "x-offset",				x_offset,
				  "y-offset",				y_offset,
				  NULL);
	gst_element_set_state (acmh264enc, GST_STATE_PLAYING);

	/* make caps */
	caps = gst_caps_from_string (VIDEO_CAPS_STRING);
	fail_unless (gst_pad_set_caps (mysrcpad, caps));

	/* create buffer */
	fail_unless ((buffer = create_video_buffer (caps)) != NULL);

	/* push buffer */
	fail_unless (gst_pad_push (mysrcpad, buffer) == result);

	/* release encoded data */
	if (g_list_length (buffers) > 0) {
		g_print ("num_buffers : %d\n", g_list_length (buffers));
		outbuffer = GST_BUFFER (buffers->data);
		buffers = g_list_remove (buffers, outbuffer);
		
		ASSERT_BUFFER_REFCOUNT (outbuffer, "outbuffer", 1);
		gst_buffer_unref (outbuffer);
		outbuffer = NULL;
	}
	
	/* cleanup */
	gst_caps_unref (caps);
	gst_element_set_state (acmh264enc, GST_STATE_NULL);
	cleanup_acmh264enc (acmh264enc);
}

GST_START_TEST (test_property_comb_offset)
{
	/* 入力オフセットは32の倍数であること。
	 * src.width * y_offset / 2 + x_offsetも同様に32の倍数であること
	 */
	check_property_comb_offset(0, 0, &sinktemplate_avc, GST_FLOW_OK);
	check_property_comb_offset(160, 120, &sinktemplate_stride, GST_FLOW_OK);
	check_property_comb_offset(10, 10, &sinktemplate_stride, GST_FLOW_NOT_NEGOTIATED);
	check_property_comb_offset(15, 35, &sinktemplate_stride, GST_FLOW_NOT_NEGOTIATED);
}
GST_END_TEST;

/* check caps	*/
GST_START_TEST (test_check_caps)
{
	GstElement *acmh264enc;
	GstCaps *srccaps;
    GstCaps *outcaps;
	GstBuffer *inbuffer, *outbuffer;
	int i, num_buffers;

	/* setup */
	acmh264enc = setup_acmh264enc (&sinktemplate_avc);
	/* all I picture */
	g_object_set (acmh264enc,
				  "max-gop-length",			1,
				  "b-pic-mode",				0,
				  NULL);
	fail_unless (gst_element_set_state (acmh264enc, GST_STATE_PLAYING)
				 == GST_STATE_CHANGE_SUCCESS, "could not set to playing");

	/* set src caps */
	srccaps = gst_caps_from_string (VIDEO_CAPS_STRING);
	gst_pad_set_caps (mysrcpad, srccaps);

	/* corresponds to I420 buffer for the size mentioned in the caps */
	/* YUV : 320 width x 240 height */
	inbuffer = gst_buffer_new_and_alloc (320 * 240 * 3 / 2);
	/* makes valgrind's memcheck happier */
	gst_buffer_memset (inbuffer, 0, 0, -1);
	GST_BUFFER_TIMESTAMP (inbuffer) = 0;
	ASSERT_BUFFER_REFCOUNT (inbuffer, "inbuffer", 1);
	fail_unless (gst_pad_push (mysrcpad, inbuffer) == GST_FLOW_OK);
	
	/* send eos to have all flushed if needed */
	fail_unless (gst_pad_push_event (mysrcpad, gst_event_new_eos ()) == TRUE);

	num_buffers = g_list_length (buffers);
	g_print ("num_buffers : %d\n", num_buffers);
	fail_unless (num_buffers == 1);

	/* check the out caps */
	outcaps = gst_pad_get_current_caps (mysinkpad);
	g_print ("outcaps : %p\n", outcaps);
	{
		GstStructure *s;
		const GValue *sf, *cd;
		const gchar *stream_format;
		
		fail_unless (outcaps != NULL);
		
		GST_INFO ("outcaps %" GST_PTR_FORMAT "\n", outcaps);
		s = gst_caps_get_structure (outcaps, 0);
		fail_unless (s != NULL);
		fail_if (!gst_structure_has_name (s, "video/x-h264"));
		sf = gst_structure_get_value (s, "stream-format");
		fail_unless (sf != NULL);
		fail_unless (G_VALUE_HOLDS_STRING (sf));
		stream_format = g_value_get_string (sf);
		fail_unless (stream_format != NULL);
		if (strcmp (stream_format, "avc") == 0) {
			GstMapInfo map;
			GstBuffer *buf;
			
			cd = gst_structure_get_value (s, "codec_data");
			fail_unless (cd != NULL);
			fail_unless (GST_VALUE_HOLDS_BUFFER (cd));
			buf = gst_value_get_buffer (cd);
			fail_unless (buf != NULL);
			gst_buffer_map (buf, &map, GST_MAP_READ);
			fail_unless_equals_int (map.data[0], 1);
			/* RMA001_マルチメディアミドル_機能仕様書 : 映像エンコード仕様
			 * 画像サイズや、Bピクチャの有無によってプロファイルが自動的に変わる
			 */
			fail_unless (map.data[1] == 0x42);

			gst_buffer_unmap (buf, &map);
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
	cleanup_acmh264enc (acmh264enc);
	gst_caps_unref (srccaps);
	gst_caps_unref (outcaps);
	if (buffers) {
		g_list_free (buffers);
		buffers = NULL;
	}
}
GST_END_TEST;

/* H264 encode	*/
static gint g_nOutputBuffers = 0;
static GstFlowReturn
test_encode_sink_chain(GstPad * pad, GstObject * parent, GstBuffer * buf)
{
	size_t size;
	void *p;
	int fd;
	char file[PATH_MAX];
	GstBuffer *outbuffer;
	GstMapInfo map;
	GstFlowReturn ret;

	ret = g_sink_base_chain(pad, parent, buf);
	
	/* check outputed buffer */
	if (g_list_length (buffers) > 0) {
		++g_nOutputBuffers;
		
		outbuffer = GST_BUFFER (buffers->data);
		fail_if (outbuffer == NULL);
		fail_unless (GST_IS_BUFFER (outbuffer));

		sprintf(file, g_output_data_file_path, g_nOutputBuffers);
		g_print("%s\n", file);

		get_data(file, &size, &p, &fd);

//		g_print("%d - %d\n", gst_buffer_get_size (outbuffer), size);
		fail_unless (gst_buffer_get_size (outbuffer) == size);
		gst_buffer_map (outbuffer, &map, GST_MAP_READ);
		fail_unless (0 == memcmp(p, map.data, size));
		gst_buffer_unmap (outbuffer, &map);

		fail_unless (0 == munmap(p, size));
		close(fd);

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
	int fd;
	char file[PATH_MAX];
	GstBuffer *inbuffer;
	gint nInputBuffers = 0;
	
	while (TRUE) {
		if (++nInputBuffers < num_bufs) {
			sprintf(file, data_path, nInputBuffers);
			g_print("%s\n", file);

			get_data(file, &size, &p, &fd);

			inbuffer = gst_buffer_new_and_alloc (size);
			gst_buffer_fill (inbuffer, 0, p, size);

			fail_unless (0 == munmap(p, size));
			close(fd);

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

static void
check_encode_avc(gint B_pic_mode, gint max_GOP_length)
{
	GstElement *acmh264enc;
	GstCaps *srccaps;

	/* setup */
	acmh264enc = setup_acmh264enc (&sinktemplate_avc);
	g_object_set (acmh264enc,
				  "b-pic-mode",				B_pic_mode,
				  "max-gop-length",			max_GOP_length,
				  NULL);
	fail_unless (gst_element_set_state (acmh264enc, GST_STATE_PLAYING)
				 == GST_STATE_CHANGE_SUCCESS, "could not set to playing");
	
	g_sink_base_chain = GST_PAD_CHAINFUNC (mysinkpad);
	gst_pad_set_chain_function (mysinkpad,
								GST_DEBUG_FUNCPTR (test_encode_sink_chain));
	g_nOutputBuffers = 0;
	
	sprintf(g_output_data_file_path, "data/h264_enc/avc_propset%02d%02d/",
			B_pic_mode, max_GOP_length);
	strcat(g_output_data_file_path, "h264_%03d.data");
	
	/* set src caps */
	srccaps = gst_caps_from_string (VIDEO_CAPS_STRING);
	gst_pad_set_caps (mysrcpad, srccaps);
	
	/* input buffers */
	input_buffers(PUSH_BUFFERS, "data/h264_enc/input01/yuv_%03d.data");
	
	/* cleanup */
	cleanup_acmh264enc (acmh264enc);
	g_list_free (buffers);
	buffers = NULL;
	gst_caps_unref (srccaps);
}

static void
check_encode_bs(gint B_pic_mode, gint max_GOP_length)
{
	GstElement *acmh264enc;
	GstCaps *srccaps;
	
	/* setup */
	acmh264enc = setup_acmh264enc (&sinktemplate_bs);
	g_object_set (acmh264enc,
				  "b-pic-mode",				B_pic_mode,
				  "max-gop-length",			max_GOP_length,
				  NULL);
	fail_unless (gst_element_set_state (acmh264enc, GST_STATE_PLAYING)
				 == GST_STATE_CHANGE_SUCCESS, "could not set to playing");
	
	g_sink_base_chain = GST_PAD_CHAINFUNC (mysinkpad);
	gst_pad_set_chain_function (mysinkpad,
								GST_DEBUG_FUNCPTR (test_encode_sink_chain));
	g_nOutputBuffers = 0;

	sprintf(g_output_data_file_path, "data/h264_enc/bs_propset%02d%02d/",
			B_pic_mode, max_GOP_length);
	strcat(g_output_data_file_path, "h264_%03d.data");
	
	/* set src caps */
	srccaps = gst_caps_from_string (VIDEO_CAPS_STRING);
	gst_pad_set_caps (mysrcpad, srccaps);
	
	/* input buffers */
	input_buffers(PUSH_BUFFERS, "data/h264_enc/input01/yuv_%03d.data");
	
	/* cleanup */
	cleanup_acmh264enc (acmh264enc);
	g_list_free (buffers);
	buffers = NULL;
	gst_caps_unref (srccaps);
}

GST_START_TEST (test_encode_avc0000)
{
	check_encode_avc(0, 0);
}
GST_END_TEST;

GST_START_TEST (test_encode_avc0001)
{
	check_encode_avc(0, 1);
}
GST_END_TEST;

GST_START_TEST (test_encode_avc0002)
{
	check_encode_avc(0, 2);
}
GST_END_TEST;

GST_START_TEST (test_encode_avc0003)
{
	check_encode_avc(0, 3);
}
GST_END_TEST;

GST_START_TEST (test_encode_avc0015)
{
	check_encode_avc(0, 15);
}
GST_END_TEST;

GST_START_TEST (test_encode_avc0102)
{
	check_encode_avc(1, 2);
}
GST_END_TEST;

GST_START_TEST (test_encode_avc0103)
{
	check_encode_avc(1, 3);
}
GST_END_TEST;

GST_START_TEST (test_encode_avc0115)
{
	check_encode_avc(1, 15);
}
GST_END_TEST;

GST_START_TEST (test_encode_avc0203)
{
	check_encode_avc(2, 3);
}
GST_END_TEST;

GST_START_TEST (test_encode_avc0215)
{
	check_encode_avc(2, 15);
}
GST_END_TEST;

GST_START_TEST (test_encode_avc0315)
{
	check_encode_avc(3, 15);
}
GST_END_TEST;

GST_START_TEST (test_encode_bs0000)
{
	check_encode_bs(0, 0);
}
GST_END_TEST;

GST_START_TEST (test_encode_bs0001)
{
	check_encode_bs(0, 1);
}
GST_END_TEST;

GST_START_TEST (test_encode_bs0002)
{
	check_encode_bs(0, 2);
}
GST_END_TEST;

GST_START_TEST (test_encode_bs0003)
{
	check_encode_bs(0, 3);
}
GST_END_TEST;

GST_START_TEST (test_encode_bs0015)
{
	check_encode_bs(0, 15);
}
GST_END_TEST;

GST_START_TEST (test_encode_bs0102)
{
	check_encode_bs(1, 2);
}
GST_END_TEST;

GST_START_TEST (test_encode_bs0103)
{
	check_encode_bs(1, 3);
}
GST_END_TEST;

GST_START_TEST (test_encode_bs0115)
{
	check_encode_bs(1, 15);
}
GST_END_TEST;

GST_START_TEST (test_encode_bs0203)
{
	check_encode_bs(2, 3);
}
GST_END_TEST;

GST_START_TEST (test_encode_bs0215)
{
	check_encode_bs(2, 15);
}
GST_END_TEST;

GST_START_TEST (test_encode_bs0315)
{
	check_encode_bs(3, 15);
}
GST_END_TEST;

static Suite *
acmh264enc_suite (void)
{
	Suite *s = suite_create ("acmh264enc");
	TCase *tc_chain = tcase_create ("general");

	tcase_set_timeout (tc_chain, 0);

	suite_add_tcase (s, tc_chain);

	tcase_add_test (tc_chain, test_properties);
	tcase_add_test (tc_chain, test_property_comb_gop);
	tcase_add_test (tc_chain, test_property_comb_input_size);
	tcase_add_test (tc_chain, test_property_comb_offset);

	tcase_add_test (tc_chain, test_check_caps);

	/* avc */
	/* B_pic_mode:0	*/
	tcase_add_test (tc_chain, test_encode_avc0000);
	tcase_add_test (tc_chain, test_encode_avc0001);
	tcase_add_test (tc_chain, test_encode_avc0002);
	tcase_add_test (tc_chain, test_encode_avc0003);
	tcase_add_test (tc_chain, test_encode_avc0015);
	/* B_pic_mode:1	*/
	tcase_add_test (tc_chain, test_encode_avc0102);
	tcase_add_test (tc_chain, test_encode_avc0103);
	tcase_add_test (tc_chain, test_encode_avc0115);
	/* B_pic_mode:2	*/
	tcase_add_test (tc_chain, test_encode_avc0203);
	tcase_add_test (tc_chain, test_encode_avc0215);
	/* B_pic_mode:3	*/
	tcase_add_test (tc_chain, test_encode_avc0315);

	/* byte-stream */
	/* B_pic_mode:0	*/
	tcase_add_test (tc_chain, test_encode_bs0000);
	tcase_add_test (tc_chain, test_encode_bs0001);
	tcase_add_test (tc_chain, test_encode_bs0002);
	tcase_add_test (tc_chain, test_encode_bs0003);
	tcase_add_test (tc_chain, test_encode_bs0015);
	/* B_pic_mode:1	*/
	tcase_add_test (tc_chain, test_encode_bs0102);
	tcase_add_test (tc_chain, test_encode_bs0103);
	tcase_add_test (tc_chain, test_encode_bs0115);
	/* B_pic_mode:2	*/
	tcase_add_test (tc_chain, test_encode_bs0203);
	tcase_add_test (tc_chain, test_encode_bs0215);
	/* B_pic_mode:3	*/
	tcase_add_test (tc_chain, test_encode_bs0315);
	
	return s;
}

int
main (int argc, char **argv)
{
	int nf;
	
	Suite *s = acmh264enc_suite ();
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
