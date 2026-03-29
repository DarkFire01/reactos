/*
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         ReactOS Runtime Library
 * PURPOSE:         Vista+ performance / interrupt time helpers for ntdll
 */

#include <rtl.h>

#define NDEBUG
#include <debug.h>

BOOL
WINAPI
RtlQueryPerformanceCounter(
    _Out_ PLARGE_INTEGER PerformanceCount)
{
    LARGE_INTEGER Frequency;
    NTSTATUS Status;

    Status = NtQueryPerformanceCounter(PerformanceCount, &Frequency);
    if (Frequency.QuadPart == 0)
        Status = STATUS_NOT_IMPLEMENTED;

    if (!NT_SUCCESS(Status))
    {
        RtlSetLastWin32Error(RtlNtStatusToDosError(Status));
        return FALSE;
    }
    return TRUE;
}

BOOL
WINAPI
RtlQueryPerformanceFrequency(
    _Out_ PLARGE_INTEGER Frequency)
{
    LARGE_INTEGER Count;
    NTSTATUS Status;

    Status = NtQueryPerformanceCounter(&Count, Frequency);
    if (Frequency->QuadPart == 0)
        Status = STATUS_NOT_IMPLEMENTED;

    if (!NT_SUCCESS(Status))
    {
        RtlSetLastWin32Error(RtlNtStatusToDosError(Status));
        return FALSE;
    }
    return TRUE;
}

BOOL
WINAPI
RtlQueryUnbiasedInterruptTime(
    _Out_ PULONGLONG UnbiasedTime)
{
    ULONG High1, Low, High2;
    ULONGLONG InterruptTime;

    if (!UnbiasedTime)
    {
        RtlSetLastWin32Error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    do
    {
        High1 = SharedUserData->InterruptTime.High1Time;
        Low = SharedUserData->InterruptTime.LowPart;
        High2 = SharedUserData->InterruptTime.High2Time;
    } while (High1 != High2);

    InterruptTime = ((ULONGLONG)High1 << 32) | Low;
    *UnbiasedTime = InterruptTime + SharedUserData->InterruptTimeBias;
    return TRUE;
}
