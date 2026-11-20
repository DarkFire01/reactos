/*
 * PROJECT:     ReactOS Browser
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Touch browser window, address bar, history and page scrolling
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "webbrowser.h"
#include <windowsx.h>

#define WM_BR_LOADED            (WM_APP + 1)

/* Dark theme shared with the mobile shell */
#define BR_BG                   RGB(0x12, 0x12, 0x14)
#define BR_TILE                 RGB(0x24, 0x25, 0x2A)
#define BR_ACCENT               RGB(0x3A, 0x96, 0xDD)
#define BR_TEXT                 RGB(0xF2, 0xF2, 0xF4)
#define BR_SUBTEXT              RGB(0x9A, 0x9B, 0xA3)
#define BR_FIELD                RGB(0x1A, 0x1A, 0x1E)
#define BR_DIM                  RGB(0x3A, 0x3B, 0x42)

/* Touch sized layout in pixels, the top row stays clear of the camera cutout */
#define BR_MARGIN               20
#define BR_SAFE_TOP             120
#define BR_GAP                  12
#define BR_ROW                  88
#define BR_STATUS_H             48

/* Pages lay out at about this many CSS pixels across, then scale up */
#define BR_CSS_WIDTH            400
#define BR_DRAG_SLOP            16
#define BR_WHEEL_STEP           60
#define BR_HISTORY_MAX          64
#define BR_ADDRESS_MAX          1024

enum
{
    IDC_BACK = 100,
    IDC_ADDRESS,
    IDC_GO,
    IDC_HOME,
    IDC_RELOAD,
    IDC_EXIT
};

typedef enum _BR_HISTORY_ACTION
{
    BrHistoryPush,
    BrHistoryMove,
    BrHistoryStay
} BR_HISTORY_ACTION;

/* One navigation for the loader thread, posted back with the page it built */
typedef struct _BR_JOB
{
    PSTR Url;
    ULONG Width;
    ULONG Height;
    BR_HISTORY_ACTION Action;
    ULONG HistoryIndex;
    PBR_PAGE Page;
    DWORD Error;
} BR_JOB, *PBR_JOB;

static HINSTANCE BrInstance;
static HWND BrWindow;
static HFONT BrFont;
static HFONT BrSmallFont;
static HFONT BrButtonFont;
static HBRUSH BrBgBrush;
static HBRUSH BrFieldBrush;

static PBR_PAGE BrPage;
static BOOL BrLoading;
static PSTR BrLoadingUrl;
static LONG BrScroll;
static LONG BrZoom = 1;
static RECT BrContent;

static PSTR BrHistory[BR_HISTORY_MAX];
static ULONG BrHistoryCount;
static ULONG BrHistoryIndex;

static BOOL BrPressed;
static BOOL BrDragging;
static POINT BrPressPoint;
static LONG BrPressScroll;

