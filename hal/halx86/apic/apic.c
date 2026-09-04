/*
 * PROJECT:         ReactOS HAL
 * LICENSE:         GNU GPL - See COPYING in the top level directory
 * FILE:            hal/halx86/apic/apic.c
 * PURPOSE:         HAL APIC Management and Control Code
 * PROGRAMMERS:     Timo Kreuzer (timo.kreuzer@reactos.org)
 * REFERENCES:      https://web.archive.org/web/20190407074221/http://www.joseflores.com/docs/ExploringIrql.html
 *                  https://www.codeproject.com/KB/system/soviet_kernel_hack.aspx
 *                  http://bbs.unixmap.net/thread-2022-1-1.html (DEAD_LINK)
 *                  https://codemachine.com/articles/interrupt_dispatching.html
 *                  https://www.osronline.com/article.cfm%5Earticle=211.htm
 */

/* INCLUDES *******************************************************************/

#include <hal.h>
#include "apicp.h"
#include <smp.h>
#define NDEBUG
#include <debug.h>

#ifndef _M_AMD64
#define APIC_LAZY_IRQL
#endif

/* GLOBALS ********************************************************************/

ULONG ApicVersion;

/*
 * The two halves of the interrupt allocation record, both in SOFTWARE.
 *
 *   HalpVectorToIndex[Vector] -> the I/O APIC input that vector serves
 *   HalpGsivToVector[Gsiv]    -> the vector currently allocated to that input
 *
 * They are maintained strictly together, by HalpAllocateSystemInterrupt and by
 * HalpSetVectorState, and are the only authority on what is allocated.  A
 * redirection register is never read back to answer that question - see
 * HalpIrqToVector.  Both are APIC_FREE_VECTOR-filled in ApicInitializeIOApic.
 */
UCHAR HalpVectorToIndex[256];
UCHAR HalpGsivToVector[256];

/* The units and interrupt sources the firmware tables listed */
extern HALP_APIC_INFO_TABLE HalpApicInfoTable;

/* Every I/O APIC the firmware described, mapped one page each behind
   IOAPIC_BASE, with the global system interrupts it serves */
HALP_IOAPIC_UNIT HalpIoApics[HALP_MAX_IOAPICS];
ULONG HalpIoApicCount;

/* One past the highest global system interrupt any unit serves */
ULONG HalpMaxGsi;

/* Last value written to each redirection entry, so the I/O APICs can be
   reprogrammed after a transition that lost them */
IOAPIC_REDIRECTION_REGISTER HalpIoApicShadow[HALP_MAX_INPUTS];

#ifndef _M_AMD64
const UCHAR
HalpIRQLtoTPR[32] =
{
    0x00, /*  0 PASSIVE_LEVEL */
    0x3d, /*  1 APC_LEVEL */
    0x41, /*  2 DISPATCH_LEVEL */
    0x41, /*  3 \  */
    0x51, /*  4  \ */
    0x61, /*  5  | */
    0x71, /*  6  | */
    0x81, /*  7  | */
    0x91, /*  8  | */
    0xa1, /*  9  | */
    0xb1, /* 10  | */
    0xb1, /* 11  | */
    0xb1, /* 12  | */
    0xb1, /* 13  | */
    0xb1, /* 14  | */
    0xb1, /* 15 DEVICE IRQL */
    0xb1, /* 16  | */
    0xb1, /* 17  | */
    0xb1, /* 18  | */
    0xb1, /* 19  | */
    0xb1, /* 20  | */
    0xb1, /* 21  | */
    0xb1, /* 22  | */
    0xb1, /* 23  | */
    0xb1, /* 24  | */
    0xb1, /* 25  / */
    0xb1, /* 26 /  */
    0xc1, /* 27 PROFILE_LEVEL */
    0xd1, /* 28 CLOCK2_LEVEL */
    0xe1, /* 29 IPI_LEVEL */
    0xef, /* 30 POWER_LEVEL */
    0xff, /* 31 HIGH_LEVEL */
};

const KIRQL
HalVectorToIRQL[16] =
{
       0, /* 00 PASSIVE_LEVEL */
    0xff, /* 10 */
    0xff, /* 20 */
       1, /* 3D APC_LEVEL */
       2, /* 41 DISPATCH_LEVEL */
       4, /* 50 \ */
       5, /* 60  \ */
       6, /* 70  | */
       7, /* 80 DEVICE IRQL */
       8, /* 90  | */
       9, /* A0  / */
      10, /* B0 /  */
      27, /* C1 PROFILE_LEVEL */
      28, /* D1 CLOCK2_LEVEL */
      29, /* E1 IPI_LEVEL / EF POWER_LEVEL */
      31, /* FF HIGH_LEVEL */
};
#endif

/* PRIVATE FUNCTIONS **********************************************************/

FORCEINLINE
ULONG
IOApicRead(ULONG_PTR Base, UCHAR Register)
{
    ULONG_PTR Flags;
    ULONG Value;

    /*
     * The I/O APIC is addressed through an index/data register pair: the caller
     * writes the register number to IOREGSEL then accesses IOWIN.  That pair is
     * shared global state, so the select+access must be atomic against local
     * preemption - otherwise a higher-IRQL caller (e.g. an ISR/DPC masking a
     * line) can reselect the index between our select and our access, and we
     * read/program a DIFFERENT redirection entry.  The HAL's own callers happen
     * not to overlap, but a driver-level line mask legitimately can; guard the
     * sequence with a local interrupt hold rather than relying on that.
     */
    Flags = __readeflags();
    _disable();
    WRITE_REGISTER_ULONG((PULONG)(Base + IOAPIC_IOREGSEL), Register);
    Value = READ_REGISTER_ULONG((PULONG)(Base + IOAPIC_IOWIN));
    __writeeflags(Flags);
    return Value;
}

FORCEINLINE
VOID
IOApicWrite(ULONG_PTR Base, UCHAR Register, ULONG Value)
{
    ULONG_PTR Flags;

    /* Atomic select+write of the shared index/data pair; see IOApicRead. */
    Flags = __readeflags();
    _disable();
    WRITE_REGISTER_ULONG((PULONG)(Base + IOAPIC_IOREGSEL), Register);
    WRITE_REGISTER_ULONG((PULONG)(Base + IOAPIC_IOWIN), Value);
    __writeeflags(Flags);
}

/**
 * @brief
 * Finds the I/O APIC that serves a global system interrupt and the
 * redirection entry (pin) it uses for it.
 */
FORCEINLINE
BOOLEAN
HalpFindIoApicInput(
    _In_ ULONG Input,
    _Out_ PULONG_PTR Base,
    _Out_ PUCHAR Pin)
{
    ULONG i;

    for (i = 0; i < HalpIoApicCount; i++)
    {
        if ((Input >= HalpIoApics[i].InputBase) &&
            (Input < HalpIoApics[i].InputBase + HalpIoApics[i].InputCount))
        {
            *Base = HalpIoApics[i].Base;
            *Pin = (UCHAR)(Input - HalpIoApics[i].InputBase);
            return TRUE;
        }
    }

    return FALSE;
}

