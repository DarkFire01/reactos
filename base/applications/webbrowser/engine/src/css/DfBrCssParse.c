/** @file
  DfBrCssParse.c - CSS stylesheet parser built on top of the
                   DfBrCssTokenize tokenizer.

  Produces:
    - DfBrCssStylesheet -> linked list of DfBrCssRule
    - Each qualified rule owns a DfBrCssSelectorList (via
      DfBrCssParseSelectorList - see DfBrCssSelector.c) and a
      DfBrCssDeclaration list.
    - Each declaration owns a DfBrCssCv list representing its value.
    - Each at-rule preserves its name + prelude CV list + optional
      inner rule list (for nested blocks like @media).

  Error-tolerance rules (matching the CSS-Syntax spec):
    - A malformed declaration is discarded (with a diagnostic) but
      does not break the surrounding block.
    - A qualified rule with a malformed body opens / closes braces
      in a balanced way; we seek to `}` before continuing.
    - EOF at any unterminated block is reported but doesn't
      cascade into the caller.
**/

#include "DfBrCssInternal.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------
 *  Component value construction
 * -----------------------------------------------------------------*/

static DfBrCssCv *NewCv (DfBrCssArena *A, DfBrCssCvKind K) {
  DfBrCssCv *C = (DfBrCssCv *)DfBrCssArenaZero (A, sizeof (DfBrCssCv));
  if (C == DFBR_NULL) return DFBR_NULL;
  C->Kind = K;
  return C;
}

static void AppendCv (DfBrCssCv **Head, DfBrCssCv **Tail, DfBrCssCv *N) {
  if (N == DFBR_NULL) return;
  if (*Tail == DFBR_NULL) {
    *Head = N;
    *Tail = N;
  } else {
    (*Tail)->Next = N;
    *Tail = N;
  }
}

/* ------------------------------------------------------------------
 *  CSS custom property (var()) resolution
 *
 *  We keep unresolved `var(--foo, fallback)` CVs inside declaration
 *  values until apply time; resolution walks the element's custom-
 *  property chain (chained through the parent), producing a fresh
 *  CV list allocated from the style-context arena. Failing to resolve
 *  a mandatory var() returns NULL so the caller can mark the
 *  declaration invalid-at-computed-value.
 *
 *  A depth limit prevents cyclic `--a: var(--b); --b: var(--a)` from
 *  blowing the stack; the CSS spec mandates this treat the declaration
 *  as invalid.
 * -----------------------------------------------------------------*/

#define DFBR_VAR_MAX_DEPTH 32

static DfBrCssCv *
CloneCvShallow (DfBrCssArena *A, const DfBrCssCv *Src) {
  DfBrCssCv *N;
  if (Src == DFBR_NULL) return DFBR_NULL;
  N = (DfBrCssCv *)DfBrCssArenaZero (A, sizeof (DfBrCssCv));
  if (N == DFBR_NULL) return DFBR_NULL;
  *N = *Src;
  N->Next     = DFBR_NULL;
  N->Children = DFBR_NULL;
  return N;
}

static b32
AsciiEqCase (const char *A, usize Al, const char *B, usize Bl) {
  usize i;
  if (Al != Bl) return DFBR_FALSE;
  for (i = 0; i < Al; i++) {
    char a = A[i]; char b = B[i];
    if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
    if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
    if (a != b) return DFBR_FALSE;
  }
  return DFBR_TRUE;
}

static const DfBrCssCv *
LookupVar (const DfBrCssComputedStyle *Cs, const char *Name, usize NameLen) {
  const DfBrCssCustomProp *P;
  if (Cs == DFBR_NULL) return DFBR_NULL;
  for (P = Cs->CustomProps; P != DFBR_NULL; P = P->Next) {
    if (P->NameLen == NameLen &&
        memcmp (P->Name, Name, NameLen) == 0)
    {
      return P->Value;
    }
  }
  return DFBR_NULL;
}

static DfBrCssCv *
ResolveVarsImpl (
  DfBrCssArena               *A,
  const DfBrCssComputedStyle *Cs,
  const DfBrCssCv            *Src,
  i32                         Depth,
  b32                        *OutFailed
  );

/* Append a deep-cloned copy of chain `Src` onto (Head, Tail), recursing
 * into Children and substituting nested var()s. */
