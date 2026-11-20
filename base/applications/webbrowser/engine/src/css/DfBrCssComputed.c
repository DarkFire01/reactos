/** @file
  DfBrCssComputed.c - computed-style initialization, inheritance, and
  per-declaration application.

  Philosophy: this file owns the mapping
      (Prop, Value CVs)  ->  field writes on DfBrCssComputedStyle.
  The cascade (DfBrCssCascade.c) drives it; everything that needs to
  know what "color: red" means is here.

  v1 implements the common subset (~40 properties). Unknown properties
  are preserved in the rule tree (for debugging / future features)
  but ignored at apply time. Extending coverage is one case per
  property in `ApplySimple`, by design.
**/

#include "DfBrCssInternal.h"

#include <string.h>

/* ------------------------------------------------------------------
 *  Initial-value seed
 *
 *  Every field gets a sensible default that the CSS spec calls out
 *  as the "initial value" for that property. Inherited properties
 *  additionally get overwritten by `DfBrCssComputedInherit` before
 *  the cascade applies author decls.
 * -----------------------------------------------------------------*/

static DfBrCssLength AutoLen (void) {
  DfBrCssLength L;
  L.Value = 0.0f; L.Unit = DFBR_CSS_UNIT_NONE; L.Auto = DFBR_TRUE;
  L.CalcExpr = DFBR_NULL;
  return L;
}

static DfBrCssLength ZeroLen (void) {
  DfBrCssLength L;
  L.Value = 0.0f; L.Unit = DFBR_CSS_UNIT_PX; L.Auto = DFBR_FALSE;
  L.CalcExpr = DFBR_NULL;
  return L;
}

static DfBrCssLength PxLen (f32 V) {
  DfBrCssLength L;
  L.Value = V; L.Unit = DFBR_CSS_UNIT_PX; L.Auto = DFBR_FALSE;
  L.CalcExpr = DFBR_NULL;
  return L;
}

static DfBrCssColor Rgba (u8 R, u8 G, u8 B, f32 A) {
  DfBrCssColor C;
  C.R = R; C.G = G; C.B = B; C.A = A;
  return C;
}

static void SeedInitial (DfBrCssComputedStyle *Cs) {
  memset (Cs, 0, sizeof (*Cs));

  Cs->Display        = DFBR_CSS_DISPLAY_INLINE;
  Cs->Position       = DFBR_CSS_POSITION_STATIC;
  Cs->Float          = DFBR_CSS_FLOAT_NONE;
  Cs->Clear          = DFBR_CSS_CLEAR_NONE;
  Cs->Visibility     = DFBR_CSS_VISIBLE;
  Cs->OverflowX      = DFBR_CSS_OVERFLOW_VISIBLE;
  Cs->OverflowY      = DFBR_CSS_OVERFLOW_VISIBLE;
  Cs->BoxSizing      = DFBR_CSS_BOX_CONTENT;
  Cs->ZIndexAuto     = DFBR_TRUE;
  Cs->ZIndex         = 0;
  Cs->Opacity        = 1.0f;

  Cs->Width          = AutoLen ();
  Cs->Height         = AutoLen ();
  Cs->MinWidth       = ZeroLen ();
  Cs->MinHeight      = ZeroLen ();
  Cs->MaxWidth       = AutoLen ();
  Cs->MaxHeight      = AutoLen ();
  Cs->Top = Cs->Right = Cs->Bottom = Cs->Left = AutoLen ();

  Cs->MarginTop = Cs->MarginRight = Cs->MarginBottom = Cs->MarginLeft = ZeroLen ();
  Cs->PaddingTop = Cs->PaddingRight = Cs->PaddingBottom = Cs->PaddingLeft = ZeroLen ();

  Cs->BorderTop.Width    = ZeroLen ();
  Cs->BorderTop.Style    = DFBR_CSS_BORDER_NONE;
  Cs->BorderTop.Color    = Rgba (0, 0, 0, 1.0f);
  Cs->BorderTop.ColorIsCurrent = DFBR_TRUE;
  Cs->BorderRight  = Cs->BorderTop;
  Cs->BorderBottom = Cs->BorderTop;
  Cs->BorderLeft   = Cs->BorderTop;

  Cs->BorderTopLeftRadius     = ZeroLen ();
  Cs->BorderTopRightRadius    = ZeroLen ();
  Cs->BorderBottomRightRadius = ZeroLen ();
  Cs->BorderBottomLeftRadius  = ZeroLen ();

  /* transparent background + inherit text color defaults. */
  Cs->BackgroundColor          = Rgba (0, 0, 0, 0.0f);
  Cs->BackgroundColorIsCurrent = DFBR_FALSE;
  Cs->BackgroundImageUrl       = DFBR_NULL;
  Cs->BackgroundImageUrlLen    = 0;
  Cs->BackgroundGradient       = DFBR_NULL;
  Cs->BackgroundClip           = DFBR_CSS_BGCLIP_BORDER_BOX;

  Cs->Color                    = Rgba (0, 0, 0, 1.0f);
  Cs->FontSize                 = PxLen (16.0f);
  Cs->FontWeight               = 400;
  Cs->FontStyle                = DFBR_CSS_FSTYLE_NORMAL;
  Cs->FontFamilies             = DFBR_NULL;
  Cs->FontFamilyCount          = 0;

  Cs->LineHeight.Kind          = DFBR_CSS_LINEHEIGHT_NORMAL;
  Cs->LineHeight.Number        = 1.2f;
  Cs->LineHeight.Length        = ZeroLen ();

  Cs->TextAlign                = DFBR_CSS_TALIGN_START;
  memset (&Cs->TextDecoration, 0, sizeof (Cs->TextDecoration));
  Cs->TextDecorationColor      = Rgba (0, 0, 0, 1.0f);
  Cs->TextTransform            = DFBR_CSS_TTRANSFORM_NONE;
  Cs->TextIndent               = ZeroLen ();
  Cs->LetterSpacing            = ZeroLen ();
  Cs->WordSpacing              = ZeroLen ();
  Cs->WhiteSpace               = DFBR_CSS_WS_NORMAL;
  Cs->VerticalAlign            = DFBR_CSS_VALIGN_BASELINE;
  Cs->VerticalAlignLength      = ZeroLen ();

  Cs->ListStyleType            = DFBR_CSS_LIST_DISC;

  Cs->FlexDirection            = DFBR_CSS_FLEX_ROW;
  Cs->FlexWrap                 = DFBR_CSS_FLEX_NOWRAP;
  Cs->FlexGrow                 = 0.0f;
  Cs->FlexShrink               = 1.0f;
  Cs->FlexBasis                = AutoLen ();
  Cs->JustifyContent           = DFBR_CSS_ALIGN_NORMAL;
  Cs->AlignItems               = DFBR_CSS_ALIGN_NORMAL;
  Cs->AlignSelf                = DFBR_CSS_ALIGN_AUTO;
  Cs->AlignContent             = DFBR_CSS_ALIGN_NORMAL;
  Cs->RowGap                   = ZeroLen ();
  Cs->ColumnGap                = ZeroLen ();
  Cs->Order                    = 0;
}

DfBrCssComputedStyle *
DfBrCssComputedNew (DfBrCssStyleContext *Sc)
{
  DfBrCssComputedStyle *Cs = DfBrCssArenaAlloc (&Sc->Arena, sizeof (*Cs));
  if (Cs != DFBR_NULL) SeedInitial (Cs);
  return Cs;
}

/* ------------------------------------------------------------------
 *  Inheritance
 *
 *  Copies parent's values to child for inherited properties only.
 *  Mirrors the "inherited property" table in CSS 2.1 Appendix F + a
 *  handful of Level-3 additions.
 * -----------------------------------------------------------------*/

void
DfBrCssComputedInherit (
  DfBrCssComputedStyle       *Child,
  const DfBrCssComputedStyle *Parent
  )
{
  if (Child == DFBR_NULL || Parent == DFBR_NULL) return;

  Child->Color                 = Parent->Color;
  Child->FontSize              = Parent->FontSize;
  Child->FontWeight            = Parent->FontWeight;
  Child->FontStyle             = Parent->FontStyle;
  Child->FontFamilies          = Parent->FontFamilies;
  Child->FontFamilyCount       = Parent->FontFamilyCount;
  Child->LineHeight            = Parent->LineHeight;
  Child->TextAlign             = Parent->TextAlign;
  Child->TextTransform         = Parent->TextTransform;
  Child->TextIndent            = Parent->TextIndent;
  Child->LetterSpacing         = Parent->LetterSpacing;
  Child->WordSpacing           = Parent->WordSpacing;
  Child->WhiteSpace            = Parent->WhiteSpace;
  Child->Visibility            = Parent->Visibility;
  Child->ListStyleType         = Parent->ListStyleType;

  /* Custom properties cascade through the parent chain: the child's
   * list starts as the parent's, and any `--foo: ...` declarations on
   * this element are prepended in cascade order. Since new entries
   * prepend and lookup walks head-first, later (more specific)
   * declarations correctly shadow earlier ones. */
  Child->CustomProps           = Parent->CustomProps;
}

/* ------------------------------------------------------------------
 *  Small CV helpers
 * -----------------------------------------------------------------*/

static const DfBrCssCv *
SkipWs (const DfBrCssCv *Cv) {
  while (Cv != DFBR_NULL && Cv->Kind == DfBrCssCvKindWhitespace) Cv = Cv->Next;
  return Cv;
}

static b32
CvIsIdentEq (const DfBrCssCv *Cv, const char *Lit) {
  if (Cv == DFBR_NULL || Cv->Kind != DfBrCssCvKindIdent) return DFBR_FALSE;
  return DfBrCssAsciiEqZ (Cv->Str, Cv->StrLen, Lit);
}

static DfBrCssKeyword
CvAsKeyword (const DfBrCssCv *Cv) {
  if (Cv == DFBR_NULL || Cv->Kind != DfBrCssCvKindIdent) return DFBR_CSS_KW_UNKNOWN;
  return DfBrCssKeywordFromName (Cv->Str, Cv->StrLen);
}

static b32
CvAsNumber (const DfBrCssCv *Cv, f32 *Out) {
  if (Cv == DFBR_NULL) return DFBR_FALSE;
  if (Cv->Kind == DfBrCssCvKindNumber) { *Out = (f32)Cv->Number; return DFBR_TRUE; }
  return DFBR_FALSE;
}

/* ------------------------------------------------------------------
 *  Shorthand expansion helpers
 * -----------------------------------------------------------------*/

/* For `margin: A [B [C [D]]]` - returns four DfBrCssLengths. */
static b32
ParseFourLengths (
  const DfBrCssCv   *Cv,
  DfBrCssLength      Out[4]
  )
{
  DfBrCssLength Vals[4];
  usize N = 0;
  const DfBrCssCv *P = SkipWs (Cv);

  while (P != DFBR_NULL && N < 4) {
    DfBrCssLength L;
    L.CalcExpr = DFBR_NULL;
    if (DfBrCssCvIsAuto (P)) {
      L.Value = 0.0f; L.Unit = DFBR_CSS_UNIT_NONE; L.Auto = DFBR_TRUE;
    } else if (DfBrCssCvAsLength (P, &L)) {
      /* ok */
    } else {
      return DFBR_FALSE;
    }
    Vals[N++] = L;
    P = SkipWs (P->Next);
  }
  if (N == 0) return DFBR_FALSE;

  /* Expand per CSS shorthand rules. */
  switch (N) {
    case 1: Out[0] = Out[1] = Out[2] = Out[3] = Vals[0]; break;
    case 2: Out[0] = Out[2] = Vals[0]; Out[1] = Out[3] = Vals[1]; break;
    case 3: Out[0] = Vals[0]; Out[1] = Out[3] = Vals[1]; Out[2] = Vals[2]; break;
    default: Out[0] = Vals[0]; Out[1] = Vals[1]; Out[2] = Vals[2]; Out[3] = Vals[3]; break;
  }
  return DFBR_TRUE;
}

/* ------------------------------------------------------------------
 *  Keyword -> enum mapping helpers
 * -----------------------------------------------------------------*/

