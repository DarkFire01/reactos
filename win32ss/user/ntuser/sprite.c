/*
 * PROJECT:     ReactOS Win32k subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     DWM sprites: redirected top level windows and the surfaces they draw into
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <win32k.h>
#include <reactos/dwmport.h>

DBG_DEFAULT_CHANNEL(UserMisc);

#ifndef WS_EX_NOREDIRECTIONBITMAP
#define WS_EX_NOREDIRECTIONBITMAP 0x00200000
#endif

typedef struct _DWM_SPRITE
{
    LIST_ENTRY Link;
    PWND Window;
    ULONG64 Handle;
    PLOGICAL_SURFACE Surface;
    RECT WindowRect;
    BOOLEAN Visible;
    BOOLEAN ActiveFrame;
    BOOLEAN RendersFrame;
} DWM_SPRITE, *PDWM_SPRITE;

/* Every sprite; only touched with the user lock held exclusively */
static LIST_ENTRY gDwmSprites = { &gDwmSprites, &gDwmSprites };
static ULONG64 gNextSprite = 0x100;

/* The window whose caption button the DWM last showed under the pointer */
static HWND ghwndDwmHover = NULL;

static
PDWM_SPRITE
IntDwmFindSprite(
    _In_ PWND Window)
{
    PLIST_ENTRY Entry;
    PDWM_SPRITE Sprite;

    for (Entry = gDwmSprites.Flink; Entry != &gDwmSprites; Entry = Entry->Flink)
    {
        Sprite = CONTAINING_RECORD(Entry, DWM_SPRITE, Link);
        if (Sprite->Window == Window)
            return Sprite;
    }
    return NULL;
}

/* The redirected top level window a window draws into, and the bitmap it draws to */
PWND
FASTCALL
IntDwmGetRedirectedWindow(
    _In_ PWND Window,
    _Out_opt_ HBITMAP *phbm)
{
    PWND Desktop = UserGetDesktopWindow();
    PWND TopLevel = Window;
    PDWM_SPRITE Sprite;

    if (phbm)
        *phbm = NULL;
    if (IsListEmpty(&gDwmSprites) || !Desktop || !Window)
        return NULL;

    while (TopLevel->spwndParent && TopLevel->spwndParent != Desktop)
        TopLevel = TopLevel->spwndParent;
    if (TopLevel->spwndParent != Desktop)
        return NULL;

    Sprite = IntDwmFindSprite(TopLevel);
    if (!Sprite)
        return NULL;

    if (phbm)
        *phbm = Sprite->Surface->hbm;
    return TopLevel;
}

BOOLEAN
FASTCALL
IntDwmIsRedirected(
    _In_ PWND Window)
{
    return !IsListEmpty(&gDwmSprites) && IntDwmFindSprite(Window) != NULL;
}

/* The size of the surface a redirected top level window draws into */
BOOLEAN
FASTCALL
IntDwmGetSurfaceSize(
    _In_ PWND Window,
    _Out_ PSIZEL Size)
{
    PDWM_SPRITE Sprite = IntDwmFindSprite(Window);

    if (!Sprite)
    {
        Size->cx = Size->cy = 0;
        return FALSE;
    }
    *Size = Sprite->Surface->Size;
    return TRUE;
}

/* The DWM draws the frame of a window that has a sizing border or a caption, as dwmredir decides */
static
BOOLEAN
IntDwmRendersFrame(
    _In_ PWND Window)
{
    return (Window->style & WS_THICKFRAME) || (Window->style & WS_CAPTION) == WS_CAPTION;
}

/* Tells the window, and so the theme, whether the DWM draws its frame */
static
VOID
IntDwmSendNcRendering(
    _In_ PDWM_SPRITE Sprite,
    _In_ BOOLEAN Enabled)
{
    if (Sprite->RendersFrame == Enabled || (Sprite->Window->state & WNDS_DESTROYED))
        return;
    Sprite->RendersFrame = Enabled;
    co_IntSendMessageNoWait(UserHMGetHandle(Sprite->Window), WM_DWMNCRENDERINGCHANGED, Enabled, 0);
}

