  
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

VOID
HalpParseApicTables(
    IN PLOADER_PARAMETER_BLOCK LoaderBlock
);
