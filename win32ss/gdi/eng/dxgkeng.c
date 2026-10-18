/*
 * PROJECT:     ReactOS Win32k (WDDM display path)
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     The engine interface dxgkrnl calls win32k back through
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <win32k.h>

#define NDEBUG
#include <debug.h>

/* The DirectX share semaphore, see reactx/ntddraw/dxeng.c */
BOOLEAN NTAPI DxEngLockShareSem(VOID);
BOOLEAN NTAPI DxEngUnlockShareSem(VOID);

/* Slot order comes from the 14361 win32kbase PDB, and dxgkrnl indexes it by offset */
typedef struct _DXGKWIN32KENG_INTERFACE
{
    USHORT Size;
    USHORT Version;
    PVOID pfnDxgkEngVisRgnUniq;
    PVOID pfnDxgkEngLockVisRgn;
    PVOID pfnDxgkEngUnlockVisRgn;
    PVOID pfnDxgkEngEnterUserCrit;
    PVOID pfnDxgkEngLeaveUserCrit;
    PVOID pfnDxgkEngGetDC;
    PVOID pfnDxgkEngIsRedirectionDC;
    PVOID pfnDxgkEngReleaseDC;
    PVOID pfnDxgkEngGetClientRect;
    PVOID pfnDxgkEngCreateRectRgn;
    PVOID pfnDxgkEngGetVisRgn;
    PVOID pfnDxgkEngSetRgn;
    PVOID pfnDxgkEngCombineRgn;
    PVOID pfnDxgkEngGetRgnData;
    PVOID pfnDxgkEngGetBoxRgn;
    PVOID pfnDxgkEngDeleteObject;
    PVOID pfnDxgkEngDetectGDIPath;
    PVOID pfnDxgkEngBltViaGDI;
    PVOID pfnDxgkEngColorFillViaGDI;
    PVOID pfnDxgkEngLockShareSem;
    PVOID pfnDxgkEngUnlockShareSem;
    PVOID pfnDxgkEngAcquireWin32kAndPDEVLocks;
    PVOID pfnDxgkEngAssertGdiOutput;
    PVOID pfnDxgkEngResetPointer;
    PVOID pfnDxgkEngReleaseWin32kAndPDEVLocks;
    PVOID pfnDxgkEngScreenAccessCheck;
    PVOID pfnDxgkEngIsDwmProcess;
    PVOID pfnDxgkEngIsRemoteConnection;
    PVOID pfnDxgkEngGetRedirBitmapSharedHandle;
    PVOID pfnDxgkEngAddRedirBitmapD3DDirtyRgn;
    PVOID pfnDxgkEngAccumD3DPresentBounds;
    PVOID pfnDxgkEngRefPresentHistoryToken;
    PVOID pfnDxgkEngAcquireStableVisRgn;
    PVOID pfnDxgkEngReleaseStableVisRgn;
    PVOID pfnDxgkEngAcquireStableSprite;
    PVOID pfnDxgkEngReleaseStableSprite;
    PVOID pfnDxgkEngWatchVisRgnChange;
    PVOID pfnDxgkEngFindViewDesktopPosition;
    PVOID pfnDxgkEngIsDwmComposing;
    PVOID pfnDxgkEngQuerySwapChainBindingStatus;
    PVOID pfnDxgkEngGetRedirectedWindowOrigin;
    PVOID pfnDxgkEngAdjustMonitorPosition;
    PVOID pfnDxgkEngGetRemoteDeviceCount;
    PVOID pfnDxgkEngGetAdapterUniquenessPointer;
    PVOID pfbDxgkEngIncSpritetUniq;
    PVOID pfnDxgkEngQueryWin32Info;
    PVOID pfnDxgkEngGetWindowRect;
    PVOID pfnDxgkEngNotifyDisplayChange;
} DXGKWIN32KENG_INTERFACE;

/* Size and Version share the first pointer slot, so this is 392 bytes on x64 */
C_ASSERT(sizeof(DXGKWIN32KENG_INTERFACE) == 49 * sizeof(PVOID));

