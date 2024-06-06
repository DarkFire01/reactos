 #include <kxarm64.h>
    EXTERN ContinueExecution
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

    NESTED_ENTRY set_mair_el1
    msr mair_el1, x0
    ret
    NESTED_END set_mair_el1


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

    NESTED_ENTRY write_ttbr0_el1
    msr ttbr0_el1, x0
    ret
    NESTED_END write_ttbr0_el1

    NESTED_ENTRY write_ttbr1_el1
    msr ttbr1_el1, x0
    ret
    NESTED_END write_ttbr1_el1

    NESTED_ENTRY TriggerIsb
    isb
    ret
    NESTED_END TriggerIsb

    NESTED_ENTRY SmashTlb
    isb
	tlbi vmalle1
	dsb ish
	dsb sy
	isb
    ret
    NESTED_END SmashTlb

    NESTED_ENTRY GetSCTLREL1
    mrs x0, sctlr_el1
    ret
    NESTED_END GetSCTLREL1

    NESTED_ENTRY SetSCTLREL1
    msr sctlr_el1, x0
    ret
    NESTED_END SetSCTLREL1

    NESTED_ENTRY SetSCTLREL1_hold
    msr sctlr_el1, x0
    mov x22, #0xDEAD
    b ContinueExecution
    NESTED_END SetSCTLREL1_hold
#define SCTLR_M 2_1
    NESTED_ENTRY write_tcr
    MSR TCR_EL1, X0
    mov x28, #0xDEAD
    ret
    NESTED_END write_tcr


    NESTED_ENTRY read_tcr
    mrs X0, TCR_EL1
    ret
    NESTED_END read_tcr


    NESTED_ENTRY DisalbeMMU
    isb
    dsb nsh

    // Disable MMU while we mess with translation registers
    mrs x6, sctlr_el1
    and x6, x6, ~SCTLR_M
    msr sctlr_el1, x6

    isb
    dsb nsh
    ret

    NESTED_END DisalbeMMU
    NESTED_ENTRY HoldForArmTests
    mov x20, #0xDEAF
    NESTED_END HoldForArmTests
    END