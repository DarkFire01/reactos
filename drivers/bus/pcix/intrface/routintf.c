/*
 * PROJECT:         ReactOS PCI Bus Driver
 * LICENSE:         BSD - See COPYING.ARM in the top level directory
 * FILE:            drivers/bus/pci/intrface/routinf.c
 * PURPOSE:         Routing Interface
 * PROGRAMMERS:     ReactOS Portable Systems Group
 */

/* INCLUDES *******************************************************************/

#include <pci.h>

#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

PPCI_LEGACY_DEVICE PciLegacyDeviceHead;

PCI_INTERFACE PciRoutingInterface =
{
    &GUID_INT_ROUTE_INTERFACE_STANDARD,
    sizeof(INT_ROUTE_INTERFACE_STANDARD),
    PCI_INT_ROUTE_INTRF_STANDARD_VER,
    PCI_INT_ROUTE_INTRF_STANDARD_VER,
    PCI_INTERFACE_FDO,
    0,
    PciInterface_IntRouteHandler,
    routeintrf_Constructor,
    routeintrf_Initializer
};

/* FUNCTIONS ******************************************************************/

NTSTATUS
NTAPI
routeintrf_Initializer(IN PVOID Instance)
{
    UNREFERENCED_PARAMETER(Instance);
    /* PnP Interfaces don't get Initialized */
    ASSERTMSG("PCI routeintrf_Initializer, unexpected call.\n", FALSE);
    return STATUS_UNSUCCESSFUL;
}

/*
 * Legacy PCI interrupt routing ($PIR) support.
 *
 * On a system booted with the legacy PIC HAL (no ACPI, no I/O APIC) the kernel
 * root IRQ arbiter (IopRootIrqArbiter) hands out PIC IRQs, but nothing teaches
 * it which IRQs a given PCI interrupt pin may legally use, and nothing programs
 * the chipset's PCI interrupt router.  Worse, some BIOSes leave a bogus value
 * (e.g. an out-of-PIC-range number) in a device's InterruptLine register,
 * which then fails to translate to a vector.
 *
 * The BIOS publishes a PCI IRQ Routing Table ($PIR) describing, per device
 * pin, which router "link" it is wired to and the bitmask of legacy IRQs that
 * link may be steered to.  freeldr scans for it and ntoskrnl stashes it in the
 * registry; PciGetIrqRoutingTableFromRegistry() loads it into the global
 * PciIrqRoutingTable.  Here we use it to compute a valid PIC IRQ for each
 * device, repairing the InterruptLine so the normal arbiter + HAL translator
 * path succeeds.
 */

/* Remember which IRQ we (or the BIOS) have steered each router link to, so
 * sibling devices sharing the same link get the same IRQ. */
typedef struct _PCI_ROUTED_LINK
{
    UCHAR Link;
    UCHAR Irq;
} PCI_ROUTED_LINK;

static PCI_ROUTED_LINK PciRoutedLinks[32];
static ULONG PciRoutedLinkCount;

static
UCHAR
PciLinkCacheGet(IN UCHAR Link)
{
    ULONG i;
    for (i = 0; i < PciRoutedLinkCount; i++)
        if (PciRoutedLinks[i].Link == Link) return PciRoutedLinks[i].Irq;
    return 0;
}

static
VOID
PciLinkCacheSet(IN UCHAR Link, IN UCHAR Irq)
{
    ULONG i;
    for (i = 0; i < PciRoutedLinkCount; i++)
    {
        if (PciRoutedLinks[i].Link == Link)
        {
            PciRoutedLinks[i].Irq = Irq;
            return;
        }
    }
    if (PciRoutedLinkCount < RTL_NUMBER_OF(PciRoutedLinks))
    {
        PciRoutedLinks[PciRoutedLinkCount].Link = Link;
        PciRoutedLinks[PciRoutedLinkCount].Irq = Irq;
        PciRoutedLinkCount++;
    }
}

/* Find the $PIR slot entry for a given (bus, device).  The table stores the
 * device number in the upper 5 bits per the PCI IRQ Routing Table spec. */