static b32
KwToDisplay (DfBrCssKeyword K, DfBrCssDisplay *Out) {
  switch (K) {
    case DFBR_CSS_KW_NONE:                 *Out = DFBR_CSS_DISPLAY_NONE; return DFBR_TRUE;
    case DFBR_CSS_KW_BLOCK:                *Out = DFBR_CSS_DISPLAY_BLOCK; return DFBR_TRUE;
    case DFBR_CSS_KW_INLINE:               *Out = DFBR_CSS_DISPLAY_INLINE; return DFBR_TRUE;
    case DFBR_CSS_KW_INLINE_BLOCK:         *Out = DFBR_CSS_DISPLAY_INLINE_BLOCK; return DFBR_TRUE;
    case DFBR_CSS_KW_LIST_ITEM:            *Out = DFBR_CSS_DISPLAY_LIST_ITEM; return DFBR_TRUE;
    case DFBR_CSS_KW_FLEX:                 *Out = DFBR_CSS_DISPLAY_FLEX; return DFBR_TRUE;
    case DFBR_CSS_KW_INLINE_FLEX:          *Out = DFBR_CSS_DISPLAY_INLINE_FLEX; return DFBR_TRUE;
    case DFBR_CSS_KW_GRID:                 *Out = DFBR_CSS_DISPLAY_GRID; return DFBR_TRUE;
    case DFBR_CSS_KW_TABLE:                *Out = DFBR_CSS_DISPLAY_TABLE; return DFBR_TRUE;
    case DFBR_CSS_KW_INLINE_TABLE:         *Out = DFBR_CSS_DISPLAY_INLINE_TABLE; return DFBR_TRUE;
    case DFBR_CSS_KW_TABLE_ROW:            *Out = DFBR_CSS_DISPLAY_TABLE_ROW; return DFBR_TRUE;
    case DFBR_CSS_KW_TABLE_CELL:           *Out = DFBR_CSS_DISPLAY_TABLE_CELL; return DFBR_TRUE;
    case DFBR_CSS_KW_TABLE_ROW_GROUP:      *Out = DFBR_CSS_DISPLAY_TABLE_ROW_GROUP; return DFBR_TRUE;
    case DFBR_CSS_KW_TABLE_HEADER_GROUP:   *Out = DFBR_CSS_DISPLAY_TABLE_HEADER_GROUP; return DFBR_TRUE;
    case DFBR_CSS_KW_TABLE_FOOTER_GROUP:   *Out = DFBR_CSS_DISPLAY_TABLE_FOOTER_GROUP; return DFBR_TRUE;
    case DFBR_CSS_KW_TABLE_COLUMN:         *Out = DFBR_CSS_DISPLAY_TABLE_COLUMN; return DFBR_TRUE;
    case DFBR_CSS_KW_TABLE_COLUMN_GROUP:   *Out = DFBR_CSS_DISPLAY_TABLE_COLUMN_GROUP; return DFBR_TRUE;
    case DFBR_CSS_KW_TABLE_CAPTION:        *Out = DFBR_CSS_DISPLAY_TABLE_CAPTION; return DFBR_TRUE;
    case DFBR_CSS_KW_RUN_IN:               *Out = DFBR_CSS_DISPLAY_RUN_IN; return DFBR_TRUE;
    case DFBR_CSS_KW_CONTENTS:             *Out = DFBR_CSS_DISPLAY_CONTENTS; return DFBR_TRUE;
    default: return DFBR_FALSE;
  }
}

static b32
KwToPosition (DfBrCssKeyword K, DfBrCssPosition *Out) {
  switch (K) {
    case DFBR_CSS_KW_STATIC:   *Out = DFBR_CSS_POSITION_STATIC;   return DFBR_TRUE;
    case DFBR_CSS_KW_RELATIVE: *Out = DFBR_CSS_POSITION_RELATIVE; return DFBR_TRUE;
    case DFBR_CSS_KW_ABSOLUTE: *Out = DFBR_CSS_POSITION_ABSOLUTE; return DFBR_TRUE;
    case DFBR_CSS_KW_FIXED:    *Out = DFBR_CSS_POSITION_FIXED;    return DFBR_TRUE;
    case DFBR_CSS_KW_STICKY:   *Out = DFBR_CSS_POSITION_STICKY;   return DFBR_TRUE;
    default: return DFBR_FALSE;
  }
}

static b32
KwToFloat (DfBrCssKeyword K, DfBrCssFloat *Out) {
  switch (K) {
    case DFBR_CSS_KW_LEFT:  *Out = DFBR_CSS_FLOAT_LEFT;  return DFBR_TRUE;
    case DFBR_CSS_KW_RIGHT: *Out = DFBR_CSS_FLOAT_RIGHT; return DFBR_TRUE;
    case DFBR_CSS_KW_NONE:  *Out = DFBR_CSS_FLOAT_NONE;  return DFBR_TRUE;
    default: return DFBR_FALSE;
  }
}

static b32
KwToVisibility (DfBrCssKeyword K, DfBrCssVisibility *Out) {
  switch (K) {
    case DFBR_CSS_KW_VISIBLE:  *Out = DFBR_CSS_VISIBLE;  return DFBR_TRUE;
    case DFBR_CSS_KW_HIDDEN:   *Out = DFBR_CSS_HIDDEN;   return DFBR_TRUE;
    case DFBR_CSS_KW_COLLAPSE: *Out = DFBR_CSS_COLLAPSE; return DFBR_TRUE;
    default: return DFBR_FALSE;
  }
}

static b32
KwToOverflow (DfBrCssKeyword K, DfBrCssOverflowKind *Out) {
  switch (K) {
    case DFBR_CSS_KW_VISIBLE: *Out = DFBR_CSS_OVERFLOW_VISIBLE; return DFBR_TRUE;
    case DFBR_CSS_KW_HIDDEN:  *Out = DFBR_CSS_OVERFLOW_HIDDEN;  return DFBR_TRUE;
    case DFBR_CSS_KW_SCROLL:  *Out = DFBR_CSS_OVERFLOW_SCROLL;  return DFBR_TRUE;
    case DFBR_CSS_KW_AUTO:    *Out = DFBR_CSS_OVERFLOW_AUTO;    return DFBR_TRUE;
    case DFBR_CSS_KW_CLIP:    *Out = DFBR_CSS_OVERFLOW_CLIP;    return DFBR_TRUE;
    default: return DFBR_FALSE;
  }
}

static b32
KwToBoxSizing (DfBrCssKeyword K, DfBrCssBoxSizing *Out) {
  switch (K) {
    case DFBR_CSS_KW_CONTENT_BOX: *Out = DFBR_CSS_BOX_CONTENT; return DFBR_TRUE;
    case DFBR_CSS_KW_BORDER_BOX:  *Out = DFBR_CSS_BOX_BORDER;  return DFBR_TRUE;
    case DFBR_CSS_KW_PADDING_BOX: *Out = DFBR_CSS_BOX_PADDING; return DFBR_TRUE;
    default: return DFBR_FALSE;
  }
}

static b32
KwToTextAlign (DfBrCssKeyword K, DfBrCssTextAlign *Out) {
  switch (K) {
    case DFBR_CSS_KW_LEFT:    *Out = DFBR_CSS_TALIGN_LEFT;    return DFBR_TRUE;
    case DFBR_CSS_KW_RIGHT:   *Out = DFBR_CSS_TALIGN_RIGHT;   return DFBR_TRUE;
    case DFBR_CSS_KW_CENTER:  *Out = DFBR_CSS_TALIGN_CENTER;  return DFBR_TRUE;
    case DFBR_CSS_KW_JUSTIFY: *Out = DFBR_CSS_TALIGN_JUSTIFY; return DFBR_TRUE;
    case DFBR_CSS_KW_START:   *Out = DFBR_CSS_TALIGN_START;   return DFBR_TRUE;
    case DFBR_CSS_KW_END:     *Out = DFBR_CSS_TALIGN_END;     return DFBR_TRUE;
    default: return DFBR_FALSE;
  }
}

static b32
KwToFontStyle (DfBrCssKeyword K, DfBrCssFontStyle *Out) {
  switch (K) {
    case DFBR_CSS_KW_NORMAL:  *Out = DFBR_CSS_FSTYLE_NORMAL;  return DFBR_TRUE;
    case DFBR_CSS_KW_ITALIC:  *Out = DFBR_CSS_FSTYLE_ITALIC;  return DFBR_TRUE;
    case DFBR_CSS_KW_OBLIQUE: *Out = DFBR_CSS_FSTYLE_OBLIQUE; return DFBR_TRUE;
    default: return DFBR_FALSE;
  }
}

static b32
KwToWhiteSpace (DfBrCssKeyword K, DfBrCssWhiteSpace *Out) {
  switch (K) {
    case DFBR_CSS_KW_NORMAL:       *Out = DFBR_CSS_WS_NORMAL;       return DFBR_TRUE;
    case DFBR_CSS_KW_NOWRAP:       *Out = DFBR_CSS_WS_NOWRAP;       return DFBR_TRUE;
    case DFBR_CSS_KW_PRE:          *Out = DFBR_CSS_WS_PRE;          return DFBR_TRUE;
    case DFBR_CSS_KW_PRE_WRAP:     *Out = DFBR_CSS_WS_PRE_WRAP;     return DFBR_TRUE;
    case DFBR_CSS_KW_PRE_LINE:     *Out = DFBR_CSS_WS_PRE_LINE;     return DFBR_TRUE;
    case DFBR_CSS_KW_BREAK_SPACES: *Out = DFBR_CSS_WS_BREAK_SPACES; return DFBR_TRUE;
    default: return DFBR_FALSE;
  }
}

static b32
KwToTextTransform (DfBrCssKeyword K, DfBrCssTextTransform *Out) {
  switch (K) {
    case DFBR_CSS_KW_NONE:       *Out = DFBR_CSS_TTRANSFORM_NONE;       return DFBR_TRUE;
    case DFBR_CSS_KW_UPPERCASE:  *Out = DFBR_CSS_TTRANSFORM_UPPERCASE;  return DFBR_TRUE;
    case DFBR_CSS_KW_LOWERCASE:  *Out = DFBR_CSS_TTRANSFORM_LOWERCASE;  return DFBR_TRUE;
    case DFBR_CSS_KW_CAPITALIZE: *Out = DFBR_CSS_TTRANSFORM_CAPITALIZE; return DFBR_TRUE;
    default: return DFBR_FALSE;
  }
}

static b32
KwToListStyle (DfBrCssKeyword K, DfBrCssListStyleTypeK *Out) {
  switch (K) {
    case DFBR_CSS_KW_NONE:                 *Out = DFBR_CSS_LIST_NONE; return DFBR_TRUE;
    case DFBR_CSS_KW_DISC:                 *Out = DFBR_CSS_LIST_DISC; return DFBR_TRUE;
    case DFBR_CSS_KW_CIRCLE:               *Out = DFBR_CSS_LIST_CIRCLE; return DFBR_TRUE;
    case DFBR_CSS_KW_SQUARE:               *Out = DFBR_CSS_LIST_SQUARE; return DFBR_TRUE;
    case DFBR_CSS_KW_DECIMAL:              *Out = DFBR_CSS_LIST_DECIMAL; return DFBR_TRUE;
    case DFBR_CSS_KW_DECIMAL_LEADING_ZERO: *Out = DFBR_CSS_LIST_DECIMAL_LEADING_ZERO; return DFBR_TRUE;
    case DFBR_CSS_KW_LOWER_ROMAN:          *Out = DFBR_CSS_LIST_LOWER_ROMAN; return DFBR_TRUE;
    case DFBR_CSS_KW_UPPER_ROMAN:          *Out = DFBR_CSS_LIST_UPPER_ROMAN; return DFBR_TRUE;
    case DFBR_CSS_KW_LOWER_ALPHA:          *Out = DFBR_CSS_LIST_LOWER_ALPHA; return DFBR_TRUE;
    case DFBR_CSS_KW_UPPER_ALPHA:          *Out = DFBR_CSS_LIST_UPPER_ALPHA; return DFBR_TRUE;
    default: return DFBR_FALSE;
  }
}

static b32
KwToBorderStyle (DfBrCssKeyword K, DfBrCssBorderStyleK *Out) {
  switch (K) {
    case DFBR_CSS_KW_NONE:          *Out = DFBR_CSS_BORDER_NONE;   return DFBR_TRUE;
    case DFBR_CSS_KW_HIDDEN_BORDER: *Out = DFBR_CSS_BORDER_HIDDEN; return DFBR_TRUE;
    case DFBR_CSS_KW_HIDDEN:        *Out = DFBR_CSS_BORDER_HIDDEN; return DFBR_TRUE;
    case DFBR_CSS_KW_DOTTED:        *Out = DFBR_CSS_BORDER_DOTTED; return DFBR_TRUE;
    case DFBR_CSS_KW_DASHED:        *Out = DFBR_CSS_BORDER_DASHED; return DFBR_TRUE;
    case DFBR_CSS_KW_SOLID:         *Out = DFBR_CSS_BORDER_SOLID;  return DFBR_TRUE;
    case DFBR_CSS_KW_DOUBLE:        *Out = DFBR_CSS_BORDER_DOUBLE; return DFBR_TRUE;
    case DFBR_CSS_KW_GROOVE:        *Out = DFBR_CSS_BORDER_GROOVE; return DFBR_TRUE;
    case DFBR_CSS_KW_RIDGE:         *Out = DFBR_CSS_BORDER_RIDGE;  return DFBR_TRUE;
    case DFBR_CSS_KW_INSET:         *Out = DFBR_CSS_BORDER_INSET;  return DFBR_TRUE;
    case DFBR_CSS_KW_OUTSET:        *Out = DFBR_CSS_BORDER_OUTSET; return DFBR_TRUE;
    default: return DFBR_FALSE;
  }
}

static b32
KwToFlexDir (DfBrCssKeyword K, DfBrCssFlexDirection *Out) {
  switch (K) {
    case DFBR_CSS_KW_ROW:            *Out = DFBR_CSS_FLEX_ROW;            return DFBR_TRUE;
    case DFBR_CSS_KW_ROW_REVERSE:    *Out = DFBR_CSS_FLEX_ROW_REVERSE;    return DFBR_TRUE;
    case DFBR_CSS_KW_COLUMN:         *Out = DFBR_CSS_FLEX_COLUMN;         return DFBR_TRUE;
    case DFBR_CSS_KW_COLUMN_REVERSE: *Out = DFBR_CSS_FLEX_COLUMN_REVERSE; return DFBR_TRUE;
    default: return DFBR_FALSE;
  }
}

