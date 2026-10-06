/*
 * PROJECT:     ReactOS ACPI Platform Extensions
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Resource Hub driver entry and control device
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

/*
 * The hub is a KMDF driver with no PnP device of its own. It owns one control
 * device, \Device\RESOURCE_HUB, that everything else opens by name. ACPI starts
 * it on demand (the real acpi.sys ZwLoadDriver's the acpiex service before
 * ZwOpenFile'ing the hub device).
 */

#include "rhpriv.h"

static EVT_WDF_DRIVER_UNLOAD RhEvtDriverUnload;
static EVT_WDF_IO_QUEUE_IO_READ RhEvtProcessReadRequest;
static EVT_WDF_IO_QUEUE_IO_WRITE RhEvtProcessWriteRequest;

/*
 * Parallel dispatch, passive execution level and no synchronization scope:
 * every handler below can block, and no framework lock is taken because a
 * create walks to the provider and may open an I/O target underneath.
 */
static
NTSTATUS
RhpSetupIoQueues(
    _In_ WDFDEVICE Device)
{
    WDF_IO_QUEUE_CONFIG QueueConfig;
    WDF_OBJECT_ATTRIBUTES QueueAttributes;
    WDFQUEUE Queue;

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&QueueConfig, WdfIoQueueDispatchParallel);
    QueueConfig.EvtIoRead = RhEvtProcessReadRequest;
    QueueConfig.EvtIoWrite = RhEvtProcessWriteRequest;
    QueueConfig.EvtIoDeviceControl = RhEvtProcessDeviceIoControl;

    WDF_OBJECT_ATTRIBUTES_INIT(&QueueAttributes);
    QueueAttributes.ExecutionLevel = WdfExecutionLevelPassive;
    QueueAttributes.SynchronizationScope = WdfSynchronizationScopeNone;

    return WdfIoQueueCreate(Device, &QueueConfig, &QueueAttributes, &Queue);
}

/*
 * The symbolic link, created once the control device is finished.
 */
static
NTSTATUS
RhpInitialize(
    _In_ WDFDEVICE Device)
{
    DECLARE_CONST_UNICODE_STRING(SymbolicLink, RESOURCE_HUB_SYMBOLIC_NAME);

    return WdfDeviceCreateSymbolicLink(Device, &SymbolicLink);
}

static
VOID
NTAPI
RhEvtDriverUnload(
    _In_ WDFDRIVER Driver)
{
    UNREFERENCED_PARAMETER(Driver);
}

/*
 * The hub exposes no read/write semantics of its own; a reparsed open never
 * comes back here, so anything that reaches these was aimed at the hub device
 * itself.
 */
static
VOID
NTAPI
RhEvtProcessReadRequest(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t Length)
{
    UNREFERENCED_PARAMETER(Queue);
    UNREFERENCED_PARAMETER(Length);

    WdfRequestComplete(Request, STATUS_NOT_SUPPORTED);
}

static
VOID
NTAPI
RhEvtProcessWriteRequest(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t Length)
{
    UNREFERENCED_PARAMETER(Queue);
    UNREFERENCED_PARAMETER(Length);

    WdfRequestComplete(Request, STATUS_NOT_SUPPORTED);
}

/*
 * SYSTEM and Administrators get all access, and so does every logged-on user,
 * because a peripheral driver opening its own controller runs in arbitrary
 * context.
 */
NTSTATUS
NTAPI
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    DECLARE_CONST_UNICODE_STRING(DeviceName, RESOURCE_HUB_DEVICE_NAME);
    DECLARE_CONST_UNICODE_STRING(DeviceSddl,
        L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;UD)");
    WDF_DRIVER_CONFIG DriverConfig;
    WDF_OBJECT_ATTRIBUTES DeviceAttributes;
    PWDFDEVICE_INIT DeviceInit;
    PRH_DEVICE_CONTEXT DeviceContext;
    WDFDRIVER Driver;
    WDFDEVICE Device;
    NTSTATUS Status;

    WDF_DRIVER_CONFIG_INIT(&DriverConfig, WDF_NO_EVENT_CALLBACK);
    DriverConfig.EvtDriverUnload = RhEvtDriverUnload;
    DriverConfig.DriverInitFlags = WdfDriverInitNonPnpDriver;

    Status = WdfDriverCreate(DriverObject,
                             RegistryPath,
                             WDF_NO_OBJECT_ATTRIBUTES,
                             &DriverConfig,
                             &Driver);
    if (!NT_SUCCESS(Status))
        return Status;

    DeviceInit = WdfControlDeviceInitAllocate(Driver, &DeviceSddl);
    if (DeviceInit == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = WdfDeviceInitAssignName(DeviceInit, &DeviceName);
    if (!NT_SUCCESS(Status))
        goto Failure;

    /*
     * The whole point of the driver. Taking IRP_MJ_CREATE before the framework
     * sees it is what lets a create be answered with STATUS_REPARSE, which WDF
     * cannot express through its own file-object callbacks.
     */
    Status = WdfDeviceInitAssignWdmIrpPreprocessCallback(DeviceInit,
                                                         RhWdmDeviceFileCreate,
                                                         IRP_MJ_CREATE,
                                                         NULL,
                                                         0);
    if (!NT_SUCCESS(Status))
        goto Failure;

    WdfDeviceInitSetIoType(DeviceInit, WdfDeviceIoBuffered);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&DeviceAttributes, RH_DEVICE_CONTEXT);

    Status = WdfDeviceCreate(&DeviceInit, &DeviceAttributes, &Device);
    if (!NT_SUCCESS(Status))
        goto Failure;

    DeviceContext = RhGetDeviceContext(Device);
    DeviceContext->Device = Device;
    KeInitializeSpinLock(&DeviceContext->Lock);
    InitializeListHead(&DeviceContext->ConnectionList);
    InitializeListHead(&DeviceContext->ProviderList);

    Status = RhpSetupIoQueues(Device);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = RhpInitialize(Device);
    if (!NT_SUCCESS(Status))
        return Status;

    /* Until this runs the device rejects every open */
    WdfControlFinishInitializing(Device);

    return STATUS_SUCCESS;

Failure:
    /*
     * WdfDeviceCreate consumes DeviceInit on success only; anything that fails
     * before it still owns the allocation.
     */
    WdfDeviceInitFree(DeviceInit);
    return Status;
}
