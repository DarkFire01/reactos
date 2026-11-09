/*
 * PROJECT:     ReactOS MSRil Broker
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Routes RIL records between rilproxy clients and the RIL host service
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "msril.h"
#include <ntstrsafe.h>

#define NDEBUG
#include <debug.h>

static LONG MsRilInstanceCount;

DRIVER_INITIALIZE DriverEntry;
static DRIVER_ADD_DEVICE MsRilAddDevice;
static DRIVER_UNLOAD MsRilUnload;
static DRIVER_CANCEL MsRilCancel;
static DRIVER_DISPATCH MsRilCreate;
static DRIVER_DISPATCH MsRilCleanup;
static DRIVER_DISPATCH MsRilClose;
static DRIVER_DISPATCH MsRilRead;
static DRIVER_DISPATCH MsRilWrite;
static DRIVER_DISPATCH MsRilDeviceControl;
static DRIVER_DISPATCH MsRilPnp;
static DRIVER_DISPATCH MsRilPower;
static DRIVER_DISPATCH MsRilSystemControl;

static
NTSTATUS
MsRilCompleteIrp(
    _In_ PIRP Irp,
    _In_ NTSTATUS Status,
    _In_ ULONG_PTR Information)
{
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = Information;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

/* Completes every IRP gathered while the lock was held */
static
VOID
MsRilCompleteList(
    _Inout_ PLIST_ENTRY List)
{
    PIRP Irp;

    while (!IsListEmpty(List))
    {
        Irp = CONTAINING_RECORD(RemoveHeadList(List), IRP, Tail.Overlay.ListEntry);
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
    }
}

static
PMSRIL_RECORD
MsRilAllocateRecord(
    _In_ ULONG DataLength)
{
    PMSRIL_RECORD Record;

    Record = ExAllocatePoolWithTag(NonPagedPool,
                                   FIELD_OFFSET(MSRIL_RECORD, Data) + DataLength,
                                   TAG_MSRIL);
    if (!Record)
        return NULL;

    RtlZeroMemory(Record, FIELD_OFFSET(MSRIL_RECORD, Data));
    Record->Length = DataLength;
    Record->Event.DataLength = DataLength;
    return Record;
}

static
VOID
MsRilFreeRecords(
    _Inout_ PMSRIL_FILE File)
{
    PMSRIL_RECORD Record;

    while (!IsListEmpty(&File->Records))
    {
        Record = CONTAINING_RECORD(RemoveHeadList(&File->Records), MSRIL_RECORD, Link);
        ExFreePoolWithTag(Record, TAG_MSRIL);
    }
    File->RecordCount = 0;
}

/* Caller holds the device lock */
static
NTSTATUS
MsRilPendIrp(
    _In_ PMSRIL_FILE File,
    _In_ PIRP Irp)
{
    IoSetCancelRoutine(Irp, MsRilCancel);
    if (Irp->Cancel && IoSetCancelRoutine(Irp, NULL))
        return STATUS_CANCELLED;

    /* If the cancel routine already owns it, it will find the IRP on the list */
    IoMarkIrpPending(Irp);
    InsertTailList(&File->Irps, &Irp->Tail.Overlay.ListEntry);
    return STATUS_PENDING;
}

/* Caller holds the device lock */
static
PIRP
MsRilPopIrp(
    _In_ PMSRIL_FILE File)
{
    PLIST_ENTRY Entry;
    PIRP Irp;

    while (!IsListEmpty(&File->Irps))
    {
        Entry = RemoveHeadList(&File->Irps);
        Irp = CONTAINING_RECORD(Entry, IRP, Tail.Overlay.ListEntry);
        if (IoSetCancelRoutine(Irp, NULL))
            return Irp;

        /* Being cancelled, let the cancel routine unlink a harmless self pointing entry */
        InitializeListHead(Entry);
    }

    return NULL;
}

