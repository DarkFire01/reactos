/** @file
  DfBrLayout.h - box-tree construction and block/inline layout for
                 DefyBrowser.

  Design intent
  =============
  DfBrLayout is the third pillar of the browser engine. Given a parsed
  HTML document and a CSS style context (see DfBrCss), it builds a
  "box tree" in which every formatting context is an explicit box, and
  then runs layout to fill in concrete geometry (content / padding /
  border / margin rects in CSS pixels) for every box. Paint will walk
  this tree; it never touches the DOM directly.

  The design deliberately mirrors real browser engines so the library
  can grow into a full Chrome-replacement layout engine without a
  ground-up rewrite:

    1. **Separate box tree.** The DOM + computed styles produce a new
       tree of DfBrLayoutBox nodes, not annotations on DfBrHtmlNode.
       Anonymous boxes (strut, line box, inline wrapper) have no DOM
       counterpart and need to live somewhere; the box tree is that
       place. The box keeps a back-pointer to its originating
       DfBrHtmlNode (or NULL for anonymous boxes).

    2. **Two-phase pipeline.**
         build   : DOM + computed styles  ->  box tree (unsized).
         layout  : box tree + viewport    ->  box tree with geometry.
       These are separate so layout can be rerun on resize without
       rebuilding the box tree.

    3. **Normal-flow block + inline first.** v1 implements:
         - block formatting context
         - anonymous block wrapping around inline children
         - inline formatting context (line boxes, simple text metrics)
         - margin / border / padding resolution
         - width: auto, width: <length>, width: <percent>
         - height: auto (from content) or explicit
         - static positioning only
       Float / flex / grid / absolute positioning are tracked as
       follow-ups.

    4. **Length resolution.** Percent / em / rem / vw / vh are
       resolved per-axis against the containing block at layout time.
       em/rem/% for font-size were already resolved to absolute px by
       the cascade; layout only has to deal with box-axis lengths.

    5. **Text as fragments.** Inline text is broken into runs, each
       with an approximate advance in CSS pixels. v1 uses a very
       crude "8 px per ASCII character" metric - good enough to ship
       a box tree that paint can draw until stb_truetype lands.

    6. **Arena allocation**, same convention as DfBrHtml / DfBrCss.

    7. **Engine-agnostic.** This header pulls in DfBrTypes.h,
       DfBrHtml.h, and DfBrCss.h only; no platform, no paint types.

  Deliberate v1 gaps (tracked in the roadmap):
    - Float / clear (parsed + tracked, not laid out).
    - Flex / grid (boxes keep their display type, laid out as block).
    - Absolute / fixed / sticky positioning (static only).
    - Transforms / opacity / clipping.
    - Writing modes other than horizontal-tb.
    - Bidi resolution (we assume LTR).
    - Real text shaping + font metrics (stb_truetype coming).
    - Tables (boxes are built, laid out as nested blocks).
**/

#ifndef DFBR_LAYOUT_H_
#define DFBR_LAYOUT_H_

#include "../common/DfBrTypes.h"
#include "../html/DfBrHtml.h"
#include "../css/DfBrCss.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------
 *  Geometry types
 *
 *  All lengths are CSS pixels (f32). The layout engine's internal
 *  resolution is subpixel; paint rounds at the very end.
 * -----------------------------------------------------------------*/

typedef struct {
  f32 X;
  f32 Y;
  f32 W;
  f32 H;
} DfBrLayoutRect;

typedef struct {
  f32 Top;
  f32 Right;
  f32 Bottom;
  f32 Left;
} DfBrLayoutEdges;

/* ------------------------------------------------------------------
 *  Box kinds
 *
 *  A DfBrLayoutBox is *either* a DOM-backed box (typical element or
 *  text node) or an anonymous wrapper that the spec inserts to keep
 *  formatting contexts separate (eg. a block wrapping a run of inline
 *  siblings whose parent is itself a block).
 * -----------------------------------------------------------------*/

typedef enum {
  /* A box that participates in block formatting (block / list-item /
   * flex / table-* all collapse to this for v1). */
  DFBR_LAYOUT_BOX_BLOCK,

  /* A box that participates in inline formatting. Includes both
   * element inlines (<span>, <a>, <em>, ...) and text nodes. */
  DFBR_LAYOUT_BOX_INLINE,

  /* A box that is a leaf text run. Always a child of an inline box
   * (potentially an anonymous one). TextData / TextLen point into
   * the original DOM string, after CSS whitespace collapsing. */
  DFBR_LAYOUT_BOX_TEXT,

  /* Anonymous block box wrapping a run of inline siblings. Only
   * exists to keep a block container from having mixed block + inline
   * direct children. */
  DFBR_LAYOUT_BOX_ANON_BLOCK,

  /* Replaced content (<img>, <video>, <canvas>, ...). v1 treats these
   * as empty inline boxes with a declared width / height if the
   * element supplied one. */
  DFBR_LAYOUT_BOX_REPLACED
} DfBrLayoutBoxKind;

