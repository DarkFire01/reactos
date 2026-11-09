/*
 * PROJECT:     ReactOS Networking Debugging Module
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     kdnet primitives for architectures without their own
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "kdnet.h"

ULONG64
KdNetReadTimeStampCounter(VOID)
{
    return (ULONG64)KeQueryPerformanceCounter(NULL).QuadPart;
}

ULONG64
KdNetGetTicksPerMicrosecond(VOID)
{
    LARGE_INTEGER Frequency;

    KeQueryPerformanceCounter(&Frequency);

    /* Rounded up, and never zero so a stall still ends */
    return max(1, ((ULONG64)Frequency.QuadPart + 999999) / 1000000);
}

BOOLEAN
NTAPI
KdNetArchSerialInitialize(VOID)
{
    return FALSE;
}

VOID
NTAPI
KdNetArchSerialPutByte(
    _In_ UCHAR Byte)
{
    UNREFERENCED_PARAMETER(Byte);
}
