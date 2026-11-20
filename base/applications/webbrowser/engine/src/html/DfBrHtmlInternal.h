/** @file
  DfBrHtmlInternal.h - shared internals for the DfBrHtml library TUs.
  Not part of the public API.
**/

#ifndef DFBR_HTML_INTERNAL_H_
#define DFBR_HTML_INTERNAL_H_

#include "DfBrHtml.h"

/* ------------------------------------------------------------------
 *  Arena
 *
 *  Chunked bump allocator. Each chunk is a singly-linked list node
 *  with a flexible payload tail; new allocations bump Used until the
 *  current chunk is exhausted, then we malloc a new one. Freeing the
 *  arena walks the chunk list once.
 *
 *  Every node, attribute, string, and diagnostic in a DfBrHtmlDocument
 *  lives in the arena, so DfBrHtmlDocumentDestroy() is O(chunks).
 * -----------------------------------------------------------------*/

typedef struct DfBrHtmlArenaChunk {
  struct DfBrHtmlArenaChunk  *Next;
  usize                       Used;
  usize                       Cap;
  u8                          Data[1];    /* flexible tail */
} DfBrHtmlArenaChunk;

typedef struct {
  DfBrHtmlArenaChunk  *First;
  DfBrHtmlArenaChunk  *Current;
  usize                TotalBytes;
} DfBrHtmlArena;

void  *DfBrHtmlArenaAlloc       (DfBrHtmlArena *A, usize Size);
void  *DfBrHtmlArenaZero        (DfBrHtmlArena *A, usize Size);
char  *DfBrHtmlArenaStrDup      (DfBrHtmlArena *A, const char *S, usize Len);
char  *DfBrHtmlArenaStrDupLower (DfBrHtmlArena *A, const char *S, usize Len);
void   DfBrHtmlArenaDispose     (DfBrHtmlArena *A);

/* ------------------------------------------------------------------
 *  Diagnostics list
 * -----------------------------------------------------------------*/

typedef struct DfBrHtmlDiagList {
  DfBrHtmlDiagEntry  *Entries;
  usize               Count;
  usize               Cap;
} DfBrHtmlDiagList;

void DfBrHtmlDiagEmit (
  DfBrHtmlDiagList  *List,
  DfBrHtmlArena     *A,
  i32                Line,
  i32                Column,
  const char        *Message
  );

/* ------------------------------------------------------------------
 *  Document (internal layout)
 * -----------------------------------------------------------------*/

struct DfBrHtmlDocument {
  DfBrHtmlArena         Arena;
  DfBrHtmlNode         *Root;        /* DOCUMENT node */
  DfBrHtmlDiagList      Diags;
  char                 *Source;      /* arena copy */
  usize                 SourceLen;
  DfBrHtmlParseOptions  Options;

  /* Bumped by any public mutation call. Parsers leave it at zero so
   * a host that hasn't touched the tree since parse can skip its
   * "is relayout needed" check entirely. */
  u64                   Generation;

  /* Head of an intrusive list of DfBrCanvasBacking records - one per
   * <canvas> element the document has handed out pixels for.
   * Populated by DfBrCanvasEnsure; walked by DfBrCanvasDestroyAll
   * during document teardown so pixel arrays (malloc-backed, since
   * they can be reallocated) don't leak. Kept as void* here to keep
   * DfBrCanvas.h out of the parser's include graph. */
  void                 *CanvasListHead;
};

/* ------------------------------------------------------------------
 *  DOM construction helpers (used by the parser)
 * -----------------------------------------------------------------*/

DfBrHtmlNode *DfBrHtmlNewElement (
  DfBrHtmlDocument  *Doc,
  const char        *Name,
  usize              NameLen,
  DfBrHtmlTag        Tag
  );

DfBrHtmlNode *DfBrHtmlNewText    (DfBrHtmlDocument *Doc, const char *Text, usize TextLen);
DfBrHtmlNode *DfBrHtmlNewComment (DfBrHtmlDocument *Doc, const char *Text, usize TextLen);
DfBrHtmlNode *DfBrHtmlNewDoctype (DfBrHtmlDocument *Doc, const char *Text, usize TextLen);

DfBrHtmlAttr *DfBrHtmlAppendAttr (
  DfBrHtmlDocument  *Doc,
  DfBrHtmlNode      *Elem,
  const char        *Name,
  usize              NameLen,
  const char        *Value,       /* NULL = value-less attribute */
  usize              ValueLen
  );

void DfBrHtmlAppendChild  (DfBrHtmlNode *Parent, DfBrHtmlNode *Child);
void DfBrHtmlInsertBefore (DfBrHtmlNode *Parent, DfBrHtmlNode *New, DfBrHtmlNode *Ref);
void DfBrHtmlDetachChild  (DfBrHtmlNode *Child);

