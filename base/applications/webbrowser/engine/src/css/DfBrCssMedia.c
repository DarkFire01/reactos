/** @file
  DfBrCssMedia.c - evaluate @media / @import / <link media="..."> query
  lists against the active viewport.

  This is intentionally a small subset of the Media Queries Level 4
  spec, aimed at the shapes real-world stylesheets use today:

    @media (min-width: 768px)                             -> yes
    @media (max-width: 768px)                             -> yes
    @media (min-width: 768px) and (max-width: 1199px)     -> yes
    @media screen and (max-width: 600px)                  -> yes
    @media only screen and (min-width: 768px)             -> yes
    @media (orientation: landscape)                       -> yes
    @media all                                            -> yes
    @media print                                          -> no (we're not print)
    @media (hover: hover), (min-width: 768px)             -> list, OR of parts
    @media not screen                                     -> negated media type

  A query list is evaluated as the OR of its comma-separated parts.
  Unknown features (e.g. `(prefers-color-scheme: dark)`) evaluate to
  false, per spec - but we only reject the single query part, never
  the whole list, so neighbouring parts can still match.

  An entirely empty prelude (`@media { ... }`) is treated as matching
  `all` - Firefox/Blink do the same in practice.
**/

#include "DfBrCssInternal.h"
#include "DfBrCss.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------
 *  CV list helpers (skip whitespace tokens, split on comma)
 * -----------------------------------------------------------------*/

static const DfBrCssCv *
SkipWs (const DfBrCssCv *C) {
  while (C != DFBR_NULL && C->Kind == DfBrCssCvKindWhitespace) C = C->Next;
  /* Query parts are comma-separated; individual part walkers must
   * never cross a comma. Returning NULL at a comma boundary is how we
   * isolate one part of a media query list from the next. */
  if (C != DFBR_NULL && C->Kind == DfBrCssCvKindComma) return DFBR_NULL;
  return C;
}

static b32
IsIdentEq (const DfBrCssCv *C, const char *LitZ) {
  if (C == DFBR_NULL || C->Kind != DfBrCssCvKindIdent) return DFBR_FALSE;
  return DfBrCssAsciiEqZ (C->Str, C->StrLen, LitZ);
}

static b32
IsComma (const DfBrCssCv *C) {
  return C != DFBR_NULL && C->Kind == DfBrCssCvKindComma;
}

/* ------------------------------------------------------------------
 *  Unit conversion to CSS pixels
 *
 *  We evaluate media features before any element has a computed
 *  font size, so font-relative units use the default 16px root. That
 *  matches what WebKit/Blink do for the initial media query match.
 * -----------------------------------------------------------------*/

static b32
ResolveLengthPx (const DfBrCssCv *C, f32 *OutPx) {
  f32 V;
  if (C == DFBR_NULL) return DFBR_FALSE;
  V = (f32)C->Number;

  if (C->Kind == DfBrCssCvKindNumber) {
    /* Zero is the only unit-less length accepted in media queries. */
    if (V == 0.0f) { *OutPx = 0.0f; return DFBR_TRUE; }
    return DFBR_FALSE;
  }
  if (C->Kind != DfBrCssCvKindDimension) return DFBR_FALSE;

  switch (C->UnitId) {
    case DFBR_CSS_UNIT_PX:   *OutPx = V;                    return DFBR_TRUE;
    case DFBR_CSS_UNIT_PT:   *OutPx = V * (96.0f / 72.0f);  return DFBR_TRUE;
    case DFBR_CSS_UNIT_PC:   *OutPx = V * 16.0f;            return DFBR_TRUE;
    case DFBR_CSS_UNIT_IN:   *OutPx = V * 96.0f;            return DFBR_TRUE;
    case DFBR_CSS_UNIT_CM:   *OutPx = V * (96.0f / 2.54f);  return DFBR_TRUE;
    case DFBR_CSS_UNIT_MM:   *OutPx = V * (96.0f / 25.4f);  return DFBR_TRUE;
    case DFBR_CSS_UNIT_Q:    *OutPx = V * (96.0f / 101.6f); return DFBR_TRUE;
    case DFBR_CSS_UNIT_EM:
    case DFBR_CSS_UNIT_REM:  *OutPx = V * 16.0f;            return DFBR_TRUE;
    default: return DFBR_FALSE;
  }
}

