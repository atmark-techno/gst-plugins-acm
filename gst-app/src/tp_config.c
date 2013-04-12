/////////////////////////////////////////////////////////////////////////
// tp_config.c: プロパティ設定ファイルのパース（実装）
//
// Author       Nobutaka Kimura (kimura@stprec.co.jp)
// Created:     Feb. 15, 2013
// Last update: Mar. 04, 2013
/////////////////////////////////////////////////////////////////////////

//      #########################################################       //
//      #               I N C L U D E   F I L E S               #       //
//      #########################################################       //

#include "tp_config.h"
#include "tp_log_util.h"

#include <glib/gprintf.h>

//      #########################################################       //
//      #               L O C A L   D E F I N E S               #       //
//      #########################################################       //

//#define VERBOSE_DEBUG

#define TYPE_INT       "(int)"
#define TYPE_GINT      "(gint)"
#define TYPE_UINT      "(uint)"
#define TYPE_GUINT     "(guint)"
#define TYPE_INT64     "(int64)"
#define TYPE_GINT64    "(gint64)"
#define TYPE_UINT64    "(uint64)"
#define TYPE_GUINT64   "(guint64)"
#define TYPE_FLOAT     "(float)"
#define TYPE_GFLOAT    "(gfloat)"
#define TYPE_DOUBLE    "(double)"
#define TYPE_GDOUBLE   "(gdouble)"

#define PATTERN_IMPLICIT_INT          "^[-+]?[0-9]+[ \t]*$"
#define PATTERN_INT                   "^(\\(g?int\\))[ \t]*([-+]?[0-9]+)[ \t]*$"
#define PATTERN_UINT                  "^(\\(g?uint\\))[ \t]*([+]?[0-9]+)[ \t]*$"
#define PATTERN_INT64                 "^(\\(g?int64\\))[ \t]*([-+]?[0-9]+)[ \t]*$"
#define PATTERN_UINT64                "^(\\(g?uint64\\))[ \t]*([+]?[0-9]+)[ \t]*$"
#define PATTERN_IMPLICIT_FLOAT        "^[-+]?[0-9]+\\.[0-9]+[ \t]*$"
#define PATTERN_FLOAT                 "^(\\(g?float\\))[ \t]*([-+]?[0-9]+\\.[0-9]+)[ \t]*$"
#define PATTERN_DOUBLE                "^(\\(g?double\\))[ \t]*([-+]?[0-9]+\\.[0-9]+)[ \t]*$"
#define PATTERN_BOOLEAN               "^[ \t]*(true|false)[ \t]*$"
#define PATTERN_DOUBLE_QUOTED_STRING  "^\"(.+)\"$"
#define PATTERN_SINGLE_QUOTED_STRING  "^'(.+)'$"

#define COMPILE_OPTIONS G_REGEX_CASELESS

struct _TpConfig
{
    GPtrArray *list;
};

//      #########################################################       //
//      #               L O C A L   S T O R A G E               #       //
//      #########################################################       //

//      #########################################################       //
//      #            P R I V A T E   F U N C T I O N S          #       //
//      #########################################################       //

