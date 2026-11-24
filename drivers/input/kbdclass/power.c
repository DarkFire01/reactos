/*
 * PROJECT:     ReactOS Keyboard Class Driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Power and wait wake handling
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "kbdclass.h"

#define NDEBUG
#include <debug.h>

#ifndef STATUS_ACPI_POWER_REQUEST_FAILED
#define STATUS_ACPI_POWER_REQUEST_FAILED ((NTSTATUS)0xC0140021L)
#endif

static IO_COMPLETION_ROUTINE KbdClassPowerComplete;
static REQUEST_POWER_COMPLETE KbdClassSleepPowerComplete;
static REQUEST_POWER_COMPLETE KbdClassResumePowerComplete;
static REQUEST_POWER_COMPLETE KbdClassWaitWakeComplete;
static REQUEST_POWER_COMPLETE KbdClassWakeUpPowerComplete;
static IO_WORKITEM_ROUTINE KbdClassArmWaitWakeWorker;
static IO_WORKITEM_ROUTINE KbdClassToggleWaitWakeWorker;

/* WAIT WAKE STATE ************************************************************/

BOOLEAN
NTAPI
KbdClassWakeCapable(
    _In_ PKBDCLASS_DEVICE_EXTENSION DeviceExtension)
{
    return DeviceExtension->MinDeviceWake > PowerDeviceD0 &&
           DeviceExtension->MinSystemWake > PowerSystemWorking;
}

BOOLEAN
NTAPI
KbdClassWaitWakePending(
    _In_ PKBDCLASS_DEVICE_EXTENSION DeviceExtension)
{
    BOOLEAN Pending;
    KIRQL OldIrql;

    KeAcquireSpinLock(&DeviceExtension->WaitWakeLock, &OldIrql);
    Pending = DeviceExtension->WaitWakeIrp && !DeviceExtension->WaitWakeCancelled;
    KeReleaseSpinLock(&DeviceExtension->WaitWakeLock, OldIrql);

    return Pending;
}

BOOLEAN
NTAPI
KbdClassCheckWaitWakeEnabled(
    _In_ PKBDCLASS_DEVICE_EXTENSION DeviceExtension)
{
    BOOLEAN Enabled;
    KIRQL OldIrql;

    KeAcquireSpinLock(&DeviceExtension->WaitWakeLock, &OldIrql);
    Enabled = DeviceExtension->WaitWakeEnabled;
    KeReleaseSpinLock(&DeviceExtension->WaitWakeLock, OldIrql);

    return Enabled;
}

/**
 * @brief Loads WaitWakeEnabled from the device key, or picks a default.
 *
 * Without a stored value wake is enabled on workstations when the device can
 * wake the system from the deepest sleep state the machine supports.
 */
VOID
NTAPI
KbdClassReadWaitWakeState(
    _In_ PKBDCLASS_DEVICE_EXTENSION DeviceExtension)
{
    SYSTEM_POWER_CAPABILITIES PowerCapabilities;
    RTL_OSVERSIONINFOEXW VersionInfo;
    SYSTEM_POWER_STATE Deepest;
    ULONGLONG ConditionMask;
    HANDLE KeyHandle;
    ULONG Value;
    NTSTATUS Status;

    Status = IoOpenDeviceRegistryKey(DeviceExtension->Pdo,
                                     PLUGPLAY_REGKEY_DEVICE,
                                     KEY_READ,
                                     &KeyHandle);
    if (NT_SUCCESS(Status))
    {
        Status = KbdClassQueryRegistryUlong(KeyHandle, L"WaitWakeEnabled", &Value);
        ZwClose(KeyHandle);

        if (NT_SUCCESS(Status))
        {
            DeviceExtension->WaitWakeEnabled = (Value != 0);
            return;
        }
    }

    if (DeviceExtension->WaitWakeEnabled)
        return;

    RtlZeroMemory(&VersionInfo, sizeof(VersionInfo));
    VersionInfo.dwOSVersionInfoSize = sizeof(VersionInfo);
    VersionInfo.wProductType = VER_NT_WORKSTATION;

    ConditionMask = VerSetConditionMask(0, VER_PRODUCT_TYPE, VER_EQUAL);
    if (!NT_SUCCESS(RtlVerifyVersionInfo(&VersionInfo, VER_PRODUCT_TYPE, ConditionMask)))
        return;

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

    if (DeviceExtension->MinSystemWake >= Deepest)
        DeviceExtension->WaitWakeEnabled = TRUE;
}

