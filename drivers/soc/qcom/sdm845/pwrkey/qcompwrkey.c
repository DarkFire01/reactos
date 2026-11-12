/*
 * PROJECT:     ReactOS Qualcomm platform drivers
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     PM8998 power key as a system power button
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <ntddk.h>
#include <initguid.h>
#include <poclass.h>
#include <qcom/spmiif.h>
#include <qcom/qcompnp.h>

#define QCDBG_COMPONENT "qcompwrkey"
#include <qcom/qcomdbg.h>

#define QCPWRKEY_POLL_MS        50

/* PM8998 sits at SPMI slave 0, its revision block names the part */
#define PM8998_SID              0
#define PM8998_REVID_SUBTYPE    0x0105
#define PM8998_SUBTYPE          0x14

/* Power on block real time status, the key reads 1 while it is held */
#define PM8998_PON_RT_STS       0x0810
#define PM8998_PON_KPDPWR       0x01

typedef struct _QCPWRKEY_DEVICE
{
    QCOM_PNP_DEVICE Pnp;
    UNICODE_STRING Interface;
    BOOLEAN InterfaceRegistered;
    PKTHREAD Thread;
    KEVENT StopEvent;
    BOOLEAN Held;

    /* Event IRPs the power manager keeps pending, and a press nobody waited for */
    KSPIN_LOCK Lock;
    LIST_ENTRY Waiters;
    ULONG Latched;
} QCPWRKEY_DEVICE, *PQCPWRKEY_DEVICE;

static
NTSTATUS
QcPwrKeyComplete(
    _Inout_ PIRP Irp,
    _In_ NTSTATUS Status,
    _In_ ULONG_PTR Information)
{
    Irp->IoStatus.Information = Information;
    return QcPnpComplete(Irp, Status);
}

static
VOID
NTAPI
QcPwrKeyCancel(
    _Inout_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PQCPWRKEY_DEVICE Device = DeviceObject->DeviceExtension;
    KIRQL OldIrql;

    IoReleaseCancelSpinLock(Irp->CancelIrql);

    KeAcquireSpinLock(&Device->Lock, &OldIrql);
    RemoveEntryList(&Irp->Tail.Overlay.ListEntry);
    KeReleaseSpinLock(&Device->Lock, OldIrql);

    QcPwrKeyComplete(Irp, STATUS_CANCELLED, 0);
}

/* Hands a press to the oldest waiter, or keeps it until the next event request */
static
VOID
QcPwrKeyDeliver(
    _In_ PQCPWRKEY_DEVICE Device,
    _In_ ULONG Event)
{
    PIRP Waiter = NULL;
    PIRP Head;
    KIRQL OldIrql;

    KeAcquireSpinLock(&Device->Lock, &OldIrql);
    if (!IsListEmpty(&Device->Waiters))
    {
        Head = CONTAINING_RECORD(Device->Waiters.Flink, IRP, Tail.Overlay.ListEntry);

        /* A cancel that already started owns the IRP, latch instead */
        if (IoSetCancelRoutine(Head, NULL))
        {
            RemoveEntryList(&Head->Tail.Overlay.ListEntry);
            Waiter = Head;
        }
    }
    if (!Waiter)
        Device->Latched |= Event;
    KeReleaseSpinLock(&Device->Lock, OldIrql);

    if (Waiter)
    {
        *(PULONG)Waiter->AssociatedIrp.SystemBuffer = Event;
        QcPwrKeyComplete(Waiter, STATUS_SUCCESS, sizeof(Event));
    }
}

static
NTSTATUS
QcPwrKeyWaitForEvent(
    _In_ PQCPWRKEY_DEVICE Device,
    _Inout_ PIRP Irp)
{
    ULONG Event;
    KIRQL OldIrql;

    KeAcquireSpinLock(&Device->Lock, &OldIrql);

    Event = Device->Latched;
    if (Event)
    {
        Device->Latched = 0;
        KeReleaseSpinLock(&Device->Lock, OldIrql);

        *(PULONG)Irp->AssociatedIrp.SystemBuffer = Event;
        return QcPwrKeyComplete(Irp, STATUS_SUCCESS, sizeof(Event));
    }

    IoMarkIrpPending(Irp);
    InsertTailList(&Device->Waiters, &Irp->Tail.Overlay.ListEntry);
    IoSetCancelRoutine(Irp, QcPwrKeyCancel);

    /* Cancelled before the routine was in place, undo the queueing */
    if (Irp->Cancel && IoSetCancelRoutine(Irp, NULL))
    {
        RemoveEntryList(&Irp->Tail.Overlay.ListEntry);
        KeReleaseSpinLock(&Device->Lock, OldIrql);
        QcPwrKeyComplete(Irp, STATUS_CANCELLED, 0);
        return STATUS_PENDING;
    }

    KeReleaseSpinLock(&Device->Lock, OldIrql);
    return STATUS_PENDING;
}

