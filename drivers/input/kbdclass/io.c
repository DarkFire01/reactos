/*
 * PROJECT:     ReactOS Keyboard Class Driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Open, close, read and device control handling
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "kbdclass.h"

#define NDEBUG
#include <debug.h>

static DRIVER_CANCEL KbdClassCancelRead;
static IO_COMPLETION_ROUTINE KbdClassCallAllPorts;

/* INPUT QUEUE ****************************************************************/

VOID
NTAPI
KbdClassResetQueue(
    _In_ PKBDCLASS_DEVICE_EXTENSION DeviceExtension)
{
    KIRQL OldIrql;

    KeAcquireSpinLock(&DeviceExtension->QueueLock, &OldIrql);
    DeviceExtension->QueueIn = DeviceExtension->QueueBase;
    DeviceExtension->QueueOut = DeviceExtension->QueueBase;
    DeviceExtension->InputCount = 0;
    DeviceExtension->OkayToLogOverflow = TRUE;
    KeReleaseSpinLock(&DeviceExtension->QueueLock, OldIrql);
}

/**
 * @brief Takes the oldest pending read that can still be completed.
 *
 * Caller holds QueueLock.
 */
static
PIRP
KbdClassNextRead(
    _In_ PKBDCLASS_DEVICE_EXTENSION DeviceExtension)
{
    PLIST_ENTRY Entry;
    PIRP Irp;

    while (!IsListEmpty(&DeviceExtension->ReadQueue))
    {
        Entry = RemoveHeadList(&DeviceExtension->ReadQueue);
        Irp = CONTAINING_RECORD(Entry, IRP, Tail.Overlay.ListEntry);

        if (IoSetCancelRoutine(Irp, NULL))
            return Irp;

        /* The cancel routine owns this IRP and unlinks the entry itself */
        InitializeListHead(Entry);
    }

    return NULL;
}

/**
 * @brief Moves queued input into a read IRP. Caller holds QueueLock.
 */
static
NTSTATUS
KbdClassCopyQueuedData(
    _In_ PKBDCLASS_DEVICE_EXTENSION DeviceExtension,
    _In_ PIRP Irp)
{
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    PUCHAR Destination = Irp->AssociatedIrp.SystemBuffer;
    PUCHAR QueueEnd = DeviceExtension->QueueBase + DeviceExtension->QueueLength;
    ULONG Wanted;
    ULONG FirstPart;

    Wanted = DeviceExtension->InputCount * sizeof(KEYBOARD_INPUT_DATA);
    if (Wanted > Stack->Parameters.Read.Length)
        Wanted = Stack->Parameters.Read.Length;

    FirstPart = (ULONG)(QueueEnd - DeviceExtension->QueueOut);
    if (FirstPart > Wanted)
        FirstPart = Wanted;

    RtlCopyMemory(Destination, DeviceExtension->QueueOut, FirstPart);

    if (FirstPart == Wanted)
    {
        DeviceExtension->QueueOut += FirstPart;
    }
    else
    {
        RtlCopyMemory(Destination + FirstPart, DeviceExtension->QueueBase, Wanted - FirstPart);
        DeviceExtension->QueueOut = DeviceExtension->QueueBase + (Wanted - FirstPart);
    }

    DeviceExtension->InputCount -= Wanted / sizeof(KEYBOARD_INPUT_DATA);
    if (DeviceExtension->InputCount == 0)
        DeviceExtension->OkayToLogOverflow = TRUE;

    Irp->IoStatus.Information = Wanted;
    Stack->Parameters.Read.Length = Wanted;
    return STATUS_SUCCESS;
}

/**
 * @brief Appends port input to the ring buffer. Caller holds QueueLock.
 *
 * @return Number of KEYBOARD_INPUT_DATA entries taken.
 */
static
ULONG
KbdClassQueueInput(
    _In_ PKBDCLASS_DEVICE_EXTENSION DeviceExtension,
    _In_reads_bytes_(Bytes) PUCHAR Source,
    _In_ ULONG Bytes)
{
    PUCHAR QueueEnd = DeviceExtension->QueueBase + DeviceExtension->QueueLength;
    ULONG Capacity = DeviceExtension->QueueLength / sizeof(KEYBOARD_INPUT_DATA);
    ULONG FirstPart;
    ULONG Entries;

    if (Bytes > DeviceExtension->QueueLength)
        Bytes = DeviceExtension->QueueLength;

    FirstPart = (ULONG)(QueueEnd - DeviceExtension->QueueIn);
    if (FirstPart > Bytes)
        FirstPart = Bytes;

    RtlCopyMemory(DeviceExtension->QueueIn, Source, FirstPart);
    DeviceExtension->QueueIn += FirstPart;
    if (DeviceExtension->QueueIn >= QueueEnd)
        DeviceExtension->QueueIn = DeviceExtension->QueueBase;

    if (Bytes > FirstPart)
    {
        RtlCopyMemory(DeviceExtension->QueueIn, Source + FirstPart, Bytes - FirstPart);
        DeviceExtension->QueueIn += Bytes - FirstPart;
    }

    Entries = Bytes / sizeof(KEYBOARD_INPUT_DATA);

    /* New input replaced the oldest entries, keep the ring consistent */
    if (Entries > Capacity - DeviceExtension->InputCount)
    {
        DeviceExtension->InputCount = Capacity;
        DeviceExtension->QueueOut = DeviceExtension->QueueIn;
    }
    else
    {
        DeviceExtension->InputCount += Entries;
    }

    return Entries;
}

