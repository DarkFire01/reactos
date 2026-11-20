/** @file
  DfBrHtmlTree.c - WHATWG tree construction for DfBrHtml.

  What this file does
  ===================
  The tokenizer (DfBrHtmlParse.c) produces a stream of five token
  kinds: StartTag, EndTag, Character, Comment, Doctype, plus an EOF
  signal. It calls into the entry points declared in
  DfBrHtmlInternal.h:

      DfBrHtmlTreeProcessStartTag
      DfBrHtmlTreeProcessEndTag
      DfBrHtmlTreeProcessText
      DfBrHtmlTreeProcessComment
      DfBrHtmlTreeProcessDoctype
      DfBrHtmlTreeProcessEof

  Each of those dispatches through P->IM (current insertion mode) to
  a per-mode handler, which may:

    * insert the token into the DOM (possibly via foster parenting),
    * push / pop open-elements stack entries,
    * change the insertion mode,
    * switch the tokenizer's lexical state (for rawtext / rcdata),
    * signal Reprocess so the outer dispatcher re-invokes the new
      mode's handler with the same token.

  The dispatcher loop restarts until a mode handles the token without
  requesting reprocessing, matching the spec's "reprocess the token"
  language.

  v2 coverage (this file)
  =======================

    * Insertion modes: Initial, BeforeHtml, BeforeHead, InHead,
      AfterHead, InBody, Text, InTable, InTableText, InCaption,
      InColumnGroup, InTableBody, InRow, InCell, AfterBody,
      AfterAfterBody.
    * Implicit <html>, <head>, <body> insertion from the first
      content token.
    * "have element in (default/list-item/button/table/select) scope"
      predicates.
    * "generate implied end tags" + "close a p element" helpers.
    * "reset the insertion mode appropriately".
    * Foster parenting (adjusted insertion location when the current
      node is table/tbody/thead/tfoot/tr).
    * Heading / <li> / <dd> / <dt> / <p> / <option>/<optgroup>
      auto-close on opening another.
    * Rawtext / rcdata element insertion (script, style, title,
      textarea) via IMText + tokenizer state flip.

  Deliberate v2 gaps (future bites)
  =================================

    * **Adoption Agency Algorithm.** `<b><i>x</b>y</i>` does not
      re-clone the formatting element yet; we do simple stack pop on
      end tag which gives reasonable but not spec-correct output.
    * **Active formatting elements list.** Needed for the above.
    * **<template>.** Treated as an ordinary element; no template-
      contents fragment, no per-template insertion-mode stack.
    * **InSelect / InSelectInTable.** <select> is a normal element;
      its children are regular DOM.
    * **Frameset.** <frameset> is a normal element; no
      IMInFrameset / IMAfterFrameset behaviour beyond reset.
    * **Foreign content.** <svg>/<math> do NOT enter the "in foreign
      content" mode; we ignore HTML-integration-point nuances.
    * **Full DOCTYPE parsing.** Name / PublicId / SystemId / quirks
      flag not separated out yet.

  The gaps are all additive: landing them extends this file (and a
  few helpers) without rewiring callers.
**/

#include "DfBrHtmlInternal.h"

#include <stdlib.h>
#include <string.h>

/* ==================================================================
 *  Diagnostics passthroughs (shared name between TUs)
 * ==================================================================*/

void
DfBrHtmlDiagAt (DfBrHtmlParser *P, i32 Line, i32 Column, const char *Msg)
{
  DfBrHtmlDiagEmit (&P->Doc->Diags, &P->Doc->Arena, Line, Column, Msg);
}

void
DfBrHtmlDiagHere (DfBrHtmlParser *P, const char *Msg)
{
  DfBrHtmlDiagAt (P, P->Line, P->Column, Msg);
}

/* ==================================================================
 *  Utilities
 * ==================================================================*/

static b32
IsWS (char c)
{
  return (c == ' ') || (c == '\t') || (c == '\n') ||
         (c == '\r') || (c == '\f');
}

static b32
AllWhitespace (const char *S, usize Len)
{
  usize i;
  for (i = 0; i < Len; i++) {
    if (!IsWS (S[i])) {
      return DFBR_FALSE;
    }
  }
  return DFBR_TRUE;
}

static b32
TagEq (DfBrHtmlNode *N, DfBrHtmlTag T)
{
  return (N != DFBR_NULL) && (N->Kind == DFBR_HTML_NODE_ELEMENT) && (N->Tag == T);
}

/* ==================================================================
 *  Open-elements stack
 * ==================================================================*/

static void
StackPush (DfBrHtmlParser *P, DfBrHtmlNode *N)
{
  if (P->StackTop + 1 >= DFBR_HTML_OPEN_STACK_LIMIT) {
    DfBrHtmlDiagHere (P, "open-element-stack-overflow");
    return;
  }
  P->Stack[++P->StackTop] = N;
}

static DfBrHtmlNode *
StackTop (DfBrHtmlParser *P)
{
  if (P->StackTop < 0) {
    return DFBR_NULL;
  }
  return P->Stack[P->StackTop];
}

/* CurrentNode is "top of stack if any, else document". The
 * tokenizer-era code also used this as the insertion parent; with
 * foster parenting we consult AdjustedInsertionLocation instead. */
static DfBrHtmlNode *
CurrentNode (DfBrHtmlParser *P)
{
  if (P->StackTop < 0) {
    return P->DocumentNode;
  }
  return P->Stack[P->StackTop];
}

static void
StackPop (DfBrHtmlParser *P)
{
  if (P->StackTop >= 0) {
    P->StackTop--;
  }
}

static i32
StackFindTag (DfBrHtmlParser *P, DfBrHtmlTag T)
{
  i32  i;
  for (i = P->StackTop; i >= 0; i--) {
    if (P->Stack[i]->Tag == T) {
      return i;
    }
  }
  return -1;
}

static b32
StackHasTag (DfBrHtmlParser *P, DfBrHtmlTag T)
{
  return StackFindTag (P, T) >= 0;
}

/* Pop until (and including) the stack entry at index idx. */
static void
StackPopTo (DfBrHtmlParser *P, i32 Idx)
{
  if (Idx < 0) {
    return;
  }
  P->StackTop = Idx - 1;
}

/* Pop elements whose tag is one of dd, dt, li, optgroup, option, p,
 * rb, rp, rt, rtc - unless the tag matches ExceptFor. DFBR_HTML_TAG_UNKNOWN
 * in ExceptFor means "no exception, pop all of them". */
static void
GenerateImpliedEndTags (DfBrHtmlParser *P, DfBrHtmlTag ExceptFor)
{
  for (;;) {
    DfBrHtmlNode *N = StackTop (P);
    if (N == DFBR_NULL) {
      return;
    }
    if (N->Tag == ExceptFor) {
      return;
    }
    switch (N->Tag) {
      case DFBR_HTML_TAG_DD:
      case DFBR_HTML_TAG_DT:
      case DFBR_HTML_TAG_LI:
      case DFBR_HTML_TAG_OPTGROUP:
      case DFBR_HTML_TAG_OPTION:
      case DFBR_HTML_TAG_P:
      case DFBR_HTML_TAG_RP:
      case DFBR_HTML_TAG_RT:
        StackPop (P);
        continue;
      default:
        return;
    }
  }
}

/* "generate implied end tags thoroughly" - same set plus a few more
 * used by <caption> / <table> close paths. */
static void
GenerateImpliedEndTagsThoroughly (DfBrHtmlParser *P)
{
  for (;;) {
    DfBrHtmlNode *N = StackTop (P);
    if (N == DFBR_NULL) {
      return;
    }
    switch (N->Tag) {
      case DFBR_HTML_TAG_CAPTION:
      case DFBR_HTML_TAG_COLGROUP:
      case DFBR_HTML_TAG_DD:
      case DFBR_HTML_TAG_DT:
      case DFBR_HTML_TAG_LI:
      case DFBR_HTML_TAG_OPTGROUP:
      case DFBR_HTML_TAG_OPTION:
      case DFBR_HTML_TAG_P:
      case DFBR_HTML_TAG_RP:
      case DFBR_HTML_TAG_RT:
      case DFBR_HTML_TAG_TBODY:
      case DFBR_HTML_TAG_TD:
      case DFBR_HTML_TAG_TFOOT:
      case DFBR_HTML_TAG_TH:
      case DFBR_HTML_TAG_THEAD:
      case DFBR_HTML_TAG_TR:
        StackPop (P);
        continue;
      default:
        return;
    }
  }
}

/* ==================================================================
 *  Scope predicates
 *
 *  "Have X in Y scope": walk the stack from top down; if we hit X,
 *  return true; if we hit a "scope terminator" element for Y, return
 *  false. Scope terminators vary by scope kind.
 * ==================================================================*/

/* Default scope terminators per spec. */
static b32
IsDefaultScopeTerminator (DfBrHtmlTag T)
{
  switch (T) {
    case DFBR_HTML_TAG_HTML:
    case DFBR_HTML_TAG_TABLE:
    case DFBR_HTML_TAG_TEMPLATE:
    case DFBR_HTML_TAG_CAPTION:
    case DFBR_HTML_TAG_TD:
    case DFBR_HTML_TAG_TH:
    case DFBR_HTML_TAG_OBJECT:
    case DFBR_HTML_TAG_SVG:
    case DFBR_HTML_TAG_MATH:
      return DFBR_TRUE;
    default:
      return DFBR_FALSE;
  }
}

/* "Special category" per HTML spec (§13.2.4.2). This set is used by the
 * <li>, <dd>, <dt>, and adoption agency paths to stop walking up the
 * open-element stack as soon as a "block-ish" ancestor is encountered
 * (so that e.g. a new <li> nested inside a fresh <ul> does NOT close
 * the <li> that CONTAINS that <ul>). Without this, a structure like
 *
 *   <ul><li>Item A<ul><li>A1</li></ul></li></ul>
 *
 * gets mis-parsed: the inner <li>A1</li> start tag walks past the
 * inner <ul>, finds the outer <li>, closes it, and hoists A1 up as a
 * sibling. That exact pattern appears in every Bootstrap/WordPress
 * navigation menu on the web, so getting it right fixes e.g. the
 * reactos.org header (which otherwise renders all dropdown items
 * stacked on top of the page title). */
static b32
IsSpecialCategory (DfBrHtmlTag T)
{
  switch (T) {
    case DFBR_HTML_TAG_ADDRESS:
    case DFBR_HTML_TAG_APPLET:
    case DFBR_HTML_TAG_AREA:
    case DFBR_HTML_TAG_ARTICLE:
    case DFBR_HTML_TAG_ASIDE:
    case DFBR_HTML_TAG_BASE:
    case DFBR_HTML_TAG_BLOCKQUOTE:
    case DFBR_HTML_TAG_BODY:
    case DFBR_HTML_TAG_BR:
    case DFBR_HTML_TAG_BUTTON:
    case DFBR_HTML_TAG_CAPTION:
    case DFBR_HTML_TAG_CENTER:
    case DFBR_HTML_TAG_COL:
    case DFBR_HTML_TAG_COLGROUP:
    case DFBR_HTML_TAG_DD:
    case DFBR_HTML_TAG_DETAILS:
    case DFBR_HTML_TAG_DIR:
    case DFBR_HTML_TAG_DIV:
    case DFBR_HTML_TAG_DL:
    case DFBR_HTML_TAG_DT:
    case DFBR_HTML_TAG_EMBED:
    case DFBR_HTML_TAG_FIELDSET:
    case DFBR_HTML_TAG_FIGCAPTION:
    case DFBR_HTML_TAG_FIGURE:
    case DFBR_HTML_TAG_FOOTER:
    case DFBR_HTML_TAG_FORM:
    case DFBR_HTML_TAG_FRAME:
    case DFBR_HTML_TAG_FRAMESET:
    case DFBR_HTML_TAG_H1:
    case DFBR_HTML_TAG_H2:
    case DFBR_HTML_TAG_H3:
    case DFBR_HTML_TAG_H4:
    case DFBR_HTML_TAG_H5:
    case DFBR_HTML_TAG_H6:
    case DFBR_HTML_TAG_HEAD:
    case DFBR_HTML_TAG_HEADER:
    case DFBR_HTML_TAG_HGROUP:
    case DFBR_HTML_TAG_HR:
    case DFBR_HTML_TAG_HTML:
    case DFBR_HTML_TAG_IFRAME:
    case DFBR_HTML_TAG_IMG:
    case DFBR_HTML_TAG_INPUT:
    case DFBR_HTML_TAG_KEYGEN:
    case DFBR_HTML_TAG_LI:
    case DFBR_HTML_TAG_LINK:
    case DFBR_HTML_TAG_LISTING:
    case DFBR_HTML_TAG_MAIN:
    case DFBR_HTML_TAG_MARQUEE:
    case DFBR_HTML_TAG_MENU:
    case DFBR_HTML_TAG_META:
    case DFBR_HTML_TAG_NAV:
    case DFBR_HTML_TAG_NOEMBED:
    case DFBR_HTML_TAG_NOFRAMES:
    case DFBR_HTML_TAG_NOSCRIPT:
    case DFBR_HTML_TAG_OBJECT:
    case DFBR_HTML_TAG_OL:
    case DFBR_HTML_TAG_P:
    case DFBR_HTML_TAG_PARAM:
    case DFBR_HTML_TAG_PLAINTEXT:
    case DFBR_HTML_TAG_PRE:
    case DFBR_HTML_TAG_SCRIPT:
    case DFBR_HTML_TAG_SECTION:
    case DFBR_HTML_TAG_SELECT:
    case DFBR_HTML_TAG_SOURCE:
    case DFBR_HTML_TAG_STYLE:
    case DFBR_HTML_TAG_SUMMARY:
    case DFBR_HTML_TAG_TABLE:
    case DFBR_HTML_TAG_TBODY:
    case DFBR_HTML_TAG_TD:
    case DFBR_HTML_TAG_TEMPLATE:
    case DFBR_HTML_TAG_TEXTAREA:
    case DFBR_HTML_TAG_TFOOT:
    case DFBR_HTML_TAG_TH:
    case DFBR_HTML_TAG_THEAD:
    case DFBR_HTML_TAG_TITLE:
    case DFBR_HTML_TAG_TR:
    case DFBR_HTML_TAG_TRACK:
    case DFBR_HTML_TAG_UL:
    case DFBR_HTML_TAG_WBR:
    case DFBR_HTML_TAG_XMP:
    case DFBR_HTML_TAG_SVG:
    case DFBR_HTML_TAG_MATH:
      return DFBR_TRUE;
    default:
      return DFBR_FALSE;
  }
}

