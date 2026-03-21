/*
 * PROJECT:     ReactOS XInput HID Filter Driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     XInput-compatible HID filter driver for game controllers
 * COPYRIGHT:   Based on Microsoft xinputhid.sys reference
 */

#define INITGUID
#include "xinputhid.h"

#define NDEBUG
#include <debug.h>

/* Forward declarations - must match WDF callback signatures */
static NTSTATUS EvtDriverDeviceAdd(
    _In_ WDFDRIVER Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit);

static NTSTATUS EvtDevicePrepareHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesRaw,
    _In_ WDFCMRESLIST ResourcesTranslated);

static NTSTATUS EvtDeviceReleaseHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesTranslated);

static VOID EvtIoRead(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t Length);

static VOID EvtIoDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode);

/* D-pad mapping: HID hat value index -> XInput button bits (up=1, down=2, left=4, right=8) */
static const UCHAR DpadMapping[12] = { 0, 1, 9, 8, 10, 2, 6, 4, 5, 0, 0, 0 };

/* Default capabilities - neutral stick positions */
static const XINPUT_CAPABILITIES_POSITION DefaultCapabilities = {
    .Buttons = 0xFFFF,
    .ThumbLX = 0x7FFF,
    .ThumbLY = 0x7FFF,
    .ThumbRX = 0x7FFF,
    .ThumbRY = 0x7FFF,
    .LeftTrigger = 0xFF,
    .RightTrigger = 0xFF,
    .ControllerCapabilities = 0,
    .Reserved = 0xFFFF,
};

/* Dispatch XInput-specific IOCTLs */
static VOID DispatchGetInformation(
    _In_ PDEVICE_CONTEXT Context,
    _In_ WDFREQUEST Request);

static VOID DispatchGetGamepadState(
    _In_ PDEVICE_CONTEXT Context,
    _In_ WDFREQUEST Request);

static VOID DispatchSetGamepadState(
    _In_ PDEVICE_CONTEXT Context,
    _In_ WDFREQUEST Request);

static VOID DispatchGetCapabilities(
    _In_ PDEVICE_CONTEXT Context,
    _In_ WDFREQUEST Request);

static VOID DispatchGetLedState(
    _In_ PDEVICE_CONTEXT Context,
    _In_ WDFREQUEST Request);

static VOID DispatchGetBattery(
    _In_ PDEVICE_CONTEXT Context,
    _In_ WDFREQUEST Request);

static VOID DispatchWaitForInput(
    _In_ PDEVICE_CONTEXT Context,
    _In_ WDFREQUEST Request);

static VOID DispatchGetInformationEx(
    _In_ PDEVICE_CONTEXT Context,
    _In_ WDFREQUEST Request);

static BOOLEAN IsXInputIoctl(ULONG IoControlCode);

static BOOLEAN ParseReport(
    _In_ PDEVICE_CONTEXT Context,
    _In_reads_(ReportLength) PCHAR Report,
    _In_ ULONG ReportLength);

static VOID ReadCompletionRoutine(
    _In_ WDFREQUEST Request,
    _In_ WDFIOTARGET IoTarget,
    _In_ PWDF_REQUEST_COMPLETION_PARAMS Params,
    _In_ WDFCONTEXT Context);

NTSTATUS
NTAPI
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    NTSTATUS Status;
    WDF_DRIVER_CONFIG Config;
    WDF_OBJECT_ATTRIBUTES Attributes;

    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    WDF_DRIVER_CONFIG_INIT(&Config, EvtDriverDeviceAdd);

    Status = WdfDriverCreate(DriverObject,
                            RegistryPath,
                            &Attributes,
                            &Config,
                            WDF_NO_HANDLE);

    if (!NT_SUCCESS(Status))
    {
        DPRINT1("xinputhid: WdfDriverCreate failed 0x%08lx\n", Status);
    }

    return Status;
}

