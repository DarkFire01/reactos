/*
 * PROJECT:     ReactOS Qualcomm xHCI lower filter
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Filter setup and power callbacks
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "xhcif.h"

#define QCDBG_COMPONENT "qcomxhcif"
#include <qcom/qcomdbg.h>

DRIVER_INITIALIZE DriverEntry;
static EVT_WDF_DRIVER_DEVICE_ADD XhcifEvtDeviceAdd;
static EVT_WDF_DEVICE_PREPARE_HARDWARE XhcifEvtPrepareHardware;
static EVT_WDF_DEVICE_RELEASE_HARDWARE XhcifEvtReleaseHardware;
static EVT_WDF_DEVICE_D0_ENTRY XhcifEvtD0Entry;
static EVT_WDF_DEVICE_D0_EXIT XhcifEvtD0Exit;

/* Controllers this filter brings up itself, any other one is left as firmware set it up */
static const XHCIF_CONTROLLER_ID XhcifControllers[] =
{
    { L"QCOM24B6", XhcifSocMsm8994, XHCIF_CONTROLLER_BASE },
    { L"QCOM24B7", XhcifSocMsm8994, XHCIF_CONTROLLER_BASE },
    { L"QCOM2430", XhcifSocMsm8994, XHCIF_CONTROLLER_BASE },
    { L"QCOM0304", XhcifSocSdm845, XHCIF_SDM845_CONTROLLER_BASE },
    { L"QCOM0305", XhcifSocSdm845, XHCIF_SDM845_CONTROLLER_BASE },
    { L"QCOM0C8B", XhcifSocX1, 0 },
    { L"QCOM0C8C", XhcifSocX1, 0 },
    { L"QCOM0D07", XhcifSocX1, 0 },
    { L"QCOM0CA1", XhcifSocX1, 0 },
    { L"QCOM0D08", XhcifSocX1, 0 },
    { L"QCOM0D09", XhcifSocX1, 0 },
};

static const XHCIF_X1_WINDOW XhcifX1Windows[] =
{
    { XHCIF_X1_PRIMARY_BASE, XhcifX1Primary },
    { XHCIF_X1_SECONDARY_BASE, XhcifX1Secondary },
    { XHCIF_X1_TERTIARY_BASE, XhcifX1Tertiary },
    { XHCIF_X1_MULTIPORT_BASE, XhcifX1Multiport },
    { XHCIF_X1_HIGH_SPEED_BASE, XhcifX1HighSpeed },
};

