/*
 * PROJECT:     ReacTVmm
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The window the machine is watched and typed at through
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "panel.h"

#include <stdio.h>
#include <string.h>

namespace rtvm
{

/* WHAT IT LOOKS LIKE *********************************************************/

static const char PanelClassName[] = "RtvmFrontPanel";

/* How often the window catches up with the machine */
constexpr UINT RedrawTimer = 1;
constexpr UINT RedrawInterval = 40;

/* The sixteen colours a text mode has, in the order the attribute byte uses */
static const COLORREF TextPalette[16] =
{
    RGB(0x00, 0x00, 0x00), RGB(0x00, 0x00, 0xAA), RGB(0x00, 0xAA, 0x00),
    RGB(0x00, 0xAA, 0xAA), RGB(0xAA, 0x00, 0x00), RGB(0xAA, 0x00, 0xAA),
    RGB(0xAA, 0x55, 0x00), RGB(0xAA, 0xAA, 0xAA), RGB(0x55, 0x55, 0x55),
    RGB(0x55, 0x55, 0xFF), RGB(0x55, 0xFF, 0x55), RGB(0x55, 0xFF, 0xFF),
    RGB(0xFF, 0x55, 0x55), RGB(0xFF, 0x55, 0xFF), RGB(0xFF, 0xFF, 0x55),
    RGB(0xFF, 0xFF, 0xFF)
};

/* The window around the screen, which is not the guest's to colour */
constexpr COLORREF ShellColour = RGB(0x14, 0x16, 0x1A);
constexpr COLORREF LabelColour = RGB(0x78, 0x80, 0x8C);
constexpr COLORREF ValueColour = RGB(0xD8, 0xDC, 0xE4);
constexpr COLORREF LampColour = RGB(0x40, 0xE0, 0x60);
constexpr COLORREF LampOff = RGB(0x24, 0x2A, 0x30);

/* Room for the strip on the right, and the gap around everything */
constexpr int PanelWidth = 210;
constexpr int Margin = 12;

/* What each line is for on a machine shaped like this one */
static const char *const LineNames[16] =
{
    "timer",   "keyboard", "cascade", "com2",
    "com1",    "lpt2",     "floppy",  "lpt1",
    "clock",   "free",     "free",    "free",
    "mouse",   "coproc",   "disk 0",  "disk 1"
};

/* OPENING AND CLOSING ********************************************************/

Panel::~Panel()
{
    if (m_Buffer != nullptr)
    {
        SelectObject(m_Buffer, m_Previous);
        DeleteDC(m_Buffer);
    }

    if (m_Surface != nullptr)
        DeleteObject(m_Surface);

    if (m_Font != nullptr)
        DeleteObject(m_Font);

    if (m_LabelFont != nullptr)
        DeleteObject(m_LabelFont);

    if (m_Window != nullptr)
        DestroyWindow(m_Window);

    DeleteCriticalSection(&m_Lock);
}

/**
 * @brief
 * Works out how big one character is, which everything else is laid out from.
 */
void Panel::Measure()
{
    HDC Screen = GetDC(nullptr);
    TEXTMETRICA Metrics = {};

    /*
     * The display font is asked for by the face the codepage 437 glyphs live
     * under, because a DOS menu is drawn out of line characters and a font
     * without them draws a menu as punctuation.
     */
    m_Font = CreateFontA(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                         OEM_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         DEFAULT_QUALITY, FIXED_PITCH | FF_MODERN,
                         "Consolas");

    if (m_Font == nullptr)
        m_Font = (HFONT)GetStockObject(OEM_FIXED_FONT);

    m_LabelFont = CreateFontA(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                              CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                              FIXED_PITCH | FF_MODERN, "Consolas");

    if (m_LabelFont == nullptr)
        m_LabelFont = (HFONT)GetStockObject(ANSI_FIXED_FONT);

    HFONT Was = (HFONT)SelectObject(Screen, m_Font);

    GetTextMetricsA(Screen, &Metrics);
    m_CellWidth = Metrics.tmAveCharWidth;
    m_CellHeight = Metrics.tmHeight;

    SelectObject(Screen, Was);
    ReleaseDC(nullptr, Screen);

    if (m_CellWidth <= 0)
        m_CellWidth = 8;

    if (m_CellHeight <= 0)
        m_CellHeight = 16;
}

