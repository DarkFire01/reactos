/*
 * PROJECT:     ReactOS GPIO framework extension
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Class library registration and client binding
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/*
 * A GPIO controller driver never links a GpioClx import library. It carries a
 * .kmdfclassbind entry naming this class, wdfldr calls the bind below at load
 * time, and the four GPIO_CLX_Xxx() inlines in <gpioclx.h> then dispatch
 * through the table filled in here.
 */

#define NDEBUG
#include "gpioclxp.h"

static EVT_WDF_DRIVER_UNLOAD GcxEvtDriverUnload;

static NTSTATUS NTAPI GcxClassInitialize(VOID);
static VOID NTAPI GcxClassDeinitialize(VOID);
static NTSTATUS NTAPI GcxClassBindClient(PWDF_CLASS_BIND_INFO ClassBindInfo,
                                         PWDF_COMPONENT_GLOBALS ComponentGlobals);
static VOID NTAPI GcxClassUnbindClient(PWDF_CLASS_BIND_INFO ClassBindInfo,
                                       PWDF_COMPONENT_GLOBALS ComponentGlobals);

/*
 * 1.1 is the only version Windows registers for this class: the live
 * Control\Wdf\Kmdf\GPIOClx key has a single Versions\1\1 entry.
 */
#define GCX_MAJOR_VERSION 1
#define GCX_MINOR_VERSION 1

WDF_CLASS_LIBRARY_INFO GcxWdfClassLibraryInfo = {
    sizeof(WDF_CLASS_LIBRARY_INFO),
    {
        GCX_MAJOR_VERSION,
        GCX_MINOR_VERSION,
        0,
    },
    GcxClassInitialize,
    GcxClassDeinitialize,
    GcxClassBindClient,
    GcxClassUnbindClient,
};

/*
 * The reference names its own device \Device\MSGpioClassExt. That string is the
 * one the loader resolves a .kmdfclassbind entry against, so it is kept.
 */
#define GCX_CLASS_EXTENSION_NAME L"\\Device\\MSGpioClassExt"

/**
 * @brief
 * Recovers our block from the pointer a client holds.
 *
 * @param[in] ClientGlobals
 * The globals pointer handed to the client at bind time.
 *
 * @return
 * Our block, or NULL if the pointer did not come from this class extension.
 */
PGCX_CLIENT_GLOBALS
GcxGlobalsFromClient(
    _In_ PGPIO_DRIVER_GLOBALS ClientGlobals)
{
    PGCX_CLIENT_GLOBALS Globals;

    if (ClientGlobals == NULL)
    {
        return NULL;
    }

    Globals = CONTAINING_RECORD(ClientGlobals, GCX_CLIENT_GLOBALS, ClientGlobals);
    if (Globals->Signature != GCX_CLIENT_GLOBALS_SIGNATURE)
    {
        return NULL;
    }

    return Globals;
}

static
NTSTATUS
NTAPI
GcxClassInitialize(VOID)
{
    /* Nothing to do at class init */
    return STATUS_SUCCESS;
}

static
VOID
NTAPI
GcxClassDeinitialize(VOID)
{
}

/**
 * @brief
 * Binds one client to this class extension.
 *
 * The client hands us a table it wants filled and a slot for its globals. The
 * requested function count is checked against the accepted counts and anything
 * else is refused: filling a table part-way would leave the client calling
 * through uninitialized slots.
 *
 * @param[in] ClassBindInfo
 * What the client asked for.
 *
 * @param[in] ComponentGlobals
 * Unused. The class library keeps no per-component state.
 *
 * @return
 * STATUS_SUCCESS, or STATUS_INVALID_PARAMETER if the client does not fit.
 */
