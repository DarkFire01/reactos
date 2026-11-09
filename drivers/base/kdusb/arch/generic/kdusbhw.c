/*
 * PROJECT:     ReactOS USB Kernel Debugger Transport
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Stall routine for architectures without their own
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "kdusb.h"

/**
 * @brief
 * Busy-waits for a number of microseconds.
 *
 * @param[in] Microseconds
 * How long to wait.
 */
VOID
NTAPI
KdUsbStall(
    _In_ ULONG Microseconds)
{
    KeStallExecutionProcessor(Microseconds);
}
