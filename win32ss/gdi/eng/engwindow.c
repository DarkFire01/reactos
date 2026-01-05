/*
 * COPYRIGHT:         See COPYING in the top level directory
 * PROJECT:           ReactOS kernel
 * PURPOSE:           GDI WNDOBJ Functions
 * FILE:              win32ss/gdi/eng/engwindow.c
 * PROGRAMER:         Gregor Anich
 */

#include <win32k.h>
#include <debug.h>
DBG_DEFAULT_CHANNEL(EngWnd);

INT gcountPWO = 0;

/* Lightweight periodic profiling for WNDOBJ change notifications.
 * This is intended for diagnosing GPU-driver regressions where the driver
 * change proc (and/or the clip recomputation) becomes disproportionately
 * expensive.
 */
static volatile ULONG g_EngWnd_ChangeProcCalls = 0;
static volatile ULONG g_EngWnd_WindowChangedCalls = 0;
static volatile LONG64 g_EngWnd_TotalChangeProcQpc = 0;
static volatile LONG64 g_EngWnd_TotalWindowChangedQpc = 0;
static volatile LONG64 g_EngWnd_MaxChangeProcQpc = 0;
static volatile LONG64 g_EngWnd_MaxWindowChangedQpc = 0;
static LARGE_INTEGER g_EngWnd_QpcFreq = { 0 };

static VOID
EngWndInitQpcFreq(VOID)
{
    if (g_EngWnd_QpcFreq.QuadPart == 0)
    {
        LARGE_INTEGER freq;
        (VOID)KeQueryPerformanceCounter(&freq);
        if (freq.QuadPart != 0)
        {
            g_EngWnd_QpcFreq = freq;
        }
    }
}

static VOID
EngWndUpdateMaxQpc(_Inout_ volatile LONG64* pMax, _In_ LONG64 value)
{
    LONG64 prev;
    do
    {
        prev = *pMax;
        if (value <= prev)
            return;
    } while (InterlockedCompareExchange64((volatile LONG64*)pMax, value, prev) != prev);
}

/*
 * Calls the WNDOBJCHANGEPROC of the given WNDOBJ
 */
VOID
FASTCALL
IntEngWndCallChangeProc(
    _In_ EWNDOBJ *Clip,
    _In_ FLONG   flChanged)
{
    LARGE_INTEGER qpcStart = { 0 };
    LARGE_INTEGER qpcEnd;
    LONG64 qpcDelta;
    ULONG callCount;

    if (Clip->ChangeProc == NULL)
    {
        return;
    }

    /* check flags of the WNDOBJ */
    flChanged &= Clip->Flags;
    if (flChanged == 0)
    {
        return;
    }

    TRACE("Calling WNDOBJCHANGEPROC (0x%p), Changed = 0x%x\n",
           Clip->ChangeProc, flChanged);

    EngWndInitQpcFreq();
    qpcStart = KeQueryPerformanceCounter(NULL);

    /* Call the WNDOBJCHANGEPROC */
    if (flChanged == WOC_CHANGED)
        Clip->ChangeProc(NULL, flChanged);
    else
        Clip->ChangeProc((WNDOBJ *)Clip, flChanged);

    qpcEnd = KeQueryPerformanceCounter(NULL);
    qpcDelta = qpcEnd.QuadPart - qpcStart.QuadPart;

    InterlockedExchangeAdd64(&g_EngWnd_TotalChangeProcQpc, qpcDelta);
    EngWndUpdateMaxQpc(&g_EngWnd_MaxChangeProcQpc, qpcDelta);
    callCount = (ULONG)InterlockedIncrement((volatile LONG*)&g_EngWnd_ChangeProcCalls);

    /* Print periodically to avoid spamming (and at early milestones). */
    if ((callCount == 1) || (callCount == 128) || ((callCount & 0x3FF) == 0))
    {
        const LONG64 freq = g_EngWnd_QpcFreq.QuadPart ? g_EngWnd_QpcFreq.QuadPart : 1;
        const LONG64 total = g_EngWnd_TotalChangeProcQpc;
        const LONG64 maxv = g_EngWnd_MaxChangeProcQpc;
        const LONG64 avg = (callCount != 0) ? (total / (LONG64)callCount) : 0;
        DbgPrint("ENGWND: ChangeProc calls=%lu avg=%.3fms max=%.3fms (qpc freq=%lld)\n",
                 callCount,
                 (double)avg * 1000.0 / (double)freq,
                 (double)maxv * 1000.0 / (double)freq,
                 freq);
    }
}

/*
 * Fills the CLIPOBJ and client rect of the WNDOBJ with the data from the given WND
 */
