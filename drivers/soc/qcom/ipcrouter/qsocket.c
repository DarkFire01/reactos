/*
 * PROJECT:     ReactOS Qualcomm IPC Router driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Kernel qsocket query interface over the router
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "ipcrouter.h"

#define NDEBUG
#include <debug.h>

#define QSOCK_OK            0
#define QSOCK_ETIMEDOUT     (-1)
#define QSOCK_EINVAL        (-2)
#define QSOCK_ECLOSED       (-3)
#define QSOCK_ENOMEM        (-5)
#define QSOCK_ENODEV        (-6)

typedef struct _QSOCK_MSG
{
    LIST_ENTRY Link;
    IPC_ADDR Src;
    ULONG Length;
    BOOLEAN ConfirmRx;
    UCHAR Data[1];
} QSOCK_MSG, *PQSOCK_MSG;

/*
 * A kernel endpoint. Reference counted like the file sink so a router receive
 * in flight never touches a socket a caller has already closed. The owning
 * port holds one reference, dropped through the port destroy hook.
 */
typedef struct _QSOCK
{
    LONG RefCount;
    FAST_MUTEX Lock;
    LIST_ENTRY RxData;
    KEVENT DataReady;
    PIPC_PORT Port;
    IPC_ADDR Addr;
    BOOLEAN Bound;
    BOOLEAN Closing;
    IPC_ADDR Name;
} QSOCK, *PQSOCK;

static QCOM_IPC_INTERFACE IpcQsocketInterface;

static
VOID
QsockRelease(
    _In_ PQSOCK Socket)
{
    PLIST_ENTRY Entry;

    if (InterlockedDecrement(&Socket->RefCount) != 0)
        return;

    while (!IsListEmpty(&Socket->RxData))
    {
        Entry = RemoveHeadList(&Socket->RxData);
        ExFreePoolWithTag(CONTAINING_RECORD(Entry, QSOCK_MSG, Link), IPC_TAG);
    }

    ExFreePoolWithTag(Socket, IPC_TAG);
}

static
VOID
NTAPI
QsockOnPortDestroy(
    _In_ PVOID Context)
{
    QsockRelease((PQSOCK)Context);
}

static
ULONG
NTAPI
QsockRx(
    _In_ PIPC_PORT Port,
    _In_opt_ PVOID Context,
    _Inout_ PIPC_MBUF *Payload,
    _In_ PIPC_HEADER Header)
{
    PQSOCK Socket = Context;
    PQSOCK_MSG Message;
    ULONG Length;

    UNREFERENCED_PARAMETER(Port);

    Length = IpcMbufChainLength(*Payload);
    Message = ExAllocatePoolWithTag(IPC_POOL, FIELD_OFFSET(QSOCK_MSG, Data) + Length, IPC_TAG);
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

    ExAcquireFastMutex(&Socket->Lock);
    if (Socket->Closing)
    {
        ExReleaseFastMutex(&Socket->Lock);
        ExFreePoolWithTag(Message, IPC_TAG);
        return IPC_OK;
    }
    InsertTailList(&Socket->RxData, &Message->Link);
    KeSetEvent(&Socket->DataReady, 0, FALSE);
    ExReleaseFastMutex(&Socket->Lock);

    return IPC_OK;
}

