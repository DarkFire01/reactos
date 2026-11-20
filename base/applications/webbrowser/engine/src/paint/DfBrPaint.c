/** @file
  DfBrPaint.c - software BGRA rasterizer for the DfBrLayout box tree.

  Paint order (CSS 2.1 + HTML painter's algorithm, simplified):

    1. Clear the canvas (unless the caller opts out).
    2. Pre-order walk of the box tree. At each box:
       2a. Background fill (solid color).
       2b. Border fill (solid style, four sides).
    3. Post-order walk - or, equivalently, second pre-order - for the
       foreground (text + replaced placeholders). We fold this into
       the same pass by emitting text + replaced *after* descending
       into the box's children are done with their backgrounds.

  v1 chooses the simpler "two passes" approach to keep things
  obvious: first pass paints backgrounds + borders for the entire
  tree, second pass paints text + replaced placeholders. This is
  correct for normal flow without stacking contexts and matches
  what browsers used to do before the spec introduced stacking
  contexts.

  The module only touches:
    * DfBrBitmap  (core)
    * DfBrLayout* (geometry, traversal, text fragments)
    * DfBrFont*   (text rasterization)

  It does not know about the platform backend, DirectX, or any
  specific window system; the bitmap is the hand-off point.
**/

#include "DfBrPaint.h"

#include "../core/DfBrSvg.h"
#include "../font/DfBrFont.h"
#include "../html/DfBrHtml.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------
 *  Forward decls (a couple of helpers used across paint phases)
 * -----------------------------------------------------------------*/

static DfBrFontStyle MapFontStyle (DfBrCssFontStyle S);

/* ------------------------------------------------------------------
 *  Defaults + helpers
 * -----------------------------------------------------------------*/

DfBrPaintOptions
DfBrPaintDefaultOptions (void) {
  DfBrPaintOptions O;
  memset (&O, 0, sizeof (O));
  O.CanvasColor          = DfBrRgb (255, 255, 255);
  O.ClearCanvas          = DFBR_TRUE;
  O.ReplacedPlaceholders = DFBR_TRUE;
  return O;
}

/* DfBrCssColor stores alpha as f32 in [0, 1]; DfBrColor is 8bpp
 * (alpha 0..255). Clamp-and-round on the way through. */
static DfBrColor
CssColorToDfBrColor (DfBrCssColor C) {
  DfBrColor D;
  f32       A = C.A;
  if (A < 0.0f) A = 0.0f;
  if (A > 1.0f) A = 1.0f;
  D.R = C.R;
  D.G = C.G;
  D.B = C.B;
  D.A = (u8)(A * 255.0f + 0.5f);
  return D;
}

static b32
ColorVisible (DfBrCssColor C) {
  return (b32)(C.A > 0.0f);
}

/* Source-over alpha blend of one solid pixel onto a DfBrColor cell. */
static void
BlendPixel (DfBrColor *Dst, DfBrColor Src) {
  u32 A = Src.A;
  if (A == 0) return;
  if (A == 255) { *Dst = Src; return; }
  u32 Ia = 255u - A;
  Dst->R = (u8)((A * Src.R + Ia * Dst->R + 127u) / 255u);
  Dst->G = (u8)((A * Src.G + Ia * Dst->G + 127u) / 255u);
  Dst->B = (u8)((A * Src.B + Ia * Dst->B + 127u) / 255u);
  if (A > Dst->A) Dst->A = (u8)A;
}

/* Fill a solid axis-aligned rect, clipped to the target. Coordinates
 * are in CSS pixels; the raster snaps to integer pixels via +0.5. */
static void
FillRect (DfBrBitmap *Bmp,
          f32 X, f32 Y, f32 W, f32 H,
          DfBrColor Color)
{
  if (Bmp == DFBR_NULL || Bmp->Pixels == DFBR_NULL) return;
  if (W <= 0.0f || H <= 0.0f || Color.A == 0)       return;

  i32 Tw = (i32)Bmp->Width;
  i32 Th = (i32)Bmp->Height;
  i32 X0 = (i32)(X + 0.5f);
  i32 Y0 = (i32)(Y + 0.5f);
  i32 X1 = (i32)(X + W + 0.5f);
  i32 Y1 = (i32)(Y + H + 0.5f);
  if (X0 < 0)  X0 = 0;
  if (Y0 < 0)  Y0 = 0;
  if (X1 > Tw) X1 = Tw;
  if (Y1 > Th) Y1 = Th;
  if (X0 >= X1 || Y0 >= Y1) return;

  for (i32 Py = Y0; Py < Y1; Py++) {
    DfBrColor *Row = Bmp->Pixels + (usize)Py * Bmp->Width;
    for (i32 Px = X0; Px < X1; Px++) {
      BlendPixel (&Row[Px], Color);
    }
  }
}

/* Opaque clear of the whole bitmap. */
static void
ClearAll (DfBrBitmap *Bmp, DfBrColor C) {
  usize I;
  usize N;
  if (Bmp == DFBR_NULL || Bmp->Pixels == DFBR_NULL) return;
  N = (usize)Bmp->Width * (usize)Bmp->Height;
  for (I = 0; I < N; I++) Bmp->Pixels[I] = C;
}

/* Resolve a CSS <length> to pixels for box-shadow offset / blur /
 * spread. Percentages are invalid on box-shadow; treated as 0. */
static f32
ResolveShadowLenPx (const DfBrCssLength *L, f32 FontSizePx)
{
  f32 V;
  if (L == DFBR_NULL || L->Auto) return 0.0f;
  V = L->Value;
  switch (L->Unit) {
    case DFBR_CSS_UNIT_EM:      V = V * FontSizePx; break;
    case DFBR_CSS_UNIT_REM:     V = V * 16.0f;      break;
    case DFBR_CSS_UNIT_PERCENT: V = 0.0f;           break;
    default: /* px and unitless */ break;
  }
  return V;
}

/* ------------------------------------------------------------------
 *  Backgrounds
 * -----------------------------------------------------------------*/

/* The paint rect of a box includes its padding (it extends out to
 * the inner edge of the border). Border + margin live outside. */
static void
BoxPaddingRect (const DfBrLayoutBox *Box,
                f32 *OutX, f32 *OutY, f32 *OutW, f32 *OutH)
{
  *OutX = Box->ContentRect.X - Box->Padding.Left;
  *OutY = Box->ContentRect.Y - Box->Padding.Top;
  *OutW = Box->ContentRect.W + Box->Padding.Left + Box->Padding.Right;
  *OutH = Box->ContentRect.H + Box->Padding.Top  + Box->Padding.Bottom;
}

/* ------------------------------------------------------------------
 *  border-radius resolution
 *
 *  CSS Backgrounds §5.3 says each radius resolves against:
 *    - horizontal component -> border-box width
 *    - vertical   component -> border-box height
 *  and the four corners are then proportionally clamped so their
 *  sum on any side never exceeds the side length.
 *
 *  v1 doesn't support elliptical radii (the horizontal+vertical pair
 *  form a circle of the specified radius). We keep the simpler
 *  single-value API and reduce the clamp to the tightest side.
 * -----------------------------------------------------------------*/

static f32
ResolveRadiusPx (const DfBrCssLength *L, f32 RefW, f32 RefH, f32 FontSizePx)
{
  f32 V;
  f32 Base = (RefW < RefH) ? RefW : RefH;
  if (L == DFBR_NULL || L->Auto) return 0.0f;
  V = L->Value;
  switch (L->Unit) {
    case DFBR_CSS_UNIT_PERCENT: V = V * 0.01f * Base; break;
    case DFBR_CSS_UNIT_EM:      V = V * FontSizePx;   break;
    case DFBR_CSS_UNIT_REM:     V = V * 16.0f;        break;
    default: /* px, unit-less, any other unit the parser kept as px. */ break;
  }
  if (V < 0.0f) V = 0.0f;
  return V;
}

static void
ResolveCornerRadii (const DfBrLayoutBox *Box, f32 W, f32 H,
                    f32 *Tl, f32 *Tr, f32 *Br, f32 *Bl)
{
  f32 Fs = Box->FontSizePx;
  f32 Scale;
  f32 Max = (W < H ? W : H) * 0.5f;
  const DfBrCssComputedStyle *S = Box->Style;
  *Tl = *Tr = *Br = *Bl = 0.0f;
  if (S == DFBR_NULL) return;
  *Tl = ResolveRadiusPx (&S->BorderTopLeftRadius,     W, H, Fs);
  *Tr = ResolveRadiusPx (&S->BorderTopRightRadius,    W, H, Fs);
  *Br = ResolveRadiusPx (&S->BorderBottomRightRadius, W, H, Fs);
  *Bl = ResolveRadiusPx (&S->BorderBottomLeftRadius,  W, H, Fs);
  /* Per spec: scale all radii by the most aggressive side-overflow
   * ratio so no pair on any side sums beyond the side length. */
  Scale = 1.0f;
  if (*Tl + *Tr > W && (*Tl + *Tr) > 0.0f) {
    f32 S1 = W / (*Tl + *Tr); if (S1 < Scale) Scale = S1;
  }
  if (*Bl + *Br > W && (*Bl + *Br) > 0.0f) {
    f32 S1 = W / (*Bl + *Br); if (S1 < Scale) Scale = S1;
  }
  if (*Tl + *Bl > H && (*Tl + *Bl) > 0.0f) {
    f32 S1 = H / (*Tl + *Bl); if (S1 < Scale) Scale = S1;
  }
  if (*Tr + *Br > H && (*Tr + *Br) > 0.0f) {
    f32 S1 = H / (*Tr + *Br); if (S1 < Scale) Scale = S1;
  }
  if (Scale < 1.0f) { *Tl *= Scale; *Tr *= Scale; *Br *= Scale; *Bl *= Scale; }
  if (*Tl > Max) *Tl = Max;
  if (*Tr > Max) *Tr = Max;
  if (*Br > Max) *Br = Max;
  if (*Bl > Max) *Bl = Max;
}

/* Per-pixel coverage (0..1) for membership inside a rounded rect
 * whose outer border lives at [X, X+W] x [Y, Y+H] with corner radii
 * (Tl, Tr, Br, Bl). The test is run at the pixel's center. We shift
 * the corner circle centers in by the radius, then for the four
 * outside-corner regions we compute a signed distance and convert
 * that to a half-pixel anti-aliased coverage. Straight edges get
 * the same half-pixel AA, which is what gives small boxes a clean
 * 1px fade rather than a jagged step. */
static f32
RoundRectCoverage (f32 Px, f32 Py,
                   f32 X, f32 Y, f32 W, f32 H,
                   f32 Tl, f32 Tr, f32 Br, f32 Bl)
{
  f32 Dx = 0.0f, Dy = 0.0f, R = 0.0f;
  f32 Left = X, Right = X + W, Top = Y, Bottom = Y + H;
  f32 Inside;
  if (Px < Left - 0.5f || Px > Right + 0.5f || Py < Top - 0.5f || Py > Bottom + 0.5f) {
    return 0.0f;
  }
  if (Px < Left + Tl && Py < Top + Tl) {
    Dx = (Left + Tl) - Px; Dy = (Top + Tl) - Py; R = Tl;
  } else if (Px > Right - Tr && Py < Top + Tr) {
    Dx = Px - (Right - Tr); Dy = (Top + Tr) - Py; R = Tr;
  } else if (Px > Right - Br && Py > Bottom - Br) {
    Dx = Px - (Right - Br); Dy = Py - (Bottom - Br); R = Br;
  } else if (Px < Left + Bl && Py > Bottom - Bl) {
    Dx = (Left + Bl) - Px; Dy = Py - (Bottom - Bl); R = Bl;
  } else {
    /* Straight-edge region: AA against the axis-aligned side. */
    f32 EdgeCov = 1.0f;
    if (Px < Left + 0.5f)  EdgeCov = Px - (Left - 0.5f);
    if (Px > Right - 0.5f) { f32 E = (Right + 0.5f) - Px; if (E < EdgeCov) EdgeCov = E; }
    if (Py < Top + 0.5f)   { f32 E = Py - (Top - 0.5f);   if (E < EdgeCov) EdgeCov = E; }
    if (Py > Bottom - 0.5f){ f32 E = (Bottom + 0.5f) - Py;if (E < EdgeCov) EdgeCov = E; }
    if (EdgeCov < 0.0f) EdgeCov = 0.0f;
    if (EdgeCov > 1.0f) EdgeCov = 1.0f;
    return EdgeCov;
  }
  /* Inside one of the four corner fillets. Signed distance from the
   * fillet center, positive = outside circle. The half-pixel AA
   * band gives a clean edge at 1px resolution. */
  {
    /* Hypotenuse via float math. We avoid sqrt for speed by testing
     * squared distances against (R +/- 0.5)^2 first. */
    f32 D2 = Dx * Dx + Dy * Dy;
    if (D2 <= (R - 0.5f) * (R - 0.5f) || R <= 0.5f) return 1.0f;
    if (D2 >= (R + 0.5f) * (R + 0.5f)) return 0.0f;
    /* In the AA band; one sqrt. */
    {
      f32 D = D2; f32 Q = D; f32 Est = R;
      /* Newton two iterations - plenty for 1px precision. */
      Est = 0.5f * (Est + Q / Est);
      Est = 0.5f * (Est + Q / Est);
      Inside = (R + 0.5f) - Est;
      if (Inside < 0.0f) Inside = 0.0f;
      if (Inside > 1.0f) Inside = 1.0f;
      return Inside;
    }
  }
}

