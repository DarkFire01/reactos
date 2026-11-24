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

typedef struct _WMI_GUID_OBJECT_ENTRY
{
    HANDLE Handle;
    ULONG Unknown04;
} WMI_GUID_OBJECT_ENTRY, *PWMI_GUID_OBJECT_ENTRY;

typedef struct _WMI_NOTIFICATION
{
    ULONG NumberOfGuidObjects;
    ULONG Unknown04;
    ULONG Unknown08;
    ULONG Unknown0C;
    ULONG Unknown10;
    ULONG Unknown14;
    WMI_GUID_OBJECT_ENTRY GuidObjects[0];
} WMI_NOTIFICATION, *PWMI_NOTIFICATION;

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
    UNIMPLEMENTED_DBGBREAK();
    return STATUS_SUCCESS;
}

static
NTSTATUS
WmiTraceUserMessage(
    PVOID InputBuffer,
    ULONG InputBufferLength)
{
    UNIMPLEMENTED_DBGBREAK();
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

VOID
NTAPI
WmipClearIrpObjectList(
    _In_ PIRP Irp)
{
    PWMIP_IRP_CONTEXT IrpContext;
    PLIST_ENTRY ListEntry;
    PWMIP_GUID_OBJECT GuidObject;

    /* Get the IRP context */
    IrpContext = (PWMIP_IRP_CONTEXT)Irp->Tail.Overlay.DriverContext;

    /* Loop all GUID objects attached to this IRP */
    for (ListEntry = IrpContext->GuidObjectListHead.Flink;
         ListEntry != &IrpContext->GuidObjectListHead;
         ListEntry = ListEntry->Flink)
    {
        /* Get the GUID object */
        GuidObject = CONTAINING_RECORD(ListEntry, WMIP_GUID_OBJECT, IrpLink);

        /* Make sure the IRP matches and clear it */
        ASSERT(GuidObject->Irp == Irp);
        GuidObject->Irp = NULL;

        /* Remove the entry */
        RemoveEntryList(ListEntry);
    }
}

VOID
NTAPI
WmipNotificationIrpCancel(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    /* Clear the list */
    WmipClearIrpObjectList(Irp);

    /* Release the cancel spin lock */
    IoReleaseCancelSpinLock(Irp->CancelIrql);

    /* Set the status to cancelled and complete the IRP */
    Irp->IoStatus.Status = STATUS_CANCELLED;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
}

static
VOID
WmipInitializeIrpContext(
    PWMIP_IRP_CONTEXT IrpContext)
{
    /* Initialize the list head for GUID objects */
    InitializeListHead(&IrpContext->GuidObjectListHead);
}

static
NTSTATUS
WmipReceiveNotifications(
    _Inout_ PIRP Irp,
    _In_ PVOID Buffer,
    _In_ ULONG InputLength,
    _Inout_ PULONG OutputLength)
{
    PWMI_NOTIFICATION Notification;
    PWMIP_IRP_CONTEXT IrpContext;
    NTSTATUS Status;

    //__debugbreak();
    if ((InputLength < sizeof(WMI_NOTIFICATION)) || (*OutputLength < 0x38))
    {
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    /// FIXME: For now we don't do any actual work, but simply pretend we are
    /// waiting for notifications. We won't ever deliver any though.
    Notification = (PWMI_NOTIFICATION)Buffer;
    DBG_UNREFERENCED_LOCAL_VARIABLE(Notification);

    // loop all objects
        // reference the object
            // on failure, fail the whole request

    // loop all objects
        // update the irp (synchronization!)
            // if we had one before complete the old irp with an error

    /* Get the IRP context and initialize it */
    IrpContext = (PWMIP_IRP_CONTEXT)Irp->Tail.Overlay.DriverContext;
    WmipInitializeIrpContext(IrpContext);

    // loop all objects
        // insert the objects into the IRP list

    /* Set our cancel routine for cleanup */
    IoSetCancelRoutine(Irp, WmipNotificationIrpCancel);

    /* Check if the IRP is already being cancelled */
    if (Irp->Cancel && IoSetCancelRoutine(Irp, NULL))
    {
        Status = STATUS_CANCELLED;
    }
    else
    {
        /* Mark the IRP as pending */
        IoMarkIrpPending(Irp);
        Status = STATUS_PENDING;
    }

    return Status;
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

    if ((IoControlCode == IOCTL_WMI_OPEN_GUID_FOR_QUERYSET) && !WmipIsQuerySetGuid(&Guid))
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

    ObDereferenceObject(GuidObject);

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

/* Bounds of the instance name and data offset of an incoming WNODE_SINGLE_INSTANCE query. */
static
NTSTATUS
WmipProbeSingleInstance(
    _In_ PWNODE_SINGLE_INSTANCE Wnode,
    _In_ ULONG InputLength,
    _In_ ULONG OutputLength)
{
    ULONG HeaderSize = FIELD_OFFSET(WNODE_SINGLE_INSTANCE, VariableData);
    ULONG NameOffset, NameEnd, DataOffset;

    if ((InputLength < HeaderSize) || (OutputLength < HeaderSize))
        return STATUS_UNSUCCESSFUL;

    if ((Wnode->WnodeHeader.BufferSize != InputLength) ||
        !(Wnode->WnodeHeader.Flags & WNODE_FLAG_SINGLE_INSTANCE) ||
        (Wnode->WnodeHeader.Flags & ~(WNODE_FLAG_SINGLE_INSTANCE | WNODE_FLAG_STATIC_INSTANCE_NAMES)))
    {
        return STATUS_UNSUCCESSFUL;
    }

    NameOffset = Wnode->OffsetInstanceName;
    if ((NameOffset < HeaderSize) || (NameOffset & 1) || (NameOffset > InputLength - sizeof(USHORT)))
        return STATUS_UNSUCCESSFUL;

    NameEnd = NameOffset + sizeof(USHORT) + *(PUSHORT)((PUCHAR)Wnode + NameOffset);
    if ((NameEnd > InputLength) || (NameEnd > OutputLength))
        return STATUS_UNSUCCESSFUL;

    /* The provider writes its data at DataBlockOffset, past the name */
    DataOffset = Wnode->DataBlockOffset;
    if ((DataOffset < NameEnd) || (DataOffset & 7) || (DataOffset > OutputLength))
        return STATUS_UNSUCCESSFUL;

    return STATUS_SUCCESS;
}

/**
 * @brief Handles IOCTL_WMI_QUERY_SINGLE_INSTANCE.
 */
static
NTSTATUS
WmipQuerySingleInstanceIoctl(
    _Inout_ PVOID Buffer,
    _In_ ULONG InputLength,
    _Inout_ PULONG OutputLength)
{
    PWNODE_SINGLE_INSTANCE Wnode = Buffer;
    PWMIP_GUID_OBJECT GuidObject;
    ULONG ReturnSize;
    NTSTATUS Status;

    if (*OutputLength < sizeof(WNODE_TOO_SMALL))
        return STATUS_BUFFER_TOO_SMALL;

    Status = WmipProbeSingleInstance(Wnode, InputLength, *OutputLength);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = ObReferenceObjectByHandle(Wnode->WnodeHeader.KernelHandle,
                                       WMIGUID_QUERY,
                                       WmipGuidObjectType,
                                       ExGetPreviousMode(),
                                       (PVOID *)&GuidObject,
                                       NULL);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = WmipQuerySingleInstance(GuidObject, Wnode, *OutputLength, &ReturnSize);
    ObDereferenceObject(GuidObject);

    if (NT_SUCCESS(Status))
        *OutputLength = ReturnSize;

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

        case 0x228168:
        {
            DPRINT1("IOCTL 0x228168 is unimplemented, ignoring\n");
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
            Status = WmipQuerySingleInstanceIoctl(Buffer, InputLength, &OutputLength);
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
            __debugbreak();
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
    UNIMPLEMENTED_DBGBREAK();
    return STATUS_NOT_IMPLEMENTED;
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
    UNIMPLEMENTED_DBGBREAK();
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


