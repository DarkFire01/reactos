/*
 * PROJECT:     ReactOS Qualcomm IPC Router driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Router core: addressing, control protocol and routing
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "ipcrouter.h"

#define NDEBUG
#include <debug.h>

PIPC_DEVICE IpcGlobal;

static ULONG IpcLocalNodeId = (ULONG)-1;
static ULONG IpcDynamicPort;
static FAST_MUTEX IpcAddressLock;
static BOOLEAN IpcCoreReady;

/* Control message body, mirrors the five u32 carried after the header */
typedef struct _IPC_CONTROL
{
    ULONG Command;
    ULONG Arg0;
    ULONG Arg1;
    ULONG Arg2;
    ULONG Arg3;
} IPC_CONTROL, *PIPC_CONTROL;

C_ASSERT(sizeof(IPC_CONTROL) == IPC_CONTROL_SIZE);

NTSTATUS
IpcCoreInitialize(
    _In_ ULONG LocalNode)
{
    IpcLocalNodeId = LocalNode;
    IpcDynamicPort = 0;
    ExInitializeFastMutex(&IpcAddressLock);
    IpcDbInitialize();
    IpcCoreReady = TRUE;
    return STATUS_SUCCESS;
}

VOID
IpcCoreTeardown(VOID)
{
    if (!IpcCoreReady)
        return;

    IpcCoreReady = FALSE;
    IpcDbTeardown();
}

ULONG
IpcLocalNode(VOID)
{
    return IpcLocalNodeId;
}

/* Wrapping 16 bit port generator, never returns 0 or 0xFFFF */
ULONG
IpcAddressAllocate(VOID)
{
    ULONG Port;

    ExAcquireFastMutex(&IpcAddressLock);
    IpcDynamicPort = (IpcDynamicPort + 1) & 0xFFFF;
    if (IpcDynamicPort == 0 || IpcDynamicPort == 0xFFFF)
        IpcDynamicPort = 1;
    Port = IpcDynamicPort;
    ExReleaseFastMutex(&IpcAddressLock);
    return Port;
}

static
PIPC_XPORT
IpcXportForNode(
    _In_ ULONG Node)
{
    PIPC_DEVICE Device = IpcGlobal;
    ULONG i;

    if (!Device)
        return NULL;

    for (i = 0; i < Device->XportCount; i++)
    {
        if (Device->Xports[i].Up && Device->Xports[i].Remote.Node == Node)
            return &Device->Xports[i];
    }

    return NULL;
}

/* Reserve one send credit. 0 unlimited, 1 reserved, 3 window full */
static
ULONG
IpcTxReserve(
    _In_ PIPC_PORT Port,
    _Out_ PBOOLEAN ConfirmRx)
{
    ULONG Result;

    *ConfirmRx = FALSE;

    ExAcquireFastMutex(&Port->Lock);
    if (Port->TxQuota == 0)
    {
        Result = 0;
    }
    else if (Port->TxUsed < Port->TxQuota)
    {
        Port->TxUsed++;
        /* The last credit asks the peer to confirm so the window can reopen */
        *ConfirmRx = (Port->TxUsed == Port->TxQuota);
        Result = 1;
    }
    else
    {
        Result = 3;
    }
    ExReleaseFastMutex(&Port->Lock);

    return Result;
}

VOID
IpcTxResume(
    _In_ PIPC_PORT Port)
{
    ExAcquireFastMutex(&Port->Lock);
    Port->TxUsed = 0;
    ExReleaseFastMutex(&Port->Lock);
    KeSetEvent(&Port->TxEvent, 0, FALSE);
}

