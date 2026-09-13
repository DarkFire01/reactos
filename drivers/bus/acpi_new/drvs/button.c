/*
 * PROJECT:     uACPI-NT
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     SYS_BUTTON interface for the power, sleep and lid buttons
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

// Button and lid events reach the power service only via SYS_BUTTON IOCTLs.

#include "acpipriv.h"
#include <ntpoapi.h>        // SYSTEM_POWER_POLICY, POWER_ACTION_POLICY (LidClose policy)
#include <uacpi/uacpi.h>
#include <uacpi/utilities.h>
#include <uacpi/tables.h>   // uacpi_table_fadt
#include <uacpi/acpi.h>     // struct acpi_fadt, ACPI_PWR_BUTTON / ACPI_SLP_BUTTON

extern const GUID GUID_DEVICE_SYS_BUTTON;   // guid.c

// poclass.h: FILE_DEVICE_BATTERY, functions 0x50/0x51, METHOD_BUFFERED.
#define IOCTL_GET_SYS_BUTTON_CAPS    0x00294140
#define IOCTL_GET_SYS_BUTTON_EVENT   0x00294144

#define SYS_BUTTON_POWER   0x00000001
#define SYS_BUTTON_SLEEP   0x00000002
#define SYS_BUTTON_LID     0x00000004
#define SYS_BUTTON_WAKE    0x80000000

// Set ButtonCaps from _HID at PDO creation; nonzero marks a button or lid.
BOOLEAN
UacpiButtonClassify(PUACPI_PDO Pdo)
{
    ULONG caps = 0;

    // Power and sleep are wake sources (WAKE bit); the lid is not.
    if (_stricmp(Pdo->Hid, "PNP0C0C") == 0)
    {
        caps = SYS_BUTTON_WAKE | SYS_BUTTON_POWER;   // 0x80000001
    }
    else if (_stricmp(Pdo->Hid, "PNP0C0E") == 0)
    {
        caps = SYS_BUTTON_WAKE | SYS_BUTTON_SLEEP;   // 0x80000002
    }
    else if (_stricmp(Pdo->Hid, "PNP0C0D") == 0)
    {
        caps = SYS_BUTTON_LID;                       // 0x00000004
    }

    Pdo->ButtonCaps = caps;
    if (caps != 0)
    {
        KeInitializeSpinLock(&Pdo->ButtonLock);
        InitializeListHead(&Pdo->ButtonIrpQueue);
    }
    return caps != 0;
}

// \Callback\PowerState (lid only): track whether the LidClose action is None.
static VOID
NTAPI
UacpiLidPowerStateCallback(PVOID Context, PVOID Argument1, PVOID Argument2)
{
    PUACPI_PDO Pdo = (PUACPI_PDO)Context;
    SYSTEM_POWER_POLICY policy;

    UNREFERENCED_PARAMETER(Argument2);
    if (Argument1 != NULL)   // only the system-power-policy notification (code 0)
    {
        return;
    }
    RtlZeroMemory(&policy, sizeof(policy));
    if (NT_SUCCESS(ZwPowerInformation(SystemPowerPolicyCurrent, NULL, 0,
                                      &policy, sizeof(policy))))
                                      {
        POWER_ACTION a = policy.LidClose.Action;
        Pdo->LidCloseNoAction = (BOOLEAN)(a == PowerActionNone ||
                                          a == PowerActionReserved);
    }
}

// Register and enable GUID_DEVICE_SYS_BUTTON at IRP_MN_START_DEVICE.
VOID
UacpiButtonStart(PUACPI_PDO Pdo)
{
    NTSTATUS status;

    if (Pdo->ButtonCaps == 0 || Pdo->ButtonIfRegistered)
    {
        return;
    }

    status = IoRegisterDeviceInterface(Pdo->Common.Self, &GUID_DEVICE_SYS_BUTTON,
                                       NULL, &Pdo->ButtonSymLink);
    if (!NT_SUCCESS(status))
    {
        UacpiTrace("[acpi] button: %s IoRegisterDeviceInterface failed 0x%X\n",
                  Pdo->Name, status);
        return;
    }
    Pdo->ButtonIfRegistered = TRUE;
    (void)IoSetDeviceInterfaceState(&Pdo->ButtonSymLink, TRUE);
    UacpiTrace("[acpi] button: %s SYS_BUTTON interface up (caps 0x%X)\n",
              Pdo->Name, Pdo->ButtonCaps);

    // Lid: track the LidClose policy and report the initial _LID state.
    if (Pdo->ButtonCaps & SYS_BUTTON_LID)
    {
        UNICODE_STRING     cbName;
        OBJECT_ATTRIBUTES  oa;
        RtlInitUnicodeString(&cbName, L"\\Callback\\PowerState");
        InitializeObjectAttributes(&oa, &cbName,
                                   OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
        if (NT_SUCCESS(ExCreateCallback(&Pdo->LidPowerCallback, &oa, FALSE, TRUE)))
        {
            Pdo->LidPowerCbReg = ExRegisterCallback(Pdo->LidPowerCallback,
                                                    UacpiLidPowerStateCallback, Pdo);
            UacpiLidPowerStateCallback(Pdo, NULL, NULL);   // prime current policy
        }
        UacpiButtonNotify(Pdo, 0x80);   // deliver/latch the initial lid state
    }

    // Issue our own WAIT_WAKE; UacpiWakeArm arms the _PRW GPE for a real button.
    UacpiButtonArmWaitWake(Pdo);
}

// WAIT_WAKE completion: re-arm the loop.
static VOID
NTAPI
UacpiButtonWaitWakeComplete(PDEVICE_OBJECT DeviceObject, UCHAR MinorFunction,
                           POWER_STATE PowerState, PVOID Context,
                           PIO_STATUS_BLOCK IoStatus)
{
    PUACPI_PDO Pdo = (PUACPI_PDO)Context;

    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(MinorFunction);
    UNREFERENCED_PARAMETER(PowerState);

    // Canceled or failed: do not re-arm, or a hard error would spin.
    if (IoStatus == NULL || !NT_SUCCESS(IoStatus->Status))
    {
        return;
    }

    // The event arrives via Notify(2) at PASSIVE_LEVEL; no _LID eval here.
    UacpiButtonArmWaitWake(Pdo);
}

VOID
UacpiButtonArmWaitWake(PUACPI_PDO Pdo)
{
    POWER_STATE ps;

    if (Pdo->ButtonCaps == 0)
    {
        return;
    }
    // Do not arm the lid as a wake source when lid-close has no action.
    if ((Pdo->ButtonCaps & SYS_BUTTON_LID) && Pdo->LidCloseNoAction)
    {
        return;
    }

    // A control-method button carries a _PRW: arm at its computed deepest wake
    // state, and skip entirely if it declared a _PRW with no usable state. The
    // node-less fixed button has no _PRW and keeps the S3 default (its wake is a
    // PM1 fixed event, so the WAIT_WAKE is bookkeeping either way).
    ps.SystemState = PowerSystemSleeping3;
    if (Pdo->Wake.Node != NULL && UacpiWakeHasPrw(&Pdo->Wake))
    {
        if (Pdo->Wake.SysWake == PowerSystemUnspecified)
        {
            return;
        }
        ps.SystemState = Pdo->Wake.SysWake;
    }

    (void)PoRequestPowerIrp(Pdo->Common.Self, IRP_MN_WAIT_WAKE, ps,
                            UacpiButtonWaitWakeComplete, Pdo, NULL);
}

// Cancel routine for a pended IOCTL_GET_SYS_BUTTON_EVENT.
static VOID
NTAPI
UacpiButtonCancelIrp(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PUACPI_PDO Pdo = (PUACPI_PDO)DeviceObject->DeviceExtension;
    KIRQL     irql;

    IoReleaseCancelSpinLock(Irp->CancelIrql);

    KeAcquireSpinLock(&Pdo->ButtonLock, &irql);
    RemoveEntryList(&Irp->Tail.Overlay.ListEntry);
    KeReleaseSpinLock(&Pdo->ButtonLock, irql);

    Irp->IoStatus.Status = STATUS_CANCELLED;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
}

// SYS_BUTTON IOCTLs. On *Handled == FALSE the caller tries the eval IOCTLs.
NTSTATUS
UacpiButtonDeviceControl(PUACPI_PDO Pdo, PIRP Irp, PBOOLEAN Handled)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    ULONG code   = sp->Parameters.DeviceIoControl.IoControlCode;
    ULONG outLen = sp->Parameters.DeviceIoControl.OutputBufferLength;
    KIRQL irql;

    *Handled = FALSE;
    if (Pdo->ButtonCaps == 0)
    {
        return STATUS_NOT_SUPPORTED;
    }
    // SYS_BUTTON IOCTLs are kernel-mode only.
    if (Irp->RequestorMode != KernelMode &&
        (code == IOCTL_GET_SYS_BUTTON_CAPS || code == IOCTL_GET_SYS_BUTTON_EVENT))
        {
        *Handled = TRUE;
        return UacpiCompleteIrp(Irp, STATUS_NOT_IMPLEMENTED, 0);
    }

    if (code == IOCTL_GET_SYS_BUTTON_CAPS)
    {
        *Handled = TRUE;
        if (outLen < sizeof(ULONG) || Irp->AssociatedIrp.SystemBuffer == NULL)
        {
            return UacpiCompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);
        }
        *(PULONG)Irp->AssociatedIrp.SystemBuffer = Pdo->ButtonCaps;
        return UacpiCompleteIrp(Irp, STATUS_SUCCESS, sizeof(ULONG));
    }

    if (code == IOCTL_GET_SYS_BUTTON_EVENT)
    {
        *Handled = TRUE;
        if (outLen < sizeof(ULONG) || Irp->AssociatedIrp.SystemBuffer == NULL)
        {
            return UacpiCompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);
        }

        KeAcquireSpinLock(&Pdo->ButtonLock, &irql);

        // Event already latched: return it now.
        if (Pdo->ButtonEvents != 0)
        {
            ULONG ev = Pdo->ButtonEvents;
            Pdo->ButtonEvents = 0;
            KeReleaseSpinLock(&Pdo->ButtonLock, irql);
            *(PULONG)Irp->AssociatedIrp.SystemBuffer = ev;
            return UacpiCompleteIrp(Irp, STATUS_SUCCESS, sizeof(ULONG));
        }

        // Otherwise pend until a button/lid Notify arrives.
        IoSetCancelRoutine(Irp, UacpiButtonCancelIrp);
        if (Irp->Cancel && IoSetCancelRoutine(Irp, NULL) != NULL)
        {
            KeReleaseSpinLock(&Pdo->ButtonLock, irql);
            return UacpiCompleteIrp(Irp, STATUS_CANCELLED, 0);
        }
        IoMarkIrpPending(Irp);
        InsertTailList(&Pdo->ButtonIrpQueue, &Irp->Tail.Overlay.ListEntry);
        KeReleaseSpinLock(&Pdo->ButtonLock, irql);
        return STATUS_PENDING;
    }

    return STATUS_NOT_SUPPORTED;   // not a button IOCTL; caller handles it
}

// Lid event values reported through GUID_DEVICE_SYS_BUTTON, matching Vista's
// _LID worker: the power service reads the lid-present bit (0x00080000), the
// open flag (0x80000000) and the closed marker (0x00000004). The first report
// after start additionally carries the initial-state bit (0x00040000).
#define SYS_BUTTON_LID_PRESENT  0x00080000u
#define SYS_BUTTON_LID_CLOSED   (SYS_BUTTON_LID_PRESENT | 0x00000004u)   // 0x00080004
#define SYS_BUTTON_LID_OPEN     (SYS_BUTTON_LID_PRESENT | 0x80000000u)   // 0x80080000
#define SYS_BUTTON_LID_INITIAL  0x00040000u

// Complete a waiting event IRP or latch it. No AML; safe at <= DISPATCH.
VOID
UacpiButtonDeliverEvent(PUACPI_PDO Pdo, ULONG Event)
{
    KIRQL irql;
    PIRP  irp = NULL;

    KeAcquireSpinLock(&Pdo->ButtonLock, &irql);
    if (!IsListEmpty(&Pdo->ButtonIrpQueue))
    {
        PLIST_ENTRY e = Pdo->ButtonIrpQueue.Flink;
        PIRP cand = CONTAINING_RECORD(e, IRP, Tail.Overlay.ListEntry);
        // Claim the IRP only if we win the race with its cancel routine.
        if (IoSetCancelRoutine(cand, NULL) != NULL)
        {
            RemoveEntryList(e);
            irp = cand;
        }
        // else: the cancel routine owns it; latch instead.
    }
    if (irp == NULL)
    {
        Pdo->ButtonEvents |= Event;
    }
    KeReleaseSpinLock(&Pdo->ButtonLock, irql);

    if (irp != NULL)
    {
        *(PULONG)irp->AssociatedIrp.SystemBuffer = Event;
        irp->IoStatus.Status = STATUS_SUCCESS;
        irp->IoStatus.Information = sizeof(ULONG);
        IoCompleteRequest(irp, IO_NO_INCREMENT);
        UacpiTrace("[acpi] button: %s delivered event 0x%X\n", Pdo->Name, Event);
    }
    else
    {
        UacpiTrace("[acpi] button: %s latched event 0x%X (no waiter)\n",
                  Pdo->Name, Event);
    }
}

VOID
UacpiButtonNotify(PUACPI_PDO Pdo, ULONG NotifyValue)
{
    ULONG ev;

    if (Pdo->ButtonCaps == 0)
    {
        return;
    }

    // Notify(0x02) on any button reports the WAKE event.
    if (NotifyValue == 0x02)
    {
        ev = SYS_BUTTON_WAKE;
    }
    else if (NotifyValue == 0x80)
    {
        // Power/sleep: caps without WAKE. Lid: report the _LID state.
        if (Pdo->ButtonCaps & SYS_BUTTON_LID)
        {
            uacpi_u64 lid = 1;
            if (Pdo->Node != NULL)
            {
                (void)uacpi_eval_simple_integer(Pdo->Node, "_LID", &lid);
            }
            ev = lid ? SYS_BUTTON_LID_OPEN : SYS_BUTTON_LID_CLOSED;
            if (!Pdo->LidInitialReported)
            {
                ev |= SYS_BUTTON_LID_INITIAL;   // first report after start
                Pdo->LidInitialReported = TRUE;
            }
        }
        else
        {
            ev = Pdo->ButtonCaps & 0x7FFFFFFFu;
        }
    }
    else
    {
        return;   // not a button/lid/wake notify
    }

    UacpiButtonDeliverEvent(Pdo, ev);
}

// Fixed-feature power/sleep buttons: one node-less ACPI\FixedButton PDO.
// Fixed events arrive in the SCI ISR; latch them and deliver from a DPC.
_Function_class_(KDEFERRED_ROUTINE)
static VOID
NTAPI
UacpiFixedButtonDpc(PKDPC Dpc, PVOID Context, PVOID Arg1, PVOID Arg2)
{
    PUACPI_PDO pdo = (PUACPI_PDO)Context;
    ULONG events = (ULONG)InterlockedExchange(&pdo->ButtonDeferred, 0);

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(Arg1);
    UNREFERENCED_PARAMETER(Arg2);

    // A press during the resume window is the wake source, not a fresh press:
    // acpi.sys reports SYS_BUTTON_WAKE (it keys off WAK_STS). Reporting POWER
    // here would let the power policy immediately re-sleep or shut down.
    if (events != 0 && InterlockedCompareExchange(&UacpiSystemResuming, 0, 0) != 0)
    {
        UacpiButtonDeliverEvent(pdo, SYS_BUTTON_WAKE);
        return;
    }

    if (events & SYS_BUTTON_POWER)
    {
        UacpiButtonDeliverEvent(pdo, SYS_BUTTON_POWER);
    }
    if (events & SYS_BUTTON_SLEEP)
    {
        UacpiButtonDeliverEvent(pdo, SYS_BUTTON_SLEEP);
    }
}

static VOID
UacpiFixedButtonDefer(PUACPI_PDO Pdo, ULONG Event)
{
    InterlockedOr(&Pdo->ButtonDeferred, (LONG)Event);
    KeInsertQueueDpc(&Pdo->ButtonDpc, NULL, NULL);
}

static uacpi_interrupt_ret
UacpiFixedPowerButtonHandler(uacpi_handle ctx)
{
    UacpiFixedButtonDefer((PUACPI_PDO)ctx, SYS_BUTTON_POWER);
    return UACPI_INTERRUPT_HANDLED;
}

static uacpi_interrupt_ret
UacpiFixedSleepButtonHandler(uacpi_handle ctx)
{
    UacpiFixedButtonDefer((PUACPI_PDO)ctx, SYS_BUTTON_SLEEP);
    return UACPI_INTERRUPT_HANDLED;
}

static BOOLEAN UacpiFixedButtonDone;

VOID
UacpiFixedButtonInit(PUACPI_FDO Fdo)
{
    struct acpi_fadt *fadt = NULL;
    PDEVICE_OBJECT pdoDevice;
    PUACPI_PDO pdo;
    ULONG caps = 0;
    NTSTATUS status;

    // Once only; a restarted FDO must not duplicate the PDO or handlers.
    if (UacpiFixedButtonDone)
    {
        return;
    }
    if (uacpi_unlikely_error(uacpi_table_fadt(&fadt)) || fadt == NULL)
    {
        return;
    }
    // A *clear* control-method flag means the button is a fixed feature.
    if (!(fadt->flags & ACPI_PWR_BUTTON))
    {
        caps |= SYS_BUTTON_POWER;
    }
    if (!(fadt->flags & ACPI_SLP_BUTTON))
    {
        caps |= SYS_BUTTON_SLEEP;
    }
    UacpiFixedButtonDone = TRUE;   // decision made (whether or not a fixed button exists)
    if (caps == 0)
    {
        return;   // both buttons are control-method (handled by the PNP0C0C/E PDOs)
    }
    caps |= SYS_BUTTON_WAKE;

    status = IoCreateDevice(Fdo->Common.Self->DriverObject, sizeof(UACPI_PDO), NULL,
                            FILE_DEVICE_ACPI,
                            FILE_AUTOGENERATED_DEVICE_NAME | FILE_DEVICE_SECURE_OPEN,
                            FALSE, &pdoDevice);
    if (!NT_SUCCESS(status))
    {
        return;
    }

    pdo = (PUACPI_PDO)pdoDevice->DeviceExtension;
    RtlZeroMemory(pdo, sizeof(*pdo));
    pdo->Common.Type = UacpiExtPdo;
    pdo->Common.Self = pdoDevice;
    pdo->Parent      = Fdo;
    pdo->Node        = NULL;                 // node-less: a fixed feature, no namespace object
    pdo->ParentNode  = uacpi_namespace_get_predefined(UACPI_PREDEFINED_NAMESPACE_SB);
    pdo->Present     = TRUE;
    pdo->ButtonCaps  = caps;
    UacpiWakeInit(&pdo->Wake, NULL);   // fixed feature: wakes via PM1, not a GPE
    KeInitializeSpinLock(&pdo->ButtonLock);
    InitializeListHead(&pdo->ButtonIrpQueue);
    KeInitializeDpc(&pdo->ButtonDpc, UacpiFixedButtonDpc, pdo);
    RtlStringCbCopyA(pdo->Name, sizeof(pdo->Name), "FXBT");
    RtlStringCbCopyA(pdo->Hid, sizeof(pdo->Hid), "FixedButton");   // -> ID "ACPI\FixedButton"
    RtlStringCbCopyA(pdo->Instance, sizeof(pdo->Instance), "0");

    pdoDevice->Flags |= DO_BUFFERED_IO | DO_POWER_PAGABLE;
    pdoDevice->Flags &= ~DO_DEVICE_INITIALIZING;

    ExAcquireFastMutex(&Fdo->ChildLock);
    InsertTailList(&Fdo->Children, &pdo->Link);
    ExReleaseFastMutex(&Fdo->ChildLock);

    if (caps & SYS_BUTTON_POWER)
    {
        (void)uacpi_install_fixed_event_handler(UACPI_FIXED_EVENT_POWER_BUTTON,
                                                UacpiFixedPowerButtonHandler, pdo);
    }
    if (caps & SYS_BUTTON_SLEEP)
    {
        (void)uacpi_install_fixed_event_handler(UACPI_FIXED_EVENT_SLEEP_BUTTON,
                                                UacpiFixedSleepButtonHandler, pdo);
    }
    UacpiTrace("[acpi] button: fixed-feature button PDO up (caps 0x%X)\n", caps);
}

// Remove: disable the interface and complete pended event IRPs.
VOID
UacpiButtonRemove(PUACPI_PDO Pdo)
{
    KIRQL      irql;
    LIST_ENTRY drain;

    if (Pdo->ButtonCaps == 0)
    {
        return;
    }

    if (Pdo->ButtonIfRegistered)
    {
        (void)IoSetDeviceInterfaceState(&Pdo->ButtonSymLink, FALSE);
        RtlFreeUnicodeString(&Pdo->ButtonSymLink);
        Pdo->ButtonIfRegistered = FALSE;
    }

    // Tear down the lid \Callback\PowerState hook.
    if (Pdo->LidPowerCbReg != NULL)
    {
        ExUnregisterCallback(Pdo->LidPowerCbReg);
        Pdo->LidPowerCbReg = NULL;
    }
    if (Pdo->LidPowerCallback != NULL)
    {
        ObDereferenceObject(Pdo->LidPowerCallback);
        Pdo->LidPowerCallback = NULL;
    }

    InitializeListHead(&drain);
    KeAcquireSpinLock(&Pdo->ButtonLock, &irql);
    while (!IsListEmpty(&Pdo->ButtonIrpQueue))
    {
        PLIST_ENTRY e = RemoveHeadList(&Pdo->ButtonIrpQueue);
        PIRP irp = CONTAINING_RECORD(e, IRP, Tail.Overlay.ListEntry);
        if (IoSetCancelRoutine(irp, NULL) != NULL)
        {
            InsertTailList(&drain, e);
        }
        // else: cancel routine owns it and will complete it.
    }
    KeReleaseSpinLock(&Pdo->ButtonLock, irql);

    while (!IsListEmpty(&drain))
    {
        PLIST_ENTRY e = RemoveHeadList(&drain);
        PIRP irp = CONTAINING_RECORD(e, IRP, Tail.Overlay.ListEntry);
        irp->IoStatus.Status = STATUS_DELETE_PENDING;
        irp->IoStatus.Information = 0;
        IoCompleteRequest(irp, IO_NO_INCREMENT);
    }
}