/**
 * @brief Asks the stack for an IRP_MN_WAIT_WAKE.
 */
BOOLEAN
NTAPI
KbdClassCreateWaitWakeIrp(
    _In_ PKBDCLASS_DEVICE_EXTENSION DeviceExtension)
{
    POWER_STATE PowerState;

    PowerState.SystemState = DeviceExtension->MinSystemWake;

    return PoRequestPowerIrp(DeviceExtension->Pdo,
                             IRP_MN_WAIT_WAKE,
                             PowerState,
                             KbdClassWaitWakeComplete,
                             DeviceExtension,
                             NULL) == STATUS_PENDING;
}

static
VOID
NTAPI
KbdClassArmWaitWakeWorker(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_opt_ PVOID Context)
{
    PKBDCLASS_WORK_CONTEXT WorkContext = Context;
    PKBDCLASS_DEVICE_EXTENSION DeviceExtension = WorkContext->DeviceExtension;

    UNREFERENCED_PARAMETER(DeviceObject);

    KbdClassCreateWaitWakeIrp(DeviceExtension);

    IoReleaseRemoveLock(&DeviceExtension->RemoveLock, WorkContext);
    IoFreeWorkItem(WorkContext->WorkItem);
    ExFreePoolWithTag(WorkContext, KBDCLASS_TAG);
}

/**
 * @brief Requests the wait wake IRP from a work item.
 */
static
NTSTATUS
KbdClassQueueWaitWakeArm(
    _In_ PKBDCLASS_DEVICE_EXTENSION DeviceExtension)
{
    PKBDCLASS_WORK_CONTEXT WorkContext;
    NTSTATUS Status;

    WorkContext = ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*WorkContext), KBDCLASS_TAG);
    if (!WorkContext)
        return STATUS_INSUFFICIENT_RESOURCES;

    WorkContext->WorkItem = IoAllocateWorkItem(DeviceExtension->Self);
    if (!WorkContext->WorkItem)
    {
        ExFreePoolWithTag(WorkContext, KBDCLASS_TAG);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    WorkContext->DeviceExtension = DeviceExtension;

    Status = IoAcquireRemoveLock(&DeviceExtension->RemoveLock, WorkContext);
    if (!NT_SUCCESS(Status))
    {
        IoFreeWorkItem(WorkContext->WorkItem);
        ExFreePoolWithTag(WorkContext, KBDCLASS_TAG);
        return Status;
    }

    IoQueueWorkItem(WorkContext->WorkItem, KbdClassArmWaitWakeWorker, DelayedWorkQueue, WorkContext);
    return STATUS_SUCCESS;
}

static
VOID
NTAPI
KbdClassWakeUpPowerComplete(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ UCHAR MinorFunction,
    _In_ POWER_STATE PowerState,
    _In_opt_ PVOID Context,
    _In_ PIO_STATUS_BLOCK IoStatus)
{
    PKBDCLASS_DEVICE_EXTENSION DeviceExtension = Context;

    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(MinorFunction);
    UNREFERENCED_PARAMETER(PowerState);
    UNREFERENCED_PARAMETER(IoStatus);

    if (!DeviceExtension->WaitWakeEnabled)
        return;

    if (KbdClassQueueWaitWakeArm(DeviceExtension) == STATUS_INSUFFICIENT_RESOURCES)
    {
        KbdClassLogError(DeviceExtension->Self,
                         KBDCLASS_LOG_WAIT_WAKE_RESOURCES,
                         2,
                         STATUS_INSUFFICIENT_RESOURCES,
                         0,
                         NULL,
                         0);
    }
}