BOOLEAN
FASTCALL
IntEngWndUpdateClipObj(
    EWNDOBJ* Clip,
    PWND Window)
{
    PREGION visRgn;

    TRACE("IntEngWndUpdateClipObj\n");

    visRgn = VIS_ComputeVisibleRegion(Window, TRUE, TRUE, TRUE);
    if (visRgn != NULL)
    {
        if (visRgn->rdh.nCount > 0)
        {
            IntEngUpdateClipRegion((XCLIPOBJ*)Clip, visRgn->rdh.nCount, visRgn->Buffer, &visRgn->rdh.rcBound);
            TRACE("Created visible region with %lu rects\n", visRgn->rdh.nCount);
            TRACE("  BoundingRect: %d, %d  %d, %d\n",
                   visRgn->rdh.rcBound.left, visRgn->rdh.rcBound.top,
                   visRgn->rdh.rcBound.right, visRgn->rdh.rcBound.bottom);
            {
                ULONG i;
                for (i = 0; i < visRgn->rdh.nCount; i++)
                {
                    TRACE("  Rect #%lu: %ld,%ld  %ld,%ld\n", i+1,
                           visRgn->Buffer[i].left, visRgn->Buffer[i].top,
                           visRgn->Buffer[i].right, visRgn->Buffer[i].bottom);
                }
            }
        }
        REGION_Delete(visRgn);
    }
    else
    {
        /* Fall back to client rect */
        IntEngUpdateClipRegion((XCLIPOBJ*)Clip, 1, &Window->rcClient, &Window->rcClient);
    }

    /* Update the WNDOBJ */
    Clip->rclClient = Window->rcClient;
    Clip->iUniq++;

    return TRUE;
}

/*
 * Updates all WNDOBJs of the given WND and calls the change-procs.
 */
VOID
FASTCALL
IntEngWindowChanged(
    _In_    PWND  Window,
    _In_    FLONG flChanged)
{
    EWNDOBJ *Clip;
    LARGE_INTEGER qpcStart;
    LARGE_INTEGER qpcEnd;
    LONG64 qpcDelta;
    ULONG callCount;

    ASSERT_IRQL_LESS_OR_EQUAL(PASSIVE_LEVEL);

    Clip = UserGetProp(Window, AtomWndObj, TRUE);
    if (!Clip)
    {
        return;
    }

    ASSERT(Clip->Hwnd == Window->head.h);

    EngWndInitQpcFreq();
    qpcStart = KeQueryPerformanceCounter(NULL);
    // if (Clip->pvConsumer != NULL)
    {
        /* Update the WNDOBJ */
        switch (flChanged)
        {
        case WOC_RGN_CLIENT:
            /* Update the clipobj and client rect of the WNDOBJ */
            IntEngWndUpdateClipObj(Clip, Window);
            break;

        case WOC_DELETE:
            /* FIXME: Should the WNDOBJs be deleted by win32k or by the driver? */
            break;
        }

        /* Call the change proc */
        IntEngWndCallChangeProc(Clip, flChanged);

        /* HACK: Send WOC_CHANGED after WOC_RGN_CLIENT */
        if (flChanged == WOC_RGN_CLIENT)
        {
            IntEngWndCallChangeProc(Clip, WOC_CHANGED);
        }
    }

    qpcEnd = KeQueryPerformanceCounter(NULL);
    qpcDelta = qpcEnd.QuadPart - qpcStart.QuadPart;
    InterlockedExchangeAdd64(&g_EngWnd_TotalWindowChangedQpc, qpcDelta);
    EngWndUpdateMaxQpc(&g_EngWnd_MaxWindowChangedQpc, qpcDelta);
    callCount = (ULONG)InterlockedIncrement((volatile LONG*)&g_EngWnd_WindowChangedCalls);
    if ((callCount == 1) || (callCount == 128) || ((callCount & 0x3FF) == 0))
    {
        const LONG64 freq = g_EngWnd_QpcFreq.QuadPart ? g_EngWnd_QpcFreq.QuadPart : 1;
        const LONG64 total = g_EngWnd_TotalWindowChangedQpc;
        const LONG64 maxv = g_EngWnd_MaxWindowChangedQpc;
        const LONG64 avg = (callCount != 0) ? (total / (LONG64)callCount) : 0;
        DbgPrint("ENGWND: WindowChanged calls=%lu avg=%.3fms max=%.3fms (qpc freq=%lld)\n",
                 callCount,
                 (double)avg * 1000.0 / (double)freq,
                 (double)maxv * 1000.0 / (double)freq,
                 freq);
    }
}

/*
 * @implemented
 */