FORCEINLINE
VOID
ApicWriteIORedirectionEntry(
    ULONG Input,
    IOAPIC_REDIRECTION_REGISTER ReDirReg)
{
    ULONG_PTR Base;
    UCHAR Pin;

    if (!HalpFindIoApicInput(Input, &Base, &Pin))
    {
        ASSERT(FALSE);
        return;
    }

    HalpIoApicShadow[Input] = ReDirReg;
    IOApicWrite(Base, IOAPIC_REDTBL + 2 * Pin, ReDirReg.Long0);
    IOApicWrite(Base, IOAPIC_REDTBL + 2 * Pin + 1, ReDirReg.Long1);
}

FORCEINLINE
IOAPIC_REDIRECTION_REGISTER
ApicReadIORedirectionEntry(
    ULONG Input)
{
    IOAPIC_REDIRECTION_REGISTER ReDirReg;
    ULONG_PTR Base;
    UCHAR Pin;

    if (!HalpFindIoApicInput(Input, &Base, &Pin))
    {
        ASSERT(FALSE);
        ReDirReg.LongLong = 0;
        ReDirReg.Vector = APIC_FREE_VECTOR;
        ReDirReg.Mask = 1;
        return ReDirReg;
    }

    ReDirReg.Long0 = IOApicRead(Base, IOAPIC_REDTBL + 2 * Pin);
    ReDirReg.Long1 = IOApicRead(Base, IOAPIC_REDTBL + 2 * Pin + 1);

    return ReDirReg;
}

FORCEINLINE
VOID
ApicRequestSelfInterrupt(IN UCHAR Vector, UCHAR TriggerMode)
{
    ULONG Flags;
    APIC_INTERRUPT_COMMAND_REGISTER Icr;
    APIC_INTERRUPT_COMMAND_REGISTER IcrStatus;

    /*
     * The IRR registers are spaced 16 bytes apart and hold 32 status bits each.
     * Pre-compute the register and bit that match our vector.
     */
    ULONG VectorHigh = Vector / 32;
    ULONG VectorLow = Vector % 32;
    ULONG Irr = APIC_IRR + 0x10 * VectorHigh;
    ULONG IrrBit = 1UL << VectorLow;

    /* Setup the command register */
    Icr.LongLong = 0;
    Icr.Vector = Vector;
    Icr.MessageType = APIC_MT_Fixed;
    Icr.TriggerMode = TriggerMode;
    Icr.DestinationShortHand = APIC_DSH_Self;

    /* Disable interrupts so that we can change IRR without being interrupted */
    Flags = __readeflags();
    _disable();

    /* Wait for the APIC to be idle */
    do
    {
        IcrStatus.Long0 = ApicRead(APIC_ICR0);
    } while (IcrStatus.DeliveryStatus);

    /* Write high dword first, then low dword to send the interrupt */
    ApicWrite(APIC_ICR1, Icr.Long1);
    ApicWrite(APIC_ICR0, Icr.Long0);

    /* Wait until we see the interrupt request.
     * It will stay in requested state until we re-enable interrupts.
     */
    while (!(ApicRead(Irr) & IrrBit))
    {
        YieldProcessor();
    }

    /* Finally, restore the original interrupt state */
    if (Flags & EFLAGS_INTERRUPT_MASK)
    {
        _enable();
    }
}

FORCEINLINE
VOID
ApicSendEOI(void)
{
    ApicWrite(APIC_EOI, 0);
}

FORCEINLINE
KIRQL
ApicGetProcessorIrql(VOID)
{
    /* Read the TPR and convert it to an IRQL */
    return TprToIrql(ApicRead(APIC_PPR));
}

FORCEINLINE
KIRQL
ApicGetCurrentIrql(VOID)
{
#ifdef _M_AMD64
    return (KIRQL)__readcr8();
#elif defined(APIC_LAZY_IRQL)
    /* Return the field in the PCR */
    return (KIRQL)__readfsbyte(FIELD_OFFSET(KPCR, Irql));
#else
    /* Read the TPR and convert it to an IRQL */
    return TprToIrql(ApicRead(APIC_TPR));
#endif
}

FORCEINLINE
VOID
ApicSetIrql(KIRQL Irql)
{
#ifdef _M_AMD64
    __writecr8(Irql);
#elif defined(APIC_LAZY_IRQL)
    __writefsbyte(FIELD_OFFSET(KPCR, Irql), Irql);
#else
    /* Convert IRQL and write the TPR */
    ApicWrite(APIC_TPR, IrqlToTpr(Irql));
#endif
}
#define ApicRaiseIrql ApicSetIrql

#ifdef APIC_LAZY_IRQL
FORCEINLINE
VOID
ApicLowerIrql(KIRQL Irql)
{
    __writefsbyte(FIELD_OFFSET(KPCR, Irql), Irql);

    /* Is the new Irql lower than set in the TPR? */
    if (Irql < KeGetPcr()->IRR)
    {
        /* Save the new hard IRQL in the IRR field */
        KeGetPcr()->IRR = Irql;

        /* Need to lower it back */
        ApicWrite(APIC_TPR, IrqlToTpr(Irql));
    }
}
#else
#define ApicLowerIrql ApicSetIrql
#endif

UCHAR
FASTCALL
HalpIrqToVector(UCHAR Irq)
{
    /* Inputs no I/O APIC serves carry no vector */
    if (Irq >= HalpMaxGsi)
    {
        return APIC_FREE_VECTOR;
    }

    /*
     * Answer from our own record, NOT from the redirection register.
     *
     * The RTE is hardware: firmware can leave an input programmed, and other
     * agents (HalpSetVectorState, driver-supplied INTERRUPT_CONNECTION_DATA)
     * can program one without this HAL having allocated anything.  Treating it
     * as the allocation database made hardware state outrank our own, so an
     * input could report a vector that HalpVectorToIndex[] had never recorded.
     * Callers then either tripped the consistency assert in
     * HalpGetRootInterruptVector or - on a free build - were handed a vector
     * that HalEnableSystemInterrupt would later reject as "not in use",
     * failing every IoConnectInterrupt on that line.
     *
     * The real HAL never reverse-maps a vector at all: HalEnableInterrupt is
     * given the GSIV and resolves the I/O APIC input from the firmware tables
     * (HalpGetApicInti).  We cannot change the legacy HalEnableSystemInterrupt
     * signature here, so we keep an explicit software forward map instead - but
     * the principle is the same: allocation lives in software, and hardware is
     * only ever written from it.
     */
    return HalpGsivToVector[Irq];
}

KIRQL
FASTCALL
HalpVectorToIrql(UCHAR Vector)
{
    return TprToIrql(Vector);
}

UCHAR
FASTCALL
HalpVectorToIrq(UCHAR Vector)
{
    return HalpVectorToIndex[Vector];
}

VOID
NTAPI
HalpSendEOI(VOID)
{
    ApicSendEOI();
}

