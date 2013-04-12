/////////////////////////////////////////////////////////////////////////
// tp_gst_ctrl.c: GStreamer のコントロール（実装）
//
// Author       Nobutaka Kimura (kimura@stprec.co.jp)
// Created:     Feb. 14, 2013
// Last update: Mar. 02, 2013
/////////////////////////////////////////////////////////////////////////

//      #########################################################       //
//      #               I N C L U D E   F I L E S               #       //
//      #########################################################       //

#include "tp_gst_ctrl.h"
#include "tp_detect_media.h"
#include "tp_config.h"
#include "tp_log_util.h"

#include <gst/gst.h>
#include <glib/gstdio.h>
#include <glib/gprintf.h>

//      #########################################################       //
//      #               L O C A L   D E F I N E S               #       //
//      #########################################################       //

#define VERBOSE_LOG

/* プラグインのプロパティ設定ファイル */
#define PROPERTY_FILE_NAME "properties.conf"

/* factory name  */
#define FILESRC        "filesrc"
#define QT_DEMUX       "qtdemux"
#define QUEUE          "queue"
#define AAC_PARSE      "aacparse"
#define AUDIO_CONVERT  "audioconvert"
#define AUDIO_RESAMPLE "audioresample"
#define H264_PARSE     "h264parse"
#define CAPS_FILTER     "capsfilter"

#ifdef _USE_SW_DECODER
# define AAC_DECODER     "faad"          /* SW AAC デコーダ */
# define AUDIO_SINK      "alsasink"
# define H264_DECODER    "avdec_h264"    /* SW H264 デコーダ */
# define VIDEO_SINK		 "autovideosink" /* video sink */
# define TS_DEMUX        "tsdemux"
#else
# define AAC_DECODER     "rtoaacdec"     /* HW AAC デコーダ */
# define AUDIO_SINK      "acmalsasink"
# define H264_DECODER    "rtoh264dec"    /* HW H264 デコーダ */
# define VIDEO_SINK      "rtofbdevsink"  /* video sink */
# define TS_DEMUX        "acmtsdemux"
#endif // _USE_SW_DECODER

/* element name */
#define FILESRC_ELEM_NAME            FILESRC
#define QT_DEMUX_ELEM_NAME           QT_DEMUX
#define TS_DEMUX_ELEM_NAME           TS_DEMUX
#define PRE_ADEC_QUEUE_ELEM_NAME     "pre-adec-queue"
#define POST_ADEC_QUEUE_ELEM_NAME    "post-adec-queue"
#define AAC_PARSE_ELEM_NAME          AAC_PARSE
#define AUDIO_CONVERT_ELEM_NAME      AUDIO_CONVERT
#define AUDIO_RESAMPLE_ELEM_NAME     AUDIO_RESAMPLE
#define AAC_DECODER_ELEM_NAME        AAC_DECODER
#define AUDIO_SINK_ELEM_NAME         AUDIO_SINK
#define PRE_VDEC_QUEUE_ELEM_NAME     "pre-vdec-queue"
#define POST_VDEC_QUEUE_ELEM_NAME    "post-vdec-queue"
#define H264_PARSE_ELEM_NAME         H264_PARSE
#define H264_DECODER_ELEM_NAME       H264_DECODER
#define VIDEO_SINK_ELEM_NAME         VIDEO_SINK
#define AUDIO_CAPS_FILTER            "audio-capsfilter"

/* デフォルトのシーク間隔(msec) */
#define DEFAULT_SEEK_INTERVAL_MSEC (5 * 1000)

#define EXIT_IF_NULL(obj)                       \
    do {                                        \
        if (NULL == obj) {                      \
            goto ERR_EXIT;                      \
        }                                       \
    } while(0)

#define UNREF_UNLESS_NULL(obj)                  \
    do {                                        \
        if (NULL != obj) {                      \
            gst_object_unref(GST_OBJECT(obj));  \
        }                                       \
    } while(0)

