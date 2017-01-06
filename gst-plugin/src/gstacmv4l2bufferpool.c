/* GStreamer
 *
 * Copyright (C) 2001-2002 Ronald Bultje <rbultje@ronald.bitfreak.net>
 *               2006 Edgard Lima <edgard.lima@indt.org.br>
 *               2009 Texas Instruments, Inc - http://www.ti.com/
 *               2013 Atmark Techno, Inc.
 *
 * gstacmv4l2bufferpool.c V4L2 buffer pool class
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
#  include <config.h>
#endif

#include <sys/mman.h>
#include <string.h>
#include <unistd.h>

#include "gst/video/video.h"
#include "gst/video/gstvideometa.h"
#include "gst/video/gstvideopool.h"

#include "gstacmv4l2bufferpool.h"
#include "gstacmv4l2_util.h"
#include "gstacmdmabufmeta.h"

/* videodev2.h is not versioned and we can't easily check for the presence
 * of enum values at compile time, but the V4L2_CAP_VIDEO_OUTPUT_OVERLAY define
 * was added in the same commit as V4L2_FIELD_INTERLACED_{TB,BT} (b2787845) */
#ifndef V4L2_CAP_VIDEO_OUTPUT_OVERLAY
#define V4L2_FIELD_INTERLACED_TB 8
#define V4L2_FIELD_INTERLACED_BT 9
#endif

/* 確保するバッファ数	*/
#define DEFAULT_MIN_BUFFERS		4
#define DEFAULT_NUM_BUFFERS		4
#define DEFAULT_MAX_BUFFERS		0	/* 0 for unlimited.	*/


/* デバッグログ出力フラグ		*/
#define DBG_LOG_DQBUF			0

#define TYPE_STR(type)	\
	(V4L2_BUF_TYPE_VIDEO_CAPTURE == type ? "CAP" : "OUT")

//GST_DEBUG_CATEGORY_EXTERN (acm_v4l2_debug);
GST_DEBUG_CATEGORY (acm_v4l2_debug);
#define GST_CAT_DEFAULT acm_v4l2_debug

/*
 * GstAcmV4l2Buffer:
 */
GType
gst_acm_v4l2_meta_api_get_type (void)
{
	static volatile GType type;
	static const gchar *tags[] = { "memory", NULL };
	
	if (g_once_init_enter (&type)) {
		GType _type = gst_meta_api_type_register ("GstAcmV4l2MetaAPI", tags);
		g_once_init_leave (&type, _type);
	}
	return type;
}

const GstMetaInfo *
gst_acm_v4l2_meta_get_info (void)
{
	static const GstMetaInfo *meta_info = NULL;
	
	if (g_once_init_enter (&meta_info)) {
		const GstMetaInfo *meta =
        gst_meta_register (gst_acm_v4l2_meta_api_get_type (), "GstAcmV4l2Meta",
			sizeof (GstAcmV4l2Meta), (GstMetaInitFunction) NULL,
			(GstMetaFreeFunction) NULL, (GstMetaTransformFunction) NULL);
		g_once_init_leave (&meta_info, meta);
	}
	return meta_info;
}

/*
 * GstAcmV4l2BufferPool:
 */
#define gst_acm_v4l2_buffer_pool_parent_class parent_class
G_DEFINE_TYPE (GstAcmV4l2BufferPool, gst_acm_v4l2_buffer_pool, GST_TYPE_BUFFER_POOL);

static void gst_acm_v4l2_buffer_pool_release_buffer (GstBufferPool * bpool,
    GstBuffer * buffer);

static void
gst_acm_v4l2_buffer_pool_free_buffer (GstBufferPool * bpool, GstBuffer * buffer)
{
	GstAcmV4l2BufferPool *pool = GST_ACM_V4L2_BUFFER_POOL (bpool);

	GST_INFO_OBJECT (pool, "%s: - FREE BUFFER. (%p)",
					 TYPE_STR(pool->init_param.type), buffer);

	switch (pool->init_param.mode) {
	case GST_ACM_V4L2_IO_RW:
		break;
	case GST_ACM_V4L2_IO_MMAP:
	{
		GstAcmV4l2Meta *meta;
		gint index;

		meta = GST_ACM_V4L2_META_GET (buffer);
		if (NULL == meta) {
			GST_ERROR_OBJECT (pool, "%s: - meta is NULL",
							  TYPE_STR(pool->init_param.type));
		}
		g_assert (meta != NULL);

		index = meta->vbuffer.index;
		GST_INFO_OBJECT (pool,
			"%s: - unmap buffer %p idx %d (data %p, len %u)",
			TYPE_STR(pool->init_param.type), buffer,index, meta->mem, meta->vbuffer.length);

		gst_acm_v4l2_munmap (meta->mem, meta->vbuffer.length);
		pool->buffers[index] = NULL;
		break;
	}
	case GST_ACM_V4L2_IO_DMABUF:
	{
		GstAcmV4l2Meta *meta;
		gint index;

		meta = GST_ACM_V4L2_META_GET (buffer);
		if (NULL == meta) {
			GST_ERROR_OBJECT (pool, "%s: - meta is NULL",
							  TYPE_STR(pool->init_param.type));
		}
		g_assert (meta != NULL);

		index = meta->vbuffer.index;
		GST_INFO_OBJECT (pool,
			"%s: - free buffer %p idx %d (data %p, len %u)",
			TYPE_STR(pool->init_param.type), buffer,index, meta->mem, meta->vbuffer.length);

		pool->buffers[index] = NULL;
		break;
	}
	default:
		g_assert_not_reached ();
		break;
	}
	gst_buffer_unref (buffer);
}