bool Panel::Open(Machine &Subject, const char *Title)
{
    WNDCLASSEXA Class = {};

    m_Machine = &Subject;
    InitializeCriticalSection(&m_Lock);

    Measure();

    Class.cbSize = sizeof(Class);
    Class.lpfnWndProc = Dispatch;
    Class.hInstance = GetModuleHandleA(nullptr);
    Class.hCursor = LoadCursor(nullptr, IDC_ARROW);
    Class.hbrBackground = nullptr;
    Class.lpszClassName = PanelClassName;

    /* Already there when a second machine is opened in one process */
    if ((RegisterClassExA(&Class) == 0) &&
        (GetLastError() != ERROR_CLASS_ALREADY_EXISTS))
    {
        Log(RtvmLogError, "the window class would not register, error %lu\n",
            GetLastError());
        return false;
    }

    RECT Wanted;

    Wanted.left = 0;
    Wanted.top = 0;
    Wanted.right = (Margin * 3) + (LONG)(80 * m_CellWidth) + PanelWidth;
    Wanted.bottom = (Margin * 2) + (LONG)(25 * m_CellHeight) + 22;

    AdjustWindowRect(&Wanted, WS_OVERLAPPEDWINDOW, FALSE);

    m_Window = CreateWindowExA(0, PanelClassName, Title, WS_OVERLAPPEDWINDOW,
                               CW_USEDEFAULT, CW_USEDEFAULT,
                               Wanted.right - Wanted.left,
                               Wanted.bottom - Wanted.top,
                               nullptr, nullptr, Class.hInstance, this);

    if (m_Window == nullptr)
    {
        Log(RtvmLogError, "the window would not open, error %lu\n", GetLastError());
        return false;
    }

    ShowWindow(m_Window, SW_SHOW);
    SetTimer(m_Window, RedrawTimer, RedrawInterval, nullptr);
    return true;
}

/* THE PAGE COMING UP *********************************************************/

STDMETHODIMP Panel::QueryInterface(REFIID Interface, void **Object)
{
    if (Object == nullptr)
        return E_POINTER;

    if (IsEqualIID(Interface, IID_IUnknown) ||
        IsEqualIID(Interface, IID_IMonitorDevice))
    {
        *Object = static_cast<IMonitorDevice *>(this);
        AddRef();
        return S_OK;
    }

    *Object = nullptr;
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) Panel::AddRef()
{
    return (ULONG)InterlockedIncrement(&m_Count);
}

STDMETHODIMP_(ULONG) Panel::Release()
{
    /* The window outlives everything holding one of these, so nothing is freed */
    return (ULONG)InterlockedDecrement(&m_Count);
}

/**
 * @brief
 * Told that part of what the display holds is no longer what it was.
 *
 * @remarks
 * What changed is not handed over. The whole page is fetched instead, because
 * a screen of eighty by twenty five is four thousand bytes and working out
 * which of them to copy costs more than copying all of them.
 */
