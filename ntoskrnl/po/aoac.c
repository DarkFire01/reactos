/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Power Manager Modern Standby support (Always On & Always Connected)
 * COPYRIGHT:   Copyright 2023 George Bișoc <george.bisoc@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#include <drivers/acpi/acpi.h>
#define NDEBUG
#include <debug.h>

/* The FADT flag a platform sets when it idles in S0 instead of sleeping, revision 5 and later */
#define POP_FADT_LOW_POWER_S0_IDLE_CAPABLE  0x00200000
#define POP_FADT_REVISION_LOW_POWER_S0      5

/* GLOBALS ********************************************************************/

BOOLEAN PopAoAcPresent = FALSE;

/* The console display is off and the platform idles in connected standby */
BOOLEAN PopAoAcInStandby = FALSE;

/* PRIVATE FUNCTIONS **********************************************************/

/**
 * @brief
 * Determines whether the platform is a Modern Standby one, by checking the
 * low power S0 idle capability the firmware declares in its FADT.
 *
 * @param[in] LoaderBlock
 * The loader parameter block the firmware tables came with.
 */
CODE_SEG("INIT")
VOID
NTAPI
PopInitializeAoAc(
    _In_ PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    PFADT Fadt;

    /* Only a HAL that knows its firmware tables can tell us */
    if (!KdGetAcpiTablePhase0)
        return;

    Fadt = KdGetAcpiTablePhase0(LoaderBlock, FADT_SIGNATURE);
    if (!Fadt ||
        Fadt->Header.Revision < POP_FADT_REVISION_LOW_POWER_S0 ||
        Fadt->Header.Length < RTL_SIZEOF_THROUGH_FIELD(FADT, flags))
    {
        return;
    }

    if (Fadt->flags & POP_FADT_LOW_POWER_S0_IDLE_CAPABLE)
    {
        PopAoAcPresent = TRUE;
        DPRINT1("The platform supports Modern Standby\n");
    }
}

/* PUBLIC FUNCTIONS ***********************************************************/

/**
 * @brief
 * Handles a power button sleep request on a Modern Standby platform. Instead
 * of entering a sleep state the console display goes off and the platform
 * stays in S0, the next press brings the display back.
 */
VOID
NTAPI
PopAoAcHandleSleepButton(VOID)
{
    WIN32_POWEREVENT_PARAMETERS Params;

    PAGED_CODE();

    PopAoAcInStandby = !PopAoAcInStandby;
    if (PopAoAcInStandby)
    {
        DPRINT1("Entering connected standby, turning the display off\n");
        Params.EventNumber = PsW32DisplayState;
        Params.Code = 0;
        PopDispatchPowerEvent(&Params);
        return;
    }

    DPRINT1("Leaving connected standby, turning the display on\n");
    Params.EventNumber = PsW32DisplayState;
    Params.Code = 1;
    PopDispatchPowerEvent(&Params);

    Params.EventNumber = PsW32FullWake;
    Params.Code = 0;
    PopDispatchPowerEvent(&Params);
}

/* EOF */
