/*
 * PROJECT:     ReactOS Qualcomm subsystem manager
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Modem state machine
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "subsys.h"

#define NDEBUG
#include <debug.h>

#define SS_SECONDS(s)               (-(LONGLONG)(s) * 10 * 1000 * 1000)
#define SS_STOP_ACK_SECONDS         10
#define SS_FORCE_STOP_SECONDS       5

typedef struct _SS_POST_CONTEXT
{
    PSS_DEVICE Device;
    ULONG Event;
    ULONG_PTR Param;
} SS_POST_CONTEXT, *PSS_POST_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SS_POST_CONTEXT, SsGetPostContext)

static
ULONG
SsActVoteOn(
    _In_ PSS_DEVICE Device,
    _In_ ULONG Event,
    _In_ ULONG_PTR Param)
{
    UNREFERENCED_PARAMETER(Event);
    UNREFERENCED_PARAMETER(Param);

    return SsPowerVote(Device, SS_VOTE_ON) ? SS_EV_VOTE_ON_DONE : SS_EV_FAILURE;
}

static
ULONG
SsActHandover(
    _In_ PSS_DEVICE Device,
    _In_ ULONG Event,
    _In_ ULONG_PTR Param)
{
    UNREFERENCED_PARAMETER(Event);
    UNREFERENCED_PARAMETER(Param);

    return SsPowerVote(Device, SS_VOTE_HANDOVER) ? SS_EV_HANDOVER_DONE : SS_EV_FAILURE;
}

static
ULONG
SsActVoteOff(
    _In_ PSS_DEVICE Device,
    _In_ ULONG Event,
    _In_ ULONG_PTR Param)
{
    UNREFERENCED_PARAMETER(Event);
    UNREFERENCED_PARAMETER(Param);

    return SsPowerVote(Device, SS_VOTE_OFF) ? SS_EV_VOTE_OFF_DONE : SS_EV_FAILURE;
}

static
VOID
SsStartTimer(
    _In_ PSS_DEVICE Device,
    _In_ ULONG Seconds)
{
    WdfTimerStart(Device->Timer, SS_SECONDS(Seconds));
}

static
ULONG
SsActBoot(
    _In_ PSS_DEVICE Device,
    _In_ ULONG Event,
    _In_ ULONG_PTR Param)
{
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(Event);
    UNREFERENCED_PARAMETER(Param);

    /* Failed boots count against the restart limit too */
    Device->BootAttempts++;
    DPRINT1("[subsys] BOOT attempt %lu (family 0x%lx)\n", Device->BootAttempts, Device->ChipFamily);
    DPRINT1("[subsys]   fw '%ls'\n", Device->FwImagePath);
    DPRINT1("[subsys]   sw '%ls'\n", Device->SwImagePath);

    if (!NT_SUCCESS(SsBroadcast(Device, "ssr:modem:before_powerup")))
        return SS_EV_FAILURE;

    Status = SsBringUpMba(Device);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("[subsys] MBA bring up FAILED 0x%lx\n", Status);
        return SS_EV_MBA_FAILED;
    }
    DPRINT1("[subsys] MBA is up\n");

    /* The MBA unlocked modem memory, the dump of the previous crash would be taken here */
    if (Device->State == SS_ST_REBOOTED_AFTER_SSR && !Device->NoDump)
        DPRINT1("No dump collector, previous modem crash is not saved\n");
    Device->NoDump = FALSE;

    Status = SsLoadModemImage(Device);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("[subsys] modem image load FAILED 0x%lx\n", Status);
        return SS_EV_IMAGE_FAILED;
    }
    DPRINT1("[subsys] modem image authenticated\n");

    if (!NT_SUCCESS(SsPublishState(Device, SS_PEER_BOOTING)))
        return SS_EV_FAILURE;

    Status = SsStartMonitor(Device);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("[subsys] SMP2P monitor start FAILED 0x%lx\n", Status);
        return SS_EV_FAILURE;
    }

    DPRINT1("[subsys] waiting up to %lu s for modem ready/handover\n", Device->LoadTimerSeconds);
    SsStartTimer(Device, Device->LoadTimerSeconds);
    return SS_EV_BOOTED;
}

static
ULONG
SsActImageFailed(
    _In_ PSS_DEVICE Device,
    _In_ ULONG Event,
    _In_ ULONG_PTR Param)
{
    UNREFERENCED_PARAMETER(Event);
    UNREFERENCED_PARAMETER(Param);

    SsHaltAxi(Device);
    KeStallExecutionProcessor(2);
    return SS_EV_HALTED;
}

/* No kernel QMI client exists yet, and QMI never holds up READY */
static
ULONG
SsActQmiStart(
    _In_ PSS_DEVICE Device,
    _In_ ULONG Event,
    _In_ ULONG_PTR Param)
{
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(Event);
    UNREFERENCED_PARAMETER(Param);

    return SS_EV_QMI_STARTED;
}

static
ULONG
SsActPoweredUp(
    _In_ PSS_DEVICE Device,
    _In_ ULONG Event,
    _In_ ULONG_PTR Param)
{
    UNREFERENCED_PARAMETER(Event);
    UNREFERENCED_PARAMETER(Param);

    if (!NT_SUCCESS(SsBroadcast(Device, "ssr:modem:after_powerup")))
        return SS_EV_FAILURE;

    if (!NT_SUCCESS(SsPublishState(Device, SS_PEER_UP)))
        return SS_EV_FAILURE;

    SsAnnounceChildren(Device);
    return SS_EV_POWERED_UP;
}

