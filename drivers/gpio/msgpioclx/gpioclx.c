/*
 * PROJECT:     ReactOS GPIO framework extension
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Class library registration and client binding
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#define NDEBUG
#include "gpioclxp.h"

#include <ntstrsafe.h>
#include <wdfldr.h>

DRIVER_INITIALIZE DriverEntry;
static EVT_WDF_DRIVER_UNLOAD GcxEvtDriverUnload;

#ifdef ALLOC_PRAGMA
#pragma alloc_text(INIT, DriverEntry)
#endif

static
NTSTATUS
NTAPI
GcxClassInitialize(VOID);

static
VOID
NTAPI
GcxClassDeinitialize(VOID);

static
NTSTATUS
NTAPI
GcxClassBindClient(
    _In_ PWDF_CLASS_BIND_INFO ClassBindInfo,
    _In_ PWDF_COMPONENT_GLOBALS ComponentGlobals);

static
VOID
NTAPI
GcxClassUnbindClient(
    _In_ PWDF_CLASS_BIND_INFO ClassBindInfo,
    _In_ PWDF_COMPONENT_GLOBALS ComponentGlobals);

static WDF_CLASS_LIBRARY_INFO GcxClassLibraryInfo =
{
    sizeof(GcxClassLibraryInfo),
    { GCX_CLASS_MAJOR_VERSION, GCX_CLASS_MINOR_VERSION, 0 },
    GcxClassInitialize,
    GcxClassDeinitialize,
    GcxClassBindClient,
    GcxClassUnbindClient
};

/* Table order is GPIO_CLX_EXPORT_INDEX from <gpioclx.h> */
static const PGPIO_CLX_EXPORTED_INTERFACES GcxExports[GPIO_CLX_TOTAL_EXPORTS] =
{
    (PGPIO_CLX_EXPORTED_INTERFACES)GcxRegisterClient,
    (PGPIO_CLX_EXPORTED_INTERFACES)GcxUnregisterClient,
    (PGPIO_CLX_EXPORTED_INTERFACES)GcxProcessAddDevicePreDeviceCreate,
    (PGPIO_CLX_EXPORTED_INTERFACES)GcxProcessAddDevicePostDeviceCreate,
    (PGPIO_CLX_EXPORTED_INTERFACES)GcxAcquireInterruptLock,
    (PGPIO_CLX_EXPORTED_INTERFACES)GcxReleaseInterruptLock
};

#define GCX_CLASS_DEVICE_PREFIX L"\\Device\\MSGpioClassExt"

/* Protected DACL with no entries, only kernel mode reaches the class device */
static const UNICODE_STRING GcxKernelOnlySddl = RTL_CONSTANT_STRING(L"D:P");

static WDFDEVICE GcxClassDevice;

static
NTSTATUS
NTAPI
GcxClassInitialize(VOID)
{
    return STATUS_SUCCESS;
}

static
VOID
NTAPI
GcxClassDeinitialize(VOID)
{
}

static
NTSTATUS
NTAPI
GcxClassBindClient(
    _In_ PWDF_CLASS_BIND_INFO ClassBindInfo,
    _In_ PWDF_COMPONENT_GLOBALS ComponentGlobals)
{
    UNREFERENCED_PARAMETER(ComponentGlobals);

    /* Clients identify themselves by WDFDRIVER, so no per client globals are handed back */
    if (ClassBindInfo->Version.Minor != GCX_CLASS_MINOR_VERSION)
    {
        DPRINT1("GpioClx: client asks for %lu.%lu\n",
                ClassBindInfo->Version.Major,
                ClassBindInfo->Version.Minor);
        return STATUS_INVALID_PARAMETER;
    }

    if (ClassBindInfo->FunctionTableCount != RTL_NUMBER_OF(GcxExports))
    {
        DPRINT1("GpioClx: client table has %lu slots\n", ClassBindInfo->FunctionTableCount);
        return STATUS_INVALID_PARAMETER;
    }

    RtlCopyMemory(ClassBindInfo->FunctionTable, GcxExports, sizeof(GcxExports));
    return STATUS_SUCCESS;
}

