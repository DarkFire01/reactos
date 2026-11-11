/*
 * PROJECT:     ReactOS Qualcomm IPC Router driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     The \Device\IPC file, read, write and control interface
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "ipcrouter.h"

#define NDEBUG
#include <debug.h>

/* A received data message held for the owning sink until it is read */
typedef struct _IPC_RXMSG
{
    LIST_ENTRY Link;
    IPC_ADDR Src;
    ULONG Length;
    BOOLEAN ConfirmRx;
    UCHAR Data[1];
} IPC_RXMSG, *PIPC_RXMSG;

/* A control event (server up/down, client gone) queued for the owning sink */
typedef struct _IPC_EVTMSG
{
    LIST_ENTRY Link;
    ULONG Event;
    IPC_ADDR Peer;
} IPC_EVTMSG, *PIPC_EVTMSG;

/*
 * The receiving endpoint for one open handle. It is reference counted and
 * lives independently of the WDF file object so that a router callback in
 * flight never touches a file context that cleanup has already freed. The
 * owning port holds one reference, released through the port destroy hook
 * once no further callback can run.
 */
typedef struct _IPC_SINK
{
    LONG RefCount;
    FAST_MUTEX Lock;
    LIST_ENTRY RxData;
    LIST_ENTRY RxEvents;
    WDFQUEUE ReadWait;
    BOOLEAN Closing;
    IPC_ADDR Addr;
} IPC_SINK, *PIPC_SINK;

typedef struct _IPC_FILE
{
    PIPC_SINK Sink;
    PIPC_PORT Port;
    IPC_ADDR Addr;
    BOOLEAN Bound;
    IPC_ADDR Name;
} IPC_FILE, *PIPC_FILE;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(IPC_FILE, IpcGetFile);

#include <pshpack4.h>
typedef struct _IPC_SRV_CTL
{
    ULONG Command;
    ULONG Service;
    ULONG Instance;
    ULONG Node;
    ULONG Port;
} IPC_SRV_CTL;
#include <poppack.h>

static
PIPC_SINK
IpcSinkCreate(
    _In_ WDFDEVICE WdfDevice)
{
    WDF_IO_QUEUE_CONFIG QueueConfig;
    PIPC_SINK Sink;

    Sink = ExAllocatePoolWithTag(IPC_POOL, sizeof(*Sink), IPC_TAG);
    if (!Sink)
        return NULL;

    RtlZeroMemory(Sink, sizeof(*Sink));
    Sink->RefCount = 1;
    ExInitializeFastMutex(&Sink->Lock);
    InitializeListHead(&Sink->RxData);
    InitializeListHead(&Sink->RxEvents);

    WDF_IO_QUEUE_CONFIG_INIT(&QueueConfig, WdfIoQueueDispatchManual);
    if (!NT_SUCCESS(WdfIoQueueCreate(WdfDevice, &QueueConfig, WDF_NO_OBJECT_ATTRIBUTES, &Sink->ReadWait)))
    {
        ExFreePoolWithTag(Sink, IPC_TAG);
        return NULL;
    }

    return Sink;
}

static
VOID
IpcSinkAddRef(
    _In_ PIPC_SINK Sink)
{
    InterlockedIncrement(&Sink->RefCount);
}

static
VOID
IpcSinkRelease(
    _In_ PIPC_SINK Sink)
{
    PLIST_ENTRY Entry;

    if (InterlockedDecrement(&Sink->RefCount) != 0)
        return;

    if (Sink->ReadWait)
        WdfObjectDelete(Sink->ReadWait);

    while (!IsListEmpty(&Sink->RxData))
    {
        Entry = RemoveHeadList(&Sink->RxData);
        ExFreePoolWithTag(CONTAINING_RECORD(Entry, IPC_RXMSG, Link), IPC_TAG);
    }
    while (!IsListEmpty(&Sink->RxEvents))
    {
        Entry = RemoveHeadList(&Sink->RxEvents);
        ExFreePoolWithTag(CONTAINING_RECORD(Entry, IPC_EVTMSG, Link), IPC_TAG);
    }

    ExFreePoolWithTag(Sink, IPC_TAG);
}