static void
AppendResolved (
  DfBrCssArena               *A,
  const DfBrCssComputedStyle *Cs,
  const DfBrCssCv            *Src,
  DfBrCssCv                 **Head,
  DfBrCssCv                 **Tail,
  i32                         Depth,
  b32                        *OutFailed
  )
{
  const DfBrCssCv *C;
  for (C = Src; C != DFBR_NULL; C = C->Next) {
    if (C->Kind == DfBrCssCvKindFunction && C->Str != DFBR_NULL &&
        AsciiEqCase (C->Str, C->StrLen, "var", 3))
    {
      /* var(--name [, fallback]) */
      const DfBrCssCv *Arg = C->Children;
      while (Arg != DFBR_NULL && Arg->Kind == DfBrCssCvKindWhitespace) {
        Arg = Arg->Next;
      }
      if (Arg != DFBR_NULL && Arg->Kind == DfBrCssCvKindIdent &&
          Arg->StrLen >= 2 && Arg->Str[0] == '-' && Arg->Str[1] == '-')
      {
        const DfBrCssCv *Found = LookupVar (Cs, Arg->Str, Arg->StrLen);
        const DfBrCssCv *Fallback = DFBR_NULL;
        const DfBrCssCv *F = Arg->Next;
        while (F != DFBR_NULL && F->Kind == DfBrCssCvKindWhitespace) F = F->Next;
        if (F != DFBR_NULL && F->Kind == DfBrCssCvKindComma) {
          Fallback = F->Next;
          while (Fallback != DFBR_NULL && Fallback->Kind == DfBrCssCvKindWhitespace) {
            Fallback = Fallback->Next;
          }
        }
        if (Found != DFBR_NULL) {
          AppendResolved (A, Cs, Found, Head, Tail, Depth + 1, OutFailed);
        } else if (Fallback != DFBR_NULL) {
          AppendResolved (A, Cs, Fallback, Head, Tail, Depth + 1, OutFailed);
        } else {
          *OutFailed = DFBR_TRUE;
        }
        continue;
      }
      /* Malformed var() (e.g. var(42) with no --ident): drop. */
      *OutFailed = DFBR_TRUE;
      continue;
    }

    /* Normal CV: shallow-clone, recurse into Children for
     * functions / blocks so nested var()s inside e.g. calc() or
     * rgba() get resolved too. */
    {
      DfBrCssCv *N = CloneCvShallow (A, C);
      if (N == DFBR_NULL) { *OutFailed = DFBR_TRUE; return; }
      if (C->Children != DFBR_NULL && Depth + 1 < DFBR_VAR_MAX_DEPTH) {
        N->Children = ResolveVarsImpl (A, Cs, C->Children, Depth + 1, OutFailed);
      }
      AppendCv (Head, Tail, N);
    }
  }
}

static DfBrCssCv *
ResolveVarsImpl (
  DfBrCssArena               *A,
  const DfBrCssComputedStyle *Cs,
  const DfBrCssCv            *Src,
  i32                         Depth,
  b32                        *OutFailed
  )
{
  DfBrCssCv *Head = DFBR_NULL;
  DfBrCssCv *Tail = DFBR_NULL;
  if (Depth >= DFBR_VAR_MAX_DEPTH) { *OutFailed = DFBR_TRUE; return DFBR_NULL; }
  AppendResolved (A, Cs, Src, &Head, &Tail, Depth, OutFailed);
  return Head;
}

DfBrCssCv *
DfBrCssResolveVars (
  DfBrCssArena               *A,
  const DfBrCssComputedStyle *Cs,
  const DfBrCssCv            *Src
  )
{
  b32 Failed = DFBR_FALSE;
  DfBrCssCv *Out;
  if (Src == DFBR_NULL || A == DFBR_NULL) return DFBR_NULL;
  Out = ResolveVarsImpl (A, Cs, Src, 0, &Failed);
  if (Failed) return DFBR_NULL;
  return Out;
}

/* Convert a token to a CV, ignoring pure bracket balancing (the
 * caller handles `{`, `}`, `[`, `]`, `(`, `)`). Returns NULL for
 * tokens we collapse (EOF, whitespace left for caller to preserve). */