/* ------------------------------------------------------------------
 *  Feature evaluation
 *
 *  `Inside` points at a CV list that was inside a `(...)` block - i.e.
 *  the Children pointer of a DfBrCssCvKindBlock with BlockKind 0. A
 *  feature is either a single ident ("(hover)") or "ident: value".
 * -----------------------------------------------------------------*/

static b32
EvalFeature (const DfBrCssCv *Inside, const DfBrCssMatchContext *Ctx, b32 *OutKnown) {
  const DfBrCssCv *Name;
  const DfBrCssCv *Val;
  b32              HasMin   = DFBR_FALSE;
  b32              HasMax   = DFBR_FALSE;
  usize            NameLen;
  const char      *NameStr;

  *OutKnown = DFBR_FALSE;

  Name = SkipWs (Inside);
  if (Name == DFBR_NULL || Name->Kind != DfBrCssCvKindIdent) {
    return DFBR_FALSE;
  }

  NameStr = Name->Str;
  NameLen = Name->StrLen;
  if (NameLen > 4 && NameStr[0] == 'm' && NameStr[1] == 'i'
      && NameStr[2] == 'n' && NameStr[3] == '-')
  { HasMin = DFBR_TRUE; NameStr += 4; NameLen -= 4; }
  else if (NameLen > 4 && NameStr[0] == 'm' && NameStr[1] == 'a'
           && NameStr[2] == 'x' && NameStr[3] == '-')
  { HasMax = DFBR_TRUE; NameStr += 4; NameLen -= 4; }

  /* After the ident we either have EOF (bare feature) or a colon
   * followed by the value list. */
  Val = SkipWs (Name->Next);
  if (Val == DFBR_NULL) {
    /* Bare feature. Treat known boolean-ish features as true so stuff
     * like `(hover)` or `(color)` matches. */
    if (DfBrCssAsciiEqZ (NameStr, NameLen, "hover")
        || DfBrCssAsciiEqZ (NameStr, NameLen, "any-hover")
        || DfBrCssAsciiEqZ (NameStr, NameLen, "pointer")
        || DfBrCssAsciiEqZ (NameStr, NameLen, "any-pointer")
        || DfBrCssAsciiEqZ (NameStr, NameLen, "color")
        || DfBrCssAsciiEqZ (NameStr, NameLen, "color-index")
        || DfBrCssAsciiEqZ (NameStr, NameLen, "grid")
        || DfBrCssAsciiEqZ (NameStr, NameLen, "monochrome")
        || DfBrCssAsciiEqZ (NameStr, NameLen, "width")
        || DfBrCssAsciiEqZ (NameStr, NameLen, "height")
        || DfBrCssAsciiEqZ (NameStr, NameLen, "aspect-ratio")
        || DfBrCssAsciiEqZ (NameStr, NameLen, "orientation")
        || DfBrCssAsciiEqZ (NameStr, NameLen, "resolution")
        || DfBrCssAsciiEqZ (NameStr, NameLen, "prefers-reduced-motion"))
    {
      *OutKnown = DFBR_TRUE;
      return DFBR_TRUE;
    }
    return DFBR_FALSE;
  }
  if (Val->Kind != DfBrCssCvKindColon) return DFBR_FALSE;
  Val = SkipWs (Val->Next);
  if (Val == DFBR_NULL) return DFBR_FALSE;

  /* width / height (+ min-/max-) */
  if (DfBrCssAsciiEqZ (NameStr, NameLen, "width")
      || DfBrCssAsciiEqZ (NameStr, NameLen, "device-width"))
  {
    f32 Px;
    if (!ResolveLengthPx (Val, &Px)) return DFBR_FALSE;
    *OutKnown = DFBR_TRUE;
    if (Ctx->VpWidthPx <= 0.0f) return DFBR_TRUE;   /* unknown viewport */
    if (HasMin) return Ctx->VpWidthPx >= Px;
    if (HasMax) return Ctx->VpWidthPx <= Px;
    return Ctx->VpWidthPx == Px;
  }
  if (DfBrCssAsciiEqZ (NameStr, NameLen, "height")
      || DfBrCssAsciiEqZ (NameStr, NameLen, "device-height"))
  {
    f32 Px;
    if (!ResolveLengthPx (Val, &Px)) return DFBR_FALSE;
    *OutKnown = DFBR_TRUE;
    if (Ctx->VpHeightPx <= 0.0f) return DFBR_TRUE;
    if (HasMin) return Ctx->VpHeightPx >= Px;
    if (HasMax) return Ctx->VpHeightPx <= Px;
    return Ctx->VpHeightPx == Px;
  }

  /* orientation: portrait | landscape */
  if (DfBrCssAsciiEqZ (NameStr, NameLen, "orientation")) {
    if (Val->Kind != DfBrCssCvKindIdent) return DFBR_FALSE;
    *OutKnown = DFBR_TRUE;
    if (Ctx->VpWidthPx <= 0.0f || Ctx->VpHeightPx <= 0.0f) return DFBR_TRUE;
    if (DfBrCssAsciiEqZ (Val->Str, Val->StrLen, "landscape")) {
      return Ctx->VpWidthPx >= Ctx->VpHeightPx;
    }
    if (DfBrCssAsciiEqZ (Val->Str, Val->StrLen, "portrait")) {
      return Ctx->VpWidthPx <  Ctx->VpHeightPx;
    }
    return DFBR_FALSE;
  }

  /* hover / pointer / color-scheme / etc: pretend we're a desktop with
   * fine pointer, hover, color, no reduced motion. */
  if (DfBrCssAsciiEqZ (NameStr, NameLen, "hover")
      || DfBrCssAsciiEqZ (NameStr, NameLen, "any-hover"))
  {
    if (Val->Kind != DfBrCssCvKindIdent) return DFBR_FALSE;
    *OutKnown = DFBR_TRUE;
    return DfBrCssAsciiEqZ (Val->Str, Val->StrLen, "hover");
  }
  if (DfBrCssAsciiEqZ (NameStr, NameLen, "pointer")
      || DfBrCssAsciiEqZ (NameStr, NameLen, "any-pointer"))
  {
    if (Val->Kind != DfBrCssCvKindIdent) return DFBR_FALSE;
    *OutKnown = DFBR_TRUE;
    return DfBrCssAsciiEqZ (Val->Str, Val->StrLen, "fine");
  }
  if (DfBrCssAsciiEqZ (NameStr, NameLen, "prefers-color-scheme")) {
    *OutKnown = DFBR_TRUE;
    if (Val->Kind != DfBrCssCvKindIdent) return DFBR_FALSE;
    return DfBrCssAsciiEqZ (Val->Str, Val->StrLen, "light");
  }
  if (DfBrCssAsciiEqZ (NameStr, NameLen, "prefers-reduced-motion")) {
    *OutKnown = DFBR_TRUE;
    if (Val->Kind != DfBrCssCvKindIdent) return DFBR_FALSE;
    return DfBrCssAsciiEqZ (Val->Str, Val->StrLen, "no-preference");
  }

  /* Unknown feature: don't claim to know; caller treats as non-match. */
  return DFBR_FALSE;
}

