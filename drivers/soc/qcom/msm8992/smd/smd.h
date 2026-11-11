/*
 * PROJECT:     ReactOS Qualcomm SMD driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Private definitions
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#include <ntddk.h>
#include <wdf.h>
#include <acpiioct.h>
#include <initguid.h>
#include <qcom/smdif.h>

#define SMD_TAG                     'dmSQ'

/* Fatal shared memory corruption is not recoverable, the reference driver stops the system too */
#define SMD_BUGCHECK_DRIVER_VIOLATION 0x121
#define SMD_FATAL(Code)             KeBugCheckEx(SMD_BUGCHECK_DRIVER_VIOLATION, 0x534D44, (Code), 0, 0)

/* Resource order in _CRS */
#define SMD_RES_SMEM                0
#define SMD_RES_MUTEX               1
#define SMD_RES_SMEM_TARGET         2
#define SMD_RES_AUX                 3
#define SMD_RES_IPC_FIRST           4
#define SMD_MAX_IPC_REGS            4

/* SMEM fixed layout */
#define SMEM_ITEM_VERSION           3
#define SMEM_ITEM_SMD_VERSION       107
#define SMEM_ITEM_CHANNEL_TABLE     13
#define SMEM_ITEM_CHANNEL_TABLE2    266
#define SMEM_ITEM_SMD_CTRL_BASE     14
#define SMEM_ITEM_SMD_FIFO_BASE     338
#define SMEM_ITEM_SMD_CTRL_BASE2    138
#define SMEM_ITEM_SMD_FIFO_BASE2    202
#define SMEM_ITEM_SMD_FEATURES      411
#define SMEM_ITEM_SMSM_STATE        85
#define SMEM_ITEM_SMSM_MASKS        333
#define SMEM_ITEM_SMSM_SIZE         419
#define SMEM_ITEM_SMP2P_OUT_BASE    427
#define SMEM_ITEM_COUNT             489
#define SMEM_FIRST_DYNAMIC_ITEM     9

#define SMEM_OFFSET_VERSION         0x0040
#define SMEM_OFFSET_HEAP_INFO       0x00C0
#define SMEM_OFFSET_TOC             0x00D0
#define SMEM_OFFSET_RESET_DETECT    0x20D0

#define SMEM_VERSION_APPS_INDEX     8
#define SMEM_APPS_VERSION           0x000B2002
#define SMEM_VERSION_MASK           0xFFFF0000
#define SMD_PROTOCOL_VERSION        0x00020000

#define SMEM_MUTEX_HEAP             3
#define SMEM_MUTEX_COUNT            8
#define SMEM_MUTEX_APPS_VALUE       1

/* SMEM request flags */
#define SMEM_FLAG_CACHED            0x80000000
#define SMEM_FLAG_NO_GLOBAL         0x40000000

/* SMEM request results */
#define SMEM_OK                     0
#define SMEM_E_INVALID              (-2)
#define SMEM_E_NO_SPACE             (-3)
#define SMEM_E_NOT_FOUND            (-4)
#define SMEM_E_SIZE_MISMATCH        (-6)

#include <pshpack1.h>
typedef struct _SMEM_HEAP_INFO
{
    ULONG Initialized;
    ULONG FreeOffset;
    ULONG HeapRemaining;
    ULONG Reserved;
} SMEM_HEAP_INFO, *PSMEM_HEAP_INFO;

typedef struct _SMEM_TOC_ENTRY
{
    ULONG Allocated;
    ULONG Offset;
    ULONG Size;
    ULONG Reserved;
} SMEM_TOC_ENTRY, *PSMEM_TOC_ENTRY;

typedef struct _SMEM_PTABLE
{
    ULONG Magic;
    ULONG Version;
    ULONG NumEntries;
    ULONG Reserved[5];
} SMEM_PTABLE, *PSMEM_PTABLE;

typedef struct _SMEM_PTABLE_ENTRY
{
    ULONG Offset;
    ULONG Size;
    ULONG Flags;
    USHORT Host0;
    USHORT Host1;
    ULONG CacheLine;
    ULONG Reserved[7];
} SMEM_PTABLE_ENTRY, *PSMEM_PTABLE_ENTRY;

typedef struct _SMEM_PARTITION_HEADER
{
    ULONG Magic;
    USHORT Host0;
    USHORT Host1;
    ULONG Size;
    ULONG FreeUncached;
    ULONG FreeCached;
    ULONG Reserved[3];
} SMEM_PARTITION_HEADER, *PSMEM_PARTITION_HEADER;

typedef struct _SMEM_PRIVATE_ENTRY
{
    USHORT Canary;
    USHORT Item;
    ULONG Size;
    USHORT PaddingData;
    USHORT PaddingHeader;
    ULONG Reserved;
} SMEM_PRIVATE_ENTRY, *PSMEM_PRIVATE_ENTRY;