static DfBrCssCv *CvFromToken (
  DfBrCssArena        *A,
  const DfBrCssToken  *T
  )
{
  DfBrCssCv  *Cv;

  switch (T->Kind) {
    case DfBrCssTokIdent:
      Cv = NewCv (A, DfBrCssCvKindIdent);
      break;
    case DfBrCssTokFunction:
      Cv = NewCv (A, DfBrCssCvKindFunction);
      break;
    case DfBrCssTokAtKeyword:
      Cv = NewCv (A, DfBrCssCvKindAtKeyword);
      break;
    case DfBrCssTokHash:
      Cv = NewCv (A, DfBrCssCvKindHash);
      break;
    case DfBrCssTokString:
    case DfBrCssTokBadString:
      Cv = NewCv (A, DfBrCssCvKindString);
      break;
    case DfBrCssTokUrl:
    case DfBrCssTokBadUrl:
      Cv = NewCv (A, DfBrCssCvKindUrl);
      break;
    case DfBrCssTokNumber:
      Cv = NewCv (A, DfBrCssCvKindNumber);
      break;
    case DfBrCssTokPercentage:
      Cv = NewCv (A, DfBrCssCvKindPercentage);
      break;
    case DfBrCssTokDimension:
      Cv = NewCv (A, DfBrCssCvKindDimension);
      break;
    case DfBrCssTokDelim:
      Cv = NewCv (A, DfBrCssCvKindDelim);
      break;
    case DfBrCssTokColon:
      Cv = NewCv (A, DfBrCssCvKindColon);
      break;
    case DfBrCssTokComma:
      Cv = NewCv (A, DfBrCssCvKindComma);
      break;
    case DfBrCssTokWhitespace:
      Cv = NewCv (A, DfBrCssCvKindWhitespace);
      break;
    case DfBrCssTokInclude:
      Cv = NewCv (A, DfBrCssCvKindInclude);
      break;
    case DfBrCssTokDashMatch:
      Cv = NewCv (A, DfBrCssCvKindDashMatch);
      break;
    case DfBrCssTokPrefixMatch:
      Cv = NewCv (A, DfBrCssCvKindPrefixMatch);
      break;
    case DfBrCssTokSuffixMatch:
      Cv = NewCv (A, DfBrCssCvKindSuffixMatch);
      break;
    case DfBrCssTokSubstringMatch:
      Cv = NewCv (A, DfBrCssCvKindSubstringMatch);
      break;
    case DfBrCssTokColumn:
      Cv = NewCv (A, DfBrCssCvKindColumn);
      break;
    default:
      return DFBR_NULL;
  }
  if (Cv == DFBR_NULL) return DFBR_NULL;
  Cv->Str       = T->Str;
  Cv->StrLen    = T->StrLen;
  Cv->Number    = T->Number;
  Cv->IsInteger = T->IsInteger;
  Cv->HasSign   = T->HasSign;
  Cv->Unit      = T->Unit;
  Cv->UnitLen   = T->UnitLen;
  Cv->UnitId    = T->UnitId;
  Cv->Line      = T->Line;
  Cv->Column    = T->Column;
  return Cv;
}

/* Read until the matching closing bracket at top level of a balanced
 * construct. Consumes the closer. CVs for whitespace and nested
 * blocks/functions are preserved in Children. */
static DfBrCssCv *CollectBalanced (
  DfBrCssTokenizer  *T,
  DfBrCssArena      *A,
  DfBrCssDiagList   *Diags,
  DfBrCssTokKind     EndKind,
  i32               *OutBlockKind
  )
{
  DfBrCssCv     *Head = DFBR_NULL;
  DfBrCssCv     *Tail = DFBR_NULL;
  DfBrCssToken   Tk;
  DfBrCssCv     *Node;

  if (OutBlockKind != DFBR_NULL) *OutBlockKind = 0;

  for (;;) {
    DfBrCssTokNext (T, &Tk);
    if (Tk.Kind == DfBrCssTokEof) {
      DfBrCssDiagEmit (Diags, A, Tk.Line, Tk.Column, "unclosed-block-at-eof");
      return Head;
    }
    if (Tk.Kind == EndKind) {
      return Head;
    }
    if (Tk.Kind == DfBrCssTokLParen) {
      Node = NewCv (A, DfBrCssCvKindFunction);    /* anonymous paren group */
      Node->Str = DfBrCssArenaStrDup (A, "", 0);
      Node->Line = Tk.Line; Node->Column = Tk.Column;
      Node->Children = CollectBalanced (T, A, Diags, DfBrCssTokRParen, DFBR_NULL);
      AppendCv (&Head, &Tail, Node);
      continue;
    }
    if (Tk.Kind == DfBrCssTokLBracket) {
      Node = NewCv (A, DfBrCssCvKindBlock);
      Node->BlockKind = 0;
      Node->Line = Tk.Line; Node->Column = Tk.Column;
      Node->Children = CollectBalanced (T, A, Diags, DfBrCssTokRBracket, DFBR_NULL);
      AppendCv (&Head, &Tail, Node);
      continue;
    }
    if (Tk.Kind == DfBrCssTokLBrace) {
      Node = NewCv (A, DfBrCssCvKindBlock);
      Node->BlockKind = 1;
      Node->Line = Tk.Line; Node->Column = Tk.Column;
      Node->Children = CollectBalanced (T, A, Diags, DfBrCssTokRBrace, DFBR_NULL);
      AppendCv (&Head, &Tail, Node);
      continue;
    }
    if (Tk.Kind == DfBrCssTokFunction) {
      Node = NewCv (A, DfBrCssCvKindFunction);
      Node->Str = Tk.Str; Node->StrLen = Tk.StrLen;
      Node->Line = Tk.Line; Node->Column = Tk.Column;
      Node->Children = CollectBalanced (T, A, Diags, DfBrCssTokRParen, DFBR_NULL);
      AppendCv (&Head, &Tail, Node);
      continue;
    }
    Node = CvFromToken (A, &Tk);
    if (Node != DFBR_NULL) AppendCv (&Head, &Tail, Node);
  }
}