#define DXGKWIN32KENG_INTERFACE_VERSION     5

/* Request codes dxgkrnl passes to DxgkEngQueryWin32Info */
typedef enum _DXGKENG_QUERY_TYPE
{
    DxgkEngQueryDpiOverride = 0,
    DxgkEngQueryScaleFactors = 1,
    DxgkEngQuerySessionProtocol = 2,
    DxgkEngQueryTtmSupport = 3
} DXGKENG_QUERY_TYPE;

typedef struct _DXGKENG_QUERY
{
    DXGKENG_QUERY_TYPE Type;
    ULONG DataSize;
    PVOID Data;
} DXGKENG_QUERY, *PDXGKENG_QUERY;

/* Desktop scale factor table dxgkrnl picks its default DPI from */
typedef struct _DXGKENG_SCALE_FACTORS
{
    SIZE MinResolution;
    ULONG Count;
    const ULONG *Factors;
    const ULONG *Cutoffs;
} DXGKENG_SCALE_FACTORS, *PDXGKENG_SCALE_FACTORS;

/* Session protocol value dxgkrnl expects for a local console session */
#define DXGKENG_PROTOCOL_CONSOLE    0

static ULONG gulVisRgnUniq = 0;
static ULONG gulSpriteUniq = 0;
static volatile LONG glAdapterUniqueness = 0;

static const ULONG gaulScaleFactors[] = { 100, 125, 150, 175, 200, 225, 250, 300, 350, 400, 450, 500 };
static const ULONG gaulScaleCutoffs[] = { 120, 139, 178, 178, 232, 232, 275, 325, 375, 375, 450 };

/* PRIVATE FUNCTIONS **********************************************************/

static
ULONG
APIENTRY
DxgkEngVisRgnUniq(VOID)
{
    return gulVisRgnUniq;
}

/* The device the DC draws on, which the caller holds on to while it reads the region */
static
PVOID
APIENTRY
DxgkEngLockVisRgn(
    _In_ HDC hdc)
{
    PVOID ppdev = NULL;
    PDC pdc;

    pdc = DC_LockDc(hdc);
    if (pdc != NULL)
    {
        ppdev = pdc->ppdev;
        DC_UnlockDc(pdc);
    }

    return ppdev;
}

static
VOID
APIENTRY
DxgkEngUnlockVisRgn(
    _In_ PVOID ppdev)
{
    UNREFERENCED_PARAMETER(ppdev);
}

static
VOID
APIENTRY
DxgkEngEnterUserCrit(
    _In_ BOOL bExclusive)
{
    if (bExclusive)
        UserEnterExclusive();
    else
        UserEnterShared();
}

static
VOID
APIENTRY
DxgkEngLeaveUserCrit(VOID)
{
    UserLeave();
}

/**
 * @brief The client area DC of a window that is being presented to.
 *
 * @param hwnd The window.
 * @param phFullscreenDC Optionally receives a DC over the whole display the window is on.
 *
 * @return The window DC, NULL on failure.
 */
static
HDC
APIENTRY
DxgkEngGetDC(
    _In_ HWND hwnd,
    _Out_opt_ HDC *phFullscreenDC)
{
    HDC hdc;

    if (phFullscreenDC != NULL)
        *phFullscreenDC = NULL;

    hdc = NtUserGetDC(hwnd);
    if ((hdc == NULL) || (phFullscreenDC == NULL))
        return hdc;

    /* There is a single display device, so the primary's DC covers the window */
    *phFullscreenDC = IntGdiCreateDC(NULL, NULL, NULL, NULL, FALSE);
    if (*phFullscreenDC != NULL)
        return hdc;

    UserEnterExclusive();
    UserReleaseDC(NULL, hdc, FALSE);
    UserLeave();
    return NULL;
}

static
BOOL
APIENTRY
DxgkEngIsRedirectionDC(
    _In_ HDC hdc)
{
    UNREFERENCED_PARAMETER(hdc);

    return FALSE;
}

