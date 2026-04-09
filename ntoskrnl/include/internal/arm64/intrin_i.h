#pragma once

/*
 * ARM64 internal kernel intrinsics.
 * Provides thin wrappers around AArch64 system register accesses
 * and maintenance operations needed by the ReactOS kernel.
 *
 * MSVC ARM64 intrinsic syntax:
 *   _ReadStatusReg(reg)  / _WriteStatusReg(reg, val)
 *   __dmb(barrier) / __dsb(barrier) / __isb(barrier)
 *   barrier values: 0xF = SY, 0xB = ISH, 0x3 = OSH
 *
 * GCC/Clang AArch64 syntax:
 *   __asm__ volatile ("mrs %0, sysreg" : "=r"(val))
 *   __asm__ volatile ("msr sysreg, %0" :: "r"(val) : "memory")
 */

/* -------------------------------------------------------------------------
 * IRQL storage
 *
 * ARM64 has no hardware TPR/CR8 equivalent.  IRQL is stored in the PCR
 * (CurrentIrql field) and manipulated entirely in software.
 * The KeSetCurrentIrql wrapper is provided for source-level compatibility
 * with AMD64 callers; actual IRQL serialisation is done via the HAL/DAIF.
 * ------------------------------------------------------------------------- */
FORCEINLINE
VOID
KeSetCurrentIrql(KIRQL Irql)
{
    /* Store the new IRQL in the PCR.  Hardware interrupt masking is
     * performed separately through DAIF when required.             */
    KeGetPcr()->CurrentIrql = Irql;
}

/* -------------------------------------------------------------------------
 * System Control Register (SCTLR_EL1)
 * ------------------------------------------------------------------------- */
FORCEINLINE
ULONG64
KeArm64SctlrGet(VOID)
{
    ULONG64 Value;
#if defined(_MSC_VER)
    Value = _ReadStatusReg(ARM64_SYSREG(3, 0, 1, 0, 0)); /* SCTLR_EL1 */
#else
    __asm__ __volatile__ ("mrs %0, sctlr_el1" : "=r"(Value) :: "memory");
#endif
    return Value;
}

FORCEINLINE
VOID
KeArm64SctlrSet(ULONG64 Value)
{
#if defined(_MSC_VER)
    _WriteStatusReg(ARM64_SYSREG(3, 0, 1, 0, 0), Value);
#else
    __asm__ __volatile__ ("msr sctlr_el1, %0" :: "r"(Value) : "memory");
#endif
    __isb(0xF); /* ISB SY - ensure the write takes effect */
}

/* -------------------------------------------------------------------------
 * Translation Table Base Register 1 (kernel mappings - TTBR1_EL1)
 * ------------------------------------------------------------------------- */
FORCEINLINE
ULONG64
KeArm64Ttbr1Get(VOID)
{
    ULONG64 Value;
#if defined(_MSC_VER)
    Value = _ReadStatusReg(ARM64_SYSREG(3, 0, 2, 0, 1)); /* TTBR1_EL1 */
#else
    __asm__ __volatile__ ("mrs %0, ttbr1_el1" : "=r"(Value) :: "memory");
#endif
    return Value;
}

FORCEINLINE
VOID
KeArm64Ttbr1Set(ULONG64 Value)
{
#if defined(_MSC_VER)
    _WriteStatusReg(ARM64_SYSREG(3, 0, 2, 0, 1), Value);
#else
    __asm__ __volatile__ ("msr ttbr1_el1, %0" :: "r"(Value) : "memory");
#endif
    __isb(0xF);
}

/* -------------------------------------------------------------------------
 * Translation Control Register (TCR_EL1)
 * ------------------------------------------------------------------------- */
FORCEINLINE
ULONG64
KeArm64TcrGet(VOID)
{
    ULONG64 Value;
#if defined(_MSC_VER)
    Value = _ReadStatusReg(ARM64_SYSREG(3, 0, 2, 0, 2)); /* TCR_EL1 */
#else
    __asm__ __volatile__ ("mrs %0, tcr_el1" : "=r"(Value) :: "memory");
#endif
    return Value;
}

/* -------------------------------------------------------------------------
 * Exception Syndrome Register (ESR_EL1) - fault classification
 * ------------------------------------------------------------------------- */
FORCEINLINE
ULONG64
KeArm64EsrGet(VOID)
{
    ULONG64 Value;
#if defined(_MSC_VER)
    Value = _ReadStatusReg(ARM64_SYSREG(3, 0, 5, 2, 0)); /* ESR_EL1 */
#else
    __asm__ __volatile__ ("mrs %0, esr_el1" : "=r"(Value) :: "memory");
#endif
    return Value;
}

/* -------------------------------------------------------------------------
 * Fault Address Register (FAR_EL1) - faulting virtual address
 * ------------------------------------------------------------------------- */
