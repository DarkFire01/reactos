/** @file
  DfBrCssMatch.c - selector -> DfBrHtmlNode matcher.

  The matcher walks a complex selector right-to-left: the subject of
  the selector is the rightmost compound, and earlier compounds are
  tested against ancestors / siblings according to the combinator
  between them. This is the algorithm every production engine uses;
  it minimises work because most selectors have a restrictive
  rightmost compound that prunes candidates fast.
**/

#include "DfBrCssInternal.h"

#include <string.h>

/* ------------------------------------------------------------------
 *  ASCII case-insensitive string utilities used only here
 * -----------------------------------------------------------------*/

static b32 EqIgnoreCase (const char *A, usize ALen, const char *B, usize BLen) {
  usize i;
  if (ALen != BLen) return DFBR_FALSE;
  for (i = 0; i < ALen; i++) {
    char ca = A[i];
    char cb = B[i];
    if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
    if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
    if (ca != cb) return DFBR_FALSE;
  }
  return DFBR_TRUE;
}

static b32 EqCase (const char *A, usize ALen, const char *B, usize BLen) {
  return (ALen == BLen && (ALen == 0 || memcmp (A, B, ALen) == 0))
         ? DFBR_TRUE : DFBR_FALSE;
}

/* ------------------------------------------------------------------
 *  Attribute match
 * -----------------------------------------------------------------*/

static b32 AttrMatchIncludes (const char *Haystack, usize HLen,
                              const char *Needle,   usize NLen,
                              b32 IgnoreCase) {
  usize  i = 0;
  usize  TokStart;

  if (NLen == 0) return DFBR_FALSE;
  while (i < HLen) {
    /* Skip leading whitespace. */
    while (i < HLen && (Haystack[i] == ' ' || Haystack[i] == '\t' ||
                        Haystack[i] == '\n' || Haystack[i] == '\f' ||
                        Haystack[i] == '\r')) i++;
    TokStart = i;
    while (i < HLen && !(Haystack[i] == ' ' || Haystack[i] == '\t' ||
                         Haystack[i] == '\n' || Haystack[i] == '\f' ||
                         Haystack[i] == '\r')) i++;
    if (i - TokStart == NLen) {
      b32 Eq = IgnoreCase
               ? EqIgnoreCase (Haystack + TokStart, NLen, Needle, NLen)
               : EqCase       (Haystack + TokStart, NLen, Needle, NLen);
      if (Eq) return DFBR_TRUE;
    }
  }
  return DFBR_FALSE;
}

static b32 AttrMatchDash (const char *Haystack, usize HLen,
                          const char *Needle,   usize NLen,
                          b32 IgnoreCase) {
  b32 Eq;
  if (HLen < NLen) return DFBR_FALSE;
  Eq = IgnoreCase
       ? EqIgnoreCase (Haystack, NLen, Needle, NLen)
       : EqCase       (Haystack, NLen, Needle, NLen);
  if (!Eq) return DFBR_FALSE;
  if (HLen == NLen) return DFBR_TRUE;
  return (Haystack[NLen] == '-') ? DFBR_TRUE : DFBR_FALSE;
}

static b32 AttrMatchPrefix (const char *Haystack, usize HLen,
                            const char *Needle,   usize NLen,
                            b32 IgnoreCase) {
  if (NLen == 0 || HLen < NLen) return DFBR_FALSE;
  return IgnoreCase
         ? EqIgnoreCase (Haystack, NLen, Needle, NLen)
         : EqCase       (Haystack, NLen, Needle, NLen);
}

static b32 AttrMatchSuffix (const char *Haystack, usize HLen,
                            const char *Needle,   usize NLen,
                            b32 IgnoreCase) {
  if (NLen == 0 || HLen < NLen) return DFBR_FALSE;
  return IgnoreCase
         ? EqIgnoreCase (Haystack + (HLen - NLen), NLen, Needle, NLen)
         : EqCase       (Haystack + (HLen - NLen), NLen, Needle, NLen);
}

