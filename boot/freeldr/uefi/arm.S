/*
 * PROJECT:         ReactOS Boot Loader
 * LICENSE:         BSD - See COPYING.ARM in the top level directory
 * FILE:            boot/freeldr/freeldr/arch/arm/boot.S
 * PURPOSE:         Implements the entry point for ARM machines
 * PROGRAMMERS:     ReactOS Portable Systems Group
 */

#include <ksarm.h>
    EXTERN stackSpace
    NESTED_ENTRY _changestack
    PROLOG_END _changestack

    LDR     R0, =stackSpace
    MOV     sp, R0 ;

    NESTED_END _changestack

//L_ArmInit:
//    .long ArmInit

    END
/* EOF */
