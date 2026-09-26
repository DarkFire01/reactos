#include "k32_vista.h"

#define NDEBUG
#include <debug.h>

VOID
WINAPI
AcquireSRWLockExclusive(PSRWLOCK Lock)
{
    RtlAcquireSRWLockExclusive((PRTL_SRWLOCK)Lock);
}

VOID
WINAPI
AcquireSRWLockShared(PSRWLOCK Lock)
{
    RtlAcquireSRWLockShared((PRTL_SRWLOCK)Lock);
}

VOID
WINAPI
InitializeConditionVariable(PCONDITION_VARIABLE ConditionVariable)
{
    RtlInitializeConditionVariable((PRTL_CONDITION_VARIABLE)ConditionVariable);
}

VOID
WINAPI
InitializeSRWLock(PSRWLOCK Lock)
{
    RtlInitializeSRWLock((PRTL_SRWLOCK)Lock);
}

VOID
WINAPI
ReleaseSRWLockExclusive(PSRWLOCK Lock)
{
    RtlReleaseSRWLockExclusive((PRTL_SRWLOCK)Lock);
}

VOID
WINAPI
ReleaseSRWLockShared(PSRWLOCK Lock)
{
    RtlReleaseSRWLockShared((PRTL_SRWLOCK)Lock);
}

FORCEINLINE
PLARGE_INTEGER
GetNtTimeout(PLARGE_INTEGER Time, DWORD Timeout)
{
    if (Timeout == INFINITE) return NULL;
    Time->QuadPart = (ULONGLONG)Timeout * -10000;
    return Time;
}

BOOL
WINAPI
SleepConditionVariableCS(PCONDITION_VARIABLE ConditionVariable, PCRITICAL_SECTION CriticalSection, DWORD Timeout)
{
    NTSTATUS Status;
    LARGE_INTEGER Time;

    Status = RtlSleepConditionVariableCS(ConditionVariable, (PRTL_CRITICAL_SECTION)CriticalSection, GetNtTimeout(&Time, Timeout));
    if (!NT_SUCCESS(Status) || Status == STATUS_TIMEOUT)
    {
        SetLastError(RtlNtStatusToDosError(Status));
        return FALSE;
    }
    return TRUE;
}

BOOL
WINAPI
SleepConditionVariableSRW(PCONDITION_VARIABLE ConditionVariable, PSRWLOCK Lock, DWORD Timeout, ULONG Flags)
{
    NTSTATUS Status;
    LARGE_INTEGER Time;

    Status = RtlSleepConditionVariableSRW(ConditionVariable, Lock, GetNtTimeout(&Time, Timeout), Flags);
    if (!NT_SUCCESS(Status) || Status == STATUS_TIMEOUT)
    {
        SetLastError(RtlNtStatusToDosError(Status));
        return FALSE;
    }
    return TRUE;
}

VOID
WINAPI
WakeAllConditionVariable(PCONDITION_VARIABLE ConditionVariable)
{
    RtlWakeAllConditionVariable((PRTL_CONDITION_VARIABLE)ConditionVariable);
}

VOID
WINAPI
WakeConditionVariable(PCONDITION_VARIABLE ConditionVariable)
{
    RtlWakeConditionVariable((PRTL_CONDITION_VARIABLE)ConditionVariable);
}


/*
* @implemented
*/
BOOL WINAPI InitializeCriticalSectionEx(OUT LPCRITICAL_SECTION lpCriticalSection,
                                        IN DWORD dwSpinCount,
                                        IN DWORD flags)
{
    NTSTATUS Status;

    /* FIXME: Flags ignored */

    /* Initialize the critical section */
    Status = RtlInitializeCriticalSectionAndSpinCount(
        (PRTL_CRITICAL_SECTION)lpCriticalSection,
        dwSpinCount);
    if (!NT_SUCCESS(Status))
    {
        /* Set failure code */
        SetLastError(RtlNtStatusToDosError(Status));
        return FALSE;
    }

    /* Success */
    return TRUE;
}


/**
 * @brief
 * Waits for the value at an address to change.
 *
 * @param[in] Address
 * The value to watch.
 *
 * @param[in] CompareAddress
 * The value it currently holds. The wait returns at once if the two already
 * differ.
 *
 * @param[in] AddressSize
 * How wide the value is, which may be one, two, four or eight bytes.
 *
 * @param[in] Milliseconds
 * How long to wait, or INFINITE to wait for as long as it takes.
 *
 * @return
 * TRUE once the value changed, FALSE with the last error set otherwise.
 */
BOOL
WINAPI
WaitOnAddress(
    _In_ volatile VOID *Address,
    _In_ PVOID CompareAddress,
    _In_ SIZE_T AddressSize,
    _In_ DWORD Milliseconds)
{
    LARGE_INTEGER Timeout;
    NTSTATUS Status;

    if (Milliseconds == INFINITE)
    {
        Status = RtlWaitOnAddress((PVOID)Address, CompareAddress, AddressSize, NULL);
    }
    else
    {
        Timeout.QuadPart = Milliseconds * -10000LL;
        Status = RtlWaitOnAddress((PVOID)Address, CompareAddress, AddressSize, &Timeout);
    }

    if (Status != STATUS_SUCCESS)
    {
        SetLastError(RtlNtStatusToDosError(Status));
        return FALSE;
    }

    return TRUE;
}

/**
 * @brief
 * Wakes one waiter on an address.
 */
VOID
WINAPI
WakeByAddressSingle(
    _In_ PVOID Address)
{
    RtlWakeAddressSingle(Address);
}

/**
 * @brief
 * Wakes every waiter on an address.
 */
VOID
WINAPI
WakeByAddressAll(
    _In_ PVOID Address)
{
    RtlWakeAddressAll(Address);
}