static b32 AttrMatchSubstring (const char *Haystack, usize HLen,
                               const char *Needle,   usize NLen,
                               b32 IgnoreCase) {
  usize i;
  if (NLen == 0 || HLen < NLen) return DFBR_FALSE;
  for (i = 0; i + NLen <= HLen; i++) {
    b32 Eq = IgnoreCase
             ? EqIgnoreCase (Haystack + i, NLen, Needle, NLen)
             : EqCase       (Haystack + i, NLen, Needle, NLen);
    if (Eq) return DFBR_TRUE;
  }
  return DFBR_FALSE;
}

/* ------------------------------------------------------------------
 *  :nth-child helpers
 * -----------------------------------------------------------------*/

static usize IndexAmongSiblings (
  const DfBrHtmlNode  *N,
  b32                  OfType,
  b32                  FromEnd
  )
{
  const DfBrHtmlNode *P;
  usize               Idx = 0;

  if (N == DFBR_NULL || N->Parent == DFBR_NULL) return 1;

  if (!FromEnd) {
    for (P = N->Parent->FirstChild; P != DFBR_NULL; P = P->NextSibling) {
      if (P->Kind != DFBR_HTML_NODE_ELEMENT) continue;
      if (OfType && P->Tag != N->Tag) continue;
      Idx++;
      if (P == N) return Idx;
    }
  } else {
    for (P = N->Parent->LastChild; P != DFBR_NULL; P = P->PrevSibling) {
      if (P->Kind != DFBR_HTML_NODE_ELEMENT) continue;
      if (OfType && P->Tag != N->Tag) continue;
      Idx++;
      if (P == N) return Idx;
    }
  }
  return 1;
}

static b32 NthMatch (i32 A, i32 B, usize Index) {
  /* Matches iff there exists non-negative n with An+B == Index. */
  i32  I = (i32)Index;
  i32  D;
  if (A == 0) return (I == B) ? DFBR_TRUE : DFBR_FALSE;
  D = I - B;
  if ((D > 0 && A > 0) || (D < 0 && A < 0)) {
    return (D % A == 0) ? DFBR_TRUE : DFBR_FALSE;
  }
  if (D == 0) return DFBR_TRUE;
  return DFBR_FALSE;
}

/* ------------------------------------------------------------------
 *  Individual simple-selector match
 * -----------------------------------------------------------------*/

static b32 MatchSimple (
  const DfBrCssSimpleSel   *S,
  const DfBrHtmlNode       *E,
  const DfBrCssMatchContext *Ctx
  );

/* For pseudo-class functional bodies (:not, :is, :has, :where). */
static b32 MatchSelectorListAny (
  const DfBrCssComplex     *First,
  const DfBrHtmlNode       *E,
  const DfBrCssMatchContext *Ctx
  );

static b32 MatchCompound (
  const DfBrCssCompound    *Cmp,
  const DfBrHtmlNode       *E,
  const DfBrCssMatchContext *Ctx
  )
{
  const DfBrCssSimpleSel *S;
  for (S = Cmp->Simples; S != DFBR_NULL; S = S->Next) {
    if (!MatchSimple (S, E, Ctx)) return DFBR_FALSE;
  }
  return DFBR_TRUE;
}

