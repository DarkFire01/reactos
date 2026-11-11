/*
 * PROJECT:     ReactOS USB Role Switch Class Extension
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Host and function role child devices
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "urscx.h"
#include <wdmguid.h>

#define NDEBUG
#include <debug.h>

DECLARE_CONST_UNICODE_STRING(UrsInstanceId, L"0");

static EVT_WDF_DEVICE_PREPARE_HARDWARE UrsPdoEvtPrepareHardware;
static EVT_WDF_DEVICE_RELEASE_HARDWARE UrsPdoEvtReleaseHardware;
static EVT_WDF_DEVICE_SURPRISE_REMOVAL UrsPdoEvtSurpriseRemoval;
static EVT_WDF_DEVICE_RESOURCE_REQUIREMENTS_QUERY UrsPdoEvtResourceRequirementsQuery;
static EVT_WDF_IO_QUEUE_IO_INTERNAL_DEVICE_CONTROL UrsPdoEvtIoInternalDeviceControl;
static IO_COMPLETION_ROUTINE UrsPdoForwardComplete;

/* The role takes the controller's IDs under the URS enumerator with a role suffix */
static
NTSTATUS
UrsPdoTransformId(
    _In_ WDFSTRING Source,
    _In_ URS_ROLE Role,
    _Out_ WDFSTRING *Result)
{
    PCWSTR Suffix = (Role == UrsRoleHost) ? L"&HOST" : L"&FUNCTION";
    UNICODE_STRING Id, Built;
    USHORT Separator;
    NTSTATUS Status;

    WdfStringGetUnicodeString(Source, &Id);

    Built.Length = 0;
    Built.MaximumLength = Id.Length + (USHORT)(wcslen(Suffix) * sizeof(WCHAR)) + 4 * sizeof(WCHAR);
    Built.Buffer = ExAllocatePoolWithTag(PagedPool, Built.MaximumLength, URS_POOL_TAG);
    if (!Built.Buffer)
        return STATUS_INSUFFICIENT_RESOURCES;

    for (Separator = 0; Separator < Id.Length / sizeof(WCHAR); Separator++)
    {
        if (Id.Buffer[Separator] == L'\\')
            break;
    }

    if (Separator < Id.Length / sizeof(WCHAR))
    {
        Id.Buffer += Separator;
        Id.Length -= Separator * sizeof(WCHAR);
        Id.MaximumLength -= Separator * sizeof(WCHAR);
        Status = RtlAppendUnicodeToString(&Built, L"URS");
        if (!NT_SUCCESS(Status))
            goto Exit;
    }

    Status = RtlAppendUnicodeStringToString(&Built, &Id);
    if (NT_SUCCESS(Status))
        Status = RtlAppendUnicodeToString(&Built, Suffix);
    if (NT_SUCCESS(Status))
        Status = WdfStringCreate(&Built, WDF_NO_OBJECT_ATTRIBUTES, Result);

Exit:
    ExFreePoolWithTag(Built.Buffer, URS_POOL_TAG);
    return Status;
}

