/*
 * PROJECT:     ReactOS Trusted Runtime class extension
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Device that talks to the secure environment and its service children
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#include "wtrp.h"

#define NDEBUG
#include <debug.h>

typedef NTSTATUS NTAPI WTR_IO_RESERVE_DEPENDENCY(_In_ PDEVICE_OBJECT Device, _In_ PCUNICODE_STRING DependencyId, _In_ ULONG Flags);

#define WTR_DEPENDENCY_POWER        2

DECLARE_CONST_UNICODE_STRING(WtrSecureServiceId, L"{4E815EE1-20F8-41EF-8CFF-3C283F02D722}\\SecureService");

static PVOID WtrRpmbNotificationEntry;

static EVT_WDF_DEVICE_PREPARE_HARDWARE WtrEvtPrepareHardware;
static EVT_WDF_DEVICE_RELEASE_HARDWARE WtrEvtReleaseHardware;
static EVT_WDF_DEVICE_D0_ENTRY WtrEvtD0Entry;
static EVT_WDF_DEVICE_D0_EXIT WtrEvtD0Exit;
static EVT_WDF_OBJECT_CONTEXT_DESTROY WtrEvtMasterDestroy;
static EVT_WDF_DEVICE_FILE_CREATE WtrEvtMasterCreate;
static EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL WtrEvtMasterIoControl;
static EVT_WDF_IO_QUEUE_IO_INTERNAL_DEVICE_CONTROL WtrEvtMasterInternalIoControl;
static EVT_WDF_IO_QUEUE_IO_INTERNAL_DEVICE_CONTROL WtrEvtSerializeRequest;
static EVT_WDF_CHILD_LIST_CREATE_DEVICE WtrEvtCreateServicePdo;
static EVT_WDF_CHILD_LIST_IDENTIFICATION_DESCRIPTION_DUPLICATE WtrEvtChildIdDuplicate;
static EVT_WDF_CHILD_LIST_IDENTIFICATION_DESCRIPTION_COMPARE WtrEvtChildIdCompare;
static EVT_WDF_CHILD_LIST_IDENTIFICATION_DESCRIPTION_CLEANUP WtrEvtChildIdCleanup;
static EVT_WDFDEVICE_WDM_IRP_PREPROCESS WtrEvtQueryDeviceRelations;

typedef struct _WTR_OTHER_IO_CALL
{
    WTR_EVT_DEVICE_OTHER_IO *Callback;
    WDFDEVICE Device;
    WDFREQUEST Request;
} WTR_OTHER_IO_CALL, *PWTR_OTHER_IO_CALL;

static
PWTR_SERVICE_ENTRY
NTAPI
WtrFindServiceEntry(
    _In_ PWTR_MASTER_CONTEXT Master,
    _In_ const GUID *ServiceId)
{
    PWTR_SERVICE_ENTRY Entry;
    ULONG Count = WdfCollectionGetCount(Master->Services);
    ULONG Index;

    for (Index = 0; Index < Count; Index++)
    {
        Entry = WtrGetServiceEntry(WdfCollectionGetItem(Master->Services, Index));
        if (IsEqualGUID(&Entry->Descriptor.ServiceId, ServiceId))
            return Entry;
    }

    return NULL;
}

/* A malformed extension is dropped rather than failing the service */
static
VOID
NTAPI
WtrCheckExtension(
    _Inout_ PWTR_SERVICE_DESCRIPTOR Descriptor,
    _In_ ULONG Size)
{
    ULONG Offset = Descriptor->ExtensionOffset;

    if (!Offset)
        return;

    if (Offset >= FIELD_OFFSET(WTR_SERVICE_DESCRIPTOR, Dependencies) + Descriptor->DependencyCount * sizeof(WTR_SERVICE_DEPENDENCY) &&
        Offset < Size - sizeof(WTR_SERVICE_EXTENSION))
    {
        return;
    }

    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
               "Secure service (%08lx) descriptor's ExtensionOffset field is not valid.\n",
               Descriptor->ServiceId.Data1);
    Descriptor->ExtensionOffset = 0;
}

