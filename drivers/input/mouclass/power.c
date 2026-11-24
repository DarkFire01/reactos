/*
 * PROJECT:     ReactOS Mouse Class Driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Power, wait wake and WMI
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "mouclass.h"
#include <wmidata.h>

#define NDEBUG
#include <debug.h>

static IO_COMPLETION_ROUTINE MousePowerComplete;
static REQUEST_POWER_COMPLETE MouseDevicePowerDone;
static REQUEST_POWER_COMPLETE MouseWaitWakeDone;
static REQUEST_POWER_COMPLETE MouseWakePowerUpDone;
static IO_WORKITEM_ROUTINE MouseWaitWakeWorker;
static IO_WORKITEM_ROUTINE MouseToggleWorker;

static const GUID MouClassInformationGuid = MSMouse_ClassInformationGuid;

WMIGUIDREGINFO MouWmiGuidList[2] =
{
    { &MouClassInformationGuid, 1, 0 },
    { &GUID_POWER_DEVICE_WAKE_ENABLE, 1, 0 }
};

static UNICODE_STRING MouWaitWakeValueName = RTL_CONSTANT_STRING(L"WaitWakeEnabled");

/**
 * @brief
 * The device can wake the system from some sleeping state.
 */
BOOLEAN
NTAPI
MouseCanWake(
    _In_ PMOUCLASS_EXTENSION Extension)
{
    return (Extension->WakeDeviceState > PowerDeviceD0) &&
           (Extension->WakeSystemState > PowerSystemWorking);
}

/**
 * @brief
 * A wait wake IRP is outstanding and nobody is cancelling it.
 */
BOOLEAN
NTAPI
MouseIsWaitWakeArmed(
    _In_ PMOUCLASS_EXTENSION Extension)
{
    BOOLEAN IsArmed;
    KIRQL OldIrql;

    KeAcquireSpinLock(&Extension->WaitWakeLock, &OldIrql);
    IsArmed = (Extension->WaitWakeIrp != NULL) && !Extension->IsWaitWakeCancelling;
    KeReleaseSpinLock(&Extension->WaitWakeLock, OldIrql);

    return IsArmed;
}

BOOLEAN
NTAPI
MouseIsWaitWakeEnabled(
    _In_ PMOUCLASS_EXTENSION Extension)
{
    BOOLEAN IsEnabled;
    KIRQL OldIrql;

    KeAcquireSpinLock(&Extension->WaitWakeLock, &OldIrql);
    IsEnabled = Extension->IsWaitWakeEnabled;
    KeReleaseSpinLock(&Extension->WaitWakeLock, OldIrql);

    return IsEnabled;
}

/**
 * @brief
 * Asks the power manager for a wait wake IRP on the device stack.
 */
VOID
NTAPI
MouseRequestWaitWake(
    _In_ PMOUCLASS_EXTENSION Extension)
{
    POWER_STATE PowerState;

    PowerState.SystemState = Extension->WakeSystemState;
    PoRequestPowerIrp(Extension->Pdo,
                      IRP_MN_WAIT_WAKE,
                      PowerState,
                      MouseWaitWakeDone,
                      Extension,
                      NULL);
}

static
VOID
NTAPI
MouseWaitWakeWorker(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_opt_ PVOID Context)
{
    PMOUCLASS_WORK_CONTEXT WorkContext = Context;
    PMOUCLASS_EXTENSION Extension = WorkContext->Extension;

    UNREFERENCED_PARAMETER(DeviceObject);

    MouseRequestWaitWake(Extension);

    IoReleaseRemoveLock(&Extension->RemoveLock, WorkContext);
    IoFreeWorkItem(WorkContext->WorkItem);
    ExFreePoolWithTag(WorkContext, MOUCLASS_TAG);
}

/**
 * @brief
 * Requests a wait wake IRP from a work item, for callers above passive level.
 */
