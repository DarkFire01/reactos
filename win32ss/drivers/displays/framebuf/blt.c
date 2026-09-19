/*
 * PROJECT:     ReactOS generic framebuffer display driver
 * LICENSE:     Public Domain
 * PURPOSE:     Shadow-surface drawing and dirty-rectangle framebuffer flushing
 */

#include "framebuf.h"

#define FRAMEBUF_ROP4_SRCCOPY ((ROP4)0xCCCC)

static
PPDEV
IntUseShadowSurface(
    _Inout_ SURFOBJ **ppso)
{
    PPDEV ppdev;
    SURFOBJ *pso;

    pso = *ppso;

    if (!pso || (pso->iType != STYPE_DEVICE))
        return NULL;

    ppdev = (PPDEV)(pso->dhpdev);
    *ppso = ppdev->psoShadow;
    return ppdev;
}

VOID
IntFlushScreen(
    _In_ PPDEV ppdev,
    _In_ const RECTL *prcl)
{
    RECTL rcl;
    SURFOBJ *psoShadow;
    PVOID ScreenPtr;
    PBYTE pjSrc;
    PBYTE pjDest;
    ULONG cjScan;
    ULONG cjPixel;
    LONG lTemp;
    LONG y;

    psoShadow = ppdev->psoShadow;
    ScreenPtr = InterlockedCompareExchangePointer(
                    (PVOID volatile *)&ppdev->ScreenPtr,
                    NULL,
                    NULL);
    if (!psoShadow || !ScreenPtr)
        return;

    if (prcl)
    {
        rcl = *prcl;

        if (rcl.left > rcl.right)
        {
            lTemp = rcl.left;
            rcl.left = rcl.right;
            rcl.right = lTemp;
        }

        if (rcl.top > rcl.bottom)
        {
            lTemp = rcl.top;
            rcl.top = rcl.bottom;
            rcl.bottom = lTemp;
        }

        rcl.left = max(rcl.left, 0);
        rcl.top = max(rcl.top, 0);
        rcl.right = min(rcl.right, (LONG)ppdev->ScreenWidth);
        rcl.bottom = min(rcl.bottom, (LONG)ppdev->ScreenHeight);
    }
    else
    {
        rcl.left = 0;
        rcl.top = 0;
        rcl.right = (LONG)ppdev->ScreenWidth;
        rcl.bottom = (LONG)ppdev->ScreenHeight;
    }

    if ((rcl.left >= rcl.right) || (rcl.top >= rcl.bottom))
        return;

    cjPixel = ppdev->BitsPerPixel / 8;
    cjScan = (rcl.right - rcl.left) * cjPixel;
    pjSrc = (PBYTE)psoShadow->pvScan0 +
            rcl.top * psoShadow->lDelta + rcl.left * cjPixel;
    pjDest = (PBYTE)ScreenPtr +
             rcl.top * ppdev->ScreenDelta + rcl.left * cjPixel;

    /* Copy full-width rectangles in one operation when the pitches match. */
    if ((rcl.left == 0) &&
        (rcl.right == (LONG)ppdev->ScreenWidth) &&
        (psoShadow->lDelta == (LONG)ppdev->ScreenDelta))
    {
        RtlCopyMemory(pjDest,
                      pjSrc,
                      (SIZE_T)(rcl.bottom - rcl.top) * ppdev->ScreenDelta);
    }
    else
    {
        for (y = rcl.top; y < rcl.bottom; ++y)
        {
            RtlCopyMemory(pjDest, pjSrc, cjScan);
            pjSrc += psoShadow->lDelta;
            pjDest += ppdev->ScreenDelta;
        }
    }

    /* Complete write-combined stores before reporting the present done. */
    MemoryBarrier();
}