/**
 * @brief Completion of a wait wake IRP the driver asked for.
 *
 * Drops the remove lock the dispatch routine left held for the IRP.
 */
static
VOID
NTAPI
KbdClassWaitWakeComplete(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ UCHAR MinorFunction,
    _In_ POWER_STATE PowerState,
    _In_opt_ PVOID Context,
    _In_ PIO_STATUS_BLOCK IoStatus)
{
    PKBDCLASS_DEVICE_EXTENSION DeviceExtension = Context;
    POWER_STATE DeviceState;
    PIRP Irp = NULL;
    KIRQL OldIrql;

    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(MinorFunction);
    UNREFERENCED_PARAMETER(PowerState);

    KeAcquireSpinLock(&DeviceExtension->WaitWakeLock, &OldIrql);

    if (DeviceExtension->ExtraWaitWakeIrp &&
        IoStatus == &DeviceExtension->ExtraWaitWakeIrp->IoStatus)
    {
        Irp = DeviceExtension->ExtraWaitWakeIrp;
        DeviceExtension->ExtraWaitWakeIrp = NULL;
    }
    else if (DeviceExtension->WaitWakeIrp &&
             IoStatus == &DeviceExtension->WaitWakeIrp->IoStatus)
    {
        Irp = DeviceExtension->WaitWakeIrp;
        DeviceExtension->WaitWakeIrp = NULL;
        DeviceExtension->WaitWakeCancelled = FALSE;
    }

    KeReleaseSpinLock(&DeviceExtension->WaitWakeLock, OldIrql);

    switch (IoStatus->Status)
    {
        case STATUS_SUCCESS:
            DeviceState.DeviceState = PowerDeviceD0;
            PoRequestPowerIrp(DeviceExtension->Pdo,
                              IRP_MN_SET_POWER,
                              DeviceState,
                              KbdClassWakeUpPowerComplete,
                              DeviceExtension,
                              NULL);
            break;

        case STATUS_DEVICE_BUSY:
        case STATUS_CANCELLED:
        case STATUS_INVALID_DEVICE_STATE:
        case STATUS_POWER_STATE_INVALID:
        case STATUS_ACPI_POWER_REQUEST_FAILED:
            break;

        default:
            /* The stack cannot wake the system, stop asking */
            KbdClassToggleWaitWake(DeviceExtension, FALSE);
            break;
    }

    if (Irp)
        IoReleaseRemoveLock(&DeviceExtension->RemoveLock, Irp);
}

static
VOID
NTAPI
KbdClassToggleWaitWakeWorker(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_opt_ PVOID Context)
{
    PKBDCLASS_WORK_CONTEXT WorkContext = Context;
    PKBDCLASS_DEVICE_EXTENSION DeviceExtension = WorkContext->DeviceExtension;
    UNICODE_STRING ValueName = RTL_CONSTANT_STRING(L"WaitWakeEnabled");
    BOOLEAN Enable = WorkContext->Enable;
    BOOLEAN Changed = FALSE;
    PIRP CancelIrp = NULL;
    HANDLE KeyHandle;
    KIRQL OldIrql;
    ULONG Value;

    UNREFERENCED_PARAMETER(DeviceObject);

    KeAcquireSpinLock(&DeviceExtension->WaitWakeLock, &OldIrql);

    if (Enable != DeviceExtension->WaitWakeEnabled)
    {
        Changed = TRUE;

        if (DeviceExtension->WaitWakeEnabled &&
            DeviceExtension->WaitWakeIrp &&
            !DeviceExtension->WaitWakeCancelled)
        {
            DeviceExtension->WaitWakeCancelled = TRUE;
            CancelIrp = DeviceExtension->WaitWakeIrp;
        }

        DeviceExtension->WaitWakeEnabled = Enable;
    }

    KeReleaseSpinLock(&DeviceExtension->WaitWakeLock, OldIrql);

    if (Changed)
    {
        if (NT_SUCCESS(IoOpenDeviceRegistryKey(DeviceExtension->Pdo,
                                               PLUGPLAY_REGKEY_DEVICE,
                                               KEY_WRITE,
                                               &KeyHandle)))
        {
            Value = Enable;
            ZwSetValueKey(KeyHandle, &ValueName, 0, REG_DWORD, &Value, sizeof(Value));
            ZwClose(KeyHandle);
        }

        if (Enable)
            KbdClassCreateWaitWakeIrp(DeviceExtension);
    }

    if (CancelIrp)
        IoCancelIrp(CancelIrp);

    IoReleaseRemoveLock(&DeviceExtension->RemoveLock, &DeviceExtension->WaitWakeEnabled);
    IoFreeWorkItem(WorkContext->WorkItem);
    ExFreePoolWithTag(WorkContext, KBDCLASS_TAG);
}

