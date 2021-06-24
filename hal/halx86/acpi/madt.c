
/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * FILE:        hal/halx86/acpi/madt.c
 * PROGRAMMERS:  Copyright 2021 Justin Miller <justinmiller100@gmail.com>
 */

/* INCLUDES *******************************************************************/

#include <hal.h>
#include <acpi.h>
/* ACPI_BIOS_ERROR defined in acoutput.h and bugcodes.h */
#undef ACPI_BIOS_ERROR
#include <smp.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

/* FUNCTIONS ******************************************************************/

VOID
HalpParseApicTables(IN PLOADER_PARAMETER_BLOCK LoaderBlock)
{
 //@Unimplemented
}
