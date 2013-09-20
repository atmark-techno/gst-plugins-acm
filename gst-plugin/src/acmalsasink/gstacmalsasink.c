/* GStreamer
 * Copyright (C) 2005 Wim Taymans <wim@fluendo.com>
 * Copyright (C) 2006 Tim-Philipp Müller <tim centricular net>
 *
 * gstacmalsasink.c:
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
 * SECTION:element-acmalsasink
 * @see_also: alsasrc
 *
 * This element renders raw audio samples using the ALSA api.
 *
 * <refsect2>
 * <title>Example pipelines</title>
 * |[
 * gst-launch -v filesrc location=sine.ogg ! oggdemux ! vorbisdec ! audioconvert ! audioresample ! acmalsasink
 * ]| Play an Ogg/Vorbis file.
 * </refsect2>
 *
 * Last reviewed on 2006-03-01 (0.10.4)
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <sys/ioctl.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <getopt.h>
#include <pthread.h>

#include "gstacmalsasink.h"

GST_DEBUG_CATEGORY (alsa_debug);
#define GST_CAT_DEFAULT alsa_debug

/* ************************************************************************ */
#define GST_TYPE_AUDIO_SINK_RING_BUFFER        \
(gst_audio_sink_ring_buffer_get_type())
#define GST_AUDIO_SINK_RING_BUFFER(obj)        \
(G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_AUDIO_SINK_RING_BUFFER,GstAcmAudioSinkRingBuffer))
#define GST_AUDIO_SINK_RING_BUFFER_CLASS(klass) \
(G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_AUDIO_SINK_RING_BUFFER,GstAudioSinkRingBufferClass))
#define GST_AUDIO_SINK_RING_BUFFER_GET_CLASS(obj) \
(G_TYPE_INSTANCE_GET_CLASS ((obj), GST_TYPE_AUDIO_SINK_RING_BUFFER, GstAudioSinkRingBufferClass))
#define GST_AUDIO_SINK_RING_BUFFER_CAST(obj)        \
((GstAcmAudioSinkRingBuffer *)obj)
#define GST_IS_AUDIO_SINK_RING_BUFFER(obj)     \
(G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_AUDIO_SINK_RING_BUFFER))
#define GST_IS_AUDIO_SINK_RING_BUFFER_CLASS(klass)\
(G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_AUDIO_SINK_RING_BUFFER))

typedef struct _GstAudioSinkRingBuffer GstAcmAudioSinkRingBuffer;
typedef struct _GstAudioSinkRingBufferClass GstAudioSinkRingBufferClass;

#define GST_AUDIO_SINK_RING_BUFFER_GET_COND(buf) (&(((GstAcmAudioSinkRingBuffer *)buf)->cond))
#define GST_AUDIO_SINK_RING_BUFFER_WAIT(buf)     (g_cond_wait (GST_AUDIO_SINK_RING_BUFFER_GET_COND (buf), GST_OBJECT_GET_LOCK (buf)))
#define GST_AUDIO_SINK_RING_BUFFER_SIGNAL(buf)   (g_cond_signal (GST_AUDIO_SINK_RING_BUFFER_GET_COND (buf)))
#define GST_AUDIO_SINK_RING_BUFFER_BROADCAST(buf)(g_cond_broadcast (GST_AUDIO_SINK_RING_BUFFER_GET_COND (buf)))

struct _GstAudioSinkRingBuffer
{
	GstAudioRingBuffer object;
	
	gboolean running;
	gint queuedseg;
	
	GCond cond;
	
#if 1	// by ACM-ALSA-SINK
	pthread_t task_id;
#endif
};

struct _GstAudioSinkRingBufferClass
{
	GstAudioRingBufferClass parent_class;
};

static void gst_audio_sink_ring_buffer_class_init (GstAudioSinkRingBufferClass *
												   klass);
