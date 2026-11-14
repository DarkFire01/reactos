/*
 * PROJECT:     ReactOS Mobile Shell
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Full screen dark surface that shows touch input
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <windef.h>
#include <winbase.h>
#include <wingdi.h>
#include <winuser.h>
#include <strsafe.h>

#include "resource.h"

/* Exported by user32 without a public prototype */
BOOL WINAPI SetShellWindow(_In_ HWND hwnd);

#define SHELL_CLASS_NAME        L"ReactOSMobileShell"
#define SHELL_MAX_CONTACTS      16
#define SHELL_STATUS_HEIGHT     32
#define SHELL_CONTACT_RADIUS    48
#define SHELL_CLOCK_TIMER       1

/* Mouse messages promoted from touch carry this signature */
#define SHELL_PROMOTED_MASK     0xFFFFFF80
#define SHELL_PROMOTED_TOUCH    0xFF515780

#define SHELL_BACKGROUND        RGB(0, 0, 0)
#define SHELL_ACCENT            RGB(0x1B, 0xA1, 0xE2)
#define SHELL_TEXT              RGB(0xFF, 0xFF, 0xFF)
#define SHELL_DIM_TEXT          RGB(0x80, 0x80, 0x80)

typedef struct _SHELL_CONTACT
{
    BOOL Active;
    DWORD Id;
    POINT Point;
} SHELL_CONTACT;

static SHELL_CONTACT Contacts[SHELL_MAX_CONTACTS];
static WCHAR LastEvent[128] = L"Touch the screen";
static ULONG TouchMessages;
static ULONG PromotedClicks;
static POINT LastMouse = { -1, -1 };
static HFONT StatusFont;
static HFONT LargeFont;

static
SHELL_CONTACT *
ShellFindContact(
    _In_ DWORD Id,
    _In_ BOOL Create)
{
    ULONG Index;

    for (Index = 0; Index < SHELL_MAX_CONTACTS; Index++)
    {
        if (Contacts[Index].Active && Contacts[Index].Id == Id)
            return &Contacts[Index];
    }

    if (!Create)
        return NULL;

    for (Index = 0; Index < SHELL_MAX_CONTACTS; Index++)
    {
        if (!Contacts[Index].Active)
        {
            Contacts[Index].Active = TRUE;
            Contacts[Index].Id = Id;
            return &Contacts[Index];
        }
    }

    return NULL;
}

/**
 * @brief Tracks every contact a WM_TOUCH reports and closes the handle.
 */
static
VOID
ShellOnTouch(
    _In_ HWND hwnd,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam)
{
    TOUCHINPUT Inputs[SHELL_MAX_CONTACTS];
    UINT Count = min(LOWORD(wParam), SHELL_MAX_CONTACTS);
    SHELL_CONTACT *Contact;
    POINT Point;
    UINT Index;

    TouchMessages++;

    if (!GetTouchInputInfo((HTOUCHINPUT)lParam, Count, Inputs, sizeof(TOUCHINPUT)))
    {
        StringCchPrintfW(LastEvent, ARRAYSIZE(LastEvent), L"GetTouchInputInfo failed, error %lu", GetLastError());
        InvalidateRect(hwnd, NULL, TRUE);
        return;
    }

    for (Index = 0; Index < Count; Index++)
    {
        Point.x = TOUCH_COORD_TO_PIXEL(Inputs[Index].x);
        Point.y = TOUCH_COORD_TO_PIXEL(Inputs[Index].y);
        ScreenToClient(hwnd, &Point);

        Contact = ShellFindContact(Inputs[Index].dwID, (Inputs[Index].dwFlags & TOUCHEVENTF_UP) == 0);
        if (!Contact)
            continue;

        Contact->Point = Point;
        if (Inputs[Index].dwFlags & TOUCHEVENTF_UP)
            Contact->Active = FALSE;

        StringCchPrintfW(LastEvent, ARRAYSIZE(LastEvent), L"WM_TOUCH id %lu %s at %ld, %ld",
                         Inputs[Index].dwID,
                         (Inputs[Index].dwFlags & TOUCHEVENTF_DOWN) ? L"down" :
                         (Inputs[Index].dwFlags & TOUCHEVENTF_UP) ? L"up" : L"move",
                         Point.x, Point.y);
    }

    CloseTouchInputHandle((HTOUCHINPUT)lParam);
    InvalidateRect(hwnd, NULL, TRUE);
}

