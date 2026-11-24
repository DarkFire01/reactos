/*
 * PROJECT:     ReactOS HID Class Driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     System and device power, selective suspend and wake
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "precomp.h"

#define NDEBUG
#include <debug.h>

/* Idle timeouts in milliseconds when the device key says nothing */
#define HIDCLASS_DEFAULT_SS_TIMEOUT             5000
#define HIDCLASS_DEFAULT_NO_HANDLE_TIMEOUT      1000
#define HIDCLASS_DEFAULT_NO_HANDLE_INITIAL      5000

static IO_COMPLETION_ROUTINE HidClassFdoPowerCompletion;
static IO_COMPLETION_ROUTINE HidClassIdleIrpCompletion;
static DRIVER_CANCEL HidClassCancelDelayedIrp;
static DRIVER_CANCEL HidClassCancelCollectionWaitWake;
static REQUEST_POWER_COMPLETE HidClassSystemDxComplete;
static REQUEST_POWER_COMPLETE HidClassSignalPowerComplete;
static REQUEST_POWER_COMPLETE HidClassIgnorePowerComplete;
static REQUEST_POWER_COMPLETE HidClassFdoWaitWakeComplete;
static REQUEST_POWER_COMPLETE HidClassCollectionPowerComplete;
static REQUEST_POWER_COMPLETE HidClassRemoteWakeComplete;
static REQUEST_POWER_COMPLETE HidClassIdleDxComplete;
static IO_WORKITEM_ROUTINE HidClassPassIoWorker;
static IO_WORKITEM_ROUTINE HidClassIdleOutWorker;
static IO_WORKITEM_ROUTINE HidClassPowerUpWorker;
static IO_WORKITEM_ROUTINE HidClassSelectiveSuspendWorker;
static IO_WORKITEM_ROUTINE HidClassRemoteWakeWorker;

typedef struct _HIDCLASS_WORK_CONTEXT
{
    PIO_WORKITEM WorkItem;
    PVOID Context;
    BOOLEAN Enable;
} HIDCLASS_WORK_CONTEXT, *PHIDCLASS_WORK_CONTEXT;

static
BOOLEAN
NTAPI
HidClassQueueWork(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIO_WORKITEM_ROUTINE Routine,
    _In_opt_ PVOID Context,
    _In_ BOOLEAN Enable)
{
    PHIDCLASS_WORK_CONTEXT Work;

    Work = ExAllocatePoolZero(NonPagedPool, sizeof(*Work), HIDCLASS_TAG);
    if (!Work)
        return FALSE;

    Work->WorkItem = IoAllocateWorkItem(DeviceObject);
    if (!Work->WorkItem)
    {
        ExFreePoolWithTag(Work, HIDCLASS_TAG);
        return FALSE;
    }

    Work->Context = Context;
    Work->Enable = Enable;
    IoQueueWorkItem(Work->WorkItem, Routine, DelayedWorkQueue, Work);
    return TRUE;
}

static
VOID
NTAPI
HidClassFreeWork(
    _In_ PHIDCLASS_WORK_CONTEXT Work)
{
    IoFreeWorkItem(Work->WorkItem);
    ExFreePoolWithTag(Work, HIDCLASS_TAG);
}

static
VOID
NTAPI
HidClassAcquirePowerReference(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo)
{
    if (Fdo->PoFxHandle)
        PoFxActivateComponent(Fdo->PoFxHandle, 0, PO_FX_FLAG_ASYNC_ONLY);
}

static
VOID
NTAPI
HidClassReleasePowerReference(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo)
{
    if (Fdo->PoFxHandle)
        PoFxIdleComponent(Fdo->PoFxHandle, 0, PO_FX_FLAG_ASYNC_ONLY);
}

/**
 * @brief
 * Counts as device activity, so the idle timeout starts over.
 */
VOID
NTAPI
HidClassResetIdleTimer(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo)
{
    HidClassAcquirePowerReference(Fdo);
    HidClassReleasePowerReference(Fdo);
}

static
PIRP
NTAPI
HidClassDequeueDelayedIrp(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo)
{
    PIRP Found = NULL;
    KIRQL OldIrql;

    KeAcquireSpinLock(&Fdo->IoLock, &OldIrql);
    while (!IsListEmpty(&Fdo->DelayedIrps))
    {
        PLIST_ENTRY Entry = RemoveHeadList(&Fdo->DelayedIrps);
        PIRP Irp = CONTAINING_RECORD(Entry, IRP, Tail.Overlay.ListEntry);

        if (IoSetCancelRoutine(Irp, NULL))
        {
            Found = Irp;
            break;
        }
        InitializeListHead(Entry);
    }
    KeReleaseSpinLock(&Fdo->IoLock, OldIrql);

    return Found;
}

/**
 * @brief
 * Switches how requests are handled. Held requests are sent again when the
 * device passes I/O and failed when it goes away.
 */
VOID
NTAPI
HidClassSetIoState(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo,
    _In_ HIDCLASS_IO_STATE IoState)
{
    KIRQL OldIrql;
    PIRP Irp;

    KeAcquireSpinLock(&Fdo->IoLock, &OldIrql);
    Fdo->IoState = IoState;
    KeReleaseSpinLock(&Fdo->IoLock, OldIrql);

    if (IoState == HidClassIoQueue)
        return;

    while ((Irp = HidClassDequeueDelayedIrp(Fdo)) != NULL)
    {
        if (IoState == HidClassIoPass)
        {
            PHIDCLASS_DEVICE_EXTENSION Owner = Irp->Tail.Overlay.DriverContext[0];
            PDEVICE_OBJECT DeviceObject = Owner->Pdo.DeviceObject;

            DeviceObject->DriverObject->MajorFunction[IoGetCurrentIrpStackLocation(Irp)->MajorFunction](DeviceObject, Irp);
        }
        else
        {
            Irp->IoStatus.Status = STATUS_NO_SUCH_DEVICE;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
        }

        HidClassReleasePowerReference(Fdo);
    }
}

static
VOID
NTAPI
HidClassCancelDelayedIrp(
    _Inout_ PDEVICE_OBJECT DeviceObject,
    _Inout_ _IRQL_uses_cancel_ PIRP Irp)
{
    PHIDCLASS_FDO_EXTENSION Fdo = HidClassGetFdo(DeviceObject->DeviceExtension);
    KIRQL OldIrql;

    IoReleaseCancelSpinLock(Irp->CancelIrql);

    KeAcquireSpinLock(&Fdo->IoLock, &OldIrql);
    RemoveEntryList(&Irp->Tail.Overlay.ListEntry);
    KeReleaseSpinLock(&Fdo->IoLock, OldIrql);

    Irp->IoStatus.Status = STATUS_CANCELLED;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    HidClassReleasePowerReference(Fdo);
}