/**
 * @brief Class service callback handed to the port in CONNECT_DATA.
 */
VOID
NTAPI
KbdClassServiceCallback(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PKEYBOARD_INPUT_DATA InputDataStart,
    _In_ PKEYBOARD_INPUT_DATA InputDataEnd,
    _Inout_ PULONG InputDataConsumed)
{
    PKBDCLASS_DEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
    PUCHAR Source = (PUCHAR)InputDataStart;
    ULONG Bytes = (ULONG)((PUCHAR)InputDataEnd - (PUCHAR)InputDataStart);
    PIO_STACK_LOCATION Stack;
    LIST_ENTRY Completed;
    PLIST_ENTRY Entry;
    KIRQL OldIrql;
    ULONG Copied;
    PIRP Irp;

    *InputDataConsumed = 0;
    InitializeListHead(&Completed);

    KeAcquireSpinLock(&DeviceExtension->QueueLock, &OldIrql);

    /* A waiting reader gets the new input without going through the queue */
    Irp = KbdClassNextRead(DeviceExtension);
    if (Irp)
    {
        Stack = IoGetCurrentIrpStackLocation(Irp);

        Copied = Stack->Parameters.Read.Length;
        if (Copied > Bytes)
            Copied = Bytes;

        RtlCopyMemory(Irp->AssociatedIrp.SystemBuffer, Source, Copied);
        Irp->IoStatus.Status = STATUS_SUCCESS;
        Irp->IoStatus.Information = Copied;
        Stack->Parameters.Read.Length = Copied;

        *InputDataConsumed += Copied / sizeof(KEYBOARD_INPUT_DATA);
        InsertTailList(&Completed, &Irp->Tail.Overlay.ListEntry);

        Source += Copied;
        Bytes -= Copied;
    }

    if (Bytes)
        *InputDataConsumed += KbdClassQueueInput(DeviceExtension, Source, Bytes);

    while (DeviceExtension->InputCount)
    {
        Irp = KbdClassNextRead(DeviceExtension);
        if (!Irp)
            break;

        Irp->IoStatus.Status = KbdClassCopyQueuedData(DeviceExtension, Irp);
        InsertTailList(&Completed, &Irp->Tail.Overlay.ListEntry);
    }

    KeReleaseSpinLock(&DeviceExtension->QueueLock, OldIrql);

    while (!IsListEmpty(&Completed))
    {
        Entry = RemoveHeadList(&Completed);
        Irp = CONTAINING_RECORD(Entry, IRP, Tail.Overlay.ListEntry);

        IoCompleteRequest(Irp, IO_KEYBOARD_INCREMENT);
        IoReleaseRemoveLock(&DeviceExtension->RemoveLock, Irp);
    }
}

/**
 * @brief Cancels pending reads, either all of them or those of one file.
 */
VOID
NTAPI
KbdClassFlushReads(
    _In_ PKBDCLASS_DEVICE_EXTENSION DeviceExtension,
    _In_opt_ PFILE_OBJECT FileObject)
{
    LIST_ENTRY Cancelled;
    PLIST_ENTRY Entry;
    PLIST_ENTRY Next;
    KIRQL OldIrql;
    PIRP Irp;

    InitializeListHead(&Cancelled);

    KeAcquireSpinLock(&DeviceExtension->QueueLock, &OldIrql);

    for (Entry = DeviceExtension->ReadQueue.Flink;
         Entry != &DeviceExtension->ReadQueue;
         Entry = Next)
    {
        Next = Entry->Flink;
        Irp = CONTAINING_RECORD(Entry, IRP, Tail.Overlay.ListEntry);

        if (FileObject && IoGetCurrentIrpStackLocation(Irp)->FileObject != FileObject)
            continue;

        RemoveEntryList(Entry);

        if (IoSetCancelRoutine(Irp, NULL))
        {
            Irp->IoStatus.Status = STATUS_CANCELLED;
            Irp->IoStatus.Information = 0;
            InsertTailList(&Cancelled, Entry);
        }
        else
        {
            InitializeListHead(Entry);
        }
    }

    KeReleaseSpinLock(&DeviceExtension->QueueLock, OldIrql);

    while (!IsListEmpty(&Cancelled))
    {
        Entry = RemoveHeadList(&Cancelled);
        Irp = CONTAINING_RECORD(Entry, IRP, Tail.Overlay.ListEntry);

        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        IoReleaseRemoveLock(&DeviceExtension->RemoveLock, Irp);
    }
}

static
VOID
NTAPI
KbdClassCancelRead(
    _Inout_ PDEVICE_OBJECT DeviceObject,
    _Inout_ _IRQL_uses_cancel_ PIRP Irp)
{
    PKBDCLASS_DEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
    KIRQL OldIrql;

    IoReleaseCancelSpinLock(Irp->CancelIrql);

    KeAcquireSpinLock(&DeviceExtension->QueueLock, &OldIrql);
    RemoveEntryList(&Irp->Tail.Overlay.ListEntry);
    KeReleaseSpinLock(&DeviceExtension->QueueLock, OldIrql);

    Irp->IoStatus.Status = STATUS_CANCELLED;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    IoReleaseRemoveLock(&DeviceExtension->RemoveLock, Irp);
}

