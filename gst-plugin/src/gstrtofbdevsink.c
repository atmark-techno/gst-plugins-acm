/* GStreamer fbdev plugin
 * Copyright (C) 2007 Sean D'Epagnier <sean@depagnier.com>
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <signal.h>
#include <string.h>
#include <sys/time.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdint.h>

#include <gst/video/video.h>

#include "gstrtofbdevsink.h"
#include "gstrtodmabufmeta.h"


/* デコーダ初期化パラメータのデフォルト値	*/
#define DEFAULT_FB_DEVICE		"/dev/fb0"
#define DEFAULT_USE_DMABUF		TRUE
#define DEFAULT_ENABLE_VSYNC	TRUE

/* デバッグログ出力フラグ		*/
#define DBG_LOG_RENDER				0
#define DBG_LOG_RENDER_SKIP			0

/* 描画時間の計測		*/
#define DBG_MEASURE_PERF			0
#if DBG_MEASURE_PERF
# define DBG_MEASURE_PERF_CHAIN		0
# define DBG_MEASURE_PERF_RENDER	0
#endif

static double
gettimeofday_sec()
{
	struct timeval t;
	
	gettimeofday(&t, NULL);
	return (double)t.tv_sec + (double)t.tv_usec * 1e-6;
}

struct _GstRtoFBDevSinkPrivate
{
	struct fb_dmabuf_export fb_dmabuf_exp[NUM_FB_DMABUF];
	
	/* 最後にレンダリングした、DMABUF インデックス		*/
	int last_show_fb_dmabuf_index;

	/* FBIOPAN_DISPLAY と FBIO_WAITFORVSYNC を呼び出した直後でも、HW 側で PAN が完了
	 * しておらず、QBUF したフレームバッファ領域に HW デコーダがデコード済みデータの書き出し
	 * を開始してしまい、結果ディスプレイ上でちらつきが起こるケースが生じる場合がある。
	 * これを防ぐため、PAN が終わったら、前回 PAN が終わったフレームバッファ領域を QBUF する
	 * ようにする。つまり、本 sink 側では、常に２つのフレームバッファ領域を保持しておく事になる。
	 */
	GstBuffer* prev_displaying_buf;

	/* ディスプレイ表示中のバッファは、次の表示を終えるまで、ref して
	 * 保持しておかないと、m2m デバイスに enqueue され、ディスプレイに表示中に、
	 * デコードデータを上書きされてしまい、画面が乱れてしまう
	 */
	GstBuffer* displaying_buf;

	/* フレームレート（秒）		*/
	double framerate_sec;

	/* 表示遅延許容時間（秒）	 */
	double lateness_sec;

	/* 前回表示時刻（秒）	*/
	double prev_display_time_sec;

	GstPadChainFunction base_chain;
};

GST_DEBUG_CATEGORY_STATIC (rtofbdevsink_debug);
#define GST_CAT_DEFAULT rtofbdevsink_debug

#define GST_RTOFBDEVSINK_GET_PRIVATE(obj)  \
	(G_TYPE_INSTANCE_GET_PRIVATE ((obj), GST_TYPE_RTOFBDEVSINK, \
		GstRtoFBDevSinkPrivate))

enum
{
	PROP_0,
	PROP_DEVICE,
	PROP_USE_DMABUF,
	PROP_ENABLE_VSYNC,
};

#define GST_FBDEV_TEMPLATE_CAPS_RGB \
	GST_VIDEO_CAPS_MAKE ("RGB") 

#define GST_FBDEV_TEMPLATE_CAPS_RGBx \
	GST_VIDEO_CAPS_MAKE ("RGBx")  

#define GST_FBDEV_TEMPLATE_CAPS_RGB16 \
	GST_VIDEO_CAPS_MAKE ("RGB16") 

#define GST_FBDEV_TEMPLATE_CAPS \
	GST_FBDEV_TEMPLATE_CAPS_RGB ";" \
	GST_FBDEV_TEMPLATE_CAPS_RGBx ";" \
	GST_FBDEV_TEMPLATE_CAPS_RGB16 

/* pad template caps for sink pads.	*/
static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE ("sink",
	GST_PAD_SINK,
	GST_PAD_ALWAYS,
	GST_STATIC_CAPS (GST_FBDEV_TEMPLATE_CAPS)
	);

static uint32_t
swapendian (uint32_t val)
{
	return (val & 0xff) << 24 | (val & 0xff00) << 8
			| (val & 0xff0000) >> 8 | (val & 0xff000000) >> 24;
}

static guint32
get_disp_refresh_rate(GstRtoFBDevSink *me, struct fb_var_screeninfo * var)
{
	guint32 pixclock, hfreq, htotal, vtotal;
	guint32 refresh;

#if 0	// for debug
	GST_INFO_OBJECT(me, "pixclock:%u", var->pixclock);
	GST_INFO_OBJECT(me, "xres:%u, right_margin:%u, hsync_len:%u, left_margin:%u",
		var->xres, var->right_margin, var->hsync_len, var->left_margin);
	GST_INFO_OBJECT(me, "yres:%u, lower_margin:%u, vsync_len:%u, upper_margin:%u",
		var->yres, var->lower_margin, var->vsync_len, var->upper_margin);
#endif

	if (! var->pixclock) {
		GST_WARNING_OBJECT(me, "pixclock is not set!");
		return 60;
	}

	pixclock = PICOS2KHZ(var->pixclock) * 1000;
	
	htotal = var->xres + var->right_margin + var->hsync_len + var->left_margin;
	vtotal = var->yres + var->lower_margin + var->vsync_len + var->upper_margin;
	
	if (var->vmode & FB_VMODE_INTERLACED) {
		vtotal /= 2;
	}
	if (var->vmode & FB_VMODE_DOUBLE) {
		vtotal *= 2;
	}
	
	hfreq = pixclock / htotal;
	refresh = hfreq / vtotal;

#if 0	// for debug
	GST_INFO_OBJECT(me, "pixclock:%u, htotal:%u, vtotal:%u, hfreq:%u, refresh:%u",
					 pixclock, htotal, vtotal, hfreq, refresh);
#endif

	GST_INFO_OBJECT(me, "display's refresh rate is %u Hz", refresh);

	return refresh;
}