static b32
HasInScope (DfBrHtmlParser *P, DfBrHtmlTag Target)
{
  i32 i;
  for (i = P->StackTop; i >= 0; i--) {
    DfBrHtmlTag T = P->Stack[i]->Tag;
    if (T == Target) {
      return DFBR_TRUE;
    }
    if (IsDefaultScopeTerminator (T)) {
      return DFBR_FALSE;
    }
  }
  return DFBR_FALSE;
}

/* List-item scope = default + ol, ul. */
static b32
HasInListItemScope (DfBrHtmlParser *P, DfBrHtmlTag Target)
{
  i32 i;
  for (i = P->StackTop; i >= 0; i--) {
    DfBrHtmlTag T = P->Stack[i]->Tag;
    if (T == Target) {
      return DFBR_TRUE;
    }
    if (IsDefaultScopeTerminator (T) ||
        (T == DFBR_HTML_TAG_OL) ||
        (T == DFBR_HTML_TAG_UL)) {
      return DFBR_FALSE;
    }
  }
  return DFBR_FALSE;
}

/* Button scope = default + button. */
static b32
HasInButtonScope (DfBrHtmlParser *P, DfBrHtmlTag Target)
{
  i32 i;
  for (i = P->StackTop; i >= 0; i--) {
    DfBrHtmlTag T = P->Stack[i]->Tag;
    if (T == Target) {
      return DFBR_TRUE;
    }
    if (IsDefaultScopeTerminator (T) || (T == DFBR_HTML_TAG_BUTTON)) {
      return DFBR_FALSE;
    }
  }
  return DFBR_FALSE;
}

/* Table scope = html, table, template only. */
static b32
HasInTableScope (DfBrHtmlParser *P, DfBrHtmlTag Target)
{
  i32 i;
  for (i = P->StackTop; i >= 0; i--) {
    DfBrHtmlTag T = P->Stack[i]->Tag;
    if (T == Target) {
      return DFBR_TRUE;
    }
    if ((T == DFBR_HTML_TAG_HTML) ||
        (T == DFBR_HTML_TAG_TABLE) ||
        (T == DFBR_HTML_TAG_TEMPLATE)) {
      return DFBR_FALSE;
    }
  }
  return DFBR_FALSE;
}

/* ==================================================================
 *  Adjusted insertion location (foster parenting)
 *
 *  Applies when the current node is table-ish and we have content
 *  that doesn't belong there. The adjusted location moves the
 *  insertion to "immediately before the last table on the stack"
 *  inside its parent, falling back to appending inside the node just
 *  below that table if the table has no parent.
 * ==================================================================*/

typedef struct {
  DfBrHtmlNode  *Parent;
  DfBrHtmlNode  *BeforeRef;   /* NULL => append, else insertBefore(Ref) */
} InsertionLocation;

static b32
CurrentNodeIsTableContext (DfBrHtmlParser *P)
{
  DfBrHtmlNode *N = CurrentNode (P);
  if (N == DFBR_NULL) {
    return DFBR_FALSE;
  }
  switch (N->Tag) {
    case DFBR_HTML_TAG_TABLE:
    case DFBR_HTML_TAG_TBODY:
    case DFBR_HTML_TAG_THEAD:
    case DFBR_HTML_TAG_TFOOT:
    case DFBR_HTML_TAG_TR:
      return DFBR_TRUE;
    default:
      return DFBR_FALSE;
  }
}

static InsertionLocation
AdjustedInsertionLocation (DfBrHtmlParser *P, b32 AllowFoster)
{
  InsertionLocation Loc;
  Loc.Parent    = CurrentNode (P);
  Loc.BeforeRef = DFBR_NULL;

  if (!AllowFoster || !CurrentNodeIsTableContext (P)) {
    return Loc;
  }

  /* Foster parenting: last <table> on the stack. */
  {
    i32 i = StackFindTag (P, DFBR_HTML_TAG_TABLE);
    if (i < 0) {
      /* no table on the stack - fall back to the first element on
       * the stack (should be <html>). */
      Loc.Parent = (P->StackTop >= 0) ? P->Stack[0] : P->DocumentNode;
      Loc.BeforeRef = DFBR_NULL;
      return Loc;
    }
    {
      DfBrHtmlNode *Table = P->Stack[i];
      if (Table->Parent != DFBR_NULL) {
        Loc.Parent    = Table->Parent;
        Loc.BeforeRef = Table;
      } else if (i > 0) {
        /* Table's parent was detached (e.g. during fragment parsing).
         * Append into the element just below it on the stack. */
        Loc.Parent    = P->Stack[i - 1];
        Loc.BeforeRef = DFBR_NULL;
      } else {
        Loc.Parent    = P->DocumentNode;
        Loc.BeforeRef = DFBR_NULL;
      }
    }
  }
  return Loc;
}

static void
InsertAt (InsertionLocation Loc, DfBrHtmlNode *N)
{
  if (Loc.BeforeRef != DFBR_NULL) {
    DfBrHtmlInsertBefore (Loc.Parent, N, Loc.BeforeRef);
  } else {
    DfBrHtmlAppendChild (Loc.Parent, N);
  }
}

/* ==================================================================
 *  Insertion helpers
 * ==================================================================*/

/* Insert an element honouring foster-parenting when applicable and
 * push it on the open-elements stack. */
static void
InsertElementPush (DfBrHtmlParser *P, DfBrHtmlNode *N, b32 AllowFoster)
{
  InsertionLocation Loc = AdjustedInsertionLocation (P, AllowFoster);
  InsertAt (Loc, N);
  StackPush (P, N);
}

/* Same but for a void element - never pushed. */
static void
InsertVoidElement (DfBrHtmlParser *P, DfBrHtmlNode *N, b32 AllowFoster)
{
  InsertionLocation Loc = AdjustedInsertionLocation (P, AllowFoster);
  InsertAt (Loc, N);
}

static void
InsertCharacters (DfBrHtmlParser *P, const char *Data, usize Len, b32 AllowFoster)
{
  InsertionLocation  Loc;
  DfBrHtmlNode      *Target;
  DfBrHtmlNode      *Prev;
  DfBrHtmlNode      *T;
  char              *Joined;
  usize              TotalLen;

  if (Len == 0) {
    return;
  }
  Loc    = AdjustedInsertionLocation (P, AllowFoster);
  Target = Loc.Parent;
  Prev   = (Loc.BeforeRef != DFBR_NULL) ? Loc.BeforeRef->PrevSibling
                                        : Target->LastChild;

  /* Spec: if the previous sibling at the insertion location is a Text
   * node, append to it instead of creating a new one. Simplifies the
   * tree and keeps serialize() output contiguous. */
  if ((Prev != DFBR_NULL) && (Prev->Kind == DFBR_HTML_NODE_TEXT)) {
    TotalLen = Prev->TextLen + Len;
    Joined   = (char *)DfBrHtmlArenaAlloc (&P->Doc->Arena, TotalLen + 1);
    if (Joined != DFBR_NULL) {
      memcpy (Joined, Prev->Text, Prev->TextLen);
      memcpy (Joined + Prev->TextLen, Data, Len);
      Joined[TotalLen] = '\0';
      Prev->Text    = Joined;
      Prev->TextLen = TotalLen;
    }
    return;
  }

  T = DfBrHtmlNewText (P->Doc, Data, Len);
  if (T != DFBR_NULL) {
    InsertAt (Loc, T);
  }
}

static void
InsertComment (DfBrHtmlParser *P, const char *Data, usize Len, DfBrHtmlNode *ForceParent)
{
  DfBrHtmlNode *C;
  if (!P->Doc->Options.KeepComments) {
    return;
  }
  C = DfBrHtmlNewComment (P->Doc, Data, Len);
  if (C == DFBR_NULL) {
    return;
  }
  if (ForceParent != DFBR_NULL) {
    DfBrHtmlAppendChild (ForceParent, C);
  } else {
    InsertionLocation Loc = AdjustedInsertionLocation (P, DFBR_FALSE);
    InsertAt (Loc, C);
  }
}

/* Create a "fresh" element with no attributes (used for implicit
 * insertion: <html>, <head>, <body>, implicit <tbody>, <tr>). */
static DfBrHtmlNode *
MakeFreshElement (DfBrHtmlParser *P, const char *Name, usize NameLen, DfBrHtmlTag Tag)
{
  DfBrHtmlNode *N = DfBrHtmlNewElement (P->Doc, Name, NameLen, Tag);
  if (N != DFBR_NULL) {
    N->Line   = P->Line;
    N->Column = P->Column;
  }
  return N;
}

/* Copy attributes from Src element into Dst, but only those whose
 * name is not already present on Dst. Used by InBody's merge rules
 * for duplicate <html> / <body> start tags. */
static void
MergeAttrsFrom (DfBrHtmlParser *P, DfBrHtmlNode *Dst, DfBrHtmlNode *Src)
{
  DfBrHtmlAttr *A;
  for (A = Src->FirstAttr; A != DFBR_NULL; A = A->Next) {
    if (DfBrHtmlNodeGetAttr (Dst, A->Name) == DFBR_NULL) {
      DfBrHtmlAppendAttr (P->Doc, Dst,
                          A->Name, A->NameLen,
                          A->HasValue ? A->Value : DFBR_NULL,
                          A->HasValue ? A->ValueLen : 0);
    }
  }
}

/* ==================================================================
 *  Close a p element
 *
 *  "generate implied end tags except for p; if current node is not
 *  p, parse error; pop elements until a p is popped."
 * ==================================================================*/

static void
CloseP (DfBrHtmlParser *P)
{
  GenerateImpliedEndTags (P, DFBR_HTML_TAG_P);
  if (!TagEq (CurrentNode (P), DFBR_HTML_TAG_P)) {
    DfBrHtmlDiagHere (P, "implicit-close-p-at-mismatched-node");
  }
  while (P->StackTop >= 0) {
    DfBrHtmlTag T = P->Stack[P->StackTop]->Tag;
    StackPop (P);
    if (T == DFBR_HTML_TAG_P) {
      break;
    }
  }
}

/* ==================================================================
 *  Reset insertion mode appropriately
 *
 *  Called after popping out of certain contexts (e.g. closing a
 *  caption, table, cell). Walks the stack top-to-bottom and picks a
 *  new insertion mode based on what we find.
 * ==================================================================*/

static void
ResetInsertionMode (DfBrHtmlParser *P)
{
  i32 i;
  for (i = P->StackTop; i >= 0; i--) {
    b32           Last = (i == 0) ? DFBR_TRUE : DFBR_FALSE;
    DfBrHtmlNode *N    = P->Stack[i];

    switch (N->Tag) {
      case DFBR_HTML_TAG_SELECT:
        /* TODO: InSelect / InSelectInTable. For v2 fall through to
         * InBody so <select>'s children still parse. */
        P->IM = DfBrHtmlImInBody;
        return;
      case DFBR_HTML_TAG_TD:
      case DFBR_HTML_TAG_TH:
        if (!Last) { P->IM = DfBrHtmlImInCell; return; }
        break;
      case DFBR_HTML_TAG_TR:
        P->IM = DfBrHtmlImInRow;
        return;
      case DFBR_HTML_TAG_TBODY:
      case DFBR_HTML_TAG_THEAD:
      case DFBR_HTML_TAG_TFOOT:
        P->IM = DfBrHtmlImInTableBody;
        return;
      case DFBR_HTML_TAG_CAPTION:
        P->IM = DfBrHtmlImInCaption;
        return;
      case DFBR_HTML_TAG_COLGROUP:
        P->IM = DfBrHtmlImInColumnGroup;
        return;
      case DFBR_HTML_TAG_TABLE:
        P->IM = DfBrHtmlImInTable;
        return;
      case DFBR_HTML_TAG_TEMPLATE:
        /* TODO: template-mode stack. */
        P->IM = DfBrHtmlImInBody;
        return;
      case DFBR_HTML_TAG_HEAD:
        if (!Last) { P->IM = DfBrHtmlImInHead; return; }
        P->IM = DfBrHtmlImInBody;
        return;
      case DFBR_HTML_TAG_BODY:
        P->IM = DfBrHtmlImInBody;
        return;
      case DFBR_HTML_TAG_FRAMESET:
        /* TODO: IMInFrameset - we don't support framesets yet. */
        P->IM = DfBrHtmlImInBody;
        return;
      case DFBR_HTML_TAG_HTML:
        P->IM = (P->HeadElement != DFBR_NULL)
                  ? DfBrHtmlImAfterHead
                  : DfBrHtmlImBeforeHead;
        return;
      default:
        break;
    }
    if (Last) {
      P->IM = DfBrHtmlImInBody;
      return;
    }
  }
  P->IM = DfBrHtmlImInBody;
}