/* PORT ENABLE AND DISABLE ****************************************************/

static
NTSTATUS
KbdClassSendLegacyEnable(
    _In_ BOOLEAN Enable,
    _In_ PIRP Irp,
    _In_ PKBDCLASS_DEVICE_EXTENSION PortExtension)
{
    PIO_STACK_LOCATION Next;

    PortExtension->LegacyEnabled = Enable;

    IoCopyCurrentIrpStackLocationToNext(Irp);
    Next = IoGetNextIrpStackLocation(Irp);
    Next->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
    Next->Parameters.DeviceIoControl.OutputBufferLength = 0;
    Next->Parameters.DeviceIoControl.InputBufferLength = 0;
    Next->Parameters.DeviceIoControl.IoControlCode = Enable ? IOCTL_INTERNAL_KEYBOARD_ENABLE :
                                                              IOCTL_INTERNAL_KEYBOARD_DISABLE;
    Next->Parameters.DeviceIoControl.Type3InputBuffer = NULL;

    return KbdClassSendIrpSynchronously(PortExtension->TopPort, Irp, FALSE);
}

/**
 * @brief Opens the stack of a grand master port through its PDO name.
 */
static
NTSTATUS
KbdClassOpenPortStack(
    _In_opt_ PIRP Irp,
    _In_ PKBDCLASS_DEVICE_EXTENSION PortExtension,
    _Out_ PFILE_OBJECT *File)
{
    PIO_STACK_LOCATION Next;
    PDEVICE_OBJECT TopDevice;
    UNICODE_STRING PdoName;
    PVOID SavedBuffer;
    ULONG Length = 0;
    NTSTATUS Status;

    *File = NULL;

    IoGetDeviceProperty(PortExtension->Pdo,
                        DevicePropertyPhysicalDeviceObjectName,
                        0,
                        NULL,
                        &Length);
    if (Length < sizeof(WCHAR) || Length > MAXUSHORT)
        return STATUS_INSUFFICIENT_RESOURCES;

    PdoName.Buffer = ExAllocatePool2(POOL_FLAG_PAGED, Length, KBDCLASS_TAG);
    if (!PdoName.Buffer)
        return STATUS_INSUFFICIENT_RESOURCES;

    IoGetDeviceProperty(PortExtension->Pdo,
                        DevicePropertyPhysicalDeviceObjectName,
                        Length,
                        PdoName.Buffer,
                        &Length);
    PdoName.MaximumLength = (USHORT)Length;
    PdoName.Length = (USHORT)(Length - sizeof(WCHAR));

    Status = IoGetDeviceObjectPointer(&PdoName, FILE_ALL_ACCESS, File, &TopDevice);
    ExFreePoolWithTag(PdoName.Buffer, KBDCLASS_TAG);
    if (!NT_SUCCESS(Status))
    {
        *File = NULL;
        return Status;
    }

    /* Bring the new port in line with the LEDs of the shared device */
    if (Irp)
    {
        IoCopyCurrentIrpStackLocationToNext(Irp);
        Next = IoGetNextIrpStackLocation(Irp);
        Next->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
        Next->Parameters.DeviceIoControl.OutputBufferLength = 0;
        Next->Parameters.DeviceIoControl.InputBufferLength = sizeof(KEYBOARD_INDICATOR_PARAMETERS);
        Next->Parameters.DeviceIoControl.IoControlCode = IOCTL_KEYBOARD_SET_INDICATORS;
        Next->FileObject = *File;

        SavedBuffer = Irp->AssociatedIrp.SystemBuffer;
        Irp->AssociatedIrp.SystemBuffer = &KbdClassGlobals.GrandMaster->Indicators;
        KbdClassSendIrpSynchronously(PortExtension->TopPort, Irp, FALSE);
        Irp->AssociatedIrp.SystemBuffer = SavedBuffer;
    }

    Status = IoRegisterPlugPlayNotification(EventCategoryTargetDeviceChange,
                                            0,
                                            *File,
                                            PortExtension->Self->DriverObject,
                                            KbdClassPlugPlayNotification,
                                            PortExtension,
                                            &PortExtension->TargetNotifyHandle);
    if (!NT_SUCCESS(Status))
    {
        ObDereferenceObject(*File);
        *File = NULL;
    }

    return Status;
}

/**
 * @brief Opens or closes the port behind a class device.
 *
 * A one to one class device passes the IRP_MJ_CREATE itself down, and turns
 * the close into IRP_MJ_CLOSE. A grand master port is opened by PDO name.
 * Legacy ports get IOCTL_INTERNAL_KEYBOARD_ENABLE or DISABLE instead.
 */
