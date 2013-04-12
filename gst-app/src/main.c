/////////////////////////////////////////////////////////////////////////
// main.c: GStreamer プラグインのテストアプリケーション
//
// Author       Nobutaka Kimura (kimura@stprec.co.jp)
// Created:     Feb. 07, 2013
// Last update: Feb. 21, 2013
/////////////////////////////////////////////////////////////////////////

//      #########################################################       //
//      #               I N C L U D E   F I L E S               #       //
//      #########################################################       //

#include "tp_cui.h"
#include "tp_gst_ctrl.h"
#include "tp_log_util.h"

#include <unistd.h> // STDIN_FILENO
#include <signal.h>

//      #########################################################       //
//      #               L O C A L   D E F I N E S               #       //
//      #########################################################       //

/* コマンド */
typedef struct _TpCommand
{
    gchar *name;                  /* コマンド名 */
    gchar key;                    /* キー */
    TpCuiCallbackFunc callback;   /* コマンドのコールバック */
    gpointer cb_arg;              /* コールバックの引数 */
} TpCommand;

//      #########################################################       //
//      #               L O C A L   S T O R A G E               #       //
//      #########################################################       //

TpCui *g_cui_handle = NULL;
TpGstCtrl *g_ctrl = NULL;

//      #########################################################       //
//      #            P R I V A T E   F U N C T I O N S          #       //
//      #########################################################       //

// シグナルハンドラ
static void
sig_catch(int sig)
{
	g_print("catch signal. (%d)\n", sig);
	
	if (g_ctrl) {
		// 再生停止
		tp_gst_ctrl_stop (g_ctrl);
		
		// パイプラインの解放
		tp_gst_ctrl_cleanup_pipeline (g_ctrl);

		// GStreamer コントロールオブジェクトの解放
		tp_gst_ctrl_destroy (g_ctrl);
		
		g_ctrl = NULL;
	}
	
	if (g_cui_handle) {
    	// CUI ハンドルの解放
		tp_cui_destroy (g_cui_handle);
		
		g_cui_handle = NULL;
	}
	
	// シグナルハンドラをデフォルトに戻す
	signal(sig, SIG_DFL);
	
	exit(1);
}

static void
print_commands (const TpCommand * cmdTable)
{
    const TpCommand *cmd;

    for (cmd = cmdTable; 0 != cmd->key; cmd++) {
        if (' ' == cmd->key) {
            g_print ("[SPACE]:%s", cmd->name);
        } else {
            g_print ("[%c]:%s", cmd->key, cmd->name);
        }
        if (0 != (cmd + 1)->key) {
            g_print (", ");
        }
    }
    g_print ("\n-----\n");
}

// 再生位置を先頭に戻す
static gboolean
rewind_cb (gpointer data)
{
    TpGstCtrl **ctrl = (TpGstCtrl **) data;
    tp_gst_ctrl_rewind (*ctrl);
    return TRUE;
}

// 前方への seek
static gboolean
seek_forward_cb (gpointer data)
{
    TpGstCtrl **ctrl = (TpGstCtrl **) data;
    tp_gst_ctrl_seek_forward (*ctrl);
    return TRUE;
}

// 後方への seek
static gboolean
seek_backword_cb (gpointer data)
{
    TpGstCtrl **ctrl = (TpGstCtrl **) data;
    tp_gst_ctrl_seek_backward (*ctrl);
    return TRUE;
}

// 一時停止・再開のトグル
static gboolean
toggle_pause_cb (gpointer data)
{
    static gboolean isPaused = FALSE;
    TpGstCtrl **ctrl = (TpGstCtrl **) data;

    if (isPaused) {
        tp_gst_ctrl_resume (*ctrl);
    } else {
        tp_gst_ctrl_pause (*ctrl);
    }
    isPaused = !isPaused;

    return TRUE;
}

// 停止・再開のトグル
static gboolean
toggle_stop_cb (gpointer data)
{
    static gboolean isStopped = FALSE;
    TpGstCtrl **ctrl = (TpGstCtrl **) data;
	
    if (isStopped) {
        tp_gst_ctrl_start (*ctrl);
    } else {
        tp_gst_ctrl_stop (*ctrl);
    }
    isStopped = !isStopped;
	
    return TRUE;
}

// 終了
static gboolean
quit_cb (gpointer data)
{
    TP_LOG_INFO ("quit\n");
    /* コールバックが FALSE を返すとメインループが停止する */
    return FALSE;
}

//      #########################################################       //
//      #             P U B L I C   F U N C T I O N S           #       //
//      #########################################################       //

gint
main (gint argc, gchar * argv[])
{
    gint ret = 1;
    TpCui *cui_handle = NULL;
    TpGstCtrl *ctrl = NULL;
    GMainLoop *loop = NULL;
    gchar *media = NULL;
    const TpCommand *cmd;

    // コマンド定義
    const TpCommand commandTable[] = {
        {"Quit", 'q', quit_cb, NULL},
        {"Pause/Resume", ' ', toggle_pause_cb, &ctrl},
        {"Stop/Restart", 's', toggle_stop_cb, &ctrl},
        {"Seek forward", 'f', seek_forward_cb, &ctrl},
        {"Seek backward", 'b', seek_backword_cb, &ctrl},
        {"Rewind", 'r', rewind_cb, &ctrl},
        {NULL, 0, NULL, NULL}
    };

	// シグナルハンドラの設定
	if (SIG_ERR == signal(SIGINT, sig_catch)) {
    	g_print("failed to set signal handler.\n");
        return 1;
	}

    // UI 初期化
    cui_handle = tp_cui_create (STDIN_FILENO);
    if (NULL == cui_handle) {
        return 1;
    }
	g_cui_handle = cui_handle;

    // コマンド登録
    for (cmd = commandTable; 0 != cmd->key; cmd++) {
        tp_cui_add_command (cui_handle, cmd->key, cmd->callback, cmd->cb_arg);
    }
    loop = tp_cui_get_main_loop (cui_handle);

    // GStreamer 初期化
    ctrl = tp_gst_ctrl_create (&argc, &argv);
    if (NULL == ctrl) {
        goto CUI_RELEASE_EXIT;
    }
	g_ctrl = ctrl;
    if (argc > 1) {
        media = argv[1];
    }
    print_commands (commandTable);

    // パイプラインのセットアップ
    if (!tp_gst_ctrl_setup_pipeline (ctrl, loop, media)) {
        goto GST_RELASE_EXIT;
    }
    // 再生開始
    tp_gst_ctrl_start (ctrl);

    // メインループスタート
    tp_cui_run (cui_handle);

    // 再生停止
    tp_gst_ctrl_stop (ctrl);

    // パイプラインの解放
    tp_gst_ctrl_cleanup_pipeline (ctrl);
    ret = 0;

GST_RELASE_EXIT:
    // GStreamer コントロールオブジェクトの解放
    tp_gst_ctrl_destroy (ctrl);

CUI_RELEASE_EXIT:
    // CUI ハンドルの解放
    tp_cui_destroy (cui_handle);
    return ret;
}

//
// End of File
//
