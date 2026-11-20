/** @file
  DfBrHtmlParse.c - byte-level tokenizer for DfBrHtml.

  Design
  ======
  Single-function switch-state machine keyed on DfBrHtmlTokState.
  State names follow the WHATWG HTML tokenizer spec section headings
  so you can grep straight from the standard. Adding a new state is
  additive: one `case` in the switch, nothing else to touch.

  The tokenizer runs over `Doc->Source` byte-by-byte, building up:

    * contiguous character runs ("Text"), flushed on state changes
      via DfBrHtmlTreeProcessText,
    * pending start/end tags (Parser::Pending) with attributes
      attached incrementally, emitted via
      DfBrHtmlTreeProcessStartTag / DfBrHtmlTreeProcessEndTag,
    * comments and DOCTYPEs, emitted via
      DfBrHtmlTreeProcessComment / DfBrHtmlTreeProcessDoctype,
    * EOF -> DfBrHtmlTreeProcessEof.

  All tree-shape decisions (insertion modes, scope checks, foster
  parenting, implicit element insertion) live in DfBrHtmlTree.c.
  That file owns the open-elements stack and the current insertion
  mode; this file only owns the lexical state.

  Entity decoding
  ===============
  `&name;` (curated common-name table), `&#N;` decimal, `&#xN;` hex,
  plus tolerance for un-terminated / unknown references (passed
  through literally, browser-style). Decoded strings are allocated
  in the document arena.

  Deliberate v2 gaps
  ==================
    * Full 80+-state tokenizer. v2 has ~28 of them. Missing: the
      script-data escape ladder, CDATA sections, PLAINTEXT, a
      per-field DOCTYPE parser, and the "named character reference"
      state (we fold ref decoding into FlushText instead).
    * NULL-byte handling (spec says emit U+FFFD in many states).
**/

#include "DfBrHtmlInternal.h"

#include <stdlib.h>
#include <string.h>

/* ==================================================================
 *  Entity decoding
 * ==================================================================*/

typedef struct {
  const char  *Name;
  const char  *Replacement;   /* UTF-8 bytes */
} NamedEntity;

/* Small curated table. Covers the ~50 most common named references
 * that appear in real HTML in the wild. Full WHATWG entity list
 * (2125 names) is a future expansion; slotting it in only means
 * replacing this table + its lookup. */
static const NamedEntity mEntities[] = {
  { "amp",    "&" },
  { "lt",     "<" },
  { "gt",     ">" },
  { "quot",   "\"" },
  { "apos",   "'" },
  { "nbsp",   "\xC2\xA0" },
  { "copy",   "\xC2\xA9" },
  { "reg",    "\xC2\xAE" },
  { "trade",  "\xE2\x84\xA2" },
  { "hellip", "\xE2\x80\xA6" },
  { "mdash",  "\xE2\x80\x94" },
  { "ndash",  "\xE2\x80\x93" },
  { "lsquo",  "\xE2\x80\x98" },
  { "rsquo",  "\xE2\x80\x99" },
  { "ldquo",  "\xE2\x80\x9C" },
  { "rdquo",  "\xE2\x80\x9D" },
  { "laquo",  "\xC2\xAB" },
  { "raquo",  "\xC2\xBB" },
  { "bull",   "\xE2\x80\xA2" },
  { "middot", "\xC2\xB7" },
  { "deg",    "\xC2\xB0" },
  { "times",  "\xC3\x97" },
  { "divide", "\xC3\xB7" },
  { "plusmn", "\xC2\xB1" },
  { "micro",  "\xC2\xB5" },
  { "para",   "\xC2\xB6" },
  { "sect",   "\xC2\xA7" },
  { "euro",   "\xE2\x82\xAC" },
  { "pound",  "\xC2\xA3" },
  { "yen",    "\xC2\xA5" },
  { "cent",   "\xC2\xA2" },
  { "iexcl",  "\xC2\xA1" },
  { "iquest", "\xC2\xBF" },
  { "Auml",   "\xC3\x84" }, { "auml",   "\xC3\xA4" },
  { "Ouml",   "\xC3\x96" }, { "ouml",   "\xC3\xB6" },
  { "Uuml",   "\xC3\x9C" }, { "uuml",   "\xC3\xBC" },
  { "szlig",  "\xC3\x9F" },
  { "Aacute", "\xC3\x81" }, { "aacute", "\xC3\xA1" },
  { "Eacute", "\xC3\x89" }, { "eacute", "\xC3\xA9" },
  { "Iacute", "\xC3\x8D" }, { "iacute", "\xC3\xAD" },
  { "Oacute", "\xC3\x93" }, { "oacute", "\xC3\xB3" },
  { "Uacute", "\xC3\x9A" }, { "uacute", "\xC3\xBA" },
  { "Agrave", "\xC3\x80" }, { "agrave", "\xC3\xA0" },
  { "Egrave", "\xC3\x88" }, { "egrave", "\xC3\xA8" },
  { "Igrave", "\xC3\x8C" }, { "igrave", "\xC3\xAC" },
  { "Ograve", "\xC3\x92" }, { "ograve", "\xC3\xB2" },
  { "Ugrave", "\xC3\x99" }, { "ugrave", "\xC3\xB9" },
  { "Ntilde", "\xC3\x91" }, { "ntilde", "\xC3\xB1" },
  { "Ccedil", "\xC3\x87" }, { "ccedil", "\xC3\xA7" },
};

