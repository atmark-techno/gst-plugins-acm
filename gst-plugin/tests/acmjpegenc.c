/* GStreamer
 *
 * unit test for acmjpegenc
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

#define SUPPORT_NV16					0

/* input format		*/
enum {
	FMT_NV12,
#if SUPPORT_NV16
	FMT_NV16,
#endif
};

/* For ease of programming we use globals to keep refs for our floating
 * src and sink pads we create; otherwise we always have to do get_pad,
 * get_peer, and then remove references in every test function */
static GstPad *mysrcpad, *mysinkpad;

#define YUV420_CAPS_STRING "video/x-raw, " \
	"format = (string) NV12, " \
	"width = (int) 320, " \
	"height = (int) 240, " \
	"framerate = (fraction) 30/1 "

#if SUPPORT_NV16
#define YUV422_CAPS_STRING "video/x-raw, " \
	"format = (string) NV16, " \
	"width = (int) 320, " \
	"height = (int) 240, " \
	"framerate = (fraction) 30/1 "
#endif

#define JPEG_CAPS_STRING "image/jpeg, " \
	"width = (int) [ 16, 1920 ], " \
	"height = (int) [ 16, 1080 ], " \
	"framerate = (fraction) [ 0/1, MAX ]"
/* for use offset */
#define JPEG_STRIDE_CAPS_STRING "image/jpeg, " \
	"width = (int) 160, " \
	"height = (int) 160, " \
	"framerate = (fraction) [ 0/1, MAX ] "

/* output */
static GstStaticPadTemplate sinktemplate = GST_STATIC_PAD_TEMPLATE ("sink",
	GST_PAD_SINK,
	GST_PAD_ALWAYS,
	GST_STATIC_CAPS (JPEG_CAPS_STRING));
static GstStaticPadTemplate sinktemplate_stride = GST_STATIC_PAD_TEMPLATE ("sink",
	GST_PAD_SINK,
	GST_PAD_ALWAYS,
	GST_STATIC_CAPS (JPEG_STRIDE_CAPS_STRING));

/* input */
static GstStaticPadTemplate srctemplate = GST_STATIC_PAD_TEMPLATE ("src",
	GST_PAD_SRC,
	GST_PAD_ALWAYS,
	GST_STATIC_CAPS (YUV420_CAPS_STRING));

/* number of the buffers which push */
#define PUSH_BUFFERS	5

/* chain function of sink pad */
static GstPadChainFunction g_sink_base_chain = NULL;

/* output data file path */
static char g_output_data_file_path[PATH_MAX];


/* setup */
static GstElement *
setup_acmjpegenc (GstStaticPadTemplate *sinktempl)
{
	GstElement *acmjpegenc;

	g_print ("setup_acmjpegenc\n");
	acmjpegenc = gst_check_setup_element ("acmjpegenc");
	g_print ("pass : gst_check_setup_element()\n");

	mysrcpad = gst_check_setup_src_pad (acmjpegenc, &srctemplate);
	g_print ("pass : gst_check_setup_src_pad()\n");
	
	mysinkpad = gst_check_setup_sink_pad (acmjpegenc, sinktempl);
	g_print ("pass : gst_check_setup_sink_pad()\n");
	
	gst_pad_set_active (mysrcpad, TRUE);
	gst_pad_set_active (mysinkpad, TRUE);

	return acmjpegenc;
}

/* cleanup */
static void
cleanup_acmjpegenc (GstElement * acmjpegenc)
{
	GST_DEBUG ("cleanup_acmjpegenc");
	gst_element_set_state (acmjpegenc, GST_STATE_NULL);

	gst_pad_set_active (mysrcpad, FALSE);
	gst_pad_set_active (mysinkpad, FALSE);
	gst_check_teardown_src_pad (acmjpegenc);
	gst_check_teardown_sink_pad (acmjpegenc);
	gst_check_teardown_element (acmjpegenc);
}

