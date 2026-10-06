/*
 * PROJECT:     ReactOS Simple Peripheral Bus
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     SpbCx target object - one peripheral's connection
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

/*
 * This is the far end of the resource hub. A peripheral formats its connection
 * id into \Device\RESOURCE_HUB\<16 hex digits> and opens it; acpiex answers with
 * STATUS_REPARSE naming this controller, and the reopened create arrives here
 * still carrying the id in its file name. We parse it back out, ask the hub what
 * connection it describes, and hand the controller driver a target it can read a
 * slave address off.
 */

#define NDEBUG
#include "spbcxp.h"

/*
 * The name that survives the reparse is the connection id, and nothing else is a
 * valid way to reach an SPB controller: an open with no name is somebody probing
 * the device itself, which is not a target.
 */
static
NTSTATUS
ScxParseFileObjectName(
    _In_ PCUNICODE_STRING FileName,
    _Out_ PLARGE_INTEGER ConnectionId)
{
    UNICODE_STRING NameCopy;
    PWSTR Name;
    USHORT Length;
    NTSTATUS Status;

    ConnectionId->QuadPart = 0;

    if (FileName == NULL || FileName->Buffer == NULL || FileName->Length == 0)
    {
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    Name = FileName->Buffer;
    Length = FileName->Length;

    if (*Name == L'\\')
    {
        Name++;
        Length -= sizeof(WCHAR);
    }

    if (Length < (RESOURCE_HUB_CONNECTION_FILE_SIZE - sizeof(UNICODE_NULL)))
    {
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    /* The helper wants a terminated string; a file name is not guaranteed one */
    NameCopy.Length = Length;
    NameCopy.MaximumLength = Length + sizeof(UNICODE_NULL);
    NameCopy.Buffer = ExAllocatePoolWithTag(PagedPool,
                                            NameCopy.MaximumLength,
                                            SCX_POOL_TAG);
    if (NameCopy.Buffer == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlCopyMemory(NameCopy.Buffer, Name, Length);
    NameCopy.Buffer[Length / sizeof(WCHAR)] = UNICODE_NULL;

    Status = RESOURCE_HUB_ID_FROM_FILE_NAME(NameCopy.Buffer, ConnectionId);

    ExFreePoolWithTag(NameCopy.Buffer, SCX_POOL_TAG);
    return Status;
}

/*
 * Ask the hub for the firmware descriptor behind the id. This is the only way to
 * learn the slave address and bus speed: they live in the peripheral's ACPI
 * Connection(), not in anything the controller can see.
 *
 * Sized in two passes because the descriptor length depends on the
 * ResourceSource string the firmware appended.
 */
static
NTSTATUS
ScxQueryDescriptorFromResourceHub(
    _In_ LARGE_INTEGER ConnectionId,
    _Outptr_result_bytebuffer_(*PropertiesLength) PVOID *Properties,
    _Out_ PULONG PropertiesLength)
{
    DECLARE_CONST_UNICODE_STRING(HubDeviceName, RESOURCE_HUB_DEVICE_NAME);
    RH_QUERY_CONNECTION_PROPERTIES_INPUT_BUFFER Input;
    PRH_QUERY_CONNECTION_PROPERTIES_OUTPUT_BUFFER Output = NULL;
    OBJECT_ATTRIBUTES ObjectAttributes;
    IO_STATUS_BLOCK IoStatusBlock;
    PDEVICE_OBJECT HubDevice = NULL;
    PFILE_OBJECT HubFile = NULL;
    HANDLE HubHandle = NULL;
    ULONG OutputLength;
    ULONG Attempt;
    KEVENT Event;
    PIRP Irp;
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
    {
        return Status;
    }

    Status = ObReferenceObjectByHandle(HubHandle,
                                       GENERIC_READ | GENERIC_WRITE,
                                       *IoFileObjectType,
                                       KernelMode,
                                       (PVOID *)&HubFile,
                                       NULL);
    ZwClose(HubHandle);

    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    HubDevice = IoGetAttachedDeviceReference(HubFile->DeviceObject);

    RtlZeroMemory(&Input, sizeof(Input));
    Input.Version = RH_QUERY_CONNECTION_PROPERTIES_INPUT_VERSION;
    Input.QueryType = ConnectionIdType;
    Input.u.ConnectionId = ConnectionId;

    /* First pass at a size that covers most descriptors, then exactly once more */
    OutputLength = sizeof(RH_QUERY_CONNECTION_PROPERTIES_OUTPUT_BUFFER) + 128;

    for (Attempt = 0; Attempt < 2; Attempt++)
    {
        Output = ExAllocatePoolWithTag(PagedPool, OutputLength, SCX_POOL_TAG);
        if (Output == NULL)
        {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            break;
        }

        RtlZeroMemory(Output, OutputLength);
        KeInitializeEvent(&Event, SynchronizationEvent, FALSE);

        Irp = IoBuildDeviceIoControlRequest(IOCTL_RH_QUERY_CONNECTION_PROPERTIES,
                                            HubDevice,
                                            &Input,
                                            sizeof(Input),
                                            Output,
                                            OutputLength,
                                            FALSE,
                                            &Event,
                                            &IoStatusBlock);
        if (Irp == NULL)
        {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            break;
        }

        Status = IoCallDriver(HubDevice, Irp);
        if (Status == STATUS_PENDING)
        {
            KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
            Status = IoStatusBlock.Status;
        }

        if (NT_SUCCESS(Status) && Output->PropertiesLength != 0)
        {
            /*
             * Keep the hub's whole answer, header and all. What a client is
             * handed by SpbTargetGetConnectionParameters is this buffer, not the
             * bare descriptor: the reference's own I2C driver reads
             * PropertiesLength at +4 and only then the descriptor at +8
             *. Handing back the
             * descriptor alone would have every client reading its bus type out
             * of the wrong bytes.
             */
            ULONG Total = FIELD_OFFSET(RH_QUERY_CONNECTION_PROPERTIES_OUTPUT_BUFFER,
                                       ConnectionProperties) +
                          Output->PropertiesLength;

            *Properties = ExAllocatePoolWithTag(NonPagedPool, Total, SCX_POOL_TAG);
            if (*Properties == NULL)
            {
                Status = STATUS_INSUFFICIENT_RESOURCES;
            }
            else
            {
                RtlCopyMemory(*Properties, Output, Total);
                *PropertiesLength = Total;
            }
            break;
        }

        if (Status != STATUS_BUFFER_TOO_SMALL || Output->PropertiesLength == 0)
        {
            if (NT_SUCCESS(Status))
            {
                Status = STATUS_UNSUCCESSFUL;
            }
            break;
        }

        /*
         * The hub filled in the length it needs even though it could not fill in
         * the descriptor, which is what makes the second pass exact.
         */
        OutputLength = sizeof(RH_QUERY_CONNECTION_PROPERTIES_OUTPUT_BUFFER) +
                       Output->PropertiesLength;
        ExFreePoolWithTag(Output, SCX_POOL_TAG);
        Output = NULL;
    }

    if (Output != NULL)
    {
        ExFreePoolWithTag(Output, SCX_POOL_TAG);
    }

    ObDereferenceObject(HubDevice);
    ObDereferenceObject(HubFile);

    return Status;
}

/*
 * CScxTarget::OnCreate, reached through the file-object config.
 *
 * By the time this runs the create has already been reparsed here by the hub, so
 * the name is a connection id and the controller driver is about to be told it
 * has a new peripheral.
 */
BOOLEAN
NTAPI
ScxEvtDeviceFileCreate(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_opt_ WDFFILEOBJECT FileObject)
{
    PSCX_CONTROLLER Controller;
    PSCX_TARGET Target;
    PFILE_OBJECT WdmFileObject;
    LARGE_INTEGER ConnectionId;
    NTSTATUS Status;

    PAGED_CODE();

    Controller = ScxGetControllerContext(Device);
    if (Controller == NULL || FileObject == NULL)
    {
        WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST);
        return TRUE;
    }

    Target = ScxGetTargetContext(FileObject);
    WdmFileObject = WdfFileObjectWdmGetFileObject(FileObject);

    if (Target == NULL || WdmFileObject == NULL)
    {
        WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST);
        return TRUE;
    }

    RtlZeroMemory(Target, sizeof(SCX_TARGET));
    Target->Controller = Controller;
    Target->FileObject = FileObject;

    Status = ScxParseFileObjectName(&WdmFileObject->FileName, &ConnectionId);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("SpbCx: create with a name that is not a connection id\n");
        WdfRequestComplete(Request, Status);
        return TRUE;
    }

    Target->ConnectionId = ConnectionId;

    Status = ScxQueryDescriptorFromResourceHub(ConnectionId,
                                               &Target->ConnectionProperties,
                                               &Target->PropertiesLength);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("SpbCx: hub has no descriptor for connection %I64x (0x%08lX)\n",
                ConnectionId.QuadPart, Status);
        WdfRequestComplete(Request, Status);
        return TRUE;
    }

    /*
     * Now the controller driver can read the slave address out of the descriptor
     * through SpbTargetGetConnectionParameters and program its hardware.
     */
    if (Controller->Config.EvtSpbTargetConnect != NULL)
    {
        Status = Controller->Config.EvtSpbTargetConnect(Device, (SPBTARGET)FileObject);
        if (!NT_SUCCESS(Status))
        {
            ExFreePoolWithTag(Target->ConnectionProperties, SCX_POOL_TAG);
            Target->ConnectionProperties = NULL;
            WdfRequestComplete(Request, Status);
            return TRUE;
        }
    }

    WdfRequestComplete(Request, STATUS_SUCCESS);
    return TRUE;
}