ULONG
IpcPortSendTo(
    _In_ PIPC_PORT Src,
    _In_ PIPC_ADDR Dst,
    _Inout_ PIPC_MBUF *Payload,
    _In_ LONG TimeoutMs)
{
    PIPC_PORT DstPort;
    BOOLEAN Confirm;
    ULONG Reserved, Status;
    LARGE_INTEGER Timeout;
    PLARGE_INTEGER TimeoutPtr;

    /* The window belongs to the destination, so reserve against that port */
    DstPort = IpcPortFind(Dst, TRUE);
    if (!DstPort)
        return IpcSendTo(&Src->Addr, Dst, 0, Payload);

    for (;;)
    {
        Reserved = IpcTxReserve(DstPort, &Confirm);
        if (Reserved != 3)
        {
            Status = IpcSendTo(&Src->Addr, Dst, Confirm ? 1 : 0, Payload);
            IpcPortRelease(DstPort);
            return Status;
        }

        /* Window full: clear, re-check to close the lost wakeup race, then wait */
        KeClearEvent(&DstPort->TxEvent);

        Reserved = IpcTxReserve(DstPort, &Confirm);
        if (Reserved != 3)
        {
            Status = IpcSendTo(&Src->Addr, Dst, Confirm ? 1 : 0, Payload);
            IpcPortRelease(DstPort);
            return Status;
        }

        if (TimeoutMs == QCOM_IPC_TIMEOUT_INFINITE)
        {
            TimeoutPtr = NULL;
        }
        else
        {
            Timeout.QuadPart = -((LONGLONG)TimeoutMs * 10000);
            TimeoutPtr = &Timeout;
        }

        if (KeWaitForSingleObject(&DstPort->TxEvent, Executive, KernelMode, FALSE, TimeoutPtr) == STATUS_TIMEOUT)
        {
            IpcMbufFreeChain(Payload);
            IpcPortRelease(DstPort);
            return IPC_EBUSY;
        }
    }
}

/*
 * A confirm_rx message addressed to Self was consumed. Reopen the flow the
 * sender holds toward Self: resume it directly when the sender is local, else
 * return a RESUME_TX naming Self so the remote resets its own copy.
 */
VOID
IpcSendResume(
    _In_ PIPC_ADDR Sender,
    _In_ PIPC_ADDR Self)
{
    PIPC_PORT Port;
    PIPC_XPORT Xport;
    IPC_CONTROL Body;

    if (Sender->Node == IpcLocalNodeId)
    {
        Port = IpcPortFind(Self, TRUE);
        if (Port)
        {
            IpcTxResume(Port);
            IpcPortRelease(Port);
        }
        return;
    }

    Xport = IpcXportForNode(Sender->Node);
    if (!Xport)
        return;

    RtlZeroMemory(&Body, sizeof(Body));
    Body.Command = IPC_TYPE_RESUME_TX;
    Body.Arg0 = Self->Node;
    Body.Arg1 = Self->Port;
    IpcControlSend(IPC_TYPE_RESUME_TX, Xport, NULL, &Body);
}

ULONG
IpcControlSend(
    _In_ ULONG Command,
    _In_opt_ PIPC_XPORT Xport,
    _In_opt_ PIPC_ADDR Dst,
    _In_ PVOID Body)
{
    IPC_HEADER Header;
    PIPC_MBUF Chain;
    ULONG Status;

    Chain = IpcPacketAllocate(Body, IPC_CONTROL_SIZE);
    if (!Chain)
        return IPC_ENOMEM;

    Header.Type = Command;
    Header.SrcNode = IpcLocalNodeId;
    Header.SrcPort = IPC_PORT_CONTROL;
    Header.ConfirmRx = 0;
    Header.Size = IPC_CONTROL_SIZE;
    Header.DstNode = Dst ? Dst->Node : (Xport ? Xport->Remote.Node : IPC_NODE_BROADCAST);
    Header.DstPort = IPC_PORT_CONTROL;

    if (IpcHeaderEncode(&Header, &Chain) != IPC_OK)
    {
        IpcMbufFreeChain(&Chain);
        return IPC_ENOMEM;
    }

    if (!Xport)
    {
        IpcMbufFreeChain(&Chain);
        return IPC_ENODEV;
    }

    /* IpcXportSend takes ownership of the chain on every path */
    Status = IpcXportSend(Xport, &Chain);
    return Status;
}

/* Announce every local server to a newly discovered node */
static
VOID
IpcAnnounceServers(
    _In_ PIPC_XPORT Xport)
{
    UNREFERENCED_PARAMETER(Xport);
    /* Local servers are published by clients through the bind path, and the
     * bind path broadcasts NEW_SERVER itself, so nothing is replayed here. */
}

static
VOID
IpcHandleHello(
    _In_ PIPC_XPORT Xport,
    _In_ PIPC_HEADER Header)
{
    IPC_CONTROL Body;

    Xport->Remote.Node = Header->SrcNode;
    Xport->Remote.Port = Header->SrcPort;
    Xport->Up = TRUE;

    if (!Xport->HelloSent)
    {
        RtlZeroMemory(&Body, sizeof(Body));
        Body.Command = IPC_TYPE_HELLO;
        if (IpcControlSend(IPC_TYPE_HELLO, Xport, NULL, &Body) == IPC_OK)
            Xport->HelloSent = TRUE;
    }

    IpcAnnounceServers(Xport);
}

