/*
 * PROJECT:         ReactOS Kernel
 * LICENSE:         BSD - See COPYING.ARM in the top level directory
 * FILE:            ntoskrnl/ke/arm/boot.s
 * PURPOSE:         Implements the kernel entry point for ARM machines
 * PROGRAMMERS:     ReactOS Portable Systems Group
 */

#include <ksarm.h>

    TEXTAREA

    NESTED_ENTRY pArmControlRegisterGet
    PROLOG_END pArmControlRegisterGet
    mrc p15, 0, r0, c1, c0, 0
    bx lr
    NESTED_END pArmControlRegisterGet

    NESTED_ENTRY  pArmControlRegisterSet
    PROLOG_END pArmControlRegisterSet
    mcr p15, 0, r0, c1, c0, 0
    bx lr
    NESTED_END pArmControlRegisterSet

    END
/* EOF */