static
BOOL
APIENTRY
DxgkEngReleaseDC(
    _In_ HDC hdc,
    _In_opt_ HDC hFullscreenDC)
{
    INT iResult;

    if (hFullscreenDC != NULL)
        return IntGdiDeleteDC(hFullscreenDC, TRUE);

    UserEnterExclusive();
    iResult = UserReleaseDC(NULL, hdc, FALSE);
    UserLeave();

    return (iResult != 0);
}

/* The screen rectangle the DC draws into, the client area for a window DC */
static
BOOL
APIENTRY
DxgkEngGetClientRect(
    _In_ HDC hdc,
    _Out_ LPRECT prcl)
{
    PDC pdc;

    pdc = DC_LockDc(hdc);
    if (pdc == NULL)
        return FALSE;

    *prcl = *(LPRECT)&pdc->erclWindow;
    DC_UnlockDc(pdc);

    return TRUE;
}

static
HANDLE
APIENTRY
DxgkEngCreateRectRgn(
    _In_ INT left,
    _In_ INT top,
    _In_ INT right,
    _In_ INT bottom)
{
    return EngCreateRectRgn(left, top, right, bottom);
}

/**
 * @brief Copy one of a DC's regions into a region dxgkrnl created.
 *
 * @param hdc The DC.
 * @param hrgn Receives the region, in screen coordinates for the system region.
 * @param iCode Which region, as GetRandomRgn takes it.
 *
 * @return 1 when copied, 0 when the DC has no such region, -1 on failure.
 */
static
INT
APIENTRY
DxgkEngGetVisRgn(
    _In_ HDC hdc,
    _In_ HANDLE hrgn,
    _In_ INT iCode)
{
    PDC pdc;
    PREGION prgnDest;
    INT iResult = -1;

    /* 5 asks for the system region too, without the offset of a redirected window */
    if ((iCode != SYSRGN) && (iCode != SYSRGN + 1))
        return NtGdiGetRandomRgn(hdc, hrgn, iCode);

    pdc = DC_LockDc(hdc);
    if (pdc == NULL)
        return -1;

    prgnDest = (PREGION)GDIOBJ_ReferenceObjectByHandle(hrgn, GDIObjType_RGN_TYPE);
    if (prgnDest != NULL)
    {
        /* The DC keeps its visible region relative to its origin */
        if ((IntGdiCombineRgn(prgnDest, pdc->prgnVis, NULL, RGN_COPY) != ERROR) &&
            REGION_bOffsetRgn(prgnDest, pdc->ptlDCOrig.x, pdc->ptlDCOrig.y))
        {
            iResult = 1;
        }

        GDIOBJ_vDereferenceObject(&prgnDest->BaseObject);
    }

    DC_UnlockDc(pdc);
    return iResult;
}

static
BOOL
APIENTRY
DxgkEngSetRgn(
    _In_ HANDLE hrgn,
    _In_ INT left,
    _In_ INT top,
    _In_ INT right,
    _In_ INT bottom)
{
    return EngSetRectRgn(hrgn, left, top, right, bottom);
}

static
INT
APIENTRY
DxgkEngCombineRgn(
    _In_ HANDLE hrgnTrg,
    _In_ HANDLE hrgnSrc1,
    _In_ HANDLE hrgnSrc2,
    _In_ INT iMode)
{
    return EngCombineRgn(hrgnTrg, hrgnSrc1, hrgnSrc2, iMode);
}

static
DWORD
APIENTRY
DxgkEngGetRgnData(
    _In_ HANDLE hrgn,
    _In_ DWORD nCount,
    _Out_ LPRGNDATA lpRgnData)
{
    return EngGetRgnData(hrgn, nCount, lpRgnData);
}

static
INT
APIENTRY
DxgkEngGetBoxRgn(
    _In_ HANDLE hrgn,
    _Out_ LPRECT prcl)
{
    return EngGetRgnBox(hrgn, prcl);
}

/* dxgkrnl only ever hands back regions it got from DxgkEngCreateRectRgn */
static
BOOL
APIENTRY
DxgkEngDeleteObject(
    _In_ HANDLE hobj)
{
    EngDeleteRgn(hobj);
    return TRUE;
}

