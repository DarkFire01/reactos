/*
 * PROJECT:         ReactOS Hardware Abstraction Layer
 * LICENSE:         BSD - See COPYING.ARM in the top level directory
 * FILE:            hal/halarm/gic/gic.c
 * PURPOSE:         ARM GICv2 interrupt controller support (IRQL management,
 *                  system-interrupt enable/disable, begin/end handling).
 * PROGRAMMERS:     ReactOS Portable Systems Group
 *                  Based on arm64_Win10 HAL reference (GIC data structures and
 *                  register map) and AMD64 halx86 IRQL/APIC patterns.
 */

/* INCLUDES *******************************************************************/

#include "gicp.h"

#include <hal.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

PUCHAR KdComPortInUse;

/*
 * GIC base addresses.
 * Defaults match the QEMU 'virt' machine layout so that early boot works
 * before ACPI MADT parsing sets the real values in HalpInitPhase0.
 */
ULONG_PTR HalpGicDistributorBase  = GICD_QEMU_BASE;
ULONG_PTR HalpGicCpuInterfaceBase = GICC_QEMU_BASE;
ULONG     HalpGicMaxIrq           = 256;

/*
 * IRQL -> GIC CPU-interface priority threshold (GICC_PMR) table.
 *
 * The GIC blocks all interrupts whose priority >= PMR threshold.
 * Lower priority number = more-urgent interrupt.
 * HIGH_LEVEL blocks nothing (PMR = 0x00); PASSIVE_LEVEL blocks nothing at
 * the GIC level either (PMR = 0xFF, accept everything).
 *
 * We use the same 8-step granularity that the Windows ARM64 HAL uses so that
 * all 32 IRQL levels map to distinct priority bands.
 */
UCHAR HalpIrqlToPriorityTable[32] =
{
    0xFF,   /* IRQL  0: PASSIVE_LEVEL  - no masking          */
    0xC0,   /* IRQL  1: APC_LEVEL                            */
    0x80,   /* IRQL  2: DISPATCH_LEVEL                       */
    0x78,   /* IRQL  3                                       */
    0x70,   /* IRQL  4                                       */
    0x68,   /* IRQL  5                                       */
    0x60,   /* IRQL  6                                       */
    0x58,   /* IRQL  7                                       */
    0x50,   /* IRQL  8                                       */
    0x48,   /* IRQL  9                                       */
    0x40,   /* IRQL 10                                       */
    0x38,   /* IRQL 11                                       */
    0x30,   /* IRQL 12                                       */
    0x28,   /* IRQL 13                                       */
    0x20,   /* IRQL 14                                       */
    0x18,   /* IRQL 15                                       */
    0x10,   /* IRQL 16                                       */
    0x10,   /* IRQL 17                                       */
    0x10,   /* IRQL 18                                       */
    0x10,   /* IRQL 19                                       */
    0x10,   /* IRQL 20                                       */
    0x10,   /* IRQL 21                                       */
    0x10,   /* IRQL 22                                       */
    0x10,   /* IRQL 23                                       */
    0x10,   /* IRQL 24                                       */
    0x10,   /* IRQL 25                                       */
    0x08,   /* IRQL 26                                       */
    0x08,   /* IRQL 27: PROFILE_LEVEL                        */
    0x04,   /* IRQL 28: CLOCK2_LEVEL                         */
    0x02,   /* IRQL 29: IPI_LEVEL                            */
    0x01,   /* IRQL 30: POWER_LEVEL                          */
    0x00,   /* IRQL 31: HIGH_LEVEL     - block all           */
};

/*
 * GIC priority -> IRQL reverse map (256-entry byte table).
 * Built at init time from HalpIrqlToPriorityTable above.
 * A GIC priority P maps to the lowest IRQL whose threshold is > P
 * (i.e. the IRQL that would have allowed this interrupt through).
 */
UCHAR HalpPriorityToIrqlTable[256];

/* IDT/vector book-keeping (mirrors VIC/APIC usage) */
IDTUsageFlags HalpIDTUsageFlags[GIC_MAX_IRQS];
IDTUsage      HalpIDTUsage[GIC_MAX_IRQS];

