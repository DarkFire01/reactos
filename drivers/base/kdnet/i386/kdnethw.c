/*
 * PROJECT:     ReactOS Networking Debugging Module
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Architecture-specific kdnet timing primitives (i386)
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "../kdnet.h"

/*
 * Time source for the early kdnet init path.
 *
 * kdnet runs inside KdInitSystem(0), called from KiSystemStartup BEFORE the HAL
 * calibrates KeGetPcr()->StallScaleFactor (still INITIAL_STALL_COUNT == 100), so
 * KeStallExecutionProcessor under-delays by ~1000x and every timeout in the init
 * path (auto-negotiation, DHCP, ARP) expires almost immediately. We instead read
 * the CPU timestamp counter (TSC) and calibrate it once against the 8254 PIT,
 * which runs at a fixed 1.193182 MHz and needs no OS timing services this early.
 */

#define PIT_FREQ        1193182u
#define PIT_CH2_DATA    0x42
#define PIT_CMD         0x43
#define PIT_CH2_GATE    0x61    /* bit0 = gate enable, bit1 = speaker, bit5 = OUT */

static ULONG64 KdNetTicksPerUs = 0;

ULONG64
KdNetReadTimeStampCounter(VOID)
{
    return __rdtsc();
}

ULONG64
KdNetGetTicksPerMicrosecond(VOID)
{
    UCHAR gate;
    ULONG64 t0, t1, elapsedUs, perUs;
    const ULONG pitCount = 0xFFFF;  /* ~54.9 ms full countdown */
    ULONG spin;
    BOOLEAN asserted = FALSE;

    if (KdNetTicksPerUs != 0)
        return KdNetTicksPerUs;

    /* Enable channel 2 gate (bit0), keep speaker output disconnected (bit1=0). */
    gate = READ_PORT_UCHAR((PUCHAR)PIT_CH2_GATE);
    WRITE_PORT_UCHAR((PUCHAR)PIT_CH2_GATE, (UCHAR)((gate & ~0x02) | 0x01));

    /* Channel 2, access lo/hi byte, mode 0 (interrupt on terminal count), binary. */
    WRITE_PORT_UCHAR((PUCHAR)PIT_CMD, 0xB0);
    WRITE_PORT_UCHAR((PUCHAR)PIT_CH2_DATA, (UCHAR)(pitCount & 0xFF));
    WRITE_PORT_UCHAR((PUCHAR)PIT_CH2_DATA, (UCHAR)(pitCount >> 8));

    t0 = __rdtsc();

    /* In mode 0 the OUT pin (port 0x61 bit5) goes high when the count hits 0.
     * Bound the spin: a port read is ~1 us, so ~8M reads (~8 s worst case) is a
     * generous ceiling. If OUT never asserts (PIT gated off on a UEFI-only box)
     * fall back to a conservative default rather than hang the debugger. */
    for (spin = 0; spin < 8000000u; spin++)
    {
        if (READ_PORT_UCHAR((PUCHAR)PIT_CH2_GATE) & 0x20)
        {
            asserted = TRUE;
            break;
        }
        YieldProcessor();
    }

    if (!asserted)
    {
        WRITE_PORT_UCHAR((PUCHAR)PIT_CH2_GATE, (UCHAR)(gate & ~0x03));
        if (FrLdrDbgPrint)
            FrLdrDbgPrint("kdnet: PIT calibration failed, defaulting to 3000 ticks/us\n");
        KdNetTicksPerUs = 3000;  /* assume ~3 GHz; better an approximate stall than none */
        return KdNetTicksPerUs;
    }

    t1 = __rdtsc();

    /* Drop the gate again. */
    WRITE_PORT_UCHAR((PUCHAR)PIT_CH2_GATE, (UCHAR)(gate & ~0x03));

    elapsedUs = ((ULONG64)pitCount * 1000000ULL) / PIT_FREQ;   /* ~54925 us */
    perUs = (t1 - t0) / elapsedUs;
    if (perUs == 0)
        perUs = 1;

    if (FrLdrDbgPrint)
        FrLdrDbgPrint("kdnet: TSC calibrated: %lu ticks/us (%lu MHz)\n",
                      (ULONG)perUs, (ULONG)perUs);

    KdNetTicksPerUs = perUs;
    return KdNetTicksPerUs;
}