/* ------------------------------------------------------------------
 *  Query part evaluation
 *
 *  A single query (between commas) is:
 *    [ not | only ]?  <media-type>?  ( 'and' <feature> )*
 *    | <feature> ( 'and' <feature> )*
 *
 *  Media types: all / screen / print / speech / tv / projection / ...
 *  We treat 'all' and 'screen' as matching, everything else as non-
 *  matching. 'only' is a legacy HTML4 guard and is ignored.
 * -----------------------------------------------------------------*/

static b32
MediaTypeMatches (const DfBrCssCv *Tok) {
  if (Tok == DFBR_NULL || Tok->Kind != DfBrCssCvKindIdent) return DFBR_FALSE;
  if (DfBrCssAsciiEqZ (Tok->Str, Tok->StrLen, "all"))    return DFBR_TRUE;
  if (DfBrCssAsciiEqZ (Tok->Str, Tok->StrLen, "screen")) return DFBR_TRUE;
  return DFBR_FALSE;
}

static b32
EvalPart (const DfBrCssCv *Part, const DfBrCssMatchContext *Ctx) {
  const DfBrCssCv *C      = SkipWs (Part);
  b32              Negate = DFBR_FALSE;
  b32              Typed  = DFBR_FALSE;
  b32              TypeOk = DFBR_TRUE;
  b32              Result = DFBR_TRUE;

  if (C == DFBR_NULL) return DFBR_TRUE;  /* empty media list matches */

  if (IsIdentEq (C, "not"))  { Negate = DFBR_TRUE;  C = SkipWs (C->Next); }
  else if (IsIdentEq (C, "only")) { C = SkipWs (C->Next); }

  /* Optional media type. */
  if (C != DFBR_NULL && C->Kind == DfBrCssCvKindIdent && !IsIdentEq (C, "and")) {
    TypeOk = MediaTypeMatches (C);
    Typed  = DFBR_TRUE;
    C      = SkipWs (C->Next);
    if (C != DFBR_NULL) {
      if (!IsIdentEq (C, "and")) {
        /* garbage after type - fail this part */
        b32 R = TypeOk;
        return Negate ? !R : R;
      }
      C = SkipWs (C->Next);
    }
  }

  /* At this point C is either null (just a media type) or an
   * "(feature) and (feature) and ..." chain. */
  if (C == DFBR_NULL) {
    b32 R = Typed ? TypeOk : DFBR_TRUE;
    return Negate ? !R : R;
  }

  while (C != DFBR_NULL) {
    b32 Known   = DFBR_FALSE;
    b32 FeatureOk;
    /* "(min-width: 768px)" is parsed as an anonymous-paren Function CV
     * (empty name, arg list in Children). The old parser emitted a
     * Block for bracket-groups, so we accept both to be forgiving. */
    if ((C->Kind != DfBrCssCvKindFunction || C->StrLen != 0)
        && C->Kind != DfBrCssCvKindBlock)
    {
      return Negate ? DFBR_TRUE : DFBR_FALSE;
    }
    FeatureOk = EvalFeature (C->Children, Ctx, &Known);
    if (!Known) Result = DFBR_FALSE;
    if (!FeatureOk) Result = DFBR_FALSE;
    C = SkipWs (C->Next);
    if (C == DFBR_NULL) break;
    if (!IsIdentEq (C, "and")) {
      /* unrecognised connector - treat the whole part as failing */
      Result = DFBR_FALSE;
      break;
    }
    C = SkipWs (C->Next);
  }

  if (Typed && !TypeOk) Result = DFBR_FALSE;
  return Negate ? !Result : Result;
}

