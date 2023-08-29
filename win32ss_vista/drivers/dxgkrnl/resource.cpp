/*
 * PROJECT:     ReactOS Display Driver Model
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     RDDM Miniport IOCTRL Handlers
 * COPYRIGHT:   Copyright 2023 Justin Miller <justin.miller@reactos.org>
 */

#include <dxgkrnl.h>
//#define NDEBUG
#include <debug.h>


extern PDXGKRNL_PRIVATE_EXTENSION DxgkpExtension;
#define TAG_DXGK_KRNL          'KFXD'


NTSTATUS
NTAPI
DxgkpCreateIds()
{
    PCI_SLOT_NUMBER SlotNumber;
    ULONG PciSlotNumber;
    ULONG Size;

    Size = sizeof(ULONG);
    IoGetDeviceProperty(DxgkpExtension->MiniportPdo,
                                 DevicePropertyBusNumber,
                                 Size,
                                 &DxgkpExtension->SystemIoBusNumber,
                                 &Size);
    Size = sizeof(ULONG);
    IoGetDeviceProperty(DxgkpExtension->MiniportPdo,
                        DevicePropertyLegacyBusType,
                        Size,
                        &DxgkpExtension->AdapterInterfaceType,
                        &Size);
    DPRINT1("AdapterInterfaceType :%d\n", DxgkpExtension->AdapterInterfaceType);


    Size = sizeof(ULONG);
    IoGetDeviceProperty(DxgkpExtension->MiniportPdo,
                        DevicePropertyAddress,
                        Size,
                        &PciSlotNumber,
                        &Size);
    SlotNumber.u.AsULONG = 0;
    SlotNumber.u.bits.DeviceNumber = (PciSlotNumber >> 16) & 0xFFFF;
    SlotNumber.u.bits.FunctionNumber = PciSlotNumber & 0xFFFF;
    DxgkpExtension->SystemIoSlotNumber = SlotNumber.u.AsULONG;

    DPRINT1("Device Number: %d\n",  SlotNumber.u.bits.DeviceNumber);
    DPRINT1("FunctionNumber: %d\n", SlotNumber.u.bits.FunctionNumber);

    return 0;
}

NTSTATUS
NTAPI
DxgkrnlSetupResourceList(_Inout_ PCM_RESOURCE_LIST* ResourceList)
{
    NTSTATUS Status;
    PCI_SLOT_NUMBER PciSlotNumber;
    PCI_COMMON_CONFIG Config;
    ULONG ReturnedLength;



    PciSlotNumber.u.AsULONG = DxgkpExtension->SystemIoSlotNumber;


    if (DxgkpExtension->MiniportPdo != NULL)
    {
        PciSlotNumber.u.AsULONG = DxgkpExtension->SystemIoSlotNumber;

        ReturnedLength = HalGetBusData(PCIConfiguration,
                                       DxgkpExtension->SystemIoBusNumber,
                                       PciSlotNumber.u.AsULONG,
                                       &Config,
                                       sizeof(Config));

        if (ReturnedLength != sizeof(Config))
        {
            __debugbreak();
            return STATUS_NO_MEMORY;
        }
    }
    else
    {
        __debugbreak();
    }

    Status = HalAssignSlotResources(&DxgkpExtension->RegistryPath,
                                            NULL,
                                            DxgkpExtension->MiniportDriverObject,
                                            DxgkpExtension->MiniportDriverObject->DeviceObject,
                                            DxgkpExtension->AdapterInterfaceType,
                                            DxgkpExtension->SystemIoBusNumber,
                                            PciSlotNumber.u.AsULONG,
                                            ResourceList);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("HalAssignSlotResources failed with status %x.\n",Status);
        __debugbreak();
        return Status;
    }
    /* ******************************************************************/
    DPRINT1("ResourceList loc %X\n", ResourceList);
    return 0;
}
