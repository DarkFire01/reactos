/*
 * PROJECT:     ReactOS Trusted Runtime class extension
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Driver entry, control device and the client function table
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#include "wtrp.h"

#define NDEBUG
#include <debug.h>

#ifndef IO_REPARSE_GLOBAL
#define IO_REPARSE_GLOBAL 2
#endif

DRIVER_INITIALIZE DriverEntry;
static EVT_WDF_DRIVER_UNLOAD WtrEvtDriverUnload;
static EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL WtrEvtControlIoControl;
static EVT_WDFDEVICE_WDM_IRP_PREPROCESS WtrEvtControlCreate;

static WTR_HANDOFF_MASTER_DEVICE WtrHandoffMasterDevice;
static WTR_HANDOFF_SERVICE_DEVICE WtrHandoffServiceDevice;
static WTR_LOG_MESSAGE WtrLogMessage;
static WTR_QUERY_OS_SERVICE WtrQueryOsService;
static WTR_CALL_OS_SERVICE WtrCallOsServiceEntry;
static WTR_COMPLETE_ASYNC_REQUEST WtrCompleteAsyncRequest;

static
NTSTATUS
NTAPI
WtrClassInitialize(VOID);

static
VOID
NTAPI
WtrClassDeinitialize(VOID);

static
NTSTATUS
NTAPI
WtrClassBindClient(
    _In_ PWDF_CLASS_BIND_INFO ClassBindInfo,
    _In_ PWDF_COMPONENT_GLOBALS ComponentGlobals);

static
VOID
NTAPI
WtrClassUnbindClient(
    _In_ PWDF_CLASS_BIND_INFO ClassBindInfo,
    _In_ PWDF_COMPONENT_GLOBALS ComponentGlobals);

static WDF_CLASS_LIBRARY_INFO WtrClassLibraryInfo =
{
    sizeof(WtrClassLibraryInfo),
    { WTR_CLASS_MAJOR_VERSION, WTR_CLASS_MINOR_VERSION, 0 },
    WtrClassInitialize,
    WtrClassDeinitialize,
    WtrClassBindClient,
    WtrClassUnbindClient
};

static const PVOID WtrFunctionTable[WtrFunctionTableNumEntries] =
{
    (PVOID)WtrHandoffMasterDevice,
    (PVOID)WtrHandoffServiceDevice,
    (PVOID)WtrLogMessage,
    (PVOID)WtrQueryOsService,
    (PVOID)WtrCallOsServiceEntry,
    (PVOID)WtrCompleteAsyncRequest
};

DECLARE_CONST_UNICODE_STRING(WtrControlDeviceName, L"\\Device\\WindowsTrustedRT");
DECLARE_CONST_UNICODE_STRING(WtrControlLinkName, L"\\DosDevices\\WindowsTrustedRT");
DECLARE_CONST_UNICODE_STRING(WtrControlSddl,
    L"D:P(A;;GRGWGX;;;SY)(A;;GRGWGX;;;BA)(A;;GRGWGX;;;WD)(A;;GRGWGX;;;RC)(A;;GRGWGX;;;AC)"
    L"(A;;GRGWGX;;;S-1-15-3-1024-3203351429-2120443784-2872670797-1918958302-2829055647-4275794519-765664414-2751773334)");

/* Every secure service of every master, for the control device */
WDFCOLLECTION WtrSecureServices;

static WDFDEVICE WtrControlDevice;

typedef struct _WTR_LOG_PARAMETERS
{
    ULONG Level;
    PCSTR Format;
    va_list Arguments;
} WTR_LOG_PARAMETERS, *PWTR_LOG_PARAMETERS;

static
NTSTATUS
NTAPI
WtrClassInitialize(VOID)
{
    return STATUS_SUCCESS;
}

static
VOID
NTAPI
WtrClassDeinitialize(VOID)
{
}

