/*
 * PROJECT:     ReactOS GPIO framework extension
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     The four entry points a controller driver calls
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/*
 * These are what GpioClxExports[4] points at, so a client reaches them through
 * its bound function table rather than by import.
 */

#define NDEBUG
#include "gpioclxp.h"

/**
 * @brief
 * Records a controller driver's callbacks.
 *
 * The packet is copied, so a client may build it on the stack. Only the
 * version and size are checked here: which callbacks a controller answers is
 * its own business, and a NULL slot means "this controller does not do that"
 * rather than an error.
 *
 * @param[in] DriverGlobals
 * The client's globals, from the bind.
 *
 * @param[in] Driver
 * The calling driver.
 *
 * @param[in] RegistrationPacket
 * The callbacks the client answers.
 *
 * @param[in] RegistryPath
 * The client's service key. Unused so far; the reference reads its per-client
 * settings from underneath it.
 *
 * @return
 * STATUS_SUCCESS, or STATUS_INVALID_PARAMETER for a packet that does not fit.
 */
NTSTATUS
NTAPI
GcxRegisterClient(
    _In_ PGPIO_DRIVER_GLOBALS DriverGlobals,
    _In_ WDFDRIVER Driver,
    _In_ PGPIO_CLIENT_REGISTRATION_PACKET RegistrationPacket,
    _In_opt_ PCUNICODE_STRING RegistryPath)
{
    PGCX_CLIENT_GLOBALS Globals;

    UNREFERENCED_PARAMETER(Driver);
    UNREFERENCED_PARAMETER(RegistryPath);

    Globals = GcxGlobalsFromClient(DriverGlobals);
    if (Globals == NULL || RegistrationPacket == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (RegistrationPacket->Version != GPIO_CLIENT_REGISTRATION_PACKET_VERSION ||
        RegistrationPacket->Size != sizeof(GPIO_CLIENT_REGISTRATION_PACKET))
    {
        DPRINT1("GpioClx: client packet is version %u size %u, expected %u size %u\n",
                RegistrationPacket->Version, RegistrationPacket->Size,
                GPIO_CLIENT_REGISTRATION_PACKET_VERSION,
                (ULONG)sizeof(GPIO_CLIENT_REGISTRATION_PACKET));
        return STATUS_INVALID_PARAMETER;
    }

    /*
     * Every callback is handed the client's own controller context, so a client
     * that asks for none has nowhere to keep its register base and cannot work.
     */
    if (RegistrationPacket->ControllerContextSize == 0)
    {
        DPRINT1("GpioClx: client asked for no controller context\n");
        return STATUS_INVALID_PARAMETER;
    }

    Globals->Registration = *RegistrationPacket;
    Globals->Registered = TRUE;

    DPRINT("GpioClx: registered a client, %lu bytes of controller context\n",
           RegistrationPacket->ControllerContextSize);

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Drops a controller driver's registration as it unloads.
 *
 * @param[in] DriverGlobals
 * The client's globals, from the bind.
 *
 * @param[in] Driver
 * The calling driver.
 *
 * @return
 * STATUS_SUCCESS, or STATUS_INVALID_PARAMETER for globals we did not hand out.
 */
NTSTATUS
NTAPI
GcxUnregisterClient(
    _In_ PGPIO_DRIVER_GLOBALS DriverGlobals,
    _In_ WDFDRIVER Driver)
{
    PGCX_CLIENT_GLOBALS Globals;

    UNREFERENCED_PARAMETER(Driver);

    Globals = GcxGlobalsFromClient(DriverGlobals);
    if (Globals == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(&Globals->Registration, sizeof(Globals->Registration));
    Globals->Registered = FALSE;

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Prepares a controller device before the client creates it.
 *
 * The class extension needs its own context on the device, so it hands back the
 * attributes the client must create the device with. The controller context is
 * attached here rather than after the fact because WDF fixes an object's
 * context types at creation.
 *
 * @param[in] DriverGlobals
 * The client's globals, from the bind.
 *
 * @param[in] Driver
 * The calling driver.
 *
 * @param[in,out] DeviceInit
 * The initialization block for the device being added.
 *
 * @param[out] DeviceAttributes
 * Receives the attributes to pass to WdfDeviceCreate.
 *
 * @return
 * STATUS_SUCCESS, or STATUS_INVALID_PARAMETER from an unregistered client.
 */
NTSTATUS
NTAPI
GcxProcessAddDevicePreDeviceCreate(
    _In_ PGPIO_DRIVER_GLOBALS DriverGlobals,
    _In_ WDFDRIVER Driver,
    _In_ PWDFDEVICE_INIT DeviceInit,
    _Out_ PWDF_OBJECT_ATTRIBUTES DeviceAttributes)
{
    PGCX_CLIENT_GLOBALS Globals;
    WDF_PNPPOWER_EVENT_CALLBACKS PnpPowerCallbacks;

    UNREFERENCED_PARAMETER(Driver);

    Globals = GcxGlobalsFromClient(DriverGlobals);
    if (Globals == NULL || DeviceAttributes == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (!Globals->Registered)
    {
        DPRINT1("GpioClx: AddDevice from a client that never registered\n");
        return STATUS_INVALID_DEVICE_STATE;
    }

    /*
     * The class extension owns the device's PnP and power callbacks: a client
     * never sees a WDF callback, only the four hardware calls made out of
     * controller.c. They go on the DeviceInit rather than after the fact
     * because WDF will not take them once the device exists.
     */
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&PnpPowerCallbacks);
    PnpPowerCallbacks.EvtDevicePrepareHardware = GcxEvtDevicePrepareHardware;
    PnpPowerCallbacks.EvtDeviceReleaseHardware = GcxEvtDeviceReleaseHardware;
    PnpPowerCallbacks.EvtDeviceD0Entry = GcxEvtDeviceD0Entry;
    PnpPowerCallbacks.EvtDeviceD0Exit = GcxEvtDeviceD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &PnpPowerCallbacks);

    /*
     * The client's controller context follows ours in the same allocation, so
     * the device is created oversized by however much the client asked for.
     * That is what lets GcxClientContext() find it by offset.
     */
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(DeviceAttributes, GCX_CONTROLLER);
    DeviceAttributes->ContextSizeOverride =
        sizeof(GCX_CONTROLLER) + Globals->Registration.ControllerContextSize;

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Attaches the class extension once the client has created its device.
 *
 * The storage was already sized in PreDeviceCreate, so this only has to claim
 * it: the controller block is initialized and the client's callbacks copied
 * onto it. Nothing touches hardware yet, which does not happen until PnP calls
 * PrepareHardware.
 *
 * @param[in] DriverGlobals
 * The client's globals, from the bind.
 *
 * @param[in] Driver
 * The calling driver.
 *
 * @param[in] Device
 * The device the client just created.
 *
 * @return
 * STATUS_SUCCESS, or the failure that stopped the controller coming up.
 */
NTSTATUS
NTAPI
GcxProcessAddDevicePostDeviceCreate(
    _In_ PGPIO_DRIVER_GLOBALS DriverGlobals,
    _In_ WDFDRIVER Driver,
    _In_ WDFDEVICE Device)
{
    PGCX_CLIENT_GLOBALS Globals;
    PGCX_CONTROLLER Controller;

    UNREFERENCED_PARAMETER(Driver);

    Globals = GcxGlobalsFromClient(DriverGlobals);
    if (Globals == NULL || Device == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    Controller = GcxGetController(Device);
    if (Controller == NULL)
    {
        DPRINT1("GpioClx: the device was not created with our attributes\n");
        return STATUS_INVALID_DEVICE_STATE;
    }

    /*
     * The whole context, ours and the client's, came from one oversized
     * allocation sized in PreDeviceCreate, so zeroing it here covers both.
     */
    RtlZeroMemory(Controller,
                  sizeof(*Controller) + Globals->Registration.ControllerContextSize);

    Controller->Device = Device;
    Controller->Client = Globals;

    /*
     * The callbacks are copied onto the controller. A controller outlives
     * nothing here, but reaching through the globals on every hardware call
     * would mean a client that unregistered while a device was still up could
     * be called through a table it had already dropped.
     */
    Controller->Registration = Globals->Registration;
    Controller->State = GcxControllerStopped;

    return GcxCreateControllerDevice(Controller);
}
