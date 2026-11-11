/*
 * PROJECT:     ReactOS USB Role Switch Class Extension
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Client entry points
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "urscx.h"

#define NDEBUG
#include <debug.h>

static
VOID
UrsClientError(
    _In_z_ PCSTR Message)
{
    DbgPrint("UrsCx Verifier Break: %s\n", Message);
    WdfVerifierDbgBreakPoint();
}

static
NTSTATUS
UrsCheckGlobals(
    _In_ PURS_DRIVER_GLOBALS Globals)
{
    if (Globals && CONTAINING_RECORD(Globals, URS_GLOBALS_HEADER, Globals)->Signature == URS_GLOBALS_SIGNATURE)
        return STATUS_SUCCESS;

    UrsClientError("Driver globals are invalid");
    return STATUS_INVALID_DEVICE_REQUEST;
}

static
NTSTATUS
UrsCheckCall(
    _In_ PURS_DRIVER_GLOBALS Globals)
{
    if (KeGetCurrentIrql() != PASSIVE_LEVEL)
    {
        UrsClientError("API called at incorrect IRQL");
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    return UrsCheckGlobals(Globals);
}

static
NTSTATUS
NTAPI
UrsApiDeviceInitInitialize(
    _In_ PURS_DRIVER_GLOBALS Globals,
    _In_ PWDFDEVICE_INIT DeviceInit)
{
    NTSTATUS Status = UrsCheckCall(Globals);

    if (!NT_SUCCESS(Status))
        return Status;

    return UrsFdoInitializeDeviceInit(DeviceInit);
}

static
NTSTATUS
NTAPI
UrsApiDeviceInitialize(
    _In_ PURS_DRIVER_GLOBALS Globals,
    _In_ WDFDEVICE Device,
    _In_ PURS_CONFIG Config)
{
    NTSTATUS Status = UrsCheckCall(Globals);

    if (!NT_SUCCESS(Status))
        return Status;

    if (Config->Size != sizeof(*Config))
    {
        UrsClientError("Unexpected size for URS_CONFIG");
        return STATUS_INFO_LENGTH_MISMATCH;
    }

    if (!Config->EvtUrsFilterRemoveResourceRequirements)
    {
        UrsClientError("EvtUrsFilterRemoveResourceRequirements is a required callback");
        return STATUS_INVALID_PARAMETER;
    }

    return UrsFdoInitialize(Device, Config);
}

static
NTSTATUS
NTAPI
UrsApiIoResourceListAppendDescriptor(
    _In_ PURS_DRIVER_GLOBALS Globals,
    _In_ URSIORESLIST IoResourceList,
    _In_ PIO_RESOURCE_DESCRIPTOR Descriptor)
{
    WDFCOLLECTION List = (WDFCOLLECTION)IoResourceList;
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDFMEMORY Entry;
    NTSTATUS Status = UrsCheckCall(Globals);

    if (!NT_SUCCESS(Status))
        return Status;

    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ParentObject = List;
    Status = WdfMemoryCreate(&Attributes, PagedPool, URS_POOL_TAG, sizeof(*Descriptor), &Entry, NULL);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = WdfMemoryCopyFromBuffer(Entry, 0, Descriptor, sizeof(*Descriptor));
    if (NT_SUCCESS(Status))
        Status = WdfCollectionAdd(List, Entry);

    if (!NT_SUCCESS(Status))
        WdfObjectDelete(Entry);

    return Status;
}

static
VOID
NTAPI
UrsApiReportHardwareEvent(
    _In_ PURS_DRIVER_GLOBALS Globals,
    _In_ WDFDEVICE Device,
    _In_ URS_HARDWARE_EVENT Event)
{
    PURS_FDO Fdo;

    /* Interrupt service routines report events, so there is no IRQL check */
    if (!NT_SUCCESS(UrsCheckGlobals(Globals)))
        return;

    if (Event == UrsHardwareEventNone)
    {
        UrsClientError("Client driver is not allowed to report UrsHardwareEventNone");
        return;
    }

    Fdo = UrsGetFdo(Device);
    if (Fdo->HardwareEventSupport != 1)
    {
        UrsClientError("Client driver has not indicated to the class extension that it supports hardware events");
        return;
    }

    /* Host only mode never brings the function role up */
    if (Fdo->HostOnlyEvents && (Event == UrsHardwareEventIdFloat || Event == UrsHardwareEventIdB))
        Event = UrsHardwareEventDetach;

    if (!NT_SUCCESS(UrsRoleReportEvent(Fdo, Event)))
        UrsClientError("Client driver has reported an invalid hardware event");
}

static
VOID
NTAPI
UrsApiSetPoHandle(
    _In_ PURS_DRIVER_GLOBALS Globals,
    _In_ WDFDEVICE Device,
    _In_opt_ POHANDLE PoHandle)
{
    if (!NT_SUCCESS(UrsCheckCall(Globals)))
        return;

    UrsGetFdo(Device)->PoHandle = PoHandle;
}

static
VOID
NTAPI
UrsApiSetHardwareEventSupport(
    _In_ PURS_DRIVER_GLOBALS Globals,
    _In_ WDFDEVICE Device,
    _In_ BOOLEAN Supported)
{
    if (!NT_SUCCESS(UrsCheckCall(Globals)))
        return;

    UrsFdoSetHardwareEventSupport(Device, Supported);
}

VOID
NTAPI
UrsBuildExportTable(
    _Out_writes_(UrsFunctionTableNumEntries) PFN_URSFUNC *Table)
{
    Table[UrsDeviceInitializeTableIndex] = (PFN_URSFUNC)UrsApiDeviceInitialize;
    Table[UrsDeviceInitInitializeTableIndex] = (PFN_URSFUNC)UrsApiDeviceInitInitialize;
    Table[UrsIoResourceListAppendDescriptorTableIndex] = (PFN_URSFUNC)UrsApiIoResourceListAppendDescriptor;
    Table[UrsReportHardwareEventTableIndex] = (PFN_URSFUNC)UrsApiReportHardwareEvent;
    Table[UrsSetPoHandleTableIndex] = (PFN_URSFUNC)UrsApiSetPoHandle;
    Table[UrsSetHardwareEventSupportTableIndex] = (PFN_URSFUNC)UrsApiSetHardwareEventSupport;
}