static void gst_audio_sink_ring_buffer_init (GstAcmAudioSinkRingBuffer *
											 ringbuffer, GstAudioSinkRingBufferClass * klass);
static void gst_audio_sink_ring_buffer_dispose (GObject * object);
static void gst_audio_sink_ring_buffer_finalize (GObject * object);

static GstAudioRingBufferClass *ring_parent_class = NULL;

static gboolean gst_audio_sink_ring_buffer_open_device (GstAudioRingBuffer *
														buf);
static gboolean gst_audio_sink_ring_buffer_close_device (GstAudioRingBuffer *
														 buf);
static gboolean gst_audio_sink_ring_buffer_acquire (GstAudioRingBuffer * buf,
													GstAudioRingBufferSpec * spec);
static gboolean gst_audio_sink_ring_buffer_release (GstAudioRingBuffer * buf);
static gboolean gst_audio_sink_ring_buffer_start (GstAudioRingBuffer * buf);
static gboolean gst_audio_sink_ring_buffer_pause (GstAudioRingBuffer * buf);
static gboolean gst_audio_sink_ring_buffer_stop (GstAudioRingBuffer * buf);
static guint gst_audio_sink_ring_buffer_delay (GstAudioRingBuffer * buf);
static gboolean gst_audio_sink_ring_buffer_activate (GstAudioRingBuffer * buf,
													 gboolean active);

/* ringbuffer abstract base class */
static GType
gst_audio_sink_ring_buffer_get_type (void)
{
	static GType ringbuffer_type = 0;
	
	if (!ringbuffer_type) {
		static const GTypeInfo ringbuffer_info = {
			sizeof (GstAudioSinkRingBufferClass),
			NULL,
			NULL,
			(GClassInitFunc) gst_audio_sink_ring_buffer_class_init,
			NULL,
			NULL,
			sizeof (GstAcmAudioSinkRingBuffer),
			0,
			(GInstanceInitFunc) gst_audio_sink_ring_buffer_init,
			NULL
		};
		
		ringbuffer_type =
        g_type_register_static (GST_TYPE_AUDIO_RING_BUFFER,
								"GstAcmAudioSinkRingBuffer", &ringbuffer_info, 0);
	}
	return ringbuffer_type;
}

static void
gst_audio_sink_ring_buffer_class_init (GstAudioSinkRingBufferClass * klass)
{
	GObjectClass *gobject_class;
	GstAudioRingBufferClass *gstringbuffer_class;
	
	gobject_class = (GObjectClass *) klass;
	gstringbuffer_class = (GstAudioRingBufferClass *) klass;
	
	ring_parent_class = g_type_class_peek_parent (klass);
	
	gobject_class->dispose = gst_audio_sink_ring_buffer_dispose;
	gobject_class->finalize = gst_audio_sink_ring_buffer_finalize;
	
	gstringbuffer_class->open_device =
	GST_DEBUG_FUNCPTR (gst_audio_sink_ring_buffer_open_device);
	gstringbuffer_class->close_device =
	GST_DEBUG_FUNCPTR (gst_audio_sink_ring_buffer_close_device);
	gstringbuffer_class->acquire =
	GST_DEBUG_FUNCPTR (gst_audio_sink_ring_buffer_acquire);
	gstringbuffer_class->release =
	GST_DEBUG_FUNCPTR (gst_audio_sink_ring_buffer_release);
	gstringbuffer_class->start =
	GST_DEBUG_FUNCPTR (gst_audio_sink_ring_buffer_start);
	gstringbuffer_class->pause =
	GST_DEBUG_FUNCPTR (gst_audio_sink_ring_buffer_pause);
	gstringbuffer_class->resume =
	GST_DEBUG_FUNCPTR (gst_audio_sink_ring_buffer_start);
	gstringbuffer_class->stop =
	GST_DEBUG_FUNCPTR (gst_audio_sink_ring_buffer_stop);
	
	gstringbuffer_class->delay =
	GST_DEBUG_FUNCPTR (gst_audio_sink_ring_buffer_delay);
	gstringbuffer_class->activate =
	GST_DEBUG_FUNCPTR (gst_audio_sink_ring_buffer_activate);
}