static
ULONG
SsActStopTimer(
    _In_ PSS_DEVICE Device,
    _In_ ULONG Event,
    _In_ ULONG_PTR Param)
{
    UNREFERENCED_PARAMETER(Event);
    UNREFERENCED_PARAMETER(Param);

    /* If the timer already fired, its timeout event is still on the way */
    return WdfTimerStop(Device->Timer, FALSE) ? SS_EV_TIMER_CANCELLED : SS_EV_NONE;
}

static
ULONG
SsActReady(
    _In_ PSS_DEVICE Device,
    _In_ ULONG Event,
    _In_ ULONG_PTR Param)
{
    UNREFERENCED_PARAMETER(Event);
    UNREFERENCED_PARAMETER(Param);

    DPRINT1("Modem is ready\n");
    SsCopyVersions(Device);
    return SS_EV_NONE;
}

static
ULONG
SsActLoadTimeout(
    _In_ PSS_DEVICE Device,
    _In_ ULONG Event,
    _In_ ULONG_PTR Param)
{
    UNREFERENCED_PARAMETER(Event);
    UNREFERENCED_PARAMETER(Param);

    /* Ask the modem to crash so the restart path collects its state */
    if (SsWriteOutbound(Device, SS_SMP2P_OUT_FORCE_STOP) < 0)
        return SS_EV_FORCE_STOP_FAILED;

    SsStartTimer(Device, SS_FORCE_STOP_SECONDS);
    return SS_EV_FORCE_STOP_SENT;
}

static
ULONG
SsActDecide(
    _In_ PSS_DEVICE Device,
    _In_ ULONG Event,
    _In_ ULONG_PTR Param)
{
    UNREFERENCED_PARAMETER(Event);
    UNREFERENCED_PARAMETER(Param);

    return SsDecideRestart(Device);
}

static
ULONG
SsActDumpStart(
    _In_ PSS_DEVICE Device,
    _In_ ULONG Event,
    _In_ ULONG_PTR Param)
{
    UNREFERENCED_PARAMETER(Event);

    SsMaskWatchdog(Device);
    SsSetFailure(Device, Param);
    if (Device->Failure)
        DPRINT1("Modem failure: %s\n", Device->Failure->Reason);

    return SS_EV_DUMP_STARTED;
}

static
ULONG
SsActPublishCrashed(
    _In_ PSS_DEVICE Device,
    _In_ ULONG Event,
    _In_ ULONG_PTR Param)
{
    UNREFERENCED_PARAMETER(Event);
    UNREFERENCED_PARAMETER(Param);

    return NT_SUCCESS(SsPublishState(Device, SS_PEER_CRASHED)) ? SS_EV_STATE_PUBLISHED : SS_EV_FAILURE;
}

static
ULONG
SsActStopMonitor(
    _In_ PSS_DEVICE Device,
    _In_ ULONG Event,
    _In_ ULONG_PTR Param)
{
    UNREFERENCED_PARAMETER(Event);
    UNREFERENCED_PARAMETER(Param);

    SsStopMonitor(Device);
    return SS_EV_MONITOR_STOPPED;
}

static
ULONG
SsGoingDown(
    _In_ PSS_DEVICE Device)
{
    Device->Acks = 0;

    if (!NT_SUCCESS(SsBroadcast(Device, "ssr:modem:before_shutdown")))
        return SS_EV_BROADCAST_FAILED;

    if (!NT_SUCCESS(SsPublishState(Device, SS_PEER_GOING_DOWN)))
        return SS_EV_BROADCAST_FAILED;

    return SS_EV_STATE_PUBLISHED;
}

static
ULONG
SsActBeforeShutdown(
    _In_ PSS_DEVICE Device,
    _In_ ULONG Event,
    _In_ ULONG_PTR Param)
{
    UNREFERENCED_PARAMETER(Event);
    UNREFERENCED_PARAMETER(Param);

    return SsGoingDown(Device);
}

/* Same as above but stops to wait for the peer acknowledgements */
static
ULONG
SsActBeforeShutdownWait(
    _In_ PSS_DEVICE Device,
    _In_ ULONG Event,
    _In_ ULONG_PTR Param)
{
    ULONG Next;

    UNREFERENCED_PARAMETER(Event);
    UNREFERENCED_PARAMETER(Param);

    Next = SsGoingDown(Device);
    return (Next == SS_EV_STATE_PUBLISHED) ? SS_EV_NONE : Next;
}

static
ULONG
SsActAfterShutdown(
    _In_ PSS_DEVICE Device,
    _In_ ULONG Event,
    _In_ ULONG_PTR Param)
{
    UNREFERENCED_PARAMETER(Event);
    UNREFERENCED_PARAMETER(Param);

    if (!NT_SUCCESS(SsBroadcast(Device, "ssr:modem:after_shutdown")))
        return SS_EV_BROADCAST_FAILED;

    if (!NT_SUCCESS(SsPublishState(Device, SS_PEER_DOWN)))
        return SS_EV_BROADCAST_FAILED;

    return SS_EV_STATE_PUBLISHED;
}

static
ULONG
SsActAck(
    _In_ PSS_DEVICE Device,
    _In_ ULONG Event,
    _In_ ULONG_PTR Param)
{
    UNREFERENCED_PARAMETER(Param);

    /* The two non SMD peers share a bit, either one satisfies it */
    Device->Acks |= (Event == SS_EV_PEER_SMD) ? 0x1 : 0x2;

    if ((Device->Acks & Device->AcksRequired) == Device->AcksRequired)
        return SS_EV_PEERS_DONE;

    return SS_EV_NONE;
}

static
ULONG
SsActChildren(
    _In_ PSS_DEVICE Device,
    _In_ ULONG Event,
    _In_ ULONG_PTR Param)
{
    UNREFERENCED_PARAMETER(Event);
    UNREFERENCED_PARAMETER(Param);

    return SsRemoveChildren(Device);
}

