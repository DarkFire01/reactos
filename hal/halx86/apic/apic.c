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
 * Interrupt allocation tables:
 *   HalpVectorToIndex[Vector] is the I/O APIC input a vector is connected to
 *   HalpGsivToVector[Input] is the vector allocated to an I/O APIC input
 * Both tables are always updated together. Redirection entries are programmed
 * from them, and are never read back to find out what is allocated.
 */
UCHAR HalpVectorToIndex[256];
UCHAR HalpGsivToVector[256];

/* Filled in from the firmware tables */
extern HALP_APIC_INFO_TABLE HalpApicInfoTable;

/* I/O APICs in use, each one mapped at its own page starting at IOAPIC_BASE */
HALP_IOAPIC_UNIT HalpIoApics[HALP_MAX_IOAPICS];
ULONG HalpIoApicCount;

/* One past the highest global system interrupt served by an I/O APIC */
ULONG HalpMaxGsi;

/* Last value written to each redirection entry, used to restore the
   I/O APICs when they lose their state */
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

    /* IOREGSEL and IOWIN are a shared index/data pair. Keep interrupts
       disabled so an interrupt handler cannot change the selected register
       between the select and the read */
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

    /* Select and write with interrupts disabled, see IOApicRead */
    Flags = __readeflags();
    _disable();
    WRITE_REGISTER_ULONG((PULONG)(Base + IOAPIC_IOREGSEL), Register);
    WRITE_REGISTER_ULONG((PULONG)(Base + IOAPIC_IOWIN), Value);
    __writeeflags(Flags);
}

/**
 * @brief
 * Looks up the registers and the pin of the I/O APIC that serves
 * a global system interrupt.
 */
