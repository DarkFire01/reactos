/*
/*
 * PROJECT:     ReactOS CRT library
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Implementation of ceil
 * COPYRIGHT:   Copyright 2022 Justin Miller<justinmiller100@gmail.com>
 */

/* INCLUDES ******************************************************************/

#include <kxarm64.h>

/* CODE **********************************************************************/

    TEXTAREA

    LEAF_ENTRY ceil
        /* round toward +infinity */
        frintp d0, d0
        ret
    LEAF_END ceil

    END
/* EOF */