static void gst_rto_fbdevsink_get_times (GstBaseSink * basesink,
    GstBuffer * buffer, GstClockTime * start, GstClockTime * end);

static GstCaps *gst_rto_fbdevsink_getcaps (GstBaseSink * bsink, GstCaps *filter);
static gboolean gst_rto_fbdevsink_setcaps (GstBaseSink * bsink, GstCaps * caps);

static gboolean gst_rto_fbdevsink_start (GstBaseSink * bsink);
static gboolean gst_rto_fbdevsink_stop (GstBaseSink * bsink);
static gboolean gst_rto_fbdevsink_query (GstBaseSink * sink, GstQuery * query);
static GstFlowReturn gst_rto_fbdevsink_chain (GstPad * pad,
	GstObject * parent, GstBuffer * buf);
static GstFlowReturn gst_rto_fbdevsink_preroll (GstBaseSink * bsink,
	GstBuffer * buff);
static GstFlowReturn gst_rto_fbdevsink_render (GstBaseSink * bsink,
	GstBuffer * buff);

static void gst_rto_fbdevsink_set_property (GObject * object,
    guint prop_id, const GValue * value, GParamSpec * pspec);
static void gst_rto_fbdevsink_get_property (GObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec);
static void gst_rto_fbdevsink_finalize (GObject * object);

static GstStateChangeReturn gst_rto_fbdevsink_change_state(GstElement * element,
	GstStateChange transition);



#define gst_rto_fbdevsink_parent_class parent_class
G_DEFINE_TYPE (GstRtoFBDevSink, gst_rto_fbdevsink, GST_TYPE_BASE_SINK);

static gboolean
change_fb_varinfo_bpp(GstRtoFBDevSink *me, gint new_bpp)
{
	if (me->varinfo.bits_per_pixel != new_bpp) {
		GST_INFO_OBJECT (me, "varinfo.bits_per_pixel(%d) != %d, forcing...",
						 me->varinfo.bits_per_pixel, new_bpp);
		me->varinfo.bits_per_pixel = new_bpp;
		if (0 != ioctl(me->fd, FBIOPUT_VSCREENINFO, &(me->varinfo))) {
			goto fbioput_failed;
		}
		
		/* get the fixed screen info again */
		if (0 != ioctl (me->fd, FBIOGET_FSCREENINFO, &me->fixinfo)) {
			goto fbioget_failed;
		}
		GST_INFO_OBJECT(me,
			"FBIOGET_FSCREENINFO - smem_start:%p, smem_len:%u, line_length:%u",
			me->fixinfo.smem_start, me->fixinfo.smem_len, me->fixinfo.line_length);
		GST_INFO_OBJECT(me,
			"FBIOGET_VSCREENINFO - xres:%u, yres:%u, xres_v:%u, yres_v:%u, xoffset:%u, yoffset:%u, bits_per_pixel:%u",
			me->varinfo.xres, me->varinfo.yres,
			me->varinfo.xres_virtual, me->varinfo.yres_virtual,
			me->varinfo.xoffset, me->varinfo.yoffset,
			me->varinfo.bits_per_pixel);

		me->is_changed_fb_varinfo = TRUE;
	}
	
	return TRUE;
	
fbioput_failed:
	{
		GST_ELEMENT_ERROR (me, RESOURCE, SETTINGS, (NULL),
			("error with ioctl(FBIOPUT) %d (%s)", errno, g_strerror (errno)));
		return FALSE;
	}
fbioget_failed:
	{
		GST_ELEMENT_ERROR (me, RESOURCE, SETTINGS, (NULL),
			("error with ioctl(FBIOGET) %d (%s)", errno, g_strerror (errno)));
		return FALSE;
	}
}

static gboolean
do_blank_screen(GstRtoFBDevSink *me)
{
	gboolean ret = TRUE;
	int vsyncArg = 0;
	int r;

	GST_INFO_OBJECT (me, "do blank screen");

	/* 先頭のフレームバッファに PAN する */
	me->varinfo.yoffset = 0;
	r = ioctl (me->fd, FBIOPAN_DISPLAY, &(me->varinfo));
	if (0 != r) {
		goto fbiopan_display_failed;
	}

	/* ブランクスクリーンにする	*/
#if 0	// 2013-07-23 : patch
	r = ioctl (me->fd, FBIOBLANK, FB_BLANK_UNBLANK);
	if (0 != r) {
		goto fbioblank_failed;
	}
	r = ioctl (me->fd, FBIOBLANK, FB_BLANK_NORMAL);
	if (0 != r) {
		goto fbioblank_failed;
	}
	r = ioctl (me->fd, FBIOBLANK, FB_BLANK_UNBLANK);
	if (0 != r) {
		goto fbioblank_failed;
	}
#else
	/* ストライドを設定した場合に、2番目3番目のスクリーンがブランクされない問題に関して
	 * ユーザー側でフレームバッファ全領域に0を書き込む必要がある
	 */
	if (NULL == me->framebuffer) {
		/* map the framebuffer */
		me->framebuffer = mmap (0, me->fixinfo.smem_len,
					PROT_WRITE, MAP_SHARED, me->fd, 0);
		if (MAP_FAILED == me->framebuffer) {
			goto mmap_failed;
		}
	}

	/* entirely clear screen */
	memset (me->framebuffer, 0x00, me->fixinfo.smem_len);
#endif

	r = ioctl (me->fd, FBIO_WAITFORVSYNC, &vsyncArg);
	if (0 != r) {
		goto fbio_waitforvsync_failed;
	}

out:
	return ret;

fbiopan_display_failed:
	{
		GST_ELEMENT_ERROR (me, RESOURCE, SETTINGS, (NULL),
			("error with ioctl(FBIOPAN_DISPLAY) %d (%s)", errno, g_strerror (errno)));
		ret = FALSE;
		goto out;
	}
#if 0	// 2013-07-23 : patch
fbioblank_failed:
	{
		GST_ELEMENT_ERROR (me, RESOURCE, SETTINGS, (NULL),
			("error with ioctl(FBIOBLANK) %d (%s)", errno, g_strerror (errno)));
		ret = FALSE;
		goto out;
	}
#else
mmap_failed:
	{
		GST_ELEMENT_ERROR (me, RESOURCE, SETTINGS, (NULL),
			("error with mmap() %d (%s)", errno, g_strerror (errno)));
		ret = FALSE;
		goto out;
	}
#endif
fbio_waitforvsync_failed:
	{
		GST_ELEMENT_ERROR (me, RESOURCE, SETTINGS, (NULL),
			("error with ioctl(FBIO_WAITFORVSYNC) %d (%s)", errno, g_strerror (errno)));
		ret = FALSE;
		goto out;
	}
}