// テストの条件
typedef struct _TestCondition
{
    gboolean audio;               /* 音声のデコードをテストするかどうか */
    gboolean video;               /* 映像のデコードをテストするかどうか */
    gboolean queue;               /* デコーダの前段に queue を入れるかどうか */
} TestCondition;

typedef struct _GstElementTuple
{
    GstElement *audio;
    GstElement *video;
} GstElementTuple;

typedef struct _SetPropertyArg
{
    GstElement *elem;
    const gchar *plugin_name;
} SetPropertyArg;

struct _TpGstCtrl
{
    GstElement *pipeline;
    GMainLoop *main_loop;
    guint bus_watch_id;
    GstElementTuple tuple;
    TestCondition test_condition;
    gint64 seek_interval_ms;      // シーク間隔(msec)
};

//      #########################################################       //
//      #               L O C A L   S T O R A G E               #       //
//      #########################################################       //

//      #########################################################       //
//      #            P R I V A T E   F U N C T I O N S          #       //
//      #########################################################       //

#ifdef VERBOSE_LOG
static gchar *
get_chain_string (GstElement * elem)
{
    GstIterator *pad_iter;
    gboolean done;
    GValue item = G_VALUE_INIT;
    GstPad *pad = NULL;
    gchar *ret_str = NULL;
    gchar *upstream_str = NULL;

    pad_iter = gst_element_iterate_sink_pads (elem);
    done = FALSE;
    while (!done) {
        switch (gst_iterator_next (pad_iter, &item)) {
            case GST_ITERATOR_OK:
                pad = g_value_get_object (&item);
                if (pad) {
                    GstPad *peerPad = gst_pad_get_peer (pad);
                    if (peerPad) {
                        GstElement *peerElem = gst_pad_get_parent_element (peerPad);
                        upstream_str = get_chain_string (peerElem);
                    }
                }
                g_value_reset (&item);
                break;
            case GST_ITERATOR_RESYNC:
                gst_iterator_resync (pad_iter);
                break;
            case GST_ITERATOR_ERROR:
            case GST_ITERATOR_DONE:
                done = TRUE;
                break;
        }
    }

    g_value_unset (&item);
    gst_iterator_free (pad_iter);

    if (NULL == upstream_str) {
        ret_str = g_strdup (G_OBJECT_TYPE_NAME (GST_OBJECT (elem)));
    } else {
        ret_str = g_strdup_printf ("%s ! %s", upstream_str,
                                   G_OBJECT_TYPE_NAME (GST_OBJECT (elem)));
        g_free (upstream_str);
    }

    return ret_str;
}

static void
print_pipeline_info (GstBin * bin)
{
    GstIterator *it;
    gboolean done;
    GstElement *elem;
    GValue item = G_VALUE_INIT;
    gchar *elem_str;
    gint count = 0;

    it = gst_bin_iterate_sinks (bin);
    done = FALSE;
    while (!done) {
        switch (gst_iterator_next (it, &item)) {
            case GST_ITERATOR_OK:
                elem = g_value_get_object (&item);
                elem_str = get_chain_string (elem);
                if (elem_str) {
                    TP_LOG_INFO ("stream%d %s\n", count++, elem_str);
                    g_free (elem_str);
                }
                g_value_reset (&item);
                break;
            case GST_ITERATOR_RESYNC:
                gst_iterator_resync (it);
                break;
            case GST_ITERATOR_ERROR:
            case GST_ITERATOR_DONE:
                done = TRUE;
                break;
        }
    }

    g_value_unset (&item);
    gst_iterator_free (it);
}
#endif // VERBOSE_LOG

/**
 * プラグインのプロパティを設定する
 */
