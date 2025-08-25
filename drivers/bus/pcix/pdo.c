/*
 * PROJECT:         ReactOS PCI Bus Driver
 * LICENSE:         BSD - See COPYING.ARM in the top level directory
 * FILE:            drivers/bus/pci/pdo.c
 * PURPOSE:         PDO Device Management
 * PROGRAMMERS:     ReactOS Portable Systems Group
 */

/* INCLUDES *******************************************************************/

#include <pci.h>

#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

LONG PciPdoSequenceNumber;

C_ASSERT(FIELD_OFFSET(PCI_FDO_EXTENSION, DeviceState) == FIELD_OFFSET(PCI_PDO_EXTENSION, DeviceState));
C_ASSERT(FIELD_OFFSET(PCI_FDO_EXTENSION, TentativeNextState) == FIELD_OFFSET(PCI_PDO_EXTENSION, TentativeNextState));
C_ASSERT(FIELD_OFFSET(PCI_FDO_EXTENSION, List) == FIELD_OFFSET(PCI_PDO_EXTENSION, Next));

PCI_MN_DISPATCH_TABLE PciPdoDispatchPowerTable[] =
{
    {IRP_DISPATCH, (PCI_DISPATCH_FUNCTION)PciPdoWaitWake},
    {IRP_COMPLETE, (PCI_DISPATCH_FUNCTION)PciIrpNotSupported},
    {IRP_COMPLETE, (PCI_DISPATCH_FUNCTION)PciPdoSetPowerState},
    {IRP_COMPLETE, (PCI_DISPATCH_FUNCTION)PciPdoIrpQueryPower},
    {IRP_COMPLETE, (PCI_DISPATCH_FUNCTION)PciIrpNotSupported}
};

PCI_MN_DISPATCH_TABLE PciPdoDispatchPnpTable[] =
{
    {IRP_COMPLETE, (PCI_DISPATCH_FUNCTION)PciPdoIrpStartDevice},
    {IRP_COMPLETE, (PCI_DISPATCH_FUNCTION)PciPdoIrpQueryRemoveDevice},
    {IRP_COMPLETE, (PCI_DISPATCH_FUNCTION)PciPdoIrpRemoveDevice},
    {IRP_COMPLETE, (PCI_DISPATCH_FUNCTION)PciPdoIrpCancelRemoveDevice},
    {IRP_COMPLETE, (PCI_DISPATCH_FUNCTION)PciPdoIrpStopDevice},
    {IRP_COMPLETE, (PCI_DISPATCH_FUNCTION)PciPdoIrpQueryStopDevice},
    {IRP_COMPLETE, (PCI_DISPATCH_FUNCTION)PciPdoIrpCancelStopDevice},
    {IRP_COMPLETE, (PCI_DISPATCH_FUNCTION)PciPdoIrpQueryDeviceRelations},
    {IRP_COMPLETE, (PCI_DISPATCH_FUNCTION)PciPdoIrpQueryInterface},
    {IRP_COMPLETE, (PCI_DISPATCH_FUNCTION)PciPdoIrpQueryCapabilities},
    {IRP_COMPLETE, (PCI_DISPATCH_FUNCTION)PciPdoIrpQueryResources},
    {IRP_COMPLETE, (PCI_DISPATCH_FUNCTION)PciPdoIrpQueryResourceRequirements},
    {IRP_COMPLETE, (PCI_DISPATCH_FUNCTION)PciPdoIrpQueryDeviceText},
    {IRP_COMPLETE, (PCI_DISPATCH_FUNCTION)PciPdoIrpFilterResourceRequirements},
    {IRP_COMPLETE, (PCI_DISPATCH_FUNCTION)PciIrpNotSupported},
    {IRP_COMPLETE, (PCI_DISPATCH_FUNCTION)PciPdoIrpReadConfig},
    {IRP_COMPLETE, (PCI_DISPATCH_FUNCTION)PciPdoIrpWriteConfig},
    {IRP_COMPLETE, (PCI_DISPATCH_FUNCTION)PciIrpNotSupported},
    {IRP_COMPLETE, (PCI_DISPATCH_FUNCTION)PciIrpNotSupported},
    {IRP_COMPLETE, (PCI_DISPATCH_FUNCTION)PciPdoIrpQueryId},
    {IRP_COMPLETE, (PCI_DISPATCH_FUNCTION)PciPdoIrpQueryDeviceState},
    {IRP_COMPLETE, (PCI_DISPATCH_FUNCTION)PciPdoIrpQueryBusInformation},
    {IRP_COMPLETE, (PCI_DISPATCH_FUNCTION)PciPdoIrpDeviceUsageNotification},
    {IRP_COMPLETE, (PCI_DISPATCH_FUNCTION)PciPdoIrpSurpriseRemoval},
    {IRP_COMPLETE, (PCI_DISPATCH_FUNCTION)PciPdoIrpQueryLegacyBusInformation},
    {IRP_COMPLETE, (PCI_DISPATCH_FUNCTION)PciIrpNotSupported}
};

