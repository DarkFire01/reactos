
#pragma once

#include <wmiioctl.h>

extern POBJECT_TYPE WmipGuidObjectType;

#define GUID_STRING_LENGTH 36

/* How a GUID object was opened, which decides the requests WMI sends for it */
#define WMIP_OPEN_PLAIN     0
#define WMIP_OPEN_QUERYSET  1
#define WMIP_OPEN_EVENTS    2

/* Events not yet picked up by IOCTL_WMI_RECEIVE_NOTIFICATIONS, chained through Linkage */
typedef struct _WMIP_EVENT_QUEUE
{
    PUCHAR Buffer;
    ULONG Size;
    ULONG Used;
    ULONG LastOffset;
    ULONG EventsLost;
} WMIP_EVENT_QUEUE, *PWMIP_EVENT_QUEUE;

/* Head of the GUID objects a pending receive IRP waits on, kept in its DriverContext */
typedef struct _WMIP_IRP_CONTEXT
{
    LIST_ENTRY GuidObjectListHead;
} WMIP_IRP_CONTEXT, *PWMIP_IRP_CONTEXT;

typedef struct _WMIP_GUID_OBJECT
{
    KEVENT Event;
    GUID Guid;
    PIRP Irp;
    LIST_ENTRY IrpLink;
    WMI_NOTIFICATION_CALLBACK NotificationCallback;
    PVOID NotificationContext;
    LIST_ENTRY ConsumerLink;
    ULONG OpenType;
    BOOLEAN KernelConsumer;
    BOOLEAN Linked;
    WMIP_EVENT_QUEUE Queue;
} WMIP_GUID_OBJECT, *PWMIP_GUID_OBJECT;


_Function_class_(DRIVER_INITIALIZE)
_IRQL_requires_same_
NTSTATUS
NTAPI
WmipDriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath);

NTSTATUS
NTAPI
WmipInitializeGuidObjectType(
    VOID);

NTSTATUS
NTAPI
WmipOpenGuidObject(
    _In_ LPCGUID Guid,
    _In_ ACCESS_MASK DesiredAccess,
    _In_ KPROCESSOR_MODE AccessMode,
    _Out_ PHANDLE OutGuidObjectHandle,
    _Outptr_ PVOID *OutGuidObject);

NTSTATUS
NTAPI
WmipGuidFromObjectName(
    _In_ POBJECT_ATTRIBUTES ObjectAttributes,
    _Out_ PGUID Guid);

NTSTATUS
NTAPI
WmipOpenGuidObjectByName(
    _In_ POBJECT_ATTRIBUTES ObjectAttributes,
    _In_ ACCESS_MASK DesiredAccess,
    _In_ KPROCESSOR_MODE AccessMode,
    _Out_ PHANDLE OutGuidObjectHandle,
    _Outptr_ PVOID *OutGuidObject);

extern PDEVICE_OBJECT WmipServiceDeviceObject;

/* WNODE_ALL_DATA with room for one OFFSETINSTANCEDATAANDLENGTH, the smallest one Windows sends */
#define WMIP_MIN_ALL_DATA_SIZE \
    ((FIELD_OFFSET(WNODE_ALL_DATA, OffsetInstanceDataAndLength) + \
      sizeof(OFFSETINSTANCEDATAANDLENGTH) + 7) & ~7)

/* Registration action modifiers from the public wdm.h */
#ifndef WMIREG_FLAG_CALLBACK
#define WMIREG_FLAG_CALLBACK            0x80000000
#endif
#ifndef WMIREG_FLAG_TRACE_PROVIDER
#define WMIREG_FLAG_TRACE_PROVIDER      0x00010000
#endif
#ifndef WMIREG_FLAG_TRACE_NOTIFY_MASK
#define WMIREG_FLAG_TRACE_NOTIFY_MASK   0x00F00000
#endif

/* provider.c */

VOID
NTAPI
WmipInitializeProviders(
    VOID);

NTSTATUS
NTAPI
WmipRegisterProvider(
    _In_ PDEVICE_OBJECT DeviceObject);

NTSTATUS
NTAPI
WmipDeregisterProvider(
    _In_ PDEVICE_OBJECT DeviceObject);

NTSTATUS
NTAPI
WmipUpdateProvider(
    _In_ PDEVICE_OBJECT DeviceObject);

