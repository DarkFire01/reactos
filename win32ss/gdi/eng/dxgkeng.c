/*
 * PROJECT:     ReactOS Win32k (WDDM display path)
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     The engine interface dxgkrnl calls win32k back through
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <win32k.h>
#include <d3dkmthk.h>
#include <reactos/rddm/rddm_private.h>

#define NDEBUG
#include <debug.h>

/* The DirectX share semaphore, see reactx/ntddraw/dxeng.c */
BOOLEAN NTAPI DxEngLockShareSem(VOID);
BOOLEAN NTAPI DxEngUnlockShareSem(VOID);

#if DXGKRNL_TARGET == DXGKRNL_TARGET_WIN11
/* The Windows 11 slot order, dxgkrnl indexes it by offset */
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
    PVOID pfnDxgkEngAcquireWin32kLocks;
    PVOID pfnDxgkEngReleaseWin32kLocks;
    PVOID pfnDxgkEngAcquireDwmStateAndPDevLocks;
    PVOID pfnDxgkEngReleasePDevAndDwmStateLocks;
    PVOID pfnDxgkEngAssertGdiOutput;
    PVOID pfnDxgkEngResetPointer;
    PVOID pfnDxgkEngScreenAccessCheck;
    PVOID pfnDxgkEngIsDwmProcess;
    PVOID pfnDxgkEngIsRemoteConnection;
    PVOID pfnDxgkEngGetRedirBitmapSharedHandle;
    PVOID pfnDxgkEngAddRedirBitmapD3DDirtyRgn;
    PVOID pfnDxgkEngAccumD3DPresentBounds;
    PVOID pfnDxgkEngAcquireStableVisRgn;
    PVOID pfnDxgkEngReleaseStableVisRgn;
    PVOID pfnDxgkEngAcquireStableSprite;
    PVOID pfnDxgkEngReleaseStableSprite;
    PVOID pfnDxgkEngWatchVisRgnChange;
    PVOID pfnDxgkEngFindViewDesktopPosition;
    PVOID pfnDxgkEngIsDwmComposing;
    PVOID pfnDxgkEngGetRedirectedWindowOrigin;
    PVOID pfnDxgkEngAdjustMonitorPosition;
    PVOID pfnDxgkEngGetRemoteDeviceCount;
    PVOID pfnDxgkEngGetAdapterUniquenessPointer;
    PVOID pfnDxgkEngIncPresentUniq;
    PVOID pfnDxgkEngQueryWin32Info;
    PVOID pfnDxgkEngGetWindowRect;
    PVOID pfnDxgkEngNotifyDisplayChange;
    PVOID pfnDxgkEngGetWindowGdiDpiScalingFactor;
    PVOID pfnDxgkEngGetMonitorUniquenessPointer;
    PVOID pfnDxgkEngSampleDisplayState;
    PVOID pfnDxgkEngIsGDIOn;
    PVOID pfnDxgkEngSetDisplayModeCallback;
    PVOID pfnDxgkEngProcessWin32kEscape;
    PVOID pfnDxgkEngIsWinLogonProcess;
    PVOID pfnDxgkEngSetHwProtectionTeardownRecovery;
    PVOID pfnDxgkEngQueryRemoteVidPnSourceFromGdiDisplayName;
    PVOID pfnDxgkEngCheckRemoteSessionRemoteAdapterAccess;
    PVOID pfnDxgkEngOpenAdapterFromHdc;
} DXGKWIN32KENG_INTERFACE;

/* Size and Version share the first pointer slot, so this is 480 bytes on x64 */
C_ASSERT(sizeof(DXGKWIN32KENG_INTERFACE) == 60 * sizeof(PVOID));

#define DXGKWIN32KENG_INTERFACE_VERSION     12

#else
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
#endif /* DXGKRNL_TARGET */

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

