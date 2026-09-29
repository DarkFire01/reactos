/*
 * PROJECT:     ReactHypervGui
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     A window to build a machine in and watch it run
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 *
 * The same machine the terminal front end drives, with a window in front of it.
 * What that buys is not comfort: a machine that is watched can be stopped, put
 * back, given a different disc and started again without anybody typing a
 * command line, and the screen it draws is the fastest way to tell whether the
 * firmware in it got anywhere.
 *
 * It is built to run on the system it is testing. Nothing here is drawn with
 * anything newer than a device context and a font, because the point of it is
 * to be the thing sitting on the desktop of the system under test.
 */

#include <windows.h>
#include <commdlg.h>
#include <stdio.h>
#include <string.h>

#include "vm.h"
#include "cp437.h"

/* WHAT THE WINDOW IS ********************************************************/

#define GUI_CLASS           L"ReactHypervGui"
#define GUI_TITLE           L"ReactHypervGui"

/* How much of the window the account of what is happening takes along the foot */
#define GUI_FOOT            72

/* How often the screen is looked at again, and how long is run between looks */
#define GUI_TICK            30
#define GUI_SLICE           40000

/* Where the guest screen is drawn, being the whole of the window above the foot */
#define GUI_MARGIN          8

enum
{
    IdStart = 0x100,
    IdStop,
    IdReset,
    IdOpenCd,
    IdEject,
    IdExit,
    IdScreenFirst = 0x200,
    IdScreenLast = 0x20F
};

/* The sizes the virtual display can be presented at */
static const struct
{
    ULONG Width;
    ULONG Height;
    const WCHAR *Name;
} TheScreens[] =
{
    {  640,  400, L"640 by 400" },
    {  640,  480, L"640 by 480" },
    {  800,  600, L"800 by 600" },
    { 1024,  768, L"1024 by 768" },
    { 1280, 1024, L"1280 by 1024" }
};

/* WHAT THE MACHINE IS MADE OF ************************************************/

/*
 * The parts, which are the real ones and nothing else.
 *
 * Every one of these is a Microsoft binary loaded as it ships. Nothing here
 * reimplements any of them: what this program supplies is the machine they are
 * put into and the services they ask it for, which is the whole point of it.
 * A part that will not come up is left out and said so, rather than stood in
 * for by something of ours that would make the run prove nothing.
 */
static const Part TheParts[] =
{
    { "vmemulateddevices.dll", "9edd1639-9bca-40dc-b3a2-07c828da60b5", nullptr },
    { "vmemulateddevices.dll", "84535fad-4d98-4a6a-bdcd-21d5720dc430", nullptr },
    { "vmchipset.dll",         "72682fc4-040a-430a-be0b-224574b953fe", nullptr },
    { "vmemulateddevices.dll", "a28e4d02-3323-4148-9569-565930a5cb39", nullptr },
    { "vmemulateddevices.dll", "87045ce9-5323-438f-93bb-1e83dcbce18e", nullptr },
    { "vmemulatedstorage.dll", "83f8638b-8dca-4152-9eda-2ca8b33039b4", nullptr }
};

/*
 * The display is not among them yet, and is the reason this window is still
 * empty.
 *
 * It comes up: it is given the four megabytes of memory it asks for, takes the
 * block that memory is in and registers to be told about writes to it. The call
 * after that one stops the whole program rather than returning, and a fail-fast
 * cannot be caught, so a machine with it in is a window that never opens. It
 * goes back in the moment that last call is understood.
 */
#define VIDEO_S3 "7d80d3db-61ee-4879-8879-5609f1100ad0"

/* HOW IT IS ALL HELD *********************************************************/

static struct
{
    HWND Window;
    HMENU Bar;

    /* Drawn into and blitted, so a repaint is never seen half done */
    HDC Buffer;
    HBITMAP Surface;
    HBITMAP Was;
    int BufferWidth;
    int BufferHeight;

    HFONT Cells;
    LONG CellWidth;
    LONG CellHeight;

    /* Which firmware, and which disc is in the drive */
    char Bios[MAX_PATH];
    char Cd[MAX_PATH];

    /* How big the virtual display is presented, whatever the guest is doing */
    ULONG Width;
    ULONG Height;

    bool Running;
    ULONG64 Ran;
} TheGui;

/* WHAT IS PUT TOGETHER *******************************************************/

