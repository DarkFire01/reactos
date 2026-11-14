/*
 * PROJECT:     ReactOS USB Role Switch Class Extension
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Role switch controller device
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "urscx.h"
#include <initguid.h>

#define NDEBUG
#include <debug.h>

DEFINE_GUID(GUID_DEVINTERFACE_USB_ROLE_SWITCH,
            0x5e837b9c, 0x19ba, 0x43fc, 0xb8, 0xdd, 0x84, 0x9e, 0x62, 0x93, 0xe5, 0x43);

static EVT_WDF_DEVICE_FILTER_RESOURCE_REQUIREMENTS UrsFdoEvtFilterRemoveResourceRequirements;
static EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL UrsFdoEvtIoDeviceControl;
static EVT_WDFDEVICE_WDM_IRP_DISPATCH UrsFdoEvtWdmDeviceControl;

/* Empties a role resource list so the next filter pass starts clean */
static
VOID
UrsFdoPurgeResources(
    _In_ WDFCOLLECTION List)
{
    WDFOBJECT Entry;

    while ((Entry = WdfCollectionGetFirstItem(List)) != NULL)
    {
        WdfCollectionRemove(List, Entry);
        WdfObjectDelete(Entry);
    }
}

static
NTSTATUS
UrsFdoEvtFilterRemoveResourceRequirements(
    _In_ WDFDEVICE Device,
    _In_ WDFIORESREQLIST Requirements)
{
    PURS_FDO Fdo = UrsGetFdo(Device);
    IO_RESOURCE_DESCRIPTOR Shared;
    PIO_RESOURCE_DESCRIPTOR Descriptor;
    WDFIORESLIST List;
    ULONG ListCount, ListIndex, Count, Index;
    NTSTATUS Status;

    UrsFdoPurgeResources(Fdo->HostResources);
    UrsFdoPurgeResources(Fdo->FunctionResources);

    /* The role child maps the same registers, so the windows are shared */
    ListCount = WdfIoResourceRequirementsListGetCount(Requirements);
    for (ListIndex = 0; ListIndex < ListCount; ListIndex++)
    {
        List = WdfIoResourceRequirementsListGetIoResList(Requirements, ListIndex);
        Count = WdfIoResourceListGetCount(List);
        for (Index = 0; Index < Count; Index++)
        {
            Descriptor = WdfIoResourceListGetDescriptor(List, Index);
            if (Descriptor->Type != CmResourceTypeMemory && Descriptor->Type != CmResourceTypeMemoryLarge)
                continue;

            Shared = *Descriptor;
            Shared.ShareDisposition = CmResourceShareShared;
            WdfIoResourceListUpdateDescriptor(List, &Shared, Index);
        }
    }

    Status = Fdo->Config.EvtUrsFilterRemoveResourceRequirements(Device,
                                                               Requirements,
                                                               (URSIORESLIST)Fdo->HostResources,
                                                               (URSIORESLIST)Fdo->FunctionResources);
    if (!NT_SUCCESS(Status))
    {
        UrsFdoPurgeResources(Fdo->HostResources);
        UrsFdoPurgeResources(Fdo->FunctionResources);
    }

    return Status;
}

