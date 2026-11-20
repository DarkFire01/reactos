/** @file
  DfBrHtmlSerialize.c - DfBrHtmlSerialize() (valid HTML round-trip)
                        + DfBrHtmlDumpTree() (indented debug view).

  Both share a tiny string builder with exponential grow. Output is
  malloc-backed so callers can hand it off to write() / fwrite()
  without touching the document arena.
**/

#include "DfBrHtmlInternal.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------------
 *  String builder
 * -----------------------------------------------------------------*/

typedef struct {
  char   *Buf;
  usize   Len;
  usize   Cap;
  b32     Oom;
} Sb;

static void
SbReserve (Sb *S, usize Extra)
{
  usize  Need;
  usize  NewCap;
  char  *New;

  if (S->Oom) {
    return;
  }
  Need = S->Len + Extra + 1;
  if (Need <= S->Cap) {
    return;
  }
  NewCap = S->Cap ? S->Cap : 256;
  while (NewCap < Need) {
    NewCap *= 2;
  }
  New = (char *)realloc (S->Buf, NewCap);
  if (New == DFBR_NULL) {
    S->Oom = DFBR_TRUE;
    return;
  }
  S->Buf = New;
  S->Cap = NewCap;
}

static void
SbAppend (Sb *S, const char *Str, usize Len)
{
  SbReserve (S, Len);
  if (S->Oom) return;
  memcpy (&S->Buf[S->Len], Str, Len);
  S->Len += Len;
}

static void
SbAppendZ (Sb *S, const char *Str)
{
  SbAppend (S, Str, strlen (Str));
}

static void
SbAppendC (Sb *S, char C)
{
  SbReserve (S, 1);
  if (S->Oom) return;
  S->Buf[S->Len++] = C;
}

/* Escape for HTML text content: &, <, >. Attribute values additionally
 * escape the active quote; the caller toggles QuoteChar to '"' or '\''. */
static void
SbAppendEscaped (
  Sb          *S,
  const char  *Str,
  usize        Len,
  char         QuoteChar     /* 0 for text nodes, '"' or '\'' in attrs */
  )
{
  usize  i;
  char   C;

  for (i = 0; i < Len; i++) {
    C = Str[i];
    switch (C) {
      case '&':  SbAppendZ (S, "&amp;");  break;
      case '<':  SbAppendZ (S, "&lt;");   break;
      case '>':  SbAppendZ (S, "&gt;");   break;
      default:
        if (C == QuoteChar) {
          if (C == '"')  SbAppendZ (S, "&quot;");
          else           SbAppendZ (S, "&#39;");
        } else {
          SbAppendC (S, C);
        }
        break;
    }
  }
}

static char *
SbFinish (Sb *S, usize *OutLen)
{
  if (S->Oom) {
    free (S->Buf);
    if (OutLen) *OutLen = 0;
    return DFBR_NULL;
  }
  SbReserve (S, 1);
  if (S->Oom) {
    free (S->Buf);
    if (OutLen) *OutLen = 0;
    return DFBR_NULL;
  }
  S->Buf[S->Len] = '\0';
  if (OutLen) *OutLen = S->Len;
  return S->Buf;
}

/* ------------------------------------------------------------------
 *  HTML serializer
 *
 *  Opens tags with attributes, recurses children, closes unless the
 *  element is void. Raw-text element children (text inside <script>
 *  / <style>) are emitted verbatim to match HTML semantics - escaping
 *  `<` there would corrupt the source.
 * -----------------------------------------------------------------*/

static void
SerializeAttr (Sb *S, const DfBrHtmlAttr *A)
{
  SbAppendC (S, ' ');
  SbAppend  (S, A->Name, A->NameLen);
  if (A->HasValue) {
    SbAppendZ (S, "=\"");
    SbAppendEscaped (S, A->Value, A->ValueLen, '"');
    SbAppendC (S, '"');
  }
}

static void
SerializeNode (Sb *S, const DfBrHtmlNode *N)
{
  const DfBrHtmlAttr  *A;
  const DfBrHtmlNode  *C;
  b32                  IsRaw;

  switch (N->Kind) {

  case DFBR_HTML_NODE_DOCUMENT:
    for (C = N->FirstChild; C != DFBR_NULL; C = C->NextSibling) {
      SerializeNode (S, C);
    }
    break;

  case DFBR_HTML_NODE_DOCTYPE:
    SbAppendZ (S, "<!DOCTYPE ");
    SbAppend  (S, N->Text, N->TextLen);
    SbAppendC (S, '>');
    break;

  case DFBR_HTML_NODE_COMMENT:
    SbAppendZ (S, "<!--");
    SbAppend  (S, N->Text, N->TextLen);
    SbAppendZ (S, "-->");
    break;

  case DFBR_HTML_NODE_TEXT: {
    /* If parent is a raw-text element, emit verbatim. */
    b32 ParentIsRaw = DFBR_FALSE;
    if ((N->Parent != DFBR_NULL) &&
        (N->Parent->Kind == DFBR_HTML_NODE_ELEMENT)) {
      ParentIsRaw = DfBrHtmlTagIsRawText (N->Parent->Tag);
    }
    if (ParentIsRaw) {
      SbAppend (S, N->Text, N->TextLen);
    } else {
      SbAppendEscaped (S, N->Text, N->TextLen, 0);
    }
    break;
  }

  case DFBR_HTML_NODE_ELEMENT:
    SbAppendC (S, '<');
    SbAppend  (S, N->Name, N->NameLen);
    for (A = N->FirstAttr; A != DFBR_NULL; A = A->Next) {
      SerializeAttr (S, A);
    }
    SbAppendC (S, '>');

    if (DfBrHtmlTagIsVoid (N->Tag)) {
      break;
    }

    IsRaw = DfBrHtmlTagIsRawText (N->Tag);
    (void)IsRaw;   /* child text emit handles the raw case */

    for (C = N->FirstChild; C != DFBR_NULL; C = C->NextSibling) {
      SerializeNode (S, C);
    }
    SbAppendZ (S, "</");
    SbAppend  (S, N->Name, N->NameLen);
    SbAppendC (S, '>');
    break;
  }
}

