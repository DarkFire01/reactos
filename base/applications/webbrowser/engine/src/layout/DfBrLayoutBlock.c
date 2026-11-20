/** @file
  DfBrLayoutBlock.c - normal-flow block layout.

  v1 scope:
    * Resolve box-edge lengths (margin/padding/border-width) for the
      four sides against the containing block width (percent
      resolution on margins/padding).
    * Resolve `width`:
        auto          -> containing-block content width minus
                         horizontal margins/padding/borders
                         (classic block behavior).
        <length>      -> use the resolved px.
        <percent>     -> percent of containing-block content width.
    * Resolve `height`:
        auto          -> sum of child block heights / inline-line
                         heights.
        <length>      -> use the resolved px.
        <percent>     -> percent of containing-block content height,
                         or 0 if the container's height is auto.
    * Stack block children vertically, each starting at the previous
      sibling's bottom. Margins do NOT collapse in v1.
    * Inline children are laid out via DfBrLayoutInlineChildren and
      consume a height returned by that helper.
    * Position boxes by walking the tree once from the root with
      (X, Y) accumulators; no second pass needed for normal flow.
**/

#include "DfBrLayoutInternal.h"

#include <string.h>

static f32
ResolveOrZero (const DfBrLayoutEngine *E, const DfBrLayoutBox *Box,
               const DfBrCssLength *L, f32 PctBase) {
  if (L == DFBR_NULL || L->Auto) return 0.0f;
  return DfBrLayoutResolveLengthPx (E, Box, L, PctBase);
}

/* Apply CSS 2.1 §10.4 min-width / max-width clamping to a used width.
 * The CSS rule is "clamp width by max-width first, then by min-width"
 * (min wins over max when min > max, which we achieve by doing min
 * last). `max-width: none` - represented as `Auto` in our AST - opts
 * out of the upper clamp. `min-width: auto` defaults to 0 which is
 * already its initial value, so the helper treats `Auto` as "no
 * constraint" uniformly. */
static f32
ClampWidth (const DfBrLayoutEngine *E, const DfBrLayoutBox *Box,
            f32 W, f32 Cbw,
            const DfBrCssLength *MaxW, const DfBrCssLength *MinW) {
  if (MaxW != DFBR_NULL && !MaxW->Auto) {
    f32 Max = DfBrLayoutResolveLengthPx (E, Box, MaxW, Cbw);
    if (Max >= 0.0f && W > Max) W = Max;
  }
  if (MinW != DFBR_NULL && !MinW->Auto) {
    f32 Min = DfBrLayoutResolveLengthPx (E, Box, MinW, Cbw);
    if (Min >= 0.0f && W < Min) W = Min;
  }
  return W;
}


/* Used-value width for a block box.
 *
 * Returns the content width. Also writes the used left/right margins
 * to `*OutMl` / `*OutMr` for "margin: auto" centering.
 */
