/*
 * PROJECT:     ReactOS Qualcomm IPC Router driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Private definitions
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#include <ntddk.h>
#include <wdf.h>
#include <initguid.h>
#include <qcom/smdif.h>

#define IPC_TAG                     'RCPI'

/* Non paged pool for all buffers, as the platform driver uses */
#define IPC_POOL                    NonPagedPool

/* Debug output uses the IHV driver filter, like the reference driver */
#define IPC_DPFLTR                  DPFLTR_IHVDRIVER_ID

/* Wire protocol version this driver speaks */
#define IPC_WIRE_VERSION            1
#define IPC_HEADER_SIZE             32

/* Control endpoint and well known node ids */
#define IPC_PORT_CONTROL            0xFFFFFFFE
#define IPC_NODE_BROADCAST          0xFFFFFFFF
#define IPC_PORT_BROADCAST          0xFFFFFFFE

/* Message types, index into IpcTypeNames */
#define IPC_TYPE_DATA               1
#define IPC_TYPE_HELLO              2
#define IPC_TYPE_BYE                3
#define IPC_TYPE_NEW_SERVER         4
#define IPC_TYPE_REMOVE_SERVER      5
#define IPC_TYPE_REMOVE_CLIENT      6
#define IPC_TYPE_RESUME_TX          7
#define IPC_TYPE_MCAST              8

/* Control message body, five little endian u32, Command mirrors the header Type */
#define IPC_CONTROL_SIZE            20

/* Router result codes, the client front ends map them to NTSTATUS */
#define IPC_OK                      0
#define IPC_ENODEV                  2
#define IPC_ENOMEM                  5
#define IPC_EEXIST                  7
#define IPC_EBUSY                   8
#define IPC_EINVAL                  11

/* Hashing: nodes bucket by id, ports by port, servers by service */
#define IPC_NODE_BUCKETS            4
#define IPC_PORT_BUCKETS            32
#define IPC_SERVER_BUCKETS          32

/* Server name entry used by the qsocket find/new interfaces */
#define IPC_NAME_PORT               27

/* SMD channel name length, matches the platform transport */
#define IPC_NAME_LENGTH             20

/* Transports this node may bring up, bounded by the registry NumTransports */
#define IPC_MAX_XPORTS              8

/* Priority options carried on a port, clamped to 0..3 */
#define IPC_PRIORITY_DEFAULT        2

/*
 * Outbound flow control window, in unconfirmed messages. When a local port has
 * this many sends outstanding it marks the next message confirm_rx and blocks
 * until the peer returns a RESUME_TX. Zero disables throttling for a port.
 */
#define IPC_TX_WINDOW_DEFAULT       16
#define IPC_TX_TIMEOUT_MS           5000

/*
 * Client visible control events, delivered through the read path and the
 * get-message IOCTL. The wire control type is mapped onto these values.
 */
#define IPC_EVENT_NEW_SERVER        0
#define IPC_EVENT_DATA              1
#define IPC_EVENT_REMOVE_SERVER     2
#define IPC_EVENT_REMOVE_CLIENT     3
#define IPC_EVENT_RESUME_TX         4

/* \Device\IPC control interface, FILE_DEVICE_NETWORK function codes 0x800.. */
#define IPC_IOCTL_GET_EVENT         CTL_CODE(FILE_DEVICE_NETWORK, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IPC_IOCTL_POLL_EVENT        CTL_CODE(FILE_DEVICE_NETWORK, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IPC_IOCTL_LOOKUP_SERVER     CTL_CODE(FILE_DEVICE_NETWORK, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IPC_IOCTL_BIND_NAME         CTL_CODE(FILE_DEVICE_NETWORK, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IPC_IOCTL_UNBIND_NAME       CTL_CODE(FILE_DEVICE_NETWORK, 0x804, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IPC_IOCTL_GET_SELF          CTL_CODE(FILE_DEVICE_NETWORK, 0x805, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IPC_IOCTL_SET_RX_ENABLED    CTL_CODE(FILE_DEVICE_NETWORK, 0x806, METHOD_BUFFERED, FILE_ANY_ACCESS)

typedef struct _IPC_DEVICE *PIPC_DEVICE;

/*
 * Message buffer fragment. Payload follows the header; Data walks forward as
 * bytes are consumed and backward as headers are prepended, Capacity counts
 * the bytes still reachable from Data.
 */
typedef struct _IPC_MBUF
{
    struct _IPC_MBUF *Next;
    ULONG Length;
    PUCHAR Data;
    PUCHAR Base;
    ULONG Capacity;
    VOID (NTAPI *FreeRoutine)(PVOID Base, PVOID Context);
    PVOID FreeContext;
    PVOID Reserved;
    ULONG Canary;
} IPC_MBUF, *PIPC_MBUF;

#define IPC_MBUF_CANARY             0xDEADBEEF
#define IPC_MBUF_HEADROOM           32

