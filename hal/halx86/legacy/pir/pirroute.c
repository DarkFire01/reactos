/*
 * PROJECT:     ReactOS HAL
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Legacy PC PCI IRQ routing table and links
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <hal.h>
#include <wdmguid.h>
#include "irqrouter.h"

#define NDEBUG
#include <debug.h>

#define PIR_SIGNATURE           'RIP$'
#define PIR_BIOS_BASE           0xF0000
#define PIR_BIOS_LENGTH         0x10000
#define PIR_ALIGNMENT           16
#define PIR_VERSION             0x0100
#define PIR_HEADER_SIZE         FIELD_OFFSET(PCI_IRQ_ROUTING_TABLE, Slot)
#define PIR_PIN_COUNT           4
#define PIR_ALL_PINS            0x0F

#define HALP_PIC_IRQ_COUNT      16
#define HALP_IDE_NATIVE_MODES   0x05

#define HALP_ROUTING_KEY        L"\\Registry\\Machine\\System\\CurrentControlSet\\Control\\Pnp\\PciIrqRouting"
#define HALP_MINIPORTS_KEY      HALP_ROUTING_KEY L"\\IrqMiniports"
#define HALP_BIOS_ROUTING_KEY   L"\\Registry\\Machine\\System\\CurrentControlSet\\Control\\BiosInfo\\PciIrqRouting"

/* PciIrqRouting Options */
#define HALP_ROUTING_ENABLED    0x01
#define HALP_ROUTING_USE_PIR    0x04

/* BiosInfo PciIrqRouting Attributes */
#define HALP_BIOS_PIR_BROKEN    0x04

extern NTSYSAPI ULONG InitSafeBootMode;

typedef struct _HALP_PCI_ROUTE
{
    ULONG Bus;
    PCI_SLOT_NUMBER Slot;
    UCHAR Pin;
    UCHAR BaseClass;
    UCHAR SubClass;
    PDEVICE_OBJECT Parent;
    ROUTING_TOKEN Token;
} HALP_PCI_ROUTE, *PHALP_PCI_ROUTE;

static struct
{
    ULONG RouterBus;
    PCI_SLOT_NUMBER RouterSlot;
    PPCI_IRQ_ROUTING_TABLE Table;
    PHALP_PCI_LINK Links;
    INT_ROUTE_INTERFACE_STANDARD Interface;
} HalpPciIrqRouting;

/* ROUTER ACCESS ****************************************************************/

VOID
NTAPI
HalpPirReadRouter(
    _In_ ULONG Offset,
    _Out_writes_bytes_(Length) PVOID Buffer,
    _In_ ULONG Length)
{
    HalGetBusDataByOffset(PCIConfiguration,
                          HalpPciIrqRouting.RouterBus,
                          HalpPciIrqRouting.RouterSlot.u.AsULONG,
                          Buffer,
                          Offset,
                          Length);
}

VOID
NTAPI
HalpPirWriteRouter(
    _In_ ULONG Offset,
    _In_reads_bytes_(Length) PVOID Buffer,
    _In_ ULONG Length)
{
    HalSetBusDataByOffset(PCIConfiguration,
                          HalpPciIrqRouting.RouterBus,
                          HalpPciIrqRouting.RouterSlot.u.AsULONG,
                          Buffer,
                          Offset,
                          Length);
}

/* Lowest and highest nonzero link values, both 0 when no pin is routed */
VOID
NTAPI
HalpPirLinkRange(
    _In_ PPCI_IRQ_ROUTING_TABLE Table,
    _Out_ PUCHAR Lowest,
    _Out_ PUCHAR Highest)
{
    PSLOT_INFO Slot;
    PSLOT_INFO End = (PSLOT_INFO)((PUCHAR)Table + Table->TableSize);
    ULONG Pin;

    *Lowest = MAXUCHAR;
    *Highest = 0;

    for (Slot = Table->Slot; Slot < End; Slot++)
    {
        for (Pin = 0; Pin < RTL_NUMBER_OF(Slot->PinInfo); Pin++)
        {
            UCHAR Link = Slot->PinInfo[Pin].Link;

            if (Link == 0)
                continue;

            *Lowest = (UCHAR)min(*Lowest, Link);
            *Highest = (UCHAR)max(*Highest, Link);
        }
    }

    if (*Highest == 0)
        *Lowest = 0;
}

