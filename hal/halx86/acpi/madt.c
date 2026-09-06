/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Source File for MADT Table parsing
 * COPYRIGHT:   Copyright 2021 Justin Miller <justinmiller100@gmail.com>
 *              Copyright 2023 Serge Gautherie <reactos-git_serge_171003@gautherie.fr>
 */

/* INCLUDES *******************************************************************/

#include <hal.h>
#include <acpi.h>
/* ACPI_BIOS_ERROR defined in acoutput.h and bugcodes.h */
#undef ACPI_BIOS_ERROR
#include <smp.h>

#define NDEBUG
#include <debug.h>

// See HalpParseApicTables(). Only enable this to local-debug it.
// That needs, for example, to test-call the function later or to use the "FrLdrDbgPrint" hack.
#if DBG && 0
    #define DPRINT01        DPRINT1
    #define DPRINT00        DPRINT
#else
#if defined(_MSC_VER)
    #define DPRINT01        __noop
    #define DPRINT00        __noop
#else
    #define DPRINT01(...)   do { if(0) { DbgPrint(__VA_ARGS__); } } while(0)
    #define DPRINT00(...)   do { if(0) { DbgPrint(__VA_ARGS__); } } while(0)
#endif // _MSC_VER
#endif // DBG && 0

/*
 * What the parse below found, for HalpPrintApicTables to report once the debug
 * output is up.
 *
 * HalpParseApicTables runs too early to print anything - that is what the note
 * above is about - so every reason it can refuse a subtable is invisible at the
 * time, and it refuses easily. A machine whose firmware routes PCI interrupts
 * to global system interrupt 27 booted believing its only I/O APIC served
 * inputs 0..23, because ApicInitializeIOApic had fallen back to assuming a
 * single legacy unit at the historical address, and that fallback is
 * indistinguishable from a real unit afterwards. Recording it costs nothing
 * here and the print costs nothing later.
 */
typedef struct _HALP_MADT_RECORD
{
    ULONG Subtables;                /* subtables walked */
    ULONG TypeSeen[256 / 32];       /* one bit per subtable type encountered */
    ULONG IoApicSeen;               /* type 1 subtables encountered */
    ULONG IoApicBadLength;          /* ...refused for their size */
    ULONG IoApicOverflow;           /* ...refused, no room left */
    ULONG LocalApicBadLength;
    ULONG OverrideRefused;
    ULONG SkippedUnknown;
    ULONG StopReason;               /* HALP_MADT_STOP_* */
    ULONG StopType;                 /* subtable type the walk stopped on */
    ULONG StopLength;
} HALP_MADT_RECORD;

#define HALP_MADT_STOP_COMPLETE       0
#define HALP_MADT_STOP_NO_TABLE       1
#define HALP_MADT_STOP_TABLE_SHORT    2
#define HALP_MADT_STOP_SUBTABLE_SHORT 3
#define HALP_MADT_STOP_PAST_END       4
#define HALP_MADT_STOP_TRAILING       5

static HALP_MADT_RECORD HalpMadtRecord;

#if DBG
static PCSTR HalpMadtStopReason[] =
{
    "ran to the end of the table",
    "no MADT was found",
    "the MADT header was too short",
    "a subtable length was smaller than a subtable header",
    "a subtable ran past the end of the table",
    "the last subtable did not end on the end of the table"
};
#endif // DBG

/* GLOBALS ********************************************************************/

/* Defined by the APIC component; the tables parsed here fill it in */
extern HALP_APIC_INFO_TABLE HalpApicInfoTable;

// ACPI_MADT_LOCAL_APIC.LapicFlags masks
#define LAPIC_FLAG_ENABLED          0x00000001
#define LAPIC_FLAG_ONLINE_CAPABLE   0x00000002
// Bits 2-31 are reserved.

static PROCESSOR_IDENTITY HalpStaticProcessorIdentity[MAXIMUM_PROCESSORS];
const PPROCESSOR_IDENTITY HalpProcessorIdentity = HalpStaticProcessorIdentity;

/* ISA line to global interrupt mapping, filled from the overrides below */
extern ULONG HalpPicVectorRedirect[16];
extern ULONG HalpPicVectorFlags[16];

/* FUNCTIONS ******************************************************************/