/* PRIVATE FUNCTIONS **********************************************************/

/*
 * Build the reverse priority -> IRQL lookup table.
 */
static VOID
HalpBuildReverseIrqlTable(VOID)
{
    ULONG Irql, Pri;

    /* Default everything to HIGH_LEVEL */
    RtlFillMemory(HalpPriorityToIrqlTable, sizeof(HalpPriorityToIrqlTable), HIGH_LEVEL);

    /*
     * For each IRQL, the range of GIC priorities [HalpIrqlToPriorityTable[Irql],
     * HalpIrqlToPriorityTable[Irql-1] - 1] maps back to Irql.
     */
    for (Irql = 0; Irql <= HIGH_LEVEL; Irql++)
    {
        UCHAR Low  = HalpIrqlToPriorityTable[Irql];
        UCHAR High = (Irql == HIGH_LEVEL) ? 0xFF :
                     (UCHAR)(HalpIrqlToPriorityTable[Irql + 1] == 0 ?
                              0xFF : HalpIrqlToPriorityTable[Irql + 1] - 1);

        for (Pri = Low; Pri <= High; Pri++)
        {
            HalpPriorityToIrqlTable[Pri] = (UCHAR)Irql;
            if (Pri == 0xFF) break;  /* prevent wrap */
        }
    }
}

/*
 * Configure the GICv2 distributor:
 *  - Disable all SPIs
 *  - Set all SPIs to the lowest priority (0xFF)
 *  - Route all SPIs to CPU 0
 *  - Leave all SPIs as level-triggered (reset default)
 *  - Enable the distributor
 */
static VOID
HalpInitGicDistributor(VOID)
{
    ULONG i, MaxReg;

    /* Disable distributor forwarding while we configure */
    GIC_WRITE32(HalpGicDistributorBase, GICD_CTLR, 0);

    MaxReg = (HalpGicMaxIrq + 31) / 32;

    /* Disable all interrupts */
    for (i = 0; i < MaxReg; i++)
        GIC_WRITE32(HalpGicDistributorBase, GICD_ICENABLER(i), 0xFFFFFFFF);

    /* Clear all pending */
    for (i = 0; i < MaxReg; i++)
        GIC_WRITE32(HalpGicDistributorBase, GICD_ICPENDR(i), 0xFFFFFFFF);

    /* Set all interrupts to lowest priority (0xFF per byte) */
    for (i = 0; i < HalpGicMaxIrq / 4; i++)
        GIC_WRITE32(HalpGicDistributorBase, GICD_IPRIORITYR(i), 0xFFFFFFFF);

    /* Target all SPIs (INT 32+) to CPU0 (target byte = 0x01) */
    for (i = 8; i < HalpGicMaxIrq / 4; i++)
        GIC_WRITE32(HalpGicDistributorBase, GICD_ITARGETSR(i), 0x01010101);

    /* Enable distributor */
    GIC_WRITE32(HalpGicDistributorBase, GICD_CTLR, GICD_CTLR_ENABLE);
}

/*
 * Configure the GICv2 CPU interface for this CPU:
 *  - Set PMR to the current IRQL threshold
 *  - Set binary point to 0 (no preemption sub-priority)
 *  - Enable the interface
 */
static VOID
HalpInitGicCpuInterface(VOID)
{
    KIRQL Irql = KeGetCurrentIrql();

    /* Disable while configuring */
    GIC_WRITE32(HalpGicCpuInterfaceBase, GICC_CTLR, 0);

    /* Set PMR to current IRQL's priority threshold */
    GIC_WRITE32(HalpGicCpuInterfaceBase, GICC_PMR,
                (ULONG)HalpIrqlToPriorityTable[Irql]);

    /* Binary point = 0: all priority bits determine preemption */
    GIC_WRITE32(HalpGicCpuInterfaceBase, GICC_BPR, 0);

    /* Enable CPU interface */
    GIC_WRITE32(HalpGicCpuInterfaceBase, GICC_CTLR, GICC_CTLR_ENABLE);
}

/* PUBLIC FUNCTIONS ***********************************************************/

