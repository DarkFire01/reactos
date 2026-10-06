/*
 * PROJECT:     ReactOS Simple Peripheral Bus
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     SpbCx class library registration and client binding
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

/*
 * A controller driver never links an SpbCx import library. It carries a
 * .kmdfclassbind entry naming this class, wdfldr calls ScxClassBindClient at
 * load time, and every SpbXxx() inline in <spbcx.h> then dispatches through the
 * table filled in below. Structured the same way as ucx01000 in this tree.
 */

#define NDEBUG
#include "spbcxp.h"

static EVT_WDF_DRIVER_UNLOAD ScxEvtDriverUnload;

static NTSTATUS NTAPI ScxClassInitialize(VOID);
static VOID NTAPI ScxClassDeinitialize(VOID);
static NTSTATUS NTAPI ScxClassBindClient(PWDF_CLASS_BIND_INFO ClassBindInfo,
                                         PWDF_COMPONENT_GLOBALS ComponentGlobals);
static VOID NTAPI ScxClassUnbindClient(PWDF_CLASS_BIND_INFO ClassBindInfo,
                                       PWDF_COMPONENT_GLOBALS ComponentGlobals);

/*
 * SpbCx 1.0 and 1.1 publish the same thirteen functions, and the bind checks for
 * exactly that count on either minor version, so a 1.0 client and a 1.1 client
 * bind to an identical table.
 */
#define SCX_MAJOR_VERSION 1
#define SCX_MINOR_VERSION 1

WDF_CLASS_LIBRARY_INFO ScxWdfClassLibraryInfo = {
    sizeof(WDF_CLASS_LIBRARY_INFO),
    {
        SCX_MAJOR_VERSION,
        SCX_MINOR_VERSION,
        0,
    },
    ScxClassInitialize,
    ScxClassDeinitialize,
    ScxClassBindClient,
    ScxClassUnbindClient,
};

#define SCX_CLASS_EXTENSION_NAME L"\\Device\\SpbCx"

/*
 * Recovers our block from the pointer the client holds. Every entry point does
 * this before touching anything, because a client that was linked against a
 * different class extension would otherwise be walking our structures.
 */
