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

/* Optional driver handlers */

#define NDIS_OBJECT_TYPE_MINIPORT_PNP_CHARACTERISTICS                0x92
#define NDIS_OBJECT_TYPE_MINIPORT_SS_CHARACTERISTICS                 0xB4

typedef struct _NDIS_DRIVER_OPTIONAL_HANDLERS
{
    NDIS_OBJECT_HEADER Header;
} NDIS_DRIVER_OPTIONAL_HANDLERS, *PNDIS_DRIVER_OPTIONAL_HANDLERS;

_IRQL_requires_(PASSIVE_LEVEL)
NDIS_STATUS
NTAPI
NdisSetOptionalHandlers(
    _In_ NDIS_HANDLE NdisHandle,
    _In_ PNDIS_DRIVER_OPTIONAL_HANDLERS OptionalHandlers);

typedef NDIS_STATUS (NTAPI MINIPORT_ADD_DEVICE)(
    _In_ NDIS_HANDLE NdisMiniportHandle,
    _In_ NDIS_HANDLE MiniportDriverContext);
typedef MINIPORT_ADD_DEVICE *MINIPORT_ADD_DEVICE_HANDLER;

typedef VOID (NTAPI MINIPORT_REMOVE_DEVICE)(
    _In_ NDIS_HANDLE MiniportAddDeviceContext);
typedef MINIPORT_REMOVE_DEVICE *MINIPORT_REMOVE_DEVICE_HANDLER;

typedef NDIS_STATUS (NTAPI MINIPORT_PNP_IRP)(
    _In_ NDIS_HANDLE MiniportAddDeviceContext,
    _In_ PIRP Irp);
typedef MINIPORT_PNP_IRP *MINIPORT_PNP_IRP_HANDLER;
typedef MINIPORT_PNP_IRP MINIPORT_START_DEVICE;
typedef MINIPORT_PNP_IRP *MINIPORT_START_DEVICE_HANDLER;
typedef MINIPORT_PNP_IRP MINIPORT_FILTER_RESOURCE_REQUIREMENTS;
typedef MINIPORT_PNP_IRP *MINIPORT_FILTER_RESOURCE_REQUIREMENTS_HANDLER;

#define NDIS_MINIPORT_PNP_CHARACTERISTICS_REVISION_1                 1

typedef struct _NDIS_MINIPORT_PNP_CHARACTERISTICS
{
    NDIS_OBJECT_HEADER Header;
    MINIPORT_ADD_DEVICE_HANDLER MiniportAddDeviceHandler;
    MINIPORT_REMOVE_DEVICE_HANDLER MiniportRemoveDeviceHandler;
    MINIPORT_FILTER_RESOURCE_REQUIREMENTS_HANDLER MiniportFilterResourceRequirementsHandler;
    MINIPORT_START_DEVICE_HANDLER MiniportStartDeviceHandler;
    ULONG Flags;
} NDIS_MINIPORT_PNP_CHARACTERISTICS, *PNDIS_MINIPORT_PNP_CHARACTERISTICS;

#define NDIS_SIZEOF_MINIPORT_PNP_CHARACTERISTICS_REVISION_1 \
    RTL_SIZEOF_THROUGH_FIELD(NDIS_MINIPORT_PNP_CHARACTERISTICS, Flags)

/* Selective suspend */

typedef NDIS_STATUS (NTAPI MINIPORT_IDLE_NOTIFICATION)(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ BOOLEAN ForceIdle);
typedef MINIPORT_IDLE_NOTIFICATION *MINIPORT_IDLE_NOTIFICATION_HANDLER;

typedef VOID (NTAPI MINIPORT_CANCEL_IDLE_NOTIFICATION)(
    _In_ NDIS_HANDLE MiniportAdapterContext);
typedef MINIPORT_CANCEL_IDLE_NOTIFICATION *MINIPORT_CANCEL_IDLE_NOTIFICATION_HANDLER;

#define NDIS_MINIPORT_SS_CHARACTERISTICS_REVISION_1                  1

typedef struct _NDIS_MINIPORT_SS_CHARACTERISTICS
{
    NDIS_OBJECT_HEADER Header;
    ULONG Flags;
    MINIPORT_IDLE_NOTIFICATION_HANDLER IdleNotificationHandler;
    MINIPORT_CANCEL_IDLE_NOTIFICATION_HANDLER CancelIdleNotificationHandler;
} NDIS_MINIPORT_SS_CHARACTERISTICS, *PNDIS_MINIPORT_SS_CHARACTERISTICS;

#define NDIS_SIZEOF_MINIPORT_SS_CHARACTERISTICS_REVISION_1 \
    RTL_SIZEOF_THROUGH_FIELD(NDIS_MINIPORT_SS_CHARACTERISTICS, CancelIdleNotificationHandler)