static
VOID
BrFillRound(
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
BrWiden(
    _In_z_ PCSTR Utf8,
    _Out_writes_(Count) PWSTR Text,
    _In_ DWORD Count)
{
    if (!MultiByteToWideChar(CP_UTF8, 0, Utf8, -1, Text, (int)Count))
        Text[0] = UNICODE_NULL;
    Text[Count - 1] = UNICODE_NULL;
}

static
VOID
BrInvalidateContent(VOID)
{
    InvalidateRect(BrWindow, &BrContent, FALSE);
}

static
VOID
BrInvalidateStatus(VOID)
{
    RECT Client, Status;

    GetClientRect(BrWindow, &Client);
    Status.left = 0;
    Status.right = Client.right;
    Status.top = BrContent.bottom;
    Status.bottom = Client.bottom;
    InvalidateRect(BrWindow, &Status, FALSE);
}

static
LONG
BrMaxScroll(VOID)
{
    LONG Visible;

    if (!BrPage)
        return 0;

    Visible = (BrContent.bottom - BrContent.top) / BrZoom;
    return max((LONG)BrPage->Canvas.Height - Visible, 0);
}

static
VOID
BrScrollTo(
    _In_ LONG Scroll)
{
    Scroll = max(min(Scroll, BrMaxScroll()), 0);
    if (Scroll == BrScroll)
        return;

    BrScroll = Scroll;
    BrInvalidateContent();
}

static
VOID
BrViewport(
    _Out_ PULONG Width,
    _Out_ PULONG Height)
{
    *Width = (ULONG)max((BrContent.right - BrContent.left) / BrZoom, 8);
    *Height = (ULONG)max((BrContent.bottom - BrContent.top) / BrZoom, 8);
}

static
VOID
BrUpdateControls(VOID)
{
    WCHAR Address[BR_ADDRESS_MAX];
    HWND Edit = GetDlgItem(BrWindow, IDC_ADDRESS);

    EnableWindow(GetDlgItem(BrWindow, IDC_BACK), !BrLoading && BrHistoryIndex > 0);
    EnableWindow(GetDlgItem(BrWindow, IDC_RELOAD), !BrLoading && BrPage);
    SetDlgItemTextW(BrWindow, IDC_GO, BrLoading ? L"..." : L"Go");

    /* Leave the address alone while it is being typed */
    if (GetFocus() != Edit && BrPage && BrPage->Url)
    {
        if (!_stricmp(BrPage->Url, BR_HOME_URL))
            Address[0] = UNICODE_NULL;
        else
            BrWiden(BrPage->Url, Address, ARRAYSIZE(Address));
        SetWindowTextW(Edit, Address);
    }

    BrInvalidateStatus();
}

static
DWORD
WINAPI
BrLoader(
    _In_ LPVOID Parameter)
{
    PBR_JOB Job = Parameter;

    Job->Error = BrPageLoad(Job->Url, Job->Width, Job->Height, &Job->Page);

    if (!PostMessageW(BrWindow, WM_BR_LOADED, 0, (LPARAM)Job))
    {
        BrPageDestroy(Job->Page);
        free(Job->Url);
        free(Job);
    }
    return 0;
}

static
VOID
BrNavigate(
    _In_z_ PCSTR Url,
    _In_ BR_HISTORY_ACTION Action,
    _In_ ULONG HistoryIndex)
{
    PBR_JOB Job;
    HANDLE Thread;

    if (BrLoading)
        return;

    Job = calloc(1, sizeof(*Job));
    if (!Job)
        return;

    Job->Url = BrStringDuplicate(Url);
    Job->Action = Action;
    Job->HistoryIndex = HistoryIndex;
    BrViewport(&Job->Width, &Job->Height);
    if (!Job->Url)
    {
        free(Job);
        return;
    }

    Thread = CreateThread(NULL, 0, BrLoader, Job, 0, NULL);
    if (!Thread)
    {
        free(Job->Url);
        free(Job);
        return;
    }
    CloseHandle(Thread);

    free(BrLoadingUrl);
    BrLoadingUrl = BrStringDuplicate(Url);
    BrLoading = TRUE;
    BrUpdateControls();
    BrInvalidateContent();
}

static
VOID
BrRecord(
    _In_ PBR_JOB Job)
{
    ULONG i;

    switch (Job->Action)
    {
        case BrHistoryPush:
            /* Going somewhere new drops the entries ahead of this one */
            for (i = BrHistoryCount ? BrHistoryIndex + 1 : 0; i < BrHistoryCount; i++)
            {
                free(BrHistory[i]);
                BrHistory[i] = NULL;
            }
            BrHistoryCount = BrHistoryCount ? BrHistoryIndex + 1 : 0;

            if (BrHistoryCount == BR_HISTORY_MAX)
            {
                free(BrHistory[0]);
                MoveMemory(&BrHistory[0], &BrHistory[1], (BR_HISTORY_MAX - 1) * sizeof(BrHistory[0]));
                BrHistoryCount--;
            }

            BrHistory[BrHistoryCount] = BrStringDuplicate(Job->Page->Url);
            if (BrHistory[BrHistoryCount])
                BrHistoryCount++;
            BrHistoryIndex = BrHistoryCount ? BrHistoryCount - 1 : 0;
            break;

        case BrHistoryMove:
            if (Job->HistoryIndex < BrHistoryCount)
                BrHistoryIndex = Job->HistoryIndex;
            break;

        case BrHistoryStay:
            break;
    }
}

static
VOID
BrLoaded(
    _In_ PBR_JOB Job)
{
    ULONG Width, Height;

    BrLoading = FALSE;
    free(BrLoadingUrl);
    BrLoadingUrl = NULL;

    if (Job->Page)
    {
        BrPageDestroy(BrPage);
        BrPage = Job->Page;
        BrScroll = 0;
        BrRecord(Job);

        /* The window may have changed size while the page loaded */
        BrViewport(&Width, &Height);
        if (Width != BrPage->ViewportWidth || Height != BrPage->ViewportHeight)
            BrPageRelayout(BrPage, Width, Height);

        SetFocus(BrWindow);
    }
    else
    {
        MessageBeep(MB_ICONWARNING);
    }

    free(Job->Url);
    free(Job);

    BrUpdateControls();
    BrInvalidateContent();
}

/* Typed text becomes a URL, or a search when it does not look like one */
static
VOID
BrGo(VOID)
{
    static const char Search[] = "https://lite.duckduckgo.com/lite/?q=";
    WCHAR Typed[BR_ADDRESS_MAX];
    char Text[BR_ADDRESS_MAX * 3];
    char Url[BR_ADDRESS_MAX * 9];
    PCSTR Start;
    size_t Length, i;
    BOOL LooksLikeUrl;

    GetDlgItemTextW(BrWindow, IDC_ADDRESS, Typed, ARRAYSIZE(Typed));
    if (!WideCharToMultiByte(CP_UTF8, 0, Typed, -1, Text, sizeof(Text), NULL, NULL))
        return;

    for (Start = Text; *Start == ' '; Start++);
    Length = strlen(Start);
    while (Length && Start[Length - 1] == ' ')
        Length--;
    if (!Length)
    {
        BrNavigate(BR_HOME_URL, BrHistoryPush, 0);
        return;
    }

    LooksLikeUrl = (memchr(Start, ' ', Length) == NULL) &&
                   (strstr(Start, "://") || !_strnicmp(Start, "about:", 6) || memchr(Start, '.', Length));

    if (strstr(Start, "://") || !_strnicmp(Start, "about:", 6))
    {
        StringCchCopyNA(Url, ARRAYSIZE(Url), Start, Length);
    }
    else if (LooksLikeUrl)
    {
        StringCchCopyA(Url, ARRAYSIZE(Url), "https://");
        StringCchCatNA(Url, ARRAYSIZE(Url), Start, Length);
    }
    else
    {
        StringCchCopyA(Url, ARRAYSIZE(Url), Search);
        for (i = 0; i < Length; i++)
        {
            UCHAR c = (UCHAR)Start[i];
            char Escape[4];

            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                c == '-' || c == '_' || c == '.' || c == '~')
            {
                Escape[0] = (char)c;
                Escape[1] = '\0';
            }
            else if (c == ' ')
            {
                StringCchCopyA(Escape, ARRAYSIZE(Escape), "+");
            }
            else
            {
                StringCchPrintfA(Escape, ARRAYSIZE(Escape), "%%%02X", c);
            }
            StringCchCatA(Url, ARRAYSIZE(Url), Escape);
        }
    }

    SetFocus(BrWindow);
    BrNavigate(Url, BrHistoryPush, 0);
}

