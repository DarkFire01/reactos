/*
 * PROJECT:     ReactOS Mobile Shell
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Launcher screen and the always-on-top bottom bar
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "mobileshell.h"

HINSTANCE MsInstance;
HWND MsLauncherWindow;
HWND MsBarWindow;

#define MS_TILE_COUNT       3
#define MS_TILE_FIRST_ID    1000
#define MS_EXPLORER_ID      1100
#define MS_CLOCK_TIMER      1
#define MS_TOPMOST_TIMER    2

typedef struct _MS_TILE
{
    UINT TitleId;
    PCWSTR Program;
    HWND Button;
    HICON Icon;
} MS_TILE;

static MS_TILE MsTiles[MS_TILE_COUNT] =
{
    { IDS_DEVICE_MANAGER, L"devmgmt.exe", NULL, NULL },
    { IDS_PHONE_TEST, L"phonetest.exe", NULL, NULL },
    { IDS_COMMAND_PROMPT, L"cmd.exe", NULL, NULL },
};

static INT MsScreenWidth;
static INT MsScreenHeight;
static INT MsBarHeight;
static HFONT MsTitleFont;
static HFONT MsClockFont;
static HFONT MsTileFont;
static HFONT MsBarFont;
static INT MsTileSize;
static HWND MsExplorerButton;
static BOOL MsBatteryPresent;
static ULONG MsBatteryPercent;
static BOOL MsBatteryCharging;
static BOOL MsBarPressed;

/**
 * @brief
 * Creates the system message font at a given pixel height and weight.
 */
HFONT
NTAPI
MsCreateFont(
    _In_ INT Height,
    _In_ INT Weight)
{
    NONCLIENTMETRICSW Metrics;
    LOGFONTW Font;

    Metrics.cbSize = sizeof(Metrics);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(Metrics), &Metrics, 0))
    {
        Font = Metrics.lfMessageFont;
    }
    else
    {
        ZeroMemory(&Font, sizeof(Font));
        StringCchCopyW(Font.lfFaceName, ARRAYSIZE(Font.lfFaceName), L"Tahoma");
    }

    Font.lfHeight = -Height;
    Font.lfWidth = 0;
    Font.lfWeight = Weight;
    Font.lfQuality = ANTIALIASED_QUALITY;
    return CreateFontIndirectW(&Font);
}

/**
 * @brief
 * Fills a rounded rectangle without an outline.
 */
VOID
NTAPI
MsFillRoundRect(
    _In_ HDC Dc,
    _In_ const RECT *Rect,
    _In_ INT Radius,
    _In_ COLORREF Color)
{
    HBRUSH Brush = CreateSolidBrush(Color);
    HGDIOBJ OldBrush = SelectObject(Dc, Brush);
    HGDIOBJ OldPen = SelectObject(Dc, GetStockObject(NULL_PEN));

    /* A null pen leaves the right and bottom edge unpainted, so grow by one */
    RoundRect(Dc, Rect->left, Rect->top, Rect->right + 1, Rect->bottom + 1, Radius, Radius);

    SelectObject(Dc, OldPen);
    SelectObject(Dc, OldBrush);
    DeleteObject(Brush);
}

/**
 * @brief
 * Keeps maximized windows clear of the bar and the keyboard, and fits the launcher into what is left.
 */
