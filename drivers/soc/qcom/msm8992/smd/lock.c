/*
 * PROJECT:     ReactOS Qualcomm SMD driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Re-entrant spin lock
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "smd.h"

VOID
SmdRLockInitialize(
    _Out_ PSMD_RLOCK Lock)
{
    KeInitializeSpinLock(&Lock->Lock);
    Lock->Owner = NULL;
    Lock->Depth = 0;
}

/* Holding it raises to DISPATCH_LEVEL, so the owning thread cannot migrate */
VOID
SmdRLockAcquire(
    _Inout_ PSMD_RLOCK Lock)
{
    PKTHREAD Self = KeGetCurrentThread();
    KIRQL OldIrql;

    if (Lock->Owner == Self)
    {
        Lock->Depth++;
        return;
    }

    KeAcquireSpinLock(&Lock->Lock, &OldIrql);
    Lock->Owner = Self;
    Lock->Depth = 1;
    Lock->OldIrql = OldIrql;
}

VOID
SmdRLockRelease(
    _Inout_ PSMD_RLOCK Lock)
{
    KIRQL OldIrql;

    ASSERT(Lock->Owner == KeGetCurrentThread());

    if (--Lock->Depth != 0)
        return;

    OldIrql = Lock->OldIrql;
    Lock->Owner = NULL;
    KeReleaseSpinLock(&Lock->Lock, OldIrql);
}
