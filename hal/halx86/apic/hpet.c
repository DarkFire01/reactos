/*
 * PROJECT:     ReactOS Hardware Abstraction Layer
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     High Precision Event Timer as the system clock source
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 * REFERENCES:  IA-PC HPET Specification, revision 1.0a
 *              https://wiki.osdev.org/HPET
 */

/*
 * Why this exists
 * ---------------
 * The APIC HALs drive the system clock from the CMOS real time clock: the RTC
 * raises a periodic interrupt on ISA IRQ 8, the boot processor turns that into
 * KeUpdateSystemTime() and broadcasts a clock IPI to the other processors.
 *
 * That works, but it hangs on a loaded SMP guest. The RTC only re-arms when
 * register C is read, which means every single tick depends on two port I/O
 * accesses completing behind the CMOS spinlock, and both QEMU's and
 * VirtualBox's virtual RTC have been observed to simply stop advancing under
 * load. When the boot processor's clock dies nothing anywhere decrements a
 * thread quantum, so preemption stops and the machine freezes with the desktop
 * still painted and no fault reported.
 *
 * The HPET has none of that. It is memory mapped, it free runs from its own
 * counter, and a periodic timer re-arms itself in hardware, so a tick needs no
 * acknowledgement of any kind. Windows moved to it for the same reason - see
 * HalpHpetInitializeClock() and friends in Vista's halmacpi.
 *
 * How the clock source is chosen
 * ------------------------------
 * The HPET is described by an ACPI table, so it is only reachable in the HALs
 * that have ACPI: halaacpi (ACPI APIC uniprocessor) and halmacpi (ACPI APIC
 * SMP). halapic has no ACPI tables, HalAcpiGetTable() answers NULL there, and
 * the RTC stays in charge.
 *
 * The switch happens in HalpInitPhase0(), not in HalpInitializeClock(), because
 * the ACPI table cache needs the loader block and HalpInitializeClock() is not
 * given one. The RTC is therefore started first and handed over a moment later,
 * while interrupts are still disabled - see HalpHpetInitializeClock() below.
 * Everything that can fail is checked before the RTC is stopped, so a machine
 * with an unusable HPET keeps the clock it already had.
 *
 * What is deliberately not implemented
 * ------------------------------------
 * The main counter is not exposed as a time source. ReactOS answers
 * KeQueryPerformanceCounter() from the TSC and this file does not change that,
 * which is also why there is no rollover handling here: only the periodic
 * interrupt is consumed, and periodic accumulation wraps correctly in hardware.
 */

/* INCLUDES *******************************************************************/

#include <hal.h>
#include "apicp.h"
#include <smp.h>
#define NDEBUG
#include <debug.h>

/* HPET REGISTERS *************************************************************/

/* General registers, relative to the event timer block base address */
#define HPET_CAPABILITIES           0x000 /* General capabilities and ID (R) */
#define HPET_PERIOD                 0x004 /* Main counter tick period, in fs (R) */
#define HPET_CONFIGURATION          0x010 /* General configuration (R/W) */
#define HPET_INTERRUPT_STATUS       0x020 /* General interrupt status (R/W clear) */
#define HPET_MAIN_COUNTER           0x0F0 /* Main counter value (R/W) */

/* Per timer registers. Each timer owns 0x20 bytes starting at 0x100. */
#define HPET_TIMER_BLOCK(n)         (0x100 + 0x20 * (n))
#define HPET_TIMER_CONFIG(n)        (HPET_TIMER_BLOCK(n) + 0x00) /* (R/W) */
#define HPET_TIMER_ROUTE_CAP(n)     (HPET_TIMER_BLOCK(n) + 0x04) /* (R) */
#define HPET_TIMER_COMPARATOR(n)    (HPET_TIMER_BLOCK(n) + 0x08) /* (R/W) */

/* HPET_CAPABILITIES */
#define HPET_CAP_NUM_TIM_MASK       0x00001F00 /* Index of the last timer */
#define HPET_CAP_NUM_TIM_SHIFT      8
#define HPET_CAP_COUNT_SIZE         0x00002000 /* Main counter is 64 bit */
#define HPET_CAP_LEG_ROUTE          0x00008000 /* Legacy replacement route capable */

/* HPET_CONFIGURATION */
#define HPET_CFG_ENABLE             0x00000001 /* Main counter runs, timers may fire */
#define HPET_CFG_LEGACY_ROUTE       0x00000002 /* Timer 0 -> IRQ 0, timer 1 -> IRQ 8 */

