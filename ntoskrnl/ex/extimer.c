/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Executive timers allocated with ExAllocateTimer
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* DEFINES ********************************************************************/

/* TIMER_OR_DPC_INVALID parameters raised for a misused executive timer */
#define EXP_TIMER_BUGCHECK_EX_TIMER         9
#define EXP_TIMER_BAD_ATTRIBUTES            0
#define EXP_TIMER_BAD_PARAMETERS            1
#define EXP_TIMER_ABSOLUTE_HIGH_RESOLUTION  2
#define EXP_TIMER_NEGATIVE_PERIOD           4

#define EXP_TIMER_VALID_ATTRIBUTES \
    (EX_TIMER_HIGH_RESOLUTION | EX_TIMER_NO_WAKE | EX_TIMER_NOTIFICATION)

typedef struct _EX_TIMER
{
    KTIMER KeTimer; /* First, so callers can wait on the timer itself */
    KDPC Dpc;
    WORK_QUEUE_ITEM DeleteWorkItem;
    KSPIN_LOCK Lock;
    PEXT_CALLBACK Callback;
    PVOID CallbackContext;
    PEXT_DELETE_CALLBACK DeleteCallback;
    PVOID DeleteContext;
    ULONG Attributes;
    ULONG Signature;
    ULONG_PTR Generation;
    BOOLEAN IsArmed;
    BOOLEAN IsPeriodic;
    BOOLEAN IsDeleting;
    BOOLEAN DeleteOnExpiration;
    BOOLEAN IsDeleteQueued;
} EX_TIMER;

/* PRIVATE FUNCTIONS **********************************************************/

static
VOID
ExpCheckTimer(
    _In_ PEX_TIMER Timer)
{
    if (Timer->Signature != TAG_EX_TIMER)
    {
        KeBugCheckEx(DRIVER_CAUGHT_MODIFYING_FREED_POOL,
                     (ULONG_PTR)Timer,
                     1,
                     ExGetPreviousMode(),
                     0);
    }
}

static
LONG
ExpPeriodToMilliseconds(
    _In_ LONGLONG Period)
{
    LONGLONG Milliseconds;

    if (Period == 0)
        return 0;

    /* Round up so a period below one millisecond still repeats */
    Milliseconds = Period / 10000;
    if ((Period % 10000) != 0)
        Milliseconds++;

    return (Milliseconds > MAXLONG) ? MAXLONG : (LONG)Milliseconds;
}

/**
 * @brief
 * Stops a pending expiration and any callback it has not started yet.
 * The timer lock must be held.
 *
 * @return
 * TRUE if the timer was set.
 */
static
BOOLEAN
ExpDisarmTimer(
    _Inout_ PEX_TIMER Timer)
{
    BOOLEAN WasSet = Timer->IsArmed;

    KeCancelTimer(&Timer->KeTimer);
    KeRemoveQueueDpc(&Timer->Dpc);
    Timer->IsArmed = FALSE;

    return WasSet;
}

/**
 * @brief
 * Runs the delete callback and frees the timer once no expiration can still
 * be running against it.
 */
static
VOID
ExpFinishTimerDelete(
    _Inout_ PEX_TIMER Timer)
{
    KIRQL OldIrql;

    PAGED_CODE();

    KeCancelTimer(&Timer->KeTimer);
    KeRemoveQueueDpc(&Timer->Dpc);
    KeFlushQueuedDpcs();

    if (Timer->DeleteCallback)
    {
        KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);
        Timer->DeleteCallback(Timer->DeleteContext);
        KeLowerIrql(OldIrql);
    }

    Timer->Signature = ~TAG_EX_TIMER;
    ExFreePoolWithTag(Timer, TAG_EX_TIMER);
}

static
VOID
NTAPI
ExpTimerDeleteWorker(
    _In_ PVOID Parameter)
{
    ExpFinishTimerDelete(Parameter);
}

static
VOID
NTAPI
ExpTimerDpcRoutine(
    _In_ PKDPC Dpc,
    _In_opt_ PVOID DeferredContext,
    _In_opt_ PVOID SystemArgument1,
    _In_opt_ PVOID SystemArgument2)
{
    PEX_TIMER Timer = CONTAINING_RECORD(Dpc, EX_TIMER, Dpc);
    BOOLEAN IsCurrent;
    BOOLEAN QueueDelete = FALSE;

    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);

    /* An expiration left over from a cancel or an earlier arming does not call back */
    KeAcquireSpinLockAtDpcLevel(&Timer->Lock);
    IsCurrent = Timer->IsArmed && ((ULONG_PTR)DeferredContext == Timer->Generation);
    if (IsCurrent && !Timer->IsPeriodic)
        Timer->IsArmed = FALSE;
    KeReleaseSpinLockFromDpcLevel(&Timer->Lock);

    if (!IsCurrent)
        return;

    if (Timer->Callback)
        Timer->Callback(Timer, Timer->CallbackContext);

    /* A delete that did not cancel waited for this expiration */
    KeAcquireSpinLockAtDpcLevel(&Timer->Lock);
    if (Timer->DeleteOnExpiration && !Timer->IsDeleteQueued)
    {
        Timer->IsArmed = FALSE;
        Timer->IsDeleteQueued = TRUE;
        QueueDelete = TRUE;
    }
    KeReleaseSpinLockFromDpcLevel(&Timer->Lock);

    if (QueueDelete)
    {
        KeCancelTimer(&Timer->KeTimer);
        ExQueueWorkItem(&Timer->DeleteWorkItem, DelayedWorkQueue);
    }
}

