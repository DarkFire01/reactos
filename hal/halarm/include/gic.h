
#pragma once

/* ---- GICv2 Distributor register offsets (byte offsets from GICD base) ---- */
#define GICD_CTLR           0x000   /* Distributor Control Register             */
#define GICD_TYPER          0x004   /* Interrupt Controller Type Register        */
#define GICD_IIDR           0x008   /* Distributor Implementer Identification    */
#define GICD_IGROUPR(n)     (0x080 + 4*(n))  /* Interrupt Group (n covers 32 ints) */
#define GICD_ISENABLER(n)   (0x100 + 4*(n))  /* Interrupt Set-Enable               */
#define GICD_ICENABLER(n)   (0x180 + 4*(n))  /* Interrupt Clear-Enable             */
#define GICD_ISPENDR(n)     (0x200 + 4*(n))  /* Interrupt Set-Pending              */
#define GICD_ICPENDR(n)     (0x280 + 4*(n))  /* Interrupt Clear-Pending            */
#define GICD_ISACTIVER(n)   (0x300 + 4*(n))  /* Interrupt Set-Active               */
#define GICD_ICACTIVER(n)   (0x380 + 4*(n))  /* Interrupt Clear-Active             */
#define GICD_IPRIORITYR(n)  (0x400 + 4*(n))  /* Interrupt Priority (8 bits each)   */
#define GICD_ITARGETSR(n)   (0x800 + 4*(n))  /* Interrupt Processor Targets        */
#define GICD_ICFGR(n)       (0xC00 + 4*(n))  /* Interrupt Config (edge/level)      */
#define GICD_SGIR           0xF00   /* Software Generated Interrupt Register     */
#define GICD_PIDR2          0xFE8   /* Peripheral ID2 (GIC revision field)       */

/* GICD_CTLR bits */
#define GICD_CTLR_ENABLE    (1u << 0)

/* GICD_SGIR fields */
#define GICD_SGIR_TGTLIST_SHIFT     16
#define GICD_SGIR_TGTFLT_SHIFT      24
#define GICD_SGIR_TGTFLT_LIST       (0u << GICD_SGIR_TGTFLT_SHIFT)
#define GICD_SGIR_TGTFLT_OTHERS     (1u << GICD_SGIR_TGTFLT_SHIFT)
#define GICD_SGIR_TGTFLT_SELF       (2u << GICD_SGIR_TGTFLT_SHIFT)

/* ---- GICv2 CPU Interface register offsets (byte offsets from GICC base) ---- */
#define GICC_CTLR           0x000   /* CPU Interface Control Register            */
#define GICC_PMR            0x004   /* Interrupt Priority Mask Register          */
#define GICC_BPR            0x008   /* Binary Point Register                     */
#define GICC_IAR            0x00C   /* Interrupt Acknowledge Register            */
#define GICC_EOIR           0x010   /* End Of Interrupt Register                 */
#define GICC_RPR            0x014   /* Running Priority Register                 */
#define GICC_HPPIR          0x018   /* Highest Priority Pending Interrupt        */
#define GICC_ABPR           0x01C   /* Aliased Binary Point Register             */
#define GICC_AIAR           0x020   /* Aliased Interrupt Acknowledge Register    */
#define GICC_AEOIR          0x024   /* Aliased End Of Interrupt Register         */
#define GICC_AHPPIR         0x028   /* Aliased Highest Priority Pending          */
#define GICC_APR(n)         (0x0D0 + 4*(n))  /* Active Priorities Register        */
#define GICC_NSAPR(n)       (0x0E0 + 4*(n))  /* Non-Secure Active Priorities      */
#define GICC_IIDR           0x0FC   /* CPU Interface Identification Register     */
#define GICC_DIR            0x1000  /* Deactivate Interrupt Register             */

/* GICC_CTLR bits */
#define GICC_CTLR_ENABLE    (1u << 0)
#define GICC_CTLR_ACKCTL    (1u << 2)   /* IRQ/FIQ Acknowledge control           */
#define GICC_CTLR_FIQEN     (1u << 3)   /* Signal FIQ for Group 0 interrupts     */
#define GICC_CTLR_CBPR      (1u << 4)   /* Common Binary Point Register          */
#define GICC_CTLR_EOIMODENS (1u << 9)   /* Non-Secure EOI mode                   */

/* IAR / EOIR interrupt ID mask (10 bits in GICv2) */
#define GICC_IAR_INTID_MASK     0x3FFu
#define GICC_IAR_SPURIOUS       1023u   /* Spurious interrupt ID                 */

/* Default QEMU virt GICv2 physical base addresses */
#define GICD_QEMU_BASE      0x08000000ULL
#define GICC_QEMU_BASE      0x08010000ULL

/* Max supported SPI count (beyond 32 SGI/PPI) */
#define GIC_MAX_IRQS        1020u

/*
 * IRQL -> GIC priority mapping.
 * GIC uses inverted semantics: lower number = higher priority.
 * The GICC_PMR is written with the threshold below which interrupts are
 * blocked; a PMR of 0xFF accepts everything.
 *
 * Mapping (8 significant priority bits with 32-step granularity):
 *   HIGH_LEVEL(31)      -> PMR 0x00  (no interrupts blocked)
 *   IPI_LEVEL(29)       -> PMR 0x10
 *   CLOCK2_LEVEL(28)    -> PMR 0x20
 *   PROFILE_LEVEL(27)   -> PMR 0x30
 *   DISPATCH_LEVEL(2)   -> PMR 0x80
 *   APC_LEVEL(1)        -> PMR 0xC0
 *   PASSIVE_LEVEL(0)    -> PMR 0xFF  (all interrupts allowed)
 */
extern UCHAR HalpIrqlToPriorityTable[32];
extern UCHAR HalpPriorityToIrqlTable[256];

/* GIC base addresses (set during HalpInitPhase0 from ACPI MADT) */
extern ULONG_PTR HalpGicDistributorBase;
extern ULONG_PTR HalpGicCpuInterfaceBase;
extern ULONG     HalpGicMaxIrq;

/* Read/write helpers for MMIO-mapped GIC registers */
#define GIC_READ32(base, off)       READ_REGISTER_ULONG((PULONG)((base) + (off)))
#define GIC_WRITE32(base, off, val) WRITE_REGISTER_ULONG((PULONG)((base) + (off)), (val))
#define GIC_READ8(base, off)        READ_REGISTER_UCHAR((PUCHAR)((base) + (off)))
#define GIC_WRITE8(base, off, val)  WRITE_REGISTER_UCHAR((PUCHAR)((base) + (off)), (val))