static
VOID
NTAPI
MsRilCancel(
    _Inout_ PDEVICE_OBJECT DeviceObject,
    _Inout_ _IRQL_uses_cancel_ PIRP Irp)
{
    PMSRIL_EXTENSION Extension = DeviceObject->DeviceExtension;
    KIRQL OldIrql;

    IoReleaseCancelSpinLock(Irp->CancelIrql);

    KeAcquireSpinLock(&Extension->Lock, &OldIrql);
    RemoveEntryList(&Irp->Tail.Overlay.ListEntry);
    KeReleaseSpinLock(&Extension->Lock, OldIrql);

    MsRilCompleteIrp(Irp, STATUS_CANCELLED, 0);
}

/* Fails every queued IRP of a file. Caller holds the device lock. */
static
VOID
MsRilFlushIrps(
    _In_ PMSRIL_FILE File,
    _In_ NTSTATUS Status,
    _Inout_ PLIST_ENTRY CompleteList)
{
    PIRP Irp;

    while ((Irp = MsRilPopIrp(File)) != NULL)
    {
        Irp->IoStatus.Status = Status;
        Irp->IoStatus.Information = 0;
        InsertTailList(CompleteList, &Irp->Tail.Overlay.ListEntry);
    }
}

/* Tries to satisfy a client read from its queue. Caller holds the device lock. */
static
BOOLEAN
MsRilFillClientRead(
    _In_ PMSRIL_FILE File,
    _In_ PIRP Irp)
{
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    PMSRIL_RECORD Record;

    if (IsListEmpty(&File->Records))
    {
        if (!File->Orphaned)
            return FALSE;

        Irp->IoStatus.Status = STATUS_DEVICE_NOT_READY;
        Irp->IoStatus.Information = 0;
        return TRUE;
    }

    Record = CONTAINING_RECORD(File->Records.Flink, MSRIL_RECORD, Link);
    if (Stack->Parameters.Read.Length < Record->Length)
    {
        /* The record stays queued, the client retries with a larger buffer */
        Irp->IoStatus.Status = STATUS_BUFFER_TOO_SMALL;
        Irp->IoStatus.Information = 0;
        return TRUE;
    }

    RtlCopyMemory(Irp->AssociatedIrp.SystemBuffer, Record->Data, Record->Length);
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = Record->Length;

    RemoveEntryList(&Record->Link);
    File->RecordCount--;
    ExFreePoolWithTag(Record, TAG_MSRIL);
    return TRUE;
}

/* Tries to satisfy a host event fetch. Caller holds the device lock. */
static
BOOLEAN
MsRilFillHostFetch(
    _In_ PMSRIL_FILE Host,
    _In_ PIRP Irp)
{
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    PMSRIL_RECORD Record;
    PUCHAR Output = Irp->AssociatedIrp.SystemBuffer;
    ULONG Needed;

    if (IsListEmpty(&Host->Records))
        return FALSE;

    Record = CONTAINING_RECORD(Host->Records.Flink, MSRIL_RECORD, Link);
    Needed = sizeof(MSRIL_HOST_EVENT) + Record->Length;
    if (Stack->Parameters.DeviceIoControl.OutputBufferLength < Needed)
    {
        Irp->IoStatus.Status = STATUS_BUFFER_TOO_SMALL;
        Irp->IoStatus.Information = 0;
        return TRUE;
    }

    RtlCopyMemory(Output, &Record->Event, sizeof(MSRIL_HOST_EVENT));
    RtlCopyMemory(Output + sizeof(MSRIL_HOST_EVENT), Record->Data, Record->Length);
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = Needed;

    RemoveEntryList(&Record->Link);
    Host->RecordCount--;
    ExFreePoolWithTag(Record, TAG_MSRIL);
    return TRUE;
}

/* Queues a record on a file and wakes waiting IRPs. Caller holds the device lock. */
static
VOID
MsRilPostRecord(
    _In_ PMSRIL_FILE File,
    _In_ PMSRIL_RECORD Record,
    _Inout_ PLIST_ENTRY CompleteList)
{
    PIRP Irp;
    BOOLEAN Done;

    InsertTailList(&File->Records, &Record->Link);
    File->RecordCount++;

    while (!IsListEmpty(&File->Records) && (Irp = MsRilPopIrp(File)) != NULL)
    {
        if (File->Role == MsRilRoleHost)
            Done = MsRilFillHostFetch(File, Irp);
        else
            Done = MsRilFillClientRead(File, Irp);

        ASSERT(Done);
        InsertTailList(CompleteList, &Irp->Tail.Overlay.ListEntry);
    }
}

