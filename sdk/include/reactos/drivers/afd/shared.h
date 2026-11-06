/*
 * COPYRIGHT:   See COPYING in the top level directory
 * PROJECT:     ReactOS Ancillary Function Driver
 * FILE:        include/reactos/drivers/afd/shared.h
 * PURPOSE:     Shared definitions for AFD.SYS and MSAFD.DLL
 */

#if !defined(__AFD_SHARED_H) || \
    (defined(EXPLICIT_32BIT) && !defined(__AFD_SHARED_H_32)) || \
    (defined(EXPLICIT_64BIT) && !defined(__AFD_SHARED_H_64))

#if defined(EXPLICIT_32BIT) && !defined(__AFD_SHARED_H_32)
#define __AFD_SHARED_H_32
#elif defined(EXPLICIT_64BIT) && !defined(__AFD_SHARED_H_64)
#define __AFD_SHARED_H_64
#elif !defined(__AFD_SHARED_H)
#define __AFD_SHARED_H
#endif

/* This is copied from sdk/include/ndk/peb_teb.h */
#define PASTE2(x,y)       x##y
#define PASTE(x,y)         PASTE2(x,y)
 
#ifdef EXPLICIT_32BIT
  #define STRUCT(x) PASTE(x,32)
  #define PTR(x) ULONG
#elif defined(EXPLICIT_64BIT)
  #define STRUCT(x) PASTE(x,64)
  #define PTR(x) ULONG64
#else
  #define STRUCT(x) x
  #define PTR(x) x
#endif
 
#if (defined(_WIN64) && !defined(EXPLICIT_32BIT)) || defined(EXPLICIT_64BIT)
  #define _STRUCT64
  #define _SELECT3264(x32, x64) (x64)
#else
  #undef _STRUCT64
  #define _SELECT3264(x32, x64) (x32)
#endif

#define AFD_MAX_EVENTS              10
#define AFD_PACKET_COMMAND_LENGTH   15
#define AfdCommand "AfdOpenPacketXX"

/* Extra definition of WSABUF for AFD so that I don't have to include any
 * userland winsock headers. */
typedef struct STRUCT(_AFD_WSABUF)
{
    UINT len;
    PTR(PCHAR) buf;
} STRUCT(AFD_WSABUF), *STRUCT(PAFD_WSABUF);

typedef struct STRUCT(_AFD_CREATE_PACKET)
{
    DWORD EndpointFlags;
    DWORD GroupID;
    DWORD SizeOfTransportName;
    WCHAR TransportName[1];
} STRUCT(AFD_CREATE_PACKET), *STRUCT(PAFD_CREATE_PACKET);

typedef struct STRUCT(_AFD_INFO)
{
    ULONG               InformationClass;
    union
    {
        ULONG           Ulong;
        LARGE_INTEGER   LargeInteger;
        BOOLEAN         Boolean;
    } Information;
    ULONG               Padding;
} STRUCT(AFD_INFO), *STRUCT(PAFD_INFO);

typedef struct STRUCT(_AFD_BIND_DATA)
{
    ULONG               ShareType;
    TRANSPORT_ADDRESS   Address;
} STRUCT(AFD_BIND_DATA), *STRUCT(PAFD_BIND_DATA);

typedef struct STRUCT(_AFD_LISTEN_DATA)
{
    BOOLEAN             UseSAN;
    ULONG               Backlog;
    BOOLEAN             UseDelayedAcceptance;
} STRUCT(AFD_LISTEN_DATA), *STRUCT(PAFD_LISTEN_DATA);

typedef struct STRUCT(_AFD_HANDLE_)
{
    SOCKET      Handle;
    ULONG       Events;
    NTSTATUS    Status;
} STRUCT(AFD_HANDLE), *STRUCT(PAFD_HANDLE);

typedef struct STRUCT(_AFD_POLL_INFO)
{
    LARGE_INTEGER   Timeout;
    ULONG           HandleCount;
    PTR(ULONG_PTR)  Exclusive;
    AFD_HANDLE      Handles[1];
} STRUCT(AFD_POLL_INFO), *STRUCT(PAFD_POLL_INFO);

