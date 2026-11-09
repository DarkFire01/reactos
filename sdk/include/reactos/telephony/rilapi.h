/*
 * PROJECT:     ReactOS Telephony
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     rilproxy.dll entry points used by ReactOS components
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#include <telephony/rilcmd.h>

#ifdef __cplusplus
extern "C" {
#endif

HRESULT
WINAPI
RIL_Initialize(
    _In_ DWORD Index,
    _In_opt_ PRIL_RESULT_CALLBACK ResultCallback,
    _In_opt_ PRIL_NOTIFY_CALLBACK NotifyCallback,
    _In_reads_opt_(ClassCount) const DWORD *NotificationClasses,
    _In_ DWORD ClassCount,
    _In_opt_ PVOID Param,
    _In_opt_ LPCWSTR ClientName,
    _Out_ HRIL *phRil);

HRESULT
WINAPI
RIL_Deinitialize(
    _In_ HRIL hRil);

HRESULT
WINAPI
RIL_GetNumberOfModems(
    _Out_ PDWORD Count);

HRESULT
WINAPI
RIL_EnableNotifications(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_reads_(Count) const DWORD *Classes,
    _In_ DWORD Count);

HRESULT
WINAPI
RIL_SetEquipmentState(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD State);

HRESULT
WINAPI
RIL_SetExecutorRFState(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Executor,
    _In_ DWORD State);

HRESULT
WINAPI
RIL_Dial(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Executor,
    _In_ const RILADDRESS *Address,
    _In_ DWORD Options,
    _In_ DWORD Domain,
    _In_opt_ const RIL_MEDIA_SET *Media);

HRESULT
WINAPI
RIL_ManageCalls_V2(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Executor,
    _In_ DWORD Action,
    _In_ DWORD CallId,
    _In_opt_ const RIL_MEDIA_SET *Media);

HRESULT
WINAPI
RIL_SendDTMF(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Executor,
    _In_opt_ LPCSTR Digits,
    _In_ DWORD OnMs,
    _In_ DWORD OffMs);

HRESULT
WINAPI
RIL_SendMsg(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Executor,
    _In_ DWORD Reserved,
    _In_reads_bytes_(sizeof(RIL_MESSAGE)) const VOID *Message,
    _In_ DWORD Options);

HRESULT
WINAPI
RIL_SendMsgAck(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Executor,
    _In_ DWORD Reserved,
    _In_ DWORD AckId,
    _In_ DWORD Status,
    _In_ DWORD Protocol,
    _In_ DWORD OverIms);

#ifdef __cplusplus
}
#endif