static
LONG
NTAPI
QsockOpen(
    _Out_ QCOM_IPC_SOCKET *Handle)
{
    PQSOCK Socket;
    ULONG Status;

    if (!Handle)
        return QSOCK_EINVAL;

    *Handle = NULL;

    Socket = ExAllocatePoolWithTag(IPC_POOL, sizeof(*Socket), IPC_TAG);
    if (!Socket)
        return QSOCK_ENOMEM;

    RtlZeroMemory(Socket, sizeof(*Socket));
    Socket->RefCount = 1;
    ExInitializeFastMutex(&Socket->Lock);
    InitializeListHead(&Socket->RxData);
    KeInitializeEvent(&Socket->DataReady, NotificationEvent, FALSE);

    Socket->Addr.Node = IpcLocalNode();
    Socket->Addr.Port = IpcAddressAllocate();

    /* The port keeps the socket alive while it can still deliver to it */
    InterlockedIncrement(&Socket->RefCount);

    Status = IpcPortInsert(&Socket->Addr, NULL, QsockRx, Socket, NULL, NULL,
                           IPC_PRIORITY_DEFAULT, &Socket->Port);
    if (Status != IPC_OK)
    {
        QsockRelease(Socket);
        QsockRelease(Socket);
        return QSOCK_ENOMEM;
    }

    IpcPortSetDestroyHook(Socket->Port, QsockOnPortDestroy, Socket);
    *Handle = Socket;
    return QSOCK_OK;
}

static
LONG
NTAPI
QsockClose(
    _In_ QCOM_IPC_SOCKET Handle)
{
    PQSOCK Socket = Handle;

    if (!Socket)
        return QSOCK_EINVAL;

    if (Socket->Bound)
        IpcServerRemove(&Socket->Name, &Socket->Addr);

    ExAcquireFastMutex(&Socket->Lock);
    Socket->Closing = TRUE;
    KeSetEvent(&Socket->DataReady, 0, FALSE);
    ExReleaseFastMutex(&Socket->Lock);

    if (Socket->Port)
    {
        IpcPortRemove(&Socket->Addr);
        IpcPortRelease(Socket->Port);
        Socket->Port = NULL;
    }

    QsockRelease(Socket);
    return QSOCK_OK;
}

static
LONG
NTAPI
QsockGetAddr(
    _In_ QCOM_IPC_SOCKET Handle,
    _Out_ PQCOM_IPC_ADDR Addr)
{
    PQSOCK Socket = Handle;

    if (!Socket || !Addr)
        return QSOCK_EINVAL;

    Addr->Node = Socket->Addr.Node;
    Addr->Port = Socket->Addr.Port;
    return QSOCK_OK;
}

static
LONG
NTAPI
QsockBind(
    _In_ QCOM_IPC_SOCKET Handle,
    _In_ ULONG Service,
    _In_ ULONG Instance)
{
    PQSOCK Socket = Handle;
    IPC_ADDR Name;

    if (!Socket)
        return QSOCK_EINVAL;

    Name.Node = Service;
    Name.Port = Instance;

    if (IpcServerAdd(&Name, &Socket->Addr) != IPC_OK)
        return QSOCK_ENODEV;

    Socket->Bound = TRUE;
    Socket->Name = Name;
    return QSOCK_OK;
}

static
LONG
NTAPI
QsockLookup(
    _In_ ULONG Service,
    _In_ ULONG Instance,
    _Out_ PQCOM_IPC_ADDR Addr)
{
    IPC_ADDR Name;
    PIPC_PORT Port;

    if (!Addr)
        return QSOCK_EINVAL;

    Name.Node = Service;
    Name.Port = Instance;

    Port = IpcServerLookup(&Name);
    if (!Port)
        return QSOCK_ENODEV;

    Addr->Node = Port->Addr.Node;
    Addr->Port = Port->Addr.Port;
    IpcPortRelease(Port);
    return QSOCK_OK;
}

static
LONG
NTAPI
QsockSendTo(
    _In_ QCOM_IPC_SOCKET Handle,
    _In_ const QCOM_IPC_ADDR *Dst,
    _In_reads_bytes_(Length) const VOID *Buffer,
    _In_ ULONG Length)
{
    PQSOCK Socket = Handle;
    IPC_ADDR Destination;
    PIPC_MBUF Payload;

    if (!Socket || !Dst || (!Buffer && Length))
        return QSOCK_EINVAL;

    Destination.Node = Dst->Node;
    Destination.Port = Dst->Port;

    Payload = IpcPacketAllocate((PVOID)Buffer, Length);
    if (!Payload)
        return QSOCK_ENOMEM;

    switch (IpcPortSendTo(Socket->Port, &Destination, &Payload, IPC_TX_TIMEOUT_MS))
    {
        case IPC_OK:
            return QSOCK_OK;
        case IPC_EBUSY:
            return QSOCK_ETIMEDOUT;
        default:
            return QSOCK_ENODEV;
    }
}