/* Trim leading/trailing whitespace CVs. */
DfBrCssCv *
DfBrCssCvTrim (DfBrCssCv *Head)
{
  DfBrCssCv *H = Head;
  DfBrCssCv *Prev;
  DfBrCssCv *N;

  while (H != DFBR_NULL && H->Kind == DfBrCssCvKindWhitespace) {
    H = H->Next;
  }
  if (H == DFBR_NULL) return DFBR_NULL;
  Prev = DFBR_NULL;
  for (N = H; N != DFBR_NULL; N = N->Next) {
    if (N->Kind != DfBrCssCvKindWhitespace) {
      Prev = N;
    }
  }
  if (Prev != DFBR_NULL) Prev->Next = DFBR_NULL;
  return H;
}

/* ------------------------------------------------------------------
 *  Public parsers
 * -----------------------------------------------------------------*/

/* Read a CV list until a top-level semicolon, `}`, or EOF. Stop
 * token kind is reported via *OutStop. Used by the declaration-block
 * parser for per-declaration value reads. */
DfBrCssCv *
DfBrCssParseCvList (
  DfBrCssTokenizer  *T,
  DfBrCssArena      *A,
  DfBrCssDiagList   *Diags,
  DfBrCssTokKind    *OutStop
  )
{
  DfBrCssCv     *Head = DFBR_NULL;
  DfBrCssCv     *Tail = DFBR_NULL;
  DfBrCssToken   Tk;
  DfBrCssCv     *Node;

  for (;;) {
    DfBrCssTokNext (T, &Tk);
    if (Tk.Kind == DfBrCssTokEof
        || Tk.Kind == DfBrCssTokSemicolon
        || Tk.Kind == DfBrCssTokRBrace) {
      if (OutStop != DFBR_NULL) *OutStop = Tk.Kind;
      return Head;
    }
    if (Tk.Kind == DfBrCssTokLParen) {
      Node = NewCv (A, DfBrCssCvKindFunction);
      Node->Str = DfBrCssArenaStrDup (A, "", 0);
      Node->Line = Tk.Line; Node->Column = Tk.Column;
      Node->Children = CollectBalanced (T, A, Diags, DfBrCssTokRParen, DFBR_NULL);
      AppendCv (&Head, &Tail, Node);
      continue;
    }
    if (Tk.Kind == DfBrCssTokLBracket) {
      Node = NewCv (A, DfBrCssCvKindBlock);
      Node->BlockKind = 0;
      Node->Line = Tk.Line; Node->Column = Tk.Column;
      Node->Children = CollectBalanced (T, A, Diags, DfBrCssTokRBracket, DFBR_NULL);
      AppendCv (&Head, &Tail, Node);
      continue;
    }
    if (Tk.Kind == DfBrCssTokLBrace) {
      Node = NewCv (A, DfBrCssCvKindBlock);
      Node->BlockKind = 1;
      Node->Line = Tk.Line; Node->Column = Tk.Column;
      Node->Children = CollectBalanced (T, A, Diags, DfBrCssTokRBrace, DFBR_NULL);
      AppendCv (&Head, &Tail, Node);
      continue;
    }
    if (Tk.Kind == DfBrCssTokFunction) {
      Node = NewCv (A, DfBrCssCvKindFunction);
      Node->Str = Tk.Str; Node->StrLen = Tk.StrLen;
      Node->Line = Tk.Line; Node->Column = Tk.Column;
      Node->Children = CollectBalanced (T, A, Diags, DfBrCssTokRParen, DFBR_NULL);
      AppendCv (&Head, &Tail, Node);
      continue;
    }
    Node = CvFromToken (A, &Tk);
    if (Node != DFBR_NULL) AppendCv (&Head, &Tail, Node);
  }
}

