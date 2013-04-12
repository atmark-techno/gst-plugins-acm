////////////////////////////////////////////////////////////////////////
// tp_detect_media.h: メディア種別の判別（宣言）
//
// Author:      Nobutaka Kimura (kimura@stprec.co.jp)
// Created:     Feb. 14, 2013
// Last update: Feb. 14, 2013
/////////////////////////////////////////////////////////////////////////

#ifndef _TP_DETECT_MEDIA_H_
#define _TP_DETECT_MEDIA_H_

//      #########################################################       //
//      #               I N C L U D E   F I L E S               #       //
//      #########################################################       //

#include <glib.h>

//      #########################################################       //
//      #               T Y P E   D E F I N E S                 #       //
//      #########################################################       //

typedef enum {
    MEDIA_TYPE_UNKNOWN = 0,
    MEDIA_TYPE_QUICKTIME,
    MEDIA_TYPE_MPEGTS
} TpMediaType;

//      #########################################################       //
//      #             P U B L I C   F U N C T I O N S           #       //
//      #########################################################       //

TpMediaType tp_detect_media_type(const gchar* path);

#endif // _TP_DETECT_MEDIA_H_

//
// End of File
//