/* HPET_TIMER_CONFIG */
#define HPET_TN_LEVEL               0x00000002 /* Level triggered, rather than edge */
#define HPET_TN_INT_ENABLE          0x00000004 /* The timer raises an interrupt */
#define HPET_TN_PERIODIC            0x00000008 /* Periodic, rather than one shot */
#define HPET_TN_PERIODIC_CAP        0x00000010 /* Periodic capable (R) */
#define HPET_TN_SIZE_CAP            0x00000020 /* 64 bit capable (R) */
#define HPET_TN_VALUE_SET           0x00000040 /* Next comparator write sets the period */
#define HPET_TN_32BIT               0x00000100 /* Force 32 bit operation */
#define HPET_TN_ROUTE_MASK          0x00003E00 /* I/O APIC input the timer drives */
#define HPET_TN_ROUTE_SHIFT         9
#define HPET_TN_FSB_ENABLE          0x00004000 /* Deliver by FSB message, not I/O APIC */

/* The timer this file drives. Timer 0 is the only one every HPET must have. */
#define HPET_CLOCK_TIMER            0

/* Where legacy replacement mode puts timer 0, per the specification */
#define HPET_LEGACY_ROUTE_INDEX     2

/* First I/O APIC input above the ISA range, see HalpHpetSelectInterruptRoute() */
#define HPET_FIRST_NON_ISA_INDEX    16

/* 100ns expressed in femtoseconds, the unit HPET states its period in */
#define FEMTOSECONDS_PER_100NS      100000000ULL

/* GLOBALS ********************************************************************/

/*
 * Set once HalpHpetInitializeClock() has handed the clock over. rtctimer.c
 * reads it to know that a tick needs no acknowledgement at its source, and that
 * a rate change belongs to us.
 */
BOOLEAN HalpHpetEnabled = FALSE;

/* Virtual address of the event timer block, valid once HalpHpetEnabled */
static PUCHAR HalpHpetAddress;

/* Main counter tick period in femtoseconds, straight out of the hardware */
static ULONG HalpHpetPeriod;

/* Current period of timer 0, in main counter ticks */
static ULONG HalpHpetTicksPerInterrupt;

/*
 * The time increments this clock offers, in 100ns units, smallest first. These
 * are the intervals NT itself uses, and they are all exact in 100ns units so
 * that no fractional accounting is needed the way the RTC's 1024Hz rate needs
 * it. What the hardware can actually produce is a whole number of main counter
 * ticks, so the value handed to the kernel is computed back from the tick count
 * rather than taken from this table - see HalpHpetTicksToIncrement().
 */
static const ULONG HalpHpetIncrements[] =
{
     10000, /*  1.0000 ms */
     20000, /*  2.0000 ms */
     40000, /*  4.0000 ms */
     80000, /*  8.0000 ms */
    156250, /* 15.6250 ms, the rate the RTC clock defaults to */
};

#define HPET_DEFAULT_INCREMENT_INDEX (RTL_NUMBER_OF(HalpHpetIncrements) - 1)

/* A rate change requested from HalSetTimeIncrement(), applied by the next tick */
static BOOLEAN HalpHpetSetClockRate = FALSE;
static ULONG HalpHpetNextTicks;

/* PRIVATE FUNCTIONS **********************************************************/

static
ULONG
HpetRead(ULONG Register)
{
    return READ_REGISTER_ULONG((PULONG)(HalpHpetAddress + Register));
}

static
VOID
HpetWrite(ULONG Register, ULONG Value)
{
    WRITE_REGISTER_ULONG((PULONG)(HalpHpetAddress + Register), Value);
}

/*!
    \brief Converts a main counter tick count into a time increment in 100ns units.

    Integer division loses a fraction of a tick in both directions, so the
    kernel is told what the hardware will really deliver rather than what was
    asked for. Anything else would let the system clock drift against the
    interrupts that advance it.
*/
static
ULONG
HalpHpetTicksToIncrement(ULONG Ticks)
{
    return (ULONG)(((ULONGLONG)Ticks * HalpHpetPeriod) / FEMTOSECONDS_PER_100NS);
}

/*!
    \brief Converts a time increment in 100ns units into main counter ticks.
*/
static
ULONG
HalpHpetIncrementToTicks(ULONG Increment)
{
    return (ULONG)((Increment * FEMTOSECONDS_PER_100NS) / HalpHpetPeriod);
}

