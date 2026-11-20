/** @file
  DfBrLayoutInternal.h - private shared state for the DfBrLayout
                         translation units.
**/

#ifndef DFBR_LAYOUT_INTERNAL_H_
#define DFBR_LAYOUT_INTERNAL_H_

#include "DfBrLayout.h"
#include "../image/DfBrImage.h"

/* ------------------------------------------------------------------
 *  Arena
 * -----------------------------------------------------------------*/

typedef struct DfBrLayoutArenaChunk {
  u8                            *Data;
  usize                          Used;
  usize                          Capacity;
  struct DfBrLayoutArenaChunk   *Next;
} DfBrLayoutArenaChunk;

typedef struct {
  DfBrLayoutArenaChunk *First;
  DfBrLayoutArenaChunk *Current;
} DfBrLayoutArena;

void *DfBrLayoutArenaAlloc      (DfBrLayoutArena *A, usize Bytes);
char *DfBrLayoutArenaStrdupN    (DfBrLayoutArena *A, const char *Src, usize Len);
void  DfBrLayoutArenaDispose    (DfBrLayoutArena *A);

/* ------------------------------------------------------------------
 *  Floats
 *
 *  v1 scope (CSS 2.1, section 9.5):
 *    - A box with `float: left` / `right` is taken out of the normal
 *      flow but still stays within its containing block's "block
 *      formatting context". The current block layout treats the
 *      synthetic root box as the single BFC - good enough for the
 *      majority of real-world pages and for bigger.html.
 *    - Preceding block / inline content places the float as high as
 *      possible at the current pen Y, anchored to the left or right
 *      inner edge of the containing block, shifting downward when
 *      existing floats don't leave enough room.
 *    - Subsequent line boxes in the same BFC shrink to the horizontal
 *      band between the active left / right floats at each line's Y.
 *    - `clear: left | right | both` advances the current Y past the
 *      bottom of all matching active floats before placing the box.
 *
 *  Known v1 gaps:
 *    - No new BFCs established by overflow/display/position; all
 *      floats accumulate in one list.
 *    - Block boxes do not grow to contain their floated descendants
 *      (no automatic clearfix).
 *    - No shrink-to-fit for `float` + `width: auto`.
 * -----------------------------------------------------------------*/

typedef enum {
  DFBR_LAYOUT_FLOAT_SIDE_LEFT,
  DFBR_LAYOUT_FLOAT_SIDE_RIGHT
} DfBrLayoutFloatSide;

typedef struct DfBrLayoutFloat {
  DfBrLayoutBox              *Box;
  DfBrLayoutFloatSide         Side;
  /* Margin-box extents in viewport coordinates. */
  f32                         OuterLeft;
  f32                         OuterTop;
  f32                         OuterRight;
  f32                         OuterBottom;
  struct DfBrLayoutFloat     *Next;
} DfBrLayoutFloat;

/* ------------------------------------------------------------------
 *  Engine
 * -----------------------------------------------------------------*/

struct DfBrLayoutEngine {
  DfBrLayoutArena       Arena;
  DfBrHtmlDocument     *Doc;
  DfBrCssStyleContext  *Sc;
  DfBrLayoutBox        *Root;             /* initial containing block */
  DfBrLayoutViewport    LastViewport;

  /* Cached values set at the start of DfBrLayoutRun so length
   * resolution helpers don't have to chase pointers. */
  f32                   ViewportW;
  f32                   ViewportH;
  f32                   RootFontSizePx;   /* for rem */

  /* Active floats accumulated during the in-flow pass. Cleared at
   * the start of every DfBrLayoutRun. */
  DfBrLayoutFloat      *FloatsHead;
  DfBrLayoutFloat      *FloatsTail;

  /* Decoded images, kept in a side list so DfBrLayoutEngineDestroy
   * can free stb_image malloc'd pixel buffers that the arena
   * doesn't own. The layout box stores a const pointer into one of
   * these entries. */
  struct DfBrLayoutImageNode {
    DfBrImageDecoded            Img;
    struct DfBrLayoutImageNode *Next;
  }                    *Images;