static b32
KwToFlexWrap (DfBrCssKeyword K, DfBrCssFlexWrap *Out) {
  switch (K) {
    case DFBR_CSS_KW_NOWRAP:       *Out = DFBR_CSS_FLEX_NOWRAP;       return DFBR_TRUE;
    case DFBR_CSS_KW_WRAP:         *Out = DFBR_CSS_FLEX_WRAP;         return DFBR_TRUE;
    case DFBR_CSS_KW_WRAP_REVERSE: *Out = DFBR_CSS_FLEX_WRAP_REVERSE; return DFBR_TRUE;
    default: return DFBR_FALSE;
  }
}

static b32
KwToAlign (DfBrCssKeyword K, DfBrCssAlign *Out) {
  switch (K) {
    case DFBR_CSS_KW_NORMAL:         *Out = DFBR_CSS_ALIGN_NORMAL;        return DFBR_TRUE;
    case DFBR_CSS_KW_AUTO:           *Out = DFBR_CSS_ALIGN_AUTO;          return DFBR_TRUE;
    case DFBR_CSS_KW_START:          *Out = DFBR_CSS_ALIGN_START;         return DFBR_TRUE;
    case DFBR_CSS_KW_END:            *Out = DFBR_CSS_ALIGN_END;           return DFBR_TRUE;
    case DFBR_CSS_KW_FLEX_START:     *Out = DFBR_CSS_ALIGN_FLEX_START;    return DFBR_TRUE;
    case DFBR_CSS_KW_FLEX_END:       *Out = DFBR_CSS_ALIGN_FLEX_END;      return DFBR_TRUE;
    case DFBR_CSS_KW_CENTER:         *Out = DFBR_CSS_ALIGN_CENTER;        return DFBR_TRUE;
    case DFBR_CSS_KW_BASELINE:       *Out = DFBR_CSS_ALIGN_BASELINE;      return DFBR_TRUE;
    case DFBR_CSS_KW_STRETCH:        *Out = DFBR_CSS_ALIGN_STRETCH;       return DFBR_TRUE;
    case DFBR_CSS_KW_SPACE_BETWEEN:  *Out = DFBR_CSS_ALIGN_SPACE_BETWEEN; return DFBR_TRUE;
    case DFBR_CSS_KW_SPACE_AROUND:   *Out = DFBR_CSS_ALIGN_SPACE_AROUND;  return DFBR_TRUE;
    case DFBR_CSS_KW_SPACE_EVENLY:   *Out = DFBR_CSS_ALIGN_SPACE_EVENLY;  return DFBR_TRUE;
    default: return DFBR_FALSE;
  }
}

/* font-weight: normal|bold|bolder|lighter|100..900 */
static b32
ApplyFontWeight (DfBrCssComputedStyle *Cs, const DfBrCssCv *Cv) {
  f32 N;
  DfBrCssKeyword K = CvAsKeyword (Cv);
  if (K == DFBR_CSS_KW_NORMAL)  { Cs->FontWeight = 400; return DFBR_TRUE; }
  if (K == DFBR_CSS_KW_BOLD)    { Cs->FontWeight = 700; return DFBR_TRUE; }
  if (K == DFBR_CSS_KW_BOLDER)  { Cs->FontWeight = (Cs->FontWeight <= 500) ? 700 : 900; return DFBR_TRUE; }
  if (K == DFBR_CSS_KW_LIGHTER) { Cs->FontWeight = (Cs->FontWeight >= 600) ? 400 : 100; return DFBR_TRUE; }
  if (CvAsNumber (Cv, &N)) {
    i32 W = (i32)N;
    if (W < 1) W = 1; else if (W > 1000) W = 1000;
    Cs->FontWeight = W;
    return DFBR_TRUE;
  }
  return DFBR_FALSE;
}

/* line-height: normal | <number> | <length> | <percentage> */
static b32
ApplyLineHeight (DfBrCssComputedStyle *Cs, const DfBrCssCv *Cv) {
  DfBrCssKeyword K = CvAsKeyword (Cv);
  f32            N;
  DfBrCssLength  L;

  if (K == DFBR_CSS_KW_NORMAL) {
    Cs->LineHeight.Kind   = DFBR_CSS_LINEHEIGHT_NORMAL;
    Cs->LineHeight.Number = 1.2f;
    return DFBR_TRUE;
  }
  if (CvAsNumber (Cv, &N)) {
    Cs->LineHeight.Kind   = DFBR_CSS_LINEHEIGHT_NUMBER;
    Cs->LineHeight.Number = N;
    return DFBR_TRUE;
  }
  if (DfBrCssCvAsLength (Cv, &L)) {
    Cs->LineHeight.Kind   = DFBR_CSS_LINEHEIGHT_LENGTH;
    Cs->LineHeight.Length = L;
    return DFBR_TRUE;
  }
  return DFBR_FALSE;
}

/* text-decoration: space-separated line keywords. */
static b32
ApplyTextDecoration (DfBrCssComputedStyle *Cs, const DfBrCssCv *Value) {
  const DfBrCssCv *P = SkipWs (Value);
  DfBrCssTextDecorationLines L;
  memset (&L, 0, sizeof (L));

  if (P == DFBR_NULL) return DFBR_FALSE;
  /* `none` clears everything. */
  if (CvIsIdentEq (P, "none")) {
    memset (&Cs->TextDecoration, 0, sizeof (Cs->TextDecoration));
    return DFBR_TRUE;
  }
  while (P != DFBR_NULL) {
    DfBrCssKeyword K = CvAsKeyword (P);
    switch (K) {
      case DFBR_CSS_KW_UNDERLINE:    L.Underline   = DFBR_TRUE; break;
      case DFBR_CSS_KW_OVERLINE:     L.Overline    = DFBR_TRUE; break;
      case DFBR_CSS_KW_LINE_THROUGH: L.LineThrough = DFBR_TRUE; break;
      case DFBR_CSS_KW_BLINK:        L.Blink       = DFBR_TRUE; break;
      default: {
        /* Allow a color tail - attempt and ignore errors. */
        DfBrCssColor Col;
        if (DfBrCssCvAsColor (P, &Col, DFBR_NULL)) {
          Cs->TextDecorationColor = Col;
        } else {
          return DFBR_FALSE;
        }
      }
    }
    P = SkipWs (P->Next);
  }
  Cs->TextDecoration = L;
  return DFBR_TRUE;
}

/* Single-field color apply used by color, border-color-*, etc. */
static b32
ApplySolidColor (DfBrCssColor *Dst, b32 *IsCurrent, const DfBrCssCv *Cv) {
  DfBrCssKeyword K = CvAsKeyword (Cv);
  if (K == DFBR_CSS_KW_CURRENTCOLOR) {
    if (IsCurrent != DFBR_NULL) *IsCurrent = DFBR_TRUE;
    return DFBR_TRUE;
  }
  if (K == DFBR_CSS_KW_TRANSPARENT) {
    Dst->R = 0; Dst->G = 0; Dst->B = 0; Dst->A = 0.0f;
    if (IsCurrent != DFBR_NULL) *IsCurrent = DFBR_FALSE;
    return DFBR_TRUE;
  }
  if (DfBrCssCvAsColor (Cv, Dst, DFBR_NULL)) {
    if (IsCurrent != DFBR_NULL) *IsCurrent = DFBR_FALSE;
    return DFBR_TRUE;
  }
  return DFBR_FALSE;
}

/* ------------------------------------------------------------------
 *  font-family: comma-separated list of <family-name> | <generic>.
 *
 *  Each family is either a <string> or a sequence of <ident>s
 *  joined by single spaces (e.g. `Helvetica Neue, sans-serif`).
 *  Generic keywords (serif / sans-serif / monospace / cursive /
 *  fantasy / system-ui / ui-* / emoji / math / fangsong) are kept
 *  as-is; font matching in the DirectWrite backend uses the raw
 *  strings in order.
 *
 *  Storage: we need to outlive the rule's CV arena, so duplicate
 *  each family name plus the top-level pointer array into the
 *  style context's arena.
 * -----------------------------------------------------------------*/
static void
ApplyFontFamily (
  DfBrCssStyleContext       *Sc,
  DfBrCssComputedStyle      *Cs,
  const DfBrCssCv           *Cv
  )
{
  const DfBrCssCv *P;
  usize            Count = 0;
  usize            Cap   = 4;
  const char     **Names;
  char             Scratch[256];

  if (Sc == DFBR_NULL) return;
  P = SkipWs (Cv);
  if (P == DFBR_NULL) return;

  Names = (const char **)DfBrCssArenaAlloc (&Sc->Arena,
                                             Cap * sizeof (*Names));
  if (Names == DFBR_NULL) return;

  while (P != DFBR_NULL) {
    usize Len = 0;
    /* Consume one family: either a single <string> or a run of
     * <ident>s separated by whitespace, stopping at comma / end. */
    if (P->Kind == DfBrCssCvKindString) {
      if (P->StrLen > 0 && P->StrLen < sizeof (Scratch)) {
        memcpy (Scratch, P->Str, P->StrLen);
        Len = P->StrLen;
      }
      P = SkipWs (P->Next);
    } else {
      while (P != DFBR_NULL && P->Kind == DfBrCssCvKindIdent) {
        if (Len > 0 && Len + 1 < sizeof (Scratch)) {
          Scratch[Len++] = ' ';
        }
        if (Len + P->StrLen < sizeof (Scratch)) {
          memcpy (Scratch + Len, P->Str, P->StrLen);
          Len += P->StrLen;
        }
        P = P->Next;
        while (P != DFBR_NULL && P->Kind == DfBrCssCvKindWhitespace) {
          P = P->Next;
        }
      }
    }

    if (Len > 0) {
      char *Dup = DfBrCssArenaStrDup (&Sc->Arena, Scratch, Len);
      if (Dup == DFBR_NULL) return;
      if (Count + 1 >= Cap) {
        usize NC = Cap * 2;
        const char **NN = (const char **)DfBrCssArenaAlloc (
                          &Sc->Arena, NC * sizeof (*NN));
        usize i;
        if (NN == DFBR_NULL) return;
        for (i = 0; i < Count; i++) NN[i] = Names[i];
        Names = NN; Cap = NC;
      }
      Names[Count++] = Dup;
    }

    /* Skip optional comma + whitespace between families. */
    while (P != DFBR_NULL && P->Kind == DfBrCssCvKindWhitespace) {
      P = P->Next;
    }
    if (P != DFBR_NULL && P->Kind == DfBrCssCvKindComma) {
      P = SkipWs (P->Next);
      continue;
    }
    break;
  }

  if (Count == 0) return;
  Names[Count] = DFBR_NULL;
  Cs->FontFamilies    = Names;
  Cs->FontFamilyCount = Count;
}

/* Forward decl so ApplyFontShorthand can dispatch into the
 * longhand-specific handlers without being bodied yet. */
static b32
ApplyFontWeight (DfBrCssComputedStyle *Cs, const DfBrCssCv *Cv);

/* font: [style || variant || weight || stretch]? size[/line-height] family
 *
 * Also accepts "caption", "icon", "menu", "message-box",
 * "small-caption", "status-bar" system-font keywords, which we
 * don't resolve to OS fonts but do reset to UA defaults.
 */
