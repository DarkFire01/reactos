/*
 * PROJECT:     ReactOS system libraries
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Implementation for WaitOnAddress / WakeByAddress APIs
 * COPYRIGHT:   Copyright 2025 <justin.miller@reactos.org>
 */

/* INCLUDES *****************************************************************/

#include <rtl_vista.h>

#define NDEBUG
#include <debug.h>

/* GLOBALS ******************************************************************/

#define ADDRESS_WAIT_BUCKETS        128
#define ADDRESS_WAIT_SPIN_COUNT     1024

/* Low bits of a bucket value, the rest points at the newest waiter */
#define ADDRESS_BUCKET_WAKE_ALL     ((ULONG_PTR)1)
#define ADDRESS_BUCKET_OWNED        ((ULONG_PTR)2)
#define ADDRESS_BUCKET_FLAGS        (ADDRESS_BUCKET_WAKE_ALL | ADDRESS_BUCKET_OWNED)

typedef enum _ADDRESS_WAITER_STATE
{
    WaiterSleeping,
    WaiterSpinning,
    WaiterWoken,
    WaiterLeaving
} ADDRESS_WAITER_STATE;

typedef struct _ADDRESS_WAITER
{
    struct _ADDRESS_WAITER *Older;
    struct _ADDRESS_WAITER *Newer;

    /* Set on the oldest waiter, and cached on the newest once back links are built */
    struct _ADDRESS_WAITER *Tail;

    const volatile VOID *Address;
    HANDLE ThreadId;
    volatile LONG State;
} ADDRESS_WAITER, *PADDRESS_WAITER;

C_ASSERT(TYPE_ALIGNMENT(ADDRESS_WAITER) > ADDRESS_BUCKET_FLAGS);

static PVOID volatile RtlpAddressWaitTable[ADDRESS_WAIT_BUCKETS];

/* PRIVATE FUNCTIONS *********************************************************/

static
NTSTATUS
RtlpSleepOnAddressWaiter(
    _Inout_ PADDRESS_WAITER Waiter,
    _In_opt_ PLARGE_INTEGER Timeout);

static
FORCEINLINE
PVOID volatile *
RtlpAddressWaitBucket(
    _In_ const volatile VOID *Address)
{
    return &RtlpAddressWaitTable[((ULONG_PTR)Address >> 5) % ADDRESS_WAIT_BUCKETS];
}

static
FORCEINLINE
PADDRESS_WAITER
RtlpNewestAddressWaiter(
    _In_ ULONG_PTR BucketValue)
{
    return (PADDRESS_WAITER)(BucketValue & ~ADDRESS_BUCKET_FLAGS);
}

static
FORCEINLINE
ULONG_PTR
RtlpExchangeAddressBucket(
    _Inout_ PVOID volatile *Bucket,
    _In_ ULONG_PTR NewValue,
    _In_ ULONG_PTR OldValue)
{
    return (ULONG_PTR)InterlockedCompareExchangePointer(Bucket, (PVOID)NewValue, (PVOID)OldValue);
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
        case sizeof(ULONGLONG):
            return *(const volatile ULONGLONG *)Address == *(PULONGLONG)CompareAddress;
    }

    return FALSE;
}

/**
 * @brief Fills in Newer links for waiters pushed since the last walk.
 *
 * @return The oldest waiter in the list.
 */
static
PADDRESS_WAITER
RtlpRelinkAddressWaiters(
    _Inout_ PADDRESS_WAITER Newest)
{
    PADDRESS_WAITER Waiter;

    Waiter = Newest;
    while (!Waiter->Tail)
    {
        Waiter->Older->Newer = Waiter;
        Waiter = Waiter->Older;
    }

    Newest->Tail = Waiter->Tail;
    return Newest->Tail;
}

/**
 * @brief Marks every waiter in a detached chain as woken.
 */