/* Port destroy hook: the owning port drops its sink reference here */
static
VOID
NTAPI
IpcSinkOnPortDestroy(
    _In_ PVOID Context)
{
    IpcSinkRelease((PIPC_SINK)Context);
}

static
VOID
IpcBroadcastServer(
    _In_ ULONG Command,
    _In_ PIPC_ADDR Name,
    _In_ PIPC_ADDR Addr)
{
    PIPC_DEVICE Device = IpcGlobal;
    IPC_SRV_CTL Body;
    ULONG i;

    if (!Device)
        return;

    Body.Command = Command;
    Body.Service = Name->Node;
    Body.Instance = Name->Port;
    Body.Node = Addr->Node;
    Body.Port = Addr->Port;

    for (i = 0; i < Device->XportCount; i++)
    {
        if (Device->Xports[i].Up)
            IpcControlSend(Command, &Device->Xports[i], NULL, &Body);
    }
}

/* Router receive hook for a client sink, runs at PASSIVE_LEVEL */
static
ULONG
NTAPI
IpcClientRx(
    _In_ PIPC_PORT Port,
    _In_opt_ PVOID Context,
    _Inout_ PIPC_MBUF *Payload,
    _In_ PIPC_HEADER Header)
{
    PIPC_SINK Sink = Context;
    PIPC_RXMSG Message, Head = NULL;
    WDFREQUEST Request = NULL;
    WDFMEMORY Memory;
    PVOID Buffer = NULL;
    size_t Size = 0;
    ULONG Length;

    UNREFERENCED_PARAMETER(Port);

    Length = IpcMbufChainLength(*Payload);
    Message = ExAllocatePoolWithTag(IPC_POOL, FIELD_OFFSET(IPC_RXMSG, Data) + Length, IPC_TAG);
    if (!Message)
    {
        IpcMbufFreeChain(Payload);
        return IPC_ENOMEM;
    }

    Message->Src.Node = Header->SrcNode;
    Message->Src.Port = Header->SrcPort;
    Message->Length = Length;
    Message->ConfirmRx = (Header->ConfirmRx != 0);
    IpcMbufCopyOut(Payload, Message->Data, Length);

    ExAcquireFastMutex(&Sink->Lock);
    if (Sink->Closing)
    {
        ExReleaseFastMutex(&Sink->Lock);
        ExFreePoolWithTag(Message, IPC_TAG);
        return IPC_OK;
    }

    InsertTailList(&Sink->RxData, &Message->Link);

    /* Hand the earliest queued message to a waiting reader if one is pended */
    if (WdfIoQueueRetrieveNextRequest(Sink->ReadWait, &Request) == STATUS_SUCCESS)
    {
        Head = CONTAINING_RECORD(RemoveHeadList(&Sink->RxData), IPC_RXMSG, Link);
    }
    ExReleaseFastMutex(&Sink->Lock);

    if (!Request)
        return IPC_OK;

    if (NT_SUCCESS(WdfRequestRetrieveOutputMemory(Request, &Memory)))
        Buffer = WdfMemoryGetBuffer(Memory, &Size);

    if (Buffer && Size >= (size_t)Head->Length + 8)
    {
        BOOLEAN Confirm = Head->ConfirmRx;
        IPC_ADDR Src = Head->Src;

        RtlCopyMemory(Buffer, &Head->Src, 8);
        RtlCopyMemory((PUCHAR)Buffer + 8, Head->Data, Head->Length);
        WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, Head->Length + 8);
        ExFreePoolWithTag(Head, IPC_TAG);

        /* Delivered to the client, let the sender reopen its window */
        if (Confirm)
            IpcSendResume(&Src, &Sink->Addr);
    }
    else
    {
        /* Give the client the size it needs and keep the message queued */
        ExAcquireFastMutex(&Sink->Lock);
        InsertHeadList(&Sink->RxData, &Head->Link);
        ExReleaseFastMutex(&Sink->Lock);
        WdfRequestCompleteWithInformation(Request, STATUS_BUFFER_TOO_SMALL, Head->Length + 8);
    }

    return IPC_OK;
}

