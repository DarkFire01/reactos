/*
 * PROJECT:     ReactOS Storage Stack / Qualcomm SD Host Controller miniport
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Driver entry and MSM vendor initialization
 * COPYRIGHT:   Justin Miller (justinmiller100@gmail.com)
 */

#include <sdhci.h>

#define NDEBUG
#include <debug.h>

/*
 * The ACPI tables of Qualcomm boards hand out only the standard host
 * controller window, not the SDCC core registers behind it, and firmware has
 * already switched the core into host controller mode before boot. Card detect
 * always goes through a TLMM GPIO on these boards, never the controller's own
 * line, so the slot is run as if its card were always there.
 */
static
NTSTATUS
QcsdhcVendorInitialize(
    _In_ PSDHCI_EXTENSION Extension)
{
    Extension->ForceCardPresent = TRUE;
    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
QcsdhcInitialize(
    _In_ PVOID Extension,
    _In_ PHYSICAL_ADDRESS PhysicalBase,
    _In_ PVOID VirtualBase,
    _In_ ULONG Length,
    _In_ BOOLEAN CrashdumpMode)
{
    ((PSDHCI_EXTENSION)Extension)->VendorInitialize = QcsdhcVendorInitialize;

    return SdhciInitialize(Extension, PhysicalBase, VirtualBase, Length, CrashdumpMode);
}

NTSTATUS
NTAPI
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    SDPORT_INITIALIZATION_DATA InitializationData;

    DPRINT1("qcsdhc: DriverEntry\n");
    SdhciFillInitializationData(&InitializationData);
    InitializationData.Initialize = QcsdhcInitialize;

    return SdPortInitialize(DriverObject, RegistryPath, &InitializationData);
}