static void
set_property_cb (gpointer name, gpointer value, gpointer data)
{
    gchar *prop_name = (gchar *) name;
    GValue *prop_value = (GValue *) value;
    SetPropertyArg *arg = (SetPropertyArg *) data;
    GstElement *elem = arg->elem;
    const gchar *plugin_name = arg->plugin_name;

    if (0 == g_strcmp0 (AUDIO_CAPS_FILTER, plugin_name) &&
        0 == g_strcmp0 ("caps", prop_name)) {
        GstCaps* caps;
        if (G_TYPE_STRING != G_VALUE_TYPE (prop_value)) {
            TP_LOG_WARN ("invalid property %s@%s\n", plugin_name, prop_name);
            return;
        }
        caps = gst_caps_from_string (g_value_get_string(prop_value));
        g_object_set (G_OBJECT (elem), prop_name, caps, NULL);
        gst_caps_unref (caps);
    }
    else {
        g_object_set_property (G_OBJECT (elem), prop_name, prop_value);
    }
#ifdef VERBOSE_LOG
    {
        gchar *valStr = g_strdup_value_contents (prop_value);
        TP_LOG_INFO ("%s@%s=%s\n", plugin_name, prop_name, valStr);
        g_free (valStr);
    }
#endif // VERBOSE_LOG
}

static void
set_plugin_properties (const gchar * plugin_name, GstElement * plugin,
                       const TpConfig * conf)
{
    SetPropertyArg arg;
    TpProperty *tp_prop = tp_config_get_by_name (conf, plugin_name);

    arg.elem = plugin;
    arg.plugin_name = plugin_name;
    tp_property_foreach (tp_prop, set_property_cb, &arg);
}

/**
 * バス監視用コールバック
 */
static gboolean
bus_watch_cb (GstBus * bus, GstMessage * msg, gpointer data)
{
    TpGstCtrl *ctrl = (TpGstCtrl *) data;
    GMainLoop *loop = ctrl->main_loop;

    switch (GST_MESSAGE_TYPE (msg)) {
#ifdef VERBOSE_LOG
        case GST_MESSAGE_STATE_CHANGED:
            if (GST_MESSAGE_SRC (msg) == GST_OBJECT_CAST (ctrl->pipeline)) {
                GstState old, new, pending;
                gst_message_parse_state_changed (msg, &old, &new, &pending);
                if (GST_STATE_READY == old && GST_STATE_PAUSED == new) {
                    print_pipeline_info (GST_BIN (ctrl->pipeline));
                }
            }
            break;
#endif // VERBOSE_LOG
        case GST_MESSAGE_EOS:
            TP_LOG_INFO ("end of stream\n");
            g_main_loop_quit (loop);
            break;
        case GST_MESSAGE_ERROR:{
            gchar *debug;
            GError *error;

            gst_message_parse_error (msg, &error, &debug);
            g_free (debug);

            TP_LOG_ERROR ("%s\n", error->message);
            g_error_free (error);

            g_main_loop_quit (loop);
            break;
        }
        default:
            break;
    }

    return TRUE;
}

static void
demux_pad_added_cb (GstElement * element, GstPad * pad, gpointer user_data)
{
    GstCaps *caps;
    GstStructure *str;
    GstElementTuple *tuple = (GstElementTuple *) user_data;

    caps = gst_pad_get_current_caps (pad);
    g_assert (caps != NULL);
    str = gst_caps_get_structure (caps, 0);
    g_assert (str != NULL);

    const gchar *c = gst_structure_get_name (str);
    if ((NULL != tuple->video) &&
        ((g_strrstr (c, "video") || g_strrstr (c, "image")))) {
        GstPad *targetsink = gst_element_get_static_pad (tuple->video, "sink");
        g_assert (targetsink != NULL);
        gst_pad_link (pad, targetsink);
        gst_object_unref (targetsink);
    }

    if ((NULL != tuple->audio) && g_strrstr (c, "audio")) {
        GstPad *targetsink = gst_element_get_static_pad (tuple->audio, "sink");
        g_assert (targetsink != NULL);
        gst_pad_link (pad, targetsink);
        gst_object_unref (targetsink);
    }

    gst_caps_unref (caps);
}

