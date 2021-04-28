/*
 * PROJECT:     ReactOS Hardware Abstraction Layer
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Initialize the APIC HAL
 * PROGRAMMERS:    Copyright 2011 Timo Kreuzer <timo.kreuzer@reactos.org>
 *                 Copyright 2021 Justin Miller <the_darkfire@ring0productions.com>
 */

/* INCLUDES *****************************************************************/

#include <hal.h>
#include <apic.h>
#define NDEBUG
#include <debug.h>

VOID
NTAPI
ApicInitializeLocalApic(ULONG Cpu);

/* GLOBALS ******************************************************************/

const USHORT HalpBuildType = HAL_BUILD_TYPE;

/* FUNCTIONS ****************************************************************/

VOID
NTAPI
HalpInitProcessor(
    IN ULONG ProcessorNumber,
    IN PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    /* We should only Init MADT once; the BSP core number isn't always equal to the NT processor 0 */
    if(ProcessorNumber == 0)
    {
        HalpInitMADT(LoaderBlock);
    }

    /* Initialize the local APIC for this cpu */
    ApicInitializeLocalApic(ProcessorNumber);
    
    /* Initialize profiling data (but don't start it) */
    HalInitializeProfiling();
}

VOID
HalpInitPhase0(IN PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    /* Enable clock interrupt handler */
    HalpEnableInterruptHandler(IDT_INTERNAL,
                               0,
                               APIC_CLOCK_VECTOR,
                               CLOCK2_LEVEL,
                               HalpClockInterrupt,
                               Latched);
}

VOID
HalpInitPhase1(VOID)
{
    /* Initialize DMA. NT does this in Phase 0 */
    HalpInitDma();
}

/* EOF */