VOID
NTAPI
ApicInitializeLocalApic(ULONG Cpu)
{
    APIC_BASE_ADDRESS_REGISTER BaseRegister;
    APIC_SPURIOUS_INERRUPT_REGISTER SpIntRegister;
    LVT_REGISTER LvtEntry;

    /* Enable the APIC if it wasn't yet */
    BaseRegister.LongLong = __readmsr(MSR_APIC_BASE);
    BaseRegister.Enable = 1;
    BaseRegister.BootStrapCPUCore = (Cpu == 0);
    __writemsr(MSR_APIC_BASE, BaseRegister.LongLong);

    /* Set spurious vector and SoftwareEnable to 1 */
    SpIntRegister.Long = ApicRead(APIC_SIVR);
    SpIntRegister.Vector = APIC_SPURIOUS_VECTOR;
    SpIntRegister.SoftwareEnable = 1;
    SpIntRegister.FocusCPUCoreChecking = 0;
    ApicWrite(APIC_SIVR, SpIntRegister.Long);

    /* Read the version and save it globally */
    if (Cpu == 0) ApicVersion = ApicRead(APIC_VER);

    /* Set the mode to flat (max 8 CPUs supported!) */
    ApicWrite(APIC_DFR, APIC_DF_Flat);

    /* Set logical apic ID */
    ApicWrite(APIC_LDR, ApicLogicalId(Cpu) << 24);

    /* Set the spurious ISR */
    KeRegisterInterruptHandler(APIC_SPURIOUS_VECTOR, ApicSpuriousService);

    /* Create a template LVT */
    LvtEntry.Long = 0;
    LvtEntry.Vector = APIC_FREE_VECTOR;
    LvtEntry.MessageType = APIC_MT_Fixed;
    LvtEntry.DeliveryStatus = 0;
    LvtEntry.RemoteIRR = 0;
    LvtEntry.TriggerMode = APIC_TGM_Edge;
    LvtEntry.Mask = 1;
    LvtEntry.TimerMode = 0;

    /* Initialize and mask the LVTs this local APIC actually implements.  The
       count of LVT entries (minus one) is in the version register; writing a
       register past it - notably the AMD extended-interrupt LVTs at 0x500 -
       raises the "illegal register address" error (ESR bit 7) and, because the
       error LVT is unmasked, delivers a spurious error interrupt the moment
       interrupts are first enabled (an unhandled-vector #GP on QEMU/Intel). */
    {
        ULONG MaxLvt = (ApicRead(APIC_VER) >> 16) & 0xFF;   /* Max LVT Entry */

        /* Entry 0 = Timer, always present */
        ApicWrite(APIC_TMRLVTR, LvtEntry.Long);
        /* Entry 4 = Performance counter, entry 5 = Thermal (order per the SDM) */
        if (MaxLvt >= 4)
            ApicWrite(APIC_PCLVTR, LvtEntry.Long);
        if (MaxLvt >= 5)
            ApicWrite(APIC_THRMLVTR, LvtEntry.Long);
        /* The 0x500-range extended LVTs are AMD-only and are masked at reset;
           leave them untouched. */
    }

    /* LINT0 */
    LvtEntry.Vector = APIC_SPURIOUS_VECTOR;
    LvtEntry.MessageType = APIC_MT_ExtInt;
    ApicWrite(APIC_LINT0, LvtEntry.Long);

    /* Enable LINT1 (NMI) */
    LvtEntry.Mask = 0;
    LvtEntry.Vector = APIC_NMI_VECTOR;
    LvtEntry.MessageType = APIC_MT_NMI;
    LvtEntry.TriggerMode = APIC_TGM_Level;
    ApicWrite(APIC_LINT1, LvtEntry.Long);

    /* Enable error LVTR.  Give its vector a real IDT handler (the spurious
       service just acknowledges and returns) so a latched APIC error cannot
       fault into an unregistered gate, and clear any error the setup writes
       left behind before the first interrupt window opens. */
    LvtEntry.Vector = APIC_ERROR_VECTOR;
    LvtEntry.MessageType = APIC_MT_Fixed;
    ApicWrite(APIC_ERRLVTR, LvtEntry.Long);
    KeRegisterInterruptHandler(APIC_ERROR_VECTOR, ApicSpuriousService);
    ApicWrite(APIC_ESR, 0);

    /* Set the IRQL from the PCR */
    ApicSetIrql(KeGetPcr()->Irql);
#ifdef APIC_LAZY_IRQL
    /* Save the new hard IRQL in the IRR field */
    KeGetPcr()->IRR = KeGetPcr()->Irql;
#endif
}

UCHAR
NTAPI
HalpAllocateSystemInterrupt(
    _In_ UCHAR Irq,
    _In_ UCHAR Vector)
{
    IOAPIC_REDIRECTION_REGISTER ReDirReg;

    ASSERT(Irq < HalpMaxGsi);
    ASSERT(HalpVectorToIndex[Vector] == APIC_FREE_VECTOR);

    /* Setup a redirection entry */
    ReDirReg.Vector = Vector;
    ReDirReg.MessageType = APIC_MT_LowestPriority;
    ReDirReg.DestinationMode = APIC_DM_Logical;
    ReDirReg.DeliveryStatus = 0;
    ReDirReg.Polarity = 0;
    ReDirReg.RemoteIRR = 0;
    ReDirReg.TriggerMode = APIC_TGM_Edge;
    ReDirReg.Mask = 1;
    ReDirReg.Reserved = 0;
    ReDirReg.Destination = ApicRead(APIC_ID) >> 24;

    /* Initialize entry */
    ApicWriteIORedirectionEntry(Irq, ReDirReg);

    /* Record the pairing in BOTH directions - this is the allocation itself */
    HalpVectorToIndex[Vector] = Irq;
    HalpGsivToVector[Irq] = Vector;

    return Vector;
}

ULONG
NTAPI
HalpGetRootInterruptVector(
    _In_ ULONG BusInterruptLevel,
    _In_ ULONG BusInterruptVector,
    _Out_ PKIRQL OutIrql,
    _Out_ PKAFFINITY OutAffinity)
{
    UCHAR Vector;
    KIRQL Irql;

    /* No I/O APIC serves this input */
    if (BusInterruptLevel >= HalpMaxGsi)
    {
        /* Not an error path.  The ACPI root PDO advertises the whole block
         * of device IDT vectors it owns on an APIC HAL (halacpi.c:
         * HALP_DEVICE_VECTOR_FIRST 0x51, HALP_DEVICE_VECTOR_COUNT 110),
         * and IopTranslateDeviceResources runs every one of them through
         * HalGetInterruptVector precisely to learn that they are already
         * system vectors rather than bus lines - it takes the 0 return as
         * the answer it wanted.  Logging 110 warnings per boot for that
         * buried the real failures. */
        DPRINT("Interrupt input %lu is not routed through an I/O APIC\n", BusInterruptLevel);
        *OutAffinity = 0;
        *OutIrql = 0;
        return 0;
    }

    /* Get the vector currently registered */
    Vector = HalpIrqToVector(BusInterruptLevel);

    /*
     * A non-free answer now means WE allocated it: HalpIrqToVector reads the
     * software record, and both halves of that record are only ever written
     * together, so HalpVectorToIndex[Vector] == BusInterruptLevel holds by
     * construction.  Anything the firmware or another agent left in a
     * redirection register is invisible here, which is the point - a vector
     * this HAL never handed out must never be returned, or the caller's
     * IoConnectInterrupt -> HalEnableSystemInterrupt fails it as "not in use".
     *
     * Shared lines land here too: the second device on a PCI INTx line gets
     * back the SAME vector as the first, which is what sharing requires.
     */
    if (Vector != APIC_FREE_VECTOR)
    {
        NT_ASSERT(HalpVectorToIndex[Vector] == BusInterruptLevel);

        /* Calculate IRQL */
        *OutIrql = HalpVectorToIrql(Vector);
    }
    else
    {
        ULONG Offset;

        /* Outer loop to find alternative slots, when all IRQLs are in use */
        for (Offset = 0; Offset < 15; Offset++)
        {
            /* Loop allowed IRQL range */
            for (Irql = CLOCK_LEVEL - 1; Irql >= CMCI_LEVEL; Irql--)
            {
                /* Calculate the vactor */
                Vector = IrqlToTpr(Irql) + Offset;

                /* Check if the vector is free */
                if (HalpVectorToIrq(Vector) == APIC_FREE_VECTOR)
                {
                    /* Found one, allocate the interrupt */
                    Vector = HalpAllocateSystemInterrupt(BusInterruptLevel, Vector);
                    *OutIrql = Irql;
                    goto Exit;
                }
            }
        }

        DPRINT1("Failed to get an interrupt vector for IRQ %lu\n", BusInterruptLevel);
        *OutAffinity = 0;
        *OutIrql = 0;
        return 0;
    }

Exit:

    *OutAffinity = HalpDefaultInterruptAffinity;
    ASSERT(HalpDefaultInterruptAffinity);

    return Vector;
}

