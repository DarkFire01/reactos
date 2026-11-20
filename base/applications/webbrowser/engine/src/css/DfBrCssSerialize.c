/** @file
  DfBrCssSerialize.c - debug dumps for CSS stylesheets, computed
  styles, and per-document cascade results.

  Output is diagnostic, not a round-trippable serializer: it's shaped
  for humans reading the console and for snapshot tests. A future
  CSS-OM grammar-correct serializer can live alongside without
  interfering with these helpers.
**/

#include "DfBrCssInternal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------
 *  Growable buffer
 * -----------------------------------------------------------------*/

typedef struct {
  char  *Data;
  usize  Len;
  usize  Cap;
  b32    Oom;
} Buf;

static void
BufInit (Buf *B) { memset (B, 0, sizeof (*B)); }

static void
BufReserve (Buf *B, usize Extra) {
  usize Need = B->Len + Extra + 1;
  usize NewCap;
  char *NB;
  if (B->Oom || Need <= B->Cap) return;
  NewCap = (B->Cap == 0) ? 256 : B->Cap * 2;
  while (NewCap < Need) NewCap *= 2;
  NB = (char *)realloc (B->Data, NewCap);
  if (NB == DFBR_NULL) { B->Oom = DFBR_TRUE; return; }
  B->Data = NB;
  B->Cap  = NewCap;
}

static void
BufAppend (Buf *B, const char *S, usize N) {
  BufReserve (B, N);
  if (B->Oom) return;
  memcpy (B->Data + B->Len, S, N);
  B->Len += N;
  B->Data[B->Len] = '\0';
}

static void
BufAppendZ (Buf *B, const char *S) { BufAppend (B, S, strlen (S)); }

static void
BufAppendF (Buf *B, const char *Fmt, ...) {
  char    Tmp[256];
  va_list Ap;
  int     N;
  va_start (Ap, Fmt);
  N = vsnprintf (Tmp, sizeof (Tmp), Fmt, Ap);
  va_end (Ap);
  if (N < 0) return;
  if ((usize)N < sizeof (Tmp)) { BufAppend (B, Tmp, (usize)N); return; }
  /* Large format - allocate a big scratch. */
  {
    char *Heap = (char *)malloc ((usize)N + 1);
    if (Heap == DFBR_NULL) { B->Oom = DFBR_TRUE; return; }
    va_start (Ap, Fmt);
    vsnprintf (Heap, (usize)N + 1, Fmt, Ap);
    va_end (Ap);
    BufAppend (B, Heap, (usize)N);
    free (Heap);
  }
}

/* ------------------------------------------------------------------
 *  Selector dump
 * -----------------------------------------------------------------*/

static void
DumpSimple (const DfBrCssSimpleSel *S, Buf *B) {
  switch (S->Kind) {
    case DfBrCssSelUniversal: BufAppendZ (B, "*"); break;
    case DfBrCssSelType:      BufAppend  (B, S->Name, S->NameLen); break;
    case DfBrCssSelId:        BufAppendZ (B, "#"); BufAppend (B, S->Name, S->NameLen); break;
    case DfBrCssSelClass:     BufAppendZ (B, "."); BufAppend (B, S->Name, S->NameLen); break;
    case DfBrCssSelAttr: {
      BufAppendZ (B, "[");
      BufAppend  (B, S->Name, S->NameLen);
      switch (S->AttrOp) {
        case DfBrCssAttrExists:    break;
        case DfBrCssAttrEq:        BufAppendZ (B, "="); break;
        case DfBrCssAttrIncludes:  BufAppendZ (B, "~="); break;
        case DfBrCssAttrDashMatch: BufAppendZ (B, "|="); break;
        case DfBrCssAttrPrefix:    BufAppendZ (B, "^="); break;
        case DfBrCssAttrSuffix:    BufAppendZ (B, "$="); break;
        case DfBrCssAttrSubstring: BufAppendZ (B, "*="); break;
      }
      if (S->AttrOp != DfBrCssAttrExists) {
        BufAppendZ (B, "\"");
        BufAppend  (B, S->AttrValue, S->AttrValueLen);
        BufAppendZ (B, "\"");
        if (S->AttrIgnoreCase) BufAppendZ (B, " i");
      }
      BufAppendZ (B, "]");
      break;
    }
    case DfBrCssSelPseudoClass:
      BufAppendZ (B, ":");
      BufAppend  (B, S->Name, S->NameLen);
      break;
    case DfBrCssSelPseudoElement:
      BufAppendZ (B, "::");
      BufAppend  (B, S->Name, S->NameLen);
      break;
  }
}