/* ------------------------------------------------------------------
 *  Box
 *
 *  The box tree node. Child / sibling links mirror the DOM's to let
 *  a single recursive walk handle either tree. Geometry fields are
 *  filled in by DfBrLayoutRun; they are zero until then.
 * -----------------------------------------------------------------*/

typedef struct DfBrLayoutBox         DfBrLayoutBox;
typedef struct DfBrLayoutTextFragment DfBrLayoutTextFragment;

/* A single positioned glyph run inside a DFBR_LAYOUT_BOX_TEXT. The
 * inline layout emits one of these per word (non-whitespace span)
 * it places on a line, so paint can draw wrapped / multi-line text
 * correctly without redoing line-packing.
 *
 * (X, BaselineY) is the pen origin in viewport coordinates for the
 * first glyph; Str / Len reference the original text data (not the
 * arena; no nul-termination). Advance is the run's total horizontal
 * extent in CSS pixels, as measured by the font engine. */
struct DfBrLayoutTextFragment {
  f32                       X;
  f32                       BaselineY;
  const char               *Str;
  usize                     Len;
  f32                       Advance;
  DfBrLayoutTextFragment   *Next;
};

struct DfBrLayoutBox {
  DfBrLayoutBoxKind            Kind;

  /* DOM / style back-pointers. DomNode is NULL for anonymous boxes.
   * Style is NULL for anonymous boxes (they inherit via Parent). */
  const DfBrHtmlNode          *DomNode;
  const DfBrCssComputedStyle  *Style;

  /* Text payload (DFBR_LAYOUT_BOX_TEXT only). Points into the DOM
   * arena; already whitespace-collapsed per CSS. */
  const char                  *TextData;
  usize                        TextLen;

  /* Tree links. */
  DfBrLayoutBox               *Parent;
  DfBrLayoutBox               *FirstChild;
  DfBrLayoutBox               *LastChild;
  DfBrLayoutBox               *NextSibling;
  DfBrLayoutBox               *PrevSibling;

  /* Geometry - all in CSS pixels, relative to the viewport origin
   * (top-left of the initial containing block). Margins collapse
   * through the normal CSS rules. */
  DfBrLayoutRect               ContentRect;   /* the content box */
  DfBrLayoutEdges              Padding;
  DfBrLayoutEdges              Border;
  DfBrLayoutEdges              Margin;

  /* Intrinsic / used font size for this box, in CSS pixels. Already
   * resolved by the cascade; cached here for convenience. */
  f32                          FontSizePx;

  /* Baseline offset from ContentRect.Y for inline boxes (line metrics). */
  f32                          Baseline;

  /* Text-run-specific advance (Kind == TEXT). 0 until laid out. */
  f32                          TextAdvance;

  /* Text fragments (Kind == TEXT only). Linked list in paint order;
   * one entry per word placed on a line. NULL for non-text boxes
   * and for text boxes that never got painted (zero-length or
   * whitespace-only after collapsing). Arena-allocated; lifetime
   * matches the engine. */
  DfBrLayoutTextFragment      *FirstTextFragment;
  DfBrLayoutTextFragment      *LastTextFragment;

  /* Decoded replaced-content bitmap (Kind == REPLACED, typically an
   * <img> whose src resolved to a data: URL). RGBA8, row-major,
   * top-down. NULL if the element isn't an image, decoding failed,
   * or the bytes haven't been fetched (http/https). Owned by the
   * engine; freed in DfBrLayoutEngineDestroy. */
  const u8                    *ImagePixels;
  u32                          ImageW;
  u32                          ImageH;

  /* Static position (CSS 2.1 §10.3.7 / §10.6.4) captured during the
   * normal-flow pass for `position: absolute` boxes. When left /
   * right / top / bottom all resolve to auto, the positioning pass
   * falls back to these coordinates instead of the containing-block
   * origin, matching the spec's "the position the element would have
   * had in the normal flow" rule. HasStaticPos is DFBR_FALSE until
   * the in-flow walker records the location. */
  f32                          StaticX;
  f32                          StaticY;
  b32                          HasStaticPos;
};