static void
FillRectRounded (DfBrBitmap *Bmp, f32 X, f32 Y, f32 W, f32 H,
                 f32 Tl, f32 Tr, f32 Br, f32 Bl, DfBrColor C)
{
  i32 Tw = (i32)Bmp->Width, Th = (i32)Bmp->Height;
  i32 X0, Y0, X1, Y1, Px, Py;
  if (Tl <= 0.0f && Tr <= 0.0f && Br <= 0.0f && Bl <= 0.0f) {
    FillRect (Bmp, X, Y, W, H, C);
    return;
  }
  if (W <= 0.0f || H <= 0.0f) return;
  X0 = (i32)(X - 0.5f);
  Y0 = (i32)(Y - 0.5f);
  X1 = (i32)(X + W + 0.5f) + 1;
  Y1 = (i32)(Y + H + 0.5f) + 1;
  if (X0 < 0) X0 = 0;
  if (Y0 < 0) Y0 = 0;
  if (X1 > Tw) X1 = Tw;
  if (Y1 > Th) Y1 = Th;
  /* Effective source alpha = coverage * src alpha. This matters for
   * translucent fills (e.g. `color-mix(in srgb, accent 8%, transparent)`
   * used for tinted pill backgrounds), where ignoring `C.A` (a u8 in
   * 0..255) would stamp the source color opaquely and erase the visible
   * dark backdrop. */
  for (Py = Y0; Py < Y1; Py++) {
    DfBrColor *Row = Bmp->Pixels + (usize)Py * Bmp->Width;
    for (Px = X0; Px < X1; Px++) {
      f32 Cov = RoundRectCoverage ((f32)Px + 0.5f, (f32)Py + 0.5f,
                                   X, Y, W, H, Tl, Tr, Br, Bl);
      u32 A, Ia;
      DfBrColor Dst;
      if (Cov <= 0.0f) continue;
      A = (u32)(Cov * (f32)C.A + 0.5f);
      if (A == 0) continue;
      if (A >= 255) {
        Row[Px] = C;
        Row[Px].A = 0xFF;
        continue;
      }
      Dst = Row[Px];
      Ia = 255 - A;
      Row[Px].R = (u8)(((u32)C.R * A + (u32)Dst.R * Ia + 127) / 255);
      Row[Px].G = (u8)(((u32)C.G * A + (u32)Dst.G * Ia + 127) / 255);
      Row[Px].B = (u8)(((u32)C.B * A + (u32)Dst.B * Ia + 127) / 255);
      Row[Px].A = 0xFF;
    }
  }
}

/* Interpolate a color value between two gradient stops. Both Lo and
 * Hi are fully resolved (post-distribution) with positions in [0, 1].
 * T is the unit position along the gradient line. */
static DfBrColor
SampleGradient (const DfBrCssGradient *G,
                DfBrCssColor CurrentColor,
                f32 T)
{
  const DfBrCssColorStop *Lo;
  const DfBrCssColorStop *Hi;
  DfBrCssColor A, B;
  DfBrColor    Out;
  f32          Local;
  usize        I;

  if (G == DFBR_NULL || G->StopCount == 0) {
    Out.R = Out.G = Out.B = 0; Out.A = 0;
    return Out;
  }
  /* Repeating gradients wrap T into [0, 1). */
  if (G->Repeating) {
    T = T - (f32)((i32)T);
    if (T < 0.0f) T += 1.0f;
  }
  if (T <= G->Stops[0].Position) {
    A = G->Stops[0].Color;
    if (G->Stops[0].ColorIsCurrent) A = CurrentColor;
    Out = CssColorToDfBrColor (A);
    return Out;
  }
  if (T >= G->Stops[G->StopCount - 1].Position) {
    A = G->Stops[G->StopCount - 1].Color;
    if (G->Stops[G->StopCount - 1].ColorIsCurrent) A = CurrentColor;
    Out = CssColorToDfBrColor (A);
    return Out;
  }
  /* Linear scan; the stop list is sorted by position at parse time
   * and is rarely longer than 3-5 entries in real sites. */
  for (I = 1; I < G->StopCount; I++) {
    if (T <= G->Stops[I].Position) break;
  }
  Lo = &G->Stops[I - 1];
  Hi = &G->Stops[I];
  {
    f32 Denom = Hi->Position - Lo->Position;
    Local = (Denom > 0.0f) ? (T - Lo->Position) / Denom : 0.0f;
  }
  A = Lo->Color; if (Lo->ColorIsCurrent) A = CurrentColor;
  B = Hi->Color; if (Hi->ColorIsCurrent) B = CurrentColor;
  /* Interpolate in sRGB (not gamma-correct; matches how Ladybird's
   * v1 renderer and nearly every non-flagship engine ship today). */
  Out.R = (u8)((f32)A.R * (1.0f - Local) + (f32)B.R * Local + 0.5f);
  Out.G = (u8)((f32)A.G * (1.0f - Local) + (f32)B.G * Local + 0.5f);
  Out.B = (u8)((f32)A.B * (1.0f - Local) + (f32)B.B * Local + 0.5f);
  {
    f32 Af = A.A * (1.0f - Local) + B.A * Local;
    if (Af < 0.0f) Af = 0.0f; if (Af > 1.0f) Af = 1.0f;
    Out.A = (u8)(Af * 255.0f + 0.5f);
  }
  return Out;
}

/* Compute the "gradient line" parameters for a linear gradient
 * inside the rect [X, X+W] x [Y, Y+H]. Per CSS Images L3 §3.4, the
 * gradient line is centered in the box and sized so its endpoints
 * line up with the farthest box corners measured perpendicular to
 * the gradient line.
 *
 * Returns the unit direction (Dx, Dy) and the starting point
 * (X0, Y0) on the gradient line that corresponds to t=0, along
 * with the gradient line's length `Len`. T for any pixel Px, Py is
 * then `((Px - X0) * Dx + (Py - Y0) * Dy) / Len`. */
static void
GradientLineFromAngle (f32 AngleRad, f32 W, f32 H,
                       f32 *OutDx, f32 *OutDy, f32 *OutLen)
{
  /* CSS gradient angle: 0 = upwards, clockwise positive. */
  f32 Dx = (f32)sin ((f64)AngleRad);
  f32 Dy = -(f32)cos ((f64)AngleRad);
  f32 Ax = Dx < 0.0f ? -Dx : Dx;
  f32 Ay = Dy < 0.0f ? -Dy : Dy;
  f32 Len = Ax * W + Ay * H;
  *OutDx  = Dx;
  *OutDy  = Dy;
  *OutLen = (Len > 0.0f) ? Len : 1.0f;
}

/* Paint a linear gradient into the rect [X, X+W] x [Y, Y+H], clipped
 * to the supplied corner radii (same shape as the background fill).
 * CurrentColor resolves any `currentColor` stops. */
static void
FillGradientRounded (DfBrBitmap *Bmp,
                     f32 X, f32 Y, f32 W, f32 H,
                     f32 Tl, f32 Tr, f32 Br, f32 Bl,
                     const DfBrCssGradient *G,
                     DfBrCssColor CurrentColor)
{
  i32 Tw = (i32)Bmp->Width, Th = (i32)Bmp->Height;
  i32 X0, Y0, X1, Y1, Px, Py;
  f32 Dx, Dy, Len;
  f32 Cx, Cy;
  /* Start point on the gradient line (t=0). For a vector (Dx, Dy)
   * and a box of size W x H, t=0 lives at the box center shifted
   * by -0.5 * Len along the direction. */
  f32 Sx, Sy;

  if (G == DFBR_NULL || G->StopCount == 0) return;
  if (W <= 0.0f || H <= 0.0f)              return;

  GradientLineFromAngle (G->AngleRad, W, H, &Dx, &Dy, &Len);
  Cx = X + W * 0.5f;
  Cy = Y + H * 0.5f;
  Sx = Cx - Dx * (Len * 0.5f);
  Sy = Cy - Dy * (Len * 0.5f);

  X0 = (i32)(X - 0.5f);
  Y0 = (i32)(Y - 0.5f);
  X1 = (i32)(X + W + 0.5f) + 1;
  Y1 = (i32)(Y + H + 0.5f) + 1;
  if (X0 < 0) X0 = 0;
  if (Y0 < 0) Y0 = 0;
  if (X1 > Tw) X1 = Tw;
  if (Y1 > Th) Y1 = Th;

  for (Py = Y0; Py < Y1; Py++) {
    DfBrColor *Row = Bmp->Pixels + (usize)Py * Bmp->Width;
    f32 Fy = (f32)Py + 0.5f;
    for (Px = X0; Px < X1; Px++) {
      f32 Cov;
      f32 Fx = (f32)Px + 0.5f;
      f32 T  = ((Fx - Sx) * Dx + (Fy - Sy) * Dy) / Len;
      DfBrColor Src;
      Cov = (Tl > 0.0f || Tr > 0.0f || Br > 0.0f || Bl > 0.0f)
            ? RoundRectCoverage (Fx, Fy, X, Y, W, H, Tl, Tr, Br, Bl)
            : 1.0f;
      if (Cov <= 0.0f) continue;
      Src = SampleGradient (G, CurrentColor, T);
      if (Cov < 1.0f) {
        u32 A = (u32)(Src.A * Cov + 0.5f);
        Src.A = (u8)A;
      }
      BlendPixel (&Row[Px], Src);
    }
  }
}

/* Signed distance from a point (Px, Py) to a rounded rect whose
 * border-box is [X, X+W] x [Y, Y+H] with per-corner radii. Negative
 * inside the rounded rect, positive outside, ~0 on the edge. The
 * corner radius used is the one for the quadrant the point lies
 * in. This is a reasonable approximation when the four radii are
 * equal (Bootstrap/normalize patterns almost always are); when they
 * differ the quadrant split still gives a continuous SDF across
 * straight edges. */
static f32
RoundRectSdf (f32 Px, f32 Py,
              f32 X, f32 Y, f32 W, f32 H,
              f32 Tl, f32 Tr, f32 Br, f32 Bl)
{
  f32 Cx = X + W * 0.5f;
  f32 Cy = Y + H * 0.5f;
  f32 Rx = W * 0.5f;
  f32 Ry = H * 0.5f;
  f32 Rel = Px - Cx, Rey = Py - Cy;
  f32 R = 0.0f;
  f32 Dx, Dy;
  if (Rel <= 0.0f && Rey <= 0.0f) R = Tl;
  else if (Rel >= 0.0f && Rey <= 0.0f) R = Tr;
  else if (Rel >= 0.0f && Rey >= 0.0f) R = Br;
  else                                 R = Bl;
  if (R > Rx) R = Rx;
  if (R > Ry) R = Ry;
  if (R < 0.0f) R = 0.0f;
  {
    f32 Ax = Rel; if (Ax < 0.0f) Ax = -Ax;
    f32 Ay = Rey; if (Ay < 0.0f) Ay = -Ay;
    Dx = Ax - (Rx - R);
    Dy = Ay - (Ry - R);
  }
  if (Dx < 0.0f && Dy < 0.0f) {
    /* Inside the inner rect (between the corner fillets). Signed
     * distance equals the distance to the nearer edge. */
    return (Dx > Dy) ? Dx : Dy;
  }
  if (Dx < 0.0f) return Dy;
  if (Dy < 0.0f) return Dx;
  {
    /* Outside the corner square: distance to the fillet centre minus r. */
    f32 D2 = Dx * Dx + Dy * Dy;
    f32 Est = (D2 > 0.0f) ? D2 : 1.0f;
    /* Two Newton iterations - enough for 1px precision. */
    Est = 0.5f * (Est + D2 / Est);
    Est = 0.5f * (Est + D2 / Est);
    return Est - R;
  }
}

/* Fast tanh approximation (|err| < 3e-4 in the range we use). Only
 * the magnitude matters here; accuracy inside 1/255 of alpha. */
static f32
TanhApprox (f32 X) {
  f32 X2 = X * X;
  f32 N  = X * (27.0f + X2);
  f32 D  = 27.0f + 9.0f * X2;
  if (X >  4.0f) return 1.0f;
  if (X < -4.0f) return -1.0f;
  return N / D;
}

/* Paint one outer-shadow layer. Shape is the padding-box rounded
 * rect inflated by Spread and translated by (Ox, Oy). Pixel alpha
 * follows a Gaussian-step falloff where the sigma is Blur / 2 -
 * this matches the Chrome/Firefox visual convention for small
 * blurs. Inside the shape, alpha is the source color's alpha; it
 * then smoothly decays as the SDF goes positive. */
