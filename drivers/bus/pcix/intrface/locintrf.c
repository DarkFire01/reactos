/*
 * PROJECT:         ReactOS PCI Bus Driver
 * LICENSE:         BSD - See COPYING.ARM in the top level directory
 * FILE:            drivers/bus/pci/intrface/locintrf.c
 * PURPOSE:         Location Interface
 * PROGRAMMERS:     ReactOS Portable Systems Group
 */

/* INCLUDES *******************************************************************/

#include <pci.h>

#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

PCI_INTERFACE PciLocationInterface =
{
    &GUID_PNP_LOCATION_INTERFACE,
    sizeof(PNP_LOCATION_INTERFACE),
    PNP_LOCATION_INTERFACE_VERSION,
    PNP_LOCATION_INTERFACE_VERSION,
    PCI_INTERFACE_FDO | PCI_INTERFACE_ROOT | PCI_INTERFACE_PDO,
    0,
    PciInterface_Location,
    locintrf_Constructor,
    locintrf_Initializer
};

/* FUNCTIONS ******************************************************************/

NTSTATUS
NTAPI
locintrf_Initializer(IN PVOID Instance)
{
    UNREFERENCED_PARAMETER(Instance);
    /* PnP Interfaces don't get Initialized */
    ASSERTMSG("PCI locintrf_Initializer, unexpected call.\n", FALSE);
    return STATUS_UNSUCCESSFUL;
}

VOID
NTAPI
PciLocationInterface_Reference(IN PVOID Context)
{
    PPCI_PDO_EXTENSION PdoExtension = (PPCI_PDO_EXTENSION)Context;

    /* The device outlives anything holding a description of where it is */
    UNREFERENCED_PARAMETER(PdoExtension);
}

VOID
NTAPI
PciLocationInterface_Dereference(IN PVOID Context)
{
    PPCI_PDO_EXTENSION PdoExtension = (PPCI_PDO_EXTENSION)Context;

    UNREFERENCED_PARAMETER(PdoExtension);
}

/*
 * Describe where on its bus a device sits. The answer is this driver's one
 * piece of a location the whole stack builds together, so it names only the
 * device and function; whoever produced the bus names the part in front.
 */
NTSTATUS
NTAPI
PciLocationInterface_GetLocationString(IN PVOID Context,
                                       OUT PWCHAR *LocationStrings)
{
    PPCI_PDO_EXTENSION PdoExtension = (PPCI_PDO_EXTENSION)Context;
    PWCHAR Buffer;
    SIZE_T Size;
    PAGED_CODE();

    /* One string, and the empty one that ends a multi-string */
    Size = sizeof(L"PCI(XXXX)") + sizeof(UNICODE_NULL);

    Buffer = ExAllocatePoolWithTag(PagedPool, Size, PCI_POOL_TAG);
    if (!Buffer) return STATUS_INSUFFICIENT_RESOURCES;

    RtlZeroMemory(Buffer, Size);
    _swprintf(Buffer,
              L"PCI(%02X%02X)",
              PdoExtension->Slot.u.bits.DeviceNumber,
              PdoExtension->Slot.u.bits.FunctionNumber);

    *LocationStrings = Buffer;
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
locintrf_Constructor(IN PVOID DeviceExtension,
                     IN PVOID Instance,
                     IN PVOID InterfaceData,
                     IN USHORT Version,
                     IN USHORT Size,
                     IN PINTERFACE Interface)
{
    PPNP_LOCATION_INTERFACE LocationInterface = (PPNP_LOCATION_INTERFACE)Interface;
    PAGED_CODE();

    UNREFERENCED_PARAMETER(Instance);
    UNREFERENCED_PARAMETER(InterfaceData);
    UNREFERENCED_PARAMETER(Version);
    UNREFERENCED_PARAMETER(Size);

    LocationInterface->Size = sizeof(PNP_LOCATION_INTERFACE);
    LocationInterface->Version = PNP_LOCATION_INTERFACE_VERSION;
    LocationInterface->Context = DeviceExtension;
    LocationInterface->InterfaceReference = PciLocationInterface_Reference;
    LocationInterface->InterfaceDereference = PciLocationInterface_Dereference;
    LocationInterface->GetLocationString = PciLocationInterface_GetLocationString;
    return STATUS_SUCCESS;
}

/* EOF */