/* ------------------------------------------------------------------
 *  !important detection
 *
 *  Spec: a trailing  `! important`  (with optional whitespace after `!`)
 *  just before the end of a declaration's value marks it important.
 *  If found, we trim both CVs off the value list.
 * -----------------------------------------------------------------*/

static b32 StripImportant (DfBrCssCv **HeadRef) {
  DfBrCssCv  *H      = *HeadRef;
  DfBrCssCv  *Prev   = DFBR_NULL;
  DfBrCssCv  *Bang   = DFBR_NULL;
  DfBrCssCv  *BangP  = DFBR_NULL;
  DfBrCssCv  *Imp    = DFBR_NULL;
  DfBrCssCv  *ImpP   = DFBR_NULL;
  DfBrCssCv  *N;

  /* Walk; track the last non-whitespace CV and its predecessor. */
  for (N = H; N != DFBR_NULL; N = N->Next) {
    if (N->Kind == DfBrCssCvKindWhitespace) continue;
    BangP = ImpP;
    Bang  = Imp;
    ImpP  = Prev;
    Imp   = N;
    Prev  = N;
  }
  if (Imp == DFBR_NULL || Bang == DFBR_NULL) return DFBR_FALSE;
  if (!(Bang->Kind == DfBrCssCvKindDelim &&
        Bang->StrLen == 1 && Bang->Str[0] == '!')) return DFBR_FALSE;
  if (!(Imp->Kind == DfBrCssCvKindIdent &&
        DfBrCssAsciiEqZ (Imp->Str, Imp->StrLen, "important"))) return DFBR_FALSE;

  /* Cut the list at BangP (the CV before the `!`). */
  if (BangP != DFBR_NULL) {
    BangP->Next = DFBR_NULL;
  } else {
    *HeadRef = DFBR_NULL;
  }
  *HeadRef = DfBrCssCvTrim (*HeadRef);
  return DFBR_TRUE;
}

/* ------------------------------------------------------------------
 *  Declaration block parsing
 * -----------------------------------------------------------------*/

