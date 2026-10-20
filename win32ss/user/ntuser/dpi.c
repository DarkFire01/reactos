/*
 * PROJECT:     ReactOS win32 kernel mode subsystem
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     DPI awareness of processes, threads and windows
 */

#include <win32k.h>
DBG_DEFAULT_CHANNEL(UserMisc);

/* Whether the user lets the DWM show unaware applications at 96 DPI, stretched */
static BOOLEAN gbDpiVirtualizationPolicy = FALSE;

VOID
FASTCALL
UserSetDpiVirtualizationPolicy(
    _In_ BOOLEAN bEnabled)
{
    gbDpiVirtualizationPolicy = bEnabled;
}

BOOLEAN
FASTCALL
UserIsDpiVirtualizationActive(VOID)
{
    return gbDpiVirtualizationPolicy && gpsi && gpsi->dmLogPixels != USER_VIRTUAL_DPI && IntDwmIsActive();
}

/* Whether the thread draws at 96 DPI for the DWM to stretch */
BOOLEAN
FASTCALL
UserIsThreadDpiVirtualized(
    _In_opt_ PTHREADINFO pti)
{
    ULONG Awareness;

    if (!pti || !pti->ppi || !pti->ppi->DpiVirtualized)
        return FALSE;
    Awareness = UserGetThreadDpiAwareness(pti);
    return Awareness == USER_DPI_UNAWARE || Awareness == USER_DPI_UNAWARE_GDISCALED;
}

/* user32 and gdi32 read the thread's view of the resolution from its client info */
VOID
FASTCALL
UserUpdateClientDpi(
    _In_ PTHREADINFO pti)
{
    if (!pti->pClientInfo)
        return;
    _SEH2_TRY
    {
        ((PULONG_PTR)pti->pClientInfo)[USER_CLIENTINFO_VIRTUAL_DPI_SLOT] =
            UserIsThreadDpiVirtualized(pti) ? USER_VIRTUAL_DPI : 0;
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
    }
    _SEH2_END;
}

/* A process decides once, with its first top level window, whether the DWM stretches it */
VOID
FASTCALL
UserDecideProcessDpiVirtualization(
    _In_ PPROCESSINFO ppi)
{
    PTHREADINFO pti;

    if (ppi->DpiVirtualizationDecided)
        return;
    ppi->DpiVirtualizationDecided = TRUE;
    ppi->DpiVirtualized = UserIsDpiVirtualizationActive() &&
                          (ppi->DpiAwareness == USER_DPI_UNAWARE || ppi->DpiAwareness == USER_DPI_UNAWARE_GDISCALED);
    for (pti = ppi->ptiList; pti; pti = pti->ptiSibling)
        UserUpdateClientDpi(pti);
}

/* Between the screen's pixels and the ones an application the DWM stretches works in */
INT
FASTCALL
UserDpiToVirtual(
    _In_ INT Value)
{
    return EngMulDiv(Value, USER_VIRTUAL_DPI, gpsi->dmLogPixels);
}

INT
FASTCALL
UserDpiFromVirtual(
    _In_ INT Value)
{
    return EngMulDiv(Value, gpsi->dmLogPixels, USER_VIRTUAL_DPI);
}

VOID
FASTCALL
UserDpiRectToVirtual(
    _Inout_ PRECT Rect)
{
    Rect->left = UserDpiToVirtual(Rect->left);
    Rect->top = UserDpiToVirtual(Rect->top);
    Rect->right = UserDpiToVirtual(Rect->right);
    Rect->bottom = UserDpiToVirtual(Rect->bottom);
}

VOID
FASTCALL
UserDpiRectFromVirtual(
    _Inout_ PRECT Rect)
{
    Rect->left = UserDpiFromVirtual(Rect->left);
    Rect->top = UserDpiFromVirtual(Rect->top);
    Rect->right = UserDpiFromVirtual(Rect->right);
    Rect->bottom = UserDpiFromVirtual(Rect->bottom);
}

/* Whether the metrics read now are the ones at 96 DPI */
BOOLEAN
FASTCALL
UserDpiMetricsVirtualized(VOID)
{
    PTHREADINFO pti = PsGetCurrentThreadWin32Thread();

    if (!pti)
        return FALSE;
    if (pti->DpiMetricsScope != USER_DPI_SCOPE_THREAD)
        return pti->DpiMetricsScope == USER_DPI_SCOPE_VIRTUAL;
    return UserIsThreadDpiVirtualized(pti);
}