static
VOID
BrTap(
    _In_ POINT Point)
{
    PSTR Link;

    if (!BrPage || BrLoading)
        return;

    Link = BrPageLinkAt(BrPage, (Point.x - BrContent.left) / BrZoom,
                        (Point.y - BrContent.top) / BrZoom + BrScroll);
    if (Link)
    {
        BrNavigate(Link, BrHistoryPush, 0);
        free(Link);
    }
}

static
VOID
BrLayout(VOID)
{
    RECT Client;
    LONG Width, Edit, Bottom, Third;
    ULONG ViewWidth, ViewHeight;

    GetClientRect(BrWindow, &Client);
    Width = Client.right - 2 * BR_MARGIN;
    if (Width < 200)
        return;

    Edit = Width - 2 * BR_ROW - 2 * BR_GAP;
    MoveWindow(GetDlgItem(BrWindow, IDC_BACK), BR_MARGIN, BR_SAFE_TOP, BR_ROW, BR_ROW, TRUE);
    MoveWindow(GetDlgItem(BrWindow, IDC_ADDRESS), BR_MARGIN + BR_ROW + BR_GAP, BR_SAFE_TOP, Edit, BR_ROW, TRUE);
    MoveWindow(GetDlgItem(BrWindow, IDC_GO), Client.right - BR_MARGIN - BR_ROW, BR_SAFE_TOP, BR_ROW, BR_ROW, TRUE);

    Bottom = Client.bottom - BR_MARGIN - BR_ROW;
    Third = (Width - 2 * BR_GAP) / 3;
    MoveWindow(GetDlgItem(BrWindow, IDC_HOME), BR_MARGIN, Bottom, Third, BR_ROW, TRUE);
    MoveWindow(GetDlgItem(BrWindow, IDC_RELOAD), BR_MARGIN + Third + BR_GAP, Bottom, Third, BR_ROW, TRUE);
    MoveWindow(GetDlgItem(BrWindow, IDC_EXIT), BR_MARGIN + 2 * (Third + BR_GAP), Bottom, Third, BR_ROW, TRUE);

    /* The page runs edge to edge between the two bars */
    BrContent.left = 0;
    BrContent.right = Client.right;
    BrContent.top = BR_SAFE_TOP + BR_ROW + BR_GAP;
    BrContent.bottom = max(Bottom - BR_GAP - BR_STATUS_H, BrContent.top + BR_ROW);

    BrZoom = max((Client.right / BR_CSS_WIDTH), 1);

    if (BrPage && !BrLoading)
    {
        BrViewport(&ViewWidth, &ViewHeight);
        if (ViewWidth != BrPage->ViewportWidth)
            BrPageRelayout(BrPage, ViewWidth, ViewHeight);
    }

    BrScrollTo(BrScroll);
}