// 音声用
static gboolean
setup_audio_elements (GstElement * pipeline, gboolean use_pre_queue,
                      GstElement ** head_element, const TpConfig * conf)
{
    GstElement *pre_queue = NULL, *aparse = NULL, *adec = NULL,
        *aconvert = NULL, *resample = NULL, *capsfilter = NULL,
        *asink = NULL;

    // 出力初期化
    *head_element = NULL;

    if (use_pre_queue) {
        pre_queue = gst_element_factory_make (QUEUE, PRE_ADEC_QUEUE_ELEM_NAME);
        EXIT_IF_NULL (pre_queue);
        set_plugin_properties (PRE_ADEC_QUEUE_ELEM_NAME, pre_queue, conf);
    }

    aparse = gst_element_factory_make (AAC_PARSE, AAC_PARSE_ELEM_NAME);
    EXIT_IF_NULL (aparse);
    set_plugin_properties (AAC_PARSE_ELEM_NAME, aparse, conf);

    adec = gst_element_factory_make (AAC_DECODER, AAC_DECODER_ELEM_NAME);
    EXIT_IF_NULL (adec);
    set_plugin_properties (AAC_DECODER_ELEM_NAME, adec, conf);

    aconvert = gst_element_factory_make (AUDIO_CONVERT, AUDIO_CONVERT_ELEM_NAME);
    EXIT_IF_NULL (aconvert);
    set_plugin_properties (AUDIO_CONVERT_ELEM_NAME, aconvert, conf);

    resample =
        gst_element_factory_make (AUDIO_RESAMPLE, AUDIO_RESAMPLE_ELEM_NAME);
    EXIT_IF_NULL (resample);
    set_plugin_properties (AUDIO_RESAMPLE_ELEM_NAME, resample, conf);

    capsfilter = gst_element_factory_make (CAPS_FILTER, AUDIO_CAPS_FILTER);
    EXIT_IF_NULL (capsfilter);
    set_plugin_properties (AUDIO_CAPS_FILTER, capsfilter, conf);

    asink = gst_element_factory_make (AUDIO_SINK, AUDIO_SINK_ELEM_NAME);
    EXIT_IF_NULL (asink);
    set_plugin_properties (AUDIO_SINK_ELEM_NAME, asink, conf);

    // エレメントをパイプラインに追加
    if (use_pre_queue) {
        gst_bin_add (GST_BIN (pipeline), pre_queue);
    }
#if 0
    gst_bin_add_many (GST_BIN (pipeline), aparse, adec, aconvert, resample,
                      capsfilter, asink, NULL);
#else
    gst_bin_add_many (GST_BIN (pipeline), aparse, adec, resample,
                      capsfilter, asink, NULL);
#endif
	
    // リンク
    if (use_pre_queue) {
        *head_element = pre_queue;
        gst_element_link (pre_queue, aparse);
    } else {
        *head_element = aparse;
    }
#if 0
    gst_element_link_many (aparse, adec, aconvert, resample, capsfilter, asink, NULL);
#else
    gst_element_link_many (aparse, adec, resample, capsfilter, asink, NULL);
#endif

    TP_LOG_INFO ("audio stream enabled\n");
    return TRUE;

ERR_EXIT:
    if (use_pre_queue) {
        UNREF_UNLESS_NULL (pre_queue);
    }
    UNREF_UNLESS_NULL (aparse);
    UNREF_UNLESS_NULL (adec);
    UNREF_UNLESS_NULL (aconvert);
    UNREF_UNLESS_NULL (resample);
    UNREF_UNLESS_NULL (capsfilter);
    UNREF_UNLESS_NULL (asink);

    return FALSE;
}

