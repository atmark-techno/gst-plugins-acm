/* GStreamer
 *
 * Copyright (C) 2013 Atmark Techno, Inc.
 *
 * gstacm_debug.h - debug utility methods
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

#ifndef __GSTACM_DEBUG_H__
#define __GSTACM_DEBUG_H__

#include <gst/gst.h>


/* dump buffer to file		*/
void dump_input_buf(GstBuffer *buffer);
void dump_output_buf(GstBuffer *buffer);

/* parse AAC ADTS header	*/
void parse_adts_header(GstBuffer *buffer);

#endif /* __GSTACM_DEBUG_H__ */

/*
 * End of file
 */
