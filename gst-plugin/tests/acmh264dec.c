/* GStreamer
 *
 * unit test for acmh264dec
 *
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


/* v4l2 mem2mem デバイスファイル	*/
#define M2M_DEVICE	"/dev/video2"

#define DEFAULT_FMEM_NUM				17

/* 入力データの種類	*/
enum {
	AVC_AU,
	BS_NAL,
};

/* For ease of programming we use globals to keep refs for our floating
 * src and sink pads we create; otherwise we always have to do get_pad,
 * get_peer, and then remove references in every test function */
static GstPad *mysrcpad, *mysinkpad;

#define VIDEO_CAPS_STRING "video/x-raw, " \
	"format = (string) RGB, " \
	"width = (int)[16, 4096], " \
	"height = (int)[16, 4096], " \
	"framerate = (fraction) [ 0/1, 2147483647/1 ]; "

#define AVC_CAPS_STRING "video/x-h264, " \
	"stream-format = (string) avc, alignment = (string) au, " \
	"width  = (int)320, " \
	"height  = (int)240, " \
	"framerate = (fraction)30/1"


static GstStaticPadTemplate sinktemplate = GST_STATIC_PAD_TEMPLATE ("sink",
	GST_PAD_SINK,
	GST_PAD_ALWAYS,
	GST_STATIC_CAPS (VIDEO_CAPS_STRING));

static GstStaticPadTemplate srctemplate = GST_STATIC_PAD_TEMPLATE ("src",
	GST_PAD_SRC,
	GST_PAD_ALWAYS,
	GST_STATIC_CAPS (AVC_CAPS_STRING));



/* push するバッファ数 */
#define PUSH_BUFFERS	100

GstPadChainFunction g_base_chain = NULL;

static void
get_data(char *file, size_t *size, void **p)
{
	struct stat sb;
	int pcm;
	void *ref;
	int ret;

	pcm = open(file, O_RDONLY);
	fail_if(pcm < 0, "open: '%s': %s", file, strerror(errno));
	ret = fstat(pcm, &sb);
	fail_if(ret < 0, "fstat: '%s': %s", strerror(errno));
	*size = sb.st_size;
	
	ref = mmap(NULL, sb.st_size, PROT_READ, MAP_PRIVATE, pcm, 0);
	fail_if(ref == MAP_FAILED, "mmap: %s", strerror(errno));
	*p = ref;
}

static GstElement *
setup_acmh264dec ()
{
	GstElement *acmh264dec;
	
	g_print ("setup_acmh264dec\n");
	acmh264dec = gst_check_setup_element ("acmh264dec");
	g_object_set (acmh264dec,
				  "device", M2M_DEVICE,
				  NULL);
	g_print ("pass : gst_check_setup_element()\n");
	mysrcpad = gst_check_setup_src_pad (acmh264dec, &srctemplate);
	g_print ("pass : gst_check_setup_src_pad()\n");
	mysinkpad = gst_check_setup_sink_pad (acmh264dec, &sinktemplate);
	g_print ("pass : gst_check_setup_sink_pad()\n");

	gst_pad_set_active (mysrcpad, TRUE);
	gst_pad_set_active (mysinkpad, TRUE);
	
	return acmh264dec;
}

static void
cleanup_acmh264dec (GstElement * acmh264dec)
{
	g_print ("cleanup_acmh264dec\n");
	gst_element_set_state (acmh264dec, GST_STATE_NULL);
	
	gst_pad_set_active (mysrcpad, FALSE);
	gst_pad_set_active (mysinkpad, FALSE);
	gst_check_teardown_src_pad (acmh264dec);
	gst_check_teardown_sink_pad (acmh264dec);
	gst_check_teardown_element (acmh264dec);
}

