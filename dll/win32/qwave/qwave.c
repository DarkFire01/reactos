/*
 * PROJECT:     ReactOS Quality Windows Audio/Video Experience
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     qWAVE client API with in-process flow bookkeeping
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <winsock2.h>
#include <mswsock.h>
#include <qos2.h>

/*
 * Windows keeps flows in the qWAVE service and reaches it through a driver.
 * ReactOS has neither, so flows live in this process. Only non-adaptive flows
 * exist: bandwidth estimation is not available, which Windows reports as
 * ERROR_NOT_SUPPORTED. ReactOS tcpip ignores IP_TOS, so DSCP values are kept
 * and reported but packets are not marked.
 */

/* Largest Size the request path accepts */
#define QWAVE_MAX_REQUEST_SIZE 0x21BC

/* Rate stored when the caller passes QOS_OUTGOING_DEFAULT_MINIMUM_BANDWIDTH */
#define QWAVE_DEFAULT_MINIMUM_RATE 250000

/* No DSCP set through QOSSetOutgoingDSCPValue */
#define QWAVE_DSCP_FROM_TYPE ((ULONG)-1)

typedef struct _QWAVE_CLIENT
{
    struct _QWAVE_CLIENT *Next;
    HANDLE Handle;
} QWAVE_CLIENT, *PQWAVE_CLIENT;

typedef struct _QWAVE_FLOW
{
    struct _QWAVE_FLOW *Next;
    PQWAVE_CLIENT Owner;
    QOS_FLOWID Id;
    QOS_TRAFFIC_TYPE TrafficType;
    ULONG Dscp;
    UINT64 Rate;
    QOS_SHAPING Shaping;

    /* Adds minus removes; removing the last one ends the flow */
    ULONG SocketCount;
} QWAVE_FLOW, *PQWAVE_FLOW;

/* What QOSAddSocketToFlow learns about a socket before it looks at flows */
typedef struct _QWAVE_SOCKET_INFO
{
    BOOL HaveProtocol;
    INT AddressFamily;
    INT PeerError;
    SOCKADDR_STORAGE Peer;
} QWAVE_SOCKET_INFO, *PQWAVE_SOCKET_INFO;

static CRITICAL_SECTION QwLock;
static PQWAVE_CLIENT QwClients;

/* Kept in creation order, the order QOSEnumerateFlows lists them in */
static PQWAVE_FLOW QwFlows;
static QOS_FLOWID QwNextFlowId = 1;

/* Conformant DSCP for each QOS_TRAFFIC_TYPE */
static const UCHAR QwTypeDscp[] = { 0, 8, 40, 40, 56, 56 };

static
PQWAVE_CLIENT
QwFindClient(
    _In_ HANDLE Handle)
{
    PQWAVE_CLIENT Client;

    for (Client = QwClients; Client; Client = Client->Next)
    {
        if (Client->Handle == Handle)
            return Client;
    }

    return NULL;
}

/* Flow IDs are unique across handles, like the ones the qWAVE service hands out */
static
PQWAVE_FLOW
QwFindFlow(
    _In_ QOS_FLOWID Id)
{
    PQWAVE_FLOW Flow;

    for (Flow = QwFlows; Flow; Flow = Flow->Next)
    {
        if (Flow->Id == Id)
            return Flow;
    }

    return NULL;
}

static
PQWAVE_FLOW
QwFindOwnFlow(
    _In_ PQWAVE_CLIENT Client,
    _In_ QOS_FLOWID Id)
{
    PQWAVE_FLOW Flow = QwFindFlow(Id);

    return (Flow && Flow->Owner == Client) ? Flow : NULL;
}

static
VOID
QwDeleteFlow(
    _In_ PQWAVE_FLOW Flow)
{
    PQWAVE_FLOW *Link;

    for (Link = &QwFlows; *Link; Link = &(*Link)->Next)
    {
        if (*Link == Flow)
        {
            *Link = Flow->Next;
            break;
        }
    }

    HeapFree(GetProcessHeap(), 0, Flow);
}