static
PSLOT_INFO
PciFindRoutingEntry(IN ULONG Bus, IN UCHAR Device)
{
    PPCI_IRQ_ROUTING_TABLE Table = PciIrqRoutingTable;
    ULONG Count, i;

    if (!Table) return NULL;
    if (Table->TableSize <= FIELD_OFFSET(PCI_IRQ_ROUTING_TABLE, Slot)) return NULL;

    Count = (Table->TableSize - FIELD_OFFSET(PCI_IRQ_ROUTING_TABLE, Slot)) /
            sizeof(SLOT_INFO);
    for (i = 0; i < Count; i++)
    {
        if ((Table->Slot[i].BusNumber == Bus) &&
            ((UCHAR)(Table->Slot[i].DeviceNumber >> 3) == Device))
        {
            return &Table->Slot[i];
        }
    }
    return NULL;
}

/* Walk up through any PCI-PCI bridges applying the standard interrupt-pin
 * swizzle, looking up the routing entry at each level until one with a
 * connected link is found.  Returns the router link and its allowable IRQ
 * mask. */
static
BOOLEAN
PciFindLinkForPin(IN PPCI_PDO_EXTENSION PdoExtension,
                  OUT PUCHAR OutLink,
                  OUT PUSHORT OutMask)
{
    UCHAR Pin, Device;
    PPCI_FDO_EXTENSION Fdo;

    Pin = PdoExtension->InterruptPin;
    if (!PciIrqRoutingTable || (Pin == 0) || (Pin > 4)) return FALSE;

    Device = (UCHAR)PdoExtension->Slot.u.bits.DeviceNumber;
    Fdo = PdoExtension->ParentFdoExtension;

    for (;;)
    {
        PSLOT_INFO Slot = PciFindRoutingEntry(Fdo->BaseBus, Device);
        if (Slot && Slot->PinInfo[Pin - 1].Link)
        {
            *OutLink = Slot->PinInfo[Pin - 1].Link;
            *OutMask = Slot->PinInfo[Pin - 1].InterruptMap;
            return TRUE;
        }

        /* Reached the root bus without a match */
        if (PCI_IS_ROOT_FDO(Fdo)) return FALSE;

        /* Swizzle the pin across the bridge, then move up to its parent bus */
        Pin = (UCHAR)(((Pin - 1 + Device) % 4) + 1);
        {
            PPCI_PDO_EXTENSION BridgePdo =
                (PPCI_PDO_EXTENSION)Fdo->PhysicalDeviceObject->DeviceExtension;
            Device = (UCHAR)BridgePdo->Slot.u.bits.DeviceNumber;
            Fdo = BridgePdo->ParentFdoExtension;
        }
    }
}

/* Pick a usable IRQ out of an allowable mask, preferring IRQs that are
 * traditionally free for PCI use and avoiding fixed legacy assignments. */
static
UCHAR
PciPickIrqFromMask(IN USHORT Mask)
{
    static const UCHAR Preference[] = {11, 10, 9, 5, 7, 6, 12, 15, 14, 4, 3};
    ULONG i;
    for (i = 0; i < RTL_NUMBER_OF(Preference); i++)
        if (Mask & (1 << Preference[i])) return Preference[i];
    return 0;
}

/* Router-agnostic discovery: find the IRQ the BIOS already programmed onto a
 * link by reading the InterruptLine of any other device wired to that link. */
static
UCHAR
PciDiscoverLinkIrq(IN UCHAR Link, IN USHORT Mask)
{
    PPCI_IRQ_ROUTING_TABLE Table = PciIrqRoutingTable;
    ULONG Count, i;
    UCHAR Pin;

    if (!Table || (Table->TableSize <= FIELD_OFFSET(PCI_IRQ_ROUTING_TABLE, Slot)))
        return 0;

    Count = (Table->TableSize - FIELD_OFFSET(PCI_IRQ_ROUTING_TABLE, Slot)) /
            sizeof(SLOT_INFO);
    for (i = 0; i < Count; i++)
    {
        PSLOT_INFO Slot = &Table->Slot[i];
        for (Pin = 0; Pin < 4; Pin++)
        {
            PCI_SLOT_NUMBER SlotNumber;
            UCHAR Line = 0;

            if (Slot->PinInfo[Pin].Link != Link) continue;

            /* Read function 0's interrupt line for that device */
            SlotNumber.u.AsULONG = 0;
            SlotNumber.u.bits.DeviceNumber = (Slot->DeviceNumber >> 3);
            SlotNumber.u.bits.FunctionNumber = 0;
            HalGetBusDataByOffset(PCIConfiguration,
                                  Slot->BusNumber,
                                  SlotNumber.u.AsULONG,
                                  &Line,
                                  FIELD_OFFSET(PCI_COMMON_HEADER,
                                               u.type0.InterruptLine),
                                  sizeof(UCHAR));

            if ((Line >= 1) && (Line <= 15) && (Mask & (1 << Line)))
                return Line;
        }
    }
    return 0;
}

