/*
 * PROJECT:     ReactOS HAL
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Legacy PC PCI IRQ routing table and links
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <hal.h>
#include <wdmguid.h>

#define NDEBUG
#include <debug.h>

#define PIR_BIOS_BASE           0xF0000
#define PIR_BIOS_LENGTH         0x10000
#define PIR_ALIGNMENT           16
#define PIR_VERSION             0x0100
#define PIR_HEADER_SIZE         FIELD_OFFSET(PCI_IRQ_ROUTING_TABLE, Slot)
#define PIR_PIN_COUNT           4
#define PIR_ALL_PINS            0x0F

#define HALP_PIC_IRQ_COUNT      16
#define HALP_IDE_NATIVE_MODES   0x05

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
    BOOLEAN Active;
    PHALP_IRQ_ROUTER Router;
    PPCI_IRQ_ROUTING_TABLE Table;
    PHALP_PCI_LINK Links;
    INT_ROUTE_INTERFACE_STANDARD Interface;
} HalpPciIrqRouting;

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

    if (Candidate->Signature != PCI_IRQ_ROUTING_TABLE_SIGNATURE ||
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

    if (!HalpPciIrqRouting.Active)
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
    return HalpPciIrqRouting.Active ? HalpPciIrqRouting.Links : NULL;
}

NTSTATUS
NTAPI
HalpLegacyPCGetLinkIrq(
    _In_ PHALP_PCI_LINK Link,
    _Out_ PUCHAR Irq)
{
    return HalpPciIrqRouting.Router->GetIrq(Link->Link, Irq);
}

NTSTATUS
NTAPI
HalpLegacyPCSetLinkIrq(
    _In_ PHALP_PCI_LINK Link,
    _In_ UCHAR Irq)
{
    return HalpPciIrqRouting.Router->SetIrq(Link->Link, Irq);
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

/* INITIALIZATION ***************************************************************/

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
    PPCI_IRQ_ROUTING_TABLE Table;
    PHALP_IRQ_ROUTER Router;
    NTSTATUS Status;

    PAGED_CODE();

    if (HalpPciIrqRouting.Router)
        return;

    /* $PIR only covers the 8259 inputs */
    if (HalpInterruptControllerType != 0)
        return;

    Table = HalpPirFindTable();

    Router = HalpIntelFindRouter(Table);
    if (!Router)
    {
        DPRINT1("HAL: No supported PCI IRQ router\n");
        if (Table)
            ExFreePoolWithTag(Table, TAG_HAL);
        return;
    }

    HalpPciIrqRouting.Router = Router;
    HalpIrqRouterInitialized = TRUE;

    if (!Table)
    {
        DPRINT1("HAL: No usable $PIR table\n");
        return;
    }

    Status = HalpPirQueryInterface(PciPdo, &HalpPciIrqRouting.Interface);
    if (NT_SUCCESS(Status))
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
    HalpPciIrqRouting.Active = TRUE;
}
