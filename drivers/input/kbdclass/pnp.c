/*
 * PROJECT:     ReactOS Keyboard Class Driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Plug and play handling
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "kbdclass.h"

#define NDEBUG
#include <debug.h>

static IO_COMPLETION_ROUTINE KbdClassStartComplete;
static IO_WORKITEM_ROUTINE KbdClassStartWorker;

/* HELPERS ********************************************************************/

/**
 * @brief Stores a port file back into the port list. Caller holds Mutex.
 */
static
VOID
KbdClassUpdatePortEntry(
    _In_ ULONG Index,
    _In_ PKBDCLASS_DEVICE_EXTENSION Port,
    _In_opt_ PFILE_OBJECT File,
    _In_ BOOLEAN Disable)
{
    PKBDCLASS_PORT_ENTRY Entry;

    if (Index >= KbdClassGlobals.PortCount)
        return;

    Entry = &KbdClassGlobals.PortList[Index];
    if (Entry->Port != Port)
        return;

    Entry->File = File;
    if (Disable)
        Entry->Enabled = FALSE;
}

BOOLEAN
NTAPI
KbdClassCancelPendingLeds(
    _In_ PKBDCLASS_DEVICE_EXTENSION DeviceExtension)
{
    PIRP Irp;

    Irp = InterlockedExchangePointer((PVOID *)&DeviceExtension->LedIrp, NULL);
    if (!Irp)
        return FALSE;

    IoCancelIrp(Irp);

    /* Whoever finishes last frees the IRP */
    if (InterlockedExchange(&DeviceExtension->LedState, KBDCLASS_LEDS_CANCELLED) ==
        KBDCLASS_LEDS_COMPLETED)
    {
        IoFreeIrp(Irp);
    }

    return TRUE;
}

/**
 * @brief Enables or disables one grand master port after a PnP event.
 */
static
NTSTATUS
KbdClassEnableGlobalPort(
    _In_ PKBDCLASS_DEVICE_EXTENSION PortExtension,
    _In_ BOOLEAN Enable)
{
    PKBDCLASS_PORT_ENTRY Entry;
    BOOLEAN WasEnabled;
    PFILE_OBJECT File;
    ULONG Index;
    NTSTATUS Status;

    ExAcquireFastMutex(&KbdClassGlobals.Mutex);

    if (KbdClassGlobals.GrandMasterOpens <= 0)
    {
        ExReleaseFastMutex(&KbdClassGlobals.Mutex);
        return STATUS_SUCCESS;
    }

    for (Index = 0; Index < KbdClassGlobals.PortCount; Index++)
    {
        Entry = &KbdClassGlobals.PortList[Index];
        if (!Entry->Free && Entry->Port == PortExtension)
            break;
    }

    if (Index == KbdClassGlobals.PortCount)
    {
        ExReleaseFastMutex(&KbdClassGlobals.Mutex);
        return STATUS_NO_SUCH_DEVICE;
    }

    Entry = &KbdClassGlobals.PortList[Index];
    WasEnabled = Entry->Enabled;
    Entry->Enabled = Enable;
    File = Entry->File;
    ExReleaseFastMutex(&KbdClassGlobals.Mutex);

    if (WasEnabled == Enable)
        return STATUS_SUCCESS;

    Status = KbdClassEnableDisablePort(Enable, NULL, PortExtension, &File);

    ExAcquireFastMutex(&KbdClassGlobals.Mutex);
    KbdClassUpdatePortEntry(Index, PortExtension, File, FALSE);
    ExReleaseFastMutex(&KbdClassGlobals.Mutex);

    return Status;
}

/**
 * @brief Target device notification for an opened grand master port.
 */
NTSTATUS
NTAPI
KbdClassPlugPlayNotification(
    _In_ PVOID NotificationStructure,
    _Inout_opt_ PVOID Context)
{
    PTARGET_DEVICE_REMOVAL_NOTIFICATION Notification = NotificationStructure;
    PKBDCLASS_DEVICE_EXTENSION PortExtension = Context;
    PVOID Notify;

    if (IsEqualGUID(&Notification->Event, &GUID_TARGET_DEVICE_QUERY_REMOVE))
        return KbdClassEnableGlobalPort(PortExtension, FALSE);

    if (IsEqualGUID(&Notification->Event, &GUID_TARGET_DEVICE_REMOVE_COMPLETE))
    {
        Notify = InterlockedExchangePointer(&PortExtension->TargetNotifyHandle, NULL);
        if (!Notify)
            return STATUS_SUCCESS;

        IoUnregisterPlugPlayNotification(Notify);
        return KbdClassEnableGlobalPort(PortExtension, FALSE);
    }

    if (IsEqualGUID(&Notification->Event, &GUID_TARGET_DEVICE_REMOVE_CANCELLED))
    {
        Notify = InterlockedExchangePointer(&PortExtension->TargetNotifyHandle, NULL);
        if (Notify)
        {
            IoUnregisterPlugPlayNotification(Notify);
            return KbdClassEnableGlobalPort(PortExtension, TRUE);
        }
    }

    return STATUS_SUCCESS;
}