  /* Base URL the document was loaded from, used when resolving
   * relative <img src> references through DfBrImageDecodeFromUrl.
   * Owned by the engine (strdup'd in DfBrLayoutBuild); freed in
   * DfBrLayoutEngineDestroy. May be NULL. */
  char                 *BaseUrl;
};

/* ------------------------------------------------------------------
 *  Box creation
 * -----------------------------------------------------------------*/

DfBrLayoutBox *
DfBrLayoutBoxNew (
  DfBrLayoutEngine            *E,
  DfBrLayoutBoxKind            Kind,
  const DfBrHtmlNode          *Dom,
  const DfBrCssComputedStyle  *Style
  );

void
DfBrLayoutBoxAppend (DfBrLayoutBox *Parent, DfBrLayoutBox *Child);

/* ------------------------------------------------------------------
 *  Length resolution (DfBrLayoutLength.c)
 *
 *  ResolveLengthPx: returns the best-effort px value for a CSS
 *  length, resolving em/rem/vw/vh/percent against the given context.
 *  `PercentBase` is the box-axis size to resolve percents against
 *  (containing-block content width/height); pass 0 if percent should
 *  be treated as 0 (spec fallback for "auto"-ish contexts).
 *
 *  Auto returns 0. Callers check DfBrCssLengthIsAuto(L) first when
 *  they need to distinguish "auto" from an explicit zero.
 * -----------------------------------------------------------------*/

f32
DfBrLayoutResolveLengthPx (
  const DfBrLayoutEngine  *E,
  const DfBrLayoutBox     *Box,       /* for em resolution (font-size) */
  const DfBrCssLength     *L,
  f32                       PercentBase
  );

/* Resolve the used border width for a single side of `Box`, honoring
 * CSS 2.1 §8.5.2: when the side's border-style is `none` or `hidden`,
 * the used border width is forced to 0 regardless of the declared
 * width. Callers should use this for any site that takes a border
 * width into account for box geometry. `B` is `&S->BorderTop` /
 * `&S->BorderRight` / `&S->BorderBottom` / `&S->BorderLeft` on the
 * box's computed style. */
f32
DfBrLayoutResolveBorderPx (
  const DfBrLayoutEngine  *E,
  const DfBrLayoutBox     *Box,
  const DfBrCssBorder     *B,
  f32                       PercentBase
  );

/* ------------------------------------------------------------------
 *  Block + inline layout (DfBrLayoutBlock.c, DfBrLayoutInline.c)
 * -----------------------------------------------------------------*/

void
DfBrLayoutBlockBox (
  DfBrLayoutEngine  *E,
  DfBrLayoutBox     *Box,
  f32                ContainingBlockWidth,
  f32                ContentLeft,
  f32                ContentTop
  );

/* True when `Box` is out of the normal flow (position: absolute or
 * fixed). v1 treats relative / sticky as in-flow; they are handled
 * by shifting paint coordinates after layout.
 *
 * Position applies to real element boxes only. Text boxes and
 * anonymous wrappers share their parent's computed style by pointer
 * so a naive `Style->Position` check would incorrectly classify
 * children of positioned blocks as positioned too. */
static inline b32
DfBrLayoutIsOutOfFlow (const DfBrLayoutBox *Box) {
  if (Box == DFBR_NULL || Box->Style == DFBR_NULL) return DFBR_FALSE;
  if (Box->Kind == DFBR_LAYOUT_BOX_TEXT ||
      Box->Kind == DFBR_LAYOUT_BOX_ANON_BLOCK) return DFBR_FALSE;
  if (Box->DomNode == DFBR_NULL ||
      Box->DomNode->Kind != DFBR_HTML_NODE_ELEMENT) return DFBR_FALSE;
  return (b32)(Box->Style->Position == DFBR_CSS_POSITION_ABSOLUTE ||
               Box->Style->Position == DFBR_CSS_POSITION_FIXED);
}