typedef gint (*WriteFunc) (GstAudioSink * sink, gpointer data, guint length);

/* this internal thread does nothing else but write samples to the audio device.
 * It will write each segment in the ringbuffer and will update the play
 * pointer.
 * The start/stop methods control the thread.
 */
static void
audioringbuffer_thread_func (GstAudioRingBuffer * buf)
{
	GstAudioSink *sink;
	GstAudioSinkClass *csink;
	GstAcmAudioSinkRingBuffer *abuf = GST_AUDIO_SINK_RING_BUFFER_CAST (buf);
	WriteFunc writefunc;
	GstMessage *message;
	GValue val = { 0 };

	sink = GST_AUDIO_SINK (GST_OBJECT_PARENT (buf));
	csink = GST_AUDIO_SINK_GET_CLASS (sink);

	GST_DEBUG_OBJECT (sink, "enter thread");

	GST_OBJECT_LOCK (abuf);
	GST_DEBUG_OBJECT (sink, "signal wait");
	GST_AUDIO_SINK_RING_BUFFER_SIGNAL (buf);
	GST_OBJECT_UNLOCK (abuf);

	writefunc = csink->write;
	if (writefunc == NULL)
		goto no_function;

	g_value_init (&val, G_TYPE_POINTER);
	g_value_set_pointer (&val, sink->thread);
	message = gst_message_new_stream_status (GST_OBJECT_CAST (buf),
											 GST_STREAM_STATUS_TYPE_ENTER, GST_ELEMENT_CAST (sink));
	gst_message_set_stream_status_object (message, &val);
	GST_DEBUG_OBJECT (sink, "posting ENTER stream status");
	gst_element_post_message (GST_ELEMENT_CAST (sink), message);

	while (TRUE) {
		gint left, len;
		guint8 *readptr;
		gint readseg;

		/* buffer must be started */
		if (gst_audio_ring_buffer_prepare_read (buf, &readseg, &readptr, &len)) {
			gint written;

			left = len;
			do {
				written = writefunc (sink, readptr, left);
				GST_DEBUG_OBJECT (sink, "transfered %d bytes of %d from segment %d",
								written, left, readseg);
				if (written < 0 || written > left) {
					/* might not be critical, it e.g. happens when aborting playback */
					GST_WARNING_OBJECT (sink,
										"error writing data in %s (reason: %s), skipping segment (left: %d, written: %d)",
										GST_DEBUG_FUNCPTR_NAME (writefunc),
										(errno > 1 ? g_strerror (errno) : "unknown"), left, written);
					break;
				}
				left -= written;
				readptr += written;
			} while (left > 0);

			/* clear written samples */
			gst_audio_ring_buffer_clear (buf, readseg);

			/* we wrote one segment */
			gst_audio_ring_buffer_advance (buf, 1);
		} else {
			GST_OBJECT_LOCK (abuf);
			if (!abuf->running)
				goto stop_running;
			if (G_UNLIKELY (g_atomic_int_get (&buf->state) ==
							GST_AUDIO_RING_BUFFER_STATE_STARTED)) {
				GST_OBJECT_UNLOCK (abuf);
				continue;
			}
			GST_DEBUG_OBJECT (sink, "signal wait");
			GST_AUDIO_SINK_RING_BUFFER_SIGNAL (buf);
			GST_DEBUG_OBJECT (sink, "wait for action");
			GST_AUDIO_SINK_RING_BUFFER_WAIT (buf);
			GST_DEBUG_OBJECT (sink, "got signal");
			if (!abuf->running)
				goto stop_running;
			GST_DEBUG_OBJECT (sink, "continue running");
			GST_OBJECT_UNLOCK (abuf);
		}
	}

	/* Will never be reached */
	g_assert_not_reached ();
	return;

	/* ERROR */
no_function:
	{
		GST_DEBUG_OBJECT (sink, "no write function, exit thread");
		return;
	}
stop_running:
	{
		GST_OBJECT_UNLOCK (abuf);
		GST_DEBUG_OBJECT (sink, "stop running, exit thread");
		message = gst_message_new_stream_status (GST_OBJECT_CAST (buf),
			GST_STREAM_STATUS_TYPE_LEAVE, GST_ELEMENT_CAST (sink));
		gst_message_set_stream_status_object (message, &val);
		GST_DEBUG_OBJECT (sink, "posting LEAVE stream status");
		gst_element_post_message (GST_ELEMENT_CAST (sink), message);
		return;
	}
}

