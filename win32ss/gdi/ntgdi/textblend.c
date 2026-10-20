/*
 * PROJECT:     ReactOS win32 kernel mode subsystem
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Blending glyph coverage straight into DIB surfaces
 */

#include <win32k.h>
#include <math.h>
#include "textblend.h"

#define NDEBUG
#include <debug.h>

/* Channel values in linear light have this many steps */
#define GLYPH_LINEAR_STEPS 4096

/* Light to channel value and back for the font smoothing contrast, which is a gamma times 1000 */
static UINT gGlyphContrast = 0;
static USHORT gausToLinear[256];
static BYTE gajFromLinear[GLYPH_LINEAR_STEPS];

static
VOID
GlyphBlendUpdateGamma(VOID)
{
    KFLOATING_SAVE FloatSave;
    UINT Contrast = gspv.uiFontSmoothingContrast;
    double Gamma;
    ULONG i;

    if (Contrast < 1000 || Contrast > 2200)
        Contrast = 1400;
    if (Contrast == gGlyphContrast)
        return;

    if (!NT_SUCCESS(KeSaveFloatingPointState(&FloatSave)))
        return;

    Gamma = Contrast / 1000.0;
    for (i = 0; i < 256; i++)
        gausToLinear[i] = (USHORT)(pow(i / 255.0, Gamma) * (GLYPH_LINEAR_STEPS - 1) + 0.5);
    for (i = 0; i < GLYPH_LINEAR_STEPS; i++)
        gajFromLinear[i] = (BYTE)(pow(i / (double)(GLYPH_LINEAR_STEPS - 1), 1.0 / Gamma) * 255.0 + 0.5);

    KeRestoreFloatingPointState(&FloatSave);
    gGlyphContrast = Contrast;
}

/* The bit a channel mask starts at, when it is one whole byte */
static
BOOLEAN
GlyphBlendByteShift(
    _In_ ULONG ulMask,
    _Out_ PULONG pulShift)
{
    ULONG Shift;

    for (Shift = 0; Shift <= 24; Shift += 8)
    {
        if (ulMask == (0xFFul << Shift))
        {
            *pulShift = Shift;
            return TRUE;
        }
    }
    return FALSE;
}

BOOLEAN
NTAPI
GlyphBlendPrepare(
    _In_ SURFOBJ *pso,
    _In_ BRUSHOBJ *pbo,
    _Out_ PGLYPH_TARGET pTarget)
{
    PSURFACE psurf = CONTAINING_RECORD(pso, SURFACE, SurfObj);
    PPALETTE ppal = psurf->ppal;
    ULONG i;

    RtlZeroMemory(pTarget, sizeof(*pTarget));

    if (pso->iType != STYPE_BITMAP || !pso->pvScan0 || pbo->iSolidColor == 0xFFFFFFFF || !ppal)
        return FALSE;
    if (pso->iBitmapFormat == BMF_32BPP)
        pTarget->cjPixel = 4;
    else if (pso->iBitmapFormat == BMF_24BPP)
        pTarget->cjPixel = 3;
    else
        return FALSE;

    if (!GlyphBlendByteShift(ppal->RedMask, &pTarget->aulShift[0]) ||
        !GlyphBlendByteShift(ppal->GreenMask, &pTarget->aulShift[1]) ||
        !GlyphBlendByteShift(ppal->BlueMask, &pTarget->aulShift[2]))
        return FALSE;

    for (i = 0; i < 3; i++)
    {
        if (pTarget->aulShift[i] >= pTarget->cjPixel * 8)
            return FALSE;
        pTarget->ajColor[i] = (BYTE)(pbo->iSolidColor >> pTarget->aulShift[i]);
    }

    pTarget->pjBits = pso->pvScan0;
    pTarget->lDelta = pso->lDelta;
    pTarget->sizl = pso->sizlBitmap;
    pTarget->bBgrSubpixels = (gspv.uiFontSmoothingOrientation == FE_FONTSMOOTHINGORIENTATIONBGR);

    GlyphBlendUpdateGamma();
    return gGlyphContrast != 0;
}

/* One channel moved toward the text color by Coverage of the way, in linear light */
FORCEINLINE
BYTE
GlyphBlendChannel(
    _In_ BYTE jDest,
    _In_ BYTE jColor,
    _In_ ULONG Coverage)
{
    LONG Dest = gausToLinear[jDest];
    LONG Color = gausToLinear[jColor];

    return gajFromLinear[Dest + (Color - Dest) * (LONG)Coverage / 255];
}

