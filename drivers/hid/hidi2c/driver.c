/*
 * PROJECT:     ReactOS HID Stack
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     HID over I2C - driver and PnP
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */


#define NDEBUG
#include "hidi2cp.h"

static EVT_WDF_DRIVER_DEVICE_ADD HidI2cEvtDeviceAdd;
static EVT_WDF_DEVICE_PREPARE_HARDWARE HidI2cEvtPrepareHardware;
static EVT_WDF_DEVICE_RELEASE_HARDWARE HidI2cEvtReleaseHardware;
static EVT_WDF_DEVICE_D0_ENTRY HidI2cEvtD0Entry;
static EVT_WDF_DEVICE_D0_EXIT HidI2cEvtD0Exit;

/*
 * Two resources matter: the connection descriptor naming the I2C bus this
 * peripheral sits on, and the interrupt. The connection is not a bus address --
 * it is the id acpiex minted, which only means anything to the resource hub.
 */
static
NTSTATUS
NTAPI
HidI2cEvtPrepareHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesRaw,
    _In_ WDFCMRESLIST ResourcesTranslated)
{
    PHIDI2C_CONTEXT Context = HidI2cGetContext(Device);
    BOOLEAN FoundConnection = FALSE;
    ULONG Count;
    ULONG Index;

    UNREFERENCED_PARAMETER(ResourcesRaw);

    PAGED_CODE();

    Count = WdfCmResourceListGetCount(ResourcesTranslated);

    for (Index = 0; Index < Count; Index++)
    {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR Descriptor =
            WdfCmResourceListGetDescriptor(ResourcesTranslated, Index);

        if (Descriptor == NULL)
        {
            continue;
        }

        if (Descriptor->Type == CmResourceTypeConnection &&
            Descriptor->u.Connection.Class == CM_RESOURCE_CONNECTION_CLASS_SERIAL &&
            Descriptor->u.Connection.Type == CM_RESOURCE_CONNECTION_TYPE_SERIAL_I2C)
        {
            Context->ConnectionId.LowPart = Descriptor->u.Connection.IdLowPart;
            Context->ConnectionId.HighPart = Descriptor->u.Connection.IdHighPart;
            FoundConnection = TRUE;
        }
    }

    if (!FoundConnection)
    {
        DPRINT1("hidi2c: no I2C connection resource\n");
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    return HidI2cOpenSpbTarget(Context);
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

    HidI2cCloseSpbTarget(HidI2cGetContext(Device));
    return STATUS_SUCCESS;
}

/*
 * Talking to the device needs the bus, which needs the controller in D0. WDF has
 * already brought the I/O target's stack up by the time this runs, so the first
 * transfer here is safe.
 */
static
NTSTATUS
NTAPI
HidI2cEvtD0Entry(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE PreviousState)
{
    PHIDI2C_CONTEXT Context = HidI2cGetContext(Device);

    UNREFERENCED_PARAMETER(PreviousState);

    /* Only the first entry does the full bring-up; a resume just powers on */
    if (Context->ReportDescriptor != NULL)
    {
        return HidI2cSetPower(Context, HIDI2C_POWER_ON);
    }

    return HidI2cInitialize(Context);
}

static
NTSTATUS
NTAPI
HidI2cEvtD0Exit(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE TargetState)
{
    PHIDI2C_CONTEXT Context = HidI2cGetContext(Device);

    UNREFERENCED_PARAMETER(TargetState);

    (VOID)HidI2cSetPower(Context, HIDI2C_POWER_SLEEP);
    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
HidI2cEvtDeviceAdd(
    _In_ WDFDRIVER Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS PnpCallbacks;
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDF_IO_QUEUE_CONFIG QueueConfig;
    WDF_INTERRUPT_CONFIG InterruptConfig;
    PHIDI2C_CONTEXT Context;
    WDFDEVICE Device;
    WDFQUEUE Queue;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(Driver);

    PAGED_CODE();

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&PnpCallbacks);
    PnpCallbacks.EvtDevicePrepareHardware = HidI2cEvtPrepareHardware;
    PnpCallbacks.EvtDeviceReleaseHardware = HidI2cEvtReleaseHardware;
    PnpCallbacks.EvtDeviceD0Entry = HidI2cEvtD0Entry;
    PnpCallbacks.EvtDeviceD0Exit = HidI2cEvtD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &PnpCallbacks);

    /*
     * hidclass owns the device interface and the user-facing names; this driver
     * is only ever reached through mshidkmdf, so it needs no interface of its own.
     */
    WdfDeviceInitSetIoType(DeviceInit, WdfDeviceIoBuffered);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, HIDI2C_CONTEXT);

    Status = WdfDeviceCreate(&DeviceInit, &Attributes, &Device);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("hidi2c: WdfDeviceCreate failed 0x%08lX\n", Status);
        return Status;
    }

    Context = HidI2cGetContext(Device);
    RtlZeroMemory(Context, sizeof(HIDI2C_CONTEXT));
    Context->Device = Device;

    /* Everything hidclass sends arrives as internal device control */
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&QueueConfig, WdfIoQueueDispatchParallel);
    QueueConfig.EvtIoInternalDeviceControl = HidI2cEvtInternalDeviceControl;

    Status = WdfIoQueueCreate(Device, &QueueConfig, WDF_NO_OBJECT_ATTRIBUTES, &Queue);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    /*
     * The read loop's parking queue. Manual dispatch because a read waits for
     * the device to interrupt rather than for the framework to present it.
     */
    WDF_IO_QUEUE_CONFIG_INIT(&QueueConfig, WdfIoQueueDispatchManual);

    Status = WdfIoQueueCreate(Device, &QueueConfig, WDF_NO_OBJECT_ATTRIBUTES,
                              &Context->ReadQueue);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    WDF_INTERRUPT_CONFIG_INIT(&InterruptConfig,
                              HidI2cEvtInterruptIsr,
                              HidI2cEvtInterruptDpc);

    /*
     * The DPC talks to the bus, which means blocking, so it has to run at
     * passive level. WDF gives a passive-level DPC when the interrupt is created
     * with a passive-level lock.
     */
    InterruptConfig.PassiveHandling = TRUE;

    Status = WdfInterruptCreate(Device, &InterruptConfig,
                                WDF_NO_OBJECT_ATTRIBUTES, &Context->Interrupt);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("hidi2c: WdfInterruptCreate failed 0x%08lX\n", Status);
        return Status;
    }

    return STATUS_SUCCESS;
}

CODE_SEG("INIT")
NTSTATUS
NTAPI
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG Config;

    WDF_DRIVER_CONFIG_INIT(&Config, HidI2cEvtDeviceAdd);

    return WdfDriverCreate(DriverObject,
                           RegistryPath,
                           WDF_NO_OBJECT_ATTRIBUTES,
                           &Config,
                           WDF_NO_HANDLE);
}