#if 1 	// by ACM-ALSA-SINK
static void*
audioringbuffer_thread_proc (void * arg)
{
	GstAudioRingBuffer * buf = (GstAudioRingBuffer*)arg;

	audioringbuffer_thread_func(buf);

	return NULL;
}
#endif

static void
gst_audio_sink_ring_buffer_init (GstAcmAudioSinkRingBuffer * ringbuffer,
								 GstAudioSinkRingBufferClass * g_class)
{
	ringbuffer->running = FALSE;
	ringbuffer->queuedseg = 0;

	g_cond_init (&ringbuffer->cond);
}

static void
gst_audio_sink_ring_buffer_dispose (GObject * object)
{
	G_OBJECT_CLASS (ring_parent_class)->dispose (object);
}

static void
gst_audio_sink_ring_buffer_finalize (GObject * object)
{
	GstAcmAudioSinkRingBuffer *ringbuffer = GST_AUDIO_SINK_RING_BUFFER_CAST (object);

	g_cond_clear (&ringbuffer->cond);

	G_OBJECT_CLASS (ring_parent_class)->finalize (object);
}

static gboolean
gst_audio_sink_ring_buffer_open_device (GstAudioRingBuffer * buf)
{
	GstAudioSink *sink;
	GstAudioSinkClass *csink;
	gboolean result = TRUE;

	sink = GST_AUDIO_SINK (GST_OBJECT_PARENT (buf));
	csink = GST_AUDIO_SINK_GET_CLASS (sink);

	if (csink->open)
		result = csink->open (sink);

	if (!result)
		goto could_not_open;

	return result;
	
could_not_open:
	{
		GST_DEBUG_OBJECT (sink, "could not open device");
		return FALSE;
	}
}

static gboolean
gst_audio_sink_ring_buffer_close_device (GstAudioRingBuffer * buf)
{
	GstAudioSink *sink;
	GstAudioSinkClass *csink;
	gboolean result = TRUE;

	sink = GST_AUDIO_SINK (GST_OBJECT_PARENT (buf));
	csink = GST_AUDIO_SINK_GET_CLASS (sink);

	if (csink->close)
		result = csink->close (sink);

	if (!result)
		goto could_not_close;

	return result;

could_not_close:
	{
		GST_DEBUG_OBJECT (sink, "could not close device");
		return FALSE;
	}
}

static gboolean
gst_audio_sink_ring_buffer_acquire (GstAudioRingBuffer * buf,
									GstAudioRingBufferSpec * spec)
{
	GstAudioSink *sink;
	GstAudioSinkClass *csink;
	gboolean result = FALSE;

	sink = GST_AUDIO_SINK (GST_OBJECT_PARENT (buf));
	csink = GST_AUDIO_SINK_GET_CLASS (sink);

	if (csink->prepare)
		result = csink->prepare (sink, spec);
	if (!result)
		goto could_not_prepare;

	/* set latency to one more segment as we need some headroom */
	spec->seglatency = spec->segtotal + 1;

	buf->size = spec->segtotal * spec->segsize;
	buf->memory = g_malloc0 (buf->size);

	return TRUE;

	/* ERRORS */
could_not_prepare:
	{
		GST_DEBUG_OBJECT (sink, "could not prepare device");
		return FALSE;
	}
}