DfBrCssDeclaration *
DfBrCssParseDeclarationBlock (
  DfBrCssTokenizer  *T,
  DfBrCssArena      *A,
  DfBrCssDiagList   *Diags
  )
{
  DfBrCssDeclaration  *Head = DFBR_NULL;
  DfBrCssDeclaration  *Tail = DFBR_NULL;

  for (;;) {
    DfBrCssToken Tk;
    DfBrCssTokNext (T, &Tk);
    if (Tk.Kind == DfBrCssTokEof || Tk.Kind == DfBrCssTokRBrace) {
      return Head;
    }
    if (Tk.Kind == DfBrCssTokWhitespace || Tk.Kind == DfBrCssTokSemicolon) {
      continue;
    }
    if (Tk.Kind == DfBrCssTokAtKeyword) {
      /* Nested at-rule in a declaration block (e.g. @media inside
       * a future @supports). For now we just skip its prelude and
       * body and continue. */
      DfBrCssTokKind Stop = DfBrCssTokEof;
      (void)DfBrCssParseCvList (T, A, Diags, &Stop);
      if (Stop == DfBrCssTokRBrace) return Head;
      continue;
    }
    if (Tk.Kind != DfBrCssTokIdent) {
      /* Malformed declaration - skip to next semicolon / block end. */
      DfBrCssTokKind Stop = DfBrCssTokEof;
      DfBrCssDiagEmit (Diags, A, Tk.Line, Tk.Column,
                       "bad-declaration-start");
      (void)DfBrCssParseCvList (T, A, Diags, &Stop);
      if (Stop == DfBrCssTokRBrace) return Head;
      continue;
    }

    /* Got an ident. Expect whitespace / colon next. */
    {
      const char *PropName   = Tk.Str;
      usize       PropLen    = Tk.StrLen;
      i32         PropLine   = Tk.Line;
      i32         PropCol    = Tk.Column;
      DfBrCssToken  Peek;
      DfBrCssCv    *Value;
      DfBrCssTokKind Stop = DfBrCssTokEof;
      b32           Important;
      DfBrCssDeclaration *Decl;

      /* Absorb whitespace, then the colon. */
      do {
        DfBrCssTokNext (T, &Peek);
      } while (Peek.Kind == DfBrCssTokWhitespace);
      if (Peek.Kind != DfBrCssTokColon) {
        DfBrCssDiagEmit (Diags, A, Peek.Line, Peek.Column,
                         "missing-colon-in-declaration");
        /* Skip to next `;` / `}` to resync. */
        if (Peek.Kind != DfBrCssTokSemicolon &&
            Peek.Kind != DfBrCssTokRBrace &&
            Peek.Kind != DfBrCssTokEof) {
          (void)DfBrCssParseCvList (T, A, Diags, &Stop);
          if (Stop == DfBrCssTokRBrace) return Head;
        } else if (Peek.Kind == DfBrCssTokRBrace || Peek.Kind == DfBrCssTokEof) {
          return Head;
        }
        continue;
      }

      Value = DfBrCssParseCvList (T, A, Diags, &Stop);
      Value = DfBrCssCvTrim (Value);
      Important = StripImportant (&Value);

      Decl = (DfBrCssDeclaration *)DfBrCssArenaZero (A, sizeof (DfBrCssDeclaration));
      if (Decl == DFBR_NULL) return Head;
      Decl->Name      = DfBrCssArenaStrDupLower (A, PropName, PropLen);
      Decl->NameLen   = PropLen;
      Decl->Prop      = DfBrCssPropFromName (Decl->Name, Decl->NameLen);
      Decl->Value     = Value;
      Decl->Important = Important;
      Decl->Line      = PropLine;
      Decl->Column    = PropCol;
      if (Tail == DFBR_NULL) {
        Head = Decl;
      } else {
        Tail->Next = Decl;
      }
      Tail = Decl;

      if (Stop == DfBrCssTokRBrace || Stop == DfBrCssTokEof) return Head;
    }
  }
}

/* ------------------------------------------------------------------
 *  Top-level rule reading
 * -----------------------------------------------------------------*/

static DfBrCssRule *ReadQualifiedRule (
  DfBrCssTokenizer  *T,
  DfBrCssArena      *A,
  DfBrCssDiagList   *Diags
  );

