/* GStreamer
 *
 * unit test for acmaacdec
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


/* チャンネル数のプロパティ指定は意味ない	*/
#define ENABLE_CHANNEL_PROPERTY		0

/* v4l2 mem2mem デバイスファイル	*/
#define M2M_DEVICE	"/dev/video1"

/* For ease of programming we use globals to keep refs for our floating
 * src and sink pads we create; otherwise we always have to do get_pad,
 * get_peer, and then remove references in every test function */
static GstPad *mysrcpad, *mysinkpad;

#define AUDIO_CAPS_STRING "audio/x-raw, " \
	"format = (string) " GST_AUDIO_NE (S16) ", " \
	"rate = (int) [ 8000, 96000 ], " \
	"channels = (int)  [ 1, 8 ]"

#define AAC_CAPS_STRING "audio/mpeg, " \
	"mpegversion = (int) 4, " \
	"rate = (int) 48000, " \
	"channels = (int) 2, " \
	"framed = (boolean) true "

static GstStaticPadTemplate sinktemplate = GST_STATIC_PAD_TEMPLATE ("sink",
	GST_PAD_SINK,
	GST_PAD_ALWAYS,
	GST_STATIC_CAPS (AUDIO_CAPS_STRING));

static GstStaticPadTemplate srctemplate = GST_STATIC_PAD_TEMPLATE ("src",
	GST_PAD_SRC,
	GST_PAD_ALWAYS,
	GST_STATIC_CAPS (AAC_CAPS_STRING));


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
setup_acmaacdec (void)
{
	GstElement *acmaacdec;
	
	g_print ("setup_acmaacdec\n");
	acmaacdec = gst_check_setup_element ("acmaacdec");
	g_object_set (acmaacdec,
				  "device", M2M_DEVICE,
				  NULL);
	g_print ("pass : gst_check_setup_element()\n");
	mysrcpad = gst_check_setup_src_pad (acmaacdec, &srctemplate);
	g_print ("pass : gst_check_setup_src_pad()\n");
	mysinkpad = gst_check_setup_sink_pad (acmaacdec, &sinktemplate);
	g_print ("pass : gst_check_setup_sink_pad()\n");

	gst_pad_set_active (mysrcpad, TRUE);
	gst_pad_set_active (mysinkpad, TRUE);
	
	return acmaacdec;
}

static void
cleanup_acmaacdec (GstElement * acmaacdec)
{
	g_print ("cleanup_acmaacdec\n");
	gst_element_set_state (acmaacdec, GST_STATE_NULL);
	
	gst_pad_set_active (mysrcpad, FALSE);
	gst_pad_set_active (mysinkpad, FALSE);
	gst_check_teardown_src_pad (acmaacdec);
	gst_check_teardown_sink_pad (acmaacdec);
	gst_check_teardown_element (acmaacdec);
}

GST_START_TEST (test_properties)
{
	GstElement *acmaacdec;
	gchar *device;
	gboolean allow_mixdown;
	gint	mixdown_mode;
	gint 	compliant_standard;
	gint 	pcm_format;
#if ENABLE_CHANNEL_PROPERTY
	guint 	max_channel;
#endif

	acmaacdec = setup_acmaacdec ();
	
	g_object_set (acmaacdec,
				  "device", 			"/dev/video1",
				  "allow-mixdown", 		TRUE,
				  "mixdown-mode", 		1,
				  "compliant-standard", 1,
				  "pcm-format", 		1,
#if ENABLE_CHANNEL_PROPERTY
				  "max-channel", 		1,
#endif
				  NULL);
	g_object_get (acmaacdec,
				  "device", 			&device,
				  "allow-mixdown", 		&allow_mixdown,
				  "mixdown-mode", 		&mixdown_mode,
				  "compliant-standard", &compliant_standard,
				  "pcm-format", 		&pcm_format,
#if ENABLE_CHANNEL_PROPERTY
				  "max-channel", 		&max_channel,
#endif
				  NULL);
	fail_unless (g_str_equal (device, "/dev/video1"));
	fail_unless (allow_mixdown == TRUE);
	fail_unless_equals_int (mixdown_mode, 1);
	fail_unless_equals_int (compliant_standard, 1);
	fail_unless_equals_int (pcm_format, 1);
#if ENABLE_CHANNEL_PROPERTY
	fail_unless_equals_int (max_channel, 1);
#endif
	g_free (device);
	device = NULL;
	
	/* new properties */
	g_object_set (acmaacdec,
				  "device", 			"/dev/video2",
				  "allow-mixdown", 		FALSE,
				  "mixdown-mode", 		0,
				  "compliant-standard", 0,
				  "pcm-format", 		0,
#if ENABLE_CHANNEL_PROPERTY
				  "max-channel", 		2,
#endif
				  NULL);
	g_object_get (acmaacdec,
				  "device", 			&device,
				  "allow-mixdown", 		&allow_mixdown,
				  "mixdown-mode", 		&mixdown_mode,
				  "compliant-standard", &compliant_standard,
				  "pcm-format", 		&pcm_format,
#if ENABLE_CHANNEL_PROPERTY
				  "max-channel", 		&max_channel,
#endif
				  NULL);
	fail_unless (g_str_equal (device, "/dev/video2"));
	fail_unless (allow_mixdown == FALSE);
	fail_unless_equals_int (mixdown_mode, 0);
	fail_unless_equals_int (compliant_standard, 0);
	fail_unless_equals_int (pcm_format, 0);
#if ENABLE_CHANNEL_PROPERTY
	fail_unless_equals_int (max_channel, 2);
#endif

	cleanup_acmaacdec (acmaacdec);
}
GST_END_TEST;