/* property set / get */
GST_START_TEST (test_properties)
{
	GstElement *acmjpegenc;
	gchar 	*device = NULL;
	gint 	quality;
	gint 	x_offset;
	gint 	y_offset;

	/* setup */
	acmjpegenc = setup_acmjpegenc (&sinktemplate);

	/* set and check properties */
	g_object_set (G_OBJECT (acmjpegenc),
				  "device", 		"/dev/video7",
				  "quality", 		50,
				  "x-offset",		160,
				  "y-offset",		160,
				  NULL);
	g_object_get (acmjpegenc,
				  "device", 		&device,
				  "quality", 		&quality,
				  "x-offset",		&x_offset,
				  "y-offset",		&y_offset,
				  NULL);
	fail_unless (g_str_equal (device, "/dev/video7"));
	fail_unless_equals_int (quality, 50);
	fail_unless_equals_int (x_offset, 160);
	fail_unless_equals_int (y_offset, 160);
	g_free (device);
	device = NULL;

	/* new properties */
	g_object_set (G_OBJECT (acmjpegenc),
				  "device", 		"/dev/video9",
				  "quality", 		95,
				  "x-offset",		0,
				  "y-offset",		0,
				  NULL);
	g_object_get (acmjpegenc,
				  "device", 		&device,
				  "quality", 		&quality,
				  "x-offset",		&x_offset,
				  "y-offset",		&y_offset,
				  NULL);
	fail_unless (g_str_equal (device, "/dev/video9"));
	fail_unless_equals_int (quality, 95);
	fail_unless_equals_int (x_offset, 0);
	fail_unless_equals_int (y_offset, 0);
	g_free (device);
	device = NULL;

	/* cleanup	*/
	cleanup_acmjpegenc (acmjpegenc);
}
GST_END_TEST;

/* input image's width and height	*/
static void
check_input_img_size(gint width, gint height, gint fmt, GstFlowReturn result)
{
	GstElement *acmjpegenc;
	GstBuffer *buffer;
	GstCaps *caps;
	GstBuffer *outbuffer;

	/* setup */
	acmjpegenc = setup_acmjpegenc (&sinktemplate);
	gst_element_set_state (acmjpegenc, GST_STATE_PLAYING);

	/* make caps */
	caps = gst_caps_new_simple ("video/x-raw",
								"width", G_TYPE_INT, width,
								"height", G_TYPE_INT, height,
								"framerate", GST_TYPE_FRACTION, 1, 1,
								NULL);
	if (FMT_NV12 == fmt) {
		gst_caps_set_simple (caps, "format", G_TYPE_STRING, "NV12", NULL);
	}
#if SUPPORT_NV16
	else if (FMT_NV16 == fmt) {
		gst_caps_set_simple (caps, "format", G_TYPE_STRING, "NV16", NULL);
	}
#endif

	fail_unless (gst_pad_set_caps (mysrcpad, caps));

	/* create buffer */
	fail_unless ((buffer = create_video_buffer (caps)) != NULL);

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
	gst_element_set_state (acmjpegenc, GST_STATE_NULL);
	cleanup_acmjpegenc (acmjpegenc);
}

GST_START_TEST (test_input_img_size)
{
	/* NV12 - width : 8pixel の倍数, height : 16pixel の倍数 */
	check_input_img_size(640, 320, FMT_NV12, GST_FLOW_OK);
	check_input_img_size(645, 320, FMT_NV12, GST_FLOW_NOT_NEGOTIATED);
	check_input_img_size(640, 325, FMT_NV12, GST_FLOW_NOT_NEGOTIATED);
	check_input_img_size(645, 325, FMT_NV12, GST_FLOW_NOT_NEGOTIATED);

#if SUPPORT_NV16	/* 結局 NV16 はエラーとなってしまうため、使用不可	*/
	/* NV16 - width : 8pixel の倍数, height : 8pixel の倍数  */
	check_input_img_size(800, 600, FMT_NV16, GST_FLOW_OK);
	check_input_img_size(805, 600, FMT_NV16, GST_FLOW_NOT_NEGOTIATED);
	check_input_img_size(800, 605, FMT_NV16, GST_FLOW_NOT_NEGOTIATED);
	check_input_img_size(805, 605, FMT_NV16, GST_FLOW_NOT_NEGOTIATED);
#endif
}
GST_END_TEST;