WNDOBJ*
APIENTRY
EngCreateWnd(
    SURFOBJ          *pso,
    HWND              hWnd,
    WNDOBJCHANGEPROC  pfn,
    FLONG             fl,
    int               iPixelFormat)
{
    EWNDOBJ *Clip = NULL;
    WNDOBJ *WndObjUser = NULL;
    PWND Window;

    TRACE("EngCreateWnd: pso = 0x%p, hwnd = 0x%p, pfn = 0x%p, fl = 0x%lx, pixfmt = %d\n",
            pso, hWnd, pfn, fl, iPixelFormat);

    UserEnterExclusive();

    if (fl & (WO_RGN_WINDOW | WO_RGN_DESKTOP_COORD | WO_RGN_UPDATE_ALL))
    {
        FIXME("Unsupported flags: 0x%lx\n", fl & ~(WO_RGN_CLIENT_DELTA | WO_RGN_CLIENT | WO_RGN_SURFACE_DELTA | WO_RGN_SURFACE));
    }

    /* Get window object */
    Window = UserGetWindowObject(hWnd);
    if (Window == NULL)
    {
        goto Exit;
    }

    /* Create WNDOBJ */
    Clip = EngAllocMem(FL_ZERO_MEMORY, sizeof(EWNDOBJ), GDITAG_WNDOBJ);
    if (Clip == NULL)
    {
        ERR("Failed to allocate memory for a WND structure!\n");
        goto Exit;
    }
    IntEngInitClipObj((XCLIPOBJ*)Clip);

    /* Fill the clipobj */
    if (!IntEngWndUpdateClipObj(Clip, Window))
    {
        EngFreeMem(Clip);
        goto Exit;
    }

    /* Fill user object */
    WndObjUser = (WNDOBJ *)Clip;
    WndObjUser->psoOwner = pso;
    WndObjUser->pvConsumer = NULL;

    /* Fill internal object */
    Clip->Hwnd = hWnd;
    Clip->ChangeProc = pfn;
    /* Keep track of relevant flags */
    Clip->Flags = fl & (WO_RGN_CLIENT_DELTA | WO_RGN_CLIENT | WO_RGN_SURFACE_DELTA | WO_RGN_SURFACE | WO_DRAW_NOTIFY);
    if (fl & WO_SPRITE_NOTIFY)
        Clip->Flags |= WOC_SPRITE_OVERLAP | WOC_SPRITE_NO_OVERLAP;
    /* Those should always be sent */
    Clip->Flags |= WOC_CHANGED | WOC_DELETE;
    Clip->PixelFormat = iPixelFormat;

    /* associate object with window */
    UserSetProp(Window, AtomWndObj, Clip, TRUE);
    ++gcountPWO;

    TRACE("EngCreateWnd: SUCCESS: %p!\n", WndObjUser);

Exit:
    UserLeave();
    return WndObjUser;
}


/*
 * @implemented
 */
VOID
APIENTRY
EngDeleteWnd(
    IN WNDOBJ *pwo)
{
    EWNDOBJ* Clip = (EWNDOBJ *)pwo;//CONTAINING_RECORD(pwo, XCLIPOBJ, WndObj);
    PWND Window;

    TRACE("EngDeleteWnd: pwo = 0x%p\n", pwo);

    UserEnterExclusive();

    /* Get window object */
    Window = UserGetWindowObject(Clip->Hwnd);
    if (Window == NULL)
    {
        ERR("Couldnt get window object for WndObjInt->Hwnd!!!\n");
    }
    else
    {
        /* Remove object from window */
        UserRemoveProp(Window, AtomWndObj, TRUE);
    }
    --gcountPWO;

    UserLeave();

    /* Free resources */
    IntEngFreeClipResources((XCLIPOBJ*)Clip);
    EngFreeMem(Clip);
}


/*
 * @implemented
 */
BOOL
APIENTRY
WNDOBJ_bEnum(
    IN WNDOBJ  *pwo,
    IN ULONG  cj,
    OUT ULONG  *pul)
{
    /* Relay */
    return CLIPOBJ_bEnum(&pwo->coClient, cj, pul);
}


/*
 * @implemented
 */
ULONG
APIENTRY
WNDOBJ_cEnumStart(
    IN WNDOBJ  *pwo,
    IN ULONG  iType,
    IN ULONG  iDirection,
    IN ULONG  cLimit)
{
    /* Relay */
    // FIXME: Should we enumerate all rectangles or not?
    return CLIPOBJ_cEnumStart(&pwo->coClient, FALSE, iType, iDirection, cLimit);
}


/*
 * @implemented
 */
VOID
APIENTRY
WNDOBJ_vSetConsumer(
    IN WNDOBJ  *pwo,
    IN PVOID  pvConsumer)
{
    EWNDOBJ* Clip = (EWNDOBJ *)pwo;//CONTAINING_RECORD(pwo, XCLIPOBJ, WndObj);
    BOOL Hack;

    TRACE("WNDOBJ_vSetConsumer: pwo = 0x%p, pvConsumer = 0x%p\n", pwo, pvConsumer);

    Hack = (pwo->pvConsumer == NULL);
    pwo->pvConsumer = pvConsumer;

    /* HACKHACKHACK
     *
     * MSDN says that the WNDOBJCHANGEPROC will be called with the most recent state
     * when a WNDOBJ is created - we do it here because most drivers will need pvConsumer
     * in the callback to identify the WNDOBJ I think.
     *
     *  - blight
     */
    if (Hack)
    {
        FIXME("Is this hack really needed?\n");
        IntEngWndCallChangeProc(Clip, WOC_RGN_CLIENT);
        IntEngWndCallChangeProc(Clip, WOC_CHANGED);
        IntEngWndCallChangeProc(Clip, WOC_DRAWN);
    }
}

/* EOF */