static void
PaintOuterShadow (DfBrBitmap *Bmp,
                  f32 X, f32 Y, f32 W, f32 H,
                  f32 Tl, f32 Tr, f32 Br, f32 Bl,
                  f32 Ox, f32 Oy, f32 Blur, f32 Spread,
                  DfBrColor C)
{
  f32 Sx = X + Ox - Spread;
  f32 Sy = Y + Oy - Spread;
  f32 Sw = W + 2.0f * Spread;
  f32 Sh = H + 2.0f * Spread;
  f32 STl = Tl + Spread, STr = Tr + Spread;
  f32 SBr = Br + Spread, SBl = Bl + Spread;
  f32 Pad = Blur * 1.8f + 1.0f;        /* 3-sigma coverage in pixels */
  f32 Sigma = (Blur > 0.0f) ? Blur * 0.5f : 0.0f;
  i32 Tw = (i32)Bmp->Width, Th = (i32)Bmp->Height;
  i32 X0, Y0, X1, Y1, Px, Py;

  if (C.A == 0) return;
  if (Sw <= 0.0f || Sh <= 0.0f) return;
  if (STl < 0.0f) STl = 0.0f;
  if (STr < 0.0f) STr = 0.0f;
  if (SBr < 0.0f) SBr = 0.0f;
  if (SBl < 0.0f) SBl = 0.0f;

  X0 = (i32)(Sx - Pad - 0.5f);
  Y0 = (i32)(Sy - Pad - 0.5f);
  X1 = (i32)(Sx + Sw + Pad + 0.5f) + 1;
  Y1 = (i32)(Sy + Sh + Pad + 0.5f) + 1;
  if (X0 < 0) X0 = 0;
  if (Y0 < 0) Y0 = 0;
  if (X1 > Tw) X1 = Tw;
  if (Y1 > Th) Y1 = Th;

  for (Py = Y0; Py < Y1; Py++) {
    DfBrColor *Row = Bmp->Pixels + (usize)Py * Bmp->Width;
    f32 Pfy = (f32)Py + 0.5f;
    for (Px = X0; Px < X1; Px++) {
      f32 Pfx = (f32)Px + 0.5f;
      f32 D = RoundRectSdf (Pfx, Pfy, Sx, Sy, Sw, Sh, STl, STr, SBr, SBl);
      f32 A;
      u32 Ai, Iai;
      DfBrColor Dst;
      if (Sigma <= 0.01f) {
        /* Hard-edged shadow (blur = 0): full inside, zero outside. */
        if (D > 0.0f) continue;
        A = 1.0f;
      } else {
        /* Gaussian-blurred step: alpha = 0.5 * (1 - erf(D/(sigma*sqrt2))).
         * erf(x) ~ tanh(1.2x) is close enough for visible alpha. */
        f32 T = D / (Sigma * 1.41421356f);
        A = 0.5f * (1.0f - TanhApprox (1.2f * T));
      }
      if (A <= 0.0f) continue;
      if (A > 1.0f) A = 1.0f;
      Ai = (u32)(A * (f32)C.A + 0.5f);
      if (Ai == 0) continue;
      if (Ai >= 255) {
        Row[Px] = C;
        Row[Px].A = 0xFF;
        continue;
      }
      Dst = Row[Px];
      Iai = 255 - Ai;
      Row[Px].R = (u8)(((u32)C.R * Ai + (u32)Dst.R * Iai + 127) / 255);
      Row[Px].G = (u8)(((u32)C.G * Ai + (u32)Dst.G * Iai + 127) / 255);
      Row[Px].B = (u8)(((u32)C.B * Ai + (u32)Dst.B * Iai + 127) / 255);
      Row[Px].A = 0xFF;
    }
  }
}

/* Paint one inset shadow layer. The visible area is the padding-box
 * rounded rect; inside that, alpha is zero well inside the
 * spread-inset / offset inner shape and ramps up to full as the
 * point crosses the inner edge towards the outer edge. */
static void
PaintInsetShadow (DfBrBitmap *Bmp,
                  f32 X, f32 Y, f32 W, f32 H,
                  f32 Tl, f32 Tr, f32 Br, f32 Bl,
                  f32 Ox, f32 Oy, f32 Blur, f32 Spread,
                  DfBrColor C)
{
  f32 Ix = X + Ox + Spread;
  f32 Iy = Y + Oy + Spread;
  f32 Iw = W - 2.0f * Spread;
  f32 Ih = H - 2.0f * Spread;
  f32 ITl = Tl - Spread, ITr = Tr - Spread;
  f32 IBr = Br - Spread, IBl = Bl - Spread;
  f32 Sigma = (Blur > 0.0f) ? Blur * 0.5f : 0.0f;
  i32 Tw = (i32)Bmp->Width, Th = (i32)Bmp->Height;
  i32 X0, Y0, X1, Y1, Px, Py;

  if (C.A == 0) return;
  if (ITl < 0.0f) ITl = 0.0f;
  if (ITr < 0.0f) ITr = 0.0f;
  if (IBr < 0.0f) IBr = 0.0f;
  if (IBl < 0.0f) IBl = 0.0f;
  if (Iw < 0.0f) Iw = 0.0f;
  if (Ih < 0.0f) Ih = 0.0f;

  X0 = (i32)(X - 0.5f);
  Y0 = (i32)(Y - 0.5f);
  X1 = (i32)(X + W + 0.5f) + 1;
  Y1 = (i32)(Y + H + 0.5f) + 1;
  if (X0 < 0) X0 = 0;
  if (Y0 < 0) Y0 = 0;
  if (X1 > Tw) X1 = Tw;
  if (Y1 > Th) Y1 = Th;

  for (Py = Y0; Py < Y1; Py++) {
    DfBrColor *Row = Bmp->Pixels + (usize)Py * Bmp->Width;
    f32 Pfy = (f32)Py + 0.5f;
    for (Px = X0; Px < X1; Px++) {
      f32 Pfx = (f32)Px + 0.5f;
      /* Outer clip: only paint inside the padding-box rounded rect. */
      f32 Dout = RoundRectSdf (Pfx, Pfy, X, Y, W, H, Tl, Tr, Br, Bl);
      f32 Din, A;
      u32 Ai, Iai;
      DfBrColor Dst;
      if (Dout > 0.0f) continue;
      Din = RoundRectSdf (Pfx, Pfy, Ix, Iy, Iw, Ih, ITl, ITr, IBr, IBl);
      if (Sigma <= 0.01f) {
        if (Din < 0.0f) continue;
        A = 1.0f;
      } else {
        /* Full alpha outside the inner shape, fades to 0 inside. */
        f32 T = Din / (Sigma * 1.41421356f);
        A = 0.5f * (1.0f + TanhApprox (1.2f * T));
      }
      if (A <= 0.0f) continue;
      if (A > 1.0f) A = 1.0f;
      Ai = (u32)(A * (f32)C.A + 0.5f);
      if (Ai == 0) continue;
      if (Ai >= 255) {
        Row[Px] = C;
        Row[Px].A = 0xFF;
        continue;
      }
      Dst = Row[Px];
      Iai = 255 - Ai;
      Row[Px].R = (u8)(((u32)C.R * Ai + (u32)Dst.R * Iai + 127) / 255);
      Row[Px].G = (u8)(((u32)C.G * Ai + (u32)Dst.G * Iai + 127) / 255);
      Row[Px].B = (u8)(((u32)C.B * Ai + (u32)Dst.B * Iai + 127) / 255);
      Row[Px].A = 0xFF;
    }
  }
}

/* Paint all outer / inset box-shadow layers for Box. `Inset`
 * selects which pass to run: outer shadows paint BEFORE the
 * background (they sit behind the box) while inset shadows paint
 * between the background and borders. Per CSS the first shadow in
 * the declaration list paints on top, so later entries are drawn
 * first to preserve z-order. */
static void
PaintBoxShadows (const DfBrLayoutBox *Box, DfBrBitmap *Bmp, b32 Inset)
{
  f32 X, Y, W, H;
  f32 OW, OH;
  f32 Tl, Tr, Br, Bl;
  f32 Fs;
  const DfBrCssComputedStyle *S;
  const DfBrCssShadow *Head;
  const DfBrCssShadow *Chain[64];
  i32 Depth = 0;
  const DfBrCssShadow *L;
  i32 I;

  if (Box == DFBR_NULL || Box->Style == DFBR_NULL) return;
  S = Box->Style;
  Head = S->BoxShadow;
  if (Head == DFBR_NULL) return;

  BoxPaddingRect (Box, &X, &Y, &W, &H);
  OW = W + Box->Border.Left + Box->Border.Right;
  OH = H + Box->Border.Top  + Box->Border.Bottom;
  ResolveCornerRadii (Box, OW, OH, &Tl, &Tr, &Br, &Bl);
  if (!Inset) {
    /* Outer shadows clip to the border-box; treat radii as-is and
     * treat the shape as the border-box (padding rect + borders). */
    X -= Box->Border.Left;
    Y -= Box->Border.Top;
    W  = OW;
    H  = OH;
  } else {
    /* Inset shadows clip to the padding-box: subtract per-side
     * border from the corner radii so the shape is concentric. */
    if (Tl > 0.0f) { Tl -= (Box->Border.Top > Box->Border.Left  ? Box->Border.Top : Box->Border.Left);  if (Tl < 0.0f) Tl = 0.0f; }
    if (Tr > 0.0f) { Tr -= (Box->Border.Top > Box->Border.Right ? Box->Border.Top : Box->Border.Right); if (Tr < 0.0f) Tr = 0.0f; }
    if (Br > 0.0f) { Br -= (Box->Border.Bottom > Box->Border.Right ? Box->Border.Bottom : Box->Border.Right); if (Br < 0.0f) Br = 0.0f; }
    if (Bl > 0.0f) { Bl -= (Box->Border.Bottom > Box->Border.Left  ? Box->Border.Bottom : Box->Border.Left);  if (Bl < 0.0f) Bl = 0.0f; }
  }

  Fs = Box->FontSizePx;

  for (L = Head; L != DFBR_NULL && Depth < 64; L = L->Next) Chain[Depth++] = L;

  /* Paint from last to first so the first declared layer ends up on top. */
  for (I = Depth - 1; I >= 0; I--) {
    const DfBrCssShadow *Sh = Chain[I];
    if ((b32)Sh->Inset != Inset) continue;
    {
      DfBrCssColor Src = Sh->Color;
      DfBrColor    C;
      f32 Ox, Oy, Bl2, Sp;
      if (Sh->ColorIsCurrent) Src = S->Color;
      C = CssColorToDfBrColor (Src);
      Ox  = ResolveShadowLenPx (&Sh->OffsetX, Fs);
      Oy  = ResolveShadowLenPx (&Sh->OffsetY, Fs);
      Bl2 = ResolveShadowLenPx (&Sh->Blur,    Fs);
      Sp  = ResolveShadowLenPx (&Sh->Spread,  Fs);
      if (Bl2 < 0.0f) Bl2 = 0.0f;
      if (Inset) {
        PaintInsetShadow (Bmp, X, Y, W, H, Tl, Tr, Br, Bl, Ox, Oy, Bl2, Sp, C);
      } else {
        PaintOuterShadow (Bmp, X, Y, W, H, Tl, Tr, Br, Bl, Ox, Oy, Bl2, Sp, C);
      }
    }
  }
}

static void
PaintBackground (const DfBrLayoutBox *Box, DfBrBitmap *Bmp)
{
  f32 X, Y, W, H;
  f32 OW, OH;
  f32 Tl, Tr, Br, Bl;
  f32 BT, BR2, BB, BL;
  DfBrColor Fill;
  const DfBrCssComputedStyle *S;
  const DfBrCssGradient      *G;

  if (Box == DFBR_NULL || Box->Style == DFBR_NULL) return;
  S = Box->Style;
  G = S->BackgroundGradient;

  /* Skip when there's nothing to paint: no solid color AND no
   * gradient. `background-clip: text` also routes color+gradient
   * through the text renderer rather than filling the box, so
   * suppress the box fill in that case. */
  if (!ColorVisible (S->BackgroundColor) && G == DFBR_NULL) return;
  if (S->BackgroundClip == DFBR_CSS_BGCLIP_TEXT)             return;

  BoxPaddingRect (Box, &X, &Y, &W, &H);
  Fill = CssColorToDfBrColor (S->BackgroundColor);

  /* Border-radius is resolved against the border-box; we approximate
   * the inner (padding-box) radii by subtracting the per-side border
   * width. This keeps the visible background curve concentric with
   * the border's outer edge. */
  BT = Box->Border.Top; BR2 = Box->Border.Right;
  BB = Box->Border.Bottom; BL  = Box->Border.Left;
  OW = W + BL + BR2; OH = H + BT + BB;
  ResolveCornerRadii (Box, OW, OH, &Tl, &Tr, &Br, &Bl);
  if (Tl > 0.0f) { Tl -= (BT > BL  ? BT : BL);  if (Tl < 0.0f) Tl = 0.0f; }
  if (Tr > 0.0f) { Tr -= (BT > BR2 ? BT : BR2); if (Tr < 0.0f) Tr = 0.0f; }
  if (Br > 0.0f) { Br -= (BB > BR2 ? BB : BR2); if (Br < 0.0f) Br = 0.0f; }
  if (Bl > 0.0f) { Bl -= (BB > BL  ? BB : BL);  if (Bl < 0.0f) Bl = 0.0f; }

  /* CSS Backgrounds L3 §3.10: background-color paints underneath
   * background-image (or the gradient). */
  if (ColorVisible (S->BackgroundColor)) {
    FillRectRounded (Bmp, X, Y, W, H, Tl, Tr, Br, Bl, Fill);
  }
  if (G != DFBR_NULL) {
    FillGradientRounded (Bmp, X, Y, W, H, Tl, Tr, Br, Bl, G, S->Color);
  }
}