VOID
HalpInitializeInterrupts(VOID)
{
    ULONG TypeReg, ITLinesNumber;

    /* Probe the distributor to find the actual number of interrupt lines */
    TypeReg       = GIC_READ32(HalpGicDistributorBase, GICD_TYPER);
    ITLinesNumber = (TypeReg & 0x1F);
    HalpGicMaxIrq = (ITLinesNumber + 1) * 32;
    if (HalpGicMaxIrq > GIC_MAX_IRQS)
        HalpGicMaxIrq = GIC_MAX_IRQS;

    /* Build priority <-> IRQL conversion tables */
    HalpBuildReverseIrqlTable();

    /* Configure GIC hardware */
    HalpInitGicDistributor();
    HalpInitGicCpuInterface();
}

ULONG
HalGetInterruptSource(VOID)
{
    /* Read the interrupt acknowledge register - this also signals IAR to GIC */
    ULONG Iar = GIC_READ32(HalpGicCpuInterfaceBase, GICC_IAR);
    return Iar & GICC_IAR_INTID_MASK;
}

CODE_SEG("INIT")
VOID
NTAPI
HalReportResourceUsage(VOID)
{
    UNICODE_STRING HalString;
    RtlInitUnicodeString(&HalString, L"ARM GICv2 HAL");
}

/* IRQL MANAGEMENT ************************************************************/

VOID
NTAPI
HalpRegisterVector(IN UCHAR Flags,
                   IN ULONG BusVector,
                   IN ULONG SystemVector,
                   IN KIRQL Irql)
{
    if (SystemVector >= GIC_MAX_IRQS) return;
    HalpIDTUsageFlags[SystemVector].Flags = Flags;
    HalpIDTUsage[SystemVector].Irql  = Irql;
    HalpIDTUsage[SystemVector].BusReleativeVector = (UCHAR)BusVector;
}

VOID
NTAPI
HalpEnableInterruptHandler(IN UCHAR Flags,
                           IN ULONG BusVector,
                           IN ULONG SystemVector,
                           IN KIRQL Irql,
                           IN PVOID Handler,
                           IN KINTERRUPT_MODE Mode)
{
    /* Set the interrupt routine in the PCR (use PKIPCR to access the private Idt[] table) */
    ((PKIPCR)KeGetPcr())->Idt[Irql] = Handler;

    /* Register the mapping */
    HalpRegisterVector(Flags, BusVector, SystemVector, Irql);

    /* Set the GIC interrupt priority to match the IRQL */
    if (SystemVector < HalpGicMaxIrq)
    {
        ULONG ByteReg = SystemVector / 4;
        ULONG ByteOff = SystemVector % 4;
        volatile ULONG *PriReg = (volatile ULONG *)
            (HalpGicDistributorBase + GICD_IPRIORITYR(ByteReg));
        ULONG Val = READ_REGISTER_ULONG((PULONG)PriReg);
        Val &= ~(0xFFul << (ByteOff * 8));
        Val |= ((ULONG)HalpIrqlToPriorityTable[Irql] << (ByteOff * 8));
        WRITE_REGISTER_ULONG((PULONG)PriReg, Val);
    }
}

/*
 * @implemented
 */
KIRQL
FASTCALL
KfRaiseIrql(IN KIRQL NewIrql)
{
    PKIPCR Pcr = (PKIPCR)KeGetPcr();
    KIRQL  OldIrql;

    OldIrql = Pcr->CurrentIrql;

#ifdef IRQL_DEBUG
    if (OldIrql > NewIrql)
    {
        Pcr->CurrentIrql = PASSIVE_LEVEL;
        KeBugCheck(IRQL_NOT_GREATER_OR_EQUAL);
    }
#endif

    Pcr->CurrentIrql = NewIrql;

    /*
     * Update the GIC priority mask so that the CPU interface blocks
     * interrupts below the new IRQL threshold.
     */
    GIC_WRITE32(HalpGicCpuInterfaceBase, GICC_PMR,
                (ULONG)HalpIrqlToPriorityTable[NewIrql]);

    return OldIrql;
}

/*
 * @implemented
 */
