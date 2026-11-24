/*
 * PROJECT:     ReactOS Intel Cherryview GPIO controller driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Driver entry and GpioClx registration
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "iagpioe.h"

#define NDEBUG
#include <debug.h>

DRIVER_INITIALIZE DriverEntry;
static EVT_WDF_DRIVER_DEVICE_ADD ChvEvtDeviceAdd;
static EVT_WDF_DRIVER_UNLOAD ChvEvtDriverUnload;

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
ChvEvtDeviceAdd(
    _In_ WDFDRIVER Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    WDF_DEVICE_STATE DeviceState;
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
        DPRINT1("Cherryview GPIO: WdfDeviceCreate failed 0x%08lx\n", Status);
        return Status;
    }

    Status = GPIO_CLX_ProcessAddDevicePostDeviceCreate(Driver, Device);
    if (!NT_SUCCESS(Status))
        return Status;

    /* Firmware methods reach the pads through this device, it cannot be turned off */
    WDF_DEVICE_STATE_INIT(&DeviceState);
    DeviceState.NotDisableable = WdfTrue;
    WdfDeviceSetDeviceState(Device, &DeviceState);
    WdfDeviceSetStaticStopRemove(Device, FALSE);

    return STATUS_SUCCESS;
}

static
VOID
NTAPI
ChvEvtDriverUnload(
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

    ChvInitializeRegisterLock();

    WDF_DRIVER_CONFIG_INIT(&Config, ChvEvtDeviceAdd);
    Config.EvtDriverUnload = ChvEvtDriverUnload;

    Status = WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &Config, &Driver);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = GpioClxStubBindClass();
    if (!NT_SUCCESS(Status))
        return Status;

    RtlZeroMemory(&Packet, sizeof(Packet));
    Packet.Version = GPIO_CLIENT_VERSION;
    Packet.Size = sizeof(Packet);
    Packet.ControllerContextSize = sizeof(CHV_CONTROLLER);

    Packet.CLIENT_PrepareController = ChvPrepareController;
    Packet.CLIENT_ReleaseController = ChvReleaseController;
    Packet.CLIENT_StartController = ChvStartController;
    Packet.CLIENT_StopController = ChvStopController;
    Packet.CLIENT_QueryControllerBasicInformation = ChvQueryControllerBasicInformation;

    Packet.CLIENT_EnableInterrupt = ChvEnableInterrupt;
    Packet.CLIENT_DisableInterrupt = ChvDisableInterrupt;
    Packet.CLIENT_UnmaskInterrupt = ChvUnmaskInterrupt;
    Packet.CLIENT_MaskInterrupts = ChvMaskInterrupts;
    Packet.CLIENT_QueryActiveInterrupts = ChvQueryActiveInterrupts;
    Packet.CLIENT_ClearActiveInterrupts = ChvClearActiveInterrupts;
    Packet.CLIENT_ReconfigureInterrupt = ChvReconfigureInterrupt;

    Packet.CLIENT_ConnectIoPins = ChvConnectIoPins;
    Packet.CLIENT_DisconnectIoPins = ChvDisconnectIoPins;
    Packet.CLIENT_ReadGpioPins = ChvReadGpioPins;
    Packet.CLIENT_WriteGpioPins = ChvWriteGpioPins;

    Packet.CLIENT_SaveBankHardwareContext = ChvSaveBankHardwareContext;
    Packet.CLIENT_RestoreBankHardwareContext = ChvRestoreBankHardwareContext;

    Status = GPIO_CLX_RegisterClient(Driver, &Packet, RegistryPath);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Cherryview GPIO: GpioClx registration failed 0x%08lx\n", Status);
        GpioClxStubUnbindClass();
    }

    return Status;
}
