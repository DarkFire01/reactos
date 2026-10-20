/*
 * PROJECT:     ReactOS Kernel - Vista+ APIs
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     DPI functions for user32 and user32_vista.
 * COPYRIGHT:   Copyright 2024 Carl Bialorucki <cbialo2@outlook.com>
 */

/* The Windows 10 DPI functions and the Vista metrics they scale */
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#undef WINVER
#define WINVER 0x0A00

#define WIN32_NO_STATUS
#define _INC_WINDOWS
#define COM_NO_WINDOWS_H
#include <windef.h>
#include <winbase.h>
#include <wingdi.h>
#include <winuser.h>
#include <userdpi.h>

HDC APIENTRY
NtUserGetDC(HWND hWnd);

DWORD_PTR
NTAPI
NtUserCallOneParam(
    DWORD_PTR Param,
    DWORD Routine);

DWORD_PTR
APIENTRY
NtUserQueryWindow(
    HWND hWnd,
    DWORD Index);

#define NDEBUG
#include <debug.h>

/*
 * The contexts Windows hands out: the awareness in the low nibble, 0x10 when the DPI follows from
 * bit 8 or the awareness is version one, 0x20 for version two, and GDI scaling at bit 30.
 */
#define DPI_CONTEXT_UNAWARE             0x00006010
#define DPI_CONTEXT_SYSTEM_AWARE        0x00000011
#define DPI_CONTEXT_PER_MONITOR         0x00000012
#define DPI_CONTEXT_PER_MONITOR_V2      0x00000022
#define DPI_CONTEXT_UNAWARE_GDISCALED   0x40006010

/* The displays' resolution as the calling thread sees it: 96 when the DWM stretches it */
static
UINT
DpiGetDisplayDpi(VOID)
{
    HDC hDC;
    UINT Dpi;

    hDC = NtUserGetDC(NULL);
    Dpi = GetDeviceCaps(hDC, LOGPIXELSY);
    ReleaseDC(NULL, hDC);
    return Dpi ? Dpi : USER_DEFAULT_SCREEN_DPI;
}

/* The resolution a window lives at, and the one of the screen itself */
static
UINT
DpiGetWindowDpi(
    _In_ HWND hWnd)
{
    UINT Dpi = (UINT)NtUserQueryWindow(hWnd, QUERY_WINDOW_DPI);
    return Dpi ? Dpi : USER_DEFAULT_SCREEN_DPI;
}

static
UINT
DpiGetScreenDpi(VOID)
{
    return DpiGetWindowDpi(GetDesktopWindow());
}

static
VOID
DpiScalePoint(
    _Inout_ POINT *Point,
    _In_ UINT To,
    _In_ UINT From)
{
    if (To == From)
        return;
    Point->x = MulDiv(Point->x, To, From);
    Point->y = MulDiv(Point->y, To, From);
}

static
ULONG
DpiAwarenessFromContext(
    _In_ DPI_AWARENESS_CONTEXT Context)
{
    ULONG_PTR Value = (ULONG_PTR)Context;

    if (Context == DPI_AWARENESS_CONTEXT_UNAWARE)
        return USER_DPI_UNAWARE;
    if (Context == DPI_AWARENESS_CONTEXT_SYSTEM_AWARE)
        return USER_DPI_SYSTEM_AWARE;
    if (Context == DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE)
        return USER_DPI_PER_MONITOR_AWARE;
    if (Context == DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)
        return USER_DPI_PER_MONITOR_AWARE_V2;
    if (Context == DPI_AWARENESS_CONTEXT_UNAWARE_GDISCALED)
        return USER_DPI_UNAWARE_GDISCALED;

    switch (Value)
    {
        case DPI_CONTEXT_UNAWARE:
            return USER_DPI_UNAWARE;
        case DPI_CONTEXT_PER_MONITOR:
            return USER_DPI_PER_MONITOR_AWARE;
        case DPI_CONTEXT_PER_MONITOR_V2:
            return USER_DPI_PER_MONITOR_AWARE_V2;
        case DPI_CONTEXT_UNAWARE_GDISCALED:
            return USER_DPI_UNAWARE_GDISCALED;
    }

    /* A system aware context carries the DPI it was made at */
    if ((Value & 0xFF) == DPI_CONTEXT_SYSTEM_AWARE && (Value >> 8) >= 72 && (Value >> 8) <= 480)
        return USER_DPI_SYSTEM_AWARE;
    return USER_DPI_INVALID;
}