/* ==================================================================
 *  Clear-stack helpers used by table insertion modes
 *
 *  "clear the stack back to a table context" - pop until current is
 *  table / template / html.
 * ==================================================================*/

static void
ClearBackToTableContext (DfBrHtmlParser *P)
{
  while (P->StackTop >= 0) {
    DfBrHtmlTag T = P->Stack[P->StackTop]->Tag;
    if ((T == DFBR_HTML_TAG_TABLE) ||
        (T == DFBR_HTML_TAG_TEMPLATE) ||
        (T == DFBR_HTML_TAG_HTML)) {
      return;
    }
    StackPop (P);
  }
}

static void
ClearBackToTableBodyContext (DfBrHtmlParser *P)
{
  while (P->StackTop >= 0) {
    DfBrHtmlTag T = P->Stack[P->StackTop]->Tag;
    if ((T == DFBR_HTML_TAG_TBODY) ||
        (T == DFBR_HTML_TAG_THEAD) ||
        (T == DFBR_HTML_TAG_TFOOT) ||
        (T == DFBR_HTML_TAG_TEMPLATE) ||
        (T == DFBR_HTML_TAG_HTML)) {
      return;
    }
    StackPop (P);
  }
}

static void
ClearBackToTableRowContext (DfBrHtmlParser *P)
{
  while (P->StackTop >= 0) {
    DfBrHtmlTag T = P->Stack[P->StackTop]->Tag;
    if ((T == DFBR_HTML_TAG_TR) ||
        (T == DFBR_HTML_TAG_TEMPLATE) ||
        (T == DFBR_HTML_TAG_HTML)) {
      return;
    }
    StackPop (P);
  }
}

/* "Close the cell" - used on end-of-row and start-tags inside cells. */
static void
CloseCell (DfBrHtmlParser *P)
{
  GenerateImpliedEndTags (P, DFBR_HTML_TAG_UNKNOWN);
  while (P->StackTop >= 0) {
    DfBrHtmlTag T = P->Stack[P->StackTop]->Tag;
    StackPop (P);
    if ((T == DFBR_HTML_TAG_TD) || (T == DFBR_HTML_TAG_TH)) {
      break;
    }
  }
  P->IM = DfBrHtmlImInRow;
}

/* ==================================================================
 *  Tokenizer-state flip: raw text / rcdata elements
 *
 *  When the tree constructor inserts one of these, it also flips the
 *  tokenizer into the right lexical mode and records the element
 *  name so the matching </name> end tag will be recognised from
 *  inside rawtext.
 * ==================================================================*/

static void
SwitchToText (DfBrHtmlParser *P, DfBrHtmlNode *N, b32 Rawtext)
{
  P->TokState       = Rawtext ? DfBrHtmlStateRawText : DfBrHtmlStateRcData;
  P->EndTagMatch    = N->Name;
  P->EndTagMatchLen = N->NameLen;
  P->OriginalIM     = P->IM;
  P->IM             = DfBrHtmlImText;
}

/* ==================================================================
 *  Initialise tree state
 * ==================================================================*/

void
DfBrHtmlTreeInit (DfBrHtmlParser *P)
{
  P->IM            = DfBrHtmlImInitial;
  P->OriginalIM    = DfBrHtmlImInitial;
  P->StackTop      = -1;
  P->DocumentNode  = P->Doc->Root;
  P->HtmlElement   = DFBR_NULL;
  P->HeadElement   = DFBR_NULL;
  P->BodyElement   = DFBR_NULL;
  P->FormElement   = DFBR_NULL;
  P->FramesetOk    = DFBR_TRUE;
  P->Reprocess     = DFBR_FALSE;
}

/* ==================================================================
 *  Forward declarations of per-mode dispatchers
 * ==================================================================*/

static void StartTag_InBody    (DfBrHtmlParser *P);
static void EndTag_InBody      (DfBrHtmlParser *P);
static void Text_InBody        (DfBrHtmlParser *P, const char *Data, usize Len);

/* ==================================================================
 *  Mode: Initial
 * ==================================================================*/

static void
StartTag_Initial (DfBrHtmlParser *P)
{
  /* Anything that isn't doctype / ws / comment: switch to BeforeHtml
   * and reprocess. (Spec also emits a quirks-mode parse error.) */
  DfBrHtmlDiagAt (P, P->Pending->Line, P->Pending->Column,
                  "missing-doctype");
  P->IM        = DfBrHtmlImBeforeHtml;
  P->Reprocess = DFBR_TRUE;
}

static void
EndTag_Initial (DfBrHtmlParser *P)
{
  DfBrHtmlDiagHere (P, "missing-doctype");
  P->IM        = DfBrHtmlImBeforeHtml;
  P->Reprocess = DFBR_TRUE;
}

static void
Text_Initial (DfBrHtmlParser *P, const char **Data, usize *Len)
{
  /* Leading whitespace is ignored in Initial. If there's non-ws,
   * switch and reprocess. */
  usize i = 0;
  while ((i < *Len) && IsWS ((*Data)[i])) {
    i++;
  }
  if (i == *Len) {
    return;   /* all whitespace - consumed, no reprocess */
  }
  *Data += i;
  *Len  -= i;
  DfBrHtmlDiagHere (P, "missing-doctype");
  P->IM        = DfBrHtmlImBeforeHtml;
  P->Reprocess = DFBR_TRUE;
}

/* ==================================================================
 *  Mode: BeforeHtml
 * ==================================================================*/

static DfBrHtmlNode *
EnsureHtmlFromPending (DfBrHtmlParser *P)
{
  DfBrHtmlNode *H = P->Pending;
  if (H == DFBR_NULL) {
    H = MakeFreshElement (P, "html", 4, DFBR_HTML_TAG_HTML);
  } else if (H->Tag != DFBR_HTML_TAG_HTML) {
    H = MakeFreshElement (P, "html", 4, DFBR_HTML_TAG_HTML);
  }
  if (H == DFBR_NULL) {
    return DFBR_NULL;
  }
  DfBrHtmlAppendChild (P->DocumentNode, H);
  StackPush (P, H);
  P->HtmlElement = H;
  return H;
}

static void
StartTag_BeforeHtml (DfBrHtmlParser *P)
{
  if (P->Pending->Tag == DFBR_HTML_TAG_HTML) {
    DfBrHtmlAppendChild (P->DocumentNode, P->Pending);
    StackPush (P, P->Pending);
    P->HtmlElement = P->Pending;
    P->IM          = DfBrHtmlImBeforeHead;
    return;
  }
  /* Anything else: insert implicit <html> and reprocess. */
  EnsureHtmlFromPending (P);   /* allocates a fresh html since Pending isn't html */
  P->IM        = DfBrHtmlImBeforeHead;
  P->Reprocess = DFBR_TRUE;
}

static void
EndTag_BeforeHtml (DfBrHtmlParser *P)
{
  DfBrHtmlTag T = P->Pending->Tag;
  if ((T == DFBR_HTML_TAG_HEAD) || (T == DFBR_HTML_TAG_BODY) ||
      (T == DFBR_HTML_TAG_HTML) || (T == DFBR_HTML_TAG_BR)) {
    EnsureHtmlFromPending (P);
    P->IM        = DfBrHtmlImBeforeHead;
    P->Reprocess = DFBR_TRUE;
    return;
  }
  DfBrHtmlDiagHere (P, "unexpected-end-tag-before-html");
}

static void
Text_BeforeHtml (DfBrHtmlParser *P, const char **Data, usize *Len)
{
  usize i = 0;
  while ((i < *Len) && IsWS ((*Data)[i])) {
    i++;
  }
  if (i == *Len) {
    return;
  }
  *Data += i;
  *Len  -= i;
  EnsureHtmlFromPending (P);
  P->IM        = DfBrHtmlImBeforeHead;
  P->Reprocess = DFBR_TRUE;
}

/* ==================================================================
 *  Mode: BeforeHead
 * ==================================================================*/

static void
InsertImplicitHead (DfBrHtmlParser *P)
{
  DfBrHtmlNode *H = MakeFreshElement (P, "head", 4, DFBR_HTML_TAG_HEAD);
  if (H == DFBR_NULL) {
    return;
  }
  DfBrHtmlAppendChild (CurrentNode (P), H);
  StackPush (P, H);
  P->HeadElement = H;
}

static void
StartTag_BeforeHead (DfBrHtmlParser *P)
{
  DfBrHtmlTag T = P->Pending->Tag;
  if (T == DFBR_HTML_TAG_HTML) {
    StartTag_InBody (P);
    return;
  }
  if (T == DFBR_HTML_TAG_HEAD) {
    DfBrHtmlAppendChild (CurrentNode (P), P->Pending);
    StackPush (P, P->Pending);
    P->HeadElement = P->Pending;
    P->IM          = DfBrHtmlImInHead;
    return;
  }
  InsertImplicitHead (P);
  P->IM        = DfBrHtmlImInHead;
  P->Reprocess = DFBR_TRUE;
}

static void
EndTag_BeforeHead (DfBrHtmlParser *P)
{
  DfBrHtmlTag T = P->Pending->Tag;
  if ((T == DFBR_HTML_TAG_HEAD) || (T == DFBR_HTML_TAG_BODY) ||
      (T == DFBR_HTML_TAG_HTML) || (T == DFBR_HTML_TAG_BR)) {
    InsertImplicitHead (P);
    P->IM        = DfBrHtmlImInHead;
    P->Reprocess = DFBR_TRUE;
    return;
  }
  DfBrHtmlDiagHere (P, "unexpected-end-tag-before-head");
}

static void
Text_BeforeHead (DfBrHtmlParser *P, const char **Data, usize *Len)
{
  usize i = 0;
  while ((i < *Len) && IsWS ((*Data)[i])) {
    i++;
  }
  if (i == *Len) {
    return;
  }
  *Data += i;
  *Len  -= i;
  InsertImplicitHead (P);
  P->IM        = DfBrHtmlImInHead;
  P->Reprocess = DFBR_TRUE;
}

/* ==================================================================
 *  Mode: InHead
 * ==================================================================*/

static void
PopHeadAndSwitchToAfterHead (DfBrHtmlParser *P)
{
  /* Pop the <head> element from the stack. */
  while (P->StackTop >= 0) {
    DfBrHtmlTag T = P->Stack[P->StackTop]->Tag;
    StackPop (P);
    if (T == DFBR_HTML_TAG_HEAD) {
      break;
    }
  }
  P->IM = DfBrHtmlImAfterHead;
}

static void
StartTag_InHead (DfBrHtmlParser *P)
{
  DfBrHtmlTag   T = P->Pending->Tag;
  DfBrHtmlNode *N = P->Pending;

  switch (T) {
    case DFBR_HTML_TAG_HTML:
      StartTag_InBody (P);
      return;

    case DFBR_HTML_TAG_BASE:
    case DFBR_HTML_TAG_LINK:
    case DFBR_HTML_TAG_META:
      InsertVoidElement (P, N, DFBR_FALSE);
      return;

    case DFBR_HTML_TAG_TITLE:
      DfBrHtmlAppendChild (CurrentNode (P), N);
      StackPush (P, N);
      SwitchToText (P, N, DFBR_FALSE);   /* RCDATA */
      return;

    case DFBR_HTML_TAG_NOSCRIPT:
      /* TODO: IMInHeadNoscript with scripting-flag awareness. For v2
       * we treat it as a regular element in head. */
      DfBrHtmlAppendChild (CurrentNode (P), N);
      StackPush (P, N);
      return;

    case DFBR_HTML_TAG_STYLE:
      DfBrHtmlAppendChild (CurrentNode (P), N);
      StackPush (P, N);
      SwitchToText (P, N, DFBR_TRUE);    /* RAWTEXT */
      return;

    case DFBR_HTML_TAG_SCRIPT:
      DfBrHtmlAppendChild (CurrentNode (P), N);
      StackPush (P, N);
      SwitchToText (P, N, DFBR_TRUE);    /* RAWTEXT */
      return;

    case DFBR_HTML_TAG_TEMPLATE:
      /* TODO: template-contents fragment + template-mode stack. */
      DfBrHtmlAppendChild (CurrentNode (P), N);
      StackPush (P, N);
      return;

    case DFBR_HTML_TAG_HEAD:
      DfBrHtmlDiagHere (P, "unexpected-head-in-head");
      return;

    default:
      /* Any other start tag: pop head and reprocess. */
      PopHeadAndSwitchToAfterHead (P);
      P->Reprocess = DFBR_TRUE;
      return;
  }
}

