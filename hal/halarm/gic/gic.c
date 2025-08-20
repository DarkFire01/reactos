#include "gicp.h"

/* INCLUDES *******************************************************************/

#include <hal.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/
// Default GICv2 base addresses (platform-specific, may need override)
// Default GICv2 base addresses (QEMU virt platform)
#define GIC_DIST_BASE   0x08000000
#define GIC_CPU_BASE    0x08010000

// GIC register offsets
#define GICD_ISENABLER  0x100
#define GICC_CTLR       0x000
#define GICC_PMR        0x004
#define GICC_IAR        0x00C
#define GICC_EOIR       0x010

//
// GIC SPI and extended SPI ranges
//
#define ARM_GIC_ARCH_SPI_MIN      32
#define ARM_GIC_ARCH_SPI_MAX      1019
#define ARM_GIC_ARCH_EXT_SPI_MIN  4096
#define ARM_GIC_ARCH_EXT_SPI_MAX  5119

// GIC Distributor
#define ARM_GIC_ICDDCR   0x000        // Distributor Control Register
#define ARM_GIC_ICDICTR  0x004        // Interrupt Controller Type Register
#define ARM_GIC_ICDIIDR  0x008        // Implementer Identification Register

// ICDICTR is also called GICD_TYPER.

// Intids per LSB for EXT_SPI_RANGE and ITLINES.
#define ARM_GIC_ICDICTR_INTID_RANGE_RESOLUTION  (32)

// Converts an register range of IntIds to the maximum IntId using Base as an
// offset.
#define ARM_GIC_ICDICTR_INTID_RANGE_TO_MAX_INTID(Range, Base) \
  (ARM_GIC_ICDICTR_INTID_RANGE_RESOLUTION * ((Range) + 1) - 1 + (Base))

#define ARM_GIC_ICDICTR_ITLINES_MASK   (0x1F)
#define ARM_GIC_ICDICTR_ITLINES_SHIFT  (0)

// Gets the range for SPI IntIds from TypeReg.
#define ARM_GIC_ICDICTR_GET_SPI_RANGE(TypeReg) \
  (((TypeReg) >> ARM_GIC_ICDICTR_ITLINES_SHIFT) & ARM_GIC_ICDICTR_ITLINES_MASK)

// Converts a range of SPI IntIds to the maximum SPI IntId.
#define ARM_GIC_ICDICTR_SPI_RANGE_TO_MAX_INTID(SpiRange) \
  (((SpiRange) == ARM_GIC_ICDICTR_ITLINES_MASK)          \
       ? ARM_GIC_ARCH_SPI_MAX                   \
       : ARM_GIC_ICDICTR_INTID_RANGE_TO_MAX_INTID(SpiRange, 0))

// Extracts the maximum SPI IntId from TypeReg.
#define ARM_GIC_ICDICTR_GET_SPI_MAX_INTID(TypeReg) \
  ARM_GIC_ICDICTR_SPI_RANGE_TO_MAX_INTID(ARM_GIC_ICDICTR_GET_SPI_RANGE(TypeReg))

#define ARM_GIC_ICDICTR_EXT_SPI_ENABLED      (1 << 8) // Extended SPI enabled bit.
#define ARM_GIC_ICDICTR_EXT_SPI_RANGE_SHIFT  (27)     // Extended SPI range position.
#define ARM_GIC_ICDICTR_EXT_SPI_RANGE_MASK   (0x1F)   // Extended SPI range mask.
#define ARM_GIC_ICDICTR_GET_EXT_SPI_RANGE(TypeReg)      \
  (((TypeReg) >> ARM_GIC_ICDICTR_EXT_SPI_RANGE_SHIFT) & \
   ARM_GIC_ICDICTR_EXT_SPI_RANGE_MASK)

// Extracts the maximum EXT SPI IntId from TypeReg.
#define ARM_GIC_ICDICTR_GET_EXT_SPI_MAX_INTID(TypeReg) \
  ARM_GIC_ICDICTR_INTID_RANGE_TO_MAX_INTID(            \
      ARM_GIC_ICDICTR_GET_EXT_SPI_RANGE(TypeReg), ARM_GIC_ARCH_EXT_SPI_MIN)

// Each reg base below repeats for Number of interrupts / 4 (see GIC spec)
#define ARM_GIC_ICDISR   0x080        // Interrupt Security Registers
#define ARM_GIC_ICDISER  0x100        // Interrupt Set-Enable Registers
#define ARM_GIC_ICDICER  0x180        // Interrupt Clear-Enable Registers
#define ARM_GIC_ICDSPR   0x200        // Interrupt Set-Pending Registers
#define ARM_GIC_ICDICPR  0x280        // Interrupt Clear-Pending Registers
#define ARM_GIC_ICDABR   0x300        // Active Bit Registers