static
NTSTATUS
NTAPI
WtrAddServiceEntry(
    _In_ WDFDEVICE Device,
    _In_ PWTR_MASTER_CONTEXT Master,
    _In_ PWTR_SERVICE_DESCRIPTOR Descriptor)
{
    WDF_OBJECT_ATTRIBUTES Attributes;
    PWTR_SERVICE_DEPENDENCY Dependency;
    PWTR_SERVICE_ENTRY Entry;
    WDFOBJECT Handle;
    ULONG Index;
    NTSTATUS Status;

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, WTR_SERVICE_ENTRY);
    Attributes.ParentObject = Device;
    if (Descriptor->Size + FIELD_OFFSET(WTR_SERVICE_ENTRY, Descriptor) > sizeof(WTR_SERVICE_ENTRY))
        Attributes.ContextSizeOverride = Descriptor->Size + FIELD_OFFSET(WTR_SERVICE_ENTRY, Descriptor);

    Status = WdfObjectCreate(&Attributes, &Handle);
    if (!NT_SUCCESS(Status))
        return Status;

    Entry = WtrGetServiceEntry(Handle);
    RtlCopyMemory(&Entry->Descriptor, Descriptor, Descriptor->Size);

    for (Index = 0; Index < Entry->Descriptor.DependencyCount; Index++)
    {
        Dependency = &Entry->Descriptor.Dependencies[Index];
        if (Dependency->Type > WTR_DEPENDENCY_DEVICE)
            break;

        if (Dependency->Type == WTR_DEPENDENCY_OS_SERVICE &&
            IsEqualGUID(&Dependency->Id, &WTR_DEPRECATED_RPMB_ID))
        {
            DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_WARNING_LEVEL,
                       "GUID_TREE_RPMB_SERVICE is deprecated. Please use GUID_DEVINTERFACE_EMMC_PARTITION_ACCESS_RPMB instead.\n");
            Dependency->Id = GUID_DEVINTERFACE_EMMC_PARTITION_ACCESS_RPMB;
        }
    }

    Status = WdfCollectionAdd(Master->Services, Handle);
    if (NT_SUCCESS(Status))
        Status = WdfCollectionAdd(WtrSecureServices, Handle);

    return Status;
}

/* Asks the client for each service descriptor until it runs out */
static
NTSTATUS
NTAPI
WtrDetectSecureServices(
    _In_ WDFDEVICE Device)
{
    PWTR_MASTER_CONTEXT Master = WtrGetMasterContext(Device);
    PWTR_SERVICE_DESCRIPTOR Descriptor = NULL;
    ULONG BufferSize = 0;
    ULONG Size;
    ULONG Index = 0;
    NTSTATUS Status;

    for (;;)
    {
        Size = BufferSize;
        if (Master->Callbacks.EvtDeviceQueryService)
            Status = Master->Callbacks.EvtDeviceQueryService(Device, Index, Descriptor, &Size);
        else
            Status = STATUS_NOT_SUPPORTED;

        if (Status == STATUS_BUFFER_OVERFLOW || Status == STATUS_BUFFER_TOO_SMALL)
        {
            if (Descriptor)
                ExFreePoolWithTag(Descriptor, WTR_POOL_TAG);

            Descriptor = ExAllocatePoolWithTag(NonPagedPoolNx, Size, WTR_POOL_TAG);
            BufferSize = Descriptor ? Size : 0;
            if (!Descriptor)
            {
                Status = STATUS_INSUFFICIENT_RESOURCES;
                break;
            }
            continue;
        }

        if (!NT_SUCCESS(Status))
        {
            DPRINT1("WTR: client service query %lu ended with 0x%lx\n", Index, Status);
            break;
        }

        if (!Descriptor)
        {
            Status = STATUS_INVALID_BUFFER_SIZE;
            break;
        }

        if (Descriptor->Size != Size || Size < FIELD_OFFSET(WTR_SERVICE_DESCRIPTOR, Dependencies) ||
            Descriptor->DependencyCount > (Size - FIELD_OFFSET(WTR_SERVICE_DESCRIPTOR, Dependencies)) / sizeof(WTR_SERVICE_DEPENDENCY))
        {
            Status = STATUS_INVALID_PARAMETER;
            break;
        }

        DPRINT1("WTR: service %lu {%08lx-%04x-%04x} v%lu.%lu, %lu dependencies\n", Index, Descriptor->ServiceId.Data1, Descriptor->ServiceId.Data2, Descriptor->ServiceId.Data3, Descriptor->MajorVersion, Descriptor->MinorVersion, Descriptor->DependencyCount);

        WtrCheckExtension(Descriptor, Size);

        Status = WtrAddServiceEntry(Device, Master, Descriptor);
        if (!NT_SUCCESS(Status))
            break;

        Index++;
    }

    /* Running out of services is the normal way out */
    if (Status == STATUS_NO_MORE_ENTRIES)
        Status = STATUS_SUCCESS;
    else if (NT_SUCCESS(Status))
        Status = STATUS_UNSUCCESSFUL;

    if (Descriptor)
        ExFreePoolWithTag(Descriptor, WTR_POOL_TAG);

    return Status;
}

static
NTSTATUS
NTAPI
WtrConfigureSecureServices(
    _In_ WDFDEVICE Device)
{
    PWTR_MASTER_CONTEXT Master = WtrGetMasterContext(Device);
    PWTR_SERVICE_ENTRY Entry;
    WTR_CHILD_ID ChildId;
    ULONG Count;
    ULONG Index;
    NTSTATUS Status;

    Status = WtrDetectSecureServices(Device);
    if (!NT_SUCCESS(Status))
        return Status;

    Count = WdfCollectionGetCount(Master->Services);
    for (Index = 0; Index < Count; Index++)
    {
        Entry = WtrGetServiceEntry(WdfCollectionGetItem(Master->Services, Index));

        WDF_CHILD_IDENTIFICATION_DESCRIPTION_HEADER_INIT(&ChildId.Header, sizeof(ChildId));
        ChildId.ServiceId = Entry->Descriptor.ServiceId;

        Status = WdfChildListAddOrUpdateChildDescriptionAsPresent(WdfFdoGetDefaultChildList(Device),
                                                                  &ChildId.Header,
                                                                  NULL);
        if (!NT_SUCCESS(Status))
            break;
    }

    return Status;
}

