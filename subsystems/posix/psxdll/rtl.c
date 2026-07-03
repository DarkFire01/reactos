/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     PSXDLL Rtl memory re-exports (ordinals 7/8/15). The NT 4.0 psxdll
 *              re-exports a handful of ntdll Rtl helpers the reskit CRT uses; the
 *              other seven (ordinals 6,9-14) are real ntdll exports and are
 *              forwarded in the .spec. RtlFill/Move/ZeroMemory are macros in the
 *              headers, so they are provided as real functions here.
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "psxdllp.h"

#undef RtlZeroMemory
#undef RtlFillMemory
#undef RtlMoveMemory
#undef RtlCopyMemory

VOID __stdcall
RtlZeroMemory(PVOID Destination, ULONG Length)
{
    PCHAR d = (PCHAR)Destination;
    while (Length--)
        *d++ = 0;
}

VOID __stdcall
RtlFillMemory(PVOID Destination, ULONG Length, UCHAR Fill)
{
    PCHAR d = (PCHAR)Destination;
    while (Length--)
        *d++ = (CHAR)Fill;
}

VOID __stdcall
RtlMoveMemory(PVOID Destination, const VOID *Source, ULONG Length)
{
    PCHAR d = (PCHAR)Destination;
    const CHAR *s = (const CHAR *)Source;

    if (d <= s || d >= s + Length)
    {
        while (Length--)
            *d++ = *s++;
    }
    else    // overlap: copy backwards
    {
        d += Length;
        s += Length;
        while (Length--)
            *--d = *--s;
    }
}