static
VOID
RtlpWakeAddressWaiterChain(
    _In_opt_ PADDRESS_WAITER Waiter)
{
    PADDRESS_WAITER Older;
    HANDLE ThreadId;

    while (Waiter)
    {
        /* A spinning waiter may return as soon as it sees the new state */
        Older = Waiter->Older;
        ThreadId = Waiter->ThreadId;

        if (InterlockedExchange(&Waiter->State, WaiterWoken) == WaiterSleeping)
            NtAlertThreadByThreadId(ThreadId);

        Waiter = Older;
    }
}

/**
 * @brief Takes ownership of a bucket.
 *
 * @return The owned bucket value, or 0 if the bucket is empty or its
 *         owner was told to wake every waiter instead.
 */
static
ULONG_PTR
RtlpAcquireAddressBucket(
    _Inout_ PVOID volatile *Bucket)
{
    ULONG_PTR Value;
    ULONG_PTR Desired;
    ULONG_PTR Observed;

    Value = (ULONG_PTR)*Bucket;
    while (Value && !(Value & ADDRESS_BUCKET_WAKE_ALL))
    {
        if (Value & ADDRESS_BUCKET_OWNED)
            Desired = Value | ADDRESS_BUCKET_WAKE_ALL;
        else
            Desired = Value | ADDRESS_BUCKET_OWNED;

        Observed = RtlpExchangeAddressBucket(Bucket, Desired, Value);
        if (Observed == Value)
            return (Desired & ADDRESS_BUCKET_WAKE_ALL) ? 0 : Desired;

        Value = Observed;
    }

    return 0;
}

static
VOID
RtlpReleaseAddressBucket(
    _Inout_ PVOID volatile *Bucket,
    _In_ ULONG_PTR Value)
{
    ULONG_PTR Released;
    ULONG_PTR Observed;

    for (;;)
    {
        RtlpRelinkAddressWaiters(RtlpNewestAddressWaiter(Value));

        if (Value & ADDRESS_BUCKET_WAKE_ALL)
            Released = 0;
        else
            Released = (ULONG_PTR)RtlpNewestAddressWaiter(Value);

        Observed = RtlpExchangeAddressBucket(Bucket, Released, Value);
        if (Observed == Value)
            break;

        Value = Observed;
    }

    if (Value & ADDRESS_BUCKET_WAKE_ALL)
        RtlpWakeAddressWaiterChain(RtlpNewestAddressWaiter(Value));
}

static
VOID
RtlpPushAddressWaiter(
    _Inout_ PADDRESS_WAITER Waiter)
{
    PVOID volatile *Bucket;
    ULONG_PTR Value;
    ULONG_PTR Pushed;
    ULONG_PTR Observed;

    Bucket = RtlpAddressWaitBucket(Waiter->Address);
    Value = (ULONG_PTR)*Bucket;

    for (;;)
    {
        Waiter->Older = RtlpNewestAddressWaiter(Value);
        Pushed = (ULONG_PTR)Waiter | (Value & ADDRESS_BUCKET_FLAGS);

        if (Waiter->Older)
        {
            Waiter->Tail = NULL;
            Pushed |= ADDRESS_BUCKET_OWNED;
        }
        else
        {
            Waiter->Tail = Waiter;
        }

        Observed = RtlpExchangeAddressBucket(Bucket, Pushed, Value);
        if (Observed == Value)
            break;

        Value = Observed;
    }

    /* Pushing onto an idle list made us the owner, so build the back links now */
    if ((Pushed & ADDRESS_BUCKET_OWNED) && !(Value & ADDRESS_BUCKET_OWNED))
        RtlpReleaseAddressBucket(Bucket, Pushed);
}

/**
 * @brief Takes a waiter that will not sleep, or timed out, off its list.
 */