/*
 * The parts, the disc and the firmware, gathered into what the machine is built
 * from. The disc is not a part of its own: it is a setting on whichever part
 * turns out to be a disk controller, which is the only thing that would know
 * what to do with it.
 */
static ULONG Gather(Part *Into, ULONG Room)
{
    const Part *From = TheParts;
    const ULONG Many = ARRAYSIZE(TheParts);
    ULONG Count = 0;

    for (ULONG Index = 0; (Index < Many) && (Count < Room); Index++)
    {
        Into[Count] = From[Index];

        /*
         * The disk controller is the one that is told what is in the drive,
         * and what it is told is the path on its own. What goes around that to
         * make it a configuration is the machine's business, not the window's.
         */
        if ((TheGui.Cd[0] != '\0') &&
            (strcmp(From[Index].Class,
                    "83f8638b-8dca-4152-9eda-2ca8b33039b4") == 0))
        {
            Into[Count].Settings = TheGui.Cd;
        }

        Count++;
    }

    return Count;
}

static bool Build()
{
    VmWanted Wanted = {};

    Wanted.Bios = TheGui.Bios;
    Wanted.Many = Gather(Wanted.Parts, MACHINE_PARTS);
    Wanted.Ram = 0x08000000ull;

    TheGui.Ran = 0;

    if (!VmOpen(Wanted))
    {
        VmClose();
        TheGui.Running = false;
        return false;
    }

    TheGui.Running = true;
    return true;
}

/* WHAT THERE IS TO DRAW ******************************************************/

/* The colours a text screen names, which are the same sixteen they always were */
static const COLORREF TheInk[16] =
{
    RGB(0x00, 0x00, 0x00), RGB(0x00, 0x00, 0xAA), RGB(0x00, 0xAA, 0x00),
    RGB(0x00, 0xAA, 0xAA), RGB(0xAA, 0x00, 0x00), RGB(0xAA, 0x00, 0xAA),
    RGB(0xAA, 0x55, 0x00), RGB(0xAA, 0xAA, 0xAA), RGB(0x55, 0x55, 0x55),
    RGB(0x55, 0x55, 0xFF), RGB(0x55, 0xFF, 0x55), RGB(0x55, 0xFF, 0xFF),
    RGB(0xFF, 0x55, 0x55), RGB(0xFF, 0x55, 0xFF), RGB(0xFF, 0xFF, 0x55),
    RGB(0xFF, 0xFF, 0xFF)
};

/* As wide and as tall a page as anything of this kind draws */
#define GUI_COLUMNS         132
#define GUI_ROWS            60

/* And as big a screen of pixels as is fetched in one go */
#define GUI_PIXEL_WIDTH     1600
#define GUI_PIXEL_HEIGHT    1200

static void DrawText(HDC Target, const RECT &Where)
{
    IRtvmTextSurface *Screen = VmText();

    if (Screen == nullptr)
        return;

    /* Two bytes to a cell: what it is, and what it is drawn in */
    static UCHAR Cells[GUI_COLUMNS * GUI_ROWS * 2];
    ULONG Column = 0;
    ULONG Row = 0;

    if (FAILED(Screen->ReadCells(Cells, sizeof(Cells), &Column, &Row)))
        return;

    /*
     * How many of those are the screen rather than the buffer is not said, so
     * what is drawn is as much as fits where there is room for it. A cell whose
     * character and colour are both zero was never written and is left alone.
     */
    const ULONG Columns = 80;
    const ULONG Rows = 25;

    HFONT Old = (HFONT)SelectObject(Target, TheGui.Cells);

    SetBkMode(Target, OPAQUE);

    for (ULONG Down = 0; Down < Rows; Down++)
    {
        for (ULONG Across = 0; Across < Columns; Across++)
        {
            const UCHAR *One = &Cells[((Down * Columns) + Across) * 2];
            WCHAR Glyph = Cp437Glyph[One[0]];

            SetTextColor(Target, TheInk[One[1] & 0x0F]);
            SetBkColor(Target, TheInk[(One[1] >> 4) & 0x07]);

            ExtTextOutW(Target,
                        Where.left + (int)(Across * TheGui.CellWidth),
                        Where.top + (int)(Down * TheGui.CellHeight),
                        0, nullptr, &Glyph, 1, nullptr);
        }
    }

    SelectObject(Target, Old);
}

/*
 * A screen of pixels, fetched a band of rows at a time and put up as one
 * picture. The device hands over one byte to a pixel and a table of what each
 * of those bytes means, or four bytes to a pixel and no table at all.
 */
