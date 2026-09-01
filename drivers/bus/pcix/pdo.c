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
    {IRP_COMPLETE, (PCI_DISPATCH_FUNCTION)PciIrpNotSupported},
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
    PCM_PARTIAL_RESOURCE_DESCRIPTOR MessageResource;
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

    /* Update resource information now that the device is powered up and active */
    Status = PciSetResources(DeviceExtension, DoReset, TRUE);
    if (!NT_SUCCESS(Status))
    {
        /* That failed, so cancel the transition */
        PciCancelStateTransition((PVOID)DeviceExtension, PciStarted);
    }
    else
    {
        /*
         * The device is decoding its windows now, so if the arbiter granted it
         * message interrupts they can be written in. That has to happen here
         * rather than earlier, because an MSI-X table lives inside one of those
         * windows and is not reachable until the window is programmed.
         */
        MessageResource = PciFindMessageInterruptResource(IoStackLocation->
                                                          Parameters.StartDevice.
                                                          AllocatedResources);
        if (MessageResource)
        {
            PciProgramMessageInterrupt(DeviceExtension, MessageResource);
        }

        /* Fully commit, as the device is now started up and ready to go */
        PciCommitStateTransition((PVOID)DeviceExtension, PciStarted);
    }

    /* Return the result of the start request */
    return Status;
}

/*
 * A device the system is leaning on cannot be taken away from it. Losing the
 * device underneath the paging file, the hibernation image or the crash dump
 * would take the system with it, so those requests are refused outright.
 */
static
BOOLEAN
NTAPI
PciPdoIsDeviceRemovable(IN PPCI_PDO_EXTENSION DeviceExtension)
{
    return !((DeviceExtension->PowerState.Paging) ||
             (DeviceExtension->PowerState.Hibernate) ||
             (DeviceExtension->PowerState.CrashDump));
}

/*
 * Stop the device using the resources it was given. Once its decodes are off
 * and its messages are disabled, the addresses and vectors it was holding can
 * safely be handed to something else.
 */
static
VOID
NTAPI
PciPdoReleaseDevice(IN PPCI_PDO_EXTENSION DeviceExtension)
{
    if (DeviceExtension->DeviceState != PciStarted) return;

    PciDisableMessageInterrupt(DeviceExtension);
    PciDecodeEnable(DeviceExtension, FALSE, NULL);
}

/*
 * Move to a state the device was not already asked to move to. A request that
 * follows a successful query has already begun its transition, and only needs
 * committing.
 */
static
VOID
NTAPI
PciPdoEnterState(IN PPCI_PDO_EXTENSION DeviceExtension,
                 IN PCI_STATE NewState)
{
    if (DeviceExtension->DeviceState == NewState) return;

    if (DeviceExtension->TentativeNextState == DeviceExtension->DeviceState)
    {
        if (!NT_SUCCESS(PciBeginStateTransition((PVOID)DeviceExtension, NewState))) return;
    }
    else if (DeviceExtension->TentativeNextState != NewState)
    {
        /* Something else is pending, so abandon that first */
        PciCancelStateTransition((PVOID)DeviceExtension,
                                 DeviceExtension->TentativeNextState);
        if (!NT_SUCCESS(PciBeginStateTransition((PVOID)DeviceExtension, NewState))) return;
    }

    PciCommitStateTransition((PVOID)DeviceExtension, NewState);
}

NTSTATUS
NTAPI
PciPdoIrpQueryRemoveDevice(IN PIRP Irp,
                           IN PIO_STACK_LOCATION IoStackLocation,
                           IN PPCI_PDO_EXTENSION DeviceExtension)
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(Irp);
    UNREFERENCED_PARAMETER(IoStackLocation);

    /* Refuse to give up a device the system cannot do without */
    if (!PciPdoIsDeviceRemovable(DeviceExtension)) return STATUS_DEVICE_BUSY;

    /* Ask the state machine whether leaving the started state is legal */
    return PciBeginStateTransition((PVOID)DeviceExtension, PciNotStarted);
}

