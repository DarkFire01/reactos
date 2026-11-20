/** @file
  DfBrLayoutInline.c - inline formatting context: lay out a run of
                       inline children inside a block container.

  Strategy:
    * Walk the inline subtree in order, producing a stream of
      "inline fragments". A fragment is a leaf that consumes advance
      on a line (TEXT word, single space, replaced box) or is a
      control signal (inline open/close, forced line break for
      <br>, etc.).
    * Pack fragments onto successive "line boxes", wrapping at word
      boundaries. Line height per line = max(line-height) of the
      inline boxes that contribute to that line, or font-size * 1.2
      as a fallback.
    * When a word wouldn't fit on the remaining space, start a new
      line and put the word there. If a single word is wider than
      the container, it overflows (no hyphenation).
    * <br> elements produce a FragBreak that forces a new line even
      on an otherwise-non-full current line.
    * text-align: the packer collects every placement it makes on
      the current line; at line close (wrap, break, end), it shifts
      those placements horizontally by an alignment offset derived
      from the line's used width, the container's content width, and
      the computed text-align value of the containing block.
    * Text metrics: we consult DfBrFontGetDefaultEngine() for a face
      matching the computed font-family/weight/style/size. On Windows
      that resolves to DirectWrite, which shapes the run via
      IDWriteTextLayout and returns real advance + per-face ascent /
      descent / line-gap. If no backend is linked (future UEFI build
      before a font library drops in) we fall back to a 0.5 * em-per-
      char approximation and a 1.2 * font-size line-height.
    * Whitespace-only text fragments between words collapse; leading
      whitespace on a line is dropped.
**/

#include "DfBrLayoutInternal.h"

#include "../font/DfBrFont.h"

#include <string.h>

/* ------------------------------------------------------------------
 *  Font engine bridge
 *
 *  We consult the process-wide default font engine (DirectWrite on
 *  Windows) lazily. If the engine is unavailable we fall back to a
 *  rough em-based approximation so layout stays finite.
 * -----------------------------------------------------------------*/

static DfBrFontStyle
MapFontStyle (DfBrCssFontStyle S) {
  switch (S) {
    case DFBR_CSS_FSTYLE_ITALIC:  return DFBR_FONT_STYLE_ITALIC;
    case DFBR_CSS_FSTYLE_OBLIQUE: return DFBR_FONT_STYLE_OBLIQUE;
    default:                      return DFBR_FONT_STYLE_NORMAL;
  }
}

/* Resolve a face for the style at this font-size, or NULL if the
 * engine is unavailable / resolution failed. */
static DfBrFontFace *
ResolveFaceFor (const DfBrCssComputedStyle *Style, f32 FontSizePx) {
  DfBrFontEngine *E = DfBrFontGetDefaultEngine ();
  if (E == DFBR_NULL || Style == DFBR_NULL) return DFBR_NULL;
  return DfBrFontEngineResolve (E,
                                Style->FontFamilies,
                                Style->FontFamilyCount,
                                Style->FontWeight,
                                MapFontStyle (Style->FontStyle),
                                FontSizePx);
}

/* Approximate ASCII advance per character at a given font-size, used
 * only when no backend is linked. */
static f32
FallbackCharAdvancePx (f32 FontSizePx) {
  return FontSizePx * 0.5f;
}

/* Measure one shaped text run. Tries the font engine first; falls
 * back to a per-byte 0.5em approximation. */
static f32
MeasureRun (DfBrFontFace *Face, const char *S, usize Len, f32 FontSizePx) {
  if (Face != DFBR_NULL) {
    return DfBrFontFaceMeasureUtf8 (Face, S, Len);
  }
  return (f32)Len * FallbackCharAdvancePx (FontSizePx);
}

/* ------------------------------------------------------------------
 *  Fragment stream
 * -----------------------------------------------------------------*/

typedef enum {
  FragWord,         /* a run of non-space characters from a TEXT box */
  FragSpace,        /* a single collapsed space between words */
  FragReplaced,     /* a replaced inline (image etc.) */
  FragInlineStart,  /* open an inline element (for container geometry) */
  FragInlineEnd,    /* close the most recent inline element */
  FragBreak,        /* <br> - forced line break */
  FragSpacer        /* non-collapsing horizontal advance (inline-block
                     * margin/padding lead-in and trail-out) */
} FragKind;

typedef struct {
  FragKind        Kind;
  DfBrLayoutBox  *Box;       /* owning inline/replaced/text box */
  const char     *Str;       /* for FragWord / FragSpace (points into arena) */
  usize           Len;
  f32             Advance;   /* px width */
  f32             FontSizePx;
  f32             LineHeightPx;
  f32             AscentPx;   /* text: alphabetic-baseline offset from Y; 0 otherwise */
} Fragment;

typedef struct {
  Fragment *Items;
  usize     Count;
  usize     Cap;
} FragList;

static void
FragPush (FragList *L, const Fragment *F, DfBrLayoutArena *A) {
  /* Arena-backed dynamic array: on grow, copy into a new arena block.
   * We lose the old allocation (arena can't free), but fragments are
   * short-lived compared to the overall engine lifetime, and the
   * worst-case waste is bounded by the document size. */
  if (L->Count == L->Cap) {
    usize     NewCap = (L->Cap == 0) ? 32 : L->Cap * 2;
    Fragment *NB     = (Fragment *)DfBrLayoutArenaAlloc (A, NewCap * sizeof (Fragment));
    if (NB == DFBR_NULL) return;
    if (L->Items != DFBR_NULL && L->Count > 0) {
      memcpy (NB, L->Items, L->Count * sizeof (Fragment));
    }
    L->Items = NB;
    L->Cap   = NewCap;
  }
  L->Items[L->Count++] = *F;
}

/* Placements on the current line. The packer records every text
 * fragment and every replaced box it anchors so a subsequent
 * text-align shift can move them all in one pass. */
typedef struct {
  DfBrLayoutTextFragment *Tf;       /* non-NULL for a word placement */
  DfBrLayoutBox          *Replaced; /* non-NULL for a replaced placement */
} LinePlacement;