static NTSTATUS
EvtDriverDeviceAdd(
    _In_ WDFDRIVER Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    NTSTATUS Status;
    WDFDEVICE Device;
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDF_IO_QUEUE_CONFIG QueueConfig;
    PDEVICE_CONTEXT Context;
    PDEVICE_OBJECT PhysicalDevice;
    HANDLE RegKey = NULL;
    ULONG DevicePropertyFlags = 0;
    ULONG ResultLength;
    UNICODE_STRING ValueName;
    struct
    {
        KEY_VALUE_PARTIAL_INFORMATION Info;
        UCHAR Data[sizeof(ULONG)];
    } KeyValue;

    UNREFERENCED_PARAMETER(Driver);

    /* This is a filter driver - attach to HID devices */
    WdfFdoInitSetFilter(DeviceInit);

    /* Register PnP/Power callbacks for hardware init */
    {
        WDF_PNPPOWER_EVENT_CALLBACKS PnpPowerCallbacks;
        WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&PnpPowerCallbacks);
        PnpPowerCallbacks.EvtDevicePrepareHardware = EvtDevicePrepareHardware;
        PnpPowerCallbacks.EvtDeviceReleaseHardware = EvtDeviceReleaseHardware;
        WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &PnpPowerCallbacks);
    }

    /* Get device property flags from registry - INF sets DevicePropertyFlags */
    PhysicalDevice = WdfFdoInitWdmGetPhysicalDevice(DeviceInit);
    Status = IoOpenDeviceRegistryKey(PhysicalDevice,
                                      PLUGPLAY_REGKEY_DEVICE,
                                      KEY_READ,
                                      &RegKey);
    if (NT_SUCCESS(Status))
    {
        RtlInitUnicodeString(&ValueName, L"DevicePropertyFlags");
        ResultLength = 0;
        Status = ZwQueryValueKey(RegKey,
                                 &ValueName,
                                 KeyValuePartialInformation,
                                 &KeyValue.Info,
                                 sizeof(KeyValue),
                                 &ResultLength);
        ZwClose(RegKey);
        if (NT_SUCCESS(Status) && KeyValue.Info.DataLength >= sizeof(ULONG))
        {
            DevicePropertyFlags = *(PULONG)KeyValue.Info.Data & 0xF;
        }
    }

    /* Only attach to XInput-compatible or generic HID devices */
    if (DevicePropertyFlags == 0)
    {
        DPRINT("xinputhid: No DevicePropertyFlags, skipping device\n");
        return STATUS_NOT_SUPPORTED;
    }

    /* Create device with context */
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, DEVICE_CONTEXT);
    Status = WdfDeviceCreate(&DeviceInit, &Attributes, &Device);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("xinputhid: WdfDeviceCreate failed 0x%08lx\n", Status);
        return Status;
    }

    Context = DeviceGetContext(Device);
    RtlZeroMemory(Context, sizeof(DEVICE_CONTEXT));
    Context->Device = Device;
    Context->Flags = (XINPUTHID_DEVICEPROPERTY_FLAGS)DevicePropertyFlags;

    /* Create spinlock */
    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ParentObject = Device;
    Status = WdfSpinLockCreate(&Attributes, &Context->DeviceSpinLock);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("xinputhid: WdfSpinLockCreate failed 0x%08lx\n", Status);
        WdfDeviceSetFailed(Device, Status);
        return Status;
    }

    /* Create device interface for XInput */
    if (Context->Flags & (XInputCompatibleDevice | XInputGenericHidDevice))
    {
        Status = WdfDeviceCreateDeviceInterface(Device,
                                                &GUID_XUSB_INTERFACE_CLASS,
                                                NULL);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("xinputhid: WdfDeviceCreateDeviceInterface failed 0x%08lx\n", Status);
        }
    }

    /* Create default queue for IOCTL and read handling */
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&QueueConfig, WdfIoQueueDispatchSequential);
    QueueConfig.EvtIoDeviceControl = EvtIoDeviceControl;
    QueueConfig.EvtIoRead = EvtIoRead;
    QueueConfig.PowerManaged = WdfFalse;

    Status = WdfIoQueueCreate(Device, &QueueConfig, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("xinputhid: WdfIoQueueCreate failed 0x%08lx\n", Status);
        WdfDeviceSetFailed(Device, Status);
        return Status;
    }

    /* Create manual queue for WaitForInput - requests stay pending until input arrives */
    WDF_IO_QUEUE_CONFIG_INIT(&QueueConfig, WdfIoQueueDispatchManual);
    QueueConfig.PowerManaged = WdfFalse;
    Status = WdfIoQueueCreate(Device, &QueueConfig, WDF_NO_OBJECT_ATTRIBUTES, &Context->WaitingXInputQueue);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("xinputhid: WaitingXInputQueue create failed 0x%08lx\n", Status);
    }

    /* VendorId/ProductId/BcdDevice set in EvtDevicePrepareHardware from HID_COLLECTION_INFORMATION */

    return STATUS_SUCCESS;
}

static NTSTATUS
EvtDevicePrepareHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesRaw,
    _In_ WDFCMRESLIST ResourcesTranslated)
{
    PDEVICE_CONTEXT Context = DeviceGetContext(Device);
    WDFIOTARGET IoTarget;
    NTSTATUS Status;
    WDFREQUEST Request = NULL;
    WDFMEMORY OutputMemory = NULL;
    PHID_COLLECTION_INFORMATION CollectionInfo;
    ULONG DescriptorSize;
    PHIDP_PREPARSED_DATA PreparsedData;
    HIDP_VALUE_CAPS ValueCaps;
    USHORT ValueCapsLength = 1;

    UNREFERENCED_PARAMETER(ResourcesRaw);
    UNREFERENCED_PARAMETER(ResourcesTranslated);

    IoTarget = WdfDeviceGetIoTarget(Device);

    /* 1. Get HID_COLLECTION_INFORMATION (VendorId, ProductId, DescriptorSize) */
    Status = WdfMemoryCreate(WDF_NO_OBJECT_ATTRIBUTES,
                            NonPagedPool,
                            'xihD',
                            sizeof(HID_COLLECTION_INFORMATION),
                            &OutputMemory,
                            (PVOID*)&CollectionInfo);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    RtlZeroMemory(CollectionInfo, sizeof(HID_COLLECTION_INFORMATION));
    {
        WDF_MEMORY_DESCRIPTOR OutputDesc;
        WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&OutputDesc, CollectionInfo, sizeof(HID_COLLECTION_INFORMATION));
        Status = WdfIoTargetSendIoctlSynchronously(IoTarget,
                                                   NULL,
                                                   IOCTL_HID_GET_COLLECTION_INFORMATION,
                                                   NULL,
                                                   &OutputDesc,
                                                   NULL,
                                                   NULL);
    }
    if (!NT_SUCCESS(Status))
    {
        DPRINT("xinputhid: IOCTL_HID_GET_COLLECTION_INFORMATION failed 0x%08lx\n", Status);
        goto Cleanup;
    }

    Context->VendorId = CollectionInfo->VendorID;
    Context->ProductId = CollectionInfo->ProductID;
    Context->BcdDevice = CollectionInfo->VersionNumber;
    DescriptorSize = CollectionInfo->DescriptorSize;

    WdfObjectDelete(OutputMemory);
    OutputMemory = NULL;

    if (DescriptorSize == 0)
    {
        DPRINT("xinputhid: DescriptorSize is 0\n");
        return STATUS_SUCCESS;  /* Continue without preparsed data */
    }

    /* 2. Allocate and get preparsed data via IOCTL_HID_GET_COLLECTION_DESCRIPTOR */
    Status = WdfMemoryCreate(WDF_NO_OBJECT_ATTRIBUTES,
                             NonPagedPool,
                             'xihD',
                             DescriptorSize,
                             &Context->PreparsedData,
                             (PVOID*)&PreparsedData);
    if (!NT_SUCCESS(Status))
        return Status;

    RtlZeroMemory(PreparsedData, DescriptorSize);

    Status = WdfRequestCreate(WDF_NO_OBJECT_ATTRIBUTES, Device, &Request);
    if (!NT_SUCCESS(Status))
        goto CleanupPreparsed;

    Status = WdfIoTargetFormatRequestForIoctl(IoTarget,
                                              Request,
                                              IOCTL_HID_GET_COLLECTION_DESCRIPTOR,
                                              NULL,
                                              NULL,
                                              Context->PreparsedData,
                                              NULL);
    if (!NT_SUCCESS(Status))
        goto CleanupPreparsed;

    Status = WdfRequestSend(Request, IoTarget, WDF_NO_SEND_OPTIONS);
    if (!Status)
        Status = WdfRequestGetStatus(Request);
    WdfObjectDelete(Request);
    Request = NULL;

    if (!NT_SUCCESS(Status))
    {
        DPRINT("xinputhid: IOCTL_HID_GET_COLLECTION_DESCRIPTOR failed 0x%08lx\n", Status);
        goto CleanupPreparsed;
    }

    /* 3. Get HID capabilities */
    Status = HidP_GetCaps(PreparsedData, &Context->Capabilities);
    if (!NT_SUCCESS(Status))
    {
        DPRINT("xinputhid: HidP_GetCaps failed 0x%08lx\n", Status);
        goto CleanupPreparsed;
    }

    /* 4. Check if battery is in separate report (Usage 0x20, UsagePage 6 = Battery) */
    RtlZeroMemory(&ValueCaps, sizeof(ValueCaps));
    ValueCapsLength = 1;
    if (NT_SUCCESS(HidP_GetSpecificValueCaps(HidP_Input, 6 /* HID_USAGE_PAGE_BATTERY */, 0,
                                             0x20 /* HID_USAGE_BATTERY_STRENGTH */,
                                             &ValueCaps, &ValueCapsLength, PreparsedData)))
    {
        if (ValueCaps.ReportID != 0)
            Context->BatteryInSeparateReport = TRUE;
    }

    /* 5. Allocate button usages buffer for HidP_GetUsages */
    {
        ULONG MaxUsageLength = HidP_MaxUsageListLength(HidP_Input, HID_USAGE_PAGE_BUTTON, PreparsedData);
        if (MaxUsageLength > 0)
        {
            Status = WdfMemoryCreate(WDF_NO_OBJECT_ATTRIBUTES,
                                     NonPagedPool,
                                     'xihD',
                                     MaxUsageLength * sizeof(USAGE),
                                     &Context->ButtonUsages,
                                     NULL);
            if (!NT_SUCCESS(Status))
                goto CleanupPreparsed;
        }
    }

    /* 6. Allocate read/write report buffers */
    if (Context->Capabilities.InputReportByteLength > 0)
    {
        Status = WdfMemoryCreate(WDF_NO_OBJECT_ATTRIBUTES,
                                 NonPagedPool,
                                 'xihD',
                                 Context->Capabilities.InputReportByteLength,
                                 &Context->ReadReport,
                                 NULL);
        if (!NT_SUCCESS(Status))
            goto CleanupPreparsed;

        Status = WdfMemoryCreate(WDF_NO_OBJECT_ATTRIBUTES,
                                 NonPagedPool,
                                 'xihD',
                                 Context->Capabilities.InputReportByteLength,
                                 &Context->EmptyReport,
                                 NULL);
        if (!NT_SUCCESS(Status))
            goto CleanupPreparsed;
    }

    if (Context->Capabilities.OutputReportByteLength > 0)
    {
        Status = WdfMemoryCreate(WDF_NO_OBJECT_ATTRIBUTES,
                                 NonPagedPool,
                                 'xihD',
                                 Context->Capabilities.OutputReportByteLength,
                                 &Context->WriteReport,
                                 NULL);
        if (!NT_SUCCESS(Status))
            goto CleanupPreparsed;
    }

    return STATUS_SUCCESS;