/* Read one at-rule. Returns NULL on EOF. */
static DfBrCssRule *ReadAtRule (
  DfBrCssTokenizer    *T,
  DfBrCssArena        *A,
  DfBrCssDiagList     *Diags,
  const DfBrCssToken  *AtTok
  )
{
  DfBrCssRule   *R;
  DfBrCssCv     *Head = DFBR_NULL;
  DfBrCssCv     *Tail = DFBR_NULL;
  DfBrCssToken   Tk;
  DfBrCssCv     *Node;

  R = (DfBrCssRule *)DfBrCssArenaZero (A, sizeof (DfBrCssRule));
  if (R == DFBR_NULL) return DFBR_NULL;
  R->Kind      = DFBR_CSS_RULE_AT;
  R->AtName    = DfBrCssArenaStrDupLower (A, AtTok->Str, AtTok->StrLen);
  R->AtNameLen = AtTok->StrLen;
  R->Line      = AtTok->Line;
  R->Column    = AtTok->Column;

  /* Prelude: up until `;` (which terminates the rule) or `{` (which
   * opens a body). */
  for (;;) {
    DfBrCssTokNext (T, &Tk);
    if (Tk.Kind == DfBrCssTokEof) {
      DfBrCssDiagEmit (Diags, A, Tk.Line, Tk.Column, "at-rule-eof");
      R->AtPrelude = DfBrCssCvTrim (Head);
      return R;
    }
    if (Tk.Kind == DfBrCssTokSemicolon) {
      R->AtPrelude = DfBrCssCvTrim (Head);
      return R;
    }
    if (Tk.Kind == DfBrCssTokLBrace) {
      /* Body: for @media / @supports / @keyframes we would recurse
       * into inner rules; we preserve the block content as a list
       * of top-level rules when the prelude name is a known block
       * at-rule, otherwise opaque. */
      R->AtPrelude = DfBrCssCvTrim (Head);
      if (DfBrCssAsciiEqZ (R->AtName, R->AtNameLen, "media") ||
          DfBrCssAsciiEqZ (R->AtName, R->AtNameLen, "supports") ||
          DfBrCssAsciiEqZ (R->AtName, R->AtNameLen, "document")) {
        /* Parse inner rules using the same qualified-rule / at-rule
         * parsers we use at the stylesheet top level, but stop as soon
         * as we see the matching `}`. Using the real selector parser
         * means rules inside @media blocks actually participate in
         * the cascade (previously their Selectors list was NULL and
         * every rule was silently dropped). */
        DfBrCssRule *InnerHead = DFBR_NULL;
        DfBrCssRule *InnerTail = DFBR_NULL;
        for (;;) {
          DfBrCssToken Peek;
          DfBrCssTokNext (T, &Peek);
          if (Peek.Kind == DfBrCssTokEof) {
            DfBrCssDiagEmit (Diags, A, Peek.Line, Peek.Column,
                             "at-rule-body-eof");
            break;
          }
          if (Peek.Kind == DfBrCssTokRBrace) break;
          if (Peek.Kind == DfBrCssTokWhitespace
              || Peek.Kind == DfBrCssTokCdo
              || Peek.Kind == DfBrCssTokCdc) continue;
          if (Peek.Kind == DfBrCssTokAtKeyword) {
            DfBrCssRule *Nested = ReadAtRule (T, A, Diags, &Peek);
            if (Nested != DFBR_NULL) {
              if (InnerTail == DFBR_NULL) InnerHead = Nested;
              else                        InnerTail->Next = Nested;
              InnerTail = Nested;
            }
            continue;
          }
          /* Qualified rule. Rewind the tokenizer so the selector
           * parser sees `Peek` as its first token (same trick used
           * by the top-level DfBrCssParseStylesheet loop). */
          {
            DfBrCssRule *Q;
            T->Pos    = (usize)(Peek.Start - T->Src);
            T->Line   = Peek.Line;
            T->Column = Peek.Column;
            Q = ReadQualifiedRule (T, A, Diags);
            if (Q != DFBR_NULL) {
              if (InnerTail == DFBR_NULL) InnerHead = Q;
              else                        InnerTail->Next = Q;
              InnerTail = Q;
            }
          }
        }
        R->InnerRules = InnerHead;
      } else {
        /* Opaque block body - collect it as a single block CV and
         * hang it off AtPrelude tail for debug / @font-face etc. */
        DfBrCssCv *Body = NewCv (A, DfBrCssCvKindBlock);
        Body->BlockKind = 1;
        Body->Children  = CollectBalanced (T, A, Diags, DfBrCssTokRBrace, DFBR_NULL);
        if (R->AtPrelude == DFBR_NULL) {
          R->AtPrelude = Body;
        } else {
          DfBrCssCv *P = R->AtPrelude;
          while (P->Next != DFBR_NULL) P = P->Next;
          P->Next = Body;
        }
      }
      return R;
    }

    /* Regular prelude tokens. */
    if (Tk.Kind == DfBrCssTokFunction) {
      Node = NewCv (A, DfBrCssCvKindFunction);
      Node->Str = Tk.Str; Node->StrLen = Tk.StrLen;
      Node->Children = CollectBalanced (T, A, Diags, DfBrCssTokRParen, DFBR_NULL);
      AppendCv (&Head, &Tail, Node);
      continue;
    }
    if (Tk.Kind == DfBrCssTokLBracket) {
      Node = NewCv (A, DfBrCssCvKindBlock);
      Node->BlockKind = 0;
      Node->Children = CollectBalanced (T, A, Diags, DfBrCssTokRBracket, DFBR_NULL);
      AppendCv (&Head, &Tail, Node);
      continue;
    }
    if (Tk.Kind == DfBrCssTokLParen) {
      Node = NewCv (A, DfBrCssCvKindFunction);
      Node->Str = DfBrCssArenaStrDup (A, "", 0);
      Node->Children = CollectBalanced (T, A, Diags, DfBrCssTokRParen, DFBR_NULL);
      AppendCv (&Head, &Tail, Node);
      continue;
    }
    Node = CvFromToken (A, &Tk);
    if (Node != DFBR_NULL) AppendCv (&Head, &Tail, Node);
  }
}

/* Read one qualified rule. The selector runs from the current
 * position up to the `{`; we tokenize it twice once it's captured -
 * once as a selector, once as the raw CV list for dumping. */