typedef struct {
  LinePlacement *Items;
  usize          Count;
  usize          Cap;
} PlacementList;

static void
PlacementPush (PlacementList *L,
               DfBrLayoutTextFragment *Tf,
               DfBrLayoutBox *Rep,
               DfBrLayoutArena *A)
{
  if (L->Count == L->Cap) {
    usize          NewCap = (L->Cap == 0) ? 16 : L->Cap * 2;
    LinePlacement *NB     = (LinePlacement *)
      DfBrLayoutArenaAlloc (A, NewCap * sizeof (LinePlacement));
    if (NB == DFBR_NULL) return;
    if (L->Items != DFBR_NULL && L->Count > 0) {
      memcpy (NB, L->Items, L->Count * sizeof (LinePlacement));
    }
    L->Items = NB;
    L->Cap   = NewCap;
  }
  L->Items[L->Count].Tf       = Tf;
  L->Items[L->Count].Replaced = Rep;
  L->Count++;
}

/* "normal" line-height: use DirectWrite's ascent+descent+gap if we
 * can resolve a face, else fall back to the CSS rule-of-thumb
 * 1.2 * font-size. */
static f32
NormalLineHeight (const DfBrCssComputedStyle *Style, f32 FontSizePx) {
  DfBrFontFace    *Face = ResolveFaceFor (Style, FontSizePx);
  DfBrFontMetrics  M;
  f32              H;
  if (Face == DFBR_NULL) return FontSizePx * 1.2f;
  DfBrFontFaceGetMetrics (Face, &M);
  H = M.AscentPx + M.DescentPx + M.LineGapPx;
  if (H <= 0.0f) return FontSizePx * 1.2f;
  return H;
}

static f32
LineHeightOf (const DfBrCssComputedStyle *Style, f32 FontSizePx) {
  if (Style == DFBR_NULL) return NormalLineHeight (Style, FontSizePx);
  switch (Style->LineHeight.Kind) {
    case DFBR_CSS_LINEHEIGHT_NORMAL:
      return NormalLineHeight (Style, FontSizePx);
    case DFBR_CSS_LINEHEIGHT_NUMBER:
      return FontSizePx * Style->LineHeight.Number;
    case DFBR_CSS_LINEHEIGHT_LENGTH: {
      /* Length-resolve against FontSizePx for em, else px. Percent is
       * rare here; fall back to FontSizePx. */
      const DfBrCssLength *L = &Style->LineHeight.Length;
      if (L->Auto) return NormalLineHeight (Style, FontSizePx);
      switch (L->Unit) {
        case DFBR_CSS_UNIT_PX:  return L->Value;
        case DFBR_CSS_UNIT_EM:  return L->Value * FontSizePx;
        case DFBR_CSS_UNIT_PERCENT: return L->Value * 0.01f * FontSizePx;
        default: return L->Value; /* best effort */
      }
    }
  }
  return NormalLineHeight (Style, FontSizePx);
}

/* Map the computed text-align value to the resolved left-to-right
 * anchor (left, center, right). v1 assumes LTR writing mode, so
 * `start` and `justify` fall back to left; `end` to right. */
static DfBrCssTextAlign
ResolveTextAlign (const DfBrCssComputedStyle *S) {
  if (S == DFBR_NULL) return DFBR_CSS_TALIGN_LEFT;
  switch (S->TextAlign) {
    case DFBR_CSS_TALIGN_CENTER: return DFBR_CSS_TALIGN_CENTER;
    case DFBR_CSS_TALIGN_RIGHT:
    case DFBR_CSS_TALIGN_END:    return DFBR_CSS_TALIGN_RIGHT;
    default:                     return DFBR_CSS_TALIGN_LEFT;
  }
}

/* Apply `text-transform` to a text run. ASCII-only for now: our
 * common call sites are reset / hero pills / nav labels that stick to
 * Latin. Non-ASCII bytes pass through unchanged so UTF-8 sequences are
 * preserved. The returned buffer is arena-allocated when a transform
 * is active, otherwise the caller keeps the original pointer. */
static const char *
ApplyTextTransform (DfBrCssTextTransform Tt, const char *S, usize N,
                    DfBrLayoutArena *A)
{
  char  *B;
  usize  I;
  b32    AtWordStart = DFBR_TRUE;
  if (Tt == DFBR_CSS_TTRANSFORM_NONE || S == DFBR_NULL || N == 0) return S;
  B = (char *)DfBrLayoutArenaAlloc (A, N);
  if (B == DFBR_NULL) return S;
  for (I = 0; I < N; I++) {
    char C = S[I];
    switch (Tt) {
      case DFBR_CSS_TTRANSFORM_UPPERCASE:
        B[I] = (C >= 'a' && C <= 'z') ? (char)(C - 32) : C;
        break;
      case DFBR_CSS_TTRANSFORM_LOWERCASE:
        B[I] = (C >= 'A' && C <= 'Z') ? (char)(C + 32) : C;
        break;
      case DFBR_CSS_TTRANSFORM_CAPITALIZE:
        if (C == ' ' || C == '\t' || C == '\n' || C == '\r') {
          B[I] = C;
          AtWordStart = DFBR_TRUE;
        } else if (AtWordStart && C >= 'a' && C <= 'z') {
          B[I] = (char)(C - 32);
          AtWordStart = DFBR_FALSE;
        } else {
          B[I] = C;
          AtWordStart = DFBR_FALSE;
        }
        break;
      default:
        B[I] = C;
        break;
    }
  }
  return B;
}

/* Split the text in a TEXT box into Word/Space fragments. One face
 * is resolved for the entire box so we don't pay for a DWrite lookup
 * per word. */