/**
 * @brief Turns wait wake on or off and remembers the choice.
 */
NTSTATUS
NTAPI
KbdClassToggleWaitWake(
    _In_ PKBDCLASS_DEVICE_EXTENSION DeviceExtension,
    _In_ BOOLEAN Enable)
{
    PKBDCLASS_WORK_CONTEXT WorkContext;
    NTSTATUS Status;

    Status = IoAcquireRemoveLock(&DeviceExtension->RemoveLock, &DeviceExtension->WaitWakeEnabled);
    if (!NT_SUCCESS(Status))
        return Status;

    WorkContext = ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*WorkContext), KBDCLASS_TAG);
    if (!WorkContext)
    {
        IoReleaseRemoveLock(&DeviceExtension->RemoveLock, &DeviceExtension->WaitWakeEnabled);
        return STATUS_SUCCESS;
    }

    WorkContext->WorkItem = IoAllocateWorkItem(DeviceExtension->Self);
    if (!WorkContext->WorkItem)
    {
        IoReleaseRemoveLock(&DeviceExtension->RemoveLock, &DeviceExtension->WaitWakeEnabled);
        ExFreePoolWithTag(WorkContext, KBDCLASS_TAG);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    WorkContext->DeviceExtension = DeviceExtension;
    WorkContext->Enable = (Enable != FALSE);

    if (KeGetCurrentIrql() > PASSIVE_LEVEL)
    {
        IoQueueWorkItem(WorkContext->WorkItem,
                        KbdClassToggleWaitWakeWorker,
                        DelayedWorkQueue,
                        WorkContext);
    }
    else
    {
        KbdClassToggleWaitWakeWorker(DeviceExtension->Self, WorkContext);
    }

    return STATUS_SUCCESS;
}

/* LEDS ***********************************************************************/

NTSTATUS
NTAPI
KbdClassSetLedsComplete(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_reads_opt_(_Inexpressible_("varies")) PVOID Context)
{
    PKBDCLASS_DEVICE_EXTENSION DeviceExtension = Context;
    BOOLEAN FreeIrp;
    PIRP Current;
    LONG Previous;

    UNREFERENCED_PARAMETER(DeviceObject);

    Current = InterlockedCompareExchangePointer((PVOID *)&DeviceExtension->LedIrp, NULL, Irp);
    if (Current && Current != Irp)
    {
        /* A newer LED IRP replaced this one */
        FreeIrp = TRUE;
    }
    else
    {
        Previous = InterlockedExchange(&DeviceExtension->LedState, KBDCLASS_LEDS_COMPLETED);
        FreeIrp = (Current == Irp) || (Previous == KBDCLASS_LEDS_CANCELLED);
    }

    IoReleaseRemoveLock(&DeviceExtension->RemoveLock, Irp);

    if (FreeIrp)
        IoFreeIrp(Irp);

    return STATUS_MORE_PROCESSING_REQUIRED;
}

