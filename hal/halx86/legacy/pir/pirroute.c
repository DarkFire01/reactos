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

#define HALP_ROUTING_KEY        L"\\Registry\\Machine\\System\\CurrentControlSet" \
                                L"\\Control\\Pnp\\PciIrqRouting"
#define HALP_MINIPORTS_KEY      HALP_ROUTING_KEY L"\\IrqMiniports"
#define HALP_BIOS_ROUTING_KEY   L"\\Registry\\Machine\\System\\CurrentControlSet" \
                                L"\\Control\\BiosInfo\\PciIrqRouting"

/* PciIrqRouting Options */
#define HALP_ROUTING_ENABLED    0x01
#define HALP_ROUTING_USE_PIR    0x04

/* BiosInfo PciIrqRouting Attributes */
#define HALP_BIOS_PIR_BROKEN    0x04

/* IrqMiniports Parameters */
#define HALP_MINIPORT_UNIQUE_LINK_IRQS  0x01

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

/* Called for each function on bus 0, returns FALSE to stop the walk */
typedef BOOLEAN
(NTAPI *PHALP_PIR_BUS_ZERO_VISIT)(
    _In_ PCI_SLOT_NUMBER Slot,
    _In_ PPCI_COMMON_HEADER Header,
    _Inout_ PVOID Context);