// 映像ストリームのエレメント生成
static gboolean
setup_video_elements (GstElement * pipeline, gboolean use_pre_queue,
                      GstElement ** head_element, const TpConfig * conf)
{
    GstElement *pre_queue = NULL, *post_queue = NULL, *vdec = NULL,
        *vsink = NULL, *vparse = NULL;

    // 出力初期化
    *head_element = NULL;

    if (use_pre_queue) {
        pre_queue = gst_element_factory_make (QUEUE, PRE_VDEC_QUEUE_ELEM_NAME);
        EXIT_IF_NULL (pre_queue);
        set_plugin_properties (PRE_VDEC_QUEUE_ELEM_NAME, pre_queue, conf);
    }

    post_queue = gst_element_factory_make (QUEUE, POST_VDEC_QUEUE_ELEM_NAME);
    EXIT_IF_NULL (post_queue);
    set_plugin_properties (POST_VDEC_QUEUE_ELEM_NAME, post_queue, conf);

    vparse = gst_element_factory_make (H264_PARSE, H264_PARSE_ELEM_NAME);
    EXIT_IF_NULL (vparse);
    set_plugin_properties (H264_PARSE_ELEM_NAME, vparse, conf);

    vdec = gst_element_factory_make (H264_DECODER, H264_DECODER_ELEM_NAME);
    EXIT_IF_NULL (vdec);
    set_plugin_properties (H264_DECODER_ELEM_NAME, vdec, conf);

    vsink = gst_element_factory_make (VIDEO_SINK, VIDEO_SINK_ELEM_NAME);
    EXIT_IF_NULL (vsink);
    set_plugin_properties (VIDEO_SINK_ELEM_NAME, vsink, conf);

    // エレメントをパイプラインに追加
    if (use_pre_queue) {
        gst_bin_add (GST_BIN (pipeline), pre_queue);
    }
    gst_bin_add_many (GST_BIN (pipeline), vparse, vdec,
                      vsink, NULL);

    // リンク
    if (use_pre_queue) {
        *head_element = pre_queue;
        gst_element_link (pre_queue, vparse);
    } else {
        *head_element = vparse;
    }

    gst_element_link_many (vparse, vdec, vsink, NULL);

    TP_LOG_INFO ("video stream enabled\n");

    return TRUE;

ERR_EXIT:
    UNREF_UNLESS_NULL (vparse);
    UNREF_UNLESS_NULL (pre_queue);;
    UNREF_UNLESS_NULL (post_queue);
    UNREF_UNLESS_NULL (vdec);
    UNREF_UNLESS_NULL (vsink);
    return FALSE;
}

/**
 * filesrc@location を取得する
 */
static const gchar *
tp_config_get_filesrc_location (const TpConfig * conf)
{
    TpProperty *tp_prop = tp_config_get_by_name (conf, FILESRC);
    GValue *val = tp_property_get_by_name (tp_prop, "location");

    if (val) {
        return g_value_get_string (val);
    } else {
        return NULL;
    }
}

/**
 * テスト条件に合わせてパイプラインを組み立てる
 */
