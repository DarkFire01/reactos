/*
 * PROJECT:     ReactOS Desktop Window Manager
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Messages win32k sends to the DWM session port
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Window state changes, sent as kernel datagrams after DwmKernelStartup */
#define DWM_PORT_WINDOW_NOTIFICATION 0x40000100

typedef struct _DWM_WINDOW_NOTIFICATION
{
    PORT_MESSAGE Header;
    ULONG Command;
    ULONG Event;
    ULONG64 Window;
    ULONG64 InsertAfter;
    RECT WindowRect;
    RECT ClientRect;
    ULONG Style;
    ULONG ExStyle;
    ULONG ProcessId;
    ULONG Reserved;
} DWM_WINDOW_NOTIFICATION, *PDWM_WINDOW_NOTIFICATION;

#ifdef __cplusplus
}
#endif
