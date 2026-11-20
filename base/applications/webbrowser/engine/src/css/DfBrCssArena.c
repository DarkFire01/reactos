/** @file
  DfBrCssArena.c - arena, diagnostics list, and a handful of shared
                   string helpers used by every CSS TU.

  Mirrors DfBrHtml's arena exactly so the two libraries have the
  same ownership story.
**/

#include "DfBrCssInternal.h"

#include <stdlib.h>
#include <string.h>

#define DFBR_CSS_ARENA_MIN_CHUNK  (64 * 1024)

/* ------------------------------------------------------------------
 *  Arena
 * -----------------------------------------------------------------*/

static DfBrCssArenaChunk *
NewChunk (
  usize  MinPayload
  )
{
  DfBrCssArenaChunk  *C;
  usize               Cap;

  Cap = MinPayload;
  if (Cap < DFBR_CSS_ARENA_MIN_CHUNK) {
    Cap = DFBR_CSS_ARENA_MIN_CHUNK;
  }

  C = (DfBrCssArenaChunk *)malloc (sizeof (DfBrCssArenaChunk) + Cap - 1);
  if (C == DFBR_NULL) {
    return DFBR_NULL;
  }
  C->Next = DFBR_NULL;
  C->Used = 0;
  C->Cap  = Cap;
  return C;
}

void *
DfBrCssArenaAlloc (
  DfBrCssArena  *A,
  usize          Size
  )
{
  DfBrCssArenaChunk  *C;
  usize               Aligned;
  void               *P;

  /* 8-byte alignment covers worst-case on 64-bit targets. */
  Aligned = (Size + 7U) & ~(usize)7U;

  if ((A->Current == DFBR_NULL) ||
      ((A->Current->Used + Aligned) > A->Current->Cap)) {
    C = NewChunk (Aligned);
    if (C == DFBR_NULL) {
      return DFBR_NULL;
    }
    if (A->First == DFBR_NULL) {
      A->First = C;
    } else {
      A->Current->Next = C;
    }
    A->Current = C;
  }

  P = &A->Current->Data[A->Current->Used];
  A->Current->Used += Aligned;
  A->TotalBytes    += Aligned;
  return P;
}

void *
DfBrCssArenaZero (
  DfBrCssArena  *A,
  usize          Size
  )
{
  void *P = DfBrCssArenaAlloc (A, Size);
  if (P != DFBR_NULL) {
    memset (P, 0, Size);
  }
  return P;
}

char *
DfBrCssArenaStrDup (
  DfBrCssArena  *A,
  const char    *S,
  usize          Len
  )
{
  char *Out = (char *)DfBrCssArenaAlloc (A, Len + 1);
  if (Out == DFBR_NULL) {
    return DFBR_NULL;
  }
  if (Len > 0) {
    memcpy (Out, S, Len);
  }
  Out[Len] = '\0';
  return Out;
}

char *
DfBrCssArenaStrDupLower (
  DfBrCssArena  *A,
  const char    *S,
  usize          Len
  )
{
  char   *Out = (char *)DfBrCssArenaAlloc (A, Len + 1);
  usize   i;
  char    C;

  if (Out == DFBR_NULL) {
    return DFBR_NULL;
  }
  for (i = 0; i < Len; i++) {
    C = S[i];
    if ((C >= 'A') && (C <= 'Z')) {
      C = (char)(C - 'A' + 'a');
    }
    Out[i] = C;
  }
  Out[Len] = '\0';
  return Out;
}

void
DfBrCssArenaDispose (
  DfBrCssArena  *A
  )
{
  DfBrCssArenaChunk  *C;
  DfBrCssArenaChunk  *N;

  for (C = A->First; C != DFBR_NULL; C = N) {
    N = C->Next;
    free (C);
  }
  A->First      = DFBR_NULL;
  A->Current    = DFBR_NULL;
  A->TotalBytes = 0;
}

/* ------------------------------------------------------------------
 *  Diagnostics
 * -----------------------------------------------------------------*/

