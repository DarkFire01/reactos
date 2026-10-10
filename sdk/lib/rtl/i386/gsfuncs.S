/*
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         ReactOS Runtime Library (RTL)
 * FILE:            sdk/lib/rtl/i386/gsfuncs.S
 * PURPOSE:         WOW64 helper functions for accesing the 64-bit TEB
 * PROGRAMMERS:     Marcin Jabłoński
 */

/* INCLUDES ******************************************************************/

#include <asm.inc>

/* FUNCTIONS ****************************************************************/
.code

PUBLIC ___readgsdword
___readgsdword:
    mov eax, [esp + 4]
    mov eax, gs:[eax]
    ret

PUBLIC ___writegsdword
___writegsdword:
    mov eax, [esp + 4]
    mov ecx, [esp + 8]
    mov gs:[eax], ecx
    ret
    
END