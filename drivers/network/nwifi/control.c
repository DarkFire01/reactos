/*
 * PROJECT:     ReactOS Native WiFi filter
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The control device the WLAN service reaches adapters through
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "nwifi.h"

#define NDEBUG
#include <debug.h>

/* An OID request the control device sends down for the WLAN service */
typedef struct _NWIFI_REQUEST
{
    NDIS_OID_REQUEST OidRequest;
    PIRP Irp;
    PNWIFI_MODULE Module;
} NWIFI_REQUEST, *PNWIFI_REQUEST;

static PDEVICE_OBJECT NwifiControlDevice;

static DRIVER_DISPATCH NwifiDispatchCreate;
static DRIVER_DISPATCH NwifiDispatchClose;
static DRIVER_DISPATCH NwifiDispatchDeviceControl;

/* A handle opened on \\.\nativewifip\{GUID} carries that interface's GUID */
#define NWIFI_FILE_GUID(_FileObject) ((GUID *)(_FileObject)->FsContext)

/* Indications */

/**
 * @brief
 * Keeps a dot11 status indication for the WLAN service. The oldest one is
 * dropped when the service falls behind.
 */
VOID
NTAPI
NwifiQueueIndication(
    _In_ PNWIFI_MODULE Module,
    _In_ PNDIS_STATUS_INDICATION StatusIndication)
{
    PNWIFI_QUEUED_INDICATION Slot;
    KIRQL OldIrql;
    ULONG Length;

    Length = min(StatusIndication->StatusBufferSize, NWIFI_INDICATION_MAX);
    if (StatusIndication->StatusBuffer == NULL)
        Length = 0;

    KeAcquireSpinLock(&Module->Lock, &OldIrql);

    if (Module->IndicationCount == NWIFI_INDICATION_SLOTS)
    {
        Module->IndicationHead = (Module->IndicationHead + 1) % NWIFI_INDICATION_SLOTS;
        Module->IndicationCount--;
    }

    Slot = &Module->Indications[(Module->IndicationHead + Module->IndicationCount) % NWIFI_INDICATION_SLOTS];
    Slot->StatusCode = StatusIndication->StatusCode;
    Slot->Length = Length;
    if (Length != 0)
        RtlCopyMemory(Slot->Data, StatusIndication->StatusBuffer, Length);
    Module->IndicationCount++;

    KeReleaseSpinLock(&Module->Lock, OldIrql);
}

static
NTSTATUS
NwifiGetIndication(
    _In_ PNWIFI_MODULE Module,
    _In_ PIRP Irp,
    _In_ ULONG OutputLength)
{
    PNWIFI_INDICATION Out = Irp->AssociatedIrp.SystemBuffer;
    PNWIFI_QUEUED_INDICATION Slot;
    NTSTATUS Status;
    KIRQL OldIrql;

    KeAcquireSpinLock(&Module->Lock, &OldIrql);

    if (Module->IndicationCount == 0)
    {
        Status = STATUS_NO_MORE_ENTRIES;
    }
    else
    {
        Slot = &Module->Indications[Module->IndicationHead];
        if (OutputLength < FIELD_OFFSET(NWIFI_INDICATION, Data) + Slot->Length)
        {
            Status = STATUS_BUFFER_TOO_SMALL;
        }
        else
        {
            Out->StatusCode = Slot->StatusCode;
            Out->Length = Slot->Length;
            RtlCopyMemory(Out->Data, Slot->Data, Slot->Length);
            Irp->IoStatus.Information = FIELD_OFFSET(NWIFI_INDICATION, Data) + Slot->Length;

            Module->IndicationHead = (Module->IndicationHead + 1) % NWIFI_INDICATION_SLOTS;
            Module->IndicationCount--;
            Status = STATUS_SUCCESS;
        }
    }

    KeReleaseSpinLock(&Module->Lock, OldIrql);
    return Status;
}

/* OID requests */

static
VOID
NwifiFinishRequest(
    _In_ PNWIFI_REQUEST Request,
    _In_ NDIS_STATUS Status)
{
    PIRP Irp = Request->Irp;
    PNWIFI_OID_REQUEST Out = Irp->AssociatedIrp.SystemBuffer;
    PNDIS_OID_REQUEST OidRequest = &Request->OidRequest;

    Out->Status = Status;
    Out->BytesWritten = 0;
    Out->BytesRead = 0;
    Out->BytesNeeded = 0;

    switch (OidRequest->RequestType)
    {
        case NdisRequestQueryInformation:
            Out->BytesWritten = OidRequest->DATA.QUERY_INFORMATION.BytesWritten;
            Out->BytesNeeded = OidRequest->DATA.QUERY_INFORMATION.BytesNeeded;
            break;

        case NdisRequestSetInformation:
            Out->BytesRead = OidRequest->DATA.SET_INFORMATION.BytesRead;
            Out->BytesNeeded = OidRequest->DATA.SET_INFORMATION.BytesNeeded;
            break;

        default:
            Out->BytesWritten = OidRequest->DATA.METHOD_INFORMATION.BytesWritten;
            Out->BytesRead = OidRequest->DATA.METHOD_INFORMATION.BytesRead;
            Out->BytesNeeded = OidRequest->DATA.METHOD_INFORMATION.BytesNeeded;
            break;
    }

    /* The NDIS status travels in the buffer; the IOCTL itself worked */
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = FIELD_OFFSET(NWIFI_OID_REQUEST, Data) + min(Out->BytesWritten, Out->OutputLength);

    NwifiDereferenceModule(Request->Module);
    ExFreePoolWithTag(Request, NWIFI_TAG);
    IoCompleteRequest(Irp, IO_NETWORK_INCREMENT);
}