static gboolean
parse_as_integer (const gchar * str, GValue * outValue)
{
    gboolean ret = FALSE;
    gint i;
    const gchar *p;
    const gchar *patt[] = {
        PATTERN_IMPLICIT_INT,
        PATTERN_INT,
        PATTERN_UINT,
        PATTERN_INT64,
        PATTERN_UINT64,
        NULL
    };

    for (i = 0, p = patt[i]; FALSE == ret && NULL != p; p = patt[++i]) {
        GError *err = NULL;
        GMatchInfo *matchInfo = NULL;
        GRegex *regex = g_regex_new (p, COMPILE_OPTIONS, 0, &err);
        if (err) {
            TP_LOG_ERROR ("%s\n", err->message);
            g_error_free (err);
            return FALSE;
        }
        if (g_regex_match (regex, str, 0, &matchInfo)) {
            if (g_match_info_matches (matchInfo)) {
                gchar *capture1 = g_match_info_fetch (matchInfo, 1);
                gchar *capture2 = g_match_info_fetch (matchInfo, 2);
                if (NULL == capture1 && NULL == capture2) {     // INT
                    gint n = (gint) g_ascii_strtoll (str, NULL, 10);
                    g_value_init (outValue, G_TYPE_INT);
                    g_value_set_int (outValue, n);
                    ret = TRUE;
                } else if (0 == g_ascii_strcasecmp (TYPE_INT, capture1) || 0 == g_ascii_strcasecmp (TYPE_GINT, capture1)) {     // INT
                    gint n = (gint) g_ascii_strtoll (capture2, NULL, 10);
                    g_value_init (outValue, G_TYPE_INT);
                    g_value_set_int (outValue, n);
                    ret = TRUE;
                } else if (0 == g_ascii_strcasecmp (TYPE_UINT, capture1) || 0 == g_ascii_strcasecmp (TYPE_GUINT, capture1)) {   // UINT
                    guint n = (guint) g_ascii_strtoull (capture2, NULL, 10);
                    g_value_init (outValue, G_TYPE_UINT);
                    g_value_set_uint (outValue, n);
                    ret = TRUE;
                } else if (0 == g_ascii_strcasecmp (TYPE_INT64, capture1) || 0 == g_ascii_strcasecmp (TYPE_GINT64, capture1)) { // INT64
                    gint64 n = g_ascii_strtoll (capture2, NULL, 10);
                    g_value_init (outValue, G_TYPE_INT64);
                    g_value_set_int64 (outValue, n);
                    ret = TRUE;
                } else if (0 == g_ascii_strcasecmp (TYPE_UINT64, capture1) || 0 == g_ascii_strcasecmp (TYPE_GUINT64, capture1)) {       // UINT64
                    guint64 n = g_ascii_strtoull (capture2, NULL, 10);
                    g_value_init (outValue, G_TYPE_UINT64);
                    g_value_set_uint64 (outValue, n);
                    ret = TRUE;
                }

                g_free (capture2);
                g_free (capture1);
            }

        }
        g_match_info_free (matchInfo);
        g_regex_unref (regex);
        regex = NULL;
    }

    return ret;
}

static gboolean
parse_as_float (const gchar * str, GValue * outValue)
{
    gboolean ret = FALSE;
    gint i;
    const gchar *p;
    const gchar *patt[] = {
        PATTERN_IMPLICIT_FLOAT,
        PATTERN_FLOAT,
        PATTERN_DOUBLE,
        NULL
    };

    for (i = 0, p = patt[i]; FALSE == ret && NULL != p; p = patt[++i]) {
        GError *err = NULL;
        GMatchInfo *matchInfo = NULL;
        GRegex *regex = g_regex_new (p, COMPILE_OPTIONS, 0, &err);
        if (err) {
            TP_LOG_ERROR ("%s\n", err->message);
            g_error_free (err);
            return FALSE;
        }
        if (g_regex_match (regex, str, 0, &matchInfo)) {
            if (g_match_info_matches (matchInfo)) {
                gchar *capture1 = g_match_info_fetch (matchInfo, 1);
                gchar *capture2 = g_match_info_fetch (matchInfo, 2);

                if (NULL == capture1 && NULL == capture2) {     // FLOAT
                    gdouble n = (gfloat) g_ascii_strtod (str, NULL);
                    g_value_init (outValue, G_TYPE_FLOAT);
                    g_value_set_float (outValue, n);
                    ret = TRUE;
                } else if (0 == g_ascii_strcasecmp (TYPE_FLOAT, capture1) || 0 == g_ascii_strcasecmp (TYPE_GFLOAT, capture1)) { // FLOAT
                    gdouble n = (gfloat) g_ascii_strtod (capture2, NULL);
                    g_value_init (outValue, G_TYPE_FLOAT);
                    g_value_set_float (outValue, n);
                    ret = TRUE;
                } else if (0 == g_ascii_strcasecmp (TYPE_DOUBLE, capture1) || 0 == g_ascii_strcasecmp (TYPE_GDOUBLE, capture1)) {       // DOUBLE
                    gdouble n = g_ascii_strtod (capture2, NULL);
                    g_value_init (outValue, G_TYPE_DOUBLE);
                    g_value_set_double (outValue, n);
                    ret = TRUE;
                }

                g_free (capture2);
                g_free (capture1);
            }
        }
        g_match_info_free (matchInfo);
        g_regex_unref (regex);
    }

    return ret;
}

static gboolean
parse_as_boolean (const gchar * str, GValue * outValue)
{
    if (g_regex_match_simple (PATTERN_BOOLEAN, str, G_REGEX_CASELESS, 0)) {
        gchar *tmpStr = g_strdup (str);
        if (!tmpStr) {
            TP_LOG_ERROR ("out of memory\n");
            return FALSE;
        }
        g_value_init (outValue, G_TYPE_BOOLEAN);
        g_value_set_boolean (outValue,
                             (0 == g_ascii_strcasecmp (g_strstrip (tmpStr), "true")));
        g_free (tmpStr);
        return TRUE;
    } else {
        return FALSE;
    }
}