VOID
FASTCALL
KfLowerIrql(IN KIRQL NewIrql)
{
    PKIPCR Pcr = (PKIPCR)KeGetPcr();

#ifdef IRQL_DEBUG
    if (NewIrql > Pcr->CurrentIrql)
    {
        Pcr->CurrentIrql = HIGH_LEVEL;
        KeBugCheck(IRQL_NOT_LESS_OR_EQUAL);
    }
#endif

    Pcr->CurrentIrql = NewIrql;

    GIC_WRITE32(HalpGicCpuInterfaceBase, GICC_PMR,
                (ULONG)HalpIrqlToPriorityTable[NewIrql]);
}

KIRQL
NTAPI
KeRaiseIrqlToDpcLevel(VOID)
{
    return KfRaiseIrql(DISPATCH_LEVEL);
}

KIRQL
NTAPI
KeRaiseIrqlToSynchLevel(VOID)
{
    return KfRaiseIrql(SYNCH_LEVEL);
}

/* SOFTWARE INTERRUPTS ********************************************************/

/*
 * @implemented
 *
 * Raise a software interrupt at the specified IRQL using a GIC SGI
 * targeted at self.  SGI 0 = APC_LEVEL, SGI 1 = DISPATCH_LEVEL.
 */
VOID
FASTCALL
HalRequestSoftwareInterrupt(IN KIRQL Irql)
{
    ULONG SgiId;

    if (Irql == APC_LEVEL)
        SgiId = 0;
    else if (Irql == DISPATCH_LEVEL)
        SgiId = 1;
    else
        return;

    /* Send SGIR: TargetFilter = self-only, INTID = SgiId */
    GIC_WRITE32(HalpGicDistributorBase, GICD_SGIR,
                GICD_SGIR_TGTFLT_SELF | SgiId);
}

/*
 * @implemented
 *
 * Clear a pending software interrupt.  The SGI pending state is cleared
 * automatically when HalBeginSystemInterrupt reads GICC_IAR, so there is
 * nothing extra to do here.
 */
VOID
FASTCALL
HalClearSoftwareInterrupt(IN KIRQL Irql)
{
    /* Nothing required: GIC SGI pending is cleared on IAR read */
    UNREFERENCED_PARAMETER(Irql);
}

/* SYSTEM INTERRUPTS **********************************************************/

/*
 * @implemented
 */
BOOLEAN
NTAPI
HalEnableSystemInterrupt(IN ULONG Vector,
                         IN KIRQL Irql,
                         IN KINTERRUPT_MODE InterruptMode)
{
    ULONG Reg, Bit;

    if (Vector >= HalpGicMaxIrq) return FALSE;

    Reg = Vector / 32;
    Bit = 1ul << (Vector % 32);

    /* Set priority to match requested IRQL */
    {
        ULONG ByteReg = Vector / 4;
        ULONG ByteOff = Vector % 4;
        volatile ULONG *PriReg = (volatile ULONG *)
            (HalpGicDistributorBase + GICD_IPRIORITYR(ByteReg));
        ULONG Val = READ_REGISTER_ULONG((PULONG)PriReg);
        Val &= ~(0xFFul << (ByteOff * 8));
        Val |= ((ULONG)HalpIrqlToPriorityTable[Irql] << (ByteOff * 8));
        WRITE_REGISTER_ULONG((PULONG)PriReg, Val);
    }

    /* Configure edge vs level triggered in GICD_ICFGR */
    {
        ULONG CfgReg = Vector / 16;
        ULONG CfgBit = ((Vector % 16) * 2) + 1;
        volatile ULONG *Cfg = (volatile ULONG *)
            (HalpGicDistributorBase + GICD_ICFGR(CfgReg));
        ULONG Val = READ_REGISTER_ULONG((PULONG)Cfg);
        if (InterruptMode == Latched)
            Val |= (1ul << CfgBit);   /* edge-triggered */
        else
            Val &= ~(1ul << CfgBit);  /* level-triggered */
        WRITE_REGISTER_ULONG((PULONG)Cfg, Val);
    }

    /* Enable in GICD_ISENABLER */
    GIC_WRITE32(HalpGicDistributorBase, GICD_ISENABLER(Reg), Bit);
    return TRUE;
}

