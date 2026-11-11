/*
 * PROJECT:     ReactOS Mobile Shell
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Docked touch keyboard that types into the focused window
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "mobileshell.h"

#define MS_KEY_ROWS         5
#define MS_KEYS_PER_ROW     12
#define MS_KEY_NONE         (-1)

typedef enum _MS_KEY_ACTION
{
    MsKeyEnd = 0,
    MsKeyChar,
    MsKeyVirtual,
    MsKeyShift,
    MsKeyLayer,
    MsKeyGap
} MS_KEY_ACTION;

typedef enum _MS_SHIFT_STATE
{
    MsShiftOff,
    MsShiftOnce,
    MsShiftLocked
} MS_SHIFT_STATE;

typedef struct _MS_KEY
{
    MS_KEY_ACTION Action;
    WCHAR Char;
    UINT Virtual;
    PCWSTR Label;
    /* In tenths of a standard key */
    UCHAR Width;
} MS_KEY;

#define CHR(c)          { MsKeyChar, c, 0, NULL, 10 }
#define VKEY(v, l, w)   { MsKeyVirtual, 0, v, l, w }
#define GAP(w)          { MsKeyGap, 0, 0, NULL, w }
#define END             { MsKeyEnd, 0, 0, NULL, 0 }

static const MS_KEY MsLetterLayout[MS_KEY_ROWS][MS_KEYS_PER_ROW] =
{
    { CHR(L'1'), CHR(L'2'), CHR(L'3'), CHR(L'4'), CHR(L'5'), CHR(L'6'), CHR(L'7'), CHR(L'8'), CHR(L'9'), CHR(L'0'), END },
    { CHR(L'q'), CHR(L'w'), CHR(L'e'), CHR(L'r'), CHR(L't'), CHR(L'y'), CHR(L'u'), CHR(L'i'), CHR(L'o'), CHR(L'p'), END },
    { GAP(5), CHR(L'a'), CHR(L's'), CHR(L'd'), CHR(L'f'), CHR(L'g'), CHR(L'h'), CHR(L'j'), CHR(L'k'), CHR(L'l'), GAP(5), END },
    { { MsKeyShift, 0, 0, L"Shift", 15 }, CHR(L'z'), CHR(L'x'), CHR(L'c'), CHR(L'v'), CHR(L'b'), CHR(L'n'), CHR(L'm'),
      VKEY(VK_BACK, L"\x2190", 15), END },
    { { MsKeyLayer, 0, 0, L"?123", 15 }, CHR(L','), { MsKeyChar, L' ', 0, L"space", 50 }, CHR(L'.'),
      VKEY(VK_RETURN, L"Enter", 15), END },
};

static const MS_KEY MsSymbolLayout[MS_KEY_ROWS][MS_KEYS_PER_ROW] =
{
    { CHR(L'1'), CHR(L'2'), CHR(L'3'), CHR(L'4'), CHR(L'5'), CHR(L'6'), CHR(L'7'), CHR(L'8'), CHR(L'9'), CHR(L'0'), END },
    { CHR(L'!'), CHR(L'@'), CHR(L'#'), CHR(L'$'), CHR(L'%'), CHR(L'^'), CHR(L'&'), CHR(L'*'), CHR(L'('), CHR(L')'), END },
    { CHR(L'-'), CHR(L'_'), CHR(L'='), CHR(L'+'), CHR(L'/'), CHR(L'\\'), CHR(L':'), CHR(L';'), CHR(L'\''), CHR(L'"'), END },
    { VKEY(VK_TAB, L"Tab", 15), CHR(L'<'), CHR(L'>'), CHR(L'?'), CHR(L'['), CHR(L']'), CHR(L'|'), CHR(L'~'),
      VKEY(VK_BACK, L"\x2190", 15), END },
    { { MsKeyLayer, 0, 0, L"ABC", 15 }, CHR(L','), { MsKeyChar, L' ', 0, L"space", 50 }, CHR(L'.'),
      VKEY(VK_RETURN, L"Enter", 15), END },
};

static HWND MsKeyboardWindow;
static INT MsKeyboardHeight;
static BOOL MsKeyboardShown;
static BOOL MsSymbols;
static MS_SHIFT_STATE MsShift;
static INT MsPressedRow = MS_KEY_NONE;
static INT MsPressedKey = MS_KEY_NONE;
static HFONT MsKeyFont;
static HFONT MsKeyLabelFont;
static RECT MsKeyRects[MS_KEY_ROWS][MS_KEYS_PER_ROW];

