/*
 * PROJECT:     ReactOS Qualcomm SoC support
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Shared memory, SMD, SMP2P and SMSM client interfaces
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

/* Layouts match the platform driver these interfaces replace, so either side can be swapped independently */

/* {54415E20-5FC4-4938-8958-31966F441C63} */
DEFINE_GUID(GUID_QCOM_SMEM_INTERFACE,
            0x54415e20, 0x5fc4, 0x4938, 0x89, 0x58, 0x31, 0x96, 0x6f, 0x44, 0x1c, 0x63);

/* {F9D15453-8335-434C-AA72-FCD925F135F3} */
DEFINE_GUID(GUID_DEVINTERFACE_QCOM_SMD,
            0xf9d15453, 0x8335, 0x434c, 0xaa, 0x72, 0xfc, 0xd9, 0x25, 0xf1, 0x35, 0xf3);

/* {3B1E7A9C-4D2F-4E0A-9C6B-1F8D2A5E7C40} */
DEFINE_GUID(GUID_DEVINTERFACE_QCOM_IPC,
            0x3b1e7a9c, 0x4d2f, 0x4e0a, 0x9c, 0x6b, 0x1f, 0x8d, 0x2a, 0x5e, 0x7c, 0x40);

/* {6C2A9E14-7B3D-4F58-8E1A-2D9C5F3B0A71} */
DEFINE_GUID(GUID_QCOM_IPC_INTERFACE,
            0x6c2a9e14, 0x7b3d, 0x4f58, 0x8e, 0x1a, 0x2d, 0x9c, 0x5f, 0x3b, 0x0a, 0x71);

/* {07631EE5-21BD-455B-BD1F-F41B43F3A9BC} */
DEFINE_GUID(GUID_QCOM_SMDL_INTERFACE,
            0x07631ee5, 0x21bd, 0x455b, 0xbd, 0x1f, 0xf4, 0x1b, 0x43, 0xf3, 0xa9, 0xbc);

/* {42398493-37B3-412A-B8D3-098A9ED09C84} */
DEFINE_GUID(GUID_QCOM_SMP2P_INTERFACE,
            0x42398493, 0x37b3, 0x412a, 0xb8, 0xd3, 0x09, 0x8a, 0x9e, 0xd0, 0x9c, 0x84);

/* {29272484-BAEC-41BF-BA29-646D4390B7CC} */
DEFINE_GUID(GUID_QCOM_SMSM_INTERFACE,
            0x29272484, 0xbaec, 0x41bf, 0xba, 0x29, 0x64, 0x6d, 0x43, 0x90, 0xb7, 0xcc);

/* Processors sharing SMEM */
#define QCOM_HOST_APPS              0
#define QCOM_HOST_MODEM             1
#define QCOM_HOST_ADSP              2
#define QCOM_HOST_DSPS              3
#define QCOM_HOST_WCNSS             4
#define QCOM_HOST_MODEM_FW          5
#define QCOM_HOST_RPM               6
#define QCOM_HOST_COUNT             7
#define QCOM_HOST_GLOBAL            8

/* SMD edges seen from the apps processor */
#define QCOM_SMD_EDGE_APPS_MODEM    0
#define QCOM_SMD_EDGE_APPS_ADSP     1
#define QCOM_SMD_EDGE_APPS_DSPS     3
#define QCOM_SMD_EDGE_APPS_WCNSS    6
#define QCOM_SMD_EDGE_APPS_MODEM_FW 10
#define QCOM_SMD_EDGE_APPS_RPM      15
#define QCOM_SMD_EDGE_COUNT         21

/* Open flags */
#define QCOM_SMDL_OPEN_STREAM       0x00000004

/* Write flags */
#define QCOM_SMDL_WRITE_NO_SIGNAL   0x00000001

/* Vectored I/O flags */
#define QCOM_SMDL_IOV_PARTIAL       0x00000002

/* Notification events, delivered at DISPATCH_LEVEL */
#define QCOM_SMDL_EVENT_OPEN            1
#define QCOM_SMDL_EVENT_READ            2
#define QCOM_SMDL_EVENT_WRITE           3
#define QCOM_SMDL_EVENT_CLOSE_COMPLETE  4
#define QCOM_SMDL_EVENT_DSR             5
#define QCOM_SMDL_EVENT_CTS             6
#define QCOM_SMDL_EVENT_CD              7
#define QCOM_SMDL_EVENT_RI              8
#define QCOM_SMDL_EVENT_REMOTE_CLOSE    9
#define QCOM_SMDL_EVENT_READ_PARTIAL    10

/* Modem signals for SigSet and SigGet */
#define QCOM_SMDL_SIG_DSR           0
#define QCOM_SMDL_SIG_CTS           1
#define QCOM_SMDL_SIG_CD            2
#define QCOM_SMDL_SIG_RI            3

typedef struct _QCOM_SMDL_IOVEC
{
    struct _QCOM_SMDL_IOVEC *Next;
    ULONG Length;
    PVOID Buffer;
} QCOM_SMDL_IOVEC, *PQCOM_SMDL_IOVEC;

