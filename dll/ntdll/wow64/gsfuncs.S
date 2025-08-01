/*
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         ReactOS NT Library
 * FILE:            dll/ntdll/wow64/gsfuncs.S
 * PURPOSE:         WOW64 functions for dealing with 64-bit TEB
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
    mov eax, [esp + 8]
    mov ecx, [esp + 4]
    mov gs:[eax], ecx
    ret
    
END