void
DfBrCssDiagEmit (
  DfBrCssDiagList  *List,
  DfBrCssArena     *A,
  i32               Line,
  i32               Column,
  const char       *Message
  )
{
  usize              NewCap;
  DfBrCssDiagEntry  *New;

  if (List->Count == List->Cap) {
    NewCap = (List->Cap == 0) ? 8 : (List->Cap * 2);
    New    = (DfBrCssDiagEntry *)DfBrCssArenaAlloc (
               A, NewCap * sizeof (DfBrCssDiagEntry));
    if (New == DFBR_NULL) {
      return;
    }
    if (List->Entries != DFBR_NULL) {
      memcpy (New, List->Entries, List->Count * sizeof (DfBrCssDiagEntry));
    }
    List->Entries = New;
    List->Cap     = NewCap;
  }
  List->Entries[List->Count].Line    = Line;
  List->Entries[List->Count].Column  = Column;
  List->Entries[List->Count].Message =
    DfBrCssArenaStrDup (A, Message, strlen (Message));
  List->Count++;
}

/* ------------------------------------------------------------------
 *  ASCII case-insensitive compare
 * -----------------------------------------------------------------*/

b32
DfBrCssAsciiEqZ (
  const char  *A,
  usize        ALen,
  const char  *LitZ
  )
{
  usize  i = 0;
  char   x;
  char   y;

  while ((i < ALen) && (LitZ[i] != '\0')) {
    x = A[i];
    y = LitZ[i];
    if ((x >= 'A') && (x <= 'Z')) { x = (char)(x - 'A' + 'a'); }
    if ((y >= 'A') && (y <= 'Z')) { y = (char)(y - 'A' + 'a'); }
    if (x != y) {
      return DFBR_FALSE;
    }
    i++;
  }
  return ((i == ALen) && (LitZ[i] == '\0')) ? DFBR_TRUE : DFBR_FALSE;
}

/* ------------------------------------------------------------------
 *  Public: stylesheet diagnostic accessors
 * -----------------------------------------------------------------*/

usize
DfBrCssStylesheetDiagCount (
  const DfBrCssStylesheet  *S
  )
{
  return (S != DFBR_NULL) ? S->Diags.Count : 0;
}

const DfBrCssDiagEntry *
DfBrCssStylesheetDiag (
  const DfBrCssStylesheet  *S,
  usize                     Idx
  )
{
  if ((S == DFBR_NULL) || (Idx >= S->Diags.Count)) {
    return DFBR_NULL;
  }
  return &S->Diags.Entries[Idx];
}

DfBrCssOrigin
DfBrCssStylesheetOrigin (
  const DfBrCssStylesheet  *S
  )
{
  return (S != DFBR_NULL) ? S->Options.Origin : DFBR_CSS_ORIGIN_AUTHOR;
}

const DfBrCssRule *
DfBrCssStylesheetFirstRule (
  const DfBrCssStylesheet  *S
  )
{
  return (S != DFBR_NULL) ? S->FirstRule : DFBR_NULL;
}

const DfBrCssRule *
DfBrCssRuleNext (
  const DfBrCssRule  *R
  )
{
  return (R != DFBR_NULL) ? R->Next : DFBR_NULL;
}

DfBrCssRuleKind
DfBrCssRuleType (
  const DfBrCssRule  *R
  )
{
  return (R != DFBR_NULL) ? R->Kind : DFBR_CSS_RULE_QUALIFIED;
}

const DfBrCssSelectorList *
DfBrCssRuleSelectors (
  const DfBrCssRule  *R
  )
{
  return (R != DFBR_NULL) ? R->Selectors : DFBR_NULL;
}

const DfBrCssDeclaration *
DfBrCssRuleFirstDecl (
  const DfBrCssRule  *R
  )
{
  return (R != DFBR_NULL) ? R->FirstDecl : DFBR_NULL;
}

