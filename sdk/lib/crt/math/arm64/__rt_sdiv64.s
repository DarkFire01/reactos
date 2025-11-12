/*
 * PROJECT:     ReactOS CRT library
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Implementation of __rt_sdiv64
 * COPYRIGHT:   Copyright 2022 Justin Miller<justinmiller100@gmail.com>
 */

/* INCLUDES ******************************************************************/

#include <kxarm64.h>

    IMPORT __rt_sdiv64_worker

/* CODE **********************************************************************/

    TEXTAREA

    NESTED_ENTRY __rt_sdiv64
        /* Call C worker: returns {quotient, modulus} in x0/x1 */
        bl __rt_sdiv64_worker
        /* x0 holds quotient, x1 holds remainder per worker; keep as-is */
        ret
    NESTED_END __rt_sdiv64

    END
/* EOF */