static
VOID
NTAPI
IpcClientEvent(
    _In_ PIPC_PORT Port,
    _In_opt_ PVOID Context,
    _In_ ULONG Event,
    _In_ PIPC_ADDR Peer)
{
    PIPC_SINK Sink = Context;
    PIPC_EVTMSG Message;

    UNREFERENCED_PARAMETER(Port);

    Message = ExAllocatePoolWithTag(IPC_POOL, sizeof(*Message), IPC_TAG);
    if (!Message)
        return;

    Message->Event = Event;
    Message->Peer = *Peer;

    ExAcquireFastMutex(&Sink->Lock);
    if (Sink->Closing)
    {
        ExReleaseFastMutex(&Sink->Lock);
        ExFreePoolWithTag(Message, IPC_TAG);
        return;
    }
    InsertTailList(&Sink->RxEvents, &Message->Link);
    ExReleaseFastMutex(&Sink->Lock);
}

static EVT_WDF_DEVICE_FILE_CREATE IpcEvtFileCreate;
static EVT_WDF_FILE_CLEANUP IpcEvtFileCleanup;
static EVT_WDF_IO_QUEUE_IO_READ IpcEvtIoRead;
static EVT_WDF_IO_QUEUE_IO_WRITE IpcEvtIoWrite;
static EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL IpcEvtIoDeviceControl;

static
VOID
IpcEvtFileCreate(
    _In_ WDFDEVICE WdfDevice,
    _In_ WDFREQUEST Request,
    _In_ WDFFILEOBJECT FileObject)
{
    PIPC_FILE File = IpcGetFile(FileObject);
    PIPC_SINK Sink;
    ULONG Status;

    RtlZeroMemory(File, sizeof(*File));

    Sink = IpcSinkCreate(WdfDevice);
    if (!Sink)
    {
        WdfRequestComplete(Request, STATUS_INSUFFICIENT_RESOURCES);
        return;
    }

    Sink->Addr.Node = IpcLocalNode();
    Sink->Addr.Port = IpcAddressAllocate();
    File->Addr = Sink->Addr;

    /* The port keeps the sink alive for as long as it can deliver to it */
    IpcSinkAddRef(Sink);

    Status = IpcPortInsert(&File->Addr, NULL, IpcClientRx, Sink, IpcClientEvent, Sink,
                           IPC_PRIORITY_DEFAULT, &File->Port);
    if (Status != IPC_OK)
    {
        IpcSinkRelease(Sink);
        IpcSinkRelease(Sink);
        WdfRequestComplete(Request, STATUS_INSUFFICIENT_RESOURCES);
        return;
    }

    IpcPortSetDestroyHook(File->Port, IpcSinkOnPortDestroy, Sink);
    File->Sink = Sink;
    WdfRequestComplete(Request, STATUS_SUCCESS);
}

static
VOID
IpcEvtFileCleanup(
    _In_ WDFFILEOBJECT FileObject)
{
    PIPC_FILE File = IpcGetFile(FileObject);
    PIPC_SINK Sink = File->Sink;
    PLIST_ENTRY Entry;
    IPC_ADDR Zero = { 0, 0 };

    if (File->Bound)
    {
        IpcBroadcastServer(IPC_TYPE_REMOVE_SERVER, &File->Name, &File->Addr);
        IpcBroadcastEvent(IpcLocalNode(), IPC_EVENT_REMOVE_SERVER, &File->Name);
    }

    /* Tell remote nodes and local clients this client port is going away.
     * The five word body is {cmd, node, port, 0, 0}, matching REMOVE_CLIENT. */
    if (File->Port)
    {
        IpcBroadcastServer(IPC_TYPE_REMOVE_CLIENT, &File->Addr, &Zero);
        IpcBroadcastEvent(IpcLocalNode(), IPC_EVENT_REMOVE_CLIENT, &File->Addr);
    }

    if (Sink)
    {
        /* Stop queuing, free anything pending and release blocked readers */
        ExAcquireFastMutex(&Sink->Lock);
        Sink->Closing = TRUE;
        while (!IsListEmpty(&Sink->RxData))
        {
            Entry = RemoveHeadList(&Sink->RxData);
            ExFreePoolWithTag(CONTAINING_RECORD(Entry, IPC_RXMSG, Link), IPC_TAG);
        }
        while (!IsListEmpty(&Sink->RxEvents))
        {
            Entry = RemoveHeadList(&Sink->RxEvents);
            ExFreePoolWithTag(CONTAINING_RECORD(Entry, IPC_EVTMSG, Link), IPC_TAG);
        }
        ExReleaseFastMutex(&Sink->Lock);

        WdfIoQueuePurgeSynchronously(Sink->ReadWait);
    }

    if (File->Port)
    {
        IpcPortRemove(&File->Addr);
        IpcPortRelease(File->Port);
        File->Port = NULL;
    }

    if (Sink)
    {
        IpcSinkRelease(Sink);
        File->Sink = NULL;
    }
}