CleanupPreparsed:
    if (Context->PreparsedData)
    {
        WdfObjectDelete(Context->PreparsedData);
        Context->PreparsedData = NULL;
    }
    return Status;

Cleanup:
    if (OutputMemory)
        WdfObjectDelete(OutputMemory);
    if (Request)
        WdfObjectDelete(Request);
    return Status;
}

static NTSTATUS
EvtDeviceReleaseHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesTranslated)
{
    PDEVICE_CONTEXT Context = DeviceGetContext(Device);

    UNREFERENCED_PARAMETER(ResourcesTranslated);

    Context->Disposed = TRUE;

    if (Context->WriteReport)
    {
        WdfObjectDelete(Context->WriteReport);
        Context->WriteReport = NULL;
    }
    if (Context->EmptyReport)
    {
        WdfObjectDelete(Context->EmptyReport);
        Context->EmptyReport = NULL;
    }
    if (Context->ReadReport)
    {
        WdfObjectDelete(Context->ReadReport);
        Context->ReadReport = NULL;
    }
    if (Context->ButtonUsages)
    {
        WdfObjectDelete(Context->ButtonUsages);
        Context->ButtonUsages = NULL;
    }
    if (Context->PreparsedData)
    {
        WdfObjectDelete(Context->PreparsedData);
        Context->PreparsedData = NULL;
    }

    return STATUS_SUCCESS;
}

