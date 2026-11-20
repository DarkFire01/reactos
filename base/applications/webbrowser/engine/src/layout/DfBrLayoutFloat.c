/** @file
  DfBrLayoutFloat.c - placement and bookkeeping for CSS floats.

  See DfBrLayoutInternal.h for the v1 scope. In short: we maintain a
  flat list of active floats in the engine (one BFC), and expose
  helpers that answer two questions on demand:

    1. "At a given Y, how wide is the unobstructed inline band?"
       Used by DfBrLayoutInline.c to shrink line boxes.

    2. "Where does this float anchor?" Used by DfBrLayoutBlock.c when
       it encounters a floated child.

  Placement algorithm (CSS 2.1, section 9.5.1):

    - Start at the current Y (the caller's pen position).
    - At each candidate Y, consult the active float list to compute
      the horizontal band left & right. If the float's outer width
      fits, it anchors at that Y. Otherwise we advance Y to the next
      float bottom that could change the band, and retry.
    - Left floats anchor at the band's left; right floats anchor at
      the band's right minus their outer width.
    - Once anchored, we translate the float's subtree into place and
      append a record describing its outer rect to the float list.

  We lay out the float's subtree using DfBrLayoutBlockBox against a
  synthetic containing-block width equal to the float's own used
  margin-box width, so the block pass resolves the float's edges and
  inner layout consistently.
**/

#include "DfBrLayoutInternal.h"

#include <string.h>

/* ------------------------------------------------------------------
 *  Bookkeeping
 * -----------------------------------------------------------------*/

void
DfBrLayoutFloatsReset (DfBrLayoutEngine *E) {
  if (E == DFBR_NULL) return;
  E->FloatsHead = DFBR_NULL;
  E->FloatsTail = DFBR_NULL;
}

static void
FloatsAppend (DfBrLayoutEngine *E, DfBrLayoutFloat *F) {
  F->Next = DFBR_NULL;
  if (E->FloatsTail != DFBR_NULL) E->FloatsTail->Next = F;
  else                            E->FloatsHead       = F;
  E->FloatsTail = F;
}

/* ------------------------------------------------------------------
 *  Horizontal band queries
 * -----------------------------------------------------------------*/

void
DfBrLayoutFloatsBandAt (const DfBrLayoutEngine *E,
                        f32 Y,
                        f32 CbLeft,
                        f32 CbRight,
                        f32 *OutLeft,
                        f32 *OutRight)
{
  const DfBrLayoutFloat *F;
  f32 L = CbLeft;
  f32 R = CbRight;

  if (E == DFBR_NULL) { *OutLeft = L; *OutRight = R; return; }

  for (F = E->FloatsHead; F != DFBR_NULL; F = F->Next) {
    /* A float occupies [OuterTop, OuterBottom). The interval is
     * half-open: a float ending exactly at Y no longer obstructs
     * that line. */
    if (Y < F->OuterTop || Y >= F->OuterBottom) continue;
    if (F->Side == DFBR_LAYOUT_FLOAT_SIDE_LEFT) {
      if (F->OuterRight > L) L = F->OuterRight;
    } else {
      if (F->OuterLeft  < R) R = F->OuterLeft;
    }
  }
  if (R < L) R = L;
  *OutLeft  = L;
  *OutRight = R;
}

/* Find the smallest float bottom strictly greater than `Y`, or
 * DFBR_NULL-equivalent -1 when no float does. Used by the placement
 * loop to skip to the next interesting Y when the current band is
 * too narrow. */
static f32
NextFloatBottomAfter (const DfBrLayoutEngine *E, f32 Y) {
  const DfBrLayoutFloat *F;
  f32 Best = -1.0f;
  for (F = E->FloatsHead; F != DFBR_NULL; F = F->Next) {
    if (F->OuterBottom > Y) {
      if (Best < 0.0f || F->OuterBottom < Best) Best = F->OuterBottom;
    }
  }
  return Best;
}

