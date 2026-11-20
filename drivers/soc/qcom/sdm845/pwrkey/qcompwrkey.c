/*
 * PROJECT:     ReactOS Qualcomm platform drivers
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Generic buttons device, the power key as the system power button and the rest for the shell
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <ntifs.h>
#include <initguid.h>
#include <poclass.h>
#include <acpiioct.h>
#include <ndk/pofuncs.h>
#include <qcom/qcompnp.h>
#include <qcom/buttonsif.h>

#define QCDBG_COMPONENT "qcompwrkey"
#include <qcom/qcomdbg.h>

#define QCBTN_TAG               'ntBQ'
#define QCBTN_MAX_LINES         8
#define QCBTN_QUEUE_LENGTH      16

/* Held this long the key powers the phone off, a shorter press is the power button */
#define QCBTN_LONG_PRESS_MS     2000

/* A held volume key steps again after the delay, then at the rate */
#define QCBTN_REPEAT_DELAY_MS   500
#define QCBTN_REPEAT_RATE_MS    150

/* Generic Buttons Device _DSD, HID usage records per interrupt */
#define QCBTN_DSD_BUFFER        1024
#define QCBTN_DSD_RECORD_CONTROL 1
#define QCBTN_PAGE_SYSTEM       0x01
#define QCBTN_SYSTEM_POWER      0x81

DEFINE_GUID(QCBTN_DSD_UUID, 0xfa6bd625, 0x9ce8, 0x470d, 0xa2, 0xc7, 0xb3, 0xca, 0x36, 0xc4, 0x28, 0x2e);

typedef struct _QCBTN_DEVICE QCBTN_DEVICE, *PQCBTN_DEVICE;

/* One GpioInt of the device, with the usage the firmware gave it */
typedef struct _QCBTN_LINE
{
    PQCBTN_DEVICE Device;
    ULONG Index;
    PKINTERRUPT Interrupt;
    USHORT UsagePage;
    USHORT Usage;
    BOOLEAN Held;
    ULONGLONG PressTime;
    ULONGLONG NextRepeat;
} QCBTN_LINE, *PQCBTN_LINE;

struct _QCBTN_DEVICE
{
    QCOM_PNP_DEVICE Pnp;
    UNICODE_STRING Interface;
    BOOLEAN InterfaceRegistered;
    PKTHREAD Thread;
    KEVENT StopEvent;
    KEVENT EdgeEvent;
    KDPC EdgeDpc;
    volatile LONG Pending;
    BOOLEAN LongPressSent;

    QCBTN_LINE Lines[QCBTN_MAX_LINES];
    ULONG LineCount;

    /* Event IRPs the power manager keeps pending, and a press nobody waited for */
    KSPIN_LOCK Lock;
    LIST_ENTRY Waiters;
    ULONG Latched;

    /* What the shell reads through \\.\QcomButtons */
    PDEVICE_OBJECT Control;
    LIST_ENTRY ShellWaiters;
    QCOM_BUTTON_EVENT Queue[QCBTN_QUEUE_LENGTH];
    ULONG QueueHead;
    ULONG QueueCount;
};

/* The control device extension, it only points back at the button device */
typedef struct _QCBTN_CONTROL
{
    PQCBTN_DEVICE Device;
} QCBTN_CONTROL, *PQCBTN_CONTROL;

static
NTSTATUS
QcBtnComplete(
    _Inout_ PIRP Irp,
    _In_ NTSTATUS Status,
    _In_ ULONG_PTR Information)
{
    Irp->IoStatus.Information = Information;
    return QcPnpComplete(Irp, Status);
}

/* \.\QcomButtons, there is one generic buttons device per phone */
static PDEVICE_OBJECT QcBtnControlObject;

static
BOOLEAN
QcBtnIsControl(
    _In_ PDEVICE_OBJECT DeviceObject)
{
    return DeviceObject == QcBtnControlObject;
}

static
VOID
NTAPI
QcBtnCancel(
    _Inout_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PQCBTN_DEVICE Device;
    KIRQL OldIrql;

    IoReleaseCancelSpinLock(Irp->CancelIrql);

    if (QcBtnIsControl(DeviceObject))
        Device = ((PQCBTN_CONTROL)DeviceObject->DeviceExtension)->Device;
    else
        Device = DeviceObject->DeviceExtension;

    KeAcquireSpinLock(&Device->Lock, &OldIrql);
    RemoveEntryList(&Irp->Tail.Overlay.ListEntry);
    KeReleaseSpinLock(&Device->Lock, OldIrql);

    QcBtnComplete(Irp, STATUS_CANCELLED, 0);
}

