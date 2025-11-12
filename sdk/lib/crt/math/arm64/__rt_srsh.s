/*
 * PROJECT:     ReactOS CRT library
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Implementation of __rt_srsh
 * COPYRIGHT:   Copyright 2022 Justin Miller<justinmiller100@gmail.com>
 */

/* INCLUDES ******************************************************************/

#include <kxarm64.h>

/* CODE **********************************************************************/

    TEXTAREA

    LEAF_ENTRY __rt_srsh
            /* x0 = value (signed 64-bit), w1 = shift (0..n) */
            /* If shift >= 64, return sign bit replicated */
            cmp     w1, #64
            b.ge    __rt_srsh_ge64
            /* arithmetic shift right by variable count */
            asrv    x0, x0, x1
            ret
__rt_srsh_ge64
            /* shift >= 64 -> all bits become sign */
            asr     x0, x0, #63
            ret
    LEAF_END __rt_srsh

    END
/* EOF */
