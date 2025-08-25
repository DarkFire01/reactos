/*
 * PROJECT:         ReactOS PCI Bus Driver
 * LICENSE:         BSD - See COPYING.ARM in the top level directory
 * FILE:            drivers/bus/pci/intrface/locintrf.c
 * PURPOSE:         Location Interface
 * PROGRAMMERS:     ReactOS Portable Systems Group
 */

/* INCLUDES *******************************************************************/

#include <pci.h>
#include <ntstrsafe.h>
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
static
NTSTATUS
NTAPI
PciGetLocationString(
    _Inout_opt_ PVOID Context,
    _Outptr_ PZZWSTR *LocationStrings);
    
NTSTATUS
NTAPI
locintrf_Initializer(IN PVOID Instance)
{
    UNREFERENCED_PARAMETER(Instance);
    /* PnP Interfaces don't get Initialized */
    ASSERTMSG("PCI locintrf_Initializer, unexpected call.\n", FALSE);
    return STATUS_UNSUCCESSFUL;
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
    PPNP_LOCATION_INTERFACE Iface;
    PPCI_PDO_EXTENSION PdoExtension;

    UNREFERENCED_PARAMETER(Instance);
    UNREFERENCED_PARAMETER(InterfaceData);
    UNREFERENCED_PARAMETER(Version);

    /* This interface is valid for both PDOs and the root FDO; prefer PDO context */
    PdoExtension = (PPCI_PDO_EXTENSION)DeviceExtension;

    if (Size < sizeof(PNP_LOCATION_INTERFACE)) return STATUS_INVALID_PARAMETER;

    Iface = (PPNP_LOCATION_INTERFACE)Interface;
    RtlZeroMemory(Iface, sizeof(*Iface));
    Iface->Size = sizeof(PNP_LOCATION_INTERFACE);
    Iface->Version = PNP_LOCATION_INTERFACE_VERSION;
    Iface->Context = PdoExtension;
    Iface->InterfaceReference = (PINTERFACE_REFERENCE)PciInterface_RefDereference_NoOp;
    Iface->InterfaceDereference = (PINTERFACE_DEREFERENCE)PciInterface_RefDereference_NoOp;

    /* Provide a simple location string callback */
    Iface->GetLocationString = (PGET_LOCATION_STRING)PciGetLocationString;

    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
PciGetLocationString(
    _Inout_opt_ PVOID Context,
    _Outptr_ PZZWSTR *LocationStrings)
{
    PPCI_PDO_EXTENSION Pdo = (PPCI_PDO_EXTENSION)Context;
    PWSTR Buffer;
    SIZE_T Length;

    if (!LocationStrings) return STATUS_INVALID_PARAMETER;
    *LocationStrings = NULL;

    /* If no PDO context, we cannot format a specific B:D.F; return empty string list */
    if (!Pdo)
    {
        Buffer = ExAllocatePoolWithTag(PagedPool, sizeof(WCHAR) * 2, PCI_POOL_TAG);
        if (!Buffer) return STATUS_INSUFFICIENT_RESOURCES;
        Buffer[0] = L'\0';
        Buffer[1] = L'\0'; /* Double NUL terminator for multi-string */
        *LocationStrings = Buffer;
        return STATUS_SUCCESS;
    }

    /* Allocate enough for e.g. "PCI bus 0, device 1, function 2" plus double-NUL */
    Length = 64 * sizeof(WCHAR);
    Buffer = ExAllocatePoolWithTag(PagedPool, Length, PCI_POOL_TAG);
    if (!Buffer) return STATUS_INSUFFICIENT_RESOURCES;

    RtlStringCchPrintfW(Buffer,
                        Length / sizeof(WCHAR),
                        L"PCI bus %u, device %u, function %u",
                        Pdo->ParentFdoExtension ? Pdo->ParentFdoExtension->BaseBus : 0,
                        Pdo->Slot.u.bits.DeviceNumber,
                        Pdo->Slot.u.bits.FunctionNumber);

    /* Multi-string terminator */
    {
        SIZE_T cch = wcslen(Buffer);
        Buffer[cch + 0] = L'\0';
        Buffer[cch + 1] = L'\0';
    }

    *LocationStrings = Buffer;
    return STATUS_SUCCESS;
}

/* EOF */
