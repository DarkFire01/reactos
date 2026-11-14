/*
 * PROJECT:     ReactOS Simple Peripheral Bus
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     SpbCx class library registration and client binding
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#include "spbcxp.h"

#define NDEBUG
#include <debug.h>

#define SCX_MAJOR_VERSION 1
#define SCX_MINOR_VERSION 1

#define SCX_CONTROL_DEVICE_ATTEMPTS 100

DRIVER_INITIALIZE DriverEntry;
static EVT_WDF_DRIVER_UNLOAD ScxEvtDriverUnload;

static
NTSTATUS
NTAPI
ScxClassInitialize(VOID);

static
VOID
NTAPI
ScxClassDeinitialize(VOID);

static
NTSTATUS
NTAPI
ScxClassBindClient(
    _In_ PWDF_CLASS_BIND_INFO ClassBindInfo,
    _In_ PWDF_COMPONENT_GLOBALS ComponentGlobals);

static
VOID
NTAPI
ScxClassUnbindClient(
    _In_ PWDF_CLASS_BIND_INFO ClassBindInfo,
    _In_ PWDF_COMPONENT_GLOBALS ComponentGlobals);

static WDF_CLASS_LIBRARY_INFO ScxClassLibraryInfo =
{
    sizeof(ScxClassLibraryInfo),
    { SCX_MAJOR_VERSION, SCX_MINOR_VERSION, 0 },
    ScxClassInitialize,
    ScxClassDeinitialize,
    ScxClassBindClient,
    ScxClassUnbindClient
};

/* SpbCx 1.0 and 1.1 publish the same table */
static const SPBFUNC ScxExportTable[SpbFunctionTableNumEntries] =
{
    (SPBFUNC)ScxDeviceInitConfig,
    (SPBFUNC)ScxDeviceInitialize,
    (SPBFUNC)ScxControllerSetIoOtherCallback,
    (SPBFUNC)ScxControllerSetRequestAttributes,
    (SPBFUNC)ScxControllerSetTargetAttributes,
    (SPBFUNC)ScxTargetGetConnectionParameters,
    (SPBFUNC)ScxTargetGetFileObject,
    (SPBFUNC)ScxRequestGetTarget,
    (SPBFUNC)ScxRequestGetController,
    (SPBFUNC)ScxRequestGetParameters,
    (SPBFUNC)ScxRequestGetTransferParameters,
    (SPBFUNC)ScxRequestComplete,
    (SPBFUNC)ScxRequestCaptureIoOtherTransferList
};

/* SDDL_DEVOBJ_KERNEL_ONLY; wdmsec.h is not available here */
DECLARE_CONST_UNICODE_STRING(ScxKernelOnlySddl, L"D:P");

static WDFDEVICE ScxControlDevice;
static UNICODE_STRING ScxControlDeviceName;
static WCHAR ScxControlDeviceNameBuffer[32];

PSCX_CLIENT_GLOBALS
NTAPI
ScxGlobalsFromClient(
    _In_opt_ PSPB_DRIVER_GLOBALS ClientGlobals)
{
    PSCX_CLIENT_GLOBALS Globals;

    if (ClientGlobals == NULL)
        return NULL;

    Globals = CONTAINING_RECORD(ClientGlobals, SCX_CLIENT_GLOBALS, ClientGlobals);
    if (Globals->Signature != SCX_CLIENT_GLOBALS_SIGNATURE)
        return NULL;

    return Globals;
}

static
NTSTATUS
NTAPI
ScxClassInitialize(VOID)
{
    return STATUS_SUCCESS;
}

static
VOID
NTAPI
ScxClassDeinitialize(VOID)
{
}