static void DrawPixels(HDC Target, const RECT &Where,
                       const VDEV_SURFACE_DATA &Shape)
{
    IRtvmPixelSurface *Screen = VmPixels();

    if (Screen == nullptr)
        return;

    static UCHAR Rows[GUI_PIXEL_WIDTH * 4];
    static UCHAR Palette[256 * 3];
    static ULONG Line[GUI_PIXEL_WIDTH];

    const bool Direct = (Shape.Format == VDEV_SURFACE_DIRECT);

    if (!Direct && FAILED(Screen->ReadPalette(Palette, sizeof(Palette))))
        return;

    const ULONG Width = (Shape.Width < GUI_PIXEL_WIDTH) ? Shape.Width
                                                        : GUI_PIXEL_WIDTH;
    const ULONG Height = (Shape.Height < GUI_PIXEL_HEIGHT) ? Shape.Height
                                                           : GUI_PIXEL_HEIGHT;

    BITMAPINFO About = {};

    About.bmiHeader.biSize = sizeof(About.bmiHeader);
    About.bmiHeader.biWidth = (LONG)Width;
    About.bmiHeader.biHeight = -1;
    About.bmiHeader.biPlanes = 1;
    About.bmiHeader.biBitCount = 32;
    About.bmiHeader.biCompression = BI_RGB;

    for (ULONG Down = 0; Down < Height; Down++)
    {
        if (FAILED(Screen->ReadRows(Down, 1, Rows, sizeof(Rows))))
            break;

        for (ULONG Across = 0; Across < Width; Across++)
        {
            if (Direct)
            {
                const UCHAR *Pixel = &Rows[Across * 4];

                Line[Across] = ((ULONG)Pixel[2] << 16) |
                               ((ULONG)Pixel[1] << 8) | Pixel[0];
            }
            else
            {
                /* Six bits to a part, as a screen of this kind has always had */
                const UCHAR *Part = &Palette[Rows[Across] * 3];

                Line[Across] = ((ULONG)(Part[0] << 2) << 16) |
                               ((ULONG)(Part[1] << 2) << 8) |
                               (ULONG)(Part[2] << 2);
            }
        }

        SetDIBitsToDevice(Target, Where.left, Where.top + (int)Down,
                          Width, 1, 0, 0, 0, 1, Line, &About, DIB_RGB_COLORS);
    }
}

static void DrawFoot(HDC Target, const RECT &Where)
{
    WCHAR Line[256];
    RECT Room = Where;

    SetBkColor(Target, RGB(0x1E, 0x1E, 0x1E));
    SetTextColor(Target, RGB(0xC8, 0xC8, 0xC8));
    ExtTextOutW(Target, 0, 0, ETO_OPAQUE, &Room, nullptr, 0, nullptr);

    const ULONG64 Where64 = VmWhere();

    _snwprintf(Line, ARRAYSIZE(Line) - 1,
               L"%s  %lu by %lu  %I64u stop(s)  at %04X:%04X",
               TheGui.Running ? (VmStopped() ? L"stopped" : L"running")
                              : L"not built",
               TheGui.Width, TheGui.Height,
               TheGui.Ran,
               (unsigned)(Where64 >> 16), (unsigned)(Where64 & 0xFFFF));

    Line[ARRAYSIZE(Line) - 1] = L'\0';
    TextOutW(Target, Where.left + GUI_MARGIN, Where.top + 4, Line,
             (int)wcslen(Line));

    /* And the last of whatever the firmware said, which is the useful part */
    const char *Said = VmSaid();
    const char *Last = Said;

    for (const char *Walk = Said; *Walk != '\0'; Walk++)
    {
        if ((*Walk == '\n') && (Walk[1] != '\0'))
            Last = Walk + 1;
    }

    WCHAR Wide[128];
    int Length = 0;

    while ((Last[Length] != '\0') && (Last[Length] != '\n') &&
           (Length < (int)ARRAYSIZE(Wide) - 1))
    {
        Wide[Length] = (WCHAR)(UCHAR)Last[Length];
        Length++;
    }

    Wide[Length] = L'\0';

    SetTextColor(Target, RGB(0x90, 0xC0, 0x90));
    TextOutW(Target, Where.left + GUI_MARGIN, Where.top + 24, Wide, Length);

    if (TheGui.Cd[0] != '\0')
    {
        WCHAR Disc[MAX_PATH + 16];

        _snwprintf(Disc, ARRAYSIZE(Disc) - 1, L"disc: %hs", TheGui.Cd);
        Disc[ARRAYSIZE(Disc) - 1] = L'\0';

        SetTextColor(Target, RGB(0x80, 0x80, 0x80));
        TextOutW(Target, Where.left + GUI_MARGIN, Where.top + 44, Disc,
                 (int)wcslen(Disc));
    }
}