/* PUBLIC FUNCTIONS ***********************************************************/

/**
 * @brief
 * Allocates an executive timer.
 *
 * @param[in] Callback
 * Optional routine called at DISPATCH_LEVEL each time the timer expires.
 *
 * @param[in] CallbackContext
 * Context handed to the callback.
 *
 * @param[in] Attributes
 * EX_TIMER_* flags. A synchronization timer is created unless
 * EX_TIMER_NOTIFICATION is given.
 *
 * @return
 * The new timer, or NULL if no memory was available.
 */
PEX_TIMER
NTAPI
ExAllocateTimer(
    _In_opt_ PEXT_CALLBACK Callback,
    _In_opt_ PVOID CallbackContext,
    _In_ ULONG Attributes)
{
    PEX_TIMER Timer;

    if ((Attributes & ~EXP_TIMER_VALID_ATTRIBUTES) ||
        ((Attributes & EX_TIMER_HIGH_RESOLUTION) && (Attributes & EX_TIMER_NO_WAKE)))
    {
        KeBugCheckEx(TIMER_OR_DPC_INVALID,
                     EXP_TIMER_BUGCHECK_EX_TIMER,
                     EXP_TIMER_BAD_ATTRIBUTES,
                     Attributes,
                     0);
    }

    Timer = ExAllocatePoolWithTag(NonPagedPoolNx, sizeof(*Timer), TAG_EX_TIMER);
    if (!Timer)
        return NULL;

    RtlZeroMemory(Timer, sizeof(*Timer));
    KeInitializeTimerEx(&Timer->KeTimer,
                        (Attributes & EX_TIMER_NOTIFICATION) ? NotificationTimer :
                                                               SynchronizationTimer);
    KeInitializeDpc(&Timer->Dpc, ExpTimerDpcRoutine, NULL);
    ExInitializeWorkItem(&Timer->DeleteWorkItem, ExpTimerDeleteWorker, Timer);
    KeInitializeSpinLock(&Timer->Lock);
    Timer->Callback = Callback;
    Timer->CallbackContext = CallbackContext;
    Timer->Attributes = Attributes;
    Timer->Signature = TAG_EX_TIMER;

    return Timer;
}

/**
 * @brief
 * Arms an executive timer, replacing any earlier setting.
 *
 * @param[in] Timer
 * The timer returned by ExAllocateTimer().
 *
 * @param[in] DueTime
 * Expiration time in 100 nanosecond units. Negative values are relative.
 *
 * @param[in] Period
 * Period of a recurring timer in 100 nanosecond units, or zero for a one shot
 * timer.
 *
 * @param[in] Parameters
 * Optional extended parameters.
 *
 * @return
 * TRUE if the timer was already set.
 *
 * @remarks
 * High resolution timers expire on the regular clock tick.
 */
BOOLEAN
NTAPI
ExSetTimer(
    _In_ PEX_TIMER Timer,
    _In_ LONGLONG DueTime,
    _In_ LONGLONG Period,
    _In_opt_ PEXT_SET_PARAMETERS Parameters)
{
    LARGE_INTEGER KeDueTime;
    KIRQL OldIrql;
    LONG PeriodMs;
    BOOLEAN WasSet;

    if ((DueTime > 0) && (Timer->Attributes & EX_TIMER_HIGH_RESOLUTION))
    {
        KeBugCheckEx(TIMER_OR_DPC_INVALID,
                     EXP_TIMER_BUGCHECK_EX_TIMER,
                     EXP_TIMER_ABSOLUTE_HIGH_RESOLUTION,
                     (ULONG_PTR)&DueTime,
                     0);
    }

    if (Period < 0)
    {
        KeBugCheckEx(TIMER_OR_DPC_INVALID,
                     EXP_TIMER_BUGCHECK_EX_TIMER,
                     EXP_TIMER_NEGATIVE_PERIOD,
                     (ULONG_PTR)&Period,
                     0);
    }

    if (Parameters &&
        ((Parameters->Version != 0) ||
         (Parameters->NoWakeTolerance < EX_TIMER_UNLIMITED_TOLERANCE)))
    {
        KeBugCheckEx(TIMER_OR_DPC_INVALID,
                     EXP_TIMER_BUGCHECK_EX_TIMER,
                     EXP_TIMER_BAD_PARAMETERS,
                     (ULONG_PTR)Parameters,
                     0);
    }

    ExpCheckTimer(Timer);

    KeDueTime.QuadPart = DueTime;
    PeriodMs = ExpPeriodToMilliseconds(Period);

    KeAcquireSpinLock(&Timer->Lock, &OldIrql);

    /* A timer being deleted can no longer be set */
    if (Timer->IsDeleting)
    {
        KeReleaseSpinLock(&Timer->Lock, OldIrql);
        return FALSE;
    }

    WasSet = Timer->IsArmed;
    KeRemoveQueueDpc(&Timer->Dpc);

    Timer->Generation++;
    Timer->Dpc.DeferredContext = (PVOID)Timer->Generation;
    Timer->IsArmed = TRUE;
    Timer->IsPeriodic = (PeriodMs != 0);
    KeSetTimerEx(&Timer->KeTimer, KeDueTime, PeriodMs, &Timer->Dpc);

    KeReleaseSpinLock(&Timer->Lock, OldIrql);

    return WasSet;
}