static f32
ResolveBlockWidth (const DfBrLayoutEngine *E,
                   const DfBrLayoutBox *Box,
                   f32 AvailW,
                   f32 *OutMl,
                   f32 *OutMr)
{
  const DfBrCssComputedStyle *S = Box->Style;
  f32 Pl, Pr, Bl, Br, Ml, Mr, W;
  b32 MlAuto, MrAuto, WAuto;

  if (S == DFBR_NULL) {
    *OutMl = 0.0f; *OutMr = 0.0f;
    return AvailW;
  }

  Pl = ResolveOrZero (E, Box, &S->PaddingLeft,  AvailW);
  Pr = ResolveOrZero (E, Box, &S->PaddingRight, AvailW);
  Bl = DfBrLayoutResolveBorderPx (E, Box, &S->BorderLeft,  AvailW);
  Br = DfBrLayoutResolveBorderPx (E, Box, &S->BorderRight, AvailW);

  MlAuto = S->MarginLeft.Auto;
  MrAuto = S->MarginRight.Auto;
  WAuto  = S->Width.Auto;

  Ml = MlAuto ? 0.0f : DfBrLayoutResolveLengthPx (E, Box, &S->MarginLeft,  AvailW);
  Mr = MrAuto ? 0.0f : DfBrLayoutResolveLengthPx (E, Box, &S->MarginRight, AvailW);
  W  = WAuto  ? 0.0f : DfBrLayoutResolveLengthPx (E, Box, &S->Width,        AvailW);

  /* CSS UI §8.1 `box-sizing: border-box`: the specified width is
   * the border-box width (i.e. it INCLUDES padding + border).
   * Subtract them so `W` is always the content-box width - the rest
   * of the layout pipeline consistently works in content-box terms.
   * This is the single most pervasive CSS reset in the modern web
   * (`* { box-sizing: border-box; }`); without it, a container like
   * `.container { width: 100%; padding: 0 20px; max-width: 960px }`
   * overshoots its containing block by 2*20px on every page and
   * breaks every centered column. */
  if (!WAuto && S->BoxSizing == DFBR_CSS_BOX_BORDER) {
    W -= (Pl + Pr + Bl + Br);
    if (W < 0.0f) W = 0.0f;
  }

  if (WAuto) {
    /* width auto: absorbs remaining space. Both-auto margins become 0. */
    if (MlAuto) Ml = 0.0f;
    if (MrAuto) Mr = 0.0f;
    W = AvailW - (Ml + Mr + Pl + Pr + Bl + Br);
    if (W < 0.0f) W = 0.0f;
  }

  /* CSS 2.1 §10.4: apply min-width / max-width clamping. This is the
   * second pass; spec says "compute width as if min/max were not set,
   * then if resulting value violates max, recompute as if width had
   * that max value; then if result violates min, force to min".
   * We implement the simpler "tentative value, clamp, rebalance
   * margins" form which produces the same result for the common
   * cases we care about (width: auto or width: 100% with a
   * max-width cap, i.e. centered containers). For border-box
   * sizing, the clamp limits are also border-box, so translate
   * to content-box before applying. */
  {
    DfBrCssLength MaxW = S->MaxWidth;
    DfBrCssLength MinW = S->MinWidth;
    if (S->BoxSizing == DFBR_CSS_BOX_BORDER) {
      f32 Edges = Pl + Pr + Bl + Br;
      if (!MaxW.Auto) {
        f32 V = DfBrLayoutResolveLengthPx (E, Box, &MaxW, AvailW) - Edges;
        if (V < 0.0f) V = 0.0f;
        MaxW.Auto = DFBR_FALSE;
        MaxW.Unit = DFBR_CSS_UNIT_PX;
        MaxW.Value = V;
      }
      if (!MinW.Auto) {
        f32 V = DfBrLayoutResolveLengthPx (E, Box, &MinW, AvailW) - Edges;
        if (V < 0.0f) V = 0.0f;
        MinW.Auto = DFBR_FALSE;
        MinW.Unit = DFBR_CSS_UNIT_PX;
        MinW.Value = V;
      }
    }
    W = ClampWidth (E, Box, W, AvailW, &MaxW, &MinW);
  }

  /* Rebalance margins now that the width has been pinned down. The
   * classic use case is `.container { width: 100%; max-width: 960px;
   * margin: 0 auto; }`: without this clamp-then-rebalance, width
   * stays at 100% of the viewport and margins collapse to zero,
   * leaving content flush-left on any wide enough screen. */
  if (MlAuto && MrAuto) {
    f32 Rest = AvailW - (W + Pl + Pr + Bl + Br);
    if (Rest < 0.0f) Rest = 0.0f;
    Ml = Rest * 0.5f;
    Mr = Rest - Ml;
  } else if (MlAuto) {
    f32 Rest = AvailW - (W + Mr + Pl + Pr + Bl + Br);
    Ml = (Rest > 0.0f) ? Rest : 0.0f;
  } else if (MrAuto) {
    f32 Rest = AvailW - (W + Ml + Pl + Pr + Bl + Br);
    Mr = (Rest > 0.0f) ? Rest : 0.0f;
  }

  *OutMl = Ml;
  *OutMr = Mr;
  return W;
}

static b32
BoxHasInlineChildrenOnly (const DfBrLayoutBox *Box) {
  const DfBrLayoutBox *C;
  b32 Any = DFBR_FALSE;
  for (C = Box->FirstChild; C != DFBR_NULL; C = C->NextSibling) {
    if (DfBrLayoutIsOutOfFlow (C)) continue;  /* absolute/fixed don't count */
    if (DfBrLayoutIsFloat     (C)) continue;  /* neither do floats */
    Any = DFBR_TRUE;
    if (DfBrLayoutIsBlockLike (C)) return DFBR_FALSE;
  }
  return Any;
}

static void
FillEdges (DfBrLayoutEdges *E, f32 T, f32 R, f32 B, f32 L) {
  E->Top = T; E->Right = R; E->Bottom = B; E->Left = L;
}