static
VOID
IpcEvtIoRead(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t Length)
{
    PIPC_FILE File = IpcGetFile(WdfRequestGetFileObject(Request));
    PIPC_SINK Sink = File->Sink;
    PIPC_RXMSG Message = NULL;
    WDFMEMORY Memory;
    PVOID Buffer;
    size_t Size;
    NTSTATUS Status;
    BOOLEAN Confirm;
    IPC_ADDR Src;

    UNREFERENCED_PARAMETER(Queue);

    if (!Sink)
    {
        WdfRequestComplete(Request, STATUS_INVALID_DEVICE_STATE);
        return;
    }

    ExAcquireFastMutex(&Sink->Lock);
    if (!IsListEmpty(&Sink->RxData))
    {
        Message = CONTAINING_RECORD(RemoveHeadList(&Sink->RxData), IPC_RXMSG, Link);
    }
    ExReleaseFastMutex(&Sink->Lock);

    if (!Message)
    {
        /* Block until IpcClientRx completes the request with a message */
        Status = WdfRequestForwardToIoQueue(Request, Sink->ReadWait);
        if (!NT_SUCCESS(Status))
            WdfRequestComplete(Request, Status);
        return;
    }

    if (Length < (size_t)Message->Length + 8)
    {
        ExAcquireFastMutex(&Sink->Lock);
        InsertHeadList(&Sink->RxData, &Message->Link);
        ExReleaseFastMutex(&Sink->Lock);
        WdfRequestCompleteWithInformation(Request, STATUS_BUFFER_TOO_SMALL, Message->Length + 8);
        return;
    }

    Confirm = Message->ConfirmRx;
    Src = Message->Src;

    Status = WdfRequestRetrieveOutputMemory(Request, &Memory);
    if (NT_SUCCESS(Status))
    {
        Buffer = WdfMemoryGetBuffer(Memory, &Size);
        RtlCopyMemory(Buffer, &Message->Src, 8);
        RtlCopyMemory((PUCHAR)Buffer + 8, Message->Data, Message->Length);
        WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, Message->Length + 8);
    }
    else
    {
        WdfRequestComplete(Request, Status);
    }

    ExFreePoolWithTag(Message, IPC_TAG);

    /* Delivered to the client, let the sender reopen its window */
    if (NT_SUCCESS(Status) && Confirm)
        IpcSendResume(&Src, &File->Addr);
}

