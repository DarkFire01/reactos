/*
 * PROJECT:     ReactOS Kernel - Vista+ APIs
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Ex functions of Vista+
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include "ntoskrnl_vista.h"
#define NDEBUG
#include <debug.h>

/* FUNCTIONS ******************************************************************/

/**
 * @brief
 * Takes an executive spin lock for exclusive access.
 *
 * @param[in,out] SpinLock
 * The spin lock to take.
 *
 * @return
 * The IRQL the caller ran at, to be handed to ExReleaseSpinLockExclusive().
 *
 * @remarks
 * The writer bit is claimed first, which keeps further readers out, and the
 * readers already inside the lock are then waited out.
 */
KIRQL
FASTCALL
ExAcquireSpinLockExclusive(
    _Inout_ PEX_SPIN_LOCK SpinLock)
{
    KIRQL OldIrql;

    KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);

    while (InterlockedOr(SpinLock, EX_SPIN_LOCK_WRITER) & EX_SPIN_LOCK_WRITER)
    {
        YieldProcessor();
    }

    while (*(volatile LONG *)SpinLock != EX_SPIN_LOCK_WRITER)
    {
        YieldProcessor();
    }

    return OldIrql;
}

/**
 * @brief
 * Takes an executive spin lock for shared access.
 *
 * @param[in,out] SpinLock
 * The spin lock to take.
 *
 * @return
 * The IRQL the caller ran at, to be handed to ExReleaseSpinLockShared().
 *
 * @remarks
 * Any number of readers may hold the lock as long as no writer owns it.
 */
KIRQL
FASTCALL
ExAcquireSpinLockShared(
    _Inout_ PEX_SPIN_LOCK SpinLock)
{
    KIRQL OldIrql;
    LONG Readers;

    KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);

    for (;;)
    {
        /* Only the reader count takes part in the exchange */
        Readers = *(volatile LONG *)SpinLock & ~EX_SPIN_LOCK_WRITER;
        if (InterlockedCompareExchange(SpinLock, Readers + 1, Readers) == Readers)
            break;

        YieldProcessor();
    }

    return OldIrql;
}

/**
 * @brief
 * Drops an executive spin lock held for exclusive access.
 *
 * @param[in,out] SpinLock
 * The spin lock to drop.
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
    /* The writer owns the whole lock, so there is nothing else to preserve */
    InterlockedExchange(SpinLock, 0);
    KeLowerIrql(OldIrql);
}

/**
 * @brief
 * Drops an executive spin lock held for shared access.
 *
 * @param[in,out] SpinLock
 * The spin lock to drop.
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
    InterlockedDecrement(SpinLock);
    KeLowerIrql(OldIrql);
}

/**
 * @brief
 * Queues an executive work item without waiting on the queue.
 *
 * @param[in,out] WorkItem
 * The work item to queue.
 *
 * @param[in] QueueType
 * The system work queue to use.
 *
 * @return
 * FALSE, as the work item is never queued.
 *
 * @unimplemented
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

/* EOF */