static
DPI_AWARENESS_CONTEXT
DpiContextFromAwareness(
    _In_ ULONG Awareness)
{
    switch (Awareness)
    {
        case USER_DPI_UNAWARE:
            return (DPI_AWARENESS_CONTEXT)DPI_CONTEXT_UNAWARE;
        case USER_DPI_SYSTEM_AWARE:
            return (DPI_AWARENESS_CONTEXT)(ULONG_PTR)(DPI_CONTEXT_SYSTEM_AWARE | (DpiGetDisplayDpi() << 8));
        case USER_DPI_PER_MONITOR_AWARE:
            return (DPI_AWARENESS_CONTEXT)DPI_CONTEXT_PER_MONITOR;
        case USER_DPI_PER_MONITOR_AWARE_V2:
            return (DPI_AWARENESS_CONTEXT)DPI_CONTEXT_PER_MONITOR_V2;
        case USER_DPI_UNAWARE_GDISCALED:
            return (DPI_AWARENESS_CONTEXT)DPI_CONTEXT_UNAWARE_GDISCALED;
    }
    return NULL;
}

/* What DPI_AWARENESS a context reduces to */
static
DPI_AWARENESS
DpiPublicAwareness(
    _In_ ULONG Awareness)
{
    switch (Awareness)
    {
        case USER_DPI_UNAWARE:
        case USER_DPI_UNAWARE_GDISCALED:
            return DPI_AWARENESS_UNAWARE;
        case USER_DPI_SYSTEM_AWARE:
            return DPI_AWARENESS_SYSTEM_AWARE;
        case USER_DPI_PER_MONITOR_AWARE:
        case USER_DPI_PER_MONITOR_AWARE_V2:
            return DPI_AWARENESS_PER_MONITOR_AWARE;
    }
    return DPI_AWARENESS_INVALID;
}

/*
 * @implemented
 */
UINT
WINAPI
GetDpiForSystem(VOID)
{
    return DpiGetDisplayDpi();
}

/*
 * @implemented
 */
UINT
WINAPI
GetDpiForWindow(
    _In_ HWND hWnd)
{
    if (!IsWindow(hWnd))
    {
        SetLastError(ERROR_INVALID_WINDOW_HANDLE);
        return 0;
    }
    return DpiGetWindowDpi(hWnd);
}

/*
 * @implemented
 */
UINT
WINAPI
GetSystemDpiForProcess(
    _In_ HANDLE hProcess)
{
    if (NtUserCallOneParam((DWORD_PTR)hProcess, ONEPARAM_ROUTINE_GETPROCESSDPIAWARENESS) == USER_DPI_INVALID)
        return 0;
    return DpiGetDisplayDpi();
}

/*
 * @implemented
 */
BOOL
WINAPI
IsProcessDPIAware(VOID)
{
    DPI_AWARENESS Awareness =
        DpiPublicAwareness((ULONG)NtUserCallOneParam(0, ONEPARAM_ROUTINE_GETPROCESSDPIAWARENESS));

    return Awareness != DPI_AWARENESS_UNAWARE && Awareness != DPI_AWARENESS_INVALID;
}

/*
 * @implemented
 */
BOOL
WINAPI
SetProcessDPIAware(VOID)
{
    /* A process that is aware already stays as it is, and that counts as success */
    if (NtUserCallOneParam(USER_DPI_SYSTEM_AWARE, ONEPARAM_ROUTINE_SETPROCESSDPIAWARENESS))
        return TRUE;
    return IsProcessDPIAware();
}

/*
 * @implemented
 */
BOOL
WINAPI
SetProcessDpiAwarenessContext(
    _In_ DPI_AWARENESS_CONTEXT context)
{
    ULONG Awareness = DpiAwarenessFromContext(context);

    if (Awareness == USER_DPI_INVALID)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    return (BOOL)NtUserCallOneParam(Awareness, ONEPARAM_ROUTINE_SETPROCESSDPIAWARENESS);
}

/*
 * @implemented
 */
BOOL
WINAPI
GetProcessDpiAwarenessInternal(
    _In_  HANDLE process,
    _Out_ DPI_AWARENESS *awareness)
{
    ULONG Awareness = (ULONG)NtUserCallOneParam((DWORD_PTR)process, ONEPARAM_ROUTINE_GETPROCESSDPIAWARENESS);

    if (Awareness == USER_DPI_INVALID)
        return FALSE;
    *awareness = DpiPublicAwareness(Awareness);
    return TRUE;
}