static
NTSTATUS
NTAPI
QcPwrKeyDeviceControl(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PQCPWRKEY_DEVICE Device = DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    ULONG IoControlCode = Stack->Parameters.DeviceIoControl.IoControlCode;

    if (IoControlCode != IOCTL_GET_SYS_BUTTON_CAPS && IoControlCode != IOCTL_GET_SYS_BUTTON_EVENT)
        return QcPnpPassDown(&Device->Pnp, Irp);

    /* Only the kernel talks to a system button */
    if (Irp->RequestorMode != KernelMode)
        return QcPwrKeyComplete(Irp, STATUS_NOT_IMPLEMENTED, 0);

    if (Stack->Parameters.DeviceIoControl.OutputBufferLength < sizeof(ULONG) ||
        !Irp->AssociatedIrp.SystemBuffer)
    {
        return QcPwrKeyComplete(Irp, STATUS_BUFFER_TOO_SMALL, 0);
    }

    if (IoControlCode == IOCTL_GET_SYS_BUTTON_EVENT)
        return QcPwrKeyWaitForEvent(Device, Irp);

    *(PULONG)Irp->AssociatedIrp.SystemBuffer = SYS_BUTTON_POWER;
    return QcPwrKeyComplete(Irp, STATUS_SUCCESS, sizeof(ULONG));
}

static
VOID
NTAPI
QcPwrKeyThread(
    _In_ PVOID Context)
{
    PQCPWRKEY_DEVICE Device = Context;
    LARGE_INTEGER Period;
    UCHAR Status;
    BOOLEAN Held;

    Period.QuadPart = -10000LL * QCPWRKEY_POLL_MS;
    while (KeWaitForSingleObject(&Device->StopEvent, Executive, KernelMode, FALSE, &Period) == STATUS_TIMEOUT)
    {
        if (!NT_SUCCESS(QcSpmiRead(PM8998_SID, PM8998_PON_RT_STS, &Status, sizeof(Status))))
            continue;

        /* A press is the edge from released to held */
        Held = (Status & PM8998_PON_KPDPWR) != 0;
        if (Held && !Device->Held)
        {
            QC_LOG("power key pressed");
            QcPwrKeyDeliver(Device, SYS_BUTTON_POWER);
        }
        Device->Held = Held;
    }

    PsTerminateSystemThread(STATUS_SUCCESS);
}

