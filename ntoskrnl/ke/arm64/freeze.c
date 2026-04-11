/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     BSD - See COPYING.ARM in the top level directory
 * FILE:        ntoskrnl/ke/arm64/freeze.c
 * PURPOSE:     Debugger freeze IPI stubs for ARM64 SMP bring-up
 */

#include <ntoskrnl.h>

#define NDEBUG
#include <debug.h>

#ifdef CONFIG_SMP

VOID
NTAPI
KxFreezeExecution(VOID)
{
    /* TODO: ARM64 freeze IPI (see ke/amd64/freeze.c) */
}

VOID
NTAPI
KxThawExecution(VOID)
{
    /* TODO: ARM64 thaw IPI */
}

#else /* !CONFIG_SMP */

/* Keep TU non-empty for uni-processor kernel builds */
static const UCHAR Arm64FreezeNoSmp;

#endif /* CONFIG_SMP */
