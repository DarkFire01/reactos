/*
 * PROJECT:         ReactOS Kernel
 * LICENSE:         GPL - See COPYING in the top level directory
 * FILE:            ntoskrnl/wmi/wmi.c
 * PURPOSE:         I/O Windows Management Instrumentation (WMI) Support
 * PROGRAMMERS:     Alex Ionescu (alex.ionescu@reactos.org)
 */

/* INCLUDES *****************************************************************/

#include <ntoskrnl.h>
#define INITGUID
#include <wmiguid.h>
#include <wmidata.h>
#include <wmistr.h>

#include "wmip.h"

#define NDEBUG
#include <debug.h>

#define TAG_WMI_QUERY 'QimW'

typedef PVOID PWMI_LOGGER_INFORMATION; // FIXME

typedef enum _WMI_CLOCK_TYPE
{
    WMICT_DEFAULT,
    WMICT_SYSTEMTIME,
    WMICT_PERFCOUNTER,
    WMICT_PROCESS,
    WMICT_THREAD,
    WMICT_CPUCYCLE
} WMI_CLOCK_TYPE;

/*
 * The TEB activity ID sits at the end of the Vista Instrumentation array,
 * which the NT 5.2 layout ntoskrnl is built with still covers.
 */
#define ETWP_TEB_ACTIVITY_ID_SLOT   (13 - sizeof(GUID) / sizeof(PVOID))
#define EtwpTebActivityId(Teb)      ((LPGUID)&(Teb)->Instrumentation[ETWP_TEB_ACTIVITY_ID_SLOT])

#define ETWP_TEB_ACTIVITY_ID_OFFSET \
    (FIELD_OFFSET(TEB, Instrumentation) + ETWP_TEB_ACTIVITY_ID_SLOT * sizeof(PVOID))

#ifdef _M_AMD64
C_ASSERT(ETWP_TEB_ACTIVITY_ID_OFFSET == 0x1710);
#elif defined(_M_IX86)
C_ASSERT(ETWP_TEB_ACTIVITY_ID_OFFSET == 0xF50);
#endif

/* Activity IDs are a random per boot base followed by a running sequence number */
static ULONG EtwpActivityIdBase[2];
static volatile LONG64 EtwpActivityIdSequence;

/* FUNCTIONS *****************************************************************/

static
VOID
EtwpNewActivityId(
    _Out_ LPGUID ActivityId)
{
    LONG64 Sequence;

    do
    {
        Sequence = EtwpActivityIdSequence;
    } while (InterlockedCompareExchange64(&EtwpActivityIdSequence,
                                          Sequence + 1,
                                          Sequence) != Sequence);

    Sequence++;
    ActivityId->Data1 = EtwpActivityIdBase[0];
    ActivityId->Data2 = (USHORT)EtwpActivityIdBase[1];
    ActivityId->Data3 = (USHORT)(EtwpActivityIdBase[1] >> 16);
    RtlCopyMemory(ActivityId->Data4, &Sequence, sizeof(ActivityId->Data4));
}

BOOLEAN
NTAPI
WmiInitialize(
    VOID)
{
    UNICODE_STRING DriverName = RTL_CONSTANT_STRING(L"\\Driver\\WMIxWDM");
    NTSTATUS Status;
    ULONG Seed;

    Seed = KeQueryPerformanceCounter(NULL).LowPart ^ (ULONG)KeQueryInterruptTime();
    EtwpActivityIdBase[0] = RtlRandomEx(&Seed);
    EtwpActivityIdBase[1] = RtlRandomEx(&Seed);

    /* Providers may register as soon as the WMI device exists */
    WmipInitializeProviders();

    /* Initialize the GUID object type */
    Status = WmipInitializeGuidObjectType();
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("WmipInitializeGuidObjectType() failed: 0x%lx\n", Status);
        return FALSE;
    }

    /* Create the WMI driver */
    Status = IoCreateDriver(&DriverName, WmipDriverEntry);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Failed to create WMI driver: 0x%lx\n", Status);
        return FALSE;
    }

    return TRUE;
}

/**
 * @brief Adds, removes or refreshes a device as a WMI data provider.
 *
 * The registration information itself is requested with IRP_MN_REGINFO_EX or IRP_MN_REGINFO.
 */
