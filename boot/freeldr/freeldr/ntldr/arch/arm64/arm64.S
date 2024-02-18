 #include <kxarm64.h>
    EXTERN ContinueExecution
    NESTED_ENTRY write_ttbr0_el1
    mov x28, x0
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
    NESTED_END SetSCTLREL1

    NESTED_ENTRY SetSCTLREL1_hold
    msr sctlr_el1, x0
    mov x22, #0xDEAD
    b ContinueExecution
    NESTED_END SetSCTLREL1_hold

    NESTED_ENTRY write_tcr
    MSR TCR_EL1, X0
    mov x28, #0xDEAD
    ret
    NESTED_END write_tcr


    NESTED_ENTRY read_tcr
    mrs X0, TCR_EL1
    ret
    NESTED_END read_tcr

    

    NESTED_ENTRY HoldForArmTests
    mov x20, #0xDEAF
    NESTED_END HoldForArmTests
    END