NTSTATUS
NTAPI
KbdClassEnableDisablePort(
    _In_ BOOLEAN Enable,
    _In_opt_ PIRP Irp,
    _In_ PKBDCLASS_DEVICE_EXTENSION PortExtension,
    _Inout_ PFILE_OBJECT *File)
{
    PIO_STACK_LOCATION Next;

    if (!PortExtension->PnP)
    {
        ASSERT(Irp);
        return KbdClassSendLegacyEnable(Enable, Irp, PortExtension);
    }

    if (PortExtension->ClassDevice == PortExtension->Self)
    {
        ASSERT(Irp);

        /* The port stack is gone, there is nothing left to close */
        if (!Enable && PortExtension->Removed)
            return STATUS_SUCCESS;

        IoCopyCurrentIrpStackLocationToNext(Irp);
        if (!Enable)
        {
            Next = IoGetNextIrpStackLocation(Irp);
            Next->MajorFunction = IRP_MJ_CLOSE;
        }

        return KbdClassSendIrpSynchronously(PortExtension->TopPort, Irp, FALSE);
    }

    if (Enable)
        return KbdClassOpenPortStack(Irp, PortExtension, File);

    if (*File)
    {
        ObDereferenceObject(*File);
        *File = NULL;
    }

    return STATUS_SUCCESS;
}

/* CREATE, CLEANUP AND CLOSE **************************************************/

static
NTSTATUS
KbdClassOpenGrandMasterPorts(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp)
{
    PKBDCLASS_DEVICE_EXTENSION Port;
    PKBDCLASS_PORT_ENTRY Entry;
    NTSTATUS Status = STATUS_SUCCESS;
    BOOLEAN AnyOpen = FALSE;
    BOOLEAN WasEnabled;
    PFILE_OBJECT File;
    ULONG Index;

    ExAcquireFastMutex(&KbdClassGlobals.Mutex);

    /* Only the first open of the shared device opens the ports */
    if (++KbdClassGlobals.GrandMasterOpens != 1)
    {
        ExReleaseFastMutex(&KbdClassGlobals.Mutex);
        return STATUS_SUCCESS;
    }

    for (Index = 0; Index < KbdClassGlobals.PortCount; Index++)
    {
        Entry = &KbdClassGlobals.PortList[Index];
        if (Entry->Free)
            continue;

        WasEnabled = Entry->Enabled;
        Entry->Enabled = TRUE;
        Port = Entry->Port;
        File = Entry->File;
        ExReleaseFastMutex(&KbdClassGlobals.Mutex);

        if (WasEnabled)
        {
            AnyOpen = TRUE;
            ExAcquireFastMutex(&KbdClassGlobals.Mutex);
            continue;
        }

        Status = KbdClassEnableDisablePort(TRUE, Irp, Port, &File);
        if (NT_SUCCESS(Status))
        {
            AnyOpen = TRUE;
        }
        else
        {
            KbdClassLogError(DeviceObject,
                             KBDCLASS_LOG_PORT_OPEN_FAILED,
                             KBDCLASS_ERRID_PORT_OPEN_CLOSE,
                             Status,
                             0,
                             NULL,
                             IRP_MJ_CREATE);
        }

        ExAcquireFastMutex(&KbdClassGlobals.Mutex);

        Entry = &KbdClassGlobals.PortList[Index];
        if (Index < KbdClassGlobals.PortCount && Entry->Port == Port)
        {
            Entry->File = File;
            if (!NT_SUCCESS(Status))
                Entry->Enabled = FALSE;
        }
    }

    ExReleaseFastMutex(&KbdClassGlobals.Mutex);

    return AnyOpen ? STATUS_SUCCESS : Status;
}

/**
 * @brief IRP_MJ_CREATE handler.
 *
 * A checked open asking for read data is refused unless it carries
 * KBDCLASS_CREATE_READ_OPTION. Only SeTcbPrivilege holders may read.
 */
