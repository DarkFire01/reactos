/*
 * PROJECT:     ReactOS Memory Explorer
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Live view of system memory and this process' address space
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#include <stdarg.h>
#include <math.h>
#include <windef.h>
#include <winbase.h>
#include <wingdi.h>
#include <winuser.h>
#include <strsafe.h>

#define HISTORY_COUNT   120
#define MAX_REGIONS     512
#define GAUGE_SEGMENTS  90
#define SAMPLE_TIMER    1
#define SAMPLE_MS       500

#define CLR_BACKGROUND  RGB(0x14, 0x15, 0x1C)
#define CLR_PANEL       RGB(0x1E, 0x20, 0x2A)
#define CLR_TRACK       RGB(0x2A, 0x2D, 0x3A)
#define CLR_GRID        RGB(0x2C, 0x2F, 0x3C)
#define CLR_TEXT        RGB(0xEC, 0xEE, 0xF5)
#define CLR_DIM         RGB(0x86, 0x8B, 0xA0)
#define CLR_CYAN        RGB(0x4F, 0xD1, 0xC5)
#define CLR_PURPLE      RGB(0xA7, 0x8B, 0xFA)
#define CLR_PINK        RGB(0xF4, 0x72, 0xB6)
#define CLR_BLUE        RGB(0x60, 0xA5, 0xFA)
#define CLR_AMBER       RGB(0xF5, 0xB0, 0x40)

typedef struct _REGION_ENTRY
{
    SIZE_T Size;
    ULONG Kind;
} REGION_ENTRY, *PREGION_ENTRY;

typedef struct _MEMAPP_STATE
{
    SYSTEM_INFO SysInfo;
    MEMORYSTATUSEX Status;
    UCHAR LoadHistory[HISTORY_COUNT];
    UCHAR CommitHistory[HISTORY_COUNT];
    ULONG HistoryHead;
    ULONG HistoryCount;
    REGION_ENTRY Regions[MAX_REGIONS];
    ULONG RegionCount;
    ULONGLONG KindBytes[3];
    ULONG KindCount[3];
    ULONG Ticks;
    HFONT TitleFont;
    HFONT BigFont;
    HFONT ValueFont;
    HFONT SmallFont;
} MEMAPP_STATE;

static MEMAPP_STATE g_State;

static const COLORREF g_KindColors[3] = { CLR_BLUE, CLR_PURPLE, CLR_CYAN };
static const PCWSTR g_KindNames[3] = { L"image", L"mapped", L"private" };

static
ULONG
NTAPI
CommitPercent(
    _In_ const MEMORYSTATUSEX *Status)
{
    if (Status->ullTotalPageFile == 0)
        return 0;

    return (ULONG)(100 - (Status->ullAvailPageFile * 100 / Status->ullTotalPageFile));
}

static
VOID
SampleMemory(VOID)
{
    MEMORYSTATUSEX Status;

    Status.dwLength = sizeof(Status);
    if (!GlobalMemoryStatusEx(&Status))
        return;

    g_State.Status = Status;
    g_State.LoadHistory[g_State.HistoryHead] = (UCHAR)Status.dwMemoryLoad;
    g_State.CommitHistory[g_State.HistoryHead] = (UCHAR)CommitPercent(&Status);
    g_State.HistoryHead = (g_State.HistoryHead + 1) % HISTORY_COUNT;

    if (g_State.HistoryCount < HISTORY_COUNT)
        g_State.HistoryCount++;
}

static
ULONG
NTAPI
KindFromType(
    _In_ DWORD Type)
{
    if (Type == MEM_IMAGE)
        return 0;
    if (Type == MEM_MAPPED)
        return 1;
    return 2;
}

static
VOID
SampleRegions(VOID)
{
    MEMORY_BASIC_INFORMATION Mbi;
    PUCHAR Address = g_State.SysInfo.lpMinimumApplicationAddress;
    PUCHAR Limit = g_State.SysInfo.lpMaximumApplicationAddress;
    PUCHAR Next;
    ULONG Kind;

    g_State.RegionCount = 0;
    ZeroMemory(g_State.KindBytes, sizeof(g_State.KindBytes));
    ZeroMemory(g_State.KindCount, sizeof(g_State.KindCount));

    while (Address < Limit &&
           VirtualQuery(Address, &Mbi, sizeof(Mbi)) == sizeof(Mbi))
    {
        if (Mbi.State == MEM_COMMIT)
        {
            Kind = KindFromType(Mbi.Type);
            g_State.KindBytes[Kind] += Mbi.RegionSize;
            g_State.KindCount[Kind]++;

            if (g_State.RegionCount < MAX_REGIONS)
            {
                g_State.Regions[g_State.RegionCount].Size = Mbi.RegionSize;
                g_State.Regions[g_State.RegionCount].Kind = Kind;
                g_State.RegionCount++;
            }
        }

        Next = (PUCHAR)Mbi.BaseAddress + Mbi.RegionSize;
        if (Next <= Address)
            break;
        Address = Next;
    }
}

static
COLORREF
NTAPI
BlendColor(
    _In_ COLORREF From,
    _In_ COLORREF To,
    _In_ ULONG Position,
    _In_ ULONG Range)
{
    INT r = GetRValue(From) + ((INT)GetRValue(To) - GetRValue(From)) * (INT)Position / (INT)Range;
    INT g = GetGValue(From) + ((INT)GetGValue(To) - GetGValue(From)) * (INT)Position / (INT)Range;
    INT b = GetBValue(From) + ((INT)GetBValue(To) - GetBValue(From)) * (INT)Position / (INT)Range;

    return RGB(r, g, b);
}

static
COLORREF
NTAPI
GaugeColor(
    _In_ ULONG Segment)
{
    ULONG Half = GAUGE_SEGMENTS / 2;

    if (Segment < Half)
        return BlendColor(CLR_CYAN, CLR_PURPLE, Segment, Half);

    return BlendColor(CLR_PURPLE, CLR_PINK, Segment - Half, GAUGE_SEGMENTS - Half);
}

static
VOID
NTAPI
FillSolid(
    _In_ HDC hdc,
    _In_ const RECT *Rect,
    _In_ COLORREF Color)
{
    HBRUSH Brush = CreateSolidBrush(Color);

    FillRect(hdc, Rect, Brush);
    DeleteObject(Brush);
}

static
VOID
NTAPI
FillRounded(
    _In_ HDC hdc,
    _In_ const RECT *Rect,
    _In_ COLORREF Color,
    _In_ INT Radius)
{
    HBRUSH Brush = CreateSolidBrush(Color);
    HGDIOBJ OldBrush = SelectObject(hdc, Brush);
    HGDIOBJ OldPen = SelectObject(hdc, GetStockObject(NULL_PEN));

    RoundRect(hdc, Rect->left, Rect->top, Rect->right + 1, Rect->bottom + 1, Radius, Radius);

    SelectObject(hdc, OldPen);
    SelectObject(hdc, OldBrush);
    DeleteObject(Brush);
}

static
VOID
NTAPI
DrawLabel(
    _In_ HDC hdc,
    _In_ PCWSTR Text,
    _In_ const RECT *Rect,
    _In_ HFONT Font,
    _In_ COLORREF Color,
    _In_ UINT Format)
{
    RECT Copy = *Rect;
    HGDIOBJ OldFont = SelectObject(hdc, Font);

    SetTextColor(hdc, Color);
    DrawTextW(hdc, Text, -1, &Copy, Format | DT_NOPREFIX | DT_SINGLELINE);
    SelectObject(hdc, OldFont);
}

static
VOID
NTAPI
DrawSwatch(
    _In_ HDC hdc,
    _In_ INT x,
    _In_ INT y,
    _In_ COLORREF Color)
{
    RECT Rect = { x, y, x + 10, y + 10 };

    FillRounded(hdc, &Rect, Color, 4);
}

static
VOID
NTAPI
FormatGigs(
    _Out_writes_(Count) PWSTR Buffer,
    _In_ SIZE_T Count,
    _In_ ULONGLONG Bytes)
{
    ULONGLONG Tenths = Bytes * 10 / (1024ULL * 1024ULL * 1024ULL);

    StringCchPrintfW(Buffer, Count, L"%I64u.%I64u GB", Tenths / 10, Tenths % 10);
}

static
VOID
NTAPI
FormatSize(
    _Out_writes_(Count) PWSTR Buffer,
    _In_ SIZE_T Count,
    _In_ ULONGLONG Bytes)
{
    if (Bytes >= 1024 * 1024)
        StringCchPrintfW(Buffer, Count, L"%I64u MB", Bytes / (1024 * 1024));
    else
        StringCchPrintfW(Buffer, Count, L"%I64u KB", Bytes / 1024);
}

static
VOID
NTAPI
DrawGauge(
    _In_ HDC hdc,
    _In_ const RECT *Panel)
{
    const double Pi = 3.14159265358979;
    POINT Quad[4];
    RECT Text;
    WCHAR Buffer[32];
    HGDIOBJ OldPen;
    HBRUSH Brush;
    HGDIOBJ OldBrush;
    double a0, a1;
    INT cx, cy, Outer, Inner;
    ULONG Active, i;

    FillRounded(hdc, Panel, CLR_PANEL, 18);

    Text = *Panel;
    Text.left += 18;
    Text.top += 14;
    DrawLabel(hdc, L"PHYSICAL", &Text, g_State.SmallFont, CLR_DIM, DT_LEFT | DT_TOP);

    cx = (Panel->left + Panel->right) / 2;
    cy = (Panel->top + Panel->bottom) / 2 + 10;
    Outer = min(Panel->right - Panel->left, Panel->bottom - Panel->top - 40) / 2 - 18;
    Inner = Outer - 16;
    Active = g_State.Status.dwMemoryLoad * GAUGE_SEGMENTS / 100;

    OldPen = SelectObject(hdc, GetStockObject(NULL_PEN));

    for (i = 0; i < GAUGE_SEGMENTS; i++)
    {
        a0 = -Pi / 2 + 2 * Pi * i / GAUGE_SEGMENTS;
        a1 = -Pi / 2 + 2 * Pi * (i + 1) / GAUGE_SEGMENTS - 0.018;

        Quad[0].x = cx + (LONG)(cos(a0) * Outer);
        Quad[0].y = cy + (LONG)(sin(a0) * Outer);
        Quad[1].x = cx + (LONG)(cos(a1) * Outer);
        Quad[1].y = cy + (LONG)(sin(a1) * Outer);
        Quad[2].x = cx + (LONG)(cos(a1) * Inner);
        Quad[2].y = cy + (LONG)(sin(a1) * Inner);
        Quad[3].x = cx + (LONG)(cos(a0) * Inner);
        Quad[3].y = cy + (LONG)(sin(a0) * Inner);

        Brush = CreateSolidBrush((i < Active) ? GaugeColor(i) : CLR_TRACK);
        OldBrush = SelectObject(hdc, Brush);
        Polygon(hdc, Quad, 4);
        SelectObject(hdc, OldBrush);
        DeleteObject(Brush);
    }

    SelectObject(hdc, OldPen);

    StringCchPrintfW(Buffer, ARRAYSIZE(Buffer), L"%lu%%", g_State.Status.dwMemoryLoad);
    SetRect(&Text, cx - Inner, cy - 30, cx + Inner, cy + 12);
    DrawLabel(hdc, Buffer, &Text, g_State.BigFont, CLR_TEXT, DT_CENTER | DT_BOTTOM);

    SetRect(&Text, cx - Inner, cy + 12, cx + Inner, cy + 32);
    DrawLabel(hdc, L"memory load", &Text, g_State.SmallFont, CLR_DIM, DT_CENTER | DT_TOP);
}

static
VOID
NTAPI
DrawSeries(
    _In_ HDC hdc,
    _In_ const RECT *Plot,
    _In_reads_(HISTORY_COUNT) const UCHAR *History,
    _In_ COLORREF Color,
    _In_ BOOLEAN Filled)
{
    POINT Points[HISTORY_COUNT + 2];
    INT Width = Plot->right - Plot->left;
    INT Height = Plot->bottom - Plot->top;
    ULONG Count = g_State.HistoryCount;
    ULONG Oldest = (g_State.HistoryHead + HISTORY_COUNT - Count) % HISTORY_COUNT;
    HGDIOBJ OldPen, OldBrush;
    HBRUSH Brush;
    HPEN Pen;
    ULONG i;

    if (Count < 2)
        return;

    for (i = 0; i < Count; i++)
    {
        Points[i].x = Plot->right - (LONG)((Count - 1 - i) * Width / (HISTORY_COUNT - 1));
        Points[i].y = Plot->bottom - History[(Oldest + i) % HISTORY_COUNT] * Height / 100;
    }

    if (Filled)
    {
        Points[Count].x = Points[Count - 1].x;
        Points[Count].y = Plot->bottom;
        Points[Count + 1].x = Points[0].x;
        Points[Count + 1].y = Plot->bottom;

        Brush = CreateSolidBrush(BlendColor(CLR_PANEL, Color, 1, 5));
        OldBrush = SelectObject(hdc, Brush);
        OldPen = SelectObject(hdc, GetStockObject(NULL_PEN));
        Polygon(hdc, Points, Count + 2);
        SelectObject(hdc, OldPen);
        SelectObject(hdc, OldBrush);
        DeleteObject(Brush);
    }

    Pen = CreatePen(PS_SOLID, 2, Color);
    OldPen = SelectObject(hdc, Pen);
    Polyline(hdc, Points, Count);
    SelectObject(hdc, OldPen);
    DeleteObject(Pen);
}

static
VOID
NTAPI
DrawGraph(
    _In_ HDC hdc,
    _In_ const RECT *Panel)
{
    RECT Text, Plot;
    HPEN Pen;
    HGDIOBJ OldPen;
    INT i, y;

    FillRounded(hdc, Panel, CLR_PANEL, 18);

    Text = *Panel;
    Text.left += 18;
    Text.top += 14;
    DrawLabel(hdc, L"HISTORY", &Text, g_State.SmallFont, CLR_DIM, DT_LEFT | DT_TOP);

    DrawSwatch(hdc, Panel->right - 170, Panel->top + 18, CLR_CYAN);
    SetRect(&Text, Panel->right - 154, Panel->top + 14, Panel->right - 90, Panel->top + 32);
    DrawLabel(hdc, L"load", &Text, g_State.SmallFont, CLR_TEXT, DT_LEFT | DT_TOP);

    DrawSwatch(hdc, Panel->right - 96, Panel->top + 18, CLR_PURPLE);
    SetRect(&Text, Panel->right - 80, Panel->top + 14, Panel->right - 10, Panel->top + 32);
    DrawLabel(hdc, L"commit", &Text, g_State.SmallFont, CLR_TEXT, DT_LEFT | DT_TOP);

    SetRect(&Plot, Panel->left + 18, Panel->top + 48, Panel->right - 18, Panel->bottom - 18);

    Pen = CreatePen(PS_SOLID, 1, CLR_GRID);
    OldPen = SelectObject(hdc, Pen);
    for (i = 0; i <= 4; i++)
    {
        y = Plot.top + (Plot.bottom - Plot.top) * i / 4;
        MoveToEx(hdc, Plot.left, y, NULL);
        LineTo(hdc, Plot.right, y);
    }
    SelectObject(hdc, OldPen);
    DeleteObject(Pen);

    DrawSeries(hdc, &Plot, g_State.LoadHistory, CLR_CYAN, TRUE);
    DrawSeries(hdc, &Plot, g_State.CommitHistory, CLR_PURPLE, FALSE);
}

static
VOID
NTAPI
DrawCard(
    _In_ HDC hdc,
    _In_ const RECT *Card,
    _In_ PCWSTR Title,
    _In_ PCWSTR Value,
    _In_ PCWSTR Detail,
    _In_ COLORREF Accent)
{
    RECT Rect;

    FillRounded(hdc, Card, CLR_PANEL, 14);

    SetRect(&Rect, Card->left + 14, Card->top + 18, Card->left + 18, Card->bottom - 18);
    FillRounded(hdc, &Rect, Accent, 4);

    SetRect(&Rect, Card->left + 30, Card->top + 12, Card->right - 10, Card->top + 30);
    DrawLabel(hdc, Title, &Rect, g_State.SmallFont, CLR_DIM, DT_LEFT | DT_TOP);

    SetRect(&Rect, Card->left + 30, Card->top + 28, Card->right - 10, Card->top + 58);
    DrawLabel(hdc, Value, &Rect, g_State.ValueFont, CLR_TEXT, DT_LEFT | DT_TOP);

    SetRect(&Rect, Card->left + 30, Card->top + 58, Card->right - 10, Card->bottom - 6);
    DrawLabel(hdc, Detail, &Rect, g_State.SmallFont, CLR_DIM, DT_LEFT | DT_TOP | DT_END_ELLIPSIS);
}

static
VOID
NTAPI
DrawCards(
    _In_ HDC hdc,
    _In_ const RECT *Row)
{
    const MEMORYSTATUSEX *Status = &g_State.Status;
    WCHAR Value[32], Detail[64], Total[32];
    ULONGLONG Committed;
    RECT Card;
    INT Width, Gap = 16;

    Width = (Row->right - Row->left - Gap * 3) / 4;
    SetRect(&Card, Row->left, Row->top, Row->left + Width, Row->bottom);

    FormatGigs(Value, ARRAYSIZE(Value), Status->ullTotalPhys - Status->ullAvailPhys);
    FormatGigs(Total, ARRAYSIZE(Total), Status->ullTotalPhys);
    StringCchPrintfW(Detail, ARRAYSIZE(Detail), L"of %s in use", Total);
    DrawCard(hdc, &Card, L"PHYSICAL", Value, Detail, CLR_CYAN);

    OffsetRect(&Card, Width + Gap, 0);
    FormatGigs(Value, ARRAYSIZE(Value), Status->ullTotalPageFile - Status->ullAvailPageFile);
    FormatGigs(Total, ARRAYSIZE(Total), Status->ullTotalPageFile);
    StringCchPrintfW(Detail, ARRAYSIZE(Detail), L"of %s limit", Total);
    DrawCard(hdc, &Card, L"COMMIT", Value, Detail, CLR_PURPLE);

    OffsetRect(&Card, Width + Gap, 0);
    Committed = g_State.KindBytes[0] + g_State.KindBytes[1] + g_State.KindBytes[2];
    StringCchPrintfW(Value, ARRAYSIZE(Value), L"%lu",
                     g_State.KindCount[0] + g_State.KindCount[1] + g_State.KindCount[2]);
    FormatSize(Total, ARRAYSIZE(Total), Committed);
    StringCchPrintfW(Detail, ARRAYSIZE(Detail), L"regions, %s committed", Total);
    DrawCard(hdc, &Card, L"THIS PROCESS", Value, Detail, CLR_PINK);

    OffsetRect(&Card, Width + Gap, 0);
    StringCchPrintfW(Value, ARRAYSIZE(Value), L"%lu CPUs", g_State.SysInfo.dwNumberOfProcessors);
    StringCchPrintfW(Detail, ARRAYSIZE(Detail), L"%lu KB pages, %lu-bit",
                     g_State.SysInfo.dwPageSize / 1024, (ULONG)(sizeof(PVOID) * 8));
    DrawCard(hdc, &Card, L"SYSTEM", Value, Detail, CLR_AMBER);
}

static
ULONG
NTAPI
RegionWeight(
    _In_ SIZE_T Size)
{
    ULONG Weight = 1;

    Size /= 4096;
    while (Size > 1)
    {
        Size >>= 1;
        Weight++;
    }

    return Weight;
}

static
VOID
NTAPI
DrawAddressMap(
    _In_ HDC hdc,
    _In_ const RECT *Panel)
{
    RECT Text, Strip, Block;
    WCHAR Buffer[48];
    WCHAR Size[24];
    ULONG TotalWeight = 0;
    ULONG RunningWeight = 0;
    ULONG i;
    INT x, Next, Width, Legend;

    FillRounded(hdc, Panel, CLR_PANEL, 18);

    Text = *Panel;
    Text.left += 18;
    Text.top += 14;
    DrawLabel(hdc, L"ADDRESS SPACE", &Text, g_State.SmallFont, CLR_DIM, DT_LEFT | DT_TOP);

    Legend = Panel->right - 18 - 3 * 130;
    for (i = 0; i < 3; i++)
    {
        DrawSwatch(hdc, Legend, Panel->top + 18, g_KindColors[i]);
        FormatSize(Size, ARRAYSIZE(Size), g_State.KindBytes[i]);
        StringCchPrintfW(Buffer, ARRAYSIZE(Buffer), L"%s  %s", g_KindNames[i], Size);
        SetRect(&Text, Legend + 16, Panel->top + 14, Legend + 128, Panel->top + 32);
        DrawLabel(hdc, Buffer, &Text, g_State.SmallFont, CLR_TEXT, DT_LEFT | DT_TOP);
        Legend += 130;
    }

    SetRect(&Strip, Panel->left + 18, Panel->top + 46, Panel->right - 18, Panel->bottom - 18);
    FillRounded(hdc, &Strip, CLR_BACKGROUND, 8);

    for (i = 0; i < g_State.RegionCount; i++)
        TotalWeight += RegionWeight(g_State.Regions[i].Size);

    if (TotalWeight == 0)
        return;

    Width = Strip.right - Strip.left - 8;
    for (i = 0; i < g_State.RegionCount; i++)
    {
        x = Strip.left + 4 + (INT)((ULONGLONG)RunningWeight * Width / TotalWeight);
        RunningWeight += RegionWeight(g_State.Regions[i].Size);
        Next = Strip.left + 4 + (INT)((ULONGLONG)RunningWeight * Width / TotalWeight);

        SetRect(&Block, x, Strip.top + 4, max(Next - 1, x + 1), Strip.bottom - 4);
        FillSolid(hdc, &Block, g_KindColors[g_State.Regions[i].Kind]);
    }
}

static
VOID
NTAPI
PaintWindow(
    _In_ HWND hwnd,
    _In_ HDC hdc)
{
    RECT Client, Rect, Top, Gauge, Graph, Cards, Map;
    HDC MemDC;
    HBITMAP Bitmap;
    HGDIOBJ OldBitmap;
    INT Width, Height;

    GetClientRect(hwnd, &Client);
    Width = Client.right;
    Height = Client.bottom;

    MemDC = CreateCompatibleDC(hdc);
    Bitmap = CreateCompatibleBitmap(hdc, Width, Height);
    OldBitmap = SelectObject(MemDC, Bitmap);
    SetBkMode(MemDC, TRANSPARENT);

    FillSolid(MemDC, &Client, CLR_BACKGROUND);

    SetRect(&Rect, 24, 16, Width - 24, 50);
    DrawLabel(MemDC, L"Memory Explorer", &Rect, g_State.TitleFont, CLR_TEXT, DT_LEFT | DT_TOP);
    DrawLabel(MemDC, sizeof(PVOID) == 8 ? L"x64  |  live" : L"x86  |  live",
              &Rect, g_State.SmallFont, CLR_DIM, DT_RIGHT | DT_VCENTER);

    SetRect(&Rect, 24, 50, Width - 24, 70);
    DrawLabel(MemDC, L"System memory and this process' address space, sampled every 500 ms",
              &Rect, g_State.SmallFont, CLR_DIM, DT_LEFT | DT_TOP);

    SetRect(&Map, 16, Height - 16 - 110, Width - 16, Height - 16);
    SetRect(&Cards, 16, Map.top - 16 - 88, Width - 16, Map.top - 16);
    SetRect(&Top, 16, 84, Width - 16, Cards.top - 16);
    SetRect(&Gauge, Top.left, Top.top, Top.left + 270, Top.bottom);
    SetRect(&Graph, Gauge.right + 16, Top.top, Top.right, Top.bottom);

    DrawGauge(MemDC, &Gauge);
    DrawGraph(MemDC, &Graph);
    DrawCards(MemDC, &Cards);
    DrawAddressMap(MemDC, &Map);

    BitBlt(hdc, 0, 0, Width, Height, MemDC, 0, 0, SRCCOPY);

    SelectObject(MemDC, OldBitmap);
    DeleteObject(Bitmap);
    DeleteDC(MemDC);
}

static
HFONT
NTAPI
MakeFont(
    _In_ INT Height,
    _In_ INT Weight)
{
    return CreateFontW(-Height, 0, 0, 0, Weight, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
}

static
LRESULT
CALLBACK
MainWndProc(
    _In_ HWND hwnd,
    _In_ UINT Msg,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam)
{
    PAINTSTRUCT ps;
    HDC hdc;

    switch (Msg)
    {
        case WM_CREATE:
            g_State.TitleFont = MakeFont(24, FW_BOLD);
            g_State.BigFont = MakeFont(38, FW_BOLD);
            g_State.ValueFont = MakeFont(24, FW_SEMIBOLD);
            g_State.SmallFont = MakeFont(13, FW_NORMAL);

            GetNativeSystemInfo(&g_State.SysInfo);
            SampleMemory();
            SampleRegions();
            SetTimer(hwnd, SAMPLE_TIMER, SAMPLE_MS, NULL);
            return 0;

        case WM_TIMER:
            SampleMemory();
            if ((++g_State.Ticks % 4) == 0)
                SampleRegions();
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;

        case WM_SIZE:
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT:
            hdc = BeginPaint(hwnd, &ps);
            PaintWindow(hwnd, hdc);
            EndPaint(hwnd, &ps);
            return 0;

        case WM_GETMINMAXINFO:
            ((PMINMAXINFO)lParam)->ptMinTrackSize.x = 820;
            ((PMINMAXINFO)lParam)->ptMinTrackSize.y = 560;
            return 0;

        case WM_DESTROY:
            KillTimer(hwnd, SAMPLE_TIMER);
            DeleteObject(g_State.TitleFont);
            DeleteObject(g_State.BigFont);
            DeleteObject(g_State.ValueFont);
            DeleteObject(g_State.SmallFont);
            PostQuitMessage(0);
            return 0;
    }

    return DefWindowProcW(hwnd, Msg, wParam, lParam);
}

INT
WINAPI
wWinMain(
    _In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPWSTR lpCmdLine,
    _In_ INT nShowCmd)
{
    WNDCLASSEXW wc;
    HWND hwnd;
    MSG Msg;

    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hIcon = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
    wc.lpszClassName = L"MemoryExplorerWnd";

    if (!RegisterClassExW(&wc))
        return 1;

    hwnd = CreateWindowExW(0, wc.lpszClassName, L"Memory Explorer",
                           WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                           960, 640, NULL, NULL, hInstance, NULL);
    if (!hwnd)
        return 1;

    ShowWindow(hwnd, nShowCmd);
    UpdateWindow(hwnd);

    while (GetMessageW(&Msg, NULL, 0, 0))
    {
        TranslateMessage(&Msg);
        DispatchMessageW(&Msg);
    }

    return (INT)Msg.wParam;
}