static void
DumpCompound (const DfBrCssCompound *C, Buf *B) {
  const DfBrCssSimpleSel *S;
  for (S = C->Simples; S != DFBR_NULL; S = S->Next) DumpSimple (S, B);
}

static void
DumpComplex (const DfBrCssComplex *Cx, Buf *B) {
  const DfBrCssCompound *C;
  for (C = Cx->First; C != DFBR_NULL; C = C->Next) {
    DumpCompound (C, B);
    if (C->Next != DFBR_NULL) {
      switch (C->ComboToNext) {
        case DfBrCssCombDescendant: BufAppendZ (B, " ");   break;
        case DfBrCssCombChild:      BufAppendZ (B, " > "); break;
        case DfBrCssCombAdjacent:   BufAppendZ (B, " + "); break;
        case DfBrCssCombSibling:    BufAppendZ (B, " ~ "); break;
      }
    }
  }
}

void
DfBrCssDumpSelectorList (
  const DfBrCssSelectorList *SL,
  char                     **BufPtr,
  usize                     *Cap,
  usize                     *Len
  )
{
  Buf                    B;
  const DfBrCssComplex  *C;

  if (SL == DFBR_NULL) return;
  BufInit (&B);
  if (BufPtr != DFBR_NULL && *BufPtr != DFBR_NULL) {
    B.Data = *BufPtr; B.Len = *Len; B.Cap = *Cap;
  }
  for (C = SL->First; C != DFBR_NULL; C = C->Next) {
    if (C != SL->First) BufAppendZ (&B, ", ");
    DumpComplex (C, &B);
  }
  if (BufPtr != DFBR_NULL) { *BufPtr = B.Data; *Cap = B.Cap; *Len = B.Len; }
}

/* ------------------------------------------------------------------
 *  Component-value dump
 * -----------------------------------------------------------------*/

static void
DumpCv (const DfBrCssCv *Cv, Buf *B) {
  const DfBrCssCv *P;
  for (P = Cv; P != DFBR_NULL; P = P->Next) {
    switch (P->Kind) {
      case DfBrCssCvKindIdent:      BufAppend  (B, P->Str, P->StrLen); break;
      case DfBrCssCvKindFunction:
        BufAppend  (B, P->Str, P->StrLen); BufAppendZ (B, "(");
        DumpCv     (P->Children, B);       BufAppendZ (B, ")");
        break;
      case DfBrCssCvKindAtKeyword:  BufAppendZ (B, "@"); BufAppend (B, P->Str, P->StrLen); break;
      case DfBrCssCvKindHash:       BufAppendZ (B, "#"); BufAppend (B, P->Str, P->StrLen); break;
      case DfBrCssCvKindString:     BufAppendZ (B, "\""); BufAppend (B, P->Str, P->StrLen); BufAppendZ (B, "\""); break;
      case DfBrCssCvKindUrl:        BufAppendZ (B, "url(\""); BufAppend (B, P->Str, P->StrLen); BufAppendZ (B, "\")"); break;
      case DfBrCssCvKindNumber:     BufAppendF (B, "%g", P->Number); break;
      case DfBrCssCvKindPercentage: BufAppendF (B, "%g%%", P->Number); break;
      case DfBrCssCvKindDimension:
        BufAppendF (B, "%g", P->Number);
        BufAppend  (B, P->Unit, P->UnitLen);
        break;
      case DfBrCssCvKindDelim:      BufAppend  (B, P->Str, P->StrLen); break;
      case DfBrCssCvKindColon:      BufAppendZ (B, ":"); break;
      case DfBrCssCvKindComma:      BufAppendZ (B, ", "); break;
      case DfBrCssCvKindWhitespace: BufAppendZ (B, " "); break;
      case DfBrCssCvKindBlock:
        BufAppendZ (B, (P->BlockKind == 0) ? "[" : "{");
        DumpCv     (P->Children, B);
        BufAppendZ (B, (P->BlockKind == 0) ? "]" : "}");
        break;
      case DfBrCssCvKindInclude:       BufAppendZ (B, "~="); break;
      case DfBrCssCvKindDashMatch:     BufAppendZ (B, "|="); break;
      case DfBrCssCvKindPrefixMatch:   BufAppendZ (B, "^="); break;
      case DfBrCssCvKindSuffixMatch:   BufAppendZ (B, "$="); break;
      case DfBrCssCvKindSubstringMatch: BufAppendZ (B, "*="); break;
      case DfBrCssCvKindColumn:        BufAppendZ (B, "||"); break;
    }
    if (P->Next != DFBR_NULL && P->Kind != DfBrCssCvKindWhitespace &&
        P->Next->Kind != DfBrCssCvKindWhitespace &&
        P->Next->Kind != DfBrCssCvKindComma)
    {
      BufAppendZ (B, " ");
    }
  }
}