static
VOID
IpcNotifyClients(
    _In_ ULONG Event,
    _In_ PIPC_ADDR Peer)
{
    /* Every local port with an event hook learns of server and client changes */
    IpcBroadcastEvent(IpcLocalNodeId, Event, Peer);
}

static
VOID
IpcHandleControl(
    _In_ PIPC_XPORT Xport,
    _In_ PIPC_HEADER Header,
    _In_ PIPC_CONTROL Body)
{
    IPC_ADDR Name, Addr;
    PIPC_PORT Port;

    switch (Header->Type)
    {
        case IPC_TYPE_HELLO:
            IpcHandleHello(Xport, Header);
            break;

        case IPC_TYPE_BYE:
            IpcServerRemoveByNode(Header->SrcNode);
            IpcPortRemoveByXport(Xport);
            Xport->Up = FALSE;
            break;

        case IPC_TYPE_NEW_SERVER:
            Name.Node = Body->Arg0;
            Name.Port = Body->Arg1;
            Addr.Node = Body->Arg2;
            Addr.Port = Body->Arg3;
            IpcPortInsert(&Addr, Xport, NULL, NULL, NULL, NULL, 0, NULL);
            IpcServerAdd(&Name, &Addr);
            IpcNotifyClients(IPC_EVENT_NEW_SERVER, &Name);
            break;

        case IPC_TYPE_REMOVE_SERVER:
            Name.Node = Body->Arg0;
            Name.Port = Body->Arg1;
            Addr.Node = Body->Arg2;
            Addr.Port = Body->Arg3;
            IpcServerRemove(&Name, &Addr);
            IpcNotifyClients(IPC_EVENT_REMOVE_SERVER, &Name);
            break;

        case IPC_TYPE_REMOVE_CLIENT:
            Addr.Node = Body->Arg0;
            Addr.Port = Body->Arg1;
            IpcPortRemove(&Addr);
            IpcNotifyClients(IPC_EVENT_REMOVE_CLIENT, &Addr);
            break;

        case IPC_TYPE_RESUME_TX:
            Addr.Node = Body->Arg0;
            Addr.Port = Body->Arg1;
            Port = IpcPortFind(&Addr, TRUE);
            if (Port)
            {
                IpcTxResume(Port);
                IpcPortRelease(Port);
            }
            break;

        default:
            break;
    }
}

static
VOID
IpcDeliverLocal(
    _In_ PIPC_HEADER Header,
    _Inout_ PIPC_MBUF *Payload)
{
    IPC_ADDR Addr;
    PIPC_PORT Port;

    Addr.Node = Header->DstNode;
    Addr.Port = Header->DstPort;

    Port = IpcPortFind(&Addr, TRUE);
    if (!Port || !Port->Rx)
    {
        IpcMbufFreeChain(Payload);
        IpcPortRelease(Port);
        return;
    }

    Port->Rx(Port, Port->RxContext, Payload, Header);
    IpcPortRelease(Port);
}

static
VOID
IpcForwardRemote(
    _In_ PIPC_HEADER Header,
    _Inout_ PIPC_MBUF *Payload)
{
    IPC_ADDR Addr;
    PIPC_PORT Port;
    PIPC_XPORT Xport;
    IPC_HEADER Copy;

    Addr.Node = Header->DstNode;
    Addr.Port = Header->DstPort;

    Port = IpcPortFind(&Addr, TRUE);
    Xport = Port ? Port->Xport : NULL;
    if (!Xport)
    {
        IpcMbufFreeChain(Payload);
        IpcPortRelease(Port);
        return;
    }

    Copy = *Header;
    if (IpcHeaderEncode(&Copy, Payload) == IPC_OK)
        IpcXportSend(Xport, Payload);
    else
        IpcMbufFreeChain(Payload);

    IpcPortRelease(Port);
}

#define IPC_MCAST_MAX_LOCAL 16

/*
 * Fan a multicast body out to every live transport except the one it arrived
 * on, and to every local port that serves the target {service, instance}. Each
 * target gets its own copy of the body, so the original is freed at the end.
 */
