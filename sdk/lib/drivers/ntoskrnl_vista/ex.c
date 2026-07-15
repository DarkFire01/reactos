/*
 * PROJECT:     ReactOS Kernel - Vista+ APIs
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Ex functions of Vista+
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "ntoskrnl_vista.h"

/**
 * @brief
 * Acquires an executive spin lock for exclusive (write) access.
 *
 * @param[in,out] SpinLock
 * Pointer to the executive spin lock to acquire.
 *
 * @return
 * The IRQL at which the caller was running before the lock was acquired. It
 * must be passed to ExReleaseSpinLockExclusive() when the lock is released.
 *
 * @remarks
 * The lock is acquired at DISPATCH_LEVEL. Exclusive ownership requires the lock
 * to be free of both the writer bit and any shared holders.
 */
KIRQL
FASTCALL
ExAcquireSpinLockExclusive(
    _Inout_ PEX_SPIN_LOCK SpinLock)
{
    KIRQL OldIrql;

    KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);

    for (;;)
    {
        LONG OldValue = *(volatile LONG *)SpinLock;

        if (OldValue == 0 &&
            InterlockedCompareExchange((volatile LONG *)SpinLock,
                                       EX_SPIN_LOCK_WRITER_BIT,
                                       0) == 0)
        {
            break;
        }

        YieldProcessor();
    }

    return OldIrql;
}

/**
 * @brief
 * Acquires an executive spin lock for shared (read) access.
 *
 * @param[in,out] SpinLock
 * Pointer to the executive spin lock to acquire.
 *
 * @return
 * The IRQL at which the caller was running before the lock was acquired. It
 * must be passed to ExReleaseSpinLockShared() when the lock is released.
 *
 * @remarks
 * The lock is acquired at DISPATCH_LEVEL. Any number of shared holders may own
 * the lock simultaneously as long as no writer holds it.
 */
KIRQL
FASTCALL
ExAcquireSpinLockShared(
    _Inout_ PEX_SPIN_LOCK SpinLock)
{
    KIRQL OldIrql;

    KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);

    for (;;)
    {
        LONG OldValue = *(volatile LONG *)SpinLock;

        if (!(OldValue & EX_SPIN_LOCK_WRITER_BIT) &&
            InterlockedCompareExchange((volatile LONG *)SpinLock,
                                       OldValue + EX_SPIN_LOCK_SHARE_INC,
                                       OldValue) == OldValue)
        {
            break;
        }

        YieldProcessor();
    }

    return OldIrql;
}

/**
 * @brief
 * Releases an executive spin lock held for exclusive access.
 *
 * @param[in,out] SpinLock
 * Pointer to the executive spin lock to release.
 *
 * @param[in] OldIrql
 * The IRQL returned by the matching ExAcquireSpinLockExclusive() call.
 */
VOID
FASTCALL
ExReleaseSpinLockExclusive(
    _Inout_ PEX_SPIN_LOCK SpinLock,
    _In_ KIRQL OldIrql)
{
    InterlockedAnd((volatile LONG *)SpinLock, ~EX_SPIN_LOCK_WRITER_BIT);
    KeLowerIrql(OldIrql);
}

/**
 * @brief
 * Releases an executive spin lock held for shared access.
 *
 * @param[in,out] SpinLock
 * Pointer to the executive spin lock to release.
 *
 * @param[in] OldIrql
 * The IRQL returned by the matching ExAcquireSpinLockShared() call.
 */
VOID
FASTCALL
ExReleaseSpinLockShared(
    _Inout_ PEX_SPIN_LOCK SpinLock,
    _In_ KIRQL OldIrql)
{
    InterlockedExchangeAdd((volatile LONG *)SpinLock, -EX_SPIN_LOCK_SHARE_INC);
    KeLowerIrql(OldIrql);
}

/**
 * @brief
 * Attempts to queue an executive work item without blocking.
 *
 * @param[in,out] WorkItem
 * The work item to queue.
 *
 * @param[in] QueueType
 * The type of the system work queue to use.
 *
 * @return
 * TRUE if the work item was successfully queued, FALSE otherwise.
 *
 * @unimplemented
 * ReactOS does not implement the throttled work-queue back-end used by this
 * routine. The work item is currently never queued.
 */
BOOLEAN
NTAPI
ExTryQueueWorkItem(
    _Inout_ PWORK_QUEUE_ITEM WorkItem,
    _In_ WORK_QUEUE_TYPE QueueType)
{
    UNREFERENCED_PARAMETER(WorkItem);
    UNREFERENCED_PARAMETER(QueueType);

    return FALSE;
}

/**
 * @brief
 * Starts or resets an executive timer object.
 *
 * @param[in] Timer
 * The executive timer object, as returned by ExAllocateTimer().
 *
 * @param[in] DueTime
 * The timer expiration time, in 100-nanosecond units. A negative value denotes
 * relative time, a positive value denotes absolute time.
 *
 * @param[in] Period
 * The period of a periodic timer, in 100-nanosecond units, or 0 for a
 * one-shot timer.
 *
 * @param[in] Parameters
 * Optional extended set parameters.
 *
 * @return
 * TRUE if the timer was already set, FALSE otherwise.
 *
 * @unimplemented
 * ReactOS does not implement the EX_TIMER object type. This routine is a
 * placeholder for driver source compatibility.
 */
BOOLEAN
NTAPI
ExSetTimer(
    _In_ PEX_TIMER Timer,
    _In_ LONGLONG DueTime,
    _In_ LONGLONG Period,
    _In_opt_ PEXT_SET_PARAMETERS Parameters)
{
    UNREFERENCED_PARAMETER(Timer);
    UNREFERENCED_PARAMETER(DueTime);
    UNREFERENCED_PARAMETER(Period);
    UNREFERENCED_PARAMETER(Parameters);

    DbgPrint("ExSetTimer is UNIMPLEMENTED\n");
    return FALSE;
}