/* Forward decl for recursion. */
static void LayoutBlockChildren (DfBrLayoutEngine *E,
                                 DfBrLayoutBox *Parent,
                                 f32 InnerLeft,
                                 f32 InnerTop,
                                 f32 InnerWidth,
                                 f32 *OutInnerHeight);

void
DfBrLayoutBlockBox (
  DfBrLayoutEngine  *E,
  DfBrLayoutBox     *Box,
  f32                ContainingBlockWidth,
  f32                ContentLeft,
  f32                ContentTop
  )
{
  const DfBrCssComputedStyle *S = Box->Style;
  f32 Pt=0, Pr=0, Pb=0, Pl=0;
  f32 Bt=0, Br=0, Bb=0, Bl=0;
  f32 Mt=0, Mr=0, Mb=0, Ml=0;
  f32 W, H;
  b32 HAuto = DFBR_TRUE;
  f32 InnerLeft, InnerTop, InnerW, InnerH;

  if (S != DFBR_NULL) {
    Pt = ResolveOrZero (E, Box, &S->PaddingTop,    ContainingBlockWidth);
    Pr = ResolveOrZero (E, Box, &S->PaddingRight,  ContainingBlockWidth);
    Pb = ResolveOrZero (E, Box, &S->PaddingBottom, ContainingBlockWidth);
    Pl = ResolveOrZero (E, Box, &S->PaddingLeft,   ContainingBlockWidth);
    Bt = DfBrLayoutResolveBorderPx (E, Box, &S->BorderTop,    ContainingBlockWidth);
    Br = DfBrLayoutResolveBorderPx (E, Box, &S->BorderRight,  ContainingBlockWidth);
    Bb = DfBrLayoutResolveBorderPx (E, Box, &S->BorderBottom, ContainingBlockWidth);
    Bl = DfBrLayoutResolveBorderPx (E, Box, &S->BorderLeft,   ContainingBlockWidth);
    Mt = S->MarginTop.Auto    ? 0.0f : DfBrLayoutResolveLengthPx (E, Box, &S->MarginTop,    ContainingBlockWidth);
    Mb = S->MarginBottom.Auto ? 0.0f : DfBrLayoutResolveLengthPx (E, Box, &S->MarginBottom, ContainingBlockWidth);
    HAuto = S->Height.Auto;
    /* CSS 2.1 10.5: if this is a percentage `height` and the
     * containing block's height is not explicitly specified (i.e.
     * depends on content), the used value computes to `auto`.
     * The containing block for a static/relative box is its parent
     * block; if the parent's height is itself `auto`, so is ours.
     * (Absolutely/fixed-positioned boxes go through the positioned
     * path and are skipped here.) */
    if (!HAuto && S->Height.Unit == DFBR_CSS_UNIT_PERCENT) {
      const DfBrLayoutBox *P = Box->Parent;
      b32 CbHasExplicitHeight = DFBR_FALSE;
      /* Skip styleless anonymous wrappers to find the real
       * containing block ancestor. */
      while (P != DFBR_NULL && P->Style == DFBR_NULL) P = P->Parent;
      if (P == DFBR_NULL) {
        /* Reached the initial containing block (the viewport),
         * which has an explicit height. */
        CbHasExplicitHeight = DFBR_TRUE;
      } else if (!P->Style->Height.Auto) {
        CbHasExplicitHeight = DFBR_TRUE;
      }
      if (!CbHasExplicitHeight) HAuto = DFBR_TRUE;
    }
  }

  W = ResolveBlockWidth (E, Box, ContainingBlockWidth, &Ml, &Mr);

  /* CSS 2.1 §10.3.2 / §10.6.2: for replaced block elements, `width:
   * auto` means "use intrinsic width" (not "absorb containing-block
   * width"), and `height: auto` uses the intrinsic height or,
   * failing that, the intrinsic aspect ratio scaled from the used
   * width. Without this rule, a <img style="display:block"> with no
   * explicit size (or with only one axis set via CSS) stretches out
   * to the full CB, and height stays 0 - which is exactly what was
   * breaking the Google doodle thumbnails. */
  if (Box->Kind == DFBR_LAYOUT_BOX_REPLACED && S != DFBR_NULL) {
    f32 Iw = 0.0f, Ih = 0.0f;
    DfBrLayoutIntrinsicReplacedSize (Box, &Iw, &Ih);
    if (S->Width.Auto && S->Height.Auto) {
      if (Iw > 0.0f && Ih > 0.0f) {
        W = Iw;
        H = Ih;
        HAuto = DFBR_FALSE;
      }
    } else if (S->Width.Auto) {
      /* height was explicit; derive width from aspect ratio. */
      if (Iw > 0.0f && Ih > 0.0f) {
        f32 HUsed = DfBrLayoutResolveLengthPx (E, Box, &S->Height,
                                               ContainingBlockWidth);
        W = HUsed * (Iw / Ih);
        H = HUsed;
        HAuto = DFBR_FALSE;
      }
    } else if (S->Height.Auto) {
      /* width was explicit; derive height from aspect ratio. */
      if (Iw > 0.0f && Ih > 0.0f && W > 0.0f) {
        H = Ih * (W / Iw);
        HAuto = DFBR_FALSE;
      }
    }

    /* CSS 2.1 §10.4: max-width / min-width apply to replaced elements
     * too. ResolveBlockWidth's own clamp pass was applied BEFORE we
     * swapped in the intrinsic width above, so when a site uses the
     * classic responsive-image rule `img { max-width:100%; height:auto }`
     * (Bootstrap's `.img-responsive`), the authored 263x197 banner
     * would overflow a 152px column because `W` was still the raw
     * intrinsic size. Re-clamp here, and rescale `H` along the
     * intrinsic aspect so `height: auto` still stays proportional. */
    {
      DfBrCssLength MaxW = S->MaxWidth;
      DfBrCssLength MinW = S->MinWidth;
      if (S->BoxSizing == DFBR_CSS_BOX_BORDER) {
        f32 Edges = Pl + Pr + Bl + Br;
        if (!MaxW.Auto) {
          f32 V = DfBrLayoutResolveLengthPx (E, Box, &MaxW,
                                             ContainingBlockWidth) - Edges;
          if (V < 0.0f) V = 0.0f;
          MaxW.Auto = DFBR_FALSE;
          MaxW.Unit = DFBR_CSS_UNIT_PX;
          MaxW.Value = V;
        }
        if (!MinW.Auto) {
          f32 V = DfBrLayoutResolveLengthPx (E, Box, &MinW,
                                             ContainingBlockWidth) - Edges;
          if (V < 0.0f) V = 0.0f;
          MinW.Auto = DFBR_FALSE;
          MinW.Unit = DFBR_CSS_UNIT_PX;
          MinW.Value = V;
        }
      }
      {
        f32 WClamped = ClampWidth (E, Box, W, ContainingBlockWidth,
                                   &MaxW, &MinW);
        if (WClamped != W) {
          if (S->Height.Auto && Iw > 0.0f && Ih > 0.0f) {
            H = Ih * (WClamped / Iw);
            HAuto = DFBR_FALSE;
          }
          W = WClamped;
        }
      }
    }
  }

  /* Provisional inner rect - position first (X, Y), dimensions below. */
  InnerLeft = ContentLeft + Ml + Bl + Pl;
  InnerTop  = ContentTop  + Mt + Bt + Pt;
  InnerW    = W;

  /* Snapshot the float list tail so we can tell, after the children
   * finish, which floats this subtree contributed. We grow the
   * parent's height to contain its own descendant floats. This is the
   * BFC-contains-floats rule, applied implicitly here. Without it a
   * Bootstrap-style `<section class="row"><div class="col float">`
   * collapses to zero height because the floats "escape" the row and
   * every subsequent section stacks on top of them. Real CSS gates
   * this on the parent establishing a BFC (overflow != visible,
   * display: flow-root, a clearfix ::after, etc.); we don't yet model
   * those triggers, so v1 always contains. Downside: a sibling block
   * can no longer flow around a floated cousin, which is the classic
   * "float wraps around text" pattern. That pattern is rare on modern
   * layouts and can be re-enabled later by gating on explicit BFC
   * indicators. */
  {
    const DfBrLayoutFloat *FloatsSnapshot = (E != DFBR_NULL)
                                              ? E->FloatsTail : DFBR_NULL;

  /* Lay out children. */
  InnerH = 0.0f;
  if (Box->FirstChild != DFBR_NULL) {
    /* `display: flex` / `grid` route through the minimal flex
     * packer when in row mode. `flex-direction: column` items
     * stack vertically, which is functionally identical to the
     * standard block-in-block flow for v1, so we fall through to
     * the normal block path in that case. */
    b32 IsFlex = (S != DFBR_NULL) &&
                 (S->Display == DFBR_CSS_DISPLAY_FLEX ||
                  S->Display == DFBR_CSS_DISPLAY_INLINE_FLEX ||
                  S->Display == DFBR_CSS_DISPLAY_GRID);
    b32 IsRow  = (S != DFBR_NULL) &&
                 (S->FlexDirection == DFBR_CSS_FLEX_ROW ||
                  S->FlexDirection == DFBR_CSS_FLEX_ROW_REVERSE);
    /* CSS Flexbox §4 says "each contiguous sequence of text directly
     * contained inside a flex container is wrapped in an anonymous
     * flex item". Our flex packer skips text nodes entirely, which
     * loses buttons like `<a class="btn" style="display:inline-flex">
     * See the games</a>` - the text goes through DFBR_LAYOUT_BOX_TEXT
     * (not an element child) and the packer collects zero items,
     * leaving the box at height 0 with no glyph rects. When the flex
     * container has ONLY inline/text children we can safely route to
     * the inline formatting context instead: it produces the exact
     * same visual result for this very common "inline-flex wraps a
     * label" use case, and only real element-child flex containers
     * keep the packer. */
    if (IsFlex && IsRow && BoxHasInlineChildrenOnly (Box)) {
      IsFlex = DFBR_FALSE;
    }
    if (IsFlex && IsRow) {
      InnerH = DfBrLayoutFlexChildren (E, Box, InnerLeft, InnerTop, InnerW);
    } else if (BoxHasInlineChildrenOnly (Box)) {
      InnerH = DfBrLayoutInlineChildren (E, Box, InnerW, InnerLeft, InnerTop);
    } else {
      LayoutBlockChildren (E, Box, InnerLeft, InnerTop, InnerW, &InnerH);
    }

    /* `flex-direction: column` with align-items != start/stretch
     * cross-axis-aligns each in-flow child by shifting its subtree
     * horizontally within the container's content band. The regular
     * block path we just ran placed every child at InnerLeft; we
     * compute the leftover horizontal space per child and translate
     * accordingly. Stretch keeps the block-default full-width
     * behavior; start / flex-start are no-ops. */
    if (IsFlex && !IsRow && S != DFBR_NULL &&
        (S->AlignItems == DFBR_CSS_ALIGN_CENTER ||
         S->AlignItems == DFBR_CSS_ALIGN_END ||
         S->AlignItems == DFBR_CSS_ALIGN_FLEX_END))
    {
      DfBrLayoutBox *AC;
      for (AC = Box->FirstChild; AC != DFBR_NULL; AC = AC->NextSibling) {
        f32 Effective, Outer, Dx;
        if (DfBrLayoutIsOutOfFlow (AC)) continue;
        if (DfBrLayoutIsFloat     (AC)) continue;
        if (AC->Kind == DFBR_LAYOUT_BOX_TEXT) continue;
        /* Block layout stretched AC to full width. Spec'd align-items
         * (non-stretch) wants the item sized to its content and then
         * aligned. Approximate: measure the rightmost pixel the
         * subtree actually drew text/replaced content into, subtract
         * AC's content-box origin to get an effective content width,
         * and center on that. Empty or zero-extent items fall through
         * with no shift. */
        Effective = DfBrLayoutSubtreeContentRight (AC) - AC->ContentRect.X;
        if (Effective <= 0.0f || Effective >= AC->ContentRect.W) continue;
        Outer = AC->Margin.Left  + AC->Border.Left  + AC->Padding.Left
              + Effective
              + AC->Padding.Right + AC->Border.Right + AC->Margin.Right;
        if (Outer >= InnerW) continue;
        Dx = (S->AlignItems == DFBR_CSS_ALIGN_CENTER)
             ? (InnerW - Outer) * 0.5f
             : (InnerW - Outer);
        if (Dx > 0.0f) DfBrLayoutTranslateSubtreeX (AC, Dx);
      }
    }
  }

  /* Contain our subtree's floats (see snapshot comment above). Walk
   * the list from the snapshot forward and extend InnerH to cover any
   * float outer bottom that sits inside our horizontal band. We never
   * shrink below the normal-flow height - text/block content still
   * wins when it's taller than the floats. */
  if (E != DFBR_NULL) {
    const DfBrLayoutFloat *Start = (FloatsSnapshot != DFBR_NULL)
                                      ? FloatsSnapshot->Next
                                      : E->FloatsHead;
    const DfBrLayoutFloat *F;
    f32 BandL = InnerLeft;
    f32 BandR = InnerLeft + InnerW;
    for (F = Start; F != DFBR_NULL; F = F->Next) {
      /* Skip floats that landed entirely outside our content band - they
       * were placed by a nested context that doesn't overlap us. */
      if (F->OuterRight <= BandL || F->OuterLeft >= BandR) continue;
      {
        f32 Bottom = F->OuterBottom - InnerTop;
        if (Bottom > InnerH) InnerH = Bottom;
      }
    }
  }

  }  /* end FloatsSnapshot scope */

  if (HAuto) {
    H = InnerH;
  } else {
    /* Height resolves against containing block height if we knew it;
     * for v1 use viewport height as a fallback when this is the ICB
     * child. Percents default to 0 for inner blocks. */
    f32 Base = (Box->Parent != DFBR_NULL &&
                Box->Parent->ContentRect.H > 0.0f) ? Box->Parent->ContentRect.H : E->ViewportH;
    H = DfBrLayoutResolveLengthPx (E, Box, &S->Height, Base);
    /* Border-box: specified height includes padding+border on the
     * vertical axis; strip them out to keep `H` as content-box height. */
    if (S->BoxSizing == DFBR_CSS_BOX_BORDER) {
      H -= (Pt + Pb + Bt + Bb);
      if (H < 0.0f) H = 0.0f;
    }
  }

  /* Apply min-height / max-height. Percentages follow the same
   * "containing-block-height must be explicit" rule as `height`;
   * if that's not satisfied they fall back to 0 (min) / none (max). */
  if (S != DFBR_NULL) {
    f32 Base = (Box->Parent != DFBR_NULL &&
                Box->Parent->ContentRect.H > 0.0f) ? Box->Parent->ContentRect.H : E->ViewportH;

    if (!S->MinHeight.Auto) {
      b32 OkPct = DFBR_TRUE;
      if (S->MinHeight.Unit == DFBR_CSS_UNIT_PERCENT) {
        const DfBrLayoutBox *P = Box->Parent;
        while (P != DFBR_NULL && P->Style == DFBR_NULL) P = P->Parent;
        OkPct = (P == DFBR_NULL) ? DFBR_TRUE : !P->Style->Height.Auto;
      }
      if (OkPct) {
        f32 MinH = DfBrLayoutResolveLengthPx (E, Box, &S->MinHeight, Base);
        if (MinH > H) H = MinH;
      }
    }

    if (!S->MaxHeight.Auto) {
      b32 OkPct = DFBR_TRUE;
      if (S->MaxHeight.Unit == DFBR_CSS_UNIT_PERCENT) {
        const DfBrLayoutBox *P = Box->Parent;
        while (P != DFBR_NULL && P->Style == DFBR_NULL) P = P->Parent;
        OkPct = (P == DFBR_NULL) ? DFBR_TRUE : !P->Style->Height.Auto;
      }
      if (OkPct) {
        f32 MaxH = DfBrLayoutResolveLengthPx (E, Box, &S->MaxHeight, Base);
        if (H > MaxH) H = MaxH;
      }
    }
  }

  /* Commit geometry. */
  Box->ContentRect.X = InnerLeft;
  Box->ContentRect.Y = InnerTop;
  Box->ContentRect.W = InnerW;
  Box->ContentRect.H = H;
  FillEdges (&Box->Padding, Pt, Pr, Pb, Pl);
  FillEdges (&Box->Border,  Bt, Br, Bb, Bl);
  FillEdges (&Box->Margin,  Mt, Mr, Mb, Ml);
}