/* UTF-8 encode a single codepoint. Writes up to 4 bytes; returns the
 * byte count written. For invalid codepoints emits U+FFFD. */
static usize
Utf8Encode (
  u32   Cp,
  char  Dst[4]
  )
{
  if (Cp <= 0x7F) {
    Dst[0] = (char)Cp;
    return 1;
  }
  if (Cp <= 0x7FF) {
    Dst[0] = (char)(0xC0 | (Cp >> 6));
    Dst[1] = (char)(0x80 | (Cp & 0x3F));
    return 2;
  }
  if ((Cp >= 0xD800) && (Cp <= 0xDFFF)) {
    /* surrogate - invalid in UTF-8. Browsers substitute U+FFFD. */
    Cp = 0xFFFD;
  }
  if (Cp <= 0xFFFF) {
    Dst[0] = (char)(0xE0 | (Cp >> 12));
    Dst[1] = (char)(0x80 | ((Cp >> 6) & 0x3F));
    Dst[2] = (char)(0x80 | (Cp & 0x3F));
    return 3;
  }
  if (Cp <= 0x10FFFF) {
    Dst[0] = (char)(0xF0 | (Cp >> 18));
    Dst[1] = (char)(0x80 | ((Cp >> 12) & 0x3F));
    Dst[2] = (char)(0x80 | ((Cp >> 6) & 0x3F));
    Dst[3] = (char)(0x80 | (Cp & 0x3F));
    return 4;
  }
  /* out of Unicode range */
  Dst[0] = (char)0xEF; Dst[1] = (char)0xBF; Dst[2] = (char)0xBD;
  return 3;
}

static const NamedEntity *
LookupEntity (
  const char  *Name,
  usize        NameLen
  )
{
  usize  i;

  for (i = 0; i < DFBR_ARRAY_LEN (mEntities); i++) {
    if ((strlen (mEntities[i].Name) == NameLen) &&
        (memcmp (mEntities[i].Name, Name, NameLen) == 0)) {
      return &mEntities[i];
    }
  }
  return DFBR_NULL;
}

char *
DfBrHtmlDecodeEntities (
  DfBrHtmlDocument  *Doc,
  const char        *Src,
  usize              SrcLen,
  usize             *OutLen
  )
{
  char               *Out;
  usize               i;
  usize               o;
  usize               Start;
  u32                 Cp;
  char                Buf[4];
  usize               N;
  const NamedEntity  *Ent;

  /* Output is at most the size of the input: named refs like &hellip;
   * are 8 source bytes -> 3 UTF-8 bytes, and numeric refs shrink too.
   * Worst case is `&amp;` (5 bytes -> 1 byte). */
  Out = (char *)DfBrHtmlArenaAlloc (&Doc->Arena, SrcLen + 1);
  if (Out == DFBR_NULL) {
    if (OutLen != DFBR_NULL) {
      *OutLen = 0;
    }
    return DFBR_NULL;
  }

  i = 0;
  o = 0;
  while (i < SrcLen) {
    if (Src[i] != '&') {
      Out[o++] = Src[i++];
      continue;
    }

    /* `&` seen. Try to parse a reference. */
    Start = i;
    i++;
    if (i >= SrcLen) {
      Out[o++] = '&';
      break;
    }

    if (Src[i] == '#') {
      /* numeric reference */
      i++;
      Cp = 0;
      if ((i < SrcLen) && ((Src[i] == 'x') || (Src[i] == 'X'))) {
        i++;
        while (i < SrcLen) {
          char c = Src[i];
          if ((c >= '0') && (c <= '9')) {
            Cp = (Cp * 16U) + (u32)(c - '0');
          } else if ((c >= 'a') && (c <= 'f')) {
            Cp = (Cp * 16U) + (u32)(c - 'a' + 10);
          } else if ((c >= 'A') && (c <= 'F')) {
            Cp = (Cp * 16U) + (u32)(c - 'A' + 10);
          } else {
            break;
          }
          i++;
        }
      } else {
        while ((i < SrcLen) && (Src[i] >= '0') && (Src[i] <= '9')) {
          Cp = (Cp * 10U) + (u32)(Src[i] - '0');
          i++;
        }
      }

      if ((i > Start + 2) && (i < SrcLen) && (Src[i] == ';')) {
        i++;                      /* consume the trailing ';' */
        N = Utf8Encode (Cp, Buf);
        memcpy (&Out[o], Buf, N);
        o += N;
      } else {
        /* malformed: emit verbatim so authors can see what broke. */
        memcpy (&Out[o], &Src[Start], i - Start);
        o += (i - Start);
      }
      continue;
    }

    /* named reference */
    {
      usize  NameStart = i;
      while ((i < SrcLen) &&
             (((Src[i] >= 'a') && (Src[i] <= 'z')) ||
              ((Src[i] >= 'A') && (Src[i] <= 'Z')) ||
              ((Src[i] >= '0') && (Src[i] <= '9')))) {
        i++;
      }
      if ((i > NameStart) && (i < SrcLen) && (Src[i] == ';')) {
        Ent = LookupEntity (&Src[NameStart], i - NameStart);
        if (Ent != DFBR_NULL) {
          N = strlen (Ent->Replacement);
          memcpy (&Out[o], Ent->Replacement, N);
          o += N;
          i++;                    /* consume the trailing ';' */
          continue;
        }
      }
      /* not a recognised named reference - pass through verbatim. */
      memcpy (&Out[o], &Src[Start], i - Start);
      o += (i - Start);
    }
  }

  Out[o] = '\0';
  if (OutLen != DFBR_NULL) {
    *OutLen = o;
  }
  return Out;
}

