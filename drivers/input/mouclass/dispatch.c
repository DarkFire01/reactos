/*
 * PROJECT:     ReactOS Mouse Class Driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Open, close, read and control requests, the input queue
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "mouclass.h"
#include <hidclass.h>
#include <poclass.h>

#define NDEBUG
#include <debug.h>

static DRIVER_CANCEL MouseCancelRead;
static IO_COMPLETION_ROUTINE MouseSignalCompletion;
static DRIVER_NOTIFICATION_CALLBACK_ROUTINE MousePortNotification;

static
NTSTATUS
NTAPI
MouseCompleteIrp(
    _In_ PIRP Irp,
    _In_ NTSTATUS Status)
{
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

static
NTSTATUS
NTAPI
MouseSignalCompletion(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_opt_ PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);

    KeSetEvent(Context, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

/**
 * @brief
 * Sends an IRP down and waits for it, the IRP is handed back to the caller.
 *
 * @param[in] CopyStack
 * Copy the current stack location to the next one first.
 */
NTSTATUS
NTAPI
MouseForwardAndWait(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ BOOLEAN CopyStack)
{
    KEVENT Event;

    KeInitializeEvent(&Event, SynchronizationEvent, FALSE);

    if (CopyStack)
        IoCopyCurrentIrpStackLocationToNext(Irp);

    IoSetCompletionRoutine(Irp, MouseSignalCompletion, &Event, TRUE, TRUE, TRUE);
    IoCallDriver(DeviceObject, Irp);
    KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);

    return Irp->IoStatus.Status;
}

/**
 * @brief
 * Empties the input queue. Pending reads are left alone.
 */
VOID
NTAPI
MouseResetQueue(
    _In_ PMOUCLASS_EXTENSION Extension)
{
    KIRQL OldIrql;

    KeAcquireSpinLock(&Extension->QueueLock, &OldIrql);
    Extension->QueueIn = Extension->QueueStart;
    Extension->QueueOut = Extension->QueueStart;
    Extension->QueueCount = 0;
    KeReleaseSpinLock(&Extension->QueueLock, OldIrql);
}

/**
 * @brief
 * Takes the oldest pending read that is not being cancelled. QueueLock must be held.
 */
static
PIRP
NTAPI
MousePopRead(
    _In_ PMOUCLASS_EXTENSION Extension)
{
    PLIST_ENTRY Entry;
    PIRP Irp;

    while (!IsListEmpty(&Extension->ReadQueue))
    {
        Entry = RemoveHeadList(&Extension->ReadQueue);
        Irp = CONTAINING_RECORD(Entry, IRP, Tail.Overlay.ListEntry);
        if (IoSetCancelRoutine(Irp, NULL) != NULL)
            return Irp;

        /* The cancel routine owns this one and unlinks it on its own */
        InitializeListHead(Entry);
    }

    return NULL;
}

/**
 * @brief
 * Moves queued input into a read IRP. QueueLock must be held.
 */
static
VOID
NTAPI
MouseFillReadFromQueue(
    _In_ PMOUCLASS_EXTENSION Extension,
    _In_ PIRP Irp)
{
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    PUCHAR Target = Irp->AssociatedIrp.SystemBuffer;
    PUCHAR QueueEnd = (PUCHAR)Extension->QueueStart + Extension->QueueBytes;
    ULONG Bytes;
    ULONG ToEnd;

    Bytes = min(Extension->QueueCount * MOUCLASS_PACKET_SIZE,
                Stack->Parameters.Read.Length);
    ToEnd = (ULONG)(QueueEnd - (PUCHAR)Extension->QueueOut);

    if (Bytes <= ToEnd)
    {
        RtlCopyMemory(Target, Extension->QueueOut, Bytes);
        Extension->QueueOut = (PMOUSE_INPUT_DATA)((PUCHAR)Extension->QueueOut + Bytes);
        if ((PUCHAR)Extension->QueueOut >= QueueEnd)
            Extension->QueueOut = Extension->QueueStart;
    }
    else
    {
        RtlCopyMemory(Target, Extension->QueueOut, ToEnd);
        RtlCopyMemory(Target + ToEnd, Extension->QueueStart, Bytes - ToEnd);
        Extension->QueueOut = (PMOUSE_INPUT_DATA)((PUCHAR)Extension->QueueStart + (Bytes - ToEnd));
    }

    Extension->QueueCount -= Bytes / MOUCLASS_PACKET_SIZE;

    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = Bytes;
    Stack->Parameters.Read.Length = Bytes;
}