/* Program the chipset PCI interrupt router to steer a link to an IRQ.  Only
 * Intel-compatible routers (where the link byte is the PIRQ config register
 * offset and the value written is the IRQ) are programmed; for any other
 * router we refuse rather than risk corrupting an unknown register. */
static
BOOLEAN
PciProgramInterruptRouter(IN UCHAR Link, IN UCHAR Irq)
{
    PPCI_IRQ_ROUTING_TABLE Table = PciIrqRoutingTable;
    PCI_SLOT_NUMBER RouterSlot;
    USHORT VendorId;

    if (!Table) return FALSE;

    /* The compatible-router field's low word is the router's vendor ID; if it
     * is not populated, read it from the router's config space. */
    VendorId = (USHORT)(Table->CompatibleRouter & 0xFFFF);
    RouterSlot.u.AsULONG = 0;
    RouterSlot.u.bits.DeviceNumber = (Table->RouterDevFunc >> 3) & 0x1F;
    RouterSlot.u.bits.FunctionNumber = Table->RouterDevFunc & 0x7;
    if (VendorId == 0)
    {
        HalGetBusDataByOffset(PCIConfiguration,
                              Table->RouterBus,
                              RouterSlot.u.AsULONG,
                              &VendorId,
                              FIELD_OFFSET(PCI_COMMON_HEADER, VendorID),
                              sizeof(USHORT));
    }

    /* Only known-safe Intel-compatible routers, PIRQ register window */
    if (VendorId != 0x8086) return FALSE;
    if ((Link < 0x60) || (Link > 0x6B)) return FALSE;

    HalSetBusDataByOffset(PCIConfiguration,
                          Table->RouterBus,
                          RouterSlot.u.AsULONG,
                          &Irq,
                          Link,
                          sizeof(UCHAR));
    DPRINT1("PCI: Programmed router link %02x -> IRQ %u\n", Link, Irq);
    return TRUE;
}

/* Commit a routed IRQ back into the device's InterruptLine register so drivers
 * and the HAL translator agree on it. */
static
VOID
PciRouteWriteBack(IN PPCI_PDO_EXTENSION PdoExtension, IN UCHAR Irq)
{
    UCHAR Line = Irq;
    PciWriteDeviceConfig(PdoExtension,
                         &Line,
                         FIELD_OFFSET(PCI_COMMON_HEADER, u.type0.InterruptLine),
                         sizeof(UCHAR));
    PdoExtension->RawInterruptLine = Irq;
}

/*
 * Compute a valid legacy PIC IRQ (1-15) for a device using the $PIR table,
 * repairing/steering as needed.  Returns 0 if the device has no interrupt pin
 * or no IRQ could be assigned.
 */
UCHAR
NTAPI
PciRouteInterrupt(IN PPCI_PDO_EXTENSION PdoExtension)
{
    UCHAR Raw, Link = 0, Irq;
    USHORT Mask = 0;

    /* No interrupt pin -> no interrupt resource */
    if (PdoExtension->InterruptPin == 0) return 0;

    Raw = PdoExtension->RawInterruptLine;

    /* Without routing information, trust the BIOS line only if it is a valid
     * PIC IRQ; otherwise report none rather than a bogus value. */
    if (!PciFindLinkForPin(PdoExtension, &Link, &Mask))
        return ((Raw >= 1) && (Raw <= 15)) ? Raw : 0;

    /* 1. BIOS already left a valid, allowed line -> the link is routed to it */
    if ((Raw >= 1) && (Raw <= 15) && (Mask & (1 << Raw)))
    {
        PciLinkCacheSet(Link, Raw);
        return Raw;
    }

    /* 2. We already routed this link for a sibling device */
    Irq = PciLinkCacheGet(Link);
    if (Irq)
    {
        PciRouteWriteBack(PdoExtension, Irq);
        return Irq;
    }

    /* 3. Discover the IRQ the BIOS programmed onto this link via a sibling */
    Irq = PciDiscoverLinkIrq(Link, Mask);
    if (Irq)
    {
        PciLinkCacheSet(Link, Irq);
        PciRouteWriteBack(PdoExtension, Irq);
        return Irq;
    }

    /* 4. Link is unrouted: pick a free IRQ from the mask and program the router */
    Irq = PciPickIrqFromMask(Mask);
    if (Irq && PciProgramInterruptRouter(Link, Irq))
    {
        PciLinkCacheSet(Link, Irq);
        PciRouteWriteBack(PdoExtension, Irq);
        return Irq;
    }

    /* 5. Could not route it (unknown router / empty mask) */
    DPRINT1("PCI: Could not route INT%c (link %02x mask %04x raw %02x)\n",
            'A' + PdoExtension->InterruptPin - 1, Link, Mask, Raw);
    return ((Raw >= 1) && (Raw <= 15)) ? Raw : 0;
}