/* combination of a property */
static void
check_property_comb_offset(gint width, gint height, gint fmt,
	gint x_offset, gint y_offset, GstFlowReturn result)
{
	GstElement *acmjpegenc;
	GstBuffer *buffer;
	GstCaps *caps;
	GstBuffer *outbuffer;

//	g_print("check_property_comb_offset(%d, %d, %d, %d)\n",
//			width, height, x_offset, y_offset);

	/* setup */
	acmjpegenc = setup_acmjpegenc (&sinktemplate_stride);
	g_object_set (acmjpegenc,
				  "x-offset",				x_offset,
				  "y-offset",				y_offset,
				  NULL);
	gst_element_set_state (acmjpegenc, GST_STATE_PLAYING);

	/* make caps */
	caps = gst_caps_new_simple ("video/x-raw",
								"width", G_TYPE_INT, width,
								"height", G_TYPE_INT, height,
								"framerate", GST_TYPE_FRACTION, 1, 1,
								NULL);
	if (FMT_NV12 == fmt) {
		gst_caps_set_simple (caps, "format", G_TYPE_STRING, "NV12", NULL);
	}
#if SUPPORT_NV16
	else if (FMT_NV16 == fmt) {
		gst_caps_set_simple (caps, "format", G_TYPE_STRING, "NV16", NULL);
	}
#endif

	fail_unless (gst_pad_set_caps (mysrcpad, caps));

	/* create buffer */
	fail_unless ((buffer = create_video_buffer (caps)) != NULL);

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
	gst_element_set_state (acmjpegenc, GST_STATE_NULL);
	cleanup_acmjpegenc (acmjpegenc);
}

GST_START_TEST (test_property_comb_offset)
{
	/* NV12 - width - x_offset : 8pixel の倍数, 
	 *        height - y_offset : 16pixel の倍数 
	 */
	check_property_comb_offset(640, 320, FMT_NV12, 320, 160,
							   GST_FLOW_OK);
	check_property_comb_offset(640, 320, FMT_NV12, 5, 160,
							   GST_FLOW_NOT_NEGOTIATED);
	check_property_comb_offset(640, 320, FMT_NV12, 320, 5,
							   GST_FLOW_NOT_NEGOTIATED);
	check_property_comb_offset(640, 320, FMT_NV12, 5, 5,
							   GST_FLOW_NOT_NEGOTIATED);
	check_property_comb_offset(640, 320, FMT_NV12, 641, 321,
							   GST_FLOW_OK);

#if SUPPORT_NV16	/* 結局 NV16 はエラーとなってしまうため、使用不可	*/
	/* NV16 - width - x_offset : 8pixel の倍数, 
	 *        height - y_offset : 8pixel の倍数  
	 */
	check_property_comb_offset(800, 600, FMT_NV16, 400, 280,
							   GST_FLOW_OK);
	check_property_comb_offset(800, 600, FMT_NV16, 5, 280,
							   GST_FLOW_NOT_NEGOTIATED);
	check_property_comb_offset(800, 600, FMT_NV16, 400, 5,
							   GST_FLOW_NOT_NEGOTIATED);
	check_property_comb_offset(800, 600, FMT_NV16, 5, 5,
							   GST_FLOW_NOT_NEGOTIATED);
	check_property_comb_offset(800, 600, FMT_NV16, 801, 601,
							   GST_FLOW_OK);
#endif
}
GST_END_TEST;

/* check caps	*/
GST_START_TEST (test_check_caps)
{
	GstElement *acmjpegenc;
	GstCaps *srccaps;
    GstCaps *outcaps;
	GstBuffer *inbuffer, *outbuffer;
	int i, num_buffers;
	
	/* setup */
	acmjpegenc = setup_acmjpegenc (&sinktemplate);
	fail_unless (gst_element_set_state (acmjpegenc, GST_STATE_PLAYING)
				 == GST_STATE_CHANGE_SUCCESS, "could not set to playing");

	/* set src caps */
	srccaps = gst_caps_from_string (YUV420_CAPS_STRING);
	gst_pad_set_caps (mysrcpad, srccaps);

	/* corresponds to I420 buffer for the size mentioned in the caps */
	/* YUV420 : 320 width x 240 height */
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
		const GValue *value;
		gint fps_n;
		gint fps_d;

		fail_unless (outcaps != NULL);

		GST_INFO ("outcaps %" GST_PTR_FORMAT "\n", outcaps);
		s = gst_caps_get_structure (outcaps, 0);
		fail_unless (s != NULL);
		fail_if (!gst_structure_has_name (s, "image/jpeg"));

		value = gst_structure_get_value (s, "width");
		fail_unless (value != NULL);
		fail_unless (g_value_get_int (value) == 320);

		value = gst_structure_get_value (s, "height");
		fail_unless (value != NULL);
		fail_unless (g_value_get_int (value) == 240);

		fail_unless (gst_structure_get_fraction (s, "framerate", &fps_n, &fps_d));
		fail_unless (fps_n == 30);
		fail_unless (fps_d == 1);
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
	cleanup_acmjpegenc (acmjpegenc);
	gst_caps_unref (srccaps);
    gst_caps_unref (outcaps);
	if (buffers) {
		g_list_free (buffers);
		buffers = NULL;
	}
}
GST_END_TEST;

