/*
 * PROJECT:     ReactOS GNSS Service
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Shared definitions
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>
#include <winsvc.h>
#include <winioctl.h>
#include <sddl.h>
#include <stdlib.h>
#include <ndk/rtlfuncs.h>

#include <qcom/qmi.h>
#include <nativegnss.h>

#define GS_SERVICE_NAME             L"GnssSvc"

/* Largest QMI message sent or taken from the router */
#define GS_MESSAGE_MAX              4096

typedef struct _GS_ADDR
{
    ULONG Node;
    ULONG Port;
} GS_ADDR, *PGS_ADDR;

extern HANDLE GsStopEvent;

/* codec.c, declared in qcom/qmi.h */

/* router.c */
BOOL
GsRouterOpen(VOID);

VOID
GsRouterClose(VOID);

_Success_(return != FALSE)
BOOL
GsRouterFindServer(
    _In_ ULONG ServiceId,
    _In_ ULONG Instance,
    _Out_ PGS_ADDR Addr);

BOOL
GsRouterSend(
    _In_ const GS_ADDR *Dest,
    _In_reads_bytes_(Length) const VOID *Message,
    _In_ ULONG Length);

BOOL
GsRouterStartThreads(VOID);

VOID
GsRouterStopThreads(VOID);

/* loc.c, called from the router threads */
VOID
GsLocMessage(
    _In_ const GS_ADDR *Source,
    _In_reads_bytes_(Length) const VOID *Message,
    _In_ ULONG Length);

VOID
GsLocServerEvent(
    _In_ BOOL Up,
    _In_ ULONG ServiceId,
    _In_ ULONG Instance,
    _In_ const GS_ADDR *Addr);

/* loc.c */
BOOL
GsLocInit(VOID);

BOOL
GsLocStart(VOID);

VOID
GsLocStop(VOID);

VOID
GsLocDemandChanged(VOID);

VOID
GsLocGetFix(
    _Out_ PNATIVEGNSS_FIX Fix);

VOID
GsLocGetStatus(
    _Out_ PNATIVEGNSS_STATUS Status);

/* pipe.c */
BOOL
GsPipeStart(VOID);

VOID
GsPipeStop(VOID);

/* Shortest interval any interested client asked for, zero when none wants location */
DWORD
GsPipeDemand(
    _Out_ PDWORD Clients);

VOID
GsPipeBroadcast(
    _In_ DWORD Code,
    _In_ const NATIVEGNSS_STATUS *Status,
    _In_ const NATIVEGNSS_FIX *Fix);