/* Until UserDpiEndWindowScope, the metrics are those the window lives at, whoever reads them */
ULONG
FASTCALL
UserDpiBeginWindowScope(
    _In_ PWND Window)
{
    PTHREADINFO pti = PsGetCurrentThreadWin32Thread();
    ULONG Old;

    if (!pti)
        return USER_DPI_SCOPE_THREAD;
    Old = pti->DpiMetricsScope;
    pti->DpiMetricsScope = Window->DpiVirtualized ? USER_DPI_SCOPE_VIRTUAL : USER_DPI_SCOPE_SCREEN;
    return Old;
}

VOID
FASTCALL
UserDpiEndWindowScope(
    _In_ ULONG Old)
{
    PTHREADINFO pti = PsGetCurrentThreadWin32Thread();

    if (pti)
        pti->DpiMetricsScope = Old;
}

/* A point on the screen in the coordinates the window lives in, and back */
POINT
FASTCALL
UserDpiScreenPointToWindow(
    _In_ PWND Window,
    _In_ POINT Point)
{
    if (Window->DpiVirtualized)
    {
        Point.x = UserDpiToVirtual(Point.x);
        Point.y = UserDpiToVirtual(Point.y);
    }
    return Point;
}

POINT
FASTCALL
UserDpiWindowPointToScreen(
    _In_ PWND Window,
    _In_ POINT Point)
{
    if (Window->DpiVirtualized)
    {
        Point.x = UserDpiFromVirtual(Point.x);
        Point.y = UserDpiFromVirtual(Point.y);
    }
    return Point;
}

/* The calling thread's view of a point on the screen, and back */
POINT
FASTCALL
UserDpiScreenPointToCaller(
    _In_ POINT Point)
{
    if (UserIsThreadDpiVirtualized(PsGetCurrentThreadWin32Thread()))
    {
        Point.x = UserDpiToVirtual(Point.x);
        Point.y = UserDpiToVirtual(Point.y);
    }
    return Point;
}

POINT
FASTCALL
UserDpiCallerPointToScreen(
    _In_ POINT Point)
{
    if (UserIsThreadDpiVirtualized(PsGetCurrentThreadWin32Thread()))
    {
        Point.x = UserDpiFromVirtual(Point.x);
        Point.y = UserDpiFromVirtual(Point.y);
    }
    return Point;
}

/* A monitor's rectangles as the window sees them, or as the calling thread does without one */
VOID
FASTCALL
UserDpiGetMonitorRects(
    _In_ PMONITOR pMonitor,
    _In_opt_ PWND Window,
    _Out_opt_ PRECT rcMonitor,
    _Out_opt_ PRECT rcWork)
{
    BOOLEAN Virtual = Window ? Window->DpiVirtualized : UserIsThreadDpiVirtualized(PsGetCurrentThreadWin32Thread());

    if (rcMonitor)
    {
        *rcMonitor = pMonitor->rcMonitor;
        if (Virtual)
            UserDpiRectToVirtual(rcMonitor);
    }
    if (rcWork)
    {
        *rcWork = pMonitor->rcWork;
        if (Virtual)
            UserDpiRectToVirtual(rcWork);
    }
}

/* A thread's own awareness when it set one, else its process's */
ULONG
FASTCALL
UserGetThreadDpiAwareness(
    _In_ PTHREADINFO pti)
{
    if (pti->DpiAwarenessOverride != USER_DPI_INVALID)
        return pti->DpiAwarenessOverride;
    return pti->ppi ? pti->ppi->DpiAwareness : USER_DPI_UNAWARE;
}

