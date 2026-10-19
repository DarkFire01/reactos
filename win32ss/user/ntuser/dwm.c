/*
 * PROJECT:     ReactOS Win32k subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Desktop Window Manager session port and window notifications
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <win32k.h>
#include <reactos/dwmport.h>

DBG_DEFAULT_CHANNEL(UserMisc);

NTSTATUS NTAPI DlRegisterDwmProcess(VOID);
VOID NTAPI DlDwmShutdown(VOID);

/* The port dwmredir created and registered, and the process that registered it */
static PVOID gpDwmPort = NULL;
static PPROCESSINFO gppiDwm = NULL;
static BOOLEAN gbDwmKernelStarted = FALSE;

/* Drawing threads send dirty notifications without the user lock, so the port is swapped under this */
static KSPIN_LOCK gDwmPortLock;

/* Replaces the port and returns the one it replaced, for the caller to dereference */
static
PVOID
IntDwmSwapPort(
    _In_opt_ PVOID Port)
{
    PVOID Old;
    KIRQL Irql;

    KeAcquireSpinLock(&gDwmPortLock, &Irql);
    Old = gpDwmPort;
    gpDwmPort = Port;
    KeReleaseSpinLock(&gDwmPortLock, Irql);
    return Old;
}

/* Sends a kernel datagram to the DWM; Header is followed by the message's payload */
NTSTATUS
FASTCALL
IntDwmSendMessage(
    _Inout_ PPORT_MESSAGE Header,
    _In_ ULONG TotalLength)
{
    PVOID Port;
    KIRQL Irql;
    NTSTATUS Status;

    KeAcquireSpinLock(&gDwmPortLock, &Irql);
    Port = gpDwmPort;
    if (Port)
        ObReferenceObject(Port);
    KeReleaseSpinLock(&gDwmPortLock, Irql);

    if (!Port)
        return STATUS_PORT_DISCONNECTED;

    Header->u1.s1.DataLength = (CSHORT)(TotalLength - sizeof(*Header));
    Header->u1.s1.TotalLength = (CSHORT)TotalLength;
    Header->u2.s2.Type = LPC_DATAGRAM | LPC_KERNELMODE_MESSAGE;
    Status = LpcRequestPort(Port, Header);
    ObDereferenceObject(Port);
    return Status;
}

BOOLEAN
FASTCALL
IntDwmIsKernelStarted(VOID)
{
    return gbDwmKernelStarted;
}

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
    Message.Command = DWM_PORT_WINDOW_NOTIFICATION;
    Message.Event = Event;
    Message.Window = (ULONG64)(ULONG_PTR)UserHMGetHandle(Window);
    Message.InsertAfter = Window->spwndPrev ? (ULONG64)(ULONG_PTR)UserHMGetHandle(Window->spwndPrev) : 0;
    Message.WindowRect = Window->rcWindow;
    Message.ClientRect = Window->rcClient;
    Message.Style = Window->style;
    Message.ExStyle = Window->ExStyle;
    Message.ProcessId = HandleToUlong(PsGetProcessId(Window->head.pti->ppi->peProcess));

    Status = IntDwmSendMessage(&Message.Header, sizeof(Message));
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
    IntDwmSpriteWinEvent(Event, Window);
}

BOOLEAN
FASTCALL
IntDwmIsCurrentProcess(VOID)
{
    return gppiDwm && gppiDwm == GetW32ProcessInfo();
}

/* The DWM is active from the moment it registers its session port */
BOOLEAN
FASTCALL
IntDwmIsActive(VOID)
{
    return gpDwmPort != NULL;
}

/* Stops the DWM and gives its outputs back to GDI */
static
VOID
IntDwmStop(
    _In_ BOOLEAN Redraw)
{
    PWND Desktop;
    PVOID Port;

    /* Windows draw to the screen again before the DWM gives it up */
    IntDwmDecomposeAll();

    /* The DWM stops counting as active first, so dxgkrnl hands its outputs to GDI */
    gbDwmKernelStarted = FALSE;
    Port = IntDwmSwapPort(NULL);
    if (Port)
        ObDereferenceObject(Port);
    gppiDwm = NULL;

    DlDwmShutdown();

    /* Nothing GDI drew while the DWM owned the screen reached it */
    Desktop = Redraw ? UserGetDesktopWindow() : NULL;
    if (Desktop)
        co_UserRedrawWindow(Desktop, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
}

/* The DWM's last thread is leaving while its process can still draw */
VOID
FASTCALL
IntDwmProcessShutdown(
    _In_ PPROCESSINFO ProcessInfo)
{
    if (ProcessInfo == gppiDwm)
        IntDwmStop(TRUE);
}

VOID
FASTCALL
IntDwmProcessCleanup(
    _In_ PPROCESSINFO ProcessInfo)
{
    if (ProcessInfo == gppiDwm)
        IntDwmStop(FALSE);
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

    IntDwmSwapPort(PortObject);
    gppiDwm = GetW32ProcessInfo();

    /* dxgkrnl only lets the registered DWM take shared ownership of the display */
    Status = DlRegisterDwmProcess();
    if (!NT_SUCCESS(Status))
    {
        ERR("dxgkrnl refused the DWM process: 0x%08lx\n", Status);
        ObDereferenceObject(IntDwmSwapPort(NULL));
        gppiDwm = NULL;
        SetLastNtError(Status);
        goto Exit;
    }
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

    /* From here on windows draw into their sprites instead of the screen */
    IntDwmComposeAll();
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
