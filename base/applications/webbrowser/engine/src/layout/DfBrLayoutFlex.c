/** @file
  DfBrLayoutFlex.c - minimal `display: flex` row packer.

  Scope (v1, CSS Flexible Box 1):
    * flex-direction: row only. row-reverse / column* are mapped onto
      row so the page at least appears; a future pass will add the
      column main-axis and reverse logic.
    * flex-wrap: nowrap. Items overflow the container when the sum of
      their hypothetical main sizes exceeds InnerWidth.
    * flex-basis / width: used as the item's hypothetical main size.
      When both are `auto` we compute a max-content width via a
      provisional block layout at a very wide budget and measuring
      the farthest right text / replaced edge.
    * flex-grow: items with flex-grow > 0 absorb free space in
      proportion to their share. flex-shrink is not honored.
    * justify-content: flex-start / start (default), flex-end / end,
      center, space-between, space-around. space-evenly behaves like
      space-around.
    * align-items: not implemented. Items land at the container's
      content-top; cross-axis height is the tallest item.

  Out of scope (deferred):
    * min-width / max-width clamping per item.
    * align-items, align-self, align-content.
    * Wrapping (multi-line flex).
    * Row-reverse / column / column-reverse.
    * Percentage resolution for flex-basis on column containers.

  The function is called by DfBrLayoutBlock when the container has
  `display: flex` (or `display: grid`, which is currently aliased to
  flex so Google-style top strips get a sensible horizontal layout).
**/

#include "DfBrLayoutInternal.h"

#include <string.h>

/* ------------------------------------------------------------------
 *  Item collection
 * -----------------------------------------------------------------*/

typedef struct {
  DfBrLayoutBox  *Box;
  f32             Base;     /* hypothetical main size (content box) */
  f32             Grow;     /* flex-grow factor */
  f32             MarginL;  /* resolved outer margin left (0 if auto) */
  f32             MarginR;
  b32             AutoMarginL; /* `margin-left: auto` flag, for packing */
  b32             AutoMarginR;
  f32             BorderL;
  f32             BorderR;
  f32             PaddingL;
  f32             PaddingR;
  f32             FinalMain; /* resolved used main size (content box) */
} FlexItem;

typedef struct {
  FlexItem *Items;
  usize     Count;
  usize     Cap;
} FlexItemList;

static void
FilPush (FlexItemList *L, const FlexItem *It, DfBrLayoutArena *A)
{
  if (L->Count == L->Cap) {
    usize NewCap = (L->Cap == 0) ? 8 : L->Cap * 2;
    FlexItem *NB = (FlexItem *)DfBrLayoutArenaAlloc (A, NewCap * sizeof (FlexItem));
    if (NB == DFBR_NULL) return;
    if (L->Items != DFBR_NULL && L->Count > 0) {
      memcpy (NB, L->Items, L->Count * sizeof (FlexItem));
    }
    L->Items = NB;
    L->Cap   = NewCap;
  }
  L->Items[L->Count++] = *It;
}

static f32
ResolveLenOrZero (const DfBrLayoutEngine *E, const DfBrLayoutBox *Box,
                  const DfBrCssLength *L, f32 Pct)
{
  if (L == DFBR_NULL || L->Auto) return 0.0f;
  return DfBrLayoutResolveLengthPx (E, Box, L, Pct);
}

/* v1 shortcut: cheap "max-content-ish" estimate for an auto-basis
 * item that walks only the item's immediate descendants. Sums text
 * length heuristics and replaced intrinsic widths for inline-like
 * children; falls back to 0 when nothing meaningful is found.
 *
 * Running a full provisional block layout per flex item was too
 * expensive on pages like Google (nested flex containers compound).
 * This is not spec-accurate, but paired with flex-grow it keeps
 * horizontal nav strips packed on a single row without pathological
 * cost. */
