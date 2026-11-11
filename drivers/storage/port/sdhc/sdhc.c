/*
 * PROJECT:     ReactOS Storage Stack / SD Standard Host Controller miniport
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Driver entry for the SDA standard compliant host controller
 * COPYRIGHT:   Justin Miller (justinmiller100@gmail.com)
 */

#include "sdhci.h"

#define NDEBUG
#include <debug.h>

NTSTATUS
NTAPI
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    SDPORT_INITIALIZATION_DATA InitializationData;

    DPRINT1("sdhc: DriverEntry\n");
    SdhciFillInitializationData(&InitializationData);

    return SdPortInitialize(DriverObject, RegistryPath, &InitializationData);
}