static
VOID
UrsFdoCompletePldList(
    _In_ PURS_FDO Fdo,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputLength)
{
    PURS_PLD_LIST List;
    ULONG Count, Needed, i;
    WDFMEMORY Entry;
    NTSTATUS Status;

    if (OutputLength < sizeof(URS_PLD_LIST))
    {
        WdfRequestComplete(Request, STATUS_BUFFER_TOO_SMALL);
        return;
    }

    Status = WdfRequestRetrieveOutputBuffer(Request, sizeof(URS_PLD_LIST), (PVOID *)&List, NULL);
    if (!NT_SUCCESS(Status))
    {
        WdfRequestComplete(Request, Status);
        return;
    }

    Count = WdfCollectionGetCount(Fdo->Plds);
    Needed = FIELD_OFFSET(URS_PLD_LIST, Entries) + Count * sizeof(List->Entries[0]);
    List->Size = Needed;
    if (OutputLength < Needed)
    {
        WdfRequestSetInformation(Request, FIELD_OFFSET(URS_PLD_LIST, Entries));
        WdfRequestComplete(Request, STATUS_BUFFER_TOO_SMALL);
        return;
    }

    for (i = 0; i < Count; i++)
    {
        Entry = WdfCollectionGetItem(Fdo->Plds, i);
        if (!Entry)
        {
            WdfRequestComplete(Request, STATUS_UNSUCCESSFUL);
            return;
        }

        List->Entries[i] = *(PULONG64)WdfMemoryGetBuffer(Entry, NULL);
    }

    List->Count = Count;
    WdfRequestSetInformation(Request, Needed);
    WdfRequestComplete(Request, STATUS_SUCCESS);
}

static
VOID
UrsFdoEvtIoDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode)
{
    PURS_FDO Fdo = UrsGetFdo(WdfIoQueueGetDevice(Queue));
    PULONG Value;
    NTSTATUS Status;

    if (IoControlCode == IOCTL_URS_GET_PLD_LIST)
    {
        UrsFdoCompletePldList(Fdo, Request, OutputBufferLength);
        return;
    }

    switch (IoControlCode)
    {
        case IOCTL_URS_GET_ROLE_SWITCH_MODE:
        case IOCTL_URS_GET_CURRENT_ROLE:
            if (OutputBufferLength != sizeof(ULONG))
            {
                Status = STATUS_INFO_LENGTH_MISMATCH;
                break;
            }

            Status = WdfRequestRetrieveOutputBuffer(Request, sizeof(ULONG), (PVOID *)&Value, NULL);
            if (!NT_SUCCESS(Status))
                break;

            if (IoControlCode == IOCTL_URS_GET_ROLE_SWITCH_MODE)
            {
                *Value = UrsRoleGetMode(Fdo);
            }
            else
            {
                WdfWaitLockAcquire(Fdo->Lock, NULL);
                *Value = Fdo->CurrentRole;
                WdfWaitLockRelease(Fdo->Lock);
            }

            WdfRequestSetInformation(Request, sizeof(ULONG));
            break;

        case IOCTL_URS_SET_ROLE_SWITCH_MODE:
            if (InputBufferLength != sizeof(ULONG))
            {
                Status = STATUS_INFO_LENGTH_MISMATCH;
                break;
            }

            Status = WdfRequestRetrieveInputBuffer(Request, sizeof(ULONG), (PVOID *)&Value, NULL);
            if (!NT_SUCCESS(Status))
                break;

            if (*Value < UrsModeHost || *Value > UrsModeHardwareEventsHostOnly)
            {
                Status = STATUS_INVALID_PARAMETER;
                break;
            }

            UrsRoleSetMode(Fdo, (URS_ROLE_SWITCH_MODE)*Value);
            Status = STATUS_SUCCESS;
            break;

        case IOCTL_URS_REPORT_HARDWARE_EVENT:
            /* Only taken from a connector driver when the client reports nothing itself */
            if (Fdo->HardwareEventSupport != 0)
            {
                Status = STATUS_INVALID_DEVICE_REQUEST;
                break;
            }

            if (InputBufferLength != sizeof(ULONG))
            {
                Status = STATUS_INFO_LENGTH_MISMATCH;
                break;
            }

            Status = WdfRequestRetrieveInputBuffer(Request, sizeof(ULONG), (PVOID *)&Value, NULL);
            if (!NT_SUCCESS(Status))
                break;

            if (*Value == UrsHardwareEventIdB && Fdo->HostOnlyEvents)
                *Value = UrsHardwareEventDetach;

            Status = UrsRoleReportEvent(Fdo, (URS_HARDWARE_EVENT)*Value);
            break;

        default:
            Status = STATUS_INVALID_DEVICE_REQUEST;
            break;
    }

    WdfRequestComplete(Request, Status);
}