typedef struct STRUCT(_AFD_ACCEPT_DATA)
{
    ULONG   UseSAN;
    ULONG   SequenceNumber;
    HANDLE  ListenHandle;
} STRUCT(AFD_ACCEPT_DATA), *STRUCT(PAFD_ACCEPT_DATA);

typedef struct STRUCT(_AFD_SUPER_ACCEPT_INFO)
{
    BOOLEAN     SanActive;
    BOOLEAN     FixAddressAlignment;
    PTR(HANDLE) AcceptHandle;
    ULONG       ReceiveDataLength;
    ULONG       LocalAddressLength;
    ULONG       RemoteAddressLength;
} STRUCT(AFD_SUPER_ACCEPT_INFO), *STRUCT(PAFD_SUPER_ACCEPT_INFO);

typedef struct STRUCT(_AFD_RECEIVED_ACCEPT_DATA)
{
    ULONG               SequenceNumber;
    TRANSPORT_ADDRESS   Address;
} STRUCT(AFD_RECEIVED_ACCEPT_DATA), *STRUCT(PAFD_RECEIVED_ACCEPT_DATA);

typedef struct STRUCT(_AFD_PENDING_ACCEPT_DATA)
{
    ULONG SequenceNumber;
    ULONG SizeOfData;
    ULONG ReturnSize;
} STRUCT(AFD_PENDING_ACCEPT_DATA), *STRUCT(PAFD_PENDING_ACCEPT_DATA);

typedef struct STRUCT(_AFD_DEFER_ACCEPT_DATA)
{
    ULONG   SequenceNumber;
    BOOLEAN RejectConnection;
} STRUCT(AFD_DEFER_ACCEPT_DATA), *STRUCT(PAFD_DEFER_ACCEPT_DATA);

typedef struct STRUCT(_AFD_RECV_INFO)
{
    PTR(PAFD_WSABUF) BufferArray;
    ULONG            BufferCount;
    ULONG            AfdFlags;
    ULONG            TdiFlags;
} STRUCT(AFD_RECV_INFO), *STRUCT(PAFD_RECV_INFO);

typedef struct STRUCT(_AFD_RECV_INFO_UDP)
{
    PTR(PAFD_WSABUF) BufferArray;
    ULONG            BufferCount;
    ULONG            AfdFlags;
    ULONG            TdiFlags;
    PTR(PVOID)       Address;
    PTR(PINT)        AddressLength;
} STRUCT(AFD_RECV_INFO_UDP), *STRUCT(PAFD_RECV_INFO_UDP);

typedef struct STRUCT(_AFD_SEND_INFO)
{
    PTR(PAFD_WSABUF) BufferArray;
    ULONG            BufferCount;
    ULONG            AfdFlags;
    ULONG            TdiFlags;
} STRUCT(AFD_SEND_INFO), *STRUCT(PAFD_SEND_INFO);

#ifdef EXPLICIT_32BIT

typedef struct _TDI_REQUEST32
{
    union
    {
        ULONG AddressHandle;
        ULONG ConnectionContext;
        ULONG ControlChannel;
    } Handle;
    ULONG RequestNotifyObject;
    ULONG RequestContext;
    TDI_STATUS TdiStatus;
} TDI_REQUEST32, *PTDI_REQUEST32;

typedef struct _TDI_REQUEST_SEND_DATAGRAM32
{
    TDI_REQUEST32 Request;
    ULONG SendDatagramInformation;
} TDI_REQUEST_SEND_DATAGRAM32, *PTDI_REQUEST_SEND_DATAGRAM32;

typedef struct _TDI_CONNECTION_INFORMATION32
{
    LONG UserDataLength;
    ULONG UserData;
    LONG OptionsLength;
    ULONG Options;
    LONG RemoteAddressLength;
    ULONG RemoteAddress;
} TDI_CONNECTION_INFORMATION32, *PTDI_CONNECTION_INFORMATION32;