PSCX_CLIENT_GLOBALS
ScxGlobalsFromClient(
    _In_ PSPB_DRIVER_GLOBALS ClientGlobals)
{
    PSCX_CLIENT_GLOBALS Globals;

    if (ClientGlobals == NULL)
    {
        return NULL;
    }

    Globals = CONTAINING_RECORD(ClientGlobals, SCX_CLIENT_GLOBALS, ClientGlobals);
    if (Globals->Signature != SCX_CLIENT_GLOBALS_SIGNATURE)
    {
        return NULL;
    }

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

/*
 * The client hands us a table it wants filled and a slot for its globals. Both
 * the major version and the exact function count have to match: a client asking
 * for a different count was built against a different contract, and filling its
 * table part-way would leave it calling through uninitialized slots.
 */
static
NTSTATUS
NTAPI
ScxClassBindClient(
    PWDF_CLASS_BIND_INFO ClassBindInfo,
    PWDF_COMPONENT_GLOBALS ComponentGlobals)
{
    static const SPBFUNC Exports[SpbFunctionTableNumEntries] = {
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
        (SPBFUNC)ScxRequestCaptureIoOtherTransferList,
    };
    PSCX_CLIENT_GLOBALS Globals;

    UNREFERENCED_PARAMETER(ComponentGlobals);

    if (ClassBindInfo == NULL || ClassBindInfo->ClassBindInfo == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    *(PSPB_DRIVER_GLOBALS *)ClassBindInfo->ClassBindInfo = NULL;

    if (ClassBindInfo->Version.Major != SCX_MAJOR_VERSION ||
        ClassBindInfo->Version.Minor > SCX_MINOR_VERSION)
    {
        DPRINT1("SpbCx: client wants %u.%u, this is %u.%u\n",
                ClassBindInfo->Version.Major, ClassBindInfo->Version.Minor,
                SCX_MAJOR_VERSION, SCX_MINOR_VERSION);
        return STATUS_INVALID_PARAMETER;
    }

    if (ClassBindInfo->FunctionTableCount != SpbFunctionTableNumEntries)
    {
        DPRINT1("SpbCx: an SPB %u.%u client should ask for %u functions, this "
                "one asks for %u\n",
                ClassBindInfo->Version.Major, ClassBindInfo->Version.Minor,
                (ULONG)SpbFunctionTableNumEntries,
                ClassBindInfo->FunctionTableCount);
        return STATUS_INVALID_PARAMETER;
    }

    Globals = ExAllocatePoolWithTag(NonPagedPool, sizeof(SCX_CLIENT_GLOBALS), SCX_POOL_TAG);
    if (Globals == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(Globals, sizeof(SCX_CLIENT_GLOBALS));
    Globals->Signature = SCX_CLIENT_GLOBALS_SIGNATURE;
    Globals->Version = ClassBindInfo->Version;

    RtlCopyMemory(ClassBindInfo->FunctionTable, Exports, sizeof(Exports));

    /*
     * The client only ever sees the inner block; ScxGlobalsFromClient walks back
     * out to here.
     */
    *(PSPB_DRIVER_GLOBALS *)ClassBindInfo->ClassBindInfo =
        (PSPB_DRIVER_GLOBALS)&Globals->ClientGlobals;

    DPRINT("SpbCx: bound an SPB %u.%u client, %u functions\n",
           ClassBindInfo->Version.Major, ClassBindInfo->Version.Minor,
           (ULONG)SpbFunctionTableNumEntries);

    return STATUS_SUCCESS;
}

static
VOID
NTAPI
ScxClassUnbindClient(
    PWDF_CLASS_BIND_INFO ClassBindInfo,
    PWDF_COMPONENT_GLOBALS ComponentGlobals)
{
    PSCX_CLIENT_GLOBALS Globals;

    UNREFERENCED_PARAMETER(ComponentGlobals);

    if (ClassBindInfo == NULL || ClassBindInfo->ClassBindInfo == NULL)
    {
        return;
    }

    Globals = ScxGlobalsFromClient(*(PSPB_DRIVER_GLOBALS *)ClassBindInfo->ClassBindInfo);
    if (Globals != NULL)
    {
        ExFreePoolWithTag(Globals, SCX_POOL_TAG);
    }

    *(PSPB_DRIVER_GLOBALS *)ClassBindInfo->ClassBindInfo = NULL;
}

static
VOID
NTAPI
ScxEvtDriverUnload(
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
static const UNICODE_STRING ScxSddlDevObjKernelOnly = RTL_CONSTANT_STRING(L"D:P");

CODE_SEG("INIT")
NTSTATUS
NTAPI
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    DECLARE_CONST_UNICODE_STRING(ClassName, SCX_CLASS_EXTENSION_NAME);
    WDF_DRIVER_CONFIG DriverConfig;
    PWDFDEVICE_INIT DeviceInit;
    WDFDEVICE ControlDevice;
    WDFDRIVER Driver;
    NTSTATUS Status;

    WDF_DRIVER_CONFIG_INIT(&DriverConfig, WDF_NO_EVENT_CALLBACK);
    DriverConfig.EvtDriverUnload = ScxEvtDriverUnload;
    DriverConfig.DriverInitFlags = WdfDriverInitNonPnpDriver;

    Status = WdfDriverCreate(DriverObject,
                             RegistryPath,
                             WDF_NO_OBJECT_ATTRIBUTES,
                             &DriverConfig,
                             &Driver);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("SpbCx: WdfDriverCreate failed 0x%08lX\n", Status);
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
    DeviceInit = WdfControlDeviceInitAllocate(Driver, &ScxSddlDevObjKernelOnly);
    if (DeviceInit == NULL)
    {
        DPRINT1("%s: WdfControlDeviceInitAllocate failed\n", "SpbCx");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Status = WdfDeviceInitAssignName(DeviceInit, (PCUNICODE_STRING)&ClassName);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("%s: WdfDeviceInitAssignName failed 0x%08lX\n", "SpbCx", Status);
        WdfDeviceInitFree(DeviceInit);
        return Status;
    }

    Status = WdfDeviceCreate(&DeviceInit, WDF_NO_OBJECT_ATTRIBUTES, &ControlDevice);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("%s: WdfDeviceCreate failed 0x%08lX\n", "SpbCx", Status);
        WdfDeviceInitFree(DeviceInit);
        return Status;
    }

    WdfControlFinishInitializing(ControlDevice);

    Status = WdfRegisterClassLibrary(&ScxWdfClassLibraryInfo,
                                     RegistryPath,
                                     (PUNICODE_STRING)&ClassName);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("SpbCx: WdfRegisterClassLibrary failed 0x%08lX\n", Status);
        return Status;
    }

    return STATUS_SUCCESS;
}