char *
DfBrHtmlSerialize (
  const DfBrHtmlNode  *Node,
  usize               *OutLen
  )
{
  Sb  S;

  memset (&S, 0, sizeof (S));
  if (Node == DFBR_NULL) {
    if (OutLen != DFBR_NULL) *OutLen = 0;
    return DFBR_NULL;
  }
  SerializeNode (&S, Node);
  return SbFinish (&S, OutLen);
}

/* ------------------------------------------------------------------
 *  Debug tree dump
 *
 *  Each node on its own line, indented by depth. Intended to be
 *  consumed by humans and by grep-based tests, not to be read back.
 *  Format example:
 *
 *      #document
 *        <!DOCTYPE html>
 *        <html>
 *          <head>
 *            <title>
 *              "Hello"
 *          <body class="main">
 *            <p>
 *              "world"
 * -----------------------------------------------------------------*/

static void
Indent (Sb *S, int Depth)
{
  int  i;
  for (i = 0; i < Depth; i++) {
    SbAppendZ (S, "  ");
  }
}

static void
EscapeForDump (Sb *S, const char *Str, usize Len)
{
  usize  i;
  char   C;
  char   Buf[8];

  for (i = 0; i < Len; i++) {
    C = Str[i];
    switch (C) {
      case '\n': SbAppendZ (S, "\\n"); break;
      case '\r': SbAppendZ (S, "\\r"); break;
      case '\t': SbAppendZ (S, "\\t"); break;
      case '"':  SbAppendZ (S, "\\\""); break;
      case '\\': SbAppendZ (S, "\\\\"); break;
      default:
        if ((unsigned char)C < 0x20) {
          /* control char - emit \xHH */
          snprintf (Buf, sizeof (Buf), "\\x%02X", (unsigned char)C);
          SbAppendZ (S, Buf);
        } else {
          SbAppendC (S, C);
        }
        break;
    }
  }
}

static void
DumpNode (Sb *S, const DfBrHtmlNode *N, int Depth)
{
  const DfBrHtmlAttr  *A;
  const DfBrHtmlNode  *C;

  Indent (S, Depth);

  switch (N->Kind) {
  case DFBR_HTML_NODE_DOCUMENT:
    SbAppendZ (S, "#document\n");
    break;

  case DFBR_HTML_NODE_DOCTYPE:
    SbAppendZ (S, "<!DOCTYPE ");
    SbAppend  (S, N->Text, N->TextLen);
    SbAppendZ (S, ">\n");
    break;

  case DFBR_HTML_NODE_COMMENT:
    SbAppendZ (S, "<!--");
    EscapeForDump (S, N->Text, N->TextLen);
    SbAppendZ (S, "-->\n");
    break;

  case DFBR_HTML_NODE_TEXT:
    SbAppendC (S, '"');
    EscapeForDump (S, N->Text, N->TextLen);
    SbAppendZ (S, "\"\n");
    break;

  case DFBR_HTML_NODE_ELEMENT:
    SbAppendC (S, '<');
    SbAppend  (S, N->Name, N->NameLen);
    for (A = N->FirstAttr; A != DFBR_NULL; A = A->Next) {
      SbAppendC (S, ' ');
      SbAppend  (S, A->Name, A->NameLen);
      if (A->HasValue) {
        SbAppendZ (S, "=\"");
        EscapeForDump (S, A->Value, A->ValueLen);
        SbAppendC (S, '"');
      }
    }
    SbAppendZ (S, ">\n");
    break;
  }

  for (C = N->FirstChild; C != DFBR_NULL; C = C->NextSibling) {
    DumpNode (S, C, Depth + 1);
  }
}

char *
DfBrHtmlDumpTree (
  const DfBrHtmlNode  *Node,
  usize               *OutLen
  )
{
  Sb  S;

  memset (&S, 0, sizeof (S));
  if (Node == DFBR_NULL) {
    if (OutLen != DFBR_NULL) *OutLen = 0;
    return DFBR_NULL;
  }
  DumpNode (&S, Node, 0);
  return SbFinish (&S, OutLen);
}

void
DfBrHtmlFreeSerialized (
  char  *S
  )
{
  free (S);
}