static GstFlowReturn
gst_acm_v4l2_buffer_pool_alloc_buffer (GstBufferPool * bpool, GstBuffer ** buffer,
    GstBufferPoolAcquireParams * params)
{
	GstAcmV4l2BufferPool *pool = GST_ACM_V4L2_BUFFER_POOL (bpool);
	GstBuffer *newbuf;
	GstAcmV4l2Meta *meta;
	guint index;

	switch (pool->init_param.mode) {
	case GST_ACM_V4L2_IO_RW:
	{
		newbuf = gst_buffer_new_allocate (
					pool->allocator, pool->size, &pool->params);
		break;
	}
	case GST_ACM_V4L2_IO_MMAP:
	{
		newbuf = gst_buffer_new ();
		meta = GST_ACM_V4L2_META_ADD (newbuf);
		
		index = pool->num_allocated;
		
		GST_DEBUG_OBJECT (pool, "%s: - CREATING BUFFER index:%u, %p",
						 TYPE_STR(pool->init_param.type), index, newbuf);
		
		meta->vbuffer.index = index;
		meta->vbuffer.type = pool->init_param.type;
		meta->vbuffer.memory = V4L2_MEMORY_MMAP;
		
		GST_INFO_OBJECT (pool, "%s: - VIDIOC_QUERYBUF", TYPE_STR(pool->init_param.type));
		if (gst_acm_v4l2_ioctl (pool->init_param.video_fd,
						VIDIOC_QUERYBUF, &meta->vbuffer) < 0) {
			goto querybuf_failed;
		}

#if 0	// for debug
		GST_INFO_OBJECT (pool, "  index:     %u", meta->vbuffer.index);
		GST_INFO_OBJECT (pool, "  type:      %d", meta->vbuffer.type);
		GST_INFO_OBJECT (pool, "  bytesused: %u", meta->vbuffer.bytesused);
		GST_INFO_OBJECT (pool, "  flags:     %08x", meta->vbuffer.flags);
		GST_INFO_OBJECT (pool, "  field:     %d", meta->vbuffer.field);
		GST_INFO_OBJECT (pool, "  memory:    %d", meta->vbuffer.memory);
		if (meta->vbuffer.memory == V4L2_MEMORY_MMAP)
			GST_INFO_OBJECT (pool, "  MMAP offset:  %u", meta->vbuffer.m.offset);
		GST_INFO_OBJECT (pool, "  length:    %u", meta->vbuffer.length);
#endif

		meta->mem = gst_acm_v4l2_mmap (0, meta->vbuffer.length,
						PROT_READ | PROT_WRITE, MAP_SHARED, pool->init_param.video_fd,
						meta->vbuffer.m.offset);
		if (meta->mem == MAP_FAILED) {
			goto mmap_failed;
		}
		
		gst_buffer_append_memory (newbuf,
			gst_memory_new_wrapped (GST_MEMORY_FLAG_NO_SHARE,
				meta->mem, meta->vbuffer.length, 0, meta->vbuffer.length, NULL,
				NULL));
		
		break;
	}
	case GST_ACM_V4L2_IO_DMABUF:
	{
		newbuf = gst_buffer_new ();
		meta = GST_ACM_V4L2_META_ADD (newbuf);
		
		index = pool->num_allocated;
		
		GST_DEBUG_OBJECT (pool, "%s: - CREATING BUFFER index:%u, %p",
						 TYPE_STR(pool->init_param.type), index, newbuf);
		
		meta->vbuffer.index = index;
		meta->vbuffer.type = pool->init_param.type;
		meta->vbuffer.memory = V4L2_MEMORY_DMABUF;
		GST_INFO_OBJECT (pool, "%s - VIDIOC_QUERYBUF", TYPE_STR(pool->init_param.type));
		if (gst_acm_v4l2_ioctl (pool->init_param.video_fd,
						VIDIOC_QUERYBUF, &meta->vbuffer) < 0) {
			goto querybuf_failed;
		}

#if 0	// for debug
		GST_INFO_OBJECT (pool, "  index:     %u", meta->vbuffer.index);
		GST_INFO_OBJECT (pool, "  type:      %d", meta->vbuffer.type);
		GST_INFO_OBJECT (pool, "  bytesused: %u", meta->vbuffer.bytesused);
		GST_INFO_OBJECT (pool, "  flags:     %08x", meta->vbuffer.flags);
		GST_INFO_OBJECT (pool, "  field:     %d", meta->vbuffer.field);
		GST_INFO_OBJECT (pool, "  memory:    %d", meta->vbuffer.memory);
		GST_INFO_OBJECT (pool, "  userptr:   %p", meta->vbuffer.m.userptr);
		GST_INFO_OBJECT (pool, "  length:    %u", meta->vbuffer.length);
#endif

		/* DMABUFのfdをメタデータとして保存		*/
		if (! gst_buffer_add_acm_dmabuf_meta (newbuf,
				pool->init_param.fb_dmabuf_fd[index],
				index)) {
			goto add_dmabuf_meta_failed;
		}
		/* DMABUFのfdを struct v4l2_buffer に保持		*/
		meta->vbuffer.m.fd = pool->init_param.fb_dmabuf_fd[index];
		GST_INFO_OBJECT (pool, "  fd:        %u",
						 pool->init_param.fb_dmabuf_fd[index]);

		break;
	}
	default:
		newbuf = NULL;
		g_assert_not_reached ();
	}
	
	pool->num_allocated++;
	
	*buffer = newbuf;
	
	return GST_FLOW_OK;
	
	/* ERRORS */
querybuf_failed:
	{
		gint errnosave = errno;
		
		GST_ERROR_OBJECT (pool,
			"%s: - Failed QUERYBUF: %s", TYPE_STR(pool->init_param.type),
			g_strerror (errnosave));
		gst_buffer_unref (newbuf);
		errno = errnosave;
		return GST_FLOW_ERROR;
	}
mmap_failed:
	{
		gint errnosave = errno;
		
		GST_ERROR_OBJECT (pool,
			"%s: - Failed to mmap: %s", TYPE_STR(pool->init_param.type),
			g_strerror (errnosave));
		gst_buffer_unref (newbuf);
		errno = errnosave;
		return GST_FLOW_ERROR;
	}
add_dmabuf_meta_failed:
	{
		GST_ERROR_OBJECT (pool,
			"%s: - Failed to add dmabuf meta", TYPE_STR(pool->init_param.type));
		gst_buffer_unref (newbuf);
		return GST_FLOW_ERROR;
	}
}

