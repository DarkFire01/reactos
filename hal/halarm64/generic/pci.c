/*
 * PROJECT:     ReactOS ARM64 HAL
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     PCI configuration space through the MCFG window
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES ******************************************************************/

#include <hal.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS *******************************************************************/

#define HALP_ECAM_FUNCTION_SIZE     0x1000
#define HALP_ECAM_BUS_SIZE          (PCI_MAX_DEVICES * PCI_MAX_FUNCTION * HALP_ECAM_FUNCTION_SIZE)

/* Buses are mapped the first time they are touched and stay mapped, per MCFG window */
static PUCHAR HalpEcamBus[HALP_MAX_ECAM_WINDOWS][256];

/* Set once MmMapIoSpace can back the bus data routines */
BOOLEAN HalpPCIConfigInitialized;

/* FUNCTIONS *****************************************************************/

static
PHALP_ECAM_WINDOW
HalpPciFindWindow(
    _In_ ULONG Segment,
    _In_ ULONG Bus,
    _Out_ PULONG WindowIndex)
{
    ULONG Index;

    for (Index = 0; Index < HalpPlatform.EcamCount; Index++)
    {
        if ((HalpPlatform.Ecam[Index].Segment == Segment) &&
            (Bus >= HalpPlatform.Ecam[Index].StartBus) &&
            (Bus <= HalpPlatform.Ecam[Index].EndBus))
        {
            *WindowIndex = Index;
            return &HalpPlatform.Ecam[Index];
        }
    }

    return NULL;
}

static
PUCHAR
HalpPciFunctionConfig(
    _In_ ULONG Segment,
    _In_ ULONG Bus,
    _In_ PCI_SLOT_NUMBER Slot)
{
    PHALP_ECAM_WINDOW Window;
    PHYSICAL_ADDRESS Address;
    PUCHAR View;
    ULONG Index;

    /* A bus no window decodes is never touched, it may sit behind another root */
    Window = HalpPciFindWindow(Segment, Bus, &Index);
    if (!Window)
        return NULL;

    View = HalpEcamBus[Index][Bus];
    if (!View)
    {
        if (KeGetCurrentIrql() > DISPATCH_LEVEL)
            return NULL;

        Address.QuadPart = Window->Base.QuadPart +
                           ((ULONG64)(Bus - Window->StartBus) * HALP_ECAM_BUS_SIZE);
        View = MmMapIoSpace(Address, HALP_ECAM_BUS_SIZE, MmNonCached);
        if (!View)
            return NULL;

        /* Another processor may have mapped it meanwhile */
        if (InterlockedCompareExchangePointer((PVOID *)&HalpEcamBus[Index][Bus], View, NULL))
        {
            MmUnmapIoSpace(View, HALP_ECAM_BUS_SIZE);
            View = HalpEcamBus[Index][Bus];
        }
    }

    return View + (((Slot.u.bits.DeviceNumber * PCI_MAX_FUNCTION) +
                    Slot.u.bits.FunctionNumber) * HALP_ECAM_FUNCTION_SIZE);
}

/* Uses the widest access the alignment allows, devices may not decode wider ones */
static
VOID
HalpPciTransfer(
    _In_ PUCHAR Config,
    _Inout_updates_bytes_(Length) PUCHAR Buffer,
    _In_ ULONG Offset,
    _In_ ULONG Length,
    _In_ BOOLEAN Write)
{
    ULONG Width;

    for (; Length; Offset += Width, Buffer += Width, Length -= Width)
    {
        if (!(Offset & 3) && (Length >= sizeof(ULONG)))
        {
            Width = sizeof(ULONG);
            if (Write)
                WRITE_REGISTER_ULONG((PULONG)(Config + Offset), *(ULONG UNALIGNED *)Buffer);
            else
                *(ULONG UNALIGNED *)Buffer = READ_REGISTER_ULONG((PULONG)(Config + Offset));
        }
        else if (!(Offset & 1) && (Length >= sizeof(USHORT)))
        {
            Width = sizeof(USHORT);
            if (Write)
                WRITE_REGISTER_USHORT((PUSHORT)(Config + Offset), *(USHORT UNALIGNED *)Buffer);
            else
                *(USHORT UNALIGNED *)Buffer = READ_REGISTER_USHORT((PUSHORT)(Config + Offset));
        }
        else
        {
            Width = sizeof(UCHAR);
            if (Write)
                WRITE_REGISTER_UCHAR(Config + Offset, *Buffer);
            else
                *Buffer = READ_REGISTER_UCHAR(Config + Offset);
        }
    }
}

