/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Processor Power Management Initialization Code
 * COPYRIGHT:   Copyright 2023 George Bișoc <george.bisoc@reactos.org>
 *              Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* PUBLIC FUNCTIONS ***********************************************************/

/**
 * @brief Initializes processor power management.
 *
 * @param[in] EarlyPhase
 * TRUE before the processor drivers load, FALSE once the power manager is up.
 * The late phase installs the default idle states, follows the power scheme
 * and starts the boot performance override.
 */
CODE_SEG("INIT")
NTSTATUS
NTAPI
PpmInitialize(
    _In_ BOOLEAN EarlyPhase)
{
    if (EarlyPhase)
    {
        PpmPolicyInitialize();
        PpmEngineInitialize();
        PpmIdleInitialize();
        PpmPerfInitialize();
        return STATUS_SUCCESS;
    }

    PpmInstallDefaultIdleStates();
    PpmRegisterPolicyCallbacks();
    PpmStartBootOverrideTimer();
    return STATUS_SUCCESS;
}

/* EOF */
