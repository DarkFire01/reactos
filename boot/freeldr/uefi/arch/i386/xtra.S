/*
 *  FreeLoader
 *  Copyright (C) 1998-2002  Brian Palmer  <brianp@sginet.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <asm.inc>
#include <arch/pc/x86common.h>
#include <arch/pc/pcbios.h>
/* Entrypoint for realmode function calls
 * ContinueAddress must be set to the return point from realmode
 * bx must be set to the ID of the realmode function to call. */

    /* 32-bit stack pointer */
stack32:
    .long STACKADDR

PUBLIC SwitchToReal
SwitchToReal:
    /* Set sane segments */
    mov ax, PMODE_DS
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    /* Save 32-bit stack pointer */
    mov dword ptr ds:[stack32], esp

    /* jmp to 16-bit segment to set the limit correctly */
    .byte HEX(0ea) // jmp far RMODE_CS:switch_to_real16
SwitchToReal16Address:
    .long 0 // receives address of switch_to_real16
    .word RMODE_CS
    nop

PUBLIC _Relocator16Boot
_Relocator16Boot:

    /* Copy input registers */
    mov esi, dword ptr [esp + 4]
    mov edi, BSS_RegisterSet
    mov ecx, REGS_SIZE / 4
    rep movsd

    /* Set the stack segment/offset */
    // Since BSS_CallbackReturn contains a ULONG, store in its high word
    // the stack segment and in its low word the stack offset.
    mov ax, word ptr [esp + 8]
    shl eax, 16
    mov ax, word ptr [esp + 12]
    mov dword ptr ds:[BSS_CallbackReturn], eax

    /*
     * Set the code segment/offset (Copy entry point)
     * NOTE: We permanently *ERASE* the contents of ds:[BSS_RealModeEntry]
     * but it is not a problem since we are going to place the machine in
     * a permanent new CPU state.
     */
    // Since BSS_RealModeEntry contains a ULONG, store in its high word
    // the code segment and in its low word the code offset.
    mov ax, word ptr [esp + 16]
    shl eax, 16
    mov ax, word ptr [esp + 20]
    mov dword ptr ds:[BSS_RealModeEntry], eax

    /* Set the function ID and switch to real mode (we don't return) */
    mov bx, FNID_Relocator16Boot
    jmp SwitchToReal

