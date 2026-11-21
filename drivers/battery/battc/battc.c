/*
 * PROJECT:         ReactOS Kernel
 * LICENSE:         GPL - See COPYING in the top level directory
 * FILE:            drivers/battery/battc/battc.c
 * PURPOSE:         Battery Class Driver
 * PROGRAMMERS:     Cameron Gutman (cameron.gutman@reactos.org)
 */

#include <battc.h>

#define NDEBUG
#include <debug.h>

NTSTATUS
NTAPI
DriverEntry(PDRIVER_OBJECT DriverObject,
            PUNICODE_STRING RegistryPath)
{
    DPRINT("Battery class driver initialized\n");

    return STATUS_SUCCESS;
}

/* PRIVATE FUNCTIONS *********************************************************/

static
VOID
BattcCompleteWait(
    _In_ PBATTC_WAIT Wait,
    _In_ NTSTATUS Status,
    _In_ ULONG_PTR Information)
{
    PIRP Irp = Wait->Irp;

    ExFreePoolWithTag(Wait, BATTERY_CLASS_WAIT_TAG);

    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = Information;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
}

static
VOID
BattcQueueRecheck(
    _In_ PBATTERY_CLASS_DATA BattClass)
{
    if (BattClass->Unloading)
        return;

    if (!InterlockedExchange(&BattClass->WorkQueued, 1))
    {
        InterlockedIncrement(&BattClass->WorkBusy);
        ExQueueWorkItem(&BattClass->WorkItem, DelayedWorkQueue);
    }
}

static
VOID
BattcLockNotify(
    _In_ PBATTERY_CLASS_DATA BattClass)
{
    KeWaitForSingleObject(&BattClass->NotifyLock, Executive, KernelMode, FALSE, NULL);
}

static
VOID
BattcUnlockNotify(
    _In_ PBATTERY_CLASS_DATA BattClass)
{
    KeSetEvent(&BattClass->NotifyLock, IO_NO_INCREMENT, FALSE);
}

static
VOID
NTAPI
BattcCancelWait(
    _Inout_ PDEVICE_OBJECT DeviceObject,
    _Inout_ _IRQL_uses_cancel_ PIRP Irp)
{
    PBATTC_WAIT Wait = Irp->Tail.Overlay.DriverContext[0];
    PBATTERY_CLASS_DATA BattClass = Wait->Class;
    KIRQL OldIrql;

    UNREFERENCED_PARAMETER(DeviceObject);

    IoReleaseCancelSpinLock(Irp->CancelIrql);

    KeAcquireSpinLock(&BattClass->WaitLock, &OldIrql);
    RemoveEntryList(&Irp->Tail.Overlay.ListEntry);
    KeReleaseSpinLock(&BattClass->WaitLock, OldIrql);

    BattcCompleteWait(Wait, STATUS_CANCELLED, 0);

    /* The last status wait gone lets the miniport stop watching */
    BattcQueueRecheck(BattClass);
}

/**
 * @brief Puts a wait on the list, or completes it when it was cancelled meanwhile.
 */
static
VOID
BattcInsertWait(
    _In_ PBATTERY_CLASS_DATA BattClass,
    _In_ PBATTC_WAIT Wait)
{
    PIRP Irp = Wait->Irp;
    KIRQL OldIrql;

    KeAcquireSpinLock(&BattClass->WaitLock, &OldIrql);

    if (BattClass->Unloading)
    {
        KeReleaseSpinLock(&BattClass->WaitLock, OldIrql);
        BattcCompleteWait(Wait, STATUS_DEVICE_REMOVED, 0);
        return;
    }

    IoSetCancelRoutine(Irp, BattcCancelWait);
    if (Irp->Cancel && IoSetCancelRoutine(Irp, NULL))
    {
        KeReleaseSpinLock(&BattClass->WaitLock, OldIrql);
        BattcCompleteWait(Wait, STATUS_CANCELLED, 0);
        return;
    }

    /* A cancel routine already running removes it again once the lock is free */
    InsertTailList(&BattClass->WaitList, &Irp->Tail.Overlay.ListEntry);
    KeReleaseSpinLock(&BattClass->WaitLock, OldIrql);
}