#endif

typedef struct STRUCT(_AFD_SEND_INFO_UDP)
{
    PTR(PAFD_WSABUF)                    BufferArray;
    ULONG                               BufferCount;
    ULONG                               AfdFlags;
    STRUCT(TDI_REQUEST_SEND_DATAGRAM)   TdiRequest;
    STRUCT(TDI_CONNECTION_INFORMATION)  TdiConnection;
} STRUCT(AFD_SEND_INFO_UDP), *STRUCT(PAFD_SEND_INFO_UDP);

C_ASSERT(sizeof(AFD_RECV_INFO) == sizeof(AFD_SEND_INFO));

typedef struct STRUCT(_AFD_SUPER_CONNECT_INFO)
{
    BOOLEAN SanActive;
    TRANSPORT_ADDRESS RemoteAddress;
} STRUCT(AFD_SUPER_CONNECT_INFO), *STRUCT(PAFD_SUPER_CONNECT_INFO);

typedef struct STRUCT(_AFD_CONNECT_INFO)
{
    BOOLEAN             UseSAN;
    ULONG               Root;
    ULONG               Unknown;
    TRANSPORT_ADDRESS   RemoteAddress;
} STRUCT(AFD_CONNECT_INFO), *STRUCT(PAFD_CONNECT_INFO);

typedef struct STRUCT(_AFD_EVENT_SELECT_INFO)
{
    PTR(HANDLE) EventObject;
    ULONG       Events;
} STRUCT(AFD_EVENT_SELECT_INFO), *STRUCT(PAFD_EVENT_SELECT_INFO);

typedef struct STRUCT(_AFD_ENUM_NETWORK_EVENTS_INFO)
{
    PTR(HANDLE) Event;
    ULONG       PollEvents;
    NTSTATUS    EventStatus[AFD_MAX_EVENTS];
} STRUCT(AFD_ENUM_NETWORK_EVENTS_INFO), *STRUCT(PAFD_ENUM_NETWORK_EVENTS_INFO);

typedef struct STRUCT(_AFD_DISCONNECT_INFO)
{
    ULONG           DisconnectType;
    LARGE_INTEGER   Timeout;
} STRUCT(AFD_DISCONNECT_INFO), *STRUCT(PAFD_DISCONNECT_INFO);

typedef struct STRUCT(_AFD_VALIDATE_GROUP_DATA)
{
    LONG                GroupId;
    TRANSPORT_ADDRESS   Address;
} STRUCT(AFD_VALIDATE_GROUP_DATA), *STRUCT(PAFD_VALIDATE_GROUP_DATA);

typedef struct STRUCT(_AFD_TDI_HANDLE_DATA)
{
    PTR(HANDLE) TdiAddressHandle;
    PTR(HANDLE) TdiConnectionHandle;
} STRUCT(AFD_TDI_HANDLE_DATA), *STRUCT(PAFD_TDI_HANDLE_DATA);

/* AFD Packet Endpoint Flags */
#define AFD_ENDPOINT_CONNECTIONLESS     0x1
#define AFD_ENDPOINT_MESSAGE_ORIENTED   0x10
#define AFD_ENDPOINT_RAW                0x100
#define AFD_ENDPOINT_MULTIPOINT         0x1000
#define AFD_ENDPOINT_C_ROOT             0x10000
#define AFD_ENDPOINT_D_ROOT             0x100000

/* AFD TDI Query Flags */
#define AFD_ADDRESS_HANDLE      0x1L
#define AFD_CONNECTION_HANDLE   0x2L

