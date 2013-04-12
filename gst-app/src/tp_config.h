////////////////////////////////////////////////////////////////////////
// tp_config.h: プロパティ設定ファイルのパース（宣言）
//
// Author:      Nobutaka Kimura (kimura@stprec.co.jp)
// Created:     Feb. 15, 2013
// Last update: Mar. 04, 2013
/////////////////////////////////////////////////////////////////////////

#ifndef _TP_CONFIG_H_
#define _TP_CONFIG_H_

//      #########################################################       //
//      #               I N C L U D E   F I L E S               #       //
//      #########################################################       //

#include "tp_property.h"

//      #########################################################       //
//      #               T Y P E   D E F I N E S                 #       //
//      #########################################################       //

typedef struct _TpConfig TpConfig;

//      #########################################################       //
//      #             P U B L I C   F U N C T I O N S           #       //
//      #########################################################       //

// TpConfig
TpConfig* tp_config_create();
void tp_config_destroy(TpConfig* tp_conf);
guint tp_config_get_count(const TpConfig* tp_conf);
void tp_config_add(TpConfig* tp_conf, const TpProperty* tp_prop);
TpProperty* tp_config_get_by_name(const TpConfig* tp_conf,
                                  const gchar* plugin_name);
TpConfig* tp_config_parse_file(const gchar* path);

#endif // _TP_CONFIG_H_

//
// End of File
//