static
LONG
APIENTRY
DxgkEngDetectGDIPath(
    _In_ PVOID pvSurface,
    _In_ HDEV hdev,
    _In_ HWND hwnd,
    _In_ HANDLE hrgn)
{
    UNIMPLEMENTED_ONCE;

    UNREFERENCED_PARAMETER(pvSurface);
    UNREFERENCED_PARAMETER(hdev);
    UNREFERENCED_PARAMETER(hwnd);
    UNREFERENCED_PARAMETER(hrgn);

    return 0;
}

/* Presenting through GDI is not supported, so the caller keeps the work */
static
LONG
APIENTRY
DxgkEngBltViaGDI(
    _In_ PVOID pPresent,
    _In_ HDC hdc,
    _In_ PVOID prclSrc,
    _In_ PVOID prclDst,
    _In_ PVOID pvAllocation,
    _In_ ULONG ulFlags,
    _In_ ULONG ulRop,
    _In_ ULONG ulColor,
    _In_ UCHAR bFlag1,
    _In_ UCHAR bFlag2,
    _In_ ULONG ulCount,
    _In_ PVOID pfnCallback1,
    _In_ PVOID pfnCallback2)
{
    UNIMPLEMENTED_ONCE;

    UNREFERENCED_PARAMETER(pPresent);
    UNREFERENCED_PARAMETER(hdc);
    UNREFERENCED_PARAMETER(prclSrc);
    UNREFERENCED_PARAMETER(prclDst);
    UNREFERENCED_PARAMETER(pvAllocation);
    UNREFERENCED_PARAMETER(ulFlags);
    UNREFERENCED_PARAMETER(ulRop);
    UNREFERENCED_PARAMETER(ulColor);
    UNREFERENCED_PARAMETER(bFlag1);
    UNREFERENCED_PARAMETER(bFlag2);
    UNREFERENCED_PARAMETER(ulCount);
    UNREFERENCED_PARAMETER(pfnCallback1);
    UNREFERENCED_PARAMETER(pfnCallback2);

    return 0;
}

static
LONG
APIENTRY
DxgkEngColorFillViaGDI(
    _In_ HDC hdc,
    _In_ PVOID prclDst,
    _In_ PVOID prclClip,
    _In_ ULONG ulColor,
    _In_ ULONG ulFlags)
{
    UNIMPLEMENTED_ONCE;

    UNREFERENCED_PARAMETER(hdc);
    UNREFERENCED_PARAMETER(prclDst);
    UNREFERENCED_PARAMETER(prclClip);
    UNREFERENCED_PARAMETER(ulColor);
    UNREFERENCED_PARAMETER(ulFlags);

    return 0;
}

static
BOOL
APIENTRY
DxgkEngLockShareSem(VOID)
{
    return DxEngLockShareSem();
}

static
BOOL
APIENTRY
DxgkEngUnlockShareSem(VOID)
{
    return DxEngUnlockShareSem();
}

static
VOID
APIENTRY
DxgkEngAcquireWin32kAndPDEVLocks(
    _In_ PVOID pvDev,
    _In_ ULONG ulFlags)
{
    UNIMPLEMENTED_ONCE;

    UNREFERENCED_PARAMETER(pvDev);
    UNREFERENCED_PARAMETER(ulFlags);
}

/*
 * Hands the adapter's sources to or from the CDD. This is what starts the CDD presenting: it
 * asserts each display's mode with a command on the PDEV that the CDD reads back through
 * W32kCddGetWin32kCommand.
 */
static
LONG
APIENTRY
DxgkEngAssertGdiOutput(
    _In_ PVOID pAdapter,
    _In_reads_(cSources) const UCHAR *pCddStates,
    _In_ ULONG cSources,
    _Out_ PUCHAR pbResetPointer)
{
    UCHAR bResetPointer = 0;
    BOOL  bResult;

    if ((pCddStates == NULL) || (pbResetPointer == NULL))
        return 0;

    bResult = PDEVOBJ_bAssertGdiOutput(pAdapter, pCddStates, cSources, &bResetPointer);

    *pbResetPointer = bResetPointer;
    return bResult;
}