static
NTSTATUS
UrsPdoAddTransformedIds(
    _In_ PWDFDEVICE_INIT DeviceInit,
    _In_ WDFCOLLECTION Ids,
    _In_ URS_ROLE Role,
    _In_ BOOLEAN Compatible)
{
    UNICODE_STRING String;
    WDFSTRING Id;
    ULONG Count = WdfCollectionGetCount(Ids), i;
    NTSTATUS Status = STATUS_SUCCESS;

    for (i = 0; i < Count; i++)
    {
        Status = UrsPdoTransformId(WdfCollectionGetItem(Ids, i), Role, &Id);
        if (!NT_SUCCESS(Status))
            return Status;

        WdfStringGetUnicodeString(Id, &String);
        if (Compatible)
            Status = WdfPdoInitAddCompatibleID(DeviceInit, &String);
        else
            Status = WdfPdoInitAddHardwareID(DeviceInit, &String);

        WdfObjectDelete(Id);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    return Status;
}

static
PCWSTR
UrsPdoHostCompatibleId(
    _In_ PURS_FDO Fdo)
{
    PURS_BUS_INFORMATION Bus = WdfMemoryGetBuffer(Fdo->BusInformation, NULL);
    BOOLEAN Xhci = (Fdo->Config.HostInterfaceType == UrsHostInterfaceTypeXhci);

    if (Bus->BusType == URS_BUS_ACPI)
        return Xhci ? L"ACPI\\PNP0D15" : L"ACPI\\PNP0D20";

    if (Bus->BusType == URS_BUS_PCI)
        return Xhci ? L"PCI\\CC_0C0330" : L"PCI\\CC_0C0320";

    return NULL;
}

static
NTSTATUS
UrsPdoAssignIds(
    _In_ PURS_FDO Fdo,
    _In_ URS_ROLE Role,
    _In_ PWDFDEVICE_INIT DeviceInit)
{
    UNICODE_STRING String;
    PCWSTR HostId;
    WDFSTRING Id;
    NTSTATUS Status;

    if (WdfCollectionGetCount(Fdo->HardwareIds))
    {
        Status = UrsPdoAddTransformedIds(DeviceInit, Fdo->HardwareIds, Role, FALSE);
        if (!NT_SUCCESS(Status))
            return Status;

        Status = UrsPdoTransformId(WdfCollectionGetFirstItem(Fdo->HardwareIds), Role, &Id);
        if (!NT_SUCCESS(Status))
            return Status;

        WdfStringGetUnicodeString(Id, &String);
        Status = WdfPdoInitAssignDeviceID(DeviceInit, &String);
        WdfObjectDelete(Id);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    Status = WdfPdoInitAssignInstanceID(DeviceInit, &UrsInstanceId);
    if (!NT_SUCCESS(Status))
        return Status;

    /* A host role child also matches the generic controller driver for its bus */
    HostId = (Role == UrsRoleHost) ? UrsPdoHostCompatibleId(Fdo) : NULL;
    if (HostId)
    {
        RtlInitUnicodeString(&String, HostId);
        Status = WdfPdoInitAddCompatibleID(DeviceInit, &String);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    if (!Fdo->CompatibleIds)
        return STATUS_SUCCESS;

    return UrsPdoAddTransformedIds(DeviceInit, Fdo->CompatibleIds, Role, TRUE);
}

static
NTSTATUS
UrsPdoEvtPrepareHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesRaw,
    _In_ WDFCMRESLIST ResourcesTranslated)
{
    PURS_PDO Pdo = UrsGetPdo(Device);

    UNREFERENCED_PARAMETER(ResourcesRaw);
    UNREFERENCED_PARAMETER(ResourcesTranslated);

    return Pdo->RoleConfig.Prepare(Pdo->Fdo, Pdo->RoleConfig.Role);
}

static
NTSTATUS
UrsPdoEvtReleaseHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesTranslated)
{
    PURS_PDO Pdo = UrsGetPdo(Device);

    UNREFERENCED_PARAMETER(ResourcesTranslated);

    return Pdo->RoleConfig.Release(Pdo->Fdo, Pdo->RoleConfig.Role);
}

static
VOID
UrsPdoEvtSurpriseRemoval(
    _In_ WDFDEVICE Device)
{
    UNREFERENCED_PARAMETER(Device);
}

/* Hands the child the register windows the client picked from the parent */
static
NTSTATUS
UrsPdoEvtResourceRequirementsQuery(
    _In_ WDFDEVICE Device,
    _In_ WDFIORESREQLIST Requirements)
{
    PURS_PDO Pdo = UrsGetPdo(Device);
    PIO_RESOURCE_DESCRIPTOR Descriptor;
    WDFIORESLIST List;
    WDFMEMORY Entry;
    ULONG Count = WdfCollectionGetCount(Pdo->Resources), i;
    NTSTATUS Status;

    if (!Count)
        return STATUS_SUCCESS;

    Status = WdfIoResourceListCreate(Requirements, WDF_NO_OBJECT_ATTRIBUTES, &List);
    if (!NT_SUCCESS(Status))
        return Status;

    for (i = 0; i < Count; i++)
    {
        Entry = WdfCollectionGetItem(Pdo->Resources, i);
        Descriptor = Entry ? WdfMemoryGetBuffer(Entry, NULL) : NULL;
        Status = WdfIoResourceListAppendDescriptor(List, Descriptor);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    return WdfIoResourceRequirementsListAppendIoResList(Requirements, List);
}

static
VOID
UrsPdoCompleteBusInformation(
    _In_ PURS_PDO Pdo,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputLength)
{
    PURS_BUS_INFORMATION Source = WdfMemoryGetBuffer(Pdo->Fdo->BusInformation, NULL);
    PURS_BUS_INFORMATION Target;
    NTSTATUS Status;

    if (OutputLength < sizeof(*Target))
    {
        WdfRequestSetInformation(Request, 0);
        WdfRequestComplete(Request, STATUS_BUFFER_TOO_SMALL);
        return;
    }

    Status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*Target), (PVOID *)&Target, NULL);
    if (!NT_SUCCESS(Status))
    {
        WdfRequestComplete(Request, Status);
        return;
    }

    Target->Size = Source->Size;
    Target->BusType = Source->BusType;
    if (OutputLength < Source->Size)
    {
        WdfRequestSetInformation(Request, sizeof(*Target));
        WdfRequestComplete(Request, STATUS_BUFFER_OVERFLOW);
        return;
    }

    RtlCopyMemory(Target, Source, Source->Size);
    WdfRequestSetInformation(Request, Source->Size);
    WdfRequestComplete(Request, STATUS_SUCCESS);
}

