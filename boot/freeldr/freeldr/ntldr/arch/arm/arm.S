;
; PROJECT:         ReactOS Boot Loader
; FILE:            boot/freeldr/freeldr/arch/arm/arm_mmu_asm.asm
; PURPOSE:         ARM Low-Level MMU and Cache Control Functions
;

    AREA |.text|, CODE, READONLY, ALIGN=4

; Import symbols if needed (none for this basic case)
    IMPORT  PubLoaderBlockVA
    IMPORT  PubKiSystemStartup
    IMPORT BasicStack
    IMPORT JumpToKernel
; Export function names so the C linker can find them
    EXPORT  ArmDisableInterrupts
    EXPORT  ArmDisableMMUAndCaches
    EXPORT  ArmInvalidateICache
    EXPORT  ArmCleanAndInvalidateDCache
    EXPORT  ArmInvalidateTlb
    EXPORT  ArmInstructionSynchronizationBarrier
    EXPORT  ArmEnableMMU
    EXPORT  ArmDisableDCache
; This must match the #define in the C file.
KERNEL_STACK_VIRTUAL_TOP EQU 0x80200000
QEMU_UART_BASE EQU 0x09000000
;-------------------------------------------------------------------------------
; void ArmDisableInterrupts(void)
; Disables IRQ (I bit) and FIQ (F bit) in the CPSR.
;-------------------------------------------------------------------------------
ArmDisableInterrupts PROC
    mrs     r0, cpsr        ; Read Current Program Status Register
    orr     r0, r0, #0xC0   ; Set bits 7 (I) and 6 (F)
    msr     cpsr_c, r0      ; Write back to the control field
    bx      lr
    ENDP
    ;-------------------------------------------------------------------------------
    ; void ArmEnableInterrupts(void)
    ; Enables IRQ (I bit) and FIQ (F bit) in the CPSR.
    ;-------------------------------------------------------------------------------
ArmEnableInterrupts PROC
    mrs     r0, cpsr        ; Read Current Program Status Register
    bic     r0, r0, #0xC0   ; Clear bits 7 (I) and 6 (F)
    msr     cpsr_c, r0      ; Write back to the control field
    bx      lr
    ENDP
;-------------------------------------------------------------------------------
; void ArmDisableMMUAndCaches(void)
;-------------------------------------------------------------------------------
ArmDisableMMUAndCaches PROC
    mrc     p15, 0, r0, c1, c0, 0       ; Read SCTLR
    bic     r0, r0, #(1 << 12)          ; Disable I-cache
    bic     r0, r0, #(1 << 2)           ; Disable D-cache
    bic     r0, r0, #1                  ; Disable MMU
    mcr     p15, 0, r0, c1, c0, 0       ; Write SCTLR
    bx      lr
    ENDP

;-------------------------------------------------------------------------------
; void ArmInvalidateICache(void)
;-------------------------------------------------------------------------------
ArmInvalidateICache PROC
    mov     r0, #0
    mcr     p15, 0, r0, c7, c5, 0       ; Invalidate I-Cache
    bx      lr
    ENDP

;-------------------------------------------------------------------------------
; void ArmInvalidateTlb(void)
;-------------------------------------------------------------------------------
ArmInvalidateTlb PROC
    mov     r0, #0
    mcr     p15, 0, r0, c8, c7, 0       ; Invalidate entire unified TLB
    bx      lr
    ENDP

;-------------------------------------------------------------------------------
; void ArmInstructionSynchronizationBarrier(void)
;-------------------------------------------------------------------------------
ArmInstructionSynchronizationBarrier PROC
    mov     r0, #0
    mcr     p15, 0, r0, c7, c5, 4       ; Instruction Synchronization Barrier (ISB)
    bx      lr
    ENDP
;-------------------------------------------------------------------------------
; Helper to print a single Hex nibble (4 bits) from r1 to the UART
; Clobbers: r1, r2, r3
;-------------------------------------------------------------------------------
PrintNibble PROC
    and     r1, r1, #0xF             ; Mask to get the lower 4 bits
    cmp     r1, #9
    addgt   r1, r1, #'A' - 10        ; If > 9, convert to 'A'-'F'
    addls   r1, r1, #'0'             ; If <= 9, convert to '0'-'9'
    ldr     r2, =QEMU_UART_BASE      ; Load UART address
    str     r1, [r2]                 ; Print the character
    bx      lr
    ENDP

;-------------------------------------------------------------------------------
; Helper to print an entire 32-bit register (r0) as 8 hex characters
; Clobbers: r0, r1, r2, r3, r4, lr
;-------------------------------------------------------------------------------
PrintRegisterHex PROC
    stmfd   sp!, {r0, r4, lr}
    mov     r4, #8                   ; Loop 8 times for 8 nibbles
