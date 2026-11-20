/** @file
  DfBrLayoutPositioned.c - second-pass placement for absolutely- and
                           fixed-positioned boxes.

  CSS 2.1, section 9.6 describes the model this file implements:

    - A box with `position: absolute` is removed from the normal flow
      and laid out relative to its "containing block": the padding
      edge of the nearest positioned ancestor (any box whose
      `position` is not `static`). If no such ancestor exists, the
      initial containing block (the viewport) is used.

    - A box with `position: fixed` is similar, except its containing
      block is always the viewport.

    - The offsets (`top`/`right`/`bottom`/`left`) resolve against the
      containing block's width (horizontal) and height (vertical).

  v1 scope:
    * Only absolute and fixed are laid out out-of-flow. Relative and
      sticky remain in flow; their paint-time offsets are a follow-up.
    * Width / height resolution:
        - If the declared width is a <length> or a <percent>, use it.
        - Otherwise, if both left and right offsets are explicit,
          width = CBw - left - right - margin-left - margin-right -
                  border-left - border-right - padding-left -
                  padding-right.
        - Otherwise, fall back to the containing block's content
          width (a generous default; shrink-to-fit lands later).
      Height follows the same logic against the vertical axis.
    * After resolving width/height, the box is laid out as a normal
      block (DfBrLayoutBlockBox) at a provisional origin; we then
      translate the whole subtree to its final (x, y) so every
      descendant's ContentRect is consistent.

  Deliberate v1 gaps:
    - No stacking contexts / z-index ordering (paint walks the tree
      in document order).
    - No shrink-to-fit width computation (so auto + no offsets falls
      back to CB width instead of the inline intrinsic width).
    - No `margin: auto` centering for positioned boxes.
    - No interaction with containing-block resolution through
      transforms.
**/

#include "DfBrLayoutInternal.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------
 *  Containing-block lookup
 * -----------------------------------------------------------------*/

static b32
IsPositionedContainer (const DfBrLayoutBox *B) {
  if (B == DFBR_NULL || B->Style == DFBR_NULL) return DFBR_FALSE;
  return (b32)(B->Style->Position != DFBR_CSS_POSITION_STATIC);
}

/* The containing block for a `position: absolute` box is the nearest
 * positioned ancestor; if there isn't one, the initial containing
 * block (viewport). For `position: fixed`, it is always the viewport.
 *
 * Both are reported here in terms of the ancestor's *padding-box*
 * rect (the inner edge of its border). For the synthetic root box
 * there is no border / padding so the padding box == content box. */
static void
ResolveContainingBlock (const DfBrLayoutEngine *E,
                        const DfBrLayoutBox    *Box,
                        f32 *OutX, f32 *OutY,
                        f32 *OutW, f32 *OutH)
{
  const DfBrLayoutBox *A;
  const DfBrLayoutBox *Root = E->Root;

  if (Box->Style != DFBR_NULL &&
      Box->Style->Position == DFBR_CSS_POSITION_FIXED)
  {
    *OutX = Root->ContentRect.X;
    *OutY = Root->ContentRect.Y;
    *OutW = E->ViewportW;
    *OutH = E->ViewportH;
    return;
  }

  for (A = Box->Parent; A != DFBR_NULL; A = A->Parent) {
    if (IsPositionedContainer (A)) {
      *OutX = A->ContentRect.X - A->Padding.Left;
      *OutY = A->ContentRect.Y - A->Padding.Top;
      *OutW = A->ContentRect.W + A->Padding.Left + A->Padding.Right;
      *OutH = A->ContentRect.H + A->Padding.Top  + A->Padding.Bottom;
      return;
    }
  }

  *OutX = Root->ContentRect.X;
  *OutY = Root->ContentRect.Y;
  *OutW = E->ViewportW;
  *OutH = E->ViewportH;
}

/* ------------------------------------------------------------------
 *  Geometry translation
 *
 *  We lay out a positioned box at a provisional origin and then
 *  shift the whole subtree by (Dx, Dy). Every box's ContentRect and
 *  every text fragment owned by text boxes in the subtree move in
 *  lockstep.
 * -----------------------------------------------------------------*/

static void
TranslateSubtree (DfBrLayoutBox *Box, f32 Dx, f32 Dy) {
  DfBrLayoutBox *C;
  if (Box == DFBR_NULL) return;
  Box->ContentRect.X += Dx;
  Box->ContentRect.Y += Dy;
  if (Box->Kind == DFBR_LAYOUT_BOX_TEXT) {
    DfBrLayoutTextFragment *Tf;
    for (Tf = Box->FirstTextFragment; Tf != DFBR_NULL; Tf = Tf->Next) {
      Tf->X         += Dx;
      Tf->BaselineY += Dy;
    }
  }
  for (C = Box->FirstChild; C != DFBR_NULL; C = C->NextSibling) {
    TranslateSubtree (C, Dx, Dy);
  }
}