static
NTSTATUS
NTAPI
WtrClassBindClient(
    _In_ PWDF_CLASS_BIND_INFO ClassBindInfo,
    _In_ PWDF_COMPONENT_GLOBALS ComponentGlobals)
{
    WDF_OBJECT_ATTRIBUTES Attributes;
    PWTR_BIND_CONTEXT Bind;
    WDFOBJECT Handle;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(ComponentGlobals);

    if (ClassBindInfo->Version.Major != WTR_CLASS_MAJOR_VERSION ||
        ClassBindInfo->Version.Minor != WTR_CLASS_MINOR_VERSION ||
        ClassBindInfo->FunctionTableCount != WtrFunctionTableNumEntries)
    {
        DPRINT1("WTR: refusing a %lu.%lu client with %lu slots\n",
                ClassBindInfo->Version.Major,
                ClassBindInfo->Version.Minor,
                ClassBindInfo->FunctionTableCount);
        return STATUS_INVALID_PARAMETER;
    }

    DPRINT1("WTR: binding a %lu.%lu client\n", ClassBindInfo->Version.Major, ClassBindInfo->Version.Minor);

    RtlCopyMemory(ClassBindInfo->FunctionTable, WtrFunctionTable, sizeof(WtrFunctionTable));

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, WTR_BIND_CONTEXT);
    Status = WdfObjectCreate(&Attributes, &Handle);
    if (!NT_SUCCESS(Status))
        return Status;

    Bind = WtrGetBindContext(Handle);
    Bind->MajorVersion = ClassBindInfo->Version.Major;
    Bind->MinorVersion = ClassBindInfo->Version.Minor;
    *(WTR_CLIENT *)ClassBindInfo->ClassBindInfo = (WTR_CLIENT)Handle;

    return STATUS_SUCCESS;
}

static
VOID
NTAPI
WtrClassUnbindClient(
    _In_ PWDF_CLASS_BIND_INFO ClassBindInfo,
    _In_ PWDF_COMPONENT_GLOBALS ComponentGlobals)
{
    WTR_CLIENT *Client = (WTR_CLIENT *)ClassBindInfo->ClassBindInfo;

    UNREFERENCED_PARAMETER(ComponentGlobals);

    if (*Client == NULL)
        return;

    WdfObjectDelete((WDFOBJECT)*Client);
    *Client = NULL;
}