NTSTATUS
NTAPI
IoWMIRegistrationControl(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ ULONG Action)
{
    NTSTATUS Status;

    if (!WmipServiceDeviceObject)
        return STATUS_UNSUCCESSFUL;

    /* Callback providers are not supported yet and stay unregistered */
    if (Action & WMIREG_FLAG_CALLBACK)
    {
        DPRINT1("WMI: callback registration for %p ignored\n", DeviceObject);
        return STATUS_SUCCESS;
    }

    /* The trace bits only concern event tracing */
    Action &= ~(WMIREG_FLAG_TRACE_PROVIDER | WMIREG_FLAG_TRACE_NOTIFY_MASK);

    /* The WMI device itself has no data blocks to register */
    if (DeviceObject == WmipServiceDeviceObject)
        return STATUS_SUCCESS;

    switch (Action)
    {
        case WMIREG_ACTION_REGISTER:
            return WmipRegisterProvider(DeviceObject);

        case WMIREG_ACTION_DEREGISTER:
            return WmipDeregisterProvider(DeviceObject);

        case WMIREG_ACTION_REREGISTER:
            Status = WmipDeregisterProvider(DeviceObject);
            if (!NT_SUCCESS(Status))
                return Status;
            return WmipRegisterProvider(DeviceObject);

        case WMIREG_ACTION_UPDATE_GUIDS:
            return WmipUpdateProvider(DeviceObject);

        case WMIREG_ACTION_BLOCK_IRPS:
            return WmipBlockProvider(DeviceObject);

        default:
            return STATUS_INVALID_PARAMETER;
    }
}

/*
 * @unimplemented
 */
NTSTATUS
NTAPI
IoWMIAllocateInstanceIds(IN GUID *Guid,
                         IN ULONG InstanceCount,
                         OUT ULONG *FirstInstanceId)
{
    UNIMPLEMENTED;
    return STATUS_NOT_IMPLEMENTED;
}

/*
 * @unimplemented
 */
NTSTATUS
NTAPI
IoWMISuggestInstanceName(IN PDEVICE_OBJECT PhysicalDeviceObject OPTIONAL,
                         IN PUNICODE_STRING SymbolicLinkName OPTIONAL,
                         IN BOOLEAN CombineNames,
                         OUT PUNICODE_STRING SuggestedInstanceName)
{
    UNIMPLEMENTED;
    return STATUS_NOT_IMPLEMENTED;
}

/*
 * @unimplemented
 */
NTSTATUS
NTAPI
IoWMIWriteEvent(_Inout_ PVOID WnodeEventItem)
{
    PWNODE_HEADER Header = WnodeEventItem;

    if(!Header)
    {
        DPRINT1("Got NULL Item!\n");
        return STATUS_INVALID_PARAMETER;
    }

    DPRINT1("IoWMIWriteEvent() called for WnodeEventItem %p (Flags = 0x%08lx), returning success\n",
            WnodeEventItem, Header->Flags);

    if (Header->Flags & WNODE_FLAG_TRACED_GUID)
    {
        // Never free WnodeEventItem in this case.
        DPRINT("IoWMIWriteEvent(): Flags has WNODE_FLAG_TRACED_GUID\n");
        return STATUS_SUCCESS;
    }

    /* Free the buffer if we are returning success */
    ExFreePool(WnodeEventItem);

    return STATUS_SUCCESS;
}

/*
 * @unimplemented
 */
NTSTATUS
NTAPI
IoWMIOpenBlock(
    _In_ LPCGUID DataBlockGuid,
    _In_ ULONG DesiredAccess,
    _Out_ PVOID *DataBlockObject)
{
    HANDLE GuidObjectHandle;
    NTSTATUS Status;

    /* Open the GIOD object */
    Status = WmipOpenGuidObject(DataBlockGuid,
                                DesiredAccess,
                                KernelMode,
                                &GuidObjectHandle,
                                DataBlockObject);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("WmipOpenGuidObject failed: 0x%lx\n", Status);
        return Status;
    }


    return STATUS_SUCCESS;
}