/* ------------------------------------------------------------------
 *  Public entry
 * -----------------------------------------------------------------*/

b32
DfBrCssMediaQueryMatches (const DfBrCssCv *Prelude, const DfBrCssMatchContext *Ctx) {
  const DfBrCssCv *C = SkipWs (Prelude);
  const DfBrCssCv *Start;

  if (C == DFBR_NULL) return DFBR_TRUE;    /* @media { ... } -> matches all */

  Start = C;
  for (;;) {
    /* Walk to next comma at the top level; everything in [Start, C)
     * is one query part. */
    const DfBrCssCv *P = Start;
    const DfBrCssCv *Cursor = Start;
    /* Build a private linked copy? No - EvalPart stops on NULL Next or
     * comma. We isolate a part by temporarily pretending the CV list
     * ends at the comma. Since we can't mutate the shared list, we do
     * the walk inline: EvalPart reads .Next, so we pass Start and rely
     * on EvalPart stopping at the comma. Tweak EvalPart to stop at
     * commas. (See below.) */
    (void)P; (void)Cursor;

    if (EvalPart (Start, Ctx)) return DFBR_TRUE;

    /* Advance past this part */
    while (C != DFBR_NULL && !IsComma (C)) C = C->Next;
    if (C == DFBR_NULL) return DFBR_FALSE;
    C = SkipWs (C->Next);
    if (C == DFBR_NULL) return DFBR_FALSE;
    Start = C;
  }
}