void
DfBrCssDumpCv (
  const DfBrCssCv *Cv,
  char           **BufPtr,
  usize           *Cap,
  usize           *Len
  )
{
  Buf B;
  BufInit (&B);
  if (BufPtr != DFBR_NULL && *BufPtr != DFBR_NULL) {
    B.Data = *BufPtr; B.Len = *Len; B.Cap = *Cap;
  }
  DumpCv (Cv, &B);
  if (BufPtr != DFBR_NULL) { *BufPtr = B.Data; *Cap = B.Cap; *Len = B.Len; }
}

/* ------------------------------------------------------------------
 *  Rule / declaration / stylesheet dump
 * -----------------------------------------------------------------*/

static void
DumpDecl (const DfBrCssDeclaration *D, Buf *B) {
  BufAppendZ (B, "  ");
  BufAppend  (B, D->Name, D->NameLen);
  BufAppendZ (B, ": ");
  DumpCv     (D->Value, B);
  if (D->Important) BufAppendZ (B, " !important");
  BufAppendZ (B, ";\n");
}

static void
DumpRule (const DfBrCssRule *R, Buf *B) {
  if (R->Kind == DFBR_CSS_RULE_QUALIFIED) {
    if (R->Selectors != DFBR_NULL) {
      const DfBrCssComplex *C;
      for (C = R->Selectors->First; C != DFBR_NULL; C = C->Next) {
        if (C != R->Selectors->First) BufAppendZ (B, ", ");
        DumpComplex (C, B);
      }
    }
    BufAppendZ (B, " {\n");
    {
      const DfBrCssDeclaration *D;
      for (D = R->FirstDecl; D != DFBR_NULL; D = D->Next) DumpDecl (D, B);
    }
    BufAppendZ (B, "}\n");
  } else {
    BufAppendZ (B, "@");
    BufAppend  (B, R->AtName, R->AtNameLen);
    if (R->AtPrelude != DFBR_NULL) { BufAppendZ (B, " "); DumpCv (R->AtPrelude, B); }
    if (R->InnerRules != DFBR_NULL) {
      const DfBrCssRule *Ir;
      BufAppendZ (B, " {\n");
      for (Ir = R->InnerRules; Ir != DFBR_NULL; Ir = Ir->Next) DumpRule (Ir, B);
      BufAppendZ (B, "}\n");
    } else {
      BufAppendZ (B, ";\n");
    }
  }
}

char *
DfBrCssStylesheetDump (const DfBrCssStylesheet *S, usize *OutLen) {
  Buf B;
  const DfBrCssRule *R;
  if (S == DFBR_NULL) return DFBR_NULL;
  BufInit (&B);
  for (R = S->FirstRule; R != DFBR_NULL; R = R->Next) DumpRule (R, &B);
  if (OutLen != DFBR_NULL) *OutLen = B.Len;
  return B.Data;
}

/* ------------------------------------------------------------------
 *  Computed style dump
 * -----------------------------------------------------------------*/