/* JPEG encode	*/
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
check_encode(gint srcfmt, gint quality)
{
	GstElement *acmjpegenc;
	GstCaps *srccaps;

	/* setup */
	acmjpegenc = setup_acmjpegenc (&sinktemplate);
	g_object_set (G_OBJECT (acmjpegenc),
				  "quality", quality,
				  NULL);
	fail_unless (gst_element_set_state (acmjpegenc, GST_STATE_PLAYING)
				 == GST_STATE_CHANGE_SUCCESS, "could not set to playing");

	g_sink_base_chain = GST_PAD_CHAINFUNC (mysinkpad);
	gst_pad_set_chain_function (mysinkpad,
								GST_DEBUG_FUNCPTR (test_encode_sink_chain));
	g_nOutputBuffers = 0;

	sprintf(g_output_data_file_path, "data/jpeg_enc/propset%02d/", quality);
	strcat(g_output_data_file_path, "jpeg_%03d.data");

	/* set src caps */
	if (FMT_NV12 == srcfmt) {
		srccaps = gst_caps_from_string (YUV420_CAPS_STRING);
	}
#if SUPPORT_NV16
	else if (FMT_NV16 == srcfmt) {
		srccaps = gst_caps_from_string (YUV422_CAPS_STRING);
	}
#endif
	else {
		g_assert_not_reached();
	}
	gst_pad_set_caps (mysrcpad, srccaps);

	/* input buffers */
	if (FMT_NV12 == srcfmt) {
		input_buffers(PUSH_BUFFERS, "data/jpeg_enc/input01/yuv420_%03d.data");
	}
#if SUPPORT_NV16
	else if (FMT_NV16 == srcfmt) {
		input_buffers(PUSH_BUFFERS, "data/jpeg_enc/input02/yuv422_%03d.data");
	}
#endif
	else {
		g_assert_not_reached();
	}

	/* cleanup */
	cleanup_acmjpegenc (acmjpegenc);
	g_list_free (buffers);
	buffers = NULL;
	gst_caps_unref (srccaps);
}

GST_START_TEST (test_encode_yuv420_30)
{
	check_encode(FMT_NV12, 30);
}
GST_END_TEST;

GST_START_TEST (test_encode_yuv420_60)
{
	check_encode(FMT_NV12, 60);
}
GST_END_TEST;

GST_START_TEST (test_encode_yuv420_90)
{
	check_encode(FMT_NV12, 90);
}
GST_END_TEST;

#if SUPPORT_NV16
GST_START_TEST (test_encode_yuv422_30)
{
	check_encode(FMT_NV16, 30);
}
GST_END_TEST;

GST_START_TEST (test_encode_yuv422_60)
{
	check_encode(FMT_NV16, 60);
}
GST_END_TEST;

GST_START_TEST (test_encode_yuv422_90)
{
	check_encode(FMT_NV16, 90);
}
GST_END_TEST;
#endif

static Suite *
acmjpegenc_suite (void)
{
	Suite *s = suite_create ("acmjpegenc");
	TCase *tc_chain = tcase_create ("general");

	tcase_set_timeout (tc_chain, 0);

	suite_add_tcase (s, tc_chain);

	tcase_add_test (tc_chain, test_properties);

	tcase_add_test (tc_chain, test_input_img_size);
	tcase_add_test (tc_chain, test_property_comb_offset);

	tcase_add_test (tc_chain, test_check_caps);

	tcase_add_test (tc_chain, test_encode_yuv420_30);
	tcase_add_test (tc_chain, test_encode_yuv420_60);
	tcase_add_test (tc_chain, test_encode_yuv420_90);

#if SUPPORT_NV16
	tcase_add_test (tc_chain, test_encode_yuv422_30);
	tcase_add_test (tc_chain, test_encode_yuv422_60);
	tcase_add_test (tc_chain, test_encode_yuv422_90);
#endif

	return s;
}

int
main (int argc, char **argv)
{
	int nf;
	
	Suite *s = acmjpegenc_suite ();
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
