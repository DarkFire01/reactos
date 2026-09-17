/*
 * PROJECT:     ReactOS Networking Debugging Module
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Architecture-specific kdnet timing primitives (amd64)
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "../kdnet.h"

/*
 * Time source for the early kdnet init path.
 *
 * kdnet runs inside KdInitSystem(0), called from KiSystemStartup BEFORE the HAL
 * calibrates KeGetPcr()->StallScaleFactor (still INITIAL_STALL_COUNT == 100), so
 * KeStallExecutionProcessor under-delays by ~1000x and every timeout in the init
 * path (auto-negotiation, DHCP, ARP) expires almost immediately. We read the
 * timestamp counter instead, and take its rate from CPUID on any part that
 * reports it. The 8254 is the last resort: on a UEFI platform channel 2 is often
 * not wired to anything, so it is measured by reading the count back rather than
 * by watching the OUT pin on port 0x61.
 */

#define PIT_FREQ              1193182u
#define PIT_CH2_DATA          0x42
#define PIT_CMD               0x43
#define PIT_CH2_GATE          0x61  /* bit0 = gate enable, bit1 = speaker */

#define PIT_CMD_CH2_MODE0     0xB0  /* channel 2, lo/hi byte, mode 0, binary */
#define PIT_CMD_CH2_LATCH     0x80  /* channel 2, latch count for read-back */

#define PIT_GATE_ENABLE       0x01
#define PIT_GATE_SPEAKER      0x02

/* Countdown the channel starts from, and the span measured over (~41 ms). The
 * difference leaves the probe below room to consume part of the countdown. */
#define PIT_START_COUNT       0xFFFF
#define PIT_WINDOW_TICKS      0xC000

/* The count moves every ~838 ns, so a few reads tell a running channel from a
 * dead one. The window budget is only reached if the channel stops midway. */
#define PIT_PROBE_READS       64
#define PIT_WINDOW_READS      100000

/* Used when nothing reports a rate. Assumes ~3 GHz: over-delaying the transport
 * costs boot time, under-delaying expires an extension's link wait early. */
#define DEFAULT_TICKS_PER_US  3000

/* A rate outside this is a misread rather than a real part. */
#define MIN_TICKS_PER_US      50
#define MAX_TICKS_PER_US      100000

static ULONG64 KdNetTicksPerUs = 0;

ULONG64
KdNetReadTimeStampCounter(VOID)
{
    return __rdtsc();
}

/**
 * @brief
 * Takes the timestamp counter rate from CPUID.
 *
 * Leaf 0x15 reports the counter's ratio against the core crystal clock along
 * with the crystal's frequency; leaf 0x16 reports the base frequency the
 * counter runs at. Both leaves read back as zero on a part that does not
 * implement them, so a zero field is the "not reported" answer.
 *
 * @param[out] Source
 * Receives the name of the leaf that answered.
 *
 * @return
 * Ticks per microsecond, or zero when neither leaf answers.
 */
static
ULONG64
KdNetQueryCpuidTicksPerMicrosecond(
    _Out_ PCSTR *Source)
{
    int Regs[4];
    ULONG MaxLeaf;
    ULONG64 TicksPerUs;

    *Source = "none";

    __cpuid(Regs, 0);
    MaxLeaf = (ULONG)Regs[0];

    if (MaxLeaf >= 0x15)
    {
        /* EAX: ratio denominator, EBX: numerator, ECX: crystal clock in Hz. */
        __cpuidex(Regs, 0x15, 0);
        if (Regs[0] != 0 && Regs[1] != 0 && Regs[2] != 0)
        {
            TicksPerUs = ((ULONG64)(ULONG)Regs[2] * (ULONG)Regs[1]) /
                         ((ULONG64)(ULONG)Regs[0] * 1000000ULL);

            if (TicksPerUs >= MIN_TICKS_PER_US && TicksPerUs <= MAX_TICKS_PER_US)
            {
                *Source = "CPUID 15h";
                return TicksPerUs;
            }
        }
    }

    if (MaxLeaf >= 0x16)
    {
        /* EAX bits 15:0: base frequency in MHz, which is the counter's rate. */
        __cpuidex(Regs, 0x16, 0);
        TicksPerUs = (ULONG64)((ULONG)Regs[0] & 0xFFFF);

        if (TicksPerUs >= MIN_TICKS_PER_US && TicksPerUs <= MAX_TICKS_PER_US)
        {
            *Source = "CPUID 16h";
            return TicksPerUs;
        }
    }

    return 0;
}

/**
 * @brief
 * Latches and reads channel 2's current count.
 *
 * @return
 * The count.
 */