static void
ApplyFontShorthand (
  DfBrCssStyleContext        *Sc,
  DfBrCssComputedStyle       *Cs,
  const DfBrCssComputedStyle *Parent,
  const DfBrCssCv            *Cv
  )
{
  const DfBrCssCv *P = SkipWs (Cv);
  const DfBrCssCv *SizeCv = DFBR_NULL;
  const DfBrCssCv *FamilyStart = DFBR_NULL;
  usize            NonSize = 0;
  /* Buffer style/weight decisions locally so an invalid shorthand
   * (common on real-world sites: `font: inherit`, `font: Arial`,
   * stale legacy CSS) is a clean no-op instead of a partial mutate
   * that leaves font-style/weight stale while font-size/family
   * stay wrong. */
  b32              HasStyle  = DFBR_FALSE;
  DfBrCssFontStyle StyleVal  = DFBR_CSS_FSTYLE_NORMAL;
  b32              HasWeight = DFBR_FALSE;
  i32              WeightVal = 400;
  b32              HasSize   = DFBR_FALSE;
  DfBrCssLength    SizeVal;
  b32              HasLh     = DFBR_FALSE;
  const DfBrCssCv *LhCv      = DFBR_NULL;

  SizeVal.Auto = DFBR_FALSE; SizeVal.Unit = DFBR_CSS_UNIT_PX;
  SizeVal.Value = 16.0f; SizeVal.CalcExpr = DFBR_NULL;

  if (P == DFBR_NULL) return;

  /* System-font keywords reset font to a sane default; we ignore
   * the flavour but avoid mis-parsing the single keyword as a
   * family name. */
  if (P->Kind == DfBrCssCvKindIdent && P->Next == DFBR_NULL) {
    static const char *SysFonts[] = {
      "caption", "icon", "menu", "message-box",
      "small-caption", "status-bar"
    };
    usize i;
    for (i = 0; i < sizeof (SysFonts) / sizeof (SysFonts[0]); i++) {
      if (DfBrCssAsciiEqZ (P->Str, P->StrLen, SysFonts[i])) return;
    }
    /* Any other bare ident (e.g. `font: Arial;`) is not a valid
     * shorthand - it would need at least `<size> <family>`. Ignore
     * silently instead of scrambling the computed style. */
    return;
  }

  /* Up to four leading ident/number tokens are the optional
   * style|variant|weight|stretch block. Stop as soon as we see
   * a length/number that looks like a font-size. */
  while (P != DFBR_NULL && NonSize < 4) {
    DfBrCssKeyword K = CvAsKeyword (P);
    b32            Consumed = DFBR_FALSE;

    if (K == DFBR_CSS_KW_ITALIC || K == DFBR_CSS_KW_OBLIQUE) {
      StyleVal = (K == DFBR_CSS_KW_ITALIC)
                 ? DFBR_CSS_FSTYLE_ITALIC : DFBR_CSS_FSTYLE_OBLIQUE;
      HasStyle = DFBR_TRUE;
      Consumed = DFBR_TRUE;
    } else if (K == DFBR_CSS_KW_NORMAL) {
      StyleVal = DFBR_CSS_FSTYLE_NORMAL; HasStyle  = DFBR_TRUE;
      WeightVal = 400;                   HasWeight = DFBR_TRUE;
      Consumed = DFBR_TRUE;
    } else if (K == DFBR_CSS_KW_BOLD) {
      WeightVal = 700; HasWeight = DFBR_TRUE; Consumed = DFBR_TRUE;
    } else if (P->Kind == DfBrCssCvKindNumber && P->IsInteger) {
      f32 N = (f32)P->Number;
      if (N >= 1 && N <= 1000) {
        WeightVal = (i32)N; HasWeight = DFBR_TRUE; Consumed = DFBR_TRUE;
      }
    } else if (P->Kind == DfBrCssCvKindIdent &&
               DfBrCssAsciiEqZ (P->Str, P->StrLen, "small-caps")) {
      /* font-variant: small-caps -> we don't model this yet. Skip. */
      Consumed = DFBR_TRUE;
    }

    if (!Consumed) break;
    P = SkipWs (P->Next);
    NonSize++;
  }

  if (P == DFBR_NULL) return;
  SizeCv = P;

  /* Try to resolve size against this token. Only commit the shorthand
   * if the token is actually a size (absolute keyword, length, or
   * percentage); otherwise this is not a valid `font` shorthand. */
  {
    DfBrCssKeyword K = CvAsKeyword (SizeCv);
    static const f32 AbsSizes[] = {
      0.6f, 0.75f, 0.89f, 1.0f, 1.2f, 1.5f, 2.0f
    };
    switch (K) {
      case DFBR_CSS_KW_XX_SMALL: SizeVal.Value = 16.0f * AbsSizes[0]; HasSize = DFBR_TRUE; break;
      case DFBR_CSS_KW_X_SMALL:  SizeVal.Value = 16.0f * AbsSizes[1]; HasSize = DFBR_TRUE; break;
      case DFBR_CSS_KW_SMALL:    SizeVal.Value = 16.0f * AbsSizes[2]; HasSize = DFBR_TRUE; break;
      case DFBR_CSS_KW_MEDIUM:   SizeVal.Value = 16.0f * AbsSizes[3]; HasSize = DFBR_TRUE; break;
      case DFBR_CSS_KW_LARGE:    SizeVal.Value = 16.0f * AbsSizes[4]; HasSize = DFBR_TRUE; break;
      case DFBR_CSS_KW_X_LARGE:  SizeVal.Value = 16.0f * AbsSizes[5]; HasSize = DFBR_TRUE; break;
      case DFBR_CSS_KW_XX_LARGE: SizeVal.Value = 16.0f * AbsSizes[6]; HasSize = DFBR_TRUE; break;
      case DFBR_CSS_KW_LARGER:
      case DFBR_CSS_KW_SMALLER: {
        f32 Base = (Parent != DFBR_NULL) ? Parent->FontSize.Value : 16.0f;
        SizeVal.Value = (K == DFBR_CSS_KW_LARGER) ? Base * 1.2f : Base / 1.2f;
        HasSize = DFBR_TRUE;
        break;
      }
      default:
        if (SizeCv->Kind == DfBrCssCvKindDimension ||
            SizeCv->Kind == DfBrCssCvKindPercentage ||
            (SizeCv->Kind == DfBrCssCvKindNumber && SizeCv->Number == 0)) {
          if (DfBrCssCvAsLength (SizeCv, &SizeVal)) HasSize = DFBR_TRUE;
        }
        break;
    }
  }

  if (!HasSize) return;  /* Not a valid `font` shorthand - stay out. */
  P = SkipWs (P->Next);

  if (P != DFBR_NULL && P->Kind == DfBrCssCvKindDelim &&
      P->StrLen == 1 && P->Str[0] == '/') {
    P = SkipWs (P->Next);
    if (P != DFBR_NULL) {
      LhCv = P; HasLh = DFBR_TRUE;
      P = SkipWs (P->Next);
    }
  }

  FamilyStart = P;
  if (FamilyStart == DFBR_NULL) return;  /* Family is also required. */

  /* All components are valid - commit to the computed style. */
  if (HasStyle)  Cs->FontStyle  = StyleVal;
  if (HasWeight) Cs->FontWeight = WeightVal;
  Cs->FontSize = SizeVal;
  if (HasLh)     ApplyLineHeight (Cs, LhCv);
  ApplyFontFamily (Sc, Cs, FamilyStart);
}

/* Recursively look for a `var(...)` function anywhere in a CV tree.
 * Used to decide whether to run the variable substitution pass on
 * a declaration before applying it. */
static b32
ScanForVarRef (const DfBrCssCv *Cv)
{
  for (; Cv != DFBR_NULL; Cv = Cv->Next) {
    if (Cv->Kind == DfBrCssCvKindFunction && Cv->Str != DFBR_NULL &&
        Cv->StrLen == 3 &&
        (Cv->Str[0] == 'v' || Cv->Str[0] == 'V') &&
        (Cv->Str[1] == 'a' || Cv->Str[1] == 'A') &&
        (Cv->Str[2] == 'r' || Cv->Str[2] == 'R'))
    {
      return DFBR_TRUE;
    }
    if (Cv->Children != DFBR_NULL && ScanForVarRef (Cv->Children)) return DFBR_TRUE;
  }
  return DFBR_FALSE;
}

/* ------------------------------------------------------------------
 *  Gradient parsing (CSS Images Level 3 / 4)
 *
 *  Handles `linear-gradient([<angle> | to <side-or-corner>,]
 *           <color-stop-list>)` and its repeating- counterpart.
 *
 *  Parsing strategy (mirrors LibWeb's CSS::LinearGradientStyleValue):
 *
 *    1. Peek the first arg. If it's an angle/dimension, consume it.
 *       Else if it's the "to" ident, consume `to <side[-or-corner>]`.
 *       Else default to `to bottom` (180deg).
 *    2. After the first comma, each remaining arg is
 *           <color> [<percentage> | <length>]?
 *       separated by commas. Extra args are tolerated but preserved
 *       for paint-time auto-distribution.
 *    3. Normalise positions in [0..1]. Missing positions are filled
 *       using the CSS spec algorithm (first = 0, last = 1, middle
 *       stops linearly interpolated between known neighbours).
 *
 *  Percentage lengths and pixel lengths in stops are supported;
 *  pixel lengths are mapped through the gradient's yet-unknown
 *  length (linear-gradient length is the box's axis projection) -
 *  since we only know the box at paint time, we store pixel stops
 *  as raw pixel numbers in `Position` with a special marker and
 *  the paint code later normalises. For a v1 that already looks
 *  right on the huge majority of sites we just coerce pixel stops
 *  to percentages using a 100px "gradient length" fallback; the
 *  interesting real-world cases all use percent stops or no
 *  explicit positions.
 * -----------------------------------------------------------------*/

static f32
ClampUnit (f32 X) { if (X < 0.0f) return 0.0f; if (X > 1.0f) return 1.0f; return X; }

/* Convert a CSS angle dimension to radians. */
static b32
CvAsAngleRad (const DfBrCssCv *Cv, f32 *Out)
{
  f32 V;
  if (Cv == DFBR_NULL) return DFBR_FALSE;
  if (Cv->Kind == DfBrCssCvKindNumber && Cv->Number == 0.0) {
    *Out = 0.0f; return DFBR_TRUE;
  }
  if (Cv->Kind != DfBrCssCvKindDimension) return DFBR_FALSE;
  V = (f32)Cv->Number;
  switch (Cv->UnitId) {
    case DFBR_CSS_UNIT_DEG:  *Out = V * 0.017453292519943295f; return DFBR_TRUE;
    case DFBR_CSS_UNIT_RAD:  *Out = V; return DFBR_TRUE;
    case DFBR_CSS_UNIT_GRAD: *Out = V * 0.015707963267948966f; return DFBR_TRUE;
    case DFBR_CSS_UNIT_TURN: *Out = V * 6.283185307179586f;     return DFBR_TRUE;
    default: return DFBR_FALSE;
  }
}

/* CSS "gradient line" angle convention:
 *   0deg   = to top
 *   90deg  = to right
 *   180deg = to bottom
 *   270deg = to left
 * After this function, AngleRad is the angle of the gradient vector
 * measured clockwise from the upward direction. */
static f32
GradientAngleFromSides (b32 ToTop, b32 ToBottom, b32 ToLeft, b32 ToRight)
{
  /* Pure sides */
  if (ToTop    && !ToLeft && !ToRight) return 0.0f;
  if (ToRight  && !ToTop  && !ToBottom) return 1.5707963267948966f;   /* 90deg */
  if (ToBottom && !ToLeft && !ToRight) return 3.141592653589793f;     /* 180deg */
  if (ToLeft   && !ToTop  && !ToBottom) return 4.71238898038469f;     /* 270deg */
  /* Corners (CSS Images L3 §3.1 "to corner" - we approximate the
   * corner direction as 45 deg off from the adjacent side pair,
   * which matches how Chrome/FF render when no aspect-ratio
   * correction is required; a fully spec-correct implementation
   * would rotate the gradient to align with the corner diagonal
   * including the box aspect ratio. v1 approximation is good
   * enough for the common `linear-gradient(135deg, ...)` pattern
   * that dominates real-world CSS. */
  if (ToTop    && ToRight)  return 0.7853981633974483f;  /* 45deg */
  if (ToBottom && ToRight)  return 2.356194490192345f;   /* 135deg */
  if (ToBottom && ToLeft)   return 3.9269908169872414f;  /* 225deg */
  if (ToTop    && ToLeft)   return 5.497787143782138f;   /* 315deg */
  return 3.141592653589793f; /* default to bottom */
}

/* Parse one color stop token sequence starting at `*P`. Consumes
 * the color and optional position. Advances `*P` past the stop
 * (skips trailing whitespace but not the comma / EOF). */
static b32
ParseColorStop (const DfBrCssCv **P, DfBrCssColorStop *Out)
{
  const DfBrCssCv *Cur = SkipWs (*P);
  DfBrCssColor Col;
  b32 IsCurrent = DFBR_FALSE;
  if (Cur == DFBR_NULL) return DFBR_FALSE;

  /* Peek for currentcolor/transparent keyword shortcuts first. */
  {
    DfBrCssKeyword K = CvAsKeyword (Cur);
    if (K == DFBR_CSS_KW_CURRENTCOLOR) {
      Col.R = Col.G = Col.B = 0; Col.A = 1.0f;
      IsCurrent = DFBR_TRUE;
      Cur = SkipWs (Cur->Next);
      goto position;
    }
    if (K == DFBR_CSS_KW_TRANSPARENT) {
      Col.R = Col.G = Col.B = 0; Col.A = 0.0f;
      Cur = SkipWs (Cur->Next);
      goto position;
    }
  }
  if (!DfBrCssCvAsColor (Cur, &Col, DFBR_NULL)) return DFBR_FALSE;
  Cur = SkipWs (Cur->Next);

position:
  Out->Color          = Col;
  Out->ColorIsCurrent = IsCurrent;
  Out->Position       = 0.0f;
  Out->PositionAuto   = DFBR_TRUE;

  if (Cur != DFBR_NULL && Cur->Kind != DfBrCssCvKindComma) {
    if (Cur->Kind == DfBrCssCvKindPercentage) {
      Out->Position     = ClampUnit ((f32)Cur->Number / 100.0f);
      Out->PositionAuto = DFBR_FALSE;
      Cur = SkipWs (Cur->Next);
    } else if (Cur->Kind == DfBrCssCvKindDimension) {
      /* Pixel / length stops normalise against the gradient
       * length. We approximate by mapping 1px = 1% (so `0px`
       * and `100px` remain usable endpoints); this loses
       * absolute precision but keeps simple two-stop gradients
       * like `linear-gradient(red 0, blue 100px)` visually
       * correct for the common landing-page uses. */
      f32 Px = (f32)Cur->Number;
      Out->Position     = ClampUnit (Px / 100.0f);
      Out->PositionAuto = DFBR_FALSE;
      Cur = SkipWs (Cur->Next);
    } else if (Cur->Kind == DfBrCssCvKindNumber && Cur->Number == 0.0) {
      Out->Position     = 0.0f;
      Out->PositionAuto = DFBR_FALSE;
      Cur = SkipWs (Cur->Next);
    }
  }

  *P = Cur;
  return DFBR_TRUE;
}

