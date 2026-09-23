/*
 * PROJECT:     ReactOS win32k
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     GDI DDI region services (EngXxxRgn)
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 *
 * The region half of the graphics DDI. These are declared in winddi.h but have never existed in
 * ReactOS - no implementation, no stub, no export - so a display driver had no way to build or
 * query a region at all.
 *
 * The Canonical Display Driver needs them: it accumulates the desktop's damage as a region and
 * hands the resulting rectangle list to dxgkrnl at present time (Reference cdd.c:21365,
 * CddUpdatePresentRects), which is how a windowed present learns which parts of the primary to
 * update. Without regions there is no damage list, and without a damage list dxgkrnl has nothing
 * to give the miniport.
 *
 * Every routine here is a thin wrapper over win32k's existing region engine (ntgdi/region.c); the
 * algorithms are already there and are not duplicated. A driver-created region is owned by
 * GDI_OBJ_HMGR_PUBLIC so that it can be locked from whichever thread the driver happens to be
 * running on - drivers are not tied to the creating process the way a user-mode caller is.
 */

#include <win32k.h>

#define NDEBUG
#include <debug.h>

/**
 * @brief Take hold of a driver's region.
 *
 * Two things differ from the user-mode region path, and both follow the Reference, where every
 * EngXxxRgn entry point opens with GreGetObjectOwner and RGNOBJAPI holds the region by refcount:
 *
 * - Only a region a driver made (GDI_OBJ_HMGR_PUBLIC) is accepted, so the DDI can never reach a
 *   region that belongs to a process.
 * - It takes a reference rather than the ordered exclusive lock. A driver calls these from inside
 *   a Drv* callback that already runs with its surface locked, and RGN sorts below SURF, so the
 *   exclusive lock would report a lock order violation for what is really a private object.
 *   Serializing a driver region is the driver's own business; the CDD holds its device lock
 *   across every one of these calls.
 *
 * @return The region, or NULL when the handle is not a driver region.
 */
static
PREGION
RGNOBJ_pLockDriverRgn(
    _In_ HANDLE hrgn)
{
    if (GreGetObjectOwner((HGDIOBJ)hrgn) != GDI_OBJ_HMGR_PUBLIC)
        return NULL;

    return (PREGION)GDIOBJ_ReferenceObjectByHandle((HGDIOBJ)hrgn, GDIObjType_RGN_TYPE);
}

static
VOID
RGNOBJ_vUnlockDriverRgn(
    _In_ PREGION prgn)
{
    GDIOBJ_vDereferenceObject((POBJ)prgn);
}

/**
 * @brief Create a rectangular region owned by the calling driver.
 * @return An HRGN-shaped handle, or NULL on failure.
 */
ENGAPI
HANDLE
APIENTRY
EngCreateRectRgn(
    _In_ INT left,
    _In_ INT top,
    _In_ INT right,
    _In_ INT bottom)
{
    PREGION prgn;
    HRGN    hrgn;

    /* Kernel-side region: no RGN_ATTR, unlike the user path (REGION_AllocUserRgnWithHandle). */
    prgn = REGION_AllocRgnWithHandle(1);
    if (prgn == NULL)
    {
        EngSetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }

    hrgn = prgn->BaseObject.hHmgr;
    REGION_SetRectRgn(prgn, left, top, right, bottom);
    REGION_UnlockRgn(prgn);

    /* A driver's region outlives any one process context; make it globally lockable. */
    GreSetObjectOwner(hrgn, GDI_OBJ_HMGR_PUBLIC);

    return (HANDLE)hrgn;
}

ENGAPI
VOID
APIENTRY
EngDeleteRgn(
    _In_ HANDLE hrgn)
{
    if (GreGetObjectOwner((HGDIOBJ)hrgn) != GDI_OBJ_HMGR_PUBLIC)
        return;

    /* GreDeleteObject refuses a public object, so take the region back before dropping it */
    GreSetObjectOwner((HGDIOBJ)hrgn, GDI_OBJ_HMGR_POWNED);
    GreDeleteObject((HRGN)hrgn);
}

ENGAPI
BOOL
APIENTRY
EngSetRectRgn(
    _In_ HANDLE hrgn,
    _In_ INT left,
    _In_ INT top,
    _In_ INT right,
    _In_ INT bottom)
{
    PREGION prgn = RGNOBJ_pLockDriverRgn(hrgn);

    if (prgn == NULL)
    {
        EngSetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }

    REGION_SetRectRgn(prgn, left, top, right, bottom);
    RGNOBJ_vUnlockDriverRgn(prgn);
    return TRUE;
}

/**
 * @brief Combine two regions into a third (RGN_AND / RGN_OR / RGN_XOR / RGN_DIFF / RGN_COPY).
 * @return The resulting region's complexity, or ERROR.
 */
