
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

/* FUNCTIONS ******************************************************************/

VOID
HalpParseApicTables(IN PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    /* On non-apic legacy systems this function won't do much */
    //@unimplemented
}