/* ------------------------------------------------------------------
 *  Resolve offsets against a CB axis length
 * -----------------------------------------------------------------*/

static f32
ResolveOrAuto (const DfBrLayoutEngine *E,
               const DfBrLayoutBox    *Box,
               const DfBrCssLength    *L,
               f32                     Base,
               b32                    *OutAuto)
{
  if (L == DFBR_NULL || L->Auto) { *OutAuto = DFBR_TRUE; return 0.0f; }
  *OutAuto = DFBR_FALSE;
  return DfBrLayoutResolveLengthPx (E, Box, L, Base);
}

/* ------------------------------------------------------------------
 *  Lay out one positioned box
 * -----------------------------------------------------------------*/

static void
PlaceOne (DfBrLayoutEngine *E, DfBrLayoutBox *Box) {
  const DfBrCssComputedStyle *S = Box->Style;
  f32 Cbx, Cby, Cbw, Cbh;
  f32 Top, Right, Bottom, Left;
  b32 TopAuto, RightAuto, BottomAuto, LeftAuto;
  f32 Mt, Mr, Mb, Ml;
  f32 Bt, Br, Bb, Bl;
  f32 Pt, Pr, Pb, Pl;
  f32 W, H;
  b32 WAuto, HAuto;
  f32 TargetX, TargetY;
  f32 OuterW, OuterH;

  if (S == DFBR_NULL) return;

  ResolveContainingBlock (E, Box, &Cbx, &Cby, &Cbw, &Cbh);

  /* Offsets. */
  Top    = ResolveOrAuto (E, Box, &S->Top,    Cbh, &TopAuto);
  Right  = ResolveOrAuto (E, Box, &S->Right,  Cbw, &RightAuto);
  Bottom = ResolveOrAuto (E, Box, &S->Bottom, Cbh, &BottomAuto);
  Left   = ResolveOrAuto (E, Box, &S->Left,   Cbw, &LeftAuto);

  /* Margins / borders / padding (percentages use the CB width per
   * spec - percentage on vertical margins also resolves against the
   * containing block's width). */
  Ml = S->MarginLeft.Auto    ? 0.0f : DfBrLayoutResolveLengthPx (E, Box, &S->MarginLeft,   Cbw);
  Mr = S->MarginRight.Auto   ? 0.0f : DfBrLayoutResolveLengthPx (E, Box, &S->MarginRight,  Cbw);
  Mt = S->MarginTop.Auto     ? 0.0f : DfBrLayoutResolveLengthPx (E, Box, &S->MarginTop,    Cbw);
  Mb = S->MarginBottom.Auto  ? 0.0f : DfBrLayoutResolveLengthPx (E, Box, &S->MarginBottom, Cbw);
  Bt = DfBrLayoutResolveBorderPx (E, Box, &S->BorderTop,    Cbw);
  Br = DfBrLayoutResolveBorderPx (E, Box, &S->BorderRight,  Cbw);
  Bb = DfBrLayoutResolveBorderPx (E, Box, &S->BorderBottom, Cbw);
  Bl = DfBrLayoutResolveBorderPx (E, Box, &S->BorderLeft,   Cbw);
  Pt = S->PaddingTop.Auto    ? 0.0f : DfBrLayoutResolveLengthPx (E, Box, &S->PaddingTop,    Cbw);
  Pr = S->PaddingRight.Auto  ? 0.0f : DfBrLayoutResolveLengthPx (E, Box, &S->PaddingRight,  Cbw);
  Pb = S->PaddingBottom.Auto ? 0.0f : DfBrLayoutResolveLengthPx (E, Box, &S->PaddingBottom, Cbw);
  Pl = S->PaddingLeft.Auto   ? 0.0f : DfBrLayoutResolveLengthPx (E, Box, &S->PaddingLeft,   Cbw);

  /* Width. */
  WAuto = S->Width.Auto;
  if (!WAuto) {
    W = DfBrLayoutResolveLengthPx (E, Box, &S->Width, Cbw);
  } else if (!LeftAuto && !RightAuto) {
    W = Cbw - Left - Right - Ml - Mr - Bl - Br - Pl - Pr;
    if (W < 0.0f) W = 0.0f;
  } else {
    /* Shrink-to-fit fallback: use CB content width. */
    W = Cbw - Ml - Mr - Bl - Br - Pl - Pr;
    if (W < 0.0f) W = 0.0f;
  }

  /* Height (we need an explicit value to feed back into offsets; the
   * actual content height is recomputed by the block layout below). */
  HAuto = S->Height.Auto;
  if (!HAuto) {
    H = DfBrLayoutResolveLengthPx (E, Box, &S->Height, Cbh);
  } else if (!TopAuto && !BottomAuto) {
    H = Cbh - Top - Bottom - Mt - Mb - Bt - Bb - Pt - Pb;
    if (H < 0.0f) H = 0.0f;
  } else {
    /* Defer to content-driven height from the block pass below. */
    H = -1.0f;
  }

  /* Lay out the subtree provisionally at (0, 0). DfBrLayoutBlockBox
   * will position the content box at (Ml + Bl + Pl, Mt + Bt + Pt)
   * inside this origin and compute the height. We override the
   * style's Width with the resolved used-value via a temporary
   * struct so the inner layout uses the right track. */
  {
    /* Build a scratch style with the resolved used width if it
     * differs from `auto`; we do this by running the block layout
     * against a synthesized "containing block width" equal to the
     * used outer width. */
    f32 ProvisionalCbw = W + Ml + Mr + Bl + Br + Pl + Pr;
    /* CSS 2.1 §9.4.1: absolutely- and fixed-positioned boxes
     * establish a NEW block formatting context. Floats inside them
     * must be contained to the positioned box's own subtree - they
     * must not see (or extend) the outer document's float list.
     * Before our isolation, an outer document with floats that
     * reached down to (say) Y=1340 would push every <li float:left>
     * inside a position:fixed navbar down to Y=1340 as well. Save
     * and restore the engine-wide float list around the provisional
     * layout so the positioned subtree runs against a fresh one. */
    DfBrLayoutFloat *SavedHead = E->FloatsHead;
    DfBrLayoutFloat *SavedTail = E->FloatsTail;
    E->FloatsHead = DFBR_NULL;
    E->FloatsTail = DFBR_NULL;
    DfBrLayoutBlockBox (E, Box, ProvisionalCbw, 0.0f, 0.0f);
    E->FloatsHead = SavedHead;
    E->FloatsTail = SavedTail;
    /* If the caller pinned both vertical edges, overwrite the used
     * height to respect them; otherwise trust the computed one. */
    if (H >= 0.0f) {
      Box->ContentRect.H = H;
    }
  }

  /* After the provisional layout:
   *   Box->ContentRect.X == Ml + Bl + Pl
   *   Box->ContentRect.Y == Mt + Bt + Pt
   * Compute the final (outer) origin relative to the CB, then
   * translate by the delta. */
  OuterW = Box->ContentRect.W + Pl + Pr + Bl + Br + Ml + Mr;
  OuterH = Box->ContentRect.H + Pt + Pb + Bt + Bb + Mt + Mb;

  if (!LeftAuto) {
    TargetX = Cbx + Left;
  } else if (!RightAuto) {
    TargetX = Cbx + Cbw - Right - OuterW;
  } else if (Box->HasStaticPos) {
    /* CSS 2.1 §10.3.7: when left and right are both auto, the used
     * value of left is the static position - where the box would
     * have sat in the normal flow. Without this fallback, every
     * absolutely-positioned element without explicit offsets would
     * pile up at the top-left of its containing block (a very
     * common bug on pages like google.com that rely on auto static
     * positioning to keep nav icons in-line). */
    TargetX = Box->StaticX;
  } else {
    TargetX = Cbx;
  }

  if (!TopAuto) {
    TargetY = Cby + Top;
  } else if (!BottomAuto) {
    TargetY = Cby + Cbh - Bottom - OuterH;
  } else if (Box->HasStaticPos) {
    /* Same static-position fallback on the vertical axis
     * (CSS 2.1 §10.6.4). */
    TargetY = Box->StaticY;
  } else {
    TargetY = Cby;
  }

  /* The provisional ContentRect.X/Y is (Ml+Bl+Pl, Mt+Bt+Pt); we
   * want it at (TargetX + Ml+Bl+Pl, TargetY + Mt+Bt+Pt). Delta is
   * therefore simply (TargetX, TargetY). */
  TranslateSubtree (Box, TargetX, TargetY);
}

/* ------------------------------------------------------------------
 *  Public entry
 * -----------------------------------------------------------------*/

static void
WalkAndPlace (DfBrLayoutEngine *E, DfBrLayoutBox *Box) {
  DfBrLayoutBox *C;
  if (Box == DFBR_NULL) return;

  /* Descend first so we handle nested positioned boxes in document
   * order - but a positioned descendant inside another positioned
   * box needs its parent's geometry resolved first, so we skip into
   * children only for non-positioned containers on the way down;
   * for positioned boxes we place them first, then recurse. */
  if (DfBrLayoutIsOutOfFlow (Box)) {
    PlaceOne (E, Box);
  }

  for (C = Box->FirstChild; C != DFBR_NULL; C = C->NextSibling) {
    WalkAndPlace (E, C);
  }
}

void
DfBrLayoutPlacePositioned (DfBrLayoutEngine *E) {
  if (E == DFBR_NULL || E->Root == DFBR_NULL) return;
  WalkAndPlace (E, E->Root);
}