static void
EndTag_InHead (DfBrHtmlParser *P)
{
  DfBrHtmlTag T = P->Pending->Tag;
  switch (T) {
    case DFBR_HTML_TAG_HEAD:
      if (TagEq (CurrentNode (P), DFBR_HTML_TAG_HEAD)) {
        StackPop (P);
        P->IM = DfBrHtmlImAfterHead;
      } else {
        DfBrHtmlDiagHere (P, "end-head-not-current");
      }
      return;
    case DFBR_HTML_TAG_BODY:
    case DFBR_HTML_TAG_HTML:
    case DFBR_HTML_TAG_BR:
      PopHeadAndSwitchToAfterHead (P);
      P->Reprocess = DFBR_TRUE;
      return;
    case DFBR_HTML_TAG_TEMPLATE:
      /* TODO: template end-tag rules. */
      EndTag_InBody (P);
      return;
    default:
      DfBrHtmlDiagHere (P, "unexpected-end-tag-in-head");
      return;
  }
}

static void
Text_InHead (DfBrHtmlParser *P, const char **Data, usize *Len)
{
  /* In InHead, whitespace is inserted as text inside head, non-ws
   * triggers implicit pop + reprocess. */
  usize i = 0;
  while ((i < *Len) && IsWS ((*Data)[i])) {
    i++;
  }
  if (i > 0) {
    InsertCharacters (P, *Data, i, DFBR_FALSE);
    *Data += i;
    *Len  -= i;
  }
  if (*Len == 0) {
    return;
  }
  PopHeadAndSwitchToAfterHead (P);
  P->Reprocess = DFBR_TRUE;
}

/* ==================================================================
 *  Mode: AfterHead
 * ==================================================================*/

static void
InsertImplicitBody (DfBrHtmlParser *P)
{
  DfBrHtmlNode *B = MakeFreshElement (P, "body", 4, DFBR_HTML_TAG_BODY);
  if (B == DFBR_NULL) {
    return;
  }
  DfBrHtmlAppendChild (CurrentNode (P), B);
  StackPush (P, B);
  P->BodyElement = B;
}

static void
StartTag_AfterHead (DfBrHtmlParser *P)
{
  DfBrHtmlTag T = P->Pending->Tag;
  switch (T) {
    case DFBR_HTML_TAG_HTML:
      StartTag_InBody (P);
      return;
    case DFBR_HTML_TAG_BODY:
      DfBrHtmlAppendChild (CurrentNode (P), P->Pending);
      StackPush (P, P->Pending);
      P->BodyElement = P->Pending;
      P->FramesetOk  = DFBR_FALSE;
      P->IM          = DfBrHtmlImInBody;
      return;
    case DFBR_HTML_TAG_FRAMESET:
      /* TODO: IMInFrameset. For v2 treat as implicit body + inline. */
      InsertImplicitBody (P);
      P->IM        = DfBrHtmlImInBody;
      P->Reprocess = DFBR_TRUE;
      return;
    case DFBR_HTML_TAG_BASE:
    case DFBR_HTML_TAG_LINK:
    case DFBR_HTML_TAG_META:
    case DFBR_HTML_TAG_SCRIPT:
    case DFBR_HTML_TAG_STYLE:
    case DFBR_HTML_TAG_TEMPLATE:
    case DFBR_HTML_TAG_TITLE:
    case DFBR_HTML_TAG_NOSCRIPT:
      /* Spec: parse error; push head back on the stack, process via
       * InHead, pop head. */
      DfBrHtmlDiagHere (P, "head-content-after-head");
      if (P->HeadElement != DFBR_NULL) {
        StackPush (P, P->HeadElement);
      }
      StartTag_InHead (P);
      if ((P->HeadElement != DFBR_NULL) &&
          TagEq (CurrentNode (P), DFBR_HTML_TAG_HEAD)) {
        StackPop (P);
      }
      return;
    case DFBR_HTML_TAG_HEAD:
      DfBrHtmlDiagHere (P, "unexpected-head-after-head");
      return;
    default:
      InsertImplicitBody (P);
      P->IM        = DfBrHtmlImInBody;
      P->Reprocess = DFBR_TRUE;
      return;
  }
}

static void
EndTag_AfterHead (DfBrHtmlParser *P)
{
  DfBrHtmlTag T = P->Pending->Tag;
  if ((T == DFBR_HTML_TAG_BODY) || (T == DFBR_HTML_TAG_HTML) ||
      (T == DFBR_HTML_TAG_BR)) {
    InsertImplicitBody (P);
    P->IM        = DfBrHtmlImInBody;
    P->Reprocess = DFBR_TRUE;
    return;
  }
  if (T == DFBR_HTML_TAG_TEMPLATE) {
    EndTag_InBody (P);   /* via InHead/Template rules (TODO) */
    return;
  }
  DfBrHtmlDiagHere (P, "unexpected-end-tag-after-head");
}

static void
Text_AfterHead (DfBrHtmlParser *P, const char **Data, usize *Len)
{
  usize i = 0;
  while ((i < *Len) && IsWS ((*Data)[i])) {
    i++;
  }
  if (i > 0) {
    InsertCharacters (P, *Data, i, DFBR_FALSE);
    *Data += i;
    *Len  -= i;
  }
  if (*Len == 0) {
    return;
  }
  InsertImplicitBody (P);
  P->IM        = DfBrHtmlImInBody;
  P->Reprocess = DFBR_TRUE;
}

/* ==================================================================
 *  Mode: InBody
 *
 *  The big one. Handles most of the body of every document.
 * ==================================================================*/

/* Close any in-scope element by tag (generic block end-tag path). */
static void
EndTagGenericBlock (DfBrHtmlParser *P, DfBrHtmlTag T)
{
  if (!HasInScope (P, T)) {
    DfBrHtmlDiagHere (P, "unexpected-end-tag");
    return;
  }
  GenerateImpliedEndTags (P, DFBR_HTML_TAG_UNKNOWN);
  if (!TagEq (CurrentNode (P), T)) {
    DfBrHtmlDiagHere (P, "end-tag-implicitly-closes");
  }
  while (P->StackTop >= 0) {
    DfBrHtmlTag Top = P->Stack[P->StackTop]->Tag;
    StackPop (P);
    if (Top == T) {
      break;
    }
  }
}