/* ------------------------------------------------------------------
 *  Borders
 *
 *  v1 draws each of the four sides as a solid rect using the
 *  resolved border width + color. Different border-styles render
 *  identically (solid). Corners are mitered via the standard "top
 *  and bottom extend full width, left/right fill between" approach.
 * -----------------------------------------------------------------*/

static DfBrColor
BorderColorFor (const DfBrCssComputedStyle *S, int Side /* 0..3 = T/R/B/L */)
{
  const DfBrCssBorder *B;
  DfBrCssColor         C;
  switch (Side) {
    case 0:  B = &S->BorderTop;    break;
    case 1:  B = &S->BorderRight;  break;
    case 2:  B = &S->BorderBottom; break;
    default: B = &S->BorderLeft;   break;
  }
  C = B->Color;
  if (B->ColorIsCurrent) {
    /* currentColor resolution: element's foreground. Note that we do
     * NOT fall back to currentColor when the author-set color is
     * literally transparent (alpha 0) - per CSS, `transparent` means
     * "draw nothing", so we pass it through and the FillRect becomes
     * a no-op. */
    C = S->Color;
  }
  return CssColorToDfBrColor (C);
}

/* Fill the "ring" between an outer and inner rounded rectangle with a
 * single color. This is how we draw a uniform-color border with
 * rounded corners in one pass; straight edges are still perfectly
 * crisp axis-aligned rects, and the corners get full AA because
 * FillRectRounded already handles it. */
static void
FillRoundedRing (DfBrBitmap *Bmp,
                 f32 OX, f32 OY, f32 OW, f32 OH,
                 f32 OTl, f32 OTr, f32 OBr, f32 OBl,
                 f32 IX, f32 IY, f32 IW, f32 IH,
                 f32 ITl, f32 ITr, f32 IBr, f32 IBl,
                 DfBrColor C)
{
  i32 Tw = (i32)Bmp->Width, Th = (i32)Bmp->Height;
  i32 X0, Y0, X1, Y1, Px, Py;
  if (OW <= 0.0f || OH <= 0.0f) return;
  X0 = (i32)(OX - 0.5f);
  Y0 = (i32)(OY - 0.5f);
  X1 = (i32)(OX + OW + 0.5f) + 1;
  Y1 = (i32)(OY + OH + 0.5f) + 1;
  if (X0 < 0) X0 = 0;
  if (Y0 < 0) Y0 = 0;
  if (X1 > Tw) X1 = Tw;
  if (Y1 > Th) Y1 = Th;
  for (Py = Y0; Py < Y1; Py++) {
    DfBrColor *Row = Bmp->Pixels + (usize)Py * Bmp->Width;
    for (Px = X0; Px < X1; Px++) {
      f32 Fx = (f32)Px + 0.5f, Fy = (f32)Py + 0.5f;
      f32 OCov = RoundRectCoverage (Fx, Fy, OX, OY, OW, OH, OTl, OTr, OBr, OBl);
      f32 ICov;
      f32 Ring;
      if (OCov <= 0.0f) continue;
      ICov = RoundRectCoverage (Fx, Fy, IX, IY, IW, IH, ITl, ITr, IBr, IBl);
      Ring = OCov - ICov;
      if (Ring <= 0.0f) continue;
      if (Ring > 1.0f) Ring = 1.0f;
      if (Ring >= 1.0f) {
        Row[Px] = C;
      } else {
        DfBrColor Dst = Row[Px];
        u32 A  = (u32)(Ring * 255.0f + 0.5f);
        u32 Ia = 255 - A;
        Row[Px].R = (u8)(((u32)C.R * A + (u32)Dst.R * Ia + 127) / 255);
        Row[Px].G = (u8)(((u32)C.G * A + (u32)Dst.G * Ia + 127) / 255);
        Row[Px].B = (u8)(((u32)C.B * A + (u32)Dst.B * Ia + 127) / 255);
        Row[Px].A = 0xFF;
      }
    }
  }
}

static void
PaintBorders (const DfBrLayoutBox *Box, DfBrBitmap *Bmp)
{
  f32       PadX, PadY, PadW, PadH;   /* inside the border */
  f32       BorderL, BorderR, BorderT, BorderB;
  f32       OuterX, OuterY, OuterW, OuterH;
  f32       Tl, Tr, Br, Bl;
  f32       ITl, ITr, IBr, IBl;
  DfBrColor Ct, Cr, Cb, Cl;
  b32       Rounded;
  b32       Uniform;

  if (Box == DFBR_NULL || Box->Style == DFBR_NULL) return;

  BorderT = Box->Border.Top;
  BorderR = Box->Border.Right;
  BorderB = Box->Border.Bottom;
  BorderL = Box->Border.Left;
  if (BorderT <= 0.0f && BorderR <= 0.0f &&
      BorderB <= 0.0f && BorderL <= 0.0f) return;

  BoxPaddingRect (Box, &PadX, &PadY, &PadW, &PadH);
  OuterX = PadX - BorderL;
  OuterY = PadY - BorderT;
  OuterW = PadW + BorderL + BorderR;
  OuterH = PadH + BorderT + BorderB;

  Ct = BorderColorFor (Box->Style, 0);
  Cr = BorderColorFor (Box->Style, 1);
  Cb = BorderColorFor (Box->Style, 2);
  Cl = BorderColorFor (Box->Style, 3);

  ResolveCornerRadii (Box, OuterW, OuterH, &Tl, &Tr, &Br, &Bl);
  Rounded = (Tl > 0.0f || Tr > 0.0f || Br > 0.0f || Bl > 0.0f);

  /* When the four sides share the same color we can fill the entire
   * ring in a single pass, which keeps corner AA clean. A rectangle
   * with mixed border colors (e.g. the classic css-demo 4-color
   * border) still needs per-side draws, but we'd have to synthesize
   * the mitered corner triangles, which is not on the roadmap yet. */
  Uniform = (Ct.R == Cr.R && Ct.G == Cr.G && Ct.B == Cr.B && Ct.A == Cr.A &&
             Ct.R == Cb.R && Ct.G == Cb.G && Ct.B == Cb.B && Ct.A == Cb.A &&
             Ct.R == Cl.R && Ct.G == Cl.G && Ct.B == Cl.B && Ct.A == Cl.A);

  if (Rounded && Uniform) {
    /* Inner radii = outer radii minus the thicker of the adjacent
     * border widths, same rule we used in PaintBackground. */
    ITl = Tl - ((BorderT > BorderL) ? BorderT : BorderL); if (ITl < 0.0f) ITl = 0.0f;
    ITr = Tr - ((BorderT > BorderR) ? BorderT : BorderR); if (ITr < 0.0f) ITr = 0.0f;
    IBr = Br - ((BorderB > BorderR) ? BorderB : BorderR); if (IBr < 0.0f) IBr = 0.0f;
    IBl = Bl - ((BorderB > BorderL) ? BorderB : BorderL); if (IBl < 0.0f) IBl = 0.0f;
    FillRoundedRing (Bmp,
                     OuterX, OuterY, OuterW, OuterH, Tl, Tr, Br, Bl,
                     PadX, PadY, PadW, PadH,        ITl, ITr, IBr, IBl,
                     Ct);
    return;
  }

  /* Top  : full width, over the top-left to top-right corners. */
  if (BorderT > 0.0f) {
    FillRect (Bmp, OuterX, OuterY, OuterW, BorderT, Ct);
  }
  /* Bottom. */
  if (BorderB > 0.0f) {
    FillRect (Bmp, OuterX, OuterY + OuterH - BorderB, OuterW, BorderB, Cb);
  }
  /* Left   : between top + bottom borders. */
  if (BorderL > 0.0f) {
    FillRect (Bmp, OuterX, OuterY + BorderT,
              BorderL, OuterH - BorderT - BorderB, Cl);
  }
  /* Right. */
  if (BorderR > 0.0f) {
    FillRect (Bmp, OuterX + OuterW - BorderR, OuterY + BorderT,
              BorderR, OuterH - BorderT - BorderB, Cr);
  }
}

/* ------------------------------------------------------------------
 *  Decoded <img> painting
 *
 *  Nearest-neighbour blit of an RGBA8 source bitmap (from stb_image)
 *  onto the paint target at the box's content rect. We do the
 *  rounding outward by +0.5 so box geometry that lines up on pixel
 *  boundaries hits a tight [X0, X0+W) span, and we clip against the
 *  target bitmap to stay safe even when the box is partially or
 *  fully off-viewport.
 *
 *  Alpha is composited (source-over) pixel-by-pixel. This is slow
 *  for big images; a SIMD version is on the to-do list, but most
 *  pages use tiny icons (<= 64x64) for data: URLs so the naïve loop
 *  is fine for now.
 * -----------------------------------------------------------------*/

static void
PaintDecodedImage (const DfBrLayoutBox *Box, DfBrBitmap *Bmp)
{
  i32 Tw, Th;
  i32 X0, Y0, X1, Y1;
  i32 Px, Py;
  f32 ScaleX, ScaleY;
  const u8 *Src = Box->ImagePixels;
  u32 Sw = Box->ImageW, Sh = Box->ImageH;
  f32 X = Box->ContentRect.X, Y = Box->ContentRect.Y;
  f32 W = Box->ContentRect.W, H = Box->ContentRect.H;

  if (Src == DFBR_NULL || Sw == 0 || Sh == 0) return;
  if (W <= 0.0f || H <= 0.0f) return;

  Tw = (i32)Bmp->Width; Th = (i32)Bmp->Height;
  X0 = (i32)(X + 0.5f); Y0 = (i32)(Y + 0.5f);
  X1 = (i32)(X + W + 0.5f); Y1 = (i32)(Y + H + 0.5f);
  if (X0 < 0)  X0 = 0;
  if (Y0 < 0)  Y0 = 0;
  if (X1 > Tw) X1 = Tw;
  if (Y1 > Th) Y1 = Th;
  if (X0 >= X1 || Y0 >= Y1) return;

  ScaleX = (f32)Sw / W;
  ScaleY = (f32)Sh / H;

  for (Py = Y0; Py < Y1; Py++) {
    DfBrColor *Row = Bmp->Pixels + (usize)Py * Bmp->Width;
    i32        Sy  = (i32)(((f32)Py + 0.5f - Y) * ScaleY);
    const u8  *SrcRow;
    if (Sy < 0) Sy = 0;
    if ((u32)Sy >= Sh) Sy = (i32)(Sh - 1);
    SrcRow = Src + (usize)Sy * Sw * 4;
    for (Px = X0; Px < X1; Px++) {
      i32 Sx = (i32)(((f32)Px + 0.5f - X) * ScaleX);
      const u8 *P;
      u32 Sr, Sg, Sb, Sa;
      if (Sx < 0) Sx = 0;
      if ((u32)Sx >= Sw) Sx = (i32)(Sw - 1);
      P  = SrcRow + (usize)Sx * 4;
      Sr = P[0]; Sg = P[1]; Sb = P[2]; Sa = P[3];
      if (Sa == 0) continue;
      if (Sa == 255) {
        Row[Px] = DfBrRgb ((u8)Sr, (u8)Sg, (u8)Sb);
      } else {
        /* source-over: out = src + dst * (1 - srcA). */
        DfBrColor Dst = Row[Px];
        u32 Inv = 255 - Sa;
        u32 Or  = (Sr * Sa + (u32)Dst.R * Inv + 127) / 255;
        u32 Og  = (Sg * Sa + (u32)Dst.G * Inv + 127) / 255;
        u32 Ob  = (Sb * Sa + (u32)Dst.B * Inv + 127) / 255;
        Row[Px] = DfBrRgb ((u8)Or, (u8)Og, (u8)Ob);
      }
    }
  }
}

/* ------------------------------------------------------------------
 *  Replaced elements (placeholder)
 *
 *  Draw a subdued checkerboard over the box content rect plus a tiny
 *  diagonal "X" so it's obvious what didn't render. Eventually
 *  `background-image: url(...)` and `<img>` decode through stb_image
 *  replace this.
 * -----------------------------------------------------------------*/

