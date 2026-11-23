/*
 * PROJECT:     ReactOS Intel LPSS GPIO controller driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Driver entry and GpioClx registration
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "ialpss2i_gpio.h"

#define NDEBUG
#include <debug.h>

DRIVER_INITIALIZE DriverEntry;
static EVT_WDF_DRIVER_DEVICE_ADD IGpioEvtDeviceAdd;
static EVT_WDF_DRIVER_UNLOAD IGpioEvtDriverUnload;

#ifdef ALLOC_PRAGMA
#pragma alloc_text(INIT, DriverEntry)
#endif

/**
 * @brief
 * Creates the controller device with GpioClx hooked in on both sides of WdfDeviceCreate.
 */
static
NTSTATUS
NTAPI
IGpioEvtDeviceAdd(
    _In_ WDFDRIVER Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDFDEVICE Device;
    NTSTATUS Status;

    PAGED_CODE();

    Status = GPIO_CLX_ProcessAddDevicePreDeviceCreate(Driver, DeviceInit, &Attributes);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = WdfDeviceCreate(&DeviceInit, &Attributes, &Device);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("iaLPSS2 GPIO: WdfDeviceCreate failed 0x%08lx\n", Status);
        return Status;
    }

    return GPIO_CLX_ProcessAddDevicePostDeviceCreate(Driver, Device);
}

static
VOID
NTAPI
IGpioEvtDriverUnload(
    _In_ WDFDRIVER Driver)
{
    GPIO_CLX_UnregisterClient(Driver);
    GpioClxStubUnbindClass();
}

NTSTATUS
NTAPI
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    GPIO_CLIENT_REGISTRATION_PACKET Packet;
    WDF_DRIVER_CONFIG Config;
    WDFDRIVER Driver;
    NTSTATUS Status;

    WDF_DRIVER_CONFIG_INIT(&Config, IGpioEvtDeviceAdd);
    Config.EvtDriverUnload = IGpioEvtDriverUnload;

    Status = WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &Config, &Driver);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = GpioClxStubBindClass();
    if (!NT_SUCCESS(Status))
        return Status;

    RtlZeroMemory(&Packet, sizeof(Packet));
    Packet.Version = GPIO_CLIENT_VERSION;
    Packet.Size = sizeof(Packet);
    Packet.ControllerContextSize = sizeof(IGPIO_CONTROLLER);

    Packet.CLIENT_PrepareController = IGpioPrepareController;
    Packet.CLIENT_ReleaseController = IGpioReleaseController;
    Packet.CLIENT_StartController = IGpioStartController;
    Packet.CLIENT_StopController = IGpioStopController;
    Packet.CLIENT_QueryControllerBasicInformation = IGpioQueryControllerBasicInformation;

    Packet.CLIENT_EnableInterrupt = IGpioEnableInterrupt;
    Packet.CLIENT_DisableInterrupt = IGpioDisableInterrupt;
    Packet.CLIENT_UnmaskInterrupt = IGpioUnmaskInterrupt;
    Packet.CLIENT_MaskInterrupts = IGpioMaskInterrupts;
    Packet.CLIENT_QueryActiveInterrupts = IGpioQueryActiveInterrupts;
    Packet.CLIENT_ClearActiveInterrupts = IGpioClearActiveInterrupts;
    Packet.CLIENT_ReconfigureInterrupt = IGpioReconfigureInterrupt;
    Packet.CLIENT_QueryEnabledInterrupts = IGpioQueryEnabledInterrupts;

    Packet.CLIENT_ConnectIoPins = IGpioConnectIoPins;
    Packet.CLIENT_DisconnectIoPins = IGpioDisconnectIoPins;
    Packet.CLIENT_ReadGpioPins = IGpioReadGpioPins;
    Packet.CLIENT_WriteGpioPins = IGpioWriteGpioPins;

    Packet.CLIENT_SaveBankHardwareContext = IGpioSaveBankHardwareContext;
    Packet.CLIENT_RestoreBankHardwareContext = IGpioRestoreBankHardwareContext;

    Status = GPIO_CLX_RegisterClient(Driver, &Packet, RegistryPath);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("iaLPSS2 GPIO: GpioClx registration failed 0x%08lx\n", Status);
        GpioClxStubUnbindClass();
    }

    return Status;
}