static
VOID
IntFlushClippedScreen(
    _In_ PPDEV ppdev,
    _In_ const RECTL *prcl,
    _In_ CLIPOBJ *pco)
{
    RECTL rcl = *prcl;
    LONG lTemp;

    if (rcl.left > rcl.right)
    {
        lTemp = rcl.left;
        rcl.left = rcl.right;
        rcl.right = lTemp;
    }

    if (rcl.top > rcl.bottom)
    {
        lTemp = rcl.top;
        rcl.top = rcl.bottom;
        rcl.bottom = lTemp;
    }

    if (pco && (pco->iDComplexity != DC_TRIVIAL))
    {
        rcl.left = max(rcl.left, pco->rclBounds.left);
        rcl.top = max(rcl.top, pco->rclBounds.top);
        rcl.right = min(rcl.right, pco->rclBounds.right);
        rcl.bottom = min(rcl.bottom, pco->rclBounds.bottom);
    }

    IntFlushScreen(ppdev, &rcl);
}

BOOL
APIENTRY
DrvBitBlt(
    _Inout_ SURFOBJ *psoTrg,
    _In_ SURFOBJ *psoSrc,
    _In_ SURFOBJ *psoMask,
    _In_ CLIPOBJ *pco,
    _In_ XLATEOBJ *pxlo,
    _In_ RECTL *prclTrg,
    _In_ POINTL *pptlSrc,
    _In_ POINTL *pptlMask,
    _In_ BRUSHOBJ *pbo,
    _In_ POINTL *pptlBrush,
    _In_ ROP4 rop4)
{
    PPDEV ppdevTrg;
    RECTL rclTrg;
    BOOL bResult;

    rclTrg = *prclTrg;

    ppdevTrg = IntUseShadowSurface(&psoTrg);
    IntUseShadowSurface(&psoSrc);
    IntUseShadowSurface(&psoMask);

    bResult = EngBitBlt(psoTrg,
                        psoSrc,
                        psoMask,
                        pco,
                        pxlo,
                        prclTrg,
                        pptlSrc,
                        pptlMask,
                        pbo,
                        pptlBrush,
                        rop4);

    if (bResult && ppdevTrg)
        IntFlushClippedScreen(ppdevTrg, &rclTrg, pco);

    return bResult;
}

BOOL
APIENTRY
DrvCopyBits(
    _Inout_ SURFOBJ *psoDest,
    _In_ SURFOBJ *psoSrc,
    _In_ CLIPOBJ *pco,
    _In_ XLATEOBJ *pxlo,
    _In_ RECTL *prclDest,
    _In_ POINTL *pptlSrc)
{
    return DrvBitBlt(psoDest,
                     psoSrc,
                     NULL,
                     pco,
                     pxlo,
                     prclDest,
                     pptlSrc,
                     NULL,
                     NULL,
                     NULL,
                     FRAMEBUF_ROP4_SRCCOPY);
}

BOOL
APIENTRY
DrvStretchBltROP(
    _Inout_ SURFOBJ *psoDest,
    _Inout_ SURFOBJ *psoSrc,
    _In_ SURFOBJ *psoMask,
    _In_ CLIPOBJ *pco,
    _In_ XLATEOBJ *pxlo,
    _In_ COLORADJUSTMENT *pca,
    _In_ POINTL *pptlHTOrg,
    _In_ RECTL *prclDest,
    _In_ RECTL *prclSrc,
    _In_ POINTL *pptlMask,
    _In_ ULONG iMode,
    _In_ BRUSHOBJ *pbo,
    _In_ DWORD rop4)
{
    PPDEV ppdev;
    RECTL rclDest = *prclDest;
    BOOL bResult;

    ppdev = IntUseShadowSurface(&psoDest);
    IntUseShadowSurface(&psoSrc);
    IntUseShadowSurface(&psoMask);

    bResult = EngStretchBltROP(psoDest,
                               psoSrc,
                               psoMask,
                               pco,
                               pxlo,
                               pca,
                               pptlHTOrg,
                               prclDest,
                               prclSrc,
                               pptlMask,
                               iMode,
                               pbo,
                               rop4);

    if (bResult && ppdev)
        IntFlushClippedScreen(ppdev, &rclDest, pco);

    return bResult;
}

