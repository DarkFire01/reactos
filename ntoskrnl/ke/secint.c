/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Interrupts of secondary interrupt controllers, such as GPIO blocks
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES ******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/*
 * A secondary line never reaches the IDT. Its vector, from 256 on, names a slot
 * here, and the controller's own interrupt routine dispatches it through the HAL.
 */

/* GLOBALS *******************************************************************/

typedef struct _KI_SECONDARY_SLOT
{
    KSPIN_LOCK Lock;            ///< guards List
    LIST_ENTRY List;            ///< connected interrupt objects, by InterruptListEntry
    volatile LONG Dispatching;  ///< dispatches walking List right now
} KI_SECONDARY_SLOT, *PKI_SECONDARY_SLOT;

/* NULL until the HAL hands out its first secondary GSIV */
static PKI_SECONDARY_SLOT volatile KiSecondarySlots;

/* FUNCTIONS *****************************************************************/

static
PKI_SECONDARY_SLOT
KiGetSecondarySlot(
    _In_ ULONG Vector)
{
    PKI_SECONDARY_SLOT Slots = KiSecondarySlots;

    if (!Slots || !KiIsSecondaryVector(Vector))
        return NULL;

    return &Slots[Vector - KI_SECONDARY_VECTOR_FIRST];
}

/**
 * @brief Sets up the slots secondary vectors are connected to.
 *
 * @param[in] HalExports
 * Unused, kept for the HAL's call.
 */
NTSTATUS
NTAPI
KeInitializeSecondaryInterruptServices(
    _In_opt_ PVOID HalExports)
{
    PKI_SECONDARY_SLOT Slots;
    ULONG Index;

    UNREFERENCED_PARAMETER(HalExports);

    if (KiSecondarySlots)
        return STATUS_SUCCESS;

    Slots = ExAllocatePoolZero(NonPagedPoolNx, KI_SECONDARY_VECTOR_COUNT * sizeof(*Slots), TAG_KERNEL);
    if (!Slots)
        return STATUS_INSUFFICIENT_RESOURCES;

    for (Index = 0; Index < KI_SECONDARY_VECTOR_COUNT; Index++)
    {
        KeInitializeSpinLock(&Slots[Index].Lock);
        InitializeListHead(&Slots[Index].List);
    }

    /* Two first calls may race, the one that publishes first wins */
    if (InterlockedCompareExchangePointer((PVOID volatile *)&KiSecondarySlots, Slots, NULL) != NULL)
        ExFreePoolWithTag(Slots, TAG_KERNEL);

    return STATUS_SUCCESS;
}

/**
 * @brief Links an interrupt object to its secondary vector, under the same sharing rules as the IDT.
 */
BOOLEAN
NTAPI
KiConnectSecondaryInterrupt(
    _Inout_ PKINTERRUPT Interrupt)
{
    PKI_SECONDARY_SLOT Slot;
    PKINTERRUPT Connected;
    BOOLEAN Result = FALSE;
    KIRQL OldIrql;

    Slot = KiGetSecondarySlot(Interrupt->Vector);
    if (!Slot ||
        Interrupt->Irql > (IPI_LEVEL - 2) ||
        Interrupt->Number >= KeNumberProcessors ||
        Interrupt->SynchronizeIrql < Interrupt->Irql)
    {
        DPRINT1("Secondary vector %lu refused, IRQL %u\n", Interrupt->Vector, Interrupt->Irql);
        return FALSE;
    }

    if (Interrupt->Connected)
        return TRUE;

    KeRaiseIrql(HIGH_LEVEL, &OldIrql);
    KeAcquireSpinLockAtDpcLevel(&Slot->Lock);

    if (!IsListEmpty(&Slot->List))
    {
        Connected = CONTAINING_RECORD(Slot->List.Flink, KINTERRUPT, InterruptListEntry);
        if (!Interrupt->ShareVector || !Connected->ShareVector || Interrupt->Mode != Connected->Mode)
            goto Exit;
    }

    /* A dispatch may be walking the list, so the new entry is whole before it is reachable */
    Interrupt->InterruptListEntry.Flink = &Slot->List;
    Interrupt->InterruptListEntry.Blink = Slot->List.Blink;
    KeMemoryBarrier();
    Slot->List.Blink->Flink = &Interrupt->InterruptListEntry;
    Slot->List.Blink = &Interrupt->InterruptListEntry;

    Interrupt->Connected = TRUE;
    Result = TRUE;

Exit:
    KeReleaseSpinLockFromDpcLevel(&Slot->Lock);
    KeLowerIrql(OldIrql);
    return Result;
}

