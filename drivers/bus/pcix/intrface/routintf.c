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
 * The routing interface is how a HAL that steers PCI interrupts itself (the
 * legacy $PIR router) learns where a device's interrupt pin sits, remembers
 * which routing link it resolved that pin to, and writes back the IRQ it
 * chose. A device reaches these routines in one of two forms: one of our own
 * PDOs, or the device object of a non-PnP driver that claimed a PCI slot
 * through HalAssignSlotResources, which PciCacheLegacyDeviceRouting recorded
 * for exactly this purpose.
 *
 * None of them take PciGlobalLock: the legacy path in hookhal.c holds it
 * across IoAssignResources, and interrupt arbitration calls back in here.
 */

typedef struct _PCI_ROUTING_TARGET
{
    PPCI_PDO_EXTENSION PdoExtension;
    PPCI_LEGACY_DEVICE LegacyDevice;
} PCI_ROUTING_TARGET, *PPCI_ROUTING_TARGET;

static
BOOLEAN
PciRoutingResolveDevice(IN PDEVICE_OBJECT DeviceObject,
                        OUT PPCI_ROUTING_TARGET Target)
{
    PPCI_PDO_EXTENSION PdoExtension;
    PPCI_LEGACY_DEVICE LegacyDevice;

    Target->PdoExtension = NULL;
    Target->LegacyDevice = NULL;
    if (!DeviceObject) return FALSE;

    /* One of ours? Our FDOs share the driver object, so check the kind too */
    if (DeviceObject->DriverObject == PciDriverObject)
    {
        PdoExtension = (PPCI_PDO_EXTENSION)DeviceObject->DeviceExtension;
        if (PdoExtension->ExtensionType != PciPdoExtensionType) return FALSE;

        Target->PdoExtension = PdoExtension;
        return TRUE;
    }

    /* Otherwise it can only be a legacy driver's claim on one of our slots */
    for (LegacyDevice = PciLegacyDeviceHead;
         LegacyDevice;
         LegacyDevice = LegacyDevice->Next)
    {
        if ((LegacyDevice->DeviceObject == DeviceObject) &&
            (LegacyDevice->PdoExtension))
        {
            Target->PdoExtension = LegacyDevice->PdoExtension;
            Target->LegacyDevice = LegacyDevice;
            return TRUE;
        }
    }

    /* Not a PCI device at all, e.g. the PDO the root bus sits on */
    return FALSE;
}

VOID
NTAPI
PciRoutingInterface_Reference(IN PVOID Context)
{
    /* The root FDO outlives every holder of this interface */
    UNREFERENCED_PARAMETER(Context);
}

VOID
NTAPI
PciRoutingInterface_Dereference(IN PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);
}

/*
 * Describe a device's interrupt wiring. The parent is the PDO of the bridge
 * the device's bus hangs off; for the root bus that is not a PCI device, so a
 * caller walking upwards stops when this routine refuses it.
 */
NTSTATUS
NTAPI
PciRoutingInterface_GetInterruptRouting(IN PDEVICE_OBJECT Pdo,
                                        OUT ULONG *Bus,
                                        OUT ULONG *PciSlot,
                                        OUT UCHAR *InterruptLine,
                                        OUT UCHAR *InterruptPin,
                                        OUT UCHAR *ClassCode,
                                        OUT UCHAR *SubClassCode,
                                        OUT PDEVICE_OBJECT *ParentPdo,
                                        OUT ROUTING_TOKEN *RoutingToken,
                                        OUT UCHAR *Flags)
{
    PCI_ROUTING_TARGET Target;
    PPCI_PDO_EXTENSION PdoExtension;
    PPCI_LEGACY_DEVICE LegacyDevice;
    PAGED_CODE();

    if (!PciRoutingResolveDevice(Pdo, &Target)) return STATUS_NOT_FOUND;

    LegacyDevice = Target.LegacyDevice;
    if (LegacyDevice)
    {
        /* Answer from what the legacy claim recorded */
        *Bus = LegacyDevice->BusNumber;
        *PciSlot = LegacyDevice->SlotNumber;
        *InterruptLine = LegacyDevice->InterruptLine;
        *InterruptPin = LegacyDevice->InterruptPin;
        *ClassCode = LegacyDevice->BaseClass;
        *SubClassCode = LegacyDevice->SubClass;
        *ParentPdo = LegacyDevice->PhysicalDeviceObject;
        *RoutingToken = LegacyDevice->RoutingToken;
    }
    else
    {
        PdoExtension = Target.PdoExtension;
        *Bus = PdoExtension->ParentFdoExtension->BaseBus;
        *PciSlot = PdoExtension->Slot.u.AsULONG;
        *InterruptLine = PdoExtension->RawInterruptLine;
        *InterruptPin = PdoExtension->InterruptPin;
        *ClassCode = PdoExtension->BaseClass;
        *SubClassCode = PdoExtension->SubClass;
        *ParentPdo = PdoExtension->ParentFdoExtension->PhysicalDeviceObject;
        *RoutingToken = PdoExtension->RoutingToken;
    }

    /* No routing flags apply to anything this driver enumerates */
    *Flags = 0;
    return STATUS_SUCCESS;
}