ENGAPI
INT
APIENTRY
EngCombineRgn(
    _In_ HANDLE hrgnTrg,
    _In_ HANDLE hrgnSrc1,
    _In_ HANDLE hrgnSrc2,
    _In_ INT iMode)
{
    PREGION prgnTrg, prgnSrc1, prgnSrc2 = NULL;
    INT     iResult = ERROR;

    prgnTrg = RGNOBJ_pLockDriverRgn(hrgnTrg);
    if (prgnTrg == NULL)
    {
        EngSetLastError(ERROR_INVALID_HANDLE);
        return ERROR;
    }

    prgnSrc1 = RGNOBJ_pLockDriverRgn(hrgnSrc1);
    if (prgnSrc1 == NULL)
    {
        RGNOBJ_vUnlockDriverRgn(prgnTrg);
        EngSetLastError(ERROR_INVALID_HANDLE);
        return ERROR;
    }

    /* RGN_COPY takes a single source; every other mode needs both. */
    if (iMode != RGN_COPY)
    {
        prgnSrc2 = RGNOBJ_pLockDriverRgn(hrgnSrc2);
        if (prgnSrc2 == NULL)
        {
            RGNOBJ_vUnlockDriverRgn(prgnSrc1);
            RGNOBJ_vUnlockDriverRgn(prgnTrg);
            EngSetLastError(ERROR_INVALID_HANDLE);
            return ERROR;
        }
    }

    iResult = IntGdiCombineRgn(prgnTrg, prgnSrc1, prgnSrc2, iMode);

    if (prgnSrc2 != NULL)
        RGNOBJ_vUnlockDriverRgn(prgnSrc2);
    RGNOBJ_vUnlockDriverRgn(prgnSrc1);
    RGNOBJ_vUnlockDriverRgn(prgnTrg);

    return iResult;
}

ENGAPI
INT
APIENTRY
EngCopyRgn(
    _In_ HANDLE hrgnDst,
    _In_ HANDLE hrgnSrc)
{
    return EngCombineRgn(hrgnDst, hrgnSrc, NULL, RGN_COPY);
}

ENGAPI
INT
APIENTRY
EngIntersectRgn(
    _In_ HANDLE hrgnResult,
    _In_ HANDLE hRgnA,
    _In_ HANDLE hRgnB)
{
    return EngCombineRgn(hrgnResult, hRgnA, hRgnB, RGN_AND);
}

ENGAPI
INT
APIENTRY
EngSubtractRgn(
    _In_ HANDLE hrgnResult,
    _In_ HANDLE hRgnA,
    _In_ HANDLE hRgnB)
{
    return EngCombineRgn(hrgnResult, hRgnA, hRgnB, RGN_DIFF);
}

ENGAPI
INT
APIENTRY
EngUnionRgn(
    _In_ HANDLE hrgnResult,
    _In_ HANDLE hRgnA,
    _In_ HANDLE hRgnB)
{
    return EngCombineRgn(hrgnResult, hRgnA, hRgnB, RGN_OR);
}

ENGAPI
INT
APIENTRY
EngXorRgn(
    _In_ HANDLE hrgnResult,
    _In_ HANDLE hRgnA,
    _In_ HANDLE hRgnB)
{
    return EngCombineRgn(hrgnResult, hRgnA, hRgnB, RGN_XOR);
}

/**
 * @brief The region's bounding box.
 * @return NULLREGION / SIMPLEREGION / COMPLEXREGION, or ERROR.
 */
ENGAPI
INT
APIENTRY
EngGetRgnBox(
    _In_ HANDLE hrgn,
    _Out_ LPRECT prcl)
{
    PREGION prgn;
    INT     iComplexity;

    if (prcl == NULL)
        return ERROR;

    prgn = RGNOBJ_pLockDriverRgn(hrgn);
    if (prgn == NULL)
    {
        EngSetLastError(ERROR_INVALID_HANDLE);
        return ERROR;
    }

    iComplexity = REGION_GetRgnBox(prgn, (RECTL *)prcl);
    RGNOBJ_vUnlockDriverRgn(prgn);

    return iComplexity;
}

ENGAPI
INT
APIENTRY
EngOffsetRgn(
    _In_ HANDLE hrgn,
    _In_ INT x,
    _In_ INT y)
{
    PREGION prgn;
    INT     iComplexity;

    prgn = RGNOBJ_pLockDriverRgn(hrgn);
    if (prgn == NULL)
    {
        EngSetLastError(ERROR_INVALID_HANDLE);
        return ERROR;
    }

    if (!REGION_bOffsetRgn(prgn, x, y))
    {
        RGNOBJ_vUnlockDriverRgn(prgn);
        return ERROR;
    }

    iComplexity = REGION_Complexity(prgn);
    RGNOBJ_vUnlockDriverRgn(prgn);

    return iComplexity;
}

