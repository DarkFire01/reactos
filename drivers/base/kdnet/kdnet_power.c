/*
 * PROJECT:     ReactOS Networking Debugging Module
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Standard power APIs for kdnet
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "kdnet.h"

NTSTATUS
NTAPI
KdD0Transition(VOID)
{

    if (KdNetInitialized &&
        KdNetExtensibilityExports &&
        KdNetExtensibilityExports->KdInitializeController)
    {
        (VOID)KdNetExtensibilityExports->KdInitializeController(&KdNetSharedData);
    }

    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
KdD3Transition(VOID)
{
    if (KdNetInitialized && KdNetExtensibilityExports)
    {
        if (KdNetSharedData.LinkState)
            *KdNetSharedData.LinkState = 0;

        if (KdNetExtensibilityExports->KdShutdownController && KdNetHardwareContext)
            KdNetExtensibilityExports->KdShutdownController(KdNetHardwareContext);
    }

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Restores the debug NIC after the debugger's state was saved.
 *
 * Nothing is restored here yet. The controller is brought back by
 * KdD0Transition, which runs the extension's KdInitializeController again, so
 * the link comes back either way. What is missing is carrying the NIC's own
 * register state across the transition, which would let the link resume
 * without a full reinitialization.
 *
 * @param[in] SleepTransition
 * TRUE when the save was for a sleep transition rather than a debugger detach.
 *
 * @return
 * STATUS_SUCCESS.
 */
NTSTATUS
NTAPI
KdRestore(
    IN BOOLEAN SleepTransition)
{
    UNREFERENCED_PARAMETER(SleepTransition);

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Saves the debug NIC's state before the debugger is torn down.
 *
 * The counterpart of KdRestore, and equally a placeholder: KdD3Transition
 * already stops the controller, so there is nothing that must be captured for
 * correctness, only for a faster resume.
 *
 * @param[in] SleepTransition
 * TRUE when the save is for a sleep transition rather than a debugger detach.
 *
 * @return
 * STATUS_SUCCESS.
 */
NTSTATUS
NTAPI
KdSave(
    IN BOOLEAN SleepTransition)
{
    UNREFERENCED_PARAMETER(SleepTransition);

    return STATUS_SUCCESS;
}