/* Parse HID input report into ParsedReport and update GamepadState */
static BOOLEAN
ParseReport(
    _In_ PDEVICE_CONTEXT Context,
    _In_reads_(ReportLength) PCHAR Report,
    _In_ ULONG ReportLength)
{
    PHIDP_PREPARSED_DATA PreparsedData;
    HID_REPORT ReportData;
    ULONG Tmp;
    ULONG UsageLength;
    PUSAGE UsageList;
    size_t UsageListSize;
    ULONG i;
    USHORT Buttons;

    if (!Context->PreparsedData || ReportLength == 0)
        return FALSE;

    PreparsedData = (PHIDP_PREPARSED_DATA)WdfMemoryGetBuffer(Context->PreparsedData, NULL);
    if (!PreparsedData)
        return FALSE;

    RtlZeroMemory(&ReportData, sizeof(ReportData));

    /* Hat switch (0x39) - map to Dpad bits */
    if (NT_SUCCESS(HidP_GetUsageValue(HidP_Input, HID_USAGE_PAGE_GENERIC, 0,
                                       HID_USAGE_GENERIC_HATSWITCH, &Tmp,
                                       PreparsedData, Report, ReportLength)))
    {
        ReportData.Dpad = DpadMapping[(Tmp & 0xF) % 12] & 0xF;
    }

    /* Axes: X(0x30), Y(0x31), Z(0x32), RX(0x33), RY(0x34), RZ(0x35) */
    if (!NT_SUCCESS(HidP_GetUsageValue(HidP_Input, HID_USAGE_PAGE_GENERIC, 0,
                                       HID_USAGE_GENERIC_X, &Tmp,
                                       PreparsedData, Report, ReportLength)))
        ReportData.LeftThumbX = 0x8000;
    else
        ReportData.LeftThumbX = (USHORT)Tmp;

    if (!NT_SUCCESS(HidP_GetUsageValue(HidP_Input, HID_USAGE_PAGE_GENERIC, 0,
                                       HID_USAGE_GENERIC_Y, &Tmp,
                                       PreparsedData, Report, ReportLength)))
        ReportData.LeftThumbY = 0x8000;
    else
        ReportData.LeftThumbY = (USHORT)Tmp;

    if (!NT_SUCCESS(HidP_GetUsageValue(HidP_Input, HID_USAGE_PAGE_GENERIC, 0,
                                       HID_USAGE_GENERIC_RX, &Tmp,
                                       PreparsedData, Report, ReportLength)))
        ReportData.RightThumbX = 0x8000;
    else
        ReportData.RightThumbX = (USHORT)Tmp;

    if (!NT_SUCCESS(HidP_GetUsageValue(HidP_Input, HID_USAGE_PAGE_GENERIC, 0,
                                       HID_USAGE_GENERIC_RY, &Tmp,
                                       PreparsedData, Report, ReportLength)))
        ReportData.RightThumbY = 0x8000;
    else
        ReportData.RightThumbY = (USHORT)Tmp;

    HidP_GetUsageValue(HidP_Input, HID_USAGE_PAGE_GENERIC, 0,
                       HID_USAGE_GENERIC_Z, &Tmp, PreparsedData, Report, ReportLength);
    ReportData.Triggers[0] = (USHORT)(Tmp & 0x3FF);
    HidP_GetUsageValue(HidP_Input, HID_USAGE_PAGE_GENERIC, 0,
                       HID_USAGE_GENERIC_RZ, &Tmp, PreparsedData, Report, ReportLength);
    ReportData.Triggers[1] = (USHORT)(Tmp & 0x3FF);

    /* Buttons - UsagePage 9 (Button), usages 1-10 map to A,B,X,Y,etc */
    if (Context->ButtonUsages)
    {
        UsageList = (PUSAGE)WdfMemoryGetBuffer(Context->ButtonUsages, &UsageListSize);
        UsageLength = (ULONG)(UsageListSize / sizeof(USAGE));
        if (UsageList && UsageLength > 0 &&
            NT_SUCCESS(HidP_GetUsages(HidP_Input, HID_USAGE_PAGE_BUTTON, 0,
                                      UsageList, &UsageLength,
                                      PreparsedData, Report, ReportLength)))
        {
            for (i = 0; i < UsageLength; i++)
            {
                switch (UsageList[i])
                {
                    case 1: ReportData.ButtonA = 1; break;
                    case 2: ReportData.ButtonB = 1; break;
                    case 3: ReportData.ButtonX = 1; break;
                    case 4: ReportData.ButtonY = 1; break;
                    case 5: ReportData.LeftShoulder = 1; break;
                    case 6: ReportData.RightShoulder = 1; break;
                    case 7: ReportData.BackButton = 1; break;
                    case 8: ReportData.StartButton = 1; break;
                    case 9: ReportData.LeftThumbB = 1; break;
                    case 10: ReportData.RightThumbB = 1; break;
                    default: break;
                }
            }
        }
    }

    /* Guide button - UsagePage 1 (Generic), Usage 0x85 (App Menu) */
    if (Context->ButtonUsages)
    {
        UsageList = (PUSAGE)WdfMemoryGetBuffer(Context->ButtonUsages, &UsageListSize);
        UsageLength = (ULONG)(UsageListSize / sizeof(USAGE));
        if (UsageList && UsageLength > 0 &&
            NT_SUCCESS(HidP_GetUsages(HidP_Input, HID_USAGE_PAGE_GENERIC, 0,
                                      UsageList, &UsageLength,
                                      PreparsedData, Report, ReportLength)))
        {
            for (i = 0; i < UsageLength; i++)
            {
                if (UsageList[i] == 0x85)  /* Guide button */
                {
                    ReportData.GuideButton = 1;
                    break;
                }
            }
        }
    }

    /* Battery - UsagePage 6, Usage 0x20 */
    if (NT_SUCCESS(HidP_GetUsageValue(HidP_Input, 6, 0, 0x20, &Tmp,
                                      PreparsedData, Report, ReportLength)))
        ReportData.BatteryState = (UCHAR)Tmp;

    /* Convert to XInput button format and update state */
    Buttons = (ReportData.ButtonA ? 0x1000 : 0) | (ReportData.ButtonB ? 0x2000 : 0) |
              (ReportData.ButtonX ? 0x4000 : 0) | (ReportData.ButtonY ? 0x8000 : 0) |
              (ReportData.LeftShoulder ? 0x0100 : 0) | (ReportData.RightShoulder ? 0x0200 : 0) |
              (ReportData.BackButton ? 0x0020 : 0) | (ReportData.StartButton ? 0x0010 : 0) |
              (ReportData.LeftThumbB ? 0x0040 : 0) | (ReportData.RightThumbB ? 0x0080 : 0) |
              (ReportData.GuideButton ? 0x0400 : 0) | (ReportData.Dpad & 0xF);

    /* Update cached state */
    WdfSpinLockAcquire(Context->DeviceSpinLock);
    Context->ParsedReport = ReportData;
    Context->CachedIndex++;
    Context->GamepadState0100.Buttons = Buttons;
    Context->GamepadState0100.LeftTrigger = (UCHAR)(ReportData.Triggers[0] >> 2);
    Context->GamepadState0100.RightTrigger = (UCHAR)(ReportData.Triggers[1] >> 2);
    Context->GamepadState0100.ThumbLX = (SHORT)(ReportData.LeftThumbX - 0x8000);
    Context->GamepadState0100.ThumbLY = (SHORT)(0x7FFF - (SHORT)ReportData.LeftThumbY);
    Context->GamepadState0100.ThumbRX = (SHORT)(ReportData.RightThumbX - 0x8000);
    Context->GamepadState0100.ThumbRY = (SHORT)(0x7FFF - (SHORT)ReportData.RightThumbY);
    /* Copy to 0101 format (adds BatteryState, different Type) */
    Context->GamepadState0101.Buttons = Context->GamepadState0100.Buttons;
    Context->GamepadState0101.LeftTrigger = Context->GamepadState0100.LeftTrigger;
    Context->GamepadState0101.RightTrigger = Context->GamepadState0100.RightTrigger;
    Context->GamepadState0101.ThumbLX = Context->GamepadState0100.ThumbLX;
    Context->GamepadState0101.ThumbLY = Context->GamepadState0100.ThumbLY;
    Context->GamepadState0101.ThumbRX = Context->GamepadState0100.ThumbRX;
    Context->GamepadState0101.ThumbRY = Context->GamepadState0100.ThumbRY;
    Context->GamepadState0101.Type = XINPUT_STATE_TYPE_0101;
    Context->GamepadState0101.SubType = Context->GamepadState0100.SubType;
    Context->GamepadState0101.PacketNumber = Context->GamepadState0100.PacketNumber;
    Context->GamepadState0101.BatteryState = ReportData.BatteryState;
    WdfSpinLockRelease(Context->DeviceSpinLock);

    return TRUE;
}