/* Takes the oldest waiter off a list unless its cancel routine already owns it */
static
PIRP
QcBtnTakeWaiter(
    _Inout_ PLIST_ENTRY List)
{
    PIRP Irp;

    while (!IsListEmpty(List))
    {
        Irp = CONTAINING_RECORD(List->Flink, IRP, Tail.Overlay.ListEntry);
        RemoveEntryList(&Irp->Tail.Overlay.ListEntry);
        if (IoSetCancelRoutine(Irp, NULL))
            return Irp;

        /* Its cancel routine is running and removes it, it is on no list now */
        InitializeListHead(&Irp->Tail.Overlay.ListEntry);
    }

    return NULL;
}

/* Must be called with the lock held, queues the IRP or completes it if cancelled */
static
NTSTATUS
QcBtnPend(
    _Inout_ PLIST_ENTRY List,
    _Inout_ PIRP Irp,
    _Inout_ PBOOLEAN Cancelled)
{
    IoMarkIrpPending(Irp);
    InsertTailList(List, &Irp->Tail.Overlay.ListEntry);
    IoSetCancelRoutine(Irp, QcBtnCancel);

    *Cancelled = FALSE;
    if (Irp->Cancel && IoSetCancelRoutine(Irp, NULL))
    {
        RemoveEntryList(&Irp->Tail.Overlay.ListEntry);
        *Cancelled = TRUE;
    }

    return STATUS_PENDING;
}

/* Hands a press to the oldest power manager waiter, or keeps it until the next request */
static
VOID
QcBtnDeliverPower(
    _In_ PQCBTN_DEVICE Device,
    _In_ ULONG Event)
{
    PIRP Waiter;
    KIRQL OldIrql;

    KeAcquireSpinLock(&Device->Lock, &OldIrql);
    Waiter = QcBtnTakeWaiter(&Device->Waiters);
    if (!Waiter)
        Device->Latched |= Event;
    KeReleaseSpinLock(&Device->Lock, OldIrql);

    if (Waiter)
    {
        *(PULONG)Waiter->AssociatedIrp.SystemBuffer = Event;
        QcBtnComplete(Waiter, STATUS_SUCCESS, sizeof(Event));
    }
}

/* Gives the event to a waiting shell, or queues it, dropping the oldest when full */
static
VOID
QcBtnDeliverShell(
    _In_ PQCBTN_DEVICE Device,
    _In_ const QCBTN_LINE *Line,
    _In_ ULONG State)
{
    QCOM_BUTTON_EVENT Event;
    PIRP Waiter;
    KIRQL OldIrql;

    Event.UsagePage = Line->UsagePage;
    Event.Usage = Line->Usage;
    Event.State = State;

    KeAcquireSpinLock(&Device->Lock, &OldIrql);
    Waiter = QcBtnTakeWaiter(&Device->ShellWaiters);
    if (!Waiter)
    {
        if (Device->QueueCount == QCBTN_QUEUE_LENGTH)
        {
            Device->QueueHead = (Device->QueueHead + 1) % QCBTN_QUEUE_LENGTH;
            Device->QueueCount--;
        }
        Device->Queue[(Device->QueueHead + Device->QueueCount) % QCBTN_QUEUE_LENGTH] = Event;
        Device->QueueCount++;
    }
    KeReleaseSpinLock(&Device->Lock, OldIrql);

    if (Waiter)
    {
        *(PQCOM_BUTTON_EVENT)Waiter->AssociatedIrp.SystemBuffer = Event;
        QcBtnComplete(Waiter, STATUS_SUCCESS, sizeof(Event));
    }
}

static
NTSTATUS
QcBtnWaitPower(
    _In_ PQCBTN_DEVICE Device,
    _Inout_ PIRP Irp)
{
    BOOLEAN Cancelled;
    ULONG Event;
    KIRQL OldIrql;

    KeAcquireSpinLock(&Device->Lock, &OldIrql);

    Event = Device->Latched;
    if (Event)
    {
        Device->Latched = 0;
        KeReleaseSpinLock(&Device->Lock, OldIrql);

        *(PULONG)Irp->AssociatedIrp.SystemBuffer = Event;
        return QcBtnComplete(Irp, STATUS_SUCCESS, sizeof(Event));
    }

    QcBtnPend(&Device->Waiters, Irp, &Cancelled);
    KeReleaseSpinLock(&Device->Lock, OldIrql);

    if (Cancelled)
        QcBtnComplete(Irp, STATUS_CANCELLED, 0);
    return STATUS_PENDING;
}