static
NTSTATUS
NTAPI
WtrEvtPrepareHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesRaw,
    _In_ WDFCMRESLIST ResourcesTranslated)
{
    PWTR_MASTER_CONTEXT Master = WtrGetMasterContext(Device);

    if (!Master->Callbacks.EvtDevicePrepareHardware)
        return STATUS_SUCCESS;

    return Master->Callbacks.EvtDevicePrepareHardware(Device, ResourcesRaw, ResourcesTranslated);
}

static
NTSTATUS
NTAPI
WtrEvtReleaseHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesTranslated)
{
    PWTR_MASTER_CONTEXT Master = WtrGetMasterContext(Device);

    if (!Master->Callbacks.EvtDeviceReleaseHardware)
        return STATUS_SUCCESS;

    return Master->Callbacks.EvtDeviceReleaseHardware(Device, ResourcesTranslated);
}

static
NTSTATUS
NTAPI
WtrEvtD0Entry(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE PreviousState)
{
    PWTR_MASTER_CONTEXT Master = WtrGetMasterContext(Device);
    NTSTATUS Status = STATUS_SUCCESS;

    /* The secure environment cannot be powered down under the client, hold the device in D0 */
    if (!InterlockedOr(&Master->IdleHeld, 1))
    {
        Status = WdfDeviceStopIdle(Device, FALSE);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    DPRINT1("WTR: master D0 entry from %d\n", PreviousState);

    if (PreviousState < WdfPowerDeviceD3)
        return STATUS_SUCCESS;

    WtrGetOsServices(Device)->Powered = TRUE;

    if (Master->Callbacks.EvtDeviceD0Entry)
        Status = Master->Callbacks.EvtDeviceD0Entry(Device);

    if (!Master->ServicesConfigured)
    {
        Master->ServicesConfigured = TRUE;
        Status = WtrConfigureSecureServices(Device);
        DPRINT1("WTR: secure services configured, %lu found, status 0x%lx\n", WdfCollectionGetCount(Master->Services), Status);
    }

    return Status;
}

static
NTSTATUS
NTAPI
WtrEvtD0Exit(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE TargetState)
{
    PWTR_MASTER_CONTEXT Master = WtrGetMasterContext(Device);

    UNREFERENCED_PARAMETER(TargetState);

    WtrGetOsServices(Device)->Powered = FALSE;
    DPRINT1("WTR: master D0 exit to %d\n", TargetState);

    if (!Master->Callbacks.EvtDeviceD0Exit)
        return STATUS_SUCCESS;

    return Master->Callbacks.EvtDeviceD0Exit(Device);
}

static
VOID
NTAPI
WtrOtherIoWorker(
    _In_ PVOID Parameter)
{
    PWTR_OTHER_IO_CALL Call = Parameter;

    Call->Callback(Call->Device, Call->Request);
}

static
VOID
NTAPI
WtrEvtMasterIoControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode)
{
    WDFDEVICE Device = WdfIoQueueGetDevice(Queue);
    PWTR_MASTER_CONTEXT Master = WtrGetMasterContext(Device);
    WDF_REQUEST_PARAMETERS Parameters;
    WTR_OTHER_IO_CALL Call;

    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);
    UNREFERENCED_PARAMETER(IoControlCode);

    WDF_REQUEST_PARAMETERS_INIT(&Parameters);
    WdfRequestGetParameters(Request, &Parameters);

    if (!Master->Callbacks.EvtDeviceOtherIo)
    {
        WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST);
        return;
    }

    if (!Master->StackSize)
    {
        Master->Callbacks.EvtDeviceOtherIo(Device, Request);
        return;
    }

    Call.Callback = Master->Callbacks.EvtDeviceOtherIo;
    Call.Device = Device;
    Call.Request = Request;
    WtrExpandStack(WtrOtherIoWorker, &Call, Master->StackSize);
}

static
VOID
NTAPI
WtrEvtMasterInternalIoControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode)
{
    PWTR_MASTER_FILE_CONTEXT FileContext;
    WDFFILEOBJECT FileObject;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    FileObject = WdfRequestGetFileObject(Request);
    if (FileObject)
    {
        FileContext = WtrGetMasterFileContext(FileObject);
        if (FileContext->Queue)
            Status = WdfRequestForwardToIoQueue(Request, FileContext->Queue);
        else
            Status = STATUS_INVALID_DEVICE_REQUEST;
    }
    else if (IoControlCode == IOCTL_WTR_SERIALIZE_REQUEST)
    {
        Status = WdfRequestForwardToIoQueue(Request, WtrGetMasterContext(WdfIoQueueGetDevice(Queue))->SerializeQueue);
        if (NT_SUCCESS(Status))
            return;
    }
    else
    {
        Status = STATUS_INVALID_DEVICE_REQUEST;
    }

    if (!NT_SUCCESS(Status))
        WdfRequestComplete(Request, Status);
}

