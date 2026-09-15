/*
 * PROJECT:         ReactOS PCI Bus Driver
 * LICENSE:         BSD - See COPYING.ARM in the top level directory
 * FILE:            drivers/bus/pci/hookhal.c
 * PURPOSE:         HAL Bus Handler Dispatch Routine Support
 * PROGRAMMERS:     ReactOS Portable Systems Group
 */

/* INCLUDES *******************************************************************/

#include <pci.h>

#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

pHalTranslateBusAddress PcipSavedTranslateBusAddress;
pHalAssignSlotResources PcipSavedAssignSlotResources;

/* FUNCTIONS ******************************************************************/

BOOLEAN
NTAPI
PciTranslateBusAddress(IN INTERFACE_TYPE InterfaceType,
                       IN ULONG BusNumber,
                       IN PHYSICAL_ADDRESS BusAddress,
                       OUT PULONG AddressSpace,
                       OUT PPHYSICAL_ADDRESS TranslatedAddress)
{
    /* The HAL has the final say on any bus it registered */
    if (PcipSavedTranslateBusAddress(InterfaceType,
                                     BusNumber,
                                     BusAddress,
                                     AddressSpace,
                                     TranslatedAddress))
    {
        return TRUE;
    }

    /* Buses the HAL never saw still map PCI addresses 1:1 to processor addresses */
    if (InterfaceType != PCIBus)
        return FALSE;

    *TranslatedAddress = BusAddress;
    return TRUE;
}

/**
 * @brief Finds the PDO of the function in a slot. The caller holds PciGlobalLock.
 */
static
PPCI_PDO_EXTENSION
NTAPI
PciFindPdoByLocation(
    _In_ ULONG BusNumber,
    _In_ ULONG SlotNumber)
{
    PPCI_FDO_EXTENSION DeviceExtension;
    PPCI_PDO_EXTENSION PdoExtension;
    PCI_SLOT_NUMBER PciSlot;
    PciSlot.u.AsULONG = SlotNumber;

    /* Now search for the extension */
    DeviceExtension = (PPCI_FDO_EXTENSION)PciFdoExtensionListHead.Next;
    while (DeviceExtension)
    {
        /* If we found it, break out */
        if (DeviceExtension->BaseBus == BusNumber) break;

        /* Move to the next device */
        DeviceExtension = (PPCI_FDO_EXTENSION)DeviceExtension->List.Next;
    }

    /* Check if the device extension for the bus was found */
    if (!DeviceExtension)
    {
        /* It wasn't, bail out */
        DPRINT1("Pci: Could not find PCI bus FDO. Bus Number = 0x%x\n", BusNumber);
        return NULL;
    }

    /* Acquire this device's lock */
    KeEnterCriticalRegion();
    KeWaitForSingleObject(&DeviceExtension->ChildListLock,
                          Executive,
                          KernelMode,
                          FALSE,
                          NULL);

    /* Loop every child PDO */
    for (PdoExtension = DeviceExtension->ChildPdoList;
         PdoExtension;
         PdoExtension = PdoExtension->Next)
    {
        /* A function that left the slot keeps its PDO until PnP removes it */
        if ((PdoExtension->NotPresent) || (PdoExtension->ReportedMissing))
            continue;

        /* Check if the function number and header data matches */
        if ((PdoExtension->Slot.u.bits.FunctionNumber == PciSlot.u.bits.FunctionNumber) &&
            (PdoExtension->Slot.u.bits.DeviceNumber == PciSlot.u.bits.DeviceNumber))
        {
            /* This is considered to be the same PDO */
            ASSERT(PdoExtension->Slot.u.AsULONG == PciSlot.u.AsULONG);
            break;
        }
    }

    /* Release this device's lock */
    KeSetEvent(&DeviceExtension->ChildListLock, IO_NO_INCREMENT, FALSE);
    KeLeaveCriticalRegion();

    /* Check if we found something */
    if (!PdoExtension)
    {
        /* Let the debugger know */
        DPRINT1("Pci: Could not find PDO for device @ %x.%x.%x\n",
                BusNumber,
                PciSlot.u.bits.DeviceNumber,
                PciSlot.u.bits.FunctionNumber);
    }

    /* If the search found something, this is non-NULL, otherwise it's NULL */
    return PdoExtension;
}

/**
 * @brief Removes the device private descriptors from an assigned resource list.
 */
static
VOID
NTAPI
PciStripPrivateDescriptors(
    _Inout_ PCM_RESOURCE_LIST ResourceList)
{
    PCM_PARTIAL_RESOURCE_LIST PartialList;
    ULONG Source, Target;

    ASSERT(ResourceList->Count == 1);
    PartialList = &ResourceList->List[0].PartialResourceList;

    Target = 0;
    for (Source = 0; Source < PartialList->Count; Source++)
    {
        if (PartialList->PartialDescriptors[Source].Type == CmResourceTypeDevicePrivate)
            continue;

        if (Target != Source)
            PartialList->PartialDescriptors[Target] = PartialList->PartialDescriptors[Source];

        Target++;
    }

    PartialList->Count = Target;
}

/**
 * @brief Assigns resources to a function claimed by a driver outside of PnP.
 */