/* Flow DSCP changes, like the QOSEnumerateFlows list, need an administrator */
static
BOOL
QwIsAdministrator(VOID)
{
    SID_IDENTIFIER_AUTHORITY NtAuthority = {SECURITY_NT_AUTHORITY};
    PSID AdminSid;
    BOOL IsMember = FALSE;

    if (!AllocateAndInitializeSid(&NtAuthority,
                                  2,
                                  SECURITY_BUILTIN_DOMAIN_RID,
                                  DOMAIN_ALIAS_RID_ADMINS,
                                  0, 0, 0, 0, 0, 0,
                                  &AdminSid))
    {
        return FALSE;
    }

    if (!CheckTokenMembership(NULL, AdminSid, &IsMember))
        IsMember = FALSE;

    FreeSid(AdminSid);
    return IsMember;
}

/* IPv4 address carried by an IPv4-mapped, 6to4 or ISATAP IPv6 address */
static
BOOL
QwEmbeddedIpv4(
    _In_ const IN6_ADDR *Address,
    _Out_ PULONG Ipv4)
{
    const UCHAR *Bytes = Address->u.Byte;
    static const UCHAR Mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};

    if (Bytes[0] == 0x20 && Bytes[1] == 0x02)
    {
        CopyMemory(Ipv4, &Bytes[2], sizeof(*Ipv4));
        return TRUE;
    }

    if (RtlEqualMemory(Bytes, Mapped, sizeof(Mapped)) ||
        ((Bytes[8] & ~0x02) == 0 && Bytes[9] == 0 && Bytes[10] == 0x5E && Bytes[11] == 0xFE))
    {
        CopyMemory(Ipv4, &Bytes[12], sizeof(*Ipv4));
        return TRUE;
    }

    return FALSE;
}

/* Puts an address in the form two endpoints are compared in */
static
VOID
QwCanonicalAddress(
    _In_ const SOCKADDR *Address,
    _Out_ PSOCKADDR_IN6 Canonical)
{
    ULONG Ipv4;

    ZeroMemory(Canonical, sizeof(*Canonical));
    if (Address->sa_family == AF_INET6)
    {
        const SOCKADDR_IN6 *Address6 = (const SOCKADDR_IN6 *)Address;

        if (!QwEmbeddedIpv4(&Address6->sin6_addr, &Ipv4))
        {
            Canonical->sin6_family = AF_INET6;
            Canonical->sin6_addr = Address6->sin6_addr;
            Canonical->sin6_scope_id = Address6->sin6_scope_id;
            return;
        }
    }
    else
    {
        Ipv4 = ((const SOCKADDR_IN *)Address)->sin_addr.s_addr;
    }

    Canonical->sin6_family = AF_INET;
    CopyMemory(&Canonical->sin6_addr, &Ipv4, sizeof(Ipv4));
}

static
BOOL
QwSameEndpoint(
    _In_ const SOCKADDR *Left,
    _In_ const SOCKADDR *Right)
{
    SOCKADDR_IN6 LeftAddress, RightAddress;

    /* sin_port and sin6_port share an offset */
    if (((const SOCKADDR_IN *)Left)->sin_port != ((const SOCKADDR_IN *)Right)->sin_port)
        return FALSE;

    QwCanonicalAddress(Left, &LeftAddress);
    QwCanonicalAddress(Right, &RightAddress);
    return RtlEqualMemory(&LeftAddress, &RightAddress, sizeof(LeftAddress));
}

/* Traffic to these never leaves the host, so no flow can be admitted for it */
static
BOOL
QwIsLoopback(
    _In_ const SOCKADDR *Address)
{
    SOCKADDR_IN6 Canonical;

    QwCanonicalAddress(Address, &Canonical);
    if (Canonical.sin6_family == AF_INET)
        return Canonical.sin6_addr.u.Byte[0] == 127;

    return IN6_IS_ADDR_LOOPBACK(&Canonical.sin6_addr);
}

static
BOOL
QwIsIpFamily(
    _In_ const SOCKADDR *Address)
{
    return Address->sa_family == AF_INET || Address->sa_family == AF_INET6;
}

/* The socket every layered provider sits on, which is what flows are keyed by */
static
BOOL
QwGetBaseSocket(
    _In_ SOCKET Socket,
    _Out_ SOCKET *Base)
{
    DWORD Returned = 0;

    return WSAIoctl(Socket,
                    SIO_BASE_HANDLE,
                    NULL,
                    0,
                    Base,
                    sizeof(*Base),
                    &Returned,
                    NULL,
                    NULL) != SOCKET_ERROR;
}