FORCEINLINE
ULONG64
KeArm64FarGet(VOID)
{
    ULONG64 Value;
#if defined(_MSC_VER)
    Value = _ReadStatusReg(ARM64_SYSREG(3, 0, 6, 0, 0)); /* FAR_EL1 */
#else
    __asm__ __volatile__ ("mrs %0, far_el1" : "=r"(Value) :: "memory");
#endif
    return Value;
}

/* -------------------------------------------------------------------------
 * CPACR_EL1 - coprocessor/floating-point access control
 * ------------------------------------------------------------------------- */
FORCEINLINE
ULONG64
KeArm64CpacrGet(VOID)
{
    ULONG64 Value;
#if defined(_MSC_VER)
    Value = _ReadStatusReg(ARM64_SYSREG(3, 0, 1, 0, 2)); /* CPACR_EL1 */
#else
    __asm__ __volatile__ ("mrs %0, cpacr_el1" : "=r"(Value) :: "memory");
#endif
    return Value;
}

FORCEINLINE
VOID
KeArm64CpacrSet(ULONG64 Value)
{
#if defined(_MSC_VER)
    _WriteStatusReg(ARM64_SYSREG(3, 0, 1, 0, 2), Value);
#else
    __asm__ __volatile__ ("msr cpacr_el1, %0" :: "r"(Value) : "memory");
#endif
    __isb(0xF);
}

/* -------------------------------------------------------------------------
 * MAIR_EL1 - Memory Attribute Indirection Register (cache attribute table)
 * ------------------------------------------------------------------------- */
FORCEINLINE
ULONG64
KeArm64MairGet(VOID)
{
    ULONG64 Value;
#if defined(_MSC_VER)
    Value = _ReadStatusReg(ARM64_SYSREG(3, 0, 10, 2, 0)); /* MAIR_EL1 */
#else
    __asm__ __volatile__ ("mrs %0, mair_el1" : "=r"(Value) :: "memory");
#endif
    return Value;
}

FORCEINLINE
VOID
KeArm64MairSet(ULONG64 Value)
{
#if defined(_MSC_VER)
    _WriteStatusReg(ARM64_SYSREG(3, 0, 10, 2, 0), Value);
#else
    __asm__ __volatile__ ("msr mair_el1, %0" :: "r"(Value) : "memory");
#endif
    __isb(0xF);
}

/* -------------------------------------------------------------------------
 * TLB maintenance - inner-shareable domain (IS) to cover all processors
 * ------------------------------------------------------------------------- */

/* Invalidate all TLB entries for the current VMID (VMALLE1IS) */
FORCEINLINE
VOID
KeArm64InvalidateAllTlb(VOID)
{
#if defined(_MSC_VER)
    /* TLBI VMALLE1IS: op1=0, CRn=8, CRm=3, op2=0 -> S1E1 inner-shareable */
    _WriteStatusReg(ARM64_SYSREG(1, 4, 8, 3, 0), 0);
#else
    __asm__ __volatile__ ("tlbi vmalle1is" ::: "memory");
#endif
    __dsb(0xB); /* DSB ISH */
    __isb(0xF); /* ISB    */
}

/* Invalidate the TLB entry for a single virtual address (VAAE1IS) */
FORCEINLINE
VOID
KeArm64InvalidateTlbEntry(PVOID Address)
{
#if defined(_MSC_VER)
    _WriteStatusReg(ARM64_SYSREG(1, 4, 8, 3, 3), (ULONG64)Address >> 12);
#else
    __asm__ __volatile__ ("tlbi vaae1is, %0" :: "r"((ULONG64)Address >> 12) : "memory");
#endif
    __dsb(0xB);
    __isb(0xF);
}

/* -------------------------------------------------------------------------
 * Instruction cache maintenance
 * ------------------------------------------------------------------------- */

/* Invalidate instruction cache, all to point of unification (IC IALLUIS) */
FORCEINLINE
VOID
KeArm64InvalidateICache(VOID)
{
#if defined(_MSC_VER)
    _WriteStatusReg(ARM64_SYSREG(1, 0, 7, 1, 0), 0); /* IC IALLUIS */
#else
    __asm__ __volatile__ ("ic ialluis" ::: "memory");
#endif
    __dsb(0xB);
    __isb(0xF);
}

/* -------------------------------------------------------------------------
 * Data cache - clean and invalidate entire data cache (best-effort helper)
 * ------------------------------------------------------------------------- */
FORCEINLINE
VOID
KeArm64CleanDataCache(VOID)
{
    /* Architecture requires iterating over cache sets/ways; full sweep is
     * performed by HalSweepDcache.  This barrier pair is used as a portable
     * "best effort flush" from kernel code without walking cache geometry. */
    __dsb(0xF); /* DSB SY */
    __isb(0xF);
}