/* INT_ROUTE_INTERFACE_STANDARD handlers ------------------------------------ */

static
VOID
NTAPI
PciRouteIntrfReference(IN PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);
}

static
VOID
NTAPI
PciRouteIntrfDereference(IN PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);
}

static
NTSTATUS
NTAPI
PciGetInterruptRouting(IN PDEVICE_OBJECT Pdo,
                       OUT PULONG Bus,
                       OUT PULONG PciSlot,
                       OUT PUCHAR InterruptLine,
                       OUT PUCHAR InterruptPin,
                       OUT PUCHAR ClassCode,
                       OUT PUCHAR SubClassCode,
                       OUT PDEVICE_OBJECT *ParentPdo,
                       OUT ROUTING_TOKEN *RoutingToken,
                       OUT PUCHAR Flags)
{
    PPCI_PDO_EXTENSION PdoExtension = (PPCI_PDO_EXTENSION)Pdo->DeviceExtension;
    UCHAR Link = 0;
    USHORT Mask = 0;

    if (Bus) *Bus = PdoExtension->ParentFdoExtension->BaseBus;
    if (PciSlot) *PciSlot = PdoExtension->Slot.u.AsULONG;
    if (InterruptLine) *InterruptLine = PdoExtension->RawInterruptLine;
    if (InterruptPin) *InterruptPin = PdoExtension->InterruptPin;
    if (ClassCode) *ClassCode = PdoExtension->BaseClass;
    if (SubClassCode) *SubClassCode = PdoExtension->SubClass;
    if (ParentPdo)
    {
        *ParentPdo = PCI_IS_ROOT_FDO(PdoExtension->ParentFdoExtension) ?
                     NULL : PdoExtension->ParentFdoExtension->PhysicalDeviceObject;
    }
    if (RoutingToken)
    {
        RtlZeroMemory(RoutingToken, sizeof(*RoutingToken));
        if (PciFindLinkForPin(PdoExtension, &Link, &Mask))
        {
            RoutingToken->LinkNode = UlongToPtr(Link);
            RoutingToken->StaticVector = PdoExtension->AdjustedInterruptLine;
        }
    }
    if (Flags) *Flags = 0;

    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
PciSetInterruptRoutingToken(IN PDEVICE_OBJECT Pdo,
                            IN PROUTING_TOKEN RoutingToken)
{
    /* The token is purely informational in the legacy PIC path; the IRQ has
     * already been committed to hardware during enumeration. */
    UNREFERENCED_PARAMETER(Pdo);
    UNREFERENCED_PARAMETER(RoutingToken);
    return STATUS_SUCCESS;
}

VOID
NTAPI
PciUpdateInterruptLine(IN PDEVICE_OBJECT Pdo,
                       IN UCHAR LineRegister)
{
    PPCI_PDO_EXTENSION PdoExtension = (PPCI_PDO_EXTENSION)Pdo->DeviceExtension;
    PciRouteWriteBack(PdoExtension, LineRegister);
    PdoExtension->AdjustedInterruptLine = LineRegister;
}

NTSTATUS
NTAPI
routeintrf_Constructor(IN PVOID DeviceExtension,
                       IN PVOID Instance,
                       IN PVOID InterfaceData,
                       IN USHORT Version,
                       IN USHORT Size,
                       IN PINTERFACE Interface)
{
    PINT_ROUTE_INTERFACE_STANDARD Standard = (PINT_ROUTE_INTERFACE_STANDARD)Interface;
    UNREFERENCED_PARAMETER(Instance);
    UNREFERENCED_PARAMETER(InterfaceData);
    UNREFERENCED_PARAMETER(Size);

    /* Only version 1 is supported */
    if (Version != PCI_INT_ROUTE_INTRF_STANDARD_VER) return STATUS_NOINTERFACE;

    Standard->Size = sizeof(INT_ROUTE_INTERFACE_STANDARD);
    Standard->Version = PCI_INT_ROUTE_INTRF_STANDARD_VER;
    Standard->Context = DeviceExtension;
    Standard->InterfaceReference = PciRouteIntrfReference;
    Standard->InterfaceDereference = PciRouteIntrfDereference;
    Standard->GetInterruptRouting = PciGetInterruptRouting;
    Standard->SetInterruptRoutingToken = PciSetInterruptRoutingToken;
    Standard->UpdateInterruptLine = PciUpdateInterruptLine;
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
PciCacheLegacyDeviceRouting(IN PDEVICE_OBJECT DeviceObject,
                            IN ULONG BusNumber,
                            IN ULONG SlotNumber,
                            IN UCHAR InterruptLine,
                            IN UCHAR InterruptPin,
                            IN UCHAR BaseClass,
                            IN UCHAR SubClass,
                            IN PDEVICE_OBJECT PhysicalDeviceObject,
                            IN PPCI_PDO_EXTENSION PdoExtension,
                            OUT PDEVICE_OBJECT *pFoundDeviceObject)
{
    PPCI_LEGACY_DEVICE *Link;
    PPCI_LEGACY_DEVICE LegacyDevice;
    PDEVICE_OBJECT FoundDeviceObject;
    PAGED_CODE();

    /* Scan current registered devices */
    LegacyDevice = PciLegacyDeviceHead;
    Link = &PciLegacyDeviceHead;
    while (LegacyDevice)
    {
        /* Find a match */
        if ((BusNumber == LegacyDevice->BusNumber) &&
            (SlotNumber == LegacyDevice->SlotNumber))
        {
            /* We already know about this routing */
            break;
        }

        /* We know about device already, but for a different location */
        if (LegacyDevice->DeviceObject == DeviceObject)
        {
            /* Free the existing structure, move to the next one */
            *Link = LegacyDevice->Next;
            ExFreePoolWithTag(LegacyDevice, 0);
            LegacyDevice = *Link;
        }
        else
        {
            /* Keep going */
            Link = &LegacyDevice->Next;
            LegacyDevice = LegacyDevice->Next;
        }
    }

    /* Did we find a match? */
    if (!LegacyDevice)
    {
        /* Allocate a new cache structure */
        LegacyDevice = ExAllocatePoolWithTag(PagedPool,
                                             sizeof(PCI_LEGACY_DEVICE),
                                             'PciR');
        if (!LegacyDevice) return STATUS_INSUFFICIENT_RESOURCES;

        /* Save all the data in it */
        RtlZeroMemory(LegacyDevice, sizeof(PCI_LEGACY_DEVICE));
        LegacyDevice->BusNumber = BusNumber;
        LegacyDevice->SlotNumber = SlotNumber;
        LegacyDevice->InterruptLine = InterruptLine;
        LegacyDevice->InterruptPin = InterruptPin;
        LegacyDevice->BaseClass = BaseClass;
        LegacyDevice->SubClass = SubClass;
        LegacyDevice->PhysicalDeviceObject = PhysicalDeviceObject;
        LegacyDevice->DeviceObject = DeviceObject;
        LegacyDevice->PdoExtension = PdoExtension;

        /* Link it in the list */
        LegacyDevice->Next = PciLegacyDeviceHead;
        PciLegacyDeviceHead = LegacyDevice;
    }

    /* Check if we found, or created, a matching caching structure */
    FoundDeviceObject = LegacyDevice->DeviceObject;
    if (FoundDeviceObject == DeviceObject)
    {
        /* Return the device object and success */
        if (pFoundDeviceObject) *pFoundDeviceObject = DeviceObject;
        return STATUS_SUCCESS;
    }

    /* Otherwise, this is a new device object for this location */
    LegacyDevice->DeviceObject = DeviceObject;
    if (pFoundDeviceObject) *pFoundDeviceObject = FoundDeviceObject;
    return STATUS_SUCCESS;
}

/* EOF */
