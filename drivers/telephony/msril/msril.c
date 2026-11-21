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

/* RIL_NOTIFY_NITZ, rilcmd.h is user mode only */
#define MSRIL_NOTIFY_NITZ       (RIL_RECORD_NOTIFY_FIRST | 0x115)

/* The RIL_NITZ_INFO payload after the notification header, rilinfo.h is user mode only */
typedef struct _MSRIL_NITZ
{
    RIL_NOTIFICATION_RECORD Header;
    ULONG Size;
    ULONG Params;
    ULONG NotificationCode;
    LONG TimeZoneOffsetMinutes;
    LONG DaylightSavingOffsetMinutes;
    USHORT Year;
    USHORT Month;
    USHORT DayOfWeek;
    USHORT Day;
    USHORT Hour;
    USHORT Minute;
    USHORT Second;
    USHORT Milliseconds;
} MSRIL_NITZ, *PMSRIL_NITZ;

/* Moves a cached network time forward by how long it waited for the client */
static
VOID
MsRilAgeNitz(
    _Inout_ PMSRIL_RECORD Record,
    _In_ ULONGLONG Arrived)
{
    PMSRIL_NITZ Nitz = (PMSRIL_NITZ)Record->Data;
    TIME_FIELDS Fields;
    LARGE_INTEGER Time;

    if (Record->Length < sizeof(*Nitz))
        return;

    Fields.Year = (CSHORT)Nitz->Year;
    Fields.Month = (CSHORT)Nitz->Month;
    Fields.Day = (CSHORT)Nitz->Day;
    Fields.Hour = (CSHORT)Nitz->Hour;
    Fields.Minute = (CSHORT)Nitz->Minute;
    Fields.Second = (CSHORT)Nitz->Second;
    Fields.Milliseconds = (CSHORT)Nitz->Milliseconds;
    Fields.Weekday = 0;
    if (!RtlTimeFieldsToTime(&Fields, &Time))
        return;

    Time.QuadPart += KeQueryInterruptTime() - Arrived;
    RtlTimeToTimeFields(&Time, &Fields);

    Nitz->Year = (USHORT)Fields.Year;
    Nitz->Month = (USHORT)Fields.Month;
    Nitz->DayOfWeek = (USHORT)Fields.Weekday;
    Nitz->Day = (USHORT)Fields.Day;
    Nitz->Hour = (USHORT)Fields.Hour;
    Nitz->Minute = (USHORT)Fields.Minute;
    Nitz->Second = (USHORT)Fields.Second;
    Nitz->Milliseconds = (USHORT)Fields.Milliseconds;
}