/**
 * @brief Sends the last known LED state to the port after a return to D0.
 */
static
VOID
KbdClassRestoreLeds(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PKBDCLASS_DEVICE_EXTENSION DeviceExtension,
    _In_opt_ PFILE_OBJECT FileObject)
{
    PKEYBOARD_INDICATOR_PARAMETERS Indicators = &DeviceExtension->Indicators;
    PKBDCLASS_DEVICE_EXTENSION GrandMaster = KbdClassGlobals.GrandMaster;
    PIO_STACK_LOCATION Next;
    PIRP LedIrp;

    LedIrp = IoAllocateIrp(DeviceObject->StackSize, FALSE);
    if (!LedIrp)
        return;

    if (!NT_SUCCESS(IoAcquireRemoveLock(&DeviceExtension->RemoveLock, LedIrp)))
    {
        IoFreeIrp(LedIrp);
        return;
    }

    if (GrandMaster)
    {
        Indicators = &GrandMaster->Indicators;
        FileObject = DeviceExtension->InPortList ?
                     KbdClassGlobals.PortList[DeviceExtension->UnitId].File : NULL;
    }

    Next = IoGetNextIrpStackLocation(LedIrp);
    Next->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
    Next->Parameters.DeviceIoControl.OutputBufferLength = 0;
    Next->Parameters.DeviceIoControl.InputBufferLength = sizeof(KEYBOARD_INDICATOR_PARAMETERS);
    Next->Parameters.DeviceIoControl.IoControlCode = IOCTL_KEYBOARD_SET_INDICATORS;
    Next->FileObject = FileObject;
    LedIrp->AssociatedIrp.SystemBuffer = Indicators;

    IoSetCompletionRoutine(LedIrp, KbdClassSetLedsComplete, DeviceExtension, TRUE, TRUE, TRUE);

    KbdClassCancelPendingLeds(DeviceExtension);
    InterlockedExchangePointer((PVOID *)&DeviceExtension->LedIrp, LedIrp);
    InterlockedExchange(&DeviceExtension->LedState, KBDCLASS_LEDS_SENT);

    IoCallDriver(DeviceExtension->TopPort, LedIrp);
}

/* POWER IRPS *****************************************************************/

/**
 * @brief Runs once a power up or wait wake IRP comes back from the port.
 */
static
NTSTATUS
NTAPI
KbdClassPowerComplete(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_reads_opt_(_Inexpressible_("varies")) PVOID Context)
{
    PKBDCLASS_DEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    POWER_STATE State = Stack->Parameters.Power.State;
    POWER_STATE DeviceState;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(Context);

    if (Stack->MinorFunction == IRP_MN_WAIT_WAKE)
    {
        PoStartNextPowerIrp(Irp);

        /* A removal in progress cancelled it and finishes the completion */
        if (Irp == DeviceExtension->WaitWakeIrp &&
            InterlockedExchange(&DeviceExtension->WaitWakeRemoveState, KBDCLASS_WW_COMPLETED) ==
            KBDCLASS_WW_REMOVE_CANCELLING)
        {
            return STATUS_MORE_PROCESSING_REQUIRED;
        }

        return STATUS_SUCCESS;
    }

    if (Stack->MinorFunction == IRP_MN_SET_POWER)
    {
        if (Stack->Parameters.Power.Type == DevicePowerState)
        {
            if (State.DeviceState == PowerDeviceD0)
            {
                PoSetPowerState(DeviceExtension->Self, DevicePowerState, State);
                DeviceExtension->DeviceState = State.DeviceState;
                KbdClassRestoreLeds(DeviceObject, DeviceExtension, Stack->FileObject);
            }
        }
        else if (Stack->Parameters.Power.Type == SystemPowerState)
        {
            PoSetPowerState(DeviceExtension->Self, SystemPowerState, State);
            DeviceExtension->SystemState = State.SystemState;

            DeviceState.DeviceState = PowerDeviceD0;
            Status = PoRequestPowerIrp(DeviceExtension->Self,
                                       IRP_MN_SET_POWER,
                                       DeviceState,
                                       KbdClassResumePowerComplete,
                                       NULL,
                                       NULL);
            if (!NT_SUCCESS(Status))
                Irp->IoStatus.Status = Status;
        }
    }

    PoStartNextPowerIrp(Irp);
    IoReleaseRemoveLock(&DeviceExtension->RemoveLock, Irp);
    return STATUS_SUCCESS;
}

