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
#define TAG_WMI_INSTANCE 'IimW'

/* Next free instance identifier of one GUID, see IoWMIAllocateInstanceIds */
typedef struct _WMIP_INSTANCE_IDS
{
    LIST_ENTRY Link;
    GUID Guid;
    ULONG Next;
} WMIP_INSTANCE_IDS, *PWMIP_INSTANCE_IDS;

static LIST_ENTRY WmipInstanceIdListHead;
static FAST_MUTEX WmipInstanceIdLock;

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
    WmipInitializeConsumers();
    InitializeListHead(&WmipInstanceIdListHead);
    ExInitializeFastMutex(&WmipInstanceIdLock);

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

/**
 * @brief Hands out instance identifiers that stay unique per GUID until reboot.
 */
NTSTATUS
NTAPI
IoWMIAllocateInstanceIds(
    _In_ GUID *Guid,
    _In_ ULONG InstanceCount,
    _Out_ ULONG *FirstInstanceId)
{
    PWMIP_INSTANCE_IDS Ids = NULL;
    PLIST_ENTRY Entry;

    PAGED_CODE();

    if (!WmipServiceDeviceObject)
        return STATUS_UNSUCCESSFUL;

    ExAcquireFastMutex(&WmipInstanceIdLock);

    for (Entry = WmipInstanceIdListHead.Flink; Entry != &WmipInstanceIdListHead; Entry = Entry->Flink)
    {
        if (IsEqualGUID(&CONTAINING_RECORD(Entry, WMIP_INSTANCE_IDS, Link)->Guid, Guid))
        {
            Ids = CONTAINING_RECORD(Entry, WMIP_INSTANCE_IDS, Link);
            break;
        }
    }

    if (Ids)
    {
        *FirstInstanceId = Ids->Next;
        Ids->Next += InstanceCount;
    }
    else
    {
        Ids = ExAllocatePoolWithTag(PagedPool, sizeof(*Ids), TAG_WMI_INSTANCE);
        if (!Ids)
        {
            ExReleaseFastMutex(&WmipInstanceIdLock);
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        Ids->Guid = *Guid;
        Ids->Next = InstanceCount;
        InsertTailList(&WmipInstanceIdListHead, &Ids->Link);
        *FirstInstanceId = 0;
    }

    ExReleaseFastMutex(&WmipInstanceIdLock);
    return STATUS_SUCCESS;
}

/* Reads the default value of a device interface key, the interface reference string. */
static
NTSTATUS
WmipQueryInterfaceReference(
    _In_ PUNICODE_STRING SymbolicLinkName,
    _Outptr_ PKEY_VALUE_FULL_INFORMATION *OutValue,
    _Out_ PUNICODE_STRING Reference)
{
    UNICODE_STRING DefaultName = RTL_CONSTANT_STRING(L"");
    PKEY_VALUE_FULL_INFORMATION Value = NULL;
    ULONG Size = 0;
    NTSTATUS Status;
    HANDLE Key;

    *OutValue = NULL;
    RtlZeroMemory(Reference, sizeof(*Reference));

    Status = IoOpenDeviceInterfaceRegistryKey(SymbolicLinkName, KEY_ALL_ACCESS, &Key);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = ZwQueryValueKey(Key, &DefaultName, KeyValueFullInformation, NULL, 0, &Size);
    if ((Status == STATUS_BUFFER_OVERFLOW) || (Status == STATUS_BUFFER_TOO_SMALL))
    {
        Value = ExAllocatePoolWithTag(PagedPool, Size, TAG_WMI_INSTANCE);
        if (!Value)
            Status = STATUS_INSUFFICIENT_RESOURCES;
        else
            Status = ZwQueryValueKey(Key, &DefaultName, KeyValueFullInformation, Value, Size, &Size);
    }

    ZwClose(Key);

    if (!NT_SUCCESS(Status))
    {
        if (Value)
            ExFreePoolWithTag(Value, TAG_WMI_INSTANCE);
        return Status;
    }

    /* The data is a string, trailing terminators are not part of the name */
    Reference->Buffer = (PWCH)((PUCHAR)Value + Value->DataOffset);
    Size = min(Value->DataLength, MAXUSHORT) & ~(sizeof(WCHAR) - 1);
    while ((Size >= sizeof(WCHAR)) && (Reference->Buffer[Size / sizeof(WCHAR) - 1] == UNICODE_NULL))
        Size -= sizeof(WCHAR);
    Reference->Length = Reference->MaximumLength = (USHORT)Size;

    *OutValue = Value;
    return STATUS_SUCCESS;
}

/**
 * @brief Suggests an instance name from the device description, the interface reference or both.
 *
 * The caller frees SuggestedInstanceName->Buffer with ExFreePool.
 */
NTSTATUS
NTAPI
IoWMISuggestInstanceName(
    _In_opt_ PDEVICE_OBJECT PhysicalDeviceObject,
    _In_opt_ PUNICODE_STRING SymbolicLinkName,
    _In_ BOOLEAN CombineNames,
    _Out_ PUNICODE_STRING SuggestedInstanceName)
{
    NTSTATUS Status = STATUS_INVALID_PARAMETER_MIX;
    PKEY_VALUE_FULL_INFORMATION Value;
    UNICODE_STRING Reference;
    PWSTR Description = NULL;
    ULONG DescriptionSize = 0;
    ULONG Size;

    PAGED_CODE();

    if (!WmipServiceDeviceObject)
        return STATUS_UNSUCCESSFUL;

    RtlZeroMemory(SuggestedInstanceName, sizeof(*SuggestedInstanceName));

    if (PhysicalDeviceObject)
    {
        Status = IoGetDeviceProperty(PhysicalDeviceObject,
                                     DevicePropertyDeviceDescription,
                                     0,
                                     NULL,
                                     &DescriptionSize);
        if (Status != STATUS_BUFFER_TOO_SMALL)
            return NT_SUCCESS(Status) ? STATUS_UNSUCCESSFUL : Status;

        if (DescriptionSize > MAXUSHORT)
            return STATUS_INVALID_PARAMETER;

        Description = ExAllocatePoolWithTag(PagedPool, DescriptionSize, TAG_WMI_INSTANCE);
        if (!Description)
            return STATUS_INSUFFICIENT_RESOURCES;

        Status = IoGetDeviceProperty(PhysicalDeviceObject,
                                     DevicePropertyDeviceDescription,
                                     DescriptionSize,
                                     Description,
                                     &DescriptionSize);
        if (!NT_SUCCESS(Status) || (DescriptionSize < sizeof(WCHAR)))
        {
            ExFreePoolWithTag(Description, TAG_WMI_INSTANCE);
            return NT_SUCCESS(Status) ? STATUS_UNSUCCESSFUL : Status;
        }
    }

    /* The description alone is handed over as it came */
    if (!SymbolicLinkName)
    {
        if (Description)
        {
            SuggestedInstanceName->Buffer = Description;
            SuggestedInstanceName->MaximumLength = (USHORT)DescriptionSize;
            SuggestedInstanceName->Length = (USHORT)(DescriptionSize - sizeof(WCHAR));
        }
        return Status;
    }

    Status = WmipQueryInterfaceReference(SymbolicLinkName, &Value, &Reference);
    if (NT_SUCCESS(Status))
    {
        Size = Reference.Length + sizeof(UNICODE_NULL);
        if (CombineNames)
            Size += DescriptionSize + sizeof(WCHAR);

        SuggestedInstanceName->Buffer = (Size <= MAXUSHORT) ?
                                        ExAllocatePoolWithTag(PagedPool, Size, TAG_WMI_INSTANCE) : NULL;
        if (SuggestedInstanceName->Buffer)
        {
            SuggestedInstanceName->MaximumLength = (USHORT)Size;
            if (CombineNames)
            {
                if (Description)
                    RtlAppendUnicodeToString(SuggestedInstanceName, Description);
                RtlAppendUnicodeToString(SuggestedInstanceName, L"_");
            }
            RtlAppendUnicodeStringToString(SuggestedInstanceName, &Reference);
            SuggestedInstanceName->Buffer[SuggestedInstanceName->Length / sizeof(WCHAR)] = UNICODE_NULL;
        }
        else
        {
            Status = STATUS_INSUFFICIENT_RESOURCES;
        }

        ExFreePoolWithTag(Value, TAG_WMI_INSTANCE);
    }

    if (Description)
        ExFreePoolWithTag(Description, TAG_WMI_INSTANCE);

    return Status;
}

/**
 * @brief Opens a data block for a driver, the returned object is released with ObDereferenceObject.
 *
 * WMIGUID_NOTIFICATION makes an event consumer, WRITE_DAC a plain open, anything else a query open.
 */
NTSTATUS
NTAPI
IoWMIOpenBlock(
    _In_ LPCGUID DataBlockGuid,
    _In_ ULONG DesiredAccess,
    _Out_ PVOID *DataBlockObject)
{
    PWMIP_GUID_OBJECT GuidObject;
    HANDLE GuidObjectHandle;
    ULONG OpenType;
    NTSTATUS Status;

    PAGED_CODE();

    *DataBlockObject = NULL;

    if (DesiredAccess & WMIGUID_NOTIFICATION)
        OpenType = WMIP_OPEN_EVENTS;
    else if (DesiredAccess & WRITE_DAC)
        OpenType = WMIP_OPEN_PLAIN;
    else
        OpenType = WMIP_OPEN_QUERYSET;

    if ((OpenType == WMIP_OPEN_QUERYSET) && !WmipIsQuerySetGuid(DataBlockGuid))
        return STATUS_WMI_GUID_NOT_FOUND;

    Status = WmipOpenGuidObject(DataBlockGuid,
                                DesiredAccess,
                                KernelMode,
                                &GuidObjectHandle,
                                (PVOID *)&GuidObject);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("WmipOpenGuidObject failed: 0x%lx\n", Status);
        return Status;
    }

    /* Drivers hold the object itself, not a handle */
    ObCloseHandle(GuidObjectHandle, KernelMode);

    Status = WmipAttachConsumer(GuidObject, OpenType, KernelMode);
    if (!NT_SUCCESS(Status))
    {
        ObDereferenceObject(GuidObject);
        return Status;
    }

    *DataBlockObject = GuidObject;
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

/**
 * @brief Allocates a WNODE for one named instance with room for DataSize bytes behind the name.
 *
 * The header up to VariableData is zeroed, the name is in place and the data starts eight byte aligned.
 */
static
NTSTATUS
WmipAllocateInstanceWnode(
    _In_ ULONG HeaderSize,
    _In_ PCUNICODE_STRING InstanceName,
    _In_ ULONG DataSize,
    _Out_ PULONG DataOffset,
    _Out_ PULONG WnodeSize,
    _Outptr_ PWNODE_SINGLE_INSTANCE *OutWnode)
{
    PWNODE_SINGLE_INSTANCE Wnode;
    ULONGLONG Offset, Size;

    *OutWnode = NULL;

    if (!InstanceName || !InstanceName->Buffer)
        return STATUS_INVALID_PARAMETER;

    Offset = ALIGN_UP_BY((ULONGLONG)HeaderSize + sizeof(USHORT) + InstanceName->Length, 8);
    Size = Offset + DataSize;
    if (Size > MAXULONG)
        return STATUS_INVALID_PARAMETER;

    Wnode = ExAllocatePoolWithTag(NonPagedPool, (SIZE_T)Size, TAG_WMI_QUERY);
    if (!Wnode)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlZeroMemory(Wnode, HeaderSize);
    Wnode->OffsetInstanceName = HeaderSize;
    *(PUSHORT)((PUCHAR)Wnode + HeaderSize) = InstanceName->Length;
    RtlCopyMemory((PUCHAR)Wnode + HeaderSize + sizeof(USHORT), InstanceName->Buffer, InstanceName->Length);

    *DataOffset = (ULONG)Offset;
    *WnodeSize = (ULONG)Size;
    *OutWnode = Wnode;
    return STATUS_SUCCESS;
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

    Status = WmipSendSingleInstanceRequest(DataBlockObject,
                                           IRP_MN_QUERY_SINGLE_INSTANCE,
                                           Wnode,
                                           BufferSize,
                                           &ReturnSize);
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

/**
 * @brief Runs one query per data block and chains the results through WNODE_HEADER.Linkage.
 *
 * With InstanceNames every block is asked for one instance, otherwise for all its data.
 * Results that no longer fit only add to the size reported with STATUS_BUFFER_TOO_SMALL.
 */
static
NTSTATUS
WmipQueryMultiple(
    _In_reads_(ObjectCount) PVOID *DataBlockObjectList,
    _In_reads_opt_(ObjectCount) PUNICODE_STRING InstanceNames,
    _In_ ULONG ObjectCount,
    _Inout_ ULONG *InOutBufferSize,
    _Out_writes_bytes_opt_(*InOutBufferSize) PVOID OutBuffer)
{
    PWNODE_HEADER Previous = NULL, Last;
    PUCHAR Cursor = OutBuffer;
    BOOLEAN Found = FALSE, TooSmall = FALSE;
    ULONGLONG Needed = 0, Aligned;
    ULONG Left, Size, Index;
    NTSTATUS Status;
    PVOID Target;

    PAGED_CODE();

    Left = OutBuffer ? *InOutBufferSize : 0;

    for (Index = 0; Index < ObjectCount; Index++)
    {
        Target = (TooSmall || !Left) ? NULL : Cursor;
        Size = Target ? Left : 0;

        if (InstanceNames)
            Status = IoWMIQuerySingleInstance(DataBlockObjectList[Index], &InstanceNames[Index], &Size, Target);
        else
            Status = IoWMIQueryAllData(DataBlockObjectList[Index], &Size, Target);

        if ((Status != STATUS_BUFFER_TOO_SMALL) && !NT_SUCCESS(Status))
            continue;

        Found = TRUE;
        Aligned = ALIGN_UP_BY((ULONGLONG)Size, 8);
        Needed += Aligned;

        /* From here on the remaining blocks only report their size */
        if ((Status == STATUS_BUFFER_TOO_SMALL) || !Target)
        {
            TooSmall = TRUE;
            continue;
        }

        if (Previous)
            Previous->Linkage = (ULONG)(Cursor - (PUCHAR)Previous);

        /* An all data result can itself be a chain, the next one hangs off its end */
        Last = (PWNODE_HEADER)Cursor;
        while (Last->Linkage)
            Last = (PWNODE_HEADER)((PUCHAR)Last + Last->Linkage);
        Previous = Last;

        if (Aligned > Left)
            Aligned = Left;
        Cursor += (ULONG)Aligned;
        Left -= (ULONG)Aligned;
    }

    if (!Found)
        return STATUS_WMI_GUID_NOT_FOUND;

    *InOutBufferSize = (Needed > MAXULONG) ? MAXULONG : (ULONG)Needed;
    return TooSmall ? STATUS_BUFFER_TOO_SMALL : STATUS_SUCCESS;
}

/**
 * @brief Queries one named instance of each of several data blocks at once.
 */
NTSTATUS
NTAPI
IoWMIQuerySingleInstanceMultiple(
    _In_reads_(ObjectCount) PVOID *DataBlockObjectList,
    _In_reads_(ObjectCount) PUNICODE_STRING InstanceNames,
    _In_ ULONG ObjectCount,
    _Inout_ ULONG *InOutBufferSize,
    _Out_writes_bytes_opt_(*InOutBufferSize) PVOID OutBuffer)
{
    if (!InstanceNames)
        return STATUS_INVALID_PARAMETER;

    return WmipQueryMultiple(DataBlockObjectList, InstanceNames, ObjectCount, InOutBufferSize, OutBuffer);
}

/**
 * @brief Queries all data of several data blocks at once.
 */
NTSTATUS
NTAPI
IoWMIQueryAllDataMultiple(
    _In_reads_(ObjectCount) PVOID *DataBlockObjectList,
    _In_ ULONG ObjectCount,
    _Inout_ ULONG *InOutBufferSize,
    _Out_writes_bytes_opt_(*InOutBufferSize) PVOID OutBuffer)
{
    return WmipQueryMultiple(DataBlockObjectList, NULL, ObjectCount, InOutBufferSize, OutBuffer);
}

/**
 * @brief Replaces the data of one named instance for a kernel mode caller.
 */
NTSTATUS
NTAPI
IoWMISetSingleInstance(
    _In_ PVOID DataBlockObject,
    _In_ PUNICODE_STRING InstanceName,
    _In_ ULONG Version,
    _In_ ULONG ValueBufferSize,
    _In_reads_bytes_(ValueBufferSize) PVOID ValueBuffer)
{
    PWNODE_SINGLE_INSTANCE Wnode;
    ULONG DataOffset, WnodeSize, ReturnSize;
    NTSTATUS Status;

    PAGED_CODE();

    Status = ObReferenceObjectByPointer(DataBlockObject, WMIGUID_SET, WmipGuidObjectType, KernelMode);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = WmipAllocateInstanceWnode(FIELD_OFFSET(WNODE_SINGLE_INSTANCE, VariableData),
                                       InstanceName,
                                       ValueBufferSize,
                                       &DataOffset,
                                       &WnodeSize,
                                       &Wnode);
    if (NT_SUCCESS(Status))
    {
        Wnode->WnodeHeader.BufferSize = WnodeSize;
        Wnode->WnodeHeader.Version = Version;
        Wnode->WnodeHeader.Flags = WNODE_FLAG_SINGLE_INSTANCE;
        Wnode->DataBlockOffset = DataOffset;
        Wnode->SizeDataBlock = ValueBufferSize;
        RtlCopyMemory((PUCHAR)Wnode + DataOffset, ValueBuffer, ValueBufferSize);

        Status = WmipSendSingleInstanceRequest(DataBlockObject,
                                               IRP_MN_CHANGE_SINGLE_INSTANCE,
                                               Wnode,
                                               WnodeSize,
                                               &ReturnSize);
        ExFreePoolWithTag(Wnode, TAG_WMI_QUERY);
    }

    ObDereferenceObject(DataBlockObject);
    return Status;
}

/**
 * @brief Replaces one data item of one named instance for a kernel mode caller.
 */
NTSTATUS
NTAPI
IoWMISetSingleItem(
    _In_ PVOID DataBlockObject,
    _In_ PUNICODE_STRING InstanceName,
    _In_ ULONG DataItemId,
    _In_ ULONG Version,
    _In_ ULONG ValueBufferSize,
    _In_reads_bytes_(ValueBufferSize) PVOID ValueBuffer)
{
    PWNODE_SINGLE_ITEM Wnode;
    ULONG DataOffset, WnodeSize, ReturnSize;
    NTSTATUS Status;

    PAGED_CODE();

    Status = ObReferenceObjectByPointer(DataBlockObject, WMIGUID_SET, WmipGuidObjectType, KernelMode);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = WmipAllocateInstanceWnode(FIELD_OFFSET(WNODE_SINGLE_ITEM, VariableData),
                                       InstanceName,
                                       ValueBufferSize,
                                       &DataOffset,
                                       &WnodeSize,
                                       (PWNODE_SINGLE_INSTANCE *)&Wnode);
    if (NT_SUCCESS(Status))
    {
        Wnode->WnodeHeader.BufferSize = WnodeSize;
        Wnode->WnodeHeader.Version = Version;
        Wnode->WnodeHeader.Flags = WNODE_FLAG_SINGLE_ITEM;
        Wnode->ItemId = DataItemId;
        Wnode->DataBlockOffset = DataOffset;
        Wnode->SizeDataItem = ValueBufferSize;
        RtlCopyMemory((PUCHAR)Wnode + DataOffset, ValueBuffer, ValueBufferSize);

        Status = WmipSendSingleInstanceRequest(DataBlockObject,
                                               IRP_MN_CHANGE_SINGLE_ITEM,
                                               (PWNODE_SINGLE_INSTANCE)Wnode,
                                               WnodeSize,
                                               &ReturnSize);
        ExFreePoolWithTag(Wnode, TAG_WMI_QUERY);
    }

    ObDereferenceObject(DataBlockObject);
    return Status;
}

/**
 * @brief Runs a method of one named instance, the output replaces the input in InOutBuffer.
 *
 * An output larger than *OutBufferSize fails with STATUS_BUFFER_TOO_SMALL and the size needed.
 */
NTSTATUS
NTAPI
IoWMIExecuteMethod(
    _In_ PVOID DataBlockObject,
    _In_ PUNICODE_STRING InstanceName,
    _In_ ULONG MethodId,
    _In_ ULONG InBufferSize,
    _Inout_ PULONG OutBufferSize,
    _Inout_updates_bytes_to_opt_(*OutBufferSize, InBufferSize) PUCHAR InOutBuffer)
{
    PWNODE_METHOD_ITEM Wnode;
    ULONG DataOffset, WnodeSize, ReturnSize, Needed;
    NTSTATUS Status;

    PAGED_CODE();

    Status = ObReferenceObjectByPointer(DataBlockObject, WMIGUID_EXECUTE, WmipGuidObjectType, KernelMode);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = WmipAllocateInstanceWnode(FIELD_OFFSET(WNODE_METHOD_ITEM, VariableData),
                                       InstanceName,
                                       max(InBufferSize, *OutBufferSize),
                                       &DataOffset,
                                       &WnodeSize,
                                       (PWNODE_SINGLE_INSTANCE *)&Wnode);
    if (!NT_SUCCESS(Status))
    {
        ObDereferenceObject(DataBlockObject);
        return Status;
    }

    Wnode->WnodeHeader.BufferSize = WnodeSize;
    Wnode->WnodeHeader.Flags = WNODE_FLAG_METHOD_ITEM;
    Wnode->MethodId = MethodId;
    Wnode->DataBlockOffset = DataOffset;
    Wnode->SizeDataBlock = InBufferSize;
    if (InBufferSize)
        RtlCopyMemory((PUCHAR)Wnode + DataOffset, InOutBuffer, InBufferSize);

    Status = WmipSendSingleInstanceRequest(DataBlockObject,
                                           IRP_MN_EXECUTE_METHOD,
                                           (PWNODE_SINGLE_INSTANCE)Wnode,
                                           WnodeSize,
                                           &ReturnSize);
    if (NT_SUCCESS(Status))
    {
        if (Wnode->WnodeHeader.Flags & WNODE_FLAG_TOO_SMALL)
            Needed = ((PWNODE_TOO_SMALL)Wnode)->SizeNeeded - DataOffset;
        else
            Needed = Wnode->SizeDataBlock;

        if ((Wnode->WnodeHeader.Flags & WNODE_FLAG_TOO_SMALL) || (Needed > *OutBufferSize))
        {
            *OutBufferSize = (ULONG)ALIGN_UP_BY(Needed, 8);
            Status = STATUS_BUFFER_TOO_SMALL;
        }
        else
        {
            *OutBufferSize = Needed;
            RtlCopyMemory(InOutBuffer, (PUCHAR)Wnode + Wnode->DataBlockOffset, Needed);
        }
    }

    ExFreePoolWithTag(Wnode, TAG_WMI_QUERY);
    ObDereferenceObject(DataBlockObject);
    return Status;
}

/**
 * @brief Finds the device object at the bottom of the stack a file was opened on.
 *
 * Asks the stack for its target device relation, the PDO comes back referenced.
 */
NTSTATUS
NTAPI
WmipGetFilePdo(
    _In_ HANDLE FileHandle,
    _In_ KPROCESSOR_MODE AccessMode,
    _Outptr_ PDEVICE_OBJECT *Pdo)
{
    PDEVICE_RELATIONS Relations;
    PDEVICE_OBJECT TopDevice;
    PFILE_OBJECT FileObject;
    PIO_STACK_LOCATION Stack;
    IO_STATUS_BLOCK IoStatus;
    NTSTATUS Status;
    KEVENT Event;
    PIRP Irp;

    PAGED_CODE();

    *Pdo = NULL;

    Status = ObReferenceObjectByHandle(FileHandle,
                                       0,
                                       IoFileObjectType,
                                       AccessMode,
                                       (PVOID *)&FileObject,
                                       NULL);
    if (!NT_SUCCESS(Status))
        return Status;

    TopDevice = IoGetAttachedDeviceReference(IoGetRelatedDeviceObject(FileObject));

    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    Irp = IoBuildSynchronousFsdRequest(IRP_MJ_PNP, TopDevice, NULL, 0, NULL, &Event, &IoStatus);
    if (!Irp)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
    }
    else
    {
        Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
        Stack = IoGetNextIrpStackLocation(Irp);
        Stack->MinorFunction = IRP_MN_QUERY_DEVICE_RELATIONS;
        Stack->Parameters.QueryDeviceRelations.Type = TargetDeviceRelation;
        Stack->FileObject = FileObject;

        Status = IoCallDriver(TopDevice, Irp);
        if (Status == STATUS_PENDING)
        {
            KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
            Status = IoStatus.Status;
        }

        if (NT_SUCCESS(Status))
        {
            Relations = (PDEVICE_RELATIONS)IoStatus.Information;
            if (Relations && Relations->Count)
                *Pdo = Relations->Objects[0];
            else
                Status = STATUS_UNSUCCESSFUL;

            if (Relations)
                ExFreePool(Relations);
        }
    }

    ObDereferenceObject(TopDevice);
    ObDereferenceObject(FileObject);
    return Status;
}

/**
 * @brief Builds the PDO based instance name a data block uses for the device of a file.
 */
NTSTATUS
NTAPI
IoWMIHandleToInstanceName(
    _In_ PVOID DataBlockObject,
    _In_ HANDLE FileHandle,
    _Out_ PUNICODE_STRING InstanceName)
{
    PDEVICE_OBJECT Pdo;
    NTSTATUS Status;

    PAGED_CODE();

    Status = WmipGetFilePdo(FileHandle, KernelMode, &Pdo);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = IoWMIDeviceObjectToInstanceName(DataBlockObject, Pdo, InstanceName);
    ObDereferenceObject(Pdo);
    return Status;
}

/**
 * @brief Builds the PDO based instance name, device instance path and index, a data block uses for a device.
 *
 * The caller frees InstanceName->Buffer with ExFreePool.
 */
NTSTATUS
NTAPI
IoWMIDeviceObjectToInstanceName(
    _In_ PVOID DataBlockObject,
    _In_ PDEVICE_OBJECT DeviceObject,
    _Out_ PUNICODE_STRING InstanceName)
{
    PWMIP_GUID_OBJECT GuidObject = DataBlockObject;
    UNICODE_STRING InstancePath, IndexString;
    WCHAR IndexBuffer[12];
    PDEVICE_OBJECT Pdo;
    ULONG BaseIndex, Size;
    NTSTATUS Status;

    PAGED_CODE();

    RtlZeroMemory(InstanceName, sizeof(*InstanceName));

    Pdo = IoGetDeviceAttachmentBaseRef(DeviceObject);
    Status = WmipGetPdoInstanceInfo(&GuidObject->Guid, Pdo, &InstancePath, &BaseIndex);
    if (NT_SUCCESS(Status))
    {
        RtlInitEmptyUnicodeString(&IndexString, IndexBuffer, sizeof(IndexBuffer));
        RtlIntegerToUnicodeString(BaseIndex, 10, &IndexString);

        Size = InstancePath.Length + sizeof(WCHAR) + IndexString.Length + sizeof(UNICODE_NULL);
        InstanceName->Buffer = (Size <= MAXUSHORT) ?
                               ExAllocatePoolWithTag(PagedPool, Size, TAG_WMI_INSTANCE) : NULL;
        if (InstanceName->Buffer)
        {
            InstanceName->MaximumLength = (USHORT)Size;
            RtlAppendUnicodeStringToString(InstanceName, &InstancePath);
            RtlAppendUnicodeToString(InstanceName, L"_");
            RtlAppendUnicodeStringToString(InstanceName, &IndexString);
            InstanceName->Buffer[InstanceName->Length / sizeof(WCHAR)] = UNICODE_NULL;
        }
        else
        {
            Status = STATUS_INSUFFICIENT_RESOURCES;
        }
    }

    ObDereferenceObject(Pdo);
    return Status;
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
