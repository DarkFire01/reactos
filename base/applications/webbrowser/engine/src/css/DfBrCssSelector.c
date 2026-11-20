/** @file
  DfBrCssSelector.c - selector list / complex / compound / simple
                      selector parser, plus specificity computation.

  Coverage (CSS Selectors Level 4 subset):
    - type / universal / id / class
    - attribute [name], [name=value], all Level 3 operators with
      optional `i` / `s` case flag
    - pseudo-class (the Level-3 set plus :is, :where, :not(simple),
      :nth-child(An+B | even | odd), :has(simple))
    - pseudo-element (::before / ::after / ::first-line /
      ::first-letter / ::selection / ::marker / ::placeholder)
    - combinators: descendant, child, adjacent, general sibling
    - selector lists (comma-separated)

  Specificity is packed as (a<<20) | (b<<10) | c at parse time for
  O(1) cascade compares. Per spec:
      a = IDs
      b = classes + attributes + pseudo-classes
      c = types + pseudo-elements
  :not / :is / :has contribute their argument's max specificity;
  :where contributes zero.
**/

#include "DfBrCssInternal.h"

#include <stdio.h>
#include <string.h>

static DfBrCssSimpleSel *NewSimple (DfBrCssArena *A, DfBrCssSimpleSelKind K) {
  DfBrCssSimpleSel *S = (DfBrCssSimpleSel *)DfBrCssArenaZero (A, sizeof (DfBrCssSimpleSel));
  if (S != DFBR_NULL) S->Kind = K;
  return S;
}

static DfBrCssCompound *NewCompound (DfBrCssArena *A) {
  return (DfBrCssCompound *)DfBrCssArenaZero (A, sizeof (DfBrCssCompound));
}

static DfBrCssComplex *NewComplex (DfBrCssArena *A) {
  return (DfBrCssComplex *)DfBrCssArenaZero (A, sizeof (DfBrCssComplex));
}

static void AppendSimple (DfBrCssCompound *Cmp, DfBrCssSimpleSel *S) {
  DfBrCssSimpleSel **Slot;
  for (Slot = &Cmp->Simples; *Slot != DFBR_NULL; Slot = &(*Slot)->Next) { }
  *Slot = S;
}

static void AppendCompound (DfBrCssComplex *Cx, DfBrCssCompound *Cmp) {
  if (Cx->First == DFBR_NULL) {
    Cx->First = Cmp;
    Cx->Last  = Cmp;
  } else {
    Cx->Last->Next = Cmp;
    Cx->Last       = Cmp;
  }
}

/* ------------------------------------------------------------------
 *  Pseudo-class + pseudo-element name lookup
 * -----------------------------------------------------------------*/

typedef struct { const char *Name; DfBrCssPseudoClass K; } PcEntry;
static const PcEntry mPcTable[] = {
  { "hover",             DfBrCssPcHover },
  { "focus",             DfBrCssPcFocus },
  { "focus-within",      DfBrCssPcFocusWithin },
  { "focus-visible",     DfBrCssPcFocusVisible },
  { "active",            DfBrCssPcActive },
  { "first-child",       DfBrCssPcFirstChild },
  { "last-child",        DfBrCssPcLastChild },
  { "only-child",        DfBrCssPcOnlyChild },
  { "first-of-type",     DfBrCssPcFirstOfType },
  { "last-of-type",      DfBrCssPcLastOfType },
  { "only-of-type",      DfBrCssPcOnlyOfType },
  { "root",              DfBrCssPcRoot },
  { "empty",             DfBrCssPcEmpty },
  { "link",              DfBrCssPcLink },
  { "visited",           DfBrCssPcVisited },
  { "any-link",          DfBrCssPcAnyLink },
  { "local-link",        DfBrCssPcLocalLink },
  { "disabled",          DfBrCssPcDisabled },
  { "enabled",           DfBrCssPcEnabled },
  { "checked",           DfBrCssPcChecked },
  { "required",          DfBrCssPcRequired },
  { "optional",          DfBrCssPcOptional },
  { "read-only",         DfBrCssPcReadOnly },
  { "read-write",        DfBrCssPcReadWrite },
  { "placeholder-shown", DfBrCssPcPlaceholderShown },
  { "target",            DfBrCssPcTarget },
  { "scope",             DfBrCssPcScope },

  /* Functional */
  { "nth-child",         DfBrCssPcNthChild },
  { "nth-last-child",    DfBrCssPcNthLastChild },
  { "nth-of-type",       DfBrCssPcNthOfType },
  { "nth-last-of-type",  DfBrCssPcNthLastOfType },
  { "not",               DfBrCssPcNot },
  { "is",                DfBrCssPcIs },
  { "where",             DfBrCssPcWhere },
  { "has",               DfBrCssPcHas },
  { "lang",              DfBrCssPcLang },
  { "dir",               DfBrCssPcDir }
};

static DfBrCssPseudoClass LookupPc (const char *Name, usize Len) {
  usize i;
  for (i = 0; i < DFBR_ARRAY_LEN (mPcTable); i++) {
    if (DfBrCssAsciiEqZ (Name, Len, mPcTable[i].Name)) return mPcTable[i].K;
  }
  return DfBrCssPcUnknown;
}