PCI_MJ_DISPATCH_TABLE PciPdoDispatchTable =
{
    IRP_MN_QUERY_LEGACY_BUS_INFORMATION,
    PciPdoDispatchPnpTable,
    IRP_MN_QUERY_POWER,
    PciPdoDispatchPowerTable,
    IRP_COMPLETE,
    (PCI_DISPATCH_FUNCTION)PciIrpNotSupported,
    IRP_COMPLETE,
    (PCI_DISPATCH_FUNCTION)PciIrpInvalidDeviceRequest
};

/* FUNCTIONS ******************************************************************/

NTSTATUS
NTAPI
PciPdoWaitWake(IN PIRP Irp,
               IN PIO_STACK_LOCATION IoStackLocation,
               IN PPCI_PDO_EXTENSION DeviceExtension)
{
    UNREFERENCED_PARAMETER(Irp);
    UNREFERENCED_PARAMETER(IoStackLocation);
    UNREFERENCED_PARAMETER(DeviceExtension);

    UNIMPLEMENTED_DBGBREAK();
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS
NTAPI
PciPdoSetPowerState(IN PIRP Irp,
                    IN PIO_STACK_LOCATION IoStackLocation,
                    IN PPCI_PDO_EXTENSION DeviceExtension)
{
    UNREFERENCED_PARAMETER(Irp);
    UNREFERENCED_PARAMETER(IoStackLocation);
    UNREFERENCED_PARAMETER(DeviceExtension);

    UNIMPLEMENTED;
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS
NTAPI
PciPdoIrpQueryPower(IN PIRP Irp,
                    IN PIO_STACK_LOCATION IoStackLocation,
                    IN PPCI_PDO_EXTENSION DeviceExtension)
{
    UNREFERENCED_PARAMETER(Irp);
    UNREFERENCED_PARAMETER(IoStackLocation);
    UNREFERENCED_PARAMETER(DeviceExtension);

    UNIMPLEMENTED_DBGBREAK();
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS
NTAPI
PciPdoIrpStartDevice(IN PIRP Irp,
                     IN PIO_STACK_LOCATION IoStackLocation,
                     IN PPCI_PDO_EXTENSION DeviceExtension)
{
    NTSTATUS Status;
    BOOLEAN Changed, DoReset;
    POWER_STATE PowerState;
    PAGED_CODE();

    UNREFERENCED_PARAMETER(Irp);

    DoReset = FALSE;

    /* Begin entering the start phase */
    Status = PciBeginStateTransition((PVOID)DeviceExtension, PciStarted);
    if (!NT_SUCCESS(Status)) return Status;

    /* Check if this is a VGA device */
    if (((DeviceExtension->BaseClass == PCI_CLASS_PRE_20) &&
         (DeviceExtension->SubClass == PCI_SUBCLASS_PRE_20_VGA)) ||
        ((DeviceExtension->BaseClass == PCI_CLASS_DISPLAY_CTLR) &&
         (DeviceExtension->SubClass == PCI_SUBCLASS_VID_VGA_CTLR)))
    {
        /* Always force it on */
        DeviceExtension->CommandEnables |= (PCI_ENABLE_IO_SPACE |
                                            PCI_ENABLE_MEMORY_SPACE);
    }

    /* Check if native IDE is enabled and it owns the I/O ports */
    if (DeviceExtension->IoSpaceUnderNativeIdeControl)
    {
        /* Then don't allow I/O access */
        DeviceExtension->CommandEnables &= ~PCI_ENABLE_IO_SPACE;
    }

    /* Always enable bus mastering */
    DeviceExtension->CommandEnables |= PCI_ENABLE_BUS_MASTER;

    /* Check if the OS assigned resources differ from the PCI configuration */
    Changed = PciComputeNewCurrentSettings(DeviceExtension,
                                           IoStackLocation->Parameters.
                                           StartDevice.AllocatedResources);
    if (Changed)
    {
        /* Remember this for later */
        DeviceExtension->MovedDevice = TRUE;
    }
    else
    {
        /* All good */
        DPRINT1("PCI - START not changing resource settings.\n");
    }

    /* Check if the device was sleeping */
    if (DeviceExtension->PowerState.CurrentDeviceState != PowerDeviceD0)
    {
        /* Power it up */
        Status = PciSetPowerManagedDevicePowerState(DeviceExtension,
                                                    PowerDeviceD0,
                                                    FALSE);
        if (!NT_SUCCESS(Status))
        {
            /* Powerup fail, fail the request */
            PciCancelStateTransition((PVOID)DeviceExtension, PciStarted);
            return STATUS_DEVICE_POWER_FAILURE;
        }

        /* Tell the power manager that the device is powered up */
        PowerState.DeviceState = PowerDeviceD0;
        PoSetPowerState(DeviceExtension->PhysicalDeviceObject,
                        DevicePowerState,
                        PowerState);

        /* Update internal state */
        DeviceExtension->PowerState.CurrentDeviceState = PowerDeviceD0;

        /* This device's resources and decodes will need to be reset */
        DoReset = TRUE;
    }

    DPRINT1("PCI - START: going into PciSetResources DoReset = %d\n", DoReset);
    /* Update resource information now that the device is powered up and active */
    Status = PciSetResources(DeviceExtension, DoReset, TRUE);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("PCI - START: PciSetResources failed\n");
        /* That failed, so cancel the transition */
        PciCancelStateTransition((PVOID)DeviceExtension, PciStarted);
    }
    else
    {
        DPRINT1("PCI - START: PciSetResources succeeded\n");
        /* Fully commit, as the device is now started up and ready to go */
        PciCommitStateTransition((PVOID)DeviceExtension, PciStarted);
    }
    DPRINT1("PCI - START: Status = %lx\n", Status);
    /* Return the result of the start request */
    return Status;
}

NTSTATUS
NTAPI
PciPdoIrpQueryRemoveDevice(IN PIRP Irp,
                           IN PIO_STACK_LOCATION IoStackLocation,
                           IN PPCI_PDO_EXTENSION DeviceExtension)
{
    UNREFERENCED_PARAMETER(IoStackLocation);
    UNREFERENCED_PARAMETER(DeviceExtension);

    /* Allow removal by default; filter drivers may veto */
    Irp->IoStatus.Information = 0;
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
PciPdoIrpRemoveDevice(IN PIRP Irp,
                      IN PIO_STACK_LOCATION IoStackLocation,
                      IN PPCI_PDO_EXTENSION DeviceExtension)
{
    UNREFERENCED_PARAMETER(IoStackLocation);

    /* Mark PDO as removed; the bus driver will cleanup on its own flow */
    DeviceExtension->ReportedMissing = TRUE;
    DeviceExtension->NotPresent = TRUE;
    Irp->IoStatus.Information = 0;
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
PciPdoIrpCancelRemoveDevice(IN PIRP Irp,
                            IN PIO_STACK_LOCATION IoStackLocation,
                            IN PPCI_PDO_EXTENSION DeviceExtension)
{
    UNREFERENCED_PARAMETER(IoStackLocation);
    UNREFERENCED_PARAMETER(DeviceExtension);

    Irp->IoStatus.Information = 0;
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
PciPdoIrpStopDevice(IN PIRP Irp,
                    IN PIO_STACK_LOCATION IoStackLocation,
                    IN PPCI_PDO_EXTENSION DeviceExtension)
{
    UNREFERENCED_PARAMETER(IoStackLocation);
    UNREFERENCED_PARAMETER(DeviceExtension);

    /* For now, nothing to do for PDO stop; resources are managed by bus */
    Irp->IoStatus.Information = 0;
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
PciPdoIrpQueryStopDevice(IN PIRP Irp,
                         IN PIO_STACK_LOCATION IoStackLocation,
                         IN PPCI_PDO_EXTENSION DeviceExtension)
{
    UNREFERENCED_PARAMETER(IoStackLocation);
    UNREFERENCED_PARAMETER(DeviceExtension);

    Irp->IoStatus.Information = 0;
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
PciPdoIrpCancelStopDevice(IN PIRP Irp,
                          IN PIO_STACK_LOCATION IoStackLocation,
                          IN PPCI_PDO_EXTENSION DeviceExtension)
{
    UNREFERENCED_PARAMETER(IoStackLocation);
    UNREFERENCED_PARAMETER(DeviceExtension);

    Irp->IoStatus.Information = 0;
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
PciPdoIrpQueryInterface(IN PIRP Irp,
                        IN PIO_STACK_LOCATION IoStackLocation,
                        IN PPCI_PDO_EXTENSION DeviceExtension)
{
    NTSTATUS Status;
    PAGED_CODE();
    /* Query our own PDO-exposed interfaces (e.g., BUS_INTERFACE_STANDARD) */
    Status = PciQueryInterface((PPCI_FDO_EXTENSION)DeviceExtension,
                               IoStackLocation->Parameters.QueryInterface.InterfaceType,
                               IoStackLocation->Parameters.QueryInterface.Size,
                               IoStackLocation->Parameters.QueryInterface.Version,
                               IoStackLocation->Parameters.QueryInterface.InterfaceSpecificData,
                               IoStackLocation->Parameters.QueryInterface.Interface,
                               FALSE);
    /* Let the dispatcher complete the IRP for IRP_COMPLETE style */
    return Status;
}

NTSTATUS
NTAPI
PciPdoIrpQueryDeviceRelations(IN PIRP Irp,
                              IN PIO_STACK_LOCATION IoStackLocation,
                              IN PPCI_PDO_EXTENSION DeviceExtension)
{
    NTSTATUS Status;
    PAGED_CODE();

    /* Are ejection relations being queried? */
    if (IoStackLocation->Parameters.QueryDeviceRelations.Type == EjectionRelations)
    {
        /* Call the worker function */
        Status = PciQueryEjectionRelations(DeviceExtension,
                                           (PDEVICE_RELATIONS*)&Irp->
                                           IoStatus.Information);
    }
    else if (IoStackLocation->Parameters.QueryDeviceRelations.Type == TargetDeviceRelation)
    {
        /* The only other relation supported is the target device relation */
        Status = PciQueryTargetDeviceRelations(DeviceExtension,
                                               (PDEVICE_RELATIONS*)&Irp->
                                               IoStatus.Information);
    }
    else
    {
        /* All other relations are unsupported */
        Status = STATUS_NOT_SUPPORTED;
    }

    /* Return either the result of the worker function, or unsupported status */
    return Status;
}

NTSTATUS
NTAPI
PciPdoIrpQueryCapabilities(IN PIRP Irp,
                           IN PIO_STACK_LOCATION IoStackLocation,
                           IN PPCI_PDO_EXTENSION DeviceExtension)
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(Irp);

    /* Call the worker function */
    return PciQueryCapabilities(DeviceExtension,
                                IoStackLocation->
                                Parameters.DeviceCapabilities.Capabilities);
}

NTSTATUS
NTAPI
PciPdoIrpQueryResources(IN PIRP Irp,
                        IN PIO_STACK_LOCATION IoStackLocation,
                        IN PPCI_PDO_EXTENSION DeviceExtension)
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(IoStackLocation);

    /* Call the worker function */
    return PciQueryResources(DeviceExtension,
                            (PCM_RESOURCE_LIST*)&Irp->IoStatus.Information);
}

NTSTATUS
NTAPI
PciPdoIrpQueryResourceRequirements(IN PIRP Irp,
                                   IN PIO_STACK_LOCATION IoStackLocation,
                                   IN PPCI_PDO_EXTENSION DeviceExtension)
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(IoStackLocation);

    /* Call the worker function */
    return PciQueryRequirements(DeviceExtension,
                                (PIO_RESOURCE_REQUIREMENTS_LIST*)&Irp->
                                IoStatus.Information);
}

static
VOID
PciAppendMessageInterruptDescriptor(IN PIO_RESOURCE_LIST DestList)
{
    PIO_RESOURCE_DESCRIPTOR d;
    d = &DestList->Descriptors[DestList->Count++];
    RtlZeroMemory(d, sizeof(*d));
    d->Type = CmResourceTypeInterrupt;
    d->ShareDisposition = CmResourceShareDeviceExclusive;
    d->Flags = CM_RESOURCE_INTERRUPT_MESSAGE;
    d->u.Interrupt.MinimumVector = CM_RESOURCE_INTERRUPT_MESSAGE_TOKEN;
    d->u.Interrupt.MaximumVector = CM_RESOURCE_INTERRUPT_MESSAGE_TOKEN;
    d->u.Interrupt.AffinityPolicy = IrqPolicyMachineDefault;
    d->u.Interrupt.PriorityPolicy = IrqPriorityUndefined;
    d->u.Interrupt.TargetedProcessors = 0;
}

static
PIO_RESOURCE_LIST
PciDuplicateIoResourceList(IN PIO_RESOURCE_LIST Src)
{
    SIZE_T size;
    PIO_RESOURCE_LIST dst;
    size = FIELD_OFFSET(IO_RESOURCE_LIST, Descriptors) + sizeof(IO_RESOURCE_DESCRIPTOR) * Src->Count;
    dst = ExAllocatePoolWithTag(PagedPool, size, PCI_POOL_TAG);
    if (!dst) return NULL;
    RtlCopyMemory(dst, Src, size);
    return dst;
}

NTSTATUS
NTAPI
PciPdoIrpFilterResourceRequirements(IN PIRP Irp,
                                    IN PIO_STACK_LOCATION IoStackLocation,
                                    IN PPCI_PDO_EXTENSION DeviceExtension)
{
    PIO_RESOURCE_REQUIREMENTS_LIST InReqs, OutReqs;
    SIZE_T OutSize, AltSize;
    ULONG i;
    PUCHAR AppendPtr;
    PAGED_CODE();

    UNREFERENCED_PARAMETER(Irp);
    UNREFERENCED_PARAMETER(IoStackLocation);

    InReqs = (PIO_RESOURCE_REQUIREMENTS_LIST)Irp->IoStatus.Information;

    /* Only act if device supports MSI/MSI-X */
    if ((!DeviceExtension->MsiCapabilityOffset) && (!DeviceExtension->MsixCapabilityOffset))
    {
        return Irp->IoStatus.Status;
    }

    if (!InReqs)
    {
        /* Nothing to filter; leave as-is */
        return Irp->IoStatus.Status;
    }

    /*
     * Windows duplicates EACH alternative list and creates a message-capable
     * variant. If an alternative lacks an interrupt descriptor, one is added.
     * Compute the exact output size first.
     */
    OutSize = InReqs->ListSize;
    {
        PIO_RESOURCE_LIST SrcAlt;
        PUCHAR Ptr = (PUCHAR)&InReqs->List[0];
        for (i = 0; i < InReqs->AlternativeLists; i++)
        {
            BOOLEAN HasInterrupt = FALSE;
            ULONG d;
            SrcAlt = (PIO_RESOURCE_LIST)Ptr;
            for (d = 0; d < SrcAlt->Count; d++)
            {
                if (SrcAlt->Descriptors[d].Type == CmResourceTypeInterrupt)
                {
                    HasInterrupt = TRUE;
                    break;
                }
            }

            /* Size of a copy of this alternative */
            AltSize = FIELD_OFFSET(IO_RESOURCE_LIST, Descriptors) + sizeof(IO_RESOURCE_DESCRIPTOR) * SrcAlt->Count;

            if (HasInterrupt)
            {
                /* We'll duplicate the list 1:1 and convert INTx->MSI */
                OutSize += AltSize;
            }
            else
            {
                /* We'll duplicate and APPEND one MSI descriptor */
                OutSize += FIELD_OFFSET(IO_RESOURCE_LIST, Descriptors) + sizeof(IO_RESOURCE_DESCRIPTOR) * (SrcAlt->Count + 1);
            }

            /* Advance to next alternative in the input */
            Ptr += AltSize;
        }
    }

    OutReqs = ExAllocatePoolWithTag(PagedPool, OutSize, PCI_POOL_TAG);
    if (!OutReqs)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* Copy existing blob first */
    RtlCopyMemory(OutReqs, InReqs, InReqs->ListSize);

    /* We will append the new alternatives after the original content */
    AppendPtr = (PUCHAR)OutReqs + InReqs->ListSize;

    /* Recompute and append per-alternative MSI variants */
    {
        PIO_RESOURCE_LIST SrcAlt;
        PUCHAR Ptr = (PUCHAR)&InReqs->List[0];
        ULONG NewAltCount = InReqs->AlternativeLists;
        for (i = 0; i < InReqs->AlternativeLists; i++)
        {
            ULONG d;
            LONG IntIndex = -1;
            SrcAlt = (PIO_RESOURCE_LIST)Ptr;

            /* Find first interrupt descriptor, if any */
            for (d = 0; d < SrcAlt->Count; d++)
            {
                if (SrcAlt->Descriptors[d].Type == CmResourceTypeInterrupt)
                {
                    IntIndex = (LONG)d;
                    break;
                }
            }

            if (IntIndex >= 0)
            {
                /* Duplicate this alternative and convert INTx -> MSI */
                PIO_RESOURCE_LIST DstAlt;
                AltSize = FIELD_OFFSET(IO_RESOURCE_LIST, Descriptors) + sizeof(IO_RESOURCE_DESCRIPTOR) * SrcAlt->Count;
                DstAlt = (PIO_RESOURCE_LIST)AppendPtr;
                RtlCopyMemory(DstAlt, SrcAlt, AltSize);

                /* Make the copy advertise message-signaled interrupt */
                DstAlt->Descriptors[IntIndex].ShareDisposition = CmResourceShareDeviceExclusive;
                DstAlt->Descriptors[IntIndex].Flags = CM_RESOURCE_INTERRUPT_MESSAGE;
                DstAlt->Descriptors[IntIndex].u.Interrupt.MinimumVector = CM_RESOURCE_INTERRUPT_MESSAGE_TOKEN;
                DstAlt->Descriptors[IntIndex].u.Interrupt.MaximumVector = CM_RESOURCE_INTERRUPT_MESSAGE_TOKEN;
                DstAlt->Descriptors[IntIndex].u.Interrupt.AffinityPolicy = IrqPolicyMachineDefault;
                DstAlt->Descriptors[IntIndex].u.Interrupt.PriorityPolicy = IrqPriorityUndefined;
                DstAlt->Descriptors[IntIndex].u.Interrupt.TargetedProcessors = 0;

                AppendPtr += AltSize;
                NewAltCount += 1;
            }
            else
            {
                /* Duplicate and append a new MSI descriptor */
                PIO_RESOURCE_LIST DstAlt;
                AltSize = FIELD_OFFSET(IO_RESOURCE_LIST, Descriptors) + sizeof(IO_RESOURCE_DESCRIPTOR) * (SrcAlt->Count + 1);
                DstAlt = (PIO_RESOURCE_LIST)AppendPtr;
                RtlZeroMemory(DstAlt, AltSize);
                DstAlt->Version = SrcAlt->Version;
                DstAlt->Revision = SrcAlt->Revision;
                DstAlt->Count = SrcAlt->Count;
                if (SrcAlt->Count)
                {
                    RtlCopyMemory(DstAlt->Descriptors, SrcAlt->Descriptors, sizeof(IO_RESOURCE_DESCRIPTOR) * SrcAlt->Count);
                }
                /* Append MSI */
                PciAppendMessageInterruptDescriptor(DstAlt);

                AppendPtr += AltSize;
                NewAltCount += 1;
            }

            /* Advance to next source alternative */
            Ptr += FIELD_OFFSET(IO_RESOURCE_LIST, Descriptors) + sizeof(IO_RESOURCE_DESCRIPTOR) * SrcAlt->Count;
        }

        OutReqs->AlternativeLists = NewAltCount;
        OutReqs->ListSize = (ULONG)OutSize;
    }

    /* Replace the IRP information pointer, free the original */
    Irp->IoStatus.Information = (ULONG_PTR)OutReqs;
    Irp->IoStatus.Status = STATUS_SUCCESS;

    ExFreePoolWithTag(InReqs, 0);
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
PciPdoIrpQueryDeviceText(IN PIRP Irp,
                         IN PIO_STACK_LOCATION IoStackLocation,
                         IN PPCI_PDO_EXTENSION DeviceExtension)
{
    PAGED_CODE();

    /* Call the worker function */
    return PciQueryDeviceText(DeviceExtension,
                              IoStackLocation->
                              Parameters.QueryDeviceText.DeviceTextType,
                              IoStackLocation->
                              Parameters.QueryDeviceText.LocaleId,
                              (PWCHAR*)&Irp->IoStatus.Information);
}

NTSTATUS
NTAPI
PciPdoIrpQueryId(IN PIRP Irp,
                 IN PIO_STACK_LOCATION IoStackLocation,
                 IN PPCI_PDO_EXTENSION DeviceExtension)
{
    PAGED_CODE();

    /* Call the worker function */
    return PciQueryId(DeviceExtension,
                      IoStackLocation->Parameters.QueryId.IdType,
                      (PWCHAR*)&Irp->IoStatus.Information);
}

NTSTATUS
NTAPI
PciPdoIrpQueryBusInformation(IN PIRP Irp,
                             IN PIO_STACK_LOCATION IoStackLocation,
                             IN PPCI_PDO_EXTENSION DeviceExtension)
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(IoStackLocation);

    /* Call the worker function */
    return PciQueryBusInformation(DeviceExtension,
                                  (PPNP_BUS_INFORMATION*)&Irp->
                                  IoStatus.Information);
}

NTSTATUS
NTAPI
PciPdoIrpReadConfig(IN PIRP Irp,
                    IN PIO_STACK_LOCATION IoStackLocation,
                    IN PPCI_PDO_EXTENSION DeviceExtension)
{
    UNREFERENCED_PARAMETER(Irp);
    UNREFERENCED_PARAMETER(IoStackLocation);
    UNREFERENCED_PARAMETER(DeviceExtension);

    UNIMPLEMENTED_DBGBREAK();
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS
NTAPI
PciPdoIrpWriteConfig(IN PIRP Irp,
                     IN PIO_STACK_LOCATION IoStackLocation,
                     IN PPCI_PDO_EXTENSION DeviceExtension)
{
    UNREFERENCED_PARAMETER(Irp);
    UNREFERENCED_PARAMETER(IoStackLocation);
    UNREFERENCED_PARAMETER(DeviceExtension);

    UNIMPLEMENTED_DBGBREAK();
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS
NTAPI
PciPdoIrpQueryDeviceState(IN PIRP Irp,
                          IN PIO_STACK_LOCATION IoStackLocation,
                          IN PPCI_PDO_EXTENSION DeviceExtension)
{
    UNREFERENCED_PARAMETER(Irp);
    UNREFERENCED_PARAMETER(IoStackLocation);
    UNREFERENCED_PARAMETER(DeviceExtension);

    UNIMPLEMENTED;
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS
NTAPI
PciPdoIrpDeviceUsageNotification(IN PIRP Irp,
                                 IN PIO_STACK_LOCATION IoStackLocation,
                                 IN PPCI_PDO_EXTENSION DeviceExtension)
{
    UNREFERENCED_PARAMETER(Irp);
    UNREFERENCED_PARAMETER(IoStackLocation);
    UNREFERENCED_PARAMETER(DeviceExtension);

    UNIMPLEMENTED_DBGBREAK();
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS
NTAPI
PciPdoIrpSurpriseRemoval(IN PIRP Irp,
                         IN PIO_STACK_LOCATION IoStackLocation,
                         IN PPCI_PDO_EXTENSION DeviceExtension)
{
    UNREFERENCED_PARAMETER(Irp);
    UNREFERENCED_PARAMETER(IoStackLocation);
    UNREFERENCED_PARAMETER(DeviceExtension);

    UNIMPLEMENTED_DBGBREAK();
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS
NTAPI
PciPdoIrpQueryLegacyBusInformation(IN PIRP Irp,
                                   IN PIO_STACK_LOCATION IoStackLocation,
                                   IN PPCI_PDO_EXTENSION DeviceExtension)
{
    UNREFERENCED_PARAMETER(Irp);
    UNREFERENCED_PARAMETER(IoStackLocation);
    UNREFERENCED_PARAMETER(DeviceExtension);

    UNIMPLEMENTED_DBGBREAK();
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS
NTAPI
PciPdoCreate(IN PPCI_FDO_EXTENSION DeviceExtension,
             IN PCI_SLOT_NUMBER Slot,
             OUT PDEVICE_OBJECT *PdoDeviceObject)
{
    WCHAR DeviceName[32];
    UNICODE_STRING DeviceString;
    NTSTATUS Status;
    PDEVICE_OBJECT DeviceObject;
    PPCI_PDO_EXTENSION PdoExtension;
    ULONG SequenceNumber;
    PAGED_CODE();

    /* Pick an atomically unique sequence number for this device */
    SequenceNumber = InterlockedIncrement(&PciPdoSequenceNumber);

    /* Create the standard PCI device name for a PDO */
    swprintf(DeviceName, L"\\Device\\NTPNP_PCI%04d", SequenceNumber);
    RtlInitUnicodeString(&DeviceString, DeviceName);

    /* Create the actual device now */
    Status = IoCreateDevice(DeviceExtension->FunctionalDeviceObject->DriverObject,
                            sizeof(PCI_PDO_EXTENSION),
                            &DeviceString,
                            FILE_DEVICE_BUS_EXTENDER,
                            0,
                            0,
                            &DeviceObject);
    ASSERT(NT_SUCCESS(Status));

    /* Get the extension for it */
    PdoExtension = (PPCI_PDO_EXTENSION)DeviceObject->DeviceExtension;
    DPRINT1("PCI: New PDO (b=0x%x, d=0x%x, f=0x%x) @ %p, ext @ %p\n",
            DeviceExtension->BaseBus,
            Slot.u.bits.DeviceNumber,
            Slot.u.bits.FunctionNumber,
            DeviceObject,
            DeviceObject->DeviceExtension);

    /* Configure the extension */
    PdoExtension->ExtensionType = PciPdoExtensionType;
    PdoExtension->IrpDispatchTable = &PciPdoDispatchTable;
    PdoExtension->PhysicalDeviceObject = DeviceObject;
    PdoExtension->Slot = Slot;
    PdoExtension->PowerState.CurrentSystemState = PowerSystemWorking;
    PdoExtension->PowerState.CurrentDeviceState = PowerDeviceD0;
    PdoExtension->ParentFdoExtension = DeviceExtension;

    /* Initialize the lock for arbiters and other interfaces */
    KeInitializeEvent(&PdoExtension->SecondaryExtLock, SynchronizationEvent, TRUE);

    /* Initialize the state machine */
    PciInitializeState((PPCI_FDO_EXTENSION)PdoExtension);

    /* Add the PDO to the parent's list */
    PdoExtension->Next = NULL;
    PciInsertEntryAtTail((PSINGLE_LIST_ENTRY)&DeviceExtension->ChildPdoList,
                         (PPCI_FDO_EXTENSION)PdoExtension,
                         &DeviceExtension->ChildListLock);

    /* And finally return it to the caller */
    *PdoDeviceObject = DeviceObject;
    return STATUS_SUCCESS;
}

/* EOF */
