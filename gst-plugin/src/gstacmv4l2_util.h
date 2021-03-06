/* GStreamer
 *
 * Copyright (C) 2013 Atmark Techno, Inc.
 *
 * gstacmv4l2_util.h - generic V4L2 calls handling
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

#ifndef __GST_ACM_V4L2_UTIL_H__
#define __GST_ACM_V4L2_UTIL_H__

#include <gst/gst.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>
#define gst_acm_v4l2_dup      dup
#define gst_acm_v4l2_read     read
#define gst_acm_v4l2_mmap     mmap
#define gst_acm_v4l2_munmap   munmap


/* open/close the device */
gboolean	gst_acm_v4l2_open(char *dev, gint *fd, gboolean is_nonblock);
gboolean	gst_acm_v4l2_close(char *dev, gint fd);

gint gst_acm_v4l2_ioctl(int fd, int request, void* arg);

gchar *gst_acm_v4l2_getdev(gchar *driver);

#define LOG_CAPS(obj, caps) GST_DEBUG_OBJECT (obj, "%s: %" GST_PTR_FORMAT, #caps, caps)

#endif /* __GST_ACM_V4L2_UTIL_H__ */

/*
 * End of file
 */