static
VOID
MsRilSetInterface(
    _In_ PMSRIL_EXTENSION Extension,
    _In_ BOOLEAN Enable)
{
    NTSTATUS Status;

    if (!Extension->InterfaceName.Buffer || Extension->InterfaceEnabled == Enable)
        return;

    Status = IoSetDeviceInterfaceState(&Extension->InterfaceName, Enable);
    if (NT_SUCCESS(Status))
        Extension->InterfaceEnabled = Enable;
    else
        DPRINT1("IoSetDeviceInterfaceState(%u) failed 0x%lx\n", Enable, Status);
}

static
PMSRIL_FILE
MsRilFindClient(
    _In_ PMSRIL_EXTENSION Extension,
    _In_ ULONG ClientId)
{
    PLIST_ENTRY Entry;
    PMSRIL_FILE File;

    for (Entry = Extension->Clients.Flink; Entry != &Extension->Clients; Entry = Entry->Flink)
    {
        File = CONTAINING_RECORD(Entry, MSRIL_FILE, Link);
        if (File->ClientId == ClientId)
            return File;
    }

    return NULL;
}

static
NTSTATUS
NTAPI
MsRilCreate(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    static const UNICODE_STRING HostName = RTL_CONSTANT_STRING(MSRIL_HOST_FILE_NAME);
    PMSRIL_EXTENSION Extension = DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    PFILE_OBJECT FileObject = Stack->FileObject;
    PMSRIL_RECORD Opened = NULL;
    LIST_ENTRY CompleteList;
    PMSRIL_FILE File;
    BOOLEAN IsHost;
    NTSTATUS Status = STATUS_SUCCESS;
    KIRQL OldIrql;

    if (Extension->Removed)
        return MsRilCompleteIrp(Irp, STATUS_NO_SUCH_DEVICE, 0);

    IsHost = RtlEqualUnicodeString(&FileObject->FileName, &HostName, TRUE);
    if (!IsHost && FileObject->FileName.Length != 0)
        return MsRilCompleteIrp(Irp, STATUS_OBJECT_NAME_NOT_FOUND, 0);

    /* Only a trusted service may act as the RIL host */
    if (IsHost && Irp->RequestorMode != KernelMode &&
        !SeSinglePrivilegeCheck(RtlConvertLongToLuid(SE_TCB_PRIVILEGE), Irp->RequestorMode))
    {
        return MsRilCompleteIrp(Irp, STATUS_PRIVILEGE_NOT_HELD, 0);
    }

    File = ExAllocatePoolWithTag(NonPagedPool, sizeof(*File), TAG_MSRIL);
    if (!File)
        return MsRilCompleteIrp(Irp, STATUS_INSUFFICIENT_RESOURCES, 0);

    RtlZeroMemory(File, sizeof(*File));
    File->Role = IsHost ? MsRilRoleHost : MsRilRoleClient;
    File->ProcessId = HandleToUlong(PsGetCurrentProcessId());
    InitializeListHead(&File->Link);
    InitializeListHead(&File->Records);
    InitializeListHead(&File->Irps);

    if (!IsHost)
    {
        Opened = MsRilAllocateRecord(0);
        if (!Opened)
        {
            ExFreePoolWithTag(File, TAG_MSRIL);
            return MsRilCompleteIrp(Irp, STATUS_INSUFFICIENT_RESOURCES, 0);
        }
    }

    InitializeListHead(&CompleteList);
    KeAcquireSpinLock(&Extension->Lock, &OldIrql);
    if (IsHost)
    {
        if (Extension->Host)
            Status = STATUS_SHARING_VIOLATION;
        else
            Extension->Host = File;
    }
    else if (!Extension->Host)
    {
        Status = STATUS_DEVICE_NOT_READY;
    }
    else
    {
        do
        {
            File->ClientId = ++Extension->NextClientId;
        } while (File->ClientId == 0 || MsRilFindClient(Extension, File->ClientId));

        InsertTailList(&Extension->Clients, &File->Link);

        Opened->Event.EventType = MsRilClientOpened;
        Opened->Event.ClientId = File->ClientId;
        Opened->Event.ProcessId = File->ProcessId;
        MsRilPostRecord(Extension->Host, Opened, &CompleteList);
        Opened = NULL;
    }
    KeReleaseSpinLock(&Extension->Lock, OldIrql);
    MsRilCompleteList(&CompleteList);

    if (Opened)
        ExFreePoolWithTag(Opened, TAG_MSRIL);

    if (!NT_SUCCESS(Status))
    {
        ExFreePoolWithTag(File, TAG_MSRIL);
        return MsRilCompleteIrp(Irp, Status, 0);
    }

    FileObject->FsContext = File;
    if (IsHost)
        MsRilSetInterface(Extension, TRUE);

    return MsRilCompleteIrp(Irp, STATUS_SUCCESS, FILE_OPENED);
}

