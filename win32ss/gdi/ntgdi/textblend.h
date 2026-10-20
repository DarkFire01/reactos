/*
 * PROJECT:     ReactOS win32 kernel mode subsystem
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Blending glyph coverage straight into DIB surfaces
 */

#pragma once

/* A surface glyphs are blended into and the text color in it */
typedef struct _GLYPH_TARGET
{
    PBYTE pjBits;
    LONG lDelta;
    SIZEL sizl;
    ULONG cjPixel;          /* 3 or 4 */
    ULONG aulShift[3];      /* bit of red, green and blue in a pixel */
    BYTE ajColor[3];        /* red, green and blue of the text */
    BOOLEAN bBgrSubpixels;  /* the panel's subpixels run blue to red */
} GLYPH_TARGET, *PGLYPH_TARGET;

/* Whether glyphs can be blended into pso with the solid color of pbo; fills pTarget when they can */
BOOLEAN
NTAPI
GlyphBlendPrepare(
    _In_ SURFOBJ *pso,
    _In_ BRUSHOBJ *pbo,
    _Out_ PGLYPH_TARGET pTarget);

/*
 * Blends coverage over prclDest, inside the clip. Coverage starts at prclDest's top left and has
 * one byte a pixel, or one a subpixel when bSubpixel is set.
 */
VOID
NTAPI
GlyphBlendDraw(
    _In_ const GLYPH_TARGET *pTarget,
    _In_opt_ CLIPOBJ *pco,
    _In_ const RECTL *prclDest,
    _In_ const BYTE *pjCoverage,
    _In_ LONG lCoveragePitch,
    _In_ BOOLEAN bSubpixel);