/* START **********************************************************************/

static
VOID
KbdClassQueryCapabilities(
    _In_ PKBDCLASS_DEVICE_EXTENSION DeviceExtension,
    _In_ PIRP Irp)
{
    DEVICE_CAPABILITIES Capabilities;
    PIO_STACK_LOCATION Next;

    RtlZeroMemory(&Capabilities, sizeof(Capabilities));
    Capabilities.Size = sizeof(Capabilities);
    Capabilities.Version = 1;
    Capabilities.Address = MAXULONG;
    Capabilities.UINumber = MAXULONG;

    Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
    Irp->IoStatus.Information = 0;

    Next = IoGetNextIrpStackLocation(Irp);
    RtlZeroMemory(Next, sizeof(*Next));
    Next->MajorFunction = IRP_MJ_PNP;
    Next->MinorFunction = IRP_MN_QUERY_CAPABILITIES;
    Next->Parameters.DeviceCapabilities.Capabilities = &Capabilities;

    if (!NT_SUCCESS(KbdClassSendIrpSynchronously(DeviceExtension->TopPort, Irp, FALSE)))
        return;

    RtlCopyMemory(DeviceExtension->SystemToDevice,
                  Capabilities.DeviceState,
                  sizeof(DeviceExtension->SystemToDevice));
    DeviceExtension->MinDeviceWake = Capabilities.DeviceWake;
    DeviceExtension->MinSystemWake = Capabilities.SystemWake;
}

static
VOID
KbdClassSendGrandMasterLeds(
    _In_ PKBDCLASS_DEVICE_EXTENSION DeviceExtension,
    _In_ PIRP Irp,
    _In_opt_ PFILE_OBJECT File)
{
    PIO_STACK_LOCATION Next;
    ULONG_PTR SavedInformation;
    NTSTATUS SavedStatus;
    PVOID SavedBuffer;

    SavedStatus = Irp->IoStatus.Status;
    SavedInformation = Irp->IoStatus.Information;
    SavedBuffer = Irp->AssociatedIrp.SystemBuffer;

    Next = IoGetNextIrpStackLocation(Irp);
    RtlZeroMemory(Next, sizeof(*Next));
    Next->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
    Next->Parameters.DeviceIoControl.OutputBufferLength = 0;
    Next->Parameters.DeviceIoControl.InputBufferLength = sizeof(KEYBOARD_INDICATOR_PARAMETERS);
    Next->Parameters.DeviceIoControl.IoControlCode = IOCTL_KEYBOARD_SET_INDICATORS;
    Next->FileObject = File;

    Irp->IoStatus.Information = 0;
    Irp->AssociatedIrp.SystemBuffer = &KbdClassGlobals.GrandMaster->Indicators;
    KbdClassSendIrpSynchronously(DeviceExtension->TopPort, Irp, FALSE);

    Irp->IoStatus.Status = SavedStatus;
    Irp->IoStatus.Information = SavedInformation;
    Irp->AssociatedIrp.SystemBuffer = SavedBuffer;
}

/**
 * @brief Finishes IRP_MN_START_DEVICE once the lower stack has started.
 *
 * A one to one device enables its keyboard interface here. A grand master
 * port is opened right away when the shared device already has handles.
 */
