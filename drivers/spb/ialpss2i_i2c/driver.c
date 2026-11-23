/*
 * PROJECT:     ReactOS Simple Peripheral Bus
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Intel LPSS I2C controller driver and SpbCx callbacks
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#include "ialpss2i_i2c.h"

/* initguid first so GUID_BUS_INTERFACE_STANDARD gets storage here */
#include <initguid.h>
#include <wdmguid.h>

#define NDEBUG
#include <debug.h>

DRIVER_INITIALIZE DriverEntry;
static EVT_WDF_DRIVER_DEVICE_ADD Li2cEvtDeviceAdd;
static EVT_WDF_DRIVER_UNLOAD Li2cEvtDriverUnload;
static EVT_WDF_DEVICE_PREPARE_HARDWARE Li2cEvtPrepareHardware;
static EVT_WDF_DEVICE_RELEASE_HARDWARE Li2cEvtReleaseHardware;
static EVT_WDF_DEVICE_D0_ENTRY Li2cEvtD0Entry;
static EVT_WDF_DEVICE_D0_EXIT Li2cEvtD0Exit;
static EVT_SPB_TARGET_CONNECT Li2cEvtTargetConnect;

#ifdef ALLOC_PRAGMA
#pragma alloc_text(INIT, DriverEntry)
#endif

/* The I2C part of an ACPI I2cSerialBus descriptor, right after the generic header */
#include <pshpack1.h>
typedef struct _LI2C_SERIAL_BUS_DESCRIPTOR
{
    PNP_SERIAL_BUS_DESCRIPTOR Header;
    ULONG ConnectionSpeed;
    USHORT SlaveAddress;
} LI2C_SERIAL_BUS_DESCRIPTOR, *PLI2C_SERIAL_BUS_DESCRIPTOR;
#include <poppack.h>

/* Type specific flag selecting 10 bit addressing */
#define LI2C_SERIAL_BUS_10BIT 0x0001

/**
 * @brief
 * Reads the address, speed and addressing mode of a new target from its descriptor.
 */