static void
EmitTextFragments (FragList *FL, DfBrLayoutArena *A,
                   DfBrLayoutBox *Tb, f32 FontSizePx, f32 LineHeightPx)
{
  const char     *Raw   = Tb->TextData;
  DfBrCssTextTransform Tt = (Tb->Style != DFBR_NULL)
                            ? Tb->Style->TextTransform
                            : DFBR_CSS_TTRANSFORM_NONE;
  const char     *S     = ApplyTextTransform (Tt, Raw, Tb->TextLen, A);
  usize           I     = 0;
  usize           N     = Tb->TextLen;
  DfBrFontFace   *Face  = ResolveFaceFor (Tb->Style, FontSizePx);
  DfBrFontMetrics Fm;
  /* Pre-measure the advance of one space once; DWrite will give us a
   * proper width (non-zero even for " ") and the fallback path
   * matches the old 0.5em behavior. */
  f32             SpaceAdv;
  f32             AscentPx;
  Fragment        F;

  if (Face != DFBR_NULL) {
    SpaceAdv = DfBrFontFaceMeasureUtf8 (Face, " ", 1);
    if (SpaceAdv <= 0.0f) SpaceAdv = FallbackCharAdvancePx (FontSizePx);
    DfBrFontFaceGetMetrics (Face, &Fm);
    AscentPx = Fm.AscentPx;
    if (AscentPx <= 0.0f) AscentPx = FontSizePx * 0.8f;
  } else {
    SpaceAdv = FallbackCharAdvancePx (FontSizePx);
    AscentPx = FontSizePx * 0.8f;
  }

  memset (&F, 0, sizeof (F));
  F.Box          = Tb;
  F.FontSizePx   = FontSizePx;
  F.LineHeightPx = LineHeightPx;
  F.AscentPx     = AscentPx;

  while (I < N) {
    if (S[I] == ' ') {
      F.Kind    = FragSpace;
      F.Str     = S + I;
      F.Len     = 1;
      F.Advance = SpaceAdv;
      FragPush (FL, &F, A);
      I++;
    } else {
      usize Start = I;
      while (I < N && S[I] != ' ') I++;
      F.Kind    = FragWord;
      F.Str     = S + Start;
      F.Len     = I - Start;
      F.Advance = MeasureRun (Face, F.Str, F.Len, FontSizePx);
      FragPush (FL, &F, A);
    }
  }
}

/* Detect a <br> element. It's a void inline element; the HTML parser
 * emits it as an empty INLINE box. We translate it to a single
 * FragBreak during flatten. */
static b32
IsBrElement (const DfBrLayoutBox *Box) {
  return (Box != DFBR_NULL &&
          Box->DomNode != DFBR_NULL &&
          Box->DomNode->Kind == DFBR_HTML_NODE_ELEMENT &&
          Box->DomNode->Tag  == DFBR_HTML_TAG_BR);
}

/* Recursive flatten: walk inline children and produce fragments.
 *
 * `E` and `Cbw` are needed so we can resolve non-pixel lengths
 * (rem / em / %) for inline box edges. Without this, author CSS like
 * `.nav a { margin-left: 1.4rem }` was read as 1.4 px because
 * DfBrCssLength stores a raw (value, unit) pair that only becomes
 * pixels once DfBrLayoutResolveLengthPx walks it against the engine's
 * context. */