/**
 * @brief
 * Maps one I/O APIC page and records the inputs it serves.
 */
static
VOID
HalpMapIoApic(
    _In_ ULONG PhysicalBase,
    _In_ ULONG InputBase)
{
    PHARDWARE_PTE Pte;
    ULONG_PTR Base;
    ULONG Count;

    if (HalpIoApicCount >= HALP_MAX_IOAPICS)
    {
        DPRINT1("Too many I/O APICs, unit at %lx ignored\n", PhysicalBase);
        return;
    }

    /* Units get consecutive pages behind the historical address */
    Base = (ULONG_PTR)IOAPIC_BASE + HalpIoApicCount * PAGE_SIZE;
    Pte = HalAddressToPte(Base);
    Pte->PageFrameNumber = PhysicalBase / PAGE_SIZE;
    Pte->Valid = 1;
    Pte->Write = 1;
    Pte->Owner = 1;
    Pte->CacheDisable = 1;
    Pte->Global = 1;
    _ReadWriteBarrier();

    /* The version register carries the number of entries, minus one */
    Count = ((IOApicRead(Base, IOAPIC_VER) >> 16) & 0xFF) + 1;
    if (InputBase >= HALP_MAX_INPUTS)
    {
        DPRINT1("I/O APIC at %lx starts past the input space, ignored\n", PhysicalBase);
        return;
    }
    if (InputBase + Count > HALP_MAX_INPUTS)
    {
        Count = HALP_MAX_INPUTS - InputBase;
    }

    HalpIoApics[HalpIoApicCount].Base = Base;
    HalpIoApics[HalpIoApicCount].InputBase = InputBase;
    HalpIoApics[HalpIoApicCount].InputCount = Count;
    HalpIoApicCount++;

    if (InputBase + Count > HalpMaxGsi)
    {
        HalpMaxGsi = InputBase + Count;
    }
}

VOID
NTAPI
ApicInitializeIOApic(VOID)
{
    IOAPIC_REDIRECTION_REGISTER ReDirReg;
    ULONG Index, Vector, Input;

    /* Map every unit the firmware described, or the standard one when the
       tables did not name any */
    HalpIoApicCount = 0;
    HalpMaxGsi = 0;
    for (Index = 0; Index < HALP_APIC_INFO_TABLE_IOAPIC_NUMBER; Index++)
    {
        if (HalpApicInfoTable.IoApicPA[Index] != 0)
        {
            HalpMapIoApic(HalpApicInfoTable.IoApicPA[Index],
                          HalpApicInfoTable.IoApicIrqBase[Index]);
        }
    }
    if (HalpIoApicCount == 0)
    {
        HalpMapIoApic(IOAPIC_PHYS_BASE, 0);
    }

    /* Setup a redirection entry */
    ReDirReg.LongLong = 0;
    ReDirReg.Vector = APIC_FREE_VECTOR;
    ReDirReg.MessageType = APIC_MT_Fixed;
    ReDirReg.DestinationMode = APIC_DM_Physical;
    ReDirReg.TriggerMode = APIC_TGM_Edge;
    ReDirReg.Mask = 1;
    ReDirReg.Destination = ApicRead(APIC_ID) >> 24;

    /* Mask every input of every unit */
    for (Index = 0; Index < HalpIoApicCount; Index++)
    {
        for (Input = HalpIoApics[Index].InputBase;
             Input < HalpIoApics[Index].InputBase + HalpIoApics[Index].InputCount;
             Input++)
        {
            ApicWriteIORedirectionEntry(Input, ReDirReg);
        }
    }

    /* Init both halves of the allocation record: nothing is allocated yet */
    for (Vector = 0; Vector <= 255; Vector++)
    {
        HalpVectorToIndex[Vector] = APIC_FREE_VECTOR;
        HalpGsivToVector[Vector] = APIC_FREE_VECTOR;
    }

    /* Enable the timer interrupt (but keep it masked) */
    ReDirReg.Vector = APIC_CLOCK_VECTOR;
    ReDirReg.MessageType = APIC_MT_Fixed;
    ReDirReg.DestinationMode = APIC_DM_Physical;
    ReDirReg.TriggerMode = APIC_TGM_Level;
    ReDirReg.Mask = 1;
    ReDirReg.Destination = ApicRead(APIC_ID) >> 24;
    ApicWriteIORedirectionEntry(APIC_CLOCK_INDEX, ReDirReg);
}