/* AFD event bits */
#define AFD_EVENT_RECEIVE_BIT                   0
#define AFD_EVENT_OOB_RECEIVE_BIT               1
#define AFD_EVENT_SEND_BIT                      2
#define AFD_EVENT_DISCONNECT_BIT                3
#define AFD_EVENT_ABORT_BIT                     4
#define AFD_EVENT_CLOSE_BIT                     5
#define AFD_EVENT_CONNECT_BIT                   6
#define AFD_EVENT_ACCEPT_BIT                    7
#define AFD_EVENT_CONNECT_FAIL_BIT              8
#define AFD_EVENT_QOS_BIT                       9
#define AFD_EVENT_GROUP_QOS_BIT                 10
#define AFD_EVENT_ROUTING_INTERFACE_CHANGE_BIT  11
#define AFD_EVENT_ADDRESS_LIST_CHANGE_BIT       12
#define AFD_MAX_EVENT                           13
#define AFD_ALL_EVENTS                          ((1 << AFD_MAX_EVENT) - 1)

/* AFD Info Flags */
#define AFD_INFO_INLINING_MODE          0x01L
#define AFD_INFO_BLOCKING_MODE          0x02L
#define AFD_INFO_SENDS_IN_PROGRESS      0x04L
#define AFD_INFO_RECEIVE_WINDOW_SIZE    0x06L
#define AFD_INFO_SEND_WINDOW_SIZE       0x07L
#define AFD_INFO_GROUP_ID_TYPE          0x10L
#define AFD_INFO_RECEIVE_CONTENT_SIZE   0x11L

/* AFD Share Flags */
#define AFD_SHARE_UNIQUE    0x0L
#define AFD_SHARE_REUSE     0x1L
#define AFD_SHARE_WILDCARD  0x2L
#define AFD_SHARE_EXCLUSIVE 0x3L

/* AFD Disconnect Flags */
#define AFD_DISCONNECT_SEND     0x01L
#define AFD_DISCONNECT_RECV     0x02L
#define AFD_DISCONNECT_ABORT    0x04L
#define AFD_DISCONNECT_DATAGRAM 0x08L

/* AFD Event Flags */
#define AFD_EVENT_RECEIVE                   (1 << AFD_EVENT_RECEIVE_BIT)
#define AFD_EVENT_OOB_RECEIVE               (1 << AFD_EVENT_OOB_RECEIVE_BIT)
#define AFD_EVENT_SEND                      (1 << AFD_EVENT_SEND_BIT)
#define AFD_EVENT_DISCONNECT                (1 << AFD_EVENT_DISCONNECT_BIT)
#define AFD_EVENT_ABORT                     (1 << AFD_EVENT_ABORT_BIT)
#define AFD_EVENT_CLOSE                     (1 << AFD_EVENT_CLOSE_BIT)
#define AFD_EVENT_CONNECT                   (1 << AFD_EVENT_CONNECT_BIT)
#define AFD_EVENT_ACCEPT                    (1 << AFD_EVENT_ACCEPT_BIT)
#define AFD_EVENT_CONNECT_FAIL              (1 << AFD_EVENT_CONNECT_FAIL_BIT)
#define AFD_EVENT_QOS                       (1 << AFD_EVENT_QOS_BIT)
#define AFD_EVENT_GROUP_QOS                 (1 << AFD_EVENT_GROUP_QOS_BIT)
#define AFD_EVENT_ROUTING_INTERFACE_CHANGE  (1 << AFD_EVENT_ROUTING_INTERFACE_CHANGE_BIT)
#define AFD_EVENT_ADDRESS_LIST_CHANGE       (1 << AFD_EVENT_ADDRESS_LIST_CHANGE_BIT)

/* AFD SEND/RECV Flags */
#define AFD_SKIP_FIO    0x1L
#define AFD_OVERLAPPED  0x2L
#define AFD_IMMEDIATE   0x4L

/* IOCTL Generation */
#define FSCTL_AFD_BASE FILE_DEVICE_NETWORK
#define _AFD_CONTROL_CODE(Operation, Method) ((FSCTL_AFD_BASE) << 12 | (Operation << 2) | Method)

