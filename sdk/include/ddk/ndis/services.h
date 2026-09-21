/*
 * PROJECT:     ReactOS NDIS 6 support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     NDIS 6.x timer, work item, memory, device, bus and lock services
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define NDIS_OBJECT_TYPE_DEVICE_OBJECT_ATTRIBUTES                    0x85
#define NDIS_OBJECT_TYPE_TIMER_CHARACTERISTICS                       0x97

/* Timer objects */

#define NDIS_TIMER_CHARACTERISTICS_REVISION_1                        1

typedef struct _NDIS_TIMER_CHARACTERISTICS
{
    NDIS_OBJECT_HEADER Header;
    ULONG AllocationTag;
    PNDIS_TIMER_FUNCTION TimerFunction;
    PVOID FunctionContext;
} NDIS_TIMER_CHARACTERISTICS, *PNDIS_TIMER_CHARACTERISTICS;

#define NDIS_SIZEOF_TIMER_CHARACTERISTICS_REVISION_1 \
    RTL_SIZEOF_THROUGH_FIELD(NDIS_TIMER_CHARACTERISTICS, FunctionContext)

_Must_inspect_result_
_IRQL_requires_max_(DISPATCH_LEVEL)
NDIS_STATUS
NTAPI
NdisAllocateTimerObject(
    _In_ NDIS_HANDLE NdisHandle,
    _In_ PNDIS_TIMER_CHARACTERISTICS TimerCharacteristics,
    _Out_ PNDIS_HANDLE pTimerObject);

_IRQL_requires_max_(DISPATCH_LEVEL)
BOOLEAN
NTAPI
NdisSetTimerObject(
    _In_ NDIS_HANDLE TimerObject,
    _In_ LARGE_INTEGER DueTime,
    _In_opt_ LONG MillisecondsPeriod,
    _In_opt_ PVOID FunctionContext);

_IRQL_requires_max_(DISPATCH_LEVEL)
BOOLEAN
NTAPI
NdisCancelTimerObject(
    _In_ NDIS_HANDLE TimerObject);

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
NTAPI
NdisFreeTimerObject(
    _In_ NDIS_HANDLE TimerObject);

/* I/O work items */

typedef VOID (NTAPI NDIS_IO_WORKITEM_FUNCTION)(
    _In_opt_ PVOID WorkItemContext,
    _In_ NDIS_HANDLE NdisIoWorkItemHandle);
typedef NDIS_IO_WORKITEM_FUNCTION (*NDIS_IO_WORKITEM_ROUTINE);

_Must_inspect_result_
_IRQL_requires_max_(DISPATCH_LEVEL)
NDIS_HANDLE
NTAPI
NdisAllocateIoWorkItem(
    _In_ NDIS_HANDLE NdisObjectHandle);

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
NTAPI
NdisQueueIoWorkItem(
    _In_ NDIS_HANDLE NdisIoWorkItemHandle,
    _In_ NDIS_IO_WORKITEM_ROUTINE Routine,
    _In_ PVOID WorkItemContext);

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
NTAPI
NdisFreeIoWorkItem(
    _In_ NDIS_HANDLE NdisIoWorkItemHandle);

/* Memory */

_Must_inspect_result_
_IRQL_requires_max_(DISPATCH_LEVEL)
PVOID
NTAPI
NdisAllocateMemoryWithTagPriority(
    _In_ NDIS_HANDLE NdisHandle,
    _In_ UINT Length,
    _In_ ULONG Tag,
    _In_ EX_POOL_PRIORITY Priority);

VOID
NTAPI
NdisFreeMemoryWithTagPriority(
    _In_ NDIS_HANDLE NdisHandle,
    _In_ PVOID VirtualAddress,
    _In_ ULONG Tag);

_Must_inspect_result_
_IRQL_requires_max_(DISPATCH_LEVEL)
PMDL
NTAPI
NdisAllocateMdl(
    _In_ NDIS_HANDLE NdisHandle,
    _In_reads_bytes_(Length) PVOID VirtualAddress,
    _In_ ULONG Length);

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
NTAPI
NdisFreeMdl(
    _In_ PMDL Mdl);

/* Device objects */

#define NDIS_DEVICE_OBJECT_ATTRIBUTES_REVISION_1                     1