typedef struct { const char *Name; DfBrCssPseudoElement K; } PeEntry;
static const PeEntry mPeTable[] = {
  { "before",       DfBrCssPeBefore },
  { "after",        DfBrCssPeAfter },
  { "first-line",   DfBrCssPeFirstLine },
  { "first-letter", DfBrCssPeFirstLetter },
  { "selection",    DfBrCssPeSelection },
  { "marker",       DfBrCssPeMarker },
  { "placeholder",  DfBrCssPePlaceholder },
  { "backdrop",     DfBrCssPeBackdrop }
};

static DfBrCssPseudoElement LookupPe (const char *Name, usize Len) {
  usize i;
  for (i = 0; i < DFBR_ARRAY_LEN (mPeTable); i++) {
    if (DfBrCssAsciiEqZ (Name, Len, mPeTable[i].Name)) return mPeTable[i].K;
  }
  return DfBrCssPeUnknown;
}

/* ------------------------------------------------------------------
 *  Small "parser cursor" so we can peek ahead easily. We buffer the
 *  current token here; pull the next one with Advance().
 * -----------------------------------------------------------------*/

typedef struct {
  DfBrCssTokenizer *Tk;
  DfBrCssToken      Cur;
  DfBrCssArena     *A;
  DfBrCssDiagList  *Diags;
} Cur;

static void Advance (Cur *C) {
  DfBrCssTokNext (C->Tk, &C->Cur);
}

static void SkipWs (Cur *C) {
  while (C->Cur.Kind == DfBrCssTokWhitespace) Advance (C);
}

/* ------------------------------------------------------------------
 *  An+B parsing for :nth-child and friends
 *
 *  Accepted productions:
 *    odd | even | <integer> | An | An+B | An-B | +An+B | ...
 * -----------------------------------------------------------------*/

static b32 ParseAnB (const DfBrCssCv *Head, i32 *OutA, i32 *OutB) {
  const DfBrCssCv *C = Head;
  i32 A = 0, B = 0;
  b32 HaveA = DFBR_FALSE;
  b32 Sign = 1;
  const char *Ident;
  usize IdentLen;

  /* Strip leading whitespace. */
  while (C != DFBR_NULL && C->Kind == DfBrCssCvKindWhitespace) C = C->Next;
  if (C == DFBR_NULL) return DFBR_FALSE;

  /* Keywords */
  if (C->Kind == DfBrCssCvKindIdent) {
    if (DfBrCssAsciiEqZ (C->Str, C->StrLen, "odd"))  { *OutA = 2; *OutB = 1; return DFBR_TRUE; }
    if (DfBrCssAsciiEqZ (C->Str, C->StrLen, "even")) { *OutA = 2; *OutB = 0; return DFBR_TRUE; }
    /* Bare `n` / `-n` / `+n` / dimensions like `n+3` are the tricky
     * case.  In token form a single <ident-token> is one of:
     *   n       -> A=1, need optional trailing signed integer
     *   -n      -> A=-1
     * Tokens like `3n` / `3n+1` come through as <dimension-token>
     * with the unit "n" (since digits + ident = dimension). */
    Ident = C->Str; IdentLen = C->StrLen;
    if ((IdentLen == 1 && (Ident[0] == 'n' || Ident[0] == 'N'))) {
      A = 1; HaveA = DFBR_TRUE;
    } else if (IdentLen == 2 && Ident[0] == '-' &&
               (Ident[1] == 'n' || Ident[1] == 'N')) {
      A = -1; HaveA = DFBR_TRUE;
    } else {
      return DFBR_FALSE;
    }
    C = C->Next;
    goto AfterN;
  }

  /* Dimension with unit ident that begins with `n` */
  if (C->Kind == DfBrCssCvKindDimension &&
      C->UnitLen >= 1 &&
      (C->Unit[0] == 'n' || C->Unit[0] == 'N')) {
    if (C->UnitLen == 1) {
      A = (i32)C->Number;
      HaveA = DFBR_TRUE;
      C = C->Next;
      goto AfterN;
    }
    /* "n-3" or similar baked into the unit: treat the unit's 'n'
     * + trailing part as B. e.g. dimension 3 unit "n-3" -> A=3, B=-3 */
    if (C->UnitLen > 1 && (C->Unit[1] == '-' || C->Unit[1] == '+')) {
      usize i;
      i32 BSign = (C->Unit[1] == '-') ? -1 : 1;
      i32 Val = 0;
      for (i = 2; i < C->UnitLen; i++) {
        if (C->Unit[i] < '0' || C->Unit[i] > '9') return DFBR_FALSE;
        Val = Val * 10 + (C->Unit[i] - '0');
      }
      A = (i32)C->Number;
      B = BSign * Val;
      *OutA = A; *OutB = B;
      return DFBR_TRUE;
    }
    return DFBR_FALSE;
  }

  /* Bare <number> */
  if (C->Kind == DfBrCssCvKindNumber) {
    if (!C->IsInteger) return DFBR_FALSE;
    *OutA = 0;
    *OutB = (i32)C->Number;
    return DFBR_TRUE;
  }

  /* Leading sign delim */
  if (C->Kind == DfBrCssCvKindDelim && C->StrLen == 1 &&
      (C->Str[0] == '+' || C->Str[0] == '-')) {
    Sign = (C->Str[0] == '-') ? -1 : 1;
    C = C->Next;
    while (C != DFBR_NULL && C->Kind == DfBrCssCvKindWhitespace) C = C->Next;
    if (C != DFBR_NULL && C->Kind == DfBrCssCvKindIdent &&
        C->StrLen == 1 && (C->Str[0] == 'n' || C->Str[0] == 'N')) {
      A = Sign; HaveA = DFBR_TRUE;
      C = C->Next;
      goto AfterN;
    }
  }

  return DFBR_FALSE;

AfterN:
  /* After `n` we can have an optional signed integer B. */
  while (C != DFBR_NULL && C->Kind == DfBrCssCvKindWhitespace) C = C->Next;
  if (C == DFBR_NULL) { *OutA = HaveA ? A : 0; *OutB = 0; return DFBR_TRUE; }
  if (C->Kind == DfBrCssCvKindDelim && C->StrLen == 1 &&
      (C->Str[0] == '+' || C->Str[0] == '-')) {
    i32 BSign = (C->Str[0] == '-') ? -1 : 1;
    C = C->Next;
    while (C != DFBR_NULL && C->Kind == DfBrCssCvKindWhitespace) C = C->Next;
    if (C == DFBR_NULL || C->Kind != DfBrCssCvKindNumber || !C->IsInteger) {
      return DFBR_FALSE;
    }
    B = BSign * (i32)C->Number;
    *OutA = A; *OutB = B;
    return DFBR_TRUE;
  }
  if (C->Kind == DfBrCssCvKindNumber && C->IsInteger && C->HasSign) {
    B = (i32)C->Number;
    *OutA = A; *OutB = B;
    return DFBR_TRUE;
  }
  *OutA = HaveA ? A : 0; *OutB = 0;
  return DFBR_TRUE;
}

