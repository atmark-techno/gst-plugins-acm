/* GStreamer
 *
 * Copyright (C) 2013 Atmark Techno, Inc.
 *
 * utest_util.c - unit test utility methods
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

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <inttypes.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <stdlib.h>
#include <gst/app/gstappsink.h>
#include <gst/check/gstcheck.h>

#include "utest_util.h"


void
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

GstBuffer *
create_audio_buffer (GstCaps * caps)
{
	GstElement *pipeline;
	GstElement *cf;
	GstElement *sink;
	GstSample *sample;
	GstBuffer *buffer;
	
	pipeline = gst_parse_launch(
		"audiotestsrc num-buffers=1 ! capsfilter name=cf ! appsink name=sink",
		NULL);
	g_assert (pipeline != NULL);
	
	cf = gst_bin_get_by_name (GST_BIN (pipeline), "cf");
	sink = gst_bin_get_by_name (GST_BIN (pipeline), "sink");
	
	g_object_set (G_OBJECT (cf), "caps", caps, NULL);
	
	gst_element_set_state (pipeline, GST_STATE_PLAYING);
	
	sample = gst_app_sink_pull_sample (GST_APP_SINK (sink));
	
	gst_element_set_state (pipeline, GST_STATE_NULL);
	gst_object_unref (pipeline);
	gst_object_unref (sink);
	gst_object_unref (cf);
	
	buffer = gst_sample_get_buffer (sample);
	gst_buffer_ref (buffer);
	
	gst_sample_unref (sample);
	
	return buffer;
}

GstBuffer *
create_video_buffer (GstCaps * caps)
{
	GstElement *pipeline;
	GstElement *cf;
	GstElement *sink;
	GstSample *sample;
	GstBuffer *buffer;
	
	pipeline = gst_parse_launch(
		"videotestsrc num-buffers=1 ! capsfilter name=cf ! appsink name=sink",
		NULL);
	g_assert (pipeline != NULL);
	
	cf = gst_bin_get_by_name (GST_BIN (pipeline), "cf");
	sink = gst_bin_get_by_name (GST_BIN (pipeline), "sink");
	
	g_object_set (G_OBJECT (cf), "caps", caps, NULL);
	
	gst_element_set_state (pipeline, GST_STATE_PLAYING);
	
	sample = gst_app_sink_pull_sample (GST_APP_SINK (sink));
	
	gst_element_set_state (pipeline, GST_STATE_NULL);
	gst_object_unref (pipeline);
	gst_object_unref (sink);
	gst_object_unref (cf);
	
	buffer = gst_sample_get_buffer (sample);
	gst_buffer_ref (buffer);
	
	gst_sample_unref (sample);
	
	return buffer;
}


/*
 * End of file
 */