const char *
DfBrCssRuleAtName (
  const DfBrCssRule  *R,
  usize              *OutLen
  )
{
  if (R == DFBR_NULL) {
    if (OutLen != DFBR_NULL) *OutLen = 0;
    return DFBR_NULL;
  }
  if (OutLen != DFBR_NULL) *OutLen = R->AtNameLen;
  return R->AtName;
}

const DfBrCssCv *
DfBrCssRuleAtPrelude (
  const DfBrCssRule  *R
  )
{
  return (R != DFBR_NULL) ? R->AtPrelude : DFBR_NULL;
}

static char *
CvCopyStr (const DfBrCssCv *C) {
  char *Out;
  if (C == DFBR_NULL || C->Str == DFBR_NULL) return DFBR_NULL;
  Out = (char *)malloc (C->StrLen + 1);
  if (Out == DFBR_NULL) return DFBR_NULL;
  memcpy (Out, C->Str, C->StrLen);
  Out[C->StrLen] = '\0';
  return Out;
}

char *
DfBrCssRuleImportUrl (
  const DfBrCssRule  *R
  )
{
  const DfBrCssCv *P;
  /* Must be an at-rule named @import (case insensitive). */
  if (R == DFBR_NULL || R->Kind != DFBR_CSS_RULE_AT) return DFBR_NULL;
  if (!DfBrCssAsciiEqZ (R->AtName, R->AtNameLen, "import")) return DFBR_NULL;

  for (P = R->AtPrelude; P != DFBR_NULL; P = P->Next) {
    if (P->Kind == DfBrCssCvKindWhitespace) continue;
    if (P->Kind == DfBrCssCvKindString || P->Kind == DfBrCssCvKindUrl) {
      return CvCopyStr (P);
    }
    if (P->Kind == DfBrCssCvKindFunction && P->StrLen == 3
        && (P->Str[0] == 'u' || P->Str[0] == 'U')
        && (P->Str[1] == 'r' || P->Str[1] == 'R')
        && (P->Str[2] == 'l' || P->Str[2] == 'L'))
    {
      const DfBrCssCv *A;
      for (A = P->Children; A != DFBR_NULL; A = A->Next) {
        if (A->Kind == DfBrCssCvKindWhitespace) continue;
        if (A->Kind == DfBrCssCvKindString || A->Kind == DfBrCssCvKindUrl
            || A->Kind == DfBrCssCvKindIdent)
        {
          return CvCopyStr (A);
        }
        break;
      }
    }
    break; /* first non-whitespace was not a URL - malformed */
  }
  return DFBR_NULL;
}

const DfBrCssRule *
DfBrCssRuleInnerRules (
  const DfBrCssRule  *R
  )
{
  return (R != DFBR_NULL) ? R->InnerRules : DFBR_NULL;
}

DfBrCssProp
DfBrCssDeclProp (
  const DfBrCssDeclaration  *D
  )
{
  return (D != DFBR_NULL) ? D->Prop : DFBR_CSS_PROP_UNKNOWN;
}

const char *
DfBrCssDeclName (
  const DfBrCssDeclaration  *D,
  usize                     *OutLen
  )
{
  if (D == DFBR_NULL) {
    if (OutLen != DFBR_NULL) *OutLen = 0;
    return DFBR_NULL;
  }
  if (OutLen != DFBR_NULL) *OutLen = D->NameLen;
  return D->Name;
}

const DfBrCssCv *
DfBrCssDeclValue (
  const DfBrCssDeclaration  *D
  )
{
  return (D != DFBR_NULL) ? D->Value : DFBR_NULL;
}

b32
DfBrCssDeclImportant (
  const DfBrCssDeclaration  *D
  )
{
  return (D != DFBR_NULL) ? D->Important : DFBR_FALSE;
}

const DfBrCssDeclaration *
DfBrCssDeclNext (
  const DfBrCssDeclaration  *D
  )
{
  return (D != DFBR_NULL) ? D->Next : DFBR_NULL;
}
