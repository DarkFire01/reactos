/*
 * PROJECT:     ReactOS Kernel - Vista+ APIs
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Mm functions of Windows 10+
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "ntoskrnl_vista.h"

/**
 * @brief
 * Maps a range of physical memory into system address space with extended
 * options.
 *
 * @param[in] PhysicalAddress
 * The starting physical address to map.
 *
 * @param[in] NumberOfBytes
 * The number of bytes to map.
 *
 * @param[in] Flags
 * Extended mapping flags (for example, caching attributes).
 *
 * @return
 * A pointer to the mapped range, or NULL on failure.
 *
 * @unimplemented
 * ReactOS does not implement the extended physical-mapping options; use
 * MmMapIoSpace() instead.
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
