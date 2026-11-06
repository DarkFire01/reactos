
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

#elif defined(_M_ARM64) || defined(_M_ARM64EC)

#include <kxarm64.h>

/* The system call number travels in the svc immediate */
#ifdef _MSC_VER

    GBLA SyscallId
SyscallId SETA 0

#ifdef _ARM64EC_
    GBLL SyscallThunkDone

    /* x64 entry shared by all stubs: x4 is the x64 stack past the return address, arguments 5 to 8
       go to x4-x7 and arguments 9 to 16, the most any system call takes, to the native stack. */
    MACRO
    SYSCALL_ENTRY_THUNK
    IMPORT __os_arm64x_dispatch_ret
    ALIGN 4
|SyscallEntryThunk| PROC
    stp     q6, q7, [sp, #-0xA0]!
    stp     q8, q9, [sp, #0x20]
    stp     q10, q11, [sp, #0x40]
    stp     q12, q13, [sp, #0x60]
    stp     q14, q15, [sp, #0x80]
    stp     fp, lr, [sp, #-0x10]!
    mov     fp, sp
    sub     sp, sp, #0x40
    ldp     x10, x11, [x4, #0x40]
    stp     x10, x11, [sp, #0x00]
    ldp     x10, x11, [x4, #0x50]
    stp     x10, x11, [sp, #0x10]
    ldp     x10, x11, [x4, #0x60]
    stp     x10, x11, [sp, #0x20]
    ldp     x10, x11, [x4, #0x70]
    stp     x10, x11, [sp, #0x30]
    ldp     x6, x7, [x4, #0x30]
    ldp     x4, x5, [x4, #0x20]
    blr     x9
    mov     x8, x0
    add     sp, sp, #0x40
    ldp     fp, lr, [sp], #0x10
    ldp     q14, q15, [sp, #0x80]
    ldp     q12, q13, [sp, #0x60]
    ldp     q10, q11, [sp, #0x40]
    ldp     q8, q9, [sp, #0x20]
    ldp     q6, q7, [sp], #0xA0
    adrp    xip0, __os_arm64x_dispatch_ret
    ldr     xip0, [xip0, __os_arm64x_dispatch_ret]
    br      xip0
    ENDP
    MEND
#endif

    MACRO
    STUB_U $Name
    LCLS NtFuncName
    LCLS ZwFuncName
#ifdef _ARM64EC_
NtFuncName SETS "|#Nt$Name|"
ZwFuncName SETS "|#Zw$Name|"
    IF :LNOT: SyscallThunkDone
    SYSCALL_ENTRY_THUNK
SyscallThunkDone SETL {TRUE}
    ENDIF
    ALIGN 4
    /* The word before an ARM64EC function locates its x64 entry thunk */
    DCD     (|SyscallEntryThunk| - ({PC} + 4)) + 1
#else
NtFuncName SETS "Nt$Name"
ZwFuncName SETS "Zw$Name"
#endif
    ALIGN 4
    EXPORT $NtFuncName
    EXPORT $ZwFuncName
$NtFuncName
$ZwFuncName
    svc #SyscallId
    ret
SyscallId SETA SyscallId + 1
    MEND

    MACRO
    SKIP_STUB $Name
SyscallId SETA SyscallId + 1
    MEND

    /* Kernel mode callers enter the dispatcher with the number in x16 */
    MACRO
    STUB_K $Name
    LCLS ZwFuncName
ZwFuncName SETS "Zw$Name"
    ALIGN 4
    EXPORT $ZwFuncName
$ZwFuncName
    mov x16, #SyscallId
    b KiZwSystemService
SyscallId SETA SyscallId + 1
    MEND

    MACRO
    SYSCALL_BASE $Base
SyscallId SETA $Base
    MEND

#else

.set SyscallId, 0

.macro STUB_U Name
    .global Nt\Name
    .global Zw\Name
    .p2align 2
Nt\Name:
Zw\Name:
    svc #SyscallId
    ret
    .set SyscallId, SyscallId + 1
.endm

.macro SKIP_STUB Name
    .set SyscallId, SyscallId + 1
.endm

.macro STUB_K Name
    .global Zw\Name
    .p2align 2
Zw\Name:
    mov x16, #SyscallId
    b KiZwSystemService
    .set SyscallId, SyscallId + 1
.endm

.macro SYSCALL_BASE Base
    .set SyscallId, \Base
.endm

#endif

#else

#include <asm.inc>

SyscallId = 0

#ifdef _M_IX86
#ifdef BUILD_WOW6432
#define KUSER_SHARED_SYSCALL HEX(7ffe0300)
MACRO(STUBCODE_U, Name, SyscallId, ArgCount)
LOCAL x, x2, x3
    StackBytes = 4 * ArgCount
    FPO 0, 0, 0, 0, 0, FRAME_FPO
    push ecx
    push edx
    
    mov ecx, SyscallId
    mov edx, ArgCount
    
    push ebp
    mov ebp, esp
    
    /* Align the stack, and allocate shadow space for the x64 calling convention */
    sub esp, HEX(48)
    and esp, HEX(FFFFFFF0)
    
    push 0
    push ebp
    
    ljmp HEX(33), x
x:
    /* pop rdx */
    .byte HEX(5a)

    /* add rdx, 16 */
    .byte HEX(48)
    .byte HEX(83)
    .byte HEX(c2)
    .byte HEX(10)
    
    call x3
    
    push HEX(23)
    push x2
    .byte HEX(48)
    retf
x2:
    mov esp, ebp
    pop ebp
    
    pop edx
    pop ecx
    
    ret StackBytes
x3:
    .long HEX(25048b64)
    .long HEX(000000c0)
    .byte HEX(50)
    .byte HEX(c3)
ENDM
MACRO(STUBCODE_K, Name, SyscallId, ArgCount)
    int 3
ENDM
#else
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
#endif
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