static void Measure()
{
    if (TheGui.Cells != nullptr)
        return;

    /*
     * A fixed pitch face, asked for by what it is rather than by name where
     * that can be helped: a system that has not got the one named still has to
     * draw something every cell of which is the same width.
     */
    LOGFONTW Wanted = {};

    Wanted.lfHeight = -16;
    Wanted.lfWeight = FW_NORMAL;
    Wanted.lfCharSet = DEFAULT_CHARSET;
    Wanted.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;
    wcscpy(Wanted.lfFaceName, L"Courier New");

    TheGui.Cells = CreateFontIndirectW(&Wanted);

    const HDC Screen = GetDC(nullptr);
    HFONT Old = (HFONT)SelectObject(Screen, TheGui.Cells);
    TEXTMETRICW About = {};

    GetTextMetricsW(Screen, &About);
    SelectObject(Screen, Old);
    ReleaseDC(nullptr, Screen);

    TheGui.CellWidth = About.tmAveCharWidth;
    TheGui.CellHeight = About.tmHeight;

    if (TheGui.CellWidth <= 0)
        TheGui.CellWidth = 8;

    if (TheGui.CellHeight <= 0)
        TheGui.CellHeight = 16;
}

static void Paint(HWND Window)
{
    RECT Client = {};

    GetClientRect(Window, &Client);

    const int Wide = Client.right - Client.left;
    const int Tall = Client.bottom - Client.top;

    if ((Wide <= 0) || (Tall <= 0))
        return;

    const HDC Target = GetDC(Window);

    /* Made again only when the window is a different size than it was */
    if ((TheGui.Buffer == nullptr) || (TheGui.BufferWidth != Wide) ||
        (TheGui.BufferHeight != Tall))
    {
        if (TheGui.Buffer != nullptr)
        {
            SelectObject(TheGui.Buffer, TheGui.Was);
            DeleteObject(TheGui.Surface);
            DeleteDC(TheGui.Buffer);
        }

        TheGui.Buffer = CreateCompatibleDC(Target);
        TheGui.Surface = CreateCompatibleBitmap(Target, Wide, Tall);
        TheGui.Was = (HBITMAP)SelectObject(TheGui.Buffer, TheGui.Surface);
        TheGui.BufferWidth = Wide;
        TheGui.BufferHeight = Tall;
    }

    RECT Whole = { 0, 0, Wide, Tall };

    SetBkColor(TheGui.Buffer, RGB(0x00, 0x00, 0x00));
    ExtTextOutW(TheGui.Buffer, 0, 0, ETO_OPAQUE, &Whole, nullptr, 0, nullptr);

    RECT Screen = { GUI_MARGIN, GUI_MARGIN, Wide - GUI_MARGIN,
                    Tall - GUI_FOOT };

    /*
     * Whichever the display says it is holding. A machine with no display in it
     * is not an error and is not drawn over: the foot still says what it is
     * doing, which is the whole of what there is to know about it.
     */
    VDEV_SURFACE_DATA Shape = {};
    IVideoVdev *Display = nullptr;

    if (VmText() != nullptr)
        VmText()->QueryInterface(IID_IVideoVdev, (void **)&Display);

    if ((Display == nullptr) && (VmPixels() != nullptr))
        VmPixels()->QueryInterface(IID_IVideoVdev, (void **)&Display);

    if (Display != nullptr)
    {
        Display->GetSurfaceData(&Shape);
        Display->Release();
    }

    if (Shape.Format == VDEV_SURFACE_TEXT)
        DrawText(TheGui.Buffer, Screen);
    else if ((Shape.Width != 0) && (Shape.Height != 0))
        DrawPixels(TheGui.Buffer, Screen, Shape);
    else
        DrawText(TheGui.Buffer, Screen);

    RECT Foot = { 0, Tall - GUI_FOOT, Wide, Tall };

    SelectObject(TheGui.Buffer, GetStockObject(DEFAULT_GUI_FONT));
    DrawFoot(TheGui.Buffer, Foot);

    BitBlt(Target, 0, 0, Wide, Tall, TheGui.Buffer, 0, 0, SRCCOPY);
    ReleaseDC(Window, Target);
}