static void
gst_rto_fbdevsink_class_init (GstRtoFBDevSinkClass * klass)
{
	GObjectClass *gobject_class;
	GstElementClass *gstelement_class;
	GstBaseSinkClass *gstvs_class;
	
//    GST_INFO ("RTOFBDEVSINK CLASS INIT");

	gobject_class = (GObjectClass *) klass;
	gstelement_class = (GstElementClass *) klass;
	gstvs_class = (GstBaseSinkClass *) klass;

	g_type_class_add_private (klass, sizeof (GstRtoFBDevSinkPrivate));

	gobject_class->set_property = gst_rto_fbdevsink_set_property;
	gobject_class->get_property = gst_rto_fbdevsink_get_property;
	gobject_class->finalize = gst_rto_fbdevsink_finalize;

	gstelement_class->change_state =
		GST_DEBUG_FUNCPTR (gst_rto_fbdevsink_change_state);

	g_object_class_install_property (G_OBJECT_CLASS (klass), PROP_DEVICE,
		g_param_spec_string ("device", "device",
			"The framebuffer device eg: /dev/fb0", NULL, G_PARAM_READWRITE));

	g_object_class_install_property (gobject_class, PROP_USE_DMABUF,
		g_param_spec_boolean ("use-dmabuf", "Use DMABUF",
			"FALSE: do not use dma-buf, TRUE: use dma-buf",
			DEFAULT_USE_DMABUF, G_PARAM_READWRITE));

	g_object_class_install_property (gobject_class, PROP_ENABLE_VSYNC,
		g_param_spec_boolean ("enable-vsync", "Enable VSYNC",
			"FALSE: disable, TRUE: enable",
			DEFAULT_ENABLE_VSYNC, G_PARAM_READWRITE));

	gst_element_class_set_details_simple (gstelement_class,
		"RTO fbdev video sink", "Sink/Video",
		"A linux framebuffer videosink", "atmark techno");

	gst_element_class_add_pad_template (gstelement_class,
		gst_static_pad_template_get (&sink_template));

	gstvs_class->set_caps = GST_DEBUG_FUNCPTR (gst_rto_fbdevsink_setcaps);
	gstvs_class->get_caps = GST_DEBUG_FUNCPTR (gst_rto_fbdevsink_getcaps);
	gstvs_class->get_times = GST_DEBUG_FUNCPTR (gst_rto_fbdevsink_get_times);
	gstvs_class->preroll = GST_DEBUG_FUNCPTR (gst_rto_fbdevsink_preroll);
	gstvs_class->render = GST_DEBUG_FUNCPTR (gst_rto_fbdevsink_render);
	gstvs_class->start = GST_DEBUG_FUNCPTR (gst_rto_fbdevsink_start);
	gstvs_class->stop = GST_DEBUG_FUNCPTR (gst_rto_fbdevsink_stop);
	gstvs_class->query = GST_DEBUG_FUNCPTR (gst_rto_fbdevsink_query);
}

static void
gst_rto_fbdevsink_init (GstRtoFBDevSink * me)
{
//	GST_INFO_OBJECT (me, "RTOFBDEVSINK INIT");

	me->priv = GST_RTOFBDEVSINK_GET_PRIVATE (me);

	me->fd = -1;
	me->framebuffer = NULL;
	me->device = NULL;

	me->width = 0;
	me->height = 0;
	me->frame_stride = 0;
	me->frame_x_offset = 0;
	me->frame_y_offset = 0;
	me->cx = 0;
	me->cy = 0;
	me->linelen = 0;
	me->lines = 0;
	me->bytespp = 0;

	me->fps_n = 0;
	me->fps_d = 0;

	me->is_changed_fb_varinfo = FALSE;

	me->priv->framerate_sec = 0;
	me->priv->lateness_sec = 0;
	me->priv->prev_display_time_sec = 0;

	/* last buffer 保持無効にする
	 * 遅延して drop された時に、バッファが解放されず、v4l2bufferpool に戻らないため。
	 */
	/* the last buffer we prerolled or rendered. Useful for making snapshots */
	gst_base_sink_set_last_sample_enabled (GST_BASE_SINK (me), FALSE);

	/* ref : GstVideoSink - 20ms is more than enough, 80-130ms is noticable */
#if 0 /* for debug	*/
	gst_base_sink_set_max_lateness (GST_BASE_SINK (me), -1);
#else
	gst_base_sink_set_max_lateness (GST_BASE_SINK (me), 40 * GST_MSECOND);
#endif
	gst_base_sink_set_qos_enabled (GST_BASE_SINK (me), FALSE);

	me->use_dmabuf = DEFAULT_USE_DMABUF;
	me->enable_vsync = DEFAULT_ENABLE_VSYNC;

	/* retrieve and intercept base class chain. */
	me->priv->base_chain = GST_PAD_CHAINFUNC (GST_BASE_SINK_PAD (me));
	gst_pad_set_chain_function (GST_BASE_SINK_PAD (me),
								GST_DEBUG_FUNCPTR (gst_rto_fbdevsink_chain));
}

static void
gst_rto_fbdevsink_finalize (GObject * object)
{
	GstRtoFBDevSink *me = GST_RTOFBDEVSINK (object);

    GST_INFO_OBJECT (me, "RTOFBDEVSINK FINALIZE");

	if (me->device) {
		g_free (me->device);
		me->device = NULL;
	}

	G_OBJECT_CLASS (parent_class)->finalize (object);
}