FORCEINLINE
BOOLEAN
HalpFindIoApicInput(
    _In_ ULONG Input,
    _Out_ PULONG_PTR Base,
    _Out_ PUCHAR Pin)
{
    PHALP_IOAPIC_UNIT Unit;
    ULONG Offset;

    for (Unit = HalpIoApics; Unit < HalpIoApics + HalpIoApicCount; Unit++)
    {
        /* An input below the base wraps around and fails the check */
        Offset = Input - Unit->InputBase;
        if (Offset < Unit->InputCount)
        {
            *Base = Unit->Base;
            *Pin = (UCHAR)Offset;
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
    /* No I/O APIC serves this input */
    if (Irq >= HalpMaxGsi)
    {
        return APIC_FREE_VECTOR;
    }

    /* Use the allocation table instead of the redirection entry. Firmware or
       a driver can leave an entry programmed with a vector this HAL never
       allocated, and HalEnableSystemInterrupt would reject that vector */
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
    ULONG MaxLvt;

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

    /* Initialize and mask only the LVTs this local APIC has. Writing an LVT
       past the maximum entry sets the illegal register bit in the ESR, which
       raises an error interrupt as soon as interrupts are enabled. The AMD
       extended LVTs are masked at reset and are not touched */
    MaxLvt = (ApicRead(APIC_VER) >> 16) & 0xFF;

    /* The timer LVT is always present */
    ApicWrite(APIC_TMRLVTR, LvtEntry.Long);

    /* Performance counter LVT */
    if (MaxLvt >= 4)
    {
        ApicWrite(APIC_PCLVTR, LvtEntry.Long);
    }

    /* Thermal sensor LVT */
    if (MaxLvt >= 5)
    {
        ApicWrite(APIC_THRMLVTR, LvtEntry.Long);
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

    /* Enable error LVTR. The error vector uses the spurious handler, which
       just returns, and any error left by the writes above is cleared */
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

    /* Record the allocation in both tables */
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
        /* This is expected. The PnP manager also translates resources that
           already hold system vectors and treats 0 as the result, so this
           is not reported as an error */
        DPRINT("Interrupt input %lu is not routed through an I/O APIC\n", BusInterruptLevel);
        *OutAffinity = 0;
        *OutIrql = 0;
        return 0;
    }

    /* Get the vector currently registered */
    Vector = HalpIrqToVector(BusInterruptLevel);

    /* Check if it's used. Devices sharing a line get the same vector */
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
                /* Profile level is not a device level, and on x86 several
                   device IRQLs share one priority. Each priority is tried once */
                if ((IrqlToTpr(Irql) >= IrqlToTpr(PROFILE_LEVEL)) ||
                    (IrqlToTpr(Irql) == IrqlToTpr(Irql + 1)))
                {
                    continue;
                }

                /* Calculate the vactor */
                Vector = IrqlToTpr(Irql) + Offset;

                /* Check if the vector is free */
                if (HalpVectorToIrq(Vector) == APIC_FREE_VECTOR)
                {
                    /* Found one, allocate the interrupt. The IRQL is the one
                       the vector's priority maps to, as later lookups report */
                    Vector = HalpAllocateSystemInterrupt(BusInterruptLevel, Vector);
                    *OutIrql = HalpVectorToIrql(Vector);
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
 * Maps the registers of an I/O APIC and adds it to the list of units.
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

    /* Each I/O APIC gets the next page after IOAPIC_BASE */
    Base = (ULONG_PTR)IOAPIC_BASE + HalpIoApicCount * PAGE_SIZE;
    Pte = HalAddressToPte(Base);
    Pte->PageFrameNumber = PhysicalBase / PAGE_SIZE;
    Pte->Valid = 1;
    Pte->Write = 1;
    Pte->Owner = 1;
    Pte->CacheDisable = 1;
    Pte->Global = 1;
    _ReadWriteBarrier();

    /* The version register holds the index of the last redirection entry */
    Count = ((IOApicRead(Base, IOAPIC_VER) >> 16) & 0xFF) + 1;
    if (InputBase >= HALP_MAX_INPUTS)
    {
        DPRINT1("I/O APIC at %lx has an input base out of range, ignored\n", PhysicalBase);
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

    /* Map the I/O APICs from the firmware tables, or the default one if the
       tables do not list any */
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

    /* Mask all inputs on all I/O APICs */
    for (Index = 0; Index < HalpIoApicCount; Index++)
    {
        for (Input = HalpIoApics[Index].InputBase;
             Input < HalpIoApics[Index].InputBase + HalpIoApics[Index].InputCount;
             Input++)
        {
            ApicWriteIORedirectionEntry(Input, ReDirReg);
        }
    }

    /* Init the allocation tables, nothing is allocated yet */
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

    /* These are delivered by the local APIC and have no I/O APIC input. They
       are above the vectors used for devices, so reserving them here does not
       take anything away from device allocation */
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

    /* Block all vectors in the task priority register while interrupts get
       enabled, so a stale interrupt left pending by the boot loader is not
       delivered at PASSIVE_LEVEL */
    ApicWrite(APIC_TPR, 0xFF);

    /* Restore interrupt state */
    if (EnableInterrupts) EFlags |= EFLAGS_INTERRUPT_MASK;
    __writeeflags(EFlags);

    /* Accept all vectors again */
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

/* Result of looking up the input of a vector this HAL did not allocate */
typedef enum _HALP_VECTOR_INPUT
{
    HalpVectorInputUnknown = 0,   /* the vector is not known */
    HalpVectorInputResolved,      /* the vector is connected to an I/O APIC input */
    HalpVectorInputMessage        /* message-signaled, there is no input */
} HALP_VECTOR_INPUT;

/**
 * @brief
 * Looks up the I/O APIC input of a vector that was not allocated by this HAL.
 *
 * @param[in] Vector
 * The interrupt vector to look up.
 *
 * @param[out] Input
 * Receives the global system interrupt the vector is connected to.
 *
 * @param[out] Polarity
 * Receives the polarity reported for the input.
 *
 * @return
 * HalpVectorInputResolved if Input was set, HalpVectorInputMessage for a
 * message-signaled vector, or HalpVectorInputUnknown otherwise.
 *
 * @remarks
 * When the ACPI driver handles interrupt arbitration it allocates vectors on
 * its own, so this HAL first sees such a vector when it is enabled. The driver
 * provides the vector to input mapping through the HAL private dispatch table.
 */
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

    /* STATUS_INVALID_PARAMETER is returned for a message-signaled vector.
       It has no I/O APIC input, so there is nothing to program */
    if (Status == STATUS_INVALID_PARAMETER)
    {
        return HalpVectorInputMessage;
    }
    if (!NT_SUCCESS(Status))
    {
        return HalpVectorInputUnknown;
    }

    /* The input must be served by one of the I/O APICs */
    if (Resolved >= HalpMaxGsi)
    {
        DPRINT1("HalpResolveVectorInput: vector 0x%lx has input %lu, "
                "which is out of range (HalpMaxGsi %lu)\n",
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

    /* Message-signaled interrupts have no I/O APIC input to unmask */
    if (Index == APIC_MSI_VECTOR)
    {
        return TRUE;
    }

    /* Check if its valid */
    if (Index >= HalpMaxGsi)
    {
        HALP_VECTOR_INPUT Kind;
        ULONG Input;

        /* This HAL did not allocate the vector, look up its input */
        Kind = HalpResolveVectorInput(Vector, &Input, &ResolvedPolarity);

        if (Kind == HalpVectorInputMessage)
        {
            /* Nothing to unmask */
            return TRUE;
        }
        if (Kind == HalpVectorInputUnknown)
        {
            /* Interrupt is not in use */
            DPRINT1("HalEnableSystemInterrupt: no input for vector 0x%lx, irql %u "
                    "(Index %u, HalpMaxGsi %lu)\n",
                    Vector, Irql, Index, HalpMaxGsi);
            return FALSE;
        }

        /* Don't take over an input that is allocated to another vector */
        if ((HalpGsivToVector[Input] != APIC_FREE_VECTOR) &&
            (HalpGsivToVector[Input] != Vector))
        {
            DPRINT1("HalEnableSystemInterrupt: input %lu for vector 0x%lx "
                    "is already used by vector 0x%x\n",
                    Input, Vector, HalpGsivToVector[Input]);
            return FALSE;
        }

        /* Record the allocation */
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
            DPRINT1("HalEnableSystemInterrupt: input %u is already enabled with "
                    "vector 0x%lx, requested vector 0x%lx, irql %u\n",
                    Index, (ULONG)ReDirReg.Vector, Vector, Irql);
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

    /* Level-triggered sources, like PCI INTx, are active low. Edge-triggered
       sources, like ISA IRQs, are active high. A level-triggered line left
       active high would never fire */
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

    /* Only vectors connected to an I/O APIC input have an entry to mask.
       Message-signaled interrupts are masked at the device by the bus driver */
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
            /* No I/O APIC input serves this vector. This is normal for reserved
               and message-signaled vectors, but it also happens when a device
               vector is freed while one of its interrupts is still pending.
               Report other vectors only once each, to avoid flooding the debug
               output from interrupt context */
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


/* INTERRUPT CONNECTION *******************************************************/

/**
 * @brief
 * Programs an I/O APIC input with a vector and unmasks it.
 *
 * @param[in] Input
 * Global system interrupt to program.
 *
 * @param[in] Vector
 * Vector delivered by the input.
 *
 * @param[in] Mode
 * Level or edge triggered.
 *
 * @param[in] Polarity
 * Polarity of the input. InterruptPolarityUnknown uses active low for level
 * and active high for edge.
 *
 * @param[in] TargetProcessors
 * Processors that receive the interrupt.
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

    if (Polarity == InterruptPolarityUnknown)
    {
        Polarity = (Mode == LevelSensitive) ? InterruptActiveLow : InterruptActiveHigh;
    }

    ReDirReg = ApicReadIORedirectionEntry(Input);
    ReDirReg.Vector = Vector;
    ReDirReg.Destination = Destination;
    if (Logical)
    {
        ReDirReg.MessageType = APIC_MT_LowestPriority;
        ReDirReg.DestinationMode = APIC_DM_Logical;
    }
    else
    {
        ReDirReg.MessageType = APIC_MT_Fixed;
        ReDirReg.DestinationMode = APIC_DM_Physical;
    }
    ReDirReg.TriggerMode = (Mode == LevelSensitive) ? APIC_TGM_Level : APIC_TGM_Edge;
    ReDirReg.Polarity = (Polarity == InterruptActiveLow) ? 1 : 0;
    ReDirReg.Mask = 0;
    ApicWriteIORedirectionEntry(Input, ReDirReg);

    return STATUS_SUCCESS;
}

/* Returns the IRQL of a device vector */
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
 * Enables the interrupt described by a connection data block with one vector.
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

    /* The IRQL is determined by the vector */
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
            /* The vector must be free or already used for messages */
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

                /* Free the vector previously allocated to this input */
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
 * Disables an interrupt enabled with HalEnableInterrupt. Message vectors
 * stay allocated until they are freed by their owner.
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
 * Returns the input and polarity of a vector. The private dispatch override
 * is used when one is installed.
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

/* ACPI POWER MANAGEMENT ******************************************************/

/**
 * @brief
 * Returns the version register of the I/O APIC whose first input is
 * InterruptBase, or 0 if there is none. The reserved high byte is replaced
 * with the number of inputs.
 */
ULONG
NTAPI
HalpGetInterruptControllerVersion(
    _In_ ULONG InterruptBase)
{
    PHALP_IOAPIC_UNIT Unit;
    ULONG Version;

    for (Unit = HalpIoApics; Unit < HalpIoApics + HalpIoApicCount; Unit++)
    {
        if (Unit->InputBase != InterruptBase)
        {
            continue;
        }

        Version = IOApicRead(Unit->Base, IOAPIC_VER);
        Version &= ~(0xFFUL << 24);
        Version |= Unit->InputCount << 24;
        return Version;
    }

    return 0;
}

BOOLEAN
NTAPI
HalpIsInterruptInputValid(
    _In_ ULONG Input)
{
    ULONG_PTR Base;
    UCHAR Pin;

    return HalpFindIoApicInput(Input, &Base, &Pin);
}

/* Writes all redirection entries back from HalpIoApicShadow */
VOID
NTAPI
HalpRestoreInterruptController(VOID)
{
    PHALP_IOAPIC_UNIT Unit;
    ULONG_PTR Flags;
    ULONG Input;

    Flags = __readeflags();
    _disable();

    for (Unit = HalpIoApics; Unit < HalpIoApics + HalpIoApicCount; Unit++)
    {
        for (Input = Unit->InputBase; Input < Unit->InputBase + Unit->InputCount; Input++)
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