/* Distribute auto-positioned stops per CSS Images L3 §3.5. */
static void
DistributeStopPositions (DfBrCssColorStop *Stops, usize N)
{
  usize i, j;
  if (N == 0) return;
  /* First and last stops anchor at 0 and 1 respectively. */
  if (Stops[0].PositionAuto)     { Stops[0].Position = 0.0f;     Stops[0].PositionAuto = DFBR_FALSE; }
  if (Stops[N-1].PositionAuto)   { Stops[N-1].Position = 1.0f;   Stops[N-1].PositionAuto = DFBR_FALSE; }
  /* Monotonic clamp - positions never regress. */
  for (i = 1; i < N; i++) {
    if (!Stops[i].PositionAuto && Stops[i].Position < Stops[i-1].Position) {
      Stops[i].Position = Stops[i-1].Position;
    }
  }
  /* Fill runs of auto stops by linear interpolation between the
   * known neighbours. */
  for (i = 0; i < N; i++) {
    if (!Stops[i].PositionAuto) continue;
    /* Find the next fixed stop. */
    for (j = i + 1; j < N && Stops[j].PositionAuto; j++) { /* noop */ }
    if (j >= N) break;
    {
      f32 Lo = Stops[i - 1].Position;
      f32 Hi = Stops[j].Position;
      usize Run = j - i + 1;
      usize k;
      for (k = 0; k < j - i; k++) {
        Stops[i + k].Position     = Lo + (Hi - Lo) * ((f32)(k + 1) / (f32)Run);
        Stops[i + k].PositionAuto = DFBR_FALSE;
      }
      i = j - 1;
    }
  }
}

/* Parse the arg list of a (repeating-)?linear-gradient() function.
 * `Args` is the head of the Function CV's Children list. On success,
 * returns an arena-allocated DfBrCssGradient. On failure returns
 * NULL (caller may fall back to a solid background). */
static const DfBrCssGradient *
ParseLinearGradientArgs (DfBrCssStyleContext *Sc,
                         const DfBrCssCv     *Args,
                         b32                  Repeating)
{
  const DfBrCssCv *P = SkipWs (Args);
  f32              Angle = 3.141592653589793f; /* default `to bottom` */
  DfBrCssColorStop Scratch[32];
  usize            N = 0;
  DfBrCssGradient *G;
  DfBrCssColorStop *StopsOut;
  usize i;

  if (P == DFBR_NULL) return DFBR_NULL;

  /* Angle / direction. If the first chunk is a number/dim that
   * resolves to an angle, consume it. If it's `to <sides>`, consume
   * that. Otherwise the default `to bottom` stands and this chunk
   * is a color stop. */
  if (CvIsIdentEq (P, "to")) {
    b32 ToTop = 0, ToBottom = 0, ToLeft = 0, ToRight = 0;
    P = SkipWs (P->Next);
    while (P != DFBR_NULL && P->Kind != DfBrCssCvKindComma) {
      DfBrCssKeyword Kw = CvAsKeyword (P);
      if (Kw == DFBR_CSS_KW_TOP)    ToTop    = 1;
      else if (Kw == DFBR_CSS_KW_BOTTOM) ToBottom = 1;
      else if (Kw == DFBR_CSS_KW_LEFT)   ToLeft   = 1;
      else if (Kw == DFBR_CSS_KW_RIGHT)  ToRight  = 1;
      else break;
      P = SkipWs (P->Next);
    }
    Angle = GradientAngleFromSides (ToTop, ToBottom, ToLeft, ToRight);
  } else {
    f32 A;
    if (CvAsAngleRad (P, &A)) {
      Angle = A;
      P = SkipWs (P->Next);
    }
  }

  /* Skip the mandatory comma before the stop list, if present.
   * (It's absent when the gradient has no explicit direction.) */
  if (P != DFBR_NULL && P->Kind == DfBrCssCvKindComma) {
    P = SkipWs (P->Next);
  }

  while (P != DFBR_NULL && N < 32) {
    DfBrCssColorStop S;
    if (!ParseColorStop (&P, &S)) break;
    Scratch[N++] = S;
    /* Stop lists are comma-separated. */
    if (P != DFBR_NULL && P->Kind == DfBrCssCvKindComma) {
      P = SkipWs (P->Next);
    } else {
      break;
    }
  }

  if (N < 2) return DFBR_NULL;  /* a gradient needs at least two stops */

  DistributeStopPositions (Scratch, N);

  G = (DfBrCssGradient *)DfBrCssArenaZero (&Sc->Arena, sizeof (*G));
  if (G == DFBR_NULL) return DFBR_NULL;
  StopsOut = (DfBrCssColorStop *)DfBrCssArenaAlloc (&Sc->Arena,
                                                   N * sizeof (*StopsOut));
  if (StopsOut == DFBR_NULL) return DFBR_NULL;
  for (i = 0; i < N; i++) StopsOut[i] = Scratch[i];
  G->Kind      = DFBR_CSS_GRADIENT_LINEAR;
  G->AngleRad  = Angle;
  G->Repeating = Repeating;
  G->Stops     = StopsOut;
  G->StopCount = N;
  return G;
}

/* Try to interpret Cv (a single component value) as a <image> that
 * we model natively. Returns the gradient on success, NULL
 * otherwise. Consumes nothing; caller advances past the CV. */
static const DfBrCssGradient *
TryParseGradient (DfBrCssStyleContext *Sc, const DfBrCssCv *Cv)
{
  if (Cv == DFBR_NULL || Cv->Kind != DfBrCssCvKindFunction) return DFBR_NULL;
  if (DfBrCssAsciiEqZ (Cv->Str, Cv->StrLen, "linear-gradient")) {
    return ParseLinearGradientArgs (Sc, Cv->Children, DFBR_FALSE);
  }
  if (DfBrCssAsciiEqZ (Cv->Str, Cv->StrLen, "repeating-linear-gradient")) {
    return ParseLinearGradientArgs (Sc, Cv->Children, DFBR_TRUE);
  }
  return DFBR_NULL;
}

/* ------------------------------------------------------------------
 *  Single-property application
 * -----------------------------------------------------------------*/

