/*
 * PROJECT:     ReactOS Win32k subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Desktop Window Manager session port and window notifications
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <win32k.h>
#include <reactos/dwmport.h>

DBG_DEFAULT_CHANNEL(UserMisc);

/* The port dwmredir created and registered, and the process that registered it */
static PVOID gpDwmPort = NULL;
static PPROCESSINFO gppiDwm = NULL;
static BOOLEAN gbDwmKernelStarted = FALSE;

static
BOOLEAN
IntDwmIsTrackedWindow(
    _In_ PWND Window)
{
    PWND Desktop = UserGetDesktopWindow();

    return Desktop &&
           Window->spwndParent == Desktop &&
           !(Window->state & WNDS_DESTROYED);
}

static
VOID
IntDwmSendWindowNotification(
    _In_ PWND Window,
    _In_ ULONG Event)
{
    DWM_WINDOW_NOTIFICATION Message;
    NTSTATUS Status;

    RtlZeroMemory(&Message, sizeof(Message));
    Message.Header.u1.s1.DataLength = sizeof(Message) - sizeof(Message.Header);
    Message.Header.u1.s1.TotalLength = sizeof(Message);
    Message.Header.u2.s2.Type = LPC_DATAGRAM | LPC_KERNELMODE_MESSAGE;
    Message.Command = DWM_PORT_WINDOW_NOTIFICATION;
    Message.Event = Event;
    Message.Window = (ULONG64)(ULONG_PTR)UserHMGetHandle(Window);
    Message.InsertAfter = Window->spwndPrev ? (ULONG64)(ULONG_PTR)UserHMGetHandle(Window->spwndPrev) : 0;
    Message.WindowRect = Window->rcWindow;
    Message.ClientRect = Window->rcClient;
    Message.Style = Window->style;
    Message.ExStyle = Window->ExStyle;
    Message.ProcessId = HandleToUlong(PsGetProcessId(Window->head.pti->ppi->peProcess));

    Status = LpcRequestPort(gpDwmPort, &Message.Header);
    if (!NT_SUCCESS(Status))
        TRACE("DWM notification %lu for %p failed 0x%08lx\n", Event, UserHMGetHandle(Window), Status);
}

/* Called for every window event; forwards top level window changes once the DWM has started */
VOID
FASTCALL
IntDwmNotifyWinEvent(
    _In_ DWORD Event,
    _In_opt_ PWND Window,
    _In_ LONG ObjectId)
{
    if (!gbDwmKernelStarted || !Window || ObjectId != OBJID_WINDOW)
        return;

    switch (Event)
    {
        case EVENT_OBJECT_CREATE:
        case EVENT_OBJECT_DESTROY:
        case EVENT_OBJECT_SHOW:
        case EVENT_OBJECT_HIDE:
        case EVENT_OBJECT_LOCATIONCHANGE:
        case EVENT_OBJECT_NAMECHANGE:
        case EVENT_OBJECT_STATECHANGE:
        case EVENT_OBJECT_REORDER:
            break;

        default:
            return;
    }

    if (Window->spwndParent != UserGetDesktopWindow())
        return;

    IntDwmSendWindowNotification(Window, Event);
}

BOOLEAN
FASTCALL
IntDwmIsCurrentProcess(VOID)
{
    return gppiDwm && gppiDwm == GetW32ProcessInfo();
}

VOID
FASTCALL
IntDwmProcessCleanup(
    _In_ PPROCESSINFO ProcessInfo)
{
    if (ProcessInfo != gppiDwm)
        return;

    gbDwmKernelStarted = FALSE;
    if (gpDwmPort)
        ObDereferenceObject(gpDwmPort);
    gpDwmPort = NULL;
    gppiDwm = NULL;
}

BOOL
APIENTRY
NtUserRegisterSessionPort(
    _In_ HANDLE Port)
{
    PVOID PortObject;
    NTSTATUS Status;
    BOOL Result = FALSE;

    UserEnterExclusive();

    if (gpDwmPort)
    {
        EngSetLastError(RtlNtStatusToDosError(STATUS_PORT_ALREADY_SET));
        goto Exit;
    }

    Status = ObReferenceObjectByHandle(Port, 0, LpcPortObjectType, UserMode, &PortObject, NULL);
    if (!NT_SUCCESS(Status))
    {
        SetLastNtError(Status);
        goto Exit;
    }

    gpDwmPort = PortObject;
    gppiDwm = GetW32ProcessInfo();
    Result = TRUE;

Exit:
    UserLeave();
    return Result;
}

BOOL
APIENTRY
NtUserUnregisterSessionPort(VOID)
{
    BOOL Result = FALSE;

    UserEnterExclusive();
    if (gppiDwm && gppiDwm == GetW32ProcessInfo())
    {
        IntDwmProcessCleanup(gppiDwm);
        Result = TRUE;
    }
    else
    {
        EngSetLastError(ERROR_ACCESS_DENIED);
    }
    UserLeave();
    return Result;
}

/* Starts window notifications and reports every top level window that already exists */
BOOL
APIENTRY
NtUserDwmKernelStartup(VOID)
{
    PWND Desktop;
    PWND Child;
    BOOL Result = FALSE;

    UserEnterExclusive();

    if (!gpDwmPort || gppiDwm != GetW32ProcessInfo())
    {
        EngSetLastError(ERROR_ACCESS_DENIED);
        goto Exit;
    }

    gbDwmKernelStarted = TRUE;

    Desktop = UserGetDesktopWindow();
    for (Child = Desktop ? Desktop->spwndChild : NULL; Child; Child = Child->spwndNext)
    {
        if (!IntDwmIsTrackedWindow(Child))
            continue;

        IntDwmSendWindowNotification(Child, EVENT_OBJECT_CREATE);
        if (Child->style & WS_VISIBLE)
            IntDwmSendWindowNotification(Child, EVENT_OBJECT_SHOW);
    }
    Result = TRUE;

Exit:
    UserLeave();
    return Result;
}

BOOL
APIENTRY
NtUserDwmKernelShutdown(VOID)
{
    BOOL Result = FALSE;

    UserEnterExclusive();
    if (gppiDwm && gppiDwm == GetW32ProcessInfo())
    {
        gbDwmKernelStarted = FALSE;
        Result = TRUE;
    }
    else
    {
        EngSetLastError(ERROR_ACCESS_DENIED);
    }
    UserLeave();
    return Result;
}