print_loop
    mov     r1, r0, lsr #28          ; Get the top nibble
    bl      PrintNibble              ; Print it
    mov     r0, r0, lsl #4           ; Shift left for the next nibble
    subs    r4, r4, #1
    bne     print_loop
    ldmfd   sp!, {r0, r4, pc}        ; Restore and return
    ENDP

ArmDisableDCache PROC
    mrc     p15, 0, r0, c1, c0, 0       ; Read SCTLR
    bic     r0, r0, #(1 << 2)           ; Clear C bit (D-Cache)
    mcr     p15, 0, r0, c1, c0, 0       ; Write SCTLR
    bx      lr
    ENDP




;-------------------------------------------------------------------------------
; void ArmMmuEnable(UINT TtbRegister, UINT DomainRegister, UINT ControlRegister, UINT StackPointerVA)
; r0 = TtbRegister
; r1 = DomainRegister
; r2 = ControlRegister
; r3 = StackPointerVA
;-------------------------------------------------------------------------------
ArmEnableMMU PROC
    ; r0 = TTBR0 (PGD base, 64-bit)
    ; r1 = unused (was DACR)
    ; r2 = SCTLR value
    ; r3 = StackPointerVA

    ; Set MAIR0: Attr0=device nGnRE, Attr1=normal WB/WA/RA
    mov     r4, #0x00           ; Attr0: device nGnRE
    mov     r5, #0xFF           ; Attr1: normal WB/WA/RA
    orr     r4, r4, r5, lsl #8
    mcr     p15, 0, r4, c10, c2, 0   ; MAIR0

    ; Set TTBCR: EAE=1 (LPAE/long format)
    mov     r4, #(1 << 31)
    mcr     p15, 0, r4, c2, c0, 2

    ; Set TTBR0 (64-bit): use mcrr
    mov     r4, r0
    mov     r5, #0
    mcrr    p15, 0, r4, r5, c2

    ; Invalidate TLBs
    mov     r4, #0
    mcr     p15, 0, r4, c8, c7, 0

    ; Set SCTLR (enable MMU, caches, etc.)
    mcr     p15, 0, r2, c1, c0, 0
    isb

    ; Print character 'A' to UART at 0x09000000
    mov     r1, #'A'
    ldr     r2, =QEMU_UART_BASE
    str     r1, [r2]

    ; Switch to the new kernel stack
    mov     sp, r3

    b       JumpToKernel
    ENDP

is_secure PROC
    ldr r0, =0x09000000
    mov r1, #'N'
    str r1, [r0]
    bx lr
    ENDP

is_non_secure PROC
    ldr r0, =0x09000000
    mov r1, #'Y'
    str r1, [r0]
    bx lr
    ENDP

;-------------------------------------------------------------------------------
; void ArmCleanAndInvalidateDCache(void)
; A robust routine to clean and invalidate the D-cache by set/way.
;-------------------------------------------------------------------------------
ArmCleanAndInvalidateDCache PROC
    stmfd   sp!, {r4-r12}               ; Save registers

    mov     r0, #0
    mcr     p15, 2, r0, c0, c0, 0       ; Select Cache Size Selection Register, level 0

    mrc     p15, 1, r0, c0, c0, 0       ; Read CCSIDR
    
    ; Extract cache geometry
    and     r1, r0, #7                  ; r1 = log2(line size in words) - 2
    add     r1, r1, #4                  ; r1 = log2(line size in bytes)

    mov     r2, #0x3FF
    and     r2, r2, r0, lsr #3          ; r2 = (ways - 1)
    
    mov     r3, #0x7FFF
    and     r3, r3, r0, lsr #13         ; r3 = (sets - 1)

    ; r1 = log2(line size), r2 = ways-1, r3 = sets-1
    clz     r4, r2                      ; r4 = ways_clz
    
    ; Loop counters
    mov     r9, r3                      ; r9 = set counter

set_loop
    mov     r10, r2                     ; r10 = way counter
way_loop
    ; Form the value for the MCR register: (way << ways_clz) | (set << line_size_log2)
    ; --- START FIX ---
    lsl     r11, r10, r4                ; r11 = way << ways_clz
    lsl     r12, r9, r1                 ; r12 = set << line_size_log2
    orr     r11, r11, r12               ; r11 = (way << ways_clz) | (set << line_size_log2)
    ; --- END FIX ---
    
    mcr     p15, 0, r11, c7, c14, 2     ; Clean and Invalidate by set/way

    subs    r10, r10, #1                ; Decrement way
    bge     way_loop

    subs    r9, r9, #1                  ; Decrement set
    bge     set_loop

    ldmfd   sp!, {r4-r12}               ; Restore registers
    bx      lr
    ENDP

    END