/*
 * PROJECT:     ReactOS Hardware Abstraction Layer
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Header file for APIC hal
 * COPYRIGHT:   2011 Timo Kreuzer <timo.kreuzer@reactos.org>
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
    HalpInitMADT(LoaderBlock);
   
    /* Initialize the local APIC for this cpu */
    ApicInitializeLocalApic(ProcessorNumber);

    /* Initialize profiling data (but don't start it) */
    HalInitializeProfiling();

    /* Initialize the timer */
    //ApicInitializeTimer(ProcessorNumber);

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
    __debugbreak();
    /* Initialize DMA. NT does this in Phase 0 */

}

/* EOF */