/**
 * @brief
 * Cancels an executive timer.
 *
 * @param[in,out] Timer
 * The timer returned by ExAllocateTimer().
 *
 * @param[in] Parameters
 * Reserved.
 *
 * @return
 * TRUE if the timer was set, including an expiration whose callback had not
 * started yet.
 */
BOOLEAN
NTAPI
ExCancelTimer(
    _Inout_ PEX_TIMER Timer,
    _In_opt_ PEXT_CANCEL_PARAMETERS Parameters)
{
    KIRQL OldIrql;
    BOOLEAN WasSet = FALSE;

    UNREFERENCED_PARAMETER(Parameters);

    ExpCheckTimer(Timer);

    KeAcquireSpinLock(&Timer->Lock, &OldIrql);
    if (!Timer->IsDeleting)
        WasSet = ExpDisarmTimer(Timer);
    KeReleaseSpinLock(&Timer->Lock, OldIrql);

    return WasSet;
}

/**
 * @brief
 * Deletes an executive timer.
 *
 * @param[in] Timer
 * The timer returned by ExAllocateTimer().
 *
 * @param[in] Cancel
 * TRUE to cancel a pending expiration. FALSE lets it run once more, and the
 * timer is deleted after that expiration.
 *
 * @param[in] Wait
 * TRUE to return only once no callback is running.
 *
 * @param[in] Parameters
 * Optional delete callback, called at DISPATCH_LEVEL once the timer is gone.
 *
 * @return
 * TRUE if Cancel stopped a pending expiration.
 *
 * @remarks
 * Below DISPATCH_LEVEL the delete always completes before returning, as if
 * Wait were TRUE.
 */
BOOLEAN
NTAPI
ExDeleteTimer(
    _In_ PEX_TIMER Timer,
    _In_ BOOLEAN Cancel,
    _In_ BOOLEAN Wait,
    _In_opt_ PEXT_DELETE_PARAMETERS Parameters)
{
    KIRQL OldIrql;
    BOOLEAN WasSet = FALSE;
    BOOLEAN DeleteNow = TRUE;

    UNREFERENCED_PARAMETER(Wait);

    ExpCheckTimer(Timer);

    if (Parameters && (Parameters->Version != 0))
    {
        KeBugCheckEx(TIMER_OR_DPC_INVALID,
                     EXP_TIMER_BUGCHECK_EX_TIMER,
                     EXP_TIMER_BAD_PARAMETERS,
                     Parameters->Version,
                     0);
    }

    KeAcquireSpinLock(&Timer->Lock, &OldIrql);

    if (Parameters)
    {
        Timer->DeleteCallback = Parameters->DeleteCallback;
        Timer->DeleteContext = Parameters->DeleteContext;
    }

    if (Timer->IsDeleting)
    {
        KeReleaseSpinLock(&Timer->Lock, OldIrql);
        return FALSE;
    }

    Timer->IsDeleting = TRUE;
    if (Cancel)
    {
        WasSet = ExpDisarmTimer(Timer);
    }
    else if (Timer->IsArmed)
    {
        Timer->DeleteOnExpiration = TRUE;
        DeleteNow = FALSE;
    }

    KeReleaseSpinLock(&Timer->Lock, OldIrql);

    if (!DeleteNow)
        return WasSet;

    /* Flushing DPCs needs a thread that is free to move between processors */
    if ((KeGetCurrentIrql() < DISPATCH_LEVEL) &&
        !KeGetCurrentThread()->SystemAffinityActive)
    {
        ExpFinishTimerDelete(Timer);
    }
    else
    {
        ExQueueWorkItem(&Timer->DeleteWorkItem, DelayedWorkQueue);
    }

    return WasSet;
}

/* EOF */