static
VOID
APIENTRY
DxgkEngResetPointer(VOID)
{
    UNIMPLEMENTED_ONCE;
}

static
VOID
APIENTRY
DxgkEngReleaseWin32kAndPDEVLocks(
    _In_ PVOID pvDev,
    _In_ ULONG ulFlags)
{
    UNIMPLEMENTED_ONCE;

    UNREFERENCED_PARAMETER(pvDev);
    UNREFERENCED_PARAMETER(ulFlags);
}

static
BOOL
APIENTRY
DxgkEngScreenAccessCheck(VOID)
{
    return TRUE;
}

/* There is no desktop window manager here */
static
BOOL
APIENTRY
DxgkEngIsDwmProcess(VOID)
{
    return FALSE;
}

static
LONG
APIENTRY
DxgkEngIsRemoteConnection(
    _Out_ PBOOL pbRemote)
{
    if (pbRemote != NULL)
        *pbRemote = FALSE;

    return 0;
}

static
VOID
APIENTRY
DxgkEngGetRedirBitmapSharedHandle(
    _In_ HDC hdc,
    _Out_ PVOID *phSurface)
{
    UNIMPLEMENTED_ONCE;

    UNREFERENCED_PARAMETER(hdc);

    if (phSurface != NULL)
        *phSurface = NULL;
}

static
VOID
APIENTRY
DxgkEngAddRedirBitmapD3DDirtyRgn(
    _In_ HDC hdc,
    _In_ PVOID pPresentInfo)
{
    UNIMPLEMENTED_ONCE;

    UNREFERENCED_PARAMETER(hdc);
    UNREFERENCED_PARAMETER(pPresentInfo);
}

static
VOID
APIENTRY
DxgkEngAccumD3DPresentBounds(
    _In_ HDC hdc,
    _In_ PVOID prcl)
{
    UNIMPLEMENTED_ONCE;

    UNREFERENCED_PARAMETER(hdc);
    UNREFERENCED_PARAMETER(prcl);
}

static
LONG
APIENTRY
DxgkEngRefPresentHistoryToken(
    _In_ PVOID pToken)
{
    UNIMPLEMENTED_ONCE;

    UNREFERENCED_PARAMETER(pToken);

    return 0;
}

/* Window positions, and so every visible region, only change under the user lock */
static
VOID
APIENTRY
DxgkEngAcquireStableVisRgn(
    _In_ HDC hdc)
{
    UNREFERENCED_PARAMETER(hdc);

    UserEnterShared();
}

static
VOID
APIENTRY
DxgkEngReleaseStableVisRgn(VOID)
{
    UserLeave();
}

/* GDI draws no sprites over a present, there is nothing to hold still */
static
VOID
APIENTRY
DxgkEngAcquireStableSprite(
    _In_ PVOID pvDev,
    _In_ BOOL bAcquireSpriteState)
{
    UNREFERENCED_PARAMETER(pvDev);
    UNREFERENCED_PARAMETER(bAcquireSpriteState);
}

static
VOID
APIENTRY
DxgkEngReleaseStableSprite(
    _In_ PVOID pvDev,
    _In_ BOOL bReleaseSpriteState)
{
    UNREFERENCED_PARAMETER(pvDev);
    UNREFERENCED_PARAMETER(bReleaseSpriteState);
}

/* Nothing reports visible region changes yet, dxgkrnl rereads the region every present */
static
VOID
APIENTRY
DxgkEngWatchVisRgnChange(
    _In_ HDC hdc,
    _In_ BOOL bEnable)
{
    UNREFERENCED_PARAMETER(hdc);
    UNREFERENCED_PARAMETER(bEnable);
}

