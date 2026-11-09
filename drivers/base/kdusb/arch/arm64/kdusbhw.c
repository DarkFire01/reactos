/*
 * PROJECT:     ReactOS USB Kernel Debugger Transport
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     ARM64 stall routine on the generic timer
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "kdusb.h"

/*
 * Phase 0 runs before the HAL reads the timer frequency, so its stall cannot
 * be trusted yet. The architected counter has run since reset, and firmware
 * leaves its rate in CNTFRQ_EL0.
 */
#define KDUSB_CNTFRQ_EL0    ARM64_SYSREG(3, 3, 14, 0, 0)
#define KDUSB_CNTVCT_EL0    ARM64_SYSREG(3, 3, 14, 0, 2)

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
    ULONG64 Frequency;
    ULONG64 Ticks;
    ULONG64 Start;

    Frequency = _ReadStatusReg(KDUSB_CNTFRQ_EL0) & 0xFFFFFFFF;
    if (Frequency == 0)
    {
        KeStallExecutionProcessor(Microseconds);
        return;
    }

    /* Rounded up, a controller timeout must never come up short */
    Ticks = ((ULONG64)Microseconds * Frequency + 999999) / 1000000;

    /* Keep the first read from being taken ahead of earlier register writes */
    __isb(_ARM64_BARRIER_SY);
    Start = _ReadStatusReg(KDUSB_CNTVCT_EL0);

    while (_ReadStatusReg(KDUSB_CNTVCT_EL0) - Start < Ticks)
        YieldProcessor();
}
