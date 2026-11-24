/*
 * PROJECT:         ReactOS Kernel
 * LICENSE:         GPL - See COPYING in the top level directory
 * FILE:            ntoskrnl/wmi/wmidrv.c
 * PURPOSE:         I/O Windows Management Instrumentation (WMI) Support
 * PROGRAMMERS:     Timo Kreuzer (timo.kreuzer@reactos.org)
 */

/* INCLUDES *****************************************************************/

#include <ntoskrnl.h>
#include <wmistr.h>
#include <wmiioctl.h>
#include "wmip.h"

#define NDEBUG
#include <debug.h>

// FIXME: these should go to a shared header
typedef struct _WMIP_REGISTER_GUIDS
{
    POBJECT_ATTRIBUTES ObjectAttributes;
    ULONG Unknown04;
    ULONG Unknown08;
    ULONG Unknown0C;
    ULONG Unknown10;
    ULONG Unknown14;

    WMIREGINFOW RegInfo;
} WMIP_REGISTER_GUIDS, *PWMIP_REGISTER_GUIDS;

typedef struct _WMIP_RESULT
{
    HANDLE Handle;
    ULONG Unknown04;
    TRACEHANDLE TraceHandle;
    BOOLEAN Unknown10;
} WMIP_RESULT, *PWMIP_RESULT;

typedef struct _WMI_UNREGISTER_GUID
{
    GUID Guid;
    ULONG Unknown10;
    ULONG Unknown14;
    ULONG Unknown18;
    ULONG Unknown1C;
} WMI_UNREGISTER_GUID, *PWMI_UNREGISTER_GUID;

typedef struct _WMI_SET_MARK
{
    ULONG Flags;
    WCHAR Mark[1];
} WMI_SET_MARK, *PWMI_SET_MARK;

PDEVICE_OBJECT WmipServiceDeviceObject;
PDEVICE_OBJECT WmipAdminDeviceObject;
FAST_IO_DISPATCH WmipFastIoDispatch;


/* FUNCTIONS *****************************************************************/

DRIVER_DISPATCH WmipOpenCloseCleanup;
DRIVER_DISPATCH WmipIoControl;
DRIVER_DISPATCH WmipSystemControl;
DRIVER_DISPATCH WmipShutdown;