/**
 * @brief
 * Appends port input to the ring. QueueLock must be held.
 *
 * @return
 * Number of bytes taken, at most the size of the whole ring.
 */
static
ULONG
NTAPI
MouseQueueInput(
    _In_ PMOUCLASS_EXTENSION Extension,
    _In_reads_bytes_(Bytes) PUCHAR Source,
    _In_ ULONG Bytes)
{
    PUCHAR QueueEnd = (PUCHAR)Extension->QueueStart + Extension->QueueBytes;
    ULONG Capacity = Extension->QueueBytes / MOUCLASS_PACKET_SIZE;
    ULONG ToEnd;

    Bytes = min(Bytes, Extension->QueueBytes);
    ToEnd = (ULONG)(QueueEnd - (PUCHAR)Extension->QueueIn);

    if (Bytes < ToEnd)
    {
        RtlCopyMemory(Extension->QueueIn, Source, Bytes);
        Extension->QueueIn = (PMOUSE_INPUT_DATA)((PUCHAR)Extension->QueueIn + Bytes);
    }
    else
    {
        RtlCopyMemory(Extension->QueueIn, Source, ToEnd);
        RtlCopyMemory(Extension->QueueStart, Source + ToEnd, Bytes - ToEnd);
        Extension->QueueIn = (PMOUSE_INPUT_DATA)((PUCHAR)Extension->QueueStart + (Bytes - ToEnd));
    }

    Extension->QueueCount += Bytes / MOUCLASS_PACKET_SIZE;

    /* New input overwrote the oldest entries, the ring now starts at QueueIn */
    if (Extension->QueueCount > Capacity)
    {
        Extension->QueueCount = Capacity;
        Extension->QueueOut = Extension->QueueIn;
    }

    return Bytes;
}

/**
 * @brief
 * Port callback that delivers MOUSE_INPUT_DATA packets to the class device.
 */
VOID
NTAPI
MouseServiceCallback(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PMOUSE_INPUT_DATA InputDataStart,
    _In_ PMOUSE_INPUT_DATA InputDataEnd,
    _Inout_ PULONG InputDataConsumed)
{
    PMOUCLASS_EXTENSION Extension = DeviceObject->DeviceExtension;
    PUCHAR Source = (PUCHAR)InputDataStart;
    ULONG Remaining = (ULONG)((PUCHAR)InputDataEnd - (PUCHAR)InputDataStart);
    PIO_STACK_LOCATION Stack;
    LIST_ENTRY Completed;
    PLIST_ENTRY Entry;
    KIRQL OldIrql;
    ULONG Bytes;
    PIRP Irp;

    *InputDataConsumed = 0;
    InitializeListHead(&Completed);

    KeAcquireSpinLock(&Extension->QueueLock, &OldIrql);

    /* The first waiting reader gets the packets straight from the port */
    Irp = MousePopRead(Extension);
    if (Irp != NULL)
    {
        Stack = IoGetCurrentIrpStackLocation(Irp);
        Bytes = min(Remaining, Stack->Parameters.Read.Length);
        RtlCopyMemory(Irp->AssociatedIrp.SystemBuffer, Source, Bytes);

        Irp->IoStatus.Status = STATUS_SUCCESS;
        Irp->IoStatus.Information = Bytes;
        Stack->Parameters.Read.Length = Bytes;
        InsertTailList(&Completed, &Irp->Tail.Overlay.ListEntry);

        *InputDataConsumed += Bytes / MOUCLASS_PACKET_SIZE;
        Source += Bytes;
        Remaining -= Bytes;
    }

    if (Remaining != 0)
    {
        Bytes = MouseQueueInput(Extension, Source, Remaining);
        *InputDataConsumed += Bytes / MOUCLASS_PACKET_SIZE;
    }

    while (Extension->QueueCount != 0)
    {
        Irp = MousePopRead(Extension);
        if (Irp == NULL)
            break;

        MouseFillReadFromQueue(Extension, Irp);
        InsertTailList(&Completed, &Irp->Tail.Overlay.ListEntry);
    }

    KeReleaseSpinLock(&Extension->QueueLock, OldIrql);

    while (!IsListEmpty(&Completed))
    {
        Entry = RemoveHeadList(&Completed);
        Irp = CONTAINING_RECORD(Entry, IRP, Tail.Overlay.ListEntry);
        IoCompleteRequest(Irp, IO_MOUSE_INCREMENT);
        IoReleaseRemoveLock(&Extension->RemoveLock, Irp);
    }
}

