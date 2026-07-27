/*
 * COPYRIGHT:         See COPYING in the top level directory
 * PROJECT:           ReactOS system libraries
 * PURPOSE:           Address-based waiting (RtlWaitOnAddress and friends)
 *
 * NOTES:             This is the RTL half of the Win8 WaitOnAddress API. It is
 *                    the primitive a caller uses to block until the value at
 *                    some address changes, without owning a lock on it.
 *
 *                    Windows backs this with a per-address wait list inside the
 *                    kernel. Here it is a fixed hash table of condition
 *                    variables, so unrelated addresses can share a bucket. That
 *                    is invisible to callers: a thread that wakes re-reads the
 *                    address and goes back to sleep if its own value has not
 *                    changed, which is behaviour every caller must already
 *                    tolerate because spurious wakeups are permitted.
 *
 *                    The one consequence worth spelling out is that
 *                    RtlWakeAddressSingle wakes every waiter in the bucket
 *                    rather than one. Waking too many is a performance
 *                    question and callers re-check and sleep again; waking too
 *                    few would lose a wakeup and hang, so this errs the safe
 *                    way.
 */

/* INCLUDES *****************************************************************/

#include <rtl_vista.h>

#define NDEBUG
#include <debug.h>

/* GLOBALS ******************************************************************/

#define WAIT_ADDRESS_BUCKETS 32

typedef struct _RTLP_WAIT_ADDRESS_BUCKET
{
    RTL_SRWLOCK Lock;
    RTL_CONDITION_VARIABLE Condition;
} RTLP_WAIT_ADDRESS_BUCKET, *PRTLP_WAIT_ADDRESS_BUCKET;

/* Both members are initialised by storing NULL in their Ptr field, so the
   zero-initialised array below is already a table of ready-to-use buckets and
   needs no run-time setup. */
static RTLP_WAIT_ADDRESS_BUCKET RtlpWaitAddressBuckets[WAIT_ADDRESS_BUCKETS];

/* PRIVATE FUNCTIONS ********************************************************/

static
PRTLP_WAIT_ADDRESS_BUCKET
RtlpWaitAddressBucket(
    _In_ const volatile VOID *Address)
{
    ULONG_PTR Value = (ULONG_PTR)Address;

    /* Drop the bits that are always zero for aligned objects, then fold the
       rest down so that addresses far apart do not collide systematically. */
    Value >>= 3;
    Value ^= Value >> 8;
    Value ^= Value >> 16;

    return &RtlpWaitAddressBuckets[Value & (WAIT_ADDRESS_BUCKETS - 1)];
}

static
BOOLEAN
RtlpAddressStillEquals(
    _In_ const volatile VOID *Address,
    _In_ const VOID *CompareAddress,
    _In_ SIZE_T AddressSize)
{
    switch (AddressSize)
    {
        case 1:
            return *(const volatile UCHAR *)Address == *(const UCHAR *)CompareAddress;
        case 2:
            return *(const volatile USHORT *)Address == *(const USHORT *)CompareAddress;
        case 4:
            return *(const volatile ULONG *)Address == *(const ULONG *)CompareAddress;
        case 8:
            return *(const volatile ULONGLONG *)Address == *(const ULONGLONG *)CompareAddress;
        default:
            return FALSE;
    }
}

/* EXPORTED FUNCTIONS ********************************************************/

/*
 * @implemented
 */
NTSTATUS
NTAPI
RtlWaitOnAddress(
    _In_ const volatile VOID *Address,
    _In_ PVOID CompareAddress,
    _In_ SIZE_T AddressSize,
    _In_opt_ PLARGE_INTEGER Timeout)
{
    PRTLP_WAIT_ADDRESS_BUCKET Bucket;
    LARGE_INTEGER Deadline;
    PLARGE_INTEGER SleepTimeout = NULL;
    NTSTATUS Status = STATUS_SUCCESS;

    if (AddressSize != 1 && AddressSize != 2 &&
        AddressSize != 4 && AddressSize != 8)
    {
        return STATUS_INVALID_PARAMETER;
    }

    /* Resolve the caller's timeout to a single absolute deadline up front. A
       relative timeout must not restart every time we come back around the
       loop after a wakeup that was not ours, which is what re-passing it would
       do. */
    if (Timeout != NULL)
    {
        if (Timeout->QuadPart < 0)
        {
            NtQuerySystemTime(&Deadline);
            Deadline.QuadPart -= Timeout->QuadPart;
        }
        else
        {
            Deadline.QuadPart = Timeout->QuadPart;
        }
        SleepTimeout = &Deadline;
    }

    Bucket = RtlpWaitAddressBucket(Address);

    RtlAcquireSRWLockExclusive(&Bucket->Lock);
    while (RtlpAddressStillEquals(Address, CompareAddress, AddressSize))
    {
        Status = RtlSleepConditionVariableSRW(&Bucket->Condition,
                                              &Bucket->Lock,
                                              SleepTimeout,
                                              0);
        if (Status == STATUS_TIMEOUT)
            break;
    }
    RtlReleaseSRWLockExclusive(&Bucket->Lock);

    return Status;
}

/*
 * @implemented
 */
VOID
NTAPI
RtlWakeAddressAll(
    _In_ const volatile VOID *Address)
{
    PRTLP_WAIT_ADDRESS_BUCKET Bucket = RtlpWaitAddressBucket(Address);

    /* Taking the lock here is what closes the window between a waiter
       finding the value unchanged and it actually going to sleep. */
    RtlAcquireSRWLockExclusive(&Bucket->Lock);
    RtlReleaseSRWLockExclusive(&Bucket->Lock);

    RtlWakeAllConditionVariable(&Bucket->Condition);
}

/*
 * @implemented
 */
VOID
NTAPI
RtlWakeAddressSingle(
    _In_ const volatile VOID *Address)
{
    /* Deliberately a wake-all: see the note at the top of this file. Buckets
       are shared between addresses, so waking exactly one thread could wake a
       waiter on some other address and lose this wakeup entirely. */
    RtlWakeAddressAll(Address);
}

/* EOF */