// Each reg base below repeats for Number of interrupts / 4
#define ARM_GIC_ICDIPR  0x400         // Interrupt Priority Registers

// Each reg base below repeats for Number of interrupts
#define ARM_GIC_ICDIPTR  0x800        // Interrupt Processor Target Registers
#define ARM_GIC_ICDICFR  0xC00        // Interrupt Configuration Registers

#define ARM_GIC_ICDPPISR  0xD00       // PPI Status register

// just one of these
#define ARM_GIC_ICDSGIR  0xF00        // Software Generated Interrupt Register

// GICv3 specific registers
#define ARM_GICD_IROUTER    0x6100    // Interrupt Routing Registers
#define ARM_GICD_IROUTER_E  0x8000    // Interrupt Routing Registers

// GICD_CTLR bits
#define ARM_GIC_ICDDCR_ARE  (1 << 4)     // Affinity Routing Enable (ARE)
#define ARM_GIC_ICDDCR_DS   (1 << 6)     // Disable Security (DS)

// GICD_ICDICFR bits
#define ARM_GIC_ICDICFR_WIDTH            32   // ICDICFR is a 32 bit register
#define ARM_GIC_ICDICFR_BYTES            (ARM_GIC_ICDICFR_WIDTH / 8)
#define ARM_GIC_ICDICFR_F_WIDTH          2    // Each F field is 2 bits
#define ARM_GIC_ICDICFR_F_STRIDE         16   // (32/2) F fields per register
#define ARM_GIC_ICDICFR_F_CONFIG1_BIT    1    // Bit number within F field
#define ARM_GIC_ICDICFR_LEVEL_TRIGGERED  0x0  // Level triggered interrupt
#define ARM_GIC_ICDICFR_EDGE_TRIGGERED   0x1  // Edge triggered interrupt

// GICD ESPI registers
//  These registers follow the same bit pattern as the SPI registers.
#define ARM_GIC_ICDISR_E   0x1000   // Interrupt Security Registers
#define ARM_GIC_ICDISER_E  0x1200   // Interrupt Set-Enable for ESPI
#define ARM_GIC_ICDICER_E  0x1400   // Interrupt Clear-Enable Registers
#define ARM_GIC_ICDSPR_E   0x1600   // Interrupt Set-Pending Registers
#define ARM_GIC_ICDICPR_E  0x1800   // Interrupt Clear-Pending Registers
#define ARM_GIC_ICDIPR_E   0x2000   // Interrupt Priority Registers
#define ARM_GIC_ICDICFR_E  0x3000   // Interrupt Configuration Registers

// GIC Redistributor
#define ARM_GICR_CTLR_FRAME_SIZE          SIZE_64KB
#define ARM_GICR_SGI_PPI_FRAME_SIZE       SIZE_64KB
#define ARM_GICR_SGI_VLPI_FRAME_SIZE      SIZE_64KB
#define ARM_GICR_SGI_RESERVED_FRAME_SIZE  SIZE_64KB

// GIC Redistributor Control frame
#define ARM_GICR_TYPER  0x0008          // Redistributor Type Register

// GIC Redistributor TYPER bit assignments
#define ARM_GICR_TYPER_PLPIS      (1 << 0)                // Physical LPIs
#define ARM_GICR_TYPER_VLPIS      (1 << 1)                // Virtual LPIs
#define ARM_GICR_TYPER_DIRECTLPI  (1 << 3)                // Direct LPIs
#define ARM_GICR_TYPER_LAST       (1 << 4)                // Last Redistributor in series
#define ARM_GICR_TYPER_DPGS       (1 << 5)                // Disable Processor Group
                                                          // Selection Support
#define ARM_GICR_TYPER_PROCNO        (0xFFFF << 8)         // Processor Number
#define ARM_GICR_TYPER_COMMONLPIAFF  (0x3 << 24)           // Common LPI Affinity
#define ARM_GICR_TYPER_AFFINITY      (0xFFFFFFFFULL << 32) // Redistributor Affinity

#define ARM_GICR_TYPER_GET_AFFINITY(TypeReg)  (((TypeReg) & \
                                                ARM_GICR_TYPER_AFFINITY) >> 32)

// GIC SGI & PPI Redistributor frame
#define ARM_GICR_ISENABLER  0x0100      // Interrupt Set-Enable Registers
#define ARM_GICR_ICENABLER  0x0180      // Interrupt Clear-Enable Registers

