/*
 * PROJECT:     ReactOS Native Time Service
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Shared definitions
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>
#include <winnls.h>
#include <winreg.h>
#include <winsvc.h>
#include <sddl.h>
#include <stdlib.h>
#include <ndk/rtlfuncs.h>
#include <strsafe.h>

#include <nativetime.h>
#include <telephony/rilcmd.h>
#include <telephony/rilapi.h>
#include <telephony/rilinfo.h>

#define NT_SERVICE_NAME             L"NativeTimeSvc"
#define NT_TRIGGERS_KEY             L"SYSTEM\\CurrentControlSet\\Services\\NativeTimeSvc\\Triggers"
#define NT_CLOCK_APP                L"clock.exe"

/* An alarm nobody answers stops ringing on its own after this long */
#define NT_RING_TIMEOUT_MS          (10 * 60 * 1000)

extern HANDLE NtStopEvent;

/* clock.c */
BOOL
NtClockStart(VOID);

VOID
NtClockStop(VOID);

VOID
NtClockGetStatus(
    _Out_ NATIVETIME_STATUS *Status);

/* triggers.c */
BOOL
NtTriggersStart(VOID);

VOID
NtTriggersStop(VOID);

VOID
NtTriggersTimeChanged(VOID);

DWORD
NtTriggersList(
    _Out_writes_(NATIVETIME_MAX_TRIGGERS) NATIVETIME_TRIGGER *Triggers);

DWORD
NtTriggersSet(
    _In_ const NATIVETIME_TRIGGER *Trigger,
    _Out_ PDWORD Id);

DWORD
NtTriggersDelete(
    _In_ DWORD Id);

DWORD
NtTriggersSnooze(
    _In_ DWORD Id,
    _In_ DWORD Minutes);

DWORD
NtTriggersDismiss(
    _In_ DWORD Id);

/* pipe.c */
BOOL
NtPipeStart(VOID);

VOID
NtPipeStop(VOID);

VOID
NtPipeBroadcast(
    _In_ DWORD Code,
    _In_opt_ const NATIVETIME_TRIGGER *Trigger);

BOOL
NtPipeHasSubscribers(VOID);

/* power.c */
VOID
NtWakeDevice(
    _In_z_ PCSTR Reason);

BOOL
NtEnablePrivilege(
    _In_z_ LPCWSTR Name);