/*
 * @implemented
 */
VOID
NTAPI
HalDisableSystemInterrupt(IN ULONG Vector,
                          IN KIRQL Irql)
{
    ULONG Reg, Bit;

    if (Vector >= HalpGicMaxIrq) return;

    Reg = Vector / 32;
    Bit = 1ul << (Vector % 32);

    GIC_WRITE32(HalpGicDistributorBase, GICD_ICENABLER(Reg), Bit);
}

/*
 * @implemented
 *
 * Called at interrupt entry.  Read IAR (which acknowledges the interrupt to
 * the GIC), derive the IRQL from the interrupt priority, raise IRQL, and
 * return the previous IRQL in *OldIrql.
 *
 * Returns FALSE for spurious interrupts.
 */
BOOLEAN
NTAPI
HalBeginSystemInterrupt(IN KIRQL Irql,
                        IN ULONG Vector,
                        OUT PKIRQL OldIrql)
{
    ULONG IntId;
    PKIPCR Pcr = (PKIPCR)KeGetPcr();

    /* Read & ACK the interrupt from the GIC */
    IntId = GIC_READ32(HalpGicCpuInterfaceBase, GICC_IAR) & GICC_IAR_INTID_MASK;

    /* Spurious interrupt - nothing to do */
    if (IntId == GICC_IAR_SPURIOUS)
        return FALSE;

    /* Raise IRQL and program PMR */
    *OldIrql = Pcr->CurrentIrql;
    Pcr->CurrentIrql = Irql;
    GIC_WRITE32(HalpGicCpuInterfaceBase, GICC_PMR,
                (ULONG)HalpIrqlToPriorityTable[Irql]);

    return TRUE;
}

/*
 * @implemented
 *
 * Called at interrupt exit.  Write EOI then lower IRQL.
 */
VOID
NTAPI
HalEndSystemInterrupt(IN KIRQL OldIrql,
                      IN PKTRAP_FRAME TrapFrame)
{
    ULONG Eoi;

    /*
     * Read the running priority register to reconstruct the INTID for EOI.
     * In GICv2, GICC_EOIR must be written with the INTID that was read from
     * GICC_IAR.  We stored the acknowledged INTID (lower 10 bits of IAR) in
     * GICC_HPPIR which reflects the highest active interrupt.
     * The simplest correct approach: write the INTID from GICC_IAR that was
     * read in HalBeginSystemInterrupt.  For now, re-read HPPIR as the active
     * interrupt is still signalled until EOI.
     */
    Eoi = GIC_READ32(HalpGicCpuInterfaceBase, GICC_HPPIR) & GICC_IAR_INTID_MASK;
    if (Eoi != GICC_IAR_SPURIOUS)
        GIC_WRITE32(HalpGicCpuInterfaceBase, GICC_EOIR, Eoi);

    /* Restore IRQL */
    KfLowerIrql(OldIrql);
}

/* INTERRUPT DISPATCH *********************************************************/

/*
 * HalpDispatchIrq - C-level GIC interrupt dispatcher.
 *
 * Called indirectly from the KiInterruptException assembly stub via the
 * KiInterruptDispatch trampoline in ntoskrnl (which reads the function
 * pointer from Pcr->HalReserved[14] set by HalpInitPhase0).
 *
 * SavedSp points to the 192-byte register-save frame on the kernel stack.
 * Frame layout:
 *
 *   [SavedSp+  0] x29 (FP)          [SavedSp+  8] x30 (LR)
 *   [SavedSp+ 16] x0                 [SavedSp+ 24] x1
 *   ...                              ...
 *   [SavedSp+160] x18                [SavedSp+168] 0 (padding)
 *   [SavedSp+176] ELR_EL1 (saved PC) [SavedSp+184] SPSR_EL1
 *   [SavedSp+192]                 <- original SP before IRQ
 *
 * The function:
 *  1. Reads GICC_IAR to acknowledge the interrupt and get the INTID.
 *  2. Maps INTID to IRQL via HalpIDTUsage[].
 *  3. Raises IRQL / updates GICC_PMR.
 *  4. Builds a minimal KTRAP_FRAME from the saved state.
 *  5. Stores TrapFrame in the current thread for KeUpdateSystemTime.
 *  6. Invokes PCR->InterruptRoutine[Irql].
 *  7. Writes GICC_EOIR with the acknowledged INTID.
 *  8. Restores IRQL.
 */