static
VOID
BrPaintContent(
    _In_ HDC Dc)
{
    LONG Width = BrContent.right - BrContent.left;
    LONG Height = BrContent.bottom - BrContent.top;
    LONG SourceHeight, Shown;
    HBRUSH White = GetStockObject(WHITE_BRUSH);
    RECT Rest, Bar;
    HDC Memory;
    HGDIOBJ Old;

    if (!BrPage || !BrPage->Bitmap)
    {
        FillRect(Dc, &BrContent, BrBgBrush);
    }
    else
    {
        SourceHeight = min(Height / BrZoom, (LONG)BrPage->Canvas.Height - BrScroll);
        Shown = SourceHeight * BrZoom;

        Memory = CreateCompatibleDC(Dc);
        Old = SelectObject(Memory, BrPage->Bitmap);
        SetStretchBltMode(Dc, COLORONCOLOR);
        StretchBlt(Dc, BrContent.left, BrContent.top, (LONG)BrPage->Canvas.Width * BrZoom, Shown,
                   Memory, 0, BrScroll, (LONG)BrPage->Canvas.Width, SourceHeight, SRCCOPY);
        SelectObject(Memory, Old);
        DeleteDC(Memory);

        /* Whatever the page does not cover stays the page color */
        Rest = BrContent;
        Rest.left = BrContent.left + (LONG)BrPage->Canvas.Width * BrZoom;
        if (Rest.left < Rest.right)
            FillRect(Dc, &Rest, White);
        Rest = BrContent;
        Rest.top = BrContent.top + Shown;
        if (Rest.top < Rest.bottom)
            FillRect(Dc, &Rest, White);

        /* A thin position marker on the right edge */
        if (BrMaxScroll() > 0)
        {
            LONG Total = (LONG)BrPage->Canvas.Height * BrZoom;
            LONG Thumb = max(Height * Height / Total, 48);

            Bar.right = BrContent.right - 6;
            Bar.left = Bar.right - 10;
            Bar.top = BrContent.top + (Height - Thumb) * BrScroll / BrMaxScroll();
            Bar.bottom = Bar.top + Thumb;
            BrFillRound(Dc, &Bar, 10, BR_DIM);
        }
    }

    if (BrLoading)
    {
        Bar = BrContent;
        Bar.bottom = Bar.top + 8;
        FillRect(Dc, &Bar, GetStockObject(BLACK_BRUSH));
        Bar.right = Bar.left + Width / 3;
        BrFillRound(Dc, &Bar, 8, BR_ACCENT);
    }
}