static gboolean
gst_rto_fbdevsink_start (GstBaseSink * bsink)
{
	GstRtoFBDevSink *me;
	gboolean ret = TRUE;
	gint i = 0;

	me = GST_RTOFBDEVSINK (bsink);

	/* プロパティとしてセットされていなければ、デフォルト値を設定		*/
	if (! me->device) {
		me->device = g_strdup (DEFAULT_FB_DEVICE);
	}

	GST_INFO_OBJECT (me, "RTOFBDEVSINK START. (%s)", me->device);

	/* open device */
	if (-1 == me->fd) {
		me->fd = open (me->device, O_RDWR);
		if (-1 == me->fd) {
			goto open_device_failed;
		}
	}

	/* get the fixed screen info */
	if (0 != ioctl (me->fd, FBIOGET_FSCREENINFO, &(me->fixinfo))) {
		goto fbioget_failed;
	}
	GST_INFO_OBJECT(me,
		"FBIOGET_FSCREENINFO - smem_start:%p, smem_len:%u, line_length:%u",
		me->fixinfo.smem_start, me->fixinfo.smem_len, me->fixinfo.line_length);

	/* get the variable screen info */
	if (0 != ioctl (me->fd, FBIOGET_VSCREENINFO, &(me->varinfo))) {
		goto fbioget_failed;
	}
	GST_INFO_OBJECT(me,
		"FBIOGET_VSCREENINFO - xres:%u, yres:%u, xres_v:%u, yres_v:%u, xoffset:%u, yoffset:%u, bits_per_pixel:%u, activate:%u, sync:%u",
		me->varinfo.xres, me->varinfo.yres,
		me->varinfo.xres_virtual, me->varinfo.yres_virtual,
		me->varinfo.xoffset, me->varinfo.yoffset,
		me->varinfo.bits_per_pixel,
		me->varinfo.activate, me->varinfo.sync);

	/* レストア用にオリジナル情報を保存		*/
	me->saved_varinfo = me->varinfo;

	/* ディスプレイのリフレッシュレートから表示遅延許容時間を算出
	 * （1.0 (sec) / ディスプレイのリフレッシュレート x 1.1 ）
	 */
	me->priv->lateness_sec = (double)1.1 / get_disp_refresh_rate(me, &(me->varinfo));
	GST_INFO_OBJECT (me, "lateness_sec: %f", me->priv->lateness_sec);

	if (me->use_dmabuf) {
		/* DMABUF FDを取得 */
		GST_INFO_OBJECT (me, "get the dma buf's fd...");
		for (i = 0; i < NUM_FB_DMABUF; i++) {
			memset(&(me->priv->fb_dmabuf_exp[i]), 0, sizeof(struct fb_dmabuf_export));
			me->priv->fb_dmabuf_exp[i].index = i;
			me->priv->fb_dmabuf_exp[i].flags = O_CLOEXEC;
			if (0 != ioctl (me->fd, FBIOGET_DMABUF, &(me->priv->fb_dmabuf_exp[i]))) {
				goto fbioget_dmabuf_failed;
			}
			GST_INFO_OBJECT (me, "got the dma buf's fd[%d]=%d",
							 i, me->priv->fb_dmabuf_exp[i].fd);
		}
		
		me->priv->last_show_fb_dmabuf_index = -1;
		me->priv->prev_displaying_buf = NULL;
		me->priv->displaying_buf = NULL;
		me->priv->prev_display_time_sec = 0;
	}
	else {
		/* map the framebuffer */
		if (NULL == me->framebuffer) {
			/* map the framebuffer */
			me->framebuffer = mmap (0, me->fixinfo.smem_len,
									PROT_WRITE, MAP_SHARED, me->fd, 0);
			if (MAP_FAILED == me->framebuffer) {
				goto mmap_failed;
			}
		}
		GST_INFO_OBJECT(me, "framebuffer:%p", me->framebuffer);
	}

	/* 初期画面はブランクスクリーン	*/
	do_blank_screen(me);

out:
	return ret;

open_device_failed:
	{
        GST_ELEMENT_ERROR (me, RESOURCE, NOT_FOUND, (NULL),
			("Failed open device %s. (%s)", me->device, g_strerror (errno)));
		ret = FALSE;
		goto out;
	}
fbioget_failed:
	{
		GST_ELEMENT_ERROR (me, RESOURCE, SETTINGS, (NULL),
			("error with ioctl(FBIOGET) %d (%s)", errno, g_strerror (errno)));
		ret = FALSE;
		goto out;
	}
mmap_failed:
	{
		GST_ELEMENT_ERROR (me, RESOURCE, SETTINGS, (NULL),
			("error with mmap() %d (%s)", errno, g_strerror (errno)));
		ret = FALSE;
		goto out;
	}
fbioget_dmabuf_failed:
	{
		GST_ELEMENT_ERROR (me, RESOURCE, SETTINGS, (NULL),
			("error with ioctl(FBIOGET_DMABUF) %d (%s)", errno, g_strerror (errno)));
		ret = FALSE;
		goto out;
	}
}

static gboolean
gst_rto_fbdevsink_stop (GstBaseSink * bsink)
{
	GstRtoFBDevSink *me;
	gboolean ret = TRUE;
	int r = 0;

	me = GST_RTOFBDEVSINK (bsink);

	GST_INFO_OBJECT (me, "RTOFBDEVSINK STOP. (%s)", me->device);

	/* clear screen	*/
	do_blank_screen(me);

	/* VSCREENINFO を変更した場合は元に戻す	*/
	if (me->is_changed_fb_varinfo) {		
		GST_INFO_OBJECT (me, "restore VSCREENINFO...");
		if (0 != ioctl(me->fd, FBIOPUT_VSCREENINFO, &(me->saved_varinfo))) {
			goto fbioput_failed;
		}

		me->is_changed_fb_varinfo = FALSE;
	}

	if (me->use_dmabuf) {
		if (me->priv->prev_displaying_buf && GST_IS_BUFFER(me->priv->prev_displaying_buf)) {
			gst_buffer_unref(me->priv->prev_displaying_buf);
			me->priv->prev_displaying_buf = NULL;
		}
		if (me->priv->displaying_buf && GST_IS_BUFFER(me->priv->displaying_buf)) {
			gst_buffer_unref(me->priv->displaying_buf);
			me->priv->displaying_buf = NULL;
		}
	}

	/* unmap framebuffer (if used) */
	if (me->framebuffer) {
		r = munmap (me->framebuffer, me->fixinfo.smem_len);
		me->framebuffer = NULL;
		if (0 != r) {
			goto munmap_failed;
		}
	}

	/* close device */
	r = close (me->fd);
	me->fd = -1;
	if (0 != r) {
		goto close_device_failed;
	}

out:
	GST_INFO_OBJECT (me, "RTOFBDEVSINK STOPPED");
	return ret;

munmap_failed:
	{
		GST_ELEMENT_ERROR (me, RESOURCE, SETTINGS, (NULL),
			("error with munmap() %d (%s)", errno, g_strerror (errno)));
		ret = FALSE;
		goto out;
	}
close_device_failed:
	{
		GST_ELEMENT_ERROR (me, RESOURCE, FAILED, (NULL),
			("failed close(%s) %d (%s)", me->device, errno, g_strerror (errno)));
		ret = FALSE;
		goto out;
	}
fbioput_failed:
	{
		GST_ELEMENT_ERROR (me, RESOURCE, SETTINGS, (NULL),
			("error with ioctl(FBIOPUT) %d (%s)", errno, g_strerror (errno)));
		ret = FALSE;
		goto out;
	}
}