static f32
EstimateAutoBasisR (const DfBrLayoutBox *Item, int Depth)
{
  const DfBrLayoutBox *C;
  f32                  Sum = 0.0f;
  f32                  Fs;

  if (Item == DFBR_NULL) return 0.0f;
  /* Cap descent so pathological DOM fragments don't dominate layout
   * time. 4 levels is enough to reach through a typical nav bar's
   * <a><span><icon/><label/></span></a> structure. */
  if (Depth > 4) return 0.0f;

  if (Item->Kind == DFBR_LAYOUT_BOX_REPLACED) {
    f32 Iw = 0.0f, Ih = 0.0f;
    DfBrLayoutIntrinsicReplacedSize (Item, &Iw, &Ih);
    return Iw;
  }

  Fs = (Item->Style != DFBR_NULL) ? Item->Style->FontSize.Value : 16.0f;
  if (Fs <= 0.0f) Fs = 16.0f;

  /* If the item is itself a flex/inline-flex row, remember to add
   * `column-gap` between each pair of real children. Without this,
   * `.brand { display: inline-flex; gap: .6rem }` containing
   * `<span class="mark"></span> lowcoregames` underestimates its
   * intrinsic main size by ~gap px, which made the outer flex
   * packer allocate just enough room for icon + text but not the
   * gap, and the text wrapped onto a second line below the icon. */
  f32 SelfGap = 0.0f;
  b32 SelfIsFlexRow = DFBR_FALSE;
  usize ChildSeen = 0;
  if (Item->Style != DFBR_NULL) {
    DfBrCssDisplay D = Item->Style->Display;
    if (D == DFBR_CSS_DISPLAY_FLEX || D == DFBR_CSS_DISPLAY_INLINE_FLEX) {
      SelfIsFlexRow = DFBR_TRUE;
      if (Item->Style->ColumnGap.Unit == DFBR_CSS_UNIT_PX ||
          Item->Style->ColumnGap.Unit == DFBR_CSS_UNIT_NONE)
      {
        SelfGap = Item->Style->ColumnGap.Value;
      } else if (Item->Style->ColumnGap.Unit == DFBR_CSS_UNIT_EM ||
                 Item->Style->ColumnGap.Unit == DFBR_CSS_UNIT_REM)
      {
        SelfGap = Item->Style->ColumnGap.Value * Fs;
      }
      if (SelfGap < 0.0f) SelfGap = 0.0f;
    }
  }

  for (C = Item->FirstChild; C != DFBR_NULL; C = C->NextSibling) {
    if (SelfIsFlexRow && ChildSeen > 0) Sum += SelfGap;
    ChildSeen++;
    if (C->Kind == DFBR_LAYOUT_BOX_TEXT) {
      /* 0.55em is a reasonable rough average glyph width for Latin
       * text; good enough for packing estimates. */
      Sum += (f32)C->TextLen * Fs * 0.55f;
    } else if (C->Kind == DFBR_LAYOUT_BOX_REPLACED) {
      f32 Iw = 0.0f, Ih = 0.0f;
      DfBrLayoutIntrinsicReplacedSize (C, &Iw, &Ih);
      Sum += Iw;
    } else if (C->Kind == DFBR_LAYOUT_BOX_INLINE ||
               C->Kind == DFBR_LAYOUT_BOX_BLOCK   ||
               C->Kind == DFBR_LAYOUT_BOX_ANON_BLOCK)
    {
      /* Inline / inline-block children contribute their own text
       * advance PLUS the horizontal box-edges authored on them
       * (margin + border + padding). Without this, a site like
       * lowcoregames.com whose nav is `<a>Games</a><a>Support</a>
       * <a>Privacy</a>` with `.nav a { margin-left: 1.4rem }`
       * underestimates by 3 * 22.4 = 67.2 px and flex packs the nav
       * onto two lines. 1rem here is approximated as font-size; we
       * don't have a layout context in this estimator so non-pixel
       * units fall back to CSS 2.1's `1em = font-size` convention. */
      f32 Edges = 0.0f;
      f32 ExplicitW = 0.0f;
      const DfBrCssComputedStyle *Cs = C->Style;
      /* Decorative empty inline boxes (icon dots, pills, badges -
       * e.g. lowcoregames.com's `.brand .mark` span) have a fixed
       * width set in px but no children, so the text-sum path below
       * contributes 0 and the flex item underestimates its content
       * width by mark+gap (~32 px). Pick the declared pixel width
       * directly so the flex container reserves room for them
       * next to the label. */
      if (Cs != DFBR_NULL && C->FirstChild == DFBR_NULL &&
          !Cs->Width.Auto)
      {
        if (Cs->Width.Unit == DFBR_CSS_UNIT_PX ||
            Cs->Width.Unit == DFBR_CSS_UNIT_NONE)
        {
          ExplicitW = Cs->Width.Value;
        } else if (Cs->Width.Unit == DFBR_CSS_UNIT_EM ||
                   Cs->Width.Unit == DFBR_CSS_UNIT_REM)
        {
          ExplicitW = Cs->Width.Value * Fs;
        }
        if (ExplicitW < 0.0f) ExplicitW = 0.0f;
      }
      if (Cs != DFBR_NULL) {
        /* Pixel-units are safe straight from .Value; rem and em
         * resolve to the inline's own font-size for this estimate. */
        f32 Em = (Cs->FontSize.Value > 0.0f) ? Cs->FontSize.Value : Fs;
        #define EDGE_ADD(Len) \
          do { \
            if (!(Len).Auto) { \
              if ((Len).Unit == DFBR_CSS_UNIT_EM || (Len).Unit == DFBR_CSS_UNIT_REM) { \
                Edges += (Len).Value * Em; \
              } else if ((Len).Unit == DFBR_CSS_UNIT_PX || \
                         (Len).Unit == DFBR_CSS_UNIT_NONE) { \
                Edges += (Len).Value; \
              } \
            } \
          } while (0)
        EDGE_ADD (Cs->MarginLeft);
        EDGE_ADD (Cs->MarginRight);
        EDGE_ADD (Cs->PaddingLeft);
        EDGE_ADD (Cs->PaddingRight);
        if (Cs->BorderLeft.Style != DFBR_CSS_BORDER_NONE &&
            Cs->BorderLeft.Style != DFBR_CSS_BORDER_HIDDEN) {
          EDGE_ADD (Cs->BorderLeft.Width);
        }
        if (Cs->BorderRight.Style != DFBR_CSS_BORDER_NONE &&
            Cs->BorderRight.Style != DFBR_CSS_BORDER_HIDDEN) {
          EDGE_ADD (Cs->BorderRight.Width);
        }
        #undef EDGE_ADD
      }
      Sum += Edges + ExplicitW + EstimateAutoBasisR (C, Depth + 1);
    }
  }
  return Sum;
}