/* Same contract as IoWMIQueryAllData, for blocks served by registered providers. */
static
NTSTATUS
WmipKernelQueryAllData(
    _In_ PWMIP_GUID_OBJECT GuidObject,
    _Inout_ PULONG InOutBufferSize,
    _Out_writes_bytes_opt_(*InOutBufferSize) PVOID OutBuffer)
{
    ULONG64 Local[WMIP_MIN_ALL_DATA_SIZE / sizeof(ULONG64)];
    PWNODE_ALL_DATA Wnode = OutBuffer;
    ULONG BufferSize = *InOutBufferSize;
    ULONG ReturnSize;
    NTSTATUS Status;

    /* Without room for a header the caller only learns the size */
    if (!Wnode || (BufferSize < WMIP_MIN_ALL_DATA_SIZE))
    {
        Wnode = (PWNODE_ALL_DATA)Local;
        BufferSize = sizeof(Local);
    }

    Wnode->WnodeHeader.BufferSize = sizeof(WNODE_HEADER);
    Wnode->WnodeHeader.Flags = WNODE_FLAG_ALL_DATA;
    Wnode->WnodeHeader.Linkage = 0;
    Wnode->WnodeHeader.KernelHandle = NULL;

    Status = WmipQueryAllData(GuidObject, Wnode, BufferSize, &ReturnSize);
    if (!NT_SUCCESS(Status))
        return Status;

    if (Wnode->WnodeHeader.Flags & WNODE_FLAG_TOO_SMALL)
    {
        *InOutBufferSize = ((PWNODE_TOO_SMALL)Wnode)->SizeNeeded;
        return STATUS_BUFFER_TOO_SMALL;
    }

    *InOutBufferSize = ReturnSize;
    return (Wnode == OutBuffer) ? STATUS_SUCCESS : STATUS_BUFFER_TOO_SMALL;
}

/*
 * @implemented
 */
NTSTATUS
NTAPI
IoWMIQueryAllData(
    IN PVOID DataBlockObject,
    IN OUT ULONG *InOutBufferSize,
    OUT PVOID OutBuffer)
{
    PWMIP_GUID_OBJECT GuidObject;
    NTSTATUS Status;


    Status = ObReferenceObjectByPointer(DataBlockObject,
                                        WMIGUID_QUERY,
                                        WmipGuidObjectType,
                                        KernelMode);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    GuidObject = DataBlockObject;

    /* Huge HACK! */
    if (IsEqualGUID(&GuidObject->Guid, &MSSmBios_RawSMBiosTables_GUID))
    {
        Status = WmipQueryRawSMBiosTables(InOutBufferSize, OutBuffer);
    }
    else
    {
        Status = WmipKernelQueryAllData(GuidObject, InOutBufferSize, OutBuffer);
    }

    ObDereferenceObject(DataBlockObject);

    return Status;
}

/*
 * @unimplemented
 */
NTSTATUS
NTAPI
IoWMIQueryAllDataMultiple(IN PVOID *DataBlockObjectList,
                          IN ULONG ObjectCount,
                          IN OUT ULONG *InOutBufferSize,
                          OUT PVOID OutBuffer)
{
    /* No data provider ever registers a block here, so none is found */
    return STATUS_WMI_GUID_NOT_FOUND;
}

/**
 * @brief Queries one named instance of a data block for a kernel mode caller.
 */