/*!
    \brief Programs timer 0 to fire every \a Ticks main counter ticks.

    The main counter is left running. Changing the period of a timer that is
    already periodic takes the two step sequence from section 2.3.9.2.2 of the
    specification: with HPET_TN_VALUE_SET set, the first write to the comparator
    is the absolute value to fire at and the second is the period to accumulate
    from then on.
*/
static
VOID
HalpHpetProgramTimer(ULONG Ticks)
{
    ULONG Config;

    Config = HpetRead(HPET_TIMER_CONFIG(HPET_CLOCK_TIMER));
    Config |= HPET_TN_VALUE_SET;
    HpetWrite(HPET_TIMER_CONFIG(HPET_CLOCK_TIMER), Config);

    /* Fire one period from where the counter stands, then every period after */
    HpetWrite(HPET_TIMER_COMPARATOR(HPET_CLOCK_TIMER),
              HpetRead(HPET_MAIN_COUNTER) + Ticks);
    HpetWrite(HPET_TIMER_COMPARATOR(HPET_CLOCK_TIMER), Ticks);

    HalpHpetTicksPerInterrupt = Ticks;
}

/*!
    rief Picks how timer 0 reaches the processor.

    Legacy replacement mode is tried first, the way Vista's HAL chooses it. It
    puts timer 0 on I/O APIC input 2 - which is where ISA IRQ 0 is overridden to
    on a PC - and, while it is on, detaches the 8254 and the RTC from their
    interrupt lines.

    That last part is the whole point, and it is not a nicety. Input 2 is shared
    with the 8254, which the firmware leaves free running, and a clock sharing
    an input counts every one of that device's interrupts as a tick: with the
    8254 at 100Hz beside this timer at 64Hz the system clock ran two and a third
    times too fast, while the I/O APIC line itself looked perfectly correct,
    because the two sources are counted separately. Nothing here wants either
    legacy timer - the RTC is the clock being replaced, and the APIC HALs never
    arm the 8254 - so giving up both costs nothing.

    Failing that, the timer is routed by its own Tn_INT_ROUTE_CAP, and only to
    an input above the ISA range: the ISA inputs are either shared with a legacy
    timer or handed out to devices later, and there is no way to reserve one
    here.

    
eturns TRUE if a route was found, with the I/O APIC input in  OutIndex
             and whether legacy replacement mode is needed in  OutLegacy.
*/
static
BOOLEAN
HalpHpetSelectInterruptRoute(
    _In_ ULONG Capabilities,
    _Out_ PUCHAR OutIndex,
    _Out_ PBOOLEAN OutLegacy)
{
    ULONG RouteCap;
    LONG Index;

    if (Capabilities & HPET_CAP_LEG_ROUTE)
    {
        *OutIndex = HPET_LEGACY_ROUTE_INDEX;
        *OutLegacy = TRUE;
        return TRUE;
    }

    RouteCap = HpetRead(HPET_TIMER_ROUTE_CAP(HPET_CLOCK_TIMER));

    for (Index = APIC_MAX_IRQ - 1; Index >= HPET_FIRST_NON_ISA_INDEX; Index--)
    {
        if (!(RouteCap & (1UL << Index)))
            continue;

        /* Leave the input alone if something already answers on it */
        if (HalpIrqToVector((UCHAR)Index) != APIC_FREE_VECTOR)
            continue;

        *OutIndex = (UCHAR)Index;
        *OutLegacy = FALSE;
        return TRUE;
    }

    return FALSE;
}

/* PUBLIC FUNCTIONS ***********************************************************/