typedef struct _SMD_CHANNEL_ENTRY
{
    CHAR Name[20];
    ULONG ChannelId;
    ULONG Type;
    ULONG RefCount;
} SMD_CHANNEL_ENTRY, *PSMD_CHANNEL_ENTRY;

/* One direction of a channel, written only by the processor that owns it */
typedef struct _SMD_HALF_BYTE
{
    ULONG State;
    UCHAR Dsr;
    UCHAR Cts;
    UCHAR Cd;
    UCHAR Ri;
    UCHAR HeadFlag;
    UCHAR TailFlag;
    UCHAR StateFlag;
    UCHAR BlockReadIntr;
    ULONG Tail;
    ULONG Head;
} SMD_HALF_BYTE, *PSMD_HALF_BYTE;

typedef struct _SMD_HALF_WORD
{
    ULONG State;
    ULONG Dsr;
    ULONG Cts;
    ULONG Cd;
    ULONG Ri;
    ULONG HeadFlag;
    ULONG TailFlag;
    ULONG StateFlag;
    ULONG BlockReadIntr;
    ULONG Tail;
    ULONG Head;
} SMD_HALF_WORD, *PSMD_HALF_WORD;
#include <poppack.h>

C_ASSERT(sizeof(SMEM_PTABLE) == 0x20);
C_ASSERT(sizeof(SMEM_PTABLE_ENTRY) == 0x30);
C_ASSERT(sizeof(SMEM_PARTITION_HEADER) == 0x20);
C_ASSERT(sizeof(SMEM_PRIVATE_ENTRY) == 0x10);
C_ASSERT(sizeof(SMD_CHANNEL_ENTRY) == 0x20);
C_ASSERT(sizeof(SMD_HALF_BYTE) == 0x14);
C_ASSERT(sizeof(SMD_HALF_WORD) == 0x2C);

#define SMEM_PTABLE_MAGIC           0x434F5424
#define SMEM_PARTITION_MAGIC        0x54525024
#define SMEM_PRIVATE_CANARY         0xA5A5
#define SMEM_PTABLE_MAX_ENTRIES     16

#define SMD_CHANNEL_TABLE_SIZE      2048
#define SMD_CHANNELS_PER_TABLE      64
#define SMD_DEFAULT_FIFO_SIZE       0x2000
#define SMD_MIN_FIFO_SIZE           0x400
#define SMD_MAX_FIFO_SIZE           0x20000
#define SMD_PACKET_HEADER_SIZE      20
#define SMD_NAME_LENGTH             20

/* Channel states in SMD_HALF.State */
#define SMD_STATE_CLOSED            0
#define SMD_STATE_OPENING           1
#define SMD_STATE_OPENED            2
#define SMD_STATE_FLUSHING          3
#define SMD_STATE_CLOSING           4
#define SMD_STATE_RESET             5
#define SMD_STATE_RESET_OPENING     6

/* State machine inputs */
#define SMD_INPUT_LOCAL_CLOSE       0
#define SMD_INPUT_LOCAL_OPEN        1
#define SMD_INPUT_REMOTE_OPEN       2
#define SMD_INPUT_REMOTE_CLOSE      3
#define SMD_INPUT_REMOTE_FLUSH      4
#define SMD_INPUT_FLUSH_DONE        5
#define SMD_INPUT_REMOTE_REOPEN     6
#define SMD_INPUT_REMOTE_RESET      7
#define SMD_INPUT_NONE              0xFF

/* Interrupt kinds from the INTR method */
#define SMD_IRQ_SMD                 0
#define SMD_IRQ_SMP2P               1
#define SMD_IRQ_SMSM                2

typedef struct _SMD_DEVICE *PSMD_DEVICE;

/* Lock that the owning CPU may take again, SMD callbacks call back into SMD */
typedef struct _SMD_RLOCK
{
    KSPIN_LOCK Lock;
    PKTHREAD Owner;
    ULONG Depth;
    KIRQL OldIrql;
} SMD_RLOCK, *PSMD_RLOCK;

typedef struct _SMD_PORT
{
    LIST_ENTRY HostLink;
    PSMD_DEVICE Device;
    CHAR Name[SMD_NAME_LENGTH];
    ULONG Edge;
    ULONG RemoteHost;
    ULONG PartitionHost;
    ULONG ChannelId;
    ULONG FifoSize;
    BOOLEAN Stream;
    BOOLEAN WordAccess;
    BOOLEAN TxBlocked;
    BOOLEAN WaitRemoteClose;
    BOOLEAN FlushPending;
    BOOLEAN CloseRequested;
    BOOLEAN Linked;
    ULONG LocalState;
    ULONG RemoteState;
    PVOID TxHalf;
    PVOID RxHalf;
    PUCHAR TxFifo;
    PUCHAR RxFifo;
    ULONG RemoteSignals[4];
    /* Packet mode read assembly */
    UCHAR Header[SMD_PACKET_HEADER_SIZE];
    ULONG HeaderBytes;
    ULONG PacketRemaining;
    PQCOM_SMDL_NOTIFY Notify;
    PVOID NotifyContext;
} SMD_PORT, *PSMD_PORT;