/**
 * @brief The device IRP for a system sleep finished, pass the S IRP on.
 */
static
VOID
NTAPI
KbdClassSleepPowerComplete(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ UCHAR MinorFunction,
    _In_ POWER_STATE PowerState,
    _In_opt_ PVOID Context,
    _In_ PIO_STATUS_BLOCK IoStatus)
{
    PKBDCLASS_DEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
    PIRP SystemIrp = Context;
    POWER_STATE SystemState;

    UNREFERENCED_PARAMETER(MinorFunction);
    UNREFERENCED_PARAMETER(PowerState);
    UNREFERENCED_PARAMETER(IoStatus);

    SystemState = IoGetCurrentIrpStackLocation(SystemIrp)->Parameters.Power.State;
    PoSetPowerState(DeviceExtension->Self, SystemPowerState, SystemState);
    DeviceExtension->SystemState = SystemState.SystemState;

    PoStartNextPowerIrp(SystemIrp);
    IoSkipCurrentIrpStackLocation(SystemIrp);
    PoCallDriver(DeviceExtension->TopPort, SystemIrp);

    IoReleaseRemoveLock(&DeviceExtension->RemoveLock, SystemIrp);
}

/**
 * @brief The device is back in D0 after a system wake, rearm wait wake.
 */
static
VOID
NTAPI
KbdClassResumePowerComplete(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ UCHAR MinorFunction,
    _In_ POWER_STATE PowerState,
    _In_opt_ PVOID Context,
    _In_ PIO_STATUS_BLOCK IoStatus)
{
    PKBDCLASS_DEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;

    UNREFERENCED_PARAMETER(MinorFunction);
    UNREFERENCED_PARAMETER(PowerState);
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(IoStatus);

    if (!KbdClassWakeCapable(DeviceExtension))
        return;

    if (KbdClassWaitWakePending(DeviceExtension))
        return;

    if (!KbdClassCheckWaitWakeEnabled(DeviceExtension))
        return;

    if (!NT_SUCCESS(KbdClassQueueWaitWakeArm(DeviceExtension)))
    {
        KbdClassLogError(DeviceExtension->Self,
                         KBDCLASS_LOG_WAIT_WAKE_RESOURCES,
                         1,
                         STATUS_INSUFFICIENT_RESOURCES,
                         0,
                         NULL,
                         0);
    }
}

static
NTSTATUS
KbdClassPowerPassDown(
    _In_ PKBDCLASS_DEVICE_EXTENSION DeviceExtension,
    _In_ PIRP Irp)
{
    NTSTATUS Status;

    PoStartNextPowerIrp(Irp);
    IoSkipCurrentIrpStackLocation(Irp);
    Status = PoCallDriver(DeviceExtension->TopPort, Irp);
    IoReleaseRemoveLock(&DeviceExtension->RemoveLock, Irp);

    return Status;
}

/**
 * @brief Passes a power IRP down with KbdClassPowerComplete attached.
 */