static
NTSTATUS
NTAPI
WtrHandoffMasterDevice(
    _In_ WTR_CLIENT Client,
    _In_ PWDFDEVICE_INIT DeviceInit,
    _In_ PWTR_DEVICE_CALLBACKS Callbacks,
    _Out_opt_ WDFDEVICE *Device)
{
    PWTR_BIND_CONTEXT Bind = WtrGetBindContext((WDFOBJECT)Client);
    PWTR_MASTER_CONTEXT Master;
    WDFDEVICE NewDevice = NULL;
    NTSTATUS Status;

    Status = WtrAddMasterDevice(DeviceInit, Callbacks->Flags, &NewDevice);
    DPRINT1("WTR: master handoff, flags 0x%lx -> 0x%lx\n", Callbacks->Flags, Status);
    if (!NT_SUCCESS(Status))
        return Status;

    Master = WtrGetMasterContext(NewDevice);
    Master->Client = Client;
    if (Bind->MajorVersion == 1)
        Master->Callbacks = *Callbacks;

    if (Device)
        *Device = NewDevice;

    if (Master->Callbacks.EvtDeviceAdded)
    {
        Status = Master->Callbacks.EvtDeviceAdded(NewDevice);
        DPRINT1("WTR: client device added -> 0x%lx\n", Status);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
WtrHandoffServiceDevice(
    _In_ WTR_CLIENT Client,
    _In_ PWDFDEVICE_INIT DeviceInit,
    _In_ const GUID *ServiceId,
    _In_opt_ PWTR_SERVICE_CALLBACKS Callbacks,
    _Out_opt_ WDFDEVICE *Device)
{
    PWTR_SERVICE_CONTEXT Service;
    PWTR_MASTER_CONTEXT Master;
    PWTR_BIND_CONTEXT Bind;
    WDFDEVICE ServiceDevice = NULL;
    WDFDEVICE MasterDevice = NULL;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(Client);

    Status = WtrAddServiceDevice(DeviceInit, ServiceId, &ServiceDevice, &MasterDevice);
    DPRINT1("WTR: service handoff {%08lx-%04x-%04x} -> 0x%lx\n", ServiceId->Data1, ServiceId->Data2, ServiceId->Data3, Status);
    if (!NT_SUCCESS(Status))
        return Status;

    Master = WtrGetMasterContext(MasterDevice);

    /* A proxy hands the device over without callbacks; the master's client supplies them */
    if (!Callbacks && Master->Callbacks.EvtDeviceGetServiceCallbacks)
        Callbacks = Master->Callbacks.EvtDeviceGetServiceCallbacks(MasterDevice, ServiceId);

    if (!Callbacks)
    {
        DPRINT1("WTR: client gave no callbacks for the service\n");
        return STATUS_INVALID_PARAMETER;
    }

    Bind = WtrGetBindContext((WDFOBJECT)Master->Client);
    Service = WtrGetServiceContext(ServiceDevice);
    if (Bind->MajorVersion == 1)
        Service->Callbacks = *Callbacks;

    if (Service->Callbacks.EvtServiceAdded)
    {
        Status = Service->Callbacks.EvtServiceAdded(MasterDevice, ServiceId, ServiceDevice);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    Status = WdfDeviceCreateDeviceInterface(ServiceDevice, ServiceId, NULL);
    DPRINT1("WTR: service interface -> 0x%lx\n", Status);
    if (!NT_SUCCESS(Status))
        return Status;

    if (Device)
        *Device = ServiceDevice;

    return STATUS_SUCCESS;
}

static
VOID
NTAPI
WtrLogWorker(
    _In_ PVOID Parameter)
{
    PWTR_LOG_PARAMETERS Log = Parameter;
    CHAR Buffer[WTR_LOG_BUFFER_SIZE];
    int Written;

    Written = _vsnprintf(Buffer, sizeof(Buffer) - 1, Log->Format, Log->Arguments);
    if (Written < 0 || Written > (int)(sizeof(Buffer) - 1))
    {
        RtlStringCbPrintfA(Buffer,
                           sizeof(Buffer),
                           "<<Error while formatting the message, Status = %08x>>",
                           STATUS_BUFFER_OVERFLOW);
    }
    else
    {
        Buffer[Written] = ANSI_NULL;
    }

    DbgPrintEx(DPFLTR_IHVDRIVER_ID, Log->Level, "%s", Buffer);
}

static
NTSTATUS
__cdecl
WtrLogMessage(
    _In_ WTR_CLIENT Client,
    _In_ WDFDEVICE Device,
    _In_ ULONG Level,
    _In_z_ _Printf_format_string_ PCSTR Format,
    ...)
{
    WTR_LOG_PARAMETERS Log;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(Client);
    UNREFERENCED_PARAMETER(Device);

    Log.Level = Level;
    Log.Format = Format;
    va_start(Log.Arguments, Format);

    /* Clients log from deep stacks, so the formatting gets fresh stack */
    Status = KeExpandKernelStackAndCalloutEx(WtrLogWorker, &Log, WTR_LOG_STACK_SIZE, FALSE, NULL);

    va_end(Log.Arguments);
    return Status;
}

static
const GUID *
NTAPI
WtrMapOsServiceId(
    _In_ const GUID *OsServiceId)
{
    if (IsEqualGUID(OsServiceId, &WTR_DEPRECATED_RPMB_ID))
        return &GUID_DEVINTERFACE_EMMC_PARTITION_ACCESS_RPMB;

    return OsServiceId;
}

static
NTSTATUS
NTAPI
WtrQueryOsService(
    _In_ WTR_CLIENT Client,
    _In_ WDFDEVICE ServiceDevice,
    _In_ const GUID *OsServiceId,
    _Out_ PWTR_OS_SERVICE_INFORMATION Information)
{
    PWTR_SERVICE_CONTEXT Service = WtrGetServiceContext(ServiceDevice);

    UNREFERENCED_PARAMETER(Client);

    DPRINT1("WTR: query OS service {%08lx-%04x-%04x}\n", OsServiceId->Data1, OsServiceId->Data2, OsServiceId->Data3);

    if (!Service)
        return STATUS_INVALID_PARAMETER;

    return WtrOsServiceQuery(Service->MasterDevice, WtrMapOsServiceId(OsServiceId), Information);
}

static
NTSTATUS
NTAPI
WtrCallOsServiceEntry(
    _In_ WTR_CLIENT Client,
    _In_ WDFDEVICE ServiceDevice,
    _In_ const GUID *OsServiceId,
    _In_ PWTR_SERVICE_REQUEST Request,
    _Out_opt_ PULONG64 Information)
{
    UNREFERENCED_PARAMETER(Client);

    DPRINT1("WTR: call OS service {%08lx-%04x-%04x} function 0x%lx\n", OsServiceId->Data1, OsServiceId->Data2, OsServiceId->Data3, Request->FunctionCode);

    return WtrCallOsService(ServiceDevice, WtrMapOsServiceId(OsServiceId), Request, Information);
}

static
NTSTATUS
NTAPI
WtrCompleteAsyncRequest(
    _In_ WTR_CLIENT Client,
    _In_ WDFREQUEST Request,
    _In_ NTSTATUS Status,
    _In_ ULONG64 Information)
{
    PWTR_EXECUTE_RESULT Result = NULL;

    UNREFERENCED_PARAMETER(Client);

    if (NT_SUCCESS(WdfRequestRetrieveOutputBuffer(Request, sizeof(*Result), (PVOID *)&Result, NULL)))
    {
        RtlZeroMemory(Result, sizeof(*Result));
        Result->Version = 1;
        Result->Information = Information;
    }

    WtrCompleteSessionRequest(Request, Status, sizeof(*Result));
    return STATUS_SUCCESS;
}

/* Opening \Device\WindowsTrustedRT\{service} lands on that service's PDO */
static
NTSTATUS
NTAPI
WtrEvtControlCreate(
    _In_ WDFDEVICE Device,
    _Inout_ PIRP Irp)
{
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    PFILE_OBJECT FileObject = Stack->FileObject;
    PWTR_SERVICE_ENTRY Entry;
    UNICODE_STRING Name;
    WDFMEMORY Memory;
    size_t NameSize = 0;
    PVOID NameBuffer;
    GUID ServiceId;
    ULONG Count;
    ULONG Index;
    NTSTATUS Status;

    if (!FileObject || !FileObject->FileName.Length || !FileObject->FileName.Buffer)
        goto Forward;

    Name = FileObject->FileName;
    if (Name.Length > sizeof(WCHAR) && Name.Buffer[0] == L'\\')
    {
        Name.Buffer++;
        Name.Length -= sizeof(WCHAR);
        Name.MaximumLength -= sizeof(WCHAR);
    }

    if (!NT_SUCCESS(RtlGUIDFromString(&Name, &ServiceId)))
        goto Forward;

    Status = STATUS_NOT_FOUND;
    Count = WdfCollectionGetCount(WtrSecureServices);
    for (Index = 0; Index < Count; Index++)
    {
        Entry = WtrGetServiceEntry(WdfCollectionGetItem(WtrSecureServices, Index));
        if (!IsEqualGUID(&Entry->Descriptor.ServiceId, &ServiceId))
            continue;

        if (!Entry->Pdo || !Entry->ServiceDevice)
        {
            Status = STATUS_DEVICE_NOT_READY;
            break;
        }

        Status = WdfDeviceAllocAndQueryProperty(Entry->ServiceDevice,
                                                DevicePropertyPhysicalDeviceObjectName,
                                                NonPagedPoolNx,
                                                WDF_NO_OBJECT_ATTRIBUTES,
                                                &Memory);
        if (!NT_SUCCESS(Status))
            break;

        NameBuffer = WdfMemoryGetBuffer(Memory, &NameSize);
        if (NameSize < sizeof(WCHAR) || NameSize - sizeof(WCHAR) > MAXUSHORT - sizeof(WCHAR))
        {
            Status = STATUS_UNSUCCESSFUL;
        }
        else
        {
            Status = IoReplaceFileObjectName(FileObject, NameBuffer, (USHORT)(NameSize - sizeof(WCHAR)));
            if (NT_SUCCESS(Status))
                Status = STATUS_REPARSE;
        }

        WdfObjectDelete(Memory);
        break;
    }

    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = (Status == STATUS_REPARSE) ? IO_REPARSE_GLOBAL : 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;

Forward:
    IoSkipCurrentIrpStackLocation(Irp);
    return WdfDeviceWdmDispatchPreprocessedIrp(Device, Irp);
}

/* GUIDs of all secure services, 16 bytes each */
VOID
NTAPI
WtrEnumerateServiceIds(
    _In_ WDFREQUEST Request)
{
    PWTR_SERVICE_ENTRY Entry;
    ULONG_PTR Information = 0;
    GUID *Ids = NULL;
    ULONG Count;
    ULONG Index;
    NTSTATUS Status;

    Count = WdfCollectionGetCount(WtrSecureServices);
    Status = WdfRequestRetrieveOutputBuffer(Request, sizeof(GUID) * Count, (PVOID *)&Ids, NULL);
    if (NT_SUCCESS(Status))
    {
        for (Index = 0; Index < Count; Index++)
        {
            Entry = WtrGetServiceEntry(WdfCollectionGetItem(WtrSecureServices, Index));
            Ids[Index] = Entry->Descriptor.ServiceId;
        }
        Information = sizeof(GUID) * Count;
    }
    else if (Status == STATUS_BUFFER_TOO_SMALL)
    {
        Status = STATUS_BUFFER_OVERFLOW;
        Information = sizeof(GUID) * Count;
    }

    WdfRequestCompleteWithInformation(Request, Status, Information);
}

static
VOID
NTAPI
WtrEvtControlIoControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode)
{
    UNREFERENCED_PARAMETER(Queue);
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    if (IoControlCode == IOCTL_WTR_ENUMERATE_SERVICES)
        WtrEnumerateServiceIds(Request);
    else
        WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST);
}

static
NTSTATUS
NTAPI
WtrCreateControlDevice(
    _In_ WDFDRIVER Driver)
{
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDF_FILEOBJECT_CONFIG FileConfig;
    WDF_IO_QUEUE_CONFIG QueueConfig;
    PWDFDEVICE_INIT DeviceInit;
    WDFQUEUE Queue;
    NTSTATUS Status;

    PAGED_CODE();

    WtrControlDevice = NULL;

    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ParentObject = Driver;
    Status = WdfCollectionCreate(&Attributes, &WtrSecureServices);
    if (!NT_SUCCESS(Status))
        return Status;

    DeviceInit = WdfControlDeviceInitAllocate(Driver, &WtrControlSddl);
    if (!DeviceInit)
        return STATUS_INSUFFICIENT_RESOURCES;

    WdfDeviceInitSetIoType(DeviceInit, WdfDeviceIoBuffered);

    Status = WdfDeviceInitAssignName(DeviceInit, &WtrControlDeviceName);
    if (!NT_SUCCESS(Status))
        goto Fail;

    WDF_FILEOBJECT_CONFIG_INIT(&FileConfig, WDF_NO_EVENT_CALLBACK, WDF_NO_EVENT_CALLBACK, WDF_NO_EVENT_CALLBACK);
    FileConfig.FileObjectClass = WdfFileObjectWdfCanUseFsContext | WdfFileObjectCanBeOptional;
    WdfDeviceInitSetFileObjectConfig(DeviceInit, &FileConfig, WDF_NO_OBJECT_ATTRIBUTES);

    Status = WdfDeviceInitAssignWdmIrpPreprocessCallback(DeviceInit, WtrEvtControlCreate, IRP_MJ_CREATE, NULL, 0);
    if (!NT_SUCCESS(Status))
        goto Fail;

    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ExecutionLevel = WdfExecutionLevelPassive;
    Status = WdfDeviceCreate(&DeviceInit, &Attributes, &WtrControlDevice);
    if (!NT_SUCCESS(Status))
        goto Fail;

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&QueueConfig, WdfIoQueueDispatchSequential);
    QueueConfig.EvtIoDeviceControl = WtrEvtControlIoControl;
    WdfIoQueueCreate(WtrControlDevice, &QueueConfig, WDF_NO_OBJECT_ATTRIBUTES, &Queue);

    Status = WdfDeviceCreateSymbolicLink(WtrControlDevice, &WtrControlLinkName);
    if (!NT_SUCCESS(Status))
        goto Fail;

    WdfControlFinishInitializing(WtrControlDevice);
    return STATUS_SUCCESS;

Fail:
    if (WtrControlDevice)
    {
        WdfObjectDelete(WtrControlDevice);
        WtrControlDevice = NULL;
    }
    if (DeviceInit)
        WdfDeviceInitFree(DeviceInit);
    return Status;
}

static
VOID
NTAPI
WtrEvtDriverUnload(
    _In_ WDFDRIVER Driver)
{
    UNREFERENCED_PARAMETER(Driver);

    WdfObjectDelete(WtrSecureServices);
}

NTSTATUS
NTAPI
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG Config;
    WDFDRIVER Driver;
    NTSTATUS Status;

    WDF_DRIVER_CONFIG_INIT(&Config, WDF_NO_EVENT_CALLBACK);
    Config.EvtDriverUnload = WtrEvtDriverUnload;
    Config.DriverPoolTag = WTR_POOL_TAG;

    Status = WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &Config, &Driver);
    if (NT_SUCCESS(Status))
    {
        Status = WtrCreateControlDevice(Driver);
        if (NT_SUCCESS(Status))
            Status = WdfRegisterClassLibrary(&WtrClassLibraryInfo, RegistryPath, (PUNICODE_STRING)&WtrControlDeviceName);
    }

    DPRINT1("WTR: class library loaded, status 0x%lx\n", Status);

    if (!NT_SUCCESS(Status) && WtrControlDevice)
    {
        WdfObjectDelete(WtrControlDevice);
        WtrControlDevice = NULL;
    }

    return Status;
}