/* ==================================================================
 *  Small cursor / classification helpers
 * ==================================================================*/

static void
Advance (DfBrHtmlParser *P)
{
  if (P->Pos < P->Len) {
    if (P->Src[P->Pos] == '\n') {
      P->Line++;
      P->Column = 1;
    } else {
      P->Column++;
    }
    P->Pos++;
  }
}

static int
IsAlpha (char c)
{
  return (((c >= 'a') && (c <= 'z')) ||
          ((c >= 'A') && (c <= 'Z')));
}

static int
IsWhitespace (char c)
{
  return (c == ' ') || (c == '\t') || (c == '\n') ||
         (c == '\r') || (c == '\f');
}

static int
AsciiLowerEq (
  const char  *A,
  usize        ALen,
  const char  *B,
  usize        BLen
  )
{
  usize  i;
  char   c;

  if (ALen != BLen) {
    return 0;
  }
  for (i = 0; i < ALen; i++) {
    c = A[i];
    if ((c >= 'A') && (c <= 'Z')) {
      c = (char)(c - 'A' + 'a');
    }
    if (c != B[i]) {
      return 0;
    }
  }
  return 1;
}

/* ==================================================================
 *  Text buffering
 * ==================================================================*/

static void
FlushText (DfBrHtmlParser *P, b32 DecodeEntities)
{
  const char  *Data;
  usize        DataLen;
  char        *Decoded;
  usize        DecodedLen;

  if (P->TextLen == 0) {
    return;
  }

  Data    = P->TextStart;
  DataLen = P->TextLen;

  if (DecodeEntities) {
    Decoded = DfBrHtmlDecodeEntities (P->Doc, Data, DataLen, &DecodedLen);
    if (Decoded != DFBR_NULL) {
      Data    = Decoded;
      DataLen = DecodedLen;
    }
  }

  P->TextStart = DFBR_NULL;
  P->TextLen   = 0;

  DfBrHtmlTreeProcessText (P, Data, DataLen);
}

static void
ExtendText (DfBrHtmlParser *P)
{
  /* Called when the current source byte should be part of accumulated
   * text. Assumes text runs are always contiguous in source, which is
   * true for every data-family state - we flush before switching. */
  if (P->TextLen == 0) {
    P->TextStart = &P->Src[P->Pos];
  }
  P->TextLen++;
}

/* ==================================================================
 *  Tag construction / emission
 * ==================================================================*/

static void
AttrReset (DfBrHtmlParser *P)
{
  P->AttrName       = DFBR_NULL;
  P->AttrNameLen    = 0;
  P->AttrValueStart = DFBR_NULL;
  P->AttrValueLen   = 0;
  P->AttrHasValue   = DFBR_FALSE;
}

static void
AttrCommit (DfBrHtmlParser *P, b32 DecodeEntities)
{
  const char  *Value    = DFBR_NULL;
  usize        ValueLen = 0;

  if ((P->AttrName == DFBR_NULL) || (P->AttrNameLen == 0)) {
    AttrReset (P);
    return;
  }
  if (P->Pending == DFBR_NULL) {
    AttrReset (P);
    return;
  }

  if (P->AttrHasValue) {
    if (DecodeEntities && (P->AttrValueLen > 0)) {
      Value = DfBrHtmlDecodeEntities (
                P->Doc, P->AttrValueStart, P->AttrValueLen, &ValueLen);
    } else {
      Value    = P->AttrValueStart;
      ValueLen = P->AttrValueLen;
    }
  }

  DfBrHtmlAppendAttr (
    P->Doc, P->Pending, P->AttrName, P->AttrNameLen,
    P->AttrHasValue ? Value    : DFBR_NULL,
    P->AttrHasValue ? ValueLen : 0);

  AttrReset (P);
}

static void
StartTagBegin (DfBrHtmlParser *P, b32 IsEnd, i32 Line, i32 Column)
{
  P->Pending       = DFBR_NULL;
  P->PendingIsEnd  = IsEnd;
  P->SelfClose     = DFBR_FALSE;
  P->PendingLine   = Line;
  P->PendingColumn = Column;
  AttrReset (P);

  /* Placeholder element so attributes can attach before we know the
   * tag name. PendingSetName finalises name / tag enum on completion
   * of the TagName state. */
  P->Pending = DfBrHtmlNewElement (P->Doc, "", 0, DFBR_HTML_TAG_UNKNOWN);
  if (P->Pending != DFBR_NULL) {
    P->Pending->Line   = Line;
    P->Pending->Column = Column;
  }
}