static
ULONG
KdNetReadPitCount(VOID)
{
    ULONG Count;

    WRITE_PORT_UCHAR((PUCHAR)PIT_CMD, PIT_CMD_CH2_LATCH);
    Count = READ_PORT_UCHAR((PUCHAR)PIT_CH2_DATA);
    Count |= (ULONG)READ_PORT_UCHAR((PUCHAR)PIT_CH2_DATA) << 8;

    return Count;
}

/**
 * @brief
 * Measures the timestamp counter against 8254 channel 2.
 *
 * The channel is checked for movement first. A platform that does not decode
 * the gate, or has the channel switched off, answers in a handful of port reads
 * instead of stalling the debugger for as long as the measurement would take.
 *
 * @return
 * Ticks per microsecond, or zero when the channel is not usable.
 */
static
ULONG64
KdNetQueryPitTicksPerMicrosecond(VOID)
{
    UCHAR Gate;
    ULONG Reads, First, Last, Target;
    ULONG64 Start, End, ElapsedUs, TicksPerUs;
    BOOLEAN Running = FALSE;

    /* Enable the channel 2 gate, leave the speaker disconnected. */
    Gate = READ_PORT_UCHAR((PUCHAR)PIT_CH2_GATE);
    WRITE_PORT_UCHAR((PUCHAR)PIT_CH2_GATE,
                     (UCHAR)((Gate & ~PIT_GATE_SPEAKER) | PIT_GATE_ENABLE));

    WRITE_PORT_UCHAR((PUCHAR)PIT_CMD, PIT_CMD_CH2_MODE0);
    WRITE_PORT_UCHAR((PUCHAR)PIT_CH2_DATA, (UCHAR)(PIT_START_COUNT & 0xFF));
    WRITE_PORT_UCHAR((PUCHAR)PIT_CH2_DATA, (UCHAR)(PIT_START_COUNT >> 8));

    for (Reads = 0; Reads < PIT_PROBE_READS; Reads++)
    {
        if (KdNetReadPitCount() < PIT_START_COUNT)
        {
            Running = TRUE;
            break;
        }
    }

    if (!Running)
    {
        WRITE_PORT_UCHAR((PUCHAR)PIT_CH2_GATE,
                         (UCHAR)(Gate & ~(PIT_GATE_ENABLE | PIT_GATE_SPEAKER)));
        return 0;
    }

    Start = __rdtsc();
    First = KdNetReadPitCount();
    Last = First;

    if (First > PIT_WINDOW_TICKS)
    {
        Target = First - PIT_WINDOW_TICKS;

        for (Reads = 0; Reads < PIT_WINDOW_READS; Reads++)
        {
            Last = KdNetReadPitCount();
            if (Last <= Target)
                break;
        }
    }

    End = __rdtsc();
    WRITE_PORT_UCHAR((PUCHAR)PIT_CH2_GATE,
                     (UCHAR)(Gate & ~(PIT_GATE_ENABLE | PIT_GATE_SPEAKER)));

    /* Measure over what the count actually moved, not over what was asked for. */
    if (First <= PIT_WINDOW_TICKS || Last >= First)
        return 0;

    ElapsedUs = ((ULONG64)(First - Last) * 1000000ULL) / PIT_FREQ;
    if (ElapsedUs == 0)
        return 0;

    TicksPerUs = (End - Start) / ElapsedUs;
    if (TicksPerUs < MIN_TICKS_PER_US || TicksPerUs > MAX_TICKS_PER_US)
        return 0;

    return TicksPerUs;
}

ULONG64
KdNetGetTicksPerMicrosecond(VOID)
{
    PCSTR Source;
    ULONG64 TicksPerUs;

    if (KdNetTicksPerUs != 0)
        return KdNetTicksPerUs;

    if (FrLdrDbgPrint)
        FrLdrDbgPrint("kdnet: calibrating the timestamp counter\n");

    TicksPerUs = KdNetQueryCpuidTicksPerMicrosecond(&Source);

    if (TicksPerUs == 0)
    {
        TicksPerUs = KdNetQueryPitTicksPerMicrosecond();
        Source = "PIT ch2";
    }

    if (TicksPerUs == 0)
    {
        TicksPerUs = DEFAULT_TICKS_PER_US;
        Source = "default";
    }

    KdNetTicksPerUs = TicksPerUs;

    if (FrLdrDbgPrint)
        FrLdrDbgPrint("kdnet: %lu ticks/us from %s\n", (ULONG)TicksPerUs, Source);

    return KdNetTicksPerUs;
}