static
VOID
QwQuerySocket(
    _In_ SOCKET Socket,
    _Out_ PQWAVE_SOCKET_INFO Info)
{
    WSAPROTOCOL_INFOW Protocol;
    int Length;

    ZeroMemory(Info, sizeof(*Info));

    Length = sizeof(Protocol);
    if (getsockopt(Socket, SOL_SOCKET, SO_PROTOCOL_INFOW, (char *)&Protocol, &Length) == SOCKET_ERROR)
        return;

    Info->HaveProtocol = TRUE;
    Info->AddressFamily = Protocol.iAddressFamily;

    Length = sizeof(Info->Peer);
    if (getpeername(Socket, (PSOCKADDR)&Info->Peer, &Length) == SOCKET_ERROR)
        Info->PeerError = WSAGetLastError();
}

/*
 * Requests that carry a buffer fail before reaching the flow when the size is
 * over the limit, and a missing buffer fails on how it would have been mapped.
 */
static
DWORD
QwCheckRequestBuffer(
    _In_ ULONG Size,
    _In_opt_ PVOID Buffer)
{
    if (Size > QWAVE_MAX_REQUEST_SIZE)
        return ERROR_INVALID_PARAMETER;

    if (!Buffer)
        return Size ? ERROR_INVALID_PARAMETER : ERROR_NOACCESS;

    return ERROR_SUCCESS;
}

/*
 * Ends a request the way an I/O on the qWAVE handle ends. With an OVERLAPPED a
 * zero byte write on the handle completes it, so the event, a completion port
 * and GetOverlappedResult all see it.
 */
static
BOOL
QwFinishRequest(
    _In_ HANDLE Handle,
    _In_ DWORD Error,
    _Inout_opt_ LPOVERLAPPED Overlapped)
{
    static UCHAR Empty;

    if (Error != ERROR_SUCCESS)
    {
        SetLastError(Error);
        return FALSE;
    }

    if (!Overlapped)
        return TRUE;

    if (!WriteFile(Handle, &Empty, 0, NULL, Overlapped) && GetLastError() != ERROR_IO_PENDING)
        return FALSE;

    SetLastError(ERROR_IO_PENDING);
    return FALSE;
}

BOOL
WINAPI
QOSCreateHandle(
    _In_ PQOS_VERSION Version,
    _Out_ PHANDLE QOSHandle)
{
    PQWAVE_CLIENT Client;
    HANDLE Handle;

    if (!QOSHandle || !Version || Version->MajorVersion != 1 || Version->MinorVersion != 0)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    Client = HeapAlloc(GetProcessHeap(), 0, sizeof(*Client));
    if (!Client)
    {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }

    /* A real file handle, so CloseHandle, CancelIoEx and completion ports work on it */
    Handle = CreateFileW(L"\\\\.\\NUL",
                         GENERIC_READ | GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE,
                         NULL,
                         OPEN_EXISTING,
                         FILE_FLAG_OVERLAPPED,
                         NULL);
    if (Handle == INVALID_HANDLE_VALUE)
    {
        HeapFree(GetProcessHeap(), 0, Client);

        /* What Windows reports when it cannot reach the qWAVE driver */
        SetLastError(ERROR_SERVICE_DEPENDENCY_FAIL);
        return FALSE;
    }

    Client->Handle = Handle;
    EnterCriticalSection(&QwLock);
    Client->Next = QwClients;
    QwClients = Client;
    LeaveCriticalSection(&QwLock);

    *QOSHandle = Handle;
    return TRUE;
}

BOOL
WINAPI
QOSCloseHandle(
    _In_ HANDLE QOSHandle)
{
    PQWAVE_CLIENT *ClientLink;
    PQWAVE_FLOW *FlowLink;

    EnterCriticalSection(&QwLock);
    for (ClientLink = &QwClients; *ClientLink; ClientLink = &(*ClientLink)->Next)
    {
        PQWAVE_CLIENT Client = *ClientLink;

        if (Client->Handle != QOSHandle)
            continue;

        /* The flows go with the handle */
        FlowLink = &QwFlows;
        while (*FlowLink)
        {
            PQWAVE_FLOW Flow = *FlowLink;

            if (Flow->Owner == Client)
            {
                *FlowLink = Flow->Next;
                HeapFree(GetProcessHeap(), 0, Flow);
            }
            else
            {
                FlowLink = &Flow->Next;
            }
        }

        *ClientLink = Client->Next;
        HeapFree(GetProcessHeap(), 0, Client);
        break;
    }
    LeaveCriticalSection(&QwLock);

    /* Windows hands any handle straight to CloseHandle */
    return CloseHandle(QOSHandle);
}