/**
 * @brief
 * Starts a request that needs the device powered. It runs now, waits for
 * the device to power up or fails when the device is gone.
 *
 * @param[out] Counted
 * TRUE when the caller has to call HidClassEndIo once the request is done.
 *
 * @return
 * STATUS_SUCCESS to go ahead, STATUS_PENDING when the request was held.
 */
NTSTATUS
NTAPI
HidClassBeginIo(
    _In_ PHIDCLASS_DEVICE_EXTENSION DeviceExtension,
    _In_ PIRP Irp,
    _Out_ PBOOLEAN Counted)
{
    PHIDCLASS_FDO_EXTENSION Fdo = HidClassGetFdo(DeviceExtension);
    NTSTATUS Status;
    KIRQL OldIrql;

    HidClassAcquirePowerReference(Fdo);
    *Counted = TRUE;

    KeAcquireSpinLock(&Fdo->IoLock, &OldIrql);
    switch (Fdo->IoState)
    {
        case HidClassIoPass:
            Fdo->InFlightCount++;
            KeReleaseSpinLock(&Fdo->IoLock, OldIrql);
            return STATUS_SUCCESS;

        case HidClassIoQueue:
            Irp->Tail.Overlay.DriverContext[0] = DeviceExtension;
            IoSetCancelRoutine(Irp, HidClassCancelDelayedIrp);
            if (Irp->Cancel)
            {
                if (IoSetCancelRoutine(Irp, NULL))
                {
                    Status = STATUS_CANCELLED;
                    break;
                }
                InitializeListHead(&Irp->Tail.Overlay.ListEntry);
            }
            else
            {
                InsertTailList(&Fdo->DelayedIrps, &Irp->Tail.Overlay.ListEntry);
            }
            IoMarkIrpPending(Irp);
            KeReleaseSpinLock(&Fdo->IoLock, OldIrql);
            return STATUS_PENDING;

        default:
            Status = STATUS_INVALID_PARAMETER;
            break;
    }
    KeReleaseSpinLock(&Fdo->IoLock, OldIrql);

    HidClassReleasePowerReference(Fdo);
    *Counted = FALSE;
    return Status;
}

VOID
NTAPI
HidClassEndIo(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo)
{
    KIRQL OldIrql;

    KeAcquireSpinLock(&Fdo->IoLock, &OldIrql);
    if (--Fdo->InFlightCount == 0 && Fdo->DrainWaiting)
    {
        Fdo->DrainWaiting = FALSE;
        KeSetEvent(&Fdo->IoDrained, IO_NO_INCREMENT, FALSE);
    }
    KeReleaseSpinLock(&Fdo->IoLock, OldIrql);

    HidClassReleasePowerReference(Fdo);
}

static
VOID
NTAPI
HidClassWaitForIoDrain(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo)
{
    BOOLEAN Wait = FALSE;
    KIRQL OldIrql;

    KeAcquireSpinLock(&Fdo->IoLock, &OldIrql);
    if (Fdo->InFlightCount > 0)
    {
        Fdo->DrainWaiting = TRUE;
        KeClearEvent(&Fdo->IoDrained);
        Wait = TRUE;
    }
    KeReleaseSpinLock(&Fdo->IoLock, OldIrql);

    if (Wait)
        KeWaitForSingleObject(&Fdo->IoDrained, Executive, KernelMode, FALSE, NULL);
}

/**
 * @brief
 * Fails the held requests of one collection, used when its PDO goes away.
 */
VOID
NTAPI
HidClassCompleteDelayedIrpsForPdo(
    _In_ PHIDCLASS_PDO_EXTENSION Pdo,
    _In_ NTSTATUS Status)
{
    PHIDCLASS_FDO_EXTENSION Fdo = Pdo->Fdo;
    PVOID Owner = HidClassExtensionFromPdo(Pdo);
    LIST_ENTRY Taken;
    PLIST_ENTRY Entry;
    KIRQL OldIrql;

    InitializeListHead(&Taken);

    KeAcquireSpinLock(&Fdo->IoLock, &OldIrql);
    Entry = Fdo->DelayedIrps.Flink;
    while (Entry != &Fdo->DelayedIrps)
    {
        PIRP Irp = CONTAINING_RECORD(Entry, IRP, Tail.Overlay.ListEntry);

        Entry = Entry->Flink;
        if (Irp->Tail.Overlay.DriverContext[0] != Owner)
            continue;

        RemoveEntryList(&Irp->Tail.Overlay.ListEntry);
        if (IoSetCancelRoutine(Irp, NULL))
            InsertTailList(&Taken, &Irp->Tail.Overlay.ListEntry);
        else
            InitializeListHead(&Irp->Tail.Overlay.ListEntry);
    }
    KeReleaseSpinLock(&Fdo->IoLock, OldIrql);

    while (!IsListEmpty(&Taken))
    {
        PIRP Irp = CONTAINING_RECORD(RemoveHeadList(&Taken), IRP, Tail.Overlay.ListEntry);

        Irp->IoStatus.Status = Status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        HidClassReleasePowerReference(Fdo);
    }
}

static
VOID
NTAPI
HidClassPassIoWorker(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_opt_ PVOID Context)
{
    PHIDCLASS_WORK_CONTEXT Work = Context;

    UNREFERENCED_PARAMETER(DeviceObject);

    HidClassSetIoState(Work->Context, HidClassIoPass);
    HidClassFreeWork(Work);
}

/**
 * @brief
 * The device is in D0 again. Reads start over and held requests go out.
 */
static
VOID
NTAPI
HidClassDevicePoweredUp(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo)
{
    Fdo->DevicePowerState = PowerDeviceD0;

    if (!Fdo->DriverExtension->DevicesArePolled && !Fdo->NoInputCollections)
        HidClassStartReads(Fdo);

    if (!Fdo->PowerHeldIo)
        return;

    Fdo->PowerHeldIo = FALSE;

    /* Held requests may need passive level, a set or get report does */
    if (KeGetCurrentIrql() == PASSIVE_LEVEL)
        HidClassSetIoState(Fdo, HidClassIoPass);
    else if (!HidClassQueueWork(Fdo->DeviceObject, HidClassPassIoWorker, Fdo, FALSE))
        HidClassSetIoState(Fdo, HidClassIoPass);
}