static gboolean
parse_as_string (const gchar * str, GValue * outValue)
{
    gboolean ret = FALSE;
    gint i;
    const gchar *p;
    const gchar *patt[] = {
        PATTERN_DOUBLE_QUOTED_STRING,
        PATTERN_SINGLE_QUOTED_STRING,
        NULL
    };

    for (i = 0, p = patt[i]; FALSE == ret && NULL != p; p = patt[++i]) {
        GError *err = NULL;
        GMatchInfo *matchInfo = NULL;
        GRegex *regex = g_regex_new (p, COMPILE_OPTIONS, 0, &err);
        if (err) {
            TP_LOG_ERROR ("%s\n", err->message);
            g_error_free (err);
            return FALSE;
        }
        if (g_regex_match (regex, str, 0, &matchInfo)) {
            if (g_match_info_matches (matchInfo)) {
                gchar *capture = g_match_info_fetch (matchInfo, 1);
                g_value_init (outValue, G_TYPE_STRING);
                g_value_set_string (outValue, capture);
                g_free (capture);
                ret = TRUE;
            }
        }
        g_match_info_free (matchInfo);
        g_regex_unref (regex);
    }

    if (FALSE == ret) {
        gchar *tmpStr = g_strdup (str);
        if (NULL == tmpStr) {
            TP_LOG_ERROR ("out of memory\n");
            return FALSE;
        }
        g_value_init (outValue, G_TYPE_STRING);
        g_value_set_string (outValue, g_strstrip (tmpStr));
        g_free (tmpStr);
        ret = TRUE;
    }

    return ret;
}

#if 0
static gboolean
parse_value (const gchar * valueStr, const gchar * groupName,
             const gchar * itemName, TpConfig * tp_conf)
{
    GValue value = G_VALUE_INIT;
    TpProperty *tp_prop = tp_config_get_by_name (tp_conf, groupName);

    if (NULL == tp_prop) {
        tp_prop = tp_property_create (groupName);
        if (NULL == tp_prop) {
            TP_LOG_ERROR ("out of memory");
            return FALSE;
        }
        tp_config_add (tp_conf, tp_prop);
    }

    if (parse_as_integer (valueStr, &value)) {
        tp_property_add (tp_prop, itemName, &value);
    } else if (parse_as_float (valueStr, &value)) {
        tp_property_add (tp_prop, itemName, &value);
    } else if (parse_as_boolean (valueStr, &value)) {
        tp_property_add (tp_prop, itemName, &value);
    } else {
        // INT, FLOAT, BOOLEAN 以外は文字列として解釈
        parse_as_string (valueStr, &value);
        tp_property_add (tp_prop, itemName, &value);
    }

#ifdef VERBOSE_DEBUG
    {
        gchar *foo = g_strdup_value_contents (&value);
        TP_LOG_DEBUG ("%s - (%s) value = %s\n", __FUNCTION__,
                      g_type_name (G_VALUE_TYPE (&value)), foo);
        g_free (foo);
    }
#endif // VERBOSE_DEBUG

    if (G_IS_VALUE (&value))
        g_value_unset (&value);

    return TRUE;
}
#else
static gboolean
parse_value (const gchar * valueStr, const gchar * itemName, TpProperty * tp_prop)
{
    GValue value = G_VALUE_INIT;

    if (parse_as_integer (valueStr, &value)) {
        tp_property_add (tp_prop, itemName, &value);
    } else if (parse_as_float (valueStr, &value)) {
        tp_property_add (tp_prop, itemName, &value);
    } else if (parse_as_boolean (valueStr, &value)) {
        tp_property_add (tp_prop, itemName, &value);
    } else {
        // INT, FLOAT, BOOLEAN 以外は文字列として解釈
        parse_as_string (valueStr, &value);
        tp_property_add (tp_prop, itemName, &value);
    }

#ifdef VERBOSE_DEBUG
    {
        gchar *foo = g_strdup_value_contents (&value);
        TP_LOG_DEBUG ("%s - (%s) value = %s\n", __FUNCTION__,
                      g_type_name (G_VALUE_TYPE (&value)), foo);
        g_free (foo);
    }
#endif // VERBOSE_DEBUG

    if (G_IS_VALUE (&value))
        g_value_unset (&value);

    return TRUE;
}
#endif

