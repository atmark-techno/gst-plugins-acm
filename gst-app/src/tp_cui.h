////////////////////////////////////////////////////////////////////////
// tp_cui.h: ユーザインタフェース（宣言）
//
// Author:      Nobutaka Kimura (kimura@stprec.co.jp)
// Created:     Feb. 14, 2013
// Last update: Feb. 14, 2013
/////////////////////////////////////////////////////////////////////////

#ifndef _TP_CUI_H_
#define _TP_CUI_H_

//      #########################################################       //
//      #               I N C L U D E   F I L E S               #       //
//      #########################################################       //

#include <glib.h>

//      #########################################################       //
//      #               T Y P E   D E F I N E S                 #       //
//      #########################################################       //

typedef struct _TpCui TpCui;

 /*
  * コマンドが実装された時のコールバック
  * コールバックが FALSE を返した場合、メインループから抜ける
  */
typedef gboolean (*TpCuiCallbackFunc)(gpointer data);

//      #########################################################       //
//      #             P U B L I C   F U N C T I O N S           #       //
//      #########################################################       //

TpCui* tp_cui_create(const int fd);
void tp_cui_destroy(TpCui* handle);
gboolean tp_cui_add_command(TpCui* handle, gchar key,
                            TpCuiCallbackFunc func, gpointer data);
gboolean tp_cui_run(TpCui* handle);
GMainLoop* tp_cui_get_main_loop(const TpCui* handle);

#endif // _TP_CUI_H_

//
// End of File
//