/*!
    \brief Takes the system clock over from the RTC, if an HPET can be used.

    Called from HalpInitPhase0() with interrupts disabled, after
    HalpInitializeClock() has already started the RTC clock. Every reason to
    refuse is checked before the RTC is touched, so returning FALSE leaves the
    machine exactly as it was found.

    \returns TRUE if the HPET now drives APIC_CLOCK_VECTOR.
*/
CODE_SEG("INIT")
BOOLEAN
NTAPI
HalpHpetInitializeClock(
    _In_ PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    PHPET_TABLE HpetTable;
    PHARDWARE_PTE Pte;
    PHYSICAL_ADDRESS PhysicalAddress;
    PUCHAR BaseAddress;
    ULONG_PTR EFlags;
    ULONG Capabilities, Period, Config, TimerConfig;
    ULONG Ticks, Increment;
    UCHAR Index;
    BOOLEAN LegacyRoute = FALSE;

    /* No ACPI, no HPET. This is the halapic case. */
    HpetTable = HalAcpiGetTable(LoaderBlock, HPET_SIGNATURE);
    if (HpetTable == NULL)
    {
        DPRINT1("HPET: no HPET table, the RTC keeps the clock\n");
        return FALSE;
    }

    /* Only the memory mapped form is defined; an I/O space HPET does not exist */
    if (HpetTable->BaseAddress.AddressSpaceID != 0 ||
        HpetTable->BaseAddress.Address.QuadPart == 0)
    {
        DPRINT1("HPET: unusable base address, the RTC keeps the clock\n");
        return FALSE;
    }

    /*
     * Map the event timer block. The block is 1024 bytes and page aligned in
     * practice, so a single page covers it. HalpMapPhysicalMemory64() maps it
     * cached, which is wrong for a device: fix the PTE up by hand, the way the
     * I/O APIC mapping does.
     */
    PhysicalAddress = HpetTable->BaseAddress.Address;
    BaseAddress = HalpMapPhysicalMemory64(PhysicalAddress, 1);
    if (BaseAddress == NULL)
    {
        DPRINT1("HPET: could not map %I64x, the RTC keeps the clock\n",
                PhysicalAddress.QuadPart);
        return FALSE;
    }

    Pte = HalAddressToPte(BaseAddress);
    Pte->CacheDisable = 1;
    _ReadWriteBarrier();
    HalpFlushTLB();

    /* Everything below reads registers, so the base has to be live already */
    HalpHpetAddress = BaseAddress;

    Capabilities = HpetRead(HPET_CAPABILITIES);
    Period = HpetRead(HPET_PERIOD);

    /*
     * A period of zero would divide by zero below, and the specification caps
     * it at 100ns, so anything longer means we are not looking at an HPET.
     */
    if (Period == 0 || Period > FEMTOSECONDS_PER_100NS)
    {
        DPRINT1("HPET: implausible period %lu fs, the RTC keeps the clock\n", Period);
        goto Failure;
    }
    HalpHpetPeriod = Period;

    /*
     * Timer 0 must be able to run periodically. One shot would mean re-arming
     * from inside the clock interrupt, which is exactly the dependency on the
     * handler running that this file exists to remove.
     */
    TimerConfig = HpetRead(HPET_TIMER_CONFIG(HPET_CLOCK_TIMER));
    if (!(TimerConfig & HPET_TN_PERIODIC_CAP))
    {
        DPRINT1("HPET: timer 0 is not periodic capable, the RTC keeps the clock\n");
        goto Failure;
    }

    /* Work out how the timer will reach the processor */
    if (!HalpHpetSelectInterruptRoute(Capabilities, &Index, &LegacyRoute))
    {
        DPRINT1("HPET: no usable interrupt route, the RTC keeps the clock\n");
        goto Failure;
    }

    /* Work out the default rate, and refuse a counter too coarse to reach it */
    Ticks = HalpHpetIncrementToTicks(HalpHpetIncrements[HPET_DEFAULT_INCREMENT_INDEX]);
    if (Ticks == 0 || HalpHpetIncrementToTicks(HalpHpetIncrements[0]) == 0)
    {
        DPRINT1("HPET: period %lu fs is too coarse, the RTC keeps the clock\n", Period);
        goto Failure;
    }

    /*
     * Past this point nothing may fail: the RTC is about to stop.
     *
     * HalpInitializePICs() left EFLAGS.IF set, and the clock vector is armed,
     * so keep a tick out of the middle of the handover - the clock has two
     * owners for the length of it.
     */
    EFlags = __readeflags();
    _disable();

    /*
     * Mask the RTC's I/O APIC input first, then silence the device itself, so
     * that a tick already latched in the CMOS cannot arrive once interrupts are
     * enabled again and be counted against the wrong increment.
     */
    HalDisableSystemInterrupt(APIC_CLOCK_VECTOR, CLOCK_LEVEL);
    HalpRtcDisableClock();

    /*
     * Stop the main counter while timer 0 is being set up, and settle the
     * routing now rather than at the end: legacy replacement mode is what
     * detaches the 8254 from the input that is about to be unmasked.
     */
    Config = HpetRead(HPET_CONFIGURATION);
    Config &= ~(HPET_CFG_ENABLE | HPET_CFG_LEGACY_ROUTE);
    if (LegacyRoute)
        Config |= HPET_CFG_LEGACY_ROUTE;
    HpetWrite(HPET_CONFIGURATION, Config);

    /*
     * Edge triggered, so that a tick needs no acknowledgement at all: level
     * triggered would leave the interrupt asserted until the general interrupt
     * status bit were cleared, and HalBeginSystemInterrupt()'s lazy IRQL path
     * ends an interrupt it has decided to defer, which on a still asserted
     * level input would make the I/O APIC send it straight back.
     *
     * 32 bit mode keeps every access to the counter and the comparator a single
     * 32 bit read or write on a 32 bit host. Periodic accumulation still wraps
     * correctly in hardware, and nothing here reads the counter as a timebase.
     */
    TimerConfig &= ~(HPET_TN_LEVEL | HPET_TN_FSB_ENABLE | HPET_TN_ROUTE_MASK);
    TimerConfig |= HPET_TN_INT_ENABLE | HPET_TN_PERIODIC | HPET_TN_32BIT;
    TimerConfig |= (Index << HPET_TN_ROUTE_SHIFT) & HPET_TN_ROUTE_MASK;
    HpetWrite(HPET_TIMER_CONFIG(HPET_CLOCK_TIMER), TimerConfig);

    /* Start counting from zero, so the first period is a whole one */
    HpetWrite(HPET_MAIN_COUNTER, 0);
    HalpHpetProgramTimer(Ticks);

    /* Point the clock vector at the input the timer drives, and unmask it */
    HalpVectorToIndex[APIC_CLOCK_VECTOR] = Index;
    HalEnableSystemInterrupt(APIC_CLOCK_VECTOR, CLOCK_LEVEL, Latched);

    /* Let it run */
    Config |= HPET_CFG_ENABLE;
    HpetWrite(HPET_CONFIGURATION, Config);

    HalpHpetEnabled = TRUE;

    /* Restore the interrupt state HalpInitializePICs() left behind */
    __writeeflags(EFlags);

    /* Tell the kernel the range this clock really covers */
    Increment = HalpHpetTicksToIncrement(Ticks);
    KeSetTimeIncrement(Increment,
                       HalpHpetTicksToIncrement(
                           HalpHpetIncrementToTicks(HalpHpetIncrements[0])));

    DPRINT1("HPET: clock on timer 0, %lu timers, %lu fs/tick, increment %lu*100ns, "
            "I/O APIC input %u%s\n",
            ((Capabilities & HPET_CAP_NUM_TIM_MASK) >> HPET_CAP_NUM_TIM_SHIFT) + 1,
            Period,
            Increment,
            Index,
            LegacyRoute ? " (legacy replacement)" : "");

    return TRUE;

Failure:
    HalpHpetAddress = NULL;
    HalpUnmapVirtualAddress(BaseAddress, 1);
    return FALSE;
}