/**
 * @brief Accesses configuration space without looking at the function first.
 *
 * Reads of a missing function return all ones, the way a bus master abort
 * does. This backs the bus interface the ACPI driver hands to the PCI driver.
 *
 * @param[in] Bus
 * The bus number, with the PCI segment in bits 8 to 23.
 */
ULONG
NTAPI
HalpPciRawConfig(
    _In_ ULONG Bus,
    _In_ PCI_SLOT_NUMBER Slot,
    _Inout_updates_bytes_(Length) PVOID Buffer,
    _In_ ULONG Offset,
    _In_ ULONG Length,
    _In_ BOOLEAN Write)
{
    PUCHAR Config;

    if ((Offset >= HALP_ECAM_FUNCTION_SIZE) || (Length > (HALP_ECAM_FUNCTION_SIZE - Offset)))
        return 0;

    Config = HalpPciFunctionConfig(Bus >> 8, Bus & 0xFF, Slot);
    if (!Config)
    {
        if (!Write)
            RtlFillMemory(Buffer, Length, 0xFF);
        return Length;
    }

    HalpPciTransfer(Config, Buffer, Offset, Length, Write);
    return Length;
}

/**
 * @brief Reads the common configuration space of a function.
 *
 * A missing function reads back as the invalid vendor ID only, the same as
 * the legacy mechanism reports it.
 */
ULONG
NTAPI
HalpPciReadConfig(
    _In_ ULONG Bus,
    _In_ PCI_SLOT_NUMBER Slot,
    _Out_writes_bytes_(Length) PVOID Buffer,
    _In_ ULONG Offset,
    _In_ ULONG Length)
{
    PUCHAR Config;
    ULONG Missing;

    if (Offset >= sizeof(PCI_COMMON_CONFIG))
        return 0;
    Length = min(Length, sizeof(PCI_COMMON_CONFIG) - Offset);

    /* Bus data only names segment 0 */
    Config = (Bus <= 0xFF) ? HalpPciFunctionConfig(0, Bus, Slot) : NULL;
    if (!Config || (READ_REGISTER_USHORT((PUSHORT)Config) == PCI_INVALID_VENDORID))
    {
        if (Offset >= sizeof(USHORT))
            return 0;

        Missing = min(Length, sizeof(USHORT) - Offset);
        RtlFillMemory(Buffer, Missing, 0xFF);
        return Missing;
    }

    HalpPciTransfer(Config, Buffer, Offset, Length, FALSE);
    return Length;
}

ULONG
NTAPI
HalpPciWriteConfig(
    _In_ ULONG Bus,
    _In_ PCI_SLOT_NUMBER Slot,
    _In_reads_bytes_(Length) PVOID Buffer,
    _In_ ULONG Offset,
    _In_ ULONG Length)
{
    PUCHAR Config;

    if (Offset >= sizeof(PCI_COMMON_CONFIG))
        return 0;
    Length = min(Length, sizeof(PCI_COMMON_CONFIG) - Offset);

    Config = (Bus <= 0xFF) ? HalpPciFunctionConfig(0, Bus, Slot) : NULL;
    if (!Config || (READ_REGISTER_USHORT((PUSHORT)Config) == PCI_INVALID_VENDORID))
        return 0;

    HalpPciTransfer(Config, Buffer, Offset, Length, TRUE);
    return Length;
}

/**
 * @brief Configuration access for the kernel debugger at boot.
 *
 * Phase 0 runs before MmMapIoSpace works, so the function's page is mapped
 * through the HAL range for each access and dropped again. Only segment 0 is
 * reachable, as with the bus data routines. A bus no window decodes reads as
 * all ones, like a master abort.
 */