static void
PaintReplacedPlaceholder (const DfBrLayoutBox *Box, DfBrBitmap *Bmp)
{
  f32       X, Y, W, H;
  i32       Tw, Th, X0, Y0, X1, Y1;
  DfBrColor Light = DfBrRgb (235, 235, 240);
  DfBrColor Dark  = DfBrRgb (205, 205, 215);
  DfBrColor Edge  = DfBrRgb (150, 150, 160);
  i32       Py, Px;

  if (Box == DFBR_NULL || Bmp == DFBR_NULL || Bmp->Pixels == DFBR_NULL) return;

  X = Box->ContentRect.X; Y = Box->ContentRect.Y;
  W = Box->ContentRect.W; H = Box->ContentRect.H;
  if (W <= 0.0f || H <= 0.0f) return;

  Tw = (i32)Bmp->Width; Th = (i32)Bmp->Height;
  X0 = (i32)(X + 0.5f); Y0 = (i32)(Y + 0.5f);
  X1 = (i32)(X + W + 0.5f); Y1 = (i32)(Y + H + 0.5f);
  if (X0 < 0)  X0 = 0;
  if (Y0 < 0)  Y0 = 0;
  if (X1 > Tw) X1 = Tw;
  if (Y1 > Th) Y1 = Th;
  if (X0 >= X1 || Y0 >= Y1) return;

  for (Py = Y0; Py < Y1; Py++) {
    DfBrColor *Row = Bmp->Pixels + (usize)Py * Bmp->Width;
    for (Px = X0; Px < X1; Px++) {
      i32 Cx = (Px - X0) / 8;
      i32 Cy = (Py - Y0) / 8;
      Row[Px] = (((Cx + Cy) & 1) != 0) ? Dark : Light;
    }
  }

  /* 1-pixel outline so the placeholder stays visually distinct. */
  FillRect (Bmp, (f32)X0, (f32)Y0, (f32)(X1 - X0), 1.0f, Edge);
  FillRect (Bmp, (f32)X0, (f32)(Y1 - 1), (f32)(X1 - X0), 1.0f, Edge);
  FillRect (Bmp, (f32)X0, (f32)Y0, 1.0f, (f32)(Y1 - Y0), Edge);
  FillRect (Bmp, (f32)(X1 - 1), (f32)Y0, 1.0f, (f32)(Y1 - Y0), Edge);
}

/* ------------------------------------------------------------------
 *  Form controls (<input>, <textarea>, <select>, <button>)
 *
 *  DOM form elements are REPLACED boxes - their content is rendered
 *  by the UA rather than by descendant box paint. We give them a
 *  minimal "looks like a control" appearance so pages that mix text
 *  and controls (search boxes, login forms, Google homepage) render
 *  recognizably without a full forms engine.
 *
 *  v1 scope:
 *    * <input type="submit|button|reset">: light gray pill with
 *      centered label text from the `value` attribute (or a
 *      tag-specific default).
 *    * <input type="text|search|email|url|password|tel|number|
 *                    date|time|datetime-local|month|week">: white
 *      box with a 1px border and the `value` or `placeholder`
 *      attribute rendered left-aligned. Password text is replaced
 *      by bullet glyphs so real contents don't leak.
 *    * <input type="checkbox|radio">: small bordered square/circle,
 *      filled accent when the `checked` attribute is present.
 *    * <textarea>: white box with a 1px border and the element's
 *      text content rendered top-left.
 *    * <select>: white box with a 1px border, the first <option>'s
 *      text, and a small "v" dropdown indicator on the right.
 *
 *  Anything not handled here falls through to PaintReplacedPlaceholder.
 * -----------------------------------------------------------------*/

static DfBrFontFace *
ResolveControlFace (DfBrFontEngine *Engine,
                    const DfBrCssComputedStyle *S,
                    f32 SizePx)
{
  if (Engine == DFBR_NULL || S == DFBR_NULL) return DFBR_NULL;
  return DfBrFontEngineResolve (Engine,
                                S->FontFamilies, S->FontFamilyCount,
                                S->FontWeight,
                                MapFontStyle (S->FontStyle),
                                SizePx);
}

/* Draw `Utf8` horizontally centered in the rect [X..X+W]x[Y..Y+H]
 * using the face's metrics for vertical centering (baseline sits
 * at H/2 + ascent/2 - descent/2 approximately). */
static void
DrawCenteredUtf8 (DfBrBitmap *Bmp, DfBrFontFace *Face,
                  const char *Utf8, usize Len,
                  f32 X, f32 Y, f32 W, f32 H,
                  DfBrColor Color)
{
  f32             TextW;
  f32             PenX, Baseline;
  DfBrFontMetrics M;
  if (Face == DFBR_NULL || Utf8 == DFBR_NULL || Len == 0) return;

  TextW = DfBrFontFaceMeasureUtf8 (Face, Utf8, Len);
  DfBrFontFaceGetMetrics (Face, &M);

  PenX     = X + (W - TextW) * 0.5f;
  Baseline = Y + (H + (M.AscentPx - M.DescentPx)) * 0.5f;
  if (PenX < X) PenX = X;
  DfBrFontFaceDrawUtf8 (Face, Utf8, Len, Bmp, PenX, Baseline, Color);
}

/* Draw `Utf8` left-aligned (with padding) inside [X..X+W]x[Y..Y+H].
 * Clipped to the control by the face rasterizer. */
static void
DrawLeftUtf8 (DfBrBitmap *Bmp, DfBrFontFace *Face,
              const char *Utf8, usize Len,
              f32 X, f32 Y, f32 W, f32 H, f32 PadX,
              DfBrColor Color)
{
  f32             Baseline;
  DfBrFontMetrics M;
  (void)W;
  if (Face == DFBR_NULL || Utf8 == DFBR_NULL || Len == 0) return;

  DfBrFontFaceGetMetrics (Face, &M);
  Baseline = Y + (H + (M.AscentPx - M.DescentPx)) * 0.5f;
  DfBrFontFaceDrawUtf8 (Face, Utf8, Len, Bmp, X + PadX, Baseline, Color);
}

/* Filled rect + 1px border helper. If `Box` carries a border-radius
 * we route through the rounded primitives so form controls pick up
 * UA rounding on their own, without requiring a per-element
 * stylesheet rule. */
static void
DrawFilledBorderRect (DfBrBitmap *Bmp,
                      const DfBrLayoutBox *Box,
                      f32 X, f32 Y, f32 W, f32 H,
                      DfBrColor Fill, DfBrColor Border)
{
  f32 Tl = 0.0f, Tr = 0.0f, Br = 0.0f, Bl = 0.0f;
  if (W <= 0.0f || H <= 0.0f) return;
  if (Box != DFBR_NULL) {
    ResolveCornerRadii (Box, W, H, &Tl, &Tr, &Br, &Bl);
  }
  if (Tl > 0.0f || Tr > 0.0f || Br > 0.0f || Bl > 0.0f) {
    f32 ITl, ITr, IBr, IBl;
    FillRectRounded (Bmp, X, Y, W, H, Tl, Tr, Br, Bl, Fill);
    /* 1px inset for the border stroke. Inner radii shrink by 1 so
     * the ring keeps a uniform width through the corners. */
    ITl = Tl > 1.0f ? Tl - 1.0f : 0.0f;
    ITr = Tr > 1.0f ? Tr - 1.0f : 0.0f;
    IBr = Br > 1.0f ? Br - 1.0f : 0.0f;
    IBl = Bl > 1.0f ? Bl - 1.0f : 0.0f;
    FillRoundedRing (Bmp, X, Y, W, H, Tl, Tr, Br, Bl,
                     X + 1.0f, Y + 1.0f, W - 2.0f, H - 2.0f,
                     ITl, ITr, IBr, IBl, Border);
    return;
  }
  FillRect (Bmp, X, Y, W, H, Fill);
  FillRect (Bmp, X, Y, W, 1.0f, Border);
  FillRect (Bmp, X, Y + H - 1.0f, W, 1.0f, Border);
  FillRect (Bmp, X, Y, 1.0f, H, Border);
  FillRect (Bmp, X + W - 1.0f, Y, 1.0f, H, Border);
}

/* Length helper for "value" / "placeholder" / text content. */
static usize
StrLenSafe (const char *S) {
  usize N = 0;
  if (S == DFBR_NULL) return 0;
  while (S[N] != '\0') N++;
  return N;
}

/* Concatenate N bullets ("•") into a small stack buffer for password
 * inputs. Returns the number of bytes written. */
static usize
MakeBullets (char *Out, usize Cap, usize Count)
{
  usize I, J = 0;
  for (I = 0; I < Count; I++) {
    if (J + 3 > Cap) break;
    Out[J++] = (char)0xE2;
    Out[J++] = (char)0x80;
    Out[J++] = (char)0xA2;
  }
  return J;
}

static b32
StrEqI (const char *A, const char *B)
{
  if (A == DFBR_NULL || B == DFBR_NULL) return DFBR_FALSE;
  while (*A && *B) {
    char Ca = *A, Cb = *B;
    if (Ca >= 'A' && Ca <= 'Z') Ca = (char)(Ca + ('a' - 'A'));
    if (Cb >= 'A' && Cb <= 'Z') Cb = (char)(Cb + ('a' - 'A'));
    if (Ca != Cb) return DFBR_FALSE;
    A++; B++;
  }
  return (b32)(*A == '\0' && *B == '\0');
}

static const char *
FirstNonEmptyTextContent (const DfBrHtmlNode *N)
{
  const DfBrHtmlNode *C;
  if (N == DFBR_NULL) return DFBR_NULL;
  for (C = N->FirstChild; C != DFBR_NULL; C = C->NextSibling) {
    if (C->Kind == DFBR_HTML_NODE_TEXT && C->TextLen > 0) {
      return C->Text;
    }
  }
  return DFBR_NULL;
}

static void
PaintInputControl (const DfBrLayoutBox *Box, DfBrBitmap *Bmp,
                   DfBrFontEngine *FontEngine)
{
  const DfBrHtmlNode *Dom;
  const char         *Type;
  const char         *Text;
  usize               TextLen;
  f32                 X, Y, W, H;
  DfBrColor           Bg, Border, Fg;
  DfBrFontFace       *Face;
  f32                 FontPx;
  char                Buf[128];
  b32                 IsButton = DFBR_FALSE;
  b32                 IsCheck  = DFBR_FALSE;
  b32                 IsRadio  = DFBR_FALSE;
  b32                 IsHidden = DFBR_FALSE;
  b32                 IsPwd    = DFBR_FALSE;
  b32                 IsSelect = DFBR_FALSE;
  b32                 IsArea   = DFBR_FALSE;

  if (Box == DFBR_NULL || Bmp == DFBR_NULL) return;
  Dom = Box->DomNode;
  if (Dom == DFBR_NULL) return;
  X = Box->ContentRect.X; Y = Box->ContentRect.Y;
  W = Box->ContentRect.W; H = Box->ContentRect.H;
  if (W <= 0.0f || H <= 0.0f) return;

  FontPx = (Box->Style != DFBR_NULL && Box->Style->FontSize.Value > 0.0f)
           ? Box->Style->FontSize.Value : 14.0f;
  Face = ResolveControlFace (FontEngine, Box->Style, FontPx);

  switch (Dom->Tag) {
    case DFBR_HTML_TAG_INPUT: {
      Type = DfBrHtmlNodeGetAttrValue (Dom, "type");
      if (Type == DFBR_NULL) Type = "text";
      IsButton = StrEqI (Type, "submit") || StrEqI (Type, "button") ||
                 StrEqI (Type, "reset");
      IsCheck  = StrEqI (Type, "checkbox");
      IsRadio  = StrEqI (Type, "radio");
      IsHidden = StrEqI (Type, "hidden");
      IsPwd    = StrEqI (Type, "password");
      break;
    }
    case DFBR_HTML_TAG_BUTTON:   IsButton = DFBR_TRUE; break;
    case DFBR_HTML_TAG_SELECT:   IsSelect = DFBR_TRUE; break;
    case DFBR_HTML_TAG_TEXTAREA: IsArea   = DFBR_TRUE; break;
    default: return;
  }

  if (IsHidden) return;

  /* Bordered background for everything except checkbox/radio (which
   * get their own geometry below). */
  Bg     = IsButton ? DfBrRgb (241, 243, 244) : DfBrRgb (255, 255, 255);
  Border = DfBrRgb (200, 201, 203);
  Fg     = DfBrRgb (32, 33, 36);

  if (IsCheck || IsRadio) {
    f32 Size = (W < H ? W : H);
    f32 Cx   = X;
    f32 Cy   = Y;
    if (Size > 16.0f) Size = 16.0f;
    DrawFilledBorderRect (Bmp, Box, Cx, Cy, Size, Size, Bg, Border);
    if (DfBrHtmlNodeGetAttrValue (Dom, "checked") != DFBR_NULL) {
      f32 Pad = 3.0f;
      FillRect (Bmp, Cx + Pad, Cy + Pad, Size - Pad * 2.0f,
                Size - Pad * 2.0f, DfBrRgb (26, 115, 232));
    }
    return;
  }

  DrawFilledBorderRect (Bmp, Box, X, Y, W, H, Bg, Border);

  /* Resolve the text to render inside the control. */
  Text    = DFBR_NULL;
  TextLen = 0;

  if (IsButton) {
    if (Dom->Tag == DFBR_HTML_TAG_INPUT) {
      Text = DfBrHtmlNodeGetAttrValue (Dom, "value");
      if (Text == DFBR_NULL || Text[0] == '\0') {
        Type = DfBrHtmlNodeGetAttrValue (Dom, "type");
        if (Type != DFBR_NULL && StrEqI (Type, "submit")) Text = "Submit";
        else if (Type != DFBR_NULL && StrEqI (Type, "reset")) Text = "Reset";
        else Text = "";
      }
    } else {
      /* <button>: prefer child text (inner content) but the DOM walk
       * is downstream of this replaced box, so peek directly. */
      Text = FirstNonEmptyTextContent (Dom);
      if (Text == DFBR_NULL) {
        Text = DfBrHtmlNodeGetAttrValue (Dom, "value");
      }
    }
    TextLen = StrLenSafe (Text);
    if (Face != DFBR_NULL && TextLen > 0) {
      DrawCenteredUtf8 (Bmp, Face, Text, TextLen, X, Y, W, H, Fg);
    }
    return;
  }

  if (IsSelect) {
    const DfBrHtmlNode *C;
    DfBrColor           Arrow = DfBrRgb (95, 99, 104);
    Text = DFBR_NULL;
    for (C = Dom->FirstChild; C != DFBR_NULL && Text == DFBR_NULL;
         C = C->NextSibling)
    {
      if (C->Kind != DFBR_HTML_NODE_ELEMENT) continue;
      if (C->Tag == DFBR_HTML_TAG_OPTION) {
        Text = FirstNonEmptyTextContent (C);
      }
    }
    TextLen = StrLenSafe (Text);
    if (Face != DFBR_NULL && TextLen > 0) {
      DrawLeftUtf8 (Bmp, Face, Text, TextLen, X, Y, W - 16.0f, H, 6.0f, Fg);
    }
    /* Small down-chevron indicator on the right: two 6px diagonal
     * strokes forming a "v". */
    {
      f32 Ax = X + W - 12.0f;
      f32 Ay = Y + (H * 0.5f) - 1.0f;
      FillRect (Bmp, Ax,        Ay,        5.0f, 1.0f, Arrow);
      FillRect (Bmp, Ax + 4.0f, Ay - 1.0f, 1.0f, 3.0f, Arrow);
      FillRect (Bmp, Ax + 3.0f, Ay + 1.0f, 1.0f, 1.0f, Arrow);
      FillRect (Bmp, Ax + 2.0f, Ay + 2.0f, 1.0f, 1.0f, Arrow);
      FillRect (Bmp, Ax + 1.0f, Ay + 3.0f, 1.0f, 1.0f, Arrow);
    }
    return;
  }

  if (IsArea) {
    Text    = FirstNonEmptyTextContent (Dom);
    TextLen = StrLenSafe (Text);
    if (Face != DFBR_NULL && TextLen > 0) {
      DrawLeftUtf8 (Bmp, Face, Text, TextLen, X, Y, W, H, 6.0f, Fg);
    }
    return;
  }

  /* Text-ish input: value wins; fallback to placeholder drawn in
   * gray. Password input renders bullets instead of the raw value. */
  Text    = DfBrHtmlNodeGetAttrValue (Dom, "value");
  TextLen = StrLenSafe (Text);
  if (TextLen > 0 && IsPwd) {
    /* Count UTF-8 code points conservatively (bytes with !(b & 0xC0 ==
     * 0x80)); good enough for bullet count. */
    usize I, Pts = 0;
    for (I = 0; I < TextLen; I++) {
      if (((unsigned char)Text[I] & 0xC0) != 0x80) Pts++;
    }
    TextLen = MakeBullets (Buf, sizeof (Buf), Pts);
    Text    = Buf;
  }
  if (TextLen == 0) {
    Text    = DfBrHtmlNodeGetAttrValue (Dom, "placeholder");
    TextLen = StrLenSafe (Text);
    if (TextLen > 0) Fg = DfBrRgb (128, 134, 139);
  }
  if (Face != DFBR_NULL && TextLen > 0) {
    DrawLeftUtf8 (Bmp, Face, Text, TextLen, X, Y, W, H, 6.0f, Fg);
  }
}