NTSTATUS
NTAPI
WmipOpenCloseCleanup(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PAGED_CODE();

    /* No work to do, just return success */
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

static
NTSTATUS
WmiTraceEvent(
    PVOID InputBuffer,
    KPROCESSOR_MODE PreviousMode)
{
    UNIMPLEMENTED_ONCE;
    return STATUS_SUCCESS;
}

static
NTSTATUS
WmiTraceUserMessage(
    PVOID InputBuffer,
    ULONG InputBufferLength)
{
    UNIMPLEMENTED_ONCE;
    return STATUS_SUCCESS;
}

static
NTSTATUS
WmipCaptureGuidObjectAttributes(
    _In_ POBJECT_ATTRIBUTES GuidObjectAttributes,
    _Out_ POBJECT_ATTRIBUTES CapuredObjectAttributes,
    _Out_ PUNICODE_STRING CapturedObjectName,
    _Out_ PWSTR ObjectNameBuffer,
    _In_ KPROCESSOR_MODE AccessMode)
{
    ASSERT(AccessMode != KernelMode);

    _SEH2_TRY
    {
        /* Probe and copy the object attributes structure */
        ProbeForRead(GuidObjectAttributes,
                     sizeof(OBJECT_ATTRIBUTES),
                     sizeof(PVOID));
        *CapuredObjectAttributes = *GuidObjectAttributes;

        /* Probe and copy the object name UNICODE_STRING */
        ProbeForRead(CapuredObjectAttributes->ObjectName,
                     sizeof(UNICODE_STRING),
                     sizeof(PVOID));
        *CapturedObjectName = *CapuredObjectAttributes->ObjectName;

        /* Check if the object name has the expected length */
        if (CapturedObjectName->Length != 45 * sizeof(WCHAR))
        {
            _SEH2_YIELD(return STATUS_INVALID_PARAMETER);
        }

        /* Probe and copy the object name buffer */
        ProbeForRead(CapturedObjectName->Buffer,
                     CapturedObjectName->Length,
                     sizeof(WCHAR));
        RtlCopyMemory(ObjectNameBuffer,
                      CapturedObjectName->Buffer,
                      CapturedObjectName->Length);

        /* Fix pointers */
        CapturedObjectName->Buffer = ObjectNameBuffer;
        CapuredObjectAttributes->ObjectName = CapturedObjectName;
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        DPRINT1("Got exception!\n");
        _SEH2_YIELD(return _SEH2_GetExceptionCode());
    }
    _SEH2_END;

    return STATUS_SUCCESS;
}

static
NTSTATUS
WmipRegisterGuids(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PVOID Buffer,
    _In_ ULONG InputLength,
    _Inout_ PULONG OutputLength)
{
    PWMIP_REGISTER_GUIDS RegisterGuids = (PWMIP_REGISTER_GUIDS)Buffer;
    PWMIP_RESULT Result = (PWMIP_RESULT)Buffer;
    OBJECT_ATTRIBUTES LocalObjectAttributes;
    UNICODE_STRING LocalObjectName;
    WCHAR LocalObjectNameBuffer[45 + 1];
    KPROCESSOR_MODE PreviousMode;
    HANDLE GuidObjectHandle;
    PVOID GuidObject;
    NTSTATUS Status;

    /* Make sure the input buffer is large enough */
    if ((InputLength < sizeof(WMIP_REGISTER_GUIDS)) ||
        (RegisterGuids->RegInfo.BufferSize >
         (InputLength - FIELD_OFFSET(WMIP_REGISTER_GUIDS, RegInfo))))
    {
        return STATUS_UNSUCCESSFUL;
    }

    /* Make sure we have a resonable GUID count */
    if ((RegisterGuids->RegInfo.GuidCount == 0) ||
        (RegisterGuids->RegInfo.GuidCount > 0x10000))
    {
        return STATUS_UNSUCCESSFUL;
    }

    /* Capture object attributes */
    PreviousMode = ExGetPreviousMode();
    Status = WmipCaptureGuidObjectAttributes(RegisterGuids->ObjectAttributes,
                                             &LocalObjectAttributes,
                                             &LocalObjectName,
                                             LocalObjectNameBuffer,
                                             PreviousMode);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("WmipCaptureGuidObjectAttributes failed: 0x%lx\n", Status);
        return Status;
    }

    /* Open a new GUID object */
    Status = WmipOpenGuidObjectByName(&LocalObjectAttributes,
                                      SPECIFIC_RIGHTS_ALL,
                                      PreviousMode,
                                      &GuidObjectHandle,
                                      &GuidObject);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("WmipOpenGuidObjectByName failed: 0x%lx\n", Status);
        return Status;
    }

    /* Dereference the GUID object */
    ObDereferenceObject(GuidObject);

    /* Return the handle (user mode will close it) */
    Result->Handle = GuidObjectHandle;
    Result->TraceHandle = 0;
    *OutputLength = 24;

    return STATUS_SUCCESS;
}


static
NTSTATUS
WmipUnregisterGuids(
    _In_ PVOID Buffer,
    _In_ ULONG InputLength,
    _Inout_ PULONG OutputLength)
{
    /* For now we have nothing to do */
    return STATUS_SUCCESS;
}


/**
 * @brief Handles IOCTL_WMI_OPEN_GUID, IOCTL_WMI_OPEN_GUID_FOR_QUERYSET and IOCTL_WMI_OPEN_GUID_FOR_EVENTS.
 *
 * Opening for query and set fails with STATUS_WMI_GUID_NOT_FOUND unless a provider serves the GUID.
 */