static gboolean
tp_gst_ctrl_setup_elements (TpGstCtrl * ctrl, const gchar * path)
{
    GstElement *source = NULL, *demuxer = NULL;
    const gchar *demuxerName = NULL;
    TpConfig *conf = NULL;
    const gchar *mediaPath;

    // プロパティファイルが存在すれば読み込む
    if (g_file_test (PROPERTY_FILE_NAME, G_FILE_TEST_IS_REGULAR)) {
        conf = tp_config_parse_file (PROPERTY_FILE_NAME);
        if (conf) {
            TP_LOG_INFO ("%s loaded\n", PROPERTY_FILE_NAME);
        } else {
            TP_LOG_ERROR ("failed to load %s\n", PROPERTY_FILE_NAME);
            goto ERR_EXIT;
        }
    }
    // エレメント生成
    source = gst_element_factory_make (FILESRC, NULL);
    EXIT_IF_NULL (source);

    mediaPath = tp_config_get_filesrc_location (conf);
    if (NULL == mediaPath) {
        mediaPath = path;
    }
    if (NULL == mediaPath) {
        TP_LOG_ERROR ("media path not specified\n");
        goto ERR_EXIT;
    }
    switch (tp_detect_media_type (mediaPath)) {
        case MEDIA_TYPE_QUICKTIME:
            TP_LOG_INFO ("media = %s, type = QUICKTIME\n", mediaPath);
            demuxerName = QT_DEMUX;
            break;
        case MEDIA_TYPE_MPEGTS:
            TP_LOG_INFO ("media = %s, type = MPEGTS\n", mediaPath);
            demuxerName = TS_DEMUX;
            break;
        default:
            TP_LOG_ERROR ("unsupported media type: %s\n", mediaPath);
            goto ERR_EXIT;
            break;
    }

    demuxer = gst_element_factory_make (demuxerName, NULL);
    EXIT_IF_NULL (demuxer);

    // メディアのパスは propeties.conf の内容を優先
    g_object_set (G_OBJECT (source), "location", mediaPath, NULL);
    set_plugin_properties (FILESRC, source, conf);

    gst_bin_add_many (GST_BIN (ctrl->pipeline), source, demuxer, NULL);

    if (ctrl->test_condition.queue) {
        TP_LOG_INFO ("pre-decoder queue enabled\n");
    }
    // 音声ストリーム用パイプラインの作成
    if (ctrl->test_condition.audio &&
        !setup_audio_elements (ctrl->pipeline,
                               ctrl->test_condition.queue, &ctrl->tuple.audio, conf)) {
        TP_LOG_ERROR ("failed to create audio stream pipeline\n");
        goto ERR_EXIT;
    }
    // 映像ストリーム用パイプラインの作成
    if (ctrl->test_condition.video &&
        !setup_video_elements (ctrl->pipeline, ctrl->test_condition.queue,
                               &ctrl->tuple.video, conf)) {
        TP_LOG_ERROR ("failed to create video stream pipeline\n");
        goto ERR_EXIT;
    }
    // エレメントをリンク
    gst_element_link (source, demuxer);
    g_signal_connect (demuxer, "pad-added", G_CALLBACK (demux_pad_added_cb),
                      (gpointer) & ctrl->tuple);

    tp_config_destroy (conf);
    return TRUE;

ERR_EXIT:
    UNREF_UNLESS_NULL (source);
    UNREF_UNLESS_NULL (demuxer);
    tp_config_destroy (conf);
    return FALSE;
}

//      #########################################################       //
//      #             P U B L I C   F U N C T I O N S           #       //
//      #########################################################       //

// 初期化と解放

/**
 * TpGstCtrl の初期化
 * @param[in]  argc     引数の個数
 * @param[in]  argv     引数の文字列の配列
 * @return TpGstCtrl オブジェクト
 */
TpGstCtrl *
tp_gst_ctrl_create (gint * argc, gchar *** argv)
{
    TpGstCtrl *ctrl = NULL;
    TestCondition test_condition = { FALSE, FALSE, FALSE };
    GOptionContext *ctx = NULL;
    GError *err = NULL;
    gint64 seek_interval = DEFAULT_SEEK_INTERVAL_MSEC;
    GOptionEntry entries[] = {
        {"audio", 'a', 0, G_OPTION_ARG_NONE, &test_condition.audio,
         "enable audio stream", NULL}
        ,
        {"video", 'v', 0, G_OPTION_ARG_NONE, &test_condition.video,
         "enable video stream", NULL}
        ,
        {"queue", 'q', 0, G_OPTION_ARG_NONE, &test_condition.queue,
         "insert queue element before decoder", NULL}
        ,
        {"seek-interval", 's', 0, G_OPTION_ARG_INT64, &seek_interval,
         "seek interval in msec (default=5000)", "INTERVAL"}
        ,
        {NULL, 0, 0, G_OPTION_ARG_NONE, NULL, NULL, 0}
    };

    if (NULL == argc || NULL == argv) {
        TP_LOG_ERROR ("invalid arguments\n");
        return FALSE;
    }
    // コマンドオプションのパースと GStreamer の初期化
    ctx = g_option_context_new ("filepath");
    g_option_context_add_main_entries (ctx, entries, NULL);
    g_option_context_add_group (ctx, gst_init_get_option_group ());
    if (!g_option_context_parse (ctx, argc, argv, &err)) {
        TP_LOG_ERROR ("%s\n", err->message);
        g_option_context_free (ctx);
        g_error_free (err);
        return NULL;
    }
    g_option_context_free (ctx);

    // コマンドオプションの検査
    if (!test_condition.audio && !test_condition.video) {
        TP_LOG_ERROR ("at least one of '--audio' and '--video' is required\n");
        return NULL;
    }

    ctrl = (TpGstCtrl *) g_malloc (sizeof (TpGstCtrl));
    if (NULL == ctrl) {
        TP_LOG_ERROR ("out of memory\n");
        return NULL;
    }
    ctrl->pipeline = NULL;
    ctrl->main_loop = NULL;
    ctrl->bus_watch_id = 0;
    ctrl->tuple.audio = NULL;
    ctrl->tuple.video = NULL;
    ctrl->test_condition = test_condition;
    ctrl->seek_interval_ms = seek_interval;

    return ctrl;
}