static void
ApplySimple (
  DfBrCssStyleContext        *Sc,
  DfBrCssComputedStyle       *Cs,
  const DfBrCssComputedStyle *Parent,
  DfBrCssProp                 P,
  const DfBrCssCv            *Value
  )
{
  const DfBrCssCv *Cv = SkipWs (Value);
  DfBrCssKeyword   K  = (Cv != DFBR_NULL) ? CvAsKeyword (Cv) : DFBR_CSS_KW_UNKNOWN;

  /* CSS-wide keywords: `initial`, `inherit`, `unset`, `revert`. They
   * apply *before* any value parsing and short-circuit the rest of
   * this function. Per CSS-Cascade-4:
   *   - initial  -> property's initial value
   *   - inherit  -> parent's computed value
   *   - unset    -> inherit if inherited, else initial
   *   - revert   -> cascade from origins above the UA (we approximate
   *                 with `unset`, which gives the right answer when
   *                 there is no user stylesheet)
   *
   * We use a big data-driven switch per-property so any property that
   * lives on DfBrCssComputedStyle can correctly roll back - this is
   * crucial for modern reset/normalize sheets which lean heavily on
   * `all: unset` and `<prop>: revert`. */
#define WIDE_COPY_INIT(F) do { Cs->F = Init.F; } while (0)
#define WIDE_COPY_INH(F)  do { if (Parent != DFBR_NULL) Cs->F = Parent->F; } while (0)

  if (K == DFBR_CSS_KW_INITIAL || K == DFBR_CSS_KW_INHERIT ||
      K == DFBR_CSS_KW_UNSET   || K == DFBR_CSS_KW_REVERT)
  {
    DfBrCssComputedStyle Init;
    /* `Inherited`: per Appendix F, which properties inherit by
     * default. Drives `unset`/`revert`. */
    b32 Inherited = DFBR_FALSE;
    b32 DoInherit;
    SeedInitial (&Init);

    switch (P) {
      case DFBR_CSS_PROP_COLOR:
      case DFBR_CSS_PROP_FONT_SIZE:
      case DFBR_CSS_PROP_FONT_WEIGHT:
      case DFBR_CSS_PROP_FONT_STYLE:
      case DFBR_CSS_PROP_FONT_FAMILY:
      case DFBR_CSS_PROP_LINE_HEIGHT:
      case DFBR_CSS_PROP_TEXT_ALIGN:
      case DFBR_CSS_PROP_TEXT_TRANSFORM:
      case DFBR_CSS_PROP_TEXT_INDENT:
      case DFBR_CSS_PROP_LETTER_SPACING:
      case DFBR_CSS_PROP_WORD_SPACING:
      case DFBR_CSS_PROP_WHITE_SPACE:
      case DFBR_CSS_PROP_VISIBILITY:
      case DFBR_CSS_PROP_LIST_STYLE_TYPE:
      case DFBR_CSS_PROP_CURSOR:
        Inherited = DFBR_TRUE;
        break;
      default:
        Inherited = DFBR_FALSE;
    }

    DoInherit = (K == DFBR_CSS_KW_INHERIT) ||
                ((K == DFBR_CSS_KW_UNSET || K == DFBR_CSS_KW_REVERT) && Inherited);

    if (DoInherit && Parent == DFBR_NULL) {
      /* No parent -> fall back to initial. */
      DoInherit = DFBR_FALSE;
    }

#define WIDE(F) do { if (DoInherit) WIDE_COPY_INH(F); else WIDE_COPY_INIT(F); } while (0)
    switch (P) {
      case DFBR_CSS_PROP_DISPLAY:          WIDE(Display);          return;
      case DFBR_CSS_PROP_POSITION:         WIDE(Position);         return;
      case DFBR_CSS_PROP_FLOAT:            WIDE(Float);            return;
      case DFBR_CSS_PROP_CLEAR:            WIDE(Clear);            return;
      case DFBR_CSS_PROP_VISIBILITY:       WIDE(Visibility);       return;
      case DFBR_CSS_PROP_OVERFLOW:
        WIDE(OverflowX); WIDE(OverflowY); return;
      case DFBR_CSS_PROP_OVERFLOW_X:       WIDE(OverflowX);        return;
      case DFBR_CSS_PROP_OVERFLOW_Y:       WIDE(OverflowY);        return;
      case DFBR_CSS_PROP_BOX_SIZING:       WIDE(BoxSizing);        return;
      case DFBR_CSS_PROP_Z_INDEX:
        WIDE(ZIndex); WIDE(ZIndexAuto);    return;
      case DFBR_CSS_PROP_OPACITY:          WIDE(Opacity);          return;
      case DFBR_CSS_PROP_COLOR:            WIDE(Color);            return;
      case DFBR_CSS_PROP_BACKGROUND_COLOR:
        WIDE(BackgroundColor); WIDE(BackgroundColorIsCurrent); return;
      case DFBR_CSS_PROP_BACKGROUND_IMAGE:
        WIDE(BackgroundImageUrl); WIDE(BackgroundImageUrlLen);
        WIDE(BackgroundGradient); return;
      case DFBR_CSS_PROP_BACKGROUND_CLIP:
        WIDE(BackgroundClip); return;
      case DFBR_CSS_PROP_WIDTH:            WIDE(Width);            return;
      case DFBR_CSS_PROP_HEIGHT:           WIDE(Height);           return;
      case DFBR_CSS_PROP_MIN_WIDTH:        WIDE(MinWidth);         return;
      case DFBR_CSS_PROP_MIN_HEIGHT:       WIDE(MinHeight);        return;
      case DFBR_CSS_PROP_MAX_WIDTH:        WIDE(MaxWidth);         return;
      case DFBR_CSS_PROP_MAX_HEIGHT:       WIDE(MaxHeight);        return;
      case DFBR_CSS_PROP_TOP:              WIDE(Top);              return;
      case DFBR_CSS_PROP_RIGHT:            WIDE(Right);            return;
      case DFBR_CSS_PROP_BOTTOM:           WIDE(Bottom);           return;
      case DFBR_CSS_PROP_LEFT:             WIDE(Left);             return;
      case DFBR_CSS_PROP_MARGIN_TOP:       WIDE(MarginTop);        return;
      case DFBR_CSS_PROP_MARGIN_RIGHT:     WIDE(MarginRight);      return;
      case DFBR_CSS_PROP_MARGIN_BOTTOM:    WIDE(MarginBottom);     return;
      case DFBR_CSS_PROP_MARGIN_LEFT:      WIDE(MarginLeft);       return;
      case DFBR_CSS_PROP_PADDING_TOP:      WIDE(PaddingTop);       return;
      case DFBR_CSS_PROP_PADDING_RIGHT:    WIDE(PaddingRight);     return;
      case DFBR_CSS_PROP_PADDING_BOTTOM:   WIDE(PaddingBottom);    return;
      case DFBR_CSS_PROP_PADDING_LEFT:     WIDE(PaddingLeft);      return;
      case DFBR_CSS_PROP_BORDER_TOP_WIDTH:    WIDE(BorderTop.Width);    return;
      case DFBR_CSS_PROP_BORDER_RIGHT_WIDTH:  WIDE(BorderRight.Width);  return;
      case DFBR_CSS_PROP_BORDER_BOTTOM_WIDTH: WIDE(BorderBottom.Width); return;
      case DFBR_CSS_PROP_BORDER_LEFT_WIDTH:   WIDE(BorderLeft.Width);   return;
      case DFBR_CSS_PROP_BORDER_TOP_STYLE:    WIDE(BorderTop.Style);    return;
      case DFBR_CSS_PROP_BORDER_RIGHT_STYLE:  WIDE(BorderRight.Style);  return;
      case DFBR_CSS_PROP_BORDER_BOTTOM_STYLE: WIDE(BorderBottom.Style); return;
      case DFBR_CSS_PROP_BORDER_LEFT_STYLE:   WIDE(BorderLeft.Style);   return;
      case DFBR_CSS_PROP_BORDER_TOP_COLOR:    WIDE(BorderTop.Color);    WIDE(BorderTop.ColorIsCurrent);    return;
      case DFBR_CSS_PROP_BORDER_RIGHT_COLOR:  WIDE(BorderRight.Color);  WIDE(BorderRight.ColorIsCurrent);  return;
      case DFBR_CSS_PROP_BORDER_BOTTOM_COLOR: WIDE(BorderBottom.Color); WIDE(BorderBottom.ColorIsCurrent); return;
      case DFBR_CSS_PROP_BORDER_LEFT_COLOR:   WIDE(BorderLeft.Color);   WIDE(BorderLeft.ColorIsCurrent);   return;
      case DFBR_CSS_PROP_FONT_SIZE:        WIDE(FontSize);         return;
      case DFBR_CSS_PROP_FONT_WEIGHT:      WIDE(FontWeight);       return;
      case DFBR_CSS_PROP_FONT_STYLE:       WIDE(FontStyle);        return;
      case DFBR_CSS_PROP_FONT_FAMILY:
        WIDE(FontFamilies); WIDE(FontFamilyCount); return;
      case DFBR_CSS_PROP_LINE_HEIGHT:      WIDE(LineHeight);       return;
      case DFBR_CSS_PROP_TEXT_ALIGN:       WIDE(TextAlign);        return;
      case DFBR_CSS_PROP_TEXT_TRANSFORM:   WIDE(TextTransform);    return;
      case DFBR_CSS_PROP_TEXT_INDENT:      WIDE(TextIndent);       return;
      case DFBR_CSS_PROP_LETTER_SPACING:   WIDE(LetterSpacing);    return;
      case DFBR_CSS_PROP_WORD_SPACING:     WIDE(WordSpacing);      return;
      case DFBR_CSS_PROP_WHITE_SPACE:      WIDE(WhiteSpace);       return;
      case DFBR_CSS_PROP_LIST_STYLE_TYPE:  WIDE(ListStyleType);    return;
      case DFBR_CSS_PROP_VERTICAL_ALIGN:
        WIDE(VerticalAlign); WIDE(VerticalAlignLength); return;
      case DFBR_CSS_PROP_FLEX_DIRECTION:   WIDE(FlexDirection);    return;
      case DFBR_CSS_PROP_FLEX_WRAP:        WIDE(FlexWrap);         return;
      case DFBR_CSS_PROP_FLEX_GROW:        WIDE(FlexGrow);         return;
      case DFBR_CSS_PROP_FLEX_SHRINK:      WIDE(FlexShrink);       return;
      case DFBR_CSS_PROP_FLEX_BASIS:       WIDE(FlexBasis);        return;
      case DFBR_CSS_PROP_JUSTIFY_CONTENT:  WIDE(JustifyContent);   return;
      case DFBR_CSS_PROP_ALIGN_ITEMS:      WIDE(AlignItems);       return;
      case DFBR_CSS_PROP_ALIGN_SELF:       WIDE(AlignSelf);        return;
      case DFBR_CSS_PROP_ALIGN_CONTENT:    WIDE(AlignContent);     return;
      case DFBR_CSS_PROP_ROW_GAP:          WIDE(RowGap);           return;
      case DFBR_CSS_PROP_COLUMN_GAP:       WIDE(ColumnGap);        return;
      case DFBR_CSS_PROP_ORDER:            WIDE(Order);            return;
      case DFBR_CSS_PROP_BORDER_RADIUS:
        WIDE(BorderTopLeftRadius);
        WIDE(BorderTopRightRadius);
        WIDE(BorderBottomRightRadius);
        WIDE(BorderBottomLeftRadius);
        return;
      default:
        /* Shorthands and anything we haven't individually wired up
         * fall through to be re-parsed as a keyword below, where they
         * are no-ops. */
        return;
    }
#undef WIDE
  }
#undef WIDE_COPY_INIT
#undef WIDE_COPY_INH

  /* Actual value parsing. */
  switch (P) {
    case DFBR_CSS_PROP_DISPLAY:
      KwToDisplay (K, &Cs->Display); return;
    case DFBR_CSS_PROP_POSITION:
      KwToPosition (K, &Cs->Position); return;
    case DFBR_CSS_PROP_FLOAT:
      KwToFloat (K, &Cs->Float); return;
    case DFBR_CSS_PROP_CLEAR: {
      switch (K) {
        case DFBR_CSS_KW_NONE:  Cs->Clear = DFBR_CSS_CLEAR_NONE; break;
        case DFBR_CSS_KW_LEFT:  Cs->Clear = DFBR_CSS_CLEAR_LEFT; break;
        case DFBR_CSS_KW_RIGHT: Cs->Clear = DFBR_CSS_CLEAR_RIGHT; break;
        case DFBR_CSS_KW_BOTH:  Cs->Clear = DFBR_CSS_CLEAR_BOTH; break;
        default: break;
      }
      return;
    }
    case DFBR_CSS_PROP_VISIBILITY:
      KwToVisibility (K, &Cs->Visibility); return;
    case DFBR_CSS_PROP_OVERFLOW: {
      DfBrCssOverflowKind V;
      if (KwToOverflow (K, &V)) { Cs->OverflowX = Cs->OverflowY = V; }
      return;
    }
    case DFBR_CSS_PROP_OVERFLOW_X: KwToOverflow (K, &Cs->OverflowX); return;
    case DFBR_CSS_PROP_OVERFLOW_Y: KwToOverflow (K, &Cs->OverflowY); return;
    case DFBR_CSS_PROP_BOX_SIZING: KwToBoxSizing (K, &Cs->BoxSizing); return;
    case DFBR_CSS_PROP_Z_INDEX: {
      f32 N;
      if (K == DFBR_CSS_KW_AUTO) { Cs->ZIndexAuto = DFBR_TRUE; Cs->ZIndex = 0; return; }
      if (CvAsNumber (Cv, &N))   { Cs->ZIndexAuto = DFBR_FALSE; Cs->ZIndex = (i32)N; return; }
      return;
    }
    case DFBR_CSS_PROP_OPACITY: {
      f32 N;
      if (CvAsNumber (Cv, &N)) {
        if (N < 0.0f) N = 0.0f; else if (N > 1.0f) N = 1.0f;
        Cs->Opacity = N;
      }
      return;
    }

    case DFBR_CSS_PROP_WIDTH:     if (DfBrCssCvIsAuto (Cv)) Cs->Width     = AutoLen (); else DfBrCssCvAsLength (Cv, &Cs->Width); return;
    case DFBR_CSS_PROP_HEIGHT:    if (DfBrCssCvIsAuto (Cv)) Cs->Height    = AutoLen (); else DfBrCssCvAsLength (Cv, &Cs->Height); return;
    case DFBR_CSS_PROP_MIN_WIDTH:  DfBrCssCvAsLength (Cv, &Cs->MinWidth);  return;
    case DFBR_CSS_PROP_MIN_HEIGHT: DfBrCssCvAsLength (Cv, &Cs->MinHeight); return;
    case DFBR_CSS_PROP_MAX_WIDTH:  if (K == DFBR_CSS_KW_NONE) Cs->MaxWidth  = AutoLen (); else DfBrCssCvAsLength (Cv, &Cs->MaxWidth);  return;
    case DFBR_CSS_PROP_MAX_HEIGHT: if (K == DFBR_CSS_KW_NONE) Cs->MaxHeight = AutoLen (); else DfBrCssCvAsLength (Cv, &Cs->MaxHeight); return;

    case DFBR_CSS_PROP_TOP:    if (DfBrCssCvIsAuto (Cv)) Cs->Top    = AutoLen (); else DfBrCssCvAsLength (Cv, &Cs->Top);    return;
    case DFBR_CSS_PROP_RIGHT:  if (DfBrCssCvIsAuto (Cv)) Cs->Right  = AutoLen (); else DfBrCssCvAsLength (Cv, &Cs->Right);  return;
    case DFBR_CSS_PROP_BOTTOM: if (DfBrCssCvIsAuto (Cv)) Cs->Bottom = AutoLen (); else DfBrCssCvAsLength (Cv, &Cs->Bottom); return;
    case DFBR_CSS_PROP_LEFT:   if (DfBrCssCvIsAuto (Cv)) Cs->Left   = AutoLen (); else DfBrCssCvAsLength (Cv, &Cs->Left);   return;

    case DFBR_CSS_PROP_MARGIN: {
      DfBrCssLength V[4];
      if (ParseFourLengths (Cv, V)) {
        Cs->MarginTop    = V[0]; Cs->MarginRight  = V[1];
        Cs->MarginBottom = V[2]; Cs->MarginLeft   = V[3];
      }
      return;
    }
    case DFBR_CSS_PROP_MARGIN_TOP:    if (DfBrCssCvIsAuto (Cv)) Cs->MarginTop    = AutoLen (); else DfBrCssCvAsLength (Cv, &Cs->MarginTop);    return;
    case DFBR_CSS_PROP_MARGIN_RIGHT:  if (DfBrCssCvIsAuto (Cv)) Cs->MarginRight  = AutoLen (); else DfBrCssCvAsLength (Cv, &Cs->MarginRight);  return;
    case DFBR_CSS_PROP_MARGIN_BOTTOM: if (DfBrCssCvIsAuto (Cv)) Cs->MarginBottom = AutoLen (); else DfBrCssCvAsLength (Cv, &Cs->MarginBottom); return;
    case DFBR_CSS_PROP_MARGIN_LEFT:   if (DfBrCssCvIsAuto (Cv)) Cs->MarginLeft   = AutoLen (); else DfBrCssCvAsLength (Cv, &Cs->MarginLeft);   return;

    case DFBR_CSS_PROP_PADDING: {
      DfBrCssLength V[4];
      if (ParseFourLengths (Cv, V)) {
        Cs->PaddingTop    = V[0]; Cs->PaddingRight  = V[1];
        Cs->PaddingBottom = V[2]; Cs->PaddingLeft   = V[3];
      }
      return;
    }
    case DFBR_CSS_PROP_PADDING_TOP:    DfBrCssCvAsLength (Cv, &Cs->PaddingTop);    return;
    case DFBR_CSS_PROP_PADDING_RIGHT:  DfBrCssCvAsLength (Cv, &Cs->PaddingRight);  return;
    case DFBR_CSS_PROP_PADDING_BOTTOM: DfBrCssCvAsLength (Cv, &Cs->PaddingBottom); return;
    case DFBR_CSS_PROP_PADDING_LEFT:   DfBrCssCvAsLength (Cv, &Cs->PaddingLeft);   return;

    /* border-<side>-width: <length> */
    case DFBR_CSS_PROP_BORDER_TOP_WIDTH:    DfBrCssCvAsLength (Cv, &Cs->BorderTop.Width);    return;
    case DFBR_CSS_PROP_BORDER_RIGHT_WIDTH:  DfBrCssCvAsLength (Cv, &Cs->BorderRight.Width);  return;
    case DFBR_CSS_PROP_BORDER_BOTTOM_WIDTH: DfBrCssCvAsLength (Cv, &Cs->BorderBottom.Width); return;
    case DFBR_CSS_PROP_BORDER_LEFT_WIDTH:   DfBrCssCvAsLength (Cv, &Cs->BorderLeft.Width);   return;

    case DFBR_CSS_PROP_BORDER_TOP_STYLE:    KwToBorderStyle (K, &Cs->BorderTop.Style);    return;
    case DFBR_CSS_PROP_BORDER_RIGHT_STYLE:  KwToBorderStyle (K, &Cs->BorderRight.Style);  return;
    case DFBR_CSS_PROP_BORDER_BOTTOM_STYLE: KwToBorderStyle (K, &Cs->BorderBottom.Style); return;
    case DFBR_CSS_PROP_BORDER_LEFT_STYLE:   KwToBorderStyle (K, &Cs->BorderLeft.Style);   return;

    case DFBR_CSS_PROP_BORDER_TOP_COLOR:    ApplySolidColor (&Cs->BorderTop.Color,    &Cs->BorderTop.ColorIsCurrent,    Cv); return;
    case DFBR_CSS_PROP_BORDER_RIGHT_COLOR:  ApplySolidColor (&Cs->BorderRight.Color,  &Cs->BorderRight.ColorIsCurrent,  Cv); return;
    case DFBR_CSS_PROP_BORDER_BOTTOM_COLOR: ApplySolidColor (&Cs->BorderBottom.Color, &Cs->BorderBottom.ColorIsCurrent, Cv); return;
    case DFBR_CSS_PROP_BORDER_LEFT_COLOR:   ApplySolidColor (&Cs->BorderLeft.Color,   &Cs->BorderLeft.ColorIsCurrent,   Cv); return;

    case DFBR_CSS_PROP_BORDER_WIDTH: {
      DfBrCssLength V[4];
      if (ParseFourLengths (Cv, V)) {
        Cs->BorderTop.Width    = V[0]; Cs->BorderRight.Width  = V[1];
        Cs->BorderBottom.Width = V[2]; Cs->BorderLeft.Width   = V[3];
      }
      return;
    }
    case DFBR_CSS_PROP_BORDER_STYLE: {
      DfBrCssBorderStyleK S;
      if (KwToBorderStyle (K, &S)) {
        Cs->BorderTop.Style = Cs->BorderRight.Style = Cs->BorderBottom.Style = Cs->BorderLeft.Style = S;
      }
      return;
    }
    case DFBR_CSS_PROP_BORDER_COLOR: {
      DfBrCssColor Col; b32 Cur;
      if (ApplySolidColor (&Col, &Cur, Cv)) {
        Cs->BorderTop.Color    = Cs->BorderRight.Color    = Cs->BorderBottom.Color    = Cs->BorderLeft.Color    = Col;
        Cs->BorderTop.ColorIsCurrent = Cs->BorderRight.ColorIsCurrent = Cs->BorderBottom.ColorIsCurrent = Cs->BorderLeft.ColorIsCurrent = Cur;
      }
      return;
    }
    case DFBR_CSS_PROP_BORDER:
    case DFBR_CSS_PROP_BORDER_TOP:
    case DFBR_CSS_PROP_BORDER_RIGHT:
    case DFBR_CSS_PROP_BORDER_BOTTOM:
    case DFBR_CSS_PROP_BORDER_LEFT: {
      /* <line-width> || <line-style> || <color> in any order. */
      DfBrCssBorder        B;
      const DfBrCssCv     *PCv = SkipWs (Cv);
      DfBrCssBorderStyleK  Style; b32 HaveStyle = DFBR_FALSE;
      DfBrCssLength        Width; b32 HaveWidth = DFBR_FALSE;
      DfBrCssColor         Col;   b32 HaveColor = DFBR_FALSE;
      b32                  IsCur  = DFBR_FALSE;

      while (PCv != DFBR_NULL) {
        DfBrCssKeyword     KK = CvAsKeyword (PCv);
        DfBrCssBorderStyleK St;
        DfBrCssLength      L;
        if (KwToBorderStyle (KK, &St)) { Style = St; HaveStyle = DFBR_TRUE; PCv = SkipWs (PCv->Next); continue; }
        if (KK == DFBR_CSS_KW_THIN || KK == DFBR_CSS_KW_MEDIUM || KK == DFBR_CSS_KW_THICK) {
          L.Auto = DFBR_FALSE; L.Unit = DFBR_CSS_UNIT_PX;
          L.Value = (KK == DFBR_CSS_KW_THIN) ? 1.0f : (KK == DFBR_CSS_KW_MEDIUM) ? 3.0f : 5.0f;
          Width = L; HaveWidth = DFBR_TRUE; PCv = SkipWs (PCv->Next); continue;
        }
        if (DfBrCssCvAsLength (PCv, &L)) { Width = L; HaveWidth = DFBR_TRUE; PCv = SkipWs (PCv->Next); continue; }
        if (DfBrCssCvAsColor (PCv, &Col, DFBR_NULL) ||
            KK == DFBR_CSS_KW_CURRENTCOLOR ||
            KK == DFBR_CSS_KW_TRANSPARENT) {
          if (KK == DFBR_CSS_KW_CURRENTCOLOR) { IsCur = DFBR_TRUE; }
          else if (KK == DFBR_CSS_KW_TRANSPARENT) { Col.R = Col.G = Col.B = 0; Col.A = 0.0f; }
          HaveColor = DFBR_TRUE; PCv = SkipWs (PCv->Next); continue;
        }
        break; /* unknown token - bail */
      }

      memset (&B, 0, sizeof (B));
      B.Width          = HaveWidth ? Width : ZeroLen ();
      B.Style          = HaveStyle ? Style : DFBR_CSS_BORDER_NONE;
      B.Color          = HaveColor && !IsCur ? Col : Rgba (0,0,0,1.0f);
      B.ColorIsCurrent = (HaveColor && IsCur) || !HaveColor;

      switch (P) {
        case DFBR_CSS_PROP_BORDER:        Cs->BorderTop = Cs->BorderRight = Cs->BorderBottom = Cs->BorderLeft = B; break;
        case DFBR_CSS_PROP_BORDER_TOP:    Cs->BorderTop    = B; break;
        case DFBR_CSS_PROP_BORDER_RIGHT:  Cs->BorderRight  = B; break;
        case DFBR_CSS_PROP_BORDER_BOTTOM: Cs->BorderBottom = B; break;
        case DFBR_CSS_PROP_BORDER_LEFT:   Cs->BorderLeft   = B; break;
        default: break;
      }
      return;
    }
    case DFBR_CSS_PROP_BORDER_RADIUS: {
      DfBrCssLength V[4];
      if (ParseFourLengths (Cv, V)) {
        Cs->BorderTopLeftRadius     = V[0];
        Cs->BorderTopRightRadius    = V[1];
        Cs->BorderBottomRightRadius = V[2];
        Cs->BorderBottomLeftRadius  = V[3];
      }
      return;
    }

    case DFBR_CSS_PROP_BACKGROUND_COLOR:
      ApplySolidColor (&Cs->BackgroundColor, &Cs->BackgroundColorIsCurrent, Cv);
      return;
    case DFBR_CSS_PROP_BACKGROUND_IMAGE: {
      const DfBrCssGradient *G;
      if (K == DFBR_CSS_KW_NONE) {
        Cs->BackgroundImageUrl = DFBR_NULL; Cs->BackgroundImageUrlLen = 0;
        Cs->BackgroundGradient = DFBR_NULL;
        return;
      }
      if (Cv != DFBR_NULL && Cv->Kind == DfBrCssCvKindUrl) {
        Cs->BackgroundImageUrl    = Cv->Str;
        Cs->BackgroundImageUrlLen = Cv->StrLen;
        Cs->BackgroundGradient    = DFBR_NULL;
        return;
      }
      G = TryParseGradient (Sc, Cv);
      if (G != DFBR_NULL) {
        Cs->BackgroundGradient    = G;
        Cs->BackgroundImageUrl    = DFBR_NULL;
        Cs->BackgroundImageUrlLen = 0;
      }
      return;
    }
    case DFBR_CSS_PROP_BACKGROUND: {
      /* Simplified shorthand: walk the component-value list in
       * one pass, recognising color, url(), gradient functions,
       * and the `none` keyword. Everything else (position, size,
       * repeat, attachment, box) is tolerated and skipped. */
      const DfBrCssCv *PCv = SkipWs (Cv);
      while (PCv != DFBR_NULL) {
        DfBrCssKeyword KK = CvAsKeyword (PCv);
        DfBrCssColor   Col; b32 Cur;
        const DfBrCssGradient *G = TryParseGradient (Sc, PCv);
        if (G != DFBR_NULL) {
          Cs->BackgroundGradient    = G;
          Cs->BackgroundImageUrl    = DFBR_NULL;
          Cs->BackgroundImageUrlLen = 0;
        } else if (PCv->Kind == DfBrCssCvKindUrl) {
          Cs->BackgroundImageUrl    = PCv->Str;
          Cs->BackgroundImageUrlLen = PCv->StrLen;
          Cs->BackgroundGradient    = DFBR_NULL;
        } else if (KK == DFBR_CSS_KW_NONE) {
          Cs->BackgroundImageUrl = DFBR_NULL; Cs->BackgroundImageUrlLen = 0;
          Cs->BackgroundGradient = DFBR_NULL;
        } else if (ApplySolidColor (&Col, &Cur, PCv)) {
          Cs->BackgroundColor          = Col;
          Cs->BackgroundColorIsCurrent = Cur;
        }
        PCv = SkipWs (PCv->Next);
      }
      return;
    }
    case DFBR_CSS_PROP_BACKGROUND_CLIP: {
      switch (K) {
        case DFBR_CSS_KW_BORDER_BOX:  Cs->BackgroundClip = DFBR_CSS_BGCLIP_BORDER_BOX;  return;
        case DFBR_CSS_KW_PADDING_BOX: Cs->BackgroundClip = DFBR_CSS_BGCLIP_PADDING_BOX; return;
        case DFBR_CSS_KW_CONTENT_BOX: Cs->BackgroundClip = DFBR_CSS_BGCLIP_CONTENT_BOX; return;
        case DFBR_CSS_KW_TEXT:        Cs->BackgroundClip = DFBR_CSS_BGCLIP_TEXT;        return;
        default: return;
      }
    }
    case DFBR_CSS_PROP_BACKGROUND_ORIGIN:
      /* Parsed, preserved as initial for now - paint uses border-box
       * uniformly, so background-origin is a v2 feature. */
      return;

    case DFBR_CSS_PROP_COLOR:
      ApplySolidColor (&Cs->Color, DFBR_NULL, Cv);
      return;
    case DFBR_CSS_PROP_FONT_SIZE: {
      /* DfBrCssLength carries a CalcExpr pointer alongside
       * Value/Unit/Auto; forgetting to zero it here used to leak a
       * stack-garbage pointer into Cs->FontSize, which the outer
       * cascade then dereferenced as a live calc() expression and
       * crashed on any element styled with a keyword font-size
       * (e.g. UA defaults for <sup>/<sub>). Use PxLen() which
       * initialises every field. */
      static const f32 AbsSizes[] = {
        0.6f, 0.75f, 0.89f, 1.0f, 1.2f, 1.5f, 2.0f   /* xx-small .. xx-large */
      };
      switch (K) {
        case DFBR_CSS_KW_XX_SMALL: Cs->FontSize = PxLen (16.0f * AbsSizes[0]); return;
        case DFBR_CSS_KW_X_SMALL:  Cs->FontSize = PxLen (16.0f * AbsSizes[1]); return;
        case DFBR_CSS_KW_SMALL:    Cs->FontSize = PxLen (16.0f * AbsSizes[2]); return;
        case DFBR_CSS_KW_MEDIUM:   Cs->FontSize = PxLen (16.0f * AbsSizes[3]); return;
        case DFBR_CSS_KW_LARGE:    Cs->FontSize = PxLen (16.0f * AbsSizes[4]); return;
        case DFBR_CSS_KW_X_LARGE:  Cs->FontSize = PxLen (16.0f * AbsSizes[5]); return;
        case DFBR_CSS_KW_XX_LARGE: Cs->FontSize = PxLen (16.0f * AbsSizes[6]); return;
        case DFBR_CSS_KW_LARGER:
        case DFBR_CSS_KW_SMALLER: {
          f32 Base = (Parent != DFBR_NULL) ? Parent->FontSize.Value : 16.0f;
          Cs->FontSize = PxLen ((K == DFBR_CSS_KW_LARGER) ? Base * 1.2f
                                                          : Base / 1.2f);
          return;
        }
        default: break;
      }
      DfBrCssCvAsLength (Cv, &Cs->FontSize);
      return;
    }
    case DFBR_CSS_PROP_FONT_WEIGHT: ApplyFontWeight (Cs, Cv); return;
    case DFBR_CSS_PROP_FONT_STYLE:  KwToFontStyle   (K, &Cs->FontStyle); return;
    case DFBR_CSS_PROP_FONT_FAMILY:
      ApplyFontFamily (Sc, Cs, Cv);
      return;
    case DFBR_CSS_PROP_LINE_HEIGHT: ApplyLineHeight (Cs, Cv); return;
    case DFBR_CSS_PROP_FONT:
      ApplyFontShorthand (Sc, Cs, Parent, Cv);
      return;

    case DFBR_CSS_PROP_TEXT_ALIGN:      KwToTextAlign      (K, &Cs->TextAlign);     return;
    case DFBR_CSS_PROP_TEXT_DECORATION: ApplyTextDecoration (Cs, Cv);               return;
    case DFBR_CSS_PROP_TEXT_TRANSFORM:  KwToTextTransform  (K, &Cs->TextTransform); return;
    case DFBR_CSS_PROP_TEXT_INDENT:     DfBrCssCvAsLength  (Cv, &Cs->TextIndent);   return;
    case DFBR_CSS_PROP_LETTER_SPACING:  DfBrCssCvAsLength  (Cv, &Cs->LetterSpacing); return;
    case DFBR_CSS_PROP_WORD_SPACING:    DfBrCssCvAsLength  (Cv, &Cs->WordSpacing);   return;
    case DFBR_CSS_PROP_WHITE_SPACE:     KwToWhiteSpace     (K, &Cs->WhiteSpace);    return;
    case DFBR_CSS_PROP_VERTICAL_ALIGN: {
      DfBrCssLength L;
      switch (K) {
        case DFBR_CSS_KW_BASELINE:    Cs->VerticalAlign = DFBR_CSS_VALIGN_BASELINE; return;
        case DFBR_CSS_KW_SUB:         Cs->VerticalAlign = DFBR_CSS_VALIGN_SUB;      return;
        case DFBR_CSS_KW_SUPER:       Cs->VerticalAlign = DFBR_CSS_VALIGN_SUPER;    return;
        case DFBR_CSS_KW_TOP:         Cs->VerticalAlign = DFBR_CSS_VALIGN_TOP;      return;
        case DFBR_CSS_KW_MIDDLE:      Cs->VerticalAlign = DFBR_CSS_VALIGN_MIDDLE;   return;
        case DFBR_CSS_KW_BOTTOM:      Cs->VerticalAlign = DFBR_CSS_VALIGN_BOTTOM;   return;
        case DFBR_CSS_KW_TEXT_TOP:    Cs->VerticalAlign = DFBR_CSS_VALIGN_TEXT_TOP; return;
        case DFBR_CSS_KW_TEXT_BOTTOM: Cs->VerticalAlign = DFBR_CSS_VALIGN_TEXT_BOTTOM; return;
        default:
          if (DfBrCssCvAsLength (Cv, &L)) {
            Cs->VerticalAlign = DFBR_CSS_VALIGN_LENGTH;
            Cs->VerticalAlignLength = L;
          }
          return;
      }
    }

    case DFBR_CSS_PROP_LIST_STYLE_TYPE: KwToListStyle (K, &Cs->ListStyleType); return;
    case DFBR_CSS_PROP_LIST_STYLE: {
      /* list-style: [type] [position] [image] - space separated,
       * any order; image is url(), position is inside|outside. */
      const DfBrCssCv *PCv = SkipWs (Cv);
      while (PCv != DFBR_NULL) {
        DfBrCssKeyword   KK = CvAsKeyword (PCv);
        DfBrCssListStyleTypeK LT;
        if (KwToListStyle (KK, &LT)) {
          Cs->ListStyleType = LT;
        } else if (KK == DFBR_CSS_KW_NONE) {
          Cs->ListStyleType = DFBR_CSS_LIST_NONE;
        }
        /* position / image are not modelled; just skip. */
        PCv = SkipWs (PCv->Next);
      }
      return;
    }

    case DFBR_CSS_PROP_FLEX: {
      /* flex: none | [flex-grow flex-shrink? || flex-basis] */
      const DfBrCssCv *PCv = SkipWs (Cv);
      DfBrCssKeyword   KK  = (PCv != DFBR_NULL) ? CvAsKeyword (PCv) : DFBR_CSS_KW_UNKNOWN;
      f32              N;
      DfBrCssLength    L;

      if (KK == DFBR_CSS_KW_NONE) {
        Cs->FlexGrow = 0.0f; Cs->FlexShrink = 0.0f; Cs->FlexBasis = AutoLen ();
        return;
      }
      if (KK == DFBR_CSS_KW_AUTO) {
        Cs->FlexGrow = 1.0f; Cs->FlexShrink = 1.0f; Cs->FlexBasis = AutoLen ();
        return;
      }
      /* Default per spec: `flex: <number>` => grow=number, shrink=1, basis=0. */
      {
        b32 GotGrow = DFBR_FALSE, GotShrink = DFBR_FALSE, GotBasis = DFBR_FALSE;
        /* Seed sensible initials for the one/two-value shortcuts. */
        f32 Grow = 1.0f, Shrink = 1.0f;
        DfBrCssLength Basis = ZeroLen ();
        while (PCv != DFBR_NULL) {
          if (!GotBasis && DfBrCssCvIsAuto (PCv)) {
            Basis = AutoLen (); GotBasis = DFBR_TRUE;
          } else if (!GotBasis && PCv->Kind == DfBrCssCvKindDimension
                                && DfBrCssCvAsLength (PCv, &L)) {
            Basis = L; GotBasis = DFBR_TRUE;
          } else if (!GotBasis && PCv->Kind == DfBrCssCvKindPercentage
                                && DfBrCssCvAsLength (PCv, &L)) {
            Basis = L; GotBasis = DFBR_TRUE;
          } else if (PCv->Kind == DfBrCssCvKindNumber && CvAsNumber (PCv, &N)) {
            if (!GotGrow) { Grow = N; GotGrow = DFBR_TRUE; }
            else if (!GotShrink) { Shrink = N; GotShrink = DFBR_TRUE; }
            else break;
          } else {
            break;
          }
          PCv = SkipWs (PCv->Next);
        }
        if (GotGrow && !GotBasis) Basis = ZeroLen ();
        Cs->FlexGrow   = Grow;
        Cs->FlexShrink = Shrink;
        Cs->FlexBasis  = Basis;
      }
      return;
    }

    case DFBR_CSS_PROP_FLEX_FLOW: {
      /* flex-flow: [direction] || [wrap] */
      const DfBrCssCv *PCv = SkipWs (Cv);
      while (PCv != DFBR_NULL) {
        DfBrCssKeyword KK = CvAsKeyword (PCv);
        DfBrCssFlexDirection FD;
        DfBrCssFlexWrap      FW;
        if (KwToFlexDir (KK, &FD))      Cs->FlexDirection = FD;
        else if (KwToFlexWrap (KK, &FW)) Cs->FlexWrap      = FW;
        PCv = SkipWs (PCv->Next);
      }
      return;
    }

    case DFBR_CSS_PROP_OUTLINE: {
      /* outline: <outline-width> || <outline-style> || <outline-color>
       * We don't yet paint outlines but parse so the rule doesn't
       * leak through to unknown-property dead code. */
      return;
    }

    case DFBR_CSS_PROP_FLEX_DIRECTION: KwToFlexDir   (K, &Cs->FlexDirection);  return;
    case DFBR_CSS_PROP_FLEX_WRAP:      KwToFlexWrap  (K, &Cs->FlexWrap);       return;
    case DFBR_CSS_PROP_FLEX_GROW: {
      f32 N;
      if (CvAsNumber (Cv, &N)) Cs->FlexGrow = N;
      return;
    }
    case DFBR_CSS_PROP_FLEX_SHRINK: {
      f32 N;
      if (CvAsNumber (Cv, &N)) Cs->FlexShrink = N;
      return;
    }
    case DFBR_CSS_PROP_FLEX_BASIS:
      if (DfBrCssCvIsAuto (Cv)) Cs->FlexBasis = AutoLen ();
      else DfBrCssCvAsLength (Cv, &Cs->FlexBasis);
      return;
    case DFBR_CSS_PROP_JUSTIFY_CONTENT: KwToAlign (K, &Cs->JustifyContent); return;
    case DFBR_CSS_PROP_ALIGN_ITEMS:     KwToAlign (K, &Cs->AlignItems);     return;
    case DFBR_CSS_PROP_ALIGN_SELF:      KwToAlign (K, &Cs->AlignSelf);      return;
    case DFBR_CSS_PROP_ALIGN_CONTENT:   KwToAlign (K, &Cs->AlignContent);   return;
    case DFBR_CSS_PROP_ROW_GAP:         DfBrCssCvAsLength (Cv, &Cs->RowGap); return;
    case DFBR_CSS_PROP_COLUMN_GAP:      DfBrCssCvAsLength (Cv, &Cs->ColumnGap); return;
    case DFBR_CSS_PROP_GAP: {
      DfBrCssLength L1, L2;
      const DfBrCssCv *C0 = SkipWs (Cv);
      const DfBrCssCv *C1 = (C0 != DFBR_NULL) ? SkipWs (C0->Next) : DFBR_NULL;
      if (C0 != DFBR_NULL && DfBrCssCvAsLength (C0, &L1)) {
        Cs->RowGap    = L1;
        Cs->ColumnGap = (C1 != DFBR_NULL && DfBrCssCvAsLength (C1, &L2)) ? L2 : L1;
      }
      return;
    }
    case DFBR_CSS_PROP_ORDER: {
      f32 N;
      if (CvAsNumber (Cv, &N)) Cs->Order = (i32)N;
      return;
    }

    case DFBR_CSS_PROP_BOX_SHADOW: {
      /* CSS UI §7.1 `box-shadow: none | <shadow># `
       *   <shadow> = [inset? <length>{2,4} <color>?] (in any order
       *   with color and inset floating before/after the lengths).
       *
       * Build a linked list of DfBrCssShadow on the style arena,
       * preserving author order. `none` clears any previously
       * cascaded value. */
      DfBrCssShadow *Head = DFBR_NULL, *Tail = DFBR_NULL;
      const DfBrCssCv *P   = SkipWs (Cv);
      if (K == DFBR_CSS_KW_NONE || P == DFBR_NULL) {
        Cs->BoxShadow = DFBR_NULL;
        return;
      }
      if (Sc == DFBR_NULL) return;
      while (P != DFBR_NULL) {
        DfBrCssLength Lens[4];
        usize         LenCount   = 0;
        DfBrCssColor  Col;
        b32           HaveColor  = DFBR_FALSE;
        b32           ColorIsCur = DFBR_FALSE;
        b32           Inset      = DFBR_FALSE;
        b32           ParseOk    = DFBR_FALSE;
        DfBrCssShadow *S;

        while (P != DFBR_NULL && P->Kind != DfBrCssCvKindComma) {
          DfBrCssLength L;
          const DfBrCssCv *Tail2 = DFBR_NULL;
          if (P->Kind == DfBrCssCvKindWhitespace) { P = P->Next; continue; }
          if (P->Kind == DfBrCssCvKindIdent &&
              DfBrCssAsciiEqZ (P->Str, P->StrLen, "inset")) {
            Inset = DFBR_TRUE;
            P = SkipWs (P->Next);
            continue;
          }
          if (P->Kind == DfBrCssCvKindIdent &&
              DfBrCssAsciiEqZ (P->Str, P->StrLen, "currentcolor")) {
            ColorIsCur = DFBR_TRUE;
            HaveColor  = DFBR_TRUE;
            Col        = Cs->Color;
            P = SkipWs (P->Next);
            continue;
          }
          if (DfBrCssCvAsColor (P, &Col, &Tail2)) {
            HaveColor = DFBR_TRUE;
            P = SkipWs (Tail2);
            continue;
          }
          if (DfBrCssCvAsLength (P, &L)) {
            if (LenCount < 4) Lens[LenCount++] = L;
            P = SkipWs (P->Next);
            continue;
          }
          /* Unknown token inside a shadow layer - bail on this
           * layer, but keep any successfully parsed earlier layers. */
          P = SkipWs (P->Next);
        }

        ParseOk = (LenCount >= 2);
        if (ParseOk) {
          S = (DfBrCssShadow *)DfBrCssArenaZero (&Sc->Arena, sizeof (*S));
          if (S == DFBR_NULL) break;
          S->OffsetX = Lens[0];
          S->OffsetY = Lens[1];
          S->Blur    = (LenCount >= 3) ? Lens[2] : ZeroLen ();
          S->Spread  = (LenCount >= 4) ? Lens[3] : ZeroLen ();
          S->Inset   = Inset;
          if (HaveColor) {
            S->Color          = Col;
            S->ColorIsCurrent = ColorIsCur;
          } else {
            /* Per spec the default is `currentColor`. */
            S->Color          = Cs->Color;
            S->ColorIsCurrent = DFBR_TRUE;
          }
          if (Head == DFBR_NULL) Head = S;
          else                   Tail->Next = S;
          Tail = S;
        }
        if (P == DFBR_NULL) break;
        if (P->Kind == DfBrCssCvKindComma) P = SkipWs (P->Next);
      }
      Cs->BoxShadow = Head;
      return;
    }

    default:
      /* Unknown / unsupported-in-v1; preserved on the rule tree for
       * debugging but ignored by the computed style. */
      return;
  }
}