// Note: HalpParseApicTables() is called early, so its DPRINT*() do nothing.
VOID
HalpParseApicTables(
    _In_ PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    ACPI_TABLE_MADT *MadtTable;
    ACPI_SUBTABLE_HEADER *AcpiHeader;
    ULONG_PTR TableEnd;

    MadtTable = HalAcpiGetTable(LoaderBlock, APIC_SIGNATURE);
    if (!MadtTable)
    {
        DPRINT01("MADT table not found\n");
        HalpMadtRecord.StopReason = HALP_MADT_STOP_NO_TABLE;
        return;
    }

    if (MadtTable->Header.Length < sizeof(*MadtTable))
    {
        DPRINT01("Length is too short: %p, %u\n", MadtTable, MadtTable->Header.Length);
        HalpMadtRecord.StopReason = HALP_MADT_STOP_TABLE_SHORT;
        return;
    }

    DPRINT00("MADT table: Address %08X, Flags %08X\n", MadtTable->Address, MadtTable->Flags);

#if 1

    // TODO: We support only legacy APIC for now
    HalpApicInfoTable.ApicMode = HALP_APIC_MODE_LEGACY;
    // TODO: What about 'MadtTable->Flags & ACPI_MADT_PCAT_COMPAT'?

#else // TODO: Is that correct?

    if ((MadtTable->Flags & ACPI_MADT_PCAT_COMPAT) == ACPI_MADT_DUAL_PIC)
    {
        HalpApicInfoTable.ApicMode = HALP_APIC_MODE_LEGACY;
    }
    else // if ((MadtTable->Flags & ACPI_MADT_PCAT_COMPAT) == ACPI_MADT_MULTIPLE_APIC)
    {
#if 1
        DPRINT01("ACPI_MADT_MULTIPLE_APIC support is UNIMPLEMENTED\n");
        return;
#else
        HalpApicInfoTable.ApicMode = HALP_APIC_MODE_xyz;
#endif
    }

#endif

    HalpApicInfoTable.LocalApicPA = MadtTable->Address;

    AcpiHeader = (ACPI_SUBTABLE_HEADER *)((ULONG_PTR)MadtTable + sizeof(*MadtTable));
    TableEnd = (ULONG_PTR)MadtTable + MadtTable->Header.Length;
    DPRINT00(" MadtTable %p, subtables %p - %p\n", MadtTable, AcpiHeader, (PVOID)TableEnd);

    while ((ULONG_PTR)(AcpiHeader + 1) <= TableEnd)
    {
        if (AcpiHeader->Length < sizeof(*AcpiHeader))
        {
            DPRINT01("Length is too short: %p, %u\n", AcpiHeader, AcpiHeader->Length);
            HalpMadtRecord.StopReason = HALP_MADT_STOP_SUBTABLE_SHORT;
            HalpMadtRecord.StopType = AcpiHeader->Type;
            HalpMadtRecord.StopLength = AcpiHeader->Length;
            return;
        }

        if ((ULONG_PTR)AcpiHeader + AcpiHeader->Length > TableEnd)
        {
            DPRINT01("Length mismatch: %p, %u, %p\n",
                     AcpiHeader, AcpiHeader->Length, (PVOID)TableEnd);
            HalpMadtRecord.StopReason = HALP_MADT_STOP_PAST_END;
            HalpMadtRecord.StopType = AcpiHeader->Type;
            HalpMadtRecord.StopLength = AcpiHeader->Length;
            return;
        }

        /* Note it was walked, and which type it was, for the later report */
        HalpMadtRecord.Subtables++;
        HalpMadtRecord.TypeSeen[AcpiHeader->Type >> 5] |=
            (1UL << (AcpiHeader->Type & 31));

        switch (AcpiHeader->Type)
        {
            case ACPI_MADT_TYPE_LOCAL_APIC:
            {
                ACPI_MADT_LOCAL_APIC *LocalApic = (ACPI_MADT_LOCAL_APIC *)AcpiHeader;

                if (AcpiHeader->Length != sizeof(*LocalApic))
                {
                    DPRINT01("Type/Length mismatch: %p, %u\n", AcpiHeader, AcpiHeader->Length);
                    HalpMadtRecord.LocalApicBadLength++;
                    break;
                }

                DPRINT00(" Local Apic, Processor %lu: ProcessorId %u, Id %u, LapicFlags %08X\n",
                         HalpApicInfoTable.ProcessorCount,
                         LocalApic->ProcessorId, LocalApic->Id, LocalApic->LapicFlags);

                if (!(LocalApic->LapicFlags & (LAPIC_FLAG_ONLINE_CAPABLE | LAPIC_FLAG_ENABLED)))
                {
                    DPRINT00("  Ignored: unusable\n");
                    break;
                }

                if (HalpApicInfoTable.ProcessorCount == _countof(HalpStaticProcessorIdentity))
                {
                    DPRINT00("  Skipped: array is full\n");
                    // We assume ignoring this processor is acceptable, until proven otherwise.
                    break;
                }

                // Note: ProcessorId and Id are not validated in any way (yet).
                HalpProcessorIdentity[HalpApicInfoTable.ProcessorCount].ProcessorId =
                    LocalApic->ProcessorId;
                HalpProcessorIdentity[HalpApicInfoTable.ProcessorCount].LapicId = LocalApic->Id;

                HalpApicInfoTable.ProcessorCount++;

                break;
            }
            case ACPI_MADT_TYPE_IO_APIC:
            {
                ACPI_MADT_IO_APIC *IoApic = (ACPI_MADT_IO_APIC *)AcpiHeader;

                /* Which units the firmware describes decides which
                   interrupts can be routed at all, so a unit refused here is
                   one the machine cannot use. Count both, and let
                   HalpPrintApicTables say so once printing works */
                HalpMadtRecord.IoApicSeen++;

                if (AcpiHeader->Length != sizeof(*IoApic))
                {
                    DPRINT01("Type/Length mismatch: %p, %u\n", AcpiHeader, AcpiHeader->Length);
                    HalpMadtRecord.IoApicBadLength++;
                    HalpMadtRecord.StopLength = AcpiHeader->Length;
                    break;
                }

                if (HalpApicInfoTable.IOAPICCount >= HALP_APIC_INFO_TABLE_IOAPIC_NUMBER)
                {
                    DPRINT01("Too many I/O APICs: %p\n", IoApic);
                    HalpMadtRecord.IoApicOverflow++;
                    break;
                }

                /*
                 * Store it at the next free slot, in the order the firmware
                 * described it.
                 *
                 * This used to index by IoApic->Id and refuse an Id it had
                 * already seen. Nothing makes that Id unique - it is the
                 * unit's APIC id, and firmware is free to leave every unit on
                 * zero - so a second unit was dropped, and with it every
                 * global system interrupt it served. A machine whose _PRT
                 * routes PCI interrupts to interrupt 27 then had no unit that
                 * could deliver them.
                 *
                 * The reference stores each unit at a running counter and
                 * keeps the Id only as data, which is what this does.
                 */
                // Note: Address and GlobalIrqBase are not validated in any way (yet).
                HalpApicInfoTable.IoApicId[HalpApicInfoTable.IOAPICCount] = IoApic->Id;
                HalpApicInfoTable.IoApicPA[HalpApicInfoTable.IOAPICCount] = IoApic->Address;
                HalpApicInfoTable.IoApicIrqBase[HalpApicInfoTable.IOAPICCount] = IoApic->GlobalIrqBase;

                HalpApicInfoTable.IOAPICCount++;

                break;
            }
            case ACPI_MADT_TYPE_INTERRUPT_OVERRIDE:
            {
                ACPI_MADT_INTERRUPT_OVERRIDE *InterruptOverride =
                    (ACPI_MADT_INTERRUPT_OVERRIDE *)AcpiHeader;

                if (AcpiHeader->Length != sizeof(*InterruptOverride))
                {
                    DPRINT01("Type/Length mismatch: %p, %u\n", AcpiHeader, AcpiHeader->Length);
                    HalpMadtRecord.OverrideRefused++;
                    break;
                }

                DPRINT00(" Interrupt Override: Bus %u, SourceIrq %u, GlobalIrq %08X, IntiFlags %04X\n",
                         InterruptOverride->Bus, InterruptOverride->SourceIrq,
                         InterruptOverride->GlobalIrq, InterruptOverride->IntiFlags);

                if (InterruptOverride->Bus != 0) // 0 = ISA
                {
                    DPRINT01("Invalid Bus: %p, %u\n", InterruptOverride, InterruptOverride->Bus);
                    HalpMadtRecord.OverrideRefused++;
                    break;
                }

                if (InterruptOverride->SourceIrq >= _countof(HalpPicVectorRedirect))
                {
                    DPRINT01("Invalid SourceIrq: %p, %u\n",
                             InterruptOverride, InterruptOverride->SourceIrq);
                    HalpMadtRecord.OverrideRefused++;
                    break;
                }

                /* The firmware wires this ISA line to another global interrupt,
                   possibly with a non-default polarity and trigger */
                HalpPicVectorRedirect[InterruptOverride->SourceIrq] = InterruptOverride->GlobalIrq;
                HalpPicVectorFlags[InterruptOverride->SourceIrq] = InterruptOverride->IntiFlags;

                break;
            }
            default:
            {
                /*
                 * Skip it - do not abandon the table.
                 *
                 * Returning here threw away every subtable behind the first one
                 * of a type this parser does not implement, and a modern MADT is
                 * full of them: Local APIC NMI (4), Local APIC Address Override
                 * (5), the x2APIC pair (9, 10). Anything the firmware listed
                 * after one of those was never seen - including further I/O
                 * APICs, which is how a machine whose _PRT routes PCI interrupts
                 * to global system interrupt 27 and up came up believing it had
                 * a single unit serving inputs 0..23, and then refused to
                 * connect every interrupt above that.
                 *
                 * Walking past an unknown subtable is exactly what its Length
                 * field is for, and the loop above has already checked that the
                 * Length keeps us inside the table.
                 */
                DPRINT01(" Skipped: Type %u, Length %u\n",
                         AcpiHeader->Type, AcpiHeader->Length);
                HalpMadtRecord.SkippedUnknown++;
                break;
            }
        }

        AcpiHeader = (ACPI_SUBTABLE_HEADER *)((ULONG_PTR)AcpiHeader + AcpiHeader->Length);
    }

    if ((ULONG_PTR)AcpiHeader != TableEnd)
    {
        DPRINT01("Length mismatch: %p, %p, %p\n", MadtTable, AcpiHeader, (PVOID)TableEnd);
        HalpMadtRecord.StopReason = HALP_MADT_STOP_TRAILING;
        return;
    }
}