static
NTSTATUS
KbdClassStartDevice(
    _In_ PKBDCLASS_DEVICE_EXTENSION DeviceExtension,
    _In_ PIRP Irp)
{
    NTSTATUS LowerStatus = Irp->IoStatus.Status;
    ULONG_PTR LowerInformation = Irp->IoStatus.Information;
    PKBDCLASS_PORT_ENTRY Entry;
    NTSTATUS Status = STATUS_SUCCESS;
    BOOLEAN WakeCapable;
    BOOLEAN WasEnabled;
    PFILE_OBJECT File;
    ULONG UnitId;

    if (!NT_SUCCESS(LowerStatus))
        return LowerStatus;

    KbdClassQueryCapabilities(DeviceExtension, Irp);
    Irp->IoStatus.Status = LowerStatus;
    Irp->IoStatus.Information = LowerInformation;

    DeviceExtension->Started = TRUE;

    WakeCapable = KbdClassWakeCapable(DeviceExtension);
    if (WakeCapable)
        KbdClassReadWaitWakeState(DeviceExtension);

    KbdClassSetupWmi(DeviceExtension, WakeCapable);

    ExAcquireFastMutex(&KbdClassGlobals.Mutex);

    if (!KbdClassGlobals.GrandMaster)
    {
        ExReleaseFastMutex(&KbdClassGlobals.Mutex);
        Status = IoSetDeviceInterfaceState(&DeviceExtension->InterfaceName, TRUE);
    }
    else if (KbdClassGlobals.GrandMasterOpens > 0 && DeviceExtension->InPortList)
    {
        UnitId = DeviceExtension->UnitId;
        Entry = &KbdClassGlobals.PortList[UnitId];
        WasEnabled = Entry->Enabled;
        Entry->Enabled = TRUE;
        File = Entry->File;
        ExReleaseFastMutex(&KbdClassGlobals.Mutex);

        if (!WasEnabled)
        {
            Status = KbdClassEnableDisablePort(TRUE, Irp, DeviceExtension, &File);

            ExAcquireFastMutex(&KbdClassGlobals.Mutex);
            KbdClassUpdatePortEntry(UnitId, DeviceExtension, File, !NT_SUCCESS(Status));
            ExReleaseFastMutex(&KbdClassGlobals.Mutex);

            if (NT_SUCCESS(Status))
            {
                KbdClassSendGrandMasterLeds(DeviceExtension, Irp, File);
                Status = STATUS_SUCCESS;
            }
        }
    }
    else
    {
        ExReleaseFastMutex(&KbdClassGlobals.Mutex);
    }

    if (WakeCapable &&
        !KbdClassWaitWakePending(DeviceExtension) &&
        KbdClassCheckWaitWakeEnabled(DeviceExtension))
    {
        KbdClassCreateWaitWakeIrp(DeviceExtension);
    }

    return Status;
}

static
NTSTATUS
NTAPI
KbdClassStartComplete(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_reads_opt_(_Inexpressible_("varies")) PVOID Context)
{
    PKBDCLASS_WORK_CONTEXT WorkContext = Context;

    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);

    IoQueueWorkItem(WorkContext->WorkItem, KbdClassStartWorker, DelayedWorkQueue, WorkContext);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

static
VOID
NTAPI
KbdClassStartWorker(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_opt_ PVOID Context)
{
    PKBDCLASS_WORK_CONTEXT WorkContext = Context;
    PKBDCLASS_DEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
    PIRP Irp = WorkContext->Irp;

    Irp->IoStatus.Status = KbdClassStartDevice(DeviceExtension, Irp);
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    IoFreeWorkItem(WorkContext->WorkItem);
    ExFreePoolWithTag(WorkContext, KBDCLASS_TAG);
    IoReleaseRemoveLock(&DeviceExtension->RemoveLock, WorkContext);
}

static
NTSTATUS
KbdClassPnpStart(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp)
{
    PKBDCLASS_DEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
    PKBDCLASS_WORK_CONTEXT WorkContext;
    NTSTATUS Status;

    WorkContext = ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*WorkContext), KBDCLASS_TAG);
    if (WorkContext)
    {
        WorkContext->WorkItem = IoAllocateWorkItem(DeviceObject);
        if (!WorkContext->WorkItem)
        {
            ExFreePoolWithTag(WorkContext, KBDCLASS_TAG);
            WorkContext = NULL;
        }
    }

    if (WorkContext)
    {
        /* The rest of the start runs at passive level once the stack is up */
        WorkContext->DeviceExtension = DeviceExtension;
        WorkContext->Irp = Irp;
        IoAcquireRemoveLock(&DeviceExtension->RemoveLock, WorkContext);

        IoCopyCurrentIrpStackLocationToNext(Irp);
        IoSetCompletionRoutine(Irp, KbdClassStartComplete, WorkContext, TRUE, TRUE, TRUE);
        IoMarkIrpPending(Irp);
        IoCallDriver(DeviceExtension->TopPort, Irp);
        return STATUS_PENDING;
    }

    KbdClassSendIrpSynchronously(DeviceExtension->TopPort, Irp, TRUE);
    Status = KbdClassStartDevice(DeviceExtension, Irp);

    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