static
ULONG
SsActPowerDown(
    _In_ PSS_DEVICE Device,
    _In_ ULONG Event,
    _In_ ULONG_PTR Param)
{
    UNREFERENCED_PARAMETER(Event);
    UNREFERENCED_PARAMETER(Param);

    SsHaltAxi(Device);
    SsAssertReset(Device);
    SsClocksOff(Device);
    return SS_EV_POWERED_DOWN;
}

static
ULONG
SsActUnload(
    _In_ PSS_DEVICE Device,
    _In_ ULONG Event,
    _In_ ULONG_PTR Param)
{
    UNREFERENCED_PARAMETER(Event);
    UNREFERENCED_PARAMETER(Param);

    return NT_SUCCESS(SsUnloadImages(Device)) ? SS_EV_UNLOADED : SS_EV_FAILURE;
}

/* The SSCTL QMI route needs a kernel QMI client, so only the SMP2P request is sent */
static
ULONG
SsActGracefulStop(
    _In_ PSS_DEVICE Device,
    _In_ ULONG Event,
    _In_ ULONG_PTR Param)
{
    UNREFERENCED_PARAMETER(Event);
    UNREFERENCED_PARAMETER(Param);

    if (SsWriteOutbound(Device, SS_SMP2P_OUT_GRACEFUL_STOP) < 0)
        return SS_EV_FAILURE;

    if (!NT_SUCCESS(SsPublishState(Device, SS_PEER_STOP_REQUESTED)))
        return SS_EV_FAILURE;

    SsStartTimer(Device, SS_STOP_ACK_SECONDS);
    return SS_EV_NONE;
}

static
ULONG
SsActShutdownFlag(
    _In_ PSS_DEVICE Device,
    _In_ ULONG Event,
    _In_ ULONG_PTR Param)
{
    UNREFERENCED_PARAMETER(Event);
    UNREFERENCED_PARAMETER(Param);

    Device->ShutdownInProgress = TRUE;
    return SS_EV_SHUTTING_DOWN;
}

static
ULONG
SsActShutdownDump(
    _In_ PSS_DEVICE Device,
    _In_ ULONG Event,
    _In_ ULONG_PTR Param)
{
    UNREFERENCED_PARAMETER(Event);
    UNREFERENCED_PARAMETER(Param);

    if (!NT_SUCCESS(SsBringUpMba(Device)))
        return SS_EV_MBA_FAILED;

    return SS_EV_SHUTDOWN_DUMP_DONE;
}

static
ULONG
SsActOff(
    _In_ PSS_DEVICE Device,
    _In_ ULONG Event,
    _In_ ULONG_PTR Param)
{
    UNREFERENCED_PARAMETER(Event);
    UNREFERENCED_PARAMETER(Param);

    KeSetEvent(&Device->OffEvent, IO_NO_INCREMENT, FALSE);
    return SS_EV_NONE;
}

static
ULONG
SsActDumpStop(
    _In_ PSS_DEVICE Device,
    _In_ ULONG Event,
    _In_ ULONG_PTR Param)
{
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(Event);
    UNREFERENCED_PARAMETER(Param);

    return SS_EV_DUMP_STOPPED;
}

static
ULONG
SsActFatalError(
    _In_ PSS_DEVICE Device,
    _In_ ULONG Event,
    _In_ ULONG_PTR Param)
{
    UNREFERENCED_PARAMETER(Event);
    UNREFERENCED_PARAMETER(Param);

    SsPowerVote(Device, SS_VOTE_ON);
    SsStopMonitor(Device);
    SsSetFailure(Device, SS_FAULT_STATE_MACHINE);
    SsFatal(Device);
}

static
ULONG
SsActFatalCrash(
    _In_ PSS_DEVICE Device,
    _In_ ULONG Event,
    _In_ ULONG_PTR Param)
{
    UNREFERENCED_PARAMETER(Event);

    SsSetFailure(Device, Param);
    SsFatal(Device);
}

static
ULONG
SsActSysmon(
    _In_ PSS_DEVICE Device,
    _In_ ULONG Event,
    _In_ ULONG_PTR Param)
{
    UNREFERENCED_PARAMETER(Event);

    if (!Param)
        return SS_EV_NO_MESSAGE;

    SsSysmonForward(Device, (PCSTR)Param);
    return SS_EV_REVERT;
}

#define ROW(State, Event, Next, Action) { State, Event, Next, Action }