static f32
EstimateAutoBasis (const DfBrLayoutBox *Item)
{
  return EstimateAutoBasisR (Item, 0);
}

/* ------------------------------------------------------------------
 *  Subtree translation
 *
 *  Shifts `Root` and every descendant's horizontal coordinates
 *  (content rect and per-fragment text x) by `Dx` px. Used by the
 *  column-flex post-pass in DfBrLayoutBlock.c to realize
 *  align-items:center / flex-end after the regular block layout
 *  has placed children at the left edge of the content band.
 * -----------------------------------------------------------------*/
/* Walk `Root`'s subtree and return the farthest-right pixel touched
 * by any placed text fragment or replaced/block box. Anonymous
 * wrappers that stretched to their parent's full width are ignored
 * in favor of their children so we recover a "content-ish" extent
 * suitable for cross-axis alignment in flex-column. */
f32
DfBrLayoutSubtreeContentRight (const DfBrLayoutBox *Root)
{
  const DfBrLayoutBox *Cur;
  f32                  Max = 0.0f;
  if (Root == DFBR_NULL) return 0.0f;

  /* Preorder seeded with NULL returns Root first, so the loop
   * covers root + every descendant in a single pass. */
  for (Cur = DfBrLayoutBoxNextPreorder (Root, DFBR_NULL);
       Cur != DFBR_NULL;
       Cur = DfBrLayoutBoxNextPreorder (Root, Cur))
  {
    if (Cur->Kind == DFBR_LAYOUT_BOX_TEXT) {
      const DfBrLayoutTextFragment *Tf;
      for (Tf = Cur->FirstTextFragment; Tf != DFBR_NULL; Tf = Tf->Next) {
        f32 R = Tf->X + Tf->Advance;
        if (R > Max) Max = R;
      }
    } else if (Cur->Kind == DFBR_LAYOUT_BOX_REPLACED) {
      f32 R = Cur->ContentRect.X + Cur->ContentRect.W;
      if (R > Max) Max = R;
    }
  }
  return Max;
}

