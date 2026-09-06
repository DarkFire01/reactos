/*
 * PROJECT:         Win32 subsystem
 * LICENSE:         See COPYING in the top level directory
 * FILE:            win32ss/gdi/dib/dib32bppc.c
 * PURPOSE:         C language equivalents of asm optimised 32bpp functions
 * PROGRAMMERS:     Jason Filby
 *                  Magnus Olsen
 */

#include <win32k.h>

#define NDEBUG
#include <debug.h>

VOID
DIB_32BPP_HLine(SURFOBJ *SurfObj, LONG x1, LONG x2, LONG y, ULONG c)
{
  PBYTE byteaddr = (PBYTE)((ULONG_PTR)SurfObj->pvScan0 + y * SurfObj->lDelta);
  PDWORD addr = (PDWORD)byteaddr + x1;
  LONG cx = x1;

  while(cx < x2)
  {
    *addr = (DWORD)c;
    ++addr;
    ++cx;
  }
}

BOOLEAN
DIB_32BPP_ColorFill(SURFOBJ* DestSurface, RECTL* DestRect, ULONG color)
{
  LONG DestY, Width;
  PBYTE FirstRow, Row;
  PULONG Pixel;
  LONG i;

  /* Make WellOrdered by making top < bottom and left < right */
  RECTL_vMakeWellOrdered(DestRect);

  Width = DestRect->right - DestRect->left;
  if (Width <= 0 || DestRect->bottom <= DestRect->top)
    return TRUE;

  /*
   * Fill the first row a pixel at a time, then copy that row over the rest.
   *
   * A store loop moves one pixel per iteration, which is all a compiler can
   * make of it without a vector unit to target - and the i386 build is built
   * for plain Pentium, so it has none to target. memcpy has no such problem:
   * every toolchain lowers it to the widest move the target has, and the row
   * being copied from stays in L1 for the whole fill, so the copies run at
   * better than main-memory speed.
   *
   * This is the path every solid fill in the system takes - BltPatCopy() hands
   * a solid PATCOPY straight to DIB_ColorFill, so it is also every window
   * background, control face and erase-background.
   */
  FirstRow = (PBYTE)DestSurface->pvScan0 +
             DestRect->top * DestSurface->lDelta +
             DestRect->left * 4;

  Pixel = (PULONG)FirstRow;
  for (i = 0; i < Width; i++)
    Pixel[i] = color;

  Row = FirstRow;
  for (DestY = DestRect->top + 1; DestY < DestRect->bottom; DestY++)
  {
    Row += DestSurface->lDelta;
    RtlCopyMemory(Row, FirstRow, Width * 4);
  }

  return TRUE;
}