const SS_TRANSITION SsAmssTable[] =
{
    ROW(SS_ST_INIT, SS_EV_START, SS_ST_VOTING_ON, SsActVoteOn),

    ROW(SS_ST_VOTING_ON, SS_EV_VOTE_ON_DONE, SS_ST_BOOTED, SsActBoot),
    ROW(SS_ST_VOTING_ON, SS_EV_FAILURE, SS_ST_ERROR, SsActHandover),

    ROW(SS_ST_BOOTED, SS_EV_BOOTED, SS_ST_WAIT_MODEM, NULL),
    ROW(SS_ST_BOOTED, SS_EV_MBA_FAILED, SS_ST_WAIT_UNVOTE, SsActHandover),
    ROW(SS_ST_BOOTED, SS_EV_IMAGE_FAILED, SS_ST_IMAGE_FAILED, SsActImageFailed),
    ROW(SS_ST_BOOTED, SS_EV_FAILURE, SS_ST_ERROR, SsActHandover),

    ROW(SS_ST_REBOOTED_AFTER_SSR, SS_EV_BOOTED, SS_ST_WAIT_MODEM, NULL),
    ROW(SS_ST_REBOOTED_AFTER_SSR, SS_EV_MBA_FAILED, SS_ST_REBOOT_MBA_FAILED, SsActDumpStop),
    ROW(SS_ST_REBOOTED_AFTER_SSR, SS_EV_IMAGE_FAILED, SS_ST_IMAGE_FAILED, SsActImageFailed),
    ROW(SS_ST_REBOOTED_AFTER_SSR, SS_EV_FAILURE, SS_ST_ERROR, SsActHandover),

    ROW(SS_ST_IMAGE_FAILED, SS_EV_HALTED, SS_ST_WAIT_UNVOTE, SsActHandover),

    /* Image accepted, waiting for the modem to raise ready and handover */
    ROW(SS_ST_WAIT_MODEM, SS_EV_MODEM_READY, SS_ST_GOT_READY, NULL),
    ROW(SS_ST_WAIT_MODEM, SS_EV_MODEM_HANDOVER, SS_ST_GOT_HANDOVER, SsActHandover),
    ROW(SS_ST_WAIT_MODEM, SS_EV_FATAL, SS_ST_BOOT_ERROR, SsActStopTimer),
    ROW(SS_ST_WAIT_MODEM, SS_EV_TIMEOUT, SS_ST_LOAD_TIMEOUT, SsActLoadTimeout),

    ROW(SS_ST_GOT_READY, SS_EV_MODEM_HANDOVER, SS_ST_BOTH_BITS, SsActHandover),
    ROW(SS_ST_GOT_READY, SS_EV_FATAL, SS_ST_BOOT_ERROR, SsActStopTimer),
    ROW(SS_ST_GOT_READY, SS_EV_TIMEOUT, SS_ST_LOAD_TIMEOUT, SsActLoadTimeout),
    ROW(SS_ST_GOT_READY, SS_EV_BROADCAST, SS_ST_SYSMON_FORWARD, SsActSysmon),

    ROW(SS_ST_GOT_HANDOVER, SS_EV_HANDOVER_DONE, SS_ST_UNVOTED, NULL),
    ROW(SS_ST_GOT_HANDOVER, SS_EV_MODEM_READY, SS_ST_BOTH_BITS, NULL),
    ROW(SS_ST_GOT_HANDOVER, SS_EV_FATAL, SS_ST_BOOT_ERROR_UNVOTE, SsActStopTimer),
    ROW(SS_ST_GOT_HANDOVER, SS_EV_TIMEOUT, SS_ST_LOAD_TIMEOUT_UNVOTE, SsActLoadTimeout),

    ROW(SS_ST_UNVOTED, SS_EV_MODEM_READY, SS_ST_STARTING_QMI, SsActQmiStart),
    ROW(SS_ST_UNVOTED, SS_EV_FATAL, SS_ST_BOOT_ERROR_WAIT, SsActStopTimer),
    ROW(SS_ST_UNVOTED, SS_EV_TIMEOUT, SS_ST_LOAD_TIMEOUT_UNVOTED, SsActLoadTimeout),
    ROW(SS_ST_UNVOTED, SS_EV_BROADCAST, SS_ST_SYSMON_FORWARD, SsActSysmon),

    ROW(SS_ST_BOTH_BITS, SS_EV_HANDOVER_DONE, SS_ST_STARTING_QMI, SsActQmiStart),
    ROW(SS_ST_BOTH_BITS, SS_EV_FAILURE, SS_ST_ERROR, SsActFatalError),
    ROW(SS_ST_BOTH_BITS, SS_EV_FATAL, SS_ST_BOOT_ERROR_UNVOTE, SsActStopTimer),
    ROW(SS_ST_BOTH_BITS, SS_EV_TIMEOUT, SS_ST_LOAD_TIMEOUT_UNVOTE, SsActLoadTimeout),
    ROW(SS_ST_BOTH_BITS, SS_EV_BROADCAST, SS_ST_SYSMON_FORWARD, SsActSysmon),

    ROW(SS_ST_STARTING_QMI, SS_EV_FAILURE, SS_ST_ERROR, SsActFatalError),
    ROW(SS_ST_STARTING_QMI, SS_EV_QMI_STARTED, SS_ST_POWERED_UP, SsActPoweredUp),

    ROW(SS_ST_POWERED_UP, SS_EV_FAILURE, SS_ST_ERROR, SsActFatalError),
    ROW(SS_ST_POWERED_UP, SS_EV_POWERED_UP, SS_ST_CANCEL_TIMER, SsActStopTimer),

    ROW(SS_ST_CANCEL_TIMER, SS_EV_FATAL, SS_ST_BOOT_ERROR_WAIT, NULL),
    ROW(SS_ST_CANCEL_TIMER, SS_EV_TIMEOUT, SS_ST_READY, SsActReady),
    ROW(SS_ST_CANCEL_TIMER, SS_EV_TIMER_CANCELLED, SS_ST_READY, SsActReady),
    ROW(SS_ST_CANCEL_TIMER, SS_EV_BROADCAST, SS_ST_SYSMON_FORWARD, SsActSysmon),

    ROW(SS_ST_READY, SS_EV_FATAL, SS_ST_DECIDE, SsActDecide),
    ROW(SS_ST_READY, SS_EV_RESTART_REQUEST, SS_ST_RESTART_REQUEST, SsActChildren),
    ROW(SS_ST_READY, SS_EV_APPS_SHUTDOWN, SS_ST_SHUTDOWN_REQUEST, SsActShutdownFlag),
    ROW(SS_ST_READY, SS_EV_BROADCAST, SS_ST_SYSMON_FORWARD, SsActSysmon),

    ROW(SS_ST_BOOT_ERROR, SS_EV_TIMER_CANCELLED, SS_ST_WAIT_UNVOTE_FATAL, SsActHandover),
    ROW(SS_ST_BOOT_ERROR, SS_EV_TIMEOUT, SS_ST_WAIT_UNVOTE_FATAL, SsActHandover),

    ROW(SS_ST_BOOT_ERROR_UNVOTE, SS_EV_TIMEOUT, SS_ST_WAIT_UNVOTE, NULL),
    ROW(SS_ST_BOOT_ERROR_UNVOTE, SS_EV_TIMER_CANCELLED, SS_ST_WAIT_UNVOTE, NULL),
    ROW(SS_ST_BOOT_ERROR_UNVOTE, SS_EV_HANDOVER_DONE, SS_ST_BOOT_ERROR_WAIT, NULL),

    ROW(SS_ST_BOOT_ERROR_WAIT, SS_EV_TIMEOUT, SS_ST_DECIDE, SsActDecide),

    ROW(SS_ST_LOAD_TIMEOUT, SS_EV_FATAL, SS_ST_BOOT_ERROR, SsActStopTimer),
    ROW(SS_ST_LOAD_TIMEOUT, SS_EV_TIMEOUT, SS_ST_WAIT_UNVOTE_FATAL, SsActHandover),

    ROW(SS_ST_LOAD_TIMEOUT_UNVOTED, SS_EV_FATAL, SS_ST_BOOT_ERROR_WAIT, SsActStopTimer),
    ROW(SS_ST_LOAD_TIMEOUT_UNVOTED, SS_EV_TIMEOUT, SS_ST_DECIDE, SsActDecide),

    ROW(SS_ST_LOAD_TIMEOUT_UNVOTE, SS_EV_FATAL, SS_ST_BOOT_ERROR_UNVOTE, SsActStopTimer),
    ROW(SS_ST_LOAD_TIMEOUT_UNVOTE, SS_EV_TIMEOUT, SS_ST_WAIT_UNVOTE, NULL),
    ROW(SS_ST_LOAD_TIMEOUT_UNVOTE, SS_EV_HANDOVER_DONE, SS_ST_LOAD_TIMEOUT_UNVOTED, NULL),

    ROW(SS_ST_WAIT_UNVOTE, SS_EV_HANDOVER_DONE, SS_ST_DECIDE, SsActDecide),

    ROW(SS_ST_WAIT_UNVOTE_FATAL, SS_EV_HANDOVER_DONE, SS_ST_DECIDE, SsActDecide),
    ROW(SS_ST_WAIT_UNVOTE_FATAL, SS_EV_FAILURE, SS_ST_ERROR, SsActFatalError),

    ROW(SS_ST_DECIDE, SS_EV_RESTART_OK, SS_ST_DUMP_STARTED, SsActDumpStart),
    ROW(SS_ST_DECIDE, SS_EV_RESTART_REFUSED, SS_ST_FATAL, SsActPublishCrashed),

    ROW(SS_ST_FATAL, SS_EV_STATE_PUBLISHED, SS_ST_FATAL, SsActVoteOn),
    ROW(SS_ST_FATAL, SS_EV_VOTE_ON_DONE, SS_ST_FATAL, SsActFatalCrash),

    ROW(SS_ST_DUMP_STARTED, SS_EV_DUMP_STARTED, SS_ST_SSR_STOPPING, SsActStopMonitor),

    ROW(SS_ST_DUMP_ABORT, SS_EV_DUMP_STOPPED, SS_ST_ERROR, SsActFatalError),

    ROW(SS_ST_REBOOT_MBA_FAILED, SS_EV_DUMP_STOPPED, SS_ST_DECIDE, SsActDecide),

    ROW(SS_ST_SSR_STOPPING, SS_EV_MONITOR_STOPPED, SS_ST_SSR_STOPPING, SsActPublishCrashed),
    ROW(SS_ST_SSR_STOPPING, SS_EV_FAILURE, SS_ST_DUMP_ABORT, SsActDumpStop),
    ROW(SS_ST_SSR_STOPPING, SS_EV_STATE_PUBLISHED, SS_ST_SSR_WAIT_ACKS, SsActBeforeShutdown),

    ROW(SS_ST_RESTART_POWER_CYCLE, SS_EV_POWERED_DOWN, SS_ST_RESTART_POWER_CYCLE, SsActVoteOff),
    ROW(SS_ST_RESTART_POWER_CYCLE, SS_EV_VOTE_OFF_DONE, SS_ST_RESTART_POWER_CYCLE, SsActAfterShutdown),
    ROW(SS_ST_RESTART_POWER_CYCLE, SS_EV_STATE_PUBLISHED, SS_ST_RESTART_POWER_CYCLE, SsActVoteOn),
    ROW(SS_ST_RESTART_POWER_CYCLE, SS_EV_VOTE_ON_DONE, SS_ST_BOOTED, SsActBoot),
    ROW(SS_ST_RESTART_POWER_CYCLE, SS_EV_FAILURE, SS_ST_ERROR, SsActFatalError),

    ROW(SS_ST_SSR_POWER_CYCLE, SS_EV_POWERED_DOWN, SS_ST_SSR_POWER_CYCLE, SsActVoteOff),
    ROW(SS_ST_SSR_POWER_CYCLE, SS_EV_VOTE_OFF_DONE, SS_ST_SSR_POWER_CYCLE, SsActAfterShutdown),
    ROW(SS_ST_SSR_POWER_CYCLE, SS_EV_STATE_PUBLISHED, SS_ST_SSR_POWER_CYCLE, SsActVoteOn),
    ROW(SS_ST_SSR_POWER_CYCLE, SS_EV_VOTE_ON_DONE, SS_ST_REBOOTED_AFTER_SSR, SsActBoot),
    ROW(SS_ST_SSR_POWER_CYCLE, SS_EV_FAILURE, SS_ST_DUMP_ABORT, SsActDumpStop),

    ROW(SS_ST_SSR_WAIT_ACKS, SS_EV_STATE_PUBLISHED, SS_ST_SSR_WAIT_ACKS, NULL),
    ROW(SS_ST_SSR_WAIT_ACKS, SS_EV_FAILURE, SS_ST_DUMP_ABORT, SsActDumpStop),
    ROW(SS_ST_SSR_WAIT_ACKS, SS_EV_PEER_SMD, SS_ST_SSR_WAIT_ACKS, SsActAck),
    ROW(SS_ST_SSR_WAIT_ACKS, SS_EV_PEER_IPA, SS_ST_SSR_WAIT_ACKS, SsActAck),
    ROW(SS_ST_SSR_WAIT_ACKS, SS_EV_PEER_OTHER, SS_ST_SSR_WAIT_ACKS, SsActAck),
    ROW(SS_ST_SSR_WAIT_ACKS, SS_EV_PEERS_DONE, SS_ST_SSR_WAIT_CHILDREN, SsActChildren),

    ROW(SS_ST_SSR_WAIT_CHILDREN, SS_EV_CHILDREN_GONE, SS_ST_SSR_POWER_CYCLE, SsActPowerDown),

    ROW(SS_ST_RESTART_REQUEST, SS_EV_CHILDREN_GONE, SS_ST_RESTART_REQUEST, SsActGracefulStop),
    ROW(SS_ST_RESTART_REQUEST, SS_EV_STOP_ACK, SS_ST_RESTART_STOPPING, SsActStopTimer),
    ROW(SS_ST_RESTART_REQUEST, SS_EV_SHUTDOWN_READY, SS_ST_RESTART_STOPPING, SsActStopTimer),
    ROW(SS_ST_RESTART_REQUEST, SS_EV_FATAL, SS_ST_RESTART_CRASH, SsActStopTimer),
    ROW(SS_ST_RESTART_REQUEST, SS_EV_TIMEOUT, SS_ST_RESTART_FORCE_STOP, SsActLoadTimeout),
    ROW(SS_ST_RESTART_REQUEST, SS_EV_FAILURE, SS_ST_ERROR, SsActFatalError),

    ROW(SS_ST_SHUTDOWN_REQUEST, SS_EV_SHUTTING_DOWN, SS_ST_SHUTDOWN_REQUEST, SsActChildren),
    ROW(SS_ST_SHUTDOWN_REQUEST, SS_EV_CHILDREN_GONE, SS_ST_SHUTDOWN_REQUEST, SsActGracefulStop),
    ROW(SS_ST_SHUTDOWN_REQUEST, SS_EV_STOP_ACK, SS_ST_SHUTDOWN_STOPPING, SsActStopTimer),
    ROW(SS_ST_SHUTDOWN_REQUEST, SS_EV_SHUTDOWN_READY, SS_ST_SHUTDOWN_STOPPING, SsActStopTimer),
    ROW(SS_ST_SHUTDOWN_REQUEST, SS_EV_FATAL, SS_ST_SHUTDOWN_CRASH, SsActStopTimer),
    ROW(SS_ST_SHUTDOWN_REQUEST, SS_EV_TIMEOUT, SS_ST_SHUTDOWN_FORCE_STOP, SsActLoadTimeout),
    ROW(SS_ST_SHUTDOWN_REQUEST, SS_EV_FAILURE, SS_ST_ERROR, SsActFatalError),

    ROW(SS_ST_RESTART_STOPPING, SS_EV_TIMER_CANCELLED, SS_ST_RESTART_STOPPING, SsActStopMonitor),
    ROW(SS_ST_RESTART_STOPPING, SS_EV_TIMEOUT, SS_ST_RESTART_STOPPING, SsActStopMonitor),
    ROW(SS_ST_RESTART_STOPPING, SS_EV_MONITOR_STOPPED, SS_ST_RESTART_STOPPING, SsActAfterShutdown),
    ROW(SS_ST_RESTART_STOPPING, SS_EV_STATE_PUBLISHED, SS_ST_RESTART_STOPPING, SsActBeforeShutdownWait),
    ROW(SS_ST_RESTART_STOPPING, SS_EV_PEER_SMD, SS_ST_RESTART_STOPPING, SsActAck),
    ROW(SS_ST_RESTART_STOPPING, SS_EV_PEER_IPA, SS_ST_RESTART_STOPPING, SsActAck),
    ROW(SS_ST_RESTART_STOPPING, SS_EV_PEER_OTHER, SS_ST_RESTART_STOPPING, SsActAck),
    ROW(SS_ST_RESTART_STOPPING, SS_EV_PEERS_DONE, SS_ST_RESTART_STOPPING, SsActUnload),
    ROW(SS_ST_RESTART_STOPPING, SS_EV_UNLOADED, SS_ST_RESTART_POWER_CYCLE, SsActPowerDown),
    ROW(SS_ST_RESTART_STOPPING, SS_EV_FAILURE, SS_ST_ERROR, SsActFatalError),

    ROW(SS_ST_SHUTDOWN_STOPPING, SS_EV_TIMER_CANCELLED, SS_ST_SHUTDOWN_STOPPING, SsActStopMonitor),
    ROW(SS_ST_SHUTDOWN_STOPPING, SS_EV_TIMEOUT, SS_ST_SHUTDOWN_STOPPING, SsActStopMonitor),
    ROW(SS_ST_SHUTDOWN_STOPPING, SS_EV_MONITOR_STOPPED, SS_ST_SHUTDOWN_STOPPING, SsActAfterShutdown),
    ROW(SS_ST_SHUTDOWN_STOPPING, SS_EV_STATE_PUBLISHED, SS_ST_SHUTDOWN_STOPPING, SsActBeforeShutdownWait),
    ROW(SS_ST_SHUTDOWN_STOPPING, SS_EV_PEER_SMD, SS_ST_SHUTDOWN_STOPPING, SsActAck),
    ROW(SS_ST_SHUTDOWN_STOPPING, SS_EV_PEER_IPA, SS_ST_SHUTDOWN_STOPPING, SsActAck),
    ROW(SS_ST_SHUTDOWN_STOPPING, SS_EV_PEER_OTHER, SS_ST_SHUTDOWN_STOPPING, SsActAck),
    ROW(SS_ST_SHUTDOWN_STOPPING, SS_EV_PEERS_DONE, SS_ST_SHUTDOWN_STOPPING, SsActPowerDown),
    ROW(SS_ST_SHUTDOWN_STOPPING, SS_EV_POWERED_DOWN, SS_ST_SHUTDOWN_STOPPING, SsActVoteOff),
    ROW(SS_ST_SHUTDOWN_STOPPING, SS_EV_VOTE_OFF_DONE, SS_ST_OFF, SsActOff),
    ROW(SS_ST_SHUTDOWN_STOPPING, SS_EV_FAILURE, SS_ST_ERROR, SsActFatalError),

    ROW(SS_ST_RESTART_FORCE_STOP, SS_EV_FORCE_STOP_FAILED, SS_ST_RESTART_CRASH, SsActDecide),
    ROW(SS_ST_RESTART_FORCE_STOP, SS_EV_FATAL, SS_ST_RESTART_CRASH, SsActStopTimer),
    ROW(SS_ST_RESTART_FORCE_STOP, SS_EV_TIMEOUT, SS_ST_RESTART_CRASH, SsActDecide),

    ROW(SS_ST_RESTART_CRASH, SS_EV_TIMER_CANCELLED, SS_ST_RESTART_CRASH, SsActDecide),
    ROW(SS_ST_RESTART_CRASH, SS_EV_TIMEOUT, SS_ST_RESTART_CRASH, SsActDecide),
    ROW(SS_ST_RESTART_CRASH, SS_EV_RESTART_REFUSED, SS_ST_FATAL, SsActPublishCrashed),
    ROW(SS_ST_RESTART_CRASH, SS_EV_RESTART_OK, SS_ST_RESTART_CRASH, SsActDumpStart),
    ROW(SS_ST_RESTART_CRASH, SS_EV_DUMP_STARTED, SS_ST_RESTART_CRASH, SsActStopMonitor),
    ROW(SS_ST_RESTART_CRASH, SS_EV_MONITOR_STOPPED, SS_ST_RESTART_CRASH, SsActPublishCrashed),
    ROW(SS_ST_RESTART_CRASH, SS_EV_STATE_PUBLISHED, SS_ST_RESTART_CRASH, SsActBeforeShutdownWait),
    ROW(SS_ST_RESTART_CRASH, SS_EV_PEER_SMD, SS_ST_RESTART_CRASH, SsActAck),
    ROW(SS_ST_RESTART_CRASH, SS_EV_PEER_IPA, SS_ST_RESTART_CRASH, SsActAck),
    ROW(SS_ST_RESTART_CRASH, SS_EV_PEER_OTHER, SS_ST_RESTART_CRASH, SsActAck),
    ROW(SS_ST_RESTART_CRASH, SS_EV_PEERS_DONE, SS_ST_RESTART_CRASH, SsActUnload),
    ROW(SS_ST_RESTART_CRASH, SS_EV_UNLOADED, SS_ST_SSR_POWER_CYCLE, SsActPowerDown),
    ROW(SS_ST_RESTART_CRASH, SS_EV_FAILURE, SS_ST_DUMP_ABORT, SsActDumpStop),

    ROW(SS_ST_SHUTDOWN_FORCE_STOP, SS_EV_FORCE_STOP_FAILED, SS_ST_SHUTDOWN_CRASH, SsActDecide),
    ROW(SS_ST_SHUTDOWN_FORCE_STOP, SS_EV_FATAL, SS_ST_SHUTDOWN_CRASH, SsActStopTimer),
    ROW(SS_ST_SHUTDOWN_FORCE_STOP, SS_EV_TIMEOUT, SS_ST_SHUTDOWN_CRASH, SsActDecide),

    ROW(SS_ST_SHUTDOWN_CRASH, SS_EV_TIMER_CANCELLED, SS_ST_SHUTDOWN_CRASH, SsActDecide),
    ROW(SS_ST_SHUTDOWN_CRASH, SS_EV_TIMEOUT, SS_ST_SHUTDOWN_CRASH, SsActDecide),
    ROW(SS_ST_SHUTDOWN_CRASH, SS_EV_RESTART_REFUSED, SS_ST_FATAL, SsActPublishCrashed),
    ROW(SS_ST_SHUTDOWN_CRASH, SS_EV_RESTART_OK, SS_ST_SHUTDOWN_CRASH, SsActDumpStart),
    ROW(SS_ST_SHUTDOWN_CRASH, SS_EV_DUMP_STARTED, SS_ST_SHUTDOWN_CRASH, SsActStopMonitor),
    ROW(SS_ST_SHUTDOWN_CRASH, SS_EV_MONITOR_STOPPED, SS_ST_SHUTDOWN_CRASH, SsActPublishCrashed),
    ROW(SS_ST_SHUTDOWN_CRASH, SS_EV_STATE_PUBLISHED, SS_ST_SHUTDOWN_CRASH, SsActBeforeShutdownWait),
    ROW(SS_ST_SHUTDOWN_CRASH, SS_EV_PEER_SMD, SS_ST_SHUTDOWN_CRASH, SsActAck),
    ROW(SS_ST_SHUTDOWN_CRASH, SS_EV_PEER_IPA, SS_ST_SHUTDOWN_CRASH, SsActAck),
    ROW(SS_ST_SHUTDOWN_CRASH, SS_EV_PEER_OTHER, SS_ST_SHUTDOWN_CRASH, SsActAck),
    ROW(SS_ST_SHUTDOWN_CRASH, SS_EV_PEERS_DONE, SS_ST_SHUTDOWN_CRASH, SsActPowerDown),
    ROW(SS_ST_SHUTDOWN_CRASH, SS_EV_POWERED_DOWN, SS_ST_SHUTDOWN_DUMP, SsActVoteOn),
    ROW(SS_ST_SHUTDOWN_CRASH, SS_EV_FAILURE, SS_ST_ERROR, SsActFatalError),

    ROW(SS_ST_SHUTDOWN_DUMP, SS_EV_VOTE_ON_DONE, SS_ST_SHUTDOWN_DUMP, SsActShutdownDump),
    ROW(SS_ST_SHUTDOWN_DUMP, SS_EV_SHUTDOWN_DUMP_DONE, SS_ST_SHUTDOWN_DUMP, SsActPowerDown),
    ROW(SS_ST_SHUTDOWN_DUMP, SS_EV_MBA_FAILED, SS_ST_SHUTDOWN_DUMP, SsActPowerDown),
    ROW(SS_ST_SHUTDOWN_DUMP, SS_EV_SHUTDOWN_DUMP_FAILED, SS_ST_SHUTDOWN_DUMP, SsActPowerDown),
    ROW(SS_ST_SHUTDOWN_DUMP, SS_EV_POWERED_DOWN, SS_ST_SHUTDOWN_DUMP, SsActHandover),
    ROW(SS_ST_SHUTDOWN_DUMP, SS_EV_HANDOVER_DONE, SS_ST_SHUTDOWN_DUMP, SsActVoteOff),
    ROW(SS_ST_SHUTDOWN_DUMP, SS_EV_VOTE_OFF_DONE, SS_ST_OFF, SsActOff),
    ROW(SS_ST_SHUTDOWN_DUMP, SS_EV_FAILURE, SS_ST_ERROR, SsActFatalError),

    ROW(SS_ST_ERROR, SS_EV_HANDOVER_DONE, SS_ST_ERROR, SsActFatalError),

    ROW(-1, 0, 0, NULL)
};

