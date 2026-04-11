
/*
 * PROJECT:         ReactOS HAL
 * LICENSE:         BSD - See COPYING.ARM in the top level directory
 * PURPOSE:         QEMU virt ARM64 Board-Specific HAL Initialization
 *                  (GIC-based, ARM generic timer)
 */

/* INCLUDES *******************************************************************/

#include <hal.h>
#include "../timers/generic/timer.h"
#define NDEBUG
#include <debug.h>

/* Forward declaration: defined in gic.c, HAL-internal, not exported */
VOID HalpDispatchIrq(IN ULONG_PTR SavedSp);

/* PRIVATE CONSTANTS **********************************************************/

/*
 * QEMU virt: ARM generic timer EL1 Physical Timer fires as GIC PPI INTID 30.
 * PPIs occupy GIC INTIDs 16-31 (per-CPU, no ITARGETSR routing needed).
 */
#define HALP_ARM_TIMER_INTID    30

/* Target tick rate: 100 Hz (10 ms per tick, matching HalpCurrentTimeIncrement = 100000) */
#define HALP_TICKS_PER_SECOND   100

/*
 * Index into KIPCR::HalReserved[] used to store the HAL IRQ dispatch
 * function pointer.  The kernel's KiInterruptDispatch reads this slot at
 * interrupt time and calls through it, avoiding any direct ntoskrnl→HAL
 * symbol dependency.  Slot 14 is the last entry (HalReserved is [15]).
 */
#define HALP_PCR_IRQ_DISPATCH_SLOT  14

/* PRIVATE FUNCTIONS **********************************************************/

VOID
HalpInitPhase0(IN PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    UINT32 Freq, Period;
    ULONG  Val;

    /*
     * Register the HAL interrupt dispatch function in the PCR so that the
     * kernel's KiInterruptDispatch trampoline can call it without needing a
     * direct ntoskrnl→HAL symbol reference (which would fail at link time).
     */
    KeGetPcr()->HalReserved[HALP_PCR_IRQ_DISPATCH_SLOT] = (PVOID)HalpDispatchIrq;

    /*
     * Determine the ARM generic timer frequency and compute the reload
     * value for a 10 ms (100 Hz) periodic tick.
     */
    Freq   = ArmReadCntFrq();
    Period = Freq / HALP_TICKS_PER_SECOND;

    DPRINT("HAL: ARM generic timer freq=%u Hz, period=%u ticks (10ms)\n",
           Freq, Period);

    /*
     * Program the EL1 Physical Timer countdown and enable it.
     * ARM_ARCH_TIMER_ENABLE (bit 0) = enable timer.
     * IMASK (bit 1) must remain clear so the interrupt can fire.
     * Writing CNTP_TVAL starts the countdown immediately.
     */
    ArmWriteCntpTval(Period);
    ArmWriteCntpCtl(ARM_ARCH_TIMER_ENABLE);

    /*
     * GIC: set the priority of PPI INTID 30 to match CLOCK2_LEVEL.
     * INTID 30 is in GICD_IPRIORITYR[7], byte offset 2 (bits [23:16]).
     */
    Val  = GIC_READ32(HalpGicDistributorBase, GICD_IPRIORITYR(7));
    Val &= ~(0xFFUL << 16);
    Val |=  ((ULONG)HalpIrqlToPriorityTable[CLOCK2_LEVEL] << 16);
    GIC_WRITE32(HalpGicDistributorBase, GICD_IPRIORITYR(7), Val);

    /*
     * GIC: enable PPI INTID 30 in GICD_ISENABLER[0] (bit 30).
     * This forwards the timer interrupt from the distributor to CPU0.
     */
    GIC_WRITE32(HalpGicDistributorBase, GICD_ISENABLER(0), (1UL << HALP_ARM_TIMER_INTID));

    /*
     * Register the clock interrupt handler in the PCR and IDT usage tables.
     * SystemVector = INTID 30 so that HalpDispatchIrq can look up
     * HalpIDTUsage[30].Irql = CLOCK2_LEVEL at runtime.
     * The ARM generic timer interrupt is level-sensitive.
     */
    HalpEnableInterruptHandler(IDT_DEVICE,
                               HALP_ARM_TIMER_INTID,
                               HALP_ARM_TIMER_INTID,
                               CLOCK2_LEVEL,
                               HalpClockInterrupt,
                               LevelSensitive);
}

VOID
HalpInitPhase1(VOID)
{
    /* No additional board-specific interrupt sources for QEMU virt bringup. */
}

/* EOF */