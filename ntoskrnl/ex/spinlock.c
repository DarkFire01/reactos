/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Executive reader/writer spin locks
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* DEFINES ********************************************************************/

/* Bit 31 marks the writer, the low bits count the readers */
#define EXP_SPIN_LOCK_WRITER ((LONG)0x80000000)

/* PUBLIC FUNCTIONS ***********************************************************/

/**
 * @brief
 * Takes an executive spin lock for shared access at DISPATCH_LEVEL or above.
 *
 * @param[in,out] SpinLock
 * The spin lock to take.
 */
VOID
NTAPI
ExAcquireSpinLockSharedAtDpcLevel(
    _Inout_ PEX_SPIN_LOCK SpinLock)
{
    LONG Readers;

    ASSERT(KeGetCurrentIrql() >= DISPATCH_LEVEL);

    for (;;)
    {
        /* Only a lock without a writer may gain a reader */
        Readers = *SpinLock & ~EXP_SPIN_LOCK_WRITER;
        if (InterlockedCompareExchange(SpinLock, Readers + 1, Readers) == Readers)
            return;

        YieldProcessor();
    }
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
 */
KIRQL
NTAPI
ExAcquireSpinLockShared(
    _Inout_ PEX_SPIN_LOCK SpinLock)
{
    KIRQL OldIrql;

    KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);
    ExAcquireSpinLockSharedAtDpcLevel(SpinLock);

    return OldIrql;
}

/**
 * @brief
 * Drops an executive spin lock held for shared access, staying at
 * DISPATCH_LEVEL.
 *
 * @param[in,out] SpinLock
 * The spin lock to drop.
 */
VOID
NTAPI
ExReleaseSpinLockSharedFromDpcLevel(
    _Inout_ PEX_SPIN_LOCK SpinLock)
{
    InterlockedDecrement(SpinLock);
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
NTAPI
ExReleaseSpinLockShared(
    _Inout_ PEX_SPIN_LOCK SpinLock,
    _In_ KIRQL OldIrql)
{
    ExReleaseSpinLockSharedFromDpcLevel(SpinLock);
    KeLowerIrql(OldIrql);
}

/**
 * @brief
 * Takes an executive spin lock for exclusive access at DISPATCH_LEVEL or above.
 *
 * @param[in,out] SpinLock
 * The spin lock to take.
 *
 * @remarks
 * The writer bit is claimed first, which keeps further readers out, and the
 * readers already inside the lock are then waited out.
 */
VOID
NTAPI
ExAcquireSpinLockExclusiveAtDpcLevel(
    _Inout_ PEX_SPIN_LOCK SpinLock)
{
    ASSERT(KeGetCurrentIrql() >= DISPATCH_LEVEL);

    while (InterlockedOr(SpinLock, EXP_SPIN_LOCK_WRITER) & EXP_SPIN_LOCK_WRITER)
        YieldProcessor();

    while (*SpinLock != EXP_SPIN_LOCK_WRITER)
        YieldProcessor();
}

/**
 * @brief
 * Takes an executive spin lock for exclusive access.
 *
 * @param[in,out] SpinLock
 * The spin lock to take.
 *
 * @return
 * The IRQL the caller ran at, to be handed to ExReleaseSpinLockExclusive().
 */
KIRQL
NTAPI
ExAcquireSpinLockExclusive(
    _Inout_ PEX_SPIN_LOCK SpinLock)
{
    KIRQL OldIrql;

    KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);
    ExAcquireSpinLockExclusiveAtDpcLevel(SpinLock);

    return OldIrql;
}

/**
 * @brief
 * Drops an executive spin lock held for exclusive access, staying at
 * DISPATCH_LEVEL.
 *
 * @param[in,out] SpinLock
 * The spin lock to drop.
 */
VOID
NTAPI
ExReleaseSpinLockExclusiveFromDpcLevel(
    _Inout_ PEX_SPIN_LOCK SpinLock)
{
    /* The writer owns the whole lock, so there is nothing else to preserve */
    InterlockedExchange(SpinLock, 0);
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
NTAPI
ExReleaseSpinLockExclusive(
    _Inout_ PEX_SPIN_LOCK SpinLock,
    _In_ KIRQL OldIrql)
{
    ExReleaseSpinLockExclusiveFromDpcLevel(SpinLock);
    KeLowerIrql(OldIrql);
}

/* EOF */
