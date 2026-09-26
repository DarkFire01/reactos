/*
 * PROJECT:         ReactOS Kernel
 * LICENSE:         GPL - See COPYING in the top level directory
 * FILE:            ntoskrnl/ke/timerobj.c
 * PURPOSE:         Handle Kernel Timers (Kernel-part of Executive Timers)
 * PROGRAMMERS:     Alex Ionescu (alex.ionescu@reactos.org)
 */

/* INCLUDES ******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS *******************************************************************/

KTIMER_TABLE_ENTRY KiTimerTableListHead[TIMER_TABLE_SIZE];
LARGE_INTEGER KiTimeIncrementReciprocal;
UCHAR KiTimeIncrementShiftCount;
BOOLEAN KiEnableTimerWatchdog = FALSE;

/* Bit 0 lets KeCheckForTimer look, Session Manager\Kernel\TimerCheckFlags */
ULONG KeTimerCheckFlags = 1;

/* PRIVATE FUNCTIONS *********************************************************/

/**
 * @brief Bugchecks when a queued timer, its DPC or the DPC routine lies in a block that is going away.
 *
 * @param[in] BlockStart
 * First byte of the block.
 *
 * @param[in] BlockSize
 * Size of the block in bytes.
 */
VOID
NTAPI
KeCheckForTimer(
    _In_ PVOID BlockStart,
    _In_ SIZE_T BlockSize)
{
    ULONG_PTR Start = (ULONG_PTR)BlockStart;
    ULONG_PTR End = Start + BlockSize;
    PLIST_ENTRY ListHead, NextEntry;
    PKSPIN_LOCK_QUEUE LockQueue;
    ULONG_PTR Routine;
    PKTIMER Timer;
    PKDPC Dpc;
    KIRQL OldIrql;
    ULONG Hand;

    if (!(KeTimerCheckFlags & 1))
        return;

    KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);

    for (Hand = 0; Hand < TIMER_TABLE_SIZE; Hand++)
    {
        LockQueue = KiAcquireTimerLock(Hand);

        ListHead = &KiTimerTableListHead[Hand].Entry;
        for (NextEntry = ListHead->Flink; NextEntry != ListHead; NextEntry = NextEntry->Flink)
        {
            Timer = CONTAINING_RECORD(NextEntry, KTIMER, TimerListEntry);

            /* Any overlap with the block counts, the object would be cut in half */
            if (((ULONG_PTR)(Timer + 1) > Start) && ((ULONG_PTR)Timer < End))
                KeBugCheckEx(TIMER_OR_DPC_INVALID, 0, (ULONG_PTR)Timer, Start, End);

            Dpc = Timer->Dpc;
            if (Dpc == NULL)
                continue;

            if (((ULONG_PTR)(Dpc + 1) > Start) && ((ULONG_PTR)Dpc < End))
                KeBugCheckEx(TIMER_OR_DPC_INVALID, 1, (ULONG_PTR)Dpc, Start, End);

            Routine = (ULONG_PTR)Dpc->DeferredRoutine;
            if ((Routine >= Start) && (Routine < End))
                KeBugCheckEx(TIMER_OR_DPC_INVALID, 2, Routine, Start, End);
        }

        KiReleaseTimerLock(LockQueue);
    }

    KeLowerIrql(OldIrql);
}

BOOLEAN
FASTCALL
KiInsertTreeTimer(IN PKTIMER Timer,
                  IN LARGE_INTEGER Interval)
{
    BOOLEAN Inserted = FALSE;
    ULONG Hand = 0;
    PKSPIN_LOCK_QUEUE LockQueue;
    DPRINT("KiInsertTreeTimer(): Timer %p, Interval: %I64d\n", Timer, Interval.QuadPart);

    /* Setup the timer's due time */
    if (KiComputeDueTime(Timer, Interval, &Hand))
    {
        /* Acquire the lock */
        LockQueue = KiAcquireTimerLock(Hand);

        /* Insert the timer */
        if (KiInsertTimerTable(Timer, Hand))
        {
            /* It was already there, remove it */
            KiRemoveEntryTimer(Timer);
            Timer->Header.Inserted = FALSE;
        }
        else
        {
            /* Otherwise, we're now inserted */
            Inserted = TRUE;
        }

        /* Release the lock */
        KiReleaseTimerLock(LockQueue);
    }

    /* Release the lock and return insert status */
    return Inserted;
}