static
const MS_KEY *
NTAPI
MsCurrentRow(
    _In_ INT Row)
{
    return MsSymbols ? MsSymbolLayout[Row] : MsLetterLayout[Row];
}

/* Rows are laid out in tenths of a key, centred, ten keys spanning the width */
static
VOID
NTAPI
MsLayoutKeys(
    _In_ INT Width,
    _In_ INT Height)
{
    INT Unit = Width / 100, RowHeight = Height / MS_KEY_ROWS, Gap = max(2, Width / 160);
    INT Row, Key, Tenths, Left;

    for (Row = 0; Row < MS_KEY_ROWS; Row++)
    {
        const MS_KEY *Keys = MsCurrentRow(Row);

        Tenths = 0;
        for (Key = 0; Keys[Key].Action != MsKeyEnd; Key++)
            Tenths += Keys[Key].Width;

        Left = (Width - Tenths * Unit) / 2;
        for (Key = 0; Keys[Key].Action != MsKeyEnd; Key++)
        {
            RECT *Rect = &MsKeyRects[Row][Key];

            Rect->left = Left + Gap;
            Rect->right = Left + Keys[Key].Width * Unit - Gap;
            Rect->top = Row * RowHeight + Gap;
            Rect->bottom = (Row + 1) * RowHeight - Gap;
            Left += Keys[Key].Width * Unit;
        }
    }
}

static
BOOL
NTAPI
MsHitKey(
    _In_ POINT Point,
    _Out_ PINT HitRow,
    _Out_ PINT HitKey)
{
    INT Row, Key;

    for (Row = 0; Row < MS_KEY_ROWS; Row++)
    {
        const MS_KEY *Keys = MsCurrentRow(Row);

        for (Key = 0; Keys[Key].Action != MsKeyEnd; Key++)
        {
            if (Keys[Key].Action != MsKeyGap && PtInRect(&MsKeyRects[Row][Key], Point))
            {
                *HitRow = Row;
                *HitKey = Key;
                return TRUE;
            }
        }
    }

    return FALSE;
}

static
VOID
NTAPI
MsSendVirtualKey(
    _In_ UINT Virtual,
    _In_ BOOL Shift)
{
    INPUT Inputs[4];
    UINT Count = 0;

    ZeroMemory(Inputs, sizeof(Inputs));

    if (Shift)
    {
        Inputs[Count].type = INPUT_KEYBOARD;
        Inputs[Count++].ki.wVk = VK_SHIFT;
    }

    Inputs[Count].type = INPUT_KEYBOARD;
    Inputs[Count++].ki.wVk = (WORD)Virtual;

    Inputs[Count].type = INPUT_KEYBOARD;
    Inputs[Count].ki.wVk = (WORD)Virtual;
    Inputs[Count++].ki.dwFlags = KEYEVENTF_KEYUP;

    if (Shift)
    {
        Inputs[Count].type = INPUT_KEYBOARD;
        Inputs[Count].ki.wVk = VK_SHIFT;
        Inputs[Count++].ki.dwFlags = KEYEVENTF_KEYUP;
    }

    SendInput(Count, Inputs, sizeof(INPUT));
}

/**
 * @brief
 * Types one character into the focused window.
 *
 * @remarks
 * A character the active layout has a key for goes as that key, so consoles see real key events.
 * Anything else goes as a Unicode packet.
 */
static
VOID
NTAPI
MsSendChar(
    _In_ WCHAR Char)
{
    SHORT Scan = VkKeyScanW(Char);
    INPUT Inputs[2];

    /* Only plain and shifted keys are pressed, AltGr combinations go as packets */
    if (Scan != -1 && (HIBYTE(Scan) & ~1) == 0)
    {
        MsSendVirtualKey(LOBYTE(Scan), (HIBYTE(Scan) & 1) != 0);
        return;
    }

    ZeroMemory(Inputs, sizeof(Inputs));
    Inputs[0].type = INPUT_KEYBOARD;
    Inputs[0].ki.wScan = Char;
    Inputs[0].ki.dwFlags = KEYEVENTF_UNICODE;
    Inputs[1] = Inputs[0];
    Inputs[1].ki.dwFlags |= KEYEVENTF_KEYUP;
    SendInput(2, Inputs, sizeof(INPUT));
}

static
WCHAR
NTAPI
MsShiftedChar(
    _In_ WCHAR Char)
{
    if (MsShift != MsShiftOff && Char >= L'a' && Char <= L'z')
        return Char - L'a' + L'A';

    return Char;
}

