/* GStreamer
 *
 * Copyright (C) 2001-2002 Ronald Bultje <rbultje@ronald.bitfreak.net>
 *               2006 Edgard Lima <edgard.lima@indt.org.br>
 *               2009 Texas Instruments, Inc - http://www.ti.com/
 *               2013 Atmark Techno, Inc.
 *
 * gstv4l2bufferpool.h V4L2 buffer pool class
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

#ifndef __GST_V4L2_BUFFER_POOL_H__
#define __GST_V4L2_BUFFER_POOL_H__

#include <gst/gst.h>
#include <linux/videodev2.h>
#include "gstacmdmabufmeta.h"

typedef struct _GstV4l2BufferPool GstV4l2BufferPool;
typedef struct _GstV4l2BufferPoolClass GstV4l2BufferPoolClass;
typedef struct _GstV4l2Meta GstV4l2Meta;
typedef struct _GstV4l2InitParam GstV4l2InitParam;

GST_DEBUG_CATEGORY_EXTERN (v4l2buffer_debug);

G_BEGIN_DECLS


#define GST_TYPE_V4L2_BUFFER_POOL      (gst_v4l2_buffer_pool_get_type())
#define GST_IS_V4L2_BUFFER_POOL(obj)   (G_TYPE_CHECK_INSTANCE_TYPE ((obj), GST_TYPE_V4L2_BUFFER_POOL))
#define GST_V4L2_BUFFER_POOL(obj)      (G_TYPE_CHECK_INSTANCE_CAST ((obj), GST_TYPE_V4L2_BUFFER_POOL, GstV4l2BufferPool))
#define GST_V4L2_BUFFER_POOL_CAST(obj) ((GstV4l2BufferPool*)(obj))

/* size of v4l2 buffer pool in streaming case */
#define GST_V4L2_MAX_BUFFERS 16
#define GST_V4L2_MIN_BUFFERS 1

/* VIDIOC_DQBUF で、EAGAIN をエラー扱いしない	*/
#define USE_GST_FLOW_DQBUF_EAGAIN	1
#if USE_GST_FLOW_DQBUF_EAGAIN
	/* custom error : VIDIOC_DQBUF に失敗		*/
# define GST_FLOW_DQBUF_EAGAIN		GST_FLOW_CUSTOM_ERROR_2
#endif

typedef enum {
	GST_V4L2_IO_AUTO    = 0,
	GST_V4L2_IO_RW      = 1,
	GST_V4L2_IO_MMAP    = 2,
	GST_V4L2_IO_USERPTR = 3,
	GST_V4L2_IO_DMABUF  = 4,
} GstV4l2IOMode;

struct _GstV4l2Meta {
	GstMeta meta;
	
	gpointer mem;
	struct v4l2_buffer vbuffer;
};

/* 初期化パラメータ	*/
struct _GstV4l2InitParam
{
	gint video_fd;             
	enum v4l2_buf_type type;   /* VIDEO_CAPTURE, VIDEO_OUTPUT */
	GstV4l2IOMode mode;
	guint32 sizeimage;
	guint init_num_buffers;
	
	gint num_fb_dmabuf;
	gint fb_dmabuf_index[NUM_FB_DMABUF];
	gint fb_dmabuf_fd[NUM_FB_DMABUF];
};

/* クラス定義		*/
struct _GstV4l2BufferPool
{
	GstBufferPool parent;

	GstV4l2InitParam init_param;

	GstAllocator *allocator;
	GstAllocationParams params;
	guint size;

	guint num_buffers;         /* number of buffers we use */
	guint num_allocated;       /* number of buffers allocated by the driver */
	guint num_queued;          /* number of buffers queued in the driver */
	guint copy_threshold;      /* when our pool runs lower, start handing out copies */

	GstBuffer **buffers;
	
	gint next_qbuf_index;
};

struct _GstV4l2BufferPoolClass
{
	GstBufferPoolClass parent_class;
};

GType gst_v4l2_meta_api_get_type (void);
const GstMetaInfo * gst_v4l2_meta_get_info (void);
#define GST_V4L2_META_GET(buf) ((GstV4l2Meta *)gst_buffer_get_meta(buf,gst_v4l2_meta_api_get_type()))
#define GST_V4L2_META_ADD(buf) ((GstV4l2Meta *)gst_buffer_add_meta(buf,gst_v4l2_meta_get_info(),NULL))

GType gst_v4l2_buffer_pool_get_type (void);

GstV4l2BufferPool*	gst_v4l2_buffer_pool_new(
						GstV4l2InitParam *param, GstCaps *caps);

GstFlowReturn		gst_v4l2_buffer_pool_acquire_buffer(
						GstBufferPool * pool, GstBuffer ** buffer,
						GstBufferPoolAcquireParams * params);

GstFlowReturn		gst_v4l2_buffer_pool_qbuf(
						GstV4l2BufferPool * pool, GstBuffer * buf, gsize size);

gboolean 			gst_v4l2_buffer_pool_is_ready_to_dqbuf(
						GstV4l2BufferPool * pool);

GstFlowReturn		gst_v4l2_buffer_pool_dqbuf(
						GstV4l2BufferPool * pool, GstBuffer ** buffer);

GstFlowReturn		gst_v4l2_buffer_pool_dqbuf_ex(
						GstV4l2BufferPool * pool, GstBuffer ** buffer,
						guint32* bytesused);

/* for debug */
void 				gst_v4l2_buffer_pool_log_buf_status(GstV4l2BufferPool* pool);

G_END_DECLS

#endif /*__GST_V4L2_BUFFER_POOL_H__ */

/*
 * End of file
 */