static void
PendingSetName (DfBrHtmlParser *P, const char *Name, usize NameLen)
{
  if (P->Pending == DFBR_NULL) {
    return;
  }
  P->Pending->Name    = DfBrHtmlArenaStrDupLower (&P->Doc->Arena, Name, NameLen);
  P->Pending->NameLen = NameLen;
  P->Pending->Tag     = DfBrHtmlTagFromName (Name, NameLen);
}

/* Emit the current tag into the tree constructor. After this call
 * P->Pending is NULL; the tree constructor may have pushed it, moved
 * it, merged attrs with an existing element, or dropped it. */
static void
EmitTag (DfBrHtmlParser *P, b32 AttrsDecodeEntities)
{
  AttrCommit (P, AttrsDecodeEntities);

  /* Default tokenizer state after a tag is emitted is DATA. The tree
   * constructor may override via SwitchToText for rawtext/rcdata
   * elements; that call updates TokState directly. */
  P->TokState = DfBrHtmlStateData;

  if (P->PendingIsEnd) {
    DfBrHtmlTreeProcessEndTag (P);
  } else {
    DfBrHtmlTreeProcessStartTag (P);
  }
}

/* ==================================================================
 *  Raw/rcdata end-tag matcher
 *
 *  Called when we're in Raw*EndTagName. Checks whether the sequence
 *  of name chars we've been accumulating matches EndTagMatch.
 * ==================================================================*/

static int
EndTagNameMatches (DfBrHtmlParser *P)
{
  return AsciiLowerEq (P->EndTagCandStart, P->EndTagCandLen,
                       P->EndTagMatch,     P->EndTagMatchLen);
}

/* ==================================================================
 *  Markup declaration open - detect <!--, <!DOCTYPE
 * ==================================================================*/

static int
MatchLiteralCaseI (DfBrHtmlParser *P, const char *Lit, usize LitLen)
{
  usize  i;
  char   a;
  char   b;

  if (P->Pos + LitLen > P->Len) {
    return 0;
  }
  for (i = 0; i < LitLen; i++) {
    a = P->Src[P->Pos + i];
    b = Lit[i];
    if ((a >= 'A') && (a <= 'Z')) {
      a = (char)(a - 'A' + 'a');
    }
    if ((b >= 'A') && (b <= 'Z')) {
      b = (char)(b - 'A' + 'a');
    }
    if (a != b) {
      return 0;
    }
  }
  return 1;
}

/* ==================================================================
 *  The tokenizer state machine
 * ==================================================================*/

