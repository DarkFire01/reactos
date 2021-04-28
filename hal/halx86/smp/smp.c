/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * FILE:        hal/halx86/smp/smp.c
 * PURPOSE:     Source File for SMP
 * PROGRAMMERS:  Copyright 2021 Justin Miller <justinmiller100@gmail.com>
 */

#include <hal.h>
#include <smp.h>
#define NDEBUG
#include <debug.h>

VOID
HalpSendIPI(ULONG AP, ULONG Mode)
{
    //@Unimplemented
}

/* EOF */