static VOID
CompleteWaitForInputRequest(
    _In_ PDEVICE_CONTEXT Context,
    _In_ WDFREQUEST WaitRequest)
{
    NTSTATUS Status;
    PVOID InputBuffer, OutputBuffer;
    size_t InputLength, OutputLength;
    USHORT Version = 0x0100;

    Status = WdfRequestRetrieveInputBuffer(WaitRequest, 1, &InputBuffer, &InputLength);
    if (NT_SUCCESS(Status) && InputLength >= 2)
        Version = *(PUSHORT)InputBuffer;

    if (Version == 0x0101)
    {
        Status = WdfRequestRetrieveOutputBuffer(WaitRequest, sizeof(XINPUT_GAMEPAD_STATE_0101),
                                                &OutputBuffer, &OutputLength);
        if (NT_SUCCESS(Status))
        {
            RtlCopyMemory(OutputBuffer, &Context->GamepadState0101, sizeof(XINPUT_GAMEPAD_STATE_0101));
            WdfRequestCompleteWithInformation(WaitRequest, STATUS_SUCCESS, sizeof(XINPUT_GAMEPAD_STATE_0101));
            return;
        }
    }

    Status = WdfRequestRetrieveOutputBuffer(WaitRequest, sizeof(XINPUT_GAMEPAD_STATE_0100),
                                            &OutputBuffer, &OutputLength);
    if (NT_SUCCESS(Status))
    {
        RtlCopyMemory(OutputBuffer, &Context->GamepadState0100, sizeof(XINPUT_GAMEPAD_STATE_0100));
        WdfRequestCompleteWithInformation(WaitRequest, STATUS_SUCCESS, sizeof(XINPUT_GAMEPAD_STATE_0100));
    }
    else
        WdfRequestCompleteWithInformation(WaitRequest, Status, 0);
}

static VOID
ReadCompletionRoutine(
    _In_ WDFREQUEST Request,
    _In_ WDFIOTARGET IoTarget,
    _In_ PWDF_REQUEST_COMPLETION_PARAMS Params,
    _In_ WDFCONTEXT Context)
{
    PDEVICE_CONTEXT DeviceContext = (PDEVICE_CONTEXT)Context;
    PVOID OutputBuffer;
    size_t Length;
    NTSTATUS Status;
    WDFREQUEST WaitRequest;

    UNREFERENCED_PARAMETER(IoTarget);

    Status = Params->IoStatus.Status;
    if (NT_SUCCESS(Status))
    {
        if (WdfRequestRetrieveOutputBuffer(Request, 1, &OutputBuffer, &Length) &&
            Length > 0 && !DeviceContext->Disposed && DeviceContext->PreparsedData)
        {
            if (ParseReport(DeviceContext, (PCHAR)OutputBuffer, (ULONG)Length))
            {
                /* Complete any pending WaitForInput requests */
                while (DeviceContext->WaitingXInputQueue &&
                       NT_SUCCESS(WdfIoQueueRetrieveNextRequest(DeviceContext->WaitingXInputQueue, &WaitRequest)))
                {
                    CompleteWaitForInputRequest(DeviceContext, WaitRequest);
                }
            }
        }
    }
}

static VOID
EvtIoRead(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t Length)
{
    WDFDEVICE Device = WdfIoQueueGetDevice(Queue);
    WDFIOTARGET IoTarget = WdfDeviceGetIoTarget(Device);
    PDEVICE_CONTEXT Context = DeviceGetContext(Device);
    NTSTATUS Status;
    BOOLEAN Sent;

    UNREFERENCED_PARAMETER(Length);

    if (Context->Disposed)
    {
        WdfRequestCompleteWithInformation(Request, STATUS_DEVICE_NOT_READY, 0);
        return;
    }

    /* Set completion routine to parse report and update cached state */
    WdfRequestSetCompletionRoutine(Request, ReadCompletionRoutine, Context);

    /* Forward read to lower device */
    WdfRequestFormatRequestUsingCurrentType(Request);
    Sent = WdfRequestSend(Request, IoTarget, WDF_NO_SEND_OPTIONS);
    if (!Sent)
    {
        WdfRequestSetCompletionRoutine(Request, NULL, NULL);
        Status = WdfRequestGetStatus(Request);
        WdfRequestCompleteWithInformation(Request, Status, 0);
    }
}