/* ------------------------------------------------------------------
 *  Inline SVG replaced content
 *
 *  We round-trip the SVG subtree back through DfBrHtmlSerialize (the
 *  DOM preserves element/attribute structure), then hand the text to
 *  the stand-alone DfBrSvg rasterizer. The resulting bitmap is blit
 *  onto the paint canvas at the box's content rect.
 *
 *  Caveat: DfBrSvg returns an opaque bitmap pre-composited over
 *  white. For SVG icons on a white page background that's invisible;
 *  for icons on colored backgrounds it produces a white patch behind
 *  the glyph. Acceptable for v1; a future revision should teach the
 *  SVG rasterizer to return RGBA.
 * -----------------------------------------------------------------*/

static b32
IsSvgTag (const DfBrHtmlNode *N)
{
  return (N != DFBR_NULL &&
          N->Kind == DFBR_HTML_NODE_ELEMENT &&
          N->Tag  == DFBR_HTML_TAG_SVG);
}

/* Blit a DfBrSvg result into the target bitmap. We skip pixels that
 * match the pre-composited canvas color on the assumption they are
 * the SVG's empty background. Imperfect but keeps white-ish logos on
 * a white page clean and is a no-op for fully-colored pixels. */
static void
BlitSvgBitmap (DfBrBitmap *Dst, const DfBrBitmap *Src,
               f32 X, f32 Y, f32 W, f32 H)
{
  i32 Tw, Th, X0, Y0, X1, Y1;
  i32 Sw, Sh;
  i32 Py, Px;

  if (Dst == DFBR_NULL || Dst->Pixels == DFBR_NULL) return;
  if (Src == DFBR_NULL || Src->Pixels == DFBR_NULL) return;
  if (W <= 0.0f || H <= 0.0f) return;

  Tw = (i32)Dst->Width;  Th = (i32)Dst->Height;
  Sw = (i32)Src->Width;  Sh = (i32)Src->Height;
  if (Sw <= 0 || Sh <= 0) return;

  X0 = (i32)(X + 0.5f); Y0 = (i32)(Y + 0.5f);
  X1 = (i32)(X + W + 0.5f); Y1 = (i32)(Y + H + 0.5f);
  if (X0 < 0)  X0 = 0;
  if (Y0 < 0)  Y0 = 0;
  if (X1 > Tw) X1 = Tw;
  if (Y1 > Th) Y1 = Th;
  if (X0 >= X1 || Y0 >= Y1) return;

  for (Py = Y0; Py < Y1; Py++) {
    DfBrColor *Row = Dst->Pixels + (usize)Py * Dst->Width;
    i32 Syy = (i32)(((f32)(Py - Y0) / (f32)(Y1 - Y0)) * (f32)Sh);
    if (Syy < 0) Syy = 0;
    if (Syy >= Sh) Syy = Sh - 1;
    for (Px = X0; Px < X1; Px++) {
      i32 Sxx = (i32)(((f32)(Px - X0) / (f32)(X1 - X0)) * (f32)Sw);
      DfBrColor Sp;
      if (Sxx < 0) Sxx = 0;
      if (Sxx >= Sw) Sxx = Sw - 1;
      Sp = Src->Pixels[(usize)Syy * Src->Width + (usize)Sxx];
      /* Treat near-white pixels as transparent background. Anything
       * with noticeable saturation (any channel != white) or visibly
       * darker than white paints through. */
      if (Sp.R >= 250 && Sp.G >= 250 && Sp.B >= 250) continue;
      Sp.A = 255;
      Row[Px] = Sp;
    }
  }
}

static void
PaintSvgReplaced (const DfBrLayoutBox *Box, DfBrBitmap *Bmp)
{
  DfBrSvgBitmap  Out;
  DfBrResult     Rc;
  char          *Xml;
  usize          XmlLen = 0;
  u32            Tw, Th;
  f32            W, H;

  if (Box == DFBR_NULL || Box->DomNode == DFBR_NULL) return;
  W = Box->ContentRect.W;
  H = Box->ContentRect.H;
  if (W <= 0.0f || H <= 0.0f) return;

  Xml = DfBrHtmlSerialize (Box->DomNode, &XmlLen);
  if (Xml == DFBR_NULL || XmlLen == 0) {
    if (Xml != DFBR_NULL) DfBrHtmlFreeSerialized (Xml);
    return;
  }

  Tw = (u32)(W + 0.5f); if (Tw < 1) Tw = 1;
  Th = (u32)(H + 0.5f); if (Th < 1) Th = 1;
  /* DfBrSvgRender caps at 2048 internally; we stay under that. */
  if (Tw > 2048) Tw = 2048;
  if (Th > 2048) Th = 2048;

  memset (&Out, 0, sizeof (Out));
  Rc = DfBrSvgRender (Xml, XmlLen, Tw, Th, &Out);
  DfBrHtmlFreeSerialized (Xml);
  if (Rc != DFBR_OK) return;

  BlitSvgBitmap (Bmp, &Out.Bmp,
                 Box->ContentRect.X, Box->ContentRect.Y,
                 W, H);
  DfBrSvgFree (&Out);
}

/* ------------------------------------------------------------------
 *  Text
 * -----------------------------------------------------------------*/

static DfBrFontStyle
MapFontStyle (DfBrCssFontStyle S) {
  switch (S) {
    case DFBR_CSS_FSTYLE_ITALIC:  return DFBR_FONT_STYLE_ITALIC;
    case DFBR_CSS_FSTYLE_OBLIQUE: return DFBR_FONT_STYLE_OBLIQUE;
    default:                      return DFBR_FONT_STYLE_NORMAL;
  }
}

/* Returns DFBR_TRUE if Box or any of its block/inline ancestors
 * clips its visual overflow (overflow: hidden/scroll/clip, or
 * visibility: hidden). Follows the ancestor chain until we find
 * either a clipping ancestor (returns TRUE + clip rect via OutClip)
 * or the root (returns FALSE). The returned rect is the innermost
 * clipping ancestor's padding box, which is what CSS paints within.
 * Used to suppress the tell-tale sr-only "Screen-reader text" leak
 * every Bootstrap site has: the .sr-only span is a 1x1 positioned
 * span with overflow:hidden containing full-width glyphs that would
 * otherwise render at their natural size. */
static b32
FindClippingAncestor (const DfBrLayoutBox *Box,
                      f32 *OutX, f32 *OutY, f32 *OutW, f32 *OutH)
{
  const DfBrLayoutBox *A;
  /* Start from the PARENT: text boxes inherit overflow from their
   * parent element, but using the text box's own rect would degenerate
   * the clip to "the text fits inside itself", which is trivially true
   * and never culls. What we actually want is the rect of the first
   * ancestor that declared the clip - typically the sr-only span. */
  if (Box == DFBR_NULL) return DFBR_FALSE;
  for (A = Box->Parent; A != DFBR_NULL; A = A->Parent) {
    const DfBrCssComputedStyle *S = A->Style;
    if (S == DFBR_NULL) continue;
    if (S->Visibility == DFBR_CSS_HIDDEN) {
      /* Collapse the clip to empty so every child is suppressed. */
      *OutX = 0.0f; *OutY = 0.0f; *OutW = 0.0f; *OutH = 0.0f;
      return DFBR_TRUE;
    }
    if (S->OverflowX == DFBR_CSS_OVERFLOW_HIDDEN ||
        S->OverflowX == DFBR_CSS_OVERFLOW_CLIP   ||
        S->OverflowY == DFBR_CSS_OVERFLOW_HIDDEN ||
        S->OverflowY == DFBR_CSS_OVERFLOW_CLIP)
    {
      /* Padding box = ContentRect expanded by padding. */
      *OutX = A->ContentRect.X - A->Padding.Left;
      *OutY = A->ContentRect.Y - A->Padding.Top;
      *OutW = A->ContentRect.W + A->Padding.Left + A->Padding.Right;
      *OutH = A->ContentRect.H + A->Padding.Top  + A->Padding.Bottom;
      return DFBR_TRUE;
    }
  }
  return DFBR_FALSE;
}

static b32
FragmentIsClipped (const DfBrLayoutTextFragment *Tf, f32 FontSizePx,
                   f32 Cx, f32 Cy, f32 Cw, f32 Ch)
{
  /* Approximate text fragment extents: width = Advance; height ~=
   * 1.3 * FontSizePx centered on the baseline.
   *
   * Rejection rules:
   *   1. If the fragment lies entirely outside the clip, skip (the
   *      classic AABB-vs-AABB rejection).
   *   2. If the clip is "tiny" (width or height below a font-em
   *      threshold, a hallmark of the .sr-only / .visually-hidden
   *      pattern: position:absolute + width:1px + height:1px +
   *      overflow:hidden), require the fragment to be FULLY inside
   *      the clip. Without this case, bootstrap-shaped sites leak
   *      their "screen reader only" glyphs into the top-left of the
   *      viewport because a 1x1 clip overlaps a 85x20 text box by
   *      exactly one pixel and the cheap AABB rejection above keeps
   *      it alive. */
  f32 Fx1 = Tf->X;
  f32 Fx2 = Tf->X + Tf->Advance;
  f32 Fy2 = Tf->BaselineY + 0.3f * FontSizePx;
  f32 Fy1 = Tf->BaselineY - 1.0f * FontSizePx;
  if (Fx2 <= Cx || Fx1 >= Cx + Cw) return DFBR_TRUE;
  if (Fy2 <= Cy || Fy1 >= Cy + Ch) return DFBR_TRUE;
  if (Cw < 4.0f || Ch < 4.0f) {
    if (Fx1 < Cx || Fx2 > Cx + Cw) return DFBR_TRUE;
    if (Fy1 < Cy || Fy2 > Cy + Ch) return DFBR_TRUE;
  }
  return DFBR_FALSE;
}

/* Locate the nearest ancestor that declared `background-clip: text`
 * along with a linear-gradient on background(-image). The gradient
 * reference box is the ancestor's padding box - per spec, the text
 * fill is clipped to the glyph silhouettes and colored by the image
 * as if it were filling that padding-box-sized rect. Returns the
 * gradient on match; writes the rect out in OutX..OutH. */