static
NTSTATUS
NTAPI
Li2cEvtTargetConnect(
    _In_ WDFDEVICE Controller,
    _In_ SPBTARGET SpbTarget)
{
    PRH_QUERY_CONNECTION_PROPERTIES_OUTPUT_BUFFER Connection;
    PLI2C_SERIAL_BUS_DESCRIPTOR Descriptor;
    SPB_CONNECTION_PARAMETERS Parameters;
    PLI2C_TARGET Target;

    UNREFERENCED_PARAMETER(Controller);

    PAGED_CODE();

    Target = Li2cGetTarget(SpbTargetGetFileObject(SpbTarget));
    if (Target == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    SPB_CONNECTION_PARAMETERS_INIT(&Parameters);
    SpbTargetGetConnectionParameters(SpbTarget, &Parameters);

    Connection = Parameters.ConnectionParameters;
    if (Connection == NULL || Connection->PropertiesLength < sizeof(PNP_SERIAL_BUS_DESCRIPTOR))
        return STATUS_INVALID_PARAMETER;

    Descriptor = (PLI2C_SERIAL_BUS_DESCRIPTOR)Connection->ConnectionProperties;
    if (Descriptor->Header.SerialBusType != SERIAL_BUS_I2C_DESCRIPTOR_TYPE)
    {
        DPRINT1("iaLPSS2 I2C: connection is bus type %u\n", Descriptor->Header.SerialBusType);
        return STATUS_INVALID_PARAMETER;
    }

    if (Connection->PropertiesLength < sizeof(*Descriptor))
        return STATUS_INVALID_PARAMETER;

    Target->Address = Descriptor->SlaveAddress;
    Target->Speed = Descriptor->ConnectionSpeed;
    Target->TenBit = (Descriptor->Header.TypeSpecificFlags & LI2C_SERIAL_BUS_10BIT) != 0;

    DPRINT("iaLPSS2 I2C: target 0x%02X at %lu Hz\n", Target->Address, Target->Speed);
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Reads the PCI device id and revision, which pick the default timing.
 */
static
VOID
NTAPI
Li2cQueryPciIdentity(
    _Inout_ PLI2C_DEVICE Device)
{
    BUS_INTERFACE_STANDARD Bus;
    USHORT DeviceId = 0;
    UCHAR Revision = 0xFF;
    NTSTATUS Status;

    PAGED_CODE();

    Status = WdfFdoQueryForInterface(Device->Device,
                                     &GUID_BUS_INTERFACE_STANDARD,
                                     (PINTERFACE)&Bus,
                                     sizeof(Bus),
                                     1,
                                     NULL);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("iaLPSS2 I2C: no PCI bus interface 0x%08lx\n", Status);
        return;
    }

    if (Bus.GetBusData(Bus.Context,
                       PCI_WHICHSPACE_CONFIG,
                       &DeviceId,
                       FIELD_OFFSET(PCI_COMMON_HEADER, DeviceID),
                       sizeof(DeviceId)) == sizeof(DeviceId))
    {
        Device->PciDeviceId = DeviceId;
    }

    if (Bus.GetBusData(Bus.Context,
                       PCI_WHICHSPACE_CONFIG,
                       &Revision,
                       FIELD_OFFSET(PCI_COMMON_HEADER, RevisionID),
                       sizeof(Revision)) == sizeof(Revision))
    {
        Device->Revision = Revision;
    }

    if (Bus.InterfaceDereference != NULL)
        Bus.InterfaceDereference(Bus.Context);
}

/**
 * @brief
 * Maps the controller BAR; one or two memory resources and one interrupt are expected.
 */
static
NTSTATUS
NTAPI
Li2cEvtPrepareHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesRaw,
    _In_ WDFCMRESLIST ResourcesTranslated)
{
    PLI2C_DEVICE Context = Li2cGetDevice(Device);
    PCM_PARTIAL_RESOURCE_DESCRIPTOR Descriptor;
    ULONG Interrupts = 0;
    ULONG Memories = 0;
    ULONG Count;
    ULONG Index;

    UNREFERENCED_PARAMETER(ResourcesRaw);

    PAGED_CODE();

    Count = WdfCmResourceListGetCount(ResourcesTranslated);
    for (Index = 0; Index < Count; Index++)
    {
        Descriptor = WdfCmResourceListGetDescriptor(ResourcesTranslated, Index);
        if (Descriptor == NULL)
            continue;

        if (Descriptor->Type == CmResourceTypeInterrupt)
        {
            Interrupts++;
            continue;
        }

        if (Descriptor->Type != CmResourceTypeMemory)
            continue;

        /* The second BAR mirrors PCI config space and is never touched */
        if (Memories++ != 0)
            continue;

        Context->CoreLength = Descriptor->u.Memory.Length;
        Context->CorePhysical = Descriptor->u.Memory.Start;
        Context->Core = MmMapIoSpace(Descriptor->u.Memory.Start, Context->CoreLength, MmNonCached);
        if (Context->Core == NULL)
            return STATUS_INSUFFICIENT_RESOURCES;

        Context->Private = Context->Core + LPSS_PRIVATE;
    }

    if (Memories < 1 || Memories > 2 || Interrupts != 1 || Context->CoreLength < LPSS_PRIVATE + 0x100)
    {
        DPRINT1("iaLPSS2 I2C: %lu memory and %lu interrupt resources\n", Memories, Interrupts);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Li2cQueryPciIdentity(Context);
    Li2cLoadPlatformDefaults(Context);
    Li2cLoadAcpiTiming(Context);
    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
Li2cEvtReleaseHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesTranslated)
{
    PLI2C_DEVICE Context = Li2cGetDevice(Device);

    UNREFERENCED_PARAMETER(ResourcesTranslated);

    PAGED_CODE();

    if (Context->Core != NULL)
    {
        MmUnmapIoSpace(Context->Core, Context->CoreLength);
        Context->Core = NULL;
        Context->Private = NULL;
    }

    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
Li2cEvtD0Entry(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE PreviousState)
{
    UNREFERENCED_PARAMETER(PreviousState);

    Li2cHardwareInitialize(Li2cGetDevice(Device));
    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
Li2cEvtD0Exit(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE TargetState)
{
    UNREFERENCED_PARAMETER(TargetState);

    Li2cHardwareQuiesce(Li2cGetDevice(Device));
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Creates the controller. SpbDeviceInitConfig has to run before any other device init call.
 */
static
NTSTATUS
NTAPI
Li2cEvtDeviceAdd(
    _In_ WDFDRIVER Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS PnpCallbacks;
    SPB_CONTROLLER_CONFIG ControllerConfig;
    WDF_INTERRUPT_CONFIG InterruptConfig;
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDF_TIMER_CONFIG TimerConfig;
    PLI2C_DEVICE Context;
    WDFDEVICE Device;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(Driver);

    PAGED_CODE();

    Status = SpbDeviceInitConfig(DeviceInit);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("iaLPSS2 I2C: SpbDeviceInitConfig failed 0x%08lx\n", Status);
        return Status;
    }

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&PnpCallbacks);
    PnpCallbacks.EvtDevicePrepareHardware = Li2cEvtPrepareHardware;
    PnpCallbacks.EvtDeviceReleaseHardware = Li2cEvtReleaseHardware;
    PnpCallbacks.EvtDeviceD0Entry = Li2cEvtD0Entry;
    PnpCallbacks.EvtDeviceD0Exit = Li2cEvtD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &PnpCallbacks);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, LI2C_DEVICE);
    Status = WdfDeviceCreate(&DeviceInit, &Attributes, &Device);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("iaLPSS2 I2C: WdfDeviceCreate failed 0x%08lx\n", Status);
        return Status;
    }

    Context = Li2cGetDevice(Device);
    Context->Device = Device;

    SPB_CONTROLLER_CONFIG_INIT(&ControllerConfig);
    ControllerConfig.PowerManaged = WdfTrue;
    ControllerConfig.EvtSpbTargetConnect = Li2cEvtTargetConnect;
    ControllerConfig.EvtSpbIoRead = Li2cEvtIoRead;
    ControllerConfig.EvtSpbIoWrite = Li2cEvtIoWrite;
    ControllerConfig.EvtSpbIoSequence = Li2cEvtIoSequence;

    Status = SpbDeviceInitialize(Device, &ControllerConfig);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("iaLPSS2 I2C: SpbDeviceInitialize failed 0x%08lx\n", Status);
        return Status;
    }

    /* SpbCx hangs this on every target before EvtSpbTargetConnect runs */
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, LI2C_TARGET);
    SpbControllerSetTargetAttributes(Device, &Attributes);

    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ParentObject = Device;
    Status = WdfSpinLockCreate(&Attributes, &Context->Lock);
    if (!NT_SUCCESS(Status))
        return Status;

    WDF_TIMER_CONFIG_INIT(&TimerConfig, Li2cEvtDelayTimer);
    TimerConfig.AutomaticSerialization = FALSE;
    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ParentObject = Device;
    Status = WdfTimerCreate(&TimerConfig, &Attributes, &Context->DelayTimer);
    if (!NT_SUCCESS(Status))
        return Status;

    WDF_INTERRUPT_CONFIG_INIT(&InterruptConfig, Li2cEvtInterruptIsr, Li2cEvtInterruptDpc);
    Status = WdfInterruptCreate(Device, &InterruptConfig, WDF_NO_OBJECT_ATTRIBUTES, &Context->Interrupt);
    if (!NT_SUCCESS(Status))
        DPRINT1("iaLPSS2 I2C: WdfInterruptCreate failed 0x%08lx\n", Status);

    return Status;
}

static
VOID
NTAPI
Li2cEvtDriverUnload(
    _In_ WDFDRIVER Driver)
{
    UNREFERENCED_PARAMETER(Driver);

    SpbCxStubUnbindClass();
}

NTSTATUS
NTAPI
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG Config;
    NTSTATUS Status;

    WDF_DRIVER_CONFIG_INIT(&Config, Li2cEvtDeviceAdd);
    Config.EvtDriverUnload = Li2cEvtDriverUnload;
    Config.DriverPoolTag = LI2C_POOL_TAG;

    Status = WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &Config, WDF_NO_HANDLE);
    if (!NT_SUCCESS(Status))
        return Status;

    return SpbCxStubBindClass();
}