static VOID
EvtIoDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode)
{
    WDFDEVICE Device = WdfIoQueueGetDevice(Queue);
    PDEVICE_CONTEXT Context = DeviceGetContext(Device);
    NTSTATUS Status = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    if (Context->Disposed)
    {
        WdfRequestCompleteWithInformation(Request, STATUS_DEVICE_NOT_READY, 0);
        return;
    }

    if (IsXInputIoctl(IoControlCode))
    {
        switch (IoControlCode)
        {
            case IOCTL_XINPUT_GET_INFORMATION:
                DispatchGetInformation(Context, Request);
                return;

            case IOCTL_XINPUT_GET_GAMEPAD_STATE:
                DispatchGetGamepadState(Context, Request);
                return;

            case IOCTL_XINPUT_SET_GAMEPAD_STATE:
                DispatchSetGamepadState(Context, Request);
                return;

            case IOCTL_XINPUT_GET_CAPABILITIES:
                DispatchGetCapabilities(Context, Request);
                return;

            case IOCTL_XINPUT_GET_LED_STATE:
                DispatchGetLedState(Context, Request);
                return;

            case IOCTL_XINPUT_GET_BATTERY:
                DispatchGetBattery(Context, Request);
                return;

            case IOCTL_XINPUT_WAIT_FOR_INPUT:
                DispatchWaitForInput(Context, Request);
                return;

            case IOCTL_XINPUT_GET_INFORMATION_EX:
                DispatchGetInformationEx(Context, Request);
                return;

            default:
                Status = STATUS_INVALID_DEVICE_REQUEST;
                break;
        }
    }
    else
    {
        /* Forward to lower device */
        WDFIOTARGET IoTarget = WdfDeviceGetIoTarget(Device);
        BOOLEAN Sent;

        WdfRequestFormatRequestUsingCurrentType(Request);
        Sent = WdfRequestSend(Request, IoTarget, WDF_NO_SEND_OPTIONS);
        if (!Sent)
        {
            Status = WdfRequestGetStatus(Request);
            WdfRequestCompleteWithInformation(Request, Status, 0);
        }
        return;
    }

    WdfRequestCompleteWithInformation(Request, Status, 0);
}

static BOOLEAN
IsXInputIoctl(ULONG IoControlCode)
{
    switch (IoControlCode)
    {
        case IOCTL_XINPUT_GET_INFORMATION:
        case IOCTL_XINPUT_GET_GAMEPAD_STATE:
        case IOCTL_XINPUT_SET_GAMEPAD_STATE:
        case IOCTL_XINPUT_GET_CAPABILITIES:
        case IOCTL_XINPUT_GET_LED_STATE:
        case IOCTL_XINPUT_GET_BATTERY:
        case IOCTL_XINPUT_WAIT_FOR_INPUT:
        case IOCTL_XINPUT_GET_INFORMATION_EX:
            return TRUE;
        default:
            return FALSE;
    }
}

static VOID
DispatchGetInformation(
    _In_ PDEVICE_CONTEXT Context,
    _In_ WDFREQUEST Request)
{
    NTSTATUS Status;
    PVOID Buffer;
    size_t Length;

    Status = WdfRequestRetrieveOutputBuffer(Request, 12, &Buffer, &Length);
    if (!NT_SUCCESS(Status))
    {
        WdfRequestCompleteWithInformation(Request, Status, 0);
        return;
    }

    ((PXINPUT_INFORMATION)Buffer)->Type = XINPUT_INFO_TYPE;
    ((PXINPUT_INFORMATION)Buffer)->SubType = 0;
    ((PXINPUT_INFORMATION)Buffer)->VendorId = Context->VendorId;
    ((PXINPUT_INFORMATION)Buffer)->ProductId = Context->ProductId;

    WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, 12);
}

static VOID
DispatchGetGamepadState(
    _In_ PDEVICE_CONTEXT Context,
    _In_ WDFREQUEST Request)
{
    NTSTATUS Status;
    PVOID OutputBuffer;
    size_t OutputLength;
    PVOID InputBuffer;
    size_t InputLength;
    USHORT Version = 0x0100;  /* Default to 1.0 */

    Status = WdfRequestRetrieveInputBuffer(Request, 1, &InputBuffer, &InputLength);
    if (NT_SUCCESS(Status) && InputLength >= 2)
    {
        Version = *(PUSHORT)InputBuffer;
    }

    if (Version == 0x0101)
    {
        Status = WdfRequestRetrieveOutputBuffer(Request, sizeof(XINPUT_GAMEPAD_STATE_0101),
                                                &OutputBuffer, &OutputLength);
        if (!NT_SUCCESS(Status))
        {
            WdfRequestCompleteWithInformation(Request, Status, 0);
            return;
        }

        RtlZeroMemory(OutputBuffer, sizeof(XINPUT_GAMEPAD_STATE_0101));
        ((PXINPUT_GAMEPAD_STATE_0101)OutputBuffer)->Type = XINPUT_STATE_TYPE_0101;
        ((PXINPUT_GAMEPAD_STATE_0101)OutputBuffer)->SubType = 1;
        ((PXINPUT_GAMEPAD_STATE_0101)OutputBuffer)->PacketNumber = (UCHAR)(Context->CachedIndex + 1);
        ((PXINPUT_GAMEPAD_STATE_0101)OutputBuffer)->Buttons = Context->GamepadState0101.Buttons;
        ((PXINPUT_GAMEPAD_STATE_0101)OutputBuffer)->LeftTrigger = Context->GamepadState0101.LeftTrigger;
        ((PXINPUT_GAMEPAD_STATE_0101)OutputBuffer)->RightTrigger = Context->GamepadState0101.RightTrigger;
        ((PXINPUT_GAMEPAD_STATE_0101)OutputBuffer)->ThumbLX = Context->GamepadState0101.ThumbLX;
        ((PXINPUT_GAMEPAD_STATE_0101)OutputBuffer)->ThumbLY = Context->GamepadState0101.ThumbLY;
        ((PXINPUT_GAMEPAD_STATE_0101)OutputBuffer)->ThumbRX = Context->GamepadState0101.ThumbRX;
        ((PXINPUT_GAMEPAD_STATE_0101)OutputBuffer)->ThumbRY = Context->GamepadState0101.ThumbRY;
        ((PXINPUT_GAMEPAD_STATE_0101)OutputBuffer)->BatteryState = Context->GamepadState0101.BatteryState;

        WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, sizeof(XINPUT_GAMEPAD_STATE_0101));
    }
    else
    {
        Status = WdfRequestRetrieveOutputBuffer(Request, sizeof(XINPUT_GAMEPAD_STATE_0100),
                                                &OutputBuffer, &OutputLength);
        if (!NT_SUCCESS(Status))
        {
            WdfRequestCompleteWithInformation(Request, Status, 0);
            return;
        }

        RtlZeroMemory(OutputBuffer, sizeof(XINPUT_GAMEPAD_STATE_0100));
        ((PXINPUT_GAMEPAD_STATE_0100)OutputBuffer)->Type = 1;
        ((PXINPUT_GAMEPAD_STATE_0100)OutputBuffer)->SubType = 0;
        ((PXINPUT_GAMEPAD_STATE_0100)OutputBuffer)->PacketNumber = (UCHAR)(Context->CachedIndex + 1);
        ((PXINPUT_GAMEPAD_STATE_0100)OutputBuffer)->Buttons = Context->GamepadState0100.Buttons;
        ((PXINPUT_GAMEPAD_STATE_0100)OutputBuffer)->LeftTrigger = Context->GamepadState0100.LeftTrigger;
        ((PXINPUT_GAMEPAD_STATE_0100)OutputBuffer)->RightTrigger = Context->GamepadState0100.RightTrigger;
        ((PXINPUT_GAMEPAD_STATE_0100)OutputBuffer)->ThumbLX = Context->GamepadState0100.ThumbLX;
        ((PXINPUT_GAMEPAD_STATE_0100)OutputBuffer)->ThumbLY = Context->GamepadState0100.ThumbLY;
        ((PXINPUT_GAMEPAD_STATE_0100)OutputBuffer)->ThumbRX = Context->GamepadState0100.ThumbRX;
        ((PXINPUT_GAMEPAD_STATE_0100)OutputBuffer)->ThumbRY = Context->GamepadState0100.ThumbRY;

        WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, sizeof(XINPUT_GAMEPAD_STATE_0100));
    }
}

