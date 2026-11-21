/*
 * PROJECT:     ReactOS Clock
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Clock and alarms on top of the native time service
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>
#include <winuser.h>
#include <wingdi.h>
#include <winnls.h>
#include <mmsystem.h>
#include <stdlib.h>
#include <strsafe.h>
#include <wchar.h>

#include <nativetime.h>

#include "resource.h"

#define CK_CLASS_NAME               L"ReactOSClock"
#define WM_CLOCK_EVENT              (WM_APP + 1)
#define WM_CLOCK_SERVICE            (WM_APP + 2)
#define WM_CLOCK_RING_CHECK         (WM_APP + 3)

#define CK_TICK_TIMER               1
#define CK_REPEAT_TIMER             2
#define CK_REPEAT_FIRST_MS          450
#define CK_REPEAT_NEXT_MS           90
#define CK_RECONNECT_MS             3000
#define CK_PIPE_WAIT_MS             1000
#define CK_SNOOZE_MINUTES           10
#define CK_DRAG_SLOP                24
#define CK_ALARM_SOUND              L"%SystemRoot%\\Media\\ReactOS_Notify.wav"

/* Dark theme shared with the mobile shell */
#define CK_BG                       RGB(0x12, 0x12, 0x14)
#define CK_TILE                     RGB(0x24, 0x25, 0x2A)
#define CK_ACCENT                   RGB(0x3A, 0x96, 0xDD)
#define CK_TEXT                     RGB(0xF2, 0xF2, 0xF4)
#define CK_SUBTEXT                  RGB(0x9A, 0x9B, 0xA3)
#define CK_FIELD                    RGB(0x1A, 0x1A, 0x1E)
#define CK_OFF                      RGB(0x3A, 0x3B, 0x42)

/* Touch sized layout in pixels, the top row stays clear of the camera cutout */
#define CK_MARGIN                   28
#define CK_SAFE_TOP                 120
#define CK_GAP                      16
#define CK_BUTTON_H                 104
#define CK_CARD_H                   150
#define CK_TOGGLE_W                 140
#define CK_TOGGLE_H                 72
#define CK_STEP_H                   120
#define CK_DAY_SIZE                 112
#define CK_FIELD_H                  88

enum
{
    IDC_LABEL = 100
};

typedef enum _CK_VIEW
{
    CkViewMain,
    CkViewEdit,
    CkViewRing
} CK_VIEW;

/* Everything the user can touch, found again by hit testing */
typedef enum _CK_HIT_KIND
{
    CkHitNone,
    CkHitAdd,
    CkHitExit,
    CkHitCard,
    CkHitToggle,
    CkHitHourUp,
    CkHitHourDown,
    CkHitMinuteUp,
    CkHitMinuteDown,
    CkHitDay,
    CkHitSave,
    CkHitCancel,
    CkHitDelete,
    CkHitSnooze,
    CkHitDismiss
} CK_HIT_KIND;

typedef struct _CK_HIT
{
    CK_HIT_KIND Kind;
    DWORD Index;
    RECT Rect;
} CK_HIT;

#define CK_MAX_HITS                 (NATIVETIME_MAX_TRIGGERS * 2 + 32)

static HINSTANCE CkInstance;
static HWND CkWindow;
static HWND CkLabel;
static HFONT CkHugeFont;
static HFONT CkBigFont;
static HFONT CkFont;
static HFONT CkSmallFont;
static HFONT CkHeadFont;
static HFONT CkButtonFont;
static HBRUSH CkBgBrush;
static HBRUSH CkFieldBrush;

static CK_VIEW CkView = CkViewMain;
static NATIVETIME_TRIGGER CkAlarms[NATIVETIME_MAX_TRIGGERS];
static DWORD CkAlarmCount;
static BOOL CkServiceUp;

static NATIVETIME_TRIGGER CkEditing;
static NATIVETIME_TRIGGER CkRinging;
static BOOL CkSoundOn;

static CK_HIT CkHits[CK_MAX_HITS];
static DWORD CkHitCount;
static CK_HIT_KIND CkRepeatKind;

static LONG CkScroll;
static LONG CkScrollMax;
static BOOL CkPressed;
static BOOL CkDragging;
static POINT CkPressPoint;
static LONG CkPressScroll;

static HANDLE CkStopEvent;
static HANDLE CkSubscribeThread;
static HANDLE CkSubscribePipe;
static CRITICAL_SECTION CkPipeLock;