/* Runs one session request at a time on behalf of serialized clients */
static
VOID
NTAPI
WtrEvtSerializeRequest(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode)
{
    WDF_REQUEST_PARAMETERS Parameters;
    WDFREQUEST Original;

    UNREFERENCED_PARAMETER(Queue);
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);
    UNREFERENCED_PARAMETER(IoControlCode);

    WDF_REQUEST_PARAMETERS_INIT(&Parameters);
    WdfRequestGetParameters(Request, &Parameters);

    Original = (WDFREQUEST)Parameters.Parameters.Others.Arg1;
    WtrGetRequestContext(Original)->Forwarded = Request;
    WtrExecuteSessionRequest(Original);
}

/* The master itself has no namespace */
static
VOID
NTAPI
WtrEvtMasterCreate(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ WDFFILEOBJECT FileObject)
{
    PCUNICODE_STRING Name = WdfFileObjectGetFileName(FileObject);
    PWCHAR Buffer = Name->Buffer;
    USHORT Length = Name->Length;

    UNREFERENCED_PARAMETER(Device);

    if (Length && Buffer && Buffer[0] == L'\\')
    {
        Length -= sizeof(WCHAR);
        Buffer++;
    }

    WdfRequestComplete(Request, (Length && Buffer) ? STATUS_NO_SUCH_DEVICE : STATUS_SUCCESS);
}

static
VOID
NTAPI
WtrEvtMasterDestroy(
    _In_ WDFOBJECT Object)
{
    WDFDEVICE Device = (WDFDEVICE)Object;
    PWTR_MASTER_CONTEXT Master = WtrGetMasterContext(Device);
    ULONG Count = WdfCollectionGetCount(Master->Services);
    ULONG Index;

    for (Index = 0; Index < Count; Index++)
        WdfCollectionRemove(WtrSecureServices, WdfCollectionGetItem(Master->Services, Index));

    if (Master->Callbacks.EvtDeviceDestroy)
        Master->Callbacks.EvtDeviceDestroy(Device);
}

/* Ties the service FDO to the entry its PDO was created from */
NTSTATUS
NTAPI
WtrReportServiceDevice(
    _In_ WDFDEVICE ServiceDevice,
    _Out_ WDFDEVICE *MasterDevice,
    _Out_ PWTR_SERVICE_DESCRIPTOR *Descriptor)
{
    PWTR_SERVICE_ENTRY Entry;
    PWTR_MASTER_CONTEXT Master;
    WDFDEVICE Pdo;
    WDFDEVICE Parent;
    ULONG Count;
    ULONG Index;

    Pdo = WdfWdmDeviceGetWdfDeviceHandle(WdfDeviceWdmGetPhysicalDevice(ServiceDevice));
    Parent = WdfPdoGetParent(Pdo);
    Master = WtrGetMasterContext(Parent);

    Count = WdfCollectionGetCount(Master->Services);
    for (Index = 0; Index < Count; Index++)
    {
        Entry = WtrGetServiceEntry(WdfCollectionGetItem(Master->Services, Index));
        if (Entry->Pdo != Pdo)
            continue;

        Entry->ServiceDevice = ServiceDevice;
        *MasterDevice = Parent;
        *Descriptor = &Entry->Descriptor;
        return STATUS_SUCCESS;
    }

    DPRINT1("WTR: no service entry owns PDO %p\n", Pdo);
    for (Index = 0; Index < Count; Index++)
    {
        Entry = WtrGetServiceEntry(WdfCollectionGetItem(Master->Services, Index));
        DPRINT1("WTR:   {%08lx} PDO %p service device %p\n", Entry->Descriptor.ServiceId.Data1, Entry->Pdo, Entry->ServiceDevice);
    }

    return STATUS_NOT_FOUND;
}

static
NTSTATUS
NTAPI
WtrReserveDependencies(
    _In_ WDFDEVICE Master,
    _In_ WDFDEVICE Pdo,
    _In_ PWTR_SERVICE_ENTRY Entry)
{
    static UNICODE_STRING RoutineName = RTL_CONSTANT_STRING(L"IoReserveDependency");
    WTR_IO_RESERVE_DEPENDENCY *ReserveDependency;
    WTR_OS_SERVICE_INFORMATION Information;
    UNICODE_STRING IdString;
    ULONG Index;
    NTSTATUS Status = STATUS_SUCCESS;

    ReserveDependency = (WTR_IO_RESERVE_DEPENDENCY *)MmGetSystemRoutineAddress(&RoutineName);

    for (Index = 0; Index < Entry->Descriptor.DependencyCount; Index++)
    {
        Status = RtlStringFromGUID(&Entry->Descriptor.Dependencies[Index].Id, &IdString);
        if (!NT_SUCCESS(Status))
            break;

        if (ReserveDependency)
            Status = ReserveDependency(WdfDeviceWdmGetDeviceObject(Pdo), &IdString, WTR_DEPENDENCY_POWER);
        RtlFreeUnicodeString(&IdString);
        if (!NT_SUCCESS(Status))
            break;

        /* Registers the OS service so it is tracked from now on */
        Status = WtrOsServiceQuery(Master, &Entry->Descriptor.Dependencies[Index].Id, &Information);
        if (Status == STATUS_DEVICE_NOT_CONNECTED)
            Status = STATUS_SUCCESS;
        if (!NT_SUCCESS(Status))
            break;
    }

    return Status;
}

