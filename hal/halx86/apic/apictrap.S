/*
 * FILE:            hal/halx86/apic/apictrap.S
 * COPYRIGHT:       See COPYING in the top level directory
 * PURPOSE:         System Traps, Entrypoints and Exitpoints
 * PROGRAMMER:      Timo Kreuzer (timo.kreuzer@reactos.org)
 * NOTE:            See asmmacro.S for the shared entry/exit code.
 */

/* INCLUDES ******************************************************************/

#include <asm.inc>

#ifdef _M_AMD64
#include <ksamd64.inc>
#include <trapamd64.inc>
.code

/* The handlers end their own interrupts, in whichever mode the APIC is in */
TRAP_ENTRY HalpClockInterrupt, 0
TRAP_ENTRY HalpClockIpi, 0
TRAP_ENTRY HalpProfileInterrupt, 0

PUBLIC ApicSpuriousService
ApicSpuriousService:
    iretq

APIC_ESR = HEX(0FFFFFFFFFFFE0280)
X2APIC_ESR_MSR = HEX(828)

EXTERN HalpX2ApicEnabled:BYTE

/* Rearm the error status register, then end the interrupt */
PUBLIC ApicErrorService
ApicErrorService:
    cmp byte ptr HalpX2ApicEnabled[rip], 0
    jnz ApicErrorServiceX2
    mov dword ptr [APIC_ESR], 0
    mov dword ptr [APIC_EOI], 0
    iretq

ApicErrorServiceX2:
    push rax
    push rcx
    push rdx
    xor eax, eax
    xor edx, edx
    mov ecx, X2APIC_ESR_MSR
    wrmsr
    mov ecx, X2APIC_EOI_MSR
    wrmsr
    pop rdx
    pop rcx
    pop rax
    iretq

#else
#include <ks386.inc>
#include <internal/i386/asmmacro.S>
.code

TRAP_ENTRY HalpClockInterrupt, KI_PUSH_FAKE_ERROR_CODE
TRAP_ENTRY HalpClockIpi, KI_PUSH_FAKE_ERROR_CODE
TRAP_ENTRY HalpProfileInterrupt, KI_PUSH_FAKE_ERROR_CODE
TRAP_ENTRY HalpTrap0D, 0
TRAP_ENTRY HalpApcInterrupt, KI_PUSH_FAKE_ERROR_CODE
TRAP_ENTRY HalpDispatchInterrupt, KI_PUSH_FAKE_ERROR_CODE
TRAP_ENTRY HalpIpiInterrupt, KI_PUSH_FAKE_ERROR_CODE

PUBLIC _ApicSpuriousService
_ApicSpuriousService:
    iret

APIC_ESR = HEX(0FFFE0280)
APIC_EOI = HEX(0FFFE00B0)
X2APIC_ESR_MSR = HEX(828)
X2APIC_EOI_MSR = HEX(80B)

EXTERN _HalpX2ApicEnabled:BYTE

/* Rearm the error status register, then end the interrupt. SS is used
   because DS is not valid when the interrupt arrives in V86 mode */
PUBLIC _ApicErrorService
_ApicErrorService:
    cmp byte ptr ss:[_HalpX2ApicEnabled], 0
    jnz ApicErrorServiceX2
    mov dword ptr ss:[APIC_ESR], 0
    mov dword ptr ss:[APIC_EOI], 0
    iret

ApicErrorServiceX2:
    push eax
    push ecx
    push edx
    xor eax, eax
    xor edx, edx
    mov ecx, X2APIC_ESR_MSR
    wrmsr
    mov ecx, X2APIC_EOI_MSR
    wrmsr
    pop edx
    pop ecx
    pop eax
    iret

#endif





END
