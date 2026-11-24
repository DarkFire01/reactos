/*
 * PROJECT:     ReactOS USB HID Miniport Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Driver entry, registration with hidclass, power and WMI
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "hidusb.h"

#define NDEBUG
#include <debug.h>

DRIVER_INITIALIZE DriverEntry;
static DRIVER_ADD_DEVICE HidUsbAddDevice;
static DRIVER_UNLOAD HidUsbUnload;
static DRIVER_DISPATCH HidUsbCreateClose;
static DRIVER_DISPATCH HidUsbPower;
static DRIVER_DISPATCH HidUsbSystemControl;
static IO_COMPLETION_ROUTINE HidUsbPowerCompletion;
static IO_WORKITEM_ROUTINE HidUsbSetIdleWorker;

/**
 * @brief
 * Takes a reference for an outstanding request.
 *
 * @return
 * STATUS_NO_SUCH_DEVICE when the device is neither starting nor running.
 */
NTSTATUS
NTAPI
HidUsbIncrementPendingRequests(
    _Inout_ PHIDUSB_EXTENSION Extension)
{
    InterlockedIncrement(&Extension->PendingRequests);

    if ((Extension->State == HidUsbStateStarting) || (Extension->State == HidUsbStateRunning))
        return STATUS_SUCCESS;

    HidUsbDecrementPendingRequests(Extension);
    return STATUS_NO_SUCH_DEVICE;
}

VOID
NTAPI
HidUsbDecrementPendingRequests(
    _Inout_ PHIDUSB_EXTENSION Extension)
{
    if (InterlockedDecrement(&Extension->PendingRequests) < 0)
        KeSetEvent(&Extension->NoPendingRequests, IO_NO_INCREMENT, FALSE);
}

static
NTSTATUS
NTAPI
HidUsbAddDevice(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PDEVICE_OBJECT DeviceObject)
{
    PHIDUSB_EXTENSION Extension;

    UNREFERENCED_PARAMETER(DriverObject);

    /* hidclass hands over its own FDO, the minidriver extension lives inside it */
    Extension = HidUsbGetExtension(DeviceObject);

    Extension->Flags = 0;
    Extension->PendingRequests = 0;
    KeInitializeEvent(&Extension->NoPendingRequests, NotificationEvent, FALSE);
    Extension->ResetContext = NULL;
    Extension->State = HidUsbStateInitialized;
    Extension->DeviceObject = DeviceObject;
    KeInitializeSpinLock(&Extension->ResetNotifyLock);
    Extension->ResetNotifyIrp = NULL;
    IoInitializeRemoveLock(&Extension->RemoveLock, HIDUSB_TAG, 2, 0);

    return STATUS_SUCCESS;
}

static
VOID
NTAPI
HidUsbUnload(
    _In_ PDRIVER_OBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject);
}

static
NTSTATUS
NTAPI
HidUsbCreateClose(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PIO_STACK_LOCATION IoStack;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(DeviceObject);

    IoStack = IoGetCurrentIrpStackLocation(Irp);

    if ((IoStack->MajorFunction == IRP_MJ_CREATE) || (IoStack->MajorFunction == IRP_MJ_CLOSE))
    {
        Irp->IoStatus.Information = 0;
        Status = STATUS_SUCCESS;
    }
    else
    {
        Status = STATUS_INVALID_PARAMETER;
    }

    Irp->IoStatus.Status = Status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

static
VOID
NTAPI
HidUsbSetIdleWorker(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_opt_ PVOID Context)
{
    HidUsbSetIdle(DeviceObject);
    IoFreeWorkItem((PIO_WORKITEM)Context);
}

/* Devices forget their idle rate across a suspend, so it is sent again on the way back to D0 */
static
NTSTATUS
NTAPI
HidUsbPowerCompletion(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_opt_ PVOID Context)
{
    PHIDUSB_EXTENSION Extension;
    PIO_STACK_LOCATION IoStack;
    PIO_WORKITEM WorkItem;

    UNREFERENCED_PARAMETER(Context);

    Extension = HidUsbGetExtension(DeviceObject);

    if (Irp->PendingReturned)
        IoMarkIrpPending(Irp);

    IoStack = IoGetCurrentIrpStackLocation(Irp);

    if (NT_SUCCESS(Irp->IoStatus.Status) &&
        (IoStack->MinorFunction == IRP_MN_SET_POWER) &&
        (IoStack->Parameters.Power.Type == DevicePowerState) &&
        (IoStack->Parameters.Power.State.DeviceState == PowerDeviceD0) &&
        (Extension->State == HidUsbStateRunning))
    {
        if (KeGetCurrentIrql() >= DISPATCH_LEVEL)
        {
            WorkItem = IoAllocateWorkItem(DeviceObject);
            if (WorkItem != NULL)
                IoQueueWorkItem(WorkItem, HidUsbSetIdleWorker, CriticalWorkQueue, WorkItem);
            else
                DPRINT1("No work item for SET_IDLE after D0\n");
        }
        else
        {
            HidUsbSetIdle(DeviceObject);
        }
    }

    IoReleaseRemoveLock(&Extension->RemoveLock, Irp);
    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
HidUsbPower(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PHIDUSB_EXTENSION Extension;
    NTSTATUS Status;

    Extension = HidUsbGetExtension(DeviceObject);

    Status = IoAcquireRemoveLock(&Extension->RemoveLock, Irp);
    if (!NT_SUCCESS(Status))
    {
        PoStartNextPowerIrp(Irp);
        Irp->IoStatus.Status = Status;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return Status;
    }

    IoCopyCurrentIrpStackLocationToNext(Irp);
    IoSetCompletionRoutine(Irp, HidUsbPowerCompletion, NULL, TRUE, TRUE, TRUE);
    return PoCallDriver(HidUsbGetNextDevice(DeviceObject), Irp);
}

static
NTSTATUS
NTAPI
HidUsbSystemControl(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    IoCopyCurrentIrpStackLocationToNext(Irp);
    return IoCallDriver(HidUsbGetNextDevice(DeviceObject), Irp);
}

/**
 * @brief
 * Fills in the dispatch table and registers with hidclass.
 *
 * @return
 * The status hidclass returned for the registration.
 */
NTSTATUS
NTAPI
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    HID_MINIDRIVER_REGISTRATION Registration;
    NTSTATUS Status;

    DriverObject->MajorFunction[IRP_MJ_CREATE] = HidUsbCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = HidUsbCreateClose;
    DriverObject->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL] = HidUsbInternalIoctl;
    DriverObject->MajorFunction[IRP_MJ_PNP] = HidUsbPnp;
    DriverObject->MajorFunction[IRP_MJ_POWER] = HidUsbPower;
    DriverObject->MajorFunction[IRP_MJ_SYSTEM_CONTROL] = HidUsbSystemControl;
    DriverObject->DriverExtension->AddDevice = HidUsbAddDevice;
    DriverObject->DriverUnload = HidUsbUnload;

    RtlZeroMemory(&Registration, sizeof(Registration));
    Registration.Revision = HID_REVISION;
    Registration.DriverObject = DriverObject;
    Registration.RegistryPath = RegistryPath;
    Registration.DeviceExtensionSize = sizeof(HIDUSB_EXTENSION);

    /* Input arrives on the interrupt IN pipe, hidclass must not poll */
    Registration.DevicesArePolled = FALSE;

    Status = HidRegisterMinidriver(&Registration);
    if (!NT_SUCCESS(Status))
        DPRINT1("HidRegisterMinidriver failed 0x%lx\n", Status);

    return Status;
}