static
VOID
BrPaint(VOID)
{
    WCHAR Status[BR_ADDRESS_MAX + 32];
    WCHAR Text[BR_ADDRESS_MAX];
    PAINTSTRUCT Paint;
    RECT Client, Line, Band;
    HDC Dc = BeginPaint(BrWindow, &Paint);

    GetClientRect(BrWindow, &Client);

    Band = Client;
    Band.bottom = BrContent.top;
    FillRect(Dc, &Band, BrBgBrush);
    Band = Client;
    Band.top = BrContent.bottom;
    FillRect(Dc, &Band, BrBgBrush);

    BrPaintContent(Dc);

    if (BrLoading && BrLoadingUrl)
    {
        BrWiden(BrLoadingUrl, Text, ARRAYSIZE(Text));
        StringCchPrintfW(Status, ARRAYSIZE(Status), L"Loading %s", Text);
    }
    else if (BrPage && BrPage->Title && BrPage->Title[0])
    {
        BrWiden(BrPage->Title, Status, ARRAYSIZE(Status));
    }
    else if (BrPage && BrPage->Url)
    {
        BrWiden(BrPage->Url, Status, ARRAYSIZE(Status));
    }
    else
    {
        Status[0] = UNICODE_NULL;
    }

    if (BrPage && BrPage->Status >= 400 && !BrLoading)
    {
        StringCchPrintfW(Text, ARRAYSIZE(Text), L" (HTTP %lu)", BrPage->Status);
        StringCchCatW(Status, ARRAYSIZE(Status), Text);
    }

    Line.left = BR_MARGIN;
    Line.right = Client.right - BR_MARGIN;
    Line.top = BrContent.bottom + BR_GAP / 2;
    Line.bottom = Line.top + BR_STATUS_H;
    SetBkMode(Dc, TRANSPARENT);
    SetTextColor(Dc, BR_SUBTEXT);
    SelectObject(Dc, BrSmallFont);
    DrawTextW(Dc, Status, -1, &Line, DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);

    EndPaint(BrWindow, &Paint);
}