static
const SS_TRANSITION *
SsFindRow(
    _In_ LONG State,
    _In_ ULONG Event)
{
    const SS_TRANSITION *Row;

    for (Row = SsAmssTable; Row->State != -1; Row++)
    {
        if (Row->State == State && Row->Event == (LONG)Event)
            return Row;
    }

    return NULL;
}

static
VOID
SsRecord(
    _Inout_ PSS_DEVICE Device,
    _In_ ULONG Event)
{
    SS_HISTORY *Entry = &Device->History[Device->HistoryNext % SS_HISTORY_DEPTH];

    Entry->Event = Event;
    Entry->State = Device->State;
    Entry->Time = KeQueryInterruptTime();
    Device->HistoryNext++;
}

/* Runs one event and every event its actions chain into, all under the state lock */
VOID
SsDispatch(
    _In_ PSS_DEVICE Device,
    _In_ ULONG Event,
    _In_ ULONG_PTR Param)
{
    const SS_TRANSITION *Row;
    LONG Previous;

    WdfWaitLockAcquire(Device->FsmLock, NULL);

    DPRINT1("[subsys] dispatch begin state=%ld event=%lu param=0x%Ix\n", Device->State, Event, Param);

    while (Event != SS_EV_NONE)
    {
        Row = SsFindRow(Device->State, Event);
        if (!Row)
        {
            DPRINT1("[subsys] state %ld DROPS event %lu\n", Device->State, Event);
            break;
        }

        Previous = Device->State;
        Device->State = Row->Next;
        SsRecord(Device, Event);
        DPRINT1("[subsys] state %ld -> %ld on event %lu%s\n",
                Previous, Row->Next, Event, Row->Action ? " (action)" : "");

        if (!Row->Action)
            break;

        Event = Row->Action(Device, Event, Param);
        DPRINT1("[subsys] action in state %ld returned event %lu\n", Device->State, Event);
        if (Event == SS_EV_REVERT)
        {
            Device->State = Previous;
            DPRINT1("[subsys] reverting to state %ld\n", Previous);
            break;
        }
    }

    DPRINT1("[subsys] dispatch end state=%ld\n", Device->State);

    WdfWaitLockRelease(Device->FsmLock);
}

