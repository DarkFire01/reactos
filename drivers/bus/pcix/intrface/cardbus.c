/*
 * PROJECT:         ReactOS PCI Bus Driver
 * LICENSE:         BSD - See COPYING.ARM in the top level directory
 * FILE:            drivers/bus/pci/intrface/cardbus.c
 * PURPOSE:         CardBus Interface
 * PROGRAMMERS:     ReactOS Portable Systems Group
 */

/* INCLUDES *******************************************************************/

#include <pci.h>

#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

PCI_INTERFACE PciCardbusPrivateInterface =
{
    &GUID_PCI_CARDBUS_INTERFACE_PRIVATE,
    sizeof(PCI_CARDBUS_INTERFACE_PRIVATE),
    PCI_CB_INTRF_VERSION,
    PCI_CB_INTRF_VERSION,
    PCI_INTERFACE_PDO,
    0,
    PciInterface_PciCb,
    pcicbintrf_Constructor,
    pcicbintrf_Initializer
};

/* FUNCTIONS ******************************************************************/

static VOID NTAPI PciInterface_RefDereference_NoOp(IN PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);
}

NTSTATUS
NTAPI
PciCardbus_Add(
    IN PDEVICE_OBJECT DeviceObject,
    IN OUT PVOID *DeviceContext)
{
    *DeviceContext = DeviceObject;
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
PciCardbus_Delete(
    IN PVOID DeviceContext)
{
    UNREFERENCED_PARAMETER(DeviceContext);
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
PciCardbus_DispatchPnp(
    IN PVOID DeviceContext,
    IN PIRP Irp)
{
    PDEVICE_OBJECT DeviceObject = (PDEVICE_OBJECT)DeviceContext;
    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(DeviceObject, Irp);
}

VOID
NTAPI
Cardbus_SaveCurrentSettings(IN PPCI_CONFIGURATOR_CONTEXT Context)
{
    UNREFERENCED_PARAMETER(Context);
    UNIMPLEMENTED_DBGBREAK();
}

VOID
NTAPI
Cardbus_SaveLimits(IN PPCI_CONFIGURATOR_CONTEXT Context)
{
    UNREFERENCED_PARAMETER(Context);
    UNIMPLEMENTED_DBGBREAK();
}

VOID
NTAPI
Cardbus_MassageHeaderForLimitsDetermination(IN PPCI_CONFIGURATOR_CONTEXT Context)
{
    UNREFERENCED_PARAMETER(Context);
    UNIMPLEMENTED_DBGBREAK();
}

VOID
NTAPI
Cardbus_RestoreCurrent(IN PPCI_CONFIGURATOR_CONTEXT Context)
{
    UNREFERENCED_PARAMETER(Context);
    UNIMPLEMENTED_DBGBREAK();
}

VOID
NTAPI
Cardbus_GetAdditionalResourceDescriptors(IN PPCI_CONFIGURATOR_CONTEXT Context,
                                         IN PPCI_COMMON_HEADER PciData,
                                         IN PIO_RESOURCE_DESCRIPTOR IoDescriptor)
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(PciData);
    UNREFERENCED_PARAMETER(IoDescriptor);
    UNIMPLEMENTED_DBGBREAK();
}

VOID
NTAPI
Cardbus_ResetDevice(IN PPCI_PDO_EXTENSION PdoExtension,
                    IN PPCI_COMMON_HEADER PciData)
{
    UNREFERENCED_PARAMETER(PdoExtension);
    UNREFERENCED_PARAMETER(PciData);
    UNIMPLEMENTED_DBGBREAK();
}

VOID
NTAPI
Cardbus_ChangeResourceSettings(IN PPCI_PDO_EXTENSION PdoExtension,
                               IN PPCI_COMMON_HEADER PciData)
{
    UNREFERENCED_PARAMETER(PdoExtension);
    UNREFERENCED_PARAMETER(PciData);
    UNIMPLEMENTED_DBGBREAK();
}

NTSTATUS
NTAPI
pcicbintrf_Initializer(IN PVOID Instance)
{
    UNREFERENCED_PARAMETER(Instance);
    /* PnP Interfaces don't get Initialized */
    ASSERTMSG("PCI pcicbintrf_Initializer, unexpected call.\n", FALSE);
    return STATUS_UNSUCCESSFUL;
}

NTSTATUS
NTAPI
pcicbintrf_Constructor(IN PVOID DeviceExtension,
                       IN PVOID Instance,
                       IN PVOID InterfaceData,
                       IN USHORT Version,
                       IN USHORT Size,
                       IN PINTERFACE Interface)
{
    PPCI_PDO_EXTENSION PdoExtension = (PPCI_PDO_EXTENSION)DeviceExtension;
    PPCI_CARDBUS_INTERFACE_PRIVATE CbIf = (PPCI_CARDBUS_INTERFACE_PRIVATE)Interface;
    UNREFERENCED_PARAMETER(Instance);
    UNREFERENCED_PARAMETER(InterfaceData);

    ASSERT_PDO(PdoExtension);
    if (Version != PCI_CB_INTRF_VERSION) return STATUS_NOINTERFACE;
    if (Size < sizeof(PCI_CARDBUS_INTERFACE_PRIVATE)) return STATUS_INFO_LENGTH_MISMATCH;

    CbIf->Context = PdoExtension;
    CbIf->InterfaceReference = (PINTERFACE_REFERENCE)PciInterface_RefDereference_NoOp;
    CbIf->InterfaceDereference = (PINTERFACE_DEREFERENCE)PciInterface_RefDereference_NoOp;
    CbIf->DriverObject = PdoExtension->ParentFdoExtension->FunctionalDeviceObject->DriverObject;
    CbIf->AddCardBus = (PCARDBUSADD)PciCardbus_Add;
    CbIf->DeleteCardBus = (PCARDBUSDELETE)PciCardbus_Delete;
    CbIf->DispatchPnp = (PCARDBUSPCIDISPATCH)PciCardbus_DispatchPnp;
    return STATUS_SUCCESS;
}

/* EOF */