static
NTSTATUS
RtlpWithdrawAddressWaiter(
    _Inout_ PADDRESS_WAITER Waiter)
{
    PVOID volatile *Bucket;
    PADDRESS_WAITER Newest;
    PADDRESS_WAITER Current;
    PADDRESS_WAITER Newer;
    PADDRESS_WAITER Older;
    ULONG_PTR Value;
    ULONG_PTR Remaining;
    ULONG_PTR Observed;
    BOOLEAN Found;

    Bucket = RtlpAddressWaitBucket(Waiter->Address);
    Value = RtlpAcquireAddressBucket(Bucket);

    if (!Value)
    {
        /* Whoever holds the bucket will wake us, wait for that */
        if (InterlockedExchange(&Waiter->State, WaiterSpinning) == WaiterWoken)
            return STATUS_SUCCESS;

        return RtlpSleepOnAddressWaiter(Waiter, NULL);
    }

    Found = FALSE;
    Newest = RtlpNewestAddressWaiter(Value);
    Current = Newest;
    Newer = NULL;

    while (Current)
    {
        Older = Current->Older;

        if (Current != Waiter)
        {
            Current->Newer = Newer;
            Newer = Current;
            Current = Older;
            continue;
        }

        Found = TRUE;

        if (Current != Newest)
        {
            Newer->Older = Older;
            if (Older)
                Older->Newer = Newer;
            else
                Newer->Tail = Newer;

            Current = Older;
            continue;
        }

        Remaining = Older ? ((ULONG_PTR)Older | (Value & ADDRESS_BUCKET_FLAGS)) : 0;
        Observed = RtlpExchangeAddressBucket(Bucket, Remaining, Value);
        if (Observed != Value)
        {
            /* New waiters arrived, walk again from the new head */
            Value = Observed;
            Newest = RtlpNewestAddressWaiter(Value);
            Current = Newest;
            Newer = NULL;
            continue;
        }

        /* Emptying the list also gave up ownership */
        if (!Remaining)
            return STATUS_SUCCESS;

        Value = Remaining;
        Older->Newer = NULL;
        Newest = Older;
        Current = Older;
    }

    /* A wake all already detached us, so its alert is on the way */
    if (!Found && InterlockedExchange(&Waiter->State, WaiterSleeping) != WaiterWoken)
        NtWaitForAlertByThreadId((PVOID)Waiter->Address, NULL);

    Newest->Tail = Newer;
    RtlpReleaseAddressBucket(Bucket, Value);
    return STATUS_SUCCESS;
}

static
NTSTATUS
RtlpSleepOnAddressWaiter(
    _Inout_ PADDRESS_WAITER Waiter,
    _In_opt_ PLARGE_INTEGER Timeout)
{
    NTSTATUS Status;
    ULONG Spin;

    for (Spin = 0; Spin < ADDRESS_WAIT_SPIN_COUNT; Spin++)
    {
        if (Waiter->State != WaiterSpinning)
            return STATUS_SUCCESS;

        YieldProcessor();
    }

    if (InterlockedCompareExchange(&Waiter->State, WaiterSleeping, WaiterSpinning) != WaiterSpinning)
        return STATUS_SUCCESS;

    Status = NtWaitForAlertByThreadId((PVOID)Waiter->Address, Timeout);
    if (Status == STATUS_TIMEOUT)
    {
        /* A waker that claimed us first still owes us an alert */
        if (InterlockedExchange(&Waiter->State, WaiterLeaving) == WaiterWoken)
            Status = NtWaitForAlertByThreadId((PVOID)Waiter->Address, NULL);
        else
            RtlpWithdrawAddressWaiter(Waiter);
    }

    if (Status == STATUS_ALERTED)
        Status = STATUS_SUCCESS;

    return Status;
}