static void
Flatten (DfBrLayoutEngine *E, DfBrLayoutBox *Box,
         FragList *FL, DfBrLayoutArena *A,
         f32 ParentFs, f32 ParentLh, f32 Cbw)
{
  DfBrLayoutBox *C;

  if (Box->Kind == DFBR_LAYOUT_BOX_TEXT) {
    f32 Fs = (Box->Style != DFBR_NULL) ? Box->Style->FontSize.Value : ParentFs;
    f32 Lh = LineHeightOf (Box->Style, Fs);
    /* Fall back to parent inline's font if text had no computed style
     * (shouldn't happen, but be safe). */
    if (Fs <= 0.0f) Fs = ParentFs;
    if (Lh <= 0.0f) Lh = ParentLh;
    Box->FontSizePx       = Fs;
    Box->TextAdvance      = 0.0f;
    /* Paint fragments are arena-allocated; on re-layout we just drop
     * the previous list (the arena isn't freed per run, so memory
     * stays, but the entries would otherwise duplicate). */
    Box->FirstTextFragment = DFBR_NULL;
    Box->LastTextFragment  = DFBR_NULL;
    EmitTextFragments (FL, A, Box, Fs, Lh);
    return;
  }
  if (Box->Kind == DFBR_LAYOUT_BOX_REPLACED) {
    Fragment F;
    f32      W = 0.0f, H = 0.0f;
    f32      Iw = 0.0f, Ih = 0.0f;
    f32      Ml = 0.0f, Mr = 0.0f;
    /* Replaced inlines honor horizontal margin (CSS 2.1 §10.3.2 / §9.5
     * for atomic inlines). Authors rely on this to space adjacent
     * form controls and images with `margin: 0 4px` etc. We emit
     * FragSpacer fragments on either side of the FragReplaced so the
     * line packer just adds them to X. */
    if (Box->Style != DFBR_NULL) {
      if (!Box->Style->MarginLeft.Auto)
        Ml = DfBrLayoutResolveLengthPx (E, Box, &Box->Style->MarginLeft,  Cbw);
      if (!Box->Style->MarginRight.Auto)
        Mr = DfBrLayoutResolveLengthPx (E, Box, &Box->Style->MarginRight, Cbw);
    }
    if (Ml > 0.0f) {
      memset (&F, 0, sizeof (F));
      F.Kind         = FragSpacer;
      F.Box          = Box;
      F.Advance      = Ml;
      F.FontSizePx   = ParentFs;
      F.LineHeightPx = ParentLh;
      FragPush (FL, &F, A);
    }
    /* Honor explicit width/height if set; otherwise the element's
     * intrinsic size (width/height attrs, viewBox, or a tag-specific
     * default for form controls). */
    if (Box->Style != DFBR_NULL) {
      if (!Box->Style->Width.Auto)  W = Box->Style->Width.Value;
      if (!Box->Style->Height.Auto) H = Box->Style->Height.Value;
    }
    DfBrLayoutIntrinsicReplacedSize (Box, &Iw, &Ih);
    if (W <= 0.0f && Ih > 0.0f && H > 0.0f && Iw > 0.0f) {
      /* Preserve intrinsic aspect when only height was authored. */
      W = H * (Iw / Ih);
    } else if (W <= 0.0f) {
      W = Iw;
    }
    if (H <= 0.0f && Iw > 0.0f && W > 0.0f && Ih > 0.0f) {
      H = W * (Ih / Iw);
    } else if (H <= 0.0f) {
      H = Ih;
    }
    if (W <= 0.0f) W = ParentFs * 2.0f;
    if (H <= 0.0f) H = ParentFs * 2.0f;

    /* CSS 2.1 §10.4: honor max-width / min-width on atomic inline
     * replaced elements. Bootstrap's `.img-responsive { display:block;
     * max-width:100%; height:auto }` relies on this; because we
     * currently route every <img> through the inline replaced path
     * regardless of its computed `display`, the block-level max-width
     * clamp never ran and the 263-px banner would overflow a 152-px
     * Bootstrap column on reactos.org. Scale H by the intrinsic
     * aspect when height was `auto`, matching the CSS 2.1 §10.6.2
     * behavior for replaced elements whose one dimension is specified. */
    if (Box->Style != DFBR_NULL) {
      const DfBrCssComputedStyle *S = Box->Style;
      f32 Edges = 0.0f;
      DfBrCssLength MaxW = S->MaxWidth;
      DfBrCssLength MinW = S->MinWidth;
      if (S->BoxSizing == DFBR_CSS_BOX_BORDER) {
        Edges = DfBrLayoutResolveLengthPx (E, Box, &S->PaddingLeft,  Cbw)
              + DfBrLayoutResolveLengthPx (E, Box, &S->PaddingRight, Cbw)
              + DfBrLayoutResolveBorderPx (E, Box, &S->BorderLeft,   Cbw)
              + DfBrLayoutResolveBorderPx (E, Box, &S->BorderRight,  Cbw);
        if (Edges < 0.0f) Edges = 0.0f;
      }
      if (!MaxW.Auto) {
        f32 Max = DfBrLayoutResolveLengthPx (E, Box, &MaxW, Cbw) - Edges;
        if (Max >= 0.0f && W > Max) {
          if (S->Height.Auto && Iw > 0.0f && Ih > 0.0f) {
            H = Ih * (Max / Iw);
          }
          W = Max;
        }
      }
      if (!MinW.Auto) {
        f32 Min = DfBrLayoutResolveLengthPx (E, Box, &MinW, Cbw) - Edges;
        if (Min >= 0.0f && W < Min) {
          if (S->Height.Auto && Iw > 0.0f && Ih > 0.0f) {
            H = Ih * (Min / Iw);
          }
          W = Min;
        }
      }
    }

    memset (&F, 0, sizeof (F));
    F.Kind         = FragReplaced;
    F.Box          = Box;
    F.Advance      = W;
    F.FontSizePx   = ParentFs;
    F.LineHeightPx = (H > ParentLh) ? H : ParentLh;
    Box->ContentRect.W = W;
    Box->ContentRect.H = H;
    FragPush (FL, &F, A);
    if (Mr > 0.0f) {
      memset (&F, 0, sizeof (F));
      F.Kind         = FragSpacer;
      F.Box          = Box;
      F.Advance      = Mr;
      F.FontSizePx   = ParentFs;
      F.LineHeightPx = ParentLh;
      FragPush (FL, &F, A);
    }
    return;
  }
  if (Box->Kind == DFBR_LAYOUT_BOX_INLINE) {
    Fragment F;
    f32 Fs = (Box->Style != DFBR_NULL) ? Box->Style->FontSize.Value : ParentFs;
    f32 Lh = LineHeightOf (Box->Style, Fs);
    /* Display-variant flags kept for parity with the inline-sized
     * decorator and the right-edge box-edge pass below. */
    b32 IsInlineBlock = (Box->Style != DFBR_NULL) &&
                        (Box->Style->Display == DFBR_CSS_DISPLAY_INLINE_BLOCK);
    b32 IsInlineFlex  = (Box->Style != DFBR_NULL) &&
                        (Box->Style->Display == DFBR_CSS_DISPLAY_INLINE_FLEX);
    (void)IsInlineFlex;
    Box->FontSizePx = Fs;

    /* <br> is special: collapse the whole inline (it has no useful
     * children anyway, per HTML spec) into a single FragBreak. */
    if (IsBrElement (Box)) {
      memset (&F, 0, sizeof (F));
      F.Kind         = FragBreak;
      F.Box          = Box;
      F.FontSizePx   = Fs;
      F.LineHeightPx = Lh;
      FragPush (FL, &F, A);
      return;
    }

    /* Decorative empty inline atoms - icon dots, pills, bullets -
     * that authors size with `width`/`height` and paint via a
     * background. Landing pages style these three equivalent ways:
     *
     *   1. `<span class="mark"></span>`  with default `display:inline`
     *      + explicit width/height. Not spec-correct (inline boxes
     *      ignore width/height in real browsers) but widespread and
     *      visually relied upon - Ladybird treats the same shape as
     *      an atomic inline replaced element for paint purposes.
     *   2. `display: inline-block` + width/height (the textbook
     *      way). Common for chat bubble indicators and badges.
     *   3. `display: inline-flex` + width/height. Used when the
     *      element is also a flex container for a child glyph.
     *
     * All three shapes have zero visible content; the icon is drawn
     * entirely from the box's background-color / background-image /
     * gradient / border. Without this path they fall through the
     * regular inline walk, never emit geometry, and their gradient
     * paint is skipped. Match rule: no children AND at least one of
     * width/height explicitly set. */
    if (Box->Style != DFBR_NULL &&
        Box->FirstChild == DFBR_NULL &&
        (!Box->Style->Width.Auto || !Box->Style->Height.Auto))
    {
      const DfBrCssComputedStyle *S = Box->Style;
      if (!S->Width.Auto || !S->Height.Auto) {
        f32 W = 0.0f, H = 0.0f;
        if (!S->Width.Auto)
          W = DfBrLayoutResolveLengthPx (E, Box, &S->Width, Cbw);
        if (!S->Height.Auto)
          H = DfBrLayoutResolveLengthPx (E, Box, &S->Height, Cbw);
        if (W < 0.0f) W = 0.0f;
        if (H < 0.0f) H = 0.0f;
        if (W > 0.0f && H > 0.0f) {
          f32 Ml = 0.0f, Mr = 0.0f;
          if (!S->MarginLeft.Auto)
            Ml = DfBrLayoutResolveLengthPx (E, Box, &S->MarginLeft,  Cbw);
          if (!S->MarginRight.Auto)
            Mr = DfBrLayoutResolveLengthPx (E, Box, &S->MarginRight, Cbw);
          if (Ml > 0.0f) {
            memset (&F, 0, sizeof (F));
            F.Kind         = FragSpacer;
            F.Box          = Box;
            F.Advance      = Ml;
            F.FontSizePx   = Fs;
            F.LineHeightPx = Lh;
            FragPush (FL, &F, A);
          }
          memset (&F, 0, sizeof (F));
          F.Kind             = FragReplaced;
          F.Box              = Box;
          F.Advance          = W;
          F.FontSizePx       = Fs;
          F.LineHeightPx     = (H > Lh) ? H : Lh;
          Box->ContentRect.W = W;
          Box->ContentRect.H = H;
          FragPush (FL, &F, A);
          if (Mr > 0.0f) {
            memset (&F, 0, sizeof (F));
            F.Kind         = FragSpacer;
            F.Box          = Box;
            F.Advance      = Mr;
            F.FontSizePx   = Fs;
            F.LineHeightPx = Lh;
            FragPush (FL, &F, A);
          }
          return;
        }
      }
    }

    /* Horizontal box-edges at the start of an inline. Per CSS 2.1
     * §8.3.1 and §8.4 and §9.2.2.2, margin / border / padding apply
     * on the left-hand edge of an inline element (and on the right
     * hand edge at its end) but NOT across line breaks within it -
     * splits are box-edge-free. We don't yet track wrap splits here,
     * so for an un-wrapped inline we emit a leading spacer carrying
     * the full start-edge width, and a trailing spacer below. This
     * is what makes `.nav a { margin-left: 1.4em }` actually space
     * adjacent anchors apart (previously their text fragments were
     * butted against each other and rendered as "GamesSupport-
     * Privacy"). Inline-block takes the same code path; its edges
     * are identical in this respect. */
    if (Box->Style != DFBR_NULL) {
      f32 Lead = 0.0f;
      if (!Box->Style->MarginLeft.Auto)
        Lead += DfBrLayoutResolveLengthPx (E, Box, &Box->Style->MarginLeft, Cbw);
      Lead += DfBrLayoutResolveBorderPx (E, Box, &Box->Style->BorderLeft, Cbw);
      if (!Box->Style->PaddingLeft.Auto)
        Lead += DfBrLayoutResolveLengthPx (E, Box, &Box->Style->PaddingLeft, Cbw);
      if (Lead > 0.0f) {
        memset (&F, 0, sizeof (F));
        F.Kind         = FragSpacer;
        F.Box          = Box;
        F.Advance      = Lead;
        F.FontSizePx   = Fs;
        F.LineHeightPx = Lh;
        FragPush (FL, &F, A);
      }
    }

    memset (&F, 0, sizeof (F));
    F.Kind = FragInlineStart; F.Box = Box;
    F.FontSizePx = Fs; F.LineHeightPx = Lh;
    FragPush (FL, &F, A);
    /* inline-flex children: we currently use the inline path as a
     * fallback (the flex engine doesn't yet wrap anonymous text runs
     * into items), so we manually honor `column-gap` / `gap` between
     * consecutive children. Without this, patterns like
     *   <a class="brand"><span class="mark"></span> lowcoregames</a>
     * with `display: inline-flex; gap: .6rem` rendered the icon
     * butted against the first glyph. Whitespace-only text children
     * (browsers treat them as empty anon-items in flex context) are
     * skipped for gap accounting so we don't double up. */
    {
      f32  InlineFlexGap = 0.0f;
      b32  EmitGaps      = IsInlineFlex &&
                           (Box->Style != DFBR_NULL);
      b32  Seen          = DFBR_FALSE;
      if (EmitGaps) {
        InlineFlexGap = DfBrLayoutResolveLengthPx (
          E, Box, &Box->Style->ColumnGap, Cbw);
        if (InlineFlexGap < 0.0f) InlineFlexGap = 0.0f;
      }
      for (C = Box->FirstChild; C != DFBR_NULL; C = C->NextSibling) {
        b32 IsWsOnly = DFBR_FALSE;
        if (C->Kind == DFBR_LAYOUT_BOX_TEXT && C->TextData != DFBR_NULL) {
          usize K;
          IsWsOnly = DFBR_TRUE;
          for (K = 0; K < C->TextLen; K++) {
            char Ch = C->TextData[K];
            if (Ch != ' ' && Ch != '\t' && Ch != '\n' && Ch != '\r') {
              IsWsOnly = DFBR_FALSE;
              break;
            }
          }
        }
        if (EmitGaps && InlineFlexGap > 0.0f && Seen && !IsWsOnly) {
          Fragment Gf;
          memset (&Gf, 0, sizeof (Gf));
          Gf.Kind         = FragSpacer;
          Gf.Box          = Box;
          Gf.Advance      = InlineFlexGap;
          Gf.FontSizePx   = Fs;
          Gf.LineHeightPx = Lh;
          FragPush (FL, &Gf, A);
        }
        if (EmitGaps && IsWsOnly) {
          /* Drop whitespace-only text children in a flex context; they
           * would otherwise re-introduce an extra glyph-space of slack
           * on top of our explicit gap. */
        } else {
          Flatten (E, C, FL, A, Fs, Lh, Cbw);
        }
        if (!IsWsOnly) Seen = DFBR_TRUE;
      }
    }
    F.Kind = FragInlineEnd; F.Box = Box;
    FragPush (FL, &F, A);

    /* Right-edge box-edges for inline and inline-block (see start-edge
     * note above). */
    (void)IsInlineBlock;
    if (Box->Style != DFBR_NULL) {
      f32 Trail = 0.0f;
      if (!Box->Style->PaddingRight.Auto)
        Trail += DfBrLayoutResolveLengthPx (E, Box, &Box->Style->PaddingRight, Cbw);
      Trail += DfBrLayoutResolveBorderPx (E, Box, &Box->Style->BorderRight, Cbw);
      if (!Box->Style->MarginRight.Auto)
        Trail += DfBrLayoutResolveLengthPx (E, Box, &Box->Style->MarginRight, Cbw);
      if (Trail > 0.0f) {
        memset (&F, 0, sizeof (F));
        F.Kind         = FragSpacer;
        F.Box          = Box;
        F.Advance      = Trail;
        F.FontSizePx   = Fs;
        F.LineHeightPx = Lh;
        FragPush (FL, &F, A);
      }
    }
    return;
  }
  /* Block children inside an inline flow would be an author error by
   * the time the tree builder got here; ignore defensively. */
}

