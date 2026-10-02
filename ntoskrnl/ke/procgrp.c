/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Processor group interfaces not covered by processor.c
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <ntoskrnl.h>

#define NDEBUG
#include <debug.h>

/* Every processor lives in group 0 */
USHORT
NTAPI
KeQueryActiveGroupCount(VOID)
{
    return 1;
}

/* Nothing suspends the interrupt clock, so there is no sleep time to take out */
ULONGLONG
NTAPI
KeQueryUnbiasedInterruptTime(VOID)
{
    return KeQueryInterruptTime();
}
