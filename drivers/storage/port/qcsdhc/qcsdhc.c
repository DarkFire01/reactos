/*
 * PROJECT:     ReactOS Storage Stack / Qualcomm SD Host Controller miniport
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Driver entry and MSM vendor initialization
 * COPYRIGHT:   Justin Miller (justinmiller100@gmail.com)
 */

#include <sdhci.h>

#define NDEBUG
#include <debug.h>

/* Qualcomm MSM core register offsets, relative to the standard host controller
   register base (the unified layout used by the newer SDCC cores).  On the
   split-window v4 cores the core block is a separate memory resource and these
   offsets must be reached through that second window; the values below follow
   the documented MSM register map and need validation on the target silicon. */
#define QCSDHC_CORE_POWER          0x0000
#define QCSDHC_CORE_MCI_VERSION    0x0050
#define QCSDHC_CORE_HC_MODE        0x0078
#define QCSDHC_CORE_PWRCTL_STATUS  0x00DC
#define QCSDHC_CORE_PWRCTL_MASK    0x00E0
#define QCSDHC_CORE_PWRCTL_CLEAR   0x00E4
#define QCSDHC_CORE_PWRCTL_CTL     0x00E8
#define QCSDHC_CORE_VENDOR_SPEC    0x010C

#define QCSDHC_HC_MODE_ENABLE      0x00000001
#define QCSDHC_HC_FF_CLK_RST_DIS   0x00000002
#define QCSDHC_VENDOR_SPEC_RESET   0x00000001
#define QCSDHC_PWRCTL_ALL          0x0000000F

static
NTSTATUS
QcsdhcVendorInitialize(
    _In_ PSDHCI_EXTENSION Extension)
{
    ULONG Mode;

    /* Put the core into SD Host Controller mode and keep the feedback clock out
       of software reset so the standard SDHCI reset that follows can complete. */
    Mode = SdPortReadRegisterUlong(Extension->BaseAddress, QCSDHC_CORE_HC_MODE);
    Mode |= QCSDHC_HC_MODE_ENABLE | QCSDHC_HC_FF_CLK_RST_DIS;
    SdPortWriteRegisterUlong(Extension->BaseAddress, QCSDHC_CORE_HC_MODE, Mode);

    /* Release the vendor specific block from reset. */
    Mode = SdPortReadRegisterUlong(Extension->BaseAddress, QCSDHC_CORE_VENDOR_SPEC);
    Mode &= ~QCSDHC_VENDOR_SPEC_RESET;
    SdPortWriteRegisterUlong(Extension->BaseAddress, QCSDHC_CORE_VENDOR_SPEC, Mode);

    /* Acknowledge and unmask the power control interrupt.  The MSM core raises a
       dedicated power IRQ for bus power and voltage switch handshakes; leaving it
       unacknowledged stalls command completion. */
    SdPortWriteRegisterUlong(Extension->BaseAddress, QCSDHC_CORE_PWRCTL_CLEAR, QCSDHC_PWRCTL_ALL);
    SdPortWriteRegisterUlong(Extension->BaseAddress, QCSDHC_CORE_PWRCTL_MASK, QCSDHC_PWRCTL_ALL);

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
