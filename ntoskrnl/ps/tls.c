/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Kernel thread local storage for drivers
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES ******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS *******************************************************************/

#define PSP_TLS_SLOTS       64
#define PSP_TLS_BUCKETS     64
#define TAG_PS_TLS          'sLsP'

typedef VOID (NTAPI *PPSP_TLS_CALLBACK)(_In_ PVOID Value);

/* The values one thread stored, found through the thread */
typedef struct _PSP_TLS_BLOCK
{
    LIST_ENTRY Link;
    PETHREAD Thread;
    PVOID Values[PSP_TLS_SLOTS];
} PSP_TLS_BLOCK, *PPSP_TLS_BLOCK;

static KSPIN_LOCK PspTlsLock;
static LIST_ENTRY PspTlsBuckets[PSP_TLS_BUCKETS];
static BOOLEAN PspTlsSlotInUse[PSP_TLS_SLOTS];
static PPSP_TLS_CALLBACK PspTlsCallbacks[PSP_TLS_SLOTS];

/* FUNCTIONS *****************************************************************/

/**
 * @brief Prepares the slot table.
 */
CODE_SEG("INIT")
VOID
NTAPI
PspInitializeTls(VOID)
{
    ULONG i;

    KeInitializeSpinLock(&PspTlsLock);

    for (i = 0; i < PSP_TLS_BUCKETS; i++)
        InitializeListHead(&PspTlsBuckets[i]);
}

/**
 * @brief Finds the block a thread stores its values in.
 * @remarks PspTlsLock must be held.
 */
static
PPSP_TLS_BLOCK
PspFindTlsBlock(
    _In_ PETHREAD Thread)
{
    PLIST_ENTRY Bucket, Entry;
    PPSP_TLS_BLOCK Block;

    Bucket = &PspTlsBuckets[((ULONG_PTR)Thread >> 6) % PSP_TLS_BUCKETS];
    for (Entry = Bucket->Flink; Entry != Bucket; Entry = Entry->Flink)
    {
        Block = CONTAINING_RECORD(Entry, PSP_TLS_BLOCK, Link);
        if (Block->Thread == Thread)
            return Block;
    }

    return NULL;
}

/**
 * @brief Reserves a slot every thread can keep a value in.
 *
 * @param[in] Callback
 * Called with the value of a thread when the thread exits or the slot is freed.
 *
 * @param[in] Flags
 * Must be zero.
 *
 * @param[out] Index
 * Receives the slot, never zero.
 */
NTSTATUS
NTAPI
PsTlsAlloc(
    _In_opt_ PVOID Callback,
    _In_ ULONG Flags,
    _Out_ PULONG Index)
{
    NTSTATUS Status = STATUS_INSUFFICIENT_RESOURCES;
    KIRQL OldIrql;
    ULONG i;

    if (Flags != 0)
        return STATUS_INVALID_PARAMETER;

    KeAcquireSpinLock(&PspTlsLock, &OldIrql);

    for (i = 0; i < PSP_TLS_SLOTS; i++)
    {
        if (!PspTlsSlotInUse[i])
        {
            PspTlsSlotInUse[i] = TRUE;
            PspTlsCallbacks[i] = (PPSP_TLS_CALLBACK)Callback;
            *Index = i + 1;
            Status = STATUS_SUCCESS;
            break;
        }
    }

    KeReleaseSpinLock(&PspTlsLock, OldIrql);
    return Status;
}

/**
 * @brief Gives a slot back, the values threads still keep in it go to its callback.
 */
VOID
NTAPI
PsTlsFree(
    _In_ ULONG Index)
{
    PPSP_TLS_CALLBACK Callback;
    PPSP_TLS_BLOCK Block;
    PLIST_ENTRY Entry;
    PVOID Value;
    KIRQL OldIrql;
    ULONG Slot, i;

    ASSERT((Index != 0) && (Index <= PSP_TLS_SLOTS));
    if ((Index == 0) || (Index > PSP_TLS_SLOTS))
        return;

    Slot = Index - 1;
    Callback = PspTlsCallbacks[Slot];

    /* Callbacks run without the lock, so take one value at a time */
    for (;;)
    {
        Value = NULL;
        KeAcquireSpinLock(&PspTlsLock, &OldIrql);

        for (i = 0; (i < PSP_TLS_BUCKETS) && !Value; i++)
        {
            for (Entry = PspTlsBuckets[i].Flink; Entry != &PspTlsBuckets[i]; Entry = Entry->Flink)
            {
                Block = CONTAINING_RECORD(Entry, PSP_TLS_BLOCK, Link);
                if (Block->Values[Slot])
                {
                    Value = Block->Values[Slot];
                    Block->Values[Slot] = NULL;
                    break;
                }
            }
        }

        if (!Value)
        {
            PspTlsSlotInUse[Slot] = FALSE;
            PspTlsCallbacks[Slot] = NULL;
            KeReleaseSpinLock(&PspTlsLock, OldIrql);
            return;
        }

        KeReleaseSpinLock(&PspTlsLock, OldIrql);

        if (Callback)
            Callback(Value);
    }
}