/* Takes the role switch requests away from the client's own device control path */
static
NTSTATUS
UrsFdoEvtWdmDeviceControl(
    _In_ WDFDEVICE Device,
    _In_ UCHAR MajorFunction,
    _In_ UCHAR MinorFunction,
    _In_ ULONG Code,
    _In_ WDFCONTEXT DriverContext,
    _Inout_ PIRP Irp,
    _In_ WDFCONTEXT DispatchContext)
{
    PURS_FDO Fdo = UrsGetFdo(Device);

    UNREFERENCED_PARAMETER(MajorFunction);
    UNREFERENCED_PARAMETER(MinorFunction);
    UNREFERENCED_PARAMETER(DriverContext);

    switch (Code)
    {
        case IOCTL_URS_GET_ROLE_SWITCH_MODE:
        case IOCTL_URS_SET_ROLE_SWITCH_MODE:
        case IOCTL_URS_GET_CURRENT_ROLE:
        case IOCTL_URS_REPORT_HARDWARE_EVENT:
            return WdfDeviceWdmDispatchIrpToIoQueue(Device, Irp, Fdo->ControlQueue, 0);

        case IOCTL_URS_GET_PLD_LIST:
            return WdfDeviceWdmDispatchIrpToIoQueue(Device, Irp, Fdo->PldQueue, 0);

        /* The framework advances the stack like IoCallDriver does */
        default:
            IoSkipCurrentIrpStackLocation(Irp);
            return WdfDeviceWdmDispatchIrp(Device, Irp, DispatchContext);
    }
}

static
NTSTATUS
UrsFdoQueryStringList(
    _In_ PURS_FDO Fdo,
    _In_ DEVICE_REGISTRY_PROPERTY Property,
    _Out_ WDFCOLLECTION *Collection)
{
    WDF_OBJECT_ATTRIBUTES Attributes;
    UNICODE_STRING Item;
    WDFMEMORY Memory = NULL;
    WDFSTRING String;
    PWSTR Cursor;
    size_t Size;
    NTSTATUS Status;

    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ParentObject = Fdo->Device;
    Status = WdfCollectionCreate(&Attributes, Collection);
    if (!NT_SUCCESS(Status))
        return Status;

    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ParentObject = Fdo->Device;
    Status = WdfDeviceAllocAndQueryProperty(Fdo->Device, Property, PagedPool, &Attributes, &Memory);
    if (!NT_SUCCESS(Status))
    {
        /* A device without compatible IDs is fine */
        return (Property == DevicePropertyCompatibleIDs) ? STATUS_SUCCESS : Status;
    }

    Cursor = WdfMemoryGetBuffer(Memory, &Size);
    while (*Cursor)
    {
        RtlInitUnicodeString(&Item, Cursor);
        WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
        Attributes.ParentObject = *Collection;
        Status = WdfStringCreate(&Item, &Attributes, &String);
        if (NT_SUCCESS(Status))
            Status = WdfCollectionAdd(*Collection, String);
        if (!NT_SUCCESS(Status))
            break;

        Cursor += Item.Length / sizeof(WCHAR) + 1;
    }

    WdfObjectDelete(Memory);
    return Status;
}

static
NTSTATUS
UrsFdoCreateBusInformation(
    _In_ PURS_FDO Fdo,
    _In_ ULONG BusType,
    _In_ ULONG Size,
    _Outptr_ PURS_BUS_INFORMATION *Information)
{
    WDF_OBJECT_ATTRIBUTES Attributes;
    NTSTATUS Status;

    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ParentObject = Fdo->Device;
    Status = WdfMemoryCreate(&Attributes, NonPagedPool, URS_POOL_TAG, Size, &Fdo->BusInformation, (PVOID *)Information);
    if (!NT_SUCCESS(Status))
        return Status;

    RtlZeroMemory(*Information, Size);
    (*Information)->Size = Size;
    (*Information)->BusType = BusType;
    return STATUS_SUCCESS;
}