static
NTSTATUS
NTAPI
WtrEvtCreateServicePdo(
    _In_ WDFCHILDLIST ChildList,
    _In_ PWDF_CHILD_IDENTIFICATION_DESCRIPTION_HEADER IdentificationDescription,
    _In_ PWDFDEVICE_INIT ChildInit)
{
    PWTR_CHILD_ID ChildId = CONTAINING_RECORD(IdentificationDescription, WTR_CHILD_ID, Header);
    WDFDEVICE Master = WdfChildListGetDevice(ChildList);
    WDF_DEVICE_PNP_CAPABILITIES PnpCaps;
    PWTR_SERVICE_ENTRY Entry;
    UCHAR Minor = IRP_MN_QUERY_DEVICE_RELATIONS;
    DECLARE_UNICODE_STRING_SIZE(IdString, 38);
    DECLARE_UNICODE_STRING_SIZE(DeviceId, 100);
    WDFDEVICE Pdo;
    NTSTATUS Status;

    Status = RtlUnicodeStringPrintf(&IdString,
                                    L"{%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}",
                                    ChildId->ServiceId.Data1,
                                    ChildId->ServiceId.Data2,
                                    ChildId->ServiceId.Data3,
                                    ChildId->ServiceId.Data4[0],
                                    ChildId->ServiceId.Data4[1],
                                    ChildId->ServiceId.Data4[2],
                                    ChildId->ServiceId.Data4[3],
                                    ChildId->ServiceId.Data4[4],
                                    ChildId->ServiceId.Data4[5],
                                    ChildId->ServiceId.Data4[6],
                                    ChildId->ServiceId.Data4[7]);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = RtlUnicodeStringPrintf(&DeviceId, L"{4E815EE1-20F8-41EF-8CFF-3C283F02D722}\\%wZ", &IdString);
    if (!NT_SUCCESS(Status))
        return Status;

    DPRINT1("WTR: creating service PDO %wZ\n", &DeviceId);

    Status = WdfPdoInitAssignDeviceID(ChildInit, &DeviceId);
    if (NT_SUCCESS(Status))
        Status = WdfPdoInitAddHardwareID(ChildInit, &DeviceId);
    if (NT_SUCCESS(Status))
        Status = WdfPdoInitAddCompatibleID(ChildInit, &WtrSecureServiceId);
    if (NT_SUCCESS(Status))
        Status = WdfDeviceInitAssignWdmIrpPreprocessCallback(ChildInit, WtrEvtQueryDeviceRelations, IRP_MJ_PNP, &Minor, 1);
    if (!NT_SUCCESS(Status))
        return Status;

    WdfDeviceInitSetPowerNotPageable(ChildInit);

    Status = WdfDeviceCreate(&ChildInit, WDF_NO_OBJECT_ATTRIBUTES, &Pdo);
    if (!NT_SUCCESS(Status))
        return Status;

    Entry = WtrFindServiceEntry(WtrGetMasterContext(Master), &ChildId->ServiceId);
    if (!Entry)
        return STATUS_NOT_FOUND;

    Entry->ServiceDevice = NULL;
    Entry->Pdo = Pdo;

    WDF_DEVICE_PNP_CAPABILITIES_INIT(&PnpCaps);
    PnpCaps.EjectSupported = WdfFalse;
    PnpCaps.Removable = WdfFalse;
    PnpCaps.SurpriseRemovalOK = WdfFalse;
    WdfDeviceSetPnpCapabilities(Pdo, &PnpCaps);

    Status = WtrReserveDependencies(Master, Pdo, Entry);
    DPRINT1("WTR: service PDO dependencies -> 0x%lx\n", Status);
    return Status;
}

static
BOOLEAN
NTAPI
WtrEvtChildIdCompare(
    _In_ WDFCHILDLIST ChildList,
    _In_ PWDF_CHILD_IDENTIFICATION_DESCRIPTION_HEADER First,
    _In_ PWDF_CHILD_IDENTIFICATION_DESCRIPTION_HEADER Second)
{
    UNREFERENCED_PARAMETER(ChildList);

    return IsEqualGUID(&CONTAINING_RECORD(First, WTR_CHILD_ID, Header)->ServiceId,
                       &CONTAINING_RECORD(Second, WTR_CHILD_ID, Header)->ServiceId);
}

static
NTSTATUS
NTAPI
WtrEvtChildIdDuplicate(
    _In_ WDFCHILDLIST ChildList,
    _In_ PWDF_CHILD_IDENTIFICATION_DESCRIPTION_HEADER Source,
    _Out_ PWDF_CHILD_IDENTIFICATION_DESCRIPTION_HEADER Destination)
{
    UNREFERENCED_PARAMETER(ChildList);

    *CONTAINING_RECORD(Destination, WTR_CHILD_ID, Header) = *CONTAINING_RECORD(Source, WTR_CHILD_ID, Header);
    return STATUS_SUCCESS;
}