static
LONG
APIENTRY
DxgkEngFindViewDesktopPosition(
    _In_ const LUID *pAdapterLuid,
    _In_ ULONG VidPnSourceId,
    _Out_ PPOINTL pptl)
{
    UNIMPLEMENTED_ONCE;

    UNREFERENCED_PARAMETER(pAdapterLuid);
    UNREFERENCED_PARAMETER(VidPnSourceId);

    if (pptl != NULL)
        RtlZeroMemory(pptl, sizeof(*pptl));

    return 0;
}

static
LONG
APIENTRY
DxgkEngIsDwmComposing(
    _Out_ PBOOL pbComposing)
{
    if (pbComposing != NULL)
        *pbComposing = FALSE;

    return 0;
}

static
LONG
APIENTRY
DxgkEngQuerySwapChainBindingStatus(
    _In_ LONG lIndex,
    _Out_ PVOID pToken)
{
    UNIMPLEMENTED_ONCE;

    UNREFERENCED_PARAMETER(lIndex);
    UNREFERENCED_PARAMETER(pToken);

    return 0;
}

/* No window is redirected without a compositor */
static
BOOL
APIENTRY
DxgkEngGetRedirectedWindowOrigin(
    _In_ HDC hdc,
    _Out_ PPOINT ppt)
{
    UNREFERENCED_PARAMETER(hdc);

    if (ppt != NULL)
        RtlZeroMemory(ppt, sizeof(*ppt));

    return FALSE;
}

/**
 * @brief Move a set of monitor rectangles so the primary sits at the origin.
 *
 * Every coordinate the display stack goes on to use is relative to this, so leaving the
 * rectangles where they came in leaves the desktop origin wherever the monitor happened to be
 * enumerated, and the damage rectangles built from it land outside the surface.
 *
 * @param ulFlags   Passed through and not read by AlignRects.
 * @param prc       The monitor rectangles, adjusted in place.
 * @param cRects    How many; Windows refuses more than 16.
 * @param iPrimary  Index of the primary, which is the one moved to (0,0).
 * @return Nonzero when the rectangles were adjusted.
 */
static
LONG
APIENTRY
DxgkEngAdjustMonitorPosition(
    _In_ ULONG ulFlags,
    _Inout_updates_(cRects) LPRECT prc,
    _In_ ULONG cRects,
    _In_ ULONG iPrimary)
{
    LONG  dx, dy;
    ULONG i;

    UNREFERENCED_PARAMETER(ulFlags);

    /* Beyond this it does not lay them out at all. */
    if ((prc == NULL) || (cRects == 0) || (cRects > 16) || (iPrimary >= cRects))
        return 0;

    /*
     * Windows also removes overlaps and gaps first, but only for more than one monitor.
     * That layout pass is not ported; a single monitor never needs it.
     */

    dx = -prc[iPrimary].left;
    dy = -prc[iPrimary].top;

    for (i = 0; i < cRects; i++)
    {
        prc[i].left   += dx;
        prc[i].right  += dx;
        prc[i].top    += dy;
        prc[i].bottom += dy;
    }

    return 1;
}

static
ULONG
APIENTRY
DxgkEngGetRemoteDeviceCount(VOID)
{
    return 0;
}

/* dxgkrnl watches this for display changes it has to notice */
static
volatile LONG *
APIENTRY
DxgkEngGetAdapterUniquenessPointer(VOID)
{
    return &glAdapterUniqueness;
}

static
VOID
APIENTRY
DxgkEngIncSpritetUniq(VOID)
{
    gulSpriteUniq++;
}

/**
 * @brief Answer dxgkrnl's session queries. ReactOS only has console sessions, no per-driver
 *        DPI override and no TTM, so every answer is the local console default.
 */