static
ULONG
HalpPhase0Config(
    _In_ ULONG Bus,
    _In_ PCI_SLOT_NUMBER Slot,
    _Inout_updates_bytes_(Length) PVOID Buffer,
    _In_ ULONG Offset,
    _In_ ULONG Length,
    _In_ BOOLEAN Write)
{
    PHALP_ECAM_WINDOW Window;
    PHYSICAL_ADDRESS Address;
    PUCHAR Config;
    ULONG Index;

    if ((Offset >= HALP_ECAM_FUNCTION_SIZE) || (Length > (HALP_ECAM_FUNCTION_SIZE - Offset)))
        return 0;

    Window = HalpPciFindWindow(0, Bus, &Index);
    if (!Window)
    {
        if (!Write)
            RtlFillMemory(Buffer, Length, 0xFF);
        return Length;
    }

    Address.QuadPart = Window->Base.QuadPart +
                       ((ULONG64)(Bus - Window->StartBus) * HALP_ECAM_BUS_SIZE) +
                       (((Slot.u.bits.DeviceNumber * PCI_MAX_FUNCTION) +
                         Slot.u.bits.FunctionNumber) * HALP_ECAM_FUNCTION_SIZE);

    Config = HalpMapPhysicalMemory64(Address, 1);
    if (!Config)
        return 0;

    HalpPciTransfer(Config, Buffer, Offset, Length, Write);
    HalpUnmapVirtualAddress(Config, 1);
    return Length;
}

CODE_SEG("INIT")
ULONG
HalpPhase0GetPciDataByOffset(
    _In_ ULONG Bus,
    _In_ PCI_SLOT_NUMBER PciSlot,
    _Out_writes_bytes_all_(Length) PVOID Buffer,
    _In_ ULONG Offset,
    _In_ ULONG Length)
{
    return HalpPhase0Config(Bus, PciSlot, Buffer, Offset, Length, FALSE);
}

CODE_SEG("INIT")
ULONG
HalpPhase0SetPciDataByOffset(
    _In_ ULONG Bus,
    _In_ PCI_SLOT_NUMBER PciSlot,
    _In_reads_bytes_(Length) PVOID Buffer,
    _In_ ULONG Offset,
    _In_ ULONG Length)
{
    return HalpPhase0Config(Bus, PciSlot, Buffer, Offset, Length, TRUE);
}

ULONG
NTAPI
HalGetBusDataByOffset(
    _In_ BUS_DATA_TYPE BusDataType,
    _In_ ULONG BusNumber,
    _In_ ULONG SlotNumber,
    _Out_writes_bytes_(Length) PVOID Buffer,
    _In_ ULONG Offset,
    _In_ ULONG Length)
{
    PCI_SLOT_NUMBER Slot;

    if (BusDataType != PCIConfiguration)
        return 0;

    Slot.u.AsULONG = SlotNumber;
    return HalpPciReadConfig(BusNumber, Slot, Buffer, Offset, Length);
}

ULONG
NTAPI
HalGetBusData(
    _In_ BUS_DATA_TYPE BusDataType,
    _In_ ULONG BusNumber,
    _In_ ULONG SlotNumber,
    _Out_writes_bytes_(Length) PVOID Buffer,
    _In_ ULONG Length)
{
    return HalGetBusDataByOffset(BusDataType, BusNumber, SlotNumber, Buffer, 0, Length);
}

ULONG
NTAPI
HalSetBusDataByOffset(
    _In_ BUS_DATA_TYPE BusDataType,
    _In_ ULONG BusNumber,
    _In_ ULONG SlotNumber,
    _In_reads_bytes_(Length) PVOID Buffer,
    _In_ ULONG Offset,
    _In_ ULONG Length)
{
    PCI_SLOT_NUMBER Slot;

    if (BusDataType != PCIConfiguration)
        return 0;

    Slot.u.AsULONG = SlotNumber;
    return HalpPciWriteConfig(BusNumber, Slot, Buffer, Offset, Length);
}

ULONG
NTAPI
HalSetBusData(
    _In_ BUS_DATA_TYPE BusDataType,
    _In_ ULONG BusNumber,
    _In_ ULONG SlotNumber,
    _In_reads_bytes_(Length) PVOID Buffer,
    _In_ ULONG Length)
{
    return HalSetBusDataByOffset(BusDataType, BusNumber, SlotNumber, Buffer, 0, Length);
}

/* EOF */