GST_START_TEST (test_properties)
{
	GstElement *acmh264dec;
	gchar *device;
	gint	width;
	gint 	height;
	gchar 	*format;
	gint 	fmem_num;
	gint 	buf_pic_cnt;
	gboolean enable_vio6;
	gint 	stride;
	gint 	x_offset;
	gint 	y_offset;

	acmh264dec = setup_acmh264dec (AVC_AU);
	
	g_object_set (acmh264dec,
				  "device", 		"/dev/video1",
				  "width", 			1024,
				  "height", 		768,
				  "fmem-num", 		2,
				  "buf-pic-cnt", 	5, 
				  "format", 		"RGB",
				  "enable-vio6", 	TRUE,
				  "stride",			2048,
				  "x-offset",		20,
				  "y-offset",		30,
				  NULL);
	g_object_get (acmh264dec,
				  "device", 		&device,
				  "width", 			&width,
				  "height", 		&height,
				  "fmem-num", 		&fmem_num,
				  "buf-pic-cnt", 	&buf_pic_cnt,
				  "format", 		&format,
				  "enable-vio6", 	&enable_vio6,
				  "stride",			&stride,
				  "x-offset",		&x_offset,
				  "y-offset",		&y_offset,
				  NULL);
	fail_unless (g_str_equal (device, "/dev/video1"));
	fail_unless_equals_int (width, 1024);
	fail_unless_equals_int (height, 768);
	fail_unless_equals_int (fmem_num, 2);
	fail_unless_equals_int (buf_pic_cnt, 5);
	fail_unless (g_str_equal (format, "RGB"));
	fail_unless (enable_vio6 == TRUE);
	fail_unless_equals_int (stride, 2048);
	fail_unless_equals_int (x_offset, 20);
	fail_unless_equals_int (y_offset, 30);
	g_free (device);
	device = NULL;
	g_free (format);
	format = NULL;

	/* new properties */
	g_object_set (acmh264dec,
				  "device", 		"/dev/video2",
				  "width", 			1920,
				  "height", 		1080,
				  "fmem-num", 		4,
				  "buf-pic-cnt", 	8,
				  "format", 		"RGBx",
				  "enable-vio6", 	FALSE,
				  "stride",			240,
				  "x-offset",		100,
				  "y-offset",		200,
				  NULL);
	g_object_get (acmh264dec,
				  "device", 		&device,
				  "width", 			&width,
				  "height", 		&height,
				  "fmem-num", 		&fmem_num,
				  "buf-pic-cnt", 	&buf_pic_cnt,
				  "format", 		&format,
				  "enable-vio6", 	&enable_vio6,
				  "stride",			&stride,
				  "x-offset",		&x_offset,
				  "y-offset",		&y_offset,
				  NULL);
	fail_unless (g_str_equal (device, "/dev/video2"));
	fail_unless_equals_int (width, 1920);
	fail_unless_equals_int (height, 1080);
	fail_unless_equals_int (fmem_num, 4);
	fail_unless_equals_int (buf_pic_cnt, 8);
	fail_unless (g_str_equal (format, "RGBx"));
	fail_unless (enable_vio6 == FALSE);
	fail_unless_equals_int (stride, 240);
	fail_unless_equals_int (x_offset, 100);
	fail_unless_equals_int (y_offset, 200);
	g_free (device);
	device = NULL;
	g_free (format);
	format = NULL;

	cleanup_acmh264dec (acmh264dec);
}
GST_END_TEST;

static GstFlowReturn
test_decode_mp4_chain(GstPad * pad, GstObject * parent, GstBuffer * buf)
{
	size_t size;
	void *p;
	char file[PATH_MAX];
	static gint nOutputBuffers = 0;
	GstBuffer *outbuffer;
	GstMapInfo map;
	GstFlowReturn ret;

	ret = g_base_chain(pad, parent, buf);
	
	/* 出力されたバッファのチェック */
	if (g_list_length (buffers) > 0) {
		++nOutputBuffers;
		
		outbuffer = GST_BUFFER (buffers->data);
		fail_if (outbuffer == NULL);
		fail_unless (GST_IS_BUFFER (outbuffer));
		
		sprintf(file, "data/h264/mp4/rgb_%03d.data", nOutputBuffers);
		g_print("%s\n", file);

		get_data(file, &size, &p);

		fail_unless (gst_buffer_get_size (outbuffer) == size);
		gst_buffer_map (outbuffer, &map, GST_MAP_READ);
		fail_unless (0 == memcmp(p, map.data, size));
		gst_buffer_unmap (outbuffer, &map);
		
		fail_unless (0 == munmap(p, size));
		
		buffers = g_list_remove (buffers, outbuffer);
		
		ASSERT_BUFFER_REFCOUNT (outbuffer, "outbuffer", 2);
		gst_buffer_unref (outbuffer);
		outbuffer = NULL;
	}
	
	return ret;
}

