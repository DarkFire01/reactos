/*
 * PROJECT:     ReactOS Runtime Library
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Waiting on the value of an address
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *****************************************************************/

#include <rtl_vista.h>

#define NDEBUG
#include <debug.h>

/* GLOBALS ******************************************************************/

#define RTLP_WAIT_ADDRESS_BUCKETS 128

typedef struct _RTLP_ADDRESS_WAITER
{
    LIST_ENTRY Links;
    const volatile VOID *Address;
    BOOLEAN Woken;
} RTLP_ADDRESS_WAITER, *PRTLP_ADDRESS_WAITER;

typedef struct _RTLP_WAIT_ADDRESS_BUCKET
{
    RTL_SRWLOCK Lock;
    LIST_ENTRY Waiters;
} RTLP_WAIT_ADDRESS_BUCKET, *PRTLP_WAIT_ADDRESS_BUCKET;

static RTLP_WAIT_ADDRESS_BUCKET RtlpWaitAddressBuckets[RTLP_WAIT_ADDRESS_BUCKETS];

/* PRIVATE FUNCTIONS *********************************************************/

static
PRTLP_WAIT_ADDRESS_BUCKET
RtlpLockWaitAddressBucket(
    _In_ const volatile VOID *Address)
{
    PRTLP_WAIT_ADDRESS_BUCKET Bucket;

    Bucket = &RtlpWaitAddressBuckets[((ULONG_PTR)Address >> 4) % RTLP_WAIT_ADDRESS_BUCKETS];
    RtlAcquireSRWLockExclusive(&Bucket->Lock);

    /* The table starts out zeroed */
    if (!Bucket->Waiters.Flink)
        InitializeListHead(&Bucket->Waiters);

    return Bucket;
}

static
BOOLEAN
RtlpAddressHoldsValue(
    _In_ const volatile VOID *Address,
    _In_ PVOID CompareAddress,
    _In_ SIZE_T AddressSize)
{
    switch (AddressSize)
    {
        case sizeof(UCHAR):
            return *(const volatile UCHAR *)Address == *(PUCHAR)CompareAddress;

        case sizeof(USHORT):
            return *(const volatile USHORT *)Address == *(PUSHORT)CompareAddress;

        case sizeof(ULONG):
            return *(const volatile ULONG *)Address == *(PULONG)CompareAddress;

        default:
            return *(const volatile ULONG64 *)Address == *(PULONG64)CompareAddress;
    }
}

static
VOID
RtlpWakeAddress(
    _In_ const volatile VOID *Address,
    _In_ BOOLEAN WakeAll)
{
    PRTLP_WAIT_ADDRESS_BUCKET Bucket;
    PRTLP_ADDRESS_WAITER Waiter;
    PLIST_ENTRY Entry, Next;
    LIST_ENTRY Woken;

    InitializeListHead(&Woken);

    Bucket = RtlpLockWaitAddressBucket(Address);
    for (Entry = Bucket->Waiters.Flink; Entry != &Bucket->Waiters; Entry = Next)
    {
        Next = Entry->Flink;
        Waiter = CONTAINING_RECORD(Entry, RTLP_ADDRESS_WAITER, Links);
        if (Waiter->Address != Address)
            continue;

        RemoveEntryList(Entry);
        Waiter->Woken = TRUE;
        InsertTailList(&Woken, Entry);

        if (!WakeAll)
            break;
    }
    RtlReleaseSRWLockExclusive(&Bucket->Lock);

    /* A released waiter leaves with its stack, so it is unlinked first */
    while (!IsListEmpty(&Woken))
    {
        Entry = RemoveHeadList(&Woken);
        NtReleaseKeyedEvent(NULL, CONTAINING_RECORD(Entry, RTLP_ADDRESS_WAITER, Links), FALSE, NULL);
    }
}

/* EXPORTED FUNCTIONS ********************************************************/

/**
 * @brief
 * Waits until the value at an address no longer matches, or a wake call for the address.
 */
NTSTATUS
NTAPI
RtlWaitOnAddress(
    _In_reads_bytes_(AddressSize) const volatile VOID *Address,
    _In_reads_bytes_(AddressSize) PVOID CompareAddress,
    _In_ SIZE_T AddressSize,
    _In_opt_ PLARGE_INTEGER Timeout)
{
    PRTLP_WAIT_ADDRESS_BUCKET Bucket;
    RTLP_ADDRESS_WAITER Waiter;
    NTSTATUS Status;

    if (AddressSize != sizeof(UCHAR) && AddressSize != sizeof(USHORT) &&
        AddressSize != sizeof(ULONG) && AddressSize != sizeof(ULONG64))
    {
        return STATUS_INVALID_PARAMETER;
    }

    Waiter.Address = Address;
    Waiter.Woken = FALSE;

    /* Wakers take the bucket lock after changing the value, so none can be missed */
    Bucket = RtlpLockWaitAddressBucket(Address);
    if (!RtlpAddressHoldsValue(Address, CompareAddress, AddressSize))
    {
        RtlReleaseSRWLockExclusive(&Bucket->Lock);
        return STATUS_SUCCESS;
    }
    InsertTailList(&Bucket->Waiters, &Waiter.Links);
    RtlReleaseSRWLockExclusive(&Bucket->Lock);

    Status = NtWaitForKeyedEvent(NULL, &Waiter, FALSE, Timeout);
    if (Status != STATUS_TIMEOUT)
        return NT_SUCCESS(Status) ? STATUS_SUCCESS : Status;

    Bucket = RtlpLockWaitAddressBucket(Address);
    if (!Waiter.Woken)
    {
        RemoveEntryList(&Waiter.Links);
        RtlReleaseSRWLockExclusive(&Bucket->Lock);
        return STATUS_TIMEOUT;
    }
    RtlReleaseSRWLockExclusive(&Bucket->Lock);

    /* A waker already took this waiter and is about to release it */
    NtWaitForKeyedEvent(NULL, &Waiter, FALSE, NULL);
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Wakes every thread waiting on an address.
 */
VOID
NTAPI
RtlWakeAddressAll(
    _In_ const volatile VOID *Address)
{
    RtlpWakeAddress(Address, TRUE);
}

/**
 * @brief
 * Wakes the thread that has waited the longest on an address.
 */
VOID
NTAPI
RtlWakeAddressSingle(
    _In_ const volatile VOID *Address)
{
    RtlpWakeAddress(Address, FALSE);
}

/* EOF */