static
NTSTATUS
KbdClassPowerUp(
    _In_ PKBDCLASS_DEVICE_EXTENSION DeviceExtension,
    _In_ PIRP Irp)
{
    /* The completion routine drops this extra reference */
    if (!NT_SUCCESS(IoAcquireRemoveLock(&DeviceExtension->RemoveLock, Irp)))
        return KbdClassPowerPassDown(DeviceExtension, Irp);

    IoCopyCurrentIrpStackLocationToNext(Irp);
    IoSetCompletionRoutine(Irp, KbdClassPowerComplete, NULL, TRUE, TRUE, TRUE);
    IoMarkIrpPending(Irp);
    PoCallDriver(DeviceExtension->TopPort, Irp);

    IoReleaseRemoveLock(&DeviceExtension->RemoveLock, Irp);
    return STATUS_PENDING;
}

static
NTSTATUS
KbdClassPowerWaitWake(
    _In_ PKBDCLASS_DEVICE_EXTENSION DeviceExtension,
    _In_ PIRP Irp)
{
    NTSTATUS Status = STATUS_INVALID_DEVICE_STATE;
    BOOLEAN Forward = FALSE;
    LONG Previous;
    KIRQL OldIrql;

    KeAcquireSpinLock(&DeviceExtension->WaitWakeLock, &OldIrql);

    if (DeviceExtension->WaitWakeIrp)
    {
        DeviceExtension->ExtraWaitWakeIrp = Irp;
    }
    else
    {
        DeviceExtension->WaitWakeIrp = Irp;
        DeviceExtension->WaitWakeCancelled = FALSE;

        Previous = InterlockedExchange(&DeviceExtension->WaitWakeRemoveState, KBDCLASS_WW_ARMED);
        if (Previous != KBDCLASS_WW_REMOVE_CANCELLING && Previous != KBDCLASS_WW_REMOVE_DONE)
        {
            Forward = TRUE;
            Status = STATUS_SUCCESS;
        }
    }

    KeReleaseSpinLock(&DeviceExtension->WaitWakeLock, OldIrql);

    if (!Forward)
    {
        /* The remove lock stays held until KbdClassWaitWakeComplete runs */
        Irp->IoStatus.Status = Status;
        PoStartNextPowerIrp(Irp);
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return Status;
    }

    IoAcquireRemoveLock(&DeviceExtension->RemoveLock, Irp);

    IoCopyCurrentIrpStackLocationToNext(Irp);
    IoSetCompletionRoutine(Irp, KbdClassPowerComplete, NULL, TRUE, TRUE, TRUE);
    IoMarkIrpPending(Irp);
    PoCallDriver(DeviceExtension->TopPort, Irp);

    IoReleaseRemoveLock(&DeviceExtension->RemoveLock, Irp);
    return STATUS_PENDING;
}

static
NTSTATUS
KbdClassSetDevicePower(
    _In_ PKBDCLASS_DEVICE_EXTENSION DeviceExtension,
    _In_ PIRP Irp,
    _In_ POWER_STATE State)
{
    Irp->IoStatus.Status = STATUS_SUCCESS;

    if (DeviceExtension->DeviceState < State.DeviceState)
    {
        PoSetPowerState(DeviceExtension->Self, DevicePowerState, State);
        DeviceExtension->DeviceState = State.DeviceState;
        return KbdClassPowerPassDown(DeviceExtension, Irp);
    }

    if (DeviceExtension->DeviceState > State.DeviceState)
        return KbdClassPowerUp(DeviceExtension, Irp);

    return KbdClassPowerPassDown(DeviceExtension, Irp);
}