VOID
NTAPI
ScxEvtFileClose(
    _In_ WDFFILEOBJECT FileObject)
{
    PSCX_TARGET Target = ScxGetTargetContext(FileObject);

    PAGED_CODE();

    if (Target == NULL || Target->Controller == NULL)
    {
        return;
    }

    if (Target->Controller->Config.EvtSpbTargetDisconnect != NULL &&
        Target->ConnectionProperties != NULL)
    {
        Target->Controller->Config.EvtSpbTargetDisconnect(Target->Controller->Device,
                                                          (SPBTARGET)FileObject);
    }

    if (Target->ConnectionProperties != NULL)
    {
        ExFreePoolWithTag(Target->ConnectionProperties, SCX_POOL_TAG);
        Target->ConnectionProperties = NULL;
    }
}

VOID
NTAPI
ScxEvtFileCleanup(
    _In_ WDFFILEOBJECT FileObject)
{
    UNREFERENCED_PARAMETER(FileObject);
}

/*
 * The reference takes IRP_MJ_CREATE ahead of the framework so it can see the
 * file name before WDF normalizes it. Nothing else needs doing here yet, so the
 * IRP goes straight on to the framework, which routes it to
 * ScxEvtDeviceFileCreate above.
 */
NTSTATUS
NTAPI
ScxEvtWdmIrpPreprocessCreate(
    _In_ WDFDEVICE Device,
    _Inout_ PIRP Irp,
    _In_ PVOID DispatchContext)
{
    return ScxWdfDeviceWdmDispatchIrp(Device, Irp, DispatchContext);
}

VOID
NTAPI
ScxTargetGetConnectionParameters(
    _In_ PSPB_DRIVER_GLOBALS DriverGlobals,
    _In_ SPBTARGET SpbTarget,
    _Out_ SPB_CONNECTION_PARAMETERS *ConnectionParameters)
{
    PSCX_TARGET Target;

    if (ScxGlobalsFromClient(DriverGlobals) == NULL ||
        SpbTarget == NULL || ConnectionParameters == NULL)
    {
        return;
    }

    if (ConnectionParameters->Size != sizeof(SPB_CONNECTION_PARAMETERS))
    {
        return;
    }

    Target = ScxGetTargetContext((WDFFILEOBJECT)SpbTarget);
    if (Target == NULL)
    {
        return;
    }

    /*
     * The hub.s RH_QUERY_CONNECTION_PROPERTIES_OUTPUT_BUFFER: PropertiesLength
     * at +4, then the firmware descriptor at +8. A controller driver validates
     * the length, checks SerialBusType, and reads its slave address out of the
     * type-specific data past the descriptor header.
     */
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
    {
        return NULL;
    }

    return (WDFFILEOBJECT)SpbTarget;
}