NTSTATUS
NTAPI
WmipBlockProvider(
    _In_ PDEVICE_OBJECT DeviceObject);

BOOLEAN
NTAPI
WmipIsQuerySetGuid(
    _In_ LPCGUID Guid);

NTSTATUS
NTAPI
WmipQueryAllData(
    _In_ PWMIP_GUID_OBJECT GuidObject,
    _Inout_updates_bytes_(BufferSize) PWNODE_ALL_DATA Wnode,
    _In_ ULONG BufferSize,
    _Out_ PULONG ReturnSize);

NTSTATUS
NTAPI
WmipSendSingleInstanceRequest(
    _In_ PWMIP_GUID_OBJECT GuidObject,
    _In_ UCHAR MinorFunction,
    _Inout_updates_bytes_(BufferSize) PWNODE_SINGLE_INSTANCE Wnode,
    _In_ ULONG BufferSize,
    _Out_ PULONG ReturnSize);

VOID
NTAPI
WmipControlGuidLocked(
    _In_ LPCGUID Guid,
    _In_ UCHAR MinorFunction);

NTSTATUS
NTAPI
WmipQueryGuidExpense(
    _In_ LPCGUID Guid,
    _Out_ PBOOLEAN IsExpensive);

NTSTATUS
NTAPI
WmipEnumerateGuids(
    _Out_writes_bytes_(BufferSize) PWMI_GUID_LIST GuidList,
    _In_ ULONG BufferSize,
    _In_ BOOLEAN WithProperties,
    _Out_ PULONG ReturnSize);

NTSTATUS
NTAPI
WmipEnumerateMofResources(
    _Out_writes_bytes_(BufferSize) PWMI_MOF_LIST MofList,
    _In_ ULONG BufferSize,
    _Out_ PULONG ReturnSize);

NTSTATUS
NTAPI
WmipGetPdoInstanceInfo(
    _In_ LPCGUID Guid,
    _In_ PDEVICE_OBJECT Pdo,
    _Out_ PUNICODE_STRING BaseName,
    _Out_ PULONG BaseIndex);

PWNODE_HEADER
NTAPI
WmipPrepareEvent(
    _In_ PWNODE_HEADER Wnode);

VOID
NTAPI
WmipFreeEventCopy(
    _In_ PWNODE_HEADER Wnode);

/* wmi.c */

NTSTATUS
NTAPI
WmipGetFilePdo(
    _In_ HANDLE FileHandle,
    _In_ KPROCESSOR_MODE AccessMode,
    _Outptr_ PDEVICE_OBJECT *Pdo);

/* consumer.c */

VOID
NTAPI
WmipInitializeConsumers(
    VOID);

VOID
NTAPI
WmipAcquireConsumerLock(
    VOID);

VOID
NTAPI
WmipReleaseConsumerLock(
    VOID);

BOOLEAN
NTAPI
WmipHasAnyConsumer(
    VOID);

BOOLEAN
NTAPI
WmipHasConsumerLocked(
    _In_ LPCGUID Guid,
    _In_ ULONG OpenType);

NTSTATUS
NTAPI
WmipAttachConsumer(
    _Inout_ PWMIP_GUID_OBJECT GuidObject,
    _In_ ULONG OpenType,
    _In_ KPROCESSOR_MODE AccessMode);

VOID
NTAPI
WmipDetachConsumer(
    _Inout_ PWMIP_GUID_OBJECT GuidObject);

NTSTATUS
NTAPI
WmipReceiveNotifications(
    _Inout_ PIRP Irp,
    _In_ PVOID Buffer,
    _In_ ULONG InputLength,
    _Inout_ PULONG OutputLength);

NTSTATUS
NTAPI
WmipQueryRawSMBiosTables(
    _Inout_ ULONG *InOutBufferSize,
    _Out_opt_ PVOID OutBuffer);

#if (NTDDI_VERSION < NTDDI_VISTA)
NTSTATUS
NTAPI
EtwWrite(
    _In_ REGHANDLE RegHandle,
    _In_ PCEVENT_DESCRIPTOR EventDescriptor,
    _In_opt_ LPCGUID ActivityId,
    _In_ ULONG UserDataCount,
    _In_reads_opt_(UserDataCount) PEVENT_DATA_DESCRIPTOR UserData);
#endif