// GIC Cpu interface
#define ARM_GIC_ICCICR   0x00         // CPU Interface Control Register
#define ARM_GIC_ICCPMR   0x04         // Interrupt Priority Mask Register
#define ARM_GIC_ICCBPR   0x08         // Binary Point Register
#define ARM_GIC_ICCIAR   0x0C         // Interrupt Acknowledge Register
#define ARM_GIC_ICCEIOR  0x10         // End Of Interrupt Register
#define ARM_GIC_ICCRPR   0x14         // Running Priority Register
#define ARM_GIC_ICCPIR   0x18         // Highest Pending Interrupt Register
#define ARM_GIC_ICCABPR  0x1C         // Aliased Binary Point Register
#define ARM_GIC_ICCIIDR  0xFC         // Identification Register

#define ARM_GIC_ICDSGIR_FILTER_TARGETLIST    0x0
#define ARM_GIC_ICDSGIR_FILTER_EVERYONEELSE  0x1
#define ARM_GIC_ICDSGIR_FILTER_ITSELF        0x2

// Bit-masks to configure the CPU Interface Control register
#define ARM_GIC_ICCICR_ENABLE_SECURE         0x01
#define ARM_GIC_ICCICR_ENABLE_NS             0x02
#define ARM_GIC_ICCICR_ACK_CTL               0x04
#define ARM_GIC_ICCICR_SIGNAL_SECURE_TO_FIQ  0x08
#define ARM_GIC_ICCICR_USE_SBPR              0x10

// Bit Mask for GICC_IIDR
#define ARM_GIC_ICCIIDR_GET_PRODUCT_ID(IccIidr)    (((IccIidr) >> 20) & 0xFFF)
#define ARM_GIC_ICCIIDR_GET_ARCH_VERSION(IccIidr)  (((IccIidr) >> 16) & 0xF)
#define ARM_GIC_ICCIIDR_GET_REVISION(IccIidr)      (((IccIidr) >> 12) & 0xF)
#define ARM_GIC_ICCIIDR_GET_IMPLEMENTER(IccIidr)   ((IccIidr) & 0xFFF)

// Bit Mask for
#define ARM_GIC_ICCIAR_ACKINTID  0x3FF

//
// GIC SPI and extended SPI ranges
//
#define ARM_GIC_ARCH_SPI_MIN      32
#define ARM_GIC_ARCH_SPI_MAX      1019
#define ARM_GIC_ARCH_EXT_SPI_MIN  4096
#define ARM_GIC_ARCH_EXT_SPI_MAX  5119


// MMIO access helpers
static inline void gic_write32(UINT32 val, uintptr_t addr) {
    *(volatile UINT32 *)addr = val;
}
static inline UINT32 gic_read32(uintptr_t addr) {
    return *(volatile UINT32 *)addr;
}
PUCHAR KdComPortInUse;

/* FUNCTIONS ******************************************************************/
void GicV2_Initialize(void);
VOID
HalpInitializeInterrupts(VOID)
{
    // Initialize the GICv2 interrupt controller
    GicV2_Initialize();
}


ULONG HalGetInterruptSource(VOID)
{
    // Read the interrupt acknowledge register to get the interrupt ID
    return gic_read32(GIC_CPU_BASE + GICC_IAR) & 0x3FF; // 10 bits for int ID
}

// Helper to signal end of interrupt (call from ISR after handling)
void GicV2_EndInterrupt(ULONG InterruptId)
{
    gic_write32(InterruptId, GIC_CPU_BASE + GICC_EOIR);
}

#define GICD_TYPER 0x004
#define GICD_PIDR2 0xFE8

void GicV2_Detect(void)
{
    UINT32 typer = gic_read32(GIC_DIST_BASE + GICD_TYPER);
    UINT32 pidr2 = gic_read32(GIC_DIST_BASE + GICD_PIDR2);

    // Check for GICv2: PIDR2[7:4] == 0x2
    if (((pidr2 >> 4) & 0xF) != 0x2) {
        DbgPrintEarly("GICv2 not detected! PIDR2 = 0x%08X\n", pidr2);
        // Optionally halt or fallback
    } else {
        DbgPrintEarly("GICv2 detected. TYPER = 0x%08X, PIDR2 = 0x%08X\n", typer, pidr2);
    }
}
#define GICD_IPRIORITYR 0x400
#define GICD_ITARGETSR  0x800
#define GICC_BPR        0x008