void
DfBrHtmlRunParser (
  DfBrHtmlDocument  *Doc
  )
{
  DfBrHtmlParser  P;
  int             C;
  int             Eof;

  memset (&P, 0, sizeof (P));
  P.Doc      = Doc;
  P.Src      = Doc->Source;
  P.Len      = Doc->SourceLen;
  P.Line     = 1;
  P.Column   = 1;
  P.TokState = DfBrHtmlStateData;
  DfBrHtmlTreeInit (&P);

  while (P.Pos <= P.Len) {
    Eof = (P.Pos >= P.Len);
    C   = Eof ? -1 : (unsigned char)P.Src[P.Pos];

    switch (P.TokState) {

    /* ------------------------------------------------------------ */
    case DfBrHtmlStateData:
      if (Eof) {
        FlushText (&P, DFBR_TRUE);
        goto done;
      }
      if (C == '<') {
        FlushText (&P, DFBR_TRUE);
        Advance (&P);
        P.TokState = DfBrHtmlStateTagOpen;
        continue;
      }
      ExtendText (&P);
      Advance (&P);
      continue;

    /* ------------------------------------------------------------ */
    case DfBrHtmlStateTagOpen:
      if (Eof) {
        DfBrHtmlDiagHere (&P, "eof-before-tag-name");
        goto done;
      }
      if (C == '!') {
        Advance (&P);
        P.TokState = DfBrHtmlStateMarkupDeclOpen;
        continue;
      }
      if (C == '/') {
        Advance (&P);
        P.TokState = DfBrHtmlStateEndTagOpen;
        continue;
      }
      if (IsAlpha ((char)C)) {
        StartTagBegin (&P, DFBR_FALSE, P.Line, P.Column - 1);
        P.TokState = DfBrHtmlStateTagName;
        continue;
      }
      if (C == '?') {
        DfBrHtmlDiagHere (&P, "unexpected-question-mark-instead-of-tag-name");
        P.TokState = DfBrHtmlStateBogusComment;
        continue;
      }
      DfBrHtmlDiagHere (&P, "invalid-first-character-of-tag-name");
      P.TokState = DfBrHtmlStateData;
      continue;

    /* ------------------------------------------------------------ */
    case DfBrHtmlStateEndTagOpen:
      if (Eof) {
        DfBrHtmlDiagHere (&P, "eof-before-tag-name");
        goto done;
      }
      if (IsAlpha ((char)C)) {
        StartTagBegin (&P, DFBR_TRUE, P.Line, P.Column - 2);
        P.TokState = DfBrHtmlStateTagName;
        continue;
      }
      if (C == '>') {
        DfBrHtmlDiagHere (&P, "missing-end-tag-name");
        Advance (&P);
        P.TokState = DfBrHtmlStateData;
        continue;
      }
      DfBrHtmlDiagHere (&P, "invalid-first-character-of-tag-name");
      P.TokState = DfBrHtmlStateBogusComment;
      continue;

    /* ------------------------------------------------------------ */
    case DfBrHtmlStateTagName: {
      const char  *NameStart = &P.Src[P.Pos];
      usize        NameLen   = 0;

      while (P.Pos < P.Len) {
        char c = P.Src[P.Pos];
        if (IsWhitespace (c) || (c == '/') || (c == '>')) {
          break;
        }
        NameLen++;
        Advance (&P);
      }
      PendingSetName (&P, NameStart, NameLen);

      if (P.Pos >= P.Len) {
        DfBrHtmlDiagHere (&P, "eof-in-tag");
        goto done;
      }
      if (P.Src[P.Pos] == '>') {
        Advance (&P);
        EmitTag (&P, DFBR_TRUE);
        continue;
      }
      if (P.Src[P.Pos] == '/') {
        Advance (&P);
        P.TokState = DfBrHtmlStateSelfClosingStartTag;
        continue;
      }
      /* whitespace */
      Advance (&P);
      P.TokState = DfBrHtmlStateBeforeAttrName;
      continue;
    }

    /* ------------------------------------------------------------ */
    case DfBrHtmlStateBeforeAttrName:
      if (Eof) {
        DfBrHtmlDiagHere (&P, "eof-in-tag");
        goto done;
      }
      if (IsWhitespace ((char)C)) {
        Advance (&P);
        continue;
      }
      if ((C == '/') || (C == '>')) {
        P.TokState = DfBrHtmlStateAfterAttrName;
        continue;
      }
      if (C == '=') {
        DfBrHtmlDiagHere (&P, "unexpected-equals-sign-before-attribute-name");
        P.AttrName    = &P.Src[P.Pos];
        P.AttrNameLen = 1;
        Advance (&P);
        P.TokState = DfBrHtmlStateAttrName;
        continue;
      }
      P.AttrName    = &P.Src[P.Pos];
      P.AttrNameLen = 0;
      P.TokState    = DfBrHtmlStateAttrName;
      continue;

    /* ------------------------------------------------------------ */
    case DfBrHtmlStateAttrName:
      if (Eof) {
        DfBrHtmlDiagHere (&P, "eof-in-tag");
        goto done;
      }
      if (IsWhitespace ((char)C) || (C == '/') || (C == '>')) {
        P.TokState = DfBrHtmlStateAfterAttrName;
        continue;
      }
      if (C == '=') {
        Advance (&P);
        P.TokState = DfBrHtmlStateBeforeAttrValue;
        continue;
      }
      P.AttrNameLen++;
      Advance (&P);
      continue;

    /* ------------------------------------------------------------ */
    case DfBrHtmlStateAfterAttrName:
      if (Eof) {
        DfBrHtmlDiagHere (&P, "eof-in-tag");
        goto done;
      }
      if (IsWhitespace ((char)C)) {
        Advance (&P);
        continue;
      }
      if (C == '/') {
        AttrCommit (&P, DFBR_TRUE);
        Advance (&P);
        P.TokState = DfBrHtmlStateSelfClosingStartTag;
        continue;
      }
      if (C == '=') {
        Advance (&P);
        P.TokState = DfBrHtmlStateBeforeAttrValue;
        continue;
      }
      if (C == '>') {
        AttrCommit (&P, DFBR_TRUE);
        Advance (&P);
        EmitTag (&P, DFBR_TRUE);
        continue;
      }
      AttrCommit (&P, DFBR_TRUE);
      P.AttrName    = &P.Src[P.Pos];
      P.AttrNameLen = 0;
      P.TokState    = DfBrHtmlStateAttrName;
      continue;

    /* ------------------------------------------------------------ */
    case DfBrHtmlStateBeforeAttrValue:
      if (Eof) {
        DfBrHtmlDiagHere (&P, "eof-in-tag");
        goto done;
      }
      if (IsWhitespace ((char)C)) {
        Advance (&P);
        continue;
      }
      if (C == '"') {
        Advance (&P);
        P.AttrValueStart = &P.Src[P.Pos];
        P.AttrValueLen   = 0;
        P.AttrHasValue   = DFBR_TRUE;
        P.TokState       = DfBrHtmlStateAttrValueDQ;
        continue;
      }
      if (C == '\'') {
        Advance (&P);
        P.AttrValueStart = &P.Src[P.Pos];
        P.AttrValueLen   = 0;
        P.AttrHasValue   = DFBR_TRUE;
        P.TokState       = DfBrHtmlStateAttrValueSQ;
        continue;
      }
      if (C == '>') {
        DfBrHtmlDiagHere (&P, "missing-attribute-value");
        P.AttrHasValue   = DFBR_TRUE;
        P.AttrValueStart = &P.Src[P.Pos];
        P.AttrValueLen   = 0;
        AttrCommit (&P, DFBR_TRUE);
        Advance (&P);
        EmitTag (&P, DFBR_TRUE);
        continue;
      }
      P.AttrValueStart = &P.Src[P.Pos];
      P.AttrValueLen   = 0;
      P.AttrHasValue   = DFBR_TRUE;
      P.TokState       = DfBrHtmlStateAttrValueUQ;
      continue;

    /* ------------------------------------------------------------ */
    case DfBrHtmlStateAttrValueDQ:
      if (Eof) {
        DfBrHtmlDiagHere (&P, "eof-in-tag");
        goto done;
      }
      if (C == '"') {
        AttrCommit (&P, DFBR_TRUE);
        Advance (&P);
        P.TokState = DfBrHtmlStateAfterAttrValueQuoted;
        continue;
      }
      P.AttrValueLen++;
      Advance (&P);
      continue;

    /* ------------------------------------------------------------ */
    case DfBrHtmlStateAttrValueSQ:
      if (Eof) {
        DfBrHtmlDiagHere (&P, "eof-in-tag");
        goto done;
      }
      if (C == '\'') {
        AttrCommit (&P, DFBR_TRUE);
        Advance (&P);
        P.TokState = DfBrHtmlStateAfterAttrValueQuoted;
        continue;
      }
      P.AttrValueLen++;
      Advance (&P);
      continue;

    /* ------------------------------------------------------------ */
    case DfBrHtmlStateAttrValueUQ:
      if (Eof) {
        DfBrHtmlDiagHere (&P, "eof-in-tag");
        goto done;
      }
      if (IsWhitespace ((char)C)) {
        AttrCommit (&P, DFBR_TRUE);
        Advance (&P);
        P.TokState = DfBrHtmlStateBeforeAttrName;
        continue;
      }
      if (C == '>') {
        AttrCommit (&P, DFBR_TRUE);
        Advance (&P);
        EmitTag (&P, DFBR_TRUE);
        continue;
      }
      P.AttrValueLen++;
      Advance (&P);
      continue;

    /* ------------------------------------------------------------ */
    case DfBrHtmlStateAfterAttrValueQuoted:
      if (Eof) {
        DfBrHtmlDiagHere (&P, "eof-in-tag");
        goto done;
      }
      if (IsWhitespace ((char)C)) {
        Advance (&P);
        P.TokState = DfBrHtmlStateBeforeAttrName;
        continue;
      }
      if (C == '/') {
        Advance (&P);
        P.TokState = DfBrHtmlStateSelfClosingStartTag;
        continue;
      }
      if (C == '>') {
        Advance (&P);
        EmitTag (&P, DFBR_TRUE);
        continue;
      }
      DfBrHtmlDiagHere (&P, "missing-whitespace-between-attributes");
      P.TokState = DfBrHtmlStateBeforeAttrName;
      continue;

    /* ------------------------------------------------------------ */
    case DfBrHtmlStateSelfClosingStartTag:
      if (Eof) {
        DfBrHtmlDiagHere (&P, "eof-in-tag");
        goto done;
      }
      if (C == '>') {
        P.SelfClose = DFBR_TRUE;
        Advance (&P);
        EmitTag (&P, DFBR_TRUE);
        continue;
      }
      DfBrHtmlDiagHere (&P, "unexpected-solidus-in-tag");
      P.TokState = DfBrHtmlStateBeforeAttrName;
      continue;

    /* ------------------------------------------------------------ */
    case DfBrHtmlStateMarkupDeclOpen:
      if ((P.Pos + 2 <= P.Len) &&
          (P.Src[P.Pos] == '-') &&
          (P.Src[P.Pos + 1] == '-')) {
        Advance (&P);
        Advance (&P);
        P.TextStart = &P.Src[P.Pos];
        P.TextLen   = 0;
        P.TokState  = DfBrHtmlStateComment;
        continue;
      }
      if (MatchLiteralCaseI (&P, "DOCTYPE", 7)) {
        P.Pos    += 7;
        P.Column += 7;
        P.TextStart = &P.Src[P.Pos];
        P.TextLen   = 0;
        P.TokState  = DfBrHtmlStateDoctype;
        continue;
      }
      /* CDATA / unknown: bogus comment. */
      DfBrHtmlDiagHere (&P, "incorrectly-opened-comment");
      P.TextStart = &P.Src[P.Pos];
      P.TextLen   = 0;
      P.TokState  = DfBrHtmlStateBogusComment;
      continue;

    /* ------------------------------------------------------------ */
    case DfBrHtmlStateComment:
      if (Eof) {
        DfBrHtmlDiagHere (&P, "eof-in-comment");
        DfBrHtmlTreeProcessComment (&P, P.TextStart, P.TextLen);
        goto done;
      }
      if (C == '-') {
        Advance (&P);
        P.TokState = DfBrHtmlStateCommentEndDash;
        continue;
      }
      P.TextLen++;
      Advance (&P);
      continue;

    /* ------------------------------------------------------------ */
    case DfBrHtmlStateCommentEndDash:
      if (Eof) {
        DfBrHtmlDiagHere (&P, "eof-in-comment");
        goto done;
      }
      if (C == '-') {
        Advance (&P);
        P.TokState = DfBrHtmlStateCommentEnd;
        continue;
      }
      P.TextLen++;   /* fold the '-' back into the body */
      P.TokState = DfBrHtmlStateComment;
      continue;

    /* ------------------------------------------------------------ */
    case DfBrHtmlStateCommentEnd:
      if (Eof) {
        DfBrHtmlDiagHere (&P, "eof-in-comment");
        goto done;
      }
      if (C == '>') {
        DfBrHtmlTreeProcessComment (&P, P.TextStart, P.TextLen);
        P.TextStart = DFBR_NULL;
        P.TextLen   = 0;
        Advance (&P);
        P.TokState = DfBrHtmlStateData;
        continue;
      }
      if (C == '-') {
        /* `--->` tolerance; stay in CommentEnd */
        P.TextLen++;
        Advance (&P);
        continue;
      }
      P.TextLen += 2;   /* fold both dashes back into the body */
      P.TokState = DfBrHtmlStateComment;
      continue;

    /* ------------------------------------------------------------ */
    case DfBrHtmlStateDoctype: {
      /* Simplified v2: everything up to the closing '>' (or EOF)
       * becomes the doctype payload. Spec has per-field parsing
       * (name, public id, system id, quirks flag) - we'll add those
       * when the renderer actually cares about quirks. */
      const char *Start = &P.Src[P.Pos];
      usize       Len   = 0;
      while ((P.Pos < P.Len) && (P.Src[P.Pos] != '>')) {
        Len++;
        Advance (&P);
      }
      /* Strip leading whitespace for cleanliness. */
      while ((Len > 0) && IsWhitespace (Start[0])) {
        Start++; Len--;
      }
      DfBrHtmlTreeProcessDoctype (&P, Start, Len);
      if (P.Pos < P.Len) {
        Advance (&P);
      }
      P.TokState = DfBrHtmlStateData;
      continue;
    }

    /* ------------------------------------------------------------ */
    case DfBrHtmlStateBogusComment: {
      const char *Start = &P.Src[P.Pos];
      usize       Len   = 0;
      while ((P.Pos < P.Len) && (P.Src[P.Pos] != '>')) {
        Len++;
        Advance (&P);
      }
      DfBrHtmlTreeProcessComment (&P, Start, Len);
      if (P.Pos < P.Len) {
        Advance (&P);
      }
      P.TokState = DfBrHtmlStateData;
      continue;
    }

    /* ------------------------------------------------------------ */
    case DfBrHtmlStateRawText:
      if (Eof) {
        FlushText (&P, DFBR_FALSE);
        goto done;
      }
      if (C == '<') {
        P.TokState = DfBrHtmlStateRawTextLessThan;
        Advance (&P);
        continue;
      }
      ExtendText (&P);
      Advance (&P);
      continue;

    case DfBrHtmlStateRawTextLessThan:
      if (C == '/') {
        P.EndTagCandStart = &P.Src[P.Pos + 1];   /* speculative */
        P.EndTagCandLen   = 0;
        Advance (&P);
        P.TokState = DfBrHtmlStateRawTextEndTagOpen;
        continue;
      }
      if (P.TextLen == 0) {
        P.TextStart = &P.Src[P.Pos - 1];
      }
      P.TextLen++;
      P.TokState = DfBrHtmlStateRawText;
      continue;

    case DfBrHtmlStateRawTextEndTagOpen:
      if (Eof || !IsAlpha ((char)C)) {
        if (P.TextLen == 0) {
          P.TextStart = &P.Src[P.Pos - 2];
        }
        P.TextLen += 2;
        P.TokState = DfBrHtmlStateRawText;
        continue;
      }
      P.TokState = DfBrHtmlStateRawTextEndTagName;
      continue;

    case DfBrHtmlStateRawTextEndTagName:
      if (Eof) {
        FlushText (&P, DFBR_FALSE);
        goto done;
      }
      if (IsAlpha ((char)C)) {
        P.EndTagCandLen++;
        Advance (&P);
        continue;
      }
      if (EndTagNameMatches (&P) &&
          (IsWhitespace ((char)C) || (C == '/') || (C == '>'))) {
        FlushText (&P, DFBR_FALSE);
        StartTagBegin (&P, DFBR_TRUE, P.Line, P.Column);
        PendingSetName (&P, P.EndTagCandStart, P.EndTagCandLen);
        if (C == '>') {
          Advance (&P);
          EmitTag (&P, DFBR_FALSE);
        } else if (C == '/') {
          Advance (&P);
          P.TokState = DfBrHtmlStateSelfClosingStartTag;
        } else {
          Advance (&P);
          P.TokState = DfBrHtmlStateBeforeAttrName;
        }
        continue;
      }
      if (P.TextLen == 0) {
        P.TextStart = P.EndTagCandStart - 2;
      }
      P.TextLen += 2 + P.EndTagCandLen;
      P.TokState = DfBrHtmlStateRawText;
      continue;

    /* ------------------------------------------------------------ */
    case DfBrHtmlStateRcData:
      if (Eof) {
        FlushText (&P, DFBR_TRUE);
        goto done;
      }
      if (C == '<') {
        P.TokState = DfBrHtmlStateRcDataLessThan;
        Advance (&P);
        continue;
      }
      ExtendText (&P);
      Advance (&P);
      continue;

    case DfBrHtmlStateRcDataLessThan:
      if (C == '/') {
        P.EndTagCandStart = &P.Src[P.Pos + 1];
        P.EndTagCandLen   = 0;
        Advance (&P);
        P.TokState = DfBrHtmlStateRcDataEndTagOpen;
        continue;
      }
      if (P.TextLen == 0) {
        P.TextStart = &P.Src[P.Pos - 1];
      }
      P.TextLen++;
      P.TokState = DfBrHtmlStateRcData;
      continue;

    case DfBrHtmlStateRcDataEndTagOpen:
      if (Eof || !IsAlpha ((char)C)) {
        if (P.TextLen == 0) {
          P.TextStart = &P.Src[P.Pos - 2];
        }
        P.TextLen += 2;
        P.TokState = DfBrHtmlStateRcData;
        continue;
      }
      P.TokState = DfBrHtmlStateRcDataEndTagName;
      continue;

    case DfBrHtmlStateRcDataEndTagName:
      if (Eof) {
        FlushText (&P, DFBR_TRUE);
        goto done;
      }
      if (IsAlpha ((char)C)) {
        P.EndTagCandLen++;
        Advance (&P);
        continue;
      }
      if (EndTagNameMatches (&P) &&
          (IsWhitespace ((char)C) || (C == '/') || (C == '>'))) {
        FlushText (&P, DFBR_TRUE);
        StartTagBegin (&P, DFBR_TRUE, P.Line, P.Column);
        PendingSetName (&P, P.EndTagCandStart, P.EndTagCandLen);
        if (C == '>') {
          Advance (&P);
          EmitTag (&P, DFBR_TRUE);
        } else if (C == '/') {
          Advance (&P);
          P.TokState = DfBrHtmlStateSelfClosingStartTag;
        } else {
          Advance (&P);
          P.TokState = DfBrHtmlStateBeforeAttrName;
        }
        continue;
      }
      if (P.TextLen == 0) {
        P.TextStart = P.EndTagCandStart - 2;
      }
      P.TextLen += 2 + P.EndTagCandLen;
      P.TokState = DfBrHtmlStateRcData;
      continue;

    /* ------------------------------------------------------------ */
    case DfBrHtmlStatePlainText:
      /* TODO: proper PLAINTEXT state. Currently unreachable because
       * the tree constructor doesn't flip into it yet. */
      if (Eof) {
        FlushText (&P, DFBR_FALSE);
        goto done;
      }
      ExtendText (&P);
      Advance (&P);
      continue;

    /* ------------------------------------------------------------ */
    default:
      DfBrHtmlDiagHere (&P, "internal-tokenizer-state");
      goto done;
    }
  }

done:
  FlushText (&P, DFBR_TRUE);
  DfBrHtmlTreeProcessEof (&P);
}