static gboolean
gst_acm_v4l2_buffer_pool_set_config (GstBufferPool * bpool, GstStructure * config)
{
	GstAcmV4l2BufferPool *pool = GST_ACM_V4L2_BUFFER_POOL (bpool);
	GstCaps *caps;
	guint size, min_buffers, max_buffers, num_buffers, copy_threshold;
	GstAllocator *allocator;
	GstAllocationParams params;
	struct v4l2_requestbuffers breq;
	
	GST_DEBUG_OBJECT (pool, "%s: - set config", TYPE_STR(pool->init_param.type));

	/* parse the config and keep around */
	if (!gst_buffer_pool_config_get_params (config, &caps, &size, &min_buffers,
											&max_buffers)) {
		goto wrong_config;
	}
	
	if (!gst_buffer_pool_config_get_allocator (config, &allocator, &params)) {
		goto wrong_config;
	}
	
	GST_DEBUG_OBJECT (pool, "%s: - config %" GST_PTR_FORMAT,
					 TYPE_STR(pool->init_param.type), config);

	switch (pool->init_param.mode) {
	case GST_ACM_V4L2_IO_RW:
		/* we preallocate 1 buffer, this value also instructs the latency
		 * calculation to have 1 frame latency max */
		num_buffers = 1;
		copy_threshold = 0;
		break;
	case GST_ACM_V4L2_IO_MMAP:
	{
		/* request a reasonable number of buffers when no max specified. We will
		 * copy when we run out of buffers */
		if (max_buffers == 0)
			num_buffers = pool->init_param.init_num_buffers;
		else
			num_buffers = max_buffers;
		
		/* first, lets request buffers, and see how many we can get: */
		GST_DEBUG_OBJECT (pool, "%s: - starting, requesting %d MMAP buffers",
						  TYPE_STR(pool->init_param.type), num_buffers);
		
		memset (&breq, 0, sizeof (struct v4l2_requestbuffers));
		breq.type = pool->init_param.type;
		breq.count = num_buffers;
		breq.memory = V4L2_MEMORY_MMAP;
		
		GST_INFO_OBJECT (pool, "%s: - VIDIOC_REQBUFS. count:%u",
			TYPE_STR(pool->init_param.type), num_buffers);
		if (gst_acm_v4l2_ioctl (pool->init_param.video_fd, VIDIOC_REQBUFS, &breq) < 0)
			goto reqbufs_failed;
		
		GST_DEBUG_OBJECT (pool, " count:  %u", breq.count);
		GST_DEBUG_OBJECT (pool, " type:   %d", breq.type);
		GST_DEBUG_OBJECT (pool, " memory: %d", breq.memory);
		
		if (breq.count < GST_ACM_V4L2_MIN_BUFFERS)
			goto no_buffers;
		
		if (num_buffers != breq.count) {
			GST_WARNING_OBJECT (pool, "%s: - USING %u BUFFERS INSTED",
				TYPE_STR(pool->init_param.type), breq.count);
			num_buffers = breq.count;
		}
		/* update min buffers with the amount of buffers we just reserved. We need
		 * to configure this value in the bufferpool so that the default start
		 * implementation calls our allocate function */
		min_buffers = breq.count;
		
		if (max_buffers == 0 || num_buffers < max_buffers) {
			/* if we are asked to provide more buffers than we have allocated, start
			 * copying buffers when we only have 2 buffers left in the pool */
			copy_threshold = 2;
		}
		else {
			/* we are certain that we have enough buffers so we don't need to
			 * copy */
			copy_threshold = 0;
		}
		break;
	}
	case GST_ACM_V4L2_IO_DMABUF:
	{
		/* request a reasonable number of buffers when no max specified. We will
		 * copy when we run out of buffers */
		if (max_buffers == 0)
			num_buffers = pool->init_param.init_num_buffers;
		else
			num_buffers = max_buffers;
		
		/* first, lets request buffers, and see how many we can get: */
		GST_DEBUG_OBJECT (pool, "%s: - starting, requesting %d MMAP buffers",
						  TYPE_STR(pool->init_param.type), num_buffers);
		
		memset (&breq, 0, sizeof (struct v4l2_requestbuffers));
		breq.type = pool->init_param.type;
		breq.count = num_buffers;
		breq.memory = V4L2_MEMORY_DMABUF;
		
		GST_INFO_OBJECT (pool, "%s: - VIDIOC_REQBUFS. count:%u",
			TYPE_STR(pool->init_param.type), num_buffers);
		if (gst_acm_v4l2_ioctl (pool->init_param.video_fd, VIDIOC_REQBUFS, &breq) < 0) {
			goto reqbufs_failed;
		}
		
		GST_DEBUG_OBJECT (pool, " count:  %u", breq.count);
		GST_DEBUG_OBJECT (pool, " type:   %d", breq.type);
		GST_DEBUG_OBJECT (pool, " memory: %d", breq.memory);
		
		if (breq.count < GST_ACM_V4L2_MIN_BUFFERS) {
			goto no_buffers;
		}
		
		if (num_buffers != breq.count) {
			GST_WARNING_OBJECT (pool, "%s: - USING %u BUFFERS INSTED",
				TYPE_STR(pool->init_param.type), breq.count);
			num_buffers = breq.count;
		}
		/* update min buffers with the amount of buffers we just reserved. We need
		 * to configure this value in the bufferpool so that the default start
		 * implementation calls our allocate function */
		min_buffers = breq.count;
		
		if (max_buffers == 0 || num_buffers < max_buffers) {
			/* if we are asked to provide more buffers than we have allocated, start
			 * copying buffers when we only have 2 buffers left in the pool */
			copy_threshold = 2;
		}
		else {
			/* we are certain that we have enough buffers so we don't need to
			 * copy */
			copy_threshold = 0;
		}
		break;
	}
	default:
		num_buffers = 0;
		copy_threshold = 0;
		g_assert_not_reached ();
		break;
	}
	
	pool->size = size;
	pool->num_buffers = num_buffers;
	pool->copy_threshold = copy_threshold;
	if (pool->allocator)
		gst_object_unref (pool->allocator);
	if ((pool->allocator = allocator))
		gst_object_ref (allocator);
	pool->params = params;
	
	gst_buffer_pool_config_set_params (config, caps, size, min_buffers,
									   max_buffers);
	
	return GST_BUFFER_POOL_CLASS (parent_class)->set_config (bpool, config);
	
	/* ERRORS */
wrong_config:
	{
		GST_ERROR_OBJECT (pool,
			"%s: - invalid config %" GST_PTR_FORMAT,
			TYPE_STR(pool->init_param.type), config);
		return FALSE;
	}
reqbufs_failed:
	{
		GST_ERROR_OBJECT (pool,
			"%s: - error requesting %d buffers: %s",
			TYPE_STR(pool->init_param.type), num_buffers, g_strerror (errno));
		return FALSE;
	}
no_buffers:
	{
		GST_ERROR_OBJECT (pool,
			"%s: - we received %d from device, we want at least %d",
			TYPE_STR(pool->init_param.type), breq.count, GST_ACM_V4L2_MIN_BUFFERS);
		return FALSE;
	}
}

static gboolean
gst_acm_v4l2_buffer_pool_start (GstBufferPool * bpool)
{
	GstAcmV4l2BufferPool *pool = GST_ACM_V4L2_BUFFER_POOL (bpool);
	
	GST_DEBUG_OBJECT (bpool, "%s: - gst_acm_v4l2_buffer_pool_start()",
					  TYPE_STR(pool->init_param.type));
	
	pool->buffers = g_new0 (GstBuffer *, pool->num_buffers);
	pool->num_allocated = 0;
	
	/* now, allocate the buffers: */
	if (!GST_BUFFER_POOL_CLASS (parent_class)->start (bpool)) {
		goto start_failed;
	}

///  gst_poll_set_flushing (pool->poll, FALSE);

	return TRUE;
	
	/* ERRORS */
start_failed:
	{
		GST_ERROR_OBJECT (pool,
			"%s: - failed to start", TYPE_STR(pool->init_param.type));
		return FALSE;
	}
}

