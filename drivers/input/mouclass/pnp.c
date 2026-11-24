/*
 * PROJECT:     ReactOS Mouse Class Driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Plug and play requests
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "mouclass.h"

#define NDEBUG
#include <debug.h>

static IO_COMPLETION_ROUTINE MouseStartCompletion;
static IO_WORKITEM_ROUTINE MouseStartWorker;

/**
 * @brief
 * Finds the shared port table entry of a port extension. MouGlobals.Lock must be held.
 */
static
PMOUCLASS_PORT
NTAPI
MouseGetSharedEntry(
    _In_ PMOUCLASS_EXTENSION Extension)
{
    PMOUCLASS_PORT Entry;

    if (Extension->UnitId >= MouGlobals.PortCount)
        return NULL;

    Entry = &MouGlobals.Ports[Extension->UnitId];
    return (Entry->Port == Extension) ? Entry : NULL;
}

/**
 * @brief
 * Finishes a start once the lower stack has started.
 *
 * Reads the power capabilities, registers WMI, then turns the port on for the
 * shared class device or enables the mouse interface of a per port class device.
 *
 * @return
 * The status the start IRP completes with.
 */
static
NTSTATUS
NTAPI
MouseStartDevice(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PMOUCLASS_EXTENSION Extension = DeviceObject->DeviceExtension;
    NTSTATUS StartStatus = Irp->IoStatus.Status;
    ULONG_PTR StartInformation;
    DEVICE_CAPABILITIES Capabilities;
    PIO_STACK_LOCATION Next;
    PMOUCLASS_PORT Entry;
    PFILE_OBJECT FileObject;
    BOOLEAN WasEnabled;
    NTSTATUS Status = STATUS_SUCCESS;
    ULONG Index;

    if (!NT_SUCCESS(StartStatus))
        return StartStatus;

    StartInformation = Irp->IoStatus.Information;

    RtlZeroMemory(&Capabilities, sizeof(Capabilities));
    Capabilities.Size = sizeof(Capabilities);
    Capabilities.Version = 1;
    Capabilities.Address = MAXULONG;
    Capabilities.UINumber = MAXULONG;

    IoCopyCurrentIrpStackLocationToNext(Irp);
    Next = IoGetNextIrpStackLocation(Irp);
    Next->MinorFunction = IRP_MN_QUERY_CAPABILITIES;
    Next->Parameters.DeviceCapabilities.Capabilities = &Capabilities;

    Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
    Irp->IoStatus.Information = 0;

    if (NT_SUCCESS(MouseForwardAndWait(Extension->LowerDevice, Irp, FALSE)))
    {
        for (Index = 0; Index < PowerSystemHibernate; Index++)
            Extension->SystemToDevice[Index] = Capabilities.DeviceState[Index];

        Extension->WakeDeviceState = Capabilities.DeviceWake;
        Extension->WakeSystemState = Capabilities.SystemWake;
    }

    Irp->IoStatus.Status = StartStatus;
    Irp->IoStatus.Information = StartInformation;

    Extension->IsStarted = TRUE;
    MouseInitializeWmi(Extension);

    ExAcquireFastMutex(&MouGlobals.Lock);

    if (MouGlobals.SharedClass != NULL)
    {
        Entry = MouseGetSharedEntry(Extension);
        if (MouGlobals.SharedOpens <= 0 || Entry == NULL)
        {
            ExReleaseFastMutex(&MouGlobals.Lock);
        }
        else
        {
            /* The shared device is open, so this port has to be opened as well */
            WasEnabled = Entry->Enabled;
            Entry->Enabled = TRUE;
            FileObject = Entry->FileObject;
            ExReleaseFastMutex(&MouGlobals.Lock);

            if (!WasEnabled)
            {
                Status = MouseSwitchPort(TRUE, NULL, Extension, &FileObject);

                ExAcquireFastMutex(&MouGlobals.Lock);
                Entry = MouseGetSharedEntry(Extension);
                if (Entry != NULL)
                {
                    Entry->FileObject = FileObject;
                    if (!NT_SUCCESS(Status))
                        Entry->Enabled = FALSE;
                }
                ExReleaseFastMutex(&MouGlobals.Lock);
            }
        }
    }
    else
    {
        ExReleaseFastMutex(&MouGlobals.Lock);
        Status = IoSetDeviceInterfaceState(&Extension->InterfaceName, TRUE);
    }

    if (MouseCanWake(Extension) &&
        !MouseIsWaitWakeArmed(Extension) &&
        MouseIsWaitWakeEnabled(Extension))
    {
        MouseRequestWaitWake(Extension);
    }

    return Status;
}