static
BOOLEAN
XhcifX1FindController(
    _In_ PHYSICAL_ADDRESS Start,
    _Out_ XHCIF_X1_CONTROLLER *Controller)
{
    ULONG i;

    if (Start.HighPart)
        return FALSE;

    for (i = 0; i < RTL_NUMBER_OF(XhcifX1Windows); i++)
    {
        if (Start.LowPart == XhcifX1Windows[i].Base)
        {
            *Controller = XhcifX1Windows[i].Controller;
            return TRUE;
        }
    }

    return FALSE;
}

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
    PCM_PARTIAL_RESOURCE_DESCRIPTOR Descriptor = NULL;
    PHYSICAL_ADDRESS Start;
    BOOLEAN Supported;
    ULONG Count, Length, i;

    UNREFERENCED_PARAMETER(ResourcesRaw);

    if (Device->Soc == XhcifSocFirmware)
    {
        QC_LOG("Controller left as firmware set it up");
        return STATUS_SUCCESS;
    }

    Count = WdfCmResourceListGetCount(ResourcesTranslated);
    for (i = 0; i < Count; i++)
    {
        Descriptor = WdfCmResourceListGetDescriptor(ResourcesTranslated, i);
        if (Descriptor && Descriptor->Type == CmResourceTypeMemory)
            break;
    }

    if (i == Count)
    {
        QC_LOG("No controller window in the resources");
        return STATUS_INTERNAL_ERROR;
    }

    Start = Descriptor->u.Memory.Start;
    Start.LowPart &= ~(PAGE_SIZE - 1);
    Length = Descriptor->u.Memory.Length;

    /* A known ID at another address is a board this filter was not written for */
    if (Device->Soc == XhcifSocX1)
    {
        Supported = XhcifX1FindController(Start, &Device->X1Controller);

        /* The high speed only controller lists just its xHCI registers */
        Length = max(Length, XHCIF_CONTROLLER_MIN_SIZE);
    }
    else
    {
        Supported = Start.HighPart == 0 &&
                    Start.LowPart == Device->ControllerBase &&
                    Length >= XHCIF_CONTROLLER_MIN_SIZE;
    }

    if (!Supported)
    {
        QC_LOG("Controller window %I64x length 0x%lx not expected, left as firmware set it up",
               Descriptor->u.Memory.Start.QuadPart,
               Descriptor->u.Memory.Length);
        Device->Soc = XhcifSocFirmware;
        return STATUS_SUCCESS;
    }

    XhcifUnmap(&Device->Controller, Device->ControllerSize);
    Device->Controller = MmMapIoSpace(Start, Length, MmNonCached);
    Device->ControllerSize = Length;
    if (!Device->Controller)
        return STATUS_INTERNAL_ERROR;

    if (Device->Soc == XhcifSocX1)
    {
        QC_LOG("Setting up the Snapdragon X controller at %I64x, kind %u",
               Start.QuadPart,
               Device->X1Controller);
        XhcifInitializeHardware(Device);
        return STATUS_SUCCESS;
    }

    if (Device->Soc == XhcifSocSdm845)
    {
        Device->Gcc = XhcifMap(XHCIF_SDM845_GCC_BASE, XHCIF_SDM845_GCC_SIZE);
        if (!Device->Gcc)
            return STATUS_INTERNAL_ERROR;

        Device->Qusb2 = XhcifMap(XHCIF_SDM845_QUSB2_BASE, XHCIF_SDM845_QUSB2_SIZE);
        if (!Device->Qusb2)
            return STATUS_INTERNAL_ERROR;

        QC_LOG("Bringing up the SDM845 controller");
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
    QC_LOG("Bringing up the controller, MSM8992 %u", Device->Msm8992);

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

/* Finds the controller in the table by any of its hardware IDs */
static
const XHCIF_CONTROLLER_ID *
XhcifIdentify(
    _In_ PWDFDEVICE_INIT DeviceInit)
{
    const XHCIF_CONTROLLER_ID *Found = NULL;
    WDFMEMORY Memory;
    PCWSTR Ids, Id, End;
    size_t Size;
    ULONG i;

    if (!NT_SUCCESS(WdfFdoInitAllocAndQueryProperty(DeviceInit,
                                                    DevicePropertyHardwareID,
                                                    PagedPool,
                                                    WDF_NO_OBJECT_ATTRIBUTES,
                                                    &Memory)))
    {
        return NULL;
    }

    Ids = WdfMemoryGetBuffer(Memory, &Size);
    End = Ids + Size / sizeof(WCHAR);
    for (Id = Ids; !Found && Id < End && *Id; Id += wcslen(Id) + 1)
    {
        for (i = 0; i < RTL_NUMBER_OF(XhcifControllers); i++)
        {
            if (wcsstr(Id, XhcifControllers[i].Id))
            {
                Found = &XhcifControllers[i];
                break;
            }
        }
    }

    WdfObjectDelete(Memory);
    return Found;
}

static
NTSTATUS
XhcifEvtDeviceAdd(
    _In_ WDFDRIVER Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    const XHCIF_CONTROLLER_ID *Controller;
    WDF_PNPPOWER_EVENT_CALLBACKS PnpPower;
    WDF_OBJECT_ATTRIBUTES Attributes;
    PXHCIF_DEVICE Device;
    WDFDEVICE WdfDevice;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(Driver);

    Controller = XhcifIdentify(DeviceInit);
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
        QC_LOG("WdfDeviceCreate failed: 0x%lx", Status);
        return Status;
    }

    Device = XhcifGetDevice(WdfDevice);
    Device->WdfDevice = WdfDevice;
    if (Controller)
    {
        Device->Soc = Controller->Soc;
        Device->ControllerBase = Controller->Base;
    }

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
