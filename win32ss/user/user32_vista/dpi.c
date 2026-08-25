/*
 * PROJECT:     ReactOS Kernel - Vista+ APIs
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     DPI functions for user32 and user32_vista.
 * COPYRIGHT:   Copyright 2024 Carl Bialorucki <cbialo2@outlook.com>
 *              Copyright 2026 ReactOS Contributors
 */

/*
 * There is one display DPI here and no per-monitor scaling, so every monitor
 * answers with the system DPI and that is the truth rather than a placeholder.
 *
 * What a caller sets is a different matter. DPI awareness says what a process
 * or thread is prepared to be told, not what the display does, and callers
 * change it constantly around individual calls and put it back afterwards. So
 * it is tracked and reported back faithfully, which costs nothing and is what
 * a caller reads to decide whether to scale coordinates itself. Since nothing
 * is scaled here, every awareness produces the same coordinates, and a caller
 * that trusts its own reading gets consistent answers.
 *
 * The thing that must not happen is failing. Chromium changes the thread
 * context around window operations and restores it from the returned value;
 * handing back nothing there loses the caller's own state.
 */

#define WIN32_NO_STATUS
#define _INC_WINDOWS
#define COM_NO_WINDOWS_H
#include <windef.h>
#include <winbase.h>
#include <wingdi.h>
#include <winuser.h>

HDC APIENTRY
NtUserGetDC(HWND hWnd);

#define NDEBUG
#include <debug.h>

/*
 * The awareness this process and thread declared. A thread that never set one
 * of its own follows the process, which is what DPI_AWARENESS_CONTEXT_UNAWARE
 * as a starting point means for both.
 */
static DPI_AWARENESS_CONTEXT gProcessDpiContext = DPI_AWARENESS_CONTEXT_UNAWARE;

/*
 * The thread's own context lives in a TLS slot taken on first use rather than
 * in implicit thread-local storage, which user32 cannot rely on: it is loaded
 * into every process, sandboxed ones included. No valid context is NULL, so an
 * empty slot reads as "this thread never set one" and the process value stands.
 */
static DWORD gThreadDpiTlsIndex = TLS_OUT_OF_INDEXES;

static
DWORD
ThreadDpiSlot(VOID)
{
    DWORD Index;

    if (gThreadDpiTlsIndex != TLS_OUT_OF_INDEXES)
        return gThreadDpiTlsIndex;

    Index = TlsAlloc();
    if (Index == TLS_OUT_OF_INDEXES)
        return TLS_OUT_OF_INDEXES;

    if (InterlockedCompareExchange((LONG *)&gThreadDpiTlsIndex,
                                   (LONG)Index,
                                   (LONG)TLS_OUT_OF_INDEXES) != (LONG)TLS_OUT_OF_INDEXES)
    {
        /* Another thread got there first; keep theirs and give this one back */
        TlsFree(Index);
    }

    return gThreadDpiTlsIndex;
}

static
DPI_AWARENESS
DpiAwarenessFromContext(
    _In_ DPI_AWARENESS_CONTEXT Context)
{
    if (Context == DPI_AWARENESS_CONTEXT_UNAWARE ||
        Context == DPI_AWARENESS_CONTEXT_UNAWARE_GDISCALED)
    {
        return DPI_AWARENESS_UNAWARE;
    }

    if (Context == DPI_AWARENESS_CONTEXT_SYSTEM_AWARE)
        return DPI_AWARENESS_SYSTEM_AWARE;

    if (Context == DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE ||
        Context == DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)
    {
        return DPI_AWARENESS_PER_MONITOR_AWARE;
    }

    return DPI_AWARENESS_INVALID;
}

static
DPI_AWARENESS_CONTEXT
DpiContextFromAwareness(
    _In_ DPI_AWARENESS Awareness)
{
    switch (Awareness)
    {
        case DPI_AWARENESS_UNAWARE:
            return DPI_AWARENESS_CONTEXT_UNAWARE;
        case DPI_AWARENESS_SYSTEM_AWARE:
            return DPI_AWARENESS_CONTEXT_SYSTEM_AWARE;
        case DPI_AWARENESS_PER_MONITOR_AWARE:
            return DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE;
        default:
            return NULL;
    }
}

static
DPI_AWARENESS_CONTEXT
CurrentDpiContext(VOID)
{
    DWORD Index = ThreadDpiSlot();
    DPI_AWARENESS_CONTEXT Context = NULL;

    if (Index != TLS_OUT_OF_INDEXES)
        Context = (DPI_AWARENESS_CONTEXT)TlsGetValue(Index);

    return Context ? Context : gProcessDpiContext;
}

/*
 * @implemented
 */
UINT
WINAPI
GetDpiForSystem(VOID)
{
    HDC hDC;
    UINT Dpi;
    hDC = NtUserGetDC(NULL);
    Dpi = GetDeviceCaps(hDC, LOGPIXELSY);
    ReleaseDC(NULL, hDC);
    return Dpi;
}

/*
 * @implemented
 */
