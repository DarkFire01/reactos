/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Callable versions of the Rtl memory routines that are macros in the headers
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "psxdllp.h"

#undef RtlZeroMemory
#undef RtlFillMemory
#undef RtlMoveMemory
#undef RtlCopyMemory

VOID
__stdcall
RtlZeroMemory(
    _Out_writes_bytes_all_(Length) PVOID Destination,
    _In_ ULONG Length)
{
    PCHAR Dest = (PCHAR)Destination;

    while (Length--)
        *Dest++ = 0;
}

VOID
__stdcall
RtlFillMemory(
    _Out_writes_bytes_all_(Length) PVOID Destination,
    _In_ ULONG Length,
    _In_ UCHAR Fill)
{
    PCHAR Dest = (PCHAR)Destination;

    while (Length--)
        *Dest++ = (CHAR)Fill;
}

VOID
__stdcall
RtlMoveMemory(
    _Out_writes_bytes_all_(Length) PVOID Destination,
    _In_reads_bytes_(Length) const VOID *Source,
    _In_ ULONG Length)
{
    PCHAR Dest = (PCHAR)Destination;
    const CHAR *Src = (const CHAR *)Source;

    if (Dest <= Src || Dest >= Src + Length)
    {
        while (Length--)
            *Dest++ = *Src++;
    }
    else
    {
        /* Overlapping with the destination above the source, copy backwards */
        Dest += Length;
        Src += Length;
        while (Length--)
            *--Dest = *--Src;
    }
}