static b32 MatchSimple (
  const DfBrCssSimpleSel   *S,
  const DfBrHtmlNode       *E,
  const DfBrCssMatchContext *Ctx
  )
{
  switch (S->Kind) {
    case DfBrCssSelUniversal:
      return DFBR_TRUE;

    case DfBrCssSelType:
      /* Name is lowercase; element names are lowercase too. */
      return EqCase (E->Name, E->NameLen, S->Name, S->NameLen);

    case DfBrCssSelId: {
      const char *Id = DfBrHtmlNodeGetAttrValue (E, "id");
      if (Id == DFBR_NULL) return DFBR_FALSE;
      /* IDs are case-sensitive in HTML Standard mode. */
      return (strlen (Id) == S->NameLen && memcmp (Id, S->Name, S->NameLen) == 0)
             ? DFBR_TRUE : DFBR_FALSE;
    }

    case DfBrCssSelClass: {
      const char *Cls = DfBrHtmlNodeGetAttrValue (E, "class");
      if (Cls == DFBR_NULL) return DFBR_FALSE;
      return AttrMatchIncludes (Cls, strlen (Cls), S->Name, S->NameLen, DFBR_FALSE);
    }

    case DfBrCssSelAttr: {
      const DfBrHtmlAttr *A = DfBrHtmlNodeGetAttr (E, S->Name);
      const char         *V;
      usize               VLen;
      b32                 IgnoreCase = S->AttrIgnoreCase;

      if (A == DFBR_NULL) return DFBR_FALSE;
      if (S->AttrOp == DfBrCssAttrExists) return DFBR_TRUE;
      V    = A->Value;
      VLen = A->ValueLen;
      switch (S->AttrOp) {
        case DfBrCssAttrEq:
          return IgnoreCase
                 ? EqIgnoreCase (V, VLen, S->AttrValue, S->AttrValueLen)
                 : EqCase       (V, VLen, S->AttrValue, S->AttrValueLen);
        case DfBrCssAttrIncludes:
          return AttrMatchIncludes (V, VLen, S->AttrValue, S->AttrValueLen, IgnoreCase);
        case DfBrCssAttrDashMatch:
          return AttrMatchDash     (V, VLen, S->AttrValue, S->AttrValueLen, IgnoreCase);
        case DfBrCssAttrPrefix:
          return AttrMatchPrefix   (V, VLen, S->AttrValue, S->AttrValueLen, IgnoreCase);
        case DfBrCssAttrSuffix:
          return AttrMatchSuffix   (V, VLen, S->AttrValue, S->AttrValueLen, IgnoreCase);
        case DfBrCssAttrSubstring:
          return AttrMatchSubstring (V, VLen, S->AttrValue, S->AttrValueLen, IgnoreCase);
        default: return DFBR_FALSE;
      }
    }

    case DfBrCssSelPseudoClass:
      switch (S->Pc) {
        case DfBrCssPcRoot:
          /* :root matches the document element, which in HTML is <html>. */
          return (E->Tag == DFBR_HTML_TAG_HTML) ? DFBR_TRUE : DFBR_FALSE;

        case DfBrCssPcFirstChild: {
          const DfBrHtmlNode *P;
          if (E->Parent == DFBR_NULL) return DFBR_FALSE;
          for (P = E->Parent->FirstChild; P != DFBR_NULL; P = P->NextSibling) {
            if (P->Kind == DFBR_HTML_NODE_ELEMENT) return (P == E) ? DFBR_TRUE : DFBR_FALSE;
          }
          return DFBR_FALSE;
        }
        case DfBrCssPcLastChild: {
          const DfBrHtmlNode *P;
          if (E->Parent == DFBR_NULL) return DFBR_FALSE;
          for (P = E->Parent->LastChild; P != DFBR_NULL; P = P->PrevSibling) {
            if (P->Kind == DFBR_HTML_NODE_ELEMENT) return (P == E) ? DFBR_TRUE : DFBR_FALSE;
          }
          return DFBR_FALSE;
        }
        case DfBrCssPcOnlyChild: {
          const DfBrHtmlNode *P;
          usize               Count = 0;
          if (E->Parent == DFBR_NULL) return DFBR_FALSE;
          for (P = E->Parent->FirstChild; P != DFBR_NULL; P = P->NextSibling) {
            if (P->Kind == DFBR_HTML_NODE_ELEMENT) {
              Count++;
              if (Count > 1) return DFBR_FALSE;
            }
          }
          return (Count == 1) ? DFBR_TRUE : DFBR_FALSE;
        }
        case DfBrCssPcFirstOfType:
          return (IndexAmongSiblings (E, DFBR_TRUE, DFBR_FALSE) == 1) ? DFBR_TRUE : DFBR_FALSE;
        case DfBrCssPcLastOfType:
          return (IndexAmongSiblings (E, DFBR_TRUE, DFBR_TRUE) == 1) ? DFBR_TRUE : DFBR_FALSE;
        case DfBrCssPcOnlyOfType: {
          const DfBrHtmlNode *P;
          usize               Count = 0;
          if (E->Parent == DFBR_NULL) return DFBR_FALSE;
          for (P = E->Parent->FirstChild; P != DFBR_NULL; P = P->NextSibling) {
            if (P->Kind == DFBR_HTML_NODE_ELEMENT && P->Tag == E->Tag) {
              Count++;
              if (Count > 1) return DFBR_FALSE;
            }
          }
          return (Count == 1) ? DFBR_TRUE : DFBR_FALSE;
        }
        case DfBrCssPcNthChild: {
          if (!S->NthValid) return DFBR_FALSE;
          return NthMatch (S->NthA, S->NthB,
                           IndexAmongSiblings (E, DFBR_FALSE, DFBR_FALSE));
        }
        case DfBrCssPcNthLastChild: {
          if (!S->NthValid) return DFBR_FALSE;
          return NthMatch (S->NthA, S->NthB,
                           IndexAmongSiblings (E, DFBR_FALSE, DFBR_TRUE));
        }
        case DfBrCssPcNthOfType: {
          if (!S->NthValid) return DFBR_FALSE;
          return NthMatch (S->NthA, S->NthB,
                           IndexAmongSiblings (E, DFBR_TRUE, DFBR_FALSE));
        }
        case DfBrCssPcNthLastOfType: {
          if (!S->NthValid) return DFBR_FALSE;
          return NthMatch (S->NthA, S->NthB,
                           IndexAmongSiblings (E, DFBR_TRUE, DFBR_TRUE));
        }
        case DfBrCssPcEmpty: {
          const DfBrHtmlNode *P;
          for (P = E->FirstChild; P != DFBR_NULL; P = P->NextSibling) {
            if (P->Kind == DFBR_HTML_NODE_ELEMENT) return DFBR_FALSE;
            if (P->Kind == DFBR_HTML_NODE_TEXT && P->TextLen > 0) return DFBR_FALSE;
          }
          return DFBR_TRUE;
        }
        case DfBrCssPcLink:
        case DfBrCssPcAnyLink: {
          if (E->Tag == DFBR_HTML_TAG_A || E->Tag == DFBR_HTML_TAG_AREA) {
            return (DfBrHtmlNodeGetAttr (E, "href") != DFBR_NULL) ? DFBR_TRUE : DFBR_FALSE;
          }
          return DFBR_FALSE;
        }
        case DfBrCssPcVisited:
          return DFBR_FALSE; /* No history concept yet. */
        case DfBrCssPcDisabled:
          return (DfBrHtmlNodeGetAttr (E, "disabled") != DFBR_NULL) ? DFBR_TRUE : DFBR_FALSE;
        case DfBrCssPcEnabled:
          if (E->Tag != DFBR_HTML_TAG_INPUT && E->Tag != DFBR_HTML_TAG_BUTTON &&
              E->Tag != DFBR_HTML_TAG_SELECT && E->Tag != DFBR_HTML_TAG_TEXTAREA &&
              E->Tag != DFBR_HTML_TAG_OPTION) return DFBR_FALSE;
          return (DfBrHtmlNodeGetAttr (E, "disabled") == DFBR_NULL) ? DFBR_TRUE : DFBR_FALSE;
        case DfBrCssPcChecked:
          return (DfBrHtmlNodeGetAttr (E, "checked") != DFBR_NULL) ? DFBR_TRUE : DFBR_FALSE;
        case DfBrCssPcRequired:
          return (DfBrHtmlNodeGetAttr (E, "required") != DFBR_NULL) ? DFBR_TRUE : DFBR_FALSE;
        case DfBrCssPcOptional:
          if (E->Tag != DFBR_HTML_TAG_INPUT && E->Tag != DFBR_HTML_TAG_SELECT &&
              E->Tag != DFBR_HTML_TAG_TEXTAREA) return DFBR_FALSE;
          return (DfBrHtmlNodeGetAttr (E, "required") == DFBR_NULL) ? DFBR_TRUE : DFBR_FALSE;
        case DfBrCssPcReadOnly:
          return (DfBrHtmlNodeGetAttr (E, "readonly") != DFBR_NULL) ? DFBR_TRUE : DFBR_FALSE;
        case DfBrCssPcReadWrite:
          /* input / textarea with no readonly / disabled. */
          if (E->Tag != DFBR_HTML_TAG_INPUT && E->Tag != DFBR_HTML_TAG_TEXTAREA) return DFBR_FALSE;
          return (DfBrHtmlNodeGetAttr (E, "readonly") == DFBR_NULL &&
                  DfBrHtmlNodeGetAttr (E, "disabled") == DFBR_NULL) ? DFBR_TRUE : DFBR_FALSE;
        case DfBrCssPcHover:
        case DfBrCssPcFocus:
        case DfBrCssPcActive:
        case DfBrCssPcFocusWithin:
        case DfBrCssPcFocusVisible:
        case DfBrCssPcTarget:
        case DfBrCssPcPlaceholderShown:
          /* Dynamic - always false at static cascade time. */
          return DFBR_FALSE;
        case DfBrCssPcScope:
          /* With no :scope host, :scope matches :root. */
          return (E->Tag == DFBR_HTML_TAG_HTML) ? DFBR_TRUE : DFBR_FALSE;
        case DfBrCssPcNot:
          if (S->InnerList == DFBR_NULL) return DFBR_FALSE;
          return MatchSelectorListAny (S->InnerList, E, Ctx) ? DFBR_FALSE : DFBR_TRUE;
        case DfBrCssPcIs:
        case DfBrCssPcWhere:
          if (S->InnerList == DFBR_NULL) return DFBR_FALSE;
          return MatchSelectorListAny (S->InnerList, E, Ctx);
        case DfBrCssPcHas: {
          /* Match iff any descendant / sibling/adjacent target of
           * the inner complex selectors is reachable from E. v1
           * only handles descendant-relative. */
          const DfBrHtmlNode *Root = E;
          const DfBrHtmlNode *N;
          if (S->InnerList == DFBR_NULL) return DFBR_FALSE;
          for (N = E->FirstChild; N != DFBR_NULL; N = DfBrHtmlNodeNextPreorder (Root, N)) {
            if (N == E) continue;
            if (N->Kind != DFBR_HTML_NODE_ELEMENT) continue;
            if (MatchSelectorListAny (S->InnerList, N, Ctx)) return DFBR_TRUE;
          }
          return DFBR_FALSE;
        }
        case DfBrCssPcLang: {
          const char *V;
          const DfBrHtmlNode *N = E;
          while (N != DFBR_NULL) {
            if (N->Kind == DFBR_HTML_NODE_ELEMENT) {
              V = DfBrHtmlNodeGetAttrValue (N, "lang");
              if (V != DFBR_NULL) {
                usize VLen = strlen (V);
                return AttrMatchDash (V, VLen, S->LangCode, S->LangCodeLen, DFBR_TRUE);
              }
            }
            N = N->Parent;
          }
          return DFBR_FALSE;
        }
        default:
          return DFBR_FALSE;
      }

    case DfBrCssSelPseudoElement:
      /* Pseudo-elements select "virtual" children that the engine
       * generates (::before, ::after, ::first-line, ...). We don't
       * realise those boxes yet, so a rule targeting one is dead
       * code as far as the originating element is concerned. If we
       * returned TRUE here, declarations authored for the pseudo
       * would leak onto the real element - which is exactly the bug
       * that made `.row:before { content:""; display: table; }`
       * (Bootstrap's clearfix) flip every `.row` into a table box,
       * collapsing `<section class="row">` down to its shrink-to-fit
       * width and shrinking every col-* child with it. */
      return DFBR_FALSE;

    default:
      return DFBR_FALSE;
  }
}