static const char *DisplayName (DfBrCssDisplay D) {
  switch (D) {
    case DFBR_CSS_DISPLAY_NONE:                 return "none";
    case DFBR_CSS_DISPLAY_BLOCK:                return "block";
    case DFBR_CSS_DISPLAY_INLINE:               return "inline";
    case DFBR_CSS_DISPLAY_INLINE_BLOCK:         return "inline-block";
    case DFBR_CSS_DISPLAY_LIST_ITEM:            return "list-item";
    case DFBR_CSS_DISPLAY_FLEX:                 return "flex";
    case DFBR_CSS_DISPLAY_INLINE_FLEX:          return "inline-flex";
    case DFBR_CSS_DISPLAY_GRID:                 return "grid";
    case DFBR_CSS_DISPLAY_TABLE:                return "table";
    case DFBR_CSS_DISPLAY_INLINE_TABLE:         return "inline-table";
    case DFBR_CSS_DISPLAY_TABLE_ROW:            return "table-row";
    case DFBR_CSS_DISPLAY_TABLE_CELL:           return "table-cell";
    case DFBR_CSS_DISPLAY_TABLE_ROW_GROUP:      return "table-row-group";
    case DFBR_CSS_DISPLAY_TABLE_HEADER_GROUP:   return "table-header-group";
    case DFBR_CSS_DISPLAY_TABLE_FOOTER_GROUP:   return "table-footer-group";
    case DFBR_CSS_DISPLAY_TABLE_COLUMN:         return "table-column";
    case DFBR_CSS_DISPLAY_TABLE_COLUMN_GROUP:   return "table-column-group";
    case DFBR_CSS_DISPLAY_TABLE_CAPTION:        return "table-caption";
    case DFBR_CSS_DISPLAY_RUN_IN:               return "run-in";
    case DFBR_CSS_DISPLAY_CONTENTS:             return "contents";
  }
  return "?";
}

static const char *PositionName (DfBrCssPosition P) {
  switch (P) {
    case DFBR_CSS_POSITION_STATIC:   return "static";
    case DFBR_CSS_POSITION_RELATIVE: return "relative";
    case DFBR_CSS_POSITION_ABSOLUTE: return "absolute";
    case DFBR_CSS_POSITION_FIXED:    return "fixed";
    case DFBR_CSS_POSITION_STICKY:   return "sticky";
  }
  return "?";
}

static const char *UnitName (DfBrCssUnit U) {
  switch (U) {
    case DFBR_CSS_UNIT_NONE:    return "";
    case DFBR_CSS_UNIT_PX:      return "px";
    case DFBR_CSS_UNIT_PT:      return "pt";
    case DFBR_CSS_UNIT_PC:      return "pc";
    case DFBR_CSS_UNIT_CM:      return "cm";
    case DFBR_CSS_UNIT_MM:      return "mm";
    case DFBR_CSS_UNIT_Q:       return "q";
    case DFBR_CSS_UNIT_IN:      return "in";
    case DFBR_CSS_UNIT_EM:      return "em";
    case DFBR_CSS_UNIT_REM:     return "rem";
    case DFBR_CSS_UNIT_EX:      return "ex";
    case DFBR_CSS_UNIT_CH:      return "ch";
    case DFBR_CSS_UNIT_VW:      return "vw";
    case DFBR_CSS_UNIT_VH:      return "vh";
    case DFBR_CSS_UNIT_VMIN:    return "vmin";
    case DFBR_CSS_UNIT_VMAX:    return "vmax";
    case DFBR_CSS_UNIT_PERCENT: return "%";
    default:                    return "?";
  }
}

static void
DumpLen (Buf *B, const DfBrCssLength *L) {
  if (L->Auto) { BufAppendZ (B, "auto"); return; }
  BufAppendF (B, "%g%s", L->Value, UnitName (L->Unit));
}

static void
DumpColor (Buf *B, const DfBrCssColor *C) {
  if (C->A < 0.999f) BufAppendF (B, "rgba(%u,%u,%u,%g)", C->R, C->G, C->B, C->A);
  else               BufAppendF (B, "#%02x%02x%02x",    C->R, C->G, C->B);
}

