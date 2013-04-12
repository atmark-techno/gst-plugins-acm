/////////////////////////////////////////////////////////////////////////
// tp_detect_media.c: メディア種別の判別（実装）
//
// Author       Nobutaka Kimura (kimura@stprec.co.jp)
// Created:     Feb. 14, 2013
// Last update: Mar. 05, 2013
/////////////////////////////////////////////////////////////////////////

//      #########################################################       //
//      #               I N C L U D E   F I L E S               #       //
//      #########################################################       //

#include "tp_detect_media.h"
#include "tp_log_util.h"

#include <gst/gst.h>

//      #########################################################       //
//      #               L O C A L   D E F I N E S               #       //
//      #########################################################       //

#define MIME_TYPE_QUICKTIME "video/quicktime"
#define MIME_TYPE_MPEGTS    "video/mpegts"

#define EXIT_IF_NULL(obj)                       \
    do {                                        \
        if (NULL == obj) {                      \
            goto ERR_EXIT;                      \
        }                                       \
    } while(0)

#define UNREF_UNLESS_NULL(obj)                  \
    do {                                        \
        if (NULL != obj) {                      \
            gst_object_unref(obj);              \
        }                                       \
    } while(0)

//      #########################################################       //
//      #               L O C A L   S T O R A G E               #       //
//      #########################################################       //

//      #########################################################       //
//      #            P R I V A T E   F U N C T I O N S          #       //
//      #########################################################       //

static void
have_type_cb (GstElement * typefind, guint probability,
              const GstCaps * caps, GstCaps ** p_caps)
{
    if (p_caps) {
        *p_caps = gst_caps_copy (caps);
    }
}

//      #########################################################       //
//      #             P U B L I C   F U N C T I O N S           #       //
//      #########################################################       //

/**
 * メディアの種別を判定する
 * GStreamer が初期化済みであることが前提
 * @param[in] path メディアのパス
 * @return メディア種別
 */
TpMediaType
tp_detect_media_type (const gchar * path)
{
    TpMediaType ret = MEDIA_TYPE_UNKNOWN;
    GstElement *pipeline = NULL, *source = NULL, *typefind = NULL, *fakesink =
        NULL;
    GstStateChangeReturn stateRet;
    GstState state;
    GstCaps *caps = NULL;

    if (NULL == path) {
        return MEDIA_TYPE_UNKNOWN;
    }

    if (!gst_is_initialized ()) {
        TP_LOG_ERROR ("Gstreamer is not initialized\n");
        return MEDIA_TYPE_UNKNOWN;
    }

    pipeline = gst_pipeline_new ("pipeline");
    EXIT_IF_NULL (pipeline);

    source = gst_element_factory_make ("filesrc", "source");
    EXIT_IF_NULL (source);
    g_object_set (source, "location", path, NULL);

    typefind = gst_element_factory_make ("typefind", "typefind");
    EXIT_IF_NULL (typefind);

    fakesink = gst_element_factory_make ("fakesink", "fakesink");
    EXIT_IF_NULL (fakesink);

    gst_bin_add_many (GST_BIN (pipeline), source, typefind, fakesink, NULL);
    gst_element_link_many (source, typefind, fakesink, NULL);

    g_signal_connect (G_OBJECT (typefind), "have-type",
                      G_CALLBACK (have_type_cb), &caps);

    gst_element_set_state (GST_ELEMENT (pipeline), GST_STATE_PAUSED);
    stateRet = gst_element_get_state (GST_ELEMENT (pipeline), &state,
                                      NULL, GST_CLOCK_TIME_NONE);

    switch (stateRet) {
        case GST_STATE_CHANGE_SUCCESS:
            if (caps) {
                gchar *caps_str;
                caps_str = gst_caps_to_string (caps);

                if (NULL != g_strstr_len (caps_str, -1, MIME_TYPE_QUICKTIME)) {
                    ret = MEDIA_TYPE_QUICKTIME;
                } else if (NULL != g_strstr_len (caps_str, -1, MIME_TYPE_MPEGTS)) {
                    ret = MEDIA_TYPE_MPEGTS;
                } else {
                    ret = MEDIA_TYPE_UNKNOWN;
                }

                g_free (caps_str);
                gst_caps_unref (caps);
            } else {
                ret = MEDIA_TYPE_UNKNOWN;
            }
            break;
        case GST_STATE_CHANGE_FAILURE:
            TP_LOG_DEBUG ("failed to gst_element_get_state()\n");
            break;
        default:
            // ここには来ないはず
            break;
    }

    gst_element_set_state (pipeline, GST_STATE_NULL);
    gst_object_unref (pipeline);
    return ret;

ERR_EXIT:
    UNREF_UNLESS_NULL (pipeline);
    UNREF_UNLESS_NULL (source);
    UNREF_UNLESS_NULL (typefind);
    UNREF_UNLESS_NULL (fakesink);

    return MEDIA_TYPE_UNKNOWN;
}

//
// End of File
//