static
VOID
NTAPI
WtrEvtChildIdCleanup(
    _In_ WDFCHILDLIST ChildList,
    _Inout_ PWDF_CHILD_IDENTIFICATION_DESCRIPTION_HEADER Description)
{
    PWTR_CHILD_ID ChildId = CONTAINING_RECORD(Description, WTR_CHILD_ID, Header);
    PWTR_SERVICE_ENTRY Entry;

    Entry = WtrFindServiceEntry(WtrGetMasterContext(WdfChildListGetDevice(ChildList)), &ChildId->ServiceId);
    if (!Entry)
        return;

    Entry->ServiceDevice = NULL;
    Entry->Pdo = NULL;
}

/* Physical device behind an OS service the secure service depends on */
static
NTSTATUS
NTAPI
WtrGetDependencyDevice(
    _In_ WDFDEVICE Master,
    _In_ const GUID *Id,
    _Out_ PDEVICE_OBJECT *Device)
{
    WDF_IO_TARGET_OPEN_PARAMS OpenParams;
    UNICODE_STRING Link;
    WDFIOTARGET Target;
    WDFOBJECT Handle;
    PWTR_PLUGIN Plugin;
    NTSTATUS Status;

    *Device = NULL;

    if (!WtrFindPlugin(WtrGetOsServices(Master), NULL, Id, &Handle, &Plugin))
        return STATUS_DEVICE_NOT_READY;

    if (!Plugin->SymbolicLink)
    {
        Status = STATUS_DEVICE_NOT_READY;
        goto Done;
    }

    Status = WdfIoTargetCreate(Master, WDF_NO_OBJECT_ATTRIBUTES, &Target);
    if (!NT_SUCCESS(Status))
        goto Done;

    WdfStringGetUnicodeString(Plugin->SymbolicLink, &Link);
    WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(&OpenParams, &Link, STANDARD_RIGHTS_ALL);
    Status = WdfIoTargetOpen(Target, &OpenParams);
    if (NT_SUCCESS(Status))
    {
        *Device = WdfIoTargetWdmGetTargetPhysicalDevice(Target);
        WdfIoTargetClose(Target);
    }
    WdfObjectDelete(Target);

Done:
    if (!NT_SUCCESS(Status))
        DPRINT1("WTR: dependency {%08lx-%04x-%04x} device unavailable 0x%lx\n", Id->Data1, Id->Data2, Id->Data3, Status);
    WdfObjectDereference(Handle);
    return Status;
}

/* Service PDOs report the devices backing their OS services as power relations */
static
NTSTATUS
NTAPI
WtrEvtQueryDeviceRelations(
    _In_ WDFDEVICE Device,
    _Inout_ PIRP Irp)
{
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    PDEVICE_RELATIONS Existing = NULL;
    PDEVICE_RELATIONS Relations = NULL;
    PWTR_SERVICE_ENTRY Entry;
    PDEVICE_OBJECT Related;
    WTR_CHILD_ID ChildId;
    WDFDEVICE Master;
    ULONG First = 0;
    ULONG Count;
    ULONG Total;
    ULONG Index;
    NTSTATUS Status;

    if (Stack->Parameters.QueryDeviceRelations.Type != PowerRelations)
        goto Forward;

    if (NT_SUCCESS(Irp->IoStatus.Status))
        Existing = (PDEVICE_RELATIONS)Irp->IoStatus.Information;

    WDF_CHILD_IDENTIFICATION_DESCRIPTION_HEADER_INIT(&ChildId.Header, sizeof(ChildId));
    Status = WdfPdoRetrieveIdentificationDescription(Device, &ChildId.Header);
    if (!NT_SUCCESS(Status))
        goto Done;

    Master = WdfPdoGetParent(Device);
    Entry = WtrFindServiceEntry(WtrGetMasterContext(Master), &ChildId.ServiceId);
    if (!Entry || !Entry->Descriptor.DependencyCount)
    {
        Status = STATUS_SUCCESS;
        goto Done;
    }

    Count = Entry->Descriptor.DependencyCount;
    if (Existing)
        First = Existing->Count;
    Total = First + Count;

    Relations = ExAllocatePoolWithTag(PagedPool,
                                      FIELD_OFFSET(DEVICE_RELATIONS, Objects) + Total * sizeof(PDEVICE_OBJECT),
                                      WTR_POOL_TAG);
    if (!Relations)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Done;
    }

    Relations->Count = Total;
    if (Existing)
        RtlCopyMemory(Relations->Objects, Existing->Objects, First * sizeof(PDEVICE_OBJECT));

    for (Index = 0; Index < Count; Index++)
    {
        Status = WtrGetDependencyDevice(Master, &Entry->Descriptor.Dependencies[Index].Id, &Related);
        if (!NT_SUCCESS(Status))
            goto Done;
        Relations->Objects[First + Index] = Related;
    }

    for (Index = 0; Index < Count; Index++)
        ObReferenceObject(Relations->Objects[First + Index]);

    if (Existing)
        ExFreePool(Existing);
    Irp->IoStatus.Information = (ULONG_PTR)Relations;
    Relations = NULL;
    Status = STATUS_SUCCESS;

