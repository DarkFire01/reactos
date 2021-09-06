
/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * FILE:        hal/halx86/mp/mps.c
 * PROGRAMMERS:  Copyright 2021 Justin Miller <justinmiller100@gmail.com>
 */

/* INCLUDES *******************************************************************/

#include <hal.h>
#include <smp.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

#define TEMP_CPUMAX 32
UINT32 PhysicalProcessorCount = 0;
PROCESSOR_IDENTITY HalpStaticProcessorIdentity[TEMP_CPUMAX] = {{0}};

/* FUNCTIONS ******************************************************************/

VOID
HalpParseApicTables(IN PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    /* On non-apic legacy systems this function won't do much */
    //@unimplemented
}

VOID
HalpPrintApicTables()
{ 
    DPRINT1("HAL has detected a physical processor count of: %d\n", PhysicalProcessorCount);

    for(UINT32 i = 0; i < PhysicalProcessorCount; i++)
    {
        DPRINT1("Information about the following processor is for processors number: %d\n", i);
        DPRINT1("   The BSPCheck is set to: %X\n", HalpStaticProcessorIdentity[i].BSPCheck);
        DPRINT1("   The LapicID is set to: %X\n", HalpStaticProcessorIdentity[i].LapicId);
        DPRINT1("   The ProcessorID is set to: %X\n", HalpStaticProcessorIdentity[i].ProcessorId);
        DPRINT1("   The ProcessorStated check is set to: %X\n", HalpStaticProcessorIdentity[i].ProcessorStarted);
        DPRINT1("   The ROSProcessorNumber is set to: %X\n", HalpStaticProcessorIdentity[i].ROSProcessorNumber);
    }
}