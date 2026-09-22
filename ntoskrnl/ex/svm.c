/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Sharing process address spaces with devices behind an IOMMU
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* PUBLIC FUNCTIONS ***********************************************************/

/**
 * @brief
 * Lets a device reach the address space of the current process through
 * the IOMMU, using an address space ID.
 *
 * @param[in] PhysicalDeviceObject
 * The device that gets access.
 *
 * @param[out] ReturnedAsid
 * Receives the address space ID, or MAXULONG on failure.
 *
 * @return
 * STATUS_NOT_SUPPORTED, since ReactOS drives no IOMMU.
 */
NTSTATUS
NTAPI
ExShareAddressSpaceWithDevice(
    _In_ PDEVICE_OBJECT PhysicalDeviceObject,
    _Out_ PULONG ReturnedAsid)
{
    UNREFERENCED_PARAMETER(PhysicalDeviceObject);

    *ReturnedAsid = MAXULONG;
    return STATUS_NOT_SUPPORTED;
}

/* EOF */
