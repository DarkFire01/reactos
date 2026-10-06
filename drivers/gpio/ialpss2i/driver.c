/*
 * PROJECT:     ReactOS Intel LPSS GPIO controller driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Driver entry and GpioClx registration
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/**
 * @file
 * @brief
 * Becoming a GpioClx client.
 *
 *c:504 (DriverEntry) and :361
 * (GpioEvtDeviceAdd). The reference builds the registration packet on the
 * stack and hands it to the class extension through its function table; the
 * packet is copied, so a stack block is enough.
 */

#include "gpiopriv.h"

/**
 * @brief
 * Hands one controller to GpioClx.
 *
 * The class extension owns the device object, so the two calls either side of
 * WdfDeviceCreate are what let it put its own context on it: the pre-create
 * call fills in the attributes to create the device with, and the post-create
 * call is where GpioClx attaches itself.
 *
 * @param[in] Driver
 * This driver.
 *
 * @param[in,out] DeviceInit
 * The initialization block for the device being added.
 *
 * @return
 * STATUS_SUCCESS, or the failure that stopped the device coming up.
 */
NTSTATUS
NTAPI
GpioEvtDeviceAdd(
    _In_ WDFDRIVER Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    WDF_OBJECT_ATTRIBUTES DeviceAttributes;
    WDFDEVICE Device;
    NTSTATUS Status;

    PAGED_CODE();

    Status = GPIO_CLX_ProcessAddDevicePreDeviceCreate(Driver,
                                                      DeviceInit,
                                                      &DeviceAttributes);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    Status = WdfDeviceCreate(&DeviceInit, &DeviceAttributes, &Device);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    return GPIO_CLX_ProcessAddDevicePostDeviceCreate(Driver, Device);
}

/**
 * @brief
 * Withdraws the client registration as the driver goes away.
 *
 * @param[in] Object
 * The driver object being cleaned up.
 */
VOID
NTAPI
GpioEvtDriverUnload(
    _In_ WDFOBJECT Object)
{
    PAGED_CODE();

    GPIO_CLX_UnregisterClient((WDFDRIVER)Object);
}

/**
 * @brief
 * Registers this driver as a GpioClx client.
 *
 * @param[in] DriverObject
 * The driver object.
 *
 * @param[in] RegistryPath
 * This driver's service key, passed on to GpioClx so it can read its own
 * settings from underneath it.
 *
 * @return
 * STATUS_SUCCESS, or the failure that stopped the driver loading.
 */
NTSTATUS
NTAPI
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    GPIO_CLIENT_REGISTRATION_PACKET RegistrationPacket;
    WDF_DRIVER_CONFIG DriverConfig;
    WDF_OBJECT_ATTRIBUTES DriverAttributes;
    WDFDRIVER Driver;
    NTSTATUS Status;

    WDF_DRIVER_CONFIG_INIT(&DriverConfig, GpioEvtDeviceAdd);

    WDF_OBJECT_ATTRIBUTES_INIT(&DriverAttributes);
    DriverAttributes.EvtCleanupCallback = GpioEvtDriverUnload;

    Status = WdfDriverCreate(DriverObject,
                             RegistryPath,
                             &DriverAttributes,
                             &DriverConfig,
                             &Driver);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    /*
     * Only the callbacks this driver actually answers are filled in. GpioClx
     * reads the ones it needs and leaves the rest alone, so a NULL slot means
     * "this controller does not do that" rather than an error - which is how
     * the reference leaves QuerySetControllerInformation and
     * PreProcessControllerInterrupt.
     */
    RtlZeroMemory(&RegistrationPacket, sizeof(RegistrationPacket));
    RegistrationPacket.Version = GPIO_CLIENT_REGISTRATION_PACKET_VERSION;
    RegistrationPacket.Size = sizeof(RegistrationPacket);
    RegistrationPacket.ControllerContextSize = sizeof(GPIO_CONTROLLER);

    RegistrationPacket.CLIENT_PrepareController = GpioPrepareController;
    RegistrationPacket.CLIENT_ReleaseController = GpioReleaseController;
    RegistrationPacket.CLIENT_StartController = GpioStartController;
    RegistrationPacket.CLIENT_StopController = GpioStopController;
    RegistrationPacket.CLIENT_QueryControllerBasicInformation =
        GpioQueryControllerBasicInformation;

    RegistrationPacket.CLIENT_ConnectIoPins = GpioConnectIoPins;
    RegistrationPacket.CLIENT_DisconnectIoPins = GpioDisconnectIoPins;
    RegistrationPacket.CLIENT_ReadGpioPins = GpioReadGpioPins;
    RegistrationPacket.CLIENT_WriteGpioPins = GpioWriteGpioPins;

    RegistrationPacket.CLIENT_EnableInterrupt = GpioEnableInterrupt;
    RegistrationPacket.CLIENT_DisableInterrupt = GpioDisableInterrupt;
    RegistrationPacket.CLIENT_UnmaskInterrupt = GpioUnmaskInterrupt;
    RegistrationPacket.CLIENT_MaskInterrupts = GpioMaskInterrupts;
    RegistrationPacket.CLIENT_QueryActiveInterrupts = GpioQueryActiveInterrupts;
    RegistrationPacket.CLIENT_ClearActiveInterrupts = GpioClearActiveInterrupts;
    RegistrationPacket.CLIENT_ReconfigureInterrupt = GpioReconfigureInterrupt;

    RegistrationPacket.CLIENT_SaveBankHardwareContext = GpioSaveBankHardwareContext;
    RegistrationPacket.CLIENT_RestoreBankHardwareContext = GpioRestoreBankHardwareContext;

    return GPIO_CLX_RegisterClient(Driver, &RegistrationPacket, RegistryPath);
}