STDMETHODIMP Panel::OnVideoDirt(const RECT *Changed)
{
    UNREFERENCED_PARAMETER(Changed);

    IVideoVdev *Display = m_Machine->Screen();

    if (Display == nullptr)
        return E_UNEXPECTED;

    VDEV_SURFACE_DATA Surface = {};

    if (FAILED(Display->GetSurfaceData(&Surface)))
        return E_FAIL;

    if ((Surface.Width == 0) || (Surface.Width > PanelColumns) ||
        (Surface.Height == 0) || (Surface.Height > PanelRows))
    {
        return E_INVALIDARG;
    }

    IRtvmTextSurface *Characters = nullptr;

    if (FAILED(Display->QueryInterface(IID_IRtvmTextSurface,
                                       reinterpret_cast<void **>(&Characters))))
    {
        /* A display that draws pixels, which this has no way of showing yet */
        return E_NOINTERFACE;
    }

    EnterCriticalSection(&m_Lock);

    m_Columns = Surface.Width;
    m_Rows = Surface.Height;

    const HRESULT Status = Characters->ReadCells(m_Cells,
                                                 Surface.Width * Surface.Height * 2,
                                                 &m_CursorColumn, &m_CursorRow);

    if (SUCCEEDED(Status))
    {
        m_CursorVisible = true;
        m_HavePage = true;
    }

    LeaveCriticalSection(&m_Lock);

    Characters->Release();
    return Status;
}

/* DRAWING ********************************************************************/

/**
 * @brief
 * Draws the guest's page, one run of matching colour at a time.
 *
 * @remarks
 * A cell at a time would be four thousand calls a frame. A run at a time is a
 * handful, because a screen is mostly one colour, and the runs are found by
 * walking the attribute byte rather than by asking the guest anything.
 */
void Panel::PaintScreen(HDC Target)
{
    EnterCriticalSection(&m_Lock);

    const ULONG Columns = m_Columns;
    const ULONG Rows = m_Rows;

    HFONT Was = (HFONT)SelectObject(Target, m_Font);

    /*
     * Every glyph is put exactly one cell along, whatever the font thinks its
     * own advance is, and every run is drawn into exactly the rectangle its
     * cells cover. Letting the font decide leaves the grid at its mercy: it
     * paints its own idea of a line box, and a taller one wipes the row above.
     */
    INT Spacing[PanelColumns];

    for (ULONG Index = 0; Index < PanelColumns; Index++)
        Spacing[Index] = (INT)m_CellWidth;

    SetBkMode(Target, OPAQUE);

    for (ULONG Row = 0; Row < Rows; Row++)
    {
        ULONG Column = 0;

        while (Column < Columns)
        {
            const UCHAR *Cell = &m_Cells[((Row * Columns) + Column) * 2];
            const UCHAR Attribute = Cell[1];
            char Run[PanelColumns + 1];
            ULONG Length = 0;

            /* As far as the colour holds, which is usually the whole line */
            while ((Column + Length) < Columns)
            {
                const UCHAR *Next = &m_Cells[((Row * Columns) + Column + Length) * 2];

                if (Next[1] != Attribute)
                    break;

                Run[Length] = (char)(Next[0] != 0 ? Next[0] : ' ');
                Length++;
            }

            Run[Length] = '\0';

            RECT Cells;

            Cells.left = Margin + (int)(Column * m_CellWidth);
            Cells.top = Margin + (int)(Row * m_CellHeight);
            Cells.right = Cells.left + (int)(Length * m_CellWidth);
            Cells.bottom = Cells.top + (int)m_CellHeight;

            SetTextColor(Target, TextPalette[Attribute & 0x0F]);
            SetBkColor(Target, TextPalette[(Attribute >> 4) & 0x07]);

            ExtTextOutA(Target, Cells.left, Cells.top,
                        ETO_OPAQUE | ETO_CLIPPED, &Cells,
                        Run, (UINT)Length, Spacing);

            Column += Length;
        }
    }

    /* Where the guest thinks it is typing, on for half of every second */
    if (m_CursorVisible && (m_CursorColumn < Columns) && (m_CursorRow < Rows) &&
        (((GetTickCount() / 300) & 1) != 0))
    {
        RECT Bar;

        Bar.left = Margin + (int)(m_CursorColumn * m_CellWidth);
        Bar.top = Margin + (int)((m_CursorRow + 1) * m_CellHeight) - 2;
        Bar.right = Bar.left + (int)m_CellWidth;
        Bar.bottom = Bar.top + 2;

        SetBkColor(Target, TextPalette[7]);
        ExtTextOutA(Target, 0, 0, ETO_OPAQUE, &Bar, nullptr, 0, nullptr);
    }

    SelectObject(Target, Was);
    LeaveCriticalSection(&m_Lock);
}