static
NTSTATUS
NTAPI
MsRilCleanup(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PMSRIL_EXTENSION Extension = DeviceObject->DeviceExtension;
    PMSRIL_FILE File = IoGetCurrentIrpStackLocation(Irp)->FileObject->FsContext;
    PMSRIL_RECORD Closed;
    LIST_ENTRY CompleteList;
    PLIST_ENTRY Entry;
    PMSRIL_FILE Client;
    KIRQL OldIrql;

    if (!File)
        return MsRilCompleteIrp(Irp, STATUS_SUCCESS, 0);

    Closed = (File->Role == MsRilRoleClient) ? MsRilAllocateRecord(0) : NULL;

    InitializeListHead(&CompleteList);
    KeAcquireSpinLock(&Extension->Lock, &OldIrql);
    MsRilFlushIrps(File, STATUS_CANCELLED, &CompleteList);
    MsRilFreeRecords(File);

    if (File->Role == MsRilRoleHost)
    {
        Extension->Host = NULL;

        /* Every client served by this host is cut off, they reconnect to the next one */
        while (!IsListEmpty(&Extension->Clients))
        {
            Entry = RemoveHeadList(&Extension->Clients);
            InitializeListHead(Entry);
            Client = CONTAINING_RECORD(Entry, MSRIL_FILE, Link);
            Client->Orphaned = TRUE;
            if (IsListEmpty(&Client->Records))
                MsRilFlushIrps(Client, STATUS_DEVICE_NOT_READY, &CompleteList);
        }
    }
    else
    {
        if (!IsListEmpty(&File->Link))
        {
            RemoveEntryList(&File->Link);
            InitializeListHead(&File->Link);
        }

        if (!File->Orphaned && Extension->Host && Closed)
        {
            Closed->Event.EventType = MsRilClientClosed;
            Closed->Event.ClientId = File->ClientId;
            Closed->Event.ProcessId = File->ProcessId;
            MsRilPostRecord(Extension->Host, Closed, &CompleteList);
            Closed = NULL;
        }
        File->Orphaned = TRUE;
    }
    KeReleaseSpinLock(&Extension->Lock, OldIrql);
    MsRilCompleteList(&CompleteList);

    if (Closed)
        ExFreePoolWithTag(Closed, TAG_MSRIL);

    if (File->Role == MsRilRoleHost)
        MsRilSetInterface(Extension, FALSE);

    return MsRilCompleteIrp(Irp, STATUS_SUCCESS, 0);
}

static
NTSTATUS
NTAPI
MsRilClose(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PFILE_OBJECT FileObject = IoGetCurrentIrpStackLocation(Irp)->FileObject;

    UNREFERENCED_PARAMETER(DeviceObject);

    if (FileObject->FsContext)
    {
        ExFreePoolWithTag(FileObject->FsContext, TAG_MSRIL);
        FileObject->FsContext = NULL;
    }

    return MsRilCompleteIrp(Irp, STATUS_SUCCESS, 0);
}