/*!
    \brief Applies a pending rate change. Called from the clock interrupt.

    \returns The time increment this tick accounted for, in 100ns units.
*/
ULONG
NTAPI
HalpHpetUpdateClockRate(VOID)
{
    ULONG Increment = HalpHpetTicksToIncrement(HalpHpetTicksPerInterrupt);

    /* Whatever the new rate is, this tick was one of the old ones */
    if (HalpHpetSetClockRate)
    {
        HalpHpetProgramTimer(HalpHpetNextTicks);
        HalpHpetSetClockRate = FALSE;
    }

    return Increment;
}

/*!
    \brief Selects the largest supported increment that is at most \a Increment.

    The change is recorded here and applied by the next clock interrupt, so that
    the hardware is only ever reprogrammed from one place.

    \returns The increment that will be in force, in 100ns units.
*/
ULONG
NTAPI
HalpHpetSetTimeIncrement(
    _In_ ULONG Increment)
{
    ULONG Index, Ticks;

    /* Walk down to the largest entry that fits, and never below the smallest */
    for (Index = RTL_NUMBER_OF(HalpHpetIncrements) - 1; Index > 0; Index--)
    {
        if (HalpHpetIncrements[Index] <= Increment)
            break;
    }

    Ticks = HalpHpetIncrementToTicks(HalpHpetIncrements[Index]);

    HalpHpetNextTicks = Ticks;
    HalpHpetSetClockRate = TRUE;

    return HalpHpetTicksToIncrement(Ticks);
}

/* EOF */