VOID
NTAPI
HalpInitializePICs(IN BOOLEAN EnableInterrupts)
{
    ULONG_PTR EFlags;

    /* Save EFlags and disable interrupts */
    EFlags = __readeflags();
    _disable();

    /* Initialize and mask the PIC */
    HalpInitializeLegacyPICs();

    /* Initialize the I/O APIC */
    ApicInitializeIOApic();

    /* Manually reserve some vectors */
    HalpVectorToIndex[APC_VECTOR] = APIC_RESERVED_VECTOR;
    HalpVectorToIndex[DISPATCH_VECTOR] = APIC_RESERVED_VECTOR;
    HalpVectorToIndex[APIC_CLOCK_VECTOR] = 8;
    HalpGsivToVector[8] = APIC_CLOCK_VECTOR;
    HalpVectorToIndex[CLOCK_IPI_VECTOR] = APIC_RESERVED_VECTOR;
    HalpVectorToIndex[APIC_SPURIOUS_VECTOR] = APIC_RESERVED_VECTOR;

    /*
     * The local APIC delivers these itself - LVT entries and IPIs - so no I/O
     * APIC input serves them and HalpIrqToVector has nothing to record. Left
     * free, the lazy-IRQL deferral in HalBeginSystemInterrupt reads
     * APIC_FREE_VECTOR back for one of them and trips its own assertion that
     * a vector with no input must be reserved or a message vector. They are
     * reserved; say so. All four sit above the device vector pool
     * (HALP_DEVICE_VECTOR_FIRST 0x51 + HALP_DEVICE_VECTOR_COUNT 110), so
     * naming them here takes nothing away from device allocation.
     */
    HalpVectorToIndex[APIC_PROFILE_VECTOR] = APIC_RESERVED_VECTOR;
    HalpVectorToIndex[APIC_ERROR_VECTOR] = APIC_RESERVED_VECTOR;
    HalpVectorToIndex[APIC_IPI_VECTOR] = APIC_RESERVED_VECTOR;
    HalpVectorToIndex[APIC_NMI_VECTOR] = APIC_RESERVED_VECTOR;

    /* Set interrupt handlers in the IDT */
    KeRegisterInterruptHandler(APIC_CLOCK_VECTOR, HalpClockInterrupt);
    KeRegisterInterruptHandler(CLOCK_IPI_VECTOR, HalpClockIpi);
#ifndef _M_AMD64
    KeRegisterInterruptHandler(APC_VECTOR, HalpApcInterrupt);
    KeRegisterInterruptHandler(DISPATCH_VECTOR, HalpDispatchInterrupt);
#endif

    /* Register the vectors for APC and dispatch interrupts */
    HalpRegisterVector(IDT_INTERNAL, 0, APC_VECTOR, APC_LEVEL);
    HalpRegisterVector(IDT_INTERNAL, 0, DISPATCH_VECTOR, DISPATCH_LEVEL);

    /* Raise the task priority to the maximum across the moment we first open
       the interrupt flag.  The loader can leave the interrupt controllers in a
       state where the CPU is told an interrupt is pending that the local APIC
       then has no vector for; taken at IRQL 0 that phantom faults (an
       unexpected-trap #GP).  Blocking every priority while IF goes high lets
       the edge pass without an acknowledge, then dropping the task priority
       makes the APIC re-evaluate against its (empty) request register and drop
       the stale pending state instead of delivering it. */
    ApicWrite(APIC_TPR, 0xFF);

    /* Restore interrupt state */
    if (EnableInterrupts) EFlags |= EFLAGS_INTERRUPT_MASK;
    __writeeflags(EFlags);

    /* Re-lower the task priority to whatever the current IRQL implies. */
    ApicWrite(APIC_TPR, 0x00);
}


/* SOFTWARE INTERRUPT TRAPS ***************************************************/

#ifndef _M_AMD64
VOID
DECLSPEC_NORETURN
FASTCALL
HalpApcInterruptHandler(IN PKTRAP_FRAME TrapFrame)
{
    KPROCESSOR_MODE ProcessorMode;
    KIRQL OldIrql;
    ASSERT(ApicGetProcessorIrql() == APC_LEVEL);

   /* Enter trap */
    KiEnterInterruptTrap(TrapFrame);

#ifdef APIC_LAZY_IRQL
    if (!HalBeginSystemInterrupt(APC_LEVEL, APC_VECTOR, &OldIrql))
    {
        /* "Spurious" interrupt, exit the interrupt */
        KiEoiHelper(TrapFrame);
    }
#else
    /* Save the old IRQL */
    OldIrql = ApicGetCurrentIrql();
    ASSERT(OldIrql < APC_LEVEL);
#endif

    /* Raise to APC_LEVEL */
    ApicRaiseIrql(APC_LEVEL);

    /* End the interrupt */
    ApicSendEOI();

    /* Kernel or user APC? */
    if (KiUserTrap(TrapFrame)) ProcessorMode = UserMode;
    else if (TrapFrame->EFlags & EFLAGS_V86_MASK) ProcessorMode = UserMode;
    else ProcessorMode = KernelMode;

    /* Enable interrupts and call the kernel's APC interrupt handler */
    _enable();
    KiDeliverApc(ProcessorMode, NULL, TrapFrame);

    /* Disable interrupts */
    _disable();

    /* Restore the old IRQL */
    ApicLowerIrql(OldIrql);

    /* Exit the interrupt */
    KiEoiHelper(TrapFrame);
}

VOID
DECLSPEC_NORETURN
FASTCALL
HalpDispatchInterruptHandler(IN PKTRAP_FRAME TrapFrame)
{
    KIRQL OldIrql;
    ASSERT(ApicGetProcessorIrql() == DISPATCH_LEVEL);

   /* Enter trap */
    KiEnterInterruptTrap(TrapFrame);

#ifdef APIC_LAZY_IRQL
    if (!HalBeginSystemInterrupt(DISPATCH_LEVEL, DISPATCH_VECTOR, &OldIrql))
    {
        /* "Spurious" interrupt, exit the interrupt */
        KiEoiHelper(TrapFrame);
    }
#else
    /* Get the current IRQL */
    OldIrql = ApicGetCurrentIrql();
    ASSERT(OldIrql < DISPATCH_LEVEL);
#endif

    /* Raise to DISPATCH_LEVEL */
    ApicRaiseIrql(DISPATCH_LEVEL);

    /* End the interrupt */
    ApicSendEOI();

    /* Enable interrupts and call the kernel's DPC interrupt handler */
    _enable();
    KiDispatchInterrupt();
    _disable();

    /* Restore the old IRQL */
    ApicLowerIrql(OldIrql);

    /* Exit the interrupt */
    KiEoiHelper(TrapFrame);
}
#endif


/* SOFTWARE INTERRUPTS ********************************************************/


VOID
FASTCALL
HalRequestSoftwareInterrupt(IN KIRQL Irql)
{
    /* Convert irql to vector and request an interrupt */
    ApicRequestSelfInterrupt(IrqlToSoftVector(Irql), APIC_TGM_Edge);
}

VOID
FASTCALL
HalClearSoftwareInterrupt(
    IN KIRQL Irql)
{
    /* Nothing to do */
}


/* SYSTEM INTERRUPTS **********************************************************/

/**
 * @brief
 * Ask whoever owns interrupt routing which I/O APIC input a vector
 * belongs to, for a vector this HAL did not hand out itself.
 *
 * @param[in] Vector
 * The interrupt vector to resolve.
 *
 * @param[out] Input
 * Receives the global system interrupt the vector serves.
 *
 * @return
 * TRUE if the vector was resolved to an input, FALSE otherwise.
 *
 * @remarks
 * On a machine with an ACPI driver that owns the interrupt arbiter, that
 * driver allocates vectors itself and hands them straight to
 * IoConnectInterrupt, so the first this HAL hears of one is the request to
 * unmask it. It publishes the reverse mapping through the private dispatch
 * table for exactly this purpose. A vector belonging to a message interrupt
 * is reported as having no input, which is correct: it has none.
 *
 * The polarity comes back with it, which is better than the convention this
 * HAL would otherwise assume: the owner read it from the firmware tables,
 * where an override is free to describe a source that does not follow it.
 */
/* What the interrupt owner can say about a vector this HAL did not place */
typedef enum _HALP_VECTOR_INPUT
{
    HalpVectorInputUnknown = 0,   /* nobody claims it */
    HalpVectorInputResolved,      /* a real I/O APIC input, in *Input */
    HalpVectorInputMessage        /* a message: no input, nothing to unmask */
} HALP_VECTOR_INPUT;