VOID
ArmGicV2EnableInterruptInterface (
  )
{
  /*
  * Enable the CPU interface in Non-Secure world
  * Note: The ICCICR register is banked when Security extensions are implemented
  */
  gic_write32 ( 0x1, GIC_CPU_BASE + ARM_GIC_ICCICR);
}

VOID
ArmGicV2DisableInterruptInterface (
  )
{
  // Disable Gic Interface
  gic_write32 (0x0, GIC_CPU_BASE + ARM_GIC_ICCICR);
  gic_write32 (0x0, GIC_CPU_BASE + ARM_GIC_ICCPMR);
}

void GicV2_Initialize(void)
{
    UINT32 num_interrupts, i, reg_offset, reg_shift;
    UINT32 cpu_target;

    GicV2_Detect();

    // 1. Disable distributor before config
    gic_write32(0, GIC_DIST_BASE + GICD_CTLR);

    // 2. Get number of interrupts
    num_interrupts = 32 * ((gic_read32(GIC_DIST_BASE + GICD_TYPER) & 0x1F) + 1);

    // 3. Set default priority for all interrupts
    for (i = 0; i < num_interrupts; i++) {
        reg_offset = i / 4;
        reg_shift = (i % 4) * 8;
        UINT32 addr = GIC_DIST_BASE + GICD_IPRIORITYR + (reg_offset * 4);
        UINT32 val = gic_read32(addr);
        val &= ~(0xFF << reg_shift);
        val |= (0x80 << reg_shift); // Default priority 0x80
        gic_write32(val, addr);
    }

    // 4. Route all interrupts to CPU0 (if multiprocessor, adjust as needed)
    cpu_target = 1; // CPU0
    for (i = 32; i < num_interrupts; i++) {
        reg_offset = i / 4;
        reg_shift = (i % 4) * 8;
        UINT32 addr = GIC_DIST_BASE + GICD_ITARGETSR + (reg_offset * 4);
        UINT32 val = gic_read32(addr);
        val &= ~(0xFF << reg_shift);
        val |= (cpu_target << reg_shift);
        gic_write32(val, addr);
    }

    // 5. Set binary point register (no preemption)
    gic_write32(0x7, GIC_CPU_BASE + GICC_BPR);

    // 6. Set priority mask to allow all priorities
    gic_write32(0xFF, GIC_CPU_BASE + GICC_PMR);

    // 7. Enable CPU interface
    gic_write32(1, GIC_CPU_BASE + GICC_CTLR);

    // 8. Enable distributor
    gic_write32(1, GIC_DIST_BASE + GICD_CTLR);
}

CODE_SEG("INIT")
VOID
NTAPI
HalReportResourceUsage(VOID)
{
    UNICODE_STRING HalString;

    /* Build HAL usage */
    RtlInitUnicodeString(&HalString, L"ARM UEFI GIC HAL");
}

KIRQL
FASTCALL
KfRaiseIrql(IN KIRQL NewIrql)
{
    PKPCR Pcr = KeGetPcr();
    KIRQL CurrentIrql;
    /* Read current IRQL */
    CurrentIrql = Pcr->CurrentIrql;

#ifdef IRQL_DEBUG
    /* Validate correct raise */
    if (CurrentIrql > NewIrql)
    {
        /* Crash system */
        Pcr->CurrentIrql = PASSIVE_LEVEL;
        //KeBugCheck(IRQL_NOT_GREATER_OR_EQUAL);
    }
#endif
    /* Set new IRQL */
    Pcr->CurrentIrql = NewIrql;

    /* Return old IRQL */
    return CurrentIrql;
}

VOID
FASTCALL
KfLowerIrql(IN KIRQL NewIrql)
{

    PKPCR Pcr = KeGetPcr();
#ifdef IRQL_DEBUG
    /* Validate correct lower */
    if (OldIrql > Pcr->CurrentIrql)
    {
        /* Crash system */
        Pcr->CurrentIrql = HIGH_LEVEL;
       // KeBugCheck(IRQL_NOT_LESS_OR_EQUAL);
    }
#endif

    /* Save the new IRQL and restore interrupt state */
    Pcr->CurrentIrql = NewIrql;
}

#undef KeGetCurrentIrql

KIRQL
NTAPI
KeGetCurrentIrql()
{
     PKPCR Pcr = KeGetPcr();
    /* Return the IRQL */
    return Pcr->CurrentIrql;
}


/*
 * @implemented
 */
