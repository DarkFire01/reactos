 #include <kxarm64.h>

#define CURRENT_EL_EL_MASK #2_1100
#define CURRENT_EL_1 #2_0100

    NESTED_ENTRY current_el
    mrs x0, CurrentEL
    and x0, x0, CURRENT_EL_EL_MASK
    lsr x0, x0, 2
    ret
    NESTED_END current_el

    NESTED_ENTRY read_id_aa64mmfr0_el1
    mrs x0, id_aa64mmfr0_el1
    ret
    NESTED_END read_id_aa64mmfr0_el1

    NESTED_ENTRY read_id_aa64mmfr1_el1
    mrs x0, id_aa64mmfr1_el1
    ret
    NESTED_END read_id_aa64mmfr1_el1

    NESTED_ENTRY read_hcr_el2
    mrs x0, hcr_el2
    ret
    NESTED_END read_hcr_el2

    NESTED_ENTRY write_hcr_el2
    msr hcr_el2, x0
    ret
    NESTED_END write_hcr_el2



    NESTED_ENTRY holdme
    b holdme
    NESTED_END holdme

#define SCTLR_M 2_1
#define SPSEL_ELX 2_1
#define handover_info_aarch64_arg0 0
#define handover_info_aarch64_arg1 8
#define handover_info_aarch64_entrypoint 16
#define handover_info_aarch64_stack 24
#define handover_info_aarch64_direct_map_base 32

#define handover_info_aarch64_ttbr0 40
#define handover_info_aarch64_ttbr1 48
#define handover_info_aarch64_mair 56
#define handover_info_aarch64_tcr 64
#define handover_info_aarch64_sctlr 72

#define handover_info_aarch64_unmap_lower_half 80

    NESTED_ENTRY kernel_handover_aarch64
    msr daifset, #2_1111

    ldr x1, [x0, handover_info_aarch64_ttbr0]
    ldr x2, [x0, handover_info_aarch64_ttbr1]
    ldr x3, [x0, handover_info_aarch64_mair]
    ldr x4, [x0, handover_info_aarch64_tcr]
    ldr x5, [x0, handover_info_aarch64_sctlr]

    isb
    dsb nsh

    // Disable MMU while we mess with translation registers
    mrs x6, sctlr_el1
    and x6, x6, ~SCTLR_M
    msr sctlr_el1, x6

    isb
    dsb nsh

    msr mair_el1, x3
    msr TCR_EL1, x4
    msr ttbr0_el1, x1
    msr ttbr1_el1, x2

    isb
    tlbi vmalle1
    dsb nsh

    // Finally enable the MMU again with the provided configuration
    msr sctlr_el1, x5

// HOLD
    mov x20, #0x0001
    mov x21, #0xDEAD
    mov x22, #0xBEEF
    mov x23, #0xAAAA
    mov x24, #0xBBBB
    mov x25, #0xCCCC
    mov x26, #0xDDDD
    mov x27, #0xEEEE
    mov x28, #0xFFFF
    mov x29, #0xBEEF
    mov x30, #0xDEAD
    wfi
    b holdme 
// HOLD
    isb
    dsb nsh

    ldr x3, [x0, handover_info_aarch64_stack]
    msr spsel, #SPSEL_ELX
    mov sp, x3

    ldr x30, [x0, handover_info_aarch64_entrypoint]

    ldr x3, [x0, handover_info_aarch64_direct_map_base]
    add x0, x0, x3

    NESTED_END kernel_handover_aarch64

    END