static
VOID
IpcForwardMulticast(
    _In_opt_ PIPC_XPORT Source,
    _In_ PIPC_HEADER Header,
    _Inout_ PIPC_MBUF *Payload)
{
    PIPC_DEVICE Device = IpcGlobal;
    IPC_ADDR Name;
    IPC_ADDR Locals[IPC_MCAST_MAX_LOCAL];
    IPC_HEADER Copy;
    PIPC_MBUF Clone;
    PIPC_PORT Port;
    ULONG LocalCount, i;

    if (!Device)
    {
        IpcMbufFreeChain(Payload);
        return;
    }

    for (i = 0; i < Device->XportCount; i++)
    {
        PIPC_XPORT Xport = &Device->Xports[i];
        if (!Xport->Up || Xport == Source)
            continue;

        Clone = IpcMbufClone(*Payload);
        if (!Clone)
            continue;

        Copy = *Header;
        if (IpcHeaderEncode(&Copy, &Clone) == IPC_OK)
            IpcXportSend(Xport, &Clone);
        else
            IpcMbufFreeChain(&Clone);
    }

    /* Deliver locally to every server bound to the multicast name */
    Name.Node = Header->DstNode;
    Name.Port = Header->DstPort;
    LocalCount = IpcServerCollectLocal(&Name, IpcLocalNodeId, Locals, IPC_MCAST_MAX_LOCAL);

    for (i = 0; i < LocalCount; i++)
    {
        Port = IpcPortFind(&Locals[i], TRUE);
        if (!Port)
            continue;

        if (Port->Rx)
        {
            Clone = IpcMbufClone(*Payload);
            if (Clone)
            {
                IPC_HEADER Local = *Header;
                Local.DstNode = Locals[i].Node;
                Local.DstPort = Locals[i].Port;
                Local.ConfirmRx = 0;
                Port->Rx(Port, Port->RxContext, &Clone, &Local);
            }
        }

        IpcPortRelease(Port);
    }

    IpcMbufFreeChain(Payload);
}

VOID
IpcReceive(
    _In_ PIPC_XPORT Xport,
    _Inout_ PIPC_MBUF *Chain)
{
    IPC_HEADER Header;
    IPC_CONTROL Body;
    ULONG Length;

    if (IpcHeaderDecode(Chain, &Header) != IPC_OK)
    {
        DPRINT1("ipc_router_core: illegible message from proc %lu\n", Xport->Remote.Node);
        IpcMbufFreeChain(Chain);
        return;
    }

    Length = IpcMbufChainLength(*Chain);
    if (Length < Header.Size || Length > Header.Size + 4)
    {
        DPRINT1("ipc_router_core: unexpected msg size %lu instead of %lu from proc %lu\n",
                Length, Header.Size, Xport->Remote.Node);
        IpcMbufFreeChain(Chain);
        return;
    }

    if (Header.Type == IPC_TYPE_MCAST)
    {
        IpcForwardMulticast(Xport, &Header, Chain);
        return;
    }

    if (Header.Type == IPC_TYPE_HELLO || Header.DstNode == IpcLocalNodeId)
    {
        if (Header.Type == IPC_TYPE_DATA)
        {
            IpcDeliverLocal(&Header, Chain);
            return;
        }

        if (IpcMbufCopyOut(Chain, &Body, IPC_CONTROL_SIZE) == IPC_CONTROL_SIZE)
            IpcHandleControl(Xport, &Header, &Body);
        else
            DPRINT1("ipc_router_core: control message too short\n");

        IpcMbufFreeChain(Chain);
        return;
    }

    IpcForwardRemote(&Header, Chain);
}

ULONG
IpcSendTo(
    _In_ PIPC_ADDR Src,
    _In_ PIPC_ADDR Dst,
    _In_ ULONG ConfirmRx,
    _Inout_ PIPC_MBUF *Payload)
{
    IPC_HEADER Header;
    PIPC_PORT Port;
    PIPC_XPORT Xport;
    ULONG Status;
    ULONG Size = IpcMbufChainLength(*Payload);

    Header.Type = IPC_TYPE_DATA;
    Header.SrcNode = Src->Node;
    Header.SrcPort = Src->Port;
    Header.ConfirmRx = ConfirmRx;
    Header.Size = Size;
    Header.DstNode = Dst->Node;
    Header.DstPort = Dst->Port;

    if (Dst->Node == IpcLocalNodeId)
    {
        IpcDeliverLocal(&Header, Payload);
        return IPC_OK;
    }

    Port = IpcPortFind(Dst, TRUE);
    Xport = Port ? Port->Xport : NULL;
    if (!Xport)
    {
        IpcMbufFreeChain(Payload);
        IpcPortRelease(Port);
        return IPC_ENODEV;
    }

    if (IpcHeaderEncode(&Header, Payload) != IPC_OK)
    {
        IpcMbufFreeChain(Payload);
        IpcPortRelease(Port);
        return IPC_ENOMEM;
    }

    Status = IpcXportSend(Xport, Payload);
    IpcPortRelease(Port);
    return Status;
}