static gboolean
gst_acm_v4l2_buffer_pool_stop (GstBufferPool * bpool)
{
	gboolean ret;
	GstAcmV4l2BufferPool *pool = GST_ACM_V4L2_BUFFER_POOL (bpool);
	guint n;
	
	GST_DEBUG_OBJECT (pool, "%s: - stopping pool", TYPE_STR(pool->init_param.type));

///  gst_poll_set_flushing (pool->poll, TRUE);

	/* first free the buffers in the queue */
	ret = GST_BUFFER_POOL_CLASS (parent_class)->stop (bpool);
	
	/* then free the remaining buffers */
	for (n = 0; n < pool->num_buffers; n++) {
		if (pool->buffers[n]) {
			gst_acm_v4l2_buffer_pool_free_buffer (bpool, pool->buffers[n]);
		}
	}
	pool->num_queued = 0;
	g_free (pool->buffers);
	pool->buffers = NULL;
	
	return ret;
}

GstFlowReturn
gst_acm_v4l2_buffer_pool_qbuf (GstAcmV4l2BufferPool * pool, GstBuffer * buf, gsize size)
{
	GstAcmV4l2Meta *meta;

	GST_DEBUG_OBJECT (pool, "%s: - enqueue buffer %p",
					  TYPE_STR(pool->init_param.type), buf);

	meta = GST_ACM_V4L2_META_GET (buf);
	if (NULL == meta) {
		GST_ERROR_OBJECT (pool, "%s: - meta is NULL", TYPE_STR(pool->init_param.type));

		return GST_FLOW_ERROR;
	}

#if 0	/* for debug	*/
	GST_DEBUG_OBJECT (pool, "%s: - enqueue buffer %p, index:%d, queued:%d, flags:%08x",
					  TYPE_STR(pool->init_param.type), buf, meta->vbuffer.index,
					  pool->num_queued, meta->vbuffer.flags);
#endif

	if (NULL != pool->buffers[meta->vbuffer.index]) {
		goto already_queued;
	}

	/* 入力データサイズを設定		*/
	meta->vbuffer.bytesused = size;

	GST_DEBUG_OBJECT (pool, "%s: - VIDIOC_QBUF - size:%d",
					  TYPE_STR(pool->init_param.type), meta->vbuffer.bytesused);
	if (gst_acm_v4l2_ioctl (pool->init_param.video_fd, VIDIOC_QBUF, &(meta->vbuffer)) < 0) {
#if USE_GST_FLOW_DQBUF_EAGAIN
		if (EAGAIN == errno) {
#if 0
			GST_WARNING_OBJECT (pool, "%s: - VIDIOC_QBUF  : EAGAIN",
								TYPE_STR(pool->init_param.type));
#endif
			return GST_FLOW_DQBUF_EAGAIN;
		}
#endif
		goto queue_failed;
	}
	GST_DEBUG_OBJECT (pool, "%s: - VIDIOC_QBUF - END", TYPE_STR(pool->init_param.type));

#if 0	/* for debug (video ouput)	*/
	if (V4L2_BUF_TYPE_VIDEO_CAPTURE == pool->init_param.type
		&& NUM_FB_DMABUF == pool->num_buffers /* H264Dec */ ) {
		GstAcmDmabufMeta *dmabufmeta = NULL;

		dmabufmeta = gst_buffer_get_acm_dmabuf_meta (buf);
		if (dmabufmeta) {
			GST_INFO_OBJECT (pool, "VIDIOC_QBUF : %d, %d - meta(%d, %d)",
				meta->vbuffer.index, meta->vbuffer.m.fd,
				dmabufmeta->index, dmabufmeta->fd);
		}
		else {
			GST_INFO_OBJECT (pool, "VIDIOC_QBUF   : index=%d, size=%d",
							 meta->vbuffer.index, gst_buffer_get_size(buf));
		}
	}
#endif

	pool->buffers[meta->vbuffer.index] = buf;
	pool->num_queued++;
	
	return GST_FLOW_OK;

	/* ERRORS */
already_queued:
	{
		GST_WARNING_OBJECT (pool,
			"%s: - the buffer was already queued",
			TYPE_STR(pool->init_param.type));
		return GST_FLOW_ERROR;
	}
queue_failed:
	{
		GST_ERROR_OBJECT (pool,
			"%s: - could not queue a buffer %d (%s)",
			TYPE_STR(pool->init_param.type), errno, g_strerror (errno));
		return GST_FLOW_ERROR;
	}
}

/* select() の代わりに、VIDIOC_DQBUF 可能かどうかをチェックする	*/
gboolean
gst_acm_v4l2_buffer_pool_is_ready_to_dqbuf(GstAcmV4l2BufferPool * pool)
{
	struct v4l2_buffer vbuffer;
	memset(&vbuffer, 0, sizeof(struct v4l2_buffer));
	vbuffer.index	= pool->next_qbuf_index;
	vbuffer.type	= pool->init_param.type;
	switch (pool->init_param.mode) {
	case GST_ACM_V4L2_IO_RW:
		break;
	case GST_ACM_V4L2_IO_MMAP:
		vbuffer.memory = V4L2_MEMORY_MMAP;
		break;
	case GST_ACM_V4L2_IO_DMABUF:
		vbuffer.memory = V4L2_MEMORY_DMABUF;
		break;
	default:
		g_assert_not_reached ();
		break;
	}
	if (gst_acm_v4l2_ioctl (pool->init_param.video_fd,
					VIDIOC_QUERYBUF, &vbuffer) < 0) {
		goto querybuf_failed;
	}
#if DBG_LOG_DQBUF
	GST_INFO_OBJECT (pool, "  index:     %u", vbuffer.index);
//	GST_INFO_OBJECT (pool, "  type:      %d", vbuffer.type);
	GST_INFO_OBJECT (pool, "  bytesused: %u", vbuffer.bytesused);
	GST_INFO_OBJECT (pool, "  flags:     %08x", vbuffer.flags);
	if (V4L2_BUF_FLAG_QUEUED == (vbuffer.flags & V4L2_BUF_FLAG_QUEUED)) {
		GST_INFO_OBJECT (pool, "  V4L2_BUF_FLAG_QUEUED");
	}
	if (V4L2_BUF_FLAG_DONE == (vbuffer.flags & V4L2_BUF_FLAG_DONE)) {
		GST_INFO_OBJECT (pool, "  V4L2_BUF_FLAG_DONE");
	}
//	GST_INFO_OBJECT (pool, "  field:     %d", vbuffer.field);
//	GST_INFO_OBJECT (pool, "  memory:    %d", vbuffer.memory);
	GST_INFO_OBJECT (pool, "  length:    %u", vbuffer.length);
#endif
	if (V4L2_BUF_FLAG_DONE == (vbuffer.flags & V4L2_BUF_FLAG_DONE)) {
#if DBG_LOG_DQBUF
		GST_INFO_OBJECT (pool, "%s: - idx:%d is ready",
			TYPE_STR(pool->init_param.type), pool->next_qbuf_index);
#endif
		return TRUE;
	}
#if DBG_LOG_DQBUF
	else {
		GST_INFO_OBJECT (pool, "%s: - idx:%d is not ready",
			TYPE_STR(pool->init_param.type), pool->next_qbuf_index);
	}
#endif

	return FALSE;
	
querybuf_failed:
	{
		GST_ERROR_OBJECT (pool,
			"%s: - Failed QUERYBUF: %s", TYPE_STR(pool->init_param.type), g_strerror (errno));
		return FALSE;
	}
}