/* Positioning pass (DfBrLayoutPositioned.c). Walks the box tree and
 * lays out every absolutely- or fixed-positioned box using its
 * containing block (nearest positioned ancestor for absolute, the
 * viewport for fixed). Called after normal-flow layout has filled
 * geometry for in-flow ancestors. */
void
DfBrLayoutPlacePositioned (DfBrLayoutEngine *E);

/* True when `Box` is a CSS float (`float: left | right`). Position:
 * absolute / fixed suppress float per the spec. Shared anonymous
 * wrappers and text boxes inherit style pointers; they are never
 * "floats" on their own. */
static inline b32
DfBrLayoutIsFloat (const DfBrLayoutBox *Box) {
  if (Box == DFBR_NULL || Box->Style == DFBR_NULL) return DFBR_FALSE;
  if (Box->Kind == DFBR_LAYOUT_BOX_TEXT ||
      Box->Kind == DFBR_LAYOUT_BOX_ANON_BLOCK) return DFBR_FALSE;
  if (Box->DomNode == DFBR_NULL ||
      Box->DomNode->Kind != DFBR_HTML_NODE_ELEMENT) return DFBR_FALSE;
  if (Box->Style->Position == DFBR_CSS_POSITION_ABSOLUTE ||
      Box->Style->Position == DFBR_CSS_POSITION_FIXED) return DFBR_FALSE;
  return (b32)(Box->Style->Float == DFBR_CSS_FLOAT_LEFT ||
               Box->Style->Float == DFBR_CSS_FLOAT_RIGHT);
}

/* ------------------------------------------------------------------
 *  Float pass (DfBrLayoutFloat.c)
 * -----------------------------------------------------------------*/

/** Reset the engine's float list. Called at the top of DfBrLayoutRun
    so successive layout passes start fresh. */
void
DfBrLayoutFloatsReset (DfBrLayoutEngine *E);

/** Place `Box` (which must satisfy DfBrLayoutIsFloat) inside its
    containing block's content band. Determines outer geometry, picks
    the first Y >= YHint at which the float fits horizontally, runs
    the block layout for the float's subtree at that origin, and
    registers the float so subsequent lines / floats can consult it.

    The containing block is described by `CbLeft` / `CbRight` (the
    inner edges of the container's content box). Returns the Y at
    which the float was anchored (may be > YHint if earlier floats
    pushed it down). */
f32
DfBrLayoutFloatPlace (DfBrLayoutEngine *E,
                      DfBrLayoutBox    *Box,
                      f32               CbLeft,
                      f32               CbRight,
                      f32               YHint);

/** Compute the horizontal band available for inline content at a
    given Y inside the containing block [CbLeft, CbRight]. Walks the
    engine's float list; each active float that straddles Y clips the
    band from its side. Writes the inner band edges to `*OutLeft` and
    `*OutRight`. */
void
DfBrLayoutFloatsBandAt (const DfBrLayoutEngine *E,
                        f32 Y,
                        f32 CbLeft,
                        f32 CbRight,
                        f32 *OutLeft,
                        f32 *OutRight);

/** Return a Y at or below `YHint` such that:
      - any float matching `C` with outer bottom > YHint has been
        cleared: the returned Y >= that bottom.
    v1 treats `clear` strictly on the ancestor's single BFC. */
f32
DfBrLayoutFloatsClear (const DfBrLayoutEngine *E,
                       f32 YHint,
                       DfBrCssClear C);

/** Bottom of the lowest active float in the engine, or YHint if
    nothing is active. Used to grow containers that establish a new
    BFC (v1 stub: called by the root block so the viewport height
    reflects floats even when content is empty). */
f32
DfBrLayoutFloatsMaxBottom (const DfBrLayoutEngine *E);

/* Layout the inline children of Block starting at (ContentLeft,
 * ContentTop) with the given max line width. Fills in each inline
 * box's geometry and returns the total block height consumed. */