static
NTSTATUS
WmipOpenGuidBlock(
    _In_ ULONG IoControlCode,
    _Inout_ PVOID Buffer,
    _In_ ULONG InputLength,
    _Inout_ PULONG OutputLength)
{
    PWMIOPENGUIDBLOCK OpenBlock = Buffer;
    OBJECT_ATTRIBUTES LocalObjectAttributes;
    UNICODE_STRING LocalObjectName;
    WCHAR LocalObjectNameBuffer[WMI_GUID_OBJECT_NAME_LENGTH + 1];
    KPROCESSOR_MODE PreviousMode;
    ACCESS_MASK DesiredAccess;
    HANDLE GuidObjectHandle;
    PVOID GuidObject;
    ULONG OpenType;
    NTSTATUS Status;
    GUID Guid;

    if ((InputLength != sizeof(*OpenBlock)) || (*OutputLength != sizeof(*OpenBlock)))
        return STATUS_UNSUCCESSFUL;

    /* The output overlaps the input, take what is needed first */
    DesiredAccess = OpenBlock->DesiredAccess;
    PreviousMode = ExGetPreviousMode();
    Status = WmipCaptureGuidObjectAttributes(OpenBlock->ObjectAttributes,
                                             &LocalObjectAttributes,
                                             &LocalObjectName,
                                             LocalObjectNameBuffer,
                                             PreviousMode);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("WmipCaptureGuidObjectAttributes failed: 0x%lx\n", Status);
        return Status;
    }

    if (LocalObjectAttributes.SecurityDescriptor || LocalObjectAttributes.SecurityQualityOfService)
        return STATUS_INVALID_PARAMETER;

    Status = WmipGuidFromObjectName(&LocalObjectAttributes, &Guid);
    if (!NT_SUCCESS(Status))
        return Status;

    if (IoControlCode == IOCTL_WMI_OPEN_GUID_FOR_QUERYSET)
        OpenType = WMIP_OPEN_QUERYSET;
    else if (IoControlCode == IOCTL_WMI_OPEN_GUID_FOR_EVENTS)
        OpenType = WMIP_OPEN_EVENTS;
    else
        OpenType = WMIP_OPEN_PLAIN;

    if ((OpenType == WMIP_OPEN_QUERYSET) && !WmipIsQuerySetGuid(&Guid))
        return STATUS_WMI_GUID_NOT_FOUND;

    Status = WmipOpenGuidObject(&Guid,
                                DesiredAccess,
                                PreviousMode,
                                &GuidObjectHandle,
                                &GuidObject);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("WmipOpenGuidObject failed: 0x%lx\n", Status);
        return Status;
    }

    Status = WmipAttachConsumer(GuidObject, OpenType, PreviousMode);
    ObDereferenceObject(GuidObject);
    if (!NT_SUCCESS(Status))
    {
        ObCloseHandle(GuidObjectHandle, PreviousMode);
        return Status;
    }

    OpenBlock->Handle64 = 0;
    OpenBlock->Handle = GuidObjectHandle;
    return STATUS_SUCCESS;
}

/**
 * @brief Handles IOCTL_WMI_QUERY_ALL_DATA, the input is a bare WNODE_HEADER naming the opened block.
 */
static
NTSTATUS
WmipQueryAllDataIoctl(
    _Inout_ PVOID Buffer,
    _In_ ULONG InputLength,
    _Inout_ PULONG OutputLength)
{
    PWNODE_ALL_DATA Wnode = Buffer;
    PWMIP_GUID_OBJECT GuidObject;
    ULONG ReturnSize;
    NTSTATUS Status;

    if (*OutputLength < WMIP_MIN_ALL_DATA_SIZE)
        return STATUS_BUFFER_TOO_SMALL;

    if ((InputLength < sizeof(WNODE_HEADER)) ||
        (Wnode->WnodeHeader.BufferSize != InputLength) ||
        !(Wnode->WnodeHeader.Flags & WNODE_FLAG_ALL_DATA) ||
        (Wnode->WnodeHeader.Flags & ~(WNODE_FLAG_ALL_DATA | WNODE_FLAG_STATIC_INSTANCE_NAMES)))
    {
        return STATUS_UNSUCCESSFUL;
    }

    Status = ObReferenceObjectByHandle(Wnode->WnodeHeader.KernelHandle,
                                       WMIGUID_QUERY,
                                       WmipGuidObjectType,
                                       ExGetPreviousMode(),
                                       (PVOID *)&GuidObject,
                                       NULL);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = WmipQueryAllData(GuidObject, Wnode, *OutputLength, &ReturnSize);
    ObDereferenceObject(GuidObject);

    if (NT_SUCCESS(Status))
        *OutputLength = ReturnSize;

    return Status;
}

/**
 * @brief Bounds of the name and data block of a WNODE naming one instance.
 *
 * Input data must lie inside the input, a WNODE written back must keep its name and data offset in the output.
 */