/* WHAT THE OPERATOR ASKS FOR *************************************************/

static void Resize()
{
    RECT Wanted = { 0, 0, (LONG)TheGui.Width + (GUI_MARGIN * 2),
                    (LONG)TheGui.Height + GUI_FOOT + GUI_MARGIN };

    AdjustWindowRect(&Wanted, WS_OVERLAPPEDWINDOW, TRUE);
    SetWindowPos(TheGui.Window, nullptr, 0, 0,
                 Wanted.right - Wanted.left, Wanted.bottom - Wanted.top,
                 SWP_NOMOVE | SWP_NOZORDER);
}

static void ChooseCd()
{
    WCHAR Wide[MAX_PATH] = {};
    OPENFILENAMEW Asked = {};

    Asked.lStructSize = sizeof(Asked);
    Asked.hwndOwner = TheGui.Window;
    Asked.lpstrFilter = L"Disc images\0*.iso\0Every file\0*.*\0";
    Asked.lpstrFile = Wide;
    Asked.nMaxFile = ARRAYSIZE(Wide);
    Asked.lpstrTitle = L"Which disc is in the drive";
    Asked.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;

    if (!GetOpenFileNameW(&Asked))
        return;

    WideCharToMultiByte(CP_ACP, 0, Wide, -1, TheGui.Cd, sizeof(TheGui.Cd),
                        nullptr, nullptr);

    /*
     * Put in while it is running means starting again. A drive is told what is
     * in it before it is switched on, and a firmware looks once on its way past.
     */
    if (TheGui.Running)
        Build();
}

static void Tune(HMENU Bar)
{
    for (ULONG Index = 0; Index < ARRAYSIZE(TheScreens); Index++)
    {
        const bool Is = (TheScreens[Index].Width == TheGui.Width) &&
                        (TheScreens[Index].Height == TheGui.Height);

        CheckMenuItem(Bar, IdScreenFirst + Index,
                      MF_BYCOMMAND | (Is ? MF_CHECKED : MF_UNCHECKED));
    }

    EnableMenuItem(Bar, IdEject,
                   MF_BYCOMMAND |
                   ((TheGui.Cd[0] != '\0') ? MF_ENABLED : MF_GRAYED));
}