NTSTATUS
NTAPI
PciPdoIrpRemoveDevice(IN PIRP Irp,
                      IN PIO_STACK_LOCATION IoStackLocation,
                      IN PPCI_PDO_EXTENSION DeviceExtension)
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(Irp);
    UNREFERENCED_PARAMETER(IoStackLocation);

    /* A device that is still there just goes back to being merely enumerated */
    PciPdoReleaseDevice(DeviceExtension);
    PciPdoEnterState(DeviceExtension,
                     DeviceExtension->ReportedMissing ? PciDeleted : PciNotStarted);
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
PciPdoIrpCancelRemoveDevice(IN PIRP Irp,
                            IN PIO_STACK_LOCATION IoStackLocation,
                            IN PPCI_PDO_EXTENSION DeviceExtension)
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(Irp);
    UNREFERENCED_PARAMETER(IoStackLocation);

    /* The remove is off, so the device stays where it was */
    PciCancelStateTransition((PVOID)DeviceExtension, PciNotStarted);
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
PciPdoIrpStopDevice(IN PIRP Irp,
                    IN PIO_STACK_LOCATION IoStackLocation,
                    IN PPCI_PDO_EXTENSION DeviceExtension)
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(Irp);
    UNREFERENCED_PARAMETER(IoStackLocation);

    /* The device is losing its resources, so it must stop using them first */
    PciPdoReleaseDevice(DeviceExtension);
    PciPdoEnterState(DeviceExtension, PciStopped);
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
PciPdoIrpQueryStopDevice(IN PIRP Irp,
                         IN PIO_STACK_LOCATION IoStackLocation,
                         IN PPCI_PDO_EXTENSION DeviceExtension)
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(Irp);
    UNREFERENCED_PARAMETER(IoStackLocation);

    /* Refuse to give up a device the system cannot do without */
    if (!PciPdoIsDeviceRemovable(DeviceExtension)) return STATUS_DEVICE_BUSY;

    /* Ask the state machine whether the device may be stopped at all */
    return PciBeginStateTransition((PVOID)DeviceExtension, PciStopped);
}

NTSTATUS
NTAPI
PciPdoIrpCancelStopDevice(IN PIRP Irp,
                          IN PIO_STACK_LOCATION IoStackLocation,
                          IN PPCI_PDO_EXTENSION DeviceExtension)
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(Irp);
    UNREFERENCED_PARAMETER(IoStackLocation);

    /* The stop is off, so the device keeps what it has */
    PciCancelStateTransition((PVOID)DeviceExtension, PciStopped);
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

    UNREFERENCED_PARAMETER(Irp);

    /* Query the interface from the PCI driver */
    Status = PciQueryInterface(
                (PPCI_FDO_EXTENSION)DeviceExtension,
                IoStackLocation->Parameters.QueryInterface.InterfaceType,
                IoStackLocation->Parameters.QueryInterface.Size,
                IoStackLocation->Parameters.QueryInterface.Version,
                IoStackLocation->Parameters.QueryInterface.InterfaceSpecificData,
                IoStackLocation->Parameters.QueryInterface.Interface,
                FALSE);

    /* TODO: There's more to this story, but NOT TODAY! */

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

/*
 * Serve a driver's request to reach its own device's configuration space. Only
 * the space this driver owns can be served; the option ROM is decoded by the
 * device rather than read out of configuration space, so it is not handled here.
 */