static void
LayoutBlockChildren (DfBrLayoutEngine *E,
                     DfBrLayoutBox *Parent,
                     f32 InnerLeft,
                     f32 InnerTop,
                     f32 InnerWidth,
                     f32 *OutInnerHeight)
{
  DfBrLayoutBox *C;
  f32            Y = InnerTop;
  DfBrLayoutBox *InlineRun = DFBR_NULL;

  /* Children can be a mix of block and inline after anonymous-block
   * wrapping. Anonymous-block wrappers are already in place from the
   * build pass, so here every child is block-like or a stray inline
   * (which we wrap lazily into a synthetic anon block). */
  for (C = Parent->FirstChild; C != DFBR_NULL; C = C->NextSibling) {
    /* Out-of-flow boxes (position: absolute / fixed) are laid out
     * separately after the in-flow pass finishes. They reserve no
     * space in their parent, but we capture the current in-flow pen
     * as the box's "static position" so the positioning pass can
     * resolve auto offsets per CSS 2.1 §10.3.7 / §10.6.4. */
    if (DfBrLayoutIsOutOfFlow (C)) {
      C->StaticX      = InnerLeft;
      C->StaticY      = Y;
      C->HasStaticPos = DFBR_TRUE;
      continue;
    }
    if (DfBrLayoutIsFloat (C)) {
      /* Floats do not advance the in-flow Y. They do, however, pin
       * themselves at or below the current pen; subsequent block
       * boxes overlap them (contents flow around via the inline
       * path's band-clipping). */
      DfBrLayoutFloatPlace (E, C, InnerLeft, InnerLeft + InnerWidth, Y);
      continue;
    }
    if (DfBrLayoutIsBlockLike (C)) {
      /* Honor `clear: left | right | both` before placing this
       * block's top edge. We shift Y only: the outer margins still
       * apply on top of the cleared position. */
      if (C->Style != DFBR_NULL && C->Style->Clear != DFBR_CSS_CLEAR_NONE) {
        f32 Cleared = DfBrLayoutFloatsClear (E, Y, C->Style->Clear);
        if (Cleared > Y) Y = Cleared;
      }
      DfBrLayoutBlockBox (E, C, InnerWidth, InnerLeft, Y);
      Y += C->Margin.Top + C->Border.Top + C->Padding.Top
         + C->ContentRect.H
         + C->Padding.Bottom + C->Border.Bottom + C->Margin.Bottom;
    } else {
      /* A lone inline under a block container - shouldn't happen
       * after BuildSubtree, but treat as a single-line inline run. */
      f32 H;
      DfBrLayoutBox *SavedFirst = Parent->FirstChild;
      DfBrLayoutBox *SavedLast  = Parent->LastChild;
      (void)InlineRun; (void)SavedFirst; (void)SavedLast;
      H = DfBrLayoutInlineChildren (E, Parent, InnerWidth, InnerLeft, Y);
      Y += H;
      /* Stop after a single inline-run group. */
      break;
    }
  }

  *OutInnerHeight = Y - InnerTop;
}