static
VOID
UrsPdoEvtIoInternalDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode)
{
    PURS_PDO Pdo = UrsGetPdo(WdfIoQueueGetDevice(Queue));
    PBOOLEAN Loading;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(InputBufferLength);

    switch (IoControlCode)
    {
        case IOCTL_URS_INTERNAL_GET_BUS_INFO:
            UrsPdoCompleteBusInformation(Pdo, Request, OutputBufferLength);
            return;

        case IOCTL_URS_INTERNAL_FAKE_LOAD:
            if (!OutputBufferLength)
            {
                Status = STATUS_BUFFER_TOO_SMALL;
                break;
            }

            Status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*Loading), (PVOID *)&Loading, NULL);
            if (!NT_SUCCESS(Status))
                break;

            *Loading = Pdo->Fdo->FakeFunctionLoad;
            WdfRequestSetInformation(Request, sizeof(*Loading));
            break;

        default:
            Status = STATUS_INVALID_DEVICE_REQUEST;
            break;
    }

    WdfRequestComplete(Request, Status);
}

static
NTSTATUS
NTAPI
UrsPdoForwardComplete(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);

    KeSetEvent((PKEVENT)Context, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

/* Interfaces the child is asked for come from the controller's own stack */
static
NTSTATUS
NTAPI
UrsPdoEvtQueryInterface(
    _In_ WDFDEVICE Device,
    _Inout_ PIRP Irp,
    _In_ PVOID DispatchContext)
{
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    PIO_STACK_LOCATION Next;
    PDEVICE_OBJECT Top;
    KEVENT Event;
    PIRP Forward;
    NTSTATUS Status = Irp->IoStatus.Status;

    if (NT_SUCCESS(Status) ||
        IsEqualGUID(Stack->Parameters.QueryInterface.InterfaceType, &GUID_PNP_LOCATION_INTERFACE))
    {
        IoSkipCurrentIrpStackLocation(Irp);
        return WdfDeviceWdmDispatchIrp(Device, Irp, DispatchContext);
    }

    Top = IoGetAttachedDeviceReference(WdfDeviceWdmGetDeviceObject(UrsGetPdo(Device)->Fdo->Device));
    Forward = IoAllocateIrp(Top->StackSize, FALSE);
    if (!Forward)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        Irp->IoStatus.Status = Status;
        goto Exit;
    }

    Forward->IoStatus.Status = STATUS_NOT_SUPPORTED;
    Forward->IoStatus.Information = Irp->IoStatus.Information;
    Next = IoGetNextIrpStackLocation(Forward);
    Next->MajorFunction = Stack->MajorFunction;
    Next->MinorFunction = Stack->MinorFunction;
    Next->Parameters.QueryInterface = Stack->Parameters.QueryInterface;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    IoSetCompletionRoutine(Forward, UrsPdoForwardComplete, &Event, TRUE, TRUE, TRUE);
    if (IoCallDriver(Top, Forward) == STATUS_PENDING)
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);

    if (Forward->IoStatus.Status != STATUS_NOT_SUPPORTED)
        Status = Forward->IoStatus.Status;

    Stack->Parameters.QueryInterface = Next->Parameters.QueryInterface;
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = Forward->IoStatus.Information;
    IoFreeIrp(Forward);