static
VOID
NTAPI
HidClassIgnorePowerComplete(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ UCHAR MinorFunction,
    _In_ POWER_STATE PowerState,
    _In_opt_ PVOID Context,
    _In_ PIO_STATUS_BLOCK IoStatus)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(MinorFunction);
    UNREFERENCED_PARAMETER(PowerState);
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(IoStatus);
}

static
VOID
NTAPI
HidClassSignalPowerComplete(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ UCHAR MinorFunction,
    _In_ POWER_STATE PowerState,
    _In_opt_ PVOID Context,
    _In_ PIO_STATUS_BLOCK IoStatus)
{
    PHIDCLASS_FDO_EXTENSION Fdo = Context;

    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(MinorFunction);
    UNREFERENCED_PARAMETER(PowerState);

    Fdo->PowerStatus = IoStatus->Status;
    KeSetEvent(&Fdo->PowerDone, IO_NO_INCREMENT, FALSE);
}

static
VOID
NTAPI
HidClassSystemDxComplete(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ UCHAR MinorFunction,
    _In_ POWER_STATE PowerState,
    _In_opt_ PVOID Context,
    _In_ PIO_STATUS_BLOCK IoStatus)
{
    PHIDCLASS_FDO_EXTENSION Fdo = Context;
    PIRP SystemIrp = Fdo->PendingSystemIrp;

    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(MinorFunction);
    UNREFERENCED_PARAMETER(PowerState);
    UNREFERENCED_PARAMETER(IoStatus);

    Fdo->PendingSystemIrp = NULL;
    if (!SystemIrp)
        return;

    PoStartNextPowerIrp(SystemIrp);
    IoCompleteRequest(SystemIrp, IO_NO_INCREMENT);
}

static
VOID
NTAPI
HidClassFdoWaitWakeComplete(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ UCHAR MinorFunction,
    _In_ POWER_STATE PowerState,
    _In_opt_ PVOID Context,
    _In_ PIO_STATUS_BLOCK IoStatus)
{
    PHIDCLASS_FDO_EXTENSION Fdo = Context;

    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(MinorFunction);
    UNREFERENCED_PARAMETER(PowerState);

    Fdo->WaitWakeIrp = NULL;

    if (!NT_SUCCESS(IoStatus->Status))
        return;

    HidClassCompleteCollectionWaitWakes(Fdo, IoStatus->Status);

    /* Input woke a selectively suspended device, bring it back */
    if (Fdo->IdleState == HidClassIdlePoweredDown)
        HidClassResetIdleTimer(Fdo);
}

static
VOID
NTAPI
HidClassArmForWake(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo)
{
    POWER_STATE State;
    NTSTATUS Status;

    if (Fdo->WaitWakeIrp)
        return;

    State.SystemState = Fdo->Capabilities.SystemWake;
    Status = PoRequestPowerIrp(Fdo->DeviceObject,
                               IRP_MN_WAIT_WAKE,
                               State,
                               HidClassFdoWaitWakeComplete,
                               Fdo,
                               &Fdo->WaitWakeIrp);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("HIDCLASS: wait wake request failed %lx\n", Status);
        Fdo->WaitWakeIrp = NULL;
    }
}

/**
 * @brief
 * The system left S0. Pick the device state for it and arm the device when
 * a collection waits for wake.
 */
static
VOID
NTAPI
HidClassEnterSystemSleep(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo,
    _In_ PIRP SystemIrp)
{
    SYSTEM_POWER_STATE SystemState = IoGetCurrentIrpStackLocation(SystemIrp)->Parameters.Power.State.SystemState;
    BOOLEAN Armed;
    POWER_STATE State;
    KIRQL OldIrql;

    KeAcquireSpinLock(&Fdo->WaitWakeLock, &OldIrql);
    Armed = !IsListEmpty(&Fdo->CollectionWaitWakeIrps);
    KeReleaseSpinLock(&Fdo->WaitWakeLock, OldIrql);

    State.DeviceState = PowerDeviceD3;
    if (Armed && SystemState < PowerSystemMaximum)
    {
        State.DeviceState = Fdo->Capabilities.DeviceState[SystemState];
        if (State.DeviceState == PowerDeviceUnspecified)
            State.DeviceState = PowerDeviceD3;
        else
            HidClassArmForWake(Fdo);
    }

    Fdo->PendingSystemIrp = SystemIrp;
    if (!NT_SUCCESS(PoRequestPowerIrp(Fdo->DeviceObject, IRP_MN_SET_POWER, State, HidClassSystemDxComplete, Fdo, NULL)))
    {
        Fdo->PendingSystemIrp = NULL;
        PoStartNextPowerIrp(SystemIrp);
        IoCompleteRequest(SystemIrp, IO_NO_INCREMENT);
    }
}