static
VOID
NTAPI
MouseCancelRead(
    _Inout_ PDEVICE_OBJECT DeviceObject,
    _Inout_ _IRQL_uses_cancel_ PIRP Irp)
{
    PMOUCLASS_EXTENSION Extension = DeviceObject->DeviceExtension;
    KIRQL OldIrql;

    IoReleaseCancelSpinLock(Irp->CancelIrql);

    KeAcquireSpinLock(&Extension->QueueLock, &OldIrql);
    RemoveEntryList(&Irp->Tail.Overlay.ListEntry);
    KeReleaseSpinLock(&Extension->QueueLock, OldIrql);

    Irp->IoStatus.Status = STATUS_CANCELLED;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    IoReleaseRemoveLock(&Extension->RemoveLock, Irp);
}

/**
 * @brief
 * Cancels queued reads, all of them or only those of one file object.
 */
VOID
NTAPI
MouseCancelQueuedReads(
    _In_ PMOUCLASS_EXTENSION Extension,
    _In_opt_ PFILE_OBJECT FileObject)
{
    LIST_ENTRY Cancelled;
    PLIST_ENTRY Entry;
    PLIST_ENTRY Next;
    KIRQL OldIrql;
    PIRP Irp;

    InitializeListHead(&Cancelled);

    KeAcquireSpinLock(&Extension->QueueLock, &OldIrql);

    for (Entry = Extension->ReadQueue.Flink; Entry != &Extension->ReadQueue; Entry = Next)
    {
        Next = Entry->Flink;
        Irp = CONTAINING_RECORD(Entry, IRP, Tail.Overlay.ListEntry);

        if (FileObject != NULL && IoGetCurrentIrpStackLocation(Irp)->FileObject != FileObject)
            continue;

        RemoveEntryList(Entry);
        if (IoSetCancelRoutine(Irp, NULL) == NULL)
        {
            InitializeListHead(Entry);
            continue;
        }

        Irp->IoStatus.Status = STATUS_CANCELLED;
        Irp->IoStatus.Information = 0;
        InsertTailList(&Cancelled, Entry);
    }

    KeReleaseSpinLock(&Extension->QueueLock, OldIrql);

    while (!IsListEmpty(&Cancelled))
    {
        Entry = RemoveHeadList(&Cancelled);
        Irp = CONTAINING_RECORD(Entry, IRP, Tail.Overlay.ListEntry);
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        IoReleaseRemoveLock(&Extension->RemoveLock, Irp);
    }
}

/**
 * @brief
 * Opens the stack of a PnP port served by the shared class device.
 */