static
VOID
IpcEvtIoWrite(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t Length)
{
    PIPC_FILE File = IpcGetFile(WdfRequestGetFileObject(Request));
    WDFMEMORY Memory;
    PUCHAR Buffer;
    size_t Size;
    IPC_ADDR Dst;
    PIPC_MBUF Payload;
    ULONG Body;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(Queue);

    if (Length < 8)
    {
        WdfRequestCompleteWithInformation(Request, STATUS_BUFFER_TOO_SMALL, 8);
        return;
    }

    Status = WdfRequestRetrieveInputMemory(Request, &Memory);
    if (!NT_SUCCESS(Status))
    {
        WdfRequestComplete(Request, Status);
        return;
    }

    Buffer = WdfMemoryGetBuffer(Memory, &Size);
    if (!Buffer || Size < 8)
    {
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        return;
    }

    Dst.Node = ((PULONG)Buffer)[0];
    Dst.Port = ((PULONG)Buffer)[1];
    Body = (ULONG)(Size - 8);

    Payload = IpcPacketAllocate(Buffer + 8, Body);
    if (!Payload)
    {
        WdfRequestComplete(Request, STATUS_INSUFFICIENT_RESOURCES);
        return;
    }

    if (IpcPortSendTo(File->Port, &Dst, &Payload, IPC_TX_TIMEOUT_MS) == IPC_OK)
        WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, Length);
    else
        WdfRequestComplete(Request, STATUS_DEVICE_NOT_READY);
}

static
NTSTATUS
IpcIoctlGetEvent(
    _In_ PIPC_SINK Sink,
    _In_ WDFREQUEST Request,
    _Out_ PULONG_PTR Information)
{
    WDFMEMORY Memory;
    PULONG Buffer;
    size_t Size;
    PIPC_EVTMSG Message = NULL;
    NTSTATUS Status;

    Status = WdfRequestRetrieveOutputMemory(Request, &Memory);
    if (!NT_SUCCESS(Status))
        return Status;

    Buffer = WdfMemoryGetBuffer(Memory, &Size);
    if (!Buffer || Size < IPC_CONTROL_SIZE)
        return STATUS_BUFFER_TOO_SMALL;

    ExAcquireFastMutex(&Sink->Lock);
    if (!IsListEmpty(&Sink->RxEvents))
        Message = CONTAINING_RECORD(RemoveHeadList(&Sink->RxEvents), IPC_EVTMSG, Link);
    ExReleaseFastMutex(&Sink->Lock);

    if (!Message)
        return STATUS_NO_MORE_ENTRIES;

    Buffer[0] = Message->Event;
    Buffer[1] = Message->Peer.Node;
    Buffer[2] = Message->Peer.Port;
    Buffer[3] = 0;
    Buffer[4] = 0;
    *Information = IPC_CONTROL_SIZE;
    ExFreePoolWithTag(Message, IPC_TAG);
    return STATUS_SUCCESS;
}

static
NTSTATUS
IpcIoctlLookupServer(
    _In_ WDFREQUEST Request,
    _Out_ PULONG_PTR Information)
{
    WDFMEMORY InMem, OutMem;
    PULONG In;
    PIPC_ADDR Out;
    size_t InSize, OutSize;
    IPC_ADDR Name;
    PIPC_PORT Port;
    NTSTATUS Status;

    Status = WdfRequestRetrieveInputMemory(Request, &InMem);
    if (!NT_SUCCESS(Status))
        return Status;

    In = WdfMemoryGetBuffer(InMem, &InSize);
    if (!In || InSize < 8)
        return STATUS_INVALID_PARAMETER;

    Status = WdfRequestRetrieveOutputMemory(Request, &OutMem);
    if (!NT_SUCCESS(Status))
        return Status;

    Out = WdfMemoryGetBuffer(OutMem, &OutSize);
    if (!Out || OutSize < sizeof(IPC_ADDR))
        return STATUS_BUFFER_TOO_SMALL;

    Name.Node = In[0];
    Name.Port = In[1];

    Port = IpcServerLookup(&Name);
    if (!Port)
        return STATUS_OBJECT_NAME_NOT_FOUND;

    *Out = Port->Addr;
    *Information = sizeof(IPC_ADDR);
    IpcPortRelease(Port);
    return STATUS_SUCCESS;
}