static const DfBrCssGradient *
FindTextGradientAncestor (const DfBrLayoutBox *Box,
                          f32 *OutX, f32 *OutY, f32 *OutW, f32 *OutH,
                          DfBrCssColor *OutCurrentColor)
{
  const DfBrLayoutBox *A;
  if (Box == DFBR_NULL) return DFBR_NULL;
  for (A = Box; A != DFBR_NULL; A = A->Parent) {
    const DfBrCssComputedStyle *S = A->Style;
    if (S == DFBR_NULL) continue;
    if (S->BackgroundClip == DFBR_CSS_BGCLIP_TEXT &&
        S->BackgroundGradient != DFBR_NULL)
    {
      *OutX = A->ContentRect.X - A->Padding.Left;
      *OutY = A->ContentRect.Y - A->Padding.Top;
      *OutW = A->ContentRect.W + A->Padding.Left + A->Padding.Right;
      *OutH = A->ContentRect.H + A->Padding.Top  + A->Padding.Bottom;
      *OutCurrentColor = S->Color;
      return S->BackgroundGradient;
    }
  }
  return DFBR_NULL;
}

/* Locate the nearest ancestor that declared `background-clip: text`
 * with a SOLID background-color (and no gradient). In that case the
 * text idiom reduces to "paint every glyph in the background color" -
 * equivalent to normal text fill with that color, just with the box
 * fill suppressed. Returns the color on match; writes FALSE to *Found
 * on miss. This is the fallback path for authors who write
 *   background: #0a84ff; background-clip: text; color: transparent;
 * instead of a gradient. */
static DfBrCssColor
FindTextSolidColorAncestor (const DfBrLayoutBox *Box, b32 *Found)
{
  const DfBrLayoutBox *A;
  DfBrCssColor         Zero = {0, 0, 0, 0};
  *Found = DFBR_FALSE;
  if (Box == DFBR_NULL) return Zero;
  for (A = Box; A != DFBR_NULL; A = A->Parent) {
    const DfBrCssComputedStyle *S = A->Style;
    if (S == DFBR_NULL) continue;
    if (S->BackgroundClip == DFBR_CSS_BGCLIP_TEXT &&
        S->BackgroundGradient == DFBR_NULL &&
        ColorVisible (S->BackgroundColor))
    {
      *Found = DFBR_TRUE;
      return S->BackgroundColor;
    }
  }
  return Zero;
}

static void
PaintTextBox (const DfBrLayoutBox *Tb,
              DfBrBitmap *Bmp,
              DfBrFontEngine *FontEngine)
{
  const DfBrLayoutTextFragment *Tf;
  DfBrFontFace                 *Face = DFBR_NULL;
  DfBrColor                     Color;
  f32                           ClipX=0, ClipY=0, ClipW=0, ClipH=0;
  b32                           HasClip = DFBR_FALSE;
  /* background-clip: text - the gradient that will paint the glyphs
   * if the nearest matching ancestor declared it. */
  const DfBrCssGradient        *TextGrad = DFBR_NULL;
  f32                           GX=0, GY=0, GW=0, GH=0;
  DfBrCssColor                  GradCurrent = {0, 0, 0, 1.0f};
  f32                           Dx=0, Dy=0, Len=1.0f;
  f32                           Sx=0, Sy=0;

  if (Tb == DFBR_NULL || Tb->Style == DFBR_NULL) return;
  if (Tb->FirstTextFragment == DFBR_NULL)        return;

  HasClip = FindClippingAncestor (Tb, &ClipX, &ClipY, &ClipW, &ClipH);

  Color = CssColorToDfBrColor (Tb->Style->Color);
  /* When an author declares `color: transparent` they're signalling
   * "paint the text via the background" (the background-clip: text
   * idiom). Honour gradient text in that case; otherwise fall back
   * to a safe default so the text still appears. */
  TextGrad = FindTextGradientAncestor (Tb,
                                       &GX, &GY, &GW, &GH,
                                       &GradCurrent);
  if (TextGrad != DFBR_NULL) {
    GradientLineFromAngle (TextGrad->AngleRad, GW, GH, &Dx, &Dy, &Len);
    Sx = (GX + GW * 0.5f) - Dx * (Len * 0.5f);
    Sy = (GY + GH * 0.5f) - Dy * (Len * 0.5f);
  } else if (Color.A == 0) {
    /* `color: transparent` with no gradient ancestor. If a
     * `background-clip: text` ancestor has a solid color, wear that
     * as the glyph fill; otherwise fall back to black so the text
     * still reads on-screen. */
    b32           HaveSolid = DFBR_FALSE;
    DfBrCssColor  Solid     = FindTextSolidColorAncestor (Tb, &HaveSolid);
    if (HaveSolid) {
      Color = CssColorToDfBrColor (Solid);
    } else {
      Color = DfBrRgb (0, 0, 0);
    }
  }

  if (FontEngine != DFBR_NULL) {
    Face = DfBrFontEngineResolve (FontEngine,
                                  Tb->Style->FontFamilies,
                                  Tb->Style->FontFamilyCount,
                                  Tb->Style->FontWeight,
                                  MapFontStyle (Tb->Style->FontStyle),
                                  Tb->FontSizePx);
  }

  for (Tf = Tb->FirstTextFragment; Tf != DFBR_NULL; Tf = Tf->Next) {
    DfBrColor Use = Color;
    if (Tf->Len == 0) continue;
    if (HasClip && FragmentIsClipped (Tf, Tb->FontSizePx,
                                      ClipX, ClipY, ClipW, ClipH)) {
      continue;
    }

    if (TextGrad != DFBR_NULL && Face != DFBR_NULL) {
      /* Per-character gradient sampling.
       *
       * Proper background-clip: text needs the font backend to emit
       * per-glyph alpha coverage so each pixel of the glyph can pick
       * up an independent gradient color. We don't have that primitive
       * yet, so we split the fragment into its constituent code points
       * and draw each one individually with the gradient color sampled
       * at that code point's horizontal center. The result is a
       * piecewise-constant approximation of the true gradient: across
       * the five-to-twelve chars of a typical gradient wordmark it is
       * visually indistinguishable from the real thing, and crucially
       * it reaches the last color stop (which the single-sample-per-
       * fragment version never did when a run had only one fragment).
       *
       * We approximate per-char advance as (fragment advance /
       * codepoint count). For the monospace fonts this idiom tends to
       * use ("JetBrains Mono" etc.) this is exact; for proportional
       * fonts it introduces small sub-pixel shifts but kerning is
       * preserved because each character is still shaped against its
       * own face context when drawn. */
      usize CpCount = 0;
      const u8 *P = (const u8 *)Tf->Str;
      const u8 *End = P + Tf->Len;
      const u8 *Q;
      f32 PerAdv;
      f32 Pen = Tf->X;
      for (Q = P; Q < End;) {
        usize Step = 1;
        u8 B = *Q;
        if      ((B & 0x80) == 0x00) Step = 1;
        else if ((B & 0xE0) == 0xC0) Step = 2;
        else if ((B & 0xF0) == 0xE0) Step = 3;
        else if ((B & 0xF8) == 0xF0) Step = 4;
        Q += Step;
        CpCount++;
      }
      if (CpCount == 0) { Pen += Tf->Advance; continue; }
      PerAdv = Tf->Advance / (f32)CpCount;
      for (Q = P; Q < End;) {
        usize Step = 1;
        u8 B = *Q;
        f32 Cx, Cy, T;
        DfBrColor Sc;
        if      ((B & 0x80) == 0x00) Step = 1;
        else if ((B & 0xE0) == 0xC0) Step = 2;
        else if ((B & 0xF0) == 0xE0) Step = 3;
        else if ((B & 0xF8) == 0xF0) Step = 4;
        if (Q + Step > End) Step = (usize)(End - Q);
        Cx = Pen + PerAdv * 0.5f;
        Cy = Tf->BaselineY - Tb->FontSizePx * 0.35f;
        T  = ((Cx - Sx) * Dx + (Cy - Sy) * Dy) / Len;
        Sc = SampleGradient (TextGrad, GradCurrent, T);
        DfBrFontFaceDrawUtf8 (Face, (const char *)Q, Step, Bmp,
                              Pen, Tf->BaselineY, Sc);
        Pen += PerAdv;
        Q   += Step;
      }
      continue;
    }

    if (TextGrad != DFBR_NULL) {
      /* No font backend - fall back to a single-color underline using
       * the fragment-center gradient sample. */
      f32 Cx = Tf->X + Tf->Advance * 0.5f;
      f32 Cy = Tf->BaselineY - Tb->FontSizePx * 0.35f;
      f32 T  = ((Cx - Sx) * Dx + (Cy - Sy) * Dy) / Len;
      Use = SampleGradient (TextGrad, GradCurrent, T);
    }

    if (Face != DFBR_NULL) {
      DfBrFontFaceDrawUtf8 (Face, Tf->Str, Tf->Len, Bmp,
                            Tf->X, Tf->BaselineY, Use);
    } else {
      FillRect (Bmp, Tf->X, Tf->BaselineY - 1.0f, Tf->Advance, 1.0f, Use);
    }
  }
}

/* ------------------------------------------------------------------
 *  Stacking-context aware paint (CSS 2.1 Appendix E, simplified)
 *
 *  A box is treated as a "layer root" if it's positioned
 *  (relative / absolute / fixed / sticky). This covers the common
 *  case where authors use `position: absolute` + `z-index: N` to
 *  lift boxes above in-flow siblings, and makes sibling z-indexes
 *  sort correctly. Full Appendix E-style handling of opacity,
 *  transforms, and cross-context z-sort is future work - see the
 *  paint roadmap at the top of this file.
 *
 *  Paint order of a layer L (per CSS 2.1):
 *    1. Backgrounds + borders of L's root box itself
 *    2. Descendant layers with z-index < 0   (sorted asc, stable)
 *    3. In-flow block bg+border of descendants that aren't themselves
 *       in a deeper layer (tree order)
 *    4. In-flow floats (v1: folded into step 3, since floats carry
 *       backgrounds/borders just like other blocks)
 *    5. In-flow text + replaced content (tree order)
 *    6. Descendant layers with z-index auto or 0 (tree order)
 *    7. Descendant layers with z-index > 0     (sorted asc, stable)
 * -----------------------------------------------------------------*/

static b32
IsLayerRoot (const DfBrLayoutBox *B)
{
  if (B == DFBR_NULL || B->Style == DFBR_NULL) return DFBR_FALSE;
  return (b32)(B->Style->Position != DFBR_CSS_POSITION_STATIC);
}

/* Floats get their own paint phase between in-flow block backgrounds
 * and in-flow foregrounds (CSS 2.1 Appendix E step 4). They aren't
 * layer roots in the z-index sense - they don't take a z-index - but
 * they must be excluded from the normal in-flow walk so that following
 * siblings' backgrounds don't paint over them. */
static b32
IsFloat (const DfBrLayoutBox *B)
{
  if (B == DFBR_NULL || B->Style == DFBR_NULL) return DFBR_FALSE;
  return (b32)(B->Style->Float != DFBR_CSS_FLOAT_NONE);
}

/* Paints bg + border for Box if it's a block-level container, or an
 * inline whose line-packer has already populated a non-zero
 * ContentRect (inline-block, or any decorated inline with
 * padding/border/bg). Skipped for text boxes and empty inlines.
 * Called for a single box only - does not recurse. */
static void
PaintBoxBgBorder (const DfBrLayoutBox *Box, DfBrBitmap *Bmp)
{
  if (Box == DFBR_NULL) return;
  if (Box->Kind == DFBR_LAYOUT_BOX_BLOCK      ||
      Box->Kind == DFBR_LAYOUT_BOX_ANON_BLOCK ||
      Box->Kind == DFBR_LAYOUT_BOX_REPLACED)
  {
    PaintBoxShadows (Box, Bmp, DFBR_FALSE);
    PaintBackground (Box, Bmp);
    PaintBoxShadows (Box, Bmp, DFBR_TRUE);
    PaintBorders    (Box, Bmp);
    return;
  }
  /* Inline decorations: CSS 2.1 §8.4 says padding/border/background
   * apply to inlines too (typically along the line the inline sits
   * on). Our layout pass commits a content-box rect for any inline
   * with a non-empty start-to-end extent, so we can safely treat
   * these exactly like block backgrounds. Without this branch, a
   * `.pill` badge or any <span style="border:1px solid"> inline
   * renders as glyphs with no box around them. */
  if (Box->Kind == DFBR_LAYOUT_BOX_INLINE &&
      Box->ContentRect.W > 0.0f && Box->ContentRect.H > 0.0f)
  {
    PaintBoxShadows (Box, Bmp, DFBR_FALSE);
    PaintBackground (Box, Bmp);
    PaintBoxShadows (Box, Bmp, DFBR_TRUE);
    PaintBorders    (Box, Bmp);
  }
}

/* Paints foreground (text or replaced content) for a single box.
 * Does not recurse. */