static
HALP_VECTOR_INPUT
HalpResolveVectorInput(
    _In_ ULONG Vector,
    _Out_ PULONG Input,
    _Out_ PKINTERRUPT_POLARITY Polarity)
{
    NTSTATUS Status;
    ULONG Resolved;

    if (HalGetVectorInputOverride == NULL)
    {
        return HalpVectorInputUnknown;
    }

    Resolved = 0;
    *Polarity = InterruptPolarityUnknown;
    Status = HalGetVectorInputOverride(Vector,
                                       HalpDefaultInterruptAffinity,
                                       &Resolved,
                                       Polarity);

    /*
     * The owner answering "this vector is a message" is not the same as it not
     * knowing the vector. A message has no I/O APIC input by definition - the
     * device raises it by writing the local APIC - so there is nothing to
     * resolve and nothing to unmask, and treating that as a failure refuses to
     * connect an interrupt that was never going to need this table.
     */
    if (Status == STATUS_INVALID_PARAMETER)
    {
        return HalpVectorInputMessage;
    }
    if (!NT_SUCCESS(Status))
    {
        return HalpVectorInputUnknown;
    }

    /* An answer outside what the firmware described cannot be programmed */
    if (Resolved >= HalpMaxGsi)
    {
        DPRINT1("HalpResolveVectorInput: vector 0x%lx resolved to input %lu, "
                "past the %lu this machine has\n",
                Vector, Resolved, HalpMaxGsi);
        return HalpVectorInputUnknown;
    }

    *Input = Resolved;
    return HalpVectorInputResolved;
}

BOOLEAN
NTAPI
HalEnableSystemInterrupt(
    IN ULONG Vector,
    IN KIRQL Irql,
    IN KINTERRUPT_MODE InterruptMode)
{
    IOAPIC_REDIRECTION_REGISTER ReDirReg;
    KINTERRUPT_POLARITY ResolvedPolarity = InterruptPolarityUnknown;
    UCHAR Index;
    ASSERT(Irql <= HIGH_LEVEL);
    ASSERT((IrqlToTpr(Irql) & 0xF0) == (Vector & 0xF0));

    /* Get the irq for this vector */
    Index = HalpVectorToIndex[Vector];

    /* A message-signalled interrupt has no I/O APIC input to unmask: the
       device raises it by writing the local APIC directly */
    if (Index == APIC_MSI_VECTOR)
    {
        return TRUE;
    }

    /* Check if its valid */
    if (Index >= HalpMaxGsi)
    {
        ULONG Input;

        /*
         * Nothing was recorded for this vector, which on its own only means
         * this HAL did not allocate it. Ask whoever did before refusing: an
         * ACPI driver that owns the arbiter allocates vectors itself and the
         * first this HAL sees of one is right here.
         */
        HALP_VECTOR_INPUT Kind = HalpResolveVectorInput(Vector, &Input,
                                                        &ResolvedPolarity);

        if (Kind == HalpVectorInputMessage)
        {
            /* Nothing to unmask, and that is success rather than refusal */
            return TRUE;
        }
        if (Kind == HalpVectorInputUnknown)
        {
            /* Interrupt is not in use, or not routed through the I/O APIC */
            DPRINT1("HalEnableSystemInterrupt: vector 0x%lx irql %u - no input "
                    "(HalpVectorToIndex=%u, HalpMaxGsi=%lu)\n",
                    Vector, Irql, Index, HalpMaxGsi);
            return FALSE;
        }

        /*
         * The input is already spoken for by another vector. Handing this one
         * the same redirection entry would silently take the line away from
         * whatever is already using it.
         */
        if ((HalpGsivToVector[Input] != APIC_FREE_VECTOR) &&
            (HalpGsivToVector[Input] != Vector))
        {
            DPRINT1("HalEnableSystemInterrupt: vector 0x%lx wants input %lu, "
                    "which already carries vector 0x%x\n",
                    Vector, Input, HalpGsivToVector[Input]);
            return FALSE;
        }

        /* Record the pairing, so everything after this agrees on it */
        HalpAllocateSystemInterrupt((UCHAR)Input, (UCHAR)Vector);
        Index = (UCHAR)Input;
    }

    /* Read the redirection entry */
    ReDirReg = ApicReadIORedirectionEntry(Index);

    /* Check if the interrupt is already enabled */
    if (ReDirReg.Mask == FALSE)
    {
        /* If the vector matches, there is nothing more to do,
           otherwise something is wrong. */
        if (ReDirReg.Vector != Vector)
        {
            DPRINT1("HalEnableSystemInterrupt: vector 0x%lx irql %u input %u - "
                    "already unmasked carrying vector 0x%lx\n",
                    Vector, Irql, Index, (ULONG)ReDirReg.Vector);
        }
        return (ReDirReg.Vector == Vector);
    }

    /* Set up the redirection entry */
    ReDirReg.Vector = Vector;
    ReDirReg.MessageType = APIC_MT_Fixed;
    ReDirReg.DestinationMode = APIC_DM_Physical;
    ReDirReg.Destination = ApicRead(APIC_ID) >> 24;
    ReDirReg.TriggerMode = (InterruptMode == LevelSensitive) ?
        APIC_TGM_Level : APIC_TGM_Edge;

    /*
     * Polarity must be programmed too, not left at the active-high default
     * HalpAllocateSystemInterrupt wrote.  A level-triggered line that is
     * asserted low - which is every PCI INTx line - connects successfully but
     * then never fires if the entry says active high, because the pin is
     * already at its "inactive" level as far as the I/O APIC is concerned.
     *
     * The real HAL derives this per input from the MADT/ACPI-declared trigger
     * and polarity cached in HalpIntiInfo[] and applies it explicitly
     * (halmacpi HalpEnableSystemInterrupt: "if (!polarity) entryLow |= 0x2000",
     * i.e. active low; see VistaHal/interrupt/interrupt.c).  ReactOS keeps no
     * such per-input cache, so use the standard convention the firmware tables
     * follow: level-triggered sources are active low, edge-triggered ones are
     * active high.  That matches every ISA (edge/high) and PCI INTx
     * (level/low) source, which is what the MADT overrides describe.
     */
    ReDirReg.Polarity = (InterruptMode == LevelSensitive) ? 1 : 0;

    ReDirReg.Mask = FALSE;

    /* Write back the entry */
    ApicWriteIORedirectionEntry(Index, ReDirReg);

    return TRUE;
}

VOID
NTAPI
HalDisableSystemInterrupt(
    IN ULONG Vector,
    IN KIRQL Irql)
{
    IOAPIC_REDIRECTION_REGISTER ReDirReg;
    UCHAR Index;
    ASSERT(Irql <= HIGH_LEVEL);
    ASSERT(Vector < RTL_NUMBER_OF(HalpVectorToIndex));

    Index = HalpVectorToIndex[Vector];

    /* Only vectors routed through the I/O APIC have an entry to mask;
       message vectors are masked in the device by the bus driver */
    if (Index >= HalpMaxGsi)
    {
        return;
    }

    /* Read the redirection entry */
    ReDirReg = ApicReadIORedirectionEntry(Index);

    /* Mask it */
    ReDirReg.Mask = 1;

    /* Write it back */
    ApicWriteIORedirectionEntry(Index, ReDirReg);
}