#if DXGKRNL_TARGET == DXGKRNL_TARGET_1607
static const ULONG gaulScaleFactors[] = { 100, 125, 150, 175, 200, 225, 250, 300, 350, 400, 450, 500 };
static const ULONG gaulScaleCutoffs[] = { 120, 139, 178, 178, 232, 232, 275, 325, 375, 375, 450 };
#endif

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

/**
 * @brief Does a present to this window have to go through GDI?
 *
 * GDI composes the whole display in the surface it owns, so a frame dxgkrnl writes to the
 * primary itself is lost on the next GDI update of the same area. Any present that shows on
 * this device is therefore drawn through GDI.
 *
 * @param pAdapter The adapter presenting.
 * @param hdev The device the window is on.
 * @param hwnd The window.
 * @param hVisRgn The window's visible region, in screen coordinates.
 *
 * @return TRUE when the present has to go through DxgkEngBltViaGDI or DxgkEngColorFillViaGDI.
 */
static
BOOL
APIENTRY
DxgkEngDetectGDIPath(
    _In_ PVOID pAdapter,
    _In_ HDEV hdev,
    _In_ HWND hwnd,
    _In_ HANDLE hVisRgn)
{
    PPDEVOBJ ppdev = (PPDEVOBJ)hdev;
    HANDLE hrgnDevice;
    SIZEL sizl;
    INT iComplexity;

    UNREFERENCED_PARAMETER(pAdapter);
    UNREFERENCED_PARAMETER(hwnd);

    if ((ppdev == NULL) || (hVisRgn == NULL))
        return FALSE;

    PDEVOBJ_sizl(ppdev, &sizl);
    hrgnDevice = EngCreateRectRgn(ppdev->ptlOrigion.x,
                                  ppdev->ptlOrigion.y,
                                  ppdev->ptlOrigion.x + sizl.cx,
                                  ppdev->ptlOrigion.y + sizl.cy);
    if (hrgnDevice == NULL)
        return FALSE;

    iComplexity = EngCombineRgn(hrgnDevice, hrgnDevice, hVisRgn, RGN_AND);
    EngDeleteRgn(hrgnDevice);

    return ((iComplexity != ERROR) && (iComplexity != NULLREGION));
}

/* dxgkrnl's helpers: map a rectangle from one rectangle onto another with a 16.16 scale,
   and clip a source and destination pair against the present's own rectangles */
typedef VOID (APIENTRY *PFN_DXGK_XFORM_RECT)(
    _In_ const RECT *prclTo,
    _In_ const RECT *prclFrom,
    _Out_ RECT *prclOut,
    _In_ const RECT *prclIn,
    _In_ ULONG fxScaleX,
    _In_ ULONG fxScaleY);

typedef BOOL (APIENTRY *PFN_DXGK_CLIP_RECTS)(
    _Inout_ RECT *prclSrc,
    _Inout_ RECT *prclDst,
    _In_opt_ const RECT *prclPresentSrc,
    _In_opt_ const RECT *prclPresentDst);

/* SRCCOPY that keeps a mirrored DC from flipping the frame */
#define DXGK_GDI_PRESENT_ROP            (SRCCOPY | NOMIRRORBITMAP)

/**
 * @brief Draw a presented back buffer into a window through GDI.
 *
 * @param pPresent The present dxgkrnl is carrying out.
 * @param hdcWindow The DC to draw into, from DxgkEngGetDC.
 * @param prclOutput Optionally the area the frame may cover, in screen coordinates.
 * @param prclSubRects The parts of the back buffer to show, SubRectCnt of them.
 * @param pvBits The locked back buffer.
 * @param ulPitch Its pitch in bytes.
 * @param cxBuffer Its width.
 * @param cyBuffer Its height.
 * @param b16Bpp TRUE for an R5G6B5 back buffer, FALSE for X8R8G8B8.
 * @param bColorKey TRUE to leave the pixels matching crColorKey out.
 * @param crColorKey The source color key.
 * @param pfnXformRect dxgkrnl's rectangle mapping helper.
 * @param pfnClipRects dxgkrnl's rectangle clipping helper.
 *
 * @return The result of the last blit.
 */