typedef struct _SMD_HOST
{
    SMD_RLOCK Lock;
    LIST_ENTRY Ports;
    PULONG SmdIrqRegister;
    ULONG SmdIrqMask;
    PULONG Smp2pIrqRegister;
    ULONG Smp2pIrqMask;
    PULONG SmsmIrqRegister;
    ULONG SmsmIrqMask;
} SMD_HOST, *PSMD_HOST;

typedef struct _SMEM_PARTITION
{
    PSMEM_PARTITION_HEADER Header;
    ULONG Size;
    ULONG CacheLine;
} SMEM_PARTITION, *PSMEM_PARTITION;

typedef struct _SMD_IRQ_DESC
{
    ULONG Kind;
    ULONG Host;
    ULONGLONG RegisterPhys;
    ULONG Mask;
} SMD_IRQ_DESC, *PSMD_IRQ_DESC;

typedef struct _SMD_DEVICE
{
    WDFDEVICE WdfDevice;

    PUCHAR SmemBase;
    ULONG SmemSize;
    PHYSICAL_ADDRESS SmemPhys;
    PUCHAR AuxBase;
    ULONG AuxSize;
    PUCHAR MutexBase;
    ULONG MutexStride;
    ULONG MutexDepth[SMEM_MUTEX_COUNT];
    SMD_RLOCK MutexLock;

    ULONG IpcRegCount;
    PHYSICAL_ADDRESS IpcRegPhys[SMD_MAX_IPC_REGS];
    PULONG IpcRegVa[SMD_MAX_IPC_REGS];

    BOOLEAN SmemReady;
    SMEM_PARTITION Partitions[QCOM_HOST_COUNT];

    ULONG IrqCount;
    SMD_IRQ_DESC Irqs[16];

    SMD_HOST Hosts[QCOM_HOST_COUNT];
} SMD_DEVICE;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SMD_DEVICE, SmdGetDevice);

typedef struct _SMD_IRQ_CONTEXT
{
    PSMD_DEVICE Device;
    ULONG Kind;
    ULONG Host;
} SMD_IRQ_CONTEXT, *PSMD_IRQ_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SMD_IRQ_CONTEXT, SmdGetIrqContext);

/* The single instance, the exported function tables carry no context */
extern PSMD_DEVICE SmdGlobal;

/* lock.c */
VOID
SmdRLockInitialize(
    _Out_ PSMD_RLOCK Lock);

VOID
SmdRLockAcquire(
    _Inout_ PSMD_RLOCK Lock);

VOID
SmdRLockRelease(
    _Inout_ PSMD_RLOCK Lock);

/* smem.c */
NTSTATUS
SmemInitialize(
    _Inout_ PSMD_DEVICE Device);

VOID
SmemMutexAcquire(
    _Inout_ PSMD_DEVICE Device,
    _In_ ULONG Index);

VOID
SmemMutexRelease(
    _Inout_ PSMD_DEVICE Device,
    _In_ ULONG Index);

VOID
SmemMutexReleaseHost(
    _Inout_ PSMD_DEVICE Device,
    _In_ ULONG Host);

LONG
SmemAllocate(
    _Inout_ PSMD_DEVICE Device,
    _In_ ULONG Host,
    _In_ ULONG Item,
    _Inout_ PULONG Size,
    _In_ ULONG Flags,
    _Out_ PVOID *Address);

LONG
SmemLookup(
    _Inout_ PSMD_DEVICE Device,
    _In_ ULONG Host,
    _In_ ULONG Item,
    _Out_ PULONG Size,
    _Out_ PVOID *Address);

PVOID
NTAPI
SmemIfAlloc(
    _In_ ULONG Item,
    _In_ ULONG Size);

ULONG
NTAPI
SmemIfVersionSet(
    _In_ ULONG Item,
    _In_ ULONG Version,
    _In_ ULONG Mask);

PVOID
NTAPI
SmemIfGetAddr(
    _In_ ULONG Item,
    _Out_opt_ PULONG Size);

PVOID
NTAPI
SmemIfGetIpcRegister(
    _In_ ULONGLONG PhysicalAddress);

/* channel.c */
VOID
SmdSignalHost(
    _In_ PSMD_DEVICE Device,
    _In_ ULONG Host);

NTSTATUS
SmdInitialize(
    _Inout_ PSMD_DEVICE Device);

VOID
SmdProcessHost(
    _Inout_ PSMD_DEVICE Device,
    _In_ ULONG Host);

VOID
SmdResetHost(
    _Inout_ PSMD_DEVICE Device,
    _In_ ULONG Host);

ULONG
SmdEdgeRemoteHost(
    _In_ ULONG Edge);

extern const QCOM_SMDL_INTERFACE SmdlInterfaceTemplate;

/* smp2p.c and smsm.c are not part of the build yet, their interfaces are not exposed */
