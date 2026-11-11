/*
 * PROJECT:     ReactOS Qualcomm xHCI lower filter
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Filter setup and power callbacks
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "xhcif.h"

#define NDEBUG
#include <debug.h>

DRIVER_INITIALIZE DriverEntry;
static EVT_WDF_DRIVER_DEVICE_ADD XhcifEvtDeviceAdd;
static EVT_WDF_DEVICE_PREPARE_HARDWARE XhcifEvtPrepareHardware;
static EVT_WDF_DEVICE_RELEASE_HARDWARE XhcifEvtReleaseHardware;
static EVT_WDF_DEVICE_D0_ENTRY XhcifEvtD0Entry;
static EVT_WDF_DEVICE_D0_EXIT XhcifEvtD0Exit;

static
PVOID
XhcifMap(
    _In_ ULONG Address,
    _In_ SIZE_T Size)
{
    PHYSICAL_ADDRESS Physical;

    Physical.QuadPart = Address;
    return MmMapIoSpace(Physical, Size, MmNonCached);
}

static
VOID
XhcifUnmap(
    _Inout_ PUCHAR *Base,
    _In_ SIZE_T Size)
{
    if (!*Base)
        return;

    MmUnmapIoSpace(*Base, Size);
    *Base = NULL;
}

static
BOOLEAN
XhcifIsMsm8992(VOID)
{
    PULONG JtagId = XhcifMap(XHCIF_JTAG_ID_ADDRESS, sizeof(ULONG));
    ULONG Id;

    if (!JtagId)
        return FALSE;

    Id = READ_REGISTER_ULONG(JtagId);
    MmUnmapIoSpace(JtagId, sizeof(ULONG));

    return XHCIF_JTAG_PART(Id) == XHCIF_JTAG_PART_MSM8992;
}

static
NTSTATUS
XhcifEvtPrepareHardware(
    _In_ WDFDEVICE WdfDevice,
    _In_ WDFCMRESLIST ResourcesRaw,
    _In_ WDFCMRESLIST ResourcesTranslated)
{
    PXHCIF_DEVICE Device = XhcifGetDevice(WdfDevice);
    PCM_PARTIAL_RESOURCE_DESCRIPTOR Descriptor;
    PHYSICAL_ADDRESS Start;
    ULONG Count, i;

    UNREFERENCED_PARAMETER(ResourcesRaw);

    Count = WdfCmResourceListGetCount(ResourcesTranslated);
    for (i = 0; i < Count; i++)
    {
        Descriptor = WdfCmResourceListGetDescriptor(ResourcesTranslated, i);
        if (!Descriptor || Descriptor->Type != CmResourceTypeMemory)
            continue;

        /* The window has to be a controller this filter knows how to drive */
        Start = Descriptor->u.Memory.Start;
        Start.LowPart &= ~(PAGE_SIZE - 1);
        if (Start.HighPart == 0 && Start.LowPart == XHCIF_CONTROLLER_BASE)
        {
            Device->Soc = XhcifSocMsm8994;
        }
        else if (Start.HighPart == 0 && Start.LowPart == XHCIF_SDM845_CONTROLLER_BASE)
        {
            Device->Soc = XhcifSocSdm845;
        }
        else
        {
            DPRINT1("Unexpected controller window %I64x\n", Descriptor->u.Memory.Start.QuadPart);
            return STATUS_INTERNAL_ERROR;
        }

        if (Descriptor->u.Memory.Length < XHCIF_CONTROLLER_MIN_SIZE)
        {
            DPRINT1("Controller window too small: 0x%lx\n", Descriptor->u.Memory.Length);
            return STATUS_INTERNAL_ERROR;
        }

        XhcifUnmap(&Device->Controller, Device->ControllerSize);
        Device->Controller = MmMapIoSpace(Start, Descriptor->u.Memory.Length, MmNonCached);
        Device->ControllerSize = Descriptor->u.Memory.Length;
        if (!Device->Controller)
            return STATUS_INTERNAL_ERROR;
    }

    if (!Device->Controller)
    {
        DPRINT1("No controller window in the resources\n");
        return STATUS_INTERNAL_ERROR;
    }

    if (Device->Soc == XhcifSocSdm845)
    {
        Device->Gcc = XhcifMap(XHCIF_SDM845_GCC_BASE, XHCIF_SDM845_GCC_SIZE);
        if (!Device->Gcc)
            return STATUS_INTERNAL_ERROR;

        Device->Qusb2 = XhcifMap(XHCIF_SDM845_QUSB2_BASE, XHCIF_SDM845_QUSB2_SIZE);
        if (!Device->Qusb2)
            return STATUS_INTERNAL_ERROR;

        DPRINT("Bringing up the SDM845 controller\n");
        XhcifInitializeHardware(Device);
        return STATUS_SUCCESS;
    }

    Device->Gcc = XhcifMap(XHCIF_GCC_BASE, XHCIF_GCC_SIZE);
    if (!Device->Gcc)
        return STATUS_INTERNAL_ERROR;

    Device->TcsrUsb = XhcifMap(XHCIF_TCSR_USB_BASE, XHCIF_TCSR_USB_SIZE);
    if (!Device->TcsrUsb)
        return STATUS_INTERNAL_ERROR;

    Device->SsPhy = XhcifMap(XHCIF_SS_PHY_BASE, XHCIF_SS_PHY_SIZE);
    if (!Device->SsPhy)
        return STATUS_INTERNAL_ERROR;

    Device->Msm8992 = XhcifIsMsm8992();
    DPRINT("Bringing up the controller, MSM8992 %u\n", Device->Msm8992);

    XhcifInitializeHardware(Device);
    return STATUS_SUCCESS;
}

