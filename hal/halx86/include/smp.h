  
/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * FILE:        hal/halx86/include/smp.h
 * PROGRAMMERS:  Copyright 2021 Justin Miller <justinmiller100@gmail.com>
 */

#pragma once

/* This table is filled for each physical processor on system */
typedef struct _PROCESSOR_IDENTITY
{
    UCHAR ProcessorId;
    UCHAR LapicId;
    UCHAR ROSProcessorNumber;
    BOOLEAN ProcessorStarted;
    BOOLEAN BSPCheck;

} PROCESSOR_IDENTITY, *PPROCESSOR_IDENTITY;

/* This table is counter of the overall APIC constants acquired from madt */
typedef struct _HALP_APIC_INFO_TABLE
{
    ULONG ApicMode;
    ULONG ProcessorCount; /* Count of all physical cores, This includes BSP */
    ULONG IOAPICCount;
    ULONG LocalApicPA;                // The 32-bit physical address at which each processor can access its local interrupt controller
    ULONG IoApicVA[256];
    ULONG IoApicPA[256];
    ULONG IoApicIrqBase[256]; // Global system interrupt base 

} HALP_APIC_INFO_TABLE, *PHALP_APIC_INFO_TABLE;

VOID
HalpParseApicTables(
    IN PLOADER_PARAMETER_BLOCK LoaderBlock
);

VOID
HalpPrintApicTables();

VOID
ApicStartApplicationProcessor(ULONG NTProcessorNumber, PHYSICAL_ADDRESS StartupLoc);

VOID
NTAPI
HalpRequestIpi(KAFFINITY TargetProcessors);
