/*
 * PROJECT:     ReactOS USB Role Switch Class Extension
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Driver entry and KMDF class library binding
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "urscx.h"

#define NDEBUG
#include <debug.h>

#define URS_CONTROL_DEVICE_ATTEMPTS 100

DRIVER_INITIALIZE DriverEntry;
static EVT_WDF_DRIVER_UNLOAD UrsEvtDriverUnload;

DECLARE_CONST_UNICODE_STRING(UrsKernelOnlySddl, L"D:P");

static PFN_URSFUNC UrsExportTable[UrsFunctionTableNumEntries];
static WDFDEVICE UrsControlDevice;
static WCHAR UrsControlDeviceNameBuffer[32];
static UNICODE_STRING UrsControlDeviceName;

static
NTSTATUS
NTAPI
UrsOnClassLoad(VOID)
{
    UrsBuildExportTable(UrsExportTable);
    return STATUS_SUCCESS;
}

static
VOID
NTAPI
UrsOnClassUnload(VOID)
{
}

static
NTSTATUS
NTAPI
UrsOnClientBind(
    _In_ PWDF_CLASS_BIND_INFO ClassBindInfo,
    _In_ PWDF_COMPONENT_GLOBALS ComponentGlobals)
{
    PURS_DRIVER_GLOBALS *ClientGlobals = (PURS_DRIVER_GLOBALS *)ClassBindInfo->ClassBindInfo;
    PURS_GLOBALS_HEADER Header;

    UNREFERENCED_PARAMETER(ComponentGlobals);

    if (!ClientGlobals)
        return STATUS_INVALID_PARAMETER;

    /* KMDF unbinds even after a failed bind */
    *ClientGlobals = NULL;

    if (ClassBindInfo->Version.Major != URS_CLASS_MAJOR_VERSION ||
        ClassBindInfo->Version.Minor > URS_CLASS_MINOR_VERSION ||
        ClassBindInfo->FunctionTableCount != UrsFunctionTableNumEntries)
    {
        DPRINT1("Client wants URS %lu.%lu with %lu slots\n",
                ClassBindInfo->Version.Major,
                ClassBindInfo->Version.Minor,
                ClassBindInfo->FunctionTableCount);
        return STATUS_INVALID_PARAMETER;
    }

    Header = ExAllocatePoolZero(NonPagedPool, sizeof(*Header), URS_POOL_TAG);
    if (!Header)
        return STATUS_INSUFFICIENT_RESOURCES;

    Header->Signature = URS_GLOBALS_SIGNATURE;
    RtlCopyMemory(ClassBindInfo->FunctionTable, UrsExportTable, sizeof(UrsExportTable));
    *ClientGlobals = &Header->Globals;
    return STATUS_SUCCESS;
}

static
VOID
NTAPI
UrsOnClientUnbind(
    _In_ PWDF_CLASS_BIND_INFO ClassBindInfo,
    _In_ PWDF_COMPONENT_GLOBALS ComponentGlobals)
{
    PURS_DRIVER_GLOBALS *ClientGlobals = (PURS_DRIVER_GLOBALS *)ClassBindInfo->ClassBindInfo;

    UNREFERENCED_PARAMETER(ComponentGlobals);

    if (!ClientGlobals || !*ClientGlobals)
        return;

    ExFreePoolWithTag(CONTAINING_RECORD(*ClientGlobals, URS_GLOBALS_HEADER, Globals), URS_POOL_TAG);
    *ClientGlobals = NULL;
}

WDF_CLASS_LIBRARY_INFO UrsClassLibraryInfo =
{
    sizeof(UrsClassLibraryInfo),
    { URS_CLASS_MAJOR_VERSION, URS_CLASS_MINOR_VERSION, 0 },
    UrsOnClassLoad,
    UrsOnClassUnload,
    UrsOnClientBind,
    UrsOnClientUnbind
};

static
NTSTATUS
UrsCreateControlDevice(
    _In_ WDFDRIVER Driver)
{
    PWDFDEVICE_INIT DeviceInit;
    NTSTATUS Status = STATUS_OBJECT_NAME_COLLISION;
    ULONG Index;

    DeviceInit = WdfControlDeviceInitAllocate(Driver, &UrsKernelOnlySddl);
    if (!DeviceInit)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlInitEmptyUnicodeString(&UrsControlDeviceName,
                              UrsControlDeviceNameBuffer,
                              sizeof(UrsControlDeviceNameBuffer));

    /* Another instance may hold a name already */
    for (Index = 0; Index < URS_CONTROL_DEVICE_ATTEMPTS && Status == STATUS_OBJECT_NAME_COLLISION; Index++)
    {
        Status = RtlUnicodeStringPrintf(&UrsControlDeviceName, L"\\Device\\UrsCx%lu", Index);
        if (!NT_SUCCESS(Status))
            break;

        Status = WdfDeviceInitAssignName(DeviceInit, &UrsControlDeviceName);
        if (!NT_SUCCESS(Status))
            break;

        Status = WdfDeviceCreate(&DeviceInit, WDF_NO_OBJECT_ATTRIBUTES, &UrsControlDevice);
    }

    if (!NT_SUCCESS(Status))
    {
        if (DeviceInit)
            WdfDeviceInitFree(DeviceInit);
        return Status;
    }

    WdfControlFinishInitializing(UrsControlDevice);
    return STATUS_SUCCESS;
}

static
VOID
UrsEvtDriverUnload(
    _In_ WDFDRIVER Driver)
{
    UNREFERENCED_PARAMETER(Driver);

    if (UrsControlDevice)
    {
        WdfObjectDelete(UrsControlDevice);
        UrsControlDevice = NULL;
    }
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
    Config.DriverInitFlags |= WdfDriverInitNonPnpDriver;
    Config.EvtDriverUnload = UrsEvtDriverUnload;
    Config.DriverPoolTag = URS_POOL_TAG;

    Status = WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &Config, &Driver);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = UrsCreateControlDevice(Driver);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Cannot create the control device 0x%lx\n", Status);
        return Status;
    }

    Status = WdfRegisterClassLibrary(&UrsClassLibraryInfo, RegistryPath, &UrsControlDeviceName);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("WdfRegisterClassLibrary failed 0x%lx\n", Status);
        WdfObjectDelete(UrsControlDevice);
        UrsControlDevice = NULL;
    }

    return Status;
}