static
NTSTATUS
NTAPI
MsRilRead(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PMSRIL_EXTENSION Extension = DeviceObject->DeviceExtension;
    PMSRIL_FILE File = IoGetCurrentIrpStackLocation(Irp)->FileObject->FsContext;
    NTSTATUS Status;
    KIRQL OldIrql;

    if (!File || File->Role != MsRilRoleClient)
        return MsRilCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);

    KeAcquireSpinLock(&Extension->Lock, &OldIrql);
    if (MsRilFillClientRead(File, Irp))
    {
        KeReleaseSpinLock(&Extension->Lock, OldIrql);
        Status = Irp->IoStatus.Status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return Status;
    }

    Status = MsRilPendIrp(File, Irp);
    KeReleaseSpinLock(&Extension->Lock, OldIrql);

    if (Status == STATUS_CANCELLED)
        return MsRilCompleteIrp(Irp, STATUS_CANCELLED, 0);

    return Status;
}

static
NTSTATUS
NTAPI
MsRilWrite(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PMSRIL_EXTENSION Extension = DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    PMSRIL_FILE File = Stack->FileObject->FsContext;
    ULONG Length = Stack->Parameters.Write.Length;
    LIST_ENTRY CompleteList;
    PMSRIL_RECORD Record;
    NTSTATUS Status = STATUS_SUCCESS;
    KIRQL OldIrql;

    if (!File || File->Role != MsRilRoleClient)
        return MsRilCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);

    if (Length < sizeof(RIL_COMMAND_RECORD) || Length > RIL_RECORD_MAX_LENGTH)
        return MsRilCompleteIrp(Irp, STATUS_INVALID_BUFFER_SIZE, 0);

    Record = MsRilAllocateRecord(Length);
    if (!Record)
        return MsRilCompleteIrp(Irp, STATUS_INSUFFICIENT_RESOURCES, 0);

    RtlCopyMemory(Record->Data, Irp->AssociatedIrp.SystemBuffer, Length);
    Record->Event.EventType = MsRilClientRecord;
    Record->Event.ClientId = File->ClientId;
    Record->Event.ProcessId = File->ProcessId;

    InitializeListHead(&CompleteList);
    KeAcquireSpinLock(&Extension->Lock, &OldIrql);
    if (File->Orphaned || !Extension->Host)
    {
        Status = STATUS_DEVICE_NOT_READY;
    }
    else
    {
        MsRilPostRecord(Extension->Host, Record, &CompleteList);
        Record = NULL;
    }
    KeReleaseSpinLock(&Extension->Lock, OldIrql);
    MsRilCompleteList(&CompleteList);

    if (Record)
        ExFreePoolWithTag(Record, TAG_MSRIL);

    return MsRilCompleteIrp(Irp, Status, NT_SUCCESS(Status) ? Length : 0);
}

static
NTSTATUS
MsRilHostDeliver(
    _In_ PMSRIL_EXTENSION Extension,
    _Inout_ PIRP Irp)
{
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    ULONG InputLength = Stack->Parameters.DeviceIoControl.InputBufferLength;
    PMSRIL_HOST_DELIVERY Delivery = Irp->AssociatedIrp.SystemBuffer;
    LIST_ENTRY CompleteList;
    PMSRIL_RECORD Record;
    PMSRIL_FILE Client;
    NTSTATUS Status = STATUS_SUCCESS;
    KIRQL OldIrql;

    if (InputLength < sizeof(*Delivery) ||
        Delivery->DataLength > InputLength - sizeof(*Delivery) ||
        Delivery->DataLength > RIL_RECORD_MAX_LENGTH ||
        Delivery->DataLength == 0)
    {
        return STATUS_INVALID_PARAMETER;
    }

    Record = MsRilAllocateRecord(Delivery->DataLength);
    if (!Record)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlCopyMemory(Record->Data, Delivery + 1, Delivery->DataLength);

    InitializeListHead(&CompleteList);
    KeAcquireSpinLock(&Extension->Lock, &OldIrql);
    Client = MsRilFindClient(Extension, Delivery->ClientId);
    if (!Client)
    {
        Status = STATUS_NOT_FOUND;
    }
    else if (Client->RecordCount >= MSRIL_MAX_CLIENT_QUEUE)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
    }
    else
    {
        MsRilPostRecord(Client, Record, &CompleteList);
        Record = NULL;
    }
    KeReleaseSpinLock(&Extension->Lock, OldIrql);
    MsRilCompleteList(&CompleteList);

    if (Record)
        ExFreePoolWithTag(Record, TAG_MSRIL);

    return Status;
}