static
BOOLEAN
BattcStatusChanged(
    _In_ PBATTERY_WAIT_STATUS Criteria,
    _In_ PBATTERY_STATUS Current)
{
    return Current->PowerState != Criteria->PowerState ||
           Current->Capacity < Criteria->LowCapacity ||
           Current->Capacity > Criteria->HighCapacity;
}

/**
 * @brief Asks the miniport again for what a wait is after.
 *
 * @return TRUE when the wait was completed.
 */
static
BOOLEAN
BattcCheckWait(
    _In_ PBATTC_WAIT Wait,
    _In_ ULONGLONG Now)
{
    PBATTERY_MINIPORT_INFO Miniport = &Wait->Class->MiniportInfo;
    PVOID Buffer = Wait->Irp->AssociatedIrp.SystemBuffer;
    BOOLEAN Expired = (Wait->Deadline != 0 && Now >= Wait->Deadline);
    BATTERY_STATUS Current;
    NTSTATUS Status;
    ULONG Tag = BATTERY_TAG_INVALID;

    if (Wait->Kind == BATTC_WAIT_TAG)
    {
        Status = Miniport->QueryTag(Miniport->Context, &Tag);
        if (NT_SUCCESS(Status) && Tag != BATTERY_TAG_INVALID)
        {
            *(PULONG)Buffer = Tag;
            BattcCompleteWait(Wait, STATUS_SUCCESS, sizeof(Tag));
            return TRUE;
        }

        if (Expired)
        {
            BattcCompleteWait(Wait, STATUS_NO_SUCH_DEVICE, 0);
            return TRUE;
        }

        return FALSE;
    }

    RtlZeroMemory(&Current, sizeof(Current));
    Status = Miniport->QueryStatus(Miniport->Context, Wait->Criteria.BatteryTag, &Current);
    if (!NT_SUCCESS(Status))
    {
        BattcCompleteWait(Wait, Status, 0);
        return TRUE;
    }

    if (Expired || BattcStatusChanged(&Wait->Criteria, &Current))
    {
        RtlCopyMemory(Buffer, &Current, sizeof(Current));
        BattcCompleteWait(Wait, STATUS_SUCCESS, sizeof(Current));
        return TRUE;
    }

    if (Wait->Polled)
        Wait->NextPoll = Now + BATTC_POLL_INTERVAL_MS * 10000ULL;

    return FALSE;
}

static
ULONGLONG
BattcEarlier(
    _In_ ULONGLONG Due,
    _In_ ULONGLONG Time)
{
    if (Time == 0)
        return Due;

    return (Due == 0 || Time < Due) ? Time : Due;
}

/**
 * @brief Rechecks every pending wait, then arms the timer for the next deadline or poll.
 */
static
VOID
NTAPI
BattcRecheckWorker(
    _In_ PVOID Parameter)
{
    PBATTERY_CLASS_DATA BattClass = Parameter;
    LIST_ENTRY Pending;
    PLIST_ENTRY Entry, Next;
    PBATTC_WAIT Wait;
    LARGE_INTEGER DueTime;
    ULONGLONG Now, Due = 0;
    BOOLEAN StatusWaits = FALSE;
    KIRQL OldIrql;
    PIRP Irp;

    InterlockedExchange(&BattClass->WorkQueued, 0);
    InitializeListHead(&Pending);

    /* The miniport is called at passive level, so the waits leave the list meanwhile */
    KeAcquireSpinLock(&BattClass->WaitLock, &OldIrql);
    for (Entry = BattClass->WaitList.Flink; Entry != &BattClass->WaitList; Entry = Next)
    {
        Next = Entry->Flink;
        Irp = CONTAINING_RECORD(Entry, IRP, Tail.Overlay.ListEntry);

        if (!IoSetCancelRoutine(Irp, NULL))
            continue;

        RemoveEntryList(Entry);
        InsertTailList(&Pending, Entry);
    }
    KeReleaseSpinLock(&BattClass->WaitLock, OldIrql);

    Now = KeQueryInterruptTime();
    while (!IsListEmpty(&Pending))
    {
        Entry = RemoveHeadList(&Pending);
        Irp = CONTAINING_RECORD(Entry, IRP, Tail.Overlay.ListEntry);
        Wait = Irp->Tail.Overlay.DriverContext[0];

        if (!BattcCheckWait(Wait, Now))
            BattcInsertWait(BattClass, Wait);
    }

    BattcLockNotify(BattClass);

    KeAcquireSpinLock(&BattClass->WaitLock, &OldIrql);
    for (Entry = BattClass->WaitList.Flink; Entry != &BattClass->WaitList; Entry = Entry->Flink)
    {
        Irp = CONTAINING_RECORD(Entry, IRP, Tail.Overlay.ListEntry);
        Wait = Irp->Tail.Overlay.DriverContext[0];

        Due = BattcEarlier(Due, Wait->Deadline);
        if (Wait->Kind != BATTC_WAIT_STATUS)
            continue;

        StatusWaits = TRUE;
        if (Wait->Polled)
            Due = BattcEarlier(Due, Wait->NextPoll);
    }
    KeReleaseSpinLock(&BattClass->WaitLock, OldIrql);

    if (!StatusWaits && BattClass->NotifySet)
    {
        BattClass->NotifySet = FALSE;
        BattClass->MiniportInfo.DisableStatusNotify(BattClass->MiniportInfo.Context);
    }

    BattcUnlockNotify(BattClass);

    if (Due != 0 && !BattClass->Unloading)
    {
        Now = KeQueryInterruptTime();
        DueTime.QuadPart = (Due > Now) ? -(LONGLONG)(Due - Now) : -1;
        KeSetTimer(&BattClass->Timer, DueTime, &BattClass->TimerDpc);
    }

    InterlockedDecrement(&BattClass->WorkBusy);
}