/* ------------------------------------------------------------------
 *  Line packing
 * -----------------------------------------------------------------*/

/* Shift every placement on the current line by `Dx` px and clear
 * the placement list. Used to realize text-align. */
static void
ShiftLine (PlacementList *PL, f32 Dx) {
  usize I;
  if (PL->Count == 0) return;
  if (Dx != 0.0f) {
    for (I = 0; I < PL->Count; I++) {
      LinePlacement *P = &PL->Items[I];
      if (P->Tf != DFBR_NULL) {
        P->Tf->X += Dx;
      }
      if (P->Replaced != DFBR_NULL) {
        P->Replaced->ContentRect.X += Dx;
      }
    }
  }
  PL->Count = 0;
}

/* Vertically center any replaced atomic inlines on the current line
 * within the final committed line box. We initially anchor replaced
 * boxes at the line top because LineLhMax only stabilises after the
 * line is fully packed; once we know the total line height (and thus
 * where the baseline-ish midline is), shift them down by the spare
 * half-leading so an `inline-flex align-items:center` icon sits beside
 * its label instead of floating at the top of the line. For a single
 * line-height'd icon this is a no-op; for an icon that's noticeably
 * shorter than the line, this is what moves the icon into visual
 * alignment with the text. */
static void
CenterLineReplacedV (PlacementList *PL, f32 LineTopY, f32 LineHeight) {
  usize I;
  for (I = 0; I < PL->Count; I++) {
    LinePlacement *P = &PL->Items[I];
    if (P->Replaced != DFBR_NULL) {
      f32 H = P->Replaced->ContentRect.H;
      f32 Slack = LineHeight - H;
      if (Slack > 0.0f) {
        P->Replaced->ContentRect.Y = LineTopY + Slack * 0.5f;
      }
    }
  }
}