BOOLEAN
NTAPI
HalBeginSystemInterrupt(
    IN KIRQL Irql,
    IN ULONG Vector,
    OUT PKIRQL OldIrql)
{
    KIRQL CurrentIrql;

    /* Get the current IRQL */
    CurrentIrql = ApicGetCurrentIrql();

#ifdef APIC_LAZY_IRQL
    /* Check if this interrupt is allowed */
    if (CurrentIrql >= Irql)
    {
        IOAPIC_REDIRECTION_REGISTER RedirReg;
        UCHAR Index;

        /* It is not, set the real Irql in the TPR! */
        ApicWrite(APIC_TPR, IrqlToTpr(CurrentIrql));

        /* Save the new hard IRQL in the IRR field */
        KeGetPcr()->IRR = CurrentIrql;

        /* End this interrupt */
        ApicSendEOI();

        /* Get the irq for this vector */
        Index = HalpVectorToIndex[Vector];

        /* Check if it's valid */
        if (Index < HalpMaxGsi)
        {
            /* Read the I/O redirection entry */
            RedirReg = ApicReadIORedirectionEntry(Index);

            /* Re-request the interrupt to be handled later */
            ApicRequestSelfInterrupt(Vector, (UCHAR)RedirReg.TriggerMode);
       }
       else
       {
            /*
             * No input serves this vector. Reserved and message vectors are
             * the expected cases, but a device vector that was released while
             * one of its interrupts was still latched in the local APIC reads
             * back APIC_FREE_VECTOR and lands here too - HalpSetVectorState
             * frees the previous vector when an input is re-pointed, and
             * HalpFreeMessageVectors does the same for a message block. That
             * is a race with hardware, not a broken invariant, so it cannot be
             * an assertion.
             *
             * The action below is right for every one of those cases: with no
             * redirection entry to read a trigger mode from, edge is the only
             * sound assumption, and a vector nobody claims any more is caught
             * by the kernel's unexpected-interrupt path once it is delivered.
             *
             * Say which vector it was, though - the state that produced it is
             * gone by the time anything else could look. Once per vector, so a
             * recurring one cannot flood the port from interrupt context.
             */
            if ((Index != APIC_RESERVED_VECTOR) && (Index != APIC_MSI_VECTOR))
            {
                static UCHAR ReportedVectors[256];

                if (ReportedVectors[Vector] == 0)
                {
                    ReportedVectors[Vector] = 1;
                    DPRINT1("Deferred vector 0x%02lx has no input (Index %u, HalpMaxGsi %lu)\n",
                            Vector,
                            Index,
                            HalpMaxGsi);
                }
            }

            /* Re-request the interrupt to be handled later */
            ApicRequestSelfInterrupt(Vector, APIC_TGM_Edge);
       }

        /* Pretend it was a spurious interrupt */
        return FALSE;
    }
#endif
    /* Save the current IRQL */
    *OldIrql = CurrentIrql;

    /* Set the new IRQL */
    ApicRaiseIrql(Irql);

    /* Turn on interrupts */
    _enable();

    /* Success */
    return TRUE;
}

VOID
NTAPI
HalEndSystemInterrupt(
    IN KIRQL OldIrql,
    IN PKTRAP_FRAME TrapFrame)
{
    /* Send an EOI */
    ApicSendEOI();

    /* Restore the old IRQL */
    ApicLowerIrql(OldIrql);
}


/* CONNECTION-DATA INTERRUPT CONTROL ******************************************/

/**
 * @brief
 * Routes an I/O APIC input to a vector and unmasks it.
 *
 * @param[in] Input
 * The I/O APIC input (global system interrupt) to program.
 *
 * @param[in] Vector
 * The IDT vector the input delivers.
 *
 * @param[in] Mode
 * Level or edge trigger.
 *
 * @param[in] Polarity
 * Requested line polarity. InterruptPolarityUnknown picks the usual
 * default: active low for level lines, active high for edge lines.
 *
 * @param[in] TargetProcessors
 * Processors the input may target. A single processor is addressed
 * physically, a set uses flat logical lowest-priority delivery.
 */