static
VOID
NTAPI
BattcTimerDpc(
    _In_ PKDPC Dpc,
    _In_opt_ PVOID DeferredContext,
    _In_opt_ PVOID SystemArgument1,
    _In_opt_ PVOID SystemArgument2)
{
    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);

    BattcQueueRecheck(DeferredContext);
}

/**
 * @brief Pends a tag or status request that has to wait, as BatteryClassIoctl may not block.
 *
 * @param[in] TimeoutMs
 * How long to wait, (ULONG)-1 for no limit.
 */
static
NTSTATUS
BattcPendWait(
    _In_ PBATTERY_CLASS_DATA BattClass,
    _In_ PIRP Irp,
    _In_ UCHAR Kind,
    _In_ ULONG TimeoutMs,
    _In_opt_ PBATTERY_WAIT_STATUS Criteria)
{
    PBATTERY_MINIPORT_INFO Miniport = &BattClass->MiniportInfo;
    ULONGLONG Now = KeQueryInterruptTime();
    BATTERY_NOTIFY Notify;
    PBATTC_WAIT Wait, Other;
    PLIST_ENTRY Entry;
    KIRQL OldIrql;
    NTSTATUS Status;

    Wait = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Wait), BATTERY_CLASS_WAIT_TAG);
    if (!Wait)
    {
        Irp->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(Wait, sizeof(*Wait));
    Wait->Class = BattClass;
    Wait->Irp = Irp;
    Wait->Kind = Kind;
    if (TimeoutMs != (ULONG)-1)
        Wait->Deadline = Now + (ULONGLONG)TimeoutMs * 10000;

    Irp->Tail.Overlay.DriverContext[0] = Wait;
    IoMarkIrpPending(Irp);

    if (Kind != BATTC_WAIT_STATUS)
    {
        BattcInsertWait(BattClass, Wait);
        BattcQueueRecheck(BattClass);
        return STATUS_PENDING;
    }

    Wait->Criteria = *Criteria;
    Notify.PowerState = Criteria->PowerState;
    Notify.LowCapacity = Criteria->LowCapacity;
    Notify.HighCapacity = Criteria->HighCapacity;

    BattcLockNotify(BattClass);

    Status = Miniport->SetStatusNotify(Miniport->Context, Criteria->BatteryTag, &Notify);
    if (NT_SUCCESS(Status))
    {
        BattClass->NotifySet = TRUE;
    }
    else
    {
        DPRINT("SetStatusNotify failed (0x%x), polling\n", Status);
        Wait->Polled = TRUE;
        Wait->NextPoll = Now + BATTC_POLL_INTERVAL_MS * 10000ULL;
    }

    /* The miniport keeps only the latest thresholds, the waits before it are polled from now on */
    KeAcquireSpinLock(&BattClass->WaitLock, &OldIrql);
    for (Entry = BattClass->WaitList.Flink; Entry != &BattClass->WaitList; Entry = Entry->Flink)
    {
        Other = CONTAINING_RECORD(Entry, IRP, Tail.Overlay.ListEntry)->Tail.Overlay.DriverContext[0];
        if (Other->Kind != BATTC_WAIT_STATUS || Other->Polled)
            continue;

        Other->Polled = TRUE;
        Other->NextPoll = Now + BATTC_POLL_INTERVAL_MS * 10000ULL;
    }
    KeReleaseSpinLock(&BattClass->WaitLock, OldIrql);

    BattcInsertWait(BattClass, Wait);
    BattcUnlockNotify(BattClass);

    BattcQueueRecheck(BattClass);
    return STATUS_PENDING;
}