static void
StartTag_InBody (DfBrHtmlParser *P)
{
  DfBrHtmlTag   T = P->Pending->Tag;
  DfBrHtmlNode *N = P->Pending;

  switch (T) {
    case DFBR_HTML_TAG_HTML:
      /* Merge attrs into existing html element. */
      DfBrHtmlDiagHere (P, "second-html-start-tag");
      if (P->HtmlElement != DFBR_NULL) {
        MergeAttrsFrom (P, P->HtmlElement, N);
      }
      return;

    case DFBR_HTML_TAG_BASE:
    case DFBR_HTML_TAG_LINK:
    case DFBR_HTML_TAG_META:
    case DFBR_HTML_TAG_SCRIPT:
    case DFBR_HTML_TAG_STYLE:
    case DFBR_HTML_TAG_TEMPLATE:
    case DFBR_HTML_TAG_TITLE:
    case DFBR_HTML_TAG_NOSCRIPT:
      /* Delegate to InHead rules. */
      StartTag_InHead (P);
      return;

    case DFBR_HTML_TAG_BODY:
      DfBrHtmlDiagHere (P, "second-body-start-tag");
      if (P->BodyElement != DFBR_NULL) {
        MergeAttrsFrom (P, P->BodyElement, N);
        P->FramesetOk = DFBR_FALSE;
      }
      return;

    case DFBR_HTML_TAG_FRAMESET:
      /* TODO: frameset replacement when frameset-ok is true. */
      DfBrHtmlDiagHere (P, "unexpected-frameset-in-body");
      return;

    case DFBR_HTML_TAG_ADDRESS:
    case DFBR_HTML_TAG_ARTICLE:
    case DFBR_HTML_TAG_ASIDE:
    case DFBR_HTML_TAG_BLOCKQUOTE:
    case DFBR_HTML_TAG_DETAILS:
    case DFBR_HTML_TAG_DIALOG:
    case DFBR_HTML_TAG_DIV:
    case DFBR_HTML_TAG_DL:
    case DFBR_HTML_TAG_FIELDSET:
    case DFBR_HTML_TAG_FIGCAPTION:
    case DFBR_HTML_TAG_FIGURE:
    case DFBR_HTML_TAG_FOOTER:
    case DFBR_HTML_TAG_HEADER:
    case DFBR_HTML_TAG_HGROUP:
    case DFBR_HTML_TAG_MAIN:
    case DFBR_HTML_TAG_MENU:
    case DFBR_HTML_TAG_NAV:
    case DFBR_HTML_TAG_OL:
    case DFBR_HTML_TAG_P:
    case DFBR_HTML_TAG_SECTION:
    case DFBR_HTML_TAG_SUMMARY:
    case DFBR_HTML_TAG_UL:
      if (HasInButtonScope (P, DFBR_HTML_TAG_P)) {
        CloseP (P);
      }
      InsertElementPush (P, N, DFBR_FALSE);
      return;

    case DFBR_HTML_TAG_H1:
    case DFBR_HTML_TAG_H2:
    case DFBR_HTML_TAG_H3:
    case DFBR_HTML_TAG_H4:
    case DFBR_HTML_TAG_H5:
    case DFBR_HTML_TAG_H6:
      if (HasInButtonScope (P, DFBR_HTML_TAG_P)) {
        CloseP (P);
      }
      /* If current is a heading, pop it (no nested headings). */
      {
        DfBrHtmlTag Cur = CurrentNode (P) ? CurrentNode (P)->Tag
                                          : DFBR_HTML_TAG_UNKNOWN;
        if ((Cur >= DFBR_HTML_TAG_H1) && (Cur <= DFBR_HTML_TAG_H6)) {
          DfBrHtmlDiagHere (P, "heading-inside-heading");
          StackPop (P);
        }
      }
      InsertElementPush (P, N, DFBR_FALSE);
      return;

    case DFBR_HTML_TAG_PRE: {
      if (HasInButtonScope (P, DFBR_HTML_TAG_P)) {
        CloseP (P);
      }
      InsertElementPush (P, N, DFBR_FALSE);
      P->FramesetOk = DFBR_FALSE;
      /* Spec: skip a leading newline immediately after <pre>. We can
       * implement this later as a tokenizer-level hint; for now we
       * preserve the newline verbatim. */
      return;
    }

    case DFBR_HTML_TAG_FORM:
      if ((P->FormElement != DFBR_NULL) &&
          !StackHasTag (P, DFBR_HTML_TAG_TEMPLATE)) {
        DfBrHtmlDiagHere (P, "nested-form-ignored");
        return;
      }
      if (HasInButtonScope (P, DFBR_HTML_TAG_P)) {
        CloseP (P);
      }
      InsertElementPush (P, N, DFBR_FALSE);
      if (!StackHasTag (P, DFBR_HTML_TAG_TEMPLATE)) {
        P->FormElement = N;
      }
      return;

    case DFBR_HTML_TAG_LI:
      /* Per HTML spec §13.2.6.4.7 (in body) <li> start tag:
       *   loop: walk open elements top-down.
       *         if node is an <li> -> close it (pop down to that li).
       *         else if node is in the special category, but is NOT
       *             address/div/p -> stop (jump to "done").
       *         else continue loop.
       * We previously only stopped on the much smaller "default scope
       * terminator" set, which meant a nested <ul> inside the current
       * <li> didn't block the walk. That caused any dropdown-menu-style
       * <ul><li><ul><li>..</li></ul></li></ul> structure to mis-parse
       * into a flat sibling list. See IsSpecialCategory's block comment
       * for the user-visible symptom. */
      P->FramesetOk = DFBR_FALSE;
      {
        i32 i;
        for (i = P->StackTop; i >= 0; i--) {
          DfBrHtmlTag ST = P->Stack[i]->Tag;
          if (ST == DFBR_HTML_TAG_LI) {
            GenerateImpliedEndTags (P, DFBR_HTML_TAG_LI);
            while (P->StackTop >= 0) {
              DfBrHtmlTag Top = P->Stack[P->StackTop]->Tag;
              StackPop (P);
              if (Top == DFBR_HTML_TAG_LI) {
                break;
              }
            }
            break;
          }
          if (IsSpecialCategory (ST) &&
              (ST != DFBR_HTML_TAG_ADDRESS) &&
              (ST != DFBR_HTML_TAG_DIV) &&
              (ST != DFBR_HTML_TAG_P)) {
            break;
          }
        }
      }
      if (HasInButtonScope (P, DFBR_HTML_TAG_P)) {
        CloseP (P);
      }
      InsertElementPush (P, N, DFBR_FALSE);
      return;

    case DFBR_HTML_TAG_DD:
    case DFBR_HTML_TAG_DT:
      /* Same "stop at special category (except address/div/p)" rule as
       * <li> above; without it, nested <dl>s inside <dd>/<dt> get
       * hoisted up. */
      P->FramesetOk = DFBR_FALSE;
      {
        i32 i;
        for (i = P->StackTop; i >= 0; i--) {
          DfBrHtmlTag ST = P->Stack[i]->Tag;
          if ((ST == DFBR_HTML_TAG_DD) || (ST == DFBR_HTML_TAG_DT)) {
            GenerateImpliedEndTags (P, ST);
            while (P->StackTop >= 0) {
              DfBrHtmlTag Top = P->Stack[P->StackTop]->Tag;
              StackPop (P);
              if (Top == ST) {
                break;
              }
            }
            break;
          }
          if (IsSpecialCategory (ST) &&
              (ST != DFBR_HTML_TAG_ADDRESS) &&
              (ST != DFBR_HTML_TAG_DIV) &&
              (ST != DFBR_HTML_TAG_P)) {
            break;
          }
        }
      }
      if (HasInButtonScope (P, DFBR_HTML_TAG_P)) {
        CloseP (P);
      }
      InsertElementPush (P, N, DFBR_FALSE);
      return;

    case DFBR_HTML_TAG_PLAINTEXT:
      /* TODO: PLAINTEXT token state flip. Until we add the state,
       * fall through to generic element insert. */
      /* fallthrough */
    case DFBR_HTML_TAG_BUTTON:
      if (HasInScope (P, DFBR_HTML_TAG_BUTTON)) {
        DfBrHtmlDiagHere (P, "button-inside-button");
        GenerateImpliedEndTags (P, DFBR_HTML_TAG_UNKNOWN);
        while (P->StackTop >= 0) {
          DfBrHtmlTag Top = P->Stack[P->StackTop]->Tag;
          StackPop (P);
          if (Top == DFBR_HTML_TAG_BUTTON) {
            break;
          }
        }
      }
      InsertElementPush (P, N, DFBR_FALSE);
      P->FramesetOk = DFBR_FALSE;
      return;

    case DFBR_HTML_TAG_A:
      /* TODO: Adoption Agency for nested <a>. For v2: if an <a> is
       * open, emit a diagnostic but still insert. */
      if (StackHasTag (P, DFBR_HTML_TAG_A)) {
        DfBrHtmlDiagHere (P, "nested-anchor");
      }
      InsertElementPush (P, N, DFBR_FALSE);
      return;

    case DFBR_HTML_TAG_B:
    case DFBR_HTML_TAG_BIG:
    case DFBR_HTML_TAG_CODE:
    case DFBR_HTML_TAG_EM:
    case DFBR_HTML_TAG_FONT:
    case DFBR_HTML_TAG_I:
    case DFBR_HTML_TAG_S:
    case DFBR_HTML_TAG_SMALL:
    case DFBR_HTML_TAG_STRIKE:
    case DFBR_HTML_TAG_STRONG:
    case DFBR_HTML_TAG_TT:
    case DFBR_HTML_TAG_U:
      /* TODO: track in active formatting elements list and honour
       * Adoption Agency on close. For v2, just insert + push. */
      InsertElementPush (P, N, DFBR_FALSE);
      return;

    case DFBR_HTML_TAG_NOBR:
      if (HasInScope (P, DFBR_HTML_TAG_NOBR)) {
        DfBrHtmlDiagHere (P, "nested-nobr");
        EndTagGenericBlock (P, DFBR_HTML_TAG_NOBR);
      }
      InsertElementPush (P, N, DFBR_FALSE);
      return;

    case DFBR_HTML_TAG_APPLET:
    case DFBR_HTML_TAG_MARQUEE:
    case DFBR_HTML_TAG_OBJECT:
      InsertElementPush (P, N, DFBR_FALSE);
      P->FramesetOk = DFBR_FALSE;
      return;

    case DFBR_HTML_TAG_TABLE:
      /* TODO: quirks-mode bypass of close-p. */
      if (HasInButtonScope (P, DFBR_HTML_TAG_P)) {
        CloseP (P);
      }
      InsertElementPush (P, N, DFBR_FALSE);
      P->FramesetOk = DFBR_FALSE;
      P->IM         = DfBrHtmlImInTable;
      return;

    case DFBR_HTML_TAG_AREA:
    case DFBR_HTML_TAG_BR:
    case DFBR_HTML_TAG_EMBED:
    case DFBR_HTML_TAG_IMG:
    case DFBR_HTML_TAG_KEYGEN:
    case DFBR_HTML_TAG_WBR:
      InsertVoidElement (P, N, DFBR_FALSE);
      P->FramesetOk = DFBR_FALSE;
      return;

    case DFBR_HTML_TAG_INPUT: {
      const char *Type = DfBrHtmlNodeGetAttrValue (N, "type");
      InsertVoidElement (P, N, DFBR_FALSE);
      if ((Type == DFBR_NULL) ||
          (strcmp (Type, "hidden") != 0)) {
        P->FramesetOk = DFBR_FALSE;
      }
      return;
    }

    case DFBR_HTML_TAG_PARAM:
    case DFBR_HTML_TAG_SOURCE:
    case DFBR_HTML_TAG_TRACK:
      InsertVoidElement (P, N, DFBR_FALSE);
      return;

    case DFBR_HTML_TAG_HR:
      if (HasInButtonScope (P, DFBR_HTML_TAG_P)) {
        CloseP (P);
      }
      InsertVoidElement (P, N, DFBR_FALSE);
      P->FramesetOk = DFBR_FALSE;
      return;

    case DFBR_HTML_TAG_IMAGE:
      /* Spec: "parse error, change the token's tag name to img". */
      DfBrHtmlDiagHere (P, "image-tag-should-be-img");
      N->Tag     = DFBR_HTML_TAG_IMG;
      N->Name    = DfBrHtmlArenaStrDup (&P->Doc->Arena, "img", 3);
      N->NameLen = 3;
      InsertVoidElement (P, N, DFBR_FALSE);
      P->FramesetOk = DFBR_FALSE;
      return;

    case DFBR_HTML_TAG_TEXTAREA:
      DfBrHtmlAppendChild (CurrentNode (P), N);
      StackPush (P, N);
      P->FramesetOk = DFBR_FALSE;
      SwitchToText (P, N, DFBR_FALSE);   /* RCDATA */
      return;

    case DFBR_HTML_TAG_XMP:
    case DFBR_HTML_TAG_IFRAME:
    case DFBR_HTML_TAG_NOEMBED:
    case DFBR_HTML_TAG_NOFRAMES:
      DfBrHtmlAppendChild (CurrentNode (P), N);
      StackPush (P, N);
      P->FramesetOk = DFBR_FALSE;
      SwitchToText (P, N, DFBR_TRUE);    /* RAWTEXT */
      return;

    case DFBR_HTML_TAG_SELECT:
      /* TODO: InSelect / InSelectInTable. For v2: insert like a
       * normal element; its children will parse through InBody
       * which is close enough to get options into the tree. */
      InsertElementPush (P, N, DFBR_FALSE);
      P->FramesetOk = DFBR_FALSE;
      return;

    case DFBR_HTML_TAG_OPTION:
    case DFBR_HTML_TAG_OPTGROUP:
      if (TagEq (CurrentNode (P), DFBR_HTML_TAG_OPTION)) {
        StackPop (P);
      }
      if ((T == DFBR_HTML_TAG_OPTGROUP) &&
          TagEq (CurrentNode (P), DFBR_HTML_TAG_OPTGROUP)) {
        StackPop (P);
      }
      InsertElementPush (P, N, DFBR_FALSE);
      return;

    case DFBR_HTML_TAG_RB:
    case DFBR_HTML_TAG_RTC:
      if (HasInScope (P, DFBR_HTML_TAG_RUBY)) {
        GenerateImpliedEndTags (P, DFBR_HTML_TAG_UNKNOWN);
      }
      InsertElementPush (P, N, DFBR_FALSE);
      return;

    case DFBR_HTML_TAG_RP:
    case DFBR_HTML_TAG_RT:
      if (HasInScope (P, DFBR_HTML_TAG_RUBY)) {
        GenerateImpliedEndTags (P, DFBR_HTML_TAG_RTC);
      }
      InsertElementPush (P, N, DFBR_FALSE);
      return;

    case DFBR_HTML_TAG_MATH:
    case DFBR_HTML_TAG_SVG:
      /* TODO: foreign content insertion mode. For v2 insert as normal. */
      InsertElementPush (P, N, DFBR_FALSE);
      return;

    case DFBR_HTML_TAG_CAPTION:
    case DFBR_HTML_TAG_COL:
    case DFBR_HTML_TAG_COLGROUP:
    case DFBR_HTML_TAG_FRAME:
    case DFBR_HTML_TAG_HEAD:
    case DFBR_HTML_TAG_TBODY:
    case DFBR_HTML_TAG_TD:
    case DFBR_HTML_TAG_TFOOT:
    case DFBR_HTML_TAG_TH:
    case DFBR_HTML_TAG_THEAD:
    case DFBR_HTML_TAG_TR:
      DfBrHtmlDiagHere (P, "table-or-head-tag-outside-table");
      return;

    default:
      /* Any other start tag: insert as generic element. */
      InsertElementPush (P, N, DFBR_FALSE);
      return;
  }
}

static void
EndTag_InBody (DfBrHtmlParser *P)
{
  DfBrHtmlTag T = P->Pending->Tag;

  switch (T) {
    case DFBR_HTML_TAG_TEMPLATE:
      /* TODO: template end-tag rules. */
      EndTagGenericBlock (P, T);
      return;

    case DFBR_HTML_TAG_BODY:
      if (!HasInScope (P, DFBR_HTML_TAG_BODY)) {
        DfBrHtmlDiagHere (P, "unexpected-end-body-no-body-in-scope");
        return;
      }
      /* Spec: don't pop body; just switch mode. */
      P->IM = DfBrHtmlImAfterBody;
      return;

    case DFBR_HTML_TAG_HTML:
      if (!HasInScope (P, DFBR_HTML_TAG_BODY)) {
        DfBrHtmlDiagHere (P, "unexpected-end-html-no-body-in-scope");
        return;
      }
      P->IM        = DfBrHtmlImAfterBody;
      P->Reprocess = DFBR_TRUE;
      return;

    case DFBR_HTML_TAG_P:
      if (!HasInButtonScope (P, DFBR_HTML_TAG_P)) {
        DfBrHtmlDiagHere (P, "missing-p-for-end-p");
        /* Spec: insert an empty <p> and close it. Equivalent tree
         * effect for our purposes is: push a fresh p, then close. */
        {
          DfBrHtmlNode *Pn = MakeFreshElement (P, "p", 1, DFBR_HTML_TAG_P);
          if (Pn != DFBR_NULL) {
            InsertionLocation Loc = AdjustedInsertionLocation (P, DFBR_FALSE);
            InsertAt (Loc, Pn);
            StackPush (P, Pn);
          }
        }
      }
      CloseP (P);
      return;

    case DFBR_HTML_TAG_LI:
      if (!HasInListItemScope (P, DFBR_HTML_TAG_LI)) {
        DfBrHtmlDiagHere (P, "missing-li-for-end-li");
        return;
      }
      GenerateImpliedEndTags (P, DFBR_HTML_TAG_LI);
      while (P->StackTop >= 0) {
        DfBrHtmlTag Top = P->Stack[P->StackTop]->Tag;
        StackPop (P);
        if (Top == DFBR_HTML_TAG_LI) {
          break;
        }
      }
      return;

    case DFBR_HTML_TAG_DD:
    case DFBR_HTML_TAG_DT:
      if (!HasInScope (P, T)) {
        DfBrHtmlDiagHere (P, "missing-dd-dt-for-end");
        return;
      }
      GenerateImpliedEndTags (P, T);
      while (P->StackTop >= 0) {
        DfBrHtmlTag Top = P->Stack[P->StackTop]->Tag;
        StackPop (P);
        if (Top == T) {
          break;
        }
      }
      return;

    case DFBR_HTML_TAG_H1:
    case DFBR_HTML_TAG_H2:
    case DFBR_HTML_TAG_H3:
    case DFBR_HTML_TAG_H4:
    case DFBR_HTML_TAG_H5:
    case DFBR_HTML_TAG_H6:
      {
        /* "have element in scope" that is any heading. */
        b32 AnyHeading = DFBR_FALSE;
        i32 i;
        for (i = P->StackTop; i >= 0; i--) {
          DfBrHtmlTag ST = P->Stack[i]->Tag;
          if ((ST >= DFBR_HTML_TAG_H1) && (ST <= DFBR_HTML_TAG_H6)) {
            AnyHeading = DFBR_TRUE;
            break;
          }
          if (IsDefaultScopeTerminator (ST)) {
            break;
          }
        }
        if (!AnyHeading) {
          DfBrHtmlDiagHere (P, "missing-heading-for-end-heading");
          return;
        }
      }
      GenerateImpliedEndTags (P, DFBR_HTML_TAG_UNKNOWN);
      while (P->StackTop >= 0) {
        DfBrHtmlTag Top = P->Stack[P->StackTop]->Tag;
        StackPop (P);
        if ((Top >= DFBR_HTML_TAG_H1) && (Top <= DFBR_HTML_TAG_H6)) {
          break;
        }
      }
      return;

    case DFBR_HTML_TAG_FORM:
      /* Simplified: close generically. (Spec has template-aware
       * behaviour.) */
      EndTagGenericBlock (P, DFBR_HTML_TAG_FORM);
      P->FormElement = DFBR_NULL;
      return;

    /* Adoption-agency territory - simplified generic close. */
    case DFBR_HTML_TAG_A:
    case DFBR_HTML_TAG_B:
    case DFBR_HTML_TAG_BIG:
    case DFBR_HTML_TAG_CODE:
    case DFBR_HTML_TAG_EM:
    case DFBR_HTML_TAG_FONT:
    case DFBR_HTML_TAG_I:
    case DFBR_HTML_TAG_NOBR:
    case DFBR_HTML_TAG_S:
    case DFBR_HTML_TAG_SMALL:
    case DFBR_HTML_TAG_STRIKE:
    case DFBR_HTML_TAG_STRONG:
    case DFBR_HTML_TAG_TT:
    case DFBR_HTML_TAG_U:
      /* TODO: Adoption Agency Algorithm. */
      EndTagGenericBlock (P, T);
      return;

    case DFBR_HTML_TAG_APPLET:
    case DFBR_HTML_TAG_MARQUEE:
    case DFBR_HTML_TAG_OBJECT:
      EndTagGenericBlock (P, T);
      return;

    case DFBR_HTML_TAG_BR:
      /* Spec: parse error; treat as a start tag <br>. */
      DfBrHtmlDiagHere (P, "end-br-treated-as-start");
      P->Pending->Kind = DFBR_HTML_NODE_ELEMENT;
      P->PendingIsEnd  = DFBR_FALSE;
      P->Pending->FirstAttr = DFBR_NULL;   /* per spec, ignore attrs */
      StartTag_InBody (P);
      return;

    case DFBR_HTML_TAG_ADDRESS:
    case DFBR_HTML_TAG_ARTICLE:
    case DFBR_HTML_TAG_ASIDE:
    case DFBR_HTML_TAG_BLOCKQUOTE:
    case DFBR_HTML_TAG_BUTTON:
    case DFBR_HTML_TAG_CENTER:
    case DFBR_HTML_TAG_DETAILS:
    case DFBR_HTML_TAG_DIALOG:
    case DFBR_HTML_TAG_DIR:
    case DFBR_HTML_TAG_DIV:
    case DFBR_HTML_TAG_DL:
    case DFBR_HTML_TAG_FIELDSET:
    case DFBR_HTML_TAG_FIGCAPTION:
    case DFBR_HTML_TAG_FIGURE:
    case DFBR_HTML_TAG_FOOTER:
    case DFBR_HTML_TAG_HEADER:
    case DFBR_HTML_TAG_HGROUP:
    case DFBR_HTML_TAG_LISTING:
    case DFBR_HTML_TAG_MAIN:
    case DFBR_HTML_TAG_MENU:
    case DFBR_HTML_TAG_NAV:
    case DFBR_HTML_TAG_OL:
    case DFBR_HTML_TAG_PRE:
    case DFBR_HTML_TAG_SECTION:
    case DFBR_HTML_TAG_SUMMARY:
    case DFBR_HTML_TAG_UL:
      EndTagGenericBlock (P, T);
      return;

    default:
      /* Generic path: walk stack top-down looking for a name match,
       * generate implied end tags, pop including. */
      {
        i32           i;
        DfBrHtmlNode *Tok = P->Pending;
        for (i = P->StackTop; i >= 0; i--) {
          DfBrHtmlNode *NodeI = P->Stack[i];
          b32 Match = DFBR_FALSE;
          if (Tok->Tag != DFBR_HTML_TAG_UNKNOWN) {
            Match = (NodeI->Tag == Tok->Tag);
          } else {
            Match = (NodeI->NameLen == Tok->NameLen) &&
                    (Tok->NameLen == 0 ||
                     memcmp (NodeI->Name, Tok->Name, Tok->NameLen) == 0);
          }
          if (Match) {
            GenerateImpliedEndTags (P, NodeI->Tag);
            if (i != P->StackTop) {
              DfBrHtmlDiagHere (P, "end-tag-implicitly-closes");
            }
            StackPopTo (P, i);
            return;
          }
          if (IsDefaultScopeTerminator (NodeI->Tag)) {
            /* Reached a scope terminator without finding a match. */
            DfBrHtmlDiagHere (P, "unexpected-end-tag");
            return;
          }
        }
        DfBrHtmlDiagHere (P, "unexpected-end-tag");
      }
      return;
  }
}

