/*
 * PROJECT:     ReacTVmm
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Where the identifiers in the device contract are given bodies
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 *
 * Every other file that names one of these only declares it. This is the one
 * that defines it, which is why it has nothing else in it.
 */

#include <windows.h>
#include <initguid.h>
#include <objbase.h>

#include "vdev.h"

/*
 * The two the component library would normally carry. Nothing here links
 * against it, and a machine needs these long before it needs anything else
 * that library has.
 */
DEFINE_GUID(IID_IUnknown,
            0x00000000, 0x0000, 0x0000, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46);
DEFINE_GUID(IID_IClassFactory,
            0x00000001, 0x0000, 0x0000, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46);