static
NTSTATUS
APIENTRY
DxgkEngQueryWin32Info(
    _Inout_ PDXGKENG_QUERY pQuery)
{
    PDXGKENG_SCALE_FACTORS Scale;

    switch (pQuery->Type)
    {
        case DxgkEngQueryDpiOverride:
            ASSERT(pQuery->DataSize == sizeof(ULONG));
            *(PULONG)pQuery->Data = 0;
            return STATUS_SUCCESS;

        case DxgkEngQueryScaleFactors:
            ASSERT(pQuery->DataSize == sizeof(*Scale));
            Scale = pQuery->Data;
            Scale->MinResolution.cx = 800;
            Scale->MinResolution.cy = 600;
            Scale->Count = RTL_NUMBER_OF(gaulScaleFactors);
            Scale->Factors = gaulScaleFactors;
            Scale->Cutoffs = gaulScaleCutoffs;
            return STATUS_SUCCESS;

        case DxgkEngQuerySessionProtocol:
            if (pQuery->DataSize != sizeof(ULONG))
                return STATUS_INVALID_PARAMETER;
            *(PULONG)pQuery->Data = DXGKENG_PROTOCOL_CONSOLE;
            return STATUS_SUCCESS;

        case DxgkEngQueryTtmSupport:
            if (pQuery->DataSize != sizeof(BOOLEAN))
                return STATUS_INVALID_PARAMETER;
            *(PBOOLEAN)pQuery->Data = FALSE;
            return STATUS_SUCCESS;

        default:
            return STATUS_NOT_IMPLEMENTED;
    }
}

static
LONG
APIENTRY
DxgkEngGetWindowRect(
    _In_ HWND hwnd,
    _Out_ LPRECT prcl)
{
    UNIMPLEMENTED_ONCE;

    UNREFERENCED_PARAMETER(hwnd);

    if (prcl != NULL)
        RtlZeroMemory(prcl, sizeof(*prcl));

    return 0;
}

static
VOID
APIENTRY
DxgkEngNotifyDisplayChange(VOID)
{
    UNIMPLEMENTED_ONCE;
}

/* GLOBALS ********************************************************************/

DXGKWIN32KENG_INTERFACE gDxgkWin32kEngInterface =
{
    sizeof(DXGKWIN32KENG_INTERFACE),
    DXGKWIN32KENG_INTERFACE_VERSION,
    DxgkEngVisRgnUniq,
    DxgkEngLockVisRgn,
    DxgkEngUnlockVisRgn,
    DxgkEngEnterUserCrit,
    DxgkEngLeaveUserCrit,
    DxgkEngGetDC,
    DxgkEngIsRedirectionDC,
    DxgkEngReleaseDC,
    DxgkEngGetClientRect,
    DxgkEngCreateRectRgn,
    DxgkEngGetVisRgn,
    DxgkEngSetRgn,
    DxgkEngCombineRgn,
    DxgkEngGetRgnData,
    DxgkEngGetBoxRgn,
    DxgkEngDeleteObject,
    DxgkEngDetectGDIPath,
    DxgkEngBltViaGDI,
    DxgkEngColorFillViaGDI,
    DxgkEngLockShareSem,
    DxgkEngUnlockShareSem,
    DxgkEngAcquireWin32kAndPDEVLocks,
    DxgkEngAssertGdiOutput,
    DxgkEngResetPointer,
    DxgkEngReleaseWin32kAndPDEVLocks,
    DxgkEngScreenAccessCheck,
    DxgkEngIsDwmProcess,
    DxgkEngIsRemoteConnection,
    DxgkEngGetRedirBitmapSharedHandle,
    DxgkEngAddRedirBitmapD3DDirtyRgn,
    DxgkEngAccumD3DPresentBounds,
    DxgkEngRefPresentHistoryToken,
    DxgkEngAcquireStableVisRgn,
    DxgkEngReleaseStableVisRgn,
    DxgkEngAcquireStableSprite,
    DxgkEngReleaseStableSprite,
    DxgkEngWatchVisRgnChange,
    DxgkEngFindViewDesktopPosition,
    DxgkEngIsDwmComposing,
    DxgkEngQuerySwapChainBindingStatus,
    DxgkEngGetRedirectedWindowOrigin,
    DxgkEngAdjustMonitorPosition,
    DxgkEngGetRemoteDeviceCount,
    DxgkEngGetAdapterUniquenessPointer,
    DxgkEngIncSpritetUniq,
    DxgkEngQueryWin32Info,
    DxgkEngGetWindowRect,
    DxgkEngNotifyDisplayChange
};

/* EOF */
