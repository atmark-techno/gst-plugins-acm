/////////////////////////////////////////////////////////////////////////
// tp_cui.c: ユーザインタフェース（実装）
//
// Author       Nobutaka Kimura (kimura@stprec.co.jp)
// Created:     Feb. 14, 2013
// Last update: Feb. 14, 2013
/////////////////////////////////////////////////////////////////////////

//      #########################################################       //
//      #               I N C L U D E   F I L E S               #       //
//      #########################################################       //

#include "tp_cui.h"
#include "tp_term.h"
#include "tp_log_util.h"

#include <unistd.h>

//      #########################################################       //
//      #               L O C A L   D E F I N E S               #       //
//      #########################################################       //

typedef struct _CmdEntry
{
    TpCuiCallbackFunc func;
    gpointer data;
} CmdEntry;

struct _TpCui
{
    GMainLoop *loop;
    GIOChannel *io;
    GHashTable *cmdTbl;
};

//      #########################################################       //
//      #               L O C A L   S T O R A G E               #       //
//      #########################################################       //

//      #########################################################       //
//      #            P R I V A T E   F U N C T I O N s          #       //
//      #########################################################       //

/**
 * コマンドを処理する
 * @param[in] handle TpCui オブジェクト
 * @param[in] key 入力されたキー
 * @return キー入力の監視を継続するかどうか
 */
static gboolean
tp_cui_process_command (const TpCui * handle, const gchar key)
{
    // コマンドテーブルを検索
    CmdEntry *entry = g_hash_table_lookup (handle->cmdTbl, GINT_TO_POINTER (key));

    if (NULL != entry) {
        return entry->func (entry->data);   // 処理を実行
    } else {
        return TRUE;                // 見つからなかったので監視を継続
    }
}

/**
 * キー読み込み時のコールバック
 */
static gboolean
key_read_cb (GIOChannel * io, GIOCondition cond, gpointer data)
{
    TpCui *handle = (TpCui *) data;
    gboolean continueWatch = FALSE;

    if (cond & G_IO_IN) {
        GError *err = NULL;
        gchar key;
        gsize bytesRead;
        switch (g_io_channel_read_chars (io, &key, 1, &bytesRead, &err)) {
            case G_IO_STATUS_NORMAL:
                // コマンドの処理
                continueWatch = tp_cui_process_command (handle, key);
                break;
            case G_IO_STATUS_AGAIN:
                TP_LOG_DEBUG ("%s - AGAIN\n", __FUNCTION__);
                continueWatch = TRUE;
                break;
            case G_IO_STATUS_ERROR:
                TP_LOG_ERROR ("%s - %s\n", __FUNCTION__, err->message);
                g_error_free (err);
                break;
            case G_IO_STATUS_EOF:
                TP_LOG_DEBUG ("%s - EOF\n", __FUNCTION__);
                break;
            default:
                break;
        }
    }

    if (!continueWatch) {
        g_main_loop_quit (handle->loop);
    }

    return continueWatch;
}


//      #########################################################       //
//      #             P U B L I C   F U N C T I O N S           #       //
//      #########################################################       //

/**
 * 初期化
 * @return TpCui オブジェクト
 */
TpCui *
tp_cui_create (const int fd)
{
    TpCui *handle = (TpCui *) g_malloc0 (sizeof (TpCui));

    if (NULL == handle) {
        TP_LOG_ERROR ("out of memory\n");
        return NULL;
    }

    /* 端末を非カノニカルモードに設定する */
    tp_term_set_non_canonical (fd);

    handle->loop = g_main_loop_new (NULL, FALSE);
    handle->io = g_io_channel_unix_new (fd);
    handle->cmdTbl =
        g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, g_free);
    g_io_add_watch (handle->io, G_IO_IN, key_read_cb, handle);
    g_io_channel_set_flags (handle->io, G_IO_FLAG_NONBLOCK, NULL);
    g_io_channel_set_close_on_unref (handle->io, TRUE);
    g_io_channel_set_encoding (handle->io, NULL, NULL);

    return handle;
}

/**
 * 解放
 * @param[in] handle TpCui オブジェクト
 * @return なし
 */
void
tp_cui_destroy (TpCui * handle)
{
    if (NULL != handle) {
        g_hash_table_destroy (handle->cmdTbl);
        g_io_channel_unref (handle->io);
        g_main_loop_unref (handle->loop);
        g_free (handle);
    }
}

/**
 * コマンドの登録
 * @param[in] handle TpCui オブジェクト
 * @param[in] key 登録するキー
 * @param[in] func キーが押された時のコールバック
 * @param[in] data コールバックに渡すデータ
 * @return 成功したか否か
 */
gboolean
tp_cui_add_command (TpCui * handle, gchar key,
                    TpCuiCallbackFunc func, gpointer data)
{
    if (NULL != handle) {
        CmdEntry *newEntry = (CmdEntry *) g_malloc (sizeof (CmdEntry));
        if (NULL == newEntry) {
            TP_LOG_ERROR ("out of memory\n");
            return FALSE;
        }
        newEntry->func = func;
        newEntry->data = data;
        g_hash_table_insert (handle->cmdTbl, GINT_TO_POINTER (key), newEntry);
        return TRUE;
    } else {
        return FALSE;
    }
}

/**
 * メインループスタート
 */
gboolean
tp_cui_run (TpCui * handle)
{
    if (NULL != handle) {
        g_main_loop_run (handle->loop);
        return TRUE;
    } else {
        return FALSE;
    }
}

/**
 * メインループオブジェクトの取得
 * @param[in] handle TpCui オブジェクト
 * @return メインループオブジェクト
 */
GMainLoop *
tp_cui_get_main_loop (const TpCui * handle)
{
    return (NULL != handle) ? handle->loop : NULL;
}

//
// End of File
//