BOOL
APIENTRY
DrvTransparentBlt(
    _Inout_ SURFOBJ *psoDest,
    _In_ SURFOBJ *psoSrc,
    _In_ CLIPOBJ *pco,
    _In_opt_ XLATEOBJ *pxlo,
    _In_ RECTL *prclDest,
    _In_ RECTL *prclSrc,
    _In_ ULONG iTransColor,
    _In_ ULONG ulReserved)
{
    PPDEV ppdev;
    RECTL rclDest = *prclDest;
    BOOL bResult;

    ppdev = IntUseShadowSurface(&psoDest);
    IntUseShadowSurface(&psoSrc);

    bResult = EngTransparentBlt(psoDest,
                                psoSrc,
                                pco,
                                pxlo,
                                prclDest,
                                prclSrc,
                                iTransColor,
                                ulReserved);

    if (bResult && ppdev)
        IntFlushClippedScreen(ppdev, &rclDest, pco);

    return bResult;
}

BOOL
APIENTRY
DrvAlphaBlend(
    _Inout_ SURFOBJ *psoDest,
    _In_ SURFOBJ *psoSrc,
    _In_ CLIPOBJ *pco,
    _In_opt_ XLATEOBJ *pxlo,
    _In_ RECTL *prclDest,
    _In_ RECTL *prclSrc,
    _In_ BLENDOBJ *pBlendObj)
{
    PPDEV ppdev;
    RECTL rclDest = *prclDest;
    BOOL bResult;

    ppdev = IntUseShadowSurface(&psoDest);
    IntUseShadowSurface(&psoSrc);

    bResult = EngAlphaBlend(psoDest,
                            psoSrc,
                            pco,
                            pxlo,
                            prclDest,
                            prclSrc,
                            pBlendObj);

    if (bResult && ppdev)
        IntFlushClippedScreen(ppdev, &rclDest, pco);

    return bResult;
}

BOOL
APIENTRY
DrvGradientFill(
    _Inout_ SURFOBJ *psoDest,
    _In_ CLIPOBJ *pco,
    _In_opt_ XLATEOBJ *pxlo,
    _In_ TRIVERTEX *pVertex,
    _In_ ULONG nVertex,
    _In_ PVOID pMesh,
    _In_ ULONG nMesh,
    _In_ RECTL *prclExtents,
    _In_ POINTL *pptlDitherOrg,
    _In_ ULONG ulMode)
{
    PPDEV ppdev;
    RECTL rclExtents = *prclExtents;
    BOOL bResult;

    ppdev = IntUseShadowSurface(&psoDest);
    bResult = EngGradientFill(psoDest,
                              pco,
                              pxlo,
                              pVertex,
                              nVertex,
                              pMesh,
                              nMesh,
                              prclExtents,
                              pptlDitherOrg,
                              ulMode);

    if (bResult && ppdev)
        IntFlushClippedScreen(ppdev, &rclExtents, pco);

    return bResult;
}

BOOL
APIENTRY
DrvLineTo(
    _Inout_ SURFOBJ *pso,
    _In_ CLIPOBJ *pco,
    _In_ BRUSHOBJ *pbo,
    _In_ LONG x1,
    _In_ LONG y1,
    _In_ LONG x2,
    _In_ LONG y2,
    _In_ RECTL *prclBounds,
    _In_ MIX mix)
{
    PPDEV ppdev;
    RECTL rclBounds;
    BOOL bResult;

    if (prclBounds)
    {
        rclBounds = *prclBounds;
    }
    else if (pco)
    {
        rclBounds = pco->rclBounds;
    }
    else
    {
        rclBounds.left = min(x1, x2);
        rclBounds.top = min(y1, y2);
        rclBounds.right = max(x1, x2) + 1;
        rclBounds.bottom = max(y1, y2) + 1;
    }

    ppdev = IntUseShadowSurface(&pso);
    bResult = EngLineTo(pso,
                        pco,
                        pbo,
                        x1,
                        y1,
                        x2,
                        y2,
                        prclBounds,
                        mix);

    if (bResult && ppdev)
        IntFlushClippedScreen(ppdev, &rclBounds, pco);

    return bResult;
}
