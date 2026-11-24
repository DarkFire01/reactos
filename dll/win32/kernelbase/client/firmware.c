/*
 * PROJECT:     ReactOS Win32 Base API
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     NT6+ Firmware API
 * COPYRIGHT:   Copyright 2023-2024 Ratin Gao <ratin@knsoft.org>
 */

/* We need NT6+ Ex definitions */
#undef NTDDI_VERSION
#define NTDDI_VERSION NTDDI_VISTA
#include <ndk/exfuncs.h>

#include "k32.h"

