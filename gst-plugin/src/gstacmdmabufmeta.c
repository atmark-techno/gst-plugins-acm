/* GStreamer
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

/**
 * SECTION:gstacmdmabufmeta
 * @short_description: DMABUF address metadata
 */

#include <string.h>

#include "gstacmdmabufmeta.h"

static gboolean
acm_dmabuf_meta_init (GstMeta * meta, gpointer params, GstBuffer * buffer)
{
	GstAcmDmabufMeta *me = (GstAcmDmabufMeta *) meta;
	
	me->fd = -1;
	me->index = -1;

	return TRUE;
}

static gboolean
acm_dmabuf_meta_transform (GstBuffer * transbuf, GstMeta * meta,
    GstBuffer * buffer, GQuark type, gpointer data)
{
	GstAcmDmabufMeta *me = (GstAcmDmabufMeta *) meta;
	
	/* we always copy no matter what transform */
	gst_buffer_add_acm_dmabuf_meta (transbuf, me->fd, me->index);
	
	return TRUE;
}

static void
acm_dmabuf_meta_free (GstMeta * meta, GstBuffer * buffer)
{
	GstAcmDmabufMeta *me = (GstAcmDmabufMeta *) meta;
	
	me->fd = -1;
	me->index = -1;
}

GType
gst_acm_dmabuf_meta_api_get_type (void)
{
	static volatile GType type;
	static const gchar *tags[] = { "origin", NULL };
	
	if (g_once_init_enter (&type)) {
		GType _type = gst_meta_api_type_register ("GstAcmDmabufMetaAPI", tags);
		g_once_init_leave (&type, _type);
	}
	return type;
}

const GstMetaInfo *
gst_acm_dmabuf_meta_get_info (void)
{
	static const GstMetaInfo *meta_info = NULL;
	
	if (g_once_init_enter (&meta_info)) {
		const GstMetaInfo *mi = gst_meta_register (
			GST_ACM_DMABUF_META_API_TYPE, "GstAcmDmabufMeta",
			sizeof (GstAcmDmabufMeta),
			acm_dmabuf_meta_init,
			acm_dmabuf_meta_free,
			acm_dmabuf_meta_transform);
		g_once_init_leave (&meta_info, mi);
	}
	return meta_info;
}

GstAcmDmabufMeta *
gst_buffer_add_acm_dmabuf_meta (GstBuffer * buffer, int fd, int index)
{
	GstAcmDmabufMeta *meta;
	
	g_return_val_if_fail (GST_IS_BUFFER (buffer), NULL);
	
	meta = (GstAcmDmabufMeta *) gst_buffer_add_meta (buffer,
									GST_ACM_DMABUF_META_INFO, NULL);
	
	meta->fd = fd;
	meta->index = index;

	return meta;
}

/*
 * End of file
 */
