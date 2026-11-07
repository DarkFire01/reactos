/*
 * PROJECT:     ReactOS Runtime Library
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Performance counter queries
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *****************************************************************/

#include <rtl_vista.h>

#define NDEBUG
#include <debug.h>

/* FUNCTIONS ****************************************************************/

/**
 * @brief
 * Reads the performance counter.
 */
BOOLEAN
NTAPI
RtlQueryPerformanceCounter(
    _Out_ PLARGE_INTEGER PerformanceCounter)
{
    NtQueryPerformanceCounter(PerformanceCounter, NULL);
    return TRUE;
}

/**
 * @brief
 * Reads how many times per second the performance counter ticks.
 */
BOOLEAN
NTAPI
RtlQueryPerformanceFrequency(
    _Out_ PLARGE_INTEGER PerformanceFrequency)
{
    LARGE_INTEGER Counter;

    NtQueryPerformanceCounter(&Counter, PerformanceFrequency);
    return TRUE;
}

/* EOF */
