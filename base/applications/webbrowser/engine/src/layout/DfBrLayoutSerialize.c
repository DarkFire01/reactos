/** @file
  DfBrLayoutSerialize.c - indented dump of the box tree with
                          geometry. Used by the CLI and by tests.
**/

#include "DfBrLayoutInternal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  char  *Data;
  usize  Len;
  usize  Cap;
} Buf;

static void
BufAppend (Buf *B, const char *S, usize Len) {
  if (B->Len + Len + 1 > B->Cap) {
    usize  NewCap = (B->Cap == 0) ? 256 : B->Cap * 2;
    char  *New;
    while (NewCap < B->Len + Len + 1) NewCap *= 2;
    New = (char *)realloc (B->Data, NewCap);
    if (New == DFBR_NULL) return;
    B->Data = New;
    B->Cap  = NewCap;
  }
  if (Len > 0) memcpy (B->Data + B->Len, S, Len);
  B->Len += Len;
  B->Data[B->Len] = '\0';
}

static void
BufPrintf (Buf *B, const char *Fmt, ...) {
  char     Tmp[512];
  va_list  Ap;
  int      N;
  va_start (Ap, Fmt);
  N = vsnprintf (Tmp, sizeof (Tmp), Fmt, Ap);
  va_end (Ap);
  if (N < 0) return;
  if ((usize)N >= sizeof (Tmp)) N = (int)(sizeof (Tmp) - 1);
  BufAppend (B, Tmp, (usize)N);
}

static const char *
BoxKindName (DfBrLayoutBoxKind K) {
  switch (K) {
    case DFBR_LAYOUT_BOX_BLOCK:      return "block";
    case DFBR_LAYOUT_BOX_INLINE:     return "inline";
    case DFBR_LAYOUT_BOX_TEXT:       return "text";
    case DFBR_LAYOUT_BOX_ANON_BLOCK: return "anon-block";
    case DFBR_LAYOUT_BOX_REPLACED:   return "replaced";
    default:                         return "?";
  }
}

static void
DumpBox (Buf *B, const DfBrLayoutBox *Box, i32 Depth) {
  i32 I;
  for (I = 0; I < Depth; I++) BufAppend (B, "  ", 2);

  BufPrintf (B, "%s", BoxKindName (Box->Kind));
  if (Box->DomNode != DFBR_NULL && Box->DomNode->Kind == DFBR_HTML_NODE_ELEMENT &&
      Box->DomNode->Name != DFBR_NULL) {
    BufPrintf (B, " <%s>", Box->DomNode->Name);
  }

  BufPrintf (B, "  rect=(%.1f, %.1f, %.1fx%.1f)",
             (double)Box->ContentRect.X, (double)Box->ContentRect.Y,
             (double)Box->ContentRect.W, (double)Box->ContentRect.H);
  if (Box->Margin.Top != 0.0f || Box->Margin.Right != 0.0f ||
      Box->Margin.Bottom != 0.0f || Box->Margin.Left != 0.0f) {
    BufPrintf (B, " m=(%.1f %.1f %.1f %.1f)",
               (double)Box->Margin.Top, (double)Box->Margin.Right,
               (double)Box->Margin.Bottom, (double)Box->Margin.Left);
  }
  if (Box->Padding.Top != 0.0f || Box->Padding.Right != 0.0f ||
      Box->Padding.Bottom != 0.0f || Box->Padding.Left != 0.0f) {
    BufPrintf (B, " p=(%.1f %.1f %.1f %.1f)",
               (double)Box->Padding.Top, (double)Box->Padding.Right,
               (double)Box->Padding.Bottom, (double)Box->Padding.Left);
  }
  if (Box->Border.Top != 0.0f || Box->Border.Right != 0.0f ||
      Box->Border.Bottom != 0.0f || Box->Border.Left != 0.0f) {
    BufPrintf (B, " b=(%.1f %.1f %.1f %.1f)",
               (double)Box->Border.Top, (double)Box->Border.Right,
               (double)Box->Border.Bottom, (double)Box->Border.Left);
  }

  if (Box->Kind == DFBR_LAYOUT_BOX_TEXT && Box->TextData != DFBR_NULL) {
    /* Trim text preview to 40 chars, escaping newlines. */
    usize L = Box->TextLen;
    usize K;
    BufAppend (B, " \"", 2);
    for (K = 0; K < L && K < 40; K++) {
      char C = Box->TextData[K];
      if (C == '\n') { BufAppend (B, "\\n", 2); }
      else           { BufAppend (B, &C, 1); }
    }
    if (L > 40) BufAppend (B, "...", 3);
    BufAppend (B, "\"", 1);
  }

  BufAppend (B, "\n", 1);

  {
    const DfBrLayoutBox *C;
    for (C = Box->FirstChild; C != DFBR_NULL; C = C->NextSibling) {
      DumpBox (B, C, Depth + 1);
    }
  }
}

char *
DfBrLayoutDump (const DfBrLayoutEngine *E, usize *OutLen) {
  Buf B;
  memset (&B, 0, sizeof (B));
  if (E == DFBR_NULL || E->Root == DFBR_NULL) {
    if (OutLen != DFBR_NULL) *OutLen = 0;
    return DFBR_NULL;
  }
  DumpBox (&B, E->Root, 0);
  if (OutLen != DFBR_NULL) *OutLen = B.Len;
  return B.Data;
}

void
DfBrLayoutFreeDump (char *S) {
  free (S);
}
