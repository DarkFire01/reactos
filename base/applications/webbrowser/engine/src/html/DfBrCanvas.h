/** @file
  DfBrCanvas.h - RGBA pixel backing store for <canvas> elements and
                 a tiny rasterizer that the JS 2D context binds to.

  Why this module lives in DfBrHtml
  ---------------------------------
  Canvas state is conceptually "per DOM node": authors create a
  <canvas>, call getContext('2d'), and then spend the rest of the
  page's life drawing into that single backing store. Both the layout
  engine (which hands the pixel pointer off to paint) and the DOM
  binding (which mutates pixels from JS) need to reach it, so the
  lowest shared floor in the library graph is DfBrHtml. Putting the
  struct + helpers here avoids a circular link between DfBrLayout and
  DfBrDom.

  Storage lives in DfBrHtmlNode::ScriptData. That slot was reserved
  in DfBrHtml.h for subsystem-owned handles; canvas is the first
  real user. The node is NEVER asked to free it - backings are
  registered on a linked list anchored in the document so a single
  DfBrHtmlDocumentDestroy walk can release every pixel array.

  Pixel format
  ------------
  32-bit RGBA8, row-major, premultiplied = no. Matches the decoder
  format used by DfBrImage and the paint blitter, so a fully-drawn
  canvas can be blitted verbatim as if it were a decoded <img>.

  Color parsing
  -------------
  The rasterizer handles #RGB, #RRGGBB, rgb(a, b, c), rgba(r, g, b,
  a), and a compact named-colors list that covers everything
  real-world canvas code sets. Unknown strings fall back to opaque
  black, which mirrors browsers' lenient behavior.
**/

#ifndef DFBR_CANVAS_H
#define DFBR_CANVAS_H

#include "../common/DfBrTypes.h"
#include "DfBrHtml.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Pixel backing store. Lifetime is bound to the owning document. **/
typedef struct DfBrCanvasBacking {
  u32                        Width;
  u32                        Height;
  u8                        *Pixels;      /* W*H*4 bytes, RGBA8 */
  /* Internal: intrusive list of every backing registered against
     a single document. Consumers may read but should not mutate. */
  struct DfBrCanvasBacking  *DocNext;
} DfBrCanvasBacking;

/** Return the canvas backing for a node, or NULL if none exists yet.
    Read-only - does not allocate. **/
DfBrCanvasBacking *
DfBrCanvasGet (
  const DfBrHtmlNode *Node
  );

/** Ensure that `Node` has a backing of at least `Width` x `Height`.
    If a backing already exists with different dimensions it is
    reallocated (contents preserved up to the min overlap). Pass 0
    for either axis to keep the current size (if any) or fall back
    to HTML-spec defaults: 300 x 150.

    Returns NULL on allocation failure or if `Node` is not an
    element node of tag CANVAS. **/
DfBrCanvasBacking *
DfBrCanvasEnsure (
  DfBrHtmlNode     *Node,
  DfBrHtmlDocument *Doc,
  u32               Width,
  u32               Height
  );

/** Drop every backing anchored to `Doc`. Idempotent. Called from
    DfBrHtmlDocumentDestroy so documents never leak canvas pixels. **/
void
DfBrCanvasDestroyAll (
  DfBrHtmlDocument *Doc
  );

/* ------------------------------------------------------------------
 *  Color parsing
 * -----------------------------------------------------------------*/

/** Parse a CSS color string into R, G, B, A (0..255 each). Returns
    DFBR_TRUE on success, DFBR_FALSE if the input is unrecognised
    (in which case the out args are filled with opaque black, so
    callers can safely ignore the result and still draw). **/
b32
DfBrCanvasParseColor (
  const char *Str,
  u8         *OutR,
  u8         *OutG,
  u8         *OutB,
  u8         *OutA
  );

/* ------------------------------------------------------------------
 *  Rasterizer primitives
 *
 *  Coordinates are floats but get rounded to integer pixel boundaries
 *  on the inside - CanvasRenderingContext2D semantics allow this
 *  ("coordinates are given as floats but pixels are integers").
 *  All ops are no-ops on a NULL backing; out-of-bounds rects are
 *  clipped to the drawable area.
 * -----------------------------------------------------------------*/

/** Fill the rect with (r,g,b,a) using source-over compositing. **/
void
DfBrCanvasFillRect (
  DfBrCanvasBacking *B,
  f32 X, f32 Y, f32 W, f32 H,
  u8 R, u8 G, u8 Bl, u8 A
  );

/** Fill a disc (center CX, CY; radius Rad) with source-over
    compositing; clipped to the backing. **/
void
DfBrCanvasFillCircle (
  DfBrCanvasBacking *B,
  f32 CX, f32 CY, f32 Rad,
  u8 R, u8 G, u8 Bl, u8 A
  );

/** Clear the rect to rgba(0, 0, 0, 0). Unlike fillRect, this
    overwrites the destination completely. **/
void
DfBrCanvasClearRect (
  DfBrCanvasBacking *B,
  f32 X, f32 Y, f32 W, f32 H
  );

/** Stroke the outline of a rect with `LineWidth`. Width is rounded
    up to at least 1 pixel; centered on the rect edges. **/
void
DfBrCanvasStrokeRect (
  DfBrCanvasBacking *B,
  f32 X, f32 Y, f32 W, f32 H,
  f32 LineWidth,
  u8 R, u8 G, u8 Bl, u8 A
  );

/** Fill the entire canvas with (r, g, b, a). Used by save/restore
    workflows that want a known starting state. **/
void
DfBrCanvasFillAll (
  DfBrCanvasBacking *B,
  u8 R, u8 G, u8 Bl, u8 A
  );

/** Copy a RGBA8 block into the canvas starting at (DestX, DestY).
    Src is SrcW*SrcH*4 bytes laid out row-major. Out-of-bounds
    regions are clipped. Ignores alpha and writes bytes verbatim
    (this matches CanvasRenderingContext2D.putImageData semantics). **/
void
DfBrCanvasPutRgba (
  DfBrCanvasBacking *B,
  i32 DestX, i32 DestY,
  const u8 *Src,
  u32 SrcW, u32 SrcH
  );

/** Read a RGBA8 block of SrcW*SrcH pixels starting at (SrcX, SrcY)
    into `Dst` (SrcW*SrcH*4 bytes). Pixels outside the canvas are
    filled with transparent black. **/
void
DfBrCanvasReadRgba (
  const DfBrCanvasBacking *B,
  i32 SrcX, i32 SrcY,
  u8 *Dst,
  u32 SrcW, u32 SrcH
  );

#ifdef __cplusplus
}
#endif

#endif /* DFBR_CANVAS_H */