static
VOID
ShellDrawText(
    _In_ HDC hdc,
    _In_ HFONT Font,
    _In_ COLORREF Color,
    _In_ LONG X,
    _In_ LONG Y,
    _In_z_ PCWSTR Text)
{
    HGDIOBJ OldFont = SelectObject(hdc, Font);

    SetTextColor(hdc, Color);
    TextOutW(hdc, X, Y, Text, lstrlenW(Text));
    SelectObject(hdc, OldFont);
}

static
VOID
ShellPaint(
    _In_ HWND hwnd,
    _In_ HDC hdc)
{
    WCHAR Text[96];
    SYSTEMTIME Time;
    RECT Client;
    HBRUSH Accent;
    HGDIOBJ OldBrush;
    HGDIOBJ OldPen;
    ULONG Index;
    ULONG Active = 0;

    GetClientRect(hwnd, &Client);
    SetBkMode(hdc, TRANSPARENT);

    /* Status bar: the clock sits on the right like the phone shell */
    GetLocalTime(&Time);
    StringCchPrintfW(Text, ARRAYSIZE(Text), L"%u:%02u", Time.wHour, Time.wMinute);
    ShellDrawText(hdc, StatusFont, SHELL_TEXT, Client.right - 80, 6, Text);

    Accent = CreateSolidBrush(SHELL_ACCENT);
    OldBrush = SelectObject(hdc, Accent);
    OldPen = SelectObject(hdc, GetStockObject(NULL_PEN));

    for (Index = 0; Index < SHELL_MAX_CONTACTS; Index++)
    {
        if (!Contacts[Index].Active)
            continue;

        Active++;
        Ellipse(hdc,
                Contacts[Index].Point.x - SHELL_CONTACT_RADIUS,
                Contacts[Index].Point.y - SHELL_CONTACT_RADIUS,
                Contacts[Index].Point.x + SHELL_CONTACT_RADIUS,
                Contacts[Index].Point.y + SHELL_CONTACT_RADIUS);

        StringCchPrintfW(Text, ARRAYSIZE(Text), L"%lu", Contacts[Index].Id);
        ShellDrawText(hdc, StatusFont, SHELL_TEXT,
                      Contacts[Index].Point.x - 8, Contacts[Index].Point.y - 10, Text);
    }

    SelectObject(hdc, OldPen);
    SelectObject(hdc, OldBrush);
    DeleteObject(Accent);

    /* A small outline marks where the promoted mouse landed */
    if (LastMouse.x >= 0)
    {
        SelectObject(hdc, GetStockObject(WHITE_PEN));
        SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Rectangle(hdc, LastMouse.x - 6, LastMouse.y - 6, LastMouse.x + 6, LastMouse.y + 6);
    }

    ShellDrawText(hdc, LargeFont, SHELL_TEXT, 24, SHELL_STATUS_HEIGHT + 24, L"touch");

    StringCchPrintfW(Text, ARRAYSIZE(Text), L"%lu contacts, %lu WM_TOUCH, %lu promoted clicks",
                     Active, TouchMessages, PromotedClicks);
    ShellDrawText(hdc, StatusFont, SHELL_DIM_TEXT, 24, Client.bottom - 72, Text);
    ShellDrawText(hdc, StatusFont, SHELL_ACCENT, 24, Client.bottom - 44, LastEvent);
}

static
BOOL
ShellIsPromotedMouse(VOID)
{
    return ((ULONG)GetMessageExtraInfo() & SHELL_PROMOTED_MASK) == SHELL_PROMOTED_TOUCH;
}

