/*
 * PROJECT:     ReactOS Networking Debugging Module
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Architecture-specific kdnet primitives (ARM64)
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "kdnet.h"

/*
 * The generic timer counts from reset at the rate firmware leaves in
 * CNTFRQ_EL0, so unlike x86 nothing has to be calibrated before the HAL is up.
 */
#define KDNET_CNTFRQ_EL0            ARM64_SYSREG(3, 3, 14, 0, 0)
#define KDNET_CNTVCT_EL0            ARM64_SYSREG(3, 3, 14, 0, 2)

/* Only for firmware that left CNTFRQ_EL0 at zero; too long is safer than too short */
#define KDNET_DEFAULT_TICKS_PER_US  100

static ULONG64 KdNetTicksPerUs = 0;

ULONG64
KdNetReadTimeStampCounter(VOID)
{
    /* Keep the read from being taken ahead of the code that precedes it */
    __isb(_ARM64_BARRIER_SY);
    return _ReadStatusReg(KDNET_CNTVCT_EL0);
}

ULONG64
KdNetGetTicksPerMicrosecond(VOID)
{
    ULONG64 Frequency;

    if (KdNetTicksPerUs != 0)
        return KdNetTicksPerUs;

    Frequency = _ReadStatusReg(KDNET_CNTFRQ_EL0) & 0xFFFFFFFF;

    /* 19.2 MHz parts are common, rounding up keeps every timeout on the long side */
    KdNetTicksPerUs = (Frequency != 0) ? ((Frequency + 999999) / 1000000) : KDNET_DEFAULT_TICKS_PER_US;

    if (FrLdrDbgPrint)
        FrLdrDbgPrint("kdnet: %lu ticks/us from CNTFRQ_EL0 %lu\n", (ULONG)KdNetTicksPerUs, (ULONG)Frequency);

    return KdNetTicksPerUs;
}

/**
 * @brief
 * Reports whether an early serial port exists.
 *
 * ARM64 platforms have no UART at a fixed address the way PCs have COM1, and
 * this runs before anything could map one, so the trace has nowhere to go.
 *
 * @return
 * FALSE.
 */
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