/* ==================================================================
 *  Public entry point
 * ==================================================================*/

DfBrHtmlDocument *
DfBrHtmlParse (
  const char                  *Source,
  usize                        SourceLen,
  const DfBrHtmlParseOptions  *Opts
  )
{
  DfBrHtmlDocument  *Doc;

  if (Source == DFBR_NULL) {
    Source    = "";
    SourceLen = 0;
  }
  if (SourceLen == 0) {
    SourceLen = strlen (Source);
  }

  Doc = (DfBrHtmlDocument *)calloc (1, sizeof (DfBrHtmlDocument));
  if (Doc == DFBR_NULL) {
    return DFBR_NULL;
  }

  if (Opts != DFBR_NULL) {
    Doc->Options = *Opts;
  } else {
    Doc->Options.KeepComments        = DFBR_TRUE;
    Doc->Options.KeepDoctype         = DFBR_TRUE;
    Doc->Options.EmitSourceLocations = DFBR_TRUE;
  }

  /* Copy the source into the arena so node fields can safely point
   * into it without tying the caller's buffer lifetime to the doc. */
  Doc->Source = DfBrHtmlArenaStrDup (&Doc->Arena, Source, SourceLen);
  if (Doc->Source == DFBR_NULL) {
    DfBrHtmlDocumentDestroy (Doc);
    return DFBR_NULL;
  }
  Doc->SourceLen = SourceLen;

  /* Root DOCUMENT node. */
  Doc->Root = (DfBrHtmlNode *)DfBrHtmlArenaZero (
                &Doc->Arena, sizeof (DfBrHtmlNode));
  if (Doc->Root == DFBR_NULL) {
    DfBrHtmlDocumentDestroy (Doc);
    return DFBR_NULL;
  }
  Doc->Root->Kind = DFBR_HTML_NODE_DOCUMENT;
  Doc->Root->Name = DfBrHtmlArenaStrDup (&Doc->Arena, "#document", 9);
  Doc->Root->NameLen = 9;

  DfBrHtmlRunParser (Doc);

  return Doc;
}