typedef PVOID QCOM_SMDL_HANDLE;

typedef VOID
(NTAPI *PQCOM_SMDL_NOTIFY)(
    _In_ QCOM_SMDL_HANDLE Port,
    _In_ ULONG Event,
    _In_opt_ PVOID Context);

typedef QCOM_SMDL_HANDLE (NTAPI *PQCOM_SMDL_OPEN)(PCSTR Name, ULONG Edge, ULONG Flags, ULONG FifoSize, PQCOM_SMDL_NOTIFY Notify, PVOID Context);
typedef LONG (NTAPI *PQCOM_SMDL_CLOSE)(QCOM_SMDL_HANDLE Port);
typedef LONG (NTAPI *PQCOM_SMDL_READ)(QCOM_SMDL_HANDLE Port, ULONG Length, PVOID Buffer);
typedef LONG (NTAPI *PQCOM_SMDL_READV)(QCOM_SMDL_HANDLE Port, PQCOM_SMDL_IOVEC Vector, ULONG Flags);
typedef LONG (NTAPI *PQCOM_SMDL_WRITE)(QCOM_SMDL_HANDLE Port, ULONG Length, const VOID *Buffer, ULONG Flags);
typedef LONG (NTAPI *PQCOM_SMDL_WRITEV)(QCOM_SMDL_HANDLE Port, PQCOM_SMDL_IOVEC Vector, ULONG Flags);
typedef LONG (NTAPI *PQCOM_SMDL_PEEK)(QCOM_SMDL_HANDLE Port);
typedef LONG (NTAPI *PQCOM_SMDL_SIGSET)(QCOM_SMDL_HANDLE Port, ULONG Signal, ULONG Value);
typedef LONG (NTAPI *PQCOM_SMDL_SIGGET)(QCOM_SMDL_HANDLE Port, ULONG Signal);

typedef struct _QCOM_SMDL_INTERFACE
{
    INTERFACE Header;
    PQCOM_SMDL_OPEN Open;
    PQCOM_SMDL_CLOSE Close;
    PQCOM_SMDL_READ Read;
    PQCOM_SMDL_READV ReadV;
    PQCOM_SMDL_WRITE Write;
    PQCOM_SMDL_WRITEV WriteV;
    PQCOM_SMDL_PEEK TxPeek;
    PQCOM_SMDL_PEEK RxPeek;
    PQCOM_SMDL_SIGSET SigSet;
    PQCOM_SMDL_SIGGET SigGet;
} QCOM_SMDL_INTERFACE, *PQCOM_SMDL_INTERFACE;

typedef PVOID (NTAPI *PQCOM_SMEM_ALLOC)(ULONG Item, ULONG Size);
typedef ULONG (NTAPI *PQCOM_SMEM_VERSION_SET)(ULONG Item, ULONG Version, ULONG Mask);
typedef PVOID (NTAPI *PQCOM_SMEM_GET_ADDR)(ULONG Item, PULONG Size);
typedef PVOID (NTAPI *PQCOM_SMEM_GET_IPC_REG)(ULONGLONG PhysicalAddress);

typedef struct _QCOM_SMEM_INTERFACE
{
    INTERFACE Header;
    PQCOM_SMEM_ALLOC Alloc;
    PQCOM_SMEM_VERSION_SET VersionSet;
    PQCOM_SMEM_GET_ADDR GetAddr;
    PQCOM_SMEM_GET_IPC_REG GetIpcRegister;
} QCOM_SMEM_INTERFACE, *PQCOM_SMEM_INTERFACE;

typedef struct _QCOM_SMP2P_ENTRY *PQCOM_SMP2P_ENTRY;

typedef LONG (NTAPI *PQCOM_SMP2P_CREATE)(PQCOM_SMP2P_ENTRY *Entry, ULONG RemoteHost, PCSTR Name);
typedef LONG (NTAPI *PQCOM_SMP2P_WRITE)(PQCOM_SMP2P_ENTRY Entry, ULONG Value);
typedef LONG (NTAPI *PQCOM_SMP2P_MODIFY)(PQCOM_SMP2P_ENTRY Entry, ULONG ClearMask, ULONG SetMask);
typedef LONG (NTAPI *PQCOM_SMP2P_REGISTER)(PQCOM_SMP2P_ENTRY *Entry, ULONG RemoteHost, PCSTR Name);
typedef LONG (NTAPI *PQCOM_SMP2P_READ)(PQCOM_SMP2P_ENTRY Entry, PULONG Value);
typedef LONG (NTAPI *PQCOM_SMP2P_WAIT)(PQCOM_SMP2P_ENTRY Entry);
typedef LONG (NTAPI *PQCOM_SMP2P_WAIT_MULTIPLE)(PQCOM_SMP2P_ENTRY *Entries, ULONG Count, PULONG Index);
typedef PKEVENT (NTAPI *PQCOM_SMP2P_GET_EVENT)(PQCOM_SMP2P_ENTRY Entry);