/**
 * @brief
 * Dims every lamp by a step, so that a line that stops moving stops glowing.
 */
void Panel::FadeLamps()
{
    MachineStatus Status = {};

    m_Machine->Snapshot(Status);

    for (ULONG Line = 0; Line < 16; Line++)
    {
        if (Status.LineCount[Line] != m_SeenCount[Line])
        {
            m_SeenCount[Line] = Status.LineCount[Line];
            m_Lamp[Line] = 255;
        }
        else if (m_Lamp[Line] > 24)
        {
            m_Lamp[Line] -= 24;
        }
        else
        {
            m_Lamp[Line] = 0;
        }
    }
}

void Panel::PaintStatus(HDC Target)
{
    MachineStatus Status = {};
    char Line[96];
    int Left;
    int Top = Margin;

    m_Machine->Snapshot(Status);

    EnterCriticalSection(&m_Lock);
    Left = (Margin * 2) + (int)(m_Columns * m_CellWidth);
    LeaveCriticalSection(&m_Lock);

    HFONT Was = (HFONT)SelectObject(Target, m_LabelFont);

    SetBkMode(Target, TRANSPARENT);

    SetTextColor(Target, ValueColour);
    TextOutA(Target, Left, Top, "ReacTVmm", 8);
    Top += 20;

    SetTextColor(Target, Status.Running ? LampColour : RGB(0xE0, 0x70, 0x60));
    TextOutA(Target, Left, Top, Status.Running ? "running" : "stopped",
             Status.Running ? 7 : 7);
    Top += 22;

    SetTextColor(Target, LabelColour);

    _snprintf(Line, sizeof(Line), "cs:ip   %04x:%04llx",
              Status.Cs, Status.Rip & 0xFFFF);
    TextOutA(Target, Left, Top, Line, (int)strlen(Line));
    Top += 16;

    _snprintf(Line, sizeof(Line), "taken   %lu", Status.Delivered);
    TextOutA(Target, Left, Top, Line, (int)strlen(Line));
    Top += 16;

    _snprintf(Line, sizeof(Line), "refused %lu", Status.Refused);
    TextOutA(Target, Left, Top, Line, (int)strlen(Line));
    Top += 24;

    /* The lamps, which are the whole point of having a panel */
    SetTextColor(Target, ValueColour);
    TextOutA(Target, Left, Top, "LINES", 5);
    Top += 16;

    for (ULONG Which = 0; Which < 16; Which++)
    {
        RECT Lamp;
        COLORREF Shade = LampOff;

        if (m_Lamp[Which] != 0)
        {
            /* Between off and full, by however much is left of the last one */
            const ULONG Level = m_Lamp[Which];

            Shade = RGB(0x24 + ((0x40 - 0x24) * Level / 255),
                        0x2A + ((0xE0 - 0x2A) * Level / 255),
                        0x30 + ((0x60 - 0x30) * Level / 255));
        }

        Lamp.left = Left;
        Lamp.top = Top + 3;
        Lamp.right = Left + 7;
        Lamp.bottom = Top + 10;

        SetBkColor(Target, Shade);
        ExtTextOutA(Target, 0, 0, ETO_OPAQUE, &Lamp, nullptr, 0, nullptr);

        /* A line nothing is on is named quietly, one that has moved is not */
        SetTextColor(Target, (m_SeenCount[Which] != 0) ? ValueColour : LabelColour);

        _snprintf(Line, sizeof(Line), "%2lu %-9s %lu",
                  Which, LineNames[Which], m_SeenCount[Which]);
        TextOutA(Target, Left + 13, Top, Line, (int)strlen(Line));

        Top += 13;
    }

    Top += 10;

    SetTextColor(Target, ValueColour);
    TextOutA(Target, Left, Top, "HARDWARE", 8);
    Top += 16;

    SetTextColor(Target, LabelColour);

    const ULONG Devices = m_Machine->DeviceCount();

    for (ULONG Index = 0; Index < Devices; Index++)
    {
        const char *Name = m_Machine->DeviceName(Index);

        TextOutA(Target, Left + 13, Top, Name, (int)strlen(Name));
        Top += 13;
    }

    SelectObject(Target, Was);
}