NTSTATUS
NTAPI
HalpProgramInterruptInput(
    _In_ ULONG Input,
    _In_ ULONG Vector,
    _In_ KINTERRUPT_MODE Mode,
    _In_ KINTERRUPT_POLARITY Polarity,
    _In_ KAFFINITY TargetProcessors)
{
    IOAPIC_REDIRECTION_REGISTER ReDirReg;
    BOOLEAN Logical;
    UCHAR Destination;
    NTSTATUS Status;

    if ((Input >= HalpMaxGsi) || (Vector > 0xFF))
    {
        return STATUS_INVALID_PARAMETER;
    }

    Status = HalpBuildInterruptDestination(TargetProcessors, &Logical, &Destination);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    ReDirReg = ApicReadIORedirectionEntry(Input);
    ReDirReg.Vector = Vector;
    ReDirReg.MessageType = Logical ? APIC_MT_LowestPriority : APIC_MT_Fixed;
    ReDirReg.DestinationMode = Logical ? APIC_DM_Logical : APIC_DM_Physical;
    ReDirReg.Destination = Destination;
    ReDirReg.TriggerMode = (Mode == LevelSensitive) ? APIC_TGM_Level : APIC_TGM_Edge;
    if (Polarity == InterruptPolarityUnknown)
    {
        ReDirReg.Polarity = (Mode == LevelSensitive) ? 1 : 0;
    }
    else
    {
        ReDirReg.Polarity = (Polarity == InterruptActiveLow) ? 1 : 0;
    }
    ReDirReg.Mask = 0;
    ApicWriteIORedirectionEntry(Input, ReDirReg);

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Returns the IRQL a device runs at when it interrupts on the given
 * IDT vector. On this HAL the IRQL is a property of the vector's
 * priority row.
 */
KIRQL
NTAPI
HalConvertDeviceIdtToIrql(
    _In_ ULONG IdtEntry)
{
    if (IdtEntry > 0xFF)
    {
        return PASSIVE_LEVEL;
    }

    return HalpVectorToIrql((UCHAR)IdtEntry);
}

/**
 * @brief
 * Connects the interrupt described by a single-element connection data
 * block. Message interrupts only need their vector marked as taken; a
 * controller input is routed to its vector and unmasked.
 */
NTSTATUS
NTAPI
HalEnableInterrupt(
    _In_ PINTERRUPT_CONNECTION_DATA ConnectionData)
{
    PINTERRUPT_VECTOR_DATA VectorData;
    ULONG Vector, Input;
    UCHAR Index;

    if ((ConnectionData == NULL) || (ConnectionData->Count != 1))
    {
        return STATUS_INVALID_PARAMETER;
    }

    VectorData = &ConnectionData->Vectors[0];
    Vector = VectorData->Vector;
    if (Vector > 0xFF)
    {
        return STATUS_INVALID_PARAMETER;
    }

    /* The IRQL is fixed by the vector row and cannot be overridden */
    if (VectorData->Irql != HalpVectorToIrql((UCHAR)Vector))
    {
        DPRINT1("Vector 0x%lx cannot run at IRQL %u\n", Vector, VectorData->Irql);
        return STATUS_INVALID_PARAMETER;
    }

    Index = HalpVectorToIndex[Vector];

    switch (VectorData->Type)
    {
        case InterruptTypeXapicMessage:
        case InterruptTypeHypertransport:
        case InterruptTypeMessageRequest:
        {
            /* Take a free vector for the message; a vector already carrying
               messages is fine, anything else belongs to someone else */
            if (Index == APIC_FREE_VECTOR)
            {
                HalpVectorToIndex[Vector] = APIC_MSI_VECTOR;
            }
            else if (Index != APIC_MSI_VECTOR)
            {
                return STATUS_INVALID_PARAMETER;
            }
            return STATUS_SUCCESS;
        }

        case InterruptTypeControllerInput:
        {
            Input = VectorData->ControllerInput.Gsiv;
            if (Input >= HalpMaxGsi)
            {
                return STATUS_INVALID_PARAMETER;
            }

            if (Index == APIC_FREE_VECTOR)
            {
                UCHAR Previous;

                /*
                 * This input is being (re)pointed at Vector.  Release whatever
                 * vector it carried before - unconditionally.  The old guard
                 * only released it when the reverse map already agreed, so a
                 * record that was skewed for any reason stayed behind and left
                 * two vectors claiming one input; the next lookup on that input
                 * then resolved to the stale one.
                 */
                Previous = HalpIrqToVector((UCHAR)Input);
                if (Previous != APIC_FREE_VECTOR)
                {
                    HalpVectorToIndex[Previous] = APIC_FREE_VECTOR;
                }
                HalpVectorToIndex[Vector] = (UCHAR)Input;
                HalpGsivToVector[Input] = (UCHAR)Vector;
            }
            else if (Index != Input)
            {
                return STATUS_INVALID_PARAMETER;
            }

            return HalpProgramInterruptInput(Input,
                                             Vector,
                                             VectorData->Mode,
                                             VectorData->Polarity,
                                             VectorData->TargetProcessors.Mask);
        }

        default:
            return STATUS_INVALID_PARAMETER;
    }
}

/**
 * @brief
 * Undoes HalEnableInterrupt. A controller input is masked again; message
 * vectors keep their reservation until their owner frees them.
 */
NTSTATUS
NTAPI
HalDisableInterrupt(
    _In_ PINTERRUPT_CONNECTION_DATA ConnectionData)
{
    PINTERRUPT_VECTOR_DATA VectorData;
    IOAPIC_REDIRECTION_REGISTER ReDirReg;
    ULONG Vector, Input;

    if ((ConnectionData == NULL) || (ConnectionData->Count != 1))
    {
        return STATUS_INVALID_PARAMETER;
    }

    VectorData = &ConnectionData->Vectors[0];
    Vector = VectorData->Vector;
    if (Vector > 0xFF)
    {
        return STATUS_INVALID_PARAMETER;
    }

    switch (VectorData->Type)
    {
        case InterruptTypeXapicMessage:
        case InterruptTypeHypertransport:
        case InterruptTypeMessageRequest:
            return STATUS_SUCCESS;

        case InterruptTypeControllerInput:
        {
            Input = VectorData->ControllerInput.Gsiv;
            if ((Input >= HalpMaxGsi) || (HalpVectorToIndex[Vector] != Input))
            {
                return STATUS_INVALID_PARAMETER;
            }

            ReDirReg = ApicReadIORedirectionEntry(Input);
            ReDirReg.Mask = 1;
            ApicWriteIORedirectionEntry(Input, ReDirReg);
            return STATUS_SUCCESS;
        }

        default:
            return STATUS_INVALID_PARAMETER;
    }
}

/**
 * @brief
 * Maps a vector back to the interrupt input that drives it. An installed
 * private-dispatch override (an ACPI driver owning the routing) is asked
 * first; otherwise the answer comes from this HAL's own vector table.
 */
NTSTATUS
NTAPI
HalGetVectorInput(
    _In_ ULONG Vector,
    _In_ KAFFINITY Affinity,
    _Out_ PULONG Input,
    _Out_ PKINTERRUPT_POLARITY Polarity)
{
    IOAPIC_REDIRECTION_REGISTER ReDirReg;
    UCHAR Index;

    if (HalGetVectorInputOverride != NULL)
    {
        return HalGetVectorInputOverride(Vector, Affinity, Input, Polarity);
    }

    if (Vector > 0xFF)
    {
        return STATUS_INVALID_PARAMETER;
    }

    Index = HalpVectorToIndex[Vector];
    if (Index >= HalpMaxGsi)
    {
        return STATUS_NOT_FOUND;
    }

    ReDirReg = ApicReadIORedirectionEntry(Index);
    *Input = Index;
    *Polarity = ReDirReg.Polarity ? InterruptActiveLow : InterruptActiveHigh;
    return STATUS_SUCCESS;
}

/* ACPI POWER-MANAGEMENT HOOKS ************************************************/

/**
 * @brief
 * Returns the version register of the I/O APIC that starts at the given
 * global system interrupt, with its number of inputs in the top byte, or
 * 0 when no I/O APIC starts there.
 */
ULONG
NTAPI
HalpGetInterruptControllerVersion(
    _In_ ULONG InterruptBase)
{
    ULONG Version, i;

    for (i = 0; i < HalpIoApicCount; i++)
    {
        if (HalpIoApics[i].InputBase == InterruptBase)
        {
            Version = IOApicRead(HalpIoApics[i].Base, IOAPIC_VER);
            return (Version & 0x00FFFFFF) | (HalpIoApics[i].InputCount << 24);
        }
    }

    return 0;
}

/**
 * @brief
 * Tells whether a global system interrupt is an input of the I/O APIC.
 */
BOOLEAN
NTAPI
HalpIsInterruptInputValid(
    _In_ ULONG Input)
{
    ULONG_PTR Base;
    UCHAR Pin;

    return HalpFindIoApicInput(Input, &Base, &Pin);
}

/**
 * @brief
 * Rewrites every I/O APIC redirection entry from the shadow copy kept
 * with each write.
 */
VOID
NTAPI
HalpRestoreInterruptController(VOID)
{
    ULONG_PTR Flags;
    ULONG Unit, Input;

    Flags = __readeflags();
    _disable();

    for (Unit = 0; Unit < HalpIoApicCount; Unit++)
    {
        for (Input = HalpIoApics[Unit].InputBase;
             Input < HalpIoApics[Unit].InputBase + HalpIoApics[Unit].InputCount;
             Input++)
        {
            ApicWriteIORedirectionEntry(Input, HalpIoApicShadow[Input]);
        }
    }

    __writeeflags(Flags);
}

/* IRQL MANAGEMENT ************************************************************/

#ifndef _M_AMD64
KIRQL
NTAPI
KeGetCurrentIrql(VOID)
{
    /* Read the current TPR and convert it to an IRQL */
    return ApicGetCurrentIrql();
}

VOID
FASTCALL
KfLowerIrql(
    IN KIRQL OldIrql)
{
#if DBG
    /* Validate correct lower */
    if (OldIrql > ApicGetCurrentIrql())
    {
        /* Crash system */
        KeBugCheck(IRQL_NOT_LESS_OR_EQUAL);
    }
#endif
    /* Set the new IRQL */
    ApicLowerIrql(OldIrql);
}

KIRQL
FASTCALL
KfRaiseIrql(
    IN KIRQL NewIrql)
{
    KIRQL OldIrql;

    /* Read the current IRQL */
    OldIrql = ApicGetCurrentIrql();
#if DBG
    /* Validate correct raise */
    if (OldIrql > NewIrql)
    {
        /* Crash system */
        KeBugCheck(IRQL_NOT_GREATER_OR_EQUAL);
    }
#endif
    /* Convert the new IRQL to a TPR value and write the register */
    ApicRaiseIrql(NewIrql);

    /* Return old IRQL */
    return OldIrql;
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

#endif /* !_M_AMD64 */

