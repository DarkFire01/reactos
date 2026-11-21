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

/* Power requests that carry input need the shutdown privilege enabled, the token has it off */
static
BOOL
PhEnableShutdownPrivilege(VOID)
{
    TOKEN_PRIVILEGES Privileges;
    HANDLE Token;
    BOOL Result;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &Token))
        return FALSE;

    Privileges.PrivilegeCount = 1;
    Privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    Result = LookupPrivilegeValueW(NULL, SE_SHUTDOWN_NAME, &Privileges.Privileges[0].Luid) &&
             AdjustTokenPrivileges(Token, FALSE, &Privileges, 0, NULL, NULL) &&
             GetLastError() == ERROR_SUCCESS;

    CloseHandle(Token);
    return Result;
}

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

    PhEnableShutdownPrivilege();
    Status = NtPowerInformation(PowerInformationInternal, &Request, sizeof(Request), NULL, 0);
    if (NT_SUCCESS(Status) && Status != STATUS_ALREADY_COMPLETE)
        DPRINT1("Woke the device from standby for %s\n", Reason);
    else if (!NT_SUCCESS(Status))
        DPRINT1("Waking the device for %s failed 0x%lx\n", Reason, Status);
}
