/* GStreamer
 *
 * Copyright (C) 2013 Atmark Techno, Inc.
 *
 * gstacm_debug.c - debug utility methods
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

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

#include "gstacm_debug.h"


void
dump_input_buf(GstBuffer *buffer)
{
	static int index = 0;
	gint i;
	GstMapInfo map_debug;
	char fileNameBuf[1024];
	sprintf(fileNameBuf, "in_%03d.data", ++index);

	FILE* fp = fopen(fileNameBuf, "w");
	if (fp) {
		gst_buffer_map (buffer, &map_debug, GST_MAP_READ);
		for (i = 0; i < map_debug.size; i++) {
#if 0
			fprintf(fp, "0x%02X, ", map_debug.data[i]);
			if((i+1) % 8 == 0)
				fprintf(fp, "\n");
#else
			fputc(map_debug.data[i], fp);
#endif
		}
		gst_buffer_unmap (buffer, &map_debug);
		fclose(fp);
	}
}

void
dump_output_buf(GstBuffer *buffer)
{
	static int index = 0;
	gint i;
	GstMapInfo map_debug;
	char fileNameBuf[1024];
	sprintf(fileNameBuf, "out_%03d.data", ++index);

	FILE* fp = fopen(fileNameBuf, "w");
	if (fp) {
		gst_buffer_map (buffer, &map_debug, GST_MAP_READ);
		for (i = 0; i < map_debug.size; i++) {
#if 0
			fprintf(fp, "0x%02X, ", map_debug.data[i]);
			if((i+1) % 8 == 0)
				fprintf(fp, "\n");
#else
			fputc(map_debug.data[i], fp);
#endif
		}
		gst_buffer_unmap (buffer, &map_debug);
		fclose(fp);
	}
}

void
parse_adts_header(GstBuffer *buffer)
{
	// see : http://wiki.multimedia.cx/index.php?title=ADTS
	GstMapInfo map;
	int version;
	int layer;
	gboolean bProtect;
	int profile;
	int sampleRateIndex;
	gboolean bPrivateBit;
	int channelConfig;
	gboolean bCopy;
	gboolean bHome;
	gboolean bCopyrightBit;
	gboolean bCopyrightStart;
	unsigned long frameSize = 0;
	unsigned long bufferFullness;
	int remainBlocks;

	gst_buffer_map (buffer, &map, GST_MAP_READ);

	// syncword
    if (0xFF != (map.data[0] & 0xFF)) {
		GST_ERROR ("Invalid syncword [0]: 0x%x", map.data[0]);
		goto out;
	}
    if (0xF0 != (map.data[1] & 0xF0)) {
		GST_ERROR ("Invalid syncword [1]: 0x%x", map.data[1]);
		goto out;
	}

    // MPEG Version : 0 for MPEG-4, 1 for MPEG-2
    version = 4 - ((map.data[1] >> 3) & 1) * 2;
	GST_INFO ("version: %ld", version);

    // layer
    layer = (map.data[1] >> 1) & 3;
    if (0 != layer) {
		GST_ERROR ("Invalid layer: 0x%x", layer);
		goto out;
	}

    // protection absent
    bProtect = map.data[1] & 1;
	GST_LOG ("bProtect: %d", bProtect);

    // profile
    profile = (map.data[2] >> 6) & 3;
	GST_INFO ("profile: %d", profile);

    // 	MPEG-4 Sampling Frequency Index
    sampleRateIndex = (map.data[2] >> 2) & 0x0F;
	GST_INFO ("sampleRateIndex: %d", sampleRateIndex);

    // private stream
    bPrivateBit = (map.data[2] >> 1) & 1;
	GST_LOG ("bPrivateBit: %d", bPrivateBit);

    // MPEG-4 Channel Configuration
    channelConfig = ((map.data[2] & 1) << 2) | ((map.data[3] >> 6) & 3);
	GST_INFO ("channelConfig: %d", channelConfig);

    // originality
    bCopy = (map.data[3] >> 5) & 1;
	GST_LOG ("bCopy: %d", bCopy);

    // home
    bHome = (map.data[3] >> 4) & 1;
	GST_LOG ("bHome: %d", bHome);

    // copyrighted stream
    bCopyrightBit = (map.data[3] >> 3) & 1;
	GST_LOG ("bCopyrightBit: %d", bCopyrightBit);

    // copyright start
    bCopyrightStart = (map.data[3] >> 2) & 1;
	GST_LOG ("bCopyrightStart: %d", bCopyrightStart);

    // frame length
    frameSize = ((((unsigned long)map.data[3]) << 11) & 0x1800)
		| ((((unsigned long)map.data[4]) << 3) & 0x7F8)
		| ((((unsigned long)map.data[5]) >> 5) & 0x007);
	GST_INFO ("frameSize: %lu", frameSize);

    // Buffer fullness
    bufferFullness = ((((unsigned long)map.data[5]) << 6) & 0x7c0)
		| ((((unsigned long)map.data[6]) >> 2) & 0x03F);
	GST_LOG ("bufferFullness: %lu", bufferFullness);

    // Number of AAC frames (RDBs) in ADTS frame minus 1
    remainBlocks = map.data[6] & 3;
	GST_LOG ("remainBlocks: %d", remainBlocks);

out:
	gst_buffer_unmap (buffer, &map);
}

/*
 * End of file
 */

