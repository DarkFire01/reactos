/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     BSD - See COPYING.ARM in the top level directory
 * FILE:        ntoskrnl/ke/arm64/mproc.c
 * PURPOSE:     Multiprocessor bring-up stubs (ARM64)
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
