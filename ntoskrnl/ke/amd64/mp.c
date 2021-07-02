/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Architecture specific source file to hold multiprocessor functions
 * COPYRIGHT:   Copyright 2022 Justin Miller <justinmiller100@gmail.com>
 */

/* INCLUDES *****************************************************************/

#include <ntoskrnl.h>
// #define NDEBUG
#include <debug.h>

typedef struct _APINFO
{
    KIPCR Pcr;
    ETHREAD Thread;
    DECLSPEC_ALIGN(PAGE_SIZE) KGDTENTRY Gdt[128];
    DECLSPEC_ALIGN(PAGE_SIZE) KIDTENTRY Idt[256];
    KTSS Tss;
    KTSS TssDoubleFault;
    KTSS TssNMI;
    DECLSPEC_ALIGN(16) UINT8 NMIStackData[DOUBLE_FAULT_STACK_SIZE];
} APINFO, *PAPINFO;

/* GLOBALS *******************************************************************/

extern PLOADER_PARAMETER_BLOCK KeLoaderBlock;

/* FUNCTIONS *****************************************************************/

CODE_SEG("INIT")
VOID
NTAPI
KeStartAllProcessors()
{
    UNIMPLEMENTED;
}