void
DfBrLayoutRun (
  DfBrLayoutEngine          *Engine,
  const DfBrLayoutViewport  *Viewport
  )
{
  f32 VpW, VpH;
  DfBrLayoutBox *HtmlBox;

  if (Engine == DFBR_NULL || Viewport == DFBR_NULL) return;
  Engine->LastViewport = *Viewport;
  Engine->ViewportW    = Viewport->WidthPx;
  Engine->ViewportH    = Viewport->HeightPx;
  Engine->RootFontSizePx = (Viewport->DefaultFontSizePx > 0.0f)
                           ? Viewport->DefaultFontSizePx
                           : 16.0f;

  /* Fresh float list for this layout pass. */
  DfBrLayoutFloatsReset (Engine);

  VpW = Viewport->WidthPx;
  VpH = Viewport->HeightPx;

  /* Initial containing block. */
  Engine->Root->ContentRect.X = 0.0f;
  Engine->Root->ContentRect.Y = 0.0f;
  Engine->Root->ContentRect.W = VpW;
  Engine->Root->ContentRect.H = VpH;
  memset (&Engine->Root->Padding, 0, sizeof (Engine->Root->Padding));
  memset (&Engine->Root->Border,  0, sizeof (Engine->Root->Border));
  memset (&Engine->Root->Margin,  0, sizeof (Engine->Root->Margin));

  /* Lay out the html child (typically the only child). */
  for (HtmlBox = Engine->Root->FirstChild;
       HtmlBox != DFBR_NULL;
       HtmlBox = HtmlBox->NextSibling)
  {
    if (DfBrLayoutIsOutOfFlow (HtmlBox)) continue;
    if (DfBrLayoutIsBlockLike (HtmlBox)) {
      DfBrLayoutBlockBox (Engine, HtmlBox, VpW, 0.0f, 0.0f);
    }
  }

  /* Second pass: lay out every absolutely/fixed-positioned box now
   * that its containing block's geometry is known. */
  DfBrLayoutPlacePositioned (Engine);
}
