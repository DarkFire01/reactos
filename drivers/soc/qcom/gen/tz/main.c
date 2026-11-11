/*
 * PROJECT:     ReactOS Qualcomm TrustZone driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Device setup and service request dispatch
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "tz.h"

#define QCDBG_COMPONENT "qcomtz"
#include <qcom/qcomdbg.h>

DRIVER_INITIALIZE DriverEntry;
static EVT_WDF_DRIVER_DEVICE_ADD TzEvtDeviceAdd;
static EVT_WDF_DEVICE_PREPARE_HARDWARE TzEvtPrepareHardware;
static EVT_WDF_DEVICE_RELEASE_HARDWARE TzEvtReleaseHardware;
static EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL TzEvtIoDeviceControl;

/* Every monitor call is serialized, the bounce page and the monitor itself are shared */
static
NTSTATUS
TzDispatch(
    _In_ PTZ_DEVICE Device,
    _In_ PQCOM_TZ_SERVICE_REQUEST Service,
    _In_ KPROCESSOR_MODE RequestorMode,
    _Out_ PULONG_PTR BytesWritten)
{
    ULONG Index = Service->FunctionCode & QCOM_TZ_FUNCTION_INDEX_MASK;
    KAFFINITY OldAffinity = 0;
    PQCOM_TZ_CALL Call;
    ULONG_PTR Expected;
    NTSTATUS Status;

    *BytesWritten = 0;

    if (Index > QCOM_TZ_FUNCTION_OS_CALL)
        return STATUS_INVALID_PARAMETER;

    if (RequestorMode != KernelMode)
        return STATUS_ACCESS_DENIED;

    Expected = (Index == QCOM_TZ_FUNCTION_SIP_CALL) ? sizeof(QCOM_TZ_SIP_RESULT)
                                                    : sizeof(QCOM_TZ_OS_RESULT);
    Call = Service->InputBuffer;
    if (!Call ||
        !Service->OutputBuffer ||
        Service->InputBufferSize != sizeof(*Call) ||
        Service->OutputBufferSize != Expected)
    {
        return STATUS_INVALID_PARAMETER;
    }

    QC_LOG("request func 0x%lx smc 0x%08lx param 0x%lx",
           Service->FunctionCode, Call->SmcId, Call->ParamId);

    if (Service->FunctionCode & QCOM_TZ_FUNCTION_ON_CPU0)
        OldAffinity = KeSetSystemAffinityThreadEx(1);

    WdfWaitLockAcquire(Device->Lock, NULL);

    if (!Device->Ready)
        Status = STATUS_DEVICE_NOT_READY;
    else if (Index == QCOM_TZ_FUNCTION_SIP_CALL)
        Status = TzSipCall(Device, Call->SmcId, Call->ParamId, Call->Args, Service->OutputBuffer);
    else
        Status = TzOsCall(Device, Call->SmcId, Call->ParamId, Call->Args, Service->OutputBuffer);

    WdfWaitLockRelease(Device->Lock);

    if (Service->FunctionCode & QCOM_TZ_FUNCTION_ON_CPU0)
        KeRevertToUserAffinityThreadEx(OldAffinity);

    if (NT_SUCCESS(Status))
        *BytesWritten = Service->OutputBufferSize;

    QC_LOG("request smc 0x%08lx -> 0x%lx", Call->SmcId, Status);
    return Status;
}

static
VOID
TzEvtIoDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode)
{
    PTZ_DEVICE Device = TzGetDevice(WdfIoQueueGetDevice(Queue));
    PQCOM_TZ_SERVICE_RESPONSE Response;
    QCOM_TZ_SERVICE_REQUEST Service;
    ULONG_PTR BytesWritten;
    PVOID Input;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    if (IoControlCode != IOCTL_QCOM_TZ_SERVICE_REQUEST)
    {
        QC_LOG("unknown IOCTL 0x%08lx", IoControlCode);
        WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST);
        return;
    }

    Status = WdfRequestRetrieveInputBuffer(Request, sizeof(Service), &Input, NULL);
    if (NT_SUCCESS(Status))
    {
        Status = WdfRequestRetrieveOutputBuffer(Request,
                                                sizeof(*Response),
                                                (PVOID *)&Response,
                                                NULL);
    }
    if (!NT_SUCCESS(Status))
    {
        WdfRequestComplete(Request, Status);
        return;
    }

    /* Buffered requests share one system buffer, so take the input before writing the reply */
    RtlCopyMemory(&Service, Input, sizeof(Service));

    Status = TzDispatch(Device, &Service, WdfRequestGetRequestorMode(Request), &BytesWritten);
    if (!NT_SUCCESS(Status))
    {
        WdfRequestComplete(Request, Status);
        return;
    }

    Response->Reserved = 0;
    Response->BytesWritten = BytesWritten;
    WdfRequestCompleteWithInformation(Request, Status, sizeof(*Response));
}

