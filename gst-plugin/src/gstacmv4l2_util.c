/* GStreamer
 *
 * Copyright (C) 2013 Atmark Techno, Inc.
 *
 * gstacmv4l2_util.c - generic V4L2 calls handling
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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <dirent.h>

#include "gstacmv4l2_util.h"


GST_DEBUG_CATEGORY_STATIC (acm_v4l2util_debug);
#define GST_CAT_DEFAULT acm_v4l2util_debug

/*
 * get the device's capabilities
 * return value: TRUE on success, FALSE on error
 */
static gboolean
get_capabilities (gint fd, struct v4l2_capability *vcap)
{
	GST_LOG("getting capabilities");
	
	if (gst_acm_v4l2_ioctl (fd, VIDIOC_QUERYCAP, vcap) < 0) {
		GST_ERROR("failed VIDIOC_QUERYCAP. %s.\n",
				  g_strerror(errno));
		
    	goto cap_failed;
	}
	
	GST_LOG ("driver:      '%s'", vcap->driver);
	GST_LOG ("card:        '%s'", vcap->card);
	GST_LOG ("bus_info:    '%s'", vcap->bus_info);
	GST_LOG ("version:     %08x", vcap->version);
	GST_LOG ("capabilites: %08x", vcap->capabilities);
	
	return TRUE;
	
	/* ERRORS */
cap_failed:
	{
		GST_ERROR ("Error getting capabilities for device : "
				   "It isn't a v4l2 driver. Check if it is a v4l1 driver.");
		return FALSE;
	}
}

static gboolean
is_video_dev(const gchar *dev)
{
	return g_str_has_prefix(dev, "video");
}

/*
 * open the video device
 * return value: TRUE on success, FALSE on error
 */
gboolean
gst_acm_v4l2_open (char *dev, gint *fd, gboolean is_nonblock)
{
	struct stat st;
	/* the video device's capabilities */
	struct v4l2_capability vcap;

	GST_DEBUG_CATEGORY_INIT (acm_v4l2util_debug, "acmv4l2util", 0,
							 "acm v4l2util debug");

	/* check if it is a device */
	if (stat (dev, &st) == -1)
		goto stat_failed;
	
	if (!S_ISCHR (st.st_mode))
		goto no_device;
	
	/* open the device */
	if (is_nonblock) {
		*fd = open (dev, O_RDWR | O_NONBLOCK, 0);
	}
	else {
		*fd = open (dev, O_RDWR, 0);
	}
	if (*fd < 0) {
		goto not_open;
	}
	
	/* get capabilities, error will be posted */
	if (!get_capabilities (*fd, &vcap))
		goto error;
	
	/* do we need to be a capture device? */
	if (!(vcap.capabilities & V4L2_CAP_VIDEO_CAPTURE))
		goto not_capture;
	
	if (!(vcap.capabilities & V4L2_CAP_VIDEO_OUTPUT))
		goto not_output;
	
	GST_INFO ("Opened device '%s' (%s) successfully",
			  vcap.card, dev);
	
	return TRUE;
	
	/* ERRORS */
stat_failed:
	{
		GST_ERROR ("Cannot identify device '%s'.", dev);
		goto error;
	}
no_device:
	{
		GST_ERROR ("This isn't a device '%s'.", dev);
		goto error;
	}
not_open:
	{
		GST_ERROR ("Could not open device '%s' for reading and writing.",
				   dev);
		goto error;
	}
not_capture:
	{
		GST_ERROR ("Device '%s' is not a capture device.",
				   dev);
		goto error;
	}
not_output:
	{
		GST_ERROR ("Device '%s' is not a output device.",
				   dev);
		goto error;
	}
error:
	{
		if (*fd > 0) {
			/* close device */
			close (*fd);
			*fd = -1;
		}
		
		return FALSE;
	}
}


/*
 * close the video device 
 * return value: TRUE on success, FALSE on error
 */
gboolean
gst_acm_v4l2_close (char *dev, gint fd)
{
  GST_INFO ("Trying to close %s (%d)", dev, fd);

  /* close device */
  close (fd);

  return TRUE;
}

/*
 * ioctl for video device
 * return value: result of ioctl()
 */
gint
gst_acm_v4l2_ioctl(int fd, int request, void* arg)
{
	int e;
	
	do {
		e = ioctl(fd, request, arg);
	} while (-1 == e && EINTR == errno);
	
	return e;
}

gchar*
gst_acm_v4l2_getdev (gchar *driver)
{
	gchar *video_dev = NULL;
	gint fd;
	gboolean ret;
	DIR *dp;
	struct dirent *ep;
	struct v4l2_capability vcap;

	GST_DEBUG_CATEGORY_INIT (acm_v4l2util_debug, "acmv4l2util", 0,
							 "acm v4l2util debug");

	GST_INFO ("Try find device '%s'", driver);

	dp = opendir("/dev");
	if (dp == NULL) {
		GST_ERROR ("Could not open directory '/dev'");
		return NULL;
	}

	while ((ep = readdir(dp))) {
		if (is_video_dev(ep->d_name)) {
			g_free(video_dev);
			video_dev = g_strdup_printf ("/dev/%s", ep->d_name);

			if (!gst_acm_v4l2_open (video_dev, &fd, TRUE))
				continue;
			ret = get_capabilities (fd, &vcap);
			gst_acm_v4l2_close(video_dev, fd);
			if (!ret)
				continue;
			if (!g_strcmp0 ((gchar *)vcap.driver, driver)) {
				GST_INFO ("Found device '%s' - %s", driver, video_dev);

				return video_dev;
			}
		}
	}

	g_free(video_dev);
	return NULL;
}

/*
 * End of file
 */