/* AFD Commands */
#define AFD_BIND                        0
#define AFD_CONNECT                     1
#define AFD_START_LISTEN                2
#define AFD_WAIT_FOR_LISTEN             3
#define AFD_ACCEPT                      4
#define AFD_RECV                        5
#define AFD_RECV_DATAGRAM               6
#define AFD_SEND                        7
#define AFD_SEND_DATAGRAM               8
#define AFD_SELECT                      9
#define AFD_DISCONNECT                  10
#define AFD_GET_SOCK_NAME               11
#define AFD_GET_PEER_NAME               12
#define AFD_GET_TDI_HANDLES             13
#define AFD_SET_INFO                    14
#define AFD_GET_CONTEXT_SIZE            15
#define AFD_GET_CONTEXT                 16
#define AFD_SET_CONTEXT                 17
#define AFD_SET_CONNECT_DATA            18
#define AFD_SET_CONNECT_OPTIONS         19
#define AFD_SET_DISCONNECT_DATA         20
#define AFD_SET_DISCONNECT_OPTIONS      21
#define AFD_GET_CONNECT_DATA            22
#define AFD_GET_CONNECT_OPTIONS         23
#define AFD_GET_DISCONNECT_DATA         24
#define AFD_GET_DISCONNECT_OPTIONS      25
#define AFD_SET_CONNECT_DATA_SIZE       26
#define AFD_SET_CONNECT_OPTIONS_SIZE    27
#define AFD_SET_DISCONNECT_DATA_SIZE    28
#define AFD_SET_DISCONNECT_OPTIONS_SIZE 29
#define AFD_GET_INFO                    30
#define AFD_SUPER_ACCEPT                32
#define AFD_EVENT_SELECT                33
#define AFD_ENUM_NETWORK_EVENTS         34
#define AFD_DEFER_ACCEPT                35
#define AFD_GET_PENDING_CONNECT_DATA    41
#define AFD_VALIDATE_GROUP              42
#define AFD_SUPER_CONNECT               49

/* AFD IOCTLs */

#define IOCTL_AFD_BIND \
  _AFD_CONTROL_CODE(AFD_BIND, METHOD_NEITHER)
#define IOCTL_AFD_CONNECT \
  _AFD_CONTROL_CODE(AFD_CONNECT, METHOD_NEITHER)
#define IOCTL_AFD_START_LISTEN \
  _AFD_CONTROL_CODE(AFD_START_LISTEN, METHOD_NEITHER)
#define IOCTL_AFD_WAIT_FOR_LISTEN \
  _AFD_CONTROL_CODE(AFD_WAIT_FOR_LISTEN, METHOD_BUFFERED )
#define IOCTL_AFD_ACCEPT \
  _AFD_CONTROL_CODE(AFD_ACCEPT, METHOD_BUFFERED )
#define IOCTL_AFD_RECV \
  _AFD_CONTROL_CODE(AFD_RECV, METHOD_NEITHER)
#define IOCTL_AFD_RECV_DATAGRAM \
  _AFD_CONTROL_CODE(AFD_RECV_DATAGRAM, METHOD_NEITHER)
#define IOCTL_AFD_SEND \
  _AFD_CONTROL_CODE(AFD_SEND, METHOD_NEITHER)
#define IOCTL_AFD_SEND_DATAGRAM \
  _AFD_CONTROL_CODE(AFD_SEND_DATAGRAM, METHOD_NEITHER)
#define IOCTL_AFD_SELECT \
  _AFD_CONTROL_CODE(AFD_SELECT, METHOD_BUFFERED )
#define IOCTL_AFD_DISCONNECT \
  _AFD_CONTROL_CODE(AFD_DISCONNECT, METHOD_NEITHER)
#define IOCTL_AFD_GET_SOCK_NAME \
  _AFD_CONTROL_CODE(AFD_GET_SOCK_NAME, METHOD_NEITHER)
#define IOCTL_AFD_GET_PEER_NAME \
  _AFD_CONTROL_CODE(AFD_GET_PEER_NAME, METHOD_NEITHER)
#define IOCTL_AFD_GET_TDI_HANDLES \
  _AFD_CONTROL_CODE(AFD_GET_TDI_HANDLES, METHOD_NEITHER)