static gboolean
gst_audio_sink_ring_buffer_activate (GstAudioRingBuffer * buf, gboolean active)
{
	GstAudioSink *sink;
	GstAcmAudioSinkRingBuffer *abuf;
#if 0	// by ACM-ALSA-SINK
	GError *error = NULL;
#endif

	sink = GST_AUDIO_SINK (GST_OBJECT_PARENT (buf));
	abuf = GST_AUDIO_SINK_RING_BUFFER_CAST (buf);

	if (active) {
		abuf->running = TRUE;
		
		GST_DEBUG_OBJECT (sink, "starting thread");
#if 0	// by ACM-ALSA-SINK
		sink->thread = g_thread_try_new ("audiosink-ringbuffer",
					(GThreadFunc) audioringbuffer_thread_func, buf, &error);
		
		if (!sink->thread || error != NULL)
			goto thread_failed;
#else
		{
			struct sched_param	sp;
			int	policy;
			int	r;

			GST_INFO_OBJECT (sink, "STARTING THREAD...");
			if (0 != pthread_create(
						&(abuf->task_id), NULL, audioringbuffer_thread_proc, buf)) {
				GST_ELEMENT_ERROR (sink, STREAM, DECODE, (NULL),
					("failed create thread. %d (%s)", errno, g_strerror (errno)));
				return FALSE;
			}

#if 0	/* for debug	*/
			GST_INFO_OBJECT(sink, "sched_get_priority_min( SCHED_FIFO ):[%d]",
				   sched_get_priority_min( SCHED_FIFO ));
			GST_INFO_OBJECT(sink, "sched_get_priority_max( SCHED_FIFO ):[%d]",
				   sched_get_priority_max( SCHED_FIFO ));
#endif

			r = pthread_getschedparam(abuf->task_id, &policy, &sp);
			if (0 != r) {
				GST_ELEMENT_ERROR (sink, STREAM, DECODE, (NULL),
					("failed getschedparam. %d (%s)", errno, g_strerror (errno)));
				return FALSE;
			}
			GST_INFO_OBJECT (sink, "old thread priority %d", sp.sched_priority);
			sp.sched_priority = 30;
			r = pthread_setschedparam(abuf->task_id, SCHED_FIFO, &sp);
			if (0 != r) {
				if (EPERM == errno) {
					GST_WARNING_OBJECT (sink, "failed set thread priority. "
										"does not have superuser permissions.");
				}
				else {
					GST_ELEMENT_ERROR (sink, STREAM, DECODE, (NULL),
						("failed setschedparam. %d (%s)", errno, g_strerror (errno)));
					return FALSE;
				}
			}
			r = pthread_getschedparam(abuf->task_id, &policy, &sp);
			if (0 != r) {
				GST_ELEMENT_ERROR (sink, STREAM, DECODE, (NULL),
					("failed getschedparam. %d (%s)", errno, g_strerror (errno)));
				return FALSE;
			}
			GST_INFO_OBJECT (sink, "new thread priority %d", sp.sched_priority);
			GST_INFO_OBJECT (sink, "STARTED THREAD");
		}
#endif
		GST_DEBUG_OBJECT (sink, "waiting for thread");
		/* the object lock is taken */
		GST_AUDIO_SINK_RING_BUFFER_WAIT (buf);
		GST_DEBUG_OBJECT (sink, "thread is started");
	} else {
		abuf->running = FALSE;
		GST_DEBUG_OBJECT (sink, "signal wait");
		GST_AUDIO_SINK_RING_BUFFER_SIGNAL (buf);

		GST_OBJECT_UNLOCK (buf);

		/* join the thread */
#if 0	// by ACM-ALSA-SINK
		g_thread_join (sink->thread);
#else
		GST_INFO_OBJECT (sink, "STOPPING THREAD...");
		if (0 != pthread_join(abuf->task_id, NULL)) {
			GST_ERROR_OBJECT (sink,
				"failed stop thread. %d (%s)", errno, g_strerror (errno));
			return FALSE;
		}
		GST_INFO_OBJECT (sink, "STOPED THREAD");
		abuf->task_id = -1;
#endif

		GST_OBJECT_LOCK (buf);
	}
	return TRUE;
	
#if 0	// by ACM-ALSA-SINK
	/* ERRORS */
thread_failed:
	{
		if (error)
			GST_ERROR_OBJECT (sink, "could not create thread %s", error->message);
		else
			GST_ERROR_OBJECT (sink, "could not create thread for unknown reason");
		return FALSE;
	}
#endif
}