static
NTSTATUS
NTAPI
MouseOpenPortStack(
    _In_ PMOUCLASS_EXTENSION Port,
    _Out_ PFILE_OBJECT *FileObject)
{
    PDEVICE_OBJECT TopDevice;
    UNICODE_STRING PdoName;
    ULONG Length = 0;
    NTSTATUS Status;

    IoGetDeviceProperty(Port->Pdo, DevicePropertyPhysicalDeviceObjectName, 0, NULL, &Length);
    if (Length < sizeof(WCHAR))
        return STATUS_UNSUCCESSFUL;

    PdoName.Buffer = ExAllocatePoolWithTag(PagedPool, Length, MOUCLASS_TAG);
    if (PdoName.Buffer == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = IoGetDeviceProperty(Port->Pdo,
                                 DevicePropertyPhysicalDeviceObjectName,
                                 Length,
                                 PdoName.Buffer,
                                 &Length);
    if (NT_SUCCESS(Status))
    {
        PdoName.MaximumLength = (USHORT)Length;
        PdoName.Length = (USHORT)(Length - sizeof(WCHAR));
        Status = IoGetDeviceObjectPointer(&PdoName, FILE_ALL_ACCESS, FileObject, &TopDevice);
    }

    ExFreePoolWithTag(PdoName.Buffer, MOUCLASS_TAG);

    if (!NT_SUCCESS(Status))
        return Status;

    return IoRegisterPlugPlayNotification(EventCategoryTargetDeviceChange,
                                          0,
                                          *FileObject,
                                          Port->Self->DriverObject,
                                          MousePortNotification,
                                          Port,
                                          &Port->TargetNotifyHandle);
}

/**
 * @brief
 * Turns a port on or off for the class device.
 *
 * A PnP per port class device passes the caller's create or close down its stack.
 * A legacy port is switched with the internal enable or disable request.
 * A PnP port behind the shared class device has its stack opened or closed by PDO name.
 *
 * @param[in] Irp
 * The create or close being served. Only optional for PnP ports of the shared device.
 *
 * @param[in,out] FileObject
 * Port file object, used by ports of the shared device.
 */
NTSTATUS
NTAPI
MouseSwitchPort(
    _In_ BOOLEAN Enable,
    _In_opt_ PIRP Irp,
    _In_ PMOUCLASS_EXTENSION Port,
    _Inout_ PFILE_OBJECT *FileObject)
{
    PIO_STACK_LOCATION Next;

    if (Port->IsPnp && Port->ClassDevice == Port->Self)
    {
        ASSERT(Irp != NULL);
        IoCopyCurrentIrpStackLocationToNext(Irp);
        if (!Enable)
            IoGetNextIrpStackLocation(Irp)->MajorFunction = IRP_MJ_CLOSE;

        return MouseForwardAndWait(Port->LowerDevice, Irp, FALSE);
    }

    if (!Port->IsPnp)
    {
        ASSERT(Irp != NULL);
        if (Irp == NULL)
            return STATUS_INVALID_PARAMETER;

        IoCopyCurrentIrpStackLocationToNext(Irp);
        Next = IoGetNextIrpStackLocation(Irp);
        Next->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
        Next->Parameters.DeviceIoControl.OutputBufferLength = 0;
        Next->Parameters.DeviceIoControl.InputBufferLength = 0;
        Next->Parameters.DeviceIoControl.IoControlCode =
            Enable ? IOCTL_INTERNAL_MOUSE_ENABLE : IOCTL_INTERNAL_MOUSE_DISABLE;
        Next->Parameters.DeviceIoControl.Type3InputBuffer = NULL;

        return MouseForwardAndWait(Port->LowerDevice, Irp, FALSE);
    }

    if (Enable)
        return MouseOpenPortStack(Port, FileObject);

    if (*FileObject != NULL)
    {
        ObDereferenceObject(*FileObject);
        *FileObject = NULL;
    }

    return STATUS_SUCCESS;
}

static
VOID
NTAPI
MouseDropPortNotification(
    _In_ PMOUCLASS_EXTENSION Port)
{
    PVOID Handle;

    Handle = InterlockedExchangePointer(&Port->TargetNotifyHandle, NULL);
    if (Handle != NULL)
        IoUnregisterPlugPlayNotification(Handle);
}

/**
 * @brief
 * Stores a port file object back into the shared port table. MouGlobals.Lock must be held.
 */
static
PMOUCLASS_PORT
NTAPI
MouseStorePortFile(
    _In_ ULONG Index,
    _In_ PMOUCLASS_EXTENSION Port,
    _In_opt_ PFILE_OBJECT FileObject)
{
    PMOUCLASS_PORT Entry;

    if (Index >= MouGlobals.PortCount)
        return NULL;

    Entry = &MouGlobals.Ports[Index];
    if (Entry->Port != Port)
        return NULL;

    Entry->FileObject = FileObject;
    return Entry;
}

/**
 * @brief
 * Switches every port of the shared class device. MouGlobals.Lock is held on entry and exit.
 */
static
NTSTATUS
NTAPI
MouseSwitchSharedPorts(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ BOOLEAN Enable,
    _In_ NTSTATUS Status)
{
    UCHAR MajorFunction = IoGetCurrentIrpStackLocation(Irp)->MajorFunction;
    PMOUCLASS_EXTENSION Port;
    PMOUCLASS_PORT Entry;
    PFILE_OBJECT FileObject;
    BOOLEAN IsAnySwitched = FALSE;
    BOOLEAN WasEnabled;
    ULONG Index;

    for (Index = 0; Index < MouGlobals.PortCount; Index++)
    {
        Entry = &MouGlobals.Ports[Index];
        if (Entry->Free)
            continue;

        WasEnabled = Entry->Enabled;
        Entry->Enabled = Enable;
        Port = Entry->Port;
        FileObject = Entry->FileObject;
        ExReleaseFastMutex(&MouGlobals.Lock);

        if (WasEnabled != Enable)
        {
            if (!Enable)
                MouseDropPortNotification(Port);

            Status = MouseSwitchPort(Enable, Irp, Port, &FileObject);
        }

        ExAcquireFastMutex(&MouGlobals.Lock);
        Entry = MouseStorePortFile(Index, Port, FileObject);

        /* A failure stays in Status and also counts against later ports */
        if (Status != STATUS_SUCCESS)
        {
            MouseLogError(DeviceObject,
                          Enable ? MOUCLASS_EVT_PORT_OPEN_FAILED : MOUCLASS_EVT_PORT_CLOSE_FAILED,
                          20120,
                          Status,
                          0,
                          NULL,
                          MajorFunction);
            if (Enable && Entry != NULL)
                Entry->Enabled = FALSE;
        }
        else
        {
            IsAnySwitched = TRUE;
        }
    }

    return IsAnySwitched ? STATUS_SUCCESS : Status;
}

/**
 * @brief
 * Switches one port of the shared class device on behalf of a PnP notification.
 */
static
NTSTATUS
NTAPI
MouseSwitchGlobalPort(
    _In_ PMOUCLASS_EXTENSION Port,
    _In_ BOOLEAN Enable)
{
    PFILE_OBJECT FileObject;
    BOOLEAN WasEnabled;
    NTSTATUS Status;
    ULONG Index;

    ExAcquireFastMutex(&MouGlobals.Lock);

    if (MouGlobals.SharedOpens <= 0)
    {
        ExReleaseFastMutex(&MouGlobals.Lock);
        return STATUS_SUCCESS;
    }

    for (Index = 0; Index < MouGlobals.PortCount; Index++)
    {
        if (!MouGlobals.Ports[Index].Free && MouGlobals.Ports[Index].Port == Port)
            break;
    }

    if (Index == MouGlobals.PortCount)
    {
        ExReleaseFastMutex(&MouGlobals.Lock);
        return STATUS_NO_SUCH_DEVICE;
    }

    WasEnabled = MouGlobals.Ports[Index].Enabled;
    MouGlobals.Ports[Index].Enabled = Enable;
    FileObject = MouGlobals.Ports[Index].FileObject;
    ExReleaseFastMutex(&MouGlobals.Lock);

    if (WasEnabled == Enable)
        return STATUS_SUCCESS;

    Status = MouseSwitchPort(Enable, NULL, Port, &FileObject);

    ExAcquireFastMutex(&MouGlobals.Lock);
    MouseStorePortFile(Index, Port, FileObject);
    ExReleaseFastMutex(&MouGlobals.Lock);

    return Status;
}

/**
 * @brief
 * Target device notification for a port stack opened by the shared class device.
 */
static
NTSTATUS
NTAPI
MousePortNotification(
    _In_ PVOID NotificationStructure,
    _Inout_opt_ PVOID Context)
{
    PTARGET_DEVICE_REMOVAL_NOTIFICATION Notification = NotificationStructure;
    PMOUCLASS_EXTENSION Port = Context;
    PVOID Handle;

    /* Let a query remove succeed by closing the port stack */
    if (IsEqualGUID(&Notification->Event, &GUID_TARGET_DEVICE_QUERY_REMOVE))
        return MouseSwitchGlobalPort(Port, FALSE);

    if (IsEqualGUID(&Notification->Event, &GUID_TARGET_DEVICE_REMOVE_COMPLETE))
    {
        Handle = InterlockedExchangePointer(&Port->TargetNotifyHandle, NULL);
        if (Handle == NULL)
            return STATUS_SUCCESS;

        IoUnregisterPlugPlayNotification(Handle);
        return MouseSwitchGlobalPort(Port, FALSE);
    }

    if (IsEqualGUID(&Notification->Event, &GUID_TARGET_DEVICE_REMOVE_CANCELLED))
    {
        Handle = InterlockedExchangePointer(&Port->TargetNotifyHandle, NULL);
        if (Handle == NULL)
            return STATUS_SUCCESS;

        IoUnregisterPlugPlayNotification(Handle);
        return MouseSwitchGlobalPort(Port, TRUE);
    }

    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
MouseCreate(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PMOUCLASS_EXTENSION Extension = DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    BOOLEAN IsAccessForced = (Stack->Flags & SL_FORCE_ACCESS_CHECK) != 0;
    KPROCESSOR_MODE CheckMode;
    KIRQL OldIrql;
    NTSTATUS Status;

    /* Only kernel mode callers may open for reading */
    if ((Irp->RequestorMode == UserMode || IsAccessForced) &&
        (Stack->Parameters.Create.SecurityContext->DesiredAccess & FILE_READ_DATA))
    {
        return MouseCompleteIrp(Irp, STATUS_ACCESS_DENIED);
    }

    Status = IoAcquireRemoveLock(&Extension->RemoveLock, Irp);
    if (!NT_SUCCESS(Status))
        return MouseCompleteIrp(Irp, Status);

    if (Extension->IsPnp && !Extension->IsStarted)
    {
        IoReleaseRemoveLock(&Extension->RemoveLock, Irp);
        return MouseCompleteIrp(Irp, STATUS_UNSUCCESSFUL);
    }

    if (Extension->Self == Extension->ClassDevice)
    {
        CheckMode = IsAccessForced ? UserMode : Irp->RequestorMode;
        if (SeSinglePrivilegeCheck(RtlConvertLongToLuid(SE_TCB_PRIVILEGE), CheckMode))
        {
            KeAcquireSpinLock(&Extension->QueueLock, &OldIrql);
            Stack->FileObject->FsContext2 = MOUCLASS_TRUSTED_FILE;
            KeReleaseSpinLock(&Extension->QueueLock, OldIrql);
        }
    }

    ExAcquireFastMutex(&MouGlobals.Lock);

    if (Extension == MouGlobals.SharedClass)
    {
        /* The first open of the shared device turns every port on */
        if (++MouGlobals.SharedOpens == 1)
            Status = MouseSwitchSharedPorts(DeviceObject, Irp, TRUE, Status);

        ExReleaseFastMutex(&MouGlobals.Lock);
    }
    else
    {
        ExReleaseFastMutex(&MouGlobals.Lock);

        /* A port stack below the shared device is opened through its own PDO name */
        if (Extension->ClassDevice != DeviceObject)
        {
            IoSkipCurrentIrpStackLocation(Irp);
            Status = IoCallDriver(Extension->LowerDevice, Irp);
            IoReleaseRemoveLock(&Extension->RemoveLock, Irp);
            return Status;
        }

        Status = MouseSwitchPort(TRUE, Irp, Extension, &Stack->FileObject);
        if (Status != STATUS_SUCCESS)
            DPRINT1("Port refused the open, 0x%08lx\n", Status);
    }

    IoReleaseRemoveLock(&Extension->RemoveLock, Irp);
    return MouseCompleteIrp(Irp, Status);
}

NTSTATUS
NTAPI
MouseCleanup(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PMOUCLASS_EXTENSION Extension = DeviceObject->DeviceExtension;
    PFILE_OBJECT FileObject = IoGetCurrentIrpStackLocation(Irp)->FileObject;

    if (FileObject->FsContext2 == MOUCLASS_TRUSTED_FILE)
        MouseCancelQueuedReads(Extension, FileObject);

    return MouseCompleteIrp(Irp, STATUS_SUCCESS);
}

NTSTATUS
NTAPI
MouseClose(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PMOUCLASS_EXTENSION Extension = DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS Status = STATUS_SUCCESS;
    KIRQL OldIrql;

    if (Extension->Self == Extension->ClassDevice)
    {
        KeAcquireSpinLock(&Extension->QueueLock, &OldIrql);
        if (Stack->FileObject->FsContext2 == MOUCLASS_TRUSTED_FILE)
            Stack->FileObject->FsContext2 = NULL;
        KeReleaseSpinLock(&Extension->QueueLock, OldIrql);
    }

    ExAcquireFastMutex(&MouGlobals.Lock);

    if (Extension == MouGlobals.SharedClass)
    {
        /* The last close of the shared device turns every port off */
        if (--MouGlobals.SharedOpens == 0)
            Status = MouseSwitchSharedPorts(DeviceObject, Irp, FALSE, Status);

        ExReleaseFastMutex(&MouGlobals.Lock);
    }
    else
    {
        ExReleaseFastMutex(&MouGlobals.Lock);

        if (Extension->ClassDevice != DeviceObject)
        {
            IoSkipCurrentIrpStackLocation(Irp);
            return IoCallDriver(Extension->LowerDevice, Irp);
        }

        Status = MouseSwitchPort(FALSE, Irp, Extension, &Stack->FileObject);
        if (Status != STATUS_SUCCESS)
        {
            MouseLogError(DeviceObject, MOUCLASS_EVT_PORT_CLOSE_FAILED, 20120, Status,
                          0, NULL, Stack->MajorFunction);
        }
    }

    return MouseCompleteIrp(Irp, Status);
}

NTSTATUS
NTAPI
MouseRead(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PMOUCLASS_EXTENSION Extension = DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    ULONG Length = Stack->Parameters.Read.Length;
    KIRQL OldIrql;
    NTSTATUS Status;

    if (Length == 0)
        return MouseCompleteIrp(Irp, STATUS_SUCCESS);

    if (Length % MOUCLASS_PACKET_SIZE != 0)
        return MouseCompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL);

    if (Extension->IsSurpriseRemoved)
        return MouseCompleteIrp(Irp, STATUS_DEVICE_NOT_CONNECTED);

    if (Stack->FileObject->FsContext2 != MOUCLASS_TRUSTED_FILE)
        return MouseCompleteIrp(Irp, STATUS_PRIVILEGE_NOT_HELD);

    Status = IoAcquireRemoveLock(&Extension->RemoveLock, Irp);
    if (!NT_SUCCESS(Status))
        return MouseCompleteIrp(Irp, Status);

    Irp->IoStatus.Status = STATUS_PENDING;
    Irp->IoStatus.Information = 0;

    KeAcquireSpinLock(&Extension->QueueLock, &OldIrql);

    if (Extension->QueueCount != 0)
    {
        MouseFillReadFromQueue(Extension, Irp);
        KeReleaseSpinLock(&Extension->QueueLock, OldIrql);

        Status = Irp->IoStatus.Status;
        IoReleaseRemoveLock(&Extension->RemoveLock, Irp);
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return Status;
    }

    InsertTailList(&Extension->ReadQueue, &Irp->Tail.Overlay.ListEntry);
    IoSetCancelRoutine(Irp, MouseCancelRead);

    if (Irp->Cancel && IoSetCancelRoutine(Irp, NULL) != NULL)
    {
        RemoveEntryList(&Irp->Tail.Overlay.ListEntry);
        KeReleaseSpinLock(&Extension->QueueLock, OldIrql);

        Irp->IoStatus.Status = STATUS_CANCELLED;
        IoReleaseRemoveLock(&Extension->RemoveLock, Irp);
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_CANCELLED;
    }

    IoMarkIrpPending(Irp);
    KeReleaseSpinLock(&Extension->QueueLock, OldIrql);
    return STATUS_PENDING;
}

NTSTATUS
NTAPI
MouseFlush(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PMOUCLASS_EXTENSION Extension = DeviceObject->DeviceExtension;
    PFILE_OBJECT FileObject = IoGetCurrentIrpStackLocation(Irp)->FileObject;

    if (Extension->Self != Extension->ClassDevice)
        return MouseCompleteIrp(Irp, STATUS_NOT_SUPPORTED);

    if (FileObject->FsContext2 != MOUCLASS_TRUSTED_FILE)
        return MouseCompleteIrp(Irp, STATUS_PRIVILEGE_NOT_HELD);

    MouseResetQueue(Extension);
    return MouseCompleteIrp(Irp, STATUS_SUCCESS);
}

/**
 * @brief
 * Picks the shared port that answers IOCTL_MOUSE_QUERY_ATTRIBUTES. MouGlobals.Lock must be held.
 *
 * @return
 * Index of an enabled port, the requested one if it is usable, or MAXULONG.
 */
static
ULONG
NTAPI
MouseFindAttributesPort(
    _In_ ULONG Requested)
{
    ULONG Index;

    if (Requested >= MouGlobals.PortCount)
        return MAXULONG;

    if (MouGlobals.Ports[Requested].Enabled && !MouGlobals.Ports[Requested].Free)
        return Requested;

    for (Index = 0; Index < MouGlobals.PortCount; Index++)
    {
        if (MouGlobals.Ports[Index].Enabled && !MouGlobals.Ports[Index].Free)
            return Index;
    }

    return MAXULONG;
}

static
BOOLEAN
NTAPI
MouseIsPortRequest(
    _In_ ULONG IoControlCode)
{
    switch (IoControlCode)
    {
        case IOCTL_HID_GET_DRIVER_CONFIG:
        case IOCTL_HID_SET_DRIVER_CONFIG:
        case IOCTL_HID_GET_POLL_FREQUENCY_MSEC:
        case IOCTL_HID_SET_POLL_FREQUENCY_MSEC:
        case IOCTL_GET_NUM_DEVICE_INPUT_BUFFERS:
        case IOCTL_SET_NUM_DEVICE_INPUT_BUFFERS:
        case IOCTL_HID_GET_COLLECTION_INFORMATION:
        case IOCTL_HID_GET_COLLECTION_DESCRIPTOR:
        case IOCTL_HID_FLUSH_QUEUE:
        case IOCTL_HID_SET_FEATURE:
        case IOCTL_HID_GET_FEATURE:
        case IOCTL_GET_PHYSICAL_DESCRIPTOR:
        case IOCTL_HID_GET_HARDWARE_ID:
        case IOCTL_HID_GET_MANUFACTURER_STRING:
        case IOCTL_HID_GET_PRODUCT_STRING:
        case IOCTL_HID_GET_SERIALNUMBER_STRING:
        case IOCTL_HID_GET_INDEXED_STRING:
        case MOUCLASS_HID_SUPPRESS_INPUT:
        case IOCTL_GET_SYS_BUTTON_CAPS:
        case IOCTL_GET_SYS_BUTTON_EVENT:
            return TRUE;

        default:
            return FALSE;
    }
}

NTSTATUS
NTAPI
MouseDeviceControl(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PMOUCLASS_EXTENSION Extension = DeviceObject->DeviceExtension;
    PMOUCLASS_EXTENSION Target = Extension;
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    PIO_STACK_LOCATION Next;
    PFILE_OBJECT PortFile = NULL;
    ULONG IoControlCode = Stack->Parameters.DeviceIoControl.IoControlCode;
    ULONG Unit = 0;
    NTSTATUS Status;

    Status = IoAcquireRemoveLock(&Extension->RemoveLock, Irp);
    if (!NT_SUCCESS(Status))
        return MouseCompleteIrp(Irp, Status);

    if (MouseIsPortRequest(IoControlCode))
    {
        /* HID and system button requests belong to the PnP port stack */
        if (!Extension->IsPnp || Extension == MouGlobals.SharedClass)
        {
            Status = MouseCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST);
        }
        else
        {
            IoSkipCurrentIrpStackLocation(Irp);
            Status = IoCallDriver(Extension->LowerDevice, Irp);
        }

        IoReleaseRemoveLock(&Extension->RemoveLock, Irp);
        return Status;
    }

    if (IoControlCode != IOCTL_MOUSE_QUERY_ATTRIBUTES)
    {
        Status = MouseCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST);
        IoReleaseRemoveLock(&Extension->RemoveLock, Irp);
        return Status;
    }

    /* An optional USHORT input picks the port of the shared device */
    if (Stack->Parameters.DeviceIoControl.InputBufferLength != 0)
    {
        if (Stack->Parameters.DeviceIoControl.InputBufferLength < sizeof(USHORT))
        {
            Status = MouseCompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL);
            IoReleaseRemoveLock(&Extension->RemoveLock, Irp);
            return Status;
        }

        Unit = *(PUSHORT)Irp->AssociatedIrp.SystemBuffer;
    }

    if (Extension->Self != Extension->ClassDevice)
    {
        Status = MouseCompleteIrp(Irp, STATUS_NOT_SUPPORTED);
        IoReleaseRemoveLock(&Extension->RemoveLock, Irp);
        return Status;
    }

    if (Extension == MouGlobals.SharedClass)
    {
        ExAcquireFastMutex(&MouGlobals.Lock);
        Unit = MouseFindAttributesPort(Unit);
        if (Unit == MAXULONG)
        {
            ExReleaseFastMutex(&MouGlobals.Lock);
            Status = MouseCompleteIrp(Irp, STATUS_INVALID_PARAMETER);
            IoReleaseRemoveLock(&Extension->RemoveLock, Irp);
            return Status;
        }

        Target = MouGlobals.Ports[Unit].Port;
        PortFile = MouGlobals.Ports[Unit].FileObject;
        ExReleaseFastMutex(&MouGlobals.Lock);
    }

    /* The port answers the attribute query as an internal request */
    IoCopyCurrentIrpStackLocationToNext(Irp);
    Next = IoGetNextIrpStackLocation(Irp);
    Next->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
    if (Target != Extension)
        Next->FileObject = PortFile;

    Status = IoCallDriver(Target->LowerDevice, Irp);
    IoReleaseRemoveLock(&Extension->RemoveLock, Irp);
    return Status;
}

NTSTATUS
NTAPI
MousePassThrough(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PMOUCLASS_EXTENSION Extension = DeviceObject->DeviceExtension;

    /* The shared class device has no stack below it */
    if (Extension->LowerDevice == NULL)
        return MouseCompleteIrp(Irp, STATUS_NOT_SUPPORTED);

    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(Extension->LowerDevice, Irp);
}