/* Safe on a half started device, every piece is torn down only if it exists */
static
VOID
QcPwrKeyStop(
    _In_ PQCPWRKEY_DEVICE Device)
{
    PLIST_ENTRY Entry;
    PIRP Irp;
    KIRQL OldIrql;

    if (Device->InterfaceRegistered)
    {
        IoSetDeviceInterfaceState(&Device->Interface, FALSE);
        RtlFreeUnicodeString(&Device->Interface);
        Device->InterfaceRegistered = FALSE;
    }

    if (Device->Thread)
    {
        KeSetEvent(&Device->StopEvent, IO_NO_INCREMENT, FALSE);
        KeWaitForSingleObject(Device->Thread, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(Device->Thread);
        Device->Thread = NULL;
    }

    /* Waiters fail so the power manager drops this switch */
    for (;;)
    {
        Irp = NULL;
        KeAcquireSpinLock(&Device->Lock, &OldIrql);
        while (!IsListEmpty(&Device->Waiters))
        {
            Entry = RemoveHeadList(&Device->Waiters);
            Irp = CONTAINING_RECORD(Entry, IRP, Tail.Overlay.ListEntry);
            if (IoSetCancelRoutine(Irp, NULL))
                break;

            /* Its cancel routine is running and removes it, it is on no list now */
            InitializeListHead(&Irp->Tail.Overlay.ListEntry);
            Irp = NULL;
        }
        KeReleaseSpinLock(&Device->Lock, OldIrql);

        if (!Irp)
            break;
        QcPwrKeyComplete(Irp, STATUS_DEVICE_REMOVED, 0);
    }
}

static
NTSTATUS
QcPwrKeyStart(
    _In_ PQCPWRKEY_DEVICE Device)
{
    HANDLE Thread;
    UCHAR Value;
    NTSTATUS Status;

    /* The key register below is the PM8998 one, refuse any other PMIC */
    Status = QcSpmiRead(PM8998_SID, PM8998_REVID_SUBTYPE, &Value, sizeof(Value));
    if (!NT_SUCCESS(Status))
        return Status;
    if (Value != PM8998_SUBTYPE)
    {
        QC_LOG("PMIC at slave %u is subtype %02x, not a PM8998", PM8998_SID, Value);
        return STATUS_NOT_SUPPORTED;
    }

    /* A key already held at start, a long press at boot, is not a press */
    Status = QcSpmiRead(PM8998_SID, PM8998_PON_RT_STS, &Value, sizeof(Value));
    if (!NT_SUCCESS(Status))
        return Status;
    Device->Held = (Value & PM8998_PON_KPDPWR) != 0;
    Device->Latched = 0;
    KeClearEvent(&Device->StopEvent);

    Status = PsCreateSystemThread(&Thread, THREAD_ALL_ACCESS, NULL, NULL, NULL, QcPwrKeyThread, Device);
    if (!NT_SUCCESS(Status))
        return Status;
    ObReferenceObjectByHandle(Thread, THREAD_ALL_ACCESS, NULL, KernelMode, (PVOID *)&Device->Thread, NULL);
    ZwClose(Thread);

    /* The power manager finds system buttons through this interface */
    Status = IoRegisterDeviceInterface(Device->Pnp.Pdo, &GUID_DEVICE_SYS_BUTTON, NULL, &Device->Interface);
    if (!NT_SUCCESS(Status))
    {
        QcPwrKeyStop(Device);
        return Status;
    }
    Device->InterfaceRegistered = TRUE;
    IoSetDeviceInterfaceState(&Device->Interface, TRUE);

    QC_LOG("power key ready");
    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
QcPwrKeyDispatchPnp(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PQCPWRKEY_DEVICE Device = DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS Status;

    switch (Stack->MinorFunction)
    {
        case IRP_MN_START_DEVICE:
            Status = QcPnpForwardAndWait(&Device->Pnp, Irp);
            if (NT_SUCCESS(Status))
                Status = QcPwrKeyStart(Device);
            return QcPnpComplete(Irp, Status);

        /* The key has no resources to move, so a rebalance is refused */
        case IRP_MN_QUERY_STOP_DEVICE:
            return QcPnpComplete(Irp, STATUS_UNSUCCESSFUL);

        case IRP_MN_STOP_DEVICE:
        case IRP_MN_SURPRISE_REMOVAL:
            QcPwrKeyStop(Device);
            Irp->IoStatus.Status = STATUS_SUCCESS;
            return QcPnpPassDown(&Device->Pnp, Irp);

        case IRP_MN_REMOVE_DEVICE:
            QcPwrKeyStop(Device);
            Irp->IoStatus.Status = STATUS_SUCCESS;
            Status = QcPnpPassDown(&Device->Pnp, Irp);
            QcPnpDeleteDevice(&Device->Pnp);
            return Status;

        case IRP_MN_QUERY_REMOVE_DEVICE:
        case IRP_MN_CANCEL_REMOVE_DEVICE:
        case IRP_MN_CANCEL_STOP_DEVICE:
            Irp->IoStatus.Status = STATUS_SUCCESS;
            return QcPnpPassDown(&Device->Pnp, Irp);

        default:
            return QcPnpPassDown(&Device->Pnp, Irp);
    }
}

static
NTSTATUS
NTAPI
QcPwrKeyAddDevice(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PDEVICE_OBJECT Pdo)
{
    PQCOM_PNP_DEVICE Pnp;
    PQCPWRKEY_DEVICE Device;
    NTSTATUS Status;

    Status = QcPnpCreateDevice(DriverObject, Pdo, sizeof(*Device), FILE_DEVICE_UNKNOWN, &Pnp);
    if (!NT_SUCCESS(Status))
        return Status;

    Pnp->Self->Flags |= DO_BUFFERED_IO;

    Device = CONTAINING_RECORD(Pnp, QCPWRKEY_DEVICE, Pnp);
    KeInitializeEvent(&Device->StopEvent, NotificationEvent, FALSE);
    KeInitializeSpinLock(&Device->Lock);
    InitializeListHead(&Device->Waiters);
    return STATUS_SUCCESS;
}

static
VOID
NTAPI
QcPwrKeyUnload(
    _In_ PDRIVER_OBJECT DriverObject)
{
    /* Every device was stopped and deleted by its REMOVE */
    UNREFERENCED_PARAMETER(DriverObject);
}

NTSTATUS
NTAPI
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    UNREFERENCED_PARAMETER(RegistryPath);

    DriverObject->DriverExtension->AddDevice = QcPwrKeyAddDevice;
    DriverObject->DriverUnload = QcPwrKeyUnload;
    DriverObject->MajorFunction[IRP_MJ_CREATE] = QcPnpDispatchCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = QcPnpDispatchCreateClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = QcPwrKeyDeviceControl;
    DriverObject->MajorFunction[IRP_MJ_PNP] = QcPwrKeyDispatchPnp;
    DriverObject->MajorFunction[IRP_MJ_POWER] = QcPnpDispatchPower;
    DriverObject->MajorFunction[IRP_MJ_SYSTEM_CONTROL] = QcPnpDispatchPassDown;

    return STATUS_SUCCESS;
}