void Panel::Paint(HDC Target)
{
    RECT Client;

    GetClientRect(m_Window, &Client);

    SetBkColor(Target, ShellColour);
    ExtTextOutA(Target, 0, 0, ETO_OPAQUE, &Client, nullptr, 0, nullptr);

    if (m_HavePage)
    {
        PaintScreen(Target);
    }
    else
    {
        static const char Waiting[] = "nothing has drawn anything yet";

        SelectObject(Target, m_LabelFont);
        SetBkMode(Target, TRANSPARENT);
        SetTextColor(Target, LabelColour);
        TextOutA(Target, Margin, Margin, Waiting, sizeof(Waiting) - 1);
    }

    PaintStatus(Target);
}

/* KEEPING A PICTURE OF IT ****************************************************/

void Panel::CaptureTo(const char *Path)
{
    m_CapturePath.Set(Path);
}

/**
 * @brief
 * Writes what the window is showing out as a bitmap.
 *
 * @remarks
 * Straight out of the buffer the window is drawn from, which is why there is
 * nothing to redraw and nothing has to be on screen or even in front.
 */
void Panel::Capture()
{
    if (m_CapturePath.Empty() || (m_Surface == nullptr))
        return;

    const LONG Stride = ((m_BufferWidth * 3) + 3) & ~3;
    const DWORD Bytes = (DWORD)(Stride * m_BufferHeight);

    BITMAPFILEHEADER File = {};
    BITMAPINFOHEADER Info = {};

    Info.biSize = sizeof(Info);
    Info.biWidth = m_BufferWidth;
    /* Upwards, which is the way a bitmap without a negative height is stored */
    Info.biHeight = m_BufferHeight;
    Info.biPlanes = 1;
    Info.biBitCount = 24;
    Info.biCompression = BI_RGB;

    OwnedArray<UCHAR> Pixels(new UCHAR[Bytes]);

    if (!Pixels)
        return;

    HDC Screen = GetDC(nullptr);

    /*
     * Out of the buffer before it is read, and everything drawn into it flushed
     * first. A bitmap still selected into a device context does not have to
     * have the drawing in it yet, and reading one that does not gives back
     * whatever it happens to hold rather than an error.
     */
    GdiFlush();
    SelectObject(m_Buffer, m_Previous);

    const int Got = GetDIBits(Screen, m_Surface, 0, (UINT)m_BufferHeight,
                              Pixels.Get(), (BITMAPINFO *)&Info, DIB_RGB_COLORS);

    m_Previous = (HBITMAP)SelectObject(m_Buffer, m_Surface);
    ReleaseDC(nullptr, Screen);

    if (Got == 0)
        return;

    File.bfType = 0x4D42;
    File.bfOffBits = sizeof(File) + sizeof(Info);
    File.bfSize = File.bfOffBits + Bytes;

    UniqueFile Output(CreateFileA(m_CapturePath.Get(), GENERIC_WRITE, 0, nullptr,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));

    if (!Output)
    {
        Log(RtvmLogError, "%s would not open, error %lu\n",
            m_CapturePath.Get(), GetLastError());
        return;
    }

    DWORD Written = 0;

    WriteFile(Output.Get(), &File, sizeof(File), &Written, nullptr);
    WriteFile(Output.Get(), &Info, sizeof(Info), &Written, nullptr);
    WriteFile(Output.Get(), Pixels.Get(), Bytes, &Written, nullptr);

    Log(RtvmLogInfo, "the window as it was is in %s\n", m_CapturePath.Get());
}

/* WHAT THE OPERATOR DID ******************************************************/