/* A process picks its awareness once, from its manifest or its first call */
BOOL
FASTCALL
UserSetProcessDpiAwareness(
    _In_ ULONG Awareness)
{
    PPROCESSINFO ppi = PsGetCurrentProcessWin32Process();

    if (Awareness > USER_DPI_LAST || !ppi)
    {
        EngSetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (ppi->DpiAwarenessSet)
    {
        EngSetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }

    ppi->DpiAwareness = Awareness;
    ppi->DpiAwarenessSet = TRUE;
    return TRUE;
}

ULONG
FASTCALL
UserGetProcessDpiAwareness(
    _In_opt_ HANDLE hProcess)
{
    PEPROCESS Process;
    PPROCESSINFO ppi;
    ULONG Awareness;
    NTSTATUS Status;

    if (!hProcess || hProcess == NtCurrentProcess())
    {
        ppi = PsGetCurrentProcessWin32Process();
        return ppi ? ppi->DpiAwareness : USER_DPI_UNAWARE;
    }

    Status = ObReferenceObjectByHandle(hProcess, PROCESS_QUERY_LIMITED_INFORMATION, *PsProcessType, UserMode,
                                       (PVOID *)&Process, NULL);
    if (!NT_SUCCESS(Status))
    {
        SetLastNtError(Status);
        return USER_DPI_INVALID;
    }

    /* A process that never used user32 is as unaware as one that did not say */
    ppi = PsGetProcessWin32Process(Process);
    Awareness = ppi ? ppi->DpiAwareness : USER_DPI_UNAWARE;
    ObDereferenceObject(Process);
    return Awareness;
}

/* Children of a window that follows its monitor are told before and after it is */
static
VOID
co_UserNotifyChildrenDpiChange(
    _In_ PWND pwndParent,
    _In_ UINT Message)
{
    HWND *List = IntWinListChildren(pwndParent);
    HWND *Phwnd;
    PWND pwnd;

    if (!List)
        return;
    for (Phwnd = List; *Phwnd; Phwnd++)
    {
        pwnd = ValidateHwndNoErr(*Phwnd);
        if (!pwnd)
            continue;
        co_IntSendMessageTimeout(*Phwnd, Message, 0, 0, SMTO_ABORTIFHUNG, 2000, NULL);

        /* The send let the lock go, so the window may be gone */
        pwnd = ValidateHwndNoErr(*Phwnd);
        if (pwnd)
            co_UserNotifyChildrenDpiChange(pwnd, Message);
    }
    ExFreePoolWithTag(List, USERTAG_WINDOWLIST);
}

/*
 * After the displays changed resolution, the windows that follow their monitor's are told and
 * offered a rectangle as large as theirs at the new one. The others keep the one they started with.
 */
VOID
FASTCALL
co_UserNotifyDpiChange(
    _In_ UINT OldDpi,
    _In_ UINT NewDpi)
{
    PWND pwndDesktop = UserGetDesktopWindow();
    HWND *List;
    HWND *Phwnd;
    PWND pwnd;
    RECT Suggested;
    ULONG Awareness;

    if (!pwndDesktop || OldDpi == NewDpi || !OldDpi)
        return;

    List = IntWinListChildren(pwndDesktop);
    if (!List)
        return;

    for (Phwnd = List; *Phwnd; Phwnd++)
    {
        pwnd = ValidateHwndNoErr(*Phwnd);
        if (pwnd && pwnd->DpiVirtualized)
        {
            IntDwmScaleChanged(pwnd);
            continue;
        }
        if (!pwnd || (pwnd->DpiAwareness != USER_DPI_PER_MONITOR_AWARE &&
                      pwnd->DpiAwareness != USER_DPI_PER_MONITOR_AWARE_V2))
            continue;

        Suggested = pwnd->rcWindow;
        Suggested.right = Suggested.left + EngMulDiv(pwnd->rcWindow.right - pwnd->rcWindow.left, NewDpi, OldDpi);
        Suggested.bottom = Suggested.top + EngMulDiv(pwnd->rcWindow.bottom - pwnd->rcWindow.top, NewDpi, OldDpi);

        Awareness = pwnd->DpiAwareness;
        if (Awareness == USER_DPI_PER_MONITOR_AWARE_V2)
            co_UserNotifyChildrenDpiChange(pwnd, WM_DPICHANGED_BEFOREPARENT);
        if (!ValidateHwndNoErr(*Phwnd))
            continue;
        co_IntSendMessageTimeout(*Phwnd, WM_DPICHANGED, MAKEWPARAM(NewDpi, NewDpi), (LPARAM)&Suggested,
                                 SMTO_ABORTIFHUNG, 2000, NULL);
        pwnd = ValidateHwndNoErr(*Phwnd);
        if (Awareness == USER_DPI_PER_MONITOR_AWARE_V2 && pwnd)
            co_UserNotifyChildrenDpiChange(pwnd, WM_DPICHANGED_AFTERPARENT);
    }
    ExFreePoolWithTag(List, USERTAG_WINDOWLIST);
}

/* Returns the awareness the thread had, so it can be put back */
ULONG
FASTCALL
UserSetThreadDpiAwareness(
    _In_ ULONG Awareness)
{
    PTHREADINFO pti = PsGetCurrentThreadWin32Thread();
    ULONG Old;

    if (!pti || (Awareness > USER_DPI_LAST && Awareness != USER_DPI_INVALID))
    {
        EngSetLastError(ERROR_INVALID_PARAMETER);
        return USER_DPI_INVALID;
    }

    Old = UserGetThreadDpiAwareness(pti);
    pti->DpiAwarenessOverride = Awareness;
    UserUpdateClientDpi(pti);
    return Old;
}
