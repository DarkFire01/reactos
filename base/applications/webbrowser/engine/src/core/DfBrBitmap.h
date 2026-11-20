/** @file
  DfBrBitmap.h - a platform-agnostic 32bpp pixel buffer.

  The channel order is BGRA, chosen to match:
    - Win32 GDI DIB                       (BGRA / 0xAARRGGBB little-endian)
    - Direct3D 9 D3DFMT_A8R8G8B8          (identical layout on LE)
    - UEFI EFI_GRAPHICS_OUTPUT_BLT_PIXEL  { Blue, Green, Red, Reserved }

  Picking BGRA means zero per-pixel swizzle on every currently-planned
  render target.
**/

#ifndef DFBR_BITMAP_H_
#define DFBR_BITMAP_H_

#include "../common/DfBrTypes.h"

typedef struct {
  u8  B;
  u8  G;
  u8  R;
  u8  A;
} DfBrColor;

typedef struct {
  u32         Width;
  u32         Height;
  DfBrColor  *Pixels;       ///< row-major, Width*Height, no row padding.
  b32         OwnsPixels;   ///< if DFBR_TRUE, DfBrBitmap teardown free()s Pixels.
} DfBrBitmap;

static inline DfBrColor DfBrRgba (u8 R, u8 G, u8 B, u8 A) {
  DfBrColor C;
  C.R = R; C.G = G; C.B = B; C.A = A;
  return C;
}

static inline DfBrColor DfBrRgb (u8 R, u8 G, u8 B) {
  return DfBrRgba (R, G, B, 0xFF);
}

#endif /* DFBR_BITMAP_H_ */
