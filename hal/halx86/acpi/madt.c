
/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Source File for MADT parses 
 * PROGRAMMERS:  Copyright 2021 Justin Miller <justinmiller100@gmail.com>
 */

/* INCLUDES *******************************************************************/

#include <hal.h>
#if 0
#include <acpi.h>
/* ACPI_BIOS_ERROR defined in acoutput.h and bugcodes.h */
#undef ACPI_BIOS_ERROR
#endif
#include <madt.h>
#include <smp.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

PROCESSOR_IDENTITY HalpStaticProcessorIdentity[TEMP_CPUMAX] = {{0}};
HALP_APIC_INFO_TABLE HalpApicInfoTable;
PPROCESSOR_IDENTITY HalpProcessorIdentity = NULL;
PACPI_TABLE_MADT MadtTable;
UINT32 PhysicalProcessorCount = 0;

/* FUNCTIONS ******************************************************************/

VOID
HalpParseApicTables(IN PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    PACPI_SUBTABLE_HEADER AcpiHeader;
    PACPI_MADT_LOCAL_APIC LocalApic;
    ULONG_PTR TableEnd;
    ULONG ValidProcessorCount;
    /* We only support legacy APIC for now, this will be updated in the future */
    HalpApicInfoTable.ApicMode = 0x10;
    MadtTable = HalAcpiGetTable(LoaderBlock, 'CIPA');

    AcpiHeader = (PACPI_SUBTABLE_HEADER)&MadtTable[1];
    TableEnd = (ULONG_PTR)MadtTable + MadtTable->Header.Length;

    HalpProcessorIdentity = HalpStaticProcessorIdentity;
    HalpStaticProcessorIdentity[0].ROSProcessorNumber = 1;

    while ((ULONG_PTR)AcpiHeader < TableEnd)
    {
        LocalApic = (PACPI_MADT_LOCAL_APIC)AcpiHeader;

        if (LocalApic->Header.Type == ACPI_MADT_TYPE_LOCAL_APIC &&
           LocalApic->Header.Length == sizeof(ACPI_MADT_LOCAL_APIC))
           {
               PhysicalProcessorCount++;
               HalpStaticProcessorIdentity[PhysicalProcessorCount].ROSProcessorNumber = PhysicalProcessorCount + 1;
           }

        if (!AcpiHeader->Length)
        {
            break;
        }

        AcpiHeader = (PACPI_SUBTABLE_HEADER)((ULONG_PTR)AcpiHeader + AcpiHeader->Length);
    }

    AcpiHeader = (PACPI_SUBTABLE_HEADER)&MadtTable[1];
    TableEnd = (ULONG_PTR)MadtTable + MadtTable->Header.Length;

    /* Parse the Major MADT tables now */
    for (ULONG i = 0; ((ULONG_PTR)AcpiHeader < TableEnd); )
    {
        if (AcpiHeader->Type == ACPI_MADT_TYPE_LOCAL_APIC &&
            AcpiHeader->Length == sizeof(ACPI_MADT_LOCAL_APIC))
        {
            LocalApic = (PACPI_MADT_LOCAL_APIC)AcpiHeader;
            ValidProcessorCount = HalpApicInfoTable.ProcessorCount;

            HalpProcessorIdentity[ValidProcessorCount].LapicId = LocalApic->Id;
            HalpProcessorIdentity[ValidProcessorCount].ProcessorId = LocalApic->ProcessorId;

            HalpApicInfoTable.ProcessorCount++;

            i++;

            AcpiHeader = (PACPI_SUBTABLE_HEADER)((ULONG_PTR)AcpiHeader + AcpiHeader->Length);
        }
        else
        {
            AcpiHeader = (PACPI_SUBTABLE_HEADER)((ULONG_PTR)AcpiHeader + 1);
        }
    }
}

VOID
HalpPrintApicTables()
{ 
    DPRINT1("HAL has detected a physical processor count of: %d\n", PhysicalProcessorCount);

    for (UINT32 i = 0; i < PhysicalProcessorCount; i++)
    {
        DPRINT1("Information about the following processor is for processors number: %d\n", i);
        DPRINT1("   The BSPCheck is set to: %X\n", HalpStaticProcessorIdentity[i].BSPCheck);
        DPRINT1("   The LapicID is set to: %X\n", HalpStaticProcessorIdentity[i].LapicId);
        DPRINT1("   The ProcessorID is set to: %X\n", HalpStaticProcessorIdentity[i].ProcessorId);
        DPRINT1("   The ProcessorStated check is set to: %X\n", HalpStaticProcessorIdentity[i].ProcessorStarted);
        DPRINT1("   The ROSProcessorNumber is set to: %X\n", HalpStaticProcessorIdentity[i].ROSProcessorNumber);
    }
}