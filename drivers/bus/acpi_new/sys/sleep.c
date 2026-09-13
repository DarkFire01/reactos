/*
 * PROJECT:     uACPI-NT
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     System sleep-state handling on the ACPI FDO
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#include "acpipriv.h"
#include <uacpi/sleep.h>
#include <uacpi/event.h>
#include <uacpi/registers.h>
#include <uacpi/acpi.h>                  // ACPI_PM1_STS_CLEAR
#include <uacpi/internal/event.h>        // uacpi_clear_all_events

static uacpi_sleep_state
UacpiMapSystemState(SYSTEM_POWER_STATE s)
{
    switch (s)
    {
    case PowerSystemSleeping1: return UACPI_SLEEP_STATE_S1;
    case PowerSystemSleeping2: return UACPI_SLEEP_STATE_S2;
    case PowerSystemSleeping3: return UACPI_SLEEP_STATE_S3;
    case PowerSystemHibernate: return UACPI_SLEEP_STATE_S4;
    case PowerSystemShutdown:  return UACPI_SLEEP_STATE_S5;
    default:                   return UACPI_SLEEP_STATE_S0;
    }
}

// Last prepared sleep state, for the matching \_WAK on resume.
static uacpi_sleep_state UacpiPendingSleepState = UACPI_SLEEP_STATE_S0;

// Set from the sleep IRP, cleared after \_WAK on the resume IRP. While set, a
// fixed power/sleep button event is the wake press and is reported as
// SYS_BUTTON_WAKE, not a fresh press (acpi.sys keys this off WAK_STS).
volatile LONG UacpiSystemResuming;
volatile LONG UacpiWakeSourceClaimed;

NTSTATUS
UacpiSystemSetPower(PUACPI_FDO Fdo, PIRP Irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    SYSTEM_POWER_STATE sys = sp->Parameters.Power.State.SystemState;

    if (!Fdo->UacpiUp)
    {
        PoStartNextPowerIrp(Irp);
        IoSkipCurrentIrpStackLocation(Irp);
        return PoCallDriver(Fdo->LowerDevice, Irp);
    }

    PoSetPowerState(Fdo->Common.Self, SystemPowerState, sp->Parameters.Power.State);

    if (sys == PowerSystemWorking)
    {
        // Resume: run \_WAK, then pass the IRP down.
        if (UacpiPendingSleepState != UACPI_SLEEP_STATE_S0)
        {
            uacpi_sleep_state woke = UacpiPendingSleepState;
            uacpi_status st = uacpi_wake_from_sleep_state(UacpiPendingSleepState);
            UacpiTrace("[acpi] _WAK for S%u: %s\n", (ULONG)UacpiPendingSleepState,
                      uacpi_unlikely_error(st) ? uacpi_status_to_string(st) : "OK");
            UacpiPendingSleepState = UACPI_SLEEP_STATE_S0;

            // Re-assert _ON on held power resources before the D0 IRPs arrive.
            UacpiPowerResResume();

            // Restore the wake mask on devices masked for this sleep's depth.
            UacpiWakeRestoreSuspended();

            // A hibernate cut all power: re-assert each armed device's wake
            // circuit (_PSW/_DSW). S1-S3 keep the circuit powered, so skip it.
            if (woke == UACPI_SLEEP_STATE_S4)
            {
                UacpiWakeReArmAfterHibernate();
            }

            // Firmware reset may have reverted PCI link routing; restore each
            // decided link's _SRS (PIC model only, a no-op under APIC).
            UacpiIrqLinksResume();
        }
        // uacpi_wake_from_sleep_state already evaluated \_SI._SST(1).
        // The wake press (if any) has been delivered by now; later button
        // events are real presses again.
        InterlockedExchange(&UacpiSystemResuming, 0);
    }
    else if (sys > PowerSystemWorking)
    {
        uacpi_sleep_state target = UacpiMapSystemState(sys);
        POWER_ACTION action = sp->Parameters.Power.ShutdownType;

        // \_PTS(target) runs before any sleep or shutdown.
        uacpi_status st = uacpi_prepare_for_sleep_state(target);
        if (uacpi_unlikely_error(st))
        {
            UacpiTrace("[acpi] prepare_for_sleep_state(S%u) failed: %s\n",
                      (ULONG)target, uacpi_status_to_string(st));
        }
        else
        {
            UacpiTrace("[acpi] _PTS done for S%u\n", (ULONG)target);
        }

        // uacpi_prepare_for_sleep_state already evaluated \_PTS(target) and
        // \_SI._SST (3 for S1-S3, 4 for S4, 0 for S5), matching acpi.sys.

        // The HAL does the SLP_TYP write and picks off or reset from ShutdownType.
        UacpiTrace("[acpi] system -> S%u (ShutdownType %d); HAL drives the write\n",
                  (ULONG)target, (int)action);

        if (target != UACPI_SLEEP_STATE_S5)
        {
            // Arm only wake GPEs; uacpi_wake_from_sleep_state restores the rest.
            // We do not call uacpi_enter_sleep_state (the HAL owns the SLP_TYP
            // write), so reproduce the same pre-sleep housekeeping it would have
            // done: clear the leftover WAK_STS from a prior resume and every
            // latched GPE/fixed-event status, otherwise a stale bit fires the
            // instant wake GPEs are enabled and bounces the machine back awake.
            (void)uacpi_write_register_field(UACPI_REGISTER_FIELD_WAK_STS,
                                             ACPI_PM1_STS_CLEAR);
            (void)uacpi_disable_all_gpes();
            (void)uacpi_clear_all_events();
            // Mask devices that can only wake from a shallower state than this,
            // so they do not bounce the machine awake from a depth they cannot
            // service; UacpiWakeRestoreSuspended puts them back on resume.
            UacpiWakeSuspendShallow(sys);
            (void)uacpi_enable_all_wake_gpes();
            UacpiPendingSleepState = target;
            // A button press during the resume window is the wake, not a press.
            InterlockedExchange(&UacpiWakeSourceClaimed, 0);
            InterlockedExchange(&UacpiSystemResuming, 1);
        }
        else
        {
            // S5: no wake source. Drop every GPE enable so nothing fires
            // through soft-off (the HAL's EnableDisableGpeEvents(0) also does
            // this, but only if it reaches our callback).
            (void)uacpi_disable_all_gpes();
        }

        // Let the HAL's HIGH_LEVEL callbacks read a current wake-mask snapshot.
        UacpiHalRefreshWakeCache();

        // Drain any GPE/_Qxx/Notify work so nothing is frozen mid-AML (holding
        // an interpreter or EC lock) when the HAL parks the CPUs to sleep.
        (void)uacpi_kernel_wait_for_work_completion();
    }

    PoStartNextPowerIrp(Irp);
    IoSkipCurrentIrpStackLocation(Irp);
    return PoCallDriver(Fdo->LowerDevice, Irp);
}