static
NTSTATUS
WmipProbeInstanceWnode(
    _In_ PWNODE_HEADER Wnode,
    _In_ ULONG MinimumSize,
    _In_ ULONG NameOffset,
    _In_ ULONG DataOffset,
    _In_ ULONG DataSize,
    _In_ ULONG InputLength,
    _In_ ULONG OutputLength,
    _In_ BOOLEAN CheckOutput,
    _In_ BOOLEAN CheckInputData)
{
    ULONG NameEnd;

    /* Requests are routed by name, so one has to be there */
    if ((InputLength < MinimumSize) || (NameOffset < MinimumSize) || (NameOffset & 1) ||
        (NameOffset > InputLength - sizeof(USHORT)))
    {
        return STATUS_UNSUCCESSFUL;
    }

    NameEnd = NameOffset + sizeof(USHORT) + *(PUSHORT)((PUCHAR)Wnode + NameOffset);
    if ((NameEnd > InputLength) || (CheckOutput && (NameEnd > OutputLength)))
        return STATUS_UNSUCCESSFUL;

    if ((DataOffset && (DataOffset < NameEnd)) || (DataOffset & 7))
        return STATUS_UNSUCCESSFUL;

    if (CheckInputData && DataOffset && ((DataSize > InputLength) || (DataOffset > InputLength - DataSize)))
        return STATUS_UNSUCCESSFUL;

    if (CheckOutput && ((OutputLength < MinimumSize) || !DataOffset || (DataOffset > OutputLength)))
        return STATUS_UNSUCCESSFUL;

    return STATUS_SUCCESS;
}

/**
 * @brief Validates the WNODE of a single instance query, set, item set or method call.
 */
static
NTSTATUS
WmipProbeSingleInstanceRequest(
    _In_ UCHAR MinorFunction,
    _In_ PVOID Buffer,
    _In_ ULONG InputLength,
    _In_ ULONG OutputLength)
{
    PWNODE_SINGLE_INSTANCE Single = Buffer;
    PWNODE_SINGLE_ITEM Item = Buffer;
    PWNODE_METHOD_ITEM Method = Buffer;
    ULONG Flags, Required, Allowed;
    NTSTATUS Status;

    if (InputLength < sizeof(WNODE_HEADER))
        return STATUS_UNSUCCESSFUL;

    switch (MinorFunction)
    {
        case IRP_MN_QUERY_SINGLE_INSTANCE:
            Status = WmipProbeInstanceWnode(&Single->WnodeHeader,
                                            FIELD_OFFSET(WNODE_SINGLE_INSTANCE, VariableData),
                                            Single->OffsetInstanceName,
                                            Single->DataBlockOffset,
                                            Single->SizeDataBlock,
                                            InputLength,
                                            OutputLength,
                                            TRUE,
                                            FALSE);
            Required = WNODE_FLAG_SINGLE_INSTANCE;
            break;

        case IRP_MN_CHANGE_SINGLE_INSTANCE:
            Status = WmipProbeInstanceWnode(&Single->WnodeHeader,
                                            FIELD_OFFSET(WNODE_SINGLE_INSTANCE, VariableData),
                                            Single->OffsetInstanceName,
                                            Single->DataBlockOffset,
                                            Single->SizeDataBlock,
                                            InputLength,
                                            OutputLength,
                                            FALSE,
                                            TRUE);
            Required = WNODE_FLAG_SINGLE_INSTANCE;
            break;

        case IRP_MN_CHANGE_SINGLE_ITEM:
            Status = WmipProbeInstanceWnode(&Item->WnodeHeader,
                                            FIELD_OFFSET(WNODE_SINGLE_ITEM, VariableData),
                                            Item->OffsetInstanceName,
                                            Item->DataBlockOffset,
                                            Item->SizeDataItem,
                                            InputLength,
                                            OutputLength,
                                            FALSE,
                                            TRUE);
            Required = WNODE_FLAG_SINGLE_ITEM;
            break;

        case IRP_MN_EXECUTE_METHOD:
            Status = WmipProbeInstanceWnode(&Method->WnodeHeader,
                                            FIELD_OFFSET(WNODE_METHOD_ITEM, VariableData),
                                            Method->OffsetInstanceName,
                                            Method->DataBlockOffset,
                                            Method->SizeDataBlock,
                                            InputLength,
                                            OutputLength,
                                            TRUE,
                                            TRUE);
            Required = WNODE_FLAG_METHOD_ITEM;
            break;

        default:
            return STATUS_INVALID_DEVICE_REQUEST;
    }

    if (!NT_SUCCESS(Status))
        return Status;

    /* Only the type flag and static instance names may come from user mode */
    Flags = Single->WnodeHeader.Flags;
    Allowed = Required | WNODE_FLAG_STATIC_INSTANCE_NAMES;
    if (!(Flags & Required) || (Flags & ~Allowed) || (Single->WnodeHeader.BufferSize != InputLength))
        return STATUS_UNSUCCESSFUL;

    return STATUS_SUCCESS;
}

/**
 * @brief Handles the single instance query, set, item set and method call IOCTLs.
 *
 * Sets are given the input size and return nothing, queries and methods fill the output.
 */