/* ------------------------------------------------------------------
 *  Entity decoding
 *
 *  Allocates out of Doc's arena. Handles `&name;` (small curated
 *  table of common named refs), `&#N;` decimal, and `&#xN;` hex.
 *  Unknown references are passed through literally - matching
 *  browser behaviour for e.g. `&unknown` in legacy markup.
 * -----------------------------------------------------------------*/

char *DfBrHtmlDecodeEntities (
  DfBrHtmlDocument  *Doc,
  const char        *Src,
  usize              SrcLen,
  usize             *OutLen      /* may be NULL */
  );

/* ==================================================================
 *
 *  Parser internals (shared between tokenizer TU and tree
 *  construction TU). The split is:
 *
 *    DfBrHtmlParse.c    - byte-level state machine (tokenizer).
 *                         Produces tokens by calling DfBrHtmlTreeProcess*.
 *    DfBrHtmlTree.c     - WHATWG insertion modes, open-elements stack,
 *                         scope checks, foster parenting, implicit
 *                         element insertion, reset-insertion-mode.
 *
 *  Both files #include this header, so both can reach into the full
 *  parser state directly. The alternative - an opaque API around the
 *  parser - adds noise without changing the single-TU-per-parse shape.
 *
 * ================================================================*/

#define DFBR_HTML_OPEN_STACK_LIMIT  256

/* ------------------------------------------------------------------
 *  Tokenizer state enum
 *
 *  Names follow WHATWG HTML tokenizer section headings so you can
 *  grep the spec directly into this file. Adding a new state is one
 *  `case` in the switch with no other edits.
 * -----------------------------------------------------------------*/

typedef enum {
  DfBrHtmlStateData,
  DfBrHtmlStateTagOpen,
  DfBrHtmlStateEndTagOpen,
  DfBrHtmlStateTagName,
  DfBrHtmlStateBeforeAttrName,
  DfBrHtmlStateAttrName,
  DfBrHtmlStateAfterAttrName,
  DfBrHtmlStateBeforeAttrValue,
  DfBrHtmlStateAttrValueDQ,
  DfBrHtmlStateAttrValueSQ,
  DfBrHtmlStateAttrValueUQ,
  DfBrHtmlStateAfterAttrValueQuoted,
  DfBrHtmlStateSelfClosingStartTag,
  DfBrHtmlStateMarkupDeclOpen,
  DfBrHtmlStateComment,
  DfBrHtmlStateCommentEndDash,
  DfBrHtmlStateCommentEnd,
  DfBrHtmlStateDoctype,
  DfBrHtmlStateBogusComment,
  DfBrHtmlStateRawText,
  DfBrHtmlStateRawTextLessThan,
  DfBrHtmlStateRawTextEndTagOpen,
  DfBrHtmlStateRawTextEndTagName,
  DfBrHtmlStateRcData,
  DfBrHtmlStateRcDataLessThan,
  DfBrHtmlStateRcDataEndTagOpen,
  DfBrHtmlStateRcDataEndTagName,
  DfBrHtmlStatePlainText
} DfBrHtmlTokState;

/* ------------------------------------------------------------------
 *  Insertion modes (WHATWG tree construction).
 *
 *  The tree constructor switches between these as elements open and
 *  close; each mode has its own per-token behaviour. A handful -
 *  marked with TODO comments in the tree TU - are present so
 *  ResetInsertionMode has somewhere to resolve to but fall back to
 *  IMInBody for now.
 * -----------------------------------------------------------------*/

typedef enum {
  DfBrHtmlImInitial,
  DfBrHtmlImBeforeHtml,
  DfBrHtmlImBeforeHead,
  DfBrHtmlImInHead,
  DfBrHtmlImInHeadNoscript,
  DfBrHtmlImAfterHead,
  DfBrHtmlImInBody,
  DfBrHtmlImText,
  DfBrHtmlImInTable,
  DfBrHtmlImInTableText,
  DfBrHtmlImInCaption,
  DfBrHtmlImInColumnGroup,
  DfBrHtmlImInTableBody,
  DfBrHtmlImInRow,
  DfBrHtmlImInCell,
  DfBrHtmlImInSelect,
  DfBrHtmlImInSelectInTable,
  DfBrHtmlImInTemplate,
  DfBrHtmlImAfterBody,
  DfBrHtmlImInFrameset,
  DfBrHtmlImAfterFrameset,
  DfBrHtmlImAfterAfterBody,
  DfBrHtmlImAfterAfterFrameset
} DfBrHtmlIM;

/* ------------------------------------------------------------------
 *  Full parser state (shared across tokenizer + tree constructor TUs)
 * -----------------------------------------------------------------*/