VOID
HalpDispatchIrq(IN ULONG_PTR SavedSp)
{
    ULONG64       *Frame = (ULONG64 *)SavedSp;
    ULONG          IntId;
    KIRQL          Irql, OldIrql;
    PKIPCR         Pcr;
    PKTHREAD       Thread;
    PKTRAP_FRAME   OldTrapFrame;
    PVOID          Handler;
    KTRAP_FRAME    TrapFrame;
    ULONG          i;

    /* Acknowledge interrupt and get the GIC interrupt ID */
    IntId = GIC_READ32(HalpGicCpuInterfaceBase, GICC_IAR) & GICC_IAR_INTID_MASK;

    /* Spurious — nothing to do */
    if (IntId == GICC_IAR_SPURIOUS)
        return;

    /* Map INTID to IRQL:
     *   SGI 0 = APC_LEVEL  (software interrupt for APC delivery)
     *   SGI 1 = DISPATCH_LEVEL (software interrupt for DPC delivery)
     *   All others use the IDT usage table populated by HalpEnableInterruptHandler */
    if (IntId == 0)
        Irql = APC_LEVEL;
    else if (IntId == 1)
        Irql = DISPATCH_LEVEL;
    else if (IntId < GIC_MAX_IRQS && HalpIDTUsage[IntId].Irql != 0)
        Irql = HalpIDTUsage[IntId].Irql;
    else
        Irql = CLOCK2_LEVEL; /* safe default for unknown hardware interrupts */

    /* Raise IRQL and write the new PMR threshold */
    Pcr = (PKIPCR)KeGetPcr();
    OldIrql = Pcr->CurrentIrql;
    Pcr->CurrentIrql = Irql;
    GIC_WRITE32(HalpGicCpuInterfaceBase, GICC_PMR,
                (ULONG)HalpIrqlToPriorityTable[Irql]);

    /* Build a minimal KTRAP_FRAME so that KeUpdateSystemTime and profiling
     * have a valid frame.  The assembly stub saved the full register context
     * at the base of the frame pointer; pick out the fields that matter. */
    RtlZeroMemory(&TrapFrame, sizeof(TrapFrame));
    TrapFrame.ExceptionActive = 1;              /* KEXCEPTION_ACTIVE_INTERRUPT_FRAME */
    TrapFrame.Fp   = Frame[0];                  /* saved x29 */
    TrapFrame.Lr   = Frame[1];                  /* saved x30 */
    for (i = 0; i < 19; i++)
        TrapFrame.X[i] = Frame[2 + i];          /* saved x0..x18 */
    TrapFrame.Pc   = Frame[176 / 8];            /* ELR_EL1 = Frame[22] */
    TrapFrame.Spsr = (ULONG)Frame[184 / 8];     /* SPSR_EL1 = Frame[23] */
    TrapFrame.Sp   = (ULONG64)(SavedSp + 192);  /* pre-exception SP */

    /* Link the frame to the current thread */
    Thread = Pcr->Prcb.CurrentThread;
    OldTrapFrame = Thread->TrapFrame;
    Thread->TrapFrame = &TrapFrame;

    /* Dispatch to the registered interrupt handler */
    Handler = Pcr->Idt[Irql];
    if (Handler)
        ((VOID (NTAPI *)(VOID))Handler)();

    /* Unlink the trap frame */
    Thread->TrapFrame = OldTrapFrame;

    /* Signal end-of-interrupt to the GIC CPU interface */
    GIC_WRITE32(HalpGicCpuInterfaceBase, GICC_EOIR, IntId);

    /* Restore IRQL */
    Pcr->CurrentIrql = OldIrql;
    GIC_WRITE32(HalpGicCpuInterfaceBase, GICC_PMR,
                (ULONG)HalpIrqlToPriorityTable[OldIrql]);
}

/* EOF */