static
NTSTATUS
TzEvtPrepareHardware(
    _In_ WDFDEVICE WdfDevice,
    _In_ WDFCMRESLIST ResourcesRaw,
    _In_ WDFCMRESLIST ResourcesTranslated)
{
    PTZ_DEVICE Device = TzGetDevice(WdfDevice);
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(ResourcesRaw);
    UNREFERENCED_PARAMETER(ResourcesTranslated);

    WdfWaitLockAcquire(Device->Lock, NULL);

    Status = TzInitializeMonitor(Device);
    if (NT_SUCCESS(Status))
        Device->Ready = TRUE;
    else
        QC_LOG("TrustZone monitor setup failed 0x%lx", Status);

    WdfWaitLockRelease(Device->Lock);
    return Status;
}

static
NTSTATUS
TzEvtReleaseHardware(
    _In_ WDFDEVICE WdfDevice,
    _In_ WDFCMRESLIST ResourcesTranslated)
{
    PTZ_DEVICE Device = TzGetDevice(WdfDevice);

    UNREFERENCED_PARAMETER(ResourcesTranslated);

    WdfWaitLockAcquire(Device->Lock, NULL);
    Device->Ready = FALSE;
    TzReleaseMonitor(Device);
    WdfWaitLockRelease(Device->Lock);

    return STATUS_SUCCESS;
}

static
NTSTATUS
TzEvtDeviceAdd(
    _In_ WDFDRIVER Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS PnpCallbacks;
    WDF_IO_QUEUE_CONFIG QueueConfig;
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDFDEVICE WdfDevice;
    PTZ_DEVICE Device;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(Driver);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&PnpCallbacks);
    PnpCallbacks.EvtDevicePrepareHardware = TzEvtPrepareHardware;
    PnpCallbacks.EvtDeviceReleaseHardware = TzEvtReleaseHardware;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &PnpCallbacks);
    WdfDeviceInitSetDeviceType(DeviceInit, FILE_DEVICE_TRUST_ENV);

    /* Monitor calls wait on a lock, keep every callback at passive level */
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, TZ_DEVICE);
    Attributes.ExecutionLevel = WdfExecutionLevelPassive;
    Status = WdfDeviceCreate(&DeviceInit, &Attributes, &WdfDevice);
    if (!NT_SUCCESS(Status))
        return Status;

    Device = TzGetDevice(WdfDevice);
    RtlZeroMemory(Device, sizeof(*Device));
    Device->WdfDevice = WdfDevice;

    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ParentObject = WdfDevice;
    Status = WdfWaitLockCreate(&Attributes, &Device->Lock);
    if (!NT_SUCCESS(Status))
        return Status;

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&QueueConfig, WdfIoQueueDispatchParallel);
    QueueConfig.EvtIoDeviceControl = TzEvtIoDeviceControl;
    QueueConfig.EvtIoInternalDeviceControl = TzEvtIoDeviceControl;
    QueueConfig.PowerManaged = WdfFalse;
    Status = WdfIoQueueCreate(WdfDevice, &QueueConfig, WDF_NO_OBJECT_ATTRIBUTES, NULL);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = WdfDeviceCreateDeviceInterface(WdfDevice, &GUID_DEVINTERFACE_QCOM_TZ, NULL);
    if (!NT_SUCCESS(Status))
        return Status;

    return WdfDeviceCreateDeviceInterface(WdfDevice, &GUID_DEVINTERFACE_QCOM_TZ_PASSTHROUGH, NULL);
}

NTSTATUS
NTAPI
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG Config;

    WDF_DRIVER_CONFIG_INIT(&Config, TzEvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &Config, WDF_NO_HANDLE);
}