static
BOOL
APIENTRY
DxgkEngBltViaGDI(
    _In_ const D3DKMT_PRESENT *pPresent,
    _In_ HDC hdcWindow,
    _In_opt_ const RECT *prclOutput,
    _In_reads_(pPresent->SubRectCnt) const RECT *prclSubRects,
    _In_ PVOID pvBits,
    _In_ ULONG ulPitch,
    _In_ ULONG cxBuffer,
    _In_ ULONG cyBuffer,
    _In_ BOOLEAN b16Bpp,
    _In_ BOOLEAN bColorKey,
    _In_ ULONG crColorKey,
    _In_ PFN_DXGK_XFORM_RECT pfnXformRect,
    _In_ PFN_DXGK_CLIP_RECTS pfnClipRects)
{
    RECTL rclWindow, rclLimit, rclSrc, rclDst, rclSubSrc, rclSubDst;
    BOOL bLimit = FALSE;
    BOOL bResult = FALSE;
    DWORD dwLayout;
    HBITMAP hbmBuffer;
    HDC hdcBuffer;
    PDC pdc;
    ULONG fxScaleX, fxScaleY, fxInverseX = 0, fxInverseY = 0;
    ULONG i;
    INT iOldStretchMode;

    if (hdcWindow == NULL)
        return FALSE;

    pdc = DC_LockDc(hdcWindow);
    if (pdc == NULL)
        return FALSE;
    rclWindow = pdc->erclWindow;
    dwLayout = pdc->pdcattr->dwLayout;
    DC_UnlockDc(pdc);

    /* Without a window the output rectangle stands in for one */
    if ((prclOutput != NULL) && (pPresent->hWindow == NULL))
        rclWindow = *(const RECTL *)prclOutput;

    /* The part of the window the frame may cover, relative to the window */
    if ((prclOutput != NULL) && (pPresent->hWindow != NULL))
    {
        RECTL_bIntersectRect(&rclLimit, &rclWindow, (const RECTL *)prclOutput);
        if (!RtlEqualMemory(&rclLimit, &rclWindow, sizeof(rclLimit)))
        {
            RECTL_vOffsetRect(&rclLimit, -rclWindow.left, -rclWindow.top);
            bLimit = TRUE;
        }
    }

    RECTL_vSetRect(&rclDst, 0, 0, rclWindow.right - rclWindow.left, rclWindow.bottom - rclWindow.top);
    RECTL_vSetRect(&rclSrc, 0, 0, (LONG)cxBuffer, (LONG)cyBuffer);

    if (!pfnClipRects((RECT *)&rclSrc,
                      (RECT *)&rclDst,
                      pPresent->Flags.SrcRectValid ? &pPresent->SrcRect : NULL,
                      pPresent->Flags.DstRectValid ? &pPresent->DstRect : NULL) ||
        (rclDst.right == 0) || (rclDst.bottom == 0))
    {
        /* Nothing of the frame shows */
        return TRUE;
    }

    /* A full screen DC draws in screen coordinates */
    if ((prclOutput != NULL) && (pPresent->hWindow == NULL))
        RECTL_vOffsetRect(&rclDst, prclOutput->left, prclOutput->top);

    hbmBuffer = GreCreateBitmapEx(cxBuffer,
                                  cyBuffer,
                                  ulPitch,
                                  b16Bpp ? BMF_16BPP : BMF_32BPP,
                                  BMF_TOPDOWN,
                                  0,
                                  pvBits,
                                  0);
    if (hbmBuffer == NULL)
        return FALSE;

    hdcBuffer = GreCreateCompatibleDC(hdcWindow, FALSE);
    if (hdcBuffer == NULL)
    {
        GreDeleteObject(hbmBuffer);
        return FALSE;
    }
    NtGdiSelectBitmap(hdcBuffer, hbmBuffer);

    /* The frame is never mirrored */
    if (dwLayout & LAYOUT_ORIENTATIONMASK)
        NtGdiSetLayout(hdcWindow, -1, 0);

    if (((rclDst.right - rclDst.left) == (rclSrc.right - rclSrc.left)) &&
        ((rclDst.bottom - rclDst.top) == (rclSrc.bottom - rclSrc.top)))
    {
        for (i = 0; i < pPresent->SubRectCnt; i++)
        {
            RECTL_bIntersectRect(&rclSubSrc, (const RECTL *)&prclSubRects[i], &rclSrc);

            rclSubDst = rclSubSrc;
            RECTL_vOffsetRect(&rclSubDst, rclDst.left - rclSrc.left, rclDst.top - rclSrc.top);
            if (bLimit)
            {
                RECTL_bIntersectRect(&rclSubDst, &rclSubDst, &rclLimit);
                rclSubSrc.left = rclSubDst.left - (rclDst.left - rclSrc.left);
                rclSubSrc.top = rclSubDst.top - (rclDst.top - rclSrc.top);
            }

            if (bColorKey)
            {
                bResult = NtGdiTransparentBlt(hdcWindow,
                                              rclSubDst.left,
                                              rclSubDst.top,
                                              rclSubDst.right - rclSubDst.left,
                                              rclSubDst.bottom - rclSubDst.top,
                                              hdcBuffer,
                                              rclSubSrc.left,
                                              rclSubSrc.top,
                                              rclSubDst.right - rclSubDst.left,
                                              rclSubDst.bottom - rclSubDst.top,
                                              crColorKey);
            }
            else
            {
                bResult = NtGdiBitBlt(hdcWindow,
                                      rclSubDst.left,
                                      rclSubDst.top,
                                      rclSubDst.right - rclSubDst.left,
                                      rclSubDst.bottom - rclSubDst.top,
                                      hdcBuffer,
                                      rclSubSrc.left,
                                      rclSubSrc.top,
                                      DXGK_GDI_PRESENT_ROP,
                                      0,
                                      0);
            }
        }
    }
    else
    {
        fxScaleX = ((ULONG)(rclDst.right - rclDst.left) << 16) / (rclSrc.right - rclSrc.left);
        fxScaleY = ((ULONG)(rclDst.bottom - rclDst.top) << 16) / (rclSrc.bottom - rclSrc.top);
        if (bLimit)
        {
            fxInverseX = ((ULONG)(rclSrc.right - rclSrc.left) << 16) / (rclDst.right - rclDst.left);
            fxInverseY = ((ULONG)(rclSrc.bottom - rclSrc.top) << 16) / (rclDst.bottom - rclDst.top);
        }

        iOldStretchMode = GreSetStretchBltMode(hdcWindow, COLORONCOLOR);

        for (i = 0; i < pPresent->SubRectCnt; i++)
        {
            RECTL_bIntersectRect(&rclSubSrc, (const RECTL *)&prclSubRects[i], &rclSrc);
            pfnXformRect((RECT *)&rclDst, (RECT *)&rclSrc, (RECT *)&rclSubDst, (RECT *)&rclSubSrc, fxScaleX, fxScaleY);

            if (bLimit && RECTL_bIntersectRect(&rclSubDst, &rclSubDst, &rclLimit))
                pfnXformRect((RECT *)&rclSrc, (RECT *)&rclDst, (RECT *)&rclSubSrc, (RECT *)&rclSubDst, fxInverseX, fxInverseY);

            if (bColorKey)
            {
                bResult = NtGdiTransparentBlt(hdcWindow,
                                              rclSubDst.left,
                                              rclSubDst.top,
                                              rclSubDst.right - rclSubDst.left,
                                              rclSubDst.bottom - rclSubDst.top,
                                              hdcBuffer,
                                              rclSubSrc.left,
                                              rclSubSrc.top,
                                              rclSubSrc.right - rclSubSrc.left,
                                              rclSubSrc.bottom - rclSubSrc.top,
                                              crColorKey);
            }
            else
            {
                bResult = NtGdiStretchBlt(hdcWindow,
                                          rclSubDst.left,
                                          rclSubDst.top,
                                          rclSubDst.right - rclSubDst.left,
                                          rclSubDst.bottom - rclSubDst.top,
                                          hdcBuffer,
                                          rclSubSrc.left,
                                          rclSubSrc.top,
                                          rclSubSrc.right - rclSubSrc.left,
                                          rclSubSrc.bottom - rclSubSrc.top,
                                          DXGK_GDI_PRESENT_ROP,
                                          0);
            }
        }

        GreSetStretchBltMode(hdcWindow, iOldStretchMode);
    }

    if (dwLayout & LAYOUT_ORIENTATIONMASK)
        NtGdiSetLayout(hdcWindow, -1, dwLayout);

    IntGdiDeleteDC(hdcBuffer, TRUE);
    GreDeleteObject(hbmBuffer);

    return bResult;
}