Exit:
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    ObDereferenceObject(Top);
    return Status;
}

static
NTSTATUS
UrsPdoCreateQueue(
    _In_ WDFDEVICE Device)
{
    WDF_IO_QUEUE_CONFIG Config;

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&Config, WdfIoQueueDispatchParallel);
    Config.PowerManaged = WdfFalse;
    Config.EvtIoInternalDeviceControl = UrsPdoEvtIoInternalDeviceControl;
    return WdfIoQueueCreate(Device, &Config, WDF_NO_OBJECT_ATTRIBUTES, NULL);
}

/**
 * @brief
 * Creates the child that carries the controller in one role.
 */
NTSTATUS
NTAPI
UrsPdoCreate(
    _In_ PURS_FDO Fdo,
    _In_ PURS_ROLE_CONFIG RoleConfig,
    _Out_ WDFDEVICE *Child)
{
    WDF_PNPPOWER_EVENT_CALLBACKS PnpPower;
    WDF_PDO_EVENT_CALLBACKS PdoCallbacks;
    WDF_DEVICE_PNP_CAPABILITIES Capabilities;
    WDF_OBJECT_ATTRIBUTES Attributes;
    UCHAR Minor = IRP_MN_QUERY_INTERFACE;
    PWDFCXDEVICE_INIT CxInit;
    PWDFDEVICE_INIT DeviceInit;
    WDFDEVICE Device = NULL;
    PURS_PDO Pdo;
    NTSTATUS Status;

    DeviceInit = WdfPdoInitAllocate(Fdo->Device);
    if (!DeviceInit)
        return STATUS_INSUFFICIENT_RESOURCES;

    WdfDeviceInitSetDeviceType(DeviceInit, FILE_DEVICE_CONTROLLER);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&PnpPower);
    PnpPower.EvtDevicePrepareHardware = UrsPdoEvtPrepareHardware;
    PnpPower.EvtDeviceReleaseHardware = UrsPdoEvtReleaseHardware;
    PnpPower.EvtDeviceSurpriseRemoval = UrsPdoEvtSurpriseRemoval;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &PnpPower);

    WDF_PDO_EVENT_CALLBACKS_INIT(&PdoCallbacks);
    PdoCallbacks.EvtDeviceResourceRequirementsQuery = UrsPdoEvtResourceRequirementsQuery;
    WdfPdoInitSetEventCallbacks(DeviceInit, &PdoCallbacks);

    Status = UrsPdoAssignIds(Fdo, RoleConfig->Role, DeviceInit);
    if (!NT_SUCCESS(Status))
        goto Exit;

    CxInit = UrsCxDeviceInitAllocate(DeviceInit);
    if (!CxInit)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Exit;
    }

    Status = UrsCxDeviceInitAssignWdmIrpPreprocessCallback(CxInit, UrsPdoEvtQueryInterface, IRP_MJ_PNP, &Minor, 1);
    if (!NT_SUCCESS(Status))
        goto Exit;

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, URS_PDO);
    Status = WdfDeviceCreate(&DeviceInit, &Attributes, &Device);
    if (!NT_SUCCESS(Status))
        goto Exit;

    Pdo = UrsGetPdo(Device);
    Pdo->Device = Device;
    Pdo->Fdo = Fdo;
    Pdo->RoleConfig = *RoleConfig;
    Pdo->Resources = (RoleConfig->Role == UrsRoleHost) ? Fdo->HostResources : Fdo->FunctionResources;

    /* ACPI finds the role's own namespace child through this address */
    WDF_DEVICE_PNP_CAPABILITIES_INIT(&Capabilities);
    Capabilities.UniqueID = WdfFalse;
    Capabilities.Address = (RoleConfig->Role == UrsRoleHost) ? 0 : 1;
    WdfDeviceSetPnpCapabilities(Device, &Capabilities);

    Status = UrsPdoCreateQueue(Device);
    if (NT_SUCCESS(Status))
        *Child = Device;

Exit:
    if (DeviceInit)
        WdfDeviceInitFree(DeviceInit);

    if (!NT_SUCCESS(Status) && Device)
        WdfObjectDelete(Device);

    return Status;
}