GstFlowReturn
gst_acm_v4l2_buffer_pool_dqbuf (GstAcmV4l2BufferPool * pool, GstBuffer ** buffer)
{
	return gst_acm_v4l2_buffer_pool_dqbuf_ex(pool, buffer, NULL);
}

GstFlowReturn
gst_acm_v4l2_buffer_pool_dqbuf_ex (GstAcmV4l2BufferPool * pool, GstBuffer ** buffer,
							   guint32* bytesused)
{
//	GstFlowReturn res;
	GstBuffer *outbuf;
	struct v4l2_buffer vbuffer;
//	GstClockTime timestamp;

	memset (&vbuffer, 0x00, sizeof (vbuffer));
	vbuffer.type = pool->init_param.type;
    switch (pool->init_param.mode) {
	case GST_ACM_V4L2_IO_RW:
		break;
	case GST_ACM_V4L2_IO_MMAP:
		vbuffer.memory = V4L2_MEMORY_MMAP;
		break;
	case GST_ACM_V4L2_IO_DMABUF:
		vbuffer.memory = V4L2_MEMORY_DMABUF;
		break;
	default:
		g_assert_not_reached ();
		break;
    }
	
	GST_DEBUG_OBJECT (pool, "%s: - VIDIOC_DQBUF", TYPE_STR(pool->init_param.type));
	if (gst_acm_v4l2_ioctl (pool->init_param.video_fd, VIDIOC_DQBUF, &vbuffer) < 0) {
#if USE_GST_FLOW_DQBUF_EAGAIN
		if (EAGAIN == errno) {
#if 0
			GST_WARNING_OBJECT (pool, "%s: - VIDIOC_DQBUF  : EAGAIN",
							 TYPE_STR(pool->init_param.type));
#endif
			return GST_FLOW_DQBUF_EAGAIN;
		}
#endif
		goto error;
	}

#if 0	/* for debug	*/
	if (V4L2_BUF_TYPE_VIDEO_CAPTURE == pool->init_param.type) {
		GST_INFO_OBJECT (pool, "DQBUF  : index=%d, reserved:%d, sequence:%d, bytesused:%d",
			vbuffer.index, vbuffer.reserved, vbuffer.sequence, vbuffer.bytesused);
	}
#endif

	/* get our GstBuffer with that index from the pool, if the buffer was
	 * outstanding we have a serious problem.
	 */
#if DBG_LOG_DQBUF
	GST_INFO_OBJECT (pool, "%s: - VIDIOC_DQBUFed : %d",
					 TYPE_STR(pool->init_param.type), vbuffer.index);
#endif
	outbuf = pool->buffers[vbuffer.index];
	if (outbuf == NULL) {
		goto no_buffer;
	}

	/* copy meta info 	*/
	if (V4L2_BUF_TYPE_VIDEO_CAPTURE == pool->init_param.type) {
		GstAcmV4l2Meta *meta;
		
		meta = GST_ACM_V4L2_META_GET (outbuf);
		g_assert (NULL != meta);
		memcpy(&(meta->vbuffer), &vbuffer, sizeof(struct v4l2_buffer));
	}

	/* mark the buffer outstanding */
	pool->buffers[vbuffer.index] = NULL;
	pool->num_queued--;

//	timestamp = GST_TIMEVAL_TO_TIME (vbuffer.timestamp);
#if 0
	GST_DEBUG_OBJECT (pool,
		"%s: - dequeued buffer %p seq:%d (ix=%d), used %d, flags %08x, ts %"
		GST_TIME_FORMAT ", pool-queued=%d, buffer=%p",
		TYPE_STR(pool->init_param.type), outbuf, vbuffer.sequence,
		vbuffer.index, vbuffer.bytesused, vbuffer.flags,
		GST_TIME_ARGS (timestamp), pool->num_queued, outbuf);
#else
	GST_DEBUG_OBJECT (pool,
		"%s: - dequeued buffer %p type:%d seq:%d (ix=%d), used %d, flags %08x, pool-queued=%d, buffer=%p",
		TYPE_STR(pool->init_param.type), outbuf,
		vbuffer.type, vbuffer.sequence,
		vbuffer.index, vbuffer.bytesused, vbuffer.flags,
		pool->num_queued, outbuf);
#endif

#if 0
	/* set top/bottom field first if v4l2_buffer has the information */
	if (vbuffer.field == V4L2_FIELD_INTERLACED_TB) {
		GST_BUFFER_FLAG_SET (outbuf, GST_VIDEO_BUFFER_FLAG_TFF);
	}
	if (vbuffer.field == V4L2_FIELD_INTERLACED_BT) {
		GST_BUFFER_FLAG_UNSET (outbuf, GST_VIDEO_BUFFER_FLAG_TFF);
	}
#endif
	/* this can change at every frame, esp. with jpeg */
	if (pool->init_param.type == V4L2_BUF_TYPE_VIDEO_CAPTURE) {
		if (GST_ACM_V4L2_IO_DMABUF == pool->init_param.mode) {
			/* バッファ自体は使わないので、何もしない	*/
		}
		else {
			/* デコードされたデータサイズにリサイズ	*/
//			GST_INFO_OBJECT (pool, "### %s: - buf size:%d, bytesused:%d",
//				TYPE_STR(pool->init_param.type), gst_buffer_get_size(outbuf), vbuffer.bytesused);
			gst_buffer_resize (outbuf, 0, vbuffer.bytesused);
		}
		
#if 0	/* for debug	*/
		{
			GstMapInfo map;
			gint i;
			gboolean isAllZero = TRUE;
			gst_buffer_map (outbuf, &map, GST_MAP_READ);
			for (i = 0; i < map.size; i++) {
				if (0 != map.data[i]){
					GST_INFO_OBJECT (pool, "%s: - idx:%d NON ZERO",
									 TYPE_STR(pool->init_param.type), vbuffer.index);
					isAllZero = FALSE;
					break;
				}
			}
			if (isAllZero) {
				GST_ERROR_OBJECT (pool, "%s: - idx:%d ALL ZERO",
								 TYPE_STR(pool->init_param.type), vbuffer.index);
			}
			gst_buffer_unmap (outbuf, &map);
		}
#endif
	}
	else {
		if (GST_ACM_V4L2_IO_DMABUF == pool->init_param.mode) {
			/* バッファ自体は使わないので、何もしない	*/
		}
		else {
			/* 入力データサイズから、バッファの最大サイズに戻す	*/
			gst_buffer_resize (outbuf, 0, vbuffer.length);
		}
	}

//	GST_BUFFER_TIMESTAMP (outbuf) = timestamp;
	
#if 0	/* for debug	*/
	if (V4L2_BUF_TYPE_VIDEO_CAPTURE == pool->init_param.type) {
		GstAcmV4l2Meta *meta;
		GstAcmDmabufMeta *dmabufmeta = NULL;

		meta = GST_ACM_V4L2_META_GET (outbuf);
		if (NULL == meta) {
			GST_ERROR_OBJECT (pool, "%s: - meta is NULL!",
							 TYPE_STR(pool->init_param.type));
			return GST_FLOW_ERROR;
		}

		dmabufmeta = gst_buffer_get_acm_dmabuf_meta (outbuf);
		if (dmabufmeta) {
			GST_INFO_OBJECT (pool, "VIDIOC_DQBUF : %d, %d - meta(%d, %d)",
							 meta->vbuffer.index, meta->vbuffer.m.fd,
							 dmabufmeta->index, dmabufmeta->fd);
		}
		else {
			GST_INFO_OBJECT (pool, "VIDIOC_DQBUF   : index=%d, size=%d",
							 meta->vbuffer.index, gst_buffer_get_size(outbuf));
		}
	}
#endif

	*buffer = outbuf;
	if (bytesused) {
		*bytesused = vbuffer.bytesused;
	}
	
	pool->next_qbuf_index++;
	if (pool->next_qbuf_index >= pool->num_allocated) {
		pool->next_qbuf_index = 0;
	}
#if DBG_LOG_DQBUF
	GST_INFO_OBJECT (pool, "%s: - next qbuf index : %d",
					 TYPE_STR(pool->init_param.type), pool->next_qbuf_index);
#endif
	return GST_FLOW_OK;
	
	/* ERRORS */
error:
	{
		GST_WARNING_OBJECT (pool,
			"%s: - problem dequeuing frame %d (ix=%d), pool-ct=%d, buf.flags=%d",
			TYPE_STR(pool->init_param.type), vbuffer.sequence, vbuffer.index,
			GST_MINI_OBJECT_REFCOUNT (pool), vbuffer.flags);
		
		switch (errno) {
		case EAGAIN:
			GST_WARNING_OBJECT (pool,
				"%s: - Non-blocking I/O has been selected using O_NONBLOCK and"
				" no buffer was in the outgoing queue. device",
				TYPE_STR(pool->init_param.type));
			break;
		case EINVAL:
			GST_ERROR_OBJECT (pool,
				"%s: - The buffer type is not supported, or the index is out of bounds, "
				"or no buffers have been allocated yet, or the userptr "
				"or length are invalid. device",
				TYPE_STR(pool->init_param.type));
			break;
		case ENOMEM:
			GST_ERROR_OBJECT (pool,
				"%s: - insufficient memory to enqueue a user pointer buffer",
				TYPE_STR(pool->init_param.type));
			break;
		case EIO:
			GST_INFO_OBJECT (pool,
				"%s: - VIDIOC_DQBUF failed due to an internal error."
				" Can also indicate temporary problems like signal loss."
				" Note the driver might dequeue an (empty) buffer despite"
				" returning an error, or even stop capturing.",
				TYPE_STR(pool->init_param.type));
			/* have we de-queued a buffer ? */
			if (!(vbuffer.flags & (V4L2_BUF_FLAG_QUEUED | V4L2_BUF_FLAG_DONE))) {
				GST_DEBUG_OBJECT (pool, "%s: - reenqueing buffer", TYPE_STR(pool->init_param.type));
				/* FIXME ... should we do something here? */
			}
			break;
		case EINTR:
			GST_WARNING_OBJECT (pool,
				"%s: - could not sync on a buffer on device", TYPE_STR(pool->init_param.type));
			break;
		default:
			GST_WARNING_OBJECT (pool,
				"%s: - Grabbing frame got interrupted on device unexpectedly. %d: %s.",
				TYPE_STR(pool->init_param.type), errno, g_strerror (errno));
			break;
		}
		return GST_FLOW_ERROR;
	}
no_buffer:
	{
		GST_ERROR_OBJECT (pool,
			"%s: - No free buffer found in the pool at index %d.",
			TYPE_STR(pool->init_param.type), vbuffer.index);
		return GST_FLOW_ERROR;
	}
}