/*
 * @implemented
 */
BOOL
WINAPI
SetProcessDpiAwarenessInternal(
    _In_ DPI_AWARENESS awareness)
{
    static const ULONG Map[] = { USER_DPI_UNAWARE, USER_DPI_SYSTEM_AWARE, USER_DPI_PER_MONITOR_AWARE };

    if ((ULONG)awareness >= ARRAYSIZE(Map))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    return (BOOL)NtUserCallOneParam(Map[awareness], ONEPARAM_ROUTINE_SETPROCESSDPIAWARENESS);
}

/*
 * @implemented
 */
DPI_AWARENESS_CONTEXT
WINAPI
GetDpiAwarenessContextForProcess(
    _In_ HANDLE hProcess)
{
    return DpiContextFromAwareness((ULONG)NtUserCallOneParam((DWORD_PTR)hProcess,
                                                             ONEPARAM_ROUTINE_GETPROCESSDPIAWARENESS));
}

/*
 * @implemented
 */
DPI_AWARENESS_CONTEXT
WINAPI
GetThreadDpiAwarenessContext(VOID)
{
    return DpiContextFromAwareness((ULONG)NtUserCallOneParam(0, ONEPARAM_ROUTINE_GETTHREADDPIAWARENESS));
}

/*
 * @implemented
 */
DPI_AWARENESS_CONTEXT
WINAPI
SetThreadDpiAwarenessContext(
    _In_ DPI_AWARENESS_CONTEXT dpiContext)
{
    ULONG Awareness = DpiAwarenessFromContext(dpiContext);

    if (Awareness == USER_DPI_INVALID)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    return DpiContextFromAwareness((ULONG)NtUserCallOneParam(Awareness, ONEPARAM_ROUTINE_SETTHREADDPIAWARENESS));
}

/*
 * @implemented
 */
DPI_AWARENESS_CONTEXT
WINAPI
GetWindowDpiAwarenessContext(
    _In_ HWND hwnd)
{
    if (!IsWindow(hwnd))
    {
        SetLastError(ERROR_INVALID_WINDOW_HANDLE);
        return NULL;
    }
    return DpiContextFromAwareness((ULONG)NtUserQueryWindow(hwnd, QUERY_WINDOW_DPI_AWARENESS));
}

/*
 * @implemented
 */
DPI_AWARENESS
WINAPI
GetAwarenessFromDpiAwarenessContext(
    _In_ DPI_AWARENESS_CONTEXT value)
{
    return DpiPublicAwareness(DpiAwarenessFromContext(value));
}

/*
 * @implemented
 */
UINT
WINAPI
GetDpiFromDpiAwarenessContext(
    _In_ DPI_AWARENESS_CONTEXT value)
{
    ULONG_PTR Encoded = (ULONG_PTR)value;

    switch (DpiAwarenessFromContext(value))
    {
        case USER_DPI_UNAWARE:
        case USER_DPI_UNAWARE_GDISCALED:
            return USER_DEFAULT_SCREEN_DPI;
        case USER_DPI_SYSTEM_AWARE:
            return (Encoded & 0xFF) == DPI_CONTEXT_SYSTEM_AWARE ? (UINT)(Encoded >> 8) : DpiGetDisplayDpi();
    }
    return 0;
}

/*
 * @implemented
 */
BOOL
WINAPI
AreDpiAwarenessContextsEqual(
    _In_ DPI_AWARENESS_CONTEXT dpiContextA,
    _In_ DPI_AWARENESS_CONTEXT dpiContextB)
{
    ULONG AwarenessA = DpiAwarenessFromContext(dpiContextA);

    return AwarenessA != USER_DPI_INVALID && AwarenessA == DpiAwarenessFromContext(dpiContextB);
}

/*
 * @implemented
 */
BOOL
WINAPI
IsValidDpiAwarenessContext(
    _In_ DPI_AWARENESS_CONTEXT value)
{
    return DpiAwarenessFromContext(value) != USER_DPI_INVALID;
}

/*
 * @implemented
 */
int
WINAPI
GetSystemMetricsForDpi(
    _In_ int nIndex,
    _In_ UINT dpi)
{
    int Value = GetSystemMetrics(nIndex);

    if (UserDpiIsSizeMetric(nIndex))
        return MulDiv(Value, dpi, DpiGetDisplayDpi());
    return Value;
}