static
VOID
NTAPI
MouseQueueWaitWakeRequest(
    _In_ PMOUCLASS_EXTENSION Extension,
    _In_ ULONG UniqueId)
{
    PMOUCLASS_WORK_CONTEXT WorkContext;

    WorkContext = ExAllocatePoolZero(NonPagedPool, sizeof(*WorkContext), MOUCLASS_TAG);
    if (WorkContext != NULL)
    {
        WorkContext->WorkItem = IoAllocateWorkItem(Extension->Self);
        if (WorkContext->WorkItem != NULL)
        {
            WorkContext->Extension = Extension;
            if (!NT_SUCCESS(IoAcquireRemoveLock(&Extension->RemoveLock, WorkContext)))
            {
                IoFreeWorkItem(WorkContext->WorkItem);
                ExFreePoolWithTag(WorkContext, MOUCLASS_TAG);
                return;
            }

            IoQueueWorkItem(WorkContext->WorkItem,
                            MouseWaitWakeWorker,
                            DelayedWorkQueue,
                            WorkContext);
            return;
        }

        ExFreePoolWithTag(WorkContext, MOUCLASS_TAG);
    }

    MouseLogError(Extension->Self, MOUCLASS_EVT_WAIT_WAKE_FAILED, UniqueId,
                  STATUS_INSUFFICIENT_RESOURCES, 0, NULL, 0);
}

static
VOID
NTAPI
MouseToggleWorker(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_opt_ PVOID Context)
{
    PMOUCLASS_WORK_CONTEXT WorkContext = Context;
    PMOUCLASS_EXTENSION Extension = WorkContext->Extension;
    BOOLEAN Enable = WorkContext->Enable;
    BOOLEAN IsChanged = FALSE;
    PIRP CancelIrp = NULL;
    HANDLE Key;
    ULONG Value;
    KIRQL OldIrql;

    UNREFERENCED_PARAMETER(DeviceObject);

    KeAcquireSpinLock(&Extension->WaitWakeLock, &OldIrql);
    if (Enable != Extension->IsWaitWakeEnabled)
    {
        IsChanged = TRUE;

        /* Disarm an outstanding wait wake when turning wake off */
        if (Extension->IsWaitWakeEnabled &&
            Extension->WaitWakeIrp != NULL &&
            !Extension->IsWaitWakeCancelling)
        {
            Extension->IsWaitWakeCancelling = TRUE;
            CancelIrp = Extension->WaitWakeIrp;
        }

        Extension->IsWaitWakeEnabled = Enable;
    }
    KeReleaseSpinLock(&Extension->WaitWakeLock, OldIrql);

    if (IsChanged)
    {
        if (NT_SUCCESS(IoOpenDeviceRegistryKey(Extension->Pdo,
                                               PLUGPLAY_REGKEY_DEVICE,
                                               KEY_SET_VALUE,
                                               &Key)))
        {
            Value = Enable;
            ZwSetValueKey(Key, &MouWaitWakeValueName, 0, REG_DWORD, &Value, sizeof(Value));
            ZwClose(Key);
        }

        if (Enable)
            MouseRequestWaitWake(Extension);
    }

    if (CancelIrp != NULL)
        IoCancelIrp(CancelIrp);

    IoReleaseRemoveLock(&Extension->RemoveLock, &Extension->IsWaitWakeEnabled);
    IoFreeWorkItem(WorkContext->WorkItem);
    ExFreePoolWithTag(WorkContext, MOUCLASS_TAG);
}

/**
 * @brief
 * Turns wake from the device on or off and records the choice in the device key.
 */
