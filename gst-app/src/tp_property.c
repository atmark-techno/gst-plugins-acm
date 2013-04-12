/////////////////////////////////////////////////////////////////////////
// tp_property.c: プラグインのプロパティ
//
// Author       Nobutaka Kimura (kimura@stprec.co.jp)
// Created:     Mar. 01, 2013
// Last update: Mar. 01, 2013
/////////////////////////////////////////////////////////////////////////

//      #########################################################       //
//      #               I N C L U D E   F I L E S               #       //
//      #########################################################       //

#include "tp_property.h"
#include "tp_log_util.h"

//      #########################################################       //
//      #               L O C A L   D E F I N E S               #       //
//      #########################################################       //

struct _TpProperty
{
    gchar *plugin_name;
    GHashTable *items;
};

//      #########################################################       //
//      #               L O C A L   S T O R A G E               #       //
//      #########################################################       //

//      #########################################################       //
//      #            P R I V A T E   F U N C T I O N S          #       //
//      #########################################################       //

static void
gvalue_destroy_and_free (gpointer data)
{
    GValue *val = (GValue *) data;

    if (NULL == val) {
        return;
    }

    if (G_IS_VALUE (val))
        g_value_unset (val);
    g_free (val);
}

//      #########################################################       //
//      #             P U B L I C   F U N C T I O N S           #       //
//      #########################################################       //

// TpProperty
TpProperty *
tp_property_create (const gchar * plugin_name)
{
    TpProperty *tp_prop;

    if (NULL == plugin_name) {
        return NULL;
    }

    tp_prop = (TpProperty *) g_malloc (sizeof (TpProperty));
    if (NULL == tp_prop) {
        TP_LOG_ERROR ("out of memory\n");
        return NULL;
    }

    tp_prop->plugin_name = g_strdup (plugin_name);
    tp_prop->items = g_hash_table_new_full (g_str_hash, g_str_equal,
                                            g_free, gvalue_destroy_and_free);

    return tp_prop;
}

void
tp_property_destroy (TpProperty * tp_prop)
{
    if (NULL != tp_prop) {
        g_hash_table_destroy (tp_prop->items);
        g_free (tp_prop->plugin_name);
        g_free (tp_prop);
    }
}

const gchar *
tp_property_get_plugin_name (const TpProperty * tp_prop)
{
    return (NULL != tp_prop) ? tp_prop->plugin_name : NULL;
}

guint
tp_property_get_count (const TpProperty * tp_prop)
{
    return (NULL != tp_prop) ? g_hash_table_size (tp_prop->items) : 0;
}

void
tp_property_add (const TpProperty * tp_prop,
                 const gchar * prop_name, const GValue * prop_value)
{
    GValue *val;

    if (NULL == tp_prop || NULL == prop_name || NULL == prop_value) {
        return;
    }

    val = (GValue *) g_malloc0 (sizeof (GValue));
    if (NULL == val) {
        TP_LOG_ERROR ("out of memory\n");
        return;
    }
    g_value_init (val, G_VALUE_TYPE (prop_value));
    g_value_copy (prop_value, val);
    g_hash_table_insert (tp_prop->items, g_strdup (prop_name), val);
}

GValue *
tp_property_get_by_name (const TpProperty * tp_prop, const gchar * prop_name)
{
    if (NULL == tp_prop || NULL == prop_name) {
        return NULL;
    }

    return g_hash_table_lookup (tp_prop->items, prop_name);
}

void
tp_property_foreach (const TpProperty * tp_prop,
                     TpPropertyCallback func, gpointer data)
{
    if (NULL == tp_prop || NULL == func) {
        return;
    }

    g_hash_table_foreach (tp_prop->items, func, data);
}

//
// End of File
//