static
LRESULT
CALLBACK
ShellWindowProc(
    _In_ HWND hwnd,
    _In_ UINT uMsg,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam)
{
    PAINTSTRUCT Paint;
    HDC hdc;

    switch (uMsg)
    {
        case WM_CREATE:
            if (!RegisterTouchWindow(hwnd, 0))
                StringCchPrintfW(LastEvent, ARRAYSIZE(LastEvent), L"RegisterTouchWindow failed, error %lu", GetLastError());
            SetTimer(hwnd, SHELL_CLOCK_TIMER, 30 * 1000, NULL);
            return 0;

        case WM_TOUCH:
            ShellOnTouch(hwnd, wParam, lParam);
            return 0;

        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
            LastMouse.x = (SHORT)LOWORD(lParam);
            LastMouse.y = (SHORT)HIWORD(lParam);
            if (uMsg == WM_LBUTTONDOWN && ShellIsPromotedMouse())
                PromotedClicks++;
            InvalidateRect(hwnd, NULL, TRUE);
            return 0;

        case WM_TIMER:
            InvalidateRect(hwnd, NULL, TRUE);
            return 0;

        case WM_ERASEBKGND:
        {
            RECT Client;

            GetClientRect(hwnd, &Client);
            FillRect((HDC)wParam, &Client, GetStockObject(BLACK_BRUSH));
            return 1;
        }

        case WM_PAINT:
            hdc = BeginPaint(hwnd, &Paint);
            ShellPaint(hwnd, hdc);
            EndPaint(hwnd, &Paint);
            return 0;

        case WM_DESTROY:
            UnregisterTouchWindow(hwnd);
            KillTimer(hwnd, SHELL_CLOCK_TIMER);
            PostQuitMessage(0);
            return 0;
    }

    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

static
HFONT
ShellCreateFont(
    _In_ LONG Height,
    _In_ LONG Weight)
{
    LOGFONTW Font;

    ZeroMemory(&Font, sizeof(Font));
    Font.lfHeight = -Height;
    Font.lfWeight = Weight;
    Font.lfQuality = CLEARTYPE_QUALITY;
    StringCchCopyW(Font.lfFaceName, ARRAYSIZE(Font.lfFaceName), L"Segoe UI");
    return CreateFontIndirectW(&Font);
}

INT
WINAPI
wWinMain(
    _In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPWSTR lpCmdLine,
    _In_ INT nShowCmd)
{
    WCHAR Title[64];
    WNDCLASSEXW Class;
    HWND hwnd;
    MSG Msg;

    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);
    UNREFERENCED_PARAMETER(nShowCmd);

    if (!LoadStringW(hInstance, IDS_SHELL_TITLE, Title, ARRAYSIZE(Title)))
        StringCchCopyW(Title, ARRAYSIZE(Title), L"ReactOS Mobile");

    StatusFont = ShellCreateFont(18, FW_NORMAL);
    LargeFont = ShellCreateFont(64, FW_LIGHT);

    ZeroMemory(&Class, sizeof(Class));
    Class.cbSize = sizeof(Class);
    Class.lpfnWndProc = ShellWindowProc;
    Class.hInstance = hInstance;
    Class.hCursor = LoadCursorW(NULL, IDC_ARROW);
    Class.hbrBackground = GetStockObject(BLACK_BRUSH);
    Class.lpszClassName = SHELL_CLASS_NAME;
    if (!RegisterClassExW(&Class))
        return 1;

    hwnd = CreateWindowExW(WS_EX_TOPMOST,
                           SHELL_CLASS_NAME,
                           Title,
                           WS_POPUP | WS_VISIBLE,
                           0,
                           0,
                           GetSystemMetrics(SM_CXSCREEN),
                           GetSystemMetrics(SM_CYSCREEN),
                           NULL,
                           NULL,
                           hInstance,
                           NULL);
    if (!hwnd)
        return 1;

    SetShellWindow(hwnd);

    while (GetMessageW(&Msg, NULL, 0, 0) > 0)
    {
        TranslateMessage(&Msg);
        DispatchMessageW(&Msg);
    }

    DeleteObject(StatusFont);
    DeleteObject(LargeFont);
    return (INT)Msg.wParam;
}