static
VOID
NTAPI
MsActivateKey(
    _In_ const MS_KEY *Key)
{
    RECT Client;

    switch (Key->Action)
    {
        case MsKeyChar:
            MsSendChar(MsShiftedChar(Key->Char));
            if (MsShift == MsShiftOnce)
                MsShift = MsShiftOff;
            break;

        case MsKeyVirtual:
            MsSendVirtualKey(Key->Virtual, FALSE);
            break;

        /* Off, one letter, locked, off */
        case MsKeyShift:
            MsShift = (MsShift == MsShiftOff) ? MsShiftOnce : (MsShift == MsShiftOnce) ? MsShiftLocked : MsShiftOff;
            break;

        case MsKeyLayer:
            MsSymbols = !MsSymbols;
            MsShift = MsShiftOff;
            GetClientRect(MsKeyboardWindow, &Client);
            MsLayoutKeys(Client.right, Client.bottom);
            break;

        default:
            break;
    }
}

static
VOID
NTAPI
MsDrawKey(
    _In_ HDC Dc,
    _In_ const MS_KEY *Key,
    _In_ const RECT *Rect,
    _In_ BOOL Pressed)
{
    WCHAR Text[2];
    PCWSTR Label = Key->Label;
    COLORREF Fill = MS_COLOR_KEY;
    RECT TextRect = *Rect;

    if (Key->Action != MsKeyChar || Key->Char == L' ')
        Fill = MS_COLOR_KEY_SPECIAL;
    if (Key->Action == MsKeyShift && MsShift != MsShiftOff)
        Fill = MS_COLOR_ACCENT;
    if (Pressed)
        Fill = MS_COLOR_ACCENT;

    MsFillRoundRect(Dc, Rect, (Rect->bottom - Rect->top) / 4, Fill);

    if (!Label)
    {
        Text[0] = MsShiftedChar(Key->Char);
        Text[1] = UNICODE_NULL;
        Label = Text;
    }

    SetTextColor(Dc, (Key->Action == MsKeyShift && MsShift == MsShiftLocked) ? MS_COLOR_BAR : MS_COLOR_TEXT);
    SelectObject(Dc, (Key->Action == MsKeyChar && Key->Char != L' ') ? MsKeyFont : MsKeyLabelFont);
    DrawTextW(Dc, Label, -1, &TextRect, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
}

/* Painted off screen in one go so a tap never flickers the whole keyboard */
static
VOID
NTAPI
MsPaintKeyboard(
    _In_ HWND Window)
{
    PAINTSTRUCT Paint;
    RECT Client;
    HBITMAP Bitmap;
    HGDIOBJ OldBitmap;
    HBRUSH Background;
    HDC Dc = BeginPaint(Window, &Paint), Memory;
    INT Row, Key;

    GetClientRect(Window, &Client);
    Memory = CreateCompatibleDC(Dc);
    Bitmap = CreateCompatibleBitmap(Dc, Client.right, Client.bottom);
    OldBitmap = SelectObject(Memory, Bitmap);

    Background = CreateSolidBrush(MS_COLOR_KEYBOARD);
    FillRect(Memory, &Client, Background);
    DeleteObject(Background);

    SetBkMode(Memory, TRANSPARENT);
    for (Row = 0; Row < MS_KEY_ROWS; Row++)
    {
        const MS_KEY *Keys = MsCurrentRow(Row);

        for (Key = 0; Keys[Key].Action != MsKeyEnd; Key++)
        {
            if (Keys[Key].Action == MsKeyGap)
                continue;

            MsDrawKey(Memory, &Keys[Key], &MsKeyRects[Row][Key], Row == MsPressedRow && Key == MsPressedKey);
        }
    }

    BitBlt(Dc, 0, 0, Client.right, Client.bottom, Memory, 0, 0, SRCCOPY);

    SelectObject(Memory, OldBitmap);
    DeleteObject(Bitmap);
    DeleteDC(Memory);
    EndPaint(Window, &Paint);
}

static
LRESULT
CALLBACK
MsKeyboardProc(
    _In_ HWND Window,
    _In_ UINT Message,
    _In_ WPARAM WParam,
    _In_ LPARAM LParam)
{
    POINT Point;
    INT Row, Key;

    switch (Message)
    {
        /* The window being typed into keeps the focus */
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;

        case WM_SIZE:
            MsLayoutKeys(LOWORD(LParam), HIWORD(LParam));
            return 0;

        case WM_LBUTTONDOWN:
            Point.x = (SHORT)LOWORD(LParam);
            Point.y = (SHORT)HIWORD(LParam);
            if (MsHitKey(Point, &Row, &Key))
            {
                MsPressedRow = Row;
                MsPressedKey = Key;
                SetCapture(Window);
                InvalidateRect(Window, NULL, FALSE);
            }
            return 0;

        /* A key fires when the finger lifts on the key it went down on */
        case WM_LBUTTONUP:
            if (MsPressedRow == MS_KEY_NONE)
                return 0;

            Point.x = (SHORT)LOWORD(LParam);
            Point.y = (SHORT)HIWORD(LParam);
            Row = MsPressedRow;
            Key = MsPressedKey;
            MsPressedRow = MsPressedKey = MS_KEY_NONE;
            ReleaseCapture();

            if (PtInRect(&MsKeyRects[Row][Key], Point))
                MsActivateKey(&MsCurrentRow(Row)[Key]);

            InvalidateRect(Window, NULL, FALSE);
            return 0;

        case WM_CAPTURECHANGED:
            if (MsPressedRow != MS_KEY_NONE)
            {
                MsPressedRow = MsPressedKey = MS_KEY_NONE;
                InvalidateRect(Window, NULL, FALSE);
            }
            return 0;

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT:
            MsPaintKeyboard(Window);
            return 0;

        case WM_CLOSE:
            return 0;

        case WM_SYSCOMMAND:
            if ((WParam & 0xFFF0) == SC_CLOSE)
                return 0;
            break;
    }

    return DefWindowProcW(Window, Message, WParam, LParam);
}

/**
 * @brief
 * Registers the keyboard window class.
 */
BOOL
NTAPI
MsKeyboardRegister(VOID)
{
    WNDCLASSEXW Class;

    ZeroMemory(&Class, sizeof(Class));
    Class.cbSize = sizeof(Class);
    Class.hInstance = MsInstance;
    Class.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    Class.lpfnWndProc = MsKeyboardProc;
    Class.lpszClassName = MS_KEYBOARD_CLASS;
    return RegisterClassExW(&Class) != 0;
}

/**
 * @brief
 * Creates the hidden keyboard docked with its bottom edge at Bottom.
 */
HWND
NTAPI
MsKeyboardCreate(
    _In_ INT Width,
    _In_ INT Bottom)
{
    /* Keys a little taller than wide, never more than half of what is above the bar */
    MsKeyboardHeight = min(Width * 11 / 100 * MS_KEY_ROWS, Bottom / 2);

    MsKeyFont = MsCreateFont(MsKeyboardHeight / MS_KEY_ROWS * 2 / 5, FW_NORMAL);
    MsKeyLabelFont = MsCreateFont(MsKeyboardHeight / MS_KEY_ROWS / 4, FW_SEMIBOLD);

    MsKeyboardWindow = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                       MS_KEYBOARD_CLASS,
                                       NULL,
                                       WS_POPUP,
                                       0,
                                       Bottom - MsKeyboardHeight,
                                       Width,
                                       MsKeyboardHeight,
                                       NULL,
                                       NULL,
                                       MsInstance,
                                       NULL);
    return MsKeyboardWindow;
}