static
VOID
IntDwmDescribeWindow(
    _In_ PWND Window,
    _Out_ PDWM_SPRITE_WINDOW Info)
{
    RtlZeroMemory(Info, sizeof(*Info));
    Info->WindowRect = Window->rcWindow;
    Info->ClientRect = Window->rcClient;
    Info->Style = Window->style;
    Info->ExStyle = Window->ExStyle;
    Info->ExStyle2 = Window->ExStyle2;
    Info->ActiveFrame = (Window->state & WNDS_ACTIVEFRAME) != 0;
    Info->DesktopId = (ULONG64)(ULONG_PTR)Window->head.rpdesk;
}

static
VOID
IntDwmSendUpdateSprite(
    _In_ PDWM_SPRITE Sprite,
    _In_ BOOLEAN NewSurface)
{
    DWM_UPDATE_SPRITE Message;

    RtlZeroMemory(&Message, sizeof(Message));
    Message.Command = DWM_PORT_UPDATE_SPRITE;
    Message.Sprite = Sprite->Handle;
    Message.Flags = (Sprite->Visible ? DWM_SPRITE_VISIBLE : 0) | (NewSurface ? DWM_SPRITE_NEW_SURFACE : 0);
    Message.HasInfo = TRUE;
    IntDwmDescribeWindow(Sprite->Window, &Message.Info);
    Message.Surface = Sprite->Surface->Handle;
    Message.Width = Sprite->Surface->Size.cx;
    Message.Height = Sprite->Surface->Size.cy;
    IntDwmSendMessage(&Message.Header, sizeof(Message));
}

static
VOID
IntDwmSendShowSprite(
    _In_ PDWM_SPRITE Sprite)
{
    DWM_SHOW_SPRITE Message;

    RtlZeroMemory(&Message, sizeof(Message));
    Message.Command = DWM_PORT_SHOW_SPRITE;
    Message.Sprite = Sprite->Handle;
    Message.Show = Sprite->Visible;
    IntDwmSendMessage(&Message.Header, sizeof(Message));
}

/* Places a sprite under the nearest sprite above it, or on top when there is none */
static
VOID
IntDwmSendZOrderSprite(
    _In_ PDWM_SPRITE Sprite)
{
    DWM_ZORDER_SPRITE Message;
    PDWM_SPRITE Above = NULL;
    PWND Window;

    for (Window = Sprite->Window->spwndPrev; Window && !Above; Window = Window->spwndPrev)
        Above = IntDwmFindSprite(Window);

    RtlZeroMemory(&Message, sizeof(Message));
    Message.Command = DWM_PORT_ZORDER_SPRITE;
    Message.Sprite = Sprite->Handle;
    Message.InsertAfter = Above ? Above->Handle : DWM_SPRITE_INSERT_TOP;
    IntDwmSendMessage(&Message.Header, sizeof(Message));
}

/*
 * The part of the window the DWM shows of what GDI drew: the window less its classic frame,
 * and at the top less the caption too, but never less than the client area.
 */
static
VOID
IntDwmCalculateContentRect(
    _In_ PWND Window,
    _Out_ PRECT Content,
    _Out_ PLONG BorderThickness)
{
    SIZE Border;
    LONG Caption = 0;

    UserGetWindowBorders(Window->style, Window->ExStyle, &Border, FALSE);
    if ((Window->style & WS_CAPTION) == WS_CAPTION)
    {
        Caption = UserGetSystemMetrics((Window->ExStyle & WS_EX_TOOLWINDOW) ? SM_CYSMCAPTION : SM_CYCAPTION);
    }

    *Content = Window->rcWindow;
    RECTL_vInflateRect((PRECTL)Content, -Border.cx, -Border.cy);
    Content->top += Caption;
    RECTL_bUnionRect((PRECTL)Content, (PRECTL)Content, (PRECTL)&Window->rcClient);
    RECTL_bIntersectRect((PRECTL)Content, (PRECTL)Content, (PRECTL)&Window->rcWindow);
    *BorderThickness = Border.cx;
}