static
NTSTATUS
NTAPI
GcxClassBindClient(
    PWDF_CLASS_BIND_INFO ClassBindInfo,
    PWDF_COMPONENT_GLOBALS ComponentGlobals)
{
    /* The four exports, in this order */
    static const GPIOCLXFUNC Exports[GpioClxFunctionTableNumEntries] = {
        (GPIOCLXFUNC)GcxRegisterClient,
        (GPIOCLXFUNC)GcxUnregisterClient,
        (GPIOCLXFUNC)GcxProcessAddDevicePreDeviceCreate,
        (GPIOCLXFUNC)GcxProcessAddDevicePostDeviceCreate,
    };
    PGCX_CLIENT_GLOBALS Globals;

    UNREFERENCED_PARAMETER(ComponentGlobals);

    if (ClassBindInfo == NULL || ClassBindInfo->ClassBindInfo == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    *(PGPIO_DRIVER_GLOBALS *)ClassBindInfo->ClassBindInfo = NULL;

    if (ClassBindInfo->Version.Major != GCX_MAJOR_VERSION ||
        ClassBindInfo->Version.Minor > GCX_MINOR_VERSION)
    {
        DPRINT1("GpioClx: client wants %u.%u, this is %u.%u\n",
                ClassBindInfo->Version.Major, ClassBindInfo->Version.Minor,
                GCX_MAJOR_VERSION, GCX_MINOR_VERSION);
        return STATUS_INVALID_PARAMETER;
    }

    if (ClassBindInfo->FunctionTableCount != GpioClxFunctionTableNumEntries)
    {
        DPRINT1("GpioClx: a GPIO %u.%u client should ask for %u functions, this "
                "one asks for %u\n",
                ClassBindInfo->Version.Major, ClassBindInfo->Version.Minor,
                (ULONG)GpioClxFunctionTableNumEntries,
                ClassBindInfo->FunctionTableCount);
        return STATUS_INVALID_PARAMETER;
    }

    Globals = ExAllocatePoolWithTag(NonPagedPool, sizeof(GCX_CLIENT_GLOBALS), GCX_POOL_TAG);
    if (Globals == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(Globals, sizeof(GCX_CLIENT_GLOBALS));
    Globals->Signature = GCX_CLIENT_GLOBALS_SIGNATURE;
    Globals->Version = ClassBindInfo->Version;

    RtlCopyMemory(ClassBindInfo->FunctionTable, Exports, sizeof(Exports));

    /* The client only ever sees the inner block; GcxGlobalsFromClient walks back */
    *(PGPIO_DRIVER_GLOBALS *)ClassBindInfo->ClassBindInfo =
        (PGPIO_DRIVER_GLOBALS)&Globals->ClientGlobals;

    DPRINT("GpioClx: bound a GPIO %u.%u client, %u functions\n",
           ClassBindInfo->Version.Major, ClassBindInfo->Version.Minor,
           (ULONG)GpioClxFunctionTableNumEntries);

    return STATUS_SUCCESS;
}

static
VOID
NTAPI
GcxClassUnbindClient(
    PWDF_CLASS_BIND_INFO ClassBindInfo,
    PWDF_COMPONENT_GLOBALS ComponentGlobals)
{
    PGCX_CLIENT_GLOBALS Globals;

    UNREFERENCED_PARAMETER(ComponentGlobals);

    if (ClassBindInfo == NULL || ClassBindInfo->ClassBindInfo == NULL)
    {
        return;
    }

    Globals = GcxGlobalsFromClient(*(PGPIO_DRIVER_GLOBALS *)ClassBindInfo->ClassBindInfo);
    if (Globals != NULL)
    {
        ExFreePoolWithTag(Globals, GCX_POOL_TAG);
    }

    *(PGPIO_DRIVER_GLOBALS *)ClassBindInfo->ClassBindInfo = NULL;
}

static
VOID
NTAPI
GcxEvtDriverUnload(
    _In_ WDFDRIVER Driver)
{
    UNREFERENCED_PARAMETER(Driver);
}

/*
 * A class extension has no PnP device of its own. It creates a WDF driver so it
 * has an object to hang the class library off, then publishes the library under
 * a name the loader resolves .kmdfclassbind entries against.
 */
/*
 * "D:P" - kernel and system only, nothing from user mode. The WDK declares
 * this in wdmsec.h and defines it in wdmsec.lib, which this tree does not
 * have, so the one string this driver needs is spelled out here. The control
 * device exists only to carry the class name, so nothing outside the kernel
 * has any business opening it.
 */
static const UNICODE_STRING GcxSddlDevObjKernelOnly = RTL_CONSTANT_STRING(L"D:P");

CODE_SEG("INIT")
NTSTATUS
NTAPI
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    DECLARE_CONST_UNICODE_STRING(ClassName, GCX_CLASS_EXTENSION_NAME);
    WDF_DRIVER_CONFIG DriverConfig;
    PWDFDEVICE_INIT DeviceInit;
    WDFDEVICE ControlDevice;
    WDFDRIVER Driver;
    NTSTATUS Status;

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
        DPRINT1("GpioClx: WdfDriverCreate failed 0x%08lX\n", Status);
        return Status;
    }


    /*
     * The name registered below is opened with IoGetDeviceObjectPointer, so a
     * device object has to be carrying it first. A class extension is not a
     * PnP driver and has no device of its own, so it publishes a control
     * device purely to own the name - which is what ucx01000 does, and why
     * that one binds while these two did not: without it the registration
     * fails STATUS_OBJECT_NAME_NOT_FOUND, no class is ever registered, and
     * every client that asks for it fails its own DriverEntry.
     */
    DeviceInit = WdfControlDeviceInitAllocate(Driver, &GcxSddlDevObjKernelOnly);
    if (DeviceInit == NULL)
    {
        DPRINT1("%s: WdfControlDeviceInitAllocate failed\n", "GpioClx");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Status = WdfDeviceInitAssignName(DeviceInit, (PCUNICODE_STRING)&ClassName);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("%s: WdfDeviceInitAssignName failed 0x%08lX\n", "GpioClx", Status);
        WdfDeviceInitFree(DeviceInit);
        return Status;
    }

    Status = WdfDeviceCreate(&DeviceInit, WDF_NO_OBJECT_ATTRIBUTES, &ControlDevice);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("%s: WdfDeviceCreate failed 0x%08lX\n", "GpioClx", Status);
        WdfDeviceInitFree(DeviceInit);
        return Status;
    }

    WdfControlFinishInitializing(ControlDevice);

    Status = WdfRegisterClassLibrary(&GcxWdfClassLibraryInfo,
                                     RegistryPath,
                                     (PUNICODE_STRING)&ClassName);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("GpioClx: WdfRegisterClassLibrary failed 0x%08lX\n", Status);
        return Status;
    }

    return STATUS_SUCCESS;
}