/* A font scaled from the displays' resolution to Dpi */
static
VOID
DpiScaleFont(
    _Inout_ LOGFONTW *pFont,
    _In_ UINT Dpi,
    _In_ UINT DisplayDpi)
{
    pFont->lfHeight = MulDiv(pFont->lfHeight, Dpi, DisplayDpi);
}

/*
 * @implemented
 */
BOOL
WINAPI
SystemParametersInfoForDpi(
    _In_ UINT uiAction,
    _In_ UINT uiParam,
    _Pre_maybenull_ _Post_valid_ PVOID pvParam,
    _In_ UINT fWinIni,
    _In_ UINT dpi)
{
    UINT DisplayDpi = DpiGetDisplayDpi();

    switch (uiAction)
    {
        case SPI_GETICONTITLELOGFONT:
            if (!SystemParametersInfoW(uiAction, uiParam, pvParam, 0))
                return FALSE;
            DpiScaleFont(pvParam, dpi, DisplayDpi);
            return TRUE;

        case SPI_GETICONMETRICS:
        {
            ICONMETRICSW *pMetrics = pvParam;
            if (!SystemParametersInfoW(uiAction, uiParam, pvParam, 0))
                return FALSE;
            pMetrics->iHorzSpacing = MulDiv(pMetrics->iHorzSpacing, dpi, DisplayDpi);
            pMetrics->iVertSpacing = MulDiv(pMetrics->iVertSpacing, dpi, DisplayDpi);
            DpiScaleFont(&pMetrics->lfFont, dpi, DisplayDpi);
            return TRUE;
        }

        case SPI_GETNONCLIENTMETRICS:
        {
            NONCLIENTMETRICSW *pMetrics = pvParam;
            if (!SystemParametersInfoW(uiAction, uiParam, pvParam, 0))
                return FALSE;
            pMetrics->iBorderWidth = MulDiv(pMetrics->iBorderWidth, dpi, DisplayDpi);
            pMetrics->iScrollWidth = MulDiv(pMetrics->iScrollWidth, dpi, DisplayDpi);
            pMetrics->iScrollHeight = MulDiv(pMetrics->iScrollHeight, dpi, DisplayDpi);
            pMetrics->iCaptionWidth = MulDiv(pMetrics->iCaptionWidth, dpi, DisplayDpi);
            pMetrics->iCaptionHeight = MulDiv(pMetrics->iCaptionHeight, dpi, DisplayDpi);
            pMetrics->iSmCaptionWidth = MulDiv(pMetrics->iSmCaptionWidth, dpi, DisplayDpi);
            pMetrics->iSmCaptionHeight = MulDiv(pMetrics->iSmCaptionHeight, dpi, DisplayDpi);
            pMetrics->iMenuWidth = MulDiv(pMetrics->iMenuWidth, dpi, DisplayDpi);
            pMetrics->iMenuHeight = MulDiv(pMetrics->iMenuHeight, dpi, DisplayDpi);
            if (pMetrics->cbSize >= sizeof(*pMetrics))
                pMetrics->iPaddedBorderWidth = MulDiv(pMetrics->iPaddedBorderWidth, dpi, DisplayDpi);
            DpiScaleFont(&pMetrics->lfCaptionFont, dpi, DisplayDpi);
            DpiScaleFont(&pMetrics->lfSmCaptionFont, dpi, DisplayDpi);
            DpiScaleFont(&pMetrics->lfMenuFont, dpi, DisplayDpi);
            DpiScaleFont(&pMetrics->lfStatusFont, dpi, DisplayDpi);
            DpiScaleFont(&pMetrics->lfMessageFont, dpi, DisplayDpi);
            return TRUE;
        }
    }

    /* Only the settings that depend on the resolution are taken */
    SetLastError(ERROR_INVALID_PARAMETER);
    return FALSE;
}

/*
 * @implemented
 */
BOOL
WINAPI
AdjustWindowRectExForDpi(
    _Inout_ LPRECT lpRect,
    _In_ DWORD dwStyle,
    _In_ BOOL bMenu,
    _In_ DWORD dwExStyle,
    _In_ UINT dpi)
{
    RECT Frame = { 0, 0, 0, 0 };
    UINT DisplayDpi = DpiGetDisplayDpi();

    /* The frame for the displays' resolution, its parts scaled to dpi */
    if (!AdjustWindowRectEx(&Frame, dwStyle, bMenu, dwExStyle))
        return FALSE;
    lpRect->left += MulDiv(Frame.left, dpi, DisplayDpi);
    lpRect->top += MulDiv(Frame.top, dpi, DisplayDpi);
    lpRect->right += MulDiv(Frame.right, dpi, DisplayDpi);
    lpRect->bottom += MulDiv(Frame.bottom, dpi, DisplayDpi);
    return TRUE;
}