static
VOID
SsPostWorker(
    _In_ WDFWORKITEM WorkItem)
{
    PSS_POST_CONTEXT Context = SsGetPostContext(WorkItem);

    SsDispatch(Context->Device, Context->Event, Context->Param);
    WdfObjectDelete(WorkItem);
}

/* Queues the event for a passive level worker */
NTSTATUS
SsPost(
    _In_ PSS_DEVICE Device,
    _In_ ULONG Event,
    _In_ ULONG_PTR Param)
{
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDF_WORKITEM_CONFIG Config;
    PSS_POST_CONTEXT Context;
    WDFWORKITEM WorkItem;
    NTSTATUS Status;

    WDF_WORKITEM_CONFIG_INIT(&Config, SsPostWorker);
    Config.AutomaticSerialization = FALSE;

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, SS_POST_CONTEXT);
    Attributes.ParentObject = Device->WdfDevice;

    Status = WdfWorkItemCreate(&Config, &Attributes, &WorkItem);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Lost event %lu, 0x%lx\n", Event, Status);
        return Status;
    }

    Context = SsGetPostContext(WorkItem);
    Context->Device = Device;
    Context->Event = Event;
    Context->Param = Param;
    DPRINT1("[subsys] post event %lu param 0x%Ix (state %ld)\n", Event, Param, Device->State);
    WdfWorkItemEnqueue(WorkItem);
    return STATUS_SUCCESS;
}