f32
DfBrLayoutInlineChildren (
  DfBrLayoutEngine  *E,
  DfBrLayoutBox     *Block,
  f32                MaxWidth,
  f32                ContentLeft,
  f32                ContentTop
  );

/* ------------------------------------------------------------------
 *  Flex layout (DfBrLayoutFlex.c)
 *
 *  Minimal flex-row implementation. Packs flex items horizontally
 *  from the container's content-left edge, honoring:
 *    * flex-direction: row (default; row-reverse not implemented)
 *    * flex-basis / width for each item's hypothetical main size
 *    * flex-grow for distributing free space
 *    * justify-content: flex-start / flex-end / center /
 *                       space-between / space-around
 *    * align-items is not implemented (items land at content-top).
 *
 *  Each flex item is laid out as a block using the resolved main
 *  size; cross-axis height follows the item's content. The function
 *  returns the tallest resulting item height so the container sizes
 *  correctly when `height: auto`.
 * -----------------------------------------------------------------*/
f32
DfBrLayoutFlexChildren (
  DfBrLayoutEngine  *E,
  DfBrLayoutBox     *Flex,
  f32                InnerLeft,
  f32                InnerTop,
  f32                InnerWidth
  );

/* Shift an entire subtree's horizontal coordinates in place. Used
 * by the flex-column post-pass to realize align-items:center /
 * flex-end after the regular block layout has placed children at
 * the content band's left edge. */
void
DfBrLayoutTranslateSubtreeX (DfBrLayoutBox *Root, f32 Dx);

/* Farthest-right x pixel that `Root`'s subtree actually drew
 * content into. Treats TEXT fragments by their final pen + advance
 * and REPLACED boxes by their content-box right edge. Ignores the
 * intrinsic width of block/inline wrappers that stretched to their
 * containing block, so the result approximates shrink-to-fit. */
f32
DfBrLayoutSubtreeContentRight (const DfBrLayoutBox *Root);

/* ------------------------------------------------------------------
 *  Box building (DfBrLayoutBuild.c)
 * -----------------------------------------------------------------*/

DfBrLayoutBox *
DfBrLayoutBuildSubtree (
  DfBrLayoutEngine    *E,
  const DfBrHtmlNode  *DomNode
  );

/* ------------------------------------------------------------------
 *  Replaced-element intrinsic sizing (DfBrLayoutBuild.c)
 *
 *  Resolves the intrinsic (content-box) width/height for a REPLACED
 *  box based on its DOM element's attributes plus any tag-specific
 *  defaults:
 *
 *    * <svg>    -> width/height attrs, else viewBox, else 300x150.
 *    * <img>    -> width/height attrs, else 0 (unknown).
 *    * <input>  -> type-aware default (text ~ 160x22, button ~ 72x22).
 *    * <textarea>, <select>, <iframe>, <canvas> -> sensible defaults.
 *
 *  Callers are free to override with CSS width/height when set.
 * -----------------------------------------------------------------*/
void
DfBrLayoutIntrinsicReplacedSize (
  const DfBrLayoutBox *Box,
  f32                 *OutW,
  f32                 *OutH
  );

/* ------------------------------------------------------------------
 *  Small helpers
 * -----------------------------------------------------------------*/

static inline b32
DfBrLayoutIsBlockLike (const DfBrLayoutBox *B) {
  return (B != DFBR_NULL) &&
         (B->Kind == DFBR_LAYOUT_BOX_BLOCK ||
          B->Kind == DFBR_LAYOUT_BOX_ANON_BLOCK);
}

static inline b32
DfBrLayoutIsInlineLike (const DfBrLayoutBox *B) {
  return (B != DFBR_NULL) &&
         (B->Kind == DFBR_LAYOUT_BOX_INLINE ||
          B->Kind == DFBR_LAYOUT_BOX_TEXT   ||
          B->Kind == DFBR_LAYOUT_BOX_REPLACED);
}

#endif /* DFBR_LAYOUT_INTERNAL_H_ */