/* STOP AND REMOVE ************************************************************/

static
VOID
KbdClassPnpStop(
    _In_ PKBDCLASS_DEVICE_EXTENSION DeviceExtension,
    _In_ PIRP Irp)
{
    PKBDCLASS_PORT_ENTRY Entry;
    BOOLEAN WasEnabled;
    PFILE_OBJECT File;
    PVOID Notify;
    ULONG UnitId;

    if (!DeviceExtension->Started)
        return;

    ExAcquireFastMutex(&KbdClassGlobals.Mutex);

    if (!KbdClassGlobals.GrandMaster ||
        KbdClassGlobals.GrandMasterOpens <= 0 ||
        !DeviceExtension->InPortList)
    {
        ExReleaseFastMutex(&KbdClassGlobals.Mutex);
        return;
    }

    UnitId = DeviceExtension->UnitId;
    Entry = &KbdClassGlobals.PortList[UnitId];
    WasEnabled = Entry->Enabled;
    Entry->Enabled = FALSE;
    File = Entry->File;
    ExReleaseFastMutex(&KbdClassGlobals.Mutex);

    if (!WasEnabled)
        return;

    Notify = InterlockedExchangePointer(&DeviceExtension->TargetNotifyHandle, NULL);
    if (Notify)
        IoUnregisterPlugPlayNotification(Notify);

    KbdClassEnableDisablePort(FALSE, Irp, DeviceExtension, &File);

    ExAcquireFastMutex(&KbdClassGlobals.Mutex);
    KbdClassUpdatePortEntry(UnitId, DeviceExtension, File, FALSE);
    ExReleaseFastMutex(&KbdClassGlobals.Mutex);
}

/**
 * @brief Cancels wait wake, unregisters WMI and the interface, flushes reads.
 */
static
VOID
KbdClassRemoveDevice(
    _In_ PKBDCLASS_DEVICE_EXTENSION DeviceExtension)
{
    PVOID Notify;
    KIRQL OldIrql;
    PIRP Irp;

    if (InterlockedExchange(&DeviceExtension->WaitWakeRemoveState, KBDCLASS_WW_REMOVE_CANCELLING) ==
        KBDCLASS_WW_ARMED)
    {
        KeAcquireSpinLock(&DeviceExtension->WaitWakeLock, &OldIrql);
        Irp = DeviceExtension->WaitWakeIrp;

        if (!Irp || DeviceExtension->WaitWakeCancelled)
        {
            KeReleaseSpinLock(&DeviceExtension->WaitWakeLock, OldIrql);
            InterlockedExchange(&DeviceExtension->WaitWakeRemoveState, KBDCLASS_WW_REMOVE_DONE);
        }
        else
        {
            DeviceExtension->WaitWakeCancelled = TRUE;
            KeReleaseSpinLock(&DeviceExtension->WaitWakeLock, OldIrql);

            IoCancelIrp(Irp);

            /* The completion routine left the IRP to us */
            if (InterlockedExchange(&DeviceExtension->WaitWakeRemoveState, KBDCLASS_WW_REMOVE_DONE) ==
                KBDCLASS_WW_COMPLETED)
            {
                IoCompleteRequest(Irp, IO_NO_INCREMENT);
            }
        }
    }

    IoWMIRegistrationControl(DeviceExtension->Self, WMIREG_ACTION_DEREGISTER);

    if (DeviceExtension->Started)
    {
        ExAcquireFastMutex(&KbdClassGlobals.Mutex);

        if (KbdClassGlobals.GrandMaster)
        {
            if (KbdClassGlobals.GrandMasterOpens > 0 && DeviceExtension->InPortList)
            {
                KbdClassGlobals.PortList[DeviceExtension->UnitId].Enabled = FALSE;
                ExReleaseFastMutex(&KbdClassGlobals.Mutex);

                Notify = InterlockedExchangePointer(&DeviceExtension->TargetNotifyHandle, NULL);
                if (Notify)
                    IoUnregisterPlugPlayNotification(Notify);
            }
            else
            {
                ExReleaseFastMutex(&KbdClassGlobals.Mutex);
            }
        }
        else
        {
            ExReleaseFastMutex(&KbdClassGlobals.Mutex);

            if (!DeviceExtension->SurpriseRemoved && DeviceExtension->InterfaceName.Buffer)
                IoSetDeviceInterfaceState(&DeviceExtension->InterfaceName, FALSE);
        }
    }

    if (DeviceExtension->PnP)
        KbdClassFlushReads(DeviceExtension, NULL);
}