BOOLEAN
NTAPI
HalpPirAllLinks(
    _In_ PPCI_IRQ_ROUTING_TABLE Table,
    _In_ BOOLEAN (NTAPI *IsValid)(_In_ UCHAR Link))
{
    PSLOT_INFO Slot;
    PSLOT_INFO End = (PSLOT_INFO)((PUCHAR)Table + Table->TableSize);
    ULONG Pin;

    for (Slot = Table->Slot; Slot < End; Slot++)
    {
        for (Pin = 0; Pin < RTL_NUMBER_OF(Slot->PinInfo); Pin++)
        {
            if (Slot->PinInfo[Pin].Link != 0 && !IsValid(Slot->PinInfo[Pin].Link))
                return FALSE;
        }
    }

    return TRUE;
}

/* For routers that leave the trigger modes in the EISA edge/level register */
NTSTATUS
NTAPI
HalpElcrGetTrigger(
    _Out_ PUSHORT LevelIrqs)
{
    *LevelIrqs = (USHORT)((__inbyte(EISA_ELCR_SLAVE) << 8) | __inbyte(EISA_ELCR_MASTER));
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
HalpElcrSetTrigger(
    _In_ USHORT LevelIrqs)
{
    __outbyte(EISA_ELCR_MASTER, (UCHAR)LevelIrqs);
    __outbyte(EISA_ELCR_SLAVE, (UCHAR)(LevelIrqs >> 8));
    return STATUS_SUCCESS;
}

/* TABLE ************************************************************************/

static
PSLOT_INFO
HalpPirSlotEnd(
    _In_ PPCI_IRQ_ROUTING_TABLE Table)
{
    return (PSLOT_INFO)((PUCHAR)Table + Table->TableSize);
}

static
VOID
HalpPirDropSlot(
    _Inout_ PPCI_IRQ_ROUTING_TABLE Table,
    _Inout_ PSLOT_INFO Slot)
{
    *Slot = *(HalpPirSlotEnd(Table) - 1);
    Table->TableSize = (USHORT)(Table->TableSize - sizeof(SLOT_INFO));
}

static
BOOLEAN
HalpPirSlotUsed(
    _In_ PSLOT_INFO Slot)
{
    ULONG Pin;

    for (Pin = 0; Pin < PIR_PIN_COUNT; Pin++)
    {
        if (Slot->PinInfo[Pin].Link != 0)
            return TRUE;
    }

    return FALSE;
}

/* Fails when both entries route the same pin */
static
BOOLEAN
HalpPirMergeSlot(
    _Inout_ PSLOT_INFO Slot,
    _In_ PSLOT_INFO Duplicate)
{
    ULONG Pin;

    for (Pin = 0; Pin < PIR_PIN_COUNT; Pin++)
    {
        if (Duplicate->PinInfo[Pin].Link == 0)
            continue;

        if (Slot->PinInfo[Pin].Link != 0)
            return FALSE;

        Slot->PinInfo[Pin] = Duplicate->PinInfo[Pin];
    }

    return TRUE;
}

static
BOOLEAN
HalpPirCleanTable(
    _Inout_ PPCI_IRQ_ROUTING_TABLE Table)
{
    PSLOT_INFO Slot, Other;
    ULONG Pin;

    /* Pins limited to IRQ 0 are unconnected */
    for (Slot = Table->Slot; Slot < HalpPirSlotEnd(Table); Slot++)
    {
        for (Pin = 0; Pin < PIR_PIN_COUNT; Pin++)
        {
            if ((Slot->PinInfo[Pin].InterruptMap & ~1) == 0)
                RtlZeroMemory(&Slot->PinInfo[Pin], sizeof(PIN_INFO));
        }
    }

    Slot = Table->Slot;
    while (Slot < HalpPirSlotEnd(Table))
    {
        if (!HalpPirSlotUsed(Slot))
        {
            HalpPirDropSlot(Table, Slot);
            continue;
        }

        Other = Slot + 1;
        while (Other < HalpPirSlotEnd(Table))
        {
            if (Other->BusNumber != Slot->BusNumber ||
                (Other->DeviceNumber >> 3) != (Slot->DeviceNumber >> 3))
            {
                Other++;
                continue;
            }

            if (!HalpPirMergeSlot(Slot, Other))
                return FALSE;

            HalpPirDropSlot(Table, Other);
        }

        Slot++;
    }

    return (Table->TableSize > PIR_HEADER_SIZE);
}

static
BOOLEAN
HalpPirListsPins(
    _In_ PPCI_IRQ_ROUTING_TABLE Table,
    _In_ ULONG Device,
    _In_ ULONG PinMask)
{
    PSLOT_INFO Slot;
    ULONG Pin;

    for (Slot = Table->Slot; Slot < HalpPirSlotEnd(Table); Slot++)
    {
        if ((ULONG)(Slot->DeviceNumber >> 3) != Device)
            continue;

        for (Pin = 0; Pin < PIR_PIN_COUNT; Pin++)
        {
            if ((PinMask & (1 << Pin)) && Slot->PinInfo[Pin].Link != 0)
                return TRUE;
        }
    }

    return FALSE;
}

/* Pins of a bus 0 function that the table has to route */
static
ULONG
HalpPirRequiredPins(
    _In_ PPCI_COMMON_CONFIG Config,
    _In_ BOOLEAN SecondaryBuses)
{
    UCHAR Pin = Config->u.type0.InterruptPin;
    UCHAR Line = Config->u.type0.InterruptLine;

    if ((Config->BaseClass == PCI_CLASS_MASS_STORAGE_CTLR ||
         Config->SubClass == PCI_SUBCLASS_MSC_IDE_CTLR) &&
        !(Config->ProgIf & HALP_IDE_NATIVE_MODES))
    {
        return 0;
    }

    if (PCI_CONFIGURATION_TYPE(Config) == PCI_BRIDGE_TYPE &&
        Config->BaseClass == PCI_CLASS_BRIDGE_DEV &&
        Config->SubClass == PCI_SUBCLASS_BR_PCI_TO_PCI)
    {
        return SecondaryBuses ? 0 : PIR_ALL_PINS;
    }

    if (Pin == 0 || Pin > PIR_PIN_COUNT)
        return 0;

    if ((Config->Command & (PCI_ENABLE_IO_SPACE | PCI_ENABLE_MEMORY_SPACE)) &&
        (Line == 0 || Line >= HALP_PIC_IRQ_COUNT))
    {
        return 0;
    }

    return 1 << (Pin - 1);
}

static
BOOLEAN
HalpPirMatchesBusZero(
    _In_ PPCI_IRQ_ROUTING_TABLE Table)
{
    PCI_COMMON_CONFIG Config;
    PCI_SLOT_NUMBER SlotNumber;
    BOOLEAN SecondaryBuses = FALSE;
    PSLOT_INFO Slot;
    ULONG Device, Function, Required;

    for (Slot = Table->Slot; Slot < HalpPirSlotEnd(Table); Slot++)
    {
        if (Slot->BusNumber != 0)
            SecondaryBuses = TRUE;
    }

    SlotNumber.u.AsULONG = 0;
    for (Device = 0; Device < PCI_MAX_DEVICES; Device++)
    {
        SlotNumber.u.bits.DeviceNumber = Device;

        for (Function = 0; Function < PCI_MAX_FUNCTION; Function++)
        {
            SlotNumber.u.bits.FunctionNumber = Function;

            Config.VendorID = PCI_INVALID_VENDORID;
            Config.DeviceID = PCI_INVALID_VENDORID;
            HalGetBusDataByOffset(PCIConfiguration,
                                  0,
                                  SlotNumber.u.AsULONG,
                                  &Config,
                                  0,
                                  PCI_COMMON_HDR_LENGTH);

            if (Config.VendorID == PCI_INVALID_VENDORID || Config.DeviceID == PCI_INVALID_VENDORID)
                continue;

            Required = HalpPirRequiredPins(&Config, SecondaryBuses);
            if (Required != 0 && !HalpPirListsPins(Table, Device, Required))
                return FALSE;

            if (Function == 0 && !PCI_MULTIFUNCTION_DEVICE(&Config))
                break;
        }
    }

    return TRUE;
}

static
PPCI_IRQ_ROUTING_TABLE
HalpPirCaptureTable(
    _In_ PPCI_IRQ_ROUTING_TABLE Candidate,
    _In_ ULONG Available)
{
    PPCI_IRQ_ROUTING_TABLE Table;
    UCHAR Checksum = 0;
    ULONG Index;

    if (Candidate->Signature != PIR_SIGNATURE ||
        Candidate->Version != PIR_VERSION ||
        Candidate->TableSize > Available ||
        Candidate->TableSize <= PIR_HEADER_SIZE ||
        (Candidate->TableSize % sizeof(SLOT_INFO)) != 0)
    {
        return NULL;
    }

    for (Index = 0; Index < Candidate->TableSize; Index++)
        Checksum += ((PUCHAR)Candidate)[Index];

    if (Checksum != 0)
        return NULL;

    Table = ExAllocatePoolWithTag(NonPagedPool, Candidate->TableSize, TAG_HAL);
    if (!Table)
        return NULL;

    RtlCopyMemory(Table, Candidate, Candidate->TableSize);

    if (!HalpPirCleanTable(Table) || !HalpPirMatchesBusZero(Table))
    {
        DPRINT1("HAL: $PIR table does not match the hardware, ignoring it\n");
        ExFreePoolWithTag(Table, TAG_HAL);
        return NULL;
    }

    return Table;
}

static
PPCI_IRQ_ROUTING_TABLE
HalpPirFindTable(VOID)
{
    PPCI_IRQ_ROUTING_TABLE Table = NULL;
    PHYSICAL_ADDRESS Address;
    PUCHAR Bios;
    ULONG Offset;

    Address.QuadPart = PIR_BIOS_BASE;
    Bios = MmMapIoSpace(Address, PIR_BIOS_LENGTH, MmNonCached);
    if (!Bios)
        return NULL;

    for (Offset = 0;
         !Table && Offset + PIR_HEADER_SIZE <= PIR_BIOS_LENGTH;
         Offset += PIR_ALIGNMENT)
    {
        Table = HalpPirCaptureTable((PPCI_IRQ_ROUTING_TABLE)(Bios + Offset),
                                    PIR_BIOS_LENGTH - Offset);
    }

    MmUnmapIoSpace(Bios, PIR_BIOS_LENGTH);
    return Table;
}

static
PSLOT_INFO
HalpPirFindSlot(
    _In_ ULONG Bus,
    _In_ ULONG Device)
{
    PPCI_IRQ_ROUTING_TABLE Table = HalpPciIrqRouting.Table;
    PSLOT_INFO Slot;

    for (Slot = Table->Slot; Slot < HalpPirSlotEnd(Table); Slot++)
    {
        if (Slot->BusNumber == Bus && (ULONG)(Slot->DeviceNumber >> 3) == Device)
            return Slot;
    }

    return NULL;
}

/* LINKS ************************************************************************/

static
PHALP_PCI_LINK
HalpPirLookupLink(
    _In_ UCHAR Value)
{
    PHALP_PCI_LINK Link;

    for (Link = HalpPciIrqRouting.Links; Link; Link = Link->Next)
    {
        if (Link->Link == Value)
            return Link;
    }

    return NULL;
}

static
VOID
HalpPirFreeLinks(VOID)
{
    PHALP_PCI_LINK Link;

    while (HalpPciIrqRouting.Links)
    {
        Link = HalpPciIrqRouting.Links;
        HalpPciIrqRouting.Links = Link->Next;
        ExFreePoolWithTag(Link, TAG_HAL);
    }
}

static
NTSTATUS
HalpPirBuildLinks(
    _In_ PPCI_IRQ_ROUTING_TABLE Table)
{
    PHALP_PCI_LINK Link;
    PSLOT_INFO Slot;
    ULONG Pin;

    for (Slot = Table->Slot; Slot < HalpPirSlotEnd(Table); Slot++)
    {
        for (Pin = 0; Pin < PIR_PIN_COUNT; Pin++)
        {
            if (Slot->PinInfo[Pin].Link == 0 || HalpPirLookupLink(Slot->PinInfo[Pin].Link))
                continue;

            Link = ExAllocatePoolZero(NonPagedPool, sizeof(*Link), TAG_HAL);
            if (!Link)
            {
                HalpPirFreeLinks();
                return STATUS_INSUFFICIENT_RESOURCES;
            }

            Link->Link = Slot->PinInfo[Pin].Link;
            Link->IrqMask = Slot->PinInfo[Pin].InterruptMap;
            Link->Next = HalpPciIrqRouting.Links;
            HalpPciIrqRouting.Links = Link;
        }
    }

    return STATUS_SUCCESS;
}

static
NTSTATUS
HalpPirQueryRoute(
    _In_ PDEVICE_OBJECT Device,
    _Out_ PHALP_PCI_ROUTE Route)
{
    UCHAR Line, Flags;

    return HalpPciIrqRouting.Interface.GetInterruptRouting(Device,
                                                           &Route->Bus,
                                                           &Route->Slot.u.AsULONG,
                                                           &Line,
                                                           &Route->Pin,
                                                           &Route->BaseClass,
                                                           &Route->SubClass,
                                                           &Route->Parent,
                                                           &Route->Token,
                                                           &Flags);
}

static
BOOLEAN
HalpPirIsNativeIde(
    _In_ PHALP_PCI_ROUTE Route)
{
    PCI_COMMON_HEADER Header;

    Header.VendorID = PCI_INVALID_VENDORID;
    Header.DeviceID = PCI_INVALID_VENDORID;
    HalGetBusDataByOffset(PCIConfiguration,
                          Route->Bus,
                          Route->Slot.u.AsULONG,
                          &Header,
                          0,
                          PCI_COMMON_HDR_LENGTH);

    return Header.VendorID != PCI_INVALID_VENDORID &&
           Header.DeviceID != PCI_INVALID_VENDORID &&
           Header.BaseClass == Route->BaseClass &&
           Header.SubClass == Route->SubClass &&
           (Header.ProgIf & HALP_IDE_NATIVE_MODES);
}

/* Walks the pin up through bridges to a bus the table lists */
static
BOOLEAN
HalpPirResolveLink(
    _In_ PHALP_PCI_ROUTE Route,
    _Out_ PUCHAR Value)
{
    HALP_PCI_ROUTE Bridge;
    PCI_SLOT_NUMBER Slot = Route->Slot;
    PDEVICE_OBJECT Parent = Route->Parent;
    ULONG Bus = Route->Bus;
    ULONG PinIndex;
    PSLOT_INFO Entry;

    if (Route->Pin == 0 || Route->Pin > PIR_PIN_COUNT)
        return FALSE;

    PinIndex = Route->Pin - 1;

    for (;;)
    {
        Entry = HalpPirFindSlot(Bus, Slot.u.bits.DeviceNumber);
        if (Entry)
        {
            *Value = Entry->PinInfo[PinIndex].Link;
            return (*Value != 0);
        }

        if (!NT_SUCCESS(HalpPirQueryRoute(Parent, &Bridge)))
            return FALSE;

        if (Bridge.BaseClass == PCI_CLASS_BRIDGE_DEV)
        {
            if (Bridge.SubClass == PCI_SUBCLASS_BR_PCI_TO_PCI)
            {
                PinIndex = (PinIndex + Slot.u.bits.DeviceNumber) % PIR_PIN_COUNT;
            }
            else if (Bridge.SubClass == PCI_SUBCLASS_BR_CARDBUS &&
                     Bridge.Pin >= 1 && Bridge.Pin <= PIR_PIN_COUNT)
            {
                PinIndex = Bridge.Pin - 1;
            }
            else
            {
                return FALSE;
            }
        }

        Bus = Bridge.Bus;
        Slot = Bridge.Slot;
        Parent = Bridge.Parent;
    }
}

/**
 * @brief
 * Returns the $PIR link a device interrupts through.
 *
 * @return
 * STATUS_SUCCESS, with Link NULL for a PCI device that has no link.
 * STATUS_NOT_FOUND for a device that is not on PCI.
 * STATUS_NOT_SUPPORTED when routing is off.
 */
NTSTATUS
NTAPI
HalpLegacyPCFindLink(
    _In_opt_ PDEVICE_OBJECT Device,
    _Out_ PHALP_PCI_LINK *Link)
{
    HALP_PCI_ROUTE Route;
    ROUTING_TOKEN Token;
    UCHAR Value;

    PAGED_CODE();

    *Link = NULL;

    if (!HalpPciIrqRoutingActive)
        return STATUS_NOT_SUPPORTED;

    if (!Device || !NT_SUCCESS(HalpPirQueryRoute(Device, &Route)))
        return STATUS_NOT_FOUND;

    if (Route.BaseClass == PCI_CLASS_MASS_STORAGE_CTLR &&
        Route.SubClass == PCI_SUBCLASS_MSC_IDE_CTLR &&
        !HalpPirIsNativeIde(&Route))
    {
        return STATUS_SUCCESS;
    }

    if (Route.Token.LinkNode)
    {
        *Link = Route.Token.LinkNode;
        return STATUS_SUCCESS;
    }

    if (!HalpPirResolveLink(&Route, &Value))
        return STATUS_SUCCESS;

    *Link = HalpPirLookupLink(Value);
    if (*Link)
    {
        RtlZeroMemory(&Token, sizeof(Token));
        Token.LinkNode = *Link;
        HalpPciIrqRouting.Interface.SetInterruptRoutingToken(Device, &Token);
    }

    return STATUS_SUCCESS;
}

PHALP_PCI_LINK
NTAPI
HalpLegacyPCFirstLink(VOID)
{
    return HalpPciIrqRoutingActive ? HalpPciIrqRouting.Links : NULL;
}

NTSTATUS
NTAPI
HalpLegacyPCGetLinkIrq(
    _In_ PHALP_PCI_LINK Link,
    _Out_ PUCHAR Irq)
{
    return HalpIrqRouter->GetIrq(Link->Link, Irq);
}

NTSTATUS
NTAPI
HalpLegacyPCSetLinkIrq(
    _In_ PHALP_PCI_LINK Link,
    _In_ UCHAR Irq)
{
    return HalpIrqRouter->SetIrq(Link->Link, Irq);
}

VOID
NTAPI
HalpLegacyPCUpdateInterruptLine(
    _In_ PDEVICE_OBJECT Device,
    _In_ UCHAR Irq)
{
    PAGED_CODE();

    HalpPciIrqRouting.Interface.UpdateInterruptLine(Device, Irq);
}

/**
 * @brief
 * Tells whether PCI interrupts are routed through the $PIR links.
 */
BOOLEAN
NTAPI
HalpLegacyPCIrqRoutingActive(VOID)
{
    return HalpPciIrqRoutingActive;
}

/* INITIALIZATION ***************************************************************/

static
NTSTATUS
HalpPirReadDword(
    _In_opt_ HANDLE RootKey,
    _In_ PCWSTR KeyPath,
    _In_ PCWSTR ValueName,
    _Out_ PULONG Value)
{
    UCHAR Buffer[FIELD_OFFSET(KEY_VALUE_PARTIAL_INFORMATION, Data) + sizeof(ULONG)];
    PKEY_VALUE_PARTIAL_INFORMATION Information = (PKEY_VALUE_PARTIAL_INFORMATION)Buffer;
    OBJECT_ATTRIBUTES ObjectAttributes;
    UNICODE_STRING Name;
    HANDLE KeyHandle;
    ULONG Length;
    NTSTATUS Status;

    PAGED_CODE();

    RtlInitUnicodeString(&Name, KeyPath);
    InitializeObjectAttributes(&ObjectAttributes,
                               &Name,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               RootKey,
                               NULL);

    Status = ZwOpenKey(&KeyHandle, KEY_READ, &ObjectAttributes);
    if (!NT_SUCCESS(Status))
        return Status;

    RtlInitUnicodeString(&Name, ValueName);
    Status = ZwQueryValueKey(KeyHandle,
                             &Name,
                             KeyValuePartialInformation,
                             Information,
                             sizeof(Buffer),
                             &Length);
    ZwClose(KeyHandle);

    if (!NT_SUCCESS(Status))
        return Status;

    if (Information->Type != REG_DWORD || Information->DataLength != sizeof(ULONG))
        return STATUS_OBJECT_TYPE_MISMATCH;

    *Value = *(PULONG)Information->Data;
    return STATUS_SUCCESS;
}

/* Looks up the miniport instance registered for a router PCI ID */
static
BOOLEAN
HalpPirMiniportForId(
    _In_ HANDLE MiniportsKey,
    _In_ ULONG Id,
    _Out_ PULONG Instance)
{
    WCHAR Name[9];

    if (Id == 0 || Id == MAXULONG)
        return FALSE;

    _swprintf(Name, L"%08lX", Id);
    return NT_SUCCESS(HalpPirReadDword(MiniportsKey, Name, L"Instance", Instance));
}

static
ULONG
HalpPirReadPciId(
    _In_ ULONG Bus,
    _In_ PCI_SLOT_NUMBER Slot)
{
    ULONG Id = MAXULONG;

    HalGetBusDataByOffset(PCIConfiguration, Bus, Slot.u.AsULONG, &Id, 0, sizeof(Id));
    return Id;
}

static
BOOLEAN
HalpPirScanBusZero(
    _In_ HANDLE MiniportsKey,
    _Out_ PPCI_SLOT_NUMBER Found,
    _Out_ PULONG Instance)
{
    PCI_SLOT_NUMBER Slot;
    UCHAR HeaderType;
    ULONG Device, Function, Id;

    Slot.u.AsULONG = 0;
    for (Device = 0; Device < PCI_MAX_DEVICES; Device++)
    {
        Slot.u.bits.DeviceNumber = Device;

        for (Function = 0; Function < PCI_MAX_FUNCTION; Function++)
        {
            Slot.u.bits.FunctionNumber = Function;

            Id = HalpPirReadPciId(0, Slot);
            if (Id == MAXULONG)
                continue;

            if (HalpPirMiniportForId(MiniportsKey, Id, Instance))
            {
                *Found = Slot;
                return TRUE;
            }

            if (Function != 0)
                continue;

            HeaderType = 0;
            HalGetBusDataByOffset(PCIConfiguration,
                                  0,
                                  Slot.u.AsULONG,
                                  &HeaderType,
                                  FIELD_OFFSET(PCI_COMMON_HEADER, HeaderType),
                                  sizeof(HeaderType));
            if (!(HeaderType & PCI_MULTIFUNCTION))
                break;
        }
    }

    return FALSE;
}

/* Indexed by the Instance value of the IrqMiniports entries */
static PHALP_IRQ_ROUTER HalpPirRouters[] =
{
    &HalpEscRouter,         /* 0x00 Intel 82375EB/SB */
    &HalpPiixRouter,        /* 0x01 Intel PIIX and ICH */
    &HalpVlsiRouter,        /* 0x02 VLSI */
    &HalpOptiViperRouter,   /* 0x03 OPTi Viper */
    &HalpSisRouter,         /* 0x04 SiS 5503 */
    &HalpVlsiEagleRouter,   /* 0x05 VLSI Eagle */
    &HalpAli1523Router,     /* 0x06 ALi M1523 */
    &HalpNs87560Router,     /* 0x07 NS 87560 */
    &HalpCompaqMisc3Router, /* 0x08 Compaq MISC-3 */
    &HalpAli1533Router,     /* 0x09 ALi M1533 */
    &HalpOptiFireStarRouter,/* 0x0A OPTi FireStar */
    &HalpViaRouter,         /* 0x0B VIA VT82C586B, VT82C596B, VT82C686B */
    &HalpCompaqOsbRouter,   /* 0x0C Compaq OSB */
    &HalpCompaqCmc2Router,  /* 0x0D Compaq CMC-2 */
    &HalpCx5520Router,      /* 0x0E Cyrix 5520 */
    &HalpToshibaRouter,     /* 0x0F Toshiba */
    NULL,                   /* 0x10 NEC, not supported */
    &HalpVesuviusRouter,    /* 0x11 PicoPower Vesuvius */
};

/*
 * The router is picked from the IrqMiniports key: an Override entry first,
 * then the device at the $PIR router location, the compatible router the
 * table names, and finally any listed device on bus 0.
 */
static
PHALP_IRQ_ROUTER
HalpPirFindRouter(
    _In_opt_ PPCI_IRQ_ROUTING_TABLE Table)
{
    OBJECT_ATTRIBUTES ObjectAttributes;
    UNICODE_STRING KeyName = RTL_CONSTANT_STRING(HALP_MINIPORTS_KEY);
    PCI_SLOT_NUMBER Slot;
    HANDLE MiniportsKey;
    ULONG Bus = 0, Compatible = MAXULONG, Instance;
    BOOLEAN Found;

    PAGED_CODE();

    InitializeObjectAttributes(&ObjectAttributes,
                               &KeyName,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               NULL,
                               NULL);
    if (!NT_SUCCESS(ZwOpenKey(&MiniportsKey, KEY_READ, &ObjectAttributes)))
        return NULL;

    Slot.u.AsULONG = 0;
    if (Table)
    {
        Bus = Table->RouterBus;
        Slot.u.bits.DeviceNumber = Table->RouterDevFunc >> 3;
        Slot.u.bits.FunctionNumber = Table->RouterDevFunc & 7;
        Compatible = Table->CompatibleRouter;
    }

    Found = NT_SUCCESS(HalpPirReadDword(MiniportsKey, L"Override", L"Instance", &Instance)) ||
            HalpPirMiniportForId(MiniportsKey, HalpPirReadPciId(Bus, Slot), &Instance) ||
            HalpPirMiniportForId(MiniportsKey, Compatible, &Instance);

    if (!Found)
    {
        Bus = 0;
        Found = HalpPirScanBusZero(MiniportsKey, &Slot, &Instance);
    }

    ZwClose(MiniportsKey);

    if (!Found)
        return NULL;

    if (Instance >= RTL_NUMBER_OF(HalpPirRouters) || !HalpPirRouters[Instance])
    {
        DPRINT1("HAL: IRQ miniport instance %lu is not supported\n", Instance);
        return NULL;
    }

    HalpPciIrqRouting.RouterBus = Bus;
    HalpPciIrqRouting.RouterSlot = Slot;
    return HalpPirRouters[Instance];
}

static
NTSTATUS
HalpPirQueryInterface(
    _In_ PDEVICE_OBJECT PciPdo,
    _Out_ PINT_ROUTE_INTERFACE_STANDARD Interface)
{
    PIO_STACK_LOCATION Stack;
    IO_STATUS_BLOCK IoStatus;
    PDEVICE_OBJECT TopDevice;
    KEVENT Event;
    NTSTATUS Status;
    PIRP Irp;

    PAGED_CODE();

    RtlZeroMemory(Interface, sizeof(*Interface));

    TopDevice = IoGetAttachedDeviceReference(PciPdo);
    KeInitializeEvent(&Event, NotificationEvent, FALSE);

    Irp = IoBuildSynchronousFsdRequest(IRP_MJ_PNP, TopDevice, NULL, 0, NULL, &Event, &IoStatus);
    if (!Irp)
    {
        ObDereferenceObject(TopDevice);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;

    Stack = IoGetNextIrpStackLocation(Irp);
    Stack->MinorFunction = IRP_MN_QUERY_INTERFACE;
    Stack->Parameters.QueryInterface.InterfaceType = &GUID_INT_ROUTE_INTERFACE_STANDARD;
    Stack->Parameters.QueryInterface.Size = sizeof(*Interface);
    Stack->Parameters.QueryInterface.Version = PCI_INT_ROUTE_INTRF_STANDARD_VER;
    Stack->Parameters.QueryInterface.Interface = (PINTERFACE)Interface;
    Stack->Parameters.QueryInterface.InterfaceSpecificData = NULL;

    Status = IoCallDriver(TopDevice, Irp);
    if (Status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
        Status = IoStatus.Status;
    }

    ObDereferenceObject(TopDevice);

    if (NT_SUCCESS(Status) &&
        (!Interface->GetInterruptRouting ||
         !Interface->SetInterruptRoutingToken ||
         !Interface->UpdateInterruptLine))
    {
        Status = STATUS_NOT_SUPPORTED;
    }

    return Status;
}

/**
 * @brief
 * Sets up the PCI IRQ router and the $PIR links when the PCI bus starts.
 *
 * @param[in] PciPdo
 * The HAL PDO the PCI bus driver sits on.
 */
CODE_SEG("PAGE")
VOID
NTAPI
HalpLegacyPCInitIrqRouting(
    _In_ PDEVICE_OBJECT PciPdo)
{
    PPCI_IRQ_ROUTING_TABLE Table = NULL;
    PHALP_IRQ_ROUTER Router;
    ULONG Options = 0, BiosAttributes = 0;
    NTSTATUS Status;

    PAGED_CODE();

    /* $PIR only covers the 8259 inputs */
    if (HalpIrqRouter || HalpInterruptControllerType != 0 || InitSafeBootMode)
        return;

    /* Devices are matched to links through the PCI bus driver */
    Status = HalpPirQueryInterface(PciPdo, &HalpPciIrqRouting.Interface);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("HAL: No PCI interrupt routing interface (Status 0x%08lx)\n", Status);
        return;
    }

    HalpPirReadDword(NULL, HALP_ROUTING_KEY, L"Options", &Options);
    HalpPirReadDword(NULL, HALP_BIOS_ROUTING_KEY, L"Attributes", &BiosAttributes);

    if ((Options & HALP_ROUTING_ENABLED) &&
        (Options & HALP_ROUTING_USE_PIR) &&
        !(BiosAttributes & HALP_BIOS_PIR_BROKEN))
    {
        Table = HalpPirFindTable();
    }

    /* The router also owns the ELCR, so it is used even without links */
    Router = HalpPirFindRouter(Table);
    if (!Router)
    {
        DPRINT1("HAL: No supported PCI IRQ router\n");
        if (Table)
            ExFreePoolWithTag(Table, TAG_HAL);
        return;
    }

    HalpIrqRouter = Router;

    if (!Table)
    {
        DPRINT1("HAL: No usable $PIR table\n");
        return;
    }

    Status = Router->ValidateTable(Table);
    if (NT_SUCCESS(Status))
        Status = HalpPirBuildLinks(Table);

    if (!NT_SUCCESS(Status))
    {
        DPRINT1("HAL: PCI IRQ routing is not used (Status 0x%08lx)\n", Status);
        ExFreePoolWithTag(Table, TAG_HAL);
        return;
    }

    HalpPciIrqRouting.Table = Table;
    HalpPciIrqRoutingActive = TRUE;
}