/* Bit N set means \Device\MSRil<N + 1> exists */
static LONG MsRilInstanceMask;

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

    Record = ExAllocatePoolWithTag(NonPagedPoolNx,
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
    File->QueuedBytes = 0;
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

        /* Being canceled, let the cancel routine unlink a harmless self pointing entry */
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

/* Fails every queued IRP of a file and returns how many. Caller holds the device lock. */
static
ULONG
MsRilFlushIrps(
    _In_ PMSRIL_FILE File,
    _In_ NTSTATUS Status,
    _Inout_ PLIST_ENTRY CompleteList)
{
    ULONG Count = 0;
    PIRP Irp;

    while ((Irp = MsRilPopIrp(File)) != NULL)
    {
        Irp->IoStatus.Status = Status;
        Irp->IoStatus.Information = 0;
        InsertTailList(CompleteList, &Irp->Tail.Overlay.ListEntry);
        Count++;
    }

    return Count;
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
        return FALSE;

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
    File->QueuedBytes -= Record->Length;
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
    Host->QueuedBytes -= Record->Length;
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
    File->QueuedBytes += Record->Length;

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

/* Cuts a client off: pending reads fail, queued records go. Caller holds the device lock. */
static
VOID
MsRilKillClient(
    _In_ PMSRIL_FILE Client,
    _In_ MSRIL_CLIENT_STATE State,
    _Inout_ PLIST_ENTRY CompleteList)
{
    ULONG Cancelled;

    Client->State = State;
    Cancelled = MsRilFlushIrps(Client, STATUS_CANCELLED, CompleteList);
    if (Cancelled || Client->RecordCount)
    {
        DPRINT1("msril: client %lu canceled %lu read(s), dropped %lu record(s)\n",
                Client->ClientId, Cancelled, Client->RecordCount);
    }
    MsRilFreeRecords(Client);
}

/* Hands a record to a client, takes ownership of it. Caller holds the device lock. */
static
VOID
MsRilQueueToClient(
    _In_ PMSRIL_FILE Client,
    _In_ PMSRIL_RECORD Record,
    _Inout_ PLIST_ENTRY CompleteList)
{
    if (Client->State != MsRilClientLive)
    {
        ExFreePoolWithTag(Record, TAG_MSRIL);
        return;
    }

    MsRilPostRecord(Client, Record, CompleteList);
    if (Client->QueuedBytes > MSRIL_CLIENT_BUDGET)
    {
        DPRINT1("msril: client %lu (pid %lu) has %lu bytes unread, cutting it off\n",
                Client->ClientId, Client->ProcessId, Client->QueuedBytes);
        MsRilKillClient(Client, MsRilClientOverBudget, CompleteList);
    }
}

/* Brings the client interface in line with the host and PnP state. Runs at PASSIVE_LEVEL. */
static
VOID
MsRilUpdateInterface(
    _In_ PMSRIL_EXTENSION Extension)
{
    BOOLEAN Wanted;
    NTSTATUS Status;
    KIRQL OldIrql;

    KeWaitForSingleObject(&Extension->InterfaceLock, Executive, KernelMode, FALSE, NULL);

    KeAcquireSpinLock(&Extension->Lock, &OldIrql);
    Wanted = Extension->Host && Extension->Started && !Extension->Removed;
    KeReleaseSpinLock(&Extension->Lock, OldIrql);

    if (Extension->InterfaceName.Buffer && Extension->InterfaceEnabled != Wanted)
    {
        Status = IoSetDeviceInterfaceState(&Extension->InterfaceName, Wanted);
        if (NT_SUCCESS(Status))
        {
            Extension->InterfaceEnabled = Wanted;
            DPRINT1("msril: MSRil%lu client interface %s\n",
                    Extension->Instance, Wanted ? "enabled" : "disabled");
        }
        else
        {
            DPRINT1("msril: MSRil%lu interface state %u failed 0x%lx\n",
                    Extension->Instance, Wanted, Status);
        }
    }

    KeSetEvent(&Extension->InterfaceLock, IO_NO_INCREMENT, FALSE);
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
VOID
MsRilFreeFile(
    _In_ PMSRIL_FILE File)
{
    if (File->Closed)
        ExFreePoolWithTag(File->Closed, TAG_MSRIL);
    ExFreePoolWithTag(File, TAG_MSRIL);
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
        DPRINT1("msril: host open from pid %lu refused, no TCB privilege\n",
                HandleToUlong(PsGetCurrentProcessId()));
        return MsRilCompleteIrp(Irp, STATUS_PRIVILEGE_NOT_HELD, 0);
    }

    File = ExAllocatePoolWithTag(NonPagedPoolNx, sizeof(*File), TAG_MSRIL);
    if (!File)
        return MsRilCompleteIrp(Irp, STATUS_INSUFFICIENT_RESOURCES, 0);

    RtlZeroMemory(File, sizeof(*File));
    File->Role = IsHost ? MsRilRoleHost : MsRilRoleClient;
    File->ProcessId = HandleToUlong(PsGetCurrentProcessId());
    File->State = MsRilClientLive;
    InitializeListHead(&File->Link);
    InitializeListHead(&File->Records);
    InitializeListHead(&File->Irps);

    if (!IsHost)
    {
        Opened = MsRilAllocateRecord(0);
        File->Closed = MsRilAllocateRecord(0);
        if (!Opened || !File->Closed)
        {
            if (Opened)
                ExFreePoolWithTag(Opened, TAG_MSRIL);
            MsRilFreeFile(File);
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
        Status = STATUS_INVALID_DEVICE_STATE;
    }
    else
    {
        do
        {
            File->ClientId = ++Extension->NextClientId;
        } while (File->ClientId == MSRIL_CLIENT_BROADCAST || MsRilFindClient(Extension, File->ClientId));

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
        DPRINT1("msril: %s open from pid %lu failed 0x%lx\n",
                IsHost ? "host" : "client", File->ProcessId, Status);
        MsRilFreeFile(File);
        return MsRilCompleteIrp(Irp, Status, 0);
    }

    FileObject->FsContext = File;
    InterlockedIncrement(&Extension->OpenHandles);

    if (IsHost)
    {
        DPRINT1("msril: host attached to MSRil%lu (pid %lu)\n", Extension->Instance, File->ProcessId);
        MsRilUpdateInterface(Extension);
    }
    else
    {
        DPRINT1("msril: client %lu opened (pid %lu)\n", File->ClientId, File->ProcessId);
    }

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
    LIST_ENTRY CompleteList;
    PLIST_ENTRY Entry;
    PMSRIL_FILE Client;
    ULONG Orphaned = 0;
    KIRQL OldIrql;

    if (!File)
        return MsRilCompleteIrp(Irp, STATUS_SUCCESS, 0);

    InitializeListHead(&CompleteList);
    KeAcquireSpinLock(&Extension->Lock, &OldIrql);
    MsRilFlushIrps(File, STATUS_CANCELLED, &CompleteList);
    MsRilFreeRecords(File);

    if (File->Role == MsRilRoleHost)
    {
        Extension->Host = NULL;

        /* Every client served by this host is dead, they reconnect to the next one */
        while (!IsListEmpty(&Extension->Clients))
        {
            Entry = RemoveHeadList(&Extension->Clients);
            InitializeListHead(Entry);
            Client = CONTAINING_RECORD(Entry, MSRIL_FILE, Link);
            MsRilKillClient(Client, MsRilClientDisconnected, &CompleteList);
            Orphaned++;
        }
    }
    else
    {
        if (!IsListEmpty(&File->Link))
        {
            RemoveEntryList(&File->Link);
            InitializeListHead(&File->Link);

            File->Closed->Event.EventType = MsRilClientClosed;
            File->Closed->Event.ClientId = File->ClientId;
            File->Closed->Event.ProcessId = File->ProcessId;
            MsRilPostRecord(Extension->Host, File->Closed, &CompleteList);
            File->Closed = NULL;
        }
        File->State = MsRilClientDisconnected;
    }
    KeReleaseSpinLock(&Extension->Lock, OldIrql);
    MsRilCompleteList(&CompleteList);

    InterlockedDecrement(&Extension->OpenHandles);

    if (File->Role == MsRilRoleHost)
    {
        DPRINT1("msril: host detached from MSRil%lu (pid %lu), %lu client(s) cut off\n",
                Extension->Instance, File->ProcessId, Orphaned);
        MsRilUpdateInterface(Extension);
    }
    else
    {
        DPRINT1("msril: client %lu closed (pid %lu)\n", File->ClientId, File->ProcessId);
    }

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
        MsRilFreeFile(FileObject->FsContext);
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
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    PMSRIL_FILE File = Stack->FileObject->FsContext;
    NTSTATUS Status;
    KIRQL OldIrql;

    if (!File || File->Role != MsRilRoleClient)
        return MsRilCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);

    if (Extension->Removed)
        return MsRilCompleteIrp(Irp, STATUS_NO_SUCH_DEVICE, 0);

    if (Stack->Parameters.Read.Length == 0)
        return MsRilCompleteIrp(Irp, STATUS_SUCCESS, 0);

    KeAcquireSpinLock(&Extension->Lock, &OldIrql);
    if (File->State != MsRilClientLive)
    {
        KeReleaseSpinLock(&Extension->Lock, OldIrql);
        return MsRilCompleteIrp(Irp, STATUS_CANCELLED, 0);
    }

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

/* Applies an enable record to the client and answers it like the host would */
static
NTSTATUS
MsRilEnableNotifications(
    _In_ PMSRIL_EXTENSION Extension,
    _In_ PMSRIL_FILE File,
    _In_reads_bytes_(Length) const RIL_ENABLE_RECORD *Request,
    _In_ ULONG Length)
{
    PRIL_RESPONSE_RECORD Response;
    LIST_ENTRY CompleteList;
    PMSRIL_RECORD Record, Nitz = NULL;
    NTSTATUS Status = STATUS_SUCCESS;
    KIRQL OldIrql;
    BOOLEAN WantsNitz = FALSE;
    ULONG i;

    if (Length < FIELD_OFFSET(RIL_ENABLE_RECORD, Codes) ||
        Request->Count != (Length - FIELD_OFFSET(RIL_ENABLE_RECORD, Codes)) / sizeof(ULONG) ||
        (Length - FIELD_OFFSET(RIL_ENABLE_RECORD, Codes)) % sizeof(ULONG) != 0)
    {
        DPRINT1("msril: client %lu enable record of %lu bytes does not match its count\n",
                File->ClientId, Length);
        return STATUS_INVALID_PARAMETER;
    }

    for (i = 0; i < Request->Count; i++)
    {
        if (Request->Codes[i] < RIL_RECORD_NOTIFY_FIRST || Request->Codes[i] > RIL_RECORD_NOTIFY_LAST)
        {
            DPRINT1("msril: client %lu asked for unknown notification 0x%lx\n",
                    File->ClientId, Request->Codes[i]);
            return STATUS_INVALID_PARAMETER;
        }
        if (Request->Codes[i] == MSRIL_NOTIFY_NITZ)
            WantsNitz = TRUE;
    }

    Record = MsRilAllocateRecord(sizeof(*Response));
    if (!Record)
        return STATUS_INSUFFICIENT_RESOURCES;

    Response = (PRIL_RESPONSE_RECORD)Record->Data;
    Response->Type = RIL_RECORD_RESPONSE;
    Response->ResultType = RIL_RECORD_RESULT_OK;
    Response->Command = Request->Header.Command;
    Response->Context = Request->Header.Context;

    InitializeListHead(&CompleteList);
    KeAcquireSpinLock(&Extension->Lock, &OldIrql);
    if (File->State != MsRilClientLive || !Extension->Host)
    {
        Status = STATUS_CANCELLED;
    }
    else
    {
        for (i = 0; i < Request->Count; i++)
            File->Enabled[Request->Codes[i] - RIL_RECORD_NOTIFY_FIRST] = TRUE;

        MsRilQueueToClient(File, Record, &CompleteList);
        Record = NULL;

        /* The answer goes first, then the network time the client missed */
        if (WantsNitz && Extension->LastNitz)
        {
            Nitz = MsRilAllocateRecord(Extension->LastNitz->Length);
            if (Nitz)
            {
                RtlCopyMemory(Nitz->Data, Extension->LastNitz->Data, Extension->LastNitz->Length);
                MsRilAgeNitz(Nitz, Extension->LastNitzTime);
                MsRilQueueToClient(File, Nitz, &CompleteList);
            }
        }
    }
    KeReleaseSpinLock(&Extension->Lock, OldIrql);
    MsRilCompleteList(&CompleteList);

    if (Record)
        ExFreePoolWithTag(Record, TAG_MSRIL);

    if (NT_SUCCESS(Status))
        DPRINT1("msril: client %lu enabled %lu notification code(s)\n", File->ClientId, Request->Count);

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
    PRIL_COMMAND_RECORD Header = Irp->AssociatedIrp.SystemBuffer;
    LIST_ENTRY CompleteList;
    PMSRIL_RECORD Record;
    NTSTATUS Status = STATUS_SUCCESS;
    KIRQL OldIrql;

    if (!File || File->Role != MsRilRoleClient)
        return MsRilCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);

    if (Extension->Removed)
        return MsRilCompleteIrp(Irp, STATUS_NO_SUCH_DEVICE, 0);

    if (Length == 0)
        return MsRilCompleteIrp(Irp, STATUS_SUCCESS, 0);

    if (Length < sizeof(RIL_COMMAND_RECORD) || Length > RIL_RECORD_MAX_LENGTH ||
        Header->Type != RIL_RECORD_COMMAND)
    {
        DPRINT1("msril: client %lu wrote a bad record (%lu bytes)\n", File->ClientId, Length);
        return MsRilCompleteIrp(Irp, STATUS_INVALID_PARAMETER, 0);
    }

    if (File->State != MsRilClientLive)
        return MsRilCompleteIrp(Irp, STATUS_CANCELLED, 0);

    /* Notification selection lives in the broker, the host never sees it */
    if (Header->Command == RIL_RECORD_ENABLE_COMMAND)
    {
        Status = MsRilEnableNotifications(Extension, File, (PRIL_ENABLE_RECORD)Header, Length);
        return MsRilCompleteIrp(Irp, Status, NT_SUCCESS(Status) ? Length : 0);
    }

    Record = MsRilAllocateRecord(Length);
    if (!Record)
        return MsRilCompleteIrp(Irp, STATUS_INSUFFICIENT_RESOURCES, 0);

    RtlCopyMemory(Record->Data, Header, Length);
    Record->Event.EventType = MsRilClientRecord;
    Record->Event.ClientId = File->ClientId;
    Record->Event.ProcessId = File->ProcessId;

    InitializeListHead(&CompleteList);
    KeAcquireSpinLock(&Extension->Lock, &OldIrql);
    if (File->State != MsRilClientLive || !Extension->Host)
    {
        Status = STATUS_CANCELLED;
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

/* Copies a notification to every live client that enabled its code. Caller holds the device lock. */
static
VOID
MsRilBroadcast(
    _In_ PMSRIL_EXTENSION Extension,
    _In_ PMSRIL_RECORD Source,
    _Inout_ PLIST_ENTRY CompleteList)
{
    ULONG Code = ((PRIL_NOTIFICATION_RECORD)Source->Data)->Code;
    PLIST_ENTRY Entry, Next;
    PMSRIL_RECORD Copy;
    PMSRIL_FILE Client;

    if (Code < RIL_RECORD_NOTIFY_FIRST || Code > RIL_RECORD_NOTIFY_LAST)
        return;

    for (Entry = Extension->Clients.Flink; Entry != &Extension->Clients; Entry = Next)
    {
        Next = Entry->Flink;
        Client = CONTAINING_RECORD(Entry, MSRIL_FILE, Link);
        if (Client->State != MsRilClientLive || !Client->Enabled[Code - RIL_RECORD_NOTIFY_FIRST])
            continue;

        Copy = MsRilAllocateRecord(Source->Length);
        if (!Copy)
        {
            DPRINT1("msril: no memory for notification 0x%lx to client %lu\n", Code, Client->ClientId);
            continue;
        }

        RtlCopyMemory(Copy->Data, Source->Data, Source->Length);
        MsRilQueueToClient(Client, Copy, CompleteList);
    }
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
    PMSRIL_RECORD Record, Stale = NULL;
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

    if (Delivery->ClientId == MSRIL_CLIENT_BROADCAST &&
        (Delivery->DataLength < sizeof(RIL_NOTIFICATION_RECORD) ||
         ((PRIL_NOTIFICATION_RECORD)(Delivery + 1))->Type != RIL_RECORD_NOTIFICATION))
    {
        return STATUS_INVALID_PARAMETER;
    }

    Record = MsRilAllocateRecord(Delivery->DataLength);
    if (!Record)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlCopyMemory(Record->Data, Delivery + 1, Delivery->DataLength);

    InitializeListHead(&CompleteList);
    KeAcquireSpinLock(&Extension->Lock, &OldIrql);
    if (Delivery->ClientId == MSRIL_CLIENT_BROADCAST)
    {
        MsRilBroadcast(Extension, Record, &CompleteList);

        /* The broadcast copies the record, so this one can be kept as is */
        if (((PRIL_NOTIFICATION_RECORD)Record->Data)->Code == MSRIL_NOTIFY_NITZ)
        {
            Stale = Extension->LastNitz;
            Extension->LastNitz = Record;
            Extension->LastNitzTime = KeQueryInterruptTime();
            Record = NULL;
        }
    }
    else
    {
        Client = MsRilFindClient(Extension, Delivery->ClientId);
        if (!Client)
        {
            Status = STATUS_NOT_FOUND;
        }
        else
        {
            /* A client that went over budget silently loses what follows */
            MsRilQueueToClient(Client, Record, &CompleteList);
            Record = NULL;
        }
    }
    KeReleaseSpinLock(&Extension->Lock, OldIrql);
    MsRilCompleteList(&CompleteList);

    if (Record)
        ExFreePoolWithTag(Record, TAG_MSRIL);
    if (Stale)
        ExFreePoolWithTag(Stale, TAG_MSRIL);

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
            if (Extension->Removed)
            {
                KeReleaseSpinLock(&Extension->Lock, OldIrql);
                return MsRilCompleteIrp(Irp, STATUS_NO_SUCH_DEVICE, 0);
            }

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

/* Fails the pending IRPs of every open file once the device is going away */
static
VOID
MsRilFlushAll(
    _In_ PMSRIL_EXTENSION Extension)
{
    LIST_ENTRY CompleteList;
    PLIST_ENTRY Entry;
    PMSRIL_RECORD Nitz;
    KIRQL OldIrql;

    InitializeListHead(&CompleteList);
    KeAcquireSpinLock(&Extension->Lock, &OldIrql);
    Extension->Removed = TRUE;
    if (Extension->Host)
        MsRilFlushIrps(Extension->Host, STATUS_NO_SUCH_DEVICE, &CompleteList);

    for (Entry = Extension->Clients.Flink; Entry != &Extension->Clients; Entry = Entry->Flink)
        MsRilFlushIrps(CONTAINING_RECORD(Entry, MSRIL_FILE, Link), STATUS_NO_SUCH_DEVICE, &CompleteList);
    Nitz = Extension->LastNitz;
    Extension->LastNitz = NULL;
    KeReleaseSpinLock(&Extension->Lock, OldIrql);
    MsRilCompleteList(&CompleteList);

    if (Nitz)
        ExFreePoolWithTag(Nitz, TAG_MSRIL);
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
    NTSTATUS Status;

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
                if (!NT_SUCCESS(Status))
                    DPRINT1("msril: MSRil%lu interface registration failed 0x%lx\n", Extension->Instance, Status);
            }

            if (NT_SUCCESS(Status))
            {
                Extension->Started = TRUE;
                MsRilUpdateInterface(Extension);
            }

            return MsRilCompleteIrp(Irp, Status, 0);
        }

        case IRP_MN_QUERY_REMOVE_DEVICE:
            if (Extension->OpenHandles != 0)
            {
                DPRINT1("msril: MSRil%lu refused removal, %ld handle(s) open\n",
                        Extension->Instance, Extension->OpenHandles);
                return MsRilCompleteIrp(Irp, STATUS_DEVICE_BUSY, 0);
            }
            Irp->IoStatus.Status = STATUS_SUCCESS;
            break;

        case IRP_MN_SURPRISE_REMOVAL:
            MsRilFlushAll(Extension);
            MsRilUpdateInterface(Extension);
            Irp->IoStatus.Status = STATUS_SUCCESS;
            break;

        case IRP_MN_REMOVE_DEVICE:
        {
            MsRilFlushAll(Extension);
            MsRilUpdateInterface(Extension);

            Irp->IoStatus.Status = STATUS_SUCCESS;
            IoSkipCurrentIrpStackLocation(Irp);
            Status = IoCallDriver(Extension->Lower, Irp);

            IoDeleteSymbolicLink(&Extension->LinkName);
            RtlFreeUnicodeString(&Extension->InterfaceName);
            InterlockedAnd(&MsRilInstanceMask, ~(LONG)(1UL << (Extension->Instance - 1)));
            IoDetachDevice(Extension->Lower);
            IoDeleteDevice(DeviceObject);
            return Status;
        }

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

/* Takes the lowest free broker number so a lone broker is always MSRil1 */
static
ULONG
MsRilClaimInstance(VOID)
{
    ULONG Index;

    for (Index = 0; Index < MSRIL_MAX_INSTANCES; Index++)
    {
        if (!InterlockedBitTestAndSet(&MsRilInstanceMask, Index))
            return Index + 1;
    }

    return 0;
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

    Instance = MsRilClaimInstance();
    if (!Instance)
        return STATUS_TOO_MANY_NAMES;

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
        goto Fail;

    Extension = DeviceObject->DeviceExtension;
    RtlZeroMemory(Extension, sizeof(*Extension));
    Extension->Self = DeviceObject;
    Extension->Pdo = PhysicalDeviceObject;
    Extension->Instance = Instance;
    KeInitializeEvent(&Extension->InterfaceLock, SynchronizationEvent, TRUE);
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
        goto Fail;
    }

    Extension->Lower = IoAttachDeviceToDeviceStack(DeviceObject, PhysicalDeviceObject);
    if (!Extension->Lower)
    {
        IoDeleteSymbolicLink(&Extension->LinkName);
        IoDeleteDevice(DeviceObject);
        Status = STATUS_NO_SUCH_DEVICE;
        goto Fail;
    }

    DeviceObject->Flags |= DO_BUFFERED_IO | DO_POWER_PAGABLE;
    DeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;
    DPRINT1("msril: broker device MSRil%lu created\n", Instance);
    return STATUS_SUCCESS;

Fail:
    DPRINT1("msril: broker device MSRil%lu failed 0x%lx\n", Instance, Status);
    InterlockedAnd(&MsRilInstanceMask, ~(LONG)(1UL << (Instance - 1)));
    return Status;
}

static
VOID
NTAPI
MsRilUnload(
    _In_ PDRIVER_OBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject);
}

/* The broker is a PnP function driver, its root devnode comes from the hive or from riladapt */
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