GST_START_TEST (test_decode_mp4)
{
	GstCaps *caps;
	
	GstElement *acmh264dec;
	size_t size;
	void *p;
	char file[PATH_MAX];
	GstBuffer *codec_buf;
	GstBuffer *inbuffer;
	gint nInputBuffers = 0;

	sprintf(file, "data/h264/mp4/_codec_data.data");
	g_print("%s\n", file);
	get_data(file, &size, &p);
	codec_buf = gst_buffer_new_and_alloc (size);
	gst_buffer_fill (codec_buf, 0, p, size);

	caps = gst_caps_from_string (AVC_CAPS_STRING);
	gst_caps_set_simple (caps, "codec_data", GST_TYPE_BUFFER, codec_buf,
						 NULL);
	gst_buffer_unref (codec_buf);

	acmh264dec = setup_acmh264dec ();
	fail_unless (gst_element_set_state (acmh264dec, GST_STATE_PLAYING)
				 == GST_STATE_CHANGE_SUCCESS, "could not set to playing");
	
	gst_pad_set_caps (mysrcpad, caps);

	g_base_chain = GST_PAD_CHAINFUNC (mysinkpad);
	gst_pad_set_chain_function (mysinkpad,
		GST_DEBUG_FUNCPTR (test_decode_mp4_chain));

	while (TRUE) {
		/* バッファの入力 */
		if (++nInputBuffers < PUSH_BUFFERS) {
			sprintf(file, "data/h264/mp4/h264_%03d.data", nInputBuffers);
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


		if (nInputBuffers == PUSH_BUFFERS) {
			/* EOS イベント送信 */
			fail_unless (gst_pad_push_event (mysrcpad,
				gst_event_new_eos ()) == TRUE);

			break;
		}
	}

	/* クリーンアップ	*/
	g_print("cleanup...\n");
	cleanup_acmh264dec (acmh264dec);
	g_list_free (buffers);
	buffers = NULL;
	
	gst_caps_unref (caps);
}
GST_END_TEST;

static GstFlowReturn
test_decode_ts_chain(GstPad * pad, GstObject * parent, GstBuffer * buf)
{
	size_t size;
	void *p;
	char file[PATH_MAX];
	static gint nOutputBuffers = 0;
	GstBuffer *outbuffer;
	GstMapInfo map;
	GstFlowReturn ret;
	
	ret = g_base_chain(pad, parent, buf);
	
	/* 出力されたバッファのチェック */
	if (g_list_length (buffers) > 0) {
		++nOutputBuffers;
		
		outbuffer = GST_BUFFER (buffers->data);
		fail_if (outbuffer == NULL);
		fail_unless (GST_IS_BUFFER (outbuffer));
		
		sprintf(file, "data/h264/ts/rgb_%03d.data", nOutputBuffers);
		g_print("%s\n", file);
		
		get_data(file, &size, &p);
		
		fail_unless (gst_buffer_get_size (outbuffer) == size);
		gst_buffer_map (outbuffer, &map, GST_MAP_READ);
		fail_unless (0 == memcmp(p, map.data, size));
		gst_buffer_unmap (outbuffer, &map);
		
		fail_unless (0 == munmap(p, size));
		
		buffers = g_list_remove (buffers, outbuffer);
		
		ASSERT_BUFFER_REFCOUNT (outbuffer, "outbuffer", 2);
		gst_buffer_unref (outbuffer);
		outbuffer = NULL;
	}
	
	return ret;
}

GST_START_TEST (test_decode_ts)
{
	GstCaps *caps;
	
	GstElement *acmh264dec;
	size_t size;
	void *p;
	char file[PATH_MAX];
	GstBuffer *codec_buf;
	GstBuffer *inbuffer;
	gint nInputBuffers = 0;
	
	sprintf(file, "data/h264/ts/_codec_data.data");
	g_print("%s\n", file);
	get_data(file, &size, &p);
	codec_buf = gst_buffer_new_and_alloc (size);
	gst_buffer_fill (codec_buf, 0, p, size);
	
	caps = gst_caps_from_string (AVC_CAPS_STRING);
	gst_caps_set_simple (caps, "codec_data", GST_TYPE_BUFFER, codec_buf,
						 NULL);
	gst_buffer_unref (codec_buf);
	
	acmh264dec = setup_acmh264dec ();
	fail_unless (gst_element_set_state (acmh264dec, GST_STATE_PLAYING)
				 == GST_STATE_CHANGE_SUCCESS, "could not set to playing");
	
	gst_pad_set_caps (mysrcpad, caps);
	
	g_base_chain = GST_PAD_CHAINFUNC (mysinkpad);
	gst_pad_set_chain_function (mysinkpad,
								GST_DEBUG_FUNCPTR (test_decode_ts_chain));
	
	while (TRUE) {
		/* バッファの入力 */
		if (++nInputBuffers < PUSH_BUFFERS) {
			sprintf(file, "data/h264/ts/h264_%03d.data", nInputBuffers);
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
		
		
		if (nInputBuffers == PUSH_BUFFERS) {
			/* EOS イベント送信 */
			fail_unless (gst_pad_push_event (mysrcpad,
							gst_event_new_eos ()) == TRUE);
			
			break;
		}
	}
	
	/* クリーンアップ	*/
	g_print("cleanup...\n");
	cleanup_acmh264dec (acmh264dec);
	g_list_free (buffers);
	buffers = NULL;
	
	gst_caps_unref (caps);
}
GST_END_TEST;

static Suite *
acmh264dec_suite (void)
{
	Suite *s = suite_create ("acmh264dec");
	TCase *tc_chain = tcase_create ("general");

	tcase_set_timeout (tc_chain, 0);

	suite_add_tcase (s, tc_chain);
	tcase_add_test (tc_chain, test_properties);
	tcase_add_test (tc_chain, test_decode_mp4);
	tcase_add_test (tc_chain, test_decode_ts);

	return s;
}

int
main (int argc, char **argv)
{
	int nf;
	
	Suite *s = acmh264dec_suite ();
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
