/*
 * PROJECT:     ReactOS Qualcomm remote file system server
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Device setup, shared memory, storage and QMI service bring up
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "remotefs.h"
#include <ntstrsafe.h>

#define QCDBG_COMPONENT "qcomremotefs"
#include <qcom/qcomdbg.h>

DRIVER_INITIALIZE DriverEntry;
static EVT_WDF_DRIVER_DEVICE_ADD RfsEvtDeviceAdd;
static EVT_WDF_DEVICE_PREPARE_HARDWARE RfsEvtPrepareHardware;
static EVT_WDF_DEVICE_RELEASE_HARDWARE RfsEvtReleaseHardware;
static EVT_WDF_DEVICE_SELF_MANAGED_IO_INIT RfsEvtSelfManagedIoInit;
static EVT_WDF_DEVICE_SELF_MANAGED_IO_CLEANUP RfsEvtSelfManagedIoCleanup;

/* The three memory windows come in _CRS order, mapped for the iovec path */
static
NTSTATUS
RfsEvtPrepareHardware(
    _In_ WDFDEVICE WdfDevice,
    _In_ WDFCMRESLIST ResourcesRaw,
    _In_ WDFCMRESLIST ResourcesTranslated)
{
    PRFS_DEVICE Device = RfsGetDevice(WdfDevice);
    PCM_PARTIAL_RESOURCE_DESCRIPTOR Descriptor;
    ULONG i, Count;

    UNREFERENCED_PARAMETER(ResourcesRaw);

    Count = WdfCmResourceListGetCount(ResourcesTranslated);
    for (i = 0; i < Count && Device->RegionCount < RFS_REGION_COUNT; i++)
    {
        Descriptor = WdfCmResourceListGetDescriptor(ResourcesTranslated, i);
        if (Descriptor->Type != CmResourceTypeMemory)
            continue;

        Device->Regions[Device->RegionCount].Physical = Descriptor->u.Memory.Start;
        Device->Regions[Device->RegionCount].Length = Descriptor->u.Memory.Length;
        Device->Regions[Device->RegionCount].Base =
            MmMapIoSpace(Descriptor->u.Memory.Start, Descriptor->u.Memory.Length, MmNonCached);
        QC_LOG("region %lu: phys 0x%I64x len 0x%lx va %p",
               Device->RegionCount, Descriptor->u.Memory.Start.QuadPart,
               Descriptor->u.Memory.Length, Device->Regions[Device->RegionCount].Base);
        Device->RegionCount++;
    }

    return STATUS_SUCCESS;
}

static
NTSTATUS
RfsEvtReleaseHardware(
    _In_ WDFDEVICE WdfDevice,
    _In_ WDFCMRESLIST ResourcesTranslated)
{
    PRFS_DEVICE Device = RfsGetDevice(WdfDevice);
    ULONG i;

    UNREFERENCED_PARAMETER(ResourcesTranslated);

    for (i = 0; i < Device->RegionCount; i++)
    {
        if (Device->Regions[i].Base)
            MmUnmapIoSpace(Device->Regions[i].Base, Device->Regions[i].Length);
    }
    Device->RegionCount = 0;
    return STATUS_SUCCESS;
}

static
VOID
RfsReadParameters(
    _Inout_ PRFS_DEVICE Device)
{
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDFKEY Key;
    ULONG Value;

    Device->FileBased = FALSE;
    Device->PartitionNumber = 0;

    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    if (!NT_SUCCESS(WdfDeviceOpenRegistryKey(Device->WdfDevice, PLUGPLAY_REGKEY_DRIVER, KEY_READ, &Attributes, &Key)))
        return;

    if (NT_SUCCESS(WdfRegistryQueryULong(Key, &(UNICODE_STRING)RTL_CONSTANT_STRING(L"FileBased"), &Value)))
        Device->FileBased = Value != 0;
    if (NT_SUCCESS(WdfRegistryQueryULong(Key, &(UNICODE_STRING)RTL_CONSTANT_STRING(L"PartitionNum"), &Value)))
        Device->PartitionNumber = Value;

    WdfRegistryClose(Key);
}