typedef struct DfBrHtmlParser {
  DfBrHtmlDocument  *Doc;

  /* Byte-level input cursor. Line/Column are 1-based. */
  const char        *Src;
  usize              Len;
  usize              Pos;
  i32                Line;
  i32                Column;

  /* --- Tokenizer state --------------------------------------------*/

  DfBrHtmlTokState   TokState;

  /* Start tag being built. Attributes attach onto Pending from the
   * moment we know the tag name; we never emit an element with an
   * orphan attribute list. */
  DfBrHtmlNode      *Pending;
  b32                PendingIsEnd;
  b32                SelfClose;
  i32                PendingLine;
  i32                PendingColumn;

  /* Pending attribute - committed onto Pending->FirstAttr when we
   * know we've seen the end of the value. */
  const char        *AttrName;
  usize              AttrNameLen;
  const char        *AttrValueStart;
  usize              AttrValueLen;
  b32                AttrHasValue;

  /* Accumulating text in Data / RcData / RawText states. Stored as a
   * contiguous source range where possible; when the run becomes
   * non-contiguous (e.g. tag interrupts) we flush. */
  const char        *TextStart;
  usize              TextLen;

  /* End-tag matching for RAWTEXT / RCDATA. `EndTagMatch` is the name
   * of the currently-open rawtext/rcdata element we're looking for.
   * `EndTagCandStart` marks where the potential `</name>` begins in
   * source so we can rewind and treat it as literal text if it
   * doesn't pan out. */
  const char        *EndTagMatch;
  usize              EndTagMatchLen;
  const char        *EndTagCandStart;
  usize              EndTagCandLen;

  /* --- Tree construction state ------------------------------------*/

  DfBrHtmlIM         IM;
  DfBrHtmlIM         OriginalIM;   /* saved when switching to IMText */

  DfBrHtmlNode      *Stack[DFBR_HTML_OPEN_STACK_LIMIT];
  i32                StackTop;     /* -1 when empty */

  /* Quick references the tree constructor needs constantly. */
  DfBrHtmlNode      *DocumentNode; /* == Doc->Root, cached for speed */
  DfBrHtmlNode      *HtmlElement;
  DfBrHtmlNode      *HeadElement;
  DfBrHtmlNode      *BodyElement;
  DfBrHtmlNode      *FormElement;

  /* The "frameset-ok" flag. Set true until a non-whitespace character
   * or a "disallowing" start tag is seen in body; when false a later
   * <frameset> becomes a parse error. */
  b32                FramesetOk;

  /* Reprocess signal. Set by a per-mode handler when the current
   * token should be re-dispatched in the (presumably updated) mode. */
  b32                Reprocess;
} DfBrHtmlParser;

/* ------------------------------------------------------------------
 *  Tree construction entry points (called from the tokenizer TU)
 * -----------------------------------------------------------------*/

/** Initialise the tree-construction fields of P. Call once before
    feeding tokens. Does not touch the tokenizer subset of the parser. **/
void DfBrHtmlTreeInit (DfBrHtmlParser *P);

/** A start tag has been fully tokenized; P->Pending carries name,
    attrs, self-closing flag. On return, P->Pending is consumed (NULL). **/
void DfBrHtmlTreeProcessStartTag (DfBrHtmlParser *P);

/** Likewise for end tags. **/
void DfBrHtmlTreeProcessEndTag (DfBrHtmlParser *P);

/** A contiguous run of character data. `Data` is UTF-8, decoded.
    The tree constructor decides where (or whether) to insert it -
    Initial/BeforeHtml etc. discard leading whitespace, InTable
    foster-parents non-whitespace, etc. **/
void DfBrHtmlTreeProcessText (DfBrHtmlParser *P, const char *Data, usize Len);

/** A comment token. **/
void DfBrHtmlTreeProcessComment (DfBrHtmlParser *P, const char *Data, usize Len);

/** A DOCTYPE token. `Data` is the raw payload after DOCTYPE/whitespace. **/
void DfBrHtmlTreeProcessDoctype (DfBrHtmlParser *P, const char *Data, usize Len);

/** Tokenizer has hit EOF. Walks the stack, emits diagnostics for any
    unclosed elements. **/
void DfBrHtmlTreeProcessEof (DfBrHtmlParser *P);

/* ------------------------------------------------------------------
 *  Shared helpers used by both TUs
 * -----------------------------------------------------------------*/

void DfBrHtmlDiagAt   (DfBrHtmlParser *P, i32 Line, i32 Column, const char *Msg);
void DfBrHtmlDiagHere (DfBrHtmlParser *P, const char *Msg);

/* ------------------------------------------------------------------
 *  Parser entry point (implemented in DfBrHtmlParse.c)
 * -----------------------------------------------------------------*/

void DfBrHtmlRunParser (DfBrHtmlDocument *Doc);

#endif /* DFBR_HTML_INTERNAL_H_ */