/**
 * TpGstCtrl オブジェクトの解放
 * @param[in] ctrl TpGstCtrl オブジェクト
 */
void
tp_gst_ctrl_destroy (TpGstCtrl * ctrl)
{
    if (NULL != ctrl) {
        tp_gst_ctrl_cleanup_pipeline (ctrl);
        g_free (ctrl);
    }
}

// パイプラインの初期化と解放
/**
 * パイプラインの初期化
 * @param[in] ctrl TpGstCtrl オブジェクト
 * @param[in] loop メインループ
 * @param[in] media 再生対象メディアのパス（NULLも可）
 * @return 成功したか否か
 */
gboolean
tp_gst_ctrl_setup_pipeline (TpGstCtrl * ctrl, GMainLoop * loop,
                            const gchar * media)
{
    GstBus *bus;

    if (NULL == ctrl || NULL == loop) {
        return FALSE;
    }

    if (NULL != ctrl->pipeline) {
        tp_gst_ctrl_cleanup_pipeline (ctrl);
    }
    // パイプラインの作成
    if (NULL == (ctrl->pipeline = gst_pipeline_new ("test-player"))) {
        TP_LOG_ERROR ("failed to create pipeline\n");
        return FALSE;
    }
    bus = gst_pipeline_get_bus (GST_PIPELINE (ctrl->pipeline));
    ctrl->main_loop = loop;
    ctrl->bus_watch_id = gst_bus_add_watch (bus, bus_watch_cb, ctrl);
    gst_object_unref (bus);

    // テスト条件に従いエレメントをセットアップ
    if (!tp_gst_ctrl_setup_elements (ctrl, media)) {
        tp_gst_ctrl_cleanup_pipeline (ctrl);
        return FALSE;
    }

    return TRUE;
}

/**
 * パイプラインの解放
 * @param[in] ctrl TpGstCtrl オブジェクト
 */
void
tp_gst_ctrl_cleanup_pipeline (TpGstCtrl * ctrl)
{
    if (NULL != ctrl && NULL != ctrl->pipeline) {
        gst_object_unref (GST_OBJECT (ctrl->pipeline));
        g_source_remove (ctrl->bus_watch_id);
        ctrl->pipeline = NULL;
        ctrl->bus_watch_id = 0;
    }
}

// 再生開始・停止
/**
 * 再生スタート
 * @param[in] ctrl TpGstCtrl オブジェクト
 */
void
tp_gst_ctrl_start (TpGstCtrl * ctrl)
{
    if (NULL != ctrl && NULL != ctrl->pipeline) {
        TP_LOG_INFO ("playback start\n");
        gst_element_set_state (ctrl->pipeline, GST_STATE_PLAYING);
    }
}

/**
 * 再生ストップ
 * @param[in] ctrl TpGstCtrl オブジェクト
 */
void
tp_gst_ctrl_stop (TpGstCtrl * ctrl)
{
    if (NULL != ctrl && NULL != ctrl->pipeline) {
        TP_LOG_INFO ("playback stop\n");
        gst_element_set_state (ctrl->pipeline, GST_STATE_NULL);
    }
}

// 一時停止・再開
/**
 * 一時停止状態にする
 * @param[in] ctrl TpGstCtrl オブジェクト
 */