static GstFlowReturn
test_decode_adts_chain(GstPad * pad, GstObject * parent, GstBuffer * buf)
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
		
		sprintf(file, "data/aac/adts/pcm_%03d.data", nOutputBuffers);
		g_print("%s\n", file);
		get_data(file, &size, &p);
		
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

GST_START_TEST (test_decode_adts)
{
	GstCaps *caps;
	
	GstElement *acmaacdec;
	size_t size;
	void *p;
	char file[PATH_MAX];
	GstBuffer *inbuffer;
	gint nInputBuffers = 0;

	caps = gst_caps_from_string (AAC_CAPS_STRING);
	gst_caps_set_simple (caps, "stream-format", G_TYPE_STRING, "adts",
						 NULL);
	
	acmaacdec = setup_acmaacdec ();
	fail_unless (gst_element_set_state (acmaacdec, GST_STATE_PLAYING)
				 == GST_STATE_CHANGE_SUCCESS, "could not set to playing");

	gst_pad_set_caps (mysrcpad, caps);

	g_base_chain = GST_PAD_CHAINFUNC (mysinkpad);
	gst_pad_set_chain_function (mysinkpad,
		GST_DEBUG_FUNCPTR (test_decode_adts_chain));

	while (TRUE) {
		/* バッファの入力 */
		if (++nInputBuffers < PUSH_BUFFERS) {
			sprintf(file, "data/aac/adts/aac_%03d.data", nInputBuffers);
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
	cleanup_acmaacdec (acmaacdec);
	g_list_free (buffers);
	buffers = NULL;

	gst_caps_unref (caps);
}
GST_END_TEST;

static GstFlowReturn
test_decode_raw_chain(GstPad * pad, GstObject * parent, GstBuffer * buf)
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
		
		sprintf(file, "data/aac/raw/pcm_%03d.data", nOutputBuffers);
		g_print("%s\n", file);
		get_data(file, &size, &p);
		
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

GST_START_TEST (test_decode_raw)
{
	GstCaps *caps;
	
	GstElement *acmaacdec;
	size_t size;
	void *p;
	char file[PATH_MAX];
	GstBuffer *codec_buf;
	GstBuffer *inbuffer;
	gint nInputBuffers = 0;

	sprintf(file, "data/aac/raw/_codec_data.data");
	g_print("%s\n", file);
	get_data(file, &size, &p);
	codec_buf = gst_buffer_new_and_alloc (size);
	gst_buffer_fill (codec_buf, 0, p, size);

	caps = gst_caps_from_string (AAC_CAPS_STRING);
	gst_caps_set_simple (caps, "stream-format", G_TYPE_STRING, "raw",
						 "codec_data", GST_TYPE_BUFFER, codec_buf,
						 NULL);
	gst_buffer_unref (codec_buf);

	acmaacdec = setup_acmaacdec ();
	fail_unless (gst_element_set_state (acmaacdec, GST_STATE_PLAYING)
				 == GST_STATE_CHANGE_SUCCESS, "could not set to playing");
	
	gst_pad_set_caps (mysrcpad, caps);

	g_base_chain = GST_PAD_CHAINFUNC (mysinkpad);
	gst_pad_set_chain_function (mysinkpad,
		GST_DEBUG_FUNCPTR (test_decode_raw_chain));

	while (TRUE) {
		/* バッファの入力 */
		if (++nInputBuffers < PUSH_BUFFERS) {
			sprintf(file, "data/aac/raw/aac_%03d.data", nInputBuffers);
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
	cleanup_acmaacdec (acmaacdec);
	g_list_free (buffers);
	buffers = NULL;
	
	gst_caps_unref (caps);
}
GST_END_TEST;

static Suite *
acmaacdec_suite (void)
{
	Suite *s = suite_create ("acmaacdec");
	TCase *tc_chain = tcase_create ("general");

	tcase_set_timeout (tc_chain, 0);

	suite_add_tcase (s, tc_chain);
	tcase_add_test (tc_chain, test_properties);
	tcase_add_test (tc_chain, test_decode_adts);
	tcase_add_test (tc_chain, test_decode_raw);

	return s;
}

int
main (int argc, char **argv)
{
	int nf;
	
	Suite *s = acmaacdec_suite ();
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
