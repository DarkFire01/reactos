/*
 * PROJECT:     ReactOS RIL Proxy
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Private definitions
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>
#include <winreg.h>
#include <ndk/rtlfuncs.h>
#include <cfgmgr32.h>
#include <initguid.h>

#include <telephony/rilwire.h>
#include <telephony/rilcmd.h>
#include <telephony/rilapi.h>

#define RIL_INITIAL_READ_SIZE       2048
#define RIL_CONNECT_RETRY_MS        1000

typedef struct _RIL_PENDING
{
    LIST_ENTRY Link;
    ULONGLONG Context;
    DWORD Command;
} RIL_PENDING, *PRIL_PENDING;

typedef struct _RIL_CLIENT
{
    DWORD Index;
    PRIL_RESULT_CALLBACK ResultCallback;
    PRIL_NOTIFY_CALLBACK NotifyCallback;
    PVOID Param;

    /* One writer at a time, the write OVERLAPPED is shared */
    CRITICAL_SECTION SendLock;

    /* Guards the device handle, the pending list and the state flags */
    CRITICAL_SECTION DeviceLock;
    HANDLE Device;
    BOOL Started;
    BOOL Initialized;
    BOOL ShuttingDown;
    LIST_ENTRY Pending;
    DWORD LastConnectError;

    /* Threadpool callbacks run one at a time */
    SRWLOCK CallbackLock;
    HCMNOTIFICATION InterfaceNotify;

    HANDLE InitDoneEvent;
    HANDLE ShutdownDoneEvent;

    HANDLE ConnectEvent;
    PTP_WAIT ConnectWait;
    PTP_TIMER RetryTimer;

    HANDLE DisconnectEvent;
    PTP_WAIT DisconnectWait;

    HANDLE WriteEvent;
    OVERLAPPED WriteOverlapped;

    HANDLE ReadEvent;
    PTP_WAIT ReadWait;
    OVERLAPPED ReadOverlapped;
    BOOL ReadOutstanding;
    PBYTE ReadBuffer;
    DWORD ReadBufferSize;
} RIL_CLIENT, *PRIL_CLIENT;

VOID
__cdecl
RilpTrace(
    _In_z_ _Printf_format_string_ PCSTR Format,
    ...);

PCSTR
RilpCommandName(
    _In_ DWORD Command);

HRESULT
RilpCreateClient(
    _In_ DWORD Index,
    _In_opt_ PRIL_RESULT_CALLBACK ResultCallback,
    _In_opt_ PRIL_NOTIFY_CALLBACK NotifyCallback,
    _In_opt_ PVOID Param,
    _Out_ PRIL_CLIENT *Client);

VOID
RilpDestroyClient(
    _In_ PRIL_CLIENT Client);

HRESULT
RilpSend(
    _In_ HRIL hRil,
    _In_ DWORD Command,
    _In_ ULONGLONG Context,
    _Inout_updates_bytes_(Size) PVOID Block,
    _In_ DWORD Size);

HRESULT
RilpSendHeaderOnly(
    _In_ HRIL hRil,
    _In_ DWORD Command,
    _In_ ULONGLONG Context);

HRESULT
RilpSendVariable(
    _In_ HRIL hRil,
    _In_ DWORD Command,
    _In_ ULONGLONG Context,
    _In_reads_bytes_opt_(PrefixSize) const VOID *Prefix,
    _In_ DWORD PrefixSize,
    _In_reads_bytes_opt_(DataSize) const VOID *Data,
    _In_ DWORD DataSize);