/**
 * @brief
 * An OID request of ours that pended comes back from below.
 */
VOID
NTAPI
NwifiOidRequestComplete(
    _In_ NDIS_HANDLE FilterModuleContext,
    _In_ PNDIS_OID_REQUEST OidRequest,
    _In_ NDIS_STATUS Status)
{
    UNREFERENCED_PARAMETER(FilterModuleContext);

    NwifiFinishRequest(CONTAINING_RECORD(OidRequest, NWIFI_REQUEST, OidRequest), Status);
}

static
NTSTATUS
NwifiOidRequest(
    _In_ PNWIFI_MODULE Module,
    _In_ PIRP Irp,
    _In_ ULONG InputLength,
    _In_ ULONG OutputLength)
{
    PNWIFI_OID_REQUEST In = Irp->AssociatedIrp.SystemBuffer;
    PNDIS_OID_REQUEST OidRequest;
    PNWIFI_REQUEST Request;
    NDIS_STATUS Status;

    /* The data rides in the one METHOD_BUFFERED buffer, both ways */
    if (InputLength < FIELD_OFFSET(NWIFI_OID_REQUEST, Data) ||
        OutputLength < FIELD_OFFSET(NWIFI_OID_REQUEST, Data) ||
        In->InputLength > InputLength - FIELD_OFFSET(NWIFI_OID_REQUEST, Data) ||
        In->OutputLength > OutputLength - FIELD_OFFSET(NWIFI_OID_REQUEST, Data))
    {
        return STATUS_INVALID_PARAMETER;
    }

    Request = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Request), NWIFI_TAG);
    if (Request == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlZeroMemory(Request, sizeof(*Request));
    Request->Irp = Irp;
    Request->Module = Module;

    OidRequest = &Request->OidRequest;
    OidRequest->Header.Type = NDIS_OBJECT_TYPE_OID_REQUEST;
    OidRequest->Header.Revision = NDIS_OID_REQUEST_REVISION_1;
    OidRequest->Header.Size = NDIS_SIZEOF_OID_REQUEST_REVISION_1;
    OidRequest->PortNumber = NDIS_DEFAULT_PORT_NUMBER;
    OidRequest->RequestId = Request;

    switch (In->RequestType)
    {
        case NWIFI_REQUEST_QUERY:
            OidRequest->RequestType = NdisRequestQueryInformation;
            OidRequest->DATA.QUERY_INFORMATION.Oid = In->Oid;
            OidRequest->DATA.QUERY_INFORMATION.InformationBuffer = In->Data;
            OidRequest->DATA.QUERY_INFORMATION.InformationBufferLength = In->OutputLength;
            break;

        case NWIFI_REQUEST_SET:
            OidRequest->RequestType = NdisRequestSetInformation;
            OidRequest->DATA.SET_INFORMATION.Oid = In->Oid;
            OidRequest->DATA.SET_INFORMATION.InformationBuffer = In->Data;
            OidRequest->DATA.SET_INFORMATION.InformationBufferLength = In->InputLength;
            break;

        case NWIFI_REQUEST_METHOD:
            OidRequest->RequestType = NdisRequestMethod;
            OidRequest->DATA.METHOD_INFORMATION.Oid = In->Oid;
            OidRequest->DATA.METHOD_INFORMATION.InformationBuffer = In->Data;
            OidRequest->DATA.METHOD_INFORMATION.InputBufferLength = In->InputLength;
            OidRequest->DATA.METHOD_INFORMATION.OutputBufferLength = In->OutputLength;
            break;

        default:
            ExFreePoolWithTag(Request, NWIFI_TAG);
            return STATUS_INVALID_PARAMETER;
    }

    /* The request keeps the module until it comes back */
    InterlockedIncrement(&Module->References);
    IoMarkIrpPending(Irp);

    Status = NdisFOidRequest(Module->FilterHandle, OidRequest);
    if (Status != NDIS_STATUS_PENDING)
        NwifiFinishRequest(Request, Status);

    return STATUS_PENDING;
}

/* Dispatch */