static
NTSTATUS
WmipSingleInstanceIoctl(
    _In_ UCHAR MinorFunction,
    _Inout_ PVOID Buffer,
    _In_ ULONG InputLength,
    _Inout_ PULONG OutputLength)
{
    PWNODE_SINGLE_INSTANCE Wnode = Buffer;
    PWMIP_GUID_OBJECT GuidObject;
    ACCESS_MASK DesiredAccess;
    ULONG BufferSize, ReturnSize;
    BOOLEAN IsSet;
    NTSTATUS Status;

    IsSet = (MinorFunction == IRP_MN_CHANGE_SINGLE_INSTANCE) || (MinorFunction == IRP_MN_CHANGE_SINGLE_ITEM);

    if ((MinorFunction == IRP_MN_QUERY_SINGLE_INSTANCE) && (*OutputLength < sizeof(WNODE_TOO_SMALL)))
        return STATUS_BUFFER_TOO_SMALL;

    Status = WmipProbeSingleInstanceRequest(MinorFunction, Buffer, InputLength, *OutputLength);
    if (!NT_SUCCESS(Status))
        return Status;

    if (IsSet)
        DesiredAccess = WMIGUID_SET;
    else if (MinorFunction == IRP_MN_EXECUTE_METHOD)
        DesiredAccess = WMIGUID_EXECUTE;
    else
        DesiredAccess = WMIGUID_QUERY;

    Status = ObReferenceObjectByHandle(Wnode->WnodeHeader.KernelHandle,
                                       DesiredAccess,
                                       WmipGuidObjectType,
                                       ExGetPreviousMode(),
                                       (PVOID *)&GuidObject,
                                       NULL);
    if (!NT_SUCCESS(Status))
        return Status;

    BufferSize = IsSet ? InputLength : *OutputLength;
    Status = WmipSendSingleInstanceRequest(GuidObject, MinorFunction, Wnode, BufferSize, &ReturnSize);
    ObDereferenceObject(GuidObject);

    if (NT_SUCCESS(Status))
        *OutputLength = IsSet ? 0 : ReturnSize;

    return Status;
}

/**
 * @brief Handles IOCTL_WMI_QUERY_GUID_INFO, telling whether a block is expensive to collect.
 */
static
NTSTATUS
WmipQueryGuidInfoIoctl(
    _Inout_ PVOID Buffer,
    _In_ ULONG InputLength,
    _Inout_ PULONG OutputLength)
{
    PWMI_GUID_INFO_QUERY Query = Buffer;
    PWMIP_GUID_OBJECT GuidObject;
    BOOLEAN IsExpensive;
    NTSTATUS Status;

    if ((InputLength < sizeof(*Query)) || (*OutputLength < sizeof(*Query)))
        return STATUS_BUFFER_TOO_SMALL;

    Status = ObReferenceObjectByHandle(Query->Handle,
                                       WMIGUID_QUERY,
                                       WmipGuidObjectType,
                                       ExGetPreviousMode(),
                                       (PVOID *)&GuidObject,
                                       NULL);
    if (!NT_SUCCESS(Status))
        return Status;

    /* Plain opens never learn about the providers of their GUID */
    if (GuidObject->OpenType == WMIP_OPEN_PLAIN)
    {
        Status = STATUS_WMI_GUID_DISCONNECTED;
    }
    else
    {
        Status = WmipQueryGuidExpense(&GuidObject->Guid, &IsExpensive);
        Query->IsExpensive = IsExpensive;
    }

    ObDereferenceObject(GuidObject);

    *OutputLength = sizeof(*Query);
    return Status;
}

/**
 * @brief Handles IOCTL_WMI_TRANSLATE_FILE_HANDLE, the device instance path and base index of a file's device.
 *
 * A short output only gets the size needed in SizeNeeded.
 */