/* Recursively parse a selector list from a CV stream (used by :not
 * / :is / :where / :has argument bodies). We build a child tokenizer
 * over the *source span* the CVs came from: from the first CV's
 * location to the end of the parent block. Simpler: run the main
 * selector parser on a minimally reconstructed source. For v1 we
 * take the easy path: only parse if the argument has a single
 * compound selector produced from the CV list. */

static DfBrCssComplex *ParseInnerSelector (
  const DfBrCssCv  *Body,
  DfBrCssArena     *A,
  DfBrCssDiagList  *Diags
  );

/* Forward decl for recursion. */
static DfBrCssCompound *ReadCompound (Cur *C, b32 *OutDone);

/* ------------------------------------------------------------------
 *  Simple selectors
 *
 *  Reads one simple (type / universal / id / class / attr / pseudo)
 *  starting from C->Cur (which must NOT be whitespace). Advances the
 *  cursor past the entire simple selector; returns NULL on parse
 *  error (Diag has been emitted).
 * -----------------------------------------------------------------*/

static DfBrCssSimpleSel *ReadSimple (Cur *C) {
  DfBrCssSimpleSel *S;

  switch (C->Cur.Kind) {
    case DfBrCssTokIdent:
      S = NewSimple (C->A, DfBrCssSelType);
      S->Name    = DfBrCssArenaStrDupLower (C->A, C->Cur.Str, C->Cur.StrLen);
      S->NameLen = C->Cur.StrLen;
      Advance (C);
      return S;

    case DfBrCssTokDelim:
      if (C->Cur.StrLen == 1 && C->Cur.Str[0] == '*') {
        S = NewSimple (C->A, DfBrCssSelUniversal);
        S->Name = "*"; S->NameLen = 1;
        Advance (C);
        return S;
      }
      if (C->Cur.StrLen == 1 && C->Cur.Str[0] == '.') {
        Advance (C);
        if (C->Cur.Kind != DfBrCssTokIdent) {
          DfBrCssDiagEmit (C->Diags, C->A, C->Cur.Line, C->Cur.Column,
                           "bad-class-selector");
          return DFBR_NULL;
        }
        S = NewSimple (C->A, DfBrCssSelClass);
        S->Name    = DfBrCssArenaStrDup (C->A, C->Cur.Str, C->Cur.StrLen);
        S->NameLen = C->Cur.StrLen;
        Advance (C);
        return S;
      }
      return DFBR_NULL;

    case DfBrCssTokHash:
      S = NewSimple (C->A, DfBrCssSelId);
      S->Name    = DfBrCssArenaStrDup (C->A, C->Cur.Str, C->Cur.StrLen);
      S->NameLen = C->Cur.StrLen;
      Advance (C);
      return S;

    case DfBrCssTokLBracket: {
      DfBrCssSimpleSel *Attr = NewSimple (C->A, DfBrCssSelAttr);
      Attr->AttrOp = DfBrCssAttrExists;
      Advance (C);
      SkipWs (C);
      if (C->Cur.Kind != DfBrCssTokIdent) {
        DfBrCssDiagEmit (C->Diags, C->A, C->Cur.Line, C->Cur.Column,
                         "bad-attr-selector-name");
        return DFBR_NULL;
      }
      Attr->Name    = DfBrCssArenaStrDupLower (C->A, C->Cur.Str, C->Cur.StrLen);
      Attr->NameLen = C->Cur.StrLen;
      Advance (C);
      SkipWs (C);
      if (C->Cur.Kind == DfBrCssTokRBracket) { Advance (C); return Attr; }
      switch (C->Cur.Kind) {
        case DfBrCssTokDelim:
          if (C->Cur.StrLen == 1 && C->Cur.Str[0] == '=') {
            Attr->AttrOp = DfBrCssAttrEq; break;
          }
          DfBrCssDiagEmit (C->Diags, C->A, C->Cur.Line, C->Cur.Column,
                           "bad-attr-operator");
          return DFBR_NULL;
        case DfBrCssTokInclude:        Attr->AttrOp = DfBrCssAttrIncludes;  break;
        case DfBrCssTokDashMatch:      Attr->AttrOp = DfBrCssAttrDashMatch; break;
        case DfBrCssTokPrefixMatch:    Attr->AttrOp = DfBrCssAttrPrefix;    break;
        case DfBrCssTokSuffixMatch:    Attr->AttrOp = DfBrCssAttrSuffix;    break;
        case DfBrCssTokSubstringMatch: Attr->AttrOp = DfBrCssAttrSubstring; break;
        default:
          DfBrCssDiagEmit (C->Diags, C->A, C->Cur.Line, C->Cur.Column,
                           "bad-attr-operator");
          return DFBR_NULL;
      }
      Advance (C);
      SkipWs (C);
      if (C->Cur.Kind == DfBrCssTokIdent || C->Cur.Kind == DfBrCssTokString) {
        Attr->AttrValue    = DfBrCssArenaStrDup (C->A, C->Cur.Str, C->Cur.StrLen);
        Attr->AttrValueLen = C->Cur.StrLen;
        Advance (C);
      } else {
        DfBrCssDiagEmit (C->Diags, C->A, C->Cur.Line, C->Cur.Column,
                         "bad-attr-value");
        return DFBR_NULL;
      }
      SkipWs (C);
      /* Optional `i` / `s` case flag. */
      if (C->Cur.Kind == DfBrCssTokIdent &&
          C->Cur.StrLen == 1 &&
          (C->Cur.Str[0] == 'i' || C->Cur.Str[0] == 'I')) {
        Attr->AttrIgnoreCase = DFBR_TRUE;
        Advance (C);
        SkipWs (C);
      } else if (C->Cur.Kind == DfBrCssTokIdent &&
                 C->Cur.StrLen == 1 &&
                 (C->Cur.Str[0] == 's' || C->Cur.Str[0] == 'S')) {
        Attr->AttrIgnoreCase = DFBR_FALSE;
        Advance (C);
        SkipWs (C);
      }
      if (C->Cur.Kind != DfBrCssTokRBracket) {
        DfBrCssDiagEmit (C->Diags, C->A, C->Cur.Line, C->Cur.Column,
                         "unclosed-attr-selector");
        return DFBR_NULL;
      }
      Advance (C);
      return Attr;
    }

    case DfBrCssTokColon: {
      b32  DoublePc = DFBR_FALSE;
      Advance (C);
      if (C->Cur.Kind == DfBrCssTokColon) {
        DoublePc = DFBR_TRUE;
        Advance (C);
      }
      if (C->Cur.Kind == DfBrCssTokIdent) {
        if (DoublePc) {
          DfBrCssSimpleSel *Pe = NewSimple (C->A, DfBrCssSelPseudoElement);
          Pe->Name    = DfBrCssArenaStrDupLower (C->A, C->Cur.Str, C->Cur.StrLen);
          Pe->NameLen = C->Cur.StrLen;
          Pe->Pe      = LookupPe (Pe->Name, Pe->NameLen);
          Advance (C);
          return Pe;
        }
        DfBrCssSimpleSel *Pc = NewSimple (C->A, DfBrCssSelPseudoClass);
        Pc->Name    = DfBrCssArenaStrDupLower (C->A, C->Cur.Str, C->Cur.StrLen);
        Pc->NameLen = C->Cur.StrLen;
        Pc->Pc      = LookupPc (Pc->Name, Pc->NameLen);
        /* CSS legacy: ::before etc. may be spelled with one colon. */
        if (Pc->Pc == DfBrCssPcUnknown) {
          DfBrCssPseudoElement Pe = LookupPe (Pc->Name, Pc->NameLen);
          if (Pe != DfBrCssPeUnknown) {
            Pc->Kind = DfBrCssSelPseudoElement;
            Pc->Pe   = Pe;
          }
        }
        Advance (C);
        return Pc;
      }
      if (C->Cur.Kind == DfBrCssTokFunction) {
        DfBrCssSimpleSel *Pc = NewSimple (C->A, DfBrCssSelPseudoClass);
        Pc->Name    = DfBrCssArenaStrDupLower (C->A, C->Cur.Str, C->Cur.StrLen);
        Pc->NameLen = C->Cur.StrLen;
        Pc->Pc      = LookupPc (Pc->Name, Pc->NameLen);
        Advance (C);
        /* Collect body CVs up to `)` to use as argument. */
        {
          DfBrCssCv *Head = DFBR_NULL;
          DfBrCssCv *Tail = DFBR_NULL;
          while (C->Cur.Kind != DfBrCssTokRParen &&
                 C->Cur.Kind != DfBrCssTokEof) {
            DfBrCssCv *N;
            switch (C->Cur.Kind) {
              case DfBrCssTokIdent:
                N = (DfBrCssCv *)DfBrCssArenaZero (C->A, sizeof (DfBrCssCv));
                N->Kind = DfBrCssCvKindIdent;
                N->Str  = C->Cur.Str; N->StrLen = C->Cur.StrLen;
                break;
              case DfBrCssTokNumber:
                N = (DfBrCssCv *)DfBrCssArenaZero (C->A, sizeof (DfBrCssCv));
                N->Kind = DfBrCssCvKindNumber;
                N->Number = C->Cur.Number; N->IsInteger = C->Cur.IsInteger;
                N->HasSign = C->Cur.HasSign;
                break;
              case DfBrCssTokDimension:
                N = (DfBrCssCv *)DfBrCssArenaZero (C->A, sizeof (DfBrCssCv));
                N->Kind = DfBrCssCvKindDimension;
                N->Number = C->Cur.Number; N->IsInteger = C->Cur.IsInteger;
                N->Unit = C->Cur.Unit; N->UnitLen = C->Cur.UnitLen;
                N->UnitId = C->Cur.UnitId;
                break;
              case DfBrCssTokDelim:
                N = (DfBrCssCv *)DfBrCssArenaZero (C->A, sizeof (DfBrCssCv));
                N->Kind = DfBrCssCvKindDelim;
                N->Str = C->Cur.Str; N->StrLen = C->Cur.StrLen;
                break;
              case DfBrCssTokWhitespace:
                N = (DfBrCssCv *)DfBrCssArenaZero (C->A, sizeof (DfBrCssCv));
                N->Kind = DfBrCssCvKindWhitespace;
                break;
              case DfBrCssTokString:
                N = (DfBrCssCv *)DfBrCssArenaZero (C->A, sizeof (DfBrCssCv));
                N->Kind = DfBrCssCvKindString;
                N->Str = C->Cur.Str; N->StrLen = C->Cur.StrLen;
                break;
              case DfBrCssTokComma:
                N = (DfBrCssCv *)DfBrCssArenaZero (C->A, sizeof (DfBrCssCv));
                N->Kind = DfBrCssCvKindComma;
                break;
              default:
                N = (DfBrCssCv *)DfBrCssArenaZero (C->A, sizeof (DfBrCssCv));
                N->Kind = DfBrCssCvKindDelim;
                break;
            }
            if (Tail == DFBR_NULL) Head = N; else Tail->Next = N;
            Tail = N;
            Advance (C);
          }
          if (C->Cur.Kind == DfBrCssTokRParen) Advance (C);

          if (Pc->Pc == DfBrCssPcNthChild ||
              Pc->Pc == DfBrCssPcNthLastChild ||
              Pc->Pc == DfBrCssPcNthOfType ||
              Pc->Pc == DfBrCssPcNthLastOfType) {
            Pc->NthValid = ParseAnB (Head, &Pc->NthA, &Pc->NthB);
          } else if (Pc->Pc == DfBrCssPcLang || Pc->Pc == DfBrCssPcDir) {
            const DfBrCssCv *CC;
            for (CC = Head; CC != DFBR_NULL; CC = CC->Next) {
              if (CC->Kind == DfBrCssCvKindIdent || CC->Kind == DfBrCssCvKindString) {
                Pc->LangCode    = DfBrCssArenaStrDup (C->A, CC->Str, CC->StrLen);
                Pc->LangCodeLen = CC->StrLen;
                break;
              }
            }
          } else if (Pc->Pc == DfBrCssPcNot || Pc->Pc == DfBrCssPcIs ||
                     Pc->Pc == DfBrCssPcWhere || Pc->Pc == DfBrCssPcHas) {
            Pc->InnerList = ParseInnerSelector (Head, C->A, C->Diags);
          }
        }
        return Pc;
      }
      DfBrCssDiagEmit (C->Diags, C->A, C->Cur.Line, C->Cur.Column,
                       "bad-pseudo-selector");
      return DFBR_NULL;
    }

    default:
      return DFBR_NULL;
  }
}