BOOLEAN
FASTCALL
KiInsertTimerTable(IN PKTIMER Timer,
                   IN ULONG Hand)
{
    ULONGLONG InterruptTime;
    ULONGLONG DueTime = Timer->DueTime.QuadPart;
    BOOLEAN Expired = FALSE;
    PLIST_ENTRY ListHead, NextEntry;
    PKTIMER CurrentTimer;
    DPRINT("KiInsertTimerTable(): Timer %p, Hand: %lu\n", Timer, Hand);

    /* Check if the period is zero */
    if (!Timer->Period) Timer->Header.SignalState = FALSE;

    /* Sanity check */
    ASSERT(Hand == KiComputeTimerTableIndex(DueTime));

    /* Loop the timer list backwards */
    ListHead = &KiTimerTableListHead[Hand].Entry;
    NextEntry = ListHead->Blink;
    while (NextEntry != ListHead)
    {
        /* Get the timer */
        CurrentTimer = CONTAINING_RECORD(NextEntry, KTIMER, TimerListEntry);

        /* Now check if we can fit it before */
        if ((ULONGLONG)DueTime >= CurrentTimer->DueTime.QuadPart) break;

        /* Keep looping */
        NextEntry = NextEntry->Blink;
    }

    /* Looped all the list, insert it here and get the interrupt time again */
    InsertHeadList(NextEntry, &Timer->TimerListEntry);

    /* Check if we didn't find it in the list */
    if (NextEntry == ListHead)
    {
        /* Set the time */
        KiTimerTableListHead[Hand].Time.QuadPart = DueTime;

        /* Make sure it hasn't expired already */
        InterruptTime = KeQueryInterruptTime();
        if (DueTime <= InterruptTime) Expired = TRUE;
    }

    /* Return expired state */
    return Expired;
}

BOOLEAN
FASTCALL
KiSignalTimer(IN PKTIMER Timer)
{
    BOOLEAN RequestInterrupt = FALSE;
    PKDPC Dpc = Timer->Dpc;
    ULONG Period = Timer->Period;
    LARGE_INTEGER Interval, SystemTime;
    DPRINT("KiSignalTimer(): Timer %p\n", Timer);

    /* Set default values */
    Timer->Header.Inserted = FALSE;
    Timer->Header.SignalState = TRUE;

    /* Check if the timer has waiters */
    if (!IsListEmpty(&Timer->Header.WaitListHead))
    {
        /* Check the type of event */
        if (Timer->Header.Type == TimerNotificationObject)
        {
            /* Unwait the thread */
            KxUnwaitThread(&Timer->Header, IO_NO_INCREMENT);
        }
        else
        {
            /* Otherwise unwait the thread and signal the timer */
            KxUnwaitThreadForEvent((PKEVENT)Timer, IO_NO_INCREMENT);
        }
    }

    /* Check if we have a period */
    if (Period)
    {
        /* Calculate the interval and insert the timer */
        Interval.QuadPart = Int32x32To64(Period, -10000);
        while (!KiInsertTreeTimer(Timer, Interval));
    }

    /* Check if we have a DPC */
    if (Dpc)
    {
        /* Insert it in the queue */
        KeQuerySystemTime(&SystemTime);
        KeInsertQueueDpc(Dpc,
                         ULongToPtr(SystemTime.LowPart),
                         ULongToPtr(SystemTime.HighPart));
        RequestInterrupt = TRUE;
    }

    /* Return whether we need to request a DPC interrupt or not */
    return RequestInterrupt;
}

VOID
FASTCALL
KiCompleteTimer(IN PKTIMER Timer,
                IN PKSPIN_LOCK_QUEUE LockQueue)
{
    LIST_ENTRY ListHead;
    BOOLEAN RequestInterrupt = FALSE;
    DPRINT("KiCompleteTimer(): Timer %p, LockQueue: %p\n", Timer, LockQueue);

    /* Remove it from the timer list */
    KiRemoveEntryTimer(Timer);

    /* Link the timer list to our stack */
    ListHead.Flink = &Timer->TimerListEntry;
    ListHead.Blink = &Timer->TimerListEntry;
    Timer->TimerListEntry.Flink = &ListHead;
    Timer->TimerListEntry.Blink = &ListHead;

    /* Release the timer lock */
    KiReleaseTimerLock(LockQueue);

    /* Acquire dispatcher lock */
    KiAcquireDispatcherLockAtSynchLevel();

    /* Signal the timer if it's still on our list */
    if (!IsListEmpty(&ListHead)) RequestInterrupt = KiSignalTimer(Timer);

    /* Release the dispatcher lock */
    KiReleaseDispatcherLockFromSynchLevel();

    /* Request a DPC if needed */
    if (RequestInterrupt) HalRequestSoftwareInterrupt(DISPATCH_LEVEL);
}

/* PUBLIC FUNCTIONS **********************************************************/

/*
 * @implemented
 */
BOOLEAN
NTAPI
KeCancelTimer(IN OUT PKTIMER Timer)
{
    KIRQL OldIrql;
    BOOLEAN Inserted;
    ASSERT_TIMER(Timer);
    ASSERT(KeGetCurrentIrql() <= DISPATCH_LEVEL);
    DPRINT("KeCancelTimer(): Timer %p\n", Timer);

    /* Lock the Database and Raise IRQL */
    OldIrql = KiAcquireDispatcherLock();

    /* Check if it's inserted, and remove it if it is */
    Inserted = Timer->Header.Inserted;
    if (Inserted) KxRemoveTreeTimer(Timer);

    /* Release Dispatcher Lock */
    KiReleaseDispatcherLock(OldIrql);

    /* Return the old state */
    return Inserted;
}