/* Keeps the controller's own ACPI identity for the child's compatible IDs */
static
NTSTATUS
UrsFdoRetrieveAcpiInformation(
    _In_ PURS_FDO Fdo)
{
    ACPI_DEVICE_INFORMATION_OUTPUT_BUFFER Probe;
    WDF_MEMORY_DESCRIPTOR Descriptor;
    PURS_BUS_INFORMATION Information;
    WDFIOTARGET Target = WdfDeviceGetIoTarget(Fdo->Device);
    ULONG Size;
    NTSTATUS Status;

    RtlZeroMemory(&Probe, sizeof(Probe));
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&Descriptor, &Probe, sizeof(Probe));
    Status = WdfIoTargetSendIoctlSynchronously(Target,
                                               NULL,
                                               IOCTL_ACPI_GET_DEVICE_INFORMATION,
                                               NULL,
                                               &Descriptor,
                                               NULL,
                                               NULL);
    if (Status != STATUS_BUFFER_OVERFLOW && !NT_SUCCESS(Status))
        return Status;

    Size = sizeof(URS_BUS_INFORMATION) + max(Probe.Size, sizeof(Probe));
    Status = UrsFdoCreateBusInformation(Fdo, URS_BUS_ACPI, Size, &Information);
    if (!NT_SUCCESS(Status))
        return Status;

    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&Descriptor, Information + 1, Size - sizeof(*Information));
    Status = WdfIoTargetSendIoctlSynchronously(Target,
                                               NULL,
                                               IOCTL_ACPI_GET_DEVICE_INFORMATION,
                                               NULL,
                                               &Descriptor,
                                               NULL,
                                               NULL);
    if (!NT_SUCCESS(Status))
    {
        WdfObjectDelete(Fdo->BusInformation);
        Fdo->BusInformation = NULL;
    }

    return Status;
}

static
NTSTATUS
UrsFdoRetrieveBusInformation(
    _In_ PURS_FDO Fdo)
{
    PURS_BUS_INFORMATION Information;
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDFMEMORY Memory;
    PCWSTR Enumerator;
    NTSTATUS Status;

    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ParentObject = Fdo->Device;
    Status = WdfDeviceAllocAndQueryProperty(Fdo->Device,
                                            DevicePropertyEnumeratorName,
                                            PagedPool,
                                            &Attributes,
                                            &Memory);
    if (!NT_SUCCESS(Status))
        return Status;

    Enumerator = WdfMemoryGetBuffer(Memory, NULL);
    if (!_wcsicmp(Enumerator, L"ACPI"))
        Status = UrsFdoRetrieveAcpiInformation(Fdo);
    else if (!_wcsicmp(Enumerator, L"PCI"))
        Status = UrsFdoCreateBusInformation(Fdo, URS_BUS_PCI, sizeof(*Information), &Information);
    else
        Status = UrsFdoCreateBusInformation(Fdo, URS_BUS_UNKNOWN, URS_UNKNOWN_BUS_INFORMATION_SIZE, &Information);

    WdfObjectDelete(Memory);
    return Status;
}

static
NTSTATUS
UrsFdoCreateQueue(
    _In_ PURS_FDO Fdo,
    _Out_ WDFQUEUE *Queue)
{
    WDF_IO_QUEUE_CONFIG Config;
    WDF_OBJECT_ATTRIBUTES Attributes;

    WDF_IO_QUEUE_CONFIG_INIT(&Config, WdfIoQueueDispatchSequential);
    Config.PowerManaged = WdfTrue;
    Config.EvtIoDeviceControl = UrsFdoEvtIoDeviceControl;

    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ExecutionLevel = WdfExecutionLevelPassive;
    Attributes.ParentObject = Fdo->Device;
    return WdfIoQueueCreate(Fdo->Device, &Config, &Attributes, Queue);
}