BOOL
WINAPI
QOSCancel(
    _In_ HANDLE QOSHandle,
    _In_ LPOVERLAPPED Overlapped)
{
    /* Requests complete before returning, so there is never one to cancel */
    return CancelIoEx(QOSHandle, Overlapped);
}

BOOL
WINAPI
QOSAddSocketToFlow(
    _In_ HANDLE QOSHandle,
    _In_ SOCKET Socket,
    _In_opt_ PSOCKADDR DestAddr,
    _In_ QOS_TRAFFIC_TYPE TrafficType,
    _In_opt_ DWORD Flags,
    _Inout_ PQOS_FLOWID FlowId)
{
    QWAVE_SOCKET_INFO Info;
    PQWAVE_CLIENT Client;
    PQWAVE_FLOW Flow;
    const SOCKADDR *Target;
    SOCKET Base;
    DWORD Error = ERROR_SUCCESS;

    if (!FlowId || (DestAddr && !QwIsIpFamily(DestAddr)))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    /* Winsock has already set the error */
    if (!QwGetBaseSocket(Socket, &Base))
        return FALSE;

    QwQuerySocket(Base, &Info);

    EnterCriticalSection(&QwLock);
    Client = QwFindClient(QOSHandle);
    if (!Client)
    {
        Error = ERROR_INVALID_HANDLE;
        goto Quit;
    }

    /* A connected socket has its own destination, which DestAddr may only repeat */
    if (!Info.HaveProtocol)
    {
        Error = ERROR_INVALID_PARAMETER;
        goto Quit;
    }
    else if (Info.PeerError)
    {
        if (Info.PeerError != WSAENOTCONN || !DestAddr || DestAddr->sa_family != Info.AddressFamily)
        {
            Error = ERROR_INVALID_PARAMETER;
            goto Quit;
        }
        Target = DestAddr;
    }
    else
    {
        if (DestAddr && !QwSameEndpoint((PSOCKADDR)&Info.Peer, DestAddr))
        {
            Error = ERROR_INVALID_PARAMETER;
            goto Quit;
        }
        Target = (PSOCKADDR)&Info.Peer;
    }

    if ((ULONG)TrafficType > QOSTrafficTypeControl || (Flags & ~QOS_NON_ADAPTIVE_FLOW))
    {
        Error = ERROR_INVALID_PARAMETER;
        goto Quit;
    }

    if (!(Flags & QOS_NON_ADAPTIVE_FLOW))
    {
        Error = ERROR_NOT_SUPPORTED;
        goto Quit;
    }

    if (*FlowId)
    {
        Flow = QwFindOwnFlow(Client, *FlowId);
        if (!Flow || QwIsLoopback(Target))
            Error = ERROR_NOT_FOUND;
        else if (Flow->SocketCount == MAXDWORD)
            Error = ERROR_NO_SYSTEM_RESOURCES;
        else
            Flow->SocketCount++;
        goto Quit;
    }

    if (QwIsLoopback(Target))
    {
        Error = ERROR_NOT_FOUND;
        goto Quit;
    }

    Flow = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*Flow));
    if (!Flow)
    {
        Error = ERROR_NOT_ENOUGH_MEMORY;
        goto Quit;
    }

    Flow->Owner = Client;
    Flow->Id = QwNextFlowId++;
    if (!QwNextFlowId)
        QwNextFlowId = 1;
    Flow->TrafficType = TrafficType;
    Flow->Dscp = QWAVE_DSCP_FROM_TYPE;
    Flow->Shaping = QOSShapeOnly;
    Flow->SocketCount = 1;

    {
        PQWAVE_FLOW *Link = &QwFlows;

        while (*Link)
            Link = &(*Link)->Next;
        *Link = Flow;
    }

    *FlowId = Flow->Id;