NTSTATUS
NTAPI
PciAssignSlotResources(
    _In_ PUNICODE_STRING RegistryPath,
    _In_opt_ PUNICODE_STRING DriverClassName,
    _In_ PDRIVER_OBJECT DriverObject,
    _In_opt_ PDEVICE_OBJECT DeviceObject,
    _In_ INTERFACE_TYPE BusType,
    _In_ ULONG BusNumber,
    _In_ ULONG SlotNumber,
    _Inout_ PCM_RESOURCE_LIST *AllocatedResources)
{
    PIO_RESOURCE_REQUIREMENTS_LIST RequirementsList = NULL;
    PCM_RESOURCE_LIST Resources = NULL;
    PCI_COMMON_HEADER PciData;
    PPCI_PDO_EXTENSION PdoExtension;
    NTSTATUS Status;
    PDEVICE_OBJECT ExistingDeviceObject;
    PAGED_CODE();
    ASSERT(PcipSavedAssignSlotResources);

    /* Assume no resources */
    *AllocatedResources = NULL;

    if (BusType != PCIBus)
        return STATUS_INVALID_PARAMETER;

    /* A removed bus is unlinked under this lock before its PDOs are deleted */
    KeEnterCriticalRegion();
    KeWaitForSingleObject(&PciGlobalLock, Executive, KernelMode, FALSE, NULL);

    PdoExtension = PciFindPdoByLocation(BusNumber, SlotNumber);
    if (!PdoExtension)
    {
        Status = STATUS_DEVICE_DOES_NOT_EXIST;
        goto Exit;
    }

    /* Only a function no PnP driver has started can go to a legacy driver */
    if (PdoExtension->DeviceState != PciNotStarted)
    {
        Status = STATUS_INVALID_OWNER;
        goto Exit;
    }

    /* Make sure we're not on the PDO for some reason */
    ASSERT(DeviceObject != PdoExtension->PhysicalDeviceObject);

    /* Read the PCI header and cache the routing information */
    PciReadDeviceConfig(PdoExtension, &PciData, 0, PCI_COMMON_HDR_LENGTH);
    Status = PciCacheLegacyDeviceRouting(DeviceObject,
                                         BusNumber,
                                         SlotNumber,
                                         PciData.u.type0.InterruptLine,
                                         PciData.u.type0.InterruptPin,
                                         PciData.BaseClass,
                                         PciData.SubClass,
                                         PdoExtension->ParentFdoExtension->
                                         PhysicalDeviceObject,
                                         PdoExtension,
                                         &ExistingDeviceObject);
    if (!NT_SUCCESS(Status))
        goto Exit;

    /* Manually build the requirements for this device, and mark it legacy */
    PdoExtension->LegacyDriver = TRUE;
    Status = PciBuildRequirementsList(PdoExtension, &PciData, &RequirementsList);
    if (!NT_SUCCESS(Status))
        goto RestoreRouting;

    /* The shared empty list asks for nothing and must not be freed */
    if (RequirementsList == PciZeroIoResourceRequirements)
        RequirementsList = NULL;

    /* Now call the legacy Pnp function to actually assign resources */
    Status = IoAssignResources(RegistryPath,
                               DriverClassName,
                               DriverObject,
                               DeviceObject,
                               RequirementsList,
                               &Resources);
    if (!NT_SUCCESS(Status))
    {
        ASSERT(Resources == NULL);
        goto RestoreRouting;
    }

    /* Program the assignment the way a PnP start does */
    PdoExtension->CommandEnables |= (PCI_ENABLE_IO_SPACE |
                                     PCI_ENABLE_MEMORY_SPACE |
                                     PCI_ENABLE_BUS_MASTER);
    PciComputeNewCurrentSettings(PdoExtension, Resources);

    Status = PciSetResources(PdoExtension, TRUE, TRUE);
    if (NT_SUCCESS(Status))
        Status = PciProgramGrantedInterrupt(PdoExtension, Resources);

    if (!NT_SUCCESS(Status))
        goto RestoreRouting;

    /* The caller only gets the ranges and the interrupt */
    if (Resources)
        PciStripPrivateDescriptors(Resources);

    *AllocatedResources = Resources;
    Resources = NULL;
    goto Exit;

RestoreRouting:
    /* Give the slot back to the device object cached for it before */
    PciCacheLegacyDeviceRouting(ExistingDeviceObject,
                                BusNumber,
                                SlotNumber,
                                PciData.u.type0.InterruptLine,
                                PciData.u.type0.InterruptPin,
                                PciData.BaseClass,
                                PciData.SubClass,
                                PdoExtension->ParentFdoExtension->
                                PhysicalDeviceObject,
                                PdoExtension,
                                NULL);

Exit:
    KeSetEvent(&PciGlobalLock, IO_NO_INCREMENT, FALSE);
    KeLeaveCriticalRegion();

    /* Free any temporary resource data and return the status */
    if (RequirementsList)
        ExFreePoolWithTag(RequirementsList, 0);

    if (Resources)
        ExFreePoolWithTag(Resources, 0);

    return Status;
}

VOID
NTAPI
PciHookHal(VOID)
{
    /* Save the old HAL routines */
    ASSERT(PcipSavedAssignSlotResources == NULL);
    ASSERT(PcipSavedTranslateBusAddress == NULL);
    PcipSavedAssignSlotResources = HalPciAssignSlotResources;
    PcipSavedTranslateBusAddress = HalPciTranslateBusAddress;

    /* Take over the HAL's Bus Handler functions */
    HalPciAssignSlotResources = PciAssignSlotResources;
    HalPciTranslateBusAddress = PciTranslateBusAddress;
}

/* EOF */