void
DfBrCssApplyDeclaration (
  DfBrCssStyleContext        *Sc,
  DfBrCssComputedStyle       *Cs,
  const DfBrCssComputedStyle *Parent,
  const DfBrCssDeclaration   *Decl
  )
{
  const DfBrCssCv *Value;
  DfBrCssCv       *Resolved = DFBR_NULL;

  if (Cs == DFBR_NULL || Decl == DFBR_NULL) return;

  /* CSS custom property declaration (`--foo: ...`). Store it on this
   * element's custom-prop chain so that descendants (and subsequent
   * declarations on this same element) can resolve `var(--foo)`. We
   * prepend, so lookups see the most-recent binding first. */
  if (Decl->NameLen >= 2 && Decl->Name != DFBR_NULL &&
      Decl->Name[0] == '-' && Decl->Name[1] == '-')
  {
    DfBrCssCustomProp *Cp;
    if (Sc == DFBR_NULL) return;
    Cp = (DfBrCssCustomProp *)DfBrCssArenaZero (&Sc->Arena, sizeof (*Cp));
    if (Cp == DFBR_NULL) return;
    Cp->Name    = Decl->Name;
    Cp->NameLen = Decl->NameLen;
    Cp->Value   = Decl->Value;
    Cp->Next    = Cs->CustomProps;
    Cs->CustomProps = Cp;
    return;
  }

  if (Decl->Prop == DFBR_CSS_PROP_UNKNOWN) return;

  Value = Decl->Value;

  /* Only pay the cost of var() substitution when the value actually
   * references a custom property. A quick linear scan for a Function CV
   * named "var" avoids cloning every declaration on every element.
   * CRITICAL: scan recursively into function / block children too, so
   * values like `linear-gradient(var(--accent), ...)` or
   * `rgba(var(--r), ...)` trigger var() substitution. Before this
   * fix the top-level scan missed nested var()s and the gradient
   * parser saw raw var() Function CVs, silently failing. */
  {
    b32 NeedsResolve = ScanForVarRef (Value);
    if (NeedsResolve && Sc != DFBR_NULL) {
      Resolved = DfBrCssResolveVars (&Sc->Arena, Cs, Value);
      if (Resolved == DFBR_NULL) {
        /* Invalid-at-computed-value-time: drop the declaration
         * entirely per the CSS Variables spec. */
        return;
      }
      Value = Resolved;
    }
  }

  ApplySimple (Sc, Cs, Parent, Decl->Prop, Value);
}