static void
PaintBoxForeground (const DfBrLayoutBox *Box, DfBrBitmap *Bmp,
                    DfBrFontEngine *FontEngine, b32 ShowReplaced)
{
  if (Box == DFBR_NULL) return;
  if (Box->Kind == DFBR_LAYOUT_BOX_TEXT) {
    PaintTextBox (Box, Bmp, FontEngine);
  } else if (Box->Kind == DFBR_LAYOUT_BOX_REPLACED) {
    const DfBrHtmlNode *Dom = Box->DomNode;
    if (IsSvgTag (Dom)) {
      PaintSvgReplaced (Box, Bmp);
    } else if (Dom != DFBR_NULL &&
               (Dom->Tag == DFBR_HTML_TAG_INPUT   ||
                Dom->Tag == DFBR_HTML_TAG_BUTTON  ||
                Dom->Tag == DFBR_HTML_TAG_SELECT  ||
                Dom->Tag == DFBR_HTML_TAG_TEXTAREA)) {
      PaintInputControl (Box, Bmp, FontEngine);
    } else if (Box->ImagePixels != DFBR_NULL) {
      PaintDecodedImage (Box, Bmp);
    } else if (ShowReplaced) {
      PaintReplacedPlaceholder (Box, Bmp);
    }
  }
}

/* Recurse through Box's in-flow subtree painting bg+border, but do
 * NOT descend into layer roots or floats (those get their own paint
 * phase later). */
static void
PaintInFlowBg (const DfBrLayoutBox *Box, DfBrBitmap *Bmp)
{
  const DfBrLayoutBox *C;
  if (Box == DFBR_NULL) return;
  PaintBoxBgBorder (Box, Bmp);
  for (C = Box->FirstChild; C != DFBR_NULL; C = C->NextSibling) {
    if (IsLayerRoot (C) || IsFloat (C)) continue;
    PaintInFlowBg (C, Bmp);
  }
}

/* Same as PaintInFlowBg but for text + replaced content. */
static void
PaintInFlowFg (const DfBrLayoutBox *Box, DfBrBitmap *Bmp,
               DfBrFontEngine *FontEngine, b32 ShowReplaced)
{
  const DfBrLayoutBox *C;
  if (Box == DFBR_NULL) return;
  PaintBoxForeground (Box, Bmp, FontEngine, ShowReplaced);
  for (C = Box->FirstChild; C != DFBR_NULL; C = C->NextSibling) {
    if (IsLayerRoot (C) || IsFloat (C)) continue;
    PaintInFlowFg (C, Bmp, FontEngine, ShowReplaced);
  }
}

/* Paint a float's entire subtree (bg, border, foreground). Floats
 * themselves can contain further floats and positioned elements;
 * nested floats are painted inline here (they're already excluded
 * from the caller's in-flow walk), and nested layer roots are
 * skipped so they get painted at their own stacking layer. */
static void
PaintFloatSubtree (const DfBrLayoutBox *Box, DfBrBitmap *Bmp,
                   DfBrFontEngine *FontEngine, b32 ShowReplaced)
{
  const DfBrLayoutBox *C;
  if (Box == DFBR_NULL) return;
  PaintBoxBgBorder   (Box, Bmp);
  PaintBoxForeground (Box, Bmp, FontEngine, ShowReplaced);
  for (C = Box->FirstChild; C != DFBR_NULL; C = C->NextSibling) {
    if (IsLayerRoot (C)) continue;
    PaintFloatSubtree (C, Bmp, FontEngine, ShowReplaced);
  }
}

/* Growable list of descendant layer roots collected during a pass. */
typedef struct {
  const DfBrLayoutBox **Items;
  usize                Count;
  usize                Cap;
} LayerList;

static void
LayerListPush (LayerList *L, const DfBrLayoutBox *Box)
{
  if (L->Count >= L->Cap) {
    usize NewCap = (L->Cap == 0) ? 8 : (L->Cap * 2);
    const DfBrLayoutBox **New = (const DfBrLayoutBox **)realloc (
      (void *)L->Items, NewCap * sizeof (*New));
    if (New == DFBR_NULL) return; /* best-effort: drop the layer */
    L->Items = New;
    L->Cap   = NewCap;
  }
  L->Items[L->Count++] = Box;
}

/* Collect direct descendant layer roots of Box (i.e. positioned
 * boxes whose nearest positioned ancestor is Box). Does not descend
 * past a found layer - that layer recursively collects its own. */
static void
CollectChildLayers (const DfBrLayoutBox *Box, LayerList *Out)
{
  const DfBrLayoutBox *C;
  if (Box == DFBR_NULL) return;
  for (C = Box->FirstChild; C != DFBR_NULL; C = C->NextSibling) {
    if (IsLayerRoot (C)) {
      LayerListPush (Out, C);
    } else {
      CollectChildLayers (C, Out);
    }
  }
}

/* Collect floats whose in-flow ancestor chain ends at Box (i.e.
 * floats under Box that aren't inside a nested layer root). The
 * walk stops at layer roots (their floats belong to them) and
 * stops descending past a found float (floats can contain their
 * own floats, but those are painted as part of the parent float's
 * subtree by PaintFloatSubtree). */
static void
CollectChildFloats (const DfBrLayoutBox *Box, LayerList *Out)
{
  const DfBrLayoutBox *C;
  if (Box == DFBR_NULL) return;
  for (C = Box->FirstChild; C != DFBR_NULL; C = C->NextSibling) {
    if (IsLayerRoot (C)) continue;
    if (IsFloat (C)) {
      LayerListPush (Out, C);
      continue;
    }
    CollectChildFloats (C, Out);
  }
}

/* Stable comparator: by z-index ascending, preserving insertion order
 * (which reflects tree order) for ties. */
static void
LayerListSort (LayerList *L)
{
  /* Insertion sort keeps stability without needing qsort_r.  Layer
   * lists are typically small (a few positioned elements) so O(n^2)
   * is a non-issue. */
  usize I, J;
  for (I = 1; I < L->Count; I++) {
    const DfBrLayoutBox *Key = L->Items[I];
    i32 Kz = (Key->Style != DFBR_NULL && !Key->Style->ZIndexAuto)
             ? Key->Style->ZIndex : 0;
    J = I;
    while (J > 0) {
      const DfBrLayoutBox *Prev = L->Items[J - 1];
      i32 Pz = (Prev->Style != DFBR_NULL && !Prev->Style->ZIndexAuto)
               ? Prev->Style->ZIndex : 0;
      if (Pz <= Kz) break;
      L->Items[J] = Prev;
      J--;
    }
    L->Items[J] = Key;
  }
}

static void PaintLayer (const DfBrLayoutBox *Root, DfBrBitmap *Bmp,
                        DfBrFontEngine *FontEngine, b32 ShowReplaced);

/* Paint all descendant layers of Root whose z-index falls in the
 * inclusive [ZMin, ZMax] range. Layers with z-index auto are treated
 * as z=0 for this classification. */
static void
PaintLayerRangeChildren (const DfBrLayoutBox *Root, DfBrBitmap *Bmp,
                         DfBrFontEngine *FontEngine, b32 ShowReplaced,
                         i32 ZMin, i32 ZMax, b32 IncludeAuto)
{
  LayerList L;
  usize     I;

  L.Items = DFBR_NULL; L.Count = 0; L.Cap = 0;
  CollectChildLayers (Root, &L);
  LayerListSort (&L);

  for (I = 0; I < L.Count; I++) {
    const DfBrLayoutBox *B = L.Items[I];
    b32 IsAuto = (B->Style == DFBR_NULL) || B->Style->ZIndexAuto;
    i32 Z      = (IsAuto || B->Style == DFBR_NULL) ? 0 : B->Style->ZIndex;
    if (IsAuto && !IncludeAuto) continue;
    if (Z < ZMin || Z > ZMax) continue;
    PaintLayer (B, Bmp, FontEngine, ShowReplaced);
  }

  free ((void *)L.Items);
}

/* Paint a single stacking layer and all its descendant layers. */
static void
PaintLayer (const DfBrLayoutBox *Root, DfBrBitmap *Bmp,
            DfBrFontEngine *FontEngine, b32 ShowReplaced)
{
  if (Root == DFBR_NULL) return;

  /* 1. Bg + border of the layer root itself. */
  PaintBoxBgBorder (Root, Bmp);

  /* 2. Descendant layers with z-index < 0. */
  PaintLayerRangeChildren (Root, Bmp, FontEngine, ShowReplaced,
                           -2147483647 - 1, -1, DFBR_FALSE);

  /* 3. In-flow bg+border for this layer's subtree (skipping Root
   *    itself - already painted in step 1 - floats, and descendant
   *    layers). */
  {
    const DfBrLayoutBox *C;
    for (C = Root->FirstChild; C != DFBR_NULL; C = C->NextSibling) {
      if (IsLayerRoot (C) || IsFloat (C)) continue;
      PaintInFlowBg (C, Bmp);
    }
  }

  /* 4. Non-positioned floats (CSS 2.1 App. E). Each float paints
   *    its entire subtree before in-flow foreground runs, so
   *    subsequent block backgrounds don't cover them. */
  {
    LayerList F;
    usize     I;
    F.Items = DFBR_NULL; F.Count = 0; F.Cap = 0;
    CollectChildFloats (Root, &F);
    for (I = 0; I < F.Count; I++) {
      PaintFloatSubtree (F.Items[I], Bmp, FontEngine, ShowReplaced);
    }
    free ((void *)F.Items);
  }

  /* 5. In-flow foreground (text + replaced). Root's own foreground
   *    fires first in tree order. */
  PaintBoxForeground (Root, Bmp, FontEngine, ShowReplaced);
  {
    const DfBrLayoutBox *C;
    for (C = Root->FirstChild; C != DFBR_NULL; C = C->NextSibling) {
      if (IsLayerRoot (C) || IsFloat (C)) continue;
      PaintInFlowFg (C, Bmp, FontEngine, ShowReplaced);
    }
  }

  /* 6. Positioned descendants with z-index auto or 0 (in tree order). */
  PaintLayerRangeChildren (Root, Bmp, FontEngine, ShowReplaced,
                           0, 0, DFBR_TRUE);

  /* 7. Positioned descendants with z-index > 0 (sorted asc). */
  PaintLayerRangeChildren (Root, Bmp, FontEngine, ShowReplaced,
                           1, 2147483647, DFBR_FALSE);
}

/* ------------------------------------------------------------------
 *  Public entry
 * -----------------------------------------------------------------*/

DfBrResult
DfBrPaintRender (
  const DfBrLayoutEngine   *Engine,
  DfBrBitmap               *Target,
  const DfBrPaintOptions   *Opts
  )
{
  DfBrPaintOptions       Use;
  const DfBrLayoutBox   *Root;
  DfBrFontEngine        *FontEngine;

  if (Engine == DFBR_NULL || Target == DFBR_NULL || Target->Pixels == DFBR_NULL) {
    return DFBR_ERR_INVALID_ARG;
  }

  Use = (Opts != DFBR_NULL) ? *Opts : DfBrPaintDefaultOptions ();

  Root = DfBrLayoutEngineRoot (Engine);
  if (Root == DFBR_NULL) {
    if (Use.ClearCanvas) ClearAll (Target, Use.CanvasColor);
    return DFBR_OK;
  }

  /* CSS 2.1 §14.2: propagate <html> or <body> background to the
   * canvas so it fills the viewport beyond the element's own box.
   * We look at <html> first; if its background is transparent we
   * fall back to <body>; otherwise keep the caller's canvas color.
   * NOTE: We don't remove the propagated bg from the source box -
   * painting it twice over its own rect is pixel-identical and
   * simpler than threading a "skip bg" flag through the walker. */
  if (Use.ClearCanvas) {
    DfBrColor Canvas = Use.CanvasColor;
    const DfBrLayoutBox *HtmlBox = DFBR_NULL;
    const DfBrLayoutBox *BodyBox = DFBR_NULL;
    const DfBrLayoutBox *C;
    for (C = Root->FirstChild; C != DFBR_NULL; C = C->NextSibling) {
      if (C->DomNode != DFBR_NULL && C->DomNode->Tag == DFBR_HTML_TAG_HTML) {
        HtmlBox = C; break;
      }
    }
    if (HtmlBox != DFBR_NULL) {
      for (C = HtmlBox->FirstChild; C != DFBR_NULL; C = C->NextSibling) {
        if (C->DomNode != DFBR_NULL && C->DomNode->Tag == DFBR_HTML_TAG_BODY) {
          BodyBox = C; break;
        }
      }
    }
    if (HtmlBox != DFBR_NULL && HtmlBox->Style != DFBR_NULL &&
        ColorVisible (HtmlBox->Style->BackgroundColor))
    {
      Canvas = CssColorToDfBrColor (HtmlBox->Style->BackgroundColor);
    } else if (BodyBox != DFBR_NULL && BodyBox->Style != DFBR_NULL &&
               ColorVisible (BodyBox->Style->BackgroundColor))
    {
      Canvas = CssColorToDfBrColor (BodyBox->Style->BackgroundColor);
    }
    ClearAll (Target, Canvas);
  }

  FontEngine = DfBrFontGetDefaultEngine ();

  /* Paint the root (synthetic initial containing block) as the
   * outermost stacking context. PaintLayer recursively descends,
   * walking in CSS 2.1 Appendix E order for bg/border/foreground
   * and z-sorting positioned descendants. */
  PaintLayer (Root, Target, FontEngine, Use.ReplacedPlaceholders);

  return DFBR_OK;
}
