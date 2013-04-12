////////////////////////////////////////////////////////////////////////
// tp_log_util.h: ログユーティリティ
//
// Author:      Nobutaka Kimura (kimura@stprec.co.jp)
// Created:     Feb. 07, 2013
// Last update: Feb. 08, 2013
/////////////////////////////////////////////////////////////////////////

#ifndef _TP_LOG_UTIL_H_
#define _TP_LOG_UTIL_H_

//      #########################################################       //
//      #               I N C L U D E   F I L E S               #       //
//      #########################################################       //

#include <glib.h>

//      #########################################################       //
//      #               T Y P E   D E F I N E S                 #       //
//      #########################################################       //

#undef TP_LOG_DEBUG
#undef TP_LOG_INFO
#undef TP_LOG_WARN
#undef TP_LOG_ERROR

#ifdef _DEBUG
# define TP_LOG_DEBUG(fmt, args...) g_print("DEBUG: "fmt, ## args)
#else
# define TP_LOG_DEBUG(fmt, args...) ((void) (0))
#endif // DEBUG

#define TP_LOG_INFO(fmt, args...) g_print("INFO: "fmt, ## args)
#define TP_LOG_WARN(fmt, args...) g_print("WARN: "fmt, ## args)
#define TP_LOG_ERROR(fmt, args...) g_print("ERROR: "fmt, ## args)

//      #########################################################       //
//      #             P U B L I C   F U N C T I O N S           #       //
//      #########################################################       //

#endif // _LOG_UTIL_H_

//
// End of File
//
