/*
 * PROJECT:     ReactOS UFS Host Controller Miniport Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Adapter extension and internal interface
 * COPYRIGHT:   Justin Miller (justinmiller100@gmail.com)
 */

#ifndef _STORUFS_H_
#define _STORUFS_H_

#include <ntddk.h>
#include <storport.h>
#include <srbhelper.h>
#include "ufshci.h"

/* Slot and transfer limits. */
#define UFS_MAX_SLOTS        32
#define UFS_MAX_TRANSFER     (128 * 1024)
#define UFS_PRDT_ENTRIES     ((UFS_MAX_TRANSFER / PAGE_SIZE) + 1)
#define UFS_MAX_TM_SLOTS     8

/* UTP Command Descriptor layout (one per transfer slot). */
#define UFS_UCD_CMD_OFFSET   0x000
#define UFS_UCD_RESP_OFFSET  0x080
#define UFS_UCD_PRDT_OFFSET  0x100
#define UFS_UCD_SIZE         0x400
#define UFS_RESP_OFFSET_DW   (UFS_UCD_RESP_OFFSET / sizeof(ULONG))
#define UFS_PRDT_OFFSET_DW   (UFS_UCD_PRDT_OFFSET / sizeof(ULONG))

/* Common buffer layout: UTRD list, UTMRD list, then the UCD array. */
#define UFS_UTRD_LIST_SIZE   (UFS_MAX_SLOTS * sizeof(UFS_UTRD))
#define UFS_UTMRD_SLOT_SIZE  0x80
#define UFS_UTMRD_LIST_SIZE  (UFS_MAX_TM_SLOTS * UFS_UTMRD_SLOT_SIZE)
#define UFS_UTRD_LIST_OFFSET 0x000
#define UFS_UTMRD_LIST_OFFSET ALIGN_UP_BY(UFS_UTRD_LIST_SIZE, 1024)
#define UFS_UCD_ARRAY_OFFSET  (UFS_UTMRD_LIST_OFFSET + ALIGN_UP_BY(UFS_UTMRD_LIST_SIZE, 1024))
#define UFS_COMMON_BUFFER_SIZE \
    (UFS_UCD_ARRAY_OFFSET + (UFS_MAX_SLOTS * UFS_UCD_SIZE))

/* Internal command timeouts. */
#define UFS_HCE_POLL_COUNT       1000000
#define UFS_UIC_POLL_COUNT       100000
#define UFS_SLOT_POLL_COUNT      500000
#define UFS_LINKSTARTUP_ATTEMPTS 5
#define UFS_NOPOUT_ATTEMPTS      5
#define UFS_DEVINIT_SET_ATTEMPTS 5
#define UFS_DEVINIT_CLEAR_POLLS  1000

typedef enum _UFS_ADAPTER_STATE
{
    UfsAdapterStopped = 0,
    UfsAdapterFound,
    UfsAdapterRunning,
    UfsAdapterFailed
} UFS_ADAPTER_STATE;

typedef struct _UFS_ADAPTER_EXTENSION
{
    PUCHAR Registers;             /* mapped UFSHCI BAR */
    UFS_ADAPTER_STATE State;
    BOOLEAN DumpMode;
    BOOLEAN MessageInterrupts;

    ULONG Capabilities;
    ULONG Version;
    ULONG Nutrs;                  /* transfer request slots in use */

    PVOID CommonBuffer;           /* uncached extension base (virtual) */
    STOR_PHYSICAL_ADDRESS CommonBufferPhysical;

    PUFS_UTRD UtrdList;           /* virtual base of the UTRD list */
    STOR_PHYSICAL_ADDRESS UtrdListPhysical;
    STOR_PHYSICAL_ADDRESS UtmrdListPhysical;
    PUCHAR UcdArray;              /* virtual base of the UCD array */
    STOR_PHYSICAL_ADDRESS UcdArrayPhysical;

    ULONG FreeSlots;              /* bit per free transfer slot */
    PVOID SlotSrb[UFS_MAX_SLOTS]; /* outstanding SRB per slot */
} UFS_ADAPTER_EXTENSION, *PUFS_ADAPTER_EXTENSION;

/* Stored in the SRB extension. */
typedef struct _UFS_REQUEST_CONTEXT
{
    ULONG Slot;
} UFS_REQUEST_CONTEXT, *PUFS_REQUEST_CONTEXT;

/* storufs.c */
VOID
StorUfsCompleteSrb(
    _In_ PUFS_ADAPTER_EXTENSION Adapter,
    _In_ PVOID Srb,
    _In_ UCHAR SrbStatus);

BOOLEAN
NTAPI
StorUfsMsgInterrupt(
    _In_ PVOID DeviceExtension,
    _In_ ULONG MessageId);

/* ufshci.c */
FORCEINLINE
ULONG
UfsRead(
    _In_ PUFS_ADAPTER_EXTENSION Adapter,
    _In_ ULONG Offset)
{
    return StorPortReadRegisterUlong(Adapter, (PULONG)(Adapter->Registers + Offset));
}

FORCEINLINE
VOID
UfsWrite(
    _In_ PUFS_ADAPTER_EXTENSION Adapter,
    _In_ ULONG Offset,
    _In_ ULONG Value)
{
    StorPortWriteRegisterUlong(Adapter, (PULONG)(Adapter->Registers + Offset), Value);
}

PUCHAR
UfsSlotUcd(
    _In_ PUFS_ADAPTER_EXTENSION Adapter,
    _In_ ULONG Slot);

STOR_PHYSICAL_ADDRESS
UfsSlotUcdPhysical(
    _In_ PUFS_ADAPTER_EXTENSION Adapter,
    _In_ ULONG Slot);

LONG
UfsAllocSlot(
    _In_ PUFS_ADAPTER_EXTENSION Adapter);

VOID
UfsFreeSlot(
    _In_ PUFS_ADAPTER_EXTENSION Adapter,
    _In_ ULONG Slot);

VOID
UfsBuildUtrd(
    _In_ PUFS_ADAPTER_EXTENSION Adapter,
    _In_ ULONG Slot,
    _In_ ULONG DataDirection,
    _In_ USHORT PrdtLength);

VOID
UfsRingDoorbell(
    _In_ PUFS_ADAPTER_EXTENSION Adapter,
    _In_ ULONG Slot);

NTSTATUS
UfsSendUic(
    _In_ PUFS_ADAPTER_EXTENSION Adapter,
    _In_ ULONG Opcode,
    _In_ ULONG Arg1,
    _In_ ULONG Arg2,
    _In_ ULONG Arg3,
    _Out_opt_ PULONG Result);

NTSTATUS
UfsHceReset(
    _In_ PUFS_ADAPTER_EXTENSION Adapter);

NTSTATUS
UfsRunSlotSynchronous(
    _In_ PUFS_ADAPTER_EXTENSION Adapter,
    _In_ ULONG Slot);

BOOLEAN
UfsScanCompletions(
    _In_ PUFS_ADAPTER_EXTENSION Adapter);

/* ufsinit.c */
BOOLEAN
UfsStartAdapter(
    _In_ PUFS_ADAPTER_EXTENSION Adapter);

/* ufsscsi.c */
BOOLEAN
UfsSubmitScsi(
    _In_ PUFS_ADAPTER_EXTENSION Adapter,
    _In_ PSCSI_REQUEST_BLOCK Srb);

#endif /* _STORUFS_H_ */
