/* GStreamer
 * Copyright (C)  2005 Wim Taymans <wim@fluendo.com>
 * Copyright (C) 2013 Atmark Techno, Inc.
 *
 * gstalsasink.h: 
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


#ifndef __GST_ACMALSASINK_H__
#define __GST_ACMALSASINK_H__

#include <gst/gst.h>
#include "gstalsasink.h"

G_BEGIN_DECLS

#define GST_TYPE_ACM_ALSA_SINK            (gst_acmalsasink_get_type())
#define GST_ACM_ALSA_SINK(obj)            (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_ACM_ALSA_SINK,GstAcmAlsaSink))
#define GST_ACM_ALSA_SINK_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_ACM_ALSA_SINK,GstAcmAlsaSinkClass))
#define GST_IS_ACM_ALSA_SINK(obj)         (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_ACM_ALSA_SINK))
#define GST_IS_ACM_ALSA_SINK_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_ACM_ALSA_SINK))

typedef struct _GstAcmAlsaSink GstAcmAlsaSink;
typedef struct _GstAcmAlsaSinkClass GstAcmAlsaSinkClass;

/**
 * GstAcmAlsaSink:
 *
 * Opaque data structure
 */
struct _GstAcmAlsaSink {
  GstMyAlsaSink    sink;
};

struct _GstAcmAlsaSinkClass {
  GstMyAlsaSinkClass parent_class;
};

GType gst_acmalsasink_get_type(void);

G_END_DECLS

#endif /* __GST_ACMALSASINK_H__ */
