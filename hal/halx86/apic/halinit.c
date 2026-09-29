/*
 * PROJECT:     ReactOS Hardware Abstraction Layer
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Initialize the APIC HAL
 * COPYRIGHT:   Copyright 2011 Timo Kreuzer <timo.kreuzer@reactos.org>
 */

/* INCLUDES *****************************************************************/

#include <hal.h>
#include "apicp.h"
#include <smp.h>
#define NDEBUG
#include <debug.h>

/* FUNCTIONS ****************************************************************/

VOID
NTAPI
HalpInitProcessor(
    IN ULONG ProcessorNumber,
    IN PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    BOOLEAN UseX2Apic;

    if (ProcessorNumber == 0)
    {
        /* Silence the legacy controller while LINT0 still accepts it. An input
           the firmware left asserted has no way back out once the entry is
           masked, and the request stays pending until interrupts come back on */
        HalpInitializeLegacyPICs();

        HalpParseApicTables(LoaderBlock);

        /* Every processor has to be in the same mode, so the first one decides */
        UseX2Apic = X2ApicCheckPolicy(LoaderBlock);
    }
    else
    {
        UseX2Apic = HalpX2ApicEnabled;
    }

    HalpSetupProcessorsTable(ProcessorNumber);

    /* Initialize the local APIC for this cpu */
    ApicInitializeLocalApic(ProcessorNumber, UseX2Apic);

    /* Initialize profiling data (but don't start it) */
    HalInitializeProfiling();

    /* Initialize the timer */
    //ApicInitializeTimer(ProcessorNumber);
}

VOID
HalpInitPhase0(IN PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    DPRINT1("Using HAL: APIC %s %s\n",
            (HalpBuildType & PRCB_BUILD_UNIPROCESSOR) ? "UP" : "SMP",
            (HalpBuildType & PRCB_BUILD_DEBUG) ? "DBG" : "REL");

    HalpPrintApicTables();

    HalpInterruptControllerType = HALP_INTERRUPT_CONTROLLER_APIC;

    /* Set up message-signaled interrupt support */
    HalpInitializeMessageInterrupts();

    /* Enable clock interrupt handler */
    HalpEnableInterruptHandler(IDT_INTERNAL,
                               0,
                               APIC_CLOCK_VECTOR,
                               CLOCK2_LEVEL,
                               HalpClockInterrupt,
                               Latched);

    /* Enable profile interrupt handler */
    HalpEnableInterruptHandler(IDT_DEVICE,
                               0,
                               APIC_PROFILE_VECTOR,
                               APIC_PROFILE_LEVEL,
                               HalpProfileInterrupt,
                               Latched);
}

VOID
HalpInitPhase1(VOID)
{
    /* Initialize DMA. NT does this in Phase 0 */
    HalpInitDma();
}

/* EOF */
