/* GStreamer
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

#ifndef __GST_RTO_DMABUF_META_H__
#define __GST_RTO_DMABUF_META_H__

#include <gst/gst.h>

G_BEGIN_DECLS

/* DMABUF の個数		*/
#define NUM_FB_DMABUF			3

typedef struct _GstRtoDmabufMeta GstRtoDmabufMeta;

/**
 * GstRtoDmabufMeta:
 *
 * Buffer metadata for DMABUF.
 */
struct _GstRtoDmabufMeta {
	GstMeta meta;

	int fd;
	int index;
};

GType gst_rto_dmabuf_meta_api_get_type (void);
#define GST_RTO_DMABUF_META_API_TYPE (gst_rto_dmabuf_meta_api_get_type())

#define gst_buffer_get_rto_dmabuf_meta(b) \
  ((GstRtoDmabufMeta*)gst_buffer_get_meta((b),GST_RTO_DMABUF_META_API_TYPE))

/* implementation */
const GstMetaInfo *gst_rto_dmabuf_meta_get_info (void);
#define GST_RTO_DMABUF_META_INFO (gst_rto_dmabuf_meta_get_info())

GstRtoDmabufMeta * gst_buffer_add_rto_dmabuf_meta (
			GstBuffer *buffer, int fd, int index);

G_END_DECLS

#endif /* __GST_RTO_DMABUF_META_H__ */

/*
 * End of file
 */