static void
Text_InBody (DfBrHtmlParser *P, const char *Data, usize Len)
{
  if (Len == 0) {
    return;
  }
  InsertCharacters (P, Data, Len, DFBR_FALSE);
  if (!AllWhitespace (Data, Len)) {
    P->FramesetOk = DFBR_FALSE;
  }
}

/* ==================================================================
 *  Mode: Text (RAWTEXT / RCDATA body)
 * ==================================================================*/

static void
StartTag_Text (DfBrHtmlParser *P)
{
  /* Can't happen from tokenizer in rawtext/rcdata lexical mode except
   * as a synthesised end-tag match. Treat defensively. */
  DfBrHtmlDiagHere (P, "start-tag-in-text-mode");
  (void)P;
}

static void
EndTag_Text (DfBrHtmlParser *P)
{
  if (P->StackTop >= 0) {
    StackPop (P);
  }
  P->IM = P->OriginalIM;
}

static void
Text_Text (DfBrHtmlParser *P, const char *Data, usize Len)
{
  InsertCharacters (P, Data, Len, DFBR_FALSE);
}

/* ==================================================================
 *  Mode: InTable
 * ==================================================================*/

static void
StartTag_InTable (DfBrHtmlParser *P)
{
  DfBrHtmlTag   T = P->Pending->Tag;
  DfBrHtmlNode *N = P->Pending;

  switch (T) {
    case DFBR_HTML_TAG_CAPTION:
      ClearBackToTableContext (P);
      /* TODO: insert marker in active formatting elements list. */
      InsertElementPush (P, N, DFBR_FALSE);
      P->IM = DfBrHtmlImInCaption;
      return;

    case DFBR_HTML_TAG_COLGROUP:
      ClearBackToTableContext (P);
      InsertElementPush (P, N, DFBR_FALSE);
      P->IM = DfBrHtmlImInColumnGroup;
      return;

    case DFBR_HTML_TAG_COL:
      ClearBackToTableContext (P);
      {
        DfBrHtmlNode *CG = MakeFreshElement (P, "colgroup", 8,
                                             DFBR_HTML_TAG_COLGROUP);
        if (CG != DFBR_NULL) {
          DfBrHtmlAppendChild (CurrentNode (P), CG);
          StackPush (P, CG);
          P->IM = DfBrHtmlImInColumnGroup;
        }
      }
      P->Reprocess = DFBR_TRUE;
      return;

    case DFBR_HTML_TAG_TBODY:
    case DFBR_HTML_TAG_TFOOT:
    case DFBR_HTML_TAG_THEAD:
      ClearBackToTableContext (P);
      InsertElementPush (P, N, DFBR_FALSE);
      P->IM = DfBrHtmlImInTableBody;
      return;

    case DFBR_HTML_TAG_TD:
    case DFBR_HTML_TAG_TH:
    case DFBR_HTML_TAG_TR:
      ClearBackToTableContext (P);
      {
        DfBrHtmlNode *TB = MakeFreshElement (P, "tbody", 5,
                                             DFBR_HTML_TAG_TBODY);
        if (TB != DFBR_NULL) {
          DfBrHtmlAppendChild (CurrentNode (P), TB);
          StackPush (P, TB);
          P->IM = DfBrHtmlImInTableBody;
        }
      }
      P->Reprocess = DFBR_TRUE;
      return;

    case DFBR_HTML_TAG_TABLE:
      DfBrHtmlDiagHere (P, "nested-table");
      if (!HasInTableScope (P, DFBR_HTML_TAG_TABLE)) {
        return;
      }
      while (P->StackTop >= 0) {
        DfBrHtmlTag Top = P->Stack[P->StackTop]->Tag;
        StackPop (P);
        if (Top == DFBR_HTML_TAG_TABLE) {
          break;
        }
      }
      ResetInsertionMode (P);
      P->Reprocess = DFBR_TRUE;
      return;

    case DFBR_HTML_TAG_STYLE:
    case DFBR_HTML_TAG_SCRIPT:
    case DFBR_HTML_TAG_TEMPLATE:
      StartTag_InHead (P);
      return;

    case DFBR_HTML_TAG_INPUT: {
      /* <input type=hidden> is allowed directly in a table; everything
       * else falls through to foster parenting. */
      const char *Ty = DfBrHtmlNodeGetAttrValue (N, "type");
      if ((Ty != DFBR_NULL) && (strcmp (Ty, "hidden") == 0)) {
        DfBrHtmlDiagHere (P, "input-hidden-in-table");
        InsertVoidElement (P, N, DFBR_FALSE);
        return;
      }
      /* fall through to foster */
      break;
    }

    case DFBR_HTML_TAG_FORM:
      DfBrHtmlDiagHere (P, "form-in-table");
      if ((P->FormElement != DFBR_NULL) ||
          StackHasTag (P, DFBR_HTML_TAG_TEMPLATE)) {
        return;
      }
      /* Spec wants form inserted but not pushed, and form pointer set.
       * Simplified: do the same with foster parenting. */
      InsertVoidElement (P, N, DFBR_FALSE);
      P->FormElement = N;
      return;

    default:
      break;
  }

  /* Everything else: parse error, foster parent as InBody. */
  DfBrHtmlDiagHere (P, "unexpected-in-table-foster-parent");
  {
    DfBrHtmlIM  Saved = P->IM;
    /* We use InsertElementPush with AllowFoster=TRUE by re-dispatching
     * through InBody but also applying foster parenting. The easiest
     * route that respects both is to set a "foster on next insert"
     * flag - simpler: call InBody but when it tries to insert this
     * pending element, it'll honour foster via AllowFoster=TRUE. We
     * achieve that by swapping AdjustedInsertionLocation's AllowFoster
     * logic based on IM: keep IM=InTable, call the InBody handler. */
    (void)Saved;
    /* Small trick: temporarily treat as InBody but still-in-table so
     * AdjustedInsertionLocation sees a table context. Cleanest: inline
     * the "insert element" path with foster=TRUE here. */
    if (P->Pending != DFBR_NULL) {
      DfBrHtmlNode *N2 = P->Pending;
      if (DfBrHtmlTagIsVoid (N2->Tag)) {
        InsertVoidElement (P, N2, DFBR_TRUE);
      } else {
        InsertElementPush (P, N2, DFBR_TRUE);
      }
    }
  }
}

static void
EndTag_InTable (DfBrHtmlParser *P)
{
  DfBrHtmlTag T = P->Pending->Tag;

  switch (T) {
    case DFBR_HTML_TAG_TABLE:
      if (!HasInTableScope (P, DFBR_HTML_TAG_TABLE)) {
        DfBrHtmlDiagHere (P, "end-table-out-of-scope");
        return;
      }
      while (P->StackTop >= 0) {
        DfBrHtmlTag Top = P->Stack[P->StackTop]->Tag;
        StackPop (P);
        if (Top == DFBR_HTML_TAG_TABLE) {
          break;
        }
      }
      ResetInsertionMode (P);
      return;

    case DFBR_HTML_TAG_BODY:
    case DFBR_HTML_TAG_CAPTION:
    case DFBR_HTML_TAG_COL:
    case DFBR_HTML_TAG_COLGROUP:
    case DFBR_HTML_TAG_HTML:
    case DFBR_HTML_TAG_TBODY:
    case DFBR_HTML_TAG_TD:
    case DFBR_HTML_TAG_TFOOT:
    case DFBR_HTML_TAG_TH:
    case DFBR_HTML_TAG_THEAD:
    case DFBR_HTML_TAG_TR:
      DfBrHtmlDiagHere (P, "unexpected-end-tag-in-table");
      return;

    case DFBR_HTML_TAG_TEMPLATE:
      /* TODO: template end-tag rules. */
      EndTag_InBody (P);
      return;

    default:
      DfBrHtmlDiagHere (P, "end-tag-in-table-foster");
      EndTag_InBody (P);
      return;
  }
}

/* Text in InTable: if all whitespace, insert normally; otherwise
 * foster-parent the entire batch. (Simplification of spec's
 * InTableText mode - we don't buffer across tokens.) */
static void
Text_InTable (DfBrHtmlParser *P, const char *Data, usize Len)
{
  if (AllWhitespace (Data, Len)) {
    InsertCharacters (P, Data, Len, DFBR_FALSE);
    return;
  }
  DfBrHtmlDiagHere (P, "text-in-table-foster");
  InsertCharacters (P, Data, Len, DFBR_TRUE);
  P->FramesetOk = DFBR_FALSE;
}

/* ==================================================================
 *  Mode: InCaption
 * ==================================================================*/

static b32
CloseCaptionIfInScope (DfBrHtmlParser *P, const char *DiagMsg)
{
  if (!HasInTableScope (P, DFBR_HTML_TAG_CAPTION)) {
    DfBrHtmlDiagHere (P, DiagMsg);
    return DFBR_FALSE;
  }
  GenerateImpliedEndTagsThoroughly (P);
  while (P->StackTop >= 0) {
    DfBrHtmlTag Top = P->Stack[P->StackTop]->Tag;
    StackPop (P);
    if (Top == DFBR_HTML_TAG_CAPTION) {
      break;
    }
  }
  P->IM = DfBrHtmlImInTable;
  return DFBR_TRUE;
}

static void
StartTag_InCaption (DfBrHtmlParser *P)
{
  DfBrHtmlTag T = P->Pending->Tag;
  switch (T) {
    case DFBR_HTML_TAG_CAPTION:
    case DFBR_HTML_TAG_COL:
    case DFBR_HTML_TAG_COLGROUP:
    case DFBR_HTML_TAG_TBODY:
    case DFBR_HTML_TAG_TD:
    case DFBR_HTML_TAG_TFOOT:
    case DFBR_HTML_TAG_TH:
    case DFBR_HTML_TAG_THEAD:
    case DFBR_HTML_TAG_TR:
      if (CloseCaptionIfInScope (P, "start-in-caption-no-caption-scope")) {
        P->Reprocess = DFBR_TRUE;
      }
      return;
    default:
      StartTag_InBody (P);
      return;
  }
}