NTSTATUS
NTAPI
KbdClassCreate(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PKBDCLASS_DEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    PIO_SECURITY_CONTEXT SecurityContext = Stack->Parameters.Create.SecurityContext;
    KPROCESSOR_MODE CheckMode;
    BOOLEAN ForceCheck;
    KIRQL OldIrql;
    NTSTATUS Status;

    ForceCheck = (Stack->Flags & SL_FORCE_ACCESS_CHECK) != 0;

    if ((Irp->RequestorMode == UserMode || ForceCheck) &&
        SecurityContext &&
        (SecurityContext->DesiredAccess & FILE_READ_DATA) &&
        !(Stack->Parameters.Create.Options & KBDCLASS_CREATE_READ_OPTION))
    {
        Status = STATUS_ACCESS_DENIED;
        goto Complete;
    }

    Status = IoAcquireRemoveLock(&DeviceExtension->RemoveLock, Irp);
    if (!NT_SUCCESS(Status))
        goto Complete;

    if (DeviceExtension->PnP && !DeviceExtension->Started)
    {
        Status = STATUS_UNSUCCESSFUL;
        goto Release;
    }

    Stack->Parameters.Create.Options &= ~KBDCLASS_CREATE_READ_OPTION;

    if (DeviceExtension->Self == DeviceExtension->ClassDevice)
    {
        CheckMode = ForceCheck ? UserMode : Irp->RequestorMode;
        if (SeSinglePrivilegeCheck(RtlConvertLongToLuid(SE_TCB_PRIVILEGE), CheckMode))
        {
            KeAcquireSpinLock(&DeviceExtension->QueueLock, &OldIrql);
            Stack->FileObject->FsContext2 = KBDCLASS_TRUSTED_CONTEXT;
            DeviceExtension->TrustedOpens++;
            KeReleaseSpinLock(&DeviceExtension->QueueLock, OldIrql);
        }
    }

    if (DeviceExtension == KbdClassGlobals.GrandMaster)
    {
        Status = KbdClassOpenGrandMasterPorts(DeviceObject, Irp);
    }
    else if (DeviceExtension->ClassDevice != DeviceObject)
    {
        /* Port device of the grand master, the open belongs to the port */
        IoSkipCurrentIrpStackLocation(Irp);
        Status = IoCallDriver(DeviceExtension->TopPort, Irp);
        IoReleaseRemoveLock(&DeviceExtension->RemoveLock, Irp);
        return Status;
    }
    else
    {
        Status = KbdClassEnableDisablePort(TRUE, Irp, DeviceExtension, &Stack->FileObject);
        if (NT_SUCCESS(Status))
            Status = STATUS_SUCCESS;
    }

Release:
    IoReleaseRemoveLock(&DeviceExtension->RemoveLock, Irp);

Complete:
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

NTSTATUS
NTAPI
KbdClassCleanup(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PKBDCLASS_DEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
    PFILE_OBJECT FileObject = IoGetCurrentIrpStackLocation(Irp)->FileObject;

    if (FileObject && FileObject->FsContext2 == KBDCLASS_TRUSTED_CONTEXT)
        KbdClassFlushReads(DeviceExtension, FileObject);

    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

static
NTSTATUS
KbdClassCloseGrandMasterPorts(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp)
{
    PKBDCLASS_DEVICE_EXTENSION Port;
    PKBDCLASS_PORT_ENTRY Entry;
    NTSTATUS Status = STATUS_SUCCESS;
    BOOLEAN AnyClosed = FALSE;
    BOOLEAN WasEnabled;
    PFILE_OBJECT File;
    PVOID Notify;
    ULONG Index;

    ExAcquireFastMutex(&KbdClassGlobals.Mutex);

    KbdClassGlobals.GrandMasterOpens -= 1;
    if (KbdClassGlobals.GrandMasterOpens != 0)
    {
        ExReleaseFastMutex(&KbdClassGlobals.Mutex);
        return STATUS_SUCCESS;
    }

    for (Index = 0; Index < KbdClassGlobals.PortCount; Index++)
    {
        Entry = &KbdClassGlobals.PortList[Index];
        if (Entry->Free)
            continue;

        WasEnabled = Entry->Enabled;
        Entry->Enabled = FALSE;
        Port = Entry->Port;
        File = Entry->File;
        ExReleaseFastMutex(&KbdClassGlobals.Mutex);

        if (WasEnabled)
        {
            Notify = InterlockedExchangePointer(&Port->TargetNotifyHandle, NULL);
            if (Notify)
                IoUnregisterPlugPlayNotification(Notify);

            Status = KbdClassEnableDisablePort(FALSE, Irp, Port, &File);
        }

        if (NT_SUCCESS(Status))
        {
            AnyClosed = TRUE;
        }
        else
        {
            KbdClassLogError(DeviceObject,
                             KBDCLASS_LOG_PORT_CLOSE_FAILED,
                             KBDCLASS_ERRID_PORT_OPEN_CLOSE,
                             Status,
                             0,
                             NULL,
                             0);
        }

        ExAcquireFastMutex(&KbdClassGlobals.Mutex);

        Entry = &KbdClassGlobals.PortList[Index];
        if (WasEnabled && Index < KbdClassGlobals.PortCount && Entry->Port == Port)
            Entry->File = File;
    }

    ExReleaseFastMutex(&KbdClassGlobals.Mutex);

    return AnyClosed ? STATUS_SUCCESS : Status;
}

NTSTATUS
NTAPI
KbdClassClose(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PKBDCLASS_DEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    KIRQL OldIrql;
    NTSTATUS Status;

    if (DeviceExtension->Self == DeviceExtension->ClassDevice)
    {
        KeAcquireSpinLock(&DeviceExtension->QueueLock, &OldIrql);
        if (Stack->FileObject->FsContext2 == KBDCLASS_TRUSTED_CONTEXT)
        {
            DeviceExtension->TrustedOpens -= 1;
            Stack->FileObject->FsContext2 = NULL;
        }
        KeReleaseSpinLock(&DeviceExtension->QueueLock, OldIrql);
    }

    if (DeviceExtension == KbdClassGlobals.GrandMaster)
    {
        Status = KbdClassCloseGrandMasterPorts(DeviceObject, Irp);
    }
    else if (DeviceExtension->ClassDevice != DeviceObject)
    {
        IoSkipCurrentIrpStackLocation(Irp);
        return IoCallDriver(DeviceExtension->TopPort, Irp);
    }
    else
    {
        Status = KbdClassEnableDisablePort(FALSE, Irp, DeviceExtension, &Stack->FileObject);
        if (NT_SUCCESS(Status))
        {
            Status = STATUS_SUCCESS;
        }
        else
        {
            KbdClassLogError(DeviceObject,
                             KBDCLASS_LOG_PORT_CLOSE_FAILED,
                             KBDCLASS_ERRID_PORT_OPEN_CLOSE,
                             Status,
                             0,
                             NULL,
                             IRP_MJ_CLOSE);
        }
    }

    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

/* READ AND FLUSH *************************************************************/

NTSTATUS
NTAPI
KbdClassRead(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PKBDCLASS_DEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    ULONG Length = Stack->Parameters.Read.Length;
    KIRQL OldIrql;
    NTSTATUS Status;

    Irp->IoStatus.Information = 0;

    if (Length == 0)
    {
        Status = STATUS_SUCCESS;
        goto Complete;
    }

    if (Length % sizeof(KEYBOARD_INPUT_DATA))
    {
        Status = STATUS_BUFFER_TOO_SMALL;
        goto Complete;
    }

    if (DeviceExtension->SurpriseRemoved)
    {
        Status = STATUS_DEVICE_NOT_CONNECTED;
        goto Complete;
    }

    if (!Stack->FileObject || Stack->FileObject->FsContext2 != KBDCLASS_TRUSTED_CONTEXT)
    {
        Status = STATUS_PRIVILEGE_NOT_HELD;
        goto Complete;
    }

    Status = IoAcquireRemoveLock(&DeviceExtension->RemoveLock, Irp);
    if (!NT_SUCCESS(Status))
        goto Complete;

    Irp->IoStatus.Status = STATUS_PENDING;

    KeAcquireSpinLock(&DeviceExtension->QueueLock, &OldIrql);

    if (DeviceExtension->InputCount)
    {
        Status = KbdClassCopyQueuedData(DeviceExtension, Irp);
        Irp->IoStatus.Status = Status;
        KeReleaseSpinLock(&DeviceExtension->QueueLock, OldIrql);

        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        IoReleaseRemoveLock(&DeviceExtension->RemoveLock, Irp);
        return Status;
    }

    InsertTailList(&DeviceExtension->ReadQueue, &Irp->Tail.Overlay.ListEntry);
    IoSetCancelRoutine(Irp, KbdClassCancelRead);

    if (Irp->Cancel && IoSetCancelRoutine(Irp, NULL))
    {
        RemoveEntryList(&Irp->Tail.Overlay.ListEntry);
        KeReleaseSpinLock(&DeviceExtension->QueueLock, OldIrql);

        Status = STATUS_CANCELLED;
        Irp->IoStatus.Status = Status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        IoReleaseRemoveLock(&DeviceExtension->RemoveLock, Irp);
        return Status;
    }

    IoMarkIrpPending(Irp);
    KeReleaseSpinLock(&DeviceExtension->QueueLock, OldIrql);
    return STATUS_PENDING;

Complete:
    Irp->IoStatus.Status = Status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

NTSTATUS
NTAPI
KbdClassFlush(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PKBDCLASS_DEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
    PFILE_OBJECT FileObject = IoGetCurrentIrpStackLocation(Irp)->FileObject;
    NTSTATUS Status = STATUS_SUCCESS;

    if (DeviceExtension->Self != DeviceExtension->ClassDevice)
        Status = STATUS_NOT_SUPPORTED;
    else if (!FileObject || FileObject->FsContext2 != KBDCLASS_TRUSTED_CONTEXT)
        Status = STATUS_PRIVILEGE_NOT_HELD;
    else
        KbdClassResetQueue(DeviceExtension);

    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

/* DEVICE CONTROL *************************************************************/

NTSTATUS
NTAPI
KbdClassPassThrough(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PKBDCLASS_DEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;

    /* The grand master has no stack below it */
    if (!DeviceExtension->TopPort)
    {
        Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_NOT_SUPPORTED;
    }

    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(DeviceExtension->TopPort, Irp);
}

static
BOOLEAN
KbdClassIsPortOnlyIoctl(
    _In_ ULONG IoControlCode)
{
    static const ULONG PortOnlyCodes[] =
    {
        KBDCLASS_PORT_IOCTL(0x64, METHOD_BUFFERED),
        KBDCLASS_PORT_IOCTL(0x64, METHOD_IN_DIRECT),
        KBDCLASS_PORT_IOCTL(0x64, METHOD_OUT_DIRECT),
        KBDCLASS_PORT_IOCTL(0x64, METHOD_NEITHER),
        KBDCLASS_PORT_IOCTL(0x65, METHOD_BUFFERED),
        KBDCLASS_PORT_IOCTL(0x65, METHOD_NEITHER),
        KBDCLASS_PORT_IOCTL(0x66, METHOD_BUFFERED),
        KBDCLASS_PORT_IOCTL(0x66, METHOD_OUT_DIRECT),
        KBDCLASS_PORT_IOCTL(0x67, METHOD_BUFFERED),
        KBDCLASS_PORT_IOCTL(0x67, METHOD_OUT_DIRECT),
        KBDCLASS_PORT_IOCTL(0x68, METHOD_BUFFERED),
        KBDCLASS_PORT_IOCTL(0x69, METHOD_BUFFERED),
        KBDCLASS_PORT_IOCTL(0x6A, METHOD_BUFFERED),
        KBDCLASS_PORT_IOCTL(0x6E, METHOD_OUT_DIRECT),
        KBDCLASS_PORT_IOCTL(0x6F, METHOD_OUT_DIRECT),
        KBDCLASS_PORT_IOCTL(0x70, METHOD_OUT_DIRECT),
        KBDCLASS_PORT_IOCTL(0x78, METHOD_OUT_DIRECT),
        KBDCLASS_PORT_IOCTL(0x9A, METHOD_BUFFERED),
        KBDCLASS_BATTERY_IOCTL(0x50),
        KBDCLASS_BATTERY_IOCTL(0x51)
    };
    ULONG Index;

    for (Index = 0; Index < RTL_NUMBER_OF(PortOnlyCodes); Index++)
    {
        if (PortOnlyCodes[Index] == IoControlCode)
            return TRUE;
    }

    return FALSE;
}

/**
 * @brief Picks the grand master port for a unit id. Caller holds Mutex.
 *
 * An unusable unit falls back to the first enabled port.
 */
static
BOOLEAN
KbdClassPickPort(
    _Inout_ PULONG UnitId)
{
    PKBDCLASS_PORT_ENTRY List = KbdClassGlobals.PortList;
    ULONG Index;

    if (*UnitId >= KbdClassGlobals.PortCount)
        return FALSE;

    if (List[*UnitId].Enabled && !List[*UnitId].Free)
        return TRUE;

    for (Index = 0; Index < KbdClassGlobals.PortCount; Index++)
    {
        if (List[Index].Enabled && !List[Index].Free)
        {
            *UnitId = Index;
            return TRUE;
        }
    }

    return FALSE;
}

/**
 * @brief Sends one IOCTL to each enabled port in turn.
 *
 * Runs first from the dispatch routine and then as its own completion
 * routine until every port in the snapshot was called.
 */
static
NTSTATUS
NTAPI
KbdClassCallAllPorts(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_reads_opt_(_Inexpressible_("varies")) PVOID Context)
{
    PKBDCLASS_PORT_SNAPSHOT Snapshot = Context;
    PKBDCLASS_DEVICE_EXTENSION GrandMaster = KbdClassGlobals.GrandMaster;
    PKBDCLASS_DEVICE_EXTENSION Port;
    PKBDCLASS_PORT_ENTRY Entry;
    PIO_STACK_LOCATION Next;
    BOOLEAN FirstCall;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(DeviceObject);

    FirstCall = (Snapshot->Next == 0);

    IoCopyCurrentIrpStackLocationToNext(Irp);
    Next = IoGetNextIrpStackLocation(Irp);
    Next->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;

    while (Snapshot->Next < Snapshot->Count)
    {
        Entry = &Snapshot->Ports[Snapshot->Next];
        if (Entry->Enabled && !Entry->Free)
            break;

        Snapshot->Next++;
    }

    if (Snapshot->Next >= Snapshot->Count)
    {
        if (Irp->PendingReturned)
            IoMarkIrpPending(Irp);

        IoReleaseRemoveLock(&GrandMaster->RemoveLock, Irp);
        ExFreePoolWithTag(Snapshot, KBDCLASS_TAG);

        /* Every port went away after the unit was checked */
        if (FirstCall)
        {
            Irp->IoStatus.Status = STATUS_INVALID_PARAMETER;
            Irp->IoStatus.Information = 0;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            return STATUS_INVALID_PARAMETER;
        }

        return STATUS_SUCCESS;
    }

    Entry = &Snapshot->Ports[Snapshot->Next++];
    Port = Entry->Port;
    Next->FileObject = Entry->File;

    IoSetCompletionRoutine(Irp, KbdClassCallAllPorts, Snapshot, TRUE, TRUE, TRUE);
    Status = IoCallDriver(Port->TopPort, Irp);
    IoReleaseRemoveLock(&Port->RemoveLock, Irp);

    return FirstCall ? Status : STATUS_MORE_PROCESSING_REQUIRED;
}

static
NTSTATUS
KbdClassSendToAllPorts(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp)
{
    PKBDCLASS_DEVICE_EXTENSION GrandMaster = DeviceObject->DeviceExtension;
    PKBDCLASS_PORT_SNAPSHOT Snapshot;
    PKBDCLASS_PORT_ENTRY Entry;
    ULONG Index;

    IoAcquireRemoveLock(&GrandMaster->RemoveLock, Irp);

    ExAcquireFastMutex(&KbdClassGlobals.Mutex);

    Snapshot = ExAllocatePool2(POOL_FLAG_NON_PAGED,
                               FIELD_OFFSET(KBDCLASS_PORT_SNAPSHOT, Ports) +
                               KbdClassGlobals.PortCount * sizeof(KBDCLASS_PORT_ENTRY),
                               KBDCLASS_TAG);
    if (!Snapshot)
    {
        ExReleaseFastMutex(&KbdClassGlobals.Mutex);
        IoReleaseRemoveLock(&GrandMaster->RemoveLock, Irp);

        Irp->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Snapshot->Count = KbdClassGlobals.PortCount;
    Snapshot->Next = 0;

    for (Index = 0; Index < Snapshot->Count; Index++)
    {
        Entry = &Snapshot->Ports[Index];
        *Entry = KbdClassGlobals.PortList[Index];

        /* Each port stays referenced until its turn has come */
        if (Entry->Enabled && !Entry->Free &&
            !NT_SUCCESS(IoAcquireRemoveLock(&Entry->Port->RemoveLock, Irp)))
        {
            Entry->Enabled = FALSE;
        }
    }

    ExReleaseFastMutex(&KbdClassGlobals.Mutex);

    return KbdClassCallAllPorts(DeviceObject, Irp, Snapshot);
}

/**
 * @brief IRP_MJ_DEVICE_CONTROL handler.
 *
 * Keyboard IOCTLs are turned into internal device controls for the port that
 * the unit id selects. SET_INDICATORS and SET_TYPEMATIC go to every port of
 * the grand master when SendOutputToAllPorts is set.
 */
NTSTATUS
NTAPI
KbdClassDeviceControl(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PKBDCLASS_DEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
    PKBDCLASS_DEVICE_EXTENSION Target = DeviceExtension;
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    ULONG InputLength = Stack->Parameters.DeviceIoControl.InputBufferLength;
    PFILE_OBJECT TargetFile;
    PIO_STACK_LOCATION Next;
    BOOLEAN AllPorts = FALSE;
    ULONG UnitId;
    NTSTATUS Status;

    Status = IoAcquireRemoveLock(&DeviceExtension->RemoveLock, Irp);
    if (!NT_SUCCESS(Status))
    {
        Irp->IoStatus.Status = Status;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return Status;
    }

    switch (Stack->Parameters.DeviceIoControl.IoControlCode)
    {
        case IOCTL_KEYBOARD_SET_INDICATORS:
            if (InputLength < sizeof(KEYBOARD_INDICATOR_PARAMETERS))
            {
                Status = STATUS_BUFFER_TOO_SMALL;
                goto Complete;
            }

            RtlCopyMemory(&DeviceExtension->Indicators,
                          Irp->AssociatedIrp.SystemBuffer,
                          sizeof(DeviceExtension->Indicators));
            AllPorts = (KbdClassGlobals.SendOutputToAllPorts != 0);
            break;

        case IOCTL_KEYBOARD_SET_TYPEMATIC:
            AllPorts = (KbdClassGlobals.SendOutputToAllPorts != 0);
            break;

        case IOCTL_KEYBOARD_QUERY_ATTRIBUTES:
        case IOCTL_KEYBOARD_QUERY_TYPEMATIC:
        case IOCTL_KEYBOARD_QUERY_INDICATORS:
        case IOCTL_KEYBOARD_QUERY_INDICATOR_TRANSLATION:
        case IOCTL_KEYBOARD_QUERY_EXTENDED_ATTRIBUTES:
        case IOCTL_KEYBOARD_QUERY_IME_STATUS:
        case IOCTL_KEYBOARD_SET_IME_STATUS:
            break;

        default:
            if (KbdClassIsPortOnlyIoctl(Stack->Parameters.DeviceIoControl.IoControlCode) &&
                DeviceExtension->PnP &&
                DeviceExtension != KbdClassGlobals.GrandMaster)
            {
                IoSkipCurrentIrpStackLocation(Irp);
                Status = IoCallDriver(DeviceExtension->TopPort, Irp);
                goto Release;
            }

            Status = STATUS_INVALID_DEVICE_REQUEST;
            goto Complete;
    }

    if (InputLength == 0)
    {
        UnitId = 0;
    }
    else if (InputLength < sizeof(KEYBOARD_UNIT_ID_PARAMETER))
    {
        Status = STATUS_BUFFER_TOO_SMALL;
        goto Complete;
    }
    else
    {
        UnitId = ((PKEYBOARD_UNIT_ID_PARAMETER)Irp->AssociatedIrp.SystemBuffer)->UnitId;
    }

    /* Port devices of the grand master are not opened for keyboard IOCTLs */
    if (DeviceExtension->Self != DeviceExtension->ClassDevice)
    {
        Status = STATUS_NOT_SUPPORTED;
        goto Complete;
    }

    TargetFile = Stack->FileObject;

    if (DeviceExtension == KbdClassGlobals.GrandMaster)
    {
        ExAcquireFastMutex(&KbdClassGlobals.Mutex);

        if (!KbdClassPickPort(&UnitId))
        {
            ExReleaseFastMutex(&KbdClassGlobals.Mutex);
            Status = STATUS_INVALID_PARAMETER;
            goto Complete;
        }

        Target = KbdClassGlobals.PortList[UnitId].Port;
        TargetFile = KbdClassGlobals.PortList[UnitId].File;
        ExReleaseFastMutex(&KbdClassGlobals.Mutex);
    }
    else
    {
        AllPorts = FALSE;
    }

    IoCopyCurrentIrpStackLocationToNext(Irp);
    Next = IoGetNextIrpStackLocation(Irp);
    Next->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
    Next->FileObject = TargetFile;

    if (AllPorts)
        Status = KbdClassSendToAllPorts(DeviceObject, Irp);
    else
        Status = IoCallDriver(Target->TopPort, Irp);

    goto Release;

Complete:
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

Release:
    IoReleaseRemoveLock(&DeviceExtension->RemoveLock, Irp);
    return Status;
}
