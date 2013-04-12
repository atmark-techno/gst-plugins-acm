////////////////////////////////////////////////////////////////////////
// tp_gst_ctrl.h: GStreamer のコントロール（宣言）
//
// Author:      Nobutaka Kimura (kimura@stprec.co.jp)
// Created:     Feb. 14, 2013
// Last update: Feb. 14, 2013
/////////////////////////////////////////////////////////////////////////

#ifndef _TP_GST_CTRL_H_
#define _TP_GST_CTRL_H_

//      #########################################################       //
//      #               I N C L U D E   F I L E S               #       //
//      #########################################################       //

//#include <glib.h>
#include <gst/gst.h>

//      #########################################################       //
//      #               T Y P E   D E F I N E S                 #       //
//      #########################################################       //

typedef struct _TpGstCtrl TpGstCtrl;

//      #########################################################       //
//      #             P U B L I C   F U N C T I O N S           #       //
//      #########################################################       //

// 初期化と解放
TpGstCtrl* tp_gst_ctrl_create(gint *argc, gchar ***argv);
void tp_gst_ctrl_destroy(TpGstCtrl* ctrl);

// パイプラインの初期化と解放
gboolean tp_gst_ctrl_setup_pipeline(TpGstCtrl* ctrl, GMainLoop* loop, const gchar* media);
void tp_gst_ctrl_cleanup_pipeline(TpGstCtrl* ctrl);

// 再生開始・停止
void tp_gst_ctrl_start(TpGstCtrl* ctrl);
void tp_gst_ctrl_stop(TpGstCtrl* ctrl);
// 一時停止・再開
void tp_gst_ctrl_pause(TpGstCtrl* ctrl);
void tp_gst_ctrl_resume(TpGstCtrl* ctrl);
// シーク
void tp_gst_ctrl_seek_forward(TpGstCtrl* ctrl);
void tp_gst_ctrl_seek_forward_ex(TpGstCtrl* ctrl, gint64 msec);
void tp_gst_ctrl_seek_backward(TpGstCtrl* ctrl);
void tp_gst_ctrl_seek_backward_ex(TpGstCtrl* ctrl, gint64 msec);
void tp_gst_ctrl_rewind(TpGstCtrl* ctrl);

// 現在の再生位置の取得
guint64 tp_gst_ctrl_query_position(TpGstCtrl* ctrl);

#endif // _TP_GST_CTRL_H_

//
// End of File
//
