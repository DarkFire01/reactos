/*
 * PROJECT:     ReactOS Qualcomm IPC Router driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Device setup, PnP and registry configuration
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "ipcrouter.h"

#define QCDBG_COMPONENT "qcomipcrouter"
#include <qcom/qcomdbg.h>

DRIVER_INITIALIZE DriverEntry;
static EVT_WDF_DRIVER_DEVICE_ADD IpcEvtDeviceAdd;
static EVT_WDF_DEVICE_PREPARE_HARDWARE IpcEvtPrepareHardware;
static EVT_WDF_DEVICE_RELEASE_HARDWARE IpcEvtReleaseHardware;
static EVT_WDF_OBJECT_CONTEXT_CLEANUP IpcEvtDeviceCleanup;

NTSTATUS
IpcReadTransports(
    _In_ WDFDEVICE WdfDevice,
    _Inout_ PIPC_DEVICE Device)
{
    DECLARE_CONST_UNICODE_STRING(ProcessorIdName, L"ProcessorID");
    DECLARE_CONST_UNICODE_STRING(NumTransportsName, L"NumTransports");
    DECLARE_CONST_UNICODE_STRING(TypeName, L"Type");
    DECLARE_CONST_UNICODE_STRING(EdgeName, L"Edge");
    DECLARE_CONST_UNICODE_STRING(FlagsName, L"Flags");
    DECLARE_CONST_UNICODE_STRING(FifoSizeName, L"FifoSize");
    DECLARE_CONST_UNICODE_STRING(PortNameName, L"PortName");
    WDFKEY Params, Sub;
    UNICODE_STRING SubName;
    UNICODE_STRING PortName;
    ANSI_STRING AnsiName;
    WCHAR Digit[2];
    WCHAR NameBuffer[IPC_NAME_LENGTH];
    ULONG Count = 0, i;
    PIPC_XPORT Xport;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(WdfDevice);

    Status = WdfDriverOpenParametersRegistryKey(WdfGetDriver(), KEY_READ, WDF_NO_OBJECT_ATTRIBUTES, &Params);
    if (!NT_SUCCESS(Status))
        return Status;

    if (!NT_SUCCESS(WdfRegistryQueryULong(Params, &ProcessorIdName, &Device->LocalNode)))
        Device->LocalNode = 1;

    WdfRegistryQueryULong(Params, &NumTransportsName, &Count);
    if (Count > IPC_MAX_XPORTS)
        Count = IPC_MAX_XPORTS;

    for (i = 0; i < Count; i++)
    {
        Digit[0] = (WCHAR)(L'0' + i);
        Digit[1] = UNICODE_NULL;
        RtlInitUnicodeString(&SubName, Digit);

        Status = WdfRegistryOpenKey(Params, &SubName, KEY_READ, WDF_NO_OBJECT_ATTRIBUTES, &Sub);
        if (!NT_SUCCESS(Status))
            break;

        Xport = &Device->Xports[Device->XportCount];
        RtlZeroMemory(Xport, sizeof(*Xport));

        WdfRegistryQueryULong(Sub, &TypeName, &Xport->Type);
        WdfRegistryQueryULong(Sub, &EdgeName, &Xport->Edge);
        WdfRegistryQueryULong(Sub, &FlagsName, &Xport->Flags);
        WdfRegistryQueryULong(Sub, &FifoSizeName, &Xport->FifoSize);

        PortName.Buffer = NameBuffer;
        PortName.Length = 0;
        PortName.MaximumLength = sizeof(NameBuffer);
        Status = WdfRegistryQueryUnicodeString(Sub, &PortNameName, NULL, &PortName);
        if (NT_SUCCESS(Status))
        {
            AnsiName.Buffer = Xport->Name;
            AnsiName.Length = 0;
            AnsiName.MaximumLength = sizeof(Xport->Name) - 1;
            RtlUnicodeStringToAnsiString(&AnsiName, &PortName, FALSE);
            Xport->Name[AnsiName.Length] = '\0';
            Device->XportCount++;
        }

        WdfRegistryClose(Sub);
    }

    WdfRegistryClose(Params);
    return STATUS_SUCCESS;
}

static
NTSTATUS
IpcEvtPrepareHardware(
    _In_ WDFDEVICE WdfDevice,
    _In_ WDFCMRESLIST Resources,
    _In_ WDFCMRESLIST ResourcesTranslated)
{
    PIPC_DEVICE Device = IpcGetDevice(WdfDevice);
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(Resources);
    UNREFERENCED_PARAMETER(ResourcesTranslated);

    Status = IpcSmdlAcquire(Device);
    if (!NT_SUCCESS(Status))
        return Status;

    return IpcXportStartAll(Device);
}

static
NTSTATUS
IpcEvtReleaseHardware(
    _In_ WDFDEVICE WdfDevice,
    _In_ WDFCMRESLIST ResourcesTranslated)
{
    PIPC_DEVICE Device = IpcGetDevice(WdfDevice);

    UNREFERENCED_PARAMETER(ResourcesTranslated);

    IpcXportStopAll(Device);
    IpcSmdlRelease(Device);
    return STATUS_SUCCESS;
}

static
VOID
IpcEvtDeviceCleanup(
    _In_ WDFOBJECT Object)
{
    UNREFERENCED_PARAMETER(Object);

    IpcCoreTeardown();
    IpcGlobal = NULL;
}

static
NTSTATUS
IpcEvtDeviceAdd(
    _In_ WDFDRIVER Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS PnpCallbacks;
    WDF_OBJECT_ATTRIBUTES Attributes;
    DECLARE_CONST_UNICODE_STRING(DeviceName, L"\\Device\\IPC");
    DECLARE_CONST_UNICODE_STRING(SymbolicLink, L"\\DosDevices\\IPC");
    DECLARE_CONST_UNICODE_STRING(Sddl, L"D:P(A;;GA;;;LS)(A;;GA;;;BA)(A;;GA;;;SY)");
    PIPC_DEVICE Device;
    WDFDEVICE WdfDevice;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(Driver);

    /* The router keeps global state, so a single device serves it */
    if (IpcGlobal)
        return STATUS_DEVICE_ALREADY_ATTACHED;

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&PnpCallbacks);
    PnpCallbacks.EvtDevicePrepareHardware = IpcEvtPrepareHardware;
    PnpCallbacks.EvtDeviceReleaseHardware = IpcEvtReleaseHardware;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &PnpCallbacks);

    Status = WdfDeviceInitAssignName(DeviceInit, &DeviceName);
    if (!NT_SUCCESS(Status))
        return Status;

    WdfDeviceInitSetIoType(DeviceInit, WdfDeviceIoDirect);
    WdfDeviceInitSetExclusive(DeviceInit, FALSE);

    Status = WdfDeviceInitAssignSDDLString(DeviceInit, &Sddl);
    if (!NT_SUCCESS(Status))
        return Status;

    IpcFileObjectConfig(DeviceInit);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, IPC_DEVICE);
    Attributes.EvtCleanupCallback = IpcEvtDeviceCleanup;

    Status = WdfDeviceCreate(&DeviceInit, &Attributes, &WdfDevice);
    if (!NT_SUCCESS(Status))
        return Status;

    Device = IpcGetDevice(WdfDevice);
    Device->WdfDevice = WdfDevice;

    Status = IpcReadTransports(WdfDevice, Device);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = IpcCoreInitialize(Device->LocalNode);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = IpcCreateQueue(WdfDevice);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = WdfDeviceCreateDeviceInterface(WdfDevice, &GUID_DEVINTERFACE_QCOM_IPC, NULL);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = WdfDeviceCreateSymbolicLink(WdfDevice, &SymbolicLink);
    if (!NT_SUCCESS(Status))
        return Status;

    /* Kernel clients reach the router through this query interface */
    Status = IpcQsocketPublish(WdfDevice);
    if (!NT_SUCCESS(Status))
        return Status;

    IpcGlobal = Device;
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG Config;

    WDF_DRIVER_CONFIG_INIT(&Config, IpcEvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &Config, WDF_NO_HANDLE);
}