/* Parse a compound selector: run of simple selectors with no
 * whitespace between. Returns NULL on empty. */
static DfBrCssCompound *ReadCompound (Cur *C, b32 *OutDone) {
  DfBrCssCompound  *Cmp;
  DfBrCssSimpleSel *S;
  b32               First = DFBR_TRUE;

  *OutDone = DFBR_FALSE;
  SkipWs (C);

  /* Stop tokens. */
  if (C->Cur.Kind == DfBrCssTokEof ||
      C->Cur.Kind == DfBrCssTokLBrace ||
      C->Cur.Kind == DfBrCssTokComma ||
      C->Cur.Kind == DfBrCssTokSemicolon ||
      C->Cur.Kind == DfBrCssTokRBrace) {
    *OutDone = DFBR_TRUE;
    return DFBR_NULL;
  }

  Cmp = NewCompound (C->A);
  for (;;) {
    if (C->Cur.Kind == DfBrCssTokWhitespace
        || C->Cur.Kind == DfBrCssTokEof
        || C->Cur.Kind == DfBrCssTokLBrace
        || C->Cur.Kind == DfBrCssTokComma
        || C->Cur.Kind == DfBrCssTokSemicolon
        || C->Cur.Kind == DfBrCssTokRBrace) {
      break;
    }
    /* Combinator tokens inside a compound? Only if this is the first
     * simple AND the token is something like `>` - handled by the
     * caller. */
    if (C->Cur.Kind == DfBrCssTokDelim && C->Cur.StrLen == 1 &&
        (C->Cur.Str[0] == '>' || C->Cur.Str[0] == '+' || C->Cur.Str[0] == '~')) {
      if (First) {
        /* Leading combinator - bail */
        break;
      }
      break;
    }
    S = ReadSimple (C);
    if (S == DFBR_NULL) {
      /* Couldn't parse a simple - bail. */
      break;
    }
    AppendSimple (Cmp, S);
    First = DFBR_FALSE;
  }
  if (Cmp->Simples == DFBR_NULL) return DFBR_NULL;
  return Cmp;
}