/* function is called with LOCK */
static gboolean
gst_audio_sink_ring_buffer_release (GstAudioRingBuffer * buf)
{
	GstAudioSink *sink;
	GstAudioSinkClass *csink;
	gboolean result = FALSE;

	sink = GST_AUDIO_SINK (GST_OBJECT_PARENT (buf));
	csink = GST_AUDIO_SINK_GET_CLASS (sink);

	/* free the buffer */
	g_free (buf->memory);
	buf->memory = NULL;

	if (csink->unprepare)
		result = csink->unprepare (sink);

	if (!result)
		goto could_not_unprepare;

	GST_DEBUG_OBJECT (sink, "unprepared");

	return result;

could_not_unprepare:
	{
		GST_DEBUG_OBJECT (sink, "could not unprepare device");
		return FALSE;
	}
}

static gboolean
gst_audio_sink_ring_buffer_start (GstAudioRingBuffer * buf)
{
	GstAudioSink *sink;

	sink = GST_AUDIO_SINK (GST_OBJECT_PARENT (buf));

	GST_DEBUG_OBJECT (sink, "start, sending signal");
	GST_AUDIO_SINK_RING_BUFFER_SIGNAL (buf);

	return TRUE;
}

static gboolean
gst_audio_sink_ring_buffer_pause (GstAudioRingBuffer * buf)
{
	GstAudioSink *sink;
	GstAudioSinkClass *csink;

	sink = GST_AUDIO_SINK (GST_OBJECT_PARENT (buf));
	csink = GST_AUDIO_SINK_GET_CLASS (sink);

	/* unblock any pending writes to the audio device */
	if (csink->reset) {
		GST_DEBUG_OBJECT (sink, "reset...");
		csink->reset (sink);
		GST_DEBUG_OBJECT (sink, "reset done");
	}
	
	return TRUE;
}

static gboolean
gst_audio_sink_ring_buffer_stop (GstAudioRingBuffer * buf)
{
	GstAudioSink *sink;
	GstAudioSinkClass *csink;

	sink = GST_AUDIO_SINK (GST_OBJECT_PARENT (buf));
	csink = GST_AUDIO_SINK_GET_CLASS (sink);

	/* unblock any pending writes to the audio device */
	if (csink->reset) {
		GST_DEBUG_OBJECT (sink, "reset...");
		csink->reset (sink);
		GST_DEBUG_OBJECT (sink, "reset done");
	}
#if 0
	if (abuf->running) {
		GST_DEBUG_OBJECT (sink, "stop, waiting...");
		GST_AUDIO_SINK_RING_BUFFER_WAIT (buf);
		GST_DEBUG_OBJECT (sink, "stopped");
	}
#endif
	
	return TRUE;
}

static guint
gst_audio_sink_ring_buffer_delay (GstAudioRingBuffer * buf)
{
	GstAudioSink *sink;
	GstAudioSinkClass *csink;
	guint res = 0;

	sink = GST_AUDIO_SINK (GST_OBJECT_PARENT (buf));
	csink = GST_AUDIO_SINK_GET_CLASS (sink);

	if (csink->delay)
		res = csink->delay (sink);

	return res;
}
/* ************************************************************************ */