GstFlowReturn
gst_acm_v4l2_buffer_pool_acquire_buffer (GstBufferPool * bpool, GstBuffer ** buffer,
    GstBufferPoolAcquireParams * params)
{
	GstFlowReturn ret;
	GstAcmV4l2BufferPool *pool = GST_ACM_V4L2_BUFFER_POOL (bpool);
	
	GST_DEBUG_OBJECT (pool, "%s: - ACQUIRE", TYPE_STR(pool->init_param.type));

	/* GstBufferPool::priv::outstanding をインクリメントするためのダミー呼び出し用。
	 * pool の deactivate の際、outstanding == 0 でないと、解放および unmap されないため
	 */
	if (NULL != params
		&& GST_BUFFER_POOL_ACQUIRE_FLAG_LAST == params->flags) {
		GST_DEBUG_OBJECT (pool, "%s: - do nothing (FLAG_LAST)", TYPE_STR(pool->init_param.type));

		return GST_FLOW_OK;
	}
	
	if (GST_BUFFER_POOL_IS_FLUSHING (bpool))
		goto flushing;

	switch (pool->init_param.type) {
	case V4L2_BUF_TYPE_VIDEO_CAPTURE:
		/* capture, This function should return a buffer with new captured data */
		switch (pool->init_param.mode) {
		case GST_ACM_V4L2_IO_RW:
			/* take empty buffer from the pool */
			ret = GST_BUFFER_POOL_CLASS (parent_class)->acquire_buffer (
					bpool, buffer, params);
			break;
			
		case GST_ACM_V4L2_IO_MMAP:
#if 0
			/* just dequeue a buffer, we basically use the queue of v4l2 as the
			 * storage for our buffers. This function does poll first so we can
			 * interrupt it fine. */
			ret = gst_acm_v4l2_buffer_pool_dqbuf (pool, buffer);
			if (G_UNLIKELY (ret != GST_FLOW_OK))
				goto done;
			
			/* start copying buffers when we are running low on buffers */
			if (pool->num_queued < pool->copy_threshold) {
				GstBuffer *copy;
				
				/* copy the memory */
				copy = gst_buffer_copy (*buffer);
				GST_DEBUG_OBJECT (pool, "%s: - copy buffer %p->%p",
								TYPE_STR(pool->init_param.type), *buffer, copy);
				
				/* and requeue so that we can continue capturing */
				ret = gst_acm_v4l2_buffer_pool_qbuf (pool, *buffer);
				*buffer = copy;
			}
#else
			/* get an empty buffer newly */
			ret = GST_BUFFER_POOL_CLASS (parent_class)->acquire_buffer (
					bpool, buffer, params);
#endif
			break;
			
		case GST_ACM_V4L2_IO_DMABUF:
			/* get an empty buffer newly */
			ret = GST_BUFFER_POOL_CLASS (parent_class)->acquire_buffer (
					bpool, buffer, params);
			break;
		default:
			ret = GST_FLOW_ERROR;
			g_assert_not_reached ();
			break;
		}
		break;
		
	case V4L2_BUF_TYPE_VIDEO_OUTPUT:
		/* playback, This function should return an empty buffer */
		switch (pool->init_param.mode) {
		case GST_ACM_V4L2_IO_RW:
			/* get an empty buffer */
			ret = GST_BUFFER_POOL_CLASS (parent_class)->acquire_buffer (
					bpool, buffer, params);
			break;
			
		case GST_ACM_V4L2_IO_MMAP:
			/* get a free unqueued buffer */
			ret = GST_BUFFER_POOL_CLASS (parent_class)->acquire_buffer (
					bpool, buffer, params);
			break;
			
		case GST_ACM_V4L2_IO_DMABUF:
			/* get a free unqueued buffer */
			ret = GST_BUFFER_POOL_CLASS (parent_class)->acquire_buffer (
					bpool, buffer, params);
			break;

		default:
			ret = GST_FLOW_ERROR;
			g_assert_not_reached ();
			break;
		}
		break;
		
	default:
		ret = GST_FLOW_ERROR;
		g_assert_not_reached ();
		break;
	}
#if 0
done:
#endif
	return ret;
	
	/* ERRORS */
flushing:
	{
		GST_ERROR_OBJECT (pool, "%s: - We are flushing", TYPE_STR(pool->init_param.type));
		return GST_FLOW_FLUSHING;
	}
}