#define IOCTL_AFD_SET_INFO \
  _AFD_CONTROL_CODE(AFD_SET_INFO, METHOD_NEITHER)
#define IOCTL_AFD_GET_CONTEXT_SIZE \
  _AFD_CONTROL_CODE(AFD_GET_CONTEXT_SIZE, METHOD_NEITHER)
#define IOCTL_AFD_GET_CONTEXT \
  _AFD_CONTROL_CODE(AFD_GET_CONTEXT, METHOD_NEITHER)
#define IOCTL_AFD_SET_CONTEXT \
  _AFD_CONTROL_CODE(AFD_SET_CONTEXT, METHOD_NEITHER)
#define IOCTL_AFD_SET_CONNECT_DATA \
  _AFD_CONTROL_CODE(AFD_SET_CONNECT_DATA, METHOD_NEITHER)
#define IOCTL_AFD_SET_CONNECT_OPTIONS \
  _AFD_CONTROL_CODE(AFD_SET_CONNECT_OPTIONS, METHOD_NEITHER)
#define IOCTL_AFD_SET_DISCONNECT_DATA \
  _AFD_CONTROL_CODE(AFD_SET_DISCONNECT_DATA, METHOD_NEITHER)
#define IOCTL_AFD_SET_DISCONNECT_OPTIONS \
  _AFD_CONTROL_CODE(AFD_SET_DISCONNECT_OPTIONS, METHOD_NEITHER)
#define IOCTL_AFD_GET_CONNECT_DATA \
  _AFD_CONTROL_CODE(AFD_GET_CONNECT_DATA, METHOD_NEITHER)
#define IOCTL_AFD_GET_CONNECT_OPTIONS \
  _AFD_CONTROL_CODE(AFD_GET_CONNECT_OPTIONS, METHOD_NEITHER)
#define IOCTL_AFD_GET_DISCONNECT_DATA \
  _AFD_CONTROL_CODE(AFD_GET_DISCONNECT_DATA, METHOD_NEITHER)
#define IOCTL_AFD_GET_DISCONNECT_OPTIONS \
  _AFD_CONTROL_CODE(AFD_GET_DISCONNECT_OPTIONS, METHOD_NEITHER)
#define IOCTL_AFD_SET_CONNECT_DATA_SIZE \
  _AFD_CONTROL_CODE(AFD_SET_CONNECT_DATA_SIZE, METHOD_NEITHER)
#define IOCTL_AFD_SET_CONNECT_OPTIONS_SIZE \
  _AFD_CONTROL_CODE(AFD_SET_CONNECT_OPTIONS_SIZE, METHOD_NEITHER)
#define IOCTL_AFD_SET_DISCONNECT_DATA_SIZE \
  _AFD_CONTROL_CODE(AFD_SET_DISCONNECT_DATA_SIZE, METHOD_NEITHER)
#define IOCTL_AFD_SET_DISCONNECT_OPTIONS_SIZE \
  _AFD_CONTROL_CODE(AFD_SET_DISCONNECT_OPTIONS_SIZE, METHOD_NEITHER)
#define IOCTL_AFD_GET_INFO \
  _AFD_CONTROL_CODE(AFD_GET_INFO, METHOD_NEITHER)
#define IOCTL_AFD_SUPER_ACCEPT \
  _AFD_CONTROL_CODE(AFD_SUPER_ACCEPT, METHOD_NEITHER)
#define IOCTL_AFD_EVENT_SELECT \
  _AFD_CONTROL_CODE(AFD_EVENT_SELECT, METHOD_NEITHER)
#define IOCTL_AFD_DEFER_ACCEPT \
  _AFD_CONTROL_CODE(AFD_DEFER_ACCEPT, METHOD_NEITHER)
#define IOCTL_AFD_GET_PENDING_CONNECT_DATA \
  _AFD_CONTROL_CODE(AFD_GET_PENDING_CONNECT_DATA, METHOD_NEITHER)
