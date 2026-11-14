/*
 * PROJECT:     ReactOS Simple Peripheral Bus
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     SpbCx target object, one peripheral's connection
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#include "spbcxp.h"

#define NDEBUG
#include <debug.h>

/**
 * @brief Pulls the resource hub connection id out of a create's file name.
 *
 * @return STATUS_NO_SUCH_FILE for an open of the controller itself,
 * STATUS_OBJECT_NAME_INVALID for a name that is not a connection id.
 */
static
NTSTATUS
NTAPI
ScxParseFileObjectName(
    _In_ PCUNICODE_STRING FileName,
    _Out_ PLARGE_INTEGER ConnectionId)
{
    PWCHAR NameCopy;
    PCWCH Name;
    USHORT Length;
    NTSTATUS Status;

    ConnectionId->QuadPart = 0;

    Name = FileName->Buffer;
    Length = (Name != NULL) ? FileName->Length : 0;

    while (Length != 0 && *Name == L'\\')
    {
        Name++;
        Length -= sizeof(WCHAR);
    }

    if (Length == 0)
        return STATUS_NO_SUCH_FILE;

    if (Length < (RESOURCE_HUB_CONNECTION_FILE_SIZE - sizeof(UNICODE_NULL)))
        return STATUS_OBJECT_NAME_INVALID;

    /* The helper wants a terminated string and a file name is not one */
    NameCopy = ExAllocatePoolZero(PagedPool, Length + sizeof(UNICODE_NULL), SCX_POOL_TAG);
    if (NameCopy == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlCopyMemory(NameCopy, Name, Length);

    Status = RESOURCE_HUB_ID_FROM_FILE_NAME(NameCopy, ConnectionId);

    ExFreePoolWithTag(NameCopy, SCX_POOL_TAG);

    return NT_SUCCESS(Status) ? STATUS_SUCCESS : STATUS_OBJECT_NAME_INVALID;
}

/**
 * @brief Sends IOCTL_RH_QUERY_CONNECTION_PROPERTIES to the hub and waits for it.
 */
static
NTSTATUS
NTAPI
ScxSendHubQuery(
    _In_ PDEVICE_OBJECT HubDevice,
    _In_ PRH_QUERY_CONNECTION_PROPERTIES_INPUT_BUFFER Input,
    _Out_writes_bytes_(OutputLength) PRH_QUERY_CONNECTION_PROPERTIES_OUTPUT_BUFFER Output,
    _In_ ULONG OutputLength)
{
    IO_STATUS_BLOCK IoStatusBlock;
    KEVENT Event;
    PIRP Irp;
    NTSTATUS Status;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);

    Irp = IoBuildDeviceIoControlRequest(IOCTL_RH_QUERY_CONNECTION_PROPERTIES,
                                        HubDevice,
                                        Input,
                                        sizeof(*Input),
                                        Output,
                                        OutputLength,
                                        FALSE,
                                        &Event,
                                        &IoStatusBlock);
    if (Irp == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = IoCallDriver(HubDevice, Irp);
    if (Status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
        Status = IoStatusBlock.Status;
    }

    return Status;
}

/**
 * @brief Fetches the firmware descriptor behind a connection id.
 *
 * The caller gets the hub's whole output buffer: PropertiesLength followed by
 * the descriptor, which is what SpbTargetGetConnectionParameters hands out.
 */
static
NTSTATUS
NTAPI
ScxQueryConnectionProperties(
    _In_ LARGE_INTEGER ConnectionId,
    _Outptr_result_bytebuffer_(*PropertiesLength) PVOID *Properties,
    _Out_ PULONG PropertiesLength)
{
    DECLARE_CONST_UNICODE_STRING(HubDeviceName, RESOURCE_HUB_DEVICE_NAME);
    RH_QUERY_CONNECTION_PROPERTIES_INPUT_BUFFER Input;
    PRH_QUERY_CONNECTION_PROPERTIES_OUTPUT_BUFFER Output;
    OBJECT_ATTRIBUTES ObjectAttributes;
    IO_STATUS_BLOCK IoStatusBlock;
    PDEVICE_OBJECT HubDevice;
    PFILE_OBJECT HubFile;
    HANDLE HubHandle;
    ULONG HeaderLength;
    ULONG OutputLength;
    ULONG Pass;
    NTSTATUS Status;

    PAGED_CODE();

    *Properties = NULL;
    *PropertiesLength = 0;

    InitializeObjectAttributes(&ObjectAttributes,
                               (PUNICODE_STRING)&HubDeviceName,
                               OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
                               NULL,
                               NULL);

    Status = ZwOpenFile(&HubHandle,
                        GENERIC_READ | GENERIC_WRITE,
                        &ObjectAttributes,
                        &IoStatusBlock,
                        FILE_SHARE_READ | FILE_SHARE_WRITE,
                        FILE_NON_DIRECTORY_FILE);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = ObReferenceObjectByHandle(HubHandle,
                                       GENERIC_READ | GENERIC_WRITE,
                                       *IoFileObjectType,
                                       KernelMode,
                                       (PVOID *)&HubFile,
                                       NULL);
    ZwClose(HubHandle);
    if (!NT_SUCCESS(Status))
        return Status;

    HubDevice = IoGetAttachedDeviceReference(HubFile->DeviceObject);

    RtlZeroMemory(&Input, sizeof(Input));
    Input.Version = RH_QUERY_CONNECTION_PROPERTIES_INPUT_VERSION;
    Input.QueryType = ConnectionIdType;
    Input.u.ConnectionId = ConnectionId;

    HeaderLength = FIELD_OFFSET(RH_QUERY_CONNECTION_PROPERTIES_OUTPUT_BUFFER, ConnectionProperties);

    /* A guess first; on overflow the hub reports the size it needs */
    OutputLength = sizeof(*Output) + 128;
    Status = STATUS_BUFFER_TOO_SMALL;

    for (Pass = 0; Pass < 2 && Status == STATUS_BUFFER_TOO_SMALL; Pass++)
    {
        Output = ExAllocatePoolZero(NonPagedPoolNx, OutputLength, SCX_POOL_TAG);
        if (Output == NULL)
        {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            break;
        }

        Status = ScxSendHubQuery(HubDevice, &Input, Output, OutputLength);

        if (NT_SUCCESS(Status) && Output->PropertiesLength != 0)
        {
            *Properties = Output;
            *PropertiesLength = HeaderLength + Output->PropertiesLength;
            break;
        }

        if (NT_SUCCESS(Status))
            Status = STATUS_UNSUCCESSFUL;
        else if (Status == STATUS_BUFFER_TOO_SMALL && Output->PropertiesLength != 0)
            OutputLength = HeaderLength + Output->PropertiesLength;
        else if (Status == STATUS_BUFFER_TOO_SMALL)
            Status = STATUS_UNSUCCESSFUL;

        ExFreePoolWithTag(Output, SCX_POOL_TAG);
    }

    ObDereferenceObject(HubDevice);
    ObDereferenceObject(HubFile);

    return Status;
}

BOOLEAN
NTAPI
ScxEvtDeviceFileCreate(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ WDFFILEOBJECT FileObject)
{
    PSCX_CONTROLLER Controller;
    PFILE_OBJECT WdmFileObject;
    PSCX_TARGET Target;
    LARGE_INTEGER ConnectionId;
    NTSTATUS Status;

    PAGED_CODE();

    Controller = ScxGetControllerContext(Device);
    Target = (FileObject != NULL) ? ScxGetTargetContext(FileObject) : NULL;
    WdmFileObject = (FileObject != NULL) ? WdfFileObjectWdmGetFileObject(FileObject) : NULL;

    if (Controller == NULL || Target == NULL || WdmFileObject == NULL)
    {
        WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST);
        return TRUE;
    }

    RtlZeroMemory(Target, sizeof(*Target));
    Target->Controller = Controller;
    Target->FileObject = FileObject;

    Status = ScxParseFileObjectName(&WdmFileObject->FileName, &ConnectionId);
    if (Status == STATUS_NO_SUCH_FILE)
    {
        /* Opening the controller itself is allowed, it just has no bus to talk to */
        WdfRequestComplete(Request, STATUS_SUCCESS);
        return TRUE;
    }

    if (!NT_SUCCESS(Status))
    {
        DPRINT1("SpbCx: create with a name that is not a connection id\n");
        WdfRequestComplete(Request, Status);
        return TRUE;
    }

    Target->ConnectionId = ConnectionId;

    Status = ScxQueryConnectionProperties(ConnectionId,
                                          &Target->ConnectionProperties,
                                          &Target->PropertiesLength);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("SpbCx: hub has no descriptor for connection %I64x (0x%lx)\n",
                ConnectionId.QuadPart,
                Status);
        WdfRequestComplete(Request, Status);
        return TRUE;
    }

    if (Controller->TargetAttributesSet)
        Status = WdfObjectAllocateContext(FileObject, &Controller->TargetAttributes, NULL);

    if (NT_SUCCESS(Status) && Controller->Config.EvtSpbTargetConnect != NULL)
        Status = Controller->Config.EvtSpbTargetConnect(Device, (SPBTARGET)FileObject);

    if (!NT_SUCCESS(Status))
    {
        ExFreePoolWithTag(Target->ConnectionProperties, SCX_POOL_TAG);
        Target->ConnectionProperties = NULL;
    }

    WdfRequestComplete(Request, Status);
    return TRUE;
}

