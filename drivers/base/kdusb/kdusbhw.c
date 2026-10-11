/*
 * PROJECT:     ReactOS USB Kernel Debugger Transport
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Stall routine that works before the HAL calibrates its own
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "kdusb.h"

#if defined(_M_IX86) || defined(_M_AMD64)

/*
 * Phase 0 runs before the HAL measures StallScaleFactor, so
 * KeStallExecutionProcessor comes up about a thousand times short and every
 * controller timeout expires at once. The timestamp counter is used instead.
 */

#define PIT_FREQUENCY           1193182
#define PIT_CH2_DATA            0x42
#define PIT_COMMAND             0x43
#define PIT_CH2_GATE            0x61

#define PIT_CH2_MODE0           0xB0
#define PIT_CH2_LATCH           0x80
#define PIT_GATE_ENABLE         0x01
#define PIT_GATE_SPEAKER        0x02

#define PIT_START_COUNT         0xFFFF
#define PIT_SPAN                0xC000
#define PIT_PROBE_READS         64
#define PIT_SPAN_READS          100000

/* Used when nothing reports a rate; too long is safer than too short */
#define TICKS_PER_US_DEFAULT    3000
#define TICKS_PER_US_MIN        50
#define TICKS_PER_US_MAX        100000

static ULONG64 KdUsbTicksPerUs;

/**
 * @brief
 * Checks a measured counter rate for sanity.
 *
 * @param[in] TicksPerUs
 * The measured rate.
 *
 * @return
 * TicksPerUs when plausible, otherwise zero.
 */
static
ULONG64
NTAPI
KdUsbPlausibleRate(
    _In_ ULONG64 TicksPerUs)
{
    if (TicksPerUs < TICKS_PER_US_MIN || TicksPerUs > TICKS_PER_US_MAX)
        return 0;

    return TicksPerUs;
}

/**
 * @brief
 * Reads the timestamp counter rate from CPUID leaf 15h or 16h.
 *
 * @return
 * Ticks per microsecond, or zero when neither leaf reports it.
 */
static
ULONG64
NTAPI
KdUsbCpuidRate(VOID)
{
    INT Registers[4];
    ULONG MaxLeaf;
    ULONG64 Rate;

    __cpuid(Registers, 0);
    MaxLeaf = (ULONG)Registers[0];

    if (MaxLeaf >= 0x15)
    {
        /* EAX denominator, EBX numerator, ECX crystal clock in Hz */
        __cpuidex(Registers, 0x15, 0);
        if (Registers[0] != 0 && Registers[1] != 0 && Registers[2] != 0)
        {
            Rate = ((ULONG64)(ULONG)Registers[2] * (ULONG)Registers[1]) /
                   ((ULONG64)(ULONG)Registers[0] * 1000000);

            if (KdUsbPlausibleRate(Rate) != 0)
            {
                KdUsbTrace("TSC %I64u ticks/us from CPUID 15h\n", Rate);
                return Rate;
            }
        }
    }

    if (MaxLeaf >= 0x16)
    {
        /* EAX bits 15:0 hold the base frequency in MHz */
        __cpuidex(Registers, 0x16, 0);
        Rate = (ULONG)Registers[0] & 0xFFFF;

        if (KdUsbPlausibleRate(Rate) != 0)
        {
            KdUsbTrace("TSC %I64u ticks/us from CPUID 16h\n", Rate);
            return Rate;
        }
    }

    return 0;
}

/**
 * @brief
 * Latches and reads the 8254 channel 2 count.
 *
 * @return
 * The current count.
 */
static
ULONG
NTAPI
KdUsbReadPit(VOID)
{
    ULONG Count;

    WRITE_PORT_UCHAR((PUCHAR)PIT_COMMAND, PIT_CH2_LATCH);
    Count = READ_PORT_UCHAR((PUCHAR)PIT_CH2_DATA);
    Count |= (ULONG)READ_PORT_UCHAR((PUCHAR)PIT_CH2_DATA) << 8;

    return Count;
}

/**
 * @brief
 * Times the timestamp counter against 8254 channel 2.
 *
 * The channel is checked for movement first, so a platform that leaves it
 * dead answers after a few port reads.
 *
 * @return
 * Ticks per microsecond, or zero when the channel cannot be used.
 */
static
ULONG64
NTAPI
KdUsbPitRate(VOID)
{
    ULONG64 Start;
    ULONG64 End;
    ULONG64 Elapsed;
    ULONG First;
    ULONG Last;
    ULONG Reads;
    UCHAR Gate;
    BOOLEAN Moving = FALSE;

    Gate = READ_PORT_UCHAR((PUCHAR)PIT_CH2_GATE);
    WRITE_PORT_UCHAR((PUCHAR)PIT_CH2_GATE, (Gate & ~PIT_GATE_SPEAKER) | PIT_GATE_ENABLE);

    WRITE_PORT_UCHAR((PUCHAR)PIT_COMMAND, PIT_CH2_MODE0);
    WRITE_PORT_UCHAR((PUCHAR)PIT_CH2_DATA, PIT_START_COUNT & 0xFF);
    WRITE_PORT_UCHAR((PUCHAR)PIT_CH2_DATA, PIT_START_COUNT >> 8);

    for (Reads = 0; Reads < PIT_PROBE_READS && !Moving; Reads++)
        Moving = (KdUsbReadPit() < PIT_START_COUNT);

    Start = __rdtsc();
    First = KdUsbReadPit();
    Last = First;

    if (Moving && First > PIT_SPAN)
    {
        for (Reads = 0; Reads < PIT_SPAN_READS && Last > First - PIT_SPAN; Reads++)
            Last = KdUsbReadPit();
    }

    End = __rdtsc();
    WRITE_PORT_UCHAR((PUCHAR)PIT_CH2_GATE, Gate & ~(PIT_GATE_ENABLE | PIT_GATE_SPEAKER));

    if (!Moving || Last >= First)
        return 0;

    Elapsed = ((ULONG64)(First - Last) * 1000000) / PIT_FREQUENCY;
    if (Elapsed == 0)
        return 0;

    End = KdUsbPlausibleRate((End - Start) / Elapsed);
    if (End != 0)
        KdUsbTrace("TSC %I64u ticks/us from the PIT\n", End);

    return End;
}

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
    ULONG64 Start;
    ULONG64 Ticks;

    if (KdUsbTicksPerUs == 0)
    {
        KdUsbTicksPerUs = KdUsbCpuidRate();
        if (KdUsbTicksPerUs == 0)
            KdUsbTicksPerUs = KdUsbPitRate();

        if (KdUsbTicksPerUs == 0)
        {
            KdUsbTicksPerUs = TICKS_PER_US_DEFAULT;
            KdUsbTrace("TSC rate unknown, assuming %u ticks/us\n", TICKS_PER_US_DEFAULT);
        }
    }

    Ticks = (ULONG64)Microseconds * KdUsbTicksPerUs;
    for (Start = __rdtsc(); __rdtsc() - Start < Ticks; )
        YieldProcessor();
}

#else

VOID
NTAPI
KdUsbStall(
    _In_ ULONG Microseconds)
{
    KeStallExecutionProcessor(Microseconds);
}

#endif
