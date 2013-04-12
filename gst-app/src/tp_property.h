////////////////////////////////////////////////////////////////////////
// tp_property.h: プラグインのプロパティ
//
// Author:      Nobutaka Kimura (kimura@stprec.co.jp)
// Created:     Mar. 01, 2013
// Last update: Mar. 01, 2013
/////////////////////////////////////////////////////////////////////////

#ifndef _TP_PROPERTY_H_
#define _TP_PROPERTY_H_

//      #########################################################       //
//      #               I N C L U D E   F I L E S               #       //
//      #########################################################       //

#include <glib-object.h>

//      #########################################################       //
//      #               T Y P E   D E F I N E S                 #       //
//      #########################################################       //

typedef struct _TpProperty TpProperty;


typedef void (*TpPropertyCallback)(gpointer name, gpointer value, gpointer data);

//      #########################################################       //
//      #             P U B L I C   F U N C T I O N S           #       //
//      #########################################################       //

// TpProperty
TpProperty* tp_property_create(const gchar* plugin_name);
void tp_property_destroy(TpProperty* tp_prop);
const gchar* tp_property_get_plugin_name(const TpProperty* tp_prop);
guint tp_property_get_count(const TpProperty* tp_prop);
void tp_property_add(const TpProperty* tp_prop,
                     const gchar* prop_name, const GValue* prop_value);
GValue* tp_property_get_by_name(const TpProperty* tp_prop,
                                const gchar* prop_name);
void tp_property_foreach(const TpProperty* tp_prop,
                         TpPropertyCallback func, gpointer data);

#endif // _TP_PROPERTY_H_

//
// End of File
//
