/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Architecture specific source file to hold multiprocessor functions
 * COPYRIGHT:   Copyright 2022 Justin Miller <justinmiller100@gmail.com>
 */

/* INCLUDES *****************************************************************/

#include <ntoskrnl.h>

#define NDEBUG
#include <debug.h>

/* FUNCTIONS *****************************************************************/

CODE_SEG("INIT")
VOID
NTAPI
KeStartAllProcessors(VOID)
{
    UNIMPLEMENTED;
}