static void
gst_acm_v4l2_buffer_pool_release_buffer (GstBufferPool * bpool, GstBuffer * buffer)
{
	GstAcmV4l2BufferPool *pool = GST_ACM_V4L2_BUFFER_POOL (bpool);
	
	switch (pool->init_param.type) {
	case V4L2_BUF_TYPE_VIDEO_CAPTURE:
		GST_DEBUG_OBJECT (pool, "%s: - RELEASE BUFFER %p",
			TYPE_STR(pool->init_param.type), buffer);
		/* capture, put the buffer back in the queue so that we can refill it
		 * later. */
		switch (pool->init_param.mode) {
		case GST_ACM_V4L2_IO_RW:
			/* release back in the pool */
			GST_BUFFER_POOL_CLASS (parent_class)->release_buffer (bpool, buffer);
			break;
			
		case GST_ACM_V4L2_IO_MMAP:
			/* queue back in the device */
			gst_acm_v4l2_buffer_pool_qbuf (pool, buffer, gst_buffer_get_size(buffer));
			break;
			
		case GST_ACM_V4L2_IO_DMABUF:
			/* queue back in the device */
			gst_acm_v4l2_buffer_pool_qbuf (pool, buffer, gst_buffer_get_size(buffer));
			break;
		default:
			g_assert_not_reached ();
			break;
		}
		break;
		
	case V4L2_BUF_TYPE_VIDEO_OUTPUT:
		switch (pool->init_param.mode) {
		case GST_ACM_V4L2_IO_RW:
			/* release back in the pool */
			GST_BUFFER_POOL_CLASS (parent_class)->release_buffer (bpool, buffer);
			break;
			
		case GST_ACM_V4L2_IO_MMAP:
		{
			GstAcmV4l2Meta *meta;
			
			meta = GST_ACM_V4L2_META_GET (buffer);
			g_assert (meta != NULL);
			
			if (pool->buffers[meta->vbuffer.index] == NULL) {
				GST_DEBUG_OBJECT (pool, "%s: - RELEASE BUFFER %p index:%d",
					TYPE_STR(pool->init_param.type), buffer, meta->vbuffer.index);

				GST_DEBUG_OBJECT (pool,
					"%s: - buffer not queued, putting on free list",
					TYPE_STR(pool->init_param.type));
				/* playback, put the buffer back in the queue to refill later. */
				GST_BUFFER_POOL_CLASS (parent_class)->release_buffer (
					bpool, buffer);
			}
			else {
				/* the buffer is queued in the device but maybe not played yet. We just
				 * leave it there and not make it available for future calls to acquire
				 * for now. The buffer will be dequeued and reused later. */
				GST_DEBUG_OBJECT (pool, "%s: - buffer is queued",
					TYPE_STR(pool->init_param.type));
			}
			break;
		}
			
		case GST_ACM_V4L2_IO_DMABUF:
		{
			GstAcmV4l2Meta *meta;
			
			meta = GST_ACM_V4L2_META_GET (buffer);
			g_assert (meta != NULL);
			
			if (pool->buffers[meta->vbuffer.index] == NULL) {
				GST_DEBUG_OBJECT (pool, "%s: - RELEASE BUFFER %p index:%d",
					TYPE_STR(pool->init_param.type), buffer, meta->vbuffer.index);

				GST_DEBUG_OBJECT (pool,
					"%s: - buffer not queued, putting on free list",
					TYPE_STR(pool->init_param.type));
				/* playback, put the buffer back in the queue to refill later. */
				GST_BUFFER_POOL_CLASS (parent_class)->release_buffer (
					bpool, buffer);
			}
			else {
				/* the buffer is queued in the device but maybe not played yet. We just
				 * leave it there and not make it available for future calls to acquire
				 * for now. The buffer will be dequeued and reused later. */
				GST_DEBUG_OBJECT (pool, "%s: - buffer is queued",
					TYPE_STR(pool->init_param.type));
			}
			break;
		}

		default:
			g_assert_not_reached ();
			break;
		}
		break;
		
	default:
		g_assert_not_reached ();
		break;
	}
}