/* Parse one complex selector (compound [combinator compound]*). */
static DfBrCssComplex *ReadComplex (Cur *C) {
  DfBrCssComplex   *Cx;
  DfBrCssCompound  *Cmp;
  DfBrCssCombinator  Combo;
  b32               Done = DFBR_FALSE;

  SkipWs (C);
  if (C->Cur.Kind == DfBrCssTokEof ||
      C->Cur.Kind == DfBrCssTokLBrace ||
      C->Cur.Kind == DfBrCssTokComma ||
      C->Cur.Kind == DfBrCssTokSemicolon ||
      C->Cur.Kind == DfBrCssTokRBrace) {
    return DFBR_NULL;
  }

  Cx = NewComplex (C->A);
  Cmp = ReadCompound (C, &Done);
  if (Cmp == DFBR_NULL) return DFBR_NULL;
  AppendCompound (Cx, Cmp);

  for (;;) {
    /* Between compounds: whitespace is the descendant combinator;
     * an explicit `>`, `+`, `~` overrides it. */
    b32  HadWs = DFBR_FALSE;
    while (C->Cur.Kind == DfBrCssTokWhitespace) { HadWs = DFBR_TRUE; Advance (C); }

    if (C->Cur.Kind == DfBrCssTokEof ||
        C->Cur.Kind == DfBrCssTokLBrace ||
        C->Cur.Kind == DfBrCssTokComma ||
        C->Cur.Kind == DfBrCssTokSemicolon ||
        C->Cur.Kind == DfBrCssTokRBrace) {
      break;
    }

    if (C->Cur.Kind == DfBrCssTokDelim && C->Cur.StrLen == 1) {
      switch (C->Cur.Str[0]) {
        case '>': Combo = DfBrCssCombChild;    Advance (C); break;
        case '+': Combo = DfBrCssCombAdjacent; Advance (C); break;
        case '~': Combo = DfBrCssCombSibling;  Advance (C); break;
        default:
          if (HadWs) { Combo = DfBrCssCombDescendant; break; }
          /* Unexpected delim - stop. */
          goto Done;
      }
    } else {
      if (!HadWs) goto Done;
      Combo = DfBrCssCombDescendant;
    }

    /* Now read the next compound. */
    Cx->Last->ComboToNext = Combo;
    SkipWs (C);
    Cmp = ReadCompound (C, &Done);
    if (Cmp == DFBR_NULL) goto Done;
    AppendCompound (Cx, Cmp);
  }
Done:

  /* Compute specificity. */
  {
    u32 a = 0, b = 0, c = 0;
    DfBrCssCompound *Cp;
    DfBrCssSimpleSel *Sp;
    for (Cp = Cx->First; Cp != DFBR_NULL; Cp = Cp->Next) {
      for (Sp = Cp->Simples; Sp != DFBR_NULL; Sp = Sp->Next) {
        switch (Sp->Kind) {
          case DfBrCssSelId:            a++; break;
          case DfBrCssSelClass:
          case DfBrCssSelAttr:          b++; break;
          case DfBrCssSelPseudoClass:
            if (Sp->Pc == DfBrCssPcWhere) { /* zero */ }
            else if (Sp->Pc == DfBrCssPcNot || Sp->Pc == DfBrCssPcIs
                  || Sp->Pc == DfBrCssPcHas) {
              if (Sp->InnerList != DFBR_NULL) {
                DfBrCssComplex *Inner;
                u32 MaxSpec = 0;
                for (Inner = Sp->InnerList; Inner != DFBR_NULL; Inner = Inner->Next) {
                  if (Inner->Specificity > MaxSpec) MaxSpec = Inner->Specificity;
                }
                a += (MaxSpec >> 20) & 0x3FF;
                b += (MaxSpec >> 10) & 0x3FF;
                c +=  MaxSpec        & 0x3FF;
              } else {
                b++;
              }
            } else {
              b++;
            }
            break;
          case DfBrCssSelType:          c++; break;
          case DfBrCssSelPseudoElement: c++; break;
          default: break;
        }
      }
    }
    if (a > 0x3FF) a = 0x3FF;
    if (b > 0x3FF) b = 0x3FF;
    if (c > 0x3FF) c = 0x3FF;
    Cx->Specificity = (a << 20) | (b << 10) | c;
  }
  return Cx;
}