#define IOCTL_AFD_ENUM_NETWORK_EVENTS \
  _AFD_CONTROL_CODE(AFD_ENUM_NETWORK_EVENTS, METHOD_NEITHER)
#define IOCTL_AFD_VALIDATE_GROUP \
  _AFD_CONTROL_CODE(AFD_VALIDATE_GROUP, METHOD_NEITHER)
#define IOCTL_AFD_SUPER_CONNECT \
  _AFD_CONTROL_CODE(AFD_SUPER_CONNECT, METHOD_NEITHER)

typedef struct STRUCT(_AFD_SOCKET_INFORMATION)
{
    BOOL CommandChannel;
    INT AddressFamily;
    INT SocketType;
    INT Protocol;
    PTR(PVOID) HelperContext;
    DWORD NotificationEvents;
    STRUCT(UNICODE_STRING) TdiDeviceName;
    SOCKADDR Name;
} STRUCT(AFD_SOCKET_INFORMATION), *STRUCT(PAFD_SOCKET_INFORMATION);

#if !defined(EXPLICIT_32BIT) && !defined(EXPLICIT_64BIT)
typedef enum _SOCKET_STATE 
{
    SocketOpen,
    SocketBound,
    SocketBoundUdp,
    SocketConnected,
    SocketClosed
} SOCKET_STATE, *PSOCKET_STATE;
#endif

typedef struct STRUCT(_SOCK_SHARED_INFO) 
{
    SOCKET_STATE    State;
    LONG						RefCount;
    INT							AddressFamily;
    INT							SocketType;
    INT							Protocol;
    INT							SizeOfLocalAddress;
    INT							SizeOfRemoteAddress;
    struct linger	  LingerData;
    ULONG						SendTimeout;
    ULONG						RecvTimeout;
    ULONG						SizeOfRecvBuffer;
    ULONG						SizeOfSendBuffer;
    ULONG						ConnectTime;
    struct 
    {
        BOOLEAN					Listening:1;
        BOOLEAN					Broadcast:1;
        BOOLEAN					Debug:1;
        BOOLEAN					OobInline:1;
        BOOLEAN					ReuseAddresses:1;
        BOOLEAN					ExclusiveAddressUse:1;
        BOOLEAN					NonBlocking:1;
        BOOLEAN					DontUseWildcard:1;
        BOOLEAN					ReceiveShutdown:1;
        BOOLEAN					SendShutdown:1;
        BOOLEAN					UseDelayedAcceptance:1;
		    BOOLEAN					UseSAN:1;
    }; 
    // Flags
    DWORD						CreateFlags;
    DWORD						ServiceFlags1;
    DWORD						ProviderFlags;
    GROUP						GroupID;
    DWORD						GroupType;
    INT							GroupPriority;
    INT							SocketLastError;
    PTR(HWND)				hWnd;
    LONG						Unknown;
    DWORD						SequenceNumber;
    UINT						wMsg;
    LONG						AsyncEvents;
    LONG						AsyncDisabledEvents;
    SOCKADDR				WSLocalAddress;
    SOCKADDR				WSRemoteAddress;
} STRUCT(SOCK_SHARED_INFO), *STRUCT(PSOCK_SHARED_INFO);

typedef struct STRUCT(_FILE_REQUEST_BIND)
{
    SOCKADDR Name;
} STRUCT(FILE_REQUEST_BIND), *STRUCT(PFILE_REQUEST_BIND);

typedef struct STRUCT(_FILE_REPLY_BIND)
{
    INT Status;
    PTR(HANDLE) TdiAddressObjectHandle;
    PTR(HANDLE) TdiConnectionObjectHandle;
} STRUCT(FILE_REPLY_BIND), *STRUCT(PFILE_REPLY_BIND);

typedef struct STRUCT(_FILE_REQUEST_LISTEN)
{
    INT Backlog;
} STRUCT(FILE_REQUEST_LISTEN), *STRUCT(PFILE_REQUEST_LISTEN);

