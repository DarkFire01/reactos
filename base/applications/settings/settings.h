/*
 * PROJECT:     ReactOS Settings
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Shared layout, theme and helpers of the settings pages
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>
#include <winuser.h>
#include <wingdi.h>
#include <stdlib.h>
#include <winreg.h>
#include <winnls.h>
#include <strsafe.h>
#include <wchar.h>

#define WM_WIFI_DONE                (WM_APP + 1)
#define WM_NET_DONE                 (WM_APP + 2)
#define WM_SND_DONE                 (WM_APP + 3)
#define ST_REFRESH_TIMER            1
#define ST_REFRESH_MS               5000

/* Dark theme shared with the mobile shell */
#define ST_BG                       RGB(0x12, 0x12, 0x14)
#define ST_TILE                     RGB(0x24, 0x25, 0x2A)
#define ST_ACCENT                   RGB(0x3A, 0x96, 0xDD)
#define ST_TEXT                     RGB(0xF2, 0xF2, 0xF4)
#define ST_SUBTEXT                  RGB(0x9A, 0x9B, 0xA3)
#define ST_FIELD                    RGB(0x1A, 0x1A, 0x1E)
#define ST_DIM                      RGB(0x3A, 0x3B, 0x42)

/* Touch sized layout in pixels, the top row stays clear of the camera cutout */
#define ST_MARGIN                   28
#define ST_SAFE_TOP                 120
#define ST_GAP                      16
#define ST_ROW                      88
#define ST_PILL_H                   56
#define ST_TAB_H                    76
#define ST_HEAD_H                   64
#define ST_STATUS_H                 48
#define ST_CARD_H                   156
#define ST_CARD_PAD                 24
#define ST_ITEM_H                   140
#define ST_RADIUS                   24
#define ST_INFO_ROW_H               96

enum
{
    ST_PAGE_WIFI,
    ST_PAGE_NETWORK,
    ST_PAGE_SOUND,
    ST_PAGE_ABOUT
};

enum
{
    IDC_TAB_FIRST = 100,
    IDC_EXIT = 200,
    IDC_STATUS,
    IDC_WIFI_ADAPTER,
    IDC_WIFI_SCAN,
    IDC_WIFI_REFRESH,
    IDC_WIFI_DISCONNECT,
    IDC_WIFI_LIST,
    IDC_WIFI_PASSWORD_LABEL,
    IDC_WIFI_PASSWORD,
    IDC_WIFI_SHOW_PASSWORD,
    IDC_WIFI_CONNECT,
    IDC_WIFI_BACK,

    IDC_NET_LIST = 300,
    IDC_NET_RENEW,
    IDC_NET_RELEASE,
    IDC_NET_CONFIGURE,
    IDC_NET_BACK,
    IDC_NET_MODE,
    IDC_NET_ADDRESS_LABEL,
    IDC_NET_ADDRESS,
    IDC_NET_MASK_LABEL,
    IDC_NET_MASK,
    IDC_NET_GATEWAY_LABEL,
    IDC_NET_GATEWAY,
    IDC_NET_DNS_LABEL,
    IDC_NET_DNS,
    IDC_NET_SAVE,

    IDC_SND_OUTPUT = 400,
    IDC_SND_INPUT,
    IDC_SND_QUIETER,
    IDC_SND_LOUDER,
    IDC_SND_MUTE,
    IDC_SND_TEST,
    IDC_SND_REFRESH
};

/* One page of the app, reached from its tab */
typedef struct _ST_PAGE
{
    LPCWSTR Tab;
    const UINT *Controls;
    const DWORD *ControlCount;
    VOID (*Show)(VOID);
    VOID (*Layout)(_In_ const RECT *Area);
    VOID (*Paint)(_In_ HDC Dc, _In_ const RECT *Area);
    VOID (*Heading)(_Out_writes_(Count) LPWSTR Text, _In_ DWORD Count);
    VOID (*Command)(_In_ UINT Id, _In_ UINT Code);
    VOID (*Timer)(VOID);
    BOOL (*IsActive)(_In_ UINT Id);
} ST_PAGE, *PST_PAGE;

