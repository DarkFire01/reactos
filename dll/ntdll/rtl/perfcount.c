/*
 * PROJECT:     ReactOS NTDLL
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     User mode performance counter
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/*
 * These are the RTL half of QueryPerformanceCounter. Windows keeps them in
 * ntdll so that a caller can time things without the kernel32 round trip,
 * and reads the counter straight out of KUSER_SHARED_DATA where the hardware
 * allows it. We go through the syscall instead, which costs the transition
 * but gives the same answer.
 *
 * They are exported rather than merely present because applications import
 * them by name: mozglue.dll pulls in RtlQueryPerformanceCounter at load time,
 * so without it the loader refuses to start Firefox at all -
 * "Failed to snap ntdll.dll!RtlQueryPerformanceCounter for firefox.exe".
 *
 * Both return BOOLEAN, not NTSTATUS. A caller tests the result directly, so
 * returning a status here would read as failure on success - STATUS_SUCCESS
 * is 0.
 */

#include <ntdll.h>

#define NDEBUG
#include <debug.h>

/*
 * @implemented
 */
BOOLEAN
NTAPI
RtlQueryPerformanceCounter(
    _Out_ PLARGE_INTEGER PerformanceCounter)
{
    /* The frequency is not asked for here, and NtQueryPerformanceCounter
       accepts NULL for it */
    return NT_SUCCESS(NtQueryPerformanceCounter(PerformanceCounter, NULL));
}

/*
 * @implemented
 */
BOOLEAN
NTAPI
RtlQueryPerformanceFrequency(
    _Out_ PLARGE_INTEGER PerformanceFrequency)
{
    LARGE_INTEGER Counter;

    /* There is no syscall that answers the frequency on its own, so ask for
       both and drop the count */
    return NT_SUCCESS(NtQueryPerformanceCounter(&Counter, PerformanceFrequency));
}

/* EOF */