/*
 * @implemented
 *
 * Every window's frame is drawn for the resolution it is shown at already.
 */
BOOL
WINAPI
EnableNonClientDpiScaling(
    _In_ HWND hwnd)
{
    if (!IsWindow(hwnd))
    {
        SetLastError(ERROR_INVALID_WINDOW_HANDLE);
        return FALSE;
    }
    return TRUE;
}

/*
 * @implemented
 */
DPI_HOSTING_BEHAVIOR
WINAPI
SetThreadDpiHostingBehavior(
    _In_ DPI_HOSTING_BEHAVIOR value)
{
    if (value != DPI_HOSTING_BEHAVIOR_DEFAULT && value != DPI_HOSTING_BEHAVIOR_MIXED)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return DPI_HOSTING_BEHAVIOR_INVALID;
    }
    return DPI_HOSTING_BEHAVIOR_DEFAULT;
}

/*
 * @implemented
 */
DPI_HOSTING_BEHAVIOR
WINAPI
GetThreadDpiHostingBehavior(VOID)
{
    return DPI_HOSTING_BEHAVIOR_DEFAULT;
}

/*
 * @implemented
 */
DPI_HOSTING_BEHAVIOR
WINAPI
GetWindowDpiHostingBehavior(
    _In_ HWND hwnd)
{
    if (!IsWindow(hwnd))
    {
        SetLastError(ERROR_INVALID_WINDOW_HANDLE);
        return DPI_HOSTING_BEHAVIOR_INVALID;
    }
    return DPI_HOSTING_BEHAVIOR_DEFAULT;
}

/*
 * @implemented
 *
 * One resolution covers every monitor.
 */
BOOL
WINAPI
GetDpiForMonitorInternal(
    _In_  HMONITOR monitor,
    _In_  UINT type,
    _Out_ UINT *x,
    _Out_ UINT *y)
{
    MONITORINFO Info = { sizeof(Info) };

    if (!GetMonitorInfoW(monitor, &Info))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    *x = *y = DpiGetDisplayDpi();
    return TRUE;
}

/*
 * @implemented
 *
 * A window the DWM stretches lives at 96 DPI; its logical points are the screen's scaled down.
 */
BOOL
WINAPI
LogicalToPhysicalPoint(
    _In_ HWND hwnd,
    _Inout_ POINT *point)
{
    if (!IsWindow(hwnd))
        return FALSE;
    DpiScalePoint(point, DpiGetScreenDpi(), DpiGetWindowDpi(hwnd));
    return TRUE;
}

/*
 * @implemented
 */
BOOL
WINAPI
PhysicalToLogicalPoint(
    _In_ HWND hwnd,
    _Inout_ POINT *point)
{
    if (!IsWindow(hwnd))
        return FALSE;
    DpiScalePoint(point, DpiGetWindowDpi(hwnd), DpiGetScreenDpi());
    return TRUE;
}

/*
 * @implemented
 */
BOOL
WINAPI
LogicalToPhysicalPointForPerMonitorDPI(
    _In_opt_ HWND hWnd,
    _Inout_ LPPOINT lpPoint)
{
    return !hWnd || LogicalToPhysicalPoint(hWnd, lpPoint);
}

/*
 * @implemented
 */
BOOL
WINAPI
PhysicalToLogicalPointForPerMonitorDPI(
    _In_opt_ HWND hWnd,
    _Inout_ LPPOINT lpPoint)
{
    return !hWnd || PhysicalToLogicalPoint(hWnd, lpPoint);
}

/*
 * @implemented
 */
BOOL
WINAPI
GetPhysicalCursorPos(
    _Out_ LPPOINT lpPoint)
{
    if (!GetCursorPos(lpPoint))
        return FALSE;
    DpiScalePoint(lpPoint, DpiGetScreenDpi(), DpiGetDisplayDpi());
    return TRUE;
}

/*
 * @implemented
 */
HWND
WINAPI
WindowFromPhysicalPoint(
    _In_ POINT Point)
{
    DpiScalePoint(&Point, DpiGetDisplayDpi(), DpiGetScreenDpi());
    return WindowFromPoint(Point);
}