/* Rumble intensity lookup table - maps 0-255 to controller-specific values */
static const UCHAR RumbleTable[256] = {
    0x00, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E,
    0x2F, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E,
    0x3F, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x4B, 0x4C, 0x4D, 0x4E,
    0x4F, 0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x5B, 0x5C, 0x5D, 0x5E,
    0x5F, 0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x6B, 0x6C, 0x6D, 0x6E,
    0x6F, 0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x7B, 0x7C, 0x7D, 0x7E,
    0x7F, 0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8A, 0x8B, 0x8C, 0x8D, 0x8E,
    0x8F, 0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0x9B, 0x9C, 0x9D, 0x9E,
    0x9F, 0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE,
    0xAF, 0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xBB, 0xBC, 0xBD, 0xBE,
    0xBF, 0xC0, 0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xCB, 0xCC, 0xCD, 0xCE,
    0xCF, 0xD0, 0xD1, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xDB, 0xDC, 0xDD, 0xDE,
    0xDF, 0xE0, 0xE1, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xEB, 0xEC, 0xED, 0xEE,
    0xEF, 0xF0, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD, 0xFE,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
};

static VOID
SendVibrationReport(
    _In_ PDEVICE_CONTEXT Context,
    _In_ UCHAR LeftMotorSpeed,
    _In_ UCHAR RightMotorSpeed)
{
    NTSTATUS Status;
    PHIDP_PREPARSED_DATA PreparsedData;
    PVOID ReportBuffer;
    size_t ReportSize;
    UCHAR RumbleData[4];

    if (!Context->WriteReport || !Context->PreparsedData)
        return;

    PreparsedData = (PHIDP_PREPARSED_DATA)WdfMemoryGetBuffer(Context->PreparsedData, NULL);
    ReportBuffer = WdfMemoryGetBuffer(Context->WriteReport, &ReportSize);
    if (!PreparsedData || !ReportBuffer || ReportSize == 0)
        return;

    RtlZeroMemory(ReportBuffer, (ULONG)ReportSize);

    /* Xbox vibration uses UsagePage 0x0F (Vendor), Usages 0x50, 0x7C, 0xA7, 0x97, 0x70 */
    Status = HidP_SetUsageValue(HidP_Output, 0x0F, 0, 0x50, 0xFF,
                                PreparsedData, (PCHAR)ReportBuffer, (ULONG)ReportSize);
    if (!NT_SUCCESS(Status))
        return;

    Status = HidP_SetUsageValue(HidP_Output, 0x0F, 0, 0x7C, 0,
                                PreparsedData, (PCHAR)ReportBuffer, (ULONG)ReportSize);
    if (!NT_SUCCESS(Status))
        return;

    Status = HidP_SetUsageValue(HidP_Output, 0x0F, 0, 0xA7, 0,
                                PreparsedData, (PCHAR)ReportBuffer, (ULONG)ReportSize);
    if (!NT_SUCCESS(Status))
        return;

    Status = HidP_SetUsageValue(HidP_Output, 0x0F, 0, 0x97, 3,
                                PreparsedData, (PCHAR)ReportBuffer, (ULONG)ReportSize);
    if (!NT_SUCCESS(Status))
        return;

    RumbleData[0] = RumbleTable[LeftMotorSpeed];
    RumbleData[1] = 0;
    RumbleData[2] = RumbleTable[RightMotorSpeed];
    RumbleData[3] = 0;

    Status = HidP_SetUsageValueArray(HidP_Output, 0x0F, 0, 0x70,
                                    (PCHAR)RumbleData, 4,
                                    PreparsedData, (PCHAR)ReportBuffer, (ULONG)ReportSize);
    if (!NT_SUCCESS(Status))
        return;

    /* Send via IOCTL_HID_SET_OUTPUT_REPORT (synchronous - report is small) */
    {
        WDF_MEMORY_DESCRIPTOR InputDesc;
        WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&InputDesc, ReportBuffer, (ULONG)ReportSize);
        Status = WdfIoTargetSendIoctlSynchronously(WdfDeviceGetIoTarget(Context->Device),
                                                   NULL,
                                                   IOCTL_HID_SET_OUTPUT_REPORT,
                                                   &InputDesc,
                                                   NULL,
                                                   NULL,
                                                   NULL);
    }
}