typedef struct STRUCT(_FILE_REPLY_LISTEN)
{
    INT Status;
} STRUCT(FILE_REPLY_LISTEN), *STRUCT(PFILE_REPLY_LISTEN);

typedef struct STRUCT(_FILE_REQUEST_SENDTO)
{
    PTR(LPWSABUF) Buffers;
    DWORD BufferCount;
    DWORD Flags;
    SOCKADDR To;
    INT ToLen;
} STRUCT(FILE_REQUEST_SENDTO), *STRUCT(PFILE_REQUEST_SENDTO);

typedef struct STRUCT(_FILE_REPLY_SENDTO)
{
    INT Status;
    DWORD NumberOfBytesSent;
} STRUCT(FILE_REPLY_SENDTO), *STRUCT(PFILE_REPLY_SENDTO);

typedef struct STRUCT(_FILE_REQUEST_RECVFROM)
{
    PTR(LPWSABUF)   Buffers;
    DWORD           BufferCount;
    PTR(LPDWORD)    Flags;
    PTR(LPSOCKADDR) From;
    PTR(LPINT)      FromLen;
} STRUCT(FILE_REQUEST_RECVFROM), *STRUCT(PFILE_REQUEST_RECVFROM);

typedef struct STRUCT(_FILE_REPLY_RECVFROM)
{
    INT Status;
    DWORD NumberOfBytesRecvd;
} STRUCT(FILE_REPLY_RECVFROM), *STRUCT(PFILE_REPLY_RECVFROM);

typedef struct STRUCT(_FILE_REQUEST_RECV)
{
    PTR(LPWSABUF)   Buffers;
    DWORD           BufferCount;
    PTR(LPDWORD)    Flags;
} STRUCT(FILE_REQUEST_RECV), *STRUCT(PFILE_REQUEST_RECV);

typedef struct STRUCT(_FILE_REPLY_RECV)
{
    INT Status;
    DWORD NumberOfBytesRecvd;
} STRUCT(FILE_REPLY_RECV), *STRUCT(PFILE_REPLY_RECV);

typedef struct STRUCT(_FILE_REQUEST_SEND)
{
    PTR(LPWSABUF)   Buffers;
    DWORD           BufferCount;
    DWORD           Flags;
} STRUCT(FILE_REQUEST_SEND), *STRUCT(PFILE_REQUEST_SEND);

typedef struct STRUCT(_FILE_REPLY_SEND)
{
    INT Status;
    DWORD NumberOfBytesSent;
} STRUCT(FILE_REPLY_SEND), *STRUCT(PFILE_REPLY_SEND);

typedef struct STRUCT(_FILE_REQUEST_ACCEPT)
{
    PTR(LPSOCKADDR) addr;
    INT addrlen;
    PTR(LPCONDITIONPROC) lpfnCondition;
    DWORD dwCallbackData;
} STRUCT(FILE_REQUEST_ACCEPT), *STRUCT(PFILE_REQUEST_ACCEPT);

typedef struct STRUCT(_FILE_REPLY_ACCEPT)
{
    INT Status;
    INT addrlen;
    SOCKET Socket;
} STRUCT(FILE_REPLY_ACCEPT), *STRUCT(PFILE_REPLY_ACCEPT);

typedef struct STRUCT(_FILE_REQUEST_CONNECT)
{
    PTR(LPSOCKADDR) name;
    INT namelen;
    PTR(LPWSABUF) lpCallerData;
    PTR(LPWSABUF) lpCalleeData;
    PTR(LPQOS) lpSQOS;
    PTR(LPQOS) lpGQOS;
} STRUCT(FILE_REQUEST_CONNECT), *STRUCT(PFILE_REQUEST_CONNECT);

typedef struct STRUCT(_FILE_REPLY_CONNECT)
{
    INT Status;
} STRUCT(FILE_REPLY_CONNECT), *STRUCT(PFILE_REPLY_CONNECT);

#undef STRUCT
#undef PTR
#undef _SELECT3264

#endif /*__AFD_SHARED_H */

/* EOF */
