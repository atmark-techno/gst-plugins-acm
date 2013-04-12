/* GStreamer
 *
 * Copyright (C) 2013 Kazunari Ohtsuka <<user@hostname.org>>
 *
 * v4l2_util.c - generic V4L2 calls handling
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

#include "v4l2_util.h"


GST_DEBUG_CATEGORY_STATIC (v4l2util_debug);
#define GST_CAT_DEFAULT v4l2util_debug

/*
 * get the device's capabilities
 * return value: TRUE on success, FALSE on error
 */
static gboolean
gst_v4l2_get_capabilities (gint fd, struct v4l2_capability *vcap)
{
	GST_LOG("getting capabilities");
	
	if (v4l2_ioctl (fd, VIDIOC_QUERYCAP, vcap) < 0) {
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

/*
 * open the video device
 * return value: TRUE on success, FALSE on error
 */
gboolean
gst_v4l2_open (char *dev, gint *fd, gboolean is_nonblock)
{
	struct stat st;
	/* the video device's capabilities */
	struct v4l2_capability vcap;

	GST_DEBUG_CATEGORY_INIT (v4l2util_debug, "v4l2util", 0,
							 "v4l2util debug");

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
	if (!gst_v4l2_get_capabilities (*fd, &vcap))
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
			v4l2_close (*fd);
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
gst_v4l2_close (char *dev, gint fd)
{
  GST_INFO ("Trying to close %s (%d)",
      dev, fd);

  /* close device */
  v4l2_close (fd);

  return TRUE;
}

/*
 * ioctl for video device
 * return value: result of ioctl()
 */
gint
gst_v4l2_ioctl(int fd, int request, void* arg)
{
	int e;
	
	do {
		e = ioctl(fd, request, arg);
	} while (-1 == e && EINTR == errno);
	
	return e;
}

/*
 * End of file
 */