static void gst_acmalsasink_init_interfaces (GType type);
static GstAudioRingBuffer *gst_acmalsasink_create_ringbuffer (
	GstAudioBaseSink *sink);


#define gst_acmalsasink_parent_class parent_class
///G_DEFINE_TYPE (GstAcmAlsaSink, gst_acmalsasink, GST_TYPE_AUDIO_SINK);
G_DEFINE_TYPE_WITH_CODE (GstAcmAlsaSink, gst_acmalsasink,
	GST_TYPE_ALSA_SINK, gst_acmalsasink_init_interfaces (g_define_type_id));

static void
gst_acmalsasink_init_interfaces (GType type)
{
}

static void
gst_acmalsasink_class_init (GstAcmAlsaSinkClass * klass)
{
	GstElementClass *gstelement_class;
	GstAudioBaseSinkClass *gstaudiobasesink_class;

	gstelement_class = (GstElementClass *) klass;
	gstaudiobasesink_class = (GstAudioBaseSinkClass *) klass;

	gstaudiobasesink_class->create_ringbuffer =
		GST_DEBUG_FUNCPTR (gst_acmalsasink_create_ringbuffer);

	g_type_class_ref (GST_TYPE_AUDIO_SINK_RING_BUFFER);

	gst_element_class_set_static_metadata (gstelement_class,
      "ACM Audio sink (ALSA)", "Sink/Audio",
      "Output to a sound card via ALSA", "Atmark Techno, Inc.");
}

static void
gst_acmalsasink_init (GstAcmAlsaSink * me)
{
	GST_DEBUG_OBJECT (me, "INIT ACM ALSASINK");

	/* do nothing	*/
}

static GstAudioRingBuffer *
gst_acmalsasink_create_ringbuffer (GstAudioBaseSink * sink)
{
	GstAudioRingBuffer *buffer;
	GstAcmAlsaSink *me = GST_ACM_ALSA_SINK (sink);

	GST_INFO_OBJECT (me, "creating ringbuffer");
	buffer = g_object_new (GST_TYPE_AUDIO_SINK_RING_BUFFER, NULL);
	GST_INFO_OBJECT (me, "created ringbuffer @%p", buffer);

	return buffer;
}

/* ALSA debugging wrapper */
static void
gst_alsa_error_wrapper (const char *file, int line, const char *function,
						int err, const char *fmt, ...)
{
	va_list args;
	gchar *str;

	va_start (args, fmt);
	str = g_strdup_vprintf (fmt, args);
	va_end (args);
	/* FIXME: use GST_LEVEL_ERROR here? Currently warning is used because we're
	 * able to catch enough of the errors that would be printed otherwise
	 */
	GST_WARNING ("alsalib error: %s%s%s", str, err ? ": " : "",
				   err ? snd_strerror (err) : "");
	g_free (str);
}

static gboolean
plugin_init (GstPlugin * plugin)
{
	int err;

	GST_DEBUG_CATEGORY_INIT (alsa_debug, "acmalsasink", 0, "alsa plugins");

	if (!gst_element_register (plugin, "acmalsasink", GST_RANK_PRIMARY,
							   GST_TYPE_ACM_ALSA_SINK)) {
		return FALSE;
	}

	err = snd_lib_error_set_handler (gst_alsa_error_wrapper);
	if (err != 0)
		GST_WARNING ("failed to set alsa error handler");

	return TRUE;
}

GST_PLUGIN_DEFINE (
   GST_VERSION_MAJOR,
   GST_VERSION_MINOR,
   acmalsasink,
   "ALSA plugin library",
   plugin_init,
   VERSION,
   "LGPL",
   "GStreamer ACM Plugins",
   "http://armadillo.atmark-techno.com/"
);

/*
 * End of file
 */