/* ------------------------------------------------------------------
 *  Complex-selector right-to-left match
 * -----------------------------------------------------------------*/

/* Return the compound preceding `C` in the complex, i.e. the "left"
 * compound, or NULL. The combinator used is that predecessor's
 * ComboToNext. Since we only have a forward list, find it by walk. */
static const DfBrCssCompound *
PrevCompound (const DfBrCssCompound *C, const DfBrCssCompound *First) {
  const DfBrCssCompound *P;
  const DfBrCssCompound *Prev = DFBR_NULL;
  for (P = First; P != DFBR_NULL && P != C; P = P->Next) Prev = P;
  return Prev;
}

/* Recursive right-to-left match: given that compound C must match
 * element E, walk left. `First` is the complex's first compound
 * (used to find predecessors). */
static b32 MatchComplexRightWithFirst (
  const DfBrCssCompound    *C,
  const DfBrCssCompound    *First,
  const DfBrHtmlNode       *E,
  const DfBrCssMatchContext *Ctx
  )
{
  const DfBrCssCompound *Prev;
  DfBrCssCombinator      Combo;
  const DfBrHtmlNode    *N;

  if (C == DFBR_NULL) return DFBR_FALSE;
  if (!MatchCompound (C, E, Ctx)) return DFBR_FALSE;

  Prev = PrevCompound (C, First);
  if (Prev == DFBR_NULL) return DFBR_TRUE;   /* reached the leftmost */

  Combo = Prev->ComboToNext;
  switch (Combo) {
    case DfBrCssCombChild:
      if (E->Parent == DFBR_NULL) return DFBR_FALSE;
      return MatchComplexRightWithFirst (Prev, First, E->Parent, Ctx);

    case DfBrCssCombAdjacent:
      N = E->PrevSibling;
      while (N != DFBR_NULL && N->Kind != DFBR_HTML_NODE_ELEMENT) {
        N = N->PrevSibling;
      }
      if (N == DFBR_NULL) return DFBR_FALSE;
      return MatchComplexRightWithFirst (Prev, First, N, Ctx);

    case DfBrCssCombSibling:
      for (N = E->PrevSibling; N != DFBR_NULL; N = N->PrevSibling) {
        if (N->Kind != DFBR_HTML_NODE_ELEMENT) continue;
        if (MatchComplexRightWithFirst (Prev, First, N, Ctx)) return DFBR_TRUE;
      }
      return DFBR_FALSE;

    case DfBrCssCombDescendant:
    default:
      for (N = E->Parent; N != DFBR_NULL; N = N->Parent) {
        if (N->Kind != DFBR_HTML_NODE_ELEMENT) continue;
        if (MatchComplexRightWithFirst (Prev, First, N, Ctx)) return DFBR_TRUE;
      }
      return DFBR_FALSE;
  }
}