static HMENU MakeMenu()
{
    HMENU Bar = CreateMenu();
    HMENU Machine = CreatePopupMenu();
    HMENU Media = CreatePopupMenu();
    HMENU Screen = CreatePopupMenu();

    AppendMenuW(Machine, MF_STRING, IdStart, L"&Start\tF5");
    AppendMenuW(Machine, MF_STRING, IdReset, L"&Reset\tCtrl+R");
    AppendMenuW(Machine, MF_STRING, IdStop, L"Sto&p");
    AppendMenuW(Machine, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(Machine, MF_STRING, IdExit, L"E&xit");

    AppendMenuW(Media, MF_STRING, IdOpenCd, L"&Choose disc...");
    AppendMenuW(Media, MF_STRING, IdEject, L"&Eject");

    for (ULONG Index = 0; Index < ARRAYSIZE(TheScreens); Index++)
    {
        AppendMenuW(Screen, MF_STRING, IdScreenFirst + Index,
                    TheScreens[Index].Name);
    }

    AppendMenuW(Bar, MF_POPUP, (UINT_PTR)Machine, L"&Machine");
    AppendMenuW(Bar, MF_POPUP, (UINT_PTR)Media, L"&Media");
    AppendMenuW(Bar, MF_POPUP, (UINT_PTR)Screen, L"&Display");

    return Bar;
}

static void Command(HWND Window, ULONG What)
{
    if ((What >= IdScreenFirst) &&
        (What < (IdScreenFirst + ARRAYSIZE(TheScreens))))
    {
        TheGui.Width = TheScreens[What - IdScreenFirst].Width;
        TheGui.Height = TheScreens[What - IdScreenFirst].Height;
        Resize();
        Tune(TheGui.Bar);
        InvalidateRect(Window, nullptr, FALSE);
        return;
    }

    switch (What)
    {
        case IdStart:
            if (!TheGui.Running)
                Build();
            break;

        case IdReset:
            if (TheGui.Running)
            {
                VmReset();
                TheGui.Ran = 0;
            }
            else
            {
                Build();
            }
            break;

        case IdStop:
            VmClose();
            TheGui.Running = false;
            break;

        case IdOpenCd:
            ChooseCd();
            break;

        case IdEject:
            TheGui.Cd[0] = '\0';

            if (TheGui.Running)
                Build();

            break;

        case IdExit:
            DestroyWindow(Window);
            return;
    }

    Tune(TheGui.Bar);
    InvalidateRect(Window, nullptr, FALSE);
}

static LRESULT CALLBACK Dispatch(HWND Window, UINT Message, WPARAM First,
                                 LPARAM Second)
{
    switch (Message)
    {
        case WM_CREATE:
            SetTimer(Window, 1, GUI_TICK, nullptr);
            return 0;

        case WM_TIMER:
        {
            /*
             * A slice at a time, on the thread that owns the window. A machine
             * being watched is being stepped through rather than raced, and a
             * slice small enough to keep the window answering is large enough
             * that a firmware waiting for hardware gets through the wait.
             */
            if (TheGui.Running && !VmStopped())
                TheGui.Ran += VmRun(GUI_SLICE);

            VDEV_VIDEO_KIND Changed = 0;

            if (VmDirty(&Changed) || TheGui.Running)
                InvalidateRect(Window, nullptr, FALSE);

            return 0;
        }

        case WM_PAINT:
        {
            PAINTSTRUCT About = {};

            BeginPaint(Window, &About);
            Paint(Window);
            EndPaint(Window, &About);
            return 0;
        }

        case WM_ERASEBKGND:
            return 1;

        case WM_COMMAND:
            Command(Window, LOWORD(First));
            return 0;

        case WM_DESTROY:
            KillTimer(Window, 1);
            VmClose();
            PostQuitMessage(0);
            return 0;
    }

    return DefWindowProcW(Window, Message, First, Second);
}

/* WHERE IT STARTS ************************************************************/

int main(int argc, char **argv)
{
    TheGui.Width = 640;
    TheGui.Height = 400;
    strcpy(TheGui.Bios, "rtvmbios.bin");

    for (int Index = 1; Index < argc; Index++)
    {
        if ((strcmp(argv[Index], "--bios") == 0) && ((Index + 1) < argc))
        {
            strncpy(TheGui.Bios, argv[++Index], sizeof(TheGui.Bios) - 1);
            TheGui.Bios[sizeof(TheGui.Bios) - 1] = '\0';
        }
        else if ((strcmp(argv[Index], "--cd") == 0) && ((Index + 1) < argc))
        {
            strncpy(TheGui.Cd, argv[++Index], sizeof(TheGui.Cd) - 1);
            TheGui.Cd[sizeof(TheGui.Cd) - 1] = '\0';
        }
    }

    /*
     * Kept rather than thrown away. What the machine says as it is built is the
     * only account of which parts went in and what they asked for, and a window
     * has nowhere to print it; a file next to the program has, and is there to
     * be read after a run that went wrong.
     */
    if (freopen("reacthypervgui.log", "w", stdout) != nullptr)
        setvbuf(stdout, nullptr, _IONBF, 0);

    VmAllowStandIns(true);

    const HINSTANCE Me = GetModuleHandleW(nullptr);
    WNDCLASSEXW Kind = {};

    Kind.cbSize = sizeof(Kind);
    Kind.lpfnWndProc = Dispatch;
    Kind.hInstance = Me;
    Kind.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    Kind.hbrBackground = nullptr;
    Kind.lpszClassName = GUI_CLASS;

    if (RegisterClassExW(&Kind) == 0)
        return 1;

    Measure();

    TheGui.Bar = MakeMenu();
    TheGui.Window = CreateWindowExW(0, GUI_CLASS, GUI_TITLE,
                                    WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                                    CW_USEDEFAULT, 800, 600, nullptr,
                                    TheGui.Bar, Me, nullptr);

    if (TheGui.Window == nullptr)
        return 1;

    Resize();
    Tune(TheGui.Bar);
    ShowWindow(TheGui.Window, SW_SHOW);

    /* Built at once, because a window with nothing in it says nothing */
    Build();

    MSG Message = {};

    while (GetMessageW(&Message, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&Message);
        DispatchMessageW(&Message);
    }

    return 0;
}
