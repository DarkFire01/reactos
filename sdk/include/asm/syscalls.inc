
#ifdef _M_ARM

#include <ksarm.h>

    GBLA SyscallId

SyscallId SETA 0

	MACRO
	STUB_K $Name
	LCLS ZwFuncName
	LCLS ZwFuncEndName
ZwFuncName    SETS "Zw$Name"
ZwFuncEndName SETS "$ZwFuncName":CC:"_end"
	ALIGN 2
	EXPORT $ZwFuncName [FUNC]
$ZwFuncName
	ROUT
    mov r12, #SyscallId
    svc #1
    bx lr
$ZwFuncEndName
    MEND

	MACRO
	STUB_U $Name
	LCLS NtFuncName
	LCLS NtFuncEndName
	LCLS ZwFuncName
	LCLS ZwFuncEndName
NtFuncName    SETS "Nt$Name"
NtFuncEndName SETS "$NtFuncName":CC:"_end"
ZwFuncName    SETS "Zw$Name"
ZwFuncEndName SETS "$ZwFuncName":CC:"_end"
	ALIGN 2
	EXPORT $NtFuncName [FUNC]
$NtFuncName
	EXPORT $ZwFuncName [FUNC]
$ZwFuncName
	ROUT
    mov r12, #SyscallId
    svc #1
    bx lr
$NtFuncEndName
$ZwFuncEndName
    MEND

#elif defined(_M_ARM64) || defined(_ARM64_) || defined(__arm64__) || defined(__aarch64__)

#include <kxarm64.h>

    GBLA SyscallId
SyscallId SETA 0

#ifdef SVC_KERNEL_STUBS
    IMPORT KiZwSystemService
#endif

    /*
     * STUB_K - kernel-mode Zw stub (ntoskrnl).
     *
     * Loads the NtXxx address from a PC-relative literal pool entry into
     * x16 (IP0), then tail-calls KiZwSystemService, which:
     *   1. Saves callee-saved registers in a KEXCEPTION_FRAME.
     *   2. Reads KTHREAD* from TPIDR_EL1 -> KIPCR.Prcb.CurrentThread.
     *   3. Saves and replaces KTHREAD.TrapFrame / PreviousMode (KernelMode).
     *   4. Calls NtXxx (x0-x7 already contain the arguments; no copying).
     *   5. Restores TrapFrame/PreviousMode and callee-saved registers.
     *   6. Returns to the ZwXxx caller (x0 = NtXxx return value).
     */
#ifdef SVC_KERNEL_STUBS
    MACRO
    STUB_K $Name
    LCLS ZwFuncName
    LCLS ZwFuncEndName
    LCLS NtPtrLabel
ZwFuncName    SETS "Zw$Name"
ZwFuncEndName SETS "Zw$Name._end"
NtPtrLabel    SETS "ZwNtPtr$Name"
    IMPORT Nt$Name
    ALIGN 2
    EXPORT $ZwFuncName
$ZwFuncName PROC
    ROUT
    LDR  x16, $NtPtrLabel           ; load NtXxx address from literal pool
    B    KiZwSystemService           ; tail-call dispatcher; returns to caller
    ALIGN 8                          ; 8-byte alignment for 64-bit pointer
$NtPtrLabel DCQ Nt$Name             ; 64-bit address of NtXxx (ADDR64 reloc)
$ZwFuncEndName
    ENDP
SyscallId SETA SyscallId + 1
    MEND
#else
    MACRO
    STUB_K $Name
    MEND
#endif

    /*
     * STUB_U - user-mode Zw/Nt stub (ntdll)
     *
     * ARM64 Windows syscall convention: syscall number in x8, SVC #0.
     * Both Nt and Zw names are aliased to the same entry point.
     */
    MACRO
    STUB_U $Name
    LCLS NtFuncName
    LCLS ZwFuncName
    LCLS EndLabel
NtFuncName    SETS "Nt$Name"
ZwFuncName    SETS "Zw$Name"
EndLabel      SETS "Nt$Name._end"
    ALIGN 4
    EXPORT $ZwFuncName
    EXPORT $NtFuncName
$ZwFuncName PROC
$NtFuncName
    ROUT
    MOV x8, #SyscallId
    SVC #0
    RET
$EndLabel
    ENDP
SyscallId SETA SyscallId + 1
    MEND

#else

#include <asm.inc>

SyscallId = 0

#ifdef _M_IX86
#define KUSER_SHARED_SYSCALL HEX(7ffe0300)
#define KGDT_R0_CODE 8
MACRO(STUBCODE_U, Name, SyscallId, ArgCount)
    StackBytes = 4 * ArgCount
    FPO 0, 0, 0, 0, 0, FRAME_FPO
    mov eax, SyscallId
    mov edx, KUSER_SHARED_SYSCALL
    call dword ptr [edx]
    ret StackBytes
ENDM
MACRO(STUBCODE_K, Name, SyscallId, ArgCount)
    StackBytes = 4 * &ArgCount
    FPO 0, 0, 0, 0, 0, FRAME_FPO
    mov eax, SyscallId
    lea edx, [esp + 4]
    pushfd
    push KGDT_R0_CODE
    call _KiSystemService
    ret StackBytes
ENDM
#elif defined(_M_AMD64)
MACRO(STUBCODE_U, Name, SyscallId, ArgCount)
    .ENDPROLOG
    mov eax, SyscallId
    mov r10, rcx
    syscall
    ret
ENDM
MACRO(STUBCODE_K, Name, SyscallId, ArgCount)
    .ENDPROLOG
    EXTERN Nt&Name:PROC
    lea rax, Nt&Name[rip]
    mov r10, ArgCount * 8
    jmp KiZwSystemService
ENDM
#elif defined(_M_PPC)
MACRO(STUBCODE_U, Name, SyscallId, ArgCount)
    stwu 1,-16(1)
    mflr 0
    stw  0,0(1)
    li   0, SyscallId
    sc
    lwz 0,0(1)
    mtlr 0
    addi 1,1,16
    blr
ENDM
#define STUBCODE_K STUBCODE_U
#elif defined(_M_MIPS)
MACRO(STUBCODE_U, Name, SyscallId, ArgCount)
    li $8, KUSER_SHARED_SYSCALL
    lw $8,0($8)
    j $8
    nop
ENDM
MACRO(STUBCODE_K, Name, SyscallId, ArgCount)
    j KiSystemService
    nop
ENDM
#else
#error unsupported architecture
#endif

#ifdef _M_IX86
MACRO(MAKE_LABEL, Name, StackBytes)
    PUBLIC _&Name&@&StackBytes
    _&Name&@&StackBytes:
ENDM
MACRO(START_PROC, Name, StackBytes)
    PUBLIC _&Name&@&StackBytes
    .PROC _&Name&@&StackBytes
ENDM
#else
MACRO(MAKE_LABEL, Name, StackBytes)
    PUBLIC &Name
    &Name:
ENDM
MACRO(START_PROC, Name, StackBytes)
    PUBLIC &Name
    .PROC &Name
ENDM
#endif

MACRO(STUB_U, Name, ArgCount)
    MAKE_LABEL Zw&Name, %ArgCount * 4
    START_PROC Nt&Name, %ArgCount * 4
    STUBCODE_U Name, SyscallId, %ArgCount
    .ENDP
    SyscallId = SyscallId + 1
ENDM

MACRO(STUB_K, Name, ArgCount)
    START_PROC Zw&Name, %ArgCount * 4
    STUBCODE_K Name, SyscallId, %ArgCount
    .ENDP
    SyscallId = SyscallId + 1
ENDM

#endif