b32
DfBrCssMatchComplex (
  const DfBrCssComplex     *Cx,
  const DfBrHtmlNode       *E,
  const DfBrCssMatchContext *Ctx
  )
{
  if (Cx == DFBR_NULL || Cx->Last == DFBR_NULL) return DFBR_FALSE;
  return MatchComplexRightWithFirst (Cx->Last, Cx->First, E, Ctx);
}

static b32 MatchSelectorListAny (
  const DfBrCssComplex     *First,
  const DfBrHtmlNode       *E,
  const DfBrCssMatchContext *Ctx
  )
{
  const DfBrCssComplex *C;
  for (C = First; C != DFBR_NULL; C = C->Next) {
    if (DfBrCssMatchComplex (C, E, Ctx)) return DFBR_TRUE;
  }
  return DFBR_FALSE;
}

b32
DfBrCssMatchSelectorList (
  const DfBrCssSelectorList  *SL,
  const DfBrHtmlNode         *E,
  const DfBrCssMatchContext  *Ctx,
  u32                        *OutSpecificity
  )
{
  const DfBrCssComplex *C;
  u32                   Best = 0;
  b32                   Any  = DFBR_FALSE;

  if (SL == DFBR_NULL || E == DFBR_NULL) return DFBR_FALSE;

  for (C = SL->First; C != DFBR_NULL; C = C->Next) {
    if (DfBrCssMatchComplex (C, E, Ctx)) {
      Any = DFBR_TRUE;
      if (C->Specificity > Best) Best = C->Specificity;
    }
  }
  if (OutSpecificity != DFBR_NULL) *OutSpecificity = Best;
  return Any;
}