Done:
    if (Relations)
        ExFreePoolWithTag(Relations, WTR_POOL_TAG);

    Irp->IoStatus.Status = Status;
    if (!NT_SUCCESS(Status))
    {
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return Status;
    }

Forward:
    IoSkipCurrentIrpStackLocation(Irp);
    return WdfDeviceWdmDispatchPreprocessedIrp(Device, Irp);
}

static
NTSTATUS
NTAPI
WtrSetupQueues(
    _In_ WDFDEVICE Device)
{
    PWTR_MASTER_CONTEXT Master = WtrGetMasterContext(Device);
    WDF_IO_QUEUE_FORWARD_PROGRESS_POLICY Policy;
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDF_IO_QUEUE_CONFIG Config;
    WDFQUEUE Queue;
    NTSTATUS Status;

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&Config, WdfIoQueueDispatchParallel);
    Config.EvtIoDeviceControl = WtrEvtMasterIoControl;
    Config.EvtIoInternalDeviceControl = WtrEvtMasterInternalIoControl;
    Config.Settings.Parallel.NumberOfPresentedRequests = (ULONG)-1;

    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ExecutionLevel = WdfExecutionLevelPassive;
    Attributes.SynchronizationScope = WdfSynchronizationScopeNone;

    Status = WdfIoQueueCreate(Device, &Config, &Attributes, &Queue);
    if (!NT_SUCCESS(Status))
        return Status;

    /* Forward progress is best effort, the device works without it */
    WDF_IO_QUEUE_FORWARD_PROGRESS_POLICY_DEFAULT_INIT(&Policy, 1);
    WdfIoQueueAssignForwardProgressPolicy(Queue, &Policy);

    if (Master->Mode != WTR_MODE_SERIALIZED)
        return STATUS_SUCCESS;

    WDF_IO_QUEUE_CONFIG_INIT(&Config, WdfIoQueueDispatchSequential);
    Config.EvtIoInternalDeviceControl = WtrEvtSerializeRequest;

    Status = WdfIoQueueCreate(Device, &Config, &Attributes, &Master->SerializeQueue);
    if (!NT_SUCCESS(Status))
        return Status;

    WdfIoQueueAssignForwardProgressPolicy(Master->SerializeQueue, &Policy);
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
WtrAddMasterDevice(
    _In_ PWDFDEVICE_INIT DeviceInit,
    _In_ ULONG Flags,
    _Out_ WDFDEVICE *Device)
{
    WDF_DEVICE_POWER_POLICY_IDLE_SETTINGS IdleSettings;
    WDF_PNPPOWER_EVENT_CALLBACKS PnpPowerCallbacks;
    WDF_DEVICE_POWER_CAPABILITIES PowerCaps;
    WDF_DEVICE_PNP_CAPABILITIES PnpCaps;
    WDF_IO_TARGET_OPEN_PARAMS OpenParams;
    WDF_OBJECT_ATTRIBUTES FileAttributes;
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDF_FILEOBJECT_CONFIG FileConfig;
    WDF_CHILD_LIST_CONFIG ChildConfig;
    PNP_BUS_INFORMATION BusInformation;
    PWTR_MASTER_CONTEXT Master;
    WDFDEVICE NewDevice = NULL;
    ULONG State;
    NTSTATUS Status;

    *Device = NULL;

    WdfDeviceInitSetIoType(DeviceInit, WdfDeviceIoBuffered);
    WdfDeviceInitSetDeviceType(DeviceInit, FILE_DEVICE_WTR);
    WdfDeviceInitSetPowerNotPageable(DeviceInit);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&PnpPowerCallbacks);
    PnpPowerCallbacks.EvtDevicePrepareHardware = WtrEvtPrepareHardware;
    PnpPowerCallbacks.EvtDeviceReleaseHardware = WtrEvtReleaseHardware;
    PnpPowerCallbacks.EvtDeviceD0Entry = WtrEvtD0Entry;
    PnpPowerCallbacks.EvtDeviceD0Exit = WtrEvtD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &PnpPowerCallbacks);

    WDF_CHILD_LIST_CONFIG_INIT(&ChildConfig, sizeof(WTR_CHILD_ID), WtrEvtCreateServicePdo);
    ChildConfig.EvtChildListIdentificationDescriptionDuplicate = WtrEvtChildIdDuplicate;
    ChildConfig.EvtChildListIdentificationDescriptionCompare = WtrEvtChildIdCompare;
    ChildConfig.EvtChildListIdentificationDescriptionCleanup = WtrEvtChildIdCleanup;
    WdfFdoInitSetDefaultChildListConfig(DeviceInit, &ChildConfig, WDF_NO_OBJECT_ATTRIBUTES);

    WdfDeviceInitSetReleaseHardwareOrderOnFailure(DeviceInit, WdfReleaseHardwareOrderOnFailureAfterDescendants);

    WDF_FILEOBJECT_CONFIG_INIT(&FileConfig, WtrEvtMasterCreate, WDF_NO_EVENT_CALLBACK, WDF_NO_EVENT_CALLBACK);
    FileConfig.FileObjectClass = WdfFileObjectWdfCanUseFsContext | WdfFileObjectCanBeOptional;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&FileAttributes, WTR_MASTER_FILE_CONTEXT);
    WdfDeviceInitSetFileObjectConfig(DeviceInit, &FileConfig, &FileAttributes);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, WTR_MASTER_CONTEXT);
    Attributes.EvtDestroyCallback = WtrEvtMasterDestroy;
    Attributes.SynchronizationScope = WdfSynchronizationScopeNone;

    Status = WdfDeviceCreate(&DeviceInit, &Attributes, &NewDevice);
    if (!NT_SUCCESS(Status))
        return Status;

    Master = WtrGetMasterContext(NewDevice);
    Master->Mode = Flags & WTR_MODE_MASK;
    switch (Flags & WTR_STACK_MASK)
    {
        case WTR_STACK_SMALL:
            Master->StackSize = WTR_STACK_SIZE_SMALL;
            break;

        case WTR_STACK_MEDIUM:
            Master->StackSize = WTR_STACK_SIZE_MEDIUM;
            break;

        case WTR_STACK_LARGE:
            Master->StackSize = WTR_STACK_SIZE_LARGE;
            break;

        default:
            Master->StackSize = 0;
            break;
    }

    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ParentObject = NewDevice;
    Status = WdfCollectionCreate(&Attributes, &Master->Services);
    if (!NT_SUCCESS(Status))
        goto Exit;

    BusInformation.BusTypeGuid = GUID_WTR_SECURE_SERVICE_BUS;
    BusInformation.LegacyBusType = PNPBus;
    BusInformation.BusNumber = 0;
    WdfDeviceSetBusInformationForChildren(NewDevice, &BusInformation);

    WDF_DEVICE_PNP_CAPABILITIES_INIT(&PnpCaps);
    PnpCaps.Removable = WdfFalse;
    PnpCaps.NoDisplayInUI = WdfTrue;
    WdfDeviceSetPnpCapabilities(NewDevice, &PnpCaps);

    WDF_DEVICE_POWER_CAPABILITIES_INIT(&PowerCaps);
    PowerCaps.DeviceState[PowerSystemWorking] = PowerDeviceD0;
    for (State = PowerSystemSleeping1; State <= PowerSystemSleeping3; State++)
        PowerCaps.DeviceState[State] = PowerDeviceD0;
    PowerCaps.DeviceState[PowerSystemHibernate] = PowerDeviceD3;
    PowerCaps.DeviceState[PowerSystemShutdown] = PowerDeviceD3;
    PowerCaps.SystemWake = PowerSystemMaximum;
    WdfDeviceSetPowerCapabilities(NewDevice, &PowerCaps);

    WDF_DEVICE_POWER_POLICY_IDLE_SETTINGS_INIT(&IdleSettings, IdleCannotWakeFromS0);
    IdleSettings.DxState = PowerDeviceD3;
    IdleSettings.IdleTimeoutType = SystemManagedIdleTimeout;
    Status = WdfDeviceAssignS0IdleSettings(NewDevice, &IdleSettings);
    if (!NT_SUCCESS(Status))
        goto Exit;

    Status = WtrSetupQueues(NewDevice);
    if (!NT_SUCCESS(Status))
        goto Exit;

    Status = WtrOsServicesInitialize(NewDevice);
    if (!NT_SUCCESS(Status))
        goto Exit;

    /* Serialized clients get their session requests back through their own device */
    if (Master->Mode == WTR_MODE_SERIALIZED)
    {
        Status = WdfIoTargetCreate(NewDevice, WDF_NO_OBJECT_ATTRIBUTES, &Master->SelfTarget);
        if (!NT_SUCCESS(Status))
            goto Exit;

        WDF_IO_TARGET_OPEN_PARAMS_INIT_EXISTING_DEVICE(&OpenParams, WdfDeviceWdmGetDeviceObject(NewDevice));
        Status = WdfIoTargetOpen(Master->SelfTarget, &OpenParams);
        if (!NT_SUCCESS(Status))
            goto Exit;
    }
    else
    {
        Master->SelfTarget = NULL;
    }

    Status = IoRegisterPlugPlayNotification(EventCategoryDeviceInterfaceChange,
                                            PNPNOTIFY_DEVICE_INTERFACE_INCLUDE_EXISTING_INTERFACES,
                                            (PVOID)&GUID_DEVINTERFACE_EMMC_PARTITION_ACCESS_RPMB,
                                            WdfDriverWdmGetDriverObject(WdfGetDriver()),
                                            WtrRpmbNotification,
                                            NewDevice,
                                            &WtrRpmbNotificationEntry);

Exit:
    DPRINT1("WTR: master device mode %lu stack 0x%lx -> 0x%lx\n", NewDevice ? WtrGetMasterContext(NewDevice)->Mode : 0, NewDevice ? WtrGetMasterContext(NewDevice)->StackSize : 0, Status);
    if (NT_SUCCESS(Status))
        *Device = NewDevice;
    return Status;
}