static
VOID
RtlpWakeAddressWaiters(
    _In_ const volatile VOID *Address,
    _In_ BOOLEAN WakeAll)
{
    PVOID volatile *Bucket;
    PADDRESS_WAITER Newest;
    PADDRESS_WAITER Current;
    PADDRESS_WAITER Newer;
    PADDRESS_WAITER Older;
    PADDRESS_WAITER AlertList;
    HANDLE ThreadId;
    ULONG_PTR Value;
    ULONG_PTR Remaining;
    ULONG_PTR Observed;
    LONG PreviousState;
    BOOLEAN Rewalk;

    Bucket = RtlpAddressWaitBucket(Address);
    Value = RtlpAcquireAddressBucket(Bucket);
    if (!Value)
        return;

    AlertList = NULL;
    Remaining = Value;

    /* Oldest first, so waiters are released in the order they arrived */
    do
    {
        Rewalk = FALSE;
        Newest = RtlpNewestAddressWaiter(Value);
        Current = RtlpRelinkAddressWaiters(Newest);

        while (Current)
        {
            Newer = Current->Newer;

            if (Current->Address != Address)
            {
                Current = Newer;
                continue;
            }

            Older = Current->Older;

            if (Current == Newest)
            {
                Remaining = Older ? ((ULONG_PTR)Older | (Value & ADDRESS_BUCKET_FLAGS)) : 0;
                Observed = RtlpExchangeAddressBucket(Bucket, Remaining, Value);
                if (Observed != Value)
                {
                    Value = Observed;
                    Remaining = Value;
                    Rewalk = TRUE;
                    break;
                }

                Value = Remaining;
                if (Older)
                {
                    Older->Newer = NULL;
                    Older->Tail = Current->Tail;
                }
            }
            else
            {
                Newer->Older = Older;
                if (Older)
                {
                    Older->Newer = Newer;
                }
                else
                {
                    Newest->Tail = Newer;
                    Newer->Tail = Newer;
                }
            }

            PreviousState = InterlockedExchange(&Current->State, WaiterWoken);
            if (PreviousState != WaiterWoken)
            {
                /* Only a waiter already in the kernel needs an alert */
                if (PreviousState == WaiterSleeping)
                {
                    Current->Older = AlertList;
                    AlertList = Current;
                }

                if (!WakeAll)
                    break;
            }

            Current = Newer;
        }
    } while (Rewalk);

    while (AlertList)
    {
        Current = AlertList;
        AlertList = Current->Older;
        ThreadId = Current->ThreadId;
        NtAlertThreadByThreadId(ThreadId);
    }

    if (Remaining)
        RtlpReleaseAddressBucket(Bucket, Value);
}

/* EXPORTED FUNCTIONS ********************************************************/

/**
 * @brief Sleeps while the value at Address equals the value at CompareAddress.
 *
 * @return STATUS_SUCCESS when woken or when the values already differ,
 *         STATUS_TIMEOUT when Timeout expires, STATUS_INVALID_PARAMETER
 *         when AddressSize is not 1, 2, 4 or 8.
 */
NTSTATUS
NTAPI
RtlWaitOnAddress(
    _In_ const volatile VOID *Address,
    _In_ PVOID CompareAddress,
    _In_ SIZE_T AddressSize,
    _In_opt_ PLARGE_INTEGER Timeout)
{
    ADDRESS_WAITER Waiter;

    switch (AddressSize)
    {
        case sizeof(UCHAR):
        case sizeof(USHORT):
        case sizeof(ULONG):
        case sizeof(ULONGLONG):
            break;

        default:
            return STATUS_INVALID_PARAMETER;
    }

    Waiter.Address = Address;
    Waiter.ThreadId = NtCurrentTeb()->ClientId.UniqueThread;
    Waiter.State = WaiterSpinning;
    Waiter.Newer = NULL;

    /* Queue before comparing so a waker that changes the value always finds us */
    RtlpPushAddressWaiter(&Waiter);

    if (!RtlpAddressHoldsValue(Address, CompareAddress, AddressSize))
        return RtlpWithdrawAddressWaiter(&Waiter);

    return RtlpSleepOnAddressWaiter(&Waiter, Timeout);
}

/**
 * @brief Wakes every thread waiting on Address.
 */
VOID
NTAPI
RtlWakeAddressAll(
    _In_ const volatile VOID *Address)
{
    RtlpWakeAddressWaiters(Address, TRUE);
}

/**
 * @brief Wakes one thread waiting on Address.
 */
VOID
NTAPI
RtlWakeAddressSingle(
    _In_ const volatile VOID *Address)
{
    RtlpWakeAddressWaiters(Address, FALSE);
}

/* EOF */