typedef struct _NDIS_DEVICE_OBJECT_ATTRIBUTES
{
    NDIS_OBJECT_HEADER Header;
    PNDIS_STRING DeviceName;
    PNDIS_STRING SymbolicName;
    PDRIVER_DISPATCH *MajorFunctions;
    ULONG ExtensionSize;
    PCUNICODE_STRING DefaultSDDLString;
    LPCGUID DeviceClassGuid;
} NDIS_DEVICE_OBJECT_ATTRIBUTES, *PNDIS_DEVICE_OBJECT_ATTRIBUTES;

#define NDIS_SIZEOF_DEVICE_OBJECT_ATTRIBUTES_REVISION_1 \
    RTL_SIZEOF_THROUGH_FIELD(NDIS_DEVICE_OBJECT_ATTRIBUTES, DeviceClassGuid)

_IRQL_requires_(PASSIVE_LEVEL)
NDIS_STATUS
NTAPI
NdisRegisterDeviceEx(
    _In_ NDIS_HANDLE NdisHandle,
    _In_ PNDIS_DEVICE_OBJECT_ATTRIBUTES DeviceObjectAttributes,
    _Out_ PDEVICE_OBJECT *pDeviceObject,
    _Out_ PNDIS_HANDLE NdisDeviceHandle);

_IRQL_requires_(PASSIVE_LEVEL)
VOID
NTAPI
NdisDeregisterDeviceEx(
    _In_ NDIS_HANDLE NdisDeviceHandle);

_IRQL_requires_max_(HIGH_LEVEL)
PVOID
NTAPI
NdisGetDeviceReservedExtension(
    _In_ PDEVICE_OBJECT DeviceObject);

/* Bus configuration space */

_Must_inspect_result_
_IRQL_requires_max_(HIGH_LEVEL)
ULONG
NTAPI
NdisMGetBusData(
    _In_ NDIS_HANDLE NdisMiniportHandle,
    _In_ ULONG WhichSpace,
    _In_ ULONG Offset,
    _Out_writes_bytes_all_(Length) PVOID Buffer,
    _In_ ULONG Length);

_IRQL_requires_max_(HIGH_LEVEL)
ULONG
NTAPI
NdisMSetBusData(
    _In_ NDIS_HANDLE NdisMiniportHandle,
    _In_ ULONG WhichSpace,
    _In_ ULONG Offset,
    _In_reads_bytes_(Length) PVOID Buffer,
    _In_ ULONG Length);

/* Reader writer locks */

typedef struct _NDIS_RW_LOCK_EX NDIS_RW_LOCK_EX, *PNDIS_RW_LOCK_EX;

typedef struct _LOCK_STATE_EX
{
    KIRQL OldIrql;
    UCHAR LockState;
    UCHAR Flags;
} LOCK_STATE_EX, *PLOCK_STATE_EX;

#define NDIS_RWL_AT_DISPATCH_LEVEL                                   1

_IRQL_requires_max_(DISPATCH_LEVEL)
PNDIS_RW_LOCK_EX
NTAPI
NdisAllocateRWLock(
    _In_ NDIS_HANDLE NdisHandle);

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
NTAPI
NdisFreeRWLock(
    _In_ PNDIS_RW_LOCK_EX Lock);

_IRQL_raises_(DISPATCH_LEVEL)
VOID
NTAPI
NdisAcquireRWLockRead(
    _In_ PNDIS_RW_LOCK_EX Lock,
    _Out_ PLOCK_STATE_EX LockState,
    _In_ UCHAR Flags);

_IRQL_raises_(DISPATCH_LEVEL)
VOID
NTAPI
NdisAcquireRWLockWrite(
    _In_ PNDIS_RW_LOCK_EX Lock,
    _Out_ PLOCK_STATE_EX LockState,
    _In_ UCHAR Flags);

_IRQL_requires_(DISPATCH_LEVEL)
VOID
NTAPI
NdisReleaseRWLock(
    _In_ PNDIS_RW_LOCK_EX Lock,
    _In_ PLOCK_STATE_EX LockState);

/* Miniport control */

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
NTAPI
NdisMResetMiniport(
    _In_ NDIS_HANDLE MiniportAdapterHandle);

#ifdef __cplusplus
}
#endif