/**
 * @brief
 * Turns a key the window was given into the code the wire would have carried.
 *
 * @remarks
 * Windows hands over the scan code the keyboard sent, in the same set the
 * controller reports, so nothing has to be looked up. A key the wire prefixes
 * carries the prefix in the high byte, which is how both bytes reach the
 * controller as one event.
 */
void Panel::Key(WPARAM First, LPARAM Second, bool Down)
{
    ULONG Code = (ULONG)((Second >> 16) & 0xFF);

    UNREFERENCED_PARAMETER(First);

    if (Code == 0)
        return;

    if ((Second & 0x01000000) != 0)
        Code |= 0xE000;

    m_Machine->PostInput(Down ? RtvmInputKeyDown : RtvmInputKeyUp, Code);
}

/* THE WINDOW *****************************************************************/

LRESULT CALLBACK Panel::Dispatch(HWND Window, UINT Message,
                                 WPARAM First, LPARAM Second)
{
    if (Message == WM_NCCREATE)
    {
        auto *Create = reinterpret_cast<CREATESTRUCTA *>(Second);

        SetWindowLongPtrA(Window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(Create->lpCreateParams));
    }

    auto *Self = reinterpret_cast<Panel *>(GetWindowLongPtrA(Window, GWLP_USERDATA));

    if (Self == nullptr)
        return DefWindowProcA(Window, Message, First, Second);

    return Self->Handle(Window, Message, First, Second);
}

LRESULT Panel::Handle(HWND Window, UINT Message, WPARAM First, LPARAM Second)
{
    switch (Message)
    {
        case WM_TIMER:
            FadeLamps();
            InvalidateRect(Window, nullptr, FALSE);
            return 0;

        case WM_PAINT:
        {
            PAINTSTRUCT Paint;
            HDC Screen = BeginPaint(Window, &Paint);
            RECT Client;

            GetClientRect(Window, &Client);

            /* Made to fit the first time, and again whenever the window moves size */
            if ((m_Buffer == nullptr) ||
                (m_BufferWidth != Client.right) ||
                (m_BufferHeight != Client.bottom))
            {
                if (m_Buffer != nullptr)
                {
                    SelectObject(m_Buffer, m_Previous);
                    DeleteDC(m_Buffer);
                    DeleteObject(m_Surface);
                }

                m_Buffer = CreateCompatibleDC(Screen);
                m_Surface = CreateCompatibleBitmap(Screen, Client.right, Client.bottom);
                m_Previous = (HBITMAP)SelectObject(m_Buffer, m_Surface);
                m_BufferWidth = Client.right;
                m_BufferHeight = Client.bottom;
            }

            this->Paint(m_Buffer);

            BitBlt(Screen, 0, 0, Client.right, Client.bottom,
                   m_Buffer, 0, 0, SRCCOPY);

            EndPaint(Window, &Paint);
            return 0;
        }

        case WM_ERASEBKGND:
            /* Taken, because the whole client area is painted every time */
            return 1;

        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            Key(First, Second, true);
            return 0;

        case WM_KEYUP:
        case WM_SYSKEYUP:
            Key(First, Second, false);
            return 0;

        case WM_APP:
            /* The machine stopped. One last frame, so the panel says so */
            FadeLamps();
            InvalidateRect(Window, nullptr, FALSE);
            UpdateWindow(Window);
            Capture();

            if (m_CloseWhenStopped)
                PostMessageA(Window, WM_CLOSE, 0, 0);

            return 0;

        case WM_CLOSE:
            m_Machine->Stop();
            Capture();
            DestroyWindow(Window);
            return 0;

        case WM_DESTROY:
            m_Window = nullptr;
            PostQuitMessage(0);
            return 0;
    }

    return DefWindowProcA(Window, Message, First, Second);
}

void Panel::Pump()
{
    MSG Message;

    while (GetMessageA(&Message, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&Message);
        DispatchMessageA(&Message);
    }
}

} /* namespace rtvm */