/* ------------------------------------------------------------------
 *  Inner selector parser (for :not / :is / :where / :has argument
 *  bodies represented as CV lists).
 *
 *  We stream the CVs back into a fresh tokenizer over their source
 *  - but CVs don't preserve their source span. Instead we reconstruct
 *  a textual form of the CV list good enough to re-tokenize. This
 *  is slower than the direct path but handles arbitrary nesting.
 *
 *  For v1 we support the common case: a single compound selector or
 *  a comma-separated list of compound selectors (no combinators).
 * -----------------------------------------------------------------*/

static DfBrCssComplex *ParseInnerSelector (
  const DfBrCssCv  *Body,
  DfBrCssArena     *A,
  DfBrCssDiagList  *Diags
  )
{
  /* Reconstruct a short source string from the CV list. */
  char *Buf;
  usize Cap = 64;
  usize Len = 0;
  const DfBrCssCv *C;
  DfBrCssTokenizer Sub;
  Cur Cur2;
  DfBrCssComplex *Head = DFBR_NULL;
  DfBrCssComplex *Tail = DFBR_NULL;

  Buf = (char *)DfBrCssArenaAlloc (A, Cap);
  if (Buf == DFBR_NULL) return DFBR_NULL;

  #define ENSURE(n) do { \
    if (Len + (n) + 1 > Cap) { \
      usize NC = Cap; while (Len + (n) + 1 > NC) NC *= 2; \
      char *NB = (char *)DfBrCssArenaAlloc (A, NC); \
      if (NB == DFBR_NULL) return DFBR_NULL; \
      memcpy (NB, Buf, Len); Buf = NB; Cap = NC; \
    } \
  } while (0)

  for (C = Body; C != DFBR_NULL; C = C->Next) {
    switch (C->Kind) {
      case DfBrCssCvKindIdent:
      case DfBrCssCvKindFunction:
        ENSURE (C->StrLen);
        if (C->StrLen > 0) memcpy (Buf + Len, C->Str, C->StrLen);
        Len += C->StrLen;
        if (C->Kind == DfBrCssCvKindFunction) {
          ENSURE (1);
          Buf[Len++] = '(';
          /* Walk children recursively: for v1 only flat cases. */
          ENSURE (1);
          Buf[Len++] = ')';
        }
        break;
      case DfBrCssCvKindHash:
        ENSURE (C->StrLen + 1);
        Buf[Len++] = '#';
        if (C->StrLen > 0) memcpy (Buf + Len, C->Str, C->StrLen);
        Len += C->StrLen;
        break;
      case DfBrCssCvKindNumber: {
        char Tmp[32];
        int n;
        if (C->IsInteger) n = snprintf (Tmp, sizeof Tmp, "%ld", (long)C->Number);
        else              n = snprintf (Tmp, sizeof Tmp, "%g", C->Number);
        if (n < 0) n = 0;
        ENSURE ((usize)n);
        memcpy (Buf + Len, Tmp, (usize)n);
        Len += (usize)n;
        break;
      }
      case DfBrCssCvKindDelim:
        ENSURE (C->StrLen);
        if (C->StrLen > 0) memcpy (Buf + Len, C->Str, C->StrLen);
        Len += C->StrLen;
        break;
      case DfBrCssCvKindString:
        ENSURE (C->StrLen + 2);
        Buf[Len++] = '"';
        if (C->StrLen > 0) memcpy (Buf + Len, C->Str, C->StrLen);
        Len += C->StrLen;
        Buf[Len++] = '"';
        break;
      case DfBrCssCvKindComma:
        ENSURE (1); Buf[Len++] = ','; break;
      case DfBrCssCvKindColon:
        ENSURE (1); Buf[Len++] = ':'; break;
      case DfBrCssCvKindWhitespace:
        ENSURE (1); Buf[Len++] = ' '; break;
      default:
        break;
    }
  }
  #undef ENSURE
  Buf[Len] = '\0';

  DfBrCssTokInit (&Sub, Buf, Len, A, Diags);
  Cur2.Tk = &Sub;
  Cur2.A = A;
  Cur2.Diags = Diags;
  Advance (&Cur2);
  for (;;) {
    DfBrCssComplex *Cx = ReadComplex (&Cur2);
    if (Cx != DFBR_NULL) {
      if (Tail == DFBR_NULL) Head = Cx; else Tail->Next = Cx;
      Tail = Cx;
    }
    SkipWs (&Cur2);
    if (Cur2.Cur.Kind == DfBrCssTokComma) { Advance (&Cur2); continue; }
    break;
  }
  return Head;
}