static
LONG
NTAPI
QsockRecvFrom(
    _In_ QCOM_IPC_SOCKET Handle,
    _Out_opt_ PQCOM_IPC_ADDR Src,
    _Out_writes_bytes_(Length) PVOID Buffer,
    _In_ ULONG Length,
    _In_ LONG TimeoutMs,
    _Out_ PULONG Received)
{
    PQSOCK Socket = Handle;
    PQSOCK_MSG Message;
    LARGE_INTEGER Timeout;
    PLARGE_INTEGER TimeoutPtr;
    NTSTATUS WaitStatus;
    ULONG Copy;
    BOOLEAN Confirm;
    IPC_ADDR Source;

    if (!Socket || !Buffer || !Received)
        return QSOCK_EINVAL;

    *Received = 0;

    for (;;)
    {
        Message = NULL;

        ExAcquireFastMutex(&Socket->Lock);
        if (Socket->Closing)
        {
            ExReleaseFastMutex(&Socket->Lock);
            return QSOCK_ECLOSED;
        }
        if (!IsListEmpty(&Socket->RxData))
        {
            Message = CONTAINING_RECORD(RemoveHeadList(&Socket->RxData), QSOCK_MSG, Link);
            if (IsListEmpty(&Socket->RxData))
                KeClearEvent(&Socket->DataReady);
        }
        ExReleaseFastMutex(&Socket->Lock);

        if (Message)
            break;

        if (TimeoutMs == QCOM_IPC_TIMEOUT_INFINITE)
        {
            TimeoutPtr = NULL;
        }
        else
        {
            Timeout.QuadPart = -((LONGLONG)TimeoutMs * 10000);
            TimeoutPtr = &Timeout;
        }

        WaitStatus = KeWaitForSingleObject(&Socket->DataReady, Executive, KernelMode, FALSE, TimeoutPtr);
        if (WaitStatus == STATUS_TIMEOUT)
            return QSOCK_ETIMEDOUT;
    }

    Copy = (Message->Length < Length) ? Message->Length : Length;
    RtlCopyMemory(Buffer, Message->Data, Copy);
    if (Src)
    {
        Src->Node = Message->Src.Node;
        Src->Port = Message->Src.Port;
    }
    *Received = Copy;
    Confirm = Message->ConfirmRx;
    Source = Message->Src;

    ExFreePoolWithTag(Message, IPC_TAG);

    /* Delivered to the caller, let the sender reopen its window */
    if (Confirm)
        IpcSendResume(&Source, &Socket->Addr);

    return QSOCK_OK;
}

NTSTATUS
IpcQsocketPublish(
    _In_ WDFDEVICE WdfDevice)
{
    WDF_QUERY_INTERFACE_CONFIG Config;

    IpcQsocketInterface.Header.Size = sizeof(IpcQsocketInterface);
    IpcQsocketInterface.Header.Version = 1;
    IpcQsocketInterface.Open = QsockOpen;
    IpcQsocketInterface.Close = QsockClose;
    IpcQsocketInterface.GetAddr = QsockGetAddr;
    IpcQsocketInterface.Bind = QsockBind;
    IpcQsocketInterface.Lookup = QsockLookup;
    IpcQsocketInterface.SendTo = QsockSendTo;
    IpcQsocketInterface.RecvFrom = QsockRecvFrom;

    WDF_QUERY_INTERFACE_CONFIG_INIT(&Config, &IpcQsocketInterface.Header, &GUID_QCOM_IPC_INTERFACE, NULL);
    return WdfDeviceAddQueryInterface(WdfDevice, &Config);
}