static
NTSTATUS
IpcIoctlBind(
    _In_ PIPC_FILE File,
    _In_ WDFREQUEST Request,
    _In_ BOOLEAN Bind)
{
    WDFMEMORY Memory;
    PULONG In;
    size_t Size;
    IPC_ADDR Name;
    NTSTATUS Status;

    Status = WdfRequestRetrieveInputMemory(Request, &Memory);
    if (!NT_SUCCESS(Status))
        return Status;

    In = WdfMemoryGetBuffer(Memory, &Size);
    if (!In || Size < 8)
        return STATUS_INVALID_PARAMETER;

    Name.Node = In[0];
    Name.Port = In[1];

    if (Bind)
    {
        if (IpcServerAdd(&Name, &File->Addr) != IPC_OK)
            return STATUS_DUPLICATE_NAME;

        File->Bound = TRUE;
        File->Name = Name;
        IpcBroadcastServer(IPC_TYPE_NEW_SERVER, &Name, &File->Addr);
        IpcBroadcastEvent(IpcLocalNode(), IPC_EVENT_NEW_SERVER, &Name);
    }
    else
    {
        IpcServerRemove(&Name, &File->Addr);
        IpcBroadcastServer(IPC_TYPE_REMOVE_SERVER, &Name, &File->Addr);
        IpcBroadcastEvent(IpcLocalNode(), IPC_EVENT_REMOVE_SERVER, &Name);
        File->Bound = FALSE;
    }

    return STATUS_SUCCESS;
}

static
VOID
IpcEvtIoDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputLength,
    _In_ size_t InputLength,
    _In_ ULONG IoControlCode)
{
    PIPC_FILE File = IpcGetFile(WdfRequestGetFileObject(Request));
    ULONG_PTR Information = 0;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(Queue);
    UNREFERENCED_PARAMETER(OutputLength);
    UNREFERENCED_PARAMETER(InputLength);

    switch (IoControlCode)
    {
        case IPC_IOCTL_GET_EVENT:
        case IPC_IOCTL_POLL_EVENT:
            Status = File->Sink ? IpcIoctlGetEvent(File->Sink, Request, &Information)
                                : STATUS_INVALID_DEVICE_STATE;
            break;

        case IPC_IOCTL_LOOKUP_SERVER:
            Status = IpcIoctlLookupServer(Request, &Information);
            break;

        case IPC_IOCTL_BIND_NAME:
            Status = IpcIoctlBind(File, Request, TRUE);
            break;

        case IPC_IOCTL_UNBIND_NAME:
            Status = IpcIoctlBind(File, Request, FALSE);
            break;

        case IPC_IOCTL_GET_SELF:
        {
            WDFMEMORY Memory;
            PIPC_ADDR Out;
            size_t Size;

            Status = WdfRequestRetrieveOutputMemory(Request, &Memory);
            if (NT_SUCCESS(Status))
            {
                Out = WdfMemoryGetBuffer(Memory, &Size);
                if (Out && Size >= sizeof(IPC_ADDR))
                {
                    *Out = File->Addr;
                    Information = sizeof(IPC_ADDR);
                }
                else
                {
                    Status = STATUS_BUFFER_TOO_SMALL;
                }
            }
            break;
        }

        default:
            Status = STATUS_INVALID_DEVICE_REQUEST;
            break;
    }

    WdfRequestCompleteWithInformation(Request, Status, Information);
}

VOID
IpcFileObjectConfig(
    _In_ PWDFDEVICE_INIT DeviceInit)
{
    WDF_FILEOBJECT_CONFIG FileConfig;
    WDF_OBJECT_ATTRIBUTES Attributes;

    WDF_FILEOBJECT_CONFIG_INIT(&FileConfig, IpcEvtFileCreate, WDF_NO_EVENT_CALLBACK, IpcEvtFileCleanup);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, IPC_FILE);
    WdfDeviceInitSetFileObjectConfig(DeviceInit, &FileConfig, &Attributes);
}

NTSTATUS
IpcCreateQueue(
    _In_ WDFDEVICE WdfDevice)
{
    WDF_IO_QUEUE_CONFIG QueueConfig;

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&QueueConfig, WdfIoQueueDispatchParallel);
    QueueConfig.EvtIoRead = IpcEvtIoRead;
    QueueConfig.EvtIoWrite = IpcEvtIoWrite;
    QueueConfig.EvtIoDeviceControl = IpcEvtIoDeviceControl;

    return WdfIoQueueCreate(WdfDevice, &QueueConfig, WDF_NO_OBJECT_ATTRIBUTES, NULL);
}