Quit:
    LeaveCriticalSection(&QwLock);
    return QwFinishRequest(QOSHandle, Error, NULL);
}

BOOL
WINAPI
QOSRemoveSocketFromFlow(
    _In_ HANDLE QOSHandle,
    _In_opt_ SOCKET Socket,
    _In_ QOS_FLOWID FlowId,
    _Reserved_ DWORD Flags)
{
    PQWAVE_CLIENT Client;
    PQWAVE_FLOW Flow;
    SOCKET Base;
    DWORD Error = ERROR_SUCCESS;

    if (Socket && !QwGetBaseSocket(Socket, &Base))
        return FALSE;

    EnterCriticalSection(&QwLock);
    Client = QwFindClient(QOSHandle);
    if (!Client)
    {
        Error = ERROR_INVALID_HANDLE;
        goto Quit;
    }

    if (Flags)
    {
        Error = ERROR_INVALID_PARAMETER;
        goto Quit;
    }

    Flow = QwFindOwnFlow(Client, FlowId);
    if (!Flow)
    {
        Error = ERROR_NOT_FOUND;
        goto Quit;
    }

    /* No socket means the whole flow */
    if (!Socket || Flow->SocketCount == 1)
        QwDeleteFlow(Flow);
    else
        Flow->SocketCount--;

Quit:
    LeaveCriticalSection(&QwLock);
    return QwFinishRequest(QOSHandle, Error, NULL);
}

static
DWORD
QwSetTrafficType(
    _Inout_ PQWAVE_FLOW Flow,
    _In_ ULONG Size,
    _In_ const ULONG *Value)
{
    if (Size != sizeof(*Value) || *Value > QOSTrafficTypeControl)
        return ERROR_INVALID_PARAMETER;

    /* A new type replaces any DSCP value set before */
    Flow->TrafficType = *Value;
    Flow->Dscp = QWAVE_DSCP_FROM_TYPE;
    return ERROR_SUCCESS;
}

static
DWORD
QwSetOutgoingRate(
    _Inout_ PQWAVE_FLOW Flow,
    _In_ ULONG Size,
    _In_ const QOS_FLOWRATE_OUTGOING *Rate)
{
    if (Size != sizeof(*Rate) ||
        (ULONG)Rate->ShapingBehavior > QOSUseNonConformantMarkings ||
        (ULONG)Rate->Reason > QOSFlowRateUserCaused)
    {
        return ERROR_INVALID_PARAMETER;
    }

    /* A flow without a rate cannot start at zero */
    if (!Flow->Rate && !Rate->Bandwidth)
        return ERROR_INVALID_PARAMETER;

    if (Rate->Bandwidth == QOS_OUTGOING_DEFAULT_MINIMUM_BANDWIDTH)
        Flow->Rate = QWAVE_DEFAULT_MINIMUM_RATE;
    else
        Flow->Rate = Rate->Bandwidth;

    Flow->Shaping = Rate->ShapingBehavior;
    return ERROR_SUCCESS;
}

static
DWORD
QwSetDscp(
    _Inout_ PQWAVE_FLOW Flow,
    _In_ ULONG Size,
    _In_ const ULONG *Value)
{
    if (!QwIsAdministrator())
        return ERROR_ACCESS_DENIED;

    if (Size != sizeof(*Value) || *Value > 63)
        return ERROR_INVALID_PARAMETER;

    Flow->Dscp = *Value;
    return ERROR_SUCCESS;
}