/* ------------------------------------------------------------------
 *  Public: parse a selector list from the current tokenizer position.
 *  Stops at `{` (consumed) or EOF.
 * -----------------------------------------------------------------*/

DfBrCssSelectorList *
DfBrCssParseSelectorList (
  DfBrCssTokenizer  *Tok,
  DfBrCssArena      *A,
  DfBrCssDiagList   *Diags
  )
{
  DfBrCssSelectorList *SL;
  DfBrCssComplex      *Head = DFBR_NULL;
  DfBrCssComplex      *Tail = DFBR_NULL;
  Cur                  C;

  SL = (DfBrCssSelectorList *)DfBrCssArenaZero (A, sizeof (DfBrCssSelectorList));

  C.Tk    = Tok;
  C.A     = A;
  C.Diags = Diags;
  Advance (&C);

  for (;;) {
    DfBrCssComplex *Cx = ReadComplex (&C);
    if (Cx != DFBR_NULL) {
      if (Tail == DFBR_NULL) Head = Cx; else Tail->Next = Cx;
      Tail = Cx;
    }
    SkipWs (&C);
    if (C.Cur.Kind == DfBrCssTokComma) { Advance (&C); continue; }
    break;
  }

  /* Consume the `{` that normally terminates the selector list. */
  SkipWs (&C);
  /* We leave the tokenizer positioned so the declaration-block parser
   * can start on the very next token. If we're at `{`, we must
   * consume it; anything else (EOF, `}`, ...) stays as-is. */
  if (C.Cur.Kind == DfBrCssTokLBrace) {
    /* ok */
  }

  SL->First = Head;
  return SL;
}