/**
 * @brief Gets the value the current thread keeps in a slot.
 *
 * @param[in] Index
 * Slot from PsTlsAlloc.
 *
 * @param[out] Value
 * Receives the value, NULL if the thread never set one.
 */
NTSTATUS
NTAPI
PsTlsGetValue(
    _In_ ULONG Index,
    _Out_ PVOID *Value)
{
    PPSP_TLS_BLOCK Block;
    KIRQL OldIrql;

    if ((Index == 0) || (Index > PSP_TLS_SLOTS))
        return STATUS_INVALID_PARAMETER;

    KeAcquireSpinLock(&PspTlsLock, &OldIrql);
    Block = PspFindTlsBlock(PsGetCurrentThread());
    *Value = Block ? Block->Values[Index - 1] : NULL;
    KeReleaseSpinLock(&PspTlsLock, OldIrql);

    return STATUS_SUCCESS;
}

/**
 * @brief Sets the value the current thread keeps in a slot.
 *
 * @param[in] Index
 * Slot from PsTlsAlloc.
 *
 * @param[in] Value
 * Value to keep.
 */
NTSTATUS
NTAPI
PsTlsSetValue(
    _In_ ULONG Index,
    _In_opt_ PVOID Value)
{
    PETHREAD Thread = PsGetCurrentThread();
    PPSP_TLS_BLOCK Block, NewBlock;
    KIRQL OldIrql;

    if ((Index == 0) || (Index > PSP_TLS_SLOTS))
        return STATUS_INVALID_PARAMETER;

    KeAcquireSpinLock(&PspTlsLock, &OldIrql);
    Block = PspFindTlsBlock(Thread);
    if (Block)
    {
        Block->Values[Index - 1] = Value;
        KeReleaseSpinLock(&PspTlsLock, OldIrql);
        return STATUS_SUCCESS;
    }
    KeReleaseSpinLock(&PspTlsLock, OldIrql);

    if (!Value)
        return STATUS_SUCCESS;

    NewBlock = ExAllocatePoolZero(NonPagedPool, sizeof(*NewBlock), TAG_PS_TLS);
    if (!NewBlock)
        return STATUS_INSUFFICIENT_RESOURCES;

    NewBlock->Thread = Thread;
    NewBlock->Values[Index - 1] = Value;

    /* Only this thread adds its own block, nobody raced it in */
    KeAcquireSpinLock(&PspTlsLock, &OldIrql);
    InsertTailList(&PspTlsBuckets[((ULONG_PTR)Thread >> 6) % PSP_TLS_BUCKETS], &NewBlock->Link);
    KeReleaseSpinLock(&PspTlsLock, OldIrql);

    return STATUS_SUCCESS;
}

/**
 * @brief Hands the values of an exiting thread to the slot callbacks.
 */
VOID
NTAPI
PspTlsThreadExit(
    _In_ PETHREAD Thread)
{
    PPSP_TLS_CALLBACK Callbacks[PSP_TLS_SLOTS];
    PPSP_TLS_BLOCK Block;
    KIRQL OldIrql;
    ULONG i;

    KeAcquireSpinLock(&PspTlsLock, &OldIrql);
    Block = PspFindTlsBlock(Thread);
    if (Block)
    {
        RemoveEntryList(&Block->Link);
        RtlCopyMemory(Callbacks, PspTlsCallbacks, sizeof(Callbacks));
    }
    KeReleaseSpinLock(&PspTlsLock, OldIrql);

    if (!Block)
        return;

    for (i = 0; i < PSP_TLS_SLOTS; i++)
    {
        if (Block->Values[i] && Callbacks[i])
            Callbacks[i](Block->Values[i]);
    }

    ExFreePoolWithTag(Block, TAG_PS_TLS);
}