C_ASSERT(FIELD_OFFSET(IPC_MBUF, Length) == 0x08);
C_ASSERT(FIELD_OFFSET(IPC_MBUF, Data) == 0x10);
C_ASSERT(FIELD_OFFSET(IPC_MBUF, Capacity) == 0x20);
C_ASSERT(FIELD_OFFSET(IPC_MBUF, Canary) == 0x40);

/* Wire header, version 1, all fields little endian u32 */
#include <pshpack4.h>
typedef struct _IPC_HEADER
{
    ULONG Version;
    ULONG Type;
    ULONG SrcNode;
    ULONG SrcPort;
    ULONG ConfirmRx;
    ULONG Size;
    ULONG DstNode;
    ULONG DstPort;
} IPC_HEADER, *PIPC_HEADER;
#include <poppack.h>

C_ASSERT(sizeof(IPC_HEADER) == IPC_HEADER_SIZE);

typedef struct _IPC_ADDR
{
    ULONG Node;
    ULONG Port;
} IPC_ADDR, *PIPC_ADDR;

typedef struct _IPC_PORT IPC_PORT, *PIPC_PORT;
typedef struct _IPC_XPORT IPC_XPORT, *PIPC_XPORT;

/*
 * Receive hook for a local endpoint. Takes ownership of Payload, which is a
 * mbuf chain holding the unpadded message body.
 */
typedef ULONG
(NTAPI *PIPC_PORT_RX)(
    _In_ PIPC_PORT Port,
    _In_opt_ PVOID Context,
    _Inout_ PIPC_MBUF *Payload,
    _In_ PIPC_HEADER Header);

typedef VOID
(NTAPI *PIPC_PORT_EVENT)(
    _In_ PIPC_PORT Port,
    _In_opt_ PVOID Context,
    _In_ ULONG Event,
    _In_ PIPC_ADDR Peer);

/* A routable endpoint. Local ports carry callbacks, remote ports a transport */
struct _IPC_PORT
{
    LIST_ENTRY BucketLink;
    IPC_ADDR Addr;
    PIPC_XPORT Xport;
    LONG RefCount;
    ULONG Priority;
    ULONG State;
    PIPC_XPORT HelloPeer;
    FAST_MUTEX Lock;
    KEVENT TxEvent;
    KEVENT RxEvent;
    PIPC_PORT_RX Rx;
    PVOID RxContext;
    PIPC_PORT_EVENT Event;
    PVOID EventContext;
    VOID (NTAPI *OnDestroy)(PVOID Context);
    PVOID OnDestroyContext;
    ULONG TxQuota;
    ULONG TxUsed;
};

/* One SMD channel brought up as a router transport */
struct _IPC_XPORT
{
    LIST_ENTRY Link;
    PIPC_DEVICE Device;
    CHAR Name[IPC_NAME_LENGTH];
    ULONG Type;
    ULONG Edge;
    ULONG Flags;
    ULONG FifoSize;
    IPC_ADDR Remote;
    LONG RefCount;
    BOOLEAN Up;
    BOOLEAN HelloSent;
    BOOLEAN ChannelOpen;
    QCOM_SMDL_HANDLE Channel;
    WDFWORKITEM RxWork;
};

typedef struct _IPC_DEVICE
{
    WDFDEVICE WdfDevice;
    QCOM_SMDL_INTERFACE Smdl;
    WDFIOTARGET SmdTarget;
    BOOLEAN SmdlReady;
    ULONG LocalNode;
    ULONG XportCount;
    IPC_XPORT Xports[IPC_MAX_XPORTS];
} IPC_DEVICE;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(IPC_DEVICE, IpcGetDevice);

/* The exported function tables carry no context, so there is a single router */
extern PIPC_DEVICE IpcGlobal;

/* Dynamic port numbers start here and wrap below the control port */
#define IPC_PORT_DYNAMIC_FIRST      1
#define IPC_PORT_DYNAMIC_LAST       0xFFFEFFFE

/* wire.c */
PIPC_MBUF
IpcMbufAllocate(
    _In_ ULONG Headroom,
    _In_ ULONG Length);

VOID
IpcMbufFree(
    _In_opt_ PIPC_MBUF Mbuf);

VOID
IpcMbufFreeChain(
    _Inout_ PIPC_MBUF *Head);

ULONG
IpcMbufPrepend(
    _Inout_ PIPC_MBUF *Head,
    _In_reads_bytes_(Length) PVOID Source,
    _In_ ULONG Length);

ULONG
IpcMbufAppend(
    _Inout_ PIPC_MBUF *Head,
    _In_reads_bytes_(Length) PVOID Source,
    _In_ ULONG Length);

ULONG
IpcMbufCopyOut(
    _Inout_ PIPC_MBUF *Head,
    _Out_writes_bytes_(Length) PVOID Destination,
    _In_ ULONG Length);

PVOID
IpcMbufPeek(
    _In_opt_ PIPC_MBUF Head,
    _In_ ULONG Length);

ULONG
IpcMbufChainLength(
    _In_opt_ PIPC_MBUF Head);