BOOL
WINAPI
QOSSetFlow(
    _In_ HANDLE QOSHandle,
    _In_ QOS_FLOWID FlowId,
    _In_ QOS_SET_FLOW Operation,
    _In_ ULONG Size,
    _In_reads_bytes_(Size) PVOID Buffer,
    _Reserved_ DWORD Flags,
    _Out_opt_ LPOVERLAPPED Overlapped)
{
    PQWAVE_CLIENT Client;
    PQWAVE_FLOW Flow;
    DWORD Error;

    EnterCriticalSection(&QwLock);
    Client = QwFindClient(QOSHandle);
    if (!Client)
    {
        Error = ERROR_INVALID_HANDLE;
        goto Quit;
    }

    Error = QwCheckRequestBuffer(Size, Buffer);
    if (Error != ERROR_SUCCESS)
        goto Quit;

    if (Flags)
    {
        Error = ERROR_INVALID_PARAMETER;
        goto Quit;
    }

    Flow = QwFindOwnFlow(Client, FlowId);
    if (!Flow)
    {
        Error = ERROR_NOT_FOUND;
        goto Quit;
    }

    switch (Operation)
    {
        case QOSSetTrafficType:
            Error = QwSetTrafficType(Flow, Size, Buffer);
            break;

        case QOSSetOutgoingRate:
            Error = QwSetOutgoingRate(Flow, Size, Buffer);
            break;

        case QOSSetOutgoingDSCPValue:
            Error = QwSetDscp(Flow, Size, Buffer);
            break;

        default:
            Error = ERROR_INVALID_PARAMETER;
            break;
    }

Quit:
    LeaveCriticalSection(&QwLock);
    return QwFinishRequest(QOSHandle, Error, Overlapped);
}

BOOL
WINAPI
QOSQueryFlow(
    _In_ HANDLE QOSHandle,
    _In_ QOS_FLOWID FlowId,
    _In_ QOS_QUERY_FLOW Operation,
    _Inout_ PULONG Size,
    _Out_writes_bytes_(*Size) PVOID Buffer,
    _In_opt_ DWORD Flags,
    _Out_opt_ LPOVERLAPPED Overlapped)
{
    PQOS_PACKET_PRIORITY Priority;
    PQWAVE_CLIENT Client;
    PQWAVE_FLOW Flow;
    DWORD Error;

    EnterCriticalSection(&QwLock);
    Client = QwFindClient(QOSHandle);
    if (!Client)
    {
        Error = ERROR_INVALID_HANDLE;
        goto Quit;
    }

    if (!Size)
    {
        Error = ERROR_INVALID_PARAMETER;
        goto Quit;
    }

    Error = QwCheckRequestBuffer(*Size, Buffer);
    if (Error != ERROR_SUCCESS)
        goto Quit;

    /* Administrators may look at flows other handles own */
    Flow = QwFindFlow(FlowId);
    if (!Flow || (Flow->Owner != Client && !QwIsAdministrator()))
    {
        Error = ERROR_NOT_FOUND;
        goto Quit;
    }

    switch (Operation)
    {
        case QOSQueryFlowFundamentals:
            /* Only adaptive flows have path measurements */
            Error = ERROR_NOT_SUPPORTED;
            break;

        case QOSQueryPacketPriority:
            if (Flags || *Size < sizeof(*Priority))
            {
                Error = ERROR_INVALID_PARAMETER;
                break;
            }

            *Size = sizeof(*Priority);
            Priority = Buffer;
            if (Flow->Dscp == QWAVE_DSCP_FROM_TYPE)
                Priority->ConformantDSCPValue = QwTypeDscp[Flow->TrafficType];
            else
                Priority->ConformantDSCPValue = Flow->Dscp;

            /* Non-conformant traffic is not remarked and no adapter tags 802.1p */
            Priority->NonConformantDSCPValue = 0;
            Priority->ConformantL2Value = 0;
            Priority->NonConformantL2Value = 0;
            break;

        case QOSQueryOutgoingRate:
            if (Flags || *Size < sizeof(Flow->Rate))
            {
                Error = ERROR_INVALID_PARAMETER;
                break;
            }

            *Size = sizeof(Flow->Rate);
            CopyMemory(Buffer, &Flow->Rate, sizeof(Flow->Rate));
            break;

        default:
            Error = ERROR_INVALID_PARAMETER;
            break;
    }

Quit:
    LeaveCriticalSection(&QwLock);
    return QwFinishRequest(QOSHandle, Error, Overlapped);
}

