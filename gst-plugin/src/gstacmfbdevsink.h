/* GStreamer
 * Copyright (C) 2007 Sean D'Epagnier sean@depagnier.com
 * Copyright (C) 2013 Kazunari Ohtsuka <<kaz@stprec.co.jp>>
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


#ifndef __GST_ACMFBDEVSINK_H__
#define __GST_ACMFBDEVSINK_H__

#include <gst/gst.h>
#include <gst/video/gstvideosink.h>
#include <gst/video/video.h>

#include <linux/fb.h>

G_BEGIN_DECLS

#define GST_TYPE_ACMFBDEVSINK \
  (gst_acm_fbdevsink_get_type())
#define GST_ACMFBDEVSINK(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_ACMFBDEVSINK,GstAcmFBDevSink))
#define GST_ACMFBDEVSINK_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_ACMFBDEVSINK,GstAcmFBDevSinkClass))
#define GST_IS_ACMFBDEVSINK(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_ACMFBDEVSINK))
#define GST_IS_ACMFBDEVSINK_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_ACMFBDEVSINK))

typedef enum {
	GST_ACMFBDEVSINK_OPEN      = (GST_ELEMENT_FLAG_LAST << 0),
	GST_ACMFBDEVSINK_FLAG_LAST = (GST_ELEMENT_FLAG_LAST << 2),
} GstAcmFBDevSinkFlags;

typedef struct _GstAcmFBDevSink GstAcmFBDevSink;
typedef struct _GstAcmFBDevSinkPrivate GstAcmFBDevSinkPrivate;
typedef struct _GstAcmFBDevSinkClass GstAcmFBDevSinkClass;

struct _GstAcmFBDevSink {
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
	GstAcmFBDevSinkPrivate *priv;
};

struct _GstAcmFBDevSinkClass {
	GstBaseSinkClass parent_class;
};

GType gst_acm_fbdevsink_get_type(void);

G_END_DECLS

#endif /* __GST_ACMFBDEVSINK_H__ */

/*
 * End of file
 */