/**
 * @brief
 * Prepares the client's device init so the class extension sees its resource requirements.
 */
NTSTATUS
NTAPI
UrsFdoInitializeDeviceInit(
    _In_ PWDFDEVICE_INIT DeviceInit)
{
    WDF_FDO_EVENT_CALLBACKS Callbacks;

    WdfDeviceInitSetDeviceType(DeviceInit, FILE_DEVICE_BUS_EXTENDER);

    WDF_FDO_EVENT_CALLBACKS_INIT(&Callbacks);
    Callbacks.EvtDeviceFilterRemoveResourceRequirements = UrsFdoEvtFilterRemoveResourceRequirements;
    WdfFdoInitSetEventCallbacks(DeviceInit, &Callbacks);
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Attaches the role switch state to the client's device and starts the role manager.
 */
NTSTATUS
NTAPI
UrsFdoInitialize(
    _In_ WDFDEVICE Device,
    _In_ PURS_CONFIG Config)
{
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDF_DEVICE_STATE State;
    PURS_FDO Fdo;
    NTSTATUS Status;

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, URS_FDO);
    Status = WdfObjectAllocateContext(Device, &Attributes, (PVOID *)&Fdo);
    if (!NT_SUCCESS(Status))
        return Status;

    Fdo->Device = Device;
    Fdo->Config = *Config;

    WDF_DEVICE_STATE_INIT(&State);
    State.NotDisableable = WdfFalse;
    WdfDeviceSetDeviceState(Device, &State);

    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ParentObject = Device;
    Status = WdfCollectionCreate(&Attributes, &Fdo->HostResources);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = WdfCollectionCreate(&Attributes, &Fdo->FunctionResources);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = UrsFdoCreateQueue(Fdo, &Fdo->ControlQueue);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = UrsFdoCreateQueue(Fdo, &Fdo->PldQueue);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = WdfDeviceConfigureWdmIrpDispatchCallback(Device,
                                                      NULL,
                                                      IRP_MJ_DEVICE_CONTROL,
                                                      UrsFdoEvtWdmDeviceControl,
                                                      NULL);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = UrsFdoQueryStringList(Fdo, DevicePropertyHardwareID, &Fdo->HardwareIds);
    if (NT_SUCCESS(Status))
        Status = UrsFdoQueryStringList(Fdo, DevicePropertyCompatibleIDs, &Fdo->CompatibleIds);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = UrsFdoRetrieveBusInformation(Fdo);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("No bus information for the controller 0x%lx\n", Status);
        return Status;
    }

    Status = UrsAcpiEnumeratePlds(Fdo);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Port locations could not be read 0x%lx\n", Status);
        return Status;
    }

    Status = WdfDeviceCreateDeviceInterface(Device, &GUID_DEVINTERFACE_USB_ROLE_SWITCH, NULL);
    if (!NT_SUCCESS(Status))
        return Status;

    /* Undecided until the client says during PrepareHardware */
    Fdo->HardwareEventSupport = 2;
    return UrsRoleInitialize(Fdo);
}

/**
 * @brief
 * Records whether the client reports ID pin changes itself.
 */
VOID
NTAPI
UrsFdoSetHardwareEventSupport(
    _In_ WDFDEVICE Device,
    _In_ BOOLEAN Supported)
{
    PURS_FDO Fdo = UrsGetFdo(Device);

    if (WdfDeviceGetDevicePnpState(Device) == WdfDevStatePnpStarted)
    {
        DbgPrint("UrsCx Verifier Break: Client driver is not allowed to report a change in support "
                 "for reporting hardware events after the EvtDevicePrepareHardware callback has completed.\n");
        WdfVerifierDbgBreakPoint();
        return;
    }

    Fdo->HardwareEventSupport = Supported ? 1 : 0;

    /* Nothing switches VBUS when nobody watches the ID pin */
    if (!Supported)
        Fdo->VBusUnmanaged = TRUE;
}
