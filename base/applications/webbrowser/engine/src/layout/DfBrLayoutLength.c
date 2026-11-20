/** @file
  DfBrLayoutLength.c - resolve a DfBrCssLength to a px value in a
                       layout context.

  The cascade already resolved `font-size` to an absolute px value on
  every computed style, so here we only deal with the axis-lengths
  (widths, margins, paddings, etc.). em/rem are easy (multiply by the
  element's / root's font-size); percent needs the caller to supply
  the containing-block base.
**/

#include "DfBrLayoutInternal.h"

f32
DfBrLayoutResolveLengthPx (
  const DfBrLayoutEngine  *E,
  const DfBrLayoutBox     *Box,
  const DfBrCssLength     *L,
  f32                       PercentBase
  )
{
  f32 Fs;
  f32 Root;
  f32 Vw;
  f32 Vh;

  if (L == DFBR_NULL || L->Auto) return 0.0f;

  Fs   = (Box != DFBR_NULL) ? Box->FontSizePx : 16.0f;
  Root = (E  != DFBR_NULL) ? E->RootFontSizePx : 16.0f;
  Vw   = (E  != DFBR_NULL) ? E->ViewportW      : 0.0f;
  Vh   = (E  != DFBR_NULL) ? E->ViewportH      : 0.0f;

  /* Defer to the CSS-layer calc()/min()/max()/clamp() evaluator when
   * the length was parsed as a deferred expression; it understands
   * the same units we do but also walks operators and nested fns. */
  if (L->CalcExpr != DFBR_NULL) {
    f32 Px = 0.0f;
    DfBrCssLengthToPx (L, Fs, Root, Vw, Vh, PercentBase, &Px);
    return Px;
  }

  switch (L->Unit) {
    case DFBR_CSS_UNIT_NONE:    return L->Value;                  /* bare 0 */
    case DFBR_CSS_UNIT_PX:      return L->Value;
    case DFBR_CSS_UNIT_PT:      return L->Value * (96.0f / 72.0f);
    case DFBR_CSS_UNIT_PC:      return L->Value * 16.0f;
    case DFBR_CSS_UNIT_CM:      return L->Value * (96.0f / 2.54f);
    case DFBR_CSS_UNIT_MM:      return L->Value * (96.0f / 25.4f);
    case DFBR_CSS_UNIT_Q:       return L->Value * (96.0f / 101.6f);
    case DFBR_CSS_UNIT_IN:      return L->Value * 96.0f;
    case DFBR_CSS_UNIT_EM:      return L->Value * Fs;
    case DFBR_CSS_UNIT_REM:     return L->Value * Root;
    case DFBR_CSS_UNIT_EX:      return L->Value * Fs * 0.5f;   /* approx */
    case DFBR_CSS_UNIT_CH:      return L->Value * Fs * 0.5f;   /* approx */
    case DFBR_CSS_UNIT_VW:      return L->Value * 0.01f * Vw;
    case DFBR_CSS_UNIT_VH:      return L->Value * 0.01f * Vh;
    case DFBR_CSS_UNIT_VMIN:    return L->Value * 0.01f * ((Vw < Vh) ? Vw : Vh);
    case DFBR_CSS_UNIT_VMAX:    return L->Value * 0.01f * ((Vw > Vh) ? Vw : Vh);
    case DFBR_CSS_UNIT_PERCENT: return L->Value * 0.01f * PercentBase;
    default:                    return 0.0f;
  }
}

/* Per CSS 2.1 §8.5.2: "If the border style is 'none' or 'hidden', the
 * used border width is 0." Authors often write shorthand borders as
 * `border-bottom: 8px` and rely on the default style to keep the
 * side from drawing; without this check we'd emit an 8px black bar. */
f32
DfBrLayoutResolveBorderPx (
  const DfBrLayoutEngine  *E,
  const DfBrLayoutBox     *Box,
  const DfBrCssBorder     *B,
  f32                       PercentBase
  )
{
  if (B == DFBR_NULL) return 0.0f;
  if (B->Style == DFBR_CSS_BORDER_NONE ||
      B->Style == DFBR_CSS_BORDER_HIDDEN) {
    return 0.0f;
  }
  if (B->Width.Auto) return 0.0f;
  return DfBrLayoutResolveLengthPx (E, Box, &B->Width, PercentBase);
}