/** @brief Does any part of @p prcl fall inside the region? */
ENGAPI
BOOL
APIENTRY
EngRectInRgn(
    _In_ HANDLE hrgn,
    _In_ LPRECT prcl)
{
    PREGION prgn;
    BOOL    bResult;

    if (prcl == NULL)
        return FALSE;

    prgn = RGNOBJ_pLockDriverRgn(hrgn);
    if (prgn == NULL)
    {
        EngSetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }

    bResult = REGION_RectInRegion(prgn, (const RECTL *)prcl);
    RGNOBJ_vUnlockDriverRgn(prgn);

    return bResult;
}

ENGAPI
BOOL
APIENTRY
EngEqualRgn(
    _In_ HANDLE hrgn1,
    _In_ HANDLE hrgn2)
{
    PREGION prgn1, prgn2;
    BOOL    bEqual = FALSE;
    ULONG   i;

    prgn1 = RGNOBJ_pLockDriverRgn(hrgn1);
    if (prgn1 == NULL)
    {
        EngSetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }

    prgn2 = RGNOBJ_pLockDriverRgn(hrgn2);
    if (prgn2 == NULL)
    {
        RGNOBJ_vUnlockDriverRgn(prgn1);
        EngSetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }

    /* Regions are kept in a canonical form, so equality is a straight rectangle-list compare. */
    if (prgn1->rdh.nCount == prgn2->rdh.nCount)
    {
        bEqual = TRUE;
        for (i = 0; i < prgn1->rdh.nCount; i++)
        {
            if (!RtlEqualMemory(&prgn1->Buffer[i], &prgn2->Buffer[i], sizeof(RECTL)))
            {
                bEqual = FALSE;
                break;
            }
        }
    }

    RGNOBJ_vUnlockDriverRgn(prgn2);
    RGNOBJ_vUnlockDriverRgn(prgn1);

    return bEqual;
}

/**
 * @brief Serialise a region into an RGNDATA rectangle list.
 *
 * With @p lpRgnData NULL this returns the number of bytes required, which is how a caller sizes
 * its buffer before the real call (Reference cdd.c:21402 does exactly that).
 *
 * @return Bytes required (or written), 0 on failure.
 *
 * @note Unlike NtGdiGetRegionData this does NOT probe the output buffer: the caller is a kernel
 *       driver and the buffer is kernel memory.
 */
ENGAPI
DWORD
APIENTRY
EngGetRgnData(
    _In_ HANDLE hrgn,
    _In_ DWORD nCount,
    _Out_ LPRGNDATA lpRgnData)
{
    PREGION prgn;
    ULONG   cjRects, cjSize;

    prgn = RGNOBJ_pLockDriverRgn(hrgn);
    if (prgn == NULL)
    {
        EngSetLastError(ERROR_INVALID_HANDLE);
        return 0;
    }

    cjRects = prgn->rdh.nCount * sizeof(RECT);
    cjSize = cjRects + sizeof(RGNDATAHEADER);

    if (lpRgnData != NULL)
    {
        if (nCount >= cjSize)
        {
            RtlCopyMemory(lpRgnData, &prgn->rdh, sizeof(RGNDATAHEADER));
            RtlCopyMemory(lpRgnData->Buffer, prgn->Buffer, cjRects);
            lpRgnData->rdh.iType = RDH_RECTANGLES;
            lpRgnData->rdh.nRgnSize = cjRects;
        }
        else
        {
            /*
             * Leave a valid empty region behind rather than the caller's uninitialized buffer.
             * A driver that ignores the return value would otherwise read a rectangle count and
             * rectangles out of uninitialized pool, and the CDD is such a caller: it allocates
             * this buffer unzeroed and takes the count straight out of it (Reference cdd.c
             * CddPresentBlt:21503). A region that grew between the sizing call and this one is
             * the case that reaches here, and dropping the frame is the right answer to that.
             */
            if (nCount >= sizeof(RGNDATAHEADER))
            {
                RtlCopyMemory(lpRgnData, &prgn->rdh, sizeof(RGNDATAHEADER));
                lpRgnData->rdh.iType = RDH_RECTANGLES;
                lpRgnData->rdh.nCount = 0;
                lpRgnData->rdh.nRgnSize = 0;
            }

            DPRINT1("EngGetRgnData: buffer %lu too small for %lu, reporting no rectangles\n",
                    nCount, cjSize);
            EngSetLastError(ERROR_INVALID_PARAMETER);
            cjSize = 0;
        }
    }

    RGNOBJ_vUnlockDriverRgn(prgn);
    return cjSize;
}

/* EOF */
