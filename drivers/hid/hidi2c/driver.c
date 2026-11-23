/*
 * PROJECT:     ReactOS HID Stack
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     HID over I2C driver entry, PnP and power
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#include "hidi2c.h"

#define NDEBUG
#include <debug.h>

DRIVER_INITIALIZE DriverEntry;
static EVT_WDF_DRIVER_DEVICE_ADD HidI2cEvtDeviceAdd;
static EVT_WDF_DEVICE_PREPARE_HARDWARE HidI2cEvtPrepareHardware;
static EVT_WDF_DEVICE_RELEASE_HARDWARE HidI2cEvtReleaseHardware;
static EVT_WDF_DEVICE_D0_ENTRY HidI2cEvtD0Entry;
static EVT_WDF_DEVICE_D0_ENTRY_POST_INTERRUPTS_ENABLED HidI2cEvtD0EntryPostInterruptsEnabled;
static EVT_WDF_DEVICE_D0_EXIT HidI2cEvtD0Exit;
static EVT_WDF_DEVICE_SELF_MANAGED_IO_SUSPEND HidI2cEvtSelfManagedIoSuspend;

#ifdef ALLOC_PRAGMA
#pragma alloc_text(INIT, DriverEntry)
#endif

/**
 * @brief
 * Needs both the I2C connection and the interrupt; opens the connection and asks ACPI
 * where the HID descriptor lives.
 */
static
NTSTATUS
NTAPI
HidI2cEvtPrepareHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesRaw,
    _In_ WDFCMRESLIST ResourcesTranslated)
{
    PHIDI2C_DEVICE Context = HidI2cGetDevice(Device);
    PCM_PARTIAL_RESOURCE_DESCRIPTOR Resource;
    BOOLEAN HaveConnection = FALSE;
    BOOLEAN HaveInterrupt = FALSE;
    ULONG Count;
    ULONG Index;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(ResourcesRaw);

    PAGED_CODE();

    Count = WdfCmResourceListGetCount(ResourcesTranslated);
    for (Index = 0; Index < Count && !(HaveConnection && HaveInterrupt); Index++)
    {
        Resource = WdfCmResourceListGetDescriptor(ResourcesTranslated, Index);
        if (Resource == NULL)
            continue;

        if (Resource->Type == CmResourceTypeInterrupt)
        {
            HaveInterrupt = TRUE;
        }
        else if (Resource->Type == CmResourceTypeConnection &&
                 Resource->u.Connection.Class == CM_RESOURCE_CONNECTION_CLASS_SERIAL &&
                 Resource->u.Connection.Type == CM_RESOURCE_CONNECTION_TYPE_SERIAL_I2C)
        {
            Context->ConnectionId.LowPart = Resource->u.Connection.IdLowPart;
            Context->ConnectionId.HighPart = Resource->u.Connection.IdHighPart;
            HaveConnection = TRUE;
        }
    }

    if (!HaveConnection || !HaveInterrupt)
    {
        DPRINT1("hidi2c: connection %u, interrupt %u\n", HaveConnection, HaveInterrupt);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Status = HidI2cOpenTarget(Context);
    if (!NT_SUCCESS(Status))
        return Status;

    return HidI2cQueryDescriptorRegister(Context);
}

static
NTSTATUS
NTAPI
HidI2cEvtReleaseHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesTranslated)
{
    UNREFERENCED_PARAMETER(ResourcesTranslated);

    PAGED_CODE();

    HidI2cCloseTarget(HidI2cGetDevice(Device));
    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
HidI2cEvtD0Entry(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE PreviousState)
{
    PHIDI2C_DEVICE Context = HidI2cGetDevice(Device);
    NTSTATUS Status;

    Status = HidI2cInitialize(Context, PreviousState);
    if (NT_SUCCESS(Status))
        HidI2cCompleteIdleRequest(Context);

    return Status;
}

/* The reset answer is an interrupt, so the reset waits until they are connected */
static
NTSTATUS
NTAPI
HidI2cEvtD0EntryPostInterruptsEnabled(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE PreviousState)
{
    if (PreviousState != WdfPowerDeviceD3Final)
        return STATUS_SUCCESS;

    return HidI2cReset(HidI2cGetDevice(Device));
}

static
NTSTATUS
NTAPI
HidI2cEvtD0Exit(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE TargetState)
{
    return HidI2cShutdown(HidI2cGetDevice(Device), TargetState);
}

static
NTSTATUS
NTAPI
HidI2cEvtSelfManagedIoSuspend(
    _In_ WDFDEVICE Device)
{
    WdfTimerStop(HidI2cGetDevice(Device)->ResetTimer, TRUE);
    return STATUS_SUCCESS;
}

/**
 * @brief
 * The default queue for hidclass requests, and the manual queues requests wait in.
 */
static
NTSTATUS
NTAPI
HidI2cCreateQueues(
    _Inout_ PHIDI2C_DEVICE Context)
{
    WDF_IO_QUEUE_CONFIG QueueConfig;
    WDF_OBJECT_ATTRIBUTES Attributes;
    NTSTATUS Status;

    /* Handlers talk to the bus synchronously */
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&QueueConfig, WdfIoQueueDispatchParallel);
    QueueConfig.EvtIoInternalDeviceControl = HidI2cEvtInternalDeviceControl;
    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ExecutionLevel = WdfExecutionLevelPassive;

    Status = WdfIoQueueCreate(Context->Device, &QueueConfig, &Attributes, &Context->DefaultQueue);
    if (!NT_SUCCESS(Status))
        return Status;

    WDF_IO_QUEUE_CONFIG_INIT(&QueueConfig, WdfIoQueueDispatchManual);
    Status = WdfIoQueueCreate(Context->Device, &QueueConfig, WDF_NO_OBJECT_ATTRIBUTES, &Context->ReadQueue);
    if (!NT_SUCCESS(Status))
        return Status;

    /* These two must hand requests back while the device is still below D0 */
    WDF_IO_QUEUE_CONFIG_INIT(&QueueConfig, WdfIoQueueDispatchManual);
    QueueConfig.PowerManaged = WdfFalse;
    Status = WdfIoQueueCreate(Context->Device,
                              &QueueConfig,
                              WDF_NO_OBJECT_ATTRIBUTES,
                              &Context->ResetNotifyQueue);
    if (!NT_SUCCESS(Status))
        return Status;

    WDF_IO_QUEUE_CONFIG_INIT(&QueueConfig, WdfIoQueueDispatchManual);
    QueueConfig.PowerManaged = WdfFalse;
    return WdfIoQueueCreate(Context->Device, &QueueConfig, WDF_NO_OBJECT_ATTRIBUTES, &Context->IdleQueue);
}