/* ------------------------------------------------------------------
 *  Viewport / input to layout
 * -----------------------------------------------------------------*/

typedef struct {
  f32  WidthPx;
  f32  HeightPx;
  /* Default font size for rem/em resolution when the document has no
   * cascaded font-size on the root. Normally 16px. */
  f32  DefaultFontSizePx;
} DfBrLayoutViewport;

/* ------------------------------------------------------------------
 *  Engine context (opaque)
 *
 *  Owns the arena that holds every box. The box tree remains valid
 *  until DfBrLayoutEngineDestroy. Re-running layout on a different
 *  viewport reuses the same boxes.
 * -----------------------------------------------------------------*/

typedef struct DfBrLayoutEngine DfBrLayoutEngine;

/* ------------------------------------------------------------------
 *  Public API
 * -----------------------------------------------------------------*/

/** Build a box tree from a styled document.
    The style context must have been produced by DfBrCssApplyToDocument
    against the same Doc. Returns NULL on OOM.

    The returned engine owns the box tree; destroy via
    DfBrLayoutEngineDestroy when done.
 **/
DfBrLayoutEngine *
DfBrLayoutBuild (
  DfBrHtmlDocument     *Doc,
  DfBrCssStyleContext  *Sc
  );

/** Same as DfBrLayoutBuild but also records the document's base URL
    (the URL the HTML was loaded from, post-redirect). Relative
    `<img src>` references encountered during build are resolved
    against this base when fetching remote images. @p BaseUrl may be
    NULL for unbased documents. */
DfBrLayoutEngine *
DfBrLayoutBuildWithBase (
  DfBrHtmlDocument     *Doc,
  DfBrCssStyleContext  *Sc,
  const char           *BaseUrl
  );

/** Run block + inline layout on the box tree with the given viewport.
    Can be called multiple times with different viewports; box
    geometry is rewritten each call. **/
void
DfBrLayoutRun (
  DfBrLayoutEngine          *Engine,
  const DfBrLayoutViewport  *Viewport
  );

/** Tear down the box tree + arena. **/
void  DfBrLayoutEngineDestroy (DfBrLayoutEngine *Engine);

/** Root box accessor. The root corresponds to the initial containing
    block (viewport) and is always a synthetic DFBR_LAYOUT_BOX_BLOCK.
    Its FirstChild is the `<html>` element's box. **/
const DfBrLayoutBox *DfBrLayoutEngineRoot (const DfBrLayoutEngine *Engine);

/** Traversal: pre-order walk over the whole box tree. Pass NULL for
    Cur to start; returns NULL when done. **/
const DfBrLayoutBox *
DfBrLayoutBoxNextPreorder (const DfBrLayoutBox *Root,
                           const DfBrLayoutBox *Cur);

/** Hit-test the box tree at the point (X, Y) in viewport (page)
    coordinates. Returns the deepest box whose border-rect contains
    the point, preferring later-in-document siblings at the same
    depth (matches painting order: last-drawn is hit first).

    Text fragment boxes are skipped because they rarely carry useful
    handler targets; callers that want fragment-level hit testing
    should walk the returned box's children.

    Returns NULL when the point lies outside the root's rect. **/
const DfBrLayoutBox *
DfBrLayoutHitTest (
  const DfBrLayoutEngine  *Engine,
  f32                      X,
  f32                      Y
  );

/** First layout box in preorder whose DomNode equals @p Node, or NULL.
    Enough for replaced elements (canvas, img) and single-box elements;
    inline splits may have multiple boxes per node (returns first). **/
const DfBrLayoutBox *
DfBrLayoutBoxForDomNode (
  const DfBrLayoutEngine *Engine,
  const DfBrHtmlNode     *Node
  );

/* ------------------------------------------------------------------
 *  Debug dumping
 * -----------------------------------------------------------------*/

/** Indented text dump of the box tree: one line per box, showing the
    kind, the originating DOM tag (if any), and the ContentRect /
    margin / padding / border in CSS pixels.

    The returned string is heap-allocated and NUL-terminated. Free
    with DfBrLayoutFreeDump. **/
char *DfBrLayoutDump (const DfBrLayoutEngine *Engine, usize *OutLen);
void  DfBrLayoutFreeDump (char *S);

#ifdef __cplusplus
}
#endif

#endif /* DFBR_LAYOUT_H_ */
