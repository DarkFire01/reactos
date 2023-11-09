 #include <kxarm64.h>
    
    NESTED_ENTRY ArmReadCntFrq
    mrs   x0, cntfrq_el0           // Read CNTFRQ
    ret
    NESTED_END ArmReadCntFrq
    
    NESTED_ENTRY ArmWriteCntFrq
    msr   cntfrq_el0, x0           // Write to CNTFRQ
    ret
    NESTED_END ArmWriteCntFrq
    
    NESTED_ENTRY ArmReadCntPct
    mrs   x0, cntpct_el0           // Read CNTPCT (Physical counter register)
    ret
    NESTED_END ArmReadCntPct
    
    NESTED_ENTRY ArmReadCntkCtl
    mrs   x0, cntkctl_el1          // Read CNTK_CTL (Timer PL1 Control Register)
    ret
    NESTED_END ArmReadCntkCtl
    
    NESTED_ENTRY ArmWriteCntkCtl
    msr   cntkctl_el1, x0          // Write to CNTK_CTL (Timer PL1 Control Register)
    ret
    NESTED_END ArmWriteCntkCtl
    
    NESTED_ENTRY ArmReadCntpTval
    mrs   x0, cntp_tval_el0        // Read CNTP_TVAL (PL1 physical timer value register)
    ret
    NESTED_END ArmReadCntpTval
    
    NESTED_ENTRY ArmWriteCntpTval
    msr   cntp_tval_el0, x0        // Write to CNTP_TVAL (PL1 physical timer value register)
    ret
    NESTED_END ArmWriteCntpTval
    
    NESTED_ENTRY ArmReadCntpCtl
    mrs   x0, cntp_ctl_el0         // Read CNTP_CTL (PL1 Physical Timer Control Register)
    ret
    NESTED_END ArmReadCntpCtl
    
    NESTED_ENTRY ArmWriteCntpCtl
    msr   cntp_ctl_el0, x0         // Write to  CNTP_CTL (PL1 Physical Timer Control Register)
    ret
    NESTED_END ArmWriteCntpCtl
    
    NESTED_ENTRY ArmReadCntvTval
    mrs   x0, cntv_tval_el0        // Read CNTV_TVAL (Virtual Timer Value register)
    ret
    NESTED_END ArmReadCntvTval
    
    NESTED_ENTRY ArmWriteCntvTval
    msr   cntv_tval_el0, x0        // Write to CNTV_TVAL (Virtual Timer Value register)
    ret
    NESTED_END ArmWriteCntvTval
    
    NESTED_ENTRY ArmReadCntvCtl
    mrs   x0, cntv_ctl_el0         // Read CNTV_CTL (Virtual Timer Control Register)
    ret
    NESTED_END ArmReadCntvCtl
    
    NESTED_ENTRY ArmWriteCntvCtl
    msr   cntv_ctl_el0, x0         // Write to CNTV_CTL (Virtual Timer Control Register)
    ret
    NESTED_END ArmWriteCntvCtl
    
    NESTED_ENTRY ArmReadCntvCt
    mrs  x0, cntvct_el0            // Read CNTVCT  (Virtual Count Register)
    ret
    NESTED_END ArmReadCntvCt
    
    NESTED_ENTRY ArmReadCntpCval
    mrs   x0, cntp_cval_el0        // Read CNTP_CTVAL (Physical Timer Compare Value Register)
    ret
    NESTED_END ArmReadCntpCval
    
    NESTED_ENTRY ArmWriteCntpCval
    msr   cntp_cval_el0, x0        // Write to CNTP_CTVAL (Physical Timer Compare Value Register)
    ret
    NESTED_END ArmWriteCntpCval
    
    NESTED_ENTRY ArmReadCntvCval
    mrs   x0, cntv_cval_el0        // Read CNTV_CTVAL (Virtual Timer Compare Value Register)
    ret
    NESTED_END ArmReadCntvCval
    
    NESTED_ENTRY ArmWriteCntvCval
    msr   cntv_cval_el0, x0        // write to  CNTV_CTVAL (Virtual Timer Compare Value Register)
    ret
    NESTED_END ArmWriteCntvCval
    
    NESTED_ENTRY ArmReadCntvOff
    mrs   x0, cntvoff_el2          // Read CNTVOFF (virtual Offset register)
    ret
    NESTED_END ArmReadCntvOff
    
    NESTED_ENTRY ArmWriteCntvOff
    msr   cntvoff_el2, x0          // Write to CNTVOFF (Virtual Offset register)
    ret
    NESTED_END ArmWriteCntvOff

    END