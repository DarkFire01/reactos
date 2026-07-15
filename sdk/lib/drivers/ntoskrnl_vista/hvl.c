/*
 * PROJECT:     ReactOS Kernel - Vista+ APIs
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Hvl (Hypervisor Library) functions of Windows 10+
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "ntoskrnl_vista.h"

/**
 * @brief
 * Determines whether the system is running underneath a hypervisor.
 *
 * @return
 * TRUE if any hypervisor is present, FALSE otherwise.
 *
 * @remarks
 * ReactOS does not yet consume hypervisor enlightenments, so no hypervisor is
 * reported as present.
 */
BOOLEAN
NTAPI
HvlIsAnyHypervisorPresent(VOID)
{
    return FALSE;
}

/**
 * @brief
 * Returns the number of processors that are active under the hypervisor.
 *
 * @return
 * The count of hypervisor-managed active processors, or 0 when no hypervisor
 * is present.
 *
 * @remarks
 * As ReactOS reports no hypervisor (see HvlIsAnyHypervisorPresent()), this
 * routine always returns 0.
 */
ULONG
NTAPI
HvlQueryActiveHypervisorProcessorCount(VOID)
{
    return 0;
}