f32
DfBrLayoutFloatsClear (const DfBrLayoutEngine *E,
                       f32 YHint,
                       DfBrCssClear C)
{
  const DfBrLayoutFloat *F;
  f32 Y = YHint;
  if (E == DFBR_NULL || C == DFBR_CSS_CLEAR_NONE) return YHint;
  for (F = E->FloatsHead; F != DFBR_NULL; F = F->Next) {
    b32 Match = DFBR_FALSE;
    switch (C) {
      case DFBR_CSS_CLEAR_LEFT:
        Match = (b32)(F->Side == DFBR_LAYOUT_FLOAT_SIDE_LEFT);  break;
      case DFBR_CSS_CLEAR_RIGHT:
        Match = (b32)(F->Side == DFBR_LAYOUT_FLOAT_SIDE_RIGHT); break;
      case DFBR_CSS_CLEAR_BOTH:
        Match = DFBR_TRUE; break;
      default: break;
    }
    if (Match && F->OuterBottom > Y) Y = F->OuterBottom;
  }
  return Y;
}

f32
DfBrLayoutFloatsMaxBottom (const DfBrLayoutEngine *E) {
  const DfBrLayoutFloat *F;
  f32 Best = 0.0f;
  if (E == DFBR_NULL) return 0.0f;
  for (F = E->FloatsHead; F != DFBR_NULL; F = F->Next) {
    if (F->OuterBottom > Best) Best = F->OuterBottom;
  }
  return Best;
}

/* ------------------------------------------------------------------
 *  Translation helper (mirror of the one in DfBrLayoutPositioned.c)
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
 *  Float placement
 * -----------------------------------------------------------------*/

static f32
ResolveOrZero (const DfBrLayoutEngine *E, const DfBrLayoutBox *Box,
               const DfBrCssLength *L, f32 PctBase) {
  if (L == DFBR_NULL || L->Auto) return 0.0f;
  return DfBrLayoutResolveLengthPx (E, Box, L, PctBase);
}

