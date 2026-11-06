/*
 * PROJECT:     ReactOS ARM64EC support library
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Load config for the ARM64 half of ARM64X images
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <kxarm64.h>

/* IMAGE_LOAD_CONFIG_DIRECTORY64, the linker fills the hybrid and DVRT fields */
    AREA |.rdata|,DATA,READONLY,ALIGN=3

    EXPORT _load_config_used
_load_config_used
    DCD     0x140
    SPACE   0x140 - 4

    END