static struct
{
    BOOLEAN Active;
    ULONG RouterBus;
    PCI_SLOT_NUMBER RouterSlot;
    ULONG Parameters;
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

/* For the routers that leave the trigger modes in the EISA edge/level register */
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

/* PCI CONFIGURATION ************************************************************/

/* Reads the common header, FALSE when no function answers at Slot */
static
BOOLEAN
NTAPI
HalpPirReadHeader(
    _In_ ULONG Bus,
    _In_ PCI_SLOT_NUMBER Slot,
    _Out_ PPCI_COMMON_HEADER Header)
{
    RtlFillMemory(Header, sizeof(*Header), 0xFF);
    HalGetBusDataByOffset(PCIConfiguration,
                          Bus,
                          Slot.u.AsULONG,
                          Header,
                          0,
                          PCI_COMMON_HDR_LENGTH);

    return Header->VendorID != PCI_INVALID_VENDORID &&
           Header->DeviceID != PCI_INVALID_VENDORID;
}

static
BOOLEAN
NTAPI
HalpPirWalkBusZero(
    _In_ PHALP_PIR_BUS_ZERO_VISIT Visit,
    _Inout_ PVOID Context)
{
    PCI_COMMON_HEADER Header;
    PCI_SLOT_NUMBER Slot;
    ULONG Device, Function, FunctionLimit;

    Slot.u.AsULONG = 0;
    for (Device = 0; Device < PCI_MAX_DEVICES; Device++)
    {
        Slot.u.bits.DeviceNumber = Device;
        FunctionLimit = PCI_MAX_FUNCTION;

        for (Function = 0; Function < FunctionLimit; Function++)
        {
            Slot.u.bits.FunctionNumber = Function;

            if (!HalpPirReadHeader(0, Slot, &Header))
                continue;

            if (!Visit(Slot, &Header, Context))
                return FALSE;

            /* A single function device only decodes function 0 */
            if (Function == 0 && !PCI_MULTIFUNCTION_DEVICE(&Header))
                FunctionLimit = 1;
        }
    }

    return TRUE;
}

/* TABLE ************************************************************************/

static
PSLOT_INFO
NTAPI
HalpPirSlotEnd(
    _In_ PPCI_IRQ_ROUTING_TABLE Table)
{
    return (PSLOT_INFO)((PUCHAR)Table + Table->TableSize);
}

/* Lowest and highest nonzero link values, both zero when nothing is routed */
VOID
NTAPI
HalpPirLinkRange(
    _In_ PPCI_IRQ_ROUTING_TABLE Table,
    _Out_ PUCHAR Lowest,
    _Out_ PUCHAR Highest)
{
    PSLOT_INFO Slot;
    ULONG Pin;

    *Lowest = MAXUCHAR;
    *Highest = 0;

    for (Slot = Table->Slot; Slot < HalpPirSlotEnd(Table); Slot++)
    {
        for (Pin = 0; Pin < PIR_PIN_COUNT; Pin++)
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

/* Tells whether every routed pin of the table names a link the router knows */
BOOLEAN
NTAPI
HalpPirAllLinks(
    _In_ PPCI_IRQ_ROUTING_TABLE Table,
    _In_ BOOLEAN (NTAPI *IsValid)(_In_ UCHAR Link))
{
    PSLOT_INFO Slot;
    ULONG Pin;

    for (Slot = Table->Slot; Slot < HalpPirSlotEnd(Table); Slot++)
    {
        for (Pin = 0; Pin < PIR_PIN_COUNT; Pin++)
        {
            UCHAR Link = Slot->PinInfo[Pin].Link;

            if (Link != 0 && !IsValid(Link))
                return FALSE;
        }
    }

    return TRUE;
}

static
VOID
NTAPI
HalpPirDropSlot(
    _Inout_ PPCI_IRQ_ROUTING_TABLE Table,
    _Inout_ PSLOT_INFO Slot)
{
    *Slot = *(HalpPirSlotEnd(Table) - 1);
    Table->TableSize = (USHORT)(Table->TableSize - sizeof(*Slot));
}

static
BOOLEAN
NTAPI
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
NTAPI
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
NTAPI
HalpPirSameDevice(
    _In_ PSLOT_INFO First,
    _In_ PSLOT_INFO Second)
{
    return First->BusNumber == Second->BusNumber &&
           (First->DeviceNumber >> 3) == (Second->DeviceNumber >> 3);
}

/* Drops unconnected pins and empty entries, and folds duplicate entries together */
static
BOOLEAN
NTAPI
HalpPirCleanTable(
    _Inout_ PPCI_IRQ_ROUTING_TABLE Table)
{
    PSLOT_INFO Slot, Other;
    ULONG Pin;

    /* A pin that can only reach IRQ 0 is not wired */
    for (Slot = Table->Slot; Slot < HalpPirSlotEnd(Table); Slot++)
    {
        for (Pin = 0; Pin < PIR_PIN_COUNT; Pin++)
        {
            if ((Slot->PinInfo[Pin].InterruptMap & ~1) == 0)
                RtlZeroMemory(&Slot->PinInfo[Pin], sizeof(Slot->PinInfo[Pin]));
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

        for (Other = Slot + 1; Other < HalpPirSlotEnd(Table);)
        {
            if (!HalpPirSameDevice(Slot, Other))
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

/* Tells whether any entry for Device routes one of the pins in PinMask, on any bus */
static
BOOLEAN
NTAPI
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

/* Pins of a bus 0 function that the table has to route, zero for none */
static
ULONG
NTAPI
HalpPirPinsToCheck(
    _In_ PPCI_COMMON_HEADER Header,
    _In_ BOOLEAN SecondaryBuses)
{
    UCHAR Pin = Header->u.type0.InterruptPin;
    UCHAR Line = Header->u.type0.InterruptLine;
    BOOLEAN Decodes, LineUsable;

    /* Compatibility mode storage uses ISA IRQs. The class test is loose on purpose. */
    if (!(Header->ProgIf & HALP_IDE_NATIVE_MODES))
    {
        if (Header->BaseClass == PCI_CLASS_MASS_STORAGE_CTLR)
            return 0;

        if (Header->SubClass == PCI_SUBCLASS_MSC_IDE_CTLR)
            return 0;
    }

    /* Without secondary bus entries a bridge needs at least one routed pin */
    if (PCI_CONFIGURATION_TYPE(Header) == PCI_BRIDGE_TYPE &&
        Header->BaseClass == PCI_CLASS_BRIDGE_DEV &&
        Header->SubClass == PCI_SUBCLASS_BR_PCI_TO_PCI)
    {
        return SecondaryBuses ? 0 : PIR_ALL_PINS;
    }

    if (Pin < 1 || Pin > PIR_PIN_COUNT)
        return 0;

    /* A decoding function the BIOS left without a line is not expected in the table */
    Decodes = (Header->Command & (PCI_ENABLE_IO_SPACE | PCI_ENABLE_MEMORY_SPACE)) != 0;
    LineUsable = (Line >= 1 && Line < HALP_PIC_IRQ_COUNT);
    if (Decodes && !LineUsable)
        return 0;

    return 1 << (Pin - 1);
}

typedef struct _HALP_PIR_CHECK
{
    PPCI_IRQ_ROUTING_TABLE Table;
    BOOLEAN SecondaryBuses;
} HALP_PIR_CHECK, *PHALP_PIR_CHECK;

static
BOOLEAN
NTAPI
HalpPirCheckFunction(
    _In_ PCI_SLOT_NUMBER Slot,
    _In_ PPCI_COMMON_HEADER Header,
    _Inout_ PVOID Context)
{
    PHALP_PIR_CHECK Check = Context;
    ULONG Pins;

    Pins = HalpPirPinsToCheck(Header, Check->SecondaryBuses);
    if (Pins == 0)
        return TRUE;

    return HalpPirListsPins(Check->Table, Slot.u.bits.DeviceNumber, Pins);
}

/* Rejects a table that misses interrupts the bus 0 functions are known to use */
static
BOOLEAN
NTAPI
HalpPirMatchesBusZero(
    _In_ PPCI_IRQ_ROUTING_TABLE Table)
{
    HALP_PIR_CHECK Check;
    PSLOT_INFO Slot;

    Check.Table = Table;
    Check.SecondaryBuses = FALSE;

    for (Slot = Table->Slot; Slot < HalpPirSlotEnd(Table); Slot++)
    {
        if (Slot->BusNumber != 0)
        {
            Check.SecondaryBuses = TRUE;
            break;
        }
    }

    return HalpPirWalkBusZero(HalpPirCheckFunction, &Check);
}

static
BOOLEAN
NTAPI
HalpPirHeaderValid(
    _In_ PPCI_IRQ_ROUTING_TABLE Candidate,
    _In_ ULONG Available)
{
    ULONG Size = Candidate->TableSize;
    UCHAR Sum = 0;
    ULONG Index;

    if (Candidate->Signature != PIR_SIGNATURE || Candidate->Version != PIR_VERSION)
        return FALSE;

    if (Size > Available || Size <= PIR_HEADER_SIZE || (Size % sizeof(SLOT_INFO)) != 0)
        return FALSE;

    for (Index = 0; Index < Size; Index++)
        Sum = (UCHAR)(Sum + ((PUCHAR)Candidate)[Index]);

    return (Sum == 0);
}

static
PPCI_IRQ_ROUTING_TABLE
NTAPI
HalpPirCaptureTable(
    _In_ PPCI_IRQ_ROUTING_TABLE Candidate,
    _In_ ULONG Available)
{
    PPCI_IRQ_ROUTING_TABLE Table;

    if (!HalpPirHeaderValid(Candidate, Available))
        return NULL;

    Table = ExAllocatePoolWithTag(NonPagedPool, Candidate->TableSize, TAG_HAL);
    if (!Table)
        return NULL;

    RtlCopyMemory(Table, Candidate, Candidate->TableSize);

    if (HalpPirCleanTable(Table) && HalpPirMatchesBusZero(Table))
        return Table;

    DPRINT1("HAL: $PIR table does not match the hardware, ignoring it\n");
    ExFreePoolWithTag(Table, TAG_HAL);
    return NULL;
}

/* Scans the BIOS segment on 16 byte boundaries */
static
PPCI_IRQ_ROUTING_TABLE
NTAPI
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

    for (Offset = 0; Offset + PIR_HEADER_SIZE <= PIR_BIOS_LENGTH; Offset += PIR_ALIGNMENT)
    {
        Table = HalpPirCaptureTable((PPCI_IRQ_ROUTING_TABLE)(Bios + Offset),
                                    PIR_BIOS_LENGTH - Offset);
        if (Table)
            break;
    }

    MmUnmapIoSpace(Bios, PIR_BIOS_LENGTH);
    return Table;
}

static
PSLOT_INFO
NTAPI
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
NTAPI
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
NTAPI
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

/* One link per distinct link value, its IRQ mask taken from the first pin naming it */
static
NTSTATUS
NTAPI
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
NTAPI
HalpPirQueryRoute(
    _In_ PDEVICE_OBJECT Device,
    _Out_ PHALP_PCI_ROUTE Route)
{
    UCHAR Line, Flags;

    RtlZeroMemory(Route, sizeof(*Route));

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

/* The controller must still report the same class and run a channel in native mode */
static
BOOLEAN
NTAPI
HalpPirIsNativeIde(
    _In_ PHALP_PCI_ROUTE Route)
{
    PCI_COMMON_HEADER Header;

    if (!HalpPirReadHeader(Route->Bus, Route->Slot, &Header))
        return FALSE;

    return Header.BaseClass == Route->BaseClass &&
           Header.SubClass == Route->SubClass &&
           (Header.ProgIf & HALP_IDE_NATIVE_MODES) != 0;
}

/* Walks the pin up through bridges to a bus the table lists */
static
BOOLEAN
NTAPI
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

    *Value = 0;

    if (Route->Pin < 1 || Route->Pin > PIR_PIN_COUNT)
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

        /* Other parents pass the pin through unchanged */
        if (Bridge.BaseClass == PCI_CLASS_BRIDGE_DEV)
        {
            if (Bridge.SubClass == PCI_SUBCLASS_BR_PCI_TO_PCI)
            {
                /* Standard swizzle across a PCI to PCI bridge */
                PinIndex = (PinIndex + Slot.u.bits.DeviceNumber) % PIR_PIN_COUNT;
            }
            else if (Bridge.SubClass == PCI_SUBCLASS_BR_CARDBUS &&
                     Bridge.Pin >= 1 && Bridge.Pin <= PIR_PIN_COUNT)
            {
                /* CardBus functions share the interrupt pin of the socket */
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

    if (!HalpPciIrqRouting.Active)
        return STATUS_NOT_SUPPORTED;

    if (!Device || !NT_SUCCESS(HalpPirQueryRoute(Device, &Route)))
        return STATUS_NOT_FOUND;

    /* Compatibility mode IDE uses the ISA IRQs 14 and 15 */
    if (Route.BaseClass == PCI_CLASS_MASS_STORAGE_CTLR &&
        Route.SubClass == PCI_SUBCLASS_MSC_IDE_CTLR &&
        !HalpPirIsNativeIde(&Route))
    {
        return STATUS_SUCCESS;
    }

    /* The PCI driver keeps the link found earlier */
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
    return HalpPciIrqRouting.Active ? HalpPciIrqRouting.Links : NULL;
}

/*
 * Link and trigger accesses run at DISPATCH_LEVEL or above, so on this
 * uniprocessor HAL they never interleave through a shared index register.
 */
NTSTATUS
NTAPI
HalpLegacyPCGetLinkIrq(
    _In_ PHALP_PCI_LINK Link,
    _Out_ PUCHAR Irq)
{
    NTSTATUS Status;
    KIRQL OldIrql;

    *Irq = 0;

    KeRaiseIrql((KIRQL)max(KeGetCurrentIrql(), DISPATCH_LEVEL), &OldIrql);
    Status = HalpIrqRouter->GetIrq(Link->Link, Irq);
    KeLowerIrql(OldIrql);

    return Status;
}

NTSTATUS
NTAPI
HalpLegacyPCSetLinkIrq(
    _In_ PHALP_PCI_LINK Link,
    _In_ UCHAR Irq)
{
    NTSTATUS Status;
    KIRQL OldIrql;

    KeRaiseIrql((KIRQL)max(KeGetCurrentIrql(), DISPATCH_LEVEL), &OldIrql);
    Status = HalpIrqRouter->SetIrq(Link->Link, Irq);
    KeLowerIrql(OldIrql);

    return Status;
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
    return HalpPciIrqRouting.Active;
}

/**
 * @brief
 * Tells whether the router miniport wants every link on an IRQ of its own.
 */
BOOLEAN
NTAPI
HalpLegacyPCLinkIrqsExclusive(VOID)
{
    return (HalpPciIrqRouting.Parameters & HALP_MINIPORT_UNIQUE_LINK_IRQS) != 0;
}

/* INITIALIZATION ***************************************************************/

/* Value is zero when the DWORD cannot be read */
static
NTSTATUS
NTAPI
HalpPirReadDword(
    _In_opt_ HANDLE RootKey,
    _In_ PCWSTR KeyPath,
    _In_ PCWSTR ValueName,
    _Out_ PULONG Value)
{
    union
    {
        KEY_VALUE_PARTIAL_INFORMATION Information;
        UCHAR Space[FIELD_OFFSET(KEY_VALUE_PARTIAL_INFORMATION, Data) + sizeof(ULONG)];
    } Buffer;
    OBJECT_ATTRIBUTES ObjectAttributes;
    UNICODE_STRING Name;
    HANDLE KeyHandle;
    ULONG Length;
    NTSTATUS Status;

    PAGED_CODE();

    *Value = 0;

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
                             &Buffer,
                             sizeof(Buffer),
                             &Length);
    ZwClose(KeyHandle);

    if (!NT_SUCCESS(Status))
        return Status;

    if (Buffer.Information.Type != REG_DWORD || Buffer.Information.DataLength != sizeof(ULONG))
        return STATUS_OBJECT_TYPE_MISMATCH;

    RtlCopyMemory(Value, Buffer.Information.Data, sizeof(*Value));
    return STATUS_SUCCESS;
}

/* Looks up the miniport registered for a router PCI ID and its parameters */
static
BOOLEAN
NTAPI
HalpPirMiniportForId(
    _In_ HANDLE MiniportsKey,
    _In_ ULONG Id,
    _Out_ PULONG Instance,
    _Out_ PULONG Parameters)
{
    WCHAR Name[9];

    *Instance = 0;
    *Parameters = 0;

    if (Id == 0 || Id == MAXULONG)
        return FALSE;

    _swprintf(Name, L"%08lX", Id);
    if (!NT_SUCCESS(HalpPirReadDword(MiniportsKey, Name, L"Instance", Instance)))
        return FALSE;

    HalpPirReadDword(MiniportsKey, Name, L"Parameters", Parameters);
    return TRUE;
}

static
ULONG
NTAPI
HalpPirReadPciId(
    _In_ ULONG Bus,
    _In_ PCI_SLOT_NUMBER Slot)
{
    ULONG Id = MAXULONG;

    HalGetBusDataByOffset(PCIConfiguration, Bus, Slot.u.AsULONG, &Id, 0, sizeof(Id));
    return Id;
}

typedef struct _HALP_PIR_ROUTER_SEARCH
{
    HANDLE MiniportsKey;
    PCI_SLOT_NUMBER Slot;
    ULONG Instance;
    ULONG Parameters;
    BOOLEAN Found;
} HALP_PIR_ROUTER_SEARCH, *PHALP_PIR_ROUTER_SEARCH;

static
BOOLEAN
NTAPI
HalpPirMatchRouter(
    _In_ PCI_SLOT_NUMBER Slot,
    _In_ PPCI_COMMON_HEADER Header,
    _Inout_ PVOID Context)
{
    PHALP_PIR_ROUTER_SEARCH Search = Context;
    ULONG Id = ((ULONG)Header->DeviceID << 16) | Header->VendorID;

    if (!HalpPirMiniportForId(Search->MiniportsKey, Id, &Search->Instance, &Search->Parameters))
        return TRUE;

    Search->Slot = Slot;
    Search->Found = TRUE;
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
    &HalpOptiFireStarRouter, /* 0x0A OPTi FireStar */
    &HalpViaRouter,         /* 0x0B VIA VT82C586B, VT82C596B and VT82C686B */
    &HalpCompaqOsbRouter,   /* 0x0C Compaq OSB */
    &HalpCompaqCmc2Router,  /* 0x0D Compaq CMC-2 */
    &HalpCx5520Router,      /* 0x0E Cyrix 5520 */
    &HalpToshibaRouter,     /* 0x0F Toshiba */
    NULL,                   /* 0x10 NEC, not supported */
    &HalpVesuviusRouter,    /* 0x11 PicoPower Vesuvius */
    &HalpAtiRouter,         /* 0x12 ATI SB600 and AMD SB7x0 */
};

/*
 * The router is picked from the IrqMiniports key. An Override entry wins and
 * uses the $PIR router location. Otherwise the device at that location, then
 * the compatible router the table names, then the first listed bus 0 device.
 */
static
PHALP_IRQ_ROUTER
NTAPI
HalpPirFindRouter(
    _In_opt_ PPCI_IRQ_ROUTING_TABLE Table)
{
    OBJECT_ATTRIBUTES ObjectAttributes;
    UNICODE_STRING KeyName = RTL_CONSTANT_STRING(HALP_MINIPORTS_KEY);
    HALP_PIR_ROUTER_SEARCH Search;
    ULONG Bus = 0, Compatible = MAXULONG;

    PAGED_CODE();

    RtlZeroMemory(&Search, sizeof(Search));

    InitializeObjectAttributes(&ObjectAttributes,
                               &KeyName,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               NULL,
                               NULL);
    if (!NT_SUCCESS(ZwOpenKey(&Search.MiniportsKey, KEY_READ, &ObjectAttributes)))
        return NULL;

    if (Table)
    {
        Bus = Table->RouterBus;
        Search.Slot.u.bits.DeviceNumber = Table->RouterDevFunc >> 3;
        Search.Slot.u.bits.FunctionNumber = Table->RouterDevFunc & 7;
        Compatible = Table->CompatibleRouter;
    }

    if (NT_SUCCESS(HalpPirReadDword(Search.MiniportsKey, L"Override", L"Instance", &Search.Instance)))
    {
        Search.Found = TRUE;
    }
    else if (HalpPirMiniportForId(Search.MiniportsKey,
                                  HalpPirReadPciId(Bus, Search.Slot),
                                  &Search.Instance,
                                  &Search.Parameters) ||
             HalpPirMiniportForId(Search.MiniportsKey,
                                  Compatible,
                                  &Search.Instance,
                                  &Search.Parameters))
    {
        Search.Found = TRUE;
    }
    else
    {
        Bus = 0;
        HalpPirWalkBusZero(HalpPirMatchRouter, &Search);
    }

    ZwClose(Search.MiniportsKey);

    if (!Search.Found)
        return NULL;

    if (Search.Instance >= RTL_NUMBER_OF(HalpPirRouters) || !HalpPirRouters[Search.Instance])
    {
        DPRINT1("HAL: IRQ miniport instance %lu is not supported\n", Search.Instance);
        return NULL;
    }

    HalpPciIrqRouting.RouterBus = Bus;
    HalpPciIrqRouting.RouterSlot = Search.Slot;
    HalpPciIrqRouting.Parameters = Search.Parameters;
    return HalpPirRouters[Search.Instance];
}

static
NTSTATUS
NTAPI
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
        if (Interface->InterfaceDereference)
            Interface->InterfaceDereference(Interface->Context);

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
    PINT_ROUTE_INTERFACE_STANDARD Interface = &HalpPciIrqRouting.Interface;
    PPCI_IRQ_ROUTING_TABLE Table = NULL;
    PHALP_IRQ_ROUTER Router;
    ULONG Options, BiosAttributes;
    NTSTATUS Status;

    PAGED_CODE();

    /* $PIR only covers the 8259 inputs */
    if (HalpIrqRouter || HalpInterruptControllerType != HALP_INTERRUPT_CONTROLLER_PIC || InitSafeBootMode)
        return;

    /* Devices are matched to links through the PCI bus driver */
    Status = HalpPirQueryInterface(PciPdo, Interface);
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

    /* The router owns the trigger modes, so it is used even without links */
    Router = HalpPirFindRouter(Table);
    if (!Router)
    {
        DPRINT1("HAL: No supported PCI IRQ router\n");
        Status = STATUS_NOT_FOUND;
    }
    else if (!Table)
    {
        DPRINT1("HAL: No usable $PIR table\n");
        Status = STATUS_NOT_FOUND;
    }
    else
    {
        Status = Router->ValidateTable(Table);
        if (NT_SUCCESS(Status))
            Status = HalpPirBuildLinks(Table);
    }

    if (!NT_SUCCESS(Status))
    {
        if (Table)
        {
            DPRINT1("HAL: PCI IRQ routing is not used (Status 0x%08lx)\n", Status);
            ExFreePoolWithTag(Table, TAG_HAL);
        }

        if (Interface->InterfaceDereference)
            Interface->InterfaceDereference(Interface->Context);
        RtlZeroMemory(Interface, sizeof(*Interface));
    }

    /* The trigger path only sees the router once the table checks are done with it */
    HalpIrqRouter = Router;

    if (NT_SUCCESS(Status))
    {
        HalpPciIrqRouting.Table = Table;
        HalpPciIrqRouting.Active = TRUE;
    }
}