static
NTSTATUS
NTAPI
PciPdoReadWriteConfig(IN PIRP Irp,
                      IN PIO_STACK_LOCATION IoStackLocation,
                      IN PPCI_PDO_EXTENSION DeviceExtension,
                      IN BOOLEAN Read)
{
    ULONG Offset, Length, Limit;
    PAGED_CODE();

    if (IoStackLocation->Parameters.ReadWriteConfig.WhichSpace !=
        PCI_WHICHSPACE_CONFIG)
    {
        return STATUS_INVALID_PARAMETER;
    }

    Offset = IoStackLocation->Parameters.ReadWriteConfig.Offset;
    Length = IoStackLocation->Parameters.ReadWriteConfig.Length;

    /* Only a function on an Express machine has anything past the first 256 */
    Limit = PciEcamEnabled ? PCI_EXTENDED_CONFIG_LENGTH : PCI_LEGACY_CONFIG_LENGTH;

    /* The whole run has to land inside that space */
    if ((Offset >= Limit) || (Length > (Limit - Offset)))
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (Read)
    {
        PciReadDeviceConfig(DeviceExtension,
                            IoStackLocation->Parameters.ReadWriteConfig.Buffer,
                            Offset,
                            Length);
    }
    else
    {
        PciWriteDeviceConfig(DeviceExtension,
                             IoStackLocation->Parameters.ReadWriteConfig.Buffer,
                             Offset,
                             Length);
    }

    Irp->IoStatus.Information = Length;
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
PciPdoIrpReadConfig(IN PIRP Irp,
                    IN PIO_STACK_LOCATION IoStackLocation,
                    IN PPCI_PDO_EXTENSION DeviceExtension)
{
    PAGED_CODE();

    return PciPdoReadWriteConfig(Irp, IoStackLocation, DeviceExtension, TRUE);
}

NTSTATUS
NTAPI
PciPdoIrpWriteConfig(IN PIRP Irp,
                     IN PIO_STACK_LOCATION IoStackLocation,
                     IN PPCI_PDO_EXTENSION DeviceExtension)
{
    PAGED_CODE();

    return PciPdoReadWriteConfig(Irp, IoStackLocation, DeviceExtension, FALSE);
}

NTSTATUS
NTAPI
PciPdoIrpQueryDeviceState(IN PIRP Irp,
                          IN PIO_STACK_LOCATION IoStackLocation,
                          IN PPCI_PDO_EXTENSION DeviceExtension)
{
    PNP_DEVICE_STATE State;
    PAGED_CODE();

    UNREFERENCED_PARAMETER(IoStackLocation);

    /* Whatever the rest of the stack decided about this device still holds */
    State = (PNP_DEVICE_STATE)Irp->IoStatus.Information;

    /*
     * A device the system cannot be taken away from must not be offered up for
     * disabling: the hardware the debugger talks through, and anything
     * carrying the paging file, the hibernation image or the crash dump.
     */
    if ((DeviceExtension->OnDebugPath) ||
        (DeviceExtension->PowerState.Paging) ||
        (DeviceExtension->PowerState.Hibernate) ||
        (DeviceExtension->PowerState.CrashDump))
    {
        State |= PNP_DEVICE_NOT_DISABLEABLE;
    }

    Irp->IoStatus.Information = State;
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
PciPdoIrpDeviceUsageNotification(IN PIRP Irp,
                                 IN PIO_STACK_LOCATION IoStackLocation,
                                 IN PPCI_PDO_EXTENSION DeviceExtension)
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(Irp);

    /*
     * Record it against the device, and against the bus it sits on: both have
     * to stay where they are for as long as anything depends on the device.
     */
    PciApplyDeviceUsage(&DeviceExtension->PowerState, IoStackLocation);
    PciApplyDeviceUsage(&DeviceExtension->ParentFdoExtension->PowerState,
                        IoStackLocation);
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
PciPdoIrpSurpriseRemoval(IN PIRP Irp,
                         IN PIO_STACK_LOCATION IoStackLocation,
                         IN PPCI_PDO_EXTENSION DeviceExtension)
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(Irp);
    UNREFERENCED_PARAMETER(IoStackLocation);

    /*
     * The device is already gone, so its configuration space cannot be touched
     * to turn anything off - there is nothing left to turn off. All that is
     * left is to write it off, so nothing tries to use it again.
     */
    DeviceExtension->ReportedMissing = TRUE;
    PciPdoEnterState(DeviceExtension, PciSurpriseRemoved);
    return STATUS_SUCCESS;
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
    _swprintf(DeviceName, L"\\Device\\NTPNP_PCI%04d", SequenceNumber);
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