/* ==================================================================
 *
 *  Public query surface (DfBrCss.h)
 *
 *  Lives here so the selector list lifetime (and its arena) are
 *  confined to one TU. A query owns a CSS arena whose chunks are
 *  freed on Destroy; the node pointers it yields belong to the DOM
 *  document, not the query, and stay valid until the doc is freed.
 *
 * ================================================================*/

#include "../html/DfBrHtml.h"
#include <stdlib.h>
#include <string.h>

struct DfBrCssQueryResult {
  DfBrCssArena               Arena;      /* owns selector + node array */
  const DfBrHtmlNode       **Nodes;
  usize                      Count;
  usize                      Cap;
};

static void
PushNode (
  DfBrCssQueryResult    *R,
  const DfBrHtmlNode    *N
  )
{
  const DfBrHtmlNode  **New;
  usize                 NewCap;
  usize                 i;

  if (R->Count >= R->Cap) {
    NewCap = (R->Cap == 0) ? 8 : (R->Cap * 2);
    New = (const DfBrHtmlNode **)DfBrCssArenaAlloc (
            &R->Arena, NewCap * sizeof (*New));
    if (New == DFBR_NULL) return;
    for (i = 0; i < R->Count; i++) New[i] = R->Nodes[i];
    R->Nodes = New;
    R->Cap   = NewCap;
  }
  R->Nodes[R->Count++] = N;
}

DfBrCssQueryResult *
DfBrCssQuerySelectorAll (
  const struct DfBrHtmlNode  *Root,
  const char                 *Selector,
  usize                       SelLen
  )
{
  DfBrCssQueryResult   *R;
  DfBrCssTokenizer      Tok;
  DfBrCssSelectorList  *SL;
  DfBrCssMatchContext   Ctx;
  const DfBrHtmlNode   *N;
  DfBrCssDiagList       Diags;

  R = (DfBrCssQueryResult *)calloc (1, sizeof (*R));
  if (R == DFBR_NULL) return DFBR_NULL;

  if (Root == DFBR_NULL || Selector == DFBR_NULL) {
    return R;      /* valid, empty */
  }
  if (SelLen == 0) SelLen = strlen (Selector);
  if (SelLen == 0) return R;

  memset (&Diags, 0, sizeof (Diags));
  DfBrCssTokInit (&Tok, Selector, SelLen, &R->Arena, &Diags);
  SL = DfBrCssParseSelectorList (&Tok, &R->Arena, &Diags);
  if (SL == DFBR_NULL || SL->First == DFBR_NULL) {
    return R;      /* empty (invalid selector -> empty set, per spec:
                    * throw SyntaxError. We softly swallow for now.) */
  }

  /* :root checks use the document element, but we also need it to
   * stay valid across the pre-order walk. Walk from Root: if Root is
   * the DOCUMENT node, then `:root` is its child <html>. Use the
   * document root when we can find it. */
  memset (&Ctx, 0, sizeof (Ctx));
  Ctx.Root = Root;
  for (N = Root; N != DFBR_NULL; N = DfBrHtmlNodeNextPreorder (Root, N)) {
    if (N->Kind != DFBR_HTML_NODE_ELEMENT) continue;
    if (DfBrCssMatchSelectorList (SL, N, &Ctx, DFBR_NULL)) {
      PushNode (R, N);
    }
  }
  return R;
}

const struct DfBrHtmlNode *
DfBrCssQuerySelector (
  const struct DfBrHtmlNode  *Root,
  const char                 *Selector,
  usize                       SelLen
  )
{
  DfBrCssQueryResult        *R;
  const struct DfBrHtmlNode *Out = DFBR_NULL;

  R = DfBrCssQuerySelectorAll (Root, Selector, SelLen);
  if (R != DFBR_NULL && R->Count > 0) {
    Out = R->Nodes[0];
  }
  DfBrCssQueryResultDestroy (R);
  return Out;
}

usize
DfBrCssQueryResultCount (
  const DfBrCssQueryResult  *R
  )
{
  return (R != DFBR_NULL) ? R->Count : 0;
}

const struct DfBrHtmlNode *
DfBrCssQueryResultAt (
  const DfBrCssQueryResult  *R,
  usize                      Idx
  )
{
  if (R == DFBR_NULL || Idx >= R->Count) return DFBR_NULL;
  return R->Nodes[Idx];
}

void
DfBrCssQueryResultDestroy (
  DfBrCssQueryResult  *R
  )
{
  if (R == DFBR_NULL) return;
  DfBrCssArenaDispose (&R->Arena);
  free (R);
}