static
NTSTATUS
NTAPI
MouseToggleWaitWake(
    _In_ PMOUCLASS_EXTENSION Extension,
    _In_ BOOLEAN Enable)
{
    PMOUCLASS_WORK_CONTEXT WorkContext;
    NTSTATUS Status;

    Status = IoAcquireRemoveLock(&Extension->RemoveLock, &Extension->IsWaitWakeEnabled);
    if (!NT_SUCCESS(Status))
        return Status;

    WorkContext = ExAllocatePoolZero(NonPagedPool, sizeof(*WorkContext), MOUCLASS_TAG);
    if (WorkContext == NULL)
    {
        IoReleaseRemoveLock(&Extension->RemoveLock, &Extension->IsWaitWakeEnabled);
        return STATUS_SUCCESS;
    }

    WorkContext->WorkItem = IoAllocateWorkItem(Extension->Self);
    if (WorkContext->WorkItem == NULL)
    {
        IoReleaseRemoveLock(&Extension->RemoveLock, &Extension->IsWaitWakeEnabled);
        ExFreePoolWithTag(WorkContext, MOUCLASS_TAG);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    WorkContext->Extension = Extension;
    WorkContext->Enable = Enable;

    if (KeGetCurrentIrql() > PASSIVE_LEVEL)
        IoQueueWorkItem(WorkContext->WorkItem, MouseToggleWorker, DelayedWorkQueue, WorkContext);
    else
        MouseToggleWorker(Extension->Self, WorkContext);

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Loads the wake setting from the device key, or picks the default.
 *
 * Without a stored value wake is turned on for workstations when the device
 * can wake the system from the deepest sleeping state the machine supports.
 */
VOID
NTAPI
MouseReadWaitWakeSetting(
    _In_ PMOUCLASS_EXTENSION Extension)
{
    UCHAR Buffer[FIELD_OFFSET(KEY_VALUE_PARTIAL_INFORMATION, Data) + sizeof(ULONG)];
    PKEY_VALUE_PARTIAL_INFORMATION Info = (PKEY_VALUE_PARTIAL_INFORMATION)Buffer;
    SYSTEM_POWER_CAPABILITIES PowerCapabilities;
    RTL_OSVERSIONINFOEXW Version;
    SYSTEM_POWER_STATE Deepest;
    ULONGLONG ConditionMask;
    ULONG ResultLength;
    ULONG Value = 0;
    HANDLE Key;
    NTSTATUS Status;

    if (NT_SUCCESS(IoOpenDeviceRegistryKey(Extension->Pdo,
                                           PLUGPLAY_REGKEY_DEVICE,
                                           KEY_QUERY_VALUE,
                                           &Key)))
    {
        Status = ZwQueryValueKey(Key,
                                 &MouWaitWakeValueName,
                                 KeyValuePartialInformation,
                                 Info,
                                 sizeof(Buffer),
                                 &ResultLength);
        ZwClose(Key);

        if (NT_SUCCESS(Status) && Info->DataLength <= sizeof(Value))
        {
            RtlCopyMemory(&Value, Info->Data, Info->DataLength);
            Extension->IsWaitWakeEnabled = (Value != 0);
            return;
        }
    }

    if (Extension->IsWaitWakeEnabled)
        return;

    RtlZeroMemory(&Version, sizeof(Version));
    Version.dwOSVersionInfoSize = sizeof(Version);
    Version.wProductType = VER_NT_WORKSTATION;
    ConditionMask = VerSetConditionMask(0, VER_PRODUCT_TYPE, VER_EQUAL);
    if (!NT_SUCCESS(RtlVerifyVersionInfo(&Version, VER_PRODUCT_TYPE, ConditionMask)))
    {
        return;
    }

    RtlZeroMemory(&PowerCapabilities, sizeof(PowerCapabilities));
    Status = ZwPowerInformation(SystemPowerCapabilities,
                                NULL,
                                0,
                                &PowerCapabilities,
                                sizeof(PowerCapabilities));
    if (!NT_SUCCESS(Status))
        return;

    if (PowerCapabilities.SystemS3)
        Deepest = PowerSystemSleeping3;
    else if (PowerCapabilities.SystemS2)
        Deepest = PowerSystemSleeping2;
    else if (PowerCapabilities.SystemS1)
        Deepest = PowerSystemSleeping1;
    else
        Deepest = PowerSystemUnspecified;

    if (Extension->WakeSystemState >= Deepest)
        Extension->IsWaitWakeEnabled = TRUE;
}

/**
 * @brief
 * Cancels the wait wake IRP while the device is being removed.
 */
VOID
NTAPI
MouseCancelWaitWakeForRemoval(
    _In_ PMOUCLASS_EXTENSION Extension)
{
    KIRQL OldIrql;
    LONG Previous;
    PIRP Irp;

    if (InterlockedExchange(&Extension->WaitWakeRemoval, MOUCLASS_WW_REMOVING) != MOUCLASS_WW_IDLE)
        return;

    KeAcquireSpinLock(&Extension->WaitWakeLock, &OldIrql);
    Irp = Extension->WaitWakeIrp;
    if (Irp == NULL || Extension->IsWaitWakeCancelling)
    {
        KeReleaseSpinLock(&Extension->WaitWakeLock, OldIrql);
        InterlockedExchange(&Extension->WaitWakeRemoval, MOUCLASS_WW_REMOVED);
        return;
    }

    Extension->IsWaitWakeCancelling = TRUE;
    KeReleaseSpinLock(&Extension->WaitWakeLock, OldIrql);

    IoCancelIrp(Irp);

    /* The completion routine already ran and left the IRP to us */
    Previous = InterlockedExchange(&Extension->WaitWakeRemoval, MOUCLASS_WW_REMOVED);
    if (Previous == MOUCLASS_WW_COMPLETED)
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
}

static
VOID
NTAPI
MouseWakePowerUpDone(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ UCHAR MinorFunction,
    _In_ POWER_STATE PowerState,
    _In_opt_ PVOID Context,
    _In_ PIO_STATUS_BLOCK IoStatus)
{
    PMOUCLASS_EXTENSION Extension = Context;

    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(MinorFunction);
    UNREFERENCED_PARAMETER(PowerState);
    UNREFERENCED_PARAMETER(IoStatus);

    /* Re-arm for the next wake */
    if (Extension->IsWaitWakeEnabled)
        MouseQueueWaitWakeRequest(Extension, 1);
}

static
VOID
NTAPI
MouseWaitWakeDone(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ UCHAR MinorFunction,
    _In_ POWER_STATE PowerState,
    _In_opt_ PVOID Context,
    _In_ PIO_STATUS_BLOCK IoStatus)
{
    PMOUCLASS_EXTENSION Extension = Context;
    POWER_STATE DeviceOn;
    NTSTATUS Status;
    KIRQL OldIrql;
    PIRP Irp = NULL;

    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(MinorFunction);
    UNREFERENCED_PARAMETER(PowerState);

    KeAcquireSpinLock(&Extension->WaitWakeLock, &OldIrql);
    if (Extension->ExtraWaitWakeIrp != NULL &&
        IoStatus == &Extension->ExtraWaitWakeIrp->IoStatus)
    {
        Irp = Extension->ExtraWaitWakeIrp;
        Extension->ExtraWaitWakeIrp = NULL;
    }
    else if (Extension->WaitWakeIrp != NULL &&
             IoStatus == &Extension->WaitWakeIrp->IoStatus)
    {
        Irp = Extension->WaitWakeIrp;
        Extension->WaitWakeIrp = NULL;
        Extension->IsWaitWakeCancelling = FALSE;
    }
    KeReleaseSpinLock(&Extension->WaitWakeLock, OldIrql);

    Status = IoStatus->Status;
    if (Status == STATUS_SUCCESS)
    {
        DeviceOn.DeviceState = PowerDeviceD0;
        PoRequestPowerIrp(Extension->Pdo,
                          IRP_MN_SET_POWER,
                          DeviceOn,
                          MouseWakePowerUpDone,
                          Extension,
                          NULL);
    }
    else if (Status != STATUS_DEVICE_BUSY &&
             Status != STATUS_CANCELLED &&
             Status != STATUS_INVALID_DEVICE_STATE &&
             Status != STATUS_POWER_STATE_INVALID &&
             Status != STATUS_ACPI_POWER_REQUEST_FAILED)
    {
        /* The stack cannot wake us, stop asking */
        DPRINT1("Wait wake failed, 0x%08lx\n", Status);
        MouseToggleWaitWake(Extension, FALSE);
    }

    /* Each wait wake IRP that went through MousePower holds one reference */
    if (Irp != NULL)
        IoReleaseRemoveLock(&Extension->RemoveLock, Irp);
}

static
VOID
NTAPI
MouseDevicePowerDone(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ UCHAR MinorFunction,
    _In_ POWER_STATE PowerState,
    _In_opt_ PVOID Context,
    _In_ PIO_STATUS_BLOCK IoStatus)
{
    PMOUCLASS_EXTENSION Extension = DeviceObject->DeviceExtension;
    PIRP SystemIrp = Context;
    POWER_STATE SystemState;

    UNREFERENCED_PARAMETER(MinorFunction);
    UNREFERENCED_PARAMETER(PowerState);
    UNREFERENCED_PARAMETER(IoStatus);

    if (SystemIrp != NULL)
    {
        /* The device IRP for a sleep transition finished, now pass the system IRP on */
        SystemState = IoGetCurrentIrpStackLocation(SystemIrp)->Parameters.Power.State;
        PoSetPowerState(Extension->Self, SystemPowerState, SystemState);
        Extension->SystemState = SystemState.SystemState;

        PoStartNextPowerIrp(SystemIrp);
        IoSkipCurrentIrpStackLocation(SystemIrp);
        PoCallDriver(Extension->LowerDevice, SystemIrp);
        IoReleaseRemoveLock(&Extension->RemoveLock, SystemIrp);
        return;
    }

    /* Back in D0 after waking, arm wake again if nothing is armed */
    if (!MouseCanWake(Extension) || MouseIsWaitWakeArmed(Extension))
        return;

    if (MouseIsWaitWakeEnabled(Extension))
        MouseQueueWaitWakeRequest(Extension, 2);
}

static
NTSTATUS
NTAPI
MousePowerComplete(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_opt_ PVOID Context)
{
    PMOUCLASS_EXTENSION Extension = DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    POWER_STATE State = Stack->Parameters.Power.State;
    POWER_STATE DeviceOn;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(Context);

    if (Stack->MinorFunction == IRP_MN_WAIT_WAKE)
    {
        /* Removal is cancelling this IRP and completes it once it sees the handshake */
        if (Irp == Extension->WaitWakeIrp &&
            InterlockedExchange(&Extension->WaitWakeRemoval,
                                MOUCLASS_WW_COMPLETED) == MOUCLASS_WW_REMOVING)
        {
            PoStartNextPowerIrp(Irp);
            return STATUS_MORE_PROCESSING_REQUIRED;
        }

        PoStartNextPowerIrp(Irp);
        return STATUS_SUCCESS;
    }

    if (Stack->MinorFunction == IRP_MN_SET_POWER)
    {
        if (Stack->Parameters.Power.Type == DevicePowerState)
        {
            Extension->DeviceState = State.DeviceState;
            PoSetPowerState(Extension->Self, DevicePowerState, State);
        }
        else if (Stack->Parameters.Power.Type == SystemPowerState)
        {
            PoSetPowerState(Extension->Self, SystemPowerState, State);
            Extension->SystemState = State.SystemState;

            DeviceOn.DeviceState = PowerDeviceD0;
            Status = PoRequestPowerIrp(Extension->Self,
                                       IRP_MN_SET_POWER,
                                       DeviceOn,
                                       MouseDevicePowerDone,
                                       NULL,
                                       NULL);
            if (!NT_SUCCESS(Status))
                Irp->IoStatus.Status = Status;
        }
    }

    PoStartNextPowerIrp(Irp);
    IoReleaseRemoveLock(&Extension->RemoveLock, Irp);
    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
MousePassPower(
    _In_ PMOUCLASS_EXTENSION Extension,
    _Inout_ PIRP Irp)
{
    PoStartNextPowerIrp(Irp);
    IoSkipCurrentIrpStackLocation(Irp);
    return PoCallDriver(Extension->LowerDevice, Irp);
}

/**
 * @brief
 * Sends a power IRP down with MousePowerComplete, which drops the extra reference.
 */
static
NTSTATUS
NTAPI
MouseSendPowerAndWatch(
    _In_ PMOUCLASS_EXTENSION Extension,
    _Inout_ PIRP Irp)
{
    IoAcquireRemoveLock(&Extension->RemoveLock, Irp);
    IoCopyCurrentIrpStackLocationToNext(Irp);
    IoSetCompletionRoutine(Irp, MousePowerComplete, NULL, TRUE, TRUE, TRUE);
    IoMarkIrpPending(Irp);
    PoCallDriver(Extension->LowerDevice, Irp);
    return STATUS_PENDING;
}

static
NTSTATUS
NTAPI
MouseFailPower(
    _Inout_ PIRP Irp,
    _In_ NTSTATUS Status)
{
    Irp->IoStatus.Status = Status;
    PoStartNextPowerIrp(Irp);
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

/**
 * @brief
 * Moves the device to its sleep state before the system IRP goes down.
 */
static
NTSTATUS
NTAPI
MouseSystemSleep(
    _In_ PMOUCLASS_EXTENSION Extension,
    _Inout_ PIRP Irp,
    _In_ SYSTEM_POWER_STATE SystemState)
{
    POWER_STATE DeviceTarget;
    NTSTATUS Status;

    Status = IoAcquireRemoveLock(&Extension->RemoveLock, Irp);
    if (!NT_SUCCESS(Status))
    {
        MouseFailPower(Irp, Status);
        IoReleaseRemoveLock(&Extension->RemoveLock, Irp);
        return Status;
    }

    /* An armed device keeps the state that lets it wake, otherwise it goes to D3 */
    if (MouseIsWaitWakeArmed(Extension) && SystemState < PowerSystemHibernate)
        DeviceTarget.DeviceState = Extension->SystemToDevice[SystemState];
    else
        DeviceTarget.DeviceState = PowerDeviceD3;

    IoMarkIrpPending(Irp);
    Status = PoRequestPowerIrp(Extension->Self,
                               IRP_MN_SET_POWER,
                               DeviceTarget,
                               MouseDevicePowerDone,
                               Irp,
                               NULL);
    if (!NT_SUCCESS(Status))
    {
        IoReleaseRemoveLock(&Extension->RemoveLock, Irp);
        MouseFailPower(Irp, Status);
    }

    IoReleaseRemoveLock(&Extension->RemoveLock, Irp);
    return STATUS_PENDING;
}

NTSTATUS
NTAPI
MousePower(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PMOUCLASS_EXTENSION Extension = DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    POWER_STATE_TYPE Type = Stack->Parameters.Power.Type;
    POWER_STATE State = Stack->Parameters.Power.State;
    BOOLEAN IsPassedDown;
    LONG Previous;
    KIRQL OldIrql;
    NTSTATUS Status;

    if (Extension == MouGlobals.SharedClass || !Extension->IsPnp)
        return MouseFailPower(Irp, STATUS_NOT_SUPPORTED);

    Status = IoAcquireRemoveLock(&Extension->RemoveLock, Irp);
    if (!NT_SUCCESS(Status))
        return MouseFailPower(Irp, Status);

    switch (Stack->MinorFunction)
    {
        case IRP_MN_SET_POWER:
        {
            if (Type == SystemPowerState)
            {
                if (Extension->SystemState < State.SystemState)
                    return MouseSystemSleep(Extension, Irp, State.SystemState);

                Irp->IoStatus.Status = STATUS_SUCCESS;
                if (Extension->SystemState > State.SystemState)
                {
                    Status = MouseSendPowerAndWatch(Extension, Irp);
                    break;
                }

                if (State.SystemState == PowerSystemWorking &&
                    MouseCanWake(Extension) &&
                    !MouseIsWaitWakeArmed(Extension) &&
                    MouseIsWaitWakeEnabled(Extension))
                {
                    MouseRequestWaitWake(Extension);
                }
            }
            else if (Type == DevicePowerState)
            {
                Irp->IoStatus.Status = STATUS_SUCCESS;
                if (Extension->DeviceState > State.DeviceState)
                {
                    Status = MouseSendPowerAndWatch(Extension, Irp);
                    break;
                }

                if (Extension->DeviceState < State.DeviceState)
                {
                    PoSetPowerState(Extension->Self, DevicePowerState, State);
                    Extension->DeviceState = State.DeviceState;
                }
            }

            Status = MousePassPower(Extension, Irp);
            break;
        }

        case IRP_MN_WAIT_WAKE:
        {
            KeAcquireSpinLock(&Extension->WaitWakeLock, &OldIrql);
            if (Extension->WaitWakeIrp != NULL)
            {
                Extension->ExtraWaitWakeIrp = Irp;
                IsPassedDown = FALSE;
            }
            else
            {
                Extension->WaitWakeIrp = Irp;
                Extension->IsWaitWakeCancelling = FALSE;
                Previous = InterlockedExchange(&Extension->WaitWakeRemoval, MOUCLASS_WW_IDLE);
                IsPassedDown = (Previous != MOUCLASS_WW_REMOVING &&
                                Previous != MOUCLASS_WW_REMOVED);
            }
            KeReleaseSpinLock(&Extension->WaitWakeLock, OldIrql);

            /* The reference taken above is dropped by MouseWaitWakeDone */
            if (!IsPassedDown)
                return MouseFailPower(Irp, STATUS_INVALID_DEVICE_STATE);

            Status = MouseSendPowerAndWatch(Extension, Irp);
            break;
        }

        case IRP_MN_QUERY_POWER:
        {
            /* Refuse a sleep state the armed device could not wake the system from */
            if (State.SystemState < PowerSystemHibernate &&
                State.SystemState > Extension->WakeSystemState &&
                MouseIsWaitWakeArmed(Extension))
            {
                Status = MouseFailPower(Irp, STATUS_POWER_STATE_INVALID);
                break;
            }

            Irp->IoStatus.Status = STATUS_SUCCESS;
            Status = MousePassPower(Extension, Irp);
            break;
        }

        default:
            Status = MousePassPower(Extension, Irp);
            break;
    }

    IoReleaseRemoveLock(&Extension->RemoveLock, Irp);
    return Status;
}

static
NTSTATUS
NTAPI
MouseQueryWmiRegInfo(
    _Inout_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PULONG RegFlags,
    _Inout_ PUNICODE_STRING InstanceName,
    _Outptr_result_maybenull_ PUNICODE_STRING *RegistryPath,
    _Inout_ PUNICODE_STRING MofResourceName,
    _Outptr_result_maybenull_ PDEVICE_OBJECT *Pdo)
{
    PMOUCLASS_EXTENSION Extension = DeviceObject->DeviceExtension;

    UNREFERENCED_PARAMETER(InstanceName);
    UNREFERENCED_PARAMETER(MofResourceName);

    *RegFlags = WMIREG_FLAG_INSTANCE_PDO;
    *RegistryPath = &MouGlobals.RegistryPath;
    *Pdo = Extension->Pdo;
    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
MouseQueryWmiDataBlock(
    _Inout_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp,
    _In_ ULONG GuidIndex,
    _In_ ULONG InstanceIndex,
    _In_ ULONG InstanceCount,
    _Out_writes_opt_(InstanceCount) PULONG InstanceLengthArray,
    _In_ ULONG BufferAvail,
    _Out_writes_bytes_opt_(BufferAvail) PUCHAR Buffer)
{
    PMOUCLASS_EXTENSION Extension = DeviceObject->DeviceExtension;
    ULONG Size = 0;
    NTSTATUS Status;

    switch (GuidIndex)
    {
        case MOUCLASS_WMI_CLASS_INFO:
            if (InstanceIndex != 0 || InstanceCount != 1)
            {
                Status = STATUS_INVALID_DEVICE_REQUEST;
                break;
            }

            /* MSMouse_ClassInformation.DeviceId identifies the class device */
            Size = sizeof(ULONGLONG);
            if (BufferAvail < Size)
            {
                Status = STATUS_BUFFER_TOO_SMALL;
                break;
            }

            *(PULONGLONG)Buffer = (ULONGLONG)(ULONG_PTR)DeviceObject;
            *InstanceLengthArray = Size;
            Status = STATUS_SUCCESS;
            break;

        case MOUCLASS_WMI_WAKE_ENABLE:
            if (InstanceIndex != 0 || InstanceCount != 1)
            {
                Status = STATUS_INVALID_DEVICE_REQUEST;
                break;
            }

            Size = sizeof(BOOLEAN);
            if (BufferAvail < Size)
            {
                Status = STATUS_BUFFER_TOO_SMALL;
                break;
            }

            *(PBOOLEAN)Buffer = MouseIsWaitWakeArmed(Extension);
            *InstanceLengthArray = Size;
            Status = STATUS_SUCCESS;
            break;

        default:
            Status = STATUS_WMI_GUID_NOT_FOUND;
            break;
    }

    return WmiCompleteRequest(DeviceObject, Irp, Status, Size, IO_NO_INCREMENT);
}

static
NTSTATUS
NTAPI
MouseSetWmiDataBlock(
    _Inout_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp,
    _In_ ULONG GuidIndex,
    _In_ ULONG InstanceIndex,
    _In_ ULONG BufferSize,
    _In_reads_bytes_(BufferSize) PUCHAR Buffer)
{
    ULONG Size = 0;
    NTSTATUS Status;

    switch (GuidIndex)
    {
        case MOUCLASS_WMI_CLASS_INFO:
            Status = STATUS_WMI_READ_ONLY;
            break;

        case MOUCLASS_WMI_WAKE_ENABLE:
            Size = sizeof(BOOLEAN);
            if (BufferSize == 0)
                Status = STATUS_BUFFER_TOO_SMALL;
            else if (InstanceIndex != 0)
                Status = STATUS_INVALID_DEVICE_REQUEST;
            else
                Status = MouseToggleWaitWake(DeviceObject->DeviceExtension, *Buffer != 0);
            break;

        default:
            Status = STATUS_WMI_GUID_NOT_FOUND;
            break;
    }

    return WmiCompleteRequest(DeviceObject, Irp, Status, Size, IO_NO_INCREMENT);
}

static
NTSTATUS
NTAPI
MouseSetWmiDataItem(
    _Inout_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp,
    _In_ ULONG GuidIndex,
    _In_ ULONG InstanceIndex,
    _In_ ULONG DataItemId,
    _In_ ULONG BufferSize,
    _In_reads_bytes_(BufferSize) PUCHAR Buffer)
{
    NTSTATUS Status;

    switch (GuidIndex)
    {
        case MOUCLASS_WMI_CLASS_INFO:
            Status = STATUS_WMI_READ_ONLY;
            break;

        case MOUCLASS_WMI_WAKE_ENABLE:
            if (BufferSize == 0)
                Status = STATUS_BUFFER_TOO_SMALL;
            else if (DataItemId != 1 || InstanceIndex != 0)
                Status = STATUS_INVALID_DEVICE_REQUEST;
            else
                Status = MouseToggleWaitWake(DeviceObject->DeviceExtension, *Buffer != 0);
            break;

        default:
            Status = STATUS_WMI_GUID_NOT_FOUND;
            break;
    }

    return WmiCompleteRequest(DeviceObject, Irp, Status, 0, IO_NO_INCREMENT);
}

/**
 * @brief
 * Fills the WMI library context and registers the device with WMI.
 */
VOID
NTAPI
MouseInitializeWmi(
    _In_ PMOUCLASS_EXTENSION Extension)
{
    RtlZeroMemory(&Extension->WmiLib, sizeof(Extension->WmiLib));

    /* The wake enable block only exists for devices that can wake the system */
    if (MouseCanWake(Extension))
    {
        Extension->WmiLib.GuidCount = RTL_NUMBER_OF(MouWmiGuidList);
        MouseReadWaitWakeSetting(Extension);
    }
    else
    {
        Extension->WmiLib.GuidCount = 1;
    }

    Extension->WmiLib.GuidList = MouWmiGuidList;
    Extension->WmiLib.QueryWmiRegInfo = MouseQueryWmiRegInfo;
    Extension->WmiLib.QueryWmiDataBlock = MouseQueryWmiDataBlock;
    Extension->WmiLib.SetWmiDataBlock = MouseSetWmiDataBlock;
    Extension->WmiLib.SetWmiDataItem = MouseSetWmiDataItem;

    IoWMIRegistrationControl(Extension->Self, WMIREG_ACTION_REGISTER);
}

NTSTATUS
NTAPI
MouseSystemControl(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PMOUCLASS_EXTENSION Extension = DeviceObject->DeviceExtension;
    SYSCTL_IRP_DISPOSITION Disposition = IrpProcessed;
    NTSTATUS Status;

    Status = IoAcquireRemoveLock(&Extension->RemoveLock, Irp);
    if (!NT_SUCCESS(Status))
    {
        Irp->IoStatus.Status = Status;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return Status;
    }

    Status = WmiSystemControl(&Extension->WmiLib, DeviceObject, Irp, &Disposition);
    switch (Disposition)
    {
        case IrpProcessed:
            break;

        case IrpNotCompleted:
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            break;

        default:
            Status = MousePassThrough(DeviceObject, Irp);
            break;
    }

    IoReleaseRemoveLock(&Extension->RemoveLock, Irp);
    return Status;
}