/* Compute the effective left / right edge for a line at Y, shrinking
 * the nominal [ContentLeft, ContentLeft + MaxWidth] band by any
 * active float that straddles Y. */
static void
LineBandAt (const DfBrLayoutEngine *E,
            f32 Y, f32 ContentLeft, f32 MaxWidth,
            f32 *OutLeft, f32 *OutWidth)
{
  f32 L, R;
  DfBrLayoutFloatsBandAt (E, Y, ContentLeft, ContentLeft + MaxWidth, &L, &R);
  *OutLeft  = L;
  *OutWidth = R - L;
  if (*OutWidth < 0.0f) *OutWidth = 0.0f;
}

f32
DfBrLayoutInlineChildren (
  DfBrLayoutEngine  *E,
  DfBrLayoutBox     *Block,
  f32                MaxWidth,
  f32                ContentLeft,
  f32                ContentTop
  )
{
  FragList         FL;
  PlacementList    PL;
  DfBrLayoutBox   *C;
  f32              ParentFs = Block->FontSizePx;
  f32              ParentLh = LineHeightOf (Block->Style, ParentFs);
  DfBrCssTextAlign Align    = ResolveTextAlign (Block->Style);
  usize            I = 0;
  f32              LineLeft;        /* effective start of the current line */
  f32              LineWidth;       /* effective width of the current line */
  f32              X;
  f32              Y = ContentTop;
  f32              LineFontMax  = ParentFs;
  f32              LineLhMax    = ParentLh;
  f32              LineLastEndX;    /* last non-space trailing edge */
  f32              TotalHeight  = 0.0f;
  b32              LineHasContent = DFBR_FALSE;

  memset (&FL, 0, sizeof (FL));
  memset (&PL, 0, sizeof (PL));

  /* When the containing block is a flex / inline-flex row that routed
   * here (BoxHasInlineChildrenOnly short-circuit in DfBrLayoutBlock),
   * honor `column-gap` / `gap` between sibling children and drop
   * whitespace-only text nodes - mirroring the fix already in the
   * inline Flatten path for inline-flex boxes. Without this, sites
   * like lowcoregames.com whose header flex-items are
   *     <a class="brand"><span class="mark"></span> lowcoregames</a>
   * saw the icon butted against the first glyph because our flex
   * packer skipped text children yet the inline path still accepted
   * them, with no gap awareness at either layer. */
  {
    const DfBrCssComputedStyle *Bs = Block->Style;
    b32  EmitGaps = (Bs != DFBR_NULL) &&
                    (Bs->Display == DFBR_CSS_DISPLAY_FLEX ||
                     Bs->Display == DFBR_CSS_DISPLAY_INLINE_FLEX);
    f32  FlexGap  = 0.0f;
    b32  Seen     = DFBR_FALSE;
    if (EmitGaps) {
      FlexGap = DfBrLayoutResolveLengthPx (
        E, Block, &Bs->ColumnGap, MaxWidth);
      if (FlexGap < 0.0f) FlexGap = 0.0f;
    }
    for (C = Block->FirstChild; C != DFBR_NULL; C = C->NextSibling) {
      b32 IsWsOnly = DFBR_FALSE;
      if (C->Kind == DFBR_LAYOUT_BOX_TEXT && C->TextData != DFBR_NULL) {
        usize K;
        IsWsOnly = DFBR_TRUE;
        for (K = 0; K < C->TextLen; K++) {
          char Ch = C->TextData[K];
          if (Ch != ' ' && Ch != '\t' && Ch != '\n' && Ch != '\r') {
            IsWsOnly = DFBR_FALSE;
            break;
          }
        }
      }
      if (EmitGaps && FlexGap > 0.0f && Seen && !IsWsOnly) {
        Fragment Gf;
        memset (&Gf, 0, sizeof (Gf));
        Gf.Kind         = FragSpacer;
        Gf.Box          = Block;
        Gf.Advance      = FlexGap;
        Gf.FontSizePx   = ParentFs;
        Gf.LineHeightPx = ParentLh;
        FragPush (&FL, &Gf, &E->Arena);
      }
      if (EmitGaps && IsWsOnly) {
        /* drop: see rationale above */
      } else {
        Flatten (E, C, &FL, &E->Arena, ParentFs, ParentLh, MaxWidth);
      }
      if (!IsWsOnly) Seen = DFBR_TRUE;
    }
  }

  /* Initialise the first line's band. */
  LineBandAt (E, Y, ContentLeft, MaxWidth, &LineLeft, &LineWidth);
  X            = LineLeft;
  LineLastEndX = LineLeft;

  /* Pack fragments onto lines. Whitespace at the start of a line is
   * dropped; whitespace that would overflow triggers a wrap at that
   * space. */
  while (I < FL.Count) {
    Fragment *F = &FL.Items[I];

    /* Drop leading whitespace. */
    if (!LineHasContent && F->Kind == FragSpace) { I++; continue; }

    if (F->Kind == FragInlineStart || F->Kind == FragInlineEnd) {
      /* Inline open/close do not consume advance. We still set their
       * box's X/Y anchor from the current pen so dumps are sensible.
       *
       * For FragInlineEnd we also commit the inline's content-box
       * geometry: width = (current pen - anchor X), height = the
       * line's line-height. Without this, an inline-block like
       * <span class="pill"> has its border/background drawn into a
       * 0x0 rect and every decorated inline (pills, tags, chips,
       * inline-block buttons) renders as invisible glyph-only text.
       * We include the inline's padding/border edges so paint draws
       * the full decoration rect, not just the text band. */
      if (F->Kind == FragInlineStart && F->Box != DFBR_NULL) {
        F->Box->ContentRect.X = X;
        F->Box->ContentRect.Y = Y;
      }
      if (F->Kind == FragInlineEnd && F->Box != DFBR_NULL) {
        DfBrLayoutBox *B = F->Box;
        f32 W = X - B->ContentRect.X;
        /* Trim the trailing-edge spacer contribution we added for
         * this box so ContentRect captures padding+border on both
         * sides, not twice on the right. The leading spacer already
         * shifted X forward past B->ContentRect.X; we don't want to
         * double-count its width on the right. */
        if (B->Style != DFBR_NULL) {
          f32 Trail = 0.0f;
          if (!B->Style->PaddingRight.Auto) Trail += B->Style->PaddingRight.Value;
          if (!B->Style->BorderRight.Width.Auto &&
              B->Style->BorderRight.Style != DFBR_CSS_BORDER_NONE &&
              B->Style->BorderRight.Style != DFBR_CSS_BORDER_HIDDEN)
          {
            Trail += B->Style->BorderRight.Width.Value;
          }
          if (!B->Style->MarginRight.Auto) Trail += B->Style->MarginRight.Value;
          (void)Trail;
        }
        if (W < 0.0f) W = 0.0f;
        if (W > B->ContentRect.W) B->ContentRect.W = W;
        if (LineLhMax > B->ContentRect.H) B->ContentRect.H = LineLhMax;
        /* Populate resolved padding / border so paint draws the
         * authored background + border. Percentages here resolve
         * against the CB width, which for an inline inside a line
         * is its containing block (the inline formatting context's
         * width = MaxWidth). */
        if (B->Style != DFBR_NULL) {
          const DfBrCssComputedStyle *S = B->Style;
          f32 Cbw = MaxWidth;
          B->Padding.Top    = S->PaddingTop.Auto    ? 0.0f : DfBrLayoutResolveLengthPx (E, B, &S->PaddingTop,    Cbw);
          B->Padding.Right  = S->PaddingRight.Auto  ? 0.0f : DfBrLayoutResolveLengthPx (E, B, &S->PaddingRight,  Cbw);
          B->Padding.Bottom = S->PaddingBottom.Auto ? 0.0f : DfBrLayoutResolveLengthPx (E, B, &S->PaddingBottom, Cbw);
          B->Padding.Left   = S->PaddingLeft.Auto   ? 0.0f : DfBrLayoutResolveLengthPx (E, B, &S->PaddingLeft,   Cbw);
          B->Border.Top     = DfBrLayoutResolveBorderPx (E, B, &S->BorderTop,    Cbw);
          B->Border.Right   = DfBrLayoutResolveBorderPx (E, B, &S->BorderRight,  Cbw);
          B->Border.Bottom  = DfBrLayoutResolveBorderPx (E, B, &S->BorderBottom, Cbw);
          B->Border.Left    = DfBrLayoutResolveBorderPx (E, B, &S->BorderLeft,   Cbw);
        }
      }
      I++;
      continue;
    }

    if (F->Kind == FragBreak) {
      /* Forced line break. <br> contributes its own line-height even
       * when the current line is otherwise empty so subsequent lines
       * flow at the right baseline. */
      f32 Lh = (F->LineHeightPx > LineLhMax) ? F->LineHeightPx : LineLhMax;
      /* Align + commit the current line (may be empty, in which case
       * ShiftLine is a no-op). */
      {
        f32 Used  = LineLastEndX - LineLeft;
        f32 Space = LineWidth - Used;
        f32 Dx    = 0.0f;
        if (Space > 0.0f) {
          if (Align == DFBR_CSS_TALIGN_CENTER) Dx = Space * 0.5f;
          else if (Align == DFBR_CSS_TALIGN_RIGHT) Dx = Space;
        }
        CenterLineReplacedV (&PL, Y, Lh);
        ShiftLine (&PL, Dx);
      }
      Y += Lh;
      TotalHeight += Lh;
      LineBandAt (E, Y, ContentLeft, MaxWidth, &LineLeft, &LineWidth);
      X            = LineLeft;
      LineLastEndX = LineLeft;
      LineFontMax  = ParentFs;
      LineLhMax    = ParentLh;
      LineHasContent = DFBR_FALSE;
      I++;
      continue;
    }

    if (F->Kind == FragWord || F->Kind == FragReplaced) {
      /* Does it fit? If not and we already have content on this line,
       * wrap. A word wider than the line's band still gets placed
       * (overflows). */
      if (LineHasContent && (X + F->Advance > LineLeft + LineWidth)) {
        /* Wrap: align + commit the line, then advance. */
        f32 Used  = LineLastEndX - LineLeft;
        f32 Space = LineWidth - Used;
        f32 Dx    = 0.0f;
        if (Space > 0.0f) {
          if (Align == DFBR_CSS_TALIGN_CENTER) Dx = Space * 0.5f;
          else if (Align == DFBR_CSS_TALIGN_RIGHT) Dx = Space;
        }
        CenterLineReplacedV (&PL, Y, LineLhMax);
        ShiftLine (&PL, Dx);
        Y += LineLhMax;
        TotalHeight += LineLhMax;
        LineBandAt (E, Y, ContentLeft, MaxWidth, &LineLeft, &LineWidth);
        X            = LineLeft;
        LineLastEndX = LineLeft;
        LineFontMax  = ParentFs;
        LineLhMax    = ParentLh;
        LineHasContent = DFBR_FALSE;
        continue;   /* retry placing this fragment on the new line */
      }
      /* Place. */
      if (F->Box != DFBR_NULL) {
        if (F->Kind == FragWord) {
          /* A text box can emit multiple word fragments. On the first
           * one, anchor its rect at (X,Y); on subsequent ones, extend
           * the rect to cover them. The fragment's right edge
           * compared to the box origin gives the new width. */
          f32 Right = X + F->Advance;
          if (F->Box->TextAdvance == 0.0f) {
            F->Box->ContentRect.X = X;
            F->Box->ContentRect.Y = Y;
            F->Box->ContentRect.W = F->Advance;
          } else if (Right - F->Box->ContentRect.X > F->Box->ContentRect.W) {
            F->Box->ContentRect.W = Right - F->Box->ContentRect.X;
          }
          if (F->LineHeightPx > F->Box->ContentRect.H) {
            F->Box->ContentRect.H = F->LineHeightPx;
          }
          F->Box->TextAdvance += F->Advance;

          /* Record a paint fragment at the current pen / baseline.
           * Paint walks these (not ContentRect) so wrapped and
           * multi-line text renders at every word's real position. */
          {
            DfBrLayoutTextFragment *Tf = (DfBrLayoutTextFragment *)
              DfBrLayoutArenaAlloc (&E->Arena, sizeof (*Tf));
            if (Tf != DFBR_NULL) {
              Tf->X         = X;
              Tf->BaselineY = Y + F->AscentPx;
              Tf->Str       = F->Str;
              Tf->Len       = F->Len;
              Tf->Advance   = F->Advance;
              Tf->Next      = DFBR_NULL;
              if (F->Box->FirstTextFragment == DFBR_NULL) {
                F->Box->FirstTextFragment = Tf;
              } else {
                F->Box->LastTextFragment->Next = Tf;
              }
              F->Box->LastTextFragment = Tf;
              PlacementPush (&PL, Tf, DFBR_NULL, &E->Arena);
            }
          }
        } else {
          /* Replaced element: anchor + advance. */
          F->Box->ContentRect.X = X;
          F->Box->ContentRect.Y = Y;
          PlacementPush (&PL, DFBR_NULL, F->Box, &E->Arena);
        }
      }
      X += F->Advance;
      LineLastEndX   = X;                 /* trailing edge of content */
      LineHasContent = DFBR_TRUE;
      if (F->FontSizePx > LineFontMax)   LineFontMax = F->FontSizePx;
      if (F->LineHeightPx > LineLhMax)   LineLhMax   = F->LineHeightPx;
      I++;
      continue;
    }

    if (F->Kind == FragSpace) {
      /* Only add if not at start-of-line (already dropped above) and
       * we still have room; otherwise ignore (acts as a wrap opp). */
      if (X + F->Advance <= LineLeft + LineWidth) {
        X += F->Advance;
      }
      I++;
      continue;
    }

    if (F->Kind == FragSpacer) {
      /* Non-collapsing geometric gap (inline-block margin/padding).
       * Always consumes advance, including at line start; counts as
       * content so a subsequent plain space doesn't get dropped. */
      X += F->Advance;
      LineLastEndX   = X;
      LineHasContent = DFBR_TRUE;
      if (F->LineHeightPx > LineLhMax) LineLhMax = F->LineHeightPx;
      I++;
      continue;
    }

    I++;
  }

  /* Close the trailing line. */
  if (LineHasContent) {
    f32 Used  = LineLastEndX - LineLeft;
    f32 Space = LineWidth - Used;
    f32 Dx    = 0.0f;
    if (Space > 0.0f) {
      if (Align == DFBR_CSS_TALIGN_CENTER) Dx = Space * 0.5f;
      else if (Align == DFBR_CSS_TALIGN_RIGHT) Dx = Space;
    }
    CenterLineReplacedV (&PL, Y, LineLhMax);
    ShiftLine (&PL, Dx);
    Y += LineLhMax;
    TotalHeight += LineLhMax;
  }

  (void)LineFontMax;
  return TotalHeight;
}