/**
 * @brief Fill parts of a window with a solid color through GDI.
 *
 * @param hdcWindow The DC to draw into, from DxgkEngGetDC.
 * @param prclOutput Optionally the origin the rectangles are relative to.
 * @param prclSubRects The rectangles to fill.
 * @param cRects How many.
 * @param ulColor The color, as X8R8G8B8.
 *
 * @return The result of the last fill.
 */
static
BOOL
APIENTRY
DxgkEngColorFillViaGDI(
    _In_ HDC hdcWindow,
    _In_opt_ const RECT *prclOutput,
    _In_reads_(cRects) const RECT *prclSubRects,
    _In_ ULONG cRects,
    _In_ ULONG ulColor)
{
    HBRUSH hbrFill, hbrOld;
    LONG dx = 0, dy = 0;
    BOOL bResult = FALSE;
    ULONG i;

    if (hdcWindow == NULL)
        return FALSE;

    hbrFill = IntGdiCreateSolidBrush(RGB((ulColor >> 16) & 0xFF, (ulColor >> 8) & 0xFF, ulColor & 0xFF));
    if (hbrFill == NULL)
        return FALSE;

    hbrOld = NtGdiSelectBrush(hdcWindow, hbrFill);

    if (prclOutput != NULL)
    {
        dx = prclOutput->left;
        dy = prclOutput->top;
    }

    for (i = 0; i < cRects; i++)
    {
        bResult = NtGdiPatBlt(hdcWindow,
                              prclSubRects[i].left + dx,
                              prclSubRects[i].top + dy,
                              prclSubRects[i].right - prclSubRects[i].left,
                              prclSubRects[i].bottom - prclSubRects[i].top,
                              PATCOPY);
    }

    if (hbrOld != NULL)
        NtGdiSelectBrush(hdcWindow, hbrOld);
    GreDeleteObject(hbrFill);

    return bResult;
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

#if DXGKRNL_TARGET == DXGKRNL_TARGET_1607
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
#endif

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

#if DXGKRNL_TARGET == DXGKRNL_TARGET_1607
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
#endif

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

#if DXGKRNL_TARGET == DXGKRNL_TARGET_1607
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
#endif

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

#if DXGKRNL_TARGET == DXGKRNL_TARGET_1607
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
#endif

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
 * @param ulFlags   1607 only, passed through and not read by AlignRects.
 * @param prc       The monitor rectangles, adjusted in place.
 * @param cRects    How many; Windows refuses more than 16.
 * @param iPrimary  Index of the primary, which is the one moved to (0,0).
 * @return Nonzero when the rectangles were adjusted.
 */
static
LONG
APIENTRY
DxgkEngAdjustMonitorPosition(
#if (DXGKRNL_TARGET == DXGKRNL_TARGET_1607)
    _In_ ULONG ulFlags,
#endif
    _Inout_updates_(cRects) LPRECT prc,
    _In_ ULONG cRects,
    _In_ ULONG iPrimary)
{
    LONG  dx, dy;
    ULONG i;

#if (DXGKRNL_TARGET == DXGKRNL_TARGET_1607)
    UNREFERENCED_PARAMETER(ulFlags);
#endif

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

#if DXGKRNL_TARGET == DXGKRNL_TARGET_1607
static
VOID
APIENTRY
DxgkEngIncSpritetUniq(VOID)
{
    gulSpriteUniq++;
}
#endif

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
#if DXGKRNL_TARGET == DXGKRNL_TARGET_WIN11
            /* Windows 11 only gets the scale factors from an optional provider */
            UNREFERENCED_PARAMETER(Scale);
            return STATUS_NOT_SUPPORTED;
#else
            ASSERT(pQuery->DataSize == sizeof(*Scale));
            Scale = pQuery->Data;
            Scale->MinResolution.cx = 800;
            Scale->MinResolution.cy = 600;
            Scale->Count = RTL_NUMBER_OF(gaulScaleFactors);
            Scale->Factors = gaulScaleFactors;
            Scale->Cutoffs = gaulScaleCutoffs;
            return STATUS_SUCCESS;
#endif

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

#if DXGKRNL_TARGET == DXGKRNL_TARGET_WIN11
/* Bumped whenever the set of monitors changes */
static volatile LONG glMonitorUniqueness = 0;

/*
 * The locks dxgkrnl takes around a mode change. GDI and USER already hold their own locks
 * around everything that changes the display, so nothing more is taken here.
 */
static
VOID
APIENTRY
DxgkEngAcquireWin32kLocks(VOID)
{
}

static
VOID
APIENTRY
DxgkEngReleaseWin32kLocks(VOID)
{
}

static
VOID
APIENTRY
DxgkEngAcquireDwmStateAndPDevLocks(
    _In_ PVOID pAdapter,
    _In_ ULONG cSources)
{
    UNREFERENCED_PARAMETER(pAdapter);
    UNREFERENCED_PARAMETER(cSources);
}

static
VOID
APIENTRY
DxgkEngReleasePDevAndDwmStateLocks(
    _In_ PVOID pAdapter,
    _In_ ULONG cSources)
{
    UNREFERENCED_PARAMETER(pAdapter);
    UNREFERENCED_PARAMETER(cSources);
}

static
VOID
APIENTRY
DxgkEngIncPresentUniq(VOID)
{
    gulSpriteUniq++;
}

/* GDI draws every window at the system DPI, so a window is never scaled */
static
ULONG
APIENTRY
DxgkEngGetWindowGdiDpiScalingFactor(
    _In_ HWND hwnd)
{
    UNREFERENCED_PARAMETER(hwnd);
    return 1;
}

static
volatile LONG *
APIENTRY
DxgkEngGetMonitorUniquenessPointer(VOID)
{
    return &glMonitorUniqueness;
}

/**
 * @brief Takes a snapshot of the display state counters for dxgkrnl.
 *
 * @param[out] pulState
 * Seven counters: the display state, the monitors, the adapters, then four kept zero.
 */
static
NTSTATUS
APIENTRY
DxgkEngSampleDisplayState(
    _Out_writes_(7) PULONG pulState)
{
    if (!pulState)
        return STATUS_INVALID_PARAMETER;

    RtlZeroMemory(pulState, 7 * sizeof(ULONG));
    pulState[1] = (ULONG)glMonitorUniqueness;
    pulState[2] = (ULONG)glAdapterUniqueness;
    return STATUS_SUCCESS;
}

/* The display is never powered off underneath GDI */
static
BOOLEAN
APIENTRY
DxgkEngIsGDIOn(VOID)
{
    return TRUE;
}

/* A mode change GDI asks for updates its PDEV itself, so dxgkrnl has nothing to hand over */
static
NTSTATUS
APIENTRY
DxgkEngSetDisplayModeCallback(
    _In_opt_ PVOID pContext,
    _In_ ULONG VidPnSourceId)
{
    UNREFERENCED_PARAMETER(pContext);
    UNREFERENCED_PARAMETER(VidPnSourceId);
    return STATUS_SUCCESS;
}

static
NTSTATUS
APIENTRY
DxgkEngProcessWin32kEscape(
    _In_ PVOID pEscape)
{
    UNREFERENCED_PARAMETER(pEscape);
    return STATUS_NOT_SUPPORTED;
}

static
BOOLEAN
APIENTRY
DxgkEngIsWinLogonProcess(VOID)
{
    return (BOOLEAN)(PsGetCurrentProcessId() == gpidLogon);
}

/* Hardware content protection is never used */
static
NTSTATUS
APIENTRY
DxgkEngSetHwProtectionTeardownRecovery(
    _In_ PVOID pRecovery)
{
    UNREFERENCED_PARAMETER(pRecovery);
    return STATUS_NOT_SUPPORTED;
}

/* Only console sessions exist, so no display is ever remote */
static
NTSTATUS
APIENTRY
DxgkEngQueryRemoteVidPnSourceFromGdiDisplayName(
    _Inout_ PVOID pQuery)
{
    UNREFERENCED_PARAMETER(pQuery);
    return STATUS_NOT_SUPPORTED;
}

static
NTSTATUS
APIENTRY
DxgkEngCheckRemoteSessionRemoteAdapterAccess(
    _In_opt_ PVOID pAdapter)
{
    UNREFERENCED_PARAMETER(pAdapter);
    return STATUS_SUCCESS;
}

/* The adapter behind a DC is found from the PDEV it draws on */
static
NTSTATUS
APIENTRY
DxgkEngOpenAdapterFromHdc(
    _Inout_ D3DKMT_OPENADAPTERFROMHDC *pOpenAdapter)
{
    return NtGdiDdDDIOpenAdapterFromHdc(pOpenAdapter);
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
    DxgkEngAcquireWin32kLocks,
    DxgkEngReleaseWin32kLocks,
    DxgkEngAcquireDwmStateAndPDevLocks,
    DxgkEngReleasePDevAndDwmStateLocks,
    DxgkEngAssertGdiOutput,
    DxgkEngResetPointer,
    DxgkEngScreenAccessCheck,
    DxgkEngIsDwmProcess,
    DxgkEngIsRemoteConnection,
    DxgkEngGetRedirBitmapSharedHandle,
    DxgkEngAddRedirBitmapD3DDirtyRgn,
    DxgkEngAccumD3DPresentBounds,
    DxgkEngAcquireStableVisRgn,
    DxgkEngReleaseStableVisRgn,
    DxgkEngAcquireStableSprite,
    DxgkEngReleaseStableSprite,
    DxgkEngWatchVisRgnChange,
    DxgkEngFindViewDesktopPosition,
    DxgkEngIsDwmComposing,
    DxgkEngGetRedirectedWindowOrigin,
    DxgkEngAdjustMonitorPosition,
    DxgkEngGetRemoteDeviceCount,
    DxgkEngGetAdapterUniquenessPointer,
    DxgkEngIncPresentUniq,
    DxgkEngQueryWin32Info,
    DxgkEngGetWindowRect,
    DxgkEngNotifyDisplayChange,
    DxgkEngGetWindowGdiDpiScalingFactor,
    DxgkEngGetMonitorUniquenessPointer,
    DxgkEngSampleDisplayState,
    DxgkEngIsGDIOn,
    DxgkEngSetDisplayModeCallback,
    DxgkEngProcessWin32kEscape,
    DxgkEngIsWinLogonProcess,
    DxgkEngSetHwProtectionTeardownRecovery,
    DxgkEngQueryRemoteVidPnSourceFromGdiDisplayName,
    DxgkEngCheckRemoteSessionRemoteAdapterAccess,
    DxgkEngOpenAdapterFromHdc
};

#else
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
#endif /* DXGKRNL_TARGET */

/* EOF */