PIPC_MBUF
IpcMbufClone(
    _In_opt_ PIPC_MBUF Head);

PIPC_MBUF
IpcPacketAllocate(
    _In_reads_bytes_(Length) PVOID Payload,
    _In_ ULONG Length);

ULONG
IpcHeaderEncode(
    _Inout_ PIPC_HEADER Header,
    _Inout_ PIPC_MBUF *Chain);

ULONG
IpcHeaderDecode(
    _Inout_ PIPC_MBUF *Chain,
    _Out_ PIPC_HEADER Header);

extern const char *IpcTypeNames[8];

/* database.c */
VOID
IpcDbInitialize(VOID);

VOID
IpcDbTeardown(VOID);

PIPC_PORT
IpcPortFind(
    _In_ PIPC_ADDR Addr,
    _In_ BOOLEAN Reference);

ULONG
IpcPortInsert(
    _In_ PIPC_ADDR Addr,
    _In_opt_ PIPC_XPORT Xport,
    _In_ PIPC_PORT_RX Rx,
    _In_opt_ PVOID RxContext,
    _In_opt_ PIPC_PORT_EVENT Event,
    _In_opt_ PVOID EventContext,
    _In_ ULONG Priority,
    _Outptr_opt_ PIPC_PORT *Port);

VOID
IpcPortRelease(
    _In_opt_ PIPC_PORT Port);

VOID
IpcPortRemove(
    _In_ PIPC_ADDR Addr);

VOID
IpcPortRemoveByXport(
    _In_ PIPC_XPORT Xport);

ULONG
IpcServerAdd(
    _In_ PIPC_ADDR Name,
    _In_ PIPC_ADDR Addr);

ULONG
IpcServerRemove(
    _In_ PIPC_ADDR Name,
    _In_ PIPC_ADDR Addr);

PIPC_PORT
IpcServerLookup(
    _In_ PIPC_ADDR Name);

ULONG
IpcServerCollectLocal(
    _In_ PIPC_ADDR Name,
    _In_ ULONG LocalNode,
    _Out_writes_to_(Max, return) PIPC_ADDR Addrs,
    _In_ ULONG Max);

VOID
IpcServerRemoveByNode(
    _In_ ULONG Node);

VOID
IpcPortSetDestroyHook(
    _In_ PIPC_PORT Port,
    _In_ VOID (NTAPI *OnDestroy)(PVOID Context),
    _In_opt_ PVOID Context);

VOID
IpcBroadcastEvent(
    _In_ ULONG LocalNode,
    _In_ ULONG Event,
    _In_ PIPC_ADDR Peer);

/* core.c */
NTSTATUS
IpcCoreInitialize(
    _In_ ULONG LocalNode);

VOID
IpcCoreTeardown(VOID);

ULONG
IpcLocalNode(VOID);

ULONG
IpcAddressAllocate(VOID);

ULONG
IpcControlSend(
    _In_ ULONG Command,
    _In_opt_ PIPC_XPORT Xport,
    _In_opt_ PIPC_ADDR Dst,
    _In_ PVOID Body);

VOID
IpcReceive(
    _In_ PIPC_XPORT Xport,
    _Inout_ PIPC_MBUF *Chain);

ULONG
IpcSendTo(
    _In_ PIPC_ADDR Src,
    _In_ PIPC_ADDR Dst,
    _In_ ULONG ConfirmRx,
    _Inout_ PIPC_MBUF *Payload);

ULONG
IpcPortSendTo(
    _In_ PIPC_PORT Src,
    _In_ PIPC_ADDR Dst,
    _Inout_ PIPC_MBUF *Payload,
    _In_ LONG TimeoutMs);

VOID
IpcTxResume(
    _In_ PIPC_PORT Port);

VOID
IpcSendResume(
    _In_ PIPC_ADDR Sender,
    _In_ PIPC_ADDR Self);

/* transport.c */
NTSTATUS
IpcSmdlAcquire(
    _Inout_ PIPC_DEVICE Device);

VOID
IpcSmdlRelease(
    _Inout_ PIPC_DEVICE Device);

NTSTATUS
IpcXportStartAll(
    _Inout_ PIPC_DEVICE Device);

VOID
IpcXportStopAll(
    _Inout_ PIPC_DEVICE Device);

ULONG
IpcXportSend(
    _In_ PIPC_XPORT Xport,
    _Inout_ PIPC_MBUF *Chain);

/* client.c */
VOID
IpcFileObjectConfig(
    _In_ PWDFDEVICE_INIT DeviceInit);

NTSTATUS
IpcCreateQueue(
    _In_ WDFDEVICE WdfDevice);

/* qsocket.c */
NTSTATUS
IpcQsocketPublish(
    _In_ WDFDEVICE WdfDevice);

/* main.c */
NTSTATUS
IpcReadTransports(
    _In_ WDFDEVICE WdfDevice,
    _Inout_ PIPC_DEVICE Device);