static
HANDLE
CkOpenPipe(VOID)
{
    DWORD Mode = PIPE_READMODE_MESSAGE;
    HANDLE Pipe;

    for (;;)
    {
        Pipe = CreateFileW(NATIVETIME_PIPE_NAME, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (Pipe != INVALID_HANDLE_VALUE)
            break;

        if (GetLastError() != ERROR_PIPE_BUSY || !WaitNamedPipeW(NATIVETIME_PIPE_NAME, CK_PIPE_WAIT_MS))
            return INVALID_HANDLE_VALUE;
    }

    if (!SetNamedPipeHandleState(Pipe, &Mode, NULL, NULL))
    {
        CloseHandle(Pipe);
        return INVALID_HANDLE_VALUE;
    }

    return Pipe;
}

/* One request and its reply on a fresh connection */
static
BOOL
CkCall(
    _In_ const NATIVETIME_REQUEST *Request,
    _Out_ PNATIVETIME_REPLY Reply)
{
    HANDLE Pipe;
    DWORD Bytes;
    BOOL Ok;

    ZeroMemory(Reply, sizeof(*Reply));
    Pipe = CkOpenPipe();
    if (Pipe == INVALID_HANDLE_VALUE)
        return FALSE;

    Ok = WriteFile(Pipe, Request, sizeof(*Request), &Bytes, NULL) && Bytes == sizeof(*Request) &&
         ReadFile(Pipe, Reply, sizeof(*Reply), &Bytes, NULL) && Bytes == sizeof(*Reply);

    CloseHandle(Pipe);
    return Ok && Reply->Error == ERROR_SUCCESS;
}

static
BOOL
CkSimpleCall(
    _In_ DWORD Command,
    _In_ DWORD Id,
    _In_ DWORD SnoozeMinutes)
{
    NATIVETIME_REQUEST Request;
    PNATIVETIME_REPLY Reply;
    BOOL Ok;

    Reply = HeapAlloc(GetProcessHeap(), 0, sizeof(*Reply));
    if (!Reply)
        return FALSE;

    ZeroMemory(&Request, sizeof(Request));
    Request.Command = Command;
    Request.Id = Id;
    Request.SnoozeMinutes = SnoozeMinutes;
    Ok = CkCall(&Request, Reply);

    HeapFree(GetProcessHeap(), 0, Reply);
    return Ok;
}

static
int
__cdecl
CkCompareAlarms(
    _In_ const void *First,
    _In_ const void *Second)
{
    const NATIVETIME_TRIGGER *A = First, *B = Second;

    if (A->Hour != B->Hour)
        return (int)A->Hour - (int)B->Hour;
    if (A->Minute != B->Minute)
        return (int)A->Minute - (int)B->Minute;
    return (int)A->Id - (int)B->Id;
}

/* Reloads the alarm list, returns the one ringing if any */
static
BOOL
CkRefresh(
    _Out_opt_ PNATIVETIME_TRIGGER Ringing)
{
    NATIVETIME_REQUEST Request;
    PNATIVETIME_REPLY Reply;
    BOOL Found = FALSE;
    DWORD i;

    Reply = HeapAlloc(GetProcessHeap(), 0, sizeof(*Reply));
    if (!Reply)
        return FALSE;

    ZeroMemory(&Request, sizeof(Request));
    Request.Command = NativeTimeList;
    CkServiceUp = CkCall(&Request, Reply);

    CkAlarmCount = 0;
    if (CkServiceUp)
    {
        for (i = 0; i < Reply->Count && i < NATIVETIME_MAX_TRIGGERS; i++)
        {
            if (Reply->Triggers[i].Kind != NativeTimeAlarm)
                continue;

            CkAlarms[CkAlarmCount++] = Reply->Triggers[i];
            if (Ringing && !Found && (Reply->Triggers[i].Flags & NATIVETIME_FLAG_RINGING))
            {
                *Ringing = Reply->Triggers[i];
                Found = TRUE;
            }
        }
        qsort(CkAlarms, CkAlarmCount, sizeof(CkAlarms[0]), CkCompareAlarms);
    }

    HeapFree(GetProcessHeap(), 0, Reply);
    InvalidateRect(CkWindow, NULL, FALSE);
    return Found;
}

/* Keeps one connection subscribed and turns its events into window messages */
static
DWORD
WINAPI
CkSubscribeProc(
    _In_ LPVOID Parameter)
{
    NATIVETIME_REQUEST Request;
    NATIVETIME_EVENT Event;
    PNATIVETIME_EVENT Copy;
    HANDLE Pipe;
    DWORD Bytes;

    UNREFERENCED_PARAMETER(Parameter);

    while (WaitForSingleObject(CkStopEvent, 0) != WAIT_OBJECT_0)
    {
        Pipe = CkOpenPipe();
        if (Pipe == INVALID_HANDLE_VALUE)
        {
            PostMessageW(CkWindow, WM_CLOCK_SERVICE, FALSE, 0);
            WaitForSingleObject(CkStopEvent, CK_RECONNECT_MS);
            continue;
        }

        ZeroMemory(&Request, sizeof(Request));
        Request.Command = NativeTimeSubscribe;
        if (!WriteFile(Pipe, &Request, sizeof(Request), &Bytes, NULL))
        {
            CloseHandle(Pipe);
            WaitForSingleObject(CkStopEvent, CK_RECONNECT_MS);
            continue;
        }

        EnterCriticalSection(&CkPipeLock);
        CkSubscribePipe = Pipe;
        LeaveCriticalSection(&CkPipeLock);

        PostMessageW(CkWindow, WM_CLOCK_SERVICE, TRUE, 0);

        while (ReadFile(Pipe, &Event, sizeof(Event), &Bytes, NULL) && Bytes == sizeof(Event))
        {
            Copy = HeapAlloc(GetProcessHeap(), 0, sizeof(*Copy));
            if (!Copy)
                continue;

            *Copy = Event;
            if (!PostMessageW(CkWindow, WM_CLOCK_EVENT, 0, (LPARAM)Copy))
                HeapFree(GetProcessHeap(), 0, Copy);
        }

        EnterCriticalSection(&CkPipeLock);
        CkSubscribePipe = NULL;
        LeaveCriticalSection(&CkPipeLock);
        CloseHandle(Pipe);

        PostMessageW(CkWindow, WM_CLOCK_SERVICE, FALSE, 0);
        WaitForSingleObject(CkStopEvent, CK_RECONNECT_MS);
    }

    return 0;
}

static
VOID
CkStartSound(VOID)
{
    WCHAR Path[MAX_PATH];

    if (CkSoundOn)
        return;

    if (ExpandEnvironmentStringsW(CK_ALARM_SOUND, Path, _countof(Path)) &&
        PlaySoundW(Path, NULL, SND_ASYNC | SND_LOOP | SND_FILENAME | SND_NODEFAULT))
    {
        CkSoundOn = TRUE;
        return;
    }

    CkSoundOn = PlaySoundW(L"SystemExclamation", NULL, SND_ASYNC | SND_LOOP | SND_ALIAS);
}

static
VOID
CkStopSound(VOID)
{
    PlaySoundW(NULL, NULL, 0);
    CkSoundOn = FALSE;
}

static
VOID
CkShowView(
    _In_ CK_VIEW View)
{
    CkView = View;
    CkScroll = 0;
    ShowWindow(CkLabel, (View == CkViewEdit) ? SW_SHOW : SW_HIDE);
    if (View != CkViewEdit && GetFocus() == CkLabel)
        SetFocus(CkWindow);
    InvalidateRect(CkWindow, NULL, FALSE);
}

static
VOID
CkStartRinging(
    _In_ const NATIVETIME_TRIGGER *Trigger)
{
    CkRinging = *Trigger;
    CkShowView(CkViewRing);
    CkStartSound();

    ShowWindow(CkWindow, SW_SHOWMAXIMIZED);
    SetForegroundWindow(CkWindow);
}

static
VOID
CkStopRinging(VOID)
{
    CkStopSound();
    ZeroMemory(&CkRinging, sizeof(CkRinging));
    if (CkView == CkViewRing)
        CkShowView(CkViewMain);
    CkRefresh(NULL);
}

static
VOID
CkBeginEdit(
    _In_opt_ const NATIVETIME_TRIGGER *Alarm)
{
    SYSTEMTIME Now;

    if (Alarm)
    {
        CkEditing = *Alarm;
    }
    else
    {
        GetLocalTime(&Now);
        ZeroMemory(&CkEditing, sizeof(CkEditing));
        CkEditing.Kind = NativeTimeAlarm;
        CkEditing.Flags = NATIVETIME_FLAG_ENABLED;
        CkEditing.Hour = (Now.wHour + 1) % 24;
        CkEditing.Minute = 0;
    }

    SetWindowTextW(CkLabel, CkEditing.Label);
    CkShowView(CkViewEdit);
}

static
VOID
CkSaveEdit(VOID)
{
    NATIVETIME_REQUEST Request;
    PNATIVETIME_REPLY Reply;

    GetWindowTextW(CkLabel, CkEditing.Label, _countof(CkEditing.Label));
    CkEditing.Flags |= NATIVETIME_FLAG_ENABLED;

    Reply = HeapAlloc(GetProcessHeap(), 0, sizeof(*Reply));
    if (Reply)
    {
        ZeroMemory(&Request, sizeof(Request));
        Request.Command = NativeTimeSet;
        Request.Trigger = CkEditing;
        CkCall(&Request, Reply);
        HeapFree(GetProcessHeap(), 0, Reply);
    }

    CkShowView(CkViewMain);
    CkRefresh(NULL);
}

static
VOID
CkToggle(
    _In_ DWORD Index)
{
    NATIVETIME_REQUEST Request;
    PNATIVETIME_REPLY Reply;

    if (Index >= CkAlarmCount)
        return;

    Reply = HeapAlloc(GetProcessHeap(), 0, sizeof(*Reply));
    if (!Reply)
        return;

    ZeroMemory(&Request, sizeof(Request));
    Request.Command = NativeTimeSet;
    Request.Trigger = CkAlarms[Index];
    Request.Trigger.Flags ^= NATIVETIME_FLAG_ENABLED;
    CkCall(&Request, Reply);
    HeapFree(GetProcessHeap(), 0, Reply);

    CkRefresh(NULL);
}

static
VOID
CkStep(
    _In_ CK_HIT_KIND Kind)
{
    switch (Kind)
    {
        case CkHitHourUp:
            CkEditing.Hour = (CkEditing.Hour + 1) % 24;
            break;
        case CkHitHourDown:
            CkEditing.Hour = (CkEditing.Hour + 23) % 24;
            break;
        case CkHitMinuteUp:
            CkEditing.Minute = (CkEditing.Minute + 1) % 60;
            break;
        case CkHitMinuteDown:
            CkEditing.Minute = (CkEditing.Minute + 59) % 60;
            break;
        default:
            return;
    }

    InvalidateRect(CkWindow, NULL, FALSE);
}

static
VOID
CkFormatTime(
    _In_ WORD Hour,
    _In_ WORD Minute,
    _Out_writes_(Count) LPWSTR Text,
    _In_ DWORD Count)
{
    StringCchPrintfW(Text, Count, L"%u:%02u %s", (Hour % 12) ? (Hour % 12) : 12, Minute, (Hour < 12) ? L"AM" : L"PM");
}

static
VOID
CkFormatDays(
    _In_ DWORD Days,
    _Out_writes_(Count) LPWSTR Text,
    _In_ DWORD Count)
{
    static const PCWSTR Names[7] = { L"Sun", L"Mon", L"Tue", L"Wed", L"Thu", L"Fri", L"Sat" };
    DWORD i;

    Text[0] = UNICODE_NULL;
    if (!Days)
    {
        StringCchCopyW(Text, Count, L"Once");
        return;
    }
    if (Days == NATIVETIME_DAY_ALL)
    {
        StringCchCopyW(Text, Count, L"Every day");
        return;
    }
    if (Days == (NATIVETIME_DAY_ALL & ~(NATIVETIME_DAY_SUNDAY | NATIVETIME_DAY_SATURDAY)))
    {
        StringCchCopyW(Text, Count, L"Weekdays");
        return;
    }
    if (Days == (NATIVETIME_DAY_SUNDAY | NATIVETIME_DAY_SATURDAY))
    {
        StringCchCopyW(Text, Count, L"Weekends");
        return;
    }

    for (i = 0; i < 7; i++)
    {
        if (!(Days & (1 << i)))
            continue;
        if (Text[0])
            StringCchCatW(Text, Count, L" ");
        StringCchCatW(Text, Count, Names[i]);
    }
}

/* "in 7 h 20 min" from the service's next ring */
static
VOID
CkFormatUntil(
    _In_ const FILETIME *Due,
    _Out_writes_(Count) LPWSTR Text,
    _In_ DWORD Count)
{
    ULARGE_INTEGER DueValue, NowValue;
    FILETIME Now;
    ULONGLONG Minutes;

    Text[0] = UNICODE_NULL;
    DueValue.LowPart = Due->dwLowDateTime;
    DueValue.HighPart = Due->dwHighDateTime;
    GetSystemTimeAsFileTime(&Now);
    NowValue.LowPart = Now.dwLowDateTime;
    NowValue.HighPart = Now.dwHighDateTime;
    if (!DueValue.QuadPart || DueValue.QuadPart <= NowValue.QuadPart)
        return;

    Minutes = (DueValue.QuadPart - NowValue.QuadPart + 600000000ULL - 1) / 600000000ULL;
    if (Minutes >= 24 * 60)
        StringCchPrintfW(Text, Count, L"in %I64u d %I64u h", Minutes / (24 * 60), (Minutes / 60) % 24);
    else if (Minutes >= 60)
        StringCchPrintfW(Text, Count, L"in %I64u h %I64u min", Minutes / 60, Minutes % 60);
    else
        StringCchPrintfW(Text, Count, L"in %I64u min", Minutes);
}

static
VOID
CkAddHit(
    _In_ CK_HIT_KIND Kind,
    _In_ DWORD Index,
    _In_ const RECT *Rect)
{
    if (CkHitCount >= CK_MAX_HITS)
        return;

    CkHits[CkHitCount].Kind = Kind;
    CkHits[CkHitCount].Index = Index;
    CkHits[CkHitCount].Rect = *Rect;
    CkHitCount++;
}

static
const CK_HIT *
CkHitTest(
    _In_ POINT Point)
{
    LONG i;

    /* Later hits sit on top of earlier ones, the toggle on its card */
    for (i = (LONG)CkHitCount - 1; i >= 0; i--)
    {
        if (PtInRect(&CkHits[i].Rect, Point))
            return &CkHits[i];
    }

    return NULL;
}

static
VOID
CkFillRound(
    _In_ HDC Dc,
    _In_ const RECT *Rect,
    _In_ int Radius,
    _In_ COLORREF Color)
{
    HBRUSH Brush = CreateSolidBrush(Color);
    HGDIOBJ OldBrush = SelectObject(Dc, Brush);
    HGDIOBJ OldPen = SelectObject(Dc, GetStockObject(NULL_PEN));

    RoundRect(Dc, Rect->left, Rect->top, Rect->right + 1, Rect->bottom + 1, Radius, Radius);

    SelectObject(Dc, OldPen);
    SelectObject(Dc, OldBrush);
    DeleteObject(Brush);
}

static
VOID
CkDrawText(
    _In_ HDC Dc,
    _In_z_ LPCWSTR Text,
    _In_ const RECT *Rect,
    _In_ HFONT Font,
    _In_ COLORREF Color,
    _In_ UINT Format)
{
    RECT Copy = *Rect;

    SelectObject(Dc, Font);
    SetTextColor(Dc, Color);
    DrawTextW(Dc, Text, -1, &Copy, Format | DT_NOPREFIX);
}

static
VOID
CkDrawButton(
    _In_ HDC Dc,
    _In_ const RECT *Rect,
    _In_z_ LPCWSTR Text,
    _In_ BOOL Accent,
    _In_ CK_HIT_KIND Kind,
    _In_ DWORD Index)
{
    CkFillRound(Dc, Rect, (Rect->bottom - Rect->top) / 3, Accent ? CK_ACCENT : CK_TILE);
    CkDrawText(Dc, Text, Rect, CkButtonFont, CK_TEXT, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_END_ELLIPSIS);
    CkAddHit(Kind, Index, Rect);
}

/* The two buttons along the bottom edge, the first one wider */
static
VOID
CkBottomRow(
    _In_ const RECT *Client,
    _Out_ RECT *Left,
    _Out_ RECT *Right)
{
    LONG Top = Client->bottom - CK_MARGIN * 2 - CK_BUTTON_H;
    LONG Split = Client->left + CK_MARGIN + (Client->right - Client->left - CK_MARGIN * 2 - CK_GAP) * 2 / 3;

    SetRect(Left, Client->left + CK_MARGIN, Top, Split, Top + CK_BUTTON_H);
    SetRect(Right, Split + CK_GAP, Top, Client->right - CK_MARGIN, Top + CK_BUTTON_H);
}

static
VOID
CkPaintMain(
    _In_ HDC Dc,
    _In_ const RECT *Client)
{
    WCHAR Line[96], Days[64], Until[48];
    SYSTEMTIME Now;
    RECT Rect, Card, Toggle, Knob, Add, Exit, ListArea;
    LONG Top, Bottom;
    DWORD i;
    HRGN Clip;

    GetLocalTime(&Now);

    CkFormatTime(Now.wHour, Now.wMinute, Line, _countof(Line));
    SetRect(&Rect, Client->left, CK_SAFE_TOP, Client->right, CK_SAFE_TOP + 200);
    CkDrawText(Dc, Line, &Rect, CkHugeFont, CK_TEXT, DT_SINGLELINE | DT_CENTER | DT_VCENTER);

    GetDateFormatW(LOCALE_USER_DEFAULT, 0, &Now, L"dddd, MMMM d", Line, _countof(Line));
    SetRect(&Rect, Client->left, Rect.bottom, Client->right, Rect.bottom + 60);
    CkDrawText(Dc, Line, &Rect, CkFont, CK_SUBTEXT, DT_SINGLELINE | DT_CENTER | DT_VCENTER);

    SetRect(&Rect, Client->left + CK_MARGIN, Rect.bottom + CK_GAP, Client->right - CK_MARGIN, Rect.bottom + CK_GAP + 64);
    CkDrawText(Dc, CkServiceUp ? L"Alarms" : L"Alarm service not running", &Rect, CkHeadFont,
               CkServiceUp ? CK_TEXT : CK_SUBTEXT, DT_SINGLELINE | DT_LEFT | DT_VCENTER);

    CkBottomRow(Client, &Add, &Exit);
    CkDrawButton(Dc, &Add, L"Add alarm", TRUE, CkHitAdd, 0);
    CkDrawButton(Dc, &Exit, L"Exit", FALSE, CkHitExit, 0);

    SetRect(&ListArea, Client->left, Rect.bottom + CK_GAP, Client->right, Add.top - CK_GAP);
    Top = ListArea.top;
    Bottom = ListArea.bottom;

    CkScrollMax = max(0, (LONG)CkAlarmCount * (CK_CARD_H + CK_GAP) - (Bottom - Top));
    CkScroll = min(max(CkScroll, 0), CkScrollMax);

    if (!CkAlarmCount && CkServiceUp)
    {
        SetRect(&Rect, Client->left + CK_MARGIN, Top, Client->right - CK_MARGIN, Top + 80);
        CkDrawText(Dc, L"No alarms yet", &Rect, CkFont, CK_SUBTEXT, DT_SINGLELINE | DT_LEFT | DT_VCENTER);
        return;
    }

    Clip = CreateRectRgnIndirect(&ListArea);
    SelectClipRgn(Dc, Clip);

    for (i = 0; i < CkAlarmCount; i++)
    {
        const NATIVETIME_TRIGGER *Alarm = &CkAlarms[i];
        BOOL Enabled = (Alarm->Flags & NATIVETIME_FLAG_ENABLED) != 0;
        LONG CardTop = Top + (LONG)i * (CK_CARD_H + CK_GAP) - CkScroll;

        if (CardTop + CK_CARD_H < Top || CardTop > Bottom)
            continue;

        SetRect(&Card, Client->left + CK_MARGIN, CardTop, Client->right - CK_MARGIN, CardTop + CK_CARD_H);
        CkFillRound(Dc, &Card, 36, CK_TILE);

        CkFormatTime(Alarm->Hour, Alarm->Minute, Line, _countof(Line));
        SetRect(&Rect, Card.left + 32, Card.top + 12, Card.right - CK_TOGGLE_W - 48, Card.top + 88);
        CkDrawText(Dc, Line, &Rect, CkBigFont, Enabled ? CK_TEXT : CK_SUBTEXT, DT_SINGLELINE | DT_LEFT | DT_VCENTER);

        CkFormatDays(Alarm->Days, Days, _countof(Days));
        Until[0] = UNICODE_NULL;
        if (Enabled)
            CkFormatUntil(&Alarm->Due, Until, _countof(Until));
        StringCchPrintfW(Line, _countof(Line), L"%s%s%s%s%s", Days,
                         Alarm->Label[0] ? L", " : L"", Alarm->Label,
                         Until[0] ? L", " : L"", Until);
        SetRect(&Rect, Card.left + 32, Card.top + 88, Card.right - CK_TOGGLE_W - 48, Card.bottom - 12);
        CkDrawText(Dc, Line, &Rect, CkSmallFont, CK_SUBTEXT, DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);

        SetRect(&Toggle, Card.right - 32 - CK_TOGGLE_W, Card.top + (CK_CARD_H - CK_TOGGLE_H) / 2,
                Card.right - 32, Card.top + (CK_CARD_H + CK_TOGGLE_H) / 2);
        CkFillRound(Dc, &Toggle, CK_TOGGLE_H, Enabled ? CK_ACCENT : CK_OFF);
        Knob = Toggle;
        InflateRect(&Knob, -8, -8);
        if (Enabled)
            Knob.left = Knob.right - (Knob.bottom - Knob.top);
        else
            Knob.right = Knob.left + (Knob.bottom - Knob.top);
        CkFillRound(Dc, &Knob, Knob.bottom - Knob.top, CK_TEXT);

        /* Hits outside the visible list must not catch touches */
        IntersectRect(&Card, &Card, &ListArea);
        IntersectRect(&Toggle, &Toggle, &ListArea);
        CkAddHit(CkHitCard, i, &Card);
        CkAddHit(CkHitToggle, i, &Toggle);
    }

    SelectClipRgn(Dc, NULL);
    DeleteObject(Clip);
}

static
VOID
CkPaintEdit(
    _In_ HDC Dc,
    _In_ const RECT *Client)
{
    static const PCWSTR Letters[7] = { L"S", L"M", L"T", L"W", L"T", L"F", L"S" };
    WCHAR Line[32];
    RECT Rect, Column, Up, Down, Day, Save, Cancel, Delete, Label;
    LONG Half, DaySpan, Left, Top;
    DWORD i;

    SetRect(&Rect, Client->left + CK_MARGIN, CK_SAFE_TOP, Client->right - CK_MARGIN, CK_SAFE_TOP + 72);
    CkDrawText(Dc, CkEditing.Id ? L"Edit alarm" : L"New alarm", &Rect, CkHeadFont, CK_TEXT,
               DT_SINGLELINE | DT_LEFT | DT_VCENTER);

    /* Hour on the left, minute on the right, each with a step up above and down below */
    Half = (Client->right - Client->left) / 2;
    Top = Rect.bottom + CK_GAP * 2;
    for (i = 0; i < 2; i++)
    {
        SetRect(&Column, Client->left + i * Half + CK_MARGIN, Top, Client->left + (i + 1) * Half - CK_MARGIN, Top);

        SetRect(&Up, Column.left, Top, Column.right, Top + CK_STEP_H);
        CkDrawButton(Dc, &Up, L"+", FALSE, i ? CkHitMinuteUp : CkHitHourUp, 0);

        StringCchPrintfW(Line, _countof(Line), L"%02u", i ? CkEditing.Minute : CkEditing.Hour);
        SetRect(&Rect, Column.left, Up.bottom, Column.right, Up.bottom + 220);
        CkDrawText(Dc, Line, &Rect, CkHugeFont, CK_TEXT, DT_SINGLELINE | DT_CENTER | DT_VCENTER);

        SetRect(&Down, Column.left, Rect.bottom, Column.right, Rect.bottom + CK_STEP_H);
        CkDrawButton(Dc, &Down, L"-", FALSE, i ? CkHitMinuteDown : CkHitHourDown, 0);
    }

    CkFormatTime(CkEditing.Hour, CkEditing.Minute, Line, _countof(Line));
    SetRect(&Rect, Client->left, Down.bottom + CK_GAP, Client->right, Down.bottom + CK_GAP + 60);
    CkDrawText(Dc, Line, &Rect, CkFont, CK_SUBTEXT, DT_SINGLELINE | DT_CENTER | DT_VCENTER);

    SetRect(&Rect, Client->left + CK_MARGIN, Rect.bottom + CK_GAP, Client->right - CK_MARGIN, Rect.bottom + CK_GAP + 50);
    CkDrawText(Dc, L"Repeat", &Rect, CkSmallFont, CK_SUBTEXT, DT_SINGLELINE | DT_LEFT | DT_VCENTER);

    DaySpan = (Client->right - Client->left - CK_MARGIN * 2) / 7;
    Top = Rect.bottom + CK_GAP / 2;
    for (i = 0; i < 7; i++)
    {
        Left = Client->left + CK_MARGIN + (LONG)i * DaySpan + (DaySpan - CK_DAY_SIZE) / 2;
        SetRect(&Day, Left, Top, Left + CK_DAY_SIZE, Top + CK_DAY_SIZE);
        CkFillRound(Dc, &Day, CK_DAY_SIZE, (CkEditing.Days & (1 << i)) ? CK_ACCENT : CK_TILE);
        CkDrawText(Dc, Letters[i], &Day, CkButtonFont, CK_TEXT, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
        CkAddHit(CkHitDay, i, &Day);
    }

    SetRect(&Rect, Client->left + CK_MARGIN, Day.bottom + CK_GAP * 2, Client->right - CK_MARGIN, Day.bottom + CK_GAP * 2 + 50);
    CkDrawText(Dc, L"Label", &Rect, CkSmallFont, CK_SUBTEXT, DT_SINGLELINE | DT_LEFT | DT_VCENTER);

    SetRect(&Label, Rect.left, Rect.bottom + CK_GAP / 2, Rect.right, Rect.bottom + CK_GAP / 2 + CK_FIELD_H);
    CkFillRound(Dc, &Label, 24, CK_FIELD);
    MoveWindow(CkLabel, Label.left + 20, Label.top + 16, Label.right - Label.left - 40, CK_FIELD_H - 32, FALSE);

    CkBottomRow(Client, &Save, &Cancel);
    CkDrawButton(Dc, &Save, L"Save", TRUE, CkHitSave, 0);
    CkDrawButton(Dc, &Cancel, L"Cancel", FALSE, CkHitCancel, 0);

    if (CkEditing.Id)
    {
        SetRect(&Delete, Save.left, Save.top - CK_GAP - CK_BUTTON_H, Cancel.right, Save.top - CK_GAP);
        CkDrawButton(Dc, &Delete, L"Delete alarm", FALSE, CkHitDelete, 0);
    }
}

static
VOID
CkPaintRing(
    _In_ HDC Dc,
    _In_ const RECT *Client)
{
    WCHAR Line[96];
    SYSTEMTIME Now;
    RECT Rect, Snooze, Dismiss;
    LONG Height = Client->bottom - Client->top;

    GetLocalTime(&Now);

    SetRect(&Rect, Client->left, CK_SAFE_TOP + Height / 8, Client->right, CK_SAFE_TOP + Height / 8 + 70);
    CkDrawText(Dc, L"Alarm", &Rect, CkHeadFont, CK_SUBTEXT, DT_SINGLELINE | DT_CENTER | DT_VCENTER);

    CkFormatTime(Now.wHour, Now.wMinute, Line, _countof(Line));
    SetRect(&Rect, Client->left, Rect.bottom, Client->right, Rect.bottom + 220);
    CkDrawText(Dc, Line, &Rect, CkHugeFont, CK_TEXT, DT_SINGLELINE | DT_CENTER | DT_VCENTER);

    SetRect(&Rect, Client->left + CK_MARGIN, Rect.bottom, Client->right - CK_MARGIN, Rect.bottom + 140);
    CkDrawText(Dc, CkRinging.Label[0] ? CkRinging.Label : L"Time to wake up", &Rect, CkBigFont, CK_TEXT,
               DT_CENTER | DT_WORDBREAK | DT_END_ELLIPSIS);

    SetRect(&Dismiss, Client->left + CK_MARGIN, Client->bottom - CK_MARGIN * 2 - CK_BUTTON_H * 2,
            Client->right - CK_MARGIN, Client->bottom - CK_MARGIN * 2);
    SetRect(&Snooze, Dismiss.left, Dismiss.top - CK_GAP * 2 - CK_BUTTON_H * 2, Dismiss.right, Dismiss.top - CK_GAP * 2);

    StringCchPrintfW(Line, _countof(Line), L"Snooze %u min", CK_SNOOZE_MINUTES);
    CkDrawButton(Dc, &Snooze, Line, FALSE, CkHitSnooze, 0);
    CkDrawButton(Dc, &Dismiss, L"Dismiss", TRUE, CkHitDismiss, 0);
}

static
VOID
CkPaint(
    _In_ HWND Window)
{
    PAINTSTRUCT Paint;
    RECT Client;
    HDC Dc, Memory;
    HBITMAP Bitmap;
    HGDIOBJ OldBitmap;

    Dc = BeginPaint(Window, &Paint);
    GetClientRect(Window, &Client);

    /* Drawn off screen, the clock repaints every second */
    Memory = CreateCompatibleDC(Dc);
    Bitmap = CreateCompatibleBitmap(Dc, Client.right, Client.bottom);
    OldBitmap = SelectObject(Memory, Bitmap);

    FillRect(Memory, &Client, CkBgBrush);
    SetBkMode(Memory, TRANSPARENT);
    CkHitCount = 0;

    if (CkView == CkViewEdit)
        CkPaintEdit(Memory, &Client);
    else if (CkView == CkViewRing)
        CkPaintRing(Memory, &Client);
    else
        CkPaintMain(Memory, &Client);

    BitBlt(Dc, 0, 0, Client.right, Client.bottom, Memory, 0, 0, SRCCOPY);

    SelectObject(Memory, OldBitmap);
    DeleteObject(Bitmap);
    DeleteDC(Memory);
    EndPaint(Window, &Paint);
}

static
VOID
CkActivate(
    _In_ const CK_HIT *Hit)
{
    switch (Hit->Kind)
    {
        case CkHitAdd:
            CkBeginEdit(NULL);
            break;

        case CkHitExit:
            DestroyWindow(CkWindow);
            break;

        case CkHitCard:
            if (Hit->Index < CkAlarmCount)
                CkBeginEdit(&CkAlarms[Hit->Index]);
            break;

        case CkHitToggle:
            CkToggle(Hit->Index);
            break;

        case CkHitDay:
            CkEditing.Days ^= (1 << Hit->Index);
            InvalidateRect(CkWindow, NULL, FALSE);
            break;

        case CkHitSave:
            CkSaveEdit();
            break;

        case CkHitCancel:
            CkShowView(CkViewMain);
            break;

        case CkHitDelete:
            CkSimpleCall(NativeTimeDelete, CkEditing.Id, 0);
            CkShowView(CkViewMain);
            CkRefresh(NULL);
            break;

        case CkHitSnooze:
            CkSimpleCall(NativeTimeSnooze, CkRinging.Id, CK_SNOOZE_MINUTES);
            CkStopRinging();
            break;

        case CkHitDismiss:
            CkSimpleCall(NativeTimeDismiss, CkRinging.Id, 0);
            CkStopRinging();
            break;

        default:
            break;
    }
}

static
VOID
CkHandleEvent(
    _In_ const NATIVETIME_EVENT *Event)
{
    switch (Event->Code)
    {
        case NativeTimeEventRing:
            CkRefresh(NULL);
            CkStartRinging(&Event->Trigger);
            break;

        case NativeTimeEventStopped:
            if (CkView == CkViewRing && Event->Trigger.Id == CkRinging.Id)
                CkStopRinging();
            else
                CkRefresh(NULL);
            break;

        case NativeTimeEventChanged:
            if (CkView != CkViewEdit)
                CkRefresh(NULL);
            break;

        case NativeTimeEventTimeSet:
            InvalidateRect(CkWindow, NULL, FALSE);
            break;
    }
}

/* Rings the alarm the service reports as ringing, for a start through the ring switch */
static
VOID
CkCheckRinging(VOID)
{
    NATIVETIME_TRIGGER Ringing;

    if (CkRefresh(&Ringing))
        CkStartRinging(&Ringing);
}

static
LRESULT
CALLBACK
CkWndProc(
    _In_ HWND Window,
    _In_ UINT Message,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam)
{
    const CK_HIT *Hit;
    POINT Point;

    switch (Message)
    {
        case WM_CREATE:
            CkWindow = Window;
            CkLabel = CreateWindowExW(0, L"EDIT", NULL, WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL,
                                      0, 0, 10, 10, Window, (HMENU)(ULONG_PTR)IDC_LABEL, CkInstance, NULL);
            SendMessageW(CkLabel, WM_SETFONT, (WPARAM)CkFont, FALSE);
            SendMessageW(CkLabel, EM_LIMITTEXT, NATIVETIME_MAX_LABEL - 1, 0);
            SetTimer(Window, CK_TICK_TIMER, 1000, NULL);
            return 0;

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT:
            CkPaint(Window);
            return 0;

        case WM_SIZE:
            InvalidateRect(Window, NULL, FALSE);
            return 0;

        case WM_CTLCOLOREDIT:
            SetTextColor((HDC)wParam, CK_TEXT);
            SetBkColor((HDC)wParam, CK_FIELD);
            return (LRESULT)CkFieldBrush;

        case WM_TIMER:
            if (wParam == CK_REPEAT_TIMER)
            {
                CkStep(CkRepeatKind);
                SetTimer(Window, CK_REPEAT_TIMER, CK_REPEAT_NEXT_MS, NULL);
            }
            else if (CkView != CkViewEdit)
            {
                InvalidateRect(Window, NULL, FALSE);
            }
            return 0;

        case WM_LBUTTONDOWN:
            Point.x = (SHORT)LOWORD(lParam);
            Point.y = (SHORT)HIWORD(lParam);
            CkPressed = TRUE;
            CkDragging = FALSE;
            CkPressPoint = Point;
            CkPressScroll = CkScroll;
            SetCapture(Window);

            /* The time steps act on press and keep stepping while held */
            Hit = CkHitTest(Point);
            if (Hit && Hit->Kind >= CkHitHourUp && Hit->Kind <= CkHitMinuteDown)
            {
                CkRepeatKind = Hit->Kind;
                CkStep(CkRepeatKind);
                SetTimer(Window, CK_REPEAT_TIMER, CK_REPEAT_FIRST_MS, NULL);
            }
            return 0;

        case WM_MOUSEMOVE:
            if (!CkPressed || CkView != CkViewMain)
                return 0;

            Point.y = (SHORT)HIWORD(lParam);
            if (!CkDragging && abs(Point.y - CkPressPoint.y) > CK_DRAG_SLOP)
                CkDragging = TRUE;
            if (CkDragging)
            {
                CkScroll = min(max(CkPressScroll - (Point.y - CkPressPoint.y), 0), CkScrollMax);
                InvalidateRect(Window, NULL, FALSE);
            }
            return 0;

        case WM_LBUTTONUP:
            if (!CkPressed)
                return 0;

            CkPressed = FALSE;
            ReleaseCapture();

            if (CkRepeatKind != CkHitNone)
            {
                KillTimer(Window, CK_REPEAT_TIMER);
                CkRepeatKind = CkHitNone;
                return 0;
            }

            if (CkDragging)
                return 0;

            Point.x = (SHORT)LOWORD(lParam);
            Point.y = (SHORT)HIWORD(lParam);
            Hit = CkHitTest(Point);
            if (Hit && PtInRect(&Hit->Rect, CkPressPoint))
                CkActivate(Hit);
            return 0;

        case WM_MOUSEWHEEL:
            if (CkView == CkViewMain)
            {
                CkScroll = min(max(CkScroll - GET_WHEEL_DELTA_WPARAM(wParam), 0), CkScrollMax);
                InvalidateRect(Window, NULL, FALSE);
            }
            return 0;

        case WM_CLOCK_EVENT:
            CkHandleEvent((const NATIVETIME_EVENT *)lParam);
            HeapFree(GetProcessHeap(), 0, (PVOID)lParam);
            return 0;

        case WM_CLOCK_SERVICE:
            if (wParam)
                CkCheckRinging();
            else if (CkServiceUp)
                CkRefresh(NULL);
            return 0;

        case WM_CLOCK_RING_CHECK:
            ShowWindow(Window, SW_SHOWMAXIMIZED);
            SetForegroundWindow(Window);
            CkCheckRinging();
            return 0;

        case WM_DESTROY:
            KillTimer(Window, CK_TICK_TIMER);
            KillTimer(Window, CK_REPEAT_TIMER);
            CkStopSound();
            PostQuitMessage(0);
            return 0;
    }

    return DefWindowProcW(Window, Message, wParam, lParam);
}

static
HFONT
CkMakeFont(
    _In_ int Height,
    _In_ int Weight)
{
    HFONT Font;

    Font = CreateFontW(Height, 0, 0, 0, Weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, L"Tahoma");
    return Font ? Font : (HFONT)GetStockObject(DEFAULT_GUI_FONT);
}

int
WINAPI
wWinMain(
    _In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPWSTR lpCmdLine,
    _In_ int nShowCmd)
{
    WNDCLASSEXW Class;
    HWND Existing, Window;
    BOOL Ring;
    MSG Msg;

    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(nShowCmd);

    CkInstance = hInstance;
    Ring = lpCmdLine && wcsstr(lpCmdLine, NATIVETIME_RING_SWITCH) != NULL;

    /* One clock at a time, a second start hands its ring request to the first */
    Existing = FindWindowW(CK_CLASS_NAME, NULL);
    if (Existing)
    {
        if (Ring)
            PostMessageW(Existing, WM_CLOCK_RING_CHECK, 0, 0);
        ShowWindow(Existing, SW_SHOWMAXIMIZED);
        SetForegroundWindow(Existing);
        return 0;
    }

    CkHugeFont = CkMakeFont(180, FW_LIGHT);
    CkBigFont = CkMakeFont(72, FW_SEMIBOLD);
    CkFont = CkMakeFont(40, FW_NORMAL);
    CkSmallFont = CkMakeFont(30, FW_NORMAL);
    CkHeadFont = CkMakeFont(52, FW_SEMIBOLD);
    CkButtonFont = CkMakeFont(38, FW_SEMIBOLD);

    CkBgBrush = CreateSolidBrush(CK_BG);
    CkFieldBrush = CreateSolidBrush(CK_FIELD);
    InitializeCriticalSection(&CkPipeLock);
    CkStopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!CkStopEvent)
        return 1;

    ZeroMemory(&Class, sizeof(Class));
    Class.cbSize = sizeof(Class);
    Class.lpfnWndProc = CkWndProc;
    Class.hInstance = hInstance;
    Class.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    Class.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_CLOCK));
    Class.hbrBackground = CkBgBrush;
    Class.lpszClassName = CK_CLASS_NAME;
    if (!RegisterClassExW(&Class))
        return 1;

    Window = CreateWindowExW(0, Class.lpszClassName, L"Clock", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                             CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                             NULL, NULL, hInstance, NULL);
    if (!Window)
        return 1;

    ShowWindow(Window, SW_SHOWMAXIMIZED);
    UpdateWindow(Window);

    if (Ring)
        CkCheckRinging();
    else
        CkRefresh(NULL);

    CkSubscribeThread = CreateThread(NULL, 0, CkSubscribeProc, NULL, 0, NULL);

    while (GetMessageW(&Msg, NULL, 0, 0) > 0)
    {
        TranslateMessage(&Msg);
        DispatchMessageW(&Msg);
    }

    /* A blocked read on the subscription ends when its handle is cancelled */
    SetEvent(CkStopEvent);
    EnterCriticalSection(&CkPipeLock);
    if (CkSubscribePipe)
        CancelIoEx(CkSubscribePipe, NULL);
    LeaveCriticalSection(&CkPipeLock);
    if (CkSubscribeThread)
    {
        WaitForSingleObject(CkSubscribeThread, 2000);
        CloseHandle(CkSubscribeThread);
    }

    DeleteObject(CkBgBrush);
    DeleteObject(CkFieldBrush);
    return (int)Msg.wParam;
}