static
NTSTATUS
NTAPI
HidClassFdoPowerCompletion(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_opt_ PVOID Context)
{
    PHIDCLASS_FDO_EXTENSION Fdo = Context;
    PIO_STACK_LOCATION IoStack = IoGetCurrentIrpStackLocation(Irp);
    POWER_STATE State;

    UNREFERENCED_PARAMETER(DeviceObject);

    if (Irp->PendingReturned)
        IoMarkIrpPending(Irp);

    if (IoStack->MinorFunction == IRP_MN_WAIT_WAKE)
    {
        if (NT_SUCCESS(Irp->IoStatus.Status) && PoGetSystemWake(Irp))
            Fdo->WokeSystem = TRUE;
        return STATUS_CONTINUE_COMPLETION;
    }

    if (IoStack->MinorFunction != IRP_MN_SET_POWER)
        return STATUS_CONTINUE_COMPLETION;

    if (IoStack->Parameters.Power.Type == DevicePowerState)
    {
        if (IoStack->Parameters.Power.State.DeviceState == PowerDeviceD0)
        {
            if (NT_SUCCESS(Irp->IoStatus.Status))
                HidClassDevicePoweredUp(Fdo);
            IoReleaseRemoveLock(&Fdo->RemoveLock, Irp);
        }
        return STATUS_CONTINUE_COMPLETION;
    }

    if (!NT_SUCCESS(Irp->IoStatus.Status))
        return STATUS_CONTINUE_COMPLETION;

    Fdo->SystemPowerState = IoStack->Parameters.Power.State.SystemState;

    if (Fdo->SystemPowerState == PowerSystemWorking)
    {
        /* Resume does not wait for the device */
        State.DeviceState = PowerDeviceD0;
        PoRequestPowerIrp(Fdo->DeviceObject, IRP_MN_SET_POWER, State, HidClassIgnorePowerComplete, NULL, NULL);
        PoStartNextPowerIrp(Irp);
        return STATUS_CONTINUE_COMPLETION;
    }

    HidClassEnterSystemSleep(Fdo, Irp);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

NTSTATUS
NTAPI
HidClassFdoPower(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo,
    _In_ PIRP Irp)
{
    PIO_STACK_LOCATION IoStack = IoGetCurrentIrpStackLocation(Irp);
    BOOLEAN Pending = FALSE;
    NTSTATUS Status;

    if (IoStack->MinorFunction == IRP_MN_SET_POWER)
    {
        PoSetPowerState(Fdo->DeviceObject, IoStack->Parameters.Power.Type, IoStack->Parameters.Power.State);

        if (IoStack->Parameters.Power.Type == DevicePowerState)
        {
            DEVICE_POWER_STATE DeviceState = IoStack->Parameters.Power.State.DeviceState;

            if (DeviceState == PowerDeviceD0)
            {
                Status = IoAcquireRemoveLock(&Fdo->RemoveLock, Irp);
                if (!NT_SUCCESS(Status))
                {
                    PoStartNextPowerIrp(Irp);
                    Irp->IoStatus.Status = Status;
                    IoCompleteRequest(Irp, IO_NO_INCREMENT);
                    return Status;
                }
            }
            else if (DeviceState > PowerDeviceD0 && DeviceState < PowerDeviceMaximum)
            {
                /* Requests wait for D0, the ones already sent finish first */
                if (Fdo->IoState == HidClassIoPass)
                {
                    HidClassSetIoState(Fdo, HidClassIoQueue);
                    Fdo->PowerHeldIo = TRUE;
                }
                HidClassWaitForIoDrain(Fdo);
                HidClassStopReads(Fdo);
                Fdo->DevicePowerState = DeviceState;
            }
        }
        else
        {
            Fdo->SystemPowerState = IoStack->Parameters.Power.State.SystemState;
            if (Fdo->SystemPowerState > Fdo->Capabilities.SystemWake)
                HidClassCompleteCollectionWaitWakes(Fdo, STATUS_POWER_STATE_INVALID);
            Pending = TRUE;
        }
    }

    PoStartNextPowerIrp(Irp);
    IoCopyCurrentIrpStackLocationToNext(Irp);
    IoSetCompletionRoutine(Irp, HidClassFdoPowerCompletion, Fdo, TRUE, TRUE, TRUE);

    if (Pending)
    {
        IoMarkIrpPending(Irp);
        HidClassCallMinidriver(Fdo, Irp);
        return STATUS_PENDING;
    }

    return HidClassCallMinidriver(Fdo, Irp);
}

static
VOID
NTAPI
HidClassCancelCollectionWaitWake(
    _Inout_ PDEVICE_OBJECT DeviceObject,
    _Inout_ _IRQL_uses_cancel_ PIRP Irp)
{
    PHIDCLASS_PDO_EXTENSION Pdo = &((PHIDCLASS_DEVICE_EXTENSION)DeviceObject->DeviceExtension)->Pdo;
    PHIDCLASS_FDO_EXTENSION Fdo = Pdo->Fdo;
    KIRQL OldIrql;

    KeAcquireSpinLock(&Fdo->WaitWakeLock, &OldIrql);
    RemoveEntryList(&Irp->Tail.Overlay.ListEntry);
    InterlockedExchangePointer((PVOID *)&Pdo->WaitWakeIrp, NULL);
    KeReleaseSpinLock(&Fdo->WaitWakeLock, OldIrql);

    IoReleaseCancelSpinLock(Irp->CancelIrql);

    Irp->IoStatus.Status = STATUS_CANCELLED;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
}

/**
 * @brief
 * Holds a wait wake request of a collection until the device wakes.
 */
static
NTSTATUS
NTAPI
HidClassQueueCollectionWaitWake(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo,
    _In_ PHIDCLASS_PDO_EXTENSION Pdo,
    _In_ PIRP Irp)
{
    NTSTATUS Status = STATUS_PENDING;
    KIRQL OldIrql;

    KeAcquireSpinLock(&Fdo->WaitWakeLock, &OldIrql);

    if (InterlockedCompareExchangePointer((PVOID *)&Pdo->WaitWakeIrp, Irp, NULL) != NULL)
    {
        Status = STATUS_DEVICE_BUSY;
        goto Exit;
    }

    IoSetCancelRoutine(Irp, HidClassCancelCollectionWaitWake);
    if (Irp->Cancel)
    {
        if (IoSetCancelRoutine(Irp, NULL))
        {
            InterlockedExchangePointer((PVOID *)&Pdo->WaitWakeIrp, NULL);
            Status = STATUS_CANCELLED;
            goto Exit;
        }
        InitializeListHead(&Irp->Tail.Overlay.ListEntry);
    }
    else
    {
        InsertTailList(&Fdo->CollectionWaitWakeIrps, &Irp->Tail.Overlay.ListEntry);
    }

    IoMarkIrpPending(Irp);

Exit:
    KeReleaseSpinLock(&Fdo->WaitWakeLock, OldIrql);
    return Status;
}

VOID
NTAPI
HidClassCompleteCollectionWaitWakes(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo,
    _In_ NTSTATUS Status)
{
    LIST_ENTRY Taken;
    KIRQL OldIrql;

    InitializeListHead(&Taken);

    KeAcquireSpinLock(&Fdo->WaitWakeLock, &OldIrql);
    while (!IsListEmpty(&Fdo->CollectionWaitWakeIrps))
    {
        PLIST_ENTRY Entry = RemoveHeadList(&Fdo->CollectionWaitWakeIrps);
        PIRP Irp = CONTAINING_RECORD(Entry, IRP, Tail.Overlay.ListEntry);

        InitializeListHead(Entry);
        if (!IoSetCancelRoutine(Irp, NULL))
            continue;

        InsertTailList(&Taken, Entry);
        InterlockedExchangePointer((PVOID *)&((PHIDCLASS_DEVICE_EXTENSION)IoGetCurrentIrpStackLocation(Irp)->DeviceObject->DeviceExtension)->Pdo.WaitWakeIrp, NULL);
    }
    KeReleaseSpinLock(&Fdo->WaitWakeLock, OldIrql);

    while (!IsListEmpty(&Taken))
    {
        PIRP Irp = CONTAINING_RECORD(RemoveHeadList(&Taken), IRP, Tail.Overlay.ListEntry);

        Irp->IoStatus.Status = Status;
        if (Fdo->WokeSystem)
            PoSetSystemWake(Irp);
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
    }

    Fdo->WokeSystem = FALSE;
}

static
VOID
NTAPI
HidClassCollectionPowerComplete(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ UCHAR MinorFunction,
    _In_ POWER_STATE PowerState,
    _In_opt_ PVOID Context,
    _In_ PIO_STATUS_BLOCK IoStatus)
{
    PHIDCLASS_PDO_EXTENSION Pdo = &((PHIDCLASS_DEVICE_EXTENSION)DeviceObject->DeviceExtension)->Pdo;
    PIRP SystemIrp = Context;
    SYSTEM_POWER_STATE SystemState;

    UNREFERENCED_PARAMETER(MinorFunction);
    UNREFERENCED_PARAMETER(PowerState);

    SystemState = IoGetCurrentIrpStackLocation(SystemIrp)->Parameters.Power.State.SystemState;

    SystemIrp->IoStatus.Status = IoStatus->Status;
    PoStartNextPowerIrp(SystemIrp);
    IoCompleteRequest(SystemIrp, IO_NO_INCREMENT);

    /* A remote wake request used up by the sleep is sent again */
    if (SystemState == PowerSystemWorking && HidClassPdoCanWake(Pdo))
        HidClassRequestRemoteWake(Pdo);
}

NTSTATUS
NTAPI
HidClassPdoPower(
    _In_ PHIDCLASS_PDO_EXTENSION Pdo,
    _In_ PIRP Irp)
{
    PIO_STACK_LOCATION IoStack = IoGetCurrentIrpStackLocation(Irp);
    PHIDCLASS_FDO_EXTENSION Fdo = Pdo->Fdo;
    POWER_STATE State;
    NTSTATUS Status;

    if (Pdo->State < HidClassPdoStarted || Pdo->State > HidClassPdoStopped)
    {
        switch (IoStack->MinorFunction)
        {
            case IRP_MN_WAIT_WAKE:
                Status = STATUS_POWER_STATE_INVALID;
                break;

            case IRP_MN_SET_POWER:
                if (IoStack->Parameters.Power.Type == SystemPowerState)
                    Pdo->SystemPowerState = IoStack->Parameters.Power.State.SystemState;
                else
                    Pdo->DevicePowerState = IoStack->Parameters.Power.State.DeviceState;
                Status = STATUS_SUCCESS;
                break;

            case IRP_MN_QUERY_POWER:
                Status = STATUS_SUCCESS;
                break;

            default:
                Status = Irp->IoStatus.Status;
                break;
        }
        goto Complete;
    }

    switch (IoStack->MinorFunction)
    {
        case IRP_MN_WAIT_WAKE:
            if (Fdo->SystemPowerState > Fdo->Capabilities.SystemWake)
            {
                Status = STATUS_POWER_STATE_INVALID;
                break;
            }

            Status = HidClassQueueCollectionWaitWake(Fdo, Pdo, Irp);
            if (Status == STATUS_PENDING)
                return Status;
            break;

        case IRP_MN_SET_POWER:
            PoSetPowerState(Pdo->DeviceObject, IoStack->Parameters.Power.Type, IoStack->Parameters.Power.State);

            if (IoStack->Parameters.Power.Type == DevicePowerState)
            {
                DEVICE_POWER_STATE DeviceState = IoStack->Parameters.Power.State.DeviceState;

                if (DeviceState >= PowerDeviceD0 && DeviceState <= PowerDeviceD3)
                {
                    Pdo->DevicePowerState = DeviceState;
                    Status = STATUS_SUCCESS;
                }
                else
                {
                    Status = Irp->IoStatus.Status;
                }
                break;
            }

            /* The collection follows the system state with D0 or D3 */
            Pdo->SystemPowerState = IoStack->Parameters.Power.State.SystemState;
            State.DeviceState = (Pdo->SystemPowerState == PowerSystemWorking) ? PowerDeviceD0 : PowerDeviceD3;

            IoMarkIrpPending(Irp);
            Status = PoRequestPowerIrp(Pdo->DeviceObject,
                                       IRP_MN_SET_POWER,
                                       State,
                                       HidClassCollectionPowerComplete,
                                       Irp,
                                       NULL);
            if (!NT_SUCCESS(Status))
            {
                PoStartNextPowerIrp(Irp);
                Irp->IoStatus.Status = Status;
                IoCompleteRequest(Irp, IO_NO_INCREMENT);
            }
            return STATUS_PENDING;

        case IRP_MN_QUERY_POWER:
            Status = STATUS_SUCCESS;
            break;

        default:
            Status = Irp->IoStatus.Status;
            break;
    }

Complete:
    PoStartNextPowerIrp(Irp);
    Irp->IoStatus.Status = Status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

/**
 * @brief
 * Decides whether the device may idle out and after how long, then holds
 * or drops the policy reference on the power framework component.
 */
VOID
NTAPI
HidClassUpdateIdlePolicy(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo)
{
    BOOLEAN Allowed = FALSE;
    ULONG Timeout = 0;
    BOOLEAN Acquire = FALSE;
    BOOLEAN Release = FALSE;
    KIRQL OldIrql;

    if (!Fdo->PoFxHandle)
        return;

    KeAcquireSpinLock(&Fdo->OpenCountLock, &OldIrql);

    if (Fdo->PolicyStarted)
    {
        /* Without handles an enhanced power managed device idles regardless of wake */
        if (Fdo->OpenCount == 1 && (Fdo->IdleFlags & HIDCLASS_IDLE_ENHANCED_PM))
        {
            Allowed = TRUE;
            Timeout = Fdo->NoHandleInitialTimeout;
        }
        else if ((Fdo->IdleFlags & HIDCLASS_IDLE_SS_ENABLED) &&
                 (Fdo->IdleFlags & HIDCLASS_IDLE_SS_ON) &&
                 Fdo->Capabilities.SystemWake > PowerSystemUnspecified &&
                 Fdo->Capabilities.DeviceWake > PowerDeviceUnspecified)
        {
            Allowed = TRUE;
            Timeout = Fdo->ClientIdleTimeout ? Fdo->ClientIdleTimeout : Fdo->SelectiveSuspendTimeout;
        }
    }

    if (Allowed && Fdo->PolicyReferenceHeld)
    {
        Fdo->PolicyReferenceHeld = FALSE;
        Release = TRUE;
    }
    else if (!Allowed && !Fdo->PolicyReferenceHeld)
    {
        Fdo->PolicyReferenceHeld = TRUE;
        Acquire = TRUE;
    }

    KeReleaseSpinLock(&Fdo->OpenCountLock, OldIrql);

    if (Allowed)
        PoFxSetDeviceIdleTimeout(Fdo->PoFxHandle, (ULONGLONG)Timeout * 10000);

    if (Acquire)
        HidClassAcquirePowerReference(Fdo);
    else if (Release)
        HidClassReleasePowerReference(Fdo);
}

static
NTSTATUS
NTAPI
HidClassIdleIrpCompletion(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_opt_ PVOID Context)
{
    PHIDCLASS_FDO_EXTENSION Fdo = Context;

    UNREFERENCED_PARAMETER(DeviceObject);

    /* The minidriver refused before calling back, the device stays on */
    if (InterlockedCompareExchange(&Fdo->IdleState, HidClassIdleActive, HidClassIdleIrpPending) == HidClassIdleIrpPending)
    {
        DPRINT("HIDCLASS: idle request ended with %lx\n", Irp->IoStatus.Status);
        if (InterlockedExchange(&Fdo->NotRequiredPending, 0))
            PoFxCompleteDevicePowerNotRequired(Fdo->PoFxHandle);
    }

    KeSetEvent(&Fdo->IdleIrpDone, IO_NO_INCREMENT, FALSE);
    IoReleaseRemoveLock(&Fdo->RemoveLock, Irp);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

/**
 * @brief
 * The minidriver says the bus may suspend the device. Arm for wake when the
 * device can wake itself and go to the matching low power state.
 */
static
VOID
NTAPI
HidClassIdleDxComplete(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ UCHAR MinorFunction,
    _In_ POWER_STATE PowerState,
    _In_opt_ PVOID Context,
    _In_ PIO_STATUS_BLOCK IoStatus)
{
    PHIDCLASS_FDO_EXTENSION Fdo = Context;

    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(MinorFunction);
    UNREFERENCED_PARAMETER(PowerState);

    Fdo->PowerStatus = IoStatus->Status;
    InterlockedExchange(&Fdo->IdleState, HidClassIdlePoweredDown);
    if (InterlockedExchange(&Fdo->NotRequiredPending, 0))
        PoFxCompleteDevicePowerNotRequired(Fdo->PoFxHandle);

    KeSetEvent(&Fdo->PowerDone, IO_NO_INCREMENT, FALSE);
}

/**
 * @brief
 * The minidriver says the bus may suspend the device. Arm for wake when the
 * device can wake itself and go to the matching low power state.
 */
static
VOID
NTAPI
HidClassIdleCallback(
    _In_ PVOID Context)
{
    PHIDCLASS_FDO_EXTENSION Fdo = Context;
    POWER_STATE State;
    NTSTATUS Status;

    if (InterlockedCompareExchange(&Fdo->IdleState, HidClassIdlePoweringDown, HidClassIdleIrpPending) != HidClassIdleIrpPending)
        return;

    State.DeviceState = PowerDeviceD3;
    if (Fdo->Capabilities.DeviceWake > PowerDeviceD0 && Fdo->Capabilities.DeviceWake < PowerDeviceMaximum)
    {
        State.DeviceState = Fdo->Capabilities.DeviceWake;
        HidClassArmForWake(Fdo);
    }

    KeClearEvent(&Fdo->PowerDone);
    Status = PoRequestPowerIrp(Fdo->DeviceObject, IRP_MN_SET_POWER, State, HidClassIdleDxComplete, Fdo, NULL);
    if (!NT_SUCCESS(Status))
    {
        InterlockedExchange(&Fdo->IdleState, HidClassIdlePoweredDown);
        if (InterlockedExchange(&Fdo->NotRequiredPending, 0))
            PoFxCompleteDevicePowerNotRequired(Fdo->PoFxHandle);
        return;
    }

    /* The bus expects the device in Dx once the callback returns */
    if (KeGetCurrentIrql() == PASSIVE_LEVEL)
        KeWaitForSingleObject(&Fdo->PowerDone, Executive, KernelMode, FALSE, NULL);
}

static
VOID
NTAPI
HidClassIdleOutWorker(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_opt_ PVOID Context)
{
    PHIDCLASS_WORK_CONTEXT Work = Context;
    PHIDCLASS_FDO_EXTENSION Fdo = Work->Context;
    PIO_STACK_LOCATION IoStack;
    PIRP Irp = Fdo->IdleIrp;

    UNREFERENCED_PARAMETER(DeviceObject);
    HidClassFreeWork(Work);

    if (!Irp || Fdo->IoState != HidClassIoPass || Fdo->DevicePowerState != PowerDeviceD0 ||
        !NT_SUCCESS(IoAcquireRemoveLock(&Fdo->RemoveLock, Irp)))
    {
        if (InterlockedExchange(&Fdo->NotRequiredPending, 0))
            PoFxCompleteDevicePowerNotRequired(Fdo->PoFxHandle);
        return;
    }

    InterlockedExchange(&Fdo->IdleState, HidClassIdleIrpPending);
    KeClearEvent(&Fdo->IdleIrpDone);

    Fdo->IdleCallbackInfo.IdleCallback = HidClassIdleCallback;
    Fdo->IdleCallbackInfo.IdleContext = Fdo;

    IoReuseIrp(Irp, STATUS_NOT_SUPPORTED);
    IoStack = IoGetNextIrpStackLocation(Irp);
    IoStack->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
    IoStack->Parameters.DeviceIoControl.IoControlCode = IOCTL_HID_SEND_IDLE_NOTIFICATION_REQUEST;
    IoStack->Parameters.DeviceIoControl.InputBufferLength = sizeof(Fdo->IdleCallbackInfo);
    IoStack->Parameters.DeviceIoControl.Type3InputBuffer = &Fdo->IdleCallbackInfo;
    IoSetCompletionRoutine(Irp, HidClassIdleIrpCompletion, Fdo, TRUE, TRUE, TRUE);

    HidClassCallMinidriver(Fdo, Irp);
}

static
VOID
NTAPI
HidClassPowerUpWorker(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_opt_ PVOID Context)
{
    PHIDCLASS_WORK_CONTEXT Work = Context;
    PHIDCLASS_FDO_EXTENSION Fdo = Work->Context;
    POWER_STATE State;

    UNREFERENCED_PARAMETER(DeviceObject);
    HidClassFreeWork(Work);

    InterlockedExchange(&Fdo->IdleState, HidClassIdlePoweringUp);

    if (Fdo->DevicePowerState != PowerDeviceD0)
    {
        if (Fdo->WaitWakeIrp)
            IoCancelIrp(Fdo->WaitWakeIrp);

        State.DeviceState = PowerDeviceD0;
        KeClearEvent(&Fdo->PowerDone);
        if (NT_SUCCESS(PoRequestPowerIrp(Fdo->DeviceObject, IRP_MN_SET_POWER, State, HidClassSignalPowerComplete, Fdo, NULL)))
            KeWaitForSingleObject(&Fdo->PowerDone, Executive, KernelMode, FALSE, NULL);
    }

    /* The bus ends its suspend when the idle request comes back */
    if (Fdo->IdleIrp)
    {
        IoCancelIrp(Fdo->IdleIrp);
        KeWaitForSingleObject(&Fdo->IdleIrpDone, Executive, KernelMode, FALSE, NULL);
    }

    InterlockedExchange(&Fdo->IdleState, HidClassIdleActive);
    PoFxReportDevicePoweredOn(Fdo->PoFxHandle);
}

static
VOID
NTAPI
HidClassPoFxPowerNotRequired(
    _In_ PVOID Context)
{
    PHIDCLASS_FDO_EXTENSION Fdo = Context;

    InterlockedExchange(&Fdo->NotRequiredPending, 1);
    if (!HidClassQueueWork(Fdo->DeviceObject, HidClassIdleOutWorker, Fdo, FALSE))
    {
        InterlockedExchange(&Fdo->NotRequiredPending, 0);
        PoFxCompleteDevicePowerNotRequired(Fdo->PoFxHandle);
    }
}

static
VOID
NTAPI
HidClassPoFxPowerRequired(
    _In_ PVOID Context)
{
    PHIDCLASS_FDO_EXTENSION Fdo = Context;

    if (!HidClassQueueWork(Fdo->DeviceObject, HidClassPowerUpWorker, Fdo, FALSE))
        PoFxReportDevicePoweredOn(Fdo->PoFxHandle);
}

/**
 * @brief
 * Reads the idle settings of the device and registers it with the power
 * framework, which then runs the idle timeout.
 */
VOID
NTAPI
HidClassConfigureIdle(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo)
{
    PDEVICE_OBJECT Pdo = HidClassExtensionFromFdo(Fdo)->Hid.PhysicalDeviceObject;
    PO_FX_DEVICE_V1 Device;
    NTSTATUS Status;
    ULONG Value;

    Fdo->IdleFlags = HIDCLASS_IDLE_SS_ON;
    Fdo->SelectiveSuspendTimeout = HIDCLASS_DEFAULT_SS_TIMEOUT;
    Fdo->NoHandleTimeout = HIDCLASS_DEFAULT_NO_HANDLE_TIMEOUT;
    Fdo->NoHandleInitialTimeout = HIDCLASS_DEFAULT_NO_HANDLE_INITIAL;

    if (NT_SUCCESS(HidClassQueryDeviceRegistryValue(Pdo, L"EnhancedPowerManagementEnabled", &Value)) && Value)
    {
        Fdo->IdleFlags |= HIDCLASS_IDLE_ENHANCED_PM;
        if (NT_SUCCESS(HidClassQueryDeviceRegistryValue(Pdo, L"EnhancedPowerManagementUseMonitor", &Value)) && Value)
            Fdo->IdleFlags |= HIDCLASS_IDLE_USE_MONITOR;
    }

    if (NT_SUCCESS(HidClassQueryDeviceRegistryValue(Pdo, L"SelectiveSuspendOn", &Value)) && !Value)
        Fdo->IdleFlags &= ~HIDCLASS_IDLE_SS_ON;

    if (NT_SUCCESS(HidClassQueryDeviceRegistryValue(Pdo, L"SelectiveSuspendEnabled", &Value)) && Value)
        Fdo->IdleFlags |= HIDCLASS_IDLE_SS_ENABLED;

    if (NT_SUCCESS(HidClassQueryDeviceRegistryValue(Pdo, L"SelectiveSuspendTimeout", &Value)))
        Fdo->SelectiveSuspendTimeout = Value;

    if (NT_SUCCESS(HidClassQueryDeviceRegistryValue(Pdo, L"TestIdleTimeoutNoHandlesInitial", &Value)))
        Fdo->NoHandleInitialTimeout = Value;

    if (NT_SUCCESS(HidClassQueryDeviceRegistryValue(Pdo, L"TestIdleTimeoutNoHandles", &Value)))
        Fdo->NoHandleTimeout = Value * 1000;

    if (Fdo->PoFxHandle)
        return;

    RtlZeroMemory(&Device, sizeof(Device));
    RtlZeroMemory(&Fdo->PoFxIdleState, sizeof(Fdo->PoFxIdleState));
    Fdo->PoFxIdleState.NominalPower = PO_FX_UNKNOWN_POWER;

    Device.Version = PO_FX_VERSION_V1;
    Device.ComponentCount = 1;
    Device.DevicePowerRequiredCallback = HidClassPoFxPowerRequired;
    Device.DevicePowerNotRequiredCallback = HidClassPoFxPowerNotRequired;
    Device.DeviceContext = Fdo;
    Device.Components[0].IdleStateCount = 1;
    Device.Components[0].IdleStates = &Fdo->PoFxIdleState;

    Status = PoFxRegisterDevice(Pdo, (PPO_FX_DEVICE)&Device, &Fdo->PoFxHandle);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("HIDCLASS: power framework registration failed %lx\n", Status);
        Fdo->PoFxHandle = NULL;
        return;
    }

    /* The policy reference keeps the device on until the policy allows idle */
    Fdo->PolicyReferenceHeld = TRUE;
    HidClassAcquirePowerReference(Fdo);
    PoFxStartDevicePowerManagement(Fdo->PoFxHandle);

    Fdo->PolicyStarted = TRUE;
    HidClassUpdateIdlePolicy(Fdo);
}

VOID
NTAPI
HidClassUnregisterPoFx(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo)
{
    POHANDLE Handle = Fdo->PoFxHandle;

    if (!Handle)
        return;

    if (InterlockedExchange(&Fdo->NotRequiredPending, 0))
        PoFxCompleteDevicePowerNotRequired(Handle);

    Fdo->PoFxHandle = NULL;
    PoFxUnregisterDevice(Handle);
}

static
VOID
NTAPI
HidClassSelectiveSuspendWorker(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_opt_ PVOID Context)
{
    PHIDCLASS_WORK_CONTEXT Work = Context;
    PHIDCLASS_FDO_EXTENSION Fdo = Work->Context;

    UNREFERENCED_PARAMETER(DeviceObject);

    HidClassSetDeviceRegistryValue(HidClassExtensionFromFdo(Fdo)->Hid.PhysicalDeviceObject,
                                   L"SelectiveSuspendOn",
                                   (Fdo->IdleFlags & HIDCLASS_IDLE_SS_ON) ? 1 : 0);
    HidClassFreeWork(Work);
}

/**
 * @brief
 * WMI switch of selective suspend, remembered in the device key.
 */
NTSTATUS
NTAPI
HidClassToggleSelectiveSuspend(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo,
    _In_ BOOLEAN Enable)
{
    BOOLEAN Previous;
    KIRQL OldIrql;

    KeAcquireSpinLock(&Fdo->OpenCountLock, &OldIrql);
    Previous = (Fdo->IdleFlags & HIDCLASS_IDLE_SS_ON) != 0;
    if (Enable)
        Fdo->IdleFlags |= HIDCLASS_IDLE_SS_ON;
    else
        Fdo->IdleFlags &= ~HIDCLASS_IDLE_SS_ON;
    KeReleaseSpinLock(&Fdo->OpenCountLock, OldIrql);

    HidClassUpdateIdlePolicy(Fdo);

    if (Previous != Enable)
        HidClassQueueWork(Fdo->DeviceObject, HidClassSelectiveSuspendWorker, Fdo, Enable);

    return STATUS_SUCCESS;
}

BOOLEAN
NTAPI
HidClassPdoCanWake(
    _In_ PHIDCLASS_PDO_EXTENSION Pdo)
{
    return !Pdo->IsKeyboardOrMouse &&
           Pdo->Fdo->Capabilities.DeviceWake > PowerDeviceD0 &&
           Pdo->Fdo->Capabilities.SystemWake > PowerSystemWorking;
}

static
VOID
NTAPI
HidClassRemoteWakeWorker(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_opt_ PVOID Context)
{
    PHIDCLASS_WORK_CONTEXT Work = Context;
    PHIDCLASS_PDO_EXTENSION Pdo = Work->Context;
    BOOLEAN Enable = Work->Enable;
    BOOLEAN Changed = FALSE;
    PIRP Cancel = NULL;
    KIRQL OldIrql;

    UNREFERENCED_PARAMETER(DeviceObject);
    HidClassFreeWork(Work);

    KeAcquireSpinLock(&Pdo->RemoteWakeLock, &OldIrql);
    if (Pdo->RemoteWakeEnabled != Enable)
    {
        Changed = TRUE;
        if (Pdo->RemoteWakeEnabled)
            Cancel = InterlockedExchangePointer((PVOID *)&Pdo->RemoteWakeIrp, NULL);
        Pdo->RemoteWakeEnabled = Enable;
    }
    KeReleaseSpinLock(&Pdo->RemoteWakeLock, OldIrql);

    if (Changed)
        HidClassSetDeviceRegistryValue(Pdo->DeviceObject, L"RemoteWakeEnabled", Enable ? 1 : 0);

    if (Enable)
        HidClassRequestRemoteWake(Pdo);

    if (Cancel)
        IoCancelIrp(Cancel);

    IoReleaseRemoveLock(&Pdo->RemoveLock, Pdo);
}

/**
 * @brief
 * WMI switch that lets a raw collection wake the system.
 */
NTSTATUS
NTAPI
HidClassToggleRemoteWake(
    _In_ PHIDCLASS_PDO_EXTENSION Pdo,
    _In_ BOOLEAN Enable)
{
    if (!NT_SUCCESS(IoAcquireRemoveLock(&Pdo->RemoveLock, Pdo)))
        return STATUS_SUCCESS;

    if (!HidClassQueueWork(Pdo->DeviceObject, HidClassRemoteWakeWorker, Pdo, Enable))
        IoReleaseRemoveLock(&Pdo->RemoveLock, Pdo);

    return STATUS_SUCCESS;
}

static
VOID
NTAPI
HidClassRemoteWakeComplete(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ UCHAR MinorFunction,
    _In_ POWER_STATE PowerState,
    _In_opt_ PVOID Context,
    _In_ PIO_STATUS_BLOCK IoStatus)
{
    PHIDCLASS_PDO_EXTENSION Pdo = Context;
    NTSTATUS Status = IoStatus->Status;

    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(MinorFunction);
    UNREFERENCED_PARAMETER(PowerState);

    InterlockedExchangePointer((PVOID *)&Pdo->RemoteWakeIrp, NULL);
    InterlockedExchange(&Pdo->RemoteWakeIrpPending, 0);

    if (Status == STATUS_DEVICE_BUSY || Status == STATUS_CANCELLED ||
        Status == STATUS_INVALID_DEVICE_STATE || Status == STATUS_POWER_STATE_INVALID)
    {
        return;
    }

    /* A wake that failed turns the feature off, a good one arms again */
    if (!NT_SUCCESS(Status))
    {
        HidClassToggleRemoteWake(Pdo, FALSE);
        return;
    }

    if (Pdo->RemoteWakeEnabled && NT_SUCCESS(IoAcquireRemoveLock(&Pdo->RemoveLock, Pdo)))
    {
        if (!HidClassQueueWork(Pdo->DeviceObject, HidClassRemoteWakeWorker, Pdo, TRUE))
            IoReleaseRemoveLock(&Pdo->RemoveLock, Pdo);
    }
}

/**
 * @brief
 * Sends the collection its own wait wake request while remote wake is on.
 */
VOID
NTAPI
HidClassRequestRemoteWake(
    _In_ PHIDCLASS_PDO_EXTENSION Pdo)
{
    POWER_STATE State;

    if (!Pdo->RemoteWakeEnabled || Pdo->State != HidClassPdoStarted)
        return;

    if (InterlockedCompareExchange(&Pdo->RemoteWakeIrpPending, 1, 0) != 0)
        return;

    State.SystemState = Pdo->Fdo->Capabilities.SystemWake;
    if (!NT_SUCCESS(PoRequestPowerIrp(Pdo->DeviceObject,
                                      IRP_MN_WAIT_WAKE,
                                      State,
                                      HidClassRemoteWakeComplete,
                                      Pdo,
                                      &Pdo->RemoteWakeIrp)))
    {
        InterlockedExchangePointer((PVOID *)&Pdo->RemoteWakeIrp, NULL);
        InterlockedExchange(&Pdo->RemoteWakeIrpPending, 0);
    }
}

VOID
NTAPI
HidClassCancelRemoteWake(
    _In_ PHIDCLASS_PDO_EXTENSION Pdo)
{
    PIRP Irp = InterlockedExchangePointer((PVOID *)&Pdo->RemoteWakeIrp, NULL);

    if (Irp)
        IoCancelIrp(Irp);
}