_IRQL_requires_(PASSIVE_LEVEL)
VOID
NTAPI
NdisMIdleNotificationConfirm(
    _In_ NDIS_HANDLE MiniportAdapterHandle,
    _In_ NDIS_DEVICE_POWER_STATE IdlePowerState);

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
NTAPI
NdisMIdleNotificationComplete(
    _In_ NDIS_HANDLE MiniportAdapterHandle);

/* SR-IOV */

typedef USHORT NDIS_SRIOV_FUNCTION_ID, *PNDIS_SRIOV_FUNCTION_ID;

_IRQL_requires_max_(PASSIVE_LEVEL)
NDIS_STATUS
NTAPI
NdisMEnableVirtualization(
    _In_ NDIS_HANDLE NdisMiniportHandle,
    _In_ USHORT NumVFs,
    _In_ BOOLEAN EnableVFMigration,
    _In_ BOOLEAN EnableMigrationInterrupt,
    _In_ BOOLEAN EnableVirtualization);

_IRQL_requires_max_(DISPATCH_LEVEL)
ULONG
NTAPI
NdisMGetVirtualFunctionBusData(
    _In_ NDIS_HANDLE NdisMiniportHandle,
    _In_ NDIS_SRIOV_FUNCTION_ID VFId,
    _Out_writes_bytes_(Length) PVOID Buffer,
    _In_ ULONG Offset,
    _In_ ULONG Length);

_IRQL_requires_max_(DISPATCH_LEVEL)
ULONG
NTAPI
NdisMSetVirtualFunctionBusData(
    _In_ NDIS_HANDLE NdisMiniportHandle,
    _In_ NDIS_SRIOV_FUNCTION_ID VFId,
    _In_reads_bytes_(Length) PVOID Buffer,
    _In_ ULONG Offset,
    _In_ ULONG Length);

_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
NTAPI
NdisMGetVirtualFunctionLocation(
    _In_ NDIS_HANDLE NdisMiniportHandle,
    _In_ NDIS_SRIOV_FUNCTION_ID VFId,
    _Out_ PUSHORT SegmentNumber,
    _Out_ PUCHAR BusNumber,
    _Out_ PUCHAR FunctionNumber);

_IRQL_requires_max_(PASSIVE_LEVEL)
NDIS_STATUS
NTAPI
NdisMQueryProbedBars(
    _In_ NDIS_HANDLE NdisMiniportHandle,
    _Out_writes_(PCI_TYPE0_ADDRESSES) PULONG BaseRegisterValues);

/* Processors */

typedef enum _NDIS_PROCESSOR_VENDOR
{
    NdisProcessorVendorUnknown,
    NdisProcessorVendorGenuinIntel,
    NdisProcessorVendorGenuineIntel = NdisProcessorVendorGenuinIntel,
    NdisProcessorVendorAuthenticAMD
} NDIS_PROCESSOR_VENDOR, *PNDIS_PROCESSOR_VENDOR;

typedef struct _NDIS_PROCESSOR_INFO_EX
{
    PROCESSOR_NUMBER ProcNum;
    ULONG SocketId;
    ULONG CoreId;
    ULONG HyperThreadId;
    USHORT NodeId;
    USHORT NodeDistance;
} NDIS_PROCESSOR_INFO_EX, *PNDIS_PROCESSOR_INFO_EX;

#define NDIS_SYSTEM_PROCESSOR_INFO_EX_REVISION_1                     1

typedef struct _NDIS_SYSTEM_PROCESSOR_INFO_EX
{
    NDIS_OBJECT_HEADER Header;
    ULONG Flags;
    NDIS_PROCESSOR_VENDOR ProcessorVendor;
    ULONG NumSockets;
    ULONG NumCores;
    ULONG NumCoresPerSocket;
    ULONG MaxHyperThreadingProcsPerCore;
    ULONG ProcessorInfoOffset;
    ULONG NumberOfProcessors;
    ULONG ProcessorInfoEntrySize;
} NDIS_SYSTEM_PROCESSOR_INFO_EX, *PNDIS_SYSTEM_PROCESSOR_INFO_EX;

#define NDIS_SIZEOF_SYSTEM_PROCESSOR_INFO_EX_REVISION_1 \
    RTL_SIZEOF_THROUGH_FIELD(NDIS_SYSTEM_PROCESSOR_INFO_EX, ProcessorInfoEntrySize)

ULONG
NTAPI
NdisGroupActiveProcessorCount(
    _In_ USHORT Group);

_IRQL_requires_max_(DISPATCH_LEVEL)
NDIS_STATUS
NTAPI
NdisGetProcessorInformationEx(
    _In_opt_ NDIS_HANDLE NdisHandle,
    _Out_writes_bytes_to_opt_(*Size, *Size) PNDIS_SYSTEM_PROCESSOR_INFO_EX SystemProcessorInfo,
    _Inout_ PSIZE_T Size);

#ifdef __cplusplus
}
#endif