/* Remember the caller's routing token for a device, for its next lookup */
NTSTATUS
NTAPI
PciRoutingInterface_SetInterruptRoutingToken(IN PDEVICE_OBJECT Pdo,
                                             IN PROUTING_TOKEN RoutingToken)
{
    PCI_ROUTING_TARGET Target;
    PAGED_CODE();

    if (!RoutingToken) return STATUS_INVALID_PARAMETER;
    if (!PciRoutingResolveDevice(Pdo, &Target)) return STATUS_NOT_FOUND;

    if (Target.LegacyDevice)
    {
        Target.LegacyDevice->RoutingToken = *RoutingToken;
    }
    else
    {
        Target.PdoExtension->RoutingToken = *RoutingToken;
    }

    return STATUS_SUCCESS;
}

/*
 * Write the IRQ the caller routed a device onto into its interrupt-line
 * register. Our own copies have to follow: PciSetResources writes the line
 * back from RawInterruptLine whenever it programs the device, and the boot
 * configuration is reported from AdjustedInterruptLine, so leaving either
 * stale would quietly undo or misreport the new routing.
 */
VOID
NTAPI
PciRoutingInterface_UpdateInterruptLine(IN PDEVICE_OBJECT Pdo,
                                        IN UCHAR LineRegister)
{
    PCI_ROUTING_TARGET Target;
    PPCI_PDO_EXTENSION PdoExtension;
    PAGED_CODE();

    if (!PciRoutingResolveDevice(Pdo, &Target)) return;

    PdoExtension = Target.PdoExtension;
    PdoExtension->RawInterruptLine = LineRegister;
    PdoExtension->AdjustedInterruptLine = LineRegister;
    if (Target.LegacyDevice) Target.LegacyDevice->InterruptLine = LineRegister;

    PciWriteDeviceConfig(PdoExtension,
                         &LineRegister,
                         FIELD_OFFSET(PCI_COMMON_HEADER, u.type0.InterruptLine),
                         sizeof(UCHAR));
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
    PINT_ROUTE_INTERFACE_STANDARD RouteInterface = (PINT_ROUTE_INTERFACE_STANDARD)Interface;
    PAGED_CODE();

    UNREFERENCED_PARAMETER(Instance);
    UNREFERENCED_PARAMETER(InterfaceData);
    UNREFERENCED_PARAMETER(Size);

    /* Only version 1 is supported */
    if (Version != PCI_INT_ROUTE_INTRF_STANDARD_VER) return STATUS_NOINTERFACE;

    RouteInterface->Size = sizeof(INT_ROUTE_INTERFACE_STANDARD);
    RouteInterface->Version = PCI_INT_ROUTE_INTRF_STANDARD_VER;
    RouteInterface->Context = DeviceExtension;
    RouteInterface->InterfaceReference = PciRoutingInterface_Reference;
    RouteInterface->InterfaceDereference = PciRoutingInterface_Dereference;
    RouteInterface->GetInterruptRouting = PciRoutingInterface_GetInterruptRouting;
    RouteInterface->SetInterruptRoutingToken = PciRoutingInterface_SetInterruptRoutingToken;
    RouteInterface->UpdateInterruptLine = PciRoutingInterface_UpdateInterruptLine;
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