static
VOID
GlyphBlendRect(
    _In_ const GLYPH_TARGET *pTarget,
    _In_ const RECTL *prcl,
    _In_ const BYTE *pjCoverage,
    _In_ LONG lCoveragePitch,
    _In_ BOOLEAN bSubpixel)
{
    const ULONG cjPixel = pTarget->cjPixel;
    const ULONG cjCoverage = bSubpixel ? 3 : 1;
    const ULONG RedSubpixel = pTarget->bBgrSubpixels ? 2 : 0;
    ULONG Solid = 0;
    LONG x, y;
    ULONG i;

    for (i = 0; i < 3; i++)
        Solid |= (ULONG)pTarget->ajColor[i] << pTarget->aulShift[i];

    for (y = prcl->top; y < prcl->bottom; y++)
    {
        PBYTE pjPixel = pTarget->pjBits + y * pTarget->lDelta + prcl->left * cjPixel;
        const BYTE *pjCover = pjCoverage;

        for (x = prcl->left; x < prcl->right; x++, pjPixel += cjPixel, pjCover += cjCoverage)
        {
            ULONG Pixel;
            ULONG Coverage[3];

            if (bSubpixel)
            {
                Coverage[0] = pjCover[RedSubpixel];
                Coverage[1] = pjCover[1];
                Coverage[2] = pjCover[2 - RedSubpixel];
            }
            else
            {
                Coverage[0] = Coverage[1] = Coverage[2] = pjCover[0];
            }

            if (!(Coverage[0] | Coverage[1] | Coverage[2]))
                continue;

            Pixel = pjPixel[0] | ((ULONG)pjPixel[1] << 8) | ((ULONG)pjPixel[2] << 16);
            if (cjPixel == 4)
                Pixel |= (ULONG)pjPixel[3] << 24;

            if ((Coverage[0] & Coverage[1] & Coverage[2]) == 255)
            {
                for (i = 0; i < 3; i++)
                    Pixel &= ~(0xFFul << pTarget->aulShift[i]);
                Pixel |= Solid;
            }
            else
            {
                for (i = 0; i < 3; i++)
                {
                    BYTE Channel = GlyphBlendChannel((BYTE)(Pixel >> pTarget->aulShift[i]),
                                                     pTarget->ajColor[i], Coverage[i]);
                    Pixel = (Pixel & ~(0xFFul << pTarget->aulShift[i])) | ((ULONG)Channel << pTarget->aulShift[i]);
                }
            }

            pjPixel[0] = (BYTE)Pixel;
            pjPixel[1] = (BYTE)(Pixel >> 8);
            pjPixel[2] = (BYTE)(Pixel >> 16);
            if (cjPixel == 4)
                pjPixel[3] = (BYTE)(Pixel >> 24);
        }
        pjCoverage += lCoveragePitch;
    }
}

VOID
NTAPI
GlyphBlendDraw(
    _In_ const GLYPH_TARGET *pTarget,
    _In_opt_ CLIPOBJ *pco,
    _In_ const RECTL *prclDest,
    _In_ const BYTE *pjCoverage,
    _In_ LONG lCoveragePitch,
    _In_ BOOLEAN bSubpixel)
{
    const ULONG cjCoverage = bSubpixel ? 3 : 1;
    const RECTL rclSurface = { 0, 0, pTarget->sizl.cx, pTarget->sizl.cy };
    RECTL rclBounds;
    RECTL rclPart;
    struct
    {
        ULONG c;
        RECTL arcl[8];
    } Enum;
    BOOL bMore;
    ULONG i;

    if (!RECTL_bIntersectRect(&rclBounds, prclDest, &rclSurface))
        return;

#define GLYPH_BLEND_PART(prcl) \
    GlyphBlendRect(pTarget, (prcl), \
                   pjCoverage + ((prcl)->top - prclDest->top) * lCoveragePitch + \
                   ((prcl)->left - prclDest->left) * cjCoverage, \
                   lCoveragePitch, bSubpixel)

    if (!pco || pco->iDComplexity == DC_TRIVIAL)
    {
        GLYPH_BLEND_PART(&rclBounds);
        return;
    }

    if (pco->iDComplexity == DC_RECT)
    {
        if (RECTL_bIntersectRect(&rclPart, &rclBounds, &pco->rclBounds))
            GLYPH_BLEND_PART(&rclPart);
        return;
    }

    if (!RECTL_bIntersectRect(&rclBounds, &rclBounds, &pco->rclBounds))
        return;

    CLIPOBJ_cEnumStart(pco, FALSE, CT_RECTANGLES, CD_ANY, 0);
    do
    {
        bMore = CLIPOBJ_bEnum(pco, sizeof(Enum), (PULONG)&Enum);
        for (i = 0; i < Enum.c; i++)
        {
            if (RECTL_bIntersectRect(&rclPart, &rclBounds, &Enum.arcl[i]))
                GLYPH_BLEND_PART(&rclPart);
        }
    } while (bMore);

#undef GLYPH_BLEND_PART
}