void
tp_gst_ctrl_pause (TpGstCtrl * ctrl)
{
    if (NULL != ctrl && NULL != ctrl->pipeline) {
        TP_LOG_INFO ("pause\n");
        gst_element_set_state (ctrl->pipeline, GST_STATE_PAUSED);
    }
}

/**
 * 再生を再開する
 * @param[in] ctrl TpGstCtrl オブジェクト
 */
void
tp_gst_ctrl_resume (TpGstCtrl * ctrl)
{
    if (NULL != ctrl && NULL != ctrl->pipeline) {
        TP_LOG_INFO ("resume\n");
        gst_element_set_state (ctrl->pipeline, GST_STATE_PLAYING);
    }
}

// シーク
/**
 * 前方へ seek する
 * @param[in] ctrl TpGstCtrl オブジェクト
 */
void
tp_gst_ctrl_seek_forward (TpGstCtrl * ctrl)
{
    tp_gst_ctrl_seek_forward_ex (ctrl, ctrl->seek_interval_ms);
}

/**
 * 前方へ seek する
 * @param[in] ctrl TpGstCtrl オブジェクト
 * @param[in] msec seek する時間(msec)
 */
void
tp_gst_ctrl_seek_forward_ex (TpGstCtrl * ctrl, gint64 msec)
{
    if (NULL != ctrl && NULL != ctrl->pipeline) {
        gint64 pos = tp_gst_ctrl_query_position (ctrl);
        pos += msec * GST_MSECOND;
        TP_LOG_INFO ("seek forward, pos = %" G_GINT64_FORMAT " msec\n",
                     pos / GST_MSECOND);
        gst_element_seek_simple (ctrl->pipeline, GST_FORMAT_TIME,
                                 GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT, pos);
    }
}

/**
 * 後方へ seek する
 * @param[in] ctrl TpGstCtrl オブジェクト
 */
void
tp_gst_ctrl_seek_backward (TpGstCtrl * ctrl)
{
    tp_gst_ctrl_seek_backward_ex (ctrl, ctrl->seek_interval_ms);
}

/**
 * 後方へ seek する
 * @param[in] ctrl TpGstCtrl オブジェクト
 * @param[in] msec seek する時間(msec)
 */
void
tp_gst_ctrl_seek_backward_ex (TpGstCtrl * ctrl, gint64 msec)
{
    if (NULL != ctrl && NULL != ctrl->pipeline) {
        gint64 pos = tp_gst_ctrl_query_position (ctrl);
        pos -= msec * GST_MSECOND;
        if (pos < 0) {
            pos = 0;
        }
        TP_LOG_INFO ("seek backward, pos = %" G_GINT64_FORMAT " msec\n",
                     pos / GST_MSECOND);
        gst_element_seek_simple (ctrl->pipeline, GST_FORMAT_TIME,
                                 GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT, pos);
    }
}

/**
 * ファイルの先頭に戻る
 * @param[in] ctrl TpGstCtrl オブジェクト
 */
void
tp_gst_ctrl_rewind (TpGstCtrl * ctrl)
{
    if (NULL != ctrl && NULL != ctrl->pipeline) {
        TP_LOG_INFO ("rewind, pos = 0 msec\n");
        gst_element_seek_simple (ctrl->pipeline,
                                 GST_FORMAT_TIME, GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT, 0);
    }
}

/**
 * 現在の再生位置の取得
 * @param[in] ctrl TpGstCtrl オブジェクト
 * @return 現在の再生位置(nanosecond)
 */
guint64
tp_gst_ctrl_query_position (TpGstCtrl * ctrl)
{
    gint64 pos = 0;

    if (NULL != ctrl && ctrl->pipeline) {
        if (! gst_element_query_position (ctrl->pipeline, GST_FORMAT_TIME, &pos)) {
			TP_LOG_ERROR("Failed gst_element_query_position\n");
		}
        TP_LOG_INFO ("query_position : %" G_GINT64_FORMAT " msec\n",
					 pos / GST_MSECOND);
    }

    return pos;
}

//
// End of File
//
