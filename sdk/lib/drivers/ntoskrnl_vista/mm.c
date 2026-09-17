/*
 * PROJECT:     ReactOS Kernel - Vista+ APIs
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Mm functions of Windows 10+
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include "ntoskrnl_vista.h"

/* FUNCTIONS ******************************************************************/

/**
 * @brief
 * Maps a physical range into system address space with extended options.
 *
 * @param[in] PhysicalAddress
 * First physical address of the range.
 *
 * @param[in] NumberOfBytes
 * Size of the range, in bytes.
 *
 * @param[in] Flags
 * Extended mapping flags, such as the caching attribute.
 *
 * @return
 * NULL. Callers should use MmMapIoSpace() instead.
 *
 * @unimplemented
 */
PVOID
NTAPI
MmMapInSpaceEx(
    _In_ PHYSICAL_ADDRESS PhysicalAddress,
    _In_ SIZE_T NumberOfBytes,
    _In_ ULONG Flags)
{
    UNREFERENCED_PARAMETER(PhysicalAddress);
    UNREFERENCED_PARAMETER(NumberOfBytes);
    UNREFERENCED_PARAMETER(Flags);

    return NULL;
}

/* EOF */