static DfBrCssRule *ReadQualifiedRule (
  DfBrCssTokenizer  *T,
  DfBrCssArena      *A,
  DfBrCssDiagList   *Diags
  )
{
  DfBrCssRule  *R;
  i32           StartLine = T->Line;
  i32           StartCol  = T->Column;

  R = (DfBrCssRule *)DfBrCssArenaZero (A, sizeof (DfBrCssRule));
  if (R == DFBR_NULL) return DFBR_NULL;
  R->Kind   = DFBR_CSS_RULE_QUALIFIED;
  R->Line   = StartLine;
  R->Column = StartCol;

  /* Selector parser consumes tokens up to `{` or EOF. */
  R->Selectors = DfBrCssParseSelectorList (T, A, Diags);
  /* DfBrCssParseSelectorList leaves the tokenizer positioned just
   * past the terminating `{` (if one was present). */

  R->FirstDecl = DfBrCssParseDeclarationBlock (T, A, Diags);
  return R;
}

/* Top-level stylesheet loop. */
void
DfBrCssParseStylesheet (
  DfBrCssStylesheet  *Sheet
  )
{
  DfBrCssTokenizer  T;
  DfBrCssRule      *Head = DFBR_NULL;
  DfBrCssRule      *Tail = DFBR_NULL;
  DfBrCssToken      Tk;
  DfBrCssRule      *R;

  DfBrCssTokInit (&T, Sheet->Source, Sheet->SourceLen, &Sheet->Arena, &Sheet->Diags);

  for (;;) {
    DfBrCssTokNext (&T, &Tk);
    if (Tk.Kind == DfBrCssTokEof) break;
    if (Tk.Kind == DfBrCssTokWhitespace
        || Tk.Kind == DfBrCssTokCdo
        || Tk.Kind == DfBrCssTokCdc) continue;
    if (Tk.Kind == DfBrCssTokAtKeyword) {
      R = ReadAtRule (&T, &Sheet->Arena, &Sheet->Diags, &Tk);
      if (R != DFBR_NULL) {
        if (Tail == DFBR_NULL) Head = R;
        else                   Tail->Next = R;
        Tail = R;
      }
      continue;
    }
    /* Rewind by re-feeding: we can't actually rewind the tokenizer
     * easily, so we hand off the first token to the selector parser
     * by seeding it. Simpler: the selector parser in DfBrCssSelector.c
     * starts by reading tokens, and we pass the current token as a
     * "pushback". But we don't have pushback plumbing, so we just
     * position the tokenizer at the start of Tk and let the selector
     * parser re-read. */
    T.Pos    = (usize)(Tk.Start - T.Src);
    T.Line   = Tk.Line;
    T.Column = Tk.Column;

    R = ReadQualifiedRule (&T, &Sheet->Arena, &Sheet->Diags);
    if (R != DFBR_NULL) {
      if (Tail == DFBR_NULL) Head = R;
      else                   Tail->Next = R;
      Tail = R;
    }
  }
  Sheet->FirstRule = Head;
}

/* ------------------------------------------------------------------
 *  Public entry
 * -----------------------------------------------------------------*/

DfBrCssStylesheet *
DfBrCssParse (
  const char                 *Source,
  usize                       SourceLen,
  const DfBrCssParseOptions  *Opts
  )
{
  DfBrCssStylesheet  *S;
  DfBrCssParseOptions Options;

  memset (&Options, 0, sizeof (Options));
  Options.Origin              = DFBR_CSS_ORIGIN_AUTHOR;
  Options.KeepUnknownProps    = DFBR_TRUE;
  Options.EmitSourceLocations = DFBR_TRUE;
  if (Opts != DFBR_NULL) Options = *Opts;

  if (Source == DFBR_NULL) {
    Source = "";
    SourceLen = 0;
  }
  if (SourceLen == 0) {
    SourceLen = strlen (Source);
  }

  S = (DfBrCssStylesheet *)calloc (1, sizeof (DfBrCssStylesheet));
  if (S == DFBR_NULL) return DFBR_NULL;
  S->Options = Options;

  /* Copy source into the arena so the sheet owns its storage. */
  S->Source    = DfBrCssArenaStrDup (&S->Arena, Source, SourceLen);
  S->SourceLen = SourceLen;
  if (S->Source == DFBR_NULL) {
    DfBrCssArenaDispose (&S->Arena);
    free (S);
    return DFBR_NULL;
  }

  DfBrCssParseStylesheet (S);
  return S;
}

void
DfBrCssStylesheetDestroy (
  DfBrCssStylesheet  *S
  )
{
  if (S == DFBR_NULL) return;
  DfBrCssArenaDispose (&S->Arena);
  free (S);
}