static
VOID
NTAPI
GcxClassUnbindClient(
    _In_ PWDF_CLASS_BIND_INFO ClassBindInfo,
    _In_ PWDF_COMPONENT_GLOBALS ComponentGlobals)
{
    UNREFERENCED_PARAMETER(ClassBindInfo);
    UNREFERENCED_PARAMETER(ComponentGlobals);
}

static
VOID
NTAPI
GcxEvtDriverUnload(
    _In_ WDFDRIVER Driver)
{
    UNREFERENCED_PARAMETER(Driver);

    GcxUnregisterSecondaryInterruptController();
    GcxCloseResourceHub();
}

/**
 * @brief
 * Creates the control device that carries the class library name.
 *
 * @param[in] Driver
 * This driver.
 *
 * @param[out] ClassName
 * Receives the name the device was created under. Must have room for it.
 *
 * @return
 * STATUS_SUCCESS, or the failure from WDF.
 */
static
NTSTATUS
NTAPI
GcxCreateClassDevice(
    _In_ WDFDRIVER Driver,
    _Inout_ PUNICODE_STRING ClassName)
{
    PWDFDEVICE_INIT DeviceInit;
    NTSTATUS Status;
    ULONG Instance;

    DeviceInit = WdfControlDeviceInitAllocate(Driver, &GcxKernelOnlySddl);
    if (DeviceInit == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    /* Another instance may already hold a name, so take the first free one */
    for (Instance = 0; ; Instance++)
    {
        Status = RtlUnicodeStringPrintf(ClassName, L"%s%lu", GCX_CLASS_DEVICE_PREFIX, Instance);
        if (!NT_SUCCESS(Status))
            break;

        Status = WdfDeviceInitAssignName(DeviceInit, ClassName);
        if (!NT_SUCCESS(Status))
            break;

        Status = WdfDeviceCreate(&DeviceInit, WDF_NO_OBJECT_ATTRIBUTES, &GcxClassDevice);
        if (Status != STATUS_OBJECT_NAME_COLLISION)
            break;
    }

    if (!NT_SUCCESS(Status))
    {
        DPRINT1("GpioClx: class device creation failed 0x%08lx\n", Status);
        if (DeviceInit != NULL)
            WdfDeviceInitFree(DeviceInit);
        return Status;
    }

    WdfControlFinishInitializing(GcxClassDevice);
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    DECLARE_UNICODE_STRING_SIZE(ClassName, 64);
    WDF_DRIVER_CONFIG DriverConfig;
    WDFDRIVER Driver;
    NTSTATUS Status;

    GcxInitializeGlobals();

    WDF_DRIVER_CONFIG_INIT(&DriverConfig, WDF_NO_EVENT_CALLBACK);
    DriverConfig.EvtDriverUnload = GcxEvtDriverUnload;
    DriverConfig.DriverInitFlags = WdfDriverInitNonPnpDriver;

    Status = WdfDriverCreate(DriverObject,
                             RegistryPath,
                             WDF_NO_OBJECT_ATTRIBUTES,
                             &DriverConfig,
                             &Driver);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("GpioClx: WdfDriverCreate failed 0x%08lx\n", Status);
        return Status;
    }

    /* wdfldr opens the library by this name, so a device has to own it first */
    Status = GcxCreateClassDevice(Driver, &ClassName);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = WdfRegisterClassLibrary(&GcxClassLibraryInfo, RegistryPath, &ClassName);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("GpioClx: WdfRegisterClassLibrary failed 0x%08lx\n", Status);
        WdfObjectDelete(GcxClassDevice);
        GcxClassDevice = NULL;
        return Status;
    }

    return STATUS_SUCCESS;
}