static
VOID
BrDrawButton(
    _In_ const DRAWITEMSTRUCT *Item)
{
    WCHAR Text[64];
    RECT Rect = Item->rcItem;
    BOOL Pressed = (Item->itemState & ODS_SELECTED) != 0;
    BOOL Disabled = (Item->itemState & ODS_DISABLED) != 0;
    COLORREF Fill = Disabled ? BR_FIELD : ((Pressed || Item->CtlID == IDC_GO) ? BR_ACCENT : BR_TILE);

    FillRect(Item->hDC, &Rect, BrBgBrush);
    BrFillRound(Item->hDC, &Rect, (Rect.bottom - Rect.top) / 3, Fill);

    GetWindowTextW(Item->hwndItem, Text, ARRAYSIZE(Text));
    SetBkMode(Item->hDC, TRANSPARENT);
    SetTextColor(Item->hDC, Disabled ? BR_DIM : BR_TEXT);
    SelectObject(Item->hDC, BrButtonFont);
    DrawTextW(Item->hDC, Text, -1, &Rect, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
}

static
HWND
BrControl(
    _In_ LPCWSTR Class,
    _In_opt_ LPCWSTR Text,
    _In_ DWORD Style,
    _In_ UINT Id,
    _In_ HFONT Font)
{
    HWND Control;

    Control = CreateWindowExW(0, Class, Text, WS_CHILD | WS_VISIBLE | Style, 0, 0, 10, 10,
                              BrWindow, (HMENU)(ULONG_PTR)Id, BrInstance, NULL);
    if (Control)
        SendMessageW(Control, WM_SETFONT, (WPARAM)Font, FALSE);
    return Control;
}

static
LRESULT
CALLBACK
BrWndProc(
    _In_ HWND Window,
    _In_ UINT Message,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam)
{
    POINT Point;

    switch (Message)
    {
        case WM_CREATE:
            BrWindow = Window;
            BrControl(L"BUTTON", L"<", WS_TABSTOP | BS_OWNERDRAW, IDC_BACK, BrButtonFont);
            BrControl(L"EDIT", NULL, WS_TABSTOP | ES_AUTOHSCROLL, IDC_ADDRESS, BrFont);
            BrControl(L"BUTTON", L"Go", WS_TABSTOP | BS_OWNERDRAW, IDC_GO, BrButtonFont);
            BrControl(L"BUTTON", L"Home", WS_TABSTOP | BS_OWNERDRAW, IDC_HOME, BrButtonFont);
            BrControl(L"BUTTON", L"Reload", WS_TABSTOP | BS_OWNERDRAW, IDC_RELOAD, BrButtonFont);
            BrControl(L"BUTTON", L"Exit", WS_TABSTOP | BS_OWNERDRAW, IDC_EXIT, BrButtonFont);
            SendDlgItemMessageW(Window, IDC_ADDRESS, EM_LIMITTEXT, BR_ADDRESS_MAX - 1, 0);
            return 0;

        case WM_SIZE:
            BrLayout();
            InvalidateRect(Window, NULL, FALSE);
            return 0;

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT:
            BrPaint();
            return 0;

        case WM_DRAWITEM:
            BrDrawButton((const DRAWITEMSTRUCT *)lParam);
            return TRUE;

        case WM_CTLCOLOREDIT:
            SetTextColor((HDC)wParam, BR_TEXT);
            SetBkColor((HDC)wParam, BR_FIELD);
            return (LRESULT)BrFieldBrush;

        case WM_LBUTTONDOWN:
            Point.x = GET_X_LPARAM(lParam);
            Point.y = GET_Y_LPARAM(lParam);
            if (!PtInRect(&BrContent, Point))
                break;

            SetFocus(Window);
            SetCapture(Window);
            BrPressed = TRUE;
            BrDragging = FALSE;
            BrPressPoint = Point;
            BrPressScroll = BrScroll;
            return 0;

        case WM_MOUSEMOVE:
            if (!BrPressed)
                break;

            /* A finger that moves far enough is scrolling, not tapping */
            Point.y = GET_Y_LPARAM(lParam);
            if (!BrDragging && abs(Point.y - BrPressPoint.y) > BR_DRAG_SLOP)
                BrDragging = TRUE;
            if (BrDragging)
                BrScrollTo(BrPressScroll + (BrPressPoint.y - Point.y) / BrZoom);
            return 0;

        case WM_LBUTTONUP:
            if (!BrPressed)
                break;

            BrPressed = FALSE;
            ReleaseCapture();
            if (!BrDragging)
                BrTap(BrPressPoint);
            return 0;

        case WM_CAPTURECHANGED:
            BrPressed = FALSE;
            break;

        case WM_MOUSEWHEEL:
            BrScrollTo(BrScroll - GET_WHEEL_DELTA_WPARAM(wParam) * BR_WHEEL_STEP / WHEEL_DELTA);
            return 0;

        case WM_KEYDOWN:
            switch (wParam)
            {
                case VK_UP: BrScrollTo(BrScroll - BR_WHEEL_STEP); return 0;
                case VK_DOWN: BrScrollTo(BrScroll + BR_WHEEL_STEP); return 0;
                case VK_PRIOR: BrScrollTo(BrScroll - (BrContent.bottom - BrContent.top) / BrZoom); return 0;
                case VK_NEXT: BrScrollTo(BrScroll + (BrContent.bottom - BrContent.top) / BrZoom); return 0;
                case VK_HOME: BrScrollTo(0); return 0;
                case VK_END: BrScrollTo(BrMaxScroll()); return 0;
            }
            break;

        case WM_COMMAND:
            switch (LOWORD(wParam))
            {
                case IDOK:
                case IDC_GO:
                    if (LOWORD(wParam) == IDOK || HIWORD(wParam) == BN_CLICKED)
                        BrGo();
                    break;

                case IDC_BACK:
                    if (BrHistoryIndex > 0 && BrHistoryIndex - 1 < BrHistoryCount)
                        BrNavigate(BrHistory[BrHistoryIndex - 1], BrHistoryMove, BrHistoryIndex - 1);
                    break;

                case IDC_HOME:
                    BrNavigate(BR_HOME_URL, BrHistoryPush, 0);
                    break;

                case IDC_RELOAD:
                    if (BrPage && BrPage->Url)
                        BrNavigate(BrPage->Url, BrHistoryStay, 0);
                    break;

                case IDCANCEL:
                    SetFocus(Window);
                    BrUpdateControls();
                    break;

                case IDC_EXIT:
                    DestroyWindow(Window);
                    break;
            }
            return 0;

        case WM_BR_LOADED:
            BrLoaded((PBR_JOB)lParam);
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }

    return DefWindowProcW(Window, Message, wParam, lParam);
}

static
HFONT
BrMakeFont(
    _In_ int Height,
    _In_ int Weight)
{
    HFONT Font = CreateFontW(Height, 0, 0, 0, Weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"Tahoma");

    return Font ? Font : (HFONT)GetStockObject(DEFAULT_GUI_FONT);
}

/* The start page, or the URL given on the command line */
static
VOID
BrFirstUrl(
    _In_opt_ LPCWSTR CommandLine,
    _Out_writes_(Count) PSTR Url,
    _In_ DWORD Count)
{
    WCHAR Argument[BR_ADDRESS_MAX];
    size_t Length;

    StringCchCopyA(Url, Count, BR_HOME_URL);
    if (!CommandLine)
        return;

    while (*CommandLine == L' ' || *CommandLine == L'"')
        CommandLine++;
    StringCchCopyW(Argument, ARRAYSIZE(Argument), CommandLine);
    StringCchLengthW(Argument, ARRAYSIZE(Argument), &Length);
    while (Length && (Argument[Length - 1] == L' ' || Argument[Length - 1] == L'"'))
        Argument[--Length] = UNICODE_NULL;

    if (Length && !WideCharToMultiByte(CP_UTF8, 0, Argument, -1, Url, (int)Count, NULL, NULL))
        StringCchCopyA(Url, Count, BR_HOME_URL);
}

int
WINAPI
wWinMain(
    _In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPWSTR lpCmdLine,
    _In_ int nShowCmd)
{
    char First[BR_ADDRESS_MAX];
    WNDCLASSEXW Class;
    HWND Window;
    MSG Msg;
    ULONG i;

    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(nShowCmd);

    BrInstance = hInstance;
    BrFont = BrMakeFont(34, FW_NORMAL);
    BrSmallFont = BrMakeFont(28, FW_NORMAL);
    BrButtonFont = BrMakeFont(32, FW_SEMIBOLD);
    BrBgBrush = CreateSolidBrush(BR_BG);
    BrFieldBrush = CreateSolidBrush(BR_FIELD);

    /* Shared engine state is built once here, before any loader thread runs */
    DfBrNetInit();
    DfBrCssUserAgentStylesheet();

    ZeroMemory(&Class, sizeof(Class));
    Class.cbSize = sizeof(Class);
    Class.lpfnWndProc = BrWndProc;
    Class.hInstance = hInstance;
    Class.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    Class.hIcon = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
    Class.hbrBackground = BrBgBrush;
    Class.lpszClassName = L"ReactOSBrowser";
    if (!RegisterClassExW(&Class))
        return 1;

    Window = CreateWindowExW(0, Class.lpszClassName, L"Browser", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                             CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                             NULL, NULL, hInstance, NULL);
    if (!Window)
        return 1;

    ShowWindow(Window, SW_SHOWMAXIMIZED);
    UpdateWindow(Window);

    BrFirstUrl(lpCmdLine, First, ARRAYSIZE(First));
    BrNavigate(First, BrHistoryPush, 0);

    while (GetMessageW(&Msg, NULL, 0, 0) > 0)
    {
        if (!IsDialogMessageW(Window, &Msg))
        {
            TranslateMessage(&Msg);
            DispatchMessageW(&Msg);
        }
    }

    BrPageDestroy(BrPage);
    for (i = 0; i < BrHistoryCount; i++)
        free(BrHistory[i]);
    free(BrLoadingUrl);
    DfBrNetShutdown();
    DeleteObject(BrBgBrush);
    DeleteObject(BrFieldBrush);
    return (int)Msg.wParam;
}