f32
DfBrLayoutFloatPlace (DfBrLayoutEngine *E,
                      DfBrLayoutBox    *Box,
                      f32               CbLeft,
                      f32               CbRight,
                      f32               YHint)
{
  const DfBrCssComputedStyle *S = Box->Style;
  f32 Cbw = CbRight - CbLeft;
  f32 Ml, Mr, Mt, Mb;
  f32 Bl, Br, Bt, Bb;
  f32 Pl, Pr, Pt, Pb;
  f32 W;
  f32 OuterW, OuterH;
  f32 Y;
  f32 L, R;
  f32 OuterX, OuterY;
  DfBrLayoutFloat *Rec;

  if (S == DFBR_NULL) return YHint;

  /* Resolve horizontal edges up front; vertical edges used after
   * the provisional inner layout. */
  Ml = S->MarginLeft.Auto    ? 0.0f : DfBrLayoutResolveLengthPx (E, Box, &S->MarginLeft,    Cbw);
  Mr = S->MarginRight.Auto   ? 0.0f : DfBrLayoutResolveLengthPx (E, Box, &S->MarginRight,   Cbw);
  Mt = S->MarginTop.Auto     ? 0.0f : DfBrLayoutResolveLengthPx (E, Box, &S->MarginTop,     Cbw);
  Mb = S->MarginBottom.Auto  ? 0.0f : DfBrLayoutResolveLengthPx (E, Box, &S->MarginBottom,  Cbw);
  Bl = DfBrLayoutResolveBorderPx (E, Box, &S->BorderLeft,   Cbw);
  Br = DfBrLayoutResolveBorderPx (E, Box, &S->BorderRight,  Cbw);
  Bt = DfBrLayoutResolveBorderPx (E, Box, &S->BorderTop,    Cbw);
  Bb = DfBrLayoutResolveBorderPx (E, Box, &S->BorderBottom, Cbw);
  Pl = ResolveOrZero (E, Box, &S->PaddingLeft,   Cbw);
  Pr = ResolveOrZero (E, Box, &S->PaddingRight,  Cbw);
  Pt = ResolveOrZero (E, Box, &S->PaddingTop,    Cbw);
  Pb = ResolveOrZero (E, Box, &S->PaddingBottom, Cbw);

  /* Width. For `width: auto` we apply a simplified shrink-to-fit:
   *
   *   1. Lay out the subtree once at the full available inline width.
   *   2. Measure how far right the subtree's content actually reached.
   *   3. Cap the float's width at that measurement (never above the
   *      available width), then re-lay at the shrunk width so inline
   *      wrapping agrees with the chosen box size.
   *
   * This lets `.nav > li { float: left }` menus (and similar) sit
   * side-by-side instead of each occupying a full row. */
  {
    DfBrLayoutFloat *SavedHead = E->FloatsHead;
    DfBrLayoutFloat *SavedTail = E->FloatsTail;
    f32 AvailW  = Cbw - Ml - Mr - Bl - Br - Pl - Pr;
    b32 IsAuto  = S->Width.Auto;

    if (AvailW < 0.0f) AvailW = 0.0f;

    /* Hide the outer float list: the provisional layout is in the
     * float's own local coords and must not see viewport-coord
     * obstructions. */
    E->FloatsHead = DFBR_NULL;
    E->FloatsTail = DFBR_NULL;

    /* Lay the float's subtree out against its REAL containing block
     * width (Cbw), not a recomputed-from-OuterW value. Previously we
     * passed `OuterW` (margin-box width) which meant every percentage
     * descendant - including `width: 41.66%` on the float itself -
     * was resolved against the wrong base, scaling all Bootstrap
     * `.col-sm-5` cells to roughly half of what they should be.
     * BlockBox re-resolves the float's own edges + width against
     * Cbw and arrives at the same W that we computed above, so the
     * two are consistent. */
    DfBrLayoutBlockBox (E, Box, Cbw, 0.0f, 0.0f);

    /* After the subtree layout, Box->ContentRect.W is the float's
     * content-box width. Derive the margin-box OuterW from it,
     * honoring box-sizing: for border-box the authored `width`
     * already includes padding+border, so we must not add them again
     * to the margin-box extent. */
    W = Box->ContentRect.W;

    if (IsAuto) {
      /* Shrink-to-fit: cap content width at measured right edge. */
      f32 Right    = DfBrLayoutSubtreeContentRight (Box);
      f32 Preferred = Right - (Ml + Bl + Pl);
      if (Preferred < 0.0f)    Preferred = 0.0f;
      if (Preferred > AvailW)  Preferred = AvailW;
      if (Preferred + 0.5f < W) {
        W = Preferred;
        /* Width shrunk: re-lay at the smaller content width so
         * inline wrapping and child percentages agree. We stash the
         * shrunk value back onto the style copy by manually driving
         * the block box with a computed CB width = Preferred + edges
         * (the smallest Cbw that yields this content width). This
         * matches how auto-width floats work in real browsers. */
        DfBrLayoutBlockBox (E, Box, W + Pl + Pr + Bl + Br + Ml + Mr,
                            0.0f, 0.0f);
        W = Box->ContentRect.W;
      }
    }
    OuterW = W + Pl + Pr + Bl + Br + Ml + Mr;

    E->FloatsHead = SavedHead;
    E->FloatsTail = SavedTail;
  }
  OuterH = Box->ContentRect.H + Pt + Pb + Bt + Bb + Mt + Mb;

  /* Find the lowest Y at which a OuterW-wide horizontal band fits. */
  Y = YHint;
  for (;;) {
    f32 Avail;
    DfBrLayoutFloatsBandAt (E, Y, CbLeft, CbRight, &L, &R);
    Avail = R - L;
    if (Avail + 0.01f >= OuterW) break;  /* fits */
    {
      f32 Next = NextFloatBottomAfter (E, Y);
      if (Next <= Y) break;              /* no more floats; give up */
      Y = Next;
    }
  }

  if (S->Float == DFBR_CSS_FLOAT_RIGHT) {
    OuterX = R - OuterW;
    if (OuterX < L) OuterX = L;
  } else {
    OuterX = L;
  }
  OuterY = Y;

  /* After the provisional call, Box->ContentRect.X/Y is at
   * (Ml + Bl + Pl, Mt + Bt + Pt). Final content rect anchor is
   * (OuterX + Ml + Bl + Pl, OuterY + Mt + Bt + Pt); translate by
   * (OuterX, OuterY). */
  TranslateSubtree (Box, OuterX, OuterY);

  /* Register so subsequent lines & floats see this one. */
  Rec = (DfBrLayoutFloat *)DfBrLayoutArenaAlloc (&E->Arena, sizeof (*Rec));
  if (Rec != DFBR_NULL) {
    Rec->Box         = Box;
    Rec->Side        = (S->Float == DFBR_CSS_FLOAT_RIGHT)
                       ? DFBR_LAYOUT_FLOAT_SIDE_RIGHT
                       : DFBR_LAYOUT_FLOAT_SIDE_LEFT;
    Rec->OuterLeft   = OuterX;
    Rec->OuterTop    = OuterY;
    Rec->OuterRight  = OuterX + OuterW;
    Rec->OuterBottom = OuterY + OuterH;
    FloatsAppend (E, Rec);
  }

  return OuterY;
}
