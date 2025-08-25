/*
 * PROJECT:     Xbox HAL
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Initialize the x86 HAL
 * COPYRIGHT:   Copyright 1998 David Welch (welch@cwcom.net)
 */

/* INCLUDES *****************************************************************/

#include "halxbox.h"

#define NDEBUG
#include <debug.h>

/* FUNCTIONS ****************************************************************/

VOID
NTAPI
HalpInitProcessor(
    IN ULONG ProcessorNumber,
    IN PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    /* Set default IDR */
    KeGetPcr()->IDR = 0xFFFFFFFF & ~(1 << PIC_CASCADE_IRQ);
}

VOID
HalpInitPhase0(IN PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    /* Initialize Xbox-specific disk hacks */
    HalpXboxInitPartIo();
}
NTSTATUS
NTAPI
HalpGetInterruptTranslator(IN INTERFACE_TYPE ParentInterfaceType,
                           IN ULONG ParentBusNumber,
                           IN INTERFACE_TYPE BridgeInterfaceType,
                           IN USHORT Size,
                           IN USHORT Version,
                           OUT PTRANSLATOR_INTERFACE Translator,
                           OUT PULONG BridgeBusNumber)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(ParentInterfaceType);
    UNREFERENCED_PARAMETER(ParentBusNumber);
    UNREFERENCED_PARAMETER(BridgeBusNumber);
    return STATUS_NOT_IMPLEMENTED;
}
VOID
HalpInitPhase1(VOID)
{
    /* Enable timer interrupt handler */
    HalpEnableInterruptHandler(IDT_DEVICE,
                               0,
                               PRIMARY_VECTOR_BASE + PIC_TIMER_IRQ,
                               CLOCK2_LEVEL,
                               HalpClockInterrupt,
                               Latched);

    /* Enable RTC interrupt handler */
    HalpEnableInterruptHandler(IDT_DEVICE,
                               0,
                               PRIMARY_VECTOR_BASE + PIC_RTC_IRQ,
                               PROFILE_LEVEL,
                               HalpProfileInterrupt,
                               Latched);
}

/* EOF */
