/*
 * PROJECT:     ReactOS Phone Service
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Waking the device from connected standby on incoming calls and messages
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "phonesvc.h"
#include <ndk/potypes.h>
#include <ndk/pofuncs.h>

#define NDEBUG
#include <debug.h>

/**
 * @brief
 * Brings the display back on as if the power button had been pressed.
 * Nothing happens when the display is on already.
 *
 * @param[in] Reason
 * What woke the device, for the log.
 */
VOID
PhWakeDevice(
    _In_z_ PCSTR Reason)
{
    POWER_ACTIVATOR_REQUEST Request;
    NTSTATUS Status;

    ZeroMemory(&Request, sizeof(Request));
    Request.Version = POWER_ACTIVATOR_REQUEST_VERSION;
    Request.Action = PowerActivatorWake;

    Status = NtPowerInformation(PowerInformationInternal, &Request, sizeof(Request), NULL, 0);
    if (NT_SUCCESS(Status) && Status != STATUS_ALREADY_COMPLETE)
        DPRINT1("Woke the device from standby for %s\n", Reason);
    else if (!NT_SUCCESS(Status))
        DPRINT1("Waking the device for %s failed 0x%lx\n", Reason, Status);
}