static
NTSTATUS
NTAPI
HidI2cEvtDeviceAdd(
    _In_ WDFDRIVER Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS PnpCallbacks;
    WDF_INTERRUPT_CONFIG InterruptConfig;
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDF_TIMER_CONFIG TimerConfig;
    WDF_DEVICE_STATE DeviceState;
    PHIDI2C_DEVICE Context;
    WDFDEVICE Device;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(Driver);

    PAGED_CODE();

    /* hidclass above owns power policy for the stack */
    WdfDeviceInitSetPowerPolicyOwnership(DeviceInit, FALSE);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&PnpCallbacks);
    PnpCallbacks.EvtDevicePrepareHardware = HidI2cEvtPrepareHardware;
    PnpCallbacks.EvtDeviceReleaseHardware = HidI2cEvtReleaseHardware;
    PnpCallbacks.EvtDeviceD0Entry = HidI2cEvtD0Entry;
    PnpCallbacks.EvtDeviceD0EntryPostInterruptsEnabled = HidI2cEvtD0EntryPostInterruptsEnabled;
    PnpCallbacks.EvtDeviceD0Exit = HidI2cEvtD0Exit;
    PnpCallbacks.EvtDeviceSelfManagedIoSuspend = HidI2cEvtSelfManagedIoSuspend;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &PnpCallbacks);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, HIDI2C_DEVICE);
    Status = WdfDeviceCreate(&DeviceInit, &Attributes, &Device);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("hidi2c: WdfDeviceCreate failed 0x%08lx\n", Status);
        return Status;
    }

    Context = HidI2cGetDevice(Device);
    Context->Device = Device;

    WDF_DEVICE_STATE_INIT(&DeviceState);
    DeviceState.NotDisableable = WdfFalse;
    WdfDeviceSetDeviceState(Device, &DeviceState);

    Status = HidI2cCreateQueues(Context);
    if (!NT_SUCCESS(Status))
        return Status;

    /* Passive, since giving up on the reset writes the registry */
    WDF_TIMER_CONFIG_INIT(&TimerConfig, HidI2cEvtResetTimer);
    TimerConfig.AutomaticSerialization = FALSE;
    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ParentObject = Device;
    Attributes.ExecutionLevel = WdfExecutionLevelPassive;

    Status = WdfTimerCreate(&TimerConfig, &Attributes, &Context->ResetTimer);
    if (!NT_SUCCESS(Status))
        return Status;

    /* The ISR reads the report over the bus, so it runs at passive level */
    WDF_INTERRUPT_CONFIG_INIT(&InterruptConfig, HidI2cEvtInterruptIsr, NULL);
    InterruptConfig.PassiveHandling = TRUE;

    Status = WdfInterruptCreate(Device, &InterruptConfig, WDF_NO_OBJECT_ATTRIBUTES, &Context->Interrupt);
    if (!NT_SUCCESS(Status))
        DPRINT1("hidi2c: WdfInterruptCreate failed 0x%08lx\n", Status);

    return Status;
}

NTSTATUS
NTAPI
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG Config;

    WDF_DRIVER_CONFIG_INIT(&Config, HidI2cEvtDeviceAdd);
    Config.DriverPoolTag = HIDI2C_POOL_TAG;

    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &Config, WDF_NO_HANDLE);
}