static void
gst_acm_v4l2_buffer_pool_finalize (GObject * object)
{
	GstAcmV4l2BufferPool *pool = GST_ACM_V4L2_BUFFER_POOL (object);
	
	if (pool->init_param.video_fd >= 0)
		close (pool->init_param.video_fd);
	if (pool->allocator)
		gst_object_unref (pool->allocator);
	g_free (pool->buffers);
	
	G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
gst_acm_v4l2_buffer_pool_init (GstAcmV4l2BufferPool * pool)
{
}

static void
gst_acm_v4l2_buffer_pool_class_init (GstAcmV4l2BufferPoolClass * klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	GstBufferPoolClass *bufferpool_class = GST_BUFFER_POOL_CLASS (klass);
	
	object_class->finalize = gst_acm_v4l2_buffer_pool_finalize;
	
	bufferpool_class->start = gst_acm_v4l2_buffer_pool_start;
	bufferpool_class->stop = gst_acm_v4l2_buffer_pool_stop;
	bufferpool_class->set_config = gst_acm_v4l2_buffer_pool_set_config;
	bufferpool_class->alloc_buffer = gst_acm_v4l2_buffer_pool_alloc_buffer;
	bufferpool_class->acquire_buffer = gst_acm_v4l2_buffer_pool_acquire_buffer;
	bufferpool_class->release_buffer = gst_acm_v4l2_buffer_pool_release_buffer;
	bufferpool_class->free_buffer = gst_acm_v4l2_buffer_pool_free_buffer;

	GST_DEBUG_CATEGORY_INIT (acm_v4l2_debug, "acmv4l2", 0, "ACM V4L2 API calls");
}

/**
 * gst_acm_v4l2_buffer_pool_new:
 * @obj:  the v4l2 object owning the pool
 *
 * Construct a new buffer pool.
 *
 * Returns: the new pool, use gst_object_unref() to free resources
 */
GstAcmV4l2BufferPool *
gst_acm_v4l2_buffer_pool_new (GstAcmV4l2InitParam *param, GstCaps * caps)
{
	GstAcmV4l2BufferPool *pool = NULL;
	GstStructure *s;
	gint fd;
	gint i;
	
	fd = gst_acm_v4l2_dup (param->video_fd);
	if (fd < 0)
		goto dup_failed;
	
	pool = (GstAcmV4l2BufferPool *) g_object_new (GST_TYPE_ACM_V4L2_BUFFER_POOL, NULL);
	pool->init_param.video_fd = fd;
	pool->init_param.type = param->type;
	pool->init_param.mode = param->mode;
	pool->init_param.sizeimage = param->sizeimage;
	pool->init_param.init_num_buffers = param->init_num_buffers;
	pool->init_param.num_fb_dmabuf = param->num_fb_dmabuf;
	for (i = 0; i < param->num_fb_dmabuf; i++) {
		pool->init_param.fb_dmabuf_index[i] = param->fb_dmabuf_index[i];
		pool->init_param.fb_dmabuf_fd[i] = param->fb_dmabuf_fd[i];
	}
	pool->next_qbuf_index = 0;
	
	s = gst_buffer_pool_get_config (GST_BUFFER_POOL_CAST (pool));
	/**
	 * gst_buffer_pool_config_set_params:
	 * @config: a #GstBufferPool configuration
	 * @caps: caps for the buffers
	 * @size: the size of each buffer, not including prefix and padding
	 * @min_buffers: the minimum amount of buffers to allocate.
	 * @max_buffers: the maximum amount of buffers to allocate or 0 for unlimited.
	 *
	 * Configure @config with the given parameters.
	 */
	gst_buffer_pool_config_set_params (s, caps, pool->init_param.sizeimage,
									   DEFAULT_MIN_BUFFERS, DEFAULT_MAX_BUFFERS);
	gst_buffer_pool_set_config (GST_BUFFER_POOL_CAST (pool), s);
	
	return pool;
	
	/* ERRORS */
dup_failed:
	{
		GST_ERROR_OBJECT (pool,
			"%s: - failed to dup fd %d (%s)",
			TYPE_STR(pool->init_param.type), errno, g_strerror (errno));
		return NULL;
	}
}


/* VIDIOC_QUERYBUF により、各バッファの状態をログ出力	（デバッグ用途）	*/
void
gst_acm_v4l2_buffer_pool_log_buf_status(GstAcmV4l2BufferPool* pool)
{
	struct v4l2_buffer vbuffer;
	int index;
	int r = 0;
	gboolean isAllQueued = TRUE;
	gboolean isAllDone = TRUE;

	GST_INFO_OBJECT (pool, "BUF STATUS");
	for (index = 0; index < pool->num_allocated; index++) {
		memset(&vbuffer, 0, sizeof(struct v4l2_buffer));
		
		vbuffer.index	= index;
		vbuffer.type	= pool->init_param.type;
		switch (pool->init_param.mode) {
		case GST_ACM_V4L2_IO_RW:
			break;
		case GST_ACM_V4L2_IO_MMAP:
			vbuffer.memory = V4L2_MEMORY_MMAP;
			break;
		case GST_ACM_V4L2_IO_DMABUF:
			vbuffer.memory = V4L2_MEMORY_DMABUF;
			break;
		default:
			g_assert_not_reached ();
			break;
		}
		
//		GST_INFO_OBJECT (pool, "%s: - VIDIOC_QUERYBUF",
//						 TYPE_STR(pool->init_param.type));
		r = gst_acm_v4l2_ioctl (pool->init_param.video_fd, VIDIOC_QUERYBUF, &vbuffer);
		if (r < 0) {
			GST_ERROR_OBJECT (pool,
				"%s: - Failed QUERYBUF: %s",
				TYPE_STR(pool->init_param.type), g_strerror (errno));
		}

		GST_INFO_OBJECT (pool, "  index:     %u", vbuffer.index);
//		GST_INFO_OBJECT (pool, "  type:      %d", vbuffer.type);
//		GST_INFO_OBJECT (pool, "  bytesused: %u", vbuffer.bytesused);
		GST_INFO_OBJECT (pool, "  flags:     %08x", vbuffer.flags);
		if (V4L2_BUF_FLAG_QUEUED == (vbuffer.flags & V4L2_BUF_FLAG_QUEUED)) {
			GST_INFO_OBJECT (pool, "  V4L2_BUF_FLAG_QUEUED");
			isAllDone = FALSE;
		}
		if (V4L2_BUF_FLAG_DONE == (vbuffer.flags & V4L2_BUF_FLAG_DONE)) {
			GST_INFO_OBJECT (pool, "  V4L2_BUF_FLAG_DONE");
			isAllQueued = FALSE;
		}
		if (V4L2_BUF_FLAG_PREPARED == (vbuffer.flags & V4L2_BUF_FLAG_PREPARED)) {
			GST_INFO_OBJECT (pool, "  V4L2_BUF_FLAG_PREPARED");
		}
//		GST_INFO_OBJECT (pool, "  field:     %d", vbuffer.field);
//		GST_INFO_OBJECT (pool, "  memory:    %d", vbuffer.memory);
//		GST_INFO_OBJECT (pool, "  length:    %u", vbuffer.length);
	}

	if (isAllQueued) {
		GST_INFO_OBJECT (pool, "%s: NOT EXIST DONE STATE",
						 TYPE_STR(pool->init_param.type));
	}
	if (isAllDone) {
		GST_INFO_OBJECT (pool, "%s: ALL DONE STATE",
						 TYPE_STR(pool->init_param.type));
	}
}

/*
 * End of file
 */