char *
DfBrCssComputedStyleDump (const DfBrCssComputedStyle *Cs, usize *OutLen) {
  Buf B;
  if (Cs == DFBR_NULL) return DFBR_NULL;
  BufInit (&B);

  BufAppendZ (&B, "display: ");  BufAppendZ (&B, DisplayName (Cs->Display)); BufAppendZ (&B, "; ");
  BufAppendZ (&B, "position: "); BufAppendZ (&B, PositionName (Cs->Position)); BufAppendZ (&B, "; ");
  BufAppendZ (&B, "color: ");          DumpColor (&B, &Cs->Color);          BufAppendZ (&B, "; ");
  BufAppendZ (&B, "background-color: "); DumpColor (&B, &Cs->BackgroundColor); BufAppendZ (&B, "; ");
  BufAppendZ (&B, "font-size: ");      DumpLen   (&B, &Cs->FontSize);       BufAppendZ (&B, "; ");
  BufAppendF (&B, "font-weight: %d; ", Cs->FontWeight);
  BufAppendZ (&B, "width: ");          DumpLen (&B, &Cs->Width);            BufAppendZ (&B, "; ");
  BufAppendZ (&B, "height: ");         DumpLen (&B, &Cs->Height);           BufAppendZ (&B, "; ");
  BufAppendZ (&B, "margin: ");         DumpLen (&B, &Cs->MarginTop);   BufAppendZ (&B, " ");
                                       DumpLen (&B, &Cs->MarginRight); BufAppendZ (&B, " ");
                                       DumpLen (&B, &Cs->MarginBottom); BufAppendZ (&B, " ");
                                       DumpLen (&B, &Cs->MarginLeft);   BufAppendZ (&B, "; ");
  BufAppendZ (&B, "padding: ");        DumpLen (&B, &Cs->PaddingTop);   BufAppendZ (&B, " ");
                                       DumpLen (&B, &Cs->PaddingRight); BufAppendZ (&B, " ");
                                       DumpLen (&B, &Cs->PaddingBottom); BufAppendZ (&B, " ");
                                       DumpLen (&B, &Cs->PaddingLeft);   BufAppendZ (&B, "; ");

  if (OutLen != DFBR_NULL) *OutLen = B.Len;
  return B.Data;
}

/* ------------------------------------------------------------------
 *  Per-document style dump
 * -----------------------------------------------------------------*/

static void
DumpNodePath (const DfBrHtmlNode *N, Buf *B) {
  if (N == DFBR_NULL || N->Kind != DFBR_HTML_NODE_ELEMENT) return;
  if (N->Parent != DFBR_NULL && N->Parent->Kind == DFBR_HTML_NODE_ELEMENT) {
    DumpNodePath (N->Parent, B);
    BufAppendZ (B, " > ");
  }
  BufAppend (B, N->Name, N->NameLen);
  {
    const DfBrHtmlAttr *A = DfBrHtmlNodeGetAttr (N, "id");
    if (A != DFBR_NULL) {
      BufAppendZ (B, "#");
      BufAppend  (B, A->Value, A->ValueLen);
    }
  }
}

char *
DfBrCssStyleContextDump (DfBrCssStyleContext *Sc, usize *OutLen) {
  Buf                 B;
  const DfBrHtmlNode *Root;
  const DfBrHtmlNode *N;

  if (Sc == DFBR_NULL || Sc->Doc == DFBR_NULL) return DFBR_NULL;
  BufInit (&B);

  Root = DfBrHtmlDocumentRoot (Sc->Doc);
  N    = DFBR_NULL;
  while ((N = DfBrHtmlNodeNextPreorder (Root, N)) != DFBR_NULL) {
    const DfBrCssComputedStyle *Cs;
    if (N->Kind != DFBR_HTML_NODE_ELEMENT) continue;
    Cs = (const DfBrCssComputedStyle *)N->StyleData;
    if (Cs == DFBR_NULL) continue;

    DumpNodePath (N, &B);
    BufAppendZ (&B, " { ");
    {
      char *Sub = DfBrCssComputedStyleDump (Cs, DFBR_NULL);
      if (Sub != DFBR_NULL) { BufAppendZ (&B, Sub); free (Sub); }
    }
    BufAppendZ (&B, "}\n");
  }
  if (OutLen != DFBR_NULL) *OutLen = B.Len;
  return B.Data;
}

void
DfBrCssFreeDump (char *S) { free (S); }
