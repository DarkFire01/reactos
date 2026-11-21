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

/* Defaults of the WatchdogTimeout and MessageHoldTimeout parameters, in seconds */
#define RILADAPT_WATCHDOG_DEFAULT   10
#define RILADAPT_HOLD_DEFAULT       30

/* SendMsgAck keys remembered for duplicate suppression */
#define RILADAPT_ACK_HISTORY        10

typedef struct _RIL_MODEM_CONFIG
{
    WCHAR LibraryPath[MAX_PATH];
    DWORD InitIndex;
    WCHAR BrokerId[64];
    WCHAR BrokerInstance[16];
    WCHAR BrokerName[64];
    DWORD WatchdogTimeout;
    DWORD MessageHoldTimeout;
} RIL_MODEM_CONFIG, *PRIL_MODEM_CONFIG;

/* A command the library accepted and will answer through its notification queue */
typedef struct _RIL_HOST_COMMAND
{
    LIST_ENTRY Link;
    DWORD CommandId;
    DWORD ClientId;
    DWORD Command;
    ULONGLONG Context;
} RIL_HOST_COMMAND, *PRIL_HOST_COMMAND;

/* A notification kept back until the hold timer releases it */
typedef struct _RIL_HELD_NOTIFICATION
{
    LIST_ENTRY Link;
    DWORD Code;
    DWORD DataSize;
    BYTE Data[ANYSIZE_ARRAY];
} RIL_HELD_NOTIFICATION, *PRIL_HELD_NOTIFICATION;

typedef struct _RIL_MODEM
{
    RIL_MODEM_CONFIG Config;
    DWORD Index;
    HMODULE Library;
    PFN_RIL_VERSION RilVersion;
    PFN_RIL_INIT RilInit;
    PFN_RIL_OPEN RilOpen;
    PFN_RIL_CLOSE RilClose;
    PFN_RIL_IOCONTROL RilIoControl;
    DWORD Version;
    DWORD OemDriver;
    DWORD OemClient;

    HANDLE Broker;
    CRITICAL_SECTION DeliverLock;
    HANDLE DeliverEvent;
    HANDLE BrokerThread;
    BOOL Stopping;

    /* Guards the command list and the acknowledgement history */
    CRITICAL_SECTION CommandLock;
    LIST_ENTRY Commands;
    DWORD NextCommandId;
    ULONGLONG AckKeys[RILADAPT_ACK_HISTORY];
    BOOL AckValid[RILADAPT_ACK_HISTORY];
    DWORD AckNext;

    /* Notification thread state */
    HANDLE StopEvent;
    HANDLE CancelEvent;
    HANDLE CancelHandle;
    HANDLE NotifyEvent;
    HANDLE NotifyThread;
    PBYTE QueueBuffer;
    DWORD QueueBufferSize;
    HANDLE HoldTimer;
    BOOL HoldReleased;
    LIST_ENTRY Held;

    PTP_TIMER CommandWatchdog;
    PTP_TIMER FetchWatchdog;
} RIL_MODEM, *PRIL_MODEM;

/* main.c */
VOID
__cdecl
RilLog(
    _In_z_ _Printf_format_string_ PCSTR Format,
    ...);

DECLSPEC_NORETURN
VOID
__cdecl
RilFatal(
    _In_z_ _Printf_format_string_ PCSTR Format,
    ...);

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
VOID
RilStartModem(
    _Inout_ PRIL_MODEM Modem);

VOID
RilStopModem(
    _Inout_ PRIL_MODEM Modem);