/*
 * @implemented
 */
VOID
NTAPI
KeInitializeTimer(OUT PKTIMER Timer)
{
    /* Call the New Function */
    KeInitializeTimerEx(Timer, NotificationTimer);
}

/*
 * @implemented
 */
VOID
NTAPI
KeInitializeTimerEx(OUT PKTIMER Timer,
                    IN TIMER_TYPE Type)
{
    DPRINT("KeInitializeTimerEx(): Timer %p, Type %s\n",
            Timer, (Type == NotificationTimer) ?
           "NotificationTimer" : "SynchronizationTimer");

    /* Initialize the Dispatch Header */
    ASSERT((Type == NotificationTimer) || (Type == SynchronizationTimer));
    Timer->Header.Type = TimerNotificationObject + Type;
    //Timer->Header.TimerControlFlags = 0; // win does not init this field
    Timer->Header.Hand = sizeof(KTIMER) / sizeof(ULONG);
    Timer->Header.Inserted = 0; // win7: Timer->Header.TimerMiscFlags = 0;
    Timer->Header.SignalState = 0;
    InitializeListHead(&(Timer->Header.WaitListHead));

    /* Initialize the Other data */
    Timer->DueTime.QuadPart = 0;
    Timer->Period = 0;
}

/*
 * @implemented
 */
BOOLEAN
NTAPI
KeReadStateTimer(IN PKTIMER Timer)
{
    /* Return the Signal State */
    ASSERT_TIMER(Timer);
    return (BOOLEAN)Timer->Header.SignalState;
}

/*
 * @implemented
 */
BOOLEAN
NTAPI
KeSetTimer(IN OUT PKTIMER Timer,
           IN LARGE_INTEGER DueTime,
           IN PKDPC Dpc OPTIONAL)
{
    /* Call the newer function and supply a period of 0 */
    return KeSetTimerEx(Timer, DueTime, 0, Dpc);
}

/*
 * @implemented
 */
BOOLEAN
NTAPI
KeSetTimerEx(IN OUT PKTIMER Timer,
             IN LARGE_INTEGER DueTime,
             IN LONG Period,
             IN PKDPC Dpc OPTIONAL)
{
    KIRQL OldIrql;
    BOOLEAN Inserted;
    ULONG Hand = 0;
    BOOLEAN RequestInterrupt = FALSE;
    ASSERT_TIMER(Timer);
    ASSERT(KeGetCurrentIrql() <= DISPATCH_LEVEL);
    DPRINT("KeSetTimerEx(): Timer %p, DueTime %I64d, Period %d, Dpc %p\n",
           Timer, DueTime.QuadPart, Period, Dpc);

    /* Lock the Database and Raise IRQL */
    OldIrql = KiAcquireDispatcherLock();

    /* Check if it's inserted, and remove it if it is */
    Inserted = Timer->Header.Inserted;
    if (Inserted) KxRemoveTreeTimer(Timer);

    /* Set Default Timer Data */
    Timer->Dpc = Dpc;
    Timer->Period = Period;
    if (!KiComputeDueTime(Timer, DueTime, &Hand))
    {
        /* Signal the timer */
        RequestInterrupt = KiSignalTimer(Timer);

        /* Release the dispatcher lock */
        KiReleaseDispatcherLockFromSynchLevel();

        /* Check if we need to do an interrupt */
        if (RequestInterrupt) HalRequestSoftwareInterrupt(DISPATCH_LEVEL);
    }
    else
    {
        /* Insert the timer */
        Timer->Header.SignalState = FALSE;
        KxInsertTimer(Timer, Hand);
    }

    /* Exit the dispatcher */
    KiExitDispatcher(OldIrql);

    /* Return old state */
    return Inserted;
}

/**
 * @brief
 * Sets a timer that may fire up to a given delay late, so it can share an
 * expiration with other timers.
 *
 * @param[in,out] Timer
 * The timer to set.
 *
 * @param[in] DueTime
 * Absolute or relative expiration time.
 *
 * @param[in] Period
 * Period in milliseconds, or zero for a one shot timer.
 *
 * @param[in] TolerableDelay
 * How late the expiration may run, in milliseconds.
 *
 * @param[in] Dpc
 * Optional DPC to queue when the timer expires.
 *
 * @return
 * TRUE if the timer was already in the timer queue.
 */
BOOLEAN
NTAPI
KeSetCoalescableTimer(
    _Inout_ PKTIMER Timer,
    _In_ LARGE_INTEGER DueTime,
    _In_ ULONG Period,
    _In_ ULONG TolerableDelay,
    _In_opt_ PKDPC Dpc)
{
    /* Timers are never coalesced, so every one expires on time */
    UNREFERENCED_PARAMETER(TolerableDelay);

    return KeSetTimerEx(Timer, DueTime, (LONG)Period, Dpc);
}

