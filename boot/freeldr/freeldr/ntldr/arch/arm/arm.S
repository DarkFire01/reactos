;
; PROJECT:         ReactOS Boot Loader
; FILE:            boot/freeldr/freeldr/arch/arm/arm_mmu_asm.asm
; PURPOSE:         ARM Low-Level MMU and Cache Control Functions
;

    AREA |.text|, CODE, READONLY, ALIGN=4

; Import symbols if needed (none for this basic case)
    IMPORT  PubLoaderBlockVA
    IMPORT  PubKiSystemStartup
; Export function names so the C linker can find them
    EXPORT  ArmDisableInterrupts
    EXPORT  ArmDisableMMUAndCaches
    EXPORT  ArmInvalidateICache
    EXPORT  ArmCleanAndInvalidateDCache
    EXPORT  ArmInvalidateTlb
    EXPORT  ArmInstructionSynchronizationBarrier
    EXPORT  ArmEnableMMU
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

;-------------------------------------------------------------------------------
; void ArmMmuEnable(UINT TtbRegister, UINT DomainRegister, UINT ControlRegister)
; Enables MMU, switches stack, prints args, and then jumps to the kernel.
; r0 = TtbRegister
; r1 = DomainRegister
; r2 = ControlRegister
;-------------------------------------------------------------------------------
ArmEnableMMU PROC
    mcr     p15, 0, r0, c2, c0, 0       ; Write TTBR0
    mcr     p15, 0, r1, c3, c0, 0       ; Write DACR
    mcr     p15, 0, r2, c1, c0, 0       ; Write SCTLR
    isb                                 ; Flush pipeline
    ldr     sp, =KERNEL_STACK_VIRTUAL_TOP ; Switch stack

    ; --- DEBUG: Print the arguments before jumping ---
    ldr     r2, =QEMU_UART_BASE
    
    ; Print "R0="
    mov     r1, #'R'
    str     r1, [r2]
    mov     r1, #'0'
    str     r1, [r2]
    mov     r1, #'='
    str     r1, [r2]
    
    ; Load and print the Loader Block VA
    ldr     r0, =PubLoaderBlockVA
    ldr     r0, [r0]
    bl      PrintRegisterHex

    ; Print " EP="
    mov     r1, #' '
    str     r1, [r2]
    mov     r1, #'E'
    str     r1, [r2]
    mov     r1, #'P'
    str     r1, [r2]
    mov     r1, #'='
    str     r1, [r2]

    ; Load and print the Kernel Entry Point VA
    ldr     r0, =PubKiSystemStartup
    ldr     r0, [r0]
    bl      PrintRegisterHex

    ; --- JUMP TO KERNEL ---
    ldr     r0, =PubLoaderBlockVA       ; Reload r0 with the first argument
    ldr     r0, [r0]
    ldr     r3, =PubKiSystemStartup     ; Reload r3 with the entry point
    ldr     r3, [r3]
    bx      r3
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