/**
 * @brief
 * Removes the keyboard window, a later MsKeyboardCreate brings it back hidden.
 */
VOID
NTAPI
MsKeyboardDestroy(VOID)
{
    if (MsKeyboardWindow)
        DestroyWindow(MsKeyboardWindow);

    MsKeyboardWindow = NULL;
    MsKeyboardShown = FALSE;

    if (MsKeyFont)
        DeleteObject(MsKeyFont);
    if (MsKeyLabelFont)
        DeleteObject(MsKeyLabelFont);

    MsKeyFont = MsKeyLabelFont = NULL;
}

/**
 * @brief
 * Shows or hides the keyboard and moves the work area out of its way.
 */
VOID
NTAPI
MsKeyboardToggle(VOID)
{
    if (!MsKeyboardWindow)
        return;

    MsKeyboardShown = !MsKeyboardShown;
    MsShift = MsShiftOff;
    ShowWindow(MsKeyboardWindow, MsKeyboardShown ? SW_SHOWNOACTIVATE : SW_HIDE);
    MsUpdateWorkArea();
}

/**
 * @brief
 * Height the keyboard takes from the work area, zero while hidden.
 */
INT
NTAPI
MsKeyboardVisibleHeight(VOID)
{
    return MsKeyboardShown ? MsKeyboardHeight : 0;
}

/**
 * @brief
 * Puts a visible keyboard back above windows made topmost after it.
 */
VOID
NTAPI
MsKeyboardKeepOnTop(VOID)
{
    if (MsKeyboardShown)
        SetWindowPos(MsKeyboardWindow, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}
