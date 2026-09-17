/*
 * PROJECT:     ReactOS Kernel - Vista+ APIs
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Hvl (Hypervisor Library) functions of Windows 10+
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include "ntoskrnl_vista.h"

/* FUNCTIONS ******************************************************************/

/**
 * @brief
 * Tells whether the system runs underneath a hypervisor.
 *
 * @return
 * FALSE. ReactOS does not consume hypervisor enlightenments yet.
 */
BOOLEAN
NTAPI
HvlIsAnyHypervisorPresent(VOID)
{
    return FALSE;
}

/**
 * @brief
 * Returns the count of processors the hypervisor keeps active.
 *
 * @return
 * Zero, as no hypervisor is reported by HvlIsAnyHypervisorPresent().
 */
ULONG
NTAPI
HvlQueryActiveHypervisorProcessorCount(VOID)
{
    return 0;
}

/* EOF */
