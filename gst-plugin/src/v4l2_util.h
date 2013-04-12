/* GStreamer
 *
 * Copyright (C) 2013 Kazunari Ohtsuka <<user@hostname.org>>
 *
 * v4l2_util.h - generic V4L2 calls handling
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

#ifndef __V4L2_UTIL_H__
#define __V4L2_UTIL_H__

#include <gst/gst.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>
#define v4l2_close    close
#define v4l2_dup      dup
#define v4l2_read     read
#define v4l2_mmap     mmap
#define v4l2_munmap   munmap
#define v4l2_ioctl    gst_v4l2_ioctl


/* open/close the device */
gboolean	gst_v4l2_open			(char *dev, gint *fd, gboolean is_nonblock);
gboolean	gst_v4l2_close			(char *dev, gint fd);

gint gst_v4l2_ioctl(int fd, int request, void* arg);

#define LOG_CAPS(obj, caps) GST_DEBUG_OBJECT (obj, "%s: %" GST_PTR_FORMAT, #caps, caps)

#endif /* __V4L2_UTIL_H__ */

/*
 * End of file
 */