/* get the start and end times for syncing on this buffer */
static void
gst_rto_fbdevsink_get_times (GstBaseSink * basesink, GstBuffer * buffer,
    GstClockTime * start, GstClockTime * end)
{
	GstRtoFBDevSink *me;

	me = GST_RTOFBDEVSINK (basesink);

#if 0	/* for debug	*/
	GST_INFO_OBJECT (me, "DTS: %" GST_TIME_FORMAT
					 ", PTS: %" GST_TIME_FORMAT
					 ", DULATION: %" GST_TIME_FORMAT,
					 GST_TIME_ARGS (GST_BUFFER_DTS(buffer)),
					 GST_TIME_ARGS (GST_BUFFER_PTS(buffer)),
					 GST_TIME_ARGS (GST_BUFFER_DURATION(buffer)));
#endif

	if (GST_BUFFER_TIMESTAMP_IS_VALID (buffer)) {
		*start = GST_BUFFER_TIMESTAMP (buffer);
		if (GST_BUFFER_DURATION_IS_VALID (buffer)) {
			*end = *start + GST_BUFFER_DURATION (buffer);
		}
		else {
//			GST_WARNING_OBJECT (me, "BUFFER DURATION IS INVALID");
			if (me->fps_n > 0) {
#if 1
				*end = *start
						+ gst_util_uint64_scale_int (GST_SECOND,
							me->fps_d, me->fps_n);
#else
				*end = *start + (GST_SECOND * me->fps_d) / me->fps_n;
#endif
			}
		}
	}
}

static gboolean
gst_rto_fbdevsink_query (GstBaseSink * sink, GstQuery * query)
{
	GstRtoFBDevSink *me;
	gboolean ret = TRUE;

	me = GST_RTOFBDEVSINK (sink);

	GST_INFO_OBJECT (me, "query:%" GST_PTR_FORMAT, query);

	switch (GST_QUERY_TYPE (query)) {
	case GST_QUERY_ACCEPT_CAPS:
	{
		GstCaps *caps = NULL;
		gboolean result = FALSE;
		GstStructure *structure;
		const gchar *format = NULL;

		gst_query_parse_accept_caps (query, &caps);
		GST_INFO_OBJECT (me, "GST_QUERY_ACCEPT_CAPS %" GST_PTR_FORMAT, caps);
		
		structure = gst_caps_get_structure (caps, 0);
		format = gst_structure_get_string (structure, "format");

		switch (me->varinfo.bits_per_pixel) {
		case 32:
			if (g_str_equal(format, "RGBx")) {
				result = TRUE;
			}
			else if (g_str_equal(format, "RGB")) {
				result = change_fb_varinfo_bpp(me, 24);
			}
			break;
		case 24:
			if (g_str_equal(format, "RGB")) {
				result = TRUE;
			}
			else if (g_str_equal(format, "RGBx")) {
				result = change_fb_varinfo_bpp(me, 32);
			}
			break;
		case 16:
			if (g_str_equal(format, "RGB16")) {
				result = TRUE;
			}
			break;
		default:
			g_assert_not_reached ();
			break;
		}
		
		if (FALSE == result) {
			GST_WARNING_OBJECT (me, "Can't accept caps - bpp:%d, format:%s",
				me->varinfo.bits_per_pixel, format);
		}
		gst_query_set_accept_caps_result (query, result);
		ret = TRUE;
		break;
	}
	case GST_QUERY_CUSTOM:
	{
		GstStructure *structure;
		gint index = 0;

		structure = gst_query_writable_structure (query);
		GST_INFO_OBJECT (me, "GST_QUERY_CUSTOM %" GST_PTR_FORMAT, structure);

		if (gst_structure_has_name (structure, "GstRtoFBDevScreenInfoQuery")) {
			gst_structure_set (structure, "screen_width", G_TYPE_INT,
							   me->varinfo.xres, NULL);
			gst_structure_set (structure, "screen_height", G_TYPE_INT,
							   me->varinfo.yres, NULL);
		}
		else if (gst_structure_has_name (structure, "GstRtoFBDevDmaBufQuery")) {
			if (! me->use_dmabuf) {
				/* 無効値を返す */
				gst_structure_set (structure, "fd", G_TYPE_INT, -1, NULL);
			}
			else {
				/* DMABUF FDを返す */
				if (! gst_structure_get_int(structure, "index", &index)) {
					goto get_index_failed;
				}
				if (index < NUM_FB_DMABUF) {
					gst_structure_set (structure, "fd", G_TYPE_INT,
									   me->priv->fb_dmabuf_exp[index].fd, NULL);
					GST_INFO_OBJECT (me, "return fd[%d]=%d",
									 index, me->priv->fb_dmabuf_exp[index].fd);
				}
				else {
					gst_structure_set (structure, "fd", G_TYPE_INT, -1, NULL);
				}
			}
		}

		ret = TRUE;
		break;
	}
	default:
		ret = GST_BASE_SINK_CLASS (parent_class)->query (sink, query);
	}

out:
	return ret;

get_index_failed:
	{
		GST_ELEMENT_ERROR (me, RESOURCE, SETTINGS, (NULL),
			("not exist 'index' field"));
		ret = FALSE;
		goto out;
	}
}

