/////////////////////////////////////////////////////////////////////////
// tp_term.c: 端末制御ユーティリティ（実装）
//
// Author       Nobutaka Kimura (kimura@stprec.co.jp)
// Created:     Feb. 08, 2013
// Last update: Feb. 12, 2013
/////////////////////////////////////////////////////////////////////////

//      #########################################################       //
//      #               I N C L U D E   F I L E S               #       //
//      #########################################################       //

#include "tp_term.h"
#include "tp_log_util.h"

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>

//      #########################################################       //
//      #               L O C A L   D E F I N E S               #       //
//      #########################################################       //

//      #########################################################       //
//      #               L O C A L   S T O R A G E               #       //
//      #########################################################       //

struct termios g_saved_attr;    /* 端末設定のバックアップ用 */
int g_fd;

//      #########################################################       //
//      #            P R I V A T E   F U N C T I O N S          #       //
//      #########################################################       //

/**
 * 端末の設定を元に戻す
 */
static void
revert_input_mode (void)
{
    tcsetattr (g_fd, TCSANOW, &g_saved_attr);
}

//      #########################################################       //
//      #             P U B L I C   F U N C T I O N S           #       //
//      #########################################################       //

/**
 * 端末を非カノニカルモードに設定する
 */
void
tp_term_set_non_canonical (const int fd)
{
    struct termios tattr;

    if (!isatty (fd)) {
        TP_LOG_ERROR ("failed to set non-canonical mode\n");
        exit (EXIT_FAILURE);
    }

    /* プログラム終了時に元に戻すため、現在の設定をバックアップ */
    g_fd = fd;
    tcgetattr (fd, &g_saved_attr);
    atexit (revert_input_mode);

    tcgetattr (fd, &tattr);
    tattr.c_lflag &= ~(ICANON | ECHO);
    tattr.c_cc[VMIN] = 1;
    tattr.c_cc[VTIME] = 0;
    tcsetattr (fd, TCSAFLUSH, &tattr);
}

//
// End of File
//