static void
EndTag_InCaption (DfBrHtmlParser *P)
{
  DfBrHtmlTag T = P->Pending->Tag;
  if (T == DFBR_HTML_TAG_CAPTION) {
    CloseCaptionIfInScope (P, "end-caption-no-caption-scope");
    return;
  }
  if (T == DFBR_HTML_TAG_TABLE) {
    if (CloseCaptionIfInScope (P, "end-table-in-caption-no-caption-scope")) {
      P->Reprocess = DFBR_TRUE;
    }
    return;
  }
  switch (T) {
    case DFBR_HTML_TAG_BODY:
    case DFBR_HTML_TAG_COL:
    case DFBR_HTML_TAG_COLGROUP:
    case DFBR_HTML_TAG_HTML:
    case DFBR_HTML_TAG_TBODY:
    case DFBR_HTML_TAG_TD:
    case DFBR_HTML_TAG_TFOOT:
    case DFBR_HTML_TAG_TH:
    case DFBR_HTML_TAG_THEAD:
    case DFBR_HTML_TAG_TR:
      DfBrHtmlDiagHere (P, "unexpected-end-tag-in-caption");
      return;
    default:
      EndTag_InBody (P);
      return;
  }
}

/* ==================================================================
 *  Mode: InColumnGroup
 * ==================================================================*/

static void
StartTag_InColumnGroup (DfBrHtmlParser *P)
{
  DfBrHtmlTag T = P->Pending->Tag;
  if (T == DFBR_HTML_TAG_HTML) {
    StartTag_InBody (P);
    return;
  }
  if (T == DFBR_HTML_TAG_COL) {
    InsertVoidElement (P, P->Pending, DFBR_FALSE);
    return;
  }
  if (T == DFBR_HTML_TAG_TEMPLATE) {
    StartTag_InHead (P);
    return;
  }
  /* Anything else: close colgroup implicitly, reprocess in InTable. */
  if (!TagEq (CurrentNode (P), DFBR_HTML_TAG_COLGROUP)) {
    DfBrHtmlDiagHere (P, "no-colgroup-to-close");
    return;
  }
  StackPop (P);
  P->IM        = DfBrHtmlImInTable;
  P->Reprocess = DFBR_TRUE;
}

static void
EndTag_InColumnGroup (DfBrHtmlParser *P)
{
  DfBrHtmlTag T = P->Pending->Tag;
  if (T == DFBR_HTML_TAG_COLGROUP) {
    if (!TagEq (CurrentNode (P), DFBR_HTML_TAG_COLGROUP)) {
      DfBrHtmlDiagHere (P, "end-colgroup-no-colgroup");
      return;
    }
    StackPop (P);
    P->IM = DfBrHtmlImInTable;
    return;
  }
  if (T == DFBR_HTML_TAG_COL) {
    DfBrHtmlDiagHere (P, "end-col-in-colgroup");
    return;
  }
  if (T == DFBR_HTML_TAG_TEMPLATE) {
    EndTag_InBody (P);   /* TODO: template rules */
    return;
  }
  if (!TagEq (CurrentNode (P), DFBR_HTML_TAG_COLGROUP)) {
    DfBrHtmlDiagHere (P, "end-in-colgroup-no-colgroup");
    return;
  }
  StackPop (P);
  P->IM        = DfBrHtmlImInTable;
  P->Reprocess = DFBR_TRUE;
}

/* ==================================================================
 *  Mode: InTableBody
 * ==================================================================*/

static void
StartTag_InTableBody (DfBrHtmlParser *P)
{
  DfBrHtmlTag T = P->Pending->Tag;
  switch (T) {
    case DFBR_HTML_TAG_TR:
      ClearBackToTableBodyContext (P);
      InsertElementPush (P, P->Pending, DFBR_FALSE);
      P->IM = DfBrHtmlImInRow;
      return;
    case DFBR_HTML_TAG_TH:
    case DFBR_HTML_TAG_TD:
      DfBrHtmlDiagHere (P, "cell-without-tr");
      ClearBackToTableBodyContext (P);
      {
        DfBrHtmlNode *Tr = MakeFreshElement (P, "tr", 2, DFBR_HTML_TAG_TR);
        if (Tr != DFBR_NULL) {
          DfBrHtmlAppendChild (CurrentNode (P), Tr);
          StackPush (P, Tr);
          P->IM = DfBrHtmlImInRow;
        }
      }
      P->Reprocess = DFBR_TRUE;
      return;
    case DFBR_HTML_TAG_CAPTION:
    case DFBR_HTML_TAG_COL:
    case DFBR_HTML_TAG_COLGROUP:
    case DFBR_HTML_TAG_TBODY:
    case DFBR_HTML_TAG_TFOOT:
    case DFBR_HTML_TAG_THEAD:
      if (!HasInTableScope (P, DFBR_HTML_TAG_TBODY) &&
          !HasInTableScope (P, DFBR_HTML_TAG_THEAD) &&
          !HasInTableScope (P, DFBR_HTML_TAG_TFOOT)) {
        DfBrHtmlDiagHere (P, "start-in-table-body-no-body");
        return;
      }
      ClearBackToTableBodyContext (P);
      StackPop (P);   /* pop tbody/thead/tfoot */
      P->IM        = DfBrHtmlImInTable;
      P->Reprocess = DFBR_TRUE;
      return;
    default:
      StartTag_InTable (P);
      return;
  }
}

static void
EndTag_InTableBody (DfBrHtmlParser *P)
{
  DfBrHtmlTag T = P->Pending->Tag;
  switch (T) {
    case DFBR_HTML_TAG_TBODY:
    case DFBR_HTML_TAG_THEAD:
    case DFBR_HTML_TAG_TFOOT:
      if (!HasInTableScope (P, T)) {
        DfBrHtmlDiagHere (P, "end-body-group-out-of-scope");
        return;
      }
      ClearBackToTableBodyContext (P);
      StackPop (P);
      P->IM = DfBrHtmlImInTable;
      return;
    case DFBR_HTML_TAG_TABLE:
      if (!HasInTableScope (P, DFBR_HTML_TAG_TBODY) &&
          !HasInTableScope (P, DFBR_HTML_TAG_THEAD) &&
          !HasInTableScope (P, DFBR_HTML_TAG_TFOOT)) {
        DfBrHtmlDiagHere (P, "end-table-in-body-no-body");
        return;
      }
      ClearBackToTableBodyContext (P);
      StackPop (P);
      P->IM        = DfBrHtmlImInTable;
      P->Reprocess = DFBR_TRUE;
      return;
    case DFBR_HTML_TAG_BODY:
    case DFBR_HTML_TAG_CAPTION:
    case DFBR_HTML_TAG_COL:
    case DFBR_HTML_TAG_COLGROUP:
    case DFBR_HTML_TAG_HTML:
    case DFBR_HTML_TAG_TD:
    case DFBR_HTML_TAG_TH:
    case DFBR_HTML_TAG_TR:
      DfBrHtmlDiagHere (P, "unexpected-end-in-table-body");
      return;
    default:
      EndTag_InTable (P);
      return;
  }
}

/* ==================================================================
 *  Mode: InRow
 * ==================================================================*/

static void
StartTag_InRow (DfBrHtmlParser *P)
{
  DfBrHtmlTag T = P->Pending->Tag;
  switch (T) {
    case DFBR_HTML_TAG_TH:
    case DFBR_HTML_TAG_TD:
      ClearBackToTableRowContext (P);
      InsertElementPush (P, P->Pending, DFBR_FALSE);
      P->IM = DfBrHtmlImInCell;
      /* TODO: insert marker in active formatting elements. */
      return;
    case DFBR_HTML_TAG_CAPTION:
    case DFBR_HTML_TAG_COL:
    case DFBR_HTML_TAG_COLGROUP:
    case DFBR_HTML_TAG_TBODY:
    case DFBR_HTML_TAG_TFOOT:
    case DFBR_HTML_TAG_THEAD:
    case DFBR_HTML_TAG_TR:
      if (!HasInTableScope (P, DFBR_HTML_TAG_TR)) {
        DfBrHtmlDiagHere (P, "start-in-row-no-tr");
        return;
      }
      ClearBackToTableRowContext (P);
      StackPop (P);    /* pop tr */
      P->IM        = DfBrHtmlImInTableBody;
      P->Reprocess = DFBR_TRUE;
      return;
    default:
      StartTag_InTable (P);
      return;
  }
}

static void
EndTag_InRow (DfBrHtmlParser *P)
{
  DfBrHtmlTag T = P->Pending->Tag;
  switch (T) {
    case DFBR_HTML_TAG_TR:
      if (!HasInTableScope (P, DFBR_HTML_TAG_TR)) {
        DfBrHtmlDiagHere (P, "end-tr-no-tr");
        return;
      }
      ClearBackToTableRowContext (P);
      StackPop (P);
      P->IM = DfBrHtmlImInTableBody;
      return;
    case DFBR_HTML_TAG_TABLE:
      if (!HasInTableScope (P, DFBR_HTML_TAG_TR)) {
        DfBrHtmlDiagHere (P, "end-table-no-tr");
        return;
      }
      ClearBackToTableRowContext (P);
      StackPop (P);
      P->IM        = DfBrHtmlImInTableBody;
      P->Reprocess = DFBR_TRUE;
      return;
    case DFBR_HTML_TAG_TBODY:
    case DFBR_HTML_TAG_TFOOT:
    case DFBR_HTML_TAG_THEAD:
      if (!HasInTableScope (P, T)) {
        DfBrHtmlDiagHere (P, "end-body-group-not-in-scope");
        return;
      }
      if (!HasInTableScope (P, DFBR_HTML_TAG_TR)) {
        return;
      }
      ClearBackToTableRowContext (P);
      StackPop (P);
      P->IM        = DfBrHtmlImInTableBody;
      P->Reprocess = DFBR_TRUE;
      return;
    case DFBR_HTML_TAG_BODY:
    case DFBR_HTML_TAG_CAPTION:
    case DFBR_HTML_TAG_COL:
    case DFBR_HTML_TAG_COLGROUP:
    case DFBR_HTML_TAG_HTML:
    case DFBR_HTML_TAG_TD:
    case DFBR_HTML_TAG_TH:
      DfBrHtmlDiagHere (P, "unexpected-end-in-row");
      return;
    default:
      EndTag_InTable (P);
      return;
  }
}

/* ==================================================================
 *  Mode: InCell
 * ==================================================================*/

static void
StartTag_InCell (DfBrHtmlParser *P)
{
  DfBrHtmlTag T = P->Pending->Tag;
  switch (T) {
    case DFBR_HTML_TAG_CAPTION:
    case DFBR_HTML_TAG_COL:
    case DFBR_HTML_TAG_COLGROUP:
    case DFBR_HTML_TAG_TBODY:
    case DFBR_HTML_TAG_TD:
    case DFBR_HTML_TAG_TFOOT:
    case DFBR_HTML_TAG_TH:
    case DFBR_HTML_TAG_THEAD:
    case DFBR_HTML_TAG_TR:
      if (!HasInTableScope (P, DFBR_HTML_TAG_TD) &&
          !HasInTableScope (P, DFBR_HTML_TAG_TH)) {
        DfBrHtmlDiagHere (P, "start-in-cell-no-cell");
        return;
      }
      CloseCell (P);
      P->Reprocess = DFBR_TRUE;
      return;
    default:
      StartTag_InBody (P);
      return;
  }
}

static void
EndTag_InCell (DfBrHtmlParser *P)
{
  DfBrHtmlTag T = P->Pending->Tag;
  switch (T) {
    case DFBR_HTML_TAG_TD:
    case DFBR_HTML_TAG_TH:
      if (!HasInTableScope (P, T)) {
        DfBrHtmlDiagHere (P, "end-cell-out-of-scope");
        return;
      }
      GenerateImpliedEndTags (P, DFBR_HTML_TAG_UNKNOWN);
      if (!TagEq (CurrentNode (P), T)) {
        DfBrHtmlDiagHere (P, "end-cell-not-current");
      }
      while (P->StackTop >= 0) {
        DfBrHtmlTag Top = P->Stack[P->StackTop]->Tag;
        StackPop (P);
        if (Top == T) {
          break;
        }
      }
      P->IM = DfBrHtmlImInRow;
      return;
    case DFBR_HTML_TAG_BODY:
    case DFBR_HTML_TAG_CAPTION:
    case DFBR_HTML_TAG_COL:
    case DFBR_HTML_TAG_COLGROUP:
    case DFBR_HTML_TAG_HTML:
      DfBrHtmlDiagHere (P, "unexpected-end-in-cell");
      return;
    case DFBR_HTML_TAG_TABLE:
    case DFBR_HTML_TAG_TBODY:
    case DFBR_HTML_TAG_TFOOT:
    case DFBR_HTML_TAG_THEAD:
    case DFBR_HTML_TAG_TR:
      if (!HasInTableScope (P, T)) {
        DfBrHtmlDiagHere (P, "end-table-stuff-in-cell-out-of-scope");
        return;
      }
      CloseCell (P);
      P->Reprocess = DFBR_TRUE;
      return;
    default:
      EndTag_InBody (P);
      return;
  }
}

/* ==================================================================
 *  Mode: AfterBody
 * ==================================================================*/

static void
StartTag_AfterBody (DfBrHtmlParser *P)
{
  if (P->Pending->Tag == DFBR_HTML_TAG_HTML) {
    StartTag_InBody (P);
    return;
  }
  DfBrHtmlDiagHere (P, "start-tag-after-body");
  P->IM        = DfBrHtmlImInBody;
  P->Reprocess = DFBR_TRUE;
}