static
NTSTATUS
NwifiCompleteIrp(
    _In_ PIRP Irp,
    _In_ NTSTATUS Status)
{
    Irp->IoStatus.Status = Status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

static
NTSTATUS
NTAPI
NwifiDispatchCreate(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    PFILE_OBJECT FileObject = Stack->FileObject;
    UNICODE_STRING GuidString;
    GUID *InterfaceGuid;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(DeviceObject);

    Irp->IoStatus.Information = 0;
    FileObject->FsContext = NULL;

    /* A bare open reaches no interface; \{GUID} picks one */
    if (FileObject->FileName.Length == 0)
        return NwifiCompleteIrp(Irp, STATUS_SUCCESS);

    GuidString = FileObject->FileName;
    if (GuidString.Buffer[0] == L'\\')
    {
        GuidString.Buffer++;
        GuidString.Length -= sizeof(WCHAR);
        GuidString.MaximumLength -= sizeof(WCHAR);
    }

    InterfaceGuid = ExAllocatePoolWithTag(PagedPool, sizeof(*InterfaceGuid), NWIFI_TAG);
    if (InterfaceGuid == NULL)
        return NwifiCompleteIrp(Irp, STATUS_INSUFFICIENT_RESOURCES);

    Status = RtlGUIDFromString(&GuidString, InterfaceGuid);
    if (!NT_SUCCESS(Status))
    {
        ExFreePoolWithTag(InterfaceGuid, NWIFI_TAG);
        return NwifiCompleteIrp(Irp, STATUS_OBJECT_NAME_NOT_FOUND);
    }

    FileObject->FsContext = InterfaceGuid;
    return NwifiCompleteIrp(Irp, STATUS_SUCCESS);
}

static
NTSTATUS
NTAPI
NwifiDispatchClose(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);

    UNREFERENCED_PARAMETER(DeviceObject);

    if (Stack->MajorFunction == IRP_MJ_CLOSE && Stack->FileObject->FsContext != NULL)
    {
        ExFreePoolWithTag(Stack->FileObject->FsContext, NWIFI_TAG);
        Stack->FileObject->FsContext = NULL;
    }

    Irp->IoStatus.Information = 0;
    return NwifiCompleteIrp(Irp, STATUS_SUCCESS);
}

static
NTSTATUS
NTAPI
NwifiDispatchDeviceControl(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    ULONG InputLength = Stack->Parameters.DeviceIoControl.InputBufferLength;
    ULONG OutputLength = Stack->Parameters.DeviceIoControl.OutputBufferLength;
    PNWIFI_MODULE Module;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(DeviceObject);

    Irp->IoStatus.Information = 0;

    if (NWIFI_FILE_GUID(Stack->FileObject) == NULL)
        return NwifiCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST);

    Module = NwifiReferenceModule(NWIFI_FILE_GUID(Stack->FileObject));
    if (Module == NULL)
        return NwifiCompleteIrp(Irp, STATUS_DEVICE_DOES_NOT_EXIST);

    switch (Stack->Parameters.DeviceIoControl.IoControlCode)
    {
        case IOCTL_NWIFI_OID_REQUEST:
            Status = NwifiOidRequest(Module, Irp, InputLength, OutputLength);
            NwifiDereferenceModule(Module);
            if (Status == STATUS_PENDING)
                return Status;
            break;

        case IOCTL_NWIFI_GET_INDICATION:
            Status = NwifiGetIndication(Module, Irp, OutputLength);
            NwifiDereferenceModule(Module);
            break;

        default:
            NwifiDereferenceModule(Module);
            Status = STATUS_INVALID_DEVICE_REQUEST;
            break;
    }

    return NwifiCompleteIrp(Irp, Status);
}

/**
 * @brief
 * Creates \Device\nativewifip and the name user mode opens it by.
 */
NTSTATUS
NTAPI
NwifiCreateControlDevice(
    _In_ PDRIVER_OBJECT DriverObject)
{
    UNICODE_STRING DeviceName = RTL_CONSTANT_STRING(NWIFI_DEVICE_NAME);
    UNICODE_STRING DosName = RTL_CONSTANT_STRING(NWIFI_DOS_DEVICE_NAME);
    NTSTATUS Status;

    Status = IoCreateDevice(DriverObject,
                            0,
                            &DeviceName,
                            FILE_DEVICE_NETWORK,
                            FILE_DEVICE_SECURE_OPEN,
                            FALSE,
                            &NwifiControlDevice);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = IoCreateSymbolicLink(&DosName, &DeviceName);
    if (!NT_SUCCESS(Status))
    {
        IoDeleteDevice(NwifiControlDevice);
        NwifiControlDevice = NULL;
        return Status;
    }

    DriverObject->MajorFunction[IRP_MJ_CREATE] = NwifiDispatchCreate;
    DriverObject->MajorFunction[IRP_MJ_CLEANUP] = NwifiDispatchClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = NwifiDispatchClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = NwifiDispatchDeviceControl;

    NwifiControlDevice->Flags |= DO_BUFFERED_IO;
    NwifiControlDevice->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Removes the control device again.
 */
VOID
NTAPI
NwifiDeleteControlDevice(VOID)
{
    UNICODE_STRING DosName = RTL_CONSTANT_STRING(NWIFI_DOS_DEVICE_NAME);

    if (NwifiControlDevice == NULL)
        return;

    IoDeleteSymbolicLink(&DosName);
    IoDeleteDevice(NwifiControlDevice);
    NwifiControlDevice = NULL;
}