VOID
NTAPI
ScxEvtFileClose(
    _In_ WDFFILEOBJECT FileObject)
{
    PSCX_CONTROLLER Controller;
    PSCX_TARGET Target;

    PAGED_CODE();

    Target = ScxGetTargetContext(FileObject);
    if (Target == NULL || Target->ConnectionProperties == NULL)
        return;

    Controller = Target->Controller;
    if (Controller != NULL && Controller->Config.EvtSpbTargetDisconnect != NULL)
        Controller->Config.EvtSpbTargetDisconnect(Controller->Device, (SPBTARGET)FileObject);

    ExFreePoolWithTag(Target->ConnectionProperties, SCX_POOL_TAG);
    Target->ConnectionProperties = NULL;
}

VOID
NTAPI
ScxEvtFileCleanup(
    _In_ WDFFILEOBJECT FileObject)
{
    UNREFERENCED_PARAMETER(FileObject);
}

/* The create goes on to the framework, which routes it to ScxEvtDeviceFileCreate */
NTSTATUS
NTAPI
ScxEvtWdmIrpPreprocessCreate(
    _In_ WDFDEVICE Device,
    _Inout_ PIRP Irp,
    _In_ WDFCONTEXT DispatchContext)
{
    return WdfDeviceWdmDispatchIrp(Device, Irp, DispatchContext);
}

VOID
NTAPI
ScxTargetGetConnectionParameters(
    _In_ PSPB_DRIVER_GLOBALS DriverGlobals,
    _In_ SPBTARGET SpbTarget,
    _Inout_ SPB_CONNECTION_PARAMETERS *ConnectionParameters)
{
    PSCX_TARGET Target;

    if (ScxGlobalsFromClient(DriverGlobals) == NULL || SpbTarget == NULL || ConnectionParameters == NULL)
        return;

    if (ConnectionParameters->Size != sizeof(*ConnectionParameters))
        return;

    Target = ScxGetTargetContext((WDFFILEOBJECT)SpbTarget);
    if (Target == NULL)
        return;

    ConnectionParameters->ConnectionParameters = Target->ConnectionProperties;
    ConnectionParameters->ConnectionTag = NULL;
}

WDFFILEOBJECT
NTAPI
ScxTargetGetFileObject(
    _In_ PSPB_DRIVER_GLOBALS DriverGlobals,
    _In_ SPBTARGET SpbTarget)
{
    if (ScxGlobalsFromClient(DriverGlobals) == NULL || SpbTarget == NULL)
        return NULL;

    return (WDFFILEOBJECT)SpbTarget;
}