static
NTSTATUS
XhcifEvtReleaseHardware(
    _In_ WDFDEVICE WdfDevice,
    _In_ WDFCMRESLIST ResourcesTranslated)
{
    PXHCIF_DEVICE Device = XhcifGetDevice(WdfDevice);

    UNREFERENCED_PARAMETER(ResourcesTranslated);

    XhcifUnmap(&Device->Controller, Device->ControllerSize);
    XhcifUnmap(&Device->Gcc, (Device->Soc == XhcifSocSdm845) ? XHCIF_SDM845_GCC_SIZE : XHCIF_GCC_SIZE);
    XhcifUnmap(&Device->Qusb2, XHCIF_SDM845_QUSB2_SIZE);
    XhcifUnmap(&Device->SsPhy, XHCIF_SS_PHY_SIZE);
    XhcifUnmap(&Device->TcsrUsb, XHCIF_TCSR_USB_SIZE);
    return STATUS_SUCCESS;
}

static
NTSTATUS
XhcifEvtD0Entry(
    _In_ WDFDEVICE WdfDevice,
    _In_ WDF_POWER_DEVICE_STATE PreviousState)
{
    UNREFERENCED_PARAMETER(PreviousState);

    XhcifResumePhys(XhcifGetDevice(WdfDevice));
    return STATUS_SUCCESS;
}

static
NTSTATUS
XhcifEvtD0Exit(
    _In_ WDFDEVICE WdfDevice,
    _In_ WDF_POWER_DEVICE_STATE TargetState)
{
    UNREFERENCED_PARAMETER(TargetState);

    XhcifSuspendPhys(XhcifGetDevice(WdfDevice));
    return STATUS_SUCCESS;
}

static
NTSTATUS
XhcifEvtDeviceAdd(
    _In_ WDFDRIVER Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS PnpPower;
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDFDEVICE WdfDevice;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(Driver);

    WdfFdoInitSetFilter(DeviceInit);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&PnpPower);
    PnpPower.EvtDevicePrepareHardware = XhcifEvtPrepareHardware;
    PnpPower.EvtDeviceReleaseHardware = XhcifEvtReleaseHardware;
    PnpPower.EvtDeviceD0Entry = XhcifEvtD0Entry;
    PnpPower.EvtDeviceD0Exit = XhcifEvtD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &PnpPower);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, XHCIF_DEVICE);
    Status = WdfDeviceCreate(&DeviceInit, &Attributes, &WdfDevice);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("WdfDeviceCreate failed: 0x%lx\n", Status);
        return Status;
    }

    XhcifGetDevice(WdfDevice)->WdfDevice = WdfDevice;
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG Config;

    WDF_DRIVER_CONFIG_INIT(&Config, XhcifEvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &Config, WDF_NO_HANDLE);
}