static
NTSTATUS
WmipTranslateFileHandleIoctl(
    _Inout_ PVOID Buffer,
    _In_ ULONG InputLength,
    _Inout_ PULONG OutputLength)
{
    PWMI_FILE_HANDLE_NAME Request = Buffer;
    PWMIP_GUID_OBJECT GuidObject;
    KPROCESSOR_MODE PreviousMode;
    UNICODE_STRING InstancePath;
    PDEVICE_OBJECT Pdo;
    ULONG BaseIndex, Needed;
    NTSTATUS Status;

    if (InputLength != FIELD_OFFSET(WMI_FILE_HANDLE_NAME, InstanceNames))
        return STATUS_UNSUCCESSFUL;

    PreviousMode = ExGetPreviousMode();
    Status = WmipGetFilePdo((HANDLE)(ULONG_PTR)Request->FileHandle64, PreviousMode, &Pdo);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = ObReferenceObjectByHandle((HANDLE)(ULONG_PTR)Request->KernelHandle64,
                                       WMIGUID_QUERY,
                                       WmipGuidObjectType,
                                       PreviousMode,
                                       (PVOID *)&GuidObject,
                                       NULL);
    if (NT_SUCCESS(Status))
    {
        Status = WmipGetPdoInstanceInfo(&GuidObject->Guid, Pdo, &InstancePath, &BaseIndex);
        ObDereferenceObject(GuidObject);
    }

    if (NT_SUCCESS(Status))
    {
        /* The path is followed by two terminators, user mode appends the index */
        Needed = FIELD_OFFSET(WMI_FILE_HANDLE_NAME, InstanceNames) + InstancePath.Length + 2 * sizeof(WCHAR);
        if (*OutputLength >= Needed)
        {
            Request->BaseIndex = BaseIndex;
            Request->InstanceNameLength = InstancePath.Length + 2 * sizeof(WCHAR);
            RtlCopyMemory(Request->InstanceNames, InstancePath.Buffer, InstancePath.Length);
            Request->InstanceNames[InstancePath.Length / sizeof(WCHAR)] = UNICODE_NULL;
            Request->InstanceNames[InstancePath.Length / sizeof(WCHAR) + 1] = UNICODE_NULL;
            *OutputLength = Needed;
        }
        else if (*OutputLength >= sizeof(ULONG))
        {
            Request->SizeNeeded = Needed;
            *OutputLength = sizeof(ULONG);
        }
        else
        {
            Status = STATUS_UNSUCCESSFUL;
        }
    }

    ObDereferenceObject(Pdo);
    return Status;
}