/* get caps from subclass */
static GstCaps *
gst_rto_fbdevsink_getcaps (GstBaseSink * bsink, GstCaps *filter)
{
	GstRtoFBDevSink *me;
	GstCaps *caps;
	uint32_t rmask;
	uint32_t gmask;
	uint32_t bmask;
	int endianness;
	gint bpp = 0;	/* bits per pixel	*/

	GST_INFO_OBJECT (bsink, "getcaps - filter:%" GST_PTR_FORMAT, filter);

	me = GST_RTOFBDEVSINK (bsink);

	if (-1 == me->fd) {
		return gst_caps_from_string (GST_FBDEV_TEMPLATE_CAPS);
	}

	if (filter) {
		GstStructure *structure;
		const gchar *format = NULL;

		/* LCD:16(RGB565), HDMI:24 or 32	*/
		bpp = me->varinfo.bits_per_pixel;

		structure = gst_caps_get_structure (filter, 0);
		format = gst_structure_get_string (structure, "format");
	
		if (NULL != format) {
			if (g_str_equal(format, "RGB")){
				bpp = 24;
			}
			else if (g_str_equal(format, "RGBx")){
				bpp = 32;
			}
			if (g_str_equal(format, "RGB16")){
				bpp = 16;
			}
		}
		
		if (bpp != me->varinfo.bits_per_pixel) {
			/* BPP が異なる時は、FBデバイスを更新	*/
			if (! change_fb_varinfo_bpp(me, bpp)) {
				return NULL;
			}
		}
	}

	rmask = ((1 << me->varinfo.red.length) - 1)
			<< me->varinfo.red.offset;
	gmask = ((1 << me->varinfo.green.length) - 1)
			<< me->varinfo.green.offset;
	bmask = ((1 << me->varinfo.blue.length) - 1)
			<< me->varinfo.blue.offset;

	endianness = 0;

	switch (me->varinfo.bits_per_pixel) {
	case 32:
		/* swap endian of masks */
		rmask = swapendian (rmask);
		gmask = swapendian (gmask);
		bmask = swapendian (bmask);
		endianness = 4321;
		break;
	case 24:{
		/* swap red and blue masks */
		uint32_t t = rmask;
		
		rmask = bmask;
		bmask = t;
		endianness = 4321;
		break;
	}
	case 15:
	case 16:
		endianness = 1234;
		break;
	default:
		/* other bit depths are not supported */
		g_warning ("unsupported bit depth: %d\n",
				   me->varinfo.bits_per_pixel);
		return NULL;
	}

	/* replace all but width, height, and framerate */
	if (24 == me->varinfo.bits_per_pixel) {
		caps = gst_caps_from_string (GST_FBDEV_TEMPLATE_CAPS_RGB);
	}
	else if (16 == me->varinfo.bits_per_pixel) {
		caps = gst_caps_from_string (GST_FBDEV_TEMPLATE_CAPS_RGB16);
	}
	else {
		caps = gst_caps_from_string (GST_FBDEV_TEMPLATE_CAPS_RGBx);
	}

	gst_caps_set_simple (caps,
						 "bpp", G_TYPE_INT, me->varinfo.bits_per_pixel,
						 "depth", G_TYPE_INT, me->varinfo.red.length +
						 	me->varinfo.green.length +
						 	me->varinfo.blue.length +
						 	me->varinfo.transp.length,
						 "endianness", G_TYPE_INT, endianness,
						 "red_mask", G_TYPE_INT, rmask,
						 "green_mask", G_TYPE_INT, gmask, "blue_mask", G_TYPE_INT, bmask, NULL);
	
	GST_INFO_OBJECT (me, "return caps: %" GST_PTR_FORMAT, caps);

	return caps;
}

/* notify subclass of new caps */
static gboolean
gst_rto_fbdevsink_setcaps (GstBaseSink * bsink, GstCaps * vscapslist)
{
	GstRtoFBDevSink *me;
	GstStructure *structure;
	const GValue *fps;

	me = GST_RTOFBDEVSINK (bsink);

	GST_INFO_OBJECT (me, "receive caps: %" GST_PTR_FORMAT, vscapslist);

	structure = gst_caps_get_structure (vscapslist, 0);

	fps = gst_structure_get_value (structure, "framerate");
	me->fps_n = gst_value_get_fraction_numerator (fps);
	me->fps_d = gst_value_get_fraction_denominator (fps);

	me->priv->framerate_sec = (double)1.0 / ( (double)me->fps_n / me->fps_d );
	GST_INFO_OBJECT (me, "framerate_sec: %f", me->priv->framerate_sec);

	gst_structure_get_int (structure, "width", &(me->width));
	gst_structure_get_int (structure, "height", &(me->height));

	gst_structure_get_int (structure, "stride", &(me->frame_stride));
	gst_structure_get_int (structure, "x-offset", &(me->frame_x_offset));
	gst_structure_get_int (structure, "y-offset", &(me->frame_y_offset));

	/* calculate centering and scanlengths for the video */
#if 0
	me->bytespp = me->fixinfo.line_length / me->varinfo.xres;
#else
	me->bytespp = me->varinfo.bits_per_pixel / 8;
#endif

	me->cx = ((int) me->varinfo.xres - me->width) / 2;
	if (me->cx < 0)
		me->cx = 0;

	me->cy = ((int) me->varinfo.yres - me->height) / 2;
	if (me->cy < 0)
		me->cy = 0;

	me->linelen = me->width * me->bytespp;
	if (me->linelen > me->fixinfo.line_length)
		me->linelen = me->fixinfo.line_length;

	me->lines = me->height;
	if (me->lines > me->varinfo.yres)
		me->lines = me->varinfo.yres;

	GST_INFO_OBJECT (me,
		"width:%d, height:%d, bytespp:%d, cx:%d, cy:%d, linelen:%d, lines:%d",
		me->width, me->height, me->bytespp,
		me->cx, me->cy, me->linelen, me->lines);
	GST_INFO_OBJECT (me,
		"stride:%d, x-offset:%d, y-offset:%d",
		me->frame_stride, me->frame_x_offset, me->frame_y_offset);

	return TRUE;
}

