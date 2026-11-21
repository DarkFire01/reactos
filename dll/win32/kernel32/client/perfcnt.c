/*
 * COPYRIGHT:            See COPYING in the top level directory
 * PROJECT:              ReactOS Win32 Base API
 * FILE:                 dll/win32/kernel32/client/perfcnt.c
 * PURPOSE:              Performance Counter
 * PROGRAMMER:           Eric Kohl
 */

/* INCLUDES *******************************************************************/

#include <k32.h>

#define NDEBUG
#include <debug.h>

/* FUNCTIONS ******************************************************************/

/*
 * @implemented
 */
BOOL
WINAPI
QueryPerformanceCounter(OUT PLARGE_INTEGER lpPerformanceCount)
{
    LARGE_INTEGER Frequency;
    NTSTATUS Status;

    Status = NtQueryPerformanceCounter(lpPerformanceCount, &Frequency);
    if (Frequency.QuadPart == 0) Status = STATUS_NOT_IMPLEMENTED;

    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        return FALSE;
    }

    return TRUE;
}

/*
 * @implemented
 */
BOOL
WINAPI
QueryPerformanceFrequency(OUT PLARGE_INTEGER lpFrequency)
{
    LARGE_INTEGER Count;
    NTSTATUS Status;

    Status = NtQueryPerformanceCounter(&Count, lpFrequency);
    if (lpFrequency->QuadPart == 0) Status = STATUS_NOT_IMPLEMENTED;

    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        return FALSE;
    }

    return TRUE;
}

/**
 * @brief Gets the frequency of the platform's auxiliary counter.
 * @return E_NOTIMPL, the platform has no auxiliary counter.
 */
HRESULT
WINAPI
QueryAuxiliaryCounterFrequency(
    _Out_ PULONGLONG AuxiliaryCounterFrequency)
{
    if (AuxiliaryCounterFrequency)
        *AuxiliaryCounterFrequency = 0;

    return E_NOTIMPL;
}

/**
 * @brief Converts an auxiliary counter value to a performance counter value.
 * @return E_NOTIMPL, there is no auxiliary counter to convert from.
 */
HRESULT
WINAPI
ConvertAuxiliaryCounterToPerformanceCounter(
    _In_ ULONGLONG AuxiliaryCounterValue,
    _Out_ PULONGLONG PerformanceCounterValue,
    _Out_opt_ PULONGLONG ConversionError)
{
    UNREFERENCED_PARAMETER(AuxiliaryCounterValue);

    if (PerformanceCounterValue)
        *PerformanceCounterValue = 0;
    if (ConversionError)
        *ConversionError = 0;

    return E_NOTIMPL;
}

/**
 * @brief Converts a performance counter value to an auxiliary counter value.
 * @return E_NOTIMPL, there is no auxiliary counter to convert to.
 */
HRESULT
WINAPI
ConvertPerformanceCounterToAuxiliaryCounter(
    _In_ ULONGLONG PerformanceCounterValue,
    _Out_ PULONGLONG AuxiliaryCounterValue,
    _Out_opt_ PULONGLONG ConversionError)
{
    UNREFERENCED_PARAMETER(PerformanceCounterValue);

    if (AuxiliaryCounterValue)
        *AuxiliaryCounterValue = 0;
    if (ConversionError)
        *ConversionError = 0;

    return E_NOTIMPL;
}

/* EOF */