static
NTSTATUS
WmiSetMark(
    PWMI_SET_MARK Buffer,
    ULONG Length)
{
    UNIMPLEMENTED;
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
WmipIoControl(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PIO_STACK_LOCATION IoStackLocation;
    ULONG IoControlCode;
    PVOID Buffer;
    ULONG InputLength, OutputLength;
    NTSTATUS Status;
    PAGED_CODE();

    /* Get the current stack location */
    IoStackLocation = IoGetCurrentIrpStackLocation(Irp);

    /* Get the io control parameters */
    IoControlCode = IoStackLocation->Parameters.DeviceIoControl.IoControlCode;
    Buffer = Irp->AssociatedIrp.SystemBuffer;
    InputLength = IoStackLocation->Parameters.DeviceIoControl.InputBufferLength;
    OutputLength = IoStackLocation->Parameters.DeviceIoControl.OutputBufferLength;

    switch (IoControlCode)
    {

        case IOCTL_WMI_REGISTER_GUIDS:
        {
            Status = WmipRegisterGuids(DeviceObject,
                                       Buffer,
                                       InputLength,
                                       &OutputLength);
            break;
        }

        case IOCTL_WMI_UNREGISTER_GUIDS:
        {
            Status = WmipUnregisterGuids(Buffer,
                                         InputLength,
                                         &OutputLength);
            break;
        }

        case IOCTL_WMI_RECEIVE_NOTIFICATIONS:
        {
            Status = WmipReceiveNotifications(Irp,
                                              Buffer,
                                              InputLength,
                                              &OutputLength);
            break;
        }

        case IOCTL_WMI_MARK_HANDLE_AS_CLOSED:
        {
            /* Handles are torn down when they are closed, nothing to prepare */
            OutputLength = 0;
            Status = STATUS_SUCCESS;
            break;
        }

        case IOCTL_WMI_OPEN_GUID:
        case IOCTL_WMI_OPEN_GUID_FOR_QUERYSET:
        case IOCTL_WMI_OPEN_GUID_FOR_EVENTS:
        {
            Status = WmipOpenGuidBlock(IoControlCode, Buffer, InputLength, &OutputLength);
            break;
        }

        case IOCTL_WMI_QUERY_ALL_DATA:
        {
            Status = WmipQueryAllDataIoctl(Buffer, InputLength, &OutputLength);
            break;
        }

        case IOCTL_WMI_QUERY_SINGLE_INSTANCE:
        {
            Status = WmipSingleInstanceIoctl(IRP_MN_QUERY_SINGLE_INSTANCE, Buffer, InputLength, &OutputLength);
            break;
        }

        case IOCTL_WMI_SET_SINGLE_INSTANCE:
        {
            Status = WmipSingleInstanceIoctl(IRP_MN_CHANGE_SINGLE_INSTANCE, Buffer, InputLength, &OutputLength);
            break;
        }

        case IOCTL_WMI_SET_SINGLE_ITEM:
        {
            Status = WmipSingleInstanceIoctl(IRP_MN_CHANGE_SINGLE_ITEM, Buffer, InputLength, &OutputLength);
            break;
        }

        case IOCTL_WMI_EXECUTE_METHOD:
        {
            Status = WmipSingleInstanceIoctl(IRP_MN_EXECUTE_METHOD, Buffer, InputLength, &OutputLength);
            break;
        }

        case IOCTL_WMI_GET_VERSION:
        {
            if (OutputLength < sizeof(ULONG))
            {
                Status = STATUS_BUFFER_TOO_SMALL;
                break;
            }

            *(PULONG)Buffer = WMI_KERNEL_INTERFACE_VERSION;
            OutputLength = sizeof(ULONG);
            Status = STATUS_SUCCESS;
            break;
        }

        case IOCTL_WMI_ENUMERATE_GUIDS:
        case IOCTL_WMI_ENUMERATE_GUIDS_AND_PROPERTIES:
        {
            if (OutputLength < FIELD_OFFSET(WMI_GUID_LIST, Guids))
            {
                Status = STATUS_BUFFER_TOO_SMALL;
                break;
            }

            Status = WmipEnumerateGuids(Buffer,
                                        OutputLength,
                                        IoControlCode == IOCTL_WMI_ENUMERATE_GUIDS_AND_PROPERTIES,
                                        &OutputLength);
            break;
        }

        case IOCTL_WMI_QUERY_GUID_INFO:
        {
            Status = WmipQueryGuidInfoIoctl(Buffer, InputLength, &OutputLength);
            break;
        }

        case IOCTL_WMI_ENUMERATE_MOF_RESOURCES:
        {
            if (OutputLength < sizeof(WMI_MOF_LIST))
            {
                Status = STATUS_INVALID_PARAMETER;
                break;
            }

            Status = WmipEnumerateMofResources(Buffer, OutputLength, &OutputLength);
            break;
        }

        case IOCTL_WMI_TRANSLATE_FILE_HANDLE:
        {
            Status = WmipTranslateFileHandleIoctl(Buffer, InputLength, &OutputLength);
            break;
        }

        case IOCTL_WMI_SET_MARK:
        {
            if (InputLength < FIELD_OFFSET(WMI_SET_MARK, Mark))
            {
                Status = STATUS_UNSUCCESSFUL;
                break;
            }

            Status = WmiSetMark(Buffer, InputLength);
            break;
        }

        default:
            DPRINT1("Unsupported yet IOCTL: 0x%lx\n", IoControlCode);
            Status = STATUS_INVALID_DEVICE_REQUEST;
            break;
    }

    if (Status == STATUS_PENDING)
        return Status;

    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = NT_SUCCESS(Status) ? OutputLength : 0;

    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

NTSTATUS
NTAPI
WmipSystemControl(
  _Inout_  PDEVICE_OBJECT DeviceObject,
  _Inout_  PIRP Irp)
{
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(DeviceObject);

    /* The WMI device registers no data blocks, requests for it end here untouched */
    Status = Irp->IoStatus.Status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

NTSTATUS
NTAPI
WmipShutdown(
  _Inout_  PDEVICE_OBJECT DeviceObject,
  _Inout_  PIRP Irp)
{
    Irp->IoStatus.Status = STATUS_SUCCESS;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

_Function_class_(FAST_IO_DEVICE_CONTROL)
_IRQL_requires_same_
BOOLEAN
NTAPI
WmipFastIoDeviceControl(
    _In_ PFILE_OBJECT FileObject,
    _In_ BOOLEAN Wait,
    _In_opt_ PVOID InputBuffer,
    _In_ ULONG InputBufferLength,
    _Out_opt_ PVOID OutputBuffer,
    _In_ ULONG OutputBufferLength,
    _In_ ULONG IoControlCode,
    _Out_ PIO_STATUS_BLOCK IoStatus,
    _In_ PDEVICE_OBJECT DeviceObject)
{
    PAGED_CODE();

    if (IoControlCode == IOCTL_WMI_TRACE_EVENT)
    {
        if (InputBufferLength < 0x30)
        {
            DPRINT1("Buffer too small\n");
            return FALSE;
        }

        IoStatus->Status = WmiTraceEvent(InputBuffer, ExGetPreviousMode());
        return TRUE;
    }
    else if (IoControlCode == IOCTL_WMI_TRACE_USER_MESSAGE)
    {
        if (InputBufferLength < 0x30)
        {
            DPRINT1("Buffer too small\n");
            return FALSE;
        }

        IoStatus->Status = WmiTraceUserMessage(InputBuffer, InputBufferLength);
        return TRUE;
    }

    DPRINT1("Invalid io control code for fast dispatch: 0x%lx\n", IoControlCode);
    return FALSE;
}

NTSTATUS
NTAPI
WmipDockUndockEventCallback(
    _In_ PVOID NotificationStructure,
    _Inout_opt_ PVOID Context)
{
    UNIMPLEMENTED;
    return STATUS_NOT_IMPLEMENTED;
}

_Function_class_(DRIVER_INITIALIZE)
_IRQL_requires_same_
NTSTATUS
NTAPI
WmipDriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    static UNICODE_STRING ServiceDeviceName = RTL_CONSTANT_STRING(L"\\Device\\WMIDataDevice");
    static UNICODE_STRING ServiceDosDeviceName = RTL_CONSTANT_STRING(L"\\DosDevices\\WMIDataDevice");
    static UNICODE_STRING AdminDeviceName = RTL_CONSTANT_STRING(L"\\Device\\WMIAdminDevice");
    static UNICODE_STRING AdminDosDeviceName = RTL_CONSTANT_STRING(L"\\DosDevices\\WMIAdminDevice");
    NTSTATUS Status;
    PAGED_CODE();

    /* Create the service device object */
    Status = IoCreateDevice(DriverObject,
                            0,
                            &ServiceDeviceName,
                            FILE_DEVICE_UNKNOWN,
                            FILE_DEVICE_SECURE_OPEN,
                            0,
                            &WmipServiceDeviceObject);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Failed to create service device: 0x%lx\n", Status);
        return Status;
    }

    /* Create a symbolic link for the service device */
    Status = IoCreateSymbolicLink(&ServiceDosDeviceName, &ServiceDeviceName);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("IoCreateSymbolicLink() failed: 0x%lx\n", Status);
        IoDeleteDevice(WmipServiceDeviceObject);
        return Status;
    }

    /* Create the admin device object */
    Status = IoCreateDevice(DriverObject,
                            0,
                            &AdminDeviceName,
                            FILE_DEVICE_UNKNOWN,
                            FILE_DEVICE_SECURE_OPEN,
                            0,
                            &WmipAdminDeviceObject);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Failed to create admin device: 0x%lx\n", Status);
        IoDeleteDevice(WmipServiceDeviceObject);
        IoDeleteSymbolicLink(&ServiceDosDeviceName);
        return Status;
    }

    /* Create a symbolic link for the admin device */
    Status = IoCreateSymbolicLink(&AdminDosDeviceName, &AdminDeviceName);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("IoCreateSymbolicLink() failed: 0x%lx\n", Status);
        IoDeleteSymbolicLink(&ServiceDosDeviceName);
        IoDeleteDevice(WmipServiceDeviceObject);
        IoDeleteDevice(WmipAdminDeviceObject);
        return Status;
    }

    /* Initialize dispatch routines */
    DriverObject->MajorFunction[IRP_MJ_CREATE] = WmipOpenCloseCleanup;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = WmipOpenCloseCleanup;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = WmipIoControl;
    DriverObject->MajorFunction[IRP_MJ_CLEANUP] = WmipOpenCloseCleanup;
    DriverObject->MajorFunction[IRP_MJ_SYSTEM_CONTROL] = WmipSystemControl;
    DriverObject->MajorFunction[IRP_MJ_SHUTDOWN] = WmipShutdown;

    /* Initialize fast dispatch */
    RtlZeroMemory(&WmipFastIoDispatch, sizeof(WmipFastIoDispatch));
    WmipFastIoDispatch.SizeOfFastIoDispatch = sizeof(WmipFastIoDispatch);
    WmipFastIoDispatch.FastIoDeviceControl = WmipFastIoDeviceControl;
    DriverObject->FastIoDispatch = &WmipFastIoDispatch;

    /* Register the WMI service device */
    IoWMIRegistrationControl(WmipServiceDeviceObject, WMIREG_ACTION_REGISTER);

    /* Register a shutdown notification */
    IoRegisterShutdownNotification(WmipServiceDeviceObject);

    /* Initialization is done */
    WmipServiceDeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;
    WmipAdminDeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;

    return STATUS_SUCCESS;
}