static
NTSTATUS
NTAPI
MsRilDeviceControl(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PMSRIL_EXTENSION Extension = DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    PMSRIL_FILE File = Stack->FileObject->FsContext;
    NTSTATUS Status;
    KIRQL OldIrql;

    if (!File || File->Role != MsRilRoleHost)
        return MsRilCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);

    switch (Stack->Parameters.DeviceIoControl.IoControlCode)
    {
        case IOCTL_MSRIL_HOST_GET_EVENT:
        {
            if (Stack->Parameters.DeviceIoControl.OutputBufferLength < sizeof(MSRIL_HOST_EVENT))
                return MsRilCompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);

            KeAcquireSpinLock(&Extension->Lock, &OldIrql);
            if (MsRilFillHostFetch(File, Irp))
            {
                KeReleaseSpinLock(&Extension->Lock, OldIrql);
                Status = Irp->IoStatus.Status;
                IoCompleteRequest(Irp, IO_NO_INCREMENT);
                return Status;
            }

            Status = MsRilPendIrp(File, Irp);
            KeReleaseSpinLock(&Extension->Lock, OldIrql);

            if (Status == STATUS_CANCELLED)
                return MsRilCompleteIrp(Irp, STATUS_CANCELLED, 0);
            return Status;
        }

        case IOCTL_MSRIL_HOST_DELIVER:
            Status = MsRilHostDeliver(Extension, Irp);
            return MsRilCompleteIrp(Irp, Status, 0);

        default:
            return MsRilCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    }
}

static
NTSTATUS
NTAPI
MsRilPnp(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PMSRIL_EXTENSION Extension = DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    BOOLEAN HostAttached;
    NTSTATUS Status;
    KIRQL OldIrql;

    switch (Stack->MinorFunction)
    {
        case IRP_MN_START_DEVICE:
        {
            Status = IoForwardIrpSynchronously(Extension->Lower, Irp) ? Irp->IoStatus.Status
                                                                       : STATUS_UNSUCCESSFUL;
            if (NT_SUCCESS(Status) && !Extension->InterfaceName.Buffer)
            {
                Status = IoRegisterDeviceInterface(Extension->Pdo,
                                                   &GUID_DEVINTERFACE_RIL_CLIENT,
                                                   NULL,
                                                   &Extension->InterfaceName);
            }

            if (NT_SUCCESS(Status))
            {
                KeAcquireSpinLock(&Extension->Lock, &OldIrql);
                HostAttached = (Extension->Host != NULL);
                KeReleaseSpinLock(&Extension->Lock, OldIrql);
                if (HostAttached)
                    MsRilSetInterface(Extension, TRUE);
            }

            return MsRilCompleteIrp(Irp, Status, 0);
        }

        case IRP_MN_SURPRISE_REMOVAL:
            MsRilSetInterface(Extension, FALSE);
            Irp->IoStatus.Status = STATUS_SUCCESS;
            break;

        case IRP_MN_REMOVE_DEVICE:
        {
            MsRilSetInterface(Extension, FALSE);
            Extension->Removed = TRUE;

            Irp->IoStatus.Status = STATUS_SUCCESS;
            IoSkipCurrentIrpStackLocation(Irp);
            Status = IoCallDriver(Extension->Lower, Irp);

            IoDeleteSymbolicLink(&Extension->LinkName);
            RtlFreeUnicodeString(&Extension->InterfaceName);
            IoDetachDevice(Extension->Lower);
            IoDeleteDevice(DeviceObject);
            return Status;
        }

        case IRP_MN_QUERY_REMOVE_DEVICE:
        case IRP_MN_CANCEL_REMOVE_DEVICE:
        case IRP_MN_QUERY_STOP_DEVICE:
        case IRP_MN_CANCEL_STOP_DEVICE:
        case IRP_MN_STOP_DEVICE:
            Irp->IoStatus.Status = STATUS_SUCCESS;
            break;

        default:
            break;
    }

    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(Extension->Lower, Irp);
}