NTSTATUS
NTAPI
IoWMIQuerySingleInstance(
    _In_ PVOID DataBlockObject,
    _In_ PUNICODE_STRING InstanceName,
    _Inout_ ULONG *InOutBufferSize,
    _Out_writes_bytes_opt_(*InOutBufferSize) PVOID OutBuffer)
{
    PWNODE_SINGLE_INSTANCE Wnode;
    ULONG WnodeSize, BufferSize, ReturnSize;
    NTSTATUS Status;

    Status = ObReferenceObjectByPointer(DataBlockObject,
                                        WMIGUID_QUERY,
                                        WmipGuidObjectType,
                                        KernelMode);
    if (!NT_SUCCESS(Status))
        return Status;

    /* Header and counted name, the data starts at the next eight byte boundary */
    WnodeSize = (ULONG)ALIGN_UP_BY(FIELD_OFFSET(WNODE_SINGLE_INSTANCE, VariableData) +
                                   sizeof(USHORT) + InstanceName->Length, 8);

    Wnode = OutBuffer;
    BufferSize = *InOutBufferSize;
    if (!Wnode || (BufferSize < WnodeSize))
    {
        Wnode = ExAllocatePoolWithTag(NonPagedPool, WnodeSize, TAG_WMI_QUERY);
        BufferSize = WnodeSize;
        if (!Wnode)
        {
            ObDereferenceObject(DataBlockObject);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    }

    RtlZeroMemory(Wnode, FIELD_OFFSET(WNODE_SINGLE_INSTANCE, VariableData));
    Wnode->WnodeHeader.BufferSize = WnodeSize;
    Wnode->WnodeHeader.Flags = WNODE_FLAG_SINGLE_INSTANCE;
    Wnode->OffsetInstanceName = FIELD_OFFSET(WNODE_SINGLE_INSTANCE, VariableData);
    Wnode->DataBlockOffset = WnodeSize;
    *(PUSHORT)Wnode->VariableData = InstanceName->Length;
    RtlCopyMemory(Wnode->VariableData + sizeof(USHORT), InstanceName->Buffer, InstanceName->Length);

    Status = WmipQuerySingleInstance(DataBlockObject, Wnode, BufferSize, &ReturnSize);
    if (NT_SUCCESS(Status))
    {
        if (Wnode->WnodeHeader.Flags & WNODE_FLAG_TOO_SMALL)
        {
            *InOutBufferSize = ((PWNODE_TOO_SMALL)Wnode)->SizeNeeded;
            Status = STATUS_BUFFER_TOO_SMALL;
        }
        else
        {
            *InOutBufferSize = ReturnSize;
            if (Wnode != OutBuffer)
                Status = STATUS_BUFFER_TOO_SMALL;
        }
    }

    if (Wnode != OutBuffer)
        ExFreePoolWithTag(Wnode, TAG_WMI_QUERY);

    ObDereferenceObject(DataBlockObject);
    return Status;
}

/*
 * @unimplemented
 */
NTSTATUS
NTAPI
IoWMIQuerySingleInstanceMultiple(IN PVOID *DataBlockObjectList,
                                 IN PUNICODE_STRING InstanceNames,
                                 IN ULONG ObjectCount,
                                 IN OUT ULONG *InOutBufferSize,
                                 OUT PVOID OutBuffer)
{
    /* No data provider ever registers a block here, so none is found */
    return STATUS_WMI_GUID_NOT_FOUND;
}

/*
 * @unimplemented
 */
NTSTATUS
NTAPI
IoWMISetSingleInstance(IN PVOID DataBlockObject,
                       IN PUNICODE_STRING InstanceName,
                       IN ULONG Version,
                       IN ULONG ValueBufferSize,
                       IN PVOID ValueBuffer)
{
    /* No data provider ever registers a block here, so none is found */
    return STATUS_WMI_GUID_NOT_FOUND;
}

/*
 * @unimplemented
 */
NTSTATUS
NTAPI
IoWMISetSingleItem(IN PVOID DataBlockObject,
                   IN PUNICODE_STRING InstanceName,
                   IN ULONG DataItemId,
                   IN ULONG Version,
                   IN ULONG ValueBufferSize,
                   IN PVOID ValueBuffer)
{
    /* No data provider ever registers a block here, so none is found */
    return STATUS_WMI_GUID_NOT_FOUND;
}

/*
 * @unimplemented
 */
NTSTATUS
NTAPI
IoWMIExecuteMethod(IN PVOID DataBlockObject,
                   IN PUNICODE_STRING InstanceName,
                   IN ULONG MethodId,
                   IN ULONG InBufferSize,
                   IN OUT PULONG OutBufferSize,
                   IN OUT PUCHAR InOutBuffer)
{
    /* No data provider ever registers a block here, so none is found */
    return STATUS_WMI_GUID_NOT_FOUND;
}

static KSPIN_LOCK WmipNotificationLock;

/**
 * @brief Records the routine that receives the events of an opened WMI data block.
 */
NTSTATUS
NTAPI
IoWMISetNotificationCallback(
    _Inout_ PVOID Object,
    _In_ WMI_NOTIFICATION_CALLBACK Callback,
    _In_opt_ PVOID Context)
{
    PWMIP_GUID_OBJECT GuidObject = Object;
    KIRQL OldIrql;

    KeAcquireSpinLock(&WmipNotificationLock, &OldIrql);
    GuidObject->NotificationCallback = Callback;
    GuidObject->NotificationContext = Context;
    KeReleaseSpinLock(&WmipNotificationLock, OldIrql);

    return STATUS_SUCCESS;
}

/*
 * @unimplemented
 */
NTSTATUS
NTAPI
IoWMIHandleToInstanceName(IN PVOID DataBlockObject,
                          IN HANDLE FileHandle,
                          OUT PUNICODE_STRING InstanceName)
{
    UNIMPLEMENTED;
    return STATUS_NOT_IMPLEMENTED;
}

/*
 * @unimplemented
 */
NTSTATUS
NTAPI
IoWMIDeviceObjectToInstanceName(IN PVOID DataBlockObject,
                                IN PDEVICE_OBJECT DeviceObject,
                                OUT PUNICODE_STRING InstanceName)
{
    UNIMPLEMENTED;
    return STATUS_NOT_IMPLEMENTED;
}

/*
 * @unimplemented
 */
NTSTATUS
NTAPI
WmiQueryTraceInformation(IN TRACE_INFORMATION_CLASS TraceInformationClass,
                         OUT PVOID TraceInformation,
                         IN ULONG TraceInformationLength,
                         OUT PULONG RequiredLength OPTIONAL,
                         IN PVOID Buffer OPTIONAL)
{
    UNIMPLEMENTED;
    return STATUS_NOT_IMPLEMENTED;
}

/*
 * @unimplemented
 */
NTSTATUS
__cdecl
WmiTraceMessage(IN TRACEHANDLE LoggerHandle,
                IN ULONG MessageFlags,
                IN LPGUID MessageGuid,
                IN USHORT MessageNumber,
                IN ...)
{
    UNIMPLEMENTED;
    return STATUS_NOT_IMPLEMENTED;
}

/*
 * @unimplemented
 */
NTSTATUS
NTAPI
WmiTraceMessageVa(IN TRACEHANDLE LoggerHandle,
                  IN ULONG MessageFlags,
                  IN LPGUID MessageGuid,
                  IN USHORT MessageNumber,
                  IN va_list MessageArgList)
{
    UNIMPLEMENTED;
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
NTAPI
WmiFlushTrace(IN OUT PWMI_LOGGER_INFORMATION LoggerInfo)
{
    UNIMPLEMENTED;
    return STATUS_NOT_IMPLEMENTED;
}

LONG64
FASTCALL
WmiGetClock(IN WMI_CLOCK_TYPE ClockType,
            IN PVOID Context)
{
    UNIMPLEMENTED;
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
NTAPI
WmiQueryTrace(IN OUT PWMI_LOGGER_INFORMATION LoggerInfo)
{
    UNIMPLEMENTED;
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
NTAPI
WmiStartTrace(IN OUT PWMI_LOGGER_INFORMATION LoggerInfo)
{
    UNIMPLEMENTED;
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
NTAPI
WmiStopTrace(IN PWMI_LOGGER_INFORMATION LoggerInfo)
{
    UNIMPLEMENTED;
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
FASTCALL
WmiTraceFastEvent(IN PWNODE_HEADER Wnode)
{
    UNIMPLEMENTED;
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
NTAPI
WmiUpdateTrace(IN OUT PWMI_LOGGER_INFORMATION LoggerInfo)
{
    UNIMPLEMENTED;
    return STATUS_NOT_IMPLEMENTED;
}

/*
 * @unimplemented
 */
NTSTATUS
NTAPI
NtTraceEvent(IN ULONG TraceHandle,
             IN ULONG Flags,
             IN ULONG TraceHeaderLength,
             IN struct _EVENT_TRACE_HEADER* TraceHeader)
{
    UNIMPLEMENTED;
    return STATUS_NOT_IMPLEMENTED;
}

/*Eof*/