/**
 * @brief Unlinks an interrupt object, then waits out every dispatch that may still be running it.
 */
BOOLEAN
NTAPI
KiDisconnectSecondaryInterrupt(
    _Inout_ PKINTERRUPT Interrupt)
{
    PKI_SECONDARY_SLOT Slot;
    KIRQL OldIrql;

    Slot = KiGetSecondarySlot(Interrupt->Vector);
    if (!Slot || !Interrupt->Connected)
        return TRUE;

    KeRaiseIrql(HIGH_LEVEL, &OldIrql);
    KeAcquireSpinLockAtDpcLevel(&Slot->Lock);

    /* The entry keeps its links, a dispatch standing on it still finds its way back to the head */
    Interrupt->InterruptListEntry.Blink->Flink = Interrupt->InterruptListEntry.Flink;
    Interrupt->InterruptListEntry.Flink->Blink = Interrupt->InterruptListEntry.Blink;
    Interrupt->Connected = FALSE;

    KeReleaseSpinLockFromDpcLevel(&Slot->Lock);
    KeLowerIrql(OldIrql);

    while (Slot->Dispatching != 0)
        YieldProcessor();

    return TRUE;
}

/**
 * @brief Runs the service routines connected to a secondary vector.
 *
 * @param[in] Vector
 * The secondary vector, from 256 on.
 *
 * @param[in] Flags
 * Unused.
 *
 * @return TRUE when a service routine took the interrupt.
 *
 * @remarks Called from the controller's interrupt routine, at its IRQL or at PASSIVE_LEVEL.
 */
BOOLEAN
NTAPI
KeDispatchSecondaryInterrupt(
    _In_ ULONG Vector,
    _In_ ULONG Flags)
{
    PKI_SECONDARY_SLOT Slot;
    PKINTERRUPT Interrupt;
    PLIST_ENTRY Entry;
    BOOLEAN Handled = FALSE;
    BOOLEAN Claimed;
    KIRQL OldIrql;

    UNREFERENCED_PARAMETER(Flags);

    Slot = KiGetSecondarySlot(Vector);
    if (!Slot)
        return FALSE;

    InterlockedIncrement(&Slot->Dispatching);

    for (Entry = Slot->List.Flink; Entry != &Slot->List; Entry = Entry->Flink)
    {
        Interrupt = CONTAINING_RECORD(Entry, KINTERRUPT, InterruptListEntry);

        OldIrql = KeGetCurrentIrql();
        if (OldIrql < Interrupt->SynchronizeIrql)
            KeRaiseIrql(Interrupt->SynchronizeIrql, &OldIrql);

        KeAcquireSpinLockAtDpcLevel(Interrupt->ActualLock);
        Claimed = Interrupt->ServiceRoutine(Interrupt, Interrupt->ServiceContext);
        KeReleaseSpinLockFromDpcLevel(Interrupt->ActualLock);

        if (OldIrql < Interrupt->SynchronizeIrql)
            KeLowerIrql(OldIrql);

        if (Claimed)
        {
            Handled = TRUE;

            /* A level line stays asserted for the next sharer, so the first taker ends it */
            if (Interrupt->Mode == LevelSensitive)
                break;
        }
    }

    InterlockedDecrement(&Slot->Dispatching);
    return Handled;
}