/* PUBLIC FUNCTIONS **********************************************************/

BCLASSAPI
NTSTATUS
NTAPI
BatteryClassUnload(PVOID ClassData)
{
    PBATTERY_CLASS_DATA BattClass;
    LARGE_INTEGER Interval;
    LIST_ENTRY Pending;
    PLIST_ENTRY Entry, Next;
    KIRQL OldIrql;
    PIRP Irp;

    DPRINT("Battery %p is being unloaded\n", ClassData);

    BattClass = ClassData;
    Interval.QuadPart = -10 * 10000LL;

    KeAcquireSpinLock(&BattClass->WaitLock, &OldIrql);
    BattClass->Unloading = TRUE;
    KeReleaseSpinLock(&BattClass->WaitLock, OldIrql);

    KeCancelTimer(&BattClass->Timer);
    KeFlushQueuedDpcs();
    while (BattClass->WorkBusy)
        KeDelayExecutionThread(KernelMode, FALSE, &Interval);

    /* The battery is gone, nothing that waits for it will see it again */
    InitializeListHead(&Pending);
    KeAcquireSpinLock(&BattClass->WaitLock, &OldIrql);
    for (Entry = BattClass->WaitList.Flink; Entry != &BattClass->WaitList; Entry = Next)
    {
        Next = Entry->Flink;
        Irp = CONTAINING_RECORD(Entry, IRP, Tail.Overlay.ListEntry);

        if (!IoSetCancelRoutine(Irp, NULL))
            continue;

        RemoveEntryList(Entry);
        InsertTailList(&Pending, Entry);
    }
    KeReleaseSpinLock(&BattClass->WaitLock, OldIrql);

    while (!IsListEmpty(&Pending))
    {
        Entry = RemoveHeadList(&Pending);
        Irp = CONTAINING_RECORD(Entry, IRP, Tail.Overlay.ListEntry);
        BattcCompleteWait(Irp->Tail.Overlay.DriverContext[0], STATUS_DEVICE_REMOVED, 0);
    }

    /* Cancel routines still running take the lock, the class data has to outlive them */
    while (!IsListEmpty(&BattClass->WaitList))
        KeDelayExecutionThread(KernelMode, FALSE, &Interval);

    if (BattClass->NotifySet)
        BattClass->MiniportInfo.DisableStatusNotify(BattClass->MiniportInfo.Context);

    if (BattClass->InterfaceName.Length != 0)
    {
        IoSetDeviceInterfaceState(&BattClass->InterfaceName, FALSE);
        RtlFreeUnicodeString(&BattClass->InterfaceName);
    }

    ExFreePoolWithTag(BattClass,
                      BATTERY_CLASS_DATA_TAG);

    return STATUS_SUCCESS;
}

BCLASSAPI
NTSTATUS
NTAPI
BatteryClassSystemControl(PVOID ClassData,
                          PVOID WmiLibContext,
                          PDEVICE_OBJECT DeviceObject,
                          PIRP Irp,
                          PVOID Disposition)
{
    UNIMPLEMENTED;

    return STATUS_WMI_GUID_NOT_FOUND;
}

BCLASSAPI
NTSTATUS
NTAPI
BatteryClassQueryWmiDataBlock(PVOID ClassData,
                              PDEVICE_OBJECT DeviceObject,
                              PIRP Irp,
                              ULONG GuidIndex,
                              PULONG InstanceLengthArray,
                              ULONG OutBufferSize,
                              PUCHAR Buffer)
{
    UNIMPLEMENTED;

    return STATUS_WMI_GUID_NOT_FOUND;
}

