/*
 * PROJECT:     ReactOS Win32k subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Flip swapchains bound to a window, which the DWM composes as the window's content
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <win32k.h>
#include <reactos/dwmport.h>
#include "../../gdi/ntgdi/dxgkfwd.h"

DBG_DEFAULT_CHANNEL(UserMisc);

#define TAG_DWM_SWAPCHAIN   'cSwD'

BOOL NTAPI UserIsCurrentThreadDesktopComposed(VOID);

/* The composition surface a window's swapchain presents to */
typedef struct _DWM_WINDOW_SWAPCHAIN
{
    LIST_ENTRY Link;
    HWND Window;
    PVOID Surface;
    BOOLEAN RemovePending;      /* the swapchain let go of the window, so another may bind it */
} DWM_WINDOW_SWAPCHAIN, *PDWM_WINDOW_SWAPCHAIN;

/* Guarded by the user lock */
static LIST_ENTRY gDwmSwapChains = { &gDwmSwapChains, &gDwmSwapChains };

static
PDWM_WINDOW_SWAPCHAIN
IntDwmFindSwapChain(
    _In_ HWND Window)
{
    PLIST_ENTRY Entry;
    PDWM_WINDOW_SWAPCHAIN SwapChain;

    for (Entry = gDwmSwapChains.Flink; Entry != &gDwmSwapChains; Entry = Entry->Flink)
    {
        SwapChain = CONTAINING_RECORD(Entry, DWM_WINDOW_SWAPCHAIN, Link);
        if (SwapChain->Window == Window)
            return SwapChain;
    }
    return NULL;
}

static
VOID
IntDwmFreeSwapChain(
    _In_ PDWM_WINDOW_SWAPCHAIN SwapChain)
{
    RemoveEntryList(&SwapChain->Link);
    ObDereferenceObject(SwapChain->Surface);
    ExFreePoolWithTag(SwapChain, TAG_DWM_SWAPCHAIN);
}

/* The desktop and message windows show nothing, and a dying window takes no new content */
static
BOOLEAN
IntDwmCanHoldSwapChain(
    _In_ PWND Window)
{
    if (Window->fnid == FNID_DESKTOP || Window->fnid == FNID_MESSAGEWND)
        return FALSE;
    return !(Window->state & WNDS_DESTROYED) && !(Window->state2 & WNDS2_INDESTROY);
}

/* Tells the DWM which surface now shows in the window; without one the window goes back to its own content */
static
BOOL
IntDwmSendSwapChain(
    _In_ HWND Window,
    _In_opt_ PVOID Surface)
{
    DWM_BIND_SWAPCHAIN Message;
    HANDLE DwmHandle = NULL;
    NTSTATUS Status;

    if (Surface)
    {
        Status = DlOpenDwmHandleForSurface(Surface, &DwmHandle);
        if (!NT_SUCCESS(Status))
        {
            ERR("Opening the swapchain surface of %p for the DWM failed 0x%08lx\n", Window, Status);
            return FALSE;
        }
    }

    RtlZeroMemory(&Message, sizeof(Message));
    Message.Command = DWM_PORT_BIND_SWAPCHAIN;
    Message.Window = (ULONG64)(ULONG_PTR)Window;
    Message.Surface = (ULONG64)(ULONG_PTR)DwmHandle;
    Status = IntDwmSendMessage(&Message.Header, sizeof(Message));
    if (!NT_SUCCESS(Status))
    {
        TRACE("Swapchain notification for %p failed 0x%08lx\n", Window, Status);
        return FALSE;
    }
    return TRUE;
}

/**
 * @brief Records the composition surface a flip swapchain of the window presents to, from dxgkrnl
 *        when the surface is bound with the window.
 *
 * @param[in] hWnd
 * The window the swapchain presents to.
 *
 * @param[in] Replace
 * Whether the surface may take the window from a swapchain still bound to it.
 *
 * @param[in] Surface
 * The calling process's handle to the composition surface.
 *
 * @return
 * TRUE if the window shows the surface, FALSE if dxgkrnl must fail the bind.
 */
BOOL
NTAPI
UserSetWindowedSwapChainApiExt(
    _In_ HWND hWnd,
    _In_ BOOL Replace,
    _In_ HANDLE Surface)
{
    PDWM_WINDOW_SWAPCHAIN SwapChain;
    PVOID Object;
    PWND Window;
    BOOL Result = FALSE;

    UserEnterExclusive();

    Window = UserGetWindowObject(hWnd);
    if (!Window || !IntDwmCanHoldSwapChain(Window))
        goto Quit;

    if (!NT_SUCCESS(DlReferenceCompositionSurface(Surface, &Object)))
        goto Quit;

    /* Binding the same surface again, as a resize does, keeps the window */
    SwapChain = IntDwmFindSwapChain(hWnd);
    if (SwapChain && SwapChain->Surface == Object)
    {
        SwapChain->RemovePending = FALSE;
        ObDereferenceObject(Object);
        Result = TRUE;
        goto Quit;
    }

    if (SwapChain && !Replace && !SwapChain->RemovePending)
    {
        ObDereferenceObject(Object);
        goto Quit;
    }

    if (SwapChain)
    {
        ObDereferenceObject(SwapChain->Surface);
    }
    else
    {
        SwapChain = ExAllocatePoolWithTag(PagedPool, sizeof(*SwapChain), TAG_DWM_SWAPCHAIN);
        if (!SwapChain)
        {
            ObDereferenceObject(Object);
            goto Quit;
        }
        SwapChain->Window = hWnd;
        InsertTailList(&gDwmSwapChains, &SwapChain->Link);
    }
    SwapChain->Surface = Object;
    SwapChain->RemovePending = FALSE;

    Result = TRUE;
    if (UserIsCurrentThreadDesktopComposed())
    {
        Result = IntDwmSendSwapChain(hWnd, Object);
        if (!Result)
            IntDwmFreeSwapChain(SwapChain);
    }

Quit:
    UserLeave();
    return Result;
}

/**
 * @brief Takes a flip swapchain's surface off the window, from dxgkrnl when the surface is unbound.
 *
 * @param[in] hWnd
 * The window the swapchain presented to.
 *
 * @param[in] Pending
 * TRUE if the swapchain only lets another take the window, and the window keeps its last frame.
 *
 * @return
 * TRUE if the window exists.
 */
BOOL
NTAPI
UserRemoveWindowedSwapChain(
    _In_ HWND hWnd,
    _In_ BOOL Pending)
{
    PDWM_WINDOW_SWAPCHAIN SwapChain;
    PWND Window;
    BOOL Result = FALSE;

    UserEnterExclusive();

    Window = UserGetWindowObject(hWnd);
    if (Window && Window->fnid != FNID_DESKTOP)
    {
        SwapChain = IntDwmFindSwapChain(hWnd);
        if (SwapChain && Pending)
        {
            SwapChain->RemovePending = TRUE;
        }
        else if (SwapChain)
        {
            if (IntDwmCanHoldSwapChain(Window) && UserIsCurrentThreadDesktopComposed())
                IntDwmSendSwapChain(hWnd, NULL);
            IntDwmFreeSwapChain(SwapChain);
        }
        Result = TRUE;
    }

    UserLeave();
    return Result;
}

/* The DWM finds out about the window itself, so only the reference goes */
VOID
FASTCALL
IntDwmSwapChainWindowFreed(
    _In_ PWND Window)
{
    PDWM_WINDOW_SWAPCHAIN SwapChain = IntDwmFindSwapChain(UserHMGetHandle(Window));

    if (SwapChain)
        IntDwmFreeSwapChain(SwapChain);
}

/* EOF */
