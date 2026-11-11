/*
 * PROJECT:     ReactOS Qualcomm platform drivers
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Shared PnP plumbing for the Qualcomm function drivers (qcompnp.lib)
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <ntddk.h>
#include <qcom/qcompnp.h>
#include <qcom/qcomdbg.h>

/* Each driver links its own copy, so this is per driver */
static PCSTR QcPnpClaimComponent = "qcompnp";

NTSTATUS
NTAPI
QcPnpCreateDevice(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PDEVICE_OBJECT Pdo,
    _In_ ULONG ExtensionSize,
    _In_ DEVICE_TYPE DeviceType,
    _Outptr_ PQCOM_PNP_DEVICE *Device)
{
    PDEVICE_OBJECT Fdo;
    PQCOM_PNP_DEVICE Extension;
    NTSTATUS Status;

    ASSERT(ExtensionSize >= sizeof(*Extension));

    Status = IoCreateDevice(DriverObject, ExtensionSize, NULL, DeviceType,
                            FILE_DEVICE_SECURE_OPEN, FALSE, &Fdo);
    if (!NT_SUCCESS(Status))
        return Status;

    Extension = Fdo->DeviceExtension;
    RtlZeroMemory(Extension, ExtensionSize);
    Extension->Self = Fdo;
    Extension->Pdo = Pdo;

    Extension->Lower = IoAttachDeviceToDeviceStack(Fdo, Pdo);
    if (!Extension->Lower)
    {
        IoDeleteDevice(Fdo);
        return STATUS_NO_SUCH_DEVICE;
    }

    Fdo->Flags |= DO_POWER_PAGABLE;
    Fdo->Flags &= ~DO_DEVICE_INITIALIZING;

    *Device = Extension;
    return STATUS_SUCCESS;
}

VOID
NTAPI
QcPnpDeleteDevice(
    _In_ PQCOM_PNP_DEVICE Device)
{
    IoDetachDevice(Device->Lower);
    IoDeleteDevice(Device->Self);
}

NTSTATUS
NTAPI
QcPnpForwardAndWait(
    _In_ PQCOM_PNP_DEVICE Device,
    _Inout_ PIRP Irp)
{
    if (!IoForwardIrpSynchronously(Device->Lower, Irp))
        return STATUS_UNSUCCESSFUL;

    return Irp->IoStatus.Status;
}

NTSTATUS
NTAPI
QcPnpPassDown(
    _In_ PQCOM_PNP_DEVICE Device,
    _Inout_ PIRP Irp)
{
    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(Device->Lower, Irp);
}

NTSTATUS
NTAPI
QcPnpComplete(
    _Inout_ PIRP Irp,
    _In_ NTSTATUS Status)
{
    Irp->IoStatus.Status = Status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

NTSTATUS
NTAPI
QcPnpDispatchPower(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PQCOM_PNP_DEVICE Device = DeviceObject->DeviceExtension;

    PoStartNextPowerIrp(Irp);
    IoSkipCurrentIrpStackLocation(Irp);
    return PoCallDriver(Device->Lower, Irp);
}

NTSTATUS
NTAPI
QcPnpDispatchPassDown(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    return QcPnpPassDown(DeviceObject->DeviceExtension, Irp);
}

NTSTATUS
NTAPI
QcPnpDispatchCreateClose(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);

    Irp->IoStatus.Information = 0;
    return QcPnpComplete(Irp, STATUS_SUCCESS);
}

VOID
NTAPI
QcPnpLogResources(
    _In_z_ PCSTR Component,
    _In_opt_ PCM_RESOURCE_LIST Resources)
{
    PCM_PARTIAL_RESOURCE_LIST List;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR Descriptor;
    ULONG Index;

    if (!Resources || !Resources->Count)
    {
        QcDbgLog(Component, "started with no resources");
        return;
    }

    /* ACPI reports every resource of a node in one full descriptor */
    List = &Resources->List[0].PartialResourceList;
    for (Index = 0; Index < List->Count; Index++)
    {
        Descriptor = &List->PartialDescriptors[Index];
        switch (Descriptor->Type)
        {
            case CmResourceTypeMemory:
                QcDbgLog(Component, "memory %I64x length %lx",
                         Descriptor->u.Memory.Start.QuadPart, Descriptor->u.Memory.Length);
                break;

            case CmResourceTypeInterrupt:
                QcDbgLog(Component, "interrupt %lu", Descriptor->u.Interrupt.Vector);
                break;

            default:
                break;
        }
    }
}

static
NTSTATUS
NTAPI
QcPnpClaimAddDevice(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PDEVICE_OBJECT Pdo)
{
    PQCOM_PNP_DEVICE Device;

    return QcPnpCreateDevice(DriverObject, Pdo, sizeof(*Device), FILE_DEVICE_UNKNOWN, &Device);
}

/* The exports keep working whatever state the node is in, so every request is accepted */
static
NTSTATUS
NTAPI
QcPnpClaimDispatchPnp(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PQCOM_PNP_DEVICE Device = DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS Status;

    switch (Stack->MinorFunction)
    {
        case IRP_MN_START_DEVICE:
            Status = QcPnpForwardAndWait(Device, Irp);
            if (NT_SUCCESS(Status))
                QcPnpLogResources(QcPnpClaimComponent, Stack->Parameters.StartDevice.AllocatedResources);
            return QcPnpComplete(Irp, Status);

        case IRP_MN_REMOVE_DEVICE:
            Irp->IoStatus.Status = STATUS_SUCCESS;
            Status = QcPnpPassDown(Device, Irp);
            QcPnpDeleteDevice(Device);
            return Status;

        case IRP_MN_QUERY_STOP_DEVICE:
        case IRP_MN_CANCEL_STOP_DEVICE:
        case IRP_MN_STOP_DEVICE:
        case IRP_MN_QUERY_REMOVE_DEVICE:
        case IRP_MN_CANCEL_REMOVE_DEVICE:
        case IRP_MN_SURPRISE_REMOVAL:
            Irp->IoStatus.Status = STATUS_SUCCESS;
            return QcPnpPassDown(Device, Irp);

        default:
            return QcPnpPassDown(Device, Irp);
    }
}

VOID
NTAPI
QcPnpInitializeClaimDriver(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_z_ PCSTR Component)
{
    QcPnpClaimComponent = Component;

    /* No unload routine, other drivers keep importing this image */
    DriverObject->DriverExtension->AddDevice = QcPnpClaimAddDevice;
    DriverObject->MajorFunction[IRP_MJ_PNP] = QcPnpClaimDispatchPnp;
    DriverObject->MajorFunction[IRP_MJ_POWER] = QcPnpDispatchPower;
    DriverObject->MajorFunction[IRP_MJ_SYSTEM_CONTROL] = QcPnpDispatchPassDown;
}