BCLASSAPI
NTSTATUS
NTAPI
BatteryClassStatusNotify(
    _In_ PVOID ClassData)
{
    DPRINT("Received battery status notification from %p\n", ClassData);

    /* Callable up to dispatch level, the waits are checked from a worker */
    BattcQueueRecheck(ClassData);
    return STATUS_SUCCESS;
}

BCLASSAPI
NTSTATUS
NTAPI
BatteryClassInitializeDevice(PBATTERY_MINIPORT_INFO MiniportInfo,
                             PVOID *ClassData)
{
    NTSTATUS Status;
    PBATTERY_CLASS_DATA BattClass;

    BattClass = ExAllocatePoolWithTag(NonPagedPool,
                                      sizeof(BATTERY_CLASS_DATA),
                                      BATTERY_CLASS_DATA_TAG);
    if (BattClass == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlZeroMemory(BattClass, sizeof(BATTERY_CLASS_DATA));

    RtlCopyMemory(&BattClass->MiniportInfo,
                  MiniportInfo,
                  sizeof(BattClass->MiniportInfo));

    KeInitializeSpinLock(&BattClass->WaitLock);
    InitializeListHead(&BattClass->WaitList);
    KeInitializeEvent(&BattClass->NotifyLock, SynchronizationEvent, TRUE);
    KeInitializeTimer(&BattClass->Timer);
    KeInitializeDpc(&BattClass->TimerDpc, BattcTimerDpc, BattClass);
    ExInitializeWorkItem(&BattClass->WorkItem, BattcRecheckWorker, BattClass);

    if (MiniportInfo->Pdo != NULL)
    {
        Status = IoRegisterDeviceInterface(MiniportInfo->Pdo,
                                           &GUID_DEVICE_BATTERY,
                                           NULL,
                                           &BattClass->InterfaceName);
        if (NT_SUCCESS(Status))
        {
            DPRINT("Initialized battery interface: %wZ\n", &BattClass->InterfaceName);
            Status = IoSetDeviceInterfaceState(&BattClass->InterfaceName, TRUE);
            if (Status == STATUS_OBJECT_NAME_EXISTS)
            {
                DPRINT1("Got STATUS_OBJECT_NAME_EXISTS for SetDeviceInterfaceState\n");
                Status = STATUS_SUCCESS;
            }
        }
        else
        {
            DPRINT1("IoRegisterDeviceInterface failed (0x%x)\n", Status);
        }
    }

    *ClassData = BattClass;

    return STATUS_SUCCESS;
}

BCLASSAPI
NTSTATUS
NTAPI
BatteryClassIoctl(PVOID ClassData,
                  PIRP Irp)
{
    PBATTERY_CLASS_DATA BattClass;
    PIO_STACK_LOCATION IrpSp;
    NTSTATUS Status;
    ULONG WaitTime;
    ULONG Tag;
    BATTERY_WAIT_STATUS BattWait;
    PBATTERY_QUERY_INFORMATION BattQueryInfo;
    PBATTERY_SET_INFORMATION BattSetInfo;
    BATTERY_STATUS BattStatus;
    ULONG ReturnedLength;

    DPRINT("BatteryClassIoctl(%p %p)\n", ClassData, Irp);

    BattClass = ClassData;

    IrpSp = IoGetCurrentIrpStackLocation(Irp);
    Irp->IoStatus.Information = 0;

    DPRINT("Received IOCTL %x for %p\n", IrpSp->Parameters.DeviceIoControl.IoControlCode,
           ClassData);

    /* Requests that wait are pended, the miniport may hold its own lock around this call */
    switch (IrpSp->Parameters.DeviceIoControl.IoControlCode)
    {
        case IOCTL_BATTERY_QUERY_TAG:
            if ((IrpSp->Parameters.DeviceIoControl.InputBufferLength != sizeof(ULONG) && IrpSp->Parameters.DeviceIoControl.InputBufferLength != 0) ||
                IrpSp->Parameters.DeviceIoControl.OutputBufferLength < sizeof(ULONG))
            {
                Status = STATUS_BUFFER_TOO_SMALL;
                break;
            }

            WaitTime = IrpSp->Parameters.DeviceIoControl.InputBufferLength == sizeof(ULONG) ? *(PULONG)Irp->AssociatedIrp.SystemBuffer : 0;

            Tag = BATTERY_TAG_INVALID;
            Status = BattClass->MiniportInfo.QueryTag(BattClass->MiniportInfo.Context, &Tag);
            if (NT_SUCCESS(Status) && Tag != BATTERY_TAG_INVALID)
            {
                *(PULONG)Irp->AssociatedIrp.SystemBuffer = Tag;
                Irp->IoStatus.Information = sizeof(ULONG);
                Status = STATUS_SUCCESS;
                break;
            }

            if (WaitTime == 0)
            {
                Status = STATUS_NO_SUCH_DEVICE;
                break;
            }

            return BattcPendWait(BattClass, Irp, BATTC_WAIT_TAG, WaitTime, NULL);

        case IOCTL_BATTERY_QUERY_STATUS:
            if (IrpSp->Parameters.DeviceIoControl.InputBufferLength < sizeof(BattWait) ||
                IrpSp->Parameters.DeviceIoControl.OutputBufferLength < sizeof(BATTERY_STATUS))
            {
                Status = STATUS_BUFFER_TOO_SMALL;
                break;
            }

            BattWait = *(PBATTERY_WAIT_STATUS)Irp->AssociatedIrp.SystemBuffer;

            /* Zeroed so no stack data leaks out */
            RtlZeroMemory(&BattStatus, sizeof(BattStatus));
            Status = BattClass->MiniportInfo.QueryStatus(BattClass->MiniportInfo.Context,
                                                         BattWait.BatteryTag,
                                                         &BattStatus);
            if (!NT_SUCCESS(Status))
                break;

            /* A status that already differs from what the caller has is returned right away */
            if (BattWait.Timeout != 0 && !BattcStatusChanged(&BattWait, &BattStatus))
                return BattcPendWait(BattClass, Irp, BATTC_WAIT_STATUS, BattWait.Timeout, &BattWait);

            RtlCopyMemory(Irp->AssociatedIrp.SystemBuffer, &BattStatus, sizeof(BattStatus));
            Irp->IoStatus.Information = sizeof(BATTERY_STATUS);
            break;

        case IOCTL_BATTERY_QUERY_INFORMATION:
            if (IrpSp->Parameters.DeviceIoControl.InputBufferLength < sizeof(*BattQueryInfo))
            {
                Status = STATUS_BUFFER_TOO_SMALL;
                break;
            }

            BattQueryInfo = Irp->AssociatedIrp.SystemBuffer;

            Status = BattClass->MiniportInfo.QueryInformation(BattClass->MiniportInfo.Context,
                                                              BattQueryInfo->BatteryTag,
                                                              BattQueryInfo->InformationLevel,
                                                              BattQueryInfo->AtRate,
                                                              Irp->AssociatedIrp.SystemBuffer,
                                                              IrpSp->Parameters.DeviceIoControl.OutputBufferLength,
                                                              &ReturnedLength);
            Irp->IoStatus.Information = ReturnedLength;
            if (!NT_SUCCESS(Status))
            {
                DPRINT1("QueryInformation failed (0x%x)\n", Status);
            }
            break;

        case IOCTL_BATTERY_SET_INFORMATION:
            if (IrpSp->Parameters.DeviceIoControl.InputBufferLength < sizeof(*BattSetInfo))
            {
                Status = STATUS_BUFFER_TOO_SMALL;
                break;
            }

            BattSetInfo = Irp->AssociatedIrp.SystemBuffer;

            Status = BattClass->MiniportInfo.SetInformation(BattClass->MiniportInfo.Context,
                                                            BattSetInfo->BatteryTag,
                                                            BattSetInfo->InformationLevel,
                                                            BattSetInfo->Buffer);
            if (!NT_SUCCESS(Status))
            {
                DPRINT1("SetInformation failed (0x%x)\n", Status);
            }
            break;

        default:
            DPRINT1("Received unsupported IRP %x\n", IrpSp->Parameters.DeviceIoControl.IoControlCode);
            /* Do NOT complete the irp */
            return STATUS_NOT_SUPPORTED;
    }

    Irp->IoStatus.Status = Status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return Status;
}