static GstFlowReturn
gst_rto_fbdevsink_preroll (GstBaseSink * bsink, GstBuffer * buf)
{
	GstRtoFBDevSink *me;
	int vsyncArg = 0;
	int r;
#if DBG_MEASURE_PERF_RENDER
	double time_start = 0, time_end = 0;
#endif

	me = GST_RTOFBDEVSINK (bsink);

#if DBG_LOG_RENDER
	GST_INFO_OBJECT (me, "RTOFBDEVSINK PREROLL : %p", buf);
#endif

#if DBG_MEASURE_PERF_RENDER
	time_start = gettimeofday_sec();
#endif

	if (me->enable_vsync) {
		/* Wait for the vertical sync of the display device */
		r = ioctl (me->fd, FBIO_WAITFORVSYNC, &vsyncArg);
		if (0 != r) {
			goto fbio_waitforvsync_failed;
		}
	}

#if DBG_MEASURE_PERF_RENDER
	time_end = gettimeofday_sec();
	GST_INFO_OBJECT(me, "preroll : %10.10f", time_end - time_start);
#endif

	return GST_FLOW_OK;

	/* ERRORS */
fbio_waitforvsync_failed:
	{
		GST_ELEMENT_ERROR (me, RESOURCE, SETTINGS, (NULL),
			("error with ioctl(FBIO_WAITFORVSYNC) %d (%s)", errno, g_strerror (errno)));
		return GST_FLOW_ERROR;
	}
}

static GstFlowReturn
gst_rto_fbdevsink_chain (GstPad * pad, GstObject * parent, GstBuffer * buf)
{
	GstRtoFBDevSink *me = GST_RTOFBDEVSINK (parent);
#if DBG_MEASURE_PERF_RENDER
	static double interval_time_start = 0, interval_time_end = 0;
#endif

#if DBG_MEASURE_PERF_RENDER
	interval_time_end = gettimeofday_sec();
	if (interval_time_start > 0) {
//		if ((interval_time_end - interval_time_start) > 0.033) {
			GST_INFO_OBJECT(me, "chain at(ms) : %10.10f",
				(interval_time_end - interval_time_start)*1e+3);
//		}
	}
	interval_time_start = gettimeofday_sec();
#endif

#if 0 /* for debug */
	GST_INFO_OBJECT (me, "PTS:%" GST_TIME_FORMAT
					  ", DTS:%" GST_TIME_FORMAT ", duration:%" GST_TIME_FORMAT,
					  GST_TIME_ARGS (GST_BUFFER_PTS(buf)),
					  GST_TIME_ARGS (GST_BUFFER_DTS(buf)),
					  GST_TIME_ARGS (GST_BUFFER_DURATION(buf)));
#endif

	/* call gst_base_sink_chain() */
	return me->priv->base_chain (pad, parent, buf);
}

