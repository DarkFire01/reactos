/*
 * PROJECT:     ReactOS RIL Adaptation Service
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Private definitions
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>
#include <winreg.h>
#include <winuser.h>
#include <stdlib.h>
#include <winsvc.h>
#include <winioctl.h>
#include <setupapi.h>
#include <ndk/rtlfuncs.h>
#include <initguid.h>
#include <devguid.h>

#include <telephony/rilwire.h>
#include <telephony/rilcmd.h>

#define RILADAPT_SERVICE_NAME       L"RILAdaptation"
#define RILADAPT_PARAMETERS_KEY     L"SYSTEM\\CurrentControlSet\\Services\\RILAdaptation\\Parameters"
#define RILADAPT_MAX_MODEMS         2

/* Host interface versions offered to the OEM library */
#define RILADAPT_VERSION_MIN        1
#define RILADAPT_VERSION_MAX        2

typedef struct _RIL_MODEM_CONFIG
{
    WCHAR LibraryPath[MAX_PATH];
    DWORD InitIndex;
    WCHAR BrokerId[64];
    WCHAR BrokerInstance[16];
    WCHAR BrokerName[64];
} RIL_MODEM_CONFIG, *PRIL_MODEM_CONFIG;

typedef struct _RIL_HOST_COMMAND
{
    LIST_ENTRY Link;
    DWORD CommandId;
    DWORD Command;
    ULONGLONG Context;
} RIL_HOST_COMMAND, *PRIL_HOST_COMMAND;

struct _RIL_MODEM;

typedef struct _RIL_HOST_CLIENT
{
    LIST_ENTRY Link;
    struct _RIL_MODEM *Modem;
    DWORD ClientId;
    DWORD ProcessId;
    DWORD OemClient;
    HANDLE CancelEvent;
    HANDLE NotifyEvent;
    PTP_WAIT NotifyWait;
    PBYTE QueueBuffer;
    DWORD QueueBufferSize;

    CRITICAL_SECTION CommandLock;
    LIST_ENTRY Commands;
    DWORD NextCommandId;

    /* Notification codes the client asked for, guarded by CommandLock */
    PDWORD EnabledCodes;
    DWORD EnabledCount;
} RIL_HOST_CLIENT, *PRIL_HOST_CLIENT;

typedef struct _RIL_MODEM
{
    RIL_MODEM_CONFIG Config;
    HMODULE Library;
    PFN_RIL_VERSION RilVersion;
    PFN_RIL_INIT RilInit;
    PFN_RIL_OPEN RilOpen;
    PFN_RIL_CLOSE RilClose;
    PFN_RIL_IOCONTROL RilIoControl;
    DWORD Version;
    DWORD OemDriver;

    HANDLE Broker;
    CRITICAL_SECTION DeliverLock;
    HANDLE DeliverEvent;

    LIST_ENTRY Clients;
    HANDLE Thread;
} RIL_MODEM, *PRIL_MODEM;

/* config.c */
DWORD
RilFindModems(
    _Out_writes_to_(MaxModems, return) PRIL_MODEM_CONFIG Configs,
    _In_ DWORD MaxModems);

BOOL
RilEnsureBrokerDevice(
    _In_ PRIL_MODEM_CONFIG Config);

BOOL
RilGetBrokerHostPath(
    _In_ PRIL_MODEM_CONFIG Config,
    _Out_writes_(Chars) PWSTR Path,
    _In_ DWORD Chars);

/* host.c */
DWORD
RilStartModem(
    _Inout_ PRIL_MODEM Modem,
    _In_ HANDLE StopEvent);

VOID
RilStopModem(
    _Inout_ PRIL_MODEM Modem);