static
NTSTATUS
QcBtnWaitShell(
    _In_ PQCBTN_DEVICE Device,
    _Inout_ PIRP Irp)
{
    QCOM_BUTTON_EVENT Event;
    BOOLEAN Cancelled;
    KIRQL OldIrql;

    KeAcquireSpinLock(&Device->Lock, &OldIrql);

    if (Device->QueueCount)
    {
        Event = Device->Queue[Device->QueueHead];
        Device->QueueHead = (Device->QueueHead + 1) % QCBTN_QUEUE_LENGTH;
        Device->QueueCount--;
        KeReleaseSpinLock(&Device->Lock, OldIrql);

        *(PQCOM_BUTTON_EVENT)Irp->AssociatedIrp.SystemBuffer = Event;
        return QcBtnComplete(Irp, STATUS_SUCCESS, sizeof(Event));
    }

    QcBtnPend(&Device->ShellWaiters, Irp, &Cancelled);
    KeReleaseSpinLock(&Device->Lock, OldIrql);

    if (Cancelled)
        QcBtnComplete(Irp, STATUS_CANCELLED, 0);
    return STATUS_PENDING;
}

static
NTSTATUS
NTAPI
QcBtnDeviceControl(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    ULONG IoControlCode = Stack->Parameters.DeviceIoControl.IoControlCode;
    ULONG OutLength = Stack->Parameters.DeviceIoControl.OutputBufferLength;
    PQCBTN_DEVICE Device;

    if (QcBtnIsControl(DeviceObject))
    {
        Device = ((PQCBTN_CONTROL)DeviceObject->DeviceExtension)->Device;
        if (IoControlCode != IOCTL_QCOM_BUTTONS_WAIT || !Device)
            return QcBtnComplete(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
        if (OutLength < sizeof(QCOM_BUTTON_EVENT))
            return QcBtnComplete(Irp, STATUS_BUFFER_TOO_SMALL, 0);
        return QcBtnWaitShell(Device, Irp);
    }

    Device = DeviceObject->DeviceExtension;
    if (IoControlCode != IOCTL_GET_SYS_BUTTON_CAPS && IoControlCode != IOCTL_GET_SYS_BUTTON_EVENT)
        return QcPnpPassDown(&Device->Pnp, Irp);

    /* Only the kernel talks to a system button */
    if (Irp->RequestorMode != KernelMode)
        return QcBtnComplete(Irp, STATUS_NOT_IMPLEMENTED, 0);

    if (OutLength < sizeof(ULONG) || !Irp->AssociatedIrp.SystemBuffer)
        return QcBtnComplete(Irp, STATUS_BUFFER_TOO_SMALL, 0);

    if (IoControlCode == IOCTL_GET_SYS_BUTTON_EVENT)
        return QcBtnWaitPower(Device, Irp);

    *(PULONG)Irp->AssociatedIrp.SystemBuffer = SYS_BUTTON_POWER;
    return QcBtnComplete(Irp, STATUS_SUCCESS, sizeof(ULONG));
}

static
NTSTATUS
NTAPI
QcBtnCreateClose(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    if (QcBtnIsControl(DeviceObject))
        return QcBtnComplete(Irp, STATUS_SUCCESS, 0);

    return QcPnpDispatchCreateClose(DeviceObject, Irp);
}

static
VOID
QcBtnShutdown(VOID)
{
    NTSTATUS Status;

    Status = ZwInitiatePowerAction(PowerActionShutdownOff, PowerSystemShutdown, 0, TRUE);
    if (!NT_SUCCESS(Status))
        QC_LOG("shutdown request failed, %08lx", Status);
}

static
BOOLEAN
QcBtnIsPower(
    _In_ const QCBTN_LINE *Line)
{
    return Line->UsagePage == QCBTN_PAGE_SYSTEM && Line->Usage == QCBTN_SYSTEM_POWER;
}

static
BOOLEAN
QcBtnRepeats(
    _In_ const QCBTN_LINE *Line)
{
    return Line->UsagePage == QCOM_BUTTON_PAGE_CONSUMER &&
           (Line->Usage == QCOM_BUTTON_VOLUME_UP || Line->Usage == QCOM_BUTTON_VOLUME_DOWN);
}

/*
 * Both edges interrupt, so each one flips the state. The lines are pulled to
 * their released level, which is where every button starts.
 */
static
VOID
QcBtnEdge(
    _In_ PQCBTN_DEVICE Device,
    _Inout_ PQCBTN_LINE Line,
    _In_ ULONGLONG Now)
{
    Line->Held = !Line->Held;

    if (Line->Held)
    {
        Line->PressTime = Now;
        Line->NextRepeat = Now + 10000ULL * QCBTN_REPEAT_DELAY_MS;
        if (QcBtnIsPower(Line))
            Device->LongPressSent = FALSE;
    }

    if (QcBtnIsPower(Line))
    {
        /* A quick press is the power button, Modern Standby makes it the display key */
        if (!Line->Held && !Device->LongPressSent)
            QcBtnDeliverPower(Device, SYS_BUTTON_POWER);
        return;
    }

    QC_LOG("button %02x:%02x %s", Line->UsagePage, Line->Usage, Line->Held ? "down" : "up");
    QcBtnDeliverShell(Device, Line, Line->Held ? QCOM_BUTTON_PRESSED : QCOM_BUTTON_RELEASED);
}

/* Long presses and key repeat, both need time to pass with no edge */
static
VOID
QcBtnTick(
    _In_ PQCBTN_DEVICE Device,
    _In_ ULONGLONG Now,
    _Inout_ PULONGLONG Deadline)
{
    PQCBTN_LINE Line;
    ULONG i;

    for (i = 0; i < Device->LineCount; i++)
    {
        Line = &Device->Lines[i];
        if (!Line->Held)
            continue;

        if (QcBtnIsPower(Line))
        {
            if (Device->LongPressSent)
                continue;

            if (Now - Line->PressTime >= 10000ULL * QCBTN_LONG_PRESS_MS)
            {
                /* Still held, power the phone off whatever the button policy says */
                QC_LOG("power key held, shutting down");
                Device->LongPressSent = TRUE;
                QcBtnShutdown();
                continue;
            }

            *Deadline = min(*Deadline, Line->PressTime + 10000ULL * QCBTN_LONG_PRESS_MS);
        }
        else if (QcBtnRepeats(Line))
        {
            if (Now >= Line->NextRepeat)
            {
                QcBtnDeliverShell(Device, Line, QCOM_BUTTON_REPEAT);
                Line->NextRepeat = Now + 10000ULL * QCBTN_REPEAT_RATE_MS;
            }

            *Deadline = min(*Deadline, Line->NextRepeat);
        }
    }
}

static
VOID
NTAPI
QcBtnThread(
    _In_ PVOID Context)
{
    PQCBTN_DEVICE Device = Context;
    PVOID Objects[2];
    LARGE_INTEGER Timeout;
    ULONGLONG Now, Deadline;
    NTSTATUS Status;
    LONG Pending;
    ULONG i;

    Objects[0] = &Device->StopEvent;
    Objects[1] = &Device->EdgeEvent;

    for (;;)
    {
        Now = KeQueryInterruptTime();
        Deadline = MAXULONGLONG;
        QcBtnTick(Device, Now, &Deadline);

        if (Deadline == MAXULONGLONG)
        {
            Status = KeWaitForMultipleObjects(2, Objects, WaitAny, Executive, KernelMode, FALSE, NULL, NULL);
        }
        else
        {
            Timeout.QuadPart = -(LONGLONG)(Deadline > Now ? Deadline - Now : 1);
            Status = KeWaitForMultipleObjects(2, Objects, WaitAny, Executive, KernelMode, FALSE, &Timeout, NULL);
        }

        if (Status == STATUS_WAIT_0)
            break;

        Pending = InterlockedExchange(&Device->Pending, 0);
        Now = KeQueryInterruptTime();
        for (i = 0; i < Device->LineCount; i++)
        {
            if (Pending & (1 << i))
                QcBtnEdge(Device, &Device->Lines[i], Now);
        }
    }

    PsTerminateSystemThread(STATUS_SUCCESS);
}

static
VOID
NTAPI
QcBtnEdgeDpc(
    _In_ PKDPC Dpc,
    _In_opt_ PVOID Context,
    _In_opt_ PVOID Argument1,
    _In_opt_ PVOID Argument2)
{
    PQCBTN_DEVICE Device = Context;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(Argument1);
    UNREFERENCED_PARAMETER(Argument2);

    KeSetEvent(&Device->EdgeEvent, IO_NO_INCREMENT, FALSE);
}

static
BOOLEAN
NTAPI
QcBtnIsr(
    _In_ PKINTERRUPT Interrupt,
    _In_ PVOID Context)
{
    PQCBTN_LINE Line = Context;

    UNREFERENCED_PARAMETER(Interrupt);

    InterlockedOr(&Line->Device->Pending, 1 << Line->Index);
    KeInsertQueueDpc(&Line->Device->EdgeDpc, NULL, NULL);
    return TRUE;
}

static
ULONG
QcBtnInteger(
    _In_ const ACPI_METHOD_ARGUMENT *Argument)
{
    if (Argument->Type != ACPI_METHOD_ARGUMENT_INTEGER)
        return MAXULONG;
    return Argument->Argument;
}

/* Reads the HID usage of each interrupt from the generic buttons _DSD */
static
NTSTATUS
QcBtnReadUsages(
    _Inout_ PQCBTN_DEVICE Device)
{
    ACPI_EVAL_INPUT_BUFFER Input;
    PACPI_EVAL_OUTPUT_BUFFER Output;
    PACPI_METHOD_ARGUMENT Uuid, Records, Record, Field;
    PUCHAR RecordsEnd, RecordEnd;
    ULONG Values[5], Count;
    IO_STATUS_BLOCK IoStatus;
    KEVENT Event;
    NTSTATUS Status;
    PIRP Irp;

    Output = ExAllocatePoolWithTag(PagedPool, QCBTN_DSD_BUFFER, QCBTN_TAG);
    if (!Output)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlZeroMemory(&Input, sizeof(Input));
    Input.Signature = ACPI_EVAL_INPUT_BUFFER_SIGNATURE;
    RtlCopyMemory(Input.MethodName, "_DSD", sizeof(Input.MethodName));

    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    Irp = IoBuildDeviceIoControlRequest(IOCTL_ACPI_EVAL_METHOD,
                                        Device->Pnp.Lower,
                                        &Input,
                                        sizeof(Input),
                                        Output,
                                        QCBTN_DSD_BUFFER,
                                        FALSE,
                                        &Event,
                                        &IoStatus);
    if (!Irp)
    {
        ExFreePoolWithTag(Output, QCBTN_TAG);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Status = IoCallDriver(Device->Pnp.Lower, Irp);
    if (Status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
        Status = IoStatus.Status;
    }

    if (!NT_SUCCESS(Status) || Output->Signature != ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE || Output->Count < 2)
    {
        ExFreePoolWithTag(Output, QCBTN_TAG);
        return NT_SUCCESS(Status) ? STATUS_ACPI_INVALID_DATA : Status;
    }

    /* The UUID buffer, then the package of usage records */
    Uuid = Output->Argument;
    Records = ACPI_METHOD_NEXT_ARGUMENT(Uuid);
    if (Uuid->Type != ACPI_METHOD_ARGUMENT_BUFFER || Uuid->DataLength != sizeof(GUID) ||
        !RtlEqualMemory(Uuid->Data, &QCBTN_DSD_UUID, sizeof(GUID)) ||
        Records->Type != ACPI_METHOD_ARGUMENT_PACKAGE)
    {
        ExFreePoolWithTag(Output, QCBTN_TAG);
        return STATUS_ACPI_INVALID_DATA;
    }

    RecordsEnd = Records->Data + Records->DataLength;
    for (Record = (PACPI_METHOD_ARGUMENT)Records->Data;
         (PUCHAR)Record < RecordsEnd;
         Record = ACPI_METHOD_NEXT_ARGUMENT(Record))
    {
        if (Record->Type != ACPI_METHOD_ARGUMENT_PACKAGE)
            continue;

        RecordEnd = Record->Data + Record->DataLength;
        Count = 0;
        for (Field = (PACPI_METHOD_ARGUMENT)Record->Data;
             (PUCHAR)Field < RecordEnd && Count < RTL_NUMBER_OF(Values);
             Field = ACPI_METHOD_NEXT_ARGUMENT(Field))
        {
            Values[Count++] = QcBtnInteger(Field);
        }

        /* Control, interrupt index, collection, usage page, usage */
        if (Count < 5 || Values[0] != QCBTN_DSD_RECORD_CONTROL || Values[1] >= Device->LineCount)
            continue;

        Device->Lines[Values[1]].UsagePage = (USHORT)Values[3];
        Device->Lines[Values[1]].Usage = (USHORT)Values[4];
    }

    ExFreePoolWithTag(Output, QCBTN_TAG);
    return STATUS_SUCCESS;
}

static
VOID
QcBtnDisconnect(
    _Inout_ PQCBTN_DEVICE Device)
{
    IO_DISCONNECT_INTERRUPT_PARAMETERS Disconnect;
    ULONG i;

    for (i = 0; i < Device->LineCount; i++)
    {
        if (!Device->Lines[i].Interrupt)
            continue;

        RtlZeroMemory(&Disconnect, sizeof(Disconnect));
        Disconnect.Version = CONNECT_FULLY_SPECIFIED;
        Disconnect.ConnectionContext.InterruptObject = Device->Lines[i].Interrupt;
        IoDisconnectInterruptEx(&Disconnect);
        Device->Lines[i].Interrupt = NULL;
    }

    KeRemoveQueueDpc(&Device->EdgeDpc);
}

/* Every GpioInt arrives as an interrupt resource, in _CRS order */
static
NTSTATUS
QcBtnConnect(
    _Inout_ PQCBTN_DEVICE Device,
    _In_ PCM_RESOURCE_LIST Translated)
{
    IO_CONNECT_INTERRUPT_PARAMETERS Connect;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR Descriptor;
    PCM_PARTIAL_RESOURCE_LIST List;
    PQCBTN_LINE Line;
    NTSTATUS Status;
    ULONG i;

    if (!Translated || !Translated->Count)
        return STATUS_DEVICE_CONFIGURATION_ERROR;

    List = &Translated->List[0].PartialResourceList;
    for (i = 0; i < List->Count && Device->LineCount < QCBTN_MAX_LINES; i++)
    {
        Descriptor = &List->PartialDescriptors[i];
        if (Descriptor->Type != CmResourceTypeInterrupt)
            continue;

        Line = &Device->Lines[Device->LineCount];
        RtlZeroMemory(Line, sizeof(*Line));
        Line->Device = Device;
        Line->Index = Device->LineCount++;

        RtlZeroMemory(&Connect, sizeof(Connect));
        Connect.Version = CONNECT_FULLY_SPECIFIED;
        Connect.FullySpecified.PhysicalDeviceObject = Device->Pnp.Pdo;
        Connect.FullySpecified.InterruptObject = &Line->Interrupt;
        Connect.FullySpecified.ServiceRoutine = QcBtnIsr;
        Connect.FullySpecified.ServiceContext = Line;
        Connect.FullySpecified.SynchronizeIrql = (KIRQL)Descriptor->u.Interrupt.Level;
        Connect.FullySpecified.Irql = (KIRQL)Descriptor->u.Interrupt.Level;
        Connect.FullySpecified.Vector = Descriptor->u.Interrupt.Vector;
        Connect.FullySpecified.InterruptMode = (Descriptor->Flags & CM_RESOURCE_INTERRUPT_LATCHED) ?
                                               Latched : LevelSensitive;
        Connect.FullySpecified.ShareVector = Descriptor->ShareDisposition == CmResourceShareShared;
        Connect.FullySpecified.ProcessorEnableMask = Descriptor->u.Interrupt.Affinity;

        Status = IoConnectInterruptEx(&Connect);
        if (!NT_SUCCESS(Status))
        {
            QC_LOG("interrupt %lu (vector %lu) failed to connect, %08lx",
                   Line->Index, Descriptor->u.Interrupt.Vector, Status);
            Line->Interrupt = NULL;
        }
    }

    return Device->LineCount ? STATUS_SUCCESS : STATUS_DEVICE_CONFIGURATION_ERROR;
}

static
VOID
QcBtnDeleteControl(
    _Inout_ PQCBTN_DEVICE Device)
{
    UNICODE_STRING Link = RTL_CONSTANT_STRING(L"\\DosDevices\\QcomButtons");

    if (!Device->Control)
        return;

    IoDeleteSymbolicLink(&Link);
    QcBtnControlObject = NULL;
    ((PQCBTN_CONTROL)Device->Control->DeviceExtension)->Device = NULL;
    IoDeleteDevice(Device->Control);
    Device->Control = NULL;
}

/* \\.\QcomButtons, the shell waits on it for the keys the kernel does not handle */
static
NTSTATUS
QcBtnCreateControl(
    _Inout_ PQCBTN_DEVICE Device)
{
    UNICODE_STRING Name = RTL_CONSTANT_STRING(L"\\Device\\QcomButtons");
    UNICODE_STRING Link = RTL_CONSTANT_STRING(L"\\DosDevices\\QcomButtons");
    PDEVICE_OBJECT Control;
    NTSTATUS Status;

    Status = IoCreateDevice(Device->Pnp.Self->DriverObject,
                            sizeof(QCBTN_CONTROL),
                            &Name,
                            FILE_DEVICE_UNKNOWN,
                            FILE_DEVICE_SECURE_OPEN,
                            FALSE,
                            &Control);
    if (!NT_SUCCESS(Status))
        return Status;

    Control->Flags |= DO_BUFFERED_IO;
    ((PQCBTN_CONTROL)Control->DeviceExtension)->Device = Device;

    Status = IoCreateSymbolicLink(&Link, &Name);
    if (!NT_SUCCESS(Status))
    {
        IoDeleteDevice(Control);
        return Status;
    }

    Control->Flags &= ~DO_DEVICE_INITIALIZING;
    Device->Control = Control;
    QcBtnControlObject = Control;
    return STATUS_SUCCESS;
}

/* Fails every pending IRP of a list, used when the device goes away */
static
VOID
QcBtnFlush(
    _Inout_ PQCBTN_DEVICE Device,
    _Inout_ PLIST_ENTRY List)
{
    PIRP Irp;
    KIRQL OldIrql;

    for (;;)
    {
        KeAcquireSpinLock(&Device->Lock, &OldIrql);
        Irp = QcBtnTakeWaiter(List);
        KeReleaseSpinLock(&Device->Lock, OldIrql);

        if (!Irp)
            break;
        QcBtnComplete(Irp, STATUS_DEVICE_REMOVED, 0);
    }
}

/* Safe on a half started device, every piece is torn down only if it exists */
static
VOID
QcBtnStop(
    _Inout_ PQCBTN_DEVICE Device)
{
    if (Device->InterfaceRegistered)
    {
        IoSetDeviceInterfaceState(&Device->Interface, FALSE);
        RtlFreeUnicodeString(&Device->Interface);
        Device->InterfaceRegistered = FALSE;
    }

    QcBtnDisconnect(Device);

    if (Device->Thread)
    {
        KeSetEvent(&Device->StopEvent, IO_NO_INCREMENT, FALSE);
        KeWaitForSingleObject(Device->Thread, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(Device->Thread);
        Device->Thread = NULL;
    }

    QcBtnDeleteControl(Device);

    /* Waiters fail so the power manager and the shell drop this device */
    QcBtnFlush(Device, &Device->Waiters);
    QcBtnFlush(Device, &Device->ShellWaiters);
    Device->LineCount = 0;
}

static
NTSTATUS
QcBtnStart(
    _Inout_ PQCBTN_DEVICE Device,
    _In_ PCM_RESOURCE_LIST Translated)
{
    HANDLE Thread;
    NTSTATUS Status;
    ULONG i;

    Device->LineCount = 0;
    Device->Pending = 0;
    Device->Latched = 0;
    Device->QueueHead = 0;
    Device->QueueCount = 0;
    Device->LongPressSent = FALSE;
    KeClearEvent(&Device->StopEvent);
    KeClearEvent(&Device->EdgeEvent);

    Status = QcBtnConnect(Device, Translated);
    if (!NT_SUCCESS(Status))
        return Status;

    /* Without the records the first line is still the power key, as on every ACPI0011 phone */
    if (!NT_SUCCESS(QcBtnReadUsages(Device)))
    {
        QC_LOG("no button records in _DSD, treating the first button as power");
        Device->Lines[0].UsagePage = QCBTN_PAGE_SYSTEM;
        Device->Lines[0].Usage = QCBTN_SYSTEM_POWER;
    }

    for (i = 0; i < Device->LineCount; i++)
    {
        QC_LOG("button %lu is %02x:%02x%s", i, Device->Lines[i].UsagePage, Device->Lines[i].Usage,
               Device->Lines[i].Interrupt ? "" : ", not connected");
    }

    Status = PsCreateSystemThread(&Thread, THREAD_ALL_ACCESS, NULL, NULL, NULL, QcBtnThread, Device);
    if (!NT_SUCCESS(Status))
    {
        QcBtnStop(Device);
        return Status;
    }
    ObReferenceObjectByHandle(Thread, THREAD_ALL_ACCESS, NULL, KernelMode, (PVOID *)&Device->Thread, NULL);
    ZwClose(Thread);

    Status = QcBtnCreateControl(Device);
    if (!NT_SUCCESS(Status))
        QC_LOG("no \\\\.\\QcomButtons for the shell, %08lx", Status);

    /* The power manager finds system buttons through this interface */
    Status = IoRegisterDeviceInterface(Device->Pnp.Pdo, &GUID_DEVICE_SYS_BUTTON, NULL, &Device->Interface);
    if (!NT_SUCCESS(Status))
    {
        QcBtnStop(Device);
        return Status;
    }
    Device->InterfaceRegistered = TRUE;
    IoSetDeviceInterfaceState(&Device->Interface, TRUE);

    QC_LOG("%lu buttons ready", Device->LineCount);
    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
QcBtnDispatchPnp(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PQCBTN_DEVICE Device = DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS Status;

    if (QcBtnIsControl(DeviceObject))
        return QcBtnComplete(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);

    switch (Stack->MinorFunction)
    {
        case IRP_MN_START_DEVICE:
            Status = QcPnpForwardAndWait(&Device->Pnp, Irp);
            if (NT_SUCCESS(Status))
                Status = QcBtnStart(Device, Stack->Parameters.StartDevice.AllocatedResourcesTranslated);
            return QcPnpComplete(Irp, Status);

        /* The interrupts belong to the GPIO controller, a rebalance is refused */
        case IRP_MN_QUERY_STOP_DEVICE:
            return QcPnpComplete(Irp, STATUS_UNSUCCESSFUL);

        case IRP_MN_STOP_DEVICE:
        case IRP_MN_SURPRISE_REMOVAL:
            QcBtnStop(Device);
            Irp->IoStatus.Status = STATUS_SUCCESS;
            return QcPnpPassDown(&Device->Pnp, Irp);

        case IRP_MN_REMOVE_DEVICE:
            QcBtnStop(Device);
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
QcBtnDispatchPower(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    if (QcBtnIsControl(DeviceObject))
    {
        PoStartNextPowerIrp(Irp);
        return QcBtnComplete(Irp, STATUS_SUCCESS, 0);
    }

    return QcPnpDispatchPower(DeviceObject, Irp);
}

static
NTSTATUS
NTAPI
QcBtnAddDevice(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PDEVICE_OBJECT Pdo)
{
    PQCOM_PNP_DEVICE Pnp;
    PQCBTN_DEVICE Device;
    NTSTATUS Status;

    Status = QcPnpCreateDevice(DriverObject, Pdo, sizeof(*Device), FILE_DEVICE_UNKNOWN, &Pnp);
    if (!NT_SUCCESS(Status))
        return Status;

    Pnp->Self->Flags |= DO_BUFFERED_IO;

    Device = CONTAINING_RECORD(Pnp, QCBTN_DEVICE, Pnp);
    KeInitializeEvent(&Device->StopEvent, NotificationEvent, FALSE);
    KeInitializeEvent(&Device->EdgeEvent, SynchronizationEvent, FALSE);
    KeInitializeDpc(&Device->EdgeDpc, QcBtnEdgeDpc, Device);
    KeInitializeSpinLock(&Device->Lock);
    InitializeListHead(&Device->Waiters);
    InitializeListHead(&Device->ShellWaiters);
    return STATUS_SUCCESS;
}

static
VOID
NTAPI
QcBtnUnload(
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

    DriverObject->DriverExtension->AddDevice = QcBtnAddDevice;
    DriverObject->DriverUnload = QcBtnUnload;
    DriverObject->MajorFunction[IRP_MJ_CREATE] = QcBtnCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = QcBtnCreateClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = QcBtnDeviceControl;
    DriverObject->MajorFunction[IRP_MJ_PNP] = QcBtnDispatchPnp;
    DriverObject->MajorFunction[IRP_MJ_POWER] = QcBtnDispatchPower;
    DriverObject->MajorFunction[IRP_MJ_SYSTEM_CONTROL] = QcPnpDispatchPassDown;

    return STATUS_SUCCESS;
}
