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

NTSTATUS
NTAPI
routeintrf_Constructor(IN PVOID DeviceExtension,
                       IN PVOID Instance,
                       IN PVOID InterfaceData,
                       IN USHORT Version,
                       IN USHORT Size,
                       IN PINTERFACE Interface)
{
   // PINT_ROUTE_INTERFACE_STANDARD Rt;

    UNREFERENCED_PARAMETER(DeviceExtension);
    UNREFERENCED_PARAMETER(Instance);
    UNREFERENCED_PARAMETER(InterfaceData);
    UNREFERENCED_PARAMETER(Size);

    /* Only version 1 is supported */
    if (Version != PCI_INT_ROUTE_INTRF_STANDARD_VER) return STATUS_NOINTERFACE;
#if 0
    Rt = (PINT_ROUTE_INTERFACE_STANDARD)Interface;
    Rt->Size = sizeof(INT_ROUTE_INTERFACE_STANDARD);
    Rt->Version = PCI_INT_ROUTE_INTRF_STANDARD_VER;
    Rt->Context = NULL;
    Rt->InterfaceReference = NULL;
    Rt->InterfaceDereference = NULL;
    Rt->GetInterruptRouting = PciGetInterruptRouting;
    Rt->SetInterruptRoutingToken = PciSetInterruptRoutingToken;
#endif
    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
PciGetInterruptRouting(
    IN PDEVICE_OBJECT Pdo,
    OUT PULONG Bus,
    OUT PULONG PciSlot,
    OUT PUCHAR InterruptLine,
    OUT PUCHAR InterruptPin,
    OUT PUCHAR ClassCode,
    OUT PUCHAR SubClassCode,
    OUT PDEVICE_OBJECT* ParentPdo,
    OUT PROUTING_TOKEN RoutingToken,
    OUT PUCHAR Flags)
{
    BUS_HANDLER BusHandler;
    PCI_SLOT_NUMBER Slot;
    PPCI_COMMON_CONFIG PciConfig;
    ULONG Bytes;
    ULONG Address;
    KIRQL Irql;
    KAFFINITY Affinity;

    if (!Pdo || !Bus || !PciSlot || !InterruptLine || !InterruptPin ||
        !ClassCode || !SubClassCode || !ParentPdo || !RoutingToken || !Flags)
    {
        return STATUS_INVALID_PARAMETER;
    }

    *ParentPdo = NULL;
    *Flags = 0;
    RtlZeroMemory(RoutingToken, sizeof(*RoutingToken));

    if (!NT_SUCCESS(IoGetDeviceProperty(Pdo, DevicePropertyBusNumber, sizeof(ULONG), Bus, &Bytes)))
    {
        return STATUS_UNSUCCESSFUL;
    }

    if (!NT_SUCCESS(IoGetDeviceProperty(Pdo, DevicePropertyAddress, sizeof(ULONG), &Address, &Bytes)))
    {
        Address = 0;
    }
    Slot.u.AsULONG = Address;

   // RtlCopyMemory(&BusHandler, &HalpFakePciBusHandler, sizeof(BUS_HANDLER));
    BusHandler.BusNumber = *Bus;

    PciConfig = ExAllocatePoolWithTag(NonPagedPool, sizeof(PCI_COMMON_CONFIG), 'rtnI');
    if (!PciConfig) return STATUS_INSUFFICIENT_RESOURCES;

    Bytes = HalGetBusDataByOffset(PCIConfiguration, *Bus, Slot.u.AsULONG, PciConfig, 0, sizeof(PCI_COMMON_CONFIG));
    if (Bytes < FIELD_OFFSET(PCI_COMMON_CONFIG, BaseClass))
    {
        ExFreePoolWithTag(PciConfig, 'rtnI');
        return STATUS_UNSUCCESSFUL;
    }

    *PciSlot = (ULONG)Slot.u.AsULONG;
    *ClassCode = PciConfig->BaseClass;
    *SubClassCode = PciConfig->SubClass;
    *InterruptPin = PciConfig->u.type0.InterruptPin;
    *InterruptLine = PciConfig->u.type0.InterruptLine;

    if ((*InterruptLine == 0) || (*InterruptLine == 0xFF))
    {
        /* Derive a legacy line via swizzle */
        UCHAR pinIndex = (*InterruptPin ? (*InterruptPin - 1) : 0) & 0x3;
        if (*Bus == 0)
        {
            *InterruptLine = (UCHAR)(16 + (((*PciSlot & 0x1F) + pinIndex) & 0x3));
        }
        else
        {
            *InterruptLine = (UCHAR)(16 + (((*Bus & 0x07) * 4) + (((*PciSlot & 0x1F) + pinIndex) & 0x3)));
        }
    }

    /* Compute a system vector for this routing */
    RoutingToken->StaticVector = HalGetInterruptVector(PCIBus,
                                                       *Bus,
                                                       *InterruptLine,
                                                       *InterruptLine,
                                                       &Irql,
                                                       &Affinity);
    RoutingToken->LinkNode = NULL;
    RoutingToken->Flags = 0;

    ExFreePoolWithTag(PciConfig, 'rtnI');
    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
PciSetInterruptRoutingToken(
    IN PDEVICE_OBJECT Pdo,
    IN PROUTING_TOKEN RoutingToken)
{
    UNREFERENCED_PARAMETER(Pdo);
    UNREFERENCED_PARAMETER(RoutingToken);
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