KIRQL
NTAPI
KeRaiseIrqlToDpcLevel(VOID)
{
    PKPCR Pcr = KeGetPcr();
    KIRQL CurrentIrql;

    /* Save and update IRQL */
    CurrentIrql = Pcr->CurrentIrql;
    Pcr->CurrentIrql = DISPATCH_LEVEL;

#ifdef IRQL_DEBUG
    /* Validate correct raise */
  //  if (CurrentIrql > DISPATCH_LEVEL) KeBugCheck(IRQL_NOT_GREATER_OR_EQUAL);
#endif

    /* Return the previous value */
    return CurrentIrql;
}

/*
 * @implemented
 */
KIRQL
NTAPI
KeRaiseIrqlToSynchLevel(VOID)
{
    PKPCR Pcr = KeGetPcr();
    KIRQL CurrentIrql;

    /* Save and update IRQL */
    CurrentIrql = Pcr->CurrentIrql;
    Pcr->CurrentIrql = SYNCH_LEVEL;

#ifdef IRQL_DEBUG
    /* Validate correct raise */
    if (CurrentIrql > SYNCH_LEVEL)
    {
        /* Crash system */
        KeBugCheckEx(IRQL_NOT_GREATER_OR_EQUAL,
                     CurrentIrql,
                     SYNCH_LEVEL,
                     0,
                     1);
    }
#endif

    /* Return the previous value */
    return CurrentIrql;
}


/* SOFTWARE INTERRUPTS ********************************************************/

/*
 * @implemented
 */


#define GICD_SGIR       0xF00
#define GIC_SGI_DPC     0   // SGI 0 for DPC
#define GIC_SGI_APC     1   // SGI 1 for APC

VOID FASTCALL HalRequestSoftwareInterrupt(IN KIRQL Irql)
{
    ULONG SgiId;
    switch (Irql)
    {
        case APC_LEVEL:
            SgiId = GIC_SGI_APC;
            break;
        case DISPATCH_LEVEL:
            SgiId = GIC_SGI_DPC;
            break;
        default:
            // Unsupported software interrupt level
            return;
    }
    // Send SGI to self (target list filter = 0b10 = only this CPU)
    // Use the same GIC_DIST_BASE as above for SGI
    volatile ULONG *sgir = (volatile ULONG *)(GIC_DIST_BASE + GICD_SGIR);
    *sgir = (SgiId & 0xF) | (1 << 24); // TargetListFilter=0b10 (self), SGI ID
}

/*
 * @implemented
 */
VOID FASTCALL HalClearSoftwareInterrupt(IN KIRQL Irql)
{
    // On GICv2, SGIs are cleared by writing to the End Of Interrupt register in the CPU interface.
    // This is handled in the interrupt handler, so nothing is needed here.
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
    UNIMPLEMENTED;
    while (TRUE);
    return FALSE;
}

/*
 * @implemented
 */
VOID
NTAPI
HalDisableSystemInterrupt(IN ULONG Vector,
                          IN KIRQL Irql)
{
    UNIMPLEMENTED;
    while (TRUE);
}

/*
 * @implemented
 */
BOOLEAN
NTAPI
HalBeginSystemInterrupt(IN KIRQL Irql,
                        IN ULONG Vector,
                        OUT PKIRQL OldIrql)
{
    UNIMPLEMENTED;
    while (TRUE);
    return FALSE;
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
    // Enable the interrupt in the GIC distributor
    ULONG reg_offset = SystemVector / 32;
    ULONG bit = SystemVector % 32;
    ULONG addr = GIC_DIST_BASE + GICD_ISENABLER + (reg_offset * 4);
    gic_write32(1U << bit, addr);

    // Set default priority (0x80) for this interrupt
    reg_offset = SystemVector / 4;
    ULONG reg_shift = (SystemVector % 4) * 8;
    addr = GIC_DIST_BASE + GICD_IPRIORITYR + (reg_offset * 4);
    UINT32 val = gic_read32(addr);
    val &= ~(0xFF << reg_shift);
    val |= (0x80 << reg_shift);
    gic_write32(val, addr);

    // Route to CPU0 (target list)
    if (SystemVector >= 32) {
        reg_offset = SystemVector / 4;
        reg_shift = (SystemVector % 4) * 8;
        addr = GIC_DIST_BASE + GICD_ITARGETSR + (reg_offset * 4);
        val = gic_read32(addr);
        val &= ~(0xFF << reg_shift);
        val |= (1 << reg_shift); // CPU0
        gic_write32(val, addr);
    }

    // Note: Handler registration is platform-specific and not handled here.
}

/*
 * @implemented
 */
VOID
NTAPI
HalEndSystemInterrupt(IN KIRQL OldIrql,
                      IN PKTRAP_FRAME TrapFrame)
{
    UNIMPLEMENTED;
    while (TRUE);
}

/* EOF */