VOID
HalpPrintApicTables(VOID)
{
#if DBG
    ULONG i;

    DPRINT1("Physical processor count: %lu\n", HalpApicInfoTable.ProcessorCount);
    for (i = 0; i < HalpApicInfoTable.ProcessorCount; i++)
    {
        DPRINT1(" Processor %lu: ProcessorId %u, LapicId %u, ProcessorStarted %u, BSPCheck %u, ProcessorPrcb %p\n",
                i,
                HalpProcessorIdentity[i].ProcessorId,
                HalpProcessorIdentity[i].LapicId,
                HalpProcessorIdentity[i].ProcessorStarted,
                HalpProcessorIdentity[i].BSPCheck,
                HalpProcessorIdentity[i].ProcessorPrcb);
    }

    /*
     * Now what the MADT walk actually did, recorded at the time because
     * nothing could be printed from there.
     */
    DPRINT1("MADT: %lu subtable(s), %s\n",
            HalpMadtRecord.Subtables,
            HalpMadtStopReason[HalpMadtRecord.StopReason]);

    if (HalpMadtRecord.StopReason != HALP_MADT_STOP_COMPLETE &&
        HalpMadtRecord.StopType != 0)
    {
        DPRINT1("MADT: stopped on a type %lu subtable of length %lu\n",
                HalpMadtRecord.StopType, HalpMadtRecord.StopLength);
    }

    /* Which subtable types the firmware listed. Types this parser does not
       implement are skipped, but knowing they were there is what says whether
       the walk reached the end of the firmware's list or gave up part way */
    for (i = 0; i < 256; i++)
    {
        if (HalpMadtRecord.TypeSeen[i >> 5] & (1UL << (i & 31)))
        {
            DPRINT1("MADT:   saw subtable type %lu\n", i);
        }
    }

    DPRINT1("MADT: %lu I/O APIC subtable(s), %lu recorded"
            " (%lu bad length, %lu no room)\n",
            HalpMadtRecord.IoApicSeen,
            HalpApicInfoTable.IOAPICCount,
            HalpMadtRecord.IoApicBadLength,
            HalpMadtRecord.IoApicOverflow);

    if (HalpMadtRecord.LocalApicBadLength != 0 ||
        HalpMadtRecord.OverrideRefused != 0 ||
        HalpMadtRecord.SkippedUnknown != 0)
    {
        DPRINT1("MADT: %lu local APIC(s) of bad length, %lu override(s) refused,"
                " %lu subtable(s) of unimplemented types skipped\n",
                HalpMadtRecord.LocalApicBadLength,
                HalpMadtRecord.OverrideRefused,
                HalpMadtRecord.SkippedUnknown);
    }

    /* The units themselves, in the order the firmware described them */
    for (i = 0; i < HalpApicInfoTable.IOAPICCount; i++)
    {
        DPRINT1("MADT:   I/O APIC %lu: Id %lu at %08X, global interrupt base %lu\n",
                i,
                HalpApicInfoTable.IoApicId[i],
                HalpApicInfoTable.IoApicPA[i],
                HalpApicInfoTable.IoApicIrqBase[i]);
    }

    if (HalpApicInfoTable.IOAPICCount == 0)
    {
        DPRINT1("MADT: no I/O APIC was recorded - the interrupt controller was "
                "assumed, not described\n");
    }
#endif
}