static void
EndTag_AfterBody (DfBrHtmlParser *P)
{
  if (P->Pending->Tag == DFBR_HTML_TAG_HTML) {
    P->IM = DfBrHtmlImAfterAfterBody;
    return;
  }
  DfBrHtmlDiagHere (P, "end-tag-after-body");
  P->IM        = DfBrHtmlImInBody;
  P->Reprocess = DFBR_TRUE;
}

static void
Text_AfterBody (DfBrHtmlParser *P, const char *Data, usize Len)
{
  /* Per spec: whitespace -> process in InBody; non-ws -> parse error,
   * switch back to InBody and reprocess. Here we process as-is (which
   * inserts text into the body). */
  if (AllWhitespace (Data, Len)) {
    Text_InBody (P, Data, Len);
    return;
  }
  DfBrHtmlDiagHere (P, "text-after-body");
  P->IM = DfBrHtmlImInBody;
  Text_InBody (P, Data, Len);
}

/* ==================================================================
 *  Mode: AfterAfterBody
 * ==================================================================*/

static void
StartTag_AfterAfterBody (DfBrHtmlParser *P)
{
  if (P->Pending->Tag == DFBR_HTML_TAG_HTML) {
    StartTag_InBody (P);
    return;
  }
  DfBrHtmlDiagHere (P, "start-tag-after-after-body");
  P->IM        = DfBrHtmlImInBody;
  P->Reprocess = DFBR_TRUE;
}

static void
EndTag_AfterAfterBody (DfBrHtmlParser *P)
{
  DfBrHtmlDiagHere (P, "end-tag-after-after-body");
  P->IM        = DfBrHtmlImInBody;
  P->Reprocess = DFBR_TRUE;
}

static void
Text_AfterAfterBody (DfBrHtmlParser *P, const char *Data, usize Len)
{
  if (AllWhitespace (Data, Len)) {
    Text_InBody (P, Data, Len);
    return;
  }
  DfBrHtmlDiagHere (P, "text-after-after-body");
  P->IM = DfBrHtmlImInBody;
  Text_InBody (P, Data, Len);
}

/* ==================================================================
 *  Outer dispatchers + entry points
 * ==================================================================*/

static void
DispatchStartTag (DfBrHtmlParser *P)
{
  switch (P->IM) {
    case DfBrHtmlImInitial:          StartTag_Initial (P); return;
    case DfBrHtmlImBeforeHtml:       StartTag_BeforeHtml (P); return;
    case DfBrHtmlImBeforeHead:       StartTag_BeforeHead (P); return;
    case DfBrHtmlImInHead:           StartTag_InHead (P); return;
    case DfBrHtmlImInHeadNoscript:   /* TODO */ StartTag_InHead (P); return;
    case DfBrHtmlImAfterHead:        StartTag_AfterHead (P); return;
    case DfBrHtmlImInBody:           StartTag_InBody (P); return;
    case DfBrHtmlImText:             StartTag_Text (P); return;
    case DfBrHtmlImInTable:          StartTag_InTable (P); return;
    case DfBrHtmlImInTableText:      /* flushed lazily */ StartTag_InTable (P); return;
    case DfBrHtmlImInCaption:        StartTag_InCaption (P); return;
    case DfBrHtmlImInColumnGroup:    StartTag_InColumnGroup (P); return;
    case DfBrHtmlImInTableBody:      StartTag_InTableBody (P); return;
    case DfBrHtmlImInRow:            StartTag_InRow (P); return;
    case DfBrHtmlImInCell:           StartTag_InCell (P); return;
    case DfBrHtmlImInSelect:         /* TODO */ StartTag_InBody (P); return;
    case DfBrHtmlImInSelectInTable:  /* TODO */ StartTag_InBody (P); return;
    case DfBrHtmlImInTemplate:       /* TODO */ StartTag_InBody (P); return;
    case DfBrHtmlImAfterBody:        StartTag_AfterBody (P); return;
    case DfBrHtmlImInFrameset:       /* TODO */ StartTag_InBody (P); return;
    case DfBrHtmlImAfterFrameset:    /* TODO */ StartTag_InBody (P); return;
    case DfBrHtmlImAfterAfterBody:   StartTag_AfterAfterBody (P); return;
    case DfBrHtmlImAfterAfterFrameset: /* TODO */ StartTag_InBody (P); return;
  }
}

static void
DispatchEndTag (DfBrHtmlParser *P)
{
  switch (P->IM) {
    case DfBrHtmlImInitial:          EndTag_Initial (P); return;
    case DfBrHtmlImBeforeHtml:       EndTag_BeforeHtml (P); return;
    case DfBrHtmlImBeforeHead:       EndTag_BeforeHead (P); return;
    case DfBrHtmlImInHead:           EndTag_InHead (P); return;
    case DfBrHtmlImInHeadNoscript:   EndTag_InHead (P); return;
    case DfBrHtmlImAfterHead:        EndTag_AfterHead (P); return;
    case DfBrHtmlImInBody:           EndTag_InBody (P); return;
    case DfBrHtmlImText:             EndTag_Text (P); return;
    case DfBrHtmlImInTable:          EndTag_InTable (P); return;
    case DfBrHtmlImInTableText:      EndTag_InTable (P); return;
    case DfBrHtmlImInCaption:        EndTag_InCaption (P); return;
    case DfBrHtmlImInColumnGroup:    EndTag_InColumnGroup (P); return;
    case DfBrHtmlImInTableBody:      EndTag_InTableBody (P); return;
    case DfBrHtmlImInRow:            EndTag_InRow (P); return;
    case DfBrHtmlImInCell:           EndTag_InCell (P); return;
    case DfBrHtmlImInSelect:         EndTag_InBody (P); return;
    case DfBrHtmlImInSelectInTable:  EndTag_InBody (P); return;
    case DfBrHtmlImInTemplate:       EndTag_InBody (P); return;
    case DfBrHtmlImAfterBody:        EndTag_AfterBody (P); return;
    case DfBrHtmlImInFrameset:       EndTag_InBody (P); return;
    case DfBrHtmlImAfterFrameset:    EndTag_InBody (P); return;
    case DfBrHtmlImAfterAfterBody:   EndTag_AfterAfterBody (P); return;
    case DfBrHtmlImAfterAfterFrameset: EndTag_InBody (P); return;
  }
}

static void
DispatchText (DfBrHtmlParser *P, const char **Data, usize *Len)
{
  switch (P->IM) {
    case DfBrHtmlImInitial:          Text_Initial (P, Data, Len); return;
    case DfBrHtmlImBeforeHtml:       Text_BeforeHtml (P, Data, Len); return;
    case DfBrHtmlImBeforeHead:       Text_BeforeHead (P, Data, Len); return;
    case DfBrHtmlImInHead:           Text_InHead (P, Data, Len); return;
    case DfBrHtmlImInHeadNoscript:   Text_InHead (P, Data, Len); return;
    case DfBrHtmlImAfterHead:        Text_AfterHead (P, Data, Len); return;
    case DfBrHtmlImInBody:           Text_InBody (P, *Data, *Len); *Len = 0; return;
    case DfBrHtmlImText:             Text_Text (P, *Data, *Len); *Len = 0; return;
    case DfBrHtmlImInTable:          Text_InTable (P, *Data, *Len); *Len = 0; return;
    case DfBrHtmlImInTableText:      Text_InTable (P, *Data, *Len); *Len = 0; return;
    case DfBrHtmlImInCaption:        Text_InBody (P, *Data, *Len); *Len = 0; return;
    case DfBrHtmlImInColumnGroup:    {
      /* whitespace inserted into colgroup; non-ws ends colgroup */
      usize i = 0;
      while ((i < *Len) && IsWS ((*Data)[i])) i++;
      if (i > 0) { InsertCharacters (P, *Data, i, DFBR_FALSE); *Data += i; *Len -= i; }
      if (*Len == 0) { return; }
      if (TagEq (CurrentNode (P), DFBR_HTML_TAG_COLGROUP)) {
        StackPop (P);
        P->IM        = DfBrHtmlImInTable;
        P->Reprocess = DFBR_TRUE;
      } else {
        *Len = 0;
      }
      return;
    }
    case DfBrHtmlImInTableBody:      Text_InTable (P, *Data, *Len); *Len = 0; return;
    case DfBrHtmlImInRow:            Text_InTable (P, *Data, *Len); *Len = 0; return;
    case DfBrHtmlImInCell:           Text_InBody (P, *Data, *Len); *Len = 0; return;
    case DfBrHtmlImInSelect:         Text_InBody (P, *Data, *Len); *Len = 0; return;
    case DfBrHtmlImInSelectInTable:  Text_InBody (P, *Data, *Len); *Len = 0; return;
    case DfBrHtmlImInTemplate:       Text_InBody (P, *Data, *Len); *Len = 0; return;
    case DfBrHtmlImAfterBody:        Text_AfterBody (P, *Data, *Len); *Len = 0; return;
    case DfBrHtmlImInFrameset:       /* TODO */ *Len = 0; return;
    case DfBrHtmlImAfterFrameset:    /* TODO */ *Len = 0; return;
    case DfBrHtmlImAfterAfterBody:   Text_AfterAfterBody (P, *Data, *Len); *Len = 0; return;
    case DfBrHtmlImAfterAfterFrameset: /* TODO */ *Len = 0; return;
  }
}

void
DfBrHtmlTreeProcessStartTag (DfBrHtmlParser *P)
{
  do {
    P->Reprocess = DFBR_FALSE;
    DispatchStartTag (P);
  } while (P->Reprocess);
  P->Pending   = DFBR_NULL;
  P->SelfClose = DFBR_FALSE;
}

void
DfBrHtmlTreeProcessEndTag (DfBrHtmlParser *P)
{
  do {
    P->Reprocess = DFBR_FALSE;
    DispatchEndTag (P);
  } while (P->Reprocess);
  P->Pending   = DFBR_NULL;
  P->SelfClose = DFBR_FALSE;
}

void
DfBrHtmlTreeProcessText (DfBrHtmlParser *P, const char *Data, usize Len)
{
  if (Len == 0) {
    return;
  }
  do {
    P->Reprocess = DFBR_FALSE;
    DispatchText (P, &Data, &Len);
  } while (P->Reprocess && (Len > 0));
}

void
DfBrHtmlTreeProcessComment (DfBrHtmlParser *P, const char *Data, usize Len)
{
  if (!P->Doc->Options.KeepComments) {
    return;
  }
  switch (P->IM) {
    case DfBrHtmlImInitial:
    case DfBrHtmlImBeforeHtml:
      InsertComment (P, Data, Len, P->DocumentNode);
      return;
    case DfBrHtmlImAfterBody:
    case DfBrHtmlImAfterAfterBody:
    case DfBrHtmlImAfterFrameset:
    case DfBrHtmlImAfterAfterFrameset:
      if (P->HtmlElement != DFBR_NULL) {
        InsertComment (P, Data, Len, P->HtmlElement);
      } else {
        InsertComment (P, Data, Len, P->DocumentNode);
      }
      return;
    default:
      InsertComment (P, Data, Len, DFBR_NULL);
      return;
  }
}

void
DfBrHtmlTreeProcessDoctype (DfBrHtmlParser *P, const char *Data, usize Len)
{
  if (P->IM != DfBrHtmlImInitial) {
    DfBrHtmlDiagHere (P, "misplaced-doctype");
    return;
  }
  if (P->Doc->Options.KeepDoctype) {
    DfBrHtmlNode *D = DfBrHtmlNewDoctype (P->Doc, Data, Len);
    if (D != DFBR_NULL) {
      DfBrHtmlAppendChild (P->DocumentNode, D);
    }
  }
  P->IM = DfBrHtmlImBeforeHtml;
}

void
DfBrHtmlTreeProcessEof (DfBrHtmlParser *P)
{
  /* Close all still-open elements. Spec: a subset of tags are
   * expected to remain on the stack at EOF (html, body, head,
   * paragraphs, list items, table stuff, options) and do NOT count
   * as parse errors. Everything else gets a diagnostic so authors
   * can trace real unclosed tags. */
  while (P->StackTop >= 0) {
    DfBrHtmlNode *N = P->Stack[P->StackTop];
    switch (N->Tag) {
      case DFBR_HTML_TAG_HTML:
      case DFBR_HTML_TAG_HEAD:
      case DFBR_HTML_TAG_BODY:
      case DFBR_HTML_TAG_P:
      case DFBR_HTML_TAG_LI:
      case DFBR_HTML_TAG_DD:
      case DFBR_HTML_TAG_DT:
      case DFBR_HTML_TAG_OPTION:
      case DFBR_HTML_TAG_OPTGROUP:
      case DFBR_HTML_TAG_RB:
      case DFBR_HTML_TAG_RP:
      case DFBR_HTML_TAG_RT:
      case DFBR_HTML_TAG_RTC:
      case DFBR_HTML_TAG_TBODY:
      case DFBR_HTML_TAG_TD:
      case DFBR_HTML_TAG_TFOOT:
      case DFBR_HTML_TAG_TH:
      case DFBR_HTML_TAG_THEAD:
      case DFBR_HTML_TAG_TR:
        break;
      default:
        DfBrHtmlDiagAt (P, N->Line, N->Column, "unclosed-element-at-eof");
        break;
    }
    StackPop (P);
  }
}