static VOID
DispatchSetGamepadState(
    _In_ PDEVICE_CONTEXT Context,
    _In_ WDFREQUEST Request)
{
    NTSTATUS Status;
    PVOID InputBuffer;
    size_t InputLength;
    PXINPUT_SET_GAMEPAD_STATE State;

    Status = WdfRequestRetrieveInputBuffer(Request, sizeof(XINPUT_SET_GAMEPAD_STATE),
                                          &InputBuffer, &InputLength);
    if (!NT_SUCCESS(Status))
    {
        WdfRequestCompleteWithInformation(Request, Status, 0);
        return;
    }

    State = (PXINPUT_SET_GAMEPAD_STATE)InputBuffer;

    if (State->Flags & 1)  /* LED */
    {
        Context->LedState = State->LedState;
    }

    if (State->Flags & 2)  /* Vibration */
    {
        SendVibrationReport(Context, State->LeftMotorSpeed, State->RightMotorSpeed);
    }

    WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, 0);
}

static VOID
DispatchGetCapabilities(
    _In_ PDEVICE_CONTEXT Context,
    _In_ WDFREQUEST Request)
{
    NTSTATUS Status;
    PVOID OutputBuffer;
    size_t OutputLength;
    PVOID InputBuffer;
    size_t InputLength;
    USHORT Version = 0x0100;

    Status = WdfRequestRetrieveInputBuffer(Request, 1, &InputBuffer, &InputLength);
    if (NT_SUCCESS(Status) && InputLength >= 3)
    {
        Version = *(PUSHORT)InputBuffer;
    }

    if (Version == 0x0101)
    {
        Status = WdfRequestRetrieveOutputBuffer(Request, 36, &OutputBuffer, &OutputLength);
        if (!NT_SUCCESS(Status))
        {
            WdfRequestCompleteWithInformation(Request, Status, 0);
            return;
        }

        RtlZeroMemory(OutputBuffer, 36);
        *(PUSHORT)OutputBuffer = XINPUT_STATE_TYPE_0101;
        *((PUSHORT)OutputBuffer + 1) = 0x0101;
        *((PUSHORT)OutputBuffer + 2) = Context->VendorId;
        *((PUSHORT)OutputBuffer + 3) = Context->ProductId;
        *((PUSHORT)OutputBuffer + 4) = Context->BcdDevice;
        RtlCopyMemory((PUCHAR)OutputBuffer + 16, &DefaultCapabilities, sizeof(DefaultCapabilities));
        *((PUSHORT)OutputBuffer + 17) = 0xFFFF;

        WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, 36);
    }
    else
    {
        Status = WdfRequestRetrieveOutputBuffer(Request, 24, &OutputBuffer, &OutputLength);
        if (!NT_SUCCESS(Status))
        {
            WdfRequestCompleteWithInformation(Request, Status, 0);
            return;
        }

        RtlZeroMemory(OutputBuffer, 24);
        *(PULONG)OutputBuffer = 0x01010103;  /* Type */
        RtlCopyMemory((PUCHAR)OutputBuffer + 4, &DefaultCapabilities, sizeof(DefaultCapabilities));
        *((PUSHORT)OutputBuffer + 11) = 0xFFFF;

        WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, 24);
    }
}

static VOID
DispatchGetLedState(
    _In_ PDEVICE_CONTEXT Context,
    _In_ WDFREQUEST Request)
{
    NTSTATUS Status;
    PVOID OutputBuffer;
    size_t Length;

    Status = WdfRequestRetrieveOutputBuffer(Request, 3, &OutputBuffer, &Length);
    if (!NT_SUCCESS(Status))
    {
        WdfRequestCompleteWithInformation(Request, Status, 0);
        return;
    }

    *(PUSHORT)OutputBuffer = XINPUT_STATE_TYPE_0101;
    *((PUCHAR)OutputBuffer + 2) = Context->LedState;

    WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, 3);
}

static VOID
DispatchGetBattery(
    _In_ PDEVICE_CONTEXT Context,
    _In_ WDFREQUEST Request)
{
    NTSTATUS Status;
    PVOID OutputBuffer;
    size_t Length;

    UNREFERENCED_PARAMETER(Context);

    Status = WdfRequestRetrieveOutputBuffer(Request, 2, &OutputBuffer, &Length);
    if (!NT_SUCCESS(Status))
    {
        WdfRequestCompleteWithInformation(Request, Status, 0);
        return;
    }

    /* Battery type + level - 0 = unknown/disconnected */
    *(PUSHORT)OutputBuffer = 0;

    WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, 2);
}

static VOID
DispatchWaitForInput(
    _In_ PDEVICE_CONTEXT Context,
    _In_ WDFREQUEST Request)
{
    NTSTATUS Status;

    if (!Context->WaitingXInputQueue)
    {
        WdfRequestCompleteWithInformation(Request, STATUS_INSUFFICIENT_RESOURCES, 0);
        return;
    }

    /* Forward to manual queue - will be completed when next report arrives */
    Status = WdfRequestForwardToIoQueue(Request, Context->WaitingXInputQueue);
    if (!NT_SUCCESS(Status))
        WdfRequestCompleteWithInformation(Request, Status, 0);
}

static VOID
DispatchGetInformationEx(
    _In_ PDEVICE_CONTEXT Context,
    _In_ WDFREQUEST Request)
{
    NTSTATUS Status;
    PVOID InputBuffer;
    size_t InputLength;
    PVOID OutputBuffer;
    size_t OutputLength;

    Status = WdfRequestRetrieveInputBuffer(Request, 36, &InputBuffer, &InputLength);
    if (!NT_SUCCESS(Status))
    {
        WdfRequestCompleteWithInformation(Request, Status, 0);
        return;
    }

    Status = WdfRequestRetrieveOutputBuffer(Request, 949, &OutputBuffer, &OutputLength);
    if (!NT_SUCCESS(Status))
    {
        WdfRequestCompleteWithInformation(Request, Status, 0);
        return;
    }

    if (*(PUSHORT)InputBuffer != 260)
    {
        WdfRequestCompleteWithInformation(Request, STATUS_INVALID_PARAMETER, 0);
        return;
    }

    RtlZeroMemory(OutputBuffer, OutputLength);
    *((PUSHORT)OutputBuffer + 1) = 32;
    *((PUCHAR)OutputBuffer + 2) = 0;
    *((PUSHORT)OutputBuffer + 11) = Context->VendorId;
    *((PUSHORT)OutputBuffer + 12) = Context->ProductId;
    *((PUSHORT)OutputBuffer + 13) = Context->BcdDevice;

    WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, 32);
}
