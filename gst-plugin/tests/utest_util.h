/* GStreamer
 *
 * Copyright (C) 2013 Atmark Techno, Inc.
 *
 * utest_util.h - unit test utility methods
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

#ifndef __UTEST_UTIL_H__
#define __UTEST_UTIL_H__

#include <gst/gst.h>

/* get data from file		*/
void get_data(char *file, size_t *size, void **p);

/* create audio buffer for specified caps */
GstBuffer *create_audio_buffer (GstCaps * caps);

/* create video buffer for specified caps */
GstBuffer *create_video_buffer (GstCaps * caps);


#endif /* __UTEST_UTIL_H__ */

/*
 * End of file
 */