static GstFlowReturn
gst_rto_fbdevsink_render (GstBaseSink * bsink, GstBuffer * buf)
{
	GstRtoFBDevSink *me;
	GstMapInfo map;
	int r;
#if DBG_MEASURE_PERF_RENDER
	static double interval_time_start = 0, interval_time_end = 0;
	double time_start = 0, time_end = 0;
#endif

	me = GST_RTOFBDEVSINK (bsink);

//	GST_INFO_OBJECT (me, "RTOFBDEVSINK RENDER BEGIN : %p", buf);

	if (! me->use_dmabuf) {
		/* optimization could remove this memcpy by allocating the buffer
		 in framebuffer memory, but would only work when xres matches
		 the video width */
		gst_buffer_map (buf, &map, GST_MAP_READ);
		GST_DEBUG_OBJECT (me, "RENDER - map.size:%u", map.size);

#if 0	/* 2013-06-18 : stride, offset 対応 */
		if (0 == me->cx && 0 == me->cy) {
			memcpy (me->framebuffer, map.data, map.size);
		}
		else {
			int i;
			for (i = 0; i < me->lines; i++) {
				memcpy (me->framebuffer
						+ (i + me->cy) * me->fixinfo.line_length
						+ me->cx * me->bytespp,
						map.data + i * me->width * me->bytespp,
						me->linelen);
			}
		}
#else 
		{
			guint bytesperline = me->frame_stride * me->bytespp;
			guint offset = (me->frame_y_offset * bytesperline)
							+ (me->frame_x_offset * me->bytespp);
//			GST_INFO_OBJECT (me, "offset:%u bytesperline:%u", offset, bytesperline);
#if 0
			memcpy (me->framebuffer + offset,
					map.data + offset,
					map.size - offset);
#else
			guint pos;
			for (pos = offset; pos < map.size; pos += bytesperline) {
				memcpy (me->framebuffer + pos,
						map.data + pos,
						me->width * me->bytespp);
			}
#endif
		}
#endif

		gst_buffer_unmap (buf, &map);
	}
	else {
		GstRtoDmabufMeta *meta = NULL;
		gboolean isFrameSkipped = FALSE;

		/* バッファからDMABUF index取得	*/
		meta = gst_buffer_get_rto_dmabuf_meta (buf);
		if (meta) {
			int vsyncArg = 0;
			
#if DBG_MEASURE_PERF_RENDER
			interval_time_end = gettimeofday_sec();
			if (interval_time_start > 0) {
//				if ((interval_time_end - interval_time_start) > 0.033) {
					GST_INFO_OBJECT(me, "render at(ms) : %10.10f",
						(interval_time_end - interval_time_start)*1e+3);
//				}
			}
			interval_time_start = gettimeofday_sec();
			time_start = gettimeofday_sec();
#endif

			/* パンする	*/
			me->varinfo.yoffset = me->varinfo.yres * meta->index;
			//me->varinfo.activate = FB_ACTIVATE_VBL;

#if DBG_LOG_RENDER
			GST_INFO_OBJECT (me, "%p : index:%d, fd:%d, yoffset:%d",
							 buf, meta->index, meta->fd, me->varinfo.yoffset);
#endif

			if (me->priv->last_show_fb_dmabuf_index == meta->index) {
				GST_WARNING_OBJECT (me, "index is same as last render : %d",
									meta->index);
			}

			if (me->enable_vsync) {
				double displayTime;
				double timeDiff;
				/* 垂直同期を行う場合は、前回表示時刻からの差分で判断する。
				 * そうしないと、永久にフレームがドロップされるケースが出てくる。
				 * フレームレート + 垂直同期の最大待ち時間を超える場合は、スキップする
				 * ただし、3 フレーム分以上経過している場合は絵が止まったままになるので、
				 * 表示は行う。
				 */
				if (0 == me->priv->prev_display_time_sec) {
					me->priv->prev_display_time_sec = gettimeofday_sec();
				}

				displayTime = gettimeofday_sec();
				timeDiff = displayTime - me->priv->prev_display_time_sec;
				
				if (timeDiff < (me->priv->framerate_sec + me->priv->lateness_sec)) {
					r = ioctl (me->fd, FBIOPAN_DISPLAY, &(me->varinfo));
					if (0 != r) {
						goto fbiopan_display_failed;
					}

					if (1.0 == GST_BASE_SINK(me)->segment.rate) {
						/* Wait for the vertical sync of the display device
						 * 倍速再生の際は、コマ落ちするので、同期しない
						 */
						r = ioctl (me->fd, FBIO_WAITFORVSYNC, &vsyncArg);
						if (0 != r) {
							goto fbio_waitforvsync_failed;
						}
					}
				}
				else if (timeDiff > me->priv->framerate_sec * 3) {
					r = ioctl (me->fd, FBIOPAN_DISPLAY, &(me->varinfo));
					if (0 != r) {
						goto fbiopan_display_failed;
					}
#if DBG_LOG_RENDER_SKIP	/* for debug */
					GST_WARNING_OBJECT (me, "too late frame - at time : %f",
										timeDiff);
#endif
				}
				else {
					isFrameSkipped = TRUE;
				
#if DBG_LOG_RENDER_SKIP	/* for debug */
					GST_WARNING_OBJECT (me, "skipping frame - at time : %f",
						timeDiff);
#endif
				}

				me->priv->prev_display_time_sec = displayTime;
			}
			else {
				/* 垂直同期を行わない場合は、パンするだけ	*/
				r = ioctl (me->fd, FBIOPAN_DISPLAY, &(me->varinfo));
				if (0 != r) {
					goto fbiopan_display_failed;
				}
			}

#if DBG_MEASURE_PERF_RENDER
			time_end = gettimeofday_sec();
//			if ((time_end - time_start) > 0.033) {
				GST_INFO_OBJECT(me, "render : %10.10f", time_end - time_start);
//			}
#endif

			/* ディスプレイ表示中のバッファは ref して保持し、次のバッファを表示した後、
			 * unref してデバイスに queue する
			 */
			if (! isFrameSkipped) {
				if (NULL != me->priv->prev_displaying_buf) {
					gst_buffer_unref(me->priv->prev_displaying_buf);
					me->priv->prev_displaying_buf = NULL;
				}
				if (NULL != me->priv->displaying_buf) {
					me->priv->prev_displaying_buf = me->priv->displaying_buf;
					me->priv->displaying_buf = NULL;
				}
				me->priv->displaying_buf = gst_buffer_ref(buf);
			}
#if 0 /* for debug */
			else {
				GST_WARNING_OBJECT (me, "not ref buf");
			}
#endif

			me->priv->last_show_fb_dmabuf_index = meta->index;
		}
		else {
			goto get_meta_failed;
		}
	}

//	GST_INFO_OBJECT (me, "RTOFBDEVSINK RENDER END");

	return GST_FLOW_OK;

	/* ERRORS */
get_meta_failed:
	{
		GST_ELEMENT_ERROR (me, RESOURCE, SETTINGS, (NULL),
			("failed get dmabuf meta from buffer"));
		return GST_FLOW_ERROR;
	}
fbiopan_display_failed:
	{
		GST_ELEMENT_ERROR (me, RESOURCE, SETTINGS, (NULL),
			("error with ioctl(FBIOPAN_DISPLAY) %d (%s)", errno, g_strerror (errno)));
		return GST_FLOW_ERROR;
	}
fbio_waitforvsync_failed:
	{
		GST_ELEMENT_ERROR (me, RESOURCE, SETTINGS, (NULL),
			("error with ioctl(FBIO_WAITFORVSYNC) %d (%s)", errno, g_strerror (errno)));
		return GST_FLOW_ERROR;
	}
}

static void
gst_rto_fbdevsink_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
	GstRtoFBDevSink *me;

	me = GST_RTOFBDEVSINK (object);

	switch (prop_id) {
	case PROP_DEVICE:
		if (me->device) {
			g_free (me->device);
		}
		me->device = g_value_dup_string (value);
		break;
	case PROP_USE_DMABUF:
		me->use_dmabuf = g_value_get_boolean (value);
		break;
	case PROP_ENABLE_VSYNC:
		me->enable_vsync = g_value_get_boolean (value);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
gst_rto_fbdevsink_get_property (GObject * object, guint prop_id, GValue * value,
    GParamSpec * pspec)
{
	GstRtoFBDevSink *me;

	me = GST_RTOFBDEVSINK (object);

	switch (prop_id) {
	case PROP_DEVICE:
		g_value_set_string (value, me->device);
		break;
	case PROP_USE_DMABUF:
		g_value_set_boolean (value, me->use_dmabuf);
		break;
	case PROP_ENABLE_VSYNC:
		g_value_set_boolean (value, me->enable_vsync);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static GstStateChangeReturn
gst_rto_fbdevsink_change_state (GstElement * element, GstStateChange transition)
{
	GstStateChangeReturn ret = GST_STATE_CHANGE_SUCCESS;
	GstRtoFBDevSink *me;
	
	me = GST_RTOFBDEVSINK (element);

	g_return_val_if_fail (GST_IS_RTOFBDEVSINK (element), GST_STATE_CHANGE_FAILURE);

	ret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);

	switch (transition) {
	case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
	{
		/* PAUSE 時、ブランクスクリーンにする	*/
		do_blank_screen(me);
		break;
	}
	default:
		break;
	}
	return ret;
}

static gboolean
plugin_init (GstPlugin * plugin)
{
	GST_DEBUG_CATEGORY_INIT (rtofbdevsink_debug, "rtofbdevsink",
							 0, "RTO fbdev sink");

	return gst_element_register (plugin, "rtofbdevsink", GST_RANK_PRIMARY,
								 GST_TYPE_RTOFBDEVSINK);
}

GST_PLUGIN_DEFINE (
	GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    rtofbdevsink,
    "RTO linux framebuffer video sink",
    plugin_init,
	VERSION,
	"GPL",
	"GStreamer",
	"http://gstreamer.net/"
);

/*
 * End of file
 */