static
VOID
IntDwmSendChildMoveSize(
    _In_ PWND Window)
{
    DWM_CHILD_MOVE_SIZE Message;

    RtlZeroMemory(&Message, sizeof(Message));
    Message.Command = DWM_PORT_CHILD_MOVE_SIZE;
    Message.Window = (ULONG64)(ULONG_PTR)UserHMGetHandle(Window);
    Message.WindowRect = Window->rcWindow;
    Message.ClientRect = Window->rcClient;
    IntDwmCalculateContentRect(Window, &Message.ContentRect, &Message.BorderThickness);
    IntDwmSendMessage(&Message.Header, sizeof(Message));
}

/* Redraws a window into the surface it now has */
static
VOID
IntDwmRepaintWindow(
    _In_ PWND Window)
{
    DceResetActiveDCEs(Window);
    co_UserRedrawWindow(Window, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
}

static
BOOLEAN
IntDwmShouldCompose(
    _In_ PWND Window)
{
    return IntDwmIsKernelStarted() &&
           Window->spwndParent == UserGetDesktopWindow() &&
           (Window->style & WS_VISIBLE) &&
           !(Window->ExStyle & WS_EX_NOREDIRECTIONBITMAP) &&
           !(Window->state & WNDS_DESTROYED);
}

/* Gives a top level window a sprite and a surface to draw into, the first time it is shown */
static
VOID
IntDwmComposeWindow(
    _In_ PWND Window)
{
    DWM_CREATE_SPRITE Message;
    PDWM_SPRITE Sprite;

    if (!IntDwmShouldCompose(Window) || IntDwmFindSprite(Window))
        return;

    Sprite = ExAllocatePoolZero(PagedPool, sizeof(*Sprite), TAG_SPRITE);
    if (!Sprite)
        return;

    Sprite->Window = Window;
    Sprite->Handle = gNextSprite++;
    Sprite->WindowRect = Window->rcWindow;
    Sprite->Visible = TRUE;
    Sprite->ActiveFrame = (Window->state & WNDS_ACTIVEFRAME) != 0;
    Sprite->Surface = GreCreateLogicalSurface(Window->rcWindow.right - Window->rcWindow.left,
                                              Window->rcWindow.bottom - Window->rcWindow.top,
                                              Sprite->Handle);
    if (!Sprite->Surface)
    {
        ERR("No redirection surface for %p\n", UserHMGetHandle(Window));
        ExFreePoolWithTag(Sprite, TAG_SPRITE);
        return;
    }
    InsertTailList(&gDwmSprites, &Sprite->Link);

    RtlZeroMemory(&Message, sizeof(Message));
    Message.Command = DWM_PORT_CREATE_SPRITE;
    Message.Sprite = Sprite->Handle;
    Message.Window = (ULONG64)(ULONG_PTR)UserHMGetHandle(Window);
    Message.Rect = Window->rcWindow;
    IntDwmDescribeWindow(Window, &Message.Info);
    Message.ExpWinVer = Window->head.pti->dwExpWinVer;
    IntDwmSendMessage(&Message.Header, sizeof(Message));

    IntDwmSendZOrderSprite(Sprite);
    IntDwmSendShowSprite(Sprite);
    IntDwmSendUpdateSprite(Sprite, TRUE);
    IntDwmSendChildMoveSize(Window);
    IntDwmNotifyIconChange(Window);
    if (IntDwmRendersFrame(Window))
        IntDwmSendNcRendering(Sprite, TRUE);
    IntDwmRepaintWindow(Window);
}

static
VOID
IntDwmDestroySprite(
    _In_ PDWM_SPRITE Sprite,
    _In_ BOOLEAN Notify)
{
    DWM_SPRITE_HANDLE Message;

    RemoveEntryList(&Sprite->Link);
    if (ghwndDwmHover == UserHMGetHandle(Sprite->Window))
        ghwndDwmHover = NULL;
    IntDwmSendNcRendering(Sprite, FALSE);

    if (Notify)
    {
        RtlZeroMemory(&Message, sizeof(Message));
        Message.Command = DWM_PORT_DESTROY_SPRITE;
        Message.Sprite = Sprite->Handle;
        IntDwmSendMessage(&Message.Header, sizeof(Message));
    }

    /* Its DCs go back to the screen before the surface goes away */
    if (!(Sprite->Window->state & WNDS_DESTROYED))
        DceResetActiveDCEs(Sprite->Window);
    GreDeleteLogicalSurface(Sprite->Surface);
    ExFreePoolWithTag(Sprite, TAG_SPRITE);
}

/* A move only moves the sprite; a new size needs a new surface and a repaint */
static
VOID
IntDwmMoveSprite(
    _In_ PDWM_SPRITE Sprite)
{
    PWND Window = Sprite->Window;
    PLOGICAL_SURFACE Old;
    LONG Width = Window->rcWindow.right - Window->rcWindow.left;
    LONG Height = Window->rcWindow.bottom - Window->rcWindow.top;
    BOOLEAN Resized;

    if (RtlEqualMemory(&Sprite->WindowRect, &Window->rcWindow, sizeof(RECT)))
        return;

    Resized = Width != Sprite->WindowRect.right - Sprite->WindowRect.left ||
              Height != Sprite->WindowRect.bottom - Sprite->WindowRect.top;
    Sprite->WindowRect = Window->rcWindow;

    IntDwmSendChildMoveSize(Window);
    if (!Resized)
    {
        IntDwmSendUpdateSprite(Sprite, FALSE);
        return;
    }

    Old = Sprite->Surface;
    Sprite->Surface = GreCreateLogicalSurface(Width, Height, Sprite->Handle);
    if (!Sprite->Surface)
    {
        Sprite->Surface = Old;
        IntDwmSendUpdateSprite(Sprite, FALSE);
        return;
    }

    IntDwmSendUpdateSprite(Sprite, TRUE);
    DceResetActiveDCEs(Window);
    GreDeleteLogicalSurface(Old);
    co_UserRedrawWindow(Window, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
}

/* SetWindowPos gave the window a new rectangle; the surface follows before anything paints at the new size */
VOID
FASTCALL
IntDwmWindowRectChanged(
    _In_ PWND Window)
{
    PDWM_SPRITE Sprite = IntDwmFindSprite(Window);

    if (Sprite && Sprite->Visible)
        IntDwmMoveSprite(Sprite);
}

VOID
FASTCALL
IntDwmSpriteWinEvent(
    _In_ DWORD Event,
    _In_ PWND Window)
{
    PDWM_SPRITE Sprite = IntDwmFindSprite(Window);

    switch (Event)
    {
        case EVENT_OBJECT_SHOW:
            if (!Sprite)
            {
                IntDwmComposeWindow(Window);
            }
            else if (!Sprite->Visible)
            {
                Sprite->Visible = TRUE;
                IntDwmSendShowSprite(Sprite);
                IntDwmMoveSprite(Sprite);
            }
            break;

        case EVENT_OBJECT_HIDE:
            if (Sprite && Sprite->Visible)
            {
                Sprite->Visible = FALSE;
                IntDwmSendShowSprite(Sprite);
            }
            break;

        case EVENT_OBJECT_LOCATIONCHANGE:
            if (Sprite)
                IntDwmMoveSprite(Sprite);
            break;

        case EVENT_OBJECT_REORDER:
            if (Sprite)
                IntDwmSendZOrderSprite(Sprite);
            break;

        case EVENT_OBJECT_DESTROY:
            if (Sprite)
                IntDwmDestroySprite(Sprite, TRUE);
            break;
    }
}

/* A window that goes away without its destroy event still loses its sprite */
VOID
FASTCALL
IntDwmWindowFreed(
    _In_ PWND Window)
{
    PDWM_SPRITE Sprite = IntDwmFindSprite(Window);

    if (Sprite)
        IntDwmDestroySprite(Sprite, TRUE);
}

/* Composes every visible top level window, bottom first so each lands above the last */
VOID
FASTCALL
IntDwmComposeAll(VOID)
{
    PWND Desktop = UserGetDesktopWindow();
    PWND Window;

    if (!Desktop || !Desktop->spwndChild)
        return;

    for (Window = Desktop->spwndChild; Window->spwndNext; Window = Window->spwndNext)
        ;
    for (; Window; Window = Window->spwndPrev)
        IntDwmComposeWindow(Window);
}

/* Takes every window off its sprite; the DWM is gone, so nothing is sent */
VOID
FASTCALL
IntDwmDecomposeAll(VOID)
{
    while (!IsListEmpty(&gDwmSprites))
        IntDwmDestroySprite(CONTAINING_RECORD(gDwmSprites.Flink, DWM_SPRITE, Link), FALSE);
}

VOID
FASTCALL
IntDwmNotifyActivation(
    _In_ PWND Window)
{
    DWM_ACTIVATION_CHANGE Message;
    PDWM_SPRITE Sprite = IntDwmFindSprite(Window);
    BOOLEAN Active = (Window->state & WNDS_ACTIVEFRAME) != 0;

    if (!Sprite || Sprite->ActiveFrame == Active)
        return;
    Sprite->ActiveFrame = Active;

    RtlZeroMemory(&Message, sizeof(Message));
    Message.Command = DWM_PORT_ACTIVATION_CHANGE;
    Message.Window = (ULONG64)(ULONG_PTR)UserHMGetHandle(Window);
    Message.Active = Active;
    IntDwmSendMessage(&Message.Header, sizeof(Message));
}

/* The DWM reads the text itself */
VOID
FASTCALL
IntDwmNotifyTextChange(
    _In_ PWND Window)
{
    DWM_WINDOW_HANDLE Message;

    if (!IntDwmFindSprite(Window))
        return;

    RtlZeroMemory(&Message, sizeof(Message));
    Message.Command = DWM_PORT_TEXT_CHANGE;
    Message.Window = (ULONG64)(ULONG_PTR)UserHMGetHandle(Window);
    IntDwmSendMessage(&Message.Header, sizeof(Message));
}

/* The icons WM_SETICON gave the window; without them the DWM falls back to the class icons */
VOID
FASTCALL
IntDwmNotifyIconChange(
    _In_ PWND Window)
{
    DWM_ICON_CHANGE Message;

    if (!IntDwmFindSprite(Window))
        return;

    RtlZeroMemory(&Message, sizeof(Message));
    Message.Command = DWM_PORT_ICON_CHANGE;
    Message.Window = (ULONG64)(ULONG_PTR)UserHMGetHandle(Window);
    Message.Icon = (ULONG64)(ULONG_PTR)UserGetProp(Window, gpsi->atomIconProp, TRUE);
    Message.SmallIcon = (ULONG64)(ULONG_PTR)UserGetProp(Window, gpsi->atomIconSmProp, TRUE);
    IntDwmSendMessage(&Message.Header, sizeof(Message));
}

static
BOOLEAN
co_IntDwmHitTestQuery(
    _In_ PWND Window,
    _In_ POINT Point,
    _In_ UINT Message,
    _In_ LONG Captured,
    _Out_ PUSHORT HitTest)
{
    DWM_HIT_TEST_QUERY Query;
    USER_REFERENCE_ENTRY Ref;
    NTSTATUS Status;

    *HitTest = HTNOWHERE;

    RtlZeroMemory(&Query, sizeof(Query));
    Query.Command = DWM_PORT_HIT_TEST_QUERY;
    Query.Window = (ULONG64)(ULONG_PTR)UserHMGetHandle(Window);
    Query.Message = Message;
    Query.Point = Point;
    Query.WParam = Captured != DWM_HIT_TEST_NONE ? MK_LBUTTON : 0;
    Query.LParam = MAKELONG(Point.x, Point.y);
    Query.Captured = Captured;
    Query.HitTest = DWM_HIT_TEST_NONE;

    UserRefObjectCo(Window, &Ref);
    Status = IntDwmSendSyncMessage(&Query.Header, sizeof(Query));
    UserDerefObjectCo(Window);

    if (!NT_SUCCESS(Status) || !Query.Handled)
        return FALSE;
    *HitTest = (USHORT)Query.HitTest;
    return TRUE;
}

/*
 * Asks the DWM about a point on the frame of a composed window before the window is sent
 * WM_NCHITTEST. The DWM claims its caption buttons and shows the one under the pointer as hot.
 */
BOOLEAN
FASTCALL
co_IntDwmHitTestFrame(
    _In_ PWND Window,
    _In_ POINT Point,
    _Out_ PUSHORT HitTest)
{
    *HitTest = HTNOWHERE;
    if (!IntDwmFindSprite(Window) || RECTL_bPointInRect(&Window->rcClient, Point.x, Point.y))
        return FALSE;

    if (!co_IntDwmHitTestQuery(Window, Point, WM_MOUSEMOVE, DWM_HIT_TEST_NONE, HitTest))
    {
        if (ghwndDwmHover == UserHMGetHandle(Window))
            ghwndDwmHover = NULL;
        return FALSE;
    }

    ghwndDwmHover = UserHMGetHandle(Window);
    return TRUE;
}

/* Once the pointer is somewhere else, the DWM is told so it drops the hot button */
VOID
FASTCALL
co_IntDwmLeaveFrame(
    _In_opt_ PWND Found,
    _In_ POINT Point,
    _In_ USHORT HitTest)
{
    PWND Desktop = UserGetDesktopWindow();
    PWND Hover;
    PWND TopLevel = Found;
    USHORT Ignored;

    if (!ghwndDwmHover)
        return;

    Hover = ValidateHwndNoErr(ghwndDwmHover);
    if (!Hover)
    {
        ghwndDwmHover = NULL;
        return;
    }

    while (TopLevel && TopLevel->spwndParent && TopLevel->spwndParent != Desktop)
        TopLevel = TopLevel->spwndParent;

    /* Another thread's hit test of the same window says nothing about its frame */
    if (TopLevel == Hover &&
        (Found->head.pti != PsGetCurrentThreadWin32Thread() ||
         HitTest == HTMINBUTTON || HitTest == HTMAXBUTTON || HitTest == HTCLOSE || HitTest == HTHELP))
    {
        return;
    }

    ghwndDwmHover = NULL;
    co_IntDwmHitTestQuery(Hover, Point, WM_MOUSEMOVE, DWM_HIT_TEST_NONE, &Ignored);
}

/* One step of tracking a caption button the DWM draws: TRUE while the pointer is on it */
BOOLEAN
FASTCALL
co_IntDwmTrackCaptionButton(
    _In_ PWND Window,
    _In_ POINT Point,
    _In_ LONG Captured)
{
    USHORT HitTest;

    if (!co_IntDwmHitTestQuery(Window, Point, WM_MOUSEMOVE, Captured, &HitTest))
        return FALSE;
    return Captured != DWM_HIT_TEST_NONE && HitTest == Captured;
}

VOID
NTAPI
IntDwmNotifyDirtySprite(
    _In_ ULONG64 Sprite)
{
    DWM_DIRTY_SPRITE Message;

    RtlZeroMemory(&Message, sizeof(Message));
    Message.Command = DWM_PORT_DIRTY_SPRITE;
    Message.DirtyFlags = 1;
    Message.Sprite = Sprite;
    IntDwmSendMessage(&Message.Header, sizeof(Message));
}
