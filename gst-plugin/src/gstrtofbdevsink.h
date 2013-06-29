/* GStreamer
 * Copyright (C) 2007 Sean D'Epagnier sean@depagnier.com
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


#ifndef __GST_RTOFBDEVSINK_H__
#define __GST_RTOFBDEVSINK_H__

#include <gst/gst.h>
#include <gst/video/gstvideosink.h>
#include <gst/video/video.h>

#include <linux/fb.h>

G_BEGIN_DECLS

#define GST_TYPE_RTOFBDEVSINK \
  (gst_rto_fbdevsink_get_type())
#define GST_RTOFBDEVSINK(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_RTOFBDEVSINK,GstRtoFBDevSink))
#define GST_RTOFBDEVSINK_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_RTOFBDEVSINK,GstRtoFBDevSinkClass))
#define GST_IS_RTOFBDEVSINK(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_RTOFBDEVSINK))
#define GST_IS_RTOFBDEVSINK_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_RTOFBDEVSINK))

typedef enum {
	GST_RTOFBDEVSINK_OPEN      = (GST_ELEMENT_FLAG_LAST << 0),
	GST_RTOFBDEVSINK_FLAG_LAST = (GST_ELEMENT_FLAG_LAST << 2),
} GstRtoFBDevSinkFlags;

typedef struct _GstRtoFBDevSink GstRtoFBDevSink;
typedef struct _GstRtoFBDevSinkPrivate GstRtoFBDevSinkPrivate;
typedef struct _GstRtoFBDevSinkClass GstRtoFBDevSinkClass;

struct _GstRtoFBDevSink {
	GstBaseSink element;
	
	struct fb_fix_screeninfo fixinfo;
	struct fb_var_screeninfo varinfo;
	
	struct fb_var_screeninfo saved_varinfo;

	int fd;
	unsigned char *framebuffer;
	
	char *device;
	
	int width, height;
	int frame_stride, frame_x_offset, frame_y_offset;
	int cx, cy, linelen, lines, bytespp;
	
	int fps_n, fps_d;
	
	gboolean is_changed_fb_varinfo;

	gboolean use_dmabuf;

	/* 垂直同期の有効無効フラグ
	 * 0 : 無効
	 * 1 : 有効
	 */
	gboolean enable_vsync;

	/*< private >*/
	GstRtoFBDevSinkPrivate *priv;
};

struct _GstRtoFBDevSinkClass {
	GstBaseSinkClass parent_class;
};

GType gst_rto_fbdevsink_get_type(void);

G_END_DECLS

#endif /* __GST_RTOFBDEVSINK_H__ */

/*
 * End of file
 */