VOID
NTAPI
MsUpdateWorkArea(VOID)
{
    RECT Work;

    SetRect(&Work, 0, 0, MsScreenWidth, MsScreenHeight - MsBarHeight - MsKeyboardVisibleHeight());
    SystemParametersInfoW(SPI_SETWORKAREA, 0, &Work, SPIF_SENDCHANGE);

    if (MsLauncherWindow)
    {
        SetWindowPos(MsLauncherWindow,
                     NULL,
                     Work.left,
                     Work.top,
                     Work.right - Work.left,
                     Work.bottom - Work.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

static
VOID
NTAPI
MsLaunch(
    _In_ HWND Owner,
    _In_ PCWSTR Program)
{
    WCHAR CommandLine[MAX_PATH];
    WCHAR Format[128], Message[MAX_PATH + 128];
    STARTUPINFOW Startup;
    PROCESS_INFORMATION Process;
    UINT Length;

    Length = GetSystemDirectoryW(CommandLine, ARRAYSIZE(CommandLine));
    if (Length == 0 || Length >= ARRAYSIZE(CommandLine))
        return;

    StringCchCatW(CommandLine, ARRAYSIZE(CommandLine), L"\\");
    StringCchCatW(CommandLine, ARRAYSIZE(CommandLine), Program);

    ZeroMemory(&Startup, sizeof(Startup));
    Startup.cb = sizeof(Startup);
    if (CreateProcessW(NULL, CommandLine, NULL, NULL, FALSE, 0, NULL, NULL, &Startup, &Process))
    {
        CloseHandle(Process.hThread);
        CloseHandle(Process.hProcess);
        return;
    }

    LoadStringW(MsInstance, IDS_LAUNCH_FAILED, Format, ARRAYSIZE(Format));
    StringCchPrintfW(Message, ARRAYSIZE(Message), Format, Program, GetLastError());
    MessageBoxW(Owner, Message, NULL, MB_ICONERROR | MB_OK);
}

/* Large icons come straight from each program, the shell's own application icon stands in */
static
HICON
NTAPI
MsLoadProgramIcon(
    _In_ PCWSTR Program,
    _In_ INT Size)
{
    WCHAR Path[MAX_PATH];
    HICON Icon = NULL;
    UINT Length;

    Length = GetSystemDirectoryW(Path, ARRAYSIZE(Path));
    if (Length && Length < ARRAYSIZE(Path))
    {
        StringCchCatW(Path, ARRAYSIZE(Path), L"\\");
        StringCchCatW(Path, ARRAYSIZE(Path), Program);
        if (PrivateExtractIconsW(Path, 0, Size, Size, &Icon, NULL, 1, 0) != 1)
            Icon = NULL;
    }

    if (!Icon)
        Icon = (HICON)LoadImageW(NULL, (LPCWSTR)IDI_APPLICATION, IMAGE_ICON, Size, Size, LR_SHARED);

    return Icon;
}

/**
 * @brief
 * Starts Explorer as the shell and takes the emergency shell down.
 *
 * @remarks
 * The full screen work area is handed back first, Explorer sizes it around its own taskbar.
 */
static
VOID
NTAPI
MsSwitchToExplorer(
    _In_ HWND Owner)
{
    WCHAR CommandLine[MAX_PATH];
    WCHAR Format[128], Message[MAX_PATH + 128];
    STARTUPINFOW Startup;
    PROCESS_INFORMATION Process;
    RECT Screen;
    UINT Length;

    Length = GetWindowsDirectoryW(CommandLine, ARRAYSIZE(CommandLine));
    if (Length == 0 || Length >= ARRAYSIZE(CommandLine))
        return;

    StringCchCatW(CommandLine, ARRAYSIZE(CommandLine), L"\\explorer.exe");

    MsKeyboardDestroy();
    SetRect(&Screen, 0, 0, MsScreenWidth, MsScreenHeight);
    SystemParametersInfoW(SPI_SETWORKAREA, 0, &Screen, SPIF_SENDCHANGE);

    ZeroMemory(&Startup, sizeof(Startup));
    Startup.cb = sizeof(Startup);
    if (!CreateProcessW(NULL, CommandLine, NULL, NULL, FALSE, 0, NULL, NULL, &Startup, &Process))
    {
        LoadStringW(MsInstance, IDS_LAUNCH_FAILED, Format, ARRAYSIZE(Format));
        StringCchPrintfW(Message, ARRAYSIZE(Message), Format, L"explorer.exe", GetLastError());
        MessageBoxW(Owner, Message, NULL, MB_ICONERROR | MB_OK);

        /* Stay the shell when Explorer could not come up */
        MsKeyboardCreate(MsScreenWidth, MsScreenHeight - MsBarHeight);
        MsUpdateWorkArea();
        return;
    }

    CloseHandle(Process.hThread);
    CloseHandle(Process.hProcess);

    DestroyWindow(MsBarWindow);
    MsBarWindow = NULL;
    DestroyWindow(Owner);
}

static
VOID
NTAPI
MsDrawExplorerButton(
    _In_ const DRAWITEMSTRUCT *Item)
{
    WCHAR Label[32];
    RECT Rect = Item->rcItem;
    BOOL Pressed = (Item->itemState & ODS_SELECTED) != 0;
    HBRUSH Background = CreateSolidBrush(MS_COLOR_BACKGROUND);

    FillRect(Item->hDC, &Rect, Background);
    DeleteObject(Background);

    MsFillRoundRect(Item->hDC, &Rect, Rect.bottom - Rect.top, Pressed ? MS_COLOR_ACCENT : MS_COLOR_TILE);

    LoadStringW(MsInstance, IDS_EXPLORER, Label, ARRAYSIZE(Label));
    SetBkMode(Item->hDC, TRANSPARENT);
    SetTextColor(Item->hDC, Pressed ? MS_COLOR_TEXT : MS_COLOR_SUBTEXT);
    SelectObject(Item->hDC, MsClockFont);
    DrawTextW(Item->hDC, Label, -1, &Rect, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_END_ELLIPSIS);
}

static
VOID
NTAPI
MsDrawTile(
    _In_ const DRAWITEMSTRUCT *Item)
{
    const MS_TILE *Tile = &MsTiles[Item->CtlID - MS_TILE_FIRST_ID];
    WCHAR Title[64];
    RECT Rect = Item->rcItem, Text;
    INT Size = Rect.right - Rect.left, IconSize = Size / 2;
    BOOL Pressed = (Item->itemState & ODS_SELECTED) != 0;
    HBRUSH Background = CreateSolidBrush(MS_COLOR_BACKGROUND);

    FillRect(Item->hDC, &Rect, Background);
    DeleteObject(Background);

    MsFillRoundRect(Item->hDC, &Rect, Size / 6, Pressed ? MS_COLOR_ACCENT : MS_COLOR_TILE);

    if (Tile->Icon)
    {
        DrawIconEx(Item->hDC,
                   Rect.left + (Size - IconSize) / 2,
                   Rect.top + Size / 7,
                   Tile->Icon,
                   IconSize,
                   IconSize,
                   0,
                   NULL,
                   DI_NORMAL);
    }

    LoadStringW(MsInstance, Tile->TitleId, Title, ARRAYSIZE(Title));

    Text = Rect;
    Text.left += Size / 12;
    Text.right -= Size / 12;
    Text.top = Rect.top + Size / 7 + IconSize;
    Text.bottom = Rect.bottom - Size / 14;
    SetBkMode(Item->hDC, TRANSPARENT);
    SetTextColor(Item->hDC, MS_COLOR_TEXT);
    SelectObject(Item->hDC, MsTileFont);
    DrawTextW(Item->hDC, Title, -1, &Text, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_END_ELLIPSIS);
}

/* Title and clock share a band across the top */
static
RECT
NTAPI
MsTitleRect(
    _In_ const RECT *Client)
{
    RECT Title = *Client;
    INT Margin = min(Client->right, Client->bottom) / 16;

    Title.left += Margin;
    Title.right -= Margin;
    Title.top += Margin;
    Title.bottom = Title.top + MsTileSize / 2;
    return Title;
}

/* The title takes the top, square tiles fill a grid below it, two across in portrait */
static
VOID
NTAPI
MsLayoutLauncher(
    _In_ HWND Window)
{
    RECT Client;
    INT Columns, Margin, Gap, Header, Width, Left, Top, i;

    GetClientRect(Window, &Client);

    Columns = (Client.right > Client.bottom) ? MS_TILE_COUNT : 2;
    Margin = min(Client.right, Client.bottom) / 16;
    Gap = Margin / 2;
    Header = MsTitleRect(&Client).bottom + Margin;
    Width = Client.right - Margin * 2;

    for (i = 0; i < MS_TILE_COUNT; i++)
    {
        Left = Margin + (i % Columns) * (MsTileSize + Gap) + (Width - Columns * MsTileSize - (Columns - 1) * Gap) / 2;
        Top = Header + (i / Columns) * (MsTileSize + Gap);
        MoveWindow(MsTiles[i].Button, Left, Top, MsTileSize, MsTileSize, TRUE);
    }

    MoveWindow(MsExplorerButton,
               Client.right - Margin - MsTileSize / 2,
               Client.bottom - Margin - MsTileSize / 7,
               MsTileSize / 2,
               MsTileSize / 7,
               TRUE);
}

/* Outline with a terminal nub, filled to the charge, accent while charging */
static
VOID
NTAPI
MsDrawBattery(
    _In_ HDC Dc,
    _In_ const RECT *Body)
{
    INT Stroke = max(2, (Body->bottom - Body->top) / 8);
    RECT Inner = *Body, Nub, Level;
    COLORREF Fill;

    MsFillRoundRect(Dc, Body, Stroke * 2, MS_COLOR_SUBTEXT);
    InflateRect(&Inner, -Stroke, -Stroke);
    MsFillRoundRect(Dc, &Inner, Stroke, MS_COLOR_BACKGROUND);

    Nub.left = Body->right;
    Nub.right = Body->right + Stroke * 2;
    Nub.top = Body->top + (Body->bottom - Body->top) / 3;
    Nub.bottom = Body->bottom - (Body->bottom - Body->top) / 3;
    MsFillRoundRect(Dc, &Nub, Stroke, MS_COLOR_SUBTEXT);

    if (MsBatteryCharging)
        Fill = MS_COLOR_ACCENT;
    else if (MsBatteryPercent <= 15)
        Fill = RGB(0xE8, 0x4A, 0x4A);
    else
        Fill = RGB(0x5C, 0xC8, 0x7A);

    Level = Inner;
    InflateRect(&Level, -Stroke, -Stroke);
    Level.right = Level.left + (Level.right - Level.left) * (LONG)MsBatteryPercent / 100;
    if (Level.right > Level.left)
        MsFillRoundRect(Dc, &Level, Stroke, Fill);
}

/* Clock on the left and battery on the right of a row above the title */
static
VOID
NTAPI
MsPaintLauncher(
    _In_ HWND Window)
{
    WCHAR Title[64], Clock[32], Date[64], Line[128], Level[16];
    PAINTSTRUCT Paint;
    RECT Client, Band, Row, Text, Body;
    HBRUSH Background;
    SIZE Extent;
    INT Height;
    HDC Dc = BeginPaint(Window, &Paint);

    GetClientRect(Window, &Client);
    Background = CreateSolidBrush(MS_COLOR_BACKGROUND);
    FillRect(Dc, &Paint.rcPaint, Background);
    DeleteObject(Background);

    Band = MsTitleRect(&Client);
    Row = Band;
    Row.bottom = Band.top + (Band.bottom - Band.top) * 2 / 5;

    SetBkMode(Dc, TRANSPARENT);
    SelectObject(Dc, MsClockFont);
    SetTextColor(Dc, MS_COLOR_SUBTEXT);

    GetTimeFormatW(LOCALE_USER_DEFAULT, TIME_NOSECONDS, NULL, NULL, Clock, ARRAYSIZE(Clock));
    GetDateFormatW(LOCALE_USER_DEFAULT, DATE_SHORTDATE, NULL, NULL, Date, ARRAYSIZE(Date));
    StringCchPrintfW(Line, ARRAYSIZE(Line), L"%s  %s", Clock, Date);
    DrawTextW(Dc, Line, -1, &Row, DT_SINGLELINE | DT_LEFT | DT_VCENTER);

    if (MsBatteryPresent)
    {
        StringCchPrintfW(Level, ARRAYSIZE(Level), L"%lu%%", MsBatteryPercent);
        GetTextExtentPoint32W(Dc, Level, (INT)wcslen(Level), &Extent);

        Text = Row;
        Text.left = Text.right - Extent.cx;
        DrawTextW(Dc, Level, -1, &Text, DT_SINGLELINE | DT_RIGHT | DT_VCENTER);

        Height = Extent.cy * 3 / 5;
        Body.right = Text.left - Height;
        Body.left = Body.right - Height * 2;
        Body.top = Row.top + (Row.bottom - Row.top - Height) / 2;
        Body.bottom = Body.top + Height;
        MsDrawBattery(Dc, &Body);
    }

    LoadStringW(MsInstance, IDS_SHELL_TITLE, Title, ARRAYSIZE(Title));
    Text = Band;
    Text.top = Row.bottom;
    SetTextColor(Dc, MS_COLOR_TEXT);
    SelectObject(Dc, MsTitleFont);
    DrawTextW(Dc, Title, -1, &Text, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_END_ELLIPSIS);

    EndPaint(Window, &Paint);
}

static
LRESULT
CALLBACK
MsLauncherProc(
    _In_ HWND Window,
    _In_ UINT Message,
    _In_ WPARAM WParam,
    _In_ LPARAM LParam)
{
    INT i;

    switch (Message)
    {
        case WM_CREATE:
            for (i = 0; i < MS_TILE_COUNT; i++)
            {
                MsTiles[i].Icon = MsLoadProgramIcon(MsTiles[i].Program, MsTileSize / 2);
                MsTiles[i].Button = CreateWindowExW(0,
                                                    L"BUTTON",
                                                    MsTiles[i].Program,
                                                    WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                                    0, 0, 0, 0,
                                                    Window,
                                                    (HMENU)(UINT_PTR)(MS_TILE_FIRST_ID + i),
                                                    MsInstance,
                                                    NULL);
            }
            MsExplorerButton = CreateWindowExW(0,
                                               L"BUTTON",
                                               L"explorer.exe",
                                               WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                               0, 0, 0, 0,
                                               Window,
                                               (HMENU)(UINT_PTR)MS_EXPLORER_ID,
                                               MsInstance,
                                               NULL);
            MsBatteryPresent = MsBatteryRead(&MsBatteryPercent, &MsBatteryCharging);
            SetTimer(Window, MS_CLOCK_TIMER, 1000, NULL);
            return 0;

        case WM_SIZE:
            MsLayoutLauncher(Window);
            InvalidateRect(Window, NULL, TRUE);
            return 0;

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT:
            MsPaintLauncher(Window);
            return 0;

        case WM_DRAWITEM:
            if (((const DRAWITEMSTRUCT *)LParam)->CtlID == MS_EXPLORER_ID)
                MsDrawExplorerButton((const DRAWITEMSTRUCT *)LParam);
            else
                MsDrawTile((const DRAWITEMSTRUCT *)LParam);
            return TRUE;

        case WM_COMMAND:
            if (HIWORD(WParam) == BN_CLICKED && LOWORD(WParam) == MS_EXPLORER_ID)
            {
                MsSwitchToExplorer(Window);
                return 0;
            }

            i = LOWORD(WParam) - MS_TILE_FIRST_ID;
            if (HIWORD(WParam) == BN_CLICKED && i >= 0 && i < MS_TILE_COUNT)
                MsLaunch(Window, MsTiles[i].Program);
            return 0;

        case WM_TIMER:
            if (WParam == MS_CLOCK_TIMER)
            {
                RECT Client, Header;

                MsBatteryPresent = MsBatteryRead(&MsBatteryPercent, &MsBatteryCharging);
                GetClientRect(Window, &Client);
                Header = MsTitleRect(&Client);
                InvalidateRect(Window, &Header, FALSE);
            }
            return 0;

        /* The shell stays up for as long as the session does */
        case WM_CLOSE:
            return 0;

        case WM_DESTROY:
            KillTimer(Window, MS_CLOCK_TIMER);
            PostQuitMessage(0);
            return 0;
    }

    return DefWindowProcW(Window, Message, WParam, LParam);
}

static
VOID
NTAPI
MsGetBarButtonRect(
    _In_ HWND Window,
    _Out_ RECT *Button)
{
    RECT Client;
    INT Width, Height;

    GetClientRect(Window, &Client);
    Height = (Client.bottom - Client.top) * 3 / 5;
    Width = MsScreenWidth / 3;
    Button->left = (Client.right - Width) / 2;
    Button->right = Button->left + Width;
    Button->top = (Client.bottom - Height) / 2;
    Button->bottom = Button->top + Height;
}

/* A small key grid drawn left of the label */
static
VOID
NTAPI
MsDrawKeyboardGlyph(
    _In_ HDC Dc,
    _In_ const RECT *Area,
    _In_ COLORREF Color)
{
    INT Unit = (Area->bottom - Area->top) / 7;
    INT Columns = 4, Row, Column;
    RECT Key;

    for (Row = 0; Row < 2; Row++)
    {
        for (Column = 0; Column < Columns; Column++)
        {
            Key.left = Area->left + Column * Unit * 2;
            Key.top = Area->top + Unit + Row * Unit * 2;
            Key.right = Key.left + Unit + Unit / 2;
            Key.bottom = Key.top + Unit + Unit / 2;
            MsFillRoundRect(Dc, &Key, Unit / 2, Color);
        }
    }

    Key.left = Area->left + Unit * 2;
    Key.top = Area->top + Unit * 5;
    Key.right = Key.left + Unit * 3 + Unit / 2;
    Key.bottom = Key.top + Unit + Unit / 2;
    MsFillRoundRect(Dc, &Key, Unit / 2, Color);
}

static
VOID
NTAPI
MsPaintBar(
    _In_ HWND Window)
{
    WCHAR Label[32];
    PAINTSTRUCT Paint;
    RECT Client, Button, Glyph, Text;
    HBRUSH Background;
    COLORREF Fill;
    HDC Dc = BeginPaint(Window, &Paint);

    GetClientRect(Window, &Client);
    Background = CreateSolidBrush(MS_COLOR_BAR);
    FillRect(Dc, &Client, Background);
    DeleteObject(Background);

    MsGetBarButtonRect(Window, &Button);
    Fill = (MsBarPressed || MsKeyboardVisibleHeight()) ? MS_COLOR_ACCENT : MS_COLOR_TILE;
    MsFillRoundRect(Dc, &Button, Button.bottom - Button.top, Fill);

    Glyph = Button;
    InflateRect(&Glyph, 0, -(Button.bottom - Button.top) / 5);
    Glyph.left += (Button.bottom - Button.top) / 2;
    Glyph.right = Glyph.left + (Glyph.bottom - Glyph.top) * 8 / 7;
    MsDrawKeyboardGlyph(Dc, &Glyph, MS_COLOR_TEXT);

    LoadStringW(MsInstance, IDS_KEYBOARD, Label, ARRAYSIZE(Label));
    Text = Button;
    Text.left = Glyph.right;
    SetBkMode(Dc, TRANSPARENT);
    SetTextColor(Dc, MS_COLOR_TEXT);
    SelectObject(Dc, MsBarFont);
    DrawTextW(Dc, Label, -1, &Text, DT_SINGLELINE | DT_CENTER | DT_VCENTER);

    EndPaint(Window, &Paint);
}

static
LRESULT
CALLBACK
MsBarProc(
    _In_ HWND Window,
    _In_ UINT Message,
    _In_ WPARAM WParam,
    _In_ LPARAM LParam)
{
    POINT Point;
    RECT Button;

    switch (Message)
    {
        case WM_CREATE:
            SetTimer(Window, MS_TOPMOST_TIMER, MS_TOPMOST_INTERVAL_MS, NULL);
            return 0;

        /* Taps must leave the focus with the window being typed into */
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;

        case WM_LBUTTONDOWN:
            Point.x = (SHORT)LOWORD(LParam);
            Point.y = (SHORT)HIWORD(LParam);
            MsGetBarButtonRect(Window, &Button);
            if (PtInRect(&Button, Point))
            {
                MsBarPressed = TRUE;
                SetCapture(Window);
                InvalidateRect(Window, NULL, FALSE);
            }
            return 0;

        case WM_LBUTTONUP:
            if (!MsBarPressed)
                return 0;

            MsBarPressed = FALSE;
            ReleaseCapture();
            Point.x = (SHORT)LOWORD(LParam);
            Point.y = (SHORT)HIWORD(LParam);
            MsGetBarButtonRect(Window, &Button);
            if (PtInRect(&Button, Point))
                MsKeyboardToggle();

            InvalidateRect(Window, NULL, FALSE);
            return 0;

        case WM_CAPTURECHANGED:
            if (MsBarPressed)
            {
                MsBarPressed = FALSE;
                InvalidateRect(Window, NULL, FALSE);
            }
            return 0;

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT:
            MsPaintBar(Window);
            return 0;

        /* Windows made topmost later would otherwise cover the bar */
        case WM_TIMER:
            if (WParam == MS_TOPMOST_TIMER)
            {
                SetWindowPos(Window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                MsKeyboardKeepOnTop();
            }
            return 0;

        /* Nothing is allowed to close the bar */
        case WM_CLOSE:
            return 0;

        case WM_SYSCOMMAND:
            if ((WParam & 0xFFF0) == SC_CLOSE)
                return 0;
            break;
    }

    return DefWindowProcW(Window, Message, WParam, LParam);
}

static
BOOL
NTAPI
MsRegisterClasses(VOID)
{
    WNDCLASSEXW Class;

    ZeroMemory(&Class, sizeof(Class));
    Class.cbSize = sizeof(Class);
    Class.hInstance = MsInstance;
    Class.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);

    Class.lpfnWndProc = MsLauncherProc;
    Class.lpszClassName = MS_LAUNCHER_CLASS;
    if (!RegisterClassExW(&Class))
        return FALSE;

    Class.lpfnWndProc = MsBarProc;
    Class.lpszClassName = MS_BAR_CLASS;
    if (!RegisterClassExW(&Class))
        return FALSE;

    return MsKeyboardRegister();
}

static
VOID
NTAPI
MsCreateFonts(VOID)
{
    MsTitleFont = MsCreateFont(MsTileSize / 9, FW_SEMIBOLD);
    MsClockFont = MsCreateFont(MsTileSize / 10, FW_NORMAL);
    MsTileFont = MsCreateFont(MsTileSize / 9, FW_SEMIBOLD);
    MsBarFont = MsCreateFont(MsBarHeight / 4, FW_SEMIBOLD);
}

INT
WINAPI
wWinMain(
    _In_ HINSTANCE Instance,
    _In_opt_ HINSTANCE PrevInstance,
    _In_ LPWSTR CommandLine,
    _In_ INT ShowCommand)
{
    WCHAR Title[64];
    HANDLE Mutex;
    MSG Message;

    UNREFERENCED_PARAMETER(PrevInstance);
    UNREFERENCED_PARAMETER(CommandLine);
    UNREFERENCED_PARAMETER(ShowCommand);

    MsInstance = Instance;

    /* One shell per session */
    Mutex = CreateMutexW(NULL, TRUE, L"ReactOSMobileShell");
    if (Mutex && GetLastError() == ERROR_ALREADY_EXISTS)
        return 0;

    MsScreenWidth = GetSystemMetrics(SM_CXSCREEN);
    MsScreenHeight = GetSystemMetrics(SM_CYSCREEN);
    MsBarHeight = max(48, min(MsScreenWidth, MsScreenHeight) / 7);

    /* Two tiles across a portrait screen, three across a landscape one */
    MsTileSize = (MsScreenWidth > MsScreenHeight) ?
                 min(MsScreenWidth * 6 / 25, (MsScreenHeight - MsBarHeight) / 2) :
                 MsScreenWidth * 13 / 32;

    if (!MsRegisterClasses())
        return 1;

    MsCreateFonts();
    LoadStringW(MsInstance, IDS_SHELL_TITLE, Title, ARRAYSIZE(Title));

    MsBarWindow = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                  MS_BAR_CLASS,
                                  Title,
                                  WS_POPUP,
                                  0,
                                  MsScreenHeight - MsBarHeight,
                                  MsScreenWidth,
                                  MsBarHeight,
                                  NULL,
                                  NULL,
                                  MsInstance,
                                  NULL);
    if (!MsBarWindow)
        return 1;

    MsKeyboardCreate(MsScreenWidth, MsScreenHeight - MsBarHeight);

    MsLauncherWindow = CreateWindowExW(0,
                                       MS_LAUNCHER_CLASS,
                                       Title,
                                       WS_POPUP | WS_CLIPCHILDREN,
                                       0,
                                       0,
                                       MsScreenWidth,
                                       MsScreenHeight - MsBarHeight,
                                       NULL,
                                       NULL,
                                       MsInstance,
                                       NULL);
    if (!MsLauncherWindow)
        return 1;

    MsUpdateWorkArea();
    ShowWindow(MsLauncherWindow, SW_SHOW);
    ShowWindow(MsBarWindow, SW_SHOWNOACTIVATE);
    UpdateWindow(MsLauncherWindow);

    while (GetMessageW(&Message, NULL, 0, 0) > 0)
    {
        TranslateMessage(&Message);
        DispatchMessageW(&Message);
    }

    return (INT)Message.wParam;
}
