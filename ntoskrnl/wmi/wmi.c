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

/*
 * @unimplemented
 */
NTSTATUS
NTAPI
IoWMIRegistrationControl(IN PDEVICE_OBJECT DeviceObject,
                         IN ULONG Action)
{
    DPRINT("IoWMIRegistrationControl() called for DO %p, requesting %lu action, returning success\n",
        DeviceObject, Action);

    return STATUS_SUCCESS;
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

/*
 * @unimplemented
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
        Status = STATUS_NOT_SUPPORTED;
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
    UNIMPLEMENTED;
    return STATUS_NOT_IMPLEMENTED;
}

/*
 * @unimplemented
 */
NTSTATUS
NTAPI
IoWMIQuerySingleInstance(IN PVOID DataBlockObject,
                         IN PUNICODE_STRING InstanceName,
                         IN OUT ULONG *InOutBufferSize,
                         OUT PVOID OutBuffer)
{
    UNIMPLEMENTED;
    return STATUS_NOT_IMPLEMENTED;
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
    UNIMPLEMENTED;
    return STATUS_NOT_IMPLEMENTED;
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
    UNIMPLEMENTED;
    return STATUS_NOT_IMPLEMENTED;
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
    UNIMPLEMENTED;
    return STATUS_NOT_IMPLEMENTED;
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
    UNIMPLEMENTED;
    return STATUS_NOT_IMPLEMENTED;
}

/*
 * @unimplemented
 */
NTSTATUS
NTAPI
IoWMISetNotificationCallback(IN PVOID Object,
                             IN WMI_NOTIFICATION_CALLBACK Callback,
                             IN PVOID Context)
{
    UNIMPLEMENTED;
    return STATUS_NOT_IMPLEMENTED;
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