static gboolean
parse_section (GKeyFile * confFile, const gchar * groupName, TpConfig * tp_conf)
{
    gboolean ret = FALSE;
    gchar **items = NULL;
    gsize i, itemCount = 0;
    GError *error = NULL;
    TpProperty* tp_prop;

    items = g_key_file_get_keys (confFile, groupName, &itemCount, &error);
    if (NULL == items) {
        g_error_free (error);
        return ret;
    }

    tp_prop = tp_config_get_by_name(tp_conf, groupName);
    if (NULL == tp_prop) {
        tp_prop = tp_property_create (groupName);
        if (NULL == tp_prop) {
            TP_LOG_ERROR ("out of memory");
            goto EXIT;
        }
        tp_config_add (tp_conf, tp_prop);
    }

    for (i = 0; i < itemCount; i++) {
        gchar *val = g_key_file_get_value (confFile, groupName, items[i], &error);
        if (error) {
            TP_LOG_ERROR ("%s\n", error->message);
            g_error_free (error);
            goto EXIT;
        }
        if (!parse_value (val, items[i], tp_prop)) {
            g_free (val);
            goto EXIT;
        }
        g_free (val);
    }

    ret = TRUE;
EXIT:
    g_strfreev (items);
    return ret;
}

static void
tp_property_destroy_and_free (gpointer data)
{
    TpProperty *tp_prop = (TpProperty *) data;

    if (NULL == tp_prop) {
        return;
    }

    tp_property_destroy (tp_prop);
}

//      #########################################################       //
//      #             P U B L I C   F U N C T I O N S           #       //
//      #########################################################       //

// TpConfig
TpConfig *
tp_config_create ()
{
    TpConfig *tp_conf = (TpConfig *) g_malloc0 (sizeof (TpConfig));

    if (NULL == tp_conf) {
        TP_LOG_ERROR ("out of memory\n");
        return NULL;
    }

    tp_conf->list = g_ptr_array_new_with_free_func (tp_property_destroy_and_free);

    return tp_conf;
}

void
tp_config_destroy (TpConfig * tp_conf)
{
    if (NULL != tp_conf) {
        g_ptr_array_free (tp_conf->list, TRUE);
        g_free (tp_conf);
    }
}

guint
tp_config_get_count (const TpConfig * tp_conf)
{
    return (NULL != tp_conf) ? tp_conf->list->len : 0;
}

void
tp_config_add (TpConfig * tp_conf, const TpProperty * tp_prop)
{
    const gchar* plugin_name;
    TpProperty* old_prop;
    if (NULL == tp_conf || NULL == tp_prop) {
        return;
    }

    plugin_name = tp_property_get_plugin_name(tp_prop);
    old_prop = tp_config_get_by_name(tp_conf, plugin_name);
    if (NULL == old_prop) {
        g_ptr_array_add (tp_conf->list, (gpointer) tp_prop);
    }
    else {
        g_ptr_array_remove (tp_conf->list, (gpointer) old_prop);
        g_ptr_array_add (tp_conf->list, (gpointer) tp_prop);
    }
}

TpProperty *
tp_config_get_by_name (const TpConfig * tp_conf, const gchar * plugin_name)
{
    guint i;
    if (NULL == tp_conf || NULL == plugin_name) {
        return NULL;
    }

    for (i = 0; i < tp_conf->list->len; i++) {
        TpProperty *tp_prop = g_ptr_array_index (tp_conf->list, i);
        if (0 == g_strcmp0 (plugin_name, tp_property_get_plugin_name (tp_prop))) {
            return tp_prop;
        }
    }

    return NULL;
}

TpConfig*
tp_config_parse_file (const gchar * path)
{
    TpConfig* tp_conf = NULL;
    GKeyFile *confFile = NULL;
    gchar **sections = NULL;
    gsize i, sectionCount = 0;
    GError *error = NULL;

    if (NULL == path) {
        return NULL;
    }

    tp_conf = tp_config_create();
    if (NULL == tp_conf) {
        TP_LOG_ERROR("out of memory\n");
        return NULL;
    }

    confFile = g_key_file_new ();
    if (!g_key_file_load_from_file (confFile, path, G_KEY_FILE_NONE, &error)) {
        TP_LOG_ERROR ("%s %s\n", path, error->message);
        g_error_free (error);
        tp_config_destroy(tp_conf);
        tp_conf = NULL;
        goto EXIT;
    }
    // グループのパース
    sections = g_key_file_get_groups (confFile, &sectionCount);
    for (i = 0; i < sectionCount; i++) {
        if (!parse_section (confFile, sections[i], tp_conf)) {
            tp_config_destroy(tp_conf);
            tp_conf = NULL;
            goto EXIT;
        }
    }

EXIT:
    if (sections)
        g_strfreev (sections);
    if (confFile)
        g_key_file_free (confFile);
    return tp_conf;
}

//
// End of File
//