UINT
WINAPI
GetDpiForWindow(
    _In_ HWND hWnd)
{
    UNREFERENCED_PARAMETER(hWnd);

    /* Every window is on the one display, at the one DPI */
    return GetDpiForSystem();
}

/*
 * @implemented
 */
INT
WINAPI
GetSystemMetricsForDpi(
    _In_ INT nIndex,
    _In_ UINT dpi)
{
    /*
     * The metric a caller wants is the one that would apply if the window
     * were being drawn at the DPI it names. There is one DPI here, so every
     * answer is the system one, and scaling it by a ratio that is always 1
     * would only introduce rounding.
     *
     * This was a raising stub, which is worse than a wrong number: a caller
     * asks for a border width or a scrollbar size while laying out a window
     * and has no reason to expect an exception from it.
     */
    UNREFERENCED_PARAMETER(dpi);

    return GetSystemMetrics(nIndex);
}

/*
 * @implemented
 */
BOOL
WINAPI
IsProcessDPIAware(VOID)
{
    return DpiAwarenessFromContext(gProcessDpiContext) != DPI_AWARENESS_UNAWARE;
}

/*
 * @implemented
 */
BOOL
WINAPI
SetProcessDPIAware(VOID)
{
    gProcessDpiContext = DPI_AWARENESS_CONTEXT_SYSTEM_AWARE;
    return TRUE;
}

/*
 * @implemented
 */
BOOL
WINAPI
SetProcessDpiAwarenessContext(
    _In_ DPI_AWARENESS_CONTEXT context)
{
    if (DpiAwarenessFromContext(context) == DPI_AWARENESS_INVALID)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    gProcessDpiContext = context;
    return TRUE;
}

/*
 * @implemented
 */
DPI_AWARENESS_CONTEXT
WINAPI
GetThreadDpiAwarenessContext(VOID)
{
    return CurrentDpiContext();
}

/*
 * @implemented
 */
DPI_AWARENESS_CONTEXT
WINAPI
SetThreadDpiAwarenessContext(
    _In_ DPI_AWARENESS_CONTEXT context)
{
    DPI_AWARENESS_CONTEXT Previous = CurrentDpiContext();
    DWORD Index;

    if (DpiAwarenessFromContext(context) == DPI_AWARENESS_INVALID)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }

    Index = ThreadDpiSlot();
    if (Index == TLS_OUT_OF_INDEXES)
    {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }

    TlsSetValue(Index, (LPVOID)context);

    /* The caller restores its old state by handing this straight back */
    return Previous;
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

    /* Windows belonging to another process are not tracked separately here */
    return gProcessDpiContext;
}

/*
 * @implemented
 */
DPI_AWARENESS
WINAPI
GetAwarenessFromDpiAwarenessContext(
    _In_ DPI_AWARENESS_CONTEXT context)
{
    return DpiAwarenessFromContext(context);
}

/*
 * @implemented
 */
BOOL
WINAPI
AreDpiAwarenessContextsEqual(
    _In_ DPI_AWARENESS_CONTEXT contextA,
    _In_ DPI_AWARENESS_CONTEXT contextB)
{
    DPI_AWARENESS AwarenessA = DpiAwarenessFromContext(contextA);

    if (AwarenessA == DPI_AWARENESS_INVALID)
        return FALSE;

    return AwarenessA == DpiAwarenessFromContext(contextB);
}

/*
 * @implemented
 */
BOOL
WINAPI
IsValidDpiAwarenessContext(
    _In_ DPI_AWARENESS_CONTEXT context)
{
    return DpiAwarenessFromContext(context) != DPI_AWARENESS_INVALID;
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
    if (awareness == NULL)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    /* Only this process is tracked; another one is reported as we are */
    UNREFERENCED_PARAMETER(process);

    *awareness = DpiAwarenessFromContext(gProcessDpiContext);
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
    DPI_AWARENESS_CONTEXT Context = DpiContextFromAwareness(awareness);

    if (Context == NULL)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    gProcessDpiContext = Context;
    return TRUE;
}

/*
 * @implemented
 */
BOOL
WINAPI
GetDpiForMonitorInternal(
    _In_  HMONITOR monitor,
    _In_  UINT type,
    _Out_ UINT *x,
    _Out_ UINT *y)
{
    HDC hDC;

    /* MDT_EFFECTIVE_DPI, MDT_ANGULAR_DPI and MDT_RAW_DPI */
    if (monitor == NULL || x == NULL || y == NULL || type > 2)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    /* One display, one DPI, and the same on both axes */
    hDC = NtUserGetDC(NULL);
    *x = GetDeviceCaps(hDC, LOGPIXELSX);
    *y = GetDeviceCaps(hDC, LOGPIXELSY);
    ReleaseDC(NULL, hDC);

    return TRUE;
}

/*
 * @stub
 */
BOOL
WINAPI
LogicalToPhysicalPoint(
    _In_ HWND hwnd,
    _Inout_ POINT *point )
{
    UNIMPLEMENTED;
    return TRUE;
}
