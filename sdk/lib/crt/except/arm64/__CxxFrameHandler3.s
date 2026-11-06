/*
 * PROJECT:     ReactOS CRT library
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     __CxxFrameHandler3 alias for ARM64
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <kxarm64.h>

    /* These are the same */
    IMPORT A64NAME(__CxxFrameHandler3), WEAK A64NAME(__CxxFrameHandler)

    END