static
VOID
NTAPI
MouseStartWorker(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_opt_ PVOID Context)
{
    PMOUCLASS_EXTENSION Extension = DeviceObject->DeviceExtension;
    PMOUCLASS_START_CONTEXT StartContext = Context;
    PIRP Irp = StartContext->Irp;

    Irp->IoStatus.Status = MouseStartDevice(DeviceObject, Irp);
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    IoFreeWorkItem(StartContext->WorkItem);
    ExFreePoolWithTag(StartContext, MOUCLASS_TAG);
    IoReleaseRemoveLock(&Extension->RemoveLock, StartContext);
}

static
NTSTATUS
NTAPI
MouseStartCompletion(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_opt_ PVOID Context)
{
    PMOUCLASS_START_CONTEXT StartContext = Context;

    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);

    /* Finish the start at passive level */
    IoQueueWorkItem(StartContext->WorkItem, MouseStartWorker, DelayedWorkQueue, StartContext);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

static
NTSTATUS
NTAPI
MouseHandleStart(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PMOUCLASS_EXTENSION Extension = DeviceObject->DeviceExtension;
    PMOUCLASS_START_CONTEXT StartContext;
    NTSTATUS Status;

    StartContext = ExAllocatePoolWithTag(NonPagedPool, sizeof(*StartContext), MOUCLASS_TAG);
    if (StartContext != NULL)
    {
        StartContext->WorkItem = IoAllocateWorkItem(DeviceObject);
        if (StartContext->WorkItem == NULL)
        {
            ExFreePoolWithTag(StartContext, MOUCLASS_TAG);
            StartContext = NULL;
        }
    }

    if (StartContext != NULL)
    {
        StartContext->Irp = Irp;
        IoAcquireRemoveLock(&Extension->RemoveLock, StartContext);

        IoCopyCurrentIrpStackLocationToNext(Irp);
        IoSetCompletionRoutine(Irp, MouseStartCompletion, StartContext, TRUE, TRUE, TRUE);
        IoMarkIrpPending(Irp);
        IoCallDriver(Extension->LowerDevice, Irp);
        return STATUS_PENDING;
    }

    /* No work item, start inline instead */
    MouseForwardAndWait(Extension->LowerDevice, Irp, TRUE);
    Status = MouseStartDevice(DeviceObject, Irp);
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

/**
 * @brief
 * Takes a class device out of service on surprise or regular removal.
 */
static
VOID
NTAPI
MouseTearDown(
    _In_ PMOUCLASS_EXTENSION Extension,
    _In_ BOOLEAN IsSurprise)
{
    PMOUCLASS_PORT Entry;
    PVOID Handle;

    if (IsSurprise)
    {
        Extension->IsSurpriseRemoved = TRUE;
    }
    else if (Extension->IsSurpriseRemoved)
    {
        /* Everything but the read queue went away with the surprise removal */
        goto CancelReads;
    }

    MouseCancelWaitWakeForRemoval(Extension);
    IoWMIRegistrationControl(Extension->Self, WMIREG_ACTION_DEREGISTER);

    if (Extension->IsStarted)
    {
        ExAcquireFastMutex(&MouGlobals.Lock);

        if (MouGlobals.SharedClass != NULL)
        {
            if (MouGlobals.SharedOpens <= 0)
            {
                ExReleaseFastMutex(&MouGlobals.Lock);
            }
            else
            {
                Entry = MouseGetSharedEntry(Extension);
                if (Entry != NULL)
                    Entry->Enabled = FALSE;
                ExReleaseFastMutex(&MouGlobals.Lock);

                Handle = InterlockedExchangePointer(&Extension->TargetNotifyHandle, NULL);
                if (Handle != NULL)
                    IoUnregisterPlugPlayNotification(Handle);
            }
        }
        else
        {
            ExReleaseFastMutex(&MouGlobals.Lock);
            if (Extension->InterfaceName.Buffer != NULL)
                IoSetDeviceInterfaceState(&Extension->InterfaceName, FALSE);
        }
    }

CancelReads:
    if (Extension->IsPnp)
        MouseCancelQueuedReads(Extension, NULL);
}

static
NTSTATUS
NTAPI
MouseHandleRemove(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PMOUCLASS_EXTENSION Extension = DeviceObject->DeviceExtension;
    PMOUCLASS_PORT Entry;
    NTSTATUS Status;

    MouseTearDown(Extension, FALSE);
    IoReleaseRemoveLockAndWait(&Extension->RemoveLock, Irp);

    IoSkipCurrentIrpStackLocation(Irp);
    Status = IoCallDriver(Extension->LowerDevice, Irp);

    ExAcquireFastMutex(&MouGlobals.Lock);
    if (MouGlobals.SharedClass != NULL)
    {
        if (MouGlobals.PortCount <= 1)
        {
            MouGlobals.PortCount = 0;
            if (MouGlobals.Ports != NULL)
            {
                ExFreePoolWithTag(MouGlobals.Ports, MOUCLASS_TAG);
                MouGlobals.Ports = NULL;
            }
        }
        else
        {
            Entry = MouseGetSharedEntry(Extension);
            if (Entry != NULL)
            {
                Entry->Free = TRUE;
                Entry->FileObject = NULL;
                Entry->Port = NULL;
            }
        }
    }
    ExReleaseFastMutex(&MouGlobals.Lock);

    IoDetachDevice(Extension->LowerDevice);
    RtlFreeUnicodeString(&Extension->InterfaceName);
    ExFreePoolWithTag(Extension->QueueStart, MOUCLASS_TAG);
    IoDeleteDevice(DeviceObject);

    return Status;
}

static
VOID
NTAPI
MouseHandleStop(
    _In_ PMOUCLASS_EXTENSION Extension)
{
    PMOUCLASS_PORT Entry;
    PFILE_OBJECT FileObject;
    BOOLEAN WasEnabled;
    PVOID Handle;

    if (Extension->IsStarted)
    {
        ExAcquireFastMutex(&MouGlobals.Lock);

        Entry = (MouGlobals.SharedClass != NULL) ? MouseGetSharedEntry(Extension) : NULL;
        if (Entry != NULL && MouGlobals.SharedOpens > 0)
        {
            WasEnabled = Entry->Enabled;
            Entry->Enabled = FALSE;
            FileObject = Entry->FileObject;
            ExReleaseFastMutex(&MouGlobals.Lock);

            if (WasEnabled)
            {
                Handle = InterlockedExchangePointer(&Extension->TargetNotifyHandle, NULL);
                if (Handle != NULL)
                    IoUnregisterPlugPlayNotification(Handle);

                MouseSwitchPort(FALSE, NULL, Extension, &FileObject);

                ExAcquireFastMutex(&MouGlobals.Lock);
                Entry = MouseGetSharedEntry(Extension);
                if (Entry != NULL)
                    Entry->FileObject = FileObject;
                ExReleaseFastMutex(&MouGlobals.Lock);
            }
        }
        else
        {
            ExReleaseFastMutex(&MouGlobals.Lock);
        }
    }

    Extension->IsStarted = FALSE;
}

NTSTATUS
NTAPI
MousePnp(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PMOUCLASS_EXTENSION Extension = DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS Status;

    if (!Extension->IsPnp)
    {
        Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_NOT_SUPPORTED;
    }

    Status = IoAcquireRemoveLock(&Extension->RemoveLock, Irp);
    if (!NT_SUCCESS(Status))
    {
        Irp->IoStatus.Status = Status;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return Status;
    }

    switch (Stack->MinorFunction)
    {
        case IRP_MN_START_DEVICE:
            Status = MouseHandleStart(DeviceObject, Irp);
            break;

        case IRP_MN_REMOVE_DEVICE:
            return MouseHandleRemove(DeviceObject, Irp);

        case IRP_MN_SURPRISE_REMOVAL:
            MouseTearDown(Extension, TRUE);
            IoSkipCurrentIrpStackLocation(Irp);
            Status = IoCallDriver(Extension->LowerDevice, Irp);
            break;

        case IRP_MN_STOP_DEVICE:
            MouseHandleStop(Extension);
            IoSkipCurrentIrpStackLocation(Irp);
            Status = IoCallDriver(Extension->LowerDevice, Irp);
            break;

        default:
            IoSkipCurrentIrpStackLocation(Irp);
            Status = IoCallDriver(Extension->LowerDevice, Irp);
            break;
    }

    IoReleaseRemoveLock(&Extension->RemoveLock, Irp);
    return Status;
}
