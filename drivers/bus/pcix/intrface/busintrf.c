/*
 * PROJECT:         ReactOS PCI Bus Driver
 * LICENSE:         BSD - See COPYING.ARM in the top level directory
 * FILE:            drivers/bus/pci/intrface/busintrf.c
 * PURPOSE:         Bus Interface
 * PROGRAMMERS:     ReactOS Portable Systems Group
 */

/* INCLUDES *******************************************************************/

#include <pci.h>

#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

PCI_INTERFACE BusHandlerInterface =
{
    &GUID_BUS_INTERFACE_STANDARD,
    sizeof(BUS_INTERFACE_STANDARD),
    1,
    1,
    PCI_INTERFACE_PDO,
    0,
    PciInterface_BusHandler,
    busintrf_Constructor,
    busintrf_Initializer
};

/* FUNCTIONS ******************************************************************/

static
VOID
NTAPI
BusIf_RefDeref_NoOp(
    IN PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);
}

static
BOOLEAN
NTAPI
BusIf_TranslateBusAddress(
    IN PVOID Context,
    IN PHYSICAL_ADDRESS BusAddress,
    IN ULONG Length,
    IN OUT PULONG AddressSpace,
    OUT PPHYSICAL_ADDRESS TranslatedAddress)
{
    PPCI_PDO_EXTENSION PdoExtension = (PPCI_PDO_EXTENSION)Context;
    ASSERT_PDO(PdoExtension);
    return HalTranslateBusAddress(PCIBus,
                                  PdoExtension->ParentFdoExtension->BaseBus,
                                  BusAddress,
                                  AddressSpace,
                                  TranslatedAddress);
}

static
PDMA_ADAPTER
NTAPI
BusIf_GetDmaAdapter(
    IN PVOID Context,
    IN PDEVICE_DESCRIPTION DeviceDescription,
    OUT PULONG NumberOfMapRegisters)
{
    UNREFERENCED_PARAMETER(Context);
    return (PDMA_ADAPTER)HalGetAdapter(DeviceDescription, NumberOfMapRegisters);
}

static
ULONG
NTAPI
BusIf_SetBusData(
    IN PVOID Context,
    IN ULONG DataType,
    IN PVOID Buffer,
    IN ULONG Offset,
    IN ULONG Length)
{
    PPCI_PDO_EXTENSION PdoExtension = (PPCI_PDO_EXTENSION)Context;
    ASSERT_PDO(PdoExtension);
    if (DataType != PCI_WHICHSPACE_CONFIG) return 0;
    return HalSetBusDataByOffset(PCIConfiguration,
                                 PdoExtension->ParentFdoExtension->BaseBus,
                                 PdoExtension->Slot.u.AsULONG,
                                 Buffer,
                                 Offset,
                                 Length);
}

static
ULONG
NTAPI
BusIf_GetBusData(
    IN PVOID Context,
    IN ULONG DataType,
    IN PVOID Buffer,
    IN ULONG Offset,
    IN ULONG Length)
{
    PPCI_PDO_EXTENSION PdoExtension = (PPCI_PDO_EXTENSION)Context;
    ASSERT_PDO(PdoExtension);
    if (DataType != PCI_WHICHSPACE_CONFIG) return 0;
    return HalGetBusDataByOffset(PCIConfiguration,
                                 PdoExtension->ParentFdoExtension->BaseBus,
                                 PdoExtension->Slot.u.AsULONG,
                                 Buffer,
                                 Offset,
                                 Length);
}

NTSTATUS
NTAPI
busintrf_Initializer(IN PVOID Instance)
{
    UNREFERENCED_PARAMETER(Instance);
    /* PnP Interfaces don't get Initialized */
    ASSERTMSG("PCI busintrf_Initializer, unexpected call.\n", FALSE);
    return STATUS_UNSUCCESSFUL;
}

NTSTATUS
NTAPI
busintrf_Constructor(IN PVOID DeviceExtension,
                     IN PVOID Instance,
                     IN PVOID InterfaceData,
                     IN USHORT Version,
                     IN USHORT Size,
                     IN PINTERFACE Interface)
{
    PPCI_PDO_EXTENSION PdoExtension = (PPCI_PDO_EXTENSION)DeviceExtension;
    PBUS_INTERFACE_STANDARD Iface = (PBUS_INTERFACE_STANDARD)Interface;
    UNREFERENCED_PARAMETER(Instance);
    UNREFERENCED_PARAMETER(InterfaceData);

    ASSERT_PDO(PdoExtension);
    if (Version != 1)
    {
        __debugbreak();
        return STATUS_NOINTERFACE;
    }
    if (Size < sizeof(BUS_INTERFACE_STANDARD)) return STATUS_INFO_LENGTH_MISMATCH;

    Iface->Context = PdoExtension;
    Iface->InterfaceReference = (PINTERFACE_REFERENCE)BusIf_RefDeref_NoOp;
    Iface->InterfaceDereference = (PINTERFACE_DEREFERENCE)BusIf_RefDeref_NoOp;
    Iface->TranslateBusAddress = (PTRANSLATE_BUS_ADDRESS)BusIf_TranslateBusAddress;
    Iface->GetDmaAdapter = (PGET_DMA_ADAPTER)BusIf_GetDmaAdapter;
    Iface->SetBusData = (PGET_SET_DEVICE_DATA)BusIf_SetBusData;
    Iface->GetBusData = (PGET_SET_DEVICE_DATA)BusIf_GetBusData;
    return STATUS_SUCCESS;
}

/* EOF */