static
NTSTATUS
KbdClassSetSystemPower(
    _In_ PKBDCLASS_DEVICE_EXTENSION DeviceExtension,
    _In_ PIRP Irp,
    _In_ POWER_STATE State)
{
    POWER_STATE DeviceState;
    NTSTATUS Status;

    if (State.SystemState < DeviceExtension->SystemState)
    {
        Irp->IoStatus.Status = STATUS_SUCCESS;
        return KbdClassPowerUp(DeviceExtension, Irp);
    }

    if (State.SystemState == DeviceExtension->SystemState)
    {
        if (State.SystemState == PowerSystemWorking &&
            KbdClassWakeCapable(DeviceExtension) &&
            !KbdClassWaitWakePending(DeviceExtension) &&
            KbdClassCheckWaitWakeEnabled(DeviceExtension))
        {
            KbdClassCreateWaitWakeIrp(DeviceExtension);
        }

        Irp->IoStatus.Status = STATUS_SUCCESS;
        return KbdClassPowerPassDown(DeviceExtension, Irp);
    }

    /* Going to sleep: request the matching D IRP first */
    Status = IoAcquireRemoveLock(&DeviceExtension->RemoveLock, Irp);
    if (!NT_SUCCESS(Status))
    {
        Irp->IoStatus.Status = Status;
        PoStartNextPowerIrp(Irp);
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        IoReleaseRemoveLock(&DeviceExtension->RemoveLock, Irp);
        return Status;
    }

    if (!KbdClassWaitWakePending(DeviceExtension) || State.SystemState >= PowerSystemHibernate)
        DeviceState.DeviceState = PowerDeviceD3;
    else
        DeviceState.DeviceState = DeviceExtension->SystemToDevice[State.SystemState];

    IoMarkIrpPending(Irp);

    Status = PoRequestPowerIrp(DeviceExtension->Self,
                               IRP_MN_SET_POWER,
                               DeviceState,
                               KbdClassSleepPowerComplete,
                               Irp,
                               NULL);
    if (NT_SUCCESS(Status))
    {
        IoReleaseRemoveLock(&DeviceExtension->RemoveLock, Irp);
        return STATUS_PENDING;
    }

    IoReleaseRemoveLock(&DeviceExtension->RemoveLock, Irp);
    PoStartNextPowerIrp(Irp);
    Irp->IoStatus.Status = Status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    IoReleaseRemoveLock(&DeviceExtension->RemoveLock, Irp);
    return STATUS_PENDING;
}

NTSTATUS
NTAPI
KbdClassPower(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PKBDCLASS_DEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    POWER_STATE State = Stack->Parameters.Power.State;
    NTSTATUS Status;

    if (DeviceExtension == KbdClassGlobals.GrandMaster || !DeviceExtension->PnP)
    {
        PoStartNextPowerIrp(Irp);
        Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_NOT_SUPPORTED;
    }

    Status = IoAcquireRemoveLock(&DeviceExtension->RemoveLock, Irp);
    if (!NT_SUCCESS(Status))
    {
        PoStartNextPowerIrp(Irp);
        Irp->IoStatus.Status = Status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return Status;
    }

    switch (Stack->MinorFunction)
    {
        case IRP_MN_WAIT_WAKE:
            return KbdClassPowerWaitWake(DeviceExtension, Irp);

        case IRP_MN_QUERY_POWER:
            /* An armed wake IRP cannot wake the system from that deep */
            if ((LONG)State.SystemState < PowerSystemHibernate &&
                (LONG)State.SystemState > (LONG)DeviceExtension->MinSystemWake &&
                KbdClassWaitWakePending(DeviceExtension))
            {
                Status = STATUS_POWER_STATE_INVALID;
                Irp->IoStatus.Status = Status;
                PoStartNextPowerIrp(Irp);
                IoCompleteRequest(Irp, IO_NO_INCREMENT);
                IoReleaseRemoveLock(&DeviceExtension->RemoveLock, Irp);
                return Status;
            }

            Irp->IoStatus.Status = STATUS_SUCCESS;
            return KbdClassPowerPassDown(DeviceExtension, Irp);

        case IRP_MN_SET_POWER:
            if (Stack->Parameters.Power.Type == DevicePowerState)
                return KbdClassSetDevicePower(DeviceExtension, Irp, State);

            if (Stack->Parameters.Power.Type == SystemPowerState)
                return KbdClassSetSystemPower(DeviceExtension, Irp, State);

            return KbdClassPowerPassDown(DeviceExtension, Irp);

        default:
            return KbdClassPowerPassDown(DeviceExtension, Irp);
    }
}