/* A label over a value, as the information pages list them */
typedef struct _ST_INFO_ROW
{
    WCHAR Label[32];
    WCHAR Value[160];
} ST_INFO_ROW, *PST_INFO_ROW;

extern HINSTANCE StInstance;
extern HWND StWindow;
extern HFONT StFont;
extern HFONT StSmallFont;
extern HFONT StBigFont;
extern HFONT StButtonFont;
extern HBRUSH StBgBrush;
extern DWORD StPage;

VOID
StFillRound(
    _In_ HDC Dc,
    _In_ const RECT *Rect,
    _In_ int Radius,
    _In_ COLORREF Color);

VOID
StSetStatus(
    _In_ LPCWSTR Format,
    ...);

VOID
StShowGroup(
    _In_reads_(Count) const UINT *Ids,
    _In_ DWORD Count,
    _In_ BOOL Show);

VOID
StMove(
    _In_ UINT Id,
    _In_ int X,
    _In_ int Y,
    _In_ int Width,
    _In_ int Height);

VOID
StRelayout(VOID);

HWND
StControl(
    _In_ LPCWSTR Class,
    _In_opt_ LPCWSTR Text,
    _In_ DWORD Style,
    _In_ UINT Id,
    _In_ HFONT Font);

HWND
StButton(
    _In_ LPCWSTR Text,
    _In_ DWORD Style,
    _In_ UINT Id);

VOID
StErrorText(
    _In_ DWORD Error,
    _Out_writes_(Count) LPWSTR Text,
    _In_ DWORD Count);

VOID
StDrawCard(
    _In_ HDC Dc,
    _In_ const RECT *Card,
    _In_ LPCWSTR Title,
    _In_ LPCWSTR Detail,
    _In_ BOOL Highlight);

VOID
StSetInfoRow(
    _Out_ PST_INFO_ROW Row,
    _In_ LPCWSTR Label,
    _In_ LPCWSTR Format,
    ...);

VOID
StPaintInfoRows(
    _In_ HDC Dc,
    _In_ const RECT *Area,
    _In_reads_(Count) const ST_INFO_ROW *Rows,
    _In_ DWORD Count);

BOOL
StStartWorker(
    _In_ LPTHREAD_START_ROUTINE Routine,
    _In_ PVOID Parameter);

/* Network page */
extern const UINT StNetControls[];
extern const DWORD StNetControlCount;

VOID
StNetCreateControls(VOID);

VOID
StNetShow(VOID);

VOID
StNetLayout(
    _In_ const RECT *Area);

VOID
StNetPaint(
    _In_ HDC Dc,
    _In_ const RECT *Area);

VOID
StNetHeading(
    _Out_writes_(Count) LPWSTR Text,
    _In_ DWORD Count);

VOID
StNetCommand(
    _In_ UINT Id,
    _In_ UINT Code);

VOID
StNetTimer(VOID);

BOOL
StNetIsActive(
    _In_ UINT Id);

VOID
StNetDrawItem(
    _In_ const DRAWITEMSTRUCT *Item);

VOID
StNetDone(
    _In_ LPARAM Result);

VOID
StNetCleanup(VOID);

/* Sound page */
extern const UINT StSndControls[];
extern const DWORD StSndControlCount;

VOID
StSndCreateControls(VOID);

VOID
StSndShow(VOID);

VOID
StSndLayout(
    _In_ const RECT *Area);

VOID
StSndPaint(
    _In_ HDC Dc,
    _In_ const RECT *Area);

VOID
StSndHeading(
    _Out_writes_(Count) LPWSTR Text,
    _In_ DWORD Count);

VOID
StSndCommand(
    _In_ UINT Id,
    _In_ UINT Code);

VOID
StSndTimer(VOID);

BOOL
StSndIsActive(
    _In_ UINT Id);

VOID
StSndDone(
    _In_ LPARAM Result);

VOID
StSndCleanup(VOID);