typedef struct _QCOM_SMP2P_INTERFACE
{
    INTERFACE Header;
    PQCOM_SMP2P_CREATE Create;
    PQCOM_SMP2P_WRITE Write;
    PQCOM_SMP2P_MODIFY Modify;
    PQCOM_SMP2P_REGISTER Register;
    PQCOM_SMP2P_READ Read;
    PQCOM_SMP2P_WAIT Wait;
    PQCOM_SMP2P_WAIT_MULTIPLE WaitMultiple;
    PQCOM_SMP2P_GET_EVENT GetEvent;
} QCOM_SMP2P_INTERFACE, *PQCOM_SMP2P_INTERFACE;

typedef VOID
(NTAPI *PQCOM_SMSM_CALLBACK)(
    _In_ ULONG Host,
    _In_ ULONG OldState,
    _In_ ULONG NewState,
    _In_opt_ PVOID Context);

typedef LONG (NTAPI *PQCOM_SMSM_SET)(ULONG Host, ULONG Bits);
typedef LONG (NTAPI *PQCOM_SMSM_CLEAR_SET)(ULONG Host, ULONG ClearBits, ULONG SetBits);
typedef ULONG (NTAPI *PQCOM_SMSM_GET)(ULONG Host);
typedef LONG (NTAPI *PQCOM_SMSM_RESET_ACK)(ULONG Mask);
typedef LONG (NTAPI *PQCOM_SMSM_REGISTER)(ULONG Host, ULONG Mask, PQCOM_SMSM_CALLBACK Callback, PVOID Context);
typedef LONG (NTAPI *PQCOM_SMSM_DEREGISTER)(ULONG Host, ULONG Mask, PQCOM_SMSM_CALLBACK Callback);

typedef struct _QCOM_SMSM_INTERFACE
{
    INTERFACE Header;
    PQCOM_SMSM_SET Set;
    PQCOM_SMSM_SET Clear;
    PQCOM_SMSM_CLEAR_SET ClearSet;
    PQCOM_SMSM_GET Get;
    PQCOM_SMSM_RESET_ACK ResetAck;
    PQCOM_SMSM_REGISTER Register;
    PQCOM_SMSM_DEREGISTER Deregister;
} QCOM_SMSM_INTERFACE, *PQCOM_SMSM_INTERFACE;

/*
 * Kernel qsocket interface, a small BSD-like datagram API over the IPC router.
 * A socket is a local endpoint, addresses are {node, port}, names are
 * {service, instance}. Calls return 0 on success or a negative error.
 */
typedef struct _QCOM_IPC_ADDR
{
    ULONG Node;
    ULONG Port;
} QCOM_IPC_ADDR, *PQCOM_IPC_ADDR;

typedef PVOID QCOM_IPC_SOCKET;

#define QCOM_IPC_TIMEOUT_INFINITE   (-1)

typedef LONG (NTAPI *PQCOM_IPC_OPEN)(QCOM_IPC_SOCKET *Socket);
typedef LONG (NTAPI *PQCOM_IPC_CLOSE)(QCOM_IPC_SOCKET Socket);
typedef LONG (NTAPI *PQCOM_IPC_GETADDR)(QCOM_IPC_SOCKET Socket, PQCOM_IPC_ADDR Addr);
typedef LONG (NTAPI *PQCOM_IPC_BIND)(QCOM_IPC_SOCKET Socket, ULONG Service, ULONG Instance);
typedef LONG (NTAPI *PQCOM_IPC_LOOKUP)(ULONG Service, ULONG Instance, PQCOM_IPC_ADDR Addr);
typedef LONG (NTAPI *PQCOM_IPC_SENDTO)(QCOM_IPC_SOCKET Socket, const QCOM_IPC_ADDR *Dst, const VOID *Buffer, ULONG Length);
typedef LONG (NTAPI *PQCOM_IPC_RECVFROM)(QCOM_IPC_SOCKET Socket, PQCOM_IPC_ADDR Src, PVOID Buffer, ULONG Length, LONG TimeoutMs, PULONG Received);

typedef struct _QCOM_IPC_INTERFACE
{
    INTERFACE Header;
    PQCOM_IPC_OPEN Open;
    PQCOM_IPC_CLOSE Close;
    PQCOM_IPC_GETADDR GetAddr;
    PQCOM_IPC_BIND Bind;
    PQCOM_IPC_LOOKUP Lookup;
    PQCOM_IPC_SENDTO SendTo;
    PQCOM_IPC_RECVFROM RecvFrom;
} QCOM_IPC_INTERFACE, *PQCOM_IPC_INTERFACE;

#ifdef _WIN64
C_ASSERT(sizeof(QCOM_IPC_INTERFACE) == 0x58);
C_ASSERT(sizeof(QCOM_SMDL_INTERFACE) == 0x70);
C_ASSERT(sizeof(QCOM_SMEM_INTERFACE) == 0x40);
C_ASSERT(sizeof(QCOM_SMP2P_INTERFACE) == 0x60);
C_ASSERT(sizeof(QCOM_SMSM_INTERFACE) == 0x58);
C_ASSERT(FIELD_OFFSET(QCOM_SMDL_IOVEC, Buffer) == 0x10);
#endif
