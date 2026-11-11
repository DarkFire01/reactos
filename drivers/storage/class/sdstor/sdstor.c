/*
 * PROJECT:     ReactOS Storage Stack / SD storage class driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Driver entry, attachment and lower-edge request helpers
 * COPYRIGHT:   Justin Miller (justinmiller100@gmail.com)
 */

#include "precomp.h"

#define NDEBUG
#include <debug.h>

NTSTATUS
NTAPI
SdStorSubmitRequest(
    _In_ PDEVICE_OBJECT LowerDevice,
    _Inout_ PSDPORT_REQUEST Request)
{
    KEVENT Event;
    IO_STATUS_BLOCK IoStatus;
    PIRP Irp;
    NTSTATUS Status;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);

    Irp = IoBuildDeviceIoControlRequest(IOCTL_SDPORT_SUBMIT_REQUEST,
                                        LowerDevice,
                                        Request,
                                        sizeof(*Request),
                                        NULL,
                                        0,
                                        TRUE,
                                        &Event,
                                        &IoStatus);
    if (Irp == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = IoCallDriver(LowerDevice, Irp);
    if (Status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
        Status = IoStatus.Status;
    }

    return Status;
}

NTSTATUS
NTAPI
SdStorGetProperty(
    _In_ PDEVICE_OBJECT LowerDevice,
    _In_ SDPORT_PROPERTY PropertyId,
    _Out_writes_bytes_(OutputLength) PVOID Output,
    _In_ ULONG OutputLength)
{
    SDPORT_REQUEST Request;

    RtlZeroMemory(&Request, sizeof(Request));
    Request.Type = SdRequestGetProperty;
    Request.Data.Property.PropertyId = PropertyId;
    Request.Data.Property.OutputBuffer = Output;
    Request.Data.Property.OutputBufferLength = OutputLength;

    return SdStorSubmitRequest(LowerDevice, &Request);
}

NTSTATUS
NTAPI
SdStorAddDevice(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PDEVICE_OBJECT PhysicalDeviceObject)
{
    PSDSTOR_FDO_EXTENSION FdoExtension;
    PDEVICE_OBJECT Fdo;
    NTSTATUS Status;

    Status = IoCreateDevice(DriverObject,
                            sizeof(SDSTOR_FDO_EXTENSION),
                            NULL,
                            FILE_DEVICE_MASS_STORAGE,
                            FILE_DEVICE_SECURE_OPEN,
                            FALSE,
                            &Fdo);
    if (!NT_SUCCESS(Status))
        return Status;

    FdoExtension = Fdo->DeviceExtension;
    RtlZeroMemory(FdoExtension, sizeof(*FdoExtension));
    FdoExtension->ExtensionType = SdstorExtensionFdo;
    FdoExtension->Self = Fdo;
    FdoExtension->PhysicalDeviceObject = PhysicalDeviceObject;
    IoInitializeRemoveLock(&FdoExtension->RemoveLock, SDSTOR_POOL_TAG, 0, 0);

    FdoExtension->LowerDevice = IoAttachDeviceToDeviceStack(Fdo, PhysicalDeviceObject);
    if (FdoExtension->LowerDevice == NULL)
    {
        IoDeleteDevice(Fdo);
        return STATUS_NO_SUCH_DEVICE;
    }

    Fdo->Flags |= DO_DIRECT_IO | DO_POWER_PAGABLE;
    Fdo->Flags &= ~DO_DEVICE_INITIALIZING;

    return STATUS_SUCCESS;
}

VOID
NTAPI
SdStorUnload(
    _In_ PDRIVER_OBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject);
}

static
NTSTATUS
NTAPI
SdStorDispatchCreateClose(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);

    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    UNREFERENCED_PARAMETER(RegistryPath);

    DriverObject->DriverExtension->AddDevice = SdStorAddDevice;
    DriverObject->DriverUnload = SdStorUnload;
    DriverObject->MajorFunction[IRP_MJ_CREATE] = SdStorDispatchCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = SdStorDispatchCreateClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = SdStorDispatchDeviceControl;
    DriverObject->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL] = SdStorDispatchScsi;
    DriverObject->MajorFunction[IRP_MJ_SCSI] = SdStorDispatchScsi;
    DriverObject->MajorFunction[IRP_MJ_POWER] = SdStorDispatchPower;
    DriverObject->MajorFunction[IRP_MJ_SYSTEM_CONTROL] = SdStorDispatchWmi;
    DriverObject->MajorFunction[IRP_MJ_PNP] = SdStorDispatchPnp;

    return STATUS_SUCCESS;
}