static
NTSTATUS
NTAPI
ScxClassBindClient(
    _In_ PWDF_CLASS_BIND_INFO ClassBindInfo,
    _In_ PWDF_COMPONENT_GLOBALS ComponentGlobals)
{
    PSPB_DRIVER_GLOBALS *ClientGlobals;
    PSCX_CLIENT_GLOBALS Globals;

    UNREFERENCED_PARAMETER(ComponentGlobals);

    ClientGlobals = (PSPB_DRIVER_GLOBALS *)ClassBindInfo->ClassBindInfo;
    if (ClientGlobals == NULL)
        return STATUS_INVALID_PARAMETER;

    /* KMDF unbinds even after a failed bind */
    *ClientGlobals = NULL;

    if (ClassBindInfo->Version.Major != SCX_MAJOR_VERSION ||
        ClassBindInfo->Version.Minor > SCX_MINOR_VERSION)
    {
        DPRINT1("SpbCx: client wants %lu.%lu, have %u.%u\n",
                ClassBindInfo->Version.Major,
                ClassBindInfo->Version.Minor,
                SCX_MAJOR_VERSION,
                SCX_MINOR_VERSION);
        return STATUS_INVALID_PARAMETER;
    }

    if (ClassBindInfo->FunctionTableCount != SpbFunctionTableNumEntries)
    {
        DPRINT1("SpbCx: client has %lu table slots, expected %u\n",
                ClassBindInfo->FunctionTableCount,
                (ULONG)SpbFunctionTableNumEntries);
        return STATUS_INVALID_PARAMETER;
    }

    RtlCopyMemory(ClassBindInfo->FunctionTable, ScxExportTable, sizeof(ScxExportTable));

    Globals = ExAllocatePoolZero(NonPagedPoolNx, sizeof(*Globals), SCX_POOL_TAG);
    if (Globals == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Globals->Signature = SCX_CLIENT_GLOBALS_SIGNATURE;
    Globals->Version = ClassBindInfo->Version;

    *ClientGlobals = (PSPB_DRIVER_GLOBALS)&Globals->ClientGlobals;

    DPRINT("SpbCx: bound a %lu.%lu client\n",
           ClassBindInfo->Version.Major,
           ClassBindInfo->Version.Minor);
    return STATUS_SUCCESS;
}

static
VOID
NTAPI
ScxClassUnbindClient(
    _In_ PWDF_CLASS_BIND_INFO ClassBindInfo,
    _In_ PWDF_COMPONENT_GLOBALS ComponentGlobals)
{
    PSPB_DRIVER_GLOBALS *ClientGlobals;
    PSCX_CLIENT_GLOBALS Globals;

    UNREFERENCED_PARAMETER(ComponentGlobals);

    ClientGlobals = (PSPB_DRIVER_GLOBALS *)ClassBindInfo->ClassBindInfo;
    if (ClientGlobals == NULL)
        return;

    Globals = ScxGlobalsFromClient(*ClientGlobals);
    if (Globals != NULL)
        ExFreePoolWithTag(Globals, SCX_POOL_TAG);

    *ClientGlobals = NULL;
}

/* The loader opens the class by device name; another instance may own the first one */
static
NTSTATUS
NTAPI
ScxCreateControlDevice(
    _In_ WDFDRIVER Driver)
{
    PWDFDEVICE_INIT DeviceInit;
    WDFDEVICE Device = NULL;
    ULONG Index;
    NTSTATUS Status = STATUS_OBJECT_NAME_COLLISION;

    PAGED_CODE();

    DeviceInit = WdfControlDeviceInitAllocate(Driver, &ScxKernelOnlySddl);
    if (DeviceInit == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlInitEmptyUnicodeString(&ScxControlDeviceName,
                              ScxControlDeviceNameBuffer,
                              sizeof(ScxControlDeviceNameBuffer));

    for (Index = 0; Index < SCX_CONTROL_DEVICE_ATTEMPTS && Status == STATUS_OBJECT_NAME_COLLISION; Index++)
    {
        Status = RtlUnicodeStringPrintf(&ScxControlDeviceName, L"\\Device\\SpbCx%lu", Index);
        if (!NT_SUCCESS(Status))
            break;

        Status = WdfDeviceInitAssignName(DeviceInit, &ScxControlDeviceName);
        if (!NT_SUCCESS(Status))
            break;

        Status = WdfDeviceCreate(&DeviceInit, WDF_NO_OBJECT_ATTRIBUTES, &Device);
    }

    /* A successful create took the init block over */
    if (!NT_SUCCESS(Status))
    {
        if (DeviceInit != NULL)
            WdfDeviceInitFree(DeviceInit);
        return Status;
    }

    WdfControlFinishInitializing(Device);
    ScxControlDevice = Device;

    return STATUS_SUCCESS;
}

static
VOID
NTAPI
ScxEvtDriverUnload(
    _In_ WDFDRIVER Driver)
{
    UNREFERENCED_PARAMETER(Driver);

    PAGED_CODE();

    if (ScxControlDevice != NULL)
    {
        WdfObjectDelete(ScxControlDevice);
        ScxControlDevice = NULL;
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
    Config.EvtDriverUnload = ScxEvtDriverUnload;
    Config.DriverPoolTag = SCX_POOL_TAG;

    Status = WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &Config, &Driver);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("SpbCx: WdfDriverCreate failed 0x%lx\n", Status);
        return Status;
    }

    Status = ScxCreateControlDevice(Driver);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("SpbCx: cannot create the control device 0x%lx\n", Status);
        return Status;
    }

    Status = WdfRegisterClassLibrary(&ScxClassLibraryInfo, RegistryPath, &ScxControlDeviceName);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("SpbCx: WdfRegisterClassLibrary failed 0x%lx\n", Status);
        WdfObjectDelete(ScxControlDevice);
        ScxControlDevice = NULL;
        return Status;
    }

    DPRINT("SpbCx: loaded, control device %wZ\n", &ScxControlDeviceName);
    return STATUS_SUCCESS;
}
