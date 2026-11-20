/** @file
  DfBrPaint.h - rasterize a DfBrLayout box tree into a BGRA bitmap.

  Design intent
  =============
  DfBrPaint is the fourth pillar of the engine. Input: a laid-out
  DfBrLayoutEngine (geometry already resolved) and a target
  DfBrBitmap. Output: pixels.

  Paint walks the box tree and produces the following layers, in
  CSS paint order:

    1. A single full-viewport "canvas" color (white).
    2. Per-box backgrounds (solid color; images come with stb_image).
    3. Per-box borders (solid style, 1px+ widths, individual sides).
    4. Text runs (glyph rasterization via DfBrFont / DirectWrite).
    5. Replaced-element placeholders (checkerboard with alt-text hint;
       real image + media decode arrives later).

  v1 scope:
    * No stacking contexts, z-index, transforms, filters, shadows.
    * No border-radius, border-image, border-styles other than solid.
    * No gradients; backgrounds are solid colors.
    * No text shadows, decoration-thickness, underline-offset tuning.
    * No image decoding (placeholders only).

  The module is deliberately shaped so that a future port can swap
  DfBrPaint's scanline renderer (currently CPU into DfBrBitmap) for a
  GPU path (D2D, D3D11 triangle fill, Skia, etc.) without touching
  callers. Layout + DOM stay upstream.
**/

#ifndef DFBR_PAINT_H_
#define DFBR_PAINT_H_

#include "../common/DfBrTypes.h"
#include "../core/DfBrBitmap.h"
#include "../layout/DfBrLayout.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Paint options. Keeping a struct so future knobs (device-pixel
    ratio, debug overlay, invalid-rect, transparent canvas) don't
    require an ABI break. **/
typedef struct {
  /** Fill the whole target with this color before painting. If
      ClearAlpha == 0 the target is not pre-cleared (useful when
      layering over an existing background). Default RGB = 255/255/255,
      Alpha = 255. **/
  DfBrColor  CanvasColor;
  b32        ClearCanvas;

  /** If non-zero, draw replaced-element placeholders (checkerboard
      + alt text) so you can see where images would go. Default
      DFBR_TRUE. **/
  b32        ReplacedPlaceholders;
} DfBrPaintOptions;

/** Fill a newly allocated/opened DfBrPaintOptions with defaults. **/
DfBrPaintOptions DfBrPaintDefaultOptions (void);

/** Paint the laid-out box tree into Target. The target's Width /
    Height must match the viewport used in the most recent
    DfBrLayoutRun; otherwise the result is clipped / letterboxed. **/
DfBrResult
DfBrPaintRender (
  const DfBrLayoutEngine   *Engine,
  DfBrBitmap               *Target,
  const DfBrPaintOptions   *Opts      /* may be NULL for defaults */
  );

#ifdef __cplusplus
}
#endif

#endif /* DFBR_PAINT_H_ */
