/* GStreamer
 *
 * unit test for acmfbdevsink
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

#include <unistd.h>

#include <gst/check/gstcheck.h>

static GstStaticPadTemplate srctemplate = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY);

static GstPad *srcpad;

static GstElement *sink;

static GstElement *
setup_acmfbdevsink (void)
{
	GST_DEBUG ("setup_acmfbdevsink");
	sink = gst_check_setup_element ("acmfbdevsink");
	srcpad = gst_check_setup_src_pad (sink, &srctemplate);
	gst_pad_set_active (srcpad, TRUE);

	return sink;
}

static void
cleanup_acmfbdevsink (GstElement * sink)
{
	GST_DEBUG ("cleanup_acmfbdevsink");

	gst_check_teardown_src_pad (sink);
	gst_check_teardown_element (sink);
}


GST_START_TEST (test_properties)
{
	GstElement *sink;
	gchar *device = NULL;
	gboolean use_dmabuf;
	gboolean enable_vsync;

	sink = setup_acmfbdevsink ();

	g_object_set (G_OBJECT (sink),
				  "device", "/dev/fb7",
				  "use-dmabuf", TRUE,
				  "enable-vsync", TRUE,
				  NULL);

	g_object_get (sink,
				  "device", &device,
				  "use-dmabuf", &use_dmabuf,
				  "enable-vsync", &enable_vsync,
				  NULL);

	fail_unless (g_str_equal (device, "/dev/fb7"));
	fail_unless_equals_int (use_dmabuf, 1);

	g_free (device);
	device = NULL;

	/* new properties */
	g_object_set (G_OBJECT (sink), "device", "/dev/fb9", NULL);
	g_object_get (sink, "device", &device, NULL);
	fail_unless (g_str_equal (device, "/dev/fb9"));
	g_free (device);

	g_object_set (G_OBJECT (sink), "use-dmabuf", FALSE, NULL);
	g_object_get (sink, "use-dmabuf", &use_dmabuf, NULL);
	fail_unless_equals_int (use_dmabuf, 0);

	g_object_set (G_OBJECT (sink), "enable-vsync", FALSE, NULL);
	g_object_get (sink, "enable-vsync", &enable_vsync, NULL);
	fail_unless_equals_int (enable_vsync, 0);

	cleanup_acmfbdevsink (sink);
}
GST_END_TEST;

static Suite *
acmfbdevsink_suite (void)
{
	Suite *s = suite_create ("acmfbdevsink");
	TCase *tc_chain = tcase_create ("general");
	
	tcase_set_timeout (tc_chain, 0);
	
	suite_add_tcase (s, tc_chain);
	tcase_set_timeout (tc_chain, 20);
	tcase_add_test (tc_chain, test_properties);
	
	return s;
}

int
main (int argc, char **argv)
{
	int nf;
	
	Suite *s = acmfbdevsink_suite ();
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