static
NTSTATUS
KbdClassPnpRemove(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp)
{
    PKBDCLASS_DEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
    PKBDCLASS_PORT_ENTRY Entry;
    NTSTATUS Status;

    KbdClassCancelPendingLeds(DeviceExtension);
    KbdClassRemoveDevice(DeviceExtension);
    DeviceExtension->Removed = TRUE;

    IoReleaseRemoveLockAndWait(&DeviceExtension->RemoveLock, Irp);

    IoCopyCurrentIrpStackLocationToNext(Irp);
    Status = IoCallDriver(DeviceExtension->TopPort, Irp);

    ExAcquireFastMutex(&KbdClassGlobals.Mutex);

    if (KbdClassGlobals.GrandMaster && DeviceExtension->InPortList)
    {
        if (KbdClassGlobals.PortCount <= 1)
        {
            KbdClassGlobals.PortCount = 0;
            ExFreePoolWithTag(KbdClassGlobals.PortList, KBDCLASS_TAG);
            KbdClassGlobals.PortList = NULL;
        }
        else
        {
            Entry = &KbdClassGlobals.PortList[DeviceExtension->UnitId];
            Entry->File = NULL;
            Entry->Port = NULL;
            Entry->Free = TRUE;
        }

        DeviceExtension->InPortList = FALSE;
    }

    ExReleaseFastMutex(&KbdClassGlobals.Mutex);

    IoDetachDevice(DeviceExtension->TopPort);
    RtlFreeUnicodeString(&DeviceExtension->InterfaceName);

    ExFreePoolWithTag(DeviceExtension->QueueBase, KBDCLASS_TAG);
    DeviceExtension->QueueBase = NULL;

    IoDeleteDevice(DeviceObject);
    return Status;
}

/* DISPATCH *******************************************************************/

NTSTATUS
NTAPI
KbdClassPnp(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PKBDCLASS_DEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS Status;

    /* Only devices attached to a PnP stack take part in PnP */
    if (!DeviceExtension->PnP)
    {
        Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_NOT_SUPPORTED;
    }

    Status = IoAcquireRemoveLock(&DeviceExtension->RemoveLock, Irp);
    if (!NT_SUCCESS(Status))
    {
        Irp->IoStatus.Status = Status;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return Status;
    }

    switch (Stack->MinorFunction)
    {
        case IRP_MN_REMOVE_DEVICE:
            return KbdClassPnpRemove(DeviceObject, Irp);

        case IRP_MN_START_DEVICE:
            Status = KbdClassPnpStart(DeviceObject, Irp);
            break;

        case IRP_MN_STOP_DEVICE:
            KbdClassPnpStop(DeviceExtension, Irp);
            DeviceExtension->Started = FALSE;
            IoSkipCurrentIrpStackLocation(Irp);
            Status = IoCallDriver(DeviceExtension->TopPort, Irp);
            break;

        case IRP_MN_QUERY_PNP_DEVICE_STATE:
            if (!DeviceExtension->AllowDisable)
            {
                Irp->IoStatus.Information |= PNP_DEVICE_NOT_DISABLEABLE;
                Irp->IoStatus.Status = STATUS_SUCCESS;
            }

            IoCopyCurrentIrpStackLocationToNext(Irp);
            Status = IoCallDriver(DeviceExtension->TopPort, Irp);
            break;

        case IRP_MN_SURPRISE_REMOVAL:
            KbdClassCancelPendingLeds(DeviceExtension);
            DeviceExtension->SurpriseRemoved = TRUE;

            if (DeviceExtension->InterfaceName.Buffer)
                IoSetDeviceInterfaceState(&DeviceExtension->InterfaceName, FALSE);

            IoSkipCurrentIrpStackLocation(Irp);
            Status = IoCallDriver(DeviceExtension->TopPort, Irp);
            break;

        default:
            IoCopyCurrentIrpStackLocationToNext(Irp);
            Status = IoCallDriver(DeviceExtension->TopPort, Irp);
            break;
    }

    IoReleaseRemoveLock(&DeviceExtension->RemoveLock, Irp);
    return Status;
}