static
NTSTATUS
NTAPI
MsRilPower(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PMSRIL_EXTENSION Extension = DeviceObject->DeviceExtension;

    PoStartNextPowerIrp(Irp);
    IoSkipCurrentIrpStackLocation(Irp);
    return PoCallDriver(Extension->Lower, Irp);
}

static
NTSTATUS
NTAPI
MsRilSystemControl(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PMSRIL_EXTENSION Extension = DeviceObject->DeviceExtension;

    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(Extension->Lower, Irp);
}

static
NTSTATUS
NTAPI
MsRilAddDevice(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PDEVICE_OBJECT PhysicalDeviceObject)
{
    WCHAR DeviceNameBuffer[32];
    UNICODE_STRING DeviceName;
    PMSRIL_EXTENSION Extension;
    PDEVICE_OBJECT DeviceObject;
    ULONG Instance;
    NTSTATUS Status;

    Instance = (ULONG)InterlockedIncrement(&MsRilInstanceCount);

    RtlInitEmptyUnicodeString(&DeviceName, DeviceNameBuffer, sizeof(DeviceNameBuffer));
    RtlUnicodeStringPrintf(&DeviceName, L"\\Device\\MSRil%lu", Instance);

    Status = IoCreateDevice(DriverObject,
                            sizeof(*Extension),
                            &DeviceName,
                            FILE_DEVICE_MSRIL,
                            FILE_DEVICE_SECURE_OPEN,
                            FALSE,
                            &DeviceObject);
    if (!NT_SUCCESS(Status))
        return Status;

    Extension = DeviceObject->DeviceExtension;
    RtlZeroMemory(Extension, sizeof(*Extension));
    Extension->Self = DeviceObject;
    Extension->Pdo = PhysicalDeviceObject;
    Extension->Instance = Instance;
    KeInitializeSpinLock(&Extension->Lock);
    InitializeListHead(&Extension->Clients);

    RtlInitEmptyUnicodeString(&Extension->LinkName,
                              Extension->LinkNameBuffer,
                              sizeof(Extension->LinkNameBuffer));
    RtlUnicodeStringPrintf(&Extension->LinkName, L"\\DosDevices\\MSRil%lu", Instance);

    Status = IoCreateSymbolicLink(&Extension->LinkName, &DeviceName);
    if (!NT_SUCCESS(Status))
    {
        IoDeleteDevice(DeviceObject);
        return Status;
    }

    Extension->Lower = IoAttachDeviceToDeviceStack(DeviceObject, PhysicalDeviceObject);
    if (!Extension->Lower)
    {
        IoDeleteSymbolicLink(&Extension->LinkName);
        IoDeleteDevice(DeviceObject);
        return STATUS_NO_SUCH_DEVICE;
    }

    DeviceObject->Flags |= DO_BUFFERED_IO | DO_POWER_PAGABLE;
    DeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}

static
VOID
NTAPI
MsRilUnload(
    _In_ PDRIVER_OBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject);
}

NTSTATUS
NTAPI
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    UNREFERENCED_PARAMETER(RegistryPath);

    DriverObject->MajorFunction[IRP_MJ_CREATE] = MsRilCreate;
    DriverObject->MajorFunction[IRP_MJ_CLEANUP] = MsRilCleanup;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = MsRilClose;
    DriverObject->MajorFunction[IRP_MJ_READ] = MsRilRead;
    DriverObject->MajorFunction[IRP_MJ_WRITE] = MsRilWrite;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = MsRilDeviceControl;
    DriverObject->MajorFunction[IRP_MJ_PNP] = MsRilPnp;
    DriverObject->MajorFunction[IRP_MJ_POWER] = MsRilPower;
    DriverObject->MajorFunction[IRP_MJ_SYSTEM_CONTROL] = MsRilSystemControl;
    DriverObject->DriverExtension->AddDevice = MsRilAddDevice;
    DriverObject->DriverUnload = MsRilUnload;

    return STATUS_SUCCESS;
}