/* Opens the raw partition that backs the iovec service when not file based */
static
NTSTATUS
RfsOpenStorage(
    _Inout_ PRFS_DEVICE Device)
{
    OBJECT_ATTRIBUTES ObjectAttributes;
    IO_STATUS_BLOCK IoStatus;
    NTSTATUS Status;

    if (Device->FileBased)
    {
        QC_LOG("file based storage, no raw partition");
        return STATUS_SUCCESS;
    }

    /* The path lives in the device context since StoragePath outlives this call */
    Status = RtlStringCchPrintfW(Device->StorageBuffer, RTL_NUMBER_OF(Device->StorageBuffer),
                                 L"\\Device\\Harddisk0\\Partition%lu", Device->PartitionNumber);
    if (!NT_SUCCESS(Status))
        return Status;

    RtlInitUnicodeString(&Device->StoragePath, Device->StorageBuffer);
    InitializeObjectAttributes(&ObjectAttributes, &Device->StoragePath,
                               OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);

    Status = ZwCreateFile(&Device->Storage,
                          GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE,
                          &ObjectAttributes,
                          &IoStatus,
                          NULL,
                          FILE_ATTRIBUTE_NORMAL,
                          FILE_SHARE_READ | FILE_SHARE_WRITE,
                          FILE_OPEN,
                          FILE_SYNCHRONOUS_IO_NONALERT,
                          NULL,
                          0);
    if (!NT_SUCCESS(Status))
    {
        QC_LOG("storage open %wZ FAILED 0x%lx", &Device->StoragePath, Status);
        Device->Storage = NULL;
    }
    else
    {
        QC_LOG("storage %wZ open", &Device->StoragePath);
    }
    return STATUS_SUCCESS;
}

/* Brings up the QMI transport and announces the service to the modem */
static
NTSTATUS
RfsEvtSelfManagedIoInit(
    _In_ WDFDEVICE WdfDevice)
{
    PRFS_DEVICE Device = RfsGetDevice(WdfDevice);
    NTSTATUS Status;

    RfsReadParameters(Device);
    RfsOpenStorage(Device);

    Status = QmiIpcCreate(&Device->Transport);
    if (!NT_SUCCESS(Status))
    {
        QC_LOG("transport create FAILED 0x%lx", Status);
        return Status;
    }

    Status = QmiServiceCreate(&Device->Service,
                              Device->Transport,
                              RFS_SERVICE_ID,
                              RFS_SERVICE_VERSION,
                              RFS_SERVICE_INSTANCE,
                              RfsRequest,
                              Device);
    if (!NT_SUCCESS(Status))
    {
        QC_LOG("service 0x1C register FAILED 0x%lx", Status);
        QmiIpcDestroy(Device->Transport);
        Device->Transport = NULL;
        return Status;
    }

    Device->ServiceUp = TRUE;
    QC_LOG("service 0x1C up (file based %u, partition %lu, storage %p)",
           Device->FileBased, Device->PartitionNumber, Device->Storage);
    return STATUS_SUCCESS;
}

static
VOID
RfsEvtSelfManagedIoCleanup(
    _In_ WDFDEVICE WdfDevice)
{
    PRFS_DEVICE Device = RfsGetDevice(WdfDevice);

    if (Device->ServiceUp)
    {
        QmiServiceDestroy(&Device->Service);
        Device->ServiceUp = FALSE;
    }
    if (Device->Transport)
    {
        QmiIpcDestroy(Device->Transport);
        Device->Transport = NULL;
    }
    if (Device->Storage)
    {
        ZwClose(Device->Storage);
        Device->Storage = NULL;
    }
}

static
NTSTATUS
RfsEvtDeviceAdd(
    _In_ WDFDRIVER Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS PnpCallbacks;
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDFDEVICE WdfDevice;
    PRFS_DEVICE Device;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(Driver);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&PnpCallbacks);
    PnpCallbacks.EvtDevicePrepareHardware = RfsEvtPrepareHardware;
    PnpCallbacks.EvtDeviceReleaseHardware = RfsEvtReleaseHardware;
    PnpCallbacks.EvtDeviceSelfManagedIoInit = RfsEvtSelfManagedIoInit;
    PnpCallbacks.EvtDeviceSelfManagedIoCleanup = RfsEvtSelfManagedIoCleanup;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &PnpCallbacks);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, RFS_DEVICE);
    Status = WdfDeviceCreate(&DeviceInit, &Attributes, &WdfDevice);
    if (!NT_SUCCESS(Status))
        return Status;

    Device = RfsGetDevice(WdfDevice);
    RtlZeroMemory(Device, sizeof(*Device));
    Device->WdfDevice = WdfDevice;
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG Config;

    WDF_DRIVER_CONFIG_INIT(&Config, RfsEvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &Config, WDF_NO_HANDLE);
}