BOOL
WINAPI
QOSNotifyFlow(
    _In_ HANDLE QOSHandle,
    _In_ QOS_FLOWID FlowId,
    _In_ QOS_NOTIFY_FLOW Operation,
    _Inout_opt_ PULONG Size,
    _Inout_updates_bytes_opt_(*Size) PVOID Buffer,
    _Reserved_ DWORD Flags,
    _Out_opt_ LPOVERLAPPED Overlapped)
{
    PQWAVE_CLIENT Client;
    DWORD Error;

    UNREFERENCED_PARAMETER(Operation);
    UNREFERENCED_PARAMETER(Buffer);

    EnterCriticalSection(&QwLock);
    Client = QwFindClient(QOSHandle);
    if (!Client)
        Error = ERROR_INVALID_HANDLE;
    else if ((Size && *Size > QWAVE_MAX_REQUEST_SIZE) || Flags)
        Error = ERROR_INVALID_PARAMETER;
    else if (!QwFindOwnFlow(Client, FlowId))
        Error = ERROR_NOT_FOUND;
    else
        Error = ERROR_NOT_SUPPORTED; /* Notifications are for adaptive flows */
    LeaveCriticalSection(&QwLock);

    return QwFinishRequest(QOSHandle, Error, Overlapped);
}

BOOL
WINAPI
QOSEnumerateFlows(
    _In_ HANDLE QOSHandle,
    _Inout_ PULONG Size,
    _Out_writes_bytes_(*Size) PVOID Buffer)
{
    PQOS_FLOWID Ids = Buffer;
    PQWAVE_FLOW Flow;
    ULONG Needed = 0;
    DWORD Error = ERROR_SUCCESS;

    if (!Size || !Buffer)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    EnterCriticalSection(&QwLock);
    if (!QwFindClient(QOSHandle))
    {
        Error = ERROR_INVALID_HANDLE;
        goto Quit;
    }

    if (!QwIsAdministrator())
    {
        Error = ERROR_ACCESS_DENIED;
        goto Quit;
    }

    /* Every flow is listed, whichever handle owns it */
    for (Flow = QwFlows; Flow; Flow = Flow->Next)
        Needed += sizeof(*Ids);

    if (*Size < Needed)
    {
        *Size = Needed;
        Error = ERROR_INSUFFICIENT_BUFFER;
        goto Quit;
    }

    *Size = Needed;
    for (Flow = QwFlows; Flow; Flow = Flow->Next)
        *Ids++ = Flow->Id;

Quit:
    LeaveCriticalSection(&QwLock);
    return QwFinishRequest(QOSHandle, Error, NULL);
}

/* Client tracking measures the path, which needs adaptive flow support */
static
BOOL
QwTrackClient(
    _In_ HANDLE QOSHandle,
    _In_ PSOCKADDR DestAddr)
{
    DWORD Error;

    if (!DestAddr || !QwIsIpFamily(DestAddr))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    EnterCriticalSection(&QwLock);
    Error = QwFindClient(QOSHandle) ? ERROR_NOT_SUPPORTED : ERROR_INVALID_HANDLE;
    LeaveCriticalSection(&QwLock);

    return QwFinishRequest(QOSHandle, Error, NULL);
}

BOOL
WINAPI
QOSStartTrackingClient(
    _In_ HANDLE QOSHandle,
    _In_ PSOCKADDR DestAddr,
    _Reserved_ DWORD Flags)
{
    UNREFERENCED_PARAMETER(Flags);

    return QwTrackClient(QOSHandle, DestAddr);
}

BOOL
WINAPI
QOSStopTrackingClient(
    _In_ HANDLE QOSHandle,
    _In_ PSOCKADDR DestAddr,
    _Reserved_ DWORD Flags)
{
    UNREFERENCED_PARAMETER(Flags);

    return QwTrackClient(QOSHandle, DestAddr);
}

BOOL
WINAPI
DllMain(
    _In_ HINSTANCE hInstDLL,
    _In_ DWORD fdwReason,
    _In_ LPVOID lpvReserved)
{
    PQWAVE_CLIENT Client;
    PQWAVE_FLOW Flow;

    switch (fdwReason)
    {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(hInstDLL);
            InitializeCriticalSection(&QwLock);
            break;

        case DLL_PROCESS_DETACH:
            /* At process exit the heap goes away with everything in it */
            if (lpvReserved)
                break;

            while ((Flow = QwFlows))
            {
                QwFlows = Flow->Next;
                HeapFree(GetProcessHeap(), 0, Flow);
            }

            while ((Client = QwClients))
            {
                QwClients = Client->Next;
                HeapFree(GetProcessHeap(), 0, Client);
            }

            DeleteCriticalSection(&QwLock);
            break;
    }

    return TRUE;
}