void
DfBrLayoutTranslateSubtreeX (DfBrLayoutBox *Root, f32 Dx)
{
  DfBrLayoutBox          *Cur;
  DfBrLayoutTextFragment *Tf;
  if (Root == DFBR_NULL || Dx == 0.0f) return;

  /* DfBrLayoutBoxNextPreorder seeded with Cur==NULL returns Root
   * itself, so the loop covers the root and every descendant in a
   * single pass. */
  for (Cur = (DfBrLayoutBox *)DfBrLayoutBoxNextPreorder (Root, DFBR_NULL);
       Cur != DFBR_NULL;
       Cur = (DfBrLayoutBox *)DfBrLayoutBoxNextPreorder (Root, Cur))
  {
    Cur->ContentRect.X += Dx;
    for (Tf = Cur->FirstTextFragment; Tf != DFBR_NULL; Tf = Tf->Next) {
      Tf->X += Dx;
    }
  }
}

/* ------------------------------------------------------------------
 *  Public entry (row)
 * -----------------------------------------------------------------*/

f32
DfBrLayoutFlexChildren (
  DfBrLayoutEngine  *E,
  DfBrLayoutBox     *Flex,
  f32                InnerLeft,
  f32                InnerTop,
  f32                InnerWidth
  )
{
  const DfBrCssComputedStyle *Fs = Flex->Style;
  DfBrCssAlign                Justify = DFBR_CSS_ALIGN_START;
  FlexItemList                IL;
  DfBrLayoutBox              *C;
  usize                       I;
  f32                         TotalOuter = 0.0f;
  f32                         TotalGrow  = 0.0f;
  f32                         Free;
  f32                         X;
  f32                         Gap = 0.0f;
  f32                         LeadGap = 0.0f;
  /* Explicit `column-gap` / `gap` between items. Unlike `Gap` above
   * (which is the justify-content derived extra spacing), FixedGap
   * applies unconditionally between every adjacent pair and factors
   * into free-space calculation. Without this, `display:flex;
   * gap: .6rem` had no visual effect, and patterns like
   * lowcoregames.com's `.brand .mark` icon ended up butted against
   * the brand text with no breathing room. */
  f32                         FixedGap = 0.0f;
  f32                         MaxBottom = InnerTop;

  if (Fs != DFBR_NULL) {
    Justify = Fs->JustifyContent;
    FixedGap = ResolveLenOrZero (E, Flex, &Fs->ColumnGap, InnerWidth);
    if (FixedGap < 0.0f) FixedGap = 0.0f;
  }

  memset (&IL, 0, sizeof (IL));

  /* ---- Pass 0: collect items + margins/padding --------------------- */
  for (C = Flex->FirstChild; C != DFBR_NULL; C = C->NextSibling) {
    FlexItem It;
    const DfBrCssComputedStyle *S;

    if (DfBrLayoutIsOutOfFlow (C)) {
      /* Record the flex container's content origin as the box's
       * static position so the positioning pass can honor auto
       * offsets (CSS 2.1 §10.3.7 / §10.6.4). */
      C->StaticX      = InnerLeft;
      C->StaticY      = InnerTop;
      C->HasStaticPos = DFBR_TRUE;
      continue;
    }
    if (C->Kind == DFBR_LAYOUT_BOX_TEXT ||
        C->Kind == DFBR_LAYOUT_BOX_ANON_BLOCK)
    {
      /* Stray text / anonymous wrappers inside a flex container are
       * already blockified at build time for element children; these
       * leftovers are extremely rare. Treat as zero-width placeholders
       * so the pass stays finite. */
      continue;
    }

    memset (&It, 0, sizeof (It));
    It.Box = C;
    S = C->Style;
    if (S != DFBR_NULL) {
      It.MarginL  = ResolveLenOrZero (E, C, &S->MarginLeft,   InnerWidth);
      It.MarginR  = ResolveLenOrZero (E, C, &S->MarginRight,  InnerWidth);
      /* Per CSS Flexbox §8.1, `margin: auto` on the main axis absorbs
       * free space. We record the flag here and apply it in Pass 3;
       * ResolveLenOrZero already returned 0 for auto. */
      It.AutoMarginL = S->MarginLeft.Auto;
      It.AutoMarginR = S->MarginRight.Auto;
      It.BorderL  = DfBrLayoutResolveBorderPx (E, C, &S->BorderLeft,  InnerWidth);
      It.BorderR  = DfBrLayoutResolveBorderPx (E, C, &S->BorderRight, InnerWidth);
      It.PaddingL = ResolveLenOrZero (E, C, &S->PaddingLeft,  InnerWidth);
      It.PaddingR = ResolveLenOrZero (E, C, &S->PaddingRight, InnerWidth);
      It.Grow     = S->FlexGrow;
    }

    FilPush (&IL, &It, &E->Arena);
  }

  if (IL.Count == 0) return 0.0f;

  /* ---- Pass 1: hypothetical main size per item -------------------- */
  {
    b32 IsGrid = (Fs != DFBR_NULL) && (Fs->Display == DFBR_CSS_DISPLAY_GRID);
    for (I = 0; I < IL.Count; I++) {
      FlexItem *It = &IL.Items[I];
      const DfBrCssComputedStyle *S = It->Box->Style;
      f32 Base = -1.0f;
      if (S != DFBR_NULL) {
        if (!S->FlexBasis.Auto) {
          Base = DfBrLayoutResolveLengthPx (E, It->Box, &S->FlexBasis, InnerWidth);
        } else if (!S->Width.Auto) {
          Base = DfBrLayoutResolveLengthPx (E, It->Box, &S->Width, InnerWidth);
        }
      }
      if (Base < 0.0f) {
        /* auto basis -> cheap structural estimate. See
         * EstimateAutoBasis commentary for the trade-off. */
        Base = EstimateAutoBasis (It->Box);
        if (Base > InnerWidth) Base = InnerWidth;
      }
      if (Base < 0.0f) Base = 0.0f;
      It->Base = Base;

      /* Grid->equal-share-flex override: see block comment above the
       * item collection pass. Only items that don't already have an
       * authored grow factor pick this up, so an explicit
       * `flex: 0 0 200px` inside a grid still pins its width. */
      if (IsGrid && It->Grow <= 0.0f) {
        It->Grow = 1.0f;
        /* Also reset the hypothetical base to 0 so the grow pass
         * actually distributes the free space equally; otherwise
         * a long text child (e.g. a description paragraph) bloats
         * one column and starves its sibling. This matches how
         * `grid-template-columns: 1fr 1fr` sizes tracks. */
        It->Base = 0.0f;
      }

      TotalGrow  += (It->Grow > 0.0f) ? It->Grow : 0.0f;
    }
  }

  /* ---- Pass 1b: floor non-grow item Base values ---------------------
   * Lifting tiny items BEFORE the free-space calculation makes sure
   * grow items get only what truly remains. If we floored after the
   * grow distribution, a grow item would end up with container width
   * minus the HYPOTHETICAL (pre-floor) sizes, which over-allocates
   * space and spills its descendants off the viewport. Grow items
   * are exempt because they will size themselves from Free anyway. */
  {
    f32 MinItem = InnerWidth * 0.10f;
    if (MinItem < 40.0f) MinItem = 40.0f;
    if (MinItem > 120.0f) MinItem = 120.0f;
    for (I = 0; I < IL.Count; I++) {
      FlexItem *It = &IL.Items[I];
      if (It->Grow <= 0.0f && It->Base < MinItem) It->Base = MinItem;
    }
  }

  for (I = 0; I < IL.Count; I++) {
    FlexItem *It = &IL.Items[I];
    TotalOuter += It->MarginL + It->BorderL + It->PaddingL + It->Base
                + It->PaddingR + It->BorderR + It->MarginR;
  }

  /* ---- Pass 2: distribute free space to grow items ---------------- */
  /* Reserve N-1 fixed gaps off the available width before the free-
   * space calculation so that `gap: Xpx` actually shrinks the pool
   * that flex-grow / justify-content compete for, matching browser
   * behaviour. */
  {
    f32 GapTotal = (IL.Count > 1) ? FixedGap * (f32)(IL.Count - 1) : 0.0f;
    Free = InnerWidth - TotalOuter - GapTotal;
  }

  /* If hypothetical main sizes overflow the container, perform a
   * simplified flex-shrink pass: split the overflow across items
   * proportional to their Base size (approximates the spec's
   * `flex-shrink: 1` default scaled by basis). This keeps a single
   * long item (e.g., a paragraph <a>) from pushing a sibling column
   * out of the viewport.
   *
   * Crucially, we respect a per-item MIN-CONTENT floor: we never
   * shrink an item below its estimated max-content text width. If we
   * ignore that floor, text painted inside the (too narrow) item
   * overflows the item's right edge and collides with the next
   * item's text - which is exactly the "Privacy Terms" collision we
   * hit on Google's footer. Items with a small MinContent (icons)
   * can still be shrunk hard; only text-heavy items get protected.
   * When the protected sum itself overflows we fall back to the
   * proportional shrink for the remainder so we at least stop
   * cascading the overflow further. */
  if (Free < 0.0f) {
    f32 *MinC;
    MinC = (f32 *)DfBrLayoutArenaAlloc (&E->Arena, IL.Count * sizeof (f32));
    if (MinC != DFBR_NULL) {
      f32 TotalShrinkable = 0.0f;
      f32 FloorSum        = 0.0f;
      f32 Overflow        = -Free;
      /* Round 1: cap shrink at each item's min-content. */
      for (I = 0; I < IL.Count; I++) {
        FlexItem *It = &IL.Items[I];
        f32 Mc = EstimateAutoBasis (It->Box);
        if (Mc < 0.0f) Mc = 0.0f;
        if (Mc > It->Base) Mc = It->Base;  /* never increase via floor */
        MinC[I] = Mc;
        FloorSum += Mc + It->MarginL + It->BorderL + It->PaddingL
                     + It->PaddingR + It->BorderR + It->MarginR;
        TotalShrinkable += (It->Base - Mc);
      }

      if (TotalShrinkable >= Overflow && TotalShrinkable > 0.0f) {
        /* Overflow fits entirely in the shrink budget above the
         * floor. Distribute proportionally to each item's shrink
         * headroom so tiny items (icons near their floor) aren't
         * asked to give up space they don't have. */
        for (I = 0; I < IL.Count; I++) {
          FlexItem *It = &IL.Items[I];
          f32 Head  = It->Base - MinC[I];
          f32 Share = (Head / TotalShrinkable) * Overflow;
          It->Base -= Share;
          if (It->Base < MinC[I]) It->Base = MinC[I];
        }
        Free = 0.0f;
      } else {
        /* The shrink budget alone can't absorb the overflow, which
         * means even at min-content the row doesn't fit. Collapse
         * everyone to their floor and let the remaining overflow
         * spill; that's visually identical to the old proportional
         * shrink for extreme cases, but text-heavy items no longer
         * get squashed below their glyph width. */
        for (I = 0; I < IL.Count; I++) IL.Items[I].Base = MinC[I];
        {
          f32 GapTotal2 = (IL.Count > 1) ? FixedGap * (f32)(IL.Count - 1) : 0.0f;
          Free = InnerWidth - FloorSum - GapTotal2;
        }
      }
    } else {
      /* OOM fallback: original proportional shrink, item widths may
       * go below min-content (old behavior). */
      f32 Overflow = -Free;
      f32 TotalBase = 0.0f;
      for (I = 0; I < IL.Count; I++) TotalBase += IL.Items[I].Base;
      if (TotalBase > 0.0f) {
        for (I = 0; I < IL.Count; I++) {
          FlexItem *It = &IL.Items[I];
          f32 Share = (It->Base / TotalBase) * Overflow;
          It->Base -= Share;
          if (It->Base < 0.0f) It->Base = 0.0f;
        }
        Free = 0.0f;
      }
    }
  }

  if (Free > 0.0f && TotalGrow > 0.0f) {
    f32 Unit = Free / TotalGrow;
    for (I = 0; I < IL.Count; I++) {
      FlexItem *It = &IL.Items[I];
      f32 Add = (It->Grow > 0.0f) ? It->Grow * Unit : 0.0f;
      It->FinalMain = It->Base + Add;
    }
  } else {
    for (I = 0; I < IL.Count; I++) IL.Items[I].FinalMain = IL.Items[I].Base;
  }

  /* Non-grow items were floored in Pass 1b; Free is already net of
   * those uplifts. Grow items receive their own share and don't need
   * a post-hoc floor. */

  /* ---- Pass 3a: distribute remaining free space into auto margins  */
  /* Count auto margins on the main axis. If any item has at least one,
   * those absorb all of the free space (CSS Flexbox §8.1) and
   * justify-content is ignored. This is the mechanism pages use to
   * right-align a group: `<right-group style="margin-left: auto;">`. */
  if (Free > 0.0f) {
    usize AutoCount = 0;
    for (I = 0; I < IL.Count; I++) {
      if (IL.Items[I].AutoMarginL) AutoCount++;
      if (IL.Items[I].AutoMarginR) AutoCount++;
    }
    if (AutoCount > 0) {
      f32 Share = Free / (f32)AutoCount;
      for (I = 0; I < IL.Count; I++) {
        if (IL.Items[I].AutoMarginL) IL.Items[I].MarginL += Share;
        if (IL.Items[I].AutoMarginR) IL.Items[I].MarginR += Share;
      }
      Free = 0.0f;
    }
  }

  /* ---- Pass 3b: resolve justify-content (no-grow, no-auto-margin) - */
  if (Free > 0.0f && TotalGrow == 0.0f) {
    switch (Justify) {
      case DFBR_CSS_ALIGN_CENTER:
        LeadGap = Free * 0.5f;
        break;
      /* Both `end` (CSS 3 Align) and `flex-end` (CSS Flexbox) pack
       * items after the main-axis free space. Same for start. */
      case DFBR_CSS_ALIGN_END:
      case DFBR_CSS_ALIGN_FLEX_END:
        LeadGap = Free;
        break;
      case DFBR_CSS_ALIGN_START:
      case DFBR_CSS_ALIGN_FLEX_START:
        LeadGap = 0.0f;
        break;
      case DFBR_CSS_ALIGN_SPACE_BETWEEN:
        if (IL.Count > 1) Gap = Free / (f32)(IL.Count - 1);
        break;
      case DFBR_CSS_ALIGN_SPACE_AROUND:
      case DFBR_CSS_ALIGN_SPACE_EVENLY:
        if (IL.Count > 0) {
          Gap = Free / (f32)IL.Count;
          LeadGap = Gap * 0.5f;
        }
        break;
      default:
        break;
    }
  }

  /* ---- Pass 4: commit layout ------------------------------------- */
  X = InnerLeft + LeadGap;
  for (I = 0; I < IL.Count; I++) {
    FlexItem *It = &IL.Items[I];
    /* Place each item as a block at its assigned main size. The block
     * layout takes its margin/border/padding from the item's CSS, so
     * we just hand it a "containing block width" equal to the outer
     * width we are reserving. */
    {
      f32 Outer = It->MarginL + It->BorderL + It->PaddingL + It->FinalMain
                + It->PaddingR + It->BorderR + It->MarginR;
      /* Shim: temporarily force `width` and main-axis `margin-left` /
       * `margin-right` on the item so block layout picks up the
       * flex-assigned values instead of re-running auto-margin
       * centering (which would undo the packing we just decided).
       * Restored immediately after to keep computed styles clean.
       *
       * FinalMain is a content-box width (see Pass 1 / 2 above).
       * Block layout, however, interprets `Width` through the item's
       * own `box-sizing`: with `border-box`, it strips padding+border
       * out of the width before laying out content. That would cause
       * a double subtraction here and collapse the child's content
       * area to zero (observed on lowcoregames.com buttons - "See
       * the games" ended up rendered as an empty 40 px strip because
       * padding was eaten twice). Shim box-sizing too, and restore
       * below. */
      DfBrCssComputedStyle *Mut = (DfBrCssComputedStyle *)It->Box->Style;
      DfBrCssLength   SavedW  = { 0 };
      DfBrCssLength   SavedMl = { 0 };
      DfBrCssLength   SavedMr = { 0 };
      DfBrCssBoxSizing SavedBs = DFBR_CSS_BOX_CONTENT;
      b32             Restore = DFBR_FALSE;
      if (Mut != DFBR_NULL) {
        SavedW  = Mut->Width;
        SavedMl = Mut->MarginLeft;
        SavedMr = Mut->MarginRight;
        SavedBs = Mut->BoxSizing;
        Mut->Width.Auto  = DFBR_FALSE;
        Mut->Width.Value = It->FinalMain;
        Mut->Width.Unit  = DFBR_CSS_UNIT_PX;
        Mut->MarginLeft.Auto   = DFBR_FALSE;
        Mut->MarginLeft.Value  = It->MarginL;
        Mut->MarginLeft.Unit   = DFBR_CSS_UNIT_PX;
        Mut->MarginRight.Auto  = DFBR_FALSE;
        Mut->MarginRight.Value = It->MarginR;
        Mut->MarginRight.Unit  = DFBR_CSS_UNIT_PX;
        Mut->BoxSizing   = DFBR_CSS_BOX_CONTENT;
        Restore = DFBR_TRUE;
      }

      DfBrLayoutBlockBox (E, It->Box, Outer, X, InnerTop);

      if (Restore) {
        Mut->Width       = SavedW;
        Mut->MarginLeft  = SavedMl;
        Mut->MarginRight = SavedMr;
        Mut->BoxSizing   = SavedBs;
      }

      {
        f32 Bottom = It->Box->ContentRect.Y + It->Box->ContentRect.H
                   + It->Box->Padding.Bottom + It->Box->Border.Bottom
                   + It->Box->Margin.Bottom;
        if (Bottom > MaxBottom) MaxBottom = Bottom;
      }
      X += Outer + Gap + ((I + 1 < IL.Count) ? FixedGap : 0.0f);
    }
  }

  return MaxBottom - InnerTop;
}