/* ------------------------------------------------------------------
 *  Public: evaluate an HTML <link>/<style> media="..." attribute
 *
 *  HTML resolves `media` as an "applicable media" test: empty string
 *  (or omitted) matches everything, while a non-empty string is a
 *  Media Queries Level 4 list evaluated against the current
 *  environment. We piggyback on the @media parser by synthesizing a
 *  throwaway `@media <attr> {}` block and feeding it through
 *  DfBrCssParse - this keeps there being exactly one grammar
 *  implementation to maintain. Parse failures fall back to "matches"
 *  so a bogus media attribute never silently drops a stylesheet.
 * -----------------------------------------------------------------*/

b32
DfBrCssMediaAttrMatches (const char *MediaAttr,
                         usize       MediaAttrLen,
                         f32         VpWidthPx,
                         f32         VpHeightPx) {
  DfBrCssMatchContext  Ctx;
  DfBrCssStylesheet   *Wrapper;
  const DfBrCssRule   *R;
  const DfBrCssCv     *Prelude;
  const char          *Name;
  usize                NameLen;
  char                *Buf;
  usize                BufLen;
  usize                I;
  b32                  AllWs;
  b32                  Result;

  if (MediaAttr == DFBR_NULL || MediaAttrLen == 0) return DFBR_TRUE;

  /* Trim: all-whitespace attribute behaves like the absent attr. */
  AllWs = DFBR_TRUE;
  for (I = 0; I < MediaAttrLen; I++) {
    char C = MediaAttr[I];
    if (C != ' ' && C != '\t' && C != '\n' && C != '\r' && C != '\f') {
      AllWs = DFBR_FALSE; break;
    }
  }
  if (AllWs) return DFBR_TRUE;

  /* Guard against pathological inputs so we never heap-bomb on junk
   * author markup (spec says the attr can be any length but 64 KiB
   * is already absurd for a real media list). */
  if (MediaAttrLen > 65536) return DFBR_TRUE;

  /* "@media " + attr + " {}" */
  BufLen = 7 + MediaAttrLen + 3;
  Buf = (char *)malloc (BufLen + 1);
  if (Buf == DFBR_NULL) return DFBR_TRUE;
  memcpy (Buf, "@media ", 7);
  memcpy (Buf + 7, MediaAttr, MediaAttrLen);
  memcpy (Buf + 7 + MediaAttrLen, " {}", 3);
  Buf[BufLen] = '\0';

  Wrapper = DfBrCssParse (Buf, BufLen, DFBR_NULL);
  free (Buf);
  if (Wrapper == DFBR_NULL) return DFBR_TRUE;

  R = DfBrCssStylesheetFirstRule (Wrapper);
  if (R == DFBR_NULL || DfBrCssRuleType (R) != DFBR_CSS_RULE_AT) {
    DfBrCssStylesheetDestroy (Wrapper);
    return DFBR_TRUE;
  }
  Name = DfBrCssRuleAtName (R, &NameLen);
  if (Name == DFBR_NULL || NameLen != 5 || memcmp (Name, "media", 5) != 0) {
    DfBrCssStylesheetDestroy (Wrapper);
    return DFBR_TRUE;
  }

  Prelude = DfBrCssRuleAtPrelude (R);

  Ctx.Root       = DFBR_NULL;
  Ctx.VpWidthPx  = VpWidthPx;
  Ctx.VpHeightPx = VpHeightPx;

  Result = DfBrCssMediaQueryMatches (Prelude, &Ctx);
  DfBrCssStylesheetDestroy (Wrapper);
  return Result